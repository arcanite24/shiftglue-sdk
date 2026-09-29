#include <rex/graphics/d3d12/fh1_native_executor.h>

#include <algorithm>

#include <fmt/format.h>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/host_render_config.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/flags.h>
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

REXCVAR_DEFINE_STRING(fh1_native_dump_frames, "", "GPU/D3D12",
                      "Comma-separated frames whose native front buffer is written as PPM")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_native_dump_dir, "", "GPU/D3D12",
                      "Directory for native front-buffer PPM dumps")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_readback_new_resolves, true, "GPU/D3D12",
                    "With readback_resolve = none, still read back to guest memory, waiting for "
                    "the GPU, resolves to ranges no resolve wrote in the last few frames: one-off "
                    "captures the game reads on the CPU, such as the car thumbnails it saves. "
                    "Ranges resolved every frame are not read back")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fh1_native_gpu_profile, false, "GPU/D3D12",
                    "Measure the FH1 native executor's GPU time per phase (transfers, resolves, "
                    "clears) with timestamp queries, and its CPU time per phase (target "
                    "preparation and binding, transfers, resolves), and report them with the "
                    "periodic stats")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_color_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_color_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_depth_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_depth_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_uint_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_resolve_memory_uint_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_color_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_uint_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_color_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_color_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_depth_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_depth_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_uint_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_words_uint_ms_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_from_words_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_depth_dms_from_words_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_from_words_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fh1_native_transfer_stencil_dms_from_words_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
}  // namespace shaders

namespace {

constexpr uint32_t kRtvCapacity = 1024;
constexpr uint32_t kDsvCapacity = 256;
constexpr uint32_t kResolveMemoryConstantCount = 8;

// Formats whose guest EDRAM words are the host channel bits (16-bit or 32-bit
// float channels besides 32_FLOAT): read and transferred through UINT views.
DXGI_FORMAT ColorUintFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

// IEEE half to float, for 16-bit float clear values.
float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half & 0x8000) << 16;
  uint32_t exponent = (half >> 10) & 0x1F;
  uint32_t mantissa = half & 0x3FF;
  uint32_t bits;
  if (!exponent) {
    if (!mantissa) {
      bits = sign;
    } else {
      // Denormal: normalize.
      exponent = 113;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        --exponent;
      }
      bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
    }
  } else if (exponent == 0x1F) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Source variant of a surface: 0 color, 1 depth and stencil, 2 raw color bits.
uint32_t SourceKind(bool is_depth, uint32_t format) {
  if (is_depth) return 1;
  return ColorUintFormat(xenos::ColorRenderTargetFormat(format)) != DXGI_FORMAT_UNKNOWN ? 2 : 0;
}

// [dest kind: color, depth, stencil bit, uint][dest msaa]
// [source kind: color, depth, uint][source msaa]
const D3D12_SHADER_BYTECODE kTransferShaders[4][2][3][2] = {
    {
        {
            {{shaders::fh1_native_transfer_color_from_color_ps, sizeof(shaders::fh1_native_transfer_color_from_color_ps)}, {shaders::fh1_native_transfer_color_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_color_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_color_from_depth_ps, sizeof(shaders::fh1_native_transfer_color_from_depth_ps)}, {shaders::fh1_native_transfer_color_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_color_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_color_from_uint_ps, sizeof(shaders::fh1_native_transfer_color_from_uint_ps)}, {shaders::fh1_native_transfer_color_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_color_from_uint_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_color_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_color_ps)}, {shaders::fh1_native_transfer_color_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_color_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_depth_ps)}, {shaders::fh1_native_transfer_color_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_color_dms_from_uint_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_uint_ps)}, {shaders::fh1_native_transfer_color_dms_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_color_dms_from_uint_ms_ps)}},
        },
    },
    {
        {
            {{shaders::fh1_native_transfer_depth_from_color_ps, sizeof(shaders::fh1_native_transfer_depth_from_color_ps)}, {shaders::fh1_native_transfer_depth_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_depth_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_from_depth_ps, sizeof(shaders::fh1_native_transfer_depth_from_depth_ps)}, {shaders::fh1_native_transfer_depth_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_depth_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_from_uint_ps, sizeof(shaders::fh1_native_transfer_depth_from_uint_ps)}, {shaders::fh1_native_transfer_depth_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_depth_from_uint_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_depth_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_color_ps)}, {shaders::fh1_native_transfer_depth_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_depth_ps)}, {shaders::fh1_native_transfer_depth_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_depth_dms_from_uint_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_uint_ps)}, {shaders::fh1_native_transfer_depth_dms_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_depth_dms_from_uint_ms_ps)}},
        },
    },
    {
        {
            {{shaders::fh1_native_transfer_stencil_from_color_ps, sizeof(shaders::fh1_native_transfer_stencil_from_color_ps)}, {shaders::fh1_native_transfer_stencil_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_from_depth_ps, sizeof(shaders::fh1_native_transfer_stencil_from_depth_ps)}, {shaders::fh1_native_transfer_stencil_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_from_uint_ps, sizeof(shaders::fh1_native_transfer_stencil_from_uint_ps)}, {shaders::fh1_native_transfer_stencil_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_from_uint_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_stencil_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_color_ps)}, {shaders::fh1_native_transfer_stencil_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_depth_ps)}, {shaders::fh1_native_transfer_stencil_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_stencil_dms_from_uint_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_uint_ps)}, {shaders::fh1_native_transfer_stencil_dms_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_stencil_dms_from_uint_ms_ps)}},
        },
    },
    {
        {
            {{shaders::fh1_native_transfer_uint_from_color_ps, sizeof(shaders::fh1_native_transfer_uint_from_color_ps)}, {shaders::fh1_native_transfer_uint_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_uint_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_uint_from_depth_ps, sizeof(shaders::fh1_native_transfer_uint_from_depth_ps)}, {shaders::fh1_native_transfer_uint_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_uint_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_uint_from_uint_ps, sizeof(shaders::fh1_native_transfer_uint_from_uint_ps)}, {shaders::fh1_native_transfer_uint_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_uint_from_uint_ms_ps)}},
        },
        {
            {{shaders::fh1_native_transfer_uint_dms_from_color_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_color_ps)}, {shaders::fh1_native_transfer_uint_dms_from_color_ms_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_color_ms_ps)}},
            {{shaders::fh1_native_transfer_uint_dms_from_depth_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_depth_ps)}, {shaders::fh1_native_transfer_uint_dms_from_depth_ms_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_depth_ms_ps)}},
            {{shaders::fh1_native_transfer_uint_dms_from_uint_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_uint_ps)}, {shaders::fh1_native_transfer_uint_dms_from_uint_ms_ps, sizeof(shaders::fh1_native_transfer_uint_dms_from_uint_ms_ps)}},
        },
    },
};

uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

}  // namespace

Fh1NativeExecutor::Fh1NativeExecutor(D3D12CommandProcessor& command_processor,
                                     const RegisterFile& register_file, memory::Memory& memory)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      draw_extent_estimator_(register_file, memory),
      depth_overwrite_(register_file, memory) {}

Fh1NativeExecutor::~Fh1NativeExecutor() { Shutdown(); }

// Two clock reads per timed call, and target preparation and binding run for
// every draw: about 3 % of the race's GPU commands thread (NP-9.3), so the
// timers only run with fh1_native_gpu_profile.
Fh1NativeExecutor::CpuTimer::CpuTimer(Fh1NativeExecutor& executor, CpuPhase phase)
    : executor_(executor),
      phase_(phase),
      start_(executor.cpu_timing_ ? std::chrono::steady_clock::now().time_since_epoch().count()
                                  : 0) {}

