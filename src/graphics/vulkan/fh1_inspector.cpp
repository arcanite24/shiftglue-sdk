#include <rex/graphics/vulkan/fh1_native_executor.h>

#include <algorithm>
#include <cstring>
#include <rex/cvar.h>
#include <rex/hash.h>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#if !defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(fh1_inspector_capture, false, "GPU",
                    "Capture the next complete frame for the desktop inspector; resets when armed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace rex::graphics::vulkan {

void Fh1NativeExecutor::BeginInspection() {
#if !defined(__ANDROID__)
  if (!initialized_ || inspection_)
    return;
  auto mailbox = command_processor_.frame_inspector();
  if (REXCVAR_GET(fh1_inspector_capture)) {
    rex::cvar::SetFlagByName("fh1_inspector_capture", "false");
    mailbox->Request(Fh1InspectionView::kColor);
  }
  Fh1InspectionView view;
  if (!mailbox->Begin(view, inspection_generation_))
    return;
  inspection_ = std::make_shared<Fh1FrameInspection>();
  inspection_->view = view;
  inspection_draws_.clear();
  inspection_textures_.clear();
  command_processor_.SetInspectionView(view);
#endif
}

void Fh1NativeExecutor::InspectTextures(uint32_t mask) {
  if (inspection_ && inspection_textures_.size() < 512) {
    config_.textures->InspectActiveTextures(mask, inspection_textures_);
    inspection_->truncated |= inspection_textures_.size() >= 512;
  }
}

