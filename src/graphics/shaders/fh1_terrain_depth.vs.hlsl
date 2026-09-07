// FH1 4D5309C9 terrain-depth shaders 5A28C7FAFD86F112,
// CA293E0A1CB4B416, and 4E1DA281CC3D7EDB only.

ByteAddressBuffer xe_shared_memory_srv : register(t0);
#ifdef FH1_TERRAIN_OWNED_GEOMETRY
ByteAddressBuffer terrain_direction_srv : register(t1);
ByteAddressBuffer terrain_control_srv : register(t2);
#else
RWByteAddressBuffer xe_shared_memory_uav : register(u0);
#endif
cbuffer xe_system_cbuffer : register(b0) { uint4 xe_system[30]; };
cbuffer xe_float_cbuffer : register(b1) { float4 c[11]; };
cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch[48]; };

#include "fh1_terrain_vertex.hlsli"

float4 main(uint vertex_id : SV_VertexID) : SV_Position {
  uint index = vertex_id == xe_system[0].w ? 0 : vertex_id;
  const uint index_endian = xe_system[1].x;
  index = ApplyEndian(index, index_endian);
  index = clamp((index + xe_system[1].y) & 0x00FFFFFFu,
                xe_system[1].z, xe_system[1].w);

  const uint vertex_endian = xe_fetch[47].w & 3u;
  const uint vertex_address = (xe_fetch[47].z & ~3u) + index * 28u;
  const uint2 packed_vertex = LoadVertex(vertex_address, vertex_endian);
  const float4 fetched = DecodeSnorm16x4(packed_vertex).yxwz;

  const float3 cell_origin_delta = c[7].yxz - c[4].yxz;
  const float scale = Fh1Mul(fetched.w, c[7].w);
  const float3 scaled_local = Fh1Mul(scale.xxx, fetched.yxz);
  const float3 base_position = scaled_local.yxz + c[7].xyz;

  float3 direction;
  if (c[8].x == 0.0) {
    direction = -abs(float(index)) > c[9].z ? 1.0 : 0.0;
  } else {
    const uint packed_direction = LoadWord(
        (xe_fetch[45].x & ~3u) + index * 4u, xe_fetch[45].y & 3u);
    direction = DecodeSnorm101111(packed_direction) * 2.0;
  }

  const float3 grid_scale = Fh1Mul(c[5].yxz, c[9].yyy);
  const float3 grid_position = Fh1Mul(grid_scale, cell_origin_delta + scaled_local);
  const float3 grid_floor = floor(grid_position.xzy);
  const float3 grid_cell = clamp(grid_floor, c[9].z, c[9].w);
  precise float grid_offset = Fh1Mul(grid_cell.y, c[9].x) +
                              Fh1Mul(grid_cell.x, c[9].y) + grid_cell.z;
  const uint grid_index = uint(int(floor(grid_offset)));
  const uint control_endian = xe_fetch[44].w & 3u;
  const uint control_address = (xe_fetch[44].z & ~3u) + grid_index * 32u;
  const float4 control0 = asfloat(Load4(control_address, control_endian)).xywz;
  const float4 control1 = asfloat(Load4(control_address + 16u, control_endian)).xywz;
  const float3 fraction = grid_position.xzy - grid_floor;
  const float4 x_lerp = Fh1Mul(control0 - control1, fraction.xxxx) + control1;
  const float2 y_lerp = Fh1Mul(x_lerp.zw - x_lerp.xy, fraction.yy) + x_lerp.xy;
  precise float height = saturate(Fh1Mul(y_lerp.y - y_lerp.x, fraction.z) + y_lerp.x);
  height = saturate(Fh1Mul(clamp(height, c[6].x, c[6].y), c[6].w));
  const float3 position = Fh1Mul(height.xxx, direction) + base_position;

  precise float4 output;
#ifdef FH1_TERRAIN_DEPTH_STANDARD_TRANSFORM
  output = Fh1Mul(position.zzzz, c[2]) + c[3];
  output = Fh1Mul(position.yyyy, c[1]) + output;
  output = Fh1Mul(position.xxxx, c[0]) + output;
#else
  output = Fh1Mul(position.zzzz, c[2].xwzy);
  output = Fh1Mul(position.yyyy, c[1].xwzy) + output;
  output = Fh1Mul(position.xxxx, c[0].xywz) + output.xwyz;
  output = output.xywz + c[3];
  // The guest explicitly rounds the reciprocal and both products.
  precise float reciprocal_w = rcp(output.w);
  output.y = Fh1Mul(Fh1Mul(output.y, reciprocal_w) - c[10].x, output.w);
#endif

  if ((xe_system[0].x & 8u) == 0) output.w = 1.0 / output.w;
  if (xe_system[0].x & 2u) output.xy *= output.w;
  if (xe_system[0].x & 4u) output.z *= output.w;
  precise float3 scaled_position = output.xyz * asfloat(xe_system[8].xyz);
  output.xyz = mad(asfloat(xe_system[9].xyz), output.w, scaled_position);
  return output;
}
