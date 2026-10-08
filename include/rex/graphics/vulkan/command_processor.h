#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstring>
#include <map>
#include <source_location>
#include <utility>
#include <vector>
#include <chrono>

#include <rex/assert.h>
#include <rex/graphics/fh1_frame_dump.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/linked_type_descriptor_set_allocator.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor : public CommandProcessor {
 public:
  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBufferCompute,
    kStorageBufferPairCompute,
    // One storage image for compute shaders.
    kStorageImageCompute,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor, VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask, VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) = delete;
    ScratchBufferAcquisition& operator=(const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void OnReduceMemory() override;
  void InvalidateGpuMemory() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  // Fixed for the processor's lifetime; cached, since draws ask for it many
  // times and provider() is virtual.
  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    ui::vulkan::VulkanDevice* device = vulkan_device_cache_.load(std::memory_order_relaxed);
    if (!device) {
      device = static_cast<const ui::vulkan::VulkanProvider*>(graphics_system_->provider())
                   ->vulkan_device();
      vulkan_device_cache_.store(device, std::memory_order_relaxed);
    }
    return device;
  }

  bool CompileGlslToSpirv(VkShaderStageFlagBits stage, std::string_view source,
                          std::vector<uint32_t>& spirv_out, std::string& error_out) const;

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  uint64_t GetCurrentSubmission() const {
    return submission_completed_ + uint64_t(submissions_in_flight_fences_.size()) + 1;
  }
  uint64_t GetCompletedSubmission() const { return submission_completed_; }

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                               VkPipelineStageFlags src_stage_mask,
                               VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                               VkAccessFlags dst_access_mask,
                               uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               bool skip_if_equal = true);
  bool PushImageMemoryBarrier(VkImage image, const VkImageSubresourceRange& subresource_range,
                              VkPipelineStageFlags src_stage_mask,
                              VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                              VkAccessFlags dst_access_mask, VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              bool skip_if_equal = true);
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass,
                      std::source_location caller = std::source_location::current());

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth);
  // Dynamic rendering for the FH1 native executor: begins the scope unless
  // the one identified by `id` is already open. Submission must be open.
  void SubmitBarriersAndBeginFh1Rendering(const VkRenderingInfo& rendering_info, uint64_t id);
  // Pipeline barrier commands and FH1 renderings recorded since the last call.
  Fh1NativeExecutor* GetFh1NativeExecutor() const { return fh1_native_executor_.get(); }
  uint64_t TakeBarrierBatchCount() { return std::exchange(barrier_batch_count_, 0); }
  uint64_t TakeRenderingBeginCount() { return std::exchange(rendering_begin_count_, 0); }
  // Why each FH1 rendering began (AP-7.4: a tiler stores and reloads the
  // attachments at every rendering boundary): barriers between draws to the
  // same surfaces, barriers with a change of surfaces, a change of surfaces
  // alone, or a rendering already ended by other work (transfers, clears,
  // resolves, uploads).
  enum Fh1RenderingCause : uint32_t {
    kFh1RenderingBarriersSameSurfaces,
    kFh1RenderingBarriersNewSurfaces,
    kFh1RenderingNewSurfaces,
    kFh1RenderingEndedElsewhere,
    kFh1RenderingCauseCount,
  };
  std::array<uint64_t, kFh1RenderingCauseCount> TakeFh1RenderingCauses() {
    return std::exchange(fh1_rendering_causes_, {});
  }
  // For kFh1RenderingEndedElsewhere: the functions that ended the previous
  // rendering, with counts. Keyed by the source_location function name, a
  // static string, so counting allocates nothing.
  std::map<const char*, uint64_t> TakeFh1RenderingEnders() {
    return std::exchange(fh1_rendering_enders_, {});
  }
  // vulkan_diagnostic_checkpoints: marks the command stream so a device loss
  // reports the last draws, copies, texture loads and executor passes the GPU
  // reached. Each marker is a serial number; a ring keeps what each recent
  // serial was, with a description where the caller gives one.
  enum class CheckpointKind : uint32_t {
    kDraw = 1,
    kCopy,
    kTextureLoad,
    kResolve,
    kTransfer,
    kClear,
    kDrawEnd,
    kTextureLoadEnd,
    // Recorded on the CPU only, without a marker in the command stream.
    kNote,
  };
  // Without a value, the current draw index in the frame.
  void Checkpoint(CheckpointKind kind, uint32_t value = UINT32_MAX, std::string detail = {});
  bool checkpoints_enabled() const { return checkpoints_enabled_; }
  // A CPU-side record in the checkpoint ring (resource lifetimes, bindings).
  void NoteCheckpoint(std::string detail);
  // vulkan_debug_labels: names GPU work for capture tools (Nsight Graphics
  // GPU Trace, RenderDoc). Labels must not straddle a rendering's begin or
  // end; FH1 renderings label themselves.
  void BeginDebugLabel(const char* name) {
    if (debug_labels_) deferred_command_buffer_.CmdVkBeginDebugLabel(name);
  }
  void EndDebugLabel() {
    if (debug_labels_) deferred_command_buffer_.CmdVkEndDebugLabel();
  }
  // The label of the next FH1 rendering begun (a string literal), instead of
  // the one its id implies.
  void SetNextRenderingLabel(const char* label) { next_rendering_label_ = label; }
  bool IsFh1RenderingOpen(uint64_t id) const {
    return in_render_pass_ && id && current_fh1_rendering_id_ == id;
  }
  system::GraphicsShaderTranslationObserver GetShaderTranslationObserver() const;
  // Submits the open submission, waits for every queue operation and reopens
  // the submission if one was open (FH1 resolve read-backs).
  void Fh1AwaitAllQueueOperations();
  // fh1_frame_replay: replays a frame dump (recorded on D3D12) on Vulkan and
  // ends the process.
  void RunRequestedFrameReplay() override;
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass(std::source_location caller = std::source_location::current());

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(transient_descriptor_layout)];
  }
  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex, size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(size_t texture_count_pixel,
                                                                       size_t sampler_count_pixel,
                                                                       size_t texture_count_vertex,
                                                                       size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  ScratchBufferAcquisition AcquireScratchGpuBuffer(VkDeviceSize size,
                                                   VkPipelineStageFlags initial_stage_mask,
                                                   VkAccessFlags initial_access_mask);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline, bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  // Returns the text to display in the GPU backend name in the window title.
  std::string GetWindowTitleText() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  // Creates the render target cache, the pipeline cache, the texture cache,
  // the FH1 native executor and (at 1x) the frame dump at a draw resolution
  // scale.
  bool CreateScaledComponents(uint32_t draw_resolution_scale_x,
                              uint32_t draw_resolution_scale_y);
  // Points the shared memory and EDRAM descriptor set at the shared memory and
  // at the render target cache's EDRAM buffer.
  void WriteSharedMemoryAndEdramDescriptorSet();
  // Between frames, rebuilds the scale-dependent components when the draw
  // resolution scale cvars name another square scale from 1x to 4x, so the
  // scale changes without a restart (NP-4.7).
  void SwitchDrawResolutionScaleIfRequested();
  // Between frames, destroys the samplers when texture_mip_lod_bias differs
  // from the bias they were created with, so they are created again with it.
  void RecreateSamplersIfMipLodBiasChanged();
  // Forgets the samplers and texture descriptors draws reuse. The GPU must be
  // done with every submission and none may be open.
  void ResetSamplerBindings();
  // Forgets everything draws reuse from the render target, pipeline and
  // texture caches (pipelines, translations, viewports, samplers, descriptors
  // and the bound state), as after they are created again. Same conditions.
  void ResetCachedDrawState();

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  void WriteRegistersHost(uint32_t start_index, const uint32_t* values,
                          uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;
  void FlushCpuVisibleResults() override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;
  void OnPrimaryBufferEnd() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void IssueSwapImpl(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                     uint32_t frontbuffer_height);

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  uint64_t HashShaderMicrocode(const uint32_t* host_address,
                               uint32_t dword_count) const override;
  Shader* LoadShaderHashed(xenos::ShaderType shader_type, uint32_t guest_address,
                           const uint32_t* host_address, uint32_t dword_count,
                           uint64_t hash) override;

  bool IssueDrawImpl(xenos::PrimitiveType prim_type, uint32_t index_count,
                     IndexBufferInfo* index_buffer_info, bool major_mode_explicit);
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  bool SupportsRecordThread() const override { return true; }

 private:
  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  // Submission worker (as on D3D12): an ended submission's command stream is
  // replayed into its command buffer and submitted on another thread, so the
  // driver's recording overlaps the next submission's command processing. A
  // frame splits into submissions every vulkan_async_submission_split_draws
  // draws; the swap waits for the worker, so the presenter's work follows the
  // frame on the queue.
  struct SubmissionJob {
    std::unique_ptr<DeferredCommandBuffer> tape;
    CommandBuffer command_buffer;
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stage_masks;
    VkFence fence = VK_NULL_HANDLE;
    // Without a tape: work to run on the worker after the jobs before it (the
    // presenter's publish and paint).
    std::function<void()> callback;
    // With a tape: the submission's number.
    uint64_t submission = 0;
  };
  // Replays `tape` into `command_buffer` and submits it signaling `fence`;
  // on failure still signals the fence (an empty submission) unless the
  // device is lost, so waits for it end.
  bool ExecuteSubmission(DeferredCommandBuffer& tape, const CommandBuffer& command_buffer,
                         const std::vector<VkSemaphore>& wait_semaphores,
                         const std::vector<VkPipelineStageFlags>& wait_stage_masks,
                         VkFence fence);
  void StartSubmissionWorker();
  void StopSubmissionWorker();
  void SubmissionWorkerMain();
  void AwaitSubmissionWorker();
  // Waits until the worker has passed `submission` to vkQueueSubmit: its fence
  // must not be waited for before (the fence is externally synchronized, and
  // an AMD driver fails a wait for a fence that was never submitted).
  void AwaitSubmissionWorkerSubmitted(uint64_t submission);
  void LogCheckpoints();
  bool checkpoints_enabled_ = false;
  // What each recent checkpoint serial was, for the device-loss report.
  struct CheckpointRecord {
    uint64_t serial = 0;
    CheckpointKind kind = CheckpointKind::kDraw;
    uint32_t frame = 0;
    uint32_t value = 0;
    std::string detail;
  };
  static constexpr size_t kCheckpointRecords = size_t(1) << 16;
  std::vector<CheckpointRecord> checkpoint_records_;
  uint64_t checkpoint_serial_ = 0;
  bool async_submission_ = false;
  uint32_t submission_split_draws_ = 0;
  uint32_t submission_draws_ = 0;
  std::thread submission_worker_;
  std::mutex submission_worker_mutex_;
  std::condition_variable submission_worker_wake_;
  std::condition_variable submission_worker_idle_;
  std::deque<SubmissionJob> submission_jobs_;
  std::vector<std::unique_ptr<DeferredCommandBuffer>> submission_free_tapes_;
  bool submission_worker_busy_ = false;
  bool submission_worker_stop_ = false;
  // The last submission the worker has submitted, and its signal.
  std::atomic<uint64_t> submission_worker_submitted_{0};
  std::condition_variable submission_worker_progress_;
  // The presenter's publish-and-paint step runs on the submission worker after
  // the swap's submission instead of the swap waiting for the worker
  // (vulkan_present_on_submission_worker).
  ui::Presenter* present_deferred_presenter_ = nullptr;
  uint64_t present_steps_queued_ = 0;
  std::atomic<uint64_t> present_steps_done_{0};
  // The render-test notification of the frame being published, run after
  // the step.
  std::function<void()> pending_present_notify_;
  void DeferPresentStep(std::function<void()> step);

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const { return key == other_key.key; }
    bool operator!=(const PipelineLayoutKey& other_key) const { return !(*this == other_key); }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(VkPipelineLayout pipeline_layout,
                            VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
                            VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref,
                            bool textures_pixel_pushed)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(descriptor_set_layout_textures_pixel_ref),
          textures_pixel_pushed_(textures_pixel_pushed) {}
    VkPipelineLayout GetPipelineLayout() const override { return pipeline_layout_; }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }
    // The pixel texture set is a push descriptor set layout.
    bool textures_pixel_pushed() const { return textures_pixel_pushed_; }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
    bool textures_pixel_pushed_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,

    kSwapApplyGammaDescriptorSetCount,
  };
  enum SwapApplyGammaComputeDescriptorSet : uint32_t {
    kSwapApplyGammaComputeDescriptorSetRamp,
    kSwapApplyGammaComputeDescriptorSetSource,
    kSwapApplyGammaComputeDescriptorSetDestination,

    kSwapApplyGammaComputeDescriptorSetCount,
  };
  struct SwapApplyGammaConstants {
    uint32_t size[2];
  };
  enum SwapFxaaDescriptorSet : uint32_t {
    kSwapFxaaDescriptorSetSource,
    kSwapFxaaDescriptorSetDestination,

    kSwapFxaaDescriptorSetCount,
  };
  struct SwapFxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };

  // Framebuffer for the current presenter's guest output image revision, and
  // its usage tracking.
  struct SwapFramebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = UINT64_MAX;
    uint64_t last_submission = 0;
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFenceAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ && submissions_in_flight_fences_.empty();
  }
  // Guest frame GPU timing for the performance CSV (guest_frame_gpu_time_ns):
  // one timestamp at the frame's first submission and one at its swap, read
  // once the frame's last submission has completed, as on D3D12.
  void BeginFrameGpuTiming();
  void EndFrameGpuTiming();
  void ReadFrameGpuTiming();
  // Once a second, the perf capture's memory gauges; every
  // vulkan_memory_budget_log_seconds, a log line: each heap's usage against
  // its budget (VK_EXT_memory_budget), the native surfaces, the texture cache
  // and the resident size.
  void LogMemoryBudget();

  // RR-0.1, gpu_draw_cost_model: where a draw's recorder time goes. One draw
  // in 16 is timed step by step; every draw's IssueDraw and every register
  // run are timed whole, and the frame's record batch time is the total they
  // are checked against. Logged by the frame's draw band every 600 frames.
  enum DrawCostStep : uint32_t {
    kCostAnalysis,          // shader and memexport analysis, census
    kCostPrimitives,        // submission begin, primitive processing
    kCostTranslation,       // translations
    kCostSamplers,          // sampler parameters and UseSampler
    kCostTextures,          // RequestTextures
    kCostTargets,           // executor PrepareTargets and BindTargets
    kCostPipeline,          // pipeline lookup and bind, layout
    kCostDynamicState,      // viewport and dynamic state
    kCostSystemConstants,   // UpdateSystemConstantValues
    kCostConstantUploads,   // UpdateBindings: constant buffers
    kCostImageInfos,        // UpdateBindings: image and sampler infos
    kCostTextureBindings,   // UpdateBindings: set reuse and push checks
    kCostConstantsSet,      // UpdateBindings: constants set lookup and writes
    kCostTextureSets,       // UpdateBindings: texture set allocation and writes
    kCostDescriptorUpdate,  // UpdateBindings: vkUpdateDescriptorSets
    kCostDescriptorBinds,   // UpdateBindings: binds and pushes
    kCostVertexBuffers,     // vertex residency
    kCostMemexport,         // memexport ranges, shared memory use
    kCostBeginRendering,    // executor BeginDrawRendering
    kCostDraw,              // the draw command and what follows it
    kCostStepCount
  };
  void DrawCostMark(DrawCostStep step) {
    if (draw_cost_.sampling) {
      const uint64_t now = DrawCostNow();
      draw_cost_.pending[step] += now - draw_cost_.last;
      draw_cost_.last = now;
    }
  }
  static uint64_t DrawCostNow() { return CostTicks(); }
  void DrawCostFrame();
  struct DrawCostModel {
    bool enabled = false;
    bool sampling = false;
    uint64_t last = 0;
    uint64_t pending[kCostStepCount] = {};
    uint64_t counter = 0;
    uint64_t frame_draws = 0, frame_sampled = 0, frame_issue_ns = 0, frame_register_ns = 0;
    uint64_t frame_steps[kCostStepCount] = {};
    uint64_t batch_ns_at_frame = 0;
    uint64_t draws_ns_at_frame = 0, ones_ns_at_frame = 0, calls_ns_at_frame = 0;
    uint64_t batch_ticks_at_frame = 0, entries_at_frame = 0;
    // Tick rate: ticks and steady nanoseconds at the start of the window.
    uint64_t window_ticks = 0, window_ns = 0;
    struct Band {
      uint64_t frames = 0, draws = 0, sampled = 0, issue_ns = 0, register_ns = 0, batch_ns = 0;
      uint64_t draw_records_ns = 0, ones_ns = 0, calls_ns = 0, batch_ticks = 0, entries = 0;
      uint64_t steps[kCostStepCount] = {};
    } bands[3];
    uint64_t frames = 0;
  } draw_cost_;
  // RR-2.1 census, with the cost model: uploads by constant buffer (system,
  // float vertex, float pixel, bool and loop, fetch), their bytes, and how
  // many repeat the buffer's previous upload byte for byte.
  struct ConstantCensus {
    uint64_t draws = 0;
    uint64_t uploads[5] = {}, bytes[5] = {}, repeats[5] = {};
    std::vector<uint8_t> last[5];
  } constant_census_;
  bool constant_census_enabled_ = false;
  // PD-3.3 census, with the cost model: constants set lookups by outcome.
  uint64_t constants_set_last_ = 0, constants_set_table_ = 0, constants_set_new_ = 0;
  uint64_t prepare_calls_at_window_ = 0, prepare_hits_at_window_ = 0;
  void CountConstantUpload(uint32_t buffer, const void* data, size_t size);
  std::chrono::steady_clock::time_point memory_sampled_{};
  std::chrono::steady_clock::time_point memory_budget_logged_{};
  bool memory_over_budget_ = false;
  std::vector<uint64_t> memory_heap_peak_;
  uint64_t memory_resident_peak_ = 0;
  // Keep primary-buffer-end submit behavior aligned with D3D12: only submit
  // when immediate submission is safe.
  bool CanEndSubmissionImmediately() const;

  void ClearTransientDescriptorPools();
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(uint32_t total_size);
  bool IssueDraw_MemexportReadbackFastPath(uint32_t total_size);

  void SplitPendingBarrier();

  void DestroyScratchBuffer();
  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address);
  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);
  void DisableHostOcclusionQueries();
  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;
  void WriteGuestOcclusionResult(uint32_t sample_count_address, uint64_t samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(uint32_t vfetch_index);
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);
  struct ReadbackBuffer {
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* mapped_data[2] = {nullptr, nullptr};
    uint32_t sizes[2] = {0, 0};
    uint64_t submission_written[2] = {0, 0};
    uint32_t written_size[2] = {0, 0};
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static inline uint64_t MakeReadbackResolveKey(uint32_t address, uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }
  static inline uint64_t MakeMemexportReadbackKey(uint32_t first_base_address_dwords,
                                                  uint32_t total_size) {
    return (uint64_t(first_base_address_dwords) << 32) | uint64_t(total_size);
  }
  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t source_offset_bytes;
    uint32_t half_pixel_offset;
  };
  bool EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height);
  void DestroySwapFxaaSourceImage();

  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
                          reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
      const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask);
  bool UpdateBindings(const VulkanShader* vertex_shader, const VulkanShader* pixel_shader);
  // Allocates a descriptor set and fills one or two VkWriteDescriptorSet
  // structure instances (for images and samplers).
  // The descriptor set layout must be the one for the given is_vertex,
  // texture_count, sampler_count (from GetTextureDescriptorSetLayout - may be
  // already available at the moment of the call, no need to locate it again).
  // Returns how many VkWriteDescriptorSet structure instances have been
  // written, or 0 if there was a failure to allocate the descriptor set or no
  // bindings were requested.
  // Whether the stage's last texture descriptor set (same frame, layout,
  // views and samplers) can be bound again; if not, records the new contents
  // for the set the caller is about to write.
  bool ReuseTextureDescriptorSet(bool is_pixel, uint32_t texture_count, uint32_t sampler_count,
                                 VkDescriptorSetLayout layout,
                                 const VkDescriptorImageInfo* textures,
                                 const VkDescriptorImageInfo* samplers);
  uint32_t WriteTransientTextureBindings(bool is_vertex, uint32_t texture_count,
                                         uint32_t sampler_count,
                                         VkDescriptorSetLayout descriptor_set_layout,
                                         const VkDescriptorImageInfo* texture_image_info,
                                         const VkDescriptorImageInfo* sampler_image_info,
                                         VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkFence> fences_free_;
  std::vector<VkSemaphore> semaphores_free_;

  bool submission_open_ = false;
  uint64_t submission_completed_ = 0;
  // Guest output frames presented, for the host's presented-output observer.
  uint64_t presented_output_frames_ = 0;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::vector<VkFence> submissions_in_flight_fences_;
  std::deque<std::pair<uint64_t, VkSemaphore>> submissions_in_flight_semaphores_;

  static constexpr uint32_t kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  // Tracks whether any draw in the current frame used an async placeholder
  // graphics pipeline and may have produced incomplete output.
  bool frame_used_async_placeholder_pipeline_ = false;
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ = VK_NULL_HANDLE;

  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout, PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBufferDynamic;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageImage;
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_uniform_buffer_dynamic_;
  // Guest draw constants sets of the current frame by buffers and ranges;
  // draws pass the offsets dynamically.
  struct ConstantsDescriptorSetKey {
    VkBuffer buffers[SpirvShaderTranslator::kConstantBufferCount] = {};
    uint32_t ranges[SpirvShaderTranslator::kConstantBufferCount] = {};
    // Five ranges leave the key 4 bytes short of its alignment: an explicit
    // member keeps operator==, which compares the bytes, off indeterminate
    // padding (which made equal keys miss, PD-3.3).
    uint32_t padding = 0;
    bool operator==(const ConstantsDescriptorSetKey& other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  };
  static_assert(sizeof(ConstantsDescriptorSetKey) ==
                    sizeof(VkBuffer) * SpirvShaderTranslator::kConstantBufferCount +
                        sizeof(uint32_t) * (SpirvShaderTranslator::kConstantBufferCount + 1),
                "ConstantsDescriptorSetKey must have no padding");
  struct ConstantsDescriptorSetKeyHasher {
    size_t operator()(const ConstantsDescriptorSetKey& key) const {
      size_t hash = 0;
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
        hash = hash * 1099511628211ull ^ size_t(reinterpret_cast<uintptr_t>(key.buffers[i]));
        hash = hash * 1099511628211ull ^ key.ranges[i];
      }
      return hash;
    }
  };
  // Open addressing with a generation per entry: clearing it each frame is a
  // counter bump, and a new key allocates nothing (an unordered_map's node
  // allocations and clears were 1 % of the recorder).
  class ConstantsDescriptorSetTable {
   public:
    void Clear() {
      if (++generation_ == 0) {
        for (Entry& entry : entries_) {
          entry.generation = 0;
        }
        generation_ = 1;
      }
      size_ = 0;
    }
    VkDescriptorSet* Find(const ConstantsDescriptorSetKey& key) {
      if (entries_.empty()) {
        return nullptr;
      }
      const size_t mask = entries_.size() - 1;
      for (size_t i = ConstantsDescriptorSetKeyHasher()(key) & mask;; i = (i + 1) & mask) {
        Entry& entry = entries_[i];
        if (entry.generation != generation_) {
          return nullptr;
        }
        if (entry.key == key) {
          return &entry.set;
        }
      }
    }
    void Insert(const ConstantsDescriptorSetKey& key, VkDescriptorSet set) {
      if ((size_ + 1) * 2 > entries_.size()) {
        Grow();
      }
      const size_t mask = entries_.size() - 1;
      size_t i = ConstantsDescriptorSetKeyHasher()(key) & mask;
      while (entries_[i].generation == generation_) {
        i = (i + 1) & mask;
      }
      entries_[i].key = key;
      entries_[i].set = set;
      entries_[i].generation = generation_;
      ++size_;
    }

   private:
    struct Entry {
      ConstantsDescriptorSetKey key;
      VkDescriptorSet set = VK_NULL_HANDLE;
      uint32_t generation = 0;
    };
    void Grow() {
      std::vector<Entry> old = std::move(entries_);
      entries_.assign(std::max(old.size() * 2, size_t(1024)), Entry());
      const uint32_t old_generation = generation_;
      generation_ = 1;
      size_ = 0;
      for (const Entry& entry : old) {
        if (entry.generation == old_generation) {
          Insert(entry.key, entry.set);
        }
      }
    }
    std::vector<Entry> entries_;
    uint32_t generation_ = 1;
    size_t size_ = 0;
  };
  ConstantsDescriptorSetTable constants_descriptor_sets_frame_;
  uint64_t constants_descriptor_sets_frame_index_ = 0;
  ConstantsDescriptorSetKey last_constants_descriptor_set_key_;
  VkDescriptorSet last_constants_descriptor_set_ = VK_NULL_HANDLE;
  uint32_t current_constant_dynamic_offsets_[SpirvShaderTranslator::kConstantBufferCount] = {};
  VkDescriptorBufferInfo
      constants_descriptor_write_infos_[SpirvShaderTranslator::kConstantBufferCount] = {};
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_storage_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_storage_image_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>, size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>> constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet> texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey, std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;
  // The FH1 native renderer (NP-12.4); guest draws, copies and clears go
  // through it instead of the render target cache when it initialized.
  std::unique_ptr<Fh1NativeExecutor> fh1_native_executor_;
  // fh1_frame_dump_frame: records one frame for offline replay (at 1x).
  std::unique_ptr<Fh1FrameDump> frame_dump_;
  // Identifies the open dynamic rendering scope of the executor (0: none).
  uint64_t current_fh1_rendering_id_ = 0;
  bool debug_labels_ = false;
  // An FH1 rendering opened a label its end closes.
  bool rendering_label_open_ = false;
  const char* next_rendering_label_ = nullptr;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  // The shader storage the pipeline cache opened, to reopen at another scale.
  std::filesystem::path shader_storage_cache_root_;
  uint32_t shader_storage_title_id_ = 0;
  // A requested scale that cannot be switched to, so the request is reported
  // once.
  uint32_t declined_draw_resolution_scale_ = 0;
  // texture_mip_lod_bias when the texture cache's samplers were last dropped.
  double sampler_mip_lod_bias_ = 0.0;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_combined_image_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ = VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight> swap_descriptors_gamma_ramp_;
  // Sampled images.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Combined image sampler descriptors for FXAA.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_source_;
  // Storage image descriptors for the apply-gamma compute pass destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_destination_storage_;
  // Separate storage image descriptors for the FXAA compute destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_destination_storage_;

  VkSampler swap_sampler_linear_clamp_ = VK_NULL_HANDLE;

  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_apply_gamma_compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  // Has no dependencies on specific pipeline stages on both ends to simplify
  // use in different scenarios with different pipelines - use explicit barriers
  // for synchronization.
  VkRenderPass swap_apply_gamma_render_pass_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  VkImage swap_fxaa_source_image_ = VK_NULL_HANDLE;
  VkDeviceMemory swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
  VkImageView swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t swap_fxaa_source_image_width_ = 0;
  uint32_t swap_fxaa_source_image_height_ = 0;
  uint64_t swap_fxaa_source_image_submission_ = 0;
  VkPipelineStageFlags swap_fxaa_source_stage_mask_ = 0;
  VkAccessFlags swap_fxaa_source_access_mask_ = 0;
  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  std::array<SwapFramebuffer, ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      swap_framebuffers_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  static constexpr uint32_t kMaxOcclusionQueries = 8192;
  VkQueryPool occlusion_query_pool_ = VK_NULL_HANDLE;
  VkBuffer occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  uint32_t occlusion_query_readback_memory_type_ = UINT32_MAX;
  VkDeviceSize occlusion_query_readback_memory_size_ = 0;
  uint8_t* occlusion_query_readback_mapping_ = nullptr;
  uint32_t occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;

  static constexpr uint32_t kFrameTimingQueriesPerFrame = 2;
  VkQueryPool frame_timing_query_pool_ = VK_NULL_HANDLE;
  struct FrameTimingSlot {
    bool started = false;
    // The submission whose completion makes both timestamps readable; zero
    // while the frame is still open.
    uint64_t submission = 0;
  };
  FrameTimingSlot frame_timing_slots_[kMaxFramesInFlight] = {};
  uint64_t barrier_batch_count_ = 0;
  uint64_t rendering_begin_count_ = 0;
  std::array<uint64_t, kFh1RenderingCauseCount> fh1_rendering_causes_{};
  std::map<const char*, uint64_t> fh1_rendering_enders_;
  const char* last_rendering_ender_ = "";
  struct ActiveOcclusionQuery {
    uint32_t sample_count_address = 0;
    uint32_t host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_pixel_;
  // Sampler parameters and the sampler last used for each binding slot, reused
  // while the binding's fetch constant, filters and filtering overrides are
  // unchanged (as on D3D12); the sampler only within the submission that last
  // used it, since the texture cache frees samplers only after their last
  // submission completes.
  struct SamplerCacheEntry {
    std::array<uint32_t, 6> fetch{};
    uint32_t binding = UINT32_MAX;
    int32_t anisotropic_override = 0;
    bool force_trilinear = false;
    VulkanTextureCache::SamplerParameters parameters;
    VkSampler sampler = VK_NULL_HANDLE;
    uint64_t submission = 0;
  };
  std::vector<SamplerCacheEntry> sampler_cache_vertex_;
  // Sampler parameters by fetch words, binding filters and the filtering
  // cvars (a pure function of them), direct-mapped, behind the per-slot cache:
  // a slot whose sampler changed from the last draw usually finds a state seen
  // before.
  struct SamplerParametersMemo {
    std::array<uint32_t, 6> fetch{};
    uint32_t binding = UINT32_MAX;
    int32_t anisotropic_override = 0;
    bool force_trilinear = false;
    VulkanTextureCache::SamplerParameters parameters;
  };
  static constexpr size_t kSamplerParametersMemoCount = 1024;
  // PD-3.4: the fetch words as the sampler parameters read them. The base
  // and mip addresses matter only by whether their pages are zero (mips and
  // the base level present), so a texture moving to another address keeps
  // its slot's sampler.
  static void SamplerFetchKey(const uint32_t* fetch, uint32_t (&key)[6]) {
    for (uint32_t word = 0; word < 6; ++word) key[word] = fetch[word];
    key[1] = (fetch[1] & 0xFFF) | (((fetch[1] >> 12) & 0x1FFFF) ? 0x1000 : 0);
    key[5] = (fetch[5] & 0xFFF) | (((fetch[5] >> 12) & 0x1FFFF) ? 0x1000 : 0);
  }
  struct SamplerCensus {
    uint64_t slots = 0, slot_hits = 0, memo_hits = 0, use_sampler = 0, verified_different = 0;
  } sampler_census_;
  std::unique_ptr<SamplerParametersMemo[]> sampler_parameters_memos_{
      new SamplerParametersMemo[kSamplerParametersMemoCount]};
  // The last texture descriptor set written per stage (vertex, pixel) and its
  // contents; transient sets stay alive until their frame completes.
  struct LastTextureDescriptorSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t frame = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint32_t texture_count = 0;
    uint32_t sampler_count = 0;
    std::vector<VkDescriptorImageInfo> infos;
  };
  LastTextureDescriptorSet last_texture_descriptor_sets_[2];
  // A configured pipeline, reused while the memo state (StateMemoKey) and the
  // other inputs of the pipeline description are the same.
  struct PipelineMemo {
    uint64_t state_epoch = 0;
    const void* vertex_translation = nullptr;
    const void* pixel_translation = nullptr;
    uint32_t host_primitive_type = 0;
    uint32_t tessellation_mode = 0;
    bool host_primitive_reset_enabled = false;
    uint32_t normalized_depth_control = 0;
    uint32_t normalized_color_mask = 0;
    uint32_t render_pass_key = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VulkanPipelineCache::PipelineLayoutProvider* layout = nullptr;
    void* handle = nullptr;
  };
  // Direct-mapped by the memo state and the entry's main inputs: with
  // gpu_state_hash_memos the state is the register content hash, so a draw
  // finds the entry of any earlier draw in the same state (last frame's same
  // draw, typically), otherwise the epoch and only the last draw's.
  static constexpr size_t kStateMemoCount = 4096;
  static size_t StateMemoSlot(uint64_t state, uint64_t a, uint64_t b) {
    uint64_t x = (state ^ a * UINT64_C(0x9E3779B97F4A7C15) ^ b * UINT64_C(0xC2B2AE3D27D4EB4F)) *
                 UINT64_C(0xBF58476D1CE4E5B9);
    return size_t(x >> 52) & (kStateMemoCount - 1);
  }
  uint64_t StateMemoKey() const;
  std::unique_ptr<PipelineMemo[]> pipeline_memos_{new PipelineMemo[kStateMemoCount]};
  // The last draw's, to prefetch the next draw's pipeline memo entry.
  uint32_t last_pipeline_render_pass_key_ = 0;
  mutable std::atomic<ui::vulkan::VulkanDevice*> vulkan_device_cache_{nullptr};
  // Draws issued since the draw counter was last updated (at each swap): the
  // counter is a thread-local lookup in another module.
  uint32_t pending_draw_calls_ = 0;
  // Shader modifications and translations by memo state.
  struct TranslationMemo {
    uint64_t state_epoch = 0;
    const void* vertex_shader = nullptr;
    const void* pixel_shader = nullptr;
    uint32_t host_vertex_shader_type = 0;
    // Both modifications read whether the guest primitive is a point list.
    uint32_t guest_prim_type = 0;
    uint32_t interpolator_mask = 0;
    uint32_t ps_param_gen_pos = 0;
    uint32_t depth_control = 0;
    SpirvShaderTranslator::Modification vertex_modification;
    SpirvShaderTranslator::Modification pixel_modification;
    VulkanShader::VulkanTranslation* vertex_translation = nullptr;
    VulkanShader::VulkanTranslation* pixel_translation = nullptr;
  };
  std::unique_ptr<TranslationMemo[]> translation_memos_{new TranslationMemo[kStateMemoCount]};
  // Host viewport information by memo state.
  struct ViewportMemo {
    uint64_t state_epoch = 0;
    const void* pixel_shader = nullptr;
    uint32_t depth_control = 0;
    uint32_t color_mask = 0;
    uint32_t resolution_scale = 0;
    double hud_squeeze = 0.0;
    draw_util::ViewportInfo info;
  };
  std::unique_ptr<ViewportMemo[]> viewport_memos_{new ViewportMemo[kStateMemoCount]};
  // Pixel texture sets are pushed rather than allocated and written when the
  // device has VK_KHR_push_descriptor and the set has at most
  // kMaxPushedTextureBindings bindings (the minimum maxPushDescriptors).
  static constexpr uint32_t kMaxPushedTextureBindings = 32;
  bool push_pixel_texture_descriptors_ = false;
  bool PixelTexturesPushed(size_t binding_count) const {
    return push_pixel_texture_descriptors_ && binding_count &&
           binding_count <= kMaxPushedTextureBindings;
  }
  // The last pushed pixel texture contents (set is unused).
  LastTextureDescriptorSet last_pushed_pixel_textures_;
  std::vector<SamplerCacheEntry> sampler_cache_pixel_;

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  bool in_render_pass_ = false;

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is VK_NULL_HANDLE in this case).
  // TODO(Triang3l): Change to a deferred compilation handle.
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  VkDescriptorBufferInfo
      current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferCount];
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  VkDescriptorSet current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  // Whether the descriptor sets currently bound to the command buffer - only
  // low bits for the descriptor set layouts that remained the same are kept
  // when changing the pipeline layout. May be out of sync with
  // current_graphics_descriptor_set_values_up_to_date_, but should be ensured
  // to be a subset of it at some point when it becomes important; bits for
  // non-existent descriptor set layouts may also be set, but need to be ignored
  // when they start to matter.
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];

  // System shader constants.
  SpirvShaderTranslator::SystemConstants system_constants_;
  // RR-2.3: the inputs of the register-derived system constants at the last
  // draw. While they hold, only the per-texture fields are derived again.
  struct SystemConstantsMemoKey {
    uint64_t state_epoch;
    uint32_t words[24];
    bool operator==(const SystemConstantsMemoKey& other) const {
      return state_epoch == other.state_epoch &&
             std::memcmp(words, other.words, sizeof(words)) == 0;
    }
  };
  SystemConstantsMemoKey system_constants_memo_key_ = {};
  bool system_constants_memo_valid_ = false;
  uint64_t system_constants_memo_miss_words_[25] = {};
  uint64_t system_constants_memo_hits_ = 0, system_constants_memo_calls_ = 0,
           system_constants_memo_mismatches_ = 0, system_constants_memo_frames_ = 0;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
};

}  // namespace rex::graphics::vulkan
