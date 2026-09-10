#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace rex::graphics::d3d12 {
// Qualify only the non-indexed, zero-start draw contract used by this fixture.
// The vertex program ignores the fetch size, so rebasing without this check
// could turn reads from adjacent guest data into out-of-bounds native reads.
inline std::optional<std::pair<uint32_t, uint32_t>>
geometry_range(std::span<const uint32_t, 8> system, float scale, uint32_t count,
               uint32_t fetch_address, uint32_t fetch_size) {
  constexpr uint32_t physical_size = 1u << 29, index_mask = 0xFFFFFF;
  const uint32_t address = fetch_address & ~3u;
  const uint32_t size = ((fetch_size >> 2) & index_mask) * 4;
  if (!count || count > index_mask + 1 || (system[0] & 1) || system[4] != 0 ||
      system[6] > system[7] || system[7] > index_mask ||
      !std::isfinite(scale) || scale < 0 || !size || address >= physical_size ||
      size > physical_size - address)
    return std::nullopt;
  const uint64_t last = uint64_t(system[5] & index_mask) + count - 1;
  if (last > index_mask)
    return std::nullopt;
  // The closing vertex, if present, maps to index zero before adding the
  // offset. It can only lower this upper bound when offset wrapping is
  // excluded.
  const uint32_t maximum = std::clamp(uint32_t(last), system[6], system[7]);
  // One float ULP outward is conservative around integral products, including
  // differing host rounding modes. Small positive denormals still fetch row 0.
  const float upper = std::nextafter(float(double(maximum) * double(scale)),
                                     std::numeric_limits<float>::infinity());
  if (!std::isfinite(upper) || (std::floor(double(upper)) + 1) * 40 > size)
    return std::nullopt;
  return std::pair{address, size};
}

// Decode the exact immutable host indices bound by a zero-base-vertex draw.
inline std::optional<uint32_t> geometry_index_maximum(
    std::span<const uint32_t, 8> system, std::span<const uint8_t> indices,
    uint32_t index_width, bool primitive_reset) {
  constexpr uint32_t index_mask = 0xFFFFFF;
  if ((index_width != 2 && index_width != 4) || indices.empty() ||
      indices.size() % index_width || (system[0] & 1u) || system[4] > 3 ||
      system[6] > system[7] || system[7] > index_mask) return std::nullopt;
  uint32_t maximum = 0;
  bool has_vertex = false;
  for (size_t offset = 0; offset < indices.size(); offset += index_width) {
    uint32_t index = 0;
    if (index_width == 2) {
      uint16_t short_index;
      std::memcpy(&short_index, indices.data() + offset, sizeof(short_index));
      index = short_index;
    } else {
      std::memcpy(&index, indices.data() + offset, sizeof(index));
    }
    if (primitive_reset && index == (index_width == 2 ? 0xFFFFu : 0xFFFFFFFFu)) continue;
    has_vertex = true;
    if (index == system[3]) index = 0;
    if (system[4] == 1 || system[4] == 2) {
      index = ((index & 0x00FF00FFu) << 8) | ((index >> 8) & 0x00FF00FFu);
    }
    if (system[4] == 2 || system[4] == 3) index = (index << 16) | (index >> 16);
    index = std::clamp((index + system[5]) & index_mask, system[6], system[7]);
    maximum = std::max(maximum, index);
  }
  return has_vertex ? std::optional<uint32_t>(maximum) : std::nullopt;
}

// Bound the exact immutable host-index snapshot used by a zero-base-vertex
// depth or packed scene draw. The caller must bind that snapshot, not re-read
// mutable indices, and supply the shader's final vertex read extent.
inline std::optional<std::pair<uint32_t, uint32_t>>
depth_geometry_range(std::span<const uint32_t, 8> system,
                     std::span<const uint8_t> indices, uint32_t index_width,
                     uint32_t stride, uint32_t fetch_address, uint32_t fetch_size,
                     bool primitive_reset, uint32_t vertex_bytes = 12) {
  constexpr uint32_t physical_size = 1u << 29, index_mask = 0xFFFFFF;
  const uint32_t address = fetch_address & ~3u;
  const uint32_t size = ((fetch_size >> 2) & index_mask) * 4;
  if ((stride != 12 && stride != 20 && stride != 24 && stride != 28 && stride != 32 &&
       !(stride == 16 && vertex_bytes == 16)) ||
      !vertex_bytes || vertex_bytes > stride || (vertex_bytes & 3u) ||
      !size || address >= physical_size || size > physical_size - address) {
    return std::nullopt;
  }
  const auto maximum = geometry_index_maximum(system, indices, index_width, primitive_reset);
  if (!maximum) return std::nullopt;
  const uint64_t required = uint64_t(*maximum) * stride + vertex_bytes;
  if (required > size) return std::nullopt;
  return std::pair{address, uint32_t(required)};
}

