/**
 * @file test_network_ar_suspect.cpp
 * @brief Unit tests of the producer AR search's suspect-corner mask (card ARL, slip-ladder
 *        defect 3; include/filter/NetworkArSuspect.hpp).
 *
 * Five suites:
 *  - @c NetworkArSuspectLedger: the ledger kernel (stamp, window, age-out, the fold's
 *    three in-place sources and who owns each event, the fold clock's second-to-epoch
 *    map, the post-search stamp, the event count);
 *  - @c NetworkArSuspectSearchOrder: the masked search subset
 *    (@ref netar::maskedSearchOrder) and its trace labels (@ref netar::mapFixToBasis);
 *  - @c NetworkArSuspectRelease: one search epoch's mask and both its edges (the pool
 *    corner, the basis split, the release and the close-out tallies);
 *  - @c NetworkArSuspectReentry: the pair lanes (@ref netar::pairLanes) and what the
 *    released DDs got at their first search (@ref netar::reentryOutcome);
 *  - @c NetworkArSuspectConfig: the loader key and the knob snapshot's resolution
 *    against @c ambiguity_resolution.enable.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"     // NetworkKnobs::snapshot
#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkArSuspect.hpp"
#include "filter/NetworkProducerAr.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

LosId los(int sta, const char* sat) { return LosId{StationId{sta}, *SatId::parse(sat)}; }

using CarryMap = net::ArcTable;

LocalCarry carryEntry(double birth_s, double relevel_s, long long dropped_ep) {
    LocalCarry lc{};
    lc.value_abs  = 0.0;
    lc.var        = 1.0;
    lc.last_seen_s = relevel_s > 0.0 ? relevel_s : birth_s;
    lc.birth_s    = birth_s;
    lc.relevel_s  = relevel_s;
    lc.dropped_ep = dropped_ep;
    return lc;
}

// ------------------------------------------------------------------- ledger

TEST(NetworkArSuspectLedger, StampIsLiveForExactlyTheWindow) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    arSuspectNote(L, los(3, "G14"), 31, kArSuspectGfJump, &c);
    // Window 10: live at epochs 31..40, clean from 41.
    EXPECT_EQ(arSuspectBits(L, los(3, "G14"), 31, 10), kArSuspectGfJump);
    EXPECT_EQ(arSuspectBits(L, los(3, "G14"), 40, 10), kArSuspectGfJump);
    EXPECT_EQ(arSuspectBits(L, los(3, "G14"), 41, 10), 0);
    // Another satellite, another station: clean.
    EXPECT_EQ(arSuspectBits(L, los(3, "G05"), 31, 10), 0);
    EXPECT_EQ(arSuspectBits(L, los(4, "G14"), 31, 10), 0);
    // Window 0 is off.
    EXPECT_EQ(arSuspectBits(L, los(3, "G14"), 31, 0), 0);
    EXPECT_EQ(c.stamps[5], 1);
}

TEST(NetworkArSuspectLedger, CausesAreKeptApartAndOred) {
    ArSuspectLedger L;
    arSuspectNote(L, los(1, "G05"), 10, kArSuspectRaim, nullptr);
    arSuspectNote(L, los(1, "G05"), 14, kArSuspectMwUnknown, nullptr);
    EXPECT_EQ(arSuspectBits(L, los(1, "G05"), 15, 5), kArSuspectMwUnknown);   // RAIM aged out (15-10 = 5)
    EXPECT_EQ(arSuspectBits(L, los(1, "G05"), 14, 5),
              kArSuspectRaim | kArSuspectMwUnknown);
}

TEST(NetworkArSuspectLedger, AStampThatDoesNotAdvanceIsNotCounted) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    arSuspectNote(L, los(2, "G13"), 20, kArSuspectDrop, &c);
    arSuspectNote(L, los(2, "G13"), 20, kArSuspectDrop, &c);   // the second band, same epoch
    arSuspectNote(L, los(2, "G13"), 18, kArSuspectDrop, &c);   // older: kept out
    EXPECT_EQ(c.stamps[2], 1);
    EXPECT_EQ(L.corners.at(los(2, "G13")).last_ep[2], 20);
    arSuspectNote(L, los(2, "G13"), 22, kArSuspectDrop, &c);
    EXPECT_EQ(c.stamps[2], 2);
}

TEST(NetworkArSuspectLedger, StringNoteRefusesAnUnparsableSatellite) {
    ArSuspectLedger L;
    EXPECT_FALSE(arSuspectNote(L, 0, "X99", 5, kArSuspectRaim, nullptr));
    EXPECT_FALSE(arSuspectNote(L, -1, "G05", 5, kArSuspectRaim, nullptr));
    EXPECT_TRUE(L.corners.empty());
    EXPECT_TRUE(arSuspectNote(L, 0, "G05", 5, kArSuspectRaim, nullptr));
    EXPECT_EQ(arSuspectBits(L, los(0, "G05"), 5, 1), kArSuspectRaim);
}

TEST(NetworkArSuspectLedger, FoldStampsTheFreeEpochLatch) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    RelevelEpochState relevel;
    relevel.free_ambs.insert({7, StateKeys::makeAmbiguityKey("G14", "L1C")});
    relevel.free_ambs.insert({7, StateKeys::makeAmbiguityKey("G14", "L2W")});
    const CarryMap carry;
    arSuspectFoldEpoch(L, 31, 1000.0, 10, relevel, carry, &c);
    EXPECT_EQ(arSuspectBits(L, los(7, "G14"), 31, 10), kArSuspectFree);
    EXPECT_EQ(c.stamps[0], 1);   // two bands, one corner, one stamp
}

TEST(NetworkArSuspectLedger, FoldReadsTheDropStampInsideTheWindowOnly) {
    ArSuspectLedger L;
    const RelevelEpochState relevel;
    CarryMap carry;
    net::putArcIn(carry, {2, "G05", "L1C"}, &net::ArcState::amb) = carryEntry(100.0, 0.0, 25);
    net::putArcIn(carry, {3, "G06", "L1C"}, &net::ArcState::amb) = carryEntry(100.0, 0.0, 10);   // 30 - 10 = 20 >= 10: old
    arSuspectFoldEpoch(L, 30, 3000.0, 10, relevel, carry, nullptr);
    EXPECT_EQ(arSuspectBits(L, los(2, "G05"), 30, 10), kArSuspectDrop);
    // First seen by this fold (no fold ran at 25..29): stamped at the first search that
    // can act on it.
    EXPECT_EQ(L.corners.at(los(2, "G05")).last_ep[2], 30);
    EXPECT_EQ(arSuspectBits(L, los(3, "G06"), 30, 10), 0);
    EXPECT_EQ(L.corners.count(los(3, "G06")), 0u);
}

TEST(NetworkArSuspectLedger, APostSearchStampMasksTheNextWindowOfSearches) {
    ArSuspectLedger L;
    // A RAIM stamp written after the search of epoch 30.
    EXPECT_EQ(arSuspectPostSearchEpoch(30), 31);
    arSuspectNote(L, los(1, "G14"), arSuspectPostSearchEpoch(30), kArSuspectRaim, nullptr);
    EXPECT_EQ(arSuspectBits(L, los(1, "G14"), 30, 10), 0);   // not live in its own epoch
    EXPECT_EQ(arSuspectBits(L, los(1, "G14"), 31, 10), kArSuspectRaim);
    EXPECT_EQ(arSuspectBits(L, los(1, "G14"), 40, 10), kArSuspectRaim);
    EXPECT_EQ(arSuspectBits(L, los(1, "G14"), 41, 10), 0);   // exactly ten searches
    // The fold of epoch 31 keeps it (its newest stamp is that epoch's).
    const RelevelEpochState relevel;
    const CarryMap carry;
    arSuspectFoldEpoch(L, 31, 930.0, 10, relevel, carry, nullptr);
    EXPECT_EQ(L.corners.count(los(1, "G14")), 1u);
}

TEST(NetworkArSuspectLedger, FoldStampsADropOnceAndLeavesTheRaimDropToRaim) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    const RelevelEpochState relevel;
    CarryMap carry;
    net::putArcIn(carry, {4, "G07", "L1C"}, &net::ArcState::amb) = carryEntry(1.0, 0.0, 0);
    for (long long e = 1; e <= 4; ++e) {
        // A pre-search drop at epoch 2 (SB_PREASM, before the fold).
        if (e == 2) net::putArcIn(carry, {4, "G07", "L1C"}, &net::ArcState::amb).dropped_ep = 2;
        arSuspectFoldEpoch(L, e, 30.0 * static_cast<double>(e), 10, relevel, carry, &c);
        // A post-search drop at epoch 3 (RAIM, after the fold): RAIM stamps it itself.
        if (e == 3) {
            net::putArcIn(carry, {4, "G07", "L1C"}, &net::ArcState::amb).dropped_ep = 3;
            arSuspectNote(L, los(4, "G07"), arSuspectPostSearchEpoch(3), kArSuspectRaim, &c);
        }
    }
    EXPECT_EQ(c.stamps[2], 1);   // the drop at 2, once, though re-read at 3 and 4
    EXPECT_EQ(L.corners.at(los(4, "G07")).last_ep[2], 2);
    EXPECT_EQ(c.stamps[3], 1);
    EXPECT_EQ(L.corners.at(los(4, "G07")).last_ep[3], 4);
    EXPECT_EQ(c.events, 2);
}

TEST(NetworkArSuspectLedger, AReLevelIsOwnedByItsDropOrItsLatch) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    CarryMap carry;
    RelevelEpochState relevel;
    // Epoch 1: an arc's float-vehicle drop (pre-search) writes dropped_ep and relevel_s.
    // Epoch 2: another arc is in the latch; its harvest re-level (post-search) is read at 3.
    net::putArcIn(carry, {0, "G01", "L1C"}, &net::ArcState::amb) = carryEntry(0.5, 0.0, 0);
    net::putArcIn(carry, {1, "G02", "L1C"}, &net::ArcState::amb) = carryEntry(0.5, 0.0, 0);
    net::putArcIn(carry, {0, "G01", "L1C"}, &net::ArcState::amb).relevel_s  = 30.0;
    net::putArcIn(carry, {0, "G01", "L1C"}, &net::ArcState::amb).dropped_ep = 1;
    arSuspectFoldEpoch(L, 1, 30.0, 10, relevel, carry, &c);
    relevel.free_ambs.insert({1, StateKeys::makeAmbiguityKey("G02", "L1C")});
    arSuspectFoldEpoch(L, 2, 60.0, 10, relevel, carry, &c);
    net::putArcIn(carry, {1, "G02", "L1C"}, &net::ArcState::amb).relevel_s = 60.0;   // the harvest of epoch 2
    relevel.free_ambs.clear();
    arSuspectFoldEpoch(L, 3, 90.0, 10, relevel, carry, &c);
    EXPECT_EQ(c.stamps[1], 0);   // no re-level stamp: both are owned
    EXPECT_EQ(c.stamps[0], 1);   // the latch
    EXPECT_EQ(c.stamps[2], 1);   // the drop
    EXPECT_EQ(c.events, 2);
    // A re-level nobody owns (an unknown writer at epoch 3) is stamped at its own epoch.
    net::putArcIn(carry, {2, "G03", "L1C"}, &net::ArcState::amb) = carryEntry(0.5, 90.0, 0);
    arSuspectFoldEpoch(L, 4, 120.0, 10, relevel, carry, &c);
    EXPECT_EQ(c.stamps[1], 1);
    EXPECT_EQ(L.corners.at(los(2, "G03")).last_ep[1], 3);
}

TEST(NetworkArSuspectLedger, EventsCountDistinctCornerStamps) {
    ArSuspectLedger L;
    ArSuspectCounters c;
    arSuspectNote(L, los(5, "G10"), 7, kArSuspectFree, &c);
    arSuspectNote(L, los(5, "G10"), 7, kArSuspectDrop, &c);       // same event, two causes
    arSuspectNote(L, los(5, "G10"), 7, kArSuspectGfJump, &c);
    arSuspectNote(L, los(5, "G11"), 7, kArSuspectFree, &c);       // another corner
    arSuspectNote(L, los(5, "G10"), 9, kArSuspectMwUnknown, &c);  // another stamp
    EXPECT_EQ(c.stamps[0] + c.stamps[2] + c.stamps[4] + c.stamps[5], 5);
    EXPECT_EQ(c.events, 3);
}

TEST(NetworkArSuspectLedger, FoldTellsAReLevelFromAFirstStash) {
    ArSuspectLedger L;
    const RelevelEpochState relevel;
    CarryMap carry;
    // Epochs 1..3 at 30 s.  Arc A re-levelled at epoch 2 (born earlier); arc B born at
    // epoch 2 (relevel_s == birth_s): only A is a re-level.
    for (long long e = 1; e <= 3; ++e) {
        const double s = 30.0 * static_cast<double>(e);
        if (e == 3) {
            net::putArcIn(carry, {0, "G01", "L1C"}, &net::ArcState::amb) = carryEntry(/*birth*/ 0.5, /*relevel*/ 60.0, 0);
            net::putArcIn(carry, {1, "G02", "L1C"}, &net::ArcState::amb) = carryEntry(/*birth*/ 60.0, /*relevel*/ 60.0, 0);
        }
        arSuspectFoldEpoch(L, e, s, 10, relevel, carry, nullptr);
    }
    EXPECT_EQ(arSuspectBits(L, los(0, "G01"), 3, 10), kArSuspectRelevel);
    EXPECT_EQ(L.corners.at(los(0, "G01")).last_ep[1], 2);   // mapped back onto epoch 2
    EXPECT_EQ(arSuspectBits(L, los(1, "G02"), 3, 10), 0);
}

