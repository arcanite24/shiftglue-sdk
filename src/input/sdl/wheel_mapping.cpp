/**
 * @file        input/sdl/wheel_mapping.cpp
 * @brief       Racing wheel bindings and force feedback math for the SDL driver.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/input/sdl/wheel_mapping.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

#include <rex/input/pad_remap.h>

namespace rex::input::sdl {

namespace {

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<int> ParseIndex(std::string_view text) {
  text = Trim(text);
  int value = -1;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || value < 0 || value > 255) {
    return std::nullopt;
  }
  return value;
}

// Splits "device|rest" at the last '|', so a device name may not hold one.
void SplitDevice(std::string_view text, std::string& device, std::string_view& rest) {
  const size_t bar = text.rfind('|');
  if (bar == std::string_view::npos) {
    device.clear();
    rest = Trim(text);
    return;
  }
  device = std::string(Trim(text.substr(0, bar)));
  rest = Trim(text.substr(bar + 1));
}

struct ControlTarget {
  uint16_t xbuttons = 0;
  bool left_trigger = false;
  bool right_trigger = false;
};

std::optional<ControlTarget> TargetForName(std::string_view name) {
  auto control = PadControlFromName(Trim(name));
  if (!control) {
    return std::nullopt;
  }
  ControlTarget target;
  switch (*control) {
    case PadControl::kA:
      target.xbuttons = X_INPUT_GAMEPAD_A;
      break;
    case PadControl::kB:
      target.xbuttons = X_INPUT_GAMEPAD_B;
      break;
    case PadControl::kX:
      target.xbuttons = X_INPUT_GAMEPAD_X;
      break;
    case PadControl::kY:
      target.xbuttons = X_INPUT_GAMEPAD_Y;
      break;
    case PadControl::kLeftShoulder:
      target.xbuttons = X_INPUT_GAMEPAD_LEFT_SHOULDER;
      break;
    case PadControl::kRightShoulder:
      target.xbuttons = X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      break;
    case PadControl::kLeftTrigger:
      target.left_trigger = true;
      break;
    case PadControl::kRightTrigger:
      target.right_trigger = true;
      break;
    case PadControl::kLeftThumb:
      target.xbuttons = X_INPUT_GAMEPAD_LEFT_THUMB;
      break;
    case PadControl::kRightThumb:
      target.xbuttons = X_INPUT_GAMEPAD_RIGHT_THUMB;
      break;
    case PadControl::kBack:
      target.xbuttons = X_INPUT_GAMEPAD_BACK;
      break;
    case PadControl::kStart:
      target.xbuttons = X_INPUT_GAMEPAD_START;
      break;
    case PadControl::kDpadUp:
      target.xbuttons = X_INPUT_GAMEPAD_DPAD_UP;
      break;
    case PadControl::kDpadDown:
      target.xbuttons = X_INPUT_GAMEPAD_DPAD_DOWN;
      break;
    case PadControl::kDpadLeft:
      target.xbuttons = X_INPUT_GAMEPAD_DPAD_LEFT;
      break;
    case PadControl::kDpadRight:
      target.xbuttons = X_INPUT_GAMEPAD_DPAD_RIGHT;
      break;
    default:
      return std::nullopt;
  }
  return target;
}

}  // namespace

std::optional<WheelAxisBinding> ParseWheelAxisBinding(std::string_view text) {
  text = Trim(text);
  if (text.empty()) {
    return std::nullopt;
  }
  WheelAxisBinding binding;
  std::string_view rest;
  SplitDevice(text, binding.device, rest);
  if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
    binding.direction = rest.front() == '+' ? AxisDirection::kNormal : AxisDirection::kInverted;
    rest.remove_prefix(1);
  }
  if (StartsWithNoCase(rest, "axis")) {
    rest.remove_prefix(4);
  } else if (StartsWithNoCase(rest, "a")) {
    rest.remove_prefix(1);
  } else {
    return std::nullopt;
  }
  auto index = ParseIndex(rest);
  if (!index) {
    return std::nullopt;
  }
  binding.axis = *index;
  return binding;
}

std::vector<WheelButtonBinding> ParseWheelButtonBindings(std::string_view text) {
  std::vector<WheelButtonBinding> out;
  // Entries are "NAME=input"; a NAME may list several inputs ("A=0,1"), so a
  // comma-separated part without '=' continues the previous NAME.
  std::optional<ControlTarget> target;
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) {
      comma = text.size();
    }
    std::string_view part = Trim(text.substr(start, comma - start));
    start = comma + 1;
    if (part.empty()) {
      continue;
    }
    std::string_view input = part;
    const size_t equals = part.find('=');
    if (equals != std::string_view::npos) {
      target = TargetForName(part.substr(0, equals));
      input = part.substr(equals + 1);
    }
    if (!target) {
      continue;
    }
    WheelButtonBinding binding;
    std::string_view rest;
    SplitDevice(input, binding.device, rest);
    binding.xbuttons = target->xbuttons;
    binding.left_trigger = target->left_trigger;
    binding.right_trigger = target->right_trigger;
    std::string_view axis_text = rest;
    AxisDirection axis_direction = AxisDirection::kAuto;
    if (!axis_text.empty() && (axis_text.front() == '+' || axis_text.front() == '-')) {
      axis_direction = axis_text.front() == '+' ? AxisDirection::kNormal : AxisDirection::kInverted;
      axis_text.remove_prefix(1);
    }
    if (StartsWithNoCase(axis_text, "axis")) {
      auto axis = ParseIndex(axis_text.substr(4));
      if (!axis) {
        continue;
      }
      binding.axis = *axis;
      binding.axis_direction = axis_direction;
    } else if (StartsWithNoCase(rest, "h")) {
      rest.remove_prefix(1);
      const size_t dot = rest.find('.');
      if (dot == std::string_view::npos) {
        continue;
      }
      auto hat = ParseIndex(rest.substr(0, dot));
      const std::string direction = Lower(Trim(rest.substr(dot + 1)));
      uint8_t mask = 0;
      if (direction == "up" || direction == "1") {
        mask = 0x01;
      } else if (direction == "right" || direction == "2") {
        mask = 0x02;
      } else if (direction == "down" || direction == "4") {
        mask = 0x04;
      } else if (direction == "left" || direction == "8") {
        mask = 0x08;
      }
      if (!hat || !mask) {
        continue;
      }
      binding.hat = *hat;
      binding.hat_mask = mask;
    } else {
      if (StartsWithNoCase(rest, "b")) {
        rest.remove_prefix(1);
      }
      auto button = ParseIndex(rest);
      if (!button) {
        continue;
      }
      binding.button = *button;
    }
    out.push_back(std::move(binding));
    if (comma == text.size()) {
      break;
    }
  }
  return out;
}

bool WheelDeviceMatches(std::string_view match, std::string_view name, uint16_t vendor,
                        uint16_t product, std::string_view path) {
  match = Trim(match);
  if (match.empty()) {
    return true;
  }
  if (StartsWithNoCase(match, "path:")) {
    return !path.empty() && Lower(Trim(match.substr(5))) == Lower(path);
  }
  // "vvvv:pppp"
  if (match.size() == 9 && match[4] == ':') {
    auto hex = [](std::string_view digits, uint16_t& value) {
      unsigned parsed = 0;
      auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 16);
      if (error != std::errc() || end != digits.data() + digits.size() || parsed > 0xFFFF) {
        return false;
      }
      value = static_cast<uint16_t>(parsed);
      return true;
    };
    uint16_t want_vendor = 0;
    uint16_t want_product = 0;
    if (hex(match.substr(0, 4), want_vendor) && hex(match.substr(5, 4), want_product)) {
      return want_vendor == vendor && want_product == product;
    }
  }
  return Lower(name).find(Lower(match)) != std::string::npos;
}

bool IsKnownWheelVendor(uint16_t vendor) {
  switch (vendor) {
    case 0x0EB7:  // Fanatec
    case 0x346E:  // Moza
    case 0x2433:  // Asetek SimSports
    case 0x3416:  // Cammus
      return true;
    default:
      return false;
  }
}

AxisDirection ResolveAxisDirection(AxisDirection direction, int16_t rest) {
  if (direction != AxisDirection::kAuto) {
    return direction;
  }
  return rest > 0 ? AxisDirection::kInverted : AxisDirection::kNormal;
}

int16_t SteeringFromAxis(int16_t raw, bool invert, int scale_percent, int deadzone_percent) {
  int32_t value = raw;
  if (invert) {
    value = -value - 1;
  }
  const int32_t deadzone = std::clamp(deadzone_percent, 0, 50) * 32767 / 100;
  const int32_t magnitude = std::abs(value);
  if (magnitude <= deadzone) {
    return 0;
  }
  // Rescale what is left after the deadzone back onto the full range.
  int64_t scaled = int64_t(magnitude - deadzone) * 32767 / (32767 - deadzone);
  scaled = scaled * std::clamp(scale_percent, 10, 1000) / 100;
  scaled = std::min<int64_t>(scaled, 32767);
  return static_cast<int16_t>(value < 0 ? -scaled : scaled);
}

uint8_t PedalFromAxis(int16_t raw, AxisDirection direction, int bottom_percent, int top_percent) {
  int32_t travel = int32_t(raw) + 32768;  // 0..65535
  if (direction == AxisDirection::kInverted) {
    travel = 65535 - travel;
  }
  const int32_t bottom = std::clamp(bottom_percent, 0, 50) * 65535 / 100;
  const int32_t top = std::clamp(top_percent, 0, 50) * 65535 / 100;
  const int32_t span = std::max(65535 - bottom - top, 1);
  if (travel <= bottom) {
    return 0;
  }
  if (travel >= 65535 - top) {
    return 255;
  }
  const int32_t value = (travel - bottom) * 255 / span;
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint8_t PedalFromAxis(int16_t raw, AxisDirection direction, int deadzone_percent) {
  int32_t travel = int32_t(raw) + 32768;  // 0..65535
  if (direction == AxisDirection::kInverted) {
    travel = 65535 - travel;
  }
  const int32_t deadzone = std::clamp(deadzone_percent, 0, 50) * 65535 / 100;
  if (travel <= deadzone) {
    return 0;
  }
  const int32_t value = (travel - deadzone) * 255 / (65535 - deadzone);
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t DpadFromHat(uint8_t hat) {
  uint16_t buttons = 0;
  if (hat & 0x01)
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (hat & 0x02)
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (hat & 0x04)
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (hat & 0x08)
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  return buttons;
}

WheelFfbMode ParseWheelFfbMode(std::string_view text) {
  const std::string mode = Lower(Trim(text));
  if (mode == "off") {
    return WheelFfbMode::kOff;
  }
  if (mode == "rumble") {
    return WheelFfbMode::kRumble;
  }
  if (mode == "left-signed") {
    return WheelFfbMode::kLeftSigned;
  }
  if (mode == "right-signed") {
    return WheelFfbMode::kRightSigned;
  }
  return WheelFfbMode::kDifference;
}

int16_t ConstantForceFromMotors(uint16_t left_motor, uint16_t right_motor, int strength_percent,
                                bool invert) {
  int32_t force = int32_t(right_motor) - int32_t(left_motor);  // -65535..65535
  if (invert) {
    force = -force;
  }
  const int64_t level = int64_t(force) * 32767 / 65535 * std::clamp(strength_percent, 0, 100) / 100;
  return static_cast<int16_t>(std::clamp<int64_t>(level, -32767, 32767));
}

int16_t ConstantForceFromSignedMotor(uint16_t motor, int strength_percent, bool invert) {
  int32_t force = int32_t(motor) - 0x8000;  // -32768..32767
  if (invert) {
    force = -force;
  }
  const int64_t level = int64_t(force) * std::clamp(strength_percent, 0, 100) / 100;
  return static_cast<int16_t>(std::clamp<int64_t>(level, -32767, 32767));
}

int16_t ConstantForceForMode(WheelFfbMode mode, uint16_t left_motor, uint16_t right_motor,
                             int strength_percent, bool invert) {
  switch (mode) {
    case WheelFfbMode::kDifference:
      return ConstantForceFromMotors(left_motor, right_motor, strength_percent, invert);
    case WheelFfbMode::kLeftSigned:
      return ConstantForceFromSignedMotor(left_motor, strength_percent, invert);
    case WheelFfbMode::kRightSigned:
      return ConstantForceFromSignedMotor(right_motor, strength_percent, invert);
    default:
      return 0;
  }
}

namespace {

int64_t ScaleSigned(int64_t value, int64_t full_scale, int64_t out_max) {
  if (full_scale <= 0) {
    return 0;
  }
  return std::clamp<int64_t>(value * out_max / full_scale, -out_max, out_max);
}

int64_t ApplyGain(int64_t value, uint32_t gain, int strength_percent) {
  value = value * std::min<uint32_t>(gain, 255) / 255;
  return value * std::clamp(strength_percent, 0, 100) / 100;
}

}  // namespace

int16_t EffectConstantLevel(const xinputd_ffb::Effect& effect, const WheelEffectScale& scale) {
  int64_t level = ScaleSigned(effect.magnitude, scale.magnitude_max, 32767);
  level = ApplyGain(level, effect.gain, 100);
  if (level != 0 && scale.min_force_percent > 0) {
    const int64_t floor = int64_t(32767) * std::clamp(scale.min_force_percent, 0, 100) / 100;
    const int64_t magnitude = floor + std::abs(level) * (32767 - floor) / 32767;
    level = level < 0 ? -magnitude : magnitude;
  }
  level = level * std::clamp(scale.strength_percent, 0, 100) / 100;
  if (scale.invert) {
    level = -level;
  }
  return static_cast<int16_t>(std::clamp<int64_t>(level, -32767, 32767));
}

WheelCondition EffectCondition(const xinputd_ffb::Effect& effect, const WheelEffectScale& scale) {
  auto coeff = [&](int32_t value) {
    int64_t scaled = ScaleSigned(value, scale.magnitude_max, 32767);
    scaled = ApplyGain(scaled, effect.gain, scale.strength_percent);
    scaled = scaled * std::clamp(scale.condition_percent, 0, 400) / 100;
    return static_cast<int16_t>(std::clamp<int64_t>(scaled, -32767, 32767));
  };
  auto unsigned_scale = [&](int32_t value) {
    if (scale.saturation_max <= 0) {
      return uint16_t(0);
    }
    const int64_t scaled = int64_t(std::max(value, 0)) * 65535 / scale.saturation_max;
    return static_cast<uint16_t>(std::min<int64_t>(scaled, 65535));
  };
  WheelCondition c;
  c.right_coeff = coeff(effect.positive_coefficient);
  c.left_coeff = coeff(effect.negative_coefficient);
  c.right_sat = unsigned_scale(effect.positive_saturation);
  c.left_sat = unsigned_scale(effect.negative_saturation);
  c.deadband = unsigned_scale(effect.dead_band);
  c.center = static_cast<int16_t>(ScaleSigned(effect.magnitude, scale.magnitude_max, 32767));
  return c;
}

const char* WheelFfbModeName(WheelFfbMode mode) {
  switch (mode) {
    case WheelFfbMode::kOff:
      return "off";
    case WheelFfbMode::kDifference:
      return "difference";
    case WheelFfbMode::kRumble:
      return "rumble";
    case WheelFfbMode::kLeftSigned:
      return "left-signed";
    case WheelFfbMode::kRightSigned:
      return "right-signed";
  }
  return "?";
}

}  // namespace rex::input::sdl
