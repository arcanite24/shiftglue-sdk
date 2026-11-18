#pragma once
/**
 * Upscaler providers: what the presenter can scale the guest output with,
 * independent of the graphics API.
 *
 * Spatial providers (bilinear, CAS, FSR 1) are fragment passes of the guest
 * output paint flow. Temporal providers (FSR 2 and 3 through the FidelityFX
 * API, later DLSS and XeSS) take a set of per-frame inputs and implement the
 * API-specific interface (rex/ui/vulkan/upscaler.h).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace rex {
namespace ui {

enum class UpscalerKind : uint8_t {
  kSpatial,
  kTemporal,
};

// The quality modes FSR, DLSS and XeSS share, by render size divisor.
enum class UpscalerMode : uint8_t {
  // The guest output size as it is (the game's internal resolution).
  kAuto,
  kNativeAa,
  kQuality,
  kBalanced,
  kPerformance,
  kUltraPerformance,
  kCount,
};

// Output size divided by render size: 1, 1.5, 1.7, 2 and 3 (1 for kAuto).
float GetUpscalerModeRatio(UpscalerMode mode);
const char* GetUpscalerModeName(UpscalerMode mode);

struct UpscalerInfo {
  // Stable name for cvars and settings ("bilinear", "cas", "fsr", "fsr2",
  // "fsr3"); present_effect takes it.
  std::string id;
  // Shown to players.
  std::string name;
  // The provider's own version, from its runtime when it reports one.
  std::string version;
  UpscalerKind kind = UpscalerKind::kSpatial;
  bool available = false;
  // Why it is not available (no runtime, unsupported device).
  std::string unavailable_reason;
  // Bits of 1 << UpscalerMode.
  uint32_t mode_mask = 1u << uint32_t(UpscalerMode::kAuto);
  bool uses_depth = false;
  bool uses_motion_vectors = false;
  bool uses_jitter = false;
  bool uses_reactive_mask = false;
};

// The spatial providers every presenter has.
std::vector<UpscalerInfo> GetSpatialUpscalerInfos();

// Whether the presenter's last paint fed the guest output to a temporal
// provider (UP-1.6): games turn off effects that fight its history, such as
// motion blur and depth of field. Any thread.
bool IsTemporalUpscalerActive();
// render_scale: the provider's render width over its output width (1 for a
// 1:1 pass such as DLAA, and when none is active).
void SetTemporalUpscalerActive(bool active, float render_scale = 1.0f);
float GetTemporalUpscalerRenderScale();

// Per-frame description of a temporal upscale, for every graphics API.
// The game camera of a frame, for a temporal upscaler's depth and motion.
struct UpscalerCamera {
  float near_plane = 0.1f;
  float far_plane = 1000.0f;
  // Vertical, radians.
  float fov_y = 1.0472f;
  // No continuation of the previous frame: the history should be dropped.
  bool cut = false;
  // The frame's sub-pixel jitter in render pixels, as UpscalerFrame.
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
};

struct UpscalerFrame {
  uint32_t render_width = 0;
  uint32_t render_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  // Sub-pixel camera offset of this frame, in render pixels.
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  // Motion vectors are multiplied by this to get render pixels.
  float motion_vector_scale_x = 0.0f;
  float motion_vector_scale_y = 0.0f;
  // Time since the previous upscaled frame.
  float frame_delta_ms = 16.667f;
  // Drop the history: a camera cut, a resize, the first frame.
  bool reset = true;
  // The colour is linear HDR rather than display-referred SDR.
  bool hdr = false;
  // Depth 1 is near rather than far.
  bool depth_inverted = false;
  float camera_near = 0.1f;
  float camera_far = 1000.0f;
  float camera_fov_y = 1.0472f;
  // 0 (none) to 1 (most), for providers with their own sharpening.
  float sharpness = 0.0f;
};

}  // namespace ui
}  // namespace rex
