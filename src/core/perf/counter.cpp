/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>
#include <rex/thread/sync_census.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if REX_PLATFORM_WIN32
#include <share.h>
#include <windows.h>
#include <psapi.h>
#include <TraceLoggingProvider.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#endif
#else
#include <unistd.h>
#include <fstream>
#endif

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");
REXCVAR_DEFINE_STRING(perf_thread_cpu, "", "Perf",
                      "Up to eight thread-name prefixes separated by semicolons (for example "
                      "'Guest 825A6320;GPU Submission'): the CSV's thread_cpu_slotN_ns column "
                      "is the CPU time this frame of the threads whose name starts with prefix "
                      "N (Linux and Android)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(perf_log_max_mb, 0, "Perf",
                    "Stop CSV recording at this size (0 = unlimited)");
REXCVAR_DEFINE_BOOL(perf_critical_path_trace, false, "Perf",
                    "Log source-frame critical-path events")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(perf_aa_toggle, 0, "Perf",
                     "A/A harness: a no-op value recorded in the perf CSV each frame, which "
                     "a route flips to mark comparison windows")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(perf_inject_cpu_us, 0, "Perf",
                     "A/A harness: spin this many microseconds of CPU time once a frame, a "
                     "known cost the comparison must detect")
    .range(0, 20000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(perf_inject_cpu_thread, true, "Perf",
                    "A/A harness: spin perf_inject_cpu_us on a thread of its own, woken once "
                    "a frame, rather than on the GPU commands thread, whose idle yield loop "
                    "already burns CPU that the spin would only replace")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::perf {

namespace {

#if REX_PLATFORM_WIN32
TRACELOGGING_DEFINE_PROVIDER(
    g_critical_path_provider, "PinyonShift-CriticalPath",
    (0xf36ab1a6, 0x80bb, 0x4482, 0xb1, 0xb9, 0x92, 0xa6, 0xec, 0x87, 0x02,
     0x58));

struct CriticalPathProviderRegistration {
  CriticalPathProviderRegistration() {
    TraceLoggingRegister(g_critical_path_provider);
  }
  ~CriticalPathProviderRegistration() {
    TraceLoggingUnregister(g_critical_path_provider);
  }
};

CriticalPathProviderRegistration g_critical_path_provider_registration;

bool CriticalPathEtwEnabled() {
  return TraceLoggingProviderEnabled(g_critical_path_provider, 0, 0) != FALSE;
}
#else
constexpr bool CriticalPathEtwEnabled() { return false; }
#endif

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

// Values written with SetCounter; increments live in per-thread blocks.
std::array<std::atomic<int64_t>, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

// Increments go to a block owned by the calling thread: one writer per
// block, so an increment is a plain load and store with no locked
// instruction and no cache line shared with other threads. Readers sum the
// blocks. Blocks are never freed, so totals survive thread exit.
struct alignas(64) ThreadCounterBlock {
  std::array<std::atomic<int64_t>, kNumCounters> totals{};
  ThreadCounterBlock* next = nullptr;
};
std::atomic<ThreadCounterBlock*> g_thread_blocks{nullptr};

ThreadCounterBlock& LocalCounterBlock() {
  thread_local ThreadCounterBlock* block = nullptr;
  if (!block) {
    block = new ThreadCounterBlock();
    ThreadCounterBlock* head = g_thread_blocks.load(std::memory_order_relaxed);
    do {
      block->next = head;
    } while (!g_thread_blocks.compare_exchange_weak(head, block, std::memory_order_release,
                                                    std::memory_order_relaxed));
  }
  return *block;
}

int64_t SumThreadTotals(size_t index) {
  int64_t sum = 0;
  for (ThreadCounterBlock* block = g_thread_blocks.load(std::memory_order_acquire); block;
       block = block->next) {
    sum += block->totals[index].load(std::memory_order_relaxed);
  }
  return sum;
}

// Thread totals at the last frame snapshot (accumulators) and at Init.
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot_totals{};
std::array<std::atomic<int64_t>, kNumCounters> g_init_totals{};

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "guest_frame_gpu_time_ns",
    "native_composition_gpu_time_ns",
    "native_selection_gpu_time_ns",
    "guest_frame_gpu_timing_samples",
    "native_composition_gpu_timing_samples",
    "native_selection_gpu_timing_samples",
    "native_gpu_timing_drops",
    "xma_frames_decoded",
    "xma_no_space_stalls",
    "xma_no_progress_stalls",
    "xma_stall_recoveries",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
    "memexport_draws",
    "memexport_bytes",
    "memexport_sync_fallbacks",
    "memexport_queue_waits",
    "memexport_fence_waits",
    "resolve_readback_requests",
    "resolve_readback_bytes",
    "resolve_readback_fast_copies",
    "resolve_readback_cache_misses",
    "resolve_readback_full_waits",
    "resolve_readback_wait_time_ns",
    "guest_vblank_count",
    "guest_vblank_delta_ns",
    "simulation_tick_count",
    "simulation_time_ns",
    "simulation_delta_invalid",
    "source_frame_count",
    "present_count",
    "present_delta_ns",
    "present_queue_depth",
    "present_deadline_misses",
    "duplicate_present_count",
    "dropped_present_count",
    "texture_request_cpu_time_ns",
    "texture_request_timing_samples",
    "texture_dirty_load_attempts",
    "fh1_issue_draw_cpu_time_ns",
    "fh1_prepared_snapshot_cpu_time_ns",
    "fh1_prepared_observer_cpu_time_ns",
    "fh1_binding_cpu_time_ns",
    "fh1_issue_draw_calls",
    "fh1_material_snapshot_cpu_time_ns",
    "fh1_material_snapshot_calls",
    "fh1_native_output_cpu_time_ns",
    "fh1_title_thread_cpu_time_ns",
    "fh1_gpu_thread_cpu_time_ns",
    "texture_resolve_reloads",
    "texture_resolve_reload_bytes",
    "write_watch_triggers",
    "write_watch_protect_calls",
    "write_watch_protect_pages",
    "texture_resolve_reload_cpu_time_ns",
    "clock_mutex_contentions",
    "timer_queue_wakeups",
    "timer_queue_callbacks",
    "gpu_thread_idle_ns",
    "gpu_thread_reg_mem_wait_ns",
    "gpu_thread_fence_wait_ns",
    "gpu_submission_busy_ns",
    "gpu_recorder_busy_ns",
    "critical_region_blocked_ns",
    "gpu_decoder_cpu_ns",
    "gpu_recorder_cpu_ns",
    "gpu_reg_mem_wait_count",
    "gpu_reg_mem_slept_count",
    "title_gpu_fence_wait_count",
    "title_gpu_fence_wait_ns",
    "texture_band_reloads",
    "texture_layer_reloads",
    "memory_device_usage_mb",
    "memory_device_budget_mb",
    "fh1_surface_count",
    "fh1_surface_mb",
    "texture_cache_mb",
    "process_resident_mb",
    "aa_toggle",
    "injected_cpu_ns",
    "process_cpu_ns",
    "xma_work_event_sets",
    "fh1_untile_skipped_draws",
    "kernel_object_notifies",
    "kernel_object_notifies_no_waiter",
    "guest_range_readable_checks",
    "rtl_critical_section_thread_lookups",
    "host_path_lookup_misses",
    "gpu_decoded_draws",
    "replay_probe_ibs",
    "replay_probe_repeat_ibs",
    "replay_probe_skipped_ibs",
    "replay_probe_drawn_in_ibs",
    "replay_probe_drawn_in_repeat_ibs",
    "replay_probe_drawn_in_skippable_repeat_ibs",
    "replay_probe_skipped_drawn_draws",
    "replay_probe_hash_ns",
    "replay_probe_apply_ns",
    "replay_probe_exit_registers",
    "load_alu_constant_packets",
    "load_alu_constant_dwords",
    "load_alu_constant_sources",
    "load_alu_constant_changed",
    "thread_cpu_slot0_ns",
    "thread_cpu_slot1_ns",
    "thread_cpu_slot2_ns",
    "thread_cpu_slot3_ns",
    "thread_cpu_slot4_ns",
    "thread_cpu_slot5_ns",
    "thread_cpu_slot6_ns",
    "thread_cpu_slot7_ns",
    "write_watch_protect_ns",
    "write_watch_slow_protects",
    "xma_decoder_opens",
    "critical_region_unlocked_sections",
    "gpu_renderings",
    "gpu_upload_splits_vertex",
    "gpu_upload_splits_index",
    "gpu_upload_splits_texture",
    "gpu_uploads_hoisted",
    "gpu_upload_bytes",
    "d3d_census_visible_draws",
    "d3d_census_fallback_draws",
    "d3d_census_skipped_draws",
    "d3d_census_skipped_shader_loads",
    "d3d_census_skipped_constant_packets",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

// Gauge counters are snapshotted but NOT zeroed each frame.
// Accumulators (everything else) are zeroed after snapshot.
constexpr bool kIsGauge[] = {
    false,  // kFrameTimeUs       (set each frame)
    false,  // kFps               (set each frame)
    false,  // kDrawCalls
    false,  // kCommandBufferStalls
    false,  // kVerticesProcessed
    false,  // kGuestFrameGpuTimeNs
    false,  // kNativeCompositionGpuTimeNs
    false,  // kNativeSelectionGpuTimeNs
    false,  // kGuestFrameGpuTimingSamples
    false,  // kNativeCompositionGpuTimingSamples
    false,  // kNativeSelectionGpuTimingSamples
    false,  // kNativeGpuTimingDrops
    false,  // kXmaFramesDecoded
    false,  // kXmaNoSpaceStalls
    false,  // kXmaNoProgressStalls
    false,  // kXmaStallRecoveries
    false,  // kAudioFrameLatencyUs
    false,  // kBufferQueueDepth  (set each frame)
    false,  // kFunctionsDispatched
    false,  // kInterruptDispatches
    true,   // kActiveThreads     (inc/dec over lifetime)
    false,  // kApcQueueDepth
    true,   // kCriticalRegionContentions (running total)
    false,  // kTextureCacheHits
    false,  // kTextureCacheMisses
    false,  // kPipelineCacheHits
    false,  // kPipelineCacheMisses
    false,  // kMemexportDraws
    false,  // kMemexportBytes
    false,  // kMemexportSyncFallbacks
    false,  // kMemexportQueueWaits
    false,  // kMemexportFenceWaits
    false,  // kResolveReadbackRequests
    false,  // kResolveReadbackBytes
    false,  // kResolveReadbackFastCopies
    false,  // kResolveReadbackCacheMisses
    false,  // kResolveReadbackFullWaits
    false,  // kResolveReadbackWaitTimeNs
    false,  // kGuestVblankCount
    false,  // kGuestVblankDeltaNs
    false,  // kSimulationTickCount
    false,  // kSimulationTimeNs
    false,  // kSimulationDeltaInvalid
    false,  // kSourceFrameCount
    false,  // kPresentCount
    false,  // kPresentDeltaNs
    false,  // kPresentQueueDepth
    false,  // kPresentDeadlineMisses
    false,  // kDuplicatePresentCount
    false,  // kDroppedPresentCount
    false,  // kTextureRequestCpuTimeNs
    false,  // kTextureRequestTimingSamples
    false,  // kTextureDirtyLoadAttempts
    false,  // kFh1IssueDrawCpuTimeNs
    false,  // kFh1PreparedSnapshotCpuTimeNs
    false,  // kFh1PreparedObserverCpuTimeNs
    false,  // kFh1BindingCpuTimeNs
    false,  // kFh1IssueDrawCalls
    false,  // kFh1MaterialSnapshotCpuTimeNs
    false,  // kFh1MaterialSnapshotCalls
    false,  // kFh1NativeOutputCpuTimeNs
    false,  // kFh1TitleThreadCpuTimeNs
    false,  // kFh1GpuThreadCpuTimeNs
    false,  // kTextureResolveReloads
    false,  // kTextureResolveReloadBytes
    false,  // kWriteWatchTriggers
    false,  // kWriteWatchProtectCalls
    false,  // kWriteWatchProtectPages
    false,  // kTextureResolveReloadCpuTimeNs
    false,  // kClockMutexContentions
    false,  // kTimerQueueWakeups
    false,  // kTimerQueueCallbacks
    false,  // kGpuThreadIdleNs
    false,  // kGpuThreadRegMemWaitNs
    false,  // kGpuThreadFenceWaitNs
    false,  // kGpuSubmissionBusyNs
    false,  // kGpuRecorderBusyNs
    false,  // kCriticalRegionBlockedNs
    false,  // kGpuDecoderCpuNs
    false,  // kGpuRecorderCpuNs
    false,  // kGpuRegMemWaitCount
    false,  // kGpuRegMemSleptCount
    false,  // kTitleGpuFenceWaitCount
    false,  // kTitleGpuFenceWaitNs
    false,  // kTextureBandReloads
    false,  // kTextureLayerReloads
    true,   // kMemoryDeviceUsageMb
    true,   // kMemoryDeviceBudgetMb
    true,   // kFh1SurfaceCount
    true,   // kFh1SurfaceMb
    true,   // kTextureCacheMb
    true,   // kProcessResidentMb
    false,  // kAaToggle (set each frame)
    false,  // kInjectedCpuNs
    false,  // kProcessCpuNs (set each frame)
    false,  // kXmaWorkEventSets
    false,  // kFh1UntileSkippedDraws
    false,  // kKernelObjectNotifies
    false,  // kKernelObjectNotifiesNoWaiter
    false,  // kGuestRangeReadableChecks
    false,  // kRtlCriticalSectionThreadLookups
    false,  // kHostPathLookupMisses
    false,  // kGpuDecodedDraws
    false,  // kReplayProbeIbs
    false,  // kReplayProbeRepeatIbs
    false,  // kReplayProbeSkippedIbs
    false,  // kReplayProbeDrawnInIbs
    false,  // kReplayProbeDrawnInRepeatIbs
    false,  // kReplayProbeDrawnInSkippableRepeatIbs
    false,  // kReplayProbeSkippedDrawnDraws
    false,  // kReplayProbeHashNs
    false,  // kReplayProbeApplyNs
    false,  // kReplayProbeExitRegisters
    false,  // kLoadAluConstantPackets
    false,  // kLoadAluConstantDwords
    false,  // kLoadAluConstantSources
    false,  // kLoadAluConstantChanged
    false,  // kThreadCpuSlot0Ns (set each frame)
    false,  // kThreadCpuSlot1Ns
    false,  // kThreadCpuSlot2Ns
    false,  // kThreadCpuSlot3Ns
    false,  // kThreadCpuSlot4Ns
    false,  // kThreadCpuSlot5Ns
    false,  // kThreadCpuSlot6Ns
    false,  // kThreadCpuSlot7Ns
    false,  // kWriteWatchProtectNs
    false,  // kWriteWatchSlowProtects
    false,  // kXmaDecoderOpens
    false,  // kCriticalRegionUnlockedSections
    false,  // kGpuRenderings
    false,  // kGpuUploadSplitsVertex
    false,  // kGpuUploadSplitsIndex
    false,  // kGpuUploadSplitsTexture
    false,  // kGpuUploadsHoisted
    false,  // kGpuUploadBytes
    false,  // kD3dCensusVisibleDraws
    false,  // kD3dCensusFallbackDraws
    false,  // kD3dCensusSkippedDraws
    false,  // kD3dCensusSkippedShaderLoads
    false,  // kD3dCensusSkippedConstantPackets
};
static_assert(std::size(kIsGauge) == kNumCounters, "kIsGauge must match CounterId enum");

// CSV state
std::FILE* g_csv_file = nullptr;
std::string g_csv_path;
int g_csv_frame_count = 0;

}  // anonymous namespace

const char* CounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

void SetCounter(CounterId id, int64_t value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, int64_t delta) {
  std::atomic<int64_t>& total = LocalCounterBlock().totals[static_cast<size_t>(id)];
  total.store(total.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
}

int64_t GetCounter(CounterId id) {
  const size_t index = static_cast<size_t>(id);
  const int64_t totals = SumThreadTotals(index);
  const int64_t base = kIsGauge[index] ? g_init_totals[index].load(std::memory_order_relaxed)
                                       : g_snapshot_totals[index].load(std::memory_order_relaxed);
  return g_counters[index].load(std::memory_order_relaxed) + totals - base;
}

int64_t GetTotalCounter(CounterId id) {
  const size_t index = static_cast<size_t>(id);
  return SumThreadTotals(index) - g_init_totals[index].load(std::memory_order_relaxed);
}

namespace {
int64_t ProcessCpuTimeNs() {
#if REX_PLATFORM_WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  ULARGE_INTEGER kernel_time{}, user_time{};
  kernel_time.LowPart = kernel.dwLowDateTime;
  kernel_time.HighPart = kernel.dwHighDateTime;
  user_time.LowPart = user.dwLowDateTime;
  user_time.HighPart = user.dwHighDateTime;
  return int64_t(kernel_time.QuadPart + user_time.QuadPart) * 100;
#else
  timespec time = {};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time)) return 0;
  return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#endif
}
std::atomic<int64_t> g_last_process_cpu_ns{0};
}  // namespace

namespace {
void SpinCpu(int64_t target_ns) {
  const int64_t start = CurrentThreadCpuTimeNs();
  int64_t spent = 0;
  volatile uint64_t sink = 0;
  while (spent < target_ns) {
    for (int i = 0; i < 256; ++i) sink = sink * 6364136223846793005ull + 1442695040888963407ull;
    spent = CurrentThreadCpuTimeNs() - start;
  }
  IncrementCounter(CounterId::kInjectedCpuNs, spent);
}

// perf_inject_cpu_thread: one spin per frame on a thread that otherwise sleeps.
struct InjectWorker {
  std::mutex mutex;
  std::condition_variable wake;
  int64_t pending_ns = 0;
  InjectWorker() {
    std::thread([this]() {
      for (;;) {
        int64_t target_ns;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [this]() { return pending_ns > 0; });
          target_ns = pending_ns;
          pending_ns = 0;
        }
        SpinCpu(target_ns);
      }
    }).detach();
  }
};
}  // namespace

