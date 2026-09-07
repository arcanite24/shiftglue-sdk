// FH1 4D5309C9 pixel shader 21937679208E59A5 only.

SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices;
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

[earlydepthstencil]
float4 main(float4 uv : TEXCOORD0) : SV_Target {
  const uint width = (xe_fetch[0].z & 0x1FFFu) + 1u;
  const uint height = ((xe_fetch[0].z >> 13) & 0x1FFFu) + 1u;
  const float2 offset = (1.5 / 1024.0) / float2(width, height);
  const float2 coordinate = (xe_system[13].x & 1u)
                                ? mad(offset, 0.5, uv.xy)
                                : uv.xy + offset;
  const int lod_exponent = SignExtend((xe_fetch[1].x >> 12) & 0x3FFu, 10);
  const float lod = float(lod_exponent) / 32.0 + c.x;
  const uint4 signs = (xe_system[11].x >> uint4(0, 2, 4, 6)) & 3u;
  float4 texel = 0.0;
  if (!all(signs == 1u)) {
    texel = xe_textures_2d[xe_descriptor_indices.z].SampleLevel(
        xe_samplers[xe_descriptor_indices.y], float3(coordinate, 0.0), lod);
  }
  if (any(signs == 1u)) {
    const float4 signed_texel =
        xe_textures_2d[xe_descriptor_indices.w].SampleLevel(
            xe_samplers[xe_descriptor_indices.y], float3(coordinate, 0.0), lod);
    texel = signs == 1u ? signed_texel : texel;
  }
  texel = ApplyTextureSigns(texel, signs);
  const int exponent = SignExtend((xe_fetch[0].w >> 13) & 0x3Fu, 6);
  texel *= asfloat(uint(exponent + 127) << 23);
  return texel * asfloat(xe_system[15].y);
}
