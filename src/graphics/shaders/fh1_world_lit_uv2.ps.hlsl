// FH1 4D5309C9 material shader 6FDA0F1CDE67D12F only.
// Direct form of its 29 guest instructions and two observed texture fetches.

SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[9]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices[2];
};

struct Fh1Pixel { float4 color : SV_Target; uint coverage : SV_Coverage; };

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

float4 ApplySigns(float4 value, uint4 signs) {
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    if (signs[i] == 2) value[i] = value[i] * 2.0 - 1.0;
    else if (signs[i] == 3) value[i] = DecodeGammaSigned(value[i]);
  }
  return value;
}

float4 Fetch(float2 coordinate, uint packed_size, uint packed_gradient,
             uint packed_exponent, uint resolution_bit, uint4 signs,
             uint sampler_index, uint texture_index, uint signed_texture_index) {
  const uint width = (packed_size & 0x1FFFu) + 1;
  const uint height = ((packed_size >> 13) & 0x1FFFu) + 1;
  const float2 offset = (1.5 / 1024.0) / float2(width, height);
  const float2 uv = (xe_system[13].x & resolution_bit)
                        ? mad(offset, 0.5, coordinate)
                        : coordinate + offset;
  const int gradient_exponent = SignExtend((packed_gradient >> 12) & 0x3FFu, 10);
  const float gradient_scale = exp2(float(gradient_exponent) / 32.0);
  const float2 dx = ddx_coarse(uv) * gradient_scale;
  const float2 dy = ddy_coarse(uv) * gradient_scale;
  float4 value = 0.0;
  if (!all(signs == 1u)) {
    value = xe_textures_2d[texture_index].SampleGrad(
        xe_samplers[sampler_index], float3(uv, 0.0), dx, dy);
  }
  if (any(signs == 1u)) {
    const float4 signed_value = xe_textures_2d[signed_texture_index].SampleGrad(
        xe_samplers[sampler_index], float3(uv, 0.0), dx, dy);
    value = signs == 1u ? signed_value : value;
  }
  const int exponent = SignExtend((packed_exponent >> 13) & 0x3Fu, 6);
  return ApplySigns(value, signs) * asfloat(uint(exponent + 127) << 23);
}

bool AlphaTest(float alpha) {
  const uint compare = (xe_system[0].x >> 7) & 7;
  const float reference = asfloat(xe_system[14].x);
  return compare == 7 || ((compare & 1u) && alpha < reference) ||
         ((compare & 2u) && alpha == reference) ||
         ((compare & 4u) && alpha > reference);
}

uint AlphaCoverage(float alpha, float2 position) {
  const uint alpha_to_mask = xe_system[14].y;
  uint coverage = ~0u;
  if (alpha_to_mask) {
    uint pattern = (uint(position.y) & 1u) | ((uint(position.x) & 1u) << 1);
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

Fh1Pixel main(float4 uv : TEXCOORD0, float4 vertex : TEXCOORD1,
              float4 selector : TEXCOORD2, float4 position : SV_Position) {
  const uint4 signs0 = (xe_system[11].x >> uint4(0, 2, 4, 6)) & 3u;
  const uint4 signs1 = (xe_system[11].x >> uint4(8, 10, 12, 14)) & 3u;
  const float4 t0 = Fetch(uv.xy, xe_fetch[0].z, xe_fetch[1].x, xe_fetch[0].w,
                         1u, signs0, xe_descriptor_indices[0].y,
                         xe_descriptor_indices[0].z, xe_descriptor_indices[0].w);
  const float4 t1 = Fetch(uv.zw, xe_fetch[2].x, xe_fetch[2].z, xe_fetch[2].y,
                         2u, signs1, xe_descriptor_indices[1].x,
                         xe_descriptor_indices[1].y, xe_descriptor_indices[1].z);

  const float luminance0 = dot(t0.zxy, c[7].xyz);
  const float luminance1 = dot(t1.zxy, c[7].xyz);
  const float3 color0 = luminance0 + (t0.xyz - luminance0) * c[5].x;
  const float3 color1 = luminance1 + (t1.xyz - luminance1) * c[6].x;
  const float3 base = (color0 + c[7].w) * c[1].x + c[8].x;
  const float3 detail = (color1 + c[7].w) * c[2].x + c[8].x;
  const float3 lit = base * vertex.xyz + color0 * c[1].z + c[1].w;
  const float3 linear_color =
      ((lit * c[1].y) * detail + color1 * c[2].z + c[2].w) * c[2].y +
      selector.xyz;

  const float texture_alpha0 = t0.w * c[0].y;
  const float alpha0 = (texture_alpha0 * c[3].z - vertex.w) * c[3].x + vertex.w;
  const float alpha = (((c[4].y * selector.w) * alpha0 +
                        (t1.w * c[0].y * c[4].z - alpha0)) * c[4].x + alpha0);
  clip(AlphaTest(alpha) ? 1.0 : -1.0);

  Fh1Pixel output;
  output.coverage = AlphaCoverage(alpha, position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color = float4(sqrt(abs(linear_color)), alpha) *
                 asfloat(xe_system[15].y);
  return output;
}
