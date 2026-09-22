/**
 * @file test_signal_roster.cpp
 * @brief Card SID phase 1: the per-station signal roster
 *        (docs/refound/SID_signal_identity_2026-09-22.md section 5, P5).
 *
 * The roster's first obligation is that it REPRODUCES the rules that already
 * decide which signal stands for a band, rather than re-deriving them: phase 1's
 * armed byte-identity protects the solve, but only these tests protect the ROLE
 * assignment that phases 2 to 6 will read.  Pinned here, over synthetic header
 * contents with no station, no model and no solve:
 *
 *  - the Modelled role is the LATCH's own row, under the priority rule and under
 *    the count rule alike (the roster never re-runs the decision, so the two cases
 *    are the same code path fed two different latches, which is exactly the
 *    property that makes a latch change invisible to the roster);
 *  - a card SCC companion carries the Companion role and is NOT an exclusion;
 *  - every other tracked signal is an exclusion WITH a reason, and the reason
 *    separates the allow list from a band the latch decided against from a band
 *    the latch does not carry at all;
 *  - a band the model does not carry is Tracked only;
 *  - the arc-level tracker's own pick, reproduced, so the disagreement the card is
 *    named for is stated in one place instead of inferred.
 */

#include <gtest/gtest.h>

#include "app/SignalRoster.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

/// A station tracking the civil and the semi-codeless band-1 channels plus L2.
const std::vector<std::string> kTrackedL1cL1w{
    "C1C", "L1C", "C1W", "L1W", "C2W", "L2W"
};

std::vector<RosterLatchRow> latch(std::initializer_list<std::pair<char, const char*>> rows) {
    std::vector<RosterLatchRow> out;
    for (const auto& [t, s] : rows) out.push_back(RosterLatchRow{ t, std::string(s) });
    return out;
}

bool hasExclusion(const BandRoster& br, const std::string& sig, const std::string& reason) {
    return std::any_of(br.excluded.begin(), br.excluded.end(),
                       [&](const std::pair<std::string, const char*>& e) {
                           return e.first == sig && reason == e.second;
                       });
}

}  // namespace

// ---------------------------------------------------------------------------
// The Modelled role reproduces the latch
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, ModelledRoleIsTheLatchUnderThePriorityRule) {
    // The priority list of the RF-0hv postures is W before C, so a station that
    // tracks both latches L1W / C1W.  The roster must report exactly that and must
    // NOT fall back to the lexically first channel, which is what the arc-level
    // tracker does and what the card is about.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{}, /*companions_armed=*/false, /*allowlist=*/nullptr);

    ASSERT_EQ(cells.count('1'), 1u);
    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.modelled_phase, "L1W");
    EXPECT_EQ(b1.modelled_code, "C1W");
    EXPECT_EQ(b1.tracked_phase, (std::vector<std::string>{ "L1C", "L1W" }));
    EXPECT_EQ(b1.tracked_code, (std::vector<std::string>{ "C1C", "C1W" }));
    EXPECT_TRUE(b1.modelled_extra.empty()) << "phase 6 only";
    // The channels the latch decided against are exclusions with the reason that
    // says so, not silent omissions.
    EXPECT_TRUE(hasExclusion(b1, "L1C", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "C1C", "not_latched"));
    EXPECT_EQ(b1.excluded.size(), 2u);

    const BandRoster& b2 = cells.at('2');
    EXPECT_EQ(b2.modelled_phase, "L2W");
    EXPECT_EQ(b2.modelled_code, "C2W");
    EXPECT_TRUE(b2.excluded.empty());
}

TEST(SignalRosterTest, ModelledRoleIsTheLatchUnderTheCountRule) {
    // The count rule at the same station latches the channel with the most
    // satellites, which on the six stations card CSL names is L1C.  The roster is
    // the same code path: it reads the latch it is given, so the rule that produced
    // it is invisible here.  That is the property, not an accident of the fixture.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1C" }, { 'C', "C1C" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{}, /*companions_armed=*/false, /*allowlist=*/nullptr);

    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.modelled_phase, "L1C");
    EXPECT_EQ(b1.modelled_code, "C1C");
    EXPECT_TRUE(hasExclusion(b1, "L1W", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "C1W", "not_latched"));
}

TEST(SignalRosterTest, ALatchedBandTheReceiverDoesNotReportStillHasItsCell) {
    // The latch is the estimator's KEY, so a reader asking what this station
    // carries on band 2 needs an answer on the epochs the arc is absent too.  That
    // is the same reason the arc-level tracker reads the latch and not the epoch's
    // admitted row set.
    const auto cells = signalRosterBuildCells(
        { "C1W", "L1W" }, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                  { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, nullptr);
    ASSERT_EQ(cells.count('2'), 1u);
    const BandRoster& b2 = cells.at('2');
    EXPECT_EQ(b2.modelled_phase, "L2W");
    EXPECT_TRUE(b2.tracked_phase.empty());
    EXPECT_TRUE(b2.excluded.empty());
}

// ---------------------------------------------------------------------------
// Bands the model does not carry
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, ABandTheModelDoesNotCarryIsTrackedOnly) {
    // Band 5 on a dual posture: the receiver reports it, the latch does not name
    // it, and the roster must say TRACKED with no modelled role rather than
    // inventing one.  Its exclusions carry `no_latch`, which is a different
    // statement from `not_latched` and has to stay separable.
    const auto cells = signalRosterBuildCells(
        { "C1W", "L1W", "C5Q", "L5Q", "C5X", "L5X" },
        latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, nullptr);

    ASSERT_EQ(cells.count('5'), 1u);
    const BandRoster& b5 = cells.at('5');
    EXPECT_TRUE(b5.modelled_phase.empty());
    EXPECT_TRUE(b5.modelled_code.empty());
    EXPECT_EQ(b5.tracked_phase, (std::vector<std::string>{ "L5Q", "L5X" }));
    EXPECT_EQ(b5.excluded.size(), 4u);
    for (const char* s : { "L5Q", "L5X", "C5Q", "C5X" })
        EXPECT_TRUE(hasExclusion(b5, s, "no_latch")) << s;
}

