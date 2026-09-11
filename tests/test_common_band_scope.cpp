/**
 * @file test_common_band_scope.cpp
 * @brief COMMON-BAND SCOPE (network.cycle_slip.common_band_scope) — the §14.2
 *        rule, case by case, plus the drain hook's off-path contract.
 *
 * The defect: a single-band slip on a triple-frequency arc fires every MW pair
 * that CONTAINS the band (an L5 slip fires (1,5) and (2,5), leaving (1,2)
 * silent), and each firing pair's drain refloats BOTH of its bands — so the
 * per-epoch union is ALL bands, the §9 all-band floor, for a slip that touched
 * one.  The rule here forms ONE verdict per (station, satellite, epoch) from
 * POSITIVE exoneration: a silent, mature, un-vetoed MW pair exonerates both of
 * its bands; implicated = tracked \ exonerated; and with nothing exonerated the
 * response is today's, exactly.
 *
 * Gates:
 *   - CommonBandScopeRule — the seven worked cases of SPEC_event_harness §14.2,
 *     plus the two LLI cases, plus the counter each refusal must move.  Pure:
 *     no detector, no estimator, no network.
 *   - CommonBandScopeHook — @ref commonBandMayFree, the single predicate the
 *     three drains consult.  A null verdict set, an UNARMED one, and a
 *     (station, satellite) with no narrowed verdict must every one answer TRUE
 *     for every signal: that is what makes the knob-off run byte-identical.
 */

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "slip/NetworkCommonBandScope.hpp"

using namespace GnssProcessor;

namespace {

/// Production default of network.cycle_slip.common_band_min_arc_epochs.
constexpr int kMinArc = 10;

/// A mature, settled arc — well clear of the maturity bar.
constexpr int kMatureN = 87;

/// A detection gate tight enough that the silence means something (WL cycles).
constexpr double kTightThrWl = 3.1;

const std::set<std::string> kTriple{ "L1C", "L2W", "L5Q" };

CommonBandPair P(const std::string& a, const std::string& b) { return { a, b }; }

/// Evidence for the common GPS triple-frequency shape.
///
/// EVERY named pair is one the detector PROCESSED this epoch — that is the
/// silence source since iteration 2 — and the @p flagged list additionally
/// emitted an MwSlipResult, so those are the pairs that are NOT silent.  A pair
/// in neither list was not processed at all and cannot testify either way.
CommonBandEvidence ev(std::initializer_list<CommonBandPair> silent,
                      std::initializer_list<CommonBandPair> flagged,
                      int n = kMatureN, double thr_wl = kTightThrWl) {
    CommonBandEvidence e;
    for (const auto& p : silent)
        e.processed[p] = CommonBandProcessed{ n, /*flagged=*/false, thr_wl };
    for (const auto& p : flagged) {
        e.processed[p] = CommonBandProcessed{ n, /*flagged=*/true, thr_wl };
        e.flagged_pairs.insert(p);
    }
    return e;
}

/// §14.11 rule F2: mark every processed pair that CONTAINS @p band as
/// LLI-shadowed, i.e. one of its bands carried a receiver loss-of-lock bit at
/// the PREVIOUS epoch.  Band-scoped exactly as the detector exports it.
void shadowBand(CommonBandEvidence& e, const std::string& band) {
    for (auto& [pr, st] : e.processed)
        if (pr.first == band || pr.second == band) st.lli_shadow = true;
}

}  // namespace

// ---------------------------------------------------------------- the rule

/// §14.2 worked case 1 — an L5 slip fires (1,5) and (2,5); (1,2) stays silent
/// and exonerates L1 and L2.  Implicated is the slipped band alone.
TEST(CommonBandScopeRule, L5SlipNarrowsToL5) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
    EXPECT_EQ(v.iftd_veto, 0);
    EXPECT_EQ(v.immature_pairs, 0);
}

