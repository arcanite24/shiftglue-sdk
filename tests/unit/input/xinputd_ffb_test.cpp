#include <catch2/catch_test_macros.hpp>

#include <array>

#include <rex/input/xinputd_ffb.h>

namespace ffb = rex::input::xinputd_ffb;

namespace {

using Descriptor = std::array<uint8_t, ffb::kDescriptorSize>;

void Put(Descriptor& d, size_t offset, uint32_t value) {
  d[offset] = uint8_t(value >> 24);
  d[offset + 1] = uint8_t(value >> 16);
  d[offset + 2] = uint8_t(value >> 8);
  d[offset + 3] = uint8_t(value);
}

}  // namespace

// The layouts below are the ones Forza Horizon builds (sub_82BF8168): big-endian
// words, id at +0, type at +4, gain at +12 and the type block from +44.
TEST_CASE("XInputdFF descriptors decode a constant force", "[input][xinputd_ffb]") {
  Descriptor d{};
  Put(d, 0, 5);
  Put(d, 4, ffb::kConstant);
  Put(d, 12, 255);
  Put(d, 44, uint32_t(-90));

  const ffb::Effect effect = ffb::DecodeEffect(d.data());
  CHECK(effect.id == 5);
  CHECK(effect.type == ffb::kConstant);
  CHECK(effect.gain == 255);
  CHECK(effect.magnitude == -90);
  CHECK(effect.words[1] == 1);
  CHECK(ffb::DescribeEffect(effect) == "#5 constant gain 255 magnitude -90");
}

TEST_CASE("XInputdFF descriptors decode a condition effect", "[input][xinputd_ffb]") {
  Descriptor d{};
  Put(d, 0, 4);
  Put(d, 4, ffb::kSpring);
  Put(d, 12, 128);
  Put(d, 44, 0);
  Put(d, 48, 60);
  Put(d, 52, 61);
  Put(d, 56, 100);
  Put(d, 60, 101);
  Put(d, 64, 3);

  const ffb::Effect effect = ffb::DecodeEffect(d.data());
  CHECK(effect.id == 4);
  CHECK(ffb::IsConditionType(effect.type));
  CHECK(effect.positive_coefficient == 60);
  CHECK(effect.negative_coefficient == 61);
  CHECK(effect.positive_saturation == 100);
  CHECK(effect.negative_saturation == 101);
  CHECK(effect.dead_band == 3);
  CHECK(ffb::DescribeEffect(effect) ==
        "#4 spring gain 128 offset 0 coef 60/61 sat 100/101 deadband 3");
  CHECK(ffb::HexWords(effect).substr(0, 17) == "00000004 00000008");
}

TEST_CASE("XInputdFF effect types follow the HID PID ordinals", "[input][xinputd_ffb]") {
  CHECK(std::string(ffb::EffectTypeName(ffb::kInertia)) == "inertia");
  CHECK(std::string(ffb::EffectTypeName(ffb::kFriction)) == "friction");
  CHECK(std::string(ffb::EffectTypeName(ffb::kDamper)) == "damper");
  CHECK(std::string(ffb::EffectTypeName(99)) == "unknown");
  CHECK_FALSE(ffb::IsConditionType(ffb::kConstant));
  CHECK_FALSE(ffb::IsConditionType(ffb::kSine));
  CHECK(ffb::IsConditionType(ffb::kFriction));
}

TEST_CASE("XInputdFF has no driver until one registers", "[input][xinputd_ffb]") {
  CHECK(ffb::GetSink() == nullptr);
}