Fh1InspectionImage Fh1NativeExecutor::QueueInspectionImage(Surface& surface, Rect rect) {
  Fh1InspectionImage image;
  rect.left = std::clamp(rect.left, 0, int32_t(surface.width));
  rect.top = std::clamp(rect.top, 0, int32_t(surface.image_height));
  rect.right = std::clamp(rect.right, rect.left, int32_t(surface.width));
  rect.bottom = std::clamp(rect.bottom, rect.top, int32_t(surface.image_height));
  if (rect.left == rect.right || rect.top == rect.bottom) {
    image.error = "Empty source rectangle";
    return image;
  }
  if (inspection_readbacks_.size() >= 256) {
    inspection_->truncated = true;
    image.error = "Capture limit reached (256 previews)";
    return image;
  }
  const auto* gpu = command_processor_.GetVulkanDevice();
  const auto& fn = gpu->functions();
  const VkDevice device = gpu->device();
  const bool msaa = surface.samples > 1;
  if (!inspection_set_layout_) {
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (fn.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &inspection_set_layout_) !=
        VK_SUCCESS) {
      image.error = "Cannot allocate preview descriptor layout";
      return image;
    }
  }
  if (!inspection_pipeline_layout_) {
    const VkPushConstantRange constants = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &inspection_set_layout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    if (fn.vkCreatePipelineLayout(device, &info, nullptr, &inspection_pipeline_layout_) !=
        VK_SUCCESS) {
      image.error = "Cannot allocate preview pipeline layout";
      return image;
    }
  }
  if (!inspection_pipelines_[msaa]) {
    // Sample the typed attachment view, including float formats' float view.
    // The preview is diagnostic: nearest sample zero, clamped RGB or raw depth.
    std::string source =
        "#version 450\n#extension GL_EXT_samplerless_texture_functions : require\n";
    source += msaa ? "layout(set=0,binding=0) uniform texture2DMS src;\n"
                   : "layout(set=0,binding=0) uniform texture2D src;\n";
    source += R"(
layout(set=0,binding=1,std430) writeonly buffer Output { uint pixels[]; };
layout(push_constant) uniform Params { uvec4 rect; uvec4 size; } p;
layout(local_size_x=8,local_size_y=8) in;
void main() {
  uvec2 xy = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(xy,p.size.xy))) return;
  ivec2 at = ivec2(p.rect.xy + min((xy * p.rect.zw) / p.size.xy,p.rect.zw-1u));
  vec4 color = texelFetch(src,at,0);
  if (p.size.z != 0u) color.rgb = vec3(color.r * (p.size.w != 0u ? 2.0 : 1.0));
  pixels[xy.y*p.size.x+xy.x] = packUnorm4x8(vec4(clamp(color.rgb,0.0,1.0),1.0));
})";
    std::vector<uint32_t> spirv;
    std::string error;
    if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, spirv, error)) {
      image.error = "Preview shader: " + error;
      return image;
    }
    inspection_pipelines_[msaa] = ui::vulkan::util::CreateComputePipeline(
        gpu, inspection_pipeline_layout_, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (!inspection_pipelines_[msaa]) {
      image.error = "Cannot create preview pipeline";
      return image;
    }
  }
  const uint32_t width = uint32_t(rect.right - rect.left) * scale_x_;
  const uint32_t height = uint32_t(rect.bottom - rect.top) * scale_y_;
  const uint32_t longest = std::max(width, height);
  image.width = std::max(1u, width * std::min(longest, 256u) / longest);
  image.height = std::max(1u, height * std::min(longest, 256u) / longest);
  const VkDeviceSize bytes = image.width * image.height * 4u;
  InspectionReadback readback = {};
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          gpu, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback.buffer, readback.memory)) {
    image.error = "Cannot allocate preview readback";
    return image;
  }
  const VkDescriptorSet set = AllocateDescriptorSet(inspection_set_layout_);
  if (!set) {
    fn.vkDestroyBuffer(device, readback.buffer, nullptr);
    fn.vkFreeMemory(device, readback.memory, nullptr);
    image.error = "Cannot allocate preview descriptors";
    return image;
  }
  FlushPendingClears(surface);
  TransitionForSampling(surface, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  command_processor_.SubmitBarriers(true);
  const VkDescriptorImageInfo sampled = {VK_NULL_HANDLE,
                                         surface.key.is_depth ? surface.sampled_view : surface.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorBufferInfo buffer = {readback.buffer, 0, bytes};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &sampled;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &buffer;
  fn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  const uint32_t constants[] = {
      uint32_t(rect.left) * scale_x_,
      uint32_t(rect.top) * scale_y_,
      width,
      height,
      image.width,
      image.height,
      uint32_t(surface.key.is_depth),
      uint32_t(surface.key.is_depth &&
               surface.key.format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8))};
  auto& commands = command_processor_.deferred_command_buffer();
  command_processor_.BindExternalComputePipeline(inspection_pipelines_[msaa]);
  commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, inspection_pipeline_layout_, 0,
                                   1, &set, 0, nullptr);
  commands.CmdVkPushConstants(inspection_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(constants), constants);
  commands.CmdVkDispatch((image.width + 7) / 8, (image.height + 7) / 8, 1);
  command_processor_.PushBufferMemoryBarrier(
      readback.buffer, 0, bytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  inspection_readbacks_.push_back(readback);
  return image;
}

void Fh1NativeExecutor::InspectResolve(const CopyPlan& plan, uint32_t untile_band) {
  for (const auto& source : plan.sources) {
    if (!source.surface)
      continue;
    if (inspection_->resolves.size() >= 128) {
      inspection_->truncated = true;
      break;
    }
    Fh1InspectionResolve resolve;
    resolve.address = plan.info.copy_dest_extent_start;
    resolve.length = plan.info.copy_dest_extent_length;
    resolve.source_key = source.surface->key.Pack();
    resolve.left = source.rect.left;
    resolve.top = source.rect.top;
    resolve.right = source.rect.right;
    resolve.bottom = source.rect.bottom;
    if (source.surface->key.Pack() != plan.resolve_key.Pack()) {
      resolve.image.error = "Resolve aliases another EDRAM layout; inspect its source surface";
    } else {
      Rect sample = source.rect;
      sample.top += int32_t(untile_band);
      sample.bottom += int32_t(untile_band);
      resolve.image = QueueInspectionImage(*source.surface, sample);
    }
    inspection_->resolves.push_back(std::move(resolve));
  }
}

