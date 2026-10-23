#include <rex/graphics/texture_transcode_astc.h>

#include <algorithm>
#include <array>
#include <cstdlib>

namespace rex::graphics {
namespace {

// ASTC block layout used here: a 4x4 weight grid with 2-bit weights (block
// mode 0x042, or 0x442 with a second weight plane). One partition, CEM 8
// (RGB direct, 6 values at 8 bits) or CEM 12 (RGBA direct). With the second
// plane the 8 endpoint values have 45 bits left, which makes them trit range
// 48 (4 bits plus a trit).
constexpr uint16_t kModeOnePlane = 0x042;
constexpr uint16_t kModeTwoPlanes = 0x442;
constexpr int kCemRgb = 8;
constexpr int kCemRgba = 12;
constexpr int kEndpointRange = 48;
constexpr int kWeightLevels[4] = {0, 21, 43, 64};  // ASTC unquantised 2-bit weights

// Unquantised value (0-255) of each trit range 48 code, as the decoder
// reads them (ASTC spec, colour endpoint unquantisation).
constexpr std::array<uint8_t, 48> kUnquant48 = {
    0,  255, 16, 239, 32, 223, 48, 207, 65, 190, 81, 174, 97,  158, 113, 142,
    5,  250, 21, 234, 38, 217, 54, 201, 70, 185, 86, 169, 103, 152, 119, 136,
    11, 244, 27, 228, 43, 212, 59, 196, 76, 179, 92, 163, 108, 147, 124, 131};

struct Bits {
  uint8_t bytes[16] = {};
  void Put(int pos, uint32_t value, int count) {
    for (int i = 0; i < count; ++i) {
      if ((value >> i) & 1)
        bytes[(pos + i) >> 3] |= uint8_t(1 << ((pos + i) & 7));
    }
  }
  uint32_t Get(int pos, int count) const {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      value |= uint32_t((bytes[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
    }
    return value;
  }
};

// Trit packing (ASTC spec C.2.12): 5 trits <-> 8 bits, built by inverting the
// spec's decode for all 256 values.
struct TritTables {
  std::array<std::array<uint8_t, 5>, 256> unpack{};
  std::array<uint8_t, 243> pack{};
  TritTables() {
    for (int t = 0; t < 256; ++t) {
      auto bit = [&](int i) { return (t >> i) & 1; };
      int c, t4, t3;
      if (((t >> 2) & 7) == 7) {
        c = (bit(7) << 4) | (bit(6) << 3) | (bit(5) << 2) | (bit(1) << 1) | bit(0);
        t4 = t3 = 2;
      } else {
        c = t & 31;
        if (((t >> 5) & 3) == 3) {
          t4 = 2;
          t3 = bit(7);
        } else {
          t4 = bit(7);
          t3 = (t >> 5) & 3;
        }
      }
      int t2, t1, t0;
      if ((c & 3) == 3) {
        t2 = 2;
        t1 = (c >> 4) & 1;
        int c3 = (c >> 3) & 1, c2 = (c >> 2) & 1;
        t0 = (c3 << 1) | (c2 & ~c3 & 1);
      } else if (((c >> 2) & 3) == 3) {
        t2 = t1 = 2;
        t0 = c & 3;
      } else {
        t2 = (c >> 4) & 1;
        t1 = (c >> 2) & 3;
        int c1 = (c >> 1) & 1, c0 = c & 1;
        t0 = (c1 << 1) | (c0 & ~c1 & 1);
      }
      unpack[t] = {uint8_t(t0), uint8_t(t1), uint8_t(t2), uint8_t(t3), uint8_t(t4)};
      int key = t0 + 3 * t1 + 9 * t2 + 27 * t3 + 81 * t4;
      pack[key] = uint8_t(t);
    }
  }
};
const TritTables& Trits() {
  static const TritTables tables;
  return tables;
}

// Writes/reads n values of trit range 48 starting at bit pos: groups of five
// as m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7], m being the low 4 bits.
constexpr int kTritLow[5] = {0, 2, 4, 5, 7};    // T bit index after value i
constexpr int kTritWidth[5] = {2, 2, 1, 2, 1};  // T bits after value i
void PutEndpoints(Bits& bits, int pos, const uint8_t* codes, int n) {
  const TritTables& tables = Trits();
  for (int g = 0; g < n; g += 5) {
    int key = 0, mul = 1;
    for (int i = 0; i < 5; ++i, mul *= 3)
      key += mul * (g + i < n ? codes[g + i] >> 4 : 0);
    uint8_t t = tables.pack[key];
    for (int i = 0; i < 5 && g + i < n; ++i) {
      bits.Put(pos, codes[g + i] & 15, 4);
      pos += 4;
      bits.Put(pos, (t >> kTritLow[i]) & ((1 << kTritWidth[i]) - 1), kTritWidth[i]);
      pos += kTritWidth[i];
    }
  }
}
void GetEndpoints(const Bits& bits, int pos, uint8_t* codes, int n) {
  const TritTables& tables = Trits();
  for (int g = 0; g < n; g += 5) {
    uint32_t t = 0;
    uint8_t low[5] = {};
    for (int i = 0; i < 5 && g + i < n; ++i) {
      low[i] = uint8_t(bits.Get(pos, 4));
      pos += 4;
      t |= bits.Get(pos, kTritWidth[i]) << kTritLow[i];
      pos += kTritWidth[i];
    }
    for (int i = 0; i < 5 && g + i < n; ++i)
      codes[g + i] = uint8_t((tables.unpack[t][i] << 4) | low[i]);
  }
}

uint8_t QuantEndpoint(int value) {
  int best = 0;
  for (int q = 1; q < kEndpointRange; ++q) {
    if (std::abs(kUnquant48[q] - value) < std::abs(kUnquant48[best] - value))
      best = q;
  }
  return uint8_t(best);
}

// Weight w (0-3) of each texel; stream bit k lands on block bit 127 - k.
void PutWeights(Bits& bits, const uint8_t* weights, int count) {
  for (int k = 0; k < count * 2; ++k) {
    if ((weights[k >> 1] >> (k & 1)) & 1)
      bits.bytes[(127 - k) >> 3] |= uint8_t(1 << ((127 - k) & 7));
  }
}
uint8_t GetWeight(const Bits& bits, int index) {
  int w = 0;
  for (int b = 0; b < 2; ++b) {
    int k = index * 2 + b;
    w |= ((bits.bytes[(127 - k) >> 3] >> ((127 - k) & 7)) & 1) << b;
  }
  return uint8_t(w);
}

struct Rgb {
  int r, g, b;
  int Sum() const { return r + g + b; }
};
Rgb Expand565(uint16_t c) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}
int NearestWeight(int position64) {
  int best = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::abs(kWeightLevels[i] - position64) < std::abs(kWeightLevels[best] - position64))
      best = i;
  }
  return best;
}

// Writes one block. alpha_weights == nullptr selects the RGB-only layout.
void WriteBlock(Rgb e0, Rgb e1, uint8_t* colour_weights, const int* alpha_range,
                const uint8_t* alpha_weights, uint8_t out[16]) {
  Bits bits;
  if (!alpha_weights) {
    // The decoder takes blue-contraction when the second sum is smaller, so
    // order the endpoints and flip the weights instead.
    if (e1.Sum() < e0.Sum()) {
      std::swap(e0, e1);
      for (int i = 0; i < 16; ++i)
        colour_weights[i] = uint8_t(3 - colour_weights[i]);
    }
    bits.Put(0, kModeOnePlane, 11);
    bits.Put(11, 0, 2);
    bits.Put(13, kCemRgb, 4);
    bits.Put(17, e0.r, 8);
    bits.Put(25, e1.r, 8);
    bits.Put(33, e0.g, 8);
    bits.Put(41, e1.g, 8);
    bits.Put(49, e0.b, 8);
    bits.Put(57, e1.b, 8);
    PutWeights(bits, colour_weights, 16);
  } else {
    uint8_t q[8] = {QuantEndpoint(e0.r),           QuantEndpoint(e1.r),
                    QuantEndpoint(e0.g),           QuantEndpoint(e1.g),
                    QuantEndpoint(e0.b),           QuantEndpoint(e1.b),
                    QuantEndpoint(alpha_range[0]), QuantEndpoint(alpha_range[1])};
    if (kUnquant48[q[1]] + kUnquant48[q[3]] + kUnquant48[q[5]] <
        kUnquant48[q[0]] + kUnquant48[q[2]] + kUnquant48[q[4]]) {
      std::swap(q[0], q[1]);
      std::swap(q[2], q[3]);
      std::swap(q[4], q[5]);
      for (int i = 0; i < 16; ++i)
        colour_weights[i] = uint8_t(3 - colour_weights[i]);
    }
    bits.Put(0, kModeTwoPlanes, 11);
    bits.Put(11, 0, 2);
    bits.Put(13, kCemRgba, 4);
    PutEndpoints(bits, 17, q, 8);
    bits.Put(128 - 64 - 2, 3, 2);  // alpha is the component with its own weights
    uint8_t interleaved[32];
    for (int i = 0; i < 16; ++i) {
      interleaved[2 * i] = colour_weights[i];
      interleaved[2 * i + 1] = alpha_weights[i];
    }
    PutWeights(bits, interleaved, 32);
  }
  std::copy(bits.bytes, bits.bytes + 16, out);
}

}  // namespace