/// §14.2 worked case 2 — an L1 slip fires (1,2) and (1,5); the IF-TD detector
/// fires on ITS pair (1,2), which is already flagged by MW and so was never an
/// exonerator.  (2,5) is silent and clears L2 and L5.  No veto is recorded:
/// the vetoed population is SILENT pairs only.
TEST(CommonBandScopeRule, L1SlipNarrowsToL1EvenWithIfTdOnThePair) {
    auto e = ev({ P("L2W", "L5Q") }, { P("L1C", "L2W"), P("L1C", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L1C" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L2W", "L5Q" }));
    EXPECT_EQ(v.iftd_veto, 0);
}

/// §14.2 worked case 3 — an L2 slip fires (1,2) and (2,5); (1,5) exonerates.
TEST(CommonBandScopeRule, L2SlipNarrowsToL2) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L5Q" }));
}

/// §14.2 worked case 4 — two bands slipping by DIFFERENT counts fire every
/// pair.  Nothing is silent, nothing is exonerated: today's all-band response,
/// and the verdict must NOT be marked narrowed.
TEST(CommonBandScopeRule, AllPairsFireFallsBackToAllBands) {
    const auto v = networkCommonBandVerdict(
        kTriple,
        ev({}, { P("L1C", "L2W"), P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
}

/// §14.2 worked case 5 — the EQUAL-COUNT trap.  L1 and L2 slipping by the SAME
/// integer leave MW(1,2) silent (the wide-lane combination cancels), so the
/// naive rule would exonerate exactly the two slipped bands and free the
/// innocent one.  The IF-TD flag on (1,2) is what refuses it: the iono-free
/// combination moves 0.107 m per cycle and DOES see the event.  Nothing else
/// is silent, so the response is today's — and the veto counter must name it.
TEST(CommonBandScopeRule, EqualCountL1L2VetoedByIfTd) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.iftd_veto, 1);
    EXPECT_NE(v.basis.find("iftd(L1C+L2W)=flagged"), std::string::npos);
}

/// §14.2 worked case 6 — a pair still in WARM-UP cannot exonerate.  An immature
/// arc could not have flagged at all, so its silence is not evidence; the
/// refusal must be counted, not silent.
TEST(CommonBandScopeRule, ImmaturePairCannotExonerate) {
    const auto v = networkCommonBandVerdict(
        kTriple,
        ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") }, /*n=*/5),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.immature_pairs, 1);
    EXPECT_NE(v.basis.find("immature(n=5)"), std::string::npos);
}

/// The positive half of the maturity gate, stated on its own: the SAME
/// unflagged pair at n = 10 (the bar exactly) is silent and does exonerate.
/// Iteration 1 read the cleanliness set here and this pair was never offered,
/// which is why the rule never engaged on the sim.
TEST(CommonBandScopeRule, UnflaggedMaturePairIsSilent) {
    const auto v = networkCommonBandVerdict(
        kTriple,
        ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") },
           /*n=*/kMinArc),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.immature_pairs, 0);
    EXPECT_NE(v.basis.find("silent(n=10,thr=3.1WL)"), std::string::npos);
}

/// A pair the detector never PROCESSED this epoch is not evidence of anything.
/// Absence and silence are different facts, and only the second exonerates.
TEST(CommonBandScopeRule, UnprocessedPairCannotExonerate) {
    CommonBandEvidence e;
    e.processed[P("L1C", "L5Q")] = CommonBandProcessed{ kMatureN, true, kTightThrWl };
    e.processed[P("L2W", "L5Q")] = CommonBandProcessed{ kMatureN, true, kTightThrWl };
    e.flagged_pairs = { P("L1C", "L5Q"), P("L2W", "L5Q") };
    // (L1C,L2W) simply never appears — the arc was not sampled this epoch.
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.immature_pairs, 0);
}

