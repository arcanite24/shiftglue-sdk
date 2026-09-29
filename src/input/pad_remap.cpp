/**
 * @file        input/pad_remap.cpp
 * @brief       Controller button remapping (pad_remap)
 */

#include <rex/input/pad_remap.h>

#include <algorithm>
#include <cctype>
#include <mutex>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(pad_remap, "", "Input",
                      "Controller remapping as PHYSICAL=GAME pairs, e.g. \"A=B,B=A\" swaps A and "
                      "B. Names: A B X Y LB RB LT RT LS RS BACK START UP DOWN LEFT RIGHT. "
                      "Applies to controllers, not to mouse and keyboard, and not to host menus")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pad_invert_right_stick_y, false, "Input",
                    "Invert the right stick's vertical axis (camera look) on controllers")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input {
namespace {

constexpr std::array<std::string_view, kPadControlCount> kNames = {
    "A",  "B",  "X",    "Y",     "LB", "RB",   "LT",   "RT",
    "LS", "RS", "BACK", "START", "UP", "DOWN", "LEFT", "RIGHT"};

// XINPUT_GAMEPAD button bits; triggers are analog and have none.
constexpr std::array<uint16_t, kPadControlCount> kBits = {
    X_INPUT_GAMEPAD_A,           X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,           X_INPUT_GAMEPAD_Y,
    X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    0,                           0,
    X_INPUT_GAMEPAD_LEFT_THUMB,  X_INPUT_GAMEPAD_RIGHT_THUMB,
    X_INPUT_GAMEPAD_BACK,        X_INPUT_GAMEPAD_START,
    X_INPUT_GAMEPAD_DPAD_UP,     X_INPUT_GAMEPAD_DPAD_DOWN,
    X_INPUT_GAMEPAD_DPAD_LEFT,   X_INPUT_GAMEPAD_DPAD_RIGHT};

// A trigger counts as pressed past XINPUT_GAMEPAD_TRIGGER_THRESHOLD.
constexpr uint8_t kTriggerThreshold = 30;

std::string Trim(std::string_view text) {
  size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  std::string result(text.substr(begin, end - begin));
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return result;
}

}  // namespace

std::string_view PadControlName(PadControl control) {
  return control < PadControl::kCount ? kNames[size_t(control)] : std::string_view();
}

std::optional<PadControl> PadControlFromName(std::string_view name) {
  const std::string upper = Trim(name);
  for (size_t i = 0; i < kPadControlCount; ++i) {
    if (kNames[i] == upper) {
      return PadControl(i);
    }
  }
  return std::nullopt;
}

PadRemap IdentityPadRemap() {
  PadRemap remap{};
  for (size_t i = 0; i < kPadControlCount; ++i) {
    remap[i] = PadControl(i);
  }
  return remap;
}

PadRemap ParsePadRemap(std::string_view text) {
  PadRemap remap = IdentityPadRemap();
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string_view pair = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    const size_t equals = pair.find('=');
    if (equals == std::string_view::npos) continue;
    const auto physical = PadControlFromName(pair.substr(0, equals));
    const auto game = PadControlFromName(pair.substr(equals + 1));
    if (physical && game) {
      remap[size_t(*physical)] = *game;
    }
  }
  return remap;
}

std::string FormatPadRemap(const PadRemap& remap) {
  std::string text;
  for (size_t i = 0; i < kPadControlCount; ++i) {
    if (remap[i] != PadControl(i)) {
      if (!text.empty()) text += ',';
      text += kNames[i];
      text += '=';
      text += kNames[size_t(remap[i])];
    }
  }
  return text;
}

void ApplyPadRemap(X_INPUT_GAMEPAD& pad) {
  if (REXCVAR_GET(pad_invert_right_stick_y)) {
    pad.thumb_ry = int16_t(std::clamp(-int32_t(int16_t(pad.thumb_ry)), -32768, 32767));
  }
  // Parsed again only when the cvar text changes.
  static std::mutex mutex;
  static std::string parsed_text;
  static PadRemap remap = IdentityPadRemap();
  PadRemap current;
  {
    std::lock_guard lock(mutex);
    const std::string& text = REXCVAR_GET(pad_remap);
    if (text != parsed_text) {
      parsed_text = text;
      remap = ParsePadRemap(text);
    }
    if (parsed_text.empty()) return;
    current = remap;
  }
  const uint16_t buttons = uint16_t(pad.buttons);
  const uint8_t triggers[2] = {uint8_t(pad.left_trigger), uint8_t(pad.right_trigger)};
  uint16_t out_buttons = 0;
  uint8_t out_triggers[2] = {0, 0};
  for (size_t i = 0; i < kPadControlCount; ++i) {
    const auto source = PadControl(i);
    const bool is_trigger = source == PadControl::kLeftTrigger ||
                            source == PadControl::kRightTrigger;
    const uint8_t value = is_trigger ? triggers[i - size_t(PadControl::kLeftTrigger)]
                                     : ((buttons & kBits[i]) ? 255 : 0);
    if (!value) continue;
    const auto target = current[i];
    if (target == PadControl::kLeftTrigger || target == PadControl::kRightTrigger) {
      uint8_t& out = out_triggers[size_t(target) - size_t(PadControl::kLeftTrigger)];
      out = std::max(out, value);
    } else if (!is_trigger || value > kTriggerThreshold) {
      out_buttons |= kBits[size_t(target)];
    }
  }
  pad.buttons = out_buttons;
  pad.left_trigger = out_triggers[0];
  pad.right_trigger = out_triggers[1];
}

}  // namespace rex::input
