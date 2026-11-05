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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>
#include <rex/system/gpu_plugin.h>
#include <rex/thread.h>
#include <rex/ui/frame_generation_pacer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/upscaler.h>
#include <rex/ui/window.h>


REXCVAR_DEFINE_BOOL(host_present_from_non_ui_thread, true, "UI/Presenter",
                    "Allow presentation from non-UI thread");

REXCVAR_DEFINE_INT32(host_present_fps_limit, 60, "UI/Presenter",
                     "Host presentation FPS limit (0 disables pacing)")
    .range(0, 240);

REXCVAR_DEFINE_BOOL(host_present_sleep_spin, true, "UI/Presenter",
                    "Use a sleep/yield hybrid for host presentation pacing");

REXCVAR_DEFINE_BOOL(pinyon_shift_fh1_source_presentation, false, "Pinyon Shift",
                    "Present each real FH1 source frame exactly once");

REXCVAR_DEFINE_BOOL(present_letterbox, true, "UI/Presenter",
                    "Enable letterboxing for non-native aspect ratios");

REXCVAR_DEFINE_INT32(present_safe_area_x, 90, "UI/Presenter",
                     "Horizontal safe area percentage (0-100)")
    .range(0, 100);

REXCVAR_DEFINE_INT32(present_safe_area_y, 90, "UI/Presenter",
                     "Vertical safe area percentage (0-100)")
    .range(0, 100);

// The guest output paint settings apply live: GraphicsSystem passes each
// change to Presenter::RefreshGuestOutputPaintConfigFromUIThread.
#if defined(REX_HAS_FIDELITYFX_SDK)
REXCVAR_DEFINE_STRING(present_effect, "bilinear", "UI/Presenter",
                      "Guest output effect: bilinear, cas (sharpen, and resample when "
                      "downscaling), fsr (FSR 1 upscale with RCAS sharpening), fsr2 and fsr3 "
                      "(temporal FSR through the FidelityFX runtime), dlss (DLSS through NGX); "
                      "FSR 1 when the temporal upscaler is unavailable")
    .allowed({"bilinear", "cas", "fsr", "fsr2", "fsr3", "dlss"});

REXCVAR_DEFINE_DOUBLE(present_cas_additional_sharpness,
                      rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
                      "UI/Presenter", "Additional CAS sharpness in [0, 1]")
    .range(rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
           rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax);

REXCVAR_DEFINE_INT32(present_fsr_max_upsampling_passes,
                     rex::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
                     "UI/Presenter", "Maximum chained FSR EASU passes")
    .range(1, int32_t(rex::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax));

REXCVAR_DEFINE_DOUBLE(present_fsr_sharpness_reduction,
                      rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
                      "UI/Presenter", "FSR RCAS sharpness reduction in stops")
    .range(rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
           rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax);

REXCVAR_DEFINE_STRING(
    present_fsr_quality_mode, "auto", "UI/Presenter",
    "Temporal FSR quality mode: auto, nativeaa, quality, balanced, performance, ultra_performance")
    .allowed({"auto", "nativeaa", "quality", "balanced", "performance", "ultra_performance"});
#else
REXCVAR_DEFINE_STRING(present_effect, "bilinear", "UI/Presenter", "Guest output effect: bilinear")
    .allowed({"bilinear"});
#endif

REXCVAR_DEFINE_BOOL(present_dither, false, "UI/Presenter",
                    "Enable output dithering in the final present pass");

REXCVAR_DEFINE_BOOL(present_allow_overscan_cutoff, false, "UI/Presenter",
                    "Allow overscan cutoff based on safe area settings");

// Frame generation (opt-in, off by default): one generated frame presented
// between each pair of the title's frames. metalfx: Apple MetalFX frame
// interpolation (macOS 26, Apple silicon). fsr3: FidelityFX FSR 3.1 frame
// generation (not wired yet; reports unavailable). repeat: development only,
// re-presents the previous frame to measure the pacing and counters.
REXCVAR_DEFINE_STRING(present_frame_generation, "off", "UI/Presenter",
                      "Frame generation backend: off, metalfx, fsr3 (unavailable), repeat (debug)")
    .allowed({"off", "metalfx", "fsr3", "repeat"});

// The real frame is presented this fraction of the measured base interval
// after it was produced, the generated frame right away: 0.5 spaces the
// presents evenly, less trades evenness for latency.
REXCVAR_DEFINE_DOUBLE(present_frame_generation_delay,
                      rex::ui::Presenter::GuestOutputPaintConfig::kFrameGenerationDelayDefault,
                      "UI/Presenter",
                      "Real-frame delay with frame generation, as a fraction of the base interval")
    .range(rex::ui::Presenter::GuestOutputPaintConfig::kFrameGenerationDelayMin,
           rex::ui::Presenter::GuestOutputPaintConfig::kFrameGenerationDelayMax);


namespace {
using GuestOutputPaintConfig = rex::ui::Presenter::GuestOutputPaintConfig;

GuestOutputPaintConfig::Effect ParsePresentEffect(const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (lowered == "cas") {
    return GuestOutputPaintConfig::Effect::kCas;
  }
  if (lowered == "fsr") {
    return GuestOutputPaintConfig::Effect::kFsr;
  }
  if (lowered == "fsr2") {
    return GuestOutputPaintConfig::Effect::kFsr2;
  }
  if (lowered == "fsr3") {
    return GuestOutputPaintConfig::Effect::kFsr3;
  }
  if (lowered == "dlss") {
    return GuestOutputPaintConfig::Effect::kDlss;
  }
#endif
  return GuestOutputPaintConfig::Effect::kBilinear;
}

#if defined(REX_HAS_FIDELITYFX_SDK)
bool IsTemporalFsrCompatibilityEffect(GuestOutputPaintConfig::Effect effect) {
  return GuestOutputPaintConfig::IsTemporalEffect(effect);
}

GuestOutputPaintConfig::FsrQualityMode ParsePresentFsrQualityMode(const std::string& mode_name) {
  std::string lowered = mode_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "nativeaa" || lowered == "native_aa" || lowered == "native") {
    return GuestOutputPaintConfig::FsrQualityMode::kNativeAa;
  }
  if (lowered == "quality") {
    return GuestOutputPaintConfig::FsrQualityMode::kQuality;
  }
  if (lowered == "balanced") {
    return GuestOutputPaintConfig::FsrQualityMode::kBalanced;
  }
  if (lowered == "performance") {
    return GuestOutputPaintConfig::FsrQualityMode::kPerformance;
  }
  if (lowered == "ultra_performance" || lowered == "ultra") {
    return GuestOutputPaintConfig::FsrQualityMode::kUltraPerformance;
  }
  return GuestOutputPaintConfig::FsrQualityMode::kAuto;
}

rex::ui::UpscalerMode ToUpscalerMode(GuestOutputPaintConfig::FsrQualityMode mode) {
  switch (mode) {
    case GuestOutputPaintConfig::FsrQualityMode::kNativeAa:
      return rex::ui::UpscalerMode::kNativeAa;
    case GuestOutputPaintConfig::FsrQualityMode::kQuality:
      return rex::ui::UpscalerMode::kQuality;
    case GuestOutputPaintConfig::FsrQualityMode::kBalanced:
      return rex::ui::UpscalerMode::kBalanced;
    case GuestOutputPaintConfig::FsrQualityMode::kPerformance:
      return rex::ui::UpscalerMode::kPerformance;
    case GuestOutputPaintConfig::FsrQualityMode::kUltraPerformance:
      return rex::ui::UpscalerMode::kUltraPerformance;
    case GuestOutputPaintConfig::FsrQualityMode::kAuto:
    default:
      return rex::ui::UpscalerMode::kAuto;
  }
}

// The render size of a quality mode, by the ratios FSR, DLSS and XeSS share.
bool QueryTemporalFsrRenderResolutionFromQualityMode(uint32_t display_width,
                                                     uint32_t display_height,
                                                     GuestOutputPaintConfig::FsrQualityMode mode,
                                                     uint32_t& render_width_out,
                                                     uint32_t& render_height_out) {
  if (mode == GuestOutputPaintConfig::FsrQualityMode::kAuto) {
    return false;
  }
  float ratio = rex::ui::GetUpscalerModeRatio(ToUpscalerMode(mode));
  render_width_out = std::max(uint32_t(1), uint32_t(float(display_width) / ratio + 0.5f));
  render_height_out = std::max(uint32_t(1), uint32_t(float(display_height) / ratio + 0.5f));
  return true;
}

void LogTemporalFsrCompatibilityPathOnce() {
  static std::atomic<bool> temporal_fsr_compatibility_logged = false;
  if (!temporal_fsr_compatibility_logged.exchange(true)) {
    REXLOG_INFO(
        "present_effect=fsr2/fsr3: temporal FSR with the game's depth, camera motion "
        "and jitter where the title provides them; FSR 1 when the FidelityFX runtime "
        "is unavailable");
  }
}

void LogTemporalFsrQualityModeInputLimitOnce() {
  static std::atomic<bool> temporal_fsr_quality_mode_input_limit_logged = false;
  if (!temporal_fsr_quality_mode_input_limit_logged.exchange(true)) {
    REXLOG_WARN(
        "present_fsr_quality_mode requested a render size larger than the "
        "guest output; using guest output size");
  }
}
#endif  // defined(REX_HAS_FIDELITYFX_SDK)

GuestOutputPaintConfig BuildGuestOutputPaintConfigFromCVar() {
  GuestOutputPaintConfig config;
  GuestOutputPaintConfig::Effect parsed_effect = ParsePresentEffect(REXCVAR_GET(present_effect));
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (IsTemporalFsrCompatibilityEffect(parsed_effect)) {
    LogTemporalFsrCompatibilityPathOnce();
  }
#endif
  config.SetAllowOverscanCutoff(REXCVAR_GET(present_allow_overscan_cutoff));
  config.SetEffect(parsed_effect);
#if defined(REX_HAS_FIDELITYFX_SDK)
  config.SetCasAdditionalSharpness(float(REXCVAR_GET(present_cas_additional_sharpness)));
  config.SetFsrMaxUpsamplingPasses(
      uint32_t(std::max(int32_t(1), REXCVAR_GET(present_fsr_max_upsampling_passes))));
  config.SetFsrSharpnessReduction(float(REXCVAR_GET(present_fsr_sharpness_reduction)));
  config.SetFsrQualityMode(ParsePresentFsrQualityMode(REXCVAR_GET(present_fsr_quality_mode)));
#endif
  config.SetDither(REXCVAR_GET(present_dither));
  config.SetFrameGeneration(
      rex::ui::ParseFrameGenerationBackend(REXCVAR_GET(present_frame_generation)));
  config.SetFrameGenerationDelay(float(REXCVAR_GET(present_frame_generation_delay)));
  return config;
}

}  // namespace

