/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace rex::ui {

// The build line the F3 overlay's watermark and the window title show. A host
// sets it before setup (for example in OnPreSetup) to name its own release
// and commits; empty restores the SDK's stamp.
void SetBuildLabel(std::string label);
std::string GetBuildLabel();

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~DebugOverlayDialog();

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameStatsProvider stats_provider_;
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
  // The presenter's totals at the last rate sample, and the rates.
  uint64_t rate_tick_ = 0;
  uint64_t rate_real_ = 0;
  uint64_t rate_generated_ = 0;
  uint64_t rate_failed_ = 0;
  double real_fps_ = 0.0;
  double generated_fps_ = 0.0;
  double failed_per_second_ = 0.0;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  uint64_t rate_sample_tick_ = 0;
  int64_t rate_source_frames_ = 0;
  int64_t rate_presents_ = 0;
  int64_t rate_simulation_time_ns_ = 0;
  double source_fps_ = 0.0;
  double present_fps_ = 0.0;
  double title_speed_ = 0.0;
#endif
};

}  // namespace rex::ui