Fh1NativeExecutor::CpuTimer::~CpuTimer() {
  if (!executor_.cpu_timing_) {
    return;
  }
  executor_.cpu_ns_[phase_] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::duration(
          std::chrono::steady_clock::now().time_since_epoch().count() - start_)).count());
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

  native_memory_ = config_.memory;
  native_textures_ = config_.textures;
  if (!native_memory_ || !native_textures_) return false;

  tiles_.Reset();
  scale_ = native_textures_->draw_resolution_scale_x();
  if (native_textures_->draw_resolution_scale_y() != scale_ || scale_ > 4) {
    REXGPU_ERROR("FH1 native executor: unsupported resolution scale {}x{}",
                 native_textures_->draw_resolution_scale_x(),
                 native_textures_->draw_resolution_scale_y());
    return false;
  }
  // Frame dumps record and compare the 1x guest-memory mirror.
  if (const uint64_t dump_frame = scale_ == 1 ? Fh1FrameDump::RequestedFrame() : 0) {
    frame_dump_ = std::make_unique<Fh1FrameDump>(command_processor_, register_file_, memory_,
                                                 *native_memory_, dump_frame,
                                                 Fh1FrameDump::RequestedPath());
  }

  std::stringstream frames(REXCVAR_GET(fh1_native_dump_frames));
  std::string frame;
  while (std::getline(frames, frame, ',')) {
    if (!frame.empty()) dump_frames_.insert(std::strtoull(frame.c_str(), nullptr, 10));
  }
  dump_directory_ = std::filesystem::path(REXCVAR_GET(fh1_native_dump_dir));
  cpu_timing_ = REXCVAR_GET(fh1_native_gpu_profile);
  if (REXCVAR_GET(fh1_native_gpu_profile)) {
    D3D12_QUERY_HEAP_DESC query_desc = {};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_desc.Count = kGpuProfileSlots * kGpuProfileQueries;
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = kGpuProfileQueries * sizeof(uint64_t);
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&gpu_query_heap_))) &&
              SUCCEEDED(provider.GetDirectQueue()->GetTimestampFrequency(
                  &gpu_timestamp_frequency_));
    for (auto& slot : gpu_slots_) {
      ok = ok && SUCCEEDED(device->CreateCommittedResource(
                     &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback)));
    }
    if (!ok) gpu_query_heap_.Reset();
  }
  initialized_ = true;
  REXGPU_INFO("FH1 native executor enabled: native ({} dump frames)", dump_frames_.size());
  return true;
}

void Fh1NativeExecutor::Shutdown() {
  if (initialized_) LogStats(frame_);
  initialized_ = false;
  dumps_.clear();
  surfaces_.clear();
  native_textures_ = nullptr;
  native_memory_ = nullptr;
  for (auto& by_depth : resolve_memory_pipelines_) {
    for (auto& pipeline : by_depth) pipeline.Reset();
  }
  resolve_memory_root_signature_.Reset();
  transfer_pipelines_.clear();
  transfer_root_signature_.Reset();
  rtv_heap_.Reset();
  dsv_heap_.Reset();
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
  return Fh1PitchTiles(pitch_pixels, msaa);
}

uint32_t Fh1NativeExecutor::SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const {
  return Fh1SurfaceHeight(pitch_tiles, msaa, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, scale_);
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeColorKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
    xenos::ColorRenderTargetFormat format) const {
  return SurfaceKey::Color(base, pitch_tiles, msaa, format, config_.gamma_as_unorm16);
}

Fh1NativeExecutor::SurfaceKey Fh1NativeExecutor::MakeDepthKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa, xenos::DepthRenderTargetFormat format) {
  return SurfaceKey::Depth(base, pitch_tiles, msaa, format);
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
  desc.Width = surface.width * scale_;
  desc.Height = surface.height * scale_;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = surface.samples;
  D3D12_CLEAR_VALUE clear_value = {};
  if (key.is_depth) {
    const auto format = xenos::DepthRenderTargetFormat(key.format);
    desc.Format = D3D12HostRenderConfig::GetDepthResourceDXGIFormat(format);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    surface.view_format = D3D12HostRenderConfig::GetDepthDSVDXGIFormat(format);
    surface.srv_format = D3D12HostRenderConfig::GetDepthSRVDepthDXGIFormat(format);
    surface.stencil_srv_format = D3D12HostRenderConfig::GetDepthSRVStencilDXGIFormat(format);
    surface.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    clear_value.Format = surface.view_format;
    clear_value.DepthStencil.Depth = format == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f : 0.0f;
  } else {
    const auto format = xenos::ColorRenderTargetFormat(key.format);
    desc.Format = ColorResourceFormat(format);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    surface.view_format = ColorDrawFormat(format);
    const DXGI_FORMAT uint_format = ColorUintFormat(format);
    surface.srv_format = uint_format != DXGI_FORMAT_UNKNOWN ? uint_format : surface.view_format;
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
    if (surface.srv_format != surface.view_format && rtv_used_ < kRtvCapacity) {
      // Transfers write raw channel bits.
      surface.uint_view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
      surface.uint_view.ptr += SIZE_T(rtv_used_++) * rtv_size_;
      rtv.Format = surface.srv_format;
      device->CreateRenderTargetView(surface.resource.Get(), &rtv, surface.uint_view);
    }
  }
  // Render targets must be initialized before use; zero is also what the
  // stencil tracking assumes for a new surface.
  auto& list = command_processor_.GetDeferredCommandList();
  if (key.is_depth) {
    list.D3DClearDepthStencilView(surface.view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                  0.0f, 0, 0, nullptr);
  } else {
    const float zero[4] = {};
    list.D3DClearRenderTargetView(surface.view, zero, 0, nullptr);
  }
  Count("surface_created");
  return &surfaces_.emplace(packed, std::move(surface)).first->second;
}

void Fh1NativeExecutor::ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key,
                                   bool transfer) {
  const auto runs = tiles_.Claim(base, length, packed_key, transfer);
  if (runs.empty()) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  for (const TileRun& run : runs) TransferTiles(*dest, run);
}

void Fh1NativeExecutor::MarkTileStencil(uint32_t base, uint32_t length, bool nonzero) {
  tiles_.MarkStencil(base, length, nonzero);
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
  const uint32_t kind = key.dest_kind == kTransferDestUint ? 3 : std::min(key.dest_kind, 2u);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = transfer_root_signature_.Get();
  desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
  desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
  static const D3D12_SHADER_BYTECODE kFromWordsShaders[2][2] = {
      {{shaders::fh1_native_transfer_depth_from_words_ps,
        sizeof(shaders::fh1_native_transfer_depth_from_words_ps)},
       {shaders::fh1_native_transfer_depth_dms_from_words_ps,
        sizeof(shaders::fh1_native_transfer_depth_dms_from_words_ps)}},
      {{shaders::fh1_native_transfer_stencil_from_words_ps,
        sizeof(shaders::fh1_native_transfer_stencil_from_words_ps)},
       {shaders::fh1_native_transfer_stencil_dms_from_words_ps,
        sizeof(shaders::fh1_native_transfer_stencil_dms_from_words_ps)}},
  };
  desc.PS = key.source_kind == kTransferSourceWords
                ? kFromWordsShaders[kind == 1 ? 0 : 1][key.dest_samples > 1]
                : kTransferShaders[kind][key.dest_samples > 1][key.source_kind][key.source_msaa];
  desc.SampleMask = key.sample_mask;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = FALSE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.SampleDesc.Count = key.dest_samples;
  if (kind == 0 || kind == 3) {
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
  // The run's tiles as at most three rectangles of the destination: the rest
  // of the first tile row, whole rows, and the start of the last row.
  const uint32_t msaa_x_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(dest.key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t dest_64bpp = dest.key.Is64bpp() ? 1 : 0;
  const uint32_t tile_width = (xenos::kEdramTileWidthSamples >> msaa_x_log2) >> dest_64bpp;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t pitch = dest.key.pitch_tiles << dest_64bpp;
  const uint32_t first = (run.first - dest.key.base_tiles) & (xenos::kEdramTileCount - 1);
  const uint32_t end = first + run.count;
  D3D12_RECT rects[3];
  uint32_t rect_count = 0;
  auto add_rect = [&](uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    D3D12_RECT rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
                       LONG(std::min(column_end * tile_width, dest.width)),
                       LONG(std::min(row_end * tile_height, dest.height))};
    if (rect.left < rect.right && rect.top < rect.bottom) rects[rect_count++] = HostRect(rect);
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
  if (rect_count) {
    TransferRects(dest, run.previous_owner, rects, rect_count, run.count,
                  AnyTileStencil(run.first, run.count));
  }
}

bool Fh1NativeExecutor::AnyTileStencil(uint32_t base, uint32_t count) const {
  return tiles_.AnyStencil(base, count);
}

void Fh1NativeExecutor::ClaimTileRect(const SurfaceKey& key, uint32_t column_first,
                                      uint32_t row_first, uint32_t column_end,
                                      uint32_t row_end) {
  const uint32_t packed_key = key.Pack();
  const auto claim = tiles_.ClaimRect(key.base_tiles, key.pitch_tiles, packed_key, column_first,
                                      row_first, column_end, row_end);
  if (claim.per_row) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      ClaimTiles(key.base_tiles + row * key.pitch_tiles + column_first,
                 column_end - column_first, packed_key);
    }
    return;
  }
  const uint32_t previous_owner = claim.previous_owner;
  if (previous_owner == kNoOwner) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const D3D12_RECT rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
                           LONG(std::min(column_end * tile_width, dest->width)),
                           LONG(std::min(row_end * tile_height, dest->height))};
  if (rect.left < rect.right && rect.top < rect.bottom) {
    bool tiles_stencil = false;
    for (uint32_t row = row_first; row < row_end && !tiles_stencil; ++row) {
      tiles_stencil = AnyTileStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                                     column_end - column_first);
    }
    const D3D12_RECT host_rect = HostRect(rect);
    TransferRects(*dest, previous_owner, &host_rect, 1,
                  (column_end - column_first) * (row_end - row_first), tiles_stencil);
  }
}

