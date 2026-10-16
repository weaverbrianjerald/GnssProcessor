/**
 * @file test_network_fixed_cycle_release.cpp
 * @brief Unit gates of the fixed-cycle hold's one release seam (step FCF-3;
 *        network/ar/ArLattice.hpp and network/ar/FixedCycles.hpp).
 *
 * Suite @c NetworkFixedCycleRelease (the gates of the fixed-cycle release):
 *  - each evidence cause releases exactly its arcs, with its detail bits (vanished, first seen,
 *    freed with the free-epoch causes, a stamp change with its fields, the landed replay's
 *    culprits, the gross-LOS Decide), in ascending ArcId order, one release per arc;
 *  - the evidence is typed from the landed replay and from the gross-LOS Decide, and the
 *    gross-LOS lists are read only when the Decide ran before the seam;
 *  - a pinned anchor (no carry) is released through @c amb_flag_ep alone;
 *  - a wind-up bit releases only the freed band;
 *  - a seam gap releases everything, streaks included;
 *  - localization: two failing units sharing an arc eliminate that arc; an isolated failing
 *    unit releases the arcs of its own cycles; a passing unit keeps its rows;
 *  - a failure released by its arcs leaves no row that keeps an arc's old integer, so later
 *    unrelated fixes cannot make the ledger imply the released cycle again
 *    (the review's finding 1; the test fails on the rows-behind-the-cycle release);
 *  - a new life on an arc of a waiting fix restarts its streak; a fix is offered after
 *    @c persist_ep epochs; a missed epoch or another integer restarts it;
 *  - a contradiction releases the arcs of its cycle, and a row through none of them stays;
 *  - a contradiction among one epoch's own fixes, at admission, marks the unit whose rows it
 *    took released (the review's finding 6);
 *  - the lapse: a unit the search has not determined for @c lapse_ep epochs lapses, an epoch
 *    that determines it resets the count, an epoch it is not implied ends it, 0 is off;
 *  - an added row folds from the next epoch;
 *  - the fold's refusals (acceptance, degenerate) release their arcs, a block left float keeps
 *    its rows;
 *  - a planted wrong integer on a converging synthetic float trips at the epoch where the
 *    analytic statistic crosses the bar, and the null trips at most 2 times in 10 000 epochs;
 *  - the cause table (the registry row order test's counterpart) is in enum
 *    order, with unique names and the trace's columns.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "config/GnssConfig.hpp"
#include "network/ar/ArLattice.hpp"
#include "network/ar/ArcLife.hpp"
#include "network/ar/FixedCycles.hpp"
#include "network/driver/RunTypes.hpp"     // RelevelEpochState::kCause*, WithinEpochCulprits
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"   // kArLatticeHeader

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using arcgraph::Cycle;
using arcgraph::CycleLattice;
using arcgraph::CycleTerm;
using arcgraph::LatticeVerdict;
using fcyc::Unit;

// ============================== helpers ==============================

SignalId sigOf(const char* code) { return *SignalId::fromObsCode(System::GPS, code); }
SignalId L1() { return sigOf("L1C"); }
SignalId L2() { return sigOf("L2W"); }
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

ArDdIdentity idOf(int r, int p, int j, int k, const SignalId& s) {
    return ArDdIdentity{s, sta(r), sta(p), gps(j), gps(k)};
}

/// A fixed DD as the search hands it over.
ArFixedCycle fixOf(int r, int p, int j, int k, const SignalId& s, long long value) {
    return ArFixedCycle{dd(r, p, j, k, s), value, idOf(r, p, j, k, s)};
}

/// A ledger holding @p fixes, each offered at epoch 1.
ArLatticeState ledgerOf(const std::vector<ArFixedCycle>& fixes) {
    ArLatticeState S;
    for (const ArFixedCycle& f : fixes)
        EXPECT_EQ(arcgraph::latticeOffer(S.lattice, f.cycle, f.value, 1), LatticeVerdict::Added);
    return S;
}

/// A reading of every arc the lattice holds as an unbroken life.
std::map<ArcId, ArArcReading> continuing(const CycleLattice& L) {
    std::map<ArcId, ArArcReading> out;
    for (const Cycle& row : L.rows)
        for (const CycleTerm& t : row.terms) out[t.arc] = ArArcReading{};
    return out;
}

/// Releases by cause in an epoch.
int releasedBy(const ArLatticeEpoch& e, ArLatticeRelease r) { return e.released_by[static_cast<std::size_t>(r)]; }

/// One L1/L2 pair (r, p; j, k) of searched floats, appended to @p fx; returns its unit.
Unit addPair(ArEpochFixes& fx, int r, int p, int j, int k, long long n1, long long n2) {
    const int a = static_cast<int>(fx.floats.size());
    for (const SignalId& s : {L1(), L2()}) {
        SearchedCycle c;
        c.id       = idOf(r, p, j, k, s);
        c.cycle    = dd(r, p, j, k, s);
        c.y_cyc    = static_cast<double>(s == L1() ? n1 : n2);
        c.var_cyc2 = 0.01;
        fx.floats.push_back(c);
    }
    fx.floats[a].twin = a + 1;
    fx.floats[a + 1].twin = a;
    Unit u;
    u.l1 = a;
    u.l2 = a + 1;
    u.n1 = n1;
    u.n2 = n2;
    return u;
}

/// A loaded network configuration (for a run context).
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    std::filesystem::path path;
    Loaded(const std::string& tag, const std::string& network_extra) {
        path = std::filesystem::temp_directory_path() / ("fcyc_release_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << "{\n  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
             "                  \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
             "  \"network\": { \"clock_datum_station\": \"AAAA\",\n"
          << network_extra <<
             "    \"stations\": [\n"
             "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
             "    ] }\n}\n";
        f.close();
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; std::filesystem::remove(path, ec); }
};

// ============================== the evidence ==============================

TEST(NetworkFixedCycleRelease, EachEvidenceCauseReleasesExactlyItsArcsWithItsBits) {
    const SignalId s = L1();
    // Stations 0..3 against station 4, satellites G02..G04 against G01: twelve rows.
    std::vector<ArFixedCycle> held;
    for (int r = 0; r <= 3; ++r)
        for (int j = 2; j <= 4; ++j) held.push_back(fixOf(r, 4, j, 1, s, 10 * r + j));
    ArLatticeState S = ledgerOf(held);
    std::map<ArcId, ArArcReading> arcs = continuing(S.lattice);
    arcs.erase(arcOf(0, 2, s));                                                           // vanished
    arcs[arcOf(0, 3, s)] = ArArcReading{ArcLifeEdge::FirstSeen, 0u, 0};
    arcs[arcOf(1, 2, s)] = ArArcReading{ArcLifeEdge::Freed, kArcLifeRelevelS,
                                        RelevelEpochState::kCauseMw | RelevelEpochState::kCauseGfJump};
    arcs[arcOf(1, 3, s)] = ArArcReading{ArcLifeEdge::StampChange, kArcLifeDroppedEp | kArcLifeRelevelS, 0};
    arcs[arcOf(1, 4, s)] = ArArcReading{ArcLifeEdge::Echo, kArcLifeAdmRunStart, 0};       // kept
    ArLatticeEvidence ev;
    ev.replay.los.insert(LosId{sta(2), gps(3)});
    ev.replay.los.insert(LosId{sta(0), gps(3)});       // already first seen: one release per arc
    ev.gross.stations.insert(sta(3));
    const std::vector<ArLatticeReleaseEvent> rel = arLatticeEvidenceReleases(S.lattice, arcs, ev);
    std::map<ArcId, ArLatticeReleaseEvent> by;
    for (const ArLatticeReleaseEvent& r : rel) by[r.arc] = r;
    ASSERT_EQ(by.size(), rel.size());
    EXPECT_TRUE(std::is_sorted(rel.begin(), rel.end(),
                               [](const ArLatticeReleaseEvent& a, const ArLatticeReleaseEvent& b) { return a.arc < b.arc; }));
    std::set<ArcId> want = {arcOf(0, 2, s), arcOf(0, 3, s), arcOf(1, 2, s), arcOf(1, 3, s), arcOf(2, 3, s)};
    for (int j = 1; j <= 4; ++j) want.insert(arcOf(3, j, s));   // the declared station's arcs
    std::set<ArcId> got;
    for (const auto& [a, r] : by) got.insert(a);
    EXPECT_EQ(got, want);
    EXPECT_EQ(by.at(arcOf(0, 2, s)).cause, ArLatticeRelease::Vanished);
    EXPECT_EQ(by.at(arcOf(0, 3, s)).cause, ArLatticeRelease::FirstSeen);
    EXPECT_EQ(by.at(arcOf(1, 2, s)).cause, ArLatticeRelease::Freed);
    EXPECT_EQ(by.at(arcOf(1, 2, s)).free_cause,
              static_cast<std::uint8_t>(RelevelEpochState::kCauseMw | RelevelEpochState::kCauseGfJump));
    EXPECT_EQ(by.at(arcOf(1, 3, s)).cause, ArLatticeRelease::StampChange);
    EXPECT_EQ(by.at(arcOf(1, 3, s)).change, static_cast<unsigned>(kArcLifeDroppedEp | kArcLifeRelevelS));
    EXPECT_EQ(by.at(arcOf(2, 3, s)).cause, ArLatticeRelease::ReplayCulprit);
    for (int j = 1; j <= 4; ++j) EXPECT_EQ(by.at(arcOf(3, j, s)).cause, ArLatticeRelease::GrossLos);

    // Through the ledger: the evidence and the arcs it names break the streaks through them.
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = arcs;
    S.streaks[idOf(2, 4, 3, 1, s)] = ArFixStreak{23, 4, 0};
    S.streaks[idOf(2, 4, 4, 1, s)] = ArFixStreak{24, 4, 0};
    const ArLatticeEpoch e = arLatticeBegin(S, fixes, ev, 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::Vanished), 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::FirstSeen), 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::Freed), 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::StampChange), 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::ReplayCulprit), 1);
    EXPECT_EQ(releasedBy(e, ArLatticeRelease::GrossLos), 4);
    EXPECT_EQ(S.streaks.count(idOf(2, 4, 3, 1, s)), 0u);   // through the replay's line of sight
    EXPECT_EQ(S.streaks.count(idOf(2, 4, 4, 1, s)), 1u);
    EXPECT_TRUE(e.broken.count(arcOf(2, 3, s)));
    EXPECT_FALSE(e.broken.count(arcOf(1, 4, s)));          // an echo is not a new life
}

TEST(NetworkFixedCycleRelease, TheEvidenceIsTypedFromTheReplayAndTheGrossLosDecide) {
    WithinEpochCulprits we;
    we.sats     = {"G05", "not_a_sat"};
    we.arcs     = {{1, {"G07"}}};
    we.stations = {2};
    auto declare = [](EpochEvents& c) {
        c.gross.gl_act_declare      = {{0, "G09"}};
        c.gross.gl_act_station      = {{3, true}, {1, false}};
        c.gross.gl_act_cold_refloat = {{1, "G11"}};
    };
    // Without the split screen the lists are the last epoch's: not read.
    Loaded unsplit("evidence_unsplit", "    \"gross_los_screen\": true,\n");
    NetworkRunContext ctx_u(unsplit.cfg, unsplit.cfg.getNetworkConfig());
    ASSERT_FALSE(ctx_u.knobs.gl_emit_exclude);
    EpochEvents events_u;
    declare(events_u);
    ArLatticeEvidence ev = networkArLatticeEvidence(ctx_u, we, events_u);
    EXPECT_EQ(ev.replay.sats, (std::set<SatId>{gps(5)}));
    EXPECT_EQ(ev.replay.los, (std::set<LosId>{LosId{sta(1), gps(7)}}));
    EXPECT_EQ(ev.replay.stations, (std::set<StationId>{sta(2)}));
    EXPECT_TRUE(ev.gross.empty());
    // The Decide half ran before the seam (gross_los_emit_exclude): this epoch's declarations.
    Loaded split("evidence_split",
                       "    \"gross_los_screen\": true, \"gross_los_emit_exclude\": true,\n");
    NetworkRunContext ctx(split.cfg, split.cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.gl_emit_exclude);
    EpochEvents events;
    declare(events);
    ev = networkArLatticeEvidence(ctx, we, events);
    EXPECT_EQ(ev.gross.los, (std::set<LosId>{LosId{sta(0), gps(9)}, LosId{sta(1), gps(11)}}));
    EXPECT_EQ(ev.gross.stations, (std::set<StationId>{sta(3)}));   // a station release names nothing
    EXPECT_TRUE(ev.replay.covers(arcOf(4, 5, L2())));                // a satellite at every station, every band
    EXPECT_TRUE(ev.replay.covers(arcOf(2, 30, L1())));
    EXPECT_FALSE(ev.replay.covers(arcOf(1, 8, L1())));
}

TEST(NetworkFixedCycleRelease, APinnedAnchorIsReleasedThroughTheFlagAlone) {
    // A pinned anchor arc has no carry: the ledger fields are its life, and only amb_flag_ep can
    // end it (a free-set, T3 / T4 or gross-LOS flag under the held basis).
    ArcLifeStamp before;
    before.adm_generation = 1;
    before.adm_run_start  = 5;
    before.has_carry      = false;
    ArcLifeStamp now = before;
    now.flag_ep = 40;
    const ArcLifeTrack track{before, 5};
    const ArcLifeStep step = arcLifeStep(&track, 40, now, /*freed=*/false, 41);
    ASSERT_EQ(step.edge, ArcLifeEdge::StampChange);
    EXPECT_EQ(step.change, static_cast<unsigned>(kArcLifeFlagEp));
    ArLatticeState S = ledgerOf({fixOf(0, 1, 2, 3, L1(), 4)});
    std::map<ArcId, ArArcReading> arcs = continuing(S.lattice);
    arcs[arcOf(0, 2, L1())] = ArArcReading{step.edge, step.change, 0};
    const std::vector<ArLatticeReleaseEvent> rel = arLatticeEvidenceReleases(S.lattice, arcs, ArLatticeEvidence{});
    ASSERT_EQ(rel.size(), 1u);
    EXPECT_EQ(rel[0].arc, arcOf(0, 2, L1()));
    EXPECT_EQ(rel[0].cause, ArLatticeRelease::StampChange);
    EXPECT_EQ(rel[0].change, static_cast<unsigned>(kArcLifeFlagEp));
}

