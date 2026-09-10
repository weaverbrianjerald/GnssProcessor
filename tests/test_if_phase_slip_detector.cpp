/**
 * @file test_if_phase_slip_detector.cpp
 * @brief First dedicated unit coverage of IfPhaseSlipDetector — the
 *        between-satellite / between-epoch IF phase slip detector — with the
 *        2026-07-21 Card-R reference-gate hardening
 *        (docs/SPEC_slip_qc_hardening.md §2, MUST R7) as the focus.
 *
 * The whole chain had ZERO dedicated tests before this file (spec rule 7).
 * Everything here drives the PUBLIC detect() API with synthetic uncombined
 * (PPP-RTK) observation streams: each PppObservation carries phase on two
 * bands (L1C, L2W) in both @c observed and @c modeled, and the detector
 * synthesizes the IF OMC itself.  Because the IF coefficients satisfy
 * c1 + c2 == 1 exactly, giving BOTH bands the same observed-minus-modeled
 * value makes the detector's internal IF OMC equal that value — so every
 * double difference below is a DESIGNED quantity, not a derived one, and
 * every threshold crossing is a designed margin rather than a coin flip.
 *
 * Unless a case says otherwise the detector runs with setDeferFlagging(true):
 * that is the NETWORK wiring Card R hardens, and in that mode ONLY the
 * reference-slip branch pushes reset keys, so a nonempty return from detect()
 * is unambiguous evidence about the reference gate.  The self-contained
 * (user/kinematic) mode required by MUST R5 has its own case at the end.
 *
 * Card-R cases (spec R7 names them verbatim):
 *   - legacy / gate-OFF byte behaviour (the byte-identity guard),
 *   - step-fires-once,
 *   - drift-does-not-refire (the audit's 9-consecutive-epoch pattern),
 *   - thin-family defer (both directions),
 *   - reference-handover offset correctness,
 *   - persistence escalation -> forced reference re-selection.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/IfPhaseSlipDetector.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
// Synthetic stream geometry (GPS L1C / L2W)
// ---------------------------------------------------------------------------

constexpr double kF1 = Constants::GPS_L1_FREQ;
constexpr double kF2 = Constants::GPS_L2_FREQ;

/// IF wavelength |c1·λ1 − c2·λ2| with c_i = f_i²/(f1² − f2²): ≈ 0.10695 m for
/// GPS L1/L2.  This is the detector's PHYSICAL floor — the legacy reference
/// gate fires at |median(non-ref ΔΔ)| > floor_frac·λ_IF = 0.5·λ_IF ≈ 5.35 cm.
const double kLambdaIf =
    std::abs((kF1 * kF1 / (kF1 * kF1 - kF2 * kF2)) * (Constants::SPEED_OF_LIGHT / kF1)
           - (kF2 * kF2 / (kF1 * kF1 - kF2 * kF2)) * (Constants::SPEED_OF_LIGHT / kF2));

/// The legacy fire threshold (and the armed gate's physical floor).
const double kRefFloorM = 0.5 * kLambdaIf;

/// The ambiguity reset key the reference branch emits for our synthetic pair
/// (`<ref sat>_<phase IF key>`, and the uncombined path synthesizes the key
/// as "sig1_sig2").
std::string refKey(const std::string& sat) { return sat + "_L1C_L2W"; }

/// Deterministic sub-millimetre phase wobble (m).  Keeps the non-reference
/// family's dispersion non-degenerate — a MAD of exactly zero would make the
/// Card-R coherence test vacuous — while staying three orders of magnitude
/// below the 5.35 cm physical floor, so it can never move a verdict.
constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };
double wobble(std::size_t epoch, std::size_t sat) {
    return kWobbleM[(epoch * 3u + sat) % 8u];
}

/// Per-satellite IF OMC state (metres); the tests mutate it between epochs.
using OmcState = std::map<std::string, double>;
/// Per-satellite elevation (degrees).
using ElevState = std::map<std::string, double>;

/**
 * @brief Build one epoch of uncombined observations from designed IF OMCs.
 *
 * The model value is zero on both bands and the observed value carries the
 * whole OMC: the detector consumes only (observed − modeled), so a large
 * common base would add float cancellation noise and buy nothing.
 */
