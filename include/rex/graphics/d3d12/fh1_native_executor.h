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
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
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
class D3D12SharedMemory;
class D3D12TextureCache;

// State of a consumed guest draw that the executor needs, captured by
// D3D12CommandProcessor::IssueDraw.
struct Fh1NativeDrawInfo {
  bool memexport = false;
  bool occlusion_query_active = false;
  bool rasterization_done = false;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  uint32_t normalized_color_mask = 0;
  const Shader* vertex_shader = nullptr;
  const Shader* pixel_shader = nullptr;
};

// Host configuration the executor must share with the pipelines it reuses.
struct Fh1NativeExecutorConfig {
  bool msaa_2x_supported = true;
  bool gamma_as_unorm16 = false;
  bool depth_float24_round = false;
  // 16_16[_16_16] host targets are snorm limited to -1...1
  // (D3D12HostRenderConfig::IsFixed16TruncatedToMinus1To1).
  bool fixed16_truncated = true;
  // The command processor's guest-memory mirror and texture cache.
  D3D12SharedMemory* memory = nullptr;
  D3D12TextureCache* textures = nullptr;
};

// XR-02/03/04 native executor: the FH1 renderer. The command processor draws
// every consumed guest draw into native
// render surfaces whose EDRAM tile ownership, clears and resolves the executor
// tracks from the guest registers, using the command processor's guest-memory
// mirror and texture cache. Resolves write the guest texture layout into the
// mirror, so later fetches of resolve output decode native data like any other
// guest texture. The native front buffer is presented and can be dumped as
// PPM. Anything it cannot execute is counted by a named skip reason.
class Fh1NativeExecutor {
 public:
  Fh1NativeExecutor(D3D12CommandProcessor& command_processor, const RegisterFile& register_file,
                    memory::Memory& memory);
  ~Fh1NativeExecutor();

  bool Initialize(const Fh1NativeExecutorConfig& config);
  void Shutdown();

  // Before the targets are bound for a draw: derives the surfaces the draw
  // writes and takes their EDRAM tiles, transferring the previous owners'
  // contents like the guest's EDRAM aliasing does.
  void PrepareTargets(const Fh1NativeDrawInfo& draw);
  void LogStats(uint64_t frame);
  void RecordCopyInputs() {
    if (frame_dump_) frame_dump_->RecordCopyInputs();
  }
  // Frame dumps: the guest ranges a draw reads, once the mirror holds them.
  void RecordDrawInputs(uint32_t used_texture_mask, const Shader& vertex_shader,
                        uint32_t guest_dma_index_offset, uint32_t guest_dma_index_size);