TEST(NetworkFixedCycleRelease, AWindUpBitReleasesOnlyTheFreedBand) {
    ArLatticeState S = ledgerOf({fixOf(0, 1, 2, 3, L1(), 4), fixOf(0, 1, 2, 3, L2(), -6)});
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(S.lattice);
    fixes.arcs[arcOf(0, 2, L1())] = ArArcReading{ArcLifeEdge::Freed, 0u, RelevelEpochState::kCauseWindup};
    const ArLatticeEpoch e = arLatticeBegin(S, fixes, ArLatticeEvidence{}, 2);
    ASSERT_EQ(e.releases.size(), 1u);
    EXPECT_EQ(e.releases[0].arc, arcOf(0, 2, L1()));
    EXPECT_EQ(e.releases[0].free_cause, static_cast<std::uint8_t>(RelevelEpochState::kCauseWindup));
    EXPECT_EQ(e.dims_lost, 1);
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, dd(0, 1, 2, 3, L1())).has_value());
    EXPECT_EQ(arcgraph::latticeImplied(S.lattice, dd(0, 1, 2, 3, L2())), -6);   // the other band holds
}

TEST(NetworkFixedCycleRelease, ASeamGapReleasesEverything) {
    ArLatticeState S = ledgerOf({fixOf(0, 1, 2, 3, L1(), 4), fixOf(0, 1, 4, 3, L1(), 1),
                                 fixOf(0, 1, 2, 3, L2(), -6)});
    S.streaks[idOf(2, 1, 2, 3, L1())] = ArFixStreak{5, 3, 9};
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(S.lattice);
    // The step right after the last one: nothing released.
    S.last_step_ep = 10;
    ArLatticeState next = S;
    EXPECT_TRUE(arLatticeBegin(next, fixes, ArLatticeEvidence{}, 11).releases.empty());
    // One loop epoch went without a step.
    const ArLatticeEpoch e = arLatticeBegin(S, fixes, ArLatticeEvidence{}, 12);
    ASSERT_EQ(e.releases.size(), 1u);
    EXPECT_EQ(e.releases[0].cause, ArLatticeRelease::SeamGap);
    EXPECT_EQ(e.releases[0].scope, ArLatticeScope::All);
    EXPECT_EQ(e.releases[0].rows_dropped, 3);
    EXPECT_EQ(e.dims_lost, 3);
    EXPECT_TRUE(S.lattice.rows.empty());
    EXPECT_TRUE(S.streaks.empty());
}