void Fh1NativeExecutor::TransferRects(Surface& dest, uint32_t previous_owner,
                                      const D3D12_RECT* rects, uint32_t rect_count,
                                      uint32_t tile_count, bool tiles_stencil) {
  CpuTimer timer(*this, kCpuTransfers);
  GpuTimer gpu_timer(*this, kGpuTransfers);
  Surface* source = FindSurface(previous_owner);
  if (!source) return Skip("transfer_source_missing");
  if ((!dest.key.is_depth &&
       !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(dest.key.format))) ||
      (!source->key.is_depth &&
       !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(source->key.format)))) {
    return Skip("transfer_format");
  }
  Count("transfer");
  // The depth pass resets stencil to 0; per-bit passes are needed only when
  // the source's stencil (or a color word's low byte) may be nonzero in the
  // transferred tiles.
  const bool source_stencil =
      source->key.is_depth ? source->stencil_nonzero && tiles_stencil : true;
  uint32_t pass_count = 1;
  if (dest.key.is_depth) {
    // Depth destinations take up to nine passes from the EDRAM words, which
    // are computed once per destination sample.
    if (!EnsureTransferWords(dest)) return Skip("transfer_words_buffer");
    dest.stencil_nonzero |= source_stencil;
    pass_count = source_stencil ? 9 : 1;
    if (!source_stencil) Count("transfer_stencil_skipped");
  }
  counters_.Count("transfer_tile_passes", uint64_t(tile_count) * pass_count);
  if (gpu_query_heap_) {
    transfer_volume_[source->key.Describe() + "->" + dest.key.Describe()] +=
        uint64_t(tile_count) * pass_count;
  }
  for (uint32_t i = 0; i < rect_count; ++i) {
    pending_transfers_.push_back({dest.key.Pack(), previous_owner, rects[i], source_stencil});
  }
}

