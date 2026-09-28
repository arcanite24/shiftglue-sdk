#include <rex/graphics/d3d12/fh1_native_executor.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/graphics_util.h>

REXCVAR_DEFINE_STRING(fh1_renderer, "xenos", "GPU/D3D12",
                      "FH1 session renderer: xenos, native-shadow (the native executor renders "
                      "privately beside Xenos, which presents) or native (the native executor "
                      "renders and presents; the Xenos EDRAM emulation is not used)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_shadow, false, "GPU/D3D12",
                    "Same as fh1_renderer=native-shadow")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_native_shadow_dump_frames, "", "GPU/D3D12",
                      "Comma-separated frames whose native front buffer is written as PPM")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_native_shadow_dump_dir, "", "GPU/D3D12",
                      "Directory for native front-buffer PPM dumps")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_shadow_verify, false, "GPU/D3D12",
                    "On dump frames, compare every native resolve's guest-memory bytes with "
                    "the Xenos resolve of the same copy")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_color_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_color_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_depth_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_depth_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
}  // namespace shaders

namespace {

constexpr uint32_t kRtvCapacity = 1024;
constexpr uint32_t kDsvCapacity = 256;
constexpr uint32_t kResolveMemoryConstantCount = 8;

// Same rectangle derivation as draw_util::GetResolveInfo, in surface pixels
// relative to the base in the color or depth info register.
bool ResolveRectangle(const RegisterFile& regs, const memory::Memory& memory, int32_t& x0,
                      int32_t& y0, int32_t& x1, int32_t& y1) {
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) return false;
  const float* vertices =
      reinterpret_cast<const float*>(memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  if (!vertices) return false;
  const float half_pixel =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (size_t i = 0; i < 6; ++i) {
    fixed[i] = ui::FloatToD3D11Fixed16p8(xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel);
  }
  x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    x1 += offset.window_x_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  const int32_t right = int32_t(scissor.offset[0] + scissor.extent[0]);
  const int32_t bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), bottom);
  constexpr int32_t kAlign = int32_t(xenos::kResolveAlignmentPixels);
  x0 &= ~(kAlign - 1);
  y0 &= ~(kAlign - 1);
  x1 = (x1 + kAlign - 1) & ~(kAlign - 1);
  y1 = (y1 + kAlign - 1) & ~(kAlign - 1);
  const int32_t pitch =
      int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch & ~uint32_t(kAlign - 1));
  x0 = std::min(x0, pitch);
  x1 = std::min(x1, pitch);
  return x0 < x1 && y0 < y1;
}

// Color formats whose guest EDRAM words the resolve shader can encode and
// decode.
bool IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return true;
    default:
      return false;
  }
}

// [dest kind][dest msaa][source depth][source msaa]
const D3D12_SHADER_BYTECODE kTransferShaders[3][2][2][2] = {
    {
        {
            {{shaders::fh1_native_transfer_color_from_color_ps, sizeof(shaders::fh1_native_transfer_color_from_color_ps)}, {shaders::fh1_native_transfer_color_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_color_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_color_from_depth_ps, sizeof(shaders::fh1_native_transfer_color_from_depth_ps)}, {shaders::fh1_native_transfer_color_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_color_from_depth_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_color_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_color_ps)}, {shaders::fh1_native_transfer_color_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_color_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_depth_ps)}, {shaders::fh1_native_transfer_color_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_depth_ms_ps)}},
        },
    },
    {
        {
            {{shaders::fh1_native_transfer_depth_from_color_ps, sizeof(shaders::fh1_native_transfer_depth_from_color_ps)}, {shaders::fh1_native_transfer_depth_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_depth_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_from_depth_ps, sizeof(shaders::fh1_native_transfer_depth_from_depth_ps)}, {shaders::fh1_native_transfer_depth_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_depth_from_depth_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_depth_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_color_ps)}, {shaders::fh1_native_transfer_depth_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_depth_ps)}, {shaders::fh1_native_transfer_depth_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_depth_ms_ps)}},
        },
    },
    {
        {
            {{shaders::fh1_native_transfer_stencil_from_color_ps, sizeof(shaders::fh1_native_transfer_stencil_from_color_ps)}, {shaders::fh1_native_transfer_stencil_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_from_depth_ps, sizeof(shaders::fh1_native_transfer_stencil_from_depth_ps)}, {shaders::fh1_native_transfer_stencil_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_from_depth_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_stencil_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_color_ps)}, {shaders::fh1_native_transfer_stencil_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_depth_ps)}, {shaders::fh1_native_transfer_stencil_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_depth_ms_ps)}},
        },
    },
};

uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

}  // namespace

std::string Fh1NativeExecutor::SurfaceKey::Describe() const {
  static const char* kMsaa[] = {"1x", "2x", "4x", "?"};
  return std::to_string(base_tiles) + "/" + std::to_string(pitch_tiles) + "/" + kMsaa[msaa] +
         (is_depth ? "/d" : "/c") + std::to_string(format);
}

Fh1NativeExecutor::Fh1NativeExecutor(D3D12CommandProcessor& command_processor,
                                     const RegisterFile& register_file, memory::Memory& memory)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      draw_extent_estimator_(register_file, memory) {}

Fh1NativeExecutor::~Fh1NativeExecutor() { Shutdown(); }

Fh1NativeExecutor::CpuTimer::CpuTimer(Fh1NativeExecutor& executor, CpuPhase phase)
    : executor_(executor),
      phase_(phase),
      start_(std::chrono::steady_clock::now().time_since_epoch().count()) {}

Fh1NativeExecutor::CpuTimer::~CpuTimer() {
  executor_.cpu_ns_[phase_] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::duration(
          std::chrono::steady_clock::now().time_since_epoch().count() - start_)).count());
}

bool Fh1NativeExecutor::Enabled() {
  static const bool enabled = REXCVAR_GET(fh1_native_shadow) || Presents() ||
                              REXCVAR_GET(fh1_renderer) == "native-shadow";
  return enabled;
}

bool Fh1NativeExecutor::Presents() {
  static const bool presents = REXCVAR_GET(fh1_renderer) == "native";
  return presents;
}

bool Fh1NativeExecutor::Initialize(const Fh1NativeExecutorConfig& config) {
  config_ = config;
  auto& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = kRtvCapacity;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)))) return false;
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heap_desc.NumDescriptors = kDsvCapacity;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_)))) return false;
  rtv_size_ = provider.GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_size_ = provider.GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  {
    // The format of a null view does not matter, but it must be renderable.
    D3D12_RENDER_TARGET_VIEW_DESC null_desc = {};
    null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    null_rtv_single_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    null_rtv_single_.ptr += SIZE_T(rtv_used_++) * rtv_size_;
    device->CreateRenderTargetView(nullptr, &null_desc, null_rtv_single_);
    null_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    null_rtv_multisample_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    null_rtv_multisample_.ptr += SIZE_T(rtv_used_++) * rtv_size_;
    device->CreateRenderTargetView(nullptr, &null_desc, null_rtv_multisample_);
  }

  // Resolve to memory: constants, source (color or depth), stencil, mirror.
  D3D12_ROOT_PARAMETER parameters[4] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = kResolveMemoryConstantCount;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = i;
    parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[3].Descriptor.ShaderRegister = 0;
  parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 4;
  root_desc.pParameters = parameters;
  ID3D12RootSignature* root_signature = ui::d3d12::util::CreateRootSignature(provider, root_desc);
  if (!root_signature) return false;
  resolve_memory_root_signature_.Attach(root_signature);

  // Ownership transfer: constants, source (color or depth), stencil.
  D3D12_ROOT_PARAMETER transfer_parameters[3] = {};
  transfer_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  transfer_parameters[0].Constants.ShaderRegister = 0;
  transfer_parameters[0].Constants.Num32BitValues = 3;
  transfer_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  for (uint32_t i = 0; i < 2; ++i) {
    transfer_parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    transfer_parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    transfer_parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    transfer_parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  root_desc.NumParameters = 3;
  root_desc.pParameters = transfer_parameters;
  root_signature = ui::d3d12::util::CreateRootSignature(provider, root_desc);
  if (!root_signature) return false;
  transfer_root_signature_.Attach(root_signature);

  if (config_.presents) {
    native_memory_ = config_.memory;
    native_textures_ = config_.textures;
    if (!native_memory_ || !native_textures_) return false;
  } else {
    owned_memory_ = std::make_unique<D3D12SharedMemory>(command_processor_, memory_);
    if (!owned_memory_->Initialize()) {
      REXGPU_ERROR("FH1 native executor: failed to initialize the guest-memory mirror");
      return false;
    }
    owned_textures_ = D3D12TextureCache::Create(register_file_, *owned_memory_, 1, 1,
                                                command_processor_, config_.bindless);
    if (!owned_textures_) {
      REXGPU_ERROR("FH1 native executor: failed to initialize the texture cache");
      return false;
    }
    native_memory_ = owned_memory_.get();
    native_textures_ = owned_textures_.get();
  }

  tile_owners_.assign(xenos::kEdramTileCount, kNoOwner);

  std::stringstream frames(REXCVAR_GET(fh1_native_shadow_dump_frames));
  std::string frame;
  while (std::getline(frames, frame, ',')) {
    if (!frame.empty()) dump_frames_.insert(std::strtoull(frame.c_str(), nullptr, 10));
  }
  dump_directory_ = std::filesystem::path(REXCVAR_GET(fh1_native_shadow_dump_dir));
  // Verification compares with Xenos, which does not render in native mode.
  verify_ = REXCVAR_GET(fh1_native_shadow_verify) && !config_.presents;
  initialized_ = true;
  REXGPU_INFO("FH1 native executor enabled: {} ({} dump frames, verify {})",
              config_.presents ? "native" : "native-shadow", dump_frames_.size(), verify_);
  return true;
}

void Fh1NativeExecutor::Shutdown() {
  if (initialized_) LogStats(frame_);
  initialized_ = false;
  dumps_.clear();
  verifies_.clear();
  surfaces_.clear();
  native_textures_ = nullptr;
  native_memory_ = nullptr;
  owned_textures_.reset();
  if (owned_memory_) owned_memory_->Shutdown();
  owned_memory_.reset();
  for (auto& by_depth : resolve_memory_pipelines_) {
    for (auto& pipeline : by_depth) pipeline.Reset();
  }
  resolve_memory_root_signature_.Reset();
  transfer_pipelines_.clear();
  transfer_root_signature_.Reset();
  rtv_heap_.Reset();
  dsv_heap_.Reset();
}

