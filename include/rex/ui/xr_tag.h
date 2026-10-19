#pragma once
/**
 * @file        rex/ui/xr_tag.h
 * @brief       A frame's VR stereo tag in its vertical field of view.
 *
 * The OpenXR output's alternate-eye stereo (rex/ui/vulkan/openxr.h) needs to
 * know which request a finished frame was rendered for. The host puts the
 * request's tag in the frame's field of view; the frame's world-view-
 * projection gives the field of view back. Free of any graphics API.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdint>

namespace rex {
namespace ui {
namespace xr {

// The field of view is rounded down to a 0.002 rad step (0.11 degrees) and
// the tag sits in the fraction, at most 0.001 rad, which nobody sees.
// FovTag gives it back, or UINT32_MAX for an untagged field of view.
constexpr uint32_t kTagCount = 16;
double TagFov(double fov_y, uint32_t tag);
uint32_t FovTag(double fov_y);

}  // namespace xr
}  // namespace ui
}  // namespace rex
