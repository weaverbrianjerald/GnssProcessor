/**
 * @file test_relevel_bridge_v3.cpp
 * @brief Bridge v3: the second equation from a GF float row (E4) over the pend
 *        cascade (networkSlipBridgeV3Solve, networkSlipBridgeV3Replace).
 *
 * Kernel-level tests in the style of test_relevel_delta_bridge.cpp.  One sign
 * convention is pinned: the E4 GF row has the form λ1ΔN1 − λ2ΔN2 (the documented
 * λ1-in-numerator error family).  The E1/E4 correlation direction is also pinned:
 * under shared phase noise, independent propagation overstates σ(ΔN) because the
 * errors partially cancel through the solve, so a positive ρ tightens σ, and the
 * exact ρ of phase-consistent inputs recovers the single-noise σ.  The EWL integer
 * slot (E3) and its band-3 chain are retired with
 * network.cycle_slip.relevel_bridge_ewl; the replace is covered on the IF pair's
 * two bands.
 */
#include <gtest/gtest.h>

#include "slip/NetworkSlipQc.hpp"

#include <cmath>
#include <map>
#include <string>
#include <tuple>

namespace GnssProcessor {
namespace {

constexpr double kC = 299792458.0;
// GPS L1/L2.
constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6;
const double kLam1  = kC / kF1;                  // 0.1903 m
const double kLam2  = kC / kF2;                  // 0.2442 m
const double kLamWl = kC / (kF1 - kF2);          // 0.8619 m
const double kLamIf = kC / (kF1 + kF2);          // 0.1070 m
const double kBeta1 = kLamWl * kF1 / (kF1 + kF2);
const double kBeta2 = kLamWl * kF2 / (kF1 + kF2);

double difFor(int dn1, int dn2) { return kBeta1 * dn1 - kBeta2 * dn2; }
double dgfFor(int dn1, int dn2) { return kLam1 * dn1 - kLam2 * dn2; }

RelevelPend gpsPend() {
    RelevelPend pd;
    pd.sig1 = "L1C";  pd.sig2 = "L2W";
    pd.lambda_if = kLamIf;
    pd.lambda_wl = kLamWl;
    return pd;
}

}  // namespace

TEST(RelevelBridgeV3, GfRouteIntegerInjectionRoundTrips) {
    const int cases[][2] = {{2, 1}, {-3, -3}, {4, 5}, {-12, -9}, {50, 48}};
    for (const auto& c : cases) {
        const int dn1 = c[0], dn2 = c[1];
        auto pd = gpsPend();
        pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 0.0;
        pd.has_gf = true;
        pd.dgf_m = dgfFor(dn1, dn2);  pd.sigma_gf_m = 1e-12;
        const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35);
        ASSERT_TRUE(r.ok) << "dn1=" << dn1 << " dn2=" << dn2;
        EXPECT_TRUE(r.used_gf);
        EXPECT_NEAR(r.dn1_cyc, dn1, 1e-6);
        EXPECT_NEAR(r.dn2_cyc, dn2, 1e-6);
        EXPECT_NEAR(r.shift1_m, kLam1 * dn1, 1e-6);
        EXPECT_NEAR(r.shift2_m, kLam2 * dn2, 1e-6);
    }
}

TEST(RelevelBridgeV3, GfRouteConditioningLandsInTheDerivedBand) {
    // Reference values: at σ_if = 2 cm / σ_gf = 1.5 cm, ρ = 0,
    // σ(ΔN1) ≈ 0.161 cyc and σ(ΔN2) ≈ 0.177 cyc (det ≈ −0.0465 m²).
    auto pd = gpsPend();
    pd.dif_m = difFor(3, 2);  pd.sigma_if_m = 0.02;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(3, 2);  pd.sigma_gf_m = 0.015;
    const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 1.0);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(std::sqrt(r.var1_m2) / kLam1, 0.161, 0.01);
    EXPECT_NEAR(std::sqrt(r.var2_m2) / kLam2, 0.177, 0.01);
}