// The command processor drives the lifecycle of its own mirror and texture
// cache; only the executor's own ones (shadow mode) are forwarded here.
void Fh1NativeExecutor::CompletedSubmissionUpdated(uint64_t completed_submission) {
  if (!initialized_) return;
  if (owned_memory_) owned_memory_->CompletedSubmissionUpdated();
  if (owned_textures_) owned_textures_->CompletedSubmissionUpdated(completed_submission);
  DrainVerifies();
}

void Fh1NativeExecutor::BeginSubmission(uint64_t current_submission) {
  if (!initialized_) return;
  if (owned_textures_) owned_textures_->BeginSubmission(current_submission);
}

void Fh1NativeExecutor::BeginFrame() {
  if (!initialized_) return;
  if (owned_textures_) owned_textures_->BeginFrame();
}

void Fh1NativeExecutor::EndFrame() {
  if (!initialized_) return;
  if (owned_textures_) owned_textures_->EndFrame();
}

void Fh1NativeExecutor::ClearCache() {
  if (!initialized_) return;
  if (owned_textures_) owned_textures_->ClearCache();
  if (owned_memory_) owned_memory_->ClearCache();
}

void Fh1NativeExecutor::SkipDraw(const char* reason, const Fh1NativeDrawInfo& draw) {
  Skip(reason);
  if (tracing_) Trace(std::string("draw skipped ") + reason);
  const uint64_t vs = draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0;
  const uint64_t ps = draw.pixel_shader ? draw.pixel_shader->ucode_data_hash() : 0;
  if (ShouldLog()) LogOnce(vs ^ (ps << 1) ^ std::hash<std::string>{}(reason),
          std::string("draw skipped (") + reason + ") vs " + std::to_string(vs) + " ps " +
              std::to_string(ps));
}

void Fh1NativeExecutor::TextureFetchConstantsWritten(uint32_t first_index,
                                                     uint32_t last_index) {
  if (owned_textures_) owned_textures_->TextureFetchConstantsWritten(first_index, last_index);
}

void Fh1NativeExecutor::Trace(const std::string& event) {
  if (!verify_ || !dump_frames_.count(frame_ + 1) || trace_lines_ >= 400) return;
  ++trace_lines_;
  REXGPU_INFO("FH1 native trace {} {}", frame_ + 1, event);
}

void Fh1NativeExecutor::LogOnce(uint64_t signature, const std::string& message) {
  if (logged_.size() >= 256 || !logged_.insert(signature).second) return;
  REXGPU_INFO("FH1 native executor {}", message);
}

void Fh1NativeExecutor::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                                   D3D12_RESOURCE_STATES new_state) {
  if (state == new_state) return;
  command_processor_.PushTransitionBarrier(resource, state, new_state);
  state = new_state;
}

uint32_t Fh1NativeExecutor::PitchTiles(uint32_t pitch_pixels, uint32_t msaa) {
  const uint32_t msaa_x_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k4X));
  return ((pitch_pixels << msaa_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
         xenos::kEdramTileWidthSamples;
}

uint32_t Fh1NativeExecutor::SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) {
  // Down to the start of the same surface in the next EDRAM addressing period,
  // clamped to the guest texture size limit (RenderTargetCache at 1x scale).
  if (!pitch_tiles) return 0;
  uint32_t tile_rows = (xenos::kEdramTileCount + pitch_tiles - 1) / pitch_tiles;
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  tile_rows = std::min(tile_rows, (xenos::kTexture2DCubeMaxWidthHeight << msaa_y_log2) /
                                      xenos::kEdramTileHeightSamples);
  return tile_rows * (xenos::kEdramTileHeightSamples >> msaa_y_log2);
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeColorKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
    xenos::ColorRenderTargetFormat format) const {
  SurfaceKey key;
  key.base_tiles = base & (xenos::kEdramTileCount - 1);
  key.pitch_tiles = pitch_tiles;
  key.msaa = msaa;
  key.is_depth = false;
  format = xenos::GetStorageColorFormat(format);
  if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA && !config_.gamma_as_unorm16) {
    format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  key.format = uint32_t(format);
  return key;
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeDepthKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa, xenos::DepthRenderTargetFormat format) {
  SurfaceKey key;
  key.base_tiles = base & (xenos::kEdramTileCount - 1);
  key.pitch_tiles = pitch_tiles;
  key.msaa = msaa;
  key.is_depth = true;
  key.format = uint32_t(format);
  return key;
}

DXGI_FORMAT Fh1NativeExecutor::ColorResourceFormat(xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return config_.gamma_as_unorm16 ? DXGI_FORMAT_R16G16B16A16_UNORM
                                      : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT Fh1NativeExecutor::ColorDrawFormat(xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return ColorResourceFormat(format);
  }
}

Fh1NativeExecutor::Surface* Fh1NativeExecutor::FindSurface(uint32_t packed_key) {
  auto it = surfaces_.find(packed_key);
  return it != surfaces_.end() ? &it->second : nullptr;
}

Fh1NativeExecutor::Surface* Fh1NativeExecutor::GetOrCreateSurface(const SurfaceKey& key) {
  const uint32_t packed = key.Pack();
  if (Surface* existing = FindSurface(packed)) return existing;
  if (key.is_depth ? dsv_used_ >= kDsvCapacity : rtv_used_ >= kRtvCapacity) {
    Skip("surface_view_capacity");
    return nullptr;
  }
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  Surface surface;
  surface.key = key;
  surface.width = key.pitch_tiles * (xenos::kEdramTileWidthSamples >> msaa_x_log2);
  surface.height = SurfaceHeight(key.pitch_tiles, key.msaa);
  surface.samples = key.msaa == uint32_t(xenos::MsaaSamples::k2X) && !config_.msaa_2x_supported
                        ? 4
                        : 1u << key.msaa;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface.width;
  desc.Height = surface.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = surface.samples;
  D3D12_CLEAR_VALUE clear_value = {};
  if (key.is_depth) {
    const auto format = xenos::DepthRenderTargetFormat(key.format);
    desc.Format = D3D12RenderTargetCache::GetDepthResourceDXGIFormat(format);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    surface.view_format = D3D12RenderTargetCache::GetDepthDSVDXGIFormat(format);
    surface.srv_format = D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(format);
    surface.stencil_srv_format = D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(format);
    surface.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    clear_value.Format = surface.view_format;
    clear_value.DepthStencil.Depth = format == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f : 0.0f;
  } else {
    const auto format = xenos::ColorRenderTargetFormat(key.format);
    desc.Format = ColorResourceFormat(format);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    surface.view_format = ColorDrawFormat(format);
    surface.srv_format = surface.view_format;
    surface.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    clear_value.Format = surface.view_format;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN || !surface.width || !surface.height) {
    Skip("surface_format");
    return nullptr;
  }
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, surface.state,
                                             &clear_value, IID_PPV_ARGS(&surface.resource)))) {
    Skip("surface_create");
    return nullptr;
  }
  const bool msaa = surface.samples > 1;
  if (key.is_depth) {
    surface.view = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    surface.view.ptr += SIZE_T(dsv_used_++) * dsv_size_;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = surface.view_format;
    dsv.ViewDimension = msaa ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(surface.resource.Get(), &dsv, surface.view);
  } else {
    surface.view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    surface.view.ptr += SIZE_T(rtv_used_++) * rtv_size_;
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = surface.view_format;
    rtv.ViewDimension = msaa ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(surface.resource.Get(), &rtv, surface.view);
  }
  Count("surface_created");
  return &surfaces_.emplace(packed, std::move(surface)).first->second;
}

void Fh1NativeExecutor::ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key) {
  length = std::min(length, xenos::kEdramTileCount);
  if (!length) return;
  // A surface that already owns the whole range needs no work; its cached
  // claim is dropped whenever another surface takes any of its tiles.
  auto last = last_claims_.find(packed_key);
  if (last != last_claims_.end() && last->second.first == base &&
      last->second.second >= length) {
    return;
  }
  std::vector<TileRun> runs;
  for (uint32_t i = 0; i < length; ++i) {
    uint32_t& owner = tile_owners_[(base + i) & (xenos::kEdramTileCount - 1)];
    if (owner == packed_key) continue;
    if (owner != kNoOwner) {
      if (!runs.empty() && runs.back().previous_owner == owner &&
          runs.back().first + runs.back().count == base + i) {
        ++runs.back().count;
      } else {
        runs.push_back({base + i, 1, owner});
      }
    }
    owner = packed_key;
  }
  for (const TileRun& run : runs) last_claims_.erase(run.previous_owner);
  last_claims_[packed_key] = {base, length};
  if (runs.empty()) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  for (const TileRun& run : runs) TransferTiles(*dest, run);
}

uint32_t Fh1NativeExecutor::LayoutConstant(const Surface& surface) const {
  const SurfaceKey& key = surface.key;
  const uint32_t host_sample_mode =
      key.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  return PackLayout(key.base_tiles, key.pitch_tiles, key.msaa, key.Is64bpp(), key.is_depth,
                    key.format) |
         (host_sample_mode << 27);
}

ID3D12PipelineState* Fh1NativeExecutor::GetTransferPipeline(const TransferPipelineKey& key) {
  auto it = transfer_pipelines_.find(key);
  if (it != transfer_pipelines_.end()) return it->second.Get();
  const uint32_t kind = std::min(key.dest_kind, 2u);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = transfer_root_signature_.Get();
  desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
  desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
  desc.PS = kTransferShaders[kind][key.dest_samples > 1][key.source_depth][key.source_msaa];
  desc.SampleMask = key.sample_mask;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = FALSE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.SampleDesc.Count = key.dest_samples;
  if (kind == 0) {
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = key.dest_format;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  } else {
    desc.DSVFormat = key.dest_format;
    auto& depth_stencil = desc.DepthStencilState;
    depth_stencil.StencilEnable = TRUE;
    depth_stencil.StencilReadMask = 0xFF;
    depth_stencil.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth_stencil.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth_stencil.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth_stencil.BackFace = depth_stencil.FrontFace;
    if (kind == 1) {
      // Depth, and stencil reset to 0 (reference 0).
      depth_stencil.DepthEnable = TRUE;
      depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      depth_stencil.StencilWriteMask = 0xFF;
    } else {
      // One stencil bit where the source has it (reference 0xFF).
      depth_stencil.DepthEnable = FALSE;
      depth_stencil.StencilWriteMask = uint8_t(1u << (key.dest_kind - 2));
    }
  }
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    return nullptr;
  }
  return transfer_pipelines_.emplace(key, pipeline).first->second.Get();
}

