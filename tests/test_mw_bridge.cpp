/**
 * @file test_mw_bridge.cpp
 * @brief M5 card WL-2 gate — the tri-frequency phase bridge accumulator
 *        (SPEC docs/SPEC_ewl_wl_cascade_card.md §3.4).
 *
 * The bridge is the load-bearing NEW math of the WL lane: a per-arc Welford of the
 * geometry-free phase-only observable GF_b = φ_WL − φ_EWL (meters, ZD), lagged one
 * epoch (the same double-count guard as arcZdMean), reset from the same funnel that
 * resets the ambiguity, and guarded against a missed slip by a gross-jump refloat.
 * These gates pin: the GF_b assembly + λ table, the lag, the maturity/validity,
 * the gap + jump refloats, the per-signal / per-sat / full reset hooks, and — the
 * standing-invariant-#6 crux — that with the bridge DISARMED (default) the detector
 * never touches a bridge arc (byte-identical).
 *
 * These are pure unit gates: a MwSlipDetector fed hand-built phase-only observations,
 * no estimator, no network.  frequencies come from the production PppUtils table so
 * the λ assertions double as a wavelength-table check.
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

// GPS carrier frequencies (Hz) — the hand-value reference for the λ table.
constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6, kF5 = 1176.45e6;
constexpr double kC  = 299792458.0;
const double kLamWl  = kC / (kF1 - kF2);   // ≈ 0.8619 m
const double kLamEwl = kC / (kF2 - kF5);   // ≈ 5.861 m

// Hand GF_b (m) = λ_WL·(L1−L2) − λ_EWL·(L2−L5), raw cycles in.
double handGf(double l1c, double l2c, double l5c) {
    return kLamWl * (l1c - l2c) - kLamEwl * (l2c - l5c);
}

// Hand C_iono = f_ref²·(1/(f1·f2) − 1/(f2·f5)), f_ref = GPS L1 (the estimator iono frame).
// GPS: ≈ −0.4351 (a −0.44 cyc/m WL-domain bias per meter of L1-referenced slant iono).
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

// The core assembly + λ table + validity/maturity.  Constant phases ⇒ the lagged
// mean IS the hand GF_b and the variance collapses toward zero.
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
    // Internal consistency (against the RETURNED λ) — exact — and the hand value.
    EXPECT_NEAR(b.gf_mean_m, b.lambda_wl * 20.0 - b.lambda_ewl * 10.0, 1e-9);
    EXPECT_NEAR(b.gf_mean_m, handGf(100.0, 80.0, 70.0), 1e-2);
    EXPECT_LT(b.var_of_mean_m2, 1e-12);   // constant input ⇒ ~zero variance
    // §3.4 v2 iono coefficient (the frequency-derived de-bias sensitivity): GPS ≈ −0.4351.
    EXPECT_NEAR(b.iono_coeff, kIonoCoeff, 5e-4);
    EXPECT_NEAR(b.iono_coeff, -0.4351, 2e-3);
}

// Byte-identity crux (invariant #6): with the bridge disarmed (default) NO arc is
// ever accumulated, so arcBridge is always invalid.
TEST(MwBridge, DisarmedNeverAccumulates) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);           // setBridgeEnabled NOT called
    driveConst(det, "G01", 5, 100.0, 80.0, 70.0);
    EXPECT_FALSE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);
    EXPECT_FALSE(det.bridgeEnabled());
}

// Lag + maturity: the arc is invalid at count_lag < 2 (one processed epoch) and
// becomes valid only once a LAGGED pair exists — the same-epoch double-count guard.
TEST(MwBridge, LaggedMaturity) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G07", 2, 50.0, 40.0, 30.0);
    EXPECT_FALSE(det.arcBridge("G07", "L1C", "L2W", "L5Q").valid);  // count_lag == 1
    driveConst(det, "G07", 1, 50.0, 40.0, 30.0, /*tow0=*/100002.0);
    EXPECT_TRUE(det.arcBridge("G07", "L1C", "L2W", "L5Q").valid);   // count_lag == 2
}

