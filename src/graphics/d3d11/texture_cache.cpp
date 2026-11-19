/**
 * @file        graphics/d3d11/texture_cache.cpp
 * @brief       Direct3D 11 (feature level 11_0) guest texture cache.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/texture_cache.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/dxbc_shader_model_5_0.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/fh1_mip_contract.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>

// The Direct3D 12 texture cache defines this when it is built into the same
// library; the Direct3D 11 one shares it, so a configuration switches backends
// without renaming the setting.
#if REX_HAS_D3D12
namespace rex::graphics::d3d12 {
REXCVAR_DECLARE(bool, fh1_direct_reflection_cube_import);
}  // namespace rex::graphics::d3d12
namespace rex::graphics::d3d11 {
using ::rex::graphics::d3d12::FLAGS_fh1_direct_reflection_cube_import_storage_;
}  // namespace rex::graphics::d3d11
#else
namespace rex::graphics::d3d11 {
REXCVAR_DEFINE_BOOL(fh1_direct_reflection_cube_import, true, "GPU",
                    "Import FH1 reflection cubes directly into their host texture");
}  // namespace rex::graphics::d3d11
#endif

namespace rex::graphics::d3d11 {

using Microsoft::WRL::ComPtr;

// Texture load shaders: the verified shader model 5.1 code of the Direct3D 12
// texture cache, converted to shader model 5.0 at initialization.
namespace shaders_sm51 {
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_ctx1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxn_rg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3a_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3aas1111_bgra4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5a_r8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_cs.h"
}  // namespace shaders_sm51

// Our shaders, compiled for shader model 5.0 (compile-d3d11.sh).
namespace shaders_sm50 {
#include "../shaders/bytecode/d3d11_5_0/d3d11_texture_copy_2d_cs.h"
#include "../shaders/bytecode/d3d11_5_0/d3d11_texture_copy_3d_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_reflection_cube_import_cs.h"
}  // namespace shaders_sm50

namespace {

// The host layout of the load shaders' output, as the Direct3D 12 copy
// footprints have it (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT), so the load shaders see the same
// pitches and offsets.
constexpr uint32_t kHostRowPitchAlignment = 256;
constexpr uint64_t kHostSlicePlacementAlignment = 512;

// d3d11_texture_copy.cs.hlsl XE_TEXTURE_COPY_MODE_*.
enum CopyMode : uint32_t {
  kCopyModeR8,
  kCopyModeR8G8,
  kCopyModeR16,
  kCopyModeR8G8B8A8,
  kCopyModeR16G16,
  kCopyModeR32,
  kCopyModeR10G10B10A2,
  kCopyModeR16G16B16A16,
  kCopyModeR32G32,
  kCopyModeR32G32B32A32,
  kCopyModeB5G6R5ToRGBA8,
  kCopyModeB5G5R5A1ToRGBA8,
  kCopyModeB4G4R4A4ToRGBA8,
};

// d3d11_texture_copy.cs.hlsl XeTextureCopyConstants.
struct CopyConstants {
  uint32_t source_offset;
  uint32_t source_row_pitch;
  uint32_t source_slice_pitch;
  uint32_t mode;
  uint32_t source_origin[3];
  uint32_t padding_0;
  uint32_t size[3];
  uint32_t padding_1;
};
static_assert(sizeof(CopyConstants) == 48);

constexpr uint32_t kCopyThreadGroupSizeLog2 = 3;

// How the copy writes a texture of a resource format: the typed UAV format
// (with a typed UAV store at feature level 11_0) and the element layout. For a
// block-compressed format, those of the block texture copied into it.
bool GetCopyDestination(DXGI_FORMAT resource_format, DXGI_FORMAT& uav_format_out,
                        uint32_t& mode_out) {
  switch (resource_format) {
    case DXGI_FORMAT_R8_TYPELESS:
      uav_format_out = DXGI_FORMAT_R8_UINT;
      mode_out = kCopyModeR8;
      return true;
    case DXGI_FORMAT_R8G8_TYPELESS:
      uav_format_out = DXGI_FORMAT_R8G8_UINT;
      mode_out = kCopyModeR8G8;
      return true;
    case DXGI_FORMAT_R16_TYPELESS:
      uav_format_out = DXGI_FORMAT_R16_UINT;
      mode_out = kCopyModeR16;
      return true;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      uav_format_out = DXGI_FORMAT_R8G8B8A8_UINT;
      mode_out = kCopyModeR8G8B8A8;
      return true;
    case DXGI_FORMAT_R16G16_TYPELESS:
      uav_format_out = DXGI_FORMAT_R16G16_UINT;
      mode_out = kCopyModeR16G16;
      return true;
    case DXGI_FORMAT_R32_TYPELESS:
      uav_format_out = DXGI_FORMAT_R32_UINT;
      mode_out = kCopyModeR32;
      return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      uav_format_out = DXGI_FORMAT_R10G10B10A2_UINT;
      mode_out = kCopyModeR10G10B10A2;
      return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      uav_format_out = DXGI_FORMAT_R16G16B16A16_UINT;
      mode_out = kCopyModeR16G16B16A16;
      return true;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
      uav_format_out = DXGI_FORMAT_R32G32_UINT;
      mode_out = kCopyModeR32G32;
      return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
      uav_format_out = DXGI_FORMAT_R32G32B32A32_UINT;
      mode_out = kCopyModeR32G32B32A32;
      return true;
    default:
      return false;
  }
}

bool IsBlockCompressedDXGIFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
      return true;
    default:
      return false;
  }
}

// Bytes per texel, or per 4x4 block of a block-compressed format, of the
// resource formats used here.
uint32_t GetResourceFormatElementBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_TYPELESS:
      return 1;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
      return 2;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
      return 8;
    default:
      return 16;
  }
}

uint64_t EstimateTextureBytes(DXGI_FORMAT format, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t array_size, uint32_t mip_levels) {
  bool block_compressed = IsBlockCompressedDXGIFormat(format);
  uint64_t element_bytes = GetResourceFormatElementBytes(format);
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < mip_levels; ++level) {
    uint64_t level_width = std::max(width >> level, uint32_t(1));
    uint64_t level_height = std::max(height >> level, uint32_t(1));
    uint64_t level_depth = std::max(depth >> level, uint32_t(1));
    if (block_compressed) {
      level_width = (level_width + 3) >> 2;
      level_height = (level_height + 3) >> 2;
    }
    bytes += level_width * level_height * level_depth * element_bytes;
  }
  return bytes * array_size;
}

uint64_t EstimateResourceBytes(ID3D11Resource* resource) {
  D3D11_RESOURCE_DIMENSION type;
  resource->GetType(&type);
  if (type == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
    D3D11_TEXTURE3D_DESC desc;
    static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
    return EstimateTextureBytes(desc.Format, desc.Width, desc.Height, desc.Depth, 1,
                                desc.MipLevels);
  }
  D3D11_TEXTURE2D_DESC desc;
  static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
  return EstimateTextureBytes(desc.Format, desc.Width, desc.Height, 1, desc.ArraySize,
                              desc.MipLevels);
}

// View dimensions of a texture's SRVs (and of the null textures).
enum ViewKind : uint32_t {
  kViewKind2DArray,
  kViewKind3D,
  kViewKindCube,
  // A plain 2D view of mip 0 for presentation.
  kViewKindSwap2D,

  kViewKindCount,
};

ViewKind GetViewKind(xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k3D:
      return kViewKind3D;
    case xenos::DataDimension::kCube:
      return kViewKindCube;
    default:
      return kViewKind2DArray;
  }
}

}  // namespace

struct D3D11TextureCache::Impl {
  // 16-bit packed Direct3D 12 host formats stored as R8G8B8A8 (no typed UAV
  // store at feature level 11_0): the copy expands the load shader's 16-bit
  // output.
  enum class Expand16 : uint8_t {
    kNone,
    kB5G6R5,
    kB5G5R5A1,
    kB4G4R4A4,
  };

  struct HostFormat {
    // Resource format: typeless (so the copy can write a *_UINT UAV of it)
    // or block-compressed.
    DXGI_FORMAT dxgi_format_resource;
    // Unsigned normalized or unsigned/signed float SRV format.
    DXGI_FORMAT dxgi_format_unsigned;
    // The regular load shader.
    LoadShaderIndex load_shader;
    // Signed normalized or unsigned/signed float SRV format.
    DXGI_FORMAT dxgi_format_signed;
    // If the signed version has a different host representation, its load
    // shader (a separate texture is created for it).
    LoadShaderIndex load_shader_signed;
    // Whether the host format is block-compressed (when not decompressing).
    bool is_block_compressed;
    // Decompression for block-compressed textures with sizes not multiple of
    // 4x4, which Direct3D 11 can't create: resource and SRV formats and the
    // load shader. Unsigned normalized only, as on Direct3D 12.
    DXGI_FORMAT dxgi_format_uncompressed_resource;
    DXGI_FORMAT dxgi_format_uncompressed;
    LoadShaderIndex load_shader_decompress;
    // Expansion of the regular load shader's output in the copy.
    Expand16 expand;
    // Mapping of Xenos swizzle components to the host format's components,
    // as the Direct3D 12 SRV component mapping (applied in the translated
    // shaders here).
    uint32_t swizzle;
  };

  static const HostFormat kHostFormats[64];

  class D3D11Texture final : public Texture {
   public:
    D3D11Texture(D3D11TextureCache& texture_cache, const TextureKey& key,
                 ComPtr<ID3D11Resource> resource, bool track_usage = true)
        : Texture(texture_cache, key, track_usage), resource_(std::move(resource)) {
      UpdateResourceInfo();
      guest_memory_usage_ = EstimateResourceBytes(resource_.Get());
      SetHostMemoryUsage(guest_memory_usage_);
    }

    ID3D11Resource* resource() const { return resource_.Get(); }
    bool resource_is_3d() const { return resource_is_3d_; }
    uint32_t resource_mip_levels() const { return resource_mip_levels_; }
    uint32_t resource_array_size() const { return resource_array_size_; }

    ComPtr<ID3D11ShaderResourceView>& srv(bool is_signed, ViewKind kind) {
      return srvs_[uint32_t(is_signed)][kind];
    }

    // Higher-resolution texture replacements (as on Direct3D 12): the texture
    // draws from a larger resource, with its own views, while its data is
    // replaced; the guest-size resource stays for when the guest data
    // changes to data without a replacement. The runtime keeps resources and
    // views alive while the GPU uses them, so switching needs no retirement.
    bool uses_replacement_resource() const { return uses_replacement_; }
    ID3D11Resource* guest_resource() const {
      return uses_replacement_ ? other_resource_.Get() : resource_.Get();
    }
    uint32_t extra_mip_levels() const { return uses_replacement_ ? replacement_extra_levels_ : 0; }

    // The replacement resource matching the description, created on demand
    // (null on failure); makes it the active resource.
    ID3D11Resource* UseReplacementResource(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc,
                                           uint32_t extra_levels) {
      if (uses_replacement_) {
        SwapActiveResource();
      }
      if (other_resource_) {
        D3D11_TEXTURE2D_DESC other_desc;
        static_cast<ID3D11Texture2D*>(other_resource_.Get())->GetDesc(&other_desc);
        if (other_desc.Width != desc.Width || other_desc.Height != desc.Height ||
            other_desc.MipLevels != desc.MipLevels || other_desc.Format != desc.Format) {
          other_resource_.Reset();
          for (auto& srvs_of_sign : other_srvs_) {
            for (auto& view : srvs_of_sign) {
              view.Reset();
            }
          }
        }
      }
      if (!other_resource_) {
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
          return nullptr;
        }
        other_resource_ = texture;
        SetHostMemoryUsage(guest_memory_usage_ + EstimateResourceBytes(other_resource_.Get()));
      }
      replacement_extra_levels_ = extra_levels;
      SwapActiveResource();
      return resource_.Get();
    }

    void UseGuestResource() {
      if (uses_replacement_) {
        SwapActiveResource();
      }
    }

    // The 2D wrapper of a 3D texture sampled as 2D (gpu_3d_to_2d_texture).
    std::unique_ptr<D3D11Texture> texture_3d_as_2d;

   private:
    void UpdateResourceInfo() {
      D3D11_RESOURCE_DIMENSION type;
      resource_->GetType(&type);
      resource_is_3d_ = type == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
      if (resource_is_3d_) {
        D3D11_TEXTURE3D_DESC desc;
        static_cast<ID3D11Texture3D*>(resource_.Get())->GetDesc(&desc);
        resource_mip_levels_ = desc.MipLevels;
        resource_array_size_ = 1;
      } else {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D*>(resource_.Get())->GetDesc(&desc);
        resource_mip_levels_ = desc.MipLevels;
        resource_array_size_ = desc.ArraySize;
      }
    }

    void SwapActiveResource() {
      std::swap(resource_, other_resource_);
      std::swap(srvs_, other_srvs_);
      uses_replacement_ = !uses_replacement_;
      UpdateResourceInfo();
    }

    ComPtr<ID3D11Resource> resource_;
    bool resource_is_3d_ = false;
    uint32_t resource_mip_levels_ = 1;
    uint32_t resource_array_size_ = 1;
    using SRVArray = std::array<std::array<ComPtr<ID3D11ShaderResourceView>, kViewKindCount>, 2>;
    SRVArray srvs_;
    // The inactive one of the guest and replacement resources.
    ComPtr<ID3D11Resource> other_resource_;
    SRVArray other_srvs_;
    bool uses_replacement_ = false;
    uint32_t replacement_extra_levels_ = 0;
    uint64_t guest_memory_usage_ = 0;
  };

  // A grow-only single-level texture of R32G32_UINT or R32G32B32A32_UINT
  // blocks, 2D array or 3D, that the copy writes and CopySubresourceRegion
  // reinterprets into a block-compressed level (block counts of mips don't
  // follow a mip chain, so a single level with a source box is used).
  struct BlockStaging {
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11UnorderedAccessView> uav;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth_or_array_size = 0;
  };

  Impl(D3D11TextureCache& cache, D3D11CommandProcessor& command_processor,
       D3D11SharedMemory& shared_memory)
      : cache(cache), command_processor(command_processor), shared_memory(shared_memory) {
    std::memset(unsupported_format_features_used, 0, sizeof(unsupported_format_features_used));
  }

  D3D11TextureCache& cache;
  D3D11CommandProcessor& command_processor;
  D3D11SharedMemory& shared_memory;

  std::array<ComPtr<ID3D11ComputeShader>, kLoadShaderCount> load_shaders;
  ComPtr<ID3D11ComputeShader> reflection_cube_import_shader;
  ComPtr<ID3D11ComputeShader> copy_2d_shader;
  ComPtr<ID3D11ComputeShader> copy_3d_shader;
  // 48 bytes each (LoadConstants is 40, rounded up to whole registers).
  ComPtr<ID3D11Buffer> load_constants_buffer;
  ComPtr<ID3D11Buffer> copy_constants_buffer;

  // Linear load output: typed UAVs for the load shaders (by element size
  // log2), a raw SRV for the copy.
  ComPtr<ID3D11Buffer> scratch_buffer;
  uint64_t scratch_buffer_size = 0;
  ComPtr<ID3D11ShaderResourceView> scratch_buffer_raw_srv;
  std::array<ComPtr<ID3D11UnorderedAccessView>, 5> scratch_buffer_uavs;

  // [128-bit blocks][3D].
  BlockStaging block_staging[2][2];

  // Zero 1x1 R8G8B8A8 textures for bindings without a valid texture.
  ComPtr<ID3D11ShaderResourceView> null_srvs[kViewKindCube + 1];

  // Leaves room under the 4096 sampler state objects Direct3D 11 allows for
  // the new samplers of one frame (creation beyond the limit fails and the
  // fallback sampler is used).
  static constexpr size_t kMaxCachedSamplers = 3072;
  std::unordered_map<uint32_t, ComPtr<ID3D11SamplerState>> samplers;
  // texture_mip_lod_bias the cached samplers were created with.
  double samplers_mip_lod_bias = 0.0;
  ComPtr<ID3D11SamplerState> fallback_sampler;

  // Unsupported texture formats used during this frame (for research and
  // testing).
  enum : uint8_t {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  uint8_t unsupported_format_features_used[64];

  std::vector<uint8_t> cpu_upload_scratch;
  bool swap_swizzle_error_logged = false;

  ID3D11Device* device() const { return command_processor.GetDevice(); }
  ID3D11DeviceContext* context() const { return command_processor.GetContext(); }

  static bool IsFh1ReflectionCube(TextureKey key) {
    return key.dimension == xenos::DataDimension::kCube && key.GetWidth() == 256 &&
           key.GetHeight() == 256 && key.GetDepthOrArraySize() == 6 && key.mip_max_level == 8 &&
           uint32_t(key.format) == 7 && key.tiled && !key.packed_mips &&
           (uint32_t(key.mip_page) << 12) ==
               (uint32_t(key.base_page) << 12) + Fh1MipChain::kBaseBytes;
  }

  // Whether decompression is needed on the host: Direct3D 11 can't create
  // block-compressed textures with sizes not multiple of the block size (no
  // equivalent of the Direct3D 12 unaligned block textures option).
  static bool IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width, uint32_t height) {
    if (kHostFormats[uint32_t(format)].dxgi_format_uncompressed == DXGI_FORMAT_UNKNOWN) {
      return false;
    }
    const FormatInfo* format_info = FormatInfo::Get(format);
    return (width & (format_info->block_width - 1)) || (height & (format_info->block_height - 1));
  }

  static DXGI_FORMAT GetResourceFormat(xenos::TextureFormat format, uint32_t width,
                                       uint32_t height) {
    const HostFormat& host_format = kHostFormats[uint32_t(format)];
    return IsDecompressionNeeded(format, width, height)
               ? host_format.dxgi_format_uncompressed_resource
               : host_format.dxgi_format_resource;
  }
  static DXGI_FORMAT GetResourceFormat(TextureKey key) {
    return GetResourceFormat(key.format, key.GetWidth(), key.GetHeight());
  }
  static DXGI_FORMAT GetUnormFormat(TextureKey key) {
    const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
    return IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())
               ? host_format.dxgi_format_uncompressed
               : host_format.dxgi_format_unsigned;
  }

  static LoadShaderIndex GetLoadShaderIndex(TextureKey key) {
    const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
    if (key.signed_separate) {
      return host_format.load_shader_signed;
    }
    if (IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
      return host_format.load_shader_decompress;
    }
    return host_format.load_shader;
  }

  static bool IsSignedVersionSeparate(TextureKey key) {
    const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
    return host_format.load_shader_signed != kLoadShaderIndexUnknown &&
           host_format.load_shader_signed != host_format.load_shader;
  }

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  static xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) {
    if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
      // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D,
      // but there's no Direct3D 9 equivalent anyway, and too weird to be
      // suitable for intentional real usage.
      return xenos::ClampMode::kClampToEdge;
    }
    if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
        clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
      // No Direct3D equivalents.
      return xenos::ClampMode::kMirrorClampToEdge;
    }
    return clamp_mode;
  }

  bool Initialize();

  bool WriteConstants(ID3D11Buffer* buffer, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context()->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    std::memcpy(mapped.pData, data, size);
    context()->Unmap(buffer, 0);
    return true;
  }

  // Unbinds the guest draws' vertex and pixel shader SRVs before a texture is
  // written through a UAV (the runtime would otherwise unbind a conflicting
  // SRV itself, and the debug layer reports that). The command processor
  // binds all SRVs again on the next draw.
  void UnbindGraphicsShaderResources() {
    static ID3D11ShaderResourceView* const kNullSrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] =
        {};
    context()->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, kNullSrvs);
    context()->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, kNullSrvs);
  }

  bool EnsureScratchBuffer(uint64_t size);
  ID3D11UnorderedAccessView* GetScratchBufferUAV(uint32_t element_size_log2);
  BlockStaging* GetBlockStaging(bool blocks_128bit, bool is_3d, uint32_t width, uint32_t height,
                                uint32_t depth_or_array_size);

  std::unique_ptr<Texture> CreateTexture(TextureKey key);
  ID3D11ShaderResourceView* FindOrCreateSRV(D3D11Texture& texture, xenos::DataDimension dimension,
                                            bool is_signed);
  D3D11Texture* GetOrCreate3DAs2D(D3D11Texture& texture);

  bool LoadFromResidentMemory(D3D11Texture& texture, bool load_base, bool load_mips);
  bool LoadFh1ReflectionCube(D3D11Texture& texture);
  bool TryLoadReplacement(D3D11Texture& texture);
  bool TryLoadFromCpu(D3D11Texture& texture, bool load_base, bool load_mips,
                      bool resolve_sourced);

  ID3D11SamplerState* GetSampler(SamplerParameters parameters);
};

#define XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(swizzle)                                    \
  {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN, \
   kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,             \
   kLoadShaderIndexUnknown, Expand16::kNone, xenos::swizzle}

// The Direct3D 12 table (same load shaders, signedness and swizzles) with
// typeless resource formats, and R8G8B8A8 for the 16-bit packed formats and
// the subsampled 4:2:2 ones.
const D3D11TextureCache::Impl::HostFormat D3D11TextureCache::Impl::kHostFormats[64] = {
    // k_1_REVERSE
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_1
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    // B5G5R5A1 (red and blue swapped in the load shader) expanded to RGBA8.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexR5G5B5A1ToB5G5R5A1, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false,
     DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kB5G5R5A1,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // B5G6R5 (red and blue swapped in the load shader) expanded to RGBA8.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kB5G6R5, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kB5G6R5,
     XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Always the Direct3D 12 decompression path (the subsampled G8R8_G8B8 has
    // no typed UAV store): RGB8 into R8G8B8A8, red and blue swapped in the
    // load shader.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexGBGR8ToRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // As above, for R8G8_B8G8.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexBGRG8ToRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_8_8_8_8_A
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // k_4_4_4_4
    // B4G4R4A4 (red and blue swapped in the load shader) expanded to RGBA8.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kB4G4R4A4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT1ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT3ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT5ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_32
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_32_32_32_32
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // k_32_FLOAT
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_8
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_16_MPEG
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_16_MPEG
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_8_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_32_AS_8_8_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_16_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_MPEG_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RRRR),
    // k_16_16_MPEG_INTERLACED
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGGG),
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM,
     kLoadShaderIndexDXNToRG8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kNone,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT1ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT3ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT5ToRGBA8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBB),
    // k_DXT3A
    // R8 has the same size as BC2, but doesn't have the 4x4 size alignment
    // requirement.
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM,
     kLoadShaderIndexDXT5AToR8, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, Expand16::kNone, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    // B4G4R4A4 expanded to RGBA8.
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexDXT3AAs1111ToBGRA4, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false,
     DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, Expand16::kB4G4R4A4,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBA),
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED(XE_GPU_TEXTURE_SWIZZLE_RGBA),
};

#undef XE_D3D11_TEXTURE_FORMAT_UNSUPPORTED

bool D3D11TextureCache::Impl::Initialize() {
  ID3D11Device* d3d_device = device();

  // Typed UAV stores the copy relies on (required at feature level 11_0).
  static const DXGI_FORMAT kCopyUavFormats[] = {
      DXGI_FORMAT_R8_UINT,           DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_R16_UINT,
      DXGI_FORMAT_R8G8B8A8_UINT,     DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R32_UINT,
      DXGI_FORMAT_R10G10B10A2_UINT,  DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R32G32_UINT,
      DXGI_FORMAT_R32G32B32A32_UINT,
  };
  for (DXGI_FORMAT format : kCopyUavFormats) {
    UINT support = 0;
    if (FAILED(d3d_device->CheckFormatSupport(format, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
      REXGPU_ERROR(
          "D3D11TextureCache: DXGI format {} has no typed UAV support, which feature level 11_0 "
          "requires; textures can't be loaded on this device",
          uint32_t(format));
      return false;
    }
  }

  // Texture load shaders.
  struct ShaderCode {
    const void* code;
    size_t size;
  };
  std::array<ShaderCode, kLoadShaderCount> load_shader_code = {};
#define XE_D3D11_LOAD_SHADER(index, name) \
  load_shader_code[index] = ShaderCode{shaders_sm51::name, sizeof(shaders_sm51::name)}
  XE_D3D11_LOAD_SHADER(kLoadShaderIndex8bpb, texture_load_8bpb_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndex16bpb, texture_load_16bpb_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndex32bpb, texture_load_32bpb_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndex64bpb, texture_load_64bpb_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndex128bpb, texture_load_128bpb_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR5G5B5A1ToB5G5R5A1, texture_load_r5g5b5a1_b5g5r5a1_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR5G6B5ToB5G6R5, texture_load_r5g6b5_b5g6r5_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
                       texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexRGBA4ToBGRA4, texture_load_r4g4b4a4_b4g4r4a4_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexGBGR8ToRGB8, texture_load_gbgr8_rgb8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexBGRG8ToRGB8, texture_load_bgrg8_rgb8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16, texture_load_r10g11b11_rgba16_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                       texture_load_r10g11b11_rgba16_snorm_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16, texture_load_r11g11b10_rgba16_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                       texture_load_r11g11b10_rgba16_snorm_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT1ToRGBA8, texture_load_dxt1_rgba8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3ToRGBA8, texture_load_dxt3_rgba8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT5ToRGBA8, texture_load_dxt5_rgba8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXNToRG8, texture_load_dxn_rg8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3A, texture_load_dxt3a_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT3AAs1111ToBGRA4, texture_load_dxt3aas1111_bgra4_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDXT5AToR8, texture_load_dxt5a_r8_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexCTX1, texture_load_ctx1_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_cs);
  XE_D3D11_LOAD_SHADER(kLoadShaderIndexDepthFloat, texture_load_depth_float_cs);
#undef XE_D3D11_LOAD_SHADER
  std::vector<uint32_t> converted;
  std::string conversion_error;
  for (uint32_t i = 0; i < kLoadShaderCount; ++i) {
    const ShaderCode& code = load_shader_code[i];
    if (!code.code) {
      continue;
    }
    if (!ConvertDxbcToShaderModel5_0(code.code, code.size, converted, conversion_error)) {
      REXGPU_ERROR(
          "D3D11TextureCache: Failed to convert texture load shader {} to shader model 5.0: {}", i,
          conversion_error);
      return false;
    }
    if (FAILED(d3d_device->CreateComputeShader(converted.data(), converted.size() * sizeof(uint32_t),
                                               nullptr, &load_shaders[i]))) {
      REXGPU_ERROR("D3D11TextureCache: Failed to create texture load shader {}", i);
      return false;
    }
  }

  if (FAILED(d3d_device->CreateComputeShader(shaders_sm50::d3d11_texture_copy_2d_cs,
                                             sizeof(shaders_sm50::d3d11_texture_copy_2d_cs),
                                             nullptr, &copy_2d_shader)) ||
      FAILED(d3d_device->CreateComputeShader(shaders_sm50::d3d11_texture_copy_3d_cs,
                                             sizeof(shaders_sm50::d3d11_texture_copy_3d_cs),
                                             nullptr, &copy_3d_shader))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture copy shaders");
    return false;
  }
  if (FAILED(d3d_device->CreateComputeShader(
          shaders_sm50::fh1_reflection_cube_import_cs,
          sizeof(shaders_sm50::fh1_reflection_cube_import_cs), nullptr,
          &reflection_cube_import_shader))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the FH1 reflection cube import shader");
    return false;
  }

  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = 48;
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  static_assert(sizeof(LoadConstants) <= 48);
  if (FAILED(d3d_device->CreateBuffer(&constants_desc, nullptr, &load_constants_buffer)) ||
      FAILED(d3d_device->CreateBuffer(&constants_desc, nullptr, &copy_constants_buffer))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture load constant buffers");
    return false;
  }

  // Null textures: zeros in every component (the Direct3D 12 null descriptors
  // force 0 through their component mapping).
  static const uint32_t kZeros[6] = {};
  D3D11_SUBRESOURCE_DATA zero_data[6];
  for (D3D11_SUBRESOURCE_DATA& data : zero_data) {
    data.pSysMem = kZeros;
    data.SysMemPitch = sizeof(uint32_t);
    data.SysMemSlicePitch = sizeof(uint32_t);
  }
  D3D11_TEXTURE2D_DESC null_2d_desc = {};
  null_2d_desc.Width = 1;
  null_2d_desc.Height = 1;
  null_2d_desc.MipLevels = 1;
  null_2d_desc.ArraySize = 1;
  null_2d_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_2d_desc.SampleDesc.Count = 1;
  null_2d_desc.Usage = D3D11_USAGE_IMMUTABLE;
  null_2d_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> null_2d, null_cube;
  ComPtr<ID3D11Texture3D> null_3d;
  D3D11_TEXTURE2D_DESC null_cube_desc = null_2d_desc;
  null_cube_desc.ArraySize = 6;
  null_cube_desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  D3D11_TEXTURE3D_DESC null_3d_desc = {};
  null_3d_desc.Width = 1;
  null_3d_desc.Height = 1;
  null_3d_desc.Depth = 1;
  null_3d_desc.MipLevels = 1;
  null_3d_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_3d_desc.Usage = D3D11_USAGE_IMMUTABLE;
  null_3d_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SHADER_RESOURCE_VIEW_DESC null_srv_desc = {};
  null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  null_srv_desc.Texture2DArray.MipLevels = 1;
  null_srv_desc.Texture2DArray.ArraySize = 1;
  if (FAILED(d3d_device->CreateTexture2D(&null_2d_desc, zero_data, &null_2d)) ||
      FAILED(d3d_device->CreateShaderResourceView(null_2d.Get(), &null_srv_desc,
                                                  &null_srvs[kViewKind2DArray])) ||
      FAILED(d3d_device->CreateTexture2D(&null_cube_desc, zero_data, &null_cube)) ||
      FAILED(d3d_device->CreateTexture3D(&null_3d_desc, zero_data, &null_3d))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the null textures");
    return false;
  }
  null_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  null_srv_desc.Texture3D.MostDetailedMip = 0;
  null_srv_desc.Texture3D.MipLevels = 1;
  if (FAILED(d3d_device->CreateShaderResourceView(null_3d.Get(), &null_srv_desc,
                                                  &null_srvs[kViewKind3D]))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the null 3D texture view");
    return false;
  }
  null_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
  null_srv_desc.TextureCube.MostDetailedMip = 0;
  null_srv_desc.TextureCube.MipLevels = 1;
  if (FAILED(d3d_device->CreateShaderResourceView(null_cube.Get(), &null_srv_desc,
                                                  &null_srvs[kViewKindCube]))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the null cube texture view");
    return false;
  }

  D3D11_SAMPLER_DESC fallback_sampler_desc = {};
  fallback_sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  fallback_sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  fallback_sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  fallback_sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  fallback_sampler_desc.MaxAnisotropy = 1;
  fallback_sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  fallback_sampler_desc.MaxLOD = FLT_MAX;
  if (FAILED(d3d_device->CreateSamplerState(&fallback_sampler_desc, &fallback_sampler))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the fallback sampler");
    return false;
  }
  samplers_mip_lod_bias = REXCVAR_GET(texture_mip_lod_bias);

  return true;
}

bool D3D11TextureCache::Impl::EnsureScratchBuffer(uint64_t size) {
  if (size <= scratch_buffer_size && scratch_buffer) {
    return true;
  }
  // Typed views address at most 2^27 elements of 16 bytes.
  constexpr uint64_t kMaxSize = uint64_t(1) << (D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP + 4);
  if (size > kMaxSize || size > UINT32_MAX) {
    REXGPU_ERROR("D3D11TextureCache: A texture load needs {} bytes of scratch memory", size);
    return false;
  }
  uint64_t new_size = std::max(uint64_t(1) << 20, scratch_buffer_size);
  while (new_size < size) {
    new_size <<= 1;
  }
  new_size = std::min(new_size, std::min(kMaxSize, uint64_t(UINT32_MAX) & ~uint64_t(15)));
  scratch_buffer_raw_srv.Reset();
  for (auto& uav : scratch_buffer_uavs) {
    uav.Reset();
  }
  scratch_buffer.Reset();
  scratch_buffer_size = 0;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(new_size);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device()->CreateBuffer(&desc, nullptr, &scratch_buffer))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a {} byte texture load scratch buffer",
                 new_size);
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.FirstElement = 0;
  srv_desc.BufferEx.NumElements = UINT(new_size >> 2);
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  if (FAILED(device()->CreateShaderResourceView(scratch_buffer.Get(), &srv_desc,
                                                &scratch_buffer_raw_srv))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create the texture load scratch buffer view");
    scratch_buffer.Reset();
    return false;
  }
  scratch_buffer_size = new_size;
  return true;
}

ID3D11UnorderedAccessView* D3D11TextureCache::Impl::GetScratchBufferUAV(
    uint32_t element_size_log2) {
  if (element_size_log2 < 2 || element_size_log2 > 4 || !scratch_buffer) {
    return nullptr;
  }
  ComPtr<ID3D11UnorderedAccessView>& uav = scratch_buffer_uavs[element_size_log2];
  if (!uav) {
    static const DXGI_FORMAT kFormats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
                                           DXGI_FORMAT_R32G32B32A32_UINT};
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = kFormats[element_size_log2 - 2];
    desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = UINT(scratch_buffer_size >> element_size_log2);
    if (FAILED(device()->CreateUnorderedAccessView(scratch_buffer.Get(), &desc, &uav))) {
      return nullptr;
    }
  }
  return uav.Get();
}

D3D11TextureCache::Impl::BlockStaging* D3D11TextureCache::Impl::GetBlockStaging(
    bool blocks_128bit, bool is_3d, uint32_t width, uint32_t height,
    uint32_t depth_or_array_size) {
  BlockStaging& staging = block_staging[uint32_t(blocks_128bit)][uint32_t(is_3d)];
  if (staging.resource && staging.width >= width && staging.height >= height &&
      staging.depth_or_array_size >= depth_or_array_size) {
    return &staging;
  }
  uint32_t new_width = std::max(width, staging.width);
  uint32_t new_height = std::max(height, staging.height);
  uint32_t new_depth = std::max(depth_or_array_size, staging.depth_or_array_size);
  staging.uav.Reset();
  staging.resource.Reset();
  staging.width = 0;
  staging.height = 0;
  staging.depth_or_array_size = 0;
  DXGI_FORMAT format =
      blocks_128bit ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = format;
  if (is_3d) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = new_width;
    desc.Height = new_height;
    desc.Depth = new_depth;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture3D> texture;
    if (FAILED(device()->CreateTexture3D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    staging.resource = texture;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    uav_desc.Texture3D.MipSlice = 0;
    uav_desc.Texture3D.FirstWSlice = 0;
    uav_desc.Texture3D.WSize = new_depth;
  } else {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = new_width;
    desc.Height = new_height;
    desc.MipLevels = 1;
    desc.ArraySize = new_depth;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device()->CreateTexture2D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    staging.resource = texture;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    uav_desc.Texture2DArray.MipSlice = 0;
    uav_desc.Texture2DArray.FirstArraySlice = 0;
    uav_desc.Texture2DArray.ArraySize = new_depth;
  }
  if (FAILED(device()->CreateUnorderedAccessView(staging.resource.Get(), &uav_desc,
                                                 &staging.uav))) {
    staging.resource.Reset();
    return nullptr;
  }
  staging.width = new_width;
  staging.height = new_height;
  staging.depth_or_array_size = new_depth;
  return &staging;
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::Impl::CreateTexture(TextureKey key) {
  DXGI_FORMAT format = GetResourceFormat(key);
  if (format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used[uint32_t(key.format)] |= kUnsupportedResourceBit;
    return nullptr;
  }
  // Written by the copy through a UAV, except for block-compressed formats
  // (written with CopySubresourceRegion).
  UINT bind_flags = D3D11_BIND_SHADER_RESOURCE;
  if (!IsBlockCompressedDXGIFormat(format)) {
    bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
  }
  ComPtr<ID3D11Resource> resource;
  if (key.dimension == xenos::DataDimension::k3D) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = key.GetWidth();
    desc.Height = key.GetHeight();
    desc.Depth = key.GetDepthOrArraySize();
    desc.MipLevels = key.mip_max_level + 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    ComPtr<ID3D11Texture3D> texture;
    if (FAILED(device()->CreateTexture3D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    resource = texture;
  } else {
    // 1D textures are treated as 2D for simplicity.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = key.GetWidth();
    desc.Height = key.GetHeight();
    desc.MipLevels = key.mip_max_level + 1;
    desc.ArraySize = key.GetDepthOrArraySize();
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    if (key.dimension == xenos::DataDimension::kCube) {
      desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device()->CreateTexture2D(&desc, nullptr, &texture))) {
      return nullptr;
    }
    resource = texture;
  }
  return std::unique_ptr<Texture>(new D3D11Texture(cache, key, std::move(resource)));
}

D3D11TextureCache::Impl::D3D11Texture* D3D11TextureCache::Impl::GetOrCreate3DAs2D(
    D3D11Texture& texture) {
  if (!REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return nullptr;
  }
  if (texture.texture_3d_as_2d) {
    return texture.texture_3d_as_2d.get();
  }
  TextureKey key = texture.key();
  DXGI_FORMAT format = GetResourceFormat(key);
  if (format == DXGI_FORMAT_UNKNOWN) {
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (!IsBlockCompressedDXGIFormat(format)) {
    desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
  }
  ComPtr<ID3D11Texture2D> resource_2d;
  if (FAILED(device()->CreateTexture2D(&desc, nullptr, &resource_2d))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create 3D-as-2D wrapper resource");
    return nullptr;
  }
  // The first slice of the 3D texture, loaded with its 3D tiling, keeping the
  // 3D key (as on Direct3D 12).
  TextureKey key_2d = key;
  key_2d.depth_or_array_size_minus_1 = 0;
  key_2d.mip_max_level = 0;
  texture.texture_3d_as_2d.reset(new D3D11Texture(cache, key_2d, resource_2d, false));
  texture.texture_3d_as_2d->SetForceLoad3DTiling(true);
  if (!cache.LoadTextureData(*texture.texture_3d_as_2d)) {
    REXGPU_ERROR("D3D11TextureCache: Failed to load 3D-as-2D wrapper data");
    texture.texture_3d_as_2d.reset();
    return nullptr;
  }
  return texture.texture_3d_as_2d.get();
}

ID3D11ShaderResourceView* D3D11TextureCache::Impl::FindOrCreateSRV(D3D11Texture& texture,
                                                                   xenos::DataDimension dimension,
                                                                   bool is_signed) {
  TextureKey texture_key = texture.key();
  if (IsSignedVersionSeparate(texture_key) && texture_key.signed_separate != uint32_t(is_signed)) {
    // Not the version with the needed signedness.
    return nullptr;
  }
  ViewKind view_kind = GetViewKind(dimension);
  if (texture_key.dimension == xenos::DataDimension::k3D && view_kind == kViewKind2DArray &&
      !REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return nullptr;
  }
  ComPtr<ID3D11ShaderResourceView>& srv = texture.srv(is_signed, view_kind);
  if (srv) {
    return srv.Get();
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  xenos::TextureFormat format = texture_key.format;
  // Not supporting signed compressed textures - hopefully DXN and DXT5A are
  // not used as signed.
  desc.Format = is_signed ? kHostFormats[uint32_t(format)].dxgi_format_signed
                          : GetUnormFormat(texture_key);
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used[uint32_t(format)] |=
        is_signed ? kUnsupportedSnormBit : kUnsupportedUnormBit;
    return nullptr;
  }

  uint32_t mip_levels = texture_key.mip_max_level + 1 + texture.extra_mip_levels();
  ID3D11Resource* resource_for_view = texture.resource();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MostDetailedMip = 0;
      desc.Texture3D.MipLevels = mip_levels;
      break;
    case xenos::DataDimension::kCube:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
      desc.TextureCube.MostDetailedMip = 0;
      desc.TextureCube.MipLevels = mip_levels;
      break;
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MostDetailedMip = 0;
      desc.Texture2DArray.FirstArraySlice = 0;
      if (texture_key.dimension == xenos::DataDimension::k3D) {
        // Created and loaded by RequestTextures: views are requested while
        // the draw's resources are bound, where a load's dispatches would
        // unbind them.
        D3D11Texture* texture_2d =
            REXCVAR_GET(gpu_3d_to_2d_texture) ? texture.texture_3d_as_2d.get() : nullptr;
        if (!texture_2d) {
          return nullptr;
        }
        resource_for_view = texture_2d->resource();
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.ArraySize = 1;
      } else {
        desc.Texture2DArray.MipLevels = mip_levels;
        desc.Texture2DArray.ArraySize = texture_key.GetDepthOrArraySize();
      }
      break;
    default:
      assert_unhandled_case(dimension);
      return nullptr;
  }
  if (FAILED(device()->CreateShaderResourceView(resource_for_view, &desc, &srv))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a texture SRV");
    return nullptr;
  }
  return srv.Get();
}

bool D3D11TextureCache::Impl::LoadFh1ReflectionCube(D3D11Texture& texture) {
  // Writes the 9 levels of the 6 faces straight from guest memory into the
  // R10G10B10A2 cube through R10G10B10A2_UINT UAVs, as on Direct3D 12 at 1x.
  constexpr uint32_t kMipCount = 9;
  TextureKey texture_key = texture.key();
  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  const FormatInfo* format_info = FormatInfo::Get(texture_key.format);
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();

  std::array<ComPtr<ID3D11UnorderedAccessView>, kMipCount> uavs;
  for (uint32_t level = 0; level < kMipCount; ++level) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R10G10B10A2_UINT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    uav_desc.Texture2DArray.MipSlice = level;
    uav_desc.Texture2DArray.FirstArraySlice = 0;
    uav_desc.Texture2DArray.ArraySize = 6;
    if (FAILED(device()->CreateUnorderedAccessView(texture.resource(), &uav_desc, &uavs[level]))) {
      return false;
    }
  }

  ID3D11DeviceContext* d3d_context = context();
  UnbindGraphicsShaderResources();
  // t0 source, u0 destination, cb0 constants.
  ID3D11ShaderResourceView* source_srv = shared_memory.GetUintPow2SRV(4);
  ID3D11Buffer* constants_buffer = load_constants_buffer.Get();
  d3d_context->CSSetShader(reflection_cube_import_shader.Get(), nullptr, 0);
  d3d_context->CSSetShaderResources(0, 1, &source_srv);
  d3d_context->CSSetConstantBuffers(0, 1, &constants_buffer);
  bool succeeded = true;
  LoadConstants constants = {};
  // Tiled, 8-in-32 endianness, scale 1x1.
  constants.is_tiled_3d_endian_scale = 1 | (2 << 2) | (1 << 4) | (1 << 7);
  for (uint32_t level = 0; level < kMipCount; ++level) {
    const texture_util::TextureGuestLayout::Level& level_layout =
        level ? guest_layout.mips[level] : guest_layout.base;
    constants.guest_offset =
        level ? (uint32_t(texture_key.mip_page) << 12) + guest_layout.mip_offsets_bytes[level]
              : uint32_t(texture_key.base_page) << 12;
    constants.guest_pitch_aligned = level_layout.row_pitch_bytes / bytes_per_block;
    constants.size_blocks[0] = std::max(width >> level, uint32_t(1));
    constants.size_blocks[1] = std::max(height >> level, uint32_t(1));
    constants.size_blocks[2] = 6;
    constants.host_offset = level_layout.array_slice_stride_bytes;
    if (!WriteConstants(constants_buffer, &constants, sizeof(constants))) {
      succeeded = false;
      break;
    }
    ID3D11UnorderedAccessView* uav = uavs[level].Get();
    d3d_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    d3d_context->Dispatch((constants.size_blocks[0] + 7) / 8, (constants.size_blocks[1] + 7) / 8,
                          6);
  }
  command_processor.UnbindComputeResources(1, 1, 1);
  d3d_context->CSSetShader(nullptr, nullptr, 0);
  command_processor.InvalidateGuestDrawState();
  if (succeeded) {
    texture.MarkAsUsed();
  }
  return succeeded;
}

bool D3D11TextureCache::Impl::LoadFromResidentMemory(D3D11Texture& texture, bool load_base,
                                                     bool load_mips) {
  TextureKey texture_key = texture.key();
  // Draw resolution scaling is not available on Direct3D 11.
  if (texture_key.scaled_resolve) {
    return false;
  }
  // Guest data loads into the guest-size resource (a replacement is chosen
  // by TryLoadFromCpu before this).
  texture.UseGuestResource();

  LoadShaderIndex load_shader = GetLoadShaderIndex(texture_key);
  if (load_shader == kLoadShaderIndexUnknown || !load_shaders[load_shader]) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  if (REXCVAR_GET(fh1_direct_reflection_cube_import) && IsFh1ReflectionCube(texture_key) &&
      load_base && load_mips && !texture.force_load_3d_tiling() &&
      LoadFh1ReflectionCube(texture)) {
    return true;
  }

  // Get the guest layout.
  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  assert_true(level_first <= level_last);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);

  // The loop counter can mean two things depending on whether the packed mip
  // tail is stored as mip 0, because in this case, it would be ambiguous since
  // both the base and the mips would be on "level 0", but stored in separate
  // places.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Loop iteration 0 - base packed mip tail.
    // Loop iteration 1 - mips packed mip tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    // Loop iteration is the actual level being loaded.
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // The host layout in the scratch buffer, the same as the Direct3D 12 copy
  // footprints.
  const HostFormat& host_format = kHostFormats[uint32_t(guest_format)];
  bool decompress = IsDecompressionNeeded(guest_format, width, height);
  bool host_block_compressed = host_format.is_block_compressed && !decompress;
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1)
                                      << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  struct HostSliceLayout {
    uint64_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t row_pitch;
    uint64_t slice_size;
  };
  // Indexing is the same as for guest stored mips:
  // 1...min(level_last, level_packed) if level_packed is not 0, or only 0 if
  // level_packed == 0.
  HostSliceLayout host_slice_layout_base = {};
  HostSliceLayout host_slice_layouts_mips[xenos::kTextureMaxMips] = {};
  uint64_t copy_buffer_size = 0;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    HostSliceLayout& layout = is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    layout.offset = copy_buffer_size;
    if (level == level_packed) {
      // Loading the packed tail for the base or the mips - load the whole tail
      // to copy regions out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      layout.width = guest_layout_packed.x_extent_blocks * block_width;
      layout.height = guest_layout_packed.y_extent_blocks * block_height;
      layout.depth = guest_layout_packed.z_extent;
    } else {
      layout.width = std::max(width >> level, uint32_t(1));
      layout.height = std::max(height >> level, uint32_t(1));
      layout.depth = std::max(depth >> level, uint32_t(1));
    }
    layout.width = rex::round_up(layout.width, host_block_width);
    layout.height = rex::round_up(layout.height, host_block_height);
    layout.row_pitch = rex::align(rex::round_up(layout.width / host_block_width,
                                                host_x_blocks_per_thread) *
                                      load_shader_info.bytes_per_host_block,
                                  kHostRowPitchAlignment);
    layout.slice_size = rex::align(
        uint64_t(layout.row_pitch) * (layout.height / host_block_height) * layout.depth,
        kHostSlicePlacementAlignment);
    copy_buffer_size += layout.slice_size * array_size;
  }
  if (!EnsureScratchBuffer(copy_buffer_size)) {
    return false;
  }
  ID3D11UnorderedAccessView* scratch_uav = GetScratchBufferUAV(load_shader_info.dest_bpe_log2);
  ID3D11ShaderResourceView* source_srv =
      shared_memory.GetUintPow2SRV(load_shader_info.source_bpe_log2);
  if (!scratch_uav || !source_srv) {
    return false;
  }

  // The copy destination format and element layout.
  DXGI_FORMAT resource_format = GetResourceFormat(texture_key);
  DXGI_FORMAT copy_uav_format;
  uint32_t copy_mode;
  if (!GetCopyDestination(resource_format, copy_uav_format, copy_mode)) {
    return false;
  }
  if (!decompress && !texture_key.signed_separate) {
    switch (host_format.expand) {
      case Expand16::kB5G6R5:
        copy_mode = kCopyModeB5G6R5ToRGBA8;
        break;
      case Expand16::kB5G5R5A1:
        copy_mode = kCopyModeB5G5R5A1ToRGBA8;
        break;
      case Expand16::kB4G4R4A4:
        copy_mode = kCopyModeB4G4R4A4ToRGBA8;
        break;
      default:
        break;
    }
  }
  bool resource_is_3d = texture.resource_is_3d();

  ID3D11DeviceContext* d3d_context = context();
  UnbindGraphicsShaderResources();

  // Load into the scratch buffer: t0 guest memory, u0 the scratch buffer,
  // cb0 the load constants (root constants on Direct3D 12).
  ID3D11Buffer* load_constants_cbuffer = load_constants_buffer.Get();
  d3d_context->CSSetShader(load_shaders[load_shader].Get(), nullptr, 0);
  d3d_context->CSSetShaderResources(0, 1, &source_srv);
  d3d_context->CSSetUnorderedAccessViews(0, 1, &scratch_uav, nullptr);
  d3d_context->CSSetConstantBuffers(0, 1, &load_constants_cbuffer);

  bool succeeded = true;
  LoadConstants load_constants = {};
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (1u << 4) | (1u << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; succeeded && loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;

    uint32_t guest_address = (is_base ? texture_key.base_page : texture_key.mip_page) << 12;
    load_constants.guest_offset = guest_address;
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level];
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (texture_key.tiled) {
      // Shaders expect pitch in blocks for tiled textures.
      level_guest_pitch /= guest_format_info->bytes_per_block();
      assert_zero(level_guest_pitch & (xenos::kTextureTileWidthHeight - 1));
    }
    load_constants.guest_pitch_aligned = level_guest_pitch;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    assert_true(!is_3d_tiling || !(load_constants.guest_z_stride_block_rows_aligned &
                                   (xenos::kTextureTileWidthHeight - 1)));

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      // This is the packed mip tail, containing not only the specified level,
      // but also other levels at different offsets - load the entire needed
      // extents.
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    load_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    load_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const HostSliceLayout& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    load_constants.host_offset = uint32_t(level_host_slice_layout.offset);
    load_constants.host_pitch = level_host_slice_layout.row_pitch;

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (!WriteConstants(load_constants_cbuffer, &load_constants, sizeof(load_constants))) {
        succeeded = false;
        break;
      }
      d3d_context->Dispatch(group_count_x, group_count_y, load_constants.size_blocks[2]);
      load_constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
      load_constants.host_offset += uint32_t(level_host_slice_layout.slice_size);
    }
  }
  // The scratch buffer goes from u0 to t0.
  command_processor.UnbindComputeResources(1, 1);

  // Copy into the texture: t0 the scratch buffer, u0 the level (or the block
  // staging texture), cb0 the copy constants.
  ID3D11Buffer* copy_constants_cbuffer = copy_constants_buffer.Get();
  ID3D11ShaderResourceView* scratch_srv = scratch_buffer_raw_srv.Get();
  d3d_context->CSSetShader(resource_is_3d ? copy_3d_shader.Get() : copy_2d_shader.Get(), nullptr,
                           0);
  d3d_context->CSSetShaderResources(0, 1, &scratch_srv);
  d3d_context->CSSetConstantBuffers(0, 1, &copy_constants_cbuffer);
  uint32_t resource_mip_levels = texture.resource_mip_levels();
  for (uint32_t level = level_first; succeeded && level <= level_last; ++level) {
    uint32_t guest_level = std::min(level, level_packed);
    const HostSliceLayout& layout =
        level ? host_slice_layouts_mips[guest_level] : host_slice_layout_base;
    CopyConstants copy_constants = {};
    copy_constants.source_offset = uint32_t(layout.offset);
    copy_constants.source_row_pitch = layout.row_pitch;
    copy_constants.mode = copy_mode;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      copy_constants.source_origin[0] = level_offset_blocks_x * block_width / host_block_width;
      copy_constants.source_origin[1] = level_offset_blocks_y * block_height / host_block_height;
      copy_constants.source_origin[2] = level_offset_z;
    }
    uint32_t level_depth = std::max(depth >> level, uint32_t(1));
    copy_constants.size[0] =
        (std::max(width >> level, uint32_t(1)) + (host_block_width - 1)) / host_block_width;
    copy_constants.size[1] =
        (std::max(height >> level, uint32_t(1)) + (host_block_height - 1)) / host_block_height;
    if (is_3d) {
      // Depth slices of the level; a 3D-as-2D wrapper has a depth of 1.
      copy_constants.source_slice_pitch =
          layout.row_pitch * (layout.height / host_block_height);
      copy_constants.size[2] = level_depth;
    } else {
      // Array layers (cube faces), each in its own placement.
      copy_constants.source_slice_pitch = uint32_t(layout.slice_size);
      copy_constants.size[2] = array_size;
    }
    if (!WriteConstants(copy_constants_cbuffer, &copy_constants, sizeof(copy_constants))) {
      succeeded = false;
      break;
    }

    ComPtr<ID3D11UnorderedAccessView> level_uav;
    ID3D11UnorderedAccessView* dest_uav;
    BlockStaging* staging = nullptr;
    if (host_block_compressed) {
      staging = GetBlockStaging(copy_uav_format == DXGI_FORMAT_R32G32B32A32_UINT, resource_is_3d,
                                copy_constants.size[0], copy_constants.size[1],
                                copy_constants.size[2]);
      if (!staging) {
        succeeded = false;
        break;
      }
      dest_uav = staging->uav.Get();
    } else {
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = copy_uav_format;
      if (resource_is_3d) {
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        uav_desc.Texture3D.MipSlice = level;
        uav_desc.Texture3D.FirstWSlice = 0;
        uav_desc.Texture3D.WSize = level_depth;
      } else {
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        uav_desc.Texture2DArray.MipSlice = level;
        uav_desc.Texture2DArray.FirstArraySlice = 0;
        uav_desc.Texture2DArray.ArraySize = texture.resource_array_size();
      }
      if (FAILED(device()->CreateUnorderedAccessView(texture.resource(), &uav_desc, &level_uav))) {
        succeeded = false;
        break;
      }
      dest_uav = level_uav.Get();
    }
    d3d_context->CSSetUnorderedAccessViews(0, 1, &dest_uav, nullptr);
    d3d_context->Dispatch(
        (copy_constants.size[0] + ((1u << kCopyThreadGroupSizeLog2) - 1)) >>
            kCopyThreadGroupSizeLog2,
        (copy_constants.size[1] + ((1u << kCopyThreadGroupSizeLog2) - 1)) >>
            kCopyThreadGroupSizeLog2,
        copy_constants.size[2]);
    if (staging) {
      // Reinterpret the blocks as the block-compressed level.
      command_processor.UnbindComputeResources(0, 1);
      D3D11_BOX box = {};
      box.right = copy_constants.size[0];
      box.bottom = copy_constants.size[1];
      if (resource_is_3d) {
        box.back = copy_constants.size[2];
        d3d_context->CopySubresourceRegion(texture.resource(), level, 0, 0, 0,
                                           staging->resource.Get(), 0, &box);
      } else {
        box.back = 1;
        for (uint32_t slice = 0; slice < copy_constants.size[2]; ++slice) {
          box.front = 0;
          d3d_context->CopySubresourceRegion(
              texture.resource(), D3D11CalcSubresource(level, slice, resource_mip_levels), 0, 0, 0,
              staging->resource.Get(), D3D11CalcSubresource(0, slice, 1), &box);
        }
      }
    }
  }

  command_processor.UnbindComputeResources(1, 1, 1);
  d3d_context->CSSetShader(nullptr, nullptr, 0);
  command_processor.InvalidateGuestDrawState();

  // Update LRU caching because the texture will be used by the GPU.
  texture.MarkAsUsed();
  return succeeded;
}

bool D3D11TextureCache::Impl::TryLoadReplacement(D3D11Texture& texture) {
  const TextureKey key = texture.key();
  const texture_replacement::Source source{key.format,
                                           key.dimension,
                                           key.GetWidth(),
                                           key.GetHeight(),
                                           key.GetDepthOrArraySize(),
                                           key.mip_max_level,
                                           key.base_page,
                                           key.mip_page,
                                           bool(key.tiled),
                                           bool(key.packed_mips),
                                           bool(key.scaled_resolve),
                                           bool(key.signed_separate),
                                           uint32_t(key.endianness),
                                           texture.force_load_3d_tiling()};
  if (!texture_replacement::IsEligible(source) ||
      IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
    return false;
  }
  texture_replacement::Lookup lookup;
  switch (texture_replacement::Find(source, texture.guest_layout(), cache.shared_memory(),
                                    lookup)) {
    case texture_replacement::LookupResult::kNotEligible:
      return false;
    case texture_replacement::LookupResult::kNone:
      // New guest data: back to the guest-size resource.
      texture.UseGuestResource();
      return false;
    case texture_replacement::LookupResult::kFound:
      break;
  }
  const texture_replacement::Format& format = *lookup.format;
  const texture_replacement::Image& image = *lookup.image;
  const uint32_t guest_levels = key.mip_max_level + 1u;
  ID3D11Resource* guest_resource = texture.guest_resource();
  D3D11_RESOURCE_DIMENSION guest_type;
  guest_resource->GetType(&guest_type);
  if (guest_type != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    texture.UseGuestResource();
    return false;
  }
  D3D11_TEXTURE2D_DESC guest_desc;
  static_cast<ID3D11Texture2D*>(guest_resource)->GetDesc(&guest_desc);
  if (guest_desc.ArraySize != 1 || (guest_desc.Format != DXGI_FORMAT(format.dxgi_unorm) &&
                                    guest_desc.Format != DXGI_FORMAT(format.dxgi_srgb) &&
                                    guest_desc.Format != DXGI_FORMAT(format.dxgi_typeless))) {
    texture.UseGuestResource();
    return false;
  }
  ID3D11Resource* resource;
  uint32_t levels;
  if (lookup.scale_log2) {
    D3D11_TEXTURE2D_DESC replacement_desc = guest_desc;
    replacement_desc.Width = image.width;
    replacement_desc.Height = image.height;
    replacement_desc.MipLevels = lookup.levels;
    resource = texture.UseReplacementResource(device(), replacement_desc,
                                              lookup.levels - guest_levels);
    if (!resource) {
      texture.UseGuestResource();
      return false;
    }
    levels = lookup.levels;
  } else {
    texture.UseGuestResource();
    resource = texture.resource();
    levels = guest_desc.MipLevels;
  }
  ID3D11DeviceContext* d3d_context = context();
  size_t source_offset = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    const UINT pitch = UINT(texture_replacement::Blocks(format, image.width, level)) *
                       format.block_bytes;
    d3d_context->UpdateSubresource(resource, level, nullptr, image.data.data() + source_offset,
                                   pitch, 0);
    source_offset += texture_replacement::LevelBytes(format, image.width, image.height, level);
  }
  texture.MarkAsUsed();
  texture_replacement::LogLoaded(lookup);
  return true;
}

bool D3D11TextureCache::Impl::TryLoadFromCpu(D3D11Texture& texture, bool load_base,
                                             bool load_mips, bool resolve_sourced) {
  // Resolve output is the title's own rendering, never a replacement; it
  // loads into the guest-size resource.
  if (resolve_sourced) {
    texture.UseGuestResource();
  } else if (TryLoadReplacement(texture)) {
    return true;
  }
  // Linear single-level 8-bit textures (FH1's video planes, rewritten by the
  // CPU every frame) are uploaded straight from guest memory instead of
  // through the untiling compute load. Anything else takes the normal load.
  const TextureKey key = texture.key();
  if (!load_base || load_mips || key.mip_max_level || key.scaled_resolve || key.tiled ||
      texture.force_load_3d_tiling() || !key.base_page || key.signed_separate ||
      key.dimension != xenos::DataDimension::k2DOrStacked || key.GetDepthOrArraySize() != 1 ||
      key.format != xenos::TextureFormat::k_8 || key.endianness != xenos::Endian::kNone) {
    return false;
  }
  const auto& guest = texture.guest_layout().base;
  const uint32_t width = key.GetWidth();
  const uint32_t height = key.GetHeight();
  // The guest row pitch matches the Direct3D 12 upload footprint there; keep
  // the same eligibility.
  if (guest.row_pitch_bytes != rex::align(width, kHostRowPitchAlignment) ||
      !guest.level_data_extent_bytes ||
      guest.level_data_extent_bytes > uint64_t(guest.row_pitch_bytes) * height ||
      guest.level_data_extent_bytes < uint64_t(guest.row_pitch_bytes) * (height - 1) + width) {
    return false;
  }
  cpu_upload_scratch.resize(size_t(guest.row_pitch_bytes) * height);
  // CopyCpuRange refuses GPU-written pages. The texture load lifecycle armed
  // its watch before this call, so writes racing the copy remain dirty.
  if (!cache.shared_memory().CopyCpuRange(
          key.base_page << 12, {cpu_upload_scratch.data(), guest.level_data_extent_bytes})) {
    return false;
  }
  context()->UpdateSubresource(texture.resource(), 0, nullptr, cpu_upload_scratch.data(),
                               guest.row_pitch_bytes, 0);
  texture.MarkAsUsed();
  return true;
}

ID3D11SamplerState* D3D11TextureCache::Impl::GetSampler(SamplerParameters parameters) {
  auto it = samplers.find(parameters.value);
  if (it != samplers.end()) {
    return it->second.Get();
  }
  // Direct3D 11 allows 4096 unique sampler state objects; the cache is trimmed
  // in BeginFrame (kMaxCachedSamplers), never here, so pointers returned for
  // the bindings of a draw stay valid until they are bound.

  D3D11_SAMPLER_DESC desc;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(parameters.aniso_filter) - 1);
  } else {
    D3D11_FILTER_TYPE d3d_filter_min =
        parameters.min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE d3d_filter_mag =
        parameters.mag_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE d3d_filter_mip =
        parameters.mip_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    desc.Filter = D3D11_ENCODE_BASIC_FILTER(d3d_filter_min, d3d_filter_mag, d3d_filter_mip, FALSE);
    desc.MaxAnisotropy = 1;
  }
  static const D3D11_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D11_TEXTURE_ADDRESS_WRAP,
      /* kMirroredRepeat       */ D3D11_TEXTURE_ADDRESS_MIRROR,
      /* kClampToEdge          */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToEdge    */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToHalfway       */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToHalfway */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToBorder        */ D3D11_TEXTURE_ADDRESS_BORDER,
      /* kMirrorClampToBorder  */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[uint32_t(parameters.clamp_x)];
  desc.AddressV = kAddressModeMap[uint32_t(parameters.clamp_y)];
  desc.AddressW = kAddressModeMap[uint32_t(parameters.clamp_z)];
  // The title's LOD bias is applied in shaders; this is the player's global
  // offset on top of it. The cache is cleared when it changes (BeginFrame).
  desc.MipLODBias = float(samplers_mip_lod_bias);
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      assert_true(parameters.border_color == xenos::BorderColor::k_ABGR_Black);
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  desc.MinLOD = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    // "It is undefined whether LOD clamping based on MinLOD and MaxLOD Sampler
    // states should happen before or after deciding if magnification is
    // occuring" - Direct3D 11.3 Functional Specification.
    // Using the GL_NEAREST / GL_LINEAR minification filter emulation logic
    // described in the Vulkan VkSamplerCreateInfo specification, preserving
    // magnification vs. minification - point mip sampling (usable only without
    // anisotropic filtering) and MaxLOD 0.25. With anisotropic filtering,
    // magnification vs. minification doesn't matter as the filter is always
    // linear for both - but linear filtering specifically is what must not be
    // done for kBaseMap, so setting MaxLOD to MinLOD.
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      assert_false(parameters.mip_linear);
      desc.MaxLOD += 0.25f;
    }
  } else {
    // Maximum mip level is in the texture resource itself.
    desc.MaxLOD = FLT_MAX;
  }
  ComPtr<ID3D11SamplerState> sampler;
  if (FAILED(device()->CreateSamplerState(&desc, &sampler))) {
    REXGPU_ERROR("D3D11TextureCache: Failed to create a sampler state {:08X}", parameters.value);
    return fallback_sampler.Get();
  }
  ID3D11SamplerState* sampler_raw = sampler.Get();
  samplers.emplace(parameters.value, std::move(sampler));
  return sampler_raw;
}

