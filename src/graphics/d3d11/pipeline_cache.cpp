/**
 * @file        graphics/d3d11/pipeline_cache.cpp
 * @brief       Direct3D 11 shaders, geometry shaders and state objects.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The geometry shader generator is the Direct3D 12 one (originally from
 * Xenia, Copyright 2022 Ben Vanik, BSD license) emitting shader model 5.0.
 */

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/float24_round_ps.h"
#include "../shaders/bytecode/d3d11_5_0/float24_truncate_ps.h"
}  // namespace shaders

namespace {

// Generates code with the shader model 5.0 operand and declaration forms on
// this thread while alive.
class ScopedShaderModel5_0 {
 public:
  ScopedShaderModel5_0() { dxbc::SetTargetShaderModel(dxbc::TargetShaderModel::k5_0); }
  ~ScopedShaderModel5_0() { dxbc::SetTargetShaderModel(dxbc::TargetShaderModel::k5_1); }
};

// The device allows 4096 unique objects of each state type; warn well before.
constexpr size_t kStateObjectWarningCount = 3072;

template <typename Desc>
uint64_t HashDesc(const Desc& desc) {
  return XXH3_64bits(&desc, sizeof(desc));
}

}  // namespace

D3D11PipelineCache::D3D11PipelineCache(D3D11CommandProcessor& command_processor,
                                       const RegisterFile& register_file, bool msaa_2x_supported)
    : command_processor_(command_processor),
      register_file_(register_file),
      msaa_2x_supported_(msaa_2x_supported) {}

D3D11PipelineCache::~D3D11PipelineCache() { Shutdown(); }

bool D3D11PipelineCache::Initialize() {
  // Read once so the translations stay consistent within the run.
  depth_float24_round_ = REXCVAR_GET(depth_float24_round);
  depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);
  gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);

  const ui::d3d11::D3D11Provider& provider = command_processor_.GetD3D11Provider();
  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(), false, false, !gamma_render_target_as_unorm16_,
      msaa_2x_supported_, 1, 1, false, true);

  ID3D11Device* device = command_processor_.GetDevice();
  if (FAILED(device->CreatePixelShader(shaders::float24_round_ps, sizeof(shaders::float24_round_ps),
                                       nullptr, &float24_round_ps_)) ||
      FAILED(device->CreatePixelShader(shaders::float24_truncate_ps,
                                       sizeof(shaders::float24_truncate_ps), nullptr,
                                       &float24_truncate_ps_))) {
    REXGPU_ERROR("D3D11 pipeline cache: Failed to create the float24 depth pixel shaders");
    return false;
  }
  return true;
}

void D3D11PipelineCache::Shutdown() {
  pipelines_.clear();
  blend_states_.clear();
  depth_stencil_states_.clear();
  rasterizer_states_.clear();
  geometry_shaders_.clear();
  float24_truncate_ps_.Reset();
  float24_round_ps_.Reset();
  for (auto& shader_pair : shaders_) {
    delete shader_pair.second;
  }
  shaders_.clear();
  shader_translator_.reset();
}

D3D11Shader* D3D11PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address, uint32_t dword_count) {
  uint64_t data_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second;
  }
  // Tracked even if the translation fails, to not retry.
  D3D11Shader* shader = new D3D11Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

void D3D11PipelineCache::AnalyzeShaderUcode(Shader& shader) {
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }
}

DxbcShaderTranslator::Modification D3D11PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification;
  modification.vertex.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg);
  modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  modification.vertex.vertex_kill_and = uint32_t(
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);
  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  return modification;
}

DxbcShaderTranslator::Modification D3D11PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification;
  modification.pixel.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg);
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (depth_float24_convert_in_pixel_shader_ && normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode = depth_float24_round_
                                                ? DepthStencilMode::kFloat24Rounding
                                                : DepthStencilMode::kFloat24Truncating;
  } else if (shader.implicit_early_z_write_allowed() &&
             (!shader.writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }
  return modification;
}

bool D3D11PipelineCache::EnsureShaderTranslated(D3D11Shader::D3D11Translation& translation) {
  if (translation.host_shader_creation_attempted()) {
    return translation.vertex_shader() || translation.pixel_shader();
  }
  translation.host_shader_creation_attempted_ = true;
  D3D11Shader& shader = static_cast<D3D11Shader&>(translation.shader());
  AnalyzeShaderUcode(shader);
  if (!translation.is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(translation)) {
      REXGPU_ERROR("D3D11: Shader {:016X} modification {:016X} translation failed",
                   shader.ucode_data_hash(), translation.modification());
      return false;
    }
  }
  if (!translation.is_valid()) {
    return false;
  }
  if (!REXCVAR_GET(dump_shaders).empty()) {
    translation.Dump(REXCVAR_GET(dump_shaders), "d3d11");
  }
  const std::vector<uint8_t>& binary = translation.translated_binary();
  ID3D11Device* device = command_processor_.GetDevice();
  HRESULT result;
  if (shader.type() == xenos::ShaderType::kVertex) {
    DxbcShaderTranslator::Modification modification(translation.modification());
    if (modification.vertex.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      // Tessellation is not implemented on the Direct3D 11 path.
      REXGPU_ERROR("D3D11: Shader {:016X} needs tessellation, which is not supported",
                   shader.ucode_data_hash());
      return false;
    }
    result = device->CreateVertexShader(binary.data(), binary.size(), nullptr,
                                        &translation.vertex_shader_);
  } else {
    result = device->CreatePixelShader(binary.data(), binary.size(), nullptr,
                                       &translation.pixel_shader_);
  }
  if (FAILED(result)) {
    REXGPU_ERROR("D3D11: Failed to create the host shader for {:016X} modification {:016X} ({:08X})",
                 shader.ucode_data_hash(), translation.modification(), uint32_t(result));
    return false;
  }
  return true;
}

