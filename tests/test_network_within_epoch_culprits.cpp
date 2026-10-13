/**
 * @file test_network_within_epoch_culprits.cpp
 * @brief Unit gates of the within-epoch culprit hand-off (card WCF;
 *        filter/NetworkWithinEpochCulprits.hpp).
 *
 * Suite @c WithinEpochCulprits (the set the T2 block writes and the DD-carry rebuild reads):
 *  - a whole-satellite culprit names that satellite at every station, and no other;
 *  - an arc culprit names its satellite at its own station only;
 *  - arcCount sums the arcs over the stations;
 *  - an excluded station names no ambiguity (the rebuild never sees it) but is not empty;
 *  - clear names nothing again, and a fresh epoch workspace starts empty.
 *
 * Suite @c WithinEpochCulpritVerdict (the run-end reading of the two counters):
 *  - both counts 0: NoCulprit, at info level;
 *  - equal positive counts: Agree, at info level;
 *  - re-solves with culprits and no rebuild that read one: INERT WIRING, at warn level;
 *  - any other disagreement: OutOfStep, at warn level;
 *  - the withheld totals do not move the verdict.
 *
 * Both suite names are owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "filter/NetworkRunTypes.hpp"             // EpochWorkspace
#include "filter/NetworkWithinEpochCulprits.hpp"

#include <string>
#include <utility>

namespace GnssProcessor {
namespace {

TEST(WithinEpochCulprits, WholeSatelliteNamesItAtEveryStation) {
    WithinEpochCulprits c;
    c.sats.insert("G05");
    EXPECT_TRUE(c.names(0, "G05"));
    EXPECT_TRUE(c.names(7, "G05"));
    EXPECT_FALSE(c.names(0, "G06"));
    EXPECT_TRUE(c.namesAmbiguities());
    EXPECT_FALSE(c.empty());
    EXPECT_EQ(c.arcCount(), 0u);
}

TEST(WithinEpochCulprits, ArcNamesItsSatelliteAtItsStationOnly) {
    WithinEpochCulprits c;
    c.arcs[3].insert("E11");
    EXPECT_TRUE(c.names(3, "E11"));
    EXPECT_FALSE(c.names(4, "E11"));
    EXPECT_FALSE(c.names(3, "E12"));
    EXPECT_TRUE(c.namesAmbiguities());
    EXPECT_FALSE(c.empty());
    EXPECT_EQ(c.arcCount(), 1u);
}

TEST(WithinEpochCulprits, ArcCountSumsOverStations) {
    WithinEpochCulprits c;
    c.arcs[1] = {"G01", "G02"};
    c.arcs[4] = {"G01"};
    c.arcs[6];   // a station entry with no satellite names no arc
    EXPECT_EQ(c.arcCount(), 3u);
    c.arcs.erase(1);
    c.arcs.erase(4);
    EXPECT_EQ(c.arcCount(), 0u);
    EXPECT_FALSE(c.namesAmbiguities());
}

TEST(WithinEpochCulprits, EmptyAndAnExcludedStation) {
    WithinEpochCulprits c;
    EXPECT_TRUE(c.empty());
    EXPECT_FALSE(c.namesAmbiguities());
    EXPECT_FALSE(c.names(0, "G05"));
    // An excluded station is out of the re-solve's estimator: it names no ambiguity for the
    // rebuild, yet the set is not empty (the fixed-cycle ledger reads it).
    c.stations.insert(2);
    EXPECT_FALSE(c.empty());
    EXPECT_FALSE(c.namesAmbiguities());
    EXPECT_FALSE(c.names(2, "G05"));
}

TEST(WithinEpochCulprits, ClearNamesNothing) {
    WithinEpochCulprits c;
    c.sats.insert("G05");
    c.arcs[3].insert("E11");
    c.stations.insert(2);
    c.clear();
    EXPECT_TRUE(c.empty());
    EXPECT_FALSE(c.namesAmbiguities());
    EXPECT_EQ(c.arcCount(), 0u);
    EXPECT_FALSE(c.names(0, "G05"));
    EXPECT_FALSE(c.names(3, "E11"));
}

TEST(WithinEpochCulprits, AFreshEpochWorkspaceNamesNothing) {
    // The workspace is built fresh every epoch, so the epoch's primary DD-carry build reads
    // an empty set.
    EpochWorkspace ws;
    EXPECT_TRUE(ws.we_culprits.empty());
}

/// A tally with the two hand-off counts set and the withheld totals at @p withheld.
WithinEpochCulpritTally tally(long long resolves, long long rebuilds, long long withheld = 0) {
    WithinEpochCulpritTally t;
    t.resolves = resolves;
    t.rebuilds = rebuilds;
    t.sats = withheld;
    t.arcs = withheld;
    t.ambs = withheld;
    return t;
}

TEST(WithinEpochCulpritVerdict, NoCulpritWhenBothCountsAreZero) {
    const WithinEpochCulpritReading r = withinEpochCulpritVerdict(tally(0, 0));
    EXPECT_EQ(r.verdict, WithinEpochCulpritVerdict::NoCulprit);
    EXPECT_FALSE(r.warn);
    ASSERT_NE(r.text, nullptr);
}

TEST(WithinEpochCulpritVerdict, AgreeWhenTheCountsMatch) {
    const WithinEpochCulpritReading r = withinEpochCulpritVerdict(tally(4, 4, 9));
    EXPECT_EQ(r.verdict, WithinEpochCulpritVerdict::Agree);
    EXPECT_FALSE(r.warn);
}

TEST(WithinEpochCulpritVerdict, InertWiringWhenNoRebuildReadACulprit) {
    // The defect card WCF fixed: re-solves with culprits, no rebuild that saw one.
    const WithinEpochCulpritReading r = withinEpochCulpritVerdict(tally(3, 0));
    EXPECT_EQ(r.verdict, WithinEpochCulpritVerdict::InertWiring);
    EXPECT_TRUE(r.warn);
    ASSERT_NE(r.text, nullptr);
    EXPECT_NE(std::string(r.text).find("INERT WIRING"), std::string::npos);
}

TEST(WithinEpochCulpritVerdict, OutOfStepOnAnyOtherDisagreement) {
    for (const auto& [resolves, rebuilds] : {std::pair<long long, long long>{3, 2},
                                             std::pair<long long, long long>{2, 3},
                                             std::pair<long long, long long>{0, 2}}) {
        const WithinEpochCulpritReading r = withinEpochCulpritVerdict(tally(resolves, rebuilds));
        EXPECT_EQ(r.verdict, WithinEpochCulpritVerdict::OutOfStep) << resolves << " " << rebuilds;
        EXPECT_TRUE(r.warn);
        ASSERT_NE(r.text, nullptr);
        EXPECT_EQ(std::string(r.text).find("INERT WIRING"), std::string::npos);
    }
}

TEST(WithinEpochCulpritVerdict, TheWithheldTotalsDoNotMoveTheVerdict) {
    EXPECT_EQ(withinEpochCulpritVerdict(tally(0, 0, 5)).verdict,
              WithinEpochCulpritVerdict::NoCulprit);
    EXPECT_EQ(withinEpochCulpritVerdict(tally(2, 2, 0)).verdict,
              WithinEpochCulpritVerdict::Agree);
}

} // namespace
} // namespace GnssProcessor
