#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <rex/graphics/d3d12/fh1_frame_dump.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12RenderTargetCache;
class D3D12SharedMemory;
class D3D12TextureCache;

// State of a consumed guest draw that the executor needs, captured by
// D3D12CommandProcessor::IssueDraw right after the Xenos draw was recorded.
struct Fh1NativeDrawInfo {
  bool indexed = false;
  uint32_t vertex_count = 0;
  bool memexport = false;
  bool occlusion_query_active = false;
  bool rasterization_done = false;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  uint32_t normalized_color_mask = 0;
  const Shader* vertex_shader = nullptr;
  const Shader* pixel_shader = nullptr;
  // Indices read from guest memory (DXGI_FORMAT_UNKNOWN otherwise).
  uint32_t guest_dma_index_offset = 0;
  uint32_t guest_dma_index_size = 0;
  DXGI_FORMAT guest_dma_index_format = DXGI_FORMAT_UNKNOWN;
};

// Host configuration the executor must share with the pipelines it reuses.
struct Fh1NativeExecutorConfig {
  bool msaa_2x_supported = true;
  bool gamma_as_unorm16 = false;
  bool bindless = true;
  bool depth_float24_round = false;
  // 16_16[_16_16] host targets are snorm limited to -1...1
  // (D3D12RenderTargetCache::IsFixed16TruncatedToMinus1To1).
  bool fixed16_truncated = true;
  // `native` mode: the executor is the only renderer. It uses the command
  // processor's guest-memory mirror and texture cache instead of its own, binds
  // its surfaces for the command processor's draws, and performs every clear
  // and resolve; nothing is compared with Xenos.
  bool presents = false;
  D3D12SharedMemory* memory = nullptr;
  D3D12TextureCache* textures = nullptr;
};

// XR-02/03/04 native executor, shadow mode. It replays every consumed guest
// draw into native surfaces with the pipeline and constants the Xenos path has
// just set, but with native resources: its own guest-memory mirror for vertex
// and index fetch, its own texture cache decoding that mirror, and native
// render surfaces whose EDRAM tile ownership, clears and resolves it tracks
// from the guest registers. Resolves write the guest texture layout into the
// native mirror, so later fetches of resolve output decode native data like
// any other guest texture. Xenos still presents; the native front buffer can
// be dumped for same-frame comparison, and native resolve bytes can be
// compared with Xenos's. Anything it cannot execute is counted by a named
// skip reason.
class Fh1NativeExecutor {
 public:
  Fh1NativeExecutor(D3D12CommandProcessor& command_processor, const RegisterFile& register_file,
                    memory::Memory& memory);
  ~Fh1NativeExecutor();

  // Session renderer (fh1_renderer): native-shadow or native.
  static bool Enabled();
  static bool Presents();

  bool Initialize(const Fh1NativeExecutorConfig& config);
  void Shutdown();

  // The executor's own mirror in shadow mode (none in native mode).
  D3D12SharedMemory* shared_memory() const { return owned_memory_.get(); }
  bool verifying() const { return verify_; }
  bool presents() const { return config_.presents; }

  // Submission and frame lifecycle, mirroring the Xenos caches.
  void CompletedSubmissionUpdated(uint64_t completed_submission);
  void BeginSubmission(uint64_t current_submission);
  void BeginFrame();
  void EndFrame();
  void ClearCache();
  // Guest texture fetch constants changed (register writes).
  void TextureFetchConstantsWritten(uint32_t first_index, uint32_t last_index);

