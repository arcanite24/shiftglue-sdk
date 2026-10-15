#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
  // SDL_AddGamepadMappingsFromFile only takes lines with a platform field.
  CHECK(mapping.find(",platform:") != std::string::npos);
  CHECK(mapping.back() == ',');
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

// Some DirectInput wheels (a Thrustmaster T248 on Windows) report every axis
// as 0 when opened and their real positions about half a second later, so
// SDL's initial state for pedals resting at the top is 0.
TEST_CASE("A pedal reporting its rest after capture begins is a trigger", "[input][sdl]") {
  rex::input::sdl::VirtualJoystick wheel;
  REQUIRE(wheel.Attach("Late Pedal Wheel", 4, 12, 1, true));
  JoystickCapture capture;
  REQUIRE(capture.Begin(wheel.id()));
  CHECK_FALSE(capture.Poll());

  // The pedal's real position arrives: resting at the top, not pressed.
  wheel.SetAxis(1, 32767);
  CHECK_FALSE(capture.Poll());
  CHECK_FALSE(capture.Poll());
  wheel.SetAxis(1, -32768);
  CHECK(capture.Poll() == "a1~");
  wheel.SetAxis(1, 32767);
  CHECK_FALSE(capture.Poll());
  capture.End();
}

TEST_CASE("A pedal SDL first saw at 0 rests where it reads", "[input][sdl]") {
  rex::input::sdl::VirtualJoystick wheel;
  REQUIRE(wheel.Attach("Stale Pedal Wheel", 4, 12, 1, true));
  // Held open, as the input driver holds a mapped wheel. The wheel reports 0
  // first, which SDL keeps as the pedal's initial state, then its real
  // position.
  JoystickCapture driver;
  REQUIRE(driver.Begin(wheel.id()));
  wheel.SetAxis(1, 0);
  driver.Poll();
  wheel.SetAxis(1, 32767);
  driver.Poll();

  JoystickCapture capture;
  REQUIRE(capture.Begin(wheel.id()));
  CHECK_FALSE(capture.Poll());
  wheel.SetAxis(1, -32768);
  CHECK(capture.Poll() == "a1~");
  capture.End();
  driver.End();
}

TEST_CASE("A wheel's steering counts from an eighth of its travel", "[input][sdl]") {
  rex::input::sdl::VirtualJoystick wheel;
  REQUIRE(wheel.Attach("Mapping Test Wheel", 4, 12, 1, true));
  JoystickCapture capture;
  REQUIRE(capture.Begin(wheel.id()));
  CHECK_FALSE(capture.Poll());

  wheel.SetAxis(0, -3000);
  CHECK_FALSE(capture.Poll());
  wheel.SetAxis(0, -6000);
  CHECK(capture.Poll() == "-a0");
  // Not back at rest until within a sixteenth of centre.
  wheel.SetAxis(0, -3000);
  CHECK_FALSE(capture.Poll());
  wheel.SetAxis(0, 0);
  CHECK_FALSE(capture.Poll());
  wheel.SetAxis(0, 6000);
  CHECK(capture.Poll() == "+a0");
  capture.End();
}

TEST_CASE("Saved mappings load with or without a platform field", "[input][sdl]") {
  // Its own name, so no other test has mapped its GUID in this SDL.
  rex::input::sdl::VirtualJoystick pad;
  REQUIRE(pad.Attach("Saved Mapping Pad", 4, 12, 1));
  JoystickInfo info = FindPad(pad.id());
  REQUIRE(info.mapping.empty());

  const auto path = std::filesystem::temp_directory_path() / "rex_user_mappings_test.txt";
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "# a comment\n";
    // Another platform's line for the same pad: skipped.
    file << info.guid << ",Elsewhere,a:b1,platform:NoSuchPlatform,\n";
    // As the assistant saved it before it wrote a platform field.
    file << info.guid << ",Saved Mapping Pad,a:b3,start:b7,\n";
  }
  CHECK(rex::input::sdl::LoadUserMappings(path.string()) == 1);
  const std::string mapping = FindPad(pad.id()).mapping;
  CHECK(mapping.find("a:b3") != std::string::npos);
  CHECK(mapping.find("Elsewhere") == std::string::npos);
  std::filesystem::remove(path);
  CHECK(rex::input::sdl::LoadUserMappings("") == 0);
}

TEST_CASE("A virtual wheel lists as a racing wheel", "[input][sdl]") {
  rex::input::sdl::VirtualJoystick wheel;
  REQUIRE(wheel.Attach("Mapping Test Wheel", 4, 12, 1, true));
  CHECK(FindPad(wheel.id()).wheel_type);
  VirtualPad pad;
  REQUIRE(pad.ok());
  CHECK_FALSE(FindPad(pad.id()).wheel_type);
}