TEST(NetworkArSuspectLedger, FoldAgesOutAndKeepsItsClockAtTheWindow) {
    ArSuspectLedger L;
    const RelevelEpochState relevel;
    const CarryMap carry;
    arSuspectNote(L, los(5, "G20"), 1, kArSuspectRaim, nullptr);
    for (long long e = 1; e <= 6; ++e)
        arSuspectFoldEpoch(L, e, 30.0 * static_cast<double>(e), 3, relevel, carry, nullptr);
    EXPECT_EQ(L.recent.size(), 3u);
    EXPECT_EQ(L.recent.front().first, 4);
    EXPECT_TRUE(L.corners.empty());   // 6 - 1 >= 3
}

TEST(NetworkArSuspectLedger, EpochOfSecondMapsInsideTheClockOnly) {
    std::deque<std::pair<long long, double>> recent{{10, 300.0}, {11, 330.0}, {13, 390.0}};
    EXPECT_EQ(arSuspectEpochOfSecond(recent, 330.0), 11);
    EXPECT_EQ(arSuspectEpochOfSecond(recent, 330.0005), 11);   // inside the tolerance
    EXPECT_EQ(arSuspectEpochOfSecond(recent, 360.0), 11);      // an epoch the fold skipped
    EXPECT_EQ(arSuspectEpochOfSecond(recent, 299.0), 0);       // older than the clock
    EXPECT_EQ(arSuspectEpochOfSecond({}, 330.0), 0);
}

