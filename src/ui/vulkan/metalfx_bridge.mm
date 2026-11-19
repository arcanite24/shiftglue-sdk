/**
 * @file        ui/vulkan/metalfx_bridge.mm
 * @brief       Metal and MetalFX behind a plain C++ interface
 *
 * @copyright   Copyright (c) 2026 ShiftGlue contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Built with ARC and without the precompiled header (src/ui/CMakeLists.txt).
 * Pointers from the Vulkan half are unretained: each is bridged into a strong
 * reference before use.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include "metalfx_bridge.h"

#include <algorithm>
#include <utility>

namespace rex {
namespace ui {
namespace vulkan {

namespace {

constexpr MTLTextureUsage kOutputUsageFallback =
    MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;

}  // namespace

struct MetalFxBridge::Impl {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLFXTemporalScaler> scaler = nil;
  id<MTLTexture> own_output = nil;
};

MetalFxBridge::MetalFxBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetalFxBridge::~MetalFxBridge() {
  @autoreleasepool {
    impl_.reset();
  }
}

std::unique_ptr<MetalFxBridge> MetalFxBridge::Create(void* mtl_device, std::string& reason_out) {
  @autoreleasepool {
    if (!NSClassFromString(@"MTLFXTemporalScalerDescriptor")) {
      reason_out = "the MetalFX framework is missing";
      return nullptr;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
    if (!device) {
      reason_out = "MoltenVK exported no MTLDevice";
      return nullptr;
    }
    if (![MTLFXTemporalScalerDescriptor supportsDevice:device]) {
      reason_out = "MetalFX temporal scaling is not supported on this GPU";
      return nullptr;
    }
    float min_scale = 1.0f;
    // Without the macOS 14 query, the 1x to 2x MetalFX had from the start.
    float max_scale = 2.0f;
    if (@available(macOS 14.0, iOS 17.0, *)) {
      min_scale = std::max(
          1.0f, [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:device]);
      max_scale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:device];
    }
    if (max_scale < min_scale) {
      reason_out = "MetalFX reports no usable scale range";
      return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    std::unique_ptr<MetalFxBridge> bridge(new MetalFxBridge(std::move(impl)));
    const char* name = [[device name] UTF8String];
    bridge->device_name_ = name ? name : "";
    bridge->min_scale_ = min_scale;
    bridge->max_scale_ = max_scale;
    return bridge;
  }
}

bool MetalFxBridge::has_queue() const { return impl_->queue != nil; }

bool MetalFxBridge::CreateQueue() {
  @autoreleasepool {
    if (impl_->queue) {
      return true;
    }
    impl_->queue = [impl_->device newCommandQueue];
    if (!impl_->queue) {
      return false;
    }
    impl_->queue.label = @"MetalFX upscale";
    return true;
  }
}

void MetalFxBridge::ReleaseResources() {
  @autoreleasepool {
    impl_->scaler = nil;
    impl_->own_output = nil;
    impl_->queue = nil;
  }
}

MetalFxBridge::TextureInfo MetalFxBridge::GetTextureInfo(void* mtl_texture) {
  TextureInfo info;
  id<MTLTexture> texture = (__bridge id<MTLTexture>)mtl_texture;
  if (texture) {
    info.width = uint32_t(texture.width);
    info.height = uint32_t(texture.height);
    info.pixel_format = uint64_t(texture.pixelFormat);
    info.usage = uint64_t(texture.usage);
  }
  return info;
}

uint64_t MetalFxBridge::GetEventValue(void* mtl_shared_event) {
  id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)mtl_shared_event;
  return event ? event.signaledValue : 0;
}

void MetalFxBridge::SetEventValue(void* mtl_shared_event, uint64_t value) {
  id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)mtl_shared_event;
  if (event) {
    event.signaledValue = value;
  }
}

bool MetalFxBridge::has_scaler() const { return impl_->scaler != nil; }

bool MetalFxBridge::CreateScaler(const ScalerDesc& desc, ScalerUsages& usages_out) {
  @autoreleasepool {
    impl_->scaler = nil;
    impl_->own_output = nil;
    MTLFXTemporalScalerDescriptor* descriptor = [[MTLFXTemporalScalerDescriptor alloc] init];
    descriptor.colorTextureFormat = MTLPixelFormat(desc.color_format);
    descriptor.depthTextureFormat = MTLPixelFormat(desc.depth_format);
    descriptor.motionTextureFormat = MTLPixelFormat(desc.motion_format);
    descriptor.outputTextureFormat = MTLPixelFormat(desc.output_format);
    descriptor.inputWidth = desc.input_width;
    descriptor.inputHeight = desc.input_height;
    descriptor.outputWidth = desc.output_width;
    descriptor.outputHeight = desc.output_height;
    descriptor.autoExposureEnabled = NO;
    // The render region of the color texture, set per frame.
    descriptor.inputContentPropertiesEnabled = YES;
    descriptor.inputContentMinScale = min_scale_;
    descriptor.inputContentMaxScale = max_scale_;
    id<MTLFXTemporalScaler> scaler = [descriptor newTemporalScalerWithDevice:impl_->device];
    if (!scaler) {
      return false;
    }
    impl_->scaler = scaler;
    usages_out.color = uint64_t(scaler.colorTextureUsage);
    usages_out.depth = uint64_t(scaler.depthTextureUsage);
    usages_out.motion = uint64_t(scaler.motionTextureUsage);
    usages_out.output = uint64_t(scaler.outputTextureUsage);
    return true;
  }
}

bool MetalFxBridge::EnsureOwnOutput(uint32_t width, uint32_t height, uint64_t pixel_format,
                                    uint64_t usage) {
  @autoreleasepool {
    id<MTLTexture> own_output = impl_->own_output;
    if (own_output && own_output.width == width && own_output.height == height &&
        uint64_t(own_output.pixelFormat) == pixel_format &&
        (uint64_t(own_output.usage) & usage) == usage) {
      return true;
    }
    impl_->own_output = nil;
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(pixel_format)
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsage(usage) | kOutputUsageFallback;
    descriptor.storageMode = MTLStorageModePrivate;
    impl_->own_output = [impl_->device newTextureWithDescriptor:descriptor];
    if (!impl_->own_output) {
      return false;
    }
    impl_->own_output.label = @"MetalFX output";
    return true;
  }
}

bool MetalFxBridge::EncodeAndCommit(const Frame& frame,
                                    std::shared_ptr<std::atomic<bool>> completed,
                                    std::string& error_out) {
  @autoreleasepool {
    id<MTLFXTemporalScaler> scaler = impl_->scaler;
    id<MTLTexture> color = (__bridge id<MTLTexture>)frame.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)frame.depth;
    id<MTLTexture> motion = (__bridge id<MTLTexture>)frame.motion;
    id<MTLTexture> output = (__bridge id<MTLTexture>)frame.output;
    id<MTLSharedEvent> inputs_ready = (__bridge id<MTLSharedEvent>)frame.inputs_ready;
    id<MTLSharedEvent> output_ready = (__bridge id<MTLSharedEvent>)frame.output_ready;
    id<MTLTexture> scaler_output = frame.use_own_output ? impl_->own_output : output;
    if (!impl_->queue || !scaler || !color || !depth || !motion || !output || !inputs_ready ||
        !output_ready || !scaler_output || !completed) {
      error_out = "missing scaler, texture or event";
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [impl_->queue commandBuffer];
    if (!command_buffer) {
      error_out = "could not get a Metal command buffer";
      return false;
    }
    command_buffer.label = @"MetalFX upscale";
    [command_buffer encodeWaitForEvent:inputs_ready value:1];
    scaler.colorTexture = color;
    scaler.depthTexture = depth;
    scaler.motionTexture = motion;
    scaler.outputTexture = scaler_output;
    scaler.inputContentWidth = frame.render_width;
    scaler.inputContentHeight = frame.render_height;
    scaler.jitterOffsetX = frame.jitter_x;
    scaler.jitterOffsetY = frame.jitter_y;
    scaler.motionVectorScaleX = frame.motion_vector_scale_x;
    scaler.motionVectorScaleY = frame.motion_vector_scale_y;
    scaler.reset = frame.reset;
    scaler.depthReversed = frame.depth_reversed;
    scaler.preExposure = 1.0f;
    @try {
      [scaler encodeToCommandBuffer:command_buffer];
      if (scaler_output != output) {
        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
        [blit copyFromTexture:scaler_output
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(frame.output_width, frame.output_height, 1)
                    toTexture:output
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
      }
    } @catch (NSException* exception) {
      // Not committed, so the wait for inputs_ready never reaches the GPU.
      const char* description = [[exception description] UTF8String];
      error_out = description ? description : "an Objective-C exception";
      return false;
    }
    [command_buffer encodeSignalEvent:output_ready value:1];
    // On a Metal thread: only the shared flag.
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      (void)buffer;
      completed->store(true, std::memory_order_release);
    }];
    [command_buffer commit];
    return true;
  }
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
