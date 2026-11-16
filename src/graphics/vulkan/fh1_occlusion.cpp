// FR-3b: an occlusion grid for the title's own culling (Pinyon Shift
// docs/FULL_FRAME_RENDERER.md). When the scene is whole (untiled, at its
// first resolve), its depth surface (guest z with 1 near and 0 far) is
// reduced to tiles that each hold their farthest depth. The grid is copied
// to host memory and published, with that frame's world matrix and depth
// mapping, once the GPU has finished it, so the title sees a grid two or
// three frames old. The title drops a box whose nearest point lies behind
// the farthest depth of every tile it covers. That is a pop-in of a few
// frames for things that come into view, accepted here as performance over
// exactness.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/logging.h>
#include <rex/system/fh1_occlusion_grid.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(fh1_occlusion_grid, false, "GPU",
                    "FR-3b: publish the scene depth of a frame two or three frames back as a "
                    "tile grid for the title's occlusion culling (turns camera tracking on; "
                    "untiled predicated tiling only)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fh1_occlusion_tile, 16, "GPU",
                     "FR-3b: occlusion grid tile size in guest pixels (scaled by the resolution "
                     "scale)")
    .range(8, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fh1_occlusion_debug, 0, "GPU",
                     "FR-3b debug: 1 fills the occlusion grid with each tile's nearest depth, 2 "
                     "with 0.25 (checks the readback); either hides far too much")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(fh1_occlusion_max_drift, 0.1, "GPU",
                      "FR-3b: a finished grid is not published when the view turned more than "
                      "this (largest change of the matrix's normalised x and y rows) since its "
                      "frame")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(fh1_occlusion_dump_dir, "", "GPU",
                      "FR-3b debug: write each published grid with a frame in "
                      "fh1_occlusion_dump_frames to this folder as a PGM image")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(fh1_occlusion_dump_frames, "", "GPU",
                      "FR-3b debug: comma-separated executor frames to dump (the grid's own "
                      "frame, published 2-3 frames later)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

namespace {

// How far the view turned between two world matrices: the largest change of
// the rotation part of the x and y rows, each row normalised by its length.
double MatrixDrift(const float* a, const float* b) {
  double drift = 0.0;
  for (int r = 0; r < 2; ++r) {
    double la = 0.0, lb = 0.0;
    for (int c = 0; c < 3; ++c) {
      la += double(a[r * 4 + c]) * a[r * 4 + c];
      lb += double(b[r * 4 + c]) * b[r * 4 + c];
    }
    la = std::sqrt(la), lb = std::sqrt(lb);
    if (la <= 0.0 || lb <= 0.0) return 1e9;
    for (int c = 0; c < 3; ++c) {
      drift = std::max(drift, std::abs(a[r * 4 + c] / la - b[r * 4 + c] / lb));
    }
  }
  return drift;
}

bool WantDump(uint64_t frame) {
  const std::string frames = REXCVAR_GET(fh1_occlusion_dump_frames);
  if (frames.empty() || REXCVAR_GET(fh1_occlusion_dump_dir).empty()) return false;
  size_t start = 0;
  while (start < frames.size()) {
    size_t end = frames.find(',', start);
    if (end == std::string::npos) end = frames.size();
    if (std::strtoull(frames.substr(start, end - start).c_str(), nullptr, 10) == frame) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

constexpr uint32_t kSlots = 4;
constexpr VkDeviceSize kSlotBytes = rex::system::Fh1OcclusionGrid::kMaxTiles * sizeof(float);

const char* const kOcclusionSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform OcclusionConstants {
  uvec2 size;
  uint tile;
  uint columns;
  uint rows;
  // 1: the tile's nearest depth instead of its farthest; 2: a constant.
  uint mode;
  // Host depth to guest z: float24 depth is kept halved.
  float scale;
} c;
#if MULTISAMPLED
layout(set = 0, binding = 0) uniform texture2DMS depth;
#else
layout(set = 0, binding = 0) uniform texture2D depth;
#endif
layout(set = 0, binding = 1, std430) writeonly buffer Grid { float farthest[]; } grid;
void main() {
  uvec2 t = gl_GlobalInvocationID.xy;
  if (t.x >= c.columns || t.y >= c.rows) {
    return;
  }
  if (c.mode == 2u) {
    grid.farthest[t.y * c.columns + t.x] = 0.25;
    return;
  }
  uvec2 p0 = t * c.tile;
  uvec2 p1 = min(p0 + c.tile, c.size);
  float m = c.mode == 1u ? 0.0 : 1.0;
  for (uint y = p0.y; y < p1.y; ++y) {
    for (uint x = p0.x; x < p1.x; ++x) {
      // Sample 0 of a multisampled surface.
      float z = texelFetch(depth, ivec2(x, y), 0).r * c.scale;
      m = c.mode == 1u ? max(m, z) : min(m, z);
    }
  }
  grid.farthest[t.y * c.columns + t.x] = m;
}
)";

}  // namespace

bool Fh1NativeExecutor::EnsureOcclusionPipeline() {
  if (occlusion_pipelines_[0] != VK_NULL_HANDLE) return true;
  if (occlusion_failed_) return false;
  occlusion_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &occlusion_set_layout_) != VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                             sizeof(uint32_t) * 7};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &occlusion_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &occlusion_pipeline_layout_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kSlots;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &occlusion_descriptor_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  std::array<VkDescriptorSetLayout, kSlots> layouts;
  layouts.fill(occlusion_set_layout_);
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = occlusion_descriptor_pool_;
  set_info.descriptorSetCount = kSlots;
  set_info.pSetLayouts = layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &set_info, occlusion_sets_.data()) != VK_SUCCESS) {
    return false;
  }
  void* mapped = nullptr;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, kSlotBytes * kSlots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, occlusion_buffer_, occlusion_memory_) ||
      dfn.vkMapMemory(device, occlusion_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  occlusion_mapped_ = static_cast<const float*>(mapped);
  for (uint32_t multisampled = 0; multisampled < 2; ++multisampled) {
    std::string source = kOcclusionSource;
    source.insert(source.find('\n') + 1,
                  std::string("#define MULTISAMPLED ") + (multisampled ? "1\n" : "0\n"));
    std::vector<uint32_t> spirv;
    std::string error;
    if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, spirv,
                                               error)) {
      REXGPU_WARN("Failed to compile the occlusion grid shader: {}", error);
      return false;
    }
    occlusion_pipelines_[multisampled] = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, occlusion_pipeline_layout_, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (occlusion_pipelines_[multisampled] == VK_NULL_HANDLE) return false;
  }
  occlusion_failed_ = false;
  return true;
}

