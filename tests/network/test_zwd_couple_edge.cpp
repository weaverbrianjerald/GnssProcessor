/**
 * @file test_zwd_couple_edge.cpp
 * @brief Contract tests for the re-coupling edge of the wet delay's event response on the
 *        float vehicle (standing invariant 7: a returning station is a re-coupling, with
 *        counters and identities, never a first sighting).
 *
 * The station record @c net::StationState::zwd_edge (network/carry/ZwdEdge.hpp) follows one
 * station's carried wet delay from a decoupling edge to the first fold of the entry the harvest
 * re-creates.  The cases here pin:
 *   - each cause re-couples at the first fold of the re-created entry, with the seconds away;
 *   - a first sighting's first fold is a birth, counted apart, never a re-coupling;
 *   - an epoch that does not fold the re-created entry keeps the record open (the fold after it
 *     is the re-coupling, so none is missed);
 *   - a kept entry folded after more than one epoch without a stash is a re-coupling of cause
 *     return_in_horizon, and an ordinary fold or a network-wide gap is not;
 *   - a re-created entry removed again before its first fold is a supersession;
 *   - every decoupling ends in exactly one of: a re-coupling, a supersession, an open record;
 *   - the erase helper opens the record only when it erased an entry;
 *   - the identity row: event, statistic (the fold's step), threshold (the folded sigma),
 *     detail (cause, seconds away, the satellite and drop cause of a RAIM per-arc erase);
 *   - the prior channel's flags on the wet delay's own block row take the response only when
 *     the selection is wired for it, and every other unwired flag stays unwired.
 */

#include <gtest/gtest.h>

#include "math/TimeUtils.hpp"   // GnssTime
#include "network/carry/ZwdCarry.hpp"
#include "network/carry/ZwdEdge.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/qc/RaimPriorChannel.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kDt = 30.0;   // the beds' epoch interval (s)

std::size_t at(ZwdEdgeCause c) { return static_cast<std::size_t>(c); }

/// A carried wet-delay entry last stashed at @p last_seen_s.
LocalCarry entryAt(double last_seen_s) {
    LocalCarry lc;
    lc.value_abs   = 0.12;
    lc.var         = 1e-5;
    lc.last_seen_s = last_seen_s;
    return lc;
}

}  // namespace

// --- the record's lifecycle ---------------------------------------------------

TEST(ZwdCoupleEdgeTest, SlipEraseRecouplesAtTheFirstFoldOfTheRecreatedEntry) {
    std::optional<ZwdCoupleEdge> edge;
    // Epoch k: the slip latch erases the entry before PASS B; the harvest of k re-creates it.
    EXPECT_FALSE(zwdEdgeDecouple(edge, ZwdEdgeCause::SlipLatch, 1000.0).has_value());
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->created_s, 0.0);
    zwdEdgeCreate(edge, 1000.0);
    EXPECT_EQ(edge->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_EQ(edge->created_s, 1000.0);
    // Epoch k+1 folds the re-created entry: the re-coupling, and the record closes.
    const auto rc = zwdEdgeFold(edge, /*folded=*/true, kDt, kDt, 1030.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_DOUBLE_EQ(rc->away_s, 30.0);
    EXPECT_FALSE(edge.has_value());
    // The next ordinary fold closes nothing.
    EXPECT_FALSE(zwdEdgeFold(edge, true, kDt, kDt, 1060.0).has_value());
}

TEST(ZwdCoupleEdgeTest, RaimEraseCarriesItsSatelliteAndDropCause) {
    std::optional<ZwdCoupleEdge> edge;
    // The attribution runs after the harvest of k: no entry at k+1's PASS B, re-created at k+1.
    zwdEdgeDecouple(edge, ZwdEdgeCause::RaimArc, 2000.0, "E04", "t4_attrib");
    zwdEdgeCreate(edge, 2030.0);
    const auto rc = zwdEdgeFold(edge, true, kDt, kDt, 2060.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::RaimArc);
    EXPECT_DOUBLE_EQ(rc->away_s, 60.0);
    EXPECT_EQ(rc->sat, "E04");
    EXPECT_EQ(rc->via, "t4_attrib");
}

TEST(ZwdCoupleEdgeTest, FirstSightingIsABirthNotARecoupling) {
    std::optional<ZwdCoupleEdge> edge;
    zwdEdgeCreate(edge, 500.0);   // no record: the station's first entry on this run
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->cause, ZwdEdgeCause::FirstSighting);
    const auto rc = zwdEdgeFold(edge, true, kDt, kDt, 530.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::FirstSighting);
    EXPECT_DOUBLE_EQ(rc->away_s, 0.0);
}

