#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>

struct SDL_Gamepad;
struct SDL_Joystick;

// SDL mappings the player made, loaded after hid_mappings_file.
REXCVAR_DECLARE(std::string, hid_user_mappings_file);

// Building an SDL gamepad mapping from a controller's raw inputs, for a
// controller SDL's database does not know or a player wants to remap. All of
// it runs on the UI thread, after the SDL input driver has started.
namespace rex::input::sdl {

struct JoystickInfo {
  uint32_t instance_id = 0;
  std::string name;
  std::string guid;
  // The mapping SDL uses for it now (database or user), or empty.
  std::string mapping;
  // hid_user_mappings_file has a line for it.
  bool user_mapping = false;
};

std::vector<JoystickInfo> ListJoysticks();

// Raw input capture on one joystick. Poll returns an SDL mapping input for the
// next control moved from rest: "b3" (button), "h0.4" (hat direction), "+a2"
// or "-a2" (half axis from a centred rest), "a5" or "a5~" (an axis resting at
// one end, a trigger). After each capture every control has to return to rest
// before the next one counts, so one press is never taken twice.
class JoystickCapture {
 public:
  ~JoystickCapture() { End(); }
  bool Begin(uint32_t instance_id);
  std::optional<std::string> Poll();
  void End();
  bool active() const { return joystick_ != nullptr; }

 private:
  void SampleRest();
  bool AtRest() const;

  ::SDL_Joystick* joystick_ = nullptr;
  std::vector<int16_t> rest_axes_;
  bool waiting_for_rest_ = false;
};

// "<guid>,<name>,<bindings>" from output names (a, b, leftx, -lefty,
// lefttrigger, dpup...) and captured inputs.
std::string BuildMapping(const JoystickInfo& joystick,
                         const std::vector<std::pair<std::string, std::string>>& bindings);

// Applies a mapping for this session only (to try it before saving).
bool ApplyMapping(std::string_view mapping);
// Applies a mapping now and keeps it in hid_user_mappings_file (replacing the
// line for the same GUID), which is loaded after hid_mappings_file at start.
bool SaveMapping(std::string_view mapping);
// Drops the user mapping for a GUID and goes back to the hid_mappings_file
// line for it. Returns false when the file has none: SDL's built-in mapping,
// if any, then applies after a restart.
bool RemoveUserMapping(std::string_view guid);

// A virtual controller SDL has no mapping for, for tests of the above: it
// lives in the same SDL as the input driver.
class VirtualJoystick {
 public:
  ~VirtualJoystick() { Detach(); }
  bool Attach(const char* name, int axes, int buttons, int hats);
  void Detach();
  uint32_t id() const { return id_; }
  void SetButton(int button, bool down);
  void SetAxis(int axis, int16_t value);
  void SetHat(int hat, uint8_t value);

 private:
  uint32_t id_ = 0;
  ::SDL_Joystick* joystick_ = nullptr;
  bool initialized_ = false;
};

// The gamepad controls held now on a joystick, through its current mapping,
// as SDL names them ("a", "leftshoulder", "-leftx"), for a test screen.
class GamepadView {
 public:
  ~GamepadView() { End(); }
  bool Begin(uint32_t instance_id);
  std::vector<std::string> Held();
  void End();

 private:
  ::SDL_Gamepad* gamepad_ = nullptr;
};

}  // namespace rex::input::sdl
