// FH1 packed-world pixel program E163D0BE1C2F9775; exact captured opaque variant.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[12]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[4]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 7
    precise float4 v = Mul(r2.zwww, c[1].zwww);
    r7.xy = v.xy;
  }
  r4.z = SampleTexture(r7.xy, 5, 1, 0.0, 2).y;
  { // 9
    precise float4 v = Mul(r5.yyyy, c[11].xxxx);
    precise float s = rcp(r5.w);
    r0.w = v.w;
    r4.x = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r4.xxxz, c[11].xxxy);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r2.zw = v.zw;
    ps = s;
  }
  { // 11
    precise float4 v = Mul(r2.zzzz, r5.xxxx);
    precise float s = MulS(ps, r4.x);
    r4.y = v.y;
    r4.x = s;
    ps = s;
  }
  { // 12
    precise float4 v = r4.yyxx + c[11].xxxx;
    r4.yz = v.yz;
  }
  { // 13
    precise float4 v = Mul(r2.xyxy, c[0]);
    precise float s = c[11].w - (r4.z);
    r5.xyzw = v.xyzw;
    r4.x = s;
    ps = s;
  }
  r8.xyzw = SampleTexture(r4.yx, 13, 4, 0.5, 15).wzyx;
  r4.xyz = SampleTexture(r5.zw, 1, 7, 0.0, 7).xyz;
  r5.xyz = SampleTexture(r5.xy, 0, 10, 0.0, 7).xyz;
#if defined(FH1_BLEND_MASK_Y)
  r1.w = SampleTexture(r7.xy, 2, 13, 0.0, 2).y;
#else
  r1.w = SampleTexture(r7.xy, 2, 13, 0.0, 1).x;
