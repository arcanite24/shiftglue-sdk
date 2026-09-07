// FH1 shadow mask 93626E75D17576C5 / 0000400300000001.
#define FH1_SHADOW_CONSTANT_COUNT 12
#include "fh1_shadow_sampling.hlsli"

[earlydepthstencil]
float4 main(float4 projected:TEXCOORD0, float4 position:SV_Position):SV_Target {
 precise float4 r0=projected,r1=float4(abs(floor(position.xy)*FH1_SHADOW_PIXEL_SCALE),0,0),r2=0,r3=0,oC0=0;
 precise float ps=0;
  { // 4
    precise float s = rcp(r0.w);
    r2.x = s;
    ps = s;
  }
  { // 5
    precise float4 v = Mul(r2.xxxx, c[11].xyzz);
    r2.xyz = v.xyz;
  }
  { // 6
    precise float4 v = Mul(r2.xzzy, r0.yxyx);
    r0.xyzw = v.xyzw;
  }
  { // 7
    precise float4 v = r0.xwww + c[11].yyyy;
    r2.xy = v.xy;
  }
  r2.x = SampleGradientX(r2.yx);
  { // 9
    precise float4 v = -c[6].xxxx + c[11].zzzz;
    precise float s = max((c[4].zz).x, (c[4].zz).y);
    r2.y = v.y;
    ps = s;
  }
  { // 10
    precise float4 v = r2.yyyy + -r2.xxxx;
    precise float s = MulS(ps, c[6].y);
    r2.x = v.x;
    r2.z = s;
    ps = s;
  }
  { // 11
    precise float4 v = Mul((abs(r1)).xyyy, c[7].xyyy);
    precise float s = rcp(r2.x);
    r3.xy = v.xy;
    r0.x = s;
    ps = s;
  }
  { // 12
    precise float4 v = r0.yyyy + -c[0].zzzz;
    precise float s = MulS(c[6].y, r0.x);
    r0.y = v.y;
    r1.z = s;
    ps = s;
  }
  { // 13
    precise float4 v = r0.zzzz + -c[1].zzzz;
    precise float s = rcp(c[0].x);
    r0.w = v.w;
    r0.z = s;
    ps = s;
  }
  { // 14
    precise float4 v = Mul(r1.zzzz, r0.zwww);
    precise float s = rcp(c[1].y);
    r2.xy = v.xy;
    r0.z = s;
    ps = s;
  }
  { // 15
    precise float4 v = Mul(r2.xyzz, r0.yzxx);
    r1.xyw = v.xyw;
  }
  { // 16
    precise float4 v = Mul(r1.xxxy, c[4].xxxy);
    r3.zw = v.zw;
  }
  { // 17
    precise float4 v = (c[11].wwwz == 0.0) ? r1.zxyy : c[11].zzzz;
    r2.xyzw = v.xyzw;
  }
  { // 18
    precise float4 v = r3.zxxx + r3.wyyy;
    r0.xy = v.xy;
  }
  { // 19
    precise float4 v = Mul(r0.yyyy, c[10].zzzz) + c[11].yyyy;
    r0.y = v.y;
  }
  { // 20
    precise float4 v = r0.xxxx + r1.wwww;
    precise float s = frac(r0.y);
    r0.x = v.x;
    r0.y = s;
    ps = s;
  }
  { // 21
    precise float4 v = Mul(r0.yyyy, c[10].xxxx) + c[10].wwww;
    r0.w = v.w;
  }
  { // 22
    precise float4 v = saturate(Dot4(c[2].zxyw, r2));
    precise float s = sin(r0.w);
    r0.y = v.y;
    r1.x = s;
    ps = s;
  }
  { // 23
    precise float4 v = saturate(Dot4(c[3].zxyw, r2));
    precise float s = cos(r0.w);
    r0.z = v.z;
    r1.y = s;
    ps = s;
  }
  { // 24
    precise float4 v = Mul(r1.xyyx, c[9]) + r0.yzyz;
    r2.xyzw = v.xyzw;
  }
  { // 25
    precise float4 v = Mul(r1.xyxy, c[8].xzwy) + r0.yyzz;
    r1.xyzw = v.xyzw;
  }
  r1.x = SampleShadowX(r1.xw);
  r1.y = SampleShadowX(r1.yz);
  r1.z = SampleShadowX(r2.xy);
  r1.w = SampleShadowX(r2.zw);
  { // 30
    precise float s = saturate(c[4].w + r0.x);
    r0.x = s;
    ps = s;
  }
  { // 31
    precise float4 v = float4(r1 >= r0.xxxx);
    r0.xyzw = v.xyzw;
  }
  { // 32
    precise float4 v = Dot4(r0.wzxy, c[10].yyyy);
    r0.x = v.x;
  }
  { // 33
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
