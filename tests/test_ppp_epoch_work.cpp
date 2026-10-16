/**
 * @file test_ppp_epoch_work.cpp
 * @brief The working values of the PPP / PPP-RTK pipeline.
 *
 * Application::runPpp builds one PppEpochWork per epoch and the stages read and write
 * its members.  Those members were the epoch loop's locals, and the stages rely on the
 * initial values the locals had (no update applied, no reset, no gap, an empty log
 * prefix).  These tests hold each default to that initializer, so an epoch keeps starting
 * from the values it did before the cut, and hold the run-end view to the two values the
 * reports read.
 */

#include <gtest/gtest.h>

#include "app/PppPipelineWork.hpp"

#include <chrono>

using namespace GnssProcessor;

TEST(PppEpochWork, StartsFromTheEpochLoopsInitialValues) {
    const PppEpochWork w;

    // 4a: the observations, and the always-empty log prefix (the log pattern carries
    // the epoch).
    EXPECT_EQ(w.current_time.week, 0);
    EXPECT_EQ(w.current_time.tow, 0.0);
    EXPECT_TRUE(w.epoch_obs.empty());
    EXPECT_TRUE(w.epoch_prefix.empty());

    // 4b: no time step and no gap until stepEpochClock_ sets them.
    EXPECT_EQ(w.dt, 0.0);
    EXPECT_FALSE(w.gap_detected);
    EXPECT_FALSE(w.long_gap);

    // 4d, 4e, 4g: no models, no WLS solution, no slip action.
    EXPECT_TRUE(w.modeled_obs.empty());
    EXPECT_TRUE(w.drop_reasons.empty());
    EXPECT_FALSE(w.wls_result.success());
    EXPECT_FALSE(w.slip_result.state_modified);
    EXPECT_EQ(w.slip_result.n_repaired, 0);
    EXPECT_EQ(w.slip_result.n_reset, 0);
    EXPECT_TRUE(w.slip_result.reset_satellites.empty());
    EXPECT_TRUE(w.slip_result.ambiguity_repairs.empty());

    // 4h: no update applied and no reset, so the smoother, the NEQ and AR see an
    // epoch without an update until the update loop says otherwise.
    EXPECT_FALSE(w.have_applied);
    EXPECT_EQ(w.applied_meas.innovations.size(), 0);
    EXPECT_TRUE(w.epoch_reset_sats.empty());
    EXPECT_FALSE(w.epoch_complete_reset);
}

TEST(PppRunEndView, CarriesTheLoopStartAndTheMode) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    const PppRunEndView v{ t0, true };
    EXPECT_EQ(v.wall_start, t0);
    EXPECT_TRUE(v.is_ppp_rtk);
}
