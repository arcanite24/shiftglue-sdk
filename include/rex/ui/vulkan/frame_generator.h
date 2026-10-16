/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 * Frame generation backends for the Vulkan presenter. The presenter owns the
 * history and output images and the pacing; a backend fills the output from
 * the two frames (and the depth, motion and HUD inputs of later stages).
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/ui/frame_generator.h>
#include <rex/ui/vulkan/api.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

struct VulkanFrameGenerationInputs {
  // The presenter's paint command buffer, recording, before any render pass.
  // A backend that records Vulkan work puts it here; one that runs on another
  // API (MetalFX) synchronizes through the presenter's timeline semaphore.
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  // The two frames, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, readable by
  // fragment and compute shaders and as transfer sources.
  VkImage previous = VK_NULL_HANDLE;
  VkImageView previous_view = VK_NULL_HANDLE;
  VkImage current = VK_NULL_HANDLE;
  VkImageView current_view = VK_NULL_HANDLE;
  // The output. `output_ever_written` false means its contents and layout are
  // undefined; otherwise it is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
  // after a fragment-shader read. The backend leaves it in
  // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for a fragment-shader read.
  VkImage output = VK_NULL_HANDLE;
  VkImageView output_view = VK_NULL_HANDLE;
  bool output_ever_written = false;
  VkExtent2D extent = {};
  // The measured interval between the two frames, in seconds.
  float delta_s = 0.0f;
  // The frames are not a continuation of the previous call's.
  bool reset_history = false;
};

class VulkanFrameGenerator {
 public:
  virtual ~VulkanFrameGenerator() = default;
  virtual FrameGenerationBackend backend() const = 0;
  // Records or submits the generation; false leaves the output undefined and
  // the presenter shows the real frame instead.
  virtual bool Generate(const VulkanFrameGenerationInputs& inputs) = 0;
  // GPU time of the last generation the backend could measure, or 0.
  virtual uint64_t TakeLastGpuTimeNs() { return 0; }
};

// Whether a backend can run on this device; the reason when it cannot.
FrameGenerationAvailability ProbeVulkanFrameGenerationBackend(FrameGenerationBackend backend,
                                                              const VulkanDevice* vulkan_device);
// Null when the backend is unavailable.
std::unique_ptr<VulkanFrameGenerator> CreateVulkanFrameGenerator(FrameGenerationBackend backend,
                                                                 const VulkanDevice* vulkan_device);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
