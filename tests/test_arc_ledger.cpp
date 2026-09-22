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

// ---------------------------------------------------------------------------
// Card SID phase 2: the ADMITTED lifecycle beside the raw one
// ---------------------------------------------------------------------------

namespace {

/// Drive one epoch on BOTH sides: the raw pass (build seam) and then the admitted
/// pass (audit seam), which is the order the producer runs them in.
struct BothSides {
    std::vector<std::pair<ArcLedger::Key, ArcStep>> raw_edges;
    std::vector<std::pair<ArcLedger::Key, ArcStep>> adm_edges;
    std::vector<ArcStep> observed;
};

BothSides runBoth(ArcLedger& led, long long ep,
                  const std::vector<ArcLedger::Key>& raw,
                  const std::vector<ArcLedger::Key>& admitted,
                  const std::set<int>& present) {
    BothSides out;
    led.beginEpoch(ep, static_cast<double>(ep) * kDt);
    for (const auto& k : raw) out.observed.push_back(led.observe(k, /*modelled=*/true));
    led.endEpoch(present, out.raw_edges);
    led.admitEpoch(std::set<ArcLedger::Key>(admitted.begin(), admitted.end()),
                   out.adm_edges);
    return out;
}

}  // namespace

TEST(ArcLedgerTest, ARawPresentButUnadmittedEpochIsTrackingAndAdmittedGap) {
    // THE structural finding of phase 1, as a state: the raw side arrives upstream
    // of the elevation mask, the allow list and every admission screen, so an arc
    // can be reported by the receiver and carried by nothing.  A reader that keys on
    // the carry (phase 3) has to see the ADMITTED state, and it must be Gap here
    // while the raw state stays Tracking.
    ArcLedger led;
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };
    const auto b = K(0, "G02", "L1W");   // a second arc, admitted throughout, so the
                                         // station never leaves the admitted present set

    runBoth(led, 0, { a, b }, { a, b }, present);
    ASSERT_NE(led.find(a), nullptr);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Tracking);

    // The receiver keeps reporting it; the station stops modelling it.
    auto s = runBoth(led, 1, { a, b }, { b }, present);
    EXPECT_TRUE(s.raw_edges.empty()) << "the raw side must not gap a reported arc";
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);
    ASSERT_EQ(s.adm_edges.size(), 1u);
    EXPECT_EQ(s.adm_edges[0].first, a);
    EXPECT_EQ(s.adm_edges[0].second.transition, ArcTransition::Gap);
    EXPECT_EQ(s.adm_edges[0].second.from, ArcState::Tracking);
    EXPECT_EQ(s.adm_edges[0].second.to, ArcState::Gap);
}

TEST(ArcLedgerTest, TheAdmittedGapHasItsOwnLengthAndReentryEdge) {
    ArcLedger led;
    led.configure(900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const std::set<int> present{ 0 };

    runBoth(led, 0, { a, b }, { a, b }, present);
    for (long long ep = 1; ep <= 3; ++ep) runBoth(led, ep, { a, b }, { b }, present);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);
    EXPECT_EQ(led.find(a)->adm_gap_len_ep, 3);
    EXPECT_EQ(led.find(a)->admittedGapEp(3), 2);   // epochs MISSED, not epochs since

    const auto s = runBoth(led, 4, { a, b }, { a, b }, present);
    ASSERT_EQ(s.adm_edges.size(), 1u);
    EXPECT_EQ(s.adm_edges[0].second.transition, ArcTransition::Reentry);
    EXPECT_EQ(s.adm_edges[0].second.gap_len_ep, 3);   // epochs 1, 2 and 3
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Reentered);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking) << "the raw side never gapped";

    // Reentered is one epoch wide on this side too.
    runBoth(led, 5, { a, b }, { a, b }, present);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Tracking);
}

TEST(ArcLedgerTest, ARawStepCarriesBothGapLengths) {
    // The `arc_reentry` event has to state the admitted gap as well as the raw one:
    // the two are different numbers and neither is derivable from the other.
    ArcLedger led;
    led.configure(900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const std::set<int> present{ 0 };

    // Never admitted at all: the admitted gap is -1, which is a different statement
    // from a zero-length gap and must stay separable.
    auto s = runBoth(led, 0, { a, b }, { b }, present);
    ASSERT_EQ(s.observed.size(), 2u);
    EXPECT_EQ(s.observed[0].transition, ArcTransition::Birth);
    EXPECT_EQ(s.observed[0].adm_gap_len_ep, -1);
    EXPECT_EQ(s.observed[0].adm_state, ArcState::Absent);

    runBoth(led, 1, { a, b }, { a, b }, present);   // admitted from here
    runBoth(led, 2, { a, b }, { b }, present);      // admitted side gaps
    runBoth(led, 3, { b }, { b }, present);         // raw side gaps too
    EXPECT_EQ(led.find(a)->state, ArcState::Gap);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);

    s = runBoth(led, 5, { a, b }, { b }, present);  // the RAW side returns alone
    ASSERT_EQ(s.observed.size(), 2u);
    EXPECT_EQ(s.observed[0].transition, ArcTransition::Reentry);
    EXPECT_EQ(s.observed[0].gap_len_ep, 2);          // raw: epochs 3 and 4
    EXPECT_EQ(s.observed[0].adm_gap_len_ep, 3);      // admitted: epochs 2, 3 and 4
    EXPECT_EQ(s.observed[0].adm_state, ArcState::Gap);
}

TEST(ArcLedgerTest, TheAdmittedSideDiesOnItsOwnHorizonAndAnAbsentStationFreezesIt) {
    ArcLedger led;
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const auto c = K(1, "G01", "L1W");
    const std::set<int> present{ 0, 1 };

    runBoth(led, 0, { a, b, c }, { a, b, c }, present);
    // Station 0 keeps modelling G02 and stops modelling G01; station 1 models
    // nothing at all, so its arc must be FROZEN rather than gapped.
    for (long long ep = 1; ep <= 3; ++ep) runBoth(led, ep, { a, b, c }, { b }, present);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Dead);
    EXPECT_EQ(led.find(a)->adm_reason_mask & kArcReasonHorizon, kArcReasonHorizon);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking) << "the raw side tracked through";
    EXPECT_EQ(led.find(c)->adm_state, ArcState::Tracking)
        << "a station with no admitted row must not gap its arcs";

    // A return after an admitted death is an admitted BIRTH that remembers its
    // generation, exactly as on the raw side.
    const auto s = runBoth(led, 4, { a, b, c }, { a, b }, present);
    bool seen = false;
    for (const auto& [key, step] : s.adm_edges) {
        if (key != a) continue;
        seen = true;
        EXPECT_EQ(step.transition, ArcTransition::Birth);
        EXPECT_EQ(step.generation, 2);
        EXPECT_EQ(step.prev_generation, 1);
    }
    EXPECT_TRUE(seen);
    EXPECT_EQ(led.find(a)->ep_first_admitted, 0);
}

TEST(ArcLedgerTest, AdmittedLiveCountIsItsOwnPopulation) {
    ArcLedger led;
    led.configure(900.0, kArcLedgerForgetEp);
    const std::set<int> present{ 0 };
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G01", "L2W");
    const auto c = K(0, "G02", "L1W");

    runBoth(led, 0, { a, b, c }, { a, b }, present);
    EXPECT_EQ(led.liveCount(), 3u);
    EXPECT_EQ(led.admittedLiveCount(), 2u)
        << "the admitted population is a strict subset of the raw one";
    EXPECT_EQ(led.size(), 3u);
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