std::unique_ptr<D3D11TextureCache> D3D11TextureCache::Create(
    const RegisterFile& register_file, D3D11SharedMemory& shared_memory,
    D3D11CommandProcessor& command_processor) {
  std::unique_ptr<D3D11TextureCache> texture_cache(
      new D3D11TextureCache(register_file, shared_memory, command_processor));
  if (!texture_cache->Initialize()) {
    return nullptr;
  }
  return texture_cache;
}

D3D11TextureCache::D3D11TextureCache(const RegisterFile& register_file,
                                     D3D11SharedMemory& shared_memory,
                                     D3D11CommandProcessor& command_processor)
    : TextureCache(register_file, shared_memory, 1, 1),
      command_processor_(command_processor),
      d3d11_shared_memory_(shared_memory) {}

D3D11TextureCache::~D3D11TextureCache() {
  // Textures hold nothing of the implementation, but destroy them while it
  // still exists, as on Direct3D 12.
  DestroyAllTextures(true);
}

bool D3D11TextureCache::Initialize() {
  impl_ = std::make_unique<Impl>(*this, command_processor_, d3d11_shared_memory_);
  return impl_->Initialize();
}

void D3D11TextureCache::ClearCache() { TextureCache::ClearCache(); }

void D3D11TextureCache::BeginFrame() {
  // Frames are the submissions of the base class's texture lifetime tracking
  // (the textures used in the frame being recorded, and those the GPU has
  // finished with).
  uint64_t current_frame = command_processor_.GetCurrentFrame();
  if (current_frame > current_submission_index()) {
    TextureCache::BeginSubmission(current_frame);
  }
  TextureCache::CompletedSubmissionUpdated(command_processor_.GetCompletedFrame());

  TextureCache::BeginFrame();

  // A reload of texture replacements: rescan the folders and clear the
  // caches, so every texture loads again (through replacements) at the next
  // use. The first frame only records the setting.
  if (texture_replacement::ReloadRequested()) {
    texture_replacement::Reset();
    command_processor_.ClearCaches();
    REXGPU_INFO("Texture replacements: reloading");
  }

  std::memset(impl_->unsupported_format_features_used, 0,
              sizeof(impl_->unsupported_format_features_used));

  // Samplers dropped here stay alive while bound (the context references
  // them), so a new sampler can't take the address of one still bound.
  double mip_lod_bias = REXCVAR_GET(texture_mip_lod_bias);
  if (mip_lod_bias != impl_->samplers_mip_lod_bias) {
    impl_->samplers.clear();
    impl_->samplers_mip_lod_bias = mip_lod_bias;
  } else if (impl_->samplers.size() > Impl::kMaxCachedSamplers) {
    impl_->samplers.clear();
  }
}

