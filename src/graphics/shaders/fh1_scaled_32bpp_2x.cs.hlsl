// Qualified specialization: tiled 2D, 32 bits per pixel, endian 8-in-32, symmetric 2x.
// Existing resolve buffer preserves partial updates and history; admission is in texture_cache.cpp.
cbuffer Load : register(b0) {
 uint flags, guest_offset, guest_pitch, guest_z_stride;
 uint3 size_blocks; uint host_offset;
 uint host_pitch, height_texels;
};
Buffer<uint4> source : register(t0);
// Direct-output experiment failed retention; this variant is not bound at runtime.
#ifdef FH1_DIRECT_TEXTURE_OUTPUT
RWTexture2D<uint4> dest : register(u0);
#else
RWBuffer<uint4> dest : register(u0);
#endif
uint TiledOffset(uint x, uint y) {
 uint macro=((x>>5)+(y>>5)*(guest_pitch>>5))<<9;
 uint micro=((x&7)+((y&14)<<2))<<2;
 uint offset=macro+((micro&~15u)<<1)+(micro&15)+((y&1)<<4);
 return ((offset&~511u)<<3)+((y&16)<<7)+((offset&448)<<2)+
   (((((y&8)>>2)+(x>>3))&3)<<6)+(offset&63);
}
uint4 Swap(uint4 v) {
 return (v<<24)|((v&0xFF00)<<8)|((v>>8)&0xFF00)|(v>>24);
}
[numthreads(4,32,1)]
void main(uint3 tid : SV_DispatchThreadID) {
 uint x=tid.x*8, y=tid.y;
 if(x>=size_blocks.x || y>=size_blocks.y) return;
 uint input=(guest_offset+TiledOffset(tid.x*4,y>>1)*4+(y&1)*16)>>4;
 uint output=(host_offset+y*host_pitch+x*4)>>4;
#ifdef FH1_DIRECT_TEXTURE_OUTPUT
 uint4 a=Swap(source[input]), b=Swap(source[input+2]);
 [unroll] for(uint i=0;i<4;++i) {
  dest[uint2(x+i,y)]=uint4(a[i]&1023,(a[i]>>10)&1023,(a[i]>>20)&1023,a[i]>>30);
  dest[uint2(x+4+i,y)]=uint4(b[i]&1023,(b[i]>>10)&1023,(b[i]>>20)&1023,b[i]>>30);
 }
#else
 dest[output]=Swap(source[input]);
 dest[output+1]=Swap(source[input+2]);
#endif
}
