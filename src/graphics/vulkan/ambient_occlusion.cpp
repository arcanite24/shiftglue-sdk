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
// Twelve samples a pixel are too few to hold still: up close the spiral
// spreads over a hundred pixels and its noise crawled as the camera moved.
// So each frame turns the pattern by a new step, and a second pass blends
// the estimate with the previous frame's result, found through the
// camera's reprojection (the one the upscalers' motion vectors use). The
// history is dropped where its view z disagrees (disocclusion, moving cars)
// or after a camera cut, and clamped to the range of this frame's estimate
// around the pixel: a moving occluder's shade (the wheels' on the road,
// where the depth is unchanged) otherwise trailed behind it as a row of
// fading copies. Two result images alternate: one written, the other read
// as history.
//
// The passes write the occlusion and the view z it was computed at
// (RG16F), so the blur and the history test need no second depth fetch.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace {

// Bindings: 0 the camera depth, 1 this frame's estimate, 2 the result, 3 the
// previous result (history).
const char* const kAmbientOcclusionCommon = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform AmbientOcclusionConstants {
  layout(row_major) mat4 reprojection;
  uvec2 full_size;
  uvec2 ao_size;
  float depth_a;
  float depth_b;
  float tan_half_x;
  float tan_half_y;
  float radius;
  float fade_start;
  float fade_end;
  // 1: the history is this view's previous frame; 2: the matrix's depth is
  // 1 minus the stored one (FH1).
  uint flags;
  float rotation;
} c;

layout(set = 0, binding = 0) uniform texture2D depth;
layout(set = 0, binding = 3) uniform texture2D history;

// The share of each new frame in the running result: about 7 frames of
// memory, short enough that moving shade does not trail.
const float kNewWeight = 0.15;

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

// The previous frame's result at the full-size pixel p's place in it, or -1.
float History(ivec2 p, float stored_z) {
  if ((c.flags & 1u) == 0u) return -1.0;
  vec2 uv = (vec2(p) + 0.5) / vec2(c.full_size);
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float matrix_z = (c.flags & 2u) != 0u ? 1.0 - stored_z : stored_z;
  // The scene's clip x and y are the screen's negated (turned 180 degrees).
  vec4 previous = c.reprojection * vec4(-ndc, matrix_z, 1.0);
  if (!(previous.w > 1e-6)) return -1.0;
  vec3 previous_clip = previous.xyz / previous.w;
  vec2 previous_uv = vec2(-previous_clip.x * 0.5 + 0.5, 0.5 + previous_clip.y * 0.5);
  if (any(lessThan(previous_uv, vec2(0.0))) || any(greaterThan(previous_uv, vec2(1.0)))) {
    return -1.0;
  }
  float previous_stored = (c.flags & 2u) != 0u ? 1.0 - previous_clip.z : previous_clip.z;
  float previous_view_z = ViewZ(previous_stored);
  // Bilinear over the half-size texels, each kept only if it saw the same
  // surface (view z within 3 %).
  vec2 texel = previous_uv * vec2(c.full_size) * 0.5 - 0.5;
  ivec2 base = ivec2(floor(texel));
  vec2 f = texel - vec2(base);
  float sum = 0.0;
  float weights = 0.0;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      ivec2 tap = base + ivec2(x, y);
      if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, ivec2(c.ao_size)))) continue;
      vec2 h = texelFetch(history, tap, 0).rg;
      float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
      if (abs(h.g - previous_view_z) > 0.03 * previous_view_z + 0.02) continue;
      sum += h.r * w;
      weights += w;
    }
  }
  return weights > 0.25 ? sum / weights : -1.0;
}
)";

// This frame's estimate.
const char* const kAmbientOcclusionEstimate = R"(
layout(set = 0, binding = 1, rg16f) writeonly uniform image2D raw;

const int kSamples = 12;
const float kTurns = 7.0;

void main() {
  ivec2 ao_pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(ao_pixel), c.ao_size))) {
    return;
  }
  ivec2 p = ao_pixel * 2;
  vec3 position = ViewPosition(p);
  // The sky, and anything past the fade, gets none.
  if (!(position.z > 0.0) || position.z >= c.fade_end) {
    imageStore(raw, ao_pixel, vec4(1.0, 1e6, 0.0, 0.0));
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
    // Up close the spiral would spread past what 12 samples can cover
    // without blotches; a world radius that shrinks there is the lesser
    // error.
    float max_pixels = 0.06 * float(c.full_size.y);
    float radius = c.radius * min(1.0, max_pixels / radius_pixels);
    radius_pixels = min(radius_pixels, max_pixels);
    // A 4x4 ordered pattern turns the spiral per pixel, and each frame turns
    // it all by a further step: the upsample's 4x4 box holds all 16 turns
    // wherever it sits, and the history averages the frames' steps.
    const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                       3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    float noise =
        fract((kBayer[(ao_pixel.y & 3) * 4 + (ao_pixel.x & 3)] + 0.5) / 16.0 + c.rotation);
    float radius2 = radius * radius;
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
  imageStore(raw, ao_pixel, vec4(1.0 - occlusion, position.z, 0.0, 0.0));
}
)";

