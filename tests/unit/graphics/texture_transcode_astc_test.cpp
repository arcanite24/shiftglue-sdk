#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <rex/graphics/texture_transcode_astc.h>

using namespace rex::graphics;

namespace {

struct Rgba {
  int c[4];
};

Rgba Expand565(uint16_t v) {
  int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return {{(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255}};
}

// The reference BC1 decode (the same one the RGBA8 load shaders write).
void DecodeBc1(const uint8_t* b, Rgba out[16]) {
  uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
  Rgba p[4] = {Expand565(c0), Expand565(c1), {}, {}};
  for (int k = 0; k < 3; ++k) {
    if (c0 > c1) {
      p[2].c[k] = (2 * p[0].c[k] + p[1].c[k]) / 3;
      p[3].c[k] = (p[0].c[k] + 2 * p[1].c[k]) / 3;
    } else {
      p[2].c[k] = (p[0].c[k] + p[1].c[k]) / 2;
      p[3].c[k] = 0;
    }
  }
  p[2].c[3] = 255;
  p[3].c[3] = c0 > c1 ? 255 : 0;
  for (int i = 0; i < 16; ++i)
    out[i] = p[(idx >> (2 * i)) & 3];
}

void DecodeBc3(const uint8_t* b, Rgba out[16]) {
  DecodeBc1(b + 8, out);
  // BC3 colour is always the 4-colour form.
  uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
  uint32_t idx = b[12] | (b[13] << 8) | (b[14] << 16) | (uint32_t(b[15]) << 24);
  Rgba p[4] = {Expand565(c0), Expand565(c1), {}, {}};
  for (int k = 0; k < 3; ++k) {
    p[2].c[k] = (2 * p[0].c[k] + p[1].c[k]) / 3;
    p[3].c[k] = (p[0].c[k] + 2 * p[1].c[k]) / 3;
  }
  int a0 = b[0], a1 = b[1];
  int ramp[8] = {a0, a1};
  for (int i = 1; i < (a0 > a1 ? 7 : 5); ++i) {
    ramp[1 + i] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : ((5 - i) * a0 + i * a1) / 5;
  }
  if (a0 <= a1) {
    ramp[6] = 0;
    ramp[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i)
    bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) {
    out[i] = p[(idx >> (2 * i)) & 3];
    out[i].c[3] = ramp[(bits >> (3 * i)) & 7];
  }
}

void DecodeBc2(const uint8_t* b, Rgba out[16]) {
  uint8_t as_bc3[16];
  std::copy(b, b + 16, as_bc3);
  DecodeBc3(as_bc3, out);  // colour half is the same; alpha replaced below
  for (int i = 0; i < 16; ++i)
    out[i].c[3] = ((b[i >> 1] >> (4 * (i & 1))) & 15) * 17;
}

// PSNR over channels [first, last) of many blocks, by total squared error.
double Psnr(const Rgba* a, const Rgba* b, int count, int first, int last) {
  double sum = 0;
  for (int i = 0; i < count; ++i) {
    for (int k = first; k < last; ++k) {
      double d = a[i].c[k] - b[i].c[k];
      sum += d * d;
    }
  }
  double mse = sum / (double(count) * (last - first));
  return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

Rgba ToRgba(const uint8_t* rgba, int i) {
  return {{rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2], rgba[4 * i + 3]}};
}

}  // namespace

TEST_CASE("BC1 opaque blocks transcode to ASTC within 40 dB", "[graphics]") {
  std::mt19937 rng(1);
  std::vector<Rgba> want, got;
  for (int n = 0; n < 4000; ++n) {
    uint8_t bc[8];
    for (auto& v : bc)
      v = uint8_t(rng());
    uint16_t c0 = uint16_t(bc[0] | (bc[1] << 8)), c1 = uint16_t(bc[2] | (bc[3] << 8));
    if (c0 <= c1) {  // keep this case to the 4-colour form; 3-colour has its own test
      std::swap(bc[0], bc[2]);
      std::swap(bc[1], bc[3]);
      if (c0 == c1)
        bc[0] ^= 1;
    }
    Rgba ref[16];
    DecodeBc1(bc, ref);
    uint8_t astc[16], rgba[64];
    TranscodeBc1ToAstc4x4(bc, astc);
    REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
    for (int i = 0; i < 16; ++i) {
      want.push_back(ref[i]);
      got.push_back(ToRgba(rgba, i));
      REQUIRE(got.back().c[3] == 255);
    }
  }

  CHECK(Psnr(want.data(), got.data(), int(want.size()), 0, 3) >= 40.0);
}

TEST_CASE("A flat BC1 block transcodes to the same flat colour", "[graphics]") {
  // c0 = c1 = 0xFFFF would be 3-colour mode; use c0 > c1 with every index 0.
  const uint8_t bc[8] = {0x1F, 0xF8, 0x00, 0x00, 0, 0, 0, 0};
  Rgba ref[16];
  DecodeBc1(bc, ref);
  uint8_t astc[16], rgba[64];
  TranscodeBc1ToAstc4x4(bc, astc);
  REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 4; ++k)
      CHECK(std::abs(rgba[4 * i + k] - ref[i].c[k]) <= 3);
  }
}

