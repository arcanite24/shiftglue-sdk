/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                   *
 ******************************************************************************
 * MetalFX frame interpolation (MTLFXFrameInterpolator, macOS 26, Apple
 * silicon), reached from the Vulkan presenter on MoltenVK.
 *
 * The presenter's history and output images are MoltenVK device-local images
 * created with VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT; this backend
 * resolves each to its MTLTexture with vkExportMetalObjectsEXT and runs the
 * interpolator on the Metal command queue MoltenVK exports from the Vulkan
 * graphics/compute queue. Because that is the same queue the presenter submits
 * its Vulkan work on, and MoltenVK keeps synchronous submits on, the work is
 * ordered without a cross-API semaphore: the presenter submits the history
 * copy, then this backend's Metal command buffer runs after it and before the
 * paint that samples the output.
 *
 * The M1 probe (docs/FRAME_GENERATION.md) showed MetalFX interpolating from
 * the colour pair alone, running its own optical flow, so no depth or motion
 * texture is supplied.
 *
 * This translation unit is compiled WITHOUT Objective-C ARC (see the CMake
 * entry). With ARC on and VK_USE_PLATFORM_METAL_EXT defined, vulkan.hpp's
 * CppType<VkMetalSurfaceCreateInfoEXT> holds a CAMetalLayer* __strong member,
 * which makes a default template argument non-constant and fails to compile.
 * The Metal objects kept across calls are retained and released by hand.
 */

#include <rex/ui/vulkan/metalfx_frame_generator.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

namespace rex {
namespace ui {
namespace vulkan {

namespace {

// Exports one Metal object of the requested type from the device. Returns nil
// when the extension is absent or MoltenVK did not fill the field.
id ExportMetalObject(const VulkanDevice* vulkan_device,
                     VkExportMetalObjectTypeFlagBitsEXT type, VkImage image, VkImageView image_view,
                     VkQueue queue, VkSemaphore semaphore) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  if (!dfn.vkExportMetalObjectsEXT) {
    return nil;
  }
  VkExportMetalObjectsInfoEXT objects_info;
  objects_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
  objects_info.pNext = nullptr;
  // One typed info chained in; MoltenVK fills the object field in place.
  union {
    VkExportMetalDeviceInfoEXT device_info;
    VkExportMetalCommandQueueInfoEXT queue_info;
    VkExportMetalTextureInfoEXT texture_info;
    VkExportMetalSharedEventInfoEXT shared_event_info;
  } info;
  switch (type) {
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT:
      info.device_info = {VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT, nullptr, nil};
      objects_info.pNext = &info.device_info;
      break;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT:
      info.queue_info = {VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT, nullptr, queue, nil};
      objects_info.pNext = &info.queue_info;
      break;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT:
      info.texture_info = {VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
                           nullptr,
                           image,
                           image_view,
                           VK_NULL_HANDLE,
                           VK_IMAGE_ASPECT_PLANE_0_BIT,
                           nil};
      objects_info.pNext = &info.texture_info;
      break;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT:
      info.shared_event_info = {VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT, nullptr,
                                semaphore, VK_NULL_HANDLE, nil};
      objects_info.pNext = &info.shared_event_info;
      break;
    default:
      return nil;
  }
  dfn.vkExportMetalObjectsEXT(vulkan_device->device(), &objects_info);
  switch (type) {
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT:
      return info.device_info.mtlDevice;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT:
      return info.queue_info.mtlCommandQueue;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT:
      return info.texture_info.mtlTexture;
    case VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT:
      return info.shared_event_info.mtlSharedEvent;
    default:
      return nil;
  }
}

class API_AVAILABLE(macos(26.0)) MetalFxFrameGenerator final : public VulkanFrameGenerator {
 public:
  explicit MetalFxFrameGenerator(const VulkanDevice* vulkan_device)
      : vulkan_device_(vulkan_device) {}

  ~MetalFxFrameGenerator() override {
    for (auto& entry : texture_cache_) {
      [entry.second release];
    }
    [interpolator_ release];
    [queue_ release];
    [device_ release];
  }

  FrameGenerationBackend backend() const override { return FrameGenerationBackend::kMetalFx; }
  bool UsesExternalQueue() const override { return true; }

  bool Initialize() {
    device_ = [(id<MTLDevice>)ExportMetalObject(
        vulkan_device_, VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT, VK_NULL_HANDLE,
        VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE) retain];
    if (!device_) {
      REXLOG_ERROR("MetalFX: MoltenVK did not export an MTLDevice");
      return false;
    }
    if (![MTLFXFrameInterpolatorDescriptor supportsDevice:device_]) {
      REXLOG_ERROR("MetalFX: the exported device does not support frame interpolation");
      return false;
    }
    const VkQueue queue =
        vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0).queue();
    queue_ = [(id<MTLCommandQueue>)ExportMetalObject(
        vulkan_device_, VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT, VK_NULL_HANDLE,
        VK_NULL_HANDLE, queue, VK_NULL_HANDLE) retain];
    if (!queue_) {
      REXLOG_ERROR("MetalFX: MoltenVK did not export the graphics queue's MTLCommandQueue");
      return false;
    }
    return true;
  }