const D3D11PipelineCache::Pipeline* D3D11PipelineCache::ConfigurePipeline(
    D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats) {
  assert_not_null(vertex_shader);
  if (!vertex_shader->vertex_shader() || (pixel_shader && !pixel_shader->pixel_shader())) {
    return nullptr;
  }
  if (primitive_processing_result.IsTessellated()) {
    return nullptr;
  }

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (!rasterization_enabled && pixel_shader) {
    assert_always();
    return nullptr;
  }

  Pipeline pipeline;
  pipeline.vertex_shader = vertex_shader->vertex_shader();

  // Topology. Direct3D 11 always restarts strips at the all-ones index of the
  // index format.
  GeometryShaderType geometry_shader_type = GeometryShaderType::kNone;
  switch (primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
      geometry_shader_type = GeometryShaderType::kPointList;
      break;
    case xenos::PrimitiveType::kLineList:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::k2DLineStrip:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kRectangleList:
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      geometry_shader_type = GeometryShaderType::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      // Quads are drawn as line lists with adjacency.
      pipeline.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
      geometry_shader_type = GeometryShaderType::kQuadList;
      break;
    default:
      REXGPU_ERROR("D3D11: Unsupported host primitive type {}",
                   uint32_t(primitive_processing_result.host_primitive_type));
      return nullptr;
  }
  if (geometry_shader_type != GeometryShaderType::kNone) {
    GeometryShaderKey geometry_shader_key;
    DxbcShaderTranslator::Modification pixel_modification(
        pixel_shader ? pixel_shader->modification() : 0);
    if (!pixel_shader) {
      // Without a pixel shader, the geometry shader still passes the
      // interpolators the vertex shader writes.
      pixel_modification.pixel.interpolator_mask =
          DxbcShaderTranslator::Modification(vertex_shader->modification())
              .vertex.interpolator_mask;
    }
    if (GetGeometryShaderKey(geometry_shader_type,
                             DxbcShaderTranslator::Modification(vertex_shader->modification()),
                             pixel_modification, geometry_shader_key)) {
      pipeline.geometry_shader = GetGeometryShader(geometry_shader_key);
      if (!pipeline.geometry_shader) {
        return nullptr;
      }
    }
  }

  D3D11_RASTERIZER_DESC rasterizer_desc;
  std::memset(&rasterizer_desc, 0, sizeof(rasterizer_desc));
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.ScissorEnable = TRUE;
  // Zeroed with padding, since the descriptions are hashed as bytes.
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
  std::memset(&depth_stencil_desc, 0, sizeof(depth_stencil_desc));
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth_stencil_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  D3D11_BLEND_DESC blend_desc;
  std::memset(&blend_desc, 0, sizeof(blend_desc));
  blend_desc.IndependentBlendEnable = TRUE;
  for (uint32_t i = 0; i < 8; ++i) {
    D3D11_RENDER_TARGET_BLEND_DESC& rt = blend_desc.RenderTarget[i];
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_ZERO;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 0;
  }

  if (rasterization_enabled) {
    pipeline.pixel_shader = pixel_shader ? pixel_shader->pixel_shader() : nullptr;

    // Direct3D has no per-face fill mode and depth bias: as on Direct3D 12,
    // use the values of the side that isn't culled.
    bool cull_front = false, cull_back = false;
    bool fill_mode_wireframe = false;
    if (primitive_polygonal) {
      rasterizer_desc.FrontCounterClockwise = pa_su_sc_mode_cntl.face == 0 ? TRUE : FALSE;
      cull_front = pa_su_sc_mode_cntl.cull_front != 0;
      cull_back = pa_su_sc_mode_cntl.cull_back != 0;
      if (cull_front) {
        assert_false(cull_back);
        rasterizer_desc.CullMode = D3D11_CULL_FRONT;
      } else if (cull_back) {
        rasterizer_desc.CullMode = D3D11_CULL_BACK;
      }
      if (!cull_front &&
          pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles) {
        fill_mode_wireframe = true;
      }
      if (!cull_back && pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles) {
        fill_mode_wireframe = true;
      }
      if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
        fill_mode_wireframe = false;
      }
    }
    rasterizer_desc.FillMode = fill_mode_wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    float polygon_offset, polygon_offset_scale;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                             polygon_offset);
    rasterizer_desc.DepthBias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    rasterizer_desc.SlopeScaledDepthBias =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
    rasterizer_desc.DepthClipEnable = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ? FALSE : TRUE;

    // Depth / stencil.
    bool depth_stencil_used = false;
    xenos::DepthRenderTargetFormat depth_format = xenos::DepthRenderTargetFormat::kD24S8;
    if (bound_depth_and_color_render_target_bits & 1) {
      depth_format =
          xenos::DepthRenderTargetFormat(bound_depth_and_color_render_target_formats[0]);
      xenos::CompareFunction depth_func = xenos::CompareFunction::kAlways;
      bool depth_write = false;
      if (normalized_depth_control.z_enable) {
        depth_func = normalized_depth_control.zfunc;
        depth_write = normalized_depth_control.z_write_enable != 0;
      }
      if (depth_func != xenos::CompareFunction::kAlways || depth_write) {
        depth_stencil_desc.DepthEnable = TRUE;
        depth_stencil_desc.DepthWriteMask =
            depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        // Same as Direct3D, but minus one (bit 0 less, 1 equal, 2 greater).
        depth_stencil_desc.DepthFunc =
            D3D11_COMPARISON_FUNC(uint32_t(D3D11_COMPARISON_NEVER) + uint32_t(depth_func));
        depth_stencil_used = true;
      }
      if (normalized_depth_control.stencil_enable) {
        depth_stencil_desc.StencilEnable = TRUE;
        depth_stencil_used = true;
        bool stencil_backface_enable =
            primitive_polygonal && normalized_depth_control.backface_enable;
        // No per-face masks in Direct3D: the back face ones only when drawing
        // only back faces.
        Register stencil_ref_mask_reg = (stencil_backface_enable && cull_front)
                                            ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                            : XE_GPU_REG_RB_STENCILREFMASK;
        auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        depth_stencil_desc.StencilReadMask = UINT8(stencil_ref_mask.stencilmask);
        depth_stencil_desc.StencilWriteMask = UINT8(stencil_ref_mask.stencilwritemask);
        auto stencil_op = [](xenos::StencilOp op) {
          return D3D11_STENCIL_OP(uint32_t(D3D11_STENCIL_OP_KEEP) + uint32_t(op));
        };
        auto compare = [](xenos::CompareFunction func) {
          return D3D11_COMPARISON_FUNC(uint32_t(D3D11_COMPARISON_NEVER) + uint32_t(func));
        };
        depth_stencil_desc.FrontFace.StencilFailOp = stencil_op(normalized_depth_control.stencilfail);
        depth_stencil_desc.FrontFace.StencilDepthFailOp =
            stencil_op(normalized_depth_control.stencilzfail);
        depth_stencil_desc.FrontFace.StencilPassOp =
            stencil_op(normalized_depth_control.stencilzpass);
        depth_stencil_desc.FrontFace.StencilFunc = compare(normalized_depth_control.stencilfunc);
        if (stencil_backface_enable) {
          depth_stencil_desc.BackFace.StencilFailOp =
              stencil_op(normalized_depth_control.stencilfail_bf);
          depth_stencil_desc.BackFace.StencilDepthFailOp =
              stencil_op(normalized_depth_control.stencilzfail_bf);
          depth_stencil_desc.BackFace.StencilPassOp =
              stencil_op(normalized_depth_control.stencilzpass_bf);
          depth_stencil_desc.BackFace.StencilFunc =
              compare(normalized_depth_control.stencilfunc_bf);
        } else {
          depth_stencil_desc.BackFace = depth_stencil_desc.FrontFace;
        }
      }
    }
    if (!pixel_shader && depth_float24_convert_in_pixel_shader_ && depth_stencil_used &&
        depth_stencil_desc.DepthEnable &&
        depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      pipeline.pixel_shader =
          depth_float24_round_ ? float24_round_ps_.Get() : float24_truncate_ps_.Get();
    }

    // Blending, with the factor mapping of Direct3D 12 (constant alpha as the
    // blend factor, color factors for alpha as their alpha versions).
    static const D3D11_BLEND kBlendFactorMap[32] = {
        /*  0 */ D3D11_BLEND_ZERO,
        /*  1 */ D3D11_BLEND_ONE,
        /*  2 */ D3D11_BLEND_ZERO,
        /*  3 */ D3D11_BLEND_ZERO,
        /*  4 */ D3D11_BLEND_SRC_COLOR,
        /*  5 */ D3D11_BLEND_INV_SRC_COLOR,
        /*  6 */ D3D11_BLEND_SRC_ALPHA,
        /*  7 */ D3D11_BLEND_INV_SRC_ALPHA,
        /*  8 */ D3D11_BLEND_DEST_COLOR,
        /*  9 */ D3D11_BLEND_INV_DEST_COLOR,
        /* 10 */ D3D11_BLEND_DEST_ALPHA,
        /* 11 */ D3D11_BLEND_INV_DEST_ALPHA,
        /* 12 */ D3D11_BLEND_BLEND_FACTOR,
        /* 13 */ D3D11_BLEND_INV_BLEND_FACTOR,
        /* 14 */ D3D11_BLEND_BLEND_FACTOR,
        /* 15 */ D3D11_BLEND_INV_BLEND_FACTOR,
        /* 16 */ D3D11_BLEND_SRC_ALPHA_SAT,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
    };
    static const D3D11_BLEND kBlendFactorAlphaMap[32] = {
        /*  0 */ D3D11_BLEND_ZERO,
        /*  1 */ D3D11_BLEND_ONE,
        /*  2 */ D3D11_BLEND_ZERO,
        /*  3 */ D3D11_BLEND_ZERO,
        /*  4 */ D3D11_BLEND_SRC_ALPHA,
        /*  5 */ D3D11_BLEND_INV_SRC_ALPHA,
        /*  6 */ D3D11_BLEND_SRC_ALPHA,
        /*  7 */ D3D11_BLEND_INV_SRC_ALPHA,
        /*  8 */ D3D11_BLEND_DEST_ALPHA,
        /*  9 */ D3D11_BLEND_INV_DEST_ALPHA,
        /* 10 */ D3D11_BLEND_DEST_ALPHA,
        /* 11 */ D3D11_BLEND_INV_DEST_ALPHA,
        /* 12 */ D3D11_BLEND_BLEND_FACTOR,
        /* 13 */ D3D11_BLEND_INV_BLEND_FACTOR,
        /* 14 */ D3D11_BLEND_BLEND_FACTOR,
        /* 15 */ D3D11_BLEND_INV_BLEND_FACTOR,
        /* 16 */ D3D11_BLEND_SRC_ALPHA_SAT,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
        D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO,
    };
    // 8 entries since 3 guest bits are used directly.
    static const D3D11_BLEND_OP kBlendOpMap[8] = {
        D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT,     D3D11_BLEND_OP_MIN,
        D3D11_BLEND_OP_MAX, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_ADD,
        D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_ADD};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i)))) {
        continue;
      }
      D3D11_RENDER_TARGET_BLEND_DESC& rt = blend_desc.RenderTarget[i];
      rt.RenderTargetWriteMask = UINT8((normalized_color_mask >> (i * 4)) & 0xF);
      if (!rt.RenderTargetWriteMask) {
        continue;
      }
      auto blendcontrol =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      D3D11_BLEND src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend) & 31];
      D3D11_BLEND dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend) & 31];
      D3D11_BLEND_OP blend_op = kBlendOpMap[uint32_t(blendcontrol.color_comb_fcn) & 7];
      D3D11_BLEND src_blend_alpha =
          kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend) & 31];
      D3D11_BLEND dest_blend_alpha =
          kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend) & 31];
      D3D11_BLEND_OP blend_op_alpha = kBlendOpMap[uint32_t(blendcontrol.alpha_comb_fcn) & 7];
      if (src_blend != D3D11_BLEND_ONE || dest_blend != D3D11_BLEND_ZERO ||
          blend_op != D3D11_BLEND_OP_ADD || src_blend_alpha != D3D11_BLEND_ONE ||
          dest_blend_alpha != D3D11_BLEND_ZERO || blend_op_alpha != D3D11_BLEND_OP_ADD) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = src_blend;
        rt.DestBlend = dest_blend;
        rt.BlendOp = blend_op;
        rt.SrcBlendAlpha = src_blend_alpha;
        rt.DestBlendAlpha = dest_blend_alpha;
        rt.BlendOpAlpha = blend_op_alpha;
      }
    }

    // 2x MSAA emulated with samples 0 and 3 of 4x when 2x is unsupported.
    xenos::MsaaSamples msaa_samples = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
    if (msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
      pipeline.sample_mask = 0b1001;
    }
  } else {
    // Rasterization is disabled in Direct3D by having no pixel shader and no
    // depth / stencil.
    pipeline.pixel_shader = nullptr;
  }

  pipeline.rasterizer_state = GetRasterizerState(rasterizer_desc);
  pipeline.depth_stencil_state = GetDepthStencilState(depth_stencil_desc);
  pipeline.blend_state = GetBlendState(blend_desc);
  if (!pipeline.rasterizer_state || !pipeline.depth_stencil_state || !pipeline.blend_state) {
    return nullptr;
  }

  // The objects are unique per description, so the pointers identify the
  // pipeline.
  uint64_t hash = XXH3_64bits(&pipeline, sizeof(pipeline));
  auto range = pipelines_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(it->second.get(), &pipeline, sizeof(pipeline))) {
      return it->second.get();
    }
  }
  auto stored = std::make_unique<Pipeline>(pipeline);
  const Pipeline* result = stored.get();
  pipelines_.emplace(hash, std::move(stored));
  return result;
}

