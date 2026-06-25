/**
 * @file test_slip_bridge_kernel.cpp
 * @brief Unit tests for SlipBridgeKernel::recover — the non-causal GF+MW
 *        cycle-slip integer attribution.
 *
 * These integer-injection cases are the primary guard against the two
 * documented arithmetic traps: the asymmetric PppUtils arg order
 * (getFrequency(sig,sys,sat) vs getWavelength(sys,sig,sat)) and the
 * lambda1-in-numerator term in the dN2 inversion.  Synthetic GF/MW steps are
 * built from the SAME GPS frequencies the kernel looks up, so a correct
 * recovery is an exact integer match AND a sane wide-lane wavelength.
 */
#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "slip/JumpGapFitter.hpp"
#include "slip/SlipBridgeKernel.hpp"

#include <cmath>
#include <string>

namespace GnssProcessor {
namespace {

constexpr double kC = 299792458.0;

// Build hi-freq-first GF/MW windows for an injected (dN1,dN2) on G01 L1C/L2W:
// pre side flat at 0, post side flat at the injected step.
SlipBridgeKernel::BridgeWindows makeInjected(PppUtils& ppp, int dN1, int dN2,
                                             double extra_gf_m = 0.0) {
    const System sys = System::GPS;
    const std::string sat = "G01";
    const double f1 = ppp.getFrequency("L1C", sys, sat);
    const double f2 = ppp.getFrequency("L2W", sys, sat);
    const double lam1 = ppp.getWavelength(sys, "L1C", sat);
    const double lam2 = ppp.getWavelength(sys, "L2W", sat);
    const double lam_wl = kC / std::abs(f1 - f2);

    const int dN_wl = dN1 - dN2;
    const double gf_step = lam1 * dN1 - lam2 * dN2 + extra_gf_m;
    const double mw_step = dN_wl * lam_wl;

    SlipBridgeKernel::BridgeWindows w;
    w.ok = true;
    w.t_boundary_s = 100.0;
    for (int k = 0; k < 5; ++k) {
        const double tpre = 90.0 + 2.0 * k;
        const double tpost = 100.0 + 2.0 * k;
        w.gf_pre.t_s.push_back(tpre);  w.gf_pre.y_m.push_back(0.0);
        w.mw_pre.t_s.push_back(tpre);  w.mw_pre.y_m.push_back(0.0);
        w.gf_post.t_s.push_back(tpost); w.gf_post.y_m.push_back(gf_step);
        w.mw_post.t_s.push_back(tpost); w.mw_post.y_m.push_back(mw_step);
    }
    return w;
}

TEST(SlipBridgeKernel, RecoversPositiveIntegers) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    const auto w = makeInjected(ppp, /*dN1=*/2, /*dN2=*/1);
    const auto r = kernel.recover(w, "G01", System::GPS, "L1C", "L2W", fitter);

    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.dN1, 2);
    EXPECT_EQ(r.dN2, 1);
    EXPECT_EQ(r.dN_wl, 1);
    EXPECT_EQ(r.sig1, "L1C");
    EXPECT_EQ(r.sig2, "L2W");
    EXPECT_NEAR(r.lambda_wl_m, 0.8619, 1e-3);  // GPS L1/L2 wide-lane ≈ 0.862 m
    EXPECT_LT(r.frac_resid_cyc, 1e-6);
}

TEST(SlipBridgeKernel, RecoversNegativeAndEqualSlips) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    // Equal slip on both bands (dN_wl = 0) — the MW blind spot GF must catch.
    auto w = makeInjected(ppp, /*dN1=*/-3, /*dN2=*/-3);
    auto r = kernel.recover(w, "G01", System::GPS, "L1C", "L2W", fitter);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.dN1, -3);
    EXPECT_EQ(r.dN2, -3);
    EXPECT_EQ(r.dN_wl, 0);
}

TEST(SlipBridgeKernel, OrderIndependentForReversedSignalArgs) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    const auto w = makeInjected(ppp, 2, 1);  // built hi-first
    // Pass the pair reversed (lo first) — kernel must reorder hi-first.
    const auto r = kernel.recover(w, "G01", System::GPS, "L2W", "L1C", fitter);

    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.dN1, 2);      // still the L1C (hi) integer
    EXPECT_EQ(r.sig1, "L1C");
    EXPECT_EQ(r.sig2, "L2W");
}

TEST(SlipBridgeKernel, RejectsNonIntegerStep) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    const double f1 = ppp.getFrequency("L1C", System::GPS, "G01");
    const double f2 = ppp.getFrequency("L2W", System::GPS, "G01");
    const double lam1 = ppp.getWavelength(System::GPS, "L1C", "G01");
    const double lam2 = ppp.getWavelength(System::GPS, "L2W", "G01");
    // Force dN2_real = 1.5 (frac 0.5 > 0.30) with dN_wl = 1.
    const double half_gf = 1.5 * (lam1 - lam2) + lam1 * 1.0;
    (void)f1; (void)f2;

    auto w = makeInjected(ppp, 2, 1);          // start from integer windows
    for (double& y : w.gf_post.y_m) y = half_gf;  // overwrite GF post with non-integer
    const auto r = kernel.recover(w, "G01", System::GPS, "L1C", "L2W", fitter);
    EXPECT_FALSE(r.ok);
}

TEST(SlipBridgeKernel, RejectsDegenerateSameBand) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    const auto w = makeInjected(ppp, 2, 1);
    // Same-band pair ⇒ GF degenerate ⇒ recovery must fail.
    const auto r = kernel.recover(w, "G01", System::GPS, "L1C", "L1W", fitter);
    EXPECT_FALSE(r.ok);
}

TEST(SlipBridgeKernel, RejectsWhenWindowNotReady) {
    LogManager log;
    PppUtils ppp(log);
    SlipBridgeKernel kernel(ppp);
    JumpGapFitter fitter;

    auto w = makeInjected(ppp, 2, 1);
    w.ok = false;  // caller signalled incomplete windows
    const auto r = kernel.recover(w, "G01", System::GPS, "L1C", "L2W", fitter);
    EXPECT_FALSE(r.ok);
}

}  // namespace
}  // namespace GnssProcessor
