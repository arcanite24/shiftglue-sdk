#include <catch2/catch_test_macros.hpp>

#include <rex/input/sdl/wheel_mapping.h>

using namespace rex::input;
using namespace rex::input::sdl;

TEST_CASE("Wheel axis bindings parse device, sign and index", "[input][wheel]") {
  auto plain = ParseWheelAxisBinding("axis2");
  REQUIRE(plain);
  CHECK(plain->device.empty());
  CHECK(plain->axis == 2);
  CHECK(plain->direction == AxisDirection::kAuto);

  auto pedals = ParseWheelAxisBinding(" SimJack | -a1 ");
  REQUIRE(pedals);
  CHECK(pedals->device == "SimJack");
  CHECK(pedals->axis == 1);
  CHECK(pedals->direction == AxisDirection::kInverted);

  auto ids = ParseWheelAxisBinding("0eb7:0020|+axis0");
  REQUIRE(ids);
  CHECK(ids->device == "0eb7:0020");
  CHECK(ids->direction == AxisDirection::kNormal);

  CHECK_FALSE(ParseWheelAxisBinding(""));
  CHECK_FALSE(ParseWheelAxisBinding("button3"));
  CHECK_FALSE(ParseWheelAxisBinding("axis"));
  CHECK_FALSE(ParseWheelAxisBinding("axis-1"));
}

TEST_CASE("Wheel button bindings map buttons and hats to pad controls", "[input][wheel]") {
  auto bindings =
      ParseWheelButtonBindings("A=0, START=9,10, UP=h0.up, LB=Shifter|b3, RT=4, NOPE=5, B=x");
  REQUIRE(bindings.size() == 6);
  CHECK(bindings[0].button == 0);
  CHECK(bindings[0].xbuttons == X_INPUT_GAMEPAD_A);
  CHECK(bindings[1].button == 9);
  CHECK(bindings[2].button == 10);
  CHECK(bindings[2].xbuttons == X_INPUT_GAMEPAD_START);
  CHECK(bindings[3].hat == 0);
  CHECK(bindings[3].hat_mask == 0x01);
  CHECK(bindings[3].xbuttons == X_INPUT_GAMEPAD_DPAD_UP);
  CHECK(bindings[4].device == "Shifter");
  CHECK(bindings[4].button == 3);
  CHECK(bindings[4].xbuttons == X_INPUT_GAMEPAD_LEFT_SHOULDER);
  CHECK(bindings[5].right_trigger);
  CHECK(bindings[5].xbuttons == 0);
  CHECK(ParseWheelButtonBindings("").empty());
}

TEST_CASE("Wheel devices match by USB IDs or name", "[input][wheel]") {
  CHECK(WheelDeviceMatches("", "Anything", 1, 2));
  CHECK(WheelDeviceMatches("0EB7:0020", "FANATEC CSL DD", 0x0EB7, 0x0020));
  CHECK_FALSE(WheelDeviceMatches("0eb7:0021", "FANATEC CSL DD", 0x0EB7, 0x0020));
  CHECK(WheelDeviceMatches("csl dd", "FANATEC CSL DD", 0, 0));
  CHECK_FALSE(WheelDeviceMatches("pedals", "FANATEC CSL DD", 0, 0));
}

TEST_CASE("Pedal direction is detected from the resting value", "[input][wheel]") {
  CHECK(ResolveAxisDirection(AxisDirection::kAuto, -32768) == AxisDirection::kNormal);
  CHECK(ResolveAxisDirection(AxisDirection::kAuto, 32767) == AxisDirection::kInverted);
  CHECK(ResolveAxisDirection(AxisDirection::kNormal, 32767) == AxisDirection::kNormal);

  CHECK(PedalFromAxis(-32768, AxisDirection::kNormal, 0) == 0);
  CHECK(PedalFromAxis(32767, AxisDirection::kNormal, 0) == 255);
  CHECK(PedalFromAxis(32767, AxisDirection::kInverted, 0) == 0);
  CHECK(PedalFromAxis(-32768, AxisDirection::kInverted, 0) == 255);
  // A pedal resting a little off its end stays released inside the deadzone.
  CHECK(PedalFromAxis(-31000, AxisDirection::kNormal, 5) == 0);
  CHECK(PedalFromAxis(32767, AxisDirection::kNormal, 5) == 255);
}