void Fh1NativeExecutor::TransferTiles(Surface& dest, const TileRun& run) {
  CpuTimer timer(*this, kCpuTransfers);
  Surface* source = FindSurface(run.previous_owner);
  if (!source) return Skip("transfer_source_missing");
  if (dest.key.Is64bpp() || source->key.Is64bpp()) return Skip("transfer_64bpp");
  if ((!dest.key.is_depth &&
       !IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(dest.key.format))) ||
      (!source->key.is_depth &&
       !IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(source->key.format)))) {
    return Skip("transfer_format");
  }
  Count("transfer");
  if (tracing_) Trace("transfer " + source->key.Describe() + " -> " + dest.key.Describe() + " tiles " +
        std::to_string(run.first) + "+" + std::to_string(run.count));
  if (verify_ && ShouldLog()) LogOnce((uint64_t(source->key.Pack()) << 32) ^ dest.key.Pack() ^ 0x7F7F,
          "transfer " + source->key.Describe() + " -> " + dest.key.Describe());

  // The run's tiles as at most three rectangles of the destination: the rest
  // of the first tile row, whole rows, and the start of the last row.
  const uint32_t msaa_x_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t pitch = dest.key.pitch_tiles;
  const uint32_t first = (run.first - dest.key.base_tiles) & (xenos::kEdramTileCount - 1);
  const uint32_t end = first + run.count;
  D3D12_RECT rects[3];
  uint32_t rect_count = 0;
  auto add_rect = [&](uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    D3D12_RECT rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
                       LONG(std::min(column_end * tile_width, dest.width)),
                       LONG(std::min(row_end * tile_height, dest.height))};
    if (rect.left < rect.right && rect.top < rect.bottom) rects[rect_count++] = rect;
  };
  const uint32_t first_row = first / pitch, last_row = (end - 1) / pitch;
  if (first_row == last_row) {
    add_rect(first % pitch, first_row, (end - 1) % pitch + 1, first_row + 1);
  } else {
    uint32_t full_first = first_row;
    if (first % pitch) {
      add_rect(first % pitch, first_row, pitch, first_row + 1);
      ++full_first;
    }
    uint32_t full_end = last_row + 1;
    if (end % pitch) {
      --full_end;
      add_rect(0, last_row, end % pitch, last_row + 1);
    }
    if (full_first < full_end) add_rect(0, full_first, pitch, full_end);
  }
  if (!rect_count) return;

  ui::d3d12::util::DescriptorCpuGpuHandlePair srvs[2];
  const bool source_depth = source->key.is_depth;
  if (!command_processor_.RequestOneUseSingleViewDescriptors(source_depth ? 2 : 1, srvs)) {
    return Skip("transfer_descriptor");
  }
  if (!source_depth) srvs[1] = srvs[0];
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  const bool source_msaa = source->samples > 1;
  for (uint32_t i = 0; i < (source_depth ? 2u : 1u); ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = i ? source->stencil_srv_format : source->srv_format;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (source_msaa) {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Texture2D.MipLevels = 1;
      srv_desc.Texture2D.PlaneSlice = i;
    }
    device->CreateShaderResourceView(source->resource.Get(), &srv_desc, srvs[i].first);
  }
  Transition(source->resource.Get(), source->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(dest.resource.Get(), dest.state,
             dest.key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                               : D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_processor_.SubmitBarriers();

  const uint32_t constants[3] = {
      LayoutConstant(dest), LayoutConstant(*source),
      (config_.depth_float24_round ? 1u : 0u) | (config_.gamma_as_unorm16 ? 2u : 0u)};
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                             : UINT_MAX;
  auto& list = command_processor_.GetDeferredCommandList();
  command_processor_.SetExternalGraphicsRootSignature(transfer_root_signature_.Get());
  list.D3DSetGraphicsRootDescriptorTable(1, srvs[0].second);
  list.D3DSetGraphicsRootDescriptorTable(2, srvs[1].second);
  command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (dest.key.is_depth) {
    list.D3DOMSetRenderTargets(0, nullptr, FALSE, &dest.view);
  } else {
    list.D3DOMSetRenderTargets(1, &dest.view, FALSE, nullptr);
  }
  const uint32_t pass_count = dest.key.is_depth ? 9 : 1;
  for (uint32_t pass = 0; pass < pass_count; ++pass) {
    TransferPipelineKey key;
    key.dest_kind = dest.key.is_depth ? (pass ? 1 + pass : 1) : 0;
    key.dest_format = dest.view_format;
    key.dest_samples = dest.samples;
    key.sample_mask = sample_mask;
    key.source_depth = source_depth;
    key.source_msaa = source_msaa;
    ID3D12PipelineState* pipeline = GetTransferPipeline(key);
    if (!pipeline) return Skip("transfer_pipeline");
    command_processor_.SetExternalPipeline(pipeline);
    const uint32_t pass_constants[3] = {constants[0], constants[1],
                                        constants[2] | (pass ? (pass - 1) << 8 : 0)};
    list.D3DSetGraphicsRoot32BitConstants(0, 3, pass_constants, 0);
    if (dest.key.is_depth) command_processor_.SetStencilReference(pass ? 0xFF : 0);
    for (uint32_t i = 0; i < rect_count; ++i) {
      const D3D12_RECT& rect = rects[i];
      D3D12_VIEWPORT viewport = {float(rect.left), float(rect.top),
                                 float(rect.right - rect.left), float(rect.bottom - rect.top),
                                 0.0f, 1.0f};
      command_processor_.SetViewport(viewport);
      command_processor_.SetScissorRect(rect);
      list.D3DDrawInstanced(3, 1, 0, 0);
    }
  }
}

void Fh1NativeExecutor::GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0,
                                          int32_t x1, int32_t y1,
                                          std::vector<SourceRect>& sources_out) {
  sources_out.clear();
  const uint32_t msaa_x_log2 = uint32_t(resolve_key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(resolve_key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t is_64bpp = resolve_key.Is64bpp() ? 1 : 0;
  const int32_t tile_width = int32_t((xenos::kEdramTileWidthSamples >> msaa_x_log2) >> is_64bpp);
  const int32_t tile_height = int32_t(xenos::kEdramTileHeightSamples >> msaa_y_log2);
  const uint32_t pitch = resolve_key.pitch_tiles << is_64bpp;
  const int32_t column_first = x0 / tile_width, column_end = (x1 + tile_width - 1) / tile_width;
  const int32_t row_first = y0 / tile_height, row_end = (y1 + tile_height - 1) / tile_height;
  struct Run {
    int32_t column_first, column_end;
    uint32_t owner;
    size_t source;
  };
  std::vector<Run> previous_row, current_row;
  for (int32_t row = row_first; row < row_end; ++row) {
    current_row.clear();
    for (int32_t column = column_first; column < column_end; ++column) {
      const uint32_t tile =
          (resolve_key.base_tiles + uint32_t(row) * pitch + uint32_t(column)) &
          (xenos::kEdramTileCount - 1);
      const uint32_t owner = tile_owners_[tile];
      if (!current_row.empty() && current_row.back().owner == owner &&
          current_row.back().column_end == column) {
        ++current_row.back().column_end;
      } else {
        current_row.push_back({column, column + 1, owner, SIZE_MAX});
      }
    }
    for (Run& run : current_row) {
      // Extend the rectangle of an identical run in the previous tile row.
      for (const Run& above : previous_row) {
        if (above.column_first == run.column_first && above.column_end == run.column_end &&
            above.owner == run.owner && above.source != SIZE_MAX) {
          run.source = above.source;
          sources_out[run.source].rect.bottom = (row + 1) * tile_height;
          break;
        }
      }
      if (run.source != SIZE_MAX) continue;
      SourceRect source;
      source.rect = {std::max(run.column_first * tile_width, x0), row * tile_height,
                     std::min(run.column_end * tile_width, x1), (row + 1) * tile_height};
      source.unowned = run.owner == kNoOwner;
      source.surface = source.unowned ? nullptr : FindSurface(run.owner);
      run.source = sources_out.size();
      sources_out.push_back(source);
    }
    std::swap(previous_row, current_row);
  }
  for (SourceRect& source : sources_out) {
    source.rect.top = std::max<LONG>(source.rect.top, y0);
    source.rect.bottom = std::min<LONG>(source.rect.bottom, y1);
  }
}

ID3D12PipelineState* Fh1NativeExecutor::GetResolveMemoryPipeline(bool depth, bool msaa) {
  auto& pipeline = resolve_memory_pipelines_[depth][msaa];
  if (pipeline) return pipeline.Get();
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = resolve_memory_root_signature_.Get();
  if (depth) {
    desc.CS.pShaderBytecode = msaa ? shaders::fh1_native_resolve_memory_depth_ms_cs
                                   : shaders::fh1_native_resolve_memory_depth_cs;
    desc.CS.BytecodeLength = msaa ? sizeof(shaders::fh1_native_resolve_memory_depth_ms_cs)
                                  : sizeof(shaders::fh1_native_resolve_memory_depth_cs);
  } else {
    desc.CS.pShaderBytecode = msaa ? shaders::fh1_native_resolve_memory_color_ms_cs
                                   : shaders::fh1_native_resolve_memory_color_cs;
    desc.CS.BytecodeLength = msaa ? sizeof(shaders::fh1_native_resolve_memory_color_ms_cs)
                                  : sizeof(shaders::fh1_native_resolve_memory_color_cs);
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    return nullptr;
  }
  return pipeline.Get();
}

void Fh1NativeExecutor::PrepareDraw(uint32_t used_texture_mask, const Shader& vertex_shader,
                                    uint32_t guest_dma_index_offset,
                                    uint32_t guest_dma_index_size) {
  if (!initialized_) return;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        register_file_.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) continue;
    if (!native_memory_->RequestRange(fetch.address << 2, fetch.size << 2)) {
      Skip("draw_vertex_range");
    }
  }
  if (guest_dma_index_size &&
      !native_memory_->RequestRange(guest_dma_index_offset, guest_dma_index_size)) {
    Skip("draw_index_range");
  }
  native_textures_->RequestTextures(used_texture_mask);
  pending_texture_mask_ = used_texture_mask;
  if (verify_ && !dump_frames_.count(frame_ + 1)) {
    // Compare the mirrors' bytes of each fetched base level, once per texture.
    for (uint32_t mask = used_texture_mask; mask; mask &= mask - 1) {
      const auto fetch = register_file_.GetTextureFetch(uint32_t(std::countr_zero(mask)));
      uint32_t width = 0, height = 0, depth = 1;
      switch (fetch.dimension) {
        case xenos::DataDimension::k1D:
          width = fetch.size_1d.width + 1;
          height = 1;
          break;
        case xenos::DataDimension::k3D:
          width = fetch.size_3d.width + 1;
          height = fetch.size_3d.height + 1;
          depth = fetch.size_3d.depth + 1;
          break;
        default:
          width = fetch.size_2d.width + 1;
          height = fetch.size_2d.height + 1;
          depth = fetch.size_2d.stack_depth + 1;
          break;
      }
      const uint32_t base_size =
          texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, depth,
                                              fetch.tiled, fetch.format, fetch.packed_mips, true,
                                              0)
              .base.level_data_extent_bytes;
      const uint32_t base = fetch.base_address << 12;
      // Once per texture per 30 frames.
      if (!base || !base_size ||
          !verified_textures_.insert(base ^ (uint64_t(base_size) << 32) ^ ((frame_ / 30) << 40))
               .second) {
        continue;
      }
      const auto native_valid = native_memory_->CountValidPages(base, base_size);
      const auto xenos_valid =
          command_processor_.Fh1XenosSharedMemory().CountValidPages(base, base_size);
      QueueVerify(base, std::min<uint32_t>(base_size, 4u << 20),
                  "tex" + std::to_string(uint32_t(fetch.format)) + "d" +
                      std::to_string(uint32_t(fetch.dimension)) + " valid native " +
                      std::to_string(native_valid.first) + "/" +
                      std::to_string(native_valid.second) + " xenos " +
                      std::to_string(xenos_valid.first) + "/" +
                      std::to_string(xenos_valid.second) + " pages " +
                      std::to_string((base_size + 4095) / 4096));
    }
  }
}

