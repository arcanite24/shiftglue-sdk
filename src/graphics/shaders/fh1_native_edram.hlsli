// FH1 native executor: guest EDRAM semantics over native host surfaces.
//
// A native surface keeps a guest render target in a host texture. The guest
// addresses EDRAM in 80x16-sample tiles; a surface's layout (base tile, pitch
// in tiles, MSAA) maps its pixels and samples to tiles, and its format defines
// the 32-bit word each sample holds. Reading a sample through another layout
// or format - as resolves and render-target aliasing do on the guest - means
// locating the EDRAM sample through the reader's layout, finding it in the
// source surface through the source's layout, encoding the source's host
// value as its guest word and decoding that word in the reader's format.
//
// Source textures: FH1_SOURCE_DEPTH selects depth and stencil sources instead
// of a color source, FH1_SOURCE_UINT a color source read as raw channel bits
// (formats with 16-bit or 32-bit float channels, 32bpp or 64bpp, which the
// guest stores as the host's bits), FH1_SOURCE_MSAA multisampled ones.
//
// A 64bpp sample takes two adjacent 32-bit EDRAM columns of an 80x16 tile,
// the low half first, so a 64bpp surface row of tiles is twice its 32bpp
// pitch.

#ifndef FH1_NATIVE_EDRAM_HLSLI_
#define FH1_NATIVE_EDRAM_HLSLI_

#ifdef FH1_SOURCE_DEPTH
#ifdef FH1_SOURCE_MSAA
Texture2DMS<float> fh1_source_depth : register(t0);
Texture2DMS<uint2> fh1_source_stencil : register(t1);
#else
Texture2D<float> fh1_source_depth : register(t0);
Texture2D<uint2> fh1_source_stencil : register(t1);
#endif
#elif defined(FH1_SOURCE_UINT)
#ifdef FH1_SOURCE_MSAA
Texture2DMS<uint4> fh1_source_color : register(t0);
#else
Texture2D<uint4> fh1_source_color : register(t0);
#endif
#else
#ifdef FH1_SOURCE_MSAA
Texture2DMS<float4> fh1_source_color : register(t0);
#else
Texture2D<float4> fh1_source_color : register(t0);
#endif
#endif

// Guest ColorRenderTargetFormat values.
#define FORMAT_8_8_8_8 0u
#define FORMAT_8_8_8_8_GAMMA 1u
#define FORMAT_2_10_10_10 2u
#define FORMAT_2_10_10_10_FLOAT 3u
#define FORMAT_16_16 4u
#define FORMAT_16_16_16_16 5u
#define FORMAT_16_16_FLOAT 6u
#define FORMAT_16_16_16_16_FLOAT 7u
#define FORMAT_2_10_10_10_AS_10_10_10_10 10u
#define FORMAT_2_10_10_10_FLOAT_AS_16_16_16_16 12u
#define FORMAT_32_FLOAT 14u
#define FORMAT_32_32_FLOAT 15u
// Guest DepthRenderTargetFormat values.
#define DEPTH_D24S8 0u
#define DEPTH_D24FS8 1u

// Flags shared by the shaders.
#define FH1_FLAG_FLOAT24_ROUND 1u     // round float24 depth to nearest even
#define FH1_FLAG_GAMMA_UNORM16 2u     // gamma targets hold linear unorm16 values
#define FH1_FLAG_FIXED16_FULL_RANGE 4u  // 16_16[_16_16] snorm hosts hold guest / 32

// Scale from a 16_16[_16_16] host snorm value to the guest value: 32 when the
// host keeps the full -32...32 range as snorm / 32, 1 when it truncates to
// -1...1 (the resolve's exponent bias then restores the range). Set by main.
static float fh1_fixed16_scale = 32.0f;

// Layouts: base_tiles 0:10, pitch_tiles (32bpp) 11:18, msaa 19:20,
// is_64bpp 21, is_depth 22, guest format 23:26 (color or depth format),
// host sample mode 27:28 (0: as guest, 1: native 2x, 2: 2x stored as 4x).
uint LayoutBase(uint layout) { return layout & 0x7FFu; }
uint LayoutPitch(uint layout) { return (layout >> 11u) & 0xFFu; }
uint LayoutMsaa(uint layout) { return (layout >> 19u) & 3u; }
uint LayoutIs64bpp(uint layout) { return (layout >> 21u) & 1u; }
uint LayoutIsDepth(uint layout) { return (layout >> 22u) & 1u; }
uint LayoutFormat(uint layout) { return (layout >> 23u) & 0xFu; }
uint LayoutHostSampleMode(uint layout) { return (layout >> 27u) & 3u; }