// ============================== the re-validation ==============================

TEST(NetworkFixedCycleRelease, TwoFailingUnitsSharingAnArcEliminateItAndALoneFailureReleasesItsArcs) {
    ArEpochFixes fx;
    std::vector<Unit> units = {addPair(fx, 0, 1, 2, 1, 5, 7),     // fails, shares (0, G02) with the next
                               addPair(fx, 0, 2, 2, 3, 6, 8),     // fails
                               addPair(fx, 4, 5, 6, 7, 9, 11),    // fails alone
                               addPair(fx, 8, 9, 6, 7, 3, 4)};    // passes
    units[0].fail = units[1].fail = units[2].fail = true;
    const std::vector<ArLatticeReleaseEvent> rel = fcyc::localizeFailures(units, fx);
    std::vector<ArLatticeReleaseEvent> arc_rel, cyc_rel;
    for (const ArLatticeReleaseEvent& r : rel) {
        EXPECT_EQ(r.cause, ArLatticeRelease::Revalidation);
        (r.scope == ArLatticeScope::Arc ? arc_rel : cyc_rel).push_back(r);
    }
    ASSERT_EQ(arc_rel.size(), 2u);   // (0, G02) in both bands
    EXPECT_EQ(arc_rel[0].arc, arcOf(0, 2, L1()));
    EXPECT_EQ(arc_rel[1].arc, arcOf(0, 2, L2()));
    ASSERT_EQ(cyc_rel.size(), 2u);   // the lone failure's two cycles
    EXPECT_EQ(cyc_rel[0].scope, ArLatticeScope::Cycle);
    EXPECT_EQ(cyc_rel[0].dd, idOf(4, 5, 6, 7, L1()));
    EXPECT_EQ(cyc_rel[1].dd, idOf(4, 5, 6, 7, L2()));

    // On a ledger of the four units: the shared arc costs one row per band, the lone failure's
    // arcs its two rows, and the passing unit keeps its rows.
    ArLatticeState S;
    for (const SearchedCycle& c : fx.floats)
        ASSERT_EQ(arcgraph::latticeOffer(S.lattice, c.cycle, static_cast<long long>(c.y_cyc), 1), LatticeVerdict::Added);
    ArLatticeEpoch e;
    arLatticeRelease(S, rel, e);
    EXPECT_EQ(e.dims_lost, 4);
    EXPECT_EQ(S.lattice.rows.size(), 4u);
    fcyc::markReleased(units, S.lattice, fx);
    EXPECT_FALSE(units[3].released);
    EXPECT_EQ(arcgraph::latticeImplied(S.lattice, fx.floats[6].cycle), 3);
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, fx.floats[4].cycle).has_value());
    // The two units through the eliminated arc no longer imply anything; their other rows stay.
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, fx.floats[0].cycle).has_value());
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, fx.floats[2].cycle).has_value());
    EXPECT_TRUE(e.broken.count(arcOf(0, 2, L1())));
    EXPECT_TRUE(e.broken.count(arcOf(4, 6, L2())));
}

