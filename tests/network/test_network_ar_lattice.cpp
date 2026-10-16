/**
 * @file test_network_ar_lattice.cpp
 * @brief Unit gates of the producer AR search's lattice observatory (step AGT-6;
 *        network/ar/ArLattice.hpp).
 *
 * Suite @c NetworkArLattice (the ledger's steps, pure;
 * run here with @c persist_ep 1, the observatory's immediate offers):
 *  - the order: an arc released this epoch is eliminated before the epoch's fixes are offered,
 *    so a DD fixed again on it enters as a new row (offered first, it would contradict);
 *  - the attribution: each released arc carries its cause (vanished, first seen, freed, stamp
 *    change), the reading's changed stamp fields and the free-epoch latch's causes, in ascending
 *    ArcId order; an arc read as an echo or as no change is kept;
 *  - the dimensions: ten held DDs on a released reference lose one dimension and the nine left
 *    still imply the DDs between their satellites; a satellite that vanishes at four stations
 *    costs one dimension per released arc that still holds rows (three), the last arc holding
 *    none;
 *  - a contradiction releases the arcs of its cycle (AGT-6 left the lattice
 *    unchanged) and reports the implied integer;
 *  - the run tallies count stamp fields and free causes on the arc releases that held rows only.
 * The fixed-cycle fold's release seam (the evidence, the cycle releases, the seam gap, the admission
 * streak, the re-validation) is suite @c NetworkFixedCycleRelease
 * (tests/network/test_network_fixed_cycle_release.cpp).
 *
 * Suite @c NetworkArLatticeConfig (the loader): the knob defaults off, arms with the cycle basis
 * and is forced off without it; the two files' headers.
 *
 * Suite names owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/ar/ArLattice.hpp"
#include "network/driver/RunTypes.hpp"     // RelevelEpochState::kCause*
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using arcgraph::Cycle;
using arcgraph::CycleLattice;
using arcgraph::CycleTerm;
using arcgraph::LatticeVerdict;

// ============================== helpers ==============================

SignalId sigOf(const char* code) { return *SignalId::fromObsCode(System::GPS, code); }
SatId gps(int prn) { return SatId{System::GPS, prn}; }
StationId sta(int index) { return StationId{index}; }
ArcId arcOf(int station, int prn, const SignalId& s) { return ArcId{sta(station), gps(prn), s}; }

/// The double difference (r, p; j, k) in one signal: +(r,j) - (r,k) - (p,j) + (p,k).
Cycle dd(int r, int p, int j, int k, const SignalId& s) {
    std::map<ArcId, long long> acc;
    acc[arcOf(r, j, s)] += 1;
    acc[arcOf(r, k, s)] -= 1;
    acc[arcOf(p, j, s)] -= 1;
    acc[arcOf(p, k, s)] += 1;
    Cycle x;
    for (const auto& [a, v] : acc) if (v) x.terms.push_back(CycleTerm{a, static_cast<int>(v)});
    return x;
}

/// A fixed DD as the search hands it over.
ArFixedCycle fixOf(int r, int p, int j, int k, const SignalId& s, long long value) {
    return ArFixedCycle{dd(r, p, j, k, s), value, ArDdIdentity{s, sta(r), sta(p), gps(j), gps(k)}};
}

/// A reading of every arc the lattice holds as an unbroken life.
std::map<ArcId, ArArcReading> continuing(const CycleLattice& L) {
    std::map<ArcId, ArArcReading> out;
    for (const Cycle& row : L.rows)
        for (const CycleTerm& t : row.terms) out[t.arc] = ArArcReading{};
    return out;
}

/// Offer @p fixes to an empty lattice; every one must be added.
CycleLattice heldLattice(const std::vector<ArFixedCycle>& fixes) {
    CycleLattice L;
    for (const ArFixedCycle& f : fixes) EXPECT_EQ(arcgraph::latticeOffer(L, f.cycle, f.value), LatticeVerdict::Added);
    return L;
}

int verdictCount(const ArLatticeEpoch& e, LatticeVerdict v) { return e.verdicts[static_cast<std::size_t>(v)]; }

/// One ledger epoch on @p L with no outside evidence and immediate offers (persist_ep 1): the
/// AGT-6 observatory's step, through the fixed-cycle fold's functions.
ArLatticeEpoch stepOf(CycleLattice& L, const ArEpochFixes& fixes) {
    ArLatticeState S;
    S.lattice = L;
    ArLatticeEpoch e = arLatticeBegin(S, fixes, ArLatticeEvidence{}, 1);
    arLatticeAdmit(S, fixes, 1, 1, e);
    arLatticeClose(S, 1, e);
    L = S.lattice;
    return e;
}

// ============================== the step ==============================

TEST(NetworkArLattice, ReleasesBeforeOfferingSoARefixOfANewLifeIsAdded) {
    const SignalId l1 = sigOf("L1C");
    const CycleLattice held = heldLattice({fixOf(0, 1, 2, 3, l1, 7), fixOf(0, 1, 4, 3, l1, -2)});
    // This epoch (0, G02) is freed by the GF jump trigger, and the search fixes the same DD in
    // its new life at 11.
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(held);
    fixes.arcs[arcOf(0, 2, l1)] = ArArcReading{ArcLifeEdge::Freed, 0u, RelevelEpochState::kCauseGfJump};
    fixes.fixed    = {fixOf(0, 1, 2, 3, l1, 11)};

    // Offered against the old life, the fix contradicts it ...
    CycleLattice offered_first = held;
    EXPECT_EQ(arcgraph::latticeOffer(offered_first, fixes.fixed[0].cycle, 11), LatticeVerdict::Contradicted);
    // ... so the step releases first, and the fix enters the new life.
    CycleLattice L = held;
    const ArLatticeEpoch e = stepOf(L, fixes);
    ASSERT_EQ(e.releases.size(), 1u);
    EXPECT_EQ(e.releases[0].arc, arcOf(0, 2, l1));
    EXPECT_EQ(e.releases[0].cause, ArLatticeRelease::Freed);
    EXPECT_EQ(e.releases[0].rows_touched, 1);
    EXPECT_EQ(e.releases[0].rows_dropped, 1);
    EXPECT_EQ(e.dims_lost, 1);
    EXPECT_EQ(e.released_touching, 1);
    EXPECT_EQ(verdictCount(e, LatticeVerdict::Added), 1);
    EXPECT_EQ(verdictCount(e, LatticeVerdict::Contradicted), 0);
    ASSERT_EQ(e.offers.size(), 1u);
    EXPECT_EQ(e.offers[0].verdict, LatticeVerdict::Added);
    EXPECT_EQ(e.offers[0].len, 4);
    EXPECT_EQ(e.rows, 2);
    // The new life's integer now holds: the DD confirms at 11 and the surviving row at -2.
    EXPECT_EQ(arcgraph::latticeImplied(L, dd(0, 1, 2, 3, l1)), 11);
    EXPECT_EQ(arcgraph::latticeImplied(L, dd(0, 1, 4, 3, l1)), -2);
}

TEST(NetworkArLattice, AttributesEachReleaseToItsReadingInArcOrder) {
    const SignalId l1 = sigOf("L1C"), l2 = sigOf("L2W");
    // Six DDs over stations 0..2 and G01..G04 in two bands.
    const CycleLattice held = heldLattice({fixOf(0, 1, 2, 1, l1, 3), fixOf(0, 1, 3, 1, l1, 4),
                                           fixOf(2, 1, 2, 1, l1, 5), fixOf(0, 1, 2, 1, l2, 6),
                                           fixOf(0, 1, 4, 1, l2, 7), fixOf(2, 1, 4, 1, l2, 8)});
    std::map<ArcId, ArArcReading> arcs = continuing(held);
    arcs.erase(arcOf(2, 2, l1));                                                    // vanished
    arcs[arcOf(0, 3, l1)] = ArArcReading{ArcLifeEdge::FirstSeen, 0u, 0};           // back after a gap
    arcs[arcOf(1, 2, l2)] = ArArcReading{ArcLifeEdge::Freed, kArcLifeRelevelS,
                                         RelevelEpochState::kCauseIfTd | RelevelEpochState::kCauseGfJump};
    arcs[arcOf(2, 4, l2)] = ArArcReading{ArcLifeEdge::StampChange, kArcLifeDroppedEp | kArcLifeFlagEp, 0};
    arcs[arcOf(0, 1, l1)] = ArArcReading{ArcLifeEdge::Echo, kArcLifeAdmRunStart | kArcLifeHasCarry, 0};  // kept
    arcs[arcOf(0, 4, l2)] = ArArcReading{ArcLifeEdge::None, 0u, 0};                                      // kept

    const std::vector<ArLatticeReleaseEvent> rel = arLatticeEvidenceReleases(held, arcs, ArLatticeEvidence{});
    ASSERT_EQ(rel.size(), 4u);
    EXPECT_TRUE(std::is_sorted(rel.begin(), rel.end(),
                               [](const ArLatticeReleaseEvent& a, const ArLatticeReleaseEvent& b) { return a.arc < b.arc; }));
    std::map<ArcId, ArLatticeReleaseEvent> by;
    for (const ArLatticeReleaseEvent& r : rel) by[r.arc] = r;
    EXPECT_EQ(by.at(arcOf(2, 2, l1)).cause, ArLatticeRelease::Vanished);
    EXPECT_EQ(by.at(arcOf(2, 2, l1)).change, 0u);
    EXPECT_EQ(by.at(arcOf(0, 3, l1)).cause, ArLatticeRelease::FirstSeen);
    EXPECT_EQ(by.at(arcOf(1, 2, l2)).cause, ArLatticeRelease::Freed);
    EXPECT_EQ(by.at(arcOf(1, 2, l2)).change, static_cast<unsigned>(kArcLifeRelevelS));
    EXPECT_EQ(by.at(arcOf(1, 2, l2)).free_cause,
              static_cast<std::uint8_t>(RelevelEpochState::kCauseIfTd | RelevelEpochState::kCauseGfJump));
    EXPECT_EQ(by.at(arcOf(2, 4, l2)).cause, ArLatticeRelease::StampChange);
    EXPECT_EQ(by.at(arcOf(2, 4, l2)).change, static_cast<unsigned>(kArcLifeDroppedEp | kArcLifeFlagEp));
    EXPECT_EQ(by.at(arcOf(2, 4, l2)).free_cause, 0);
    EXPECT_EQ(by.count(arcOf(0, 1, l1)), 0u);   // an echo is not a new life
    EXPECT_EQ(by.count(arcOf(0, 4, l2)), 0u);

    // The step eliminates them in that order; each held one row at +-1, so each costs one.
    CycleLattice L = held;
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = arcs;
    const ArLatticeEpoch e = stepOf(L, fixes);
    EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::Vanished)], 1);
    EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::FirstSeen)], 1);
    EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::Freed)], 1);
    EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::StampChange)], 1);
    EXPECT_EQ(e.dims_lost, e.released_touching);
    EXPECT_EQ(e.no_unit, 0);
    EXPECT_EQ(e.rows, 6 - e.dims_lost);
}

TEST(NetworkArLattice, TenHeldDdsOnAReleasedReferenceLoseOneDimension) {
    const SignalId l1 = sigOf("L1C");
    std::vector<ArFixedCycle> held_fixes;
    for (int j = 2; j <= 11; ++j) held_fixes.push_back(fixOf(0, 1, j, 1, l1, 10 * j));
    const CycleLattice held = heldLattice(held_fixes);
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(held);
    fixes.arcs[arcOf(0, 1, l1)] = ArArcReading{ArcLifeEdge::Freed, 0u, RelevelEpochState::kCauseMw};
    fixes.fixed    = {fixOf(0, 1, 5, 9, l1, 50 - 90)};   // between two held satellites: implied
    CycleLattice L = held;
    const ArLatticeEpoch e = stepOf(L, fixes);
    ASSERT_EQ(e.releases.size(), 1u);
    // The reference is the lowest arc of the first DD, so the reduced echelon form made it that
    // row's pivot and removed it from the other nine: one reduced row holds it.
    EXPECT_EQ(e.releases[0].rows_touched, 1);
    EXPECT_EQ(e.releases[0].rows_dropped, 1);
    EXPECT_EQ(e.dims_lost, 1);
    EXPECT_EQ(e.released_touching, 1);
    EXPECT_EQ(e.rows, 9);
    EXPECT_EQ(verdictCount(e, LatticeVerdict::Confirmed), 1);
    EXPECT_EQ(e.max_len, 4);   // the nine re-referenced rows are still DDs
}

TEST(NetworkArLattice, AVanishedSatelliteCostsOneDimensionPerArcThatStillHoldsRows) {
    const SignalId l1 = sigOf("L1C");
    // Stations 1..3 against station 0, satellites G02 and G03 against G01: six rows, the whole
    // cycle space of 4 stations x 3 satellites (12 - 7 + 1).
    std::vector<ArFixedCycle> held_fixes;
    for (int s = 1; s <= 3; ++s)
        for (int j = 2; j <= 3; ++j) held_fixes.push_back(fixOf(s, 0, j, 1, l1, s * 10 + j));
    const CycleLattice held = heldLattice(held_fixes);
    ASSERT_EQ(held.rows.size(), 6u);
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(held);
    for (int s = 0; s <= 3; ++s) fixes.arcs.erase(arcOf(s, 1, l1));   // G01 sets everywhere
    CycleLattice L = held;
    const ArLatticeEpoch e = stepOf(L, fixes);
    EXPECT_EQ(e.releases.size(), 4u);
    EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::Vanished)], 4);
    EXPECT_EQ(e.released_touching, 3);   // the last arc of G01 holds no row: a cycle cannot end there
    EXPECT_EQ(e.releases.back().rows_touched, 0);
    EXPECT_EQ(e.dims_lost, 3);           // 4 x 3 graph to 4 x 2: 6 to 3
    EXPECT_EQ(e.no_unit, 0);
    EXPECT_EQ(e.rows, 3);
    // What is left implies the G02 - G03 DDs with the integers it was fixed with.
    EXPECT_EQ(arcgraph::latticeImplied(L, dd(2, 0, 2, 3, l1)), (20 + 2) - (20 + 3));
}

TEST(NetworkArLattice, AContradictionReleasesTheArcsOfItsCycleAndReportsTheImpliedInteger) {
    const SignalId l1 = sigOf("L1C");
    const CycleLattice held = heldLattice({fixOf(0, 1, 2, 3, l1, 7)});
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(held);
    fixes.fixed    = {fixOf(0, 1, 2, 3, l1, 8), fixOf(0, 1, 4, 3, l1, 1)};
    CycleLattice L = held;
    const ArLatticeEpoch e = stepOf(L, fixes);
    ASSERT_EQ(e.offers.size(), 2u);
    EXPECT_EQ(e.offers[0].verdict, LatticeVerdict::Contradicted);
    EXPECT_EQ(e.offers[0].implied, 7);
    // The contradiction released the arcs of the cycle, and with them its row (AGT-6
    // kept it); the contradicting fix waits for its next epoch; the other fix enters.
    ASSERT_EQ(e.releases.size(), 1u);
    EXPECT_EQ(e.releases[0].cause, ArLatticeRelease::Contradiction);
    EXPECT_EQ(e.releases[0].scope, ArLatticeScope::Cycle);
    EXPECT_EQ(e.releases[0].rows_touched, 1);
    EXPECT_EQ(e.releases[0].offered, 8);
    EXPECT_EQ(e.releases[0].implied, 7);
    EXPECT_EQ(e.releases[0].rows_dropped, 1);
    EXPECT_EQ(e.dims_lost, 1);
    EXPECT_EQ(e.offers[1].verdict, LatticeVerdict::Added);
    EXPECT_EQ(e.rows, 1);
    EXPECT_FALSE(arcgraph::latticeImplied(L, dd(0, 1, 2, 3, l1)).has_value());
}

TEST(NetworkArLattice, TheTalliesCountFieldsAndCausesOnReleasesThatHeldRows) {
    ArLatticeEpoch e;
    ArLatticeReleaseEvent held, empty;
    held.cause        = ArLatticeRelease::StampChange;
    held.change       = kArcLifeBirthEp | kArcLifeHasCarry;
    held.free_cause   = RelevelEpochState::kCauseLli;
    held.rows_touched = 2;
    held.rows_dropped = 1;
    empty.cause       = ArLatticeRelease::Freed;
    empty.change      = kArcLifeRelevelS;
    empty.free_cause  = RelevelEpochState::kCauseWindup;
    e.releases = {held, empty};
    e.released_touching = 1;
    e.dims_lost = 1;
    e.verdicts[static_cast<std::size_t>(LatticeVerdict::Contradicted)] = 2;
    e.verdicts[static_cast<std::size_t>(LatticeVerdict::Confirmed)]    = 5;
    ArLatticeCounters c;
    arLatticeTally(c, e, 261930.0, 0.5);
    EXPECT_EQ(c.epochs, 1);
    EXPECT_EQ(c.fixes, 7);
    EXPECT_EQ(c.contradicted_epochs, 1);
    EXPECT_DOUBLE_EQ(c.first_contradicted_tow, 261930.0);
    EXPECT_EQ(c.released_by[static_cast<std::size_t>(ArLatticeRelease::StampChange)], 1);
    EXPECT_EQ(c.released_by[static_cast<std::size_t>(ArLatticeRelease::Freed)], 1);
    EXPECT_EQ(c.touching_by[static_cast<std::size_t>(ArLatticeRelease::StampChange)], 1);
    EXPECT_EQ(c.touching_by[static_cast<std::size_t>(ArLatticeRelease::Freed)], 0);
    EXPECT_EQ(c.change_by[3], 1);   // birth_ep
    EXPECT_EQ(c.change_by[2], 1);   // has_carry
    EXPECT_EQ(c.change_by[5], 0);   // relevel_s: only on the release that held no row
    EXPECT_EQ(c.free_by[1], 1);     // LLI
    EXPECT_EQ(c.free_by[5], 0);     // wind-up: only on the release that held no row
    EXPECT_EQ(c.dims_by[static_cast<std::size_t>(ArLatticeRelease::StampChange)], 1);
    EXPECT_EQ(c.dims_lost, 1);
    arLatticeTally(c, e, 261960.0, 0.5);
    EXPECT_EQ(c.contradicted_epochs, 2);
    EXPECT_DOUBLE_EQ(c.first_contradicted_tow, 261930.0);
}

TEST(NetworkArLattice, NamesTheCausesAndVerdictsAsTheTraceWritesThem) {
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Vanished), "vanished");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::FirstSeen), "first_seen");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Freed), "freed");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::StampChange), "stamp");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::ReplayCulprit), "replay");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::GrossLos), "gross");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Contradiction), "contradiction");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Revalidation), "revalidation");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Acceptance), "acceptance");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::Degenerate), "degenerate");
    EXPECT_STREQ(arLatticeReleaseName(ArLatticeRelease::SeamGap), "seam_gap");
    EXPECT_STREQ(arLatticeVerdictName(LatticeVerdict::Added), "added");
    EXPECT_STREQ(arLatticeVerdictName(LatticeVerdict::Confirmed), "confirmed");
    EXPECT_STREQ(arLatticeVerdictName(LatticeVerdict::Contradicted), "contradicted");
    EXPECT_STREQ(arLatticeVerdictName(LatticeVerdict::NotPrimitive), "not_primitive");
}

// ============================== the loader ==============================

namespace fs = std::filesystem;

std::string arConfig(const std::string& ar_body) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"ambiguity_resolution\": { " + ar_body + " },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// Load @p ar_body as network.ambiguity_resolution and return the parsed block.
NetworkArConfig load(const std::string& tag, const std::string& ar_body) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / ("ar_lattice_cfg_" + tag + ".json");
    { std::ofstream f(p); f << arConfig(ar_body); }
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec;
    fs::remove(p, ec);
    return cfg.getNetworkConfig().ambiguity_resolution;
}

const std::string kCycleBasis = "\"enable\": true, \"tree_basis\": true, \"decorrelated_partial\": true, "
                                "\"block_search\": true, \"cycle_basis\": true";

TEST(NetworkArLatticeConfig, DefaultsOff) {
    EXPECT_FALSE(load("default", kCycleBasis).trace_lattice);
}

TEST(NetworkArLatticeConfig, ArmsWithTheCycleBasis) {
    const NetworkArConfig ar = load("armed", kCycleBasis + ", \"trace_lattice\": true");
    EXPECT_TRUE(ar.cycle_basis);
    EXPECT_TRUE(ar.trace_lattice);
}

TEST(NetworkArLatticeConfig, IsForcedOffWithoutTheCycleBasis) {
    // No cycle basis asked for.
    EXPECT_FALSE(load("no_cb", "\"enable\": true, \"tree_basis\": true, \"decorrelated_partial\": true, "
                               "\"block_search\": true, \"trace_lattice\": true").trace_lattice);
    // The cycle basis asked for but forced off (block_search missing): the observatory goes with it.
    const NetworkArConfig ar = load("cb_forced_off", "\"enable\": true, \"tree_basis\": true, "
                                                     "\"decorrelated_partial\": true, \"cycle_basis\": true, "
                                                     "\"trace_lattice\": true");
    EXPECT_FALSE(ar.cycle_basis);
    EXPECT_FALSE(ar.trace_lattice);
}

TEST(NetworkArLatticeConfig, TheTwoFilesHaveTheirDocumentedColumns) {
    auto columns = [](const std::string& h) {
        return static_cast<int>(std::count(h.begin(), h.end(), ',')) + 1;
    };
    const std::string epoch = kArLatticeHeader, events = kArLatticeEventsHeader;
    EXPECT_EQ(columns(epoch), 29);   // AGT-6's 19, then the fixed-cycle fold's 10 (step FCF-3)
    EXPECT_EQ(epoch.rfind("week,tow,rows,arcs,fixes,added,confirmed,contradicted,not_primitive,", 0), 0u);
    EXPECT_NE(epoch.find(",max_len,mean_len,waiting,rel_replay,"), std::string::npos);
    EXPECT_EQ(columns(events), 17);
    EXPECT_EQ(events.rfind("week,tow,event,cons,signal,sat_j,ref_sat,sta_r,ref_sta,", 0), 0u);
    EXPECT_EQ(epoch.back(), '\n');
    EXPECT_EQ(events.back(), '\n');
}

}  // namespace
}  // namespace GnssProcessor
