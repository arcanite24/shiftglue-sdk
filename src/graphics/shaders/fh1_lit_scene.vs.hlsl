// FH1 lit scene vertex program AD2C355A6BE1EE87.
// Direct straight-line form; guest bindings remain during stage migration.
ByteAddressBuffer shared_srv : register(t0);
#ifndef FH1_SCENE_OWNED_GEOMETRY
RWByteAddressBuffer shared_uav : register(u0);
#endif
cbuffer system_constants : register(b0) { uint4 system_data[30]; };
cbuffer float_constants : register(b1) { float4 c[23]; };
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

struct Vertex { float4 uv_alpha:TEXCOORD0; float4 blend:TEXCOORD1; float4 color:TEXCOORD2; float4 material:TEXCOORD3; float4 world:TEXCOORD4; float4 position:SV_Position; };
Vertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == system_data[0].w ? 0 : vertex_id;
  index = Endian(index, system_data[1].x);
  index = clamp((index + system_data[1].y) & 0xFFFFFF, system_data[1].z, system_data[1].w);
  uint address = (fetch_data[47].z & ~3u) + index * 20;
  uint4 data = LoadGeometry4(address);
  uint endian = fetch_data[47].w & 3;
  data = uint4(Endian(data.x,endian),Endian(data.y,endian),Endian(data.z,endian),Endian(data.w,endian));
  precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0,r9=0;
  precise float4 o0=0,o1=0,o2=0,o3=0,o4=0,oPos=0;
  precise float ps=0;
  r0.yzw = asfloat(data.xyz);
#ifdef FH1_SCENE_OWNED_GEOMETRY
  uint uv = LoadGeometryWord(address + 16);
#else
  uint uv;
  if (system_data[0].x & 1) uv = shared_uav.Load(address + 16); else uv = shared_srv.Load(address + 16);