// Data-gap refloat: a gap beyond the tolerance restarts the arc (invalid until it
// re-matures) — the pre-gap history is not carried across.
TEST(MwBridge, GapRefloat) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G05", 4, 100.0, 80.0, 70.0);
    ASSERT_TRUE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);
    det.detectLargeMwResets({ triObs("G05", 100000.0 + 5000.0, 100.0, 80.0, 70.0) });  // >120 s gap
    EXPECT_FALSE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);   // refloated (count_lag 0)
}

// Gross-jump refloat: a WL/EWL slip the funnel missed (GF_b jump ≥ kBridgeJumpResetM)
// resets the accumulator rather than poisoning the slow mean.  A +0.6 cyc L1 step is
// a ~0.5 m GF_b jump (> 0.4 m guard).
TEST(MwBridge, GrossJumpRefloat) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G09", 4, 100.0, 80.0, 70.0);
    ASSERT_TRUE(det.arcBridge("G09", "L1C", "L2W", "L5Q").valid);
    det.detectLargeMwResets({ triObs("G09", 100000.0 + 4, 100.6, 80.0, 70.0) });  // ΔGF ≈ 0.52 m
    EXPECT_FALSE(det.arcBridge("G09", "L1C", "L2W", "L5Q").valid);   // jump refloated
}

// Reset funnel: a per-signal reset on ANY band of the triple erases the bridge arc;
// a reset on an unrelated signal leaves it intact; resetSatellite / resetAll clear it.
TEST(MwBridge, ResetFunnelHooks) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);

    auto rewarm = [&](const std::string& sat) { driveConst(det, sat, 4, 100.0, 80.0, 70.0); };

    // (a) unrelated signal — arc survives.
    rewarm("G01");
    det.resetSatelliteSignal("G01", "L9X");
    EXPECT_TRUE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);

    // (b) the third (EWL) band — arc erased.
    det.resetSatelliteSignal("G01", "L5Q");
    EXPECT_FALSE(det.arcBridge("G01", "L1C", "L2W", "L5Q").valid);

    // (c) a WL-pair band — arc erased.
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

// LLI on a phase re-warms the bridge (it has no detection statistic of its own).
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
// M5 card UWL-1 (docs/SPEC_uwl_card.md) — per-constellation lane tiers.
// =====================================================================

namespace {

// Galileo carrier frequencies (Hz).
constexpr double kFE1  = 1575.42e6;    // E1
constexpr double kFE5a = 1176.45e6;    // E5a (band '5')
constexpr double kFE5b = 1207.14e6;    // E5b (band '7')
constexpr double kFE5  = 1191.795e6;   // E5 AltBOC (band '8')
const double kLamGalWl   = kC / (kFE1 - kFE5a);    // ≈ 0.7514 m
const double kLamGalUwl7 = kC / (kFE5b - kFE5a);   // ≈ 9.7685 m
const double kLamGalUwl8 = kC / (kFE5 - kFE5a);    // ≈ 19.537 m

// A Galileo tri-frequency phase-only observation on (E1, E5a, top band).
PppObservation galTriObs(const std::string& sat, double tow,
                         double le1, double le5a, double ltop,
                         const std::string& top_key) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::Galileo;
    obs.time      = GnssTime{ 2400, tow };
    obs.elevation = 45.0;
    obs.azimuth   = 0.0;
    obs.raw_observed = { { "L1C", le1 }, { "L5Q", le5a }, { top_key, ltop } };
    obs.observed     = obs.raw_observed;
    return obs;
}

std::map<char, MwLaneTier> armedTiers(char gal_top = '7', bool gal_mid = false,
                                      bool gal_dual = false) {
    std::map<char, MwLaneTier> t;
    for (const char cons : { 'G', 'E' })
        if (const auto tier = mwLaneTierOf(cons, gal_top, gal_mid, gal_dual)) t[cons] = *tier;
    return t;
}

// A Galileo QUAD-frequency phase-only observation (E1, E5a, E6, E5b).
PppObservation galQuadObs(const std::string& sat, double tow,
                          double le1, double le5a, double le6, double le5b) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::Galileo;
    obs.time      = GnssTime{ 2400, tow };
    obs.elevation = 45.0;
    obs.azimuth   = 0.0;
    obs.raw_observed = { { "L1C", le1 }, { "L5Q", le5a },
                         { "L6C", le6 }, { "L7Q", le5b } };
    obs.observed     = obs.raw_observed;
    return obs;
}

}  // namespace

