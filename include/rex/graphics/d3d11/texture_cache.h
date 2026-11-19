/**
 * @file        graphics/d3d11/texture_cache.h
 * @brief       Direct3D 11 (feature level 11_0) guest texture cache.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/d3d11/shader.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// Guest textures for feature level 11_0, unscaled only (draw resolution scale
// 1).
//
// Loading: the same texture load compute shaders as on Direct3D 12 (compiled
// for shader model 5.0) read the guest data from the shared memory mirror -
// which holds resolve output only on the GPU - and write the host layout into
// a linear scratch buffer. Direct3D 11 can't copy a buffer into a texture on
// the GPU, so a small compute shader then copies the scratch buffer into the
// texture through a typed *_UINT UAV of its typeless format family (typed UAV
// stores of those are required at 11_0). Block-compressed textures are
// written as an R32G32_UINT or R32G32B32A32_UINT texture of their blocks and
// copied into the BC texture with CopySubresourceRegion, which allows that
// reinterpretation.
//
// Host formats without a typed UAV store path at 11_0 are stored as R8G8B8A8:
// B5G6R5, B5G5R5A1 and B4G4R4A4 are loaded with the Direct3D 12 16-bit load
// shaders and expanded to 8 bits per component by the copy; R8G8_B8G8 and
// G8R8_G8B8 always take the Direct3D 12 decompression load shaders.
//
// Swizzles: SRVs have no component mapping in Direct3D 11. The host swizzle
// (TextureCache::GetActiveTextureHostSwizzle) is applied by the translated
// shaders from the system constants (DxbcShaderTranslator::SystemConstants::
// texture_swizzles), so the views here are plain.
//
// Threading: GPU commands thread only, on the device's immediate context;
// calls D3D11CommandProcessor::InvalidateGuestDrawState() after its
// dispatches.
class D3D11TextureCache final : public TextureCache {
 public:
  // Sampler parameters that can be directly converted to a host sampler.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      // For anisotropic, these are true.
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  static std::unique_ptr<D3D11TextureCache> Create(const RegisterFile& register_file,
                                                   D3D11SharedMemory& shared_memory,
                                                   D3D11CommandProcessor& command_processor);
  ~D3D11TextureCache();

  void ClearCache() override;
  void BeginFrame() override;
  void EndFrame();

  // TextureCache::RequestTextures, then the loads it queued run.
  void RequestTextures(uint32_t used_texture_mask) override;

  // The SRV to bind for a host shader texture binding after RequestTextures:
  // the texture of the binding's fetch constant (its signed version for a
  // signed binding when the format has one), or a null texture of the
  // binding's dimension (reading zeros) if there is no valid texture. Never
  // nullptr after a successful Initialize.
  ID3D11ShaderResourceView* GetActiveTextureSRV(const DxbcShader::TextureBinding& binding);

  SamplerParameters GetSamplerParameters(const DxbcShader::SamplerBinding& binding) const;
  // Cached by the parameters; never nullptr after a successful Initialize.
  ID3D11SamplerState* GetSampler(SamplerParameters parameters);

  // The front buffer texture for presentation (from fetch constant 0 as set
  // up by the swap, or fetch_override), loaded if needed. Returns nullptr if
  // there is no valid texture. The SRV is a plain 2D view of mip 0: Direct3D
  // 11 has no SRV component mapping, so the host swizzle (3 bits per
  // component, xenos::XE_GPU_TEXTURE_SWIZZLE_*) goes to host_swizzle_out for
  // the caller to apply. Without host_swizzle_out, a texture whose R, G and B
  // aren't in place is rejected.
  ID3D11ShaderResourceView* RequestSwapTexture(
      xenos::TextureFormat& format_out, uint32_t* width_out = nullptr,
      uint32_t* height_out = nullptr,
      const xenos::xe_gpu_texture_fetch_t* fetch_override = nullptr,
      uint32_t* host_swizzle_out = nullptr);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  // Texture replacements and the direct upload of CPU-written linear 8-bit
  // textures, as on Direct3D 12.
  bool TryLoadTextureDataFromCpu(Texture& texture, bool load_base, bool load_mips,
                                 bool resolve_sourced) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  D3D11TextureCache(const RegisterFile& register_file, D3D11SharedMemory& shared_memory,
                    D3D11CommandProcessor& command_processor);
  bool Initialize();

  // Implementation details (host format table, the texture class with its
  // unsigned and signed SRVs, load and copy compute shaders, the scratch
  // buffer, null textures, samplers) are private to the implementation.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  D3D11CommandProcessor& command_processor_;
  D3D11SharedMemory& d3d11_shared_memory_;
};

}  // namespace rex::graphics::d3d11
