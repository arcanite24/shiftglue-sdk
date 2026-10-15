#pragma once

#include <cstdint>
#include <string_view>

#include <rex/cvar.h>

// Racing wheels through the SDL gamepad path. A wheel is a gamepad whose
// steering is mapped to the left stick's X axis and whose pedals are mapped to
// the triggers (SDL's database or the mapping assistant). The game shapes
// that input for a thumbstick, so a wheel's steering and pedals are reshaped
// here before the guest reads them; every other controller is untouched.
REXCVAR_DECLARE(std::string, hid_wheel_guids);
REXCVAR_DECLARE(int32_t, hid_wheel_steering_saturation);
REXCVAR_DECLARE(int32_t, hid_wheel_steering_deadzone);
REXCVAR_DECLARE(int32_t, hid_wheel_steering_linearity);
REXCVAR_DECLARE(int32_t, hid_wheel_pedal_deadzone);
REXCVAR_DECLARE(int32_t, hid_wheel_steering_centre);

namespace rex::input::sdl {

struct WheelShaping {
  // Percent of the wheel's travel either side of centre that reaches full
  // lock (100 = the wheel's whole rotation).
  int saturation = 100;
  // Percent of the output skipped at the first movement off centre, to cancel
  // a stick deadzone the game applies after us.
  int deadzone = 0;
  // Percent: 100 is linear, above makes the centre less sensitive (power
  // curve with exponent linearity / 100).
  int linearity = 100;
  // Percent of each pedal's travel ignored at rest.
  int pedal_deadzone = 0;
  // Tenths of a percent of the wheel's travel either side of centre read as
  // exactly centred, before the rest of the shaping. A wheel rarely rests at
  // exactly 0 (a T248 rests about 1.3% off), and the steering deadzone jumps
  // any reading off 0 past the game's stick deadzone.
  int centre = 0;
};

// The shaping the cvars hold now.
WheelShaping CurrentWheelShaping();
int16_t ShapeWheelSteering(int16_t value, const WheelShaping& shaping);
uint8_t ShapeWheelPedal(uint8_t value, const WheelShaping& shaping);

// The player marked this joystick GUID as a wheel (hid_wheel_guids).
bool IsListedWheel(std::string_view guid);
// Adds or removes a GUID in hid_wheel_guids (the caller saves the config).
void SetListedWheel(std::string_view guid, bool wheel);

}  // namespace rex::input::sdl