void Fh1NativeExecutor::FlushTransfers() {
  if (pending_transfers_.empty()) return;
  CpuTimer timer(*this, kCpuTransfers);
  GpuTimer gpu_timer(*this, kGpuTransfers);
  // Grouped by destination, then by source, keeping each group's order.
  std::stable_sort(pending_transfers_.begin(), pending_transfers_.end(),
                   [](const PendingTransfer& a, const PendingTransfer& b) {
                     return a.dest != b.dest ? a.dest < b.dest : a.source < b.source;
                   });
  const uint32_t flags = TransferFlags();
  auto& list = command_processor_.GetDeferredCommandList();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  for (size_t group = 0; group < pending_transfers_.size();) {
    size_t group_end = group;
    while (group_end < pending_transfers_.size() &&
           pending_transfers_[group_end].dest == pending_transfers_[group].dest) {
      ++group_end;
    }
    Surface* dest = FindSurface(pending_transfers_[group].dest);
    if (dest && !dest->key.is_depth) {
      FlushColorTransfers(*dest, group, group_end);
      group = group_end;
      continue;
    }
    if (!dest || !EnsureTransferWords(*dest)) {
      Skip("transfer_words_buffer");
      group = group_end;
      continue;
    }
    // Words of every rectangle, one source at a time.
    for (size_t i = group; i < group_end; ++i) {
      Surface* source = FindSurface(pending_transfers_[i].source);
      if (source) {
        Transition(source->resource.Get(), source->state,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
    }
    Transition(transfer_words_.Get(), transfer_words_state_,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_processor_.SubmitBarriers();
    list.D3DSetComputeRootSignature(resolve_memory_root_signature_.Get());
    list.D3DSetComputeRootUnorderedAccessView(3, transfer_words_->GetGPUVirtualAddress());
    bool any_stencil = false;
    for (size_t i = group; i < group_end; ++i) any_stencil |= pending_transfers_[i].stencil;
    for (size_t i = group; i < group_end;) {
      size_t source_end = i;
      while (source_end < group_end &&
             pending_transfers_[source_end].source == pending_transfers_[i].source) {
        ++source_end;
      }
      Surface* source = FindSurface(pending_transfers_[i].source);
      if (!source) {
        i = source_end;
        continue;
      }

      const bool source_depth = source->key.is_depth;
      ID3D12PipelineState* words_pipeline = GetTransferWordsPipeline(
          SourceKind(source_depth, source->key.format), source->samples > 1);
      ui::d3d12::util::DescriptorCpuGpuHandlePair srvs[2];
      if (!words_pipeline || !CreateTransferSourceViews(*source, srvs)) {
        Skip("transfer_words_pipeline");
        i = source_end;
        continue;
      }
      command_processor_.SetExternalPipeline(words_pipeline);
      list.D3DSetComputeRootSignature(resolve_memory_root_signature_.Get());
      list.D3DSetComputeRootDescriptorTable(1, srvs[0].second);
      list.D3DSetComputeRootDescriptorTable(2, srvs[1].second);
      list.D3DSetComputeRootUnorderedAccessView(3, transfer_words_->GetGPUVirtualAddress());
      for (; i < source_end; ++i) {
        const D3D12_RECT& rect = pending_transfers_[i].rect;
        const uint32_t words_constants[kResolveMemoryConstantCount] = {
            uint32_t(rect.left) | (uint32_t(rect.top) << 16),
            uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16),
            LayoutConstant(*dest), LayoutConstant(*source), flags, dest->width * scale_,
            dest->samples, 0};
        list.D3DSetComputeRoot32BitConstants(0, kResolveMemoryConstantCount, words_constants, 0);
        list.D3DDispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                         (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
      }
    }
    command_processor_.PushUAVBarrier(transfer_words_.Get());
    Transition(transfer_words_.Get(), transfer_words_state_,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dest->resource.Get(), dest->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_processor_.SubmitBarriers();
    ui::d3d12::util::DescriptorCpuGpuHandlePair words_srv;
    if (!command_processor_.RequestOneUseSingleViewDescriptors(1, &words_srv)) {
      Skip("transfer_descriptor");
      group = group_end;
      continue;
    }
    ui::d3d12::util::CreateBufferRawSRV(device, words_srv.first, transfer_words_.Get(),
                                        transfer_words_size_);
    command_processor_.SetExternalGraphicsRootSignature(transfer_root_signature_.Get());
    list.D3DSetGraphicsRootDescriptorTable(1, words_srv.second);
    list.D3DSetGraphicsRootDescriptorTable(2, words_srv.second);
    command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.D3DOMSetRenderTargets(0, nullptr, FALSE, &dest->view);
    const uint32_t sample_mask =
        dest->key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest->samples == 4 ? 0b1001u
                                                                               : UINT_MAX;
    // The depth pass (which resets stencil to 0), then one pass per stencil
    // bit over the rectangles whose source may have nonzero stencil.
    for (uint32_t pass = 0; pass < (any_stencil ? 9u : 1u); ++pass) {
      TransferPipelineKey key;
      key.dest_kind = pass ? 1 + pass : 1;
      key.dest_format = dest->view_format;
      key.dest_samples = dest->samples;
      key.sample_mask = sample_mask;
      key.source_kind = kTransferSourceWords;
      key.source_msaa = false;
      ID3D12PipelineState* pipeline = GetTransferPipeline(key);
      if (!pipeline) {
        Skip("transfer_pipeline");
        break;
      }
      command_processor_.SetExternalPipeline(pipeline);
      const uint32_t pass_constants[3] = {LayoutConstant(*dest),
                                          (dest->width * scale_) | (dest->samples << 16),
                                          flags | (pass ? (pass - 1) << 8 : 0)};
      list.D3DSetGraphicsRoot32BitConstants(0, 3, pass_constants, 0);
      command_processor_.SetStencilReference(pass ? 0xFF : 0);
      for (size_t i = group; i < group_end; ++i) {
        if (pass && !pending_transfers_[i].stencil) continue;
        const D3D12_RECT& rect = pending_transfers_[i].rect;
        D3D12_VIEWPORT viewport = {float(rect.left), float(rect.top),
                                   float(rect.right - rect.left), float(rect.bottom - rect.top),
                                   0.0f, 1.0f};
        command_processor_.SetViewport(viewport);
        command_processor_.SetScissorRect(rect);
        list.D3DDrawInstanced(3, 1, 0, 0);
      }
    }
    Count("transfer_batch");
    group = group_end;
  }
  pending_transfers_.clear();
}

bool Fh1NativeExecutor::CreateTransferSourceViews(
    const Surface& source, ui::d3d12::util::DescriptorCpuGpuHandlePair (&srvs)[2]) {
  const bool source_depth = source.key.is_depth;
  if (!command_processor_.RequestOneUseSingleViewDescriptors(source_depth ? 2 : 1, srvs)) {
    return false;
  }
  if (!source_depth) srvs[1] = srvs[0];
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  for (uint32_t plane = 0; plane < (source_depth ? 2u : 1u); ++plane) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = plane ? source.stencil_srv_format : source.srv_format;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (source.samples > 1) {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Texture2D.MipLevels = 1;
      srv_desc.Texture2D.PlaneSlice = plane;
    }
    device->CreateShaderResourceView(source.resource.Get(), &srv_desc, srvs[plane].first);
  }
  return true;
}

void Fh1NativeExecutor::FlushColorTransfers(Surface& dest, size_t first, size_t end) {
  // All sources read as pixel shader resources, one barrier batch.
  for (size_t i = first; i < end; ++i) {
    if (Surface* source = FindSurface(pending_transfers_[i].source)) {
      Transition(source->resource.Get(), source->state,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }
  Transition(dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_processor_.SubmitBarriers();
  auto& list = command_processor_.GetDeferredCommandList();
  const uint32_t flags = TransferFlags();
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                             : UINT_MAX;
  command_processor_.SetExternalGraphicsRootSignature(transfer_root_signature_.Get());
  command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const bool dest_uint = dest.uint_view.ptr != 0;
  list.D3DOMSetRenderTargets(1, dest_uint ? &dest.uint_view : &dest.view, FALSE, nullptr);
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    ui::d3d12::util::DescriptorCpuGpuHandlePair srvs[2];
    if (!source || !CreateTransferSourceViews(*source, srvs)) {
      Skip("transfer_descriptor");
      i = source_end;
      continue;
    }
    TransferPipelineKey key;
    key.dest_kind = dest_uint ? kTransferDestUint : 0;
    key.dest_format = dest_uint ? dest.srv_format : dest.view_format;
    key.dest_samples = dest.samples;
    key.sample_mask = sample_mask;
    key.source_kind = SourceKind(source->key.is_depth, source->key.format);
    key.source_msaa = source->samples > 1;
    ID3D12PipelineState* pipeline = GetTransferPipeline(key);
    if (!pipeline) {
      Skip("transfer_pipeline");
      i = source_end;
      continue;
    }
    command_processor_.SetExternalPipeline(pipeline);
    list.D3DSetGraphicsRootDescriptorTable(1, srvs[0].second);
    list.D3DSetGraphicsRootDescriptorTable(2, srvs[1].second);
    const uint32_t constants[3] = {LayoutConstant(dest), LayoutConstant(*source), flags};
    list.D3DSetGraphicsRoot32BitConstants(0, 3, constants, 0);
    for (; i < source_end; ++i) {
      const D3D12_RECT& rect = pending_transfers_[i].rect;
      D3D12_VIEWPORT viewport = {float(rect.left), float(rect.top),
                                 float(rect.right - rect.left), float(rect.bottom - rect.top),
                                 0.0f, 1.0f};
      command_processor_.SetViewport(viewport);
      command_processor_.SetScissorRect(rect);
      list.D3DDrawInstanced(3, 1, 0, 0);
    }
  }
  Count("transfer_batch");
}

bool Fh1NativeExecutor::EnsureTransferWords(const Surface& dest) {
  const uint64_t size = uint64_t(dest.width) * dest.height * scale_ * scale_ * dest.samples *
                        sizeof(uint32_t);
  if (transfer_words_ && transfer_words_size_ >= size) return true;
  if (transfer_words_) {
    // The old buffer may still be in use by submitted commands; the buffer only
    // grows a few times, so keep outgrown ones until shutdown.
    retired_transfer_words_.push_back(std::move(transfer_words_));
  }
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = size;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  transfer_words_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                             transfer_words_state_, nullptr,
                                             IID_PPV_ARGS(&transfer_words_)))) {
    transfer_words_size_ = 0;
    return false;
  }
  transfer_words_size_ = uint32_t(size);
  return true;
}

ID3D12PipelineState* Fh1NativeExecutor::GetTransferWordsPipeline(uint32_t source_kind,
                                                                 bool msaa) {
  auto& pipeline = transfer_words_pipelines_[source_kind][msaa];
  if (pipeline) return pipeline.Get();
  static const D3D12_SHADER_BYTECODE kWordsShaders[3][2] = {
      {{shaders::fh1_native_transfer_words_color_cs,
        sizeof(shaders::fh1_native_transfer_words_color_cs)},
       {shaders::fh1_native_transfer_words_color_ms_cs,
        sizeof(shaders::fh1_native_transfer_words_color_ms_cs)}},
      {{shaders::fh1_native_transfer_words_depth_cs,
        sizeof(shaders::fh1_native_transfer_words_depth_cs)},
       {shaders::fh1_native_transfer_words_depth_ms_cs,
        sizeof(shaders::fh1_native_transfer_words_depth_ms_cs)}},
      {{shaders::fh1_native_transfer_words_uint_cs,
        sizeof(shaders::fh1_native_transfer_words_uint_cs)},
       {shaders::fh1_native_transfer_words_uint_ms_cs,
        sizeof(shaders::fh1_native_transfer_words_uint_ms_cs)}},
  };
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = resolve_memory_root_signature_.Get();
  desc.CS = kWordsShaders[source_kind][msaa];
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    return nullptr;
  }
  return pipeline.Get();
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
  for (const auto& rect : tiles_.SplitByOwner(resolve_key.base_tiles,
                                              resolve_key.pitch_tiles << is_64bpp, tile_width,
                                              tile_height, x0, y0, x1, y1)) {
    SourceRect source;
    source.rect = {rect.left, rect.top, rect.right, rect.bottom};
    source.unowned = rect.owner == kNoOwner;
    source.surface = source.unowned ? nullptr : FindSurface(rect.owner);
    sources_out.push_back(source);
  }
}

ID3D12PipelineState* Fh1NativeExecutor::GetResolveMemoryPipeline(uint32_t source_kind,
                                                                 bool msaa) {
  auto& pipeline = resolve_memory_pipelines_[source_kind][msaa];
  if (pipeline) return pipeline.Get();
  static const D3D12_SHADER_BYTECODE kResolveShaders[3][2] = {
      {{shaders::fh1_native_resolve_memory_color_cs,
        sizeof(shaders::fh1_native_resolve_memory_color_cs)},
       {shaders::fh1_native_resolve_memory_color_ms_cs,
        sizeof(shaders::fh1_native_resolve_memory_color_ms_cs)}},
      {{shaders::fh1_native_resolve_memory_depth_cs,
        sizeof(shaders::fh1_native_resolve_memory_depth_cs)},
       {shaders::fh1_native_resolve_memory_depth_ms_cs,
        sizeof(shaders::fh1_native_resolve_memory_depth_ms_cs)}},
      {{shaders::fh1_native_resolve_memory_uint_cs,
        sizeof(shaders::fh1_native_resolve_memory_uint_cs)},
       {shaders::fh1_native_resolve_memory_uint_ms_cs,
        sizeof(shaders::fh1_native_resolve_memory_uint_ms_cs)}},
  };
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = resolve_memory_root_signature_.Get();
  desc.CS = kResolveShaders[source_kind][msaa];
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    return nullptr;
  }
  return pipeline.Get();
}

