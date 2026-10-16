/**
 * @file test_common_band_scope.cpp
 * @brief Common-band scope (network.cycle_slip.common_band_scope): the §14.2 rule
 *        case by case, its later clauses, and the drain hook's off-path contract.
 *
 * The problem: a single-band slip on a triple-frequency arc fires every MW pair that
 * contains the band (an L5 slip fires (1,5) and (2,5) and leaves (1,2) silent), and
 * each firing pair's drain refloats both of its bands, so the per-epoch union is all
 * bands (the §9 all-band floor) for a slip that touched one.  The rule forms one
 * verdict per (station, satellite, epoch) from positive exoneration: a silent, mature,
 * un-vetoed MW pair exonerates both of its bands; implicated = tracked \ exonerated;
 * and with nothing exonerated the response is exactly the unscoped all-band one.
 *
 * Suites:
 *   - CommonBandScopeRule: the seven worked cases of SPEC_event_harness §14.2 and the
 *     two LLI cases; the §14.5 anchor clause; the §14.10 IF-TD veto on every lane; the
 *     §22 reference-arc clause (a silent pair whose IF-TD sample has dd ≡ 0 by
 *     construction is untested, not clean) with its named cost and its knob-off
 *     control; the §14.11 LLI shadow (F2); the §14.12 LLI-epoch verdict (F3); and the
 *     counter each refusal must move.  Pure: no detector, no estimator, no network.
 *   - CommonBandScopeSplitRef: the §14.5 split-referenced satellite count.
 *   - CommonBandScopeHook: @ref commonBandMayFree, the single predicate the three
 *     drains consult.  A null verdict set, an unarmed one, and a (station, satellite)
 *     with no narrowed verdict must each answer true for every signal; that makes the
 *     knob-off run byte-identical.
 */

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "network/slip/CommonBandScope.hpp"

using namespace GnssProcessor;

namespace {

/// Production default of network.cycle_slip.common_band_min_arc_epochs.
constexpr int kMinArc = 10;

/// A mature, settled arc, well clear of the maturity bar.
constexpr int kMatureN = 87;

/// A detection gate tight enough that the silence means something (WL cycles).
constexpr double kTightThrWl = 3.1;

const std::set<std::string> kTriple{ "L1C", "L2W", "L5Q" };

CommonBandPair P(const std::string& a, const std::string& b) { return { a, b }; }

/// Evidence for the common GPS triple-frequency shape.
///
/// Every named pair is one the detector processed this epoch (the processed set
/// is the source of silence), and the pairs in @p flagged also emitted an
/// MwSlipResult, so they are not silent.  A pair in neither list was not
/// processed at all and is evidence neither way.
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

/// Marks every processed pair that contains @p band as LLI-shadowed (§14.11 rule
/// F2), i.e. one of its bands carried a receiver loss-of-lock bit at the previous
/// epoch.  Band-scoped exactly as the detector exports it.
void shadowBand(CommonBandEvidence& e, const std::string& band) {
    for (auto& [pr, st] : e.processed)
        if (pr.first == band || pr.second == band) st.lli_shadow = true;
}

}  // namespace

// ---------------------------------------------------------------- the rule

/// Worked case 1 (§14.2): an L5 slip fires (1,5) and (2,5); (1,2) stays silent
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

/// Worked case 2 (§14.2): an L1 slip fires (1,2) and (1,5); the IF-TD detector
/// fires on its pair (1,2), which MW already flagged and so was never an
/// exonerator.  (2,5) is silent and clears L2 and L5.  No veto is recorded: the
/// vetoed population is silent pairs only.
TEST(CommonBandScopeRule, L1SlipNarrowsToL1EvenWithIfTdOnThePair) {
    auto e = ev({ P("L2W", "L5Q") }, { P("L1C", "L2W"), P("L1C", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L1C" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L2W", "L5Q" }));
    EXPECT_EQ(v.iftd_veto, 0);
}

/// Worked case 3 (§14.2): an L2 slip fires (1,2) and (2,5); (1,5) exonerates.
TEST(CommonBandScopeRule, L2SlipNarrowsToL2) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L5Q" }));
}

/// Worked case 4 (§14.2): two bands slipping by different counts fire every
/// pair.  Nothing is silent and nothing is exonerated, so the response is the
/// all-band one and the verdict must not be marked narrowed.
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

