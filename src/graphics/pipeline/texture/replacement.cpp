/**
 * @file        graphics/pipeline/texture/replacement.cpp
 *
 * @brief       Texture dumps and replacements shared by the texture caches.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(texture_dump_dir, "", "GPU",
                      "Write each DXT1, DXT3 and DXT5 texture the title loads as <hash>.dds "
                      "into this folder (texture modding)");
REXCVAR_DEFINE_STRING(texture_replacement_dirs, "", "GPU",
                      "Folders of <hash>.dds texture replacements, separated by ';', "
                      "earlier folders first; read when the first texture loads");
REXCVAR_DEFINE_INT32(texture_replacement_reload, 0, "GPU",
                     "Change to rescan texture_replacement_dirs and reload every texture at "
                     "the next frame (clears the GPU caches once)");

namespace rex::graphics::texture_replacement {

namespace {

// DXGI_FORMAT values, so this file needs no D3D12 headers.
constexpr uint32_t kDxgiR8G8B8A8Typeless = 27, kDxgiR8G8B8A8Unorm = 28,
                   kDxgiR8G8B8A8UnormSrgb = 29, kDxgiBc1Typeless = 70, kDxgiBc1Unorm = 71,
                   kDxgiBc1UnormSrgb = 72, kDxgiBc2Typeless = 73, kDxgiBc2Unorm = 74,
                   kDxgiBc2UnormSrgb = 75, kDxgiBc3Typeless = 76, kDxgiBc3Unorm = 77,
                   kDxgiBc3UnormSrgb = 78, kDxgiBc4Typeless = 79, kDxgiBc4Unorm = 80,
                   kDxgiBc5Typeless = 82, kDxgiBc5Unorm = 83;

constexpr Format kFormats[] = {
    {xenos::TextureFormat::k_DXT1, 4, 8, 0x31545844u /* DXT1 */, 0, kDxgiBc1Unorm,
     kDxgiBc1UnormSrgb, kDxgiBc1Typeless},
    {xenos::TextureFormat::k_DXT2_3, 4, 16, 0x33545844u /* DXT3 */, 0, kDxgiBc2Unorm,
     kDxgiBc2UnormSrgb, kDxgiBc2Typeless},
    {xenos::TextureFormat::k_DXT4_5, 4, 16, 0x35545844u /* DXT5 */, 0, kDxgiBc3Unorm,
     kDxgiBc3UnormSrgb, kDxgiBc3Typeless},
    {xenos::TextureFormat::k_DXT5A, 4, 8, 0x31495441u /* ATI1 */, 0x55344342u /* BC4U */,
     kDxgiBc4Unorm, kDxgiBc4Unorm, kDxgiBc4Typeless},
    {xenos::TextureFormat::k_DXN, 4, 16, 0x32495441u /* ATI2 */, 0x55354342u /* BC5U */,
     kDxgiBc5Unorm, kDxgiBc5Unorm, kDxgiBc5Typeless},
    // Channels as stored (the fetch constant's swizzle picks them).
    {xenos::TextureFormat::k_8_8_8_8, 1, 4, 0, 0, kDxgiR8G8B8A8Unorm, kDxgiR8G8B8A8UnormSrgb,
     kDxgiR8G8B8A8Typeless},
};

constexpr uint32_t kDdsMagic = 0x20534444u;    // "DDS "
constexpr uint32_t kFourCcDx10 = 0x30315844u;  // "DX10"

// As texture_util::ImportBc3, for any of the formats, from a texture's key
// and guest layout.
bool Untile(const Source& key, const Format& format, const texture_util::TextureGuestLayout& layout,
            std::span<const uint8_t> base, std::span<const uint8_t> mips, uint32_t levels,
            std::vector<uint8_t>& output) {
  const uint32_t width = key.width, height = key.height;
  const uint32_t block = format.block_bytes;
  const uint32_t block_log2 = block == 4 ? 2 : block == 8 ? 3 : 4;
  // Byte permutations for none, 8-in-16, 8-in-32 and 16-in-32.
  constexpr uint32_t kMasks[] = {0, 1, 3, 2};
  for (uint32_t mip = 0; mip < levels; ++mip) {
    const uint32_t stored = std::min(mip, layout.packed_level);
    const auto& level = mip ? layout.mips[stored] : layout.base;
    const auto source = mip ? mips : base;
    const uint32_t level_offset = mip ? layout.mip_offsets_bytes[stored] : 0;
    uint32_t ox = 0, oy = 0, oz = 0;
    if (key.packed_mips) {
      texture_util::GetPackedMipOffset(width, height, 1, format.guest, mip, ox, oy, oz);
    }
    const uint32_t columns = Blocks(format, width, mip);
    const uint32_t rows = Blocks(format, height, mip);
    for (uint32_t y = 0; y < rows; ++y) {
      for (uint32_t x = 0; x < columns; ++x) {
        const uint64_t offset =
            uint64_t(level_offset) +
            (key.tiled ? uint64_t(uint32_t(texture_util::GetTiledOffset2D(
                             int32_t(x + ox), int32_t(y + oy), level.row_pitch_bytes / block,
                             block_log2)))
                       : uint64_t(y + oy) * level.row_pitch_bytes + uint64_t(x + ox) * block);
        if (offset > source.size() || source.size() - offset < block) return false;
        for (uint32_t i = 0; i < block; ++i) {
          output.push_back(source[size_t(offset) + (i ^ kMasks[key.endianness & 3])]);
        }
      }
    }
  }
  return true;
}