void D3D11TextureCache::EndFrame() {
  // Report used unsupported texture formats.
  bool unsupported_header_written = false;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t unsupported_features = impl_->unsupported_format_features_used[i];
    if (unsupported_features == 0) {
      continue;
    }
    if (!unsupported_header_written) {
      REXGPU_ERROR("Unsupported texture formats used in the frame:");
      unsupported_header_written = true;
    }
    REXGPU_ERROR("* {}{}{}{}", FormatInfo::Get(xenos::TextureFormat(i))->name,
                 unsupported_features & Impl::kUnsupportedResourceBit ? " resource" : "",
                 unsupported_features & Impl::kUnsupportedUnormBit ? " unsigned" : "",
                 unsupported_features & Impl::kUnsupportedSnormBit ? " signed" : "");
    impl_->unsupported_format_features_used[i] = 0;
  }
}

void D3D11TextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);

  // Pre-create 3D-as-2D wrappers in the texture request phase, since loading
  // them dispatches compute work.
  if (REXCVAR_GET(gpu_3d_to_2d_texture)) {
    uint32_t textures_3d = used_texture_mask;
    uint32_t index_3d;
    while (rex::bit_scan_forward(textures_3d, &index_3d)) {
      textures_3d &= ~(uint32_t(1) << index_3d);
      const TextureBinding* binding = GetValidTextureBinding(index_3d);
      if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
        continue;
      }
      if (binding->texture) {
        impl_->GetOrCreate3DAs2D(*static_cast<Impl::D3D11Texture*>(binding->texture));
      }
      if (binding->texture_signed) {
        impl_->GetOrCreate3DAs2D(*static_cast<Impl::D3D11Texture*>(binding->texture_signed));
      }
    }
  }

  // The textures will be referenced by the draw.
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    if (binding->texture) {
      binding->texture->MarkAsUsed();
    }
    if (binding->texture_signed) {
      binding->texture_signed->MarkAsUsed();
    }
  }
}

