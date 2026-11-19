/**
 * @file        graphics/d3d11/fh1_native_executor.cpp
 * @brief       Direct3D 11 FH1 native executor.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/fh1_native_executor.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/fh1_executor_counters.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

// The Direct3D 12 executor defines these when it is built into the same
// library; the Direct3D 11 one shares them, so a configuration switches
// backends without renaming its settings.
#if REX_HAS_D3D12
REXCVAR_DECLARE(std::string, fh1_native_dump_frames);
REXCVAR_DECLARE(std::string, fh1_native_dump_dir);
REXCVAR_DECLARE(bool, fh1_native_readback_new_resolves);
// Defined by the Direct3D 12 command processor or the Vulkan executor.
#if REX_HAS_D3D12 || REX_HAS_VULKAN
#define REX_D3D11_FH1_RESOLVE_DUMP 1
REXCVAR_DECLARE(std::string, fh1_resolve_dump_dir);
REXCVAR_DECLARE(int32_t, fh1_resolve_dump_frame);
#endif
#else
REXCVAR_DEFINE_STRING(fh1_native_dump_frames, "", "GPU",
                      "Comma-separated frames whose native front buffer is written as PPM")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_native_dump_dir, "", "GPU",
                      "Directory for native front-buffer PPM dumps")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_readback_new_resolves, true, "GPU",
                    "With readback_resolve = none, still read back to guest memory, waiting for "
                    "the GPU, resolves to ranges no resolve wrote in the last few frames: one-off "
                    "captures the game reads on the CPU, such as the car thumbnails it saves. "
                    "Ranges resolved every frame are not read back")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_color_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_color_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_depth_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_depth_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_uint_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_uint_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_color_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_color_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_depth_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_depth_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_uint_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_uint_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_words_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_words_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_words_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_words_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"

// Feature level 11_0 has no rectangle clears (ClearView is Direct3D 11.1), so
// partial color clears are drawn with this pixel shader, which writes the
// value in b0 to SV_Target 0. Source (fxc /T ps_5_0 /O3 /Qstrip_reflect
// /Qstrip_debug /Qstrip_priv):
//
//   cbuffer Fh1NativeClearConstants : register(b0) { float4 fh1_clear_color; };
//   float4 main() : SV_Target { return fh1_clear_color; }
//
// It belongs with the other fh1_native_* shaders in src/graphics/shaders
// (fh1_native_clear_color.ps.hlsl and a d3d11_5_0 header) once the shader
// build has it; until then it is kept here.
const BYTE fh1_native_clear_color_ps[] = {
    68,  88,  66,  67,  193, 3,   231, 105, 170, 80,  219, 240, 118, 174, 247, 154, 247, 147,
    59,  98,  1,   0,   0,   0,   188, 0,   0,   0,   3,   0,   0,   0,   44,  0,   0,   0,
    60,  0,   0,   0,   112, 0,   0,   0,   73,  83,  71,  78,  8,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   0,   0,   79,  83,  71,  78,  44,  0,   0,   0,   1,   0,   0,   0,
    8,   0,   0,   0,   32,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   3,   0,
    0,   0,   0,   0,   0,   0,   15,  0,   0,   0,   83,  86,  95,  84,  97,  114, 103, 101,
    116, 0,   171, 171, 83,  72,  69,  88,  68,  0,   0,   0,   80,  0,   0,   0,   17,  0,
    0,   0,   106, 8,   0,   1,   89,  0,   0,   4,   70,  142, 32,  0,   0,   0,   0,   0,
    1,   0,   0,   0,   101, 0,   0,   3,   242, 32,  16,  0,   0,   0,   0,   0,   54,  0,
    0,   6,   242, 32,  16,  0,   0,   0,   0,   0,   70,  142, 32,  0,   0,   0,   0,   0,
    0,   0,   0,   0,   62,  0,   0,   1};
}  // namespace shaders

namespace {

struct ShaderBytecode {
  const void* data;
  size_t size;
};
#define FH1_SHADER(name) ShaderBytecode{shaders::name, sizeof(shaders::name)}

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

// Bytes per sample of the host formats surfaces use, for the memory stats
// (Direct3D 11 has no GetResourceAllocationInfo).
uint32_t FormatBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      return 8;
    default:
      return 4;
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
const ShaderBytecode kTransferShaders[4][2][3][2] = {
    {
        {
            {FH1_SHADER(fh1_native_transfer_color_from_color_ps),
             FH1_SHADER(fh1_native_transfer_color_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_color_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_color_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_color_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_color_from_uint_ms_ps)},
        },
        {
            {FH1_SHADER(fh1_native_transfer_color_dms_from_color_ps),
             FH1_SHADER(fh1_native_transfer_color_dms_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_color_dms_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_color_dms_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_color_dms_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_color_dms_from_uint_ms_ps)},
        },
    },
    {
        {
            {FH1_SHADER(fh1_native_transfer_depth_from_color_ps),
             FH1_SHADER(fh1_native_transfer_depth_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_depth_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_depth_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_depth_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_depth_from_uint_ms_ps)},
        },
        {
            {FH1_SHADER(fh1_native_transfer_depth_dms_from_color_ps),
             FH1_SHADER(fh1_native_transfer_depth_dms_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_depth_dms_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_depth_dms_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_depth_dms_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_depth_dms_from_uint_ms_ps)},
        },
    },
    {
        {
            {FH1_SHADER(fh1_native_transfer_stencil_from_color_ps),
             FH1_SHADER(fh1_native_transfer_stencil_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_stencil_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_stencil_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_stencil_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_stencil_from_uint_ms_ps)},
        },
        {
            {FH1_SHADER(fh1_native_transfer_stencil_dms_from_color_ps),
             FH1_SHADER(fh1_native_transfer_stencil_dms_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_stencil_dms_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_stencil_dms_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_stencil_dms_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_stencil_dms_from_uint_ms_ps)},
        },
    },
    {
        {
            {FH1_SHADER(fh1_native_transfer_uint_from_color_ps),
             FH1_SHADER(fh1_native_transfer_uint_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_uint_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_uint_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_uint_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_uint_from_uint_ms_ps)},
        },
        {
            {FH1_SHADER(fh1_native_transfer_uint_dms_from_color_ps),
             FH1_SHADER(fh1_native_transfer_uint_dms_from_color_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_uint_dms_from_depth_ps),
             FH1_SHADER(fh1_native_transfer_uint_dms_from_depth_ms_ps)},
            {FH1_SHADER(fh1_native_transfer_uint_dms_from_uint_ps),
             FH1_SHADER(fh1_native_transfer_uint_dms_from_uint_ms_ps)},
        },
    },
};

// [depth, stencil bit][dest msaa]
const ShaderBytecode kFromWordsShaders[2][2] = {
    {FH1_SHADER(fh1_native_transfer_depth_from_words_ps),
     FH1_SHADER(fh1_native_transfer_depth_dms_from_words_ps)},
    {FH1_SHADER(fh1_native_transfer_stencil_from_words_ps),
     FH1_SHADER(fh1_native_transfer_stencil_dms_from_words_ps)},
};

// [source kind: color, depth, uint][source msaa]
const ShaderBytecode kWordsShaders[3][2] = {
    {FH1_SHADER(fh1_native_transfer_words_color_cs),
     FH1_SHADER(fh1_native_transfer_words_color_ms_cs)},
    {FH1_SHADER(fh1_native_transfer_words_depth_cs),
     FH1_SHADER(fh1_native_transfer_words_depth_ms_cs)},
    {FH1_SHADER(fh1_native_transfer_words_uint_cs),
     FH1_SHADER(fh1_native_transfer_words_uint_ms_cs)},
};

// [source kind: color, depth, uint][source msaa]
const ShaderBytecode kResolveShaders[3][2] = {
    {FH1_SHADER(fh1_native_resolve_memory_color_cs),
     FH1_SHADER(fh1_native_resolve_memory_color_ms_cs)},
    {FH1_SHADER(fh1_native_resolve_memory_depth_cs),
     FH1_SHADER(fh1_native_resolve_memory_depth_ms_cs)},
    {FH1_SHADER(fh1_native_resolve_memory_uint_cs),
     FH1_SHADER(fh1_native_resolve_memory_uint_ms_cs)},
};

#undef FH1_SHADER

uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

// Constant buffer sizes: the transfer and clear pixel shaders take 3 or 4
// 32-bit values in b0 (16 bytes, the minimum), the words and resolve compute
// shaders 8 (the Direct3D 12 root constants, in the same order).
constexpr uint32_t kSmallConstantsSize = 16;
constexpr uint32_t kComputeConstantCount = 8;

}  // namespace

struct Fh1NativeExecutor::Impl {
  Impl(D3D11CommandProcessor& command_processor, const RegisterFile& register_file,
       memory::Memory& memory)
      : command_processor_(command_processor),
        register_file_(register_file),
        memory_(memory),
        draw_extent_estimator_(register_file, memory),
        depth_overwrite_(register_file, memory) {}

  using SurfaceKey = Fh1SurfaceKey;
  struct Surface {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    // Color: the draw format's view; depth: the depth stencil view.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    // UINT render target view for formats kept as raw channel bits.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uint_rtv;
    // Transfer and resolve sources: color (raw bits for the UINT formats) or
    // depth, and the stencil plane of depth surfaces.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stencil_srv;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    SurfaceKey key;
    // Depth surfaces: whether any stencil value may be nonzero. Transfers
    // from a surface whose stencil is all zero skip the per-bit passes.
    bool stencil_nonzero = false;
    uint32_t samples = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;
  };
  struct PendingDump {
    uint64_t frame = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_gamma_pwl = false;
    std::array<uint32_t, 128 * 3> gamma_pwl = {};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  };
  // A rectangle of a resolve, in resolve surface pixels, whose tiles one
  // native surface owns.
  struct SourceRect {
    Surface* surface = nullptr;
    D3D11_RECT rect = {};
    // No surface: whether no surface owns the tiles, or the owner's surface is
    // gone.
    bool unowned = false;
  };

  static constexpr uint32_t kNoOwner = Fh1EdramTiles::kNoOwner;
  using TileRun = Fh1EdramTiles::Run;
  using OverwriteRect = Fh1DepthOverwrite::Rect;

  // readback_resolve: copies of resolved ranges into guest RAM, as the
  // Vulkan backend does (the guest GPU writes resolves to RAM, which the CPU
  // may read, e.g. to compress car thumbnails). Double-buffered per range for
  // the delayed modes.
  struct ResolveReadback {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffers[2];
    uint32_t sizes[2] = {};
    uint32_t current = 0;
    uint64_t last_used_frame = 0;
    // First frame of the current run of frames resolving this range, and of
    // the range being new (first resolved, or again after an idle time).
    uint64_t run_start_frame = 0;
    uint64_t new_since_frame = 0;
    // Resolves this frame (count_frame) and in the frame before it.
    uint64_t count_frame = UINT64_MAX;
    uint32_t count = 0;
    uint32_t previous_count = 0;
  };
  struct PendingReadback {
    uint32_t address;
    uint32_t length;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  };
  // Depth destinations' transfers, batched until FlushTransfers so each
  // destination takes its nine passes once for all of them.
  struct PendingTransfer {
    uint32_t dest;
    uint32_t source;
    D3D11_RECT rect;
    // Whether the transferred words' stencil (low byte) may be nonzero.
    bool stencil;
  };
  // A guest copy: its rectangle, destination and the native sources.
  struct CopyPlan : Fh1ResolvePlan {
    std::vector<SourceRect> sources;
  };
  struct PrepareSignature {
    uint64_t generation = 0;
    uint32_t used_bits = 0;
    uint32_t length_tiles = 0;
    bool stencil_written = false;
    uint32_t keys[5] = {};
    bool operator==(const PrepareSignature&) const = default;
  };
  // CPU time on the GPU command thread, per phase, since the last stats line.
  enum CpuPhase { kCpuPrepareTargets, kCpuTransfers, kCpuBindTargets, kCpuResolves, kCpuPhases };
  // Two clock reads per timed call, and target preparation and binding run
  // for every draw: about 3 % of the race's GPU commands thread on Direct3D 12
  // (NP-9.3), so the timers only run with fh1_native_gpu_profile.
  class CpuTimer {
   public:
    CpuTimer(Impl& impl, CpuPhase phase)
        : impl_(impl),
          phase_(phase),
          start_(impl.cpu_timing_ ? std::chrono::steady_clock::now().time_since_epoch().count()
                                  : 0) {}
    ~CpuTimer() {
      if (!impl_.cpu_timing_) return;
      impl_.cpu_ns_[phase_] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::duration(
              std::chrono::steady_clock::now().time_since_epoch().count() - start_))
                                            .count());
    }

   private:
    Impl& impl_;
    CpuPhase phase_;
    int64_t start_;
  };

  bool Initialize(const Fh1NativeExecutorConfig& config);
  void Shutdown();
  void PrepareTargets(const Fh1NativeDrawInfo& draw);
  void LogStats(uint64_t frame);
  bool BindTargets(uint32_t& bound_bits, uint32_t* formats, uint32_t& bound_width,
                   uint32_t& bound_height);
  void NativeDrawIssued(const Fh1NativeDrawInfo& draw);
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  void FlushResolveReadbacks();
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width, uint32_t height,
              const uint32_t* gamma_pwl);

  ID3D11Device* device() const { return command_processor_.GetDevice(); }
  ID3D11DeviceContext* context() const { return command_processor_.GetContext(); }

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const {
    return SurfaceKey::Color(base, pitch_tiles, msaa, format, config_.gamma_as_unorm16);
  }
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format) {
    return SurfaceKey::Depth(base, pitch_tiles, msaa, format);
  }
  static uint32_t PitchTiles(uint32_t pitch_pixels, uint32_t msaa) {
    return Fh1PitchTiles(pitch_pixels, msaa);
  }
  static uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) {
    return Fh1SurfaceHeight(pitch_tiles, msaa, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, 1);
  }
  DXGI_FORMAT ColorResourceFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT ColorDrawFormat(xenos::ColorRenderTargetFormat format) const;
  Surface* GetOrCreateSurface(const SurfaceKey& key);
  Surface* FindSurface(uint32_t packed_key) {
    auto it = surfaces_.find(packed_key);
    return it != surfaces_.end() ? &it->second : nullptr;
  }
  // Claims tiles [base, base + length) (with EDRAM address wrapping) for key
  // and transfers the previous owners' contents into the surface.
  // Without transfer, the tiles change owner without their contents (the
  // caller overwrites them, as a resolve clear does).
  void ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key, bool transfer = true);
  void MarkTileStencil(uint32_t base, uint32_t length, bool nonzero) {
    tiles_.MarkStencil(base, length, nonzero);
  }
  bool AnyTileStencil(uint32_t base, uint32_t count) const {
    return tiles_.AnyStencil(base, count);
  }
  // Whether the current draw's stencil state can leave a nonzero value.
  bool DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const;
  // Finds the draw's depth-overwrite rectangles (depth_overwrite_.rects()).
  // Whether stencil is rewritten too is returned separately.
  bool GetDepthOverwriteRects(const Fh1NativeDrawInfo& draw, bool& stencil_overwritten) {
    return depth_overwrite_.Derive(draw, REXCVAR_GET(half_pixel_offset), stencil_overwritten);
  }
  // Claims only the depth tiles the rectangles touch: covered tiles without a
  // transfer when no stencil would be lost, edge tiles with one. False when
  // the rectangles do not fit the surface's claimed range.
  bool ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length, bool stencil_overwritten,
                                bool stencil_written);
  // Claims the depth tiles a depth-overwriting draw covers without transfers,
  // when no stencil would be lost.
  void ClaimOverwrittenDepthTiles(const SurfaceKey& key, const std::array<int32_t, 4>& rect,
                                  bool stencil_overwritten);
  // Claims a rectangle of tiles with one transfer when it has a single
  // previous owner; per-row claims otherwise.
  void ClaimTileRect(const SurfaceKey& key, uint32_t column_first, uint32_t row_first,
                     uint32_t column_end, uint32_t row_end);
  void TransferTiles(Surface& dest, const TileRun& run);
  // Transfers destination pixel rectangles from the previous owner.
  // `tiles_stencil`: whether any transferred tile's stencil may be nonzero.
  void TransferRects(Surface& dest, uint32_t previous_owner, const D3D11_RECT* rects,
                     uint32_t rect_count, uint32_t tile_count, bool tiles_stencil);
  void FlushTransfers();
  void FlushColorTransfers(Surface& dest, size_t first, size_t end);
  void FlushDepthTransfers(Surface& dest, size_t first, size_t end);
  bool EnsureTransferWords(const Surface& dest);
  uint32_t LayoutConstant(const Surface& surface) const;
  // Flags shared by the transfer shaders (resolution scale bits 12:13 are 0).
  uint32_t TransferFlags() const {
    return (config_.depth_float24_round ? 1u : 0u) | (config_.gamma_as_unorm16 ? 2u : 0u) |
           (config_.fixed16_truncated ? 0u : 4u);
  }
  // Splits the resolve's pixel rectangle into rectangles per owning surface.
  void GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0, int32_t x1,
                         int32_t y1, std::vector<SourceRect>& sources_out);
  bool ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch);
  // False when there is nothing to do or the copy cannot be planned.
  bool PlanCopy(CopyPlan& plan);
  // Returns whether the copy ran completely.
  bool Resolve(uint32_t* written_address, uint32_t* written_length);
  void ClearSurfaceRect(Surface& surface, const D3D11_RECT& rect, uint32_t clear_value,
                        uint32_t clear_value_lo);
  // Drawn depth and stencil rectangle clear (feature level 11_0 clears have
  // no rectangles): depth from the viewport depth range, stencil from the
  // reference.
  void DrawDepthStencilClear(Surface& surface, const D3D11_RECT& rect, bool depth,
                             float depth_value, uint8_t stencil_value);
  void DrawColorClear(Surface& surface, const D3D11_RECT& rect, const float color[4]);
  void ReadBackResolve(uint32_t address, uint32_t length);
  // With readback_resolve = none: whether a resolve to the range is a one-off
  // capture to read back (fh1_native_readback_new_resolves), and queueing it.
  bool IsOneOffResolve(uint32_t address, uint32_t length);
  void QueueResolveReadback(uint32_t address, uint32_t length);
  Microsoft::WRL::ComPtr<ID3D11Buffer> CreateReadbackBuffer(uint32_t size);
  void QueueFrontBufferDump(uint64_t frame, uint32_t width, uint32_t height,
                            const uint32_t* gamma_pwl);
  void DrainDumps();

  // Pass helpers.
  // The fixed state every executor draw needs, set completely because the
  // guest draw before it may have left tessellation or geometry shaders,
  // vertex input and other rasterizer state bound.
  void BeginDrawPass();
  // Updates a dynamic constant buffer; nullptr on failure.
  ID3D11Buffer* UpdateConstants(ID3D11Buffer* buffer, const void* data, uint32_t size);
  ID3D11PixelShader* GetPixelShader(Microsoft::WRL::ComPtr<ID3D11PixelShader>& cached,
                                    const ShaderBytecode& bytecode);
  ID3D11ComputeShader* GetComputeShader(Microsoft::WRL::ComPtr<ID3D11ComputeShader>& cached,
                                        const ShaderBytecode& bytecode);
  void SetPassViewport(const D3D11_RECT& rect, float min_depth = 0.0f, float max_depth = 1.0f);
  uint32_t SampleMask(const Surface& dest) const {
    // 2x surfaces stored as 4x write only the samples the guest's 2x maps to.
    return dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                                 : UINT_MAX;
  }

  void Skip(const char* reason) { counters_.Skip(reason); }
  void Count(const char* stat) { counters_.Count(stat); }
  void LogOnce(uint64_t signature, const std::string& message) {
    if (logged_.size() >= 256 || !logged_.insert(signature).second) return;
    REXGPU_INFO("FH1 native executor {}", message);
  }

  D3D11CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  DrawExtentEstimator draw_extent_estimator_;
  Fh1DepthOverwrite depth_overwrite_;
  Fh1NativeExecutorConfig config_;

  // The command processor's mirror and texture cache.
  D3D11SharedMemory* native_memory_ = nullptr;
  D3D11TextureCache* native_textures_ = nullptr;

  // Shaders, created on first use.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreen_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> clear_color_ps_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> transfer_ps_[4][2][3][2];
  Microsoft::WRL::ComPtr<ID3D11PixelShader> from_words_ps_[2][2];
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> words_cs_[3][2];
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> resolve_cs_[3][2];
  // Fixed state objects of the passes. The Direct3D 12 pipelines differ only
  // by shaders, formats, sample count and mask, which Direct3D 11 binds
  // separately, so a handful of state objects covers them all.
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  // Color passes: no depth or stencil.
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_none_;
  // Depth transfer pass and depth clears: depth always written, stencil
  // replaced with the reference (0 for transfers, the clear value for
  // clears).
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_depth_;
  // One stencil bit replaced (reference 0xFF) where the pixel shader keeps
  // the pixel.
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_bit_[8];
  // Stencil-only clear (reference 0).
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_stencil_clear_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> small_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> compute_constants_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> transfer_words_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> transfer_words_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> transfer_words_uav_;
  uint32_t transfer_words_size_ = 0;

  // Targets PrepareTargets derived for the draw BindTargets binds.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  std::map<uint32_t, Surface> surfaces_;
  // EDRAM tile -> packed key of the surface that last wrote it, and whether
  // the low byte of its words (stencil when read as depth) may be nonzero.
  Fh1EdramTiles tiles_;
  std::vector<PendingTransfer> pending_transfers_;
  PrepareSignature last_prepare_;
  std::map<uint64_t, ResolveReadback> resolve_readbacks_;
  std::vector<PendingReadback> pending_readbacks_;
  std::deque<PendingDump> dumps_;
  std::set<uint64_t> dump_frames_;
  std::filesystem::path dump_directory_;
  uint64_t frame_ = 0;
  Fh1ExecutorCounters counters_;
  std::set<uint64_t> logged_;
  uint64_t draws_ = 0;
  std::array<uint64_t, kCpuPhases> cpu_ns_{};
  uint64_t cpu_frames_ = 0;
  // fh1_native_gpu_profile: whether CpuTimer measures (GPU timestamps are not
  // implemented on Direct3D 11).
  bool cpu_timing_ = false;
  // Profiling: transferred tile-passes by source -> destination surface.
  std::map<std::string, uint64_t> transfer_volume_;
  uint64_t resolves_ = 0;
  bool initialized_ = false;
};

bool Fh1NativeExecutor::Impl::Initialize(const Fh1NativeExecutorConfig& config) {
  config_ = config;
  native_memory_ = config_.memory;
  native_textures_ = config_.textures;
  if (!native_memory_ || !native_textures_) return false;
  if (native_textures_->draw_resolution_scale_x() != 1 ||
      native_textures_->draw_resolution_scale_y() != 1) {
    // The Direct3D 11 backend is for old hardware and keeps the guest
    // resolution: rectangles, transfers and resolves here are unscaled.
    REXGPU_ERROR("FH1 native executor (D3D11): unsupported resolution scale {}x{}",
                 native_textures_->draw_resolution_scale_x(),
                 native_textures_->draw_resolution_scale_y());
    return false;
  }
  ID3D11Device* d = device();
  if (FAILED(d->CreateVertexShader(shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs),
                                   nullptr, &fullscreen_vs_)) ||
      FAILED(d->CreatePixelShader(shaders::fh1_native_clear_color_ps,
                                  sizeof(shaders::fh1_native_clear_color_ps), nullptr,
                                  &clear_color_ps_))) {
    REXGPU_ERROR("FH1 native executor (D3D11): failed to create the pass shaders");
    return false;
  }

  D3D11_RASTERIZER_DESC rasterizer = {};
  rasterizer.FillMode = D3D11_FILL_SOLID;
  rasterizer.CullMode = D3D11_CULL_NONE;
  rasterizer.DepthClipEnable = FALSE;
  // Passes are limited to their rectangles by the scissor, which Direct3D 11
  // applies only when enabled in the rasterizer state.
  rasterizer.ScissorEnable = TRUE;
  rasterizer.MultisampleEnable = TRUE;
  if (FAILED(d->CreateRasterizerState(&rasterizer, &rasterizer_state_))) return false;

  D3D11_DEPTH_STENCIL_DESC depth_stencil = {};
  depth_stencil.DepthEnable = FALSE;
  depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil.StencilEnable = FALSE;
  depth_stencil.StencilReadMask = 0xFF;
  depth_stencil.StencilWriteMask = 0xFF;
  depth_stencil.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  depth_stencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  depth_stencil.BackFace = depth_stencil.FrontFace;
  if (FAILED(d->CreateDepthStencilState(&depth_stencil, &depth_stencil_none_))) return false;
  depth_stencil.StencilEnable = TRUE;
  if (FAILED(d->CreateDepthStencilState(&depth_stencil, &depth_stencil_stencil_clear_))) {
    return false;
  }
  for (uint32_t bit = 0; bit < 8; ++bit) {
    depth_stencil.StencilWriteMask = uint8_t(1u << bit);
    if (FAILED(d->CreateDepthStencilState(&depth_stencil, &depth_stencil_bit_[bit]))) {
      return false;
    }
  }
  depth_stencil.DepthEnable = TRUE;
  depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth_stencil.StencilWriteMask = 0xFF;
  if (FAILED(d->CreateDepthStencilState(&depth_stencil, &depth_stencil_depth_))) return false;

  D3D11_BUFFER_DESC constants = {};
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  constants.ByteWidth = kSmallConstantsSize;
  if (FAILED(d->CreateBuffer(&constants, nullptr, &small_constants_))) return false;
  constants.ByteWidth = kComputeConstantCount * sizeof(uint32_t);
  if (FAILED(d->CreateBuffer(&constants, nullptr, &compute_constants_))) return false;

  tiles_.Reset();
  std::stringstream frames(REXCVAR_GET(fh1_native_dump_frames));
  std::string frame;
  while (std::getline(frames, frame, ',')) {
    if (!frame.empty()) dump_frames_.insert(std::strtoull(frame.c_str(), nullptr, 10));
  }
  dump_directory_ = std::filesystem::path(REXCVAR_GET(fh1_native_dump_dir));
  cpu_timing_ = REXCVAR_GET(fh1_native_gpu_profile);
  if (cpu_timing_) {
    REXGPU_INFO(
        "FH1 native executor (D3D11): fh1_native_gpu_profile measures CPU phases only; GPU "
        "timestamps are not implemented on Direct3D 11");
  }
  initialized_ = true;
  REXGPU_INFO("FH1 native executor enabled: native D3D11 ({} dump frames)", dump_frames_.size());
  return true;
}

void Fh1NativeExecutor::Impl::Shutdown() {
  if (initialized_) LogStats(frame_);
  initialized_ = false;
  dumps_.clear();
  pending_readbacks_.clear();
  resolve_readbacks_.clear();
  pending_transfers_.clear();
  surfaces_.clear();
  native_textures_ = nullptr;
  native_memory_ = nullptr;
  transfer_words_uav_.Reset();
  transfer_words_srv_.Reset();
  transfer_words_.Reset();
  transfer_words_size_ = 0;
  for (auto& a : transfer_ps_) {
    for (auto& b : a) {
      for (auto& c : b) {
        for (auto& shader : c) shader.Reset();
      }
    }
  }
  for (auto& a : from_words_ps_) {
    for (auto& shader : a) shader.Reset();
  }
  for (auto& a : words_cs_) {
    for (auto& shader : a) shader.Reset();
  }
  for (auto& a : resolve_cs_) {
    for (auto& shader : a) shader.Reset();
  }
  clear_color_ps_.Reset();
  fullscreen_vs_.Reset();
  rasterizer_state_.Reset();
  depth_stencil_none_.Reset();
  depth_stencil_depth_.Reset();
  for (auto& state : depth_stencil_bit_) state.Reset();
  depth_stencil_stencil_clear_.Reset();
  small_constants_.Reset();
  compute_constants_.Reset();
}

ID3D11PixelShader* Fh1NativeExecutor::Impl::GetPixelShader(
    Microsoft::WRL::ComPtr<ID3D11PixelShader>& cached, const ShaderBytecode& bytecode) {
  if (!cached &&
      FAILED(device()->CreatePixelShader(bytecode.data, bytecode.size, nullptr, &cached))) {
    return nullptr;
  }
  return cached.Get();
}

ID3D11ComputeShader* Fh1NativeExecutor::Impl::GetComputeShader(
    Microsoft::WRL::ComPtr<ID3D11ComputeShader>& cached, const ShaderBytecode& bytecode) {
  if (!cached &&
      FAILED(device()->CreateComputeShader(bytecode.data, bytecode.size, nullptr, &cached))) {
    return nullptr;
  }
  return cached.Get();
}

ID3D11Buffer* Fh1NativeExecutor::Impl::UpdateConstants(ID3D11Buffer* buffer, const void* data,
                                                       uint32_t size) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context()->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return nullptr;
  std::memcpy(mapped.pData, data, size);
  context()->Unmap(buffer, 0);
  return buffer;
}

void Fh1NativeExecutor::Impl::BeginDrawPass() {
  ID3D11DeviceContext* ctx = context();
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->RSSetState(rasterizer_state_.Get());
}

void Fh1NativeExecutor::Impl::SetPassViewport(const D3D11_RECT& rect, float min_depth,
                                              float max_depth) {
  // Through the command processor, which shadows the fixed-function state
  // guest draws set.
  D3D11_VIEWPORT viewport = {float(rect.left),
                             float(rect.top),
                             float(rect.right - rect.left),
                             float(rect.bottom - rect.top),
                             min_depth,
                             max_depth};
  command_processor_.SetViewport(viewport);
  command_processor_.SetScissorRect(rect);
}

DXGI_FORMAT Fh1NativeExecutor::Impl::ColorResourceFormat(
    xenos::ColorRenderTargetFormat format) const {
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

DXGI_FORMAT Fh1NativeExecutor::Impl::ColorDrawFormat(xenos::ColorRenderTargetFormat format) const {
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

Fh1NativeExecutor::Impl::Surface* Fh1NativeExecutor::Impl::GetOrCreateSurface(
    const SurfaceKey& key) {
  const uint32_t packed = key.Pack();
  if (Surface* existing = FindSurface(packed)) return existing;
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  Surface surface;
  surface.key = key;
  surface.width = key.pitch_tiles * (xenos::kEdramTileWidthSamples >> msaa_x_log2);
  surface.height = SurfaceHeight(key.pitch_tiles, key.msaa);
  surface.samples = key.msaa == uint32_t(xenos::MsaaSamples::k2X) && !config_.msaa_2x_supported
                        ? 4
                        : 1u << key.msaa;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = surface.width;
  desc.Height = surface.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.SampleDesc.Count = surface.samples;
  desc.Usage = D3D11_USAGE_DEFAULT;
  DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;
  if (key.is_depth) {
    const bool float24 =
        xenos::DepthRenderTargetFormat(key.format) == xenos::DepthRenderTargetFormat::kD24FS8;
    // As the Direct3D 12 host render config: float24 depth is kept in a
    // 32-bit float (halved, in [0, 1)), unorm24 in D24.
    desc.Format = float24 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    surface.view_format = float24 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    surface.srv_format =
        float24 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    stencil_srv_format =
        float24 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  } else {
    const auto format = xenos::ColorRenderTargetFormat(key.format);
    desc.Format = ColorResourceFormat(format);
    surface.view_format = ColorDrawFormat(format);
    const DXGI_FORMAT uint_format = ColorUintFormat(format);
    surface.srv_format = uint_format != DXGI_FORMAT_UNKNOWN ? uint_format : surface.view_format;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN || !surface.width || !surface.height) {
    Skip("surface_format");
    return nullptr;
  }
  ID3D11Device* d = device();
  const bool msaa = surface.samples > 1;
  if (msaa) {
    UINT quality_levels = 0;
    if (FAILED(d->CheckMultisampleQualityLevels(surface.view_format, surface.samples,
                                                &quality_levels)) ||
        !quality_levels) {
      Skip("surface_msaa_unsupported");
      return nullptr;
    }
  }
  if (FAILED(d->CreateTexture2D(&desc, nullptr, &surface.texture))) {
    Skip("surface_create");
    return nullptr;
  }
  surface.bytes =
      uint64_t(surface.width) * surface.height * surface.samples * FormatBytes(desc.Format);
  D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = surface.srv_format;
  if (msaa) {
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = 1;
  }
  bool views_ok = SUCCEEDED(d->CreateShaderResourceView(surface.texture.Get(), &srv, &surface.srv));
  if (key.is_depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = surface.view_format;
    dsv.ViewDimension = msaa ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    views_ok = views_ok &&
               SUCCEEDED(d->CreateDepthStencilView(surface.texture.Get(), &dsv, &surface.dsv));
    srv.Format = stencil_srv_format;
    views_ok = views_ok && SUCCEEDED(d->CreateShaderResourceView(surface.texture.Get(), &srv,
                                                                 &surface.stencil_srv));
  } else {
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = surface.view_format;
    rtv.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    views_ok =
        views_ok && SUCCEEDED(d->CreateRenderTargetView(surface.texture.Get(), &rtv, &surface.rtv));
    if (surface.srv_format != surface.view_format) {
      // Transfers write raw channel bits.
      rtv.Format = surface.srv_format;
      views_ok = views_ok && SUCCEEDED(d->CreateRenderTargetView(surface.texture.Get(), &rtv,
                                                                 &surface.uint_rtv));
    }
  }
  if (!views_ok) {
    Skip("surface_view_create");
    return nullptr;
  }
  // Render targets must be initialized before use; zero is also what the
  // stencil tracking assumes for a new surface.
  if (key.is_depth) {
    context()->ClearDepthStencilView(surface.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     0.0f, 0);
  } else {
    const float zero[4] = {};
    context()->ClearRenderTargetView(surface.rtv.Get(), zero);
  }
  Count("surface_created");
  return &surfaces_.emplace(packed, std::move(surface)).first->second;
}

void Fh1NativeExecutor::Impl::ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key,
                                         bool transfer) {
  const auto runs = tiles_.Claim(base, length, packed_key, transfer);
  if (runs.empty()) return;
  Surface* dest = FindSurface(packed_key);
  if (!dest) return;
  for (const TileRun& run : runs) TransferTiles(*dest, run);
}

uint32_t Fh1NativeExecutor::Impl::LayoutConstant(const Surface& surface) const {
  const SurfaceKey& key = surface.key;
  const uint32_t host_sample_mode =
      key.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  return PackLayout(key.base_tiles, key.pitch_tiles, key.msaa, key.Is64bpp(), key.is_depth,
                    key.format) |
         (host_sample_mode << 27);
}

void Fh1NativeExecutor::Impl::TransferTiles(Surface& dest, const TileRun& run) {
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
  D3D11_RECT rects[3];
  uint32_t rect_count = 0;
  auto add_rect = [&](uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    D3D11_RECT rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
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
  if (rect_count) {
    TransferRects(dest, run.previous_owner, rects, rect_count, run.count,
                  AnyTileStencil(run.first, run.count));
  }
}

void Fh1NativeExecutor::Impl::ClaimTileRect(const SurfaceKey& key, uint32_t column_first,
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
  const D3D11_RECT rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
                           LONG(std::min(column_end * tile_width, dest->width)),
                           LONG(std::min(row_end * tile_height, dest->height))};
  if (rect.left < rect.right && rect.top < rect.bottom) {
    bool tiles_stencil = false;
    for (uint32_t row = row_first; row < row_end && !tiles_stencil; ++row) {
      tiles_stencil = AnyTileStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                                     column_end - column_first);
    }
    TransferRects(*dest, previous_owner, &rect, 1,
                  (column_end - column_first) * (row_end - row_first), tiles_stencil);
  }
}

void Fh1NativeExecutor::Impl::TransferRects(Surface& dest, uint32_t previous_owner,
                                            const D3D11_RECT* rects, uint32_t rect_count,
                                            uint32_t tile_count, bool tiles_stencil) {
  CpuTimer timer(*this, kCpuTransfers);
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
  if (cpu_timing_) {
    transfer_volume_[source->key.Describe() + "->" + dest.key.Describe()] +=
        uint64_t(tile_count) * pass_count;
  }
  for (uint32_t i = 0; i < rect_count; ++i) {
    pending_transfers_.push_back({dest.key.Pack(), previous_owner, rects[i], source_stencil});
  }
}

void Fh1NativeExecutor::Impl::FlushTransfers() {
  if (pending_transfers_.empty()) return;
  CpuTimer timer(*this, kCpuTransfers);
  // Grouped by destination, then by source, keeping each group's order.
  std::stable_sort(pending_transfers_.begin(), pending_transfers_.end(),
                   [](const PendingTransfer& a, const PendingTransfer& b) {
                     return a.dest != b.dest ? a.dest < b.dest : a.source < b.source;
                   });
  for (size_t group = 0; group < pending_transfers_.size();) {
    size_t group_end = group;
    while (group_end < pending_transfers_.size() &&
           pending_transfers_[group_end].dest == pending_transfers_[group].dest) {
      ++group_end;
    }
    Surface* dest = FindSurface(pending_transfers_[group].dest);
    if (!dest) {
      Skip("transfer_words_buffer");
    } else if (dest->key.is_depth) {
      FlushDepthTransfers(*dest, group, group_end);
    } else {
      FlushColorTransfers(*dest, group, group_end);
    }
    group = group_end;
  }
  pending_transfers_.clear();
  // The passes replaced the guest draw's shaders, views, constants and state.
  command_processor_.InvalidateGuestDrawState();
}

void Fh1NativeExecutor::Impl::FlushColorTransfers(Surface& dest, size_t first, size_t end) {
  ID3D11DeviceContext* ctx = context();
  const uint32_t flags = TransferFlags();
  const bool dest_uint = dest.uint_rtv != nullptr;
  // The destination first: binding it as the only output unbinds whatever the
  // last guest draw left as targets, which may include a source, whose shader
  // resource view the runtime would otherwise refuse.
  ID3D11RenderTargetView* rtv = dest_uint ? dest.uint_rtv.Get() : dest.rtv.Get();
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  BeginDrawPass();
  ctx->OMSetDepthStencilState(depth_stencil_none_.Get(), 0);
  ctx->OMSetBlendState(nullptr, nullptr, SampleMask(dest));
  const uint32_t dest_kind = dest_uint ? 3 : 0;
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    if (!source) {
      Skip("transfer_descriptor");
      i = source_end;
      continue;
    }
    const uint32_t source_kind = SourceKind(source->key.is_depth, source->key.format);
    const bool source_msaa = source->samples > 1;
    ID3D11PixelShader* shader =
        GetPixelShader(transfer_ps_[dest_kind][dest.samples > 1][source_kind][source_msaa],
                       kTransferShaders[dest_kind][dest.samples > 1][source_kind][source_msaa]);
    const uint32_t constants[4] = {LayoutConstant(dest), LayoutConstant(*source), flags, 0};
    ID3D11Buffer* constant_buffer =
        shader ? UpdateConstants(small_constants_.Get(), constants, sizeof(constants)) : nullptr;
    if (!constant_buffer) {
      Skip("transfer_pipeline");
      i = source_end;
      continue;
    }
    ctx->PSSetShader(shader, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &constant_buffer);
    ID3D11ShaderResourceView* srvs[2] = {
        source->srv.Get(), source->key.is_depth ? source->stencil_srv.Get() : source->srv.Get()};
    ctx->PSSetShaderResources(0, 2, srvs);
    for (; i < source_end; ++i) {
      SetPassViewport(pending_transfers_[i].rect);
      ctx->Draw(3, 0);
    }
  }
  ID3D11ShaderResourceView* null_srvs[2] = {};
  ctx->PSSetShaderResources(0, 2, null_srvs);
  Count("transfer_batch");
}

void Fh1NativeExecutor::Impl::FlushDepthTransfers(Surface& dest, size_t first, size_t end) {
  if (!EnsureTransferWords(dest)) {
    Skip("transfer_words_buffer");
    return;
  }
  ID3D11DeviceContext* ctx = context();
  const uint32_t flags = TransferFlags();
  // Words of every rectangle, one source at a time. No render targets: a
  // source may still be bound as the last guest draw's target.
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ID3D11UnorderedAccessView* words_uav = transfer_words_uav_.Get();
  ctx->CSSetUnorderedAccessViews(0, 1, &words_uav, nullptr);
  bool any_stencil = false;
  for (size_t i = first; i < end; ++i) any_stencil |= pending_transfers_[i].stencil;
  for (size_t i = first; i < end;) {
    size_t source_end = i;
    while (source_end < end &&
           pending_transfers_[source_end].source == pending_transfers_[i].source) {
      ++source_end;
    }
    Surface* source = FindSurface(pending_transfers_[i].source);
    if (!source) {
      i = source_end;
      continue;
    }
    const uint32_t source_kind = SourceKind(source->key.is_depth, source->key.format);
    const bool source_msaa = source->samples > 1;
    ID3D11ComputeShader* shader = GetComputeShader(words_cs_[source_kind][source_msaa],
                                                   kWordsShaders[source_kind][source_msaa]);
    if (!shader) {
      Skip("transfer_words_pipeline");
      i = source_end;
      continue;
    }
    ctx->CSSetShader(shader, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {
        source->srv.Get(), source->key.is_depth ? source->stencil_srv.Get() : source->srv.Get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    for (; i < source_end; ++i) {
      const D3D11_RECT& rect = pending_transfers_[i].rect;
      const uint32_t words_constants[kComputeConstantCount] = {
          uint32_t(rect.left) | (uint32_t(rect.top) << 16),
          uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16),
          LayoutConstant(dest),
          LayoutConstant(*source),
          flags,
          dest.width,
          dest.samples,
          0};
      ID3D11Buffer* constant_buffer =
          UpdateConstants(compute_constants_.Get(), words_constants, sizeof(words_constants));
      if (!constant_buffer) {
        Skip("transfer_words_constants");
        continue;
      }
      ctx->CSSetConstantBuffers(0, 1, &constant_buffer);
      ctx->Dispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                    (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
    }
  }
  // The words buffer is read next as a shader resource, which the runtime
  // refuses while its UAV is bound.
  command_processor_.UnbindComputeResources(2, 1, 1);

  ctx->OMSetRenderTargets(0, nullptr, dest.dsv.Get());
  BeginDrawPass();
  ctx->OMSetBlendState(nullptr, nullptr, SampleMask(dest));
  ID3D11ShaderResourceView* words_srvs[2] = {transfer_words_srv_.Get(),
                                             transfer_words_srv_.Get()};
  ctx->PSSetShaderResources(0, 2, words_srvs);
  // The depth pass (which resets stencil to 0), then one pass per stencil
  // bit over the rectangles whose source may have nonzero stencil.
  for (uint32_t pass = 0; pass < (any_stencil ? 9u : 1u); ++pass) {
    ID3D11PixelShader* shader =
        GetPixelShader(from_words_ps_[pass ? 1 : 0][dest.samples > 1],
                       kFromWordsShaders[pass ? 1 : 0][dest.samples > 1]);
    const uint32_t pass_constants[4] = {LayoutConstant(dest), dest.width | (dest.samples << 16),
                                        flags | (pass ? (pass - 1) << 8 : 0), 0};
    ID3D11Buffer* constant_buffer =
        shader ? UpdateConstants(small_constants_.Get(), pass_constants, sizeof(pass_constants))
               : nullptr;
    if (!constant_buffer) {
      Skip("transfer_pipeline");
      break;
    }
    ctx->PSSetShader(shader, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &constant_buffer);
    ctx->OMSetDepthStencilState(
        pass ? depth_stencil_bit_[pass - 1].Get() : depth_stencil_depth_.Get(), pass ? 0xFF : 0);
    for (size_t i = first; i < end; ++i) {
      if (pass && !pending_transfers_[i].stencil) continue;
      SetPassViewport(pending_transfers_[i].rect);
      ctx->Draw(3, 0);
    }
  }
  ID3D11ShaderResourceView* null_srvs[2] = {};
  ctx->PSSetShaderResources(0, 2, null_srvs);
  Count("transfer_batch");
}

bool Fh1NativeExecutor::Impl::EnsureTransferWords(const Surface& dest) {
  const uint64_t size = uint64_t(dest.width) * dest.height * dest.samples * sizeof(uint32_t);
  if (transfer_words_ && transfer_words_size_ >= size) return true;
  // The runtime keeps the outgrown buffer alive while queued work uses it.
  transfer_words_uav_.Reset();
  transfer_words_srv_.Reset();
  transfer_words_.Reset();
  transfer_words_size_ = 0;
  if (size > D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024ull * 1024ull) {
    return false;
  }
  ID3D11Device* d = device();
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(size);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(d->CreateBuffer(&desc, nullptr, &transfer_words_))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R32_TYPELESS;
  srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv.BufferEx.FirstElement = 0;
  srv.BufferEx.NumElements = UINT(size / 4);
  srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = 0;
  uav.Buffer.NumElements = UINT(size / 4);
  uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(d->CreateShaderResourceView(transfer_words_.Get(), &srv, &transfer_words_srv_)) ||
      FAILED(d->CreateUnorderedAccessView(transfer_words_.Get(), &uav, &transfer_words_uav_))) {
    transfer_words_uav_.Reset();
    transfer_words_srv_.Reset();
    transfer_words_.Reset();
    return false;
  }
  transfer_words_size_ = uint32_t(size);
  return true;
}

void Fh1NativeExecutor::Impl::GetResolveSources(const SurfaceKey& resolve_key, int32_t x0,
                                                int32_t y0, int32_t x1, int32_t y1,
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
    source.rect = {LONG(rect.left), LONG(rect.top), LONG(rect.right), LONG(rect.bottom)};
    source.unowned = rect.owner == kNoOwner;
    source.surface = source.unowned ? nullptr : FindSurface(rect.owner);
    sources_out.push_back(source);
  }
}

bool Fh1NativeExecutor::Impl::DrawMayWriteNonzeroStencil(
    reg::RB_DEPTHCONTROL depth_control) const {
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

bool Fh1NativeExecutor::Impl::ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length,
                                                       bool stencil_overwritten,
                                                       bool stencil_written) {
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

void Fh1NativeExecutor::Impl::ClaimOverwrittenDepthTiles(const SurfaceKey& key,
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
    const D3D11_RECT clear_rect = {LONG(column_first * tile_width), LONG(row_first * tile_height),
                                   LONG(std::min(column_end * tile_width, dest->width)),
                                   LONG(std::min(row_end * tile_height, dest->height))};
    DrawDepthStencilClear(*dest, clear_rect, false, 0.0f, 0);
  }
  Count("depth_overwrite_claim");
}

void Fh1NativeExecutor::Impl::PrepareTargets(const Fh1NativeDrawInfo& draw) {
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
  if (depth_stencil_written) {
    // The depth surface may have failed to be created above.
    if (Surface* depth = FindSurface(keys[0].Pack())) depth->stencil_nonzero = true;
  }
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

bool Fh1NativeExecutor::Impl::BindTargets(uint32_t& bound_bits, uint32_t* formats,
                                          uint32_t& bound_width, uint32_t& bound_height) {
  bound_bits = 0;
  bound_width = 0;
  bound_height = 0;
  if (!initialized_) return false;
  CpuTimer timer(*this, kCpuBindTargets);
  if (!pending_targets_valid_) {
    Skip("draw_targets_not_prepared");
    return false;
  }
  const uint32_t used_bits = pending_used_bits_;
  const SurfaceKey* keys = pending_keys_;
  // Gaps between bound color slots stay null, which Direct3D 11 allows
  // without null views.
  ID3D11RenderTargetView* rtvs[xenos::kMaxColorRenderTargets] = {};
  uint32_t rtv_count = 0;
  ID3D11DepthStencilView* dsv = nullptr;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) continue;
    Surface* surface = FindSurface(keys[i].Pack());
    if (!surface) {
      // PrepareTargets could not create it.
      Skip("draw_surface_create");
      return false;
    }
    if (i == 0) {
      dsv = surface->dsv.Get();
    } else {
      rtvs[i - 1] = surface->rtv.Get();
      rtv_count = i;
    }
    formats[i] = keys[i].format;
    bound_bits |= 1u << i;
    bound_width = bound_width ? std::min(bound_width, surface->width) : surface->width;
    bound_height = bound_height ? std::min(bound_height, surface->height) : surface->height;
  }
  context()->OMSetRenderTargets(rtv_count, rtv_count ? rtvs : nullptr, dsv);
  if (!used_bits) Count("draw_writes_nothing");
  return true;
}

void Fh1NativeExecutor::Impl::NativeDrawIssued(const Fh1NativeDrawInfo& draw) {
  if (!initialized_) return;
  pending_targets_valid_ = false;
  if (draw.occlusion_query_active) Count("draw_in_occlusion_query");
  ++draws_;
}

void Fh1NativeExecutor::Impl::DrawDepthStencilClear(Surface& surface, const D3D11_RECT& rect,
                                                    bool depth, float depth_value,
                                                    uint8_t stencil_value) {
  // No pixel shader: the depth written is the full-screen triangle's z (0)
  // mapped into the viewport depth range, so a range of [value, value] writes
  // the value as a clear would, converted the same way.
  ID3D11DeviceContext* ctx = context();
  ctx->OMSetRenderTargets(0, nullptr, surface.dsv.Get());
  BeginDrawPass();
  ctx->PSSetShader(nullptr, nullptr, 0);
  ctx->OMSetBlendState(nullptr, nullptr, UINT_MAX);
  ctx->OMSetDepthStencilState(
      depth ? depth_stencil_depth_.Get() : depth_stencil_stencil_clear_.Get(), stencil_value);
  SetPassViewport(rect, depth ? depth_value : 0.0f, depth ? depth_value : 1.0f);
  ctx->Draw(3, 0);
  command_processor_.InvalidateGuestDrawState();
}

void Fh1NativeExecutor::Impl::DrawColorClear(Surface& surface, const D3D11_RECT& rect,
                                             const float color[4]) {
  ID3D11DeviceContext* ctx = context();
  ID3D11Buffer* constant_buffer =
      UpdateConstants(small_constants_.Get(), color, kSmallConstantsSize);
  if (!constant_buffer) return Skip("clear_constants");
  ID3D11RenderTargetView* rtv = surface.rtv.Get();
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  BeginDrawPass();
  ctx->PSSetShader(clear_color_ps_.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, &constant_buffer);
  ctx->OMSetBlendState(nullptr, nullptr, UINT_MAX);
  ctx->OMSetDepthStencilState(depth_stencil_none_.Get(), 0);
  SetPassViewport(rect);
  ctx->Draw(3, 0);
  command_processor_.InvalidateGuestDrawState();
}

void Fh1NativeExecutor::Impl::ClearSurfaceRect(Surface& surface, const D3D11_RECT& guest_rect,
                                               uint32_t clear_value, uint32_t clear_value_lo) {
  // Clipped to the surface, as the Direct3D 12 rectangle clear does.
  const D3D11_RECT rect = {std::max(guest_rect.left, LONG(0)), std::max(guest_rect.top, LONG(0)),
                           std::min(guest_rect.right, LONG(surface.width)),
                           std::min(guest_rect.bottom, LONG(surface.height))};
  if (rect.left >= rect.right || rect.top >= rect.bottom) return;
  // Whole-surface clears can use the view clears; resolve clears rarely are,
  // since surfaces span the whole EDRAM height.
  const bool whole = rect.left == 0 && rect.top == 0 && rect.right == LONG(surface.width) &&
                     rect.bottom == LONG(surface.height);
  if (surface.key.is_depth) {
    // The host keeps float24 depth halved in [0, 1) (as the Xenos render
    // target cache did).
    const uint32_t depth_bits = (clear_value >> 8) & 0xFFFFFF;
    const float value = xenos::DepthRenderTargetFormat(surface.key.format) ==
                                xenos::DepthRenderTargetFormat::kD24FS8
                            ? xenos::Float20e4To32(depth_bits) * 0.5f
                            : xenos::UNorm24To32(depth_bits);
    surface.stencil_nonzero |= (clear_value & 0xFF) != 0;
    if (whole) {
      context()->ClearDepthStencilView(surface.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                       value, uint8_t(clear_value & 0xFF));
    } else {
      DrawDepthStencilClear(surface, rect, true, value, uint8_t(clear_value & 0xFF));
    }
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
  if (whole) {
    context()->ClearRenderTargetView(surface.rtv.Get(), color);
  } else {
    DrawColorClear(surface, rect, color);
  }
}

bool Fh1NativeExecutor::Impl::ResolveToMemory(const SourceRect& source,
                                              const SurfaceKey& resolve_key,
                                              uint32_t sample_select, uint32_t dest_info,
                                              uint32_t dest_base, uint32_t dest_pitch) {
  Surface& surface = *source.surface;
  const bool depth = surface.key.is_depth;
  const bool msaa = surface.samples > 1;
  const uint32_t source_kind = SourceKind(depth, surface.key.format);
  ID3D11ComputeShader* shader =
      GetComputeShader(resolve_cs_[source_kind][msaa], kResolveShaders[source_kind][msaa]);
  if (!shader) {
    Skip("resolve_pipeline_create");
    return false;
  }
  const SurfaceKey& owner = surface.key;
  const uint32_t host_sample_mode =
      owner.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  const D3D11_RECT& rect = source.rect;
#if REX_D3D11_FH1_RESOLVE_DUMP
  if (!REXCVAR_GET(fh1_resolve_dump_dir).empty()) {
    REXGPU_INFO(
        "FH1 resolve dump: source {} format {} samples {} kind {} rect {},{} {}x{} -> dest "
        "{:08X} info {:08X} pitch {}",
        depth ? "depth" : "color", owner.format, surface.samples, source_kind, rect.left,
        rect.top, rect.right - rect.left, rect.bottom - rect.top, dest_base, dest_info,
        dest_pitch);
  }
#endif
  uint32_t constants[kComputeConstantCount];
  constants[0] = uint32_t(rect.left) | (uint32_t(rect.top) << 16);
  constants[1] = uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
  constants[2] = PackLayout(resolve_key.base_tiles, resolve_key.pitch_tiles, resolve_key.msaa,
                            resolve_key.Is64bpp(), resolve_key.is_depth, resolve_key.format);
  constants[3] = PackLayout(owner.base_tiles, owner.pitch_tiles, owner.msaa, owner.Is64bpp(),
                            owner.is_depth, owner.format) |
                 (host_sample_mode << 27);
  constants[4] = sample_select;
  // Resolution scale bits 20:21 and the unscaled-destination bit 22 stay 0.
  constants[5] = dest_info;
  constants[6] = dest_base;
  constants[7] = dest_pitch;
  ID3D11Buffer* constant_buffer =
      UpdateConstants(compute_constants_.Get(), constants, sizeof(constants));
  if (!constant_buffer) {
    Skip("resolve_constants");
    return false;
  }
  ID3D11DeviceContext* ctx = context();
  // The source may still be the last guest draw's target, whose shader
  // resource view the runtime would refuse while it is bound for output.
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ctx->CSSetShader(shader, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, &constant_buffer);
  ID3D11ShaderResourceView* srvs[2] = {surface.srv.Get(),
                                       depth ? surface.stencil_srv.Get() : surface.srv.Get()};
  ctx->CSSetShaderResources(0, 2, srvs);
  // Binding the mirror for writing unbinds its shader resource views and the
  // index buffer everywhere; the guest draw state is invalidated for that.
  ID3D11UnorderedAccessView* memory_uav = native_memory_->GetRawUAV();
  ctx->CSSetUnorderedAccessViews(0, 1, &memory_uav, nullptr);
  ctx->Dispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
  command_processor_.UnbindComputeResources(2, 1, 1);
  command_processor_.InvalidateGuestDrawState();
  return true;
}

bool Fh1NativeExecutor::Impl::PlanCopy(CopyPlan& plan) {
  plan = CopyPlan();
  const Fh1ResolveFlags flags{config_.depth_float24_round, config_.gamma_as_unorm16,
                              config_.fixed16_truncated};
  if (!Fh1PlanResolve(register_file_, memory_, flags, plan)) return false;
  if (plan.copy) {
    GetResolveSources(plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1, plan.sources);
  }
  return true;
}

bool Fh1NativeExecutor::Impl::NativeResolve(uint32_t& written_address,
                                            uint32_t& written_length) {
  written_address = 0;
  written_length = 0;
  if (!initialized_) return false;
  if (!Resolve(&written_address, &written_length)) return false;
  if (written_length) ReadBackResolve(written_address, written_length);
#if REX_D3D11_FH1_RESOLVE_DUMP
  // As on Vulkan: every resolve's output bytes, waiting for the GPU after
  // each, to compare backends resolve by resolve.
  if (written_length && !REXCVAR_GET(fh1_resolve_dump_dir).empty() &&
      (!REXCVAR_GET(fh1_resolve_dump_frame) ||
       frame_ == uint64_t(REXCVAR_GET(fh1_resolve_dump_frame)))) {
    static uint32_t sequence = 0;
    std::vector<uint8_t> bytes;
    auto lock = command_processor_.GetD3D11Provider().LockImmediateContext();
    if (command_processor_.ReadbackSharedMemory(written_address, written_length, bytes)) {
      char name[64];
      std::snprintf(name, sizeof(name), "%05u_%08X_%u.bin", sequence++, written_address,
                    written_length);
      std::ofstream(std::filesystem::path(REXCVAR_GET(fh1_resolve_dump_dir)) / name,
                    std::ios::binary)
          .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
  }
#endif
  return true;
}

bool Fh1NativeExecutor::Impl::IsOneOffResolve(uint32_t address, uint32_t length) {
  // Resolves to a range come in runs of frames (a new run after a gap of a few
  // frames). A range is new when first resolved or after seconds without a
  // run. One-off captures such as a car thumbnail are a few runs over a new,
  // large range; per-frame targets and periodic updates (reflection faces
  // every few frames) keep a range busy. While a range is new, the first
  // frames of each run are read back. So is a resolve to a range already
  // resolved more times this frame than in the previous one: a capture drawn
  // after the frame into one of its own targets, such as the photo mode
  // picture the game encodes (#399).
  constexpr uint64_t kRunGapFrames = 4, kRunReadbackFrames = 2, kIdleFrames = 300,
                     kNewFrames = 120;
  constexpr uint32_t kMinLength = 256 * 1024;
  ResolveReadback& readback = resolve_readbacks_[(uint64_t(address) << 32) | length];
  if (readback.count_frame != frame_) {
    readback.previous_count = readback.count_frame + 1 == frame_ ? readback.count : 0;
    readback.count = 0;
    readback.count_frame = frame_;
  }
  ++readback.count;
  if (!readback.last_used_frame || frame_ > readback.last_used_frame + kRunGapFrames) {
    if (!readback.last_used_frame || frame_ > readback.last_used_frame + kIdleFrames) {
      readback.new_since_frame = frame_;
    }
    readback.run_start_frame = frame_;
  }
  readback.last_used_frame = frame_;
  const bool extra = readback.previous_count && readback.count > readback.previous_count;
  return REXCVAR_GET(fh1_native_readback_new_resolves) && length >= kMinLength &&
         (extra || (frame_ < readback.run_start_frame + kRunReadbackFrames &&
                    frame_ < readback.new_since_frame + kNewFrames));
}

Microsoft::WRL::ComPtr<ID3D11Buffer> Fh1NativeExecutor::Impl::CreateReadbackBuffer(
    uint32_t size) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  if (FAILED(device()->CreateBuffer(&desc, nullptr, &buffer))) return nullptr;
  return buffer;
}

void Fh1NativeExecutor::Impl::QueueResolveReadback(uint32_t address, uint32_t length) {
  // The CPU can only read the result after the GPU reports progress, so the
  // copy is mapped at the next CPU-visible command (FlushResolveReadbacks).
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = CreateReadbackBuffer(length);
  if (!buffer) return Skip("resolve_readback_buffer");
  const D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
  context()->CopySubresourceRegion(buffer.Get(), 0, 0, 0, 0, native_memory_->GetBuffer(), 0,
                                   &box);
  pending_readbacks_.push_back({address, length, std::move(buffer)});
  Count("resolve_readback_one_off");
}

void Fh1NativeExecutor::Impl::ReadBackResolve(uint32_t address, uint32_t length) {
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
    // The runtime keeps the old buffer alive for a copy still queued into it.
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = CreateReadbackBuffer(size);
    if (!buffer) return Skip("resolve_readback_buffer");
    readback.buffers[write_index] = std::move(buffer);
    readback.sizes[write_index] = size;
  }
  ID3D11DeviceContext* ctx = context();
  // The copy and the map must not interleave with the presenter's use of the
  // immediate context.
  auto lock = command_processor_.GetD3D11Provider().LockImmediateContext();
  const D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
  ctx->CopySubresourceRegion(readback.buffers[write_index].Get(), 0, 0, 0, 0,
                             native_memory_->GetBuffer(), 0, &box);
  Count("resolve_readback");

  // Full: wait for this resolve. Fast and some: take the previous frame's copy
  // of the same range, waiting only when there is none (some copies only
  // then). A blocking Map waits exactly until the copy it reads is done.
  uint32_t read_index = write_index;
  bool miss = false;
  if (mode != ReadbackResolveMode::kFull) {
    read_index = 1 - write_index;
    if (!readback.buffers[read_index] || readback.sizes[read_index] < length) {
      miss = true;
      read_index = write_index;
    }
  }
  if (mode != ReadbackResolveMode::kSome || miss) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(readback.buffers[read_index].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      std::memcpy(destination, mapped.pData, length);
      ctx->Unmap(readback.buffers[read_index].Get(), 0);
    }
  }
  readback.current = 1 - readback.current;
}

void Fh1NativeExecutor::Impl::FlushResolveReadbacks() {
  if (pending_readbacks_.empty()) return;
  ID3D11DeviceContext* ctx = context();
  auto lock = command_processor_.GetD3D11Provider().LockImmediateContext();
  // In order: a later resolve over the same bytes wins, as on the GPU. Each
  // blocking Map waits for its copy.
  for (PendingReadback& pending : pending_readbacks_) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(pending.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      native_memory_->CopyReadbackToGuest(pending.address,
                                          static_cast<const uint8_t*>(mapped.pData),
                                          pending.length);
      ctx->Unmap(pending.buffer.Get(), 0);
    }
  }
  pending_readbacks_.clear();
  Count("resolve_readback_flush");
}

bool Fh1NativeExecutor::Impl::Resolve(uint32_t* written_address, uint32_t* written_length) {
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
  const D3D11_RECT rect = {LONG(x0), LONG(y0), LONG(x1), LONG(y1)};
  const uint32_t msaa = plan.msaa;
  const uint32_t pitch_tiles = plan.pitch_tiles;
  const auto& resolve_info = plan.info;
  const auto& color_info = plan.color_info;
  const auto& depth_info = plan.depth_info;

  if (plan.skip) {
    Skip(plan.skip);
    succeeded = false;
  } else if (plan.copy) {
    // Pages the resolve writes only partly keep the CPU's bytes.
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
                           plan.dest_base, plan.dest_pitch)) {
        complete = false;
      }
    }
    // Invalidates textures over the range and marks it GPU-written in the
    // shared memory (SharedMemory::RangeWrittenByGpu).
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

void Fh1NativeExecutor::Impl::OnSwap(uint64_t frame, uint32_t /*frontbuffer_address*/,
                                     uint32_t width, uint32_t height,
                                     const uint32_t* gamma_pwl) {
  if (!initialized_) return;
  frame_ = frame;
  ++cpu_frames_;
  DrainDumps();
  if (dump_frames_.count(frame) && !dump_directory_.empty()) {
    QueueFrontBufferDump(frame, width, height, gamma_pwl);
  }
  if (frame % 600 == 0) LogStats(frame);
}

