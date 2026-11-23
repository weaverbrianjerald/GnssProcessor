/**
 * @file test_unrecorded_sat_gate_edges.cpp
 * @brief Unit gates for the edge facts and the solvable floor of the consumer's rule for
 *        satellites without their stream records (card USC,
 *        docs/cards/user-ar/USC-USER-SATELLITE-WITHOUT-RECORD.md): the cause of a Drop, the age of
 *        the last record, the reference satellite lost, the gap of a Readmit, the counters by bin,
 *        the epochs without a corrected solution with their two edges, and the memory a warm resume
 *        restores.  The first edges of the rule are gated in tests/test_unrecorded_sat_gate.cpp.
 */

#include "realtime/UnrecordedSatGate.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using GnssProcessor::UnrecordedSatGate;

namespace {

using Missing   = UnrecordedSatGate::Missing;
using Kind      = UnrecordedSatGate::EventKind;
using Cause     = UnrecordedSatGate::DropCause;
using States    = std::vector<UnrecordedSatGate::SatState>;

UnrecordedSatGate::SatState ok(const std::string& sat) {
    UnrecordedSatGate::SatState s;
    s.sat = sat;
    return s;
}

UnrecordedSatGate::SatState noClock(const std::string& sat, double age_s = -1.0, bool ref = false) {
    UnrecordedSatGate::SatState s;
    s.sat           = sat;
    s.missing       = Missing::Clock;
    s.record_age_s  = age_s;
    s.was_reference = ref;
    return s;
}

/// Five recorded satellites besides the ones a test varies, so the floor of 5 is met.
States withFive(States extra) {
    States s = { ok("G01"), ok("G02"), ok("G03"), ok("G04"), ok("G05") };
    s.insert(s.end(), extra.begin(), extra.end());
    return s;
}

}  // namespace

TEST(UnrecordedSatGateEdges, DropCausesAreFirstSightingRecordLostAndBackOnStale) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    // E01 is seen first without a record; E02 in view with its record, then loses it; E03 seen
    // with its record, out of view for an epoch, back without a usable record.
    auto ev = g.step(withFive({ noClock("E01"), ok("E02"), ok("E03") }), out, 0.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].cause, Cause::FirstSighting);
    EXPECT_TRUE(ev[0].first_sighting);
    ev = g.step(withFive({ noClock("E01"), noClock("E02", 60.0) }), out, 30.0, 5);   // E03 out of view
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].sat, "E02");
    EXPECT_EQ(ev[0].cause, Cause::RecordLost);
    ev = g.step(withFive({ noClock("E01"), noClock("E02", 90.0), noClock("E03", 3390.0) }), out, 60.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].sat, "E03");
    EXPECT_EQ(ev[0].cause, Cause::BackOnStale);
    EXPECT_DOUBLE_EQ(ev[0].record_age_s, 3390.0);
    const auto& c = g.counters();
    EXPECT_EQ(c.drops, 3u);
    EXPECT_EQ(c.drops_first_sighting, 1u);
    EXPECT_EQ(c.drops_record_lost, 1u);
    EXPECT_EQ(c.drops_back_on_stale, 1u);
    EXPECT_EQ(c.drops_by_age[0], 1u);   // no record
    EXPECT_EQ(c.drops_by_age[1], 1u);   // 60 s
    EXPECT_EQ(c.drops_by_age[4], 1u);   // 3390 s
}

TEST(UnrecordedSatGateEdges, ReferenceSatelliteLostIsCountedAndNamed) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    g.step(withFive({ ok("E25") }), out, 0.0, 5);
    const auto ev = g.step(withFive({ noClock("E25", 45.0, true) }), out, 30.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_TRUE(ev[0].was_reference);
    EXPECT_EQ(g.counters().references_lost, 1u);
}

TEST(UnrecordedSatGateEdges, ReadmitCarriesTheGapAndItsBin) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    g.step(withFive({ ok("E25") }), out, 0.0, 5);
    g.step(withFive({ noClock("E25", 60.0) }), out, 30.0, 5);
    g.step(withFive({ noClock("E25", 90.0) }), out, 60.0, 5);
    const auto ev = g.step(withFive({ ok("E25") }), out, 210.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, Kind::Readmit);
    EXPECT_DOUBLE_EQ(ev[0].gap_s, 180.0);
    EXPECT_EQ(g.counters().readmits_by_gap[1], 1u);   // <= 5 min
    EXPECT_TRUE(g.dropTimes().empty());
    EXPECT_TRUE(out.empty());
}

TEST(UnrecordedSatGateEdges, ReadmitWithoutEpochTimesHasAnUnknownGap) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    g.step(std::vector<std::pair<std::string, bool>>{ { "G01", false } }, out);
    const auto ev = g.step(std::vector<std::pair<std::string, bool>>{ { "G01", true } }, out);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_LT(ev[0].gap_s, 0.0);
    EXPECT_EQ(g.counters().readmits_by_gap[3], 1u);   // unknown counts as longer
}