std::vector<PppObservation> makeEpoch(double tow, std::size_t epoch_idx,
                                      const OmcState& omc_m,
                                      const ElevState& elev_deg) {
    std::vector<PppObservation> epoch;
    epoch.reserve(omc_m.size());
    std::size_t k = 0;
    for (const auto& [sat, omc] : omc_m) {
        const double v = omc + wobble(epoch_idx, k++);
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2400, tow };
        obs.elevation = elev_deg.at(sat);
        obs.azimuth   = 0.0;
        obs.los       = Eigen::Vector3d(0.0, 0.0, 1.0);
        obs.sat_pos   = Eigen::Vector3d::Zero();
        obs.rx_pos_linearization = Eigen::Vector3d::Zero();
        for (const char* sig : { "L1C", "L2W" }) {
            obs.modeled[sig]  = 0.0;
            obs.observed[sig] = v;   // same OMC on both bands => IF OMC == v
        }
        epoch.push_back(std::move(obs));
    }
    return epoch;
}

/// Card-R parameters as the network arms them: spec defaults, gate ON.
/// The header defaults ARE the shipped config defaults (pinned by
/// HeaderDefaultsMatchTheShippedConfigDefaults below), so arming nothing else
/// keeps every case here testing the gate the field actually runs.
IfPhaseSlipDetector::RefGateParams armedGate() {
    IfPhaseSlipDetector::RefGateParams p;   // k=6, min_n=4, thin_widen=3,
    p.enable = true;                        // coherence=0.5, max_consecutive=1
    return p;
}

/// Detector under test plus the config/log it borrows.  Member order keeps the
/// references alive for the detector.
struct IfHarness {
    LogManager log_manager;
    GnssConfig config;                      // defaults: 10 deg elevation mask
    IfPhaseSlipDetector det{ config, log_manager };

    explicit IfHarness(bool defer_flagging = true) {
        det.setDeferFlagging(defer_flagging);
    }

    /// Push one epoch through detect() and return the reset keys it produced.
    std::vector<std::string> step(std::size_t epoch_idx, const OmcState& omc,
                                  const ElevState& elev) {
        return det.detect(makeEpoch(30.0 * static_cast<double>(epoch_idx),
                                    epoch_idx, omc, elev));
    }
};

/// A six-satellite GPS family: G01 is the only satellite above the detector's
/// 45 deg reference-retention threshold, so it is elected once and retained —
/// which makes the reference identity a controlled variable.
ElevState sixSatElevations() {
    return { { "G01", 80.0 }, { "G02", 70.0 }, { "G03", 40.0 },
             { "G04", 35.0 }, { "G05", 30.0 }, { "G06", 25.0 } };
}

OmcState sixSatOmc() {
    return { { "G01", 0.00 }, { "G02", 0.12 }, { "G03", -0.07 },
             { "G04", 0.21 }, { "G05", -0.15 }, { "G06", 0.05 } };
}

/// Every HARDENED-GATE Card-R counter is zero (the gate never ran).
///
/// @warning @c gate_opportunities / @c gate_flags are DELIBERATELY absent here.
/// They are the tier-T1r flag-rate pair and count UNCONDITIONALLY — the legacy
/// reference condition runs and acts whether or not the gate is armed, so on a
/// legacy lineage they are the ONLY reference-arm telemetry there is.  Adding
/// them to this helper would make the gate-OFF cases demand they stay zero,
/// which is precisely the invariant-#4 blindness this pair exists to remove
/// (docs/SPEC_amb_iono_split_overcount.md §6.14.5 defect 1).  The positive
/// assertion lives in FlagRateCountersCountTheLegacyArmToo below.
void expectAllCountersZero(const IfPhaseSlipDetector::RefGateCounters& c) {
    EXPECT_EQ(c.legacy_candidates, 0);
    EXPECT_EQ(c.fires, 0);
    EXPECT_EQ(c.suppressed_scale, 0);
    EXPECT_EQ(c.suppressed_thin, 0);
    EXPECT_EQ(c.suppressed_persistence, 0);
    EXPECT_EQ(c.fires_station_event, 0);
    EXPECT_EQ(c.reselects, 0);
}

// ---------------------------------------------------------------------------
// Tier T1r + the pool-scale reference marking (§6.14.5 defects 1 and 2)
// ---------------------------------------------------------------------------

