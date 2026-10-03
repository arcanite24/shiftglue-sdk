#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <rex/graphics/fh1_depth_overwrite.h>
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
#include <rex/graphics/fh1_executor_counters.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;
class VulkanSharedMemory;
class VulkanTextureCache;

// Host configuration the executor shares with the pipelines it draws with.
struct Fh1VulkanExecutorConfig {
  bool msaa_2x_supported = true;
  bool gamma_as_unorm16 = false;
  bool depth_float24_round = false;
  // 16_16[_16_16] host targets are snorm limited to -1...1.
  bool fixed16_truncated = true;
  VulkanSharedMemory* memory = nullptr;
  VulkanTextureCache* textures = nullptr;
  // Formats of the host attachments the pipelines are created for.
  const VulkanRenderTargetCache* render_targets = nullptr;
};

// The FH1 native executor on Vulkan (NP-12.4), the D3D12 executor's
// counterpart over the same API-agnostic core: every guest draw goes into a
// native surface (an image) whose EDRAM tile ownership, clears and resolves
// the executor tracks from the guest registers. Ownership transfers and
// resolves use the same shaders as D3D12, compiled to SPIR-V. Guest draws
// render with dynamic rendering into the surfaces BindTargets selects.
// Scales 1x to 4x; the GPU profile (fh1_native_gpu_profile) times its phases
// and the texture loads as on D3D12.
class Fh1NativeExecutor {
 public:
  Fh1NativeExecutor(VulkanCommandProcessor& command_processor, const RegisterFile& register_file,
                    memory::Memory& memory);
  ~Fh1NativeExecutor();

  bool Initialize(const Fh1VulkanExecutorConfig& config);
  void Shutdown();
  // Counts an event in the rendering stats (such as occlusion query ends).
  void CountEvent(const char* stat) { Count(stat); }

  // Before the targets are bound for a draw: derives the surfaces the draw
  // writes and takes their EDRAM tiles, transferring the previous owners'
  // contents.
  void PrepareTargets(const Fh1DrawInfo& draw);
  // The render pass key the draw's pipeline is created for (formats and
  // samples of the surfaces PrepareTargets derived). False skips the draw.
  bool BindTargets(VulkanRenderTargetCache::RenderPassKey& key_out);
  // Enters dynamic rendering with the bound surfaces (after all barriers).
  void BeginDrawRendering();
  void NativeDrawIssued(const Fh1DrawInfo& draw);
  // In place of the render target cache's resolve.
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  // Copies one-off resolve read-backs into guest memory, waiting for the GPU.
  void FlushResolveReadbacks();
  void OnSwap(uint64_t frame);
  void LogStats(uint64_t frame);
  // Texture loads (untile and copy) timed with the GPU profile, split by
  // whether a resolve wrote the texture's memory.
  uint32_t BeginTextureLoadGpuTiming() { return GpuBegin(); }
  void EndTextureLoadGpuTiming(uint32_t begin, bool resolve_sourced) {
    GpuEnd(resolve_sourced ? kGpuTextureReloads : kGpuTextureLoads, begin);
  }

