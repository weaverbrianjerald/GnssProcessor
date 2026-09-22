/**
 * @file test_arc_ledger.cpp
 * @brief Card SID phase 1: the arc lifecycle state machine
 *        (docs/refound/SID_signal_identity_2026-09-22.md section 5, P5).
 *
 * The ledger is the single state machine the 29 arc-keyed containers of the audit
 * can later read, and phase 1's whole job is that it be RIGHT before anything reads
 * it.  The properties pinned here are the ones every later phase depends on:
 *
 *  - the state sequence over a synthetic presence pattern, including that a
 *    re-entry is a state and not an inference;
 *  - the GENERATION counter, which is what tells a returning key from a first
 *    sighting (the distinction the anchor election does not make today);
 *  - the HORIZON, which is a TIME and not an epoch count, so a run at a different
 *    epoch interval means the same thing;
 *  - the FORGET window, after which a record stops being remembered at all;
 *  - the event throttle, which is what keeps a 305-station first epoch from
 *    writing about 12 000 birth rows;
 *  - that an arc whose STATION reported nothing is frozen and never gapped.
 *
 * Everything below drives the class directly: no station, no model, no solve.
 */

#include <gtest/gtest.h>

#include "filter/ArcLedger.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kDt = 30.0;   // the producer's epoch interval on both beds

ArcLedger::Key K(int sta, const char* sat, const char* sig) {
    return ArcLedger::Key{ sta, std::string(sat), std::string(sig) };
}

/// Drive one epoch: open it, observe the listed arcs, close it over @p present.
std::vector<std::pair<ArcLedger::Key, ArcStep>>
runEpoch(ArcLedger& led, long long ep, const std::vector<ArcLedger::Key>& seen,
         const std::set<int>& present, std::vector<ArcStep>* observed = nullptr) {
    led.beginEpoch(ep, static_cast<double>(ep) * kDt);
    for (const auto& k : seen) {
        const ArcStep s = led.observe(k, /*modelled=*/true);
        if (observed) observed->push_back(s);
    }
    std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
    led.endEpoch(present, edges);
    return edges;
}

}  // namespace

// ---------------------------------------------------------------------------
// The state sequence
// ---------------------------------------------------------------------------

TEST(ArcLedgerTest, BirthTrackingGapReentryIsTheFullSequence) {
    ArcLedger led;
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };

    std::vector<ArcStep> obs;
    runEpoch(led, 0, { a }, present, &obs);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].transition, ArcTransition::Birth);
    EXPECT_EQ(obs[0].generation, 1);
    EXPECT_EQ(obs[0].prev_generation, 0);   // a TRUE first sighting
    ASSERT_NE(led.find(a), nullptr);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);

    obs.clear();
    runEpoch(led, 1, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::None);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);

    // Two missed epochs: the gap opens on the first and stays open on the second.
    auto edges = runEpoch(led, 2, {}, present);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].second.transition, ArcTransition::Gap);
    EXPECT_EQ(led.find(a)->state, ArcState::Gap);
    edges = runEpoch(led, 3, {}, present);
    EXPECT_TRUE(edges.empty());             // still gapped: no second edge
    EXPECT_EQ(led.find(a)->state, ArcState::Gap);

    // The return is a RE-ENTRY with the gap length in epochs, not a birth.
    obs.clear();
    runEpoch(led, 4, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::Reentry);
    EXPECT_EQ(obs[0].gap_len_ep, 2);        // epochs 2 and 3 were missed
    EXPECT_EQ(obs[0].generation, 1);        // a re-entry does NOT advance it
    EXPECT_EQ(led.find(a)->state, ArcState::Reentered);

    // Reentered is one epoch wide.
    obs.clear();
    runEpoch(led, 5, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::None);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);
}