void InjectFrameCost() {
  const int32_t inject_us = REXCVAR_GET(perf_inject_cpu_us);
  if (inject_us <= 0) return;
  const int64_t target_ns = int64_t(inject_us) * 1000;
  if (!REXCVAR_GET(perf_inject_cpu_thread)) {
    SpinCpu(target_ns);
    return;
  }
  static InjectWorker* const worker = new InjectWorker();
  {
    std::lock_guard<std::mutex> lock(worker->mutex);
    // A frame the worker has not caught up with adds no more.
    worker->pending_ns = target_ns;
  }
  worker->wake.notify_one();
}

// perf_thread_cpu: threads matched by name and the CPU time each had at the
// last frame. Linux CPU clocks can be read for any thread of the process by
// its id (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
namespace {
constexpr size_t kThreadCpuSlots = 8;
struct TrackedThread {
  int64_t tid;
  size_t slot;
  int64_t last_ns;
};
std::mutex g_thread_cpu_mutex;
std::vector<TrackedThread> g_thread_cpu_threads;

std::vector<std::string> ThreadCpuPrefixes() {
  std::vector<std::string> prefixes;
  const std::string value = REXCVAR_GET(perf_thread_cpu);
  size_t start = 0;
  while (start <= value.size() && prefixes.size() < kThreadCpuSlots) {
    const size_t end = value.find(';', start);
    prefixes.push_back(value.substr(start, end == std::string::npos ? std::string::npos
                                                                    : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return prefixes;
}

int64_t ThreadCpuNs(int64_t tid) {
#if REX_PLATFORM_LINUX
  timespec time = {};
  const clockid_t clock = clockid_t((~clockid_t(tid) << 3) | 6);
  if (clock_gettime(clock, &time)) return -1;
  return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#else
  (void)tid;
  return -1;
#endif
}

void SampleThreadCpu() {
  std::array<int64_t, kThreadCpuSlots> sums{};
  {
    std::lock_guard<std::mutex> lock(g_thread_cpu_mutex);
    for (size_t i = 0; i < g_thread_cpu_threads.size();) {
      TrackedThread& thread = g_thread_cpu_threads[i];
      const int64_t now = ThreadCpuNs(thread.tid);
      if (now < 0) {
        // The thread exited.
        g_thread_cpu_threads.erase(g_thread_cpu_threads.begin() + i);
        continue;
      }
      if (thread.last_ns >= 0 && now >= thread.last_ns) sums[thread.slot] += now - thread.last_ns;
      thread.last_ns = now;
      ++i;
    }
  }
  for (size_t slot = 0; slot < kThreadCpuSlots; ++slot) {
    SetCounter(CounterId(size_t(CounterId::kThreadCpuSlot0Ns) + slot), sums[slot]);
  }
}
}  // namespace

void NoteNamedThread(int64_t tid, std::string_view name) {
  // Parsed per call: threads are named rarely, some before the cvars load.
  const std::vector<std::string> prefixes = ThreadCpuPrefixes();
  if (prefixes.empty() || tid <= 0) return;
  for (size_t slot = 0; slot < prefixes.size(); ++slot) {
    const std::string& prefix = prefixes[slot];
    if (prefix.empty() || name.substr(0, prefix.size()) != prefix) continue;
    std::lock_guard<std::mutex> lock(g_thread_cpu_mutex);
    for (TrackedThread& thread : g_thread_cpu_threads) {
      if (thread.tid == tid) {
        thread.slot = slot;
        return;
      }
    }
    g_thread_cpu_threads.push_back({tid, slot, ThreadCpuNs(tid)});
    REXSYS_INFO("perf_thread_cpu: slot {} tracks thread {} '{}'", slot, tid, name);
    return;
  }
}

void ResetFrameCounters() {
  // critical_region_census and write_watch_fault_trace (NN-3.10/3.12 probes).
  rex::thread::census::Frame();
  SetCounter(CounterId::kAaToggle, REXCVAR_GET(perf_aa_toggle));
  SampleThreadCpu();
  {
    const int64_t now = ProcessCpuTimeNs();
    const int64_t last = g_last_process_cpu_ns.exchange(now, std::memory_order_relaxed);
    SetCounter(CounterId::kProcessCpuNs, last ? now - last : 0);
  }
  for (size_t i = 0; i < kNumCounters; ++i) {
    const int64_t totals = SumThreadTotals(i);
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed) + totals -
                              g_init_totals[i].load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot this frame's increments and set value, then
      // start the next frame from here.
      const int64_t previous = g_snapshot_totals[i].exchange(totals, std::memory_order_relaxed);
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed) + totals - previous,
                          std::memory_order_relaxed);
    }
  }
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

