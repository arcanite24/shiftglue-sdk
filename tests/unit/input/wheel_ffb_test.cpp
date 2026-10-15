#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rex/input/wheel_ffb.h>

using Catch::Approx;
using rex::input::dualsense::CarTelemetry;
namespace ffb = rex::input::wheel_ffb;

namespace {

ffb::Settings On() {
  ffb::Settings settings;
  settings.enabled = true;
  return settings;
}

CarTelemetry Torque(float torque) {
  CarTelemetry t;
  t.valid = true;
  t.speed_mps = 20.0f;
  t.has_steer_torque = true;
  t.steer_torque = torque;
  return t;
}

ffb::Forces StepWith(const CarTelemetry& telemetry, const ffb::Settings& settings = On(),
                     float motor_low = 0.0f) {
  ffb::Inputs inputs;
  inputs.driving = true;
  inputs.telemetry = &telemetry;
  inputs.motor_low = motor_low;
  return ffb::Step(inputs, settings);
}

}  // namespace

TEST_CASE("Wheel forces stay off unless enabled, driving and given a torque",
          "[input][wheel_ffb]") {
  const CarTelemetry t = Torque(0.5f);
  ffb::Inputs inputs;
  inputs.driving = true;
  inputs.telemetry = &t;
  CHECK_FALSE(ffb::Step(inputs, ffb::Settings{}).active);

  inputs.driving = false;
  CHECK_FALSE(ffb::Step(inputs, On()).active);

  inputs.driving = true;
  inputs.telemetry = nullptr;
  CHECK_FALSE(ffb::Step(inputs, On()).active);

  CarTelemetry stale = t;
  stale.valid = false;
  inputs.telemetry = &stale;
  CHECK_FALSE(ffb::Step(inputs, On()).active);

  CarTelemetry no_torque = t;
  no_torque.has_steer_torque = false;
  inputs.telemetry = &no_torque;
  CHECK_FALSE(ffb::Step(inputs, On()).active);

  inputs.telemetry = &t;
  const ffb::Forces forces = ffb::Step(inputs, On());
  CHECK(forces.active);
  CHECK(forces.gain == Approx(0.5f));
  CHECK(forces.damper == Approx(ffb::kDamper));
}

TEST_CASE("The title's steering torque becomes the wheel's constant force", "[input][wheel_ffb]") {
  // A force from the side being steered to, pushing back toward centre.
  CHECK(StepWith(Torque(-0.6f)).constant == Approx(0.6f));
  CHECK(StepWith(Torque(0.6f)).constant == Approx(-0.6f));
  CHECK(StepWith(Torque(0.0f)).constant == Approx(0.0f));

  ffb::Settings settings = On();
  settings.invert = true;
  settings.torque = 150;
  CHECK(StepWith(Torque(-0.6f), settings).constant == Approx(-0.9f));
  CHECK(StepWith(Torque(0.9f), settings).constant == Approx(1.0f));  // clamped
}

TEST_CASE("Minimum force lifts small torque past the wheel's friction", "[input][wheel_ffb]") {
  ffb::Settings settings = On();  // strength 50 %
  settings.min_force = 6;
  // What reaches the wheel is the gain times the constant, here in the
  // torque's own sign.
  const auto out = [&](float torque) {
    const ffb::Forces f = StepWith(Torque(torque), settings);
    return -f.gain * f.constant;
  };
  CHECK(out(0.0f) == Approx(0.0f));
  CHECK(out(0.005f) == Approx(0.03f));  // fading in near zero
  CHECK(out(0.06f) == Approx(0.06f + 0.44f * 0.06f));
  CHECK(out(-0.06f) == Approx(-(0.06f + 0.44f * 0.06f)));
  CHECK(out(1.0f) == Approx(0.5f));  // full torque is still the strength
  settings.min_force = 0;
  CHECK(out(0.06f) == Approx(0.5f * 0.06f));
}

TEST_CASE("Only the title's impacts become wheel vibration", "[input][wheel_ffb]") {
  const CarTelemetry t = Torque(0.0f);
  ffb::Forces forces = StepWith(t);
  CHECK(forces.vibration == Approx(0.0f));
  CHECK(forces.vibration_period_ms == 0);

  // The heavy motor's light rumble stays off the wheel.
  CHECK(StepWith(t, On(), ffb::kImpactThreshold).vibration == Approx(0.0f));

  forces = StepWith(t, On(), 1.0f);
  CHECK(forces.vibration == Approx(1.0f));
  CHECK(forces.vibration_period_ms == ffb::kImpactPeriodMs);
  const float halfway = (ffb::kImpactThreshold + 1.0f) / 2.0f;
  CHECK(StepWith(t, On(), halfway).vibration == Approx(0.5f));

  ffb::Settings quiet = On();
  quiet.effects = 50;
  CHECK(StepWith(t, quiet, 1.0f).vibration == Approx(0.5f));
}

TEST_CASE("Bad telemetry never reaches the wheel", "[input][wheel_ffb]") {
  const ffb::Forces forces = StepWith(Torque(std::numeric_limits<float>::quiet_NaN()), On(),
                                      std::numeric_limits<float>::quiet_NaN());
  CHECK(forces.constant == Approx(0.0f));
  CHECK(forces.vibration == Approx(0.0f));
  CHECK(StepWith(Torque(std::numeric_limits<float>::infinity())).constant == Approx(0.0f));
  CHECK(StepWith(Torque(0.0f), On(), 2.0f).vibration == Approx(1.0f));
}