TEST(ArcLedgerTest, HorizonIsATimeAndKillsTheArcOnce) {
    ArcLedger led;
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };
    runEpoch(led, 0, { a }, present);

    // 900 s at 30 s per epoch is 30 epochs.  The gap opens at epoch 1 and the arc
    // is still alive at the boundary; it dies on the first epoch PAST it.
    for (long long ep = 1; ep <= 30; ++ep) {
        const auto edges = runEpoch(led, ep, {}, present);
        if (ep == 1) {
            ASSERT_EQ(edges.size(), 1u);
            EXPECT_EQ(edges[0].second.transition, ArcTransition::Gap);
        } else {
            EXPECT_TRUE(edges.empty()) << "unexpected edge at epoch " << ep;
        }
        EXPECT_EQ(led.find(a)->state, ArcState::Gap) << "at epoch " << ep;
    }
    const auto edges = runEpoch(led, 31, {}, present);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].second.transition, ArcTransition::Death);
    EXPECT_EQ(edges[0].second.reason_mask & kArcReasonHorizon, kArcReasonHorizon);
    EXPECT_EQ(led.find(a)->state, ArcState::Dead);
    // A death is reported ONCE: the state is terminal until the arc returns.
    EXPECT_TRUE(runEpoch(led, 32, {}, present).empty());
}

TEST(ArcLedgerTest, ReturnAfterDeathIsABirthThatRemembersItsGeneration) {
    ArcLedger led;
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };
    runEpoch(led, 0, { a }, present);
    for (long long ep = 1; ep <= 4; ++ep) runEpoch(led, ep, {}, present);
    ASSERT_EQ(led.find(a)->state, ArcState::Dead);

    std::vector<ArcStep> obs;
    runEpoch(led, 5, { a }, present, &obs);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].transition, ArcTransition::Birth);
    EXPECT_EQ(obs[0].generation, 2);
    // THE distinction the anchor election does not make: a return carries its
    // previous generation, a first sighting carries zero.
    EXPECT_EQ(obs[0].prev_generation, 1);
    EXPECT_EQ(led.find(a)->ep_first_seen, 0);   // the first sighting survives a death
}

TEST(ArcLedgerTest, ForgetWindowErasesTheRecordAndTheReturnIsAFirstSighting) {
    ArcLedger led;
    led.configure(/*horizon_s=*/60.0, /*forget_ep=*/5);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };
    runEpoch(led, 0, { a }, present);
    for (long long ep = 1; ep <= 4; ++ep) runEpoch(led, ep, {}, present);
    ASSERT_EQ(led.find(a)->state, ArcState::Dead);   // died at epoch 3

    for (long long ep = 5; ep <= 8; ++ep) runEpoch(led, ep, {}, present);
    EXPECT_NE(led.find(a), nullptr) << "forgotten before the window elapsed";
    runEpoch(led, 20, {}, present);
    EXPECT_EQ(led.find(a), nullptr) << "the record outlived the forget window";

    std::vector<ArcStep> obs;
    runEpoch(led, 21, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::Birth);
    EXPECT_EQ(obs[0].prev_generation, 0);   // nothing is remembered any more
    EXPECT_EQ(obs[0].generation, 1);
}

TEST(ArcLedgerTest, AnAbsentStationFreezesItsArcsInsteadOfGappingThem) {
    ArcLedger led;
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(1, "G01", "L1W");
    runEpoch(led, 0, { a, b }, { 0, 1 });

    // Station 1 reports nothing at all.  Its arc must NOT be gapped: a station
    // outage would otherwise manufacture a gap at every one of its arcs, and the
    // population the card is about is the arc that vanished while its station kept
    // reporting.
    const auto edges = runEpoch(led, 1, { a }, { 0 });
    EXPECT_TRUE(edges.empty());
    EXPECT_EQ(led.find(b)->state, ArcState::Tracking);

    // Station 0 kept reporting and dropped this arc: THAT is a gap.
    const auto edges2 = runEpoch(led, 2, {}, { 0 });
    ASSERT_EQ(edges2.size(), 1u);
    EXPECT_EQ(std::get<0>(edges2[0].first), 0);
    EXPECT_EQ(edges2[0].second.transition, ArcTransition::Gap);
    EXPECT_EQ(led.find(b)->state, ArcState::Tracking);
}

