/**
 * @file        ui/vulkan/vulkan_metalfx_upscaler.cpp
 * @brief       Apple MetalFX temporal upscaler on MoltenVK
 *
 * @copyright   Copyright (c) 2026 ShiftGlue contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * MetalFX runs on a Metal queue of its own, on the Metal textures under the
 * presenter's Vulkan images (VK_EXT_metal_objects). A pair of fresh VkEvents
 * backed by MTLSharedEvents orders the two queues each frame: Vulkan sets
 * inputs_ready, Metal waits for it, upscales and signals output_ready, which
 * the Vulkan command buffer waits for. MoltenVK encodes a set as the event's
 * value + 1 and a wait for the value + 1, both at encode time, so an event
 * is never reused: a new one is at 0, and both sides use the value 1.
 *
 * This is the Vulkan half, plain C++: Vulkan-Hpp does not build under
 * Objective-C++ with ARC. The Metal work is in metalfx_bridge.mm; Metal
 * objects reach it as the void* that vulkan_metal.h exports outside
 * Objective-C.
 */

#include <rex/ui/vulkan/upscaler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include "metalfx_bridge.h"

REXCVAR_DEFINE_INT32(present_metalfx_jitter_sign, 0, "UI/Presenter",
                     "Debug: negate the jitter MetalFX is given, 1 for x, 2 for y, 3 for both")
    .range(0, 3);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

// Pairs whose Metal command buffer is still pending; past this, Dispatch
// declines rather than let [MTLCommandQueue commandBuffer] block at 64.
constexpr size_t kMaxPendingPairs = 16;
// Dispatches after a pair before its events may go: more than the frames
// the presenter keeps in flight, so its Vulkan command buffer is done too.
constexpr uint64_t kPairRetireDispatches = 8;
// Release's bound on each wait for the Metal command buffers.
constexpr auto kReleaseWait = std::chrono::seconds(2);

// One warning per kind of failure, as these repeat every frame.
#define METALFX_WARN_ONCE(warned, ...) \
  do {                                 \
    if (!(warned)) {                   \
      (warned) = true;                 \
      REXLOG_WARN(__VA_ARGS__);        \
    }                                  \
  } while (0)

