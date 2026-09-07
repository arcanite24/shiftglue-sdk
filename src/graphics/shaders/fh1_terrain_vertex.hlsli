// Shared packed terrain fetch and arithmetic helpers.
// Explicit branches prevent FXC from loading both SRV and UAV before selection.
uint Swap8In16(uint value) {
  return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
}

uint ApplyEndian(uint value, uint endian) {
  if (endian == 1 || endian == 2) value = Swap8In16(value);
  if (endian == 2 || endian == 3) value = (value << 16) | (value >> 16);
  return value;
}

uint LoadWord(uint address, uint endian) {
#ifdef FH1_TERRAIN_OWNED_GEOMETRY
  uint value = 0;
  [branch]
  if (address < 0x20000000u) value = terrain_direction_srv.Load(address);
#else
  uint value;
  [branch]
  if (xe_system[0].x & 1u) value = xe_shared_memory_uav.Load(address);
  else value = xe_shared_memory_srv.Load(address);
#endif
  return ApplyEndian(value, endian);
}

uint4 Load4(uint address, uint endian) {
#ifdef FH1_TERRAIN_OWNED_GEOMETRY
  // Direct root SRVs lack hardware bounds. Keep each component's shared
  // fallback zero at the physical limit, without wrapping address arithmetic.
  uint4 value = uint4(0, 0, 0, 0);
  [branch]
  if (address <= 0x20000000u - 16u) {
    value = terrain_control_srv.Load4(address);
  } else {
    if (address < 0x20000000u) value.x = terrain_control_srv.Load(address);
    if (address < 0x20000000u - 4u) value.y = terrain_control_srv.Load(address + 4u);
    if (address < 0x20000000u - 8u) value.z = terrain_control_srv.Load(address + 8u);
  }
#else
  uint4 value;
  [branch]
  if (xe_system[0].x & 1u) value = xe_shared_memory_uav.Load4(address);
  else value = xe_shared_memory_srv.Load4(address);
#endif
  return uint4(ApplyEndian(value.x, endian), ApplyEndian(value.y, endian),
               ApplyEndian(value.z, endian), ApplyEndian(value.w, endian));
}

uint2 LoadVertex(uint address, uint endian) {
#ifdef FH1_TERRAIN_OWNED_GEOMETRY
  uint2 value = uint2(0, 0);
  [branch]
  if (address <= 0x20000000u - 8u) {
    value = xe_shared_memory_srv.Load2(address);
  } else if (address < 0x20000000u) {
    value.x = xe_shared_memory_srv.Load(address);
  }
#else
  uint2 value;
  [branch]
  if (xe_system[0].x & 1u) value = xe_shared_memory_uav.Load2(address);
  else value = xe_shared_memory_srv.Load2(address);
#endif
  return uint2(ApplyEndian(value.x, endian), ApplyEndian(value.y, endian));
}

int SignExtend(uint value, uint bits) {
  const uint shift = 32 - bits;
  return int(value << shift) >> shift;
}

// Match guest packed normalization: multiply by rounded float reciprocals.
float4 DecodeSnorm16x4(uint2 packed) {
  const int4 value = int4(SignExtend(packed.x, 16),
                          SignExtend(packed.x >> 16, 16),
                          SignExtend(packed.y, 16),
                          SignExtend(packed.y >> 16, 16));
  return max(float4(value) * (1.0 / 32767.0), -1.0);
}

float3 DecodeSnorm101111(uint packed) {
  return max(float3(SignExtend(packed, 11), SignExtend(packed >> 11, 11),
                    SignExtend(packed >> 22, 10)) *
                 float3(1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 511.0),
             -1.0);
}

// Preserve guest zero multiplication and separate rounding before addition.
float Fh1Mul(float a, float b) {
  precise float product = a * b;
  return min(abs(a), abs(b)) == 0.0 ? 0.0 : product;
}
float2 Fh1Mul(float2 a, float2 b) {
  return float2(Fh1Mul(a.x, b.x), Fh1Mul(a.y, b.y));
}
float3 Fh1Mul(float3 a, float3 b) {
  return float3(Fh1Mul(a.x, b.x), Fh1Mul(a.y, b.y), Fh1Mul(a.z, b.z));
}
float4 Fh1Mul(float4 a, float4 b) {
  return float4(Fh1Mul(a.x, b.x), Fh1Mul(a.y, b.y), Fh1Mul(a.z, b.z), Fh1Mul(a.w, b.w));
}

