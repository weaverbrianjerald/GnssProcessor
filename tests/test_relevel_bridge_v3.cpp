/**
 * @file test_relevel_bridge_v3.cpp
 * @brief Bridge v3: the second equation from a GF float row (E4) and an EWL
 *        integer slot (E3) over the pend cascade (networkSlipBridgeV3Solve,
 *        networkSlipBridgeV3Replace, networkSlipFillEwlSlot).
 *
 * Kernel-level tests in the style of test_relevel_delta_bridge.cpp.  Two sign
 * conventions are pinned: (1) the Galileo EWL candidate arrives frequency-sorted
 * as (L7Q,L5Q) = ΔN3 − ΔN2, while GPS's (L2W,L5Q) = ΔN2 − ΔN3, and a missed
 * flip passes every σ cap; (2) the E4 GF row has the form λ1ΔN1 − λ2ΔN2 (the
 * documented λ1-in-numerator error family).  The E1/E4 correlation direction is
 * also pinned: under shared phase noise, independent propagation overstates
 * σ(ΔN) because the errors partially cancel through the solve, so a positive ρ
 * tightens σ, and the exact ρ of phase-consistent inputs recovers the
 * single-noise σ.
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
// GPS L1/L2/L5.
constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6, kF5 = 1176.45e6;
const double kLam1  = kC / kF1;                  // 0.1903 m
const double kLam2  = kC / kF2;                  // 0.2442 m
const double kLam5  = kC / kF5;                  // 0.2548 m
const double kLamWl = kC / (kF1 - kF2);          // 0.8619 m
const double kLamIf = kC / (kF1 + kF2);          // 0.1070 m
const double kBeta1 = kLamWl * kF1 / (kF1 + kF2);
const double kBeta2 = kLamWl * kF2 / (kF1 + kF2);
const double kLamEwlGps = kC / (kF2 - kF5);      // 5.861 m
// GAL E1/E5a/E5b (E1 ≡ L1, E5a ≡ L5).
constexpr double kF7 = 1207.14e6;
const double kLamWlGal = kC / (kF1 - kF5);       // 0.7514 m
const double kLamIfGal = kC / (kF1 + kF5);       // 0.1089 m
const double kLam7  = kC / kF7;                  // 0.2483 m
const double kLamEwlGal = kC / (kF7 - kF5);      // 9.768 m

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

TEST(RelevelBridgeV3, EwlFillAndChainGpsSign) {
    // GPS: IF pair (L1C,L2W); EWL candidate freq-sorted (L2W,L5Q) carries
    // ΔN2 − ΔN3.  Truth (dn1,dn2,dn3) = (7,4,9): cand.dn = 4 − 9 = −5.
    auto pd = gpsPend();
    pd.dif_m = difFor(7, 4);  pd.sigma_if_m = 0.0;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(7, 4);  pd.sigma_gf_m = 1e-12;
    MwBridgeCand cand{0, "G05", "L2W", "L5Q", -5, 0.05, kLamEwlGps};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, cand, 0.05));
    EXPECT_EQ(pd.dn_ewl_b3, 5);            // ΔN3 − ΔN2 = +5
    EXPECT_EQ(pd.ewl_shared_band, 2);
    EXPECT_FALSE(pd.ewl_b3_hi);            // L5Q is the EWL pair's low side
    EXPECT_EQ(pd.sig3, "L5Q");
    const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.has3);
    EXPECT_NEAR(r.dn3_cyc, 9.0, 1e-6);
    EXPECT_NEAR(r.lam3_m, kLam5, 1e-9);    // λ3 derived from (λ_shared, λ_EWL)
    EXPECT_NEAR(r.shift3_m, kLam5 * 9.0, 1e-5);
}

TEST(RelevelBridgeV3, EwlFillAndChainGalSignFlip) {
    // GAL: IF pair (L1C,L5Q); EWL candidate freq-sorted (L7Q,L5Q) carries
    // ΔN3 − ΔN2, the opposite of GPS.  Truth (dn1,dn2,dn3) = (6,2,-3):
    // cand.dn = −3 − 2 = −5 (same raw number as the GPS case, different band3).
    RelevelPend pd;
    pd.sig1 = "L1C";  pd.sig2 = "L5Q";
    pd.lambda_if = kLamIfGal;  pd.lambda_wl = kLamWlGal;
    const double b1 = kLamWlGal * kF1 / (kF1 + kF5);
    const double b2 = b1 - kLamIfGal;
    pd.dif_m = b1 * 6 - b2 * 2;  pd.sigma_if_m = 0.0;
    pd.has_gf = true;
    pd.dgf_m = kLam1 * 6 - kLam5 * 2;  pd.sigma_gf_m = 1e-12;
    MwBridgeCand cand{0, "E11", "L7Q", "L5Q", -5, 0.03, kLamEwlGal};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, cand, 0.05));
    EXPECT_EQ(pd.dn_ewl_b3, -5);           // ΔN3 − ΔN2 = −5 (kept, not flipped)
    EXPECT_EQ(pd.ewl_shared_band, 2);
    EXPECT_TRUE(pd.ewl_b3_hi);             // L7Q is the EWL pair's high side
    EXPECT_EQ(pd.sig3, "L7Q");
    const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.35);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.has3);
    EXPECT_NEAR(r.dn3_cyc, -3.0, 1e-6);
    EXPECT_NEAR(r.lam3_m, kLam7, 1e-9);
    EXPECT_NEAR(r.shift3_m, kLam7 * -3.0, 1e-5);
}

TEST(RelevelBridgeV3, EwlFillRefusalLadder) {
    auto pd = gpsPend();
    // The IF pair itself (both bands shared) must refuse.
    MwBridgeCand same{0, "G05", "L1C", "L2W", 3, 0.05, kLamWl};
    EXPECT_FALSE(networkSlipFillEwlSlot(pd, same, 0.05));
    // A disjoint pair must refuse.
    MwBridgeCand disjoint{0, "G05", "L5Q", "L6X", 3, 0.05, 1.0};
    EXPECT_FALSE(networkSlipFillEwlSlot(pd, disjoint, 0.05));
    // A recorded sig3 contradiction must refuse.
    pd.sig3 = "L5Q";
    MwBridgeCand wrong3{0, "G05", "L2W", "L6X", 3, 0.05, 1.0};
    EXPECT_FALSE(networkSlipFillEwlSlot(pd, wrong3, 0.05));
    // A valid link fills once and refuses a second fill.
    MwBridgeCand good{0, "G05", "L2W", "L5Q", -5, 0.05, kLamEwlGps};
    EXPECT_TRUE(networkSlipFillEwlSlot(pd, good, 0.05));
    EXPECT_FALSE(networkSlipFillEwlSlot(pd, good, 0.05));
    // The stored σ is max(candidate σ, floor) = max(0.05, 0.05); a floor that
    // raises the value is covered by FillEwlSigmaFloorApplies.
    EXPECT_DOUBLE_EQ(pd.sigma_ewl, 0.05);
}

TEST(RelevelBridgeV3, Band3SigmaCapDropsChainNotThePlane) {
    auto pd = gpsPend();
    pd.dif_m = difFor(2, 1);  pd.sigma_if_m = 0.005;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(2, 1);  pd.sigma_gf_m = 0.008;
    MwBridgeCand cand{0, "G05", "L2W", "L5Q", -1, 5.0 /*hopeless σ*/, kLamEwlGps};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, cand, 0.05));
    const auto r = networkSlipBridgeV3Solve(pd, 0.0, 200.0, 0.45);
    ASSERT_TRUE(r.ok);          // the (1,2) plane survives
    EXPECT_FALSE(r.has3);       // the band-3 chain is dropped, not the bridge
}

