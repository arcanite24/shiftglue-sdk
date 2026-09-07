SamplerState samplers[]:register(s0);
Texture2DArray<float4> tex2d[]:register(t0,space1);
TextureCube<float4> cubes[]:register(t0,space3);
cbuffer System:register(b0){uint4 xe_system[30];};
cbuffer Floats:register(b1){float4 c[17];};
cbuffer Bools:register(b2){uint4 bool_loop[10];};
cbuffer Fetch:register(b3){uint4 xe_fetch[48];};
cbuffer Descriptors:register(b4){uint4 descriptors[3];};
int SignExtend(uint value, uint bits) {
  const uint shift = 32 - bits;
  return int(value << shift) >> shift;
}

float DecodeGammaSigned(float value) {
  value = saturate(value) * 261120.0;
  float scale, bias;
  if (value >= 98304.0) {
    const bool high = value >= 196608.0;
    scale = high ? 1.0 / 128.0 : 1.0 / 256.0;
    bias = high ? -1024.0 : -256.0;
  } else {
    const bool high = value >= 65536.0;
    scale = high ? 1.0 / 512.0 : 1.0 / 1024.0;
    bias = high ? -64.0 : 0.0;
  }
  value = value * scale + bias;
  return (value + trunc(value * scale)) / 1023.0;
}

float DecodeSample(float value, uint sign, uint fetch_word) {
  if (sign == 2u) value = value * 2.0 - 1.0;
  else if (sign == 3u) value = DecodeGammaSigned(value);
  const int exponent = SignExtend((fetch_word >> 13) & 0x3Fu, 6);
  return value * asfloat(uint(exponent + 127) << 23);
}

float4 Mul(float4 a, float4 b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float MulS(float a, float b) { return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b; }
float Dot3(float4 a, float4 b) { precise float r = MulS(a.x,b.x); r += MulS(a.y,b.y); r += MulS(a.z,b.z); return r; }
float Dot4(float4 a, float4 b) { precise float r = Dot3(a,b); r += MulS(a.w,b.w); return r; }

bool AlphaTest(float alpha) {
  const uint compare = (xe_system[0].x >> 7) & 7u;
  const float reference = asfloat(xe_system[14].x);
  switch (compare) {
    case 0u: return false;
    case 1u: return alpha < reference;
    case 2u: return alpha == reference;
    case 3u: return alpha <= reference;
    case 4u: return alpha > reference;
    case 5u: return alpha != reference;
    case 6u: return alpha >= reference;
    default: return true;
  }
}

uint AlphaCoverage(float alpha, float2 position) {
  const uint alpha_to_mask = xe_system[14].y;
  uint coverage = ~0u;
  if (alpha_to_mask) {
    const uint pattern =
        (uint(position.y) & 1u) | ((uint(position.x) & 1u) << 1);
    const float offset = float((alpha_to_mask >> (pattern << 1)) & 3u);
    coverage = 0;
    if (xe_system[13].w && xe_system[13].z) {
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .75) ? 1u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .25) ? 2u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, .50) ? 4u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 16.0, 1.0) ? 8u : 0u;
    } else if (xe_system[13].w) {
      coverage |= alpha >= mad(offset, -1.0 / 8.0, .5) ? 2u : 0u;
      coverage |= alpha >= mad(offset, -1.0 / 8.0, 1.0) ? 1u : 0u;
    } else {
      coverage = alpha >= mad(offset, -1.0 / 4.0, 1.0) ? 1u : 0u;
    }
  }
  return coverage;
}

#define c42 c[0]
#define c43 c[1]
#define c47 c[2]
#define c57 c[3]
#define c59 c[4]
#define c113 c[5]
#define c124 c[6]
#define c125 c[7]
#define c126 c[8]
#define c127 c[9]
#define c156 c[10]
#define c157 c[11]
#define c159 c[12]
#define c168 c[13]
#define c169 c[14]
#define c254 c[15]
#define c255 c[16]