TEST(NetworkArSuspectLedger, CauseSummaryNamesTheNonZeroCauses) {
    std::array<long long, kArSuspectCauseCount> by{};
    EXPECT_EQ(arSuspectCauseSummary(by), "none");
    by[0] = 2; by[4] = 1;
    EXPECT_EQ(arSuspectCauseSummary(by), "free 2, mw_unknown 1");
    EXPECT_STREQ(arSuspectCauseName(5), "gf_jump");
    EXPECT_STREQ(arSuspectCauseName(6), "?");
}

// ------------------------------------------------------------- search order

Eigen::MatrixXd diagQ(std::initializer_list<double> d) {
    Eigen::VectorXd v(static_cast<Eigen::Index>(d.size()));
    Eigen::Index k = 0;
    for (double x : d) v(k++) = x;
    return v.asDiagonal();
}

TEST(NetworkArSuspectSearchOrder, NoMaskIsTheUnmaskedOrder) {
    const Eigen::MatrixXd Q = diagQ({0.4, 0.1, 0.3, 0.2});
    for (int cap : {0, 2, 4, 9}) {
        EXPECT_EQ(netar::maskedSearchOrder(Q, cap, {}), netar::lowestSigmaOrder(Q, cap));
        EXPECT_EQ(netar::maskedSearchOrder(Q, cap, {0, 0, 0, 0}),
                  netar::lowestSigmaOrder(Q, cap));
    }
}

