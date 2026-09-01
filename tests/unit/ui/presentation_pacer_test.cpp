#include <catch2/catch_test_macros.hpp>

#include <rex/ui/presentation_pacer.h>

TEST_CASE("presentation pacer advances fixed monotonic deadlines", "[ui][pacing]") {
  rex::ui::PresentationDeadlineScheduler scheduler;
  auto first = scheduler.Plan(1000000000LL, 60);
  REQUIRE(first.wait_ns == 0);
  REQUIRE(first.missed_deadlines == 0);

  auto second = scheduler.Plan(1005000000LL, 60);
  REQUIRE(second.deadline_ns == 1016666666LL);
  REQUIRE(second.wait_ns == 11666666LL);
  REQUIRE(second.missed_deadlines == 0);
}

TEST_CASE("presentation pacer reports skipped deadlines without drift", "[ui][pacing]") {
  rex::ui::PresentationDeadlineScheduler scheduler;
  scheduler.Plan(0, 60);
  auto late = scheduler.Plan(50000000LL, 60);
  REQUIRE(late.missed_deadlines == 2);
  REQUIRE(late.wait_ns == 0);
  auto recovered = scheduler.Plan(60000000LL, 60);
  REQUIRE(recovered.deadline_ns == 66666664LL);
  REQUIRE(recovered.wait_ns == 6666664LL);
}

TEST_CASE("presentation pacer supports 30 fps and disabling", "[ui][pacing]") {
  rex::ui::PresentationDeadlineScheduler scheduler;
  scheduler.Plan(2000000000LL, 30);
  REQUIRE(scheduler.Plan(2010000000LL, 30).wait_ns == 23333333LL);
  REQUIRE(scheduler.Plan(2020000000LL, 0).wait_ns == 0);
  REQUIRE(scheduler.Plan(2020000000LL, 60).wait_ns == 0);
}
