// FH1 native executor EDRAM ownership transfer: when a surface takes over
// EDRAM tiles another surface wrote, the guest sees the other surface's words
// through its own layout and format. This pass writes them into the new owner
// (the render target) from the previous owner (the source textures), per
// sample.
//
// Variants: FH1_SOURCE_DEPTH / FH1_SOURCE_UINT / FH1_SOURCE_MSAA (see
// fh1_native_edram.hlsli), FH1_DEST_MSAA (per-sample shading), FH1_DEST_KIND
// 0 (color), 1 (depth; the pipeline also resets stencil to 0), 2 (one stencil
// bit: discards where the bit is clear, the pipeline replaces that bit
// elsewhere), 3 (color written as raw channel bits through a UINT view, for
// formats with 16-bit or 32-bit float channels).

#include "fh1_native_edram.hlsli"

cbuffer Fh1NativeTransferConstants : register(b0) {
  uint fh1_dest_layout;    // Layout of the new owner, with its host sample mode.
  uint fh1_source_layout;  // Layout of the previous owner, with its host sample mode.
  uint fh1_flags;          // FH1_FLAG_*, stencil bit index 8:10, scale - 1 12:13.
};

#ifdef FH1_DEST_MSAA
#define FH1_SAMPLE_INPUT , uint host_sample : SV_SampleIndex
#else
#define FH1_SAMPLE_INPUT
#endif

uint LoadWord(float4 position, uint host_sample, uint half) {
  uint guest_sample = GuestSample(host_sample, LayoutMsaa(fh1_dest_layout),
                                  LayoutHostSampleMode(fh1_dest_layout));
  uint2 pixel = uint2(position.xy);
  SetScaledPixel(pixel, ((fh1_flags >> FH1_FLAG_SCALE_SHIFT) & 3u) + 1u);
  return LoadSourceWord(fh1_dest_layout, pixel, guest_sample, half, fh1_source_layout,
                        fh1_flags);
}

#if FH1_DEST_KIND == 0
float4 main(float4 position : SV_Position FH1_SAMPLE_INPUT) : SV_Target {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  uint format = LayoutFormat(fh1_dest_layout);
  fh1_fixed16_scale = (fh1_flags & FH1_FLAG_FIXED16_FULL_RANGE) != 0u ? 32.0f : 1.0f;
  float4 color = DecodeColor(LoadWord(position, host_sample, 0u), format);
  if (format == FORMAT_8_8_8_8_GAMMA && (fh1_flags & FH1_FLAG_GAMMA_UNORM16) != 0u) {
    color.rgb = float3(PWLGammaToLinear(color.r), PWLGammaToLinear(color.g),
                       PWLGammaToLinear(color.b));
  }
  return color;
}
#elif FH1_DEST_KIND == 1
float main(float4 position : SV_Position FH1_SAMPLE_INPUT) : SV_Depth {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  uint depth24 = LoadWord(position, host_sample, 0u) >> 8u;
  if (LayoutFormat(fh1_dest_layout) == DEPTH_D24FS8) {
    // The host keeps float24 depth halved.
    return Float20e4To32(depth24) * 0.5f;
  }
  return float(depth24) * (1.0f / 16777215.0f);
}
#elif FH1_DEST_KIND == 2
void main(float4 position : SV_Position FH1_SAMPLE_INPUT) {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  if (!(LoadWord(position, host_sample, 0u) & (1u << ((fh1_flags >> 8u) & 7u)))) {
    discard;
  }
}
#else
uint4 main(float4 position : SV_Position FH1_SAMPLE_INPUT) : SV_Target {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  uint low = LoadWord(position, host_sample, 0u);
  uint format = LayoutFormat(fh1_dest_layout);
  if (format == FORMAT_32_FLOAT) return uint4(low, 0u, 0u, 0u);
  if (LayoutIs64bpp(fh1_dest_layout) == 0u) {
    return uint4(low & 0xFFFFu, low >> 16u, 0u, 0u);
  }
  uint high = LoadWord(position, host_sample, 1u);
  if (format == FORMAT_32_32_FLOAT) return uint4(low, high, 0u, 0u);
  return uint4(low & 0xFFFFu, low >> 16u, high & 0xFFFFu, high >> 16u);
}
#endif