// xenos::Float32To20e4.
uint Float32To20e4(float f32, bool round_to_nearest_even) {
  uint bits = asuint(f32);
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  if (round_to_nearest_even) {
    bits += 3u + ((bits >> 3u) & 1u);
  }
  uint result = (bits >> 3u) & 0xFFFFFFu;
  // Positive only (not -0 or NaN), saturating.
  result = asuint(f32) >= 0x3FFFFFF8u ? 0xFFFFFFu : result;
  return f32 > 0.0f ? result : 0u;
}

// xenos::Float20e4To32.
float Float20e4To32(uint f24) {
  f24 &= 0xFFFFFFu;
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u && mantissa != 0u) {
    uint lzcnt = 31u - firstbithigh(mantissa) - 11u;
    exponent = uint(1 - int(lzcnt));
    mantissa = (mantissa << lzcnt) & 0xFFFFFu;
  }
  return f24 != 0u ? asfloat(((exponent + 112u) << 23u) | (mantissa << 3u)) : 0.0f;
}

// Float to 7e3 with round to nearest even, saturating to [0, 31.875].
uint Float32To7e3(float f32) {
  uint bits = asuint(clamp(f32, 0.0f, 31.875f));
  uint biased = bits < 0x3E800000u
                    ? ((0x800000u | (bits & 0x7FFFFFu)) >> min(125u - (bits >> 23u), 24u))
                    : (bits + 0xC2000000u);
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

// xenos::Float7e3To32.
float Float7e3To32(uint f10) {
  f10 &= 0x3FFu;
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u && mantissa != 0u) {
    uint lzcnt = 31u - firstbithigh(mantissa) - 24u;
    exponent = uint(1 - int(lzcnt));
    mantissa = (mantissa << lzcnt) & 0x7Fu;
  }
  return f10 != 0u ? asfloat(((exponent + 124u) << 23u) | (mantissa << 16u)) : 0.0f;
}

