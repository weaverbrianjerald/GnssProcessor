/**
 * @file test_ledger_anchor.cpp
 * @brief The anchor election's re-entry rule, card SID phase 4b
 *        (@c network.ledger_anchor, docs/refound/SID_signal_identity_2026-09-22.md).
 *
 * The election decides "first sighting or return" by one test: whether the key had
 * an incumbent at the previous epoch.  A family the coverage gate dropped loses its
 * incumbent at the drop edge, so its return reads as a birth however continuously
 * its arcs tracked through the gated era.  Phase 4b replaces that test for one
 * population only: a family a drop edge erased, returning with the previous
 * incumbent's arc at the same admitted generation.
 *
 * The rule itself lives in @c anchorReentryClassify, a function without side effects
 * over the ledger and the drop memo, so it can be stated here without a station, a
 * model or a solve.  What the election adds on top is its own eligibility (does the station
 * still track the key, is it banned, does it pass the hygiene and basis rules), and
 * that composition is what the election's own suites and the armed A/B cover.
 */

#include <gtest/gtest.h>

#include "network/state/Entities.hpp"
#include "network/state/ArcLedger.hpp"
#include "network/solve/AnchorReentry.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kDt = 30.0;   // the producer's epoch interval on both beds

ArcLedger::Key K(int sta, const char* sat, const char* sig) {
    return ArcLedger::Key{ sta, std::string(sat), std::string(sig) };
}

/// Advance the admitted side by one epoch over the listed arcs.  The raw side is
/// driven alongside so an arc is never admitted without having been observed,
/// which is the producer's own ordering.
void admit(ArcLedger& led, long long ep, const std::vector<ArcLedger::Key>& arcs) {
    // Every station keeps one arc of a satellite no test queries.  The ledger
    // freezes the arcs of a station that contributed nothing (so a station outage
    // does not create a gap at every one of its arcs), and without a keeper a test
    // that drops a station's only arc would exercise the freeze rather than the gap.
    std::vector<ArcLedger::Key> all = arcs;
    for (int sta = 0; sta < 3; ++sta) all.push_back(K(sta, "G99", "L9X"));
    led.beginEpoch(ep, static_cast<double>(ep) * kDt);
    for (const auto& k : all) led.observe(k, /*modelled=*/true);
    std::set<int> present;
    for (const auto& k : all) present.insert(std::get<0>(k));
    std::vector<std::pair<ArcLedger::Key, ArcStep>> raw_edges, adm_edges;
    led.endEpoch(present, raw_edges);
    led.admitEpoch(std::set<ArcLedger::Key>(all.begin(), all.end()), adm_edges);
}

/// The station-name to index map the election builds for the memo lookup.
std::map<std::string, int> idx() {
    return { { "STA0", 0 }, { "STA1", 1 }, { "STA2", 2 } };
}

}  // namespace

// ---------------------------------------------------------------------------
// The classification
// ---------------------------------------------------------------------------

TEST(LedgerAnchorTest, SameGenerationReturnRestoresTheIncumbent) {
    // The population phase 4b exists for: the family kept tracking through the
    // gated era, so the arc the incumbent anchored is the arc that came back and
    // the election has no reason to re-realize the datum on another station.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    for (long long ep = 0; ep <= 3; ++ep) admit(led, ep, { a, b });

    AnchorReentryState st;
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "STA0", 1, 3);
    ASSERT_EQ(st.drop.count("G30_L1W"), 1u);
    EXPECT_EQ(st.drop["G30_L1W"].n_tracking, 2);
    EXPECT_EQ(st.drop["G30_L1W"].generation.at(0), 1);

    // The gated era: the rows keep coming, which is card EV7's own mechanism.
    for (long long ep = 4; ep <= 8; ++ep) admit(led, ep, { a, b });

    int sta = -1;
    const auto cls = anchorReentryClassify(led, st.drop["G30_L1W"], "G30", "L1W",
                                           idx(), &sta);
    EXPECT_EQ(cls, AnchorReentryClass::SameGeneration);
    EXPECT_EQ(sta, 0) << "the restore target is the station the memo names";
}

