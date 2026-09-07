// FH1 4D5309C9 pixel shader 57B9400F6B398736 only.

cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[3]; };

struct Fh1GradientFadePixel {
  float4 color : SV_Target;
  uint coverage : SV_Coverage;
};

bool AlphaTest(float alpha) {
  const uint compare = (xe_system[0].x >> 7) & 7u;
  const float reference = asfloat(xe_system[14].x);
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

Fh1GradientFadePixel main(float4 curve : TEXCOORD0,
                          float4 fade : TEXCOORD1,
                          float4 position : SV_Position) {
  const float value = (curve.x * curve.x - curve.y) * curve.z;
  const float2 gradient = float2(ddx_coarse(value), ddy_coarse(value));
  const float fade_amount = saturate(fade.x * c[1].w / fade.y);
  const float gradient_length = sqrt(abs(dot(gradient, gradient) + c[1].y));
  const float fade_scale = mad(-fade_amount, c[2].x, c[1].z);
  const float normalized = value / (c[1].x * gradient_length);
  const float edge = saturate(mad(-normalized, c[1].x, c[1].x));
  const float alpha = c[0].w * fade_amount * fade_amount * fade_scale * edge;
  clip(AlphaTest(alpha) ? 1.0 : -1.0);

  Fh1GradientFadePixel output;
  output.coverage = AlphaCoverage(alpha, position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color = float4(sqrt(abs(c[0].xyz)), alpha) *
                 asfloat(xe_system[15].y);
  return output;
}
