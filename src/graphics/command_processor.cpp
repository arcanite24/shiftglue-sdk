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
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/ui/xr.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/session_probe.h>
#include <rex/cs_scheduler.h>
#include <rex/system/d3d_census.h>
#include <rex/system/d3d_draw.h>
#include <rex/system/d3d_program.h>
#include <rex/system/gpu_write_signal.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>

#if REX_ARCH_AMD64
#include <immintrin.h>
#elif REX_ARCH_ARM64
#include <arm_neon.h>
#endif

#include "d3d_census_decoder.h"
#include "d3d_draw_window.h"
#include "d3d_list_compiler.h"

REXCVAR_DEFINE_INT32(gpu_trace_bins_vblank, -1, "GPU",
                     "Diagnostics: log the bin masks, bin selects, screen extent writes, draws, "
                     "interrupts and indirect buffers of the frame after this many swaps, and "
                     "with the Vulkan FH1 executor the tiling registers of its draws and "
                     "resolves (-1: off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");
#if defined(REXGLUE_ENABLE_CS_PROBES)
REXCVAR_DECLARE(bool, gpu_async_fence_probe);
REXCVAR_DEFINE_BOOL(gpu_async_fence_completion, false, "GPU",
                    "CS-0.4: publish guest fences and interrupts after GPU completion without a CPU wait")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_true_fence_completion, false, "GPU",
                    "CS-0.4 probe: submit and await host GPU work before guest fence writes, "
                    "scratch writebacks, command-stream interrupts and WAIT_FOR_IDLE")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif
// FH1's race polls through WAIT_REG_MEM about 1.5 ms per frame on the GPU
// commands thread, its bottleneck; yielding first ends those waits sooner
// (NP-9.3: race frames 16.76-16.92 against 17.05-17.23 ms mean over three
// interleaved pairs).
REXCVAR_DEFINE_BOOL(gpu_record_thread, false, "GPU",
                    "Split the GPU commands thread: decode the guest's command stream on one "
                    "thread while a second applies register writes and records draws "
                    "(backends that support it; Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_record_elide_unchanged_registers, true, "GPU",
                    "With gpu_record_thread, the decoder drops register writes that leave a "
                    "plain state register or shader constant unchanged, so the recorder neither "
                    "applies them nor invalidates what depends on them")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(wait_reg_mem_yield_us, -1, "GPU",
                     "With vsync or configured short sleeps, how long a WAIT_REG_MEM poll yields "
                     "before sleeping; 0 sleeps at once, -1 chooses by platform and game rate")
    .range(-1, 16000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_record_fused_draw_registers, true, "GPU",
                    "With gpu_record_thread, a draw record carries its packet's "
                    "VGT_DRAW_INITIATOR, VGT_DMA_BASE and VGT_DMA_SIZE instead of the decoder "
                    "recording them as single-register entries (RECORDER_PER_DRAW_BACKLOG PD-1.1)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_record_shader_load_entries, true, "GPU",
                    "With gpu_record_thread, IM_LOAD is recorded as a data entry rather than a "
                    "closure, and not at all when it repeats the type's last hashed load "
                    "(RECORDER_PER_DRAW_BACKLOG PD-1.2)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_record_coalesce_scratch, true, "GPU",
                    "With gpu_record_thread, scratch register writes that do not write back are "
                    "recorded once with their latest value before the next batch, call, scratch "
                    "range run or scratch control write (RECORDER_PER_DRAW_BACKLOG PD-1.6)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_decode_compare_runs_wide, true, "GPU",
                    "With gpu_record_thread, the decoder compares a register run with the "
                    "register file four registers at a time when the run holds no register "
                    "whose write always matters (constants and most state)");
REXCVAR_DEFINE_BOOL(gpu_record_merge_constant_runs, true, "GPU",
                    "With gpu_record_thread, the decoder extends the last recorded float "
                    "constant run over a gap of up to 16 registers (their current values) "
                    "instead of recording another run, so the recorder handles fewer runs")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(gpu_d3d_program, 0, "GPU",
                     "NA-2.4 (issue #753): draw the command buffers the game marks as pilot "
                     "lists from an owned draw program: the backend keeps each draw's derived "
                     "state (shaders, primitive processing, translations, pipeline, viewport) "
                     "and reuses it while the draw's state hash, shaders and draw words hold "
                     "(0: off; 1: shadow, derive as usual and compare with the program; 2: "
                     "performance, draw from the program; 3: baseline, record and count but "
                     "derive as usual, for A/B runs; 4: timed only, the lists bracketed and "
                     "timed but no program looked up, the reference for the complete cost). "
                     "Read at each swap; Vulkan")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_template_stats, false, "GPU",
                    "Diagnostics (DR-4.2): with gpu_record_thread, compare every draw's "
                    "signature (register state, shaders, used fetch constants, draw packet) "
                    "with the same position in the same indirect buffer's previous execution "
                    "and log every 600 frames how many draws a compiled replay could reuse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_ib_identity_stats, false, "GPU",
                    "Diagnostics: hash every indirect buffer the commands thread executes and log "
                    "every 600 frames how many buffers, dwords and draws repeat byte for byte "
                    "from the previous execution at the same address (the display-list "
                    "compiler's potential hit rate)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gpu_replay_floor_probe, 0, "GPU",
                     "Diagnostics (NN-0.10, G0): without gpu_record_thread, capture the exit "
                     "state (registers, bins, shaders) of every indirect buffer, at any depth, "
                     "whose bytes repeat its previous execution and, at 2, skip a buffer whose "
                     "bytes and nested buffers match its capture and "
                     "that holds only state, constants, shader loads and draws, applying the "
                     "exit state instead: its draws are dropped (a wrong image), so the CPU "
                     "left is the floor of re-executing recorded lists. 1 captures and counts "
                     "only (the census)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(gpu_trace_ib_writers, 0, "GPU",
                     "Diagnostics (NN-0.10): watch the pages of the first N indirect buffers "
                     "called from another buffer (display lists) and of the first N outermost "
                     "ones, re-armed each time they execute, and log the guest threads (and "
                     "their link register) that write them")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gpu_d3d_census_skip_visible, 0, "GPU",
                     "Diagnostics (D3D_RENDERER_RESEARCH DL-1.1, wrong image): with "
                     "pinyon_shift_d3d_census on, skip the decoder's work for the draws the "
                     "census counts as visible to an API-level renderer (draw calls, and lists "
                     "unchanged or changed only by fixups). 1: their draw packets (EDRAM copies "
                     "still run) and shader loads (done at the next draw that runs); 2: also "
                     "their ALU constant packets; 3: also every register packet that touches "
                     "no control, render-target, window, copy or resolve register (or fetch "
                     "constant 0, which resolves read). Events, waits, memory writes, nested "
                     "buffers and EDRAM copies are always executed")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_trace_ib_writers_rearm, false, "GPU",
                    "Diagnostics (DL-0.1): with gpu_trace_ib_writers, watch a written page again "
                    "at the decoder's next indirect buffer instead of at that buffer's next "
                    "execution, so later writes between two runs are seen too (one fault each)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_trace_wait_reg_mem_writers, false, "GPU",
                    "Diagnostics: while WAIT_REG_MEM waits on a guest memory word, watch its "
                    "page and log the guest threads (and their link register) that write it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(gpu_log_long_wait_reg_mem_ms, 0, "GPU",
                     "Diagnostics: log each WAIT_REG_MEM that waits at least this many "
                     "milliseconds (what it polled, the compare, the value before and after); "
                     "0 turns it off")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(gpu_idle_spin_count, 500, "GPU",
                     "How many times the GPU commands thread yields, when the ring is empty, "
                     "before it waits for the guest to write more (each yield is a busy spin "
                     "when no other thread wants the core)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wait_reg_mem_sleep_us, -1, "GPU",
                     "How long each WAIT_REG_MEM sleep after the yield period lasts, even without vsync; "
                     "0 retains busy polling without vsync, or uses the packet interval with vsync. Short sleeps "
                     "stop the commands thread spinning for power without the millisecond's "
                     "latency; -1 chooses by platform and game rate")
    .range(-1, 16000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
struct WaitRegMemPolicy {
  int32_t yield_us;
  int32_t sleep_us;
};

// The WAIT_REG_MEM yield and sleep, -1 in either setting chosen here (LS-2.1,
// LS-2.2). POSIX sleeps are fine-grained: short sleeps instead of a busy
// yield, for power (Snapdragon 8 Elite: a third less CPU on the commands
// thread, same frames); Linux and the Steam Deck share it. On Windows, at a
// game rate of 60 or less, 100 us sleeps on a high-resolution waitable timer
// after a 200 us yield took 2.3-3.5 ms less decoder CPU a race frame than
// the former 2 ms yield, with a p95 about 0.9 ms lower, on 16, 8 and 4
// threads of a Ryzen 7 5800X. Above 60 the wake-up latency cost 3 % of the
// frame rate at 120, so faster rates keep the 2 ms yield (NP-9.3). macOS
// takes the Windows choice: its short sleeps overshoot under load, and on an
// M4 Pro free-roam drive the 100 us sleeps gave 187 frames over 25 ms and a
// p99 of 41.8 ms against 23 frames and 19.8 ms with the 2 ms yield.
WaitRegMemPolicy CurrentWaitRegMemPolicy() {
  WaitRegMemPolicy policy{REXCVAR_GET(wait_reg_mem_yield_us), REXCVAR_GET(wait_reg_mem_sleep_us)};
#if REX_PLATFORM_WIN32 || REX_PLATFORM_MAC
  if (policy.yield_us < 0 || policy.sleep_us < 0) {
    // The game rate's settings live in other modules: read them by name,
    // at most twice a second (the decoder thread is the only caller).
    thread_local bool fast = false;
    thread_local std::chrono::steady_clock::time_point next_read{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_read) {
      next_read = now + std::chrono::milliseconds(500);
      const uint32_t headset_hz = rex::ui::xr::HeadsetRefreshHz();
      const int limit = headset_hz ? int(headset_hz)
                                   : std::atoi(rex::cvar::GetFlagByName("pinyon_shift_fh1_render_fps_limit").c_str());
      const double rate = limit > 0 ? double(limit)
                                    : std::atof(rex::cvar::GetFlagByName("video_mode_refresh_rate").c_str());
      fast = rate > 61.0;
    }
    if (policy.yield_us < 0) policy.yield_us = fast ? 2000 : 200;
    if (policy.sleep_us < 0) policy.sleep_us = fast ? 0 : 100;
  }
#else
  if (policy.yield_us < 0) policy.yield_us = 100;
  if (policy.sleep_us < 0) policy.sleep_us = 100;
#endif
  return policy;
}
}  // namespace

REXCVAR_DEFINE_STRING(fh1_debug_skip_draws, "", "GPU",
                      "Diagnostics: skip the draws with these indices in every frame, as "
                      "<first>-<last>[,<first>-<last>...] counted from each frame's first draw "
                      "(bisecting a rendering fault in a frame replay)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_null_fetch, "", "GPU",
                      "Diagnostics: <draw>:<fetch constant> - that draw of every frame sees "
                      "the texture fetch constant as invalid (a null texture)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_log_draws, false, "GPU",
                    "Diagnostics: log every draw's index in its frame, primitive type, index "
                    "count and vertex and pixel shader hashes (for frame replays)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(clear_memory_page_state, true, "GPU",
                    "Refresh page-valid state from GPU-written memory at frame end. "
                    "Disable for minor CPU overhead reduction, but may break memory coherency.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if REX_HAS_VULKAN
REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, false, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif  // REX_HAS_VULKAN

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();
  worker_thread_->MarkLatencyCritical();

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  {
    // A paused worker (the app closed from the background) must wake to exit.
    std::lock_guard<std::mutex> lock(pause_mutex_);
    worker_paused_ = false;
  }
  pause_signal_.notify_all();
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  cs_scheduler::Actor scheduled(cs_scheduler::kMain);
  if (!pending_any_ && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_fns_.push({std::move(fn), false});
    pending_any_ = true;
  }
}

void CommandProcessor::CallInWorkerThread(std::function<void()> fn) {
  cs_scheduler::Actor scheduled(cs_scheduler::kMain);
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_fns_.push({std::move(fn), true});
  pending_any_ = true;
}

void CommandProcessor::RestoreFrameDumpRegisters(std::span<const uint32_t> registers) {
  assert_true(registers.size() == RegisterFile::kRegisterCount);
  for (uint32_t i = 0; i < registers.size(); ++i) {
    if (!IsPerDrawRegister(i) &&
        !(i >= XE_GPU_REG_SHADER_CONSTANT_000_X && i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
      state_hash_ ^= StateHashTerm(i, register_file_->values[i]) ^ StateHashTerm(i, registers[i]);
    }
  }
  ++state_epoch_;
  ++render_state_epoch_;
  std::memcpy(register_file_->values, registers.data(), registers.size_bytes());
  active_vertex_shader_ = nullptr;
  active_pixel_shader_ = nullptr;
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

#if REX_HAS_VULKAN
bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_name_ &&
      rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}
#endif  // REX_HAS_VULKAN

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  cs_scheduler::Actor scheduled(cs_scheduler::kGpu);
  if (session_probe::Enabled()) session_probe::SetHostRole(3);
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }
  if (REXCVAR_GET(gpu_record_thread) && SupportsRecordThread()) {
    StartRecordThread();
  }

  int64_t decoder_cpu_ns = 0;
  while (worker_running_) {
    while (pending_any_) {
      PendingCall call;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_fns_.empty()) {
          pending_any_ = false;
          break;
        }
        call = std::move(pending_fns_.front());
        pending_fns_.pop();
        pending_any_ = !pending_fns_.empty();
      }
      if (record_split_ && !call.on_worker) {
        RecordCall(std::move(call.fn));
        PublishRecordBatch();
      } else {
        call.fn();
      }
    }

    {
      const int64_t cpu_ns = perf::CurrentThreadCpuTimeNs();
      if (decoder_cpu_ns && cpu_ns >= decoder_cpu_ns) {
        PERF_counter_add(kGpuDecoderCpuNs, cpu_ns - decoder_cpu_ns);
      }
      decoder_cpu_ns = cpu_ns;
    }
    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PublishRecordBatch();
      if (!record_split_) {
        PrepareForWait();
      }
      const auto idle_start = std::chrono::steady_clock::now();
      uint32_t loop_count = 0;
      do {
        if (cs_scheduler::InActor()) {
          cs_scheduler::Point(0,cs_scheduler::PointKind::kWait,true,1000000);
        } else {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > uint32_t(REXCVAR_GET(gpu_idle_spin_count))) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        }
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && !pending_any_ &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      PERF_counter_add(kGpuThreadIdleNs,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - idle_start)
                           .count());
      if (!record_split_) {
        ReturnFromWait();
      }
      if (!worker_running_ || pending_any_) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  StopRecordThread();
  ShutdownContext();
}

