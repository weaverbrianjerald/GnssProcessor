/**
 * @file test_arc_ledger.cpp
 * @brief Tests for the arc lifecycle state machine
 *        (docs/refound/SID_signal_identity_2026-09-22.md section 5, P5).
 *
 * The ledger is the single state machine that the 29 arc-keyed containers of the
 * audit can later read, so it has to be correct before anything reads it.  The
 * properties tested here are the ones every later phase depends on:
 *
 *  - the state sequence over a synthetic presence pattern, including that a
 *    re-entry is a state and not an inference;
 *  - the generation counter, which tells a returning key from a first sighting
 *    (the distinction the anchor election does not make);
 *  - the horizon, which is a time and not an epoch count, so a run at a different
 *    epoch interval means the same thing;
 *  - the forget window, after which a record is erased;
 *  - the event throttle, which keeps a 305-station first epoch from writing about
 *    12 000 birth rows;
 *  - that an arc whose station reported nothing is frozen and never gapped.
 *
 * Every test drives the class directly: no station, no model, no solve.
 */

#include <gtest/gtest.h>

#include "network/state/Entities.hpp"
#include "network/state/ArcLedger.hpp"

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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };

    std::vector<ArcStep> obs;
    runEpoch(led, 0, { a }, present, &obs);
    ASSERT_EQ(obs.size(), 1u);
    EXPECT_EQ(obs[0].transition, ArcTransition::Birth);
    EXPECT_EQ(obs[0].generation, 1);
    EXPECT_EQ(obs[0].prev_generation, 0);   // a true first sighting
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

    // The return is a re-entry with the gap length in epochs, not a birth.
    obs.clear();
    runEpoch(led, 4, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::Reentry);
    EXPECT_EQ(obs[0].gap_len_ep, 2);        // epochs 2 and 3 were missed
    EXPECT_EQ(obs[0].generation, 1);        // a re-entry does not advance it
    EXPECT_EQ(led.find(a)->state, ArcState::Reentered);

    // Reentered is one epoch wide.
    obs.clear();
    runEpoch(led, 5, { a }, present, &obs);
    EXPECT_EQ(obs[0].transition, ArcTransition::None);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking);
}

TEST(ArcLedgerTest, HorizonIsATimeAndKillsTheArcOnce) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const std::set<int> present{ 0 };
    runEpoch(led, 0, { a }, present);

    // 900 s at 30 s per epoch is 30 epochs.  The gap opens at epoch 1 and the arc
    // is still alive at the boundary; it dies on the first epoch past it.
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
    // A death is reported once: the state is terminal until the arc returns.
    EXPECT_TRUE(runEpoch(led, 32, {}, present).empty());
}

TEST(ArcLedgerTest, ReturnAfterDeathIsABirthThatRemembersItsGeneration) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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
    // The distinction the anchor election does not make: a return carries its
    // previous generation, a first sighting carries zero.
    EXPECT_EQ(obs[0].prev_generation, 1);
    EXPECT_EQ(led.find(a)->ep_first_seen, 0);   // the first sighting survives a death
}

TEST(ArcLedgerTest, ForgetWindowErasesTheRecordAndTheReturnIsAFirstSighting) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(1, "G01", "L1W");
    runEpoch(led, 0, { a, b }, { 0, 1 });

    // Station 1 reports nothing at all, and its arc is not gapped.  Otherwise a
    // station outage would create a gap at every one of its arcs, while the
    // population of interest is the arc that vanished while its station
    // kept reporting.
    const auto edges = runEpoch(led, 1, { a }, { 0 });
    EXPECT_TRUE(edges.empty());
    EXPECT_EQ(led.find(b)->state, ArcState::Tracking);

    // Station 0 kept reporting and dropped this arc: that is a gap.
    const auto edges2 = runEpoch(led, 2, {}, { 0 });
    ASSERT_EQ(edges2.size(), 1u);
    EXPECT_EQ(std::get<0>(edges2[0].first), 0);
    EXPECT_EQ(edges2[0].second.transition, ArcTransition::Gap);
    EXPECT_EQ(led.find(b)->state, ArcState::Tracking);
}