// Defect 1.  `network_flag_rates.csv` T1 counts only the network POOL arm, so
// it read "opportunities=2754791 flags=0" for a 24 h run in which this
// per-station REFERENCE arm drove 1917 acting refloats — the ON-path telemetry
// did not see the ON path.  The T1r pair fixes that, and it must count on the
// LEGACY path too: that is the configuration where no other counter moves at
// all, so a conditional increment would leave the tier reading zero on exactly
// the lineages that have no other instrument.
TEST(IfPhaseSlipRefGate, FlagRateCountersCountTheLegacyArmToo) {
    IfHarness h;                       // never calls setRefGate => legacy
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();

    // Epoch 0 has no previous gradients, so no family and no opportunity.
    EXPECT_TRUE(h.step(0, omc, elev).empty());
    EXPECT_EQ(h.det.refGateCounters().gate_opportunities, 0);

    // Epoch 1: five finite non-reference DDs (n >= 2) => one opportunity, quiet.
    EXPECT_TRUE(h.step(1, omc, elev).empty());
    EXPECT_EQ(h.det.refGateCounters().gate_opportunities, 1);
    EXPECT_EQ(h.det.refGateCounters().gate_flags, 0);

    // Epoch 2: a reference slip. One more opportunity, and this one FIRES.
    omc["G01"] -= 0.30;
    ASSERT_EQ(h.step(2, omc, elev).size(), 1u);
    EXPECT_EQ(h.det.refGateCounters().gate_opportunities, 2);
    EXPECT_EQ(h.det.refGateCounters().gate_flags, 1);

    // The gate-OFF invariant still holds for every HARDENED counter: T1r is
    // telemetry, and arming it changed no decision.
    expectAllCountersZero(h.det.refGateCounters());

    // One opportunity per station-epoch-constellation — NOT per satellite.
    // (T1's denominator is per-sample; conflating the two is what hid this.)
    EXPECT_TRUE(h.step(3, omc, elev).empty());
    EXPECT_EQ(h.det.refGateCounters().gate_opportunities, 3);
    EXPECT_EQ(h.det.refGateCounters().gate_flags, 1);
}

// The armed gate counts the same opportunities; only the VERDICT differs.
// Pins that the T1r denominator is gate-independent, so an A/B across
// `if_phase_td_ref_gate` compares two rates over the same denominator.
TEST(IfPhaseSlipRefGate, FlagRateOpportunitiesAreIndependentOfArming) {
    const ElevState elev = sixSatElevations();
    long long opp_legacy = 0, opp_armed = 0;
    for (const bool armed : { false, true }) {
        IfHarness h;
        if (armed) h.det.setRefGate(armedGate());
        OmcState omc = sixSatOmc();
        for (std::size_t e = 0; e < 5; ++e) h.step(e, omc, elev);
        (armed ? opp_armed : opp_legacy) = h.det.refGateCounters().gate_opportunities;
    }
    EXPECT_EQ(opp_legacy, opp_armed);
    EXPECT_GT(opp_legacy, 0);
}

// Defect 2.  The network pool arm builds its robust scale from
// getLastIfJumps(), which includes the reference satellite — whose double
// difference is identically 0 by construction, since grad(ref) = OMC(ref) −
// OMC(ref) = 0 every epoch.  Those exact zeros sit at the centre of the
// distribution and deflate the MAD (measured 16.0 % on a 24 h / 60 sta run).
// The pool cannot see which satellite is the reference, so the detector must
// mark it — this pins both halves of that contract: the flag is set on exactly
// one satellite, and the value it guards really is the structural zero.
TEST(IfPhaseSlipRefGate, ReferenceSampleIsMarkedAndIsAStructuralZero) {
    IfHarness h;
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);

    const auto& jumps = h.det.getLastIfJumps();
    ASSERT_EQ(jumps.size(), elev.size());

    std::size_t n_ref = 0;
    for (const auto& [sat, jp] : jumps) {
        if (!jp.is_reference) continue;
        ++n_ref;
        EXPECT_EQ(sat, "G01");            // the retained 80 deg reference
        EXPECT_TRUE(jp.valid);            // it IS a sample the pool would use
        EXPECT_DOUBLE_EQ(jp.delta_grad_m, 0.0);   // ...and it is exactly zero
    }
    EXPECT_EQ(n_ref, 1u);                 // exactly one per constellation

    // The non-reference DDs are the only ones carrying noise information.
    for (const auto& [sat, jp] : jumps)
        if (!jp.is_reference) EXPECT_NE(jp.delta_grad_m, 0.0) << sat;
}

// ---------------------------------------------------------------------------
// R7 case 1 — legacy (gate OFF) behaviour is untouched
// ---------------------------------------------------------------------------

// The byte-identity guard.  With a default-constructed RefGateParams
// (enable == false) a common step across the non-reference double differences
// larger than 0.5·λ_IF must still push the reference reset key exactly as it
// did before Card R, and NO counter may move — an armed-looking counter with
// the gate off would mean the hardened path is running on the legacy config.
TEST(IfPhaseSlipRefGate, LegacyGateOffStillFiresAndLeavesCountersZero) {
    IfHarness h;                       // never calls setRefGate => legacy
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();

    EXPECT_TRUE(h.step(0, omc, elev).empty());   // no previous epoch yet
    EXPECT_TRUE(h.step(1, omc, elev).empty());   // quiet

    // A reference slip IS a common step: shifting the reference's own OMC by
    // −0.30 m moves every non-reference double difference by +0.30 m.
    omc["G01"] -= 0.30;
    const auto keys = h.step(2, omc, elev);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), refKey("G01"));
    expectAllCountersZero(h.det.refGateCounters());

    // The step was a ONE-epoch event: the level stays put, so the next epoch's
    // double differences are back at zero and nothing re-fires.
    EXPECT_TRUE(h.step(3, omc, elev).empty());
    expectAllCountersZero(h.det.refGateCounters());
}

