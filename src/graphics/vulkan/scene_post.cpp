// Scene-only post effects at swap (#528, #521): FXAA, a 3D colour LUT and
// ambient occlusion (ambient_occlusion.cpp) applied to the pixels the HUD
// left alone. The pass reads the gamma-applied
// frame (the FXAA source image, luma in alpha), compares the frame with the
// snapshot taken before the HUD was drawn to find the HUD, and writes the
// guest output. Frames without a scene snapshot (menus) skip it.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/cube_lut.h>
#include <rex/filesystem.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace {

// flags: 1 FXAA, 2 extreme quality, 4 LUT, 8 FXAA over the HUD too, 16
// ambient occlusion, 32 the occlusion term alone (a debug view).
const char* const kScenePostSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform ScenePostConstants {
  uvec2 size;
  vec2 size_inv;
  uint flags;
  float lut_strength;
  float lut_scale;
  float lut_offset;
  uvec2 ao_size;
  float ao_strength;
  float depth_a;
  float depth_b;
} c;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform texture2D frame;
layout(set = 0, binding = 2) uniform texture2D scene;
layout(set = 0, binding = 3) uniform sampler3D lut;
layout(set = 0, binding = 4, rgb10_a2) writeonly uniform image2D dest;
// The half-resolution occlusion and its view z, and the camera depth.
layout(set = 0, binding = 5) uniform texture2D ao;
layout(set = 0, binding = 6) uniform texture2D depth;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

// The frame and the snapshot may not share a channel order: compare the
// sorted channels, as the HDR HUD mask does.
vec3 Sorted(vec3 v) {
  float low = min(v.r, min(v.g, v.b));
  float high = max(v.r, max(v.g, v.b));
  return vec3(low, v.r + v.g + v.b - low - high, high);
}

bool Changed(ivec2 pixel) {
  pixel = clamp(pixel, ivec2(0), ivec2(c.size) - 1);
  vec3 difference = abs(Sorted(texelFetch(frame, pixel, 0).rgb) -
                        Sorted(texelFetch(scene, pixel, 0).rgb));
  return max(difference.x, max(difference.y, difference.z)) > 0.006;
}

// The HUD and a pixel around it, for its antialiased edges.
bool Hud(ivec2 pixel) {
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      if (Changed(pixel + ivec2(x, y))) return true;
    }
  }
  return false;
}

float SampleLuma(vec2 uv) { return textureLod(source, uv, 0.0).a; }

vec3 Fxaa(vec2 uv, vec4 rgbm, bool extreme) {
  float edge_threshold = extreme ? 0.063 : 0.166;
  float edge_threshold_min = extreme ? 0.0312 : 0.0833;
  float span_max = extreme ? 12.0 : 8.0;
  vec2 texel = c.size_inv;
  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2(texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x, texel.y));
  float lumaSE = SampleLuma(uv + vec2(texel.x, texel.y));
  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  if (lumaMax - lumaMin < max(edge_threshold_min, lumaMax * edge_threshold)) {
    return rgbm.rgb;
  }
  vec2 dir;
  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
  dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
  float dir_reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
  float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
  dir = clamp(dir * rcp_dir_min, vec2(-span_max), vec2(span_max)) * texel;
  vec3 rgbA = 0.5 * (textureLod(source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                     textureLod(source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(source, uv + dir * -0.5, 0.0).rgb +
                                   textureLod(source, uv + dir * 0.5, 0.0).rgb);
  float lumaB = dot(rgbB, kLumaWeights);
  return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

// A 4x4 bilateral box over the half-size occlusion: every 4x4 window holds
// all 16 rotations of its sampling pattern, so the box removes the pattern;
// each tap is weighted by how close its view z is to this pixel's, so edges
// do not bleed.
float AmbientOcclusion(ivec2 pixel) {
  // The scene is drawn turned 180 degrees, as in ambient_occlusion.cpp.
  float d = texelFetch(depth, ivec2(c.size) - 1 - pixel, 0).r - c.depth_a;
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
      vec2 sample_ao = texelFetch(ao, tap, 0).rg;
      float weight = exp(-abs(sample_ao.g - view_z) / tolerance);
      sum += sample_ao.r * weight;
      weights += weight;
    }
  }
  return weights > 1e-4 ? sum / weights : 1.0;
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), c.size))) {
    return;
  }
  vec2 uv = (vec2(pixel) + vec2(0.5)) * c.size_inv;
  vec4 rgbm = textureLod(source, uv, 0.0);
  vec3 color = rgbm.rgb;
  bool hud = Hud(pixel);
  if ((c.flags & 1u) != 0u && (!hud || (c.flags & 8u) != 0u)) {
    color = Fxaa(uv, rgbm, (c.flags & 2u) != 0u);
  }
  if ((c.flags & 16u) != 0u && !hud) {
    float ao = mix(1.0, AmbientOcclusion(pixel), c.ao_strength);
    color = (c.flags & 32u) != 0u ? vec3(ao) : color * ao;
  }
  if ((c.flags & 4u) != 0u && !hud) {
    vec3 graded = textureLod(lut, clamp(color, 0.0, 1.0) * c.lut_scale + c.lut_offset, 0.0).rgb;
    color = mix(color, graded, c.lut_strength);
  }
  imageStore(dest, pixel, vec4(color, 1.0));
}
)";

