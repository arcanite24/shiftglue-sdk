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

// NVIDIA Reflex (VK_NV_low_latency2): the presenter installs a hook that
// sleeps at the start of a guest frame and marks its stages. A guest frame
// starts at the first controller read after a VdSwap and ends at the next
// VdSwap.
enum class FrameEvent : uint32_t {
  kInputSample,  // XInputGetState, before the state is read
  kSwap,         // VdSwap
};
using FrameHook = void (*)(FrameEvent event);
inline std::atomic<FrameHook> frame_hook{nullptr};

inline void NoteFrameEvent(FrameEvent event) {
  if (FrameHook hook = frame_hook.load(std::memory_order_acquire)) {
    hook(event);
  }
}

// Before the title reads its controller: Reflex may sleep here first.
inline void NoteInputSample() {
  NoteFrameEvent(FrameEvent::kInputSample);
  last_input_sample_us.store(NowUs(), std::memory_order_relaxed);
}

}  // namespace rex::latency
