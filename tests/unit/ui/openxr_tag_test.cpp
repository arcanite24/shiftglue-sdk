#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include <rex/ui/xr_tag.h>

using namespace rex::ui::xr;

TEST_CASE("OpenXR field of view tags survive float storage", "[ui][openxr]") {
  for (const double fov : {0.3, 0.8466, 1.0821, 1.5, 2.2}) {
    for (uint32_t tag = 0; tag < kTagCount; ++tag) {
      const double tagged = TagFov(fov, tag);
      // At most one step away, and never a visible change.
      CHECK(std::abs(tagged - fov) <= 0.002);
      // The title keeps the field of view as a float.
      CHECK(FovTag(double(float(tagged))) == tag);
    }
  }
}

TEST_CASE("OpenXR untagged fields of view rarely decode", "[ui][openxr]") {
  int decoded = 0;
  for (int i = 0; i < 1000; ++i) {
    if (FovTag(0.5 + i * 0.000737) != UINT32_MAX) ++decoded;
  }
  // The tags cover a tenth of each step; a stray decode must also match a
  // live request to be used.
  CHECK(decoded < 150);
  CHECK(FovTag(0.0) == UINT32_MAX);
  CHECK(FovTag(-1.0) == UINT32_MAX);
}
