/**
 * @file test_relevel_delta_bridge.cpp
 * @brief Tests for the float 2×2 delta-bridge kernel (networkSlipDeltaBridgeSolve)
 *        and the LocalCarry apply semantics (SPEC_slip_relevel_walk §6).
 *
 * Kernel-first, in the test_slip_bridge_kernel.cpp style: integer-injection
 * round trips pin the algebra (the β1·ΔN_WL-over-λ_IF step is this kernel's
 * analogue of the GF kernel's documented λ1-in-numerator pitfall), the variance
 * closed forms are checked against finite-difference propagation, and each
 * rejection condition is exercised in turn.  The apply micro-test checks the
 * T0-repair mutation shape on a hand-built carry map without a driver: both
 * bands shift, var inflates, and coupling and last_seen stay untouched.
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
// GPS L1/L2 reference constants.
constexpr double kF1 = 1575.42e6, kF2 = 1227.60e6;
const double kLam1   = kC / kF1;                 // 0.1903 m
const double kLam2   = kC / kF2;                 // 0.2442 m
const double kLamWl  = kC / (kF1 - kF2);         // 0.8619 m
const double kLamIf  = kC / (kF1 + kF2);         // 0.1070 m
const double kBeta1  = kLamWl * kF1 / (kF1 + kF2);
const double kBeta2  = kLamWl * kF2 / (kF1 + kF2);

double difFor(int dn1, int dn2) { return kBeta1 * dn1 - kBeta2 * dn2; }

TEST(RelevelDeltaBridge, BetaIdentityHolds) {
    // β1 − β2 ≡ λ_IF: the identity the closed-form solve rests on.
    EXPECT_NEAR(kBeta1 - kBeta2, kLamIf, 1e-12);
}

TEST(RelevelDeltaBridge, IntegerInjectionRoundTrips) {
    const int cases[][2] = {{2, 1}, {-3, -3}, {4, 5}, {-3, -4}, {50, 48}};
    for (const auto& c : cases) {
        const int dn1 = c[0], dn2 = c[1];
        const auto r = networkSlipDeltaBridgeSolve(
            dn1 - dn2, /*sigma_wl=*/0.0, difFor(dn1, dn2), /*sigma_if=*/0.0,
            kLamWl, kLamIf, /*max_abs=*/200.0, /*max_sigma=*/0.35);
        ASSERT_TRUE(r.ok) << "dn1=" << dn1 << " dn2=" << dn2;
        EXPECT_NEAR(r.dn1_cyc, dn1, 1e-9);
        EXPECT_NEAR(r.dn2_cyc, dn2, 1e-9);
        EXPECT_NEAR(r.shift1_m, kLam1 * dn1, 1e-9);
        EXPECT_NEAR(r.shift2_m, kLam2 * dn2, 1e-9);
    }
}

TEST(RelevelDeltaBridge, MwBlindEqualSlipCase) {
    // ΔN1 = ΔN2 (the MW null space): ΔN_WL = 0 with the 0.7/3-cyc clean bound.
    // The propagated σ of ΔN2 is β1·(0.7/3)/λ_IF ≈ 1.06 cyc, above the 0.50 cap,
    // so the solve rejects (SPEC §8.1 gives this as the reason the clean-complement
    // branch was removed).  With the cap loosened to 2.0 the algebra is exact.
    const int dn = -7;
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(
        0, 0.7 / 3.0, difFor(dn, dn), 0.0, kLamWl, kLamIf, 200.0, 0.50).ok);
    const auto r = networkSlipDeltaBridgeSolve(
        0, 0.7 / 3.0, difFor(dn, dn), 0.0, kLamWl, kLamIf, 200.0, 2.0);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.dn1_cyc, dn, 1e-9);
    EXPECT_NEAR(r.dn2_cyc, dn, 1e-9);
    EXPECT_NEAR(r.dn1_cyc, difFor(dn, dn) / kLamIf, 1e-9);  // ΔN = δIF/λIF here
}

