/**
 * @file        graphics/d3d11/command_processor.cpp
 * @brief       Direct3D 11 (feature level 11_0) command processor.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/command_processor.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d11/fh1_native_executor.h>
#include <rex/graphics/d3d11/shader.h>
#include <rex/graphics/fh1_frame_replay.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

// Defined by the Direct3D 12 command processor (both backends are built on
// Windows).
REXCVAR_DECLARE(double, fh1_hud_squeeze);

REXCVAR_DEFINE_INT32(d3d11_viewport_fit_targets, -1, "GPU/D3D11",
                     "Bound the host viewport by the bound render targets' size (-1: on "
                     "Intel GPUs only, whose driver shrinks what a larger viewport draws; 0: "
                     "off; 1: on)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d11_5_0/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d11_5_0/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d11_5_0/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fxaa_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d11_5_0/d3d11_swap_swizzle_cs.h"
}  // namespace shaders

namespace {

constexpr DXGI_FORMAT kFxaaSourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
constexpr uint32_t kGammaRampTableBytes = 256 * 4;
constexpr uint32_t kGammaRampPwlBytes = 128 * 3 * 4;
// Float constants per stage at most (the full guest register range).
constexpr uint32_t kFloatConstantBufferBytes = 256 * 4 * sizeof(float);
constexpr uint32_t kBoolLoopConstantsBytes = (8 + 32) * sizeof(uint32_t);
constexpr uint32_t kFetchConstantsBytes = 32 * 6 * sizeof(uint32_t);

struct ApplyGammaConstants {
  uint32_t size[2];
  uint32_t padding[2];
};
struct FxaaConstants {
  uint32_t size[2];
  float size_inv[2];
};

// Copies the float constants a shader uses, in register order, to `out`: each
// contiguous run of used registers is one copy.
uint8_t* GatherFloatConstants(uint8_t* out, const uint64_t (&bitmap)[4], const uint32_t* first) {
  for (uint32_t word = 0; word < 4; ++word) {
    uint64_t remaining = bitmap[word];
    while (remaining) {
      const uint32_t start = uint32_t(std::countr_zero(remaining));
      const uint32_t length = uint32_t(std::countr_one(remaining >> start));
      const size_t bytes = size_t(length) * 4 * sizeof(float);
      std::memcpy(out, first + (word << 8) + (start << 2), bytes);
      out += bytes;
      remaining &= length == 64 ? 0 : ~(((uint64_t(1) << length) - 1) << start);
    }
  }
  return out;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> CreateDynamicConstantBuffer(ID3D11Device* device,
                                                                 uint32_t size) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = (size + 15) & ~uint32_t(15);
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) {
    return nullptr;
  }
  return buffer;
}

}  // namespace

D3D11CommandProcessor::D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}
D3D11CommandProcessor::~D3D11CommandProcessor() = default;

void D3D11CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void D3D11CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D11CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D11CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t i = first_vfetch; i <= last_vfetch; ++i) {
    vertex_buffers_in_sync_[i >> 6] &= ~(uint64_t(1) << (i & 63));
  }
}

void D3D11CommandProcessor::InvalidateAllConstantBuffers() {
  cbuffer_system_.up_to_date = false;
  cbuffer_float_vertex_.up_to_date = false;
  cbuffer_float_pixel_.up_to_date = false;
  cbuffer_bool_loop_.up_to_date = false;
  cbuffer_fetch_.up_to_date = false;
}

void D3D11CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void D3D11CommandProcessor::UnbindComputeResources(uint32_t srv_count, uint32_t uav_count,
                                                   uint32_t cbuffer_count) {
  ID3D11DeviceContext* context = GetContext();
  static ID3D11ShaderResourceView* const kNullSrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] =
      {};
  static ID3D11UnorderedAccessView* const kNullUavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
  static ID3D11Buffer* const kNullBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
  if (srv_count) {
    context->CSSetShaderResources(
        0, std::min(srv_count, uint32_t(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)), kNullSrvs);
  }
  if (uav_count) {
    context->CSSetUnorderedAccessViews(
        0, std::min(uav_count, uint32_t(D3D11_PS_CS_UAV_REGISTER_COUNT)), kNullUavs, nullptr);
  }
  if (cbuffer_count) {
    context->CSSetConstantBuffers(
        0, std::min(cbuffer_count, uint32_t(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)),
        kNullBuffers);
  }
}

bool D3D11CommandProcessor::CheckDeviceLost() {
  if (device_lost_) {
    return true;
  }
  HRESULT reason = GetDevice()->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) {
    return false;
  }
  device_lost_ = true;
  REXGPU_ERROR("Direct3D 11 device removed (0x{:08X})", uint32_t(reason));
  rex::FlushLogging();
  if (graphics_system_) {
    graphics_system_->OnHostGpuLossFromAnyThread(reason != DXGI_ERROR_DEVICE_REMOVED);
  }
  return true;
}

bool D3D11CommandProcessor::AwaitGpuIdle() {
  if (CheckDeviceLost() || !idle_query_) {
    return false;
  }
  ID3D11DeviceContext* context = GetContext();
  context->End(idle_query_.Get());
  // The first GetData without DONOTFLUSH submits the pending commands.
  while (true) {
    HRESULT result = context->GetData(idle_query_.Get(), nullptr, 0, 0);
    if (result == S_OK) {
      break;
    }
    if (FAILED(result) || CheckDeviceLost() || IsShutdownRequested()) {
      return false;
    }
    std::this_thread::yield();
  }
  UpdateCompletedFrame();
  return true;
}

void D3D11CommandProcessor::UpdateCompletedFrame() {
  ID3D11DeviceContext* context = GetContext();
  for (uint32_t i = 0; i < kQueueFrames; ++i) {
    uint64_t frame = frame_end_query_frames_[i];
    if (!frame || frame <= frame_completed_ || !frame_end_queries_[i]) {
      continue;
    }
    if (context->GetData(frame_end_queries_[i].Get(), nullptr, 0,
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
      frame_completed_ = std::max(frame_completed_, frame);
    }
  }
}

void D3D11CommandProcessor::BeginFrameIfNeeded() {
  if (frame_open_) {
    return;
  }
  UpdateCompletedFrame();
  // Keep at most kQueueFrames frames in flight, so per-frame host resources
  // (the converted index ring) are not overwritten while the GPU reads them.
  if (frame_current_ > kQueueFrames && frame_completed_ + kQueueFrames < frame_current_) {
    uint64_t await_frame = frame_current_ - kQueueFrames;
    ID3D11Query* query = frame_end_queries_[await_frame % kQueueFrames].Get();
    if (query && frame_end_query_frames_[await_frame % kQueueFrames] == await_frame) {
      ID3D11DeviceContext* context = GetContext();
      while (context->GetData(query, nullptr, 0, 0) == S_FALSE) {
        if (CheckDeviceLost() || IsShutdownRequested()) {
          break;
        }
        std::this_thread::yield();
      }
      frame_completed_ = std::max(frame_completed_, await_frame);
    }
  }

  frame_open_ = true;

  if (cache_clear_requested_ && AwaitGpuIdle()) {
    cache_clear_requested_ = false;
    primitive_processor_->ClearCache();
    texture_cache_->ClearCache();
    pipeline_cache_->Shutdown();
    pipeline_cache_->Initialize();
    InvalidateGuestDrawState();
  }

  texture_cache_->BeginFrame();
  primitive_processor_->BeginFrame();
}

void D3D11CommandProcessor::EndFrame() {
  if (!frame_open_) {
    return;
  }
  ID3D11DeviceContext* context = GetContext();
  uint32_t query_index = uint32_t(frame_current_ % kQueueFrames);
  if (frame_end_queries_[query_index]) {
    context->End(frame_end_queries_[query_index].Get());
    frame_end_query_frames_[query_index] = frame_current_;
  }
  texture_cache_->EndFrame();
  primitive_processor_->EndFrame();
  context->Flush();
  frame_open_ = false;
  ++frame_current_;
  CheckDeviceLost();
}

std::string D3D11CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 11";
  const D3D_FEATURE_LEVEL feature_level = GetD3D11Provider().GetFeatureLevel();
  title << " - FL " << ((uint32_t(feature_level) >> 12) & 0xF) << '_'
        << ((uint32_t(feature_level) >> 8) & 0xF);
  return title.str();
}

ReadbackResolveMode D3D11CommandProcessor::Fh1ReadbackResolveMode() const {
  return GetReadbackResolveMode(false);
}

bool D3D11CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();

  const ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  ID3D11Device* device = provider.GetDevice();

  // The FH1 native executor on Direct3D 11 renders unscaled only.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (draw_resolution_scale_x != 1 || draw_resolution_scale_y != 1) {
    REXGPU_WARN(
        "Direct3D 11 renders at 1x resolution scale only, ignoring the requested {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  // Draws without clipping use a host viewport much larger than the render
  // target (8192 pixels, see draw_util::GetHostViewportInfo). The UHD 620
  // driver rasterizes them as if that viewport were squeezed into the
  // target: scaled by the target-to-viewport size ratio towards the top left
  // (a full-screen depth clear into a 640x1024 surface covered 640/8192 of
  // its width and 1024/8192 of its height). The vertex shader rescales to
  // whatever viewport is used, so bounding it by the targets draws the same
  // pixels (on the RTX 4080 two frame replays stay bit-exact with it and a
  // third changes in 3 words), but it stays limited to Intel by default.
  const int32_t viewport_fit_targets = REXCVAR_GET(d3d11_viewport_fit_targets);
  viewport_fit_targets_ =
      viewport_fit_targets < 0
          ? provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kIntel
          : viewport_fit_targets != 0;
  if (viewport_fit_targets_) {
    REXGPU_INFO("Direct3D 11: host viewports bounded by the render target size");
  }
  gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);
  depth_float24_round_ = REXCVAR_GET(depth_float24_round);
  depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);
  msaa_2x_supported_ = false;
  if (REXCVAR_GET(native_2x_msaa)) {
    static const DXGI_FORMAT kRenderTargetFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_SNORM,
        DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_SNORM,
        DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R32_UINT,
    };
    msaa_2x_supported_ = true;
    for (DXGI_FORMAT format : kRenderTargetFormats) {
      UINT quality_levels = 0;
      if (FAILED(device->CheckMultisampleQualityLevels(format, 2, &quality_levels)) ||
          !quality_levels) {
        msaa_2x_supported_ = false;
        break;
      }
    }
  }

  for (uint32_t i = 0; i < kQueueFrames; ++i) {
    D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateQuery(&query_desc, &frame_end_queries_[i]))) {
      REXGPU_ERROR("Failed to create a frame fence query");
      return false;
    }
    frame_end_query_frames_[i] = 0;
  }
  {
    D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateQuery(&query_desc, &idle_query_))) {
      REXGPU_ERROR("Failed to create the GPU idle query");
      return false;
    }
  }

  shared_memory_ = std::make_unique<D3D11SharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  primitive_processor_ = std::make_unique<D3D11PrimitiveProcessor>(*register_file_, *memory_,
                                                                   *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ = D3D11TextureCache::Create(*register_file_, *shared_memory_, *this);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  fh1_native_executor_ = std::make_unique<Fh1NativeExecutor>(*this, *register_file_, *memory_);
  Fh1NativeExecutorConfig native_config;
  native_config.msaa_2x_supported = msaa_2x_supported_;
  native_config.gamma_as_unorm16 = gamma_render_target_as_unorm16_;
  native_config.depth_float24_round = depth_float24_round_;
  native_config.fixed16_truncated = !REXCVAR_GET(snorm16_render_target_full_range);
  native_config.memory = shared_memory_.get();
  native_config.textures = texture_cache_.get();
  if (!fh1_native_executor_->Initialize(native_config)) {
    REXGPU_ERROR("Failed to initialize the FH1 native executor");
    fh1_native_executor_.reset();
    return false;
  }

  pipeline_cache_ =
      std::make_unique<D3D11PipelineCache>(*this, *register_file_, msaa_2x_supported_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the pipeline cache");
    return false;
  }

  // Constant buffers.
  cbuffer_system_.buffer =
      CreateDynamicConstantBuffer(device, uint32_t(sizeof(DxbcShaderTranslator::SystemConstants)));
  cbuffer_float_vertex_.buffer = CreateDynamicConstantBuffer(device, kFloatConstantBufferBytes);
  cbuffer_float_pixel_.buffer = CreateDynamicConstantBuffer(device, kFloatConstantBufferBytes);
  cbuffer_bool_loop_.buffer = CreateDynamicConstantBuffer(device, kBoolLoopConstantsBytes);
  cbuffer_fetch_.buffer = CreateDynamicConstantBuffer(device, kFetchConstantsBytes);
  if (!cbuffer_system_.buffer || !cbuffer_float_vertex_.buffer || !cbuffer_float_pixel_.buffer ||
      !cbuffer_bool_loop_.buffer || !cbuffer_fetch_.buffer) {
    REXGPU_ERROR("Failed to create the guest shader constant buffers");
    return false;
  }
  InvalidateAllConstantBuffers();

  if (!InitializeGammaAndFxaa()) {
    return false;
  }

  occlusion_queries_available_ = InitializeOcclusionQueries();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));
  system_constants_dirty_ = true;

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  InvalidateGuestDrawState();
  return true;
}

bool D3D11CommandProcessor::InitializeGammaAndFxaa() {
  ID3D11Device* device = GetDevice();

  {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kGammaRampTableBytes + kGammaRampPwlBytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &gamma_ramp_buffer_))) {
      REXGPU_ERROR("Failed to create the gamma ramp buffer");
      return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = 256;
    if (FAILED(device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &srv_desc,
                                                &gamma_ramp_table_srv_))) {
      REXGPU_ERROR("Failed to create the gamma ramp table view");
      return false;
    }
    srv_desc.Format = DXGI_FORMAT_R16G16_UINT;
    srv_desc.Buffer.FirstElement = kGammaRampTableBytes / 4;
    srv_desc.Buffer.NumElements = 128 * 3;
    if (FAILED(device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &srv_desc,
                                                &gamma_ramp_pwl_srv_))) {
      REXGPU_ERROR("Failed to create the gamma ramp PWL view");
      return false;
    }
  }
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;

  auto create_cs = [device](const void* bytecode, size_t size,
                            Microsoft::WRL::ComPtr<ID3D11ComputeShader>& out,
                            const char* name) -> bool {
    if (FAILED(device->CreateComputeShader(bytecode, size, nullptr, &out))) {
      REXGPU_ERROR("Failed to create the {} compute shader", name);
      return false;
    }
    return true;
  };
  if (!create_cs(shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
                 apply_gamma_table_cs_, "gamma table") ||
      !create_cs(shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
                 apply_gamma_pwl_cs_, "gamma PWL") ||
      !create_cs(shaders::apply_gamma_table_fxaa_luma_cs,
                 sizeof(shaders::apply_gamma_table_fxaa_luma_cs), apply_gamma_table_fxaa_luma_cs_,
                 "gamma table with FXAA luma") ||
      !create_cs(shaders::apply_gamma_pwl_fxaa_luma_cs,
                 sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs), apply_gamma_pwl_fxaa_luma_cs_,
                 "gamma PWL with FXAA luma") ||
      !create_cs(shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_cs_, "FXAA") ||
      !create_cs(shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs), fxaa_extreme_cs_,
                 "extreme-quality FXAA") ||
      !create_cs(shaders::d3d11_swap_swizzle_cs, sizeof(shaders::d3d11_swap_swizzle_cs),
                 swap_swizzle_cs_, "front buffer swizzle")) {
    return false;
  }

  swap_constants_ = CreateDynamicConstantBuffer(device, 16);
  if (!swap_constants_) {
    REXGPU_ERROR("Failed to create the swap constant buffer");
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxAnisotropy = 1;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MinLOD = 0.0f;
  sampler_desc.MaxLOD = 0.0f;
  if (FAILED(device->CreateSamplerState(&sampler_desc, &fxaa_sampler_))) {
    REXGPU_ERROR("Failed to create the FXAA sampler");
    return false;
  }
  return true;
}

void D3D11CommandProcessor::ShutdownContext() {
  if (GetD3D11Provider().GetImmediateContext()) {
    AwaitGpuIdle();
  }
  InvalidateAllVertexBufferResidency();

  active_occlusion_query_ = {};
  occlusion_query_.Reset();
  occlusion_queries_available_ = false;

  fxaa_source_uav_.Reset();
  fxaa_source_srv_.Reset();
  fxaa_source_texture_.Reset();
  fxaa_source_width_ = 0;
  fxaa_source_height_ = 0;
  swap_swizzle_uav_.Reset();
  swap_swizzle_srv_.Reset();
  swap_swizzle_texture_.Reset();
  swap_swizzle_width_ = 0;
  swap_swizzle_height_ = 0;
  swap_swizzle_format_ = DXGI_FORMAT_UNKNOWN;
  swap_swizzle_cs_.Reset();
  fxaa_sampler_.Reset();
  swap_constants_.Reset();
  fxaa_extreme_cs_.Reset();
  fxaa_cs_.Reset();
  apply_gamma_pwl_fxaa_luma_cs_.Reset();
  apply_gamma_table_fxaa_luma_cs_.Reset();
  apply_gamma_pwl_cs_.Reset();
  apply_gamma_table_cs_.Reset();
  gamma_ramp_pwl_srv_.Reset();
  gamma_ramp_table_srv_.Reset();
  gamma_ramp_buffer_.Reset();

  cbuffer_fetch_ = {};
  cbuffer_bool_loop_ = {};
  cbuffer_float_pixel_ = {};
  cbuffer_float_vertex_ = {};
  cbuffer_system_ = {};

  pipeline_cache_.reset();
  fh1_native_executor_.reset();
  texture_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();

  idle_query_.Reset();
  for (uint32_t i = 0; i < kQueueFrames; ++i) {
    frame_end_queries_[i].Reset();
    frame_end_query_frames_[i] = 0;
  }
  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  current_pipeline_ = nullptr;
  guest_draw_state_valid_ = false;
  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D11CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (float_constant_index >= 256) {
      float_constant_index -= 256;
      if (current_float_constant_map_pixel_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_float_pixel_.up_to_date = false;
      }
    } else {
      if (current_float_constant_map_vertex_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_float_vertex_.up_to_date = false;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    uint32_t vfetch = (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2;
    InvalidateVertexBufferResidencyRange(vfetch, vfetch);
  }
}

void D3D11CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    for (uint32_t constant = first_constant; constant <= last_constant;) {
      uint32_t word = constant >> 6;
      uint32_t bit = constant & 63;
      uint32_t last_in_word = std::min(last_constant, (word << 6) + 63);
      uint32_t bit_count = last_in_word - constant + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << bit;
      if (usage_map[word] & mask) {
        return true;
      }
      constant = last_in_word + 1;
    }
    return false;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (first_float_constant < 256 &&
        range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                     std::min(last_float_constant, 255u))) {
      cbuffer_float_vertex_.up_to_date = false;
    }
    if (last_float_constant >= 256 &&
        range_has_any_constant_usage(
            current_float_constant_map_pixel_,
            first_float_constant >= 256 ? first_float_constant - 256 : 0,
            last_float_constant - 256)) {
      cbuffer_float_pixel_.up_to_date = false;
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D11CommandProcessor::RestoreFrameDumpRegisters(std::span<const uint32_t> registers) {
  CommandProcessor::RestoreFrameDumpRegisters(registers);
  InvalidateGuestDrawState();
  InvalidateAllConstantBuffers();
  InvalidateAllVertexBufferResidency();
  system_constants_dirty_ = true;
  if (texture_cache_) {
    texture_cache_->TextureFetchConstantsWritten(0, 31);
  }
}

void D3D11CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D11CommandProcessor::OnGammaRampPWLValueWritten() { gamma_ramp_pwl_up_to_date_ = false; }

//
// Occlusion queries.
//

bool D3D11CommandProcessor::InitializeOcclusionQueries() {
  D3D11_QUERY_DESC desc = {D3D11_QUERY_OCCLUSION, 0};
  if (FAILED(GetDevice()->CreateQuery(&desc, &occlusion_query_))) {
    REXGPU_WARN("Failed to create the occlusion query, using fake sample counts");
    return false;
  }
  return true;
}

void D3D11CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_) {
    GetContext()->End(occlusion_query_.Get());
  }
  active_occlusion_query_ = {};
  occlusion_queries_available_ = false;
}

bool D3D11CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!occlusion_queries_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN("D3D11CommandProcessor: occlusion query begin while another one is active");
    DisableHostOcclusionQueries();
    return false;
  }
  BeginFrameIfNeeded();
  GetContext()->Begin(occlusion_query_.Get());
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.valid = true;
  return true;
}

bool D3D11CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  (void)sample_count_address;
  if (!occlusion_queries_available_ || !active_occlusion_query_.valid) {
    return false;
  }
  active_occlusion_query_ = {};
  ID3D11DeviceContext* context = GetContext();
  context->End(occlusion_query_.Get());
  UINT64 samples = 0;
  while (true) {
    HRESULT result = context->GetData(occlusion_query_.Get(), &samples, sizeof(samples), 0);
    if (result == S_OK) {
      break;
    }
    if (FAILED(result) || CheckDeviceLost() || IsShutdownRequested()) {
      return false;
    }
    std::this_thread::yield();
  }
  WriteGuestOcclusionResult(sample_counts, samples);
  return true;
}

void D3D11CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

bool D3D11CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_queries_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end = sample_counts->ZPass_A == kQueryFinished ||
                  sample_counts->ZPass_B == kQueryFinished ||
                  sample_counts->ZFail_A == kQueryFinished ||
                  sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end = sample_counts->ZPass_A == kQueryFinished ||
                sample_counts->ZPass_B == kQueryFinished ||
                sample_counts->ZFail_A == kQueryFinished ||
                sample_counts->ZFail_B == kQueryFinished;
  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }
  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }
  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    return write_fallback_result();
  }
  return true;
}

//
// Swap.
//

void D3D11CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  PERF_counter_add(kDrawCalls, pending_draw_calls_);
  pending_draw_calls_ = 0;
  {
    const xenos::TextureFormat front_format = register_file_->GetTextureFetch(0).format;
    const uint32_t* gamma_pwl =
        front_format == xenos::TextureFormat::k_2_10_10_10 ||
                front_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16
            ? reinterpret_cast<const uint32_t*>(gamma_ramp_pwl_rgb())
            : nullptr;
    fh1_native_executor_->OnSwap(observation_frame_sequence_, frontbuffer_ptr, frontbuffer_width,
                                 frontbuffer_height, gamma_pwl);
  }

  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }

  BeginFrameIfNeeded();

  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  uint32_t swap_swizzle = uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
  ID3D11ShaderResourceView* swap_srv = texture_cache_->RequestSwapTexture(
      frontbuffer_format, &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, nullptr,
      &swap_swizzle);
  if (!swap_srv) {
    auto fetch = register_file_->GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    EndFrame();
    return;
  }
  // The swap texture may be allocation-padded: present the active front
  // buffer region from the swap packet.
  uint32_t guest_output_width = frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled;
  uint32_t guest_output_height =
      frontbuffer_height ? frontbuffer_height : frontbuffer_height_unscaled;
  if (frontbuffer_width_unscaled) {
    guest_output_width = std::min(guest_output_width, frontbuffer_width_unscaled);
  }
  if (frontbuffer_height_unscaled) {
    guest_output_height = std::min(guest_output_height, frontbuffer_height_unscaled);
  }
  if (!guest_output_width || !guest_output_height) {
    EndFrame();
    return;
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, swap_srv, swap_swizzle, frontbuffer_format, guest_output_width, guest_output_height,
       display_width, display_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d11_context =
            static_cast<ui::d3d11::D3D11Presenter::D3D11GuestOutputRefreshContext&>(context);
        ID3D11Device* device = GetDevice();
        ID3D11DeviceContext* immediate = GetContext();

        ID3D11UnorderedAccessView* guest_output_uav = d3d11_context.uav_or_null();
        if (!guest_output_uav) {
          // Typed UAV stores of R10G10B10A2_UNORM are required at feature
          // level 11_0, so this is not expected on a conforming driver.
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXGPU_ERROR(
                "Direct3D 11: the guest output can't be written through a UAV on this device");
          }
          return false;
        }

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa && (fxaa_source_width_ != guest_output_width ||
                         fxaa_source_height_ != guest_output_height || !fxaa_source_texture_)) {
          fxaa_source_uav_.Reset();
          fxaa_source_srv_.Reset();
          fxaa_source_texture_.Reset();
          fxaa_source_width_ = 0;
          fxaa_source_height_ = 0;
          D3D11_TEXTURE2D_DESC desc = {};
          desc.Width = guest_output_width;
          desc.Height = guest_output_height;
          desc.MipLevels = 1;
          desc.ArraySize = 1;
          desc.Format = kFxaaSourceTextureFormat;
          desc.SampleDesc.Count = 1;
          desc.Usage = D3D11_USAGE_DEFAULT;
          desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
          if (FAILED(device->CreateTexture2D(&desc, nullptr, &fxaa_source_texture_)) ||
              FAILED(device->CreateShaderResourceView(fxaa_source_texture_.Get(), nullptr,
                                                      &fxaa_source_srv_)) ||
              FAILED(device->CreateUnorderedAccessView(fxaa_source_texture_.Get(), nullptr,
                                                       &fxaa_source_uav_))) {
            REXGPU_ERROR("Failed to create the FXAA input texture");
            fxaa_source_uav_.Reset();
            fxaa_source_srv_.Reset();
            fxaa_source_texture_.Reset();
            use_fxaa = false;
          } else {
            fxaa_source_width_ = guest_output_width;
            fxaa_source_height_ = guest_output_height;
          }
        }

        // The 256-entry table for 8_8_8_8 output and the PWL ramp for
        // 2_10_10_10, as on Direct3D 12.
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          D3D11_BOX box = {};
          box.left = use_pwl_gamma_ramp ? kGammaRampTableBytes : 0;
          box.right = box.left + (use_pwl_gamma_ramp ? kGammaRampPwlBytes : kGammaRampTableBytes);
          box.bottom = 1;
          box.back = 1;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            reg::DC_LUT_PWL_DATA swapped[128 * 3];
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              swapped[i].base = gamma_ramp_pwl[i].delta;
              swapped[i].delta = gamma_ramp_pwl[i].base;
            }
            immediate->UpdateSubresource(gamma_ramp_buffer_.Get(), 0, &box, swapped, 0, 0);
          } else {
            immediate->UpdateSubresource(
                gamma_ramp_buffer_.Get(), 0, &box,
                use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                   : static_cast<const void*>(gamma_ramp_256_entry_table()),
                0, 0);
          }
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        auto write_swap_constants = [&](const void* data, size_t size) -> bool {
          D3D11_MAPPED_SUBRESOURCE mapped;
          if (FAILED(immediate->Map(swap_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                    &mapped))) {
            return false;
          }
          std::memcpy(mapped.pData, data, size);
          immediate->Unmap(swap_constants_.Get(), 0);
          return true;
        };

        // The gamma shaders read R, G and B of a plain view: apply the host
        // swizzle first when it moves them (FH1 fetches its front buffer with
        // red and blue swapped). Alpha is not read.
        constexpr uint32_t kSwizzleRGBMask = (UINT32_C(1) << 9) - 1;
        ID3D11ShaderResourceView* gamma_source_srv = swap_srv;
        if ((swap_swizzle & kSwizzleRGBMask) !=
            (uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) & kSwizzleRGBMask)) {
          D3D11_SHADER_RESOURCE_VIEW_DESC source_desc;
          swap_srv->GetDesc(&source_desc);
          if (swap_swizzle_width_ != guest_output_width ||
              swap_swizzle_height_ != guest_output_height ||
              swap_swizzle_format_ != source_desc.Format || !swap_swizzle_texture_) {
            swap_swizzle_uav_.Reset();
            swap_swizzle_srv_.Reset();
            swap_swizzle_texture_.Reset();
            swap_swizzle_width_ = 0;
            swap_swizzle_height_ = 0;
            swap_swizzle_format_ = DXGI_FORMAT_UNKNOWN;
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = guest_output_width;
            desc.Height = guest_output_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = source_desc.Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &swap_swizzle_texture_)) ||
                FAILED(device->CreateShaderResourceView(swap_swizzle_texture_.Get(), nullptr,
                                                        &swap_swizzle_srv_)) ||
                FAILED(device->CreateUnorderedAccessView(swap_swizzle_texture_.Get(), nullptr,
                                                         &swap_swizzle_uav_))) {
              REXGPU_ERROR("Failed to create the swizzled front buffer texture ({})",
                           uint32_t(source_desc.Format));
              swap_swizzle_uav_.Reset();
              swap_swizzle_srv_.Reset();
              swap_swizzle_texture_.Reset();
              return false;
            }
            swap_swizzle_width_ = guest_output_width;
            swap_swizzle_height_ = guest_output_height;
            swap_swizzle_format_ = source_desc.Format;
          }
          uint32_t swizzle_constants[4] = {guest_output_width, guest_output_height, swap_swizzle,
                                           0};
          if (!write_swap_constants(swizzle_constants, sizeof(swizzle_constants))) {
            return false;
          }
          // t0 source, u0 destination, cb0 size and swizzle.
          ID3D11Buffer* swizzle_cbuffer = swap_constants_.Get();
          ID3D11UnorderedAccessView* swizzle_uav = swap_swizzle_uav_.Get();
          immediate->CSSetShader(swap_swizzle_cs_.Get(), nullptr, 0);
          immediate->CSSetConstantBuffers(0, 1, &swizzle_cbuffer);
          ID3D11ShaderResourceView* swizzle_source_srv = swap_srv;
          immediate->CSSetShaderResources(0, 1, &swizzle_source_srv);
          immediate->CSSetUnorderedAccessViews(0, 1, &swizzle_uav, nullptr);
          immediate->Dispatch((guest_output_width + 7) / 8, (guest_output_height + 7) / 8, 1);
          UnbindComputeResources(1, 1);
          gamma_source_srv = swap_swizzle_srv_.Get();
        }

        ApplyGammaConstants apply_gamma_constants = {};
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        if (!write_swap_constants(&apply_gamma_constants, sizeof(apply_gamma_constants))) {
          return false;
        }
        ID3D11ComputeShader* apply_gamma_cs;
        if (use_pwl_gamma_ramp) {
          apply_gamma_cs =
              use_fxaa ? apply_gamma_pwl_fxaa_luma_cs_.Get() : apply_gamma_pwl_cs_.Get();
        } else {
          apply_gamma_cs =
              use_fxaa ? apply_gamma_table_fxaa_luma_cs_.Get() : apply_gamma_table_cs_.Get();
        }
        // t0 ramp, t1 source, u0 destination, cb0 size.
        ID3D11ShaderResourceView* apply_gamma_srvs[] = {
            use_pwl_gamma_ramp ? gamma_ramp_pwl_srv_.Get() : gamma_ramp_table_srv_.Get(),
            gamma_source_srv};
        ID3D11UnorderedAccessView* apply_gamma_uav =
            use_fxaa ? fxaa_source_uav_.Get() : guest_output_uav;
        ID3D11Buffer* swap_cbuffer = swap_constants_.Get();
        immediate->CSSetShader(apply_gamma_cs, nullptr, 0);
        immediate->CSSetConstantBuffers(0, 1, &swap_cbuffer);
        immediate->CSSetShaderResources(0, 2, apply_gamma_srvs);
        immediate->CSSetUnorderedAccessViews(0, 1, &apply_gamma_uav, nullptr);
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        immediate->Dispatch(group_count_x, group_count_y, 1);
        UnbindComputeResources(2, 1);

        if (use_fxaa) {
          FxaaConstants fxaa_constants;
          fxaa_constants.size[0] = guest_output_width;
          fxaa_constants.size[1] = guest_output_height;
          fxaa_constants.size_inv[0] = 1.0f / float(guest_output_width);
          fxaa_constants.size_inv[1] = 1.0f / float(guest_output_height);
          if (write_swap_constants(&fxaa_constants, sizeof(fxaa_constants))) {
            // t0 source, s0 linear clamp, u0 destination, cb0 size.
            ID3D11ShaderResourceView* fxaa_srv = fxaa_source_srv_.Get();
            ID3D11SamplerState* fxaa_sampler = fxaa_sampler_.Get();
            immediate->CSSetShader(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                       ? fxaa_extreme_cs_.Get()
                                       : fxaa_cs_.Get(),
                                   nullptr, 0);
            immediate->CSSetConstantBuffers(0, 1, &swap_cbuffer);
            immediate->CSSetShaderResources(0, 1, &fxaa_srv);
            immediate->CSSetSamplers(0, 1, &fxaa_sampler);
            immediate->CSSetUnorderedAccessViews(0, 1, &guest_output_uav, nullptr);
            immediate->Dispatch(group_count_x, group_count_y, 1);
            UnbindComputeResources(1, 1);
          }
        }
        UnbindComputeResources(0, 0, 1);
        immediate->CSSetShader(nullptr, nullptr, 0);

        auto renderer = graphics_system_->native_guest_output_renderer().Get();
        if (renderer) {
          // Notify the host of the presented guest output (render tests
          // capture it here).
          system::NativeGuestOutputRenderContext native_context;
          native_context.backend = system::NativeGuestOutputBackend::kD3D11;
          native_context.phase = system::NativeGuestOutputPhase::kPresented;
          native_context.presenter = system::NativeGuestOutputPresenter::kNativeExecutor;
          native_context.guest_output_width = guest_output_width;
          native_context.guest_output_height = guest_output_height;
          native_context.display_width = display_width;
          native_context.display_height = display_height;
          native_context.output_format = uint32_t(d3d11_context.format());
          native_context.device = device;
          native_context.command_context = immediate;
          native_context.guest_output = d3d11_context.texture();
          native_context.submission = frame_current_;
          native_context.completed_submission = frame_completed_;
          native_context.frame_sequence = observation_frame_sequence_ - 1;
          native_context.use_pwl_gamma_ramp = use_pwl_gamma_ramp;
          renderer(native_context);
        }
        // The presenter executes its own commands on the immediate context:
        // submit the refresh first.
        immediate->Flush();
        return true;
      });

  InvalidateGuestDrawState();
  // End the frame even if nothing was presented, to free per-frame resources.
  EndFrame();
}

void D3D11CommandProcessor::FlushCpuVisibleResults() {
  fh1_native_executor_->FlushResolveReadbacks();
}

void D3D11CommandProcessor::OnPrimaryBufferEnd() {}

Shader* D3D11CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

//
// Draws.
//

void D3D11CommandProcessor::SetViewport(const D3D11_VIEWPORT& viewport) {
  if (!ff_viewport_update_needed_ && std::memcmp(&ff_viewport_, &viewport, sizeof(viewport)) == 0) {
    return;
  }
  ff_viewport_ = viewport;
  GetContext()->RSSetViewports(1, &ff_viewport_);
  ff_viewport_update_needed_ = false;
}

void D3D11CommandProcessor::SetScissorRect(const D3D11_RECT& scissor_rect) {
  if (!ff_scissor_update_needed_ &&
      std::memcmp(&ff_scissor_, &scissor_rect, sizeof(scissor_rect)) == 0) {
    return;
  }
  ff_scissor_ = scissor_rect;
  GetContext()->RSSetScissorRects(1, &ff_scissor_);
  ff_scissor_update_needed_ = false;
}

void D3D11CommandProcessor::UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                                     const draw_util::Scissor& scissor,
                                                     bool primitive_polygonal,
                                                     reg::RB_DEPTHCONTROL normalized_depth_control) {
  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  D3D11_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  const RegisterFile& regs = *register_file_;

  // The blend factor and the stencil reference are set with the blend and
  // depth/stencil states when binding the pipeline.
  float blend_factor[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  // memcmp so NaN doesn't invalidate every draw.
  ff_blend_factor_update_needed_ |=
      std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
  std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);

  // Per-face stencil reference is not supported by Direct3D 11: choose the
  // back face one only if drawing only back faces.
  Register stencil_ref_mask_reg;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal && normalized_depth_control.backface_enable &&
      pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
  } else {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
  }
  uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  ff_stencil_ref_ = stencil_ref;
}

void D3D11CommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal, uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  (void)normalized_depth_control;
  (void)normalized_color_mask;
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  bool dirty = false;

  // Flags. Shared memory is never a UAV here (shader model 5.0 drops memory
  // export).
  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  if (!gamma_render_target_as_unorm16_) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    float point_screen_diameter_to_ndc_radius_x =
        1.0f / float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
    float point_screen_diameter_to_ndc_radius_y =
        1.0f / float(std::max(viewport_info.xy_extent[1], uint32_t(1)));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness and gamma, and the host swizzles (shader model 5.0
  // has no SRV component mapping, so the translated shaders apply them).
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_normalized_fixed_point = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    uint32_t host_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
    dirty |= system_constants_.texture_swizzles[texture_index] != host_swizzle;
    system_constants_.texture_swizzles[texture_index] = host_swizzle;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    textures_normalized_fixed_point |=
        uint32_t(texture_cache_->IsActiveTextureNormalizedFixedPoint(texture_index))
        << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;
  dirty |= system_constants_.textures_normalized_fixed_point != textures_normalized_fixed_point;
  system_constants_.textures_normalized_fixed_point = textures_normalized_fixed_point;

  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  const bool fixed16_truncated = !REXCVAR_GET(snorm16_render_target_full_range);
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    int32_t color_exp_bias = color_info.color_exp_bias;
    if ((color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) &&
        !fixed16_truncated) {
      // Remap from -32...32 to -1...1.
      color_exp_bias -= 5;
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  if (dirty) {
    cbuffer_system_.up_to_date = false;
  }
}

bool D3D11CommandProcessor::UploadConstantBuffer(uint32_t index, const void* data, uint32_t size) {
  ConstantBuffer* cbuffer;
  switch (index) {
    case kCbufferSystem:
      cbuffer = &cbuffer_system_;
      break;
    case kCbufferBoolLoop:
      cbuffer = &cbuffer_bool_loop_;
      break;
    case kCbufferFetch:
      cbuffer = &cbuffer_fetch_;
      break;
    default:
      return false;
  }
  ID3D11DeviceContext* context = GetContext();
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(cbuffer->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return false;
  }
  std::memcpy(mapped.pData, data, size);
  context->Unmap(cbuffer->buffer.Get(), 0);
  cbuffer->up_to_date = true;
  return true;
}

bool D3D11CommandProcessor::UpdateBindings(const D3D11Shader* vertex_shader,
                                           const D3D11Shader* pixel_shader) {
  const RegisterFile& regs = *register_file_;
  ID3D11DeviceContext* context = GetContext();

  // Float constants: the vertex and pixel shaders have their own packed
  // buffers (the registers the shader uses, in order).
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      if (float_constant_map_vertex.float_count) {
        cbuffer_float_vertex_.up_to_date = false;
      }
    }
  }
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_map_pixel.float_count) {
          cbuffer_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  auto upload_float_constants = [&](ConstantBuffer& cbuffer, const uint64_t (&bitmap)[4],
                                    const uint32_t* first) -> bool {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(cbuffer.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    GatherFloatConstants(static_cast<uint8_t*>(mapped.pData), bitmap, first);
    context->Unmap(cbuffer.buffer.Get(), 0);
    cbuffer.up_to_date = true;
    return true;
  };

  if (!cbuffer_system_.up_to_date) {
    if (!UploadConstantBuffer(kCbufferSystem, &system_constants_,
                              uint32_t(sizeof(system_constants_)))) {
      return false;
    }
  }
  if (!cbuffer_float_vertex_.up_to_date &&
      !upload_float_constants(cbuffer_float_vertex_, float_constant_map_vertex.float_bitmap,
                              &regs[XE_GPU_REG_SHADER_CONSTANT_000_X])) {
    return false;
  }
  if (!cbuffer_float_pixel_.up_to_date) {
    static const uint64_t kNoConstants[4] = {};
    if (!upload_float_constants(
            cbuffer_float_pixel_,
            pixel_shader ? pixel_shader->constant_register_map().float_bitmap : kNoConstants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_256_X])) {
      return false;
    }
  }
  if (!cbuffer_bool_loop_.up_to_date &&
      !UploadConstantBuffer(kCbufferBoolLoop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                            kBoolLoopConstantsBytes)) {
    return false;
  }
  if (!cbuffer_fetch_.up_to_date &&
      !UploadConstantBuffer(kCbufferFetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                            kFetchConstantsBytes)) {
    return false;
  }

  // The buffers are dynamic (renamed on every discard), so the bindings stay
  // valid; rebind only after outside code changed the state.
  if (!guest_draw_state_valid_) {
    ID3D11Buffer* vertex_cbuffers[kCbufferCount] = {
        cbuffer_system_.buffer.Get(), cbuffer_float_vertex_.buffer.Get(),
        cbuffer_bool_loop_.buffer.Get(), cbuffer_fetch_.buffer.Get()};
    ID3D11Buffer* pixel_cbuffers[kCbufferCount] = {
        cbuffer_system_.buffer.Get(), cbuffer_float_pixel_.buffer.Get(),
        cbuffer_bool_loop_.buffer.Get(), cbuffer_fetch_.buffer.Get()};
    context->VSSetConstantBuffers(0, kCbufferCount, vertex_cbuffers);
    context->GSSetConstantBuffers(0, kCbufferCount, vertex_cbuffers);
    context->PSSetConstantBuffers(0, kCbufferCount, pixel_cbuffers);
  }

  // Shader resources: t0 is the shared memory (vertex fetch), the textures
  // follow in the translator's binding order. Set on every draw.
  auto bind_stage_resources = [&](const D3D11Shader* shader, uint32_t& bound_srv_count,
                                  bool pixel) -> bool {
    srvs_scratch_.clear();
    samplers_scratch_.clear();
    srvs_scratch_.push_back(shared_memory_->GetRawSRV());
    if (shader) {
      for (const DxbcShader::TextureBinding& binding :
           shader->GetTextureBindingsAfterTranslation()) {
        srvs_scratch_.push_back(texture_cache_->GetActiveTextureSRV(binding));
      }
      for (const DxbcShader::SamplerBinding& binding :
           shader->GetSamplerBindingsAfterTranslation()) {
        samplers_scratch_.push_back(
            texture_cache_->GetSampler(texture_cache_->GetSamplerParameters(binding)));
      }
    }
    uint32_t srv_count = uint32_t(srvs_scratch_.size());
    if (srv_count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
        samplers_scratch_.size() > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
      return false;
    }
    // Clear the slots the previous draw used past this one's count.
    // After outside passes, also clear t0/t1, which the native executor's
    // transfer and resolve passes bind.
    uint32_t set_count = std::max(srv_count, guest_draw_state_valid_ ? bound_srv_count
                                                                      : std::max(bound_srv_count, 2u));
    srvs_scratch_.resize(set_count, nullptr);
    if (pixel) {
      context->PSSetShaderResources(0, set_count, srvs_scratch_.data());
      if (!samplers_scratch_.empty()) {
        context->PSSetSamplers(0, uint32_t(samplers_scratch_.size()), samplers_scratch_.data());
      }
    } else {
      context->VSSetShaderResources(0, set_count, srvs_scratch_.data());
      if (!samplers_scratch_.empty()) {
        context->VSSetSamplers(0, uint32_t(samplers_scratch_.size()), samplers_scratch_.data());
      }
    }
    bound_srv_count = srv_count;
    return true;
  };
  if (!bind_stage_resources(vertex_shader, bound_srv_count_vertex_, false)) {
    REXGPU_ERROR("Direct3D 11: too many vertex shader textures or samplers");
    return false;
  }
  if (!bind_stage_resources(pixel_shader, bound_srv_count_pixel_, true)) {
    REXGPU_ERROR("Direct3D 11: too many pixel shader textures or samplers");
    return false;
  }
  return true;
}

bool D3D11CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  // The primitive processor reads the draw description from the registers.
  (void)primitive_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;

  if (device_lost_) {
    return false;
  }

  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  auto vertex_shader = static_cast<D3D11Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  // Shader model 5.0 drops memory export, so a draw that only exports has no
  // effect here.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (!is_rasterization_done || surface_pitch_is_zero) {
    return true;
  }
  D3D11Shader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    pixel_shader = static_cast<D3D11Shader*>(active_pixel_shader());
    if (pixel_shader) {
      pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
      if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
        pixel_shader = nullptr;
      }
    }
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  draw_util::ApplyFh1GlowDepthTest(regs, normalized_color_mask, normalized_depth_control);
  // A draw that tests and writes neither depth nor stencil and writes no color
  // outside an occlusion query has no guest-visible output.
  if (!active_occlusion_query_.valid && !normalized_depth_control.z_enable &&
      !normalized_depth_control.stencil_enable && !normalized_color_mask) {
    return true;
  }

  BeginFrameIfNeeded();

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (primitive_processing_result.IsTessellated()) {
    // Not supported by the shader model 5.0 path (FH1 issues no tessellated
    // draws on any measured route).
    return true;
  }

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  auto* vertex_shader_translation = static_cast<D3D11Shader::D3D11Translation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  auto* pixel_shader_translation =
      pixel_shader ? static_cast<D3D11Shader::D3D11Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  // Translate before the targets are prepared: a failed translation skips the
  // draw without touching EDRAM ownership.
  if (!pipeline_cache_->EnsureShaderTranslated(*vertex_shader_translation) ||
      (pixel_shader_translation &&
       !pipeline_cache_->EnsureShaderTranslated(*pixel_shader_translation))) {
    return false;
  }

  // Native targets and EDRAM ownership transfers for this draw (may draw and
  // dispatch).
  Fh1NativeDrawInfo native_targets;
  native_targets.memexport = false;
  native_targets.rasterization_done = is_rasterization_done;
  native_targets.normalized_depth_control = normalized_depth_control;
  native_targets.normalized_color_mask = normalized_color_mask;
  native_targets.vertex_shader = vertex_shader;
  native_targets.pixel_shader = pixel_shader;
  fh1_native_executor_->PrepareTargets(native_targets);
  uint32_t bound_depth_and_color_render_target_bits = 0;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets] = {};
  uint32_t bound_width = 0, bound_height = 0;
  if (!fh1_native_executor_->BindTargets(bound_depth_and_color_render_target_bits,
                                         bound_depth_and_color_render_target_formats, bound_width,
                                         bound_height)) {
    return false;
  }

  const D3D11PipelineCache::Pipeline* pipeline = pipeline_cache_->ConfigurePipeline(
      vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
      normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
      bound_depth_and_color_render_target_formats);
  if (!pipeline) {
    return false;
  }

  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  // May dispatch texture loads.
  texture_cache_->RequestTextures(used_texture_mask);

  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();
  draw_util::ViewportInfo viewport_info;
  const uint32_t viewport_x_max = viewport_fit_targets_ && bound_width
                                      ? bound_width
                                      : uint32_t(D3D11_VIEWPORT_BOUNDS_MAX);
  const uint32_t viewport_y_max = viewport_fit_targets_ && bound_height
                                      ? bound_height
                                      : uint32_t(D3D11_VIEWPORT_BOUNDS_MAX);
  draw_util::GetHostViewportInfo(regs, 1, 1, true, viewport_x_max, viewport_y_max, false,
                                 normalized_depth_control,
                                 depth_float24_convert_in_pixel_shader_,
                                 /* full_float24_in_0_to_1 */ true, ps_writes_depth,
                                 viewport_info);
  // FH1's HUD is squeezed under Hor+ like on the other backends (NP-4.4).
  const double hud_squeeze = REXCVAR_GET(fh1_hud_squeeze);
  if (hud_squeeze > 1.0 && normalized_color_mask &&
      regs.Get<reg::RB_COLOR_INFO>(XE_GPU_REG_RB_COLOR_INFO).color_format ==
          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
    viewport_info.ndc_scale[0] = float(viewport_info.ndc_scale[0] / hud_squeeze);
    viewport_info.ndc_offset[0] = float(viewport_info.ndc_offset[0] / hud_squeeze);
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);

  if (!guest_draw_state_valid_) {
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
  }
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);
  UpdateSystemConstantValues(primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      SharedMemory::UploadKindScope upload_kind(*shared_memory_, SharedMemory::UploadKind::kVertex);
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR("Failed to request vertex buffer at 0x{:08X} (size {}) in the shared memory",
                     vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return false;
  }

  ID3D11DeviceContext* context = GetContext();

  // Pipeline state.
  if (!guest_draw_state_valid_ || current_pipeline_ != pipeline) {
    const D3D11PipelineCache::Pipeline* previous =
        guest_draw_state_valid_ ? current_pipeline_ : nullptr;
    if (!previous || previous->vertex_shader != pipeline->vertex_shader) {
      context->VSSetShader(pipeline->vertex_shader, nullptr, 0);
    }
    if (!previous || previous->geometry_shader != pipeline->geometry_shader) {
      context->GSSetShader(pipeline->geometry_shader, nullptr, 0);
    }
    if (!previous || previous->pixel_shader != pipeline->pixel_shader) {
      context->PSSetShader(pipeline->pixel_shader, nullptr, 0);
    }
    if (!previous || previous->rasterizer_state != pipeline->rasterizer_state) {
      context->RSSetState(pipeline->rasterizer_state);
    }
    if (!previous || previous->depth_stencil_state != pipeline->depth_stencil_state) {
      ff_stencil_ref_update_needed_ = true;
    }
    if (!previous || previous->blend_state != pipeline->blend_state ||
        previous->sample_mask != pipeline->sample_mask) {
      ff_blend_factor_update_needed_ = true;
    }
    if (!previous || previous->topology != pipeline->topology) {
      context->IASetPrimitiveTopology(pipeline->topology);
    }
    current_pipeline_ = pipeline;
  }
  if (ff_stencil_ref_update_needed_) {
    context->OMSetDepthStencilState(pipeline->depth_stencil_state, ff_stencil_ref_);
    ff_stencil_ref_update_needed_ = false;
  }
  if (ff_blend_factor_update_needed_) {
    context->OMSetBlendState(pipeline->blend_state, ff_blend_factor_, pipeline->sample_mask);
    ff_blend_factor_update_needed_ = false;
  }
  if (!guest_draw_state_valid_) {
    // No tessellation on the shader model 5.0 path.
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    // Vertex data is fetched from the shared memory in the shader.
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    current_index_buffer_ = nullptr;
    current_index_format_ = DXGI_FORMAT_UNKNOWN;
    current_index_offset_ = 0;
  }
  guest_draw_state_valid_ = true;

  Fh1NativeDrawInfo native_draw;
  native_draw.memexport = false;
  native_draw.occlusion_query_active = active_occlusion_query_.valid;
  native_draw.rasterization_done = is_rasterization_done;
  native_draw.normalized_depth_control = normalized_depth_control;
  native_draw.normalized_color_mask = normalized_color_mask;
  native_draw.vertex_shader = vertex_shader;
  native_draw.pixel_shader = pixel_shader;

  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    context->Draw(primitive_processing_result.host_draw_vertex_count, 0);
  } else {
    ID3D11Buffer* index_buffer = nullptr;
    uint32_t index_offset = 0;
    DXGI_FORMAT index_format =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? DXGI_FORMAT_R16_UINT
            : DXGI_FORMAT_R32_UINT;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer = shared_memory_->GetBuffer();
        index_offset = primitive_processing_result.guest_index_base;
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        if (!primitive_processor_->UploadConvertedIndexBuffer(
                primitive_processing_result.host_index_buffer_handle, index_buffer,
                index_offset)) {
          return false;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
        index_offset = D3D11PrimitiveProcessor::GetBuiltinIndexBufferOffset(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    if (current_index_buffer_ != index_buffer || current_index_format_ != index_format ||
        current_index_offset_ != index_offset) {
      context->IASetIndexBuffer(index_buffer, index_format, index_offset);
      current_index_buffer_ = index_buffer;
      current_index_format_ = index_format;
      current_index_offset_ = index_offset;
    }
    context->DrawIndexed(primitive_processing_result.host_draw_vertex_count, 0, 0);
  }
  fh1_native_executor_->NativeDrawIssued(native_draw);
  ++pending_draw_calls_;
  return true;
}

bool D3D11CommandProcessor::IssueCopy() {
  if (device_lost_) {
    return false;
  }
  BeginFrameIfNeeded();
  uint32_t written_address = 0;
  uint32_t written_length = 0;
  return fh1_native_executor_->NativeResolve(written_address, written_length);
}

//
// Frame replays.
//

bool D3D11CommandProcessor::ReadbackSharedMemory(uint32_t address, uint32_t length,
                                                 std::vector<uint8_t>& out) {
  if (!length) {
    out.clear();
    return true;
  }
  if (!shared_memory_->RequestRange(address, length)) {
    return false;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = length;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
  if (FAILED(GetDevice()->CreateBuffer(&desc, nullptr, &staging))) {
    return false;
  }
  ID3D11DeviceContext* context = GetContext();
  D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, shared_memory_->GetBuffer(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  out.resize(length);
  std::memcpy(out.data(), mapped.pData, length);
  context->Unmap(staging.Get(), 0);
  return true;
}

void D3D11CommandProcessor::RunRequestedFrameReplay() {
  const int exit_code = RunFh1FrameReplay(
      *this, *register_file_, *memory_, *shared_memory_,
      [this](uint32_t address, uint32_t length, std::vector<uint8_t>& out) {
        return ReadbackSharedMemory(address, length, out);
      });
  if (exit_code < 0) {
    return;
  }
  fh1_native_executor_->LogStats(0);
  // A replay is a tool run with the title suspended: end the process here.
  rex::FlushLogging();
  std::_Exit(exit_code);
}

}  // namespace rex::graphics::d3d11
