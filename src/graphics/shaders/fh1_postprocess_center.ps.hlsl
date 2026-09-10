// Experimental 614588 center-sample post-process. Admission is disabled.
// Remove neighboring scene blur while retaining live color and fetch semantics.
SamplerState samplers[] : register(s0);
Texture2DArray<float4> tex2[] : register(t0,space1);
Texture3D<float4> tex3[] : register(t0,space2);
cbuffer System : register(b0) { uint4 system[30]; };
cbuffer Constants : register(b1) { float4 c[16]; }; // c32..44,253..255
cbuffer Fetches : register(b3) { uint4 fetches[48]; };
cbuffer Descriptors : register(b4) { uint4 descriptors[4]; };
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
float4 Sample2(float2 uv,uint slot,uint samplerIndex,uint unsignedIndex,uint signedIndex) {
 uint w=slot*6;
 float2 dimensions=float2((F(w+2)&8191)+1,((F(w+2)>>13)&8191)+1);
 float2 offset=(1.5/1024.0)/dimensions;
 uv+=(system[13].x&(1u<<slot)) ? offset*.5 : offset;
 float scale=exp2(float(SX((F(w+4)>>12)&1023,10))/32.0);
 uint4 signs=Signs(slot);
 return Decode(Read2(float3(uv,0),ddx_coarse(uv)*scale,ddy_coarse(uv)*scale,samplerIndex,unsignedIndex,signedIndex,signs),signs)*Exponent(slot);
}
float3 SampleLut(float3 uv) {
 bool volume=((F(47)>>9)&3u)==2u;
 uint size=F(44);
 float3 dimensions=volume ? float3((size&2047)+1,((size>>11)&2047)+1,((size>>22)&1023)+1)
                          : float3((size&8191)+1,((size>>13)&8191)+1,((size>>26)&63)+1);
 float3 offset=(1.5/1024.0)/dimensions;
 uv.xy+=(system[13].x&128u) ? offset.xy*.5 : offset.xy;
 uv.z=volume ? uv.z+offset.z : uv.z*dimensions.z+(1.5/1024.0);
 float scale=exp2(float(SX((F(46)>>12)&1023,10))/32.0);
 float3 dx=ddx_coarse(uv)*scale,dy=ddy_coarse(uv)*scale;
 uint4 signs=Signs(7); signs.w=signs.x;
 float4 value=0;
 if(volume) {
  if(any(signs!=1u)) value=tex3[descriptors[3].x].SampleGrad(samplers[descriptors[2].w],uv,dx,dy);
  if(any(signs==1u)) {
   float4 signedValue=tex3[descriptors[3].y].SampleGrad(samplers[descriptors[2].w],uv,dx,dy);
   value=signs==1u ? signedValue : value;
  }
 } else {
  bool minifying=max(dx.z,dy.z)*dimensions.z>1.0;
  uint linearFilter=(F(46)>>(minifying ? 1 : 0))&1u;
  float weight=0;
  if(linearFilter) { uv.z-=.5; weight=frac(uv.z); }
  uv.z=floor(uv.z);
  value=Read2(uv,dx.xy,dy.xy,descriptors[2].w,descriptors[3].z,descriptors[3].w,signs);
  if(weight!=0) {
   uv.z+=1;
   value=lerp(value,Read2(uv,dx.xy,dy.xy,descriptors[2].w,descriptors[3].z,descriptors[3].w,signs),weight);
  }
 }
 return Decode(value,signs).xyz*Exponent(7);
}
[earlydepthstencil]
float4 main(float4 uv:TEXCOORD0,float4 info:TEXCOORD1,float4 noise:TEXCOORD2):SV_Target {
 float3 center=Sample2(uv.xy,0,descriptors[1].w,descriptors[1].y,descriptors[1].z).xyz;
 float3 glow=Sample2(uv.zw,2,descriptors[2].x,descriptors[2].y,descriptors[2].z).xyz;
 float3 scene=center*center*(5*c[15].w)+glow*glow;
 float exposure=exp2(info.w);
 float3 exposed=scene*exposure;
 float3 tone=(exposed*(scene*(c[11].y*exposure)+c[12].x)+c[12].z)
            /(exposed*(scene*(c[11].y*exposure)+c[11].z)+c[12].y)-c[12].w;
 float3 lookup=SampleLut(tone*(c[4].x/info.z)+c[5].x);
 float3 color=(c[6].x>uv.x) ? lookup : tone/info.z;
 float2 p=info.xy+c[3].xy;
 float2 v=float2(p.x*c[0].x+p.y*c[0].y,p.x*c[0].z+p.y*c[0].w);
 float vignette=saturate(exp2(log2(abs(dot(v,v)+c[13].y))*c[1].w))*c[2].w;
 color=sqrt(abs(color));
 color+=vignette*(saturate(color+c[14].w+c[2].xyz)-color);
 float3 square=color*color;
 float3 result=float3(dot(c[7].xyz*color,color)+c[7].w,
                      dot(c[8].xyz,square)+c[8].w*c[14].z,
                      dot(c[9].xyz,square)+c[9].w*c[14].z);
 return float4(sqrt(saturate(result)),1)*asfloat(system[15].y);
}
