#include <rex/input/sdl/joystick_mapping.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(hid_user_mappings_file, "", "Input",
                      "SDL gamepad mappings the player made (the mapping assistant), loaded after "
                      "hid_mappings_file so they win; empty turns them off");

namespace rex::input::sdl {

namespace {

// An axis counts as moved past half its travel from rest, and as back at
// rest within a quarter.
constexpr int kMoved = 16384;
constexpr int kRested = 8192;

std::string GuidString(SDL_GUID guid) {
  char text[64] = {};
  SDL_GUIDToString(guid, text, int(sizeof(text)));
  return text;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  for (std::string line; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
  }
  return lines;
}

bool WriteLines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
  std::error_code error;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) output << line << '\n';
    if (!output) return false;
  }
  std::filesystem::rename(temporary, path, error);
  return !error;
}

bool SameGuid(std::string_view line, std::string_view guid) {
  return line.size() > guid.size() && line.substr(0, guid.size()) == guid &&
         line[guid.size()] == ',';
}

}  // namespace

std::vector<JoystickInfo> ListJoysticks() {
  std::vector<JoystickInfo> joysticks;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (!ids) return joysticks;
  for (int i = 0; i < count; ++i) {
    JoystickInfo info;
    info.instance_id = uint32_t(ids[i]);
    const char* name = SDL_GetJoystickNameForID(ids[i]);
    info.name = name ? name : "Controller";
    info.guid = GuidString(SDL_GetJoystickGUIDForID(ids[i]));
    if (char* mapping = SDL_GetGamepadMappingForID(ids[i])) {
      info.mapping = mapping;
      SDL_free(mapping);
    }
    const std::string user_path = REXCVAR_GET(hid_user_mappings_file);
    if (!user_path.empty()) {
      const auto lines = ReadLines(user_path);
      info.user_mapping = std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
        return SameGuid(line, info.guid);
      });
    }
    joysticks.push_back(std::move(info));
  }
  SDL_free(ids);
  return joysticks;
}

bool JoystickCapture::Begin(uint32_t instance_id) {
  End();
  joystick_ = SDL_OpenJoystick(SDL_JoystickID(instance_id));
  if (!joystick_) {
    REXLOG_WARN("Controller mapping: cannot open joystick {}: {}", instance_id, SDL_GetError());
    return false;
  }
  SDL_UpdateJoysticks();
  SampleRest();
  waiting_for_rest_ = true;
  return true;
}

void JoystickCapture::End() {
  if (joystick_) {
    SDL_CloseJoystick(joystick_);
    joystick_ = nullptr;
  }
}

void JoystickCapture::SampleRest() {
  // Where each axis rests: centred sticks near 0, triggers at an end.
  const int axes = SDL_GetNumJoystickAxes(joystick_);
  rest_axes_.assign(size_t(std::max(axes, 0)), 0);
  for (int i = 0; i < axes; ++i) {
    int16_t value = SDL_GetJoystickAxis(joystick_, i);
    int16_t start = 0;
    if (SDL_GetJoystickAxisInitialState(joystick_, i, &start)) value = start;
    rest_axes_[size_t(i)] = std::abs(value) > kMoved ? (value < 0 ? -32768 : 32767) : 0;
  }
}