// B848 selects two packed transforms using unsigned byte indices plus c156.x.
// Bound all 256 possible byte values so this proof does not depend on mutable
// vertex contents. The primary stream is bounded by depth_geometry_range.
inline std::optional<std::pair<uint32_t, uint32_t>>
skinned_transform_range(float offset, uint32_t fetch_address, uint32_t fetch_size) {
  constexpr uint32_t physical_size = 1u << 29;
  const uint32_t base = fetch_address & ~3u;
  const uint32_t size = ((fetch_size >> 2) & 0xFFFFFFu) * 4;
  if (!std::isfinite(offset) || offset < 0 || !size || base >= physical_size ||
      size > physical_size - base) return std::nullopt;
  // Outward rounding also covers a different host rounding mode for the add.
  const float upper = std::nextafter(float(double(offset) + 255.0),
                                     std::numeric_limits<float>::infinity());
  if (!std::isfinite(upper) || upper >= 2147483648.0f) return std::nullopt;
  const uint64_t first = uint64_t(std::floor(double(offset))) * 12;
  const uint64_t end = (uint64_t(std::floor(double(upper))) + 1) * 12;
  if (end > size) return std::nullopt;
  return std::pair{base + uint32_t(first), uint32_t(end - first)};
}

// Fetch pairs and returned ranges are ordered: vertices (95), directions (90),
// control grid (89). An unused direction range is {0, 0}. The maximum must
// describe the immutable indices actually bound, as for depth_geometry_range.
inline std::optional<std::array<std::pair<uint32_t, uint32_t>, 3>>
terrain_geometry_ranges(uint32_t maximum, std::span<const float, 44> constants,
                        std::span<const uint32_t, 6> fetches) {
  if (maximum > 0xFFFFFFu) return std::nullopt;
  const float weight_y = constants[36], weight_x = constants[37];
  const float lower = constants[38], upper = constants[39];
  if (!std::isfinite(weight_y) || !std::isfinite(weight_x) ||
      !std::isfinite(lower) || !std::isfinite(upper) ||
      weight_y < 0 || weight_x < 0 || lower < 0 || upper < lower)
    return std::nullopt;
  // Round positive bounds outward after each GPU float operation, accounting
  // for its separate rounding and denormal flushing. Overflow means fallback.
  const auto up = [](double value) {
    if (value > std::numeric_limits<float>::max()) return std::numeric_limits<float>::infinity();
    return value == 0 ? 0.0f : std::nextafter(float(value), std::numeric_limits<float>::infinity());
  };
  const float scale = std::abs(constants[31]);
  if (!std::isfinite(scale)) return std::nullopt;
  for (uint32_t i = 0; i < 3; ++i) {
    const float origin = constants[28 + i], cell = constants[16 + i], grid = constants[20 + i];
    if (!std::isfinite(origin) || !std::isfinite(cell) || !std::isfinite(grid)) return std::nullopt;
    // Decoded SNORM components and their products with each other are <= 1.
    const float delta = up(double(std::abs(origin)) + std::abs(cell));
    const float local = up(double(delta) + scale);
    const float grid_scale = up(double(std::abs(grid)) * weight_x);
    if (!std::isfinite(local) || !std::isfinite(grid_scale) ||
        !std::isfinite(up(double(grid_scale) * local))) return std::nullopt;
  }
  const float term_y = up(double(upper) * weight_y), term_x = up(double(upper) * weight_x);
  const float sum = up(double(term_y) + term_x);
  const float last = up(double(sum) + upper);
  // HLSL converts floor(grid_offset) through a signed integer before uint.
  if (!std::isfinite(last) || last >= 2147483648.0f) return std::nullopt;
  std::array<std::pair<uint32_t, uint32_t>, 3> ranges{};
  const uint64_t bytes[] = {uint64_t(maximum) * 28 + 8,
                            uint64_t(maximum) * 4 + 4,
                            (uint64_t(std::floor(double(last))) + 1) * 32};
  for (uint32_t i = 0; i < 3; ++i) {
    if (i == 1 && constants[32] == 0) continue;
    const uint32_t address = fetches[i * 2] & ~3u;
    const uint32_t size = ((fetches[i * 2 + 1] >> 2) & 0xFFFFFFu) * 4;
    if (!size || address >= (1u << 29) || size > (1u << 29) - address || bytes[i] > size)
      return std::nullopt;
    ranges[i] = {address, uint32_t(bytes[i])};
  }
  return ranges;
}

}  // namespace rex::graphics::d3d12
