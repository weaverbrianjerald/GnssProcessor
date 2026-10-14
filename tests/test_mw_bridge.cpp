/**
 * @file test_mw_bridge.cpp
 * @brief Unit gates for the tri-frequency phase bridge accumulator in MwSlipDetector
 *        (card WL-2, M5; docs/SPEC_ewl_wl_cascade_card.md §3.4).
 *
 * The bridge is the new estimator input of the WL lane: a per-arc Welford mean of the
 * geometry-free phase-only observable GF_b = φ_WL − φ_EWL (metres, zero-differenced),
 * lagged one epoch (the same double-count guard as arcZdMean), reset from the same
 * funnel that resets the ambiguity, and refloated on a gross jump in case a slip was
 * missed.  The gates cover the GF_b assembly and λ table, the lag, maturity and
 * validity, the gap and jump refloats, the per-signal, per-satellite and full reset
 * hooks, and invariant #6: with the bridge disarmed (the default) the detector never
 * touches a bridge arc, so its output is byte-identical.
 *
 * The detector is fed hand-built phase-only observations; no estimator or network is
 * involved.  Frequencies come from the production PppUtils table, so the λ assertions
 * also check the wavelength table.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"

using namespace GnssProcessor;

namespace {

// GPS carrier frequencies (Hz): the hand-value reference for the λ table.
constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6, kF5 = 1176.45e6;
constexpr double kC  = 299792458.0;
const double kLamWl  = kC / (kF1 - kF2);   // ≈ 0.8619 m
const double kLamEwl = kC / (kF2 - kF5);   // ≈ 5.861 m

// Hand GF_b (m) = λ_WL·(L1−L2) − λ_EWL·(L2−L5), raw cycles in.
double handGf(double l1c, double l2c, double l5c) {
    return kLamWl * (l1c - l2c) - kLamEwl * (l2c - l5c);
}

// Hand C_iono = f_ref²·(1/(f1·f2) − 1/(f2·f5)), f_ref = GPS L1 (the estimator iono frame).
// GPS: ≈ −0.4352, i.e. about −0.44 m of GF_b per metre of L1-referenced slant iono.
const double kIonoCoeff = kF1 * kF1 * (1.0 / (kF1 * kF2) - 1.0 / (kF2 * kF5));

// A tri-frequency phase-only observation.  `observed` drives the detector's band
// selection; `raw_observed` supplies the raw cycle values GF_b is formed from.
PppObservation triObs(const std::string& sat, double tow,
                      double l1c, double l2c, double l5c,
                      double elev_deg = 45.0) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::GPS;
    obs.time      = GnssTime{ 2400, tow };
    obs.elevation = elev_deg;
    obs.azimuth   = 0.0;
    obs.raw_observed = { { "L1C", l1c }, { "L2W", l2c }, { "L5Q", l5c } };
    obs.observed     = obs.raw_observed;
    return obs;
}

// Drive `n` constant-phase epochs at 1 s cadence from tow0 through one detector.
void driveConst(MwSlipDetector& det, const std::string& sat, int n,
                double l1c, double l2c, double l5c, double tow0 = 100000.0) {
    for (int k = 0; k < n; ++k)
        det.detectLargeMwResets({ triObs(sat, tow0 + k, l1c, l2c, l5c) });
}

}  // namespace

// Assembly, λ table, validity and maturity.  With constant phases the lagged mean
// equals the hand GF_b and the variance collapses toward zero.
TEST(MwBridge, AssemblyHandValueAndLambdaTable) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G01", 4, /*L1*/100.0, /*L2*/80.0, /*L5*/70.0);

    const auto b = det.arcBridge("G01", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(b.valid);
    EXPECT_GE(b.count, 2u);
    EXPECT_NEAR(b.lambda_wl,  kLamWl,  5e-3);
    EXPECT_NEAR(b.lambda_ewl, kLamEwl, 5e-3);
    // Exact consistency against the returned λ values, then against the hand value.
    EXPECT_NEAR(b.gf_mean_m, b.lambda_wl * 20.0 - b.lambda_ewl * 10.0, 1e-9);
    EXPECT_NEAR(b.gf_mean_m, handGf(100.0, 80.0, 70.0), 1e-2);
    EXPECT_LT(b.var_of_mean_m2, 1e-12);   // constant input ⇒ ~zero variance
    // Iono coefficient of §3.4 v2 (the frequency-derived de-bias sensitivity): GPS ≈ −0.4352.
    EXPECT_NEAR(b.iono_coeff, kIonoCoeff, 5e-4);
    EXPECT_NEAR(b.iono_coeff, -0.4351, 2e-3);
}

