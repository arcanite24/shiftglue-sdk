/**
 * @file        graphics/vulkan/scene_host_copy.cpp
 *
 * @brief       A 16-bit host copy of the scene the composite reads (issue #5,
 *              SR-12), given to replacement shaders as host input binding 8.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// The composite's scene texture is the full-size pass's output, drawn into a
// k_2_10_10_10 render target and resolved to memory at 10 bits a channel.
// With unorm10_render_target_as_unorm16 that target is R16G16B16A16_UNORM on
// the host, so the drawn values keep 16 bits until the resolve packs them.
// Here, the resolve to the address the composite's fetch 0 read in the last
// frame (recorded at the mid-frame hook, vulkan_midframe_hook_pixel_shaders
// set to the composite) is also copied as it is into a fixed RGBA16 image the
// size of the scene, which set 0 binding 8 holds for every guest pipeline.
// The copy is in the resolve source's layout, which is the scene texture's,
// and in the target's units: the resolve's exponent bias is given as
// host input constant scene_copy_scale (binding 6).
// Until a copy is made the image is 0, and a replacement that compares it
// with the packed texel (the composite's SCENE_FLOAT) uses the texel.

#include <cmath>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/graphics/vulkan/shader_override_files.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(scene_host_copy, false, "GPU",
                    "With vulkan_shader_override_host_inputs and "
                    "unorm10_render_target_as_unorm16: copy the composite's scene into set 0 "
                    "binding 8 at 16 bits a channel when it is resolved (list the composite's "
                    "pixel shader in vulkan_midframe_hook_pixel_shaders), for a replacement "
                    "composite to read in place of the 10-bit texture (issue #5 SR-12)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {
// The scene's guest size in FH2.
constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;
}  // namespace

bool VulkanCommandProcessor::CreateSceneCopyInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t width = kDefaultWidth * texture_cache_->draw_resolution_scale_x();
  const uint32_t height = kDefaultHeight * texture_cache_->draw_resolution_scale_y();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R16G16B16A16_UNORM;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          scene_copy_image_, scene_copy_memory_)) {
    REXGPU_ERROR("Scene host copy: failed to create the image");
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = scene_copy_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = image_info.format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &scene_copy_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Scene host copy: failed to create the view");
    return false;
  }
  scene_copy_width_ = width;
  scene_copy_height_ = height;
  scene_copy_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  scene_copy_base_ = 0;
  return true;
}

void VulkanCommandProcessor::RecordSceneCopyClear() {
  if (scene_copy_image_ == VK_NULL_HANDLE || scene_copy_layout_ != VK_IMAGE_LAYOUT_UNDEFINED) {
    return;
  }
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(scene_copy_image_, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  const VkClearColorValue zero = {};
  deferred_command_buffer_.CmdVkClearColorImage(
      scene_copy_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
  PushImageMemoryBarrier(scene_copy_image_, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  scene_copy_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanCommandProcessor::RunSceneCopyHook(const MidframeHookContext& context) {
  (void)context;
  if (!fh1_native_executor_ || scene_copy_image_ == VK_NULL_HANDLE)
    return;
  if (!REXCVAR_GET(scene_host_copy)) {
    scene_copy_base_ = 0;
  } else {
    scene_copy_base_ = register_file_->GetTextureFetch(0).base_address << 12;
  }
  fh1_native_executor_->SetSceneCopyTarget(scene_copy_image_, &scene_copy_layout_,
                                           scene_copy_width_, scene_copy_height_, scene_copy_base_);
  // The resolve packs 2^bias times the target; the copy is the target.
  const float scale = std::ldexp(1.0f, fh1_native_executor_->scene_copy_exp_bias());
  if (scale != scene_copy_scale_) {
    scene_copy_scale_ = scale;
    WriteShaderHostInputConstants();
  }
}

void VulkanCommandProcessor::DestroySceneCopyInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (fh1_native_executor_) {
    fh1_native_executor_->SetSceneCopyTarget(VK_NULL_HANDLE, nullptr, 0, 0, 0);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, scene_copy_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, scene_copy_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scene_copy_memory_);
}

}  // namespace rex::graphics::vulkan
