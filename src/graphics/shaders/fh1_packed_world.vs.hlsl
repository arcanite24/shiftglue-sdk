// FH1 packed world vertex program 6934E161812AB10B.
// Direct straight-line form; guest bindings remain during stage migration.
ByteAddressBuffer shared_srv : register(t0);
#ifndef FH1_SCENE_OWNED_GEOMETRY
RWByteAddressBuffer shared_uav : register(u0);
#endif
cbuffer system_constants : register(b0) { uint4 system_data[30]; };
cbuffer float_constants : register(b1) { float4 c[20]; };
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
struct Vertex { float4 normal:TEXCOORD0; float4 eye:TEXCOORD1; float4 uv:TEXCOORD2; float4 fog:TEXCOORD3; float4 color:TEXCOORD4; float4 clip:TEXCOORD5; float4 world:TEXCOORD6; float4 position:SV_Position; };
uint LoadWord(uint address) {
  uint word;
#ifdef FH1_SCENE_OWNED_GEOMETRY
  word=LoadGeometryWord(address);
#else
  if (system_data[0].x & 1) word=shared_uav.Load(address); else word=shared_srv.Load(address);
#endif
  return Endian(word,fetch_data[47].w & 3);
}
Vertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == system_data[0].w ? 0 : vertex_id;
  index = Endian(index, system_data[1].x);
  index = clamp((index + system_data[1].y) & 0xFFFFFF, system_data[1].z, system_data[1].w);
  uint address = (fetch_data[47].z & ~3u) + index * 28;
#ifdef FH1_SCENE_OWNED_GEOMETRY
  uint4 packed=LoadGeometry4(address);
  uint3 position=packed.xyz;
#else
  uint3 position;
  if (system_data[0].x & 1) position=shared_uav.Load3(address); else position=shared_srv.Load3(address);