// Same wiring, sub-threshold step: the legacy gate is a bare comparison
// against 0.5·λ_IF, so 0.02 m (< 5.35 cm) must produce nothing.  Pins the
// floor these tests are calibrated against.
TEST(IfPhaseSlipRefGate, LegacyGateOffIgnoresSubThresholdCommonStep) {
    IfHarness h;
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();
    ASSERT_LT(0.02, kRefFloorM);

    EXPECT_TRUE(h.step(0, omc, elev).empty());
    omc["G01"] -= 0.02;
    EXPECT_TRUE(h.step(1, omc, elev).empty());
    expectAllCountersZero(h.det.refGateCounters());
}

// ---------------------------------------------------------------------------
// R7 case 2 — step-fires-once
// ---------------------------------------------------------------------------

// A genuine reference slip is a one-epoch COMMON step: every non-reference
// double difference moves by the same large amount with noise-level scatter.
// The armed gate must fire it exactly once (R2's first half) and the counters
// must show the legacy candidate was adjudicated, not merely passed through.
TEST(IfPhaseSlipRefGate, ArmedGateFiresGenuineStepExactlyOnce) {
    IfHarness h;
    h.det.setRefGate(armedGate());
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();

    EXPECT_TRUE(h.step(0, omc, elev).empty());
    EXPECT_TRUE(h.step(1, omc, elev).empty());

    omc["G01"] -= 0.30;                       // the slip epoch
    const auto keys = h.step(2, omc, elev);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), refKey("G01"));

    // Four more quiet epochs on the post-slip level: not one re-fire.
    for (std::size_t e = 3; e < 7; ++e)
        EXPECT_TRUE(h.step(e, omc, elev).empty()) << "epoch " << e;

    const auto& c = h.det.refGateCounters();
    EXPECT_EQ(c.fires, 1);
    EXPECT_EQ(c.legacy_candidates, 1);
    EXPECT_EQ(c.suppressed_scale, 0);
    EXPECT_EQ(c.suppressed_thin, 0);
    EXPECT_EQ(c.suppressed_persistence, 0);
    EXPECT_EQ(c.reselects, 0);
}

// ---------------------------------------------------------------------------
// R7 case 3 — drift-does-not-refire (the L1 defect)
// ---------------------------------------------------------------------------

// The audit's JPLM00USA / MKEA00USA G10 family: a persistent SUB-slip drift
// whose per-satellite increments scatter as widely as their own median.  The
// legacy gate re-fires on it every single epoch (9 consecutive at JPLM); the
// armed gate must not, because the family never agrees on a common step.
//
// Both arms run the SAME stream, so the contrast is the fix itself.
TEST(IfPhaseSlipRefGate, ArmedGateDriftDoesNotRefireLegacyDoes) {
    const ElevState elev = sixSatElevations();
    // Per-epoch drift increments: median 0.08 m clears the 5.35 cm legacy
    // floor every epoch, but the scatter about that median is of the SAME
    // order (MAD-sigma ≈ 7.4 cm) — the signature of drift, not of a step.
    const std::map<std::string, double> drift = {
        { "G02", 0.02 }, { "G03", 0.05 }, { "G04", 0.08 },
        { "G05", 0.13 }, { "G06", 0.20 } };
    constexpr std::size_t kEpochs = 10;   // => 9 double-difference epochs

    // --- legacy arm: fires every epoch (the defect) ---
    int legacy_fire_epochs = 0;
    {
        IfHarness h;
        OmcState omc = sixSatOmc();
        for (std::size_t e = 0; e < kEpochs; ++e) {
            if (e > 0) for (const auto& [sat, d] : drift) omc[sat] += d;
            const auto keys = h.step(e, omc, elev);
            if (!keys.empty()) {
                ++legacy_fire_epochs;
                EXPECT_EQ(keys.front(), refKey("G01"));
            }
        }
        expectAllCountersZero(h.det.refGateCounters());
    }
    EXPECT_EQ(legacy_fire_epochs, 9)
        << "the legacy gate is supposed to re-fire on every drift epoch — "
           "if it does not, this stream no longer reproduces defect L1";

    // --- armed arm: same stream, no event stream at all ---
    IfHarness h;
    h.det.setRefGate(armedGate());
    OmcState omc = sixSatOmc();
    int armed_fire_epochs = 0;
    for (std::size_t e = 0; e < kEpochs; ++e) {
        if (e > 0) for (const auto& [sat, d] : drift) omc[sat] += d;
        if (!h.step(e, omc, elev).empty()) ++armed_fire_epochs;
    }
    EXPECT_EQ(armed_fire_epochs, 0);

    const auto& c = h.det.refGateCounters();
    // Structural bound from R2: even in the worst case the run-length cap
    // makes an unbounded per-epoch stream impossible.
    EXPECT_LE(c.fires, static_cast<long long>(armedGate().max_consecutive));
    EXPECT_EQ(c.fires, 0);
    // The gate genuinely RAN (invariant 4): the legacy condition held on all
    // nine double-difference epochs and every one of them was suppressed.
    EXPECT_EQ(c.legacy_candidates, 9);
    EXPECT_GT(c.suppressed_scale + c.suppressed_thin, 0);
    EXPECT_EQ(c.legacy_candidates,
              c.fires + c.suppressed_scale + c.suppressed_thin
                  + c.suppressed_persistence);
}

