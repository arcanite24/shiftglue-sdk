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
#include <chrono>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/thread/mutex.h>

#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#define REX_CRITICAL_REGION_DLADDR 1
#endif

REXCVAR_DEFINE_INT32(critical_region_wait_log_ms, 0, "Diagnostics",
                     "Log every wait for the global critical region at least this long, with "
                     "the code addresses (module and offset) of the waiter's callers; 0 logs "
                     "none")
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

}  // namespace rex::thread
