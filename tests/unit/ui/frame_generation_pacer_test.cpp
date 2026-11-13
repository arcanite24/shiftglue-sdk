#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/ui/frame_generation_pacer.h>

namespace {
constexpr int64_t kMs = 1'000'000;
}  // namespace

TEST_CASE("frame generation pacer estimates the clamped upper median interval", "[ui][pacing]") {
  using rex::ui::FrameGenerationPacer;
  const int64_t steady[4] = {30 * kMs, 31 * kMs, 29 * kMs, 30 * kMs};
  REQUIRE(FrameGenerationPacer::EstimateInterval(steady, 4) == 30 * kMs);
  const int64_t two[2] = {30 * kMs, 50 * kMs};
  REQUIRE(FrameGenerationPacer::EstimateInterval(two, 2) == 50 * kMs);
  const int64_t fast[1] = {1 * kMs};
  REQUIRE(FrameGenerationPacer::EstimateInterval(fast, 1) == FrameGenerationPacer::kIntervalMinNs);
  const int64_t slow[1] = {900 * kMs};
  REQUIRE(FrameGenerationPacer::EstimateInterval(slow, 1) == FrameGenerationPacer::kIntervalMaxNs);
  REQUIRE(FrameGenerationPacer::EstimateInterval(slow, 0) == 0);
}

TEST_CASE("frame generation pacer generates only on a steady pair and resets on gaps",
          "[ui][pacing]") {
  rex::ui::FrameGenerationPacer pacer([](uint64_t) {});
  int64_t now = 1'000 * kMs;
  // The first publish only starts the clock.
  auto first = pacer.OnPublish(now);
  REQUIRE_FALSE(first.generate);
  REQUIRE(first.reset_history);
  // One delta: a frame, but no steady rate yet.
  now += 50 * kMs;
  auto second = pacer.OnPublish(now);
  REQUIRE_FALSE(second.generate);
  REQUIRE_FALSE(second.reset_history);
  REQUIRE(second.interval_ns == 50 * kMs);
  // Two deltas: generate.
  now += 50 * kMs;
  auto third = pacer.OnPublish(now);
  REQUIRE(third.generate);
  REQUIRE(third.interval_ns == 50 * kMs);
  REQUIRE(third.shortest_interval_ns == 50 * kMs);
  // A frame released one vblank after the previous: the shortest interval
  // follows it, the median stays.
  now += 25 * kMs;
  auto burst = pacer.OnPublish(now);
  REQUIRE(burst.interval_ns == 50 * kMs);
  REQUIRE(burst.shortest_interval_ns == 25 * kMs);
  // A loading gap resets the history and the samples.
  now += 400 * kMs;
  auto gap = pacer.OnPublish(now);
  REQUIRE_FALSE(gap.generate);
  REQUIRE(gap.reset_history);
  now += 50 * kMs;
  REQUIRE_FALSE(pacer.OnPublish(now).generate);
  now += 50 * kMs;
  REQUIRE(pacer.OnPublish(now).generate);
  // A gap counts from the larger of the fixed threshold and three times the
  // median: 160 ms after 50 ms frames is a slow frame, not a gap; 260 ms is.
  now += 160 * kMs;
  REQUIRE_FALSE(pacer.OnPublish(now).reset_history);
  now += 260 * kMs;
  REQUIRE(pacer.OnPublish(now).reset_history);
  // At a slow base rate three medians exceeds the fixed threshold.
  for (int i = 0; i < 3; ++i) {
    now += 150 * kMs;
    pacer.OnPublish(now);
  }
  now += 460 * kMs;
  REQUIRE(pacer.OnPublish(now).reset_history);
}

TEST_CASE("frame generation pacer presents a scheduled real frame once and drops replaced ones",
          "[ui][pacing]") {
  std::atomic<int> presented_count{0};
  std::atomic<uint64_t> presented_sequence{0};
  rex::ui::FrameGenerationPacer pacer([&](uint64_t sequence) {
    presented_sequence.store(sequence);
    presented_count.fetch_add(1);
  });
  const auto now = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  REQUIRE(pacer.ScheduleReal(7, now() + 500 * kMs) == UINT64_MAX);
  // Replaced before its deadline: the old one is reported, never presented.
  REQUIRE(pacer.ScheduleReal(8, now() + 20 * kMs) == 7);
  for (int i = 0; i < 200 && presented_count.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(presented_count.load() == 1);
  REQUIRE(presented_sequence.load() == 8);
  // Cancelled before its deadline: nothing is presented.
  REQUIRE(pacer.ScheduleReal(9, now() + 30 * kMs) == UINT64_MAX);
  REQUIRE(pacer.CancelPending() == 9);
  REQUIRE(pacer.CancelPending() == UINT64_MAX);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  REQUIRE(presented_count.load() == 1);
}