/// A silence whose DETECTION THRESHOLD stood above 6 wide-lane cycles could not
/// have excluded a 6-cycle slip.  It still exonerates — the rule is unchanged —
/// but it is COUNTED, so the share of narrowing resting on a loose gate is
/// measurable instead of assumed.
TEST(CommonBandScopeRule, WeakSilenceExoneratesButIsCounted) {
    const auto v = networkCommonBandVerdict(
        kTriple,
        ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") },
           kMatureN, /*thr_wl=*/8.4),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.weak_silence, 1);
    EXPECT_NE(v.basis.find("thr=8.4WL"), std::string::npos);
}

/// ...and a tight one is not counted, so the counter tracks the gate and not
/// merely the act of narrowing.
TEST(CommonBandScopeRule, TightSilenceIsNotWeak) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.weak_silence, 0);
}

/// §14.2 worked case 7 — (1,2) fires ALONE while (1,5) and (2,5) are both
/// silent.  Between them they exonerate every band, so implicated comes out
/// EMPTY although a pair fired: the evidence contradicts itself and the honest
/// response is today's.  A distinct fallback from "nothing exonerated", because
/// it means something upstream is wrong, not that the arc is simply noisy.
TEST(CommonBandScopeRule, InconsistentEvidenceFallsBackToAllBands) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L5Q"), P("L2W", "L5Q") }, { P("L1C", "L2W") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Inconsistent);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
}

/// LLI on L5Q alone, with the MW evidence of an L5 slip.  The flagged band is
/// implicated by the receiver's own bit and the silent (1,2) still exonerates
/// L1 and L2 — the same narrowing, reached through the LLI clause.
TEST(CommonBandScopeRule, LliOnOneBandNarrowsToThatBand) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    e.lli_signals.insert("L5Q");
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
    EXPECT_NE(v.basis.find("lli=L5Q"), std::string::npos);
}

/// LLI on EVERY band overrides every exoneration: a band the receiver reports
/// as broken is implicated regardless of what any pair says, so even a silent,
/// mature (1,2) cannot narrow the response.
TEST(CommonBandScopeRule, LliOnAllBandsForcesAllBandResponse) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    e.lli_signals = { "L1C", "L2W", "L5Q" };
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
}

/// No MW pair fired: there is no response to scope at all.  Distinct from the
/// three fallbacks — it is not counted as a verdict, and the caller skips it.
TEST(CommonBandScopeRule, NoFlagIsNotAVerdict) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L2W"), P("L1C", "L5Q"), P("L2W", "L5Q") }, {}),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoFlag);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
}

/// A processed pair that ALSO emitted an MwSlipResult is not silent.  The
/// emitted set is the authority over the detector's advisory per-sample flag,
/// because the emitted set is what the drains act on — a flag DROPPED on the
/// admission gate never reaches a drain but is still not silence.
TEST(CommonBandScopeRule, FlaggedPairNeverExoneratesEvenIfProcessedQuietly) {
    CommonBandEvidence e;
    // Processed, mature, and the per-sample flag says quiet — yet an
    // MwSlipResult was emitted for it (the drop-on-admission population).
    e.processed[P("L1C", "L2W")] = CommonBandProcessed{ kMatureN, false, kTightThrWl };
    e.flagged_pairs = { P("L1C", "L2W") };
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
}

/// Two tracked bands only (the dual-frequency posture): the pair IS the arc, so
/// a fired pair leaves nothing to exonerate and the response is today's.
TEST(CommonBandScopeRule, DualFrequencyArcCannotNarrow) {
    const std::set<std::string> dual{ "L1C", "L2W" };
    const auto v = networkCommonBandVerdict(dual, ev({}, { P("L1C", "L2W") }),
                                            kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, dual);
}