TEST(NetworkFixedCycleRelease, ALoneFailureReleasesItsArcsSoTheOldIntegerCannotComeBack) {
    // Card FCFC section 8 (the review of FCF-3, finding 1).  The failing unit (1, 2; G03, G04)
    // holds the arc (2, G04), which a second row, (0, 2; G05, G04), also holds as a non-pivot
    // term.  Dropping only the rows behind the failing cycles kept that row with the arc's old
    // integer, and two later fixes on DDs that miss (2, G04) completed the span again, so the
    // ledger implied the failing cycle at its old integer.  Released by its arcs, no row through
    // any of them is left; a row through none of them stays.
    const SignalId s = L1();
    ArEpochFixes fx;
    std::vector<Unit> units = {addPair(fx, 1, 2, 3, 4, 7, 9)};
    units[0].fail = true;
    ArLatticeState S;
    ASSERT_EQ(arcgraph::latticeOffer(S.lattice, dd(1, 2, 3, 4, s), 7, 1), LatticeVerdict::Added);
    ASSERT_EQ(arcgraph::latticeOffer(S.lattice, dd(0, 2, 5, 4, s), 0, 1), LatticeVerdict::Added);
    ASSERT_EQ(arcgraph::latticeOffer(S.lattice, dd(6, 7, 8, 9, s), 3, 1), LatticeVerdict::Added);
    auto rowsThrough = [&](const ArcId& a) {
        int n = 0;
        for (const Cycle& row : S.lattice.rows)
            for (const CycleTerm& t : row.terms) n += (t.arc == a) ? 1 : 0;
        return n;
    };
    ASSERT_EQ(rowsThrough(arcOf(2, 4, s)), 2);
    ArLatticeEpoch e;
    arLatticeRelease(S, fcyc::localizeFailures(units, fx), e);
    for (const SearchedCycle& c : fx.floats)
        for (const CycleTerm& t : c.cycle.terms)
            EXPECT_EQ(rowsThrough(t.arc), 0) << t.arc.station.index << " " << t.arc.sat.str();
    EXPECT_EQ(e.dims_lost, 2);
    EXPECT_EQ(arcgraph::latticeImplied(S.lattice, dd(6, 7, 8, 9, s)), 3);
    // (1, 2; 3, 4) = (0, 2; 5, 4) + (1, 0; 3, 4) + (0, 2; 3, 5): the two fixes miss (2, G04).
    EXPECT_EQ(arcgraph::latticeOffer(S.lattice, dd(1, 0, 3, 4, s), 2, 2), LatticeVerdict::Added);
    EXPECT_EQ(arcgraph::latticeOffer(S.lattice, dd(0, 2, 3, 5, s), 5, 2), LatticeVerdict::Added);
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, dd(1, 2, 3, 4, s)).has_value());
}

