// FH1 shadow mask 11824C2EC1B156C6 / 0000400300000001.
#define FH1_SHADOW_CONSTANT_COUNT 13
#include "fh1_shadow_sampling.hlsli"

[earlydepthstencil]
float4 main(float4 projected:TEXCOORD0,float4 position:SV_Position):SV_Target {
 precise float4 r0=projected,r1=float4(abs(floor(position.xy)*FH1_SHADOW_PIXEL_SCALE),0,0),r2=0,r3=0,r4=0,r5=0,r6=0,oC0=0;
 precise float ps=0;bool p0=false;
  { // 9
    precise float4 v = Mul((abs(r1)).xyyy, c[7].xyyy);
    precise float s = rcp(r0.w);
    r1.xy = v.xy;
    r1.z = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r1.zzzz, c[12].xyzz);
    r2.xyz = v.xyz;
  }
  { // 11
    precise float4 v = Mul(r2.yzxz, r0.yyxx);
    r0.xyzw = v.xyzw;
  }
  { // 12
    precise float4 v = r0.xzzz + c[12].xxxx;
    r2.xy = v.xy;
  }
  r1.z = SampleGradientX(r2.yx,1.5);
  r1.w = SampleGradientX(r2.yx,0.5);
  { // 15
    precise float4 v = -c[6].xxxx + c[12].zzzz;
    r2.x = v.x;
  }
  { // 16
    precise float4 v = r2.xxxx + -r1.wzzz;
    r0.xz = v.xz;
  }
  { // 17
    precise float4 v = r0.wwww + -c[0].zzzz;
    precise float s = rcp(r0.z);
    r1.z = v.z;
    r3.y = s;
    ps = s;
  }
  { // 18
    precise float4 v = r0.yyyy + -c[1].zzzz;
    precise float s = rcp(r0.x);
    r0.z = v.z;
    r3.x = s;
    ps = s;
  }
  { // 19
    precise float4 v = Mul(r3.xxxy, c[6].yyyy);
    r2.zw = v.zw;
  }
  { // 20
    precise float4 v = -r2.zzzz + -c[12].xxxx;
    precise float s = rcp(c[1].y);
    r0.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 21
    precise float4 v = r0.xxxx + r2.wwww;
    precise float s = rcp(c[0].x);
    r0.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 22
    precise float4 v = Mul(r2.zzzz, r0.yzzz);
    r2.xy = v.xy;
  }
  { // 23
    precise float4 v = float4(c[10].zzzz > r2.zzzz);
    precise float s = max((c[4].zz).x, (c[4].zz).y);
    r0.y = v.y;
    ps = s;
  }
  { // 24
    precise float4 v = (((c[12].wwww).x == 0.0 && (r0.yyyy).x != 0.0) ? 0.0 : (c[12].wwww).x + 1.0).xxxx;
    precise float s = MulS(ps, c[6].y);
    bool next_p0 = (c[12].wwww).w == 0.0 && (r0.yyyy).w != 0.0;
    r0.y = v.y;
    r0.z = s;
    ps = s;
    p0 = next_p0;
  }
  { // 25
    precise float4 v = Mul(r2.xyyy, r1.zwww);
    precise float s = max((r0.zz).x, (r0.zz).y);
    r2.xy = v.xy;
    ps = s;
  }
  { // 26
    precise float4 v = (MulS((r2.xyyy).x, (c[4].xyyy).x) + MulS((r2.xyyy).y, (c[4].xyyy).y)) + (c[12].wwww).x;
    r1.z = v.z;
  }
  { // 27
    precise float4 v = (c[12].wwwz == 0.0) ? r2.zxyy : c[12].zzzz;
    r2.xyzw = v.xyzw;
  }
  { // 28
    precise float4 v = saturate(Dot4(c[2].zxyw, r2));
    precise float s = MulS(ps, r3.x);
    r0.z = v.z;
    r1.w = s;
    ps = s;
  }
  { // 29
    precise float4 v = r1.xzzz + r1.ywww;
    r1.xz = v.xz;
  }
  { // 30
    precise float4 v = Mul(r1.xxxx, c[11].wwww) + c[12].xxxx;
    r0.w = v.w;
  }
  { // 31
    precise float s = frac(r0.w);
    r0.w = s;
    ps = s;
  }
  { // 32
    precise float4 v = Mul(r0.wwww, c[10].yyyy) + c[11].xxxx;
    r1.y = v.y;
  }
  { // 33
    precise float4 v = saturate(Dot4(c[3].zxyw, r2));
    precise float s = sin(r1.y);
    r0.w = v.w;
    r1.x = s;
    ps = s;
  }
  { // 34
    precise float4 v = saturate(r1.zzzz + c[4].wwww);
    precise float s = cos(r1.y);
    r2.y = v.y;
    r1.y = s;
    ps = s;
  }
  { // 35
    precise float4 v = Mul(r1.xyyx, c[8].wzyx);
    r3.xyzw = v.xyzw;
  }
  { // 36
    precise float4 v = Mul(r1.xyyx, c[9]);
    precise float s = saturate(MulS(c[11].z, r0.x));
    r1.xyzw = v.xyzw;
    r2.x = s;
    ps = s;
  }
  if(p0) { // 37
    precise float4 v = r0.zwzw + r1;
    r4.xyzw = v.xyzw;
  }
  if(p0) { // 38
    precise float4 v = r0.zzww + r3.wyxz;
    r1.xyzw = v.xyzw;
  }
  if(p0) r1.x = SampleShadowX(r1.xw);
  if(p0) r1.y = SampleShadowX(r1.yz);
  if(p0) r1.z = SampleShadowX(r4.xy);
  if(p0) r1.w = SampleShadowX(r4.zw);
  if(p0) { // 43
    precise float4 v = float4(r1 >= r2.yyyy);
    r1.xyzw = v.xyzw;
  }
  if(p0) { // 44
    precise float4 v = Dot4(r1.wzxy, c[11].zzzz);
    r0.x = v.x;
  }
  { // 45
    precise float s = (r0.y == 1.0) ? 0.0 : ((r0.y == 0.0) ? 1.0 : r0.y);
    bool next_p0 = r0.y == 1.0;
    r0.y = s;
    ps = s;
    p0 = next_p0;
  }
  if(p0) { // 46
    precise float4 v = Mul(r2.xxxx, c[10].xxxx) + c[12].zzzz;
    r0.x = v.x;
  }
  if(p0) { // 47
    precise float4 v = Mul(r1, r0.xxxx) + r0.zwzw;
    r5.xyzw = v.xyzw;
  }
  if(p0) { // 48
    precise float4 v = Mul(r3.zxyw, r0.xxxx) + r0.wwzz;
    r4.xyzw = v.xyzw;
  }
  if(p0) r4.x = SampleShadowX(r4.wx);
  if(p0) r4.y = SampleShadowX(r4.zy);
  if(p0) r4.z = SampleShadowX(r5.xy);
  if(p0) r4.w = SampleShadowX(r5.zw);
  if(p0) { // 53
    precise float4 v = float4(r2.xxxx > c[12].xxxx);
    r0.y = v.y;
  }
  if(p0) { // 54
    precise float4 v = float4(r4 >= r2.yyyy);
    r4.xyzw = v.xyzw;
  }
  if(p0) { // 55
    precise float4 v = Dot4(r4.wzxy, c[11].zzzz);
    precise float s = (r0.y != 0.0) ? 0.0 : 1.0;
    bool next_p0 = r0.y != 0.0;
    r0.x = v.x;
    ps = s;
    p0 = next_p0;
  }
  if(p0) { // 56
    precise float4 v = Mul(r2.xxxx, c[11].yyyy) + c[12].zzzz;
    r0.y = v.y;
  }
  if(p0) { // 57
    precise float4 v = Mul(r1, r0.yyyy) + r0.zwzw;
    r4.xyzw = v.xyzw;
  }
  if(p0) { // 58
    precise float4 v = Mul(r3.zxyw, r0.yyyy) + r0.wwzz;
    r1.xyzw = v.xyzw;
  }
  if(p0) r1.x = SampleShadowX(r1.wx);
  if(p0) r1.y = SampleShadowX(r1.zy);
  if(p0) r1.z = SampleShadowX(r4.xy);
  if(p0) r1.w = SampleShadowX(r4.zw);
  if(p0) { // 63
    precise float4 v = float4(r1 >= r2.yyyy);
    r1.xyzw = v.xyzw;
  }
  if(p0) { // 64
    precise float4 v = Dot4(r1.wzxy, c[11].zzzz);
    r0.y = v.y;
  }
  if(p0) { // 65
    precise float4 v = r0.yyyy + r0.xxxx;
    r0.y = v.y;
  }
  if(p0) { // 66
    precise float s = MulS(c[12].x, r0.y);
    r0.x = s;
    ps = s;
  }
  { // 67
    precise float4 v = max(r0, r0);
    precise float s = c[5].w + r0.x;
    oC0.y = 1.0;
    oC0.z = 0.0;
    oC0.w = 0.0;
    oC0.x = s;
    ps = s;
  }
  return oC0 * asfloat(xe_system[15].y);
}
