/**
 * @file        graphics/vulkan/blue_noise.cpp
 *
 * @brief       A tileable blue noise texture, made at start (void and cluster).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/vulkan/shader_override_files.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rex::graphics::vulkan {

namespace {

// Ulichney's void-and-cluster method ("The void-and-cluster method for
// dither array generation", 1993): a binary pattern's "energy" at a texel is
// the sum of a Gaussian of the toroidal distance to every set texel; the
// tightest cluster is the set texel with the most energy and the largest
// void the empty texel with the least. Each texel's rank is the order in
// which it joins the pattern, and the rank spread over 0..255 is the noise.
class VoidAndCluster {
 public:
  VoidAndCluster(uint32_t size, uint32_t seed)
      : size_(size), count_(size * size), kernel_(count_), energy_(count_), set_(count_) {
    constexpr float kSigma = 1.5f;
    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        const float dx = float(std::min(x, size - x)), dy = float(std::min(y, size - y));
        kernel_[y * size + x] = std::exp(-(dx * dx + dy * dy) / (2.0f * kSigma * kSigma));
      }
    }
    // The initial pattern: a tenth of the texels, from a small LCG.
    uint32_t state = seed * 747796405u + 2891336453u;
    const auto next = [&state] {
      state = state * 1664525u + 1013904223u;
      return state >> 8;
    };
    const uint32_t initial = std::max(count_ / 10, 1u);
    for (uint32_t placed = 0; placed < initial;) {
      const uint32_t index = next() % count_;
      if (!set_[index]) {
        Toggle(index);
        ++placed;
      }
    }
  }

  std::vector<uint32_t> Ranks() {
    // Spread the initial pattern evenly: swap its tightest cluster into the
    // largest void until that changes nothing.
    for (uint32_t guard = 0; guard < count_; ++guard) {
      const uint32_t cluster = Find(true, true);
      Toggle(cluster);
      const uint32_t void_index = Find(false, false);
      if (void_index == cluster) {
        Toggle(cluster);
        break;
      }
      Toggle(void_index);
    }
    const std::vector<uint8_t> initial = set_;
    const std::vector<float> initial_energy = energy_;
    uint32_t ones = uint32_t(std::count(set_.begin(), set_.end(), uint8_t(1)));
    std::vector<uint32_t> rank(count_, 0);
    // Phase 1: the initial texels, ranked by removing tightest clusters.
    for (uint32_t r = ones; r-- > 0;) {
      const uint32_t cluster = Find(true, true);
      Toggle(cluster);
      rank[cluster] = r;
    }
    set_ = initial;
    energy_ = initial_energy;
    // Phase 2: fill the largest voids up to half.
    for (uint32_t r = ones; r < count_ / 2; ++r) {
      const uint32_t void_index = Find(false, false);
      Toggle(void_index);
      rank[void_index] = r;
    }
    // Phase 3: the rest, by the tightest cluster of the empty texels: the
    // energy of the empty ones is the kernel sum minus the set ones'.
    float total = 0.0f;
    for (float k : kernel_)
      total += k;
    for (uint32_t r = std::max(ones, count_ / 2); r < count_; ++r) {
      uint32_t best = 0;
      float best_energy = -1.0f;
      for (uint32_t i = 0; i < count_; ++i) {
        if (set_[i])
          continue;
        const float empty_energy = total - energy_[i];
        if (empty_energy > best_energy) {
          best_energy = empty_energy;
          best = i;
        }
      }
      Toggle(best);
      rank[best] = r;
    }
    return rank;
  }

 private:
  void Toggle(uint32_t index) {
    set_[index] ^= 1;
    const float sign = set_[index] ? 1.0f : -1.0f;
    const uint32_t ix = index % size_, iy = index / size_;
    for (uint32_t y = 0; y < size_; ++y) {
      const uint32_t ky = (y + size_ - iy) % size_;
      for (uint32_t x = 0; x < size_; ++x) {
        energy_[y * size_ + x] += sign * kernel_[ky * size_ + (x + size_ - ix) % size_];
      }
    }
  }
  // The set texel with the most energy (cluster) or the empty one with the
  // least (void).
  uint32_t Find(bool among_set, bool most) const {
    uint32_t best = 0;
    bool found = false;
    for (uint32_t i = 0; i < count_; ++i) {
      if (bool(set_[i]) != among_set)
        continue;
      if (!found || (most ? energy_[i] > energy_[best] : energy_[i] < energy_[best])) {
        best = i;
        found = true;
      }
    }
    return best;
  }

  uint32_t size_, count_;
  std::vector<float> kernel_, energy_;
  std::vector<uint8_t> set_;
};

}  // namespace

std::vector<uint8_t> GenerateBlueNoiseRgba8(uint32_t size) {
  std::vector<uint8_t> texels(size_t(size) * size * 4);
  const uint32_t count = size * size;
  for (uint32_t channel = 0; channel < 4; ++channel) {
    const std::vector<uint32_t> rank = VoidAndCluster(size, channel + 1).Ranks();
    for (uint32_t i = 0; i < count; ++i) {
      texels[size_t(i) * 4 + channel] = uint8_t((uint64_t(rank[i]) * 256) / count);
    }
  }
  return texels;
}

}  // namespace rex::graphics::vulkan
