#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rex/graphics/fh1_depth_overwrite.h>
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
#include <rex/graphics/fh1_executor_counters.h>
#include <rex/graphics/fh1_frame_inspector.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/thread.h>
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
  // fh1_untile_predicated_tiling: the screen height in guest pixels, or 0.
  uint32_t untile_height = 0;
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

  // Writes these guest bytes, as the GPU has them now, to fh1_resolve_dump_dir.
  void DumpResolveOutput(uint32_t address, uint32_t length);

  bool Initialize(const Fh1VulkanExecutorConfig& config);
  void Shutdown();
  // Counts an event in the rendering stats (such as occlusion query ends).
  void CountEvent(const char* stat) { Count(stat); }
  // fh1_untile_predicated_tiling: after the last tile, the bands the later
  // tiles resolved are copied down to the EDRAM rows of their surfaces, where
  // the last tile leaves them on the console.
  void EndUntiledTiling();
  // The host sample count of surfaces the guest renders with this many
  // samples (fh1_msaa_single_sample, fh1_msaa_2x).
  // LS-4.5 (fh1_share_msaa_aliases): FH1 reads its 1280x720 1x depth (and
  // other 1x surfaces at pitch 16) as 640x360 4x MSAA surfaces at the same
  // EDRAM base: guest 4x pixel (x, y) sample s is 1x pixel (2x + (s & 1),
  // 2y + (s >> 1)). Such a 4x surface is stored as its 1x surface, and its
  // draws render into it single-sampled at twice the draw resolution scale,
  // so switching between the views transfers nothing.
  bool IsSharedMsaaAlias(uint32_t msaa, uint32_t pitch_tiles) const {
    return share_msaa_aliases_ && msaa == uint32_t(xenos::MsaaSamples::k4X) &&
           pitch_tiles == kSharedMsaaAliasPitchTiles;
  }
  // The current draw's targets are a shared 4x alias (rendered at twice the
  // scale with one sample).
  bool DrawAsDoubledScale() const;
  xenos::MsaaSamples HostMsaaSamples(xenos::MsaaSamples guest) const {
    if (guest == xenos::MsaaSamples::k1X || single_sample_msaa_) return xenos::MsaaSamples::k1X;
    return two_sample_msaa_ && guest == xenos::MsaaSamples::k4X ? xenos::MsaaSamples::k2X
                                                                 : guest;
  }

  // Before the targets are bound for a draw: derives the surfaces the draw
  // writes and takes their EDRAM tiles, transferring the previous owners'
  // contents.
  void PrepareTargets(const Fh1DrawInfo& draw);
  // PD-3.5 census: target preparations and the memo hits among them.
  uint64_t prepare_calls() const { return prepare_calls_; }
  uint64_t prepare_memo_hits() const { return prepare_memo_hits_; }
  // The render pass key the draw's pipeline is created for (formats and
  // samples of the surfaces PrepareTargets derived). False skips the draw.
  bool BindTargets(VulkanRenderTargetCache::RenderPassKey& key_out);
  // After BindTargets: the draw's fragment shading rate, coarser than 1x1 in
  // the renderings fh1_coarse_shading names.
  VkExtent2D draw_shading_rate() const { return draw_shading_rate_; }
  // Enters dynamic rendering with the bound surfaces (after all barriers).
  void BeginDrawRendering();
  // Local one-frame shader identities and the actual bound render surfaces.
  void DiscoverShaderDraw(const VulkanShader::VulkanTranslation* vertex,
                          const VulkanShader::VulkanTranslation* pixel);
  // After BindTargets: the draw has no depth, stencil or color target to use.
  bool BoundNothing() const { return bound_bits_ == 0; }
  void CountDrawSkippedWithoutEffect() { Count("draw_skipped_without_effect"); }
  void NativeDrawIssued(const Fh1DrawInfo& draw);
  // In place of the render target cache's resolve.
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  // Copies one-off resolve read-backs into guest memory, waiting for the GPU.
  void FlushResolveReadbacks();
  void OnSwap(uint64_t frame);
  void BeginInspection();
  void InspectTextures(uint32_t mask);
  bool inspection_active() const { return inspection_ != nullptr; }
  const Fh1VulkanExecutorConfig& config() const { return config_; }
  // present_hdr: before PrepareTargets, notes the scene's 2_10_10_10 target
  // and, at the first HUD draw over it (through its 2_10_10_10_AS_10_10_10_10
  // view), copies it, so the swap can tell the HUD from the scene.
  void SetHudSnapshotEnabled(bool enabled) { hud_snapshot_enabled_ = enabled; }
  // Whether the draw MaybeSnapshotBeforeHud last saw is the HUD: drawn into
  // the front buffer's 2_10_10_10_AS_10_10_10_10 view after the scene was
  // composited there this frame (menus over a game world included).
  bool IsHudDraw() const { return hud_draw_; }
  // Whether render target 0 is that HUD view now, before
  // MaybeSnapshotBeforeHud sees the draw (for its samplers).
  bool IsHudTarget() const;
  void MaybeSnapshotBeforeHud(const Fh1DrawInfo& draw);
  // After OnSwap: the swapped frame's snapshot, made readable from compute
  // shaders, if it covers width x height; null otherwise.
  VkImageView TakeHudSnapshot(uint32_t width, uint32_t height);

  // UP-1.3: the camera and depth a temporal upscaler needs (fh1_camera.cpp).
  // While enabled, every scene draw's world-view-projection (vertex float
  // constants c128-c131, one row each) is noted, and the scene depth is
  // copied at the first HUD draw. At the swap, the static world (the
  // largest group of draws sharing one matrix) is matched with the previous
  // frame's by object, which gives the camera's motion without the world
  // matrix: for static geometry WVP_prev * WVP_cur^-1 = VP_prev * VP_cur^-1.
  struct Camera {
    // Current clip space to the previous frame's, row-major.
    float reprojection[16];
    float near_plane;
    float far_plane;
    // Vertical field of view, radians.
    float fov_y;
    // No continuation of the previous frame (a cut, a load, a rebase): the
    // reprojection is the identity and the upscaler's history should go.
    bool cut;
    // The frame's jitter in render pixels, in the upscalers' convention
    // (UpscalerFrame).
    float jitter_x;
    float jitter_y;
  };
  void SetCameraTrackingEnabled(bool enabled) { camera_tracking_enabled_ = enabled; }
  // After OnSwap: the swapped frame's depth (guest z in [0, 1], R32_SFLOAT,
  // readable from compute shaders) and camera, if the depth covers width x
  // height; null otherwise.
  VkImageView TakeCameraFrame(uint32_t width, uint32_t height, Camera& camera_out);
  // UP-1.5: jitter the scene with a Halton(2, 3) sequence of this length
  // from the next frame (0: none).
  void SetJitterPhases(uint32_t phases) { jitter_phases_ = phases; }
  // Whether the current draw goes to the scene's color and depth surfaces,
  // and if so this frame's jitter, in host pixels of the surface.
  bool GetSceneJitter(float& x, float& y) const;
  // UP-1.7 per-object motion: scene draws write a slot per distinct object
  // reprojection to the object slot image, and the swap pass reprojects
  // each pixel's depth with its slot's matrix.
  static constexpr uint32_t kMaxObjectSlots = 256;
  void SetObjectMotionEnabled(bool enabled) { object_motion_enabled_ = enabled; }
  bool ObjectMotionEnabled() const { return object_motion_enabled_; }
  // Whether the current draw should write its slot: a draw testing and
  // writing the scene's depth whose vertex shader reads the camera's matrix
  // or an object's own (DrawObjectMatrixRegister).
  bool IsSceneObjectDraw(const Shader& vertex_shader) const;
  // The current draw's slot, after NoteCameraDraw (0: the camera's motion).
  uint32_t current_object_slot() const { return current_object_slot_; }
  // The full-frame row of the current surface's first row, in host pixels.
  int32_t ObjectRowOffset() const;
  // The swapped frame's reprojections by slot (row-major, slot 0 unused);
  // empty without object motion or on a cut.
  const std::vector<std::array<float, 16>>& object_reprojections_ready() const {
    return object_reprojections_ready_;
  }
  // Host depth to the camera depth copy's (float24 depth is kept halved).
  float camera_depth_scale() const { return camera_depth_scale_; }

  void LogStats(uint64_t frame);
  // Native surfaces alive and the device memory their images take (LS-0.4).
  size_t surface_count() const { return surfaces_.size(); }
  uint64_t surface_bytes() const { return surface_bytes_; }
  // EDRAM ownership transfers requested since startup (LS-3.3's census).
  uint64_t transfer_count() const { return transfer_count_; }
  // Texture loads (untile and copy) timed with the GPU profile, split by
  // whether a resolve wrote the texture's memory.
  uint32_t BeginTextureLoadGpuTiming() { return GpuBegin(); }
  void EndTextureLoadGpuTiming(uint32_t begin, bool resolve_sourced) {
    GpuEnd(resolve_sourced ? kGpuTextureReloads : kGpuTextureLoads, begin);
  }
  // fh1_native_gpu_profile: the frame from its opening to its swap's end.
  void GpuFrameOpened() {
    if (gpu_query_pool_ != VK_NULL_HANDLE) GpuBegin();
  }
  void GpuFrameSwapped() { GpuEndFrame(); }
  // fh1_native_gpu_profile: a draw rendering's span ends with the rendering,
  // so the work between renderings is not counted in it.
  void GpuRenderingEnded(const char* ender);
  // fh1_native_gpu_profile: other work's time by label (outside renderings).
  uint64_t BeginLabelGpuTiming(const char* label) {
    if (gpu_query_pool_ == VK_NULL_HANDLE) return UINT64_MAX;
    const uint32_t id = GpuLabel(label);
    return (uint64_t(id) << 32) | GpuBegin();
  }
  void EndLabelGpuTiming(uint64_t token) {
    if (token == UINT64_MAX || uint32_t(token) == UINT32_MAX) return;
    GpuEnd(GpuPhase(kGpuPhases + uint32_t(token >> 32)), uint32_t(token));
  }
  // fh1_visibility_census: an occlusion query around each draw of a sampled
  // frame, to count draws that pass no samples (the item-2 ceiling).
  uint32_t VisBegin(const Fh1DrawInfo& draw, reg::RB_DEPTHCONTROL depth,
                    uint32_t vertex_count);
  // fh1_visibility_census_no_depth: census frames draw without the depth
  // test, so draws that still pass no samples are off screen (or discard).
  bool VisNoDepth() const;
  bool VisActive() const { return vis_query_pool_ != VK_NULL_HANDLE; }
  // fh1_visibility_oracle_skip (item-2 ceiling, renders wrong): the draw
  // with this key (and ordinal in the frame) passed no samples at its last
  // census, so it is dropped before its derivation.
  bool VisOracleSkip(uint64_t key);
  void VisEnd(uint32_t query);

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
    // The EDRAM addressing period's height; the image may be taller
    // (fh1_untile_predicated_tiling), its rows past the period host-only.
    uint32_t height = 0;
    // The rows the image holds. With fh1_surface_rows_on_demand it starts at
    // what the first use claims and grows when a claim reaches further, up
    // to the period (or the untile height); rows no claim reached are never
    // read, since tile ownership routes every read to a claimed row.
    uint32_t image_height = 0;
    // The image's memory requirement.
    uint64_t bytes = 0;
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
  void FinishInspection(uint64_t frame);
  void DestroyInspector();
  Fh1InspectionImage QueueInspectionImage(Surface& surface, Rect rect);
  struct CopyPlan;
  void InspectResolve(const CopyPlan& plan, uint32_t untile_band);
  std::shared_ptr<Fh1FrameInspection> inspection_;
  uint64_t inspection_generation_ = 0;
  std::map<uint32_t, uint32_t> inspection_draws_;
  std::map<uint64_t, Fh1InspectionTexture> inspection_textures_;
  struct InspectionReadback {
    VkBuffer buffer;
    VkDeviceMemory memory;
  };
  std::vector<InspectionReadback> inspection_readbacks_;
  VkDescriptorSetLayout inspection_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout inspection_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline inspection_pipelines_[2] = {};
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
    // Specialized for the constants: the layouts but their base tiles, flags.
    bool specialized = false;
    std::array<uint32_t, 3> spec = {};
    auto tie() const {
      return std::tie(dest_kind, dest_format, dest_samples, sample_mask, source_kind,
                      source_msaa, specialized, spec);
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
    // In readback_buffer_.
    VkDeviceSize offset;
  };
  struct DeferredReadback {
    uint32_t address;
    uint32_t length;
    // In deferred_readback_buffer_.
    VkDeviceSize offset;
    uint64_t submission;
  };
  struct ResolveReadback {
    uint64_t last_used_frame = 0;
    uint64_t run_start_frame = 0;
    uint64_t new_since_frame = 0;
    // Resolves this frame (count_frame) and in the frame before it.
    uint64_t count_frame = UINT64_MAX;
    uint32_t count = 0;
    uint32_t previous_count = 0;
  };

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const;
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format);
  uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const;
  // The surface a key's contents are stored in: a shared 4x alias's 1x
  // surface (IsSharedMsaaAlias), otherwise the key's own.
  SurfaceKey StorageKey(SurfaceKey key) const {
    if (IsSharedMsaaAlias(key.msaa, key.pitch_tiles)) key.msaa = uint32_t(xenos::MsaaSamples::k1X);
    return key;
  }
  // rows: the guest rows the caller is about to claim (0: the whole period).
  Surface* GetOrCreateSurface(const SurfaceKey& key, uint32_t rows = 0);
  // Creates the image, its memory and views at surface.image_height.
  bool CreateSurfaceImage(Surface& surface);
  // Grows the image (copying what it holds) so it has at least `rows` guest rows.
  void EnsureSurfaceRows(Surface& surface, uint32_t rows);
  static uint32_t AlignSurfaceRows(uint32_t rows);
  // Guest rows a claim of tiles [base, base + length) needs in a surface.
  uint32_t ClaimRows(const SurfaceKey& key, uint32_t base, uint32_t length) const;
  // Destroys images replaced by growth once their submissions completed.
  void ReleaseRetiredImages(bool all);
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
  // BindTargets relied on the open rendering; if it ends before the draw
  // begins its rendering, the barriers are made there.
  bool attachment_barriers_skipped_ = false;
  VkImageAspectFlags AspectMask(const Surface& surface) const;

  void ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key, bool transfer = true);
  void MarkTileStencil(uint32_t base, uint32_t length, bool nonzero);
  bool DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const;
  // rect_shift: the overwrite rectangles are in pixels of a shared 4x alias
  // (1) whose storage key is key, or of key itself (0).
  bool ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length, bool stencil_overwritten,
                                bool stencil_written, uint32_t rect_shift);
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
                       uint32_t image_endian = 0, VkImageView image2_view = VK_NULL_HANDLE,
                       uint32_t image2_endian = 0);
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
  // Small resolves the CPU reads every frame (the auto exposure's luminance,
  // #548): copied into a ring and written to guest memory once their
  // submission has completed, a frame or two late, never waiting for the GPU.
  void QueueDeferredReadback(uint32_t address, uint32_t length);
  void ApplyCompletedDeferredReadbacks();

  // Dynamic rendering into one surface for transfers and clears.
  void BeginSurfaceRendering(Surface& surface, bool uint_view);

  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
  VkPipeline GetTransferPipeline(const TransferPipelineKey& key);
  static uint32_t TransferKind(const TransferPipelineKey& key);
  bool GetTransferModules(const TransferPipelineKey& key, VkShaderModule& vertex_module,
                          VkShaderModule& fragment_module);
  // Touches no executor state but the layouts, so a worker may call it.
  VkPipeline CreateTransferPipeline(const TransferPipelineKey& key, VkShaderModule vertex_module,
                                    VkShaderModule fragment_module) const;
  VkPipeline GetComputePipeline(bool words, uint32_t source_kind, bool msaa);
  // A resolve pipeline specialized for the constants' format, MSAA, sample,
  // pack and endian bits, or null to use the generic one.
  // Specializes a transfer pipeline key for its constants, unless off or
  // past the cap of specialized pipelines.
  void SpecializeTransfer(TransferPipelineKey& key, const uint32_t* constants);
  VkPipeline GetSpecializedResolvePipeline(bool image, uint32_t source_kind, bool msaa,
                                           const uint32_t* constants);
  VkShaderModule GetShaderModule(const uint32_t* code, size_t size_bytes);

  // Specialized pipelines are built on a worker, and passes use the generic
  // pipeline until theirs is ready: a cold driver cache took up to 100 ms a
  // pipeline on a phone, hundreds of milliseconds a frame at a new scene.
  struct SpecializeJob {
    bool transfer = false;
    std::array<uint32_t, 9> resolve_key = {};
    TransferPipelineKey transfer_key = {};
    std::function<VkPipeline()> create;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  void QueueSpecialize(SpecializeJob job);
  void CollectSpecialized();
  void StopSpecializeThread();
  void SpecializeThread();

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
  uint64_t surface_bytes_ = 0;
  uint64_t transfer_count_ = 0;
  // Direct-mapped front for FindSurface: map nodes stay put until erased,
  // and DestroySurface clears it.
  struct SurfaceFront {
    uint32_t packed_key = UINT32_MAX;
    Surface* surface = nullptr;
  };
  SurfaceFront surface_front_[16];
  uint32_t scale_ = 1;
  // fh1_untile_predicated_tiling: the screen height the images hold, and the
  // last later-tile band resolved from each surface since the tiling began.
  uint32_t untile_height_ = 0;
  std::unordered_map<uint32_t, uint32_t> untile_bands_;
  bool initialized_ = false;
  // present_hdr's snapshot of the scene before the HUD: the scene target's
  // base this frame, whether this frame's copy was made, whether the frame
  // swapped next has one, and the copy (image, view, format, size, layout).
  bool hud_snapshot_enabled_ = false;
  uint32_t hud_scene_base_ = UINT32_MAX;
  bool hud_draw_ = false;
  bool hud_snapshot_taken_ = false;
  bool hud_snapshot_ready_ = false;
  Surface hud_snapshot_;
  void DestroyHudSnapshot();

  // UP-1.3 camera tracking (fh1_camera.cpp).
  struct CameraDraw {
    uint64_t object;
    uint32_t depth_key;
    uint64_t color_key;
    std::array<float, 16> wvp;
  };
  void NoteCameraDraw(const Fh1DrawInfo& draw);
  // Copies the scene depth's band at screen row band_row (predicated
  // tiling) or all of it (band_row 0, untiled) into camera_depth_.
  void SnapshotCameraDepth(uint32_t band_row);
  void FinishCameraFrame();
  bool EnsureCameraDepthPipelines();
  void DestroyCamera();
  bool camera_tracking_enabled_ = false;
  std::vector<CameraDraw> camera_draws_;
  // The previous frame's scene objects and their matrices.
  std::unordered_map<uint64_t, std::array<float, 16>> camera_previous_objects_;
  Camera camera_ready_ = {};
  bool camera_ready_valid_ = false;
  // The previous frame's scene depth: the one most matrix draws tested,
  // and the color target those draws used most.
  uint32_t camera_scene_depth_ = 0;
  uint64_t camera_scene_color_ = 0;
  bool camera_scene_depth_valid_ = false;
  uint64_t CurrentColorKey() const;
  // Records the swapped frame's jitter in camera_ready_ and picks the next.
  void AdvanceJitter();
  // UP-1.5 jitter: the sequence length, the position in it, and this
  // frame's offset in surface pixels.
  uint32_t jitter_phases_ = 0;
  // Whether the current draw's vertex shader reads the camera matrix.
  bool draw_uses_camera_matrix_ = false;
  // UP-1.7: whether it is a car on the scene (SceneObjectMatrixRegister c0).
  bool draw_is_scene_car_ = false;
  // UP-1.7: this frame's reprojections by slot, the slots by matrix hash and
  // by object, and the swapped frame's table.
  bool object_motion_enabled_ = false;
  float camera_depth_scale_ = 1.0f;
  uint32_t current_object_slot_ = 0;
  std::vector<std::array<float, 16>> object_reprojections_;
  std::unordered_map<uint64_t, uint32_t> object_slot_by_matrix_;
  // Draws of each mesh so far this frame, and each draw's matrix by mesh
  // and order this frame and the last.
  std::unordered_map<uint64_t, uint32_t> object_occurrences_;
  std::unordered_map<uint64_t, std::array<float, 16>> object_current_wvp_;
  std::unordered_map<uint64_t, std::array<float, 16>> object_previous_wvp_;
  std::vector<std::array<float, 16>> object_reprojections_ready_;
  uint32_t AssignObjectSlot(uint64_t object, const std::array<float, 16>& wvp);
  // The first register of the draw's world-view-projection (c128 the
  // camera's, c0 a car's own), UINT32_MAX without one.
  uint32_t DrawObjectMatrixRegister(const Shader& vertex_shader) const;
  // As DrawObjectMatrixRegister, for draws testing and writing the scene's
  // depth while the camera is tracked; UINT32_MAX for others.
  uint32_t SceneObjectMatrixRegister(const Shader& vertex_shader) const;
  uint32_t CurrentDepthKey() const;
  uint64_t DrawObjectKey(const Fh1DrawInfo& draw) const;
  uint32_t jitter_index_ = 0;
  float jitter_x_ = 0.0f;
  float jitter_y_ = 0.0f;
  // A scene draw since the last band copy.
  bool camera_depth_dirty_ = false;
  // This frame's scene depth copy (R32_SFLOAT), the screen rows it covers,
  // and those of the frame swapped next (0: none).
  Surface camera_depth_;
  uint32_t camera_depth_rows_ = 0;
  uint32_t camera_depth_rows_ready_ = 0;
  // The rows the bands need; the copy is resized to it between frames.
  uint32_t camera_depth_wanted_height_ = 0;
  bool camera_depth_failed_ = false;
  VkDescriptorSetLayout camera_depth_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout camera_depth_pipeline_layout_ = VK_NULL_HANDLE;
  // Single-sampled and multisampled sources.
  std::array<VkPipeline, 2> camera_depth_pipelines_ = {};
  VkDescriptorPool camera_depth_descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> camera_depth_sets_ = {};

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
  std::vector<TextureCache::DirectResolveTarget> direct_resolve_candidates_;
  std::vector<uint32_t> direct_resolve_conversions_;
  // A descriptor pool per frame in flight, reset when the frame reopens.
  static constexpr uint32_t kDescriptorPoolFrames = 3;
  std::array<std::vector<VkDescriptorPool>, kDescriptorPoolFrames> descriptor_pools_;
  std::array<uint64_t, kDescriptorPoolFrames> descriptor_pool_frames_ = {};
  std::array<uint32_t, kDescriptorPoolFrames> descriptor_pool_current_ = {};
  std::map<const uint32_t*, VkShaderModule> shader_modules_;
  std::map<TransferPipelineKey, VkPipeline> transfer_pipelines_;
  // [words][source kind][msaa]
  VkPipeline compute_pipelines_[2][3][2] = {};
  // Specialized resolves by {image, source kind, msaa, specialization constants}.
  std::map<std::array<uint32_t, 9>, VkPipeline> specialized_resolve_pipelines_;
  uint32_t specialized_transfer_pipelines_ = 0;
  bool async_specialize_ = false;
  std::unique_ptr<rex::thread::Thread> specialize_thread_;
  std::mutex specialize_mutex_;
  std::condition_variable specialize_cond_;
  std::deque<SpecializeJob> specialize_queue_;
  std::vector<SpecializeJob> specialize_done_;
  std::atomic<bool> specialize_has_done_{false};
  bool specialize_stop_ = false;

  VkBuffer transfer_words_ = VK_NULL_HANDLE;
  VkDeviceMemory transfer_words_memory_ = VK_NULL_HANDLE;
  VkDeviceSize transfer_words_size_ = 0;
  // Outgrown buffers may still be in use by submitted commands.
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> retired_buffers_;
  struct RetiredImage {
    uint64_t submission = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView views[4] = {};
  };
  std::vector<RetiredImage> retired_images_;

  std::vector<PendingTransfer> pending_transfers_;
  std::vector<PendingReadback> pending_readbacks_;
  // One persistently mapped buffer for all one-off readbacks: a dedicated
  // allocation per readback cost milliseconds each on Android (KGSL zeroes
  // new memory) in the bursts at gameplay start.
  VkBuffer readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
  VkDeviceSize readback_size_ = 0;
  VkDeviceSize readback_used_ = 0;
  const uint8_t* readback_mapped_ = nullptr;
  static constexpr VkDeviceSize kDeferredReadbackSize = 64 * 1024;
  std::deque<DeferredReadback> deferred_readbacks_;
  VkBuffer deferred_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory deferred_readback_memory_ = VK_NULL_HANDLE;
  const uint8_t* deferred_readback_mapped_ = nullptr;
  VkDeviceSize deferred_readback_next_ = 0;
  std::map<uint64_t, ResolveReadback> resolve_readbacks_;

  // Targets PrepareTargets derived for the draw BindTargets binds.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  uint32_t bound_bits_ = 0;
  std::ofstream shader_discovery_draws_;
  bool shader_discovery_started_ = false;
  std::set<std::string> shader_discovery_written_;
  // DrawRenderingId(bound_bits_), set by BindTargets for BeginDrawRendering.
  uint64_t bound_rendering_id_ = 0;
  // fh1_coarse_shading, parsed at swaps, and the rate of each draw rendering.
  void UpdateCoarseShading();
  std::string coarse_shading_spec_;
  std::vector<std::pair<std::string, VkExtent2D>> coarse_shading_;
  std::unordered_map<uint64_t, VkExtent2D> shading_rate_by_rendering_;
  VkExtent2D draw_shading_rate_ = {1, 1};
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
  // Slot 0 holds the last preparation; with state hashes (NL-3.2) the
  // preparations are direct-mapped by state.
  PrepareMemo prepare_memos_[16];
  uint64_t prepare_calls_ = 0, prepare_memo_hits_ = 0;
  void PrepareTargetsImpl(const Fh1DrawInfo& draw);
  uint32_t recent_prepare_next_ = 0;

  uint64_t frame_ = 0;
  // fh1_scaled_msaa_single_sample at a scale above 1x.
  bool single_sample_msaa_ = false;
  // fh1_share_msaa_aliases: 4x surfaces at this pitch are stored as their 1x
  // surfaces (IsSharedMsaaAlias).
  static constexpr uint32_t kSharedMsaaAliasPitchTiles = 16;
  bool share_msaa_aliases_ = false;
  bool two_sample_msaa_ = false;
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
  static constexpr uint32_t kGpuProfileSlots = 16;
  static constexpr uint32_t kGpuProfileQueries = 32768;
  struct GpuProfileSlot {
    uint32_t used = 0;
    uint64_t submission = 0;
    bool pending = false;
    // Drains that found the frame's queries unavailable.
    uint32_t retries = 0;
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
  // Frames whose queries never became available, since the last report.
  uint32_t gpu_dropped_frames_ = 0;
  std::array<uint64_t, kGpuPhases> gpu_ticks_{};
  // With the profile, the time from each draw rendering's start to the next
  // one's, by the rendering's surfaces (spans with phase kGpuPhases + label).
  std::vector<std::string> gpu_labels_;
  std::unordered_map<std::string, uint32_t> gpu_label_ids_;
  std::vector<uint64_t> gpu_label_ticks_;
  std::vector<uint64_t> gpu_label_counts_;
  uint32_t gpu_rendering_label_ = UINT32_MAX;
  uint32_t gpu_rendering_begin_ = UINT32_MAX;
  std::map<std::pair<uint32_t, std::string>, uint64_t> gpu_label_enders_;
  void GpuMarkRendering(uint32_t label);
  uint32_t GpuLabel(const std::string& label);
  static constexpr uint32_t kVisQueries = 16384;
  struct VisDraw {
    uint64_t key;
    uint32_t label;
    uint32_t vertices;
    uint32_t kind;
  };
  // By label and kind (0 depth tested and written with early depth, 1 the
  // same with late depth (the pixel shader kills or writes depth, or alpha
  // decides coverage), 2 tested only, 3 not tested): draws, draws with no
  // samples, draws under 64 samples, vertices, vertices of draws with no
  // samples.
  static constexpr uint32_t kVisKinds = 4;
  struct VisTotals {
    uint64_t v[kVisKinds][5] = {};
  };
  void VisDrain();
  void VisMarkRendering();
  VkQueryPool vis_query_pool_ = VK_NULL_HANDLE;
  bool vis_frame_ = false;
  bool vis_reset_ = false;
  bool vis_pending_ = false;
  uint64_t vis_submission_ = 0;
  uint32_t vis_retries_ = 0;
  uint32_t vis_label_ = UINT32_MAX;
  uint32_t vis_frames_ = 0;
  uint32_t vis_skipped_ = 0;
  std::vector<VisDraw> vis_draws_;
  std::vector<std::string> vis_labels_;
  std::unordered_map<std::string, uint32_t> vis_label_ids_;
  std::vector<VisTotals> vis_totals_;
  uint64_t vis_key_ = 0;
  std::unordered_map<uint64_t, uint32_t> vis_ordinals_;
  // Bit 0: no samples at the last census; bit 1: late depth.
  std::unordered_map<uint64_t, uint8_t> vis_oracle_;
  uint64_t vis_oracle_skips_ = 0;
  uint64_t vis_oracle_draws_ = 0;
  uint64_t gpu_frames_ = 0;
  uint64_t draws_ = 0;
  uint64_t resolves_ = 0;
  // Tile passes per source -> destination surface pair while profiling.
  std::map<std::string, uint64_t> transfer_volume_;
};

}  // namespace rex::graphics::vulkan