// ---------------------------------------------------------------------------
// R7 case 4 — thin-family defer (R3, both directions)
// ---------------------------------------------------------------------------

// With only three non-reference double differences (the audit's L1C_L2L
// census was ~3.7 arcs/station) the median is not a trustworthy common-mode
// estimate, so the physical floor widens by thin_widen (3x => 16.0 cm) and the
// deferral is COUNTED.  A modest exceedance must defer; a large coherent one
// must still fire — "widen", never "ignore".
TEST(IfPhaseSlipRefGate, ThinFamilyDefersModestStepButStillFiresLargeOne) {
    IfHarness h;
    const auto gate = armedGate();
    h.det.setRefGate(gate);
    ASSERT_EQ(gate.min_n, 4);

    const ElevState elev = { { "G01", 80.0 }, { "G02", 40.0 },
                             { "G03", 35.0 }, { "G04", 30.0 } };   // n_nonref = 3
    OmcState omc = { { "G01", 0.00 }, { "G02", 0.12 },
                     { "G03", -0.07 }, { "G04", 0.21 } };

    const double thin_thr = gate.thin_widen * kRefFloorM;   // ≈ 0.1604 m
    EXPECT_TRUE(h.step(0, omc, elev).empty());

    // (a) modest exceedance: past the legacy floor, short of the widened one.
    ASSERT_GT(0.10, kRefFloorM);
    ASSERT_LT(0.10, thin_thr);
    omc["G01"] -= 0.10;
    EXPECT_TRUE(h.step(1, omc, elev).empty());
    {
        const auto& c = h.det.refGateCounters();
        EXPECT_EQ(c.legacy_candidates, 1);   // legacy would have fired here
        EXPECT_EQ(c.suppressed_thin, 1);     // ...counted, not silent (R3)
        EXPECT_EQ(c.fires, 0);
    }

    // (b) large coherent exceedance on the same thin family: still fires.
    ASSERT_GT(0.30, thin_thr);
    omc["G01"] -= 0.30;
    const auto keys = h.step(2, omc, elev);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), refKey("G01"));
    {
        const auto& c = h.det.refGateCounters();
        EXPECT_EQ(c.legacy_candidates, 2);
        EXPECT_EQ(c.suppressed_thin, 1);
        EXPECT_EQ(c.fires, 1);
    }
}

// ---------------------------------------------------------------------------
// R7 case 5 — reference handover offset correctness
// ---------------------------------------------------------------------------