TEST(RelevelBridgeV3, CorrelationDirectionSharedPhaseNoiseLimit) {
    // Pure shared band-TD phase noise σφ: σ_if = √(2.546²+1.546²)·σφ,
    // σ_gf = √2·σφ, ρ ≈ +0.971.  At that exact ρ the solve's ΔN1 error is the
    // L1 phase noise alone, σφ/λ1 = 5.255·σφ cycles (the e2 coefficient
    // cancels); independent propagation (ρ = 0) overstates it about 3.7×.
    // Pins both the sign of the cross term and the claim in the kernel doc.
    const double sphi = 0.003;
    const double a1 = kF1 * kF1 / (kF1 * kF1 - kF2 * kF2);   // 2.546
    const double a2 = kF2 * kF2 / (kF1 * kF1 - kF2 * kF2);   // 1.546
    // Exact joint statistics of (dIF, dGF) under iid band-TD phase noise:
    // Cov = (a1 + a2)·σφ², σ_if = √(a1²+a2²)·σφ, σ_gf = √2·σφ ⇒ ρ ≈ 0.971.
    const double sig_if = std::sqrt(a1 * a1 + a2 * a2) * sphi;
    const double sig_gf = std::sqrt(2.0) * sphi;
    const double rho_exact = (a1 + a2) * sphi * sphi / (sig_if * sig_gf);
    auto pd = gpsPend();
    pd.dif_m = difFor(1, 1);
    pd.sigma_if_m = sig_if;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(1, 1);
    pd.sigma_gf_m = sig_gf;
    const auto r_rho = networkSlipBridgeV3Solve(pd, rho_exact, 200.0, 5.0);
    const auto r_rho0 = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 5.0);
    ASSERT_TRUE(r_rho.ok);
    ASSERT_TRUE(r_rho0.ok);
    const double s1_rho = std::sqrt(r_rho.var1_m2) / kLam1;
    const double s1_rho0 = std::sqrt(r_rho0.var1_m2) / kLam1;
    EXPECT_NEAR(s1_rho, 5.257 * sphi, 0.05 * 5.257 * sphi);
    EXPECT_GT(s1_rho0, 2.0 * s1_rho);    // independence overstates
}

TEST(RelevelBridgeV3, PlaneResidualScreensWrongWlInteger) {
    // Truth (5,3): WL = 2.  The kernel receives a wrong detector-rounded
    // integer (3) with the true δIF and dGF; the E2 route then solves a biased
    // plane, and the E4 over-determination rejects it (±1 WL ≈ 2.5–5σ off).
    auto pd = gpsPend();
    pd.dif_m = difFor(5, 3);  pd.sigma_if_m = 0.005;
    pd.has_dn = true;  pd.dn_wl = 3;  pd.sigma_wl = 0.05;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(5, 3);  pd.sigma_gf_m = 0.010;
    const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 2.0);
    EXPECT_FALSE(r.ok);
    EXPECT_GT(r.plane_resid_sigma, kBridgePlaneResidMax);
    // The correct integer passes with a small residual.
    pd.dn_wl = 2;
    const auto r2 = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 2.0);
    ASSERT_TRUE(r2.ok);
    EXPECT_LT(r2.plane_resid_sigma, 1.0);
    EXPECT_NEAR(r2.dn1_cyc, 5.0, 1e-6);
    EXPECT_NEAR(r2.dn2_cyc, 3.0, 1e-6);
}

TEST(RelevelBridgeV3, V3MatchesV2OnAnE2OnlyPend) {
    auto pd = gpsPend();
    pd.dif_m = difFor(4, 1);  pd.sigma_if_m = 0.02;
    pd.has_dn = true;  pd.dn_wl = 3;  pd.sigma_wl = 0.15;
    const auto r3 = networkSlipBridgeV3Solve(pd, 0.6, 200.0, 10.0);
    const auto r2 = networkSlipDeltaBridgeSolve(3, 0.15, pd.dif_m, 0.02,
                                                kLamWl, kLamIf, 200.0, 10.0);
    ASSERT_TRUE(r3.ok);
    ASSERT_TRUE(r2.ok);
    EXPECT_FALSE(r3.used_gf);
    EXPECT_NEAR(r3.dn1_cyc, r2.dn1_cyc, 1e-12);
    EXPECT_NEAR(r3.dn2_cyc, r2.dn2_cyc, 1e-12);
    EXPECT_NEAR(r3.var1_m2, r2.var1_m2, 1e-12);
    EXPECT_NEAR(r3.var2_m2, r2.var2_m2, 1e-12);
}