/// Worked case 5 (§14.2), the equal-count trap: L1 and L2 slipping by the same
/// integer leave MW(1,2) silent (the wide-lane combination cancels), so the
/// naive rule would exonerate the two slipped bands and free only the unslipped
/// one.  The IF-TD flag on (1,2) refuses the silence: the iono-free combination
/// moves 0.107 m per cycle and sees the event.  Nothing else is silent, so the
/// response is the all-band one, and the veto counter must record it.
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

/// Worked case 6 (§14.2): a pair still in warm-up cannot exonerate.  An immature
/// arc could not have flagged at all, so its silence is not evidence; the
/// refusal must be counted.
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

/// The positive half of the maturity gate, stated on its own: the same
/// unflagged pair at n = 10 (exactly the bar) is silent and does exonerate.
/// Silence is read from the processed set; a cleanliness set never offers this
/// pair, and a rule reading it never engages.
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

/// A pair the detector did not process this epoch is not evidence of anything.
/// Absence and silence are different facts, and only the second exonerates.
TEST(CommonBandScopeRule, UnprocessedPairCannotExonerate) {
    CommonBandEvidence e;
    e.processed[P("L1C", "L5Q")] = CommonBandProcessed{ kMatureN, true, kTightThrWl };
    e.processed[P("L2W", "L5Q")] = CommonBandProcessed{ kMatureN, true, kTightThrWl };
    e.flagged_pairs = { P("L1C", "L5Q"), P("L2W", "L5Q") };
    // (L1C,L2W) does not appear: the arc was not sampled this epoch.
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.immature_pairs, 0);
}

/// A silence whose detection threshold stood above 6 wide-lane cycles could not
/// have excluded a 6-cycle slip.  It still exonerates (the rule is unchanged),
/// but it is counted, so the share of narrowing that rests on a loose gate is
/// measured instead of assumed.
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

/// A tight silence is not counted, so the counter tracks the gate and not merely
/// the act of narrowing.
TEST(CommonBandScopeRule, TightSilenceIsNotWeak) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.weak_silence, 0);
}

/// Worked case 7 (§14.2): (1,2) fires alone while (1,5) and (2,5) are both
/// silent.  Between them the silent pairs exonerate every band, so implicated
/// comes out empty although a pair fired: the evidence contradicts itself and the
/// response falls back to all bands.  This fallback is distinct from "nothing
/// exonerated", because it means something upstream is wrong, not that the arc
/// is noisy.
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
/// L1 and L2: the same narrowing, reached through the LLI clause.
TEST(CommonBandScopeRule, LliOnOneBandNarrowsToThatBand) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    e.lli_signals.insert("L5Q");
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
    EXPECT_NE(v.basis.find("lli=L5Q"), std::string::npos);
}

/// LLI on every band overrides every exoneration: a band the receiver reports
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
/// three fallbacks: it is not counted as a verdict, and the caller skips it.
TEST(CommonBandScopeRule, NoFlagIsNotAVerdict) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({ P("L1C", "L2W"), P("L1C", "L5Q"), P("L2W", "L5Q") }, {}),
        kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoFlag);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
}

/// A processed pair that also emitted an MwSlipResult is not silent.  The
/// emitted set overrides the detector's advisory per-sample flag, because the
/// emitted set is what the drains act on; a flag dropped at the admission gate
/// never reaches a drain but is still not silence.
TEST(CommonBandScopeRule, FlaggedPairNeverExoneratesEvenIfProcessedQuietly) {
    CommonBandEvidence e;
    // Processed, mature, and the per-sample flag says quiet, yet an MwSlipResult
    // was emitted for it (the drop-on-admission population).
    e.processed[P("L1C", "L2W")] = CommonBandProcessed{ kMatureN, false, kTightThrWl };
    e.flagged_pairs = { P("L1C", "L2W") };
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.exonerated.empty());
}

/// Two tracked bands only (the dual-frequency posture): the pair is the whole arc,
/// so a fired pair leaves nothing to exonerate and the response is the all-band one.
TEST(CommonBandScopeRule, DualFrequencyArcCannotNarrow) {
    const std::set<std::string> dual{ "L1C", "L2W" };
    const auto v = networkCommonBandVerdict(dual, ev({}, { P("L1C", "L2W") }),
                                            kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, dual);
}

/// A silent pair naming a band the station does not track cannot add it to the
/// verdict's band sets: the verdict is closed over the live bands.
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

