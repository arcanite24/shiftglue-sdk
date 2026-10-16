/**
 * @file        input/xinputd_ffb.cpp
 * @brief       The Xbox 360 wheel force feedback device (XInputdFF*) seen from the host.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/input/xinputd_ffb.h>

#include <atomic>

#include <fmt/format.h>

namespace rex::input::xinputd_ffb {

namespace {

std::atomic<Sink*> g_sink{nullptr};

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

const char* EffectTypeName(uint32_t type) {
  switch (type) {
    case kConstant:
      return "constant";
    case kRamp:
      return "ramp";
    case kSquare:
      return "square";
    case kSine:
      return "sine";
    case kTriangle:
      return "triangle";
    case kSawtoothUp:
      return "sawtooth-up";
    case kSawtoothDown:
      return "sawtooth-down";
    case kSpring:
      return "spring";
    case kDamper:
      return "damper";
    case kInertia:
      return "inertia";
    case kFriction:
      return "friction";
    default:
      return "unknown";
  }
}

bool IsConditionType(uint32_t type) {
  return type >= kSpring && type <= kFriction;
}

Effect DecodeEffect(const uint8_t* guest) {
  Effect effect;
  for (size_t i = 0; i < effect.words.size(); i++) {
    effect.words[i] = LoadBE32(guest + i * 4);
  }
  effect.id = effect.words[0];
  effect.type = effect.words[1];
  effect.gain = effect.words[3];
  effect.magnitude = int32_t(effect.words[11]);
  effect.positive_coefficient = int32_t(effect.words[12]);
  effect.negative_coefficient = int32_t(effect.words[13]);
  effect.positive_saturation = int32_t(effect.words[14]);
  effect.negative_saturation = int32_t(effect.words[15]);
  effect.dead_band = int32_t(effect.words[16]);
  return effect;
}

std::string DescribeEffect(const Effect& e) {
  if (e.type == kConstant) {
    return fmt::format("#{} {} gain {} magnitude {}", e.id, EffectTypeName(e.type), e.gain,
                       e.magnitude);
  }
  return fmt::format("#{} {} gain {} offset {} coef {}/{} sat {}/{} deadband {}", e.id,
                     EffectTypeName(e.type), e.gain, e.magnitude, e.positive_coefficient,
                     e.negative_coefficient, e.positive_saturation, e.negative_saturation,
                     e.dead_band);
}

std::string HexWords(const Effect& e) {
  std::string out;
  out.reserve(e.words.size() * 9);
  for (size_t i = 0; i < e.words.size(); i++) {
    if (i)
      out += ' ';
    out += fmt::format("{:08X}", e.words[i]);
  }
  return out;
}

void SetSink(Sink* sink) {
  g_sink.store(sink, std::memory_order_release);
}

Sink* GetSink() {
  return g_sink.load(std::memory_order_acquire);
}

}  // namespace rex::input::xinputd_ffb
