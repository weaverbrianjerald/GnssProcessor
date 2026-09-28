/**
 * @file test_ils_core.cpp
 * @brief Unit tests for the pure ILS core (filter/IlsCore.hpp).
 *
 * test_estimation_core.cpp covers the MLAMBDA numerical kernels
 * (ldlDecompose / reduction / search / mlambda / bootstrap), which are ils:: free
 * functions exercised there through AmbiguityResolver's forwarders.  This file
 * covers the layer above them: the acceptance battery (ratio lower/upper →
 * bootstrap FFR → per-entry frac cap) and the partial-AR backward elimination, as
 * implemented in ils::ilsFix and ils::selectFixableSubset.  These gate the
 * producer-side network AR fixes, so a regression here would pass or reject network
 * integer fixes without any other visible symptom.
 *
 * Every case is constructed analytically on diagonal covariances (Z = I, so the
 * decorrelated metric is the raw one) with floats placed at known distances
 * from their nearest integers.
 */

#include <gtest/gtest.h>

#include "filter/IlsCore.hpp"

#include <Eigen/Dense>

namespace GnssProcessor {
namespace {

using ils::IlsFailReason;
using ils::IlsGateConfig;
using ils::IlsResult;

// Default user-path gates (mirrors AmbiguityResolverConfig defaults).
IlsGateConfig defaultGates() {
    IlsGateConfig g;
    g.ratio_threshold      = 3.0;
    g.ratio_upper_bound    = 1e4;
    g.min_success_rate     = 0.999;
    g.nl_max_per_pair_frac = 0.15;
    g.enable_partial_ar    = false;
    return g;
}

Eigen::MatrixXd diag2(double a, double b) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(2, 2);
    Q(0, 0) = a;
    Q(1, 1) = b;
    return Q;
}

// ---------------------------------------------------------------------------
// Full-set accept
// ---------------------------------------------------------------------------

// Two well-converged ambiguities near integers (0, 0): the full set fixes, the
// ratio is large, bootstrap ~1, and afix matches the standalone mlambda.
TEST(IlsFixTest, FullSetAcceptsWellSeparatedProblem) {
    Eigen::VectorXd a(2);
    a << 0.04, -0.03;
    const Eigen::MatrixXd Q = diag2(0.01, 0.01);  // σ = 0.1 cyc

    const IlsResult r = ils::ilsFix(a, Q, defaultGates());

    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.partial);
    ASSERT_EQ(r.kept.size(), 2u);
    EXPECT_EQ(r.kept[0], 0);
    EXPECT_EQ(r.kept[1], 1);
    EXPECT_EQ(r.full_fail, IlsFailReason::None);
    EXPECT_NEAR(r.afix(0), 0.0, 1e-9);
    EXPECT_NEAR(r.afix(1), 0.0, 1e-9);
    EXPECT_GT(r.ratio, 3.0);
    EXPECT_LT(r.ratio, 1e4);
    EXPECT_GT(r.p_succ, 0.999);

    // ilsFix must return exactly the mlambda fix on the accept path.
    Eigen::VectorXd afix, s;
    ASSERT_TRUE(ils::mlambda(a, Q, afix, s));
    EXPECT_NEAR(r.afix(0), afix(0), 1e-12);
    EXPECT_NEAR(r.afix(1), afix(1), 1e-12);
}

// ---------------------------------------------------------------------------
// Gate rejections (partial AR off → stay float)
// ---------------------------------------------------------------------------

// Float near a half-cycle boundary → the two best candidates are nearly
// equidistant → ratio < threshold.
TEST(IlsFixTest, RejectsOnLowRatio) {
    Eigen::VectorXd a(2);
    a << 0.45, 0.0;
    const Eigen::MatrixXd Q = diag2(0.01, 0.01);

    const IlsResult r = ils::ilsFix(a, Q, defaultGates());

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, IlsFailReason::RatioLow);
    EXPECT_LT(r.full_ratio, 3.0);
}