// Byte identity (invariant #6): with the bridge disarmed (the default) no arc is
// accumulated, so arcBridge is always invalid.
TEST(MwBridge, DisarmedNeverAccumulates) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);           // setBridgeEnabled is not called
    driveConst(det, "G01", 5, 100.0, 80.0, 70.0);
    EXPECT_FALSE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);
    EXPECT_FALSE(det.bridgeEnabled());
}

// Lag and maturity: the arc is invalid while count_lag < 2 and becomes valid once
// the lagged state holds two samples (the same-epoch double-count guard).
TEST(MwBridge, LaggedMaturity) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G07", 2, 50.0, 40.0, 30.0);
    EXPECT_FALSE(det.arcBridge("G07", "L1C", "L2W", "L5Q").valid);  // count_lag == 1
    driveConst(det, "G07", 1, 50.0, 40.0, 30.0, /*tow0=*/100002.0);
    EXPECT_TRUE(det.arcBridge("G07", "L1C", "L2W", "L5Q").valid);   // count_lag == 2
}

// Data-gap refloat: a gap beyond the tolerance (120 s by default) restarts the arc,
// which stays invalid until it matures again; no pre-gap history is carried across.
TEST(MwBridge, GapRefloat) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G05", 4, 100.0, 80.0, 70.0);
    ASSERT_TRUE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);
    det.detectLargeMwResets({ triObs("G05", 100000.0 + 5000.0, 100.0, 80.0, 70.0) });  // >120 s gap
    EXPECT_FALSE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);   // refloated (count_lag 0)
}

// Gross-jump refloat: a WL/EWL slip the funnel missed (a GF_b jump above
// kBridgeJumpResetM) resets the accumulator instead of entering the slow mean.  A
// +0.6 cycle L1 step is a 0.52 m GF_b jump (0.6·λ_WL), above the 0.4 m guard.
TEST(MwBridge, GrossJumpRefloat) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G09", 4, 100.0, 80.0, 70.0);
    ASSERT_TRUE(det.arcBridge("G09", "L1C", "L2W", "L5Q").valid);
    det.detectLargeMwResets({ triObs("G09", 100000.0 + 4, 100.6, 80.0, 70.0) });  // ΔGF ≈ 0.52 m
    EXPECT_FALSE(det.arcBridge("G09", "L1C", "L2W", "L5Q").valid);   // jump refloated
}

// Reset funnel: a per-signal reset on any band of the triple erases the bridge arc,
// a reset on an unrelated signal leaves it intact, and resetSatellite and resetAll
// clear it.
TEST(MwBridge, ResetFunnelHooks) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);

    auto rewarm = [&](const std::string& sat) { driveConst(det, sat, 4, 100.0, 80.0, 70.0); };

    // (a) Unrelated signal: the arc survives.
    rewarm("G01");
    det.resetSatelliteSignal("G01", "L9X");
    EXPECT_TRUE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);

    // (b) The third (EWL) band: the arc is erased.
    det.resetSatelliteSignal("G01", "L5Q");
    EXPECT_FALSE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);

    // (c) A WL-pair band: the arc is erased.
    rewarm("G02");
    det.resetSatelliteSignal("G02", "L2W");
    EXPECT_FALSE(det.arcBridge("G02", "L1C", "L2W", "L5Q").valid);

    // (d) whole-satellite reset.
    rewarm("G03");
    det.resetSatellite("G03");
    EXPECT_FALSE(det.arcBridge("G03", "L1C", "L2W", "L5Q").valid);

    // (e) full reset.
    rewarm("G04");
    det.resetAll();
    EXPECT_FALSE(det.arcBridge("G04", "L1C", "L2W", "L5Q").valid);
}

// An LLI flag on a phase restarts the bridge arc (the bridge has no detection
// statistic of its own).
TEST(MwBridge, LossOfLockRefloats) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G11", 4, 100.0, 80.0, 70.0);
    ASSERT_TRUE(det.arcBridge("G11", "L1C", "L2W", "L5Q").valid);
    PppObservation lli = triObs("G11", 100000.0 + 4, 100.0, 80.0, 70.0);
    lli.lli_flags["L1C"] = 1;   // loss-of-lock flag on the L1 phase
    det.detectLargeMwResets({ lli });
    EXPECT_FALSE(det.arcBridge("G11", "L1C", "L2W", "L5Q").valid);
}

// =====================================================================
// Per-sample bridge iono de-bias (network.mw_wl.bridge_sample_debias), the
// 60-epoch age-bias fix.  The raw GF Welford accumulates C_iono·I over the whole
// arc while the read-time de-bias subtracts C_iono·ı̃(now); folding
// GF_k − C_iono·ı̃(t_k) per sample makes the statistic stationary.
// =====================================================================