TEST(NetworkArSuspectSearchOrder, MaskedDdsLeaveTheSearchBeforeTheCap) {
    const Eigen::MatrixXd Q = diagQ({0.4, 0.1, 0.3, 0.2});
    const std::vector<char> m{0, 1, 0, 0};
    // Uncapped: the eligible DDs in basis order.
    EXPECT_EQ(netar::maskedSearchOrder(Q, 0, m), (std::vector<int>{0, 2, 3}));
    // A cap at or above the eligible count: the same.
    EXPECT_EQ(netar::maskedSearchOrder(Q, 3, m), (std::vector<int>{0, 2, 3}));
    // A cap below it: the lowest-sigma eligible DDs (index 1, the lowest overall, is masked).
    EXPECT_EQ(netar::maskedSearchOrder(Q, 2, m), (std::vector<int>{3, 2}));
    // Everything masked: an empty search.
    EXPECT_TRUE(netar::maskedSearchOrder(Q, 0, {1, 1, 1, 1}).empty());
}

TEST(NetworkArSuspectSearchOrder, AMaskedDdKeepsItsTraceRowOutsideTheSubset) {
    const Eigen::MatrixXd Q = diagQ({0.4, 0.1, 0.3, 0.2});
    const std::vector<int> order = netar::maskedSearchOrder(Q, 0, {0, 1, 0, 0});
    Eigen::VectorXd afix(2);
    afix << 7.0, -3.0;
    // The search fixed its rows 0 and 2, i.e. basis DDs 0 and 3.
    const auto lab = netar::mapFixToBasis(4, order, {0, 2}, afix);
    ASSERT_EQ(lab.size(), 4u);
    EXPECT_TRUE(lab[0].in_subset);  EXPECT_TRUE(lab[0].kept);  EXPECT_EQ(lab[0].fixed_int, 7.0);
    EXPECT_FALSE(lab[1].in_subset); EXPECT_FALSE(lab[1].kept);   // the masked DD
    EXPECT_TRUE(lab[2].in_subset);  EXPECT_FALSE(lab[2].kept);
    EXPECT_TRUE(lab[3].in_subset);  EXPECT_TRUE(lab[3].kept);  EXPECT_EQ(lab[3].fixed_int, -3.0);
}

