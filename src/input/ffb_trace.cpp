/**
 * @file        input/ffb_trace.cpp
 * @brief       CSV trace of the title's force feedback traffic (FB-2.1).
 *
 * @license     BSD 3-Clause License
 */

#include <rex/input/ffb_trace.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

REXCVAR_DEFINE_BOOL(wheel_ffb_trace, false, "Input",
                    "Write the title's force feedback calls (motor values, caller, wheel "
                    "steering and pedals, host force) to a CSV beside the runtime log")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_ffb_trace_file, "", "Input",
                      "Path of the force feedback trace; empty writes ffb-trace-<time>.csv "
                      "beside the runtime log");

namespace rex::input::ffb_trace {

namespace {

using Clock = std::chrono::steady_clock;

struct Trace {
  std::mutex mutex;
  std::ofstream file;
  bool open_failed = false;
  Clock::time_point start;
  Clock::time_point last_flush;
  uint64_t rows = 0;
  std::map<uint32_t, std::tuple<uint32_t, uint32_t, uint8_t, uint8_t, uint16_t>> caps_by_caller;
  std::map<uint32_t, uint32_t> level_by_caller;
};

Trace& GetTrace() {
  static Trace trace;
  return trace;
}

// The wheel's latest state, written by the SDL driver under its own lock.
std::atomic<int16_t> g_steering{0};
std::atomic<uint8_t> g_throttle{0};
std::atomic<uint8_t> g_brake{0};
std::atomic<uint16_t> g_buttons{0};
std::atomic<int16_t> g_force{0};
std::atomic<const char*> g_mode{"none"};

std::filesystem::path TracePath() {
  const std::string& configured = REXCVAR_GET(wheel_ffb_trace_file);
  if (!configured.empty()) {
    return rex::to_path(configured);
  }
  std::filesystem::path directory;
  const std::string& log = REXCVAR_GET(log_file);
  if (!log.empty()) {
    directory = rex::to_path(log).parent_path();
  }
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  return directory / fmt::format("ffb-trace-{}.csv", stamp);
}

// Opens the file on first use. Caller holds trace.mutex.
bool EnsureOpenLocked(Trace& trace) {
  if (trace.file.is_open()) {
    return true;
  }
  if (trace.open_failed) {
    return false;
  }
  const std::filesystem::path path = TracePath();
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
  }
  trace.file.open(path, std::ios::out | std::ios::trunc);
  if (!trace.file) {
    trace.open_failed = true;
    REXLOG_WARN("FFB trace: could not open {}", rex::path_to_utf8(path));
    return false;
  }
  trace.start = Clock::now();
  trace.last_flush = trace.start;
  trace.file << "t_ms,event,user,caller,left,right,arg,extra,result,steering,throttle,brake,"
                "buttons,host_force,mode,detail\n";
  REXLOG_INFO("FFB trace: writing {}", rex::path_to_utf8(path));
  return true;
}

void WriteRowLocked(Trace& trace, std::string_view event, uint32_t user, uint32_t caller,
                    uint32_t left, uint32_t right, uint32_t arg, uint32_t extra, uint32_t result,
                    std::string_view detail = {}) {
  const auto now = Clock::now();
  const double t_ms = std::chrono::duration<double, std::milli>(now - trace.start).count();
  trace.file << fmt::format(
      "{:.3f},{},{},{:08X},{},{},{:08X},{:08X},{:08X},{},{},{},{:04X},{},{},\"{}\"\n", t_ms, event,
      user, caller, left, right, arg, extra, result, g_steering.load(std::memory_order_relaxed),
      g_throttle.load(std::memory_order_relaxed), g_brake.load(std::memory_order_relaxed),
      g_buttons.load(std::memory_order_relaxed), g_force.load(std::memory_order_relaxed),
      g_mode.load(std::memory_order_relaxed), detail);
  trace.rows++;
  // Flushed often enough that a crash loses at most a quarter second.
  if (now - trace.last_flush >= std::chrono::milliseconds(250)) {
    trace.file.flush();
    trace.last_flush = now;
  }
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(wheel_ffb_trace);
}

void NoteWheelInput(int16_t steering, uint8_t throttle, uint8_t brake, uint16_t buttons) {
  g_steering.store(steering, std::memory_order_relaxed);
  g_throttle.store(throttle, std::memory_order_relaxed);
  g_brake.store(brake, std::memory_order_relaxed);
  g_buttons.store(buttons, std::memory_order_relaxed);
}

void NoteWheelOutput(int16_t force, const char* mode) {
  g_force.store(force, std::memory_order_relaxed);
  g_mode.store(mode, std::memory_order_relaxed);
}

void RecordSetState(uint32_t user, uint32_t arg, uint16_t left, uint16_t right, uint32_t extra,
                    uint32_t caller, uint32_t result) {
  if (!Enabled()) {
    return;
  }
  Trace& trace = GetTrace();
  std::lock_guard lock(trace.mutex);
  if (!EnsureOpenLocked(trace)) {
    return;
  }
  WriteRowLocked(trace, "set", user, caller, left, right, arg, extra, result);
}

void RecordCapabilities(uint32_t user, uint32_t flags, uint32_t result, uint8_t type,
                        uint8_t sub_type, uint16_t caps_flags, uint32_t caller) {
  if (!Enabled()) {
    return;
  }
  Trace& trace = GetTrace();
  std::lock_guard lock(trace.mutex);
  const auto key = std::make_tuple(user, result, type, sub_type, caps_flags);
  auto it = trace.caps_by_caller.find(caller);
  if (it != trace.caps_by_caller.end() && it->second == key) {
    return;
  }
  trace.caps_by_caller[caller] = key;
  if (!EnsureOpenLocked(trace)) {
    return;
  }
  // left/right carry the device type and subtype, arg the query flags and
  // extra the capability flags (bit 0: force feedback).
  WriteRowLocked(trace, "caps", user, caller, type, sub_type, flags, caps_flags, result);
  REXLOG_INFO(
      "FFB trace: caps for user {} from {:08X}: result {:08X} type {} subtype {} flags "
      "{:04X}",
      user, caller, result, type, sub_type, caps_flags);
}

void RecordVibrationLevel(uint32_t user, uint32_t level, uint32_t caller) {
  if (!Enabled()) {
    return;
  }
  Trace& trace = GetTrace();
  std::lock_guard lock(trace.mutex);
  auto it = trace.level_by_caller.find(caller);
  if (it != trace.level_by_caller.end() && it->second == level) {
    return;
  }
  trace.level_by_caller[caller] = level;
  if (!EnsureOpenLocked(trace)) {
    return;
  }
  WriteRowLocked(trace, "vibration_level", user, caller, level, 0, 0, 0, 0);
}

void RecordWheelCall(const char* event, uint32_t caller, uint32_t a, uint32_t b, uint32_t c,
                     uint32_t result, const std::string& detail) {
  if (!Enabled()) {
    return;
  }
  Trace& trace = GetTrace();
  std::lock_guard lock(trace.mutex);
  if (!EnsureOpenLocked(trace)) {
    return;
  }
  // left/right/arg carry the call's first arguments; detail the decoded effect.
  WriteRowLocked(trace, event, 0, caller, a, b, c, 0, result, detail);
}

}  // namespace rex::input::ffb_trace