namespace rex {
namespace ui {

void Presenter::FatalErrorHostGpuLossCallback([[maybe_unused]] bool is_responsible,
                                              [[maybe_unused]] bool statically_from_ui_thread) {
  rex::system::ReportHostGpuLoss();
}

Presenter::~Presenter() {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  // The pacing thread calls into this presenter; it must stop before anything
  // else is torn down (the implementation's destructor has already done so).
  ShutdownFrameGenerationPacer();

#if REX_PLATFORM_WIN32
  if (dxgi_ui_tick_thread_.joinable()) {
    {
      std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      dxgi_ui_tick_thread_shutdown_ = true;
    }
    dxgi_ui_tick_control_condition_.notify_all();
    dxgi_ui_tick_thread_.join();
  }
#endif  // XE_PLATFORM

  if (window_) {
    Window* old_window = window_;
    // Null the pointer to prevent an infinite loop between SetPresenter and
    // SetWindowSurfaceFromUIThread calling each other.
    window_ = nullptr;
    old_window->SetPresenter(nullptr);
  }
}

void Presenter::SetWindowSurfaceFromUIThread(Window* new_window, Surface* new_surface) {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  // There can't be a valid surface pointer without a window, as a surface is
  // created and owned by the window.
  assert_false(new_surface && !new_window);

  if (window_ == new_window && (!window_ || surface_ == new_surface)) {
    // Nothing has changed (or a recursive SetWindowSurfaceFromUIThread >
    // SetPresenter > SetWindowSurfaceFromUIThread call).
    return;
  }

  // Disconnect from the current surface.
  if (surface_) {
    // Take ownership of painting, and also stop accepting paint requests from
    // the guest output thread - the window (which is required for making them)
    // may be going away, and there will be a forced paint when the connection
    // becomes available.
    SetPaintModeFromUIThread(PaintMode::kNone);
    DisconnectPaintingFromSurfaceFromUIThread(
        SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    surface_ = nullptr;
    UpdateSurfaceMonitorFromUIThread(true);
  }

  if (window_ != new_window) {
    // The window pointer may be accessed by the guest output thread if painting
    // is possible (or was possible, but the paint attempt has resulted in the
    // implementation reporting that the connection has become outdated).
    // However, a painting connection is currently not established at all, so
    // it's safe to modify the window pointer here.

    // Detach from the old window if attaching to a different one or just
    // detaching. SetPresenter for the new window might have been called without
    // it having been called with nullptr for the old window.
    if (window_) {
      Window* old_window = window_;
      // Null the pointer to prevent an infinite loop between SetPresenter and
      // SetWindowSurfaceFromUIThread calling each other.
      window_ = nullptr;
      old_window->SetPresenter(nullptr);
    }

    // Attach to the new one.
    // This function is called from SetPresenter - don't need to notify the
    // window of this, as it itself has triggered this.
    window_ = new_window;
  }

  if (new_surface) {
    assert_true(paint_mode_ == PaintMode::kNone);
    surface_ = new_surface;
    UpdateSurfaceMonitorFromUIThread(true);
    assert_true(surface_paint_connection_state_ ==
                SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    bool request_repaint;
    UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);
    // Request to paint as soon as possible in the UI thread if connected
    // successfully.
    if (request_repaint) {
      RequestPaintOrConnectionRecoveryViaWindow(true);
    }
  }
}

void Presenter::OnSurfaceMonitorUpdateFromUIThread(bool old_monitor_potentially_disconnected) {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  UpdateSurfaceMonitorFromUIThread(old_monitor_potentially_disconnected);
}

void Presenter::OnSurfaceResizeFromUIThread() {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  // Let the UI thread take ownership of painting (so the connection can be
  // updated) in a smooth way - downgrade to kUIThreadOnRequest rather than
  // kNone, because a forced repaint may not be necessary if, for example, the
  // size internally turns out to be the same after the update, and in this case
  // the current image may be kept - but the new one must not be missed either
  // if it becomes available during the resize.
  if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
    SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
  }

  bool request_repaint;
  UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);

  // Request to repaint as soon as possible in the UI thread if needed.
  if (request_repaint) {
    RequestPaintOrConnectionRecoveryViaWindow(true);
  }
}

void Presenter::PaintFromUIThread(bool force_paint) {
  // If there is no surface, this will be a no-op, nothing outdated, nothing to
  // paint. However, an explicit monitor check is needed because UI framerate
  // limiting may be tied to signals from the OS for the monitor - but painting
  // may still occur, for instance, if drawing to a composition surface in the
  // OS (which will still be live even if the window goes outside any monitor).
  // But a surface check still won't cause harm, for simplicity.
  if (!InSurfaceOnMonitorFromUIThread()) {
    return;
  }

  // Defer changes to the paint mode as well as window paint requests, and do
  // them in this function so they're consistent with the assumptions made here.
  assert_false(is_in_ui_thread_paint_);
  is_in_ui_thread_paint_ = true;
  request_guest_output_paint_after_current_ui_thread_paint_ = false;
  request_ui_paint_after_current_ui_thread_paint_ = false;

  // Actualize the connection if the UI needs to be drawn if there was some
  // explicit paint request (the guest output has been refreshed, and the guest
  // output thread was asked not to present directly due as the UI needs to be
  // drawn, or some surface state change has happened so the guest output needs
  // to be displayed as soon as possible without waiting for the guest to
  // refresh it, or the guest output thread has been notified that the
  // connection has become outdated and has requested the UI thread to
  // reconnect).
  bool draw_ui = !ui_drawers_.empty();
  bool do_paint = force_paint || draw_ui;
  // Reset ui_thread_paint_requested_ unconditionally also, regardless of
  // whether the UI needs to be drawn - the flag may be set to try reconnecting,
  // for example.
  if (ui_thread_paint_requested_.exchange(false, std::memory_order_relaxed)) {
    do_paint = true;
  }
  PaintResult paint_result = PaintResult::kNotPresented;
  bool request_repaint_at_tick = false;
  bool request_repaint_immediately = false;
  if (do_paint) {
    // Take ownership of painting if it's currently owned by the guest output
    // thread (downgrade from kGuestOutputThreadImmediately to
    // kUIThreadOnRequest - not to kNone so if during this paint a new guest
    // output frame is generated, the notification will still be sent to the UI
    // thread rather than dropped, so the frame won't be skipped). This is
    // needed to be able not only to paint, but also to try to recover from an
    // outdated surface.
    if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
    }
    // Try to recover from the connection becoming outdated in the previous
    // paint.
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
      UpdateSurfacePaintConnectionFromUIThread(nullptr, false);
    }
    // If still paintable or recovered successfully, paint.
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
      // The paint mode might have been set to kNone when the connection was
      // marked as outdated last time. Or, if wasn't reconnecting, there was
      // some other incorrect situation that caused the paint mode to be set to
      // kNone for an active connection. Make sure that the current paint mode
      // is consistent with painting from the UI thread.
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);

      // Limit the frame rate of the UI, usually to the monitor refresh rate,
      // in a way so that the UI won't be stealing all the remaining GPU
      // resources if it's repainted continuously, and the window system itself
      // doesn't limit the frame rate.
      WaitForUITickFromUIThread();

      // Frame generation: a generated frame when the publish asked for one
      // and no UI is drawn over it (menus and overlays show real frames and
      // start the history over); the real frame otherwise.
      bool more_requests_pending = false;
      const FrameGenerationUIPaintRequest generation_request =
          TakeFrameGenerationUIRequest(more_requests_pending);
      if (more_requests_pending) {
        request_repaint_immediately = true;
      }
      const uint64_t newest_sequence =
          guest_output_refresh_sequence_.load(std::memory_order_acquire);
      bool painted_generated = false;
      bool skip_paint = false;
      if (generation_request.sequence != 0 && generation_request.sequence != newest_sequence) {
        // The frame this request was for has been superseded: a real frame
        // that never showed is dropped (it lives on inside the newer
        // generated frame); a generated frame for an old pair is simply
        // stale. Painting the newest frame in its place would show frames out
        // of order, so this paint shows nothing.
        if (generation_request.source == PaintSource::kGuestOutput) {
          PROFILE_DROPPED_PRESENT();
        }
        skip_paint = !draw_ui;
      } else if (generation_request.source == PaintSource::kGenerated) {
        if (draw_ui) {
          if (frame_generation_was_active_.exchange(false, std::memory_order_relaxed)) {
            StopFrameGenerationFromAnyThread();
            PublishFrameGenerationActive(false);
          }
        } else if (IsFrameGenerationReady()) {
          {
            // The real frame of this pair already showed (the pacer's request
            // was served first): the generated one would step back in time.
            std::lock_guard<std::mutex> pacing_lock(presentation_pacing_mutex_);
            skip_paint = last_presented_refresh_sequence_ != UINT64_MAX &&
                         generation_request.sequence <= last_presented_refresh_sequence_;
          }
          if (!skip_paint) {
            painted_generated = PaintGeneratedFrameAndScheduleReal(generation_request);
            if (painted_generated) {
              paint_result = PaintResult::kPresented;
            }
          }
        }
      }
      if (!painted_generated && !skip_paint) {
        paint_result = PaintAndPresent(draw_ui);
      }
      if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
        // Request another PaintFromUIThread which will try to recover from the
        // outdated connection in the next frame (not immediately, so the
        // windowing system has some time to prepare what may be required to
        // recover from it, such as to send a resize event).
        request_repaint_immediately = true;
      }
    }
    // If can't paint anymore, notify the paint mode refresh below (which is not
    // guaranteed to have access to have ownership of painting as it's taken
    // here only conditionally, thus can't know whether the connection is
    // actually in a paintable state).
    if (surface_paint_connection_state_ != SurfacePaintConnectionState::kConnectedPaintable) {
      SetPaintModeFromUIThread(PaintMode::kNone);
    }
  }

  // Transfer the ownership of painting back to the guest output thread if it
  // was taken or if needed for any reason (however, it's taken conditionally -
  // no guarantees that the actual connection state is accessible here, so only
  // checking whether the mode is not kNone currently, not the connection
  // state), and overall synchronize the state taking into account both what has
  // been done in this function and what could have been done by the UI drawer
  // callbacks.
  if (paint_mode_ != PaintMode::kNone) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
  }
  is_in_ui_thread_paint_ = false;

  // Check if the device has been lost. There's no point in requesting repaint
  // if it has happened anyway, it won't be possible to satisfy such request
  // with the current Presenter.
  if (paint_result == PaintResult::kGpuLostExternally ||
      paint_result == PaintResult::kGpuLostResponsible) {
    if (host_gpu_loss_callback_) {
      host_gpu_loss_callback_(paint_result == PaintResult::kGpuLostResponsible, true);
    }
    // The loss callback might have destroyed the presenter, must not do
    // anything with `this` anymore.
    return;
  }

  // Request refresh if needed.
  // Can't check the exact paintability as the connection state may currently
  // be owned by the guest output thread, so check conservatively via
  // paint_mode_.
  if (paint_mode_ != PaintMode::kNone) {
    // Immediately paint the guest output if requested explicitly or if the UI
    // has hidden itself.
    if (request_guest_output_paint_after_current_ui_thread_paint_ ||
        (draw_ui && ui_drawers_.empty())) {
      request_repaint_immediately = true;
    }
    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {
      request_repaint_at_tick = true;
    }
  }
  if (request_repaint_at_tick || request_repaint_immediately) {
    RequestPaintOrConnectionRecoveryViaWindow(request_repaint_immediately);
  }
}