TEST(ArcLedgerTest, LiveAndRetainedCountsSeparateTheTwoPopulations) {
    ArcLedger led;
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);
    const std::set<int> present{ 0 };
    runEpoch(led, 0, { K(0, "G01", "L1W"), K(0, "G01", "L2W"), K(0, "G02", "L1W") },
             present);
    EXPECT_EQ(led.liveCount(), 3u);
    EXPECT_EQ(led.size(), 3u);

    runEpoch(led, 1, { K(0, "G01", "L1W") }, present);
    EXPECT_EQ(led.liveCount(), 1u);
    EXPECT_EQ(led.size(), 3u) << "a gapped arc is still retained";
    for (long long ep = 2; ep <= 4; ++ep) runEpoch(led, ep, { K(0, "G01", "L1W") }, present);
    EXPECT_EQ(led.liveCount(), 1u);
    EXPECT_EQ(led.size(), 3u) << "a dead arc is retained for the forget window";
}

// ---------------------------------------------------------------------------
// The event throttle
// ---------------------------------------------------------------------------

TEST(ArcLedgerTest, FirstEpochBirthsAreSuppressedAndTheHoldoffRateLimitsTheRest) {
    ArcLedger led;
    led.configure(900.0, kArcLedgerForgetEp);

    // Epoch 0: every arc is born there and the volume carries no information, so
    // NOTHING is evented (the NetworkSignalIntegrity.cpp rule).
    led.beginEpoch(0, 0.0);
    EXPECT_FALSE(led.eventAllowed(0, ArcTransition::Birth));
    EXPECT_FALSE(led.eventAllowed(1, ArcTransition::Birth));
    // A gap at epoch 0 is structurally impossible, but the holdoff is shared, so
    // the suppression must apply to births ALONE.
    EXPECT_TRUE(led.eventAllowed(0, ArcTransition::Gap));

    // A later birth is evented once per station, then held off.
    led.beginEpoch(1, kDt);
    EXPECT_TRUE(led.eventAllowed(0, ArcTransition::Birth));
    EXPECT_FALSE(led.eventAllowed(0, ArcTransition::Birth));
    EXPECT_TRUE(led.eventAllowed(1, ArcTransition::Birth)) << "the holdoff is per STATION";
    led.beginEpoch(1 + kArcLedgerEventHoldoffEp - 1, 0.0);
    EXPECT_FALSE(led.eventAllowed(0, ArcTransition::Birth));
    led.beginEpoch(1 + kArcLedgerEventHoldoffEp, 0.0);
    EXPECT_TRUE(led.eventAllowed(0, ArcTransition::Birth));
}

TEST(ArcLedgerTest, StateAndTransitionNamesAreStable) {
    // The trace and the ARC event tier are parsed by name offline, so the strings
    // are part of the interface, not log text.
    EXPECT_STREQ(arcStateName(ArcState::Absent), "absent");
    EXPECT_STREQ(arcStateName(ArcState::Tracking), "tracking");
    EXPECT_STREQ(arcStateName(ArcState::Gap), "gap");
    EXPECT_STREQ(arcStateName(ArcState::Reentered), "reentered");
    EXPECT_STREQ(arcStateName(ArcState::Dead), "dead");
    EXPECT_STREQ(arcTransitionName(ArcTransition::Birth), "arc_birth");
    EXPECT_STREQ(arcTransitionName(ArcTransition::Gap), "arc_gap");
    EXPECT_STREQ(arcTransitionName(ArcTransition::Reentry), "arc_reentry");
    EXPECT_STREQ(arcTransitionName(ArcTransition::Death), "arc_death");
}

}  // namespace GnssProcessor