TEST(ArcLedgerTest, LiveAndRetainedCountsSeparateTheTwoPopulations) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);

    // Epoch 0: every arc is born there and the volume carries no information, so
    // no birth is evented (the SignalIntegrity.cpp rule).
    led.beginEpoch(0, 0.0);
    EXPECT_FALSE(led.eventAllowed(0, ArcTransition::Birth));
    EXPECT_FALSE(led.eventAllowed(1, ArcTransition::Birth));
    // A gap at epoch 0 is structurally impossible, but the holdoff is shared, so
    // the suppression applies to births alone.
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
// The admitted lifecycle beside the raw one
// ---------------------------------------------------------------------------

namespace {

/// Drive one epoch on both sides: the raw pass (build seam) and then the admitted
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
    // The structural finding of phase 1, as a state: the raw side arrives upstream
    // of the elevation mask, the allow list and every admission screen, so an arc
    // can be reported by the receiver and carried by nothing.  A reader that keys on
    // the carry (phase 3) needs the admitted state, which is Gap here while the raw
    // state stays Tracking.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const std::set<int> present{ 0 };

    runBoth(led, 0, { a, b }, { a, b }, present);
    for (long long ep = 1; ep <= 3; ++ep) runBoth(led, ep, { a, b }, { b }, present);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);
    EXPECT_EQ(led.find(a)->adm_gap_len_ep, 3);
    EXPECT_EQ(led.find(a)->admittedGapEp(3), 2);   // epochs missed before epoch 3 (1, 2), not epochs since

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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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

    s = runBoth(led, 5, { a, b }, { b }, present);  // the raw side returns alone
    ASSERT_EQ(s.observed.size(), 2u);
    EXPECT_EQ(s.observed[0].transition, ArcTransition::Reentry);
    EXPECT_EQ(s.observed[0].gap_len_ep, 2);          // raw: epochs 3 and 4
    EXPECT_EQ(s.observed[0].adm_gap_len_ep, 3);      // admitted: epochs 2, 3 and 4
    EXPECT_EQ(s.observed[0].adm_state, ArcState::Gap);
}

TEST(ArcLedgerTest, TheAdmittedSideDiesOnItsOwnHorizonAndAnAbsentStationFreezesIt) {
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(/*horizon_s=*/60.0, kArcLedgerForgetEp);
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const auto c = K(1, "G01", "L1W");
    const std::set<int> present{ 0, 1 };

    runBoth(led, 0, { a, b, c }, { a, b, c }, present);
    // Station 0 keeps modelling G02 and stops modelling G01; station 1 models
    // nothing at all, so its arc is frozen rather than gapped.
    for (long long ep = 1; ep <= 3; ++ep) runBoth(led, ep, { a, b, c }, { b }, present);
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Dead);
    EXPECT_EQ(led.find(a)->adm_reason_mask & kArcReasonHorizon, kArcReasonHorizon);
    EXPECT_EQ(led.find(a)->state, ArcState::Tracking) << "the raw side tracked through";
    EXPECT_EQ(led.find(c)->adm_state, ArcState::Tracking)
        << "a station with no admitted row must not gap its arcs";

    // A return after an admitted death is an admitted birth that remembers its
    // generation, as on the raw side.
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
    net::NetworkEntities ents;
    ArcLedger led{ ents };
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

// ---------------------------------------------------------------------------
// The reason mask folded into the admitted gap
// ---------------------------------------------------------------------------

