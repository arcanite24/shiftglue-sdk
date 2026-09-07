// FH1 shadow mask 22DA22B5639EBAE4 / 0000400300000001.
#define FH1_SHADOW_CONSTANT_COUNT 15
#include "fh1_shadow_sampling.hlsli"

[earlydepthstencil]
float4 main(float4 projected:TEXCOORD0, float4 position:SV_Position):SV_Target {
 precise float4 r0=projected,r1=float4(abs(floor(position.xy)*FH1_SHADOW_PIXEL_SCALE),0,0),r2=0,r3=0,oC0=0;
 precise float ps=0;
  { // 4
    precise float4 v = Mul((abs(r1)).xxyy, c[7].xxyy);
    precise float s = rcp(r0.w);
    r1.yz = v.yz;
    r1.x = s;
    ps = s;
  }
  { // 5
    precise float4 v = Mul(r1.xxxx, c[14].xyzz);
    r2.xyz = v.xyz;
  }
  { // 6
    precise float4 v = Mul(r2.xzzy, r0.yxyx);
    r0.xyzw = v.xyzw;
  }
  { // 7
    precise float4 v = r0.xwww + c[14].yyyy;
    r1.xw = v.xw;
  }
  r1.x = SampleGradientX(r1.wx);
  { // 9
    precise float4 v = -c[6].xxxx + c[14].zzzz;
    r1.w = v.w;
  }
  { // 10
    precise float4 v = r1.wwww + -r1.xxxx;
    r1.w = v.w;
  }
  { // 11
    precise float4 v = Mul(c[8].zzzz, c[6].yyyy);
    precise float s = rcp(r1.w);
    r1.x = v.x;
    r0.x = s;
    ps = s;
  }
  { // 12
    precise float4 v = Mul(r1.xxxx, r0.xxxx);
    r1.w = v.w;
  }
  { // 13
    precise float4 v = r0.yyyy + -c[0].zzzz;
    precise float s = MulS(c[6].y, r0.x);
    r0.y = v.y;
    r2.z = s;
    ps = s;
  }
  { // 14
    precise float4 v = r0.zzzz + -c[1].zzzz;
    precise float s = rcp(c[0].x);
    r0.z = v.z;
    r0.x = s;
    ps = s;
  }
  { // 15
    precise float4 v = Mul(r2.zzzz, r0.xzzz);
    precise float s = rcp(c[1].y);
    r0.xw = v.xw;
    r0.z = s;
    ps = s;
  }
  { // 16
    precise float4 v = Mul(r0.xwww, r0.yzzz);
    r2.xy = v.xy;
  }
  { // 17
    precise float4 v = (MulS((r2.xyyy).x, (c[8].xyyy).x) + MulS((r2.xyyy).y, (c[8].xyyy).y)) + (c[12].zzzz).x;
    r1.x = v.x;
  }
  { // 18
    precise float4 v = r1.xyyy + r1.wzzz;
    precise float s = max((c[14].zz).x, (c[14].zz).y);
    r0.xy = v.xy;
    r2.w = s;
    ps = s;
  }
  { // 19
    precise float4 v = Dot4(c[9].zxyw, r2.zxyw);
    precise float s = c[8].w + r0.x;
    r0.z = v.z;
    r0.w = s;
    ps = s;
  }
  { // 20
    precise float4 v = Mul(r0.yyyy, c[13].wwww) + c[14].yyyy;
    r0.x = v.x;
  }
  { // 21
    precise float4 v = -r0.zzww + c[12].yyyy;
    r0.yz = v.yz;
  }
  { // 22
    precise float4 v = -(abs(r0)).yyzz + c[13].zzzz;
    r0.yz = v.yz;
  }
  { // 23
    precise float4 v = min(r0.zzzz, r0.yyyy);
    precise float s = frac(r0.x);
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r0.yxxx, c[13].yxxx);
    r0.xy = v.xy;
  }
  { // 25
    precise float4 v = saturate(-r0.xxxx + c[14].zzzz);
    precise float s = c[12].x + r0.y;
    r0.x = v.x;
    r0.w = s;
    ps = s;
  }
  { // 26
    precise float4 v = saturate(Dot4(c[2].zxyw, r2.zxyw));
    precise float s = sin(r0.w);
    r0.y = v.y;
    r1.x = s;
    ps = s;
  }
  { // 27
    precise float4 v = saturate(Dot4(c[3].zxyw, r2.zxyw));
    precise float s = cos(r0.w);
    r0.z = v.z;
    r1.y = s;
    ps = s;
  }
  { // 28
    precise float4 v = Mul(r1.xyyx, c[11]) + r0.yzyz;
    r3.xyzw = v.xyzw;
  }
  { // 29
    precise float4 v = Mul(r1.xyxy, c[10].xzwy) + r0.yyzz;
    r1.xyzw = v.xyzw;
  }
  r1.x = SampleShadowX(r1.xw);
  r1.y = SampleShadowX(r1.yz);
  r1.z = SampleShadowX(r3.xy);
  r1.w = SampleShadowX(r3.zw);
  { // 34
    precise float4 v = saturate(Dot4(c[4].zxyw, r2.zxyw));
    r0.y = v.y;
  }
  { // 35
    precise float4 v = float4(r1 >= r0.yyyy);
    r1.xyzw = v.xyzw;
  }
  { // 36
    precise float4 v = Dot4(r1.wzxy, c[14].wwww);
    precise float s = c[5].w + r0.x;
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 37
    precise float4 v = r0.xxxx + r0.yyyy;
    oC0.x = v.x;
    oC0.y = 1.0;
    oC0.z = 0.0;
    oC0.w = 0.0;
  }
  return oC0 * asfloat(xe_system[15].y);
}