// Main gate of the de-bias: under a linear iono drift injected into the phases, the
// raw arc's mean is biased by C_iono·mean(I) and its variance is inflated by the
// drift.  The armed arc folds stationary samples, so its mean is the true constant
// and its variance collapses.  The drift enters through the L5 phase so that
// gf_k = gf0 + C_iono·I_k, and the matching ı̃_k push uses the same C_iono the
// detector computes.
TEST(MwBridge, PerSampleDebiasStationaryUnderIonoDrift) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector raw(cfg, log), deb(cfg, log);
    raw.setBridgeEnabled(true);
    deb.setBridgeEnabled(true);
    deb.setBridgePerSampleDebias(true);

    const double gf0 = handGf(100.0, 80.0, 70.0);
    std::vector<double> iono;   // I_k ramp (m): 0.0, 0.1, ...; a strong drift
    for (int k = 0; k < 6; ++k) iono.push_back(0.1 * k);
    for (int k = 0; k < 6; ++k) {
        // gf_k = gf0 + C_iono·I_k  ⇔  L5c_k = L5c0 + C_iono·I_k/λ_EWL (∂gf/∂L5c = +λ_EWL).
        const double l5 = 70.0 + (kIonoCoeff * iono[k]) / kLamEwl;
        deb.setBridgeIonoEstimates({ { "G01", iono[k] } });
        raw.detectLargeMwResets({ triObs("G01", 100000.0 + k, 100.0, 80.0, l5) });
        deb.detectLargeMwResets({ triObs("G01", 100000.0 + k, 100.0, 80.0, l5) });
    }
    const auto br = raw.arcBridge("G01", "L1C", "L2W", "L5Q");
    const auto bd = deb.arcBridge("G01", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(br.valid);
    ASSERT_TRUE(bd.valid);
    // Armed: every folded sample equals gf0 exactly ⇒ stationary.
    EXPECT_NEAR(bd.gf_mean_m, gf0, 1e-9);
    EXPECT_LT(bd.var_of_mean_m2, 1e-18);
    EXPECT_EQ(deb.bridgeDebiasSkipped(), 0u);
    // Raw: mean biased by C_iono·mean(I over the lagged window), variance inflated.
    EXPECT_GT(std::abs(br.gf_mean_m - gf0), 0.05);
    EXPECT_GT(br.var_of_mean_m2, 1e3 * std::max(bd.var_of_mean_m2, 1e-30));
}

// A sample without a pushed estimate is skipped and counted; it is never folded raw
// into a de-biased arc, which would mix frames.  The arc starts once estimates
// arrive.
TEST(MwBridge, PerSampleDebiasSkipsWithoutEstimate) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setBridgePerSampleDebias(true);
    driveConst(det, "G05", 3, 100.0, 80.0, 70.0);            // no estimates pushed
    EXPECT_EQ(det.bridgeDebiasSkipped(), 3u);
    EXPECT_FALSE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);
    det.setBridgeIonoEstimates({ { "G05", 0.25 } });          // estimates arrive
    driveConst(det, "G05", 3, 100.0, 80.0, 70.0, /*tow0=*/100003.0);
    EXPECT_EQ(det.bridgeDebiasSkipped(), 3u);                 // no further skips
    const auto b = det.arcBridge("G05", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(b.valid);
    EXPECT_NEAR(b.gf_mean_m, handGf(100.0, 80.0, 70.0) - kIonoCoeff * 0.25, 1e-9);
}

// Off by default and byte-neutral: pushed estimates are ignored, accumulation is
// bit-identical to a detector that never received them, and no skip is counted.
TEST(MwBridge, PerSampleDebiasOffIsRawByteIdentical) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector plain(cfg, log), pushed(cfg, log);
    plain.setBridgeEnabled(true);
    pushed.setBridgeEnabled(true);                            // de-bias not armed
    pushed.setBridgeIonoEstimates({ { "G09", 1.234 } });
    driveConst(plain,  "G09", 5, 100.0, 80.0, 70.0);
    driveConst(pushed, "G09", 5, 100.0, 80.0, 70.0);
    const auto a = plain.arcBridge("G09", "L1C", "L2W", "L5Q");
    const auto b = pushed.arcBridge("G09", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(a.valid);
    ASSERT_TRUE(b.valid);
    EXPECT_EQ(a.gf_mean_m, b.gf_mean_m);                      // bit-exact
    EXPECT_EQ(a.var_of_mean_m2, b.var_of_mean_m2);
    EXPECT_EQ(pushed.bridgeDebiasSkipped(), 0u);
}