TEST_CASE("Steering keeps its sign through deadzone and scale", "[input][wheel]") {
  CHECK(SteeringFromAxis(0, false, 100, 0) == 0);
  CHECK(SteeringFromAxis(32767, false, 100, 0) == 32767);
  CHECK(SteeringFromAxis(-32768, false, 100, 0) == -32767);
  CHECK(SteeringFromAxis(16384, false, 200, 0) == 32767);
  CHECK(SteeringFromAxis(16384, true, 100, 0) < 0);
  CHECK(SteeringFromAxis(1000, false, 100, 5) == 0);
  CHECK(SteeringFromAxis(32767, false, 100, 5) == 32767);
}

TEST_CASE("Hats become the d-pad", "[input][wheel]") {
  CHECK(DpadFromHat(0) == 0);
  CHECK(DpadFromHat(0x01 | 0x02) == (X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_RIGHT));
  CHECK(DpadFromHat(0x0C) == (X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_LEFT));
}

TEST_CASE("Motor values become a signed constant force", "[input][wheel]") {
  CHECK(ConstantForceFromMotors(0, 0, 100, false) == 0);
  CHECK(ConstantForceFromMotors(0, 65535, 100, false) == 32767);
  CHECK(ConstantForceFromMotors(65535, 0, 100, false) == -32767);
  CHECK(ConstantForceFromMotors(65535, 0, 100, true) == 32767);
  CHECK(ConstantForceFromMotors(0, 65535, 50, false) == 16383);
  CHECK(ConstantForceFromMotors(30000, 30000, 100, false) == 0);
  CHECK(ConstantForceFromMotors(0, 65535, 0, false) == 0);

  CHECK(ParseWheelFfbMode("off") == WheelFfbMode::kOff);
  CHECK(ParseWheelFfbMode("Rumble") == WheelFfbMode::kRumble);
  CHECK(ParseWheelFfbMode("difference") == WheelFfbMode::kDifference);
}

TEST_CASE("One motor read as offset binary becomes a signed force", "[input][wheel]") {
  CHECK(ConstantForceFromSignedMotor(0x8000, 100, false) == 0);
  CHECK(ConstantForceFromSignedMotor(0xFFFF, 100, false) == 32767);
  CHECK(ConstantForceFromSignedMotor(0x0000, 100, false) == -32767);
  CHECK(ConstantForceFromSignedMotor(0x0000, 100, true) == 32767);
  CHECK(ConstantForceFromSignedMotor(0xFFFF, 50, false) == 16383);

  CHECK(ParseWheelFfbMode("left-signed") == WheelFfbMode::kLeftSigned);
  CHECK(ParseWheelFfbMode("RIGHT-SIGNED") == WheelFfbMode::kRightSigned);
  CHECK(ConstantForceForMode(WheelFfbMode::kLeftSigned, 0xFFFF, 0x8000, 100, false) == 32767);
  CHECK(ConstantForceForMode(WheelFfbMode::kRightSigned, 0xFFFF, 0x8000, 100, false) == 0);
  CHECK(ConstantForceForMode(WheelFfbMode::kDifference, 0, 65535, 100, false) == 32767);
  CHECK(ConstantForceForMode(WheelFfbMode::kRumble, 0, 65535, 100, false) == 0);
  CHECK(ConstantForceForMode(WheelFfbMode::kOff, 0, 65535, 100, false) == 0);
  CHECK(std::string(WheelFfbModeName(WheelFfbMode::kRightSigned)) == "right-signed");
}