// The tier table itself: GPS reproduces the legacy digits; Galileo (1,5,7|8) with
// b_top never degenerate against the shared band; untabled constellations are absent.
TEST(MwLaneTiers, TableDigitsAndNonDegeneracy) {
    const auto g = mwLaneTierOf('G');
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->b1, '1'); EXPECT_EQ(g->b2, '2'); EXPECT_EQ(g->b_top, '5');
    const auto e7 = mwLaneTierOf('E', '7');
    ASSERT_TRUE(e7.has_value());
    EXPECT_EQ(e7->b1, '1'); EXPECT_EQ(e7->b2, '5'); EXPECT_EQ(e7->b_top, '7');
    const auto e8 = mwLaneTierOf('E', '8');
    ASSERT_TRUE(e8.has_value());
    EXPECT_EQ(e8->b_top, '8');
    for (const auto& t : { *g, *e7, *e8 }) {
        EXPECT_NE(t.b_top, t.b2);   // the degenerate (L5Q,L5Q)-class pair is unrepresentable
        EXPECT_NE(t.b_top, t.b1);
        EXPECT_NE(t.b1, t.b2);
    }
    EXPECT_FALSE(mwLaneTierOf('C').has_value());   // BDS-3 deferred (clock divergence)
    EXPECT_FALSE(mwLaneTierOf('R').has_value());   // GLONASS excluded (FDMA)
}

// SPEC §1 fact 3, made loud: under the LEGACY digit scan a Galileo tri-freq obs
// (no band '2') forms NO bridge arc; with the tier table armed the (E5b,E5a) UWL
// bridge forms, λ/iono hand values check out.
TEST(MwBridge, GalileoUwlBridgeHandValues) {
    LogManager log; GnssConfig cfg;

    // Legacy: no arc under any plausible key (the silent GPS-only failure mode).
    MwSlipDetector legacy(cfg, log);
    legacy.setBridgeEnabled(true);
    for (int k = 0; k < 4; ++k)
        legacy.detectLargeMwResets({ galTriObs("E01", 100000.0 + k, 100.0, 80.0, 70.0, "L7Q") });
    EXPECT_FALSE(legacy.arcBridge("E01", "L1C", "L5Q", "L7Q").valid);

    // Armed table: the UWL bridge forms on (b1,b2,b_top) = (1,5,7).
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7'));
    for (int k = 0; k < 4; ++k)
        det.detectLargeMwResets({ galTriObs("E01", 100000.0 + k, 100.0, 80.0, 70.0, "L7Q") });
    const auto b = det.arcBridge("E01", "L1C", "L5Q", "L7Q");
    ASSERT_TRUE(b.valid);
    EXPECT_NEAR(b.lambda_wl,  kLamGalWl,   5e-3);   // (E1,E5a) WL pair
    EXPECT_NEAR(b.lambda_ewl, kLamGalUwl7, 5e-3);   // (E5b,E5a) top pair, λ POSITIVE
    // GF_b = λ_WL(φ_E1 − φ_E5a) − λ_UWL(φ_E5b − φ_E5a): the top-pair term is ordered
    // (hi,lo) by frequency even though the shared band E5a is the pair's LOW side.
    EXPECT_NEAR(b.gf_mean_m, b.lambda_wl * 20.0 - b.lambda_ewl * (-10.0), 1e-9);
    // Iono coefficient: f_ref²(1/(f_E1·f_E5a) − 1/(f_E5a·f_E5b)) — pair products.
    const double iono_hand = kF1 * kF1
        * (1.0 / (kFE1 * kFE5a) - 1.0 / (kFE5a * kFE5b));
    EXPECT_NEAR(b.iono_coeff, iono_hand, 5e-4);
}