#endif
  { // 18
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    r0.w = v.w;
  }
  { // 19
    precise float4 v = r4.xxyz + -r5.xxyz;
    r9.yzw = v.yzw;
  }
  { // 20
    precise float4 v = Dot3(r5.zxyy, c[10].wwww);
    precise float s = rsqrt((abs(r0)).w);
    r10.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 21
    precise float4 v = Mul(r0.wwww, c[11].xxxx);
    precise float s = saturate(max((r1.ww).x, (r1.ww).y));
    r6.w = v.w;
    r2.x = s;
    ps = s;
  }
  { // 22
    precise float4 v = Dot3(r4.zxyy, c[10].wwww);
    precise float s = MulS(c[8].x, r0.w);
    r10.y = v.y;
    r4.x = s;
    ps = s;
  }
  { // 23
    precise float4 v = Mul(r0.wwww, r0.xyzz);
    precise float s = MulS(c[8].y, r0.w);
    r7.xyz = v.xyz;
    r4.y = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r10.xyyy, c[2].xyyy);
    precise float s = MulS(c[8].z, r0.w);
    r10.xy = v.xy;
    r4.z = s;
    ps = s;
  }
  { // 25
    precise float4 v = r10.yyyy + -r10.xxxx;
    precise float s = c[10].y + r2.w;
    r9.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 26
    precise float4 v = Mul(r9.xxxx, r2.xxxx) + r10.xxxx;
    r0.w = v.w;
  }
  { // 27
    precise float4 v = Mul(r9.zwyy, r2.xxxx) + r5.yzxx;
    r5.xyz = v.xyz;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 28
    precise float4 v = float4(-(abs(r0)).xxxx > c[10].zzzz);
    r2.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 29
    precise float4 v = -r6.zyxx + c[3].zyxx;
    r9.xyz = v.xyz;
  }
  { // 30
    precise float4 v = max(r8.yxxz, c[10].xxxx);
    r2.xzw = v.xzw;
  }
  { // 31
    precise float4 v = Dot3(r9.xzyy, r9.xzyy);
    precise float s = log2((abs(r2)).w);
    r2.y = v.y;
    r6.x = s;
    ps = s;
  }
  { // 32
    precise float4 v = Mul(-r6.xxxx, c[4].xxxx);
    precise float s = rsqrt((abs(r2)).y);
    r6.y = v.y;
    r2.y = s;
    ps = s;
  }
  { // 33
    precise float4 v = Mul(r9.xyzz, r2.yyyy);
    r9.xyz = v.xyz;
  }
  { // 34
    precise float4 v = saturate(Dot3(r7.zxyy, r9.xzyy));
    r6.x = v.x;
  }
  { // 35
    precise float4 v = Mul(r6.xxxx, c[4].xxxx);
    precise float s = log2((abs(r2)).x);
    r2.w = v.w;
    r2.y = s;
    ps = s;
  }
  { // 36
    precise float4 v = Mul(r6.yyyy, r6.xxxx);
    precise float s = log2((abs(r2)).z);
    r2.x = v.x;
    r2.z = s;
    ps = s;
  }
  { // 37
    precise float4 v = Mul(r2.wwww, -r2.yyzz);
    r2.yz = v.yz;
  }
  }
  { // 38
    precise float4 v = Mul(r6.wwww, r0.yyyy) + c[11].xxxx;
    r2.w = v.w;
  }
  { // 39
    precise float4 v = c[5].xyzz + -c[6].xyzz;
    r6.xyz = v.xyz;
  }
  { // 40
    precise float4 v = Mul(r6.xyzz, r2.wwww) + c[6].xyzz;
    r8.xyz = v.xyz;
  }
  { // 41
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    precise float s = MulS((r4.ww).x, (r4.ww).y);
    r2.w = v.w;
    r4.w = s;
    ps = s;
  }
  { // 42
    precise float4 v = Mul(r4.wwww, r5.zxyy);
    precise float s = rsqrt((abs(r2)).w);
    r6.xyz = v.xyz;
    r2.w = s;
    ps = s;
  }
  { // 43
    precise float4 v = Mul(r2.wwww, r1.xyzz) + c[8].xyzz;
    r5.xyz = v.xyz;
  }
  { // 44
    precise float4 v = Dot3(r5.zxyy, r5.zxyy);
    precise float s = MulS(c[9].z, r1.w);
    r1.y = v.y;
    r1.x = s;
    ps = s;
  }
  { // 45
    precise float4 v = saturate(Dot3(r4.zxyy, r0.zxyy));
    precise float s = rsqrt((abs(r1)).y);
    r0.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 46
    precise float4 v = Mul(r5.xyzz, r0.xxxx);
    precise float s = MulS(c[11].z, r0.w);
    r4.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 47
    precise float4 v = saturate(Dot3(r7.zxyy, r4.zxyy));
    precise float s = max((r0.xx).x, (r0.xx).y);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r0.zzzz, r1.wwww);
    precise float s = log2(r0.y);
    r0.w = v.w;
    r0.y = s;
    ps = s;
  }
  { // 49
    precise float4 v = Mul(r0.ywww, c[9].wxyy);
    precise float s = max((c[10].zz).x, (c[10].zz).y);
    r4.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 50
    precise float4 v = Mul(r1.xxxx, r0.zzzz);
    precise float s = exp2(r4.x);
    r4.w = v.w;
    r0.z = s;
    ps = s;
  }
  { // 51
    precise float4 v = Mul(r0.zzzz, c[9].xyzz);
    precise float s = max((r0.xy).x, (r0.xy).y);
    r1.xyz = v.xyz;
    r0.w = s;
    ps = s;
  }
  { // 52
    precise float4 v = Mul(r4.yzww, r8.wwww) + r2.xyzz;
    r0.xyz = v.xyz;
  }
  { // 53
    precise float4 v = Mul(r8.xzyy, r1.wwww) + r0.xzyy;
    r0.xyz = v.xyz;
  }
  { // 54
    precise float4 v = Mul(r1.xyzz, r0.wwww);
    r1.xyz = v.xyz;
  }
  { // 55
    precise float4 v = Mul(r1.xyzz, c[7].wwww);
    r1.xyz = v.xyz;
  }
  { // 56
    precise float4 v = Mul(r6.xyzz, r0.xzyy);
    r0.xyz = v.xyz;
  }
  { // 57
    precise float4 v = Mul(r1.xxxx, r8.wwww) + r0.xxxx;
    r0.x = v.x;
  }
  { // 58
    precise float4 v = Mul(r1.yyzz, r8.wwww) + r0.yyzz;
    r0.yz = v.yz;
  }
  { // 59
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.xyzz;
    r0.xyz = v.xyz;
  }
  { // 60
    precise float4 v = max(r0, r0);
    precise float s = sqrt((abs(r0)).x);
    oC0.y = 0.0;
    oC0.z = 0.0;
    oC0.w = 1.0;
    oC0.x = s;
    ps = s;
  }
  { // 61
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 62
    precise float4 v = sqrt((abs(r0)).z);
    oC0.z = v.z;
  }
  return oC0 * asfloat(xe_system[15].y);
}
