#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>

#include <rex/graphics/zpd_report.h>

namespace rex::graphics {

enum class ZPDEndPolicy {
  kReportLayout,
  kPairwiseSentinel,
  kRelaxedSentinel,
};

enum class ZPDEndFallback {
  kNone,
  kPairwiseSentinel,
  kRelaxedSentinel,
};

enum class ZPDClassification {
  kBegin,
  kEnd,
  kOrphanedEnd,
  kMalformed,
};

enum class ZPDClassificationReason {
  kReportLayoutBegin,
  kReportLayoutEnd,
  kPairwiseSentinel,
  kRelaxedSentinel,
  kPairwiseFallback,
  kRelaxedFallback,
  kNoSentinel,
  kMalformedLayout,
};

struct ZPDPolicySettings {
  ZPDEndPolicy policy = ZPDEndPolicy::kRelaxedSentinel;
  ZPDEndFallback fallback = ZPDEndFallback::kNone;
};

struct ZPDClassificationResult {
  ZPDClassification classification = ZPDClassification::kMalformed;
  ZPDClassificationReason reason = ZPDClassificationReason::kMalformedLayout;
  bool pairwise_sentinel = false;
  bool relaxed_sentinel = false;
};

struct ZPDObservationRateLimit {
  uint64_t count = 0;
  bool overflow_sample = false;

  bool ShouldLog() const { return count <= 2 || (count & (count - 1)) == 0; }
};

class ZPDObservationRateLimiter {
 public:
  static constexpr size_t kMaxSignatures = 128;

  ZPDObservationRateLimit Observe(uint64_t signature) {
    auto observation = observations_.find(signature);
    if (observation != observations_.end()) {
      return {++observation->second, false};
    }
    if (observations_.size() < kMaxSignatures) {
      observations_.emplace(signature, 1);
      return {1, false};
    }
    return {++overflow_count_, true};
  }

 private:
  std::unordered_map<uint64_t, uint64_t> observations_;
  uint64_t overflow_count_ = 0;
};

inline ZPDPolicySettings ResolveZPDPolicy(std::string_view policy_name,
                                          std::string_view fallback_name, uint32_t title_id) {
  constexpr uint32_t kForzaHorizonTitleId = 0x4D5309C9;
  ZPDPolicySettings settings;
  if (policy_name == "report_layout" ||
      (policy_name == "auto" && title_id == kForzaHorizonTitleId)) {
    settings.policy = ZPDEndPolicy::kReportLayout;
  } else if (policy_name == "pairwise_sentinel") {
    settings.policy = ZPDEndPolicy::kPairwiseSentinel;
  } else {
    settings.policy = ZPDEndPolicy::kRelaxedSentinel;
  }
  if (fallback_name == "pairwise_sentinel" ||
      (fallback_name == "auto" && title_id == kForzaHorizonTitleId)) {
    settings.fallback = ZPDEndFallback::kPairwiseSentinel;
  } else if (fallback_name == "relaxed_sentinel") {
    settings.fallback = ZPDEndFallback::kRelaxedSentinel;
  } else {
    settings.fallback = ZPDEndFallback::kNone;
  }
  return settings;
}

inline ZPDClassificationResult ClassifyZPDReport(uint32_t report_address,
                                                 const xenos::xe_gpu_depth_sample_counts* report,
                                                 bool logical_active, ZPDPolicySettings settings) {
  ZPDClassificationResult result;
  result.pairwise_sentinel = XenosZPDReport::HasPairwisePendingSentinel(report);
  result.relaxed_sentinel = XenosZPDReport::HasPendingSentinel(report);

  bool is_end = false;
  if (settings.policy == ZPDEndPolicy::kReportLayout) {
    if (XenosZPDReport::IsBeginRecord(report_address)) {
      result.classification = ZPDClassification::kBegin;
      result.reason = ZPDClassificationReason::kReportLayoutBegin;
      return result;
    }
    if (XenosZPDReport::IsEndRecord(report_address)) {
      is_end = true;
      result.reason = ZPDClassificationReason::kReportLayoutEnd;
    } else if (settings.fallback == ZPDEndFallback::kPairwiseSentinel && result.pairwise_sentinel) {
      is_end = true;
      result.reason = ZPDClassificationReason::kPairwiseFallback;
    } else if (settings.fallback == ZPDEndFallback::kRelaxedSentinel && result.relaxed_sentinel) {
      is_end = true;
      result.reason = ZPDClassificationReason::kRelaxedFallback;
    } else {
      result.classification = ZPDClassification::kMalformed;
      result.reason = ZPDClassificationReason::kMalformedLayout;
      return result;
    }
  } else if (settings.policy == ZPDEndPolicy::kPairwiseSentinel) {
    is_end = result.pairwise_sentinel;
    result.reason =
        is_end ? ZPDClassificationReason::kPairwiseSentinel : ZPDClassificationReason::kNoSentinel;
  } else {
    is_end = result.relaxed_sentinel;
    result.reason =
        is_end ? ZPDClassificationReason::kRelaxedSentinel : ZPDClassificationReason::kNoSentinel;
  }

  result.classification =
      is_end ? (logical_active ? ZPDClassification::kEnd : ZPDClassification::kOrphanedEnd)
             : ZPDClassification::kBegin;
  return result;
}

inline const char* ZPDClassificationName(ZPDClassification classification) {
  switch (classification) {
    case ZPDClassification::kBegin:
      return "begin";
    case ZPDClassification::kEnd:
      return "end";
    case ZPDClassification::kOrphanedEnd:
      return "orphaned_end";
    default:
      return "malformed";
  }
}

inline const char* ZPDClassificationReasonName(ZPDClassificationReason reason) {
  switch (reason) {
    case ZPDClassificationReason::kReportLayoutBegin:
      return "report_layout_begin";
    case ZPDClassificationReason::kReportLayoutEnd:
      return "report_layout_end";
    case ZPDClassificationReason::kPairwiseSentinel:
      return "pairwise_sentinel";
    case ZPDClassificationReason::kRelaxedSentinel:
      return "relaxed_sentinel";
    case ZPDClassificationReason::kPairwiseFallback:
      return "pairwise_fallback";
    case ZPDClassificationReason::kRelaxedFallback:
      return "relaxed_fallback";
    case ZPDClassificationReason::kNoSentinel:
      return "no_sentinel";
    default:
      return "malformed_layout";
  }
}

}  // namespace rex::graphics