void Fh1NativeExecutor::PrepareTargets(const Fh1NativeDrawInfo& draw) {
  if (!initialized_) return;
  CpuTimer timer(*this, kCpuPrepareTargets);
  pending_targets_valid_ = true;
  pending_used_bits_ = 0;
  if (draw.memexport) return;
  const RegisterFile& regs = register_file_;

  // Targets the draw writes, derived the way the guest GPU addresses EDRAM:
  // depth when tested or stencilled, colors with a nonzero write mask, the
  // lower slot winning a shared base.
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch_tiles = PitchTiles(surface_info.surface_pitch, msaa);
  uint32_t used_bits = 0;
  SurfaceKey* keys = pending_keys_;
  if (draw.rasterization_done && pitch_tiles) {
    if (draw.normalized_depth_control.z_enable || draw.normalized_depth_control.stencil_enable) {
      const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      keys[0] = MakeDepthKey(depth_info.depth_base, pitch_tiles, msaa, depth_info.depth_format);
      used_bits |= 1;
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(draw.normalized_color_mask & (0xFu << (4 * i)))) continue;
      const auto color_info =
          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      keys[1 + i] = MakeColorKey(color_info.color_base, pitch_tiles, msaa, color_info.color_format);
      used_bits |= 1u << (1 + i);
    }
    for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << i))) continue;
      for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
        if (j != i && (j == 0 || j > i) && (used_bits & (1u << j)) &&
            keys[j].base_tiles == keys[i].base_tiles) {
          used_bits &= ~(1u << j);
        }
      }
    }
  }
  pending_used_bits_ = used_bits;
  if (tracing_) {
    std::string targets;
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (used_bits & (1u << i)) targets += keys[i].Describe() + " ";
    }
    if (tracing_) Trace("draw vs " +
          std::to_string(draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0) +
          " targets " + targets);
  }
  if (verify_) {
    std::string targets;
    uint64_t signature = draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0;
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << i))) continue;
      targets += keys[i].Describe() + " ";
      signature = signature * 31 + keys[i].Pack();
    }
    if (ShouldLog()) LogOnce(signature ^ 0x7A7A7A,
            "vs " + std::to_string(draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0) +
                " targets " + targets);
  }
  if (!used_bits) return;

  // Create the surfaces first so ownership transfers can write into them.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if ((used_bits & (1u << i)) && !GetOrCreateSurface(keys[i])) used_bits &= ~(1u << i);
  }

  // Tiles each target covers, as far down as the draw can reach.
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t height_used =
      std::min(SurfaceHeight(pitch_tiles, msaa),
               draw.vertex_shader ? draw_extent_estimator_.EstimateMaxY(true, *draw.vertex_shader)
                                  : SurfaceHeight(pitch_tiles, msaa));
  const uint32_t length_tiles_32bpp =
      ((height_used << msaa_y_log2) + xenos::kEdramTileHeightSamples - 1) /
      xenos::kEdramTileHeightSamples * pitch_tiles;
  std::vector<std::pair<uint32_t, uint32_t>> bases;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (used_bits & (1u << i)) bases.emplace_back(keys[i].base_tiles, i);
  }
  std::sort(bases.begin(), bases.end());
  for (size_t i = 0; i < bases.size(); ++i) {
    const SurfaceKey& key = keys[bases[i].second];
    const uint32_t next_base = i + 1 < bases.size()
                                   ? bases[i + 1].first
                                   : xenos::kEdramTileCount + bases[0].first;
    const uint32_t length =
        std::min(length_tiles_32bpp << uint32_t(key.Is64bpp()), next_base - key.base_tiles);
    ClaimTiles(key.base_tiles, length, key.Pack());
  }
}

void Fh1NativeExecutor::VerifyTargetsBeforeDraw(D3D12RenderTargetCache& render_target_cache,
                                                const Fh1NativeDrawInfo& draw) {
  if (!initialized_ || !verify_ || !dump_frames_.count(frame_ + 1) || !pending_targets_valid_) {
    return;
  }
  const uint64_t vs = draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0;
  const uint64_t ps = draw.pixel_shader ? draw.pixel_shader->ucode_data_hash() : 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_used_bits_ & (1u << i))) continue;
    if (Surface* surface = FindSurface(pending_keys_[i].Pack())) {
      VerifyDrawTarget(render_target_cache, *surface, vs, ps, "pre");
    }
  }
  // Verification read the Xenos targets; bind them again for the Xenos draw.
  render_target_cache.RestoreFh1UiOutputTargets();
}