#if REX_PLATFORM_WIN32 && (defined(_M_X64) || defined(__x86_64__))
namespace {
// Time-stamp counter ticks a nanosecond, measured once against the
// performance counter over 20 ms (the TSC is invariant on the CPUs the port
// supports; QueryThreadCycleTime counts in it).
double TscTicksPerNs() {
  static const double rate = [] {
    LARGE_INTEGER frequency = {}, start = {}, now = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const uint64_t tsc_start = __rdtsc();
    do {
      QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < frequency.QuadPart / 50);
    const uint64_t tsc_end = __rdtsc();
    const double ns = double(now.QuadPart - start.QuadPart) * 1e9 / double(frequency.QuadPart);
    return ns > 0 ? double(tsc_end - tsc_start) / ns : 0.0;
  }();
  return rate;
}
}  // namespace
#endif

int64_t CurrentThreadCpuTimeNs() {
#if REX_PLATFORM_WIN32 && (defined(_M_X64) || defined(__x86_64__))
  // GetThreadTimes advances in scheduler ticks (15.6 ms), coarser than a
  // frame (DR-0.2); the thread's cycle count is exact (LS-0.3).
  ULONG64 cycles = 0;
  const double rate = TscTicksPerNs();
  if (rate > 0 && QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
    return int64_t(double(cycles) / rate);
  }
  return 0;
#elif REX_PLATFORM_WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    return 0;
  ULARGE_INTEGER kernel_time{}, user_time{};
  kernel_time.LowPart = kernel.dwLowDateTime;
  kernel_time.HighPart = kernel.dwHighDateTime;
  user_time.LowPart = user.dwLowDateTime;
  user_time.HighPart = user.dwHighDateTime;
  return int64_t(kernel_time.QuadPart + user_time.QuadPart) * 100;
#else
  timespec time = {};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) return 0;
  return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#endif
}

