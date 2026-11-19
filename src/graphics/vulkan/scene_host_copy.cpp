/**
 * @file        graphics/vulkan/scene_host_copy.cpp
 *
 * @brief       16-bit host copies of the scene the composite reads (issue #5,
 *              SR-12), given to replacement shaders as host inputs 8 and 9.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// FH2 draws the scene in bands into 7e3 float targets (RGBA16 float on the
// host) and resolves each to its rows of one k_2_10_10_10 texture, 10 bits a
// channel with an exponent bias. A full-size pass (an edge filter that passes
// the texel through away from edges) reads that texture and draws into a
// k_2_10_10_10 target, which is resolved over the same texture; the
// composite reads it. Both resolves lose the targets' precision.
//
// The composite's fetch 0 address is recorded at the mid-frame hook (list
// the composite's pixel shader in vulkan_midframe_hook_pixel_shaders), and in
// the next frame the executor copies, as they are:
//  - each band's resolve into those rows of an RGBA16 float image, binding 9;
//  - with unorm10_render_target_as_unorm16, the full-size target's resolve
//    into an RGBA16 unorm image, binding 8.
// The copies are in the resolve sources' layout, which is the texture's. Host
// input constants (binding 6) give what each is multiplied by to be in the
// composite's texel units: scene_copy_scale (2^ the resolve's bias) and
// scene_band_scale (the band resolve's bias, what the full-size pass
// multiplies its texel by, which is read at its own hook, named by
// scene_host_copy_pass, and the full-size resolve's bias). Until a copy is
// made the images are 0, and a replacement that compares a copy with the
// packed texel (the composite's SCENE_FLOAT) uses the texel.

#include <cmath>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/graphics/vulkan/shader_override_files.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(scene_host_copy, false, "GPU",
                    "With vulkan_shader_override_host_inputs: copy the composite's scene "
                    "bands (binding 9) and, with unorm10_render_target_as_unorm16, the full-size "
                    "pass's output (binding 8) at 16 bits a channel when they are resolved (list "
                    "the composite's pixel shader in vulkan_midframe_hook_pixel_shaders), for a "
                    "replacement composite to read in place of the 10-bit texture (issue #5 "
                    "SR-12)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(scene_host_copy_pass, "", "GPU",
                      "The pixel shader (16 hex digits) of the pass between the scene bands and "
                      "the composite (FH2: 64AEAE9314A0118B); also list it in "
                      "vulkan_midframe_hook_pixel_shaders, so scene_host_copy can relate the "
                      "bands to the composite's texel")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {
// The scene's guest size in FH2.
constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;

uint64_t ScenePassHash() {
  const std::string text = REXCVAR_GET(scene_host_copy_pass);
  return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 16);
}
}  // namespace

bool VulkanCommandProcessor::CreateSceneCopyInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t width = kDefaultWidth * texture_cache_->draw_resolution_scale_x();
  const uint32_t height = kDefaultHeight * texture_cache_->draw_resolution_scale_y();

  auto create = [&](VkFormat format, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
            memory)) {
      REXGPU_ERROR("Scene host copy: failed to create an image");
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
      REXGPU_ERROR("Scene host copy: failed to create a view");
      return false;
    }
    return true;
  };
  if (!create(VK_FORMAT_R16G16B16A16_UNORM, scene_copy_image_, scene_copy_memory_,
              scene_copy_view_) ||
      !create(VK_FORMAT_R16G16B16A16_SFLOAT, scene_band_image_, scene_band_memory_,
              scene_band_view_)) {
    return false;
  }
  scene_copy_width_ = width;
  scene_copy_height_ = height;
  scene_copy_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  scene_band_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  scene_copy_base_ = 0;
  return true;
}

void VulkanCommandProcessor::RecordSceneCopyClear() {
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  for (auto [image, layout] : {std::pair{scene_copy_image_, &scene_copy_layout_},
                               std::pair{scene_band_image_, &scene_band_layout_}}) {
    if (image == VK_NULL_HANDLE || *layout != VK_IMAGE_LAYOUT_UNDEFINED)
      continue;
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    SubmitBarriers(true);
    const VkClearColorValue zero = {};
    deferred_command_buffer_.CmdVkClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  &zero, 1, &range);
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SubmitBarriers(true);
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

bool VulkanCommandProcessor::IsSceneCopyPassHook(const MidframeHookContext& context) const {
  const uint64_t pass = ScenePassHash();
  return pass && context.pixel_shader_hash == pass;
}

void VulkanCommandProcessor::RunSceneCopyHook(const MidframeHookContext& context) {
  if (!fh1_native_executor_ || scene_copy_image_ == VK_NULL_HANDLE)
    return;
  if (!REXCVAR_GET(scene_host_copy)) {
    scene_copy_base_ = 0;
    fh1_native_executor_->SetSceneCopyTarget(VK_NULL_HANDLE, nullptr, VK_NULL_HANDLE, nullptr, 0, 0,
                                             0);
    return;
  }
  if (IsSceneCopyPassHook(context)) {
    // The full-size pass writes its texel times its fetch's exponent scale
    // and its target's exponent bias where it passes it through.
    const int32_t fetch_exp = register_file_->GetTextureFetch(0).exp_adjust;
    const int32_t target_exp =
        register_file_->Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0])
            .color_exp_bias;
    scene_pass_scale_ = std::ldexp(1.0f, fetch_exp + target_exp);
    return;
  }
  scene_copy_base_ = register_file_->GetTextureFetch(0).base_address << 12;
  fh1_native_executor_->SetSceneCopyTarget(scene_copy_image_, &scene_copy_layout_,
                                           scene_band_image_, &scene_band_layout_,
                                           scene_copy_width_, scene_copy_height_, scene_copy_base_);
  // The resolves pack 2^bias times their targets; the copies are the targets.
  const int32_t copy_exp = fh1_native_executor_->scene_copy_exp_bias();
  const float copy_scale = std::ldexp(1.0f, copy_exp);
  const float band_scale =
      std::ldexp(scene_pass_scale_, fh1_native_executor_->scene_band_exp_bias() + copy_exp);
  if (copy_scale != scene_copy_scale_ || band_scale != scene_band_scale_) {
    scene_copy_scale_ = copy_scale;
    scene_band_scale_ = band_scale;
    WriteShaderHostInputConstants();
  }
}

void VulkanCommandProcessor::DestroySceneCopyInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (fh1_native_executor_) {
    fh1_native_executor_->SetSceneCopyTarget(VK_NULL_HANDLE, nullptr, VK_NULL_HANDLE, nullptr, 0, 0,
                                             0);
  }
  for (auto [view, image, memory] :
       {std::tuple{&scene_copy_view_, &scene_copy_image_, &scene_copy_memory_},
        std::tuple{&scene_band_view_, &scene_band_image_, &scene_band_memory_}}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, *view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, *image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, *memory);
  }
}

}  // namespace rex::graphics::vulkan
