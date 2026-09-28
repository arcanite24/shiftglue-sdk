/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {
class D3D12Provider;
}  // namespace rex::ui::d3d12

namespace rex::graphics::d3d12 {

// Host render target configuration the D3D12 pipelines, textures and the FH1
// native executor are built against. The FH1 native executor owns the render
// targets themselves; conventional host render targets (RTV / DSV) are the
// only output-merger path, there is no EDRAM buffer and no ROV path.
class D3D12HostRenderConfig {
 public:
  // The draw resolution scale must already be clamped and validated by the
  // caller (the command processor).
  D3D12HostRenderConfig(uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y);

  // Reads the host configuration cvars and queries the device capabilities.
  void Initialize(const ui::d3d12::D3D12Provider& provider);

  uint32_t draw_resolution_scale_x() const { return draw_resolution_scale_x_; }
  uint32_t draw_resolution_scale_y() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1;
  }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixed16TruncatedToMinus1To1() const {
    return !REXCVAR_GET(snorm16_render_target_full_range);
  }

  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  DXGI_FORMAT GetColorResourceDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  static DXGI_FORMAT GetDepthResourceDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVDepthDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVStencilDXGIFormat(xenos::DepthRenderTargetFormat format);

 private:
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_supported_ = false;
};

}  // namespace rex::graphics::d3d12
