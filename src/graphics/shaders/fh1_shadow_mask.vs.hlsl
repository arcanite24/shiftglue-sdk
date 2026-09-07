// Exact FH1 shadow vertex A3B9ED5D5C87230E / modification1.
ByteAddressBuffer xe_shared_memory_srv : register(t0);
#ifndef FH1_SCENE_OWNED_GEOMETRY
RWByteAddressBuffer xe_shared_memory_uav : register(u0);
#endif

cbuffer xe_system_cbuffer : register(b0) {
  uint4 xe_system[30];
};

cbuffer xe_float_cbuffer : register(b1) {
  float4 c[4];
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
#ifdef FH1_SCENE_OWNED_GEOMETRY
  // Root SRVs lack bounds. Match each component of the bounded shared Load3.
  uint3 value = uint3(0, 0, 0);
  [branch]
  if (address <= 0x20000000u - 12u) {
    value = xe_shared_memory_srv.Load3(address);
  } else {
    if (address < 0x20000000u) value.x = xe_shared_memory_srv.Load(address);
    if (address < 0x20000000u - 4u) value.y = xe_shared_memory_srv.Load(address + 4u);
  }
#else
  uint3 value;
  [branch]
  if (xe_system[0].x & 1) {
    value = xe_shared_memory_uav.Load3(address);
  } else {
    value = xe_shared_memory_srv.Load3(address);
  }
#endif

  return uint3(ApplyEndian(value.x, endian), ApplyEndian(value.y, endian),
               ApplyEndian(value.z, endian));
}

float Fh1Mul(float a, float b) {
  return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b;
}

float Fh1Dot4(float4 a, float4 b) {
  precise float result = Fh1Mul(a.z, b.z);
  result = result + Fh1Mul(a.x, b.x);
  result = result + Fh1Mul(a.y, b.y);
  result = result + Fh1Mul(a.w, b.w);
  return result;
}

struct ShadowVertex {float4 projected:TEXCOORD0;float4 position:SV_Position;};
ShadowVertex main(uint vertex_id:SV_VertexID) {
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

  const uint vertex_endian = xe_fetch[47].w & 3;
  const uint address =
      (xe_fetch[47].z & ~3) + index * 12u;
  const float4 local_position = float4(asfloat(Load3(address, vertex_endian)),1.0);
  precise float4 projected = float4(Fh1Dot4(c[0],local_position),Fh1Dot4(c[1],local_position),Fh1Dot4(c[2],local_position),Fh1Dot4(c[3],local_position));
  precise float4 position = projected;
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
  ShadowVertex output;output.projected=projected;output.position=position;return output;
}
