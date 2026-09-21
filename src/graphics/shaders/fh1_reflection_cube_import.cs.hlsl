// FH1 reflection cube import: tiled, endian 8-in-32 R10G10B10A2.
// Writes all six faces of one mip directly to the persistent host cube.
cbuffer Load : register(b0) {
  uint flags, guest_offset, guest_pitch, guest_z_stride;
  uint3 size_blocks;
  uint face_stride;
  uint unused_host_pitch, unused_height;
};
Buffer<uint4> source : register(t0);
RWTexture2DArray<uint4> dest : register(u0);

uint TiledOffset(uint x, uint y, uint pitch) {
  uint macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
  uint micro = ((x & 7) + ((y & 14) << 2)) << 2;
  uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

uint SourceOffset(uint2 p, uint scale) {
  uint result = TiledOffset(p.x, p.y, guest_pitch);
  if (scale == 2) {
    result = TiledOffset((p.x >> 3) * 4, p.y >> 1, guest_pitch) * 4 +
             (p.y & 1) * 16 + (p.x & 3) * 4 + (p.x & 4) * 8;
  }
  if (scale == 3) {
    uint2 host_group = uint2(p.x >> 2, p.y);
    uint2 guest_group = host_group / 3u;
    result = TiledOffset(guest_group.x * 4, guest_group.y, guest_pitch) * 9u +
             (((host_group.x - guest_group.x * 3u) * 3u +
               host_group.y - guest_group.y * 3u) << 4) + ((p.x & 3u) << 2);
  }
  return result;
}

uint Read32(uint byte_offset) {
  uint4 value = source[byte_offset >> 4];
  return value[(byte_offset >> 2) & 3];
}

uint Swap(uint value) {
  return (value << 24) | ((value & 0xFF00) << 8) |
         ((value >> 8) & 0xFF00) | (value >> 24);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= size_blocks.x || id.y >= size_blocks.y || id.z >= 6) return;
  uint scale = (flags >> 4) & 7;
  uint word = Swap(Read32(guest_offset + id.z * face_stride +
                          SourceOffset(id.xy, scale)));
  dest[id] = (word.xxxx >> uint4(0, 10, 20, 30)) & uint4(1023, 1023, 1023, 3);
}