namespace {

/// The phase-4 epoch driver: open the reason buffer beside the ledger's epoch,
/// let the caller write reasons, then close both sides with the fold.
BothSides runWithReasons(ArcLedger& led, ArcReasonEpoch& buf, long long ep,
                         const std::vector<ArcLedger::Key>& raw,
                         const std::vector<ArcLedger::Key>& admitted,
                         const std::set<int>& present,
                         const std::vector<std::pair<ArcLedger::Key, unsigned>>& reasons) {
    BothSides out;
    led.beginEpoch(ep, static_cast<double>(ep) * kDt);
    arcReasonBeginEpoch(buf, ep);
    for (const auto& k : raw) out.observed.push_back(led.observe(k, /*modelled=*/true));
    for (const auto& kb : reasons) arcReasonRecord(buf, kb.first, kb.second);
    led.endEpoch(present, out.raw_edges);
    led.admitEpoch(std::set<ArcLedger::Key>(admitted.begin(), admitted.end()),
                   out.adm_edges, &buf);
    return out;
}

unsigned admMask(const ArcLedger& led, const ArcLedger::Key& k) {
    const ArcRecord* a = led.find(k);
    return a == nullptr ? kArcReasonNone : a->adm_reason_mask;
}

std::size_t rIdx(unsigned bit) {
    return static_cast<std::size_t>(arcReasonIndex(bit));
}

}  // namespace

TEST(ArcLedgerTest, ReasonBitsFoldIntoTheAdmittedGapThatOpened) {
    // The S1 contract: a screen that removes a modelled row records its bit,
    // and the gap the removal opens carries it.  Two arcs, two different screens,
    // so a per-arc mask cannot be a per-epoch one in disguise.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a = K(0, "G01", "L1W");
    const auto b = K(0, "G02", "L1W");
    const auto c = K(0, "G03", "L1W");   // stays admitted: keeps the station present
    const std::set<int> present{ 0 };

    runWithReasons(led, buf, 0, { a, b, c }, { a, b, c }, present, {});
    runWithReasons(led, buf, 1, { a, b, c }, { c }, present,
                   { { a, kArcReasonMask }, { b, kArcReasonAdmission } });

    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);
    EXPECT_EQ(admMask(led, a), kArcReasonMask);
    EXPECT_EQ(admMask(led, b), kArcReasonAdmission);
    EXPECT_EQ(admMask(led, c), kArcReasonNone) << "an arc that did not gap keeps no mask";
    EXPECT_EQ(buf.calls[rIdx(kArcReasonMask)], 1);
    EXPECT_EQ(buf.calls[rIdx(kArcReasonAdmission)], 1);
}

TEST(ArcLedgerTest, ReasonMaskSurvivesTheGapAndIsReadableAtTheReentry) {
    // The no-verdict split reads the mask at the return, so the mask survives
    // the whole gap unchanged and resets only at a genuine re-birth.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a = K(0, "G01", "L1W");
    const auto c = K(0, "G03", "L1W");
    const std::set<int> present{ 0 };

    runWithReasons(led, buf, 0, { a, c }, { a, c }, present, {});
    runWithReasons(led, buf, 1, { a, c }, { c }, present, { { a, kArcReasonAdmission } });
    runWithReasons(led, buf, 2, { a, c }, { c }, present, {});
    EXPECT_EQ(admMask(led, a), kArcReasonAdmission) << "the mask must not be re-stamped";
    const auto s = runWithReasons(led, buf, 3, { a, c }, { a, c }, present, {});
    bool saw = false;
    for (const auto& ks : s.adm_edges)
        if (ks.first == a) { saw = true; EXPECT_EQ(ks.second.transition, ArcTransition::Reentry); }
    EXPECT_TRUE(saw);
    EXPECT_EQ(admMask(led, a), kArcReasonAdmission)
        << "a re-entry reads the mask of the gap it closes";
}

TEST(ArcLedgerTest, AnUnexplainedAdmittedGapIsUnknown) {
    // The number phase 3 needs: an admitted gap that no writer explains.  It is
    // named rather than left empty, so a zero in the unknown row means "every gap
    // was attributed" and not "the fold never ran".
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a = K(0, "G01", "L1W");
    const auto c = K(0, "G03", "L1W");
    const std::set<int> present{ 0 };

    runWithReasons(led, buf, 0, { a, c }, { a, c }, present, {});
    runWithReasons(led, buf, 1, { a, c }, { c }, present, {});
    EXPECT_EQ(admMask(led, a), kArcReasonUnknown);
}