// ------------------------------------------------------------------ release

TEST(NetworkArSuspectRelease, AnInertMaskTouchesNothing) {
    ArSuspectLedger L;
    ArSuspectCounters C;
    arSuspectNote(L, los(0, "G05"), 5, kArSuspectRaim, nullptr);
    ArSuspectSearch S = arSuspectOpenSearch(5, 0);
    const ArSuspectCornerState st = arSuspectPoolCorner(S, L, 0, "G05");
    EXPECT_EQ(st.bits, 0);
    EXPECT_FALSE(st.released);
    arSuspectMaskBasis(S, {st, st});
    EXPECT_TRUE(S.masked.empty());
    EXPECT_TRUE(S.reentry.empty());
    EXPECT_TRUE(arSuspectCloseSearch(S, L, C, ArSuspectReentry{}, 0.0).empty());
    EXPECT_EQ(C.epochs, 0);
    EXPECT_TRUE(L.masking.empty());
}

TEST(NetworkArSuspectRelease, CombineOrsBitsAndReleases) {
    const ArSuspectCornerState clean{}, sus{kArSuspectGfJump, false}, rel{0, true},
        sus2{kArSuspectRaim, false};
    EXPECT_EQ(arSuspectCombine(clean, clean, clean, clean).bits, 0);
    EXPECT_FALSE(arSuspectCombine(clean, clean, clean, clean).released);
    EXPECT_EQ(arSuspectCombine(sus, clean, sus2, clean).bits, kArSuspectGfJump | kArSuspectRaim);
    const ArSuspectCornerState r = arSuspectCombine(clean, rel, clean, clean);
    EXPECT_EQ(r.bits, 0);
    EXPECT_TRUE(r.released);
}

