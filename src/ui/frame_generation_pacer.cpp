/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 */

#include <rex/ui/frame_generation_pacer.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include <rex/thread.h>

namespace rex {
namespace ui {

namespace {
using PacingClock = std::chrono::steady_clock;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(PacingClock::now().time_since_epoch())
      .count();
}
}  // namespace

FrameGenerationPacer::FrameGenerationPacer(PresentCallback present) : present_(std::move(present)) {
  thread_ = std::thread([this] { ThreadMain(); });
}

FrameGenerationPacer::~FrameGenerationPacer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    pending_ = false;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

int64_t FrameGenerationPacer::EstimateInterval(const int64_t* samples, size_t count) {
  if (!count) {
    return 0;
  }
  std::array<int64_t, kIntervalSamples> sorted{};
  count = std::min(count, sorted.size());
  std::copy(samples, samples + count, sorted.begin());
  std::sort(sorted.begin(), sorted.begin() + count);
  // The upper median of an even count, so two samples lean to the longer.
  const int64_t median = sorted[count / 2];
  return std::clamp(median, kIntervalMinNs, kIntervalMaxNs);
}

FrameGenerationPacer::Decision FrameGenerationPacer::OnPublish(int64_t now_ns) {
  Decision decision;
  if (!last_publish_ns_) {
    last_publish_ns_ = now_ns;
    decision.reset_history = true;
    return decision;
  }
  const int64_t delta_ns = now_ns - last_publish_ns_;
  last_publish_ns_ = now_ns;
  const int64_t median_ns = EstimateInterval(deltas_.data(), delta_count_);
  if (delta_ns <= 0 ||
      delta_ns > std::max(kGapResetNs, delta_count_ ? kGapMedianMultiple * median_ns : int64_t(0))) {
    // A gap: the frames either side of it are not a pair to interpolate.
    delta_count_ = 0;
    delta_cursor_ = 0;
    decision.reset_history = true;
    return decision;
  }
  deltas_[delta_cursor_] = delta_ns;
  delta_cursor_ = (delta_cursor_ + 1) % deltas_.size();
  delta_count_ = std::min(delta_count_ + 1, deltas_.size());
  decision.interval_ns = EstimateInterval(deltas_.data(), delta_count_);
  decision.shortest_interval_ns =
      std::clamp(*std::min_element(deltas_.begin(), deltas_.begin() + delta_count_),
                 kIntervalMinNs, kIntervalMaxNs);
  // Two deltas: three frames, enough to tell a steady rate from a start.
  decision.generate = delta_count_ >= 2;
  return decision;
}

void FrameGenerationPacer::ResetIntervals() {
  last_publish_ns_ = 0;
  delta_count_ = 0;
  delta_cursor_ = 0;
}

uint64_t FrameGenerationPacer::ScheduleReal(uint64_t sequence, int64_t deadline_ns) {
  uint64_t replaced = UINT64_MAX;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) {
      replaced = pending_sequence_;
    }
    pending_ = true;
    pending_sequence_ = sequence;
    pending_deadline_ns_ = deadline_ns;
  }
  condition_.notify_all();
  return replaced;
}

uint64_t FrameGenerationPacer::CancelPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_) {
    return UINT64_MAX;
  }
  pending_ = false;
  return pending_sequence_;
}

void FrameGenerationPacer::ThreadMain() {
  rex::thread::set_current_thread_name("Frame Pacing");
  // Wake this long before the deadline and spin the rest, as the presenter's
  // own pacing does with high-resolution waits.
  constexpr int64_t kWakeEarlyNs = 1'000'000;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!shutdown_) {
    if (!pending_) {
      condition_.wait(lock);
      continue;
    }
    const uint64_t sequence = pending_sequence_;
    const int64_t deadline_ns = pending_deadline_ns_;
    const int64_t now_ns = NowNs();
    if (now_ns + kWakeEarlyNs < deadline_ns) {
      condition_.wait_until(lock, PacingClock::time_point(std::chrono::nanoseconds(
                                      deadline_ns - kWakeEarlyNs)));
      // Re-read: the pending present may have been replaced or cancelled.
      continue;
    }
    lock.unlock();
    rex::thread::SleepUntil(PacingClock::time_point(std::chrono::nanoseconds(deadline_ns)),
                            std::chrono::microseconds(50));
    lock.lock();
    if (shutdown_ || !pending_ || pending_sequence_ != sequence) {
      continue;
    }
    pending_ = false;
    lock.unlock();
    present_(sequence);
    lock.lock();
  }
}

}  // namespace ui
}  // namespace rex
