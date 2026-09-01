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
#include <cstdio>
#include <cstring>

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");

namespace rex::perf {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

std::array<std::atomic<int64_t>, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

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
    "zpd_reports_started",
    "zpd_reports_ended",
    "zpd_report_segments",
    "zpd_same_slot_reuse",
    "zpd_fast_speculative_writes",
    "zpd_async_result_patches",
    "zpd_strict_waits",
    "zpd_strict_wait_time_ns",
    "zpd_retire_timeouts",
    "zpd_fake_fallbacks",
    "zpd_malformed_records",
    "zpd_stale_result_rejections",
    "zpd_classified_begins",
    "zpd_classified_ends",
    "zpd_classified_orphaned_ends",
    "zpd_policy_fallbacks",
    "zpd_watchdog_recoveries",
    "guest_vblank_count",
    "guest_vblank_delta_ns",
    "simulation_tick_count",
    "present_count",
    "present_delta_ns",
    "present_queue_depth",
    "present_deadline_misses",
    "duplicate_present_count",
    "dropped_present_count",
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
    false,  // kZpdReportsStarted
    false,  // kZpdReportsEnded
    false,  // kZpdReportSegments
    false,  // kZpdSameSlotReuse
    false,  // kZpdFastSpeculativeWrites
    false,  // kZpdAsyncResultPatches
    false,  // kZpdStrictWaits
    false,  // kZpdStrictWaitTimeNs
    false,  // kZpdRetireTimeouts
    false,  // kZpdFakeFallbacks
    false,  // kZpdMalformedRecords
    false,  // kZpdStaleResultRejections
    false,  // kZpdClassifiedBegins
    false,  // kZpdClassifiedEnds
    false,  // kZpdClassifiedOrphanedEnds
    false,  // kZpdPolicyFallbacks
    false,  // kZpdWatchdogRecoveries
    false,  // kGuestVblankCount
    false,  // kGuestVblankDeltaNs
    false,  // kSimulationTickCount
    false,  // kPresentCount
    false,  // kPresentDeltaNs
    false,  // kPresentQueueDepth
    false,  // kPresentDeadlineMisses
    false,  // kDuplicatePresentCount
    false,  // kDroppedPresentCount
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
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

int64_t GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot and zero for next frame
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
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

  g_csv_file = rex::filesystem::OpenFile(rex::to_path(path), "w");
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
