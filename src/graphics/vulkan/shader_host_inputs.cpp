/**
 * @file        graphics/vulkan/shader_host_inputs.cpp
 *
 * @brief       Host inputs for replacement shaders: blue noise and the draw
 *              resolution scale in set 0 (vulkan_shader_override_host_inputs).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/vulkan/command_processor.h>

#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/vulkan/shader_override_files.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_shader_override_host_inputs, false, "GPU",
                    "Give replacement pixel shaders two extra inputs in descriptor set 0: a "
                    "64x64 tileable blue noise texture (binding 5, sampler2D, nearest, "
                    "repeat) and a 16-byte block with the draw resolution scale (binding 6). "
                    "Off leaves the descriptor layout as it was")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::vulkan {

bool VulkanCommandProcessor::ShaderHostInputsWanted() {
  return REXCVAR_GET(vulkan_shader_override_host_inputs);
}

bool VulkanCommandProcessor::CreateShaderHostInputs() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t size = kHostInputBlueNoiseSize;

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {size, size, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          host_input_noise_image_, host_input_noise_memory_)) {
    REXGPU_ERROR("Shader host inputs: failed to create the blue noise image");
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = host_input_noise_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = image_info.format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &host_input_noise_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Shader host inputs: failed to create the blue noise view");
    return false;
  }
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = 0.0f;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &host_input_noise_sampler_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Shader host inputs: failed to create the blue noise sampler");
    return false;
  }

  // The constants: host-visible, mapped for the life of the buffer, written
  // only while nothing is using set 0 (start and draw resolution scale
  // switches, the callers of WriteSharedMemoryAndEdramDescriptorSet).
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(HostInputConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, host_input_constants_buffer_,
          host_input_constants_memory_, &host_input_constants_memory_type_,
          &host_input_constants_memory_size_)) {
    REXGPU_ERROR("Shader host inputs: failed to create the constant buffer");
    return false;
  }
  if (dfn.vkMapMemory(device, host_input_constants_memory_, 0, VK_WHOLE_SIZE, 0,
                      &host_input_constants_mapping_) != VK_SUCCESS) {
    REXGPU_ERROR("Shader host inputs: failed to map the constant buffer");
    return false;
  }
  host_input_noise_upload_pending_ = true;
  SetVulkanShaderOverrideHostInputs(true);
  REXGPU_INFO("Shader host inputs: blue noise at set 0 binding {}, constants at binding {}",
              kHostInputBlueNoiseBinding, kHostInputConstantsBinding);
  return true;
}

void VulkanCommandProcessor::WriteShaderHostInputConstants() {
  if (!host_input_constants_mapping_)
    return;
  HostInputConstants constants = {};
  constants.draw_resolution_scale[0] = float(texture_cache_->draw_resolution_scale_x());
  constants.draw_resolution_scale[1] = float(texture_cache_->draw_resolution_scale_y());
  constants.blue_noise_texel = 1.0f / float(kHostInputBlueNoiseSize);
  constants.version = 1;
  std::memcpy(host_input_constants_mapping_, &constants, sizeof(constants));
  ui::vulkan::util::FlushMappedMemoryRange(GetVulkanDevice(), host_input_constants_memory_,
                                           host_input_constants_memory_type_, 0,
                                           host_input_constants_memory_size_, sizeof(constants));
}

void VulkanCommandProcessor::UploadShaderHostInputNoise() {
  if (!host_input_noise_upload_pending_)
    return;
  host_input_noise_upload_pending_ = false;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t size = kHostInputBlueNoiseSize;
  const std::vector<uint8_t> texels = GenerateBlueNoiseRgba8(size);

  VkBuffer upload_buffer = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory = VK_NULL_HANDLE;
  uint32_t upload_memory_type = 0;
  VkDeviceSize upload_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, upload_buffer, upload_memory,
          &upload_memory_type, &upload_memory_size)) {
    REXGPU_WARN("Shader host inputs: failed to create the blue noise upload buffer");
    return;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXGPU_WARN("Shader host inputs: failed to map the blue noise upload buffer");
    dfn.vkDestroyBuffer(device, upload_buffer, nullptr);
    dfn.vkFreeMemory(device, upload_memory, nullptr);
    return;
  }
  std::memcpy(mapping, texels.data(), texels.size());
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, upload_memory, upload_memory_type, 0,
                                           upload_memory_size, texels.size());
  dfn.vkUnmapMemory(device, upload_memory);

  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(host_input_noise_image_, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {size, size, 1};
  deferred_command_buffer_.CmdVkCopyBufferToImage(upload_buffer, host_input_noise_image_,
                                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  PushImageMemoryBarrier(host_input_noise_image_, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const uint64_t submission = GetCurrentSubmission();
  destroy_buffers_.emplace_back(submission, upload_buffer);
  destroy_memory_.emplace_back(submission, upload_memory);
}

void VulkanCommandProcessor::DestroyShaderHostInputs() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  SetVulkanShaderOverrideHostInputs(false);
  if (host_input_constants_mapping_) {
    dfn.vkUnmapMemory(device, host_input_constants_memory_);
    host_input_constants_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, host_input_constants_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, host_input_constants_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, host_input_noise_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, host_input_noise_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, host_input_noise_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, host_input_noise_memory_);
}

}  // namespace rex::graphics::vulkan