void TranscodeBc1ToAstc4x4(const uint8_t bc1[8], uint8_t astc[16]) {
  uint16_t c0 = uint16_t(bc1[0] | (bc1[1] << 8)), c1 = uint16_t(bc1[2] | (bc1[3] << 8));
  uint32_t indices = bc1[4] | (bc1[5] << 8) | (bc1[6] << 16) | (uint32_t(bc1[7]) << 24);
  Rgb e0 = Expand565(c0), e1 = Expand565(c1);
  bool four_colour = c0 > c1;
  // Position of each BC index along e0 -> e1, in 64ths.
  const int position[2][4] = {{0, 64, 32, 0}, {0, 64, 21, 43}};
  uint8_t colour[16], alpha[16];
  bool transparent = false;
  for (int i = 0; i < 16; ++i) {
    int index = (indices >> (2 * i)) & 3;
    colour[i] = uint8_t(NearestWeight(position[four_colour][index]));
    bool clear = !four_colour && index == 3;
    transparent |= clear;
    alpha[i] = clear ? 0 : 3;
  }
  const int alpha_range[2] = {0, 255};
  WriteBlock(e0, e1, colour, alpha_range, transparent ? alpha : nullptr, astc);
}

namespace {
// Shared by BC2 and BC3: the 16 decoded alphas plus the BC colour block.
void TranscodeWithAlpha(const int alpha[16], const uint8_t* colour_block, uint8_t astc[16]) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) {
    lo = std::min(lo, alpha[i]);
    hi = std::max(hi, alpha[i]);
  }
  uint16_t c0 = uint16_t(colour_block[0] | (colour_block[1] << 8));
  uint16_t c1 = uint16_t(colour_block[2] | (colour_block[3] << 8));
  uint32_t indices = colour_block[4] | (colour_block[5] << 8) | (colour_block[6] << 16) |
                     (uint32_t(colour_block[7]) << 24);
  const int position[4] = {0, 64, 21, 43};  // BC2/BC3 colour is always 4-colour
  uint8_t colour[16], alpha_weights[16];
  for (int i = 0; i < 16; ++i) {
    colour[i] = uint8_t(NearestWeight(position[(indices >> (2 * i)) & 3]));
    alpha_weights[i] = hi == lo ? 0 : uint8_t(NearestWeight((alpha[i] - lo) * 64 / (hi - lo)));
  }
  const int alpha_range[2] = {lo, hi};
  WriteBlock(Expand565(c0), Expand565(c1), colour, alpha_range, alpha_weights, astc);
}
}  // namespace