void CommandProcessor::StartRecordThread() {
  decode_register_file_ = std::make_unique<RegisterFile>();
  std::memcpy(decode_register_file_->values, register_file_->values,
              sizeof(decode_register_file_->values));
  decode_extended_register_values_ = extended_register_values_;
  record_batch_ = std::make_unique<RecordBatch>();
  record_stop_ = false;
  record_published_ = 0;
  record_completed_ = 0;
  record_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        RecordThreadMain();
        return 0;
      }));
  record_thread_->set_name("GPU Recorder");
  record_thread_->Create();
  record_thread_->MarkLatencyCritical();
  elide_unchanged_registers_ = REXCVAR_GET(gpu_record_elide_unchanged_registers);
  record_split_ = true;
  REXGPU_INFO("GPU commands: decoding and recording on separate threads");
}

void CommandProcessor::StopRecordThread() {
  if (!record_split_) {
    return;
  }
  RecordSync();
  {
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_stop_ = true;
  }
  record_ready_.notify_all();
  record_thread_->Wait(0, 0, 0, nullptr);
  record_thread_.reset();
  record_split_ = false;
}

void CommandProcessor::RecordThreadMain() {
  if (session_probe::Enabled()) session_probe::SetHostRole(4);
  // The thread's CPU time is a system call (GetThreadTimes cost 2 % of the
  // recorder when read around every batch), and it does not advance while the
  // thread waits, so read it before waiting and at most once a millisecond.
  int64_t cpu_read_ns = perf::CurrentThreadCpuTimeNs();
  auto cpu_read_time = std::chrono::steady_clock::now();
  auto read_cpu = [&](std::chrono::steady_clock::time_point now) {
    const int64_t cpu_ns = perf::CurrentThreadCpuTimeNs();
    if (cpu_ns >= cpu_read_ns) {
      PERF_counter_add(kGpuRecorderCpuNs, cpu_ns - cpu_read_ns);
    }
    cpu_read_ns = cpu_ns;
    cpu_read_time = now;
  };
  // A finished batch goes back to the free list and the next one is taken
  // under one lock (PD-1.5).
  std::unique_ptr<RecordBatch> batch;
  for (;;) {
    bool notify_done = false;
    {
      std::unique_lock<std::mutex> lock(record_mutex_);
      if (batch) {
        record_free_.push_back(std::move(batch));
        ++record_completed_;
        notify_done = record_sync_waiters_ != 0;
      }
      if (record_queue_.empty() && !record_stop_) {
        lock.unlock();
        if (notify_done) {
          record_done_.notify_all();
          notify_done = false;
        }
        read_cpu(std::chrono::steady_clock::now());
        lock.lock();
        record_recorder_waiting_ = true;
        record_ready_.wait(lock, [this]() { return record_stop_ || !record_queue_.empty(); });
        record_recorder_waiting_ = false;
      }
      if (record_queue_.empty()) {
        lock.unlock();
        if (notify_done) {
          record_done_.notify_all();
        }
        return;
      }
      batch = std::move(record_queue_.front());
      record_queue_.pop_front();
    }
    if (notify_done) {
      record_done_.notify_all();
    }
    const auto busy_start = std::chrono::steady_clock::now();
    ExecuteRecordBatch(*batch);
    const auto busy_end = std::chrono::steady_clock::now();
    const int64_t busy_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(busy_end - busy_start).count();
    PERF_counter_add(kGpuRecorderBusyNs, busy_ns);
    record_batch_ns_ += uint64_t(busy_ns);
    if (busy_end - cpu_read_time >= std::chrono::milliseconds(1)) {
      read_cpu(busy_end);
    }
    batch->words.clear();
    batch->fns.clear();
  }
}

void CommandProcessor::ExecuteRecordBatch(RecordBatch& batch) {
  const uint32_t* words = batch.words.data();
  const size_t count = batch.words.size();
  if (record_cost_enabled_) {
    ExecuteRecordBatchTimed(batch);
    return;
  }
  for (size_t i = 0; i < count;) {
    switch (words[i]) {
      case kRecordRun: {
        const uint32_t start = words[i + 1];
        const uint32_t run = words[i + 2];
        WriteRegistersHost(start, words + i + 3, run);
        i += 3 + run;
      } break;
      case kRecordOne:
        WriteRegister(words[i + 1], words[i + 2]);
        i += 3;
        break;
      case kRecordCall:
        batch.fns[words[i + 1]]();
        i += 2;
        break;
      case kRecordDraw: {
        DrawRecord record;
        std::memcpy(&record, words + i + 1, sizeof(record));
        ExecuteDrawRecord(record);
        i += 1 + kDrawRecordWords;
      } break;
      case kRecordLoadShader:
        ExecuteLoadShaderRecord(words + i + 1);
        i += 1 + kLoadShaderRecordWords;
        break;
      default:
        assert_always();
        return;
    }
  }
}

uint32_t CommandProcessor::RecordRunClass(uint32_t start_index) {
  if (start_index >= 0x2000 && start_index < 0x2400) return 0;
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      start_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    return 1;
  }
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      start_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    return 2;
  }
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      start_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    return 3;
  }
  return 4;
}

void CommandProcessor::ExecuteRecordBatchTimed(RecordBatch& batch) {
  // ExecuteRecordBatch with each entry kind's time (RR-0.1's coverage).
  auto elapsed = [](uint64_t start) { return CostTicks() - start; };
  const uint64_t batch_start = CostTicks();
  const uint32_t* words = batch.words.data();
  const size_t count = batch.words.size();
  if (!record_census_) {
    record_census_ = std::make_unique<RecordCensus>();
  }
  RecordCensus* census = record_census_.get();
  ++census->batches;
  for (size_t i = 0; i < count;) {
    const uint64_t start = CostTicks();
    ++record_entries_;
    switch (words[i]) {
      case kRecordRun: {
        const uint32_t first = words[i + 1];
        const uint32_t run = words[i + 2];
        WriteRegistersHost(first, words + i + 3, run);
        i += 3 + run;
        const uint32_t run_class = RecordRunClass(first);
        ++census->runs[run_class];
        census->run_registers[run_class] += run;
      } break;
      case kRecordOne:
        if (words[i + 1] >= XE_GPU_REG_SCRATCH_REG0 && words[i + 1] <= XE_GPU_REG_SCRATCH_REG7) {
          census->scratch_same += register_file_->values[words[i + 1]] == words[i + 2];
          census->scratch_writeback +=
              (register_file_->values[XE_GPU_REG_SCRATCH_UMSK] >>
               (words[i + 1] - XE_GPU_REG_SCRATCH_REG0)) & 1;
        }
        WriteRegister(words[i + 1], words[i + 2]);
        if (words[i + 1] < RegisterFile::kRegisterCount) {
          ++census->ones[words[i + 1]];
        } else {
          ++census->ones_other;
        }
        i += 3;
        record_ones_ns_ += elapsed(start);
        break;
      case kRecordCall:
        batch.fns[words[i + 1]]();
        i += 2;
        record_calls_ns_ += elapsed(start);
        ++census->calls;
        break;
      case kRecordDraw: {
        DrawRecord record;
        std::memcpy(&record, words + i + 1, sizeof(record));
        ExecuteDrawRecord(record);
        i += 1 + kDrawRecordWords;
        record_draws_ns_ += elapsed(start);
        ++census->draws;
      } break;
      case kRecordLoadShader:
        ExecuteLoadShaderRecord(words + i + 1);
        i += 1 + kLoadShaderRecordWords;
        record_calls_ns_ += elapsed(start);
        ++census->calls;
        break;
      default:
        assert_always();
        return;
    }
  }
  record_batch_ticks_ += CostTicks() - batch_start;
}

namespace {
// Registers whose write does something beyond storing the value, whatever the
// value: scratch writeback, the coherency status, the gamma ramp (its index
// auto-increments on the recorder only), and the event and draw initiators.
bool RegisterWriteAlwaysMatters(uint32_t index) {
  return (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) ||
         index == XE_GPU_REG_COHER_STATUS_HOST ||
         (index >= XE_GPU_REG_DC_LUT_RW_INDEX && index <= XE_GPU_REG_DC_LUT_30_COLOR) ||
         (index >= XE_GPU_REG_VGT_EVENT_INITIATOR && index <= XE_GPU_REG_VGT_DRAW_INITIATOR);
}

// Whether [start, end) holds a register RegisterWriteAlwaysMatters names. Most
// runs (float and fetch constants, most state) hold none, so their registers
// can be compared without asking per register.
bool RunHasRegisterWriteAlwaysMatters(uint32_t start, uint32_t end) {
  auto overlaps = [&](uint32_t first, uint32_t last) { return start <= last && end > first; };
  return overlaps(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_SCRATCH_REG7) ||
         overlaps(XE_GPU_REG_COHER_STATUS_HOST, XE_GPU_REG_COHER_STATUS_HOST) ||
         overlaps(XE_GPU_REG_DC_LUT_RW_INDEX, XE_GPU_REG_DC_LUT_30_COLOR) ||
         overlaps(XE_GPU_REG_VGT_EVENT_INITIATOR, XE_GPU_REG_VGT_DRAW_INITIATOR);
}

// The first index in [i, count) whose guest (big-endian) value differs from
// the shadow register (kChanged) or equals it (!kChanged), or count. Four
// registers a step: the decoder compares about a million dwords a frame.
template <bool kChanged>
uint32_t FindRegisterChange(const uint32_t* shadow, const uint32_t* guest, uint32_t i,
                            uint32_t count) {
#if REX_ARCH_AMD64
  const __m128i swap = _mm_set_epi8(0x0C, 0x0D, 0x0E, 0x0F, 0x08, 0x09, 0x0A, 0x0B, 0x04, 0x05,
                                    0x06, 0x07, 0x00, 0x01, 0x02, 0x03);
  for (; i + 4 <= count; i += 4) {
    const __m128i value =
        _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(guest + i)), swap);
    const __m128i current = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadow + i));
    const int equal = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(value, current)));
    const int hit = kChanged ? (~equal & 0xF) : equal;
    if (hit) {
      return i + uint32_t(std::countr_zero(uint32_t(hit)));
    }
  }
#elif REX_ARCH_ARM64
  for (; i + 4 <= count; i += 4) {
    const uint32x4_t value =
        vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(guest + i))));
    const uint32x4_t equal = vceqq_u32(value, vld1q_u32(shadow + i));
    if (vmaxvq_u32(kChanged ? vmvnq_u32(equal) : equal)) {
      break;  // the scalar loop finds the lane
    }
  }
#endif
  for (; i < count; ++i) {
    if ((shadow[i] != rex::byte_swap(guest[i])) == kChanged) {
      return i;
    }
  }
  return count;
}
}  // namespace

void CommandProcessor::FlushPendingScratchWrites() {
  uint32_t pending = decode_pending_scratch_;
  if (!pending) {
    return;
  }
  decode_pending_scratch_ = 0;
  RecordWords& words = record_batch_->words;
  uint32_t scratch;
  while (rex::bit_scan_forward(pending, &scratch)) {
    pending &= ~(UINT32_C(1) << scratch);
    const uint32_t index = XE_GPU_REG_SCRATCH_REG0 + scratch;
    words.push_back(kRecordOne);
    words.push_back(index);
    words.push_back(decode_register_file_->values[index]);
    decode_scratch_recorded_.store(decode_scratch_recorded_.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_relaxed);
  }
}