/// A silent pair naming a band the station does not track cannot conjure it
/// into the implicated set: the verdict is closed over the LIVE bands.
TEST(CommonBandScopeRule, ImplicatedIsClosedOverLiveBands) {
    const std::set<std::string> live{ "L1C", "L5Q" };
    const auto v = networkCommonBandVerdict(
        live, ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C" }));
}

// ------------------------------------------------------- §14.5 anchor clause

/// The evidence is sound and WOULD narrow, but the station anchors a family of
/// this satellite.  Narrowing there would hand over only the implicated band's
/// family and leave the rest pinned here, splitting the satellite's
/// per-frequency references across stations.  Response stays all-band.
TEST(CommonBandScopeRule, AnchorArcRefusesToNarrow) {
    const auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc,
                                            /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::AnchorColocate);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 1);
    // Today's response: every band the drains would have freed.
    EXPECT_EQ(v.implicated, kTriple);
    // The evidence itself is untouched — it was the datum topology that refused.
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
}

/// The SAME evidence on a non-anchor arc narrows exactly as before, so the
/// clause is scoped to the arcs that realize the gauge and nothing else.
TEST(CommonBandScopeRule, NonAnchorArcNarrowsUnchanged) {
    const auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc,
                                            /*station_anchors_satellite=*/false);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 0);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
}

/// The clause only bites where the evidence would have narrowed: an all-band
/// fallback on an anchor arc keeps its own reason, so the census separates
/// "nothing exonerated" from "refused for co-location".
TEST(CommonBandScopeRule, AnchorClauseDoesNotMaskOtherFallbacks) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({}, { P("L1C", "L2W"), P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc, /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.anchor_colocate, 0);
}

// ------------------------- §14.10: the veto needs an IF-TD pair on EVERY lane

// The card's defect, reproduced from the field: with the IF-TD detector forming
// ONE pair per satellite (L1C+L2W on GPS), a silent MW lane whose own IF pair
// was never evaluated has nothing behind it, and the equal-count trap is open on
// every lane but the first.  Measured on the storm bed at 4.99 h, station
// S37000SIM / G20: truth L5Q −31 cycles (receiver-flagged) then L1C −29, the
// (L1C+L5Q) wide lane moved 2 cycles and stayed silent, and the verdict read
//     keep implicated=L2W; exonerated=L1C;L5Q;
//     basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;
//           mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged
// — it freed the one band that had NOT slipped, the epoch's misfit was metres,
// and the user paid +9.5 cm for the following hour.  The three cases below are
// that shape with and without the (L1C,L5Q) IF-TD evaluation, and the genuine
// single-band control that must survive the widening unchanged.

/// §14.10 (i) — the S37/G20 shape WITH the non-first IF-TD pair evaluated.  MW
/// (1,2) and (2,5) fire; (1,5) is silent and mature; IF-TD (1,5) FLAGS, because
/// two bands moving by −29 and −31 cycles leave the wide lane at 2 cycles while
/// the iono-free combination moves metres.  The silence is refused, nothing is
/// exonerated, and the response is today's all-band one — with the veto named in
/// the basis and attributed to the pair that made it.
TEST(CommonBandScopeRule, S37G20ShapeVetoedByTheNonFirstIfTdPair) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L5Q"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
    EXPECT_EQ(v.iftd_veto, 1);
    EXPECT_EQ(v.iftd_veto_pairs, (std::set<CommonBandPair>{ P("L1C", "L5Q") }));
    EXPECT_NE(v.basis.find("mw(L1C+L5Q)silent"), std::string::npos);
    EXPECT_NE(v.basis.find("iftd(L1C+L5Q)=flagged"), std::string::npos);
}

/// The control for (i), and the measured defect itself: the SAME MW evidence
/// with the (1,5) IF-TD pair UNWATCHED — which is exactly what
/// `if_phase_td_pairs = first` leaves it — narrows to L2W, the one band that did
/// not move.  This case must keep failing to veto: it is the before picture.
TEST(CommonBandScopeRule, S37G20ShapeWithoutItsIfTdPairNarrowsToTheWrongBand) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L2W"));   // the only pair `first` evaluates
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L5Q" }));
    EXPECT_EQ(v.iftd_veto, 0);
    EXPECT_TRUE(v.iftd_veto_pairs.empty());
}

