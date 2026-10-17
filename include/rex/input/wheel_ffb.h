#pragma once
/**
 * @file        rex/input/wheel_ffb.h
 * @brief       Racing wheel force feedback: plays the title's own steering
 *              torque on an SDL haptic wheel as a constant force, with a light
 *              damper and the title's impacts as vibration.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/input/dualsense.h>

// Settings (F6 > CONTROLS > RACING WHEEL). Off until the player turns it on.
REXCVAR_DECLARE(bool, hid_wheel_ffb);
REXCVAR_DECLARE(int32_t, hid_wheel_ffb_strength);
REXCVAR_DECLARE(int32_t, hid_wheel_ffb_torque);
REXCVAR_DECLARE(int32_t, hid_wheel_ffb_min_force);
REXCVAR_DECLARE(int32_t, hid_wheel_ffb_damping);
REXCVAR_DECLARE(int32_t, hid_wheel_ffb_effects);
REXCVAR_DECLARE(bool, hid_wheel_ffb_invert);
REXCVAR_DECLARE(std::string, hid_wheel_ffb_device);
REXCVAR_DECLARE(bool, hid_wheel_ffb_log);

namespace rex::input::wheel_ffb {

struct Settings {
  bool enabled = false;
  int strength = 50;  // device gain, percent of the wheel's maximum force
  int torque = 100;   // percent of the title's steering torque
  // Percent of the wheel's maximum force any nonzero torque starts from, to
  // get past a geared or belted wheel's own friction.
  int min_force = 0;
  int damping = 100;    // percent of the default damper
  int effects = 100;    // percent of the title's impacts passed on as vibration
  bool invert = false;  // for a wheel whose constant force runs the other way
};

Settings CurrentSettings();

struct Inputs {
  bool driving = false;  // the player car is driven and the game has the input
  const dualsense::CarTelemetry* telemetry = nullptr;  // fresh telemetry or null
  float motor_low = 0.0f;                              // 0-1, the title's left (heavy) rumble motor
};

/// What the wheel should do now. Levels are 0-1, the constant force -1 to 1.
struct Forces {
  bool active = false;  // false: every effect stopped, the wheel is free
  float gain = 0.0f;    // device gain
  float constant = 0.0f;
  float damper = 0.0f;
  float vibration = 0.0f;
  uint16_t vibration_period_ms = 0;
  bool operator==(const Forces&) const = default;
};

// A light damper on top of the title's torque, which damps the steering's
// velocity itself.
constexpr float kDamper = 0.05f;
// Below this torque the minimum force fades in linearly, so the title's
// small surface noise near centre does not chatter at the minimum.
constexpr float kMinForceRamp = 0.01f;
// Vibration comes from the title's low (heavy) rumble motor only, above this
// level: impacts and rumble strips. Its high motor is tyre scrub, which on a
// T248 ran about half of the time while driving and buzzed through the rim;
// the steering torque already carries the tyres.
constexpr float kImpactThreshold = 0.1f;
constexpr uint16_t kImpactPeriodMs = 60;

/// One step. Pure: the same inputs give the same forces. Active only with the
/// title's steering torque (CarTelemetry::has_steer_torque).
Forces Step(const Inputs& inputs, const Settings& settings);

}  // namespace rex::input::wheel_ffb