void Fh1NativeExecutor::ReduceOcclusionDepth() {
  if (!REXCVAR_GET(fh1_occlusion_grid) || !camera_tracking_enabled_ ||
      !camera_scene_depth_valid_ || occlusion_reduced_valid_) {
    return;
  }
  Surface* surface = FindSurface(camera_scene_depth_);
  if (!surface || !surface->key.is_depth || surface->sampled_view == VK_NULL_HANDLE) {
    Count("occlusion_grid_no_depth");
    return;
  }
  if (occlusion_pending_.size() >= kSlots - 1) {
    Count("occlusion_grid_behind");
    return;
  }
  if (!EnsureOcclusionPipeline()) return;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // The scene is 16:9 (1280x720 at 1x); the surface can hold more rows.
  const uint32_t width = surface->width * scale_;
  const uint32_t height = std::min(width * 9 / 16, surface->image_height * scale_);
  const uint32_t tile = uint32_t(REXCVAR_GET(fh1_occlusion_tile)) * scale_;
  const uint32_t columns = (width + tile - 1) / tile;
  const uint32_t rows = (height + tile - 1) / tile;
  if (columns * rows > rex::system::Fh1OcclusionGrid::kMaxTiles) {
    Count("occlusion_grid_too_many_tiles");
    return;
  }
  const uint32_t slot = occlusion_next_slot_;
  occlusion_next_slot_ = (occlusion_next_slot_ + 1) % kSlots;

  command_processor_.EndRenderPass();
  TransitionForSampling(*surface, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  command_processor_.SubmitBarriers(true);
  const VkDescriptorSet set = occlusion_sets_[slot];
  VkDescriptorImageInfo image = {VK_NULL_HANDLE, surface->sampled_view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo buffer = {occlusion_buffer_, kSlotBytes * slot, kSlotBytes};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (i) {
      writes[i].pBufferInfo = &buffer;
    } else {
      writes[i].pImageInfo = &image;
    }
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                         occlusion_pipeline_layout_, 0, 1, &set, 0, nullptr);
  const float scale = xenos::DepthRenderTargetFormat(surface->key.format) ==
                              xenos::DepthRenderTargetFormat::kD24FS8
                          ? 2.0f
                          : 1.0f;
  uint32_t constants[7] = {width, height, tile, columns, rows,
                           uint32_t(REXCVAR_GET(fh1_occlusion_debug)), 0};
  std::memcpy(&constants[6], &scale, sizeof(scale));
  command_buffer.CmdVkPushConstants(occlusion_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(constants), constants);
  command_processor_.BindExternalComputePipeline(occlusion_pipelines_[surface->samples > 1]);
  command_buffer.CmdVkDispatch((columns + 7) / 8, (rows + 7) / 8, 1);
  VkMemoryBarrier to_host = {};
  to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0,
                                      nullptr);
  occlusion_reduced_ = {};
  occlusion_reduced_.slot = slot;
  occlusion_reduced_.columns = columns;
  occlusion_reduced_.rows = rows;
  occlusion_reduced_valid_ = true;
  Count("occlusion_grid_dispatch");
}

