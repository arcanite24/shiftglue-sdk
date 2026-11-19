/**
 * @file        graphics/d3d11/command_processor.h
 * @brief       Direct3D 11 (feature level 11_0) command processor.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/d3d11/primitive_processor.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class Fh1NativeExecutor;

// Feature level 11_0 command processor.
//
// Threading: everything here, in the shared memory, texture cache, pipeline
// cache, primitive processor and the FH1 native executor runs on the GPU
// commands thread, which owns the device's immediate context. The presenter
// (UI thread) never binds state on the immediate context: it records into a
// deferred context and executes it with RestoreContextState.
//
// State ownership: guest draws bind their full pipeline state through a
// shadow of what was last bound, so redundant binds are skipped. Any other
// code that changes immediate context state (compute dispatches for texture
// loads and resolves, the executor's transfer and clear draws) must call
// InvalidateGuestDrawState() afterwards; the next guest draw then rebinds
// everything. Code that binds a UAV of a resource must unbind it after use
// (D3D11 unbinds a resource's SRVs while it is bound for writing and the
// debug layer reports it), for instance with UnbindComputeResources().
//
// There are no submissions in D3D11. Frames are fenced with event queries:
// GetCurrentFrame() is the guest frame being recorded, GetCompletedFrame() the
// newest one the GPU has finished. Resource lifetime needs no deferral (the
// runtime keeps resources alive while the GPU uses them).
class D3D11CommandProcessor : public CommandProcessor {
 public:
  explicit D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system,
                                 system::KernelState* kernel_state);
  ~D3D11CommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  ui::d3d11::D3D11Provider& GetD3D11Provider() const {
    return *static_cast<ui::d3d11::D3D11Provider*>(graphics_system_->provider());
  }
  ID3D11Device* GetDevice() const { return GetD3D11Provider().GetDevice(); }
  ID3D11DeviceContext* GetContext() const { return GetD3D11Provider().GetImmediateContext(); }

  system::GraphicsShaderTranslationObserver GetShaderTranslationObserver() const {
    return graphics_system_->shader_translation_observer();
  }

  D3D11SharedMemory& GetSharedMemory() const { return *shared_memory_; }
  D3D11TextureCache& GetTextureCache() const { return *texture_cache_; }
  Fh1NativeExecutor* GetFh1NativeExecutor() const { return fh1_native_executor_.get(); }

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Call after binding anything on the immediate context outside the guest
  // draw path.
  void InvalidateGuestDrawState() { guest_draw_state_valid_ = false; }
  // Unbinds the compute shader's SRVs (slots [0, srv_count)), UAVs (slots
  // [0, uav_count)) and constant buffers (slots [0, cbuffer_count)).
  void UnbindComputeResources(uint32_t srv_count, uint32_t uav_count, uint32_t cbuffer_count = 0);

  // Waits until the GPU has executed everything issued so far (for one-off
  // readbacks). Returns false if the device was lost or shutdown was
  // requested.
  bool AwaitGpuIdle();
  bool IsShutdownRequested() const { return !worker_running_.load(); }

  // Draws issued with the native executor's targets reuse the guest draw
  // path's fixed-function helpers.
  void SetViewport(const D3D11_VIEWPORT& viewport);
  void SetScissorRect(const D3D11_RECT& scissor_rect);

  // For the FH1 native executor's resolve readback.
  ReadbackResolveMode Fh1ReadbackResolveMode() const;
  void RunRequestedFrameReplay() override;
  void RestoreFrameDumpRegisters(std::span<const uint32_t> registers) override;
  // Copies a guest physical range of the shared memory mirror (with the GPU's
  // writes) to `out`, waiting for the GPU.
  bool ReadbackSharedMemory(uint32_t address, uint32_t length, std::vector<uint8_t>& out);

  std::string GetWindowTitleText() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;
  void FlushCpuVisibleResults() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  static constexpr uint32_t kQueueFrames = 3;

  // Constant buffer slots of guest shaders (the DXBC translator's CbufferRegister
  // order: b0 system, b1 float constants, b2 bool and loop, b3 fetch).
  enum : uint32_t {
    kCbufferSystem,
    kCbufferFloat,
    kCbufferBoolLoop,
    kCbufferFetch,
    kCbufferCount,
  };

  void BeginFrameIfNeeded();
  void EndFrame();
  void UpdateCompletedFrame();
  bool CheckDeviceLost();

  void UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                const draw_util::Scissor& scissor, bool primitive_polygonal,
                                reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool primitive_polygonal, uint32_t line_loop_closing_index,
                                  xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UpdateBindings(const D3D11Shader* vertex_shader, const D3D11Shader* pixel_shader);
  bool UploadConstantBuffer(uint32_t index, const void* data, uint32_t size);

  bool InitializeOcclusionQueries();
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address,
                              xenos::xe_gpu_depth_sample_counts* sample_counts);
  void WriteGuestOcclusionResult(xenos::xe_gpu_depth_sample_counts* sample_counts,
                                 uint64_t samples);

  bool InitializeGammaAndFxaa();
  void InvalidateAllConstantBuffers();

  bool device_lost_ = false;

  // Host render configuration, from the cvars and the device at setup.
  bool msaa_2x_supported_ = false;
  bool gamma_render_target_as_unorm16_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;
  bool cache_clear_requested_ = false;

  bool frame_open_ = false;
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Event queries ending each in-flight frame, indexed by frame % kQueueFrames.
  Microsoft::WRL::ComPtr<ID3D11Query> frame_end_queries_[kQueueFrames];
  uint64_t frame_end_query_frames_[kQueueFrames] = {};
  Microsoft::WRL::ComPtr<ID3D11Query> idle_query_;

  std::unique_ptr<D3D11SharedMemory> shared_memory_;
  std::unique_ptr<D3D11PrimitiveProcessor> primitive_processor_;
  std::unique_ptr<D3D11TextureCache> texture_cache_;
  std::unique_ptr<D3D11PipelineCache> pipeline_cache_;
  std::unique_ptr<Fh1NativeExecutor> fh1_native_executor_;

  // Guest draw state shadow, valid while guest_draw_state_valid_.
  bool guest_draw_state_valid_ = false;
  const D3D11PipelineCache::Pipeline* current_pipeline_ = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY current_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11Buffer* current_index_buffer_ = nullptr;
  DXGI_FORMAT current_index_format_ = DXGI_FORMAT_UNKNOWN;
  uint32_t current_index_offset_ = 0;
  uint32_t current_stencil_ref_ = 0;
  std::array<float, 4> current_blend_factor_ = {};

  D3D11_VIEWPORT ff_viewport_ = {};
  D3D11_RECT ff_scissor_ = {};
  float ff_blend_factor_[4] = {};
  uint32_t ff_stencil_ref_ = 0;
  bool ff_viewport_update_needed_ = true;
  bool ff_scissor_update_needed_ = true;
  bool ff_blend_factor_update_needed_ = true;
  bool ff_stencil_ref_update_needed_ = true;

  // System shader constants.
  DxbcShaderTranslator::SystemConstants system_constants_;
  bool system_constants_dirty_ = true;
  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4] = {};
  uint64_t current_float_constant_map_pixel_[4] = {};

  // Dynamic constant buffers, one set per stage where the contents differ:
  // the vertex and pixel float constants are separate, system, bool/loop and
  // fetch constants are shared by both stages.
  struct ConstantBuffer {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    uint32_t size = 0;
    bool up_to_date = false;
  };
  ConstantBuffer cbuffer_system_;
  ConstantBuffer cbuffer_float_vertex_;
  ConstantBuffer cbuffer_float_pixel_;
  ConstantBuffer cbuffer_bool_loop_;
  ConstantBuffer cbuffer_fetch_;
  std::vector<uint8_t> cbuffer_staging_;

  // Texture and sampler bindings of the last guest draw, per stage. SRVs are
  // set on every draw (the runtime unbinds SRVs of resources bound for
  // writing, so a shadow could go stale); slots past the current count are
  // cleared once.
  std::vector<ID3D11ShaderResourceView*> srvs_scratch_;
  std::vector<ID3D11SamplerState*> samplers_scratch_;
  uint32_t bound_srv_count_vertex_ = 0;
  uint32_t bound_srv_count_pixel_ = 0;

  // Vertex buffer residency (the guest ranges vertex fetch reads uploaded to
  // the mirror), invalidated by fetch constant writes.
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);

  // Occlusion queries: the guest has one active at a time; its result is
  // read back (waiting for the GPU) at the guest's end event, like on
  // Direct3D 12.
  struct ActiveOcclusionQuery {
    uint32_t sample_count_address = 0;
    bool valid = false;
  } active_occlusion_query_;
  Microsoft::WRL::ComPtr<ID3D11Query> occlusion_query_;
  bool occlusion_queries_available_ = false;
  void DisableHostOcclusionQueries();

  // Gamma ramps in one buffer: the 256-entry table (R10G10B10A2_UNORM view,
  // 256 elements) and after it the 128-entry PWL ramp (R16G16_UINT view,
  // 128 x 3 elements), like on Direct3D 12.
  Microsoft::WRL::ComPtr<ID3D11Buffer> gamma_ramp_buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_ramp_table_srv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_ramp_pwl_srv_;
  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> apply_gamma_table_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> apply_gamma_pwl_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> apply_gamma_table_fxaa_luma_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> apply_gamma_pwl_fxaa_luma_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> fxaa_cs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> fxaa_extreme_cs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> swap_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> fxaa_sampler_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> fxaa_source_texture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fxaa_source_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> fxaa_source_uav_;
  uint32_t fxaa_source_width_ = 0;
  uint32_t fxaa_source_height_ = 0;
  // Host draws since the last swap, for the performance counters.
  uint64_t pending_draw_calls_ = 0;
  // Host viewports bounded by the bound render targets (d3d11_viewport_fit_targets).
  bool viewport_fit_targets_ = false;
  // The front buffer with its host swizzle applied (no SRV component mapping
  // on Direct3D 11), in the front buffer's format.
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> swap_swizzle_cs_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> swap_swizzle_texture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> swap_swizzle_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> swap_swizzle_uav_;
  uint32_t swap_swizzle_width_ = 0;
  uint32_t swap_swizzle_height_ = 0;
  DXGI_FORMAT swap_swizzle_format_ = DXGI_FORMAT_UNKNOWN;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
};

}  // namespace rex::graphics::d3d11