TEST(ZwdCoupleEdgeTest, AnEpochThatDoesNotFoldKeepsTheRecordOpen) {
    // The defect this record closes: the re-stash after a re-creation reset the stamp, so a
    // re-created entry whose first harvest did not fold was never counted.
    std::optional<ZwdCoupleEdge> edge;
    zwdEdgeDecouple(edge, ZwdEdgeCause::SlipLatch, 1000.0);
    zwdEdgeCreate(edge, 1000.0);
    EXPECT_FALSE(zwdEdgeFold(edge, /*folded=*/false, kDt, kDt, 1030.0).has_value());
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->created_s, 1000.0);
    const auto rc = zwdEdgeFold(edge, /*folded=*/true, kDt, kDt, 1060.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_DOUBLE_EQ(rc->away_s, 60.0);
}

TEST(ZwdCoupleEdgeTest, AFoldAfterAnAbsenceWithinTheHorizonIsAReturn) {
    std::optional<ZwdCoupleEdge> edge;
    const auto rc = zwdEdgeFold(edge, true, /*gap_s=*/300.0, kDt, 4000.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::Return);
    EXPECT_DOUBLE_EQ(rc->away_s, 300.0);
    EXPECT_FALSE(edge.has_value());   // a return keeps its entry: no record opens
    // An ordinary fold, a network-wide gap (the gap equals the epoch step), an absence with
    // no fold, and the first epoch (no step) are not returns.
    EXPECT_FALSE(zwdEdgeFold(edge, true, kDt, kDt, 4030.0).has_value());
    EXPECT_FALSE(zwdEdgeFold(edge, true, 300.0, 300.0, 4330.0).has_value());
    EXPECT_FALSE(zwdEdgeFold(edge, false, 300.0, kDt, 4360.0).has_value());
    EXPECT_FALSE(zwdEdgeFold(edge, true, 300.0, 0.0, 4390.0).has_value());
}

TEST(ZwdCoupleEdgeTest, GapPruneRecouplesOnTheStationsReturn) {
    std::optional<ZwdCoupleEdge> edge;
    zwdEdgeDecouple(edge, ZwdEdgeCause::GapPrune, 5000.0);
    // The station is away: no harvest, no fold, the record waits.
    zwdEdgeCreate(edge, 5600.0);   // it returns; the harvest re-creates the entry
    const auto rc = zwdEdgeFold(edge, true, kDt, kDt, 5630.0);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->cause, ZwdEdgeCause::GapPrune);   // a re-coupling, never a first sighting
    EXPECT_DOUBLE_EQ(rc->away_s, 630.0);
}

TEST(ZwdCoupleEdgeTest, ARecreatedEntryRemovedBeforeItsFirstFoldIsSuperseded) {
    std::optional<ZwdCoupleEdge> edge;
    zwdEdgeDecouple(edge, ZwdEdgeCause::SlipLatch, 1000.0);
    zwdEdgeCreate(edge, 1000.0);
    // The attribution after the same harvest erases the re-created entry.
    const auto lost = zwdEdgeDecouple(edge, ZwdEdgeCause::RaimArc, 1000.0, "G21", "t4_prior");
    ASSERT_TRUE(lost.has_value());
    EXPECT_EQ(*lost, ZwdEdgeCause::SlipLatch);
    EXPECT_EQ(edge->cause, ZwdEdgeCause::RaimArc);
    EXPECT_EQ(edge->created_s, 0.0);
    // A first sighting removed before its first fold is superseded too.
    std::optional<ZwdCoupleEdge> born;
    zwdEdgeCreate(born, 10.0);
    const auto lost_birth = zwdEdgeDecouple(born, ZwdEdgeCause::GapPrune, 1000.0);
    ASSERT_TRUE(lost_birth.has_value());
    EXPECT_EQ(*lost_birth, ZwdEdgeCause::FirstSighting);
}