// xenos::LinearToPWLGamma.
float LinearToPWLGamma(float value) {
  value = saturate(value);
  float scale, offset;
  if (value >= 128.0f / 1023.0f) {
    if (value >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (value >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  return trunc(value * scale) * (1.0f / 255.0f) + offset;
}

// xenos::PWLGammaToLinear.
float PWLGammaToLinear(float gamma) {
  gamma = saturate(gamma);
  float scale, offset;
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (gamma >= 64.0f / 255.0f) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  float value = gamma * ((255.0f * 1024.0f) * scale) + offset;
  value += trunc(value * scale);
  return value * (1.0f / 1023.0f);
}

// A 16_16[_16_16] channel's guest value; both -1 snorm encodings decode to -1.
float DecodeSnorm16(uint bits) {
  return max(float(int(bits << 16u) >> 16) * (1.0f / 32767.0f), -1.0f) * fh1_fixed16_scale;
}

uint PackUnorm(float value, float scale) {
  return uint(saturate(value) * scale + 0.5f);
}

// The guest EDRAM word of a color value in the given format.
uint EncodeColor(float4 color, uint format) {
  switch (format) {
    case FORMAT_8_8_8_8:
    case FORMAT_8_8_8_8_GAMMA:
      return PackUnorm(color.r, 255.0f) | (PackUnorm(color.g, 255.0f) << 8u) |
             (PackUnorm(color.b, 255.0f) << 16u) | (PackUnorm(color.a, 255.0f) << 24u);
    case FORMAT_2_10_10_10:
    case FORMAT_2_10_10_10_AS_10_10_10_10:
      return PackUnorm(color.r, 1023.0f) | (PackUnorm(color.g, 1023.0f) << 10u) |
             (PackUnorm(color.b, 1023.0f) << 20u) | (PackUnorm(color.a, 3.0f) << 30u);
    case FORMAT_2_10_10_10_FLOAT:
    case FORMAT_2_10_10_10_FLOAT_AS_16_16_16_16:
      return Float32To7e3(color.r) | (Float32To7e3(color.g) << 10u) |
             (Float32To7e3(color.b) << 20u) | (PackUnorm(color.a, 3.0f) << 30u);
    case FORMAT_32_FLOAT:
      return asuint(color.r);
    default:
      return 0u;
  }
}

float4 DecodeColor(uint word, uint format) {
  switch (format) {
    case FORMAT_8_8_8_8:
    case FORMAT_8_8_8_8_GAMMA:
      return float4(word & 0xFFu, (word >> 8u) & 0xFFu, (word >> 16u) & 0xFFu, word >> 24u) *
             (1.0f / 255.0f);
    case FORMAT_2_10_10_10:
    case FORMAT_2_10_10_10_AS_10_10_10_10:
      return float4(float3(word & 0x3FFu, (word >> 10u) & 0x3FFu, (word >> 20u) & 0x3FFu) *
                        (1.0f / 1023.0f),
                    float(word >> 30u) * (1.0f / 3.0f));
    case FORMAT_2_10_10_10_FLOAT:
    case FORMAT_2_10_10_10_FLOAT_AS_16_16_16_16:
      return float4(Float7e3To32(word), Float7e3To32(word >> 10u), Float7e3To32(word >> 20u),
                    float(word >> 30u) * (1.0f / 3.0f));
    case FORMAT_32_FLOAT:
      return float4(asfloat(word), 0.0f, 0.0f, 0.0f);
    case FORMAT_16_16:
      return float4(DecodeSnorm16(word), DecodeSnorm16(word >> 16u), 0.0f, 0.0f);
    case FORMAT_16_16_FLOAT:
      return float4(f16tof32(word), f16tof32(word >> 16u), 0.0f, 0.0f);
    default:
      return 0.0f;
  }
}

// A 64bpp EDRAM sample (low word first) as a color.
float4 DecodeColor64(uint2 words, uint format) {
  switch (format) {
    case FORMAT_16_16_16_16:
      return float4(DecodeSnorm16(words.x), DecodeSnorm16(words.x >> 16u),
                    DecodeSnorm16(words.y), DecodeSnorm16(words.y >> 16u));
    case FORMAT_16_16_16_16_FLOAT:
      return float4(f16tof32(words.x), f16tof32(words.x >> 16u), f16tof32(words.y),
                    f16tof32(words.y >> 16u));
    case FORMAT_32_32_FLOAT:
      return float4(asfloat(words.x), asfloat(words.y), 0.0f, 0.0f);
    default:
      return 0.0f;
  }
}

// Host sample of a guest sample index in a surface.
uint HostSample(uint guest_sample, uint msaa, uint host_mode) {
  uint host_sample = guest_sample;
  if (msaa == 1u && host_mode == 1u) {
    host_sample = guest_sample ^ 1u;  // Native 2x: host 1 is the top sample.
  } else if (msaa == 1u && host_mode == 2u) {
    host_sample = guest_sample ? 3u : 0u;  // 2x stored as 4x.
  }
  return host_sample;
}

// Guest sample index of a host sample in a surface.
uint GuestSample(uint host_sample, uint msaa, uint host_mode) {
  uint guest_sample = host_sample;
  if (msaa == 1u && host_mode == 1u) {
    guest_sample = host_sample ^ 1u;
  } else if (msaa == 1u && host_mode == 2u) {
    guest_sample = host_sample ? 1u : 0u;
  }
  return guest_sample;
}

// The source surface's guest EDRAM word at `pixel`, guest sample `sample` of
// a reader with `reader_layout`; for a 64bpp reader, `half` selects the low
// (0) or high (1) word of the sample.
uint LoadSourceWord(uint reader_layout, uint2 pixel, uint sample, uint half,
                    uint source_layout, uint flags) {
  uint reader_msaa = LayoutMsaa(reader_layout);
  uint rx = reader_msaa >= 2u ? 1u : 0u;
  uint ry = reader_msaa >= 1u ? 1u : 0u;
  uint sample_x = (pixel.x << rx) + (reader_msaa >= 2u ? (sample & 1u) : 0u);
  uint sample_y = (pixel.y << ry) +
                  (reader_msaa >= 2u ? (sample >> 1u) : (reader_msaa == 1u ? sample : 0u));
  // 32-bit EDRAM column of the word.
  uint column = LayoutIs64bpp(reader_layout) ? sample_x * 2u + half : sample_x;
  uint tile_column = column / 80u;
  uint tile_x = column % 80u;
  uint tile_row = sample_y >> 4u;
  uint tile_y = sample_y & 15u;
  uint tile = (LayoutBase(reader_layout) +
               tile_row * (LayoutPitch(reader_layout) << LayoutIs64bpp(reader_layout)) +
               tile_column) &
              2047u;

  uint source_local = (tile - LayoutBase(source_layout)) & 2047u;
  uint source_pitch = LayoutPitch(source_layout) << LayoutIs64bpp(source_layout);
  uint source_column = source_local % source_pitch;
  uint source_row = source_local / source_pitch;
  if (LayoutIsDepth(source_layout) != LayoutIsDepth(reader_layout)) {
    // Depth tiles keep their 40-sample halves swapped relative to color.
    tile_x = (tile_x + 40u) % 80u;
  }
  uint source_msaa = LayoutMsaa(source_layout);
  uint sx = source_column * 80u + tile_x;
  uint source_half = 0u;
  if (LayoutIs64bpp(source_layout) != 0u) {
    source_half = sx & 1u;
    sx >>= 1u;
  }
  uint sy = source_row * 16u + tile_y;
  uint smx = source_msaa >= 2u ? 1u : 0u;
  uint smy = source_msaa >= 1u ? 1u : 0u;
  int2 source_pixel = int2(sx >> smx, sy >> smy);
  uint guest_sample = (sx & smx) | ((sy & smy) << smx);
  uint host_sample =
      HostSample(guest_sample, source_msaa, LayoutHostSampleMode(source_layout));

#ifdef FH1_SOURCE_DEPTH
#ifdef FH1_SOURCE_MSAA
  float depth = fh1_source_depth.Load(source_pixel, host_sample);
  uint stencil = fh1_source_stencil.Load(source_pixel, host_sample).g;
#else
  float depth = fh1_source_depth.Load(int3(source_pixel, 0));
  uint stencil = fh1_source_stencil.Load(int3(source_pixel, 0)).g;
#endif
  uint depth24;
  if (LayoutFormat(source_layout) == DEPTH_D24FS8) {
    // The host keeps float24 depth halved.
    depth24 = Float32To20e4(depth * 2.0f, (flags & FH1_FLAG_FLOAT24_ROUND) != 0u);
  } else {
    // 1.0 * 16777215 + 0.5 rounds to 2^24 in float32; without the clamp the
    // far plane would wrap to 0 when shifted into the EDRAM word.
    depth24 = min(uint(saturate(depth) * 16777215.0f + 0.5f), 0xFFFFFFu);
  }
  return (depth24 << 8u) | (stencil & 0xFFu);
#elif defined(FH1_SOURCE_UINT)
#ifdef FH1_SOURCE_MSAA
  uint4 bits = fh1_source_color.Load(source_pixel, host_sample);
#else
  uint4 bits = fh1_source_color.Load(int3(source_pixel, 0));
#endif
  uint source_format = LayoutFormat(source_layout);
  // 32-bit channels, one per word, or 16-bit channels, two per word.
  bool channels_32 = source_format == FORMAT_32_FLOAT || source_format == FORMAT_32_32_FLOAT;
  uint2 words = channels_32 ? bits.xy
                            : uint2((bits.x & 0xFFFFu) | (bits.y << 16u),
                                    (bits.z & 0xFFFFu) | (bits.w << 16u));
  return source_half != 0u ? words.y : words.x;
#else
#ifdef FH1_SOURCE_MSAA
  float4 color = fh1_source_color.Load(source_pixel, host_sample);
#else
  float4 color = fh1_source_color.Load(int3(source_pixel, 0));
#endif
  uint source_format = LayoutFormat(source_layout);
  if (source_format == FORMAT_8_8_8_8_GAMMA && (flags & FH1_FLAG_GAMMA_UNORM16) != 0u) {
    // Gamma targets stored as unorm16 hold linear values; EDRAM holds gamma.
    color.rgb = float3(LinearToPWLGamma(color.r), LinearToPWLGamma(color.g),
                       LinearToPWLGamma(color.b));
  }
  return EncodeColor(color, source_format);
#endif
}

#endif  // FH1_NATIVE_EDRAM_HLSLI_
