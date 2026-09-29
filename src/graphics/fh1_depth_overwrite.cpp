#include <rex/graphics/fh1_depth_overwrite.h>

#include <algorithm>
#include <cmath>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::graphics {

void Fh1DepthOverwrite::PositionSink::Export(ucode::ExportRegister export_register,
                                                 const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (1u << i)) position[i] = value[i];
    }
    position_mask |= value_mask;
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex &&
             (value_mask & 0b0100) &&
             (rex::memory::Reinterpret<uint32_t>(value[2]) & ~(UINT32_C(1) << 31))) {
    killed = true;
  }
}

bool Fh1DepthOverwrite::Derive(const Fh1DrawInfo& draw, bool half_pixel_offset,
                               bool& stencil_overwritten) {
  const RegisterFile& regs = register_file_;
  const reg::RB_DEPTHCONTROL depth_control = draw.normalized_depth_control;
  if (!draw.vertex_shader || !depth_control.z_enable || !depth_control.z_write_enable ||
      depth_control.zfunc != xenos::CompareFunction::kAlways) {
    return false;
  }
  // Stencil must not fail anywhere; it is either rewritten or kept.
  const auto stencil_front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  stencil_overwritten = false;
  if (depth_control.stencil_enable) {
    if (depth_control.stencilfunc != xenos::CompareFunction::kAlways ||
        (depth_control.backface_enable &&
         depth_control.stencilfunc_bf != xenos::CompareFunction::kAlways)) {
      return false;
    }
    stencil_overwritten =
        depth_control.stencilzpass == xenos::StencilOp::kReplace &&
        stencil_front.stencilwritemask == 0xFF &&
        (!depth_control.backface_enable ||
         (depth_control.stencilzpass_bf == xenos::StencilOp::kReplace &&
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilwritemask ==
              0xFF &&
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).stencilref ==
              stencil_front.stencilref));
  }
  if (draw.pixel_shader &&
      (draw.pixel_shader->kills_pixels() || draw.pixel_shader->writes_depth())) {
    return false;
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if ((draw.pixel_shader && color_control.alpha_test_enable &&
       color_control.alpha_func != xenos::CompareFunction::kAlways) ||
      color_control.alpha_to_mask_enable || (regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFF) != 0xFFFF) {
    return false;
  }
  const auto mode_control = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (mode_control.cull_front || mode_control.cull_back ||
      mode_control.poly_mode != xenos::PolygonModeEnable::kDisabled) {
    return false;
  }
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex || !initiator.num_indices ||
      initiator.num_indices > 12 || initiator.num_indices % 3 ||
      (xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
       regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
           xenos::VGTOutputPath::kTessellationEnable) ||
      !ShaderInterpreter::CanInterpretShader(*draw.vertex_shader)) {
    return false;
  }

  const uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  const uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const bool clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  const float scale[3] = {
      vte.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
      vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f,
      vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f};
  float offset[3] = {
      vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
      vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f,
      vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f};
  if (mode_control.vtx_window_offset_enable) {
    const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset[0] += float(window_offset.window_x_offset);
    offset[1] += float(window_offset.window_y_offset);
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const float pixel_offset = half_pixel_offset &&
                                     regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
                                         xenos::PixelCenter::kD3DZero
                                 ? 0.5f
                                 : 0.0f;
  rects_.clear();
  interpreter_.SetShader(*draw.vertex_shader);
  PositionSink sink;
  interpreter_.SetExportSink(&sink);
  bool ok = true;
  for (uint32_t first = 0; ok && first < initiator.num_indices; first += 3) {
    float corners[3][2];
    for (uint32_t i = 0; ok && i < 3; ++i) {
      sink = PositionSink();
      interpreter_.temp_registers()[0] = float(std::min(
          max_index, std::max(min_index, (first + i + index_offset) & 0xFFFFFF)));
      interpreter_.Execute();
      if (sink.killed || (sink.position_mask & 0b1111) != 0b1111) {
        ok = false;
        break;
      }
      float position[3] = {sink.position[0], sink.position[1], sink.position[2]};
      const float w = sink.position[3];
      if (!vte.vtx_xy_fmt || !vte.vtx_z_fmt) {
        if (!(w > 0.0f)) {
          ok = false;
          break;
        }
      }
      if (!vte.vtx_xy_fmt) {
        position[0] /= w;
        position[1] /= w;
      }
      if (!vte.vtx_z_fmt) position[2] /= w;
      // Clipped depth could drop the rectangle.
      if (clip && !(position[2] >= 0.0f && position[2] <= 1.0f)) {
        ok = false;
        break;
      }
      for (uint32_t j = 0; j < 2; ++j) {
        corners[i][j] = position[j] * scale[j] + offset[j];
        if (!std::isfinite(corners[i][j])) ok = false;
      }
    }
    if (!ok) break;
    // The rectangle list completes three corners of an axis-aligned rectangle
    // into four: two distinct values per axis.
    float bounds[4];
    for (uint32_t j = 0; j < 2; ++j) {
      const float a = corners[0][j], b = corners[1][j], c = corners[2][j];
      const bool two_values = (a == b || b == c || a == c) && !(a == b && b == c);
      if (!two_values) ok = false;
      bounds[j] = std::min({a, b, c});
      bounds[2 + j] = std::max({a, b, c});
    }
    if (!ok) break;
    // Inner: pixels whose whole area is inside as the host rasterizes the
    // rectangle (samples are never on pixel edges). Outer: pixels any part of
    // which is inside.
    auto bound = [&](bool outer) {
      std::array<int32_t, 4> rect;
      for (uint32_t j = 0; j < 2; ++j) {
        const float low = bounds[j] + pixel_offset, high = bounds[2 + j] + pixel_offset;
        rect[j] = int32_t(outer ? std::floor(low) : std::ceil(low));
        rect[2 + j] = int32_t(outer ? std::ceil(high) : std::floor(high));
        if (clip) {
          const float viewport_low = offset[j] + pixel_offset - std::abs(scale[j]);
          const float viewport_high = offset[j] + pixel_offset + std::abs(scale[j]);
          rect[j] = std::max(rect[j], int32_t(outer ? std::floor(viewport_low)
                                                    : std::ceil(viewport_low)));
          rect[2 + j] = std::min(rect[2 + j], int32_t(outer ? std::ceil(viewport_high)
                                                            : std::floor(viewport_high)));
        }
        rect[j] = std::max({rect[j], int32_t(scissor.offset[j]), int32_t(0)});
        rect[2 + j] = std::min(rect[2 + j], int32_t(scissor.offset[j] + scissor.extent[j]));
        if (rect[2 + j] < rect[j]) rect[2 + j] = rect[j];
      }
      return rect;
    };
    const Rect rect = {bound(false), bound(true)};
    if (rect.outer[0] < rect.outer[2] && rect.outer[1] < rect.outer[3]) {
      rects_.push_back(rect);
    }
  }
  interpreter_.SetExportSink(nullptr);
  return ok;
}

}  // namespace rex::graphics