void Fh1NativeExecutor::FinishInspection(uint64_t frame) {
  inspection_->frame = frame;
  for (auto& [key, surface] : surfaces_) {
    Fh1InspectionSurface item;
    item.key = key;
    item.base = surface.key.base_tiles;
    item.pitch = surface.key.pitch_tiles;
    item.format = surface.key.format;
    item.depth = surface.key.is_depth;
    item.format_name =
        item.depth
            ? xenos::GetDepthRenderTargetFormatName(xenos::DepthRenderTargetFormat(item.format))
            : xenos::GetColorRenderTargetFormatName(xenos::ColorRenderTargetFormat(item.format));
    item.width = surface.width * scale_x_;
    item.height = surface.image_height * scale_y_;
    item.samples = surface.samples;
    item.draws = inspection_draws_[key];
    item.image = QueueInspectionImage(
        surface, {0, 0, int32_t(surface.width), int32_t(surface.image_height)});
    inspection_->surfaces.push_back(std::move(item));
  }
  command_processor_.SubmitBarriers(true);
  command_processor_.Fh1AwaitAllQueueOperations();
  const auto* gpu = command_processor_.GetVulkanDevice();
  const auto& fn = gpu->functions();
  size_t read_index = 0;
  auto read_image = [&](Fh1InspectionImage& image) {
    if (!image.error.empty())
      return;
    auto& readback = inspection_readbacks_[read_index++];
    void* data = nullptr;
    if (fn.vkMapMemory(gpu->device(), readback.memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = readback.memory;
      range.size = VK_WHOLE_SIZE;
      if (fn.vkInvalidateMappedMemoryRanges(gpu->device(), 1, &range) == VK_SUCCESS) {
        image.rgba.resize(image.width * image.height * 4u);
        std::memcpy(image.rgba.data(), data, image.rgba.size());
      } else {
        image.error = "Cannot invalidate preview readback";
      }
      fn.vkUnmapMemory(gpu->device(), readback.memory);
    } else {
      image.error = "Cannot map preview readback";
    }
    fn.vkDestroyBuffer(gpu->device(), readback.buffer, nullptr);
    fn.vkFreeMemory(gpu->device(), readback.memory, nullptr);
  };
  for (auto& resolve : inspection_->resolves)
    read_image(resolve.image);
  for (auto& surface : inspection_->surfaces)
    read_image(surface.image);
  inspection_readbacks_.clear();
  for (const auto& [key, texture] : inspection_textures_)
    inspection_->textures.push_back(texture);
  command_processor_.SetInspectionView(Fh1InspectionView::kColor);
  REXGPU_INFO("FH1 inspector: frame {} surfaces={} resolves={} textures={}", frame,
              inspection_->surfaces.size(), inspection_->resolves.size(),
              inspection_->textures.size());
  for (const auto& surface : inspection_->surfaces) {
    REXGPU_INFO("FH1 inspector surface {:08X}: {}x{} draws={} preview={:016X} {}", surface.key,
                surface.width, surface.height, surface.draws,
                XXH3_64bits(surface.image.rgba.data(), surface.image.rgba.size()),
                surface.image.error);
  }
  command_processor_.frame_inspector()->Publish(std::move(inspection_), inspection_generation_);
}

void Fh1NativeExecutor::DestroyInspector() {
  const auto* gpu = command_processor_.GetVulkanDevice();
  const auto& fn = gpu->functions();
  for (auto& readback : inspection_readbacks_) {
    fn.vkDestroyBuffer(gpu->device(), readback.buffer, nullptr);
    fn.vkFreeMemory(gpu->device(), readback.memory, nullptr);
  }
  inspection_readbacks_.clear();
  inspection_.reset();
  command_processor_.SetInspectionView(Fh1InspectionView::kColor);
  command_processor_.frame_inspector()->Clear();
  command_processor_.frame_inspector()->SetCapabilities({});
  for (auto& pipeline : inspection_pipelines_) {
    if (pipeline)
      fn.vkDestroyPipeline(gpu->device(), pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  if (inspection_pipeline_layout_)
    fn.vkDestroyPipelineLayout(gpu->device(), inspection_pipeline_layout_, nullptr);
  if (inspection_set_layout_)
    fn.vkDestroyDescriptorSetLayout(gpu->device(), inspection_set_layout_, nullptr);
  inspection_pipeline_layout_ = VK_NULL_HANDLE;
  inspection_set_layout_ = VK_NULL_HANDLE;
}
}  // namespace rex::graphics::vulkan
