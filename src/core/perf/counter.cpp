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

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#if REX_PLATFORM_WIN32
#include <share.h>
#include <windows.h>
#include <TraceLoggingProvider.h>
#endif

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");
REXCVAR_DEFINE_INT32(perf_log_max_mb, 0, "Perf",
                    "Stop CSV recording at this size (0 = unlimited)");
REXCVAR_DEFINE_BOOL(perf_critical_path_trace, false, "Perf",
                    "Log source-frame critical-path events")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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

void ResetFrameCounters() {
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

int64_t CurrentThreadCpuTimeNs() {
#if REX_PLATFORM_WIN32
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
