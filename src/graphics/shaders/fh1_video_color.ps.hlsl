// FH1 31511D87CC0C94B9 pixel program, modification 1; qualified at symmetric 1x and 2x.
SamplerState xe_samplers[] : register(s0);
Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);
cbuffer system_constants : register(b0) { uint4 xe_system[30]; };
cbuffer float_constants : register(b1) { float4 c[8]; };
cbuffer bool_constants : register(b2) { uint4 bools[10]; };
cbuffer fetch_constants : register(b3) { uint4 xe_fetch[48]; };
cbuffer descriptor_constants : register(b4) { uint4 descriptors[3]; };
struct Pixel { float4 color : SV_Target; uint coverage : SV_Coverage; };
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


uint Fetch(uint word) { return xe_fetch[word/4][word%4]; }
float SamplePlane(float2 coordinate, uint slot, uint sampler_index, uint unsigned_index, uint signed_index) {
  uint word=slot*6;
  float2 dimensions=float2((Fetch(word+2)&8191)+1,((Fetch(word+2)>>13)&8191)+1);
  float2 offset=(1.5/1024.0)/dimensions;
  coordinate=(xe_system[13].x & (1u<<slot)) ? mad(offset,0.5,coordinate) : coordinate+offset;
  float scale=exp2(float(SignExtend((Fetch(word+4)>>12)&1023,10))/32.0);
  float2 dx=ddx_coarse(coordinate)*scale,dy=ddy_coarse(coordinate)*scale;
  // Signs are packed four fetches per system word.
  uint sign=(xe_system[11+slot/16][(slot/4)%4] >> ((slot%4)*8)) & 3u;
  float value=0;
  if(sign!=1u) value=xe_textures_2d[unsigned_index].SampleGrad(xe_samplers[sampler_index],float3(coordinate,0),dx,dy).x;
  else value=xe_textures_2d[signed_index].SampleGrad(xe_samplers[sampler_index],float3(coordinate,0),dx,dy).x;
  if(sign==2u) value=value*2.0-1.0;
  else if(sign==3u) value=DecodeGammaSigned(value);
  int exponent=SignExtend((Fetch(word+3)>>13)&63,6);
  return value*asfloat(uint(exponent+127)<<23);
}
float3 Mul(float3 a,float3 b) { return min(abs(a),abs(b))==0 ? 0 : a*b; }
float MulS(float a,float b) { return min(abs(a),abs(b))==0 ? 0 : a*b; }
float Dot4(float4 a,float4 b) { precise float x=MulS(a.x,b.x); x+=MulS(a.y,b.y); x+=MulS(a.z,b.z); x+=MulS(a.w,b.w); return x; }
bool AlphaTest(float alpha) {
  uint compare=(xe_system[0].x>>7)&7u;
  float reference=asfloat(xe_system[14].x);
  uint result=0;
  if(alpha<reference) result|=compare&1u;
  if(alpha==reference) result|=compare&2u;
  if(alpha>reference) result|=compare&4u;
  return compare==7u || (compare==5u ? alpha!=reference : result!=0);
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


Pixel main(float4 uv:TEXCOORD0,float4 position:SV_Position) {
  precise float4 planes=float4(
    SamplePlane(uv.xy,2,descriptors[0].y,descriptors[0].z,descriptors[0].w),
    SamplePlane(uv.xy,1,descriptors[1].x,descriptors[1].y,descriptors[1].z),
    SamplePlane(uv.xy,0,descriptors[1].w,descriptors[2].x,descriptors[2].y),c[7].w);
  precise float3 r0=float3(Dot4(c[2].zxyw,planes.xzyw),Dot4(c[1].zxyw,planes.xzyw),Dot4(c[0].zxyw,planes.xzyw));
  precise float3 r1,r2,r3,r4;
  if(bools[1].x & 1u) {
    r1=Mul(r0.zyx,c[5].www)+c[6].yyy;
    r2=exp2(Mul(log2(abs(r1)),c[6].xxx));
    r1=Mul(r0,c[6].zzz);
    r0=(c[5].zzz>=r0).zyx ? r1.zyx : r2;
  } else {
    r1=Mul(r0.zyx,c[7].yyy)+c[7].xxx;
    r2=exp2(Mul(log2(abs(r1)),c[5].xxx));
    r1=Mul(r0,c[6].www);
    r0=(c[7].zzz>=r0).zyx ? r1.zyx : r2;
  }
  r3=Mul(r0,r0);r2=r0+r0;r1=Mul(r0,c[5].yyy);r4=Mul(r1,r0);
  r1=Mul(r3,r2)-r4;r0=r0-r4;r0=Mul(r3,r2)+r0;
  r0=Mul(r0,c[3].zzz)-r1;r0=Mul(r0,c[4].www)+c[4].xyz;
  Pixel output;
  output.color=float4(Mul(r0,c[3].xxx)+c[3].yyy,c[3].w);
  clip(AlphaTest(output.color.w)?1.0:-1.0);
  output.coverage=AlphaCoverage(output.color.w,position.xy);
  clip(output.coverage?1.0:-1.0);
  output.color*=asfloat(xe_system[15].y);
  return output;
}
