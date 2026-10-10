#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/input/sdl/joystick_mapping.h>

using rex::input::sdl::JoystickCapture;
using rex::input::sdl::JoystickInfo;

namespace {

// A controller SDL has no mapping for: 4 axes (axis 3 rests at one end, like
// a trigger), 12 buttons, 1 hat. SDL is only reached through the library,
// whose SDL is the input driver's (SDL's constants are fine to use here).
class VirtualPad {
 public:
  VirtualPad() {
    ok_ = joystick_.Attach("Mapping Test Pad", 4, 12, 1);
    joystick_.SetAxis(3, -32768);
  }
  bool ok() const { return ok_; }
  uint32_t id() const { return joystick_.id(); }
  void Button(int button, bool down) { joystick_.SetButton(button, down); }
  void Axis(int axis, int16_t value) { joystick_.SetAxis(axis, value); }
  void Hat(int hat, uint8_t value) { joystick_.SetHat(hat, value); }
  void Rest() {
    for (int i = 0; i < 12; ++i) Button(i, false);
    Axis(0, 0);
    Axis(1, 0);
    Axis(2, 0);
    Axis(3, -32768);
    Hat(0, SDL_HAT_CENTERED);
  }

 private:
  rex::input::sdl::VirtualJoystick joystick_;
  bool ok_ = false;
};

JoystickInfo FindPad(uint32_t id) {
  for (auto& joystick : rex::input::sdl::ListJoysticks()) {
    if (joystick.instance_id == id) return joystick;
  }
  return {};
}

}  // namespace

TEST_CASE("Joystick capture reads buttons, hats, half axes and triggers", "[input][sdl]") {
  VirtualPad pad;
  REQUIRE(pad.ok());
  JoystickCapture capture;
  REQUIRE(capture.Begin(pad.id()));
  CHECK_FALSE(capture.Poll());

  pad.Button(3, true);
  CHECK(capture.Poll() == "b3");
  // Still held: not taken twice.
  CHECK_FALSE(capture.Poll());
  pad.Rest();
  CHECK_FALSE(capture.Poll());

  // Resting noise below the threshold does not count.
  pad.Axis(1, 6000);
  CHECK_FALSE(capture.Poll());
  pad.Axis(1, -30000);
  CHECK(capture.Poll() == "-a1");
  pad.Rest();
  CHECK_FALSE(capture.Poll());

  pad.Hat(0, SDL_HAT_UP);
  CHECK(capture.Poll() == "h0.1");
  pad.Rest();
  CHECK_FALSE(capture.Poll());

  // Axis 3 rested at its low end: a trigger over its whole travel.
  pad.Axis(3, 32767);
  CHECK(capture.Poll() == "a3");
  pad.Rest();
  CHECK_FALSE(capture.Poll());
  capture.End();
}

TEST_CASE("A built mapping applies and the gamepad view follows it", "[input][sdl]") {
  VirtualPad pad;
  REQUIRE(pad.ok());
  JoystickInfo info = FindPad(pad.id());
  REQUIRE(info.name == "Mapping Test Pad");
  REQUIRE_FALSE(info.guid.empty());

  const std::string mapping = rex::input::sdl::BuildMapping(
      info, {{"a", "b3"}, {"start", "b7"}, {"leftx", "a0"}, {"-lefty", "-a1"},
             {"righttrigger", "a3"}, {"dpup", "h0.1"}});
  CHECK(mapping.rfind(info.guid + ",Mapping Test Pad,a:b3,start:b7,leftx:a0,", 0) == 0);
  REQUIRE(rex::input::sdl::ApplyMapping(mapping));
  CHECK_FALSE(FindPad(pad.id()).mapping.empty());

  rex::input::sdl::GamepadView view;
  REQUIRE(view.Begin(pad.id()));
  CHECK(view.Held().empty());
  pad.Button(3, true);
  pad.Axis(0, -30000);
  pad.Hat(0, SDL_HAT_UP);
  pad.Axis(3, 32767);
  auto held = view.Held();
  std::sort(held.begin(), held.end());
  CHECK(held == std::vector<std::string>{"-leftx", "a", "dpup", "righttrigger"});
  pad.Rest();
  CHECK(view.Held().empty());
  view.End();
}