// The user's ~19.5 m option: gal_top_band '8' pairs (E5 AltBOC, E5a), λ ≈ 19.537 m.
TEST(MwBridge, GalileoAltBocUwlLambda) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('8'));
    for (int k = 0; k < 4; ++k)
        det.detectLargeMwResets({ galTriObs("E03", 100000.0 + k, 100.0, 80.0, 70.0, "L8Q") });
    const auto b = det.arcBridge("E03", "L1C", "L5Q", "L8Q");
    ASSERT_TRUE(b.valid);
    EXPECT_NEAR(b.lambda_wl,  kLamGalWl,   5e-3);
    EXPECT_NEAR(b.lambda_ewl, kLamGalUwl8, 2e-2);
    EXPECT_NEAR(b.gf_mean_m, b.lambda_wl * 20.0 + b.lambda_ewl * 10.0, 1e-9);
}

// §6.1 GPS bit-identity at the unit level: pushing the armed table changes NOTHING
// for GPS (same digits fall out) — arc key, mean, variance, λs, iono coefficient.
TEST(MwBridge, GpsBitIdenticalUnderArmedTable) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector legacy(cfg, log), armed(cfg, log);
    legacy.setBridgeEnabled(true);
    armed.setBridgeEnabled(true);
    armed.setLaneTiers(armedTiers('7'));
    driveConst(legacy, "G01", 5, 100.0, 80.0, 70.0);
    driveConst(armed,  "G01", 5, 100.0, 80.0, 70.0);
    const auto bl = legacy.arcBridge("G01", "L1C", "L2W", "L5Q");
    const auto ba = armed.arcBridge("G01", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(bl.valid);
    ASSERT_TRUE(ba.valid);
    EXPECT_EQ(bl.count, ba.count);
    EXPECT_EQ(bl.gf_mean_m, ba.gf_mean_m);           // bit-exact, not NEAR
    EXPECT_EQ(bl.var_of_mean_m2, ba.var_of_mean_m2);
    EXPECT_EQ(bl.lambda_wl, ba.lambda_wl);
    EXPECT_EQ(bl.lambda_ewl, ba.lambda_ewl);
    EXPECT_EQ(bl.iono_coeff, ba.iono_coeff);
    // And a constellation ABSENT from the pushed table forms no arc (BDS-3 dark).
    PppObservation bds{};
    bds.satellite = "C06"; bds.system = System::BeiDou;
    bds.time = GnssTime{ 2400, 100000.0 }; bds.elevation = 45.0;
    bds.raw_observed = { { "L1P", 100.0 }, { "L2I", 80.0 }, { "L5P", 70.0 } };
    bds.observed = bds.raw_observed;
    for (int k = 0; k < 4; ++k) {
        bds.time = GnssTime{ 2400, 100000.0 + k };
        armed.detectLargeMwResets({ bds });
    }
    EXPECT_FALSE(armed.arcBridge("C06", "L1P", "L2I", "L5P").valid);
}

// UWL-1 §3.3 cold accessors — the bridge half: arcBridgeCold serves the UNLAGGED
// state at n ≥ 2 (one epoch ahead of the lagged arcBridge), never at n == 1.
TEST(MwBridge, ColdBridgeServesOneEpochEarly) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    driveConst(det, "G21", 1, 100.0, 80.0, 70.0);
    EXPECT_FALSE(det.arcBridge("G21", "L1C", "L2W", "L5Q").valid);       // count_lag 0
    EXPECT_FALSE(det.arcBridgeCold("G21", "L1C", "L2W", "L5Q").valid);   // n == 1: no var
    driveConst(det, "G21", 1, 100.0, 80.0, 70.0, /*tow0=*/100001.0);
    EXPECT_FALSE(det.arcBridge("G21", "L1C", "L2W", "L5Q").valid);       // count_lag 1
    const auto bc = det.arcBridgeCold("G21", "L1C", "L2W", "L5Q");       // n == 2: serves
    ASSERT_TRUE(bc.valid);
    EXPECT_EQ(bc.count, 2u);
    EXPECT_NEAR(bc.gf_mean_m, handGf(100.0, 80.0, 70.0), 1e-2);
    driveConst(det, "G21", 1, 100.0, 80.0, 70.0, /*tow0=*/100002.0);
    EXPECT_TRUE(det.arcBridge("G21", "L1C", "L2W", "L5Q").valid);        // count_lag 2
}

