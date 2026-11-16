#pragma once
/**
 * Vulkan frame generators (docs/UPSCALING_BACKLOG.md UP-4, UP-5 in Pinyon
 * Shift): from the finished frame the presenter is about to show, and the
 * previous one, a provider writes the frame halfway between them. The
 * presenter shows the generated frame first and the real one half a frame
 * later; nothing on the guest side (its swaps, the step gate, the flip
 * counters) sees the generated frames.
 */

#include <cstdint>
#include <memory>
#include <string>

#include <rex/ui/upscaler.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upscaler.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

struct FrameGeneratorInfo {
  // Stable name for settings: "fsr3", "dlss", "lsfg".
  std::string id;
  std::string name;
  std::string version;
  bool available = false;
  std::string unavailable_reason;
  // Inputs the provider reads besides the two finished frames.
  bool uses_depth = false;
  bool uses_motion_vectors = false;
};

// present_color is the finished frame at the display size, in
// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; depth and motion_vectors are the
// render-size camera inputs (as for the temporal upscalers), also shader-read,
// or null; output is VK_IMAGE_LAYOUT_GENERAL, writable by compute shaders.
struct VulkanFrameGeneratorInputs {
  VulkanUpscalerImage present_color;
  VulkanUpscalerImage depth;
  VulkanUpscalerImage motion_vectors;
  VulkanUpscalerImage output;
  // Render size, motion vector scale, camera, jitter, the time since the
  // previous real frame, and reset (a cut, a resize, a skipped frame).
  UpscalerFrame frame;
  // Grows by one for each real frame given to Dispatch.
  uint64_t frame_id = 0;
  // Where the game's picture is in present_color (letterboxing leaves bars
  // that the camera inputs do not cover).
  int32_t generation_x = 0;
  int32_t generation_y = 0;
  uint32_t generation_width = 0;
  uint32_t generation_height = 0;
};

class VulkanFrameGenerator {
 public:
  virtual ~VulkanFrameGenerator() = default;

  virtual const FrameGeneratorInfo& info() const = 0;
  // Resources for this display size and frames rendered up to the maximum
  // render size; kept if they already fit. The previous resources must be
  // out of use by the GPU. False if the provider cannot run.
  virtual bool Prepare(uint32_t display_width, uint32_t display_height,
                       uint32_t max_render_width, uint32_t max_render_height) = 0;
  // Records the generation of the frame between the previous Dispatch's
  // present_color and this one's into output; false if nothing was recorded.
  virtual bool Dispatch(VkCommandBuffer command_buffer,
                        const VulkanFrameGeneratorInputs& inputs) = 0;
  // Frees what Prepare made (the GPU must be done with it).
  virtual void Release() = 0;
};

// FSR 3.1 frame generation through the FidelityFX API (the same runtime as
// the FSR upscalers), or null when the build has no FidelityFX API.
std::unique_ptr<VulkanFrameGenerator> CreateVulkanFfxFrameGenerator(const VulkanDevice* device);
// UP-4.3: null when present_dlss_frame_generation is off.
std::unique_ptr<VulkanFrameGenerator> CreateVulkanDlssFrameGenerator(const VulkanDevice* device);
// UP-5.2: LSFG 3.1 from the player's own Lossless.dll, its shaders translated
// at run time. Null unless present_lsfg_dll or present_lsfg_experimental is set.
std::unique_ptr<VulkanFrameGenerator> CreateVulkanLsfgFrameGenerator(const VulkanDevice* device);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
