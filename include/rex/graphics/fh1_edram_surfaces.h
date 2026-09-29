#pragma once
// EDRAM surface identity for the FH1 native executor (NP-9.0): the key a
// native surface is found by, and the geometry derived from guest render
// target state. API-agnostic; backends map keys to their own resources.

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Same fields as the Xenos RenderTargetKey so the two can be compared.
struct Fh1SurfaceKey {
  uint32_t base_tiles = 0;
  uint32_t pitch_tiles = 0;  // At 32bpp.
  uint32_t msaa = 0;         // xenos::MsaaSamples.
  bool is_depth = false;
  uint32_t format = 0;  // DepthRenderTargetFormat or color storage format.

  uint32_t Pack() const {
    return base_tiles | (pitch_tiles << 11) | (msaa << 19) | (uint32_t(is_depth) << 21) |
           (format << 22);
  }
  bool Is64bpp() const {
    return !is_depth &&
           xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(format));
  }
  std::string Describe() const {
    static const char* kMsaa[] = {"1x", "2x", "4x", "?"};
    return std::to_string(base_tiles) + "/" + std::to_string(pitch_tiles) + "/" +
           kMsaa[msaa & 3] + (is_depth ? "/d" : "/c") + std::to_string(format);
  }

  // A color surface; 8_8_8_8_GAMMA shares 8_8_8_8's storage unless the
  // backend keeps gamma in 16-bit unorm.
  static Fh1SurfaceKey Color(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                             xenos::ColorRenderTargetFormat format, bool gamma_as_unorm16) {
    Fh1SurfaceKey key;
    key.base_tiles = base & (xenos::kEdramTileCount - 1);
    key.pitch_tiles = pitch_tiles;
    key.msaa = msaa;
    key.is_depth = false;
    format = xenos::GetStorageColorFormat(format);
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA && !gamma_as_unorm16) {
      format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    }
    key.format = uint32_t(format);
    return key;
  }

  static Fh1SurfaceKey Depth(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                             xenos::DepthRenderTargetFormat format) {
    Fh1SurfaceKey key;
    key.base_tiles = base & (xenos::kEdramTileCount - 1);
    key.pitch_tiles = pitch_tiles;
    key.msaa = msaa;
    key.is_depth = true;
    key.format = uint32_t(format);
    return key;
  }
};

// The surface pitch in 32bpp EDRAM tiles for a pitch in guest pixels.
inline uint32_t Fh1PitchTiles(uint32_t pitch_pixels, uint32_t msaa) {
  const uint32_t msaa_x_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k4X));
  return ((pitch_pixels << msaa_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
         xenos::kEdramTileWidthSamples;
}

// A surface's height in guest pixels: down to the start of the same surface
// in the next EDRAM addressing period, clamped to the guest texture size
// limit and to what a host texture can hold at the resolution scale
// (`max_host_dimension` / `scale`), as the Xenos render target cache did.
inline uint32_t Fh1SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa,
                                 uint32_t max_host_dimension, uint32_t scale) {
  if (!pitch_tiles) return 0;
  uint32_t tile_rows = (xenos::kEdramTileCount + pitch_tiles - 1) / pitch_tiles;
  const uint32_t msaa_y_log2 = uint32_t(msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t max_height =
      std::min(uint32_t(xenos::kTexture2DCubeMaxWidthHeight), max_host_dimension / scale);
  tile_rows = std::min(tile_rows, (max_height << msaa_y_log2) / xenos::kEdramTileHeightSamples);
  return tile_rows * (xenos::kEdramTileHeightSamples >> msaa_y_log2);
}

}  // namespace rex::graphics