  bool Generate(const VulkanFrameGenerationInputs& inputs) override {
    if (!EnsureInterpolator(inputs.extent.width, inputs.extent.height)) {
      return false;
    }
    id<MTLTexture> previous = ResolveTexture(inputs.previous, inputs.previous_view);
    id<MTLTexture> current = ResolveTexture(inputs.current, inputs.current_view);
    id<MTLTexture> output = ResolveTexture(inputs.output, inputs.output_view);
    if (!previous || !current || !output) {
      REXLOG_ERROR("MetalFX: failed to resolve the frame textures to Metal");
      return false;
    }
    interpolator_.colorTexture = current;
    interpolator_.prevColorTexture = previous;
    interpolator_.outputTexture = output;
    interpolator_.depthTexture = nil;
    interpolator_.motionTexture = nil;
    interpolator_.deltaTime = inputs.delta_s > 0.0f ? inputs.delta_s : (1.0f / 60.0f);
    interpolator_.shouldResetHistory = inputs.reset_history;
    interpolator_.motionVectorScaleX = 1.0f;
    interpolator_.motionVectorScaleY = 1.0f;

    id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
    if (!command_buffer) {
      REXLOG_ERROR("MetalFX: no Metal command buffer");
      return false;
    }
    [interpolator_ encodeToCommandBuffer:command_buffer];
    __block uint64_t gpu_ns = 0;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      const double seconds = cb.GPUEndTime - cb.GPUStartTime;
      if (seconds > 0.0) {
        gpu_ns = uint64_t(seconds * 1e9);
      }
    }];
    [command_buffer commit];
    // Order the paint after the interpolation. MoltenVK's synchronous submits
    // make the presenter's following Vulkan paint submission a fresh Metal
    // command buffer on the same queue, committed after this one; waiting here
    // is the simple, correct ordering until the asynchronous handoff is
    // measured (the queue_wait/queue_signal fields reserve it).
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      REXLOG_ERROR("MetalFX: command buffer status {}", int(command_buffer.status));
      return false;
    }
    last_gpu_time_ns_ = gpu_ns;
    return true;
  }

  uint64_t TakeLastGpuTimeNs() override {
    uint64_t v = last_gpu_time_ns_;
    last_gpu_time_ns_ = 0;
    return v;
  }

 private:
  bool EnsureInterpolator(uint32_t width, uint32_t height) {
    if (interpolator_ && width_ == width && height_ == height) {
      return true;
    }
    [interpolator_ release];
    interpolator_ = nil;
    for (auto& entry : texture_cache_) {
      [entry.second release];
    }
    texture_cache_.clear();
    MTLFXFrameInterpolatorDescriptor* descriptor = [MTLFXFrameInterpolatorDescriptor new];
    descriptor.colorTextureFormat = kColorFormat;
    descriptor.outputTextureFormat = kColorFormat;
    descriptor.depthTextureFormat = MTLPixelFormatInvalid;
    descriptor.motionTextureFormat = MTLPixelFormatInvalid;
    descriptor.uiTextureFormat = MTLPixelFormatInvalid;
    descriptor.inputWidth = width;
    descriptor.inputHeight = height;
    descriptor.outputWidth = width;
    descriptor.outputHeight = height;
    interpolator_ = [[descriptor newFrameInterpolatorWithDevice:device_] retain];
    if (!interpolator_) {
      REXLOG_ERROR("MetalFX: failed to create the interpolator at {}x{}", width, height);
      return false;
    }
    width_ = width;
    height_ = height;
    return true;
  }

  id<MTLTexture> ResolveTexture(VkImage image, VkImageView image_view) {
    if (image == VK_NULL_HANDLE) {
      return nil;
    }
    auto it = texture_cache_.find(image);
    if (it != texture_cache_.end()) {
      return it->second;
    }
    id<MTLTexture> texture = (id<MTLTexture>)ExportMetalObject(
        vulkan_device_, VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT, image, image_view,
        VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (texture) {
      [texture retain];
      texture_cache_.emplace(image, texture);
    }
    return texture;
  }

  static constexpr MTLPixelFormat kColorFormat = MTLPixelFormatRGB10A2Unorm;

  const VulkanDevice* vulkan_device_;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLFXFrameInterpolator> interpolator_ = nil;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint64_t last_gpu_time_ns_ = 0;
  // VkImage (device-local, exportable, never moved) to its exported texture.
  std::unordered_map<VkImage, id<MTLTexture>> texture_cache_;
};

}  // namespace

FrameGenerationAvailability ProbeMetalFxFrameGenerator(const VulkanDevice* vulkan_device) {
  if (@available(macOS 26.0, *)) {
    if (!vulkan_device) {
      return {false, "no Vulkan device"};
    }
    if (!vulkan_device->extensions().ext_EXT_metal_objects ||
        !vulkan_device->functions().vkExportMetalObjectsEXT) {
      return {false, "MoltenVK does not offer VK_EXT_metal_objects here"};
    }
    id<MTLDevice> device = (id<MTLDevice>)ExportMetalObject(
        vulkan_device, VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT, VK_NULL_HANDLE,
        VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (!device) {
      return {false, "MoltenVK did not export an MTLDevice"};
    }
    if (![MTLFXFrameInterpolatorDescriptor supportsDevice:device]) {
      return {false, "this GPU does not support MetalFX frame interpolation"};
    }
    return {true, ""};
  }
  return {false, "MetalFX frame interpolation needs macOS 26"};
}

std::unique_ptr<VulkanFrameGenerator> CreateMetalFxFrameGenerator(
    const VulkanDevice* vulkan_device) {
  if (@available(macOS 26.0, *)) {
    if (!ProbeMetalFxFrameGenerator(vulkan_device).available) {
      return nullptr;
    }
    auto generator = std::make_unique<MetalFxFrameGenerator>(vulkan_device);
    if (!generator->Initialize()) {
      return nullptr;
    }
    return generator;
  }
  return nullptr;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
