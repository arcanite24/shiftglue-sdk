// FH1 shadow mask 26EB620936001876 / 0000400300000001.
#define FH1_SHADOW_CONSTANT_COUNT 13
#include "fh1_shadow_sampling.hlsli"

[earlydepthstencil]
float4 main(float4 projected:TEXCOORD0, float4 position:SV_Position):SV_Target {
 precise float4 r0=projected,r1=float4(abs(floor(position.xy)*FH1_SHADOW_PIXEL_SCALE),0,0),r2=0,r3=0,oC0=0;
 precise float ps=0;
  { // 3
    precise float4 v = (MulS(((abs(r1)).xyyy).x, (c[7].xyyy).x) + MulS(((abs(r1)).xyyy).y, (c[7].xyyy).y)) + (c[10].xxxx).x;
    r1.y = v.y;
  }
  { // 4
    precise float4 v = -c[6].xxxx + c[11].zzzz;
    precise float s = rcp(r0.w);
    r3.x = v.x;
    r1.x = s;
    ps = s;
  }
  { // 5
    precise float4 v = Mul(r1.xxxy, c[11]);
    r2.xyzw = v.xyzw;
  }
  { // 6
    precise float4 v = Mul(r2.xzzy, r0.yxyx);
    r1.xyzw = v.xyzw;
  }
  { // 7
    precise float4 v = r1.xwww + c[11].yyyy;
    r0.xy = v.xy;
  }
  r0.x = SampleGradientX(r0.yx);
  { // 9
    precise float4 v = r3.xxxx + -r0.xxxx;
    r0.x = v.x;
  }
  { // 10
    precise float4 v = Mul(c[6].zzzz, c[6].yyyy);
    precise float s = rcp(r0.x);
    r1.x = v.x;
    r0.x = s;
    ps = s;
  }
  { // 11
    precise float4 v = r2.wwww + c[11].yyyy;
    precise float s = MulS(c[6].y, r0.x);
    r0.z = v.z;
    r0.w = s;
    ps = s;
  }
  { // 12
    precise float4 v = r1.yyyy + -c[0].zzzz;
    precise float s = frac(r0.z);
    r0.y = v.y;
    r0.z = s;
    ps = s;
  }
  { // 13
    precise float4 v = r1.zzzz + -c[1].zzzz;
    precise float s = rcp(c[0].x);
    r1.z = v.z;
    r1.y = s;
    ps = s;
  }
  { // 14
    precise float4 v = Mul(r0.zzzz, c[10].wwww) + c[12].xxxx;
    r2.w = v.w;
  }
  { // 15
    precise float4 v = Mul(r0.wwww, r1.yyzz);
    precise float s = rcp(c[1].y);
    r1.yz = v.yz;
    r0.z = s;
    ps = s;
  }
  { // 16
    precise float4 v = Mul(r1.yzxx, r0.yzxx);
    r0.xyz = v.xyz;
  }
  { // 17
    precise float4 v = (c[10].xxxy == 0.0) ? r0.wxyy : c[11].zzzz;
    r1.xyzw = v.xyzw;
  }
  { // 18
    precise float4 v = saturate(Dot4(c[2].zxyw, r1));
    precise float s = sin(r2.w);
    r2.x = v.x;
    r2.z = s;
    ps = s;
  }
  { // 19
    precise float4 v = saturate(Dot4(c[3].zxyw, r1));
    precise float s = cos(r2.w);
    r2.y = v.y;
    r2.w = s;
    ps = s;
  }
  { // 20
    precise float4 v = Mul(r2.zwwz, c[9]) + r2.xyxy;
    r3.xyzw = v.xyzw;
  }
  { // 21
    precise float4 v = Mul(r2.zwzw, c[8].xzwy) + r2.xxyy;
    r2.xyzw = v.xyzw;
  }
  r2.x = SampleShadowX(r2.xw);
  r2.y = SampleShadowX(r2.yz);
  r2.z = SampleShadowX(r3.xy);
  r2.w = SampleShadowX(r3.zw);
  { // 26
    precise float4 v = saturate(Dot4(c[4].zxyw, r1));
    r1.x = v.x;
  }
  { // 27
    precise float4 v = float4(r2 >= r1.xxxx);
    precise float s = saturate(c[6].w + r0.z);
    r1.xyzw = v.xyzw;
    r0.x = s;
    ps = s;
  }
  { // 28
    precise float4 v = Dot4(r1.wzxy, c[10].zzzz);
    precise float s = c[5].w + r0.x;
    r0.y = v.y;
    r0.x = s;
    ps = s;
  }
  { // 29
    precise float4 v = r0.xxxx + r0.yyyy;
    oC0.x = v.x;
    oC0.y = 1.0;
    oC0.z = 0.0;
    oC0.w = 0.0;
  }
  return oC0 * asfloat(xe_system[15].y);
}
