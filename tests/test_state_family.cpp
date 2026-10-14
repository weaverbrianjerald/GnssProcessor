/**
 * @file test_state_family.cpp
 * @brief The declared table of dynamic state families (card UOM step U1a).
 *
 * Guards the table itself (one row per family, unique serialized names) and the
 * StateManager loops that read it: the state size, the labels, the index shift
 * after a removal and the RT-restore refusal of an unserialized family.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <set>
#include <string>

using namespace GnssProcessor;

TEST(StateFamilyTest, TableListsEveryFamilyOnceWithUniqueSerializedNames) {
    std::set<std::string> prefixes, tags;
    for (std::size_t i = 0; i < kStateFamilies.size(); ++i) {
        const auto& s = kStateFamilies[i];
        EXPECT_EQ(static_cast<std::size_t>(s.family), i);
        EXPECT_TRUE(prefixes.insert(s.label_prefix).second) << s.label_prefix;
        if (s.snapshot_tag) EXPECT_TRUE(tags.insert(s.snapshot_tag).second) << s.snapshot_tag;
    }
    EXPECT_EQ(kStateFamilies.size(), kStateFamilyCount);
}

TEST(StateFamilyTest, SerializedNamesAreTheHistoricalContracts) {
    // The label prefixes key the normal-equation accumulators and the RHS
    // attribution; the tags are written to RT snapshot files.  Both predate the
    // table and must not change.
    EXPECT_STREQ(specOf(StateFamily::Ambiguity).label_prefix, "AMB:");
    EXPECT_STREQ(specOf(StateFamily::Ionosphere).label_prefix, "IONO:");
    EXPECT_STREQ(specOf(StateFamily::IonosphereRate).label_prefix, "IONORATE:");
    EXPECT_STREQ(specOf(StateFamily::ReceiverCodeBias).label_prefix, "CBIAS:");
    EXPECT_STREQ(specOf(StateFamily::SatBias).label_prefix, "SATB:");
    EXPECT_STREQ(specOf(StateFamily::SatPhaseBias).label_prefix, "SATPB:");
    EXPECT_STREQ(specOf(StateFamily::ReceiverPhaseBias).label_prefix, "RPB:");
    EXPECT_STREQ(specOf(StateFamily::PhaseColour).label_prefix, "PCOL:");
    EXPECT_STREQ(specOf(StateFamily::SatClock).label_prefix, "SATCLK:");

    EXPECT_EQ(stateFamilyFromSnapshotTag("ION"), StateFamily::Ionosphere);
    EXPECT_EQ(stateFamilyFromSnapshotTag("IONR"), StateFamily::IonosphereRate);
    EXPECT_EQ(stateFamilyFromSnapshotTag("AMB"), StateFamily::Ambiguity);
    EXPECT_EQ(stateFamilyFromSnapshotTag("RXCB"), StateFamily::ReceiverCodeBias);
    EXPECT_EQ(stateFamilyFromSnapshotTag("SATB"), StateFamily::SatBias);
    EXPECT_EQ(stateFamilyFromSnapshotTag("SATPB"), StateFamily::SatPhaseBias);
    EXPECT_EQ(stateFamilyFromSnapshotTag("RXPB"), StateFamily::ReceiverPhaseBias);
    EXPECT_EQ(stateFamilyFromSnapshotTag("SATCLK"), StateFamily::SatClock);
    EXPECT_FALSE(stateFamilyFromSnapshotTag("PCOL").has_value());
    EXPECT_FALSE(stateFamilyFromSnapshotTag("").has_value());
    EXPECT_EQ(specOf(StateFamily::PhaseColour).snapshot_tag, nullptr);
}

TEST(StateFamilyTest, StateManagerLoopsReadEveryFamilyThroughTheTable) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{ 5, lm, cfg };
    const std::size_t core = sm.getStateSize();

    ASSERT_TRUE(sm.restoreDynamicState(StateFamily::Ionosphere, "G01", 1.0, core + 0));
    ASSERT_TRUE(sm.restoreDynamicState(StateFamily::Ambiguity, "G01_L1C", 2.0, core + 1));
    ASSERT_TRUE(sm.restoreDynamicState(StateFamily::SatClock, "G01|SC", 3.0, core + 2));
    ASSERT_TRUE(sm.restoreDynamicState(StateFamily::ReceiverCodeBias, "GPS_IFB5", 4.0, core + 3));

    // indicesOf returns the same map the named getter returns.
    EXPECT_EQ(&sm.indicesOf(StateFamily::Ambiguity), &sm.getAmbiguities());
    EXPECT_EQ(&sm.indicesOf(StateFamily::PhaseColour), &sm.getPhaseColourStates());
    EXPECT_EQ(&sm.indicesOf(StateFamily::SatClock), &sm.getSatClockStates());

    // Size: core plus every family's map.
    EXPECT_EQ(sm.getStateSize(), core + 4);

    // Labels: each dynamic row carries its family's prefix.
    const auto labels = sm.getStateLabels();
    ASSERT_EQ(labels.size(), core + 4);
    EXPECT_EQ(labels[core + 0], "IONO:G01");
    EXPECT_EQ(labels[core + 1], "AMB:G01_L1C");
    EXPECT_EQ(labels[core + 2], "SATCLK:G01|SC");
    EXPECT_EQ(labels[core + 3], "CBIAS:GPS_IFB5");

    // Removing a row shifts every family's later indices.
    sm.removeAmbiguity("G01_L1C");
    EXPECT_EQ(sm.getStateSize(), core + 3);
    EXPECT_EQ(sm.getIonosphereStates().at("G01"), core + 0);
    EXPECT_EQ(sm.getSatClockStates().at("G01|SC"), core + 1);
    EXPECT_EQ(sm.getCodeBiasStates().at("GPS_IFB5"), core + 2);
}

TEST(StateFamilyTest, RestoreRefusesAFamilyTheSnapshotDoesNotSerialize) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{ 5, lm, cfg };
    const std::size_t core = sm.getStateSize();
    EXPECT_FALSE(sm.restoreDynamicState(StateFamily::PhaseColour, "G01", 0.0, core));
    EXPECT_EQ(sm.getStateSize(), core);
}