// When the reference changes, every stored gradient is expressed against the
// OLD reference and every new one against the NEW one — a constant frame shift
// of (OMC_old − OMC_new) that the median-offset rebase in
// processDoubleDifferences must remove.  Here that shift is 0.60 m, more than
// ELEVEN times the 5.35 cm fire floor, so a broken rebase would manufacture a
// reference "slip" out of a perfectly quiet stream.
//
// The handover is forced the way the field forces it: the incumbent sinks
// below the 45 deg retention threshold.
TEST(IfPhaseSlipRefGate, ReferenceHandoverDoesNotManufactureASlip) {
    for (const bool gate_on : { false, true }) {
        IfHarness h;
        if (gate_on) h.det.setRefGate(armedGate());

        ElevState elev = { { "G01", 80.0 }, { "G02", 70.0 }, { "G03", 40.0 },
                           { "G04", 35.0 }, { "G05", 30.0 } };
        // G01 (the incumbent reference) and G02 (its successor) sit 0.60 m
        // apart in OMC — that IS the frame shift the rebase has to absorb.
        OmcState omc = { { "G01", 0.00 }, { "G02", 0.60 }, { "G03", -0.45 },
                         { "G04", 0.30 }, { "G05", -0.20 } };
        ASSERT_GT(std::abs(omc.at("G01") - omc.at("G02")), 10.0 * kRefFloorM);

        for (std::size_t e = 0; e < 3; ++e)
            EXPECT_TRUE(h.step(e, omc, elev).empty())
                << (gate_on ? "armed" : "legacy") << " pre-handover epoch " << e;

        // Handover epoch: G01 drops to 30 deg, so the reference is re-elected
        // to G02 (highest remaining).  G01 stays in the family — this is a
        // reference change, not an arc death.
        elev["G01"] = 30.0;
        EXPECT_TRUE(h.step(3, omc, elev).empty())
            << (gate_on ? "armed" : "legacy") << " handover manufactured a slip";

        // The rebase is quantitative, not just non-firing: every double
        // difference must land near zero, i.e. the 0.60 m frame shift was
        // absorbed to better than a centimetre.
        for (const auto& [sat, info] : h.det.getLastIfJumps()) {
            ASSERT_TRUE(info.valid) << sat;
            EXPECT_LT(std::abs(info.delta_grad_m), 0.01)
                << (gate_on ? "armed" : "legacy") << " residual at " << sat;
        }

        for (std::size_t e = 4; e < 6; ++e)
            EXPECT_TRUE(h.step(e, omc, elev).empty())
                << (gate_on ? "armed" : "legacy") << " post-handover epoch " << e;

        if (gate_on) {
            // Nothing in a quiet stream ever looked like a slip, so the gate
            // was never even consulted.
            expectAllCountersZero(h.det.refGateCounters());
        }
    }
}

// ---------------------------------------------------------------------------
// R7 case 6 — persistence escalation
// ---------------------------------------------------------------------------

// A COHERENT step repeated on the same reference every epoch is the pathological
// case R2 must bound: the gate fires at most max_consecutive times, then stops
// re-firing and escalates to a forced reference re-selection, moving the
// offender into the network pool arm where an elevation-normalized, empirically
// scaled residual judges it.
//
// The re-selection is verified through the PUBLIC API twice over: (a) at the
// re-election epoch the offending satellite appears as a large NON-reference
// double difference while every other satellite sits at zero — impossible if it
// were still the reference (its own double difference is identically zero by
// construction); and (b) the next fire names the NEW reference in its reset key.
// Run at the SHIPPED cap (1 — the value the network actually arms) and at 2, so
// the run-length arithmetic itself is covered rather than one operating point:
// the gate must fire on each of the first `cap` consecutive epochs and escalate
// on the (cap+1)-th, whatever `cap` is.
TEST(IfPhaseSlipRefGate, PersistentCoherentStepEscalatesToReferenceReselection) {
  for (const int cap : { 1, 2 }) {
    SCOPED_TRACE("max_consecutive = " + std::to_string(cap));
    IfHarness h;
    auto gate = armedGate();
    gate.max_consecutive = cap;
    h.det.setRefGate(gate);

    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();
    constexpr double kStep = 0.30;   // ≈ 2.8 λ_IF, far above the floor

    EXPECT_TRUE(h.step(0, omc, elev).empty());

    // Epochs 1..cap: one coherent step each on G01 -> one fire each (the cap).
    for (int e = 1; e <= cap; ++e) {
        omc["G01"] -= kStep;
        const auto keys = h.step(static_cast<std::size_t>(e), omc, elev);
        ASSERT_EQ(keys.size(), 1u) << "epoch " << e;
        EXPECT_EQ(keys.front(), refKey("G01"));
    }
    {
        const auto& c = h.det.refGateCounters();
        EXPECT_EQ(c.fires, cap);
        EXPECT_EQ(c.suppressed_persistence, 0);
        EXPECT_EQ(c.reselects, 0);
    }

    // Epoch cap+1: one consecutive step too many trips the cap.  No event, no
    // refloat — the gate escalates instead.
    omc["G01"] -= kStep;
    EXPECT_TRUE(h.step(static_cast<std::size_t>(cap + 1), omc, elev).empty());
    {
        const auto& c = h.det.refGateCounters();
        EXPECT_EQ(c.fires, cap);                  // saturated at max_consecutive
        EXPECT_EQ(c.suppressed_persistence, 1);
        EXPECT_EQ(c.reselects, 1);
    }

    // Epoch cap+2: the forced re-election runs (G01 excluded) -> G02 becomes the
    // reference.  The handover rebase absorbs the common mode, so the drifting
    // G01 now shows up as an ORDINARY non-reference double difference: this is
    // the hand-off to the pool arm, and it is only possible if the reference
    // actually changed.
    omc["G01"] -= kStep;
    EXPECT_TRUE(h.step(static_cast<std::size_t>(cap + 2), omc, elev).empty());
    {
        const auto& jumps = h.det.getLastIfJumps();
        ASSERT_EQ(jumps.count("G01"), 1u);
        ASSERT_EQ(jumps.count("G02"), 1u);
        EXPECT_NEAR(jumps.at("G01").delta_grad_m, -kStep, 0.01)
            << "G01 must be a non-reference outlier now, not the reference";
        EXPECT_NEAR(jumps.at("G02").delta_grad_m, 0.0, 0.01)
            << "G02 must be the new reference (its own DD is zero)";
        for (const char* sat : { "G03", "G04", "G05", "G06" })
            EXPECT_NEAR(jumps.at(sat).delta_grad_m, 0.0, 0.01) << sat;
    }

    // Epoch cap+3: a genuine coherent slip on the NEW reference fires, and the
    // reset key NAMES it — the second, independent proof that the escalation
    // actually re-selected the reference rather than merely muting the old one.
    omc["G02"] -= kStep;
    const auto keys = h.step(static_cast<std::size_t>(cap + 3), omc, elev);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), refKey("G02"));

    const auto& c = h.det.refGateCounters();
    EXPECT_EQ(c.fires, cap + 1);              // cap pre-escalation + this one
    EXPECT_EQ(c.suppressed_persistence, 1);
    EXPECT_EQ(c.reselects, 1);
    // The old reference is never named again: it left the reference path for
    // the pool arm and stays there.
    EXPECT_EQ(c.legacy_candidates,
              c.fires + c.suppressed_scale + c.suppressed_thin
                  + c.suppressed_persistence);
  }
}