TEST(LedgerAnchorTest, NewGenerationReturnElectsFresh) {
    // The incumbent's arc died and was re-born inside the gated era, so its carry
    // means something else now: the election proceeds as it would without the rule.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);   // short, so the gap kills
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    for (long long ep = 0; ep <= 3; ++ep) admit(led, ep, { a, b });

    AnchorReentryState st;
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "STA0", 1, 3);
    EXPECT_EQ(st.drop["G30_L1W"].generation.at(0), 1);

    // Station 0 loses the arc for long enough to die, then it returns re-born.
    for (long long ep = 4; ep <= 8; ++ep) admit(led, ep, { b });
    ASSERT_EQ(led.find(a)->adm_state, ArcState::Dead);
    admit(led, 9, { a, b });
    EXPECT_EQ(led.find(a)->adm_generation, 2);

    int sta = 99;
    const auto cls = anchorReentryClassify(led, st.drop["G30_L1W"], "G30", "L1W",
                                           idx(), &sta);
    EXPECT_EQ(cls, AnchorReentryClass::NewGeneration);
    EXPECT_EQ(sta, -1) << "a new-generation return offers no restore target";
}

TEST(LedgerAnchorTest, AGappedIncumbentIsNotARestoreTarget) {
    // The incumbent's arc is inside its gap, not back: restoring a station that is
    // not tracking the key would pin a column it does not feed.  The classifier
    // refuses it before the election's own eligibility ever runs.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    for (long long ep = 0; ep <= 3; ++ep) admit(led, ep, { a, b });

    AnchorReentryState st;
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "STA0", 1, 3);
    for (long long ep = 4; ep <= 6; ++ep) admit(led, ep, { b });
    ASSERT_EQ(led.find(a)->adm_state, ArcState::Gap);

    int sta = 99;
    EXPECT_EQ(anchorReentryClassify(led, st.drop["G30_L1W"], "G30", "L1W", idx(), &sta),
              AnchorReentryClass::NewGeneration);
    EXPECT_EQ(sta, -1);
}

TEST(LedgerAnchorTest, NoDropMemoIsNoMemoryAndNotARestore) {
    // Every other first sighting on the run (a genuine rise, a bridge exit, the
    // first epoch) has no memo and is left to the unchanged election.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    admit(led, 0, { a });

    AnchorDropMemo empty;
    int sta = 99;
    EXPECT_EQ(anchorReentryClassify(led, empty, "G30", "L1W", idx(), &sta),
              AnchorReentryClass::NoMemory);
    EXPECT_EQ(sta, -1);

    // A memo with an epoch but no remembered incumbent is the same statement: the
    // family was gated while nobody anchored it, so there is nothing to restore.
    AnchorDropMemo anon;
    anon.ep = 3;
    EXPECT_EQ(anchorReentryClassify(led, anon, "G30", "L1W", idx(), &sta),
              AnchorReentryClass::NoMemory);
}

TEST(LedgerAnchorTest, AnUnknownIncumbentStationIsNotRestored) {
    // A station that left the roster between the drop and the return cannot be
    // resolved to an index, and inventing one would pin the wrong block.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    for (long long ep = 0; ep <= 3; ++ep) admit(led, ep, { a });

    AnchorReentryState st;
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "GONE", 1, 3);
    int sta = 99;
    EXPECT_EQ(anchorReentryClassify(led, st.drop["G30_L1W"], "G30", "L1W", idx(), &sta),
              AnchorReentryClass::NewGeneration);
    EXPECT_EQ(sta, -1);
}

