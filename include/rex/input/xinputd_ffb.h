#pragma once
/**
 * @file        rex/input/xinputd_ffb.h
 * @brief       The Xbox 360 wheel force feedback device (XInputdFF*) seen from the host.
 *
 * A title that finds a wheel with the force-feedback flag talks to it through the kernel's
 * XInputdFF* calls instead of XamInputSetState: it downloads condition and constant-force
 * effects (XInputdFFSetEffect), starts them (XInputdFFEffectOperation), updates them every frame
 * (XInputdFFUpdateEffect) and sends rumble separately (XInputdFFSetRumble). Forza Horizon
 * creates five: inertia, friction, damper, spring (its strength follows speed) and a constant
 * force. The kernel exports decode the guest's descriptors and hand them to the wheel driver
 * registered here, which plays them on the host wheel. With no driver registered the calls
 * are still answered, so the title's set-up completes either way.
 *
 * Descriptors are 116 bytes of big-endian words. Effect types are the USB HID PID ordinals:
 * 1 constant, 2 ramp, 3 square, 4 sine, 5 triangle, 6 sawtooth up, 7 sawtooth down,
 * 8 spring, 9 damper, 10 inertia, 11 friction.
 *
 * @license     BSD 3-Clause License
 */

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace rex::input::xinputd_ffb {

constexpr uint32_t kDescriptorSize = 116;

enum EffectType : uint32_t {
  kConstant = 1,
  kRamp = 2,
  kSquare = 3,
  kSine = 4,
  kTriangle = 5,
  kSawtoothUp = 6,
  kSawtoothDown = 7,
  kSpring = 8,
  kDamper = 9,
  kInertia = 10,
  kFriction = 11,
};

const char* EffectTypeName(uint32_t type);
bool IsConditionType(uint32_t type);

struct Effect {
  uint32_t id = 0;      // +0, 1-based effect block index
  uint32_t type = 0;    // +4, EffectType
  uint32_t gain = 255;  // +12, 0..255
  // Type-specific block from +44: a constant force's magnitude, or a condition's
  // centre offset, then its coefficients, saturations and dead band.
  int32_t magnitude = 0;                              // +44 (constant) / centre offset (conditions)
  int32_t positive_coefficient = 0;                   // +48
  int32_t negative_coefficient = 0;                   // +52
  int32_t positive_saturation = 0;                    // +56
  int32_t negative_saturation = 0;                    // +60
  int32_t dead_band = 0;                              // +64
  std::array<uint32_t, kDescriptorSize / 4> words{};  // all of it, for the trace
};

// `guest` points at a 116-byte big-endian descriptor in guest memory.
Effect DecodeEffect(const uint8_t* guest);
// "id type gain mag pc nc ps ns db" and the raw words, for logs and the trace.
std::string DescribeEffect(const Effect& effect);
std::string HexWords(const Effect& effect);

// PID device control commands.
enum DeviceCommand : uint32_t {
  kEnableActuators = 1,
  kDisableActuators = 2,
  kStopAll = 3,
  kReset = 4,
  kPause = 5,
  kContinue = 6,
};

// The wheel driver's side. Calls arrive on guest threads.
class Sink {
 public:
  virtual ~Sink() = default;
  // False when no host wheel can take force feedback; the title is still answered.
  virtual bool HasForceFeedback() = 0;
  virtual void SetEffect(const Effect& effect) = 0;
  virtual void UpdateEffect(const Effect& effect, uint32_t flags) = 0;
  // op: 1 or 2 start (2 is what Forza Horizon sends), 3 stop.
  virtual void EffectOperation(uint32_t id, uint32_t op) = 0;
  virtual void DeviceControl(uint32_t command) = 0;
  virtual void SetDeviceGain(uint32_t gain) = 0;
  virtual void SetRumble(uint16_t left, uint16_t right) = 0;
};

// SetSink waits for calls in flight through WithSink, so after SetSink(nullptr)
// returns the old sink is no longer used and can be destroyed.
void SetSink(Sink* sink);
Sink* GetSink();
// Runs fn with the registered sink (nullptr when none) while holding it registered.
// fn must not call SetSink.
void WithSink(const std::function<void(Sink*)>& fn);

}  // namespace rex::input::xinputd_ffb