bool Presenter::RefreshGuestOutput(
    uint32_t frontbuffer_width, uint32_t frontbuffer_height, uint32_t display_aspect_ratio_x,
    uint32_t display_aspect_ratio_y,
    std::function<bool(GuestOutputRefreshContext& context)> refresher) {
  GuestOutputProperties& writable_properties =
      guest_output_properties_[guest_output_mailbox_writable_];
  writable_properties.frontbuffer_width = frontbuffer_width;
  writable_properties.frontbuffer_height = frontbuffer_height;
  writable_properties.display_aspect_ratio_x = display_aspect_ratio_x;
  writable_properties.display_aspect_ratio_y = display_aspect_ratio_y;
  writable_properties.is_8bpc = false;
  bool is_active = writable_properties.IsActive();
  const uint32_t mailbox_index = guest_output_mailbox_writable_;
  if (is_active) {
    if (!RefreshGuestOutputImpl(mailbox_index, frontbuffer_width, frontbuffer_height, refresher,
                                writable_properties.is_8bpc)) {
      // If failed to refresh, don't send the currently writable image to the
      // mailbox as it may be in an undefined state. Don't disable the guest
      // output either though because the failure may be something transient.
      if (guest_output_publish_deferrer_) {
        guest_output_publish_deferrer_(
            [this, mailbox_index]() { GuestOutputRefreshSubmitted(mailbox_index); });
      }
      return false;
    }
    guest_output_active_last_refresh_ = true;
  } else {
    // Request presenting a blank image if there was a true image previously,
    // but not now.
    if (!guest_output_active_last_refresh_) {
      return false;
    }
    guest_output_active_last_refresh_ = false;
  }

  if (guest_output_publish_deferrer_) {
    guest_output_publish_deferrer_([this, is_active, mailbox_index]() {
      if (is_active) {
        GuestOutputRefreshSubmitted(mailbox_index);
      }
      PublishGuestOutput();
    });
  } else {
    PublishGuestOutput();
  }
  return is_active;
}

void Presenter::PublishGuestOutput() {
  // Make the new image the next to present on the host (the "ready" one),
  // replacing the one already specified as the next (dropping it instead of
  // enqueueing the new image after it) to achieve the lowest latency (also,
  // after switching from UI thread painting to doing it in the guest output
  // thread, will immediately recover to having the latest frame always sent to
  // the host present call on the CPU and all frames reaching a present call).
  uint32_t last_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  // Desired acquired = current acquired (changed only by the consumers).
  // Desired ready = current writable.
  // memory_order_acq_rel to acquire the new writable image and to release the
  // current one (to let the consumers take it, from ready to acquired).
  while (!guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
      last_acquired_and_ready,
      (last_acquired_and_ready & 3) | (guest_output_mailbox_writable_ << 2),
      std::memory_order_acq_rel, std::memory_order_relaxed)) {}
  const uint32_t published_acquired = last_acquired_and_ready & 3;
  const uint32_t replaced_ready = (last_acquired_and_ready >> 2) & 3;
  if (replaced_ready != published_acquired) {
    PROFILE_DROPPED_PRESENT();
  }
  guest_output_refresh_sequence_.fetch_add(1, std::memory_order_release);
  PROFILE_PRESENT_QUEUE_DEPTH(1);
  // Now, it's known that `ready == writable` on the host presentation side.
  // Take the next `writable` with this assumption about its current value in
  // mind.
  uint32_t last_acquired = last_acquired_and_ready & 3;
  if (last_acquired == guest_output_mailbox_writable_) {
    // The new image has already been acquired by the time the compare-exchange
    // loop has finished (acquired == ready == currently writable).
    // It's a valid situation from the ownership perspective, and the semantics
    // of the weak compare-exchange explicitly permit spurious `false` results.
    // (3 - a - b) % 3 cannot be used here, as (3 - a - a) % 3 results in `a` -
    // the same index.
    // Take any free image. Preferably using + 1, not ^ 1, so if the guest needs
    // to await any GPU work referencing the image, it will wait for the frame 3
    // frames ago, not 2, if this happens repeatedly.
    guest_output_mailbox_writable_ = (guest_output_mailbox_writable_ + 1) % 3;
  } else {
    // Take the image other than the last acquired one and the new one,
    // currently not accessible to the host presentation.
    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;
  }

  // Trigger the presentation on the host.
  PaintResult paint_result = PaintResult::kNotPresented;
  {
    std::lock_guard<std::mutex> paint_mode_mutex_lock(paint_mode_mutex_);
    switch (paint_mode_) {
      case PaintMode::kNone:
        // Neither painting nor window paint requesting is accessible.
        break;
      case PaintMode::kUIThreadOnRequest: {
        // Only window paint requesting is accessible. With frame generation
        // the UI thread paints the generated frame first (the request says
        // so) and the pacing thread asks for the real one later.
        const FrameGenerationUIPaintRequest request = PlanFrameGenerationForPublish();
        {
          std::lock_guard<std::mutex> request_lock(frame_generation_ui_request_mutex_);
          // The previous publish's generated frame the UI thread has not
          // painted yet gives way to this frame: a generated frame not shown,
          // never a title frame. A pending real frame stays and goes first.
          if (frame_generation_ui_generated_request_.sequence != 0) {
            PROFILE_FRAME_GENERATION_SKIPPED();
          }
          if (request.source == PaintSource::kGenerated &&
              request.overdue_real_sequence == UINT64_MAX) {
            frame_generation_ui_generated_request_ = request;
          } else {
            frame_generation_ui_generated_request_ = FrameGenerationUIPaintRequest();
            if (request.overdue_real_sequence != UINT64_MAX) {
              // This frame came before the previous real frame's deadline:
              // the UI thread paints this frame as the real one at once, and
              // a stale real request for the previous frame is withdrawn.
              frame_generation_ui_real_request_ = FrameGenerationUIPaintRequest();
              PROFILE_FRAME_GENERATION_SKIPPED();
            }
          }
        }
        RequestPaintOrConnectionRecoveryViaWindow(true);
      } break;
      case PaintMode::kGuestOutputThreadImmediately:
        // Both painting and window paint requesting are accessible.
        if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
          // Frame generation: present the frame made from the previous and
          // this one now, and this one a fraction of the interval later from
          // the pacing thread. The real frame is painted here when generation
          // is off, not ready, or the frames are not a pair (a gap, a reset).
          bool painted_generated = false;
          const FrameGenerationUIPaintRequest request = PlanFrameGenerationForPublish();
          if (request.source == PaintSource::kGenerated &&
              request.overdue_real_sequence == UINT64_MAX) {
            painted_generated = PaintGeneratedFrameAndScheduleReal(request);
            if (painted_generated) {
              paint_result = PaintResult::kPresented;
            }
          } else if (request.overdue_real_sequence != UINT64_MAX) {
            // This frame came before the previous real frame's deadline: the
            // mailbox now holds this frame, so the real paint below shows it
            // at once and this publish generates nothing (the previous frame
            // was seen through its generated one). Counted as a skip, not a
            // drop: no title frame is lost from the sequence shown.
            PROFILE_FRAME_GENERATION_SKIPPED();
          }
          if (!painted_generated) {
            paint_result = PaintAndPresent(false);
          }
          if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
            RequestPaintOrConnectionRecoveryViaWindow(true);
          }
        }
        break;
    }
  }
  // Handle GPU loss when not in the middle of the function anymore, and
  // lifecycle management from the GPU loss callback is fine on the UI thread.
  if (host_gpu_loss_callback_) {
    if (paint_result == PaintResult::kGpuLostResponsible) {
      host_gpu_loss_callback_(true, false);
    } else if (paint_result == PaintResult::kGpuLostExternally) {
      host_gpu_loss_callback_(false, false);
    }
  }
}

void Presenter::RefreshGuestOutputPaintConfigFromUIThread() {
  SetGuestOutputPaintConfigFromUIThread(BuildGuestOutputPaintConfigFromCVar());
}

void Presenter::SetGuestOutputPaintConfigFromUIThread(const GuestOutputPaintConfig& new_config) {
  // For simplicity, this may be called externally repeatedly.
  // Lock the mutex only when something has been modified, and also don't
  // request UI thread guest output redraws when not needed.
  bool modified = false;
  bool request_repaint = false;
  if (guest_output_paint_config_.GetEffect() != new_config.GetEffect()) {
    modified = true;
    request_repaint = true;
  }
  if (guest_output_paint_config_.GetAllowOverscanCutoff() != new_config.GetAllowOverscanCutoff()) {
    modified = true;
    request_repaint = true;
  }
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (guest_output_paint_config_.GetFsrSharpnessReduction() !=
      new_config.GetFsrSharpnessReduction()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
        GuestOutputPaintConfig::IsTemporalEffect(new_config.GetEffect())) {
      request_repaint = true;
    }
  }
  if (guest_output_paint_config_.GetFsrMaxUpsamplingPasses() !=
      new_config.GetFsrMaxUpsamplingPasses()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr) {
      request_repaint = true;
    }
  }
  if (guest_output_paint_config_.GetCasAdditionalSharpness() !=
      new_config.GetCasAdditionalSharpness()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kCas ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
        GuestOutputPaintConfig::IsTemporalEffect(new_config.GetEffect())) {
      request_repaint = true;
    }
  }
#endif
  if (guest_output_paint_config_.GetDither() != new_config.GetDither()) {
    modified = true;
    request_repaint = true;
  }
  if (guest_output_paint_config_.GetFrameGeneration() != new_config.GetFrameGeneration() ||
      guest_output_paint_config_.GetFrameGenerationDelay() !=
          new_config.GetFrameGenerationDelay()) {
    modified = true;
  }
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (guest_output_paint_config_.GetFsrQualityMode() != new_config.GetFsrQualityMode()) {
    modified = true;
    if (GuestOutputPaintConfig::IsTemporalEffect(new_config.GetEffect())) {
      request_repaint = true;
    }
  }