 private:
  using SurfaceKey = Fh1SurfaceKey;
  struct Rect {
    int32_t left, top, right, bottom;
  };
  struct Surface {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // Attachment view in the draw format.
    VkImageView view = VK_NULL_HANDLE;
    // Attachment and sampled view of the raw channel bits (formats with 16-bit
    // or 32-bit float channels), or null.
    VkImageView uint_view = VK_NULL_HANDLE;
    // Sampled views: color (raw bits when uint_view exists), or depth and
    // stencil aspects.
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkImageView stencil_view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat uint_format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
    SurfaceKey key;
    bool stencil_nonzero = false;
    uint32_t samples = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    // Clears not recorded yet (fh1_fold_clears), in order: folded into the
    // next draw rendering on this surface, or recorded in a rendering of
    // their own before anything else uses the image.
    struct PendingClear {
      VkClearAttachment clear;
      VkClearRect rect;
    };
    std::vector<PendingClear> pending_clears;
  };
  struct SourceRect {
    Surface* surface = nullptr;
    Rect rect = {};
    bool unowned = false;
  };
  struct CopyPlan : Fh1ResolvePlan {
    std::vector<SourceRect> sources;
  };
  using TileRun = Fh1EdramTiles::Run;
  static constexpr uint32_t kNoOwner = Fh1EdramTiles::kNoOwner;
  static constexpr uint32_t kTransferDestUint = 16;
  // Depth and stencil in one pass from the source (shader stencil export).
  static constexpr uint32_t kTransferDestDepthStencil = 17;
  static constexpr uint32_t kTransferSourceWords = 3;
  struct TransferPipelineKey {
    uint32_t dest_kind;  // 0 color, 1 depth, 2 + bit: stencil bit, kTransferDestUint
    VkFormat dest_format;
    uint32_t dest_samples;
    uint32_t sample_mask;
    uint32_t source_kind;  // 0 color, 1 depth, 2 raw color bits, 3 EDRAM words
    bool source_msaa;
    auto tie() const {
      return std::tie(dest_kind, dest_format, dest_samples, sample_mask, source_kind,
                      source_msaa);
    }
    bool operator<(const TransferPipelineKey& other) const { return tie() < other.tie(); }
  };
  struct PendingTransfer {
    uint32_t dest;
    uint32_t source;
    Rect rect;
    bool stencil;
  };
  struct PendingReadback {
    uint32_t address;
    uint32_t length;
    VkBuffer buffer;
    VkDeviceMemory memory;
  };
  struct ResolveReadback {
    uint64_t last_used_frame = 0;
    uint64_t run_start_frame = 0;
    uint64_t new_since_frame = 0;
  };

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const;
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format);
  uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const;
  Surface* GetOrCreateSurface(const SurfaceKey& key);
  Surface* FindSurface(uint32_t packed_key);
  void DestroySurface(Surface& surface);
  // Layout, stage and access transition (a barrier pushed to the command
  // processor).
  void Transition(Surface& surface, VkImageLayout layout, VkPipelineStageFlags stage,
                  VkAccessFlags access);
  void TransitionForSampling(Surface& surface, VkPipelineStageFlags stage);
  // With open_rendering_id, a surface that is already an attachment of that
  // rendering, if it is still open, needs no barrier: its writes stay in
  // rasterization order within one rendering.
  void TransitionForAttachment(Surface& surface, uint64_t open_rendering_id = 0);
  // The dynamic rendering scope of a draw to the prepared targets.
  uint64_t DrawRenderingId(uint32_t used_bits) const;

 public:
  // gpu_buffer_replay: after PrepareTargets, whether the draw's targets are
  // those of the open rendering `rendering_id` with no clear pending, so its
  // recorded commands can follow without binding or beginning anything.
  bool PreparedTargetsContinueRendering(uint64_t rendering_id);

 private:
  // BindTargets relied on the open rendering; if it ends before the draw
  // begins its rendering, the barriers are made there.
  bool attachment_barriers_skipped_ = false;
  VkImageAspectFlags AspectMask(const Surface& surface) const;

  void ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key, bool transfer = true);
  void MarkTileStencil(uint32_t base, uint32_t length, bool nonzero);
  bool DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const;
  bool ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length, bool stencil_overwritten,
                                bool stencil_written);
  void ClaimOverwrittenDepthTiles(const SurfaceKey& key, const std::array<int32_t, 4>& rect,
                                  bool stencil_overwritten);
  void ClaimTileRect(const SurfaceKey& key, uint32_t column_first, uint32_t row_first,
                     uint32_t column_end, uint32_t row_end);
  void TransferTiles(Surface& dest, const TileRun& run);
  void TransferRects(Surface& dest, uint32_t previous_owner, const Rect* rects,
                     uint32_t rect_count, uint32_t tile_count, bool tiles_stencil);
  void FlushTransfers();
  void FlushColorTransfers(Surface& dest, size_t first, size_t end);
  void FlushDepthTransfers(Surface& dest, size_t first, size_t end);
  void FlushDepthTransfersExported(Surface& dest, size_t first, size_t end);
  bool EnsureTransferWords(const Surface& dest);
  uint32_t LayoutConstant(const Surface& surface) const;
  uint32_t TransferFlags() const;
  Rect HostRect(const Rect& rect) const;

  void GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0, int32_t x1,
                         int32_t y1, std::vector<SourceRect>& sources_out);
  bool PlanCopy(CopyPlan& plan);
  bool Resolve(uint32_t* written_address, uint32_t* written_length);
  // Resolves into `buffer` (the shared memory buffer, or at scale the
  // texture cache's scaled resolve buffer) bound from `memory_offset`.
  // unscaled_dest writes the guest layout from each guest pixel's first host
  // pixel, for the guest memory copy of a resolve at scale.
  bool ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch, VkBuffer buffer, VkDeviceSize memory_offset,
                       VkDeviceSize memory_range, bool unscaled_dest = false,
                       VkImageView image_view = VK_NULL_HANDLE, uint32_t image_row = 0,
                       uint32_t image_endian = 0);
  void ClearSurfaceRect(Surface& surface, const Rect& guest_rect, uint32_t clear_value,
                        uint32_t clear_value_lo);
  // Records the surface's pending clears in a rendering of their own.
  void FlushPendingClears(Surface& surface);
  // Folds the pending clears of the draw's surfaces into its rendering of
  // width x height: those that do not fit are flushed before it begins
  // (before_begin), the rest recorded inside it (after the rendering began).
  // A full-area clear of a new rendering becomes its load op (colors and
  // depth are the rendering's attachment infos, null for an open rendering).
  void FoldPendingClears(Surface* const* bound, uint32_t width, uint32_t height,
                         VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo* depth,
                         bool before_begin);
  // Surfaces with pending clears.
  uint32_t pending_clear_surfaces_ = 0;
  bool IsOneOffResolve(uint32_t address, uint32_t length);
  void QueueResolveReadback(uint32_t address, uint32_t length);
  void DumpResolveOutput(uint32_t address, uint32_t length);

  // Dynamic rendering into one surface for transfers and clears.
  void BeginSurfaceRendering(Surface& surface, bool uint_view);

  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
  VkPipeline GetTransferPipeline(const TransferPipelineKey& key);
  VkPipeline GetComputePipeline(bool words, uint32_t source_kind, bool msaa);
  VkShaderModule GetShaderModule(const uint32_t* code, size_t size_bytes);

  void Skip(const char* reason) { counters_.Skip(reason); }
  void Count(const char* stat) { counters_.Count(stat); }

  VulkanCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  DrawExtentEstimator draw_extent_estimator_;
  Fh1DepthOverwrite depth_overwrite_;
  Fh1VulkanExecutorConfig config_;
  Fh1EdramTiles tiles_;
  Fh1ExecutorCounters counters_;
  std::map<uint32_t, Surface> surfaces_;
  // Direct-mapped front for FindSurface: map nodes stay put until erased,
  // and DestroySurface clears it.
  struct SurfaceFront {
    uint32_t packed_key = UINT32_MAX;
    Surface* surface = nullptr;
  };
  SurfaceFront surface_front_[16];
  uint32_t scale_ = 1;
  bool initialized_ = false;

  // Descriptor set layouts (set 0): source images (bindings 16 and 17) with
  // the storage buffer (binding 32) for compute, the source images for
  // transfer passes, and the EDRAM words (binding 16) for depth passes.
  VkDescriptorSetLayout compute_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout transfer_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout words_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout words_pipeline_layout_ = VK_NULL_HANDLE;
  // Resolves also written into a texture: the compute bindings and a storage
  // image (binding 33), with two more constants.
  VkDescriptorSetLayout image_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout image_pipeline_layout_ = VK_NULL_HANDLE;
  // [source kind][msaa]
  VkPipeline image_pipelines_[3][2] = {};
  std::vector<TextureCache::DirectResolveTarget> direct_resolve_targets_;
  // A descriptor pool per frame in flight, reset when the frame reopens.
  static constexpr uint32_t kDescriptorPoolFrames = 3;
  std::array<std::vector<VkDescriptorPool>, kDescriptorPoolFrames> descriptor_pools_;
  std::array<uint64_t, kDescriptorPoolFrames> descriptor_pool_frames_ = {};
  std::array<uint32_t, kDescriptorPoolFrames> descriptor_pool_current_ = {};
  std::map<const uint32_t*, VkShaderModule> shader_modules_;
  std::map<TransferPipelineKey, VkPipeline> transfer_pipelines_;
  // [words][source kind][msaa]
  VkPipeline compute_pipelines_[2][3][2] = {};

  VkBuffer transfer_words_ = VK_NULL_HANDLE;
  VkDeviceMemory transfer_words_memory_ = VK_NULL_HANDLE;
  VkDeviceSize transfer_words_size_ = 0;
  // Outgrown buffers may still be in use by submitted commands.
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> retired_buffers_;

  std::vector<PendingTransfer> pending_transfers_;
  std::vector<PendingReadback> pending_readbacks_;
  std::map<uint64_t, ResolveReadback> resolve_readbacks_;

  // Targets PrepareTargets derived for the draw BindTargets binds.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  uint32_t bound_bits_ = 0;
  // Identifies the attachments of the current dynamic rendering scope for the
  // command processor (0: none).
  uint64_t rendering_id_ = 0;
  // Draws in the open draw rendering (fh1_debug_rendering_split_draws).
  uint32_t rendering_draws_ = 0;
  uint64_t next_rendering_id_ = 1;
  struct PrepareSignature {
    uint64_t generation = 0;
    uint32_t used_bits = 0;
    uint32_t length_tiles = 0;
    bool stencil_written = false;
    uint32_t keys[5] = {};
    bool operator==(const PrepareSignature&) const = default;
    bool SameTargets(const PrepareSignature& other) const {
      return generation == other.generation && used_bits == other.used_bits &&
             stencil_written == other.stencil_written &&
             !std::memcmp(keys, other.keys, sizeof(keys));
    }
  };
  PrepareSignature recent_prepares_[4];
  // The last completed preparation by register state epoch: with the same
  // state, shaders and tiles, its targets and claims still hold.
  struct PrepareMemo {
    uint64_t state_epoch = 0;
    const Shader* vertex_shader = nullptr;
    uint32_t depth_control = 0;
    uint32_t color_mask = 0;
    bool rasterization_done = false;
    uint64_t generation = 0;
    uint32_t used_bits = 0;
    SurfaceKey keys[5];
  };
  PrepareMemo prepare_memo_;
  void PrepareTargetsImpl(const Fh1DrawInfo& draw);
  uint32_t recent_prepare_next_ = 0;

  uint64_t frame_ = 0;
  // fh1_scaled_msaa_single_sample at a scale above 1x.
  bool single_sample_msaa_ = false;
  // The device exports stencil from shaders: depth transfers take one pass.
  bool stencil_export_ = false;
  // The layout constant's host sample mode: 0 native, 1 native 2x, 2 2x
  // stored as 4x, 3 every guest sample in one host sample.
  static uint32_t HostSampleMode(const Surface& surface);

  // fh1_native_gpu_profile: GPU time per phase, and from a frame's first
  // timed phase to its swap, from timestamps in one query range per frame,
  // read once the frame's submission has completed.
  enum GpuPhase {
    kGpuTransfers,
    kGpuResolves,
    kGpuClears,
    kGpuTextureReloads,
    kGpuTextureLoads,
    kGpuFrame,
    kGpuPhases
  };
  static constexpr uint32_t kGpuProfileSlots = 4;
  static constexpr uint32_t kGpuProfileQueries = 2048;
  struct GpuProfileSlot {
    uint32_t used = 0;
    uint64_t submission = 0;
    bool pending = false;
    // Phase, first query, last query.
    std::vector<std::array<uint32_t, 3>> spans;
  };
  uint32_t GpuBegin();
  void GpuEnd(GpuPhase phase, uint32_t begin);
  void GpuEndFrame();
  void GpuDrain();
  class GpuTimer {
   public:
    GpuTimer(Fh1NativeExecutor& executor, GpuPhase phase)
        : executor_(executor), phase_(phase), begin_(executor.GpuBegin()) {}
    ~GpuTimer() { executor_.GpuEnd(phase_, begin_); }

   private:
    Fh1NativeExecutor& executor_;
    GpuPhase phase_;
    uint32_t begin_;
  };
  VkQueryPool gpu_query_pool_ = VK_NULL_HANDLE;
  std::array<GpuProfileSlot, kGpuProfileSlots> gpu_slots_;
  uint32_t gpu_slot_ = 0;
  std::array<uint64_t, kGpuPhases> gpu_ticks_{};
  uint64_t gpu_frames_ = 0;
  uint64_t draws_ = 0;
  uint64_t resolves_ = 0;
  // Tile passes per source -> destination surface pair while profiling.
  std::map<std::string, uint64_t> transfer_volume_;
};

}  // namespace rex::graphics::vulkan
