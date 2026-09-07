// FH1 4D5309C9 world shader 79034645B1CB882B only.
// Uses the existing Xenos root bindings, but executes the title's 24 guest
// instructions directly instead of the generic translated vertex program.

ByteAddressBuffer xe_shared_memory_srv : register(t0);
RWByteAddressBuffer xe_shared_memory_uav : register(u0);

cbuffer xe_system_cbuffer : register(b0) {
  uint4 xe_system[30];
};

cbuffer xe_float_cbuffer : register(b1) {
  float4 c[16];
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

uint2 Load2(uint address, uint endian) {
  uint2 value;
  [branch]
  if (xe_system[0].x & 1) {
    value = xe_shared_memory_uav.Load2(address);
  } else {
    value = xe_shared_memory_srv.Load2(address);
  }
  return uint2(ApplyEndian(value.x, endian), ApplyEndian(value.y, endian));
}

float Fh1Mul(float a, float b) {
  return min(abs(a), abs(b)) == 0.0 ? 0.0 : a * b;
}

float2 Fh1Mul2(float a, float2 b) {
  return float2(Fh1Mul(a, b.x), Fh1Mul(a, b.y));
}

float3 Fh1Mul3(float a, float3 b) {
  return float3(Fh1Mul(a, b.x), Fh1Mul(a, b.y), Fh1Mul(a, b.z));
}

float4 Fh1Mul4(float a, float4 b) {
  return float4(Fh1Mul(a, b.x), Fh1Mul(a, b.y), Fh1Mul(a, b.z),
                Fh1Mul(a, b.w));
}

float Fh1Dot3(float3 a, float3 b) {
  precise float result = Fh1Mul(a.z, b.z);
  result = result + Fh1Mul(a.x, b.x);
  result = result + Fh1Mul(a.y, b.y);
  return result;
}

struct Fh1WorldVertex {
  float4 texture_coordinate : TEXCOORD0;
  float4 color : TEXCOORD1;
  float4 position : SV_Position;
};

Fh1WorldVertex main(uint vertex_id : SV_VertexID) {
  Fh1WorldVertex output;

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
  const uint address = (xe_fetch[47].z & ~3) + index * 32;
  const float3 local_position = asfloat(Load3(address, vertex_endian));
  const float3 local_normal = asfloat(Load3(address + 12, vertex_endian));
  const float2 local_uv = asfloat(Load2(address + 24, vertex_endian));

  precise float4 position = Fh1Mul4(1.0, c[9]);
  position = Fh1Mul4(local_position.z, c[8]) + position;
  position = Fh1Mul4(local_position.y, c[7]) + position;
  position = Fh1Mul4(local_position.x, c[6]) + position;
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
  output.position = position;

  output.texture_coordinate = 0;
  precise float2 texture_coordinate = Fh1Mul2(local_uv.x, c[13].xy) +
                                      c[15].xy;
  output.texture_coordinate.xy =
      Fh1Mul2(local_uv.y, c[14].xy) + texture_coordinate;

  precise float3 normal = Fh1Mul3(local_normal.z, c[5].xyz);
  normal = Fh1Mul3(local_normal.y, c[4].xyz) + normal;
  normal = Fh1Mul3(local_normal.x, c[3].xyz) + normal;
  precise float normal_length_squared = normal.z * normal.z;
  normal_length_squared = normal.y * normal.y + normal_length_squared;
  normal_length_squared = normal.x * normal.x + normal_length_squared;
  normal = Fh1Mul3(rsqrt(abs(normal_length_squared)), normal);
  const float lighting = saturate(Fh1Dot3(normal, c[0].xyz));
  precise float3 color = Fh1Mul3(lighting, c[2].xyz);
  color = float3(Fh1Mul(color.x, c[11].x),
                 Fh1Mul(color.y, c[11].y),
                 Fh1Mul(color.z, c[11].z)) + c[12].xyz;
  color += float3(Fh1Mul(c[1].x, c[10].x),
                  Fh1Mul(c[1].y, c[10].y),
                  Fh1Mul(c[1].z, c[10].z));
  output.color.xyz = color;
  output.color.w = Fh1Mul(c[11].w, c[10].w);
  return output;
}
