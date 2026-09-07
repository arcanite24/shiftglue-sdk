// FH1 A2C1F872E049AD8B / 00004000005B007F opaque packed-world material.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[14]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[6]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 8
    precise float4 v = Mul(r2.zwxy, c[1].zwxy);
    r9.xyzw = v.xyzw;
  }
  r4.z = SampleTexture(r9.xy, 6, 1, 0.0, 2).y;
  { // 10
    precise float4 v = Mul(r5.yyyy, c[13].xxxx);
    precise float s = rcp(r5.w);
    r0.w = v.w;
    r4.y = s;
    ps = s;
  }
  { // 11
    precise float4 v = Mul(r4.yyyz, c[13].xxxy);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r2.zw = v.zw;
    ps = s;
  }
  { // 12
    precise float4 v = Mul(r2.zzzz, r5.xxxx);
    precise float s = MulS(ps, r4.y);
    r4.x = v.x;
    r4.y = s;
    ps = s;
  }
  { // 13
    precise float4 v = r4.xxyy + c[13].xxxx;
    r4.yz = v.yz;
  }
  { // 14
    precise float4 v = Mul(r2.xyxy, c[0]);
    precise float s = c[13].w - (r4.z);
    r8.xyzw = v.xyzw;
    r4.x = s;
    ps = s;
  }
  r7.xyz = SampleTexture(r9.xy, 7, 4, 0.0, 7).xyz;
  r5.xyzw = SampleTexture(r4.yx, 13, 7, 0.5, 15).wzyx;
  r2.xy = SampleTexture(r9.xy, 2, 10, 0.0, 3).xy;
  r9.xyzw = SampleTexture(r9.zw, 5, 13, 0.0, 15).xyzw;
  r4.xyz = SampleTexture(r8.zw, 1, 16, 0.0, 7).xyz;
  r11.xyz = SampleTexture(r8.xy, 0, 19, 0.0, 7).xyz;
  { // 21
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    r0.w = v.w;
  }
  { // 22
    precise float4 v = r4.xxyz + -r11.xxyz;
    r10.yzw = v.yzw;
  }
  { // 23
    precise float4 v = Dot3(r4.zxyy, c[12].wwww);
    r12.x = v.x;
  }
  { // 24
    precise float4 v = Dot3(r9.zxyy, c[12].wwww);
    r12.y = v.y;
  }
  { // 25
    precise float4 v = saturate(max(r2.yxxx, r2.yxxx));
    precise float s = rsqrt((abs(r0)).w);
    r8.xz = v.xz;
    r0.w = s;
    ps = s;
  }
  { // 26
    precise float4 v = Dot3(r11.zxyy, c[12].wwww);
    precise float s = MulS(c[13].x, r0.w);
    r12.z = v.z;
    r6.w = s;
    ps = s;
  }
  { // 27
    precise float4 v = Mul(r0.wwww, c[10].xyzz);
    precise float s = max((r8.xx).x, (r8.xx).y);
    r4.xyz = v.xyz;
    ps = s;
  }
  { // 28
    precise float4 v = Mul(r12.zxyy, c[3].xyzz);
    precise float s = MulS(ps, r9.w);
    r8.xyw = v.xyw;
    r7.w = s;
    ps = s;
  }
  { // 29
    precise float4 v = r8.yyyy + -r8.xxxx;
    precise float s = c[12].y + r2.w;
    r10.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 30
    precise float4 v = Mul(r10.yzww, r8.zzzz) + r11.xyzz;
    r2.xyz = v.xyz;
  }
  { // 31
    precise float4 v = Mul(r10.xxxx, r8.zzzz) + r8.xxxx;
    r2.w = v.w;
  }
  { // 32
    precise float4 v = Mul(r0.wwww, r0.xyzz);
    precise float s = max((-r2.ww).x, (-r2.ww).y);
    r8.xyz = v.xyz;
    ps = s;
  }
  { // 33
    precise float4 v = -r2.xyzz + r9.xyzz;
    precise float s = ps + r8.w;
    r9.xyz = v.xyz;
    r9.w = s;
    ps = s;
  }
  { // 34
    precise float4 v = Mul(r7.wwww, r9.wxyz) + r2.wxyz;
    r2.xyzw = v.xyzw;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 35
    precise float4 v = float4(-(abs(r0)).xxxx > c[12].zzzz);
    r6.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 36
    precise float4 v = -r6.zyxx + c[4].zyxx;
    r9.xyz = v.xyz;
  }
  { // 37
    precise float4 v = max(r5.zyxx, c[12].xxxx);
    r6.xyz = v.xyz;
  }
  { // 38
    precise float4 v = Dot3(r9.xzyy, r9.xzyy);
    precise float s = log2((abs(r6)).x);
    r0.w = v.w;
    r7.w = s;
    ps = s;
  }
  { // 39
    precise float4 v = Mul(-r7.wwww, c[5].xxxx);
    precise float s = rsqrt((abs(r0)).w);
    r8.w = v.w;
    r0.w = s;
    ps = s;
  }
  { // 40
    precise float4 v = Mul(r9.xyzz, r0.wwww);
    r9.xyz = v.xyz;
  }
  { // 41
    precise float4 v = saturate(Dot3(r8.zxyy, r9.xzyy));
    r7.w = v.w;
  }
  { // 42
    precise float4 v = Mul(r7.wwww, c[5].xxxx);
    precise float s = log2((abs(r6)).y);
    r0.w = v.w;
    r6.y = s;
    ps = s;
  }
  { // 43
    precise float4 v = Mul(r8.wwww, r7.wwww);
    precise float s = log2((abs(r6)).z);
    r6.x = v.x;
    r6.z = s;
    ps = s;
  }
  { // 44
    precise float4 v = Mul(r0.wwww, -r6.zzyy);
    r6.yz = v.yz;
  }
  }
  { // 45
    precise float4 v = Mul(r6.wwww, r0.yyyy) + c[13].xxxx;
    r6.w = v.w;
  }
  { // 46
    precise float4 v = c[6].xyzz + -c[7].xyzz;
    precise float s = MulS((r4.ww).x, (r4.ww).y);
    r5.xyz = v.xyz;
    r0.w = s;
    ps = s;
  }
  { // 47
    precise float4 v = Mul(r0.wwww, r2.yyzw);
    r2.yzw = v.yzw;
  }
  { // 48
    precise float4 v = max(c[8].yyyy, c[2].wwww);
    r0.w = v.w;
  }
  { // 49
    precise float4 v = Mul(r5.xyzz, r6.wwww) + c[7].xyzz;
    r5.xyz = v.xyz;
  }
  { // 50
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    precise float s = MulS(c[13].z, r2.x);
    r4.w = v.w;
    r2.x = s;
    ps = s;
  }
  { // 51
    precise float4 v = max(r2.xxxx, c[12].zzzz);
    precise float s = rsqrt((abs(r4)).w);
    r2.x = v.x;
    r4.w = s;
    ps = s;
  }
  { // 52
    precise float4 v = Mul(r4.wwww, r1.xyzz) + c[10].xyzz;
    r1.xyz = v.xyz;
  }
  { // 53
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    r4.w = v.w;
  }
  { // 54
    precise float4 v = saturate(Dot3(r4.zxyy, r0.zxyy));
    precise float s = rsqrt((abs(r4)).w);
    r0.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 55
    precise float4 v = Mul(r1.xyzz, r0.yyyy);
    r1.xyz = v.xyz;
  }
  { // 56
    precise float4 v = saturate(Dot3(r8.zxyy, r1.zxyy));
    r0.z = v.z;
  }
  { // 57
    precise float4 v = Mul(r0.xxxx, r1.wwww);
    precise float s = log2(r0.z);
    r0.y = v.y;
    r0.z = s;
    ps = s;
  }
  { // 58
    precise float4 v = Mul(r0.yyzz, c[11].xyww);
    precise float s = MulS(c[11].z, r1.w);
    r4.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 59
    precise float4 v = Mul(r0.yyyy, r0.xxxx);
    precise float s = exp2(r4.z);
    r4.w = v.w;
    r0.x = s;
    ps = s;
  }
  { // 60
    precise float4 v = Mul(r4.xwyy, r5.wwww) + r6.xyzz;
    r1.xyz = v.xyz;
  }
  { // 61
    precise float4 v = Mul(r0.xxxx, c[11].xyzz);
    r0.xyz = v.xyz;
  }
  { // 62
    precise float4 v = Mul(r0.xyzz, r2.xxxx);
    r0.xyz = v.xyz;
  }
  { // 63
    precise float4 v = Mul(r0.wwww, r7.xyzz) + r1.xzyy;
    r1.xyz = v.xyz;
  }
  { // 64
    precise float4 v = Mul(r5.xyzz, r1.wwww) + r1.xyzz;
    r1.xyz = v.xyz;
  }
  { // 65
    precise float4 v = Mul(r0.xyzz, c[9].wwww);
    r0.xyz = v.xyz;
  }
  { // 66
    precise float4 v = Mul(r0.xyzz, r5.wwww);
    r0.xyz = v.xyz;
  }
  { // 67
    precise float4 v = Mul(r2.yzww, r1.xyzz) + r0.xyzz;
    r0.xyz = v.xyz;
  }
  { // 68
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.xyzz;
    r0.xyz = v.xyz;
  }
  { // 69
    precise float4 v = max(r0, r0);
    precise float s = sqrt((abs(r0)).x);
    oC0.y = 0.0;
    oC0.z = 0.0;
    oC0.w = 1.0;
    oC0.x = s;
    ps = s;
  }
  { // 70
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 71
    precise float4 v = sqrt((abs(r0)).z);
    oC0.z = v.z;
  }
  return oC0 * asfloat(xe_system[15].y);
}