#endif
  uv = Endian(uv, endian);
  r1.yz = float2(uv >> 16, uv & 65535) * (1.0 / 65535.0);
  r3.xyz = max(float3(int(data.w << 22) >> 22, int(data.w << 12) >> 22, int(data.w << 2) >> 22) * (1.0 / 511.0), -1.0);
  { // 9
    precise float4 v = Mul(r0.yzww, c[5].xyzz);
    r0.xyz = v.xyz;
  }
  { // 10
    precise float4 v = Mul(r0.xyzz, c[1].xyzz) + c[2].xyzz;
    r0.xyz = v.xyz;
  }
  { // 11
    precise float4 v = r0.xyzz + -r0.xyzz;
    r2.xyz = v.xyz;
  }
  { // 12
    precise float4 v = Mul(r2.xzzy, c[13].wwww) + r0.xzzy;
    r2.xzw = v.xzw;
  }
  { // 13
    precise float4 v = (c[22].xxxy == 0.0) ? r2.zxww : c[22].yyyy;
    r4.xyzw = v.xyzw;
  }
  { // 14
    precise float4 v = Dot4(c[6].zxyw, r4);
    oPos.x = v.x;
  }
  { // 15
    precise float4 v = Dot4(c[7].zxyw, r4);
    oPos.y = v.y;
  }
  { // 16
    precise float4 v = Dot4(c[8].zxyw, r4);
    oPos.z = v.z;
  }
  { // 17
    precise float4 v = Dot4(c[9].zxyw, r4);
    oPos.w = v.w;
  }
  { // 18
    precise float4 v = r2.wwww + -c[3].wwww;
    precise float s = rcp(c[4].y);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 19
    precise float4 v = Mul(r0.yyyy, r0.xxxx);
    r1.x = v.x;
  }
  { // 20
    precise float4 v = Dot4(c[12].zxyw, r4);
    r0.w = v.w;
  }
  { // 21
    precise float4 v = Dot4(c[11].zxyw, r4);
    precise float s = (c[14].yx).x - (c[14].yx).y;
    r0.z = v.z;
    r0.y = s;
    ps = s;
  }
  { // 22
    precise float4 v = Dot4(c[10].zxyw, r4);
    precise float s = rcp(c[3].z);
    r0.x = v.x;
    r2.y = s;
    ps = s;
  }
  { // 23
    precise float4 v = max(r0, r0);
    precise float s = c[18].w - (r0.x);
    o1.y = 0.0;
    o1.z = 0.0;
    o1.w = 0.0;
    o1.x = s;
    ps = s;
  }
  { // 24
    precise float4 v = -r0.wwww + c[20].wwww;
    precise float s = c[19].w - (r0.z);
    o1.z = v.z;
    o1.y = s;
    ps = s;
  }
  { // 25
    precise float4 v = Dot3(r3.zxyy, c[10].zxyy);
    o0.x = v.x;
  }
  { // 26
    precise float4 v = Dot3(r3.zxyy, c[11].zxyy);
    o0.y = v.y;
  }
  { // 27
    precise float4 v = Dot3(r3.zxyy, c[12].zxyy);
    o0.z = v.z;
  }
  { // 28
    precise float4 v = Mul(r1.yzzz, c[0].zwww) + c[0].xyyy;
    o3.xy = v.xy;
  }
  { // 29
    precise float4 v = r0.zzzz + -c[14].xxxx;
    precise float s = rcp(r0.y);
    r1.y = v.y;
    r0.y = s;
    ps = s;
  }
  { // 30
    precise float4 v = saturate(Mul(r1.yyyy, r0.yyyy));
    r1.y = v.y;
  }
  { // 31
    precise float4 v = Mul(-r1.yyyy, c[22].zzzz) + c[22].wwww;
    r1.w = v.w;
  }
  { // 32
    precise float4 v = Mul(r1.yxxx, r1.yxxx);
    precise float s = rcp(c[4].z);
    r1.xz = v.xz;
    r1.y = s;
    ps = s;
  }
  { // 33
    precise float4 v = (MulS((r2.zxxx).x, (r2.zxxx).x) + MulS((r2.zxxx).y, (r2.zxxx).y)) + (r1.zzzz).x;
    r0.y = v.y;
  }
  { // 34
    precise float4 v = r0.wwww + -c[20].wwww;
    precise float s = sqrt((abs(r0)).y);
    r3.z = v.z;
    r0.y = s;
    ps = s;
  }
  { // 35
    precise float4 v = saturate(Mul(r1.yyyy, r0.yyyy));
    precise float s = -c[18].w - (-r0.x);
    r0.y = v.y;
    r3.x = s;
    ps = s;
  }
  { // 36
    precise float4 v = Mul(r0.yyyy, r0.yyyy);
    precise float s = -c[19].w - (-r0.z);
    r3.w = v.w;
    r3.y = s;
    ps = s;
  }
  { // 37
    precise float4 v = Dot3(r3.zxyy, r3.zxyy);
    precise float s = (c[13].yx).x - (c[13].yx).y;
    r1.y = v.y;
    r4.x = s;
    ps = s;
  }
  { // 38
    precise float4 v = -c[4].xxxx + c[22].yyyy;
    precise float s = sqrt((abs(r1)).y);
    r4.y = v.y;
    r0.y = s;
    ps = s;
  }
  { // 39
    precise float4 v = r0.yyyy + -c[13].xxxx;
    precise float s = rcp(r4.x);
    r4.z = v.z;
    r4.x = s;
    ps = s;
  }
  { // 40
    precise float4 v = saturate(Mul(r4.zzzz, r4.xxxx));
    precise float s = -c[16].x - (-r0.y);
    r2.z = v.z;
    r4.x = s;
    ps = s;
  }
  { // 41
    precise float4 v = max(r4.xxxx, c[22].xxxx);
    precise float s = max((r2.ww).x, (r2.ww).y);
    r2.x = v.x;
    ps = s;
  }
  { // 42
    precise float4 v = Mul(r2.zzzz, c[13].zzzz);
    precise float s = saturate(MulS(ps, r2.y));
    o3.z = v.z;
    o3.w = s;
    ps = s;
  }
  { // 43
    precise float4 v = Mul(r2.xxxx, c[16].wwww);
    precise float s = rcp(c[15].w);
    r2.z = v.z;
    r2.x = s;
    ps = s;
  }
  { // 44
    precise float4 v = r0.yyyy + -c[17].wwww;
    precise float s = exp2(r2.z);
    r2.y = v.y;
    r0.y = s;
    ps = s;
  }
  { // 45
    precise float4 v = saturate(Mul(r2.yyyy, r2.xxxx));
    precise float s = saturate(rcp(r0.y));
    r2.w = v.w;
    r4.x = s;
    ps = s;
  }
  { // 46
    precise float4 v = -r4.yxxx + c[22].yyyy;
    precise float s = rsqrt((abs(r1)).y);
    r2.xz = v.xz;
    r2.y = s;
    ps = s;
  }
  { // 47
    precise float4 v = Mul(r3.wxyz, r2.xyyy);
    r3.xyzw = v.xyzw;
  }
  { // 48
    precise float4 v = saturate(Dot3(r3.wyzz, c[21].zxyy));
    r1.y = v.y;
  }
  { // 49
    precise float4 v = r4.yyyy + r3.xxxx;
    precise float s = log2(r1.y);
    r0.y = v.y;
    r1.y = s;
    ps = s;
  }
  { // 50
    precise float4 v = max(r0.xzwy, r0.xzwy);
    o4.xyzw = v.xyzw;
  }
  { // 51
    precise float4 v = Mul(r1.xyyy, c[14].zwww);
    r0.xy = v.xy;
  }
  { // 52
    precise float4 v = c[15].xyzz + -c[17].xyzz;
    precise float s = exp2(r0.y);
    r1.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 53
    precise float4 v = Mul(-r0.xxxx, r1.wwww) + c[22].yyyy;
    r0.x = v.x;
  }
  { // 54
    precise float4 v = Mul(r2.zwww, r0.xyyy);
    r0.xy = v.xy;
  }
  { // 55
    precise float4 v = Mul(r0.yyyy, r1.xyzz) + c[17].xyzz;
    r1.xyz = v.xyz;
  }
  { // 56
    precise float4 v = Mul(r0.xxxx, r1.xyzz);
    precise float s = c[22].y - (r0.x);
    o2.xyz = v.xyz;
    o2.w = s;
    ps = s;
  }
  if (!(system_data[0].x & 8)) oPos.w = 1.0 / oPos.w;
  if (system_data[0].x & 2) oPos.xy *= oPos.w;
  if (system_data[0].x & 4) oPos.z *= oPos.w;
  oPos.xyz *= asfloat(system_data[8].xyz);
  oPos.xyz = mad(asfloat(system_data[9].xyz), oPos.w, oPos.xyz);
  Vertex output; output.uv_alpha=o0; output.blend=o1; output.color=o2; output.material=o3; output.world=o4; output.position=oPos; return output;
}