ID3D11ShaderResourceView* D3D11TextureCache::GetActiveTextureSRV(
    const DxbcShader::TextureBinding& binding) {
  ID3D11ShaderResourceView* srv = nullptr;
  const TextureBinding* texture_binding = GetValidTextureBinding(binding.fetch_constant);
  if (texture_binding &&
      Impl::AreDimensionsCompatible(binding.dimension, texture_binding->key.dimension)) {
    Texture* texture = nullptr;
    if (binding.is_signed) {
      // Not supporting signed compressed textures - hopefully DXN and DXT5A
      // are not used as signed.
      if (texture_util::IsAnySignSigned(texture_binding->swizzled_signs)) {
        texture = IsSignedVersionSeparateForFormat(texture_binding->key)
                      ? texture_binding->texture_signed
                      : texture_binding->texture;
      }
    } else if (texture_util::IsAnySignNotSigned(texture_binding->swizzled_signs)) {
      texture = texture_binding->texture;
    }
    if (texture) {
      // A 3D texture fetched as 1D or 2D samples its 2D wrapper.
      xenos::DataDimension view_dimension = texture_binding->key.dimension;
      if (view_dimension == xenos::DataDimension::k3D &&
          (binding.dimension == xenos::FetchOpDimension::k1D ||
           binding.dimension == xenos::FetchOpDimension::k2D)) {
        view_dimension = xenos::DataDimension::k2DOrStacked;
      }
      srv = impl_->FindOrCreateSRV(*static_cast<Impl::D3D11Texture*>(texture), view_dimension,
                                   binding.is_signed);
      if (srv) {
        texture->MarkAsUsed();
      }
    }
  }
  if (!srv) {
    switch (binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        srv = impl_->null_srvs[kViewKind3D].Get();
        break;
      case xenos::FetchOpDimension::kCube:
        srv = impl_->null_srvs[kViewKindCube].Get();
        break;
      default:
        assert_true(binding.dimension == xenos::FetchOpDimension::k1D ||
                    binding.dimension == xenos::FetchOpDimension::k2D);
        srv = impl_->null_srvs[kViewKind2DArray].Get();
        break;
    }
  }
  return srv;
}