#endif
  uint endian = fetch_data[47].w & 3;
  position=uint3(Endian(position.x,endian),Endian(position.y,endian),Endian(position.z,endian));
  precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0;
  precise float4 o0=0,o1=0,o2=0,o3=0,o4=0,o5=0,o6=0,oPos=0;
  precise float ps=0;
  r0.x=float(index);r0.yzw=asfloat(position);
  uint word=LoadWord(address+12);
  r1.xyz=max(float3(int(word << 22) >> 22,int(word << 12) >> 22,int(word << 2) >> 22)*(1.0/511.0),-1.0);
  word=LoadWord(address+16);r3.xy=float2(word >> 16,word & 65535u)*(1.0/65535.0);
  word=LoadWord(address+20);r3.zw=float2(word >> 16,word & 65535u)*(1.0/65535.0);
  word=LoadWord(address+24);r5=float4((word >> 16) & 255u,(word >> 8) & 255u,word & 255u,word >> 24)*(1.0/255.0);
  { // 11
    precise float4 v = Mul(r0.yzww, c[3].xyzz);
    precise float s = ((abs(r0)).x >= 0.0) ? 1.0 : 0.0;
    r0.xyz = v.xyz;
    r6.w = s;
    ps = s;
  }
  { // 12
    precise float4 v = Mul(r0.xyzz, c[1].xyzz) + c[2].xyzz;
    r6.xyz = v.xyz;
  }
  { // 13
    precise float4 v = Dot4(c[7].zxyw, r6.zxyw);
    r4.w = v.w;
  }
  { // 14
    precise float4 v = Dot4(c[6].zxyw, r6.zxyw);
    r4.z = v.z;
  }
  { // 15
    precise float4 v = Dot4(c[5].zxyw, r6.zxyw);
    r4.y = v.y;
  }
  { // 16
    precise float4 v = Dot4(c[4].zxyw, r6.zxyw);
    r4.x = v.x;
  }
  { // 17
    precise float4 v = max(r4, r4);
    oPos.xyzw = v.xyzw;
  }
  { // 18
    precise float4 v = Dot4(c[9].zxyw, r6.zxyw);
    precise float s = (c[11].yx).x - (c[11].yx).y;
    r0.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 19
    precise float4 v = Dot4(c[8].zxyw, r6.zxyw);
    precise float s = -c[16].w - (-r0.y);
    r0.x = v.x;
    r2.z = s;
    ps = s;
  }
  { // 20
    precise float4 v = Dot4(c[10].zxyw, r6.zxyw);
    precise float s = -c[15].w - (-r0.x);
    r0.z = v.z;
    r2.y = s;
    ps = s;
  }
  { // 21
    precise float4 v = r0.yyyy + -c[11].xxxx;
    precise float s = rcp(r0.w);
    r1.w = v.w;
    r0.w = s;
    ps = s;
  }
  { // 22
    precise float4 v = saturate(Mul(r1.wwww, r0.wwww));
    precise float s = -c[17].w - (-r0.z);
    r0.w = v.w;
    r2.w = s;
    ps = s;
  }
  { // 23
    precise float4 v = max(c[0], c[0]);
  }
  { // 24
    precise float4 v = max(c[0], c[0]);
  }
  { // 25
    precise float4 v = max(c[0], c[0]);
  }
  { // 26
    precise float4 v = max(c[0], c[0]);
  }
  { // 27
    precise float4 v = c[15].w - (r0.x);
    o1.x = v.x;
  }
  { // 28
    precise float4 v = -r0.zzzz + c[17].wwww;
    precise float s = c[16].w - (r0.y);
    o1.z = v.z;
    o1.y = s;
    ps = s;
  }
  { // 29
    precise float4 v = max(r0.xyzz, r0.xyzz);
    o6.xyz = v.xyz;
    o6.w = 1.0;
  }
  { // 30
    precise float4 v = max(r5, r5);
    o4.xyzw = v.xyzw;
  }
  { // 31
    precise float4 v = max(r4, r4);
    o5.xyzw = v.xyzw;
  }
  { // 32
    precise float4 v = Mul(r3, c[0].zwzw) + c[0].xyxy;
    o2.xyzw = v.xyzw;
  }
  { // 33
    precise float4 v = Dot3(r1.zxyy, c[8].zxyy);
    o0.x = v.x;
  }
  { // 34
    precise float4 v = Dot3(r1.zxyy, c[9].zxyy);
    o0.y = v.y;
  }
  { // 35
    precise float4 v = Dot3(r1.zxyy, c[10].zxyy);
    o0.z = v.z;
  }
  { // 36
    precise float4 v = Mul(-r0.wwww, c[19].xxxx) + c[19].wwww;
    r1.w = v.w;
  }
  { // 37
    precise float4 v = Dot3(r2.wyzz, r2.wyzz);
    r0.x = v.x;
  }
  { // 38
    precise float4 v = Mul(r0.wwww, r0.wwww);
    precise float s = sqrt((abs(r0)).x);
    r2.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 39
    precise float4 v = r0.yyyy + -c[14].wwww;
    precise float s = rsqrt((abs(r0)).x);
    r0.w = v.w;
    r0.x = s;
    ps = s;
  }
  { // 40
    precise float4 v = c[12].xyzz + -c[14].xyzz;
    precise float s = -c[13].x - (-r0.y);
    r1.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 41
    precise float4 v = Mul(r2.yyzw, r0.xxxx);
    precise float s = rcp(c[12].w);
    r2.yzw = v.yzw;
    r0.x = s;
    ps = s;
  }
  { // 42
    precise float4 v = max(r0.yyyy, c[19].yyyy);
    precise float s = max((r0.ww).x, (r0.ww).y);
    r0.z = v.z;
    ps = s;
  }
  { // 43
    precise float4 v = saturate(Dot3(r2.wyzz, c[18].zxyy));
    precise float s = saturate(MulS(ps, r0.x));
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 44
    precise float4 v = Mul(r0.zzzz, c[13].wwww);
    precise float s = log2(r0.y);
    r0.z = v.z;
    r2.y = s;
    ps = s;
  }
  { // 45
    precise float4 v = Mul(r2.xxyy, c[11].zzww);
    precise float s = exp2(r0.z);
    r0.yw = v.yw;
    r0.z = s;
    ps = s;
  }
  { // 46
    precise float4 v = Mul(r0.yyyy, r1.wwww);
    precise float s = saturate(rcp(r0.z));
    r2.x = v.x;
    r2.y = s;
    ps = s;
  }
  { // 47
    precise float4 v = -r2.xxyy + c[19].zzzz;
    precise float s = exp2(r0.w);
    r0.yz = v.yz;
    r0.w = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r0.zxxx, r0.ywww);
    r0.xy = v.xy;
  }
  { // 49
    precise float4 v = Mul(r0.yyyy, r1.xyzz) + c[14].xyzz;
    r1.xyz = v.xyz;
  }
  { // 50
    precise float4 v = Mul(r0.xxxx, r1.xyzz);
    precise float s = c[19].z - (r0.x);
    o3.xyz = v.xyz;
    o3.w = s;
    ps = s;
  }
  if (!(system_data[0].x & 8)) oPos.w = 1.0 / oPos.w;
  if (system_data[0].x & 2) oPos.xy *= oPos.w;
  if (system_data[0].x & 4) oPos.z *= oPos.w;
  oPos.xyz *= asfloat(system_data[8].xyz);
  oPos.xyz = mad(asfloat(system_data[9].xyz), oPos.w, oPos.xyz);
  Vertex output; output.normal=o0; output.eye=o1; output.uv=o2; output.fog=o3; output.color=o4; output.clip=o5; output.world=o6; output.position=oPos; return output;
}