  // In place of the Xenos render target cache's update: binds the surfaces
  // PrepareTargets derived and returns the bound slots and their formats as
  // the Xenos RenderTargetCache::GetLastUpdateBoundRenderTargets did. False skips the
  // draw (the reason is counted).
  bool BindTargets(uint32_t& bound_bits, uint32_t* formats);
  // After the command processor recorded the draw.
  void NativeDrawIssued(const Fh1NativeDrawInfo& draw);
  // In place of the render target cache's resolve. Returns whether the copy
  // (or clear) ran, and the guest range it wrote.
  bool NativeResolve(uint32_t& written_address, uint32_t& written_length);
  // Before the guest CPU can observe GPU progress: copies the one-off resolve
  // read-backs recorded since into guest memory, waiting for the GPU once.
  void FlushResolveReadbacks();
  // End of a guest frame, at the swap: GPU profile drain, frame dump
  // bookkeeping, native front-buffer PPM dumps and periodic stats.
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address, uint32_t width, uint32_t height,
              const uint32_t* gamma_pwl);
  // fh1_native_gpu_profile: GPU time of texture cache loads (untile and copy),
  // split by whether a GPU write (a resolve) invalidated the reloaded data.
  // Begin returns UINT32_MAX when the profile is off or the frame is not
  // measured; End ignores it.
  uint32_t BeginTextureLoadGpuTiming() { return GpuBegin(); }
  void EndTextureLoadGpuTiming(uint32_t begin, bool resolve_sourced) {
    GpuEnd(resolve_sourced ? kGpuTextureReloads : kGpuTextureLoads, begin);
  }

 private:
  using SurfaceKey = Fh1SurfaceKey;
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
  // A rectangle of a resolve, in resolve surface pixels, whose tiles one
  // native surface owns.
  struct SourceRect {
    Surface* surface = nullptr;
    D3D12_RECT rect = {};
    // No surface: whether no surface owns the tiles, or the owner's surface is
    // gone.
    bool unowned = false;
  };

  static constexpr uint32_t kNoOwner = Fh1EdramTiles::kNoOwner;
  using TileRun = Fh1EdramTiles::Run;
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
  // Writes into the guest-memory mirror, or into `target` (the scaled
  // resolve range) when nonzero.
  bool ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch, D3D12_GPU_VIRTUAL_ADDRESS target = 0,
                       bool unscaled_dest = false);
  // A guest copy: its rectangle, destination and the native sources.
  struct CopyPlan : Fh1ResolvePlan {
    std::vector<SourceRect> sources;
  };
  // False when there is nothing to do or the copy cannot be planned.
  bool PlanCopy(CopyPlan& plan);
  // Returns whether the copy ran completely.
  bool Resolve(uint32_t* written_address, uint32_t* written_length);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES new_state);
  void ClearSurfaceRect(Surface& surface, const D3D12_RECT& rect, uint32_t clear_value,
                        uint32_t clear_value_lo);
  void Skip(const char* reason) { ++skips_[reason]; }
  void Count(const char* stat) { ++stats_[stat]; }
  void LogOnce(uint64_t signature, const std::string& message);
  // Whether LogOnce can still log (checked before building a message).
  bool ShouldLog() const { return logged_.size() < 256; }
  void QueueFrontBufferDump(uint64_t frame, uint32_t width, uint32_t height,
                            const uint32_t* gamma_pwl);
  void DrainDumps();

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  DrawExtentEstimator draw_extent_estimator_;
  ShaderInterpreter overwrite_interpreter_;
  Fh1NativeExecutorConfig config_;

  // The command processor's mirror and texture cache.
  D3D12SharedMemory* native_memory_ = nullptr;
  D3D12TextureCache* native_textures_ = nullptr;
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
  // Targets PrepareTargets derived for the draw BindTargets binds.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  // [depth][msaa]
  // [source kind: color, depth, raw color bits][msaa]
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_memory_pipelines_[3][2];
  std::map<uint32_t, Surface> surfaces_;
  // EDRAM tile -> packed key of the surface that last wrote it, and whether
  // the low byte of its words (stencil when read as depth) may be nonzero.
  Fh1EdramTiles tiles_;
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
  std::deque<PendingDump> dumps_;
  std::set<uint64_t> dump_frames_;
  std::filesystem::path dump_directory_;
  uint64_t frame_ = 0;
  std::map<std::string, uint64_t> skips_;
  std::map<std::string, uint64_t> stats_;
  std::set<uint64_t> logged_;
  uint64_t draws_ = 0;
  // CPU time on the GPU command thread, per phase, since the last stats line.
  enum CpuPhase { kCpuPrepareTargets, kCpuTransfers, kCpuBindTargets, kCpuResolves, kCpuPhases };
  std::array<uint64_t, kCpuPhases> cpu_ns_{};
  uint64_t cpu_frames_ = 0;
  // Optional GPU timestamps around executor work (fh1_native_gpu_profile).
  enum GpuPhase {
    kGpuTransfers,
    kGpuResolves,
    kGpuClears,
    kGpuTextureReloads,
    kGpuTextureLoads,
    kGpuPhases
  };
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
  uint64_t resolves_ = 0;
  bool initialized_ = false;
};

}  // namespace rex::graphics::d3d12
