#include <rex/input/sdl/wheel.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(hid_wheel_guids, "", "Input",
                      "Joystick GUIDs (comma-separated) the player marked as racing wheels; SDL's "
                      "own wheel type counts without being listed");
REXCVAR_DEFINE_INT32(hid_wheel_steering_saturation, 100, "Input",
                     "Racing wheel: percent of the wheel's travel either side of centre that "
                     "reaches full steering lock")
    .range(10, 100);
REXCVAR_DEFINE_INT32(hid_wheel_steering_deadzone, 0, "Input",
                     "Racing wheel: percent of steering output skipped off centre, to cancel the "
                     "game's stick deadzone")
    .range(0, 50);
REXCVAR_DEFINE_INT32(hid_wheel_steering_linearity, 100, "Input",
                     "Racing wheel: steering curve exponent in percent; 100 is linear, higher is "
                     "less sensitive near centre")
    .range(50, 300);
REXCVAR_DEFINE_INT32(hid_wheel_steering_centre, 15, "Input",
                     "Racing wheel: tenths of a percent of the wheel's travel either side of "
                     "centre that read as exactly centred")
    .range(0, 50);
REXCVAR_DEFINE_INT32(hid_wheel_pedal_deadzone, 0, "Input",
                     "Racing wheel: percent of each pedal's travel ignored at rest")
    .range(0, 50);

namespace rex::input::sdl {

namespace {

std::vector<std::string> SplitGuids(const std::string& text) {
  std::vector<std::string> guids;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    std::string guid = text.substr(start, end - start);
    guid.erase(std::remove_if(guid.begin(), guid.end(), [](char c) { return c == ' '; }),
               guid.end());
    if (!guid.empty()) guids.push_back(std::move(guid));
    start = end + 1;
  }
  return guids;
}

}  // namespace

WheelShaping CurrentWheelShaping() {
  WheelShaping shaping;
  shaping.saturation = std::clamp(REXCVAR_GET(hid_wheel_steering_saturation), 10, 100);
  shaping.deadzone = std::clamp(REXCVAR_GET(hid_wheel_steering_deadzone), 0, 50);
  shaping.linearity = std::clamp(REXCVAR_GET(hid_wheel_steering_linearity), 50, 300);
  shaping.pedal_deadzone = std::clamp(REXCVAR_GET(hid_wheel_pedal_deadzone), 0, 50);
  shaping.centre = std::clamp(REXCVAR_GET(hid_wheel_steering_centre), 0, 50);
  return shaping;
}

int16_t ShapeWheelSteering(int16_t value, const WheelShaping& shaping) {
  if (value == 0) return 0;
  // -32768 and 32767 are both full lock.
  double travel = value < 0 ? -double(value) / 32768.0 : double(value) / 32767.0;
  const double band = std::clamp(shaping.centre, 0, 50) / 1000.0;
  if (travel <= band)
    return 0;
  travel = (travel - band) / (1.0 - band);
  travel = std::min(1.0, travel * 100.0 / std::clamp(shaping.saturation, 10, 100));
  if (shaping.linearity != 100) {
    travel = std::pow(travel, std::clamp(shaping.linearity, 50, 300) / 100.0);
  }
  const double skip = std::clamp(shaping.deadzone, 0, 50) / 100.0;
  if (skip > 0.0 && travel > 0.0) travel = skip + (1.0 - skip) * travel;
  const double out = value < 0 ? -travel * 32768.0 : travel * 32767.0;
  return int16_t(std::clamp(std::lround(out), -32768L, 32767L));
}

uint8_t ShapeWheelPedal(uint8_t value, const WheelShaping& shaping) {
  const int rest = std::clamp(shaping.pedal_deadzone, 0, 50) * 255 / 100;
  if (rest == 0) return value;
  if (value <= rest) return 0;
  return uint8_t(std::lround(double(value - rest) * 255.0 / double(255 - rest)));
}

bool IsListedWheel(std::string_view guid) {
  if (guid.empty()) return false;
  // Called on every input poll: parse only when the cvar changes.
  static std::mutex mutex;
  static std::string parsed_text;
  static std::vector<std::string> parsed;
  std::lock_guard<std::mutex> lock(mutex);
  const std::string& text = REXCVAR_GET(hid_wheel_guids);
  if (text != parsed_text) {
    parsed_text = text;
    parsed = SplitGuids(text);
  }
  return std::find(parsed.begin(), parsed.end(), guid) != parsed.end();
}

void SetListedWheel(std::string_view guid, bool wheel) {
  if (guid.empty()) return;
  std::vector<std::string> guids = SplitGuids(REXCVAR_GET(hid_wheel_guids));
  const auto found = std::find(guids.begin(), guids.end(), guid);
  if (wheel == (found != guids.end())) return;
  if (wheel) {
    guids.emplace_back(guid);
  } else {
    guids.erase(found);
  }
  std::string text;
  for (const auto& entry : guids) {
    if (!text.empty()) text += ',';
    text += entry;
  }
  rex::cvar::SetFlagByName("hid_wheel_guids", text);
}

}  // namespace rex::input::sdl