TEST(ArcLedgerTest, ADeweightIsNotAGapOpener) {
    // The brief's rule, and the reason kArcReasonGapMask exists: the gross-LOS
    // screen caps a code row's weight and the row still folds, so a de-weight is
    // never read as the screen that removed the arc.  An arc whose only record is
    // a de-weight and which then gaps reads unknown.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a = K(0, "G01", "L1W");
    const auto c = K(0, "G03", "L1W");
    const std::set<int> present{ 0 };

    runWithReasons(led, buf, 0, { a, c }, { a, c }, present, {});
    runWithReasons(led, buf, 1, { a, c }, { c }, present, { { a, kArcReasonDeweight } });
    EXPECT_EQ(admMask(led, a), kArcReasonUnknown);
    EXPECT_EQ(buf.calls[rIdx(kArcReasonDeweight)], 1)
        << "the de-weight is still COUNTED as a writer call";

    // A de-weight beside a real removal leaves the real reason alone.
    const auto b = K(1, "G01", "L1W");
    const auto d = K(1, "G03", "L1W");
    const std::set<int> two{ 0, 1 };
    runWithReasons(led, buf, 2, { b, d, c }, { b, d, c }, two, {});
    runWithReasons(led, buf, 3, { b, d, c }, { d, c }, two,
                   { { b, kArcReasonDeweight }, { b, kArcReasonGross } });
    EXPECT_EQ(admMask(led, b), kArcReasonGross);
}

TEST(ArcLedgerTest, TheLateBitSurvivesExactlyOneEpoch) {
    // The readmit park runs downstream of the fold, so its record reaches the
    // next epoch's fold and no further; every other bit is dropped at the epoch
    // boundary so an early writer cannot attribute a gap it did not open.
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a = K(0, "G01", "L1W");
    arcReasonBeginEpoch(buf, 0);
    arcReasonRecord(buf, a, kArcReasonReadmit);
    arcReasonRecord(buf, a, kArcReasonMask);
    EXPECT_EQ(arcReasonOf(buf, a), kArcReasonReadmit | kArcReasonMask);
    arcReasonBeginEpoch(buf, 1);
    EXPECT_EQ(arcReasonOf(buf, a), kArcReasonReadmit) << "only the late bit carries";
    arcReasonBeginEpoch(buf, 2);
    EXPECT_EQ(arcReasonOf(buf, a), kArcReasonNone) << "and it carries exactly one epoch";
}

TEST(ArcLedgerTest, AnUnarmedReasonBufferLeavesTheMachineAsPhase3WroteIt) {
    // Invariant #6 in both knob states, at the unit level: the fold is handed the
    // buffer on every run, and an unarmed buffer changes nothing.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;   // armed stays false
    const auto a = K(0, "G01", "L1W");
    const auto c = K(0, "G03", "L1W");
    const std::set<int> present{ 0 };

    runWithReasons(led, buf, 0, { a, c }, { a, c }, present, {});
    runWithReasons(led, buf, 1, { a, c }, { c }, present, { { a, kArcReasonMask } });
    EXPECT_EQ(led.find(a)->adm_state, ArcState::Gap);
    EXPECT_EQ(admMask(led, a), kArcReasonNone);
    EXPECT_TRUE(buf.rec.empty());
    EXPECT_EQ(buf.calls[rIdx(kArcReasonMask)], 0);
}

