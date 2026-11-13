/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 */

#include <rex/ui/frame_generator.h>

#include <atomic>
#include <cctype>
#include <string>

#include <rex/platform.h>

namespace rex {
namespace ui {

namespace {
std::atomic<bool> frame_generation_offered = false;
std::atomic<uint32_t> frame_generation_requested = uint32_t(FrameGenerationBackend::kOff);
std::atomic<bool> frame_generation_available = false;
std::atomic<bool> frame_generation_active = false;
std::atomic<const char*> frame_generation_reason = "";
}  // namespace

FrameGenerationState GetFrameGenerationState() {
  FrameGenerationState state;
  state.offered = frame_generation_offered.load(std::memory_order_relaxed);
  state.requested =
      FrameGenerationBackend(frame_generation_requested.load(std::memory_order_relaxed));
  state.available = frame_generation_available.load(std::memory_order_relaxed);
  state.active = frame_generation_active.load(std::memory_order_relaxed);
  state.reason = frame_generation_reason.load(std::memory_order_relaxed);
  return state;
}

void PublishFrameGenerationState(const FrameGenerationState& state) {
  frame_generation_offered.store(state.offered, std::memory_order_relaxed);
  frame_generation_requested.store(uint32_t(state.requested), std::memory_order_relaxed);
  frame_generation_available.store(state.available, std::memory_order_relaxed);
  frame_generation_active.store(state.active, std::memory_order_relaxed);
  frame_generation_reason.store(state.reason ? state.reason : "", std::memory_order_relaxed);
}

void PublishFrameGenerationActive(bool active) {
  frame_generation_active.store(active, std::memory_order_relaxed);
}

FrameGenerationBackend ParseFrameGenerationBackend(std::string_view name) {
  std::string lowered(name);
  for (char& c : lowered) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lowered == "metalfx") {
    return FrameGenerationBackend::kMetalFx;
  }
  if (lowered == "fsr3") {
    return FrameGenerationBackend::kFsr3;
  }
  if (lowered == "repeat") {
    return FrameGenerationBackend::kRepeatDebug;
  }
  return FrameGenerationBackend::kOff;
}

const char* FrameGenerationBackendName(FrameGenerationBackend backend) {
  switch (backend) {
    case FrameGenerationBackend::kMetalFx:
      return "metalfx";
    case FrameGenerationBackend::kFsr3:
      return "fsr3";
    case FrameGenerationBackend::kRepeatDebug:
      return "repeat";
    default:
      return "off";
  }
}

FrameGenerationAvailability ProbeFrameGenerationBackendGeneric(FrameGenerationBackend backend) {
  switch (backend) {
    case FrameGenerationBackend::kOff:
      return {false, ""};
    case FrameGenerationBackend::kMetalFx:
#if REX_PLATFORM_MAC
      return {false, "MetalFX frame generation is not built into this presenter"};
#else
      return {false, "MetalFX frame generation needs macOS 26 on Apple silicon"};
#endif
    case FrameGenerationBackend::kFsr3:
      // The stub: the dispatch into the presenter's images without the
      // FidelityFX swapchain is designed but not wired (docs/FRAME_GENERATION.md
      // in the Pinyon Shift repository).
      return {false, "FSR 3.1 frame generation is not wired yet"};
    case FrameGenerationBackend::kRepeatDebug:
      return {false, "the repeat debug backend needs the Vulkan presenter"};
  }
  return {false, "unknown frame generation backend"};
}

}  // namespace ui
}  // namespace rex