/// The evidence is sound and would narrow, but the station anchors a family of
/// this satellite.  Narrowing there would hand over only the implicated band's
/// family and leave the rest pinned here, splitting the satellite's
/// per-frequency references across stations.  The response stays all-band.
TEST(CommonBandScopeRule, AnchorArcRefusesToNarrow) {
    const auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc,
                                            /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::AnchorColocate);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 1);
    // The unscoped response: every band the drains would have freed.
    EXPECT_EQ(v.implicated, kTriple);
    // The evidence itself is untouched; the datum topology is what refused.
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
}

/// The same evidence on a non-anchor arc narrows as without the clause, so the
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

/// The clause acts only where the evidence would have narrowed: an all-band
/// fallback on an anchor arc keeps its own reason, so the census separates
/// "nothing exonerated" from "refused for co-location".
TEST(CommonBandScopeRule, AnchorClauseDoesNotMaskOtherFallbacks) {
    const auto v = networkCommonBandVerdict(
        kTriple, ev({}, { P("L1C", "L2W"), P("L1C", "L5Q"), P("L2W", "L5Q") }),
        kMinArc, /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.anchor_colocate, 0);
}

// ------------------------- §14.10: the veto needs an IF-TD pair on every lane

// With the IF-TD detector forming one pair per satellite (L1C+L2W on GPS), a silent
// MW lane whose own IF pair was never evaluated has no veto behind it, and the
// equal-count trap is open on every lane but the first.  Storm-bed instance, station
// S37000SIM / G20: truth L5Q −31 cycles (receiver-flagged) then L1C −29; the
// (L1C+L5Q) wide lane moved 2 cycles and stayed silent, and the verdict read
//     keep implicated=L2W; exonerated=L1C;L5Q;
//     basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;
//           mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged
// It freed the one band that had not slipped, and the epoch's misfit was metres.
// The three cases below are that shape with and without the (L1C,L5Q) IF-TD
// evaluation, and the genuine single-band control that must survive the widening
// unchanged.

/// Case (i) of §14.10: the S37/G20 shape with the non-first IF-TD pair evaluated.
/// MW (1,2) and (2,5) fire; (1,5) is silent and mature; IF-TD (1,5) flags, because two bands
/// moving by −29 and −31 cycles leave the wide lane at 2 cycles while the iono-free
/// combination moves metres.  The silence is refused, nothing is exonerated, and the
/// response is the all-band one, with the veto named in the basis and attributed to
/// the pair that made it.
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

/// The control for (i), and the defect itself: the same MW evidence with the
/// (1,5) IF-TD pair unevaluated (as `if_phase_td_pairs = first` leaves it)
/// narrows to L2W, the one band that did not move.  This case must keep failing
/// to veto: it records the behaviour without the widening.
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

/// Case (ii) of §14.10, the pure equal-count trap on (1,5): L1C and L5Q both −29, so
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

/// Case (iii) of §14.10: the widening must be inert on a genuine single-band slip.  L2W
/// alone moves: MW (1,2) and (2,5) fire, (1,5) stays silent, and all three IF-TD
/// pairs are evaluated with only the two that contain L2 flagging.  The silent
/// lane's IF-TD pair is silent too, so it still exonerates and the verdict is the
/// §14.2 one, narrowed to exactly the slipped band.
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

// --------------------------------------- §22 rule: the reference-arc clause

// The third refusal, found on field data.  The veto above can fire only where the
// pooled IF-TD test can flag, and on the chain's reference satellite it cannot: that
// sample's double difference is identically 0 by construction
// (IfJumpInfo::is_reference), so `|dd − med| > thr` is structurally false however
// large the slip, and the pair reaches the rule looking like a clean silence.
// Field seed instance (SPEC_event_harness §20.1): an equal-count L1C+L5X slip on
// GOPE/G24, the reference of both L5 chains, narrowed to the unslipped band L2W under
// `if_phase_td_pairs` `first` and `all`, while the same injection at G25 (not a
// reference) was vetoed.
//
// The evidence carries such a pair as `iftd_untested`, populated only with
// `common_band_ref_arc` armed, and the rule refuses it as it refuses `immature` and
// `lli_shadow`: counted, named `iftd(<pair>)=ref_arc`, and never listed as `silent`.
//
// Signal spelling is mixed across these cases on purpose: L5X (the field triple,
// where the defect was measured) and L5Q (the sim triple the rest of the file uses).
// The rule is set-theoretic over signal names, so both must behave identically; a
// case that passed on only one spelling would be name-coupled.

/// The field triple: GOPE/G24's tracked bands on the DOY-129 golden.
const std::set<std::string> kFieldTriple{ "L1C", "L2W", "L5X" };