/// §14.10 (ii) — the PURE equal-count trap on (1,5): L1C and L5Q both −29, so
/// their wide lane is silent by construction while (1,2) and (2,5) both fire.
/// Same evidence shape as (i) without the size asymmetry, and the same outcome:
/// the (1,5) IF-TD flag is the only instrument that can see it.
TEST(CommonBandScopeRule, EqualCountL1L5VetoedByIfTd) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L5Q"));
    e.iftd_silent.insert(P("L1C", "L2W"));
    e.iftd_silent.insert(P("L2W", "L5Q"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.iftd_veto, 1);
    EXPECT_EQ(v.iftd_veto_pairs, (std::set<CommonBandPair>{ P("L1C", "L5Q") }));
    // The two silent IF-TD pairs are recorded, and exonerate nothing on their own.
    EXPECT_NE(v.basis.find("iftd(L1C+L2W)silent"), std::string::npos);
    EXPECT_NE(v.basis.find("iftd(L2W+L5Q)silent"), std::string::npos);
}

/// §14.10 (iii) — the widening must be INERT on a genuine single-band slip.  L2W
/// alone moves: MW (1,2) and (2,5) fire, (1,5) stays silent, and all three IF-TD
/// pairs are now evaluated with only the two that contain L2 flagging.  The
/// silent lane's IF-TD pair is silent too, so it still exonerates and the verdict
/// is the §14.2 one — narrowed to exactly the slipped band.
TEST(CommonBandScopeRule, GenuineL2SlipStillNarrowsWithEveryIfPairEvaluated) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L2W"));
    e.iftd_flagged.insert(P("L2W", "L5Q"));
    e.iftd_silent.insert(P("L1C", "L5Q"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L5Q" }));
    EXPECT_EQ(v.iftd_veto, 0);
    EXPECT_NE(v.basis.find("iftd(L1C+L5Q)silent"), std::string::npos);
}

// ------------------------------- §14.11 RULE F2: the LLI shadow on a silent lane

// The second half of the same field defect.  The uncombined LLI branch of
// MwSlipDetector::detect schedules the ambiguity resets and `continue`s,
// deliberately PRESERVING the MW arc — the MW combination is a function of raw
// observations alone, so a short lock-loss does not invalidate the EWMA.  The
// consequence for THIS rule is that the arc count runs straight through the
// flagged epoch and the first sample after it reads "mature" while the lane saw
// nothing at the epoch the receiver declared broken.  On the storm bed all five
// spurious `keep` verdicts in 2 994 sat exactly one epoch after an LLI, and in
// every one the exonerating lane CONTAINED the flagged band (S37/G20 (L1C+L5Q)
// n 87→99 across the LLI, thr 9.0 WL; S25/E36 (L1C+L5Q) n 164→173).

/// §14.11 (a) — the S37/G20 shape with the (1,5) lane LLI-shadowed and the IF-TD
/// detector still at `first`, i.e. no IF-TD pair behind that lane.  F2 ALONE
/// closes it: the shadowed lane cannot exonerate, nothing else can, and the
/// response is today's all-band one.
TEST(CommonBandScopeRule, LliShadowedLaneCannotExonerateWithoutIfTd) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") },
                /*n=*/99, /*thr_wl=*/9.0);
    shadowBand(e, "L5Q");            // the receiver flagged L5Q one epoch ago
    e.iftd_flagged.insert(P("L1C", "L2W"));   // the only pair `first` evaluates
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
    EXPECT_EQ(v.lli_shadow, 1);
    EXPECT_EQ(v.iftd_veto, 0);       // the veto never got the chance; F2 acted first
    EXPECT_NE(v.basis.find("mw(L1C+L5Q)lli_shadow(n=99)"), std::string::npos);
}

