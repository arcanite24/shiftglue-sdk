/**
 * @file        ui/xr.cpp
 * @brief       What the VR output shares with the rest of the runtime.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/xr.h>

#include <atomic>
#include <cmath>

namespace rex {
namespace ui {
namespace xr {

namespace {
constexpr double kFovStep = 0.002;
constexpr double kTagStep = kFovStep / 32.0;
std::atomic<uint32_t> g_refresh_hz{0};
}  // namespace

uint32_t HeadsetRefreshHz() { return g_refresh_hz.load(std::memory_order_relaxed); }
void SetHeadsetRefreshHz(uint32_t hz) { g_refresh_hz.store(hz, std::memory_order_relaxed); }

double TagFov(double fov_y, uint32_t tag) {
  if (!(fov_y > 0.0)) return fov_y;
  return std::floor(fov_y / kFovStep) * kFovStep + (double(tag % kTagCount) + 0.5) * kTagStep;
}

namespace {
std::atomic<void (*)()> g_shutdown{nullptr};
}  // namespace

void SetShutdown(void (*shutdown)()) { g_shutdown.store(shutdown); }

void Shutdown() {
  if (auto shutdown = g_shutdown.exchange(nullptr)) shutdown();
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
