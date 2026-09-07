// FH1 packed-world pixel program B98566FB7CE14699; exact captured opaque variant.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[12]; };
cbuffer xe_bool_loop_cbuffer : register(b2) { uint4 boolean_loop[10]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };
cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 descriptors[5]; };
#include "fh1_world_material.hlsli"

[earlydepthstencil]
float4 main(centroid float4 p0:TEXCOORD0,centroid float4 p1:TEXCOORD1,float4 p2:TEXCOORD2,centroid float4 p3:TEXCOORD3,centroid float4 p4:TEXCOORD4,float4 p5:TEXCOORD5,centroid float4 p6:TEXCOORD6):SV_Target {
  precise float4 r0=p0,r1=p1,r2=p2,r3=p3,r4=p4,r5=p5,r6=p6,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,oC0=0;
  precise float ps=0;
  { // 7
    precise float4 v = Mul(r2.zwxy, c[1].zwxy);
    r7.xyzw = v.xyzw;
  }
  r4.z = SampleTexture(r7.xy, 6, 1, 0.0, 2).y;
  { // 9
    precise float4 v = Mul(r5.yyyy, c[11].xxxx);
    precise float s = rcp(r5.w);
    r0.w = v.w;
    r4.y = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r4.yyyz, c[11].xxxy);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r2.zw = v.zw;
    ps = s;
  }
  { // 11
    precise float4 v = Mul(r2.zzzz, r5.xxxx);
    precise float s = MulS(ps, r4.y);
    r4.x = v.x;
    r4.y = s;
    ps = s;
  }
  { // 12
    precise float4 v = r4.xxyy + c[11].xxxx;
    r4.yz = v.yz;
  }
  { // 13
    precise float4 v = Mul(r2.xyxy, c[0]);
    precise float s = c[11].w - (r4.z);
    r8.xyzw = v.xyzw;
    r4.x = s;
    ps = s;
  }
  r5.xyzw = SampleTexture(r4.yx, 13, 4, 0.5, 15).wzyx;
  r2.xy = SampleTexture(r7.xy, 2, 7, 0.0, 3).xy;
  r9.xyzw = SampleTexture(r7.zw, 5, 10, 0.0, 15).xyzw;
  r7.xyz = SampleTexture(r8.zw, 1, 13, 0.0, 7).xyz;
  r11.xyz = SampleTexture(r8.xy, 0, 16, 0.0, 7).xyz;
  { // 19
    precise float4 v = Dot3(r0.zxyy, r0.zxyy);
    r0.w = v.w;
  }
  { // 20
    precise float4 v = r7.xxyz + -r11.xxyz;
    r10.yzw = v.yzw;
  }
  { // 21
    precise float4 v = Dot3(r7.zxyy, c[10].wwww);
    r12.x = v.x;
  }
  { // 22
    precise float4 v = Dot3(r9.zxyy, c[10].wwww);
    r12.y = v.y;
  }
  { // 23
    precise float4 v = saturate(max(r2.yxxx, r2.yxxx));
    precise float s = rsqrt((abs(r0)).w);
    r8.xz = v.xz;
    r1.w = s;
    ps = s;
  }
  { // 24
    precise float4 v = Dot3(r11.zxyy, c[10].wwww);
    precise float s = MulS(c[11].x, r1.w);
    r12.z = v.z;
    r6.w = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(r1.wwww, c[8].xyzz);
    precise float s = max((r8.xx).x, (r8.xx).y);
    r7.xyz = v.xyz;
    ps = s;
  }
  { // 26
    precise float4 v = Mul(r12.zxyy, c[2].xyzz);
    precise float s = MulS(ps, r9.w);
    r8.xyw = v.xyw;
    r7.w = s;
    ps = s;
  }
  { // 27
    precise float4 v = r8.yyyy + -r8.xxxx;
    precise float s = c[10].y + r2.w;
    r10.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 28
    precise float4 v = Mul(r10.yzww, r8.zzzz) + r11.xyzz;
    r2.xyz = v.xyz;
  }
  { // 29
    precise float4 v = Mul(r10.xxxx, r8.zzzz) + r8.xxxx;
    r2.w = v.w;
  }
  { // 30
    precise float4 v = Mul(r1.wwww, r0.xyzz);
    precise float s = max((-r2.ww).x, (-r2.ww).y);
    r8.xyz = v.xyz;
    ps = s;
  }
  { // 31
    precise float4 v = -r2.xyzz + r9.xyzz;
    precise float s = ps + r8.w;
    r9.xyz = v.xyz;
    r9.w = s;
    ps = s;
  }
  { // 32
    precise float4 v = Mul(r7.wwww, r9.wxyz) + r2.wxyz;
    r2.xyzw = v.xyzw;
  }
  if (!(boolean_loop[1].w & 16u)) {
  { // 33
    precise float4 v = float4(-(abs(r0)).xxxx > c[10].zzzz);
    r4.xyz = v.xyz;
  }
  }
  if ((boolean_loop[1].w & 16u)) {
  { // 34
    precise float4 v = -r6.zyxx + c[3].zyxx;
    r9.xyz = v.xyz;
  }
  { // 35
    precise float4 v = max(r5.zyxx, c[10].xxxx);
    r4.xyz = v.xyz;
  }
  { // 36
    precise float4 v = Dot3(r9.xzyy, r9.xzyy);
    precise float s = log2((abs(r4)).x);
    r1.w = v.w;
    r6.x = s;
    ps = s;
  }
  { // 37
    precise float4 v = Mul(-r6.xxxx, c[4].xxxx);
    precise float s = rsqrt((abs(r1)).w);
    r6.y = v.y;
    r1.w = s;
    ps = s;
  }
  { // 38
    precise float4 v = Mul(r9.xyzz, r1.wwww);
    r9.xyz = v.xyz;
  }
  { // 39
    precise float4 v = saturate(Dot3(r8.zxyy, r9.xzyy));
    r6.x = v.x;
  }
  { // 40
    precise float4 v = Mul(r6.xxxx, c[4].xxxx);
    precise float s = log2((abs(r4)).y);
    r1.w = v.w;
    r4.y = s;
    ps = s;
  }
  { // 41
    precise float4 v = Mul(r6.yyyy, r6.xxxx);
    precise float s = log2((abs(r4)).z);
    r4.x = v.x;
    r4.z = s;
    ps = s;
  }
  { // 42
    precise float4 v = Mul(r1.wwww, -r4.yyzz);
    r4.yz = v.yz;
  }
  }
  { // 43
    precise float4 v = Mul(r6.wwww, r0.yyyy) + c[11].xxxx;
    r1.w = v.w;
  }
  { // 44
    precise float4 v = c[5].xyzz + -c[6].xyzz;
    r6.xyz = v.xyz;
  }
  { // 45
    precise float4 v = Mul(r6.xyzz, r1.wwww) + c[6].xyzz;
    r5.xyz = v.xyz;
  }
  { // 46
    precise float4 v = Dot3(r1.zxyy, r1.zxyy);
    precise float s = MulS((r4.ww).x, (r4.ww).y);
    r1.w = v.w;
    r4.w = s;
    ps = s;
  }
  { // 47
    precise float4 v = Mul(r4.wwww, r2.yyzw);
    precise float s = rsqrt((abs(r1)).w);
    r2.yzw = v.yzw;
    r1.w = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r1.wwww, r1.xyzz) + c[8].xyzz;
    r1.xyw = v.xyw;
  }
  { // 49
    precise float4 v = Dot3(r1.wxyy, r1.wxyy);
    precise float s = MulS(c[9].z, r0.w);
    r4.w = v.w;
    r1.z = s;
    ps = s;
  }
  { // 50
    precise float4 v = saturate(Dot3(r7.zxyy, r0.zxyy));
    precise float s = rsqrt((abs(r4)).w);
    r0.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 51
    precise float4 v = Mul(r1.xyww, r0.xxxx);
    precise float s = MulS(c[11].z, r2.x);
    r1.xyw = v.xyw;
    r0.x = s;
    ps = s;
  }
  { // 52
    precise float4 v = saturate(Dot3(r8.zxyy, r1.wxyy));
    precise float s = max((r0.xx).x, (r0.xx).y);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 53
    precise float4 v = Mul(r0.zzzz, r0.wwww);
    precise float s = log2(r0.y);
    r1.x = v.x;
    r1.y = s;
    ps = s;
  }
  { // 54
    precise float4 v = Mul(r1.xxyy, c[9].xyww);
    precise float s = max((c[10].zz).x, (c[10].zz).y);
    r1.xyw = v.xyw;
    r0.y = s;
    ps = s;
  }
  { // 55
    precise float4 v = Mul(r1.zzzz, r0.zzzz);
    precise float s = exp2(r1.w);
    r1.z = v.z;
    r0.z = s;
    ps = s;
  }
  { // 56
    precise float4 v = Mul(r0.zzzz, c[9].xyzz);
    precise float s = max((r0.xy).x, (r0.xy).y);
    r6.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 57
    precise float4 v = Mul(r1.xyzz, r5.wwww) + r4.xyzz;
    r1.xyz = v.xyz;
  }
  { // 58
    precise float4 v = Mul(r5.xyzz, r0.wwww) + r1.xyzz;
    r1.xyz = v.xyz;
  }
  { // 59
    precise float4 v = Mul(r6.xyzz, r0.xxxx);
    r0.xyz = v.xyz;
  }
  { // 60
    precise float4 v = Mul(r0.xyzz, c[7].wwww);
    r0.xyz = v.xyz;
  }
  { // 61
    precise float4 v = Mul(r0.xyzz, r5.wwww);
    r0.xyz = v.xyz;
  }
  { // 62
    precise float4 v = Mul(r2.yzww, r1.xyzz) + r0.xyzz;
    r0.xyz = v.xyz;
  }
  { // 63
    precise float4 v = Mul(r0.xyzz, r3.wwww) + r3.xyzz;
    r0.xyz = v.xyz;
  }
  { // 64
    precise float4 v = max(r0, r0);
    precise float s = sqrt((abs(r0)).x);
    oC0.y = 0.0;
    oC0.z = 0.0;
    oC0.w = 1.0;
    oC0.x = s;
    ps = s;
  }
  { // 65
    precise float4 v = sqrt((abs(r0)).y);
    oC0.y = v.y;
  }
  { // 66
    precise float4 v = sqrt((abs(r0)).z);
    oC0.z = v.z;
  }
  return oC0 * asfloat(xe_system[15].y);
}
