/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/version.h>
#include <imgui.h>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/chrono/clock.h>
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(310, 330), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(0.5f);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    if (stats_provider_) {
      auto stats = stats_provider_();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    const uint64_t now_tick = rex::chrono::Clock::QueryHostTickCount();
    const uint64_t tick_frequency = rex::chrono::Clock::QueryHostTickFrequency();
    const int64_t source_frames =
        rex::perf::GetTotalCounter(rex::perf::CounterId::kSourceFrameCount);
    const int64_t presents =
        rex::perf::GetTotalCounter(rex::perf::CounterId::kPresentCount);
    const int64_t simulation_time_ns =
        rex::perf::GetTotalCounter(rex::perf::CounterId::kSimulationTimeNs);
    if (!rate_sample_tick_) {
      rate_sample_tick_ = now_tick;
      rate_source_frames_ = source_frames;
      rate_presents_ = presents;
      rate_simulation_time_ns_ = simulation_time_ns;
    } else if (now_tick > rate_sample_tick_ &&
               now_tick - rate_sample_tick_ >= tick_frequency / 2) {
      const double elapsed_seconds =
          double(now_tick - rate_sample_tick_) / double(tick_frequency);
      source_fps_ = double(source_frames - rate_source_frames_) / elapsed_seconds;
      present_fps_ = double(presents - rate_presents_) / elapsed_seconds;
      title_speed_ =
          double(simulation_time_ns - rate_simulation_time_ns_) /
          (elapsed_seconds * 1000000000.0);
      rate_sample_tick_ = now_tick;
      rate_source_frames_ = source_frames;
      rate_presents_ = presents;
      rate_simulation_time_ns_ = simulation_time_ns;
    }

    const double source_frame_ms = source_fps_ > 0.0 ? 1000.0 / source_fps_ : 0.0;
    const double present_frame_ms = present_fps_ > 0.0 ? 1000.0 / present_fps_ : 0.0;
    ImGui::Text("Render:  %6.1f FPS  %6.2f ms", source_fps_, source_frame_ms);
    ImGui::Text("Present: %6.1f FPS  %6.2f ms", present_fps_, present_frame_ms);
    ImGui::Text("Title speed: %5.1f%%", title_speed_ * 100.0);
    ImGui::Text("Dropped: %" PRId64 "  Duplicates: %" PRId64,
                rex::perf::GetTotalCounter(rex::perf::CounterId::kDroppedPresentCount),
                rex::perf::GetTotalCounter(rex::perf::CounterId::kDuplicatePresentCount));
    ImGui::Separator();

    // Frame time graph
    auto ft_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    // GPU
    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCommandBufferStalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kVerticesProcessed));

    // Audio
    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kXmaFramesDecoded),
                static_cast<float>(
                    rex::perf::GetSnapshotCounter(rex::perf::CounterId::kAudioFrameLatencyUs)) /
                    1000.0f,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kBufferQueueDepth));

    // Dispatch
    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFunctionsDispatched),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kInterruptDispatches));

    // Threading
    ImGui::Text("Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kActiveThreads),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kApcQueueDepth),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCriticalRegionContentions));

    // Caches
    auto tex_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheHits);
    auto tex_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheMisses);
    auto pip_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheHits);
    auto pip_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  // Build stamp watermark -- centered near bottom of screen
  auto text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
  float padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  float bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, imgui_drawer()->style().debug.muted_text);
  if (ImGui::Begin("##watermark", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(REXGLUE_BUILD_STAMP);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
