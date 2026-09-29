#include <rex/graphics/fh1_edram_resolve.h>

#include <algorithm>

#include <rex/graphics/register_file.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

namespace rex::graphics {

bool Fh1ResolveRectangle(const RegisterFile& regs, const memory::Memory& memory, int32_t& x0,
                      int32_t& y0, int32_t& x1, int32_t& y1) {
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) return false;
  const float* vertices =
      reinterpret_cast<const float*>(memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  if (!vertices) return false;
  const float half_pixel =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (size_t i = 0; i < 6; ++i) {
    fixed[i] = ui::FloatToD3D11Fixed16p8(xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel);
  }
  x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    x1 += offset.window_x_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  const int32_t right = int32_t(scissor.offset[0] + scissor.extent[0]);
  const int32_t bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), bottom);
  constexpr int32_t kAlign = int32_t(xenos::kResolveAlignmentPixels);
  x0 &= ~(kAlign - 1);
  y0 &= ~(kAlign - 1);
  x1 = (x1 + kAlign - 1) & ~(kAlign - 1);
  y1 = (y1 + kAlign - 1) & ~(kAlign - 1);
  const int32_t pitch =
      int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch & ~uint32_t(kAlign - 1));
  x0 = std::min(x0, pitch);
  x1 = std::min(x1, pitch);
  return x0 < x1 && y0 < y1;
}

bool Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool Fh1PlanResolve(const RegisterFile& regs, const memory::Memory& memory,
                    const Fh1ResolveFlags& flags, Fh1ResolvePlan& plan) {
  plan = Fh1ResolvePlan();
  if (!Fh1ResolveRectangle(regs, memory, plan.x0, plan.y0, plan.x1, plan.y1)) {
    plan.empty = true;
    return false;
  }
  // As the render target cache does, so 16_16[_16_16] keep the guest's range.
  if (!draw_util::GetResolveInfo(regs, memory, 1, 1, flags.fixed16_truncated,
                                 flags.fixed16_truncated, plan.info)) {
    plan.skip = "resolve_info";
    return false;
  }
  const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  plan.msaa = uint32_t(surface_info.msaa_samples);
  plan.pitch_tiles = Fh1PitchTiles(surface_info.surface_pitch, plan.msaa);
  plan.copying_depth = plan.info.IsCopyingDepth();
  plan.color_info =
      plan.copying_depth
          ? reg::RB_COLOR_INFO{}
          : regs.Get<reg::RB_COLOR_INFO>(
                reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
  plan.depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (!plan.info.copy_dest_extent_length) return true;

  const auto dest_info = plan.info.copy_dest_info;
  plan.resolve_key =
      plan.copying_depth
          ? Fh1SurfaceKey::Depth(plan.depth_info.depth_base, plan.pitch_tiles, plan.msaa,
                                 plan.depth_info.depth_format)
          : Fh1SurfaceKey::Color(plan.color_info.color_base, plan.pitch_tiles, plan.msaa,
                                 plan.color_info.color_format, flags.gamma_as_unorm16);
  if (!plan.copying_depth) {
    // Decode in the guest format the resolve names, not the storage key.
    plan.resolve_key.format = uint32_t(plan.color_info.color_format);
  }
  uint32_t pack = UINT32_MAX, bpb_log2 = 2;
  if (plan.copying_depth) {
    pack = 4;
    plan.kind = "depth" + std::to_string(uint32_t(plan.depth_info.depth_format));
  } else {
    switch (xenos::TextureFormat(dest_info.copy_dest_format)) {
      case xenos::TextureFormat::k_8_8_8_8:
        pack = 0;
        break;
      case xenos::TextureFormat::k_2_10_10_10:
        pack = 1;
        break;
      case xenos::TextureFormat::k_32_FLOAT:
        pack = 2;
        break;
      case xenos::TextureFormat::k_16_16_16_16_FLOAT:
        pack = 3;
        bpb_log2 = 3;
        break;
      default:
        break;
    }
    plan.kind = "c" + std::to_string(uint32_t(plan.color_info.color_format)) + "->t" +
                std::to_string(uint32_t(dest_info.copy_dest_format));
  }
  plan.kind += "/" + std::to_string(1u << plan.msaa) + "x/s" +
               std::to_string(uint32_t(plan.info.copy_dest_coordinate_info.copy_sample_select));
  if (pack == UINT32_MAX) {
    plan.skip = "resolve_dest_format";
  } else if (dest_info.copy_dest_array) {
    plan.skip = "resolve_dest_array";
  } else if (uint32_t(dest_info.copy_dest_endian) > 3) {
    plan.skip = "resolve_dest_endian";
  } else if (!plan.copying_depth &&
             !Fh1IsResolveColorFormatSupported(plan.color_info.color_format)) {
    plan.skip = "resolve_source_format";
  }
  if (plan.skip) return true;
  const int32_t exp_bias = plan.copying_depth ? 0 : int32_t(dest_info.copy_dest_exp_bias);
  plan.dest_info = pack | (uint32_t(dest_info.copy_dest_endian) << 3) |
                   (uint32_t(!plan.copying_depth && dest_info.copy_dest_swap) << 6) |
                   (uint32_t(flags.depth_float24_round) << 7) |
                   ((uint32_t(exp_bias) & 0xFF) << 8) | (bpb_log2 << 16) |
                   (uint32_t(flags.gamma_as_unorm16) << 18) |
                   (uint32_t(!flags.fixed16_truncated) << 19);
  plan.dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  plan.dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch;
  plan.sample_select = uint32_t(plan.info.copy_dest_coordinate_info.copy_sample_select);
  plan.copy = true;
  return true;
}

}  // namespace rex::graphics