/// §14.11 (b) — the Galileo shape: an LLI on L5Q at t−1, and at t the lanes that
/// contain L7Q fire late on the already-answered slip while (1,5) sits silent.
/// Without F2 that narrows to L7Q, an innocent band.  With it, no narrowing.
TEST(CommonBandScopeRule, LliShadowNeverPinsTheLateFiringLanesInnocentBand) {
    const std::set<std::string> gal{ "L1C", "L5Q", "L7Q" };
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L7Q"), P("L5Q", "L7Q") },
                /*n=*/173, /*thr_wl=*/4.2);
    shadowBand(e, "L5Q");
    const auto v = networkCommonBandVerdict(gal, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_NE(v.implicated, (std::set<std::string>{ "L7Q" }));
    EXPECT_EQ(v.implicated, gal);
    EXPECT_EQ(v.lli_shadow, 1);
}

/// §14.11 (c) — the shadow is BAND-SCOPED, and that is what keeps the rule cheap.
/// The 853-of-858 population: an LLI on L5Q at t−1, and at t the two lanes that
/// CONTAIN L5Q fire late on the already-answered slip while (L1C, L2W) — the one
/// lane carrying no flagged band, so not shadowed — is silent and mature.  It
/// still exonerates, and the verdict re-implicates exactly the band the receiver
/// reported.  F2 must not cost this.
TEST(CommonBandScopeRule, LliShadowLeavesTheUnflaggedLaneFreeToExonerate) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    shadowBand(e, "L5Q");            // shadows (1,5) and (2,5); (1,2) untouched
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
    EXPECT_EQ(v.lli_shadow, 0);      // both shadowed lanes were flagged anyway
    EXPECT_FALSE(e.processed.at(P("L1C", "L2W")).lli_shadow);
}

/// And the converse, so the band scope is pinned in both directions: with the
/// SAME shadow, a genuine L1C slip at t leaves only shadowed or flagged lanes.
/// (1,2) fires, (1,5) fires, and the silent (2,5) contains the flagged band — so
/// nothing exonerates and the response is today's.  The narrowing that would
/// have pinned L2W here is exactly the spurious one the census found.
TEST(CommonBandScopeRule, LliShadowRefusesTheOnlySilentLaneWhenItCarriesTheBand) {
    auto e = ev({ P("L2W", "L5Q") }, { P("L1C", "L2W"), P("L1C", "L5Q") });
    shadowBand(e, "L5Q");
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.lli_shadow, 1);
    EXPECT_NE(v.basis.find("mw(L2W+L5Q)lli_shadow"), std::string::npos);
}

/// An unshadowed run of the SAME evidence as (a) narrows — the control that
/// proves F2 is what changed the verdict, not the thresholds or the maturity.
TEST(CommonBandScopeRule, WithoutTheShadowTheSameEvidenceStillNarrows) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") },
                /*n=*/99, /*thr_wl=*/9.0);
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.lli_shadow, 0);
}

// ------------------------------------------- §14.5 split-referenced satellites

/// Two satellites, one whose families are anchored at two different stations.
/// The split count is the instrument that caught the iteration-1 storm
/// regression, so it must count satellites, not families.
TEST(CommonBandScopeSplitRef, CountsSatellitesWithAnchorsAtDifferentStations) {
    const std::map<std::string, std::string> inc{
        { "G04_L1C", "S36000SIM" },   // split: three families, two stations
        { "G04_L2W", "S36000SIM" },
        { "G04_L5Q", "S12000SIM" },
        { "G08_L1C", "S21000SIM" },   // whole: all three at one station
        { "G08_L2W", "S21000SIM" },
        { "G08_L5Q", "S21000SIM" },
    };
    const auto sr = networkSplitReferencedSats(inc);
    EXPECT_EQ(sr.split, 1);
    EXPECT_EQ(sr.multi_anchored, 2);
}

/// A satellite with a single anchored family cannot be split and must not
/// inflate the denominator.
TEST(CommonBandScopeSplitRef, SingleFamilySatelliteIsNotCounted) {
    const std::map<std::string, std::string> inc{
        { "G04_L1C", "S36000SIM" },
        { "E11_L1C", "S02000SIM" },
        { "E11_L5Q", "S02000SIM" },
    };
    const auto sr = networkSplitReferencedSats(inc);
    EXPECT_EQ(sr.split, 0);
    EXPECT_EQ(sr.multi_anchored, 1);
}

