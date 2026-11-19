/**
 * @file        graphics/vulkan/scene_bloom.cpp
 *
 * @brief       Our own bloom from the scene's float bands (issue #5, SR-21's
 *              first step), given to replacement shaders as host input
 *              binding 10.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// At the composite's mid-frame hook, with scene_bloom and scene_host_copy,
// the bands' float copy (binding 9, scene_host_copy.cpp) is reduced to six
// levels from half size, then added back up, into an RGBA16 float image at
// half the scene's size: binding 10. The first reduction puts the copy in
// linear light (it stores the square root of the colour, in the units
// scene_bloom_prescale gives) and weighs each 2x2 group by 1 / (1 + luma)
// (Karis' average), so a single bright texel does not flicker as a block;
// the reductions are the 13-tap pattern of Jimenez 2014 (Next generation
// post processing in Call of Duty: Advanced Warfare), the additions a 3x3
// tent. The result is in the scene's linear light before exposure, the sum
// of the six levels; a replacement composite scales it. The image is cleared
// to 0 until a frame has it.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/graphics/vulkan/shader_override_files.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(scene_bloom, false, "GPU",
                    "With scene_host_copy: build our own bloom from the scene's float bands at "
                    "the composite's mid-frame hook into host input binding 10, for a "
                    "replacement composite (issue #5 SR-21)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;

// Modes: 0 the first reduction (to linear light, Karis' average), 1 a
// reduction, 2 an addition (the coarser level through a tent, plus `add`).
const char* const kSceneBloomSource = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform SceneBloomConstants {
  uvec2 dest_size;
  vec2 source_texel;
  uint mode;
  float prescale;
} c;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D add;
layout(set = 0, binding = 2, rgba16f) writeonly uniform image2D dest;

vec3 Tap(vec2 uv) {
  vec3 v = textureLod(source, uv, 0.0).rgb;
  if (c.mode == 0u) {
    v = max(v * c.prescale, vec3(0.0));
    v *= v;
  }
  return v;
}

float Weight(vec3 group) {
  return c.mode == 0u ? 1.0 / (1.0 + dot(group, vec3(0.2126, 0.7152, 0.0722))) : 1.0;
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), c.dest_size))) return;
  const vec2 uv = (vec2(pixel) + 0.5) / vec2(c.dest_size);
  const vec2 t = c.source_texel;
  vec3 result;
  if (c.mode <= 1u) {
    const vec3 a = Tap(uv + t * vec2(-2.0, -2.0)), b = Tap(uv + t * vec2(0.0, -2.0)),
               d = Tap(uv + t * vec2(2.0, -2.0));
    const vec3 e = Tap(uv + t * vec2(-1.0, -1.0)), f = Tap(uv + t * vec2(1.0, -1.0));
    const vec3 g = Tap(uv + t * vec2(-2.0, 0.0)), h = Tap(uv), i = Tap(uv + t * vec2(2.0, 0.0));
    const vec3 j = Tap(uv + t * vec2(-1.0, 1.0)), k = Tap(uv + t * vec2(1.0, 1.0));
    const vec3 l = Tap(uv + t * vec2(-2.0, 2.0)), m = Tap(uv + t * vec2(0.0, 2.0)),
               n = Tap(uv + t * vec2(2.0, 2.0));
    const vec3 groups[5] = vec3[5]((e + f + j + k) * 0.25, (a + b + g + h) * 0.25,
                                   (b + d + h + i) * 0.25, (g + h + l + m) * 0.25,
                                   (h + i + m + n) * 0.25);
    const float shares[5] = float[5](0.5, 0.125, 0.125, 0.125, 0.125);
    result = vec3(0.0);
    float total = 0.0;
    for (int s = 0; s < 5; ++s) {
      const float w = shares[s] * Weight(groups[s]);
      result += groups[s] * w;
      total += w;
    }
    result /= total;
  } else {
    result = (Tap(uv + t * vec2(-1.0, -1.0)) + Tap(uv + t * vec2(1.0, -1.0)) +
              Tap(uv + t * vec2(-1.0, 1.0)) + Tap(uv + t * vec2(1.0, 1.0))) * (1.0 / 16.0) +
             (Tap(uv + t * vec2(0.0, -1.0)) + Tap(uv + t * vec2(-1.0, 0.0)) +
              Tap(uv + t * vec2(1.0, 0.0)) + Tap(uv + t * vec2(0.0, 1.0))) * (2.0 / 16.0) +
             Tap(uv) * (4.0 / 16.0) + textureLod(add, uv, 0.0).rgb;
  }
  imageStore(dest, pixel, vec4(result, 1.0));
}
)";

}  // namespace

bool VulkanCommandProcessor::CreateSceneBloomInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  uint32_t width = kDefaultWidth * texture_cache_->draw_resolution_scale_x();
  uint32_t height = kDefaultHeight * texture_cache_->draw_resolution_scale_y();
  for (uint32_t i = 0; i < kSceneBloomImages; ++i) {
    // 0-5 the reductions, 6-10 the additions (6 at level 0, 10 at level 4).
    const uint32_t level = i < kSceneBloomLevels ? i : i - kSceneBloomLevels;
    SceneBloomImage& image = scene_bloom_images_[i];
    image.width = std::max(1u, (width + (2u << level) - 1) >> (level + 1));
    image.height = std::max(1u, (height + (2u << level) - 1) >> (level + 1));
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = {image.width, image.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
            image.memory)) {
      REXGPU_ERROR("Scene bloom: failed to create an image");
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &image.view) != VK_SUCCESS) {
      REXGPU_ERROR("Scene bloom: failed to create a view");
      return false;
    }
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  return true;
}

void VulkanCommandProcessor::RecordSceneBloomClear() {
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  for (SceneBloomImage& image : scene_bloom_images_) {
    if (image.image == VK_NULL_HANDLE || image.layout != VK_IMAGE_LAYOUT_UNDEFINED)
      continue;
    PushImageMemoryBarrier(image.image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    SubmitBarriers(true);
    const VkClearColorValue zero = {};
    deferred_command_buffer_.CmdVkClearColorImage(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  &zero, 1, &range);
    PushImageMemoryBarrier(
        image.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SubmitBarriers(true);
    image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

bool VulkanCommandProcessor::EnsureSceneBloomPipeline() {
  if (scene_bloom_pipeline_ != VK_NULL_HANDLE)
    return true;
  if (scene_bloom_failed_)
    return false;
  scene_bloom_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetLayoutBinding bindings[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 3;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &scene_bloom_set_layout_) != VK_SUCCESS) {
    REXGPU_WARN("Scene bloom: failed to create the descriptor set layout");
    return false;
  }
  VkPushConstantRange push_constant_range = {
      VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(uint32_t) * 2 + sizeof(float) * 2 + sizeof(uint32_t) + sizeof(float)};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &scene_bloom_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &scene_bloom_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_WARN("Scene bloom: failed to create the pipeline layout");
    return false;
  }
  // One set per pass; the images never change, so they are written once.
  constexpr uint32_t kPasses = kSceneBloomImages;
  VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kPasses},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kPasses},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kPasses;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &scene_bloom_descriptor_pool_) !=
      VK_SUCCESS) {
    REXGPU_WARN("Scene bloom: failed to create the descriptor pool");
    return false;
  }
  std::array<VkDescriptorSetLayout, kPasses> set_layouts;
  set_layouts.fill(scene_bloom_set_layout_);
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = scene_bloom_descriptor_pool_;
  set_info.descriptorSetCount = kPasses;
  set_info.pSetLayouts = set_layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &set_info, scene_bloom_sets_.data()) != VK_SUCCESS) {
    REXGPU_WARN("Scene bloom: failed to allocate the descriptor sets");
    return false;
  }
  // Pass p writes image p in the order the passes run: the reductions
  // 0-5 read the bands, then the reduction before; the additions 6-10 (level
  // 4 down to 0) read the next coarser level (the last reduction for level 4)
  // and add their own level's reduction.
  for (uint32_t p = 0; p < kPasses; ++p) {
    VkImageView source, add, dest;
    if (p < kSceneBloomLevels) {
      source = p ? scene_bloom_images_[p - 1].view : scene_band_view_;
      add = source;
      dest = scene_bloom_images_[p].view;
    } else {
      const uint32_t level = kSceneBloomLevels - 1 - (p - kSceneBloomLevels + 1);
      source = level == kSceneBloomLevels - 2
                   ? scene_bloom_images_[kSceneBloomLevels - 1].view
                   : scene_bloom_images_[kSceneBloomLevels + level + 1].view;
      add = scene_bloom_images_[level].view;
      dest = scene_bloom_images_[kSceneBloomLevels + level].view;
    }
    VkDescriptorImageInfo infos[3] = {
        {scene_ao_sampler_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {scene_ao_sampler_, add, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = scene_bloom_sets_[p];
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = &infos[i];
    }
    dfn.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
  }
  std::vector<uint32_t> spirv;
  std::string compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kSceneBloomSource, spirv, compile_error)) {
    REXGPU_WARN("Scene bloom: failed to compile: {}", compile_error);
    return false;
  }
  scene_bloom_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, scene_bloom_pipeline_layout_, spirv.data(), sizeof(uint32_t) * spirv.size());
  if (scene_bloom_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN("Scene bloom: failed to create the pipeline");
    return false;
  }
  scene_bloom_failed_ = false;
  return true;
}

void VulkanCommandProcessor::RunSceneBloom(const MidframeHookContext& context) {
  if (IsSceneCopyPassHook(context) || !REXCVAR_GET(scene_bloom) || !REXCVAR_GET(scene_host_copy) ||
      scene_band_image_ == VK_NULL_HANDLE || scene_bloom_images_[0].image == VK_NULL_HANDLE ||
      !EnsureSceneBloomPipeline()) {
    return;
  }
  RecordSceneBloomClear();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  // The bands' copy in the composite's texel units, times its fetch's
  // exponent scale: the square root of the scene's linear colour.
  const float prescale =
      scene_band_scale_ * std::ldexp(1.0f, register_file_->GetTextureFetch(0).exp_adjust);
  BindExternalComputePipeline(scene_bloom_pipeline_);
  for (uint32_t p = 0; p < kSceneBloomImages; ++p) {
    uint32_t dest_index, source_width, source_height;
    if (p < kSceneBloomLevels) {
      dest_index = p;
      source_width = p ? scene_bloom_images_[p - 1].width : scene_copy_width_;
      source_height = p ? scene_bloom_images_[p - 1].height : scene_copy_height_;
    } else {
      const uint32_t level = kSceneBloomLevels - 1 - (p - kSceneBloomLevels + 1);
      dest_index = kSceneBloomLevels + level;
      const SceneBloomImage& coarser = level == kSceneBloomLevels - 2
                                           ? scene_bloom_images_[kSceneBloomLevels - 1]
                                           : scene_bloom_images_[kSceneBloomLevels + level + 1];
      source_width = coarser.width;
      source_height = coarser.height;
    }
    SceneBloomImage& dest = scene_bloom_images_[dest_index];
    PushImageMemoryBarrier(
        dest.image, range,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        dest.layout, VK_IMAGE_LAYOUT_GENERAL);
    SubmitBarriers(true);
    deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                     scene_bloom_pipeline_layout_, 0, 1,
                                                     &scene_bloom_sets_[p], 0, nullptr);
    struct {
      uint32_t dest_size[2];
      float source_texel[2];
      uint32_t mode;
      float prescale;
    } constants = {{dest.width, dest.height},
                   {1.0f / float(source_width), 1.0f / float(source_height)},
                   p == 0                  ? 0u
                   : p < kSceneBloomLevels ? 1u
                                           : 2u,
                   prescale};
    deferred_command_buffer_.CmdVkPushConstants(scene_bloom_pipeline_layout_,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                                &constants);
    deferred_command_buffer_.CmdVkDispatch((dest.width + 7) / 8, (dest.height + 7) / 8, 1);
    PushImageMemoryBarrier(
        dest.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SubmitBarriers(true);
    dest.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

void VulkanCommandProcessor::DestroySceneBloomInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, scene_bloom_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         scene_bloom_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         scene_bloom_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         scene_bloom_set_layout_);
  for (SceneBloomImage& image : scene_bloom_images_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image.view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image.image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image.memory);
  }
}

}  // namespace rex::graphics::vulkan
