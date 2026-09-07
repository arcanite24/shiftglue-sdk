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

float4 SampleTexture(float2 coordinate, uint fetch, uint slot, float offset_texels, uint live_mask) {
  const uint width = (xe_fetch[(fetch * 6 + 2) / 4][(fetch * 6 + 2) % 4] & 0x1FFFu) + 1u;
  const uint height = ((xe_fetch[(fetch * 6 + 2) / 4][(fetch * 6 + 2) % 4] >> 13) & 0x1FFFu) + 1u;
  const float2 offset = (offset_texels + 1.5 / 1024.0) / float2(width, height);
  coordinate = (xe_system[13].x & (1u << fetch))
                   ? mad(offset, 0.5, coordinate)
                   : coordinate + offset;
  const int gradient_exponent =
      SignExtend((xe_fetch[(fetch * 6 + 4) / 4][(fetch * 6 + 4) % 4] >> 12) & 0x3FFu, 10);
  const float gradient_scale = exp2(float(gradient_exponent) / 32.0);
  const float2 dx = ddx_coarse(coordinate) * gradient_scale;
  const float2 dy = ddy_coarse(coordinate) * gradient_scale;
  const uint4 signs = (xe_system[11 + fetch / 16][(fetch / 4) % 4] >> (uint4(0, 2, 4, 6) + (fetch % 4) * 8)) & 3u;
  float4 texel = 0.0;
  if (any((signs != 1u) && ((live_mask & uint4(1, 2, 4, 8)) != 0u))) {
    texel = xe_textures_2d[descriptors[(slot + 1) / 4][(slot + 1) % 4]].SampleGrad(
        xe_samplers[descriptors[slot / 4][slot % 4]], float3(coordinate, 0.0), dx, dy);
  }
  if (any((signs == 1u) && ((live_mask & uint4(1, 2, 4, 8)) != 0u))) {
    const float4 signed_texel =
        xe_textures_2d[descriptors[(slot + 2) / 4][(slot + 2) % 4]].SampleGrad(
            xe_samplers[descriptors[slot / 4][slot % 4]], float3(coordinate, 0.0),
            dx, dy);
    texel = signs == 1u ? signed_texel : texel;
  }
  texel = ApplyTextureSigns(texel, signs);
  const int exponent = SignExtend((xe_fetch[(fetch * 6 + 3) / 4][(fetch * 6 + 3) % 4] >> 13) & 0x3Fu, 6);
  return texel * asfloat(uint(exponent + 127) << 23);
}

float4 Mul(float4 a, float4 b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float MulS(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float Dot3(float4 a, float4 b) { precise float r = MulS(a.x,b.x); r += MulS(a.y,b.y); r += MulS(a.z,b.z); return r; }
float Dot4(float4 a, float4 b) { precise float r = Dot3(a,b); r += MulS(a.w,b.w); return r; }
