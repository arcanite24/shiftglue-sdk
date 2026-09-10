// Reflection mip filter: endian 8-in-32, tiled R10G10B10A2, symmetric 2x.
// Addressing reuses texture_util::GetTiledOffset2D and fh1_scaled_32bpp_2x.cs.hlsl.
RWByteAddressBuffer pixels : register(u0);
cbuffer Mip : register(b0) {
  uint source_offset, destination_offset, source_side, destination_side;
  uint source_face_stride, destination_face_stride;
};
uint TiledOffset(uint x, uint y, uint pitch) {
  uint macro=((x>>5)+(y>>5)*(pitch>>5))<<9;
  uint micro=((x&7)+((y&14)<<2))<<2;
  uint offset=macro+((micro&~15u)<<1)+(micro&15)+((y&1)<<4);
  return ((offset&~511u)<<3)+((y&16)<<7)+((offset&448)<<2)+
    (((((y&8)>>2)+(x>>3))&3)<<6)+(offset&63);
}
uint ScaledOffset(uint2 p, uint pitch) {
  return TiledOffset((p.x>>3)*4,p.y>>1,pitch)*4+(p.y&1)*16+(p.x&3)*4+(p.x&4)*8;
}
uint Swap(uint v) { return (v<<24)|((v&0xFF00)<<8)|((v>>8)&0xFF00)|(v>>24); }
uint4 Unpack(uint raw) {
  uint word=Swap(raw);
  return (word.xxxx>>uint4(0,10,20,30))&uint4(1023,1023,1023,3);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if(id.x>=destination_side || id.y>=destination_side || id.z>=6) return;
  // Each aligned 2x2 block has offsets 0,4,16,20 in the qualified scaled layout.
  // Avoid the observed optimized-FXC miscompile of repeated neighbor-coordinate expressions.
  uint input=source_offset+id.z*source_face_stride+ScaledOffset(id.xy*2,max(32u,source_side>>1));
  uint4 sum=Unpack(pixels.Load(input))+Unpack(pixels.Load(input+4))+
            Unpack(pixels.Load(input+16))+Unpack(pixels.Load(input+20));
  uint4 value=(sum+1+((sum>>2)&1))>>2;
  uint packed=value.x|(value.y<<10)|(value.z<<20)|(value.w<<30);
  pixels.Store(destination_offset+id.z*destination_face_stride+ScaledOffset(id.xy,max(32u,destination_side>>1)),Swap(packed));
}