ID3D11RasterizerState* D3D11PipelineCache::GetRasterizerState(const D3D11_RASTERIZER_DESC& desc) {
  uint64_t hash = HashDesc(desc);
  auto range = rasterizer_states_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second.desc, &desc, sizeof(desc))) {
      return it->second.state.Get();
    }
  }
  StateCacheEntry<D3D11_RASTERIZER_DESC, ID3D11RasterizerState> entry;
  entry.desc = desc;
  if (FAILED(command_processor_.GetDevice()->CreateRasterizerState(&desc, &entry.state))) {
    REXGPU_ERROR("D3D11: Failed to create a rasterizer state ({} exist, depth bias {})",
                 rasterizer_states_.size(), desc.DepthBias);
    return nullptr;
  }
  if (rasterizer_states_.size() + 1 == kStateObjectWarningCount) {
    REXGPU_WARN("D3D11: {} rasterizer states created, close to the device limit of 4096",
                kStateObjectWarningCount);
  }
  ID3D11RasterizerState* state = entry.state.Get();
  rasterizer_states_.emplace(hash, std::move(entry));
  return state;
}

ID3D11DepthStencilState* D3D11PipelineCache::GetDepthStencilState(
    const D3D11_DEPTH_STENCIL_DESC& desc) {
  uint64_t hash = HashDesc(desc);
  auto range = depth_stencil_states_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second.desc, &desc, sizeof(desc))) {
      return it->second.state.Get();
    }
  }
  StateCacheEntry<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState> entry;
  entry.desc = desc;
  if (FAILED(command_processor_.GetDevice()->CreateDepthStencilState(&desc, &entry.state))) {
    REXGPU_ERROR("D3D11: Failed to create a depth / stencil state ({} exist)",
                 depth_stencil_states_.size());
    return nullptr;
  }
  ID3D11DepthStencilState* state = entry.state.Get();
  depth_stencil_states_.emplace(hash, std::move(entry));
  return state;
}

