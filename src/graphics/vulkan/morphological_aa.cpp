/**
 * @file        graphics/vulkan/morphological_aa.cpp
 *
 * @brief       Morphological antialiasing at swap (swap_morphological_aa).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/vulkan/command_processor.h>

#include <array>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(swap_morphological_aa, false, "GPU",
                    "Morphological antialiasing of the guest output at swap (Vulkan): edges "
                    "by luma, their shapes found along each edge and blended by the area "
                    "under the reconstructed silhouette; the HUD is left alone where the "
                    "pre-HUD snapshot tells it apart")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(swap_morphological_aa_threshold, 0.1, "GPU",
                      "Luma step that counts as an edge for swap_morphological_aa")
    .range(0.02, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

// Three passes, one source: edges (and a copy of the image), the blend
// weights of every edge pixel, and the blend. This is the scheme of
// morphological antialiasing (Reshetov, "Morphological Antialiasing", HPG
// 2009; Jimenez et al., "SMAA", 2012) with the coverage computed in the
// shader rather than read from a precomputed area texture, and without
// diagonal patterns.
const char* const kMorphologicalAaCommon = R"(
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform Constants {
  uvec2 size;
  uint flags;  // 1: tell the HUD apart with the snapshot
  float threshold;
} c;
layout(set = 0, binding = 0, rgb10_a2) uniform image2D target;
layout(set = 0, binding = 1, rgba16f) uniform image2D copy_image;
layout(set = 0, binding = 2, rgba8) uniform image2D edges;
layout(set = 0, binding = 3, rgba8) uniform image2D weights;
layout(set = 0, binding = 4) uniform texture2D frame;
layout(set = 0, binding = 5) uniform texture2D scene;

const int kMaxSearch = 16;

ivec2 Clamp(ivec2 p) { return clamp(p, ivec2(0), ivec2(c.size) - 1); }
bool Inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(c.size))); }
float Luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

vec3 Sorted(vec3 v) {
  float low = min(v.r, min(v.g, v.b));
  float high = max(v.r, max(v.g, v.b));
  return vec3(low, v.r + v.g + v.b - low - high, high);
}
// A HUD pixel: the frame differs from the snapshot taken before the HUD.
bool Hud(ivec2 p) {
  if ((c.flags & 1u) == 0u) return false;
  p = Clamp(p);
  vec3 d = abs(Sorted(texelFetch(frame, p, 0).rgb) - Sorted(texelFetch(scene, p, 0).rgb));
  return max(d.x, max(d.y, d.z)) > 0.006;
}

// edges: r a boundary on the pixel's left, g on its top.
vec2 Edge(ivec2 p) { return Inside(p) ? imageLoad(edges, p).rg : vec2(0.0); }

// The area under a line through the pattern, in the pixel column [0, 1]:
// the segment runs from x0 to x1 (pixel boundaries, x0 <= 0 < 1 <= x1),
// its ends at heights h0 and h1 (+-0.5 toward a crossing edge, 0 without
// one); the silhouette goes from each end to height 0 at the middle.
// Returns (area below the edge line, area above it).
vec2 Area(float x0, float x1, float h0, float h1) {
  float m = 0.5 * (x0 + x1);
  vec2 result = vec2(0.0);
  for (int half_index = 0; half_index < 2; ++half_index) {
    float a = half_index == 0 ? x0 : m;
    float b = half_index == 0 ? m : x1;
    float h = half_index == 0 ? h0 : h1;
    if (h == 0.0 || b <= a) continue;
    float lo = max(a, 0.0), hi = min(b, 1.0);
    if (hi <= lo) continue;
    // Height falls linearly to 0 toward the middle.
    float ha = half_index == 0 ? h * (m - lo) / (m - a) : h * (lo - m) / (b - m);
    float hb = half_index == 0 ? h * (m - hi) / (m - a) : h * (hi - m) / (b - m);
    float area = 0.5 * (ha + hb) * (hi - lo);
    if (area < 0.0) result.x -= area; else result.y += area;
  }
  return result;
}
)";

const char* const kMorphologicalAaEdges = R"(
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (!Inside(p)) return;
  vec4 color = imageLoad(target, p);
  imageStore(copy_image, p, color);
  float l = Luma(color.rgb);
  float left = abs(l - Luma(imageLoad(target, Clamp(p - ivec2(1, 0))).rgb));
  float top = abs(l - Luma(imageLoad(target, Clamp(p - ivec2(0, 1))).rgb));
  vec2 e = step(vec2(c.threshold), vec2(left, top));
  if (p.x == 0) e.x = 0.0;
  if (p.y == 0) e.y = 0.0;
  if (e.x + e.y > 0.0) {
    // Local contrast adaptation: a step much weaker than a neighbour's is
    // texture, not an edge.
    float right = abs(l - Luma(imageLoad(target, Clamp(p + ivec2(1, 0))).rgb));
    float bottom = abs(l - Luma(imageLoad(target, Clamp(p + ivec2(0, 1))).rgb));
    float left2 = abs(Luma(imageLoad(target, Clamp(p - ivec2(1, 0))).rgb) -
                      Luma(imageLoad(target, Clamp(p - ivec2(2, 0))).rgb));
    float top2 = abs(Luma(imageLoad(target, Clamp(p - ivec2(0, 1))).rgb) -
                     Luma(imageLoad(target, Clamp(p - ivec2(0, 2))).rgb));
    float strongest = max(max(left, top), max(max(right, bottom), max(left2, top2)));
    e *= step(vec2(strongest), 2.0 * vec2(left, top));
  }
  imageStore(edges, p, vec4(e, 0.0, 0.0));
}
)";

const char* const kMorphologicalAaWeights = R"(
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (!Inside(p)) return;
  vec2 e = Edge(p);
  vec4 w = vec4(0.0);
  if (e.g > 0.0) {
    // A horizontal boundary on the top: how far it runs left and right.
    int dl = 0, dr = 0;
    while (dl < kMaxSearch && Edge(p - ivec2(dl + 1, 0)).g > 0.0) ++dl;
    while (dr < kMaxSearch && Edge(p + ivec2(dr + 1, 0)).g > 0.0) ++dr;
    ivec2 el = p - ivec2(dl, 0), er = p + ivec2(dr + 1, 0);
    // Crossing edges at each end, below (this row, toward -0.5) or above.
    float hl = Edge(el).r - Edge(el - ivec2(0, 1)).r;
    float hr = Edge(er).r - Edge(er - ivec2(0, 1)).r;
    vec2 area = Area(float(-dl), float(dr + 1), -0.5 * hl, -0.5 * hr);
    // x: this pixel takes from its top neighbour; y: that one from this.
    w.xy = area;
  }
  if (e.r > 0.0) {
    // A vertical boundary on the left: how far it runs up and down.
    int du = 0, dd = 0;
    while (du < kMaxSearch && Edge(p - ivec2(0, du + 1)).r > 0.0) ++du;
    while (dd < kMaxSearch && Edge(p + ivec2(0, dd + 1)).r > 0.0) ++dd;
    ivec2 et = p - ivec2(0, du), eb = p + ivec2(0, dd + 1);
    float ht = Edge(et).g - Edge(et - ivec2(1, 0)).g;
    float hb = Edge(eb).g - Edge(eb - ivec2(1, 0)).g;
    vec2 area = Area(float(-du), float(dd + 1), -0.5 * ht, -0.5 * hb);
    // z: this pixel takes from its left neighbour; w: that one from this.
    w.zw = area;
  }
  imageStore(weights, p, w);
}
)";

const char* const kMorphologicalAaBlend = R"(
vec4 Copy(ivec2 p) { return imageLoad(copy_image, Clamp(p)); }
vec4 Weights(ivec2 p) { return Inside(p) ? imageLoad(weights, p) : vec4(0.0); }
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (!Inside(p)) return;
  vec4 here = Copy(p);
  float top = Weights(p).x;
  float bottom = Weights(p + ivec2(0, 1)).y;
  float left = Weights(p).z;
  float right = Weights(p + ivec2(1, 0)).w;
  if (max(max(top, bottom), max(left, right)) <= 0.0) return;
  // The HUD and a pixel around it keep their own edges.
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
      if (Hud(p + ivec2(x, y))) return;
  vec3 color;
  if (max(top, bottom) >= max(left, right)) {
    color = here.rgb * (1.0 - top - bottom) + Copy(p - ivec2(0, 1)).rgb * top +
            Copy(p + ivec2(0, 1)).rgb * bottom;
  } else {
    color = here.rgb * (1.0 - left - right) + Copy(p - ivec2(1, 0)).rgb * left +
            Copy(p + ivec2(1, 0)).rgb * right;
  }
  imageStore(target, p, vec4(color, here.a));
}
)";

constexpr uint32_t kMorphologicalAaBindings = 6;

}  // namespace

bool VulkanCommandProcessor::EnsureSwapMorphologicalAa(uint32_t width, uint32_t height) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (swap_mlaa_pipelines_[0] == VK_NULL_HANDLE) {
    if (swap_mlaa_failed_)
      return false;
    swap_mlaa_failed_ = true;
    VkDescriptorSetLayoutBinding bindings[kMorphologicalAaBindings] = {};
    for (uint32_t i = 0; i < kMorphologicalAaBindings; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType =
          i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = kMorphologicalAaBindings;
    set_layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &swap_mlaa_set_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the morphological AA descriptor set layout");
      return false;
    }
    VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                               sizeof(uint32_t) * 4};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &swap_mlaa_set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                   &swap_mlaa_pipeline_layout_) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create the morphological AA pipeline layout");
      return false;
    }
    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kMaxFramesInFlight;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &swap_mlaa_descriptor_pool_) !=
        VK_SUCCESS) {
      REXGPU_WARN("Failed to create the morphological AA descriptor pool");
      return false;
    }
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> set_layouts;
    set_layouts.fill(swap_mlaa_set_layout_);
    VkDescriptorSetAllocateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = swap_mlaa_descriptor_pool_;
    set_info.descriptorSetCount = kMaxFramesInFlight;
    set_info.pSetLayouts = set_layouts.data();
    if (dfn.vkAllocateDescriptorSets(device, &set_info, swap_mlaa_descriptor_sets_.data()) !=
        VK_SUCCESS) {
      REXGPU_WARN("Failed to allocate the morphological AA descriptor sets");
      return false;
    }
    const char* const passes[3] = {kMorphologicalAaEdges, kMorphologicalAaWeights,
                                   kMorphologicalAaBlend};
    for (uint32_t i = 0; i < 3; ++i) {
      const std::string source = std::string("#version 450\n") + kMorphologicalAaCommon + passes[i];
      std::vector<uint32_t> spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source.c_str(), spirv, compile_error)) {
        REXGPU_WARN("Failed to compile morphological AA pass {}: {}", i, compile_error);
        return false;
      }
      swap_mlaa_pipelines_[i] = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_mlaa_pipeline_layout_, spirv.data(), sizeof(uint32_t) * spirv.size());
      if (swap_mlaa_pipelines_[i] == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create morphological AA pass {}", i);
        return false;
      }
    }
    swap_mlaa_failed_ = false;
  }

  if (swap_mlaa_images_[0] != VK_NULL_HANDLE && swap_mlaa_width_ == width &&
      swap_mlaa_height_ == height) {
    return true;
  }
  DestroySwapMorphologicalAaImages();
  // The copy of the image, the edges and the weights.
  const VkFormat formats[3] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_R8G8B8A8_UNORM};
  for (uint32_t i = 0; i < 3; ++i) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = formats[i];
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            swap_mlaa_images_[i], swap_mlaa_memory_[i])) {
      REXGPU_WARN("Failed to create a morphological AA image");
      DestroySwapMorphologicalAaImages();
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = swap_mlaa_images_[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = formats[i];
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &swap_mlaa_views_[i]) != VK_SUCCESS) {
      REXGPU_WARN("Failed to create a morphological AA image view");
      DestroySwapMorphologicalAaImages();
      return false;
    }
  }
  swap_mlaa_width_ = width;
  swap_mlaa_height_ = height;
  return true;
}

void VulkanCommandProcessor::DispatchSwapMorphologicalAa(VkImage target, VkImageView target_view,
                                                         VkImageView frame, VkImageView scene,
                                                         uint32_t width, uint32_t height,
                                                         uint32_t frame_index) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  // The target was just written by the swap's compute passes.
  PushImageMemoryBarrier(target, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  for (VkImage image : swap_mlaa_images_) {
    // The previous frame's use is over; the contents need not stay.
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL);
  }
  SubmitBarriers(true);

  VkDescriptorSet set = swap_mlaa_descriptor_sets_[frame_index];
  const bool hud = frame != VK_NULL_HANDLE && scene != VK_NULL_HANDLE;
  // Without a snapshot (menus) the HUD test is off and both sampled
  // bindings take the frame, which is never read then.
  VkDescriptorImageInfo infos[kMorphologicalAaBindings] = {
      {VK_NULL_HANDLE, target_view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, swap_mlaa_views_[0], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, swap_mlaa_views_[1], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, swap_mlaa_views_[2], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, hud ? scene : frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet writes[kMorphologicalAaBindings] = {};
  for (uint32_t i = 0; i < kMorphologicalAaBindings; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &infos[i];
  }
  dfn.vkUpdateDescriptorSets(device, kMorphologicalAaBindings, writes, 0, nullptr);
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, swap_mlaa_pipeline_layout_, 0, 1, &set, 0, nullptr);
  struct {
    uint32_t size[2];
    uint32_t flags;
    float threshold;
  } constants = {
      {width, height}, hud ? 1u : 0u, float(REXCVAR_GET(swap_morphological_aa_threshold))};
  deferred_command_buffer_.CmdVkPushConstants(
      swap_mlaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  const uint32_t groups_x = (width + 15) / 16, groups_y = (height + 7) / 8;
  for (uint32_t pass = 0; pass < 3; ++pass) {
    if (pass) {
      // Each pass reads what the previous one wrote.
      for (VkImage image : swap_mlaa_images_) {
        PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
      }
      SubmitBarriers(true);
    }
    BindExternalComputePipeline(swap_mlaa_pipelines_[pass]);
    deferred_command_buffer_.CmdVkDispatch(groups_x, groups_y, 1);
  }
}

void VulkanCommandProcessor::DestroySwapMorphologicalAaImages() {
  const uint64_t submission = GetCurrentSubmission();
  for (uint32_t i = 0; i < 3; ++i) {
    if (swap_mlaa_views_[i] != VK_NULL_HANDLE) {
      destroy_image_views_.emplace_back(submission, swap_mlaa_views_[i]);
    }
    if (swap_mlaa_images_[i] != VK_NULL_HANDLE) {
      destroy_images_.emplace_back(submission, swap_mlaa_images_[i]);
    }
    if (swap_mlaa_memory_[i] != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(submission, swap_mlaa_memory_[i]);
    }
    swap_mlaa_views_[i] = VK_NULL_HANDLE;
    swap_mlaa_images_[i] = VK_NULL_HANDLE;
    swap_mlaa_memory_[i] = VK_NULL_HANDLE;
  }
  swap_mlaa_width_ = 0;
  swap_mlaa_height_ = 0;
}

void VulkanCommandProcessor::DestroySwapMorphologicalAa() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DestroySwapMorphologicalAaImages();
  for (VkPipeline& pipeline : swap_mlaa_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_mlaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_mlaa_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_mlaa_set_layout_);
  swap_mlaa_failed_ = false;
}

}  // namespace rex::graphics::vulkan