TEST(NetworkArSuspectRelease, MaskBasisSplitsMaskedFromReentered) {
    ArSuspectSearch S = arSuspectOpenSearch(10, 5);
    const ArSuspectCornerState clean{}, sus{static_cast<std::uint8_t>(kArSuspectFree | kArSuspectDrop), false},
        rel{0, true}, both{kArSuspectMwUnknown, true};
    arSuspectMaskBasis(S, {clean, sus, rel, both});
    EXPECT_EQ(S.masked, (std::vector<char>{0, 1, 0, 1}));    // a suspect corner wins over a release
    EXPECT_EQ(S.reentry, (std::vector<char>{0, 0, 1, 0}));
    EXPECT_EQ(S.dd_bits, (std::vector<std::uint8_t>{0, 5, 0, 16}));
    EXPECT_EQ(S.n_masked, 2);
    EXPECT_EQ(S.n_reentry, 1);
    EXPECT_EQ(S.masked_by[0], 1);
    EXPECT_EQ(S.masked_by[2], 1);
    EXPECT_EQ(S.masked_by[4], 1);
}

TEST(NetworkArSuspectRelease, AMaskedCornerIsReleasedAtTheFirstSearchThatMeetsItClean) {
    ArSuspectLedger L;
    ArSuspectCounters C;
    const RelevelEpochState relevel;
    const CarryMap carry;
    // An MW stamp at epoch 10, window 3: masked at the searches of 10, 11 and 12.
    arSuspectNote(L, los(2, "G14"), 10, kArSuspectMwUnknown, &C);
    for (long long e = 10; e <= 14; ++e) {
        arSuspectFoldEpoch(L, e, 30.0 * static_cast<double>(e), 3, relevel, carry, &C);
        ArSuspectSearch S = arSuspectOpenSearch(e, 3);
        // The arc is out of the pool at 13 (no search meets it), back at 14.
        std::vector<ArSuspectCornerState> dds;
        if (e != 13) {
            const ArSuspectCornerState st = arSuspectPoolCorner(S, L, 2, "G14");
            const ArSuspectCornerState ref = arSuspectPoolCorner(S, L, 2, "G05");
            dds.push_back(arSuspectCombine(st, ref, ArSuspectCornerState{}, ArSuspectCornerState{}));
        }
        arSuspectMaskBasis(S, dds);
        ArSuspectReentry re;
        re.dd = S.n_reentry;
        const std::string text = arSuspectCloseSearch(S, L, C, re, 100.0 + static_cast<double>(e));
        if (e <= 12) {
            EXPECT_EQ(S.n_masked, 1) << e;
            EXPECT_EQ(L.masking.count(los(2, "G14")), 1u) << e;
        }
        if (e == 13) {
            // The window expired while the arc was away: still awaiting its release.
            EXPECT_EQ(L.masking.count(los(2, "G14")), 1u);
            EXPECT_EQ(C.corners_released, 0);
        }
        if (e == 14) {
            EXPECT_EQ(S.n_reentry, 1);
            EXPECT_EQ(text, ", suspect mask 0 DD over 0 corner(s) [none], released 1 corner(s) "
                            "(1 DD back: 0 fixed, 0 WL-only pair(s))");
        }
    }
    EXPECT_EQ(C.epochs, 5);
    EXPECT_EQ(C.epochs_masked, 3);
    EXPECT_EQ(C.dd_masked, 3);
    EXPECT_EQ(C.corners, 3);
    EXPECT_EQ(C.corners_by[4], 3);
    EXPECT_EQ(C.events, 1);
    EXPECT_EQ(C.events_met, 1);   // one event, met three times
    EXPECT_EQ(C.corners_released, 1);
    EXPECT_EQ(C.epochs_released, 1);
    EXPECT_EQ(C.dd_reentered, 1);
    EXPECT_TRUE(L.masking.empty());
}