TEST(NetworkFixedCycleRelease, ANewLifeRestartsTheStreakAndAFixIsOfferedAfterPersistEp) {
    const ArFixedCycle f = fixOf(0, 1, 2, 3, L1(), 4);
    ArLatticeState S;
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.fixed    = {f};
    auto epoch = [&](long long ep, bool break_arc) {
        fixes.arcs.clear();
        if (break_arc) fixes.arcs[arcOf(1, 3, L1())] = ArArcReading{ArcLifeEdge::StampChange, kArcLifeBirthEp, 0};
        ArLatticeEpoch e = arLatticeBegin(S, fixes, ArLatticeEvidence{}, ep);
        arLatticeAdmit(S, fixes, 3, ep, e);
        arLatticeClose(S, ep, e);
        return e;
    };
    EXPECT_TRUE(epoch(1, false).offers[0].waiting);
    EXPECT_EQ(epoch(2, false).offers[0].streak, 2);
    const ArLatticeEpoch e3 = epoch(3, true);           // a new life on one of its arcs
    EXPECT_TRUE(e3.offers[0].waiting);
    EXPECT_EQ(e3.offers[0].streak, 1);
    EXPECT_EQ(epoch(4, false).offers[0].streak, 2);
    const ArLatticeEpoch e5 = epoch(5, false);
    EXPECT_FALSE(e5.offers[0].waiting);
    EXPECT_EQ(e5.offers[0].verdict, LatticeVerdict::Added);
    EXPECT_EQ(e5.waiting, 0);
    EXPECT_EQ(S.lattice.entered_ep.back(), 5);
    // A missed epoch drops the streak; another integer restarts it.
    fixes.fixed = {fixOf(2, 1, 2, 3, L1(), 7)};
    epoch(6, false);
    epoch(7, false);
    fixes.fixed.clear();
    epoch(8, false);
    fixes.fixed = {fixOf(2, 1, 2, 3, L1(), 7)};
    EXPECT_EQ(epoch(9, false).offers[0].streak, 1);
    fixes.fixed = {fixOf(2, 1, 2, 3, L1(), 8)};
    EXPECT_EQ(epoch(10, false).offers[0].streak, 1);
}

