#pragma once
// CPU import of 2D BC3 guest-format assets. No renderer or GPU dependency.
#include <span>
#include <stdexcept>
#include <vector>
#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::texture_util {
inline std::vector<uint8_t> ImportBc3(const xenos::xe_gpu_texture_fetch_t& fetch,
                                      std::span<const uint8_t> base,
                                      std::span<const uint8_t> mips) {
  const auto require = [](bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  };
  require(fetch.type == xenos::FetchConstantType::kTexture &&
              fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
              rex::graphics::GetBaseFormat(fetch.format) == xenos::TextureFormat::k_DXT4_5,
          "Only non-array BC3 textures are qualified");
  uint32_t width, height, base_page, mip_page, max_level;
  GetSubresourcesFromFetchConstant(fetch, &width, &height, nullptr, &base_page, &mip_page, nullptr,
                                   &max_level);
  ++width;
  ++height;
  require(base_page && width <= 8192 && height <= 8192, "Missing base or invalid extent");
  auto layout =
      GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled,
                            xenos::TextureFormat::k_DXT4_5, fetch.packed_mips, true, max_level);
  require(base.size() >= layout.base.level_data_extent_bytes &&
              mips.size() >= layout.mips_total_extent_bytes,
          "Truncated texture data");
  std::vector<uint8_t> output;
  for (uint32_t mip = 0; mip <= max_level; ++mip) {
    const auto stored = std::min(mip, layout.packed_level);
    const auto& level = mip ? layout.mips[stored] : layout.base;
    const auto source = mip ? mips : base;
    const uint32_t level_offset = mip ? layout.mip_offsets_bytes[stored] : 0;
    uint32_t ox = 0, oy = 0, oz = 0;
    if (fetch.packed_mips)
      GetPackedMipOffset(width, height, 1, xenos::TextureFormat::k_DXT4_5, mip, ox, oy, oz);
    require(!oz, "Unexpected volume offset");
    const uint32_t columns = (std::max(width >> mip, 1u) + 3) / 4;
    const uint32_t rows = (std::max(height >> mip, 1u) + 3) / 4;
    for (uint32_t y = 0; y < rows; ++y) {
      for (uint32_t x = 0; x < columns; ++x) {
        const uint64_t offset =
            uint64_t(level_offset) +
            (fetch.tiled ? uint32_t(GetTiledOffset2D(x + ox, y + oy, level.row_pitch_bytes / 16, 4))
                         : uint64_t(y + oy) * level.row_pitch_bytes + uint64_t(x + ox) * 16);
        require(offset <= source.size() && source.size() - offset >= 16, "Block outside source");
        // Byte permutations for none, 8-in-16, 8-in-32 and 16-in-32.
        constexpr uint32_t masks[] = {0, 1, 3, 2};
        for (uint32_t i = 0; i < 16; ++i)
          output.push_back(source[size_t(offset) + (i ^ masks[uint32_t(fetch.endianness)])]);
      }
    }
  }
  return output;
}
}  // namespace rex::graphics::texture_util
