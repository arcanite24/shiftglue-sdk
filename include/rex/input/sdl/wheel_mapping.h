#pragma once
/**
 * @file        rex/input/sdl/wheel_mapping.h
 * @brief       Racing wheel bindings and force feedback math for the SDL driver.
 *
 * A wheel reaches the guest as one XInput wheel (subtype 2): steering on the
 * left stick's X axis, throttle on the right trigger and brake on the left
 * trigger, which is how XInput reports wheels. Its parts may come from
 * several USB devices (a direct-drive base and a separate pedal set), so each
 * binding names the device it reads. Nothing here touches SDL, so the
 * mapping can be tested without hardware.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/xinputd_ffb.h>

namespace rex::input::sdl {

// Which way an axis travels as the pedal goes down.
enum class AxisDirection : uint8_t {
  kAuto,      // decided from the axis's resting value when the device opens
  kNormal,    // -32768 released, 32767 pressed
  kInverted,  // 32767 released, -32768 pressed
};

// "[device|][+|-]axisN": an axis on the steering device, or on the device
// matched by `device` (see WheelDeviceMatches). "+" forces normal travel, "-"
// inverted; without a sign the direction is detected.
struct WheelAxisBinding {
  std::string device;
  int axis = -1;
  AxisDirection direction = AxisDirection::kAuto;
};

// One entry of "NAME=[device|]N[,...]": button N (or "hN.up", "hN.down",
// "hN.left", "hN.right" for a hat direction, or "[+|-]axisN" for an axis
// past half travel, such as a handbrake lever) presses the pad control NAME
// (names as in pad_remap: A, B, X, Y, LB, RB, LT, RT, LS, RS, BACK, START,
// UP, DOWN, LEFT, RIGHT).
struct WheelButtonBinding {
  std::string device;
  int button = -1;  // >= 0 for a button
  int hat = -1;     // >= 0 for a hat direction
  uint8_t hat_mask = 0;
  int axis = -1;  // >= 0 for an axis used as a button
  AxisDirection axis_direction = AxisDirection::kAuto;
  uint16_t xbuttons = 0;  // pad button bits pressed
  bool left_trigger = false;
  bool right_trigger = false;
};

std::optional<WheelAxisBinding> ParseWheelAxisBinding(std::string_view text);
// Malformed entries are skipped, so a typo loses one button, not all of them.
std::vector<WheelButtonBinding> ParseWheelButtonBindings(std::string_view text);

// `match` is empty (any device), "vvvv:pppp" in hex (USB vendor and
// product), "path:<SDL device path>" (exact, case-insensitive: tells apart
// identical devices such as several vJoy devices), or a case-insensitive
// substring of the device name.
bool WheelDeviceMatches(std::string_view match, std::string_view name, uint16_t vendor,
                        uint16_t product, std::string_view path = {});

// Known wheel bases by USB vendor, for devices SDL does not type as a wheel
// (SDL types most Logitech, Thrustmaster and Fanatec bases already).
bool IsKnownWheelVendor(uint16_t vendor);

// Resolves kAuto from the axis's resting value: a pedal resting in the upper
// half travels downwards.
AxisDirection ResolveAxisDirection(AxisDirection direction, int16_t rest);

// Steering axis to the guest's left stick X. `scale_percent` above 100 needs
// less rotation for full lock; the deadzone is a percentage of half travel.
int16_t SteeringFromAxis(int16_t raw, bool invert, int scale_percent, int deadzone_percent);

// A pedal axis to a trigger value, 0 at rest and 255 fully pressed.
uint8_t PedalFromAxis(int16_t raw, AxisDirection direction, int deadzone_percent);

// The same with a dead zone at each end: travel up to `bottom_percent` reads 0 (rest noise),
// travel past 100 - `top_percent` reads 255 (full throttle or brake without bottoming the
// pedal out), and what lies between is spread over 0..255.
uint8_t PedalFromAxis(int16_t raw, AxisDirection direction, int bottom_percent, int top_percent);

// Pad buttons for an SDL hat value (SDL_HAT_UP = 1, RIGHT = 2, DOWN = 4,
// LEFT = 8).
uint16_t DpadFromHat(uint8_t hat);

enum class WheelFfbMode : uint8_t {
  kOff,
  // The signed difference of the two motors as a constant force on the
  // steering axis: right motor pushes one way, left the other.
  kDifference,
  // The two motors as plain rumble, for bases without constant force.
  kRumble,
  // One motor alone as a signed force, offset binary around 0x8000: for the
  // case where the FFB trace shows the title centring one value and leaving
  // the other for rumble.
  kLeftSigned,
  kRightSigned,
};

WheelFfbMode ParseWheelFfbMode(std::string_view text);

// The constant-force level (SDL's Sint16 scale) for the guest's motors.
int16_t ConstantForceFromMotors(uint16_t left_motor, uint16_t right_motor, int strength_percent,
                                bool invert);

// One motor value read as offset binary (0x8000 no force) to the
// constant-force level.
int16_t ConstantForceFromSignedMotor(uint16_t motor, int strength_percent, bool invert);

// The constant-force level for `mode`; 0 for kOff and kRumble.
int16_t ConstantForceForMode(WheelFfbMode mode, uint16_t left_motor, uint16_t right_motor,
                             int strength_percent, bool invert);

const char* WheelFfbModeName(WheelFfbMode mode);

// How the title's XInputdFF effect values become SDL haptic values. magnitude_max is the
// title's full-scale magnitude and coefficient (the DirectInput nominal 10000 unless the trace
// shows otherwise), saturation_max its full-scale saturation and dead band, and gains are 0..255.
struct WheelEffectScale {
  int strength_percent = 100;
  int magnitude_max = 10000;
  int saturation_max = 10000;
  bool invert = false;
  // Constant force only: any non-zero force is lifted to at least this share of full scale,
  // so small forces get past a geared or belted wheel's friction.
  int min_force_percent = 0;
  // Condition effects only: a further scale on their coefficients (the spring's, the damper's).
  int condition_percent = 100;
};

// SDL's constant-force level for a constant effect: magnitude x gain x strength, reversed by
// invert.
int16_t EffectConstantLevel(const xinputd_ffb::Effect& effect, const WheelEffectScale& scale);

struct WheelCondition {
  int16_t right_coeff = 0;  // the positive side
  int16_t left_coeff = 0;
  uint16_t right_sat = 0;
  uint16_t left_sat = 0;
  uint16_t deadband = 0;
  int16_t center = 0;
};

// SDL's condition values (spring, damper, inertia, friction). Gain and strength scale the
// coefficients; Forza Horizon's spring gain follows the car's speed.
WheelCondition EffectCondition(const xinputd_ffb::Effect& effect, const WheelEffectScale& scale);

}  // namespace rex::input::sdl
