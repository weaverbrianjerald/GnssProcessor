/**
 * @file test_mw_evidence_refloat.cpp
 * @brief Tests for the MW evidence refloat (network.cycle_slip.mw_evidence_refloat):
 *        the PASS-A admission rule and the detector-side strength export it reads.
 *
 * Without the knob, PASS A admits an MW refloat candidate only on the T0
 * integer-repair gate (a known non-zero integer whose rounding residual is within
 * network.cycle_slip.turbo_edit_frac_max), although the path is detect-only: its
 * consumer, the refloat drain, never applies the integer.  Rounding cleanliness does
 * not decide whether a slip happened.  On that gate alone, a -99 cycle L5 slip flagged
 * at 78x its own adaptive threshold, integer known, residual 0.183 cycles (1.07 m on
 * the 5.86 m wide lane, inside that lane's own 1 sigma) is refused, and so is the same
 * event on a second lane at 8.9x with an unknown integer.  The test values below come
 * from that event.
 *
 * Two gate families, matching the two halves of the build:
 *   - MwEvidenceAdmit: the pure rule (@ref networkMwAdmitCandidate).  Legacy
 *     behaviour is bit-unchanged, the evidence half acts only when armed, and each
 *     of its four refusals (BIA lane, immature arc, weak ratio, no ratio published)
 *     refuses.
 *   - MwEvidenceDetectorExport: @c MwSlipDetector publishes the ratio, the arc
 *     maturity and the lane class on its principal emission path.  A rule with
 *     nothing to read is dead wiring (invariant #4), and the fields default to
 *     NaN / 0 / false, which is what a silent detector looks like to the rule.
 *
 * Pure unit gates: a hand-fed detector, no estimator, no network.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/NetworkSlipQc.hpp"

using namespace GnssProcessor;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Production defaults of the three knobs, so a test that changes one says so.
constexpr double kFracMax  = 0.15;   // network.cycle_slip.turbo_edit_frac_max
constexpr double kRatioMin = 3.0;    // network.cycle_slip.mw_evidence_ratio_min
constexpr int    kMinArc   = 10;     // network.cycle_slip.mw_evidence_min_arc_epochs

/// The rule at production gate values; only the per-result facts and the knob vary.
MwAdmitDecision admit(bool wl_known, int dn, double frac,
                      double ratio, int n_arc, bool bia, bool knob_on) {
    return networkMwAdmitCandidate(wl_known, dn, frac, kFracMax,
                                   ratio, n_arc, bia,
                                   knob_on, kRatioMin, kMinArc);
}

// ---------------------------------------------------------------------------------
// Detector fixture: a GPS satellite with both phase and code per band.  The MW
// combination needs the code, so a phase-only observation forms no arc at all.  No
// BIA product is loaded, so these arcs run the undifferenced (bias-free) path: they
// never integer-lock, and every flag leaves by the unlocked-arc emission site.
// ---------------------------------------------------------------------------------
PppObservation mwObs(const std::string& sat, double tow,
                     const std::map<std::string, double>& obs_map,
                     double elev_deg = 45.0) {
    PppObservation o{};
    o.satellite    = sat;
    o.system       = System::GPS;
    o.time         = GnssTime{ 2400, tow };
    o.elevation    = elev_deg;
    o.azimuth      = 0.0;
    o.raw_observed = obs_map;
    o.observed     = obs_map;
    return o;
}

/// Bias-free GPS L1/L2 pair: phase in cycles, code in metres.
std::map<std::string, double> dualGps(double l1c, double l2c, double rho_m) {
    return { { "L1C", l1c }, { "L2W", l2c },
             { "C1W", rho_m }, { "C2W", rho_m } };
}

}  // namespace

// =================================================================================
// MwEvidenceAdmit: the pure PASS-A admission rule.
// =================================================================================

// The legacy gate, unchanged and independent of the knob: a known non-zero integer
// whose residual is inside turbo_edit_frac_max is admitted.  The evidence
// bookkeeping does not claim it: via_evidence is the `src=evidence` population and a
// legacy admission is not part of it.
TEST(MwEvidenceAdmit, LegacyIntegerGateAdmitsWithAndWithoutTheKnob) {
    for (bool knob : { false, true }) {
        const auto d = admit(/*wl_known=*/true, /*dn=*/-99, /*frac=*/0.02,
                             /*ratio=*/78.0, /*n_arc=*/40, /*bia=*/false, knob);
        EXPECT_TRUE(d.admit_legacy) << "knob=" << knob;
        EXPECT_TRUE(d.admitted)     << "knob=" << knob;
        EXPECT_FALSE(d.via_evidence) << "a legacy admission is not an evidence one";
        EXPECT_FALSE(d.would_admit)  << "would_admit counts only what legacy REFUSED";
        EXPECT_FALSE(d.drop_loose);
        EXPECT_FALSE(d.drop_unknown);
    }
}

