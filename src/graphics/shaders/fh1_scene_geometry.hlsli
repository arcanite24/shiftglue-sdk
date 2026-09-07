// Shared by the blended and lit scene programs, after their buffer declarations.
uint4 LoadGeometry4(uint address) {
#ifdef FH1_SCENE_OWNED_GEOMETRY
  // Root SRVs lack bounds; preserve each component's physical-memory fallback.
  uint4 value = uint4(0, 0, 0, 0);
  [branch]
  if (address <= 0x20000000u - 16u) {
    value = shared_srv.Load4(address);
  } else {
    if (address < 0x20000000u) value.x = shared_srv.Load(address);
    if (address < 0x20000000u - 4u) value.y = shared_srv.Load(address + 4u);
    if (address < 0x20000000u - 8u) value.z = shared_srv.Load(address + 8u);
  }
#else
  uint4 value;
  if (system_data[0].x & 1) value = shared_uav.Load4(address);
  else value = shared_srv.Load4(address);
#endif
  return value;
}

uint LoadGeometryWord(uint address) {
#ifdef FH1_SCENE_OWNED_GEOMETRY
  uint value = 0;
  [branch]
  if (address < 0x20000000u) value = shared_srv.Load(address);
  return value;
#else
  uint value;
  if (system_data[0].x & 1) value = shared_uav.Load(address);
  else value = shared_srv.Load(address);
  return value;
#endif
}