TEST(ZwdCoupleEdgeTest, EveryDecouplingEndsInOneOutcome) {
    // Three stations through a scripted sequence; decouplings by cause must equal
    // re-couplings + supersessions + open records, cause by cause.
    net::StationTable rows;
    ZwdEdgeTally decoupled{}, recoupled{}, superseded{};
    const auto decouple = [&](int s, ZwdEdgeCause c, double t) {
        ++decoupled[at(c)];
        if (const auto lost = zwdEdgeDecouple(rows[StationId{s}].zwd_edge, c, t))
            ++superseded[at(*lost)];
    };
    const auto create = [&](int s, double t) { zwdEdgeCreate(rows[StationId{s}].zwd_edge, t); };
    const auto fold = [&](int s, bool folded, double t) {
        if (const auto rc = zwdEdgeFold(rows[StationId{s}].zwd_edge, folded, kDt, kDt, t))
            ++recoupled[at(rc->cause)];
    };
    // Station 0: slip, re-created, folds.  Then a RAIM erase, re-created, removed by a slip
    // before folding (superseded), re-created, never folds again (open at the end).
    decouple(0, ZwdEdgeCause::SlipLatch, 100.0); create(0, 100.0); fold(0, true, 130.0);
    decouple(0, ZwdEdgeCause::RaimArc, 200.0);   create(0, 230.0);
    decouple(0, ZwdEdgeCause::SlipLatch, 260.0); create(0, 260.0);
    // Station 1: pruned, returns, its first harvest does not fold, then folds.
    decouple(1, ZwdEdgeCause::GapPrune, 300.0); create(1, 900.0);
    fold(1, false, 930.0); fold(1, true, 960.0);
    // Station 2: a prior-channel erase that is never re-created (the station left).
    decouple(2, ZwdEdgeCause::RaimPrior, 400.0);

    const ZwdEdgeTally open_at_end = zwdEdgePendingByCause(rows);
    for (std::size_t c = at(ZwdEdgeCause::SlipLatch); c <= at(ZwdEdgeCause::GapPrune); ++c)
        EXPECT_EQ(decoupled[c], recoupled[c] + superseded[c] + open_at_end[c]) << "cause " << c;
    EXPECT_EQ(recoupled[at(ZwdEdgeCause::SlipLatch)], 1);
    EXPECT_EQ(superseded[at(ZwdEdgeCause::RaimArc)], 1);
    EXPECT_EQ(open_at_end[at(ZwdEdgeCause::SlipLatch)], 1);
    EXPECT_EQ(recoupled[at(ZwdEdgeCause::GapPrune)], 1);
    EXPECT_EQ(open_at_end[at(ZwdEdgeCause::RaimPrior)], 1);
}

// --- the run-side helpers -----------------------------------------------------

TEST(ZwdCoupleEdgeTest, EraseOpensTheRecordOnlyWhenItErasedAnEntry) {
    net::StationTable rows;
    net::put(rows, StationId{0}, &net::StationState::zwd) = entryAt(990.0);
    ZwdCarryCounters c;
    EXPECT_TRUE(zwdEventErase(rows, 0, ZwdEdgeCause::RaimArc, 1000.0, c, "G07", "t4_attrib"));
    EXPECT_EQ(net::get(rows, StationId{0}, &net::StationState::zwd), nullptr);
    ASSERT_TRUE(zwdEdgeOf(rows, 0).has_value());
    EXPECT_EQ(zwdEdgeOf(rows, 0)->cause, ZwdEdgeCause::RaimArc);
    EXPECT_EQ(zwdEdgeOf(rows, 0)->sat, "G07");
    // A station with no carried entry: nothing erased, no record, no count.
    EXPECT_FALSE(zwdEventErase(rows, 1, ZwdEdgeCause::SlipLatch, 1000.0, c));
    EXPECT_FALSE(zwdEdgeOf(rows, 1).has_value());
    // A re-created entry erased before its first fold is counted under the lost cause.
    zwdEdgeCreate(zwdEdgeOf(rows, 0), 1030.0);
    net::put(rows, StationId{0}, &net::StationState::zwd) = entryAt(1030.0);
    EXPECT_TRUE(zwdEventErase(rows, 0, ZwdEdgeCause::RaimPrior, 1030.0, c));
    EXPECT_EQ(c.n_zwd_superseded_by[at(ZwdEdgeCause::RaimArc)], 1);
}