ID3D11BlendState* D3D11PipelineCache::GetBlendState(const D3D11_BLEND_DESC& desc) {
  uint64_t hash = HashDesc(desc);
  auto range = blend_states_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second.desc, &desc, sizeof(desc))) {
      return it->second.state.Get();
    }
  }
  StateCacheEntry<D3D11_BLEND_DESC, ID3D11BlendState> entry;
  entry.desc = desc;
  if (FAILED(command_processor_.GetDevice()->CreateBlendState(&desc, &entry.state))) {
    REXGPU_ERROR("D3D11: Failed to create a blend state ({} exist)", blend_states_.size());
    return nullptr;
  }
  ID3D11BlendState* state = entry.state.Get();
  blend_states_.emplace(hash, std::move(entry));
  return state;
}

ID3D11GeometryShader* D3D11PipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key.key);
  if (it != geometry_shaders_.end()) {
    return it->second.Get();
  }
  std::vector<uint32_t> bytecode;
  CreateDxbcGeometryShader(key, bytecode);
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> shader;
  if (FAILED(command_processor_.GetDevice()->CreateGeometryShader(
          bytecode.data(), bytecode.size() * sizeof(uint32_t), nullptr, &shader))) {
    REXGPU_ERROR("D3D11: Failed to create the geometry shader {:08X}", key.key);
  }
  // Stored even if failed, to not retry.
  ID3D11GeometryShader* result = shader.Get();
  geometry_shaders_.emplace(key.key, std::move(shader));
  return result;
}

bool D3D11PipelineCache::GetGeometryShaderKey(
    GeometryShaderType geometry_shader_type,
    DxbcShaderTranslator::Modification vertex_shader_modification,
    DxbcShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == GeometryShaderType::kNone) {
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = rex::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  key_out = key;
  return true;
}

