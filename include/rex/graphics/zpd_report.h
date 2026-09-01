/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Guest-memory layout helpers for Xenos ZPD reports. A slot contains an END
// record at +0x00 and a BEGIN record at +0x20.
struct XenosZPDReport {
  static constexpr uint32_t kRecordSizeBytes = 0x20;
  static constexpr uint32_t kRecordAlignMask = ~(kRecordSizeBytes - 1);
  static constexpr uint32_t kSlotSizeBytes = 0x40;
  static constexpr uint32_t kSlotAlignMask = ~(kSlotSizeBytes - 1);

  static constexpr uint32_t GetRecordBase(uint32_t address) { return address & kRecordAlignMask; }
  static constexpr uint32_t GetSlotBase(uint32_t address) { return address & kSlotAlignMask; }
  static constexpr uint32_t GetBeginRecordBase(uint32_t address) {
    return GetSlotBase(address) + kRecordSizeBytes;
  }
  static constexpr uint32_t GetEndRecordBase(uint32_t address) { return GetSlotBase(address); }
  static constexpr bool IsBeginRecord(uint32_t address) {
    uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetBeginRecordBase(record_base);
  }
  static constexpr bool IsEndRecord(uint32_t address) {
    uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetEndRecordBase(record_base);
  }

  static bool HasPendingSentinel(const xenos::xe_gpu_depth_sample_counts* report) {
    constexpr uint32_t kSentinelLE = 0xEDFEFFFFu;
    constexpr uint32_t kSentinelBE = 0xFFFFFEEDu;
    return report && (report->ZPass_A == kSentinelLE || report->ZPass_A == kSentinelBE ||
                      report->ZFail_A == kSentinelLE || report->ZFail_A == kSentinelBE);
  }

  static bool HasPairwisePendingSentinel(const xenos::xe_gpu_depth_sample_counts* report) {
    constexpr uint32_t kSentinelLE = 0xEDFEFFFFu;
    constexpr uint32_t kSentinelBE = 0xFFFFFEEDu;
    auto is_sentinel = [](uint32_t value) { return value == kSentinelLE || value == kSentinelBE; };
    return report && ((is_sentinel(report->ZPass_A) && is_sentinel(report->ZPass_B)) ||
                      (is_sentinel(report->ZFail_A) && is_sentinel(report->ZFail_B)));
  }

  static void WriteSampleCount(xenos::xe_gpu_depth_sample_counts* report, uint32_t sample_count) {
    if (!report) {
      return;
    }
    report->Total_A = sample_count;
    report->Total_B = 0;
    report->ZFail_A = 0;
    report->ZFail_B = 0;
    report->ZPass_A = sample_count;
    report->ZPass_B = 0;
    report->StencilFail_A = 0;
    report->StencilFail_B = 0;
  }

  static void WriteReportDelta(xenos::xe_gpu_depth_sample_counts* begin_report,
                               xenos::xe_gpu_depth_sample_counts* end_report, uint32_t begin_value,
                               uint32_t delta_value, bool write_begin_report) {
    uint32_t end_value = begin_value + delta_value;
    if (write_begin_report && begin_report && begin_report != end_report) {
      WriteSampleCount(begin_report, begin_value);
    }
    WriteSampleCount(end_report, end_value);
  }
};

}  // namespace rex::graphics
