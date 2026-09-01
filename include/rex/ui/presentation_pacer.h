#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::ui {

struct PresentationPacingDecision {
  int64_t deadline_ns = 0;
  int64_t wait_ns = 0;
  uint64_t missed_deadlines = 0;
};

// Monotonic fixed-rate deadline scheduler. The caller owns the clock and wait
// strategy so this remains deterministic and independently testable.
class PresentationDeadlineScheduler {
 public:
  PresentationPacingDecision Plan(int64_t now_ns, uint32_t fps_limit) {
    if (!fps_limit) {
      Reset();
      return {};
    }

    const int64_t interval_ns =
        std::max<int64_t>(1, 1000000000LL / int64_t(fps_limit));
    if (!initialized_ || interval_ns != interval_ns_) {
      initialized_ = true;
      interval_ns_ = interval_ns;
      next_deadline_ns_ = now_ns;
    }

    PresentationPacingDecision decision;
    decision.deadline_ns = next_deadline_ns_;
    if (now_ns < next_deadline_ns_) {
      decision.wait_ns = next_deadline_ns_ - now_ns;
      next_deadline_ns_ += interval_ns_;
      return decision;
    }

    const int64_t lateness_ns = now_ns - next_deadline_ns_;
    decision.missed_deadlines = uint64_t(lateness_ns / interval_ns_);
    next_deadline_ns_ +=
        int64_t(decision.missed_deadlines + 1) * interval_ns_;
    return decision;
  }

  void Reset() {
    initialized_ = false;
    interval_ns_ = 0;
    next_deadline_ns_ = 0;
  }

 private:
  bool initialized_ = false;
  int64_t interval_ns_ = 0;
  int64_t next_deadline_ns_ = 0;
};

}  // namespace rex::ui
