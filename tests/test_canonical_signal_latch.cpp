/**
 * @file test_canonical_signal_latch.cpp
 * @brief Contract tests for the canonical-signal decision rule (
 *        docs/refound/CSL_canonical_signal_latch_2026-09-20.md).
 *
 * @c PppModel::decideCanonicalPicks is the pure rule behind each station's
 * per-(band, obs-type) latch.  It is decided once, at the station's first
 * processed epoch, and held for the run, so one satellite missing the
 * semi-codeless pair at that epoch moves the whole run onto the civil signal
 * under the historical rule.
 *
 * What is pinned here:
 *   (a) @c CanonicalSignalRule::Count (the default) reproduces the historical
 *       pick: highest satellite count wins, a tie goes to the earlier entry of
 *       the priority list, and a one-satellite deficit loses;
 *   (b) @c CanonicalSignalRule::Priority takes the priority-first signal at full
 *       coverage, falls through when that signal is below the coverage floor,
 *       and clamps a floor outside [0, 1];
 *   (c) under @c Priority the code follows the phase's tracking attribute when
 *       such a code exists at the station and clears the same floor, and keeps
 *       its own pick when it does not;
 *   (d) a band offering one signal is latched identically by both rules;
 *   (e) the runner-up reported alongside the latch is what the same rule would
 *       have chosen without the winner.
 *
 * The rule is pure (no config, no logger, no observations), so these cases carry
 * no fixture.  Whether a run applies it per station is checked on the run's own
 * "Canonical latch for ..." log line in the arms, not here.
 */
#include <gtest/gtest.h>

#include "model/PppModel.hpp"

#include <string>
#include <vector>

using GnssProcessor::CanonicalSignalRule;
using GnssProcessor::PppModel;

namespace {

using Cand = PppModel::CanonicalCandidate;
using Key  = std::pair<std::string, char>;

/// GPS band-1 candidates in the project's default priority order
/// (C1W, C1C, ... then L1W, L1C, ...), with the counts the caller measured.
std::vector<Cand> band1(int c1w, int c1c, int l1w, int l1c) {
    std::vector<Cand> v;
    if (c1w >= 0) v.push_back(Cand{ "C1W", "L1", 'C', c1w });
    if (c1c >= 0) v.push_back(Cand{ "C1C", "L1", 'C', c1c });
    if (l1w >= 0) v.push_back(Cand{ "L1W", "L1", 'L', l1w });
    if (l1c >= 0) v.push_back(Cand{ "L1C", "L1", 'L', l1c });
    return v;
}

std::string latched(const std::map<Key, PppModel::CanonicalPick>& p,
                    const std::string& band, char type) {
    const auto it = p.find(Key{ band, type });
    return it == p.end() ? std::string{} : it->second.signal;
}

}  // namespace

// --------------------------------------------------------------------------
// (a) the historical rule
// --------------------------------------------------------------------------

TEST(CanonicalSignalLatchRule, CountBreaksATieByPriorityOrder) {
    // Ten satellites carry both attributes: the counts tie, so the earlier
    // priority-list entry wins (the historical tie-break).
    const auto p = PppModel::decideCanonicalPicks(band1(10, 10, 10, 10),
                                                  CanonicalSignalRule::Count, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    // The runner-up is the alternative the same rule would have taken.
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up, "L1C");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up_count, 10);
}

TEST(CanonicalSignalLatchRule, CountLosesTheSemiCodelessPairOnOneSatellite) {
    // The defect the card names: nine of ten satellites carry L1W/C1W and the
    // tenth does not, so the whole run latches the civil signal.
    const auto p = PppModel::decideCanonicalPicks(band1(9, 10, 9, 10),
                                                  CanonicalSignalRule::Count, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1C");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).count, 10);
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up, "L1W");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up_count, 9);
}

// --------------------------------------------------------------------------
// (b) the priority rule and its coverage floor
// --------------------------------------------------------------------------

TEST(CanonicalSignalLatchRule, PriorityTakesTheFirstListedSignalAtFullCoverage) {
    const auto p = PppModel::decideCanonicalPicks(band1(10, 10, 10, 10),
                                                  CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
}

TEST(CanonicalSignalLatchRule, PriorityKeepsTheSemiCodelessPairOnOneSatelliteDeficit) {
    // The same input the historical rule sends to the civil family.
    const auto p = PppModel::decideCanonicalPicks(band1(9, 10, 9, 10),
                                                  CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).count, 9);
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up, "L1C");
}