// The estimate blended with the clamped history.
const char* const kAmbientOcclusionTemporal = R"(
layout(set = 0, binding = 1, rg16f) readonly uniform image2D raw;
layout(set = 0, binding = 2, rg16f) writeonly uniform image2D dest;

void main() {
  ivec2 ao_pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(ao_pixel), c.ao_size))) {
    return;
  }
  vec2 center = imageLoad(raw, ao_pixel).rg;
  float ao = center.r;
  if (center.g < 1e5) {
    // The range of this frame's estimate over the 3x3 texels on the same
    // surface: every 3x3 window holds most of the pattern's turns, so a
    // shade the history keeps but no turn here sees is a trail.
    float low = center.r;
    float high = center.r;
    float tolerance = 0.04 * center.g + 0.05;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        ivec2 tap = clamp(ao_pixel + ivec2(x, y), ivec2(0), ivec2(c.ao_size) - 1);
        vec2 t = imageLoad(raw, tap).rg;
        if (abs(t.g - center.g) > tolerance) continue;
        low = min(low, t.r);
        high = max(high, t.r);
      }
    }
    // The stored depth back from the view z.
    float stored_z = c.depth_a + c.depth_b / center.g;
    float previous = History(ao_pixel * 2, stored_z);
    if (previous >= 0.0) ao = mix(clamp(previous, low, high), ao, kNewWeight);
  }
  imageStore(dest, ao_pixel, vec4(ao, center.g, 0.0, 0.0));
}
)";

// The golden ratio's fraction: successive frames' turns stay spread out.
constexpr double kRotationStep = 0.61803399;

}  // namespace