/// Case (a) of §22: the §20.1 trap itself.  An equal-count L1C+L5X slip leaves their wide
/// lane MW(1,5) silent by construction while (1,2) and (2,5) both fire, and the one
/// instrument that could see it, the (1,5) IF-TD pair, sits on this satellite's
/// reference arc and could not have flagged.  Armed, that pair is untested: nothing
/// exonerates and the response falls back to the all-band one, which is correct for
/// a slip that moved two bands.
TEST(CommonBandScopeRule, EqualCountL1L5OnTheReferenceArcFallsBackToAllBands) {
    auto e = ev({ P("L1C", "L5X") }, { P("L1C", "L2W"), P("L2W", "L5X") });
    e.iftd_untested.insert(P("L1C", "L5X"));   // G24 is the (1,5) chain's reference
    e.iftd_silent.insert(P("L1C", "L2W"));     // the (1,2) chain's reference is elsewhere
    const auto v = networkCommonBandVerdict(kFieldTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kFieldTriple);
    EXPECT_TRUE(v.exonerated.empty());
    EXPECT_EQ(v.iftd_ref_arc, 1);
    EXPECT_EQ(v.iftd_ref_arc_pairs, (std::set<CommonBandPair>{ P("L1C", "L5X") }));
    EXPECT_EQ(v.iftd_veto, 0);                 // no flag: the refusal is the absence of a test
    EXPECT_NE(v.basis.find("iftd(L1C+L5X)=ref_arc"), std::string::npos);
    // Named once: the refusal branch names it inline and the tail must not repeat
    // it (the `iftd_noted` contract the flagged tail already follows).
    std::size_t n = 0, at = 0;
    while ((at = v.basis.find("iftd(L1C+L5X)=ref_arc", at)) != std::string::npos) {
        ++n;
        ++at;
    }
    EXPECT_EQ(n, 1u);
}