TEST(RelevelBridgeV3, ReplaceThreeBandsMutatesValueAndVarOnly) {
    auto pd = gpsPend();
    const int dn1 = 3, dn2 = 1, dn3 = 5;
    pd.pre1 = 10.0;  pd.pre_var1 = 0.01;
    pd.pre2 = -4.0;  pd.pre_var2 = 0.02;
    pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 1e-9;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(dn1, dn2);  pd.sigma_gf_m = 1e-9;
    MwBridgeCand cand{0, "G05", "L2W", "L5Q", dn2 - dn3, 0.03, kLamEwlGps};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, cand, 0.03));
    pd.has_pre3 = true;  pd.pre3 = 7.0;  pd.pre_var3 = 0.03;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    // Free re-levels sitting AT the bridged levels (perfect corroboration).
    carry[{2, "G05", "L1C"}] = LocalCarry{10.0 + kLam1 * dn1, 0.5, 100.0};
    carry[{2, "G05", "L2W"}] = LocalCarry{-4.0 + kLam2 * dn2, 0.5, 100.0};
    carry[{2, "G05", "L5Q"}] = LocalCarry{7.0 + kLam5 * dn3, 0.5, 100.0};
    BridgeV3Solve bs;
    const auto out = networkSlipBridgeV3Replace(pd, 2, "G05", carry, 0.0,
                                                200.0, 0.35, 3.0, &bs);
    ASSERT_EQ(out, BridgeReplaceOutcome::Applied);
    EXPECT_TRUE(bs.has3);
    const auto& c1 = carry[std::make_tuple(2, std::string("G05"), std::string("L1C"))];
    const auto& c2 = carry[std::make_tuple(2, std::string("G05"), std::string("L2W"))];
    const auto& c3 = carry[std::make_tuple(2, std::string("G05"), std::string("L5Q"))];
    EXPECT_NEAR(c1.value_abs, 10.0 + kLam1 * dn1, 1e-6);
    EXPECT_NEAR(c2.value_abs, -4.0 + kLam2 * dn2, 1e-6);
    EXPECT_NEAR(c3.value_abs, 7.0 + kLam5 * dn3, 1e-5);
    EXPECT_GT(c1.var, 0.01);   // pre_var + bridge var
    EXPECT_GT(c3.var, 0.03);
    EXPECT_DOUBLE_EQ(c1.last_seen_s, 100.0);  // untouched
}

