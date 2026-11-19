// Direct3D 11 presentation: applies the front buffer's host swizzle before
// the gamma shaders.
//
// Direct3D 12 applies the swizzle with the SRV component mapping, which
// Direct3D 11 lacks, and the gamma shaders read a plain view. FH1's front
// buffer is fetched with red and blue swapped, so this pass writes the
// swizzled texels into a texture of the same format. Unorm loads and stores
// of the same format round-trip exactly.
//
// t0: the front buffer (unorm view of mip 0).
// u0: the swizzled texture, same format.
// b0: XeSwapSwizzleConstants.

cbuffer XeSwapSwizzleConstants : register(b0) {
  uint2 xe_swap_swizzle_size;
  // 3 bits per component, R first: 0-3 select X-W, 4 is 0, 5 is 1.
  uint xe_swap_swizzle;
  uint xe_swap_swizzle_padding;
};

Texture2D<float4> xe_swap_swizzle_source : register(t0);
RWTexture2D<unorm float4> xe_swap_swizzle_dest : register(u0);

float XeSwapSwizzleComponent(float4 texel, uint selector) {
  float value = selector == 5u ? 1.0 : 0.0;
  value = selector == 0u ? texel.x : value;
  value = selector == 1u ? texel.y : value;
  value = selector == 2u ? texel.z : value;
  value = selector == 3u ? texel.w : value;
  return value;
}

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  if (any(xe_thread_id.xy >= xe_swap_swizzle_size)) {
    return;
  }
  float4 texel = xe_swap_swizzle_source.Load(int3(xe_thread_id.xy, 0));
  xe_swap_swizzle_dest[xe_thread_id.xy] = float4(
      XeSwapSwizzleComponent(texel, xe_swap_swizzle & 7u),
      XeSwapSwizzleComponent(texel, (xe_swap_swizzle >> 3u) & 7u),
      XeSwapSwizzleComponent(texel, (xe_swap_swizzle >> 6u) & 7u),
      XeSwapSwizzleComponent(texel, (xe_swap_swizzle >> 9u) & 7u));
}
