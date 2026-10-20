/**
 * @file test_epoch_cadence.cpp
 * @brief The measured epoch spacing and the default data-gap threshold built on it.
 *
 * The real-time engine used a fixed 1 s threshold whenever
 * `processing.data_gap_tolerance_s` was 0, so every epoch of a 30 s stream read as a
 * data gap.  The documented default is 1.5 nominal intervals; the engine measures the
 * interval, since a live stream has no header.
 */

#include <gtest/gtest.h>

#include "realtime/EpochCadence.hpp"

#include <cstddef>

namespace GnssProcessor {
namespace {

TEST(EpochCadenceTest, UnmeasuredStreamKeepsTheFixedOneSecondThreshold) {
    EpochCadence c;
    EXPECT_EQ(c.sampleCount(), 0u);
    EXPECT_EQ(c.nominalIntervalS(), 0.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 1.0);
}

TEST(EpochCadenceTest, ConfiguredToleranceIsUsedAsGiven) {
    EpochCadence c;
    EXPECT_DOUBLE_EQ(c.gapToleranceS(45.0), 45.0);
    for (int i = 0; i < 20; ++i) c.observe(30.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(5.0), 5.0);   // not scaled by the measured interval
}

TEST(EpochCadenceTest, DefaultIsOnePointFiveMeasuredIntervals) {
    EpochCadence c;
    c.observe(30.0);
    EXPECT_DOUBLE_EQ(c.nominalIntervalS(), 30.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 45.0);      // a 30 s epoch is no gap
    EXPECT_FALSE(30.0 > c.gapToleranceS(0.0));
    EXPECT_TRUE(60.0 > c.gapToleranceS(0.0));          // a missed epoch is

    EpochCadence one_hz;
    one_hz.observe(1.0);
    EXPECT_DOUBLE_EQ(one_hz.gapToleranceS(0.0), 1.5);
}

TEST(EpochCadenceTest, StreamFasterThanOneHertzKeepsTheOneSecondFloor) {
    // 1.5 intervals would be 0.15 s at 10 Hz and 0.75 s at 2 Hz; the threshold stays at
    // the fixed 1 s the engine used for every stream, so one dropped epoch is no gap.
    EpochCadence ten_hz;
    for (int i = 0; i < 20; ++i) ten_hz.observe(0.1);
    EXPECT_DOUBLE_EQ(ten_hz.nominalIntervalS(), 0.1);
    EXPECT_DOUBLE_EQ(ten_hz.gapToleranceS(0.0), EpochCadence::kFloorToleranceS);
    EXPECT_FALSE(0.2 > ten_hz.gapToleranceS(0.0));     // one dropped epoch
    EXPECT_TRUE(1.2 > ten_hz.gapToleranceS(0.0));      // an outage of a second or more

    EpochCadence two_hz;
    for (int i = 0; i < 20; ++i) two_hz.observe(0.5);
    EXPECT_DOUBLE_EQ(two_hz.gapToleranceS(0.0), 1.0);
}

TEST(EpochCadenceTest, FloorLiftsOnceOneAndAHalfIntervalsExceedIt) {
    EpochCadence c;
    for (int i = 0; i < 20; ++i) c.observe(0.6);       // 0.9 s: still the floor
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 1.0);
    EpochCadence d;
    for (int i = 0; i < 20; ++i) d.observe(0.8);       // 1.2 s: the scaled value
    EXPECT_DOUBLE_EQ(d.gapToleranceS(0.0), 1.2);
}

TEST(EpochCadenceTest, ConfiguredToleranceBelowTheFloorIsUsedAsGiven) {
    EpochCadence c;
    for (int i = 0; i < 20; ++i) c.observe(0.1);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.25), 0.25);     // only the default has a floor
}

TEST(EpochCadenceTest, MedianIgnoresAnIsolatedOutage) {
    EpochCadence c;
    for (int i = 0; i < 6; ++i) c.observe(30.0);
    c.observe(1200.0);                                 // 20 min outage
    EXPECT_DOUBLE_EQ(c.nominalIntervalS(), 30.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 45.0);
}

TEST(EpochCadenceTest, NonPositiveSpacingsAreNotRecorded) {
    EpochCadence c;
    c.observe(0.0);
    c.observe(-30.0);
    EXPECT_EQ(c.sampleCount(), 0u);
    c.observe(30.0);
    EXPECT_EQ(c.sampleCount(), 1u);
}

TEST(EpochCadenceTest, EvenCountTakesTheLowerMiddle) {
    EpochCadence c;
    c.observe(1200.0);
    c.observe(30.0);
    // After a warm resume the first spacing spans the outage; the next one is normal.
    EXPECT_DOUBLE_EQ(c.nominalIntervalS(), 30.0);
}

TEST(EpochCadenceTest, FollowsALastingChangeOfRate) {
    EpochCadence c;
    for (std::size_t i = 0; i < EpochCadence::kWindow; ++i) c.observe(1.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 1.5);
    // The stream moves to 30 s epochs: once the window is mostly 30 s the
    // threshold follows (the first few of them are flagged, as they should be).
    for (std::size_t i = 0; i < EpochCadence::kWindow / 2 + 1; ++i) c.observe(30.0);
    EXPECT_DOUBLE_EQ(c.gapToleranceS(0.0), 45.0);
}

TEST(EpochCadenceTest, WindowForgetsTheOldestSpacing) {
    EpochCadence c;
    for (int i = 0; i < 100; ++i) c.observe(30.0);
    EXPECT_EQ(c.sampleCount(), EpochCadence::kWindow);
}

} // namespace
} // namespace GnssProcessor