namespace {
// gpu_replay_floor_probe (decoder thread only, unsplit decoding).
struct ReplayProbeIb {
  // The last execution's hash: the buffer's bytes, then those of the buffers
  // it called at its capture, in call order.
  uint64_t hash = 0;
  // The capture, valid while the hash and the entry state match it.
  uint64_t capture_hash = 0;
  uint64_t entry_bin_select = 0, entry_bin_mask = 0;
  bool entry_select_all = false;
  bool skippable = false;
  uint32_t drawn_draws = 0;
  std::vector<std::pair<uint32_t, uint32_t>> nested;
  std::vector<std::pair<uint32_t, uint32_t>> exit_registers;
  uint64_t exit_bin_select = 0, exit_bin_mask = 0;
  Shader* exit_vertex_shader = nullptr;
  Shader* exit_pixel_shader = nullptr;
};
struct ReplayProbeCapture {
  bool skippable = true;
  uint32_t generation = 0;
  std::vector<uint32_t> touched_generation;
  std::vector<uint32_t> touched;
  std::vector<std::pair<uint32_t, uint32_t>> nested;
};
struct ReplayProbe {
  std::unordered_map<uint64_t, ReplayProbeIb> ibs;
  // Buffers being captured, outermost first; `capturing` is whether any is.
  std::vector<ReplayProbeCapture> levels;
  uint32_t active = 0;
  bool capturing = false;
  // Enclosing buffers that repeated: a draw counts once, in the outermost
  // repeating buffer around it.
  uint32_t repeat_depth = 0;
  // Drawn draws of skipped buffers (they count in their callers' draws).
  uint64_t replayed_draws = 0;
  // LOAD_ALU_CONSTANT sources: the data hash at the last load and the swap
  // count it was last used in.
  struct ConstantSource {
    uint64_t hash = 0;
    uint32_t swap = UINT32_MAX;
  };
  std::unordered_map<uint64_t, ConstantSource> constant_sources;
};
ReplayProbe replay_probe;

void ReplayProbeMarkUnskippable() {
  for (uint32_t level = 0; level < replay_probe.active; ++level) {
    replay_probe.levels[level].skippable = false;
  }
}

void ReplayProbeTouch(uint32_t start_index, uint32_t count) {
  if (uint64_t(start_index) + count > RegisterFile::kRegisterCount) {
    ReplayProbeMarkUnskippable();
    return;
  }
  for (uint32_t level = 0; level < replay_probe.active; ++level) {
    ReplayProbeCapture& capture = replay_probe.levels[level];
    if (capture.touched_generation.empty()) {
      capture.touched_generation.resize(RegisterFile::kRegisterCount);
    }
    uint32_t* generations = capture.touched_generation.data();
    for (uint32_t i = start_index; i < start_index + count; ++i) {
      if (generations[i] != capture.generation) {
        generations[i] = capture.generation;
        capture.touched.push_back(i);
      }
    }
  }
}

// Returns the new capture's level; levels may move when a nested one starts.
uint32_t ReplayProbeBegin() {
  if (replay_probe.levels.size() <= replay_probe.active) {
    replay_probe.levels.emplace_back();
  }
  const uint32_t level = replay_probe.active++;
  ReplayProbeCapture& capture = replay_probe.levels[level];
  replay_probe.capturing = true;
  capture.skippable = true;
  if (!++capture.generation) {
    std::fill(capture.touched_generation.begin(), capture.touched_generation.end(), 0);
    capture.generation = 1;
  }
  capture.touched.clear();
  capture.nested.clear();
  return level;
}

uint64_t ReplayProbeHash(memory::Memory* memory, uint32_t ptr, uint32_t count,
                         const std::vector<std::pair<uint32_t, uint32_t>>& nested) {
  uint64_t hash = XXH3_64bits(memory->TranslatePhysical(ptr), size_t(count) * sizeof(uint32_t));
  for (const auto& [nested_ptr, nested_count] : nested) {
    hash = XXH3_64bits_withSeed(memory->TranslatePhysical(nested_ptr),
                                size_t(nested_count) * sizeof(uint32_t), hash);
  }
  return hash;
}

// gpu_trace_ib_writers: buffers watched, by page; the callback runs on the
// writing guest thread.
struct IbWriterTrace {
  std::mutex mutex;
  // Page -> (buffer address << 32 | bytes), depth in the top bit of bytes.
  std::unordered_map<uint32_t, uint64_t> pages;
  std::unordered_map<uint64_t, bool> watched;
  uint32_t watched_lists = 0, watched_outer = 0;
  std::unordered_map<uint64_t, uint32_t> logged;
  std::atomic<uint32_t> logs{0};
  std::atomic<uint32_t> swaps{0};
  // gpu_trace_ib_writers_rearm: written watched pages to watch again.
  std::vector<uint32_t> rearm_pages;
  std::atomic<bool> rearm_pending{false};
};
IbWriterTrace ib_writer_trace;

void LogIbWrite(void*, uint32_t physical_address, uint32_t length, bool is_write) {
  if (!is_write) {
    return;
  }
  const auto* thread_state = runtime::ThreadState::Get();
  const PPCContext* context = thread_state ? thread_state->context() : nullptr;
  const uint32_t lr = context ? uint32_t(context->lr) : 0u;
  uint64_t buffer = 0;
  uint32_t count = 0;
  const bool rearm = REXCVAR_GET(gpu_trace_ib_writers_rearm);
  {
    std::lock_guard<std::mutex> lock(ib_writer_trace.mutex);
    auto it = ib_writer_trace.pages.find(physical_address >> 12);
    if (it != ib_writer_trace.pages.end()) {
      buffer = it->second;
      if (rearm && ib_writer_trace.rearm_pages.size() < 65536) {
        ib_writer_trace.rearm_pages.push_back(physical_address >> 12);
        ib_writer_trace.rearm_pending.store(true, std::memory_order_release);
      }
    }
    // Each writer once per buffer, then every 64th write.
    count = ib_writer_trace.logged[(uint64_t(lr) << 32) ^ buffer]++;
  }
  // DL-0.1: the writer's class by the guest code its lr is in (fixup, stream
  // load, clone or other), counted per window by the D3D census.
  if (buffer && rex::system::d3d_census::Enabled()) {
    rex::system::d3d_census::NoteBufferWrite(uint32_t(buffer >> 32),
                                             (buffer & 0x80000000u) != 0,
                                             rex::system::d3d_census::ClassifyWriter(lr), lr);
  }
  if ((count && count % 64) ||
      ib_writer_trace.logs.fetch_add(1, std::memory_order_relaxed) >= 4000) {
    return;
  }
  REXGPU_INFO(
      "IB writer: {} {:08X}+{:X} page {:08X}+{:X} written at {:08X} (write {}) in swap {} by "
      "thread 0x{:X}, guest lr {:08X} r3 {:08X} r4 {:08X} r31 {:08X}",
      (buffer & 0x80000000u) ? "list" : "outer", uint32_t(buffer >> 32),
      uint32_t(buffer) & 0x7FFFFFFFu, physical_address, length,
      memory::CurrentAccessFaultVirtualAddress(), count + 1,
      ib_writer_trace.swaps.load(std::memory_order_relaxed), rex::thread::current_thread_id(), lr,
      context ? context->r3.u32 : 0u, context ? context->r4.u32 : 0u,
      context ? context->r31.u32 : 0u);
}

int64_t ReplayProbeNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void CommandProcessor::PacketWriteRegister(uint32_t index, uint32_t value) {
  if (replay_probe.capturing) {
    ReplayProbeTouch(index, 1);
  }
  if (!record_split_) {
    WriteRegister(index, value);
    return;
  }
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    const uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
    decode_scratch_writes_.store(decode_scratch_writes_.load(std::memory_order_relaxed) + 1,
                                 std::memory_order_relaxed);
    if (REXCVAR_GET(gpu_record_coalesce_scratch) &&
        !((decode_register_file_->values[XE_GPU_REG_SCRATCH_UMSK] >> scratch) & 1)) {
      // No write back: only the register holds it, and nothing on the
      // recorder reads it before the pending value is recorded.
      decode_register_file_->values[index] = value;
      decode_pending_scratch_ |= UINT32_C(1) << scratch;
      return;
    }
    FlushPendingScratchWrites();
    decode_scratch_recorded_.store(decode_scratch_recorded_.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_relaxed);
  } else if (index == XE_GPU_REG_SCRATCH_UMSK || index == XE_GPU_REG_SCRATCH_ADDR) {
    FlushPendingScratchWrites();
  }
  if (elide_unchanged_registers_ && index < RegisterFile::kRegisterCount &&
      decode_register_file_->values[index] == value && !RegisterWriteAlwaysMatters(index)) {
    // The recorder's register file already holds this value.
    return;
  }
  if (index < RegisterFile::kRegisterCount) {
    uint32_t shadow_value = value;
    if (index == XE_GPU_REG_COHER_STATUS_HOST) {
      // As CommandProcessor::WriteRegister: dirty until WAIT_REG_MEM syncs.
      shadow_value |= UINT32_C(0x80000000);
    }
    decode_register_file_->values[index] = shadow_value;
  } else {
    decode_extended_register_values_.insert_or_assign(index, value);
  }
  RecordWords& words = record_batch_->words;
  words.push_back(kRecordOne);
  words.push_back(index);
  words.push_back(value);
}

void CommandProcessor::PacketWriteRegistersFromMem(uint32_t start_index, const uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (replay_probe.capturing) {
    ReplayProbeTouch(start_index, num_registers);
  }
  if (!record_split_) {
    WriteRegistersFromMem(start_index, const_cast<uint32_t*>(base), num_registers);
    return;
  }
  if (decode_pending_scratch_ && start_index <= XE_GPU_REG_SCRATCH_REG7 &&
      uint64_t(start_index) + num_registers > XE_GPU_REG_SCRATCH_UMSK) {
    FlushPendingScratchWrites();
  }
  if (uint64_t(start_index) + num_registers > RegisterFile::kRegisterCount) {
    // Out of the register file: one at a time, as the unsplit path does.
    for (uint32_t i = 0; i < num_registers; ++i) {
      PacketWriteRegister(start_index + i, memory::load_and_swap<uint32_t>(base + i));
    }
    return;
  }
  if (elide_unchanged_registers_) {
    // Record only the sub-runs that change a register (or whose write always
    // matters); the rest would leave the recorder's register file as it is.
    uint32_t* shadow = decode_register_file_->values + start_index;
    uint32_t i = 0;
    decode_source_runs_.store(decode_source_runs_.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
    if (REXCVAR_GET(gpu_decode_compare_runs_wide) &&
        !RunHasRegisterWriteAlwaysMatters(start_index, start_index + num_registers)) {
      // The same sub-runs as below, found four registers at a time.
      i = FindRegisterChange<true>(shadow, base, 0, num_registers);
      while (i < num_registers) {
        const uint32_t end = FindRegisterChange<false>(shadow, base, i + 1, num_registers);
        memory::copy_and_swap(shadow + i, base + i, end - i);
        decode_sub_runs_.store(decode_sub_runs_.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
        RecordRegisterRun(start_index + i, base + i, end - i);
        i = FindRegisterChange<true>(shadow, base, end, num_registers);
      }
      return;
    }
    while (i < num_registers) {
      if (shadow[i] == rex::byte_swap(base[i]) && !RegisterWriteAlwaysMatters(start_index + i)) {
        ++i;
        continue;
      }
      uint32_t end = i;
      do {
        shadow[end] = rex::byte_swap(base[end]);
        ++end;
      } while (end < num_registers &&
               (shadow[end] != rex::byte_swap(base[end]) ||
                RegisterWriteAlwaysMatters(start_index + end)));
      if (start_index + i <= XE_GPU_REG_COHER_STATUS_HOST &&
          start_index + end > XE_GPU_REG_COHER_STATUS_HOST) {
        decode_register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] |= UINT32_C(0x80000000);
      }
      decode_sub_runs_.store(decode_sub_runs_.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
      RecordRegisterRun(start_index + i, base + i, end - i);
      i = end;
    }
    return;
  }
  memory::copy_and_swap(decode_register_file_->values + start_index, base, num_registers);
  if (start_index <= XE_GPU_REG_COHER_STATUS_HOST &&
      start_index + num_registers > XE_GPU_REG_COHER_STATUS_HOST) {
    decode_register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] |= UINT32_C(0x80000000);
  }
  RecordRegisterRun(start_index, base, num_registers);
}

void CommandProcessor::RecordRegisterRun(uint32_t start_index, const uint32_t* base,
                                         uint32_t num_registers) {
  RecordWords& words = record_batch_->words;
  // Float constants arrive in many small runs (about 12 a draw of 3 to 4
  // registers), and each run costs the recorder a dispatch and its checks:
  // one following the last recorded run within a short gap extends it, the
  // gap holding the registers' current values (rewriting a float constant
  // with its own value only stores it again).
  constexpr uint32_t kConstantRunMergeGap = 16;
  if (REXCVAR_GET(gpu_record_merge_constant_runs) &&
      start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      start_index + num_registers - 1 <= XE_GPU_REG_SHADER_CONSTANT_511_W &&
      last_constant_run_batch_ == record_batch_.get() &&
      last_constant_run_end_ == words.size()) {
    const uint32_t previous_start = words[last_constant_run_offset_ + 1];
    const uint32_t previous_count = words[last_constant_run_offset_ + 2];
    const uint32_t previous_end = previous_start + previous_count;
    if (start_index >= previous_end && start_index - previous_end <= kConstantRunMergeGap) {
      const uint32_t gap = start_index - previous_end;
      const size_t offset = words.size();
      words.resize(offset + gap + num_registers);
      std::memcpy(words.data() + offset, decode_register_file_->values + previous_end,
                  sizeof(uint32_t) * gap);
      memory::copy_and_swap(words.data() + offset + gap, base, num_registers);
      words[last_constant_run_offset_ + 2] = previous_count + gap + num_registers;
      last_constant_run_end_ = words.size();
      if (words.size() >= 65536) {
        PublishRecordBatch();
      }
      return;
    }
  }
  const size_t offset = words.size();
  words.resize(offset + 3 + num_registers);
  words[offset] = kRecordRun;
  words[offset + 1] = start_index;
  words[offset + 2] = num_registers;
  // In host byte order: the recorder stores them without swapping.
  memory::copy_and_swap(words.data() + offset + 3, base, num_registers);
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      start_index + num_registers - 1 <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    last_constant_run_batch_ = record_batch_.get();
    last_constant_run_offset_ = offset;
    last_constant_run_end_ = words.size();
  }
  if (words.size() >= 65536) {
    PublishRecordBatch();
  }
}

void CommandProcessor::RecordCall(std::function<void()> fn) {
  if (!record_split_) {
    fn();
    return;
  }
  // A call may read any register.
  FlushPendingScratchWrites();
  RecordBatch& batch = *record_batch_;
  batch.words.push_back(kRecordCall);
  batch.words.push_back(uint32_t(batch.fns.size()));
  batch.fns.push_back(std::move(fn));
  // Small batches keep the recorder close behind the decoder.
  if (batch.fns.size() >= 32) {
    PublishRecordBatch();
  }
}