// UWL-1 §3.3 cold accessors — the MW half: arcZdMeanCold serves the UNLAGGED n ≥ 1
// state (variance NaN at n=1 for the caller's a-priori substitution; Welford from
// n ≥ 2), while the lagged arcZdMean contract is untouched.
TEST(MwBridge, ColdMwServesUnlaggedFromOneSample) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);

    auto dualObs = [](const std::string& sat, double tow) {
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2400, tow };
        obs.elevation = 45.0;
        obs.raw_observed = { { "L1C", 100.0 }, { "L2W", 80.0 },
                             { "C1C", 2.0e7 }, { "C2W", 2.0e7 } };
        obs.observed  = obs.raw_observed;
        return obs;
    };

    det.detectLargeMwResets({ dualObs("G31", 100000.0) });
    EXPECT_FALSE(det.arcZdMean("G31", "L1C", "L2W").valid);           // count_lag 0
    const auto c1 = det.arcZdMeanCold("G31", "L1C", "L2W");
    ASSERT_TRUE(c1.valid);                                            // n == 1 serves
    EXPECT_EQ(c1.count, 1u);
    EXPECT_TRUE(std::isfinite(c1.mean_m));
    EXPECT_FALSE(std::isfinite(c1.var_of_mean_m2));                   // caller substitutes σ_apriori

    det.detectLargeMwResets({ dualObs("G31", 100001.0) });
    EXPECT_FALSE(det.arcZdMean("G31", "L1C", "L2W").valid);           // count_lag 1
    const auto c2 = det.arcZdMeanCold("G31", "L1C", "L2W");
    ASSERT_TRUE(c2.valid);
    EXPECT_EQ(c2.count, 2u);
    EXPECT_TRUE(std::isfinite(c2.var_of_mean_m2));                    // Welford takes over

    det.detectLargeMwResets({ dualObs("G31", 100002.0) });
    const auto zl = det.arcZdMean("G31", "L1C", "L2W");
    ASSERT_TRUE(zl.valid);                                            // lagged contract intact
    EXPECT_NEAR(zl.mean_m, c2.mean_m, 1e-9);                          // constant input
}

// =====================================================================
// E6 mid-ladder bridge hop (uwl_mid_enable; census-motivated Jul-16).
// =====================================================================

// The mid-armed tier feeds THREE triples per Galileo observation: the base
// (WL vs top), hop-hi (mid lane vs top), hop-lo (WL vs mid lane) — with hand
// λs, and the exact chain identity GF_direct = GF_hi + GF_lo (the two-hop
// decomposition is algebraically lossless; the GAIN is the intermediate
// rounding at λ_MID, which strips the top hop's noise before the WL).
TEST(MwBridge, GalileoMidHopTriplesAndChainIdentity) {
    constexpr double kFE6 = 1278.75e6;                 // E6
    const double kLamGalMid = kC / (kFE6 - kFE5a);     // ≈ 2.931 m

    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7', /*gal_mid=*/true));
    for (int k = 0; k < 4; ++k)
        det.detectLargeMwResets({ galQuadObs("E05", 100000.0 + k,
                                             100.0, 80.0, 90.0, 70.0) });

    const auto bd = det.arcBridge("E05", "L1C", "L5Q", "L7Q");   // direct
    const auto bh = det.arcBridge("E05", "L6C", "L5Q", "L7Q");   // hop-hi
    const auto bl = det.arcBridge("E05", "L1C", "L5Q", "L6C");   // hop-lo
    ASSERT_TRUE(bd.valid);
    ASSERT_TRUE(bh.valid);
    ASSERT_TRUE(bl.valid);
    EXPECT_NEAR(bd.lambda_wl,  kLamGalWl,   5e-3);
    EXPECT_NEAR(bd.lambda_ewl, kLamGalUwl7, 5e-3);
    EXPECT_NEAR(bh.lambda_wl,  kLamGalMid,  5e-3);   // mid lane (E6,E5a)
    EXPECT_NEAR(bh.lambda_ewl, kLamGalUwl7, 5e-3);   // vs the top lane
    EXPECT_NEAR(bl.lambda_wl,  kLamGalWl,   5e-3);   // WL lane
    EXPECT_NEAR(bl.lambda_ewl, kLamGalMid,  5e-3);   // vs the mid lane
    // Hand values: GF_hi = λ_MID(φ6−φ5a) − λ_UWL(φ7−φ5a); GF_lo = λ_WL(φ1−φ5a)
    // − λ_MID(φ6−φ5a); both top-pair terms ordered (hi,lo) by frequency.
    EXPECT_NEAR(bh.gf_mean_m, bh.lambda_wl * 10.0 - bh.lambda_ewl * (-10.0), 1e-9);
    EXPECT_NEAR(bl.gf_mean_m, bl.lambda_wl * 20.0 - bl.lambda_ewl * 10.0, 1e-9);
    // Chain identity (exact): direct = hop-hi + hop-lo.
    EXPECT_NEAR(bd.gf_mean_m, bh.gf_mean_m + bl.gf_mean_m, 1e-9);
    // Iono coefficients compose the same way: C_direct = C_hi + C_lo.
    EXPECT_NEAR(bd.iono_coeff, bh.iono_coeff + bl.iono_coeff, 1e-12);
}

