// FH1 lit scene vertex program 3BC346726C1C2535.
// Direct straight-line form; guest bindings remain during stage migration.
ByteAddressBuffer shared_srv : register(t0);
cbuffer system_constants : register(b0) { uint4 system_data[30]; };
cbuffer float_constants : register(b1) { float4 c[25]; };
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
  uint word = 0;
  // Root SRVs have no hardware bounds; preserve shared fallback's zero reads.
  // Owned geometry is separately bounded before rebasing the fetch address.
  [branch]
  if (address < 0x20000000u) word = shared_srv.Load(address);
  return Endian(word,fetch_data[47].w & 3);
}
struct Vertex { float4 uv:TEXCOORD0; float4 base_color:TEXCOORD1; float4 lighting0:TEXCOORD2; float4 lighting1:TEXCOORD3; float4 position:SV_Position; };
Vertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == system_data[0].w ? 0 : vertex_id;
  index = Endian(index,system_data[1].x);
  index = clamp((index + system_data[1].y) & 0xFFFFFF,system_data[1].z,system_data[1].w);
  precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0,r9=0,r10=0,r11=0;
  precise float4 o0=0,o1=0,o2=0,o3=0,oPos=0;
  precise float ps=0;
  r0.x=float(index);
  r0.yz=Mul(r0.xxxx,c[23].xxyy).yz;
  uint address=(fetch_data[47].z & ~3u) + uint(int(floor(r0.y))) * 40u;
  uint word=LoadWord(address);
  r6.xyz=float3(word & 2047u,(word >> 11) & 2047u,word >> 22) * float3(1.0/2047.0,1.0/2047.0,1.0/1023.0);
  word=LoadWord(address+4);
  r1.xyz=max(float3(int(word << 21) >> 21,int(word << 10) >> 21,int(word) >> 22) * float3(1.0/1023.0,1.0/1023.0,1.0/511.0),-1.0);
  word=LoadWord(address+12);
  r9.xyz=max(float3(int(word << 22) >> 22,int(word << 12) >> 22,int(word << 2) >> 22) * (1.0/511.0),-1.0);
  word=LoadWord(address+16);
  r2.zw=float2((word >> 11) & 2047u,word >> 22) * float2(1.0/2047.0,1.0/1023.0);
  word=LoadWord(address+20);
  r5.yz=float2(word & 2047u,(word >> 11) & 2047u) * (1.0/2047.0);
  uint half0=LoadWord(address+24),half1=LoadWord(address+28);
  r7=f16tof32(uint4(half1 >> 16,half1 & 65535u,half0 >> 16,half0 & 65535u));
  word=LoadWord(address+32);
  r3=float4((word >> 16) & 255u,(word >> 8) & 255u,word >> 24,word & 255u) * (1.0/255.0);
  word=LoadWord(address+36);
  r4.xyz=float3((word >> 16) & 255u,(word >> 8) & 255u,word & 255u) * (1.0/255.0);
  { // 18
    precise float4 v = Mul(r6.xxyz, c[7].xxzy) + c[6].xxzy;
    r11.yzw = v.yzw;
  }
  { // 19
    precise float4 v = Mul(r7.xyxy, c[9].xxxx) + c[4].xzyw;
    r8.xyzw = v.xyzw;
  }
  { // 20
    precise float4 v = (r0.zzzz >= -r0.zzzz) ? 1.0 : 0.0;
    r0.y = v.y;
  }
  { // 21
    precise float4 v = Mul(r0.yyyy, c[22].yyyy) + c[23].wwww;
    r6.w = v.w;
  }
  { // 22
    precise float4 v = r8.zzww + -r8.xxyy;
    r0.yw = v.yw;
  }
  { // 23
    precise float4 v = -r11.ywzz + c[5].xyzz;
    precise float s = rcp(r0.y);
    r6.xyz = v.xyz;
    r0.z = s;
    ps = s;
  }
  { // 24
    precise float4 v = Dot3(r6.zxyy, r6.zxyy);
    precise float s = rcp(r6.w);
    r1.w = v.w;
    r2.x = s;
    ps = s;
  }
  { // 25
    precise float4 v = Mul(r2.xxxx, r0.xxxx);
    precise float s = sqrt((abs(r1)).w);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 26
    precise float4 v = -r8.xyyy + r0.xxxx;
    precise float s = rsqrt((abs(r1)).w);
    r2.xy = v.xy;
    r0.x = s;
    ps = s;
  }
  { // 27
    precise float4 v = Mul(r6.zyxx, r0.xxxx);
    precise float s = rcp(r0.w);
    r8.xyz = v.xyz;
    r0.w = s;
    ps = s;
  }
  { // 28
    precise float4 v = saturate(Mul(r2.xyyy, r0.zwww));
    r0.xw = v.xw;
  }
  { // 29
    precise float4 v = Mul(r8.yxzz, r1.zxyy);
    precise float s = frac(r0.y);
    r6.xyz = v.xyz;
    r0.z = s;
    ps = s;
  }
  { // 30
    precise float4 v = Mul(r8.xzyy, r1.yzxx) + -r6.xyzz;
    r6.xyz = v.xyz;
  }
  { // 31
    precise float4 v = Dot3(r6.zxyy, r6.zxyy);
    precise float s = min((r0.xw).x, (r0.xw).y);
    r0.y = v.y;
    r8.w = s;
    ps = s;
  }
  { // 32
    precise float4 v = floor(-r8.wwww);
    precise float s = rsqrt((abs(r0)).y);
    r0.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 33
    precise float4 v = Mul(r6, r0.yyyz);
    r10.xyzw = v.xyzw;
  }
  { // 34
    precise float4 v = -c[8].xxxx + c[24].yyyy;
    precise float s = trunc(r10.w);
    r5.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 35
    precise float4 v = (r0.yyyy == c[24].xxyz) ? 1.0 : 0.0;
    r0.yzw = v.yzw;
  }
  { // 36
    precise float4 v = r0.yyww + (abs(r0)).zzzz;
    r0.yz = v.yz;
  }
  { // 37
    precise float4 v = (r0.yzzz != c[24].xxxx) ? 1.0 : 0.0;
    r7.xy = v.xy;
  }
  { // 38
    precise float4 v = (-(abs(r7)).yyyy >= 0.0) ? c[24].yyyy : c[23].zzzz;
    r0.z = v.z;
  }
  { // 39
    precise float4 v = Mul(r10.xzyy, r0.zzzz);
    precise float s = (-(abs(r7)).x >= 0.0) ? 1.0 : 0.0;
    r8.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 40
    precise float4 v = Mul(r0.yyyy, r1.xyzz);
    r6.xyz = v.xyz;
  }
  { // 41
    precise float4 v = Mul(r8.xyzz, c[24].zzzz) + r6.xzyy;
    r8.xyz = v.xyz;
  }
  { // 42
    precise float4 v = r8.xzyy + r6.xyzz;
    r6.xyz = v.xyz;
  }
  { // 43
    precise float4 v = Dot3(r6.zxyy, r6.zxyy);
    r0.w = v.w;
  }
  { // 44
    precise float4 v = Mul(r0.yyzz, r7.wwzz);
    precise float s = rsqrt((abs(r0)).w);
    r0.yz = v.yz;
    r0.w = s;
    ps = s;
  }
  { // 45
    precise float4 v = Mul(r6.xzyy, r0.wwww);
    r6.xyz = v.xyz;
  }
  { // 46
    precise float4 v = Mul(r0.yyzz, -r0.xxxx);
    precise float s = max((r10.xx).x, (r10.xx).y);
    r0.yz = v.yz;
    ps = s;
  }
  { // 47
    precise float4 v = -r10.zzzz + r6.xxxx;
    precise float s = r6.y + ps;
    r4.w = v.w;
    r6.w = s;
    ps = s;
  }
  { // 48
    precise float4 v = Mul(r0.yyyy, r1.xxzy);
    precise float s = MulS((r4.ww).x, (r4.ww).y);
    r1.yzw = v.yzw;
    r11.x = s;
    ps = s;
  }
  { // 49
    precise float4 v = (MulS((r6.wzzz).x, (r6.wzzz).x) + MulS((r6.wzzz).y, (r6.wzzz).y)) + (c[24].xxxx).x;
    r1.x = v.x;
  }
  { // 50
    precise float4 v = r11.ywxz + r1.ywxz;
    precise float s = ((abs(r0)).x >= 0.0) ? 1.0 : 0.0;
    r1.xyzw = v.xyzw;
    r0.x = s;
    ps = s;
  }
  { // 51
    precise float4 v = Mul(r10.xxzy, r0.zzzz) + r1.xxwy;
    r0.yzw = v.yzw;
  }
  { // 52
    precise float4 v = Dot4(c[0].zxyw, r0.zywx);
    oPos.x = v.x;
  }
  { // 53
    precise float4 v = Dot4(c[1].zxyw, r0.zywx);
    oPos.y = v.y;
  }
  { // 54
    precise float4 v = Dot4(c[2].zxyw, r0.zywx);
    oPos.z = v.z;
  }
  { // 55
    precise float4 v = Dot4(c[3].zxyw, r0.zywx);
    oPos.w = v.w;
  }
  { // 56
    precise float4 v = (r3.zzzz > c[22].xxxx) ? 1.0 : 0.0;
    precise float s = (r3.xx).x + (r3.xx).y;
    r2.x = v.x;
    r8.x = s;
    ps = s;
  }
  { // 57
    precise float4 v = saturate(Dot3(r9.zxyy, c[20].zxyy));
    precise float s = (r3.yy).x + (r3.yy).y;
    r0.x = v.x;
    r8.y = s;
    ps = s;
  }
  { // 58
    precise float4 v = Mul(r4.xyzz, r4.xyzz);
    precise float s = (r3.ww).x + (r3.ww).y;
    r3.xyz = v.xyz;
    r8.z = s;
    ps = s;
  }
  { // 59
    precise float4 v = Mul(r3.xyzz, c[11].yyyy) + c[14].xyzz;
    r3.xyz = v.xyz;
  }
  { // 60
    precise float4 v = Mul(r8.xyzz, c[10].xyzz);
    precise float s = log2(r0.x);
    r4.xyz = v.xyz;
    r0.x = s;
    ps = s;
  }
  { // 61
    precise float4 v = (r2.xxxx > 0.0) ? r8.xyzz : r4.xyzz;
    r8.xyz = v.xyz;
  }
  { // 62
    precise float4 v = r0.wwww + -c[12].xxxx;
    precise float s = (c[12].yx).x - (c[12].yx).y;
    r2.x = v.x;
    r2.y = s;
    ps = s;
  }
  { // 63
    precise float4 v = r0.wwww + -c[18].wwww;
    precise float s = -c[17].w - (-r0.y);
    r4.y = v.y;
    r4.x = s;
    ps = s;
  }
  { // 64
    precise float4 v = r0.zzzz + -c[19].wwww;
    precise float s = rcp(c[13].w);
    r4.z = v.z;
    r1.x = s;
    ps = s;
  }
  { // 65
    precise float4 v = max(r8, r8);
    o3.xyzw = v.xyzw;
  }
  { // 66
    precise float4 v = (r7.yxxx == 0.0) ? r5.yzzz : r2.zwww;
    o0.xy = v.xy;
    o0.z = 0.0;
    o0.w = 0.0;
  }
  { // 67
    precise float4 v = Dot3(r4.zxyy, r4.zxyy);
    precise float s = rcp(r2.y);
    r0.z = v.z;
    r0.y = s;
    ps = s;
  }
  { // 68
    precise float4 v = saturate(Mul(r2.xxxx, r0.yyyy));
    precise float s = sqrt((abs(r0)).z);
    r5.y = v.y;
    r0.y = s;
    ps = s;
  }
  { // 69
    precise float4 v = r0.yyyy + -c[16].wwww;
    precise float s = rsqrt((abs(r1)).z);
    r1.y = v.y;
    r0.w = s;
    ps = s;
  }
  { // 70
    precise float4 v = saturate(Mul(r1.yyyy, r1.xxxx));
    precise float s = rsqrt((abs(r0)).z);
    r2.w = v.w;
    r0.z = s;
    ps = s;
  }
  { // 71
    precise float4 v = c[13].xyzz + -c[16].xyzz;
    precise float s = -c[15].x - (-r0.y);
    r1.xyz = v.xyz;
    r0.y = s;
    ps = s;
  }
  { // 72
    precise float4 v = Mul(r6.zwww, r0.wwww);
    precise float s = max((r0.yy).x, (r0.yy).y);
    r2.xy = v.xy;
    r0.y = s;
    ps = s;
  }
  { // 73
    precise float4 v = Mul(r4.xwyz, r0.zwzz);
    precise float s = max((c[24].xx).x, (c[24].xx).y);
    r4.xyzw = v.xyzw;
    r0.w = s;
    ps = s;
  }
  { // 74
    precise float4 v = saturate(Dot3(r4.wxzz, c[20].zxyy));
    precise float s = max((r0.yw).x, (r0.yw).y);
    r0.z = v.z;
    r0.y = s;
    ps = s;
  }
  { // 75
    precise float4 v = Mul(r0.yyyy, c[15].wwww);
    precise float s = MulS(c[8].y, r0.x);
    r0.y = v.y;
    r1.w = s;
    ps = s;
  }
  { // 76
    precise float4 v = max(r4.yyyy, r4.yyyy);
    precise float s = exp2(r0.y);
    r2.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 77
    precise float4 v = saturate(Dot3(r2.yxzz, c[20].zyxx));
    precise float s = saturate(rcp(r0.x));
    r5.z = v.z;
    r5.w = s;
    ps = s;
  }
  { // 78
    precise float4 v = Mul(r5.yxxx, r5.yzzz);
    precise float s = log2(r0.z);
    r0.xy = v.xy;
    r0.z = s;
    ps = s;
  }
  { // 79
    precise float4 v = -r5.ywww + c[24].wyyy;
    precise float s = c[8].x + r0.y;
    r4.xy = v.xy;
    r0.w = s;
    ps = s;
  }
  { // 80
    precise float4 v = Mul(r0.xzzz, c[12].zwww);
    precise float s = exp2(r1.w);
    r2.xy = v.xy;
    r0.y = s;
    ps = s;
  }
  { // 81
    precise float4 v = r4.xxxx + -r5.yyyy;
    precise float s = exp2(r2.y);
    r0.x = v.x;
    r0.z = s;
    ps = s;
  }
  { // 82
    precise float4 v = Mul(-r2.xxxx, r0.xxxx) + c[24].yyyy;
    r0.x = v.x;
  }
  { // 83
    precise float4 v = Mul(r0.wwww, c[21].xyzz);
    precise float s = max((r4.yy).x, (r4.yy).y);
    r2.xyz = v.xyz;
    ps = s;
  }
  { // 84
    precise float4 v = Mul(r2, r0.yyyz);
    precise float s = MulS(ps, r0.x);
    r2.xyzw = v.xyzw;
    r0.x = s;
    ps = s;
  }
  { // 85
    precise float4 v = r3.xyzz + r2.xyzz;
    o2.xyz = v.xyz;
    o2.w = 0.0;
  }
  { // 86
    precise float4 v = Mul(r2.wwww, r1.xxyz) + c[16].xxyz;
    r0.yzw = v.yzw;
  }
  { // 87
    precise float4 v = Mul(r0.xxxx, r0.yzww);
    precise float s = c[24].y - (r0.x);
    o1.xyz = v.xyz;
    o1.w = s;
    ps = s;
  }
  if (!(system_data[0].x & 8)) oPos.w = 1.0 / oPos.w;
  if (system_data[0].x & 2) oPos.xy *= oPos.w;
  if (system_data[0].x & 4) oPos.z *= oPos.w;
  oPos.xyz *= asfloat(system_data[8].xyz);
  oPos.xyz = mad(asfloat(system_data[9].xyz), oPos.w, oPos.xyz);
  Vertex output; output.uv=o0; output.base_color=o1; output.lighting0=o2; output.lighting1=o3; output.position=oPos; return output;
}