TEST(NetworkArSuspectRelease, CloseSearchTalliesTheWorstEpochAndTheReentryOutcome) {
    ArSuspectLedger L;
    ArSuspectCounters C;
    arSuspectNote(L, los(0, "G01"), 4, kArSuspectGfJump, &C);
    L.masking.insert(los(1, "G02"));   // masked at an earlier search
    ArSuspectSearch S = arSuspectOpenSearch(4, 10);
    const ArSuspectCornerState a = arSuspectPoolCorner(S, L, 0, "G01");
    const ArSuspectCornerState b = arSuspectPoolCorner(S, L, 1, "G02");
    EXPECT_EQ(a.bits, kArSuspectGfJump);
    EXPECT_TRUE(b.released);
    arSuspectMaskBasis(S, {a, a, b, b});
    ArSuspectReentry re;
    re.dd = 2; re.dd_fixed = 1; re.pairs = 1; re.pairs_wl_only = 1;
    const std::string text = arSuspectCloseSearch(S, L, C, re, 261930.0);
    EXPECT_EQ(text, ", suspect mask 2 DD over 1 corner(s) [gf_jump 1], released 1 corner(s) "
                    "(2 DD back: 1 fixed, 1 WL-only pair(s))");
    EXPECT_EQ(C.max_dd_masked, 2);
    EXPECT_DOUBLE_EQ(C.max_dd_masked_tow, 261930.0);
    EXPECT_EQ(C.dd_total, 4);
    EXPECT_EQ(C.dd_masked_by[5], 2);
    EXPECT_EQ(C.dd_reentered_fixed, 1);
    EXPECT_EQ(C.pairs_reentered_wl_only, 1);
    EXPECT_EQ(L.masking, (std::set<LosId>{los(0, "G01")}));
}

// ------------------------------------------------------------------ re-entry

TEST(NetworkArSuspectReentry, PairLanesAgreeWithThePairColumns) {
    constexpr double kC = 299792458.0;
    const std::vector<netar::DdPairKey> keys = {
        {"a", kC / 1575.42e6}, {"a", kC / 1227.60e6}, {"b", kC / 1575.42e6}};
    Eigen::MatrixXd Q(3, 3);
    Q << 0.04, 0.03, 0.0,
         0.03, 0.05, 0.0,
         0.0,  0.0,  0.02;
    Eigen::VectorXd a_cond(3);
    a_cond << 3.02, 1.01, 7.4;
    const Eigen::MatrixXd Q_cond = 0.01 * Q;
    const std::vector<int> order{0, 1, 2};
    const auto lanes = netar::pairLanes(keys, order, 3, Q, a_cond, Q_cond);
    ASSERT_EQ(lanes.size(), 3u);
    EXPECT_EQ(lanes[0].partner, 1);
    EXPECT_EQ(lanes[1].partner, 0);
    EXPECT_EQ(lanes[2].partner, -1);   // no partner
    EXPECT_TRUE(lanes[0].have_cond);
    EXPECT_NEAR(lanes[0].wl_cond, 2.01, 1e-12);
    EXPECT_NEAR(lanes[0].wl_float_sigma, std::sqrt(0.04 + 0.05 - 0.06), 1e-12);
    EXPECT_EQ(netar::pairLaneText(lanes),
              netar::pairLaneColumns(keys, order, 3, Q, a_cond, Q_cond));
    EXPECT_EQ(netar::pairLaneText(lanes)[2], ",,,,");
    // Nothing conditioned: the float sigma only.
    const auto bare = netar::pairLaneColumns(keys, order, 3, Q, Eigen::VectorXd(), Eigen::MatrixXd());
    EXPECT_EQ(bare[0].substr(bare[0].size() - 3), ",,,");
}