/// Case (a′) of §22: the control, and the defect itself.  The same evidence with the
/// (1,5) sample not marked as a reference arc (i.e. the knob off, where every
/// evaluation folds into flagged/silent) narrows to L2W, the one band that did
/// not move.  This case must keep narrowing: it records the behaviour without the
/// clause.
TEST(CommonBandScopeRule, EqualCountL1L5OffTheReferenceArcNarrowsToTheWrongBand) {
    auto e = ev({ P("L1C", "L5X") }, { P("L1C", "L2W"), P("L2W", "L5X") });
    e.iftd_silent.insert(P("L1C", "L5X"));     // the pre-clause fold of the same sample
    e.iftd_silent.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandVerdict(kFieldTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L5X" }));
    EXPECT_EQ(v.iftd_ref_arc, 0);
    EXPECT_TRUE(v.iftd_ref_arc_pairs.empty());
}

/// Case (b) of §22: the clause is inert where a tested lane is silent.  A genuine L5 slip
/// on an arc that is the reference of both L5 chains fires (1,5) and (2,5); its
/// silent lane is (1,2), whose chain references a different satellite and is
/// therefore evaluated and silent.  It exonerates, and the verdict is the §14.2
/// one, narrowed to exactly the slipped band.  The two untested pairs cost nothing
/// here: they were MW-flagged and were never exoneration candidates.
TEST(CommonBandScopeRule, L5SlipOnTheL5ChainsReferenceStillNarrows) {
    auto e = ev({ P("L1C", "L2W") }, { P("L1C", "L5Q"), P("L2W", "L5Q") });
    e.iftd_silent.insert(P("L1C", "L2W"));
    e.iftd_untested.insert(P("L1C", "L5Q"));
    e.iftd_untested.insert(P("L2W", "L5Q"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L1C", "L2W" }));
    EXPECT_EQ(v.iftd_ref_arc, 0);              // the refused population is silent pairs only
    // The row still names them.  An untested pair the refusal branch never reached
    // (these two were MW-flagged) would otherwise vanish from the basis, and a
    // reader could not tell a quiet lane from an untestable one.
    EXPECT_NE(v.basis.find("iftd(L1C+L5Q)=ref_arc"), std::string::npos);
    EXPECT_NE(v.basis.find("iftd(L2W+L5Q)=ref_arc"), std::string::npos);
    EXPECT_EQ(v.basis.find("iftd(L1C+L5Q)silent"), std::string::npos);
}

/// Case (c) of §22: the named cost, accepted in advance (§22.2).  A genuine single-band
/// L1C slip on an arc that is the reference of the L5 chains fires (1,2) and
/// (1,5); its only silent lane is (2,5), which is one of the untested ones.  The
/// narrowing is lost and the response is the all-band one.  This is a real cost,
/// paid to close the equal-count hole, and it is counted so that a run's own
/// counters measure it.
TEST(CommonBandScopeRule, L1SlipOnTheReferenceArcPaysTheNamedCost) {
    auto e = ev({ P("L2W", "L5X") }, { P("L1C", "L2W"), P("L1C", "L5X") });
    e.iftd_untested.insert(P("L2W", "L5X"));
    e.iftd_untested.insert(P("L1C", "L5X"));
    e.iftd_silent.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandVerdict(kFieldTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kFieldTriple);
    EXPECT_EQ(v.iftd_ref_arc, 1);              // only the silent pair is refused
    EXPECT_EQ(v.iftd_ref_arc_pairs, (std::set<CommonBandPair>{ P("L2W", "L5X") }));
    EXPECT_NE(v.basis.find("iftd(L2W+L5X)=ref_arc"), std::string::npos);
    // The control: the same slip with the (2,5) lane tested narrows to L1C.
    auto e_off = ev({ P("L2W", "L5X") }, { P("L1C", "L2W"), P("L1C", "L5X") });
    e_off.iftd_silent.insert(P("L2W", "L5X"));
    const auto v_off = networkCommonBandVerdict(kFieldTriple, e_off, kMinArc);
    EXPECT_EQ(v_off.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v_off.implicated, (std::set<std::string>{ "L1C" }));
}

/// Case (d) of §22: an untested pair is never listed as `silent`.  The basis is the
/// evidence trail the event row carries, and listing a structural dead zone as a
/// silence there would assert what the clause denies.  The pair carries
/// `=ref_arc` instead, and the pre-clause spelling appears only on the control.
TEST(CommonBandScopeRule, UntestedPairIsNeverListedSilent) {
    auto e = ev({ P("L1C", "L5X") }, { P("L1C", "L2W"), P("L2W", "L5X") });
    e.iftd_untested.insert(P("L1C", "L5X"));
    const auto v = networkCommonBandVerdict(kFieldTriple, e, kMinArc);
    EXPECT_EQ(v.basis.find("iftd(L1C+L5X)silent"), std::string::npos);
    EXPECT_NE(v.basis.find("iftd(L1C+L5X)=ref_arc"), std::string::npos);
    EXPECT_EQ(v.basis.find("iftd(L1C+L5X)=flagged"), std::string::npos);

    // The control: the identical sample folded the pre-clause way is listed
    // `silent`, so the token comes from the clause.
    auto e_off = ev({ P("L1C", "L5X") }, { P("L1C", "L2W"), P("L2W", "L5X") });
    e_off.iftd_silent.insert(P("L1C", "L5X"));
    const auto v_off = networkCommonBandVerdict(kFieldTriple, e_off, kMinArc);
    EXPECT_NE(v_off.basis.find("iftd(L1C+L5X)silent"), std::string::npos);
    EXPECT_EQ(v_off.basis.find("=ref_arc"), std::string::npos);
}

/// The §22 off path, stated as a test rather than inferred from the loader: an empty
/// untested set leaves the verdict, the counters and the basis exactly where the
/// pre-clause rule left them.  With the knob off the preview never populates that
/// set, so this is the byte-identity claim in unit form.
TEST(CommonBandScopeRule, EmptyUntestedSetLeavesEveryVerdictUnchanged) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") });
    e.iftd_flagged.insert(P("L1C", "L5Q"));
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_TRUE(e.iftd_untested.empty());
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);   // the §14.10 veto, intact
    EXPECT_EQ(v.iftd_veto, 1);
    EXPECT_EQ(v.iftd_ref_arc, 0);
    EXPECT_TRUE(v.iftd_ref_arc_pairs.empty());
    EXPECT_NE(v.basis.find("iftd(L1C+L5Q)=flagged"), std::string::npos);
}

// ------------------------------- §14.11 rule F2: the LLI shadow on a silent lane

// The second half of the same field defect.  The uncombined LLI branch of
// MwSlipDetector::detect schedules the ambiguity resets and `continue`s, deliberately
// preserving the MW arc: the MW combination is a function of raw observations alone,
// so a short lock loss does not invalidate the EWMA.  For this rule the consequence is
// that the arc count runs straight through the flagged epoch, and the first sample
// after it reads "mature" although the lane saw nothing at the epoch the receiver
// declared broken.  On the storm bed every spurious `keep` verdict sat one epoch after
// an LLI, and in every one the exonerating lane contained the flagged band (S37/G20
// (L1C+L5Q) n 87→99 across the LLI, thr 9.0 WL; S25/E36 (L1C+L5Q) n 164→173).

/// Case (a) of §14.11: the S37/G20 shape with the (1,5) lane LLI-shadowed and the IF-TD
/// detector still at `first`, i.e. no IF-TD pair behind that lane.  F2 alone closes
/// it: the shadowed lane cannot exonerate, nothing else can, and the response is the
/// all-band one.
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
    EXPECT_EQ(v.iftd_veto, 0);       // the shadow refused the lane before any veto
    EXPECT_NE(v.basis.find("mw(L1C+L5Q)lli_shadow(n=99)"), std::string::npos);
}