void Fh1NativeExecutor::Impl::QueueFrontBufferDump(uint64_t frame, uint32_t width,
                                                   uint32_t height, const uint32_t* gamma_pwl) {
  xenos::TextureFormat format;
  uint32_t texture_width = 0, texture_height = 0;
  // The dump stores the texture bytes, so the host swizzle is not needed.
  uint32_t front_swizzle = 0;
  ID3D11ShaderResourceView* front =
      native_textures_->RequestSwapTexture(format, &texture_width, &texture_height, nullptr,
                                           &front_swizzle);
  if (!front) {
    Skip("dump_front_buffer_missing");
    return;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
  front->GetDesc(&srv_desc);
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  front->GetResource(&resource);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (!resource || FAILED(resource.As(&texture))) {
    Skip("dump_front_buffer_missing");
    return;
  }
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  if (desc.SampleDesc.Count > 1) {
    Skip("dump_front_buffer_msaa");
    return;
  }
  D3D11_TEXTURE2D_DESC staging_desc = {};
  staging_desc.Width = desc.Width;
  staging_desc.Height = desc.Height;
  staging_desc.MipLevels = 1;
  staging_desc.ArraySize = 1;
  staging_desc.Format = desc.Format;
  staging_desc.SampleDesc.Count = 1;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  PendingDump dump;
  if (FAILED(device()->CreateTexture2D(&staging_desc, nullptr, &dump.staging))) return;
  dump.frame = frame;
  if (gamma_pwl) {
    dump.has_gamma_pwl = true;
    std::memcpy(dump.gamma_pwl.data(), gamma_pwl, sizeof(dump.gamma_pwl));
  }
  dump.format = srv_desc.Format;
  dump.width = std::min(width, texture_width);
  dump.height = std::min(height, texture_height);
  // The swap texture's SRV is a plain view of mip 0 (subresource 0).
  context()->CopySubresourceRegion(dump.staging.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
  dumps_.push_back(std::move(dump));
}

void Fh1NativeExecutor::Impl::DrainDumps() {
  ID3D11DeviceContext* ctx = context();
  while (!dumps_.empty()) {
    PendingDump& dump = dumps_.front();
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;
    {
      auto lock = command_processor_.GetD3D11Provider().LockImmediateContext();
      // Not waiting: a copy still in flight is written at a later swap.
      result = ctx->Map(dump.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    }
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) break;
    if (FAILED(result)) {
      dumps_.pop_front();
      continue;
    }
    std::error_code error;
    std::filesystem::create_directories(dump_directory_, error);
    std::ofstream file(dump_directory_ / ("native-" + std::to_string(dump.frame) + ".ppm"),
                       std::ios::binary | std::ios::trunc);
    file << "P6\n" << dump.width << " " << dump.height << "\n255\n";
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    const bool wide = dump.format == DXGI_FORMAT_R16G16B16A16_UNORM;
    for (uint32_t y = 0; y < dump.height; ++y) {
      const uint8_t* row = bytes + size_t(y) * mapped.RowPitch;
      for (uint32_t x = 0; x < dump.width; ++x) {
        // 10-bit channels. Direct3D 11 views have no component mapping (the
        // texture cache's swizzle is applied in shaders), so the channels are
        // written as stored.
        uint32_t channels[4] = {0, 0, 0, 1023};
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
          const uint32_t value = channels[c];
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
    {
      auto lock = command_processor_.GetD3D11Provider().LockImmediateContext();
      ctx->Unmap(dump.staging.Get(), 0);
    }
    dumps_.pop_front();
  }
}

void Fh1NativeExecutor::Impl::LogStats(uint64_t frame) {
  const std::string skips = counters_.FormatSkips();
  const std::string stats = counters_.FormatStats();
  REXGPU_INFO(
      "FH1 native executor frame={} draws={} resolves={} surfaces={} skips={{{}}} stats={{{}}}",
      frame, draws_, resolves_, surfaces_.size(), skips, stats);
  // Memory: surfaces (estimated from their dimensions) and the executor's
  // buffers, and the command processor's texture cache.
  uint64_t surface_bytes = 0;
  for (const auto& [packed, surface] : surfaces_) surface_bytes += surface.bytes;
  REXGPU_INFO("FH1 native executor memory MB: surfaces {} transfer_words {} textures {}",
              surface_bytes >> 20, transfer_words_size_ >> 20,
              native_textures_ ? native_textures_->total_host_memory_usage() >> 20 : 0);
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
    cpu_ns_ = {};
    cpu_frames_ = 0;
    if (native_memory_) {
      // Cumulative shared-memory uploads by kind (NP-2.8): requests that
      // uploaded, page ranges and bytes.
      using Kind = SharedMemory::UploadKind;
      const auto upload_stats = [this](Kind kind) {
        const SharedMemory::UploadStats& s = native_memory_->upload_stats(kind);
        return fmt::format("{}/{}/{:.1f}MB (repeat {:.1f}MB)", s.requests, s.ranges,
                           double(s.bytes) / 1048576.0, double(s.repeat_bytes) / 1048576.0);
      };
      REXGPU_INFO("Shared memory uploads: vertex {} index {} texture {} memexport {} other {}",
                  upload_stats(Kind::kVertex), upload_stats(Kind::kIndex),
                  upload_stats(Kind::kTexture), upload_stats(Kind::kMemexport),
                  upload_stats(Kind::kOther));
    }
  }
}

Fh1NativeExecutor::Fh1NativeExecutor(D3D11CommandProcessor& command_processor,
                                     const RegisterFile& register_file, memory::Memory& memory)
    : impl_(std::make_unique<Impl>(command_processor, register_file, memory)) {}

Fh1NativeExecutor::~Fh1NativeExecutor() { impl_->Shutdown(); }

bool Fh1NativeExecutor::Initialize(const Fh1NativeExecutorConfig& config) {
  return impl_->Initialize(config);
}

void Fh1NativeExecutor::Shutdown() { impl_->Shutdown(); }

void Fh1NativeExecutor::PrepareTargets(const Fh1NativeDrawInfo& draw) {
  impl_->PrepareTargets(draw);
}

void Fh1NativeExecutor::LogStats(uint64_t frame) { impl_->LogStats(frame); }

bool Fh1NativeExecutor::BindTargets(uint32_t& bound_bits, uint32_t* formats,
                                    uint32_t& bound_width, uint32_t& bound_height) {
  return impl_->BindTargets(bound_bits, formats, bound_width, bound_height);
}

void Fh1NativeExecutor::NativeDrawIssued(const Fh1NativeDrawInfo& draw) {
  impl_->NativeDrawIssued(draw);
}

bool Fh1NativeExecutor::NativeResolve(uint32_t& written_address, uint32_t& written_length) {
  return impl_->NativeResolve(written_address, written_length);
}

void Fh1NativeExecutor::FlushResolveReadbacks() { impl_->FlushResolveReadbacks(); }

void Fh1NativeExecutor::OnSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width,
                               uint32_t height, const uint32_t* gamma_pwl) {
  impl_->OnSwap(frame, frontbuffer_address, width, height, gamma_pwl);
}

}  // namespace rex::graphics::d3d11