TEST(UnrecordedSatGateEdges, BelowTheFloorEverySatelliteIsOutWithTwoEdges) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    const States all = { ok("G01"), ok("G02"), ok("G03"), ok("G04"), ok("G05"), ok("E01") };
    EXPECT_TRUE(g.step(all, out, 0.0, 5).empty());
    EXPECT_FALSE(g.noSolution());
    // The stream stops: no satellite keeps its record.
    States none;
    for (const auto& s : all) none.push_back(noClock(s.sat, 60.0));
    auto ev = g.step(none, out, 30.0, 5);
    ASSERT_EQ(ev.size(), 7u);                          // six Drops, then the floor's edge
    EXPECT_EQ(ev.back().kind, Kind::NoSolutionBegin);
    EXPECT_EQ(ev.back().kept, 0u);
    EXPECT_TRUE(g.noSolution());
    EXPECT_EQ(out.size(), 6u);
    ev = g.step(none, out, 60.0, 5);
    EXPECT_TRUE(ev.empty());                           // no second edge, no second Drop
    EXPECT_EQ(out.size(), 6u);
    // The stream returns.
    ev = g.step(all, out, 630.0, 5);
    ASSERT_EQ(ev.size(), 7u);                          // six Readmits, then the floor's edge
    EXPECT_EQ(ev.back().kind, Kind::NoSolutionEnd);
    EXPECT_EQ(ev.back().kept, 6u);
    EXPECT_DOUBLE_EQ(ev.front().gap_s, 600.0);
    EXPECT_FALSE(g.noSolution());
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(g.counters().epochs_no_solution, 2u);
    EXPECT_EQ(g.counters().no_solution_spells, 1u);
}

TEST(UnrecordedSatGateEdges, BelowTheFloorRecordedSatellitesAreOutButNotDropped) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    const auto ev = g.step(States{ ok("G01"), ok("G02"), noClock("G03", 120.0) }, out, 0.0, 5);
    ASSERT_EQ(ev.size(), 2u);                          // G03's Drop, then the floor's edge
    EXPECT_EQ(ev[0].kind, Kind::Drop);
    EXPECT_EQ(ev[1].kind, Kind::NoSolutionBegin);
    EXPECT_EQ(ev[1].kept, 2u);
    EXPECT_EQ(out, (std::set<std::string>{ "G01", "G02", "G03" }));
    EXPECT_EQ(g.dropped(), (std::set<std::string>{ "G03" }));   // G01, G02 keep their records
}

TEST(UnrecordedSatGateEdges, FloorOfZeroNeverEndsTheSolution) {
    UnrecordedSatGate g;
    std::set<std::string> out;
    const auto ev = g.step(States{ noClock("G01"), noClock("G02") }, out, 0.0, 0);
    EXPECT_EQ(ev.size(), 2u);
    EXPECT_FALSE(g.noSolution());
    EXPECT_EQ(g.counters().epochs_no_solution, 0u);
}

TEST(UnrecordedSatGateEdges, RestoreCarriesTheDroppedSetAndTheRecordedSet) {
    UnrecordedSatGate g;
    g.restore(std::map<std::string, double>{ { "E25", 100.0 } }, std::set<std::string>{ "E25", "E11" });
    EXPECT_EQ(g.dropped(), (std::set<std::string>{ "E25" }));
    std::set<std::string> out;
    // Still without its record: no second Drop.
    auto ev = g.step(withFive({ noClock("E25", 200.0) }), out, 200.0, 5);
    EXPECT_TRUE(ev.empty());
    EXPECT_EQ(out.count("E25"), 1u);
    ev = g.step(withFive({ ok("E25") }), out, 400.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, Kind::Readmit);
    EXPECT_DOUBLE_EQ(ev[0].gap_s, 300.0);
    // A satellite recorded before the resume and seen without its record after it is no first
    // sighting; the previous epoch's set is not carried, so it reads as back on a stale record.
    ev = g.step(withFive({ noClock("E11", 60.0) }), out, 430.0, 5);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_FALSE(ev[0].first_sighting);
    EXPECT_EQ(ev[0].cause, Cause::BackOnStale);
}

TEST(UnrecordedSatGateEdges, BinsAndNames) {
    EXPECT_EQ(UnrecordedSatGate::ageBin(-1.0), 0u);
    EXPECT_EQ(UnrecordedSatGate::ageBin(std::nan("")), 0u);
    EXPECT_EQ(UnrecordedSatGate::ageBin(31.0), 1u);
    EXPECT_EQ(UnrecordedSatGate::ageBin(300.0), 2u);
    EXPECT_EQ(UnrecordedSatGate::ageBin(1800.0), 3u);
    EXPECT_EQ(UnrecordedSatGate::ageBin(1801.0), 4u);
    EXPECT_EQ(UnrecordedSatGate::gapBin(60.0), 0u);
    EXPECT_EQ(UnrecordedSatGate::gapBin(61.0), 1u);
    EXPECT_EQ(UnrecordedSatGate::gapBin(1800.0), 2u);
    EXPECT_EQ(UnrecordedSatGate::gapBin(1801.0), 3u);
    EXPECT_STREQ(UnrecordedSatGate::causeName(Cause::BackOnStale), "back on a stale record");
}