void Fh1NativeExecutor::VerifyDrawTarget(D3D12RenderTargetCache& render_target_cache,
                                         Surface& surface, uint64_t vs, uint64_t ps,
                                         const char* when) {
  if (surface.key.Is64bpp() ||
      (!surface.key.is_depth &&
       !IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(surface.key.format)))) {
    return;
  }
  // Only whole tile rows the surface owns from its base: elsewhere both
  // renderers hold stale words that no guest read can observe.
  const uint32_t packed = surface.key.Pack();
  uint32_t owned = 0;
  while (owned < xenos::kEdramTileCount &&
         tile_owners_[(surface.key.base_tiles + owned) & (xenos::kEdramTileCount - 1)] == packed) {
    ++owned;
  }
  const uint32_t owned_rows = surface.key.pitch_tiles ? owned / surface.key.pitch_tiles : 0;
  const uint32_t tile_height =
      xenos::kEdramTileHeightSamples >>
      uint32_t(surface.key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t width = std::min(surface.width, 1280u);
  const uint32_t height = std::min({surface.height, 720u, owned_rows * tile_height});
  if (!height) return;
  const uint32_t pitch = (width + 31) & ~31u;
  const uint32_t length = pitch * ((height + 31) & ~31u) * 4;
  if (length > kVerifyScratchSize / 2 || verifies_.size() >= 256 || !EnsureVerifyScratch()) {
    if (tracing_) Trace("draw verify skipped: size " + std::to_string(length) + " pending " +
          std::to_string(verifies_.size()));
    return;
  }
  ID3D12Resource* xenos_target = render_target_cache.Fh1PrepareTargetForRead(
      surface.key.Pack(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (!xenos_target) {
    if (tracing_) Trace("draw verify skipped: no xenos target " + surface.key.Describe());
    return;
  }
  Transition(surface.resource.Get(), surface.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(verify_scratch_.Get(), verify_scratch_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_processor_.SubmitBarriers();
  SourceRect source;
  source.surface = &surface;
  source.rect = {0, 0, LONG(width), LONG(height)};
  // Raw EDRAM words of sample 0 of both surfaces, tiled like a 32bpp texture.
  const uint32_t dest_info = 4u | (2u << 16) | (uint32_t(config_.depth_float24_round) << 7) |
                             (uint32_t(config_.gamma_as_unorm16) << 18);
  const D3D12_GPU_VIRTUAL_ADDRESS scratch = verify_scratch_->GetGPUVirtualAddress();
  ResolveToMemory(source, surface.key, 0, dest_info, 0, pitch, surface.resource.Get(), scratch);
  ResolveToMemory(source, surface.key, 0, dest_info, kVerifyScratchSize / 2, pitch, xenos_target,
                  scratch);
  command_processor_.PushUAVBarrier(verify_scratch_.Get());
  Transition(verify_scratch_.Get(), verify_scratch_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  PendingVerify verify;
  verify.length = length;
  verify.start = uint32_t(++draw_verify_index_);
  verify.kind = std::string(when) + " " + std::to_string(draw_verify_index_) + " " +
                surface.key.Describe() + " vs " + std::to_string(vs) + " ps " +
                std::to_string(ps);
  if (!CreateVerifyReadback(verify)) return;
  command_processor_.SubmitBarriers();
  auto& list = command_processor_.GetDeferredCommandList();
  list.D3DCopyBufferRegion(verify.readback.Get(), 0, verify_scratch_.Get(), 0, length);
  list.D3DCopyBufferRegion(verify.readback.Get(), length, verify_scratch_.Get(),
                           kVerifyScratchSize / 2, length);
  verify.submission = command_processor_.GetCurrentSubmission();
  verify.frame = frame_ + 1;
  verify.texels_width = width;
  verify.texels_height = height;
  verify.texels_pitch = pitch;
  verifies_.push_back(std::move(verify));
}

bool Fh1NativeExecutor::BindTargets(uint32_t& bound_bits, uint32_t* formats) {
  bound_bits = 0;
  if (!initialized_) return false;
  CpuTimer timer(*this, kCpuBindTargets);
  if (!pending_targets_valid_) {
    Skip("draw_targets_not_prepared");
    return false;
  }
  const uint32_t used_bits = pending_used_bits_;
  const SurfaceKey* keys = pending_keys_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[xenos::kMaxColorRenderTargets];
  uint32_t rtv_count = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  bool has_dsv = false;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) continue;
    Surface* surface = FindSurface(keys[i].Pack());
    if (!surface) {
      // PrepareTargets could not create it.
      Skip("draw_surface_create");
      return false;
    }
    if (i == 0) {
      Transition(surface->resource.Get(), surface->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      dsv = surface->view;
      has_dsv = true;
    } else {
      while (rtv_count < i - 1) {
        rtvs[rtv_count++] = surface->samples > 1 ? null_rtv_multisample_ : null_rtv_single_;
      }
      Transition(surface->resource.Get(), surface->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      rtvs[rtv_count++] = surface->view;
    }
    formats[i] = keys[i].format;
    bound_bits |= 1u << i;
  }
  command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(
      rtv_count, rtv_count ? rtvs : nullptr, FALSE, has_dsv ? &dsv : nullptr);
  if (!used_bits) Count("draw_writes_nothing");
  return true;
}

void Fh1NativeExecutor::NativeDrawIssued(const Fh1NativeDrawInfo& draw) {
  if (!initialized_) return;
  pending_targets_valid_ = false;
  if (draw.occlusion_query_active) Count("draw_in_occlusion_query");
  ++shadow_draws_;
  if (tracing_) Trace("draw executed");
}

void Fh1NativeExecutor::ShadowDraw(D3D12RenderTargetCache& render_target_cache,
                                   const Fh1NativeDrawInfo& draw) {
  if (!initialized_) return;
  if (draw.memexport) return SkipDraw("draw_memexport", draw);
  if (!pending_targets_valid_) return SkipDraw("draw_targets_not_prepared", draw);
  pending_targets_valid_ = false;
  if (draw.occlusion_query_active) Count("draw_in_occlusion_query");
  const uint32_t used_bits = pending_used_bits_;
  const SurfaceKey* keys = pending_keys_;
  if (!used_bits) {
    // No color write mask and no depth or stencil: the draw changes nothing.
    Count("draw_writes_nothing");
    return;
  }

  // Bind a native surface for every slot the Xenos pipeline was built with,
  // so the render-target formats match the pipeline state. Unused slots have a
  // zero write mask in that pipeline.
  if (!render_target_cache.Fh1CurrentRenderTargetsValid()) return SkipDraw("draw_targets_unbound", draw);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[xenos::kMaxColorRenderTargets];
  uint32_t rtv_count = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  bool has_dsv = false;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    D3D12RenderTargetCache::Fh1BoundTarget xenos_target;
    const bool xenos_bound = render_target_cache.Fh1GetBoundTarget(i, xenos_target);
    const bool used = (used_bits & (1u << i)) != 0;
    if (used && !xenos_bound) {
      Count("draw_target_unbound_by_xenos");
      continue;
    }
    if (!xenos_bound) continue;
    SurfaceKey key = used ? keys[i] : SurfaceKey::Unpack(xenos_target.key);
    if (used && key.Pack() != xenos_target.key) {
      Count("draw_target_key_differs");
      if (ShouldLog()) LogOnce((uint64_t(key.Pack()) << 32) ^ xenos_target.key ^ 0x5A5A,
              "draw target key differs: native " + key.Describe() + " xenos " +
                  SurfaceKey::Unpack(xenos_target.key).Describe());
      // The pipeline expects the Xenos target's format.
      key = SurfaceKey::Unpack(xenos_target.key);
    }
    Surface* surface = GetOrCreateSurface(key);
    if (!surface) return SkipDraw("draw_surface_create", draw);
    if (i == 0) {
      Transition(surface->resource.Get(), surface->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      dsv = surface->view;
      has_dsv = true;
    } else {
      if (rtv_count != i - 1) return SkipDraw("draw_render_target_gap", draw);
      Transition(surface->resource.Get(), surface->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      rtvs[rtv_count++] = surface->view;
    }
  }
  if (!rtv_count && !has_dsv) return SkipDraw("draw_no_bound_targets", draw);
  uint32_t null_textures = 0;
  if (!command_processor_.Fh1BindNativeDrawResources(
          static_cast<const D3D12Shader*>(draw.vertex_shader),
          static_cast<const D3D12Shader*>(draw.pixel_shader), *native_textures_,
          *native_memory_, draw.guest_dma_index_offset, draw.guest_dma_index_size,
          draw.guest_dma_index_format, &null_textures)) {
    return SkipDraw("draw_bindings", draw);
  }
  if (null_textures) {
    // The guest fetches a texture the native cache could not provide.
    Count("draw_null_texture");
    const uint64_t vs = draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0;
    const uint64_t ps = draw.pixel_shader ? draw.pixel_shader->ucode_data_hash() : 0;
    if (verify_ && ShouldLog()) LogOnce(vs ^ (ps << 1) ^ 0x4E554C4C,
            "draw binds " + std::to_string(null_textures) + " null textures: vs " +
                std::to_string(vs) + " ps " + std::to_string(ps));
  }
  command_processor_.SubmitBarriers();
  auto& list = command_processor_.GetDeferredCommandList();
  list.D3DOMSetRenderTargets(rtv_count, rtv_count ? rtvs : nullptr, FALSE,
                             has_dsv ? &dsv : nullptr);
  if (draw.indexed) {
    list.D3DDrawIndexedInstanced(draw.vertex_count, 1, 0, 0, 0);
  } else {
    list.D3DDrawInstanced(draw.vertex_count, 1, 0, 0);
  }
  render_target_cache.Fh1InvalidateCommandListRenderTargets();
  ++shadow_draws_;
  if (tracing_) Trace("draw executed");
  if (verify_ && dump_frames_.count(frame_ + 1) && trace_lines_ < 400) {
    for (uint32_t mask = pending_texture_mask_; mask; mask &= mask - 1) {
      const uint32_t fetch = uint32_t(std::countr_zero(mask));
      if (tracing_) Trace("texture " + std::to_string(fetch) + " native " +
            native_textures_->Fh1DescribeBinding(fetch) + " xenos " +
            command_processor_.Fh1XenosTextureCache().Fh1DescribeBinding(fetch));
    }
  }
  if (verify_ && dump_frames_.count(frame_ + 1)) {
    const uint64_t vs = draw.vertex_shader ? draw.vertex_shader->ucode_data_hash() : 0;
    const uint64_t ps = draw.pixel_shader ? draw.pixel_shader->ucode_data_hash() : 0;
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << i))) continue;
      if (Surface* surface = FindSurface(keys[i].Pack())) {
        VerifyDrawTarget(render_target_cache, *surface, vs, ps);
      }
    }
  }
}

void Fh1NativeExecutor::ClearSurfaceRect(Surface& surface, const D3D12_RECT& rect,
                                         uint32_t clear_value, uint32_t clear_value_lo) {
  auto& list = command_processor_.GetDeferredCommandList();
  if (surface.key.is_depth) {
    // The host keeps float24 depth halved in [0, 1) (RenderTargetCache).
    const uint32_t depth_bits = (clear_value >> 8) & 0xFFFFFF;
    const float value = xenos::DepthRenderTargetFormat(surface.key.format) ==
                                xenos::DepthRenderTargetFormat::kD24FS8
                            ? xenos::Float20e4To32(depth_bits) * 0.5f
                            : xenos::UNorm24To32(depth_bits);
    Transition(surface.resource.Get(), surface.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_processor_.SubmitBarriers();
    list.D3DClearDepthStencilView(surface.view,
                                  D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, value,
                                  uint8_t(clear_value & 0xFF), 1, &rect);
    return;
  }
  float color[4] = {};
  switch (xenos::ColorRenderTargetFormat(surface.key.format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (uint32_t i = 0; i < 4; ++i) color[i] = float((clear_value >> (8 * i)) & 0xFF) / 255.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      for (uint32_t i = 0; i < 3; ++i) {
        color[i] = float((clear_value >> (10 * i)) & 0x3FF) / 1023.0f;
      }
      color[3] = float(clear_value >> 30) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      for (uint32_t i = 0; i < 3; ++i) {
        color[i] = xenos::Float7e3To32((clear_value >> (10 * i)) & 0x3FF);
      }
      color[3] = float(clear_value >> 30) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      std::memcpy(&color[0], &clear_value, sizeof(float));
      break;
    default:
      if (clear_value || clear_value_lo) return Skip("clear_format");
      break;
  }
  Transition(surface.resource.Get(), surface.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_processor_.SubmitBarriers();
  list.D3DClearRenderTargetView(surface.view, color, 1, &rect);
}

bool Fh1NativeExecutor::ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                                        uint32_t sample_select, uint32_t dest_info,
                                        uint32_t dest_base, uint32_t dest_pitch,
                                        ID3D12Resource* source_override,
                                        D3D12_GPU_VIRTUAL_ADDRESS target) {
  Surface& surface = *source.surface;
  const bool depth = surface.key.is_depth;
  const bool msaa = surface.samples > 1;
  ID3D12PipelineState* pipeline = GetResolveMemoryPipeline(depth, msaa);
  if (!pipeline) {
    Skip("resolve_pipeline_create");
    return false;
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair srvs[2];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(depth ? 2 : 1, srvs)) {
    Skip("resolve_descriptor");
    return false;
  }
  if (!depth) srvs[1] = srvs[0];
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  for (uint32_t i = 0; i < (depth ? 2u : 1u); ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = i ? surface.stencil_srv_format : surface.srv_format;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (msaa) {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Texture2D.MipLevels = 1;
      srv_desc.Texture2D.PlaneSlice = i;
    }
    device->CreateShaderResourceView(
        source_override ? source_override : surface.resource.Get(), &srv_desc, srvs[i].first);
  }
  if (!source_override) {
    Transition(surface.resource.Get(), surface.state,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    native_memory_->UseForWriting();
  }
  command_processor_.SubmitBarriers();

  const SurfaceKey& owner = surface.key;
  const uint32_t host_sample_mode =
      owner.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  const D3D12_RECT& rect = source.rect;
  uint32_t constants[kResolveMemoryConstantCount];
  constants[0] = uint32_t(rect.left) | (uint32_t(rect.top) << 16);
  constants[1] = uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
  constants[2] = PackLayout(resolve_key.base_tiles, resolve_key.pitch_tiles, resolve_key.msaa,
                            resolve_key.Is64bpp(), resolve_key.is_depth, resolve_key.format);
  constants[3] = PackLayout(owner.base_tiles, owner.pitch_tiles, owner.msaa, owner.Is64bpp(),
                            owner.is_depth, owner.format) |
                 (host_sample_mode << 27);
  constants[4] = sample_select;
  constants[5] = dest_info;
  constants[6] = dest_base;
  constants[7] = dest_pitch;
  auto& list = command_processor_.GetDeferredCommandList();
  command_processor_.SetExternalPipeline(pipeline);
  list.D3DSetComputeRootSignature(resolve_memory_root_signature_.Get());
  list.D3DSetComputeRoot32BitConstants(0, kResolveMemoryConstantCount, constants, 0);
  list.D3DSetComputeRootDescriptorTable(1, srvs[0].second);
  list.D3DSetComputeRootDescriptorTable(2, srvs[1].second);
  list.D3DSetComputeRootUnorderedAccessView(3, target ? target : native_memory_->GetGPUAddress());
  list.D3DDispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                   (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
  if (!target) native_memory_->MarkUAVWritesCommitNeeded();
  return true;
}



bool Fh1NativeExecutor::PlanCopy(CopyPlan& plan) {
  const RegisterFile& regs = register_file_;
  plan = CopyPlan();
  if (!ResolveRectangle(regs, memory_, plan.x0, plan.y0, plan.x1, plan.y1)) {
    plan.empty = true;
    return false;
  }
  if (!draw_util::GetResolveInfo(regs, memory_, 1, 1, false, false, plan.info)) {
    plan.skip = "resolve_info";
    return false;
  }
  const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  plan.msaa = uint32_t(surface_info.msaa_samples);
  plan.pitch_tiles = PitchTiles(surface_info.surface_pitch, plan.msaa);
  plan.copying_depth = plan.info.IsCopyingDepth();
  plan.color_info =
      plan.copying_depth
          ? reg::RB_COLOR_INFO{}
          : regs.Get<reg::RB_COLOR_INFO>(
                reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
  plan.depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (!plan.info.copy_dest_extent_length) return true;

  const auto dest_info = plan.info.copy_dest_info;
  plan.resolve_key =
      plan.copying_depth
          ? MakeDepthKey(plan.depth_info.depth_base, plan.pitch_tiles, plan.msaa,
                         plan.depth_info.depth_format)
          : MakeColorKey(plan.color_info.color_base, plan.pitch_tiles, plan.msaa,
                         plan.color_info.color_format);
  if (!plan.copying_depth) {
    // Decode in the guest format the resolve names, not the storage key.
    plan.resolve_key.format = uint32_t(plan.color_info.color_format);
  }
  uint32_t pack = UINT32_MAX, bpb_log2 = 2;
  if (plan.copying_depth) {
    pack = 4;
    plan.kind = "depth" + std::to_string(uint32_t(plan.depth_info.depth_format));
  } else {
    switch (xenos::TextureFormat(dest_info.copy_dest_format)) {
      case xenos::TextureFormat::k_8_8_8_8:
        pack = 0;
        break;
      case xenos::TextureFormat::k_2_10_10_10:
        pack = 1;
        break;
      case xenos::TextureFormat::k_32_FLOAT:
        pack = 2;
        break;
      case xenos::TextureFormat::k_16_16_16_16_FLOAT:
        pack = 3;
        bpb_log2 = 3;
        break;
      default:
        break;
    }
    plan.kind = "c" + std::to_string(uint32_t(plan.color_info.color_format)) + "->t" +
                std::to_string(uint32_t(dest_info.copy_dest_format));
  }
  plan.kind += "/" + std::to_string(1u << plan.msaa) + "x/s" +
               std::to_string(uint32_t(plan.info.copy_dest_coordinate_info.copy_sample_select));
  if (pack == UINT32_MAX) {
    plan.skip = "resolve_dest_format";
  } else if (dest_info.copy_dest_array) {
    plan.skip = "resolve_dest_array";
  } else if (uint32_t(dest_info.copy_dest_endian) > 3) {
    plan.skip = "resolve_dest_endian";
  } else if (plan.resolve_key.Is64bpp()) {
    plan.skip = "resolve_64bpp";
  } else if (!plan.copying_depth && !IsResolveColorFormatSupported(plan.color_info.color_format)) {
    plan.skip = "resolve_source_format";
  }
  if (plan.skip) return true;
  const int32_t exp_bias = plan.copying_depth ? 0 : int32_t(dest_info.copy_dest_exp_bias);
  plan.dest_info = pack | (uint32_t(dest_info.copy_dest_endian) << 3) |
                   (uint32_t(!plan.copying_depth && dest_info.copy_dest_swap) << 6) |
                   (uint32_t(config_.depth_float24_round) << 7) |
                   ((uint32_t(exp_bias) & 0xFF) << 8) | (bpb_log2 << 16) |
                   (uint32_t(config_.gamma_as_unorm16) << 18);
  plan.dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  plan.dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch;
  plan.sample_select = uint32_t(plan.info.copy_dest_coordinate_info.copy_sample_select);
  GetResolveSources(plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1, plan.sources);
  plan.copy = true;
  return true;
}

void Fh1NativeExecutor::VerifySurfacesBeforeResolve(D3D12RenderTargetCache& render_target_cache) {
  if (!initialized_ || !verify_) return;
  CopyPlan plan;
  if (!PlanCopy(plan) || !plan.copy) return;
  const uint32_t extent_start = plan.info.copy_dest_extent_start;
  const uint32_t extent_length = plan.info.copy_dest_extent_length;
  if (extent_length > kVerifyScratchSize / 2 || verifies_.size() >= 64) return;
  if (!EnsureVerifyScratch()) return;
  // Both regions start from the Xenos mirror so words outside the rectangle
  // compare equal; the native owners resolve into the first, the Xenos
  // render targets of the same keys into the second.
  D3D12SharedMemory& xenos_memory = command_processor_.Fh1XenosSharedMemory();
  auto& list = command_processor_.GetDeferredCommandList();
  xenos_memory.UseAsCopySource();
  Transition(verify_scratch_.Get(), verify_scratch_state_, D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  const uint32_t half = kVerifyScratchSize / 2;
  list.D3DCopyBufferRegion(verify_scratch_.Get(), 0, xenos_memory.GetBuffer(), extent_start,
                           extent_length);
  list.D3DCopyBufferRegion(verify_scratch_.Get(), half, xenos_memory.GetBuffer(), extent_start,
                           extent_length);
  Transition(verify_scratch_.Get(), verify_scratch_state_,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const D3D12_GPU_VIRTUAL_ADDRESS scratch = verify_scratch_->GetGPUVirtualAddress();
  bool any = false;
  for (const SourceRect& source : plan.sources) {
    if (!source.surface || source.rect.left >= source.rect.right ||
        source.rect.top >= source.rect.bottom) {
      continue;
    }
    ID3D12Resource* xenos_target = render_target_cache.Fh1PrepareTargetForRead(
        source.surface->key.Pack(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!xenos_target) {
      Count("verify_xenos_target_missing");
      continue;
    }
    Transition(source.surface->resource.Get(), source.surface->state,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResolveToMemory(source, plan.resolve_key, plan.sample_select, plan.dest_info,
                    plan.dest_base - extent_start, plan.dest_pitch,
                    source.surface->resource.Get(), scratch);
    ResolveToMemory(source, plan.resolve_key, plan.sample_select, plan.dest_info,
                    plan.dest_base - extent_start + half, plan.dest_pitch, xenos_target, scratch);
    any = true;
  }
  if (!any) return;
  command_processor_.PushUAVBarrier(verify_scratch_.Get());
  Transition(verify_scratch_.Get(), verify_scratch_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  PendingVerify verify;
  verify.start = extent_start;
  verify.length = extent_length;
  verify.kind = "surfaces " + plan.kind;
  if (!CreateVerifyReadback(verify)) return;
  command_processor_.SubmitBarriers();
  list.D3DCopyBufferRegion(verify.readback.Get(), 0, verify_scratch_.Get(), 0, extent_length);
  list.D3DCopyBufferRegion(verify.readback.Get(), extent_length, verify_scratch_.Get(), half,
                           extent_length);
  verify.submission = command_processor_.GetCurrentSubmission();
  verify.frame = frame_ + 1;
  verifies_.push_back(std::move(verify));
}

bool Fh1NativeExecutor::EnsureVerifyScratch() {
  if (verify_scratch_) return true;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = kVerifyScratchSize;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  verify_scratch_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   verify_scratch_state_, nullptr,
                                                   IID_PPV_ARGS(&verify_scratch_)));
}

bool Fh1NativeExecutor::CreateVerifyReadback(PendingVerify& verify) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = uint64_t(verify.length) * 2;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&verify.readback)));
}

void Fh1NativeExecutor::ShadowResolve(D3D12RenderTargetCache& render_target_cache,
                                      bool xenos_succeeded) {
  if (!initialized_) return;
  if (!xenos_succeeded) return Skip("resolve_xenos_failed");
  Resolve(&render_target_cache, nullptr, nullptr);
}

bool Fh1NativeExecutor::NativeResolve(uint32_t& written_address, uint32_t& written_length) {
  written_address = 0;
  written_length = 0;
  if (!initialized_) return false;
  return Resolve(nullptr, &written_address, &written_length);
}

bool Fh1NativeExecutor::Resolve(D3D12RenderTargetCache* render_target_cache,
                                uint32_t* written_address, uint32_t* written_length) {
  CpuTimer timer(*this, kCpuResolves);
  const RegisterFile& regs = register_file_;
  CopyPlan plan;
  if (!PlanCopy(plan)) {
    if (plan.empty) {
      // Nothing to copy or clear: Xenos does no work either.
      Count("resolve_empty");
      return true;
    }
    Skip(plan.skip);
    return false;
  }
  bool succeeded = true;
  const int32_t x0 = plan.x0, y0 = plan.y0, x1 = plan.x1, y1 = plan.y1;
  const D3D12_RECT rect = {x0, y0, x1, y1};
  const uint32_t msaa = plan.msaa;
  const uint32_t pitch_tiles = plan.pitch_tiles;
  const auto& resolve_info = plan.info;
  const auto& color_info = plan.color_info;
  const auto& depth_info = plan.depth_info;

  if (plan.skip) {
    Skip(plan.skip);
    succeeded = false;
  } else if (plan.copy) {
    if (tracing_) Trace("resolve " + plan.kind + " of " + plan.resolve_key.Describe() + " rect " +
          std::to_string(x0) + "," + std::to_string(y0) + "-" + std::to_string(x1) + "," +
          std::to_string(y1) + " to " + std::to_string(plan.dest_base));
    native_memory_->RequestRange(resolve_info.copy_dest_extent_start,
                                 resolve_info.copy_dest_extent_length);
    bool complete = true;
    for (const SourceRect& source : plan.sources) {
      if (source.rect.left >= source.rect.right || source.rect.top >= source.rect.bottom) {
        continue;
      }
      if (!source.surface) {
        // Tiles nobody has drawn or cleared natively, or a surface that could
        // not be created: EDRAM would return stale bits here.
        Skip(source.unowned ? "resolve_tiles_unowned" : "resolve_owner_missing");
        complete = false;
        continue;
      }
      const SurfaceKey& owner = source.surface->key;
      if (owner.Is64bpp()) {
        Skip("resolve_64bpp");
        complete = false;
        continue;
      }
      if (!owner.is_depth &&
          !IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(owner.format))) {
        Skip("resolve_owner_format");
        complete = false;
        continue;
      }
      if (owner.Pack() != (plan.copying_depth
                               ? plan.resolve_key.Pack()
                               : MakeColorKey(color_info.color_base, pitch_tiles, msaa,
                                              color_info.color_format)
                                     .Pack())) {
        Count("resolve_through_alias");
      }
      if (!ResolveToMemory(source, plan.resolve_key, plan.sample_select, plan.dest_info,
                           plan.dest_base, plan.dest_pitch)) {
        complete = false;
      }
    }
    native_memory_->RangeWrittenByGpu(resolve_info.copy_dest_extent_start,
                                      resolve_info.copy_dest_extent_length);
    if (written_address) *written_address = resolve_info.copy_dest_extent_start;
    if (written_length) *written_length = resolve_info.copy_dest_extent_length;
    if (complete) {
      ++shadow_resolves_;
      Count(plan.sources.size() > 1 ? "resolve_multi_owner" : "resolve_single_owner");
    }
    succeeded = complete;
    if (verify_ && render_target_cache) {
      // Owners in both EDRAM models.
      std::vector<uint32_t> native_owners;
      for (const SourceRect& source : plan.sources) {
        const uint32_t owner = source.surface ? source.surface->key.Pack() : kNoOwner;
        if (std::find(native_owners.begin(), native_owners.end(), owner) ==
            native_owners.end()) {
          native_owners.push_back(owner);
        }
      }
      std::vector<uint32_t> xenos_owners = render_target_cache->Fh1LastResolveOwners();
      std::sort(native_owners.begin(), native_owners.end());
      std::sort(xenos_owners.begin(), xenos_owners.end());
      if (native_owners == xenos_owners) {
        Count("verify_owners_agree");
      } else {
        Count("verify_owners_differ");
        std::string text = "resolve " + plan.kind + " owners native [";
        for (uint32_t owner : native_owners) {
          text += (owner == kNoOwner ? std::string("none")
                                     : SurfaceKey::Unpack(owner).Describe()) +
                  " ";
        }
        text += "] xenos [";
        for (uint32_t owner : xenos_owners) text += SurfaceKey::Unpack(owner).Describe() + " ";
        if (ShouldLog()) LogOnce(std::hash<std::string>{}(text), text + "]");
      }
      QueueVerify(resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                  plan.kind);
    }
  }

  // Clears target the surface configured at the original base, like the
  // guest's next pass over it, and take ownership of the cleared tiles.
  const uint32_t msaa_x_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  auto clear = [&](const SurfaceKey& key, uint32_t value, uint32_t value_lo) {
    Surface* surface = GetOrCreateSurface(key);
    if (!surface) return Skip("resolve_clear_surface");
    const uint32_t is_64bpp = key.Is64bpp() ? 1 : 0;
    const uint32_t tile_width = (xenos::kEdramTileWidthSamples >> msaa_x_log2) >> is_64bpp;
    const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
    const uint32_t pitch = key.pitch_tiles << is_64bpp;
    const uint32_t column_first = uint32_t(x0) / tile_width;
    const uint32_t column_end = (uint32_t(x1) + tile_width - 1) / tile_width;
    for (uint32_t row = uint32_t(y0) / tile_height;
         row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
      ClaimTiles(key.base_tiles + row * pitch + column_first, column_end - column_first,
                 key.Pack());
    }
    ClearSurfaceRect(*surface, rect, value, value_lo);
    Count(key.is_depth ? "clear_depth" : "clear_color");
    if (tracing_) Trace("clear " + key.Describe() + " rect " + std::to_string(x0) + "," + std::to_string(y0) +
          "-" + std::to_string(x1) + "," + std::to_string(y1));
  };
  if (resolve_info.IsClearingColor()) {
    clear(MakeColorKey(color_info.color_base, pitch_tiles, msaa, color_info.color_format),
          regs[XE_GPU_REG_RB_COLOR_CLEAR], regs[XE_GPU_REG_RB_COLOR_CLEAR_LO]);
  }
  if (resolve_info.IsClearingDepth()) {
    clear(MakeDepthKey(depth_info.depth_base, pitch_tiles, msaa, depth_info.depth_format),
          regs[XE_GPU_REG_RB_DEPTH_CLEAR], 0);
  }
  return succeeded;
}

void Fh1NativeExecutor::QueueVerify(uint32_t start, uint32_t length, std::string kind) {
  if (!length) return;
  if (verifies_.size() >= 64) {
    // Bounded readback memory: skip rather than stall.
    Count("verify_dropped");
    return;
  }
  PendingVerify verify;
  verify.start = start;
  verify.length = length;
  verify.kind = std::move(kind);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = uint64_t(length) * 2;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&verify.readback)))) {
    return;
  }
  D3D12SharedMemory& xenos_memory = command_processor_.Fh1XenosSharedMemory();
  native_memory_->UseAsCopySource();
  xenos_memory.UseAsCopySource();
  command_processor_.SubmitBarriers();
  auto& list = command_processor_.GetDeferredCommandList();
  list.D3DCopyBufferRegion(verify.readback.Get(), 0, native_memory_->GetBuffer(), start, length);
  list.D3DCopyBufferRegion(verify.readback.Get(), length, xenos_memory.GetBuffer(), start,
                           length);
  verify.submission = command_processor_.GetCurrentSubmission();
  if (verify.kind.rfind("tex", 0) == 0) {
    if (const auto* cpu = static_cast<const uint8_t*>(memory_.TranslatePhysical(start))) {
      verify.cpu.assign(cpu, cpu + length);
    }
  }
  verify.frame = frame_ + 1;
  verifies_.push_back(std::move(verify));
}

void Fh1NativeExecutor::DrainVerifies() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!verifies_.empty() && verifies_.front().submission <= completed) {
    PendingVerify verify = std::move(verifies_.front());
    verifies_.pop_front();
    void* mapped = nullptr;
    if (FAILED(verify.readback->Map(0, nullptr, &mapped))) continue;
    const auto* native = static_cast<const uint8_t*>(mapped);
    const auto* xenos = native + verify.length;
    uint32_t differing_words = 0, max_delta = 0, first_difference = UINT32_MAX;
    auto compare_word = [&](uint32_t i) {
      uint32_t a, b;
      std::memcpy(&a, native + i, 4);
      std::memcpy(&b, xenos + i, 4);
      if (a == b) return;
      ++differing_words;
      if (first_difference == UINT32_MAX) first_difference = i;
      for (uint32_t byte = 0; byte < 4; ++byte) {
        max_delta = std::max<uint32_t>(
            max_delta, uint32_t(std::abs(int32_t((a >> (8 * byte)) & 0xFF) -
                                         int32_t((b >> (8 * byte)) & 0xFF))));
      }
    };
    if (verify.texels_height) {
      for (uint32_t y = 0; y < verify.texels_height; ++y) {
        for (uint32_t x = 0; x < verify.texels_width; ++x) {
          const int32_t offset =
              texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), verify.texels_pitch, 2);
          if (offset >= 0 && uint32_t(offset) + 4 <= verify.length) compare_word(uint32_t(offset));
        }
      }
    } else {
      for (uint32_t i = 0; i + 4 <= verify.length; i += 4) compare_word(i);
    }
    if (verify.cpu.size() == verify.length) {
      // Guest-memory textures: compare each mirror with the CPU bytes.
      auto& cpu_counts = verify_cpu_counts_[verify.kind.substr(0, verify.kind.find(' '))];
      ++cpu_counts[2];
      cpu_counts[0] += std::memcmp(native, verify.cpu.data(), verify.length) == 0;
      cpu_counts[1] += std::memcmp(xenos, verify.cpu.data(), verify.length) == 0;
    }
    // "pre N <surface> ..." / "draw N <surface> ...": the surface token.
    std::string surface_token;
    if (verify.texels_height) {
      const size_t first_space = verify.kind.find(' ');
      const size_t second_space = verify.kind.find(' ', first_space + 1);
      const size_t third_space = verify.kind.find(' ', second_space + 1);
      if (third_space != std::string::npos) {
        surface_token = verify.kind.substr(second_space + 1, third_space - second_space - 1);
      }
      if (verify.kind.rfind("pre ", 0) == 0 && !surface_token.empty()) {
        last_pre_native_[surface_token].assign(native, native + verify.length);
      }
    }
    auto& counts = verify_counts_[verify.kind];
    if (!differing_words) {
      ++counts.first;
    } else {
      ++counts.second;
      if (counts.second <= 2 || dump_frames_.count(verify.frame)) {
        uint32_t a = 0, b = 0;
        std::memcpy(&a, native + first_difference, 4);
        std::memcpy(&b, xenos + first_difference, 4);
        uint32_t native_cpu = 0, xenos_cpu = 0, c = 0;
        if (verify.cpu.size() == verify.length) {
          std::memcpy(&c, verify.cpu.data() + first_difference, 4);
          for (uint32_t i = 0; i + 4 <= verify.length; i += 4) {
            native_cpu += std::memcmp(native + i, verify.cpu.data() + i, 4) != 0;
            xenos_cpu += std::memcmp(xenos + i, verify.cpu.data() + i, 4) != 0;
          }
        }
        uint32_t pre = 0;
        if (verify.kind.rfind("draw ", 0) == 0) {
          auto pre_it = last_pre_native_.find(surface_token);
          if (pre_it != last_pre_native_.end() && first_difference + 4 <= pre_it->second.size()) {
            std::memcpy(&pre, pre_it->second.data() + first_difference, 4);
          }
        }
        REXGPU_INFO(
            "FH1 native executor verify frame {} {} at {:08X} differs: {} of {} words, max byte "
            "delta {}, first at +{:X}: native {:08X} xenos {:08X} cpu {:08X} pre {:08X}; words "
            "differing from cpu: native {} xenos {}",
            verify.frame, verify.kind, verify.start, differing_words, verify.length / 4, max_delta,
            first_difference, a, b, c, pre, native_cpu, xenos_cpu);
      }
    }
    verify.readback->Unmap(0, nullptr);
  }
}