D3D11TextureCache::SamplerParameters D3D11TextureCache::GetSamplerParameters(
    const DxbcShader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = Impl::NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = Impl::NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = Impl::NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  if (REXCVAR_GET(force_trilinear_filtering) && has_mips && min_mag_linear &&
      mip_filter == xenos::TextureFilter::kPoint) {
    mip_filter = xenos::TextureFilter::kLinear;
  }

  // TODO(Triang3l): Disable filtering for texture formats not supporting it.
  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;

  return parameters;
}

ID3D11SamplerState* D3D11TextureCache::GetSampler(SamplerParameters parameters) {
  return impl_->GetSampler(parameters);
}

ID3D11ShaderResourceView* D3D11TextureCache::RequestSwapTexture(
    xenos::TextureFormat& format_out, uint32_t* width_out, uint32_t* height_out,
    const xenos::xe_gpu_texture_fetch_t* fetch_override, uint32_t* host_swizzle_out) {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = fetch_override ? *fetch_override : regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return nullptr;
  }
  // The gamma shaders sample a plain SRV and read only RGB, so the swizzle
  // (an SRV component mapping on Direct3D 12) is applied by the caller, or
  // must keep R, G and B in place.
  uint32_t host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  if (host_swizzle_out) {
    *host_swizzle_out = host_swizzle;
  }
  constexpr uint32_t kRGBMask = (UINT32_C(1) << 9) - 1;
  if (!host_swizzle_out &&
      (host_swizzle & kRGBMask) != (uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) & kRGBMask)) {
    if (!impl_->swap_swizzle_error_logged) {
      impl_->swap_swizzle_error_logged = true;
      REXGPU_ERROR(
          "D3D11TextureCache: The front buffer ({}) has the host swizzle {:03X} for RGB, which "
          "this caller can't apply on Direct3D 11; not presenting it",
          FormatInfo::Get(key.format)->name, host_swizzle & kRGBMask);
    }
    return nullptr;
  }
  auto* texture = static_cast<Impl::D3D11Texture*>(FindOrCreateTexture(key));
  if (texture == nullptr || !LoadTextureData(*texture)) {
    return nullptr;
  }
  texture->MarkAsUsed();
  ComPtr<ID3D11ShaderResourceView>& srv = texture->srv(false, kViewKindSwap2D);
  if (!srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = Impl::GetUnormFormat(key);
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
      return nullptr;
    }
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;
    if (FAILED(command_processor_.GetDevice()->CreateShaderResourceView(texture->resource(), &desc,
                                                                        &srv))) {
      REXGPU_ERROR("D3D11TextureCache: Failed to create the front buffer SRV");
      return nullptr;
    }
  }
  key = texture->key();
  if (width_out) {
    *width_out = key.GetWidth();
  }
  if (height_out) {
    *height_out = key.GetHeight();
  }
  format_out = key.format;
  return srv.Get();
}

bool D3D11TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  return Impl::IsSignedVersionSeparate(key);
}

uint32_t D3D11TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return Impl::kHostFormats[uint32_t(key.format)].swizzle;
}

uint32_t D3D11TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D11_REQ_TEXTURECUBE_DIMENSION;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t D3D11TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::CreateTexture(TextureKey key) {
  return impl_->CreateTexture(key);
}

bool D3D11TextureCache::TryLoadTextureDataFromCpu(Texture& texture, bool load_base,
                                                  bool load_mips, bool resolve_sourced) {
  return impl_->TryLoadFromCpu(static_cast<Impl::D3D11Texture&>(texture), load_base, load_mips,
                               resolve_sourced);
}

bool D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  return impl_->LoadFromResidentMemory(static_cast<Impl::D3D11Texture&>(texture), load_base,
                                       load_mips);
}

}  // namespace rex::graphics::d3d11
