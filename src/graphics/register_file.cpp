/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <bitset>
#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/math.h>

namespace rex::graphics {

RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));
}

namespace {

const std::bitset<RegisterFile::kRegisterCount> kKnownRegisters = [] {
  std::bitset<RegisterFile::kRegisterCount> known;
  // The table also names registers past the file (0x8D00 and up).
#define XE_GPU_REGISTER(index, type, name) \
  if ((index) < known.size()) known.set(index);
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
  return known;
}();

}  // namespace

bool RegisterFile::IsKnownRegister(uint32_t index) {
  return index < kRegisterCount && kKnownRegisters[index];
}

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_GPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterInfo reg_info = { \
        RegisterInfo::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace rex::graphics