TEST(RelevelBridgeV3, ReplaceTwoBandsMutatesValueAndVarOnly) {
    auto pd = gpsPend();
    const int dn1 = 3, dn2 = 1;
    pd.pre1 = 10.0;  pd.pre_var1 = 0.01;
    pd.pre2 = -4.0;  pd.pre_var2 = 0.02;
    // Millimetre-class inputs, so the propagated variance is visible on both bands.
    pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 0.002;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(dn1, dn2);  pd.sigma_gf_m = 0.002;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    // Free re-levels sitting AT the bridged levels (perfect corroboration).
    carry[{2, "G05", "L1C"}] = LocalCarry{10.0 + kLam1 * dn1, 0.5, 100.0};
    carry[{2, "G05", "L2W"}] = LocalCarry{-4.0 + kLam2 * dn2, 0.5, 100.0};
    BridgeV3Solve bs;
    const auto out = networkSlipBridgeV3Replace(pd, 2, "G05", carry, 0.0,
                                                200.0, 0.35, 3.0, &bs);
    ASSERT_EQ(out, BridgeReplaceOutcome::Applied);
    EXPECT_TRUE(bs.used_gf);
    const auto& c1 = carry[std::make_tuple(2, std::string("G05"), std::string("L1C"))];
    const auto& c2 = carry[std::make_tuple(2, std::string("G05"), std::string("L2W"))];
    EXPECT_NEAR(c1.value_abs, 10.0 + kLam1 * dn1, 1e-6);
    EXPECT_NEAR(c2.value_abs, -4.0 + kLam2 * dn2, 1e-6);
    EXPECT_GT(c1.var, 0.01);   // pre_var + bridge var
    EXPECT_GT(c2.var, 0.02);
    EXPECT_DOUBLE_EQ(c1.last_seen_s, 100.0);  // untouched
}

TEST(RelevelBridgeV3, ReplaceNoPartialApplyOnBandCorrobFailure) {
    auto pd = gpsPend();
    const int dn1 = 3, dn2 = 1;
    pd.pre1 = 10.0;  pd.pre_var1 = 0.01;
    pd.pre2 = -4.0;  pd.pre_var2 = 0.02;
    pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 1e-9;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(dn1, dn2);  pd.sigma_gf_m = 1e-9;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    carry[{2, "G05", "L1C"}] = LocalCarry{10.0 + kLam1 * dn1, 0.5, 100.0};
    carry[{2, "G05", "L2W"}] = LocalCarry{-4.0 + kLam2 * dn2 + 10.0, 0.5, 100.0};
    const auto out = networkSlipBridgeV3Replace(pd, 2, "G05", carry, 0.0,
                                                200.0, 0.35, 3.0);
    EXPECT_EQ(out, BridgeReplaceOutcome::CorrobReject);
    // No band moved, the corroborated band 1 included.
    const auto& c1 = carry[std::make_tuple(2, std::string("G05"), std::string("L1C"))];
    const auto& c2 = carry[std::make_tuple(2, std::string("G05"), std::string("L2W"))];
    EXPECT_NEAR(c1.value_abs, 10.0 + kLam1 * dn1, 1e-12);
    EXPECT_NEAR(c2.value_abs, -4.0 + kLam2 * dn2 + 10.0, 1e-12);
    EXPECT_DOUBLE_EQ(c1.var, 0.5);
}

TEST(RelevelBridgeV3, GfRouteRejectLadder) {
    // No second equation at all → KernelReject (keep pending).
    auto pd = gpsPend();
    pd.dif_m = difFor(2, 1);  pd.sigma_if_m = 0.01;
    EXPECT_FALSE(networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35).ok);
    // GF armed but degenerate σ → reject.
    pd.has_gf = true;  pd.dgf_m = dgfFor(2, 1);  pd.sigma_gf_m = 0.0;
    EXPECT_FALSE(networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35).ok);
    // Loose σ over the cap → reject.
    pd.sigma_gf_m = 0.5;
    EXPECT_FALSE(networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35).ok);
    // Magnitude cap.
    pd.sigma_gf_m = 1e-9;  pd.sigma_if_m = 1e-9;
    pd.dif_m = difFor(300, 1);  pd.dgf_m = dgfFor(300, 1);
    EXPECT_FALSE(networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35).ok);
}

}  // namespace GnssProcessor
