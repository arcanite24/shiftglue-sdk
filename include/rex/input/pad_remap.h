/**
 * @file        input/pad_remap.h
 * @brief       Controller button remapping (pad_remap)
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rex/input/input.h>

namespace rex::input {

// The remappable controls of an Xbox 360 pad.
enum class PadControl : uint8_t {
  kA, kB, kX, kY, kLeftShoulder, kRightShoulder, kLeftTrigger, kRightTrigger,
  kLeftThumb, kRightThumb, kBack, kStart, kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kCount
};

constexpr size_t kPadControlCount = size_t(PadControl::kCount);

// Short names used in pad_remap ("A", "LB", "LT", "UP", ...).
std::string_view PadControlName(PadControl control);
std::optional<PadControl> PadControlFromName(std::string_view name);

// For each physical control, the control the title receives.
using PadRemap = std::array<PadControl, kPadControlCount>;

PadRemap IdentityPadRemap();
// Parses "PHYSICAL=GAME" pairs separated by commas; unknown names are
// ignored and unlisted controls map to themselves.
PadRemap ParsePadRemap(std::string_view text);
// The shortest text that parses back to `remap` (empty for identity).
std::string FormatPadRemap(const PadRemap& remap);

// Rewrites `pad` through the pad_remap and pad_invert_right_stick_y cvars.
void ApplyPadRemap(X_INPUT_GAMEPAD& pad);

}  // namespace rex::input