// The measured miss, knob off: integer known, residual 0.183 > 0.15, flagged at 78x
// threshold on a mature bias-free lane.  It is refused, and the census counts the
// refusal (drop_loose) and the would-admit verdict.
TEST(MwEvidenceAdmit, LooseKnownIntegerIsDroppedAndCountedWithKnobOff) {
    const auto d = admit(/*wl_known=*/true, /*dn=*/99, /*frac=*/0.183,
                         /*ratio=*/78.0, /*n_arc=*/40, /*bia=*/false,
                         /*knob_on=*/false);
    EXPECT_FALSE(d.admit_legacy);
    EXPECT_FALSE(d.admitted);
    EXPECT_TRUE(d.drop_loose);
    EXPECT_FALSE(d.drop_unknown);
    // The rule itself is evaluated with the knob off; that evaluation is the
    // would-admit census, which sizes the card's effect before it is armed.
    EXPECT_TRUE(d.evidence_ok);
    EXPECT_TRUE(d.would_admit);
    EXPECT_FALSE(d.admit_evidence) << "the knob is off: the rule must not act";
}

// The same result with the knob armed: admitted, and marked as the evidence
// population so its event row can carry src=evidence.
TEST(MwEvidenceAdmit, LooseKnownIntegerIsAdmittedViaEvidenceWithKnobOn) {
    const auto d = admit(/*wl_known=*/true, /*dn=*/99, /*frac=*/0.183,
                         /*ratio=*/78.0, /*n_arc=*/40, /*bia=*/false,
                         /*knob_on=*/true);
    EXPECT_FALSE(d.admit_legacy);
    EXPECT_TRUE(d.admit_evidence);
    EXPECT_TRUE(d.admitted);
    EXPECT_TRUE(d.via_evidence);
    EXPECT_FALSE(d.would_admit) << "would_admit is the knob-OFF counter only";
    EXPECT_FALSE(d.drop_loose)  << "admitted results are in no drop bucket";
}

// The second lane of the same measured event: 8.9x threshold, integer unknown.
// The legacy gate cannot admit it (it has no integer to grade); the evidence rule
// can, because the consumer does not use the integer.
TEST(MwEvidenceAdmit, UnknownIntegerIsDroppedOffAndAdmittedOn) {
    const auto off = admit(/*wl_known=*/false, /*dn=*/0, /*frac=*/0.5,
                           /*ratio=*/8.9, /*n_arc=*/40, /*bia=*/false, false);
    EXPECT_FALSE(off.admitted);
    EXPECT_TRUE(off.drop_unknown);
    EXPECT_FALSE(off.drop_loose) << "unknown and loose are disjoint populations";
    EXPECT_TRUE(off.would_admit);

    const auto on = admit(/*wl_known=*/false, /*dn=*/0, /*frac=*/0.5,
                          /*ratio=*/8.9, /*n_arc=*/40, /*bia=*/false, true);
    EXPECT_TRUE(on.admitted);
    EXPECT_TRUE(on.via_evidence);
    EXPECT_FALSE(on.drop_unknown);
}

// A known, clean zero integer is not a slip: no admission, and it belongs in
// neither drop bucket (nothing was detected, so nothing was refused).
TEST(MwEvidenceAdmit, KnownZeroCleanIntegerIsNeitherAdmittedNorCountedAsADrop) {
    const auto d = admit(/*wl_known=*/true, /*dn=*/0, /*frac=*/0.01,
                         /*ratio=*/kNaN, /*n_arc=*/40, /*bia=*/false, true);
    EXPECT_FALSE(d.admitted);
    EXPECT_FALSE(d.drop_loose);
    EXPECT_FALSE(d.drop_unknown);
}

// The four refusals of the evidence half, each isolated: a single fact is spoiled
// and the rule refuses on it even with the knob armed.
TEST(MwEvidenceAdmit, EvidenceRefusesBiaLaneImmatureWeakAndUnpublishedRatio) {
    // BIA-corrected lane: single-differenced against the detector's own MW pivot,
    // so a pivot S-transform re-reference presents as a synchronous multi-satellite
    // step.  That is the measured false-alarm population, so the class is refused.
    const auto bia = admit(true, 99, 0.183, /*ratio=*/78.0, /*n_arc=*/40,
                           /*bia=*/true, /*knob_on=*/true);
    EXPECT_FALSE(bia.evidence_ok);
    EXPECT_FALSE(bia.admitted);
    EXPECT_TRUE(bia.drop_loose);

    // Immature arc: one sample below the maturity floor is still a refusal.
    const auto young = admit(true, 99, 0.183, 78.0, /*n_arc=*/kMinArc - 1,
                             false, true);
    EXPECT_FALSE(young.evidence_ok);
    EXPECT_FALSE(young.admitted);

    // Weak flag: just under the ratio gate.
    const auto weak = admit(true, 99, 0.183, /*ratio=*/kRatioMin - 0.01,
                            40, false, true);
    EXPECT_FALSE(weak.evidence_ok);
    EXPECT_FALSE(weak.admitted);

    // No ratio published by the emitting path: NaN refuses.  Every NaN comparison
    // is false, so the >= alone would already refuse, but the isfinite check states
    // the intent and is pinned here.
    const auto nan_r = admit(true, 99, 0.183, /*ratio=*/kNaN, 40, false, true);
    EXPECT_FALSE(nan_r.evidence_ok);
    EXPECT_FALSE(nan_r.admitted);
    EXPECT_TRUE(nan_r.drop_loose);
}