#endif
  if (modified) {
    {
      std::unique_lock<std::mutex> config_lock(guest_output_paint_config_mutex_);
      guest_output_paint_config_ = new_config;
    }
    PublishFrameGenerationStateFromUIThread(new_config.GetFrameGeneration());
    if (frame_generation_was_active_.exchange(false, std::memory_order_relaxed)) {
      StopFrameGenerationFromAnyThread();
      PublishFrameGenerationActive(false);
    }
    // Coarsely check the availability of painting and of the window (for
    // calling RequestPaint) via paint_mode_ because the actual painting
    // connection state may currently be owned not by the UI thread.
    if (request_repaint && paint_mode_ != PaintMode::kNone) {
      if (is_in_ui_thread_paint_) {
        // Defer until the end of the current paint if called from, for
        // instance, a UI drawer.
        request_guest_output_paint_after_current_ui_thread_paint_ = true;
      } else {
        RequestPaintOrConnectionRecoveryViaWindow(true);
      }
    }
  }
}

FrameGenerationAvailability Presenter::ProbeFrameGenerationBackend(
    FrameGenerationBackend backend) {
  return ProbeFrameGenerationBackendGeneric(backend);
}

bool Presenter::IsFrameGenerationOffered() const { return false; }

void Presenter::PublishFrameGenerationStateFromUIThread(FrameGenerationBackend requested) {
  FrameGenerationState state;
  state.offered = IsFrameGenerationOffered();
  state.requested = requested;
  if (requested != FrameGenerationBackend::kOff) {
    const FrameGenerationAvailability availability = ProbeFrameGenerationBackend(requested);
    state.available = availability.available;
    state.reason = availability.reason;
    if (!availability.available &&
        frame_generation_unavailable_logged_ != requested) {
      frame_generation_unavailable_logged_ = requested;
      REXLOG_WARN("Presenter: frame generation '{}' is unavailable: {}",
                  FrameGenerationBackendName(requested), availability.reason);
    }
  }
  // Generated frames are presented only once a generator runs (the Vulkan
  // presenter sets `active` from its paint path); off or unavailable means
  // every present is a real frame.
  state.active = false;
  PublishFrameGenerationState(state);
}

void Presenter::AddUIDrawerFromUIThread(UIDrawer* drawer, size_t z_order) {
  assert_not_null(drawer);
  // Obtain whether the iterator list was empty before erasing in case of
  // replacing with a new entry with a different Z order happens.
  bool drawers_were_empty = ui_drawers_.empty();
  uint64_t drawer_last_draw = UINT64_MAX;
  // Check if already added.
  for (auto it_existing = ui_drawers_.begin(); it_existing != ui_drawers_.end(); ++it_existing) {
    if (it_existing->second.drawer != drawer) {
      continue;
    }
    if (it_existing->first == z_order) {
      return;
    }
    // Keep the same last draw index to prevent the drawer from being executed
    // twice if increasing its Z order during the drawer loop.
    drawer_last_draw = it_existing->second.last_draw;
    // If removing the drawer that is the next in the current drawer loop, skip
    // it (in a multimap, only one element iterator is invalidated).
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    ui_drawers_.erase(it_existing);
    break;
  }
  auto it_new = ui_drawers_.emplace(z_order, UIDrawerReference(drawer, drawer_last_draw));
  // If adding to the Z layer currently being processed (for drawing, from the
  // lowest to the highest), or to layers in between the current and the
  // previously next, make sure the new drawer is executed too.
  if (is_executing_ui_drawers_ && z_order >= ui_draw_current_z_order_ &&
      (ui_draw_next_iterator_ == ui_drawers_.end() || z_order < ui_draw_next_iterator_->first)) {
    ui_draw_next_iterator_ = it_new;
  }
  HandleUIDrawersChangeFromUIThread(drawers_were_empty);
}

void Presenter::RemoveUIDrawerFromUIThread(UIDrawer* drawer) {
  assert_not_null(drawer);
  for (auto it_existing = ui_drawers_.begin(); it_existing != ui_drawers_.end(); ++it_existing) {
    if (it_existing->second.drawer != drawer) {
      continue;
    }
    // If removing the drawer that is the next in the current drawer loop, skip
    // it (in a multimap, only one element iterator is invalidated).
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    ui_drawers_.erase(it_existing);
    HandleUIDrawersChangeFromUIThread(false);
    return;
  }
}

void Presenter::RequestUIPaintFromUIThread() {
  if (is_in_ui_thread_paint_) {
    // The paint request will be done once in the end of PaintFromUIThread
    // according to the actual state at the moment that happens. It's common for
    // drawers to call this (even every frame), and no need to do too many OS
    // paint request calls.
    request_ui_paint_after_current_ui_thread_paint_ = true;
    return;
  }
  // The connection state may be owned by the guest output thread now rather
  // than the UI thread, check whether it's not pointless to make the request
  // coarsely via paint_mode_.
  if (!ui_drawers_.empty() && paint_mode_ != PaintMode::kNone) {
    // The window must be present, otherwise the conditions wouldn't have been
    // met.
    window_->RequestPaint();
  }
}

bool Presenter::InitializeCommonSurfaceIndependent() {
  {
    std::lock_guard<std::mutex> config_lock(guest_output_paint_config_mutex_);
    guest_output_paint_config_ = BuildGuestOutputPaintConfigFromCVar();
  }
  PublishFrameGenerationStateFromUIThread(guest_output_paint_config_.GetFrameGeneration());

  // Initialize UI frame rate limiting.
#if REX_PLATFORM_WIN32
  dxgi_ui_tick_thread_ = std::thread(&Presenter::DXGIUITickThread, this);
#endif  // XE_PLATFORM

  return true;
}

std::unique_lock<std::mutex> Presenter::ConsumeGuestOutput(
    uint32_t& mailbox_index_or_max_if_inactive_out, GuestOutputProperties* properties_out,
    GuestOutputPaintConfig* paint_config_out) {
  if (paint_config_out) {
    // Get the up-to-date guest output paint configuration settings set by the
    // UI thread.
    std::unique_lock<std::mutex> config_lock(guest_output_paint_config_mutex_);
    *paint_config_out = guest_output_paint_config_;
  }

  // Lock the mutex to make sure the image that will be acquired now is owned
  // exclusively by the calling thread for the time while this mutex is still
  // locked (it needs to be held by the consumer while working with anything
  // that depends on the image now being acquired or its index in the mailbox).
  std::unique_lock<std::mutex> consumer_lock(guest_output_mailbox_consumer_mutex_);
  // Acquire the up-to-date ready guest image (may be new, in this case the last
  // acquired one will be released, or still the same or no refresh has happened
  // since the last consumption).
  // memory_order_relaxed here because the ready index from this load will be
  // used directly only if it's the same as during the last consumption - thus
  // the image has already been acquired previously, no need for
  // memory_order_acquire (if the image has been acquired by a different
  // consumer though, the consumer mutex performs memory access ordering).
  uint32_t old_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  // Desired acquired = current ready.
  // Desired ready = current ready (changed only by the producer).
  uint32_t desired_acquired_and_ready =
      (old_acquired_and_ready & ~uint32_t(3)) | (old_acquired_and_ready >> 2);
  // Either the same image as during the last consumption, or a new one, is
  // satisfying. However, if it's new, using memory_order_acq_rel to acquire the
  // new ready image (to make it acquired) and to release the old acquired image
  // (to let the producer take it as writable).
  while (old_acquired_and_ready != desired_acquired_and_ready &&
         !guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
             old_acquired_and_ready, desired_acquired_and_ready, std::memory_order_acq_rel,
             std::memory_order_relaxed)) {
    desired_acquired_and_ready =
        (old_acquired_and_ready & ~uint32_t(3)) | (old_acquired_and_ready >> 2);
  }
  PROFILE_PRESENT_QUEUE_DEPTH(0);
  uint32_t mailbox_index = desired_acquired_and_ready & 3;
  // Give the current acquired image to the caller, or UINT32_MAX if it's
  // inactive.
  const GuestOutputProperties& properties = guest_output_properties_[mailbox_index];
  mailbox_index_or_max_if_inactive_out = properties.IsActive() ? mailbox_index : UINT32_MAX;
  if (properties_out) {
    *properties_out = properties;
  }
  return std::move(consumer_lock);
}

