// FH1 E17BECBE8BE65806 RMS downsample, qualified for the symmetric 2x variant.
SamplerState samplers[] : register(s0);
Texture2DArray<float4> tex2[] : register(t0,space1);
cbuffer System : register(b0) { uint4 system[30]; };
cbuffer Constants : register(b1) { float4 c[1]; };
cbuffer Fetches : register(b3) { uint4 fetches[48]; };
cbuffer Descriptors : register(b4) { uint4 descriptors[1]; };
uint F(uint i) { return fetches[i/4][i%4]; }
int SX(uint v,uint n) { return int(v<<(32-n))>>(32-n); }
float Exponent(uint slot) { return exp2(float(SX((F(slot*6+3)>>13)&63,6))); }
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


uint4 Signs(uint slot) {
 uint packed=system[11+slot/16][(slot/4)%4]>>((slot%4)*8);
 return (packed>>uint4(0,2,4,6))&3u;
}
float4 Decode(float4 value,uint4 signs) {
 [unroll] for(uint i=0;i<4;++i) {
  if(signs[i]==2u) value[i]=value[i]*2.0-1.0;
  else if(signs[i]==3u) value[i]=DecodeGammaSigned(value[i]);
 }
 return value;
}
float4 Read2(float3 uv,float2 dx,float2 dy,uint samplerIndex,uint unsignedIndex,uint signedIndex,uint4 signs) {
 float4 value=0;
 if(any(signs!=1u)) value=tex2[unsignedIndex].SampleGrad(samplers[samplerIndex],uv,dx,dy);
 if(any(signs==1u)) {
  float4 signedValue=tex2[signedIndex].SampleGrad(samplers[samplerIndex],uv,dx,dy);
  value=signs==1u ? signedValue : value;
 }
 return value;
}
float4 SampleTap(float2 uv, float2 tap) {
 float2 dimensions=float2((F(2)&8191)+1,((F(2)>>13)&8191)+1);
 float2 offset=(tap+1.5/1024.0)/dimensions;
 uv=(system[13].x&1u) ? mad(offset,.5,uv) : uv+offset;
 float scale=exp2(float(SX((F(4)>>12)&1023,10))/32.0);
 uint4 signs=Signs(0);
 return Decode(Read2(float3(uv,0),ddx_coarse(uv)*scale,ddy_coarse(uv)*scale,
  descriptors[0].y,descriptors[0].z,descriptors[0].w,signs),signs)*Exponent(0);
}
[earlydepthstencil]
float4 main(float4 uv:TEXCOORD0):SV_Target {
 precise float4 r16=SampleTap(uv.xy,float2(-1.5,0.5));
 precise float4 r15=SampleTap(uv.xy,float2(-0.5,0.5));
 precise float4 r14=SampleTap(uv.xy,float2(1.5,-0.5));
 precise float4 r13=SampleTap(uv.xy,float2(0.5,-0.5));
 precise float4 r12=SampleTap(uv.xy,float2(0.5,0.5));
 precise float4 r11=SampleTap(uv.xy,float2(1.5,0.5));
 precise float4 r10=SampleTap(uv.xy,float2(-0.5,-0.5));
 precise float4 r9=SampleTap(uv.xy,float2(-1.5,-0.5));
 precise float4 r8=SampleTap(uv.xy,float2(-1.5,1.5));
 precise float4 r7=SampleTap(uv.xy,float2(-0.5,1.5));
 precise float4 r6=SampleTap(uv.xy,float2(1.5,-1.5));
 precise float4 r5=SampleTap(uv.xy,float2(0.5,-1.5));
 precise float4 r4=SampleTap(uv.xy,float2(0.5,1.5));
 precise float4 r3=SampleTap(uv.xy,float2(1.5,1.5));
 precise float4 r1=SampleTap(uv.xy,float2(-1.5,-1.5));
 precise float4 r2=SampleTap(uv.xy,float2(-0.5,-1.5));
 precise float3 sum=r3.xyz*r3.xyz;
 sum=r4.xyz*r4.xyz+sum;
 sum=r7.xyz*r7.xyz+sum;
 sum=r8.xyz*r8.xyz+sum;
 sum=r11.xyz*r11.xyz+sum;
 sum=r12.xyz*r12.xyz+sum;
 sum=r15.xyz*r15.xyz+sum;
 sum=r16.xyz*r16.xyz+sum;
 sum=r14.xyz*r14.xyz+sum;
 sum=r13.xyz*r13.xyz+sum;
 sum=r10.xyz*r10.xyz+sum;
 sum=r9.xyz*r9.xyz+sum;
 sum=r6.xyz*r6.xyz+sum;
 sum=r5.xyz*r5.xyz+sum;
 sum=r2.xyz*r2.xyz+sum;
 sum=r1.xyz*r1.xyz+sum;
 precise float alpha=r1.w;
 alpha=alpha+r2.w;
 alpha=alpha+r5.w;
 alpha=alpha+r6.w;
 alpha=alpha+r9.w;
 alpha=alpha+r10.w;
 alpha=alpha+r13.w;
 alpha=alpha+r14.w;
 alpha=alpha+r16.w;
 alpha=alpha+r15.w;
 alpha=alpha+r12.w;
 alpha=alpha+r11.w;
 alpha=alpha+r8.w;
 alpha=alpha+r7.w;
 alpha=alpha+r4.w;
 alpha=alpha+r3.w;
 precise float3 weighted=min(abs(sum),abs(c[0].xxx))==0 ? 0 : sum*c[0].x;
 precise float weightedAlpha=min(abs(alpha),abs(c[0].x))==0 ? 0 : alpha*c[0].x;
 return float4(sqrt(abs(weighted)),weightedAlpha)*asfloat(system[15].y);
}
