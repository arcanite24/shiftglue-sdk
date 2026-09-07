// FH1 4D5309C9 vertex shader D25C81F5BF6DD4C6, admitted only with the
// 57ACCCFC063672EC event-scene pixel shader (never the map/HUD variants).

ByteAddressBuffer xe_shared_memory_srv : register(t0);
RWByteAddressBuffer xe_shared_memory_uav : register(u0);
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[4]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };

uint Swap8In16(uint value) {
  return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
}

uint ApplyEndian(uint value, uint endian) {
  if (endian == 1 || endian == 2) value = Swap8In16(value);
  if (endian == 2 || endian == 3) value = (value << 16) | (value >> 16);
  return value;
}

uint Load(uint address, uint endian) {
  const uint value = (xe_system[0].x & 1)
                         ? xe_shared_memory_uav.Load(address)
                         : xe_shared_memory_srv.Load(address);
  return ApplyEndian(value, endian);
}

struct Fh1EventMeshVertex {
  float4 uv : TEXCOORD0;
  float4 color : TEXCOORD1;
  float4 position : SV_Position;
};

Fh1EventMeshVertex main(uint vertex_id : SV_VertexID) {
  uint index = vertex_id == xe_system[0].w ? 0 : vertex_id;
  const uint index_endian = xe_system[1].x;
  if (index_endian == 1 || index_endian == 2) index = Swap8In16(index);
  if (index_endian == 2 || index_endian == 3)
    index = (index << 16) | (index >> 16);
  index = clamp((index + xe_system[1].y) & 0x00FFFFFF,
                xe_system[1].z, xe_system[1].w);

  const uint endian = xe_fetch[47].w & 3;
  const uint address = (xe_fetch[47].z & ~3) + index * 24;
  const float3 local = asfloat(uint3(Load(address, endian),
                                     Load(address + 4, endian),
                                     Load(address + 8, endian)));
  const float2 uv = asfloat(uint2(Load(address + 12, endian),
                                  Load(address + 16, endian)));
  const uint packed_color = Load(address + 20, endian);

  float4 intermediate = local.z * c[2] + c[3];
  intermediate = local.y * c[1].xzyw + intermediate.xzyw;
  float4 position = local.x * c[0] + intermediate.xzyw;
  if (xe_system[0].x & 8) position.w = rcp(position.w);
  if (xe_system[0].x & 2) position.xy *= position.w;
  if (xe_system[0].x & 4) position.z *= position.w;
  position.xyz = position.xyz * asfloat(xe_system[8].xyz) +
                 asfloat(xe_system[9].xyz) * position.w;

  Fh1EventMeshVertex output;
  output.position = position;
  output.uv = float4(uv, 0.0, 0.0);
  output.color = float4((packed_color >> 16) & 255,
                        (packed_color >> 8) & 255,
                        packed_color & 255,
                        packed_color >> 24) / 255.0;
  return output;
}