void D3D11PipelineCache::CreateDxbcGeometryShader(GeometryShaderKey key,
                                                  std::vector<uint32_t>& shader_out) {
  // Shader model 5.0 operand and declaration forms (no register spaces).
  ScopedShaderModel5_0 shader_model_5_0;
  shader_out.clear();
  uint32_t point_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t point_user_cull_distance_count =
      key.type == GeometryShaderType::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  bool point_recalculate_clip_distances = key.type == GeometryShaderType::kPointList &&
                                          point_clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == GeometryShaderType::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  // RDEF, ISGN, OSG5, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  shader_out.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + kBlobCount);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(shader_out.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // shader_out.resize(rdef_position_dwords);

  // RDEF header - the actual definitions will be written if needed.
  shader_out.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(shader_out, "Xenia");
  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(shader_out.data() + rdef_position_dwords);
    rdef_header.shader_model = dxbc::RdefShaderModel::kGeometryShader5_0;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_0;
    rdef_header.InitializeSizes(dxbc::kRdefInputBind5_0Size);
  }

  uint32_t system_cbuffer_size_vector_aligned_bytes = 0;

  if (key.type == GeometryShaderType::kPointList) {
    // Need point parameters from the system constants.

    // Constant types - float2 only.
    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_float2 = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "float2");
    // Types.
    uint32_t rdef_type_float2_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_type_float2_ptr =
        uint32_t((rdef_type_float2_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    shader_out.resize(rdef_type_float2_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
    {
      auto& rdef_type_float2 =
          *reinterpret_cast<dxbc::RdefType*>(shader_out.data() + rdef_type_float2_position_dwords);
      rdef_type_float2.variable_class = dxbc::RdefVariableClass::kVector;
      rdef_type_float2.variable_type = dxbc::RdefVariableType::kFloat;
      rdef_type_float2.row_count = 1;
      rdef_type_float2.column_count = 2;
      rdef_type_float2.name_ptr = rdef_name_ptr_float2;
    }

    // Constants:
    // - float2 xe_point_constant_diameter
    // - float2 xe_point_screen_diameter_to_ndc_radius
    enum PointConstant : uint32_t {
      kPointConstantConstantDiameter,
      kPointConstantScreenDiameterToNDCRadius,
      kPointConstantCount,
    };
    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_point_constant_diameter = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_constant_diameter");
    uint32_t rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_screen_diameter_to_ndc_radius");
    // Constants.
    uint32_t rdef_constants_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_constants_ptr =
        uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    shader_out.resize(rdef_constants_position_dwords +
                      sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * kPointConstantCount);
    {
      auto rdef_constants =
          reinterpret_cast<dxbc::RdefVariable*>(shader_out.data() + rdef_constants_position_dwords);
      // float2 xe_point_constant_diameter
      static_assert(sizeof(DxbcShaderTranslator::SystemConstants ::point_constant_diameter) ==
                        sizeof(float) * 2,
                    "DxbcShaderTranslator point_constant_diameter system constant size "
                    "differs between the shader translator and geometry shader "
                    "generation");
      static_assert_size(DxbcShaderTranslator::SystemConstants::point_constant_diameter,
                         sizeof(float) * 2);
      dxbc::RdefVariable& rdef_constant_point_constant_diameter =
          rdef_constants[kPointConstantConstantDiameter];
      rdef_constant_point_constant_diameter.name_ptr = rdef_name_ptr_xe_point_constant_diameter;
      rdef_constant_point_constant_diameter.start_offset_bytes =
          offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter);
      rdef_constant_point_constant_diameter.size_bytes = sizeof(float) * 2;
      rdef_constant_point_constant_diameter.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_constant_diameter.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_constant_diameter.start_texture = UINT32_MAX;
      rdef_constant_point_constant_diameter.start_sampler = UINT32_MAX;
      // float2 xe_point_screen_diameter_to_ndc_radius
      static_assert(
          sizeof(DxbcShaderTranslator::SystemConstants ::point_screen_diameter_to_ndc_radius) ==
              sizeof(float) * 2,
          "DxbcShaderTranslator point_screen_diameter_to_ndc_radius system "
          "constant size differs between the shader translator and geometry "
          "shader generation");
      dxbc::RdefVariable& rdef_constant_point_screen_diameter_to_ndc_radius =
          rdef_constants[kPointConstantScreenDiameterToNDCRadius];
      rdef_constant_point_screen_diameter_to_ndc_radius.name_ptr =
          rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_offset_bytes =
          offsetof(DxbcShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius);
      rdef_constant_point_screen_diameter_to_ndc_radius.size_bytes = sizeof(float) * 2;
      rdef_constant_point_screen_diameter_to_ndc_radius.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_screen_diameter_to_ndc_radius.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_texture = UINT32_MAX;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_sampler = UINT32_MAX;
    }

    // Constant buffers - xe_system_cbuffer only.

    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_system_cbuffer = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_system_cbuffer");
    // Constant buffers.
    uint32_t rdef_cbuffer_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_cbuffer_position_dwords + sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t));
    {
      auto& rdef_cbuffer_system =
          *reinterpret_cast<dxbc::RdefCbuffer*>(shader_out.data() + rdef_cbuffer_position_dwords);
      rdef_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_cbuffer_system.variable_count = kPointConstantCount;
      rdef_cbuffer_system.variables_ptr = rdef_constants_ptr;
      auto rdef_constants = reinterpret_cast<const dxbc::RdefVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      for (uint32_t i = 0; i < kPointConstantCount; ++i) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     rdef_constants[i].start_offset_bytes + rdef_constants[i].size_bytes);
      }
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) +
                              sizeof(float) * 3));
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) +
                              sizeof(float) * 3));
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                              sizeof(float) * 4 * 6));
      }
      system_cbuffer_size_vector_aligned_bytes =
          rex::align(system_cbuffer_size_vector_aligned_bytes, uint32_t(sizeof(uint32_t) * 4));
      rdef_cbuffer_system.size_vector_aligned_bytes = system_cbuffer_size_vector_aligned_bytes;
    }

    // Bindings - xe_system_cbuffer only.
    uint32_t rdef_binding_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_binding_position_dwords +
                      sizeof(dxbc::RdefInputBind) / sizeof(uint32_t));
    {
      auto& rdef_binding_cbuffer_system =
          *reinterpret_cast<dxbc::RdefInputBind*>(shader_out.data() + rdef_binding_position_dwords);
      rdef_binding_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_binding_cbuffer_system.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_cbuffer_system.bind_point =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      rdef_binding_cbuffer_system.bind_count = 1;
      rdef_binding_cbuffer_system.flags = dxbc::kRdefInputFlagUserPacked;
    }
    shader_out.resize(rdef_binding_position_dwords +
                      dxbc::kRdefInputBind5_0Size / sizeof(uint32_t));

    // Pointers in the header.
    {
      auto& rdef_header =
          *reinterpret_cast<dxbc::RdefHeader*>(shader_out.data() + rdef_position_dwords);
      rdef_header.cbuffer_count = 1;
      rdef_header.cbuffers_ptr =
          uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
      rdef_header.input_bind_count = 1;
      rdef_header.input_binds_ptr =
          uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Clip and cull distances are tightly packed together into registers, but
  // have separate signature parameters with each being a vec4-aligned window.
  uint32_t input_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t input_cull_distance_count =
      (key.user_clip_plane_cull ? key.user_clip_plane_count : 0) + key.has_vertex_kill_and;
  uint32_t input_clip_and_cull_distance_count =
      input_clip_distance_count + input_cull_distance_count;

  // Interpolators, position, clip and cull distances (parameters containing
  // only clip or cull distances, and also one parameter containing both if
  // present), point size.
  uint32_t isgn_parameter_count =
      key.interpolator_count + 1 + ((input_clip_and_cull_distance_count + 3) / 4) +
      uint32_t(input_cull_distance_count && (input_clip_distance_count & 3) != 0) +
      key.has_point_size;

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * isgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((shader_out.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t isgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t isgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }
  uint32_t isgn_name_ptr_sv_cull_distance = name_ptr;
  if (input_cull_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_CullDistance");
  }
  uint32_t isgn_name_ptr_xepsize = name_ptr;
  if (key.has_point_size) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XEPSIZE");
  }

  // Header and parameters.
  uint32_t input_register_interpolators = UINT32_MAX;
  uint32_t input_register_position;
  uint32_t input_register_clip_and_cull_distances = UINT32_MAX;
  uint32_t input_register_point_size = UINT32_MAX;
  {
    // Header.
    auto& isgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_out.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    uint32_t input_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      input_register_interpolators = input_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(isgn_parameter_index < isgn_parameter_count);
        dxbc::SignatureParameter& isgn_interpolator = isgn_parameters[isgn_parameter_index++];
        isgn_interpolator.semantic_name_ptr = isgn_name_ptr_texcoord;
        isgn_interpolator.semantic_index = i;
        isgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        isgn_interpolator.register_index = input_register_index++;
        isgn_interpolator.mask = 0b1111;
        isgn_interpolator.always_reads_mask = 0b1111;
      }
    }

    // Position (SV_Position).
    input_register_position = input_register_index;
    assert_true(isgn_parameter_index < isgn_parameter_count);
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_name_ptr_sv_position;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = input_register_index++;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b1111;

    // Clip and cull distances (SV_ClipDistance#, SV_CullDistance#).
    if (input_clip_and_cull_distance_count) {
      input_register_clip_and_cull_distances = input_register_index;
      uint32_t isgn_cull_distance_semantic_index = 0;
      for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
        if (i < input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_clip_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_clip_distance.semantic_name_ptr = isgn_name_ptr_sv_clip_distance;
          isgn_sv_clip_distance.semantic_index = i / 4;
          isgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
          isgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_clip_distance.register_index = input_register_index;
          uint8_t isgn_sv_clip_distance_mask =
              (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
          isgn_sv_clip_distance.mask = isgn_sv_clip_distance_mask;
          isgn_sv_clip_distance.always_reads_mask = isgn_sv_clip_distance_mask;
        }
        if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_cull_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_cull_distance.semantic_name_ptr = isgn_name_ptr_sv_cull_distance;
          isgn_sv_cull_distance.semantic_index = isgn_cull_distance_semantic_index++;
          isgn_sv_cull_distance.system_value = dxbc::Name::kCullDistance;
          isgn_sv_cull_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_cull_distance.register_index = input_register_index;
          uint8_t isgn_sv_cull_distance_mask =
              (UINT8_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
          if (i < input_clip_distance_count) {
            isgn_sv_cull_distance_mask &= ~((UINT8_C(1) << (input_clip_distance_count - i)) - 1);
          }
          isgn_sv_cull_distance.mask = isgn_sv_cull_distance_mask;
          isgn_sv_cull_distance.always_reads_mask = isgn_sv_cull_distance_mask;
        }
        ++input_register_index;
      }
    }

    // Point size (XEPSIZE).
    if (key.has_point_size) {
      input_register_point_size = input_register_index;
      assert_true(isgn_parameter_index < isgn_parameter_count);
      dxbc::SignatureParameter& isgn_point_size = isgn_parameters[isgn_parameter_index++];
      isgn_point_size.semantic_name_ptr = isgn_name_ptr_xepsize;
      isgn_point_size.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      isgn_point_size.register_index = input_register_index++;
      isgn_point_size.mask = 0b0001;
      isgn_point_size.always_reads_mask =
          key.type == GeometryShaderType::kPointList ? 0b0001 : 0;
    }

    assert_true(isgn_parameter_index == isgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Interpolators, point coordinates, position, clip distances.
  uint32_t osgn_parameter_count = key.interpolator_count + key.has_point_coordinates + 1 +
                                  ((input_clip_distance_count + 3) / 4);

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameterForGS) / sizeof(uint32_t) *
                        osgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((shader_out.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t osgn_name_ptr_xespritetexcoord = name_ptr;
  if (key.has_point_coordinates) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XESPRITETEXCOORD");
  }
  uint32_t osgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t osgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }

  // Header and parameters.
  uint32_t output_register_interpolators = UINT32_MAX;
  uint32_t output_register_point_coordinates = UINT32_MAX;
  uint32_t output_register_position;
  uint32_t output_register_clip_distances = UINT32_MAX;
  {
    // Header.
    auto& osgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameterForGS*>(
        shader_out.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t osgn_parameter_index = 0;
    uint32_t output_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      output_register_interpolators = output_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(osgn_parameter_index < osgn_parameter_count);
        dxbc::SignatureParameterForGS& osgn_interpolator = osgn_parameters[osgn_parameter_index++];
        osgn_interpolator.semantic_name_ptr = osgn_name_ptr_texcoord;
        osgn_interpolator.semantic_index = i;
        osgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_interpolator.register_index = output_register_index++;
        osgn_interpolator.mask = 0b1111;
      }
    }

    // Point coordinates (XESPRITETEXCOORD).
    if (key.has_point_coordinates) {
      output_register_point_coordinates = output_register_index;
      assert_true(osgn_parameter_index < osgn_parameter_count);
      dxbc::SignatureParameterForGS& osgn_point_coordinates =
          osgn_parameters[osgn_parameter_index++];
      osgn_point_coordinates.semantic_name_ptr = osgn_name_ptr_xespritetexcoord;
      osgn_point_coordinates.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_point_coordinates.register_index = output_register_index++;
      osgn_point_coordinates.mask = 0b0011;
      osgn_point_coordinates.never_writes_mask = 0b1100;
    }

    // Position (SV_Position).
    output_register_position = output_register_index;
    assert_true(osgn_parameter_index < osgn_parameter_count);
    dxbc::SignatureParameterForGS& osgn_sv_position = osgn_parameters[osgn_parameter_index++];
    osgn_sv_position.semantic_name_ptr = osgn_name_ptr_sv_position;
    osgn_sv_position.system_value = dxbc::Name::kPosition;
    osgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    osgn_sv_position.register_index = output_register_index++;
    osgn_sv_position.mask = 0b1111;

    // Clip distances (SV_ClipDistance#).
    if (input_clip_distance_count) {
      output_register_clip_distances = output_register_index;
      for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
        dxbc::SignatureParameterForGS& osgn_sv_clip_distance =
            osgn_parameters[osgn_parameter_index++];
        osgn_sv_clip_distance.semantic_name_ptr = osgn_name_ptr_sv_clip_distance;
        osgn_sv_clip_distance.semantic_index = i / 4;
        osgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
        osgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_sv_clip_distance.register_index = output_register_index++;
        uint8_t osgn_sv_clip_distance_mask =
            (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
        osgn_sv_clip_distance.mask = osgn_sv_clip_distance_mask;
        osgn_sv_clip_distance.never_writes_mask = osgn_sv_clip_distance_mask ^ 0b1111;
      }
    }

    assert_true(osgn_parameter_index == osgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignatureForGS;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(shex_position_dwords);

  shader_out.push_back(dxbc::VersionToken(dxbc::ProgramType::kGeometryShader, 5, 0));
  // Reserve space for the length token.
  shader_out.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(shader_out, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);

  if (system_cbuffer_size_vector_aligned_bytes) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, 0,
                      uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                      uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants)),
        system_cbuffer_size_vector_aligned_bytes / (sizeof(uint32_t) * 4));
  }

  dxbc::Primitive input_primitive = dxbc::Primitive::kUndefined;
  uint32_t input_primitive_vertex_count = 0;
  dxbc::PrimitiveTopology output_primitive_topology = dxbc::PrimitiveTopology::kUndefined;
  uint32_t max_output_vertex_count = 0;
  switch (key.type) {
    case GeometryShaderType::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kPoint;
      input_primitive_vertex_count = 1;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case GeometryShaderType::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kTriangle;
      input_primitive_vertex_count = 3;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case GeometryShaderType::kQuadList:
      // 4 vertices passed via kLineWithAdjacency to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kLineWithAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  assert_false(key.interpolator_count && input_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_interpolators + i));
  }
  a.OpDclInputSIV(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_position),
                  dxbc::Name::kPosition);
  // Clip and cull plane declarations are separate in FXC-generated code even
  // for a single register.
  assert_false(input_clip_and_cull_distance_count &&
               input_register_clip_and_cull_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
    if (i < input_clip_distance_count) {
      a.OpDclInput(dxbc::Dest::V2D(
          input_primitive_vertex_count, input_register_clip_and_cull_distances + (i >> 2),
          (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1));
    }
    if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
      uint32_t cull_distance_mask =
          (UINT32_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
      if (i < input_clip_distance_count) {
        cull_distance_mask &= ~((UINT32_C(1) << (input_clip_distance_count - i)) - 1);
      }
      a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count,
                                   input_register_clip_and_cull_distances + (i >> 2),
                                   cull_distance_mask));
    }
  }
  if (key.has_point_size && key.type == GeometryShaderType::kPointList) {
    assert_true(input_register_point_size != UINT32_MAX);
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_point_size, 0b0001));
  }

  // At least 1 temporary register needed to discard primitives with NaN
  // position.
  size_t dcl_temps_count_position_dwords = a.OpDclTemps(1);

  a.OpDclInputPrimitive(input_primitive);
  dxbc::Dest stream(dxbc::Dest::M(0));
  a.OpDclStream(stream);
  a.OpDclOutputTopology(output_primitive_topology);

  assert_false(key.interpolator_count && output_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclOutput(dxbc::Dest::O(output_register_interpolators + i));
  }
  if (key.has_point_coordinates) {
    assert_true(output_register_point_coordinates != UINT32_MAX);
    a.OpDclOutput(dxbc::Dest::O(output_register_point_coordinates, 0b0011));
  }
  a.OpDclOutputSIV(dxbc::Dest::O(output_register_position), dxbc::Name::kPosition);
  assert_false(input_clip_distance_count && output_register_clip_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
    a.OpDclOutputSIV(
        dxbc::Dest::O(output_register_clip_distances + (i >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1),
        dxbc::Name::kClipDistance);
  }

  a.OpDclMaxOutputVertexCount(max_output_vertex_count);

  // Note that after every emit, all o# become initialized and must be written
  // to again.
  // Also, FXC generates only movs (from statically or dynamically indexed
  // v[#][#], from r#, or from a literal) to o# for some reason.
  // emit_then_cut_stream must not be used - it crashes the shader compiler of
  // AMD Software: Adrenalin Edition 23.3.2 on RDNA 3 if it's conditional (after
  // a `retc` or inside an `if`), and it doesn't seem to be generated by FXC or
  // DXC at all.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    a.OpNE(dxbc::Dest::R(0), dxbc::Src::V2D(i, input_register_position),
           dxbc::Src::V2D(i, input_register_position));
    a.OpOr(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b1110));
    a.OpOr(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
    a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // For point lists with ps_ucp_mode 3, user cull plane distances are
  // calculated per expanded vertex later.
  if (input_cull_distance_count) {
    uint32_t cull_distance_start =
        point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    for (uint32_t i = cull_distance_start; i < input_cull_distance_count; ++i) {
      uint32_t cull_distance_register =
          input_register_clip_and_cull_distances + ((input_clip_distance_count + i) >> 2);
      uint32_t cull_distance_component = (input_clip_distance_count + i) & 3;
      a.OpLT(dxbc::Dest::R(0, 0b0001),
             dxbc::Src::V2D(0, cull_distance_register).Select(cull_distance_component),
             dxbc::Src::LF(0.0f));
      for (uint32_t j = 1; j < input_primitive_vertex_count; ++j) {
        a.OpLT(dxbc::Dest::R(0, 0b0010),
               dxbc::Src::V2D(j, cull_distance_register).Select(cull_distance_component),
               dxbc::Src::LF(0.0f));
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::R(0, dxbc::Src::kYYYY));
      }
      a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
    }
  }

  switch (key.type) {
    case GeometryShaderType::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.
      dxbc::Src point_size_src(dxbc::Src::CB(
          0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
          offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter) >> 4,
          ((offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter[0]) >> 2) & 3) |
              (((offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter[1]) >> 2) &
                3)
               << 2)));
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        a.OpGE(dxbc::Dest::R(0, 0b0001),
               dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), dxbc::Src::LF(0.0f));
        a.OpMovC(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), point_size_src);
        point_size_src = dxbc::Src::R(0, 0b0100);
      }
      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      // XY may contain the point size with the per-vertex override applied, use
      // Z as temporary.
      for (uint32_t i = 0; i < 2; ++i) {
        a.OpLT(dxbc::Dest::R(0, 0b0100), dxbc::Src::LF(0.0f), point_size_src.SelectFromSwizzled(i));
        a.OpRetC(false, dxbc::Src::R(0, dxbc::Src::kZZZZ));
      }
      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                            offsetof(DxbcShaderTranslator::SystemConstants,
                                     point_screen_diameter_to_ndc_radius) >>
                                4,
                            ((offsetof(DxbcShaderTranslator::SystemConstants,
                                       point_screen_diameter_to_ndc_radius[0]) >>
                              2) &
                             3) |
                                (((offsetof(DxbcShaderTranslator::SystemConstants,
                                            point_screen_diameter_to_ndc_radius[1]) >>
                                   2) &
                                  3)
                                 << 2)));
      point_size_src = dxbc::Src::R(0, 0b0100);
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
      dxbc::Src point_radius_x_src(point_size_src.SelectFromSwizzled(0));
      dxbc::Src point_radius_y_src(point_size_src.SelectFromSwizzled(1));

      if (point_recalculate_cull_distances) {
        stat.temp_register_count = std::max(UINT32_C(4), stat.temp_register_count);
        dxbc::Src ndc_scale_xy(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4,
            ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3) << 2)));
        dxbc::Src ndc_scale_z(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
        dxbc::Src ndc_offset_xy(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4,
            ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                 << 2)));
        dxbc::Src ndc_offset_z(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
        for (uint32_t j = 0; j < point_user_cull_distance_count; ++j) {
          for (uint32_t i = 0; i < 4; ++i) {
            a.OpAdd(dxbc::Dest::R(2, 0b0001),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                    (i & 1) ? point_radius_x_src : -point_radius_x_src);
            a.OpAdd(dxbc::Dest::R(2, 0b0010),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                    (i >> 1) ? -point_radius_y_src : point_radius_y_src);
            a.OpMov(dxbc::Dest::R(2, 0b0100),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
            a.OpMov(dxbc::Dest::R(2, 0b1000),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
            a.OpMAd(dxbc::Dest::R(2, 0b0011), -ndc_offset_xy, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, 0b0011));
            a.OpDiv(dxbc::Dest::R(2, 0b0011), dxbc::Src::R(2, 0b0011), ndc_scale_xy);
            a.OpMAd(dxbc::Dest::R(2, 0b0100), -ndc_offset_z, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, dxbc::Src::kZZZZ));
            a.OpDiv(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ), ndc_scale_z);
            a.OpDP4(
                dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2),
                dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                              (offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                               sizeof(float) * 4 * j) >>
                                  4,
                              dxbc::Src::kXYZW));
            a.OpLT(dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX),
                   dxbc::Src::LF(0.0f));
            if (i == 0) {
              a.OpMov(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX));
            } else {
              a.OpAnd(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
                      dxbc::Src::R(2, dxbc::Src::kXXXX));
            }
          }
          a.OpRetC(true, dxbc::Src::R(3, dxbc::Src::kXXXX));
        }
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(0, input_register_interpolators + j));
        }
        // Top-left, top-right, bottom-left, bottom-right order (chosen
        // arbitrarily, simply based on clockwise meaning front with
        // FrontCounterClockwise = FALSE, but faceness is ignored for
        // non-polygon primitive types).
        // Bottom is -Y in Direct3D NDC, +V in point sprite coordinates.
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(float(i & 1), float(i >> 1), 0.0f, 0.0f));
        }
        // FXC generates only `mov`s for o#, use temporary registers (r0.zw, as
        // r0.xy already used for the point size) for calculations.
        a.OpAdd(dxbc::Dest::R(0, 0b0100),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                (i & 1) ? point_radius_x_src : -point_radius_x_src);
        a.OpAdd(dxbc::Dest::R(0, 0b1000),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                (i >> 1) ? -point_radius_y_src : point_radius_y_src);
        a.OpMov(dxbc::Dest::O(output_register_position, 0b0011), dxbc::Src::R(0, 0b1110));
        a.OpMov(dxbc::Dest::O(output_register_position, 0b1100),
                dxbc::Src::V2D(0, input_register_position));
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying
          // user clip planes.
          dxbc::Src ndc_scale_xy(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4,
              ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                  (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_scale_z(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
          dxbc::Src ndc_offset_xy(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4,
              ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                  (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_offset_z(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(0, 0b1110));
          a.OpMov(dxbc::Dest::R(1, 0b0100),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b1000),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
          a.OpMAd(dxbc::Dest::R(1, 0b0011), -ndc_offset_xy, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, 0b0100));
          a.OpDiv(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(1, 0b0100), ndc_scale_xy);
          a.OpMAd(dxbc::Dest::R(1, 0b0100), -ndc_offset_z, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, dxbc::Src::kZZZZ));
          a.OpDiv(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(1, dxbc::Src::kZZZZ), ndc_scale_z);
          for (uint32_t j = 0; j < input_clip_distance_count; ++j) {
            a.OpDP4(
                dxbc::Dest::O(output_register_clip_distances + (j >> 2), UINT32_C(1) << (j & 3)),
                dxbc::Src::R(1),
                dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                              (offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                               sizeof(float) * 4 * j) >>
                                  4,
                              dxbc::Src::kXYZW));
          }
        } else {
          for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
            a.OpMov(dxbc::Dest::O(
                        output_register_clip_distances + (j >> 2),
                        (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                    dxbc::Src::V2D(0, input_register_clip_and_cull_distances + (j >> 2)));
          }
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    case GeometryShaderType::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]
      //
      // Input vertices are implicitly indexable, dcl_indexRange is not needed
      // for the first dimension of a v[#][#] index.

      // Get squares of edge lengths into r0.xyz to choose the longest edge.
      // r0.x = ||12||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0011), dxbc::Src::V2D(2, input_register_position, 0b0100),
              -dxbc::Src::V2D(1, input_register_position, 0b0100));
      a.OpDP2(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b0100));
      // r0.y = ||20||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0110), dxbc::Src::V2D(0, input_register_position, 0b0100 << 2),
              -dxbc::Src::V2D(2, input_register_position, 0b0100 << 2));
      a.OpDP2(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, 0b1001), dxbc::Src::R(0, 0b1001));
      // r0.z = ||01||^2
      a.OpAdd(dxbc::Dest::R(0, 0b1100), dxbc::Src::V2D(1, input_register_position, 0b0100 << 4),
              -dxbc::Src::V2D(0, input_register_position, 0b0100 << 4));
      a.OpDP2(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, 0b1110), dxbc::Src::R(0, 0b1110));

      // Find the longest edge, and select the strip vertex indices into r0.xyz.
      // r0.w = 12 > 20
      a.OpLT(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 01
      a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 20 && 12 > 01
      a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
              dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
      {
        // 12 is the longest edge, the first triangle in the strip is 012.
        a.OpMov(dxbc::Dest::R(0, 0b0111), dxbc::Src::LU(0, 1, 2, 0));
      }
      a.OpElse();
      {
        // r0.x = 20 > 01
        a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
               dxbc::Src::R(0, dxbc::Src::kYYYY));
        // If 20 is the longest edge, the first triangle in the strip is 120.
        // Otherwise, it's 201.
        a.OpMovC(dxbc::Dest::R(0, 0b0111), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1, 2, 0, 0), dxbc::Src::LU(2, 0, 1, 0));
      }
      a.OpEndIf();

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        dxbc::Index input_vertex_index(0, i);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }

      // Construct the fourth vertex using r1 as temporary storage, including
      // for the final operation as FXC generates only `mov`s for o#.
      stat.temp_register_count = std::max(UINT32_C(2), stat.temp_register_count);
      for (uint32_t j = 0; j < key.interpolator_count; ++j) {
        uint32_t input_register_interpolator = input_register_interpolators + j;
        a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_interpolator),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_interpolator));
        a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_interpolator));
        a.OpMov(dxbc::Dest::O(output_register_interpolators + j), dxbc::Src::R(1));
      }
      if (key.has_point_coordinates) {
        a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
      }
      a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_position),
              dxbc::Src::V2D(dxbc::Index(0, 1), input_register_position));
      a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
              dxbc::Src::V2D(dxbc::Index(0, 2), input_register_position));
      a.OpMov(dxbc::Dest::O(output_register_position), dxbc::Src::R(1));
      for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
        uint32_t clip_distance_mask =
            (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1;
        uint32_t input_register_clip_distance = input_register_clip_and_cull_distances + (j >> 2);
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask),
                -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_clip_distance),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_clip_distance));
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_clip_distance));
        a.OpMov(dxbc::Dest::O(output_register_clip_distances + (j >> 2), clip_distance_mask),
                dxbc::Src::R(1));
      }
      a.OpEmitStream(stream);
      a.OpCutStream(stream);
    } break;

    case GeometryShaderType::kQuadList: {
      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t input_vertex_index = i ^ (i >> 1);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  a.OpRet();

  // Write the actual number of temporary registers used.
  shader_out[dcl_temps_count_position_dwords] = stat.temp_register_count;

  // Write the shader program length in dwords.
  shader_out[shex_position_dwords + 1] = uint32_t(shader_out.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(shader_out.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t shader_size_bytes = uint32_t(shader_out.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(shader_out.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(shader_out.data()),
                          static_cast<unsigned int>(shader_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }
}

}  // namespace rex::graphics::d3d11