/// An empty map (the run's first epoch, before any election) reads zero rather
/// than tripping on an absent key.
TEST(CommonBandScopeSplitRef, EmptyMapIsZero) {
    const auto sr = networkSplitReferencedSats({});
    EXPECT_EQ(sr.split, 0);
    EXPECT_EQ(sr.multi_anchored, 0);
}

// ----------------------------------------------------------- the drain hook

/// A null verdict set is the knob-off path: every drain frees everything it
/// would have freed before the card existed.
TEST(CommonBandScopeHook, NullVerdictSetFreesEverything) {
    EXPECT_TRUE(commonBandMayFree(nullptr, 3, "G08", "L1C"));
    EXPECT_TRUE(commonBandMayFree(nullptr, 3, "G08", "L5Q"));
}

/// An UNARMED epoch object (the census ran, the knob did not) must behave
/// exactly like the null pointer, so a wiring slip that passes it anyway cannot
/// change the solve.
TEST(CommonBandScopeHook, UnarmedEpochFreesEverything) {
    CommonBandEpoch cbs;
    cbs.armed = false;
    CommonBandVerdict v;
    v.narrowed   = true;
    v.implicated = { "L5Q" };
    cbs.narrowed_at[{ 3, "G08" }] = v;
    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G08", "L1C"));
}

/// Armed and narrowed: the hook frees the implicated band and refuses the rest —
/// for THAT (station, satellite) only.  Every other key is untouched, which is
/// what keeps a narrowed verdict from leaking across arcs.
TEST(CommonBandScopeHook, ArmedNarrowedVerdictGatesOnlyItsOwnArc) {
    CommonBandEpoch cbs;
    cbs.armed = true;
    CommonBandVerdict v;
    v.narrowed   = true;
    v.implicated = { "L5Q" };
    cbs.narrowed_at[{ 3, "G08" }] = v;

    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G08", "L5Q"));
    EXPECT_FALSE(commonBandMayFree(&cbs, 3, "G08", "L1C"));
    EXPECT_FALSE(commonBandMayFree(&cbs, 3, "G08", "L2W"));
    // Same satellite, different station.
    EXPECT_TRUE(commonBandMayFree(&cbs, 4, "G08", "L1C"));
    // Same station, different satellite.
    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G10", "L1C"));
}

/// clear() must drop the verdicts AND the census, or the next epoch inherits a
/// stale narrowing — the per-epoch contract the whole rule rests on.
TEST(CommonBandScopeHook, ClearDropsVerdictsAndCensus) {
    CommonBandEpoch cbs;
    cbs.armed = true;
    CommonBandVerdict v;
    v.narrowed   = true;
    v.implicated = { "L5Q" };
    cbs.narrowed_at[{ 3, "G08" }] = v;
    cbs.all.emplace_back(std::make_pair(3, std::string("G08")), v);
    cbs.verdicts = cbs.narrowed_n = cbs.bands_kept = 7;
    cbs.iftd_veto = cbs.immature_pairs = cbs.would_narrow = 7;
    cbs.all_no_exoneration = cbs.all_inconsistent = 7;

    cbs.clear();

    EXPECT_TRUE(cbs.narrowed_at.empty());
    EXPECT_TRUE(cbs.all.empty());
    EXPECT_EQ(cbs.verdicts, 0);
    EXPECT_EQ(cbs.narrowed_n, 0);
    EXPECT_EQ(cbs.bands_kept, 0);
    EXPECT_EQ(cbs.iftd_veto, 0);
    EXPECT_EQ(cbs.immature_pairs, 0);
    EXPECT_EQ(cbs.would_narrow, 0);
    EXPECT_EQ(cbs.all_no_exoneration, 0);
    EXPECT_EQ(cbs.all_inconsistent, 0);
    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G08", "L1C"));
}