TEST_CASE("Devices match by path to tell identical devices apart", "[input][wheel]") {
  const char* path = R"(\?\HID#HIDCLASS&Col02#1&2d595ca7&0&0001)";
  CHECK(WheelDeviceMatches(R"(path:\?\hid#hidclass&col02#1&2d595ca7&0&0001)", "vJoy Device", 0x1234,
                           0xBEAD, path));
  CHECK_FALSE(WheelDeviceMatches(R"(path:\?\HID#HIDCLASS&Col01#1&2d595ca7&0&0000)", "vJoy Device",
                                 0x1234, 0xBEAD, path));
  CHECK_FALSE(WheelDeviceMatches("path:anything", "vJoy Device", 0x1234, 0xBEAD, ""));
  CHECK(WheelDeviceMatches("vjoy", "vJoy Device", 0x1234, 0xBEAD, path));
  CHECK(WheelDeviceMatches("1234:bead", "vJoy Device", 0x1234, 0xBEAD, path));
  auto axis = ParseWheelAxisBinding(R"(path:\?\HID#X|-axis2)");
  REQUIRE(axis);
  CHECK(axis->device == R"(path:\?\HID#X)");
  CHECK(axis->axis == 2);
  CHECK(axis->direction == AxisDirection::kInverted);
  auto buttons = ParseWheelButtonBindings(R"(A=path:\?\HID#X|3,B=7)");
  REQUIRE(buttons.size() == 2);
  CHECK(buttons[0].device == R"(path:\?\HID#X)");
  CHECK(buttons[0].button == 3);
  CHECK(buttons[1].device.empty());
}

TEST_CASE("An axis past half travel can press a button", "[input][wheel]") {
  auto buttons = ParseWheelButtonBindings("A=Handbrake|-axis3,B=+axis1,X=axis0,Y=axisZ");
  REQUIRE(buttons.size() == 3);
  CHECK(buttons[0].device == "Handbrake");
  CHECK(buttons[0].axis == 3);
  CHECK(buttons[0].axis_direction == AxisDirection::kInverted);
  CHECK(buttons[0].button == -1);
  CHECK(buttons[0].xbuttons == X_INPUT_GAMEPAD_A);
  CHECK(buttons[1].axis == 1);
  CHECK(buttons[1].axis_direction == AxisDirection::kNormal);
  CHECK(buttons[2].axis == 0);
  CHECK(buttons[2].axis_direction == AxisDirection::kAuto);
}

TEST_CASE("Parses the bindings PinyonWheelSetup writes", "[input][wheel]") {
  auto buttons = ParseWheelButtonBindings(
      "A=0,B=1,Y=3,LB=4,RB=5,LS=6,RS=16d0:0d5a|0,BACK=+axis5,START=9,UP=h0.up,DOWN=h0.down,"
      "LEFT=h0.left,RIGHT=h0.right");
  REQUIRE(buttons.size() == 13);
  CHECK(buttons[6].device == "16d0:0d5a");
  CHECK(buttons[6].button == 0);
  CHECK(buttons[6].xbuttons == X_INPUT_GAMEPAD_RIGHT_THUMB);
  CHECK(buttons[7].axis == 5);
  CHECK(buttons[7].axis_direction == AxisDirection::kNormal);
  CHECK(buttons[7].xbuttons == X_INPUT_GAMEPAD_BACK);
  CHECK(buttons[9].hat == 0);
  CHECK(buttons[9].hat_mask == 0x01);
  auto brake = ParseWheelAxisBinding("16d0:0d5a|-axis1");
  REQUIRE(brake);
  CHECK(brake->device == "16d0:0d5a");
  CHECK(brake->axis == 1);
  CHECK(brake->direction == AxisDirection::kInverted);
  CHECK(WheelDeviceMatches("16d0:0d5a", "SimJack Pro Pedals", 0x16D0, 0x0D5A));
}

