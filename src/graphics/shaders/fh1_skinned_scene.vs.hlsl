// FH1 skinned scene vertex program B8489164D5A86043.
// Direct form qualified against 712 production 2x draws. Guest bindings
// remain until both the primary mesh and packed transform stream are owned.
ByteAddressBuffer shared_srv : register(t0);
#ifdef FH1_SKINNED_OWNED_GEOMETRY
ByteAddressBuffer transform_srv : register(t1);
#else
RWByteAddressBuffer shared_uav : register(u0);
#endif
cbuffer system_constants : register(b0) { uint4 system_data[30]; };
cbuffer float_constants : register(b1) { float4 c[22]; };
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
uint LoadWord(uint address) {
  uint value;
#ifdef FH1_SKINNED_OWNED_GEOMETRY
  value=0;
  [branch] if (address < 0x20000000u) value=shared_srv.Load(address);
#else
  if (system_data[0].x & 1) value=shared_uav.Load(address); else value=shared_srv.Load(address);
#endif
  return value;
}
float3 LoadTransform(float index, uint offset) {
  uint address=(fetch_data[47].x & ~3u) + uint(int(floor(index))) * 12 + offset;
#ifdef FH1_SKINNED_OWNED_GEOMETRY
  uint raw=0;
  [branch] if (address < 0x20000000u) raw=transform_srv.Load(address);
#else
  uint raw=LoadWord(address);
#endif
  uint word=Endian(raw,fetch_data[47].y & 3);
  return max(float3(int(word << 21) >> 21,int(word << 10) >> 21,int(word) >> 22) * float3(1.0/1023.0,1.0/1023.0,1.0/511.0),-1.0);
}
struct Vertex { float4 uv_alpha:TEXCOORD0; float4 blend:TEXCOORD1; float4 color:TEXCOORD2; float4 material:TEXCOORD3; float4 world:TEXCOORD4; float4 position:SV_Position; };
Vertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == system_data[0].w ? 0 : vertex_id;
  index = Endian(index, system_data[1].x);
  index = clamp((index + system_data[1].y) & 0xFFFFFF, system_data[1].z, system_data[1].w);
  uint address = (fetch_data[47].z & ~3u) + index * 32;
  uint endian=fetch_data[47].w & 3;
  precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0,r9=0,r10=0,r11=0,r12=0,r13=0;
  precise float4 o0=0,o1=0,o2=0,o3=0,o4=0,oPos=0;
  precise float ps=0;
  r6.xyw=asfloat(uint3(Endian(LoadWord(address),endian),Endian(LoadWord(address+4),endian),Endian(LoadWord(address+8),endian)));
  float3 normal=asfloat(uint3(Endian(LoadWord(address+12),endian),Endian(LoadWord(address+16),endian),Endian(LoadWord(address+20),endian)));
  r2.xyw=normal.yzx;
  uint uv=Endian(LoadWord(address+24),endian);
  r5.xy=float2(uv >> 16,uv & 65535) * (1.0/65535.0);
  uint weights=Endian(LoadWord(address+28),endian);
  r0.xyz=float3(weights & 255,(weights >> 16) & 255,weights >> 24);
  r1.xy=r0.zy+c[9].xx;
  r1.z=c[12].y-c[12].x;
  ps=r1.z;
  r9.xyz=LoadTransform(r1.x,0); r10.xyz=LoadTransform(r1.x,4); r12.xyz=LoadTransform(r1.x,8);
  r3.xyz=LoadTransform(r1.y,0); r8.xyz=LoadTransform(r1.y,4); r13.xyz=LoadTransform(r1.y,8);
  { // 19
    precise float4 v = Mul(r9.xzyy, r6.xxxx);
    r4.xyz = v.xyz;
  }
  { // 20
    precise float4 v = Mul(r3.zxyy, r6.xxxx);
    r11.xyz = v.xyz;
  }
  { // 21
    precise float4 v = Mul(r3.zxyy, r8.yzxx);
    r7.xyz = v.xyz;
  }
  { // 22
    precise float4 v = Mul(r9.zxyy, r10.yzxx);
    r1.xyw = v.xyw;
  }
  { // 23
    precise float4 v = Mul(r9.yxzz, r10.zyxx) + -r1.xwyy;
    r1.xyw = v.xyw;
  }
  { // 24
    precise float4 v = Mul(r3.yxzz, r8.zyxx) + -r7.xzyy;
    r7.xyz = v.xyz;
  }
  { // 25
    precise float4 v = Mul(r13.xzzz, c[20].xxxx) + r11.yxxx;
    r11.xy = v.xy;
  }
  { // 26
    precise float4 v = Mul(r13.yyyy, c[20].yyyy) + r11.zzzz;
    r11.z = v.z;
  }
  { // 27
    precise float4 v = Mul(r12.xzzz, c[20].xxxx) + r4.xyyy;
    r4.xy = v.xy;
  }
  { // 28
    precise float4 v = Mul(r12.yyyy, c[20].yyyy) + r4.zzzz;
    r4.z = v.z;
  }
  { // 29
    precise float4 v = Mul(r10.xzyy, r6.yyyy) + r4.xyzz;
    r4.xyz = v.xyz;
  }
  { // 30
    precise float4 v = Mul(r8.xzyy, r6.yyyy) + r11.xyzz;
    r11.xyz = v.xyz;
  }
  { // 31
    precise float4 v = Mul(r7.xyzz, r6.wwww) + r11.xyzz;
    r6.xyz = v.xyz;
  }
  { // 32
    precise float4 v = Mul(r1.xyww, r6.wwww) + r4.xyzz;
    r4.xyz = v.xyz;
  }
  { // 33
    precise float4 v = r6.xyzz + -r4.xyzz;
    precise float s = MulS(c[21].w, r0.x);
    r6.xyz = v.xyz;
    r5.w = s;
    ps = s;
  }
  { // 34
    precise float4 v = Mul(r5.wwww, r6.yzxx) + r4.yzxx;
    r0.xyw = v.xyw;
  }
  { // 35
    precise float4 v = Mul(r0.xxxx, c[7].xyzz) + -c[11].xyzz;
    r4.xyz = v.xyz;
  }
  { // 36
    precise float4 v = Mul(r0.yyyy, c[6].xyzz) + r4.xyzz;
    r0.xyz = v.xyz;
  }
  { // 37
    precise float4 v = Mul(r0.wwww, c[5].xzyy) + r0.xzyy;
    r0.xyz = v.xyz;
  }
  { // 38
    precise float4 v = r0.xzzy + c[8].xyyz;
    precise float s = ((abs(r0)).x >= 0.0 ? 1.0 : 0.0);
    r0.xzw = v.xzw;
    r0.y = s;
    ps = s;
  }
  { // 39
    precise float4 v = Dot4(c[4].zxyw, r0.wxzy);
    r6.w = v.w;
  }
  { // 40
    precise float4 v = Dot4(c[3].zxyw, r0.wxzy);
    r6.z = v.z;
  }
  { // 41
    precise float4 v = Dot4(c[2].zxyw, r0.wxzy);
    r6.y = v.y;
  }
  { // 42
    precise float4 v = Dot4(c[1].zxyw, r0.wxzy);
    r6.x = v.x;
  }
  { // 43
    precise float4 v = max(r6, r6);
    oPos.xyzw = v.xyzw;
  }
  { // 44
    precise float4 v = Mul(r1.xwyy, r2.yyyy);
    precise float s = (c[10].yx).x - (c[10].yx).y;
    r4.xyz = v.xyz;
    r1.w = s;
    ps = s;
  }
  { // 45
    precise float4 v = Mul(r10.xyzz, r2.xxxx) + r4.xyzz;
    r4.xyz = v.xyz;
  }
  { // 46
    precise float4 v = Mul(r9.xzyy, r2.wwww) + r4.xzyy;
    r4.xyz = v.xyz;
  }
  { // 47
    precise float4 v = Mul(r7.xyzz, r2.yyyy) + -r4.xyzz;
    r7.xyz = v.xyz;
  }
  { // 48
    precise float4 v = Mul(r8.xyzz, r2.xxxx) + r7.xzyy;
    r2.xyz = v.xyz;
  }
  { // 49
    precise float4 v = Mul(r3.xzyy, r2.wwww) + r2.xzyy;
    r3.xyz = v.xyz;
  }
  { // 50
    precise float4 v = r0.zzzz + -c[17].wwww;
    precise float s = -c[12].x - (-r0.z);
    r2.y = v.y;
    r1.y = s;
    ps = s;
  }
  { // 51
    precise float4 v = Dot3(r0.wxzz, r0.wxzz);
    precise float s = -c[16].w - (-r0.x);
    r1.x = v.x;
    r2.x = s;
    ps = s;
  }
  { // 52
    precise float4 v = r0.wwww + -c[18].wwww;
    precise float s = rcp(r1.w);
    r2.z = v.z;
    r2.w = s;
    ps = s;
  }
  { // 53
    precise float4 v = Dot3(r2.zxyy, r2.zxyy);
    precise float s = sqrt((abs(r1)).x);
    r1.w = v.w;
    r0.y = s;
    ps = s;
  }
  { // 54
    precise float4 v = r0.yyyy + -c[10].xxxx;
    precise float s = sqrt((abs(r1)).w);
    r3.w = v.w;
    r0.y = s;
    ps = s;
  }
  { // 59
    precise float4 v = r0.yyyy + -c[15].wwww;
    precise float s = rsqrt((abs(r1)).w);
    r1.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 60
    precise float4 v = saturate(Mul(r3.wwww, r2.wwww));
    o4.w = v.w;
  }
  { // 61
    precise float4 v = max(r5.xyyy, r5.xyyy);
    o1.xy = v.xy;
    o1.z = 0.0;
    o1.w = 0.0;
  }
  { // 62
    precise float4 v = max(r0.xzww, r0.xzww);
    o4.xyz = v.xyz;
  }
  { // 63
    precise float4 v = max(r6, r6);
    o3.xyzw = v.xyzw;
  }
  { // 64
    precise float4 v = Mul(r2.xyzz, r1.wwww);
    precise float s = -c[14].x - (-r0.y);
    r2.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 65
    precise float4 v = max(r0.xxxx, c[21].zzzz);
    precise float s = rcp(c[13].w);
    r0.x = v.x;
    r0.z = s;
    ps = s;
  }
  { // 66
    precise float4 v = saturate(Dot3(r2.zxyy, c[19].zxyy));
    precise float s = rcp(r1.z);
    r0.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 67
    precise float4 v = Mul(r0.xxxx, c[14].wwww);
    precise float s = log2(r0.y);
    r0.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 68
    precise float4 v = saturate(Mul(r1.yyxx, r0.wwzz));
    precise float s = exp2(r0.x);
    r5.yz = v.yz;
    r0.z = s;
    ps = s;
  }
  { // 69
    precise float4 v = Mul(r5.yyyy, r5.yyyy);
    precise float s = saturate(rcp(r0.z));
    r0.x = v.x;
    r5.x = s;
    ps = s;
  }
  { // 70
    precise float4 v = c[13].xyzz + -c[15].xyzz;
    precise float s = MulS(c[12].z, r0.x);
    r2.xyz = v.xyz;
    r0.z = s;
    ps = s;
  }
  { // 71
    precise float4 v = -r5.xyyy + c[21].xyyy;
    precise float s = MulS(c[12].w, r0.y);
    r1.xy = v.xy;
    r0.y = s;
    ps = s;
  }
  { // 72
    precise float4 v = r1.yyyy + -r5.yyyy;
    precise float s = exp2(r0.y);
    r0.x = v.x;
    r3.w = s;
    ps = s;
  }
  { // 73
    precise float4 v = Mul(-r0.zzzz, r0.xxxx) + c[21].xxxx;
    r0.z = v.z;
  }
  { // 74
    precise float4 v = Mul(r5.wwwz, r3.xzyw);
    r3.xyzw = v.xyzw;
  }
  { // 75
    precise float4 v = Mul(r3.wwww, r2.xyzz) + c[15].xyzz;
    r2.xyz = v.xyz;
  }
  { // 76
    precise float4 v = r4.zyxx + r3.yzxx;
    precise float s = max((r1.xx).x, (r1.xx).y);
    r0.xyw = v.xyw;
    ps = s;
  }
  { // 77
    precise float4 v = Mul(r0.yyyy, c[7].xyzz);
    precise float s = MulS(ps, r0.z);
    r1.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 78
    precise float4 v = Mul(r0.yyyy, r2.xyzz);
    precise float s = c[21].x - (r0.y);
    o2.xyz = v.xyz;
    o2.w = s;
    ps = s;
  }
  { // 79
    precise float4 v = Mul(r0.xxxx, c[6].xyzz) + r1.xyzz;
    r0.xyz = v.xyz;
  }
  { // 80
    precise float4 v = Mul(r0.wwww, c[5].xyzz) + r0.xyzz;
    o0.xyz = v.xyz;
  }
  if (!(system_data[0].x & 8)) oPos.w = 1.0 / oPos.w;
  if (system_data[0].x & 2) oPos.xy *= oPos.w;
  if (system_data[0].x & 4) oPos.z *= oPos.w;
  oPos.xyz *= asfloat(system_data[8].xyz);
  oPos.xyz = mad(asfloat(system_data[9].xyz), oPos.w, oPos.xyz);
  Vertex output; output.uv_alpha=o0; output.blend=o1; output.color=o2; output.material=o3; output.world=o4; output.position=oPos; return output;
}
