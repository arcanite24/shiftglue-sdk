// FH1 FF096DC71B188012 / 00004000005B007F opaque packed-world-blend material.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[14]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[5]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 7
    precise float4 v = Mul(r2.zwww, c[1].zwww);
    r10.xy = v.xy;
  }
  r4.z = SampleTexture(r10.xy, 5, 1, 0.0, 2).y;
  { // 9
    precise float4 v = Mul(r5.yyyy, c[13].xxxx);
    precise float s = rcp(r5.w);
    r0.w = v.w;
    r4.x = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r4.xxxz, c[13].xxxy);
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
    precise float4 v = r4.yyxx + c[13].xxxx;
    r4.yz = v.yz;
  }
  { // 13
    precise float4 v = Mul(r2.xyxy, c[0]);
    precise float s = c[13].w - (r4.z);
    r5.xyzw = v.xyzw;
    r4.x = s;
    ps = s;
  }
  r8.xyz = SampleTexture(r10.xy, 7, 4, 0.0, 7).xzy;
  r9.xyzw = SampleTexture(r4.yx, 13, 7, 0.5, 15).wzyx;
  r2.xyz = SampleTexture(r5.zw, 1, 10, 0.0, 7).xyz;
  r7.xyz = SampleTexture(r5.xy, 0, 13, 0.0, 7).xyz;
#if defined(FH1_BLEND_MASK_Y)
  r1.w = SampleTexture(r10.xy, 2, 16, 0.0, 2).y;
#else
  r1.w = SampleTexture(r10.xy, 2, 16, 0.0, 1).x;
