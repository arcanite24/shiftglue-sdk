// FH1 4D5309C9 pixel shader 2F2137BF953DA7AF only.

#ifdef FH1_SCENE_FIXED_TEXTURES
SamplerState material_sampler : register(s0);
Texture2DArray<float4> material_unsigned : register(t0, space1);
Texture2DArray<float4> material_signed : register(t1, space1);
#else
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
#define material_sampler xe_samplers[xe_descriptor_indices.y]
#define material_unsigned xe_textures_2d[xe_descriptor_indices.z]
#define material_signed xe_textures_2d[xe_descriptor_indices.w]
#endif
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[12]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
#ifndef FH1_SCENE_FIXED_TEXTURES
cbuffer xe_descriptor_indices_cbuffer : register(b4) {
  uint4 xe_descriptor_indices;
};
#endif

struct Fh1BlendedLitPixel {
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
    texel = material_unsigned.SampleGrad(
        material_sampler, float3(coordinate, 0.0), dx, dy);
  }
  if (any(signs == 1u)) {
    const float4 signed_texel =
        material_signed.SampleGrad(
            material_sampler, float3(coordinate, 0.0),
            dx, dy);
    texel = signs == 1u ? signed_texel : texel;
  }
  texel = ApplyTextureSigns(texel, signs);
  const int exponent = SignExtend((xe_fetch[0].w >> 13) & 0x3Fu, 6);
  return texel * asfloat(uint(exponent + 127) << 23);
}