// Mid-off (the default): only the base triple forms — byte-compatibility of
// the armed-without-mid shape with the pre-mid build.
TEST(MwBridge, MidOffFormsBaseTripleOnly) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7', /*gal_mid=*/false));
    for (int k = 0; k < 4; ++k)
        det.detectLargeMwResets({ galQuadObs("E05", 100000.0 + k,
                                             100.0, 80.0, 90.0, 70.0) });
    EXPECT_TRUE(det.arcBridge("E05", "L1C", "L5Q", "L7Q").valid);
    EXPECT_FALSE(det.arcBridge("E05", "L6C", "L5Q", "L7Q").valid);
    EXPECT_FALSE(det.arcBridge("E05", "L1C", "L5Q", "L6C").valid);
}

// Tier-table contract: b_mid arms only for Galileo and only on request.
TEST(MwLaneTiers, MidRungTable) {
    EXPECT_EQ(mwLaneTierOf('E', '7', true)->b_mid, '6');
    EXPECT_EQ(mwLaneTierOf('E', '8', true)->b_mid, '6');
    EXPECT_EQ(mwLaneTierOf('E', '7', false)->b_mid, '\0');
    EXPECT_EQ(mwLaneTierOf('G', '7', true)->b_mid, '\0');   // GPS has no mid frequency
}

// =====================================================================
// Increment 5: dual-top tier (uwl_dual_top) — preferred '8' / fallback '7'.
// =====================================================================

// Tier-table contract: the dual tier prefers '8' with the '7' fallback,
// ignores gal_top_band, keeps the primary pair, composes with the mid rung;
// GPS (no sibling pair) and single-top tiers never grow a fallback.
TEST(MwLaneTiers, DualTopTable) {
    const auto ed = mwLaneTierOf('E', '7', false, true);   // gal_top_band IGNORED
    ASSERT_TRUE(ed.has_value());
    EXPECT_EQ(ed->b1, '1');  EXPECT_EQ(ed->b2, '5');
    EXPECT_EQ(ed->b_top, '8');
    EXPECT_EQ(ed->b_top2, '7');
    EXPECT_EQ(ed->b_mid, '\0');
    EXPECT_EQ(mwLaneTierOf('E', '8', true, true)->b_mid, '6');    // mid composes
    EXPECT_EQ(mwLaneTierOf('E', '8', true, true)->b_top2, '7');
    EXPECT_EQ(mwLaneTierOf('G', '7', false, true)->b_top2, '\0'); // GPS single-top
    EXPECT_EQ(mwLaneTierOf('E', '7', false, false)->b_top2, '\0');// dual off
}

