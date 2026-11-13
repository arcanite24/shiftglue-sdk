/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 * Frame generation: an opt-in pass that presents one generated frame between
 * each pair of frames the title rendered. The presenter owns the pacing and
 * the history images; a backend turns two frames (and, when available, depth,
 * motion vectors and the HUD layer) into the generated one.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace rex {
namespace ui {

enum class FrameGenerationBackend : uint32_t {
  kOff = 0,
  // Apple MetalFX frame interpolation (macOS 26, Apple silicon). The
  // presenter's Vulkan images are exported to Metal through
  // VK_EXT_metal_objects.
  kMetalFx,
  // AMD FidelityFX FSR 3.1 frame generation, dispatched into the presenter's
  // own images without the FidelityFX swapchain. A stub until it is wired.
  kFsr3,
  // Development only, never offered in the menu: re-presents the previous
  // frame as the "generated" one so pacing and counters can be measured
  // without any backend.
  kRepeatDebug,
};

// Why a backend can or cannot run here. `reason` is a static string.
struct FrameGenerationAvailability {
  bool available = false;
  const char* reason = "";
};

// Published by the presenter for the settings menu and diagnostics, in the
// manner of HdrOutputState: readable from any thread.
struct FrameGenerationState {
  // Whether any backend is built into this binary for this platform. The
  // menu shows the FRAME GENERATION row only when one is.
  bool offered = false;
  // The backend the present_frame_generation setting asks for.
  FrameGenerationBackend requested = FrameGenerationBackend::kOff;
  // Whether the requested backend can run (false for kOff).
  bool available = false;
  // Whether generated frames are being presented right now.
  bool active = false;
  // Why the requested backend is unavailable; "" otherwise.
  const char* reason = "";
};
FrameGenerationState GetFrameGenerationState();
void PublishFrameGenerationState(const FrameGenerationState& state);
// Only the `active` flag, from the paint thread.
void PublishFrameGenerationActive(bool active);

// The setting's value (case-insensitive) to a backend; unknown text is kOff.
FrameGenerationBackend ParseFrameGenerationBackend(std::string_view name);
// The setting's value for a backend.
const char* FrameGenerationBackendName(FrameGenerationBackend backend);

// The platform-wide answer for backends the binary does not build: used by
// presenters without their own generators, and as the fallback reason.
FrameGenerationAvailability ProbeFrameGenerationBackendGeneric(FrameGenerationBackend backend);

}  // namespace ui
}  // namespace rex