/// Case (b) of §14.11, the Galileo shape: an LLI on L5Q at t−1, and at t the lanes that
/// contain L7Q fire late on the already-answered slip while (1,5) sits silent.
/// Without F2 that narrows to L7Q, an unslipped band.  With it, nothing narrows.
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

/// Case (c) of §14.11: the shadow is band-scoped, which keeps the rule cheap.  The common
/// population: an LLI on L5Q at t−1, and at t the two lanes that contain L5Q fire
/// late on the already-answered slip while (L1C, L2W), the one lane carrying no
/// flagged band and so not shadowed, is silent and mature.  It still exonerates,
/// and the verdict implicates exactly the band the receiver reported.  F2 must not
/// cost this narrowing.
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

/// The converse, so the band scope is pinned in both directions: with the same
/// shadow, a genuine L1C slip at t leaves only shadowed or flagged lanes.  (1,2)
/// fires, (1,5) fires, and the silent (2,5) contains the flagged band, so nothing
/// exonerates and the response is the all-band one.  The narrowing that would
/// have pinned L2W here is the spurious one the census found.
TEST(CommonBandScopeRule, LliShadowRefusesTheOnlySilentLaneWhenItCarriesTheBand) {
    auto e = ev({ P("L2W", "L5Q") }, { P("L1C", "L2W"), P("L1C", "L5Q") });
    shadowBand(e, "L5Q");
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::NoExoneration);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_EQ(v.lli_shadow, 1);
    EXPECT_NE(v.basis.find("mw(L2W+L5Q)lli_shadow"), std::string::npos);
}

/// An unshadowed run of the same evidence as (a) narrows: the control showing
/// that F2 changed the verdict, not the thresholds or the maturity.
TEST(CommonBandScopeRule, WithoutTheShadowTheSameEvidenceStillNarrows) {
    auto e = ev({ P("L1C", "L5Q") }, { P("L1C", "L2W"), P("L2W", "L5Q") },
                /*n=*/99, /*thr_wl=*/9.0);
    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L2W" }));
    EXPECT_EQ(v.lli_shadow, 0);
}

// ------------------------------- §14.12 rule F3: the LLI-epoch verdict

// The epoch the §14.2 rule cannot see.  The uncombined LLI branch withholds every MW
// sample of a flagged satellite (§14.11), so no pair can flag, the normal path forms
// no verdict, and without a verdict the drains run unscoped.  That is harmless while
// the LLI drain is the only actor there (it frees exactly the bands the receiver
// flagged), but an IF-TD pair containing a flagged band can fire in the same epoch
// and free both of its bands, turning the LLI epoch into an all-band free (measured
// on the §14.10 S3 leg as a level-residual and slant-iono excursion, with the next
// epoch's verdict unchanged).  F3 takes the receiver's bits as the scope, with no
// exoneration step; the LLI drain already trusts them.

