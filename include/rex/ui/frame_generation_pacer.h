/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 * Frame generation pacing: measures the title's frame interval and presents
 * the real frame a fraction of it after the generated one, from its own
 * thread, so the thread producing the frames never waits.
 */

#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace rex {
namespace ui {

class FrameGenerationPacer {
 public:
  // Called on the pacing thread when a scheduled real frame is due, with the
  // refresh sequence it was scheduled for.
  using PresentCallback = std::function<void(uint64_t sequence)>;

  struct Decision {
    // Whether a generated frame may be presented for this publish: the
    // interval is known and steady.
    bool generate = false;
    // Whether the backend must forget its history (a gap, or the first frame).
    bool reset_history = false;
    // The estimated base interval, in nanoseconds (0 when unknown): the
    // median of the recent publish deltas, for the backend's delta time.
    int64_t interval_ns = 0;
    // The shortest recent publish delta, clamped: the real frame is delayed a
    // fraction of this, so it lands before the earliest plausible next frame
    // (the title releases a late frame one vblank after the previous one).
    int64_t shortest_interval_ns = 0;
  };

  // Publish gaps above this, or above kGapMedianMultiple times the median,
  // reset the history (loading, pause, video, a hidden window).
  static constexpr int64_t kGapResetNs = 250'000'000;
  static constexpr int64_t kGapMedianMultiple = 3;
  // The interval estimate's clamp: 5-125 Hz base rates.
  static constexpr int64_t kIntervalMinNs = 8'000'000;
  static constexpr int64_t kIntervalMaxNs = 200'000'000;
  static constexpr size_t kIntervalSamples = 4;

  explicit FrameGenerationPacer(PresentCallback present);
  ~FrameGenerationPacer();

  FrameGenerationPacer(const FrameGenerationPacer&) = delete;
  FrameGenerationPacer& operator=(const FrameGenerationPacer&) = delete;

  // A real frame was published at `now_ns`.
  Decision OnPublish(int64_t now_ns);
  // Forget the interval samples; the next publish starts over.
  void ResetIntervals();
  // Present the real frame of `sequence` at `deadline_ns`, replacing a pending
  // one; returns the replaced sequence, or UINT64_MAX.
  uint64_t ScheduleReal(uint64_t sequence, int64_t deadline_ns);
  // Returns the cancelled sequence, or UINT64_MAX.
  uint64_t CancelPending();

  // The pure estimate, for tests: the median of the samples, clamped.
  static int64_t EstimateInterval(const int64_t* samples, size_t count);

 private:
  void ThreadMain();

  PresentCallback present_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
  bool shutdown_ = false;
  bool pending_ = false;
  uint64_t pending_sequence_ = 0;
  int64_t pending_deadline_ns_ = 0;

  // Publish-thread state.
  int64_t last_publish_ns_ = 0;
  std::array<int64_t, kIntervalSamples> deltas_{};
  size_t delta_count_ = 0;
  size_t delta_cursor_ = 0;
};

}  // namespace ui
}  // namespace rex