TEST(RelevelDeltaBridge, VarianceMatchesFiniteDifference) {
    const double s_if = 0.02, s_wl = 0.15;
    const auto r0 = networkSlipDeltaBridgeSolve(3, s_wl, difFor(4, 1), s_if,
                                                kLamWl, kLamIf, 200.0, 10.0);
    ASSERT_TRUE(r0.ok);
    // Jacobians by finite difference on the two inputs.
    const double eps = 1e-6;
    const auto r_dif = networkSlipDeltaBridgeSolve(3, s_wl, difFor(4, 1) + eps, s_if,
                                                   kLamWl, kLamIf, 200.0, 10.0);
    const auto r_dwl = networkSlipDeltaBridgeSolve(3 + eps, s_wl, difFor(4, 1), s_if,
                                                   kLamWl, kLamIf, 200.0, 10.0);
    const double j1_dif = (r_dif.dn1_cyc - r0.dn1_cyc) / eps;
    const double j1_dwl = (r_dwl.dn1_cyc - r0.dn1_cyc) / eps;
    const double j2_dif = (r_dif.dn2_cyc - r0.dn2_cyc) / eps;
    const double j2_dwl = (r_dwl.dn2_cyc - r0.dn2_cyc) / eps;
    const double v1 = j1_dif * j1_dif * s_if * s_if + j1_dwl * j1_dwl * s_wl * s_wl;
    const double v2 = j2_dif * j2_dif * s_if * s_if + j2_dwl * j2_dwl * s_wl * s_wl;
    EXPECT_NEAR(r0.var1_m2 / (kLam1 * kLam1), v1, 1e-6);
    EXPECT_NEAR(r0.var2_m2 / (kLam2 * kLam2), v2, 1e-6);
    // Var(ΔN2) > Var(ΔN1) iff β1 > β2 (β1 multiplies ΔN_WL into ΔN2).
    EXPECT_GT(r0.var2_m2 / (kLam2 * kLam2), r0.var1_m2 / (kLam1 * kLam1));
}

TEST(RelevelDeltaBridge, RejectLadder) {
    // Non-finite inputs.
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(std::nan(""), 0.1, 0.1, 0.01,
                                             kLamWl, kLamIf, 200.0, 0.35).ok);
    // Degenerate wavelengths.
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(1, 0.1, 0.1, 0.01, 0.0, kLamIf,
                                             200.0, 0.35).ok);
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(1, 0.1, 0.1, 0.01, kLamWl, -1.0,
                                             200.0, 0.35).ok);
    // λ_IF > λ_WL implies f2 <= 0: an impossible pair, rejected.
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(1, 0.1, 0.1, 0.01, 0.1, 0.2,
                                             200.0, 0.35).ok);
    // Magnitude cap.
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(300, 0.0, difFor(301, 1), 0.0,
                                             kLamWl, kLamIf, 200.0, 0.35).ok);
    // Sigma cap: a 5 cm IF sigma is about 0.47 cyc through 1/λ_IF, above 0.35.
    EXPECT_FALSE(networkSlipDeltaBridgeSolve(2, 0.0, difFor(3, 1), 0.05,
                                             kLamWl, kLamIf, 200.0, 0.35).ok);
    // The same solve passes with the cap loosened (isolates the reject reason).
    EXPECT_TRUE(networkSlipDeltaBridgeSolve(2, 0.0, difFor(3, 1), 0.05,
                                            kLamWl, kLamIf, 200.0, 1.0).ok);
}

TEST(RelevelDeltaBridge, FdmaShapedLambdasRoundTrip) {
    // Any f1 > f2 > 0 round-trips, including GLONASS FDMA per-satellite lambdas.
    const double f1 = 1602.0e6 + 3 * 562.5e3, f2 = 1246.0e6 + 3 * 437.5e3;
    const double lam_wl = kC / (f1 - f2), lam_if = kC / (f1 + f2);
    const double b1 = lam_wl * f1 / (f1 + f2), b2 = lam_wl * f2 / (f1 + f2);
    const int dn1 = -2, dn2 = 3;
    const auto r = networkSlipDeltaBridgeSolve(dn1 - dn2, 0.0, b1 * dn1 - b2 * dn2,
                                               0.0, lam_wl, lam_if, 200.0, 0.35);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.dn1_cyc, dn1, 1e-9);
    EXPECT_NEAR(r.dn2_cyc, dn2, 1e-9);
}

TEST(RelevelDeltaBridge, LocalCarryApplyShape) {
    // The T0-repair mutation discipline on a hand-built map: value shifts,
    // var inflates, coupling and last_seen untouched.
    net::ArcTable carry;
    net::putArcIn(carry, {3, "G07", "L1C"}, &net::ArcState::amb) = LocalCarry{100.0, 0.01, 12345.0};
    net::putArcIn(carry, {3, "G07", "L2W"}, &net::ArcState::amb) = LocalCarry{200.0, 0.02, 12345.0};
    const auto r = networkSlipDeltaBridgeSolve(2, 0.1, difFor(3, 1), 0.01,
                                               kLamWl, kLamIf, 200.0, 2.0);
    ASSERT_TRUE(r.ok);
    auto& c1 = net::putArcIn(carry, {3, "G07", "L1C"}, &net::ArcState::amb);
    auto& c2 = net::putArcIn(carry, {3, "G07", "L2W"}, &net::ArcState::amb);
    c1.value_abs += r.shift1_m;  c1.var += r.var1_m2;
    c2.value_abs += r.shift2_m;  c2.var += r.var2_m2;
    EXPECT_NEAR(c1.value_abs, 100.0 + kLam1 * 3, 1e-6);
    EXPECT_NEAR(c2.value_abs, 200.0 + kLam2 * 1, 1e-6);
    EXPECT_GT(c1.var, 0.01);
    EXPECT_GT(c2.var, 0.02);
    EXPECT_DOUBLE_EQ(c1.last_seen_s, 12345.0);
    EXPECT_TRUE(c1.coupling.empty());
}