/// Case (a) of §14.12, the S3 shape: LLI on L5Q, every MW sample withheld, the (1,5) and
/// (2,5) IF-TD pairs flagged.  The verdict implicates exactly L5Q, so the pair
/// refloats free L5Q alone instead of all three bands.
TEST(CommonBandScopeRule, LliEpochVerdictScopesToTheFlaggedBand) {
    CommonBandEvidence e;                       // no processed pairs: all withheld
    e.lli_signals.insert("L5Q");
    e.iftd_flagged.insert(P("L1C", "L5Q"));
    e.iftd_flagged.insert(P("L2W", "L5Q"));
    const auto v = networkCommonBandLliVerdict(kTriple, e);
    EXPECT_EQ(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_TRUE(v.exonerated.empty());          // nothing was tested
    EXPECT_EQ(v.basis, "lli=L5Q;mw=withheld");

    // The drain hook is the contract the IF-TD pair refloats obey: with this
    // verdict registered, (L1C,L5Q) frees L5Q and keeps L1C pinned.
    CommonBandEpoch ep;
    ep.armed = true;
    ep.narrowed_at[{ 3, "G08" }] = v;
    EXPECT_FALSE(commonBandMayFree(&ep, 3, "G08", "L1C"));
    EXPECT_FALSE(commonBandMayFree(&ep, 3, "G08", "L2W"));
    EXPECT_TRUE(commonBandMayFree(&ep, 3, "G08", "L5Q"));
}

/// Case (b) of §14.12: the same under `if_phase_td_pairs = first`, where the only IF-TD
/// pair is (L1C, L2W): an LLI on L1C scopes that drain too, which unscoped frees
/// both of its bands.  F3 is not an `all`-only rule.
TEST(CommonBandScopeRule, LliEpochVerdictScopesTheFirstPairDrainToo) {
    CommonBandEvidence e;
    e.lli_signals.insert("L1C");
    e.iftd_flagged.insert(P("L1C", "L2W"));
    const auto v = networkCommonBandLliVerdict(kTriple, e);
    EXPECT_EQ(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L1C" }));

    CommonBandEpoch ep;
    ep.armed = true;
    ep.narrowed_at[{ 0, "G08" }] = v;
    EXPECT_TRUE(commonBandMayFree(&ep, 0, "G08", "L1C"));
    EXPECT_FALSE(commonBandMayFree(&ep, 0, "G08", "L2W"));
}

/// Case (c) of §14.12: F3 must not override the normal path.  An LLI on L5Q with MW (1,2)
/// flagged in the same epoch (a second L1C slip without an LLI bit) is evidence the
/// §14.2 rule can weigh, and that rule already implicates every LLI band.  The
/// caller's guard is `flagged_pairs.empty()`, so this case never reaches F3; the
/// case asserts the guard and the normal-path verdict.
TEST(CommonBandScopeRule, LliEpochVerdictDoesNotOverrideARealFlag) {
    auto e = ev({ P("L2W", "L5Q") }, { P("L1C", "L2W") });
    e.lli_signals.insert("L5Q");
    ASSERT_FALSE(e.flagged_pairs.empty());      // F3's guard: the normal path runs

    const auto v = networkCommonBandVerdict(kTriple, e, kMinArc);
    EXPECT_NE(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_NE(v.basis.find("mw(L2W+L5Q)silent"), std::string::npos);
    // The silent lane clears L2W and L5Q, but the LLI bit re-implicates L5Q, so
    // the narrowing is {L1C, L5Q}: the slipped band and the flagged one.
    EXPECT_EQ(v.outcome, CommonBandOutcome::Narrowed);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L1C", "L5Q" }));
    EXPECT_EQ(v.exonerated, (std::set<std::string>{ "L2W" }));
}

/// A flagged band that is not tracked leaves nothing to scope: `implicated` must
/// stay the full set so a caller registering the verdict cannot restrict a drain
/// by accident, and `narrowed` must be false so it is not registered at all.
TEST(CommonBandScopeRule, LliEpochVerdictOnANonLiveBandScopesNothing) {
    CommonBandEvidence e;
    e.lli_signals.insert("L7Q");                // not in kTriple
    const auto v = networkCommonBandLliVerdict(kTriple, e);
    EXPECT_EQ(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
}

/// Every live band flagged: the scope equals the all-band response, so nothing
/// is narrowed and no drain is gated.
TEST(CommonBandScopeRule, LliEpochVerdictOnEveryBandIsTodaysResponse) {
    CommonBandEvidence e;
    for (const auto& b : kTriple) e.lli_signals.insert(b);
    const auto v = networkCommonBandLliVerdict(kTriple, e);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.implicated, kTriple);
}

// The anchor clause applies to F3 too, per §14.12.5 of the spec.  Without it, an F3
// verdict on an anchor arc hands over only the implicated band's family and splits
// the satellite's per-frequency references across stations (the §14.5 defect,
// entered through the LLI path): on the storm arm the split-referenced satellite
// share and the single-family anchor handovers roughly doubled, while the normal
// path's own clause kept its `keep` rows at zero single-family handovers.  The
// §14.10 S3 leg could not show it because S21000SIM anchors nothing.

/// (a) F3 on an arc that anchors a family of the satellite: every band is freed, so
/// all of the satellite's families move together to one successor.
TEST(CommonBandScopeRule, LliEpochVerdictOnAnAnchorArcRefusesToNarrow) {
    CommonBandEvidence e;
    e.lli_signals.insert("L5Q");
    const auto v = networkCommonBandLliVerdict(kTriple, e,
                                               /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::AnchorColocate);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 1);
    EXPECT_EQ(v.implicated, kTriple);
    EXPECT_TRUE(v.lli_epoch);                       // the row must name the rule
    // The evidence that would have narrowed is still on the row: the datum topology
    // refuses to act on it; the evidence is not changed.
    EXPECT_EQ(v.basis, "lli=L5Q;mw=withheld");

    // No drain is gated, so the LLI free and every IF-TD pair behave as unscoped.
    CommonBandEpoch ep;
    ep.armed = true;
    if (v.narrowed) ep.narrowed_at[{ 7, "G04" }] = v;
    for (const auto& b : kTriple)
        EXPECT_TRUE(commonBandMayFree(&ep, 7, "G04", b)) << b;
}

/// (b) The same evidence on a non-anchor arc is unchanged: `keep`, with implicated
/// equal to the flagged bands.  The clause must cost nothing off an anchor.
TEST(CommonBandScopeRule, LliEpochVerdictOffAnAnchorArcIsUnchanged) {
    CommonBandEvidence e;
    e.lli_signals.insert("L5Q");
    const auto v = networkCommonBandLliVerdict(kTriple, e,
                                               /*station_anchors_satellite=*/false);
    EXPECT_EQ(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_TRUE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 0);
    EXPECT_EQ(v.implicated, (std::set<std::string>{ "L5Q" }));
    EXPECT_TRUE(v.lli_epoch);
}

/// The clause fires only on a verdict that would have narrowed.  An LLI on a band
/// that is not tracked narrows nothing, so an anchor arc must not be counted as an
/// anchor co-locate; the counter has to size the real population.
TEST(CommonBandScopeRule, LliEpochAnchorClauseDoesNotFireOnANullNarrowing) {
    CommonBandEvidence e;
    e.lli_signals.insert("L7Q");                    // not in kTriple
    const auto v = networkCommonBandLliVerdict(kTriple, e,
                                               /*station_anchors_satellite=*/true);
    EXPECT_EQ(v.outcome, CommonBandOutcome::LliOnly);
    EXPECT_FALSE(v.narrowed);
    EXPECT_EQ(v.anchor_colocate, 0);
    EXPECT_EQ(v.implicated, kTriple);
}

// ------------------------------------------- §14.5 split-referenced satellites

/// Two satellites, one whose families are anchored at two different stations.
/// The split count is the instrument for the storm-bed split-reference
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
/// frees without the rule.
TEST(CommonBandScopeHook, NullVerdictSetFreesEverything) {
    EXPECT_TRUE(commonBandMayFree(nullptr, 3, "G08", "L1C"));
    EXPECT_TRUE(commonBandMayFree(nullptr, 3, "G08", "L5Q"));
}

/// An unarmed epoch object (the census ran, the knob is off) must behave exactly
/// like the null pointer, so a wiring error that passes it anyway cannot change
/// the solve.
TEST(CommonBandScopeHook, UnarmedEpochFreesEverything) {
    CommonBandEpoch cbs;
    cbs.armed = false;
    CommonBandVerdict v;
    v.narrowed   = true;
    v.implicated = { "L5Q" };
    cbs.narrowed_at[{ 3, "G08" }] = v;
    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G08", "L1C"));
}

/// Armed and narrowed: the hook frees the implicated band and refuses the rest,
/// for that (station, satellite) only.  Every other key is untouched, which keeps
/// a narrowed verdict from leaking across arcs.
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

/// clear() must drop the verdicts and the census, or the next epoch inherits a
/// stale narrowing; the rule rests on this per-epoch contract.
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
    cbs.iftd_ref_arc = 7;                                     // §22
    cbs.iftd_ref_arc_by_pair["L1C_L5X"] = 7;

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
    EXPECT_EQ(cbs.iftd_ref_arc, 0);                           // §22
    EXPECT_TRUE(cbs.iftd_ref_arc_by_pair.empty());
    EXPECT_TRUE(commonBandMayFree(&cbs, 3, "G08", "L1C"));
}
