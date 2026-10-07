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

#include <thread>

#include <rex/thread.h>

namespace rex::thread {

// =============================================================================
// Common code
// =============================================================================

namespace {
uint32_t g_logical_processor_override = 0;
}  // namespace

uint32_t logical_processor_count() {
  if (g_logical_processor_override) {
    return g_logical_processor_override;
  }
  static uint32_t value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

void SetLogicalProcessorCountOverride(uint32_t count) {
  g_logical_processor_override = count;
}

thread_local uint32_t current_thread_id_ = UINT_MAX;

uint32_t current_thread_id() {
  return current_thread_id_ == UINT_MAX ? current_thread_system_id() : current_thread_id_;
}

void set_current_thread_id(uint32_t id) {
  current_thread_id_ = id;
}

}  // namespace rex::thread
