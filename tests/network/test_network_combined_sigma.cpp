// Unit tests for the combined-correction σ arithmetic
// (include/products/CombinedCorrectionSigma.hpp): the covariance→{sigc,sigcc,rho}
// math behind the producer's sigc=/sigcc=/rho= wire tokens (M4, product contract v2).
// Hand-built covariances with known cross-terms check the formulas exactly, and
// check that the combined σ is far below the RSS of the individual σ for a
// reconverging satellite.

#include "products/CombinedCorrectionSigma.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>
#include <cmath>

using namespace GnssProcessor;

namespace {

// A 3×3 shared-covariance stub: column 0 = clock, 1 = phase bias, 2 = code bias.
Eigen::MatrixXd cov3(double a, double b, double c, double d, double e, double f) {
    Eigen::MatrixXd m(3, 3);
    m << a, d, e,
         d, b, f,
         e, f, c;
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exact arithmetic: sigc/sigcc/rho reproduce the hand-computed values.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, ReproducesHandComputedValues) {
    // a=4 b=9 c=16 (clk/pb/cb var); d=-5 e=-7 f=10 (clk-pb, clk-cb, pb-cb cov); s0=2.
    const Eigen::MatrixXd cov = cov3(4, 9, 16, -5, -7, 10);
    const double s0 = 2.0;
    const auto r = computeCombinedCorrectionSigma(cov, /*clk=*/0, /*pb=*/1, /*cb=*/2, s0);

    // Var_phase = 4+9+2(-5) = 3  ⇒ sigc  = 2·√3.
    // Var_code  = 4+16+2(-7)= 6  ⇒ sigcc = 2·√6.
    // Cov_cross = a+d+e+f = 4-5-7+10 = 2 ⇒ rho = 2/√(6·3) = 2/√18.
    EXPECT_NEAR(r.sigc,  2.0 * std::sqrt(3.0), 1e-12);
    EXPECT_NEAR(r.sigcc, 2.0 * std::sqrt(6.0), 1e-12);
    EXPECT_NEAR(r.rho,   2.0 / std::sqrt(18.0), 1e-12);
}

// ---------------------------------------------------------------------------
// For a fresh (reconverging) satellite whose clock and phase-bias estimates are
// strongly anti-correlated, the combined σ is far below the RSS of the individual
// σ.  This is the information the combined-σ channel exists to carry.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, FreshSatelliteCombinedFarBelowRss) {
    // Individual σ are large (√100 = 10 at s0=1) but the errors nearly cancel in
    // the sum (cov = -95): Var(clk+pb) = 100+100-190 = 10 ⇒ sigc = √10 ≈ 3.16.
    const Eigen::MatrixXd cov = cov3(100, 100, 100, -95, -95, 90);
    const double s0 = 1.0;
    const auto r = computeCombinedCorrectionSigma(cov, 0, 1, 2, s0);

    const double sig_clk = s0 * std::sqrt(cov(0, 0));
    const double sig_pb  = s0 * std::sqrt(cov(1, 1));
    const double rss_phase = std::sqrt(sig_clk * sig_clk + sig_pb * sig_pb);  // = √200 ≈ 14.14

    EXPECT_NEAR(r.sigc, std::sqrt(10.0), 1e-9);
    EXPECT_LT(r.sigc, 0.3 * rss_phase);          // combined ≪ individual RSS
    EXPECT_GT(r.sigc, 0.0);
}

// ---------------------------------------------------------------------------
// σ0 multiplies sigc/sigcc and cancels in rho.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, Sigma0ScalesSigmaButNotRho) {
    const Eigen::MatrixXd cov = cov3(4, 9, 16, -5, -7, 10);
    const auto r1 = computeCombinedCorrectionSigma(cov, 0, 1, 2, 1.0);
    const auto r5 = computeCombinedCorrectionSigma(cov, 0, 1, 2, 5.0);

    EXPECT_NEAR(r5.sigc,  5.0 * r1.sigc,  1e-12);
    EXPECT_NEAR(r5.sigcc, 5.0 * r1.sigcc, 1e-12);
    EXPECT_NEAR(r5.rho,   r1.rho,         1e-12);   // dimensionless, so σ0-invariant
}

// ---------------------------------------------------------------------------
// Missing / invalid columns degrade gracefully to the "absent" (0) wire value.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, NoCodeColumnGivesPhaseOnly) {
    const Eigen::MatrixXd cov = cov3(4, 9, 16, -5, -7, 10);
    const auto r = computeCombinedCorrectionSigma(cov, 0, 1, /*cb=*/-1, 2.0);
    EXPECT_NEAR(r.sigc, 2.0 * std::sqrt(3.0), 1e-12);
    EXPECT_DOUBLE_EQ(r.sigcc, 0.0);
    EXPECT_DOUBLE_EQ(r.rho, 0.0);   // rho needs both a code and a phase column
}