TEST(RelevelDeltaBridge, BridgeReplaceOutcomeLadder) {
    // Bridge v2 pend-and-replace on a hand-built carry map: the true slip is
    // (dN1, dN2) = (3, 1); the interim free re-level sits ~0.3 m off the true
    // post-slip level (the measured v1 free-epoch error class).
    const int dn1 = 3, dn2 = 1;
    RelevelPend pd;
    pd.pre1 = 100.0;  pd.pre_var1 = 0.01;
    pd.pre2 = 200.0;  pd.pre_var2 = 0.02;
    pd.dif_m = difFor(dn1, dn2);  pd.sigma_if_m = 0.01;
    pd.lambda_if = kLamIf;  pd.sig1 = "L1C";  pd.sig2 = "L2W";
    pd.has_dn = true;  pd.dn_wl = dn1 - dn2;  pd.sigma_wl = 0.02;
    pd.lambda_wl = kLamWl;
    const double true1 = 100.0 + kLam1 * dn1, true2 = 200.0 + kLam2 * dn2;

    const std::tuple<int, std::string, std::string> k1{5, "G09", "L1C"};
    const std::tuple<int, std::string, std::string> k2{5, "G09", "L2W"};
    net::ArcTable carry;
    net::putArcIn(carry, k1, &net::ArcState::amb) = LocalCarry{true1 + 0.3, 0.09, 999.0};
    net::putArcIn(carry, k2, &net::ArcState::amb) = LocalCarry{true2 - 0.2, 0.09, 999.0};

    // Applied: the bridged level agrees with the free level within 3 cyc.
    DeltaBridgeSolve bs;
    EXPECT_EQ(networkSlipBridgeReplace(pd, 5, "G09", carry, 200.0, 0.50, 3.0, &bs),
              BridgeReplaceOutcome::Applied);
    EXPECT_NEAR(net::putArcIn(carry, k1, &net::ArcState::amb).value_abs, true1, 1e-6);
    EXPECT_NEAR(net::putArcIn(carry, k2, &net::ArcState::amb).value_abs, true2, 1e-6);
    EXPECT_NEAR(net::putArcIn(carry, k1, &net::ArcState::amb).var, 0.01 + bs.var1_m2, 1e-12);
    EXPECT_DOUBLE_EQ(net::putArcIn(carry, k1, &net::ArcState::amb).last_seen_s, 999.0);  // untouched

    // CorrobReject: a wrong MW integer puts the bridged level cycles away from
    // the free level, and the carry stays untouched.
    RelevelPend bad = pd;  bad.dn_wl = dn1 - dn2 + 7;
    net::putArcIn(carry, k1, &net::ArcState::amb) = LocalCarry{true1 + 0.3, 0.09, 999.0};
    net::putArcIn(carry, k2, &net::ArcState::amb) = LocalCarry{true2 - 0.2, 0.09, 999.0};
    EXPECT_EQ(networkSlipBridgeReplace(bad, 5, "G09", carry, 200.0, 0.50, 3.0),
              BridgeReplaceOutcome::CorrobReject);
    EXPECT_NEAR(net::putArcIn(carry, k1, &net::ArcState::amb).value_abs, true1 + 0.3, 1e-9);

    // NoCarry: a band died between flag and replace.
    net::dropArc(carry, k2, &net::ArcState::amb);
    EXPECT_EQ(networkSlipBridgeReplace(pd, 5, "G09", carry, 200.0, 0.50, 3.0),
              BridgeReplaceOutcome::NoCarry);

    // KernelReject: sigma cap.
    RelevelPend noisy = pd;  noisy.sigma_if_m = 0.10;
    EXPECT_EQ(networkSlipBridgeReplace(noisy, 5, "G09", carry, 200.0, 0.50, 3.0),
              BridgeReplaceOutcome::KernelReject);
}

TEST(RelevelDeltaBridge, EpochStateClearDropsEverything) {
    RelevelEpochState st;
    st.free_ambs.insert({1, "G01_L1C"});
    st.mw_cands.push_back(MwBridgeCand{1, "G01", "L1C", "L2W", 2, 0.1, kLamWl});
    st.bridged.insert({1, "G01", "L1C"});
    st.clear();
    EXPECT_TRUE(st.free_ambs.empty());
    EXPECT_TRUE(st.mw_cands.empty());
    EXPECT_TRUE(st.bridged.empty());
}

}  // namespace
}  // namespace GnssProcessor