void Fh1NativeExecutor::ShadowSwap(uint64_t frame, uint32_t frontbuffer_address,
                                   uint32_t width, uint32_t height, const uint32_t* gamma_pwl) {
  if (!initialized_) return;
  if (tracing_) Trace("swap");
  frame_ = frame;
  tracing_ = verify_ && dump_frames_.count(frame_ + 1) != 0;
  ++cpu_frames_;
  trace_lines_ = 0;
  DrainDumps();
  DrainVerifies();
  if (dump_frames_.count(frame) && !dump_directory_.empty()) {
    QueueFrontBufferDump(*native_textures_, "native", frame, width, height, gamma_pwl);
    if (verify_) {
      // What Xenos presents this swap, for the same-frame comparison.
      QueueFrontBufferDump(command_processor_.Fh1XenosTextureCache(), "xenos", frame, width,
                           height, gamma_pwl);
    }
  }
  (void)frontbuffer_address;
  if (frame % 600 == 0) LogStats(frame);
}

void Fh1NativeExecutor::QueueFrontBufferDump(D3D12TextureCache& textures, const char* prefix,
                                             uint64_t frame, uint32_t width, uint32_t height,
                                             const uint32_t* gamma_pwl) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  xenos::TextureFormat format;
  uint32_t texture_width = 0, texture_height = 0;
  ID3D12Resource* front =
      textures.RequestSwapTexture(srv_desc, format, &texture_width, &texture_height,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, nullptr);
  if (!front) {
    Skip("dump_front_buffer_missing");
    return;
  }
  PendingDump dump;
  dump.frame = frame;
  dump.prefix = prefix;
  dump.component_mapping = srv_desc.Shader4ComponentMapping;
  if (gamma_pwl) {
    dump.has_gamma_pwl = true;
    std::memcpy(dump.gamma_pwl.data(), gamma_pwl, sizeof(dump.gamma_pwl));
  }
  dump.format = srv_desc.Format;
  dump.width = std::min(width, texture_width);
  dump.height = std::min(height, texture_height);
  D3D12_RESOURCE_DESC desc = front->GetDesc();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &dump.footprint, nullptr, nullptr, &total);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&dump.readback)))) {
    return;
  }
  command_processor_.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
  from.pResource = front;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = dump.readback.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = dump.footprint;
  command_processor_.GetDeferredCommandList().D3DCopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  dump.submission = command_processor_.GetCurrentSubmission();
  dumps_.push_back(std::move(dump));
}