void Fh1NativeExecutor::RecordDrawInputs(uint32_t used_texture_mask, const Shader& vertex_shader,
                                         uint32_t guest_dma_index_offset,
                                         uint32_t guest_dma_index_size) {
  if (!frame_dump_ || !frame_dump_->recording()) return;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        register_file_.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) continue;
    frame_dump_->RecordGpuRange(fetch.address << 2, fetch.size << 2);
  }
  if (guest_dma_index_size) {
    frame_dump_->RecordGpuRange(guest_dma_index_offset, guest_dma_index_size);
  }
  // Every level the fetch can reach, as the texture cache just loaded it.
  for (uint32_t mask = used_texture_mask; mask; mask &= mask - 1) {
    const auto fetch = register_file_.GetTextureFetch(uint32_t(std::countr_zero(mask)));
    uint32_t width = 1, height = 1, depth = 1;
    switch (fetch.dimension) {
      case xenos::DataDimension::k1D:
        width = fetch.size_1d.width + 1;
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
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width, height, depth, fetch.tiled, fetch.format,
        fetch.packed_mips, true, fetch.mip_max_level);
    const uint32_t base_size = layout.base.level_data_extent_bytes;
    const uint32_t mip_size = layout.mips_total_extent_bytes;
    if (fetch.base_address && base_size) {
      frame_dump_->RecordGpuRange(fetch.base_address << 12, base_size);
    }
    if (fetch.mip_address && mip_size) {
      frame_dump_->RecordGpuRange(fetch.mip_address << 12, mip_size);
    }
  }
}

bool Fh1NativeExecutor::DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const {
  if (!depth_control.stencil_enable) return false;
  const RegisterFile& regs = register_file_;
  auto may_write = [](xenos::StencilOp op, uint32_t reference) {
    switch (op) {
      case xenos::StencilOp::kKeep:
      case xenos::StencilOp::kZero:
        return false;
      case xenos::StencilOp::kReplace:
        return reference != 0;
      default:
        return true;
    }
  };
  auto face = [&](const reg::RB_STENCILREFMASK& mask, xenos::StencilOp fail,
                  xenos::StencilOp zfail, xenos::StencilOp zpass) {
    const uint32_t reference = mask.stencilref & mask.stencilwritemask;
    return mask.stencilwritemask != 0 &&
           (may_write(fail, reference) || may_write(zfail, reference) ||
            may_write(zpass, reference));
  };
  const auto front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  if (face(front, depth_control.stencilfail, depth_control.stencilzfail,
           depth_control.stencilzpass)) {
    return true;
  }
  return depth_control.backface_enable &&
         face(regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF),
              depth_control.stencilfail_bf, depth_control.stencilzfail_bf,
              depth_control.stencilzpass_bf);
}

bool Fh1NativeExecutor::GetDepthOverwriteRects(const Fh1NativeDrawInfo& draw,
                                               bool& stencil_overwritten) {
  return depth_overwrite_.Derive(draw, REXCVAR_GET(half_pixel_offset), stencil_overwritten);
}

bool Fh1NativeExecutor::ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length,
                                                 bool stencil_overwritten, bool stencil_written) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  for (const OverwriteRect& rect : depth_overwrite_.rects()) {
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    // Wider than the pitch wraps into the next row of tiles.
    if (column_end > key.pitch_tiles || (row_end - 1) * key.pitch_tiles + column_end > length) {
      return false;
    }
  }
  for (const OverwriteRect& rect : depth_overwrite_.rects()) {
    ClaimOverwrittenDepthTiles(key, rect.inner, stencil_overwritten);
    // The tiles the draw touches beyond the covered ones, as up to four
    // strips around them, each taken with one transfer where possible.
    const uint32_t column_first = uint32_t(rect.outer[0]) / tile_width;
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_first = uint32_t(rect.outer[1]) / tile_height;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    uint32_t inner_column_first = (uint32_t(rect.inner[0]) + tile_width - 1) / tile_width;
    uint32_t inner_column_end = uint32_t(rect.inner[2]) / tile_width;
    uint32_t inner_row_first = (uint32_t(rect.inner[1]) + tile_height - 1) / tile_height;
    uint32_t inner_row_end = uint32_t(rect.inner[3]) / tile_height;
    if (inner_column_first >= inner_column_end || inner_row_first >= inner_row_end) {
      inner_column_first = inner_column_end = column_first;
      inner_row_first = inner_row_end = row_first;
    }
    ClaimTileRect(key, column_first, row_first, column_end, inner_row_first);
    ClaimTileRect(key, column_first, inner_row_end, column_end, row_end);
    ClaimTileRect(key, column_first, inner_row_first, inner_column_first, inner_row_end);
    ClaimTileRect(key, inner_column_end, inner_row_first, column_end, inner_row_end);
    // Covered tiles the stencil check left unclaimed.
    ClaimTileRect(key, inner_column_first, inner_row_first, inner_column_end, inner_row_end);
    if (stencil_written) {
      for (uint32_t row = row_first; row < row_end; ++row) {
        MarkTileStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                        column_end - column_first, true);
      }
    }
  }
  Count("depth_overwrite_draw");
  return true;
}

void Fh1NativeExecutor::ClaimOverwrittenDepthTiles(const SurfaceKey& key,
                                                   const std::array<int32_t, 4>& rect,
                                                   bool stencil_overwritten) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t column_first = (uint32_t(rect[0]) + tile_width - 1) / tile_width;
  const uint32_t column_end = std::min(uint32_t(rect[2]) / tile_width, key.pitch_tiles);
  const uint32_t row_first = (uint32_t(rect[1]) + tile_height - 1) / tile_height;
  const uint32_t row_end = uint32_t(rect[3]) / tile_height;
  if (column_first >= column_end || row_first >= row_end) return;
  auto tile_index = [&](uint32_t row, uint32_t column) {
    return row * key.pitch_tiles + column;
  };
  const uint32_t packed_key = key.Pack();
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  // A draw that keeps stencil needs zero stencil in the covered tiles' words;
  // the new owner's stencil is then reset there instead of transferred.
  if (!stencil_overwritten) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      for (uint32_t column = column_first; column < column_end; ++column) {
        if (tiles_.StencilNonzero(key.base_tiles + tile_index(row, column))) {
          return;
        }
      }
    }
  }
  for (uint32_t row = row_first; row < row_end; ++row) {
    ClaimTiles(key.base_tiles + tile_index(row, column_first), column_end - column_first,
               packed_key, false);
  }
  if (!stencil_overwritten && dest->stencil_nonzero) {
    const D3D12_RECT clear_rect = HostRect(
        {LONG(column_first * tile_width), LONG(row_first * tile_height),
         LONG(std::min(column_end * tile_width, dest->width)),
         LONG(std::min(row_end * tile_height, dest->height))});
    GpuTimer gpu_timer(*this, kGpuClears);
    Transition(dest->resource.Get(), dest->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_processor_.SubmitBarriers();
    command_processor_.GetDeferredCommandList().D3DClearDepthStencilView(
        dest->view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 1, &clear_rect);
  }
  Count("depth_overwrite_claim");
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
  if (!used_bits) return;

  // Tiles each target covers, as far down as the draw can reach.
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t height_used =
      std::min(SurfaceHeight(pitch_tiles, msaa),
               draw.vertex_shader ? draw_extent_estimator_.EstimateMaxY(true, *draw.vertex_shader)
                                  : SurfaceHeight(pitch_tiles, msaa));
  const uint32_t length_tiles_32bpp =
      ((height_used << msaa_y_log2) + xenos::kEdramTileHeightSamples - 1) /
      xenos::kEdramTileHeightSamples * pitch_tiles;
  const bool depth_stencil_written =
      (used_bits & 1) && DrawMayWriteNonzeroStencil(draw.normalized_depth_control);
  // Most draws repeat the previous draw's targets: with no ownership or
  // stencil change since, its claims still hold. Depth-overwriting draws claim
  // by their rectangles and always take the full path.
  const bool may_overwrite_depth =
      used_bits == 1 && draw.normalized_depth_control.z_enable &&
      draw.normalized_depth_control.z_write_enable &&
      draw.normalized_depth_control.zfunc == xenos::CompareFunction::kAlways;
  PrepareSignature signature;
  signature.generation = tiles_.generation();
  signature.used_bits = used_bits;
  signature.length_tiles = length_tiles_32bpp;
  signature.stencil_written = depth_stencil_written;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (used_bits & (1u << i)) signature.keys[i] = keys[i].Pack();
  }
  if (!may_overwrite_depth && signature == last_prepare_) return;

  // Create the surfaces first so ownership transfers can write into them.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if ((used_bits & (1u << i)) && !GetOrCreateSurface(keys[i])) used_bits &= ~(1u << i);
  }
  if (depth_stencil_written) FindSurface(keys[0].Pack())->stencil_nonzero = true;
  std::vector<std::pair<uint32_t, uint32_t>> bases;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (used_bits & (1u << i)) bases.emplace_back(keys[i].base_tiles, i);
  }
  std::sort(bases.begin(), bases.end());
  bool stencil_overwritten = false;
  const bool depth_overwritten =
      used_bits == 1 && GetDepthOverwriteRects(draw, stencil_overwritten);
  for (size_t i = 0; i < bases.size(); ++i) {
    const SurfaceKey& key = keys[bases[i].second];
    const uint32_t next_base = i + 1 < bases.size()
                                   ? bases[i + 1].first
                                   : xenos::kEdramTileCount + bases[0].first;
    const uint32_t length =
        std::min(length_tiles_32bpp << uint32_t(key.Is64bpp()), next_base - key.base_tiles);
    if (depth_overwritten &&
        ClaimDepthOverwriteTiles(key, length, stencil_overwritten, depth_stencil_written)) {
      continue;
    }
    ClaimTiles(key.base_tiles, length, key.Pack());
    // Color words and depth draws that may write stencil leave the low byte
    // unknown.
    if (!key.is_depth || depth_stencil_written) MarkTileStencil(key.base_tiles, length, true);
  }
  FlushTransfers();
  // A depth-overwriting draw may have claimed only its rectangles.
  if (used_bits == signature.used_bits && !may_overwrite_depth) {
    signature.generation = tiles_.generation();
    last_prepare_ = signature;
  } else {
    last_prepare_ = {};
  }
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
  ++draws_;
}

