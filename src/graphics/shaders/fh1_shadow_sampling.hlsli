// Shared exact shadow sampling and ALU helpers.
#ifndef FH1_SHADOW_PIXEL_SCALE
#define FH1_SHADOW_PIXEL_SCALE 0.5
#endif

SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[FH1_SHADOW_CONSTANT_COUNT]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices[2];
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

float DecodeSample(float value, uint sign, uint fetch_word) {
  if (sign == 2u) value = value * 2.0 - 1.0;
  else if (sign == 3u) value = DecodeGammaSigned(value);
  const int exponent = SignExtend((fetch_word >> 13) & 0x3Fu, 6);
  return value * asfloat(uint(exponent + 127) << 23);
}

float SampleGradientX(float2 coordinate, float offset_y = 0.5) {
  const uint width = (xe_fetch[0].z & 0x1FFFu) + 1u;
  const uint height = ((xe_fetch[0].z >> 13) & 0x1FFFu) + 1u;
  const float2 offset = (float2(513.5, offset_y * 1024.0 + 1.5) / 1024.0) / float2(width, height);
  coordinate = (xe_system[13].x & 1u)
                   ? mad(offset, 0.5, coordinate)
                   : coordinate + offset;
  const int gradient_exponent =
      SignExtend((xe_fetch[1].x >> 12) & 0x3FFu, 10);
  const float gradient_scale = exp2(float(gradient_exponent) / 32.0);
  const float2 dx = ddx_coarse(coordinate) * gradient_scale;
  const float2 dy = ddy_coarse(coordinate) * gradient_scale;
  const uint sign = xe_system[11].x & 3u;
  const uint texture_index = sign == 1u ? xe_descriptor_indices[0].w
                                        : xe_descriptor_indices[0].z;
  const float value = xe_textures_2d[texture_index].SampleGrad(
      xe_samplers[xe_descriptor_indices[0].y], float3(coordinate, 0.0), dx, dy).x;
  return DecodeSample(value, sign, xe_fetch[0].w);
}

float SampleShadowX(float2 coordinate) {
  const uint width = (xe_fetch[2].x & 0x1FFFu) + 1u;
  const uint height = ((xe_fetch[2].x >> 13) & 0x1FFFu) + 1u;
  const float2 offset = (1.5 / 1024.0) / float2(width, height);
  coordinate = (xe_system[13].x & 2u)
                   ? mad(offset, 0.5, coordinate)
                   : coordinate + offset;
  const int lod_exponent = SignExtend((xe_fetch[2].z >> 12) & 0x3FFu, 10);
  const uint sign = (xe_system[11].x >> 8) & 3u;
  const uint texture_index = sign == 1u ? xe_descriptor_indices[1].z
                                        : xe_descriptor_indices[1].y;
  const float value = xe_textures_2d[texture_index].SampleLevel(
      xe_samplers[xe_descriptor_indices[1].x], float3(coordinate, 0.0),
      float(lod_exponent) / 32.0).x;
  return DecodeSample(value, sign, xe_fetch[2].y);
}

float4 Mul(float4 a, float4 b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float MulS(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float Dot3(float4 a, float4 b) { precise float r = MulS(a.x,b.x); r += MulS(a.y,b.y); r += MulS(a.z,b.z); return r; }
float Dot4(float4 a, float4 b) { precise float r = Dot3(a,b); r += MulS(a.w,b.w); return r; }

