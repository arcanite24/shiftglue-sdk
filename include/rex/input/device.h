#pragma once
/**
 * @file        rex/input/device.h
 * @brief       Input device identity shared by drivers and device assignment.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

namespace rex::input {

constexpr uint32_t kMaxGuestUsers = 4;

/// Driver-scoped device handle. Never reused within a process run, so a handle
/// that outlives its device resolves to nothing rather than aliasing whichever
/// device took the freed slot.
enum class DeviceId : uint64_t { kInvalid = 0 };

/// Whose button labels a pad carries, for glyphs.
enum class PadFamily : uint8_t { kUnknown, kXbox, kPlayStation, kNintendo };

struct DeviceInfo {
  DeviceId id = DeviceId::kInvalid;
  uint32_t ordinal = 0;  // connection order, assigned by InputSystem
  std::string name;
  std::string guid;
  bool synthetic = false;  // keyboard/mouse emulation or the NOP stand-in
  PadFamily family = PadFamily::kUnknown;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  // Steam Input's virtual pad standing in for the real one: the real pad's
  // own features (triggers, lightbar, motion) do not reach it.
  bool steam_virtual = false;
  // A Sony DualSense or DualSense Edge the driver can send trigger effects to.
  bool adaptive_triggers = false;
  bool lightbar = false;
  bool motion = false;  // gyro and accelerometer
  // A racing wheel with a force feedback motor the driver can drive.
  bool force_feedback = false;
};

}  // namespace rex::input
