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
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <xxhash.h>

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
#include <rex/system/gpu_write_signal.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");
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
REXCVAR_DEFINE_BOOL(gpu_record_merge_constant_runs, true, "GPU",
                    "With gpu_record_thread, the decoder extends the last recorded float "
                    "constant run over a gap of up to 16 registers (their current values) "
                    "instead of recording another run, so the recorder handles fewer runs")
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
REXCVAR_DEFINE_BOOL(gpu_trace_wait_reg_mem_writers, false, "GPU",
                    "Diagnostics: while WAIT_REG_MEM waits on a guest memory word, watch its "
                    "page and log the guest threads (and their link register) that write it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
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
// frame rate at 120, so faster rates keep the 2 ms yield (NP-9.3).
WaitRegMemPolicy CurrentWaitRegMemPolicy() {
  WaitRegMemPolicy policy{REXCVAR_GET(wait_reg_mem_yield_us), REXCVAR_GET(wait_reg_mem_sleep_us)};
#if REX_PLATFORM_WIN32
  if (policy.yield_us < 0 || policy.sleep_us < 0) {
    // The game rate's settings live in other modules: read them by name,
    // at most twice a second (the decoder thread is the only caller).
    thread_local bool fast = false;
    thread_local std::chrono::steady_clock::time_point next_read{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_read) {
      next_read = now + std::chrono::milliseconds(500);
      const int limit = std::atoi(rex::cvar::GetFlagByName("pinyon_shift_fh1_render_fps_limit").c_str());
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
  if (!pending_any_ && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_fns_.push({std::move(fn), false});
    pending_any_ = true;
  }
}

void CommandProcessor::CallInWorkerThread(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_fns_.push({std::move(fn), true});
  pending_any_ = true;
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
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > uint32_t(REXCVAR_GET(gpu_idle_spin_count))) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
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
  for (;;) {
    std::unique_ptr<RecordBatch> batch;
    {
      std::unique_lock<std::mutex> lock(record_mutex_);
      if (record_queue_.empty() && !record_stop_) {
        lock.unlock();
        read_cpu(std::chrono::steady_clock::now());
        lock.lock();
      }
      record_ready_.wait(lock, [this]() { return record_stop_ || !record_queue_.empty(); });
      if (record_queue_.empty()) {
        return;
      }
      batch = std::move(record_queue_.front());
      record_queue_.pop_front();
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
    {
      std::lock_guard<std::mutex> lock(record_mutex_);
      record_free_.push_back(std::move(batch));
      ++record_completed_;
    }
    record_done_.notify_all();
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
      default:
        assert_always();
        return;
    }
  }
}

void CommandProcessor::ExecuteRecordBatchTimed(RecordBatch& batch) {
  // ExecuteRecordBatch with each entry kind's time (RR-0.1's coverage).
  auto elapsed = [](uint64_t start) { return CostTicks() - start; };
  const uint64_t batch_start = CostTicks();
  const uint32_t* words = batch.words.data();
  const size_t count = batch.words.size();
  for (size_t i = 0; i < count;) {
    const uint64_t start = CostTicks();
    ++record_entries_;
    switch (words[i]) {
      case kRecordRun: {
        const uint32_t first = words[i + 1];
        const uint32_t run = words[i + 2];
        WriteRegistersHost(first, words + i + 3, run);
        i += 3 + run;
      } break;
      case kRecordOne:
        WriteRegister(words[i + 1], words[i + 2]);
        i += 3;
        record_ones_ns_ += elapsed(start);
        break;
      case kRecordCall:
        batch.fns[words[i + 1]]();
        i += 2;
        record_calls_ns_ += elapsed(start);
        break;
      case kRecordDraw: {
        DrawRecord record;
        std::memcpy(&record, words + i + 1, sizeof(record));
        ExecuteDrawRecord(record);
        i += 1 + kDrawRecordWords;
        record_draws_ns_ += elapsed(start);
      } break;
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
}  // namespace

void CommandProcessor::PacketWriteRegister(uint32_t index, uint32_t value) {
  if (!record_split_) {
    WriteRegister(index, value);
    return;
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
  std::vector<uint32_t>& words = record_batch_->words;
  words.push_back(kRecordOne);
  words.push_back(index);
  words.push_back(value);
}

void CommandProcessor::PacketWriteRegistersFromMem(uint32_t start_index, const uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (!record_split_) {
    WriteRegistersFromMem(start_index, const_cast<uint32_t*>(base), num_registers);
    return;
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
  std::vector<uint32_t>& words = record_batch_->words;
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
  if (!record_split_ || record_batch_->words.empty()) {
    return;
  }
  last_constant_run_batch_ = nullptr;
  std::unique_ptr<RecordBatch> next;
  {
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_queue_.push_back(std::move(record_batch_));
    ++record_published_;
    if (!record_free_.empty()) {
      next = std::move(record_free_.back());
      record_free_.pop_back();
    }
  }
  record_ready_.notify_one();
  record_batch_ = next ? std::move(next) : std::make_unique<RecordBatch>();
  record_batch_draws_ = 0;
}

void CommandProcessor::RecordSync() {
  if (!record_split_) {
    return;
  }
  PublishRecordBatch();
  std::unique_lock<std::mutex> lock(record_mutex_);
  record_done_.wait(lock, [this]() { return record_completed_ == record_published_; });
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

void CommandProcessor::ShutdownContext() {}

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
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
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

  if (template_stats) {
    RecordCall([this]() { TemplateStatsIbEnd(); });
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
  const uint32_t packet_offset = reader->read_offset();
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if (packet == 0) {
    return true;
  }
  if (packet_recorder_) {
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

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
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
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
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
  RecordCall([this, cpu_mask]() {
    if (record_split_) {
      FlushCpuVisibleResults();
    }
    for (int n = 0; n < 6; n++) {
      if (cpu_mask & (1 << n)) {
        if (graphics_system_) {
          graphics_system_->DispatchInterruptCallback(1, n);
        }
      }
    }
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
    if (!matched) {
      if (wait_start == std::chrono::steady_clock::time_point{}) {
        wait_start = std::chrono::steady_clock::now();
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
    PERF_counter_inc(kGpuRegMemWaitCount);
    if (slept) PERF_counter_inc(kGpuRegMemSleptCount);
    PERF_counter_add(kGpuThreadRegMemWaitNs,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - wait_start)
                         .count());
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
    memory::store(memory_->TranslatePhysical(address), data_value);
    return true;
  }
  RecordCall([this, address, data_value]() {
    FlushCpuVisibleResults();
    memory::store(memory_->TranslatePhysical(address), data_value);
    system::SignalGpuWrite();
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
  PacketWriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

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
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

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

  if (draw_succeeded) {
    DrawRecord record;
    record.packet = packet;
    record.initiator = vgt_draw_initiator.value;
    record.is_indexed = is_indexed;
    record.index_buffer_info = index_buffer_info;
    record.opcode_name = opcode_name;
    if (!record_split_) {
      ExecuteDrawRecord(record);
    } else {
      // A plain record rather than a closure: the draw's state is too large
      // for std::function's inline storage, and a heap allocation per draw
      // freed on the other thread costs more than the copy.
      std::vector<uint32_t>& words = record_batch_->words;
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

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  ++ib_stats.decoded_draws;
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
  const uint32_t* ucode = memory_->TranslatePhysical<uint32_t*>(addr);
  // Hashing the microcode is most of a shader load; the decoder has time.
  const uint64_t ucode_hash = record_split_ ? HashShaderMicrocode(ucode, size_dwords) : 0;
  RecordCall([this, shader_type, addr, ucode, size_dwords, ucode_hash]() {
    auto shader = ucode_hash ? LoadShaderHashed(shader_type, addr, ucode, size_dwords, ucode_hash)
                             : LoadShader(shader_type, addr, ucode, size_dwords);
    if (shader_type == xenos::ShaderType::kVertex) {
      active_vertex_shader_ = shader;
    } else {
      active_pixel_shader_ = shader;
    }
  });
  return true;
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
  const uint32_t guest_address = uint32_t(reader->read_ptr());
  if (!record_split_) {
    auto shader = LoadShader(shader_type, guest_address,
                             reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
    if (shader_type == xenos::ShaderType::kVertex) {
      active_vertex_shader_ = shader;
    } else {
      active_pixel_shader_ = shader;
    }
  } else {
    // The microcode is in the ring, which the title may reuse once this
    // thread moves on: the recorder gets a copy.
    std::vector<uint32_t> ucode(size_dwords);
    std::memcpy(ucode.data(), reinterpret_cast<const void*>(reader->read_ptr()),
                size_dwords * sizeof(uint32_t));
    RecordCall([this, shader_type, guest_address, ucode = std::move(ucode)]() mutable {
      auto shader = LoadShader(shader_type, guest_address, ucode.data(), uint32_t(ucode.size()));
      if (shader_type == xenos::ShaderType::kVertex) {
        active_vertex_shader_ = shader;
      } else {
        active_pixel_shader_ = shader;
      }
    });
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
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

}  // namespace rex::graphics
