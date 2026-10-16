/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 */

#include <rex/ui/vulkan/frame_generator.h>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {

namespace {

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = util::InitializeSubresourceRange();
  return barrier;
}

// Development only: the "generated" frame is the previous frame again, so the
// pacing, the counters and the image plumbing can be measured on their own.
class RepeatFrameGenerator final : public VulkanFrameGenerator {
 public:
  explicit RepeatFrameGenerator(const VulkanDevice* vulkan_device)
      : vulkan_device_(vulkan_device) {}

  FrameGenerationBackend backend() const override { return FrameGenerationBackend::kRepeatDebug; }

  bool Generate(const VulkanFrameGenerationInputs& inputs) override {
    if (inputs.command_buffer == VK_NULL_HANDLE || inputs.previous == VK_NULL_HANDLE ||
        inputs.output == VK_NULL_HANDLE) {
      return false;
    }
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkPipelineStageFlags read_stages =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkImageMemoryBarrier to_transfer[2];
    to_transfer[0] = ImageBarrier(inputs.previous, VK_ACCESS_SHADER_READ_BIT,
                                  VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_transfer[1] = ImageBarrier(
        inputs.output, inputs.output_ever_written ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        inputs.output_ever_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    dfn.vkCmdPipelineBarrier(inputs.command_buffer, read_stages | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                             to_transfer);
    VkImageCopy region;
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = {0, 0, 0};
    region.extent = {inputs.extent.width, inputs.extent.height, 1};
    dfn.vkCmdCopyImage(inputs.command_buffer, inputs.previous,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, inputs.output,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_read[2];
    to_read[0] = ImageBarrier(inputs.previous, VK_ACCESS_TRANSFER_READ_BIT,
                              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_read[1] = ImageBarrier(inputs.output, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dfn.vkCmdPipelineBarrier(inputs.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, read_stages, 0,
                             0, nullptr, 0, nullptr, 2, to_read);
    return true;
  }

 private:
  const VulkanDevice* vulkan_device_;
};

}  // namespace

FrameGenerationAvailability ProbeVulkanFrameGenerationBackend(FrameGenerationBackend backend,
                                                              const VulkanDevice* vulkan_device) {
  switch (backend) {
    case FrameGenerationBackend::kRepeatDebug:
      return {vulkan_device != nullptr, vulkan_device ? "" : "no Vulkan device"};
    case FrameGenerationBackend::kFsr3:
      // The stub. The design: ffxCreateContextDescFrameGeneration and
      // ffxDispatchDescFrameGeneration writing into the presenter's output
      // image under the presenter's own pacing, never the FidelityFX
      // swapchain (which would fight present IDs, vulkan_present_wait and the
      // UI drawers), with the same inputs as the MetalFX backend: the scene
      // without the HUD, the composited frame as the UI layer, depth, RG16F
      // motion vectors, the camera and the interval.
      return {false, "FSR 3.1 frame generation is not wired yet"};
    default:
      return ProbeFrameGenerationBackendGeneric(backend);
  }
}

std::unique_ptr<VulkanFrameGenerator> CreateVulkanFrameGenerator(FrameGenerationBackend backend,
                                                                 const VulkanDevice* vulkan_device) {
  if (!ProbeVulkanFrameGenerationBackend(backend, vulkan_device).available) {
    return nullptr;
  }
  switch (backend) {
    case FrameGenerationBackend::kRepeatDebug:
      return std::make_unique<RepeatFrameGenerator>(vulkan_device);
    default:
      return nullptr;
  }
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
