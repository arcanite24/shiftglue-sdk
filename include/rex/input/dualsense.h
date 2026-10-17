#pragma once
/**
 * @file        rex/input/dualsense.h
 * @brief       DualSense adaptive triggers and lightbar: effect bytes, the
 *              rate-limited effect channel and the racing trigger model.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include <rex/cvar.h>

// Settings (F6 > CONTROLS > DUALSENSE). Everything is off until a pad has
// been checked on hardware for the platform.
REXCVAR_DECLARE(std::string, pad_ds_triggers);
REXCVAR_DECLARE(int32_t, pad_ds_trigger_strength);
REXCVAR_DECLARE(bool, pad_ds_gear_click);
REXCVAR_DECLARE(std::string, pad_ds_lightbar);
REXCVAR_DECLARE(bool, pad_ds_player_leds);
REXCVAR_DECLARE(bool, pad_ds_log_reports);

// The DualSense output report as SDL's PS5 driver takes it in
// SDL_SendGamepadEffect: 47 bytes after the report ID, the layout of SDL's
// DS5EffectsState_t. Trigger blocks are 11 bytes, mode byte first; the modes
// and parameter packing follow the public TriggerEffectGenerator (MIT). Our
// payloads set only the trigger enable bits, so SDL's rumble and LED reports
// and ours never overwrite each other's fields; a report without a trigger's
// bit leaves that trigger as it was (checked on a DualSense over USB through
// the trigger status the pad reports back in input report byte 48).
namespace rex::input::dualsense {

constexpr uint16_t kSonyVendorId = 0x054C;
constexpr uint16_t kDualSenseProductId = 0x0CE6;
constexpr uint16_t kDualSenseEdgeProductId = 0x0DF2;

/// Sony's DualSense and DualSense Edge. Other pads SDL reports as PS5 types
/// get glyphs and rumble but never trigger bytes.
bool IsDualSense(uint16_t vendor_id, uint16_t product_id);

constexpr size_t kTriggerEffectSize = 11;
constexpr size_t kEffectPayloadSize = 47;
constexpr size_t kRightTriggerOffset = 10;
constexpr size_t kLeftTriggerOffset = 21;
constexpr uint8_t kEnableRightTrigger = 0x04;
constexpr uint8_t kEnableLeftTrigger = 0x08;

using TriggerEffect = std::array<uint8_t, kTriggerEffectSize>;
using EffectPayload = std::array<uint8_t, kEffectPayloadSize>;

// Builders. Out-of-range arguments give Off, as the reference generator does.
// Positions are the trigger's ten zones, 0 at rest and 9 fully pressed;
// strengths and amplitudes 1-8 unless noted.
TriggerEffect Off();
/// Constant resistance from `position` to the end.
TriggerEffect Feedback(int position, int strength);
/// Resistance per zone (0 = none, 1-8).
TriggerEffect MultiFeedback(const std::array<int, 10>& strengths);
/// Resistance ramping from start_strength at `start` to end_strength at `end`
/// and held after it.
TriggerEffect SlopeFeedback(int start, int end, int start_strength, int end_strength);
/// Resistance between start (2-7) and end (start+1..8), then a release.
TriggerEffect Weapon(int start, int end, int strength);
/// A buzz from `position` on at `frequency` Hz (1-255).
TriggerEffect Vibration(int position, int amplitude, int frequency);
/// Alternation between amplitudes a and b (0-7) between start (1-8) and end
/// (start+1..9) at `frequency` Hz, switching every `period` tenths of a
/// second; the usual ABS pulse.
TriggerEffect Machine(int start, int end, int amplitude_a, int amplitude_b, int frequency,
                      int period);
/// Resistance between start and end that snaps back with `snap` (1-8).
TriggerEffect Bow(int start, int end, int strength, int snap);

/// The mode byte's effect family as the pad reports it back in its input
/// report (byte 48: right in the low nibble, left in the high), as read from a
/// DualSense over USB: 0 off, 1 feedback, 2 weapon, 3 vibration, 4 bow,
/// 6 machine.
int ReportedFamily(const TriggerEffect& effect);

struct TriggerEffects {
  TriggerEffect left = Off();   // L2
  TriggerEffect right = Off();  // R2
  bool operator==(const TriggerEffects&) const = default;
};

/// Payload that sets both triggers and nothing else.
EffectPayload BuildEffectPayload(const TriggerEffects& effects);

/// One pad's trigger output: sends only when the effects changed, and at most
/// once per min_interval_ns (60 reports a second by default) so Bluetooth
/// stacks are never flooded; a change held back by the limit goes out on the
/// next Update after it.
class EffectChannel {
 public:
  static constexpr uint64_t kDefaultMinIntervalNs = 16'666'667;

  EffectChannel() = default;
  explicit EffectChannel(uint64_t min_interval_ns) : min_interval_ns_(min_interval_ns) {}

  /// True and fills `payload` when a report is due.
  bool Update(const TriggerEffects& wanted, uint64_t now_ns, EffectPayload* payload);
  /// The next Update sends whatever it is given (after a reconnect, or after
  /// something else may have changed the triggers).
  void Invalidate() { valid_ = false; }
  /// Marks `effects` as on the pad without a rate check (release paths).
  void Sent(const TriggerEffects& effects, uint64_t now_ns);
  const TriggerEffects& sent() const { return sent_; }
  uint64_t reports() const { return reports_; }

 private:
  uint64_t min_interval_ns_ = kDefaultMinIntervalNs;
  TriggerEffects sent_;
  bool valid_ = false;
  bool ever_sent_ = false;
  uint64_t last_send_ns_ = 0;
  uint64_t reports_ = 0;
};

enum class TriggerMode { kOff, kBrake, kFull };
TriggerMode CurrentTriggerMode();

enum class LightbarMode { kPlayer, kRpm, kOff };
LightbarMode CurrentLightbarMode();

/// What the game knows about the player's car this frame. A game fills it
/// from its own physics (SetCarTelemetry); without it the model works from
/// the pedals and the rumble motors alone.
struct CarTelemetry {
  bool valid = false;
  float rpm = 0.0f;
  float redline_rpm = 0.0f;
  int gear = 0;            // 0 neutral, -1 reverse
  float speed_mps = 0.0f;
  // Longitudinal slip, normalised so 1 is the tyre's peak grip: 0 rolling,
  // past -1 locking under braking, past +1 spinning under power (in the car's
  // direction of travel, so also in reverse); the largest magnitude per axle.
  float front_slip = 0.0f;
  float rear_slip = 0.0f;
  bool abs_active = false;
  bool driven_front = false;
  bool driven_rear = true;
  // The title's own steering-wheel torque, -1 to 1, with the sign of the
  // steering input it opposes: what it sent to force feedback wheels.
  bool has_steer_torque = false;
  float steer_torque = 0.0f;
};

struct TriggerInputs {
  float brake = 0.0f;       // 0-1, the title's brake (left trigger) after remapping
  float throttle = 0.0f;    // 0-1
  float motor_low = 0.0f;   // 0-1, the title's left (low-frequency) motor
  float motor_high = 0.0f;  // 0-1, right (high-frequency) motor
  bool driving = true;      // false in menus and while host UI has input
  const CarTelemetry* telemetry = nullptr;  // fresh telemetry or null
};

struct TriggerSettings {
  TriggerMode mode = TriggerMode::kOff;
  int strength_percent = 100;  // 25-100
  bool gear_click = false;
};
TriggerSettings CurrentTriggerSettings();

/// Turns pedals, motors and telemetry into trigger effects. Values are
/// smoothed and quantised to the effect's own steps, so a steady input gives
/// byte-identical effects and the channel stays quiet.
class TriggerModel {
 public:
  /// Effects for the title's brake (left) and throttle (right) controls.
  TriggerEffects Step(const TriggerInputs& inputs, const TriggerSettings& settings, double dt_s);
  void Reset();

 private:
  float brake_buzz_ = 0.0f;
  float throttle_buzz_ = 0.0f;
  bool brake_buzzing_ = false;
  bool throttle_buzzing_ = false;
  float slip_ = 0.0f;
  float lock_ = 0.0f;
  int last_gear_ = 0;
  double click_left_s_ = 0.0;
  double abs_hold_s_ = 0.0;
  bool spin_buzzing_ = false;
  double spin_hold_s_ = 0.0;

  bool SpinBuzz(float slip, double dt_s);
};

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb&) const = default;
};

/// Lightbar colour for an engine speed: green at idle through yellow to red
/// at the redline, flashing past 97 %. `blink_on` is the flash phase.
Rgb RpmColour(float rpm, float redline_rpm, bool blink_on);

}  // namespace rex::input::dualsense