TEST(RelevelBridgeV3, ReplaceNoPartialApplyOnBand3CorrobFailure) {
    auto pd = gpsPend();
    const int dn1 = 3, dn2 = 1, dn3 = 5;
    pd.pre1 = 10.0;  pd.pre_var1 = 0.01;
    pd.pre2 = -4.0;  pd.pre_var2 = 0.02;
    pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 1e-9;
    pd.has_gf = true;
    pd.dgf_m = dgfFor(dn1, dn2);  pd.sigma_gf_m = 1e-9;
    MwBridgeCand cand{0, "G05", "L2W", "L5Q", dn2 - dn3, 0.03, kLamEwlGps};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, cand, 0.03));
    pd.has_pre3 = true;  pd.pre3 = 7.0;  pd.pre_var3 = 0.03;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    carry[{2, "G05", "L1C"}] = LocalCarry{10.0 + kLam1 * dn1, 0.5, 100.0};
    carry[{2, "G05", "L2W"}] = LocalCarry{-4.0 + kLam2 * dn2, 0.5, 100.0};
    carry[{2, "G05", "L5Q"}] = LocalCarry{7.0 + kLam5 * dn3 + 10.0, 0.5, 100.0};
    const auto out = networkSlipBridgeV3Replace(pd, 2, "G05", carry, 0.0,
                                                200.0, 0.35, 3.0);
    EXPECT_EQ(out, BridgeReplaceOutcome::CorrobReject);
    // No band moved, bands 1 and 2 included.
    const auto& c1 = carry[std::make_tuple(2, std::string("G05"), std::string("L1C"))];
    const auto& c2 = carry[std::make_tuple(2, std::string("G05"), std::string("L2W"))];
    EXPECT_NEAR(c1.value_abs, 10.0 + kLam1 * dn1, 1e-12);
    EXPECT_NEAR(c2.value_abs, -4.0 + kLam2 * dn2, 1e-12);
    EXPECT_DOUBLE_EQ(c1.var, 0.5);
}

TEST(RelevelBridgeV3, FillEwlSigmaFloorApplies) {
    auto pd = gpsPend();
    MwBridgeCand good{0, "G05", "L2W", "L5Q", -5, 0.01, kLamEwlGps};
    ASSERT_TRUE(networkSlipFillEwlSlot(pd, good, 0.20));
    EXPECT_DOUBLE_EQ(pd.sigma_ewl, 0.20);   // raised to the floor (the frac proxy
                                            // understates the EWL σ at low elevation)
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