namespace {

// A 116-byte big-endian descriptor laid out as Forza Horizon's wheel set-up builds one.
std::array<uint8_t, 116> Descriptor(uint32_t id, uint32_t type, uint32_t gain, int32_t magnitude,
                                    int32_t pc, int32_t nc, int32_t ps, int32_t ns, int32_t db) {
  std::array<uint32_t, 29> words{};
  words[0] = id;
  words[1] = type;
  words[2] = 4;
  words[3] = gain;
  words[4] = 4080;
  words[7] = 255;
  words[8] = 255;
  words[11] = uint32_t(magnitude);
  words[12] = uint32_t(pc);
  words[13] = uint32_t(nc);
  words[14] = uint32_t(ps);
  words[15] = uint32_t(ns);
  words[16] = uint32_t(db);
  std::array<uint8_t, 116> bytes{};
  for (size_t i = 0; i < words.size(); i++) {
    bytes[i * 4] = uint8_t(words[i] >> 24);
    bytes[i * 4 + 1] = uint8_t(words[i] >> 16);
    bytes[i * 4 + 2] = uint8_t(words[i] >> 8);
    bytes[i * 4 + 3] = uint8_t(words[i]);
  }
  return bytes;
}

}  // namespace

TEST_CASE("Wheel effect descriptors decode from big-endian guest memory", "[input][wheel]") {
  auto spring = Descriptor(4, 8, 255, 0, 6000, 6000, 9000, 9000, 100);
  const auto e = rex::input::xinputd_ffb::DecodeEffect(spring.data());
  CHECK(e.id == 4);
  CHECK(e.type == rex::input::xinputd_ffb::kSpring);
  CHECK(e.gain == 255);
  CHECK(e.positive_coefficient == 6000);
  CHECK(e.negative_coefficient == 6000);
  CHECK(e.positive_saturation == 9000);
  CHECK(e.dead_band == 100);
  CHECK(e.words[4] == 4080);
  CHECK(rex::input::xinputd_ffb::IsConditionType(e.type));
  CHECK(std::string(rex::input::xinputd_ffb::EffectTypeName(10)) == "inertia");

  auto constant = Descriptor(5, 1, 128, -5000, 0, 0, 0, 0, 0);
  const auto c = rex::input::xinputd_ffb::DecodeEffect(constant.data());
  CHECK(c.type == rex::input::xinputd_ffb::kConstant);
  CHECK(c.magnitude == -5000);
  CHECK_FALSE(rex::input::xinputd_ffb::IsConditionType(c.type));
}

TEST_CASE("Constant effects scale by magnitude, gain, strength and invert", "[input][wheel]") {
  rex::input::xinputd_ffb::Effect e;
  e.type = rex::input::xinputd_ffb::kConstant;
  e.gain = 255;
  e.magnitude = 10000;
  WheelEffectScale scale;
  CHECK(EffectConstantLevel(e, scale) == 32767);
  e.magnitude = -10000;
  CHECK(EffectConstantLevel(e, scale) == -32767);
  scale.invert = true;
  CHECK(EffectConstantLevel(e, scale) == 32767);
  scale.invert = false;
  scale.strength_percent = 25;
  CHECK(EffectConstantLevel(e, scale) == -8191);
  scale.strength_percent = 100;
  e.gain = 0;
  CHECK(EffectConstantLevel(e, scale) == 0);
  e.gain = 255;
  e.magnitude = 50000;  // beyond full scale clamps
  CHECK(EffectConstantLevel(e, scale) == 32767);
}

TEST_CASE("Condition effects scale coefficients and saturations", "[input][wheel]") {
  rex::input::xinputd_ffb::Effect e;
  e.type = rex::input::xinputd_ffb::kSpring;
  e.gain = 255;
  e.positive_coefficient = 5000;
  e.negative_coefficient = 10000;
  e.positive_saturation = 10000;
  e.negative_saturation = 5000;
  e.dead_band = 0;
  WheelEffectScale scale;
  auto c = EffectCondition(e, scale);
  CHECK(c.right_coeff == 16383);
  CHECK(c.left_coeff == 32767);
  CHECK(c.right_sat == 65535);
  CHECK(c.left_sat == 32767);
  CHECK(c.deadband == 0);
  // The speed-dependent spring: half gain halves the coefficients only.
  e.gain = 127;
  c = EffectCondition(e, scale);
  CHECK(c.left_coeff == 16319);
  CHECK(c.right_sat == 65535);
  // Byte-range saturations.
  scale.saturation_max = 255;
  e.positive_saturation = 255;
  CHECK(EffectCondition(e, scale).right_sat == 65535);
  // Strength.
  e.gain = 255;
  scale.strength_percent = 50;
  CHECK(EffectCondition(e, scale).left_coeff == 16383);
}