bool JoystickCapture::AtRest() const {
  for (int i = 0, n = SDL_GetNumJoystickButtons(joystick_); i < n; ++i) {
    if (SDL_GetJoystickButton(joystick_, i)) return false;
  }
  for (int i = 0, n = SDL_GetNumJoystickHats(joystick_); i < n; ++i) {
    if (SDL_GetJoystickHat(joystick_, i) != SDL_HAT_CENTERED) return false;
  }
  for (size_t i = 0; i < rest_axes_.size(); ++i) {
    if (std::abs(int(SDL_GetJoystickAxis(joystick_, int(i))) - rest_axes_[i]) > kRested) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> JoystickCapture::Poll() {
  if (!joystick_) return std::nullopt;
  SDL_UpdateJoysticks();
  if (waiting_for_rest_) {
    if (!AtRest()) return std::nullopt;
    waiting_for_rest_ = false;
  }
  std::optional<std::string> input;
  for (int i = 0, n = SDL_GetNumJoystickButtons(joystick_); i < n && !input; ++i) {
    if (SDL_GetJoystickButton(joystick_, i)) input = "b" + std::to_string(i);
  }
  for (int i = 0, n = SDL_GetNumJoystickHats(joystick_); i < n && !input; ++i) {
    const uint8_t hat = SDL_GetJoystickHat(joystick_, i);
    // One direction at a time; a diagonal waits for the player to settle.
    if (hat == SDL_HAT_UP || hat == SDL_HAT_RIGHT || hat == SDL_HAT_DOWN || hat == SDL_HAT_LEFT) {
      input = "h" + std::to_string(i) + "." + std::to_string(hat);
    }
  }
  for (size_t i = 0; i < rest_axes_.size() && !input; ++i) {
    const int value = SDL_GetJoystickAxis(joystick_, int(i));
    const int rest = rest_axes_[i];
    if (std::abs(value - rest) <= kMoved) continue;
    const std::string axis = "a" + std::to_string(i);
    if (rest == 0) {
      input = (value < 0 ? "-" : "+") + axis;
    } else {
      // A trigger: its whole travel, inverted when it rests at the top.
      input = rest < 0 ? axis : axis + "~";
    }
  }
  if (input) waiting_for_rest_ = true;
  return input;
}

std::string BuildMapping(const JoystickInfo& joystick,
                         const std::vector<std::pair<std::string, std::string>>& bindings) {
  std::string name = joystick.name;
  for (char& c : name) {
    if (c == ',') c = ' ';
  }
  std::string mapping = joystick.guid + "," + name + ",";
  for (const auto& [output, input] : bindings) {
    mapping += output + ":" + input + ",";
  }
  return mapping;
}

bool ApplyMapping(std::string_view mapping) {
  const std::string text(mapping);
  if (SDL_AddGamepadMapping(text.c_str()) < 0) {
    REXLOG_ERROR("Controller mapping: SDL rejected '{}': {}", text, SDL_GetError());
    return false;
  }
  REXLOG_INFO("Controller mapping: applied '{}'", text);
  return true;
}

bool SaveMapping(std::string_view mapping) {
  if (!ApplyMapping(mapping)) return false;
  const std::string text(mapping);
  const std::string path = REXCVAR_GET(hid_user_mappings_file);
  if (path.empty()) return true;
  const std::string_view guid = mapping.substr(0, mapping.find(','));
  std::vector<std::string> lines = ReadLines(path);
  std::erase_if(lines, [&](const std::string& line) { return SameGuid(line, guid); });
  lines.push_back(text);
  return WriteLines(path, lines);
}

bool RemoveUserMapping(std::string_view guid) {
  const std::string path = REXCVAR_GET(hid_user_mappings_file);
  if (!path.empty()) {
    std::vector<std::string> lines = ReadLines(path);
    std::erase_if(lines, [&](const std::string& line) { return SameGuid(line, guid); });
    WriteLines(path, lines);
  }
  // The database line for this platform (or for every platform).
  const std::string platform = std::string("platform:") + SDL_GetPlatform() + ",";
  for (const auto& line : ReadLines(REXCVAR_GET(hid_mappings_file))) {
    if (SameGuid(line, guid) && (line.find("platform:") == std::string::npos ||
                                 (line + ",").find(platform) != std::string::npos)) {
      return ApplyMapping(line);
    }
  }
  return false;
}

bool VirtualJoystick::Attach(const char* name, int axes, int buttons, int hats) {
  Detach();
  initialized_ = SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
  desc.naxes = uint16_t(axes);
  desc.nbuttons = uint16_t(buttons);
  desc.nhats = uint16_t(hats);
  desc.name = name;
  id_ = uint32_t(SDL_AttachVirtualJoystick(&desc));
  joystick_ = id_ ? SDL_OpenJoystick(SDL_JoystickID(id_)) : nullptr;
  return joystick_ != nullptr;
}

void VirtualJoystick::Detach() {
  if (joystick_) SDL_CloseJoystick(joystick_);
  if (id_) SDL_DetachVirtualJoystick(SDL_JoystickID(id_));
  if (initialized_) SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
  joystick_ = nullptr;
  id_ = 0;
  initialized_ = false;
}

void VirtualJoystick::SetButton(int button, bool down) {
  if (joystick_) SDL_SetJoystickVirtualButton(joystick_, button, down);
}

void VirtualJoystick::SetAxis(int axis, int16_t value) {
  if (joystick_) SDL_SetJoystickVirtualAxis(joystick_, axis, value);
}

void VirtualJoystick::SetHat(int hat, uint8_t value) {
  if (joystick_) SDL_SetJoystickVirtualHat(joystick_, hat, value);
}

bool GamepadView::Begin(uint32_t instance_id) {
  End();
  gamepad_ = SDL_OpenGamepad(SDL_JoystickID(instance_id));
  return gamepad_ != nullptr;
}

void GamepadView::End() {
  if (gamepad_) {
    SDL_CloseGamepad(gamepad_);
    gamepad_ = nullptr;
  }
}

std::vector<std::string> GamepadView::Held() {
  std::vector<std::string> held;
  if (!gamepad_) return held;
  SDL_UpdateGamepads();
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    const auto button = SDL_GamepadButton(i);
    if (SDL_GetGamepadButton(gamepad_, button)) {
      if (const char* name = SDL_GetGamepadStringForButton(button)) held.emplace_back(name);
    }
  }
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
    const auto axis = SDL_GamepadAxis(i);
    const int value = SDL_GetGamepadAxis(gamepad_, axis);
    const char* name = SDL_GetGamepadStringForAxis(axis);
    if (!name || std::abs(value) <= kMoved) continue;
    const bool trigger =
        axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    held.push_back(trigger ? std::string(name) : (value < 0 ? "-" : "+") + std::string(name));
  }
  return held;
}

}  // namespace rex::input::sdl