void Fh1NativeExecutor::UpdateOcclusionGrid() {
  if (!REXCVAR_GET(fh1_occlusion_grid)) {
    if (occlusion_published_) rex::system::ClearFh1OcclusionGrid();
    occlusion_published_ = false;
    occlusion_reduced_valid_ = false;
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // This frame's grid, with the camera the frame just finished with.
  if (occlusion_reduced_valid_) {
    occlusion_reduced_valid_ = false;
    // The upscaler's cut flag is not used: the grid carries its own matrix,
    // and the drift check at publishing catches a real change of view.
    if (camera_ready_valid_ && camera_ready_.depth_b != 0.0f) {
      OcclusionPending pending = occlusion_reduced_;
      pending.submission = command_processor_.GetCurrentSubmission();
      pending.frame = frame_;
      pending.wvp = camera_world_wvp_;
      pending.depth_a = camera_ready_.depth_a;
      pending.depth_b = camera_ready_.depth_b;
      occlusion_pending_.push_back(pending);
    }
  }
  // Nothing new for a few swaps (a load, a menu, no world): stop culling.
  if (occlusion_published_ && frame_ > occlusion_published_frame_ + 4) {
    rex::system::ClearFh1OcclusionGrid();
    occlusion_published_ = false;
    Count("occlusion_grid_stale");
  }

  // Publish the newest finished grid.
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  bool have_ready = false;
  OcclusionPending ready = {};
  while (!occlusion_pending_.empty() && occlusion_pending_.front().submission <= completed) {
    ready = occlusion_pending_.front();
    occlusion_pending_.pop_front();
    have_ready = true;
  }
  if (!have_ready) return;
  if (camera_ready_valid_ &&
      MatrixDrift(ready.wvp.data(), camera_world_wvp_.data()) >
          REXCVAR_GET(fh1_occlusion_max_drift)) {
    Count("occlusion_grid_drift");
    return;
  }
  VkMappedMemoryRange range = {};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = occlusion_memory_;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
  static rex::system::Fh1OcclusionGrid grid;
  grid.frame = ready.frame;
  std::memcpy(grid.wvp, ready.wvp.data(), sizeof(grid.wvp));
  grid.depth_a = ready.depth_a;
  grid.depth_b = ready.depth_b;
  grid.columns = ready.columns;
  grid.rows = ready.rows;
  std::memcpy(grid.farthest, occlusion_mapped_ + ready.slot * (kSlotBytes / sizeof(float)),
              size_t(ready.columns) * ready.rows * sizeof(float));
  rex::system::PublishFh1OcclusionGrid(grid);
  occlusion_published_frame_ = frame_;
  if (WantDump(grid.frame)) {
    const std::string path = fmt::format("{}/occlusion-grid-{}.pgm",
                                         REXCVAR_GET(fh1_occlusion_dump_dir), grid.frame);
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fprintf(file, "P5\n%u %u\n255\n", grid.columns, grid.rows);
      for (uint32_t i = 0; i < grid.columns * grid.rows; ++i) {
        // Brighter is nearer; the square root spreads the far range.
        const int v = int(std::sqrt(std::clamp(grid.farthest[i] * 10.0f, 0.0f, 1.0f)) * 255.0f);
        std::fputc(v, file);
      }
      std::fclose(file);
      REXGPU_INFO("FH1 occlusion grid: dumped frame {} to {}", grid.frame, path);
    }
  }
  if ((frame_ % 600) == 0) {
    const size_t n = size_t(grid.columns) * grid.rows;
    std::vector<float> sorted(grid.farthest, grid.farthest + n);
    std::sort(sorted.begin(), sorted.end());
    size_t nonzero = 0;
    for (float v : sorted) nonzero += v > 0.0f;
    REXGPU_INFO(
        "FH1 occlusion grid: frame {} {}x{} nonzero {} min {:.6g} median {:.6g} p90 {:.6g} "
        "max {:.6g} depth {:.6g} + {:.6g} / z",
        grid.frame, grid.columns, grid.rows, nonzero, sorted.front(), sorted[n / 2],
        sorted[n * 9 / 10], sorted.back(), grid.depth_a, grid.depth_b);
  }
  occlusion_published_ = true;
  Count("occlusion_grid_published");
}

void Fh1NativeExecutor::DestroyOcclusion() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  if (!vulkan_device) return;
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  rex::system::ClearFh1OcclusionGrid();
  occlusion_pending_.clear();
  occlusion_reduced_valid_ = false;
  if (occlusion_memory_) {
    if (occlusion_mapped_) dfn.vkUnmapMemory(device, occlusion_memory_);
    occlusion_mapped_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, occlusion_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, occlusion_memory_);
  for (VkPipeline& pipeline : occlusion_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         occlusion_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         occlusion_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         occlusion_set_layout_);
}

}  // namespace rex::graphics::vulkan
