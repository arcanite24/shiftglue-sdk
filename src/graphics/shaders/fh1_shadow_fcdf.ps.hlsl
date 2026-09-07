// FH1 shadow mask FCDF9BE8C57F7D01 / 0000400300000001.
#define FH1_SHADOW_CONSTANT_COUNT 17
#include "fh1_shadow_sampling.hlsli"

[earlydepthstencil]
float4 main(float4 projected:TEXCOORD0,float4 position:SV_Position):SV_Target {
 precise float4 r0=projected,r1=float4(abs(floor(position.xy)*FH1_SHADOW_PIXEL_SCALE),0,0),r2=0,r3=0,r4=0,r5=0,r6=0,oC0=0;
 precise float ps=0;bool p0=false;
  { // 9
    precise float4 v = -c[6].xxxx + c[15].zzzz;
    precise float s = rcp(r0.w);
    r3.x = v.x;
    r2.x = s;
    ps = s;
  }
  { // 10
    precise float4 v = Mul(r2.xxxx, c[15].xyzz);
    r2.xyz = v.xyz;
  }
  { // 11
    precise float4 v = Mul(r2.zzyx, r0.yxyx);
    r2.xyzw = v.xyzw;
  }
  { // 12
    precise float4 v = r2.zzww + c[15].xxxx;
    r0.yz = v.yz;
  }
  r0.x = SampleGradientX(r0.zy,1.5);
  r0.y = SampleGradientX(r0.zy,0.5);
  { // 15
    precise float4 v = Mul(c[4].zzzz, c[6].yyyy);
    r0.w = v.w;
  }
  { // 16
    precise float4 v = r3.xxxx + -r0.xxyy;
    precise float s = max((r2.yy).x, (r2.yy).y);
    r0.yz = v.yz;
    r0.x = s;
    ps = s;
  }
  { // 17
    precise float4 v = r2.xxxx + -c[1].zzzz;
    precise float s = rcp(r0.z);
    r2.y = v.y;
    r0.z = s;
    ps = s;
  }
  { // 18
    precise float4 v = Mul(r0.wwww, r0.zzzz);
    precise float s = rcp(c[0].x);
    r3.w = v.w;
    r2.x = s;
    ps = s;
  }
  { // 19
    precise float4 v = Mul((abs(r1)).xyyy, c[8].xyyy);
    precise float s = MulS(c[6].y, r0.z);
    r3.xy = v.xy;
    r1.y = s;
    ps = s;
  }
  { // 20
    precise float4 v = Mul(r1.yyyy, r2.xyyy);
    precise float s = -c[0].z - (-r0.x);
    r2.xy = v.xy;
    r1.x = s;
    ps = s;
  }
  { // 21
    precise float4 v = float4(c[13].wwww > r1.yyyy);
    precise float s = -c[15].x + -r1.y;
    r0.w = v.w;
    r0.x = s;
    ps = s;
  }
  { // 22
    precise float4 v = (((c[15].wwww).x == 0.0 && (r0.wwww).x != 0.0) ? 0.0 : (c[15].wwww).x + 1.0).xxxx;
    precise float s = rcp(c[1].y);
    bool next_p0 = (c[15].wwww).w == 0.0 && (r0.wwww).w != 0.0;
    r0.z = v.z;
    r1.z = s;
    ps = s;
    p0 = next_p0;
  }
  { // 23
    precise float4 v = Mul(r2.xyyy, r1.xzzz);
    precise float s = rcp(r0.y);
    r1.xz = v.xz;
    r0.y = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r0.yyyy, c[6].yyyy) + r0.xxxx;
    r0.x = v.x;
  }
  { // 25
    precise float4 v = (MulS((r1.xzzz).x, (c[4].xyyy).x) + MulS((r1.xzzz).y, (c[4].xyyy).y)) + (c[15].wwww).x;
    r3.z = v.z;
  }
  { // 26
    precise float4 v = (c[15].wwwz == 0.0) ? r1.yxzz : c[15].zzzz;
    r2.xyzw = v.xyzw;
  }
  { // 27
    precise float4 v = r3.zzxx + r3.wwyy;
    r0.yw = v.yw;
  }
  { // 28
    precise float4 v = Mul(r0.wwww, c[14].wwww) + c[15].xxxx;
    r1.w = v.w;
  }
  { // 29
    precise float4 v = saturate(Dot4(c[2].zxyw, r2));
    precise float s = frac(r1.w);
    r4.x = v.x;
    r1.w = s;
    ps = s;
  }
  { // 30
    precise float4 v = Mul(r1.wwww, c[13].zzzz) + c[14].xxxx;
    r0.w = v.w;
  }
  { // 31
    precise float4 v = saturate(Dot4(c[3].zxyw, r2));
    precise float s = sin(r0.w);
    r4.y = v.y;
    r2.x = s;
    ps = s;
  }
  { // 32
    precise float4 v = saturate(r0.yyyy + c[4].wwww);
    precise float s = cos(r0.w);
    r0.y = v.y;
    r2.y = s;
    ps = s;
  }
  { // 33
    precise float4 v = Mul(r2.xyyx, c[11].wzyx);
    r3.xyzw = v.xyzw;
  }
  { // 34
    precise float4 v = Mul(r2.xyyx, c[12]);
    precise float s = saturate(MulS(c[14].z, r0.x));
    r2.xyzw = v.xyzw;
    r0.x = s;
    ps = s;
  }
  if(p0) { // 35
    precise float4 v = r4.xyxy + r2;
    r5.xyzw = v.xyzw;
  }
  if(p0) { // 36
    precise float4 v = r4.xxyy + r3.wyxz;
    r2.xyzw = v.xyzw;
  }
  if(p0) r2.x = SampleShadowX(r2.xw);
  if(p0) r2.y = SampleShadowX(r2.yz);
  if(p0) r2.z = SampleShadowX(r5.xy);
  if(p0) r2.w = SampleShadowX(r5.zw);
  if(p0) { // 41
    precise float4 v = float4(r2 >= r0.yyyy);
    r2.xyzw = v.xyzw;
  }
  if(p0) { // 42
    precise float4 v = Dot4(r2.wzxy, c[14].zzzz);
    r0.w = v.w;
  }
  { // 43
    precise float s = (r0.z == 1.0) ? 0.0 : ((r0.z == 0.0) ? 1.0 : r0.z);
    bool next_p0 = r0.z == 1.0;
    r0.z = s;
    ps = s;
    p0 = next_p0;
  }
  if(p0) { // 44
    precise float4 v = Mul(r0.xxxx, c[13].xxxx) + c[15].zzzz;
    r0.z = v.z;
  }
  if(p0) { // 45
    precise float4 v = Mul(r2, r0.zzzz) + r4.xyxy;
    r6.xyzw = v.xyzw;
  }
  if(p0) { // 46
    precise float4 v = Mul(r3.zxyw, r0.zzzz) + r4.yyxx;
    r5.xyzw = v.xyzw;
  }
  if(p0) r5.x = SampleShadowX(r5.wx);
  if(p0) r5.y = SampleShadowX(r5.zy);
  if(p0) r5.z = SampleShadowX(r6.xy);
  if(p0) r5.w = SampleShadowX(r6.zw);
  if(p0) { // 51
    precise float4 v = float4(r0.xxxx > c[15].xxxx);
    r0.z = v.z;
  }
  if(p0) { // 52
    precise float4 v = float4(r5 >= r0.yyyy);
    r5.xyzw = v.xyzw;
  }
  if(p0) { // 53
    precise float4 v = Dot4(r5.wzxy, c[14].zzzz);
    precise float s = (r0.z != 0.0) ? 0.0 : 1.0;
    bool next_p0 = r0.z != 0.0;
    r0.w = v.w;
    ps = s;
    p0 = next_p0;
  }
  if(p0) { // 54
    precise float4 v = Mul(r0.xxxx, c[14].yyyy) + c[15].zzzz;
    r0.x = v.x;
  }
  if(p0) { // 55
    precise float4 v = Mul(r2, r0.xxxx) + r4.xyxy;
    r5.xyzw = v.xyzw;
  }
  if(p0) { // 56
    precise float4 v = Mul(r3.zxyw, r0.xxxx) + r4.yyxx;
    r2.xyzw = v.xyzw;
  }
  if(p0) r2.x = SampleShadowX(r2.wx);
  if(p0) r2.y = SampleShadowX(r2.zy);
  if(p0) r2.z = SampleShadowX(r5.xy);
  if(p0) r2.w = SampleShadowX(r5.zw);
  if(p0) { // 61
    precise float4 v = float4(r2 >= r0.yyyy);
    r2.xyzw = v.xyzw;
  }
  if(p0) { // 62
    precise float4 v = Dot4(r2.wzxy, c[14].zzzz);
    r0.x = v.x;
  }
  if(p0) { // 63
    precise float4 v = r0.xxxx + r0.wwww;
    r0.y = v.y;
  }
  if(p0) { // 64
    precise float s = MulS(c[15].x, r0.y);
    r0.w = s;
    ps = s;
  }
  { // 65
    precise float4 v = (c[15].wwwz == 0.0) ? r1.yxzz : c[15].zzzz;
    r1.xyzw = v.xyzw;
  }
  { // 66
    precise float4 v = Dot4(c[9].zxyw, r1);
    r0.x = v.x;
  }
  { // 67
    precise float4 v = Dot4(c[10].zxyw, r1);
    r0.y = v.y;
  }
  { // 68
    precise float4 v = r0.xyyy + c[7].zwww;
    r0.xy = v.xy;
  }
  { // 69
    precise float4 v = -r0.xyyy + c[14].zzzz;
    r0.xy = v.xy;
  }
  { // 70
    precise float4 v = -(abs(r0)).xyyy + c[7].yyyy;
    r0.xy = v.xy;
  }
  { // 71
    precise float s = min((r0.xy).x, (r0.xy).y);
    r0.x = s;
    ps = s;
  }
  { // 72
    precise float4 v = saturate(Mul(r0.xxxx, c[13].yyyy) + c[16].xxxx);
    r0.x = v.x;
  }
  { // 73
    precise float s = c[5].w + r0.x;
    r0.x = s;
    ps = s;
  }
  { // 74
    precise float4 v = r0.xxxx + r0.wwww;
    oC0.x = v.x;
    oC0.y = 1.0;
    oC0.z = 0.0;
    oC0.w = 0.0;
  }
  return oC0 * asfloat(xe_system[15].y);
}