constexpr uint32_t kScenePostBindings = 7;

uint32_t PackA2B10G10R10(float r, float g, float b) {
  const auto unorm10 = [](float value) {
    return uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 1023.0f));
  };
  return unorm10(r) | (unorm10(g) << 10) | (unorm10(b) << 20) | (3u << 30);
}

}  // namespace

bool VulkanCommandProcessor::EnsureSwapScenePost() {
  if (swap_scene_post_pipeline_ != VK_NULL_HANDLE) return true;
  if (swap_scene_post_failed_) return false;
  swap_scene_post_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkDescriptorType types[kScenePostBindings] = {
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
  VkDescriptorSetLayoutBinding bindings[kScenePostBindings] = {};
  for (uint32_t i = 0; i < kScenePostBindings; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = types[i];
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = kScenePostBindings;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &swap_scene_post_set_layout_) != VK_SUCCESS) {
    REXGPU_WARN("Failed to create the scene post descriptor set layout");
    return false;
  }
  VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                             sizeof(uint32_t) * 13};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &swap_scene_post_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &swap_scene_post_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_WARN("Failed to create the scene post pipeline layout");
    return false;
  }
  VkDescriptorPoolSize pool_sizes[3] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kMaxFramesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * kMaxFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxFramesInFlight},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kMaxFramesInFlight;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr,
                                 &swap_scene_post_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_WARN("Failed to create the scene post descriptor pool");
    return false;
  }
  std::array<VkDescriptorSetLayout, kMaxFramesInFlight> set_layouts;
  set_layouts.fill(swap_scene_post_set_layout_);
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = swap_scene_post_descriptor_pool_;
  set_info.descriptorSetCount = kMaxFramesInFlight;
  set_info.pSetLayouts = set_layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &set_info,
                                   swap_scene_post_descriptor_sets_.data()) != VK_SUCCESS) {
    REXGPU_WARN("Failed to allocate the scene post descriptor sets");
    return false;
  }
  std::vector<uint32_t> spirv;
  std::string compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kScenePostSource, spirv, compile_error)) {
    REXGPU_WARN("Failed to compile the scene post shader: {}", compile_error);
    return false;
  }
  swap_scene_post_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_scene_post_pipeline_layout_, spirv.data(),
      sizeof(uint32_t) * spirv.size());
  if (swap_scene_post_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN("Failed to create the scene post pipeline");
    return false;
  }
  swap_scene_post_failed_ = false;
  return true;
}

