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
  // A backend that records Vulkan work puts it here. A backend that runs on
  // another API's queue (MetalFX) ignores it and instead orders its own work
  // through `queue_wait`/`queue_signal` below.
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  // The two frames, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, readable by
  // fragment and compute shaders and as transfer sources. An external-queue
  // backend (MetalFX) also needs them created exportable (see the factory).
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
  // External-queue ordering (MetalFX). The presenter submits the history copy
  // (and this generation's own paint) on the graphics/compute queue; a backend
  // that runs on another API's queue waits `queue_wait` on that queue's work
  // reaching `queue_wait_value` before it reads `previous`/`current`, and the
  // presenter's paint waits for the backend's completion before it reads
  // `output`. The first MetalFX version keeps this ordering on the CPU
  // (MoltenVK synchronous submits plus a wait inside Generate), so these stay
  // null; the fields reserve the asynchronous handoff for a later measurement.
  VkSemaphore queue_wait = VK_NULL_HANDLE;
  uint64_t queue_wait_value = 0;
  VkSemaphore queue_signal = VK_NULL_HANDLE;
  uint64_t queue_signal_value = 0;
};

class VulkanFrameGenerator {
 public:
  virtual ~VulkanFrameGenerator() = default;
  virtual FrameGenerationBackend backend() const = 0;
  // Whether Generate runs on another API's queue (MetalFX): the presenter
  // then creates the history and output images exportable, submits the
  // history copy on its own before calling Generate, and lets Generate order
  // the generation itself rather than recording into the paint command
  // buffer.
  virtual bool UsesExternalQueue() const { return false; }
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
