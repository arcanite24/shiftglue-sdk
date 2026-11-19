/**
 * @file        graphics/d3d11/pipeline_cache.h
 * @brief       Direct3D 11 shaders, geometry shaders and state objects.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/d3d11/shader.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/string/buffer.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// Guest shaders are analyzed and translated to shader model 5.0 DXBC at run
// time (there is no prebuilt pack for Direct3D 11), on the GPU commands
// thread, the first time a shader modification is drawn.
//
// Direct3D 11 has no pipeline state objects: a "pipeline" here is the set of
// shader and state objects a draw binds. Each state object type is cached
// separately by its description, since the device allows only 4096 unique
// objects of each state type; depth bias, which is part of the rasterizer
// state, is quantized to keep the rasterizer state count bounded.
class D3D11PipelineCache {
 public:
  struct Pipeline {
    ID3D11VertexShader* vertex_shader = nullptr;
    // For primitive types Direct3D 11 does not have (point sprites, rectangle
    // and quad lists), or nullptr.
    ID3D11GeometryShader* geometry_shader = nullptr;
    // nullptr when drawing depth only or with rasterization disabled.
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11RasterizerState* rasterizer_state = nullptr;
    ID3D11DepthStencilState* depth_stencil_state = nullptr;
    ID3D11BlendState* blend_state = nullptr;
    // 2x MSAA emulated with 4x uses samples 0 and 3 (OMSetBlendState).
    uint32_t sample_mask = UINT32_MAX;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  };

  D3D11PipelineCache(D3D11CommandProcessor& command_processor, const RegisterFile& register_file,
                     bool msaa_2x_supported);
  ~D3D11PipelineCache();

  bool Initialize();
  void Shutdown();

  // No persistent storage yet: every run translates the shaders it draws.
  void InitializeShaderStorage(const std::filesystem::path& /*cache_root*/,
                               uint32_t /*title_id*/, bool /*blocking*/) {}

  D3D11Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count);
  // Analyzes the microcode if not done yet (call before getting the
  // modification).
  void AnalyzeShaderUcode(Shader& shader);

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  // Translates the shader modification (if not translated yet) and creates
  // its host shader. Returns false if the translation or the creation failed
  // (now or before).
  bool EnsureShaderTranslated(D3D11Shader::D3D11Translation& translation);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this! Both shaders must have been
  // passed to EnsureShaderTranslated successfully. Returns nullptr if the
  // state can't be drawn. The returned object stays valid until Shutdown.
  const Pipeline* ConfigurePipeline(
      D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats);

 private:
  enum class GeometryShaderType : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  union GeometryShaderKey {
    uint32_t key;
    struct {
      GeometryShaderType type : 2;
      uint32_t interpolator_count : 5;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
      // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
      uint32_t point_ps_ucp_mode : 2;
    };
    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }
  };

  static bool GetGeometryShaderKey(GeometryShaderType geometry_shader_type,
                                   DxbcShaderTranslator::Modification vertex_shader_modification,
                                   DxbcShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  // Shader model 5.0 geometry shader bytecode (with the DXBC checksum).
  static void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);
  ID3D11GeometryShader* GetGeometryShader(GeometryShaderKey key);

  ID3D11RasterizerState* GetRasterizerState(const D3D11_RASTERIZER_DESC& desc);
  ID3D11DepthStencilState* GetDepthStencilState(const D3D11_DEPTH_STENCIL_DESC& desc);
  ID3D11BlendState* GetBlendState(const D3D11_BLEND_DESC& desc);

  D3D11CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  bool msaa_2x_supported_;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;
  bool gamma_render_target_as_unorm16_ = false;

  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  string::StringBuffer ucode_disasm_buffer_;

  // Depth-only drawing with float24 depth converted in the pixel shader.
  Microsoft::WRL::ComPtr<ID3D11PixelShader> float24_round_ps_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> float24_truncate_ps_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, D3D11Shader*, rex::IdentityHasher<uint64_t>> shaders_;

  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11GeometryShader>> geometry_shaders_;

  // State objects by the XXH3 hash of their description; the description is
  // compared on lookup.
  template <typename Desc, typename State>
  struct StateCacheEntry {
    Desc desc;
    Microsoft::WRL::ComPtr<State> state;
  };
  std::unordered_multimap<uint64_t, StateCacheEntry<D3D11_RASTERIZER_DESC, ID3D11RasterizerState>,
                          rex::IdentityHasher<uint64_t>>
      rasterizer_states_;
  std::unordered_multimap<uint64_t,
                          StateCacheEntry<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState>,
                          rex::IdentityHasher<uint64_t>>
      depth_stencil_states_;
  std::unordered_multimap<uint64_t, StateCacheEntry<D3D11_BLEND_DESC, ID3D11BlendState>,
                          rex::IdentityHasher<uint64_t>>
      blend_states_;

  // Pipelines by the XXH3 hash of the Pipeline object pointers (which are
  // unique per description), so a repeated state costs one lookup.
  std::unordered_multimap<uint64_t, std::unique_ptr<Pipeline>, rex::IdentityHasher<uint64_t>>
      pipelines_;
};

}  // namespace rex::graphics::d3d11