TEST(NetworkFixedCycleRelease, AContradictionReleasesTheArcsOfItsCycle) {
    // The two rows (0, 1; G02, G04) and (0, 1; G03, G04) both hold (0, G04) and (1, G04), arcs of
    // the contradicted cycle (0, 1; G02, G04), though only the first is behind it: both go.  The
    // row (2, 3; G05, G06) holds none of its arcs and stays.
    ArLatticeState S = ledgerOf({fixOf(0, 1, 2, 3, L1(), 7), fixOf(0, 1, 4, 3, L1(), -2),
                                 fixOf(2, 3, 5, 6, L1(), 4)});
    ArEpochFixes fixes;
    fixes.searched = true;
    fixes.arcs     = continuing(S.lattice);
    fixes.fixed    = {fixOf(0, 1, 2, 4, L1(), 10)};   // the lattice implies 7 - (-2) = 9
    const ArLatticeEpoch e = arLatticeBegin(S, fixes, ArLatticeEvidence{}, 2);
    EXPECT_EQ(e.offers[0].verdict, LatticeVerdict::Contradicted);
    EXPECT_EQ(e.offers[0].implied, 9);
    ASSERT_EQ(e.releases.size(), 1u);
    EXPECT_EQ(e.releases[0].cause, ArLatticeRelease::Contradiction);
    EXPECT_EQ(e.releases[0].scope, ArLatticeScope::Cycle);
    EXPECT_EQ(e.releases[0].dd, idOf(0, 1, 2, 4, L1()));
    EXPECT_EQ(e.releases[0].offered, 10);
    EXPECT_EQ(e.releases[0].implied, 9);
    EXPECT_EQ(e.releases[0].rows_touched, 2);
    EXPECT_EQ(e.releases[0].rows_dropped, 2);
    EXPECT_EQ(S.lattice.rows.size(), 1u);
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, dd(0, 1, 4, 3, L1())).has_value());
    EXPECT_EQ(arcgraph::latticeImplied(S.lattice, dd(2, 3, 5, 6, L1())), 4);
    EXPECT_TRUE(e.broken.count(arcOf(1, 4, L1())));
}

TEST(NetworkFixedCycleRelease, AnAdmissionContradictionMarksTheUnitsItTookReleased) {
    // The review's finding 6.  The ledger holds the pair (0, 1; G02, G03) from rows entered at
    // epoch 1, so at epoch 5 it is a unit and passes its test.  Two fixes of epoch 5, both
    // offered at once (persist_ep 1): (0, 1; G03, G04) is added, and (0, 1; G02, G04) then
    // disagrees with the ledger through that new row (it implies 4 + 1 = 5, the fix says 10).
    // The contradiction releases the arcs of (0, 1; G02, G04), which take the unit's L1 row, so
    // the unit implied before admission is marked released and is not folded.
    ArLatticeState S = ledgerOf({fixOf(0, 1, 2, 3, L1(), 4), fixOf(0, 1, 2, 3, L2(), -6)});
    ArEpochFixes fx;
    addPair(fx, 0, 1, 2, 3, 4, -6);
    fx.searched = true;
    fx.arcs     = continuing(S.lattice);
    fx.fixed    = {fixOf(0, 1, 3, 4, L1(), 1), fixOf(0, 1, 2, 4, L1(), 10)};
    ArLatticeEpoch le = arLatticeBegin(S, fx, ArLatticeEvidence{}, 5);
    EXPECT_TRUE(le.releases.empty());                  // neither fix is in the old rows' span
    std::vector<Unit> units = fcyc::revalidateUnits(S, fx, 5, 1e-6, le);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_FALSE(units[0].fail);
    EXPECT_FALSE(units[0].released);
    fcyc::admitFixes(S, fx, 1, 5, le, &units);
    EXPECT_EQ(le.offers[0].verdict, LatticeVerdict::Added);
    EXPECT_EQ(le.offers[1].verdict, LatticeVerdict::Contradicted);
    EXPECT_EQ(le.offers[1].implied, 5);
    EXPECT_TRUE(units[0].released);
    EXPECT_FALSE(arcgraph::latticeImplied(S.lattice, dd(0, 1, 2, 3, L1())).has_value());
    EXPECT_EQ(arcgraph::latticeImplied(S.lattice, dd(0, 1, 2, 3, L2())), -6);   // the other band stays
}