TEST(CombinedCorrectionSigma, NoPhaseColumnGivesCodeOnly) {
    const Eigen::MatrixXd cov = cov3(4, 9, 16, -5, -7, 10);
    const auto r = computeCombinedCorrectionSigma(cov, 0, /*pb=*/-1, 2, 2.0);
    EXPECT_DOUBLE_EQ(r.sigc, 0.0);
    EXPECT_NEAR(r.sigcc, 2.0 * std::sqrt(6.0), 1e-12);
    EXPECT_DOUBLE_EQ(r.rho, 0.0);
}

TEST(CombinedCorrectionSigma, InvalidClockColumnGivesAllZero) {
    const Eigen::MatrixXd cov = cov3(4, 9, 16, -5, -7, 10);
    const auto out_of_range = computeCombinedCorrectionSigma(cov, /*clk=*/9, 1, 2, 2.0);
    EXPECT_DOUBLE_EQ(out_of_range.sigc, 0.0);
    EXPECT_DOUBLE_EQ(out_of_range.sigcc, 0.0);
    EXPECT_DOUBLE_EQ(out_of_range.rho, 0.0);

    const auto negative = computeCombinedCorrectionSigma(cov, /*clk=*/-1, 1, 2, 2.0);
    EXPECT_DOUBLE_EQ(negative.sigc, 0.0);
    EXPECT_DOUBLE_EQ(negative.sigcc, 0.0);
}

// ---------------------------------------------------------------------------
// A non-positive combined variance (numerically degenerate) emits no σ.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, NonPositiveCombinedVarianceIsZero) {
    // Var(clk+pb) = 1 + 1 + 2(-1) = 0 ⇒ no phase σ is emitted.
    const Eigen::MatrixXd cov = cov3(1, 1, 4, -1, 0, 0);
    const auto r = computeCombinedCorrectionSigma(cov, 0, 1, 2, 1.0);
    EXPECT_DOUBLE_EQ(r.sigc, 0.0);
    EXPECT_GT(r.sigcc, 0.0);          // Var(clk+cb) = 1+4+0 = 5 > 0
    EXPECT_DOUBLE_EQ(r.rho, 0.0);     // no phase term ⇒ no correlation
}

// ---------------------------------------------------------------------------
// rho is clamped into [-1, 1] against numerical overshoot.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, RhoClamped) {
    // a=b=c=1, d=e=1, f=2 ⇒ cross = 1+1+1+2 = 5, denom = √(4·4) = 4 ⇒ raw rho 1.25.
    const Eigen::MatrixXd cov = cov3(1, 1, 1, 1, 1, 2);
    const auto r = computeCombinedCorrectionSigma(cov, 0, 1, 2, 1.0);
    EXPECT_DOUBLE_EQ(r.rho, 1.0);
}

// ---------------------------------------------------------------------------
// selectCombinedCodeRef: the code↔phase reference pairing.  Code and phase biases
// live in disjoint key namespaces ("C1W" vs "L1W"), so the pairing translates the
// RINEX obs-type letter (C→L).  A raw key equality never matches and leaves rho at 0.
// ---------------------------------------------------------------------------
TEST(CombinedCorrectionSigma, RefPairsCodeToPhaseSibling) {
    // C1W's phase sibling L1W is present ⇒ that pair is chosen.
    const auto ref = selectCombinedCodeRef({"C1W", "C2W"}, {"L1W", "L2W"});
    EXPECT_EQ(ref.code, "C1W");
    EXPECT_EQ(ref.phase, "L1W");   // not "C1W": the obs-type letter is swapped
}

TEST(CombinedCorrectionSigma, RefPrefersFirstCodeWithPhasePair) {
    // C1W has no phase sibling; C5Q does (L5Q) ⇒ skip C1W, pick the paired C5Q.
    const auto ref = selectCombinedCodeRef({"C1W", "C5Q"}, {"L5Q"});
    EXPECT_EQ(ref.code, "C5Q");
    EXPECT_EQ(ref.phase, "L5Q");
}

TEST(CombinedCorrectionSigma, RefFallsBackToFirstCodeWhenNoPhasePair) {
    // No phase sibling for any code ⇒ ref.code = first code, ref.phase empty (rho absent).
    const auto ref = selectCombinedCodeRef({"C1W", "C2W"}, {"L5Q"});
    EXPECT_EQ(ref.code, "C1W");
    EXPECT_TRUE(ref.phase.empty());
}

TEST(CombinedCorrectionSigma, RefEmptyWhenNoCodeSignals) {
    const auto ref = selectCombinedCodeRef({}, {"L1C"});
    EXPECT_TRUE(ref.code.empty());
    EXPECT_TRUE(ref.phase.empty());
}