void WriteDds(const std::filesystem::path& path, const Image& image) {
  uint32_t header[32] = {};
  header[0] = kDdsMagic;
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;  // caps..linear size
  header[3] = image.height;
  header[4] = image.width;
  header[5] = uint32_t(LevelBytes(*image.format, image.width, image.height, 0));
  header[7] = image.levels;
  header[19] = 32;   // pixel format size
  header[20] = 0x4;  // fourcc
  header[21] = image.format->fourcc ? image.format->fourcc : kFourCcDx10;
  header[27] = 0x1000 | (image.levels > 1 ? 0x400000 | 0x8 : 0);
  // DX10 header: format, 2D, no flags, one array slice.
  const uint32_t dx10[5] = {image.format->dxgi_unorm, 3, 0, 1, 0};
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return;
  file.write(reinterpret_cast<const char*>(header), sizeof(header));
  if (!image.format->fourcc) file.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
  file.write(reinterpret_cast<const char*>(image.data.data()), std::streamsize(image.data.size()));
}

std::optional<Image> ReadDds(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  const auto word = [&](size_t index) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + index * 4, 4);
    return value;
  };
  if (bytes.size() < 128 || word(0) != kDdsMagic || word(1) != 124) return std::nullopt;
  Image image;
  size_t data_offset = 128;
  if (word(21) == kFourCcDx10) {
    if (bytes.size() < 148) return std::nullopt;
    for (const auto& format : kFormats) {
      if (word(32) == format.dxgi_unorm || word(32) == format.dxgi_srgb) {
        image.format = &format;
      }
    }
    data_offset = 148;
  } else if (word(20) & 0x4) {
    for (const auto& format : kFormats) {
      if (format.fourcc && (word(21) == format.fourcc ||
                            (format.fourcc_alternative && word(21) == format.fourcc_alternative))) {
        image.format = &format;
      }
    }
  } else if ((word(20) & 0x40) && word(22) == 32 && word(23) == 0xFFu && word(24) == 0xFF00u &&
             word(25) == 0xFF0000u) {
    // Uncompressed 32 bits, R in the low byte (as the 8_8_8_8 dumps store it).
    image.format = FindFormat(xenos::TextureFormat::k_8_8_8_8);
  }
  if (!image.format) return std::nullopt;
  image.height = word(3);
  image.width = word(4);
  image.levels = std::max(word(7), 1u);
  size_t needed = 0;
  for (uint32_t level = 0; level < image.levels; ++level) {
    needed += LevelBytes(*image.format, image.width, image.height, level);
  }
  if (!image.width || !image.height || bytes.size() - data_offset < needed) return std::nullopt;
  image.data.assign(bytes.begin() + data_offset, bytes.begin() + data_offset + needed);
  return image;
}

struct State {
  std::mutex mutex;
  bool scanned = false;
  std::unordered_map<uint64_t, std::filesystem::path> files;
  std::unordered_map<uint64_t, std::shared_ptr<const Image>> loaded;
  std::unordered_set<uint64_t> dumped;
  std::unordered_set<uint64_t> logged;
  bool reload_seen = false;
  int32_t reload = 0;
};

State& GetState() {
  static State state;
  return state;
}

// With the mutex held.
void ScanIfNeeded(State& state) {
  if (state.scanned) return;
  state.scanned = true;
  auto scanned = ScanReplacementFolders(REXCVAR_GET(texture_replacement_dirs));
  for (const auto& problem : scanned.problems) REXGPU_WARN("Texture replacements: {}", problem);
  state.files = std::move(scanned.files);
  if (!state.files.empty()) {
    REXGPU_INFO("Texture replacements: {} files ({} aliases)", state.files.size(),
                scanned.aliases);
  }
}

}  // namespace

const Format* FindFormat(xenos::TextureFormat format) {
  for (const auto& entry : kFormats) {
    if (entry.guest == format) return &entry;
  }
  return nullptr;
}

uint32_t Blocks(const Format& format, uint32_t size, uint32_t level) {
  return (std::max(size >> level, 1u) + format.block_width - 1) / format.block_width;
}

