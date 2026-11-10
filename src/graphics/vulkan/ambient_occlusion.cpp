// Screen-space ambient occlusion at swap. FH1 and FH2 render none (both
// titles' depth readers are the shadow mask and soft particles), so this
// adds it on the host, after the frame, the way a post-process injector
// would: a half-resolution scalable ambient obscurance pass (McGuire et al.,
// "Scalable Ambient Obscurance", 2012) over the camera depth that UP-1.3
// already snapshots (fh1_camera.cpp), then the scene-only post
// (scene_post.cpp) blurs it with depth-aware weights and darkens the
// pixels the HUD left alone. The lens comes from the world's matrix
// (Fh1NativeExecutor::Camera): view z is B / (depth - A), which fits FH1's
// depth and FH2's reversed depth alike, and x and y come from the vertical
// field of view and the output's aspect.
//
// The pass writes the occlusion and the view z it was computed at
// (RG16F), so the blur needs no second depth fetch per tap.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace {

const char* const kAmbientOcclusionSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform AmbientOcclusionConstants {
  uvec2 full_size;
  uvec2 ao_size;
  float depth_a;
  float depth_b;
  float tan_half_x;
  float tan_half_y;
  float radius;
  float fade_start;
  float fade_end;
} c;

layout(set = 0, binding = 0) uniform texture2D depth;
layout(set = 0, binding = 1, rg16f) writeonly uniform image2D dest;

const int kSamples = 12;
const float kTurns = 7.0;

// The stored depth at a screen pixel. The scene is drawn turned 180 degrees
// (the post pass turns it back), so its depth is at the opposite pixel.
float GuestZ(ivec2 p) {
  ivec2 scene_pixel = ivec2(c.full_size) - 1 - clamp(p, ivec2(0), ivec2(c.full_size) - 1);
  return texelFetch(depth, scene_pixel, 0).r;
}

float ViewZ(float z) {
  float d = z - c.depth_a;
  return abs(d) > 1e-7 ? c.depth_b / d : 1e9;
}

vec3 ViewPosition(ivec2 p) {
  float vz = ViewZ(GuestZ(p));
  vec2 ndc = (vec2(p) + 0.5) / vec2(c.full_size) * 2.0 - 1.0;
  return vec3(ndc.x * c.tan_half_x * vz, -ndc.y * c.tan_half_y * vz, vz);
}

void main() {
  ivec2 ao_pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(ao_pixel), c.ao_size))) {
    return;
  }
  ivec2 p = ao_pixel * 2;
  vec3 position = ViewPosition(p);
  // The sky, and anything past the fade, gets none.
  if (!(position.z > 0.0) || position.z >= c.fade_end) {
    imageStore(dest, ao_pixel, vec4(1.0, 1e6, 0.0, 0.0));
    return;
  }
  // The normal from the neighbour on the smoother side, so silhouettes do not
  // tilt it.
  vec3 right = ViewPosition(p + ivec2(2, 0)) - position;
  vec3 left = position - ViewPosition(p - ivec2(2, 0));
  vec3 down = ViewPosition(p + ivec2(0, 2)) - position;
  vec3 up = position - ViewPosition(p - ivec2(0, 2));
  vec3 dx = abs(right.z) < abs(left.z) ? right : left;
  vec3 dy = abs(down.z) < abs(up.z) ? down : up;
  vec3 normal = normalize(cross(dx, dy));
  if (dot(normal, position) > 0.0) {
    normal = -normal;
  }

  float radius_pixels = c.radius * 0.5 * float(c.full_size.y) / (c.tan_half_y * position.z);
  float occlusion = 0.0;
  if (radius_pixels >= 2.0) {
    radius_pixels = min(radius_pixels, 0.12 * float(c.full_size.y));
    // A 4x4 ordered pattern rotates the spiral per pixel; the upsample's 4x4
    // box holds all 16 rotations wherever it sits, so the pattern averages
    // out instead of showing as stripes.
    const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                       3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    float noise = (kBayer[(ao_pixel.y & 3) * 4 + (ao_pixel.x & 3)] + 0.5) / 16.0;
    float radius2 = c.radius * c.radius;
    float bias = 0.02 + 0.002 * position.z;
    float sum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
      float alpha = (float(i) + 0.5) / float(kSamples);
      float angle = (alpha * kTurns + noise) * 6.2831853;
      vec2 offset = vec2(cos(angle), sin(angle)) * (alpha * radius_pixels);
      vec3 v = ViewPosition(p + ivec2(round(offset))) - position;
      float vv = dot(v, v);
      float f = max(radius2 - vv, 0.0);
      sum += f * f * f * max((dot(v, normal) - bias) / (vv + 0.01), 0.0);
    }
    occlusion = min(1.0, sum * 5.0 / (radius2 * radius2 * radius2 * float(kSamples)));
  }
  occlusion *= 1.0 - smoothstep(c.fade_start, c.fade_end, position.z);
  imageStore(dest, ao_pixel, vec4(1.0 - occlusion, position.z, 0.0, 0.0));
}
)";

}  // namespace