Presenter::GuestOutputPaintFlow Presenter::GetGuestOutputPaintFlow(
    const GuestOutputProperties& properties, uint32_t host_rt_width, uint32_t host_rt_height,
    uint32_t max_rt_width, uint32_t max_rt_height, const GuestOutputPaintConfig& config) const {
  GuestOutputPaintFlow flow = {};

  // FIXME(Triang3l): Configuration variables racing with per-game config
  // loading.

  assert_not_zero(max_rt_width);
  assert_not_zero(max_rt_height);

  // Initialize one clear rectangle for the case of drawing no guest output, for
  // consistency with fewer state dependencies.
  flow.letterbox_clear_rectangle_count = 1;
  flow.letterbox_clear_rectangles[0].width = host_rt_width;
  flow.letterbox_clear_rectangles[0].height = host_rt_height;

  // For safety such as division by zero prevention.
  if (!properties.IsActive() || !host_rt_width || !host_rt_height ||
      !surface_width_in_paint_connection_ || !surface_height_in_paint_connection_) {
    return flow;
  }

  flow.properties = properties;

  // Multiplication-division rounding to the nearest.
  auto rescale_unsigned = [](uint32_t value, uint32_t new_scale, uint32_t old_scale) -> uint32_t {
    return uint32_t((uint64_t(value) * new_scale + (old_scale >> 1)) / old_scale);
  };
  auto rescale_signed = [](int32_t value, uint32_t new_scale, uint32_t old_scale) -> int32_t {
    // Plus old_scale / 2 for positive values, minus old_scale / 2 for
    // negative values for consistent rounding for both positive and
    // negative values (as the `/` operator rounds towards zero).
    // Example:
    // (-3 - 1) / 3 == -1
    // (-2 - 1) / 3 == -1
    // (-1 - 1) / 3 == 0
    // ---
    // (0 + 1) / 3 == 0
    // (1 + 1) / 3 == 0
    // (2 + 1) / 3 == 1
    return int32_t((int64_t(value) * new_scale + int32_t(old_scale >> 1) * (value < 0 ? -1 : 1)) /
                   old_scale);
  };

  // Final output location and dimensions.
  // All host location calculations are DPI-independent, conceptually depending
  // only on the aspect ratios, not the absolute values.
  uint32_t output_width, output_height;
  if (uint64_t(surface_width_in_paint_connection_) * properties.display_aspect_ratio_y >
      uint64_t(surface_height_in_paint_connection_) * properties.display_aspect_ratio_x) {
    // The window is wider that the source - crop along Y to preserve the aspect
    // ratio while stretching throughout the entire surface's width, then limit
    // the Y cropping via letterboxing or stretching along X.
    uint32_t present_safe_area;
    if (config.GetAllowOverscanCutoff() && REXCVAR_GET(present_safe_area_y) > 0 &&
        REXCVAR_GET(present_safe_area_y) < 100) {
      present_safe_area = uint32_t(REXCVAR_GET(present_safe_area_y));
    } else {
      present_safe_area = 100;
    }
    // Scale the desired width by the H:W aspect ratio (inverse of W:H) to get
    // the height.
    output_height =
        rescale_unsigned(surface_width_in_paint_connection_, properties.display_aspect_ratio_y,
                         properties.display_aspect_ratio_x);
    bool letterbox = false;
    if (output_height * present_safe_area > surface_height_in_paint_connection_ * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      output_height = rescale_unsigned(surface_height_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && REXCVAR_GET(present_letterbox)) {
      output_width = rescale_unsigned(surface_height_in_paint_connection_ * 100,
                                      properties.display_aspect_ratio_x,
                                      properties.display_aspect_ratio_y * present_safe_area);
      // output_width might have been rounded up already by rescale_unsigned, so
      // rounding down in this division.
      flow.output_x = (int32_t(surface_width_in_paint_connection_) - int32_t(output_width)) / 2;
    } else {
      output_width = surface_width_in_paint_connection_;
      flow.output_x = 0;
    }
    // output_height might have been rounded up already by rescale_unsigned, so
    // rounding down in this division.
    flow.output_y = (int32_t(surface_height_in_paint_connection_) - int32_t(output_height)) / 2;
  } else {
    // The window is taller that the source - crop along X to preserve the
    // aspect ratio while stretching throughout the entire surface's height,
    // then limit the X cropping via letterboxing or stretching along Y.
    uint32_t present_safe_area;
    if (config.GetAllowOverscanCutoff() && REXCVAR_GET(present_safe_area_x) > 0 &&
        REXCVAR_GET(present_safe_area_x) < 100) {
      present_safe_area = uint32_t(REXCVAR_GET(present_safe_area_x));
    } else {
      present_safe_area = 100;
    }
    // Scale the desired height by the W:H aspect ratio to get the width.
    output_width =
        rescale_unsigned(surface_height_in_paint_connection_, properties.display_aspect_ratio_x,
                         properties.display_aspect_ratio_y);
    bool letterbox = false;
    if (output_width * present_safe_area > surface_width_in_paint_connection_ * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      output_width = rescale_unsigned(surface_width_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && REXCVAR_GET(present_letterbox)) {
      output_height = rescale_unsigned(surface_width_in_paint_connection_ * 100,
                                       properties.display_aspect_ratio_y,
                                       properties.display_aspect_ratio_x * present_safe_area);
      // output_height might have been rounded up already by rescale_unsigned,
      // so rounding down in this division.
      flow.output_y = (int32_t(surface_height_in_paint_connection_) - int32_t(output_height)) / 2;
    } else {
      output_height = surface_height_in_paint_connection_;
      flow.output_y = 0;
    }
    // output_width might have been rounded up already by rescale_unsigned, so
    // rounding down in this division.
    flow.output_x = (int32_t(surface_width_in_paint_connection_) - int32_t(output_width)) / 2;
  }

  // Convert the location from surface pixels (which have 1:1 aspect ratio
  // relatively to the physical display) to render target pixels (the render
  // target size may be arbitrary with any aspect ratio, but if it's different
  // than the surface size, the OS is expected to stretch it to the surface
  // boundaries), preserving the aspect ratio.
  if (host_rt_width != surface_width_in_paint_connection_) {
    flow.output_x =
        rescale_signed(flow.output_x, host_rt_width, surface_width_in_paint_connection_);
    output_width =
        rescale_unsigned(output_width, host_rt_width, surface_width_in_paint_connection_);
  }
  if (host_rt_height != surface_height_in_paint_connection_) {
    flow.output_y =
        rescale_signed(flow.output_y, host_rt_height, surface_height_in_paint_connection_);
    output_height =
        rescale_unsigned(output_height, host_rt_height, surface_height_in_paint_connection_);
  }

  // The out-of-bounds checks are needed for correct letterbox calculations.
  // Though this normally shouldn't happen, but in case of rounding issues with
  // extreme values.
  int32_t output_right = flow.output_x + int32_t(output_width);
  int32_t output_bottom = flow.output_y + int32_t(output_height);
  // In rare cases (for example, when surface and render-target coordinate
  // spaces diverge unexpectedly), the computed output may become anchored at
  // the top-left and smaller than the host render target. This results in only
  // a top-left region being presented with black right/bottom areas.
  if (flow.output_x >= 0 && flow.output_y >= 0 && flow.output_x <= 1 && flow.output_y <= 1 &&
      output_width < host_rt_width && output_height < host_rt_height) {
    static std::atomic<bool> logged_top_left_expand = false;
    if (!logged_top_left_expand.exchange(true)) {
      REXLOG_WARN(
          "Presenter: expanding guest output from {}x{} to host render target "
          "{}x{} to avoid top-left-only presentation",
          output_width, output_height, host_rt_width, host_rt_height);
    }
    output_width = host_rt_width;
    output_height = host_rt_height;
    output_right = int32_t(output_width);
    output_bottom = int32_t(output_height);
  }
  if (!output_width || !output_height || output_right <= 0 || output_bottom <= 0 ||
      flow.output_x >= int32_t(host_rt_width) || flow.output_y >= int32_t(host_rt_height)) {
    return flow;
  }

  // The output image may have a part of it outside the final render target (if
  // using the overscan area to stretch the image to the entire surface while
  // preserving the guest aspect ratio if it differs from the host one, for
  // instance). While the final render target size is known to be within the
  // host render target / image size limit, the intermediate images may be
  // larger than that as they include the overscan area that will be outside the
  // screen. Make sure the intermediate images can't be larger than the maximum
  // render target size.
  uint32_t output_width_clamped = std::min(output_width, max_rt_width);
  uint32_t output_height_clamped = std::min(output_height, max_rt_height);

#if defined(REX_HAS_FIDELITYFX_SDK)
  if (config.GetEffect() == GuestOutputPaintConfig::Effect::kCas ||
      config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
      GuestOutputPaintConfig::IsTemporalEffect(config.GetEffect())) {
    // FidelityFX Super Resolution and Contrast Adaptive Sharpening only work
    // good for up to 2x2 upscaling due to the way they fetch texels.
    // CAS is primarily a sharpening filter, not an upscaling one (its upscaling
    // eliminates reduces blurriness, but doesn't preserve the shapes of edges,
    // and executing it multiple times will only result in oversharpening. So,
    // using it for scales only of up to 2x2, then simply stretching with
    // bilinear filtering.
    // EASU of FSR, however, preserves edges, it's not supposed to blur them or
    // to make them jagged, so it can be executed multiple times - running
    // multiple EASU passes for scale factors of over 2x2.
    // Just one EASU pass rather than multiple for scaling to factors bigger
    // than 2x2 (especially significantly bigger, such as 1152x640 to 3840x2160,
    // or 3.333x3.375) results in blurry edges and an overall noisy look,
    // multiple passes improve visual stability.
    std::pair<uint32_t, uint32_t> ffx_last_size;
    if (flow.effect_count) {
      ffx_last_size = flow.effect_output_sizes[flow.effect_count - 1];
    } else {
      ffx_last_size.first = properties.frontbuffer_width;
      ffx_last_size.second = properties.frontbuffer_height;
    }
    bool is_temporal_effect = GuestOutputPaintConfig::IsTemporalEffect(config.GetEffect());
    bool temporal_quality_mode_forced =
        is_temporal_effect &&
        config.GetFsrQualityMode() != GuestOutputPaintConfig::FsrQualityMode::kAuto;
    // A temporal upscaler also runs at a 1:1 ratio (DLAA, FSR native AA,
    // #804): it anti-aliases and reconstructs instead of scaling.
    const bool temporal_native_ratio = is_temporal_effect &&
                                       ffx_last_size.first == output_width_clamped &&
                                       ffx_last_size.second == output_height_clamped;
    if ((config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr || is_temporal_effect) &&
        ((ffx_last_size.first < output_width_clamped ||
          ffx_last_size.second < output_height_clamped) ||
         temporal_quality_mode_forced || temporal_native_ratio)) {
      // AMD FidelityFX Super Resolution - upsample along at least one axis.
      // Using the output size clamped to the maximum render target size here as
      // EASU will always write to intermediate images, and RCAS supports only
      // 1:1.
      if (is_temporal_effect) {
        uint32_t temporal_input_width = ffx_last_size.first;
        uint32_t temporal_input_height = ffx_last_size.second;
        uint32_t quality_mode_render_width = 0;
        uint32_t quality_mode_render_height = 0;
        if (QueryTemporalFsrRenderResolutionFromQualityMode(
                output_width_clamped, output_height_clamped, config.GetFsrQualityMode(),
                quality_mode_render_width, quality_mode_render_height)) {
          quality_mode_render_width =
              std::max(uint32_t(1), std::min(quality_mode_render_width, output_width_clamped));
          quality_mode_render_height =
              std::max(uint32_t(1), std::min(quality_mode_render_height, output_height_clamped));
          if (quality_mode_render_width < temporal_input_width ||
              quality_mode_render_height < temporal_input_height) {
            temporal_input_width = quality_mode_render_width;
            temporal_input_height = quality_mode_render_height;
            if (temporal_input_width != ffx_last_size.first ||
                temporal_input_height != ffx_last_size.second) {
              assert_true(flow.effect_count < flow.effects.size());
              flow.effect_output_sizes[flow.effect_count] =
                  std::make_pair(temporal_input_width, temporal_input_height);
              // Pre-temporal quality mode may request a lower render
              // resolution. Use CAS resample since bilinear is not allowed in
              // intermediate passes.
              flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kCasResample;
              ffx_last_size.first = temporal_input_width;
              ffx_last_size.second = temporal_input_height;
            }
          } else if (quality_mode_render_width > temporal_input_width ||
                     quality_mode_render_height > temporal_input_height) {
            LogTemporalFsrQualityModeInputLimitOnce();
          }
        }

        // A single temporal upscaler dispatch can target the final clamped
        // size directly, unlike the spatial multi-pass chain.
        ffx_last_size.first = output_width_clamped;
        ffx_last_size.second = output_height_clamped;
        assert_true(flow.effect_count < flow.effects.size());
        flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
        flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrEasu;
      } else {
        uint32_t easu_max_passes = config.GetFsrMaxUpsamplingPasses();
        uint32_t easu_pass_count = 0;
        while (easu_pass_count < easu_max_passes &&
               (ffx_last_size.first < output_width_clamped ||
                ffx_last_size.second < output_height_clamped)) {
          ffx_last_size.first = std::min(ffx_last_size.first * uint32_t(2), output_width_clamped);
          ffx_last_size.second =
              std::min(ffx_last_size.second * uint32_t(2), output_height_clamped);
          assert_true(flow.effect_count < flow.effects.size());
          flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
          flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrEasu;
          ++easu_pass_count;
        }
      }
      assert_true(flow.effect_count < flow.effects.size());
      flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
      flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrRcas;
    } else {
      // AMD FidelityFX Contrast Adaptive Sharpening - sharpen or downsample, or
      // upsample up to 2x2 if CAS is specified to be used for upscaling too.
      // Using the unclamped output size as CAS may be the last pass - if a
      // bilinear pass is needed afterwards, and the CAS pass will be writing to
      // an intermediate image, the CAS pass output size will be clamped while
      // adding the bilinear stretch.
      std::pair<uint32_t, uint32_t> pre_cas_size = ffx_last_size;
      ffx_last_size.first = std::min(ffx_last_size.first * uint32_t(2), output_width);
      ffx_last_size.second = std::min(ffx_last_size.second * uint32_t(2), output_height);
      assert_true(flow.effect_count < flow.effects.size());
      flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
      flow.effects[flow.effect_count++] = ffx_last_size == pre_cas_size
                                              ? GuestOutputPaintEffect::kCasSharpen
                                              : GuestOutputPaintEffect::kCasResample;
    }
  }
#endif  // defined(REX_HAS_FIDELITYFX_SDK)

  std::pair<uint32_t, uint32_t>* last_pre_bilinear_effect_size =
      flow.effect_count ? &flow.effect_output_sizes[flow.effect_count - 1] : nullptr;
  if (!last_pre_bilinear_effect_size || last_pre_bilinear_effect_size->first != output_width ||
      last_pre_bilinear_effect_size->second != output_height) {
    // If not using FidelityFX, or it has reached its upscaling capabilities,
    // but more is needed, stretch via bilinear filtering.
    // Clamp the output size of the last effect to the maximum render target
    // size because it will go to an intermediate image now.
    if (last_pre_bilinear_effect_size) {
#if defined(REX_HAS_FIDELITYFX_SDK)
      // RCAS only works for 1:1, clamping must be done explicitly for FSR.
      assert_false(flow.effects[flow.effect_count - 1] == GuestOutputPaintEffect::kFsrRcas &&
                   (last_pre_bilinear_effect_size->first > max_rt_width ||
                    last_pre_bilinear_effect_size->second > max_rt_height));
#endif
      last_pre_bilinear_effect_size->first =
          std::min(last_pre_bilinear_effect_size->first, max_rt_width);
      last_pre_bilinear_effect_size->second =
          std::min(last_pre_bilinear_effect_size->second, max_rt_height);
    }
    assert_true(flow.effect_count < flow.effects.size());
    flow.effect_output_sizes[flow.effect_count] = std::make_pair(output_width, output_height);
    flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kBilinear;
  }

  assert_not_zero(flow.effect_count);

  if (config.GetDither()) {
    // Dithering must be applied only to the final effect since resampling and
    // sharpening filters may considering the dithering noise features and
    // amplify it.
    GuestOutputPaintEffect& last_effect = flow.effects[flow.effect_count - 1];
    switch (last_effect) {
      case GuestOutputPaintEffect::kBilinear:
        // Dithering has no effect for 1:1 copying of a 8bpc image.
        if (!properties.is_8bpc || flow.effect_count > 1 ||
            output_width != properties.frontbuffer_width ||
            output_height != properties.frontbuffer_height) {
          last_effect = GuestOutputPaintEffect::kBilinearDither;
        }
        break;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
        last_effect = GuestOutputPaintEffect::kCasSharpenDither;
        break;
      case GuestOutputPaintEffect::kCasResample:
        last_effect = GuestOutputPaintEffect::kCasResampleDither;
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        last_effect = GuestOutputPaintEffect::kFsrRcasDither;
        break;
#endif
      default:
        break;
    }
  }

#ifndef NDEBUG
  for (size_t i = 0; i + 1 < flow.effect_count; ++i) {
    assert_true(CanGuestOutputPaintEffectBeIntermediate(flow.effects[i]));
  }
  assert_true(CanGuestOutputPaintEffectBeFinal(flow.effects[flow.effect_count - 1]));
#endif

  // Calculate the letterbox geometry.
  if (flow.effect_count) {
    flow.letterbox_clear_rectangle_count = 0;
    uint32_t letterbox_mid_top = uint32_t(std::max(flow.output_y, int32_t(0)));
    // Top.
    if (letterbox_mid_top) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = 0;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = letterbox_mid_top;
    }
    uint32_t letterbox_mid_bottom = std::min(uint32_t(output_bottom), host_rt_height);
    uint32_t letterbox_mid_height = letterbox_mid_bottom - letterbox_mid_top;
    // Middle-left.
    if (flow.output_x > 0) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_left =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_left.x = 0;
      letterbox_clear_rectangle_left.y = letterbox_mid_top;
      letterbox_clear_rectangle_left.width = uint32_t(flow.output_x);
      letterbox_clear_rectangle_left.height = letterbox_mid_height;
    }
    // Middle-right.
    if (uint32_t(output_right) < host_rt_width) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_right =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_right.x = uint32_t(output_right);
      letterbox_clear_rectangle_right.y = letterbox_mid_top;
      letterbox_clear_rectangle_right.width = host_rt_width - uint32_t(output_right);
      letterbox_clear_rectangle_right.height = letterbox_mid_height;
    }
    // Bottom.
    if (letterbox_mid_bottom < host_rt_height) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = letterbox_mid_bottom;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = host_rt_height - letterbox_mid_bottom;
    }
  }

  return flow;
}

