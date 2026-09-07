// FH1 4D5309C9 pixel shader A4A965C189287B99 only.

cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };

struct Fh1PassthroughColorPixel {
  float4 color : SV_Target;
  uint coverage : SV_Coverage;
};

bool AlphaTest(float alpha) {
  const uint compare = (xe_system[0].x >> 7) & 7u;
  const float reference = asfloat(xe_system[14].x);
  if (compare == 5u) return alpha != reference;
  return compare == 7u || ((compare & 1u) && alpha < reference) ||
         ((compare & 2u) && alpha == reference) ||
         ((compare & 4u) && alpha > reference);
}

uint AlphaCoverage(float alpha, float2 position) {
  const uint alpha_to_mask = xe_system[14].y;
  uint coverage = ~0u;
  if (alpha_to_mask) {
    const uint pattern =
        (uint(position.y) & 1u) | ((uint(position.x) & 1u) << 1);
    const float offset = float((alpha_to_mask >> (pattern << 1)) & 3u);
    coverage = 0;
    if (xe_system[13].w && xe_system[13].z) {
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .75) ? 1u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .25) ? 2u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .50) ? 4u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, 1.0) ? 8u : 0u;
    } else if (xe_system[13].w) {
      coverage |= alpha >= mad(offset, -1.0 / 8.0, .5) ? 2u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 8.0, 1.0) ? 1u : 0u;
    } else {
      coverage = alpha >= mad(offset, -1.0 / 4.0, 1.0) ? 1u : 0u;
    }
  }
  return coverage;
}

Fh1PassthroughColorPixel main(float4 color : TEXCOORD0,
                              float4 position : SV_Position) {
  clip(AlphaTest(color.w) ? 1.0 : -1.0);

  Fh1PassthroughColorPixel output;
  output.coverage = AlphaCoverage(color.w, position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color = color * asfloat(xe_system[15].y);
  return output;
}
