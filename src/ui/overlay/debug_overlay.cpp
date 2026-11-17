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
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/version.h>
#include <imgui.h>
#include <algorithm>
#include <mutex>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

REXCVAR_DEFINE_DOUBLE(debug_overlay_scale, 0.0, "UI",
                      "Size of the F3 overlay's text: 0 follows the window's height (1 at "
                      "720 lines, 3 at 2160), or a factor from 0.5 to 4")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {
std::mutex build_label_mutex;
std::string build_label;
}  // namespace

void SetBuildLabel(std::string label) {
  std::lock_guard lock(build_label_mutex);
  build_label = std::move(label);
}

std::string GetBuildLabel() {
  std::lock_guard lock(build_label_mutex);
  return build_label.empty() ? std::string(REXGLUE_BUILD_STAMP) : build_label;
}

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  // Readable on a television across the room as on a laptop: the text grows
  // with the window's height unless the player sets a size.
  const double scale_setting = REXCVAR_GET(debug_overlay_scale);
  const float scale = scale_setting > 0.0
                          ? std::clamp(float(scale_setting), 0.5f, 4.0f)
                          : std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 4.0f);
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.6f);
  if (ImGui::Begin("Debug##overlay", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoTitleBar)) {
    ImGui::SetWindowFontScale(scale);

    // Rates over the last half second, from running totals.
    const uint64_t now_tick = rex::chrono::Clock::QueryHostTickCount();
    const uint64_t tick_frequency = rex::chrono::Clock::QueryHostTickFrequency();
    Presenter* presenter = imgui_drawer() ? imgui_drawer()->presenter() : nullptr;
    const Presenter::PresentStats present =
        presenter ? presenter->GetPresentStatsFromUIThread() : Presenter::PresentStats();
    FrameStats guest;
    if (stats_provider_) {
      guest = stats_provider_();
    }
    if (!rate_tick_) {
      rate_tick_ = now_tick;
      rate_real_ = present.real_presents;
      rate_generated_ = present.generated_presents;
      rate_failed_ = present.generation_failures;
    } else if (now_tick > rate_tick_ && now_tick - rate_tick_ >= tick_frequency / 2) {
      const double seconds = double(now_tick - rate_tick_) / double(tick_frequency);
      real_fps_ = double(present.real_presents - rate_real_) / seconds;
      generated_fps_ = double(present.generated_presents - rate_generated_) / seconds;
      failed_per_second_ = double(present.generation_failures - rate_failed_) / seconds;
      rate_tick_ = now_tick;
      rate_real_ = present.real_presents;
      rate_generated_ = present.generated_presents;
      rate_failed_ = present.generation_failures;
    }
    const double shown_fps = real_fps_ + generated_fps_;

    if (guest.frame_count > 0) {
      ImGui::Text("Game      %6.1f fps  %6.2f ms", guest.fps, guest.frame_time_ms);
    }
    if (presenter) {
      ImGui::Text("Shown     %6.1f fps  %6.2f ms", shown_fps,
                  shown_fps > 0.0 ? 1000.0 / shown_fps : 0.0);
      if (!present.frame_generation.empty()) {
        ImGui::Text("  real    %6.1f fps", real_fps_);
        ImGui::Text("  made    %6.1f fps", generated_fps_);
        const std::string& name = present.frame_generation_name.empty()
                                      ? present.frame_generation
                                      : present.frame_generation_name;
        if (present.frame_generation_waiting.empty() && generated_fps_ > 0.0) {
          ImGui::Text("Frame generation: %s", name.c_str());
        } else {
          ImGui::Text("Frame generation: %s (%s)", name.c_str(),
                      present.frame_generation_waiting.empty()
                          ? "waiting"
                          : present.frame_generation_waiting.c_str());
        }
        if (failed_per_second_ > 0.0) {
          ImGui::Text("  not made %5.1f a second", failed_per_second_);
        }
      } else {
        ImGui::TextUnformatted("Frame generation: off");
      }
      if (present.render_width && present.output_width) {
        ImGui::Text("Render %ux%u to %ux%u%s%s", present.render_width, present.render_height,
                    present.output_width, present.output_height,
                    present.upscaler.empty() ? "" : ", ", present.upscaler.c_str());
      } else if (present.output_width) {
        ImGui::Text("Output %ux%u", present.output_width, present.output_height);
      }
      ImGui::Text("%s, V-sync %s", present.hdr ? "HDR" : "SDR", present.vsync ? "on" : "off");
    }

    // Shown frame time, real frames and generated ones together.
    frame_time_history_[frame_history_idx_] =
        shown_fps > 0.0 ? float(1000.0 / shown_fps) : float(guest.frame_time_ms);
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(260.0f * scale, 50.0f * scale));
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    const int64_t source_frames =
        rex::perf::GetTotalCounter(rex::perf::CounterId::kSourceFrameCount);
    // The presenter's counter holds real frames only: add the generated
    // presents so this line shows what reaches the display, as Shown does.
    const int64_t presents =
        rex::perf::GetTotalCounter(rex::perf::CounterId::kPresentCount) +
        int64_t(present.generated_presents);
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
    if (present.frame_generation.empty()) {
      ImGui::Text("Present: %6.1f FPS  %6.2f ms", present_fps_, present_frame_ms);
    } else {
      ImGui::Text("Present: %6.1f FPS  %6.2f ms (generated included)", present_fps_,
                  present_frame_ms);
    }
    ImGui::Text("Title speed: %5.1f%%", title_speed_ * 100.0);
    ImGui::Text("Dropped: %" PRId64 "  Duplicates: %" PRId64,
                rex::perf::GetTotalCounter(rex::perf::CounterId::kDroppedPresentCount),
                rex::perf::GetTotalCounter(rex::perf::CounterId::kDuplicatePresentCount));
    ImGui::Separator();

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
  const std::string label = GetBuildLabel();
  auto text_size = ImGui::CalcTextSize(label.c_str());
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
    ImGui::TextUnformatted(label.c_str());
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
