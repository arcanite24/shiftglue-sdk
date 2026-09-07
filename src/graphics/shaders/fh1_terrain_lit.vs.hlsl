// FH1 C34795A841E7DEFF / 3F terrain vertex shader.

ByteAddressBuffer xe_shared_memory_srv : register(t0);
#ifdef FH1_TERRAIN_OWNED_GEOMETRY
ByteAddressBuffer terrain_direction_srv : register(t1);
ByteAddressBuffer terrain_control_srv : register(t2);
#else
RWByteAddressBuffer xe_shared_memory_uav : register(u0);
#endif
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[25]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };

#include "fh1_terrain_vertex.hlsli"

#define c0 c[0]
#define c1 c[1]
#define c2 c[2]
#define c3 c[3]
#define c8 c[4]
#define c9 c[5]
#define c10 c[6]
#define c11 c[7]
#define c12 c[8]
#define c13 c[9]
#define c14 c[10]
#define c36 c[11]
#define c37 c[12]
#define c38 c[13]
#define c39 c[14]
#define c40 c[15]
#define c41 c[16]
#define c42 c[17]
#define c58 c[18]
#define c116 c[19]
#define c124 c[20]
#define c126 c[21]
#define c127 c[22]
#define c254 c[23]
#define c255 c[24]

float Dot4(float4 a,float4 b) { precise float4 v=Fh1Mul(a,b); precise float s=v.x+v.y; s=s+v.z; return s+v.w; }
float Dot3(float3 a,float3 b) { precise float3 v=Fh1Mul(a,b); precise float s=v.x+v.y; return s+v.z; }
struct Output { float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 position:SV_Position; };
Output main(uint vertex_id:SV_VertexID) {
 uint index=vertex_id==xe_system[0].w?0:vertex_id;
 index=ApplyEndian(index,xe_system[1].x);
 index=clamp((index+xe_system[1].y)&0xFFFFFFu,xe_system[1].z,xe_system[1].w);
 uint endian=xe_fetch[47].w&3u,address=(xe_fetch[47].z&~3u)+index*28u;
 precise float4 r0=0,r1=0,r2=0,r3=0,r4=0,r5=0,r6=0,r7=0,r8=0,r9=0;
 r0.x=float(index);
 r4=DecodeSnorm16x4(LoadVertex(address,endian)).yxwz;
 uint uv=LoadWord(address+8u,endian);r0.yz=float2(uv>>16,uv&65535u)*(1.0/65535.0);
 r1=DecodeSnorm16x4(LoadVertex(address+16u,endian)).yxwz;
 uint rgba=LoadWord(address+24u,endian);
 r2=max(float4(SignExtend(rgba,8),SignExtend(rgba>>8,8),SignExtend(rgba>>16,8),SignExtend(rgba>>24,8))*(1.0/127.0),-1.0);
 r3.xyz=-c41.yxz+c116.yxz;
 r6.xy=Fh1Mul(r0.yz,c124.yw)+c124.xz;
 r3.w=Dot4(r1.wzxy,r1.wzxy);
 r0.y=Fh1Mul(r4.w,c116.w);
 r2=Fh1Mul(r2,c255.zzzz)+c255.zzzz;
 r2=Fh1Mul(r2.wzyx,c126.yyyy)+c126.xxxx;
 r0.yzw=Fh1Mul(r0.yyy,r4.yxz);
 r5.xyz=r0.zyw+c116.xyz;r3.w=rsqrt(abs(r3.w));
 r1=Fh1Mul(r3.wwww,r1.yxzw);
 if(c127.x==0)r4.xyz=(-abs(r0.xxx)>c254.xxx)?1.0:0.0;
 else {uint v=LoadWord((xe_fetch[45].x&~3u)+index*4u,xe_fetch[45].y&3u);r4.xyz=DecodeSnorm101111(v)*2.0;}
 r7.xyz=Fh1Mul(c42.yxz,c255.yyy);
 r9.w=Dot4(r2.wzxy,c36.wzxy);
 r0.xyz=r3.xyz+r0.yzw;r0.xyz=Fh1Mul(r7.xyz,r0.xyz);
 r9.xyz=floor(r0.xzy);r7=max(r9,c254.xxxx);r3.xyz=min(r7.xyz,c254.yyy);
 r0.w=Fh1Mul(r3.y,c255.x)+Fh1Mul(r3.x,c255.y)+r3.z;
 uint control=(xe_fetch[44].z&~3u)+uint(int(floor(r0.w)))*32u;uint ce=xe_fetch[44].w&3u;
 r8=asfloat(Load4(control,ce));r3=asfloat(Load4(control+16u,ce));
 r8=r8.xywz-r3.xywz;r0.xyz=r0.xzy-r9.xyz;r3=Fh1Mul(r8,r0.xxxx)+r3.xywz;
 r6.zw=r3.zw-r3.xy;r0.xy=Fh1Mul(r6.zw,r0.yy)+r3.xy;
 r7.x=Dot4(r2.wzxy,c37.wzxy);r0.w=r0.y-r0.x;
 r0.x=saturate(Fh1Mul(r0.w,r0.z)+r0.x);r0.x=max(r0.x,c58.x);r0.x=min(r0.x,c58.y);
 r0.xw=saturate(Fh1Mul(r0.xx,c58.wz));
 r4.xyw=Fh1Mul(r0.xxx,r4.yzx)+r5.yzx;
 r3=Fh1Mul(r4.yyyy,c2)+c3;r3=Fh1Mul(r4.xxxx,c1.xzyw)+r3.xzyw;
 r3=Fh1Mul(r4.wwww,c0.xywz)+r3.xzwy;
 Output o=(Output)0;precise float4 position=r3.xywz;
 r5=Fh1Mul(r1.xyyx,r1.xxzw);r7.y=Dot4(r2.wzxy,c38.wzxy);r5.z=r5.z-r5.w;
 r5.xy=Fh1Mul(r1.zz,r1.wz)+r5.yx;r0.xyz=r5.yzx+r5.yzx;
 r1.xyz=Fh1Mul(r0.yyy,c14.xzy);r1.w=c255.w-r0.x;
 r1.xyz=Fh1Mul(r0.zzz,c13.xzy)+r1.xyz;r1.xyz=Fh1Mul(r1.www,c12.xyz)+r1.xzy;
 r0.y=Dot3(r1.zxy,r1.zxy);r7.z=Dot4(r2.wzxy,c39.wzxy);r0.y=rsqrt(abs(r0.y));
 r5.xyz=Fh1Mul(r4.yyy,c10.xyz)+c11.xyz;r4.xyz=Fh1Mul(r4.xxx,c9.xzy)+r5.xzy;
 o.t2.w=r7.w;o.t4.xyz=max(r7.xyz,c254.xxx);o.t2.xyz=Fh1Mul(r4.www,c8.xyz)+r4.xzy;
 o.t3.x=-abs(r0.x)>0.0?1.0:0.0;o.t0.xy=r6.xy;o.t5=r3.xywz;
 o.t1.xyz=Fh1Mul(r1.xyz,r0.yyy);o.t1.w=r0.w;o.t4.w=saturate(Dot4(r2.wzxy,c40.wzxy));
 if((xe_system[0].x&8u)==0)position.w=1.0/position.w;
 if(xe_system[0].x&2u)position.xy*=position.w;if(xe_system[0].x&4u)position.z*=position.w;
 precise float3 scaled=position.xyz*asfloat(xe_system[8].xyz);position.xyz=mad(asfloat(xe_system[9].xyz),position.w,scaled);
 o.position=position;return o;
}
