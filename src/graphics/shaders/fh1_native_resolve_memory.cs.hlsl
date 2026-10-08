// FH1 native executor resolve: copies a rectangle of a guest EDRAM surface
// into the native guest-memory mirror, in the guest texture layout (2D tiled,
// packed format, endian swap), the way the Xenos GPU does.
//
// The rectangle's EDRAM tiles belong to one native surface (the owner). Each
// resolve sample is located in EDRAM through the resolve's own layout (base,
// pitch, MSAA), found in the owner through the owner's layout, encoded as
// the guest EDRAM word of the owner's format, and decoded in the resolve's
// format - so layouts, MSAA modes and formats may differ between the two, as
// they may on the guest.
//
// Variants: FH1_SOURCE_MSAA (Texture2DMS sources), FH1_SOURCE_DEPTH (depth and
// stencil sources instead of a color source), FH1_DEST_IMAGE (32bpp only: each
// resolved word is also written into a texture of the destination's layout,
// through its raw-bits view, as the texture's load would leave it: swapped by
// the texture's endianness and, for depth textures, converted to float as
// their load does, from the texture row fh1_image_row).

#include "fh1_push_constants.hlsli"

FH1_PUSH_CONSTANTS cbuffer Fh1NativeResolveMemoryConstants FH1_CONSTANTS_REGISTER {
  uint fh1_rect_origin;     // x | y << 16, resolve surface pixels
  uint fh1_rect_size;       // width | height << 16
  // Layouts: base_tiles 0:10, pitch_tiles (32bpp) 11:18, msaa 19:20,
  // is_64bpp 21, is_depth 22, guest format 23:26 (color or depth format).
  uint fh1_resolve_layout;
  uint fh1_owner_layout;    // + host sample mode 27:28 (0: as guest, 1: native 2x,
                            // 2: 2x stored as 4x)
  uint fh1_sample_select;   // sanitized xenos::CopySampleSelect
  // pack 0:2 (0: 8_8_8_8, 1: 2_10_10_10, 2: 32_FLOAT, 3: 16_16_16_16_FLOAT,
  // 4: raw 32-bit word), endian 3:5, swap red/blue 6, float24 rounding 7,
  // exp bias 8:15 (signed), bytes per texel log2 16:17, gamma targets hold
  // linear values 18, 16_16[_16_16] hosts keep the full range as snorm / 32 19,
  // resolution scale - 1 20:21 (the rectangle is then in host pixels and the
  // destination is the texture cache's scaled resolve range), unscaled
  // destination 22 (at scale: the rectangle in guest pixels, each written to
  // the guest layout from its first host pixel).
  uint fh1_dest_info;
  uint fh1_dest_base;       // bytes (scaled: from the scaled range's base, unscaled)
  uint fh1_dest_pitch;      // texels
#ifdef FH1_DEST_IMAGE
  uint fh1_image_row;       // texture row of the destination's first row, host rows
  // The texture's xenos::Endian 0:2; its load's conversion 3:4 (0: none, 1:
  // 24-bit unorm depth to float, 2: 20e4 depth to float).
  uint fh1_image_endian;
  // The same for a second texture over the same memory and rows (bit 5: it
  // exists), such as an 8_8_8_8 texture reading a depth resolve's words.
  uint fh1_image2_endian;
#endif
};

#include "fh1_native_edram.hlsli"

RWByteAddressBuffer fh1_memory : register(u0);
#ifdef FH1_DEST_IMAGE
#ifdef FH1_SPIRV
[[vk::image_format("r32ui")]]
#endif
RWTexture2D<uint> fh1_image : register(u1);
#ifdef FH1_SPIRV
[[vk::image_format("r32ui")]]
#endif
RWTexture2D<uint> fh1_image2 : register(u2);
#endif


// texture_util::GetTiledOffset2D.
int TiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int macro_offset = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << (bpb_log2 + 7u);
  int micro_offset = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int offset = macro_offset + ((micro_offset & ~0xF) << 1) + (micro_offset & 0xF) +
               ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// The texture cache's scaled resolve layout: each 16 bytes of the guest
// texture become scale x scale groups of 16 bytes, each holding one host row
// of the guest group's texels at scale, stored column-major by group.
uint ScaledOffset(uint2 pixel, uint2 subpixel, uint pitch, uint bpb_log2, uint scale) {
  uint guest = uint(TiledOffset2D(int(pixel.x), int(pixel.y), pitch, bpb_log2));
  uint texels_per_group = 16u >> bpb_log2;
  uint host_x = ((guest & 15u) >> bpb_log2) * scale + subpixel.x;
  uint group_x = host_x / texels_per_group;
  return (guest & ~15u) * scale * scale + ((group_x * scale + subpixel.y) << 4u) +
         ((host_x % texels_per_group) << bpb_log2);
}