#endif
  { // 19
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    r0.w = v.w;
  }
  { // 20
    precise float4 v = r2.xxyz + -r7.xxyz;
    r10.yzw = v.yzw;
  }
  { // 21
    precise float4 v = Dot3(r7.zxyy, c[12].wwww);
    precise float s = rsqrt((abs(r0)).w);
    r11.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 22
    precise float4 v = Mul(r0.wwww, c[13].xxxx);
    precise float s = saturate(max((r1.ww).x, (r1.ww).y));
    r6.w = v.w;
    r5.w = s;
    ps = s;
  }
  { // 23
    precise float4 v = Dot3(r2.zxyy, c[12].wwww);
    precise float s = MulS(c[10].x, r0.w);
    r11.y = v.y;
    r5.x = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r0.wwww, r0.xyzz);
    precise float s = MulS(c[10].y, r0.w);
    r2.xyz = v.xyz;
    r5.y = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(r11.xyyy, c[3].xyyy);
    precise float s = MulS(c[10].z, r0.w);
    r11.xy = v.xy;
    r5.z = s;
    ps = s;
  }
  { // 26
    precise float4 v = r11.yyyy + -r11.xxxx;
    precise float s = c[12].y + r2.w;
    r10.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 27
    precise float4 v = Mul(r10.xxxx, r5.wwww) + r11.xxxx;
    r1.w = v.w;
  }
  { // 28
    precise float4 v = Mul(r10.zwyy, r5.wwww) + r7.yzxx;
    r7.xyz = v.xyz;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 29
    precise float4 v = float4(-(abs(r0)).xxxx > c[12].zzzz);
    r4.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 30
    precise float4 v = -r6.zyxx + c[4].zyxx;
    r10.xyz = v.xyz;
  }
  { // 31
    precise float4 v = max(r9.zyxx, c[12].xxxx);
    r4.xyz = v.xyz;
  }
  { // 32
    precise float4 v = Dot3(r10.xzyy, r10.xzyy);
    precise float s = log2((abs(r4)).x);
    r2.w = v.w;
    r6.x = s;
    ps = s;
  }
  { // 33
    precise float4 v = Mul(-r6.xxxx, c[5].xxxx);
    precise float s = rsqrt((abs(r2)).w);
    r6.y = v.y;
    r2.w = s;
    ps = s;
  }
  { // 34
    precise float4 v = Mul(r10.xyzz, r2.wwww);
    r10.xyz = v.xyz;
  }
  { // 35
    precise float4 v = saturate(Dot3(r2.zxyy, r10.xzyy));
    r6.x = v.x;
  }
  { // 36
    precise float4 v = Mul(r6.xxxx, c[5].xxxx);
    precise float s = log2((abs(r4)).y);
    r2.w = v.w;
    r4.y = s;
    ps = s;
  }
  { // 37
    precise float4 v = Mul(r6.yyyy, r6.xxxx);
    precise float s = log2((abs(r4)).z);
    r4.x = v.x;
    r4.z = s;
    ps = s;
  }
  { // 38
    precise float4 v = Mul(r2.wwww, -r4.zzyy);
    r4.yz = v.yz;
  }
  }
  { // 39
    precise float4 v = Mul(r6.wwww, r0.yyyy) + c[13].xxxx;
    r2.w = v.w;
  }
  { // 40
    precise float4 v = c[6].xyzz + -c[7].xyzz;
    precise float s = MulS((r4.ww).x, (r4.ww).y);
    r9.xyz = v.xyz;
    r4.w = s;
    ps = s;
  }
  { // 41
    precise float4 v = Mul(r4.wwww, r7.zxyy);
    r6.xyz = v.xyz;
  }
  { // 42
    precise float4 v = Mul(r9.xyzz, r2.wwww) + c[7].xyzz;
    r7.xyz = v.xyz;
  }
  { // 43
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    precise float s = MulS(c[13].z, r1.w);
    r2.w = v.w;
    r1.w = s;
    ps = s;
  }
  { // 44
    precise float4 v = max(r1.wwww, c[12].zzzz);
    precise float s = rsqrt((abs(r2)).w);
    r1.w = v.w;
    r2.w = s;
    ps = s;
  }
  { // 45
    precise float4 v = Mul(r2.wwww, r1.xyzz) + c[10].xyzz;
    r1.xyz = v.xyz;
  }
  { // 46
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    r2.w = v.w;
  }
  { // 47
    precise float4 v = saturate(Dot3(r5.zxyy, r0.zxyy));
    precise float s = rsqrt((abs(r2)).w);
    r0.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r1.xyzz, r0.xxxx);
    precise float s = MulS(c[11].z, r0.w);
    r5.xyz = v.xyz;
    r1.x = s;
    ps = s;
  }
  { // 49
    precise float4 v = saturate(Dot3(r2.zxyy, r5.zxyy));
    precise float s = max((c[8].yy).x, (c[8].yy).y);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 50
    precise float4 v = Mul(r0.zzzz, r0.wwww);
    precise float s = log2(r0.y);
    r1.z = v.z;
    r1.y = s;
    ps = s;
  }
  { // 51
    precise float4 v = Mul(r1.yzzz, c[11].wxyy);
    precise float s = max((c[2].ww).x, (c[2].ww).y);
    r5.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 52
    precise float4 v = Mul(r1.xxxx, r0.zzzz);
    precise float s = exp2(r5.x);
    r5.w = v.w;
    r0.z = s;
    ps = s;
  }
  { // 53
    precise float4 v = Mul(r0.zzzz, c[11].xyzz);
    precise float s = max((r0.xy).x, (r0.xy).y);
    r1.xyz = v.xyz;
    r2.x = s;
    ps = s;
  }
  { // 54
    precise float4 v = Mul(r5.zyww, r9.wwww) + r4.zxyy;
    r0.xyz = v.xyz;
  }
  { // 55
    precise float4 v = Mul(r2.xxxx, r8.xzyy) + r0.yxzz;
    r0.xyz = v.xyz;
  }
  { // 56
    precise float4 v = Mul(r1.xyzz, r1.wwww);
    r1.xyz = v.xyz;
  }
  { // 57
    precise float4 v = Mul(r1.xyzz, c[9].wwww);
    r1.xyz = v.xyz;
  }
  { // 58
    precise float4 v = Mul(r7.xzyy, r0.wwww) + r0.xzyy;
    r0.xyz = v.xyz;
  }
  { // 59
    precise float4 v = Mul(r6.yzxx, r0.zyxx);
    r0.xyz = v.xyz;
  }
  { // 60
    precise float4 v = Mul(r1.yzzz, r9.wwww) + r0.xyyy;
    r0.xy = v.xy;
  }
  { // 61
    precise float4 v = Mul(r1.xxxx, r9.wwww) + r0.zzzz;
    r0.z = v.z;
  }
  { // 62
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.yzxx;
    r0.xyz = v.xyz;
  }
  { // 63
    precise float4 v = max(r0, r0);
    precise float s = sqrt((abs(r0)).z);
    oC0.y = 0.0;
    oC0.z = 0.0;
    oC0.w = 1.0;
    oC0.x = s;
    ps = s;
  }
  { // 64
    precise float4 v = sqrt((abs(r0)).x);
    oC0.y = v.y;
  }
  { // 65
    precise float4 v = sqrt((abs(r0)).y);
    oC0.z = v.z;
  }
  return oC0 * asfloat(xe_system[15].y);
}
