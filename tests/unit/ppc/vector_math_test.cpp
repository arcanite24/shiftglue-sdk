/**
 * Tests for Xenon-compatible vector dot-product edge behavior.
 */

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/intrinsics.h>

namespace {

uint32_t lane_bits(simde__m128 value, size_t lane = 0) {
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, value);
  return std::bit_cast<uint32_t>(lanes[lane]);
}

void check_broadcast(simde__m128 value, uint32_t expected) {
  for (size_t lane = 0; lane < 4; ++lane) {
    CHECK(lane_bits(value, lane) == expected);
  }
}

}  // namespace

TEST_CASE("vmsum finite overflow becomes canonical QNaN", "[ppc][vector][vmsum]") {
  const simde__m128 max = simde_mm_set1_ps(-std::numeric_limits<float>::max());

  check_broadcast(rex::ppc::simde_mm_vmsum3fp128(max, max), 0x7FC00000u);
  check_broadcast(rex::ppc::simde_mm_vmsum4fp128(max, max), 0x7FC00000u);
}

TEST_CASE("vmsum preserves infinite inputs and propagates NaN", "[ppc][vector][vmsum]") {
  const float infinity = std::numeric_limits<float>::infinity();
  const float qnan = std::bit_cast<float>(uint32_t{0x7FC00000});
  const simde__m128 multiplier = simde_mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);

  check_broadcast(
      rex::ppc::simde_mm_vmsum3fp128(simde_mm_set_ps(infinity, 0.0f, 0.0f, 0.0f), multiplier),
      0x7F800000u);
  check_broadcast(
      rex::ppc::simde_mm_vmsum3fp128(simde_mm_set_ps(-infinity, 0.0f, 0.0f, 0.0f), multiplier),
      0xFF800000u);

  const simde__m128 nan_result =
      rex::ppc::simde_mm_vmsum3fp128(simde_mm_set_ps(qnan, 0.0f, 0.0f, 0.0f), multiplier);
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, nan_result);
  for (float lane : lanes) {
    CHECK(std::isnan(lane));
  }
}

TEST_CASE("vmsum flushes denormal outputs with their sign", "[ppc][vector][vmsum]") {
  const double denormal = static_cast<double>(std::numeric_limits<float>::denorm_min());

  CHECK(std::bit_cast<uint32_t>(rex::ppc::simde_mm_vmsum_narrow_result(denormal)) == 0x00000000u);
  CHECK(std::bit_cast<uint32_t>(rex::ppc::simde_mm_vmsum_narrow_result(-denormal)) == 0x80000000u);
}

TEST_CASE("vmsum overflow detection occurs after narrowing", "[ppc][vector][vmsum]") {
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  const double rounds_to_max = max_float + std::ldexp(1.0, 102);
  const double overflows_float = max_float + std::ldexp(1.0, 104);

  CHECK(std::bit_cast<uint32_t>(rex::ppc::simde_mm_vmsum_narrow_result(rounds_to_max)) ==
        0x7F7FFFFFu);
  CHECK(std::bit_cast<uint32_t>(rex::ppc::simde_mm_vmsum_narrow_result(overflows_float)) ==
        0x7FC00000u);
}

TEST_CASE("vmsum uses the guest reduction order", "[ppc][vector][vmsum]") {
  const simde__m128 ones = simde_mm_set1_ps(1.0f);
  const simde__m128 terms = simde_mm_set_ps(1.0e20f, 3.0f, -1.0e20f, 4.0f);

  // vmsum3 excludes w and reduces (x + z) + y: (1e20 - 1e20) + 3.
  check_broadcast(rex::ppc::simde_mm_vmsum3fp128(ones, terms), 0x40400000u);
  // vmsum4 reduces (x + z) + (y + w): 0 + (3 + 4).
  check_broadcast(rex::ppc::simde_mm_vmsum4fp128(ones, terms), 0x40E00000u);
}
