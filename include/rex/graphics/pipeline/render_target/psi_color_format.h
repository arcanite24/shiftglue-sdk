#pragma once
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

#include <cstdint>

#include <rex/graphics/xenos.h>

// Pixel shader interlock color format flags, shared by the shader translators
// and the EDRAM render target cache, so the translators don't depend on the
// render target cache.
namespace rex::graphics::psi_color_format {

// Appended to the format in the format constant via bitwise OR.
enum : uint32_t {
  kFlag_64bpp_Shift = xenos::kColorRenderTargetFormatBits,
  // Requires clamping of blending sources and factors.
  kFlag_FixedPointColor_Shift,
  kFlag_FixedPointAlpha_Shift,

  kFlag_64bpp = uint32_t(1) << kFlag_64bpp_Shift,
  kFlag_FixedPointColor = uint32_t(1) << kFlag_FixedPointColor_Shift,
  kFlag_FixedPointAlpha = uint32_t(1) << kFlag_FixedPointAlpha_Shift,
};

constexpr uint32_t AddFlags(xenos::ColorRenderTargetFormat format) {
  uint32_t format_flags = uint32_t(format);
  if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
      format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
      format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
    format_flags |= kFlag_64bpp;
  }
  if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
      format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
      format == xenos::ColorRenderTargetFormat::k_16_16 ||
      format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
      format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
    format_flags |= kFlag_FixedPointColor | kFlag_FixedPointAlpha;
  } else if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
             format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) {
    format_flags |= kFlag_FixedPointAlpha;
  }
  return format_flags;
}

}  // namespace rex::graphics::psi_color_format
