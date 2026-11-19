/**
 * @file        rex/graphics/pipeline/texture/replacement.h
 *
 * @brief       Texture dumps and replacements shared by the texture caches.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_replacement {

// Texture dumps and replacements (NP-10.3), for 2D textures the host loads
// by copying (after untiling and byte swapping): DXT1, DXT2/3, DXT4/5, DXT5A
// (BC4), DXN (BC5) and 8_8_8_8. Each texture is named by the XXH3 hash of its
// guest base level, and a replacement is a DDS file of the same format, the
// same size or 2x, 4x or 8x it, with at least as many levels.
struct Format {
  xenos::TextureFormat guest;
  // 4 (BCn) or 1 (uncompressed) texels per block side.
  uint32_t block_width;
  uint32_t block_bytes;
  // DDS pixel format four-character code; 0 writes a DX10 header.
  uint32_t fourcc;
  // A second code DDS writers use for the format (BC4U, BC5U), or 0.
  uint32_t fourcc_alternative;
  // DXGI_FORMAT values a DX10 header may name (and D3D12 resources use).
  uint32_t dxgi_unorm, dxgi_srgb, dxgi_typeless;
};

const Format* FindFormat(xenos::TextureFormat format);

struct Image {
  const Format* format = nullptr;
  uint32_t width = 0, height = 0, levels = 0;
  std::vector<uint8_t> data;  // levels in order, blocks row by row
};

uint32_t Blocks(const Format& format, uint32_t size, uint32_t level);
size_t LevelBytes(const Format& format, uint32_t width, uint32_t height, uint32_t level);

// The <hash>.dds files of texture_replacement_dirs (';'-separated, earlier
// folders winning), plus each folder's aliases.txt: one "<hash> <target>"
// per line, the target another hash or a file name in that folder, so
// several guest textures share one image.
struct ReplacementFiles {
  std::unordered_map<uint64_t, std::filesystem::path> files;
  size_t aliases = 0;
  std::vector<std::string> problems;
};
ReplacementFiles ScanReplacementFolders(std::string_view dirs);

// Rescans the folders and forgets loaded files at the next lookup
// (texture_replacement_reload).
void Reset();

// Whether texture_replacement_reload changed since the last call; the first
// call only records it. A cache that sees true calls Reset and reloads every
// texture.
bool ReloadRequested();

// The parts of a texture key the dumps and replacements use (the key itself is
// private to the texture caches).
struct Source {
  xenos::TextureFormat format;
  xenos::DataDimension dimension;
  uint32_t width, height, depth_or_array_size;
  uint32_t mip_max_level;
  uint32_t base_page, mip_page;
  bool tiled, packed_mips, scaled_resolve, signed_separate;
  uint32_t endianness;
  bool force_load_3d_tiling;
};

// A texture the dumps and replacements can handle, apart from the backend's
// own conditions (a host format that is the guest format).
bool IsEligible(const Source& source);

struct Lookup {
  const Format* format = nullptr;
  uint64_t hash = 0;
  std::shared_ptr<const Image> image;
  // The replacement is this power of two larger in both directions.
  uint32_t scale_log2 = 0;
  // Levels to upload: the guest's, plus scale_log2 more when the file has them.
  uint32_t levels = 0;
};

enum class LookupResult {
  // Nothing to do: no folders, no dump folder, or the texture isn't eligible.
  kNotEligible,
  // An eligible texture without a usable replacement: the guest data loads.
  kNone,
  kFound,
};

// Hashes the texture's base level, dumps it (texture_dump_dir) the first time
// the hash is seen, and finds and validates its replacement.
LookupResult Find(const Source& source, const texture_util::TextureGuestLayout& layout,
                  SharedMemory& shared_memory, Lookup& lookup);

// Logs the first upload of a replacement.
void LogLoaded(const Lookup& lookup);

}  // namespace rex::graphics::texture_replacement