// The boundaries are inclusive on both evidence gates (>=, >=), and the legacy frac
// gate is inclusive (<=).
TEST(MwEvidenceAdmit, GateBoundariesAreInclusive) {
    EXPECT_TRUE(admit(true, 99, 0.183, kRatioMin, kMinArc, false, true).evidence_ok);
    EXPECT_TRUE(admit(true, 5, kFracMax, kNaN, 0, false, false).admit_legacy);
}

// =================================================================================
// MwEvidenceDetectorExport: MwSlipDetector publishes what the rule reads.
// =================================================================================

// The principal emission path (unlocked arc, bias-free lane).  Drive a quiet arc,
// then step the second band by 40 cycles: dN_WL = N1 - N2 moves by -40, i.e. ~34 m
// on the 0.862 m wide lane, far above any adaptive threshold.  The flag carries a
// finite ratio well past the production gate, the arc's sample count at the flag
// (read before the erase), and the lane class that keeps it eligible.
TEST(MwEvidenceDetectorExport, UnlockedFlagPublishesRatioArcEpochsAndLaneClass) {
    LogManager log;
    GnssConfig cfg;
    MwSlipDetector det(cfg, log);

    constexpr int    kWarm = 30;
    constexpr double kTow0 = 200000.0;
    constexpr double kL1   = 1.0e7, kL2 = 0.8e7, kRho = 2.2e7;

    for (int k = 0; k < kWarm; ++k) {
        const auto res = det.detectLargeMwResets(
            { mwObs("G01", kTow0 + k, dualGps(kL1, kL2, kRho)) });
        ASSERT_TRUE(res.empty()) << "spurious flag on quiet epoch " << k;
    }

    const auto res = det.detectLargeMwResets(
        { mwObs("G01", kTow0 + kWarm, dualGps(kL1, kL2 + 40.0, kRho)) });
    ASSERT_FALSE(res.empty()) << "the 40-cycle L2 step must flag";

    const auto& r = res.front();
    std::cout << "[mw-evidence] sig1=" << r.sig1 << " sig2=" << r.sig2
              << " evidence_ratio=" << r.evidence_ratio
              << " arc_epochs=" << r.arc_epochs
              << " bia_corrected=" << r.bia_corrected
              << " slip_wl_known=" << r.slip_wl_known
              << " slip_wl_cycles=" << r.slip_wl_cycles
              << " slip_frac=" << r.slip_frac << std::endl;

    EXPECT_FALSE(r.bia_corrected) << "no BIA product is loaded: this lane is bias-FREE";
    ASSERT_TRUE(std::isfinite(r.evidence_ratio))
        << "the unlocked emission path must publish a ratio — a NaN here is the "
           "dead-wiring case the rule cannot distinguish from a weak flag";
    EXPECT_GT(r.evidence_ratio, kRatioMin)
        << "a 34 m step on a 0.86 m lane is not a marginal detection";
    EXPECT_EQ(r.arc_epochs, kWarm)
        << "arc maturity is the count AT THE FLAG, before the erase";

    // End to end: this detector result, fed to the rule, is admitted by the
    // evidence half with the knob on and refused by it with the knob off.
    const auto on = admit(r.slip_wl_known, r.slip_wl_cycles, r.slip_frac,
                          r.evidence_ratio, r.arc_epochs, r.bia_corrected, true);
    EXPECT_TRUE(on.admitted);
    const auto off = admit(r.slip_wl_known, r.slip_wl_cycles, r.slip_frac,
                           r.evidence_ratio, r.arc_epochs, r.bia_corrected, false);
    EXPECT_EQ(off.admitted, off.admit_legacy)
        << "with the knob off, admission is the legacy gate and nothing else";
}

// A quiet arc publishes nothing: no result rows at all, so the fields never reach
// the rule.  This complements the gate above and pins that the export is a property
// of flags, not a per-epoch side channel.
TEST(MwEvidenceDetectorExport, QuietArcEmitsNoResultsAtAll) {
    LogManager log;
    GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    for (int k = 0; k < 40; ++k)
        EXPECT_TRUE(det.detectLargeMwResets(
            { mwObs("G02", 300000.0 + k, dualGps(1.0e7, 0.8e7, 2.2e7)) }).empty())
            << "epoch " << k;
}

// The struct's own defaults are the "this path published no strength" contract the
// rule leans on: a default-constructed result is refused by the evidence half.
TEST(MwEvidenceDetectorExport, DefaultResultCarriesNoEvidenceAndIsRefused) {
    MwSlipResult r;
    EXPECT_FALSE(std::isfinite(r.evidence_ratio));
    EXPECT_EQ(r.arc_epochs, 0);
    EXPECT_FALSE(r.bia_corrected);
    EXPECT_FALSE(admit(r.slip_wl_known, r.slip_wl_cycles, r.slip_frac,
                       r.evidence_ratio, r.arc_epochs, r.bia_corrected,
                       /*knob_on=*/true).admitted);
}