void TranscodeBc2ToAstc4x4(const uint8_t bc2[16], uint8_t astc[16]) {
  int alpha[16];
  for (int i = 0; i < 16; ++i)
    alpha[i] = ((bc2[i >> 1] >> (4 * (i & 1))) & 15) * 17;
  TranscodeWithAlpha(alpha, bc2 + 8, astc);
}

void TranscodeBc3ToAstc4x4(const uint8_t bc3[16], uint8_t astc[16]) {
  int a0 = bc3[0], a1 = bc3[1];
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i)
    alpha_bits |= uint64_t(bc3[2 + i]) << (8 * i);
  int ramp[8] = {a0, a1};
  for (int i = 1; i < (a0 > a1 ? 7 : 5); ++i) {
    ramp[1 + i] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : ((5 - i) * a0 + i * a1) / 5;
  }
  if (a0 <= a1) {
    ramp[6] = 0;
    ramp[7] = 255;
  }
  int alpha[16];
  for (int i = 0; i < 16; ++i)
    alpha[i] = ramp[(alpha_bits >> (3 * i)) & 7];
  TranscodeWithAlpha(alpha, bc3 + 8, astc);
}

bool DecodeTranscodedAstc4x4(const uint8_t astc[16], uint8_t rgba[64]) {
  Bits bits;
  std::copy(astc, astc + 16, bits.bytes);
  uint32_t mode = bits.Get(0, 11);
  uint32_t cem = bits.Get(13, 4);
  if (bits.Get(11, 2) != 0)
    return false;
  int e0[4] = {0, 0, 0, 255}, e1[4] = {0, 0, 0, 255};
  bool two_planes = mode == kModeTwoPlanes;
  if (mode == kModeOnePlane && cem == kCemRgb) {
    for (int c = 0; c < 3; ++c) {
      e0[c] = int(bits.Get(17 + 16 * c, 8));
      e1[c] = int(bits.Get(25 + 16 * c, 8));
    }
  } else if (two_planes && cem == kCemRgba && bits.Get(128 - 64 - 2, 2) == 3) {
    uint8_t q[8];
    GetEndpoints(bits, 17, q, 8);
    for (int c = 0; c < 4; ++c) {
      e0[c] = kUnquant48[q[2 * c]];
      e1[c] = kUnquant48[q[2 * c + 1]];
    }
  } else {
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    int w = kWeightLevels[GetWeight(bits, two_planes ? 2 * i : i)];
    int wa = two_planes ? kWeightLevels[GetWeight(bits, 2 * i + 1)] : 64;
    for (int c = 0; c < 4; ++c) {
      int weight = c == 3 ? wa : w;
      int a = e0[c] * 257, b = e1[c] * 257;
      rgba[4 * i + c] = uint8_t(((a * (64 - weight) + b * weight + 32) >> 6) >> 8);
    }
  }
  return true;
}

}  // namespace rex::graphics
