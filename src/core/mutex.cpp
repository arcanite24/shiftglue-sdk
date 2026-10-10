/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/thread/mutex.h>

#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define REX_CRITICAL_REGION_DLADDR 1
#endif

REXCVAR_DEFINE_INT32(critical_region_wait_log_ms, 0, "Diagnostics",
                     "Log every wait for the global critical region at least this long, with "
                     "the code addresses (module and offset) of the waiter's callers; 0 logs "
                     "none")
    .range(0, 100000);
REXCVAR_DEFINE_INT32(critical_region_hold_log_ms, 0, "Diagnostics",
                     "Log every hold of the global critical region at least this long: "
                     "recompiled guest sections with the function that entered them, and "
                     "on Android any holder's thread id; 0 logs none")
    .range(0, 100000);

namespace rex::thread {
namespace {
std::string DescribeSite(const void* site) {
#ifdef REX_CRITICAL_REGION_DLADDR
  Dl_info info = {};
  if (site && dladdr(site, &info) && info.dli_fname) {
    const char* name = info.dli_fname;
    for (const char* c = name; *c; ++c) {
      if (*c == '/') name = c + 1;
    }
    return fmt::format("{}+0x{:x}", name, uintptr_t(site) - uintptr_t(info.dli_fbase));
  }
#endif
  return fmt::format("{}", site);
}
constexpr size_t kSiteDepth = 4;

// The return addresses of the waiter's callers (frame pointers are kept on
// arm64 Android).
template <size_t... I>
void CaptureSites(std::array<const void*, kSiteDepth>& out, std::index_sequence<I...>) {
#if defined(__clang__) || defined(__GNUC__)
  ((out[I] = __builtin_return_address(I + 1)), ...);
#else
  out = {};
#endif
}
}  // namespace

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void global_critical_region::LockContended() {
  PROFILE_CRITICAL_REGION_CONTENTION();
  const auto start = std::chrono::steady_clock::now();
  mutex().lock();
  const int64_t waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  PERF_counter_add(kCriticalRegionBlockedNs, waited_ns);
  const int32_t log_ms = REXCVAR_GET(critical_region_wait_log_ms);
  if (log_ms && waited_ns >= int64_t(log_ms) * 1000000) {
    std::array<const void*, kSiteDepth> site;
    CaptureSites(site, std::make_index_sequence<kSiteDepth>());
    std::string sites;
    for (const void* address : site) sites += (sites.empty() ? "" : " < ") + DescribeSite(address);
    REXLOG_INFO("Critical region wait {:.1f} ms at {}", double(waited_ns) / 1e6, sites);
  }
}

#if defined(__ANDROID__) && defined(__LP64__)
namespace {
// Logs every hold by any thread (host code included) at least log_ms long,
// polling the owner bionic keeps in recursive mutexes, with CLOCK_MONOTONIC
// times to match simpleperf samples of the holder.
void HoldWatchdog(int32_t log_ms) {
  struct BionicMutex {
    std::atomic<uint16_t> state;
    uint16_t pad;
    std::atomic<int> owner_tid;
  };
  static_assert(sizeof(BionicMutex) <= sizeof(pthread_mutex_t));
  auto* m = reinterpret_cast<BionicMutex*>(global_critical_region::mutex().native_handle());
  const auto now_ns = [] {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
  };
  int owner = 0;
  int64_t since = 0;
  for (;;) {
    usleep(500);
    const int current = m->owner_tid.load(std::memory_order_relaxed);
    const int64_t now = now_ns();
    if (current == owner) continue;
    if (owner && now - since >= int64_t(log_ms) * 1000000) {
      REXLOG_INFO("Critical region held {:.1f} ms by tid {} from monotonic {}",
                  double(now - since) / 1e6, owner, since);
    }
    owner = current;
    since = now;
  }
}
}  // namespace
#endif

void global_critical_region::GuestHoldLogInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    const int32_t log_ms = REXCVAR_GET(critical_region_hold_log_ms);
#if defined(__ANDROID__) && defined(__LP64__)
    if (log_ms > 0) std::thread(HoldWatchdog, log_ms).detach();
#endif
    guest_hold_log_ms_ = log_ms;
  });
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void global_critical_region::GuestHoldBegin() {
#if defined(__clang__) || defined(__GNUC__)
  guest_hold_site_ = __builtin_return_address(0);
#endif
  guest_hold_start_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
}

void global_critical_region::GuestHoldEnd() {
  const int64_t held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() -
                          guest_hold_start_ns_;
  guest_hold_start_ns_ = 0;
  if (held_ns >= int64_t(guest_hold_log_ms_) * 1000000) {
    REXLOG_INFO("Critical region held {:.1f} ms by guest code at {}", double(held_ns) / 1e6,
                DescribeSite(guest_hold_site_));
  }
}

}  // namespace rex::thread