void Presenter::SetPaintedGuestOutputRect(const GuestOutputPaintFlow* flow) {
  if (!flow || !flow->effect_count) {
    painted_guest_output_rect_.reset();
    return;
  }
  const auto& size = flow->effect_output_sizes[flow->effect_count - 1];
  painted_guest_output_rect_ = GuestOutputRect{flow->output_x, flow->output_y, size.first, size.second};
}

void Presenter::ExecuteUIDrawersFromUIThread(UIDrawContext& ui_draw_context) {
  // May be called by the implementations only when requested.
  assert_true(is_in_ui_thread_paint_);
  // Drawers can add or remove drawers (including themselves), need to ensure
  // iterator validity in this case.
  assert_false(is_executing_ui_drawers_);
  ui_draw_next_iterator_ = ui_drawers_.begin();
  is_executing_ui_drawers_ = true;
  while (ui_draw_next_iterator_ != ui_drawers_.end()) {
    // The current iterator may be invalidated, and ui_draw_next_iterator_ may
    // be changed, during the execution of the drawer if the list of the drawers
    // is modified by it - don't assume that after the call
    // ui_draw_next_iterator_ will be the same as
    // std::next(ui_draw_next_iterator_) before it.
    auto it_current = ui_draw_next_iterator_++;
    // Don't draw twice if already drawn in this frame (may happen if the Z
    // order of a drawer was increased from below the current one to above it by
    // one of the drawers).
    if (it_current->second.last_draw != ui_draw_current_) {
      ui_draw_current_z_order_ = it_current->first;
      it_current->second.last_draw = ui_draw_current_;
      it_current->second.drawer->Draw(ui_draw_context);
    }
  }
  is_executing_ui_drawers_ = false;
  ++ui_draw_current_;
}

void Presenter::SetPaintModeFromUIThread(PaintMode new_mode) {
  // Can be modified only from the UI thread, so can skip locking if it's the
  // same.
  if (paint_mode_ == new_mode) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(paint_mode_mutex_);
    paint_mode_ = new_mode;
    // Without a paintable connection a real present the pacer still holds is
    // dropped and the pair history starts over.
    if (new_mode == PaintMode::kNone &&
        frame_generation_was_active_.exchange(false, std::memory_order_relaxed)) {
      StopFrameGenerationFromAnyThread();
      PublishFrameGenerationActive(false);
    }
  }
  UpdateUITicksNeededFromUIThread();
}

FrameGenerationPacer& Presenter::GetOrCreateFrameGenerationPacer() {
  std::lock_guard<std::mutex> lock(frame_generation_pacer_mutex_);
  if (!frame_generation_pacer_) {
    frame_generation_pacer_ = std::make_unique<FrameGenerationPacer>(
        [this](uint64_t sequence) { PresentScheduledRealFrame(sequence); });
  }
  return *frame_generation_pacer_;
}

void Presenter::ShutdownFrameGenerationPacer() {
  std::unique_ptr<FrameGenerationPacer> pacer;
  {
    std::lock_guard<std::mutex> lock(frame_generation_pacer_mutex_);
    pacer = std::move(frame_generation_pacer_);
  }
  // Joined outside the lock: the thread's last present may be running.
  pacer.reset();
}

void Presenter::StopFrameGenerationFromAnyThread() {
  std::lock_guard<std::mutex> lock(frame_generation_pacer_mutex_);
  if (frame_generation_pacer_) {
    if (frame_generation_pacer_->CancelPending() != UINT64_MAX) {
      PROFILE_DROPPED_PRESENT();
    }
    frame_generation_pacer_->ResetIntervals();
  }
  ResetFrameGenerationHistory();
}

Presenter::FrameGenerationUIPaintRequest Presenter::TakeFrameGenerationUIRequest(
    bool& more_pending_out) {
  std::lock_guard<std::mutex> request_lock(frame_generation_ui_request_mutex_);
  FrameGenerationUIPaintRequest request;
  more_pending_out = false;
  if (frame_generation_ui_real_request_.sequence != 0) {
    request = frame_generation_ui_real_request_;
    frame_generation_ui_real_request_ = FrameGenerationUIPaintRequest();
    // The real frame was published before the generated request's frame, so
    // the generated request still waiting is painted on the next request.
    more_pending_out = frame_generation_ui_generated_request_.sequence != 0;
    return request;
  }
  request = frame_generation_ui_generated_request_;
  frame_generation_ui_generated_request_ = FrameGenerationUIPaintRequest();
  return request;
}

