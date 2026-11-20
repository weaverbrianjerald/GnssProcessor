/**
 * @file test_ar_stage_group_pairs.cpp
 * @brief Unit tests of stage S1 of the WL-constrained user resolver,
 *        @c AmbiguityResolver::groupSatellitePairs_ (rules W1 and W2 of the user fixing audit).
 *
 * The stage gathers the epoch's DD pairs by satellite pair ("sat_i|sat_ref") with the
 * frequency and wavelength of each signal, and splits each satellite pair's signals into
 * its two primary signals and its extras.  The cases:
 *   - the groups come out in the order of their keys, each with its signals in pair order;
 *   - a pair whose satellite has no observation is dropped (W1);
 *   - a signal without a known frequency is dropped (W1);
 *   - two signals make a usable split with the higher frequency first (W2);
 *   - three signals with the extra signals off make an unusable group (W2);
 *   - with the extra signals on, the third signal becomes an extra;
 *   - the epoch time and the log flag come from the observations, and are 0 and false
 *     without observations.
 *
 * Suite name @c ArStageGroupPairs is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "support/ArWlcStageAccess.hpp"

#include <string>
#include <vector>

using namespace GnssProcessor;
using gnss_test::ArScene;
using gnss_test::ArStageRig;
using Access = ArWlcStageTestAccess;

namespace {

AmbiguityResolverConfig baseConfig() {
    AmbiguityResolverConfig c;
    c.ar_mode       = 1;
    c.wl_from_float = true;
    return c;
}

TEST(ArStageGroupPairs, GroupsComeInKeyOrderWithTheirSignalsInPairOrder) {
    ArStageRig rig(baseConfig());
    const ArScene s("G12", { { "G15", 3.0, 1.0 }, { "G05", 7.0, 2.0 } });
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 2u);
    EXPECT_EQ(ep.groups[0].key, "G05|G12");
    EXPECT_EQ(ep.groups[1].key, "G15|G12");
    // G05 is the second satellite of the scene: its pairs are 2 (L1C) and 3 (L2W).
    ASSERT_EQ(ep.groups[0].signals.size(), 2u);
    EXPECT_EQ(ep.groups[0].signals[0].pair_idx, 2);
    EXPECT_EQ(ep.groups[0].signals[0].signal, "L1C");
    EXPECT_EQ(ep.groups[0].signals[1].pair_idx, 3);
    EXPECT_DOUBLE_EQ(ep.groups[0].signals[0].freq, gnss_test::kFL1);
    EXPECT_DOUBLE_EQ(ep.groups[0].signals[1].freq, gnss_test::kFL2);
    EXPECT_NEAR(ep.groups[0].signals[1].wavelength, Constants::SPEED_OF_LIGHT / gnss_test::kFL2, 1e-12);
}

TEST(ArStageGroupPairs, TwoSignalsMakeAUsableSplitWithTheHigherFrequencyFirst) {
    ArStageRig rig(baseConfig());
    ArScene s("G12", { { "G15", 3.0, 1.0 } });
    std::swap(s.pairs[0], s.pairs[1]);   // L2W before L1C in the pair list
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    const auto& g = ep.groups[0];
    ASSERT_TRUE(g.split.usable);
    EXPECT_EQ(g.signals[g.split.hi].signal, "L1C");
    EXPECT_EQ(g.signals[g.split.lo].signal, "L2W");
    EXPECT_TRUE(g.split.extras.empty());
}

TEST(ArStageGroupPairs, APairWithoutAnObservationIsDropped) {
    ArStageRig rig(baseConfig());
    ArScene s("G12", { { "G15", 3.0, 1.0 }, { "G05", 7.0, 2.0 } });
    s.obs.erase(s.obs.begin());   // G15 not observed
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    EXPECT_EQ(ep.groups[0].key, "G05|G12");
}

TEST(ArStageGroupPairs, ASignalWithoutAFrequencyIsDropped) {
    ArStageRig rig(baseConfig());
    ArScene s("G12", { { "G15", 3.0, 1.0 } });
    s.pairs[1].signal = "L9Z";   // no such GPS band
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    ASSERT_EQ(ep.groups[0].signals.size(), 1u);
    EXPECT_FALSE(ep.groups[0].split.usable);
}

TEST(ArStageGroupPairs, ThreeSignalsWithTheExtraSignalsOffMakeAnUnusableGroup) {
    ArStageRig rig(baseConfig());
    ArScene s("G12", { { "G15", 3.0, 1.0 } });
    s.pairs.push_back({ 4, 4, "G15", "G12", "L5Q" });
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    EXPECT_EQ(ep.groups[0].signals.size(), 3u);
    EXPECT_FALSE(ep.groups[0].split.usable);
}

TEST(ArStageGroupPairs, WithTheExtraSignalsOnTheThirdSignalIsAnExtra) {
    AmbiguityResolverConfig c = baseConfig();
    c.extra_signals = true;
    ArStageRig rig(c);
    ArScene s("G12", { { "G15", 3.0, 1.0 } });
    s.pairs.push_back({ 4, 4, "G15", "G12", "L5Q" });
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    const auto& g = ep.groups[0];
    ASSERT_TRUE(g.split.usable);
    EXPECT_EQ(g.signals[g.split.hi].signal, "L1C");
    EXPECT_EQ(g.signals[g.split.lo].signal, "L2W");
    ASSERT_EQ(g.split.extras.size(), 1u);
    EXPECT_EQ(g.signals[g.split.extras[0]].signal, "L5Q");
}

TEST(ArStageGroupPairs, TheEpochTimeAndTheLogFlagComeFromTheObservations) {
    ArStageRig rig(baseConfig());
    const ArScene s("G12", { { "G15", 3.0, 1.0 } });
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    EXPECT_DOUBLE_EQ(ep.tow, 345600.0);
    EXPECT_TRUE(ep.log_lines);

    Access::Epoch none;
    Access::group(*rig.resolver, s.pairs, {}, none);
    EXPECT_DOUBLE_EQ(none.tow, 0.0);
    EXPECT_FALSE(none.log_lines);
    EXPECT_TRUE(none.groups.empty());   // no observation: every pair dropped (W1)
}

TEST(ArStageGroupPairs, GalileoE1E5aIsUsable) {
    ArStageRig rig(baseConfig());
    const ArScene s("E26", { { "E08", 2.0, 1.0 } }, System::Galileo, "L1X", "L5X",
                    gnss_test::kFL1, gnss_test::kFL5);
    Access::Epoch ep;
    Access::group(*rig.resolver, s.pairs, s.obs, ep);
    ASSERT_EQ(ep.groups.size(), 1u);
    EXPECT_EQ(ep.groups[0].key, "E08|E26");
    EXPECT_TRUE(ep.groups[0].split.usable);
    EXPECT_DOUBLE_EQ(ep.groups[0].signals[1].freq, gnss_test::kFL5);
}

} // namespace
