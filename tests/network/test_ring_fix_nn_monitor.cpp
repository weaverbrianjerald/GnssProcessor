// Unit tests of the near-null monitor's tally (include/network/ar/RingFix.hpp, ringFixNnMonitorStep,
// network.ring_fix.nn_monitor): a verdict at a whole step with a small ionosphere-free deviation counts as a step
// epoch; episodes open and close at the edges; a large ionosphere-free deviation or k = 0 is not a step; a chord with
// no verdict keeps its state.

#include "network/ar/RingFix.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace GnssProcessor;

namespace {

RingFixNearNullVerdict verdict(int station, int k, double dev_gf, double dev_if) {
    RingFixNearNullVerdict v;
    v.id = {station, "E11", "L1C", "L5Q"};
    v.k = k;
    v.dev_gf_m = dev_gf;
    v.dev_if_m = dev_if;
    return v;
}

}  // namespace

TEST(RingFixNearNullMonitor, StepEpochsAndEpisodesAreTallied) {
    RingFixNnMonitor m;
    // Epoch 1: at a step; epoch 2: still; epoch 3: back under half a step; epoch 4: at a step again.
    auto e1 = ringFixNnMonitorStep(m, {verdict(3, 1, 0.45, 0.05)}, 100.0, 0.30);
    auto e2 = ringFixNnMonitorStep(m, {verdict(3, 1, 0.44, 0.04)}, 130.0, 0.30);
    auto e3 = ringFixNnMonitorStep(m, {verdict(3, 0, 0.10, 0.02)}, 160.0, 0.30);
    auto e4 = ringFixNnMonitorStep(m, {verdict(3, -1, -0.46, -0.03)}, 190.0, 0.30);
    ASSERT_EQ(e1.size(), 1u);
    EXPECT_TRUE(e1.front().opened);
    EXPECT_TRUE(e2.empty());
    ASSERT_EQ(e3.size(), 1u);
    EXPECT_FALSE(e3.front().opened);
    ASSERT_EQ(e4.size(), 1u);
    EXPECT_TRUE(e4.front().opened);
    EXPECT_EQ(e4.front().k, -1);
    EXPECT_EQ(m.checks, 4);
    EXPECT_EQ(m.steps, 3);
    const RingFixNnMonitorLink& L = m.links.at({3, "E11", "L1C", "L5Q"});
    EXPECT_EQ(L.epochs, 3);
    EXPECT_EQ(L.episodes, 2);
    EXPECT_TRUE(L.on);
    EXPECT_DOUBLE_EQ(L.first_tow, 100.0);
    EXPECT_DOUBLE_EQ(L.last_tow, 190.0);
    EXPECT_NEAR(L.sum_gf_m, 0.45 + 0.44 - 0.46, 1e-12);
}

TEST(RingFixNearNullMonitor, ALargeIonosphereFreeDeviationIsNotAStep) {
    RingFixNnMonitor m;
    const auto e = ringFixNnMonitorStep(m, {verdict(1, 1, 0.45, 0.60)}, 100.0, 0.30);
    EXPECT_TRUE(e.empty());
    EXPECT_EQ(m.steps, 0);
    EXPECT_EQ(m.links.at({1, "E11", "L1C", "L5Q"}).epochs, 0);
}

TEST(RingFixNearNullMonitor, AChordWithoutAVerdictKeepsItsState) {
    RingFixNnMonitor m;
    ringFixNnMonitorStep(m, {verdict(2, 1, 0.45, 0.0)}, 100.0, 0.30);
    const auto e = ringFixNnMonitorStep(m, {}, 130.0, 0.30);   // no verdict this epoch
    EXPECT_TRUE(e.empty());
    EXPECT_TRUE(m.links.at({2, "E11", "L1C", "L5Q"}).on);
}