void CommandProcessor::PublishRecordBatch() {
  if (!record_split_) {
    return;
  }
  // The recorder's register file holds every scratch value by the end of a
  // batch, so RecordSync's readers see them.
  FlushPendingScratchWrites();
  if (record_batch_->words.empty()) {
    return;
  }
  last_constant_run_batch_ = nullptr;
  std::unique_ptr<RecordBatch> next;
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_queue_.push_back(std::move(record_batch_));
    ++record_published_;
    if (!record_free_.empty()) {
      next = std::move(record_free_.back());
      record_free_.pop_back();
    }
    wake = record_recorder_waiting_;
  }
  if (wake) {
    record_ready_.notify_one();
  }
  record_batch_ = next ? std::move(next) : std::make_unique<RecordBatch>();
  record_batch_draws_ = 0;
}

void CommandProcessor::RecordSync() {
  if (!record_split_) {
    return;
  }
  PublishRecordBatch();
  std::unique_lock<std::mutex> lock(record_mutex_);
  ++record_sync_waiters_;
  record_done_.wait(lock, [this]() { return record_completed_ == record_published_; });
  --record_sync_waiters_;
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // The decoding thread parks itself between guest commands until Resume.
  // With a recorder thread, it first lets the recorder finish what is
  // queued; the recorder then waits for batches that do not come.
  {
    std::lock_guard<std::mutex> lock(pause_mutex_);
    worker_paused_ = true;
  }
  thread::Fence fence;
  CallInWorkerThread([this, &fence]() {
    RecordSync();
    fence.Signal();
    WorkerWaitWhilePaused();
  });

  fence.Wait();
}

void CommandProcessor::WorkerWaitWhilePaused() {
  std::unique_lock<std::mutex> lock(pause_mutex_);
  for (;;) {
    pause_signal_.wait(lock, [this]() { return !worker_paused_ || reduce_memory_requested_; });
    if (!reduce_memory_requested_) {
      return;
    }
    reduce_memory_requested_ = false;
    lock.unlock();
    if (record_split_) {
      RecordCall([this]() { OnReduceMemory(); });
      RecordSync();
    } else {
      OnReduceMemory();
    }
    lock.lock();
  }
}