TEST(ArcLedgerTest, ReasonExpansionReachesOnlyAdmittedAliveArcs) {
    // The expansion rule for a screen whose identity is coarser than an arc: the
    // elevation mask names a satellite, so the writer has to reach that station's
    // arcs of it that the admitted lifecycle still calls alive, and nothing else.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto a1 = K(0, "G01", "L1W");
    const auto a2 = K(0, "G01", "L2W");
    const auto b1 = K(1, "G01", "L1W");
    const auto o  = K(0, "G02", "L1W");
    const std::set<int> present{ 0, 1 };

    runWithReasons(led, buf, 0, { a1, a2, b1, o }, { a1, a2, b1, o }, present, {});
    arcReasonBeginEpoch(buf, 1);
    EXPECT_EQ(arcReasonExpand(buf, led, 0, "G01", "", kArcReasonMask), 2);
    EXPECT_EQ(arcReasonOf(buf, a1), kArcReasonMask);
    EXPECT_EQ(arcReasonOf(buf, a2), kArcReasonMask);
    EXPECT_EQ(arcReasonOf(buf, b1), kArcReasonNone) << "another station is not reached";
    EXPECT_EQ(arcReasonOf(buf, o), kArcReasonNone)  << "another satellite is not reached";

    // Every station (-1), narrowed to one signal: the coverage gate's shape.
    EXPECT_EQ(arcReasonExpand(buf, led, -1, "G01", "L1W", kArcReasonGate), 2);
    EXPECT_EQ(arcReasonOf(buf, b1), kArcReasonGate);
    EXPECT_EQ(arcReasonOf(buf, a2), kArcReasonMask) << "L2W is not in the family";

    // A satellite the ledger does not know reaches nothing and says so.
    EXPECT_EQ(arcReasonExpand(buf, led, 0, "G99", "", kArcReasonMask), 0);
    EXPECT_GT(buf.calls_unknown[rIdx(kArcReasonMask)], 0);
}

TEST(ArcLedgerTest, ReasonExpansionNeverRunsPastItsStation) {
    // The scan starts at lower_bound(station, sat, ""), which for a station
    // holding no arc of that satellite lands on the first key of a later station;
    // the satellite test alone does not stop the scan there, because that key's
    // satellite matches.  The elevation mask reaches this case whenever it refuses
    // a satellite at a station that is not carrying it.  Without the station
    // check, a neighbouring station's gap would be attributed to the mask and
    // `unknown` would be undercounted.
    net::NetworkEntities ents;
    ArcLedger led{ ents };
    led.configure(900.0, kArcLedgerForgetEp);
    ArcReasonEpoch buf;
    buf.armed = true;
    const auto s0 = K(0, "G02", "L1W");   // station 0 holds only G02
    const auto s1 = K(1, "G01", "L1W");   // station 1 holds G01, the sought satellite
    const std::set<int> present{ 0, 1 };

    runWithReasons(led, buf, 0, { s0, s1 }, { s0, s1 }, present, {});
    arcReasonBeginEpoch(buf, 1);
    EXPECT_EQ(arcReasonExpand(buf, led, 0, "G01", "", kArcReasonMask), 0)
        << "station 0 has no G01 arc, so the expansion must reach nothing";
    EXPECT_EQ(arcReasonOf(buf, s1), kArcReasonNone)
        << "station 1's G01 arc must NOT inherit station 0's screen";
    EXPECT_EQ(arcReasonOf(buf, s0), kArcReasonNone);
}

TEST(ArcLedgerTest, ReasonNamesAreStable) {
    // Parsed offline out of the run-end table and the ARC events, so they are part
    // of the interface.
    EXPECT_STREQ(arcReasonName(kArcReasonHorizon), "horizon");
    EXPECT_STREQ(arcReasonName(kArcReasonMask), "mask");
    EXPECT_STREQ(arcReasonName(kArcReasonSlip), "slip");
    EXPECT_STREQ(arcReasonName(kArcReasonGate), "gate");
    EXPECT_STREQ(arcReasonName(kArcReasonAllowlist), "allowlist");
    EXPECT_STREQ(arcReasonName(kArcReasonAdmission), "admission");
    EXPECT_STREQ(arcReasonName(kArcReasonReadmit), "readmit");
    EXPECT_STREQ(arcReasonName(kArcReasonGross), "gross");
    EXPECT_STREQ(arcReasonName(kArcReasonDeweight), "deweight");
    EXPECT_STREQ(arcReasonName(kArcReasonNoModel), "nomodel");
    EXPECT_STREQ(arcReasonName(kArcReasonUnknown), "unknown");
    EXPECT_EQ(arcReasonMaskName(kArcReasonMask | kArcReasonSlip), "mask|slip");
    EXPECT_EQ(arcReasonMaskName(kArcReasonNone), "-");
    EXPECT_EQ(kArcReasonGapMask & kArcReasonDeweight, 0u)
        << "a de-weight can never be a gap opener";
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
