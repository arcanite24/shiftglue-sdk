// Reflection mip filter: endian 8-in-32, tiled R10G10B10A2, symmetric 3x.
// Addressing reuses texture_util::GetTiledOffset2D and the qualified load
// shader's groups of four host pixels; the 3x3 subpixel block of one guest
// texel spans four host groups of the scaled resolve layout.
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
// One guest group holds four guest texels of one guest row as nine 16-byte host
// groups (four host texels), stored column-major by host group coordinate.
uint ScaledOffset(uint2 p, uint pitch) {
  uint2 host_group=uint2(p.x>>2,p.y);
  uint2 guest_group=host_group/3u;
  return TiledOffset(guest_group.x*4u,guest_group.y,pitch)*9u+
    (((host_group.x-guest_group.x*3u)*3u+host_group.y-guest_group.y*3u)<<4)+
    ((p.x&3u)<<2);
}
uint Swap(uint v) { return (v<<24)|((v&0xFF00)<<8)|((v>>8)&0xFF00)|(v>>24); }
uint4 Unpack(uint raw) {
  uint word=Swap(raw);
  return (word.xxxx>>uint4(0,10,20,30))&uint4(1023,1023,1023,3);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if(id.x>=destination_side || id.y>=destination_side || id.z>=6) return;
  // The source level is 2x the destination level in guest texels, so one
  // destination texel averages the 3x3 subpixel block of one source texel.
  uint2 p=id.xy*3u;
  uint source_pitch=max(32u,source_side/3u);
  uint4 sum=0u;
  [unroll] for(uint dy=0u;dy<3u;++dy) {
    [unroll] for(uint dx=0u;dx<3u;++dx) {
      sum+=Unpack(pixels.Load(source_offset+id.z*source_face_stride+
        ScaledOffset(p+uint2(dx,dy),source_pitch)));
    }
  }
  uint4 value=(sum+4u)/9u;
  uint packed=value.x|(value.y<<10)|(value.z<<20)|(value.w<<30);
  pixels.Store(destination_offset+id.z*destination_face_stride+
    ScaledOffset(id.xy,max(32u,destination_side/3u)),Swap(packed));
}