TEST(NetworkArSuspectReentry, OutcomeCountsFixesAndWideLanesOfTheReenteredDdsOnly) {
    // Basis: pair (0,1) re-entered and both fixed; pair (2,3) re-entered with the wide lane
    // determined; pair (4,5) re-entered, wide lane loose; DD 6 re-entered without a partner;
    // pair (7,8) fixed but not re-entered.
    std::vector<char> reentry{1, 1, 1, 1, 1, 1, 1, 0, 0};
    std::vector<netar::DdFixLabel> labels(9);
    for (int k : {0, 1, 7, 8}) { labels[static_cast<std::size_t>(k)].in_subset = true; labels[static_cast<std::size_t>(k)].kept = true; }
    labels[2].kept = true;   // one single of the WL pair
    std::vector<netar::PairLane> lanes(9);
    auto pair = [&](int a, int b, double wl_sig) {
        for (int x : {a, b}) {
            auto& p = lanes[static_cast<std::size_t>(x)];
            p.partner = (x == a) ? b : a;
            p.have_cond = true;
            p.wl_cond_sigma = wl_sig;
        }
    };
    pair(0, 1, 0.001);
    pair(2, 3, 0.01);
    pair(4, 5, 0.3);
    pair(7, 8, 0.001);
    const ArSuspectReentry r = netar::reentryOutcome(reentry, labels, lanes, 0.05);
    EXPECT_EQ(r.dd, 7);
    EXPECT_EQ(r.dd_fixed, 3);
    EXPECT_EQ(r.pairs, 3);
    EXPECT_EQ(r.pairs_singles, 1);
    EXPECT_EQ(r.pairs_wl_only, 1);
    // Without lanes (decorrelated_partial off): DD counts only.
    const ArSuspectReentry r0 = netar::reentryOutcome(reentry, labels, {}, 0.05);
    EXPECT_EQ(r0.dd, 7);
    EXPECT_EQ(r0.pairs, 0);
    // Nothing re-entered (the mask off): all zero.
    const ArSuspectReentry z = netar::reentryOutcome({}, labels, lanes, 0.05);
    EXPECT_EQ(z.dd, 0);
}

// ------------------------------------------------------------------ config

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

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg,
                LogManager& log) {
    const fs::path p = fs::temp_directory_path() / ("ar_suspect_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

TEST(NetworkArSuspectConfig, DefaultsOff) {
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", arConfig("\"enable\": true"), cfg, log));
    EXPECT_EQ(cfg.getNetworkConfig().ambiguity_resolution.suspect_epochs, 0);
    const auto k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    EXPECT_EQ(k.ar_suspect_epochs, 0);
}

TEST(NetworkArSuspectConfig, ParsesTheWindowAndResolvesItAgainstEnable) {
    {
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(loadConfig("armed", arConfig("\"enable\": true, \"suspect_epochs\": 10"),
                               cfg, log));
        EXPECT_EQ(cfg.getNetworkConfig().ambiguity_resolution.suspect_epochs, 10);
        EXPECT_EQ(NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig()).ar_suspect_epochs, 10);
    }
    {
        // Without the search the mask has nothing to mask: the snapshot resolves to off.
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(loadConfig("noenable", arConfig("\"suspect_epochs\": 10"), cfg, log));
        EXPECT_EQ(cfg.getNetworkConfig().ambiguity_resolution.suspect_epochs, 10);
        EXPECT_EQ(NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig()).ar_suspect_epochs, 0);
    }
    {
        // A negative window is named and replaced by off.
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(loadConfig("negative",
                               arConfig("\"enable\": true, \"suspect_epochs\": -4"), cfg, log));
        EXPECT_EQ(cfg.getNetworkConfig().ambiguity_resolution.suspect_epochs, 0);
    }
}

}  // namespace
}  // namespace GnssProcessor
