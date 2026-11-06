#include <audioapi/core/inputs/ReclaimBackoff.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>

using namespace audioapi;
using Duration = ReclaimBackoff::Duration;

// NOLINTBEGIN

namespace {

constexpr Duration kFast = ReclaimBackoff::kSurvivalWindow - Duration{1};
constexpr Duration kSlow = ReclaimBackoff::kSurvivalWindow;

} // namespace

TEST(ReclaimBackoffTest, FirstFastDisconnectsReopenAtOnce) {
  ReclaimBackoff backoff;

  for (int i = 0; i < ReclaimBackoff::kImmediateReopens; ++i) {
    EXPECT_EQ(backoff.disconnected(kFast), Duration::zero());
    EXPECT_FALSE(backoff.isBackingOff());
  }

  EXPECT_EQ(backoff.disconnected(kFast), ReclaimBackoff::kInitialDelay);
  EXPECT_TRUE(backoff.isBackingOff());
}

// Property: once backing off, each fast disconnect at least doubles the delay until the cap,
// and the delay never exceeds the cap however long the run.
TEST(ReclaimBackoffTest, DelayDoublesUpToTheCapAndStaysThere) {
  ReclaimBackoff backoff;
  for (int i = 0; i < ReclaimBackoff::kImmediateReopens; ++i) {
    backoff.disconnected(kFast);
  }

  Duration previous = backoff.disconnected(kFast);
  for (int i = 0; i < 1000; ++i) {
    const Duration delay = backoff.disconnected(kFast);
    ASSERT_LE(delay, ReclaimBackoff::kMaxDelay);
    ASSERT_EQ(delay, std::min(previous * 2, ReclaimBackoff::kMaxDelay));
    previous = delay;
  }
  EXPECT_EQ(previous, ReclaimBackoff::kMaxDelay);
}

TEST(ReclaimBackoffTest, FailedOpenCountsAsAFastDisconnect) {
  ReclaimBackoff withOpenFailures;
  ReclaimBackoff withFastDisconnects;

  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(withOpenFailures.openFailed(), withFastDisconnects.disconnected(kFast));
  }
}

TEST(ReclaimBackoffTest, SlowDisconnectStartsOver) {
  ReclaimBackoff backoff;
  for (int i = 0; i < 5; ++i) {
    backoff.disconnected(kFast);
  }

  EXPECT_EQ(backoff.disconnected(kSlow), Duration::zero());
  EXPECT_FALSE(backoff.isBackingOff());
  EXPECT_EQ(backoff.fastDisconnects(), 0);
  EXPECT_EQ(backoff.disconnected(kFast), Duration::zero());
}

TEST(ReclaimBackoffTest, SurvivalEndsTheBackoff) {
  ReclaimBackoff backoff;
  for (int i = 0; i < 5; ++i) {
    backoff.disconnected(kFast);
  }
  ASSERT_TRUE(backoff.isBackingOff());

  backoff.survived();

  EXPECT_FALSE(backoff.isBackingOff());
  EXPECT_EQ(backoff.disconnected(kFast), Duration::zero());
}

// NOLINTEND