// The sibling identity's frequency basis: f_E5 = (f_E5a + f_E5b)/2 EXACTLY
// (Galileo ICD), so λ(8,5) = 2·λ(7,5) and the closure expectation
// N(7,5) = 2·N(8,5) is an exact integer relation — verified against the
// PRODUCTION frequency table via the bridge λ cache, not the test constants.
TEST(MwBridge, DualTopSiblingLambdaIdentity) {
    EXPECT_NEAR(kFE5, 0.5 * (kFE5a + kFE5b), 1e-3);   // hand constants agree

    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7', false, /*gal_dual=*/true));
    PppObservation obs{};
    obs.satellite = "E09"; obs.system = System::Galileo; obs.elevation = 45.0;
    obs.raw_observed = { { "L1C", 100.0 }, { "L5Q", 80.0 },
                         { "L7Q", 90.0 },  { "L8Q", 85.0 } };
    obs.observed = obs.raw_observed;
    for (int k = 0; k < 4; ++k) {
        obs.time = GnssTime{ 2400, 100000.0 + k };
        det.detectLargeMwResets({ obs });
    }
    const auto b8 = det.arcBridge("E09", "L1C", "L5Q", "L8Q");   // preferred triple
    const auto b7 = det.arcBridge("E09", "L1C", "L5Q", "L7Q");   // fallback triple
    ASSERT_TRUE(b8.valid);
    ASSERT_TRUE(b7.valid);
    // λ(8,5) = 2·λ(7,5) to the production table's precision (exact by ICD).
    EXPECT_NEAR(b8.lambda_ewl / b7.lambda_ewl, 2.0, 1e-9);
    EXPECT_NEAR(b8.lambda_ewl, kLamGalUwl8, 2e-2);
    EXPECT_NEAR(b7.lambda_ewl, kLamGalUwl7, 5e-3);
}

// The fallback triple must form even when the PREFERRED band is untracked at
// the (rx,sat) — that is the point of the fallback rung (the legacy early-out
// would have dropped the whole observation for want of band 8).
TEST(MwBridge, DualTopFallbackFormsWithoutPreferredBand) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7', false, /*gal_dual=*/true));
    for (int k = 0; k < 4; ++k)   // tri-freq obs: bands 1,5,7 — NO band 8
        det.detectLargeMwResets({ galTriObs("E11", 100000.0 + k, 100.0, 80.0, 90.0, "L7Q") });
    EXPECT_FALSE(det.arcBridge("E11", "L1C", "L5Q", "L8Q").valid);  // preferred absent
    const auto b7 = det.arcBridge("E11", "L1C", "L5Q", "L7Q");      // fallback forms
    ASSERT_TRUE(b7.valid);
    EXPECT_NEAR(b7.lambda_ewl, kLamGalUwl7, 5e-3);
    // And single-top '8' (dual OFF) keeps the legacy early-out: nothing forms.
    MwSlipDetector single(cfg, log);
    single.setBridgeEnabled(true);
    single.setLaneTiers(armedTiers('8', false, /*gal_dual=*/false));
    for (int k = 0; k < 4; ++k)
        single.detectLargeMwResets({ galTriObs("E11", 100000.0 + k, 100.0, 80.0, 90.0, "L7Q") });
    EXPECT_FALSE(single.arcBridge("E11", "L1C", "L5Q", "L8Q").valid);
    EXPECT_FALSE(single.arcBridge("E11", "L1C", "L5Q", "L7Q").valid);
}

// =====================================================================
// Per-sample bridge iono de-bias (network.mw_wl.bridge_sample_debias) —
// the 60-epoch age-bias fix.  The raw GF Welford accumulates C_iono·I over
// the arc's LIFETIME while the read-time de-bias subtracts C_iono·ı̃(now);
// per-sample folding (GF_k − C_iono·ı̃(t_k)) makes the statistic stationary.
// =====================================================================

// THE fix gate: under a linear iono drift injected into the phases, the raw
// arc's mean is biased by C_iono·mean(I) and its variance is drift-inflated;
// the armed arc folds stationary samples — mean = the true constant, variance
// collapses.  (Drift injected via the L5 phase so gf_k = gf0 + C_iono·I_k;
// the matching ı̃_k push uses the SAME C_iono frame the detector computes.)
TEST(MwBridge, PerSampleDebiasStationaryUnderIonoDrift) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector raw(cfg, log), deb(cfg, log);
    raw.setBridgeEnabled(true);
    deb.setBridgeEnabled(true);
    deb.setBridgePerSampleDebias(true);

    const double gf0 = handGf(100.0, 80.0, 70.0);
    std::vector<double> iono;   // I_k ramp (m): 0.0, 0.1, ... — a strong 30-min-class drift
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