void Fh1NativeExecutor::DrainDumps() {
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!dumps_.empty() && dumps_.front().submission <= completed) {
    PendingDump dump = std::move(dumps_.front());
    dumps_.pop_front();
    void* mapped = nullptr;
    if (FAILED(dump.readback->Map(0, nullptr, &mapped))) continue;
    std::error_code error;
    std::filesystem::create_directories(dump_directory_, error);
    std::ofstream file(
        dump_directory_ / (dump.prefix + "-" + std::to_string(dump.frame) + ".ppm"),
                       std::ios::binary | std::ios::trunc);
    file << "P6\n" << dump.width << " " << dump.height << "\n255\n";
    const auto* bytes = static_cast<const uint8_t*>(mapped) + dump.footprint.Offset;
    const bool wide = dump.format == DXGI_FORMAT_R16G16B16A16_UNORM;
    for (uint32_t y = 0; y < dump.height; ++y) {
      const uint8_t* row = bytes + y * dump.footprint.Footprint.RowPitch;
      for (uint32_t x = 0; x < dump.width; ++x) {
        // 10-bit channels; mapping values 4 and 5 are constant 0 and 1.
        uint32_t channels[6] = {0, 0, 0, 1023, 0, 1023};
        if (wide) {
          uint16_t values[4];
          std::memcpy(values, row + x * 8, sizeof(values));
          for (uint32_t c = 0; c < 4; ++c) channels[c] = (values[c] * 1023u + 32767u) / 65535u;
        } else {
          uint32_t pixel;
          std::memcpy(&pixel, row + x * 4, sizeof(pixel));
          if (dump.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            for (uint32_t c = 0; c < 3; ++c) channels[c] = (pixel >> (10 * c)) & 1023;
            channels[3] = ((pixel >> 30) & 3) * 341;
          } else {
            for (uint32_t c = 0; c < 4; ++c) {
              channels[c] = (((pixel >> (8 * c)) & 0xFF) * 1023 + 127) / 255;
            }
          }
        }
        char rgb[3];
        for (uint32_t c = 0; c < 3; ++c) {
          const uint32_t value =
              channels[std::min((dump.component_mapping >> (3 * c)) & 7u, 5u)];
          if (dump.has_gamma_pwl) {
            // output = base + multiplier * delta / 2^3 in 16-bit fixed point.
            const uint32_t entry = dump.gamma_pwl[(value >> 3) * 3 + c];
            const uint32_t output =
                std::min((entry & 0xFFFF) + (((entry >> 16) * (value & 7)) >> 3), 0xFFFFu);
            rgb[c] = char((output * 255 + 32767) / 65535);
          } else {
            rgb[c] = char((value * 255 + 511) / 1023);
          }
        }
        file.write(rgb, 3);
      }
    }
    dump.readback->Unmap(0, nullptr);
  }
}