void Fh1NativeExecutor::ClearSurfaceRect(Surface& surface, const D3D12_RECT& guest_rect,
                                         uint32_t clear_value, uint32_t clear_value_lo) {
  const D3D12_RECT rect = HostRect(guest_rect);
  auto& list = command_processor_.GetDeferredCommandList();
  GpuTimer gpu_timer(*this, kGpuClears);
  if (surface.key.is_depth) {
    // The host keeps float24 depth halved in [0, 1) (as the Xenos render
    // target cache did).
    const uint32_t depth_bits = (clear_value >> 8) & 0xFFFFFF;
    const float value = xenos::DepthRenderTargetFormat(surface.key.format) ==
                                xenos::DepthRenderTargetFormat::kD24FS8
                            ? xenos::Float20e4To32(depth_bits) * 0.5f
                            : xenos::UNorm24To32(depth_bits);
    surface.stencil_nonzero |= (clear_value & 0xFF) != 0;
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
    // 64bpp clears hold the low word in RB_COLOR_CLEAR_LO.
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
      const bool is_float =
          surface.key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT) ||
          surface.key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT);
      const uint32_t words[2] = {surface.key.Is64bpp() ? clear_value_lo : clear_value,
                                 clear_value};
      for (uint32_t i = 0; i < (surface.key.Is64bpp() ? 4u : 2u); ++i) {
        const uint16_t bits = uint16_t(words[i >> 1] >> (16 * (i & 1)));
        color[i] = is_float ? HalfToFloat(bits)
                            : std::max(float(int16_t(bits)) / 32767.0f, -1.0f);
      }
      break;
    }
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      std::memcpy(&color[0], &clear_value_lo, sizeof(float));
      std::memcpy(&color[1], &clear_value, sizeof(float));
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
                                        D3D12_GPU_VIRTUAL_ADDRESS target, bool unscaled_dest) {
  Surface& surface = *source.surface;
  GpuTimer gpu_timer(*this, kGpuResolves);
  const bool depth = surface.key.is_depth;
  const bool msaa = surface.samples > 1;
  ID3D12PipelineState* pipeline =
      GetResolveMemoryPipeline(SourceKind(depth, surface.key.format), msaa);
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
    device->CreateShaderResourceView(surface.resource.Get(), &srv_desc, srvs[i].first);
  }
  Transition(surface.resource.Get(), surface.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  native_memory_->UseForWriting();
  command_processor_.SubmitBarriers();

  const SurfaceKey& owner = surface.key;
  const uint32_t host_sample_mode =
      owner.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  const D3D12_RECT rect = unscaled_dest ? source.rect : HostRect(source.rect);
  uint32_t constants[kResolveMemoryConstantCount];
  constants[0] = uint32_t(rect.left) | (uint32_t(rect.top) << 16);
  constants[1] = uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
  constants[2] = PackLayout(resolve_key.base_tiles, resolve_key.pitch_tiles, resolve_key.msaa,
                            resolve_key.Is64bpp(), resolve_key.is_depth, resolve_key.format);
  constants[3] = PackLayout(owner.base_tiles, owner.pitch_tiles, owner.msaa, owner.Is64bpp(),
                            owner.is_depth, owner.format) |
                 (host_sample_mode << 27);
  constants[4] = sample_select;
  constants[5] = dest_info | ((scale_ - 1) << 20) | (unscaled_dest ? 1u << 22 : 0u);
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
  plan = CopyPlan();
  const Fh1ResolveFlags flags{config_.depth_float24_round, config_.gamma_as_unorm16,
                              config_.fixed16_truncated};
  if (!Fh1PlanResolve(register_file_, memory_, flags, plan)) return false;
  if (plan.copy) {
    GetResolveSources(plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1, plan.sources);
  }
  return true;
}

bool Fh1NativeExecutor::NativeResolve(uint32_t& written_address, uint32_t& written_length) {
  written_address = 0;
  written_length = 0;
  if (!initialized_) return false;
  if (!Resolve(&written_address, &written_length)) return false;
  if (written_length) ReadBackResolve(written_address, written_length);
  return true;
}

bool Fh1NativeExecutor::IsOneOffResolve(uint32_t address, uint32_t length) {
  // Resolves to a range come in runs of frames (a new run after a gap of a few
  // frames). A range is new when first resolved or after seconds without a
  // run. One-off captures such as a car thumbnail are a few runs over a new,
  // large range; per-frame targets and periodic updates (reflection faces
  // every few frames) keep a range busy. While a range is new, the first
  // frames of each run are read back.
  constexpr uint64_t kRunGapFrames = 4, kRunReadbackFrames = 2, kIdleFrames = 300,
                     kNewFrames = 120;
  constexpr uint32_t kMinLength = 256 * 1024;
  ResolveReadback& readback = resolve_readbacks_[(uint64_t(address) << 32) | length];
  if (!readback.last_used_frame || frame_ > readback.last_used_frame + kRunGapFrames) {
    if (!readback.last_used_frame || frame_ > readback.last_used_frame + kIdleFrames) {
      readback.new_since_frame = frame_;
    }
    readback.run_start_frame = frame_;
  }
  readback.last_used_frame = frame_;
  return REXCVAR_GET(fh1_native_readback_new_resolves) && length >= kMinLength &&
         frame_ < readback.run_start_frame + kRunReadbackFrames &&
         frame_ < readback.new_since_frame + kNewFrames;
}

void Fh1NativeExecutor::QueueResolveReadback(uint32_t address, uint32_t length) {
  // The CPU can only read the result after the GPU reports progress, so the
  // copy waits for the next CPU-visible command (FlushResolveReadbacks).
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer = CreateReadbackBuffer(length);
  if (!buffer) return Skip("resolve_readback_buffer");
  native_memory_->UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      buffer.Get(), 0, native_memory_->GetBuffer(), address, length);
  pending_readbacks_.push_back({address, length, std::move(buffer)});
  Count("resolve_readback_one_off");
}