bool VulkanCommandProcessor::EnsureSwapAmbientOcclusion(uint32_t full_width,
                                                        uint32_t full_height) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (swap_ao_pipeline_ == VK_NULL_HANDLE) {
    if (swap_ao_failed_) return false;
    swap_ao_failed_ = true;
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = (i == 1 || i == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 4;
    set_layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &swap_ao_set_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the ambient occlusion descriptor set layout");
      return false;
    }
    VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                               sizeof(float) * 16 + sizeof(uint32_t) * 13};
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
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kMaxFramesInFlight},
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
    for (VkPipeline* pipeline : {&swap_ao_pipeline_, &swap_ao_temporal_pipeline_}) {
      const std::string source = std::string(kAmbientOcclusionCommon) +
                                 (pipeline == &swap_ao_pipeline_ ? kAmbientOcclusionEstimate
                                                                 : kAmbientOcclusionTemporal);
      std::vector<uint32_t> spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source.c_str(), spirv,
                              compile_error)) {
        REXGPU_WARN("Failed to compile the ambient occlusion shader: {}", compile_error);
        return false;
      }
      *pipeline = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_ao_pipeline_layout_, spirv.data(),
          sizeof(uint32_t) * spirv.size());
      if (*pipeline == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create the ambient occlusion pipeline");
        return false;
      }
    }
    swap_ao_failed_ = false;
  }

  const uint32_t width = (full_width + 1) / 2;
  const uint32_t height = (full_height + 1) / 2;
  if (swap_ao_images_[0] != VK_NULL_HANDLE && swap_ao_width_ == width &&
      swap_ao_height_ == height) {
    return true;
  }
  DestroySwapAmbientOcclusionImages();

  // Two results (this frame's and the history), then the estimate.
  for (uint32_t i = 0; i < 3; ++i) {
    VkImage& image = i < 2 ? swap_ao_images_[i] : swap_ao_raw_image_;
    VkDeviceMemory& memory = i < 2 ? swap_ao_memory_[i] : swap_ao_raw_memory_;
    VkImageView& view = i < 2 ? swap_ao_views_[i] : swap_ao_raw_view_;
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
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
            memory)) {
      REXGPU_WARN("Failed to create the ambient occlusion image");
      DestroySwapAmbientOcclusionImages();
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16_SFLOAT;
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the ambient occlusion image view");
      DestroySwapAmbientOcclusionImages();
      return false;
    }
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
  // The other image holds the previous frame's result, if the previous swap
  // ran this pass for the same view.
  const uint32_t current = swap_ao_current_ ^ 1;
  const uint32_t previous = swap_ao_current_;
  const bool history = swap_ao_history_valid_ && swap_ao_last_frame_ + 1 == frame_current_ &&
                       !camera.cut;
  PushImageMemoryBarrier(swap_ao_images_[current], range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, swap_ao_layouts_[current],
                         VK_IMAGE_LAYOUT_GENERAL);
  // The estimate stays in GENERAL: written by the first pass, loaded by the
  // second.
  PushImageMemoryBarrier(swap_ao_raw_image_, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, swap_ao_raw_layout_,
                         VK_IMAGE_LAYOUT_GENERAL);
  swap_ao_raw_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  if (swap_ao_layouts_[previous] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    // Never written (the first frame): bound but not read.
    PushImageMemoryBarrier(swap_ao_images_[previous], range,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT,
                           swap_ao_layouts_[previous], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    swap_ao_layouts_[previous] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  SubmitBarriers(true);

  VkDescriptorSet set = swap_ao_descriptor_sets_[frame_index];
  VkDescriptorImageInfo infos[4] = {
      {VK_NULL_HANDLE, camera_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, swap_ao_raw_view_, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, swap_ao_views_[current], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, swap_ao_views_[previous], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = (i == 1 || i == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &infos[i];
  }
  dfn.vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   swap_ao_pipeline_layout_, 0, 1, &set, 0,
                                                   nullptr);
  const float tan_half_y = std::tan(camera.fov_y * 0.5f);
  const float radius = float(REXCVAR_GET(swap_ambient_occlusion_radius));
  struct {
    float reprojection[16];
    uint32_t full_size[2];
    uint32_t ao_size[2];
    float depth_a;
    float depth_b;
    float tan_half_x;
    float tan_half_y;
    float radius;
    float fade_start;
    float fade_end;
    uint32_t flags;
    float rotation;
  } constants = {{},
                 {full_width, full_height},
                 {swap_ao_width_, swap_ao_height_},
                 camera.depth_a,
                 camera.depth_b,
                 tan_half_y * float(full_width) / float(full_height),
                 tan_half_y,
                 radius,
                 radius * 80.0f,
                 radius * 200.0f,
                 (history ? 1u : 0u) | (camera.depth_matrix_flipped ? 2u : 0u),
                 float(std::fmod(double(frame_current_) * kRotationStep, 1.0))};
  std::copy(std::begin(camera.reprojection), std::end(camera.reprojection),
            constants.reprojection);
  static_assert(sizeof(constants) == sizeof(float) * 16 + sizeof(uint32_t) * 13);
  deferred_command_buffer_.CmdVkPushConstants(swap_ao_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                              &constants);
  BindExternalComputePipeline(swap_ao_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((swap_ao_width_ + 7) / 8, (swap_ao_height_ + 7) / 8, 1);
  PushImageMemoryBarrier(swap_ao_raw_image_, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  BindExternalComputePipeline(swap_ao_temporal_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((swap_ao_width_ + 7) / 8, (swap_ao_height_ + 7) / 8, 1);

  PushImageMemoryBarrier(swap_ao_images_[current], range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  swap_ao_layouts_[current] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  swap_ao_current_ = current;
  swap_ao_history_valid_ = true;
  swap_ao_last_frame_ = frame_current_;
}

void VulkanCommandProcessor::DestroySwapAmbientOcclusionImages() {
  const uint64_t submission = GetCurrentSubmission();
  for (uint32_t i = 0; i < 3; ++i) {
    VkImageView& view = i < 2 ? swap_ao_views_[i] : swap_ao_raw_view_;
    VkImage& image = i < 2 ? swap_ao_images_[i] : swap_ao_raw_image_;
    VkDeviceMemory& memory = i < 2 ? swap_ao_memory_[i] : swap_ao_raw_memory_;
    if (view != VK_NULL_HANDLE) destroy_image_views_.emplace_back(submission, view);
    if (image != VK_NULL_HANDLE) destroy_images_.emplace_back(submission, image);
    if (memory != VK_NULL_HANDLE) destroy_memory_.emplace_back(submission, memory);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }
  swap_ao_layouts_ = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
  swap_ao_raw_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  swap_ao_width_ = 0;
  swap_ao_height_ = 0;
  swap_ao_history_valid_ = false;
}

void VulkanCommandProcessor::DestroySwapAmbientOcclusion() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DestroySwapAmbientOcclusionImages();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_ao_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_ao_temporal_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_ao_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_ao_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_ao_set_layout_);
  swap_ao_failed_ = false;
}

}  // namespace rex::graphics::vulkan
