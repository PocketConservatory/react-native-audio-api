#include <audioapi/core/inputs/InterruptionCauses.h>
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

using namespace audioapi;

// NOLINTBEGIN

namespace {

constexpr std::array<InterruptionCause, 2> kCauses{
    InterruptionCause::CaptureSilenced,
    InterruptionCause::ReclaimBackoff};

} // namespace

TEST(InterruptionCausesTest, OneCauseBeginsAndEndsTheInterruption) {
  InterruptionCauses causes;

  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, true), InterruptionTransition::Began);
  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, true), InterruptionTransition::None);
  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, false), InterruptionTransition::Ended);
  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, false), InterruptionTransition::None);
}

TEST(InterruptionCausesTest, OverlappingCausesShareOneBeganAndEnded) {
  InterruptionCauses causes;

  EXPECT_EQ(causes.set(InterruptionCause::ReclaimBackoff, true), InterruptionTransition::Began);
  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, true), InterruptionTransition::None);
  EXPECT_EQ(causes.set(InterruptionCause::ReclaimBackoff, false), InterruptionTransition::None);
  EXPECT_TRUE(causes.has(InterruptionCause::CaptureSilenced));
  EXPECT_FALSE(causes.has(InterruptionCause::ReclaimBackoff));
  EXPECT_EQ(causes.set(InterruptionCause::CaptureSilenced, false), InterruptionTransition::Ended);
}

TEST(InterruptionCausesTest, ClearEndsSilentlySoTheNextCauseBeginsAgain) {
  InterruptionCauses causes;
  causes.set(InterruptionCause::CaptureSilenced, true);
  causes.set(InterruptionCause::ReclaimBackoff, true);

  causes.clear();

  EXPECT_FALSE(causes.any());
  EXPECT_EQ(causes.set(InterruptionCause::ReclaimBackoff, false), InterruptionTransition::None);
  EXPECT_EQ(causes.set(InterruptionCause::ReclaimBackoff, true), InterruptionTransition::Began);
}

// Property: over any sequence of changes, transitions alternate Began/Ended starting with Began,
// and an interruption is open exactly while some cause is active.
TEST(InterruptionCausesTest, TransitionsAlternateAndTrackActiveCausesForAnySequence) {
  std::mt19937 random(42);
  std::uniform_int_distribution<int> pick(0, 3);

  for (int run = 0; run < 200; ++run) {
    InterruptionCauses causes;
    bool open = false;
    for (int step = 0; step < 50; ++step) {
      const int choice = pick(random);
      const auto cause = kCauses[choice % 2];
      const bool active = choice >= 2;

      const auto transition = causes.set(cause, active);

      if (transition == InterruptionTransition::Began) {
        ASSERT_FALSE(open);
        open = true;
      } else if (transition == InterruptionTransition::Ended) {
        ASSERT_TRUE(open);
        open = false;
      }
      ASSERT_EQ(open, causes.any());
      ASSERT_EQ(causes.has(cause), active);
    }
  }
}

// NOLINTEND