Presenter::FrameGenerationUIPaintRequest Presenter::PlanFrameGenerationForPublish() {
  FrameGenerationUIPaintRequest request;
  request.sequence = guest_output_refresh_sequence_.load(std::memory_order_acquire);
  if (!IsFrameGenerationReady()) {
    if (frame_generation_was_active_.exchange(false, std::memory_order_relaxed)) {
      StopFrameGenerationFromAnyThread();
      PublishFrameGenerationActive(false);
    }
    return request;
  }
  FrameGenerationPacer& pacer = GetOrCreateFrameGenerationPacer();
  request.publish_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  const FrameGenerationPacer::Decision decision = pacer.OnPublish(request.publish_ns);
  frame_generation_interval_ns_.store(decision.interval_ns, std::memory_order_relaxed);
  // The previous frame's real present still pending (this frame came early,
  // before the pacer's deadline): it is shown now, before anything of this
  // frame, so a real frame is never lost to pacing.
  request.overdue_real_sequence = pacer.CancelPending();
  if (decision.reset_history) {
    ResetFrameGenerationHistory();
  }
  if (!decision.generate) {
    PROFILE_FRAME_GENERATION_SKIPPED();
    return request;
  }
  // Twice the base rate has to fit under what the host shows: the frame rate
  // limit, and the display's refresh when the swapchain waits for it (FIFO),
  // or the second present only pushes the real frame out.
  double present_cap_hz = 0.0;
  if (const int32_t limit = REXCVAR_GET(host_present_fps_limit); limit > 0) {
    present_cap_hz = double(limit);
  }
  if (surface_paint_connection_has_implicit_vsync_) {
    const double refresh_hz =
        std::atof(rex::cvar::GetFlagByName("video_mode_refresh_rate").c_str());
    if (refresh_hz > 0.0 && (present_cap_hz <= 0.0 || refresh_hz < present_cap_hz)) {
      present_cap_hz = refresh_hz;
    }
  }
  if (present_cap_hz > 0.0 && decision.interval_ns > 0) {
    const double base_hz = 1e9 / double(decision.interval_ns);
    // An exact fit (30 fps on a 60 Hz panel) is the common case and works;
    // only a base rate clearly above half the cap is refused.
    if (2.0 * base_hz > present_cap_hz * 1.05) {
      if (!frame_generation_headroom_logged_.exchange(true, std::memory_order_relaxed)) {
        REXLOG_INFO(
            "Presenter: frame generation is waiting: the base rate of {:.1f} fps doubled "
            "does not fit under {:.0f} presents a second",
            base_hz, present_cap_hz);
      }
      PROFILE_FRAME_GENERATION_SKIPPED();
      return request;
    }
    frame_generation_headroom_logged_.store(false, std::memory_order_relaxed);
  }
  request.source = PaintSource::kGenerated;
  request.interval_ns = decision.interval_ns;
  request.shortest_interval_ns = decision.shortest_interval_ns;
  {
    std::lock_guard<std::mutex> config_lock(guest_output_paint_config_mutex_);
    request.delay = guest_output_paint_config_.GetFrameGenerationDelay();
  }
  return request;
}

bool Presenter::PaintGeneratedFrameAndScheduleReal(const FrameGenerationUIPaintRequest& request) {
  const PaintResult paint_result = PaintAndPresent(false, PaintSource::kGenerated);
  const bool presented = paint_result == PaintResult::kPresented ||
                         paint_result == PaintResult::kPresentedSuboptimal;
  // The implementation shows the real frame when it has no generated one;
  // that present is then the real frame's, and nothing is left to schedule.
  const bool presented_generated = presented && last_paint_source_ == PaintSource::kGenerated;
  if (presented_generated) {
    GetOrCreateFrameGenerationPacer().ScheduleReal(
        request.sequence,
        request.publish_ns + int64_t(double(request.shortest_interval_ns) * request.delay));
  }
  frame_generation_was_active_.store(presented_generated, std::memory_order_relaxed);
  PublishFrameGenerationActive(presented_generated);
  return presented;
}

void Presenter::PresentScheduledRealFrame(uint64_t sequence) {
  // The same lock order as PublishGuestOutput: the paint mode first, the
  // mailbox consumer inside the paint.
  std::lock_guard<std::mutex> paint_mode_mutex_lock(paint_mode_mutex_);
  if (guest_output_refresh_sequence_.load(std::memory_order_acquire) != sequence) {
    // A newer frame was published between the pacer taking this present and
    // here; that publish shows the newer frame as the real one at once, so
    // this frame's own present is skipped, not lost from the sequence.
    PROFILE_FRAME_GENERATION_SKIPPED();
    return;
  }
  if (paint_mode_ == PaintMode::kUIThreadOnRequest) {
    // The UI thread paints (an implicit-vsync swapchain, or a window that
    // needs the UI thread): ask it for the real frame.
    {
      std::lock_guard<std::mutex> request_lock(frame_generation_ui_request_mutex_);
      frame_generation_ui_real_request_ = FrameGenerationUIPaintRequest();
      frame_generation_ui_real_request_.sequence = sequence;
    }
    RequestPaintOrConnectionRecoveryViaWindow(true);
    return;
  }
  if (paint_mode_ != PaintMode::kGuestOutputThreadImmediately ||
      surface_paint_connection_state_ != SurfacePaintConnectionState::kConnectedPaintable) {
    return;
  }
  PaintResult paint_result = PaintAndPresent(false);
  if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
    RequestPaintOrConnectionRecoveryViaWindow(true);
  }
  if (host_gpu_loss_callback_) {
    if (paint_result == PaintResult::kGpuLostResponsible) {
      host_gpu_loss_callback_(true, false);
    } else if (paint_result == PaintResult::kGpuLostExternally) {
      host_gpu_loss_callback_(false, false);
    }
  }
}

Presenter::PaintMode Presenter::GetDesiredPaintModeFromUIThread(bool is_paintable) const {
  if (!is_paintable) {
    // The only case when kNone can be returned, for surface connection updates
    // when it's known that the UI thread currently has access to the connection
    // lifecycle.
    return PaintMode::kNone;
  }
  if (!REXCVAR_GET(host_present_from_non_ui_thread)) {
    return PaintMode::kUIThreadOnRequest;
  }
  if (surface_paint_connection_has_implicit_vsync_) {
    // Don't be causing host vertical sync CPU waits in the thread generating
    // the guest output.
    return PaintMode::kUIThreadOnRequest;
  }
  if (!ui_drawers_.empty()) {
    // The UI can be drawn only by the UI thread, and it needs to be drawn -
    // paint in the UI thread.
    return PaintMode::kUIThreadOnRequest;
  }
  // Only the guest output needs to be drawn - let the guest output thread
  // present immediately for a lower latency.
  return PaintMode::kGuestOutputThreadImmediately;
}

void Presenter::DisconnectPaintingFromSurfaceFromUIThread(SurfacePaintConnectionState new_state) {
  assert_false(IsConnectedSurfacePaintConnectionState(new_state));
  if (IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_)) {
    DisconnectPaintingFromSurfaceFromUIThreadImpl();
  }
  surface_paint_connection_state_ = new_state;
  surface_paint_connection_has_implicit_vsync_ = false;
  surface_width_in_paint_connection_ = 0;
  surface_height_in_paint_connection_ = 0;
}

void Presenter::UpdateSurfacePaintConnectionFromUIThread(bool* repaint_needed_out,
                                                         bool update_paint_mode_to_desired) {
  assert_not_null(surface_);

  // Validate that painting lifecycle is accessible by the UI thread currently,
  // not given to the guest output thread. The mode can be modified only by
  // the UI thread, so no need to lock the mutex.
  assert_true(paint_mode_ != PaintMode::kGuestOutputThreadImmediately);

  // Initialize repaint_needed_out for failure cases.
  if (repaint_needed_out) {
    *repaint_needed_out = false;
  }

  // If the connection state is kUnconnectedSurfaceReportedUnusable, the
  // implementation has reported that the surface is not usable by the presenter
  // at all, and it's pointless to retry connecting to it.
  if (surface_paint_connection_state_ !=
      SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable) {
    uint32_t surface_width = 0, surface_height = 0;
    bool surface_area_available = surface_->GetSize(surface_width, surface_height);
    if (!surface_area_available) {
      // The surface is currently zero-area (or has become zero-area), try again
      // when it's resized.
      DisconnectPaintingFromSurfaceFromUIThread(
          SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    } else {
      bool is_reconnect = IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_);
      bool is_vsync_implicit = false;
      SurfacePaintConnectResult connect_result = ConnectOrReconnectPaintingToSurfaceFromUIThread(
          *surface_, surface_width, surface_height,
          surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable,
          is_vsync_implicit);
      switch (connect_result) {
        case SurfacePaintConnectResult::kSuccess:
          if (repaint_needed_out) {
            *repaint_needed_out = true;
          }
          // Fallthrough to common success handling.
        case SurfacePaintConnectResult::kSuccessUnchanged:
          // Don't know yet what the first result was (success or suboptimal).
          surface_paint_connection_was_optimal_at_successful_paint_ = false;
          surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedPaintable;
          surface_paint_connection_has_implicit_vsync_ = is_vsync_implicit;
          surface_width_in_paint_connection_ = surface_width;
          surface_height_in_paint_connection_ = surface_height;
          if (!is_reconnect) {
            *repaint_needed_out = true;
          }
          break;
        case SurfacePaintConnectResult::kFailure:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedRetryAtStateChange;
          break;
        case SurfacePaintConnectResult::kFailureSurfaceUnusable:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable;
          break;
      }
    }
  }

  if (update_paint_mode_to_desired) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(
        surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable));
  }
}

bool Presenter::RequestPaintOrConnectionRecoveryViaWindow(bool force_ui_thread_paint_tick) {
  // Can be called from any thread if an existing window_ is available in it,
  // and it's known to have a Surface that will be the same throughout this
  // call - not doing any checks whether this request can be satisfied
  // theoretically. For safety, check whether the window exists unconditionally.
  assert_not_null(window_);
  assert_not_null(surface_);
  if (ui_thread_paint_requested_.exchange(true, std::memory_order_relaxed)) {
    // Invalidation pending already, no need to do it twice.
    return false;
  }
  if (force_ui_thread_paint_tick) {
    ForceUIThreadPaintTick();
  }
  window_->RequestPaint();
  return true;
}

void Presenter::UpdateSurfaceMonitorFromUIThread(bool old_monitor_potentially_disconnected) {
  // For dropping the monitor when the window is closing and is losing its
  // surface, the existence of `surface_` (which implies that `window_` exists
  // too) must be the condition for a non-null monitor, not just the existence
  // of `window_`.
#if REX_PLATFORM_WIN32
  HMONITOR surface_new_win32_monitor = nullptr;
  if (surface_) {
    HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle());
    // The HWND may be non-existent if the window has been closed and destroyed
    // (the HWND, not the rex::ui::Window) already.
    if (hwnd) {
      surface_new_win32_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    }
  }
  if (old_monitor_potentially_disconnected || surface_win32_monitor_ != surface_new_win32_monitor) {
    surface_win32_monitor_ = surface_new_win32_monitor;
    if (dxgi_ui_tick_factory_ && !dxgi_ui_tick_factory_->IsCurrent()) {
      // If a monitor has been newly connected, it won't appear in the old
      // factory, need to recreate it.
      {
        Microsoft::WRL::ComPtr<IDXGIOutput> old_factory_output_to_release;
        {
          std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
          old_factory_output_to_release = std::move(dxgi_ui_tick_output_);
        }
      }
      dxgi_ui_tick_factory_.Reset();
    }
    if (!dxgi_ui_tick_factory_) {
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_ui_tick_factory_)))) {
        REXLOG_ERROR("Presenter: Failed to create a DXGI factory");
      }
    }
    Microsoft::WRL::ComPtr<IDXGIOutput> new_dxgi_output;
    if (dxgi_ui_tick_factory_ && surface_new_win32_monitor) {
      new_dxgi_output =
          GetDXGIOutputForMonitor(dxgi_ui_tick_factory_.Get(), surface_new_win32_monitor);
    }
    // If the adapter was recreated, and the old output was released before its
    // destruction, notifying is still required - the vertical blank wait thread
    // might have entered the condition variable wait already as the output was
    // null.
    bool signal_dxgi_ui_tick_control;
    {
      std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      bool dxgi_output_was_null = (dxgi_ui_tick_output_ == nullptr);
      dxgi_ui_tick_output_ = new_dxgi_output;
      signal_dxgi_ui_tick_control =
          dxgi_output_was_null && AreDXGIUITicksWaitable(dxgi_ui_tick_lock);
    }
    if (signal_dxgi_ui_tick_control) {
      dxgi_ui_tick_control_condition_.notify_all();
    }
  }
