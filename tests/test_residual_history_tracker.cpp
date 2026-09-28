/**
 * @file test_residual_history_tracker.cpp
 * @brief ResidualHistoryTracker: the warm-up flag that getStats reports.
 *
 * getStats reports warmed_up by the rule that evaluate and evaluateStuckBias
 * apply (n >= warmup_n), so a caller that gates on the flag follows
 * estimation.residual_history.warmup_n.  The persistent-bias R-inflation in
 * KalmanFilter::update gates on it; it previously compared n with a fixed 20.
 */
#include <gtest/gtest.h>

#include <cstddef>

#include "filter/ResidualHistoryTracker.hpp"
#include "logging/LogManager.hpp"

using namespace GnssProcessor;

namespace {

ResidualHistoryTracker::Config enabledConfig() {
    ResidualHistoryTracker::Config cfg;
    cfg.enabled = true;
    return cfg;
}

// Alternating-sign residuals keep the EWMA variance away from zero.
double residualAt(std::size_t k) { return (k % 2 == 0) ? 0.012 : -0.008; }

}  // namespace

// A non-default warm-up: the flag turns on at the fifth sample, and at every
// count it equals the flags that evaluate() and evaluateStuckBias() report.
TEST(ResidualHistoryTrackerWarmup, StatsFlagFollowsConfiguredWarmupN) {
    LogManager log;
    ResidualHistoryTracker::Config cfg = enabledConfig();
    cfg.warmup_n = 5;
    ResidualHistoryTracker tracker(cfg, log);

    for (std::size_t k = 1; k <= 12; ++k) {
        tracker.record("G05", "L1C", residualAt(k));
        const auto st = tracker.getStats("G05", "L1C");
        ASSERT_TRUE(st.has_value());
        EXPECT_EQ(st->n, k);
        EXPECT_EQ(st->warmed_up, k >= 5) << "n = " << k;
        EXPECT_EQ(st->warmed_up, tracker.evaluate("G05", "L1C", 0.0).warmed_up)
            << "n = " << k;
        EXPECT_EQ(st->warmed_up,
                  tracker.evaluateStuckBias("G05", "L1C", 1.0, 0).warmed_up)
            << "n = " << k;
    }
}

// The default warm-up: the flag equals the fixed n >= 20 test it replaced in
// KalmanFilter::update, so a run that leaves warmup_n unset keeps its behaviour.
TEST(ResidualHistoryTrackerWarmup, DefaultWarmupMatchesTheFixedTwenty) {
    LogManager log;
    const ResidualHistoryTracker::Config cfg = enabledConfig();
    ASSERT_EQ(cfg.warmup_n, 20u);
    ResidualHistoryTracker tracker(cfg, log);

    for (std::size_t k = 1; k <= 25; ++k) {
        tracker.record("E11", "L5Q", residualAt(k));
        const auto st = tracker.getStats("E11", "L5Q");
        ASSERT_TRUE(st.has_value());
        EXPECT_EQ(st->warmed_up, !(st->n < 20)) << "n = " << k;
    }
}
