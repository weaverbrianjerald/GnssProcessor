/**
 * @file test_epoch_seek.cpp
 * @brief Unit tests for the start-offset seek (network.start_offset_hours): which epoch a
 *        run starts on.
 *
 * Every epoch time reaches the seek through the calendar to GPS conversion, which leaves
 * about 2e-5 s of double-precision residue on the elapsed time between two epochs.  A plain
 * floating comparison of that elapsed time with the offset therefore decides an offset that
 * lies exactly on an epoch (a bare hour on a 30 s grid) by the sign of the residue: some
 * hours start on the intended epoch and some one epoch late.  The seek compares on a
 * whole-millisecond grid instead (TimeUtils::offsetReached).
 *
 * The tests build the epochs with the production conversion (TimeUtils::GnssTimeFromDate) so
 * the residue is the real one, and keep the legacy comparison in the test as the reference
 * that the new rule must agree with wherever the legacy rule was not decided by the residue.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "network/driver/EpochAdmission.hpp"
#include "network/driver/Run.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "support/NetworkTestRun.hpp"
#include "support/TestTempDir.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kStep = 30.0;           ///< Epoch interval of the fixtures, seconds.
constexpr int    kEpochsPerDay = 2880;   ///< 24 h at 30 s.

/// Continuous GPS seconds of epoch @p k of 2024-05-08, @p first_s seconds after midnight,
/// built by the production RINEX-time conversion.
std::vector<double> dayOfEpochs(double first_s) {
    std::vector<double> t;
    t.reserve(kEpochsPerDay);
    for (int k = 0; k < kEpochsPerDay; ++k) {
        const double since_midnight = first_s + k * kStep;
        const int day = 8 + static_cast<int>(since_midnight / 86400.0);   // a late start spills into 9 May
        const double sod = since_midnight - (day - 8) * 86400.0;
        const int h = static_cast<int>(sod / 3600.0);
        const int m = static_cast<int>((sod - h * 3600.0) / 60.0);
        const double s = sod - h * 3600.0 - m * 60.0;
        t.push_back(TimeUtils::continuousGpsSeconds(
            TimeUtils::GnssTimeFromDate(2024, 5, day, h, m, s, "GPS")));
    }
    return t;
}

/// The comparison the seek used before the millisecond grid: a strict floating `<` skip.
bool legacyReached(double now_s, double anchor_s, double offset_s) {
    return !(now_s - anchor_s < offset_s);
}

/// Index of the first epoch (anchored on epoch 0) for which @p reached holds; size when none.
template <class Pred>
std::size_t firstEpoch(const std::vector<double>& t, Pred reached) {
    for (std::size_t i = 0; i < t.size(); ++i)
        if (reached(t[i], t[0])) return i;
    return t.size();
}

std::size_t newRuleFirst(const std::vector<double>& t, double offset_s) {
    return firstEpoch(t, [&](double now, double anchor) {
        return TimeUtils::offsetReached(now, anchor, offset_s);
    });
}

std::size_t legacyRuleFirst(const std::vector<double>& t, double offset_s) {
    return firstEpoch(t, [&](double now, double anchor) {
        return legacyReached(now, anchor, offset_s);
    });
}

}  // namespace

// An offset that lies exactly on epoch k (k epochs after the anchor) starts the run on epoch k,
// for every epoch of the day and both fixture starts (midnight, and the 00:00:30 start of the
// simulated network).
TEST(EpochSeekTest, OffsetOnTheGridStartsOnThatEpoch) {
    for (const double first_s : {0.0, 30.0}) {
        const std::vector<double> t = dayOfEpochs(first_s);
        for (std::size_t k = 1; k < t.size(); ++k)
            ASSERT_EQ(newRuleFirst(t, static_cast<double>(k) * kStep), k)
                << "offset " << k * kStep << " s, first epoch at " << first_s << " s";
    }
}

// The reason for the rule: with the plain floating comparison some on-grid offsets start one
// epoch late. The bare hours below are the production case (a 1 h offset on the 30 s grid); if
// the conversion ever loses its residue this test goes, and with it the need for the rounding.
TEST(EpochSeekTest, LegacyComparisonMissesSomeBareHours) {
    const std::vector<double> t = dayOfEpochs(0.0);
    int late = 0, on_time = 0;
    for (int hour = 1; hour <= 23; ++hour) {
        const std::size_t want = static_cast<std::size_t>(hour) * 120;
        const std::size_t got = legacyRuleFirst(t, hour * 3600.0);
        if (got == want) ++on_time;
        else {
            EXPECT_EQ(got, want + 1) << "hour " << hour << " is late by more than one epoch";
            ++late;
        }
        EXPECT_EQ(newRuleFirst(t, hour * 3600.0), want) << "hour " << hour;
    }
    EXPECT_GT(late, 0) << "no bare hour lands late: the residue is gone from the conversion";
    EXPECT_GT(on_time, 0);
}

// An offset a half epoch short of an epoch (the back-off every production config carries) selects
// the same epoch as before, for every epoch of the day: the rounding moves the decision only when
// the offset is within a millisecond of an epoch.
TEST(EpochSeekTest, HalfEpochBackOffKeepsItsEpoch) {
    for (const double first_s : {0.0, 30.0}) {
        const std::vector<double> t = dayOfEpochs(first_s);
        for (std::size_t k = 1; k < t.size(); ++k) {
            const double offset_s = static_cast<double>(k) * kStep - 0.5 * kStep;
            ASSERT_EQ(newRuleFirst(t, offset_s), legacyRuleFirst(t, offset_s))
                << "offset " << offset_s;
            ASSERT_EQ(newRuleFirst(t, offset_s), k);
        }
    }
}

// The offsets written in the repository's configs (hours): every one that carries a half-epoch
// back-off selects the legacy epoch, whatever the anchor.  The few that sit on an epoch are
// the bare cases and select the epoch they name.
TEST(EpochSeekTest, ShippedOffsetsKeepTheirFirstEpoch) {
    const std::vector<double> offsets_h = {
        0.504167, 0.4958333, 0.0875, 1.0041666666666667, 1.9958333, 2.5208333333,
        3.020833332339923, 3.1125, 5.9958333, 11.9958333, 12.070833333333333,
        12.995833333333334, 13.1125, 17.9958333, 18.1125, 19.1125};
    const std::vector<double> bare_h = {
        0.08333333333333333, 0.09166666666666666, 0.1, 0.125, 1.0083333333333333, 2.0, 3.5,
        5.0, 6.008333333333334};
    for (const double first_s : {0.0, 30.0}) {
        const std::vector<double> t = dayOfEpochs(first_s);
        for (const double h : offsets_h) {
            const double offset_s = h * 3600.0;
            // These sit a half epoch (to within 1e-3 s) off the grid.
            const double frac = std::fmod(offset_s / kStep, 1.0);
            ASSERT_NEAR(frac, 0.5, 1e-4) << "offset " << h << " h is not a half-epoch offset";
            EXPECT_EQ(newRuleFirst(t, offset_s), legacyRuleFirst(t, offset_s)) << h << " h";
        }
        for (const double h : bare_h) {
            const double offset_s = h * 3600.0;
            const double n = std::round(offset_s / kStep);
            ASSERT_NEAR(offset_s, n * kStep, 1e-3) << "offset " << h << " h is not on the grid";
            EXPECT_EQ(newRuleFirst(t, offset_s), static_cast<std::size_t>(n)) << h << " h";
        }
    }
}

// The comparison is on whole milliseconds: an elapsed time within half a millisecond of the
// offset has reached it, one beyond half a millisecond short has not.
TEST(EpochSeekTest, ComparisonIsOnTheMillisecondGrid) {
    const double anchor = 1.4e9;
    EXPECT_TRUE(TimeUtils::offsetReached(anchor + 1800.0, anchor, 1800.0));
    EXPECT_TRUE(TimeUtils::offsetReached(anchor + 1799.99996, anchor, 1800.0));
    EXPECT_TRUE(TimeUtils::offsetReached(anchor + 1799.9996, anchor, 1800.0));
    EXPECT_FALSE(TimeUtils::offsetReached(anchor + 1799.9994, anchor, 1800.0));
    EXPECT_FALSE(TimeUtils::offsetReached(anchor + 1799.0, anchor, 1800.0));
    EXPECT_TRUE(TimeUtils::offsetReached(anchor + 1830.0, anchor, 1800.0));
    // A half-epoch offset (the production back-off) is not on any epoch.
    EXPECT_FALSE(TimeUtils::offsetReached(anchor + 1800.0, anchor, 1815.0012));
    EXPECT_TRUE(TimeUtils::offsetReached(anchor + 1830.0, anchor, 1815.0012));
}

TEST(EpochSeekTest, ElapsedMillisecondsRoundToNearest) {
    EXPECT_EQ(TimeUtils::elapsedMs(1.4e9 + 3599.9999999, 1.4e9), 3600000LL);
    EXPECT_EQ(TimeUtils::elapsedMs(1.4e9 + 3600.0004, 1.4e9), 3600000LL);
    EXPECT_EQ(TimeUtils::elapsedMs(1.4e9 + 3600.0006, 1.4e9), 3600001LL);
    EXPECT_EQ(TimeUtils::elapsedMs(1.4e9, 1.4e9), 0LL);
}

// An offset no data can reach is never reached and never overflows the integer grid.
TEST(EpochSeekTest, HugeOffsetIsNeverReached) {
    EXPECT_FALSE(TimeUtils::offsetReached(1.4e9 + 86400.0, 1.4e9, 1.0e300));
    EXPECT_FALSE(TimeUtils::offsetReached(1.4e9 + 86400.0, 1.4e9,
                                          std::numeric_limits<double>::infinity()));
}

namespace {

/// A GnssConfig whose network.start_offset_hours is @p hours.
GnssConfig configWithOffset(double hours) {
    LogManager log;
    GnssConfig cfg;
    const std::filesystem::path p = gnss_test::tempDir() / "epoch_seek_cfg.json";
    {
        std::ofstream f(p);
        f << "{\n"
             "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
             "                    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
             "  \"network\": {\n"
             "    \"clock_datum_station\": \"AAAA\",\n"
             "    \"start_offset_hours\": " << std::to_string(hours) << ",\n"
             "    \"stations\": [\n"
             "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
             "    ]\n"
             "  }\n"
             "}\n";
    }
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return cfg;
}

}  // namespace

// The producer's fast-forward gate follows the rule: a bare one-hour offset skips the 119 epochs
// before the hour and processes the epoch at the hour, on the anchor of the first epoch it sees.
TEST(EpochSeekTest, ProducerFastForwardStartsOnTheBareHour) {
    const std::vector<double> t = dayOfEpochs(0.0);
    const GnssConfig cfg = configWithOffset(1.0);
    ASSERT_DOUBLE_EQ(cfg.networkStartOffsetHours(), 1.0);
    auto fx = std::make_unique<NetworkTestRun>(cfg);
    std::size_t first = t.size();
    for (std::size_t i = 0; i < t.size() && first == t.size(); ++i) {
        SyncedEpoch se;
        const double week = std::floor(t[i] / 604800.0);
        se.time = GnssTime{static_cast<int>(week), t[i] - week * 604800.0};
        if (networkFastForward(fx->run(), se) == EpochAdmission::Process) first = i;
    }
    EXPECT_EQ(first, 120u);
    // The legacy comparison on the same epochs lands one epoch late at the hour.
    EXPECT_EQ(legacyRuleFirst(t, 3600.0), 121u);
}

// With the knob unset the gate never skips and never anchors.
TEST(EpochSeekTest, ProducerFastForwardOffByDefault) {
    const GnssConfig cfg = configWithOffset(0.0);
    auto fx = std::make_unique<NetworkTestRun>(cfg);
    SyncedEpoch se;
    se.time = GnssTime{2313, 259200.0};
    EXPECT_EQ(networkFastForward(fx->run(), se), EpochAdmission::Process);
    EXPECT_LT(fx->ctx().carry.start_anchor_s, 0.0);
}

}  // namespace GnssProcessor