  // Before the Xenos render targets are updated for a draw: derives the
  // surfaces the draw writes and takes their EDRAM tiles, transferring the
  // previous owners' contents like the guest's EDRAM aliasing does.
  void PrepareTargets(const Fh1NativeDrawInfo& draw);
  // Before the Xenos pipeline is bound: uploads the draw's vertex and index
  // ranges to the native mirror and loads its textures natively.
  void LogStats(uint64_t frame);
  void RecordCopyInputs() {
    if (frame_dump_) frame_dump_->RecordCopyInputs();
  }
  // Frame dumps: the guest ranges a draw reads, once the mirror holds them.
  void RecordDrawInputs(uint32_t used_texture_mask, const Shader& vertex_shader,
                        uint32_t guest_dma_index_offset, uint32_t guest_dma_index_size);
  void PrepareDraw(uint32_t used_texture_mask, const Shader& vertex_shader,
                   uint32_t guest_dma_index_offset, uint32_t guest_dma_index_size);
  // Right after the Xenos draw was recorded, with its state still bound.
  void ShadowDraw(D3D12RenderTargetCache& render_target_cache, const Fh1NativeDrawInfo& draw);
  // After the Xenos resolve of the same copy.
  void ShadowResolve(D3D12RenderTargetCache& render_target_cache, bool xenos_succeeded);

