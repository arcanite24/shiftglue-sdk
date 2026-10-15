#include <rex/input/wheel_ffb.h>

#include <algorithm>
#include <cmath>

REXCVAR_DEFINE_BOOL(hid_wheel_ffb, false, "Input",
                    "Racing wheel force feedback through SDL's haptic API, from the title's own "
                    "steering torque")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(hid_wheel_ffb_strength, 50, "Input",
                     "Racing wheel force feedback: overall strength in percent of the wheel's "
                     "maximum force")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(hid_wheel_ffb_torque, 100, "Input",
                     "Racing wheel force feedback: percent of the title's steering torque")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(hid_wheel_ffb_min_force, 0, "Input",
                     "Racing wheel force feedback: percent of the wheel's maximum force that any "
                     "steering torque starts from, to overcome the wheel's own friction")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(hid_wheel_ffb_damping, 100, "Input",
                     "Racing wheel force feedback: damping in percent of the default")
    .range(0, 300)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(hid_wheel_ffb_effects, 100, "Input",
                     "Racing wheel force feedback: the title's impacts and rumble strips as wheel "
                     "vibration, in percent")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(hid_wheel_ffb_invert, false, "Input",
                    "Racing wheel force feedback: reverse the steering torque, for a wheel whose "
                    "constant force pushes the other way")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(hid_wheel_ffb_device, "", "Input",
                      "Racing wheel force feedback: part of the force feedback device's name, when "
                      "SDL lists several and the wheel's maker is not in the name");
REXCVAR_DEFINE_BOOL(hid_wheel_ffb_log, false, "Input",
                    "Log the racing wheel force feedback levels twice a second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input::wheel_ffb {

namespace {

float Finite(float value) {
  return std::isfinite(value) ? value : 0.0f;
}

}  // namespace

Settings CurrentSettings() {
  Settings settings;
  settings.enabled = REXCVAR_GET(hid_wheel_ffb);
  settings.strength = std::clamp(REXCVAR_GET(hid_wheel_ffb_strength), 0, 100);
  settings.torque = std::clamp(REXCVAR_GET(hid_wheel_ffb_torque), 0, 200);
  settings.min_force = std::clamp(REXCVAR_GET(hid_wheel_ffb_min_force), 0, 30);
  settings.damping = std::clamp(REXCVAR_GET(hid_wheel_ffb_damping), 0, 300);
  settings.effects = std::clamp(REXCVAR_GET(hid_wheel_ffb_effects), 0, 200);
  settings.invert = REXCVAR_GET(hid_wheel_ffb_invert);
  return settings;
}

Forces Step(const Inputs& inputs, const Settings& settings) {
  Forces forces;
  const dualsense::CarTelemetry* t = inputs.telemetry;
  if (!settings.enabled || !inputs.driving || !t || !t->valid || !t->has_steer_torque) {
    return forces;
  }
  forces.active = true;
  forces.gain = float(settings.strength) / 100.0f;

  float torque =
      std::clamp(std::clamp(Finite(t->steer_torque), -1.0f, 1.0f) * float(settings.torque) / 100.0f,
                 -1.0f, 1.0f);
  if (settings.min_force > 0 && forces.gain > 0.0f) {
    // The floor is a share of the wheel's own maximum, so it counts after the
    // device gain: the output runs from the floor to the gain as |torque|
    // runs from 0 to 1, sent as that over the gain.
    const float floor = std::min(float(settings.min_force) / 100.0f, forces.gain);
    const float size = std::fabs(torque);
    const float out =
        size < kMinForceRamp ? floor * size / kMinForceRamp : floor + (forces.gain - floor) * size;
    torque = std::copysign(std::min(out / forces.gain, 1.0f), torque);
  }
  // The torque has the sign of the steering it pushes against. A constant
  // force's sign is the side it comes from (DirectInput's convention, which
  // SDL's steering axis keeps), so a positive force pushes the wheel toward
  // negative steering: the wheel's force is the negated torque (confirmed on a
  // T248, where the unnegated sign pushed the wheel away from centre).
  forces.constant = std::clamp(settings.invert ? torque : -torque, -1.0f, 1.0f);
  forces.damper = std::clamp(kDamper * float(settings.damping) / 100.0f, 0.0f, 1.0f);

  const float low = std::clamp(Finite(inputs.motor_low), 0.0f, 1.0f);
  const float impact =
      low <= kImpactThreshold ? 0.0f : (low - kImpactThreshold) / (1.0f - kImpactThreshold);
  forces.vibration = std::clamp(impact * float(settings.effects) / 100.0f, 0.0f, 1.0f);
  if (forces.vibration > 0.0f) {
    forces.vibration_period_ms = kImpactPeriodMs;
  }
  return forces;
}

}  // namespace rex::input::wheel_ffb