// A sample without a pushed estimate is SKIPPED (counted), never folded raw
// into a de-biased arc (mixed frames are the disease); the arc simply warms
// once estimates arrive.
TEST(MwBridge, PerSampleDebiasSkipsWithoutEstimate) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setBridgePerSampleDebias(true);
    driveConst(det, "G05", 3, 100.0, 80.0, 70.0);            // no estimates pushed
    EXPECT_EQ(det.bridgeDebiasSkipped(), 3u);
    EXPECT_FALSE(det.arcBridge("G05", "L1C", "L2W", "L5Q").valid);
    EXPECT_FALSE(det.arcBridgeCold("G05", "L1C", "L2W", "L5Q").valid);
    det.setBridgeIonoEstimates({ { "G05", 0.25 } });          // estimates arrive
    driveConst(det, "G05", 3, 100.0, 80.0, 70.0, /*tow0=*/100003.0);
    EXPECT_EQ(det.bridgeDebiasSkipped(), 3u);                 // no further skips
    const auto b = det.arcBridge("G05", "L1C", "L2W", "L5Q");
    ASSERT_TRUE(b.valid);
    EXPECT_NEAR(b.gf_mean_m, handGf(100.0, 80.0, 70.0) - kIonoCoeff * 0.25, 1e-9);
}

// Default OFF is byte-neutral: pushed estimates are ignored, accumulation is
// bit-identical to a never-pushed detector, and nothing is counted skipped.
TEST(MwBridge, PerSampleDebiasOffIsRawByteIdentical) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector plain(cfg, log), pushed(cfg, log);
    plain.setBridgeEnabled(true);
    pushed.setBridgeEnabled(true);                            // debias NOT armed
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

// Dual + mid: BOTH hop-hi triples form ((6,5,8) and (6,5,7)) plus the shared
// hop-lo (1,5,6); the chain identity holds against EACH top rung.
TEST(MwBridge, DualTopMidFeedsBothHopHiTriples) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    det.setBridgeEnabled(true);
    det.setLaneTiers(armedTiers('7', /*gal_mid=*/true, /*gal_dual=*/true));
    PppObservation obs{};
    obs.satellite = "E12"; obs.system = System::Galileo; obs.elevation = 45.0;
    obs.raw_observed = { { "L1C", 100.0 }, { "L5Q", 80.0 }, { "L6C", 95.0 },
                         { "L7Q", 90.0 },  { "L8Q", 85.0 } };
    obs.observed = obs.raw_observed;
    for (int k = 0; k < 4; ++k) {
        obs.time = GnssTime{ 2400, 100000.0 + k };
        det.detectLargeMwResets({ obs });
    }
    const auto d8 = det.arcBridge("E12", "L1C", "L5Q", "L8Q");
    const auto d7 = det.arcBridge("E12", "L1C", "L5Q", "L7Q");
    const auto h8 = det.arcBridge("E12", "L6C", "L5Q", "L8Q");
    const auto h7 = det.arcBridge("E12", "L6C", "L5Q", "L7Q");
    const auto lo = det.arcBridge("E12", "L1C", "L5Q", "L6C");
    ASSERT_TRUE(d8.valid); ASSERT_TRUE(d7.valid);
    ASSERT_TRUE(h8.valid); ASSERT_TRUE(h7.valid); ASSERT_TRUE(lo.valid);
    EXPECT_NEAR(d8.gf_mean_m, h8.gf_mean_m + lo.gf_mean_m, 1e-9);   // chain vs '8'
    EXPECT_NEAR(d7.gf_mean_m, h7.gf_mean_m + lo.gf_mean_m, 1e-9);   // chain vs '7'
    EXPECT_NEAR(d8.iono_coeff, h8.iono_coeff + lo.iono_coeff, 1e-12);
    EXPECT_NEAR(d7.iono_coeff, h7.iono_coeff + lo.iono_coeff, 1e-12);
}