float4 Mul(float4 a, float4 b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float MulS(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float Dot3(float4 a, float4 b) { precise float r = MulS(a.x,b.x); r += MulS(a.y,b.y); r += MulS(a.z,b.z); return r; }
float Dot4(float4 a, float4 b) { precise float r = Dot3(a,b); r += MulS(a.w,b.w); return r; }
Fh1BlendedLitPixel main(float4 p0:TEXCOORD0,float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,float4 p3:TEXCOORD3,float4 p4:TEXCOORD4,float4 position:SV_Position) {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=0,r6=0,r7=0,r8=0,r9=0,r10=0,oC0=0;
  precise float ps=0;
  { // 4
    precise float4 v = Mul(r3.xyyy, c[0].xyyy);
    r5.xy = v.xy;
  }
  r5.xyz = SampleTexture(r5.xy).xyz;
  { // 6
    precise float4 v = Mul(c[3].xxxx, c[10].wwww);
    r8.w = v.w;
  }
  { // 7
    precise float4 v = c[7].xzyy + -c[6].xzyy;
    r6.xyz = v.xyz;
  }
  { // 8
    precise float4 v = Mul(c[10].xyzz, c[2].xxxx);
    r8.xyz = v.xyz;
  }
  { // 9
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    precise float s = max((c[5].xx).x, (c[5].xx).y);
    r0.w = v.w;
    ps = s;
  }
  { // 10
    precise float4 v = saturate(Dot3(c[4].zxyy, c[9].zxyy));
    precise float s = MulS(ps, c[1].x);
    r6.w = v.w;
    r7.y = s;
    ps = s;
  }
  { // 11
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    precise float s = log2((abs(r3)).w);
    r5.w = v.w;
    r3.x = s;
    ps = s;
  }
  { // 12
    precise float4 v = Mul(r3.xxxx, c[3].yyyy);
    precise float s = rsqrt((abs(r5)).w);
    r9.w = v.w;
    r7.x = s;
    ps = s;
  }
  { // 13
    precise float4 v = Mul(r5.xyzx, c[0].zzzw);
    precise float s = log2(r6.w);
    r5.xyzw = v.xyzw;
    r3.x = s;
    ps = s;
  }
  { // 14
    precise float4 v = Mul(r7.xxxx, r0.xyzz);
    precise float s = MulS(c[4].w, r3.x);
    r9.xyz = v.xyz;
    r6.w = s;
    ps = s;
  }
  { // 15
    precise float4 v = Mul(r7.xxxx, c[9].xyzz);
    precise float s = rsqrt((abs(r0)).w);
    r10.xyz = v.xyz;
    r0.w = s;
    ps = s;
  }
  { // 16
    precise float4 v = Mul(r0.wwww, r1.xyyz) + c[9].xyyz;
    r7.xzw = v.xzw;
  }
  { // 17
    precise float4 v = Dot3(r7.wxzz, r7.wxzz);
    precise float s = exp2(r9.w);
    r0.w = v.w;
    r1.y = s;
    ps = s;
  }
  { // 18
    precise float4 v = Dot3(r10.zxyy, r0.zxyy);
    precise float s = exp2(r6.w);
    r0.x = v.x;
    r0.z = s;
    ps = s;
  }
  { // 19
    precise float4 v = Mul(r0.xxxx, c[11].yyyy) + c[11].yyyy;
    r0.x = v.x;
  }
  { // 20
    precise float4 v = Mul(r0.zzzz, r1.yyyy);
    precise float s = rsqrt((abs(r0)).w);
    r1.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 21
    precise float4 v = Mul(r7.xzzw, r0.yyyy);
    precise float s = max((c[5].zz).x, (c[5].zz).y);
    r7.xzw = v.xzw;
    ps = s;
  }
  { // 22
    precise float4 v = saturate(Dot3(r9.zxyy, r7.wxzz));
    precise float s = MulS(ps, c[1].x);
    r0.y = v.y;
    r7.z = s;
    ps = s;
  }
  { // 23
    precise float4 v = Mul(r8.xyzz, r1.xxyy);
    precise float s = max((c[5].yy).x, (c[5].yy).y);
    r1.xyw = v.xyw;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r1.wwww, r0.zzzz);
    precise float s = MulS(ps, c[1].x);
    r1.z = v.z;
    r7.w = s;
    ps = s;
  }
  { // 25
    precise float4 v = r7.ywzz + r1.xyzz;
    r1.xyz = v.xyz;
  }
  { // 26
    precise float4 v = r1.xyzz + -r7.ywzz;
    precise float s = log2(r0.y);
    r8.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 27
    precise float4 v = Mul(r8, r0.xxxy);
    precise float s = max((r5.ww).x, (r5.ww).y);
    r0.xyzw = v.xyzw;
    r7.x = s;
    ps = s;
  }
  { // 28
    precise float4 v = r7.ywzz + r0.xyzz;
    precise float s = exp2(r0.w);
    r1.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 29
    precise float4 v = Mul(r0.xxxx, c[10].xxyz);
    precise float s = max((c[11].xx).x, (c[11].xx).y);
    r0.yzw = v.yzw;
    r7.y = s;
    ps = s;
  }
  { // 30
    precise float4 v = Mul(r1.xyzz, r4.wwww);
    precise float s = max((r7.xy).x, (r7.xy).y);
    r1.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 31
    precise float4 v = Mul(r1.xyzz, r6.xzyy) + c[6].xyzz;
    r1.xyz = v.xyz;
  }
  { // 32
    precise float4 v = Mul(r0.yzww, r0.xxxx);
    r0.xyz = v.xyz;
  }
  { // 33
    precise float4 v = Mul(r0.xzyy, c[8].wwww);
    r0.xyz = v.xyz;
  }
  { // 34
    precise float4 v = Mul(r5.xyzz, r1.xyzz) + r0.xzyy;
    r0.xyz = v.xyz;
  }
  { // 35
    precise float4 v = Mul(r0.xyzz, r2.wwww) + r2.xyzz;
    r0.xyz = v.xyz;
  }
  { // 36
    precise float4 v = sqrt((abs(r0)).x);
    oC0.x = v.x;
  }
  { // 37
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 38
    precise float4 v = max(r3.zzzz, r3.zzzz);
    precise float s = sqrt((abs(r0)).z);
    oC0.w = v.w;
    oC0.z = s;
    ps = s;
  }
  clip(AlphaTest(oC0.w) ? 1.0 : -1.0);
  Fh1BlendedLitPixel output;
  output.coverage=AlphaCoverage(oC0.w,position.xy);
  clip(output.coverage ? 1.0 : -1.0);
  output.color=oC0 * asfloat(xe_system[15].y);
  return output;
}