uint64_t ProcessResidentBytes() {
#if REX_PLATFORM_WIN32
  PROCESS_MEMORY_COUNTERS counters = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return counters.WorkingSetSize;
  }
  return 0;
#elif REX_PLATFORM_LINUX
  // statm: total and resident pages.
  std::ifstream statm("/proc/self/statm");
  uint64_t total = 0, resident = 0;
  if (statm >> total >> resident) {
    return resident * uint64_t(sysconf(_SC_PAGESIZE));
  }
  return 0;
#else
  return 0;
#endif
}

bool CriticalPathTraceEnabled() {
  static const bool log_enabled = REXCVAR_GET(perf_critical_path_trace);
  return log_enabled || CriticalPathEtwEnabled();
}

void TraceCriticalPath(std::string_view event, int64_t source_frame,
                       int64_t value0, int64_t value1, int64_t value2) {
  if (!CriticalPathTraceEnabled()) {
    return;
  }
#if REX_PLATFORM_WIN32
  if (event == "source_frame") {
    TraceLoggingWrite(g_critical_path_provider, "SourceFrame",
                      TraceLoggingInt64(source_frame, "SourceFrame"));
  }
  TraceLoggingWrite(g_critical_path_provider, "CriticalPath",
                    TraceLoggingCountedUtf8String(event.data(),
                                                  static_cast<ULONG>(event.size()),
                                                  "Event"),
                    TraceLoggingInt64(source_frame, "SourceFrame"),
                    TraceLoggingInt64(value0, "Value0"),
                    TraceLoggingInt64(value1, "Value1"),
                    TraceLoggingInt64(value2, "Value2"));
#endif
  static const bool log_enabled = REXCVAR_GET(perf_critical_path_trace);
  if (!log_enabled) {
    return;
  }
  const int64_t time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
  REXLOG_INFO(
      "CRITICAL_PATH {{\"event\":\"{}\",\"time_ns\":{},\"thread\":{},"
      "\"source_frame\":{},\"value0\":{},\"value1\":{},\"value2\":{}}}",
      event, time_ns, std::hash<std::thread::id>{}(std::this_thread::get_id()),
      source_frame, value0, value1, value2);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
  for (size_t i = 0; i < kNumCounters; ++i) {
    const int64_t totals = SumThreadTotals(i);
    g_init_totals[i].store(totals, std::memory_order_relaxed);
    g_snapshot_totals[i].store(totals, std::memory_order_relaxed);
  }
}

void SetCsvLogPath(const std::string& path) {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

#if REX_PLATFORM_WIN32
  // A discovery recorder may tail the CSV while the game is running.
  g_csv_file = _wfsopen(rex::to_path(path).c_str(), L"w", _SH_DENYWR);
#else
  g_csv_file = rex::filesystem::OpenFile(rex::to_path(path), "w");
#endif
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  // Write header
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fputs(kCounterNames[i], g_csv_file);
  }
  std::fputc('\n', g_csv_file);
}

void WriteCsvFrame() {
  if (!g_csv_file)
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fprintf(g_csv_file, "%lld",
                 static_cast<long long>(g_snapshot[i].load(std::memory_order_relaxed)));
  }
  std::fputc('\n', g_csv_file);

  if (++g_csv_frame_count % 60 == 0) {
    std::fflush(g_csv_file);
    const auto limit_mb = REXCVAR_GET(perf_log_max_mb);
    if (limit_mb > 0 && rex::filesystem::Tell(g_csv_file) >= int64_t(limit_mb) * 1024 * 1024) {
      REXLOG_WARN("perf: CSV recording size limit reached; game continues");
      FlushCsv();
    }
  }
}

void FlushCsv() {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path.clear();
}

}  // namespace rex::perf
