#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/input/sdl/wheel.h>

using rex::input::sdl::ShapeWheelPedal;
using rex::input::sdl::ShapeWheelSteering;
using rex::input::sdl::WheelShaping;

TEST_CASE("Default wheel shaping passes input through", "[input][wheel]") {
  const WheelShaping shaping;
  for (int value : {-32768, -20000, -1, 0, 1, 12345, 32767}) {
    CHECK(ShapeWheelSteering(int16_t(value), shaping) == value);
  }
  for (int value : {0, 1, 128, 255}) {
    CHECK(ShapeWheelPedal(uint8_t(value), shaping) == value);
  }
}

TEST_CASE("Steering saturation reaches full lock early", "[input][wheel]") {
  WheelShaping shaping;
  shaping.saturation = 50;
  CHECK(ShapeWheelSteering(16384, shaping) == 32767);
  CHECK(ShapeWheelSteering(-16384, shaping) == -32768);
  CHECK(ShapeWheelSteering(8192, shaping) == 16384);
  CHECK(ShapeWheelSteering(32767, shaping) == 32767);
}

TEST_CASE("Steering deadzone skips the start of the output", "[input][wheel]") {
  WheelShaping shaping;
  shaping.deadzone = 25;
  CHECK(ShapeWheelSteering(0, shaping) == 0);
  // The first step off centre jumps past a quarter of the travel.
  CHECK(ShapeWheelSteering(1, shaping) >= 8192);
  CHECK(ShapeWheelSteering(-1, shaping) <= -8192);
  CHECK(ShapeWheelSteering(32767, shaping) == 32767);
}

TEST_CASE("Steering centre band holds a wheel resting off zero", "[input][wheel]") {
  WheelShaping shaping;
  shaping.deadzone = 25;
  shaping.centre = 15;  // 1.5% of the travel
  // A T248 at rest reads about -440: centred, not a quarter of lock.
  CHECK(ShapeWheelSteering(-440, shaping) == 0);
  CHECK(ShapeWheelSteering(440, shaping) == 0);
  // Past the band the deadzone jump applies, and full lock stays full.
  CHECK(ShapeWheelSteering(1000, shaping) >= 8192);
  CHECK(ShapeWheelSteering(-1000, shaping) <= -8192);
  CHECK(ShapeWheelSteering(32767, shaping) == 32767);
  CHECK(ShapeWheelSteering(-32768, shaping) == -32768);
  // Without a deadzone the output starts from 0 at the band's edge.
  shaping.deadzone = 0;
  CHECK(ShapeWheelSteering(500, shaping) < 100);
}

TEST_CASE("Shifter gears parse and map held buttons to FH1's gear value", "[input][wheel]") {
  using rex::input::sdl::ParseShifterGears;
  using rex::input::sdl::ShifterGearValue;
  const auto layout = ParseShifterGears("1:b0,2:b1,3:b2,4:b3,5:b4,6:b5,R:b6");
  CHECK_FALSE(layout.empty());
  CHECK(ShifterGearValue(layout, 0) == 0);  // neutral
  CHECK(ShifterGearValue(layout, 1u << 0) == 1);
  CHECK(ShifterGearValue(layout, 1u << 5) == 6);
  CHECK(ShifterGearValue(layout, 1u << 6) == -1);  // reverse
  CHECK(ShifterGearValue(layout, 1u << 9) == 0);   // an unmapped button
  // Spaces, a lower-case r, two-digit buttons and junk.
  const auto other = ParseShifterGears(" 1:b10 , r:b11, 9:b1, x:b2, 2:a3, 3:b, 4:b99");
  CHECK(ShifterGearValue(other, 1ull << 10) == 1);
  CHECK(ShifterGearValue(other, 1ull << 11) == -1);
  CHECK(ShifterGearValue(other, 1ull << 1) == 0);
  CHECK(ShifterGearValue(other, 1ull << 3) == 0);
  CHECK(ParseShifterGears("").empty());
}

TEST_CASE("Steering linearity bends the curve, not the ends", "[input][wheel]") {
  WheelShaping shaping;
  shaping.linearity = 200;
  CHECK(ShapeWheelSteering(16384, shaping) == 8192);
  CHECK(ShapeWheelSteering(32767, shaping) == 32767);
  CHECK(ShapeWheelSteering(-32768, shaping) == -32768);
}

TEST_CASE("Pedal deadzone ignores the start and keeps full travel", "[input][wheel]") {
  WheelShaping shaping;
  shaping.pedal_deadzone = 10;
  CHECK(ShapeWheelPedal(20, shaping) == 0);
  CHECK(ShapeWheelPedal(255, shaping) == 255);
  CHECK(ShapeWheelPedal(140, shaping) > 120);
}

TEST_CASE("Wheel GUIDs are listed and removed", "[input][wheel]") {
  rex::cvar::SetFlagByName("hid_wheel_guids", "");
  CHECK_FALSE(rex::input::sdl::IsListedWheel("abc"));
  rex::input::sdl::SetListedWheel("abc", true);
  rex::input::sdl::SetListedWheel("def", true);
  rex::input::sdl::SetListedWheel("abc", true);
  CHECK(rex::cvar::GetFlagByName("hid_wheel_guids") == "abc,def");
  CHECK(rex::input::sdl::IsListedWheel("def"));
  rex::input::sdl::SetListedWheel("abc", false);
  CHECK_FALSE(rex::input::sdl::IsListedWheel("abc"));
  CHECK(rex::cvar::GetFlagByName("hid_wheel_guids") == "def");
  rex::cvar::SetFlagByName("hid_wheel_guids", "");
}
