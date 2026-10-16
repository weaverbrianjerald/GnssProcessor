/**
 * @file test_canonical_signal_code_follow.cpp
 * @brief Contract tests for `processing.canonical_signal_code_follows_phase`
 *        (the code half of the priority rule).
 *
 * Under @c CanonicalSignalRule::Priority the CSL-1 rule moves each band's code
 * pick onto the tracking attribute its phase latched.  On the multi-site census,
 * 16 roster stations track L1C phase with C1W code; the follow moves their code
 * to C1C, which thins the wire's C1W code biases (median 30 satellites per epoch
 * to 24) and produces code-bias dropouts for consumers on C1W code away from the
 * European cluster (MSC section 4).  With the knob false, the code is decided on
 * its own priority list instead.
 *
 * Covered here:
 *   (a) true, the default, reproduces the CSL-1 follow exactly, so an absent key
 *       changes nothing;
 *   (b) false leaves the code on the pick the priority rule made from the code's
 *       own list, and the phase latch is the same under either value;
 *   (c) the census case: a station tracking L1C phase with C1W code keeps C1W;
 *   (d) the code's own decision is still the priority rule: its coverage floor
 *       and its runner-up are the code list's, not the phase's;
 *   (e) under @c Count the flag is unread (that rule has no follow clause), and
 *       a station whose two lists agree latches identically under either value.
 *
 * The rule is a pure function (no config, logger or observations), so these
 * cases need no fixture.  In a run, the "Canonical latch for ..." log line shows
 * the rule applied and names the setting only when it is false.
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
using Picks = std::map<Key, PppModel::CanonicalPick>;

/// GPS band-1 candidates in the project's default priority order (C1W, C1C,
/// then L1W, L1C); a negative count means the station does not offer that
/// signal at all.
std::vector<Cand> band1(int c1w, int c1c, int l1w, int l1c) {
    std::vector<Cand> v;
    if (c1w >= 0) v.push_back(Cand{ "C1W", "L1", 'C', c1w });
    if (c1c >= 0) v.push_back(Cand{ "C1C", "L1", 'C', c1c });
    if (l1w >= 0) v.push_back(Cand{ "L1W", "L1", 'L', l1w });
    if (l1c >= 0) v.push_back(Cand{ "L1C", "L1", 'L', l1c });
    return v;
}

std::string latched(const Picks& p, const std::string& band, char type) {
    const auto it = p.find(Key{ band, type });
    return it == p.end() ? std::string{} : it->second.signal;
}

Picks decide(const std::vector<Cand>& v, CanonicalSignalRule rule,
             double cov, bool follow) {
    return PppModel::decideCanonicalPicks(v, rule, cov, follow);
}

}  // namespace

// --------------------------------------------------------------------------
// (a) / (b) the clause, on and off
// --------------------------------------------------------------------------

TEST(CanonicalSignalCodeFollow, DefaultTrueReproducesTheFollow) {
    // The CSL-1 case: the code would be C1C on its own list, the phase latches
    // L1W, so the code is dragged to C1W and the displaced pick is reported.
    const std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 9 },
                               Cand{ "L1W", "L1", 'L', 9 },  Cand{ "L1C", "L1", 'L', 10 } };
    const auto on = decide(v, CanonicalSignalRule::Priority, 0.5, true);
    EXPECT_EQ(latched(on, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(on, "L1", 'C'), "C1W");
    EXPECT_EQ(on.at(Key{ "L1", 'C' }).count, 9);
    EXPECT_EQ(on.at(Key{ "L1", 'C' }).runner_up, "C1C");
    // The three-argument form gives the same decision: the default is the follow.
    const auto dflt = PppModel::decideCanonicalPicks(v, CanonicalSignalRule::Priority, 0.5);
    EXPECT_EQ(latched(dflt, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(dflt, "L1", 'L'), "L1W");
}

TEST(CanonicalSignalCodeFollow, FalseKeepsTheCodeOnItsOwnPriorityPick) {
    const std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 9 },
                               Cand{ "L1W", "L1", 'L', 9 },  Cand{ "L1C", "L1", 'L', 10 } };
    const auto off = decide(v, CanonicalSignalRule::Priority, 0.5, false);
    // The code list leads with C1C here, and C1C clears the floor, so it is the
    // pick; the phase's attribute is not consulted.
    EXPECT_EQ(latched(off, "L1", 'C'), "C1C");
    EXPECT_EQ(off.at(Key{ "L1", 'C' }).count, 10);
    // The runner-up is the code list's own next eligible entry, not a displaced
    // pick left behind by the follow.
    EXPECT_EQ(off.at(Key{ "L1", 'C' }).runner_up, "C1W");
    EXPECT_EQ(off.at(Key{ "L1", 'C' }).runner_up_count, 9);
}

TEST(CanonicalSignalCodeFollow, ThePhaseLatchIsIdenticalUnderBothValues) {
    // The knob acts on the code half only: the band-1 phase attribute (the lever
    // card CSL graded) does not move with it.
    for (const auto& v : { band1(9, 10, 9, 10), band1(2, 10, 2, 10),
                           band1(-1, 10, 10, 10), band1(10, 10, -1, 10) }) {
        const auto on  = decide(v, CanonicalSignalRule::Priority, 0.5, true);
        const auto off = decide(v, CanonicalSignalRule::Priority, 0.5, false);
        EXPECT_EQ(latched(on, "L1", 'L'), latched(off, "L1", 'L'));
        EXPECT_EQ(on.at(Key{ "L1", 'L' }).count, off.at(Key{ "L1", 'L' }).count);
        EXPECT_EQ(on.at(Key{ "L1", 'L' }).runner_up, off.at(Key{ "L1", 'L' }).runner_up);
    }
}

// --------------------------------------------------------------------------
// (c) the census case: L1C phase with C1W code
// --------------------------------------------------------------------------

TEST(CanonicalSignalCodeFollow, FalseKeepsC1WAtAStationTrackingL1CPhase) {
    // The 16 stations of MSC section 4: the receiver offers no L1W phase at all,
    // so the phase latches L1C; the code offers both and leads with C1W.
    const auto v = band1(10, 10, -1, 10);
    const auto on  = decide(v, CanonicalSignalRule::Priority, 0.5, true);
    const auto off = decide(v, CanonicalSignalRule::Priority, 0.5, false);
    EXPECT_EQ(latched(on,  "L1", 'L'), "L1C");
    EXPECT_EQ(latched(off, "L1", 'L'), "L1C");
    EXPECT_EQ(latched(on,  "L1", 'C'), "C1C");   // the follow thins the wire's C1W
    EXPECT_EQ(latched(off, "L1", 'C'), "C1W");   // the knob keeps it
}

// --------------------------------------------------------------------------
// (d) the code's own decision is the priority rule
// --------------------------------------------------------------------------

TEST(CanonicalSignalCodeFollow, TheCodeStillObeysItsOwnCoverageFloor) {
    // C1W on two satellites of ten is below the floor, so the code falls through
    // to C1C even with the follow off: the knob removes the phase's influence,
    // not the rule.
    const auto off = decide(band1(2, 10, 10, 10), CanonicalSignalRule::Priority, 0.5, false);
    EXPECT_EQ(latched(off, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(off, "L1", 'L'), "L1W");
    // Exactly at the floor it is admitted, as for the phase.
    const auto at = decide(band1(5, 10, 10, 10), CanonicalSignalRule::Priority, 0.5, false);
    EXPECT_EQ(latched(at, "L1", 'C'), "C1W");
}

TEST(CanonicalSignalCodeFollow, EachBandDecidesItsCodeIndependently) {
    // Band 1 keeps C1W with the follow off; band 2 offers one attribute only and
    // is unaffected either way.
    const std::vector<Cand> v{ Cand{ "C1W", "L1", 'C', 10 }, Cand{ "C1C", "L1", 'C', 10 },
                               Cand{ "C2W", "L2", 'C', 10 },
                               Cand{ "L1C", "L1", 'L', 10 },
                               Cand{ "L2W", "L2", 'L', 10 } };
    const auto off = decide(v, CanonicalSignalRule::Priority, 0.5, false);
    EXPECT_EQ(latched(off, "L1", 'C'), "C1W");
    EXPECT_EQ(latched(off, "L1", 'L'), "L1C");
    EXPECT_EQ(latched(off, "L2", 'C'), "C2W");
    EXPECT_EQ(latched(off, "L2", 'L'), "L2W");
    EXPECT_EQ(off.size(), 4u);
}

// --------------------------------------------------------------------------
// (e) where the knob cannot act
// --------------------------------------------------------------------------

TEST(CanonicalSignalCodeFollow, CountIgnoresTheFlagEntirely) {
    // The follow belongs to `priority`; the older `count` rule has no follow, so
    // both values give the same map.
    const std::vector<Cand> v{ Cand{ "C1C", "L1", 'C', 10 }, Cand{ "C1W", "L1", 'C', 9 },
                               Cand{ "L1W", "L1", 'L', 10 }, Cand{ "L1C", "L1", 'L', 9 } };
    const auto on  = decide(v, CanonicalSignalRule::Count, 0.5, true);
    const auto off = decide(v, CanonicalSignalRule::Count, 0.5, false);
    EXPECT_EQ(latched(on, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(off, "L1", 'C'), "C1C");
    EXPECT_EQ(latched(on, "L1", 'L'), "L1W");
    EXPECT_EQ(latched(off, "L1", 'L'), "L1W");
    EXPECT_EQ(on.size(), off.size());
}

TEST(CanonicalSignalCodeFollow, AgreeingListsLatchIdenticallyUnderBothValues) {
    // The station the knob cannot move: its code and phase lists already point at
    // the same attribute, so the follow has nothing to do.
    for (const auto& v : { band1(10, 10, 10, 10), band1(-1, 10, -1, 10) }) {
        const auto on  = decide(v, CanonicalSignalRule::Priority, 0.5, true);
        const auto off = decide(v, CanonicalSignalRule::Priority, 0.5, false);
        EXPECT_EQ(latched(on, "L1", 'C'), latched(off, "L1", 'C'));
        EXPECT_EQ(on.at(Key{ "L1", 'C' }).runner_up, off.at(Key{ "L1", 'C' }).runner_up);
    }
}

TEST(CanonicalSignalCodeFollow, EmptyCandidateSetDecidesNothingUnderEitherValue) {
    EXPECT_TRUE(decide({}, CanonicalSignalRule::Priority, 0.5, true).empty());
    EXPECT_TRUE(decide({}, CanonicalSignalRule::Priority, 0.5, false).empty());
}