// N2 / spec-followups H1 — the header defaults and the config defaults are ONE
// set of numbers.  They drifted once (max_consecutive: header 2, config 1), and
// because the config is the only armed path, every unit case above would have
// been certifying a gate the field never runs.  Pinning them together makes that
// class of drift a test failure instead of a review finding.
TEST(IfPhaseSlipRefGate, HeaderDefaultsMatchTheShippedConfigDefaults) {
    const IfPhaseSlipDetector::RefGateParams p;   // header defaults
    GnssConfig cfg;                               // JSON-less config defaults
    const auto& sq = cfg.getNetworkConfig().slip_qc;

    EXPECT_FALSE(p.enable) << "the header default must stay the LEGACY gate";
    EXPECT_FALSE(sq.ref_gate);
    EXPECT_EQ(p.max_consecutive, sq.ref_max_consecutive);
    EXPECT_EQ(p.max_consecutive, 1) << "spec R2: a genuine reference slip fires ONCE";
    EXPECT_EQ(p.min_n, sq.ref_min_n);
    EXPECT_DOUBLE_EQ(p.k, sq.ref_k);
    EXPECT_DOUBLE_EQ(p.thin_widen, sq.ref_thin_widen);
    EXPECT_DOUBLE_EQ(p.coherence, sq.ref_coherence);
    EXPECT_DOUBLE_EQ(p.ewma_alpha, sq.ref_ewma_alpha);
    EXPECT_DOUBLE_EQ(p.shrink_kappa, sq.ref_shrink_kappa);
    EXPECT_DOUBLE_EQ(p.floor_frac, sq.ref_floor_frac);
}

// A station-wide receiver common mode cannot reach this gate at all: it
// cancels in the BETWEEN-SATELLITE single difference by construction.  That is
// why the spec's third CANDIDATE mechanism ("if the freshly re-elected
// reference slips at once, hand it to the pool as station-local") was NOT
// implemented — it would have been a permanently-zero counter, the exact
// standing-invariant-#4 failure mode.  This test pins the cancellation so a
// future implementer does not re-introduce it on a false premise.
TEST(IfPhaseSlipRefGate, StationWideCommonModeCancelsAndReachesNoRefCandidate) {
    IfHarness h;
    h.det.setRefGate(armedGate());
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();
    constexpr double kStep = 0.30;

    const int cap = armedGate().max_consecutive;
    EXPECT_TRUE(h.step(0, omc, elev).empty());
    // Drive the reference through fire×cap -> the cap-tripping epoch -> the
    // forced re-election, so the scenario below runs on a FRESH reference
    // whatever the shipped cap is.
    for (int e = 1; e <= cap + 2; ++e) {
        omc["G01"] -= kStep;
        h.step(static_cast<std::size_t>(e), omc, elev);
    }
    ASSERT_EQ(h.det.refGateCounters().reselects, 1);

    const long long cand_before = h.det.refGateCounters().legacy_candidates;
    // Every satellite's OMC jumps together — a receiver-level event.
    for (auto& [sat, v] : omc) v += 0.40;
    EXPECT_TRUE(h.step(static_cast<std::size_t>(cap + 3), omc, elev).empty());

    // The common mode is gone before any statistic is formed, so the gate is
    // never even offered a candidate to adjudicate.
    EXPECT_EQ(h.det.refGateCounters().legacy_candidates, cand_before)
        << "a station-wide common mode reached the reference gate — it must "
           "cancel in the between-satellite single difference";
}