TEST(CanonicalSignalLatchRule, PriorityFallsThroughBelowTheCoverageFloor) {
    // Two of ten satellites carry the priority-first signal: 2 < 0.5 * 10, so
    // the latch falls through to the next listed signal that clears the floor.
    const auto p = PppModel::decideCanonicalPicks(band1(2, 10, 2, 10),
                                                  CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1C");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up, "L1W");
    EXPECT_EQ(p.at(Key{ "L1", 'L' }).runner_up_count, 2);
}

TEST(CanonicalSignalLatchRule, PriorityFloorIsExactlyInclusive) {
    // Five of ten is the floor itself at 0.5 and is admitted; four is not.
    const auto on  = PppModel::decideCanonicalPicks(band1(5, 10, 5, 10),
                                                    CanonicalSignalRule::Priority, 0.5);
    const auto off = PppModel::decideCanonicalPicks(band1(4, 10, 4, 10),
                                                    CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(on,  "L1", 'L'), "L1W");
    EXPECT_EQ(latched(off, "L1", 'L'), "L1C");
}

TEST(CanonicalSignalLatchRule, PriorityFloorIsClampedToTheUnitInterval) {
    // Above 1 clamps to 1 (only a maximum-count signal is eligible, so a
    // one-satellite deficit loses); below 0 clamps to 0 (any tracked signal is).
    const auto hi = PppModel::decideCanonicalPicks(band1(9, 10, 9, 10),
                                                   CanonicalSignalRule::Priority, 5.0);
    const auto lo = PppModel::decideCanonicalPicks(band1(1, 10, 1, 10),
                                                   CanonicalSignalRule::Priority, -1.0);
    EXPECT_EQ(latched(hi, "L1", 'L'), "L1C");
    EXPECT_EQ(latched(lo, "L1", 'L'), "L1W");
}

// --------------------------------------------------------------------------
// (c) the code follows the phase's tracking attribute
// --------------------------------------------------------------------------

TEST(CanonicalSignalLatchRule, PriorityCodeFollowsThePhaseAttribute) {
    // A priority list whose code order disagrees with its phase order: the code
    // would be C1C on its own, but the phase latches L1W, so the code follows.
    std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 9 },
                         Cand{ "L1W", "L1", 'L', 9 },  Cand{ "L1C", "L1", 'L', 10 } };
    const auto p = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(p, "L1", 'C'), "C1W");
    EXPECT_EQ(p.at(Key{ "L1", 'C' }).count, 9);
    // The displaced pick is reported as the runner-up.
    EXPECT_EQ(p.at(Key{ "L1", 'C' }).runner_up, "C1C");
    EXPECT_EQ(p.at(Key{ "L1", 'C' }).runner_up_count, 10);
}

TEST(CanonicalSignalLatchRule, PriorityCodeKeepsItsPickWhenTheAttributeIsAbsent) {
    // The station tracks L1W but offers no C1W at all: the code keeps C1C.
    const auto p = PppModel::decideCanonicalPicks(band1(-1, 10, 10, 10),
                                                  CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(p, "L1", 'C'), "C1C");
    EXPECT_TRUE(p.at(Key{ "L1", 'C' }).runner_up.empty());
}

TEST(CanonicalSignalLatchRule, PriorityCodeDoesNotFollowBelowTheFloor) {
    // C1W exists but on one satellite of ten, below the floor: the code stays on
    // the civil signal even though the phase latched L1W.
    std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 1 },
                         Cand{ "L1W", "L1", 'L', 10 }, Cand{ "L1C", "L1", 'L', 10 } };
    const auto p = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(p, "L1", 'C'), "C1C");
}

TEST(CanonicalSignalLatchRule, CountNeverFollowsThePhaseAttribute) {
    // Only the priority rule follows the phase; the default Count rule does not.
    std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 9 },
                         Cand{ "L1W", "L1", 'L', 10 }, Cand{ "L1C", "L1", 'L', 9 } };
    const auto p = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Count, 0.5);
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(p, "L1", 'C'), "C1C");
}

// --------------------------------------------------------------------------
// (d) / (e) degenerate bands, filtering, independence across bands
// --------------------------------------------------------------------------

TEST(CanonicalSignalLatchRule, OneSignalBandIsLatchedIdenticallyByBothRules) {
    const auto v = band1(-1, 10, -1, 10);
    const auto c = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Count, 0.5);
    const auto q = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(c, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(c, "L1", 'L'), "L1C");
    EXPECT_EQ(latched(q, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(q, "L1", 'L'), "L1C");
    // Nothing else was offered, so there is no runner-up under either rule.
    EXPECT_TRUE(c.at(Key{ "L1", 'L' }).runner_up.empty());
    EXPECT_TRUE(q.at(Key{ "L1", 'L' }).runner_up.empty());
    EXPECT_EQ(c.at(Key{ "L1", 'L' }).runner_up_count, 0);
}

TEST(CanonicalSignalLatchRule, ZeroCountAndMalformedCandidatesAreIgnored) {
    std::vector<Cand> v{ Cand{ "L1W", "L1", 'L', 0 },    // untracked this epoch
                         Cand{ "D1W", "L1", 'D', 10 },   // neither code nor phase
                         Cand{ "L1C", "",   'L', 10 },   // no band resolved
                         Cand{ "L1C", "L1", 'L', 10 } };
    const auto p = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(p.size(), 1u);
    EXPECT_EQ(latched(p, "L1", 'L'), "L1C");
    EXPECT_TRUE(p.at(Key{ "L1", 'L' }).runner_up.empty());
}

TEST(CanonicalSignalLatchRule, BandsAreDecidedIndependently) {
    // Band 1 keeps its semi-codeless pair; band 2 has no W attribute at all.
    std::vector<Cand> v{ Cand{ "C1W", "L1", 'C', 9 },  Cand{ "C1C", "L1", 'C', 10 },
                         Cand{ "C2L", "L2", 'C', 8 },
                         Cand{ "L1W", "L1", 'L', 9 },  Cand{ "L1C", "L1", 'L', 10 },
                         Cand{ "L2L", "L2", 'L', 8 } };
    const auto p = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(p, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(p, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(p, "L2", 'C'), "C2L");
    EXPECT_EQ(latched(p, "L2", 'L'), "L2L");
    EXPECT_EQ(p.size(), 4u);
}

TEST(CanonicalSignalLatchRule, EmptyCandidateSetDecidesNothing) {
    EXPECT_TRUE(PppModel::decideCanonicalPicks({}, CanonicalSignalRule::Count, 0.5).empty());
    EXPECT_TRUE(PppModel::decideCanonicalPicks({}, CanonicalSignalRule::Priority, 0.5).empty());
}
