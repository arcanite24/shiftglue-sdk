// FH1 packed-world pixel program D96CCDCC3F783790; exact captured opaque variant.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[11]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[2]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 6
    precise float4 v = Mul(r5.yyyy, c[10].xxxx);
    precise float s = rcp(r5.w);
    r4.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 7
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    precise float s = MulS(c[10].x, r0.w);
    r1.w = v.w;
    r4.x = s;
    ps = s;
  }
  { // 8
    precise float4 v = Mul(r2.xyyy, c[0].xyyy);
    precise float s = max((r4.yy).x, (r4.yy).y);
    r7.xy = v.xy;
    ps = s;
  }
  { // 9
    precise float4 v = Mul(r4.xxxx, r5.xxxx);
    precise float s = MulS(ps, r0.w);
    r2.x = v.x;
    r2.y = s;
    ps = s;
  }
  { // 10
    precise float4 v = r2.yxxx + c[10].xxxx;
    r2.xz = v.xz;
  }
  { // 11
    precise float4 v = Mul(c[1].xxxx, c[10].yyyy);
    precise float s = c[10].z - (r2.x);
    r4.x = v.x;
    r2.y = s;
    ps = s;
  }
  r5.xyzw = SampleTexture(r2.zy, 13, 1, 0.5, 15).wzyx;
  r7.xyz = SampleTexture(r7.xy, 0, 4, 0.0, 7).xyz;
  { // 14
    precise float4 v = Dot3(r7.zxyy, c[10].wwww);
    precise float s = rsqrt((abs(r1)).w);
    r4.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 15
    precise float4 v = Mul(r0.wwww, c[7].xyzz);
    r2.xyz = v.xyz;
  }
  { // 16
    precise float4 v = Mul(r0.wwww, r0.xyzz);
    precise float s = MulS(c[10].x, r0.w);
    r8.xyz = v.xyz;
    r0.w = s;
    ps = s;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 17
    precise float4 v = float4(-(abs(r0)).xxxx > c[9].xxxx);
    r6.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 18
    precise float4 v = -r6.zyxx + c[2].zyxx;
    r9.xyz = v.xyz;
  }
  { // 19
    precise float4 v = max(r5.zyxx, c[9].yyyy);
    r6.xyz = v.xyz;
  }
  { // 20
    precise float4 v = Dot3(r9.xzyy, r9.xzyy);
    precise float s = log2((abs(r6)).x);
    r1.w = v.w;
    r2.w = s;
    ps = s;
  }
  { // 21
    precise float4 v = Mul(-r2.wwww, c[3].xxxx);
    precise float s = rsqrt((abs(r1)).w);
    r6.w = v.w;
    r1.w = s;
    ps = s;
  }
  { // 22
    precise float4 v = Mul(r9.xyzz, r1.wwww);
    r9.xyz = v.xyz;
  }
  { // 23
    precise float4 v = saturate(Dot3(r8.zxyy, r9.xzyy));
    r2.w = v.w;
  }
  { // 24
    precise float4 v = Mul(r2.wwww, c[3].xxxx);
    precise float s = log2((abs(r6)).y);
    r1.w = v.w;
    r6.y = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(r6.wwww, r2.wwww);
    precise float s = log2((abs(r6)).z);
    r6.x = v.x;
    r6.z = s;
    ps = s;
  }
  { // 26
    precise float4 v = Mul(r1.wwww, -r6.yyzz);
    r6.yz = v.yz;
  }
  }
  { // 27
    precise float4 v = Mul(r0.wwww, r0.yyyy) + c[10].xxxx;
    r0.w = v.w;
  }
  { // 28
    precise float4 v = c[4].xyzz + -c[5].xyzz;
    r5.xyz = v.xyz;
  }
  { // 29
    precise float4 v = Mul(r5.xyzz, r0.wwww) + c[5].xyzz;
    r5.xyz = v.xyz;
  }
  { // 30
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    r0.w = v.w;
  }
  { // 31
    precise float4 v = saturate(Dot3(r2.zxyy, r0.zxyy));
    precise float s = rsqrt((abs(r0)).w);
    r0.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 32
    precise float4 v = Mul(r0.xxxx, r1.xyzz) + c[7].xyzz;
    r1.xyz = v.xyz;
  }
  { // 33
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    r0.w = v.w;
  }
  { // 34
    precise float4 v = Mul(r4.xwww, r4.ywww);
    precise float s = rsqrt((abs(r0)).w);
    r0.xy = v.xy;
    r0.w = s;
    ps = s;
  }
  { // 35
    precise float4 v = Mul(r1.xyzz, r0.wwww);
    r1.xyz = v.xyz;
  }
  { // 36
    precise float4 v = saturate(Dot3(r8.zxyy, r1.zxyy));
    r0.w = v.w;
  }
  { // 37
    precise float4 v = max(r0.xxxx, c[9].xxxx);
    precise float s = log2(r0.w);
    r0.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 38
    precise float4 v = Mul(r0.zzzw, c[8]);
    r1.xyzw = v.xyzw;
  }
  { // 39
    precise float4 v = Mul(r1.xyzz, r5.wwww) + r5.xyzz;
    r1.xyz = v.xyz;
  }
  { // 40
    precise float4 v = Mul(r0.yyyy, r7.xyzz);
    precise float s = exp2(r1.w);
    r2.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 41
    precise float4 v = Mul(r0.yyyy, c[8].xxyz);
    r0.yzw = v.yzw;
  }
  { // 42
    precise float4 v = r1.xyzz + r6.xyzz;
    r1.xyz = v.xyz;
  }
  { // 43
    precise float4 v = Mul(r0.yzww, r0.xxxx);
    r0.xyz = v.xyz;
  }
  { // 44
    precise float4 v = Mul(r0.xyzz, c[6].wwww);
    r0.xyz = v.xyz;
  }
  { // 45
    precise float4 v = Mul(r0.xyzz, r5.wwww);
    r0.xyz = v.xyz;
  }
  { // 46
    precise float4 v = Mul(r2.xyzz, r1.xyzz) + r0.xyzz;
    r0.xyz = v.xyz;
  }
  { // 47
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.xyzz;
    r0.xyz = v.xyz;
  }
  { // 48
    precise float4 v = max(r0, r0);
    precise float s = sqrt((abs(r0)).x);
    oC0.y = 0.0;
    oC0.z = 0.0;
    oC0.w = 1.0;
    oC0.x = s;
    ps = s;
  }
  { // 49
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 50
    precise float4 v = sqrt((abs(r0)).z);
    oC0.z = v.z;
  }
  return oC0 * asfloat(xe_system[15].y);
}