TEST(NetworkFixedCycleRelease, AUnitTheSearchNoLongerDeterminesLapsesAfterLapseEp) {
    ArEpochFixes fx;
    const Unit pair = addPair(fx, 0, 1, 2, 3, 4, -6);
    std::map<ArDdIdentity, int> unkept;
    auto epoch = [&](bool kept, int lapse_ep) {
        fx.floats[0].fixed = fx.floats[1].fixed = kept;
        std::vector<Unit> units = {pair};
        fcyc::markLapsed(units, fx, unkept, lapse_ep);
        return units[0].lapsed;
    };
    EXPECT_FALSE(epoch(true, 3));
    EXPECT_FALSE(epoch(false, 3));
    EXPECT_FALSE(epoch(false, 3));
    EXPECT_TRUE(epoch(false, 3));                      // the third epoch without the search
    EXPECT_TRUE(epoch(false, 3));
    EXPECT_FALSE(epoch(true, 3));                      // determined again: the count resets
    EXPECT_FALSE(epoch(false, 3));
    // One band determined is not the pair.
    fx.floats[0].fixed = true;
    fx.floats[1].fixed = false;
    std::vector<Unit> one = {pair};
    fcyc::markLapsed(one, fx, unkept, 3);
    EXPECT_EQ(unkept.at(fx.floats[0].id), 2);
    // An epoch at which the unit is not implied ends its count.
    std::vector<Unit> none;
    fcyc::markLapsed(none, fx, unkept, 3);
    EXPECT_TRUE(unkept.empty());
    EXPECT_FALSE(epoch(false, 3));
    // Off: nothing lapses and no count is kept.
    unkept.clear();
    for (int k = 0; k < 20; ++k) EXPECT_FALSE(epoch(false, 0));
    EXPECT_TRUE(unkept.empty());
}

TEST(NetworkFixedCycleRelease, AnAddedRowFoldsFromTheNextEpoch) {
    ArEpochFixes fx;
    addPair(fx, 0, 1, 2, 3, 4, -6);
    ArLatticeState S;
    ASSERT_EQ(arcgraph::latticeOffer(S.lattice, fx.floats[0].cycle, 4, 7), LatticeVerdict::Added);
    ASSERT_EQ(arcgraph::latticeOffer(S.lattice, fx.floats[1].cycle, -6, 7), LatticeVerdict::Added);
    EXPECT_TRUE(fcyc::impliedUnits(S.lattice, fx, 7).empty());
    const std::vector<Unit> u = fcyc::impliedUnits(S.lattice, fx, 8);
    ASSERT_EQ(u.size(), 1u);
    EXPECT_EQ(u[0].n1, 4);
    EXPECT_EQ(u[0].n2, -6);
}

TEST(NetworkFixedCycleRelease, TheFoldsRefusalsReleaseTheirArcsAndALeftFloatBlockKeepsItsRows) {
    ArEpochFixes fx;
    const std::vector<Unit> units = {addPair(fx, 0, 1, 2, 3, 1, 2), addPair(fx, 4, 5, 2, 3, 1, 2),
                                     addPair(fx, 6, 7, 2, 3, 1, 2), addPair(fx, 8, 9, 2, 3, 1, 2)};
    fcyc::EpochFold f;
    f.fate.resize(4);
    f.fate[0].action = fcyc::UnitAction::Dropped;
    f.fate[1].action = fcyc::UnitAction::Degenerate;
    f.fate[2].action = fcyc::UnitAction::LeftFloat;
    f.fate[3].action = fcyc::UnitAction::Folded;
    const std::vector<ArLatticeReleaseEvent> rel = fcyc::foldReleases(units, f, fx);
    ASSERT_EQ(rel.size(), 4u);
    EXPECT_EQ(rel[0].cause, ArLatticeRelease::Acceptance);
    EXPECT_EQ(rel[0].scope, ArLatticeScope::Cycle);
    EXPECT_EQ(rel[0].dd, idOf(0, 1, 2, 3, L1()));
    EXPECT_EQ(rel[1].dd, idOf(0, 1, 2, 3, L2()));
    EXPECT_EQ(rel[2].cause, ArLatticeRelease::Degenerate);
    EXPECT_EQ(rel[3].dd, idOf(4, 5, 2, 3, L2()));
}

/// The floats of one pair at the true integers (a noise-free, converging float) with the
/// covariance @p S; one unit through the ledger.
ArEpochFixes pairFloats(const Eigen::Matrix2d& S, double y1, double y2) {
    ArEpochFixes fx;
    addPair(fx, 0, 1, 2, 3, 0, 0);
    fx.floats[0].y_cyc = y1;
    fx.floats[1].y_cyc = y2;
    fx.floats[0].var_cyc2 = S(0, 0);
    fx.floats[1].var_cyc2 = S(1, 1);
    fx.floats[0].cov_twin_cyc2 = fx.floats[1].cov_twin_cyc2 = S(0, 1);
    return fx;
}