class VulkanMetalFxUpscaler final : public VulkanUpscaler {
 public:
  explicit VulkanMetalFxUpscaler(const VulkanDevice* device) : device_(device) {
    info_.id = "metalfx";
    info_.name = "Apple MetalFX";
    info_.kind = UpscalerKind::kTemporal;
    info_.mode_mask = 1u << uint32_t(UpscalerMode::kAuto);
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    info_.uses_jitter = true;
    // The proc address is null unless VK_EXT_metal_objects is enabled.
    const VulkanInstance* instance = device->vulkan_instance();
    const PFN_vkGetDeviceProcAddr get_device_proc_addr =
        instance->functions().vkGetDeviceProcAddr;
    const VkDevice vk_device = device->device();
    export_metal_objects_ = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        get_device_proc_addr(vk_device, "vkExportMetalObjectsEXT"));
    if (!export_metal_objects_) {
      info_.unavailable_reason = "VK_EXT_metal_objects is not enabled (vkExportMetalObjectsEXT)";
      return;
    }
    create_event_ =
        reinterpret_cast<PFN_vkCreateEvent>(get_device_proc_addr(vk_device, "vkCreateEvent"));
    destroy_event_ =
        reinterpret_cast<PFN_vkDestroyEvent>(get_device_proc_addr(vk_device, "vkDestroyEvent"));
    cmd_set_event_ =
        reinterpret_cast<PFN_vkCmdSetEvent>(get_device_proc_addr(vk_device, "vkCmdSetEvent"));
    cmd_wait_events_ =
        reinterpret_cast<PFN_vkCmdWaitEvents>(get_device_proc_addr(vk_device, "vkCmdWaitEvents"));
    if (!create_event_ || !destroy_event_ || !cmd_set_event_ || !cmd_wait_events_) {
      info_.unavailable_reason = "the device lacks the Vulkan event entry points";
      return;
    }
    VkExportMetalDeviceInfoEXT device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
    VkExportMetalObjectsInfoEXT objects_info = {};
    objects_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
    objects_info.pNext = &device_info;
    export_metal_objects_(vk_device, &objects_info);
    bridge_ = MetalFxBridge::Create(device_info.mtlDevice, info_.unavailable_reason);
    if (!bridge_) {
      return;
    }
    min_scale_ = bridge_->min_scale();
    max_scale_ = bridge_->max_scale();
    for (uint32_t mode = 0; mode < uint32_t(UpscalerMode::kCount); ++mode) {
      const float ratio = GetUpscalerModeRatio(UpscalerMode(mode));
      if (UpscalerMode(mode) == UpscalerMode::kAuto ||
          (ratio >= min_scale_ - 0.01f && ratio <= max_scale_ + 0.01f)) {
        info_.mode_mask |= 1u << mode;
      }
    }
    info_.available = true;
    REXLOG_INFO("MetalFX: temporal scaling available on {}, scale {:.2f} to {:.2f}",
                bridge_->device_name(), min_scale_, max_scale_);
  }

  ~VulkanMetalFxUpscaler() override { Release(); }

  const UpscalerInfo& info() const override { return info_; }

  bool Prepare(uint32_t max_render_width, uint32_t max_render_height, uint32_t max_output_width,
               uint32_t max_output_height, bool hdr) override {
    // SDR only for now: the guest output is display-referred at exposure 1.
    if (!info_.available || hdr) {
      return false;
    }
    if (!max_render_width || !max_render_height || !max_output_width || !max_output_height) {
      return false;
    }
    if (!bridge_->has_queue() && !bridge_->CreateQueue()) {
      REXLOG_ERROR("MetalFX: could not create a Metal command queue");
      return false;
    }
    // The scaler follows the frame's sizes in Dispatch.
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    // Everything that can fail comes before anything is recorded or
    // committed: the Vulkan commands at the end cannot fail, and the Metal
    // command buffer, once committed, always signals output_ready.
    if (!info_.available || !bridge_->has_queue() || !inputs.color.image ||
        !inputs.depth.image || !inputs.motion_vectors.image || !inputs.output.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    if (!frame.render_width || !frame.render_height || !frame.output_width ||
        !frame.output_height) {
      return false;
    }
    RetirePairs();
    if (pending_.size() >= kMaxPendingPairs) {
      METALFX_WARN_ONCE(warned_pending_, "MetalFX: {} upscales still pending on the Metal queue",
                        pending_.size());
      return false;
    }

    void* textures[4] = {};
    if (!ExportTextures(inputs, textures)) {
      METALFX_WARN_ONCE(warned_export_, "MetalFX: MoltenVK exported no MTLTexture for an input");
      return false;
    }
    const MetalFxBridge::TextureInfo color = MetalFxBridge::GetTextureInfo(textures[0]);
    const MetalFxBridge::TextureInfo depth = MetalFxBridge::GetTextureInfo(textures[1]);
    const MetalFxBridge::TextureInfo motion = MetalFxBridge::GetTextureInfo(textures[2]);
    const MetalFxBridge::TextureInfo output = MetalFxBridge::GetTextureInfo(textures[3]);
    if (frame.render_width > color.width || frame.render_height > color.height ||
        depth.width != color.width || depth.height != color.height ||
        motion.width != color.width || motion.height != color.height ||
        frame.output_width > output.width || frame.output_height > output.height) {
      METALFX_WARN_ONCE(warned_sizes_,
                        "MetalFX: input sizes do not match (render {}x{}, color {}x{}, depth "
                        "{}x{}, motion {}x{}, output {}x{} in {}x{})",
                        frame.render_width, frame.render_height, color.width, color.height,
                        depth.width, depth.height, motion.width, motion.height,
                        frame.output_width, frame.output_height, output.width, output.height);
      return false;
    }
    const float scale_x = float(frame.output_width) / float(frame.render_width);
    const float scale_y = float(frame.output_height) / float(frame.render_height);
    if (std::min(scale_x, scale_y) < min_scale_ - 0.01f ||
        std::max(scale_x, scale_y) > max_scale_ + 0.01f) {
      METALFX_WARN_ONCE(warned_scale_, "MetalFX: scale {:.2f}x{:.2f} outside {:.2f} to {:.2f}",
                        scale_x, scale_y, min_scale_, max_scale_);
      return false;
    }

    bool recreated = false;
    if (!EnsureScaler(frame, color, depth, motion, output, recreated)) {
      return false;
    }
    if (!InputUsageFits(color, usages_.color, "color", warned_color_usage_) ||
        !InputUsageFits(depth, usages_.depth, "depth", warned_depth_usage_) ||
        !InputUsageFits(motion, usages_.motion, "motion", warned_motion_usage_)) {
      return false;
    }
    // An output without the usages MetalFX writes with gets a copy of a
    // texture of its own.
    const bool use_own_output = (output.usage & usages_.output) != usages_.output;
    if (use_own_output) {
      if (!warned_output_usage_) {
        warned_output_usage_ = true;
        REXLOG_WARN(
            "MetalFX: the output texture has usage {} without {} that MetalFX needs; "
            "upscaling to a texture of its own and copying",
            output.usage, usages_.output & ~output.usage);
      }
      if (!bridge_->EnsureOwnOutput(frame.output_width, frame.output_height, output.pixel_format,
                                    usages_.output)) {
        METALFX_WARN_ONCE(warned_own_output_,
                          "MetalFX: could not create the {}x{} output texture",
                          frame.output_width, frame.output_height);
        return false;
      }
    }

    EventPair pair;
    if (!CreateEventPair(pair)) {
      METALFX_WARN_ONCE(warned_events_, "MetalFX: could not create the shared event pair");
      return false;
    }

    const int32_t jitter_sign = REXCVAR_GET(present_metalfx_jitter_sign);
    MetalFxBridge::Frame bridge_frame;
    bridge_frame.color = textures[0];
    bridge_frame.depth = textures[1];
    bridge_frame.motion = textures[2];
    bridge_frame.output = textures[3];
    bridge_frame.inputs_ready = pair.inputs_event;
    bridge_frame.output_ready = pair.output_event;
    bridge_frame.render_width = frame.render_width;
    bridge_frame.render_height = frame.render_height;
    bridge_frame.output_width = frame.output_width;
    bridge_frame.output_height = frame.output_height;
    bridge_frame.jitter_x = (jitter_sign & 1) ? -frame.jitter_x : frame.jitter_x;
    bridge_frame.jitter_y = (jitter_sign & 2) ? -frame.jitter_y : frame.jitter_y;
    bridge_frame.motion_vector_scale_x = frame.motion_vector_scale_x;
    bridge_frame.motion_vector_scale_y = frame.motion_vector_scale_y;
    bridge_frame.reset = frame.reset || recreated;
    bridge_frame.depth_reversed = frame.depth_inverted;
    bridge_frame.use_own_output = use_own_output;
    std::string error;
    if (!bridge_->EncodeAndCommit(bridge_frame, pair.completed, error)) {
      // Nothing committed, so the wait for inputs_ready never reaches the GPU.
      DestroyEventPair(pair);
      METALFX_WARN_ONCE(warned_encode_, "MetalFX: encoding failed: {}", error);
      return false;
    }
    pair.serial = ++dispatch_serial_;
    pair.committed_at = std::chrono::steady_clock::now();
    const VkEvent inputs_ready = pair.inputs_ready;
    const VkEvent output_ready = pair.output_ready;
    pending_.push_back(std::move(pair));

    // Vulkan: hand the inputs over, then wait for the output.
    const VkMemoryBarrier output_visible = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    cmd_set_event_(command_buffer, inputs_ready, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    cmd_wait_events_(command_buffer, 1, &output_ready, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 1, &output_visible, 0, nullptr, 0,
                     nullptr);
    return true;
  }

  void Release() override {
    if (!pending_.empty()) {
      if (!WaitForPending()) {
        // A Metal command buffer still waiting for inputs_ready belongs to a
        // Vulkan frame that was never submitted: let it run.
        for (EventPair& pair : pending_) {
          if (!pair.completed->load(std::memory_order_acquire) && pair.inputs_event &&
              MetalFxBridge::GetEventValue(pair.inputs_event) == 0) {
            MetalFxBridge::SetEventValue(pair.inputs_event, 1);
          }
        }
        if (!WaitForPending()) {
          REXLOG_ERROR("MetalFX: Metal command buffers still pending at release");
        }
      }
      for (EventPair& pair : pending_) {
        DestroyEventPair(pair);
      }
      pending_.clear();
    }
    if (bridge_) {
      bridge_->ReleaseResources();
    }
    scaler_key_ = {};
    failed_key_ = {};
  }

 private:
  struct EventPair {
    VkEvent inputs_ready = VK_NULL_HANDLE;
    VkEvent output_ready = VK_NULL_HANDLE;
    // The MTLSharedEvents under them, owned by the VkEvents.
    void* inputs_event = nullptr;
    void* output_event = nullptr;
    std::shared_ptr<std::atomic<bool>> completed;
    uint64_t serial = 0;
    std::chrono::steady_clock::time_point committed_at;
  };

  // What the scaler was made for; a change makes a new one.
  struct ScalerKey {
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint64_t color_format = 0;
    uint64_t depth_format = 0;
    uint64_t motion_format = 0;
    uint64_t output_format = 0;
    bool operator==(const ScalerKey& other) const = default;
  };

  // The exported MTLTextures of color, depth, motion and output, unretained
  // (owned by the VkImages).
  bool ExportTextures(const VulkanUpscalerInputs& inputs, void* (&textures_out)[4]) {
    VkExportMetalTextureInfoEXT textures[4] = {};
    const VkImage images[4] = {inputs.color.image, inputs.depth.image, inputs.motion_vectors.image,
                               inputs.output.image};
    for (uint32_t i = 0; i < 4; ++i) {
      textures[i].sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
      textures[i].pNext = i < 3 ? &textures[i + 1] : nullptr;
      textures[i].image = images[i];
      textures[i].plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    }
    VkExportMetalObjectsInfoEXT objects_info = {};
    objects_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
    objects_info.pNext = &textures[0];
    export_metal_objects_(device_->device(), &objects_info);
    bool all_exported = true;
    for (uint32_t i = 0; i < 4; ++i) {
      textures_out[i] = textures[i].mtlTexture;
      all_exported = all_exported && textures_out[i];
    }
    return all_exported;
  }

  bool EnsureScaler(const UpscalerFrame& frame, const MetalFxBridge::TextureInfo& color,
                    const MetalFxBridge::TextureInfo& depth,
                    const MetalFxBridge::TextureInfo& motion,
                    const MetalFxBridge::TextureInfo& output, bool& recreated) {
    ScalerKey key;
    key.input_width = color.width;
    key.input_height = color.height;
    key.render_width = frame.render_width;
    key.render_height = frame.render_height;
    key.output_width = frame.output_width;
    key.output_height = frame.output_height;
    key.color_format = color.pixel_format;
    key.depth_format = depth.pixel_format;
    key.motion_format = motion.pixel_format;
    key.output_format = output.pixel_format;
    if (bridge_->has_scaler() && key == scaler_key_) {
      return true;
    }
    // A scaler that failed is not retried every frame for the same inputs.
    if (key == failed_key_) {
      return false;
    }
    MetalFxBridge::ScalerDesc desc;
    desc.input_width = key.input_width;
    desc.input_height = key.input_height;
    desc.output_width = key.output_width;
    desc.output_height = key.output_height;
    desc.color_format = key.color_format;
    desc.depth_format = key.depth_format;
    desc.motion_format = key.motion_format;
    desc.output_format = key.output_format;
    scaler_key_ = {};
    if (!bridge_->CreateScaler(desc, usages_)) {
      failed_key_ = key;
      REXLOG_ERROR(
          "MetalFX: could not create the temporal scaler for {}x{} (render {}x{}) to {}x{}, "
          "formats color {} depth {} motion {} output {}",
          key.input_width, key.input_height, key.render_width, key.render_height,
          key.output_width, key.output_height, key.color_format, key.depth_format,
          key.motion_format, key.output_format);
      return false;
    }
    failed_key_ = {};
    scaler_key_ = key;
    recreated = true;
    REXLOG_INFO(
        "MetalFX: temporal scaler for {}x{} (render {}x{}) to {}x{}, formats color {} depth {} "
        "motion {} output {}, usages color {} depth {} motion {} output {} (texture usages {} {} "
        "{} {})",
        key.input_width, key.input_height, key.render_width, key.render_height, key.output_width,
        key.output_height, key.color_format, key.depth_format, key.motion_format,
        key.output_format, usages_.color, usages_.depth, usages_.motion, usages_.output,
        color.usage, depth.usage, motion.usage, output.usage);
    return true;
  }

  static bool InputUsageFits(const MetalFxBridge::TextureInfo& texture, uint64_t needed,
                             const char* name, bool& warned) {
    if ((texture.usage & needed) == needed) {
      return true;
    }
    if (!warned) {
      warned = true;
      REXLOG_WARN("MetalFX: the {} texture has usage {} without {} that MetalFX needs", name,
                  texture.usage, needed & ~texture.usage);
    }
    return false;
  }

  bool CreateEventPair(EventPair& pair) {
    VkExportMetalObjectCreateInfoEXT export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
    export_info.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT;
    VkEventCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    create_info.pNext = &export_info;
    const VkDevice vk_device = device_->device();
    if (create_event_(vk_device, &create_info, nullptr, &pair.inputs_ready) != VK_SUCCESS) {
      pair.inputs_ready = VK_NULL_HANDLE;
      return false;
    }
    if (create_event_(vk_device, &create_info, nullptr, &pair.output_ready) != VK_SUCCESS) {
      pair.output_ready = VK_NULL_HANDLE;
      DestroyEventPair(pair);
      return false;
    }
    VkExportMetalSharedEventInfoEXT events[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      events[i].sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT;
      events[i].pNext = i ? nullptr : &events[1];
      events[i].event = i ? pair.output_ready : pair.inputs_ready;
    }
    VkExportMetalObjectsInfoEXT objects_info = {};
    objects_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
    objects_info.pNext = &events[0];
    export_metal_objects_(vk_device, &objects_info);
    pair.inputs_event = events[0].mtlSharedEvent;
    pair.output_event = events[1].mtlSharedEvent;
    // Fresh events only: a reused one is past 0, and MoltenVK's values with it.
    if (!pair.inputs_event || !pair.output_event ||
        MetalFxBridge::GetEventValue(pair.inputs_event) != 0 ||
        MetalFxBridge::GetEventValue(pair.output_event) != 0) {
      DestroyEventPair(pair);
      return false;
    }
    pair.completed = std::make_shared<std::atomic<bool>>(false);
    return true;
  }

  void DestroyEventPair(EventPair& pair) {
    const VkDevice vk_device = device_->device();
    if (pair.inputs_ready != VK_NULL_HANDLE) {
      destroy_event_(vk_device, pair.inputs_ready, nullptr);
      pair.inputs_ready = VK_NULL_HANDLE;
    }
    if (pair.output_ready != VK_NULL_HANDLE) {
      destroy_event_(vk_device, pair.output_ready, nullptr);
      pair.output_ready = VK_NULL_HANDLE;
    }
    pair.inputs_event = nullptr;
    pair.output_event = nullptr;
  }

  // Frees the oldest pairs whose Metal work is done and whose Vulkan frame
  // is long past.
  void RetirePairs() {
    // A Vulkan frame that was never submitted (a failed vkQueueSubmit) leaves
    // its Metal command buffer waiting for inputs_ready, and every later one
    // queued behind it: after a second, let it run on whatever the inputs hold.
    const auto now = std::chrono::steady_clock::now();
    for (EventPair& pair : pending_) {
      if (!pair.completed->load(std::memory_order_acquire) && pair.inputs_event &&
          MetalFxBridge::GetEventValue(pair.inputs_event) == 0 &&
          now - pair.committed_at > std::chrono::seconds(1)) {
        REXLOG_WARN("MetalFX: upscale {} waited over a second for its frame; releasing it",
                    pair.serial);
        MetalFxBridge::SetEventValue(pair.inputs_event, 1);
      }
    }
    while (!pending_.empty()) {
      EventPair& pair = pending_.front();
      if (!pair.completed->load(std::memory_order_acquire) ||
          dispatch_serial_ - pair.serial < kPairRetireDispatches) {
        break;
      }
      DestroyEventPair(pair);
      pending_.pop_front();
    }
  }

  // True when every pending Metal command buffer finished within the bound.
  bool WaitForPending() const {
    const auto deadline = std::chrono::steady_clock::now() + kReleaseWait;
    for (;;) {
      bool all_completed = true;
      for (const EventPair& pair : pending_) {
        if (!pair.completed->load(std::memory_order_acquire)) {
          all_completed = false;
          break;
        }
      }
      if (all_completed) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  const VulkanDevice* device_;
  UpscalerInfo info_;
  PFN_vkExportMetalObjectsEXT export_metal_objects_ = nullptr;
  PFN_vkCreateEvent create_event_ = nullptr;
  PFN_vkDestroyEvent destroy_event_ = nullptr;
  PFN_vkCmdSetEvent cmd_set_event_ = nullptr;
  PFN_vkCmdWaitEvents cmd_wait_events_ = nullptr;
  std::unique_ptr<MetalFxBridge> bridge_;
  float min_scale_ = 1.0f;
  float max_scale_ = 2.0f;
  ScalerKey scaler_key_;
  ScalerKey failed_key_;
  MetalFxBridge::ScalerUsages usages_;
  std::deque<EventPair> pending_;
  uint64_t dispatch_serial_ = 0;
  bool warned_pending_ = false;
  bool warned_export_ = false;
  bool warned_sizes_ = false;
  bool warned_scale_ = false;
  bool warned_color_usage_ = false;
  bool warned_depth_usage_ = false;
  bool warned_motion_usage_ = false;
  bool warned_output_usage_ = false;
  bool warned_own_output_ = false;
  bool warned_events_ = false;
  bool warned_encode_ = false;
};

#undef METALFX_WARN_ONCE

}  // namespace

std::unique_ptr<VulkanUpscaler> CreateVulkanMetalFxUpscaler(const VulkanDevice* device) {
  return std::make_unique<VulkanMetalFxUpscaler>(device);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