void Fh1NativeExecutor::ReadBackResolve(uint32_t address, uint32_t length) {
  // At scale resolves write the scaled resolve range, not guest memory (the
  // resolve reads one-off captures back itself).
  if (scale_ > 1) return;
  const ReadbackResolveMode mode = command_processor_.Fh1ReadbackResolveMode();
  if (mode == ReadbackResolveMode::kDisabled) {
    if (IsOneOffResolve(address, length)) QueueResolveReadback(address, length);
    return;
  }
  ResolveReadback& readback = resolve_readbacks_[(uint64_t(address) << 32) | length];
  readback.last_used_frame = frame_;
  uint8_t* destination = memory_.TranslatePhysical(address);
  if (!destination) return;
  const uint32_t write_index = readback.current;
  const uint32_t size = (length + 0xFFFF) & ~uint32_t(0xFFFF);
  if (!readback.buffers[write_index] || readback.sizes[write_index] < size) {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer = CreateReadbackBuffer(size);
    if (!buffer) return Skip("resolve_readback_buffer");
    // The old buffer may still be the target of a submitted copy.
    if (readback.buffers[write_index]) command_processor_.Fh1AwaitAllQueueOperations();
    readback.buffers[write_index] = std::move(buffer);
    readback.sizes[write_index] = size;
  }
  native_memory_->UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      readback.buffers[write_index].Get(), 0, native_memory_->GetBuffer(), address, length);
  readback.submissions[write_index] = command_processor_.GetCurrentSubmission();
  Count("resolve_readback");

  // Full: wait for this resolve. Fast and some: take the previous frame's copy
  // of the same range, waiting only when there is none (some copies only then).
  uint32_t read_index = write_index;
  bool miss = false;
  if (mode == ReadbackResolveMode::kFull) {
    command_processor_.Fh1AwaitAllQueueOperations();
  } else {
    read_index = 1 - write_index;
    if (!readback.buffers[read_index] || readback.sizes[read_index] < length) {
      miss = true;
      read_index = write_index;
      command_processor_.Fh1AwaitAllQueueOperations();
    } else if (readback.submissions[read_index] > command_processor_.GetCompletedSubmission()) {
      command_processor_.Fh1AwaitAllQueueOperations();
    }
  }
  if (mode != ReadbackResolveMode::kSome || miss || mode == ReadbackResolveMode::kFull) {
    void* mapped = nullptr;
    D3D12_RANGE range = {0, length};
    if (SUCCEEDED(readback.buffers[read_index]->Map(0, &range, &mapped))) {
      std::memcpy(destination, mapped, length);
      D3D12_RANGE written = {0, 0};
      readback.buffers[read_index]->Unmap(0, &written);
    }
  }
  readback.current = 1 - readback.current;
}

Microsoft::WRL::ComPtr<ID3D12Resource> Fh1NativeExecutor::CreateReadbackBuffer(uint32_t size) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  if (FAILED(command_processor_.GetD3D12Provider().GetDevice()->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&buffer)))) {
    return nullptr;
  }
  return buffer;
}

void Fh1NativeExecutor::FlushResolveReadbacks() {
  if (pending_readbacks_.empty()) return;
  command_processor_.Fh1AwaitAllQueueOperations();
  // In order: a later resolve over the same bytes wins, as on the GPU.
  for (PendingReadback& pending : pending_readbacks_) {
    uint8_t* destination = memory_.TranslatePhysical(pending.address);
    void* mapped = nullptr;
    D3D12_RANGE range = {0, pending.length};
    if (destination && SUCCEEDED(pending.buffer->Map(0, &range, &mapped))) {
      std::memcpy(destination, mapped, pending.length);
      D3D12_RANGE written = {0, 0};
      pending.buffer->Unmap(0, &written);
    }
  }
  pending_readbacks_.clear();
  Count("resolve_readback_flush");
}

bool Fh1NativeExecutor::Resolve(uint32_t* written_address, uint32_t* written_length) {
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
    // At scale the destination is the texture cache's scaled resolve range,
    // relative to the adjusted destination base.
    D3D12_GPU_VIRTUAL_ADDRESS scaled_target = 0;
    uint32_t dest_base = plan.dest_base;
    if (scale_ > 1) {
      if (!native_textures_->EnsureScaledResolveMemoryCommitted(
              resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length) ||
          !native_textures_->MakeScaledResolveRangeCurrent(
              resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start -
                                               resolve_info.copy_dest_base +
                                               resolve_info.copy_dest_extent_length)) {
        Skip("resolve_scaled_memory");
        return false;
      }
      native_textures_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      scaled_target = native_textures_->GetCurrentScaledResolveRangeGPUAddress();
      dest_base = plan.dest_base - resolve_info.copy_dest_base;
    } else {
      native_memory_->RequestRange(resolve_info.copy_dest_extent_start,
                                   resolve_info.copy_dest_extent_length);
    }
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
      if (!owner.is_depth &&
          !Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(owner.format))) {
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
                           dest_base, plan.dest_pitch, scaled_target)) {
        complete = false;
      }
    }
    if (scale_ > 1) {
      native_textures_->MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
      // One-off captures the CPU reads also need the guest layout: resolved
      // again unscaled into the guest memory mirror, from each guest pixel's
      // first host pixel, and read back.
      if (written_address &&
          command_processor_.Fh1ReadbackResolveMode() == ReadbackResolveMode::kDisabled &&
          IsOneOffResolve(resolve_info.copy_dest_extent_start,
                          resolve_info.copy_dest_extent_length)) {
        native_memory_->RequestRange(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length);
        bool unscaled_complete = true;
        for (const SourceRect& source : plan.sources) {
          if (!source.surface || source.rect.left >= source.rect.right ||
              source.rect.top >= source.rect.bottom ||
              (!source.surface->key.is_depth &&
               !Fh1IsResolveColorFormatSupported(
                   xenos::ColorRenderTargetFormat(source.surface->key.format)))) {
            continue;
          }
          unscaled_complete &=
              ResolveToMemory(source, plan.resolve_key, plan.sample_select, plan.dest_info,
                              plan.dest_base, plan.dest_pitch, 0, true);
        }
        if (unscaled_complete) {
          QueueResolveReadback(resolve_info.copy_dest_extent_start,
                               resolve_info.copy_dest_extent_length);
        }
      }
    }
    // Invalidates textures over the range (and marks it scaled at scale).
    native_textures_->MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                          resolve_info.copy_dest_extent_length);
    if (written_address) *written_address = resolve_info.copy_dest_extent_start;
    if (written_length) *written_length = resolve_info.copy_dest_extent_length;
    if (complete) {
      ++resolves_;
      Count(plan.sources.size() > 1 ? "resolve_multi_owner" : "resolve_single_owner");
    }
    succeeded = complete;
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
    // Tiles the clear covers completely change owner without a transfer, as
    // on the guest their old words are overwritten; edge tiles keep the rest.
    const uint32_t inner_first = (uint32_t(x0) + tile_width - 1) / tile_width;
    const uint32_t inner_end = std::max(inner_first, uint32_t(x1) / tile_width);
    for (uint32_t row = uint32_t(y0) / tile_height;
         row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
      const uint32_t row_base = key.base_tiles + row * pitch;
      const bool row_covered =
          row * tile_height >= uint32_t(y0) && (row + 1) * tile_height <= uint32_t(y1);
      if (!row_covered || inner_first >= inner_end) {
        ClaimTiles(row_base + column_first, column_end - column_first, key.Pack());
        continue;
      }
      if (column_first < inner_first) {
        ClaimTiles(row_base + column_first, inner_first - column_first, key.Pack());
      }
      ClaimTiles(row_base + inner_first, inner_end - inner_first, key.Pack(), false);
      if (inner_end < column_end) {
        ClaimTiles(row_base + inner_end, column_end - inner_end, key.Pack());
      }
    }
    for (uint32_t row = uint32_t(y0) / tile_height;
         row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
      const uint32_t row_base = key.base_tiles + row * pitch;
      const bool row_covered =
          row * tile_height >= uint32_t(y0) && (row + 1) * tile_height <= uint32_t(y1);
      if (!key.is_depth || (value & 0xFF)) {
        MarkTileStencil(row_base + column_first, column_end - column_first, true);
      } else if (row_covered && inner_first < inner_end) {
        MarkTileStencil(row_base + inner_first, inner_end - inner_first, false);
      }
    }
    FlushTransfers();
    ClearSurfaceRect(*surface, rect, value, value_lo);
    Count(key.is_depth ? "clear_depth" : "clear_color");
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