void CommandProcessor::ReduceMemoryWhilePaused() {
  // Does not wait: the caller is the UI thread, which must stay free to
  // deliver the return to the foreground.
  {
    std::lock_guard<std::mutex> lock(pause_mutex_);
    if (!worker_paused_) {
      return;
    }
    reduce_memory_requested_ = true;
  }
  pause_signal_.notify_all();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  {
    std::lock_guard<std::mutex> lock(pause_mutex_);
    worker_paused_ = false;
  }
  pause_signal_.notify_all();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() { D3dShaderCacheReset(); }

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  // Only the decoder reads registers through this (WAIT_REG_MEM, REG_TO_MEM,
  // COND_WRITE), so while split it reads the decoder's shadow.
  if (index < RegisterFile::kRegisterCount) {
    return (record_split_ ? decode_register_file_.get() : register_file_)->values[index];
  }
  const auto& extended =
      record_split_ ? decode_extended_register_values_ : extended_register_values_;
  auto it = extended.find(index);
  return it != extended.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  if (regs.values[index] != value && !IsPerDrawRegister(index) &&
      !(index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
    ++state_epoch_;
    state_hash_ ^= StateHashTerm(index, regs.values[index]) ^ StateHashTerm(index, value);
    render_state_epoch_ += IsRenderStateRegister(index);
  }
  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  if (!regs.IsKnownRegister(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
#if defined(REXGLUE_ENABLE_CS_PROBES)
      AwaitGuestGpuCompletionIfRequested();
#endif
      auto complete = [this, mem_addr, value]() {
        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
#if defined(REXGLUE_ENABLE_CS_PROBES)
        if (REXCVAR_GET(gpu_async_fence_probe)) system::SignalGpuWrite();
#endif
      };
#if defined(REXGLUE_ENABLE_CS_PROBES)
      PublishGuestCompletion(std::move(complete));
#else
      complete();
#endif
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

namespace {
// Whether writing the registers [first, last] needs more than storing the
// values: the side effects in CommandProcessor::WriteRegister, and the shader
// constants the backends' WriteRegister overrides track.
bool RegisterRangeHasWriteSideEffects(uint32_t first, uint32_t last) {
  auto overlaps = [first, last](uint32_t range_first, uint32_t range_last) {
    return first <= range_last && last >= range_first;
  };
  return overlaps(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_SCRATCH_REG7) ||
         overlaps(XE_GPU_REG_COHER_STATUS_HOST, XE_GPU_REG_COHER_STATUS_HOST) ||
         overlaps(XE_GPU_REG_DC_LUT_RW_INDEX, XE_GPU_REG_DC_LUT_30_COLOR) ||
         overlaps(XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
}
}  // namespace

void CommandProcessor::WriteRegistersHost(uint32_t start_index, const uint32_t* values,
                                          uint32_t num_registers) {
  std::vector<uint32_t>& swapped = write_registers_host_scratch_;
  swapped.resize(num_registers);
  memory::copy_and_swap(swapped.data(), values, num_registers);
  WriteRegistersFromMem(start_index, swapped.data(), num_registers);
}

void CommandProcessor::WriteRegistersHostBase(uint32_t start_index, const uint32_t* values,
                                              uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (uint64_t(start_index) + num_registers <= RegisterFile::kRegisterCount &&
      !RegisterRangeHasWriteSideEffects(start_index, start_index + num_registers - 1)) {
    uint32_t* registers = register_file_->values + start_index;
    bool changed = false, render_changed = false;
    for (uint32_t i = 0; i < num_registers; ++i) {
      const uint32_t value = values[i];
      if (registers[i] != value) {
        const uint32_t index = start_index + i;
        if (!IsPerDrawRegister(index)) {
          changed = true;
          render_changed |= IsRenderStateRegister(index);
          state_hash_ ^= StateHashTerm(index, registers[i]) ^ StateHashTerm(index, value);
        }
        registers[i] = value;
      }
    }
    if (changed) {
      ++state_epoch_;
      render_state_epoch_ += render_changed;
    }
    return;
  }
  for (uint32_t i = 0; i < num_registers; ++i) {
    WriteRegister(start_index + i, values[i]);
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  // Most type-0 runs are plain state registers: store them in one swap-copy
  // instead of a virtual WriteRegister per register.
  if (uint64_t(start_index) + num_registers <= RegisterFile::kRegisterCount &&
      !RegisterRangeHasWriteSideEffects(start_index, start_index + num_registers - 1)) {
    uint32_t* values = register_file_->values + start_index;
    bool changed = false, render_changed = false;
    for (uint32_t i = 0; i < num_registers; ++i) {
      const uint32_t value = rex::byte_swap(base[i]);
      if (values[i] != value) {
        const uint32_t index = start_index + i;
        if (!IsPerDrawRegister(index)) {
          changed = true;
          render_changed |= IsRenderStateRegister(index);
          state_hash_ ^= StateHashTerm(index, values[i]) ^ StateHashTerm(index, value);
        }
        values[i] = value;
      }
    }
    if (changed) {
      ++state_epoch_;
      render_state_epoch_ += render_changed;
    }
    return;
  }
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    PacketWriteRegistersFromMem(base, reinterpret_cast<const uint32_t*>(range.first), first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    PacketWriteRegistersFromMem(base, reinterpret_cast<const uint32_t*>(range.second),
                                uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = decode_regs().values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {}

#if defined(REXGLUE_ENABLE_CS_PROBES)
void CommandProcessor::AwaitGuestGpuCompletionIfRequested() {
  if (REXCVAR_GET(gpu_true_fence_completion) && !AwaitGuestGpuCompletion()) {
    rex::FatalError("CS-0.4 could not await host GPU completion; refusing an early guest signal");
  }
}

void CommandProcessor::PublishGuestCompletion(std::function<void()> completion) {
  if (REXCVAR_GET(gpu_async_fence_completion))
    rex::FatalError("CS-0.4 async completion is only implemented for Vulkan");
  completion();
}

#endif
void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  if (record_split_) {
    RecordCall([this]() {
      FlushCpuVisibleResults();
      OnPrimaryBufferEnd();
    });
    PublishRecordBatch();
  } else {
    FlushCpuVisibleResults();
    OnPrimaryBufferEnd();
  }

  return write_index;
}

namespace {
// gpu_ib_identity_stats (decoder thread only).
struct IbIdentityStats {
  std::unordered_map<uint64_t, uint64_t> last_hash;
  uint64_t decoded_draws = 0;
  uint64_t frames = 0, ibs = 0, repeats = 0, dwords = 0, repeat_dwords = 0, draws = 0,
           repeat_draws = 0;
};
IbIdentityStats ib_stats;

}  // namespace

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");
  ++trace_ib_depth_;
  struct DepthGuard {
    uint32_t& depth;
    ~DepthGuard() { --depth; }
  } depth_guard{trace_ib_depth_};
  // DL-0.1 D3D source census (one flag check while it is off).
  d3d_census_decoder::IbBegin(ptr, count, trace_ib_depth_);
  d3d_skip_visible_ =
      d3d_census_decoder::Active() ? REXCVAR_GET(gpu_d3d_census_skip_visible) : 0;
  struct CensusGuard {
    uint32_t depth;
    ~CensusGuard() {
      if (d3d_census_decoder::Active()) d3d_census_decoder::IbEnd(depth);
    }
  } census_guard{trace_ib_depth_};
  // NA-2.4: a pilot list's draws run from the backend's program for it.
  struct ProgramGuard {
    CommandProcessor* processor = nullptr;
    ~ProgramGuard() {
      if (!processor) return;
      processor->RecordCall([p = processor]() { p->DrawProgramEnd(); });
      processor->d3d_program_depth_ = 0;
    }
  } program_guard;
  if (d3d_program_mode_ && trace_ib_depth_ >= 2 && !d3d_program_depth_ &&
      system::d3d_program::IsList(ptr)) {
    d3d_program_depth_ = trace_ib_depth_;
    program_guard.processor = this;
    const uint64_t key = uint64_t(ptr & 0x1FFFFFFFu) << 32 | count;
    RecordCall([this, key]() { DrawProgramBegin(key); });
  }

  for (uint32_t level = 0; level < replay_probe.active; ++level) {
    replay_probe.levels[level].nested.emplace_back(ptr, count);
  }
  if (const int32_t trace_writers = REXCVAR_GET(gpu_trace_ib_writers)) {
    static void* const callback =
        memory_->RegisterPhysicalMemoryAccessCallback(LogIbWrite, nullptr);
    (void)callback;
    ib_writer_trace.swaps.store(trace_swaps_, std::memory_order_relaxed);
    if (ib_writer_trace.rearm_pending.load(std::memory_order_acquire)) {
      std::vector<uint32_t> pages;
      {
        std::lock_guard<std::mutex> lock(ib_writer_trace.mutex);
        pages.swap(ib_writer_trace.rearm_pages);
        ib_writer_trace.rearm_pending.store(false, std::memory_order_relaxed);
      }
      for (uint32_t page : pages) {
        memory_->EnablePhysicalMemoryAccessCallbacks(page << 12, 4096, false, false, true);
      }
    }
    const bool list = trace_ib_depth_ >= 2;
    const uint32_t physical = ptr & 0x1FFFFFFF;
    const uint32_t bytes = count * uint32_t(sizeof(uint32_t));
    const uint64_t buffer = (uint64_t(physical) << 32) | bytes | (list ? 0x80000000u : 0u);
    bool watch = false;
    {
      std::lock_guard<std::mutex> lock(ib_writer_trace.mutex);
      auto [it, inserted] = ib_writer_trace.watched.try_emplace(buffer, false);
      if (inserted) {
        uint32_t& watched = list ? ib_writer_trace.watched_lists : ib_writer_trace.watched_outer;
        if (watched < uint32_t(trace_writers)) {
          ++watched;
          it->second = true;
          for (uint32_t page = physical >> 12; page <= (physical + bytes - 1) >> 12; ++page) {
            ib_writer_trace.pages[page] = buffer;
          }
        }
      }
      watch = it->second;
    }
    if (watch && bytes) {
      memory_->EnablePhysicalMemoryAccessCallbacks(physical, bytes, false, false, true);
    }
  }
  const int32_t probe_mode = !record_split_ ? REXCVAR_GET(gpu_replay_floor_probe) : 0;
  ReplayProbeIb* probe_ib = nullptr;
  bool probe_repeat = false, probe_outer_repeat = false;
  int32_t probe_level = -1;
  if (probe_mode) {
    const int64_t hash_start_ns = ReplayProbeNowNs();
    const uint64_t key = ((uint64_t(ptr) << 32) | count) ^
                         (bin_select_ * UINT64_C(0x9E3779B97F4A7C15)) ^
                         std::rotl(bin_mask_ * UINT64_C(0xC2B2AE3D27D4EB4F), 29) ^
                         uint64_t(untile_select_all_);
    probe_ib = &replay_probe.ibs[key];
    const uint64_t hash = ReplayProbeHash(memory_, ptr, count, probe_ib->nested) | 1;
    probe_repeat = probe_ib->hash == hash;
    probe_ib->hash = hash;
    const bool capture_valid = probe_ib->capture_hash == hash &&
                               probe_ib->entry_bin_select == bin_select_ &&
                               probe_ib->entry_bin_mask == bin_mask_ &&
                               probe_ib->entry_select_all == untile_select_all_;
    PERF_counter_add(kReplayProbeHashNs, ReplayProbeNowNs() - hash_start_ns);
    PERF_counter_inc(kReplayProbeIbs);
    if (probe_repeat) {
      PERF_counter_inc(kReplayProbeRepeatIbs);
    }
    probe_outer_repeat = probe_repeat && !replay_probe.repeat_depth;
    if (probe_repeat && capture_valid && probe_ib->skippable && probe_mode >= 2) {
      // The floor: what is left without decoding or recording the buffer.
      const int64_t apply_start_ns = ReplayProbeNowNs();
      for (const auto& [index, value] : probe_ib->exit_registers) {
        WriteRegister(index, value);
        if (replay_probe.capturing) {
          ReplayProbeTouch(index, 1);
        }
      }
      bin_select_ = probe_ib->exit_bin_select;
      bin_mask_ = probe_ib->exit_bin_mask;
      active_vertex_shader_ = probe_ib->exit_vertex_shader;
      active_pixel_shader_ = probe_ib->exit_pixel_shader;
      replay_probe.replayed_draws += probe_ib->drawn_draws;
      PERF_counter_add(kReplayProbeApplyNs, ReplayProbeNowNs() - apply_start_ns);
      PERF_counter_inc(kReplayProbeSkippedIbs);
      PERF_counter_add(kReplayProbeSkippedDrawnDraws, probe_ib->drawn_draws);
      PERF_counter_add(kReplayProbeExitRegisters, probe_ib->exit_registers.size());
      if (probe_outer_repeat) {
        PERF_counter_add(kReplayProbeDrawnInRepeatIbs, probe_ib->drawn_draws);
        PERF_counter_add(kReplayProbeDrawnInSkippableRepeatIbs, probe_ib->drawn_draws);
      }
      if (trace_ib_depth_ == 1) {
        PERF_counter_add(kReplayProbeDrawnInIbs, probe_ib->drawn_draws);
      }
      return;
    }
    if (probe_repeat && !capture_valid) {
      // Repeated without a capture to replay: capture this execution.
      probe_ib->entry_bin_select = bin_select_;
      probe_ib->entry_bin_mask = bin_mask_;
      probe_ib->entry_select_all = untile_select_all_;
      probe_level = int32_t(ReplayProbeBegin());
    }
    if (probe_repeat) {
      ++replay_probe.repeat_depth;
    }
  }
  const uint64_t probe_replayed_before = replay_probe.replayed_draws;
  const uint64_t probe_decoded_before = ib_stats.decoded_draws;
  const uint64_t probe_untile_before = untile_skipped_draws_;

  const bool ib_stats_enabled = REXCVAR_GET(gpu_ib_identity_stats);
  const bool template_stats = record_split_ && REXCVAR_GET(gpu_template_stats);
  bool ib_repeat = false;
  const uint64_t draws_before = ib_stats.decoded_draws;
  if (ib_stats_enabled || template_stats) {
    const uint64_t hash =
        XXH3_64bits(memory_->TranslatePhysical(ptr), size_t(count) * sizeof(uint32_t));
    const uint64_t key = (uint64_t(ptr) << 32) | count;
    uint64_t& last = ib_stats.last_hash[key];
    ib_repeat = last == hash;
    last = hash;
    if (ib_stats_enabled) {
      ++ib_stats.ibs;
      ib_stats.dwords += count;
      if (ib_repeat) {
        ++ib_stats.repeats;
        ib_stats.repeat_dwords += count;
      }
    }
    if (template_stats) {
      RecordCall([this, key, ib_repeat]() { TemplateStatsIbBegin(key, ib_repeat); });
    }
  }

  // DL-2.1: nested buffers (display lists) from their compiled form. The
  // mode is 0 whenever the census, the packet recorder or bin tracing is on.
  if (d3d_list_mode_ && trace_ib_depth_ >= 2 && !d3d_draw_open_ && !probe_mode &&
      !ib_stats_enabled && !template_stats && D3dListExecute(ptr, count)) {
    return;
  }

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
  if (d3d_list_verify_depth_ == trace_ib_depth_) D3dListVerifyEnd();

  if (template_stats) {
    RecordCall([this]() { TemplateStatsIbEnd(); });
  }
  if (probe_ib) {
    ReplayProbeIb& ib = *probe_ib;
    const uint32_t drawn = uint32_t((ib_stats.decoded_draws - probe_decoded_before) -
                                    (untile_skipped_draws_ - probe_untile_before) +
                                    (replay_probe.replayed_draws - probe_replayed_before));
    if (probe_level >= 0) {
      ReplayProbeCapture& capture = replay_probe.levels[probe_level];
      ib.skippable = capture.skippable;
      ib.drawn_draws = drawn;
      ib.nested.swap(capture.nested);
      ib.exit_registers.clear();
      ib.exit_registers.reserve(capture.touched.size());
      for (uint32_t index : capture.touched) {
        ib.exit_registers.emplace_back(index, register_file_->values[index]);
      }
      ib.exit_bin_select = bin_select_;
      ib.exit_bin_mask = bin_mask_;
      ib.exit_vertex_shader = active_vertex_shader_;
      ib.exit_pixel_shader = active_pixel_shader_;
      const int64_t hash_start_ns = ReplayProbeNowNs();
      ib.hash = ib.capture_hash = ReplayProbeHash(memory_, ptr, count, ib.nested) | 1;
      PERF_counter_add(kReplayProbeHashNs, ReplayProbeNowNs() - hash_start_ns);
      replay_probe.active = uint32_t(probe_level);
      replay_probe.capturing = replay_probe.active != 0;
    }
    if (probe_repeat) {
      --replay_probe.repeat_depth;
      if (probe_outer_repeat) {
        PERF_counter_add(kReplayProbeDrawnInRepeatIbs, drawn);
        if (ib.capture_hash == ib.hash && ib.skippable) {
          PERF_counter_add(kReplayProbeDrawnInSkippableRepeatIbs, drawn);
        }
      }
    }
    if (trace_ib_depth_ == 1) {
      PERF_counter_add(kReplayProbeDrawnInIbs, drawn);
    }
  }
  if (ib_stats_enabled) {
    // Nested buffers count in both.
    const uint64_t draws = ib_stats.decoded_draws - draws_before;
    ib_stats.draws += draws;
    if (ib_repeat) ib_stats.repeat_draws += draws;
  }
}

void CommandProcessor::TemplateStatsIbBegin(uint64_t ib_key, bool repeat) {
  // A buffer executed more than once a frame (per pass, per view) compares
  // with its previous execution in the same order.
  const uint32_t occurrence = template_stats_occurrences_[ib_key]++;
  TemplateStatsIb& ib = template_stats_stack_.emplace_back();
  ib.key = ib_key ^ (uint64_t(occurrence) * UINT64_C(0x9E3779B97F4A7C15));
  ib.repeat = repeat;
}

void CommandProcessor::TemplateStatsIbEnd() {
  if (template_stats_stack_.empty()) {
    return;
  }
  TemplateStatsIb ib = std::move(template_stats_stack_.back());
  template_stats_stack_.pop_back();
  TemplateStatsCounts& counts = template_stats_;
  const size_t draws = ib.signatures.size();
  counts.draws_in_ibs += draws;
  ++counts.ibs;
  if (ib.repeat) {
    counts.draws_in_repeated_ibs += draws;
  }
  auto& last = template_stats_last_[ib.key];
  size_t matching = 0, run = 0, in_runs = 0, replayable = 0;
  for (size_t i = 0; i < draws; ++i) {
    if (i < last.size()) {
      for (size_t part = 0; part < kTemplateParts; ++part) {
        counts.parts_matching[part] += last[i][part] == ib.signatures[i][part];
      }
    }
    if (i < last.size() && std::equal(last[i].begin(), last[i].begin() + kTemplateGuestParts,
                                      ib.signatures[i].begin())) {
      ++counts.draws_matching_with_constants;
    }
    const auto& signature = ib.signatures[i];
    counts.draws_transferring += signature[7] != 0;
    counts.draws_uploading += signature[8] != 0;
    const bool state_matches = i < last.size() && std::equal(last[i].begin(), last[i].begin() + 5,
                                                             signature.begin());
    if (state_matches && last[i][6] == signature[6] && !signature[7] && !last[i][7]) {
      ++replayable;
    }
    if (state_matches) {
      ++matching;
      ++run;
    } else {
      // Runs of 8 or more draws are what a replay would take whole.
      if (run >= 8) in_runs += run;
      run = 0;
    }
  }
  if (run >= 8) in_runs += run;
  counts.draws_matching += matching;
  counts.draws_in_matching_runs += in_runs;
  if (draws && matching == draws && last.size() == draws) {
    ++counts.matching_ibs;
    counts.draws_in_matching_ibs += draws;
  }
  counts.draws_replayable += replayable;
  if (draws && replayable == draws && last.size() == draws) {
    ++counts.replayable_ibs;
    counts.draws_in_replayable_ibs += draws;
  }
  last = std::move(ib.signatures);
}

void CommandProcessor::TemplateStatsDrawHost(uint64_t bound_views_hash, bool transferred,
                                             bool uploaded) {
  if (template_stats_stack_.empty() || template_stats_stack_.back().signatures.empty()) {
    return;
  }
  auto& signature = template_stats_stack_.back().signatures.back();
  signature[6] = bound_views_hash;
  signature[7] = transferred;
  signature[8] = uploaded;
}

void CommandProcessor::TemplateStatsDraw(const uint64_t (&parts)[kTemplateGuestParts]) {
  ++template_stats_.draws;
  if (!template_stats_stack_.empty() && template_stats_stack_.back().signatures.empty() &&
      (template_stats_stack_.back().key * UINT64_C(0x9E3779B97F4A7C15)) >> 58 == 0) {
    std::vector<uint32_t>& last = template_stats_registers_[template_stats_stack_.back().key];
    const uint32_t* now = register_file_->values;
    if (last.size() == XE_GPU_REG_SHADER_CONSTANT_000_X) {
      for (uint32_t i = 0; i < XE_GPU_REG_SHADER_CONSTANT_000_X; ++i) {
        if (last[i] != now[i] && !IsPerDrawRegister(i)) ++template_stats_register_diffs_[i];
      }
    }
    last.assign(now, now + XE_GPU_REG_SHADER_CONSTANT_000_X);
  }
  if (!template_stats_stack_.empty()) {
    auto& signature = template_stats_stack_.back().signatures.emplace_back();
    signature.fill(0);
    std::copy(std::begin(parts), std::end(parts), signature.begin());
  }
}

void CommandProcessor::TemplateStatsFrame() {
  template_stats_occurrences_.clear();
  TemplateStatsCounts& c = template_stats_;
  if (++c.frames < 600) {
    return;
  }
  const double f = double(c.frames);
  auto percent = [&](uint64_t part) { return 100.0 * double(part) / double(std::max<uint64_t>(c.draws, 1)); };
  REXGPU_INFO(
      "Draw templates per frame over 600 frames: {:.0f} draws, {:.0f} in indirect buffers "
      "({:.1f} %), {:.0f} in byte-identical buffers ({:.1f} %), {:.0f} matching their previous "
      "execution's signature ({:.1f} %), {:.0f} in matching runs of 8+ ({:.1f} %), {:.0f} in "
      "wholly matching buffers ({:.1f} %, {:.1f} of {:.1f} buffers)",
      c.draws / f, c.draws_in_ibs / f, percent(c.draws_in_ibs), c.draws_in_repeated_ibs / f,
      percent(c.draws_in_repeated_ibs), c.draws_matching / f, percent(c.draws_matching),
      c.draws_in_matching_runs / f, percent(c.draws_in_matching_runs),
      c.draws_in_matching_ibs / f, percent(c.draws_in_matching_ibs), c.matching_ibs / f,
      c.ibs / f);
  REXGPU_INFO(
      "Draw templates: parts matching at the same position: state {:.1f} %, shaders and packet "
      "{:.1f} %, index buffer {:.1f} %, texture fetch {:.1f} %, vertex fetch {:.1f} %, "
      "constants {:.1f} %; whole signature with constants {:.1f} %",
      percent(c.parts_matching[0]), percent(c.parts_matching[1]), percent(c.parts_matching[2]),
      percent(c.parts_matching[3]), percent(c.parts_matching[4]), percent(c.parts_matching[5]),
      percent(c.draws_matching_with_constants));
  REXGPU_INFO(
      "Draw templates (LS-3.3): {:.1f} % of draws needed an EDRAM transfer and {:.1f} % a shared "
      "memory upload; host image views and samplers matching at matching positions {:.1f} %; "
      "{:.0f} draws a frame ({:.1f} %) replayable as recorded commands with fresh constants, "
      "{:.0f} ({:.1f} %) in wholly replayable buffers ({:.1f} buffers)",
      percent(c.draws_transferring), percent(c.draws_uploading), percent(c.parts_matching[6]),
      c.draws_replayable / f, percent(c.draws_replayable), c.draws_in_replayable_ibs / f,
      percent(c.draws_in_replayable_ibs), c.replayable_ibs / f);
  std::vector<std::pair<uint64_t, uint32_t>> diffs;
  for (const auto& [index, count] : template_stats_register_diffs_) diffs.emplace_back(count, index);
  std::sort(diffs.rbegin(), diffs.rend());
  std::string top;
  for (size_t i = 0; i < diffs.size() && i < 16; ++i) {
    top += fmt::format(" {:04X}x{}", diffs[i].second, diffs[i].first);
  }
  REXGPU_INFO("Draw templates: registers differing at sampled buffers' first draws:{}", top);
  template_stats_register_diffs_.clear();
  c = TemplateStatsCounts();
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecuteHostPackets(const uint32_t* dwords, uint32_t count) {
  if (!count) return true;
  memory::RingBuffer reader(reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(dwords)),
                            count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) return false;
  } while (reader.read_count());
  return true;
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  if (cs_scheduler::Enabled()) cs_scheduler::Call(reader->read_offset());
  const uint32_t packet_offset = reader->read_offset();
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
#if defined(REXGLUE_ENABLE_CS_PROBES)
  if (session_probe::Enabled() || (cs_scheduler::Enabled() && cs_scheduler::Mode() != 3)) {
    uint32_t words = 1;
    if (packet && (packet_type == 0 || packet_type == 3)) words += ((packet >> 16) & 0x3FFF) + 1;
    else if (packet_type == 1) words += 2;
    memory::RingBuffer copy = *reader;
    copy.set_read_offset(packet_offset);
    auto range = copy.BeginRead(size_t(words) * 4);
    if (range.first_length + range.second_length != size_t(words) * 4)
      rex::FatalError("CS-0.1 cannot hash a truncated PM4 packet");
    session_pm4_hash_ = session_probe::Digest(range.first, range.first_length, session_pm4_hash_);
    session_pm4_hash_ = session_probe::Digest(range.second, range.second_length, session_pm4_hash_);
    session_pm4_words_ += words;
    if (packet_type == 3 && ((packet >> 8) & 0x7F) == PM4_XE_SWAP) {
      session_probe::Scalar(session_probe::Kind::kPm4Frame, session_pm4_frame_, session_pm4_hash_);
      session_probe::Scalar(session_probe::Kind::kPm4Words, session_pm4_frame_++, session_pm4_words_);
      session_probe::Flush();
      session_pm4_hash_ = UINT64_C(14695981039346656037);
      session_pm4_words_ = 0;
    }
  }
#endif
  if (packet == 0) {
    return true;
  }
  if (d3d_census_decoder::Active()) {
    d3d_census_decoder::Packet(packet_offset, packet);
  }
  if (packet_recorder_) {
    // The recorder adds the guest ranges its draws read to the same recording:
    // let it finish the packets before this one, so the two never overlap and
    // the ranges are taken in the order they are without the split.
    RecordSync();
    uint32_t dword_count = 1;
    switch (packet_type) {
      case 0x00:
      case 0x03:
        dword_count += ((packet >> 16) & 0x3FFF) + 1;
        break;
      case 0x01:
        dword_count += 2;
        break;
      default:
        break;
    }
    const uint32_t opcode = (packet >> 8) & 0x7F;
    // Indirect buffers are recorded as the packets they contain.
    if (packet_type != 0x03 ||
        (opcode != PM4_INDIRECT_BUFFER && opcode != PM4_INDIRECT_BUFFER_PFD)) {
      std::vector<uint32_t> dwords(dword_count);
      memory::RingBuffer copy = *reader;
      copy.set_read_offset(packet_offset);
      if (copy.Read(reinterpret_cast<uint8_t*>(dwords.data()), dword_count * sizeof(uint32_t)) ==
          dword_count * sizeof(uint32_t)) {
        packet_recorder_(dwords.data(), dword_count);
      }
    }
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  bool result = false;
  switch (packet_type) {
    case 0x00:
      result = ExecutePacketType0(reader, packet);
      break;
    case 0x01:
      result = ExecutePacketType1(reader, packet);
      break;
    case 0x02:
      result = ExecutePacketType2(reader, packet);
      break;
    case 0x03:
      result = ExecutePacketType3(reader, packet);
      break;
    default:
      assert_unhandled_case(packet_type);
      break;
  }
  return result;
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  if (d3d_draw_open_ && D3dDrawType0(reader, packet)) {
    return true;
  }
  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  if (d3d_skip_visible_ >= 2 && d3d_census_decoder::Active() && d3d_census_decoder::Visible() &&
      D3dCensusSkippableRange(base_index, write_one_reg ? 1 : count)) {
    // DL-1.1: state of a visible buffer.
    PERF_counter_inc(kD3dCensusSkippedConstantPackets);
    reader->AdvanceRead(count * sizeof(uint32_t));
    return true;
  }
  if (!write_one_reg) {
    WriteRegisterRangeFromRing(reader, base_index, count);
    return true;
  }
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    PacketWriteRegister(base_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  PacketWriteRegister(reg_index_1, reg_data_1);
  PacketWriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (opcode == PM4_XE_SWAP) {
    ++trace_swaps_;
    d3d_census_decoder::Swap();
    if (d3d_draw_) D3dDrawSwap();
    D3dListSwap();
    D3dShaderCacheSwap();
    d3d_program_mode_ =
        SupportsDrawPrograms() ? uint32_t(std::max(REXCVAR_GET(gpu_d3d_program), 0)) : 0;
    system::d3d_program::SetMode(d3d_program_mode_);
    system::d3d_program::Swap();
  }
  const int32_t trace_bins_vblank = REXCVAR_GET(gpu_trace_bins_vblank);
  const bool trace_bins = trace_bins_vblank >= 0 && trace_swaps_ == uint32_t(trace_bins_vblank);
  if (trace_bins &&
      (opcode == PM4_SET_BIN_MASK || opcode == PM4_SET_BIN_MASK_LO ||
       opcode == PM4_SET_BIN_MASK_HI || opcode == PM4_SET_BIN_SELECT_LO ||
       opcode == PM4_SET_BIN_SELECT_HI || opcode == PM4_EVENT_WRITE_EXT ||
       opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2 || opcode == PM4_INTERRUPT ||
       opcode == PM4_INDIRECT_BUFFER || opcode == PM4_INDIRECT_BUFFER_PFD)) {
    const uint32_t* words =
        reinterpret_cast<const uint32_t*>(reader->read_ptr());
    REXGPU_INFO("BINTRACE vb {} d {} op {:02X} pred {} sel {:016X} mask {:016X} woff {:08X} w0 {:08X} w1 {:08X}",
                counter_, trace_ib_depth_, opcode, packet & 1, bin_select_, bin_mask_,
                register_file_->values[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
                count > 0 ? rex::byte_swap(words[0]) : 0u, count > 1 ? rex::byte_swap(words[1]) : 0u);
  }
  if (d3d_draw_open_ && D3dDrawType3(reader, packet, opcode, count)) {
    return true;
  }
  if (d3d_list_verify_depth_ == trace_ib_depth_ &&
      (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2)) {
    D3dListVerifyDraw();
  }
  if ((packet & 1) && PredicatedOut(packet, opcode)) {
    reader->AdvanceRead(count * sizeof(uint32_t));
    return true;
  }

  if (replay_probe.capturing) {
    // A buffer is skippable only if it sets state, constants and shaders and
    // draws: anything that writes memory, signals, waits or reads back stays.
    switch (opcode) {
      case PM4_NOP:
      case PM4_INDIRECT_BUFFER:
      case PM4_INDIRECT_BUFFER_PFD:
      case PM4_DRAW_INDX:
      case PM4_DRAW_INDX_2:
      case PM4_SET_CONSTANT:
      case PM4_SET_CONSTANT2:
      case PM4_LOAD_ALU_CONSTANT:
      case PM4_SET_SHADER_CONSTANTS:
      case PM4_IM_LOAD:
      case PM4_IM_LOAD_IMMEDIATE:
      case PM4_SET_BIN_MASK:
      case PM4_SET_BIN_MASK_LO:
      case PM4_SET_BIN_MASK_HI:
      case PM4_SET_BIN_SELECT:
      case PM4_SET_BIN_SELECT_LO:
      case PM4_SET_BIN_SELECT_HI:
        break;
      default:
        ReplayProbeMarkUnskippable();
        break;
    }
  }

  if (d3d_skip_visible_ >= 2 && d3d_census_decoder::Active() && d3d_census_decoder::Visible() &&
      D3dCensusSkipConstants(reader, opcode, count)) {
    return true;
  }

  bool cpu_visible_write = false;
  switch (opcode) {
    case PM4_INTERRUPT:
    case PM4_XE_SWAP:
    case PM4_REG_TO_MEM:
    case PM4_MEM_WRITE:
    case PM4_COND_WRITE:
    case PM4_EVENT_WRITE_SHD:
    case PM4_EVENT_WRITE_EXT:
    case PM4_EVENT_WRITE_ZPD:
      if (!record_split_) {
        FlushCpuVisibleResults();
      } else if (opcode != PM4_EVENT_WRITE_EXT && opcode != PM4_XE_SWAP &&
                 opcode != PM4_EVENT_WRITE_SHD && opcode != PM4_INTERRUPT) {
        // The CPU may read what the draws before this wrote: let the
        // recorder finish them first. Screen extents are constant; the swap,
        // fence writes and interrupts are recorded in order like draws, so
        // the title sees a fence only once the recorder has consumed the
        // draws before it.
        RecordCall([this]() { FlushCpuVisibleResults(); });
        RecordSync();
      }
      cpu_visible_write = true;
      break;
    default:
      break;
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
#if defined(REXGLUE_ENABLE_CS_PROBES)
      if (REXCVAR_GET(gpu_true_fence_completion)) {
        RecordCall([this]() { AwaitGuestGpuCompletionIfRequested(); });
        PublishRecordBatch();
      }
#endif
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  // Wakes guest threads blocked on a fence word instead of polling it.
  if (cpu_visible_write) system::SignalGpuWrite();
  return result;
}

bool CommandProcessor::PredicatedOut(uint32_t packet, uint32_t opcode) const {
  if (!(packet & 1)) return false;
  // Untiled, the first tile also runs the later tiles' objects; the packets
  // predicated on a whole later tile set that tile's window offset,
  // scissors and resolve destination, and stay with their tile.
  const bool tile_state = (bin_mask_ & 0xFFFFFFFFu) == bin_mask_ &&
                          (bin_mask_ & ~uint64_t(3)) &&
                          (bin_mask_ == (uint64_t(3) << rex::tzcnt(uint32_t(bin_mask_)))) &&
                          !(rex::tzcnt(uint32_t(bin_mask_)) & 1);
  const uint64_t select = untile_select_all_ && TilingPassOfSelect() == 0 && !tile_state
                              ? ~uint64_t(0)
                              : bin_select_;
  const bool any_pass = (select & bin_mask_) != 0;
  return !any_pass || opcode == PM4_XE_SWAP;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  if (count == 3) {
    // DL-1.2: a D3D-level draw's marker (rex/system/d3d_draw.h).
    const uint32_t magic = reader->ReadAndSwap<uint32_t>();
    const uint32_t id = reader->ReadAndSwap<uint32_t>();
    const uint32_t mode = reader->ReadAndSwap<uint32_t>();
    if (magic == system::d3d_draw::kMarkerMagic) {
      D3dDrawMarker(id, mode);
    } else if (magic == system::d3d_draw::kReplaceMagic) {
      // DL-2.1: a replaced draw call (arena offset, (id << 2) | mode).
      D3dSliceMarker(id, mode);
    }
    return true;
  }
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  RecordCall([this, cpu_mask, frame = counter_]() {
    if (session_probe::Enabled()) session_probe::Scalar(session_probe::Kind::kGpuInterrupt, 0, cpu_mask);
#if defined(REXGLUE_ENABLE_CS_PROBES)
    AwaitGuestGpuCompletionIfRequested();
#endif
    if (record_split_) {
      FlushCpuVisibleResults();
    }
    auto complete = [this, cpu_mask]() {
      for (int n = 0; n < 6; n++) {
        if (cpu_mask & (1 << n)) {
          if (graphics_system_) {
            graphics_system_->DispatchInterruptCallback(1, n);
          }
        }
      }
    };
#if defined(REXGLUE_ENABLE_CS_PROBES)
    PublishGuestCompletion(std::move(complete));
#else
    complete();
#endif
  });
  PublishRecordBatch();
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  if (REXCVAR_GET(gpu_ib_identity_stats) && ++ib_stats.frames == 600) {
    const double f = double(ib_stats.frames);
    REXGPU_INFO(
        "Indirect buffers per frame over 600 frames: {:.1f} executed, {:.1f} repeated byte for "
        "byte ({:.1f} %); dwords {:.0f}, repeated {:.0f} ({:.1f} %); draws inside {:.0f}, in "
        "repeated buffers {:.0f} ({:.1f} %); {} distinct buffers seen",
        ib_stats.ibs / f, ib_stats.repeats / f, 100.0 * ib_stats.repeats / std::max<uint64_t>(ib_stats.ibs, 1),
        ib_stats.dwords / f, ib_stats.repeat_dwords / f,
        100.0 * ib_stats.repeat_dwords / std::max<uint64_t>(ib_stats.dwords, 1), ib_stats.draws / f,
        ib_stats.repeat_draws / f, 100.0 * ib_stats.repeat_draws / std::max<uint64_t>(ib_stats.draws, 1),
        ib_stats.last_hash.size());
    ib_stats.frames = ib_stats.ibs = ib_stats.repeats = ib_stats.dwords = ib_stats.repeat_dwords =
        ib_stats.draws = ib_stats.repeat_draws = 0;
  }

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static uint64_t last_frame_tick = 0;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      int64_t dt_us = static_cast<int64_t>((now - last_frame_tick) * 1000000 / freq);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  RecordCall([this, frontbuffer_ptr, frontbuffer_width, frontbuffer_height]() {
    // FH1's VdSwap packet ordinal pairs with its ordered title source markers.
    rex::perf::TraceCriticalPath("consumed_swap", int64_t(observation_frame_sequence_),
                                 int64_t(frontbuffer_ptr));
    IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
    ++observation_frame_sequence_;
    debug_frame_draw_index_ = 0;
    if (REXCVAR_GET(gpu_template_stats)) {
      TemplateStatsFrame();
    }
  });
  PublishRecordBatch();
  // The recorder sets and clears a packet recorder at a swap: the decoder may
  // not decode further until it has.
  if (record_split_ && (packet_recorder_ || RecordSyncAfterSwap(decode_frame_sequence_))) {
    RecordSync();
  }
  ++decode_frame_sequence_;

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

namespace {
// gpu_trace_wait_reg_mem_writers: the guest writes to the armed pages; the
// waiting loop re-arms the page after each.
std::atomic<uint32_t> wait_writer_logs{0};
std::atomic<bool> wait_word_armed{false};
void LogWaitWordAccess(void*, uint32_t physical_address, uint32_t length, bool is_write) {
  wait_word_armed.store(false, std::memory_order_relaxed);
  // Each address once per 256 writes: frequent writers (the vblank callback)
  // would use up the log otherwise.
  static std::atomic<uint32_t> repeats[64];
  const uint32_t address = memory::CurrentAccessFaultVirtualAddress();
  if (!is_write || repeats[(address >> 2) & 63].fetch_add(1, std::memory_order_relaxed) % 256 ||
      wait_writer_logs.fetch_add(1, std::memory_order_relaxed) >= 2000) {
    return;
  }
  const auto* thread_state = runtime::ThreadState::Get();
  const PPCContext* context = thread_state ? thread_state->context() : nullptr;
  REXGPU_INFO(
      "WAIT_REG_MEM word page {:08X}+{:X} written at {:08X} by thread 0x{:X}, guest lr {:08X} "
      "r3 {:08X} r31 {:08X}",
      physical_address, length, memory::CurrentAccessFaultVirtualAddress(),
      rex::thread::current_thread_id(),
      context ? uint32_t(context->lr) : 0u, context ? context->r3.u32 : 0u,
      context ? context->r31.u32 : 0u);
}
}  // namespace

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;

  bool matched = false;
  bool slept = false;
  std::chrono::steady_clock::time_point wait_start{};
  uint32_t first_value = 0, last_value = 0;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    last_value = value;
    if (!matched) {
      if (wait_start == std::chrono::steady_clock::time_point{}) {
        wait_start = std::chrono::steady_clock::now();
        first_value = value;
        if (is_memory && REXCVAR_GET(gpu_trace_wait_reg_mem_writers)) {
          static std::atomic<uint32_t> wait_logs{0};
          if (wait_logs.fetch_add(1, std::memory_order_relaxed) < 200) {
            REXGPU_INFO("WAIT_REG_MEM waits on {:08X} (function {}, ref {:08X}, mask {:08X}): "
                        "now {:08X}",
                        poll_reg_addr, wait_info & 7, ref, mask, value);
          }
          static void* const callback =
              memory_->RegisterPhysicalMemoryAccessCallback(LogWaitWordAccess, nullptr);
          (void)callback;
          memory_->EnablePhysicalMemoryAccessCallbacks(poll_reg_addr & 0x1FFFFFFC, 4, false,
                                                       false, true);
        }
        // Let the recorder work through what was decoded while this waits.
        PublishRecordBatch();
      }
      if (is_memory && REXCVAR_GET(gpu_trace_wait_reg_mem_writers) &&
          !wait_word_armed.exchange(true, std::memory_order_relaxed)) {
        memory_->EnablePhysicalMemoryAccessCallbacks(poll_reg_addr & 0x1FFFFFFC, 4, false, false,
                                                     true);
      }
      if (cs_scheduler::InActor()) {
        cs_scheduler::Point(poll_reg_addr,cs_scheduler::PointKind::kWait,true,100000);
        continue;
      }
      // Wait.
      if (wait >= 0x100) {
        if (!record_split_) {
          PrepareForWait();
        }
        // A configured short sleep bounds polling even without VSync.
        // Android defaults to this to avoid burning a core while waiting.
        const WaitRegMemPolicy policy = CurrentWaitRegMemPolicy();
        if (!REXCVAR_GET(vsync) && policy.sleep_us == 0) {
          rex::thread::MaybeYield();
        } else if (std::chrono::steady_clock::now() - wait_start <
                   std::chrono::microseconds(policy.yield_us)) {
          rex::thread::MaybeYield();
        } else if (const int32_t sleep_us = policy.sleep_us) {
          // WaitUntil uses a high-resolution waitable timer on Windows, where
          // Sleep rounds anything under a millisecond down to a yield (LS-2.2).
          rex::thread::WaitUntil(std::chrono::steady_clock::now() +
                                 std::chrono::microseconds(sleep_us));
          slept = true;
        } else {
          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
          slept = true;
        }
        rex::thread::SyncMemory();
        if (!record_split_) {
          ReturnFromWait();
        }

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);
  if (wait_start != std::chrono::steady_clock::time_point{}) {
    const auto waited = std::chrono::steady_clock::now() - wait_start;
    PERF_counter_inc(kGpuRegMemWaitCount);
    if (slept) PERF_counter_inc(kGpuRegMemSleptCount);
    PERF_counter_add(kGpuThreadRegMemWaitNs,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
    if (const int32_t log_ms = REXCVAR_GET(gpu_log_long_wait_reg_mem_ms);
        log_ms > 0 && waited >= std::chrono::milliseconds(log_ms)) {
      REXGPU_WARN("WAIT_REG_MEM waited {:.1f} ms on {} {:08X} (function {}, ref {:08X}, mask "
                  "{:08X}): {:08X} -> {:08X}",
                  std::chrono::duration<double, std::milli>(waited).count(),
                  is_memory ? "memory" : "register", poll_reg_addr, wait_info & 7, ref, mask,
                  first_value, last_value);
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = decode_regs().values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= decode_regs().values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= decode_regs().values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  PacketWriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
    } else {
      // Register.
      PacketWriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  if (!record_split_) {
#if defined(REXGLUE_ENABLE_CS_PROBES)
    AwaitGuestGpuCompletionIfRequested();
#endif
    if (session_probe::Enabled()) session_probe::Scalar(session_probe::Kind::kGpuFence, address, data_value);
    auto complete = [this, address, data_value]() {
      memory::store(memory_->TranslatePhysical(address), data_value);
#if defined(REXGLUE_ENABLE_CS_PROBES)
      if (REXCVAR_GET(gpu_async_fence_probe)) system::SignalGpuWrite();
#endif
    };
#if defined(REXGLUE_ENABLE_CS_PROBES)
    PublishGuestCompletion(std::move(complete));
#else
    complete();
#endif
    return true;
  }
  RecordCall([this, address, data_value]() {
#if defined(REXGLUE_ENABLE_CS_PROBES)
    AwaitGuestGpuCompletionIfRequested();
#endif
    if (session_probe::Enabled()) session_probe::Scalar(session_probe::Kind::kGpuFence, address, data_value);
    FlushCpuVisibleResults();
    auto complete = [this, address, data_value]() {
      memory::store(memory_->TranslatePhysical(address), data_value);
      system::SignalGpuWrite();
    };
#if defined(REXGLUE_ENABLE_CS_PROBES)
    PublishGuestCompletion(std::move(complete));
#else
    complete();
#endif
  });
  PublishRecordBatch();
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries without host queries: the packet is sent on query
  // begin and end; report a fixed number of passed samples at the end, which
  // the title marks by writing the pending sentinel into the report.
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  auto* sample_counts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
      register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
  if (!sample_counts) {
    return true;
  }
  bool is_end = sample_counts->ZPass_A == kQueryFinished ||
                sample_counts->ZPass_B == kQueryFinished ||
                sample_counts->ZFail_A == kQueryFinished ||
                sample_counts->ZFail_B == kQueryFinished;
  std::memset(sample_counts, 0, sizeof(xe_gpu_depth_sample_counts));
  if (is_end) {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    sample_counts->ZPass_A = fake_sample_count;
    sample_counts->Total_A = fake_sample_count;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  // PD-1.1: on the recorded path the draw record carries the packet's
  // register writes (they only store on the recorder, and nothing is recorded
  // between them and the draw). The decoder's shadow takes them now; a packet
  // that ends without a draw record writes them as entries.
  const bool fuse_registers =
      record_split_ && REXCVAR_GET(gpu_record_fused_draw_registers) &&
      (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA ||
       vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
  uint32_t fused_registers = 0;
  uint32_t fused_dma_base = 0, fused_dma_size = 0;
  auto write_draw_register = [&](uint32_t index, uint32_t value) {
    if (fuse_registers) {
      decode_register_file_->values[index] = value;
      ++fused_registers;
    } else {
      PacketWriteRegister(index, value);
    }
  };
  auto unfuse_registers = [&]() {
    if (fused_registers >= 1) {
      PacketWriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);
    }
    if (fused_registers >= 2) {
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_BASE, fused_dma_base);
    }
    if (fused_registers >= 3) {
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_SIZE, fused_dma_size);
    }
  };
  write_draw_register(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        unfuse_registers();
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      fused_dma_base = vgt_dma_base;
      write_draw_register(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        unfuse_registers();
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      fused_dma_size = vgt_dma_size.value;
      write_draw_register(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (!draw_succeeded) {
    unfuse_registers();
  }
  if (draw_succeeded) {
    DrawRecord record;
    record.packet = packet;
    record.initiator = vgt_draw_initiator.value;
    record.is_indexed = is_indexed;
    record.fused_registers = fused_registers;
    record.dma_base = fused_dma_base;
    record.dma_size = fused_dma_size;
    record.index_buffer_info = index_buffer_info;
    record.opcode_name = opcode_name;
    if (!record_split_) {
      ExecuteDrawRecord(record);
    } else {
      // A plain record rather than a closure: the draw's state is too large
      // for std::function's inline storage, and a heap allocation per draw
      // freed on the other thread costs more than the copy.
      RecordWords& words = record_batch_->words;
      const size_t offset = words.size();
      words.resize(offset + 1 + kDrawRecordWords);
      words[offset] = kRecordDraw;
      std::memcpy(words.data() + offset + 1, &record, sizeof(record));
      if (++record_batch_draws_ >= 32) {
        PublishRecordBatch();
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

void CommandProcessor::ExecuteDrawRecord(const DrawRecord& record) {
  if (record.fused_registers) {
    // Per-draw registers outside the state epoch and hash, with no write side
    // effect: stored as WriteRegister would.
    uint32_t* values = register_file_->values;
    values[XE_GPU_REG_VGT_DRAW_INITIATOR] = record.initiator;
    if (record.fused_registers >= 3) {
      values[XE_GPU_REG_VGT_DMA_BASE] = record.dma_base;
      values[XE_GPU_REG_VGT_DMA_SIZE] = record.dma_size;
    }
  }
  const uint32_t packet = record.packet;
  const char* opcode_name = record.opcode_name;
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = record.initiator;
  const bool is_indexed = record.is_indexed;
  IndexBufferInfo index_buffer_info = record.index_buffer_info;
  bool draw_succeeded = true;
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      // Diagnostics for frame replays: which draw is which, and dropping a
      // range of them.
      static const auto skip_ranges = [] {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        const std::string list = REXCVAR_GET(fh1_debug_skip_draws);
        for (size_t start = 0; start < list.size();) {
          const size_t end = std::min(list.find(',', start), list.size());
          uint32_t first, last;
          if (std::sscanf(list.substr(start, end - start).c_str(), "%u-%u", &first, &last) == 2) {
            ranges.emplace_back(first, last);
          }
          start = end + 1;
        }
        return ranges;
      }();
      const uint32_t draw_index = debug_frame_draw_index_++;
      if (REXCVAR_GET(fh1_debug_log_draws)) {
        const RegisterFile& regs = *register_file_;
        std::string textures;
        if (active_pixel_shader_ && active_pixel_shader_->is_ucode_analyzed()) {
          for (const auto& binding : active_pixel_shader_->texture_bindings()) {
            const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
            const uint32_t* words =
                &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 6];
            textures += fmt::format(
                " t{}={:08X}:{}x{}:f{}:e{}:type{}:[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                binding.fetch_constant, fetch.base_address << 12, fetch.size_2d.width + 1,
                fetch.size_2d.height + 1, uint32_t(fetch.format), uint32_t(fetch.exp_adjust),
                uint32_t(fetch.type), words[0], words[1], words[2], words[3], words[4], words[5]);
          }
        }
        REXGPU_INFO(
            "FH1 debug draw {} mode={} prim={} indices={} vs={:016X} ps={:016X} color0={:08X} "
            "surface={:08X} mask={:08X} blend0={:08X} depth={:08X} vte={:08X} "
            "window_scissor={:08X}/{:08X} copy_dest={:08X} copy_info={:08X} copy_ctrl={:08X}{}",
            draw_index, regs[XE_GPU_REG_RB_MODECONTROL] & 7, uint32_t(vgt_draw_initiator.prim_type),
            uint32_t(vgt_draw_initiator.num_indices),
            active_vertex_shader_ ? active_vertex_shader_->ucode_data_hash() : 0,
            active_pixel_shader_ ? active_pixel_shader_->ucode_data_hash() : 0,
            regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
            regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_BLENDCONTROL0],
            regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
            regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_RB_COPY_DEST_INFO],
            regs[XE_GPU_REG_RB_COPY_CONTROL], textures);
      }
      static const auto null_fetch = [] {
        uint32_t draw = UINT32_MAX, fetch = 0;
        const std::string value = REXCVAR_GET(fh1_debug_null_fetch);
        if (!value.empty() && (std::sscanf(value.c_str(), "%u:%u", &draw, &fetch) != 2 ||
                               fetch >= xenos::kTextureFetchConstantCount)) {
          draw = UINT32_MAX;
        }
        return std::make_pair(draw, fetch);
      }();
      if (std::any_of(skip_ranges.begin(), skip_ranges.end(), [draw_index](const auto& range) {
            return draw_index >= range.first && draw_index <= range.second;
          })) {
        draw_succeeded = true;
      } else {
        // Through WriteRegister, so the backend sees the fetch constant change.
        const uint32_t null_fetch_register =
            XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + null_fetch.second * 6;
        const uint32_t saved_fetch_word = register_file_->values[null_fetch_register];
        if (draw_index == null_fetch.first) {
          // A vertex type in a texture slot is invalid even with
          // gpu_allow_invalid_fetch_constants, which binds the invalid type.
          WriteRegister(null_fetch_register,
                        (saved_fetch_word & ~uint32_t(3)) |
                            uint32_t(xenos::FetchConstantType::kVertex));
        }
        draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                   is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
        if (draw_index == null_fetch.first) {
          WriteRegister(null_fetch_register, saved_fetch_word);
        }
      }
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        static std::atomic_flag first_failed_draw_logged = ATOMIC_FLAG_INIT;
        if (!first_failed_draw_logged.test_and_set(std::memory_order_relaxed)) {
          REXGPU_ERROR(
              "M3_TRACE draw.failure.first opcode={} packet=0x{:08X} "
              "initiator=0x{:08X} num_indices={} prim_type={} source_select={} index_size={} "
              "major_mode={} explicit_major={} output_path=0x{:08X} path_select={} "
              "hos_control=0x{:08X} tess_mode={} rb_modecontrol=0x{:08X} edram_mode={}",
              opcode_name, packet, vgt_draw_initiator.value,
              uint32_t(vgt_draw_initiator.num_indices), uint32_t(vgt_draw_initiator.prim_type),
              uint32_t(vgt_draw_initiator.source_select), uint32_t(vgt_draw_initiator.index_size),
              uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
              vgt_output_path_cntl.value, uint32_t(vgt_output_path_cntl.path_select),
              vgt_hos_cntl.value, uint32_t(vgt_hos_cntl.tess_mode), rb_modecontrol.value,
              uint32_t(rb_modecontrol.edram_mode));
        }
        // Bounded: a draw that fails once usually fails every frame.
        static std::atomic<uint64_t> failed_draws{0};
        const uint64_t failed = failed_draws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failed <= 16 || !(failed & (failed - 1))) REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend ({} so far) "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            failed, uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    }
}

bool CommandProcessor::D3dCensusDraw(memory::RingBuffer* reader, uint32_t count) {
  if (d3d_census_decoder::Active()) {
    const bool copy =
        decode_regs().Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy;
    // Drawn for the census: a draw, not an EDRAM copy, of the first tile (or
    // of no tiling pass), the draws the untiled Android path keeps.
    d3d_census_decoder::Draw(TilingPassOfSelect() <= 0 && !copy,
                             TilingPassOfSelect() <= 0 && copy);
    if (copy && d3d_skip_visible_ >= 3 && d3d_census_decoder::Visible()) {
      static uint64_t visible_copies = 0;
      const uint64_t copies = ++visible_copies;
      if (!(copies & (copies - 1)) || !(copies % 10000)) {
        REXGPU_INFO("DL-1.1: {} EDRAM copies met in visible buffers with skip mode 3", copies);
      }
    }
    if (d3d_skip_visible_ && !copy && d3d_census_decoder::Visible()) {
      // DL-1.1: no draw state derivation, no recording, no submission.
      PERF_counter_inc(kD3dCensusSkippedDraws);
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }
  if (d3d_skipped_loads_pending_) {
    FlushSkippedShaderLoads();
  }
  return false;
}

void CommandProcessor::FlushSkippedShaderLoads() {
  d3d_skipped_loads_pending_ = false;
  for (SkippedShaderLoad& load : d3d_skipped_load_) {
    if (!load.valid) continue;
    load.valid = false;
    auto shader = LoadShader(load.type, load.address, load.ucode, load.dword_count);
    if (load.type == xenos::ShaderType::kVertex) {
      active_vertex_shader_ = shader;
    } else {
      active_pixel_shader_ = shader;
    }
  }
}

bool CommandProcessor::D3dCensusSkippableRange(uint32_t first, uint32_t count) const {
  if (!count) return false;
  const uint32_t last = first + count - 1;
  // ALU constants: from mode 2.
  if (first >= 0x4000 && last < 0x4800) return true;
  if (d3d_skip_visible_ < 3) return false;
  // Mode 3: anything but the registers fences, events, render targets,
  // windows, copies and resolves read (and fetch constant 0, the resolve
  // rectangle's vertices), so EDRAM copies and the CPU-visible protocol stay
  // exact while the draw state of visible buffers is not applied.
  static constexpr uint32_t kKept[][2] = {
      {0x0000, 0x2010},  // control, scratch, coherency; RB_*_INFO, screen scissor
      {0x2080, 0x2090},  // window offset and scissors
      {0x21F9, 0x21FA},  // VGT_EVENT_INITIATOR
      {0x2200, 0x2210},  // PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, RB_MODECONTROL
      {0x2310, 0x2330},  // RB_COPY_*, clears, sample counts
      {0x4800, 0x4806},  // fetch constant 0
  };
  for (const auto& range : kKept) {
    if (first < range[1] && last >= range[0]) return false;
  }
  return last < 0x10000;
}

bool CommandProcessor::D3dCensusSkipConstants(memory::RingBuffer* reader, uint32_t opcode,
                                              uint32_t count) {
  uint32_t first = 0, registers = 0;
  if (!ConstantPacketRange(*reader, opcode, count, &first, &registers)) return false;
  if (!D3dCensusSkippableRange(first, registers)) return false;
  PERF_counter_inc(kD3dCensusSkippedConstantPackets);
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ConstantPacketRange(const memory::RingBuffer& reader, uint32_t opcode,
                                           uint32_t count, uint32_t* first_out,
                                           uint32_t* registers_out) {
  memory::RingBuffer peek = reader;
  uint32_t first = 0, registers = 0;
  switch (opcode) {
    case PM4_SET_CONSTANT: {
      static constexpr uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      const uint32_t offset_type = peek.ReadAndSwap<uint32_t>();
      const uint32_t type = (offset_type >> 16) & 0xFF;
      if (type >= std::size(kBase)) return false;
      first = kBase[type] + (offset_type & 0x7FF);
      registers = count - 1;
    } break;
    case PM4_LOAD_ALU_CONSTANT: {
      static constexpr uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (count < 3) return false;
      peek.ReadAndSwap<uint32_t>();
      const uint32_t offset_type = peek.ReadAndSwap<uint32_t>();
      const uint32_t type = (offset_type >> 16) & 0xFF;
      if (type >= std::size(kBase)) return false;
      first = kBase[type] + (offset_type & 0x7FF);
      registers = peek.ReadAndSwap<uint32_t>() & 0xFFF;
    } break;
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
      first = peek.ReadAndSwap<uint32_t>() & 0xFFFF;
      registers = count - 1;
      break;
    default:
      return false;
  }
  *first_out = first;
  *registers_out = registers;
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  ++ib_stats.decoded_draws;
  PERF_counter_inc(kGpuDecodedDraws);
  if (D3dCensusDraw(reader, count)) {
    return true;
  }
  if (replay_probe.capturing &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    ReplayProbeMarkUnskippable();
  }
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  ++ib_stats.decoded_draws;
  PERF_counter_inc(kGpuDecodedDraws);
  if (D3dCensusDraw(reader, count)) {
    return true;
  }
  if (replay_probe.capturing &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    ReplayProbeMarkUnskippable();
  }
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  if (REXCVAR_GET(gpu_replay_floor_probe)) {
    PERF_counter_inc(kLoadAluConstantPackets);
    PERF_counter_add(kLoadAluConstantDwords, size_dwords);
    auto& source =
        replay_probe.constant_sources[(uint64_t(address) << 20) | (uint64_t(type) << 12) |
                                      size_dwords];
    const uint64_t hash = XXH3_64bits(xlat_address, size_t(size_dwords) * sizeof(uint32_t));
    if (source.swap != trace_swaps_) {
      source.swap = trace_swaps_;
      PERF_counter_inc(kLoadAluConstantSources);
    }
    if (source.hash && source.hash != (hash | 1)) {
      PERF_counter_inc(kLoadAluConstantChanged);
    }
    source.hash = hash | 1;
  }
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  if (shader_type != xenos::ShaderType::kVertex && shader_type != xenos::ShaderType::kPixel) {
    assert_unhandled_case(shader_type);
    return false;
  }
  if (!record_split_ && d3d_skip_visible_ && d3d_census_decoder::Active() &&
      d3d_census_decoder::Visible()) {
    // DL-1.1: done only if a draw that runs comes before the next load.
    d3d_skipped_load_[shader_type == xenos::ShaderType::kPixel] = {
        true, shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords};
    d3d_skipped_loads_pending_ = true;
    PERF_counter_inc(kD3dCensusSkippedShaderLoads);
    return true;
  }
  LoadGuestShader(shader_type, addr, size_dwords);
  return true;
}

void CommandProcessor::LoadGuestShader(xenos::ShaderType shader_type, uint32_t addr,
                                       uint32_t size_dwords) {
  const uint32_t* ucode = memory_->TranslatePhysical<uint32_t*>(addr);
  d3d_skipped_load_[shader_type == xenos::ShaderType::kPixel].valid = false;
  if (d3d_list_verify_depth_ == trace_ib_depth_) {
    D3dListVerifyLoad(uint32_t(shader_type), addr, size_dwords);
  }
  uint64_t cached_hash = 0;
  if (d3d_shader_cache_mode_ &&
      D3dShaderCacheLoad(shader_type, addr, size_dwords, ucode, cached_hash)) {
    // The same microcode as when hashed; the shader is set.
    decode_last_shader_load_[shader_type == xenos::ShaderType::kPixel].valid = false;
    return;
  }
  // Hashing the microcode is most of a shader load; the decoder has time.
  // The shader cache may have hashed it already.
  const uint64_t ucode_hash =
      cached_hash ? cached_hash : (record_split_ ? HashShaderMicrocode(ucode, size_dwords) : 0);
  if (record_split_ && REXCVAR_GET(gpu_record_shader_load_entries)) {
    decode_shader_loads_.store(decode_shader_loads_.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
    LastShaderLoad& last = decode_last_shader_load_[shader_type == xenos::ShaderType::kPixel];
    if (last.valid && last.hash == ucode_hash && last.dword_count == size_dwords) {
      // The recorder would look up the same shader and set it again.
      decode_shader_loads_skipped_.store(
          decode_shader_loads_skipped_.load(std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
      return;
    }
    last.valid = true;
    last.hash = ucode_hash;
    last.dword_count = size_dwords;
    FlushPendingScratchWrites();
    RecordWords& words = record_batch_->words;
    const size_t offset = words.size();
    words.resize(offset + 1 + kLoadShaderRecordWords);
    uint32_t* entry = words.data() + offset;
    entry[0] = kRecordLoadShader;
    entry[1] = uint32_t(shader_type);
    entry[2] = addr;
    entry[3] = size_dwords;
    const uint64_t ucode_pointer = uint64_t(reinterpret_cast<uintptr_t>(ucode));
    std::memcpy(entry + 4, &ucode_pointer, sizeof(ucode_pointer));
    std::memcpy(entry + 6, &ucode_hash, sizeof(ucode_hash));
    return;
  }
  decode_last_shader_load_[shader_type == xenos::ShaderType::kPixel].valid = false;
  RecordCall([this, shader_type, addr, ucode, size_dwords, ucode_hash]() {
    auto shader = ucode_hash ? LoadShaderHashed(shader_type, addr, ucode, size_dwords, ucode_hash)
                             : LoadShader(shader_type, addr, ucode, size_dwords);
    if (shader_type == xenos::ShaderType::kVertex) {
      active_vertex_shader_ = shader;
    } else {
      active_pixel_shader_ = shader;
    }
  });
}

void CommandProcessor::ExecuteLoadShaderRecord(const uint32_t* words) {
  const auto shader_type = xenos::ShaderType(words[0]);
  uint64_t ucode_pointer, ucode_hash;
  std::memcpy(&ucode_pointer, words + 3, sizeof(ucode_pointer));
  std::memcpy(&ucode_hash, words + 5, sizeof(ucode_hash));
  const uint32_t* ucode = reinterpret_cast<const uint32_t*>(uintptr_t(ucode_pointer));
  Shader* shader = LoadShaderHashed(shader_type, words[1], ucode, words[2], ucode_hash);
  if (shader_type == xenos::ShaderType::kVertex) {
    active_vertex_shader_ = shader;
  } else {
    active_pixel_shader_ = shader;
  }
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  if (shader_type != xenos::ShaderType::kVertex && shader_type != xenos::ShaderType::kPixel) {
    assert_unhandled_case(shader_type);
    return false;
  }
  LoadInlineShader(shader_type, uint32_t(reader->read_ptr()),
                   reinterpret_cast<const uint32_t*>(reader->read_ptr()), size_dwords);
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

void CommandProcessor::LoadInlineShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                        const uint32_t* ucode_words, uint32_t size_dwords) {
  d3d_skipped_load_[shader_type == xenos::ShaderType::kPixel].valid = false;
  if (d3d_list_verify_depth_ == trace_ib_depth_) {
    D3dListVerifyLoad(uint32_t(shader_type), 0, size_dwords);
  }
  if (!record_split_) {
    auto shader =
        LoadShader(shader_type, guest_address, const_cast<uint32_t*>(ucode_words), size_dwords);
    if (shader_type == xenos::ShaderType::kVertex) {
      active_vertex_shader_ = shader;
    } else {
      active_pixel_shader_ = shader;
    }
  } else {
    // The microcode is in the ring, which the title may reuse once this
    // thread moves on: the recorder gets a copy.
    decode_last_shader_load_[shader_type == xenos::ShaderType::kPixel].valid = false;
    std::vector<uint32_t> ucode(size_dwords);
    std::memcpy(ucode.data(), ucode_words, size_dwords * sizeof(uint32_t));
    RecordCall([this, shader_type, guest_address, ucode = std::move(ucode)]() mutable {
      auto shader = LoadShader(shader_type, guest_address, ucode.data(), uint32_t(ucode.size()));
      if (shader_type == xenos::ShaderType::kVertex) {
        active_vertex_shader_ = shader;
      } else {
        active_pixel_shader_ = shader;
      }
    });
  }
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
  } else {
    // end the viz query
    PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    const uint32_t status_register =
        id < 32 ? XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0 : XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1;
    PacketWriteRegister(status_register,
                        decode_regs().values[status_register] | (uint32_t(1) << (id & 31)));
  }

  return true;
}


void CommandProcessor::D3dDrawCountDraw() {
  ++ib_stats.decoded_draws;
  PERF_counter_inc(kGpuDecodedDraws);
}

}  // namespace rex::graphics
