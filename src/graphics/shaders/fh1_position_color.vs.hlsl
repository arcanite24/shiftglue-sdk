// FH1 1E6883FCCDE1F688 / modifications 0 and 1.
ByteAddressBuffer xe_shared_memory_srv : register(t0);
RWByteAddressBuffer xe_shared_memory_uav : register(u0);

cbuffer xe_system_cbuffer : register(b0) {
  uint4 xe_system[30];
};


cbuffer xe_fetch_cbuffer : register(b3) {
  uint4 xe_fetch[48];
};

uint Swap8In16(uint value) {
  return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
}

uint ApplyEndian(uint value, uint endian) {
  if (endian == 1 || endian == 2) {
    value = Swap8In16(value);
  }
  if (endian == 2 || endian == 3) {
    value = (value << 16) | (value >> 16);
  }
  return value;
}

uint3 Load3(uint address, uint endian) {
  uint3 value;
  [branch]
  if (xe_system[0].x & 1) {
    value = xe_shared_memory_uav.Load3(address);
  } else {
    value = xe_shared_memory_srv.Load3(address);
  }

  return uint3(ApplyEndian(value.x, endian), ApplyEndian(value.y, endian),
               ApplyEndian(value.z, endian));
}

struct Fh1PositionVertex {
#ifndef FH1_POSITION_ONLY
float4 color:TEXCOORD0;
#endif
float4 position:SV_Position;
};
Fh1PositionVertex main(uint vertex_id:SV_VertexID) {
  uint index = vertex_id == xe_system[0].w ? 0 : vertex_id;
  const uint index_endian = xe_system[1].x;
  if (index_endian == 1 || index_endian == 2) {
    index = Swap8In16(index);
  }
  if (index_endian == 2 || index_endian == 3) {
    index = (index << 16) | (index >> 16);
  }
  index = clamp((index + xe_system[1].y) & 0x00FFFFFF,
                xe_system[1].z, xe_system[1].w);

  const uint vertex_endian = xe_fetch[0].y & 3;
  const uint address =
      (xe_fetch[0].x & ~3) + index * 28u;
  const float4 local_position = float4(asfloat(Load3(address, vertex_endian)),1.0);
  precise float4 position = local_position;
#ifndef FH1_POSITION_ONLY
  uint4 color;
  if (xe_system[0].x & 1u) color = xe_shared_memory_uav.Load4(address + 12u);
  else color = xe_shared_memory_srv.Load4(address + 12u);
  color = uint4(ApplyEndian(color.x,vertex_endian),ApplyEndian(color.y,vertex_endian),ApplyEndian(color.z,vertex_endian),ApplyEndian(color.w,vertex_endian));
#endif
  if (!(xe_system[0].x & 8)) {
    position.w = 1.0 / position.w;
  }
  if (xe_system[0].x & 2) {
    position.xy *= position.w;
  }
  if (xe_system[0].x & 4) {
    position.z *= position.w;
  }
  precise float3 scaled_position =
      position.xyz * asfloat(xe_system[8].xyz);
  position.xyz = mad(asfloat(xe_system[9].xyz), position.w,
                     scaled_position);
  Fh1PositionVertex output;
#ifndef FH1_POSITION_ONLY
  output.color=asfloat(color);
#endif
  output.position=position;return output;
}