TEST(ZwdCoupleEdgeTest, IdentityRowNamesTheCauseAndTheFoldsStep) {
    const fs::path dir = gnss_test::tempDir() / "zwd_couple_edge_rows";
    fs::create_directories(dir);
    ZwdCarryCounters c;
    {
        NetworkRunTraces traces(dir);
        traces.events_trace.open(dir / "events.csv");
        GnssTime t;
        t.week = 2313;
        t.tow  = 262800.0;
        ZwdRecouple raim;
        raim.cause = ZwdEdgeCause::RaimArc; raim.away_s = 60.0; raim.sat = "E04"; raim.via = "t4_attrib";
        zwdNoteRecouple(c, traces, t, "HKSL00HKG", raim, -0.05, 0.004);
        ZwdRecouple birth;   // a first sighting
        zwdNoteRecouple(c, traces, t, "KIT300UZB", birth, 0.01, 0.02);
        ZwdRecouple back;
        back.cause = ZwdEdgeCause::Return; back.away_s = 300.0;
        zwdNoteRecouple(c, traces, t, "KIT300UZB", back, 0.002, 0.005);
        traces.events_trace.close();
    }
    EXPECT_EQ(c.n_zwd_recouple, 2);   // the first sighting is not a re-coupling
    EXPECT_EQ(c.n_zwd_recouple_by[at(ZwdEdgeCause::RaimArc)], 1);
    EXPECT_EQ(c.n_zwd_recouple_by[at(ZwdEdgeCause::FirstSighting)], 1);
    EXPECT_EQ(c.n_zwd_recouple_by[at(ZwdEdgeCause::Return)], 1);

    std::ifstream in(dir / "events.csv");
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find(",ZWD,recouple,E04,HKSL00HKG,"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(",fold,cause=raim_attribution;away_s=60.0;via=t4_attrib"),
              std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find(",ZWD,first_fold,,KIT300UZB,"), std::string::npos) << lines[1];
    EXPECT_NE(lines[1].find("cause=first_sighting"), std::string::npos) << lines[1];
    EXPECT_NE(lines[2].find(",ZWD,recouple,,KIT300UZB,"), std::string::npos) << lines[2];
    EXPECT_NE(lines[2].find("cause=return_in_horizon;away_s=300.0"), std::string::npos) << lines[2];
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST(ZwdCoupleEdgeTest, CauseNamesAreTheTraceVocabulary) {
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::FirstSighting), "first_sighting");
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::SlipLatch), "slip_latch");
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::RaimArc), "raim_attribution");
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::RaimPrior), "raim_prior");
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::GapPrune), "gap_prune");
    EXPECT_STREQ(zwdEdgeCauseName(ZwdEdgeCause::Return), "return_in_horizon");
}

// --- the prior channel's flags on the wet delay -------------------------------

TEST(ZwdCoupleEdgeTest, PriorChannelWiresOnlyTheWetDelaysOwnRow) {
    std::vector<RaimPriorRow> rows(3);
    rows[0].station = 0; rows[0].kind = ContributionKind::BlockZwdCarry;
    rows[1].station = 1; rows[1].kind = ContributionKind::BlockRxClockCarry;
    rows[2].station = 2; rows[2].kind = ContributionKind::BlockZwdCarry;
    RaimPriorSelection sel;
    sel.cells = {{0, "block_zwd_carry", 9.0}, {1, "block_rx_clock_carry", 8.0},
                 {2, "block_zwd_carry", 7.0}};
    sel.cell_row = {0, 1, 2};
    sel.interaction = {9.0, 8.0, 7.0};
    sel.unwired = {0, 1, 2};
    raimPriorWireZwd(sel, rows);
    EXPECT_EQ(sel.zwd, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(sel.unwired, (std::vector<std::size_t>{1}));
    EXPECT_TRUE(sel.acted(0, "block_zwd_carry"));
    EXPECT_TRUE(sel.acted(2, "block_zwd_carry"));
    EXPECT_FALSE(sel.acted(1, "block_rx_clock_carry"));
    // The tally counts the wired flags apart from the unwired ones.
    RaimPriorChannelTally tally;
    raimPriorChannelNoteSelection(sel, true, tally);
    EXPECT_EQ(tally.zwd_flags, 2);
    EXPECT_EQ(tally.unwired, 1);
}

TEST(ZwdCoupleEdgeTest, AnUnwiredSelectionKeepsEveryFlagUnwired) {
    // Without the call (the response unarmed), a wet-delay flag stays unwired, as before.
    RaimPriorSelection sel;
    sel.cells = {{0, "block_zwd_carry", 9.0}};
    sel.cell_row = {0};
    sel.interaction = {9.0};
    sel.unwired = {0};
    RaimPriorChannelTally tally;
    raimPriorChannelNoteSelection(sel, true, tally);
    EXPECT_EQ(tally.unwired, 1);
    EXPECT_EQ(tally.zwd_flags, 0);
    EXPECT_TRUE(sel.zwd.empty());
}

TEST(ZwdCoupleEdgeTest, AFreshStationCarriesNoRecord) {
    net::StationState s;
    EXPECT_FALSE(s.zwd_edge.has_value());
    ZwdCarryCounters c;
    for (std::size_t k = 0; k < kZwdEdgeCauses; ++k) {
        EXPECT_EQ(c.n_zwd_recouple_by[k], 0);
        EXPECT_EQ(c.n_zwd_superseded_by[k], 0);
    }
}
