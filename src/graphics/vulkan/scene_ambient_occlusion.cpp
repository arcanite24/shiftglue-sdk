/**
 * @file        graphics/vulkan/scene_ambient_occlusion.cpp
 *
 * @brief       The swap's ambient occlusion moved into the frame (issue #5,
 *              SR-11's first user): computed at a mid-frame hook and given to
 *              replacement shaders as host input binding 7.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// At swap (ambient_occlusion.cpp, scene_post.cpp) the occlusion darkens the
// finished, tone-mapped image, so it also darkens bloom and highlights. With
// swap_ambient_occlusion_in_scene the same passes run at the mid-frame hook
// (vulkan_midframe_hook_pixel_shaders, set to the composite's hash), from
// this frame's camera depth, which the banded copy has finished by then, and
// the last swap's lens. A third pass upsamples the half-size result with the
// scene post's depth-aware 4x4 box into a fixed RGBA8 image at the output
// size, in the orientation of the scene texture the composite reads (the
// camera depth's, turned 180 degrees against the output) and with the
// strength applied; set 0
// binding 7 holds it for every guest pipeline, and a replacement composite
// multiplies its scene by it before adding the bloom. The swap then skips
// its own occlusion for that frame. Where the hook could not compute it, the
// image is cleared to 1, so a replacement reading it changes nothing.
//
// The temporal pass reprojects its history with the last swap's camera
// motion, one frame behind; its clamp to the current estimate's range keeps
// that from trailing.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/shader_override_files.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(swap_ambient_occlusion_in_scene, false, "GPU",
                    "With swap_ambient_occlusion and vulkan_shader_override_host_inputs: "
                    "compute the occlusion at the mid-frame hook (list the composite's pixel "
                    "shader in vulkan_midframe_hook_pixel_shaders) and give it to replacement "
                    "shaders as set 0 binding 7 instead of darkening the finished image at swap "
                    "(issue #5 SR-11). Needs a replacement composite that applies it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

// Bindings: 0 the camera depth, 1 the half-size occlusion (RG: occlusion,
// view z), 2 the destination.
const char* const kSceneAoUpsampleSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform SceneAoConstants {
  uvec2 full_size;
  uvec2 ao_size;
  uvec2 dest_size;
  float depth_a;
  float depth_b;
  float strength;
} c;

layout(set = 0, binding = 0) uniform texture2D depth;
layout(set = 0, binding = 1) uniform texture2D ao;
layout(set = 0, binding = 2, rgba8) writeonly uniform image2D dest;

// The scene post's upsample (scene_post.cpp): a 4x4 box over the half-size
// occlusion, which holds all 16 turns of its pattern, each tap weighted by
// how close its view z is to the pixel's. The scene is drawn turned 180
// degrees against the camera depth.
float Occlusion(ivec2 pixel) {
  float d = texelFetch(depth, ivec2(c.full_size) - 1 - pixel, 0).r - c.depth_a;
  float view_z = abs(d) > 1e-7 ? c.depth_b / d : 1e9;
  if (!(view_z > 0.0) || view_z >= 1e6) return 1.0;
  vec2 position = (vec2(pixel) + 0.5) * 0.5 - 0.5;
  ivec2 base = ivec2(floor(position)) - 1;
  float tolerance = 0.04 * view_z + 0.05;
  float sum = 0.0;
  float weights = 0.0;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), ivec2(c.ao_size) - 1);
      vec2 s = texelFetch(ao, tap, 0).rg;
      float weight = exp(-abs(s.g - view_z) / tolerance);
      sum += s.r * weight;
      weights += weight;
    }
  }
  return weights > 1e-4 ? sum / weights : 1.0;
}

void main() {
  ivec2 at = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(at), c.dest_size))) return;
  ivec2 pixel = min(ivec2((vec2(at) + 0.5) * vec2(c.full_size) / vec2(c.dest_size)),
                    ivec2(c.full_size) - 1);
  float occlusion = mix(1.0, Occlusion(pixel), c.strength);
  // The composite reads the scene the way the camera depth holds it, turned
  // against the output: store it that way round.
  imageStore(dest, ivec2(c.dest_size) - 1 - at, vec4(occlusion, 0.0, 0.0, 1.0));
}
)";

// The image's size when the first swap has not told the output's: FH's
// 1280x720. It is sampled by UV, so another output size is only filtered.
constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;

}  // namespace

bool VulkanCommandProcessor::CreateSceneAoInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t width = kDefaultWidth * texture_cache_->draw_resolution_scale_x();
  const uint32_t height = kDefaultHeight * texture_cache_->draw_resolution_scale_y();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, scene_ao_image_,
          scene_ao_memory_)) {
    REXGPU_ERROR("Scene ambient occlusion: failed to create the image");
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = scene_ao_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = image_info.format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &scene_ao_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Scene ambient occlusion: failed to create the view");
    return false;
  }
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 0.0f;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &scene_ao_sampler_) != VK_SUCCESS) {
    REXGPU_ERROR("Scene ambient occlusion: failed to create the sampler");
    return false;
  }
  scene_ao_width_ = width;
  scene_ao_height_ = height;
  scene_ao_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  scene_ao_clear_ = false;
  scene_ao_frame_ = UINT64_MAX;
  return true;
}

void VulkanCommandProcessor::RecordSceneAoClear() {
  if (scene_ao_image_ == VK_NULL_HANDLE)
    return;
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(
      scene_ao_image_, range,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      scene_ao_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  const VkClearColorValue one = {{1.0f, 1.0f, 1.0f, 1.0f}};
  deferred_command_buffer_.CmdVkClearColorImage(
      scene_ao_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
  PushImageMemoryBarrier(scene_ao_image_, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  scene_ao_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  scene_ao_clear_ = true;
}

void VulkanCommandProcessor::RunSceneAmbientOcclusion(const MidframeHookContext& context) {
  (void)context;
  const bool wanted = REXCVAR_GET(swap_ambient_occlusion_in_scene) &&
                      REXCVAR_GET(swap_ambient_occlusion) &&
                      REXCVAR_GET(swap_ambient_occlusion_strength) > 0;
  if (!wanted || !fh1_native_executor_ || scene_ao_image_ == VK_NULL_HANDLE) {
    if (!scene_ao_clear_)
      RecordSceneAoClear();
    return;
  }
  const uint32_t full_width = scene_ao_output_width_ ? scene_ao_output_width_ : scene_ao_width_;
  const uint32_t full_height = scene_ao_output_height_ ? scene_ao_output_height_ : scene_ao_height_;
  // The swap turns camera tracking on only while it wants the camera; ask
  // for it here too, from the next frame.
  fh1_native_executor_->SetCameraTrackingEnabled(true);
  Fh1NativeExecutor::Camera camera = {};
  const VkImageView depth_view =
      fh1_native_executor_->PeekCameraFrame(full_width, full_height, camera);
  if (depth_view == VK_NULL_HANDLE || camera.depth_b == 0.0f ||
      !EnsureSwapAmbientOcclusion(full_width, full_height)) {
    if (!scene_ao_clear_)
      RecordSceneAoClear();
    return;
  }

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (scene_ao_pipeline_ == VK_NULL_HANDLE) {
    if (scene_ao_failed_)
      return;
    scene_ao_failed_ = true;
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType =
          i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 3;
    set_layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &scene_ao_set_layout_) !=
        VK_SUCCESS) {
      REXGPU_WARN("Scene ambient occlusion: failed to create the descriptor set layout");
      return;
    }
    VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                               sizeof(uint32_t) * 6 + sizeof(float) * 3};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &scene_ao_set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                   &scene_ao_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Scene ambient occlusion: failed to create the pipeline layout");
      return;
    }
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kMaxFramesInFlight;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &scene_ao_descriptor_pool_) !=
        VK_SUCCESS) {
      REXGPU_WARN("Scene ambient occlusion: failed to create the descriptor pool");
      return;
    }
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> set_layouts;
    set_layouts.fill(scene_ao_set_layout_);
    VkDescriptorSetAllocateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = scene_ao_descriptor_pool_;
    set_info.descriptorSetCount = kMaxFramesInFlight;
    set_info.pSetLayouts = set_layouts.data();
    if (dfn.vkAllocateDescriptorSets(device, &set_info, scene_ao_descriptor_sets_.data()) !=
        VK_SUCCESS) {
      REXGPU_WARN("Scene ambient occlusion: failed to allocate the descriptor sets");
      return;
    }
    std::vector<uint32_t> spirv;
    std::string compile_error;
    if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kSceneAoUpsampleSource, spirv,
                            compile_error)) {
      REXGPU_WARN("Scene ambient occlusion: failed to compile the upsample: {}", compile_error);
      return;
    }
    scene_ao_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, scene_ao_pipeline_layout_, spirv.data(), sizeof(uint32_t) * spirv.size());
    if (scene_ao_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Scene ambient occlusion: failed to create the upsample pipeline");
      return;
    }
    scene_ao_failed_ = false;
  }

  const uint32_t frame_index = uint32_t(frame_current_ % kMaxFramesInFlight);
  DispatchSwapAmbientOcclusion(depth_view, camera, full_width, full_height, frame_index);

  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(scene_ao_image_, range, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, scene_ao_layout_, VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  VkDescriptorSet set = scene_ao_descriptor_sets_[frame_index];
  VkDescriptorImageInfo infos[3] = {
      {VK_NULL_HANDLE, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, swap_ao_views_[swap_ao_current_], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, scene_ao_view_, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet writes[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &infos[i];
  }
  dfn.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, scene_ao_pipeline_layout_, 0, 1, &set, 0, nullptr);
  struct {
    uint32_t full_size[2];
    uint32_t ao_size[2];
    uint32_t dest_size[2];
    float depth_a;
    float depth_b;
    float strength;
  } constants = {{full_width, full_height},
                 {swap_ao_width_, swap_ao_height_},
                 {scene_ao_width_, scene_ao_height_},
                 camera.depth_a,
                 camera.depth_b,
                 float(REXCVAR_GET(swap_ambient_occlusion_strength)) / 100.0f};
  deferred_command_buffer_.CmdVkPushConstants(
      scene_ao_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  BindExternalComputePipeline(scene_ao_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((scene_ao_width_ + 7) / 8, (scene_ao_height_ + 7) / 8, 1);
  PushImageMemoryBarrier(scene_ao_image_, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  scene_ao_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  scene_ao_clear_ = false;
  scene_ao_frame_ = frame_current_;
}

void VulkanCommandProcessor::DestroySceneAoInput() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, scene_ao_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         scene_ao_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         scene_ao_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         scene_ao_set_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, scene_ao_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, scene_ao_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, scene_ao_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scene_ao_memory_);
}

}  // namespace rex::graphics::vulkan