size_t LevelBytes(const Format& format, uint32_t width, uint32_t height, uint32_t level) {
  return size_t(Blocks(format, width, level)) * Blocks(format, height, level) * format.block_bytes;
}

void Reset() {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  state.scanned = false;
  state.files.clear();
  state.loaded.clear();
  state.logged.clear();
}

bool ReloadRequested() {
  auto& state = GetState();
  const int32_t reload = REXCVAR_GET(texture_replacement_reload);
  std::lock_guard lock(state.mutex);
  if (!state.reload_seen) {
    state.reload_seen = true;
    state.reload = reload;
    return false;
  }
  if (reload == state.reload) return false;
  state.reload = reload;
  return true;
}

bool IsEligible(const Source& source) {
  return FindFormat(source.format) && !source.scaled_resolve && source.base_page &&
         !source.signed_separate && source.dimension == xenos::DataDimension::k2DOrStacked &&
         source.depth_or_array_size == 1 && !source.force_load_3d_tiling;
}

LookupResult Find(const Source& source, const texture_util::TextureGuestLayout& layout,
                  SharedMemory& shared_memory, Lookup& lookup) {
  const std::string dump_dir = REXCVAR_GET(texture_dump_dir);
  auto& state = GetState();
  {
    std::lock_guard lock(state.mutex);
    ScanIfNeeded(state);
    if (dump_dir.empty() && state.files.empty()) return LookupResult::kNotEligible;
  }
  if (!IsEligible(source)) return LookupResult::kNotEligible;
  const Format* format = FindFormat(source.format);
  std::vector<uint8_t> base(layout.base.level_data_extent_bytes);
  if (base.empty() ||
      !shared_memory.CopyCpuRange(source.base_page << 12, {base.data(), base.size()})) {
    return LookupResult::kNotEligible;
  }
  const uint64_t hash = XXH3_64bits(base.data(), base.size());

  if (!dump_dir.empty()) {
    bool dump = false;
    {
      std::lock_guard lock(state.mutex);
      dump = state.dumped.insert(hash).second;
    }
    if (dump) {
      std::vector<uint8_t> mips(layout.mips_total_extent_bytes);
      const bool have_mips = source.mip_max_level && source.mip_page && !mips.empty() &&
                             shared_memory.CopyCpuRange(source.mip_page << 12,
                                                        {mips.data(), mips.size()});
      Image image{format, source.width, source.height, have_mips ? source.mip_max_level + 1u : 1u};
      if (Untile(source, *format, layout, base, mips, image.levels, image.data)) {
        WriteDds(rex::to_path(dump_dir) / fmt::format("{:016X}.dds", hash), image);
      }
    }
  }

  std::shared_ptr<const Image> image;
  {
    std::lock_guard lock(state.mutex);
    const auto file = state.files.find(hash);
    if (file == state.files.end()) return LookupResult::kNone;
    auto& slot = state.loaded[hash];
    if (!slot) {
      auto read = ReadDds(file->second);
      if (!read) {
        REXGPU_WARN("Texture replacement {} is not a DDS of a supported format",
                    rex::path_to_utf8(file->second));
        state.files.erase(file);
        state.loaded.erase(hash);
        return LookupResult::kNone;
      }
      slot = std::make_shared<const Image>(std::move(*read));
    }
    image = slot;
  }
  // The replacement may be larger by a power of two (the same in both
  // directions, up to 8x); it then keeps the guest's level count plus the
  // levels it has below that.
  const uint32_t guest_levels = source.mip_max_level + 1u;
  uint32_t scale_log2 = 0;
  while (scale_log2 < 3 && image->width > (source.width << scale_log2)) ++scale_log2;
  const bool size_valid = image->width == (source.width << scale_log2) &&
                          image->height == (source.height << scale_log2);
  if (image->format != format || !size_valid || image->levels < guest_levels) {
    std::lock_guard lock(state.mutex);
    if (state.logged.insert(hash).second) {
      REXGPU_WARN(
          "Texture replacement {:016X}: needs {}x{} (or 2x, 4x or 8x that) with the dumped "
          "format and at least {} levels",
          hash, source.width, source.height, guest_levels);
    }
    return LookupResult::kNone;
  }
  lookup.format = format;
  lookup.hash = hash;
  lookup.image = std::move(image);
  lookup.scale_log2 = scale_log2;
  lookup.levels = std::min(lookup.image->levels, guest_levels + scale_log2);
  return LookupResult::kFound;
}

void LogLoaded(const Lookup& lookup) {
  auto& state = GetState();
  std::lock_guard lock(state.mutex);
  if (state.logged.insert(lookup.hash).second) {
    REXGPU_INFO("Texture replacement {:016X}: {}x{}, {} levels", lookup.hash, lookup.image->width,
                lookup.image->height, lookup.levels);
  }
}

}  // namespace rex::graphics::texture_replacement