TEST(NetworkFixedCycleRelease, APlantedWrongIntegerTripsWhereTheAnalyticStatisticCrossesTheBar) {
    // A pair converging as 1/t from 1 cycle, correlation 0.9; the true integers 40 and 31.
    const double rho = 0.9;
    const Eigen::Matrix2d S0 = (Eigen::Matrix2d() << 1.0, rho, rho, 1.0).finished();
    const double alpha = 1e-6, bar = -2.0 * std::log(alpha);
    const Eigen::Vector2d d(1.0, 0.0);                     // the planted +1 L1 cycle
    int analytic = -1;
    for (int t = 1; t <= 50 && analytic < 0; ++t)
        if (d.dot((S0 / t).inverse() * d) > bar) analytic = t;
    ASSERT_GT(analytic, 1);
    ArLatticeState Sl;
    {
        const ArEpochFixes fx = pairFloats(S0, 40.0, 31.0);
        ASSERT_EQ(arcgraph::latticeOffer(Sl.lattice, fx.floats[0].cycle, 40, 0), LatticeVerdict::Added);
        ASSERT_EQ(arcgraph::latticeOffer(Sl.lattice, fx.floats[1].cycle, 31, 0), LatticeVerdict::Added);
        ASSERT_EQ(arcgraph::latticeShiftArc(Sl.lattice, arcOf(0, 2, L1()), +1), 1);   // the plant
    }
    int tripped = -1;
    for (int t = 1; t <= 50 && tripped < 0; ++t) {
        const ArEpochFixes fx = pairFloats(S0 / t, 40.0, 31.0);
        std::vector<Unit> units = fcyc::impliedUnits(Sl.lattice, fx, t);
        ASSERT_EQ(units.size(), 1u);
        EXPECT_EQ(units[0].n1, 41);                       // the planted integer
        fcyc::scoreUnits(units, fx, alpha);
        if (!units[0].fail) continue;
        tripped = t;
        ArLatticeEpoch e;
        arLatticeRelease(Sl, fcyc::localizeFailures(units, fx), e);
        EXPECT_EQ(e.released_by[static_cast<std::size_t>(ArLatticeRelease::Revalidation)], 2);
        EXPECT_EQ(e.dims_lost, 2);                        // its two rows, no collateral
        EXPECT_TRUE(fcyc::impliedUnits(Sl.lattice, fx, t + 1).empty());
    }
    EXPECT_EQ(tripped, analytic);

    // The null: right integers, a settled float (1/20 of S0) drawn from its own covariance.
    const Eigen::Matrix2d Sn = S0 / 20.0;
    const Eigen::Matrix2d Lc = Sn.llt().matrixL();
    std::mt19937 rng(20261011u);
    std::normal_distribution<double> z(0.0, 1.0);
    ArLatticeState Sr;
    int false_releases = 0;
    for (int t = 0; t < 10000; ++t) {
        if (Sr.lattice.rows.empty()) {
            const ArEpochFixes fx0 = pairFloats(Sn, 40.0, 31.0);
            arcgraph::latticeOffer(Sr.lattice, fx0.floats[0].cycle, 40, 0);
            arcgraph::latticeOffer(Sr.lattice, fx0.floats[1].cycle, 31, 0);
        }
        const Eigen::Vector2d e = Lc * Eigen::Vector2d(z(rng), z(rng));
        const ArEpochFixes fx = pairFloats(Sn, 40.0 + e(0), 31.0 + e(1));
        std::vector<Unit> units = fcyc::impliedUnits(Sr.lattice, fx, 1);
        fcyc::scoreUnits(units, fx, alpha);
        if (units.empty() || !units[0].fail) continue;
        ++false_releases;
        ArLatticeEpoch ep;
        arLatticeRelease(Sr, fcyc::localizeFailures(units, fx), ep);
    }
    EXPECT_LE(false_releases, 2);
}

// ============================== the cause table ==============================

TEST(NetworkFixedCycleRelease, TheCauseTableIsInEnumOrderWithUniqueNamesAndTheTraceColumns) {
    const std::string header = kArLatticeHeader;
    std::set<std::string> names, columns;
    for (std::size_t k = 0; k < kArLatticeReleaseCount; ++k) {
        const auto cause = static_cast<ArLatticeRelease>(k);
        const ArLatticeReleaseSpec& spec = arLatticeReleaseSpec(cause);
        EXPECT_EQ(spec.cause, cause) << k;
        EXPECT_STREQ(arLatticeReleaseName(cause), spec.name);
        EXPECT_TRUE(names.insert(spec.name).second) << spec.name;
        EXPECT_TRUE(columns.insert(spec.column).second) << spec.column;
        EXPECT_NE(header.find(std::string(",") + spec.column + ","), std::string::npos) << spec.column;
    }
    EXPECT_EQ(names.size(), kArLatticeReleaseCount);
}

}  // namespace
}  // namespace GnssProcessor