  // Native mode, in place of the render target cache's update: binds the
  // surfaces PrepareTargets derived and returns the bound slots and their
  // formats as RenderTargetCache::GetLastUpdateBoundRenderTargets would.
  // False skips the draw (the reason is counted).
  bool BindTargets(uint32_t& bound_bits, uint32_t* formats);
  // Native mode, after the command processor recorded the draw.
  void NativeDrawIssued(const Fh1NativeDrawInfo& draw);
  // Native mode, in place of the render target cache's resolve. Returns
  // whether the copy (or clear) ran, and the guest range it wrote.
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  // Before the guest CPU can observe GPU progress: copies the one-off resolve
  // read-backs recorded since into guest memory, waiting for the GPU once.
  void FlushResolveReadbacks();
  // Verification, before the Xenos resolve: the copy from the native owners
  // and from the Xenos render targets of the same keys, compared.
  void VerifySurfacesBeforeResolve(D3D12RenderTargetCache& render_target_cache);
  // Verification on dump frames, after both renderers took the draw's EDRAM
  // tiles and before either drew: separates transfer from draw differences.
  void VerifyTargetsBeforeDraw(D3D12RenderTargetCache& render_target_cache,
                               const Fh1NativeDrawInfo& draw);
  void ShadowSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width,
                  uint32_t height, const uint32_t* gamma_pwl);

 private:
  // Same fields as the Xenos RenderTargetKey so the two can be compared.
  struct SurfaceKey {
    uint32_t base_tiles = 0;
    uint32_t pitch_tiles = 0;  // At 32bpp.
    uint32_t msaa = 0;         // xenos::MsaaSamples.
    bool is_depth = false;
    uint32_t format = 0;  // DepthRenderTargetFormat or color resource format.
    uint32_t Pack() const {
      return base_tiles | (pitch_tiles << 11) | (msaa << 19) | (uint32_t(is_depth) << 21) |
             (format << 22);
    }
    static SurfaceKey Unpack(uint32_t packed) {
      SurfaceKey key;
      key.base_tiles = packed & 0x7FF;
      key.pitch_tiles = (packed >> 11) & 0xFF;
      key.msaa = (packed >> 19) & 0x3;
      key.is_depth = ((packed >> 21) & 1) != 0;
      key.format = (packed >> 22) & 0xF;
      return key;
    }
    bool Is64bpp() const {
      return !is_depth &&
             xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(format));
    }
    std::string Describe() const;
  };
  struct Surface {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
    // UINT render target view for formats kept as raw channel bits.
    D3D12_CPU_DESCRIPTOR_HANDLE uint_view = {};
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;
    SurfaceKey key;
    // Depth surfaces: whether any stencil value may be nonzero. Transfers
    // from a surface whose stencil is all zero skip the per-bit passes.
    bool stencil_nonzero = false;
    uint32_t samples = 1;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  struct PendingDump {
    uint64_t frame = 0;
    std::string prefix;
    uint64_t submission = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bool has_gamma_pwl = false;
    std::array<uint32_t, 128 * 3> gamma_pwl = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  };
  // Native and Xenos bytes of one resolve's destination range.
  struct PendingVerify {
    uint64_t submission = 0;
    uint64_t frame = 0;
    uint32_t start = 0;
    uint32_t length = 0;
    std::string kind;
    std::vector<uint8_t> cpu;  // Guest memory when queued.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;  // native, then Xenos
    // Surface verifies: only these texels of the tiled 32bpp layout count.
    uint32_t texels_width = 0, texels_height = 0, texels_pitch = 0;
  };
  // A rectangle of a resolve, in resolve surface pixels, whose tiles one
  // native surface owns.
  struct SourceRect {
    Surface* surface = nullptr;
    D3D12_RECT rect = {};
    // No surface: whether no surface owns the tiles, or the owner's surface is
    // gone.
    bool unowned = false;
  };

  static constexpr uint32_t kNoOwner = UINT32_MAX;

  // Contiguous claimed tiles that another surface owned.
  struct TileRun {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t previous_owner = kNoOwner;
  };
  struct TransferPipelineKey {
    uint32_t dest_kind;  // 0 color, 1 depth, 2 + bit: stencil bit, kTransferDestUint
    DXGI_FORMAT dest_format;
    uint32_t dest_samples;
    uint32_t sample_mask;
    uint32_t source_kind;  // 0 color, 1 depth, 2 raw color bits
    bool source_msaa;
    auto tie() const {
      return std::tie(dest_kind, dest_format, dest_samples, sample_mask, source_kind,
                      source_msaa);
    }
    bool operator<(const TransferPipelineKey& other) const { return tie() < other.tie(); }
  };

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const;
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format);
  static uint32_t PitchTiles(uint32_t pitch_pixels, uint32_t msaa);
  uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const;
  DXGI_FORMAT ColorResourceFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT ColorDrawFormat(xenos::ColorRenderTargetFormat format) const;
  Surface* GetOrCreateSurface(const SurfaceKey& key);
  Surface* FindSurface(uint32_t packed_key);
  // Claims tiles [base, base + length) (with EDRAM address wrapping) for key
  // and transfers the previous owners' contents into the surface.
  // Without transfer, the tiles change owner without their contents (the
  // caller overwrites them, as a resolve clear does).
  void ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key, bool transfer = true);
  // Whether the current draw's stencil state can leave a nonzero value.
  bool DrawMayWriteNonzeroStencil(reg::RB_DEPTHCONTROL depth_control) const;
  // A depth-only rectangle with an ALWAYS depth test, in guest pixels
  // [x0, y0, x1, y1): `inner` has the depth of every sample overwritten,
  // `outer` bounds every pixel the draw can touch.
  struct OverwriteRect {
    std::array<int32_t, 4> inner;
    std::array<int32_t, 4> outer;
  };
  // Finds the draw's rectangles by running the vertex shader on the CPU, into
  // overwrite_rects_. Whether stencil is rewritten too is returned separately.
  bool GetDepthOverwriteRects(const Fh1NativeDrawInfo& draw, bool& stencil_overwritten);
  // Claims only the depth tiles the rectangles touch: covered tiles without a
  // transfer when no stencil would be lost, edge tiles with one. False when
  // the rectangles do not fit the surface's claimed range.
  bool ClaimDepthOverwriteTiles(const SurfaceKey& key, uint32_t length, bool stencil_overwritten,
                                bool stencil_written);
  // Claims the depth tiles a depth-overwriting draw covers without transfers,
  // when no stencil would be lost.
  void ClaimOverwrittenDepthTiles(const SurfaceKey& key, const std::array<int32_t, 4>& rect,
                                  bool stencil_overwritten);
  std::vector<OverwriteRect> overwrite_rects_;
  // readback_resolve: copies of resolved ranges into guest RAM, as the
  // Vulkan backend does (the guest GPU writes resolves to RAM, which the CPU
  // may read, e.g. to compress car thumbnails). Double-buffered per range for
  // the delayed modes.
  struct ResolveReadback {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffers[2];
    uint32_t sizes[2] = {};
    uint64_t submissions[2] = {};
    uint32_t current = 0;
    uint64_t last_used_frame = 0;
    // First frame of the current run of frames resolving this range, and of
    // the range being new (first resolved, or again after an idle time).
    uint64_t run_start_frame = 0;
    uint64_t new_since_frame = 0;
  };
  std::map<uint64_t, ResolveReadback> resolve_readbacks_;
  struct PendingReadback {
    uint32_t address;
    uint32_t length;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  };
  std::vector<PendingReadback> pending_readbacks_;
  Microsoft::WRL::ComPtr<ID3D12Resource> CreateReadbackBuffer(uint32_t size);
  // fh1_frame_dump_frame: records one frame for offline replay.
  std::unique_ptr<Fh1FrameDump> frame_dump_;
  void ReadBackResolve(uint32_t address, uint32_t length);
  // With readback_resolve = none: whether a resolve to the range is a one-off
  // capture to read back (fh1_native_readback_new_resolves), and queueing it.
  bool IsOneOffResolve(uint32_t address, uint32_t length);
  void QueueResolveReadback(uint32_t address, uint32_t length);
  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    std::array<float, 4> position{};
    uint32_t position_mask = 0;
    bool killed = false;
  };
  void TransferTiles(Surface& dest, const TileRun& run);
  // Depth destinations' transfers, batched until FlushTransfers so each
  // destination takes its barriers and nine passes once for all of them.
  struct PendingTransfer {
    uint32_t dest;
    uint32_t source;
    D3D12_RECT rect;
    // Whether the transferred words' stencil (low byte) may be nonzero.
    bool stencil;
  };
  std::vector<PendingTransfer> pending_transfers_;
  void FlushTransfers();
  void FlushColorTransfers(Surface& dest, size_t first, size_t end);
  bool CreateTransferSourceViews(const Surface& source,
                                 ui::d3d12::util::DescriptorCpuGpuHandlePair (&srvs)[2]);
  // Transfers destination pixel rectangles from the previous owner.
  // `tiles_stencil`: whether any transferred tile's stencil may be nonzero.
  void TransferRects(Surface& dest, uint32_t previous_owner, const D3D12_RECT* rects,
                     uint32_t rect_count, uint32_t tile_count, bool tiles_stencil);
  bool AnyTileStencil(uint32_t base, uint32_t count) const;
  // Claims a rectangle of tiles with one transfer when it has a single
  // previous owner; per-row claims otherwise.
  void ClaimTileRect(const SurfaceKey& key, uint32_t column_first, uint32_t row_first,
                     uint32_t column_end, uint32_t row_end);
  ID3D12PipelineState* GetTransferPipeline(const TransferPipelineKey& key);
  uint32_t LayoutConstant(const Surface& surface) const;
  // Splits the resolve's pixel rectangle into rectangles per owning surface.
  void GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0, int32_t x1,
                         int32_t y1, std::vector<SourceRect>& sources_out);
  static constexpr uint32_t kTransferDestUint = 16;
  // Transfer pipeline source kind for depth destinations fed from
  // precomputed EDRAM words.
  static constexpr uint32_t kTransferSourceWords = 3;
  bool EnsureTransferWords(const Surface& dest);
  ID3D12PipelineState* GetTransferWordsPipeline(uint32_t source_kind, bool msaa);
  Microsoft::WRL::ComPtr<ID3D12Resource> transfer_words_;
  uint32_t transfer_words_size_ = 0;
  D3D12_RESOURCE_STATES transfer_words_state_ = D3D12_RESOURCE_STATE_COMMON;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> transfer_words_pipelines_[3][2];
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retired_transfer_words_;
  ID3D12PipelineState* GetResolveMemoryPipeline(uint32_t source_kind, bool msaa);
  // Writes into the native mirror, or (for verification) from another host
  // resource of the owner's layout into `target`.
  bool ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch, ID3D12Resource* source_override = nullptr,
                       D3D12_GPU_VIRTUAL_ADDRESS target = 0, bool unscaled_dest = false);
  // A guest copy: its rectangle, destination and the native sources.
  struct CopyPlan {
    bool empty = false;        // Nothing to copy or clear.
    const char* skip = nullptr;
    bool copy = false;         // The copy can run natively.
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    draw_util::ResolveInfo info;
    uint32_t msaa = 0;
    uint32_t pitch_tiles = 0;
    bool copying_depth = false;
    reg::RB_COLOR_INFO color_info;
    reg::RB_DEPTH_INFO depth_info;
    SurfaceKey resolve_key;
    uint32_t dest_info = 0;
    uint32_t dest_base = 0;
    uint32_t dest_pitch = 0;
    uint32_t sample_select = 0;
    std::string kind;
    std::vector<SourceRect> sources;
  };
  // False when there is nothing to do or the copy cannot be planned.
  bool PlanCopy(CopyPlan& plan);
  // Shared by shadow and native resolves; the render target cache is only
  // used for verification. Returns whether the copy ran completely.
  bool Resolve(D3D12RenderTargetCache* render_target_cache, uint32_t* written_address,
               uint32_t* written_length);
  bool EnsureVerifyScratch();
  // Verification: a draw's target against the Xenos target of the same key.
  void VerifyDrawTarget(D3D12RenderTargetCache& render_target_cache, Surface& surface,
                        uint64_t vs, uint64_t ps, const char* when = "draw");
  uint64_t draw_verify_index_ = 0;
  bool CreateVerifyReadback(PendingVerify& verify);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES new_state);
  void ClearSurfaceRect(Surface& surface, const D3D12_RECT& rect, uint32_t clear_value,
                        uint32_t clear_value_lo);
  void QueueVerify(uint32_t start, uint32_t length, std::string kind);
  void DrainVerifies();
  void Skip(const char* reason) { ++skips_[reason]; }
  void SkipDraw(const char* reason, const Fh1NativeDrawInfo& draw);
  void Count(const char* stat) { ++stats_[stat]; }
  void LogOnce(uint64_t signature, const std::string& message);
  // Whether LogOnce can still log (checked before building a message).
  bool ShouldLog() const { return logged_.size() < 256; }
  // Tracing the current frame (verification dump frames only).
  bool tracing_ = false;
  // Verification checks run in the frames leading up to a dump frame.
  bool verify_window_ = false;
  bool verify_draws_ = false;
  // Verification: ordered events of dump frames.
  void Trace(const std::string& event);
  uint32_t trace_lines_ = 0;
  uint32_t pending_texture_mask_ = 0;
  // Verification: the last pre-draw native readback, by surface.
  std::map<std::string, std::vector<uint8_t>> last_pre_native_;
  void QueueFrontBufferDump(D3D12TextureCache& textures, const char* prefix, uint64_t frame,
                            uint32_t width, uint32_t height, const uint32_t* gamma_pwl);
  void DrainDumps();


  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  DrawExtentEstimator draw_extent_estimator_;
  ShaderInterpreter overwrite_interpreter_;
  Fh1NativeExecutorConfig config_;

  // The mirror and texture cache the executor draws from: its own in shadow
  // mode, the command processor's in native mode.
  D3D12SharedMemory* native_memory_ = nullptr;
  D3D12TextureCache* native_textures_ = nullptr;
  std::unique_ptr<D3D12SharedMemory> owned_memory_;
  std::unique_ptr<D3D12TextureCache> owned_textures_;
  // Null render target views for gaps between bound color slots.
  D3D12_CPU_DESCRIPTOR_HANDLE null_rtv_single_ = {};
  D3D12_CPU_DESCRIPTOR_HANDLE null_rtv_multisample_ = {};

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap_;
  uint32_t rtv_used_ = 0;
  uint32_t dsv_used_ = 0;
  uint32_t rtv_size_ = 0;
  uint32_t dsv_size_ = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_memory_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> transfer_root_signature_;
  std::map<TransferPipelineKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>>
      transfer_pipelines_;
  // Targets PrepareTargets derived for the draw ShadowDraw replays.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  // [depth][msaa]
  // [source kind: color, depth, raw color bits][msaa]
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_memory_pipelines_[3][2];
  std::map<uint32_t, Surface> surfaces_;
  // EDRAM tile -> packed key of the surface that last wrote it.
  std::vector<uint32_t> tile_owners_;
  // EDRAM tile -> whether the low byte of its words (stencil when read as
  // depth) may be nonzero.
  std::vector<uint8_t> tile_stencil_nonzero_;
  // Resolution scale (symmetric): surfaces hold scale x scale host pixels per
  // guest pixel; rectangles are kept in guest pixels and scaled at use.
  uint32_t scale_ = 1;
  D3D12_RECT HostRect(const D3D12_RECT& rect) const {
    return {LONG(rect.left * LONG(scale_)), LONG(rect.top * LONG(scale_)),
            LONG(rect.right * LONG(scale_)), LONG(rect.bottom * LONG(scale_))};
  }
  // Flags shared by the transfer shaders.
  uint32_t TransferFlags() const {
    return (config_.depth_float24_round ? 1u : 0u) | (config_.gamma_as_unorm16 ? 2u : 0u) |
           (config_.fixed16_truncated ? 0u : 4u) | ((scale_ - 1) << 12);
  }
  // Changes whenever tile ownership or a tile's stencil state could change, so
  // a draw with the previous draw's targets and extent needs no work.
  uint64_t tile_generation_ = 1;
  struct PrepareSignature {
    uint64_t generation = 0;
    uint32_t used_bits = 0;
    uint32_t length_tiles = 0;
    bool stencil_written = false;
    uint32_t keys[5] = {};
    bool operator==(const PrepareSignature&) const = default;
  };
  PrepareSignature last_prepare_;
  void MarkTileStencil(uint32_t base, uint32_t length, bool nonzero);
  // Last claim per surface, so repeated draws to one pass do not rewalk tiles.
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> last_claims_;
  std::deque<PendingDump> dumps_;
  std::deque<PendingVerify> verifies_;
  std::set<uint64_t> dump_frames_;
  std::filesystem::path dump_directory_;
  bool verify_ = false;
  uint64_t frame_ = 0;
  std::map<std::string, uint64_t> skips_;
  std::map<std::string, uint64_t> stats_;
  // Resolve kind -> (equal, differing) verified copies.
  std::map<std::string, std::pair<uint64_t, uint64_t>> verify_counts_;
  std::set<uint64_t> verified_textures_;
  Microsoft::WRL::ComPtr<ID3D12Resource> verify_scratch_;
  D3D12_RESOURCE_STATES verify_scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
  static constexpr uint32_t kVerifyScratchSize = 16u << 20;
  // Texture kind -> native equal to CPU, Xenos equal to CPU, total.
  std::map<std::string, std::array<uint64_t, 3>> verify_cpu_counts_;
  std::set<uint64_t> logged_;
  uint64_t shadow_draws_ = 0;
  // CPU time on the GPU command thread, per phase, since the last stats line.
  enum CpuPhase { kCpuPrepareTargets, kCpuTransfers, kCpuBindTargets, kCpuResolves, kCpuPhases };
  std::array<uint64_t, kCpuPhases> cpu_ns_{};
  uint64_t cpu_frames_ = 0;
  // Optional GPU timestamps around executor work (fh1_native_gpu_profile).
  enum GpuPhase { kGpuTransfers, kGpuResolves, kGpuClears, kGpuPhases };
  static constexpr uint32_t kGpuProfileSlots = 4;
  static constexpr uint32_t kGpuProfileQueries = 8192;
  struct GpuProfileSlot {
    uint64_t submission = 0;
    bool pending = false;
    uint32_t used = 0;
    std::vector<std::tuple<GpuPhase, uint32_t, uint32_t>> spans;  // phase, begin, end
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  };
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> gpu_query_heap_;
  std::array<GpuProfileSlot, kGpuProfileSlots> gpu_slots_;
  uint32_t gpu_slot_ = 0;
  uint64_t gpu_timestamp_frequency_ = 0;
  std::array<uint64_t, kGpuPhases> gpu_ticks_{};
  uint64_t gpu_frames_ = 0;
  // Profiling: transferred tile-passes by source -> destination surface.
  std::map<std::string, uint64_t> transfer_volume_;
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
  class CpuTimer {
   public:
    CpuTimer(Fh1NativeExecutor& executor, CpuPhase phase);
    ~CpuTimer();

   private:
    Fh1NativeExecutor& executor_;
    CpuPhase phase_;
    int64_t start_;
  };
  uint64_t shadow_resolves_ = 0;
  bool initialized_ = false;
};

}  // namespace rex::graphics::d3d12
