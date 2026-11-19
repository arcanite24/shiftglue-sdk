#pragma once
/**
 * @file        ui/vulkan/metalfx_bridge.h
 * @brief       Metal and MetalFX behind a plain C++ interface
 *
 * @copyright   Copyright (c) 2026 ShiftGlue contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The Objective-C++ half of the MetalFX provider (vulkan_metalfx_upscaler.cpp
 * is the Vulkan half). Vulkan-Hpp does not build under Objective-C++ with
 * ARC, so this header includes neither the rex headers nor Vulkan: Metal
 * objects cross it as void*, the unretained pointers VK_EXT_metal_objects
 * exports (MTLDevice_id and the like outside Objective-C).
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace rex {
namespace ui {
namespace vulkan {

class MetalFxBridge {
 public:
  struct TextureInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    // MTLPixelFormat and MTLTextureUsage.
    uint64_t pixel_format = 0;
    uint64_t usage = 0;
  };

  struct ScalerDesc {
    // The color texture's size; the render size is its content, per frame.
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint64_t color_format = 0;
    uint64_t depth_format = 0;
    uint64_t motion_format = 0;
    uint64_t output_format = 0;
  };

  // The MTLTextureUsage the scaler needs of each texture.
  struct ScalerUsages {
    uint64_t color = 0;
    uint64_t depth = 0;
    uint64_t motion = 0;
    uint64_t output = 0;
  };

  struct Frame {
    // Exported MTLTextures.
    void* color = nullptr;
    void* depth = nullptr;
    void* motion = nullptr;
    void* output = nullptr;
    // Exported MTLSharedEvents, both fresh (at 0).
    void* inputs_ready = nullptr;
    void* output_ready = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float motion_vector_scale_x = 0.0f;
    float motion_vector_scale_y = 0.0f;
    bool reset = true;
    bool depth_reversed = false;
    // Upscale to the bridge's own output texture, then copy to output.
    bool use_own_output = false;
  };

  // Null with the reason when MetalFX cannot run on this MTLDevice.
  static std::unique_ptr<MetalFxBridge> Create(void* mtl_device, std::string& reason_out);
  ~MetalFxBridge();

  MetalFxBridge(const MetalFxBridge&) = delete;
  MetalFxBridge& operator=(const MetalFxBridge&) = delete;

  const std::string& device_name() const { return device_name_; }
  // The upscale factors (output / render content) the device supports.
  float min_scale() const { return min_scale_; }
  float max_scale() const { return max_scale_; }

  bool has_queue() const;
  bool CreateQueue();
  // Drops the queue, the scaler and the own output texture.
  void ReleaseResources();

  static TextureInfo GetTextureInfo(void* mtl_texture);
  static uint64_t GetEventValue(void* mtl_shared_event);
  // From the CPU, for a wait whose Vulkan frame never came.
  static void SetEventValue(void* mtl_shared_event, uint64_t value);

  bool has_scaler() const;
  // Replaces the scaler; false (and none) if MetalFX refused the description.
  bool CreateScaler(const ScalerDesc& desc, ScalerUsages& usages_out);
  // A texture of the bridge's own for an output that lacks the usages.
  bool EnsureOwnOutput(uint32_t width, uint32_t height, uint64_t pixel_format, uint64_t usage);

  // On the bridge's queue: wait for inputs_ready at 1, upscale (and copy
  // from the own output), signal output_ready at 1, commit. completed turns
  // true on a Metal thread when the command buffer finishes. False, with
  // nothing committed, if the command buffer or the encoding failed.
  bool EncodeAndCommit(const Frame& frame, std::shared_ptr<std::atomic<bool>> completed,
                       std::string& error_out);

 private:
  struct Impl;
  explicit MetalFxBridge(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
  std::string device_name_;
  float min_scale_ = 1.0f;
  float max_scale_ = 2.0f;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
