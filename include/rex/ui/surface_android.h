#pragma once
/**
 * @file        ui/surface_android.h
 * @brief       Android ANativeWindow surface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/surface.h>

struct ANativeWindow;

namespace rex {
namespace ui {

// The activity's window, from SDL. SDL owns it: the window is destroyed when
// the activity goes to the background and a new one arrives on its return,
// so a surface lives no longer than the SDL window that handed it out.
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}

  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex