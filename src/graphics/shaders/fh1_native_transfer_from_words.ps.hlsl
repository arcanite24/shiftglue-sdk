// FH1 native executor ownership transfer into a depth destination, second
// step: the guest EDRAM words were computed per destination sample by
// fh1_native_transfer_words.cs.hlsl; this pass writes depth (FH1_DEST_KIND 1,
// the pipeline also resets stencil to 0) or one stencil bit (FH1_DEST_KIND 2:
// discards where the bit is clear, the pipeline replaces it elsewhere).
//
// Variants: FH1_DEST_MSAA (per-sample shading).

#include "fh1_push_constants.hlsli"

FH1_PUSH_CONSTANTS cbuffer Fh1NativeTransferConstants FH1_CONSTANTS_REGISTER {
  uint fh1_dest_layout;  // Layout of the new owner.
  uint fh1_dest_shape;   // Destination width in pixels | host samples << 16.
  uint fh1_flags;        // Stencil bit index 8:10.
};

#define FH1_NO_SOURCE 1
#include "fh1_native_edram.hlsli"

ByteAddressBuffer fh1_words : register(t0);

#ifdef FH1_DEST_MSAA
#define FH1_SAMPLE_INPUT , uint host_sample : SV_SampleIndex
#else
#define FH1_SAMPLE_INPUT
#endif

uint LoadWord(float4 position, uint host_sample) {
  uint2 pixel = uint2(position.xy);
  uint samples = fh1_dest_shape >> 16u;
  return fh1_words.Load(((pixel.y * (fh1_dest_shape & 0xFFFFu) + pixel.x) * samples +
                         host_sample) * 4u);
}

#if FH1_DEST_KIND == 1
float main(float4 position : SV_Position FH1_SAMPLE_INPUT) : SV_Depth {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  uint depth24 = LoadWord(position, host_sample) >> 8u;
  float depth;
  [branch] if (LayoutFormat(fh1_dest_layout) == DEPTH_D24FS8) {
    // The host keeps float24 depth halved.
    depth = Float20e4To32(depth24) * 0.5f;
  } else {
    depth = float(depth24) * (1.0f / 16777215.0f);
  }
  return depth;
}
#else
void main(float4 position : SV_Position FH1_SAMPLE_INPUT) {
#ifndef FH1_DEST_MSAA
  uint host_sample = 0u;
#endif
  if (!(LoadWord(position, host_sample) & (1u << ((fh1_flags >> 8u) & 7u)))) {
    discard;
  }
}
#endif