uint EndianSwap32(uint value, uint endian) {
  if (endian == 1u) {  // 8in16
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  } else if (endian == 2u) {  // 8in32
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
    value = (value << 16u) | (value >> 16u);
  } else if (endian == 3u) {  // 16in32
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

#ifdef FH1_DEST_IMAGE
// The word as a texture's load leaves it: swapped by its endianness and, for
// depth textures, converted to float.
uint ImageTexel(uint word, uint endian_and_conversion) {
  uint texel = EndianSwap32(word, endian_and_conversion & 7u);
  uint conversion = (endian_and_conversion >> 3u) & 3u;
  if (conversion == 1u) {
    // As texture_load_depth_unorm: (d + (d >> 23)) * 2^-24.
    uint depth = texel >> 8u;
    texel = asuint(float(depth + (depth >> 23u)) * asfloat(0x33800000u));
  } else if (conversion == 2u) {
    texel = asuint(Float20e4To32(texel >> 8u));
  }
  return texel;
}
#endif

// Stores a 32-bit destination word (already in the guest memory's byte order).
void StoreWord(uint address, uint2 host_pixel, uint word) {
  fh1_memory.Store(address, word);
#ifdef FH1_DEST_IMAGE
  // A resolve's area may be larger than a small texture it writes (an 8x8
  // resolve into a 4x4 texture's memory): texels outside it are not stored.
  uint2 image_size;
  fh1_image.GetDimensions(image_size.x, image_size.y);
  uint2 image_pixel = host_pixel + uint2(0u, fh1_image_row);
  if (all(image_pixel < image_size)) {
    fh1_image[image_pixel] = ImageTexel(word, fh1_image_endian);
  }
  if ((fh1_image2_endian >> 5u) & 1u) {
    fh1_image2.GetDimensions(image_size.x, image_size.y);
    if (all(image_pixel < image_size)) {
      fh1_image2[image_pixel] = ImageTexel(word, fh1_image2_endian);
    }
  }
#endif
}

uint LoadOwnerWord(uint2 pixel, uint sample, uint half) {
  uint flags = (((fh1_dest_info >> 7u) & 1u) ? FH1_FLAG_FLOAT24_ROUND : 0u) |
               (((fh1_dest_info >> 18u) & 1u) ? FH1_FLAG_GAMMA_UNORM16 : 0u);
  return LoadSourceWord(fh1_resolve_layout, pixel, sample, half, fh1_owner_layout, flags);
}

float4 LoadOwnerColor(uint2 pixel, uint sample, uint format) {
  uint low = LoadOwnerWord(pixel, sample, 0u);
  float4 color;
  [branch] if (LayoutIs64bpp(fh1_resolve_layout) != 0u) {
    color = DecodeColor64(uint2(low, LoadOwnerWord(pixel, sample, 1u)), format);
  } else {
    color = DecodeColor(low, format);
  }
  return color;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  uint2 rect_size = uint2(fh1_rect_size & 0xFFFFu, fh1_rect_size >> 16u);
  if (any(thread.xy >= rect_size)) {
    return;
  }
  uint2 pixel = uint2(fh1_rect_origin & 0xFFFFu, fh1_rect_origin >> 16u) + thread.xy;
  uint2 host_pixel = pixel;
  fh1_fixed16_scale = ((fh1_dest_info >> 19u) & 1u) != 0u ? 32.0f : 1.0f;
  uint scale = ((fh1_dest_info >> 20u) & 3u) + 1u;
  bool unscaled_dest = ((fh1_dest_info >> 22u) & 1u) != 0u;
  if (unscaled_dest) {
    fh1_scale = scale;
  } else {
    SetScaledPixel(pixel, scale);
  }

  uint pack = fh1_dest_info & 7u;
  uint endian = (fh1_dest_info >> 3u) & 7u;
  uint bpb_log2 = (fh1_dest_info >> 16u) & 3u;
  uint address;
  [branch] if (scale > 1u && !unscaled_dest) {
    address = fh1_dest_base * scale * scale +
              ScaledOffset(pixel, fh1_subpixel, fh1_dest_pitch, bpb_log2, scale);
  } else {
    address = fh1_dest_base +
              uint(TiledOffset2D(int(pixel.x), int(pixel.y), fh1_dest_pitch, bpb_log2));
  }

  uint first_sample, sample_count;
  // The upper half: untiled predicated tiling's source row offset.
  fh1_source_row_offset = fh1_sample_select >> 16u;
  uint sample_select = fh1_sample_select & 0xFFFFu;
  switch (sample_select) {
    case 4u: first_sample = 0u; sample_count = 2u; break;  // 01
    case 5u: first_sample = 2u; sample_count = 2u; break;  // 23
    case 6u: first_sample = 0u; sample_count = 4u; break;  // 0123
    default: first_sample = sample_select; sample_count = 1u; break;
  }

  if (pack == 4u) {
    // Depth: the EDRAM word itself.
    StoreWord(address, host_pixel, EndianSwap32(LoadOwnerWord(pixel, first_sample, 0u), endian));
    return;
  }

  float4 color = 0.0f;
  uint format = LayoutFormat(fh1_resolve_layout);
  for (uint i = 0u; i < sample_count; ++i) {
    color += LoadOwnerColor(pixel, first_sample + i, format);
  }
  color *= 1.0f / float(sample_count);
  int exp_bias = int(fh1_dest_info << 16u) >> 24;
  color *= asfloat(uint(127 + exp_bias) << 23u);
  if ((fh1_dest_info >> 6u) & 1u) {
    color = color.bgra;
  }

  if (pack == 0u) {
    StoreWord(address, host_pixel, EndianSwap32(EncodeColor(color, FORMAT_8_8_8_8), endian));
  } else if (pack == 1u) {
    StoreWord(address, host_pixel, EndianSwap32(EncodeColor(color, FORMAT_2_10_10_10), endian));
  } else if (pack == 2u) {
    StoreWord(address, host_pixel, EndianSwap32(asuint(color.r), endian));
  } else {
    uint2 words = uint2(f32tof16(color.r) | (f32tof16(color.g) << 16u),
                        f32tof16(color.b) | (f32tof16(color.a) << 16u));
    fh1_memory.Store2(address, uint2(EndianSwap32(words.x, endian), EndianSwap32(words.y, endian)));
  }
}
