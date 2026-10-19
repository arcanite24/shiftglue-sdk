/**
 * @file        ui/xr_tag.cpp
 * @brief       A frame's VR stereo tag in its vertical field of view.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/xr_tag.h>

#include <cmath>

namespace rex {
namespace ui {
namespace xr {

namespace {
constexpr double kFovStep = 0.002;
constexpr double kTagStep = kFovStep / 32.0;
}  // namespace

double TagFov(double fov_y, uint32_t tag) {
  if (!(fov_y > 0.0)) return fov_y;
  return std::floor(fov_y / kFovStep) * kFovStep + (double(tag % kTagCount) + 0.5) * kTagStep;
}

uint32_t FovTag(double fov_y) {
  if (!(fov_y > 0.0)) return UINT32_MAX;
  const double residual = fov_y - std::floor(fov_y / kFovStep) * kFovStep;
  const double slot = residual / kTagStep - 0.5;
  const double nearest = std::round(slot);
  // An untagged field of view lands anywhere in the step; a tag lands within
  // a tenth of a slot of its centre, in the lower half of the step.
  if (nearest < 0.0 || nearest >= double(kTagCount) || std::abs(slot - nearest) > 0.1) {
    return UINT32_MAX;
  }
  return uint32_t(nearest);
}

}  // namespace xr
}  // namespace ui
}  // namespace rex
