#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include <rex/graphics/zpd_report.h>

namespace rex::graphics {

enum class ZPDMode {
  kLegacy,
  kFake,
  kFast,
  kStrict,
};

// Backend-independent logical ZPD state. Host query allocation and retirement
// stay in the renderer, while this controller owns guest slot lifetimes and
// rejects stale asynchronous results after same-slot reuse.
class ZPDLifecycle {
 public:
  using ReportHandle = uint64_t;
  static constexpr ReportHandle kInvalidReportHandle = 0;

  struct Report {
    uint64_t accumulated_samples = 0;
    uint64_t first_submission = 0;
    uint64_t last_submission = 0;
    uint64_t slot_sequence_id = 0;
    uint32_t slot_base = 0;
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    uint32_t begin_value = 0;
    uint32_t cached_delta = 0;
    uint32_t pending_segments = 0;
    bool has_cached_delta = false;
    bool ended = false;
  };

  struct ActiveSegment {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t slot_base = 0;
    bool segment_active = false;
    bool segment_pending_begin = false;
    bool logical_active = false;
  };

  struct BeginResult {
    ReportHandle report_handle = kInvalidReportHandle;
    uint64_t slot_sequence_id = 0;
    bool same_slot_reuse = false;
  };

  BeginResult Begin(uint32_t report_address) {
    uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
    if (!slot_base) {
      return {};
    }
    bool same_slot_reuse = HasPendingSlot(slot_base);
    uint64_t sequence = ++slot_sequences_[slot_base];
    ReportHandle handle = next_report_handle_++;
    if (handle == kInvalidReportHandle) {
      handle = next_report_handle_++;
    }
    Report& report = reports_[handle];
    report.slot_base = slot_base;
    report.slot_sequence_id = sequence;
    report.begin_record = XenosZPDReport::GetBeginRecordBase(slot_base);
    report.end_record = XenosZPDReport::GetEndRecordBase(slot_base);
    report.begin_value = slot_values_[slot_base];
    auto cached = cached_deltas_.find(report.end_record);
    if (cached != cached_deltas_.end()) {
      report.cached_delta = cached->second;
      report.has_cached_delta = true;
    }
    active_ = {handle, slot_base, false, true, true};
    return {handle, sequence, same_slot_reuse};
  }

  bool End(uint32_t report_address) {
    Report* report = active_report();
    if (!report) {
      return false;
    }
    uint32_t record = XenosZPDReport::GetRecordBase(report_address);
    if (record) {
      report->end_record = record;
    }
    report->ended = true;
    active_.logical_active = false;
    active_.segment_pending_begin = false;
    return true;
  }

  bool SegmentOpened() {
    if (!active_.logical_active || !active_.segment_pending_begin) {
      return false;
    }
    active_.segment_active = true;
    active_.segment_pending_begin = false;
    return true;
  }

  bool SegmentClosed(uint64_t submission) {
    Report* report = active_report();
    if (!report || !active_.segment_active) {
      return false;
    }
    if (!report->pending_segments) {
      report->first_submission = submission;
    }
    report->last_submission = submission;
    ++report->pending_segments;
    active_.segment_active = false;
    active_.segment_pending_begin = active_.logical_active;
    return true;
  }

  bool Resolve(ReportHandle handle, uint64_t raw_samples, uint32_t scale_x, uint32_t scale_y,
               uint32_t& delta_out, bool& commit_out, Report& report_out) {
    auto it = reports_.find(handle);
    if (it == reports_.end()) {
      return false;
    }
    Report& report = it->second;
    if (report.pending_segments) {
      --report.pending_segments;
    }
    report.accumulated_samples += raw_samples;
    report_out = report;
    delta_out = Normalize(report.accumulated_samples, scale_x, scale_y);
    bool current = IsCurrent(report);
    commit_out = report.ended && report.pending_segments == 0 && current;
    if (report.ended && report.pending_segments == 0) {
      if (current) {
        cached_deltas_[report.end_record] = delta_out;
        slot_values_[report.slot_base] = report.begin_value + delta_out;
      }
      report_out = report;
      reports_.erase(it);
    }
    return true;
  }

  bool Abandon(ReportHandle handle, uint32_t fallback_delta, Report& report_out, bool& commit_out) {
    auto it = reports_.find(handle);
    if (it == reports_.end()) {
      return false;
    }
    report_out = it->second;
    commit_out = IsCurrent(report_out);
    if (commit_out) {
      slot_values_[report_out.slot_base] = report_out.begin_value + fallback_delta;
    }
    reports_.erase(it);
    if (active_.report_handle == handle) {
      active_ = {};
    }
    return true;
  }

  Report* active_report() { return Find(active_.report_handle); }
  const Report* active_report() const { return Find(active_.report_handle); }
  Report* Find(ReportHandle handle) {
    auto it = reports_.find(handle);
    return it == reports_.end() ? nullptr : &it->second;
  }
  const Report* Find(ReportHandle handle) const {
    auto it = reports_.find(handle);
    return it == reports_.end() ? nullptr : &it->second;
  }
  const ActiveSegment& active() const { return active_; }
  ActiveSegment& active() { return active_; }
  bool IsCurrent(const Report& report) const {
    auto it = slot_sequences_.find(report.slot_base);
    return it != slot_sequences_.end() && it->second == report.slot_sequence_id;
  }
  bool HasPendingSlot(uint32_t slot_base) const {
    for (const auto& item : reports_) {
      if (item.second.slot_base == slot_base) {
        return true;
      }
    }
    return false;
  }
  ReportHandle OldestPendingSlot(uint32_t slot_base) const {
    ReportHandle oldest = kInvalidReportHandle;
    for (const auto& item : reports_) {
      if (item.second.slot_base == slot_base && item.second.pending_segments &&
          (oldest == kInvalidReportHandle || item.first < oldest)) {
        oldest = item.first;
      }
    }
    return oldest;
  }
  uint32_t CachedDelta(uint32_t end_record, uint32_t fallback = 1) const {
    auto it = cached_deltas_.find(end_record);
    return it == cached_deltas_.end() ? fallback : it->second;
  }
  void Reset() {
    next_report_handle_ = 1;
    slot_sequences_.clear();
    slot_values_.clear();
    cached_deltas_.clear();
    reports_.clear();
    active_ = {};
  }

  static uint32_t Normalize(uint64_t samples, uint32_t scale_x, uint32_t scale_y) {
    uint64_t scale = uint64_t(std::max(scale_x, 1u)) * std::max(scale_y, 1u);
    uint64_t normalized = scale <= 1 ? samples : (samples + (scale >> 1)) / scale;
    return uint32_t(std::min<uint64_t>(normalized, UINT32_MAX));
  }

 private:
  ReportHandle next_report_handle_ = 1;
  std::unordered_map<uint32_t, uint64_t> slot_sequences_;
  std::unordered_map<uint32_t, uint32_t> slot_values_;
  std::unordered_map<uint32_t, uint32_t> cached_deltas_;
  std::unordered_map<ReportHandle, Report> reports_;
  ActiveSegment active_{};
};

}  // namespace rex::graphics
