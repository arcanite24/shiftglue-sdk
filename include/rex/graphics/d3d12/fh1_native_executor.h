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

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>

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

  static bool Enabled();

  bool Initialize(const Fh1NativeExecutorConfig& config);
  void Shutdown();

  D3D12SharedMemory* shared_memory() const { return native_memory_.get(); }
  bool verifying() const { return verify_; }

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
  void PrepareDraw(uint32_t used_texture_mask, const Shader& vertex_shader,
                   uint32_t guest_dma_index_offset, uint32_t guest_dma_index_size);
  // Right after the Xenos draw was recorded, with its state still bound.
  void ShadowDraw(D3D12RenderTargetCache& render_target_cache, const Fh1NativeDrawInfo& draw);
  // After the Xenos resolve of the same copy.
  void ShadowResolve(D3D12RenderTargetCache& render_target_cache, bool xenos_succeeded);
  // Verification, before the Xenos resolve: the copy from the native owners
  // and from the Xenos render targets of the same keys, compared.
  void VerifySurfacesBeforeResolve(D3D12RenderTargetCache& render_target_cache);
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
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;
    SurfaceKey key;
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
    uint32_t dest_kind;  // 0 color, 1 depth, 2 + bit: stencil bit
    DXGI_FORMAT dest_format;
    uint32_t dest_samples;
    uint32_t sample_mask;
    bool source_depth;
    bool source_msaa;
    auto tie() const {
      return std::tie(dest_kind, dest_format, dest_samples, sample_mask, source_depth,
                      source_msaa);
    }
    bool operator<(const TransferPipelineKey& other) const { return tie() < other.tie(); }
  };

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const;
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format);
  static uint32_t PitchTiles(uint32_t pitch_pixels, uint32_t msaa);
  static uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa);
  DXGI_FORMAT ColorResourceFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT ColorDrawFormat(xenos::ColorRenderTargetFormat format) const;
  Surface* GetOrCreateSurface(const SurfaceKey& key);
  Surface* FindSurface(uint32_t packed_key);
  // Claims tiles [base, base + length) (with EDRAM address wrapping) for key
  // and transfers the previous owners' contents into the surface.
  void ClaimTiles(uint32_t base, uint32_t length, uint32_t packed_key);
  void TransferTiles(Surface& dest, const TileRun& run);
  ID3D12PipelineState* GetTransferPipeline(const TransferPipelineKey& key);
  uint32_t LayoutConstant(const Surface& surface) const;
  // Splits the resolve's pixel rectangle into rectangles per owning surface.
  void GetResolveSources(const SurfaceKey& resolve_key, int32_t x0, int32_t y0, int32_t x1,
                         int32_t y1, std::vector<SourceRect>& sources_out);
  ID3D12PipelineState* GetResolveMemoryPipeline(bool depth, bool msaa);
  // Writes into the native mirror, or (for verification) from another host
  // resource of the owner's layout into `target`.
  bool ResolveToMemory(const SourceRect& source, const SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch, ID3D12Resource* source_override = nullptr,
                       D3D12_GPU_VIRTUAL_ADDRESS target = 0);
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
  bool EnsureVerifyScratch();
  // Verification: a draw's target against the Xenos target of the same key.
  void VerifyDrawTarget(D3D12RenderTargetCache& render_target_cache, Surface& surface,
                        uint64_t vs, uint64_t ps);
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
  // Verification: ordered events of dump frames.
  void Trace(const std::string& event);
  uint32_t trace_lines_ = 0;
  void QueueFrontBufferDump(D3D12TextureCache& textures, const char* prefix, uint64_t frame,
                            uint32_t width, uint32_t height, const uint32_t* gamma_pwl);
  void DrainDumps();
  void LogStats(uint64_t frame);

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  DrawExtentEstimator draw_extent_estimator_;
  Fh1NativeExecutorConfig config_;

  std::unique_ptr<D3D12SharedMemory> native_memory_;
  std::unique_ptr<D3D12TextureCache> native_textures_;

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
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_memory_pipelines_[2][2];
  std::map<uint32_t, Surface> surfaces_;
  // EDRAM tile -> packed key of the surface that last wrote it.
  std::vector<uint32_t> tile_owners_;
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
  uint64_t shadow_resolves_ = 0;
  bool initialized_ = false;
};

}  // namespace rex::graphics::d3d12