// Passes the ratio test but the float covariance is too loose for the strict
// FFR target → bootstrap rejection.
TEST(IlsFixTest, RejectsOnBootstrapFailureUnderStrictFfr) {
    Eigen::VectorXd a(2);
    a << 0.1, 0.0;
    const Eigen::MatrixXd Q = diag2(0.05, 0.05);  // σ ≈ 0.224 cyc

    IlsGateConfig g = defaultGates();
    g.min_success_rate = 0.9999;  // network-strict target

    const IlsResult r = ils::ilsFix(a, Q, g);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, IlsFailReason::Bootstrap);
    EXPECT_GT(r.full_ratio, 3.0);          // ratio passed
    EXPECT_LT(r.full_p_succ, g.min_success_rate);
}

// One ambiguity sits well past the per-entry fractional cap → frac-cap
// rejection (with partial AR off).
TEST(IlsFixTest, RejectsOnFracCapWhenPartialDisabled) {
    Eigen::VectorXd a(2);
    a << 0.03, 0.30;  // entry 1 is 0.30 cyc from integer 0 > 0.15 cap
    const Eigen::MatrixXd Q = diag2(0.01, 0.01);

    const IlsResult r = ils::ilsFix(a, Q, defaultGates());

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, IlsFailReason::FracCap);
    EXPECT_GT(r.full_max_frac, 0.15);
    EXPECT_EQ(r.full_worst_idx, 1);
}

// MLAMBDA cannot decompose a non-PD covariance → no fix, no partial fallback.
TEST(IlsFixTest, RejectsNonPositiveDefiniteCovariance) {
    Eigen::VectorXd a(2);
    a << 0.0, 0.0;
    const Eigen::MatrixXd Q = diag2(-1.0, 0.01);  // negative diagonal

    IlsGateConfig g = defaultGates();
    g.enable_partial_ar = true;  // even with PAR on, a non-PD top-level stays float

    const IlsResult r = ils::ilsFix(a, Q, g);

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, IlsFailReason::MlambdaFailed);
}

// ---------------------------------------------------------------------------
// Partial AR
// ---------------------------------------------------------------------------

// The full set fails the frac cap on one noisy entry, but the remaining entry
// clears the battery → partial AR fixes the fixable subset.
TEST(IlsFixTest, PartialArFixesFixableSubset) {
    Eigen::VectorXd a(2);
    a << 0.03, 0.30;  // entry 1 fails the frac cap; entry 0 is clean
    const Eigen::MatrixXd Q = diag2(0.01, 0.01);

    IlsGateConfig g = defaultGates();
    g.enable_partial_ar = true;

    const IlsResult r = ils::ilsFix(a, Q, g);

    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.partial);
    ASSERT_EQ(r.kept.size(), 1u);
    EXPECT_EQ(r.kept[0], 0);            // the clean entry survived
    EXPECT_NEAR(r.afix(0), 0.0, 1e-9);
    // The full-set diagnostics still record why the full set was rejected.
    EXPECT_EQ(r.full_fail, IlsFailReason::FracCap);
}

// selectFixableSubset is also reachable directly (used by the network path).
TEST(IlsFixTest, SelectFixableSubsetReturnsCleanSubset) {
    Eigen::VectorXd a(2);
    a << 0.03, 0.30;
    const Eigen::MatrixXd Q = diag2(0.01, 0.01);

    IlsGateConfig g = defaultGates();
    g.enable_partial_ar = true;

    const ils::PartialFix pf = ils::selectFixableSubset(a, Q, g);

    ASSERT_TRUE(pf.success);
    ASSERT_EQ(pf.kept.size(), 1u);
    EXPECT_EQ(pf.kept[0], 0);
    EXPECT_NEAR(pf.afix_cycles(0), 0.0, 1e-9);
}

// Empty input is a no-op rejection, not a crash.
TEST(IlsFixTest, EmptyInputRejects) {
    const Eigen::VectorXd a;
    const Eigen::MatrixXd Q;
    const IlsResult r = ils::ilsFix(a, Q, defaultGates());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.full_fail, IlsFailReason::MlambdaFailed);
}

}  // namespace
}  // namespace GnssProcessor
