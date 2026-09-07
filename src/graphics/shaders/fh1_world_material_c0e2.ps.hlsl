// FH1 packed-world pixel program C0E286228970074D; exact captured opaque variant.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[13]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[3]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 6
    precise float4 v = Mul(r2.xyyy, c[1].zwww);
    precise float s = c[12].x - (r4.w);
    r7.xy = v.xy;
    r7.z = s;
    ps = s;
  }
  r0.w = SampleTexture(r7.xy, 2, 1, 0.0, 1).x;
  { // 8
    precise float s = MulS(c[12].x, r5.y);
    r1.w = s;
    ps = s;
  }
  { // 9
    precise float4 v = r0.wwww + -c[12].xxxx;
    precise float s = rcp(r5.w);
    r7.w = v.w;
    r0.w = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r1.wwww, r0.wwww);
    precise float s = MulS(c[12].x, r0.w);
    r9.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 11
    precise float4 v = Mul(r2.xyyy, c[0].xyyy);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r7.xy = v.xy;
    ps = s;
  }
  { // 12
    precise float4 v = Mul(r7.zzzw, c[2].xxxy);
    precise float s = MulS(ps, r5.x);
    r9.zw = v.zw;
    r9.x = s;
    ps = s;
  }
  { // 13
    precise float4 v = r9.yxxz + c[12].xxxx;
    r2.xzw = v.xzw;
  }
  { // 14
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    precise float s = c[12].z - (r2.x);
    r0.w = v.w;
    r2.y = s;
    ps = s;
  }
  r5.xyzw = SampleTexture(r7.xy, 0, 4, 0.0, 15).yzxw;
  r8.xyzw = SampleTexture(r2.zy, 13, 7, 0.5, 15).wzyx;
  { // 17
    precise float4 v = rsqrt((abs(r0)).w);
    r0.w = v.w;
  }
  { // 18
    precise float4 v = Mul(r0.wwww, c[9].xyzz);
    r7.xyz = v.xyz;
  }
  { // 19
    precise float4 v = Mul(r0.wwww, r0.xyzz);
    r2.xyz = v.xyz;
  }
  { // 20
    precise float4 v = saturate(r2.wwww + r9.wwww);
    precise float s = MulS(c[12].x, r0.w);
    r1.w = v.w;
    r0.w = s;
    ps = s;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 21
    precise float4 v = float4(-(abs(r0)).xxxx > c[11].xxxx);
    r6.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 22
    precise float4 v = -r6.zyxx + c[3].zyxx;
    r9.xyz = v.xyz;
  }
  { // 23
    precise float4 v = max(r8.zyxx, c[11].yyyy);
    r6.xyz = v.xyz;
  }
  { // 24
    precise float4 v = Dot3(r9.xzyy, r9.xzyy);
    precise float s = log2((abs(r6)).x);
    r2.w = v.w;
    r6.w = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(-r6.wwww, c[4].xxxx);
    precise float s = rsqrt((abs(r2)).w);
    r7.w = v.w;
    r2.w = s;
    ps = s;
  }
  { // 26
    precise float4 v = Mul(r9.xyzz, r2.wwww);
    r9.xyz = v.xyz;
  }
  { // 27
    precise float4 v = saturate(Dot3(r2.zxyy, r9.xzyy));
    r6.w = v.w;
  }
  { // 28
    precise float4 v = Mul(r6.wwww, c[4].xxxx);
    precise float s = log2((abs(r6)).y);
    r2.w = v.w;
    r6.y = s;
    ps = s;
  }
  { // 29
    precise float4 v = Mul(r7.wwww, r6.wwww);
    precise float s = log2((abs(r6)).z);
    r6.x = v.x;
    r6.z = s;
    ps = s;
  }
  { // 30
    precise float4 v = Mul(r2.wwww, -r6.zzyy);
    r6.yz = v.yz;
  }
  }
  { // 31
    precise float4 v = Mul(r0.wwww, r0.yyyy) + c[12].xxxx;
    r2.w = v.w;
  }
  { // 32
    precise float4 v = c[5].xzyy + -c[6].xzyy;
    r8.xyz = v.xyz;
  }
  { // 33
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    r0.w = v.w;
  }
  { // 34
    precise float4 v = Mul(r8.xyzz, r2.wwww) + c[6].xzyy;
    r8.xyz = v.xyz;
  }
  { // 35
    precise float4 v = Mul(r4.xyzz, c[7].yyyy) + r8.xzyy;
    r4.xyz = v.xyz;
  }
  { // 36
    precise float4 v = -r1.wwww + c[12].zzzz;
    precise float s = rsqrt((abs(r0)).w);
    r1.w = v.w;
    r0.w = s;
    ps = s;
  }
  { // 37
    precise float4 v = Mul(r0.wwww, r1.xyzz) + c[9].xyzz;
    r8.xyz = v.xyz;
  }
  { // 38
    precise float4 v = Dot3(r8.zxyy, r8.zxyy);
    r0.w = v.w;
  }
  { // 39
    precise float4 v = Dot3(r5.yzxx, c[12].wwww);
    precise float s = rsqrt((abs(r0)).w);
    r1.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 40
    precise float4 v = Mul(r8.xyzz, r0.wwww);
    r8.xyz = v.xyz;
  }
  { // 41
    precise float4 v = saturate(Dot3(r2.zxyy, r8.zxyy));
    r0.w = v.w;
  }
  { // 42
    precise float4 v = saturate(Dot3(r7.zxyy, r0.zxyy));
    precise float s = log2(r0.w);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 43
    precise float4 v = Mul(r0.yyyx, c[10].yzxw);
    r0.xyzw = v.xyzw;
  }
  { // 44
    precise float4 v = Mul(r0.zxyy, r8.wwww) + r4.xyzz;
    r0.xyz = v.xyz;
  }
  { // 45
    precise float4 v = Mul(r1.xxxx, c[12].yyyy);
    precise float s = exp2(r0.w);
    r1.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 46
    precise float4 v = max(r1.xxxx, c[11].xxxx);
    precise float s = MulS(c[10].x, r0.w);
    r1.x = v.x;
    r2.x = s;
    ps = s;
  }
  { // 47
    precise float4 v = r0.xzyy + r6.xyzz;
    precise float s = MulS(c[10].y, r0.w);
    r0.xyz = v.xyz;
    r2.y = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r0.xzyy, r5.zxyy);
    precise float s = MulS(c[10].z, r0.w);
    r0.xyz = v.xyz;
    r2.z = s;
    ps = s;
  }
  { // 49
    precise float4 v = Mul(r2.xyzz, r1.xxxx);
    r1.xyz = v.xyz;
  }
  { // 50
    precise float4 v = Mul(r1.xyzz, c[8].wwww);
    r1.xyz = v.xyz;
  }
  { // 51
    precise float4 v = Mul(r1.xxxx, r8.wwww) + r0.xxxx;
    r0.x = v.x;
  }
  { // 52
    precise float4 v = Mul(r1.yyzz, r8.wwww) + r0.yyzz;
    r0.yz = v.yz;
  }
  { // 53
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.xyzz;
    r0.xyz = v.xyz;
  }
  { // 54
    precise float4 v = sqrt((abs(r0)).x);
    oC0.x = v.x;
  }
  { // 55
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 56
    precise float4 v = Mul(r1.wwww, r5.wwww);
    precise float s = sqrt((abs(r0)).z);
    oC0.w = v.w;
    oC0.z = s;
    ps = s;
  }
  return oC0 * asfloat(xe_system[15].y);
}