TEST(LedgerAnchorTest, ASecondDropReplacesTheMemoRatherThanMergingIt) {
    // The question a return answers is about the last era the family was anchored
    // in.  Merging two eras would let an arc that died between them read as
    // continuous, which is the failure the generation test exists to prevent.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    for (long long ep = 0; ep <= 3; ++ep) admit(led, ep, { a, b });

    AnchorReentryState st;
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "STA0", 1, 3);
    for (long long ep = 4; ep <= 6; ++ep) admit(led, ep, { a, b });
    anchorReentryNoteDrop(st, led, "G30_L1W", "G30", "L1W", "STA1", 1, 6);

    EXPECT_EQ(st.drop.size(), 1u);
    EXPECT_EQ(st.drop["G30_L1W"].prev_anchor, "STA1");
    EXPECT_EQ(st.drop["G30_L1W"].ep, 6);
    int sta = -1;
    EXPECT_EQ(anchorReentryClassify(led, st.drop["G30_L1W"], "G30", "L1W", idx(), &sta),
              AnchorReentryClass::SameGeneration);
    EXPECT_EQ(sta, 1);
}

// ---------------------------------------------------------------------------
// The family census, which the drop edge's readout is built on
// ---------------------------------------------------------------------------

TEST(LedgerAnchorTest, FamilyCensusSplitsTrackingGapAndDead) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    const auto c = K(2, "G30", "L1W");
    const auto other = K(0, "G30", "L2W");   // a different family
    for (long long ep = 0; ep <= 2; ++ep) admit(led, ep, { a, b, c, other });

    // b gaps briefly, c gaps long enough to die, a keeps tracking.
    admit(led, 3, { a, c, other });
    for (long long ep = 4; ep <= 7; ++ep) admit(led, ep, { a, b, other });

    int tr = -1, gp = -1, dd = -1;
    std::map<int, int> gen;
    anchorReentryFamilyCensus(led, "G30", "L1W", tr, gp, dd, &gen);
    EXPECT_EQ(tr, 2) << "stations 0 and 1 are tracking";
    EXPECT_EQ(gp, 0);
    EXPECT_EQ(dd, 1) << "station 2 outran the horizon";
    EXPECT_EQ(gen.size(), 2u) << "only the alive arcs carry a generation";
    EXPECT_EQ(gen.at(0), 1);
    EXPECT_EQ(gen.at(1), 1) << "a gap short of the horizon does not re-generate";
}

// ---------------------------------------------------------------------------
// The ledger arc age, which the container reads (B2)
// ---------------------------------------------------------------------------

TEST(LedgerAnchorTest, LedgerArcAgeCountsTheAdmittedTrackingRun) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(3900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G30", "L1W");
    const auto b = K(1, "G30", "L1W");
    for (long long ep = 0; ep <= 5; ++ep) admit(led, ep, { a, b });
    EXPECT_EQ(anchorLedgerArcAge(led, a, 5), 5);

    // An absence resets the age one epoch after the return, which is the same
    // convention the counted map has ("absence resets its age").
    admit(led, 6, { b });
    admit(led, 7, { a, b });
    EXPECT_EQ(anchorLedgerArcAge(led, a, 7), 0) << "Reentered is entered at the return";
    admit(led, 8, { a, b });
    EXPECT_EQ(anchorLedgerArcAge(led, a, 8), 0) << "Tracking is re-entered one epoch later";
    admit(led, 9, { a, b });
    EXPECT_EQ(anchorLedgerArcAge(led, a, 9), 1);

    // An arc the ledger has no record of has no age (-1), which is a different
    // statement from age zero and stays separable from it.
    EXPECT_EQ(anchorLedgerArcAge(led, K(0, "G99", "L1W"), 9), -1);
}

TEST(LedgerAnchorTest, ClassNamesAreStable) {
    EXPECT_STREQ(anchorReentryClassName(AnchorReentryClass::NoMemory), "no_memory");
    EXPECT_STREQ(anchorReentryClassName(AnchorReentryClass::SameGeneration),
                 "same_generation");
    EXPECT_STREQ(anchorReentryClassName(AnchorReentryClass::NewGeneration),
                 "new_generation");
}

}  // namespace GnssProcessor
