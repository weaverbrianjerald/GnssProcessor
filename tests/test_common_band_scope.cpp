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
