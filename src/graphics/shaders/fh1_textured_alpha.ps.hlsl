// FH1 4D5309C9 pixel shader 98B579267495657C only.

SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices;
};

struct Fh1TexturedAlphaPixel {
  float4 color : SV_Target;
  uint coverage : SV_Coverage;
};

int SignExtend(uint value, uint bits) {
  const uint shift = 32 - bits;
  return int(value << shift) >> shift;
}

float DecodeGammaSigned(float value) {
  value = saturate(value) * 261120.0;
  float scale, bias;
  if (value >= 98304.0) {
    const bool high = value >= 196608.0;
    scale = high ? 1.0 / 128.0 : 1.0 / 256.0;
    bias = high ? -1024.0 : -256.0;
  } else {
    const bool high = value >= 65536.0;
    scale = high ? 1.0 / 512.0 : 1.0 / 1024.0;
    bias = high ? -64.0 : 0.0;
  }
  value = value * scale + bias;
  return (value + trunc(value * scale)) / 1023.0;
}

float4 ApplyTextureSigns(float4 value, uint4 signs) {
  [unroll]
  for (uint component = 0; component < 4; ++component) {
    if (signs[component] == 2u)
      value[component] = value[component] * 2.0 - 1.0;
    else if (signs[component] == 3u)
      value[component] = DecodeGammaSigned(value[component]);
  }
  return value;
}

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

float4 SampleTexture(float2 coordinate) {
  const uint width = (xe_fetch[0].z & 0x1FFFu) + 1u;
  const uint height = ((xe_fetch[0].z >> 13) & 0x1FFFu) + 1u;
  const float2 offset = (1.5 / 1024.0) / float2(width, height);
  coordinate = (xe_system[13].x & 1u)
                   ? mad(offset, 0.5, coordinate)
                   : coordinate + offset;
  const int gradient_exponent =
      SignExtend((xe_fetch[1].x >> 12) & 0x3FFu, 10);
  const float gradient_scale = exp2(float(gradient_exponent) / 32.0);
  const float2 dx = ddx_coarse(coordinate) * gradient_scale;
  const float2 dy = ddy_coarse(coordinate) * gradient_scale;
  const uint4 signs = (xe_system[11].x >> uint4(0, 2, 4, 6)) & 3u;
  float4 texel = 0.0;
  if (!all(signs == 1u)) {
    texel = xe_textures_2d[xe_descriptor_indices.z].SampleGrad(
        xe_samplers[xe_descriptor_indices.y], float3(coordinate, 0.0), dx, dy);
  }
  if (any(signs == 1u)) {
    const float4 signed_texel =
        xe_textures_2d[xe_descriptor_indices.w].SampleGrad(
            xe_samplers[xe_descriptor_indices.y], float3(coordinate, 0.0),
            dx, dy);
    texel = signs == 1u ? signed_texel : texel;
  }
  texel = ApplyTextureSigns(texel, signs);
  const int exponent = SignExtend((xe_fetch[0].w >> 13) & 0x3Fu, 6);
  return texel * asfloat(uint(exponent + 127) << 23);
}

Fh1TexturedAlphaPixel main(float4 uv_alpha : TEXCOORD0,
                           float4 position : SV_Position) {
  const float4 texel = SampleTexture(uv_alpha.xy);
  const float alpha = texel.w * uv_alpha.w;
  clip(alpha - c.x - c.y);
  clip(AlphaTest(alpha) ? 1.0 : -1.0);

  Fh1TexturedAlphaPixel output;
  output.coverage = AlphaCoverage(alpha, position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color = float4(texel.xyz, alpha) * asfloat(xe_system[15].y);
  return output;
}