TEST_CASE("BC1 punch-through texels stay transparent and the rest opaque", "[graphics]") {
  std::mt19937 rng(2);
  for (int n = 0; n < 500; ++n) {
    uint8_t bc[8];
    for (auto& v : bc)
      v = uint8_t(rng());
    uint16_t c0 = uint16_t(bc[0] | (bc[1] << 8)), c1 = uint16_t(bc[2] | (bc[3] << 8));
    if (c0 > c1) {
      std::swap(bc[0], bc[2]);
      std::swap(bc[1], bc[3]);
    }
    Rgba ref[16];
    DecodeBc1(bc, ref);
    uint8_t astc[16], rgba[64];
    TranscodeBc1ToAstc4x4(bc, astc);
    REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
    for (int i = 0; i < 16; ++i)
      CHECK(rgba[4 * i + 3] == ref[i].c[3]);
  }
}

TEST_CASE("BC3 blocks transcode to ASTC with bounded error", "[graphics]") {
  std::mt19937 rng(3);
  std::vector<Rgba> want, got;
  for (int n = 0; n < 4000; ++n) {
    uint8_t bc[16];
    for (auto& v : bc)
      v = uint8_t(rng());
    // 8-level alpha ramps only; the 6-level form with explicit 0 and 255 has its own test.
    if (bc[0] <= bc[1])
      std::swap(bc[0], bc[1]);
    Rgba ref[16];
    DecodeBc3(bc, ref);
    uint8_t astc[16], rgba[64];
    TranscodeBc3ToAstc4x4(bc, astc);
    REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
    for (int i = 0; i < 16; ++i) {
      want.push_back(ref[i]);
      got.push_back(ToRgba(rgba, i));
    }
  }
  int count = int(want.size());
  CHECK(Psnr(want.data(), got.data(), count, 0, 3) >= 40.0);
  // Alpha drops from 8 to 4 levels (the second weight plane leaves no room for 3 bits),
  // measured 28.6 dB on random ramps.
  CHECK(Psnr(want.data(), got.data(), count, 3, 4) >= 28.0);
}

TEST_CASE("BC2 blocks transcode to ASTC with bounded error", "[graphics]") {
  std::mt19937 rng(6);
  std::vector<Rgba> want, got;
  for (int n = 0; n < 4000; ++n) {
    uint8_t bc[16];
    for (auto& v : bc)
      v = uint8_t(rng());
    Rgba ref[16];
    DecodeBc2(bc, ref);
    uint8_t astc[16], rgba[64];
    TranscodeBc2ToAstc4x4(bc, astc);
    REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
    for (int i = 0; i < 16; ++i) {
      want.push_back(ref[i]);
      got.push_back(ToRgba(rgba, i));
    }
  }
  int count = int(want.size());
  CHECK(Psnr(want.data(), got.data(), count, 0, 3) >= 40.0);
  CHECK(Psnr(want.data(), got.data(), count, 3, 4) >= 20.0);
}

TEST_CASE("BC3 cut-out alpha keeps its 0 and 255 texels exactly", "[graphics]") {
  std::mt19937 rng(4);
  for (int n = 0; n < 500; ++n) {
    uint8_t bc[16];
    for (auto& v : bc)
      v = uint8_t(rng());
    bc[0] = 0;  // a0 <= a1: 6-level ramp plus explicit 0 and 255
    bc[1] = 255;
    Rgba ref[16];
    DecodeBc3(bc, ref);
    uint8_t astc[16], rgba[64];
    TranscodeBc3ToAstc4x4(bc, astc);
    REQUIRE(DecodeTranscodedAstc4x4(astc, rgba));
    for (int i = 0; i < 16; ++i) {
      if (ref[i].c[3] == 0 || ref[i].c[3] == 255)
        CHECK(rgba[4 * i + 3] == ref[i].c[3]);
    }
  }
}

TEST_CASE("Only the blocks the transcoder writes decode", "[graphics]") {
  uint8_t astc[16] = {}, rgba[64];
  CHECK_FALSE(DecodeTranscodedAstc4x4(astc, rgba));
}