bool VulkanCommandProcessor::EnsureSwapAmbientOcclusion(uint32_t full_width,
                                                        uint32_t full_height) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (swap_ao_pipeline_ == VK_NULL_HANDLE) {
    if (swap_ao_failed_) return false;
    swap_ao_failed_ = true;
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &swap_ao_set_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the ambient occlusion descriptor set layout");
      return false;
    }
    VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                               sizeof(uint32_t) * 11};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &swap_ao_set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                   &swap_ao_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the ambient occlusion pipeline layout");
      return false;
    }
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kMaxFramesInFlight;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &swap_ao_descriptor_pool_) !=
        VK_SUCCESS) {
      REXGPU_WARN("Failed to create the ambient occlusion descriptor pool");
      return false;
    }
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> set_layouts;
    set_layouts.fill(swap_ao_set_layout_);
    VkDescriptorSetAllocateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = swap_ao_descriptor_pool_;
    set_info.descriptorSetCount = kMaxFramesInFlight;
    set_info.pSetLayouts = set_layouts.data();
    if (dfn.vkAllocateDescriptorSets(device, &set_info, swap_ao_descriptor_sets_.data()) !=
        VK_SUCCESS) {
      REXGPU_WARN("Failed to allocate the ambient occlusion descriptor sets");
      return false;
    }
    std::vector<uint32_t> spirv;
    std::string compile_error;
    if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kAmbientOcclusionSource, spirv,
                            compile_error)) {
      REXGPU_WARN("Failed to compile the ambient occlusion shader: {}", compile_error);
      return false;
    }
    swap_ao_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_ao_pipeline_layout_, spirv.data(), sizeof(uint32_t) * spirv.size());
    if (swap_ao_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the ambient occlusion pipeline");
      return false;
    }
    swap_ao_failed_ = false;
  }

  const uint32_t width = (full_width + 1) / 2;
  const uint32_t height = (full_height + 1) / 2;
  if (swap_ao_image_ != VK_NULL_HANDLE && swap_ao_width_ == width && swap_ao_height_ == height) {
    return true;
  }
  const uint64_t submission = GetCurrentSubmission();
  if (swap_ao_view_ != VK_NULL_HANDLE) destroy_image_views_.emplace_back(submission, swap_ao_view_);
  if (swap_ao_image_ != VK_NULL_HANDLE) destroy_images_.emplace_back(submission, swap_ao_image_);
  if (swap_ao_memory_ != VK_NULL_HANDLE) destroy_memory_.emplace_back(submission, swap_ao_memory_);
  swap_ao_view_ = VK_NULL_HANDLE;
  swap_ao_image_ = VK_NULL_HANDLE;
  swap_ao_memory_ = VK_NULL_HANDLE;
  swap_ao_width_ = 0;
  swap_ao_height_ = 0;
  swap_ao_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R16G16_SFLOAT;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          swap_ao_image_, swap_ao_memory_)) {
    REXGPU_WARN("Failed to create the ambient occlusion image");
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = swap_ao_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R16G16_SFLOAT;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &swap_ao_view_) != VK_SUCCESS) {
    REXGPU_WARN("Failed to create the ambient occlusion image view");
    return false;
  }
  swap_ao_width_ = width;
  swap_ao_height_ = height;
  return true;
}

void VulkanCommandProcessor::DispatchSwapAmbientOcclusion(
    VkImageView camera_depth, const Fh1NativeExecutor::Camera& camera, uint32_t full_width,
    uint32_t full_height, uint32_t frame_index) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(swap_ao_image_, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, swap_ao_layout_, VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);

  VkDescriptorSet set = swap_ao_descriptor_sets_[frame_index];
  VkDescriptorImageInfo infos[2] = {
      {VK_NULL_HANDLE, camera_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, swap_ao_view_, VK_IMAGE_LAYOUT_GENERAL},
  };
  const VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = types[i];
    writes[i].pImageInfo = &infos[i];
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   swap_ao_pipeline_layout_, 0, 1, &set, 0,
                                                   nullptr);
  const float tan_half_y = std::tan(camera.fov_y * 0.5f);
  const float radius = float(REXCVAR_GET(swap_ambient_occlusion_radius));
  struct {
    uint32_t full_size[2];
    uint32_t ao_size[2];
    float near_plane;
    float far_plane;
    float tan_half_x;
    float tan_half_y;
    float radius;
    float fade_start;
    float fade_end;
  } constants = {{full_width, full_height},
                 {swap_ao_width_, swap_ao_height_},
                 camera.depth_a,
                 camera.depth_b,
                 tan_half_y * float(full_width) / float(full_height),
                 tan_half_y,
                 radius,
                 radius * 80.0f,
                 radius * 200.0f};
  deferred_command_buffer_.CmdVkPushConstants(swap_ao_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                              &constants);
  BindExternalComputePipeline(swap_ao_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((swap_ao_width_ + 7) / 8, (swap_ao_height_ + 7) / 8, 1);

  PushImageMemoryBarrier(swap_ao_image_, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  swap_ao_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanCommandProcessor::DestroySwapAmbientOcclusion() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint64_t submission = GetCurrentSubmission();
  if (swap_ao_view_ != VK_NULL_HANDLE) destroy_image_views_.emplace_back(submission, swap_ao_view_);
  if (swap_ao_image_ != VK_NULL_HANDLE) destroy_images_.emplace_back(submission, swap_ao_image_);
  if (swap_ao_memory_ != VK_NULL_HANDLE) destroy_memory_.emplace_back(submission, swap_ao_memory_);
  swap_ao_view_ = VK_NULL_HANDLE;
  swap_ao_image_ = VK_NULL_HANDLE;
  swap_ao_memory_ = VK_NULL_HANDLE;
  swap_ao_width_ = 0;
  swap_ao_height_ = 0;
  swap_ao_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_ao_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_ao_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_ao_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_ao_set_layout_);
  swap_ao_failed_ = false;
}

}  // namespace rex::graphics::vulkan