uint F(uint tf,uint word){uint i=tf*6+word;return xe_fetch[i/4][i%4];}
uint D(uint i){return descriptors[i/4][i%4];}
float Gradient13(float2 coord){
 uint size=F(13,2);float2 extent=float2((size&8191u)+1u,((size>>13)&8191u)+1u);
 float2 offset=(513.5/1024.0)/extent;
 coord=(xe_system[13].x&8192u)?mad(offset,0.5,coord):coord+offset;
 float scale=exp2(float(SignExtend((F(13,4)>>12)&1023u,10))/32.0);
 float2 dx=ddx_coarse(coord)*scale,dy=ddy_coarse(coord)*scale;
 uint sign=(xe_system[11].w>>8)&3u;
 float v=tex2d[sign==1u?D(3):D(2)].SampleGrad(samplers[D(1)],float3(coord,0),dx,dy).x;
 return DecodeSample(v,sign,F(13,3));
}
float4 CubeOp(float3 v){
 float4 r;
 if(abs(v.y)>=abs(v.x)&&abs(v.y)>=abs(v.z)){bool neg=v.y<0;r=float4(-v.z,neg?-v.x:v.x,2.0*v.y,neg?5.0:4.0);}
 else if(abs(v.z)>=abs(v.x)){bool neg=v.z<0;r=float4(neg?-v.y:v.y,v.x,2.0*v.z,neg?3.0:2.0);}
 else {bool neg=v.x<0;r=float4(-v.z,neg?v.y:-v.y,2.0*v.x,neg?1.0:0.0);}
 return r;
}
float3 CubeSample(float3 coord,uint tf,uint descriptor){
 uint size=F(tf,2);float2 extent=float2((size&8191u)+1u,((size>>13)&8191u)+1u);
 float2 offset=(1.5/1024.0)/extent;
 precise float2 uv=(xe_system[13].x&(1u<<tf))?mad(offset,0.5,coord.xy):coord.xy+offset;
 uv=mad(uv,2.0,-3.0);
 uint face=min(uint(coord.z),5u);float3 v=float3(uv,0);
 if((face>>1)==0){v.y=-v.y;v.z=(face&1)?v.x:-v.x;v.x=(face&1)?-1.0:1.0;}
 else if((face>>1)==1){v.z=(face&1)?-v.y:v.y;v.y=(face&1)?-1.0:1.0;}
 else {v.x=(face&1)?-v.x:v.x;v.y=-v.y;v.z=(face&1)?-1.0:1.0;}
 float scale=exp2(float(SignExtend((F(tf,4)>>12)&1023u,10))/32.0);
 float3 dx=ddx_coarse(v)*scale,dy=ddy_coarse(v)*scale;
 uint word=xe_system[11+tf/16][(tf%16)/4]>>((tf%4)*8);
 uint3 signs=uint3(word&3u,(word>>2)&3u,(word>>4)&3u);
 bool3 signed_channels=signs==1u;float3 value=0;
 if(!all(signed_channels))value=cubes[D(descriptor+1)].SampleGrad(samplers[D(descriptor)],v,dx,dy).xyz;
 if(any(signed_channels)){float3 sv=cubes[D(descriptor+2)].SampleGrad(samplers[D(descriptor)],v,dx,dy).xyz;value=signed_channels?sv:value;}
 return float3(DecodeSample(value.x,signs.x,F(tf,3)),DecodeSample(value.y,signs.y,F(tf,3)),DecodeSample(value.z,signs.z,F(tf,3)));
}
struct Output{float4 color:SV_Target;uint coverage:SV_Coverage;};
Output main(float4 t0:TEXCOORD0,centroid float4 t1:TEXCOORD1,centroid float4 t2:TEXCOORD2,float4 t3:TEXCOORD3,centroid float4 t4:TEXCOORD4,float4 t5:TEXCOORD5,float4 pos:SV_Position){
 precise float4 r0=t0,r1=t1,r2=t2,r3=t3,r4=t4,r5=t5,r6=0,r7=0;
 r0.x=MulS(r5.y,c254.y);r0.z=Dot3(r1.zxyy,r1.zxyy);r0.y=rcp(r5.w);
 r0.x=MulS(r0.x,r0.y);r3.w=rsqrt(abs(r0.z));r3.xyz=Mul(r3.wwww,c43.xyzz).xyz;
 r0.z=Dot3(r3.zxyy,r1.zxyy);r0.yw=Mul(r0.zzzy,c254.yyyy).yw;
 r0.z=MulS(r0.w,r5.x);r5.xzw=(r0.xyyz+c254.yyyy).xzw;
 r6.x=r4.w-c254.w;r5.y=c254.w-r5.x;r5.x=Gradient13(r5.wy);
 r5.y=max(c168.x,c127.x);r7.w=c254.w;
 r3.xyz=Mul(r2.wwww,c42.zyxx).xyz;
 r0.yzw=-r2.xyz+c59.xyz;r2.x=c254.w-c57.z;
 r0.x=Dot3(r0.wyzz,r0.wyzz);r5.w=-c57.z+r5.x;
 r7.xyz=Mul(r3.wwww,r1.xzyy).xyz;r2.x=rcp(r2.x);
 r5.w=saturate(MulS(r5.w,r2.x));r2.y=max(r5.y,r5.w);r5.y=saturate(MulS(r1.y,r3.w));
 r5.w=max(r5.w,c127.x);r0.x=rsqrt(abs(r0.x));
 r0.xyz=Mul(r0.ywzz,-r0.xxxx).xyz;r5.y=log2(r5.y);
 r2.x=max(r5.w,c124.x);r5.w=MulS(c159.x,r5.y);
 r0.w=Dot3(r0.yxzz,r7.yxzz);r5.w=exp2(r5.w);
 r1.x=Dot4(r0.yxzw,r7.yxzw);r0.xyz=(Mul(-r7.xzyy,r1.xxxx)+r0.xzyy).xyz;
 r6.yzw=-r0.xzy+r0.xzy;r5.y=saturate(MulS(r5.z,r5.z));
 r5.x=MulS(r6.x,c125.x)+c254.w;r5.z=max(r5.y,c157.x);r1.z=MulS(c113.z,r5.w);
 r0.xyz=(Mul(r6.yzww,c125.xxxx)+r0.xzyy).xyz;r0=CubeOp(r0.xyz);r1.y=MulS(c113.y,r5.w);
 r6.xyz=-r3.zyx+r5.zzz;r5.z=MulS(c169.x,r5.y);
 r2.xy=Mul(r5.xzzz,r2.xyyy).xy;r1.x=MulS(c113.x,r5.w);
 r5.xyz=(Mul(r6.xyzz,c156.xxxx)+r3.zyxx).xyz;r5.xyz+=r4.xyz;r5.w=rcp(abs(r0.z));
 r0.xy=(Mul(r0.yxxx,r5.wwww)+c254.zzzz).xy;
 if((bool_loop[1].x&4u)==0)r0.xyz=CubeSample(r0.xyw,1,4);else r0.xyz=CubeSample(r0.xyw,2,7);
 r0.xyz=Mul(r0.xyzz,r0.xyzz).xyz;r0.xyz=Mul(r0.xyzz,c126.xxxx).xyz;r0.xyz=(Mul(r0.xyzz,r2.xxxx)+r2.yyyy).xyz;
 r5.xyz+=r1.xyz;r5.xyz=max(r5.xyz,c254.xxx);r5.xyz+=c255.xxx;
 r5.xyz=(Mul(r5.xyzz,c125.xxxx)+c254.wwww).xyz;r5.xyz=Mul(r5.xyzz,r0.xyzz).xyz;
 r0.xyz=-r5.xyz+c47.xyz;r5.xyz=(Mul(r0.xyzz,c47.wwww)+r5.xyzz).xyz;
 float4 color=float4(sqrt(abs(r5.xyz)),c57.x);
 clip(AlphaTest(color.w)?1.0:-1.0);Output o;o.coverage=AlphaCoverage(color.w,pos.xy);clip(o.coverage?1.0:-1.0);
 o.color=color*asfloat(xe_system[15].y);return o;
}
