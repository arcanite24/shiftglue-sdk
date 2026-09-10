#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include <xxhash.h>

namespace rex::graphics {

// A content contract for the title's eight-level reflection face list. Addresses
// are checked separately; this signature is never an allocation/lifetime key.
struct Fh1MipChain {
  static constexpr uint32_t kCommandBytes = 6944;
  static constexpr uint32_t kBaseBytes = 6 * 256 * 256 * 4;
  static constexpr uint32_t kTotalBytes = 2211840;
  uint32_t base = 0, face = 0;
  uint32_t pixel_shader = 0, constants = 0, vertices = 0;
  std::array<uint32_t, 8> resolve_vertices{};
};

inline uint32_t Fh1MipReadWord(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline void Fh1MipWriteWord(uint8_t* p, uint32_t word) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = uint8_t(word >> (24 - i * 8));
}

inline bool Fh1MipPhysicalRange(uint32_t address, uint32_t bytes) {
  return bytes && bytes <= 0x20000000u && address <= 0x20000000u - bytes;
}

inline bool Fh1MipInheritedState(uint32_t screen_tl, uint32_t screen_br, uint32_t min_index,
                                 uint32_t max_index, uint32_t vs_constants, uint32_t ps_constants) {
  return (screen_tl & 0x7FFF7FFFu) == 0 && (screen_br & 0x7FFF) >= 128 &&
         ((screen_br >> 16) & 0x7FFF) >= 128 && min_index == 0 && max_index >= 23 &&
         (vs_constants == 0 || vs_constants == 0xFF000) &&
         (ps_constants == 0 || ps_constants == 0xFF100);
}

// Parse a private snapshot. Every non-address command bit must match the
// checked signature, including state restoration, clears and cache events.
// Unknown sequences fall back without any GPU work or register modification.
inline bool ParseFh1MipChain(std::span<const uint8_t> commands, Fh1MipChain& out,
                             XXH128_hash_t* inspected_signature = nullptr) {
  if (commands.size() != Fh1MipChain::kCommandBytes)
    return false;
  std::array<uint8_t, Fh1MipChain::kCommandBytes> normalized;
  std::memcpy(normalized.data(), commands.data(), commands.size());
  Fh1MipChain chain;
  std::array<uint32_t, 8> sources{}, destinations{};
  uint32_t draws = 0, copies = 0, texture_addresses = 0;
  uint32_t vf0 = 0, texture_address = 0, destination = 0;
  const auto word = [&](uint32_t i) { return Fh1MipReadWord(commands.data() + i * 4); };
  const auto normalize = [&](uint32_t i, uint32_t v) {
    Fh1MipWriteWord(normalized.data() + i * 4, v);
  };
  const auto same_address = [](uint32_t& stored, uint32_t address) {
    if (!address || (stored && stored != address))
      return false;
    stored = address;
    return true;
  };
  constexpr uint32_t count = Fh1MipChain::kCommandBytes / 4;
  for (uint32_t i = 0; i < count;) {
    const uint32_t header = word(i++), type = header >> 30;
    const uint32_t size = type == 2 ? 0 : ((header >> 16) & 0x3FFF) + 1;
    if (type == 1 || size > count - i)
      return false;
    if (type == 0) {
      for (uint32_t j = 0; j < size; ++j) {
        const uint32_t reg = (header & 0x7FFF) + ((header & 0x8000) ? 0 : j);
        const uint32_t value = word(i + j);
        if (reg == 0x4800)
          vf0 = value;
        if (reg == 0x2319) {
          destination = value;
          normalize(i + j, 0);
        } else if (reg == 0x4801 && (vf0 & 3) != 3) {
          texture_address = value & ~0xFFFu;
          ++texture_addresses;
          normalize(i + j, value & 0xFFF);
        } else if ((reg == 0x4800 || reg == 0x48BE) && (value & 3) == 3) {
          if (reg == 0x48BE && !same_address(chain.vertices, value & ~3u))
            return false;
          normalize(i + j, 3);
        }
      }
    } else if (type == 3) {
      switch ((header >> 8) & 0x7F) {
        case 0x27:
          if (size != 2 || !same_address(chain.pixel_shader, word(i) & ~3u))
            return false;
          normalize(i, word(i) & 3);
          break;
        case 0x2F:
          if (size != 3 || !same_address(chain.constants, word(i) & 0x3FFFFFFF))
            return false;
          normalize(i, 0);
          break;
        case 0x22:
          if (draws >= 8 || draws != copies)
            return false;
          sources[draws++] = texture_address;
          break;
        case 0x36:
          if (copies >= 8 || draws != copies + 1 || (vf0 & 3) != 3)
            return false;
          chain.resolve_vertices[copies] = vf0 & ~3u;
          destinations[copies++] = destination;
          break;
        case 0x2B:  // Inline shaders, part of the complete signature.
        case 0x3B:  // Invalidation.
        case 0x46:  // Cache flush, preserved during execution.
          break;
        default:
          return false;
      }
    }
    i += size;
  }
  if (draws != 8 || copies != 8 || texture_addresses != 9 || texture_address != sources.back())
    return false;
  const auto signature = XXH3_128bits(normalized.data(), normalized.size());
  if (inspected_signature)
    *inspected_signature = signature;
  if (signature.low64 != 0xF440A066CCDCC1C6ull || signature.high64 != 0x8EF8C435101EC4CDull)
    return false;

  constexpr int64_t base_stride = 256 * 256 * 4, first_mip_stride = 128 * 128 * 4;
  const int64_t face_numerator = Fh1MipChain::kBaseBytes - (int64_t(destinations[0]) - sources[0]);
  if (face_numerator < 0 || face_numerator % (base_stride - first_mip_stride))
    return false;
  const int64_t face = face_numerator / (base_stride - first_mip_stride);
  const int64_t base = int64_t(sources[0]) - face * base_stride;
  if (face >= 6 || base <= 0 || base > 0x20000000 - Fh1MipChain::kTotalBytes || (base & 4095))
    return false;
  chain.base = uint32_t(base);
  chain.face = uint32_t(face);
  uint32_t source_offset = 0, destination_offset = Fh1MipChain::kBaseBytes;
  for (uint32_t level = 0; level < 8; ++level) {
    const uint32_t source_pitch = (256u >> level) < 32 ? 32 : (256u >> level);
    const uint32_t dest_pitch = (128u >> level) < 32 ? 32 : (128u >> level);
    if (sources[level] !=
            chain.base + source_offset + chain.face * source_pitch * source_pitch * 4 ||
        destinations[level] !=
            chain.base + destination_offset + chain.face * dest_pitch * dest_pitch * 4)
      return false;
    source_offset = destination_offset;
    destination_offset += 6 * dest_pitch * dest_pitch * 4;
  }
  const auto external = [&](uint32_t address, uint32_t bytes) {
    return Fh1MipPhysicalRange(address, bytes) &&
           (address + bytes <= chain.base || address >= chain.base + Fh1MipChain::kTotalBytes);
  };
  if (!external(chain.pixel_shader, 84) || !external(chain.constants, 64) ||
      !external(chain.vertices, 384))
    return false;
  for (auto address : chain.resolve_vertices)
    if (!external(address, 24))
      return false;
  out = chain;
  return true;
}

// CPU-authoritative snapshots only. The caller supplies the existing shared
// memory snapshot helper, which rejects GPU-written pages and concurrent writes.
template <typename CopyCpuSnapshot>
bool CheckFh1MipInputs(const Fh1MipChain& chain, CopyCpuSnapshot copy) {
  std::array<uint8_t, 384> data;
  if (!copy(chain.pixel_shader, std::span(data).first(84)) ||
      XXH3_64bits(data.data(), 84) != 0x21937679208E59A5ull)
    return false;
  if (!copy(chain.constants, std::span(data).first(64)))
    return false;
  for (uint32_t i = 0; i < 16; ++i) {
    const float value = i == 12 ? 1280.0f : i == 13 ? 720.0f : 0.0f;
    uint32_t expected;
    std::memcpy(&expected, &value, 4);
    if (Fh1MipReadWord(data.data() + i * 4) != expected)
      return false;
  }
  if (!copy(chain.vertices, std::span(data)))
    return false;
  for (uint32_t strip = 0; strip < 8; ++strip) {
    const float left = -1.0f + strip * 0.25f, u = strip * 0.125f;
    const float expected[12] = {left, 1, u, 0, left + .25f, 1, u + .125f, 0, left, -1, u, 1};
    for (uint32_t i = 0; i < 12; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &expected[i], 4);
      if (Fh1MipReadWord(data.data() + (strip * 12 + i) * 4) != bits)
        return false;
    }
  }
  for (uint32_t level = 0; level < 8; ++level) {
    if (!copy(chain.resolve_vertices[level], std::span(data).first(24)))
      return false;
    const float side = float((128u >> level) < 8 ? 8 : (128u >> level));
    const float expected[6] = {0, 0, side, 0, side, side};
    for (uint32_t i = 0; i < 6; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &expected[i], 4);
      if (Fh1MipReadWord(data.data() + i * 4) != bits)
        return false;
    }
  }
  return true;
}

}  // namespace rex::graphics