void Fh1NativeExecutor::OnSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width,
                               uint32_t height, const uint32_t* gamma_pwl) {
  if (!initialized_) return;
  GpuEndFrame();
  GpuDrain();
  if (frame_dump_) frame_dump_->OnSwap(frame, frontbuffer_address);
  frame_ = frame;
  ++cpu_frames_;
  DrainDumps();
  if (dump_frames_.count(frame) && !dump_directory_.empty()) {
    QueueFrontBufferDump(frame, width, height, gamma_pwl);
  }
  if (frame % 600 == 0) LogStats(frame);
}

void Fh1NativeExecutor::QueueFrontBufferDump(uint64_t frame, uint32_t width, uint32_t height,
                                             const uint32_t* gamma_pwl) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  xenos::TextureFormat format;
  uint32_t texture_width = 0, texture_height = 0;
  ID3D12Resource* front =
      native_textures_->RequestSwapTexture(srv_desc, format, &texture_width, &texture_height,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, nullptr);
  if (!front) {
    Skip("dump_front_buffer_missing");
    return;
  }
  PendingDump dump;
  dump.frame = frame;
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
        dump_directory_ / ("native-" + std::to_string(dump.frame) + ".ppm"),
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

uint32_t Fh1NativeExecutor::GpuBegin() {
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  if (!gpu_query_heap_ || slot.pending || slot.used + 2 > kGpuProfileQueries) return UINT32_MAX;
  const uint32_t index = gpu_slot_ * kGpuProfileQueries + slot.used++;
  command_processor_.GetDeferredCommandList().D3DEndQuery(gpu_query_heap_.Get(),
                                                         D3D12_QUERY_TYPE_TIMESTAMP, index);
  return index;
}

void Fh1NativeExecutor::GpuEnd(GpuPhase phase, uint32_t begin) {
  if (begin == UINT32_MAX) return;
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  const uint32_t index = gpu_slot_ * kGpuProfileQueries + slot.used++;
  command_processor_.GetDeferredCommandList().D3DEndQuery(gpu_query_heap_.Get(),
                                                         D3D12_QUERY_TYPE_TIMESTAMP, index);
  slot.spans.emplace_back(phase, begin - gpu_slot_ * kGpuProfileQueries,
                          index - gpu_slot_ * kGpuProfileQueries);
}

void Fh1NativeExecutor::GpuEndFrame() {
  if (!gpu_query_heap_) return;
  GpuProfileSlot& slot = gpu_slots_[gpu_slot_];
  if (slot.pending) return;  // Previous results not read yet: this frame was not measured.
  if (slot.used) {
    command_processor_.GetDeferredCommandList().D3DResolveQueryData(
        gpu_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpu_slot_ * kGpuProfileQueries,
        slot.used, slot.readback.Get(), 0);
  }
  slot.submission = command_processor_.GetCurrentSubmission();
  slot.pending = true;
  gpu_slot_ = (gpu_slot_ + 1) % kGpuProfileSlots;
}

void Fh1NativeExecutor::GpuDrain() {
  if (!gpu_query_heap_) return;
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  for (GpuProfileSlot& slot : gpu_slots_) {
    if (!slot.pending || slot.submission > completed) continue;
    if (slot.used) {
      void* mapped = nullptr;
      D3D12_RANGE range = {0, slot.used * sizeof(uint64_t)};
      if (SUCCEEDED(slot.readback->Map(0, &range, &mapped))) {
        const auto* ticks = static_cast<const uint64_t*>(mapped);
        for (const auto& [phase, begin, end] : slot.spans) {
          if (ticks[end] > ticks[begin]) gpu_ticks_[phase] += ticks[end] - ticks[begin];
        }
        D3D12_RANGE written = {0, 0};
        slot.readback->Unmap(0, &written);
      }
    }
    ++gpu_frames_;
    slot.used = 0;
    slot.spans.clear();
    slot.pending = false;
  }
}

void Fh1NativeExecutor::LogStats(uint64_t frame) {
  const std::string skips = counters_.FormatSkips();
  const std::string stats = counters_.FormatStats();
  REXGPU_INFO(
      "FH1 native executor frame={} draws={} resolves={} surfaces={} skips={{{}}} stats={{{}}}",
      frame, draws_, resolves_, surfaces_.size(), skips, stats);
  // Memory: surfaces and the executor's buffers, and the command processor's
  // texture cache against its limits.
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  uint64_t surface_bytes = 0;
  for (const auto& [packed, surface] : surfaces_) {
    const D3D12_RESOURCE_DESC desc = surface.resource->GetDesc();
    surface_bytes += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
  }
  REXGPU_INFO(
      "FH1 native executor memory MB: surfaces {} transfer_words {} textures {} "
      "scaled_resolve {}",
      surface_bytes >> 20, transfer_words_size_ >> 20,
      native_textures_ ? native_textures_->total_host_memory_usage() >> 20 : 0,
      native_textures_ ? native_textures_->scaled_resolve_committed_bytes() >> 20 : 0);
  if (cpu_frames_) {
    if (cpu_timing_) {
      // Transfers run inside target preparation and resolve clears.
      const double frames = double(cpu_frames_);
      REXGPU_INFO(
          "FH1 native executor cpu ms/frame over {} frames: prepare_targets {:.3f} (transfers "
          "{:.3f}) bind_targets {:.3f} resolves {:.3f}",
          cpu_frames_, cpu_ns_[kCpuPrepareTargets] / frames / 1e6,
          cpu_ns_[kCpuTransfers] / frames / 1e6, cpu_ns_[kCpuBindTargets] / frames / 1e6,
          cpu_ns_[kCpuResolves] / frames / 1e6);
    }
    REXGPU_INFO("FH1 native executor render-target binds {} requested, {} elided as repeats",
                command_processor_.GetDeferredCommandList().render_target_binds(),
                command_processor_.GetDeferredCommandList().elided_render_target_binds());
    cpu_ns_ = {};
    cpu_frames_ = 0;
    // Cumulative shared-memory uploads by kind (NP-2.8): requests that
    // uploaded, page ranges and bytes.
    using Kind = SharedMemory::UploadKind;
    const auto stats = [this](Kind kind) {
      const SharedMemory::UploadStats& s = native_memory_->upload_stats(kind);
      return fmt::format("{}/{}/{:.1f}MB (repeat {:.1f}MB)", s.requests, s.ranges,
                         double(s.bytes) / 1048576.0, double(s.repeat_bytes) / 1048576.0);
    };
    REXGPU_INFO("Shared memory uploads: vertex {} index {} texture {} memexport {} other {}",
                stats(Kind::kVertex), stats(Kind::kIndex), stats(Kind::kTexture),
                stats(Kind::kMemexport), stats(Kind::kOther));
  }
  if (gpu_frames_ && gpu_timestamp_frequency_) {
    const double scale = 1000.0 / double(gpu_timestamp_frequency_) / double(gpu_frames_);
    REXGPU_INFO(
        "FH1 native executor gpu ms/frame over {} frames: transfers {:.3f} resolves {:.3f} "
        "clears {:.3f} texture_reloads {:.3f} texture_loads {:.3f}",
        gpu_frames_, gpu_ticks_[kGpuTransfers] * scale, gpu_ticks_[kGpuResolves] * scale,
        gpu_ticks_[kGpuClears] * scale, gpu_ticks_[kGpuTextureReloads] * scale,
        gpu_ticks_[kGpuTextureLoads] * scale);
    gpu_ticks_ = {};
    gpu_frames_ = 0;
    std::vector<std::pair<uint64_t, std::string>> volume;
    for (const auto& [pair, tiles] : transfer_volume_) volume.emplace_back(tiles, pair);
    std::sort(volume.rbegin(), volume.rend());
    std::string top;
    for (size_t i = 0; i < std::min<size_t>(volume.size(), 8); ++i) {
      top += volume[i].second + "=" + std::to_string(volume[i].first) + " ";
    }
    REXGPU_INFO("FH1 native executor transfer tile-passes: {}", top);
    transfer_volume_.clear();
  }
}

}  // namespace rex::graphics::d3d12
