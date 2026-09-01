#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/zpd_lifecycle.h>
#include <rex/graphics/zpd_policy.h>

namespace rex::graphics {

TEST_CASE("ZPD report layout identifies begin and end records", "[graphics][zpd]") {
  constexpr uint32_t slot = 0x1000;
  REQUIRE(XenosZPDReport::GetSlotBase(slot + 0x37) == slot);
  REQUIRE(XenosZPDReport::IsEndRecord(slot));
  REQUIRE(XenosZPDReport::IsBeginRecord(slot + 0x20));
  REQUIRE_FALSE(XenosZPDReport::IsBeginRecord(slot));
  REQUIRE_FALSE(XenosZPDReport::IsEndRecord(slot + 0x20));
}

TEST_CASE("ZPD logical report accumulates submission segments", "[graphics][zpd]") {
  ZPDLifecycle lifecycle;
  auto begin = lifecycle.Begin(0x1020);
  REQUIRE(begin.report_handle != ZPDLifecycle::kInvalidReportHandle);
  REQUIRE(lifecycle.SegmentOpened());
  REQUIRE(lifecycle.SegmentClosed(7));
  REQUIRE(lifecycle.SegmentOpened());
  REQUIRE(lifecycle.SegmentClosed(8));
  REQUIRE(lifecycle.End(0x1000));

  uint32_t delta = 0;
  bool commit = false;
  ZPDLifecycle::Report report;
  REQUIRE(lifecycle.Resolve(begin.report_handle, 12, 1, 1, delta, commit, report));
  REQUIRE(delta == 12);
  REQUIRE_FALSE(commit);
  REQUIRE(lifecycle.Resolve(begin.report_handle, 20, 1, 1, delta, commit, report));
  REQUIRE(delta == 32);
  REQUIRE(commit);
  REQUIRE(lifecycle.Find(begin.report_handle) == nullptr);
}

TEST_CASE("ZPD stale result is rejected after same-slot reuse", "[graphics][zpd]") {
  ZPDLifecycle lifecycle;
  auto old_begin = lifecycle.Begin(0x2020);
  REQUIRE(lifecycle.SegmentOpened());
  REQUIRE(lifecycle.SegmentClosed(3));
  REQUIRE(lifecycle.End(0x2000));

  auto new_begin = lifecycle.Begin(0x2020);
  REQUIRE(new_begin.same_slot_reuse);
  uint32_t delta = 0;
  bool commit = true;
  ZPDLifecycle::Report report;
  REQUIRE(lifecycle.Resolve(old_begin.report_handle, 99, 1, 1, delta, commit, report));
  REQUIRE_FALSE(commit);
  REQUIRE(lifecycle.CachedDelta(0x2000, 7) == 7);
  REQUIRE(lifecycle.Find(new_begin.report_handle) != nullptr);
}

TEST_CASE("ZPD normalization preserves one guest sample at 2x", "[graphics][zpd]") {
  REQUIRE(ZPDLifecycle::Normalize(4, 2, 2) == 1);
  REQUIRE(ZPDLifecycle::Normalize(5, 2, 2) == 1);
  REQUIRE(ZPDLifecycle::Normalize(uint64_t(UINT32_MAX) * 8, 1, 1) == UINT32_MAX);
}

TEST_CASE("ZPD policy defaults are title scoped", "[graphics][zpd]") {
  ZPDPolicySettings forza = ResolveZPDPolicy("auto", "auto", 0x4D5309C9);
  REQUIRE(forza.policy == ZPDEndPolicy::kReportLayout);
  REQUIRE(forza.fallback == ZPDEndFallback::kPairwiseSentinel);

  ZPDPolicySettings other = ResolveZPDPolicy("auto", "auto", 0x12345678);
  REQUIRE(other.policy == ZPDEndPolicy::kRelaxedSentinel);
  REQUIRE(other.fallback == ZPDEndFallback::kNone);
}

TEST_CASE("ZPD policies distinguish one-lane and pairwise sentinels", "[graphics][zpd]") {
  xenos::xe_gpu_depth_sample_counts report = {};
  report.ZPass_A = 0xEDFEFFFFu;

  auto pairwise = ClassifyZPDReport(0x10000, &report, true,
                                    {ZPDEndPolicy::kPairwiseSentinel, ZPDEndFallback::kNone});
  REQUIRE(pairwise.classification == ZPDClassification::kBegin);
  REQUIRE_FALSE(pairwise.pairwise_sentinel);
  REQUIRE(pairwise.relaxed_sentinel);

  auto relaxed = ClassifyZPDReport(0x10000, &report, true,
                                   {ZPDEndPolicy::kRelaxedSentinel, ZPDEndFallback::kNone});
  REQUIRE(relaxed.classification == ZPDClassification::kEnd);
  REQUIRE(relaxed.reason == ZPDClassificationReason::kRelaxedSentinel);

  report.ZPass_B = 0xEDFEFFFFu;
  pairwise = ClassifyZPDReport(0x10000, &report, true,
                               {ZPDEndPolicy::kPairwiseSentinel, ZPDEndFallback::kNone});
  REQUIRE(pairwise.classification == ZPDClassification::kEnd);
  REQUIRE(pairwise.pairwise_sentinel);
}

TEST_CASE("ZPD report layout classifies ends and orphaned ends", "[graphics][zpd]") {
  xenos::xe_gpu_depth_sample_counts report = {};
  auto begin = ClassifyZPDReport(0x10020, &report, false,
                                 {ZPDEndPolicy::kReportLayout, ZPDEndFallback::kPairwiseSentinel});
  REQUIRE(begin.classification == ZPDClassification::kBegin);
  REQUIRE(begin.reason == ZPDClassificationReason::kReportLayoutBegin);

  auto end = ClassifyZPDReport(0x10000, &report, true,
                               {ZPDEndPolicy::kReportLayout, ZPDEndFallback::kPairwiseSentinel});
  REQUIRE(end.classification == ZPDClassification::kEnd);
  REQUIRE(end.reason == ZPDClassificationReason::kReportLayoutEnd);

  auto orphan = ClassifyZPDReport(0x10000, &report, false,
                                  {ZPDEndPolicy::kReportLayout, ZPDEndFallback::kPairwiseSentinel});
  REQUIRE(orphan.classification == ZPDClassification::kOrphanedEnd);

  report.ZFail_A = report.ZFail_B = 0xEDFEFFFFu;
  auto fallback = ClassifyZPDReport(
      0, &report, true, {ZPDEndPolicy::kReportLayout, ZPDEndFallback::kPairwiseSentinel});
  REQUIRE(fallback.classification == ZPDClassification::kEnd);
  REQUIRE(fallback.reason == ZPDClassificationReason::kPairwiseFallback);
}

TEST_CASE("ZPD observation logging stays bounded", "[graphics][zpd]") {
  ZPDObservationRateLimiter limiter;
  for (uint64_t signature = 0; signature < ZPDObservationRateLimiter::kMaxSignatures; ++signature) {
    auto observation = limiter.Observe(signature);
    REQUIRE(observation.count == 1);
    REQUIRE_FALSE(observation.overflow_sample);
    REQUIRE(observation.ShouldLog());
  }

  auto first_overflow = limiter.Observe(1000);
  REQUIRE(first_overflow.count == 1);
  REQUIRE(first_overflow.overflow_sample);
  REQUIRE(first_overflow.ShouldLog());
  auto second_overflow = limiter.Observe(1001);
  REQUIRE(second_overflow.count == 2);
  REQUIRE(second_overflow.ShouldLog());
  auto third_overflow = limiter.Observe(1002);
  REQUIRE(third_overflow.count == 3);
  REQUIRE_FALSE(third_overflow.ShouldLog());
  auto fourth_overflow = limiter.Observe(1003);
  REQUIRE(fourth_overflow.count == 4);
  REQUIRE(fourth_overflow.ShouldLog());

  auto repeated = limiter.Observe(0);
  REQUIRE(repeated.count == 2);
  REQUIRE_FALSE(repeated.overflow_sample);
}

}  // namespace rex::graphics
