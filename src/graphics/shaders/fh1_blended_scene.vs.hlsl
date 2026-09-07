// FH1 blended scene vertex program 8D8A197476841A9A.
// Direct straight-line form; guest bindings remain during stage migration.
ByteAddressBuffer shared_srv : register(t0);
#ifndef FH1_SCENE_OWNED_GEOMETRY
RWByteAddressBuffer shared_uav : register(u0);
#endif
cbuffer system_constants : register(b0) { uint4 system_data[30]; };
cbuffer float_constants : register(b1) { float4 c[34]; };
cbuffer fetch_constants : register(b3) { uint4 fetch_data[48]; };
float4 Mul(float4 a, float4 b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float MulS(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float Dot3(float4 a, float4 b) { precise float r = MulS(a.x,b.x); r += MulS(a.y,b.y); r += MulS(a.z,b.z); return r; }
float Dot4(float4 a, float4 b) { precise float r = Dot3(a,b); r += MulS(a.w,b.w); return r; }
uint Endian(uint v, uint mode) {
  if (mode == 1 || mode == 2) v = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
  if (mode == 2 || mode == 3) v = (v << 16) | (v >> 16);
  return v;
}
#include "fh1_scene_geometry.hlsli"

struct Vertex { float4 uv_alpha:TEXCOORD0; float4 blend:TEXCOORD1; float4 color:TEXCOORD2; float4 position:SV_Position; };
Vertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == system_data[0].w ? 0 : vertex_id;
  index = Endian(index, system_data[1].x);
  index = clamp((index + system_data[1].y) & 0xFFFFFF, system_data[1].z, system_data[1].w);
  uint address = (fetch_data[47].z & ~3u) + index * 16;
  uint4 data = LoadGeometry4(address);
  uint endian = fetch_data[47].w & 3;
  data = uint4(Endian(data.x,endian),Endian(data.y,endian),Endian(data.z,endian),Endian(data.w,endian));
  precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0,r9=0;
  precise float4 o0=0,o1=0,o2=0,oPos=0;
  precise float ps=0;
  r0.yzw = asfloat(data.xyz);
  r5.yz = float2(data.w >> 16, data.w & 65535) * (1.0 / 65535.0);
  { // 9
    precise float4 v = Mul(r0.yzww, c[8].xyzz);
    r0.xyz = v.xyz;
  }
  { // 10
    precise float4 v = Mul(r0.xyzz, c[1].xyzz) + c[2].xyzz;
    r0.xyz = v.xyz;
  }
  { // 11
    precise float4 v = r0.xyzz + -r0.xyzz;
    r1.xyz = v.xyz;
  }
  { // 12
    precise float4 v = Mul(r1.xyzz, c[20].wwww) + r0.xyzz;
    r1.xyz = v.xyz;
  }
  { // 13
    precise float4 v = (c[32].xxxy == 0.0) ? r1.zxyy : c[32].yyyy;
    r7.xyzw = v.xyzw;
  }
  { // 14
    precise float4 v = Dot4(c[9].zxyw, r7);
    oPos.x = v.x;
  }
  { // 15
    precise float4 v = Dot4(c[10].zxyw, r7);
    oPos.y = v.y;
  }
  { // 16
    precise float4 v = Dot4(c[11].zxyw, r7);
    oPos.z = v.z;
  }
  { // 17
    precise float4 v = Dot4(c[12].zxyw, r7);
    oPos.w = v.w;
  }
  { // 18
    precise float4 v = -c[6].wwww + c[32].yyyy;
    r3.x = v.x;
  }
  { // 19
    precise float4 v = c[19].xzyy + c[19].xzyy;
    r2.xyz = v.xyz;
  }
  { // 20
    precise float4 v = c[23].xyzz + -c[26].xyzz;
    r8.xyz = v.xyz;
  }
  { // 21
    precise float4 v = Dot3(c[30].zxyy, c[13].zxyy);
    r6.x = v.x;
  }
  { // 22
    precise float4 v = Dot3(c[30].zxyy, c[15].zxyy);
    r6.y = v.y;
  }
  { // 23
    precise float4 v = saturate(Dot3(c[21].zxyy, c[30].zxyy));
    precise float s = (c[20].yx).x - (c[20].yx).y;
    r0.x = v.x;
    r0.z = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(c[31].xzyy, c[4].xxxx);
    precise float s = log2(r0.x);
    r4.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(r0.xxxx, c[21].wwww);
    precise float s = (c[22].yx).x - (c[22].yx).y;
    r0.x = v.x;
    r2.w = s;
    ps = s;
  }
  { // 26
    precise float4 v = Mul(c[24].xxzy, c[3].xxxx);
    precise float s = exp2(r0.x);
    r3.yzw = v.yzw;
    r0.x = s;
    ps = s;
  }
  { // 27
    precise float4 v = Mul(r4.xxzy, r0.xxxx) + r3.yywz;
    r4.yzw = v.yzw;
  }
  { // 28
    precise float4 v = (MulS((r1.zxxx).x, (r1.zxxx).x) + MulS((r1.zxxx).y, (r1.zxxx).y)) + (c[32].xxxx).x;
    r5.w = v.w;
  }
  { // 29
    precise float4 v = Dot4(c[18].zxyw, r7);
    precise float s = -c[5].x - (-r1.y);
    r5.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 30
    precise float4 v = Dot4(c[17].zxyw, r7);
    precise float s = rcp(c[7].y);
    r0.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 31
    precise float4 v = Mul(r5.yzzz, c[0].zwww) + c[0].xyyy;
    o0.xy = v.xy;
  }
  { // 32
    precise float4 v = Dot4(c[16].zxyw, r7);
    precise float s = -c[28].w - (-r0.y);
    r0.x = v.x;
    r9.y = s;
    ps = s;
  }
  { // 33
    precise float4 v = Dot3(r1.zxww, r1.zxww);
    precise float s = -c[27].w - (-r0.x);
    r4.x = v.x;
    r9.x = s;
    ps = s;
  }
  { // 34
    precise float4 v = r5.xxxx + -c[29].wwww;
    precise float s = rcp(c[7].x);
    r9.z = v.z;
    r7.x = s;
    ps = s;
  }
  { // 35
    precise float4 v = Dot3(r9.zxyy, r9.zxyy);
    precise float s = rsqrt((abs(r4)).x);
    r0.x = v.x;
    r7.y = s;
    ps = s;
  }
  { // 36
    precise float4 v = -r3.xxxx + c[32].yyyy;
    precise float s = rsqrt((abs(r0)).x);
    r4.x = v.x;
    r5.z = s;
    ps = s;
  }
  { // 37
    precise float4 v = r0.yyyy + -c[22].xxxx;
    precise float s = sqrt((abs(r0)).x);
    r5.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 38
    precise float4 v = Dot3(c[30].zxyy, c[14].zxyy);
    precise float s = -c[20].x - (-r0.y);
    r6.z = v.z;
    r5.y = s;
    ps = s;
  }
  { // 39
    precise float4 v = Mul(r1.wxzw, r7.xyyy);
    precise float s = -c[25].x - (-r0.y);
    r7.xyzw = v.xyzw;
    r0.x = s;
    ps = s;
  }
  { // 40
    precise float4 v = Mul(r7.xxxx, r7.xxxx) + r5.wwww;
    r1.x = v.x;
  }
  { // 41
    precise float4 v = r4.yyzw + -r3.yywz;
    precise float s = -c[26].w - (-r0.y);
    r4.yzw = v.yzw;
    r1.w = s;
    ps = s;
  }
  { // 42
    precise float4 v = Mul(r9.xyzz, r5.zzzz);
    precise float s = rcp(c[23].w);
    r9.xyz = v.xyz;
    r1.z = s;
    ps = s;
  }
  { // 43
    precise float4 v = saturate(Dot3(r9.zxyy, c[30].zxyy));
    precise float s = rcp(r2.w);
    r1.y = v.y;
    r0.y = s;
    ps = s;
  }
  { // 44
    precise float4 v = saturate(Mul(r1.wwww, r1.zzzz));
    precise float s = rcp(r0.z);
    r6.w = v.w;
    r0.z = s;
    ps = s;
  }
  { // 45
    precise float4 v = max(r0.xxxx, c[32].xxxx);
    precise float s = sqrt((abs(r1)).x);
    r0.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 46
    precise float4 v = saturate(Mul(r5.yyxx, r0.zzyy));
    precise float s = MulS(c[25].w, r0.x);
    r0.yz = v.yz;
    r0.x = s;
    ps = s;
  }
  { // 47
    precise float4 v = Mul(c[20].z, r0.y);
    o0.z = v.z;
  }
  { // 48
    precise float4 v = Dot3(r6.yxzz, r6.yxzz);
    precise float s = log2(r1.y);
    r1.x = v.x;
    r1.z = s;
    ps = s;
  }
  { // 49
    precise float4 v = Mul(r0.zzzz, r0.zzzz);
    precise float s = exp2(r0.x);
    r1.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 50
    precise float4 v = Mul(r1.zzyy, c[22].wwzz);
    precise float s = saturate(rcp(r0.x));
    r1.yz = v.yz;
    r0.x = s;
    ps = s;
  }
  { // 51
    precise float4 v = -r0.xzzz + c[32].yzzz;
    precise float s = rsqrt((abs(r1)).x);
    r5.xw = v.xw;
    r1.x = s;
    ps = s;
  }
  { // 52
    precise float4 v = r5.wwww + -r0.zzzz;
    precise float s = exp2(r1.y);
    r0.x = v.x;
    r1.y = s;
    ps = s;
  }
  { // 53
    precise float4 v = Mul(-r1.zzzz, r0.xxxx) + c[32].yyyy;
    r5.z = v.z;
  }
  { // 54
    precise float4 v = Mul(r6, r1.xxxy);
    r6.xyzw = v.xyzw;
  }
  { // 55
    precise float4 v = Mul(r6.wwww, r8.xyzz) + c[26].xyzz;
    r1.xyz = v.xyz;
  }
  { // 56
    precise float4 v = Dot3(r7.zyww, r6.yxzz);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r0.x = v.x;
    ps = s;
  }
  { // 57
    precise float4 v = saturate(Mul(r0.xxxx, c[32].wwww) + c[32].wwww);
    r5.y = v.y;
  }
  { // 58
    precise float4 v = Mul(-r5.yyyy, c[33].xxxx) + c[32].zzzz;
    r0.w = v.w;
  }
  { // 59
    precise float4 v = Mul(r5.xyyy, r5.zyyy);
    precise float s = saturate(MulS(ps, r1.w));
    r0.xz = v.xz;
    r0.y = s;
    ps = s;
  }
  { // 60
    precise float4 v = Mul(r0.xxxx, r1.xyzz);
    precise float s = c[32].y - (r0.x);
    o2.xyz = v.xyz;
    o2.w = s;
    ps = s;
  }
  { // 61
    precise float4 v = Mul(r0.yzzz, r0.ywww);
    r0.xy = v.xy;
  }
  { // 62
    precise float4 v = Mul(r0.xyyy, r4) + r3.xywz;
    r0.xyzw = v.xyzw;
  }
  { // 63
    precise float4 v = Mul(r0.ywzz, r2.xyzz);
    r1.xyz = v.xyz;
  }
  { // 64
    precise float4 v = Mul(r1.xzyy, r0.xxxx);
    precise float s = max((r0.xx).x, (r0.xx).y);
    o1.xyz = v.xyz;
    o1.w = s;
    ps = s;
  }
  if (!(system_data[0].x & 8)) oPos.w = 1.0 / oPos.w;
  if (system_data[0].x & 2) oPos.xy *= oPos.w;
  if (system_data[0].x & 4) oPos.z *= oPos.w;
  oPos.xyz *= asfloat(system_data[8].xyz);
  oPos.xyz = mad(asfloat(system_data[9].xyz), oPos.w, oPos.xyz);
  Vertex output; output.uv_alpha=o0; output.blend=o1; output.color=o2; output.position=oPos; return output;
}
