#pragma once
/**
 * @file        rex/ui/xr.h
 * @brief       What the VR output shares with the rest of the runtime.
 *
 * The OpenXR output's alternate-eye stereo (rex/ui/vulkan/openxr.h) needs to
 * know which request a finished frame was rendered for. The host puts the
 * request's tag in the frame's field of view; the frame's world-view-
 * projection gives the field of view back. And the headset's refresh rate,
 * which the guest's frame pacing follows in VR. Free of any graphics API.
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

// The headset's refresh rate while a VR session shows frames and
// xr_match_refresh is on, else 0: the guest's vblank pacing then renders at
// it instead of pinyon_shift_fh1_render_fps_limit (alternate-eye stereo
// gives each eye half of it).
uint32_t HeadsetRefreshHz();
void SetHeadsetRefreshHz(uint32_t hz);

}  // namespace xr
}  // namespace ui
}  // namespace rex