// R4 — a multi-satellite station re-lock (many INDEPENDENT slip-sized jumps,
// so the family is incoherent and its median is a mixture) must still reset
// the reference arc.  This is the case the 2026-07-21 ON smoke showed a
// coherence-only gate silently losing (OWMG00NZL/G31).  It fires WITHOUT the
// median rebase: the reference is reset, its slip magnitude is not invented.
TEST(IfPhaseSlipRefGate, IncoherentMultiSatelliteStationEventStillResetsTheReference) {
    IfHarness h;
    h.det.setRefGate(armedGate());
    const ElevState elev = sixSatElevations();
    OmcState omc = sixSatOmc();

    EXPECT_TRUE(h.step(0, omc, elev).empty());

    // Four of the five non-reference satellites jump by DIFFERENT slip-sized
    // amounts (>> 0.5*lambda_IF ~ 5.35 cm), all the same sign so the median is
    // large: an incoherent family whose members are individually slipping.
    omc["G02"] += 0.90;
    omc["G03"] += 0.40;
    omc["G04"] += 1.60;
    omc["G05"] += 0.70;
    const auto keys = h.step(1, omc, elev);

    bool ref_reset = false;
    for (const auto& k : keys)
        if (k.rfind("G01_", 0) == 0) ref_reset = true;
    EXPECT_TRUE(ref_reset) << "the reference arc must be reset on a station event";

    const auto& c = h.det.refGateCounters();
    EXPECT_EQ(c.fires, 1);
    EXPECT_EQ(c.fires_station_event, 1)
        << "an incoherent slip-sized family must be classified as a station "
           "event, not as a coherent reference step";

    // The rebase is what a FireNoRebase verdict refuses: the OTHER satellites'
    // jumps must survive into the cached DDs the network pool arm consumes,
    // not be flattened by subtracting the mixture median.
    const auto& jumps = h.det.getLastIfJumps();
    ASSERT_EQ(jumps.count("G04"), 1u);
    EXPECT_NEAR(jumps.at("G04").delta_grad_m, 1.60, 0.02)
        << "the median rebase must NOT have been applied";
}

// ---------------------------------------------------------------------------
// R5 — both detector modes keep working
// ---------------------------------------------------------------------------

// Deferred mode (network) skips the per-station MAD outlier arm entirely: the
// driver thresholds against a network-POOLED per-signal-pair scale instead.
// Self-contained mode (user/kinematic, currently STAGED) keeps it.  The same
// single-satellite jump therefore produces a reset key in one mode and none in
// the other — while BOTH modes cache the identical double difference for the
// caller, which is what makes the pooled path possible.
TEST(IfPhaseSlipModes, SelfContainedFlagsNonReferenceOutlierDeferredDefersIt) {
    const ElevState elev = sixSatElevations();

    for (const bool defer : { true, false }) {
        IfHarness h(defer);
        OmcState omc = sixSatOmc();

        EXPECT_TRUE(h.step(0, omc, elev).empty());

        // One non-reference satellite jumps ~4.7 IF cycles; everyone else is
        // quiet, so the common-mode reference test cannot fire.
        omc["G06"] += 0.50;
        const auto keys = h.step(1, omc, elev);

        if (defer) {
            EXPECT_TRUE(keys.empty())
                << "deferred mode must leave per-satellite flagging to the pool";
        } else {
            ASSERT_EQ(keys.size(), 1u);
            EXPECT_EQ(keys.front(), "G06_L1C_L2W");
        }

        // Either way the jump is measured and published for the caller.
        const auto& jumps = h.det.getLastIfJumps();
        ASSERT_EQ(jumps.count("G06"), 1u);
        EXPECT_TRUE(jumps.at("G06").valid);
        EXPECT_NEAR(jumps.at("G06").delta_grad_m, 0.50, 0.01);
        EXPECT_NEAR(jumps.at("G06").lambda_if_m, kLambdaIf, 1e-9);
        EXPECT_EQ(jumps.at("G06").sig1, "L1C");
        EXPECT_EQ(jumps.at("G06").sig2, "L2W");

        // The reference gate is untouched by either mode in this scenario.
        expectAllCountersZero(h.det.refGateCounters());
    }
}

} // namespace
} // namespace GnssProcessor
