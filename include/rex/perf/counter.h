/**
 * @file        perf/counter.h
 * @brief       Performance counter registry and profiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

namespace rex::perf {

enum class CounterId : uint16_t {
  // Frame
  kFrameTimeUs,
  kFps,

  // GPU
  kDrawCalls,
  kCommandBufferStalls,
  kVerticesProcessed,
  kGuestFrameGpuTimeNs,
  kNativeCompositionGpuTimeNs,
  kNativeSelectionGpuTimeNs,
  kGuestFrameGpuTimingSamples,
  kNativeCompositionGpuTimingSamples,
  kNativeSelectionGpuTimingSamples,
  kNativeGpuTimingDrops,

  // Audio
  kXmaFramesDecoded,
  kXmaNoSpaceStalls,
  kXmaNoProgressStalls,
  kXmaStallRecoveries,
  kAudioFrameLatencyUs,
  kBufferQueueDepth,

  // Dispatch
  kFunctionsDispatched,
  kInterruptDispatches,

  // Threading
  kActiveThreads,
  kApcQueueDepth,
  kCriticalRegionContentions,

  // Caches
  kTextureCacheHits,
  kTextureCacheMisses,
  kPipelineCacheHits,
  kPipelineCacheMisses,

  // D3D12 memexport coherency
  kMemexportDraws,
  kMemexportBytes,
  kMemexportSyncFallbacks,
  kMemexportQueueWaits,
  kMemexportFenceWaits,

  // D3D12 resolve readback
  kResolveReadbackRequests,
  kResolveReadbackBytes,
  kResolveReadbackFastCopies,
  kResolveReadbackCacheMisses,
  kResolveReadbackFullWaits,
  kResolveReadbackWaitTimeNs,

  // D3D12 ZPD report lifecycle
  kZpdReportsStarted,
  kZpdReportsEnded,
  kZpdReportSegments,
  kZpdSameSlotReuse,
  kZpdFastSpeculativeWrites,
  kZpdAsyncResultPatches,
  kZpdStrictWaits,
  kZpdStrictWaitTimeNs,
  kZpdRetireTimeouts,
  kZpdFakeFallbacks,
  kZpdMalformedRecords,
  kZpdStaleResultRejections,
  kZpdClassifiedBegins,
  kZpdClassifiedEnds,
  kZpdClassifiedOrphanedEnds,
  kZpdPolicyFallbacks,
  kZpdWatchdogRecoveries,

  // Guest timing and host presentation pacing
  kGuestVblankCount,
  kGuestVblankDeltaNs,
  kSimulationTickCount,
  kSimulationTimeNs,
  kSimulationDeltaInvalid,
  kSourceFrameCount,
  kPresentCount,
  kPresentDeltaNs,
  kPresentQueueDepth,
  kPresentDeadlineMisses,
  kDuplicatePresentCount,
  kDroppedPresentCount,

  kCount  // sentinel -- must be last
};

// Returns human-readable name for a counter (e.g. "frame_time_us")
const char* CounterName(CounterId id);

// Set a counter to an absolute value
void SetCounter(CounterId id, int64_t value);

// Atomically add to a counter
void IncrementCounter(CounterId id, int64_t delta = 1);

// Read a counter's current live value
int64_t GetCounter(CounterId id);

// Read the monotonic total of values added through IncrementCounter. Totals
// are not reset by the per-frame CSV snapshot.
int64_t GetTotalCounter(CounterId id);

// Snapshot current values into the read buffer and zero the live counters.
// Called once per frame by Profiler::Flip().
void ResetFrameCounters();

// Read a counter from the last-frame snapshot (stable between frames).
int64_t GetSnapshotCounter(CounterId id);

// Initialize the counter system (zeroes everything). Safe to call multiple times.
void Init();

// CSV logging
void SetCsvLogPath(const std::string& path);
void WriteCsvFrame();
void FlushCsv();

// Profiler -- coordinates Tracy frame marks and counter snapshots.
// Moved here from rex::debug to consolidate all perf code under rex::perf.
class Profiler {
 public:
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (!tracy::IsProfilerStarted())
      tracy::StartupProfiler();
#endif
  }
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name && tracy::IsProfilerStarted())
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }
  static void ThreadExit() {}
  static void Flip() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted()) {
      FrameMark;
    }
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ResetFrameCounters();
    WriteCsvFrame();
#endif
  }
  static void Flush() {}
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted())
      tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCsv();
#endif
  }
  static bool is_enabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::perf

// Perf counter macros -- compile to no-ops when counters are disabled.
#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Generic helpers for easily adding new counters
#define PERF_counter_set(id, value) rex::perf::SetCounter(rex::perf::CounterId::id, value)
#define PERF_counter_inc(id) rex::perf::IncrementCounter(rex::perf::CounterId::id)
#define PERF_counter_add(id, delta) rex::perf::IncrementCounter(rex::perf::CounterId::id, delta)

// Purpose-specific macros so callsites stay clean
#define PROFILE_FRAME_TIME_US(value) PERF_counter_set(kFrameTimeUs, value)
#define PROFILE_FPS(value) PERF_counter_set(kFps, value)
#define PROFILE_FUNCTION_DISPATCHED() PERF_counter_inc(kFunctionsDispatched)
#define PROFILE_INTERRUPT_DISPATCHED() PERF_counter_inc(kInterruptDispatches)
#define PROFILE_XMA_FRAME_DECODED() PERF_counter_inc(kXmaFramesDecoded)
#define PROFILE_XMA_NO_SPACE_STALL() PERF_counter_inc(kXmaNoSpaceStalls)
#define PROFILE_XMA_NO_PROGRESS_STALL() PERF_counter_inc(kXmaNoProgressStalls)
#define PROFILE_XMA_STALL_RECOVERY() PERF_counter_inc(kXmaStallRecoveries)
#define PROFILE_DRAW_CALL() PERF_counter_inc(kDrawCalls)
#define PROFILE_VERTICES(n) PERF_counter_add(kVerticesProcessed, n)
#define PROFILE_CMD_BUFFER_STALL() PERF_counter_inc(kCommandBufferStalls)
#define PROFILE_GUEST_FRAME_GPU_TIME_NS(n) PERF_counter_add(kGuestFrameGpuTimeNs, n)
#define PROFILE_NATIVE_COMPOSITION_GPU_TIME_NS(n) \
  PERF_counter_add(kNativeCompositionGpuTimeNs, n)
#define PROFILE_NATIVE_SELECTION_GPU_TIME_NS(n) \
  PERF_counter_add(kNativeSelectionGpuTimeNs, n)
#define PROFILE_GUEST_FRAME_GPU_TIMING_SAMPLE() \
  PERF_counter_inc(kGuestFrameGpuTimingSamples)
#define PROFILE_NATIVE_COMPOSITION_GPU_TIMING_SAMPLE() \
  PERF_counter_inc(kNativeCompositionGpuTimingSamples)
#define PROFILE_NATIVE_SELECTION_GPU_TIMING_SAMPLE() \
  PERF_counter_inc(kNativeSelectionGpuTimingSamples)
#define PROFILE_NATIVE_GPU_TIMING_DROP() PERF_counter_inc(kNativeGpuTimingDrops)
#define PROFILE_AUDIO_LATENCY_US(value) PERF_counter_set(kAudioFrameLatencyUs, value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value) PERF_counter_set(kBufferQueueDepth, value)
#define PROFILE_THREAD_CREATED() PERF_counter_inc(kActiveThreads)
#define PROFILE_THREAD_EXITED() PERF_counter_add(kActiveThreads, -1)
#define PROFILE_APC_QUEUE_DEPTH(value) PERF_counter_set(kApcQueueDepth, value)
#define PROFILE_CRITICAL_REGION_CONTENTION() PERF_counter_inc(kCriticalRegionContentions)
#define PROFILE_TEXTURE_CACHE_HIT() PERF_counter_inc(kTextureCacheHits)
#define PROFILE_TEXTURE_CACHE_MISS() PERF_counter_inc(kTextureCacheMisses)
#define PROFILE_PIPELINE_CACHE_HIT() PERF_counter_inc(kPipelineCacheHits)
#define PROFILE_PIPELINE_CACHE_MISS() PERF_counter_inc(kPipelineCacheMisses)
#define PROFILE_MEMEXPORT_DRAW() PERF_counter_inc(kMemexportDraws)
#define PROFILE_MEMEXPORT_BYTES(n) PERF_counter_add(kMemexportBytes, n)
#define PROFILE_MEMEXPORT_SYNC_FALLBACK() PERF_counter_inc(kMemexportSyncFallbacks)
#define PROFILE_MEMEXPORT_QUEUE_WAIT() PERF_counter_inc(kMemexportQueueWaits)
#define PROFILE_MEMEXPORT_FENCE_WAIT() PERF_counter_inc(kMemexportFenceWaits)
#define PROFILE_RESOLVE_READBACK_REQUEST() PERF_counter_inc(kResolveReadbackRequests)
#define PROFILE_RESOLVE_READBACK_BYTES(n) PERF_counter_add(kResolveReadbackBytes, n)
#define PROFILE_RESOLVE_READBACK_FAST_COPY() PERF_counter_inc(kResolveReadbackFastCopies)
#define PROFILE_RESOLVE_READBACK_CACHE_MISS() PERF_counter_inc(kResolveReadbackCacheMisses)
#define PROFILE_RESOLVE_READBACK_FULL_WAIT() PERF_counter_inc(kResolveReadbackFullWaits)
#define PROFILE_RESOLVE_READBACK_WAIT_TIME_NS(n) PERF_counter_add(kResolveReadbackWaitTimeNs, n)
#define PROFILE_GUEST_VBLANK() PERF_counter_inc(kGuestVblankCount)
#define PROFILE_GUEST_VBLANK_DELTA_NS(value) PERF_counter_set(kGuestVblankDeltaNs, value)
#define PROFILE_SIMULATION_TICK() PERF_counter_inc(kSimulationTickCount)
#define PROFILE_SIMULATION_TIME_NS(value) PERF_counter_add(kSimulationTimeNs, value)
#define PROFILE_SIMULATION_DELTA_INVALID() PERF_counter_inc(kSimulationDeltaInvalid)
#define PROFILE_SOURCE_FRAME() PERF_counter_inc(kSourceFrameCount)
#define PROFILE_PRESENT() PERF_counter_inc(kPresentCount)
#define PROFILE_PRESENT_DELTA_NS(value) PERF_counter_set(kPresentDeltaNs, value)
#define PROFILE_PRESENT_QUEUE_DEPTH(value) PERF_counter_set(kPresentQueueDepth, value)
#define PROFILE_PRESENT_DEADLINE_MISS(n) PERF_counter_add(kPresentDeadlineMisses, n)
#define PROFILE_DUPLICATE_PRESENT() PERF_counter_inc(kDuplicatePresentCount)
#define PROFILE_DROPPED_PRESENT() PERF_counter_inc(kDroppedPresentCount)

#else

#define PERF_counter_set(id, value)
#define PERF_counter_inc(id)
#define PERF_counter_add(id, delta)

#define PROFILE_FRAME_TIME_US(value)
#define PROFILE_FPS(value)
#define PROFILE_FUNCTION_DISPATCHED()
#define PROFILE_INTERRUPT_DISPATCHED()
#define PROFILE_XMA_FRAME_DECODED()
#define PROFILE_XMA_NO_SPACE_STALL()
#define PROFILE_XMA_NO_PROGRESS_STALL()
#define PROFILE_XMA_STALL_RECOVERY()
#define PROFILE_DRAW_CALL()
#define PROFILE_VERTICES(n)
#define PROFILE_CMD_BUFFER_STALL()
#define PROFILE_GUEST_FRAME_GPU_TIME_NS(n)
#define PROFILE_NATIVE_COMPOSITION_GPU_TIME_NS(n)
#define PROFILE_NATIVE_SELECTION_GPU_TIME_NS(n)
#define PROFILE_GUEST_FRAME_GPU_TIMING_SAMPLE()
#define PROFILE_NATIVE_COMPOSITION_GPU_TIMING_SAMPLE()
#define PROFILE_NATIVE_SELECTION_GPU_TIMING_SAMPLE()
#define PROFILE_NATIVE_GPU_TIMING_DROP()
#define PROFILE_AUDIO_LATENCY_US(value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value)
#define PROFILE_THREAD_CREATED()
#define PROFILE_THREAD_EXITED()
#define PROFILE_APC_QUEUE_DEPTH(value)
#define PROFILE_CRITICAL_REGION_CONTENTION()
#define PROFILE_MEMEXPORT_DRAW()
#define PROFILE_MEMEXPORT_BYTES(n)
#define PROFILE_MEMEXPORT_SYNC_FALLBACK()
#define PROFILE_MEMEXPORT_QUEUE_WAIT()
#define PROFILE_MEMEXPORT_FENCE_WAIT()
#define PROFILE_RESOLVE_READBACK_REQUEST()
#define PROFILE_RESOLVE_READBACK_BYTES(n)
#define PROFILE_RESOLVE_READBACK_FAST_COPY()
#define PROFILE_RESOLVE_READBACK_CACHE_MISS()
#define PROFILE_RESOLVE_READBACK_FULL_WAIT()
#define PROFILE_RESOLVE_READBACK_WAIT_TIME_NS(n)
#define PROFILE_TEXTURE_CACHE_HIT()
#define PROFILE_TEXTURE_CACHE_MISS()
#define PROFILE_PIPELINE_CACHE_HIT()
#define PROFILE_PIPELINE_CACHE_MISS()
#define PROFILE_GUEST_VBLANK()
#define PROFILE_GUEST_VBLANK_DELTA_NS(value)
#define PROFILE_SIMULATION_TICK()
#define PROFILE_SIMULATION_TIME_NS(value)
#define PROFILE_SIMULATION_DELTA_INVALID()
#define PROFILE_SOURCE_FRAME()
#define PROFILE_PRESENT()
#define PROFILE_PRESENT_DELTA_NS(value)
#define PROFILE_PRESENT_QUEUE_DEPTH(value)
#define PROFILE_PRESENT_DEADLINE_MISS(n)
#define PROFILE_DUPLICATE_PRESENT()
#define PROFILE_DROPPED_PRESENT()

#endif