void Fh1NativeExecutor::LogStats(uint64_t frame) {
  std::string skips;
  for (const auto& [reason, count] : skips_) {
    skips += (skips.empty() ? "" : ",") + reason + "=" + std::to_string(count);
  }
  std::string stats;
  for (const auto& [name, count] : stats_) {
    stats += (stats.empty() ? "" : ",") + name + "=" + std::to_string(count);
  }
  REXGPU_INFO(
      "FH1 native executor frame={} draws={} resolves={} surfaces={} skips={{{}}} stats={{{}}}",
      frame, shadow_draws_, shadow_resolves_, surfaces_.size(), skips, stats);
  if (cpu_frames_) {
    // Transfers run inside target preparation and resolve clears.
    const double frames = double(cpu_frames_);
    REXGPU_INFO(
        "FH1 native executor cpu ms/frame over {} frames: prepare_targets {:.3f} (transfers "
        "{:.3f}) bind_targets {:.3f} resolves {:.3f}",
        cpu_frames_, cpu_ns_[kCpuPrepareTargets] / frames / 1e6,
        cpu_ns_[kCpuTransfers] / frames / 1e6, cpu_ns_[kCpuBindTargets] / frames / 1e6,
        cpu_ns_[kCpuResolves] / frames / 1e6);
    cpu_ns_ = {};
    cpu_frames_ = 0;
  }
  if (!verify_counts_.empty()) {
    std::string verified;
    for (const auto& [kind, counts] : verify_counts_) {
      verified += (verified.empty() ? "" : ",") + kind + "=" + std::to_string(counts.first) +
                  "/" + std::to_string(counts.first + counts.second);
    }
    REXGPU_INFO("FH1 native executor verify equal/total {{{}}}", verified);
  }
  if (!verify_cpu_counts_.empty()) {
    std::string verified;
    for (const auto& [kind, counts] : verify_cpu_counts_) {
      verified += (verified.empty() ? "" : ",") + kind + "=" + std::to_string(counts[0]) + "/" +
                  std::to_string(counts[1]) + "/" + std::to_string(counts[2]);
    }
    REXGPU_INFO("FH1 native executor verify cpu-equal native/xenos/total {{{}}}", verified);
  }
}

}  // namespace rex::graphics::d3d12
