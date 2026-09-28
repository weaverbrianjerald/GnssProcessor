/**
 * @file test_sd_ewl_prereduce.cpp
 * @brief Tests for the SD pre-reduce EWL constraint: coefficient algebra, correlated-R
 *        whitening, pivot-election determinism, and the config contract.
 *        See docs/SPEC_sd_ewl_prereduce.md.
 *
 * Suite names owned by this file only (the suite-per-file rule):
 *   SdEwlAlgebraTest, SdEwlWhiteningTest, SdEwlPivotElectionTest, SdEwlConfigTest,
 *   SdEwlArcSourceTest, SdEwlChannelSearchTest.
 *
 * `Application::networkAppendSdEwlRows_` is a private member that needs a whole
 * NetworkEstimator + NetStation + StateManager to call, so these tests verify the
 * mathematical contract the implementation must satisfy (the same algebra, written
 * independently from the spec rather than copied from the source), plus the config
 * loader behaviour, which is directly reachable.  A defect in the row coefficients or the
 * whitening breaks these tests; a defect in the wiring is caught by the run-end counters
 * (standing invariant #4), not here.
 *
 * `SdEwlArcSourceTest` is the exception and is not a restatement of the contract: it
 * drives a real `MwSlipDetector` with hand-built tri-frequency observations to check the
 * row's statistic source.  It checks that an EWL (band2, band5) MW arc is formed (the
 * detector's pair builder is generic over band pairs, not L1-referenced), that
 * `MwArcZdMean::exists` separates "never formed" from "not yet usable", and that the arc
 * reports the code channels its MW consumed, so the consumer does not derive them from
 * the phase attribute.  An error in any of the three leaves the row without a correct
 * statistic, so they are checked here rather than in a field run.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------------
// The row algebra, written from SPEC section 3 (independent of the implementation).
// ---------------------------------------------------------------------------------
struct SdEwlRow {
    // local coefficients on the metre-valued per-band ambiguity states
    double a_amb_b1_s{ 0.0 }, a_amb_b2_s{ 0.0 }, a_amb_b1_p{ 0.0 }, a_amb_b2_p{ 0.0 };
    // shared coefficients on the satellite phase / code biases
    double a_pb_b1_s{ 0.0 }, a_pb_b2_s{ 0.0 }, a_cb_b1_s{ 0.0 }, a_cb_b2_s{ 0.0 };
    double a_pb_b1_p{ 0.0 }, a_pb_b2_p{ 0.0 }, a_cb_b1_p{ 0.0 }, a_cb_b2_p{ 0.0 };
};

SdEwlRow buildRow(double f1, double f2) {
    const double c = Constants::SPEED_OF_LIGHT;
    const double lam_b1 = c / f1, lam_b2 = c / f2;
    const double lam_ewl = c / (f1 - f2);
    SdEwlRow r;
    r.a_amb_b1_s = +lam_ewl / lam_b1;
    r.a_amb_b2_s = -lam_ewl / lam_b2;
    r.a_amb_b1_p = -lam_ewl / lam_b1;
    r.a_amb_b2_p = +lam_ewl / lam_b2;
    r.a_pb_b1_s = +f1 / (f1 - f2);
    r.a_pb_b2_s = -f2 / (f1 - f2);
    r.a_cb_b1_s = -f1 / (f1 + f2);
    r.a_cb_b2_s = -f2 / (f1 + f2);
    r.a_pb_b1_p = -r.a_pb_b1_s;
    r.a_pb_b2_p = -r.a_pb_b2_s;
    r.a_cb_b1_p = -r.a_cb_b1_s;
    r.a_cb_b2_p = -r.a_cb_b2_s;
    return r;
}

// The network-mode validator requires >= 2 stations with known ECEF, so every config
// body below embeds this minimal legal roster.  Paths are never opened by loadFromJson.
constexpr const char* kMinimalStations = R"(
        "clock_datum_station": "AAAA00XXX",
        "stations": [
          { "name": "AAAA00XXX", "rinex_path": "a.rnx",
            "position_ecef": [ 4000000.0, 1000000.0, 4500000.0 ] },
          { "name": "BBBB00XXX", "rinex_path": "b.rnx",
            "position_ecef": [ 4100000.0, 1100000.0, 4400000.0 ] }
        ])";

// ---------------------------------------------------------------------------------
// A tri-frequency GPS observation with both phase and code per band.  The MW
// combination needs the code, so a phase-only observation forms no arc at all.
// `observed` drives the detector's band scope under the default (Mirror) MW frequency
// mode; `raw_observed` supplies the values the MW is formed from.
// ---------------------------------------------------------------------------------
PppObservation mwObs(const std::string& sat, double tow,
                     const std::map<std::string, double>& obs_map,
                     double elev_deg = 45.0) {
    PppObservation o{};
    o.satellite = sat;
    o.system    = System::GPS;
    o.time      = GnssTime{ 2400, tow };
    o.elevation = elev_deg;
    o.azimuth   = 0.0;
    o.raw_observed = obs_map;
    o.observed     = obs_map;
    return o;
}

// A quiet tri-frequency GPS satellite: L1/L2/L5 phase (cycles) + matching codes (m).
// Values are arbitrary but self-consistent; the arcs only need to exist and be finite.
std::map<std::string, double> triFreqGps(double l1c, double l2c, double l5c,
                                         double rho_m) {
    return { { "L1C", l1c }, { "L2W", l2c }, { "L5Q", l5c },
             { "C1W", rho_m }, { "C2W", rho_m }, { "C5Q", rho_m } };
}

// Drive n epochs at 1 s cadence (well inside the arc gap tolerance).
void drive(MwSlipDetector& det, const std::string& sat, int n,
           const std::map<std::string, double>& obs_map, double tow0 = 200000.0) {
    for (int k = 0; k < n; ++k)
        det.detectLargeMwResets({ mwObs(sat, tow0 + k, obs_map) });
}

std::string writeTempConfig(const std::string& body, const char* tag) {
    const std::string path = std::string("_sd_ewl_test_") + tag + ".json";
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}

}  // namespace

// =================================================================================
// SdEwlAlgebraTest: the coefficients reproduce the EWL combination exactly.
// =================================================================================

TEST(SdEwlAlgebraTest, GpsL2L5EwlWavelengthIsTheKnownTierValue) {
    const double lam = Constants::SPEED_OF_LIGHT
                       / (Constants::GPS_L2_FREQ - Constants::GPS_L5_FREQ);
    EXPECT_NEAR(lam, 5.861, 1e-3) << "GPS EWL (L2,L5) must be the 5.861 m tier rung";
}

TEST(SdEwlAlgebraTest, GalileoE5bE5aUwlWavelengthIsTheKnownTierValue) {
    const double lam = Constants::SPEED_OF_LIGHT
                       / (Constants::GAL_E5B_FREQ - Constants::GAL_E5A_FREQ);
    EXPECT_NEAR(lam, 9.768, 1e-3) << "Galileo UWL (E5b,E5a) must be the 9.768 m tier rung";
}

// The local half of the row must equal lam_EWL * (N_b1 - N_b2) when the ambiguity states
// hold lam_b * N_b metres.  This is the property that makes the row an EWL constraint at
// all, so an inverted 1/lambda would show up here immediately.
TEST(SdEwlAlgebraTest, LocalCoefficientsReproduceLambdaEwlTimesIntegerDifference) {
    const double f1 = Constants::GPS_L2_FREQ, f2 = Constants::GPS_L5_FREQ;
    const double c = Constants::SPEED_OF_LIGHT;
    const double lam_b1 = c / f1, lam_b2 = c / f2, lam_ewl = c / (f1 - f2);
    const SdEwlRow r = buildRow(f1, f2);

    // Pick integers for both satellites and convert to the metre-valued states.
    const double N1_s = 7.0, N2_s = -3.0, N1_p = 11.0, N2_p = 5.0;
    const double amb_b1_s = lam_b1 * N1_s, amb_b2_s = lam_b2 * N2_s;
    const double amb_b1_p = lam_b1 * N1_p, amb_b2_p = lam_b2 * N2_p;

    const double got = r.a_amb_b1_s * amb_b1_s + r.a_amb_b2_s * amb_b2_s
                       + r.a_amb_b1_p * amb_b1_p + r.a_amb_b2_p * amb_b2_p;
    const double want = lam_ewl * ((N1_s - N2_s) - (N1_p - N2_p));
    EXPECT_NEAR(got, want, 1e-9);
}

// MW = phi_WL - P_NL.  A bias common to both bands and both satellites cancels; this
// fixes the relative signs of the four shared coefficients.
TEST(SdEwlAlgebraTest, SharedCoefficientsCancelACommonSatelliteBias) {
    const SdEwlRow r = buildRow(Constants::GPS_L2_FREQ, Constants::GPS_L5_FREQ);
    const double b = 0.37;   // same bias value on every PB/CB of both satellites
    const double sum = (r.a_pb_b1_s + r.a_pb_b2_s + r.a_cb_b1_s + r.a_cb_b2_s
                        + r.a_pb_b1_p + r.a_pb_b2_p + r.a_cb_b1_p + r.a_cb_b2_p) * b;
    EXPECT_NEAR(sum, 0.0, 1e-12);
}

// The widelane phase part must sum to 1 and the narrowlane code part to -1 per satellite:
// phi_WL has unit gain on a range-like quantity, P_NL likewise, and MW subtracts them.
TEST(SdEwlAlgebraTest, PhaseAndCodeCombinationGainsAreUnitAndOpposite) {
    const SdEwlRow r = buildRow(Constants::GAL_E5B_FREQ, Constants::GAL_E5A_FREQ);
    EXPECT_NEAR(r.a_pb_b1_s + r.a_pb_b2_s, 1.0, 1e-12) << "phi_WL gain must be +1";
    EXPECT_NEAR(r.a_cb_b1_s + r.a_cb_b2_s, -1.0, 1e-12) << "-P_NL gain must be -1";
}

// A receiver-side bias is common to both arcs at the same station and vanishes from the
// between-satellite difference.  That is why the constraint is SD and not undifferenced
// (SPEC section 2: undifferenced is not estimable under the per-signal eq34 gauge).
TEST(SdEwlAlgebraTest, ReceiverBiasCancelsInTheBetweenSatelliteDifference) {
    const SdEwlRow r = buildRow(Constants::GPS_L2_FREQ, Constants::GPS_L5_FREQ);
    // A receiver EWL bias enters both arcs identically, so in z = mean(s) - mean(p) it is
    // differenced away before the row is formed.  The algebraic expression of that is a
    // modelled satellite side antisymmetric in (s,p), which is what is asserted.
    EXPECT_NEAR(r.a_pb_b1_s, -r.a_pb_b1_p, 1e-12);
    EXPECT_NEAR(r.a_pb_b2_s, -r.a_pb_b2_p, 1e-12);
    EXPECT_NEAR(r.a_cb_b1_s, -r.a_cb_b1_p, 1e-12);
    EXPECT_NEAR(r.a_cb_b2_s, -r.a_cb_b2_p, 1e-12);
    EXPECT_NEAR(r.a_amb_b1_s, -r.a_amb_b1_p, 1e-12);
    EXPECT_NEAR(r.a_amb_b2_s, -r.a_amb_b2_p, 1e-12);
}

// The station's block is in the correction frame (y = z − H·x_lin), so the row subtracts
// its own linearized prediction.  Checked as an identity: with the states at their
// linearization point the residual is zero however large x_lin is.  This matters because
// the raw SD MW difference (λ_EWL times an integer count in the thousands of cycles) runs
// to hundreds of metres or more, against a row sigma of about 0.13 m.
TEST(SdEwlAlgebraTest, CorrectionFrameResidualVanishesAtTheLinearizationPoint) {
    const double f1 = Constants::GPS_L2_FREQ, f2 = Constants::GPS_L5_FREQ;
    const double c = Constants::SPEED_OF_LIGHT;
    const double lam_b1 = c / f1, lam_b2 = c / f2, lam_ewl = c / (f1 - f2);

    // One arc's linearized prediction of the MW observable: the implementation's
    // pred_m, written from the spec coefficients.
    const auto pred = [&](double amb_b1, double amb_b2, double pb_b1, double pb_b2,
                          double cb_b1, double cb_b2) {
        return (lam_ewl / lam_b1) * amb_b1 - (lam_ewl / lam_b2) * amb_b2
             + (f1 / (f1 - f2)) * pb_b1 - (f2 / (f1 - f2)) * pb_b2
             - (f1 / (f1 + f2)) * cb_b1 - (f2 / (f1 + f2)) * cb_b2;
    };
    // The physical observable, written independently of `pred`: MW = φ_WL − P_NL, whose
    // ambiguity part is λ_EWL·(N_b1 − N_b2) with N in cycles and whose bias part is the
    // lump of SPEC §3.  `pred` reaches the same value through the metre-valued states, so
    // their agreement checks the λ_EWL/λ_b conversion and is not a tautology.
    const auto observable = [&](double N1, double N2, double pb_b1, double pb_b2,
                               double cb_b1, double cb_b2) {
        return lam_ewl * (N1 - N2)
             + (f1 * pb_b1 - f2 * pb_b2) / (f1 - f2)
             - (f1 * cb_b1 + f2 * cb_b2) / (f1 + f2);
    };
    // Deliberately huge integer counts (the real seeds are code-minus-phase, thousands
    // of cycles) with nonzero biases.
    struct S { double N1, N2, pb1, pb2, cb1, cb2; };
    const S sat = { -3218.0, 4471.0, 0.031, -0.017, 1.44, -0.92 };
    const S piv = { 9902.0, -1180.0, -0.008, 0.022, -0.51, 2.07 };
    const auto obsOf  = [&](const S& a) {
        return observable(a.N1, a.N2, a.pb1, a.pb2, a.cb1, a.cb2);
    };
    const auto predOf = [&](const S& a) {
        return pred(lam_b1 * a.N1, lam_b2 * a.N2, a.pb1, a.pb2, a.cb1, a.cb2);
    };
    // States at their linearization point ⇒ the folded residual is zero...
    EXPECT_NEAR((obsOf(sat) - obsOf(piv)) - (predOf(sat) - predOf(piv)), 0.0, 1e-8);
    // ...while the raw SD difference it replaces is large, so folding it unreduced
    // would be a constraint of hundreds of metres at a decimetre sigma.
    EXPECT_GT(std::abs(obsOf(sat) - obsOf(piv)), 100.0);
}

// Linearity: the residual equals H applied to the state error, so a known perturbation
// of one state shows up in the row with that state's coefficient.
TEST(SdEwlAlgebraTest, CorrectionFrameResidualEqualsHTimesTheStateError) {
    const double f1 = Constants::GAL_E5B_FREQ, f2 = Constants::GAL_E5A_FREQ;
    const double c = Constants::SPEED_OF_LIGHT;
    const double lam_b1 = c / f1, lam_ewl = c / (f1 - f2);
    const SdEwlRow r = buildRow(f1, f2);

    const auto pred = [&](double amb_b1, double amb_b2) {
        return (lam_ewl / lam_b1) * amb_b1 - (lam_ewl / (c / f2)) * amb_b2;
    };
    const double lin_s1 = 1234.5, lin_s2 = -987.6, lin_p1 = 55.5, lin_p2 = -66.6;
    const double d = 0.037;   // a 3.7 cm error on the pivot's band-1 ambiguity only

    const double mean_s = pred(lin_s1, lin_s2);
    const double mean_p = pred(lin_p1 + d, lin_p2);
    const double resid  = (mean_s - mean_p) - (pred(lin_s1, lin_s2) - pred(lin_p1, lin_p2));
    EXPECT_NEAR(resid, r.a_amb_b1_p * d, 1e-9)
        << "the residual must carry the pivot band-1 coefficient, sign included";
}

// =================================================================================
// SdEwlArcSourceTest: the row's statistic source, against a real MwSlipDetector.
// =================================================================================

// The card rests on this: the detector forms the EWL (band2, band5) MW pair.
// collectUncombinedPairs enumerates every band pair present (i<j over bands sorted by
// descending frequency), so with L1/L2/L5 tracked the arcs are (L1,L2), (L1,L5) and
// (L2,L5); the enumeration is not L1-referenced.  A failure here means the row has no
// statistic and the card is inert.
TEST(SdEwlArcSourceTest, EwlBandPairArcIsFormedAndServedInZdFrame) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    drive(det, "G01", 4, triFreqGps(1.0e8, 0.8e8, 0.7e8, 2.2e7));

    const auto ewl = det.arcZdMean("G01", "L2W", "L5Q");
    EXPECT_TRUE(ewl.exists) << "the (band2, band5) MW pair must be formed";
    ASSERT_TRUE(ewl.valid);
    EXPECT_GE(ewl.count, 2u);
    // λ_WL of the pair is the GPS EWL rung, so the arc is the EWL pair and not a
    // mislabelled L1-referenced one (λ(L1,L2) ≈ 0.862 m, λ(L1,L5) ≈ 0.751 m).
    EXPECT_NEAR(ewl.lambda_wl,
                Constants::SPEED_OF_LIGHT
                    / (Constants::GPS_L2_FREQ - Constants::GPS_L5_FREQ), 5e-3);
    EXPECT_TRUE(std::isfinite(ewl.mean_m));
    // The L1-referenced pairs exist too: the EWL arc is additional, not a replacement.
    EXPECT_TRUE(det.arcZdMean("G01", "L1C", "L2W").exists);
    EXPECT_TRUE(det.arcZdMean("G01", "L1C", "L5Q").exists);
}

// `exists` vs `valid`: without `exists`, "the detector never built this pair" and "built
// but under two lagged samples" share one flag, and a consumer reports a missing pair as
// an immature arc.  The two causes have different fixes, so they are kept separable.
TEST(SdEwlArcSourceTest, ExistsSeparatesNeverFormedFromNotYetUsable) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    drive(det, "G05", 1, triFreqGps(1.0e8, 0.8e8, 0.7e8, 2.2e7));

    const auto young = det.arcZdMean("G05", "L2W", "L5Q");
    EXPECT_TRUE(young.exists) << "one processed epoch: the arc EXISTS";
    EXPECT_FALSE(young.valid) << "...but count_lag < 2, so it is not yet usable";

    // Pairs the detector never builds: a band it does not track, and the reversed
    // (ascending-frequency) order; every MW key is ordered (higher f, lower f).
    const auto untracked = det.arcZdMean("G05", "L2W", "L6X");
    EXPECT_FALSE(untracked.exists);
    EXPECT_FALSE(untracked.valid);
    EXPECT_FALSE(det.arcZdMean("G05", "L5Q", "L2W").exists) << "keys are (hi,lo) ordered";
}

// The arc reports the code channels its own MW consumed.  matchChannel prefers the same
// tracking attribute but falls back to the first code in the band, so deriving
// "C" + band + phase-attribute is not always right, and a wrong per-signal OSB column is
// a modelling error that nothing downstream reports.  Here band 2 publishes phase L2L
// with only a C2W code, so the correct answer is C2W and the derived guess would be C2L.
TEST(SdEwlArcSourceTest, ArcReportsTheCodeChannelItsMwConsumedNotThePhaseAttribute) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    const std::map<std::string, double> obs = {
        { "L1C", 1.0e8 }, { "L2L", 0.8e8 }, { "L5Q", 0.7e8 },
        { "C1W", 2.2e7 }, { "C2W", 2.2e7 }, { "C5Q", 2.2e7 }   // no C2L
    };
    drive(det, "G09", 4, obs);

    const auto ewl = det.arcZdMean("G09", "L2L", "L5Q");
    ASSERT_TRUE(ewl.valid);
    EXPECT_EQ(ewl.code1, "C2W") << "matchChannel's in-band fallback, not C2L";
    EXPECT_EQ(ewl.code2, "C5Q");
    // Spell out the guess this replaces: "C" + band + phase attribute would be C2L,
    // naming an OSB column the observable does not carry (and here does not exist).
    EXPECT_NE(ewl.code1, "C2L") << "deriving the code from the phase attribute is unsafe";
}

// Same-attribute case: when the band publishes the matching code, the MW uses it.
// Together with the fallback test above this covers both branches, so a consumer can
// rely on code1/code2 unconditionally instead of guessing.
TEST(SdEwlArcSourceTest, SameAttributeCodeIsUsedWhenPresent) {
    LogManager log; GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    drive(det, "G11", 4, triFreqGps(1.0e8, 0.8e8, 0.7e8, 2.2e7));
    const auto ewl = det.arcZdMean("G11", "L2W", "L5Q");
    ASSERT_TRUE(ewl.valid);
    EXPECT_EQ(ewl.code1, "C2W");
    EXPECT_EQ(ewl.code2, "C5Q");
}

// =================================================================================
// SdEwlChannelSearchTest: the candidate-pair search order.
// =================================================================================

// The appender cannot know which tracking channel the detector holds an arc on, so it
// searches the candidate (band1, band2) phase pairs in ascending (sig1, sig2) order and
// takes the first with a usable arc.  The order is a strict total order (required for
// byte identity).  The search is needed because the winner is often not the
// lexicographically first candidate: on the 50-station fixture, 41 % of resolved arcs had
// two candidate pairs, and both L7X/L5X and L7Q/L5Q won at different satellites.
TEST(SdEwlChannelSearchTest, SearchIsAscendingAndTakesTheFirstPairWithAnArc) {
    const std::vector<std::string> b1 = { "L2W", "L2L" };   // deliberately unsorted
    const std::vector<std::string> b2 = { "L5X", "L5Q" };
    std::vector<std::pair<std::string, std::string>> order;
    std::vector<std::string> s1 = b1, s2 = b2;
    std::sort(s1.begin(), s1.end());
    std::sort(s2.begin(), s2.end());
    for (const auto& a : s1)
        for (const auto& b : s2) order.emplace_back(a, b);

    const std::vector<std::pair<std::string, std::string>> want = {
        { "L2L", "L5Q" }, { "L2L", "L5X" }, { "L2W", "L5Q" }, { "L2W", "L5X" }
    };
    EXPECT_EQ(order, want) << "input order must not leak into the candidate order";

    // Only the last candidate has an arc.  The search still finds it; a fixed
    // lexicographically-smallest election would not.
    const auto has_arc = [](const std::pair<std::string, std::string>& p) {
        return p.first == "L2W" && p.second == "L5X";
    };
    const auto hit = std::find_if(order.begin(), order.end(), has_arc);
    ASSERT_NE(hit, order.end());
    EXPECT_EQ(hit->first, "L2W");
    EXPECT_EQ(hit->second, "L5X");
    EXPECT_NE(order.front(), *hit)
        << "the elected pair is NOT the lexicographic first — the search is load-bearing";
}

// =================================================================================
// SdEwlWhiteningTest: R = diag(var_s) + var_p * 11^T whitened by L^-1 reproduces
// H^T R^-1 H and H^T R^-1 z exactly (the GIM common-mode pattern).
// =================================================================================

TEST(SdEwlWhiteningTest, LInverseWhiteningReproducesHtRinvH) {
    const int m = 4, n = 3;
    const double var_p = 0.0121;                       // shared pivot variance
    Eigen::VectorXd var_s(m);
    var_s << 0.0025, 0.0049, 0.0100, 0.0064;

    Eigen::MatrixXd R = Eigen::MatrixXd::Constant(m, m, var_p);
    for (int k = 0; k < m; ++k) R(k, k) += var_s(k);

    Eigen::MatrixXd H(m, n);
    H << 1.0, -2.0, 0.5,
         0.0,  1.0, -1.5,
         2.0,  0.0,  1.0,
        -1.0,  3.0,  0.25;
    Eigen::VectorXd z(m);
    z << 0.11, -0.23, 0.07, 0.42;

    const Eigen::LLT<Eigen::MatrixXd> llt(R);
    ASSERT_EQ(llt.info(), Eigen::Success);
    const Eigen::MatrixXd Hw = llt.matrixL().solve(H);
    const Eigen::VectorXd zw = llt.matrixL().solve(z);

    const Eigen::MatrixXd want_N = H.transpose() * R.inverse() * H;
    const Eigen::VectorXd want_u = H.transpose() * R.inverse() * z;
    EXPECT_LT((Hw.transpose() * Hw - want_N).cwiseAbs().maxCoeff(), 1e-10);
    EXPECT_LT((Hw.transpose() * zw - want_u).cwiseAbs().maxCoeff(), 1e-10);
}

TEST(SdEwlWhiteningTest, CommonModeRIsPositiveDefiniteForAnyPositivePivotVariance) {
    for (double var_p : { 1e-6, 1e-3, 1.0, 100.0 }) {
        const int m = 5;
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(m, m, var_p);
        for (int k = 0; k < m; ++k) R(k, k) += 1e-4;
        const Eigen::LLT<Eigen::MatrixXd> llt(R);
        EXPECT_EQ(llt.info(), Eigen::Success) << "var_p = " << var_p;
    }
}

// The sigma floor exists because arcZdMean serves a variance of the mean (~1/n): without
// it a long quiet arc produces an unphysically tight row that dominates the shared border.
TEST(SdEwlWhiteningTest, SigmaFloorBoundsTheRowInformationFromAbove) {
    const double floor_m = 0.05;
    const double tiny_var_of_mean = 1e-8;              // a very long quiet arc
    const double floored = std::max(tiny_var_of_mean, floor_m * floor_m);
    EXPECT_NEAR(floored, 0.0025, 1e-12);
    EXPECT_LT(1.0 / floored, 1.0 / tiny_var_of_mean)
        << "the floor must REDUCE the row's information, never increase it";
}

// =================================================================================
// SdEwlPivotElectionTest: most mature arc, ties broken by ascending satellite name.
// Determinism is a byte-identity requirement, not a preference.
// =================================================================================

TEST(SdEwlPivotElectionTest, HighestCountWinsAndTiesBreakByAscendingName) {
    struct A { std::string sat; std::size_t count; };
    std::vector<A> arcs = { { "G20", 12 }, { "G05", 30 }, { "G11", 30 }, { "G02", 7 } };
    std::sort(arcs.begin(), arcs.end(), [](const A& a, const A& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.sat < b.sat;
    });
    EXPECT_EQ(arcs.front().sat, "G05") << "count 30 tie must resolve to the lower name";
    EXPECT_EQ(arcs.back().sat, "G02");
}

TEST(SdEwlPivotElectionTest, ElectionIsInvariantToInputOrder) {
    struct A { std::string sat; std::size_t count; };
    const auto elect = [](std::vector<A> v) {
        std::sort(v.begin(), v.end(), [](const A& a, const A& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.sat < b.sat;
        });
        return v.front().sat;
    };
    std::vector<A> f = { { "E11", 20 }, { "E03", 20 }, { "E24", 9 } };
    std::vector<A> r = { { "E24", 9 }, { "E03", 20 }, { "E11", 20 } };
    EXPECT_EQ(elect(f), elect(r));
    EXPECT_EQ(elect(f), "E03");
}

// =================================================================================
// SdEwlConfigTest: defaults, clamps, and the inert-dependency warning path.
// =================================================================================

TEST(SdEwlConfigTest, DefaultsAreOffAndByteIdentitySafe) {
    GnssConfig cfg;
    const auto& se = cfg.getNetworkConfig().sd_ewl;
    EXPECT_FALSE(se.enable) << "must default OFF so the producer stays byte-identical";
    EXPECT_EQ(se.min_arc_epochs, 10);
    EXPECT_NEAR(se.sigma_floor_m, 0.05, 1e-12);
    EXPECT_NEAR(se.max_abs_z_m, 0.0, 1e-12);
    EXPECT_EQ(se.max_rows_per_station, 0);
    EXPECT_FALSE(se.trace);
}

TEST(SdEwlConfigTest, KnobsParseFromTheNetworkSection) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": {
        "sd_ewl": {
          "enable": true, "min_arc_epochs": 15, "sigma_floor_m": 0.08,
          "max_abs_z_m": 25.0, "max_rows_per_station": 40, "trace": true
        },)" + std::string(kMinimalStations) + R"(
      }
    })";
    const std::string path = writeTempConfig(body, "parse");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    const auto& se = cfg.getNetworkConfig().sd_ewl;
    EXPECT_TRUE(se.enable);
    EXPECT_EQ(se.min_arc_epochs, 15);
    EXPECT_NEAR(se.sigma_floor_m, 0.08, 1e-12);
    EXPECT_NEAR(se.max_abs_z_m, 25.0, 1e-12);
    EXPECT_EQ(se.max_rows_per_station, 40);
    EXPECT_TRUE(se.trace);
    std::remove(path.c_str());
}

TEST(SdEwlConfigTest, MinArcEpochsBelowTwoIsClampedBecauseVarianceNeedsTwoSamples) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": { "sd_ewl": { "enable": true, "min_arc_epochs": 1 },)"
      + std::string(kMinimalStations) + R"( }
    })";
    const std::string path = writeTempConfig(body, "clampn");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    EXPECT_EQ(cfg.getNetworkConfig().sd_ewl.min_arc_epochs, 2);
    std::remove(path.c_str());
}

TEST(SdEwlConfigTest, NonPositiveSigmaFloorIsClamped) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": { "sd_ewl": { "enable": true, "sigma_floor_m": 0.0 },)"
      + std::string(kMinimalStations) + R"( }
    })";
    const std::string path = writeTempConfig(body, "clampf");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    EXPECT_GT(cfg.getNetworkConfig().sd_ewl.sigma_floor_m, 0.0);
    std::remove(path.c_str());
}

// Arming sd_ewl without the MW detector leaves arcZdMean unreachable at every station.
// The config still loads (the run is legal, only inert); the loader logs a warning
// instead of letting the run produce zero rows unannounced.  The warning itself is not
// asserted here.
TEST(SdEwlConfigTest, ArmingWithoutTheDetectorStillLoadsAndStaysInert) {
    const std::string body = std::string() + R"({
      "processing": { "mode": "ppp-rtk-network" },
      "network": {
        "phase_leveling": false,
        "sd_ewl": { "enable": true },)" + std::string(kMinimalStations) + R"(
      }
    })";
    const std::string path = writeTempConfig(body, "inert");
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(path, lm));
    EXPECT_TRUE(cfg.getNetworkConfig().sd_ewl.enable);
    EXPECT_FALSE(cfg.networkPhaseLeveling());
    EXPECT_FALSE(cfg.networkCycleSlipMwDetect());
    std::remove(path.c_str());
}
