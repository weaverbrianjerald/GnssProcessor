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
 * Suite @c WithinEpochCulpritReport (the run-end report's gate and its quiet clause):
 *  - the quiet clause names the case: leveling off, never assessed, an empty phase pool
 *    (the carry_invariant lineage, deferral D1), or rows assessed (no clause);
 *  - armed only with the DD carry and the tier both on;
 *  - off on both goldens, loaded from config/golden/ (the tier armed, the DD carry off).
 *
 * Not certified here: the hand-off itself, i.e. that the DD-carry rebuild reads the object
 * the T2 block writes (the defect card WCF closes).  The rebuild and the T2 block are not
 * unit-testable seams; the hand-off is certified by a run on a carry_invariant posture with
 * the tier armed where T2 trips, with the writer's count non-zero and equal to the
 * reader's.
 *
 * The three suite names are owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"                  // the goldens' knobs (report gate)
#include "network/driver/NetworkRunTypes.hpp"             // EpochWorkspace
#include "network/qc/NetworkWithinEpochCulprits.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
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

TEST(WithinEpochCulpritReport, QuietReasonNamesWhichQuietCaseTheRunIs) {
    // Phase leveling off: the tier never runs.
    EXPECT_NE(withinEpochCulpritQuietReason(false, 0, 0).find("network.phase_leveling"),
              std::string::npos);
    // Leveling on, no assessment: no warm carry.
    EXPECT_NE(withinEpochCulpritQuietReason(true, 0, 0).find("never assessed"),
              std::string::npos);
    // Assessments over an empty pool: the carry_invariant lineage, structurally idle (D1).
    const std::string idle = withinEpochCulpritQuietReason(true, 57, 0);
    EXPECT_NE(idle.find("admitted no row in 57 assessment(s)"), std::string::npos);
    EXPECT_NE(idle.find("deferral D1"), std::string::npos);
    // Rows assessed and no culprit named: a quiet run, nothing to add.
    EXPECT_TRUE(withinEpochCulpritQuietReason(true, 57, 1200).empty());
}

TEST(WithinEpochCulpritReport, ArmedOnlyWithTheDdCarryAndTheTier) {
    EXPECT_TRUE(withinEpochCulpritReportArmed(true, true));
    EXPECT_FALSE(withinEpochCulpritReportArmed(true, false));
    EXPECT_FALSE(withinEpochCulpritReportArmed(false, false));
}

TEST(WithinEpochCulpritReport, OffOnBothGoldens) {
    // Both goldens arm the tier with the DD carry off: the rebuild returns before it reads
    // the sets, both counts stay 0, and the report prints nothing.  The effective knob
    // (NetworkKnobs::carry_invariant) is the configured one and the eq34 phase datum, so
    // the configured value off is enough.
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(
            std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name, log))
            << name;
        EXPECT_TRUE(cfg.networkRaimWithinEpoch()) << name;
        EXPECT_FALSE(cfg.networkCarryInvariant()) << name;
        EXPECT_FALSE(withinEpochCulpritReportArmed(cfg.networkCarryInvariant(),
                                                   cfg.networkRaimWithinEpoch()))
            << name;
    }
}

} // namespace
} // namespace GnssProcessor