TEST_CASE("Pedals take a dead zone at each end", "[input][wheel]") {
  // Normal travel: -32768 released, 32767 pressed.
  CHECK(PedalFromAxis(-32768, AxisDirection::kNormal, 5, 10) == 0);
  CHECK(PedalFromAxis(32767, AxisDirection::kNormal, 5, 10) == 255);
  // 4 % of travel is inside a 5 % bottom dead zone.
  CHECK(PedalFromAxis(int16_t(-32768 + 65535 * 4 / 100), AxisDirection::kNormal, 5, 0) == 0);
  // 92 % of travel is past a 10 % top dead zone: fully pressed.
  CHECK(PedalFromAxis(int16_t(-32768 + 65535 * 92 / 100), AxisDirection::kNormal, 0, 10) == 255);
  // Halfway between the dead zones reads half.
  const int16_t mid = int16_t(-32768 + 65535 * 5 / 100 + (65535 * 85 / 100) / 2);
  CHECK(PedalFromAxis(mid, AxisDirection::kNormal, 5, 10) >= 126);
  CHECK(PedalFromAxis(mid, AxisDirection::kNormal, 5, 10) <= 128);
  // Inverted pedals too.
  CHECK(PedalFromAxis(32767, AxisDirection::kInverted, 5, 10) == 0);
  CHECK(PedalFromAxis(-32768, AxisDirection::kInverted, 5, 10) == 255);
  // No top dead zone matches the old single dead zone.
  for (int16_t raw :
       {int16_t(-32768), int16_t(-10000), int16_t(0), int16_t(20000), int16_t(32767)}) {
    CHECK(PedalFromAxis(raw, AxisDirection::kNormal, 3, 0) ==
          PedalFromAxis(raw, AxisDirection::kNormal, 3));
  }
}

TEST_CASE("Minimum force lifts small constant forces; condition gains scale coefficients",
          "[input][wheel]") {
  rex::input::xinputd_ffb::Effect e;
  e.type = rex::input::xinputd_ffb::kConstant;
  e.gain = 255;
  WheelEffectScale scale;
  scale.magnitude_max = 127;
  e.magnitude = 0;
  scale.min_force_percent = 10;
  CHECK(EffectConstantLevel(e, scale) == 0);  // no force stays no force
  e.magnitude = 1;
  CHECK(EffectConstantLevel(e, scale) >= 3276);  // lifted to the 10 % floor
  e.magnitude = -127;
  CHECK(EffectConstantLevel(e, scale) == -32767);  // full scale stays full scale
  scale.invert = true;
  CHECK(EffectConstantLevel(e, scale) == 32767);
  scale.invert = false;
  scale.min_force_percent = 0;
  e.magnitude = 64;
  CHECK(EffectConstantLevel(e, scale) == 16512);

  rex::input::xinputd_ffb::Effect spring;
  spring.type = rex::input::xinputd_ffb::kSpring;
  spring.gain = 255;
  spring.positive_coefficient = 63;
  spring.negative_coefficient = 63;
  WheelEffectScale cs;
  cs.magnitude_max = 127;
  cs.saturation_max = 255;
  const int16_t full = EffectCondition(spring, cs).right_coeff;
  cs.condition_percent = 50;
  CHECK(EffectCondition(spring, cs).right_coeff == full / 2);
  cs.condition_percent = 0;
  CHECK(EffectCondition(spring, cs).right_coeff == 0);
}
