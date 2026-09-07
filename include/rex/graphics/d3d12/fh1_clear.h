#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace rex::graphics::d3d12 {

// Shader-equivalent position/color fetch for 1E6883, after index/range proof.
// Reject floating-point edge cases rather than depend on host denormal modes.
inline std::optional<std::array<float, 8>> fh1_clear_vertex(
    std::span<const uint8_t, 28> bytes, uint32_t endian,
    std::span<const uint32_t, 120> system, bool has_color) {
  if (endian > 3) return std::nullopt;
  const auto regular = [](float v) {
    return std::isfinite(v) && (v == 0 || std::isnormal(v));
  };
  std::array<float, 8> result{};
  result[3] = 1;
  for (size_t i = 0; i < (has_color ? 7u : 3u); ++i) {
    uint32_t word;
    std::memcpy(&word, bytes.data() + i * 4, 4);
    if (endian == 1 || endian == 2)
      word = ((word & 0x00FF00FFu) << 8) | ((word >> 8) & 0x00FF00FFu);
    if (endian == 2 || endian == 3) word = (word << 16) | (word >> 16);
    float value = std::bit_cast<float>(word);
    if (!regular(value)) return std::nullopt;
    if (i < 3) {
      const float scale = std::bit_cast<float>(system[32 + i]);
      const float offset = std::bit_cast<float>(system[36 + i]);
      if (!regular(scale) || !regular(offset)) return std::nullopt;
      // Match the separate MUL followed by the shader's explicit NDC MAD.
      const float scaled = value * scale;
      if (!regular(scaled)) return std::nullopt;
      value = std::fma(offset, 1.0f, scaled);
      if (!regular(value)) return std::nullopt;
      result[i] = value;
    } else {
      result[i + 1] = value;
    }
  }
  return result;
}

struct Fh1ClearRectangle {
  std::array<int32_t, 4> bounds;  // left, top, right, bottom; empty is allowed.
  float depth;
};

// Lower three post-VS corners of FH1's rectangle-list primitive. Callers must
// separately qualify shaders, raster/depth/stencil state and memory visibility.
inline std::optional<Fh1ClearRectangle> fh1_clear_rectangle(
    std::span<const std::array<float, 4>, 3> vertices,
    std::array<uint32_t, 4> viewport,  // x, y, width, height
    std::array<float, 2> depth_range,
    std::array<uint32_t, 4> scissor) {  // x, y, width, height
  if (!std::isfinite(depth_range[0]) || !std::isfinite(depth_range[1]) ||
      depth_range[0] < 0 || depth_range[1] > 1 ||
      depth_range[0] > depth_range[1]) return std::nullopt;
  std::array<std::array<int32_t, 2>, 3> points;
  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto& v = vertices[i];
    for (float component : v) {
      if (!std::isfinite(component) ||
          (component != 0 && !std::isnormal(component))) return std::nullopt;
    }
    if (v[3] != 1 || v[2] != vertices[0][2]) return std::nullopt;
    const double xy[2] = {
        double(viewport[0]) + (double(v[0]) + 1) * viewport[2] * 0.5,
        double(viewport[1]) + (1 - double(v[1])) * viewport[3] * 0.5};
    for (size_t axis = 0; axis < 2; ++axis) {
      if (xy[axis] < std::numeric_limits<int32_t>::min() ||
          xy[axis] > std::numeric_limits<int32_t>::max() ||
          std::trunc(xy[axis]) != xy[axis]) return std::nullopt;
      points[i][axis] = int32_t(xy[axis]);
    }
  }
  const int32_t left = std::min({points[0][0], points[1][0], points[2][0]});
  const int32_t right = std::max({points[0][0], points[1][0], points[2][0]});
  const int32_t top = std::min({points[0][1], points[1][1], points[2][1]});
  const int32_t bottom = std::max({points[0][1], points[1][1], points[2][1]});
  if (left == right || top == bottom) return std::nullopt;
  uint32_t corners = 0;
  for (const auto& point : points) {
    if ((point[0] != left && point[0] != right) ||
        (point[1] != top && point[1] != bottom)) return std::nullopt;
    const uint32_t bit = 1u << (uint32_t(point[0] == right) |
                               (uint32_t(point[1] == bottom) << 1));
    if (corners & bit) return std::nullopt;
    corners |= bit;
  }
  const int64_t clipped_left = std::max<int64_t>(left, scissor[0]);
  const int64_t clipped_top = std::max<int64_t>(top, scissor[1]);
  const int64_t clipped_right = std::min<int64_t>(right, uint64_t(scissor[0]) + scissor[2]);
  const int64_t clipped_bottom = std::min<int64_t>(bottom, uint64_t(scissor[1]) + scissor[3]);
  Fh1ClearRectangle result{{0, 0, 0, 0}, std::clamp(
      std::fma(vertices[0][2], depth_range[1] - depth_range[0], depth_range[0]), 0.0f, 1.0f)};
  if (clipped_right > clipped_left && clipped_bottom > clipped_top) {
    result.bounds = {int32_t(clipped_left), int32_t(clipped_top),
                     int32_t(clipped_right), int32_t(clipped_bottom)};
  }
  return result;
}

}  // namespace rex::graphics::d3d12
