/**
 * Input-to-display timing (issue pinyon-shift#1094): when the title last
 * read its controller, for the presenter to measure from.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::latency {

inline uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The steady clock (NowUs) at the title's latest XInputGetState.
inline std::atomic<uint64_t> last_input_sample_us{0};

inline void NoteInputSample() {
  last_input_sample_us.store(NowUs(), std::memory_order_relaxed);
}

}  // namespace rex::latency