// ---------------------------------------------------------------------------
// Card SCC companions
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, CompanionCodesCarryTheCompanionRoleAndAreNotExclusions) {
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        /*companion_allow=*/{ "C1C", "C1X" }, /*companions_armed=*/true, nullptr);

    const BandRoster& b1 = cells.at('1');
    EXPECT_EQ(b1.companion_code, (std::vector<std::string>{ "C1C" }));
    // C1X is on the allow list but untracked here, so it is neither a companion nor
    // an exclusion: the roster reports what exists, not what a list mentions.
    EXPECT_FALSE(hasExclusion(b1, "C1X", "not_latched"));
    // The companion is NOT also an exclusion, and the phase it shares its attribute
    // with still is: the code side has a second channel, the phase side does not,
    // which is the whole asymmetry the audit measured.
    EXPECT_FALSE(hasExclusion(b1, "C1C", "not_latched"));
    EXPECT_TRUE(hasExclusion(b1, "L1C", "not_latched"));
    EXPECT_EQ(b1.excluded.size(), 1u);
}

TEST(SignalRosterTest, CompanionsNeedTheCanonicalPresentAndStayOnBandOne) {
    // The canonical must be tracked this epoch, which is the rule
    // NetworkSecondCodeScope.cpp and PppModel implement.  Without it the second
    // channel would ride alone and its inter-signal state would have nothing to be
    // measured against.
    const auto no_canon = signalRosterBuildCells(
        { "C1C", "L1C", "L1W" }, latch({ { 'L', "L1W" }, { 'C', "C1W" } }),
        { "C1C" }, true, nullptr);
    EXPECT_TRUE(no_canon.at('1').companion_code.empty());

    // Band 2 has no companion mechanism at all (card SCC is band 1 only).
    const auto b2 = signalRosterBuildCells(
        { "C2W", "L2W", "C2L" }, latch({ { 'L', "L2W" }, { 'C', "C2W" } }),
        { "C2L" }, true, nullptr);
    EXPECT_TRUE(b2.at('2').companion_code.empty());
    EXPECT_TRUE(hasExclusion(b2.at('2'), "C2L", "not_latched"));
}

// ---------------------------------------------------------------------------
// The allow list
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, TheAllowListIsAPerStationExclusionReason) {
    // network.signal_min_station_fraction is a GLOBAL whole-run gate today, so a
    // dropped channel is invisible per station.  As a roster reason it becomes
    // visible where it bites, which is what phase 1 has to demonstrate.
    const std::set<std::string> allow{ "C1W", "L1W", "C2W", "L2W" };
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" },
                                { 'L', "L2W" }, { 'C', "C2W" } }),
        {}, false, &allow);

    const BandRoster& b1 = cells.at('1');
    EXPECT_TRUE(hasExclusion(b1, "L1C", "allowlist"));
    EXPECT_TRUE(hasExclusion(b1, "C1C", "allowlist"));
    // The allow list is the EARLIER gate, so it names the exclusion rather than the
    // latch: the two reasons must not be interchangeable.
    EXPECT_FALSE(hasExclusion(b1, "L1C", "not_latched"));
}

TEST(SignalRosterTest, AnEmptyAllowListAdmitsEverything) {
    const std::set<std::string> empty;
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, &empty);
    EXPECT_TRUE(hasExclusion(cells.at('1'), "L1C", "not_latched"));
}

// ---------------------------------------------------------------------------
// The tracker's own pick, stated in one place
// ---------------------------------------------------------------------------

TEST(SignalRosterTest, TrackerPicksTheLexicallyLowestPhaseWithAMatchingCode) {
    // NetworkRawObservables.cpp keeps a phase only when its matching code is
    // reported, then sorts on the BAND DIGIT ALONE and takes the front.  On this
    // toolchain that is lexical order within a band, which is why a station
    // latching L1W is tracked as L1C: 46 station-band cells on the field baseline.
    const auto picks = signalRosterTrackerPicks(kTrackedL1cL1w);
    EXPECT_EQ(picks.at('1'), "L1C");
    EXPECT_EQ(picks.at('2'), "L2W");

    // A phase with no matching code is not a candidate at all.
    const auto no_code = signalRosterTrackerPicks({ "L1C", "C1W", "L1W" });
    EXPECT_EQ(no_code.at('1'), "L1W");

    // And the disagreement the card is named for, in one line.
    const auto cells = signalRosterBuildCells(
        kTrackedL1cL1w, latch({ { 'L', "L1W" }, { 'C', "C1W" } }), {}, false, nullptr);
    EXPECT_NE(picks.at('1'), cells.at('1').modelled_phase);
}

TEST(SignalRosterTest, RoleNamesAreStable) {
    // The trace and the run-end table are read by name offline.
    EXPECT_STREQ(signalRoleName(SignalRole::Tracked), "tracked");
    EXPECT_STREQ(signalRoleName(SignalRole::Modelled), "modelled");
    EXPECT_STREQ(signalRoleName(SignalRole::Datum), "datum");
    EXPECT_STREQ(signalRoleName(SignalRole::Companion), "companion");
    EXPECT_STREQ(signalRoleName(SignalRole::Excluded), "excluded");
}

}  // namespace GnssProcessor