#endif  // XE_PLATFORM
}

bool Presenter::InSurfaceOnMonitorFromUIThread() const {
  if (!surface_) {
    return false;
  }
#if REX_PLATFORM_WIN32
  return surface_win32_monitor_ != nullptr;
#else
  return true;
#endif  // XE_PLATFORM
}

Presenter::PaintResult Presenter::PaintAndPresent(bool execute_ui_drawers, PaintSource source) {
  assert_false(execute_ui_drawers && !is_in_ui_thread_paint_);
  assert_true(surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable);
  std::lock_guard<std::mutex> pacing_lock(presentation_pacing_mutex_);
  const uint64_t refresh_sequence =
      guest_output_refresh_sequence_.load(std::memory_order_acquire);
  // A generated frame is new even when the title's refresh sequence is unchanged.
  const bool generated = source == PaintSource::kGenerated;
  using PresentationClock = std::chrono::steady_clock;
  auto now = PresentationClock::now();
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             now.time_since_epoch())
                             .count();
  if (refresh_sequence != last_seen_refresh_sequence_) {
    last_seen_refresh_sequence_ = refresh_sequence;
    last_refresh_change_ns_ = now_ns;
  }
  // An unchanged guest frame is not presented again: an overlay redraws with
  // the next guest frame. Only a suspended or stalled title (no new frame for
  // 100 ms, as under the frame inspector) gets UI repaints over its last
  // frame, and a closed overlay one final paint without drawers to erase it.
  // Presenting every UI tick while the title ran doubled the presents on
  // high-refresh displays and lagged the game with F3 open.
  constexpr int64_t kStalledTitleNs = 100'000'000;
  if (!generated && REXCVAR_GET(pinyon_shift_fh1_source_presentation) &&
      refresh_sequence == last_presented_refresh_sequence_) {
    const bool erase_overlay = !execute_ui_drawers && last_presented_ui_;
    const bool repaint_stalled =
        execute_ui_drawers && now_ns - last_refresh_change_ns_ >= kStalledTitleNs;
    if (!erase_overlay && !repaint_stalled) return PaintResult::kNotPresented;
  }
  const auto pacing = presentation_deadline_scheduler_.Plan(
      now_ns, uint32_t(REXCVAR_GET(host_present_fps_limit)));
  if (pacing.missed_deadlines) {
    PROFILE_PRESENT_DEADLINE_MISS(int64_t(pacing.missed_deadlines));
  }
  if (pacing.wait_ns > 0) {
    auto deadline = PresentationClock::time_point(
        std::chrono::nanoseconds(pacing.deadline_ns));
    if (REXCVAR_GET(high_resolution_timer_waits)) {
      rex::thread::SleepUntil(deadline, std::chrono::microseconds(50));
    } else if (REXCVAR_GET(host_present_sleep_spin) && pacing.wait_ns > 500000) {
      std::this_thread::sleep_until(deadline - std::chrono::microseconds(500));
      while (PresentationClock::now() < deadline) {
        std::this_thread::yield();
      }
    } else {
      std::this_thread::sleep_until(deadline);
    }
  }
  last_paint_source_ = PaintSource::kGuestOutput;
  PaintResult result = PaintAndPresentImpl(execute_ui_drawers, source);
  if (result == PaintResult::kPresented || result == PaintResult::kPresentedSuboptimal) {
    const int64_t present_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            PresentationClock::now().time_since_epoch())
            .count();
    PROFILE_PRESENT();
    if (last_present_time_ns_) {
      PROFILE_PRESENT_DELTA_NS(present_time_ns - last_present_time_ns_);
    }
    last_present_time_ns_ = present_time_ns;
    if (generated && last_paint_source_ == PaintSource::kGenerated) {
      // A generated frame shows no new title frame: it is neither a source
      // frame nor a repeat of one, and leaves the real frame still to present.
      PROFILE_GENERATED_PRESENT();
      perf::TraceCriticalPath(
          "present_generated", perf::GetTotalCounter(perf::CounterId::kSourceFrameCount),
          int64_t(refresh_sequence));
    } else {
      perf::TraceCriticalPath(
          "present", perf::GetTotalCounter(perf::CounterId::kSourceFrameCount),
          int64_t(refresh_sequence));
      if (refresh_sequence == last_presented_refresh_sequence_) {
        PROFILE_DUPLICATE_PRESENT();
      }
      last_presented_refresh_sequence_ = refresh_sequence;
      last_presented_ui_ = execute_ui_drawers;
    }
  }
  switch (result) {
    case PaintResult::kPresented:
      surface_paint_connection_was_optimal_at_successful_paint_ = true;
      break;
    case PaintResult::kPresentedSuboptimal:
      // Make outdated if previously optimal, now suboptimal, but don't cause
      // the connection to become outdated if it has been suboptimal from the
      // very beginning.
      if (surface_paint_connection_was_optimal_at_successful_paint_) {
        surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      }
      break;
    case PaintResult::kNotPresentedConnectionOutdated:
      surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      break;
    default:
      // Another issue not directly related to the surface connection.
      break;
  }
  return result;
}

void Presenter::HandleUIDrawersChangeFromUIThread(bool drawers_were_empty) {
  if (is_in_ui_thread_paint_) {
    // Defer the refresh so no dangerous lifecycle-related changes happen during
    // drawing.
    if (!ui_drawers_.empty()) {
      request_ui_paint_after_current_ui_thread_paint_ = true;
    }
    return;
  }

  if (paint_mode_ == PaintMode::kNone) {
    // Not connected, no point in refreshing (checking a more conservative
    // paint_mode_ because the actual connection state may currently be owned by
    // the guest output thread instead) or in toggling the ownership (the rest
    // of the function can assume it's not kNone).
    return;
  }

  if (ui_drawers_.empty() != drawers_were_empty) {
    // Require the UI thread to paint if it needs the UI, or let the guest
    // output thread paint immediately if not.
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
    // Make sure the ticks for limiting the UI frame rate are sent.
    UpdateUITicksNeededFromUIThread();
  }

  // Request painting so the changes to the UI drawer list are reflected as
  // quickly as possible.
  // RequestUIPaintFromUIThread is not enough, because a paint request is also
  // needed if disabling the UI, to force paint a frame without the UI as soon
  // as possible - it can't be dropped if ui_drawers_ is empty.
  // The coarse painting availability (and thus the availability of `window_`,
  // which is required for the paint mode to be anything else than kNone) has
  // already been checked above.
  ForceUIThreadPaintTick();
  window_->RequestPaint();
}

void Presenter::UpdateUITicksNeededFromUIThread() {
#if REX_PLATFORM_WIN32
  bool new_needed = AreUITicksNeededFromUIThread();
  if (dxgi_ui_ticks_needed_ == new_needed) {
    return;
  }
  bool signal_dxgi_ui_tick_control;
  {
    std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
    dxgi_ui_ticks_needed_ = new_needed;
    signal_dxgi_ui_tick_control = AreDXGIUITicksWaitable(dxgi_ui_tick_lock);
  }
  if (signal_dxgi_ui_tick_control) {
    dxgi_ui_tick_control_condition_.notify_all();
  }
#endif
}

void Presenter::WaitForUITickFromUIThread() {
#if REX_PLATFORM_WIN32
  if (!AreUITicksNeededFromUIThread()) {
    return;
  }
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  uint64_t last_vblank_before_wait = dxgi_ui_tick_last_vblank_;
  while (true) {
    // Guest output present requests should interrupt the wait as quickly as
    // possible as they should be fulfilled as early as possible.
    if (dxgi_ui_tick_force_requested_) {
      dxgi_ui_tick_force_requested_ = false;
      return;
    }
    if (!AreDXGIUITicksWaitable(dxgi_ui_tick_lock)) {
      return;
    }
    if (dxgi_ui_tick_last_vblank_ > dxgi_ui_tick_last_draw_) {
      // If there have been multiple vblanks during the wait for some reason,
      // next time draw the UI immediately.
      dxgi_ui_tick_last_draw_ =
          std::min(last_vblank_before_wait + uint64_t(1), dxgi_ui_tick_last_vblank_);
      return;
    }
    dxgi_ui_tick_signal_condition_.wait(dxgi_ui_tick_lock);
  }
#endif  // XE_PLATFORM
}

void Presenter::ForceUIThreadPaintTick() {
#if REX_PLATFORM_WIN32
  std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  dxgi_ui_tick_force_requested_ = true;
#endif  // XE_PLATFORM
}

#if REX_PLATFORM_WIN32
Microsoft::WRL::ComPtr<IDXGIOutput> Presenter::GetDXGIOutputForMonitor(IDXGIFactory1* factory,
                                                                       HMONITOR monitor) {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  for (UINT adapter_index = 0;
       SUCCEEDED(factory->EnumAdapters(adapter_index, adapter.ReleaseAndGetAddressOf()));
       ++adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT output_index = 0; SUCCEEDED(adapter->EnumOutputs(output_index, &output));
         ++output_index) {
      DXGI_OUTPUT_DESC output_desc;
      if (SUCCEEDED(output->GetDesc(&output_desc)) && output_desc.Monitor == monitor) {
        return std::move(output);
      }
    }
  }
  return nullptr;
}

void Presenter::DXGIUITickThread() {
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  while (true) {
    if (dxgi_ui_tick_thread_shutdown_) {
      return;
    }
    if (!AreDXGIUITicksWaitable(dxgi_ui_tick_lock)) {
      dxgi_ui_tick_control_condition_.wait(dxgi_ui_tick_lock);
      continue;
    }
    // Wait for vertical blank, with the mutex unlocked (holding a new reference
    // to the current output while it's happening) so subscribers can still do
    // early-out checks.
    bool wait_succeeded;
    {
      Microsoft::WRL::ComPtr<IDXGIOutput> dxgi_output = dxgi_ui_tick_output_;
      dxgi_ui_tick_lock.unlock();
      wait_succeeded = SUCCEEDED(dxgi_ui_tick_output_->WaitForVBlank());
    }
    dxgi_ui_tick_lock.lock();
    if (wait_succeeded) {
      ++dxgi_ui_tick_last_vblank_;
    } else {
      // Lost the ability to wait for a vertical blank on this output, notify
      // the waiting threads, and wait for a new one.
      dxgi_ui_tick_output_.Reset();
    }
    dxgi_ui_tick_signal_condition_.notify_all();
  }
}
#endif  // XE_PLATFORM

}  // namespace ui
}  // namespace rex