void VulkanCommandProcessor::DestroySceneLut() {
  const uint64_t submission = GetCurrentSubmission();
  if (scene_lut_view_ != VK_NULL_HANDLE) {
    destroy_image_views_.emplace_back(submission, scene_lut_view_);
    scene_lut_view_ = VK_NULL_HANDLE;
  }
  if (scene_lut_image_ != VK_NULL_HANDLE) {
    destroy_images_.emplace_back(submission, scene_lut_image_);
    scene_lut_image_ = VK_NULL_HANDLE;
  }
  if (scene_lut_memory_ != VK_NULL_HANDLE) {
    destroy_memory_.emplace_back(submission, scene_lut_memory_);
    scene_lut_memory_ = VK_NULL_HANDLE;
  }
  scene_lut_size_ = 0;
  scene_lut_real_ = false;
}

bool VulkanCommandProcessor::UpdateSceneLut() {
  const std::string path = REXCVAR_GET(scene_lut_path);
  if (scene_lut_loaded_ && path == scene_lut_path_) return scene_lut_real_;
  scene_lut_loaded_ = true;
  scene_lut_path_ = path;
  DestroySceneLut();

  CubeLut lut;
  bool real = false;
  if (!path.empty()) {
    std::string error;
    // The cvar holds UTF-8; a std::string converts through the ANSI code
    // page on Windows.
    real = LoadCubeLut(rex::to_path(path), lut, error);
    if (real) {
      REXGPU_INFO("Scene LUT: {} ({}^3)", path, lut.size);
    } else {
      REXGPU_WARN("Scene LUT {} not used: {}", path, error);
    }
  }
  if (!real) {
    // Bound while only FXAA runs; never sampled then.
    lut.size = 2;
    lut.rgb.clear();
    for (uint32_t b = 0; b < 2; ++b) {
      for (uint32_t g = 0; g < 2; ++g) {
        for (uint32_t r = 0; r < 2; ++r) {
          lut.rgb.insert(lut.rgb.end(), {float(r), float(g), float(b)});
        }
      }
    }
  }

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint32_t size = lut.size;
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_3D;
  // Sampled with linear filtering on every Vulkan device.
  image_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  image_info.extent = {size, size, size};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(vulkan_device, image_info,
                                                        ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                        scene_lut_image_, scene_lut_memory_)) {
    REXGPU_WARN("Failed to create the scene LUT image");
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = scene_lut_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
  view_info.format = image_info.format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &scene_lut_view_) != VK_SUCCESS) {
    REXGPU_WARN("Failed to create the scene LUT view");
    DestroySceneLut();
    return false;
  }

  const VkDeviceSize bytes = VkDeviceSize(size) * size * size * sizeof(uint32_t);
  VkBuffer upload_buffer = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory = VK_NULL_HANDLE;
  uint32_t upload_memory_type = 0;
  VkDeviceSize upload_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, upload_buffer, upload_memory,
          &upload_memory_type, &upload_memory_size)) {
    REXGPU_WARN("Failed to create the scene LUT upload buffer");
    DestroySceneLut();
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXGPU_WARN("Failed to map the scene LUT upload buffer");
    dfn.vkDestroyBuffer(device, upload_buffer, nullptr);
    dfn.vkFreeMemory(device, upload_memory, nullptr);
    DestroySceneLut();
    return false;
  }
  auto* texels = static_cast<uint32_t*>(mapping);
  for (size_t i = 0; i < size_t(size) * size * size; ++i) {
    texels[i] = PackA2B10G10R10(lut.rgb[3 * i], lut.rgb[3 * i + 1], lut.rgb[3 * i + 2]);
  }
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, upload_memory, upload_memory_type, 0,
                                           upload_memory_size, bytes);
  dfn.vkUnmapMemory(device, upload_memory);

  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(scene_lut_image_, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {size, size, size};
  deferred_command_buffer_.CmdVkCopyBufferToImage(upload_buffer, scene_lut_image_,
                                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  PushImageMemoryBarrier(scene_lut_image_, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const uint64_t submission = GetCurrentSubmission();
  destroy_buffers_.emplace_back(submission, upload_buffer);
  destroy_memory_.emplace_back(submission, upload_memory);
  scene_lut_size_ = size;
  scene_lut_real_ = real;
  return real;
}

}  // namespace rex::graphics::vulkan
