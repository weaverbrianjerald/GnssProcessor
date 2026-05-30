/**
 * @file test_estimation_core.cpp
 * @brief Unit tests for the MLAMBDA integer-ambiguity-resolution kernels.
 *
 * AmbiguityResolver implements the Modified LAMBDA method (Chang et al. 2005):
 * an LDL^T decomposition of the float covariance, an integer Z-transform that
 * decorrelates the lattice, a Schnorr-Euchner integer least-squares search,
 * and a bootstrap success-rate gate.  These are the numerical heart of PPP-RTK
 * ambiguity fixing and were previously untested — a silent regression here
 * would corrupt every fix without any obvious symptom.
 *
 * The kernels are private static members.  Rather than drive them through the
 * public resolve() entry point (which needs a full StateManager + observation
 * set), we exercise them directly through MlambdaTestAccess, a shim befriended
 * in AmbiguityResolver.hpp.  Every expected value below is analytic or
 * hand-derived:
 *
 *   - Convention pinned by hand on a 2x2: the factorization is Q = Lᵀ·D·L
 *     (NOT L·D·Lᵀ), with L unit lower-triangular.
 *   - Reduction yields a unimodular integer Z whose factors represent ZᵀQZ.
 *   - Search returns the m globally-nearest lattice points, ascending.
 *   - An exactly-integer float vector is a fixed point of the full pipeline
 *     (residual 0) for any SPD Q — a back-transform round-trip check.
 *   - Bootstrap rate is ∏ erf(1/(2√(2·dᵢ))) with documented edge cases.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "filter/KalmanFilter.hpp"

#include <Eigen/Dense>
#include <cmath>

namespace GnssProcessor {

// Befriended in AmbiguityResolver.hpp — forwards to the private static MLAMBDA
// kernels so they can be tested in isolation with synthetic matrices.
struct MlambdaTestAccess {
    static bool ldl(const Eigen::MatrixXd& Q, Eigen::MatrixXd& L, Eigen::VectorXd& d) {
        return AmbiguityResolver::ldlDecompose(Q, L, d);
    }
    static void reduce(Eigen::MatrixXd& L, Eigen::VectorXd& d, Eigen::MatrixXd& Z) {
        AmbiguityResolver::reduction(L, d, Z);
    }
    static int search(const Eigen::MatrixXd& L, const Eigen::VectorXd& d,
                      const Eigen::VectorXd& zs, Eigen::MatrixXd& zn,
                      Eigen::VectorXd& sq, int m) {
        return AmbiguityResolver::search(L, d, zs, zn, sq, m);
    }
    static bool mlambda(const Eigen::VectorXd& a, const Eigen::MatrixXd& Q,
                        Eigen::VectorXd& afix, Eigen::VectorXd& s,
                        Eigen::VectorXd* d_out) {
        return AmbiguityResolver::mlambda(a, Q, afix, s, d_out);
    }
    static double bootstrap(const Eigen::VectorXd& d) {
        return AmbiguityResolver::computeBootstrapSuccessRate(d);
    }
};

namespace {

// Reconstruct the original matrix from LDL factors using the codebase
// convention Q = Lᵀ·D·L (verified by hand; see file header).
Eigen::MatrixXd reconstruct(const Eigen::MatrixXd& L, const Eigen::VectorXd& d) {
    return L.transpose() * d.asDiagonal() * L;
}

// ---------------------------------------------------------------------------
// ldlDecompose
// ---------------------------------------------------------------------------

// LDL^T factorization reproduces the input SPD matrix and produces a
// unit lower-triangular L.  Exact factor values pinned on the 2x2.
TEST(MlambdaLdlTest, ReconstructsKnownSpdMatrices) {
    Eigen::MatrixXd Q(2, 2);
    Q << 2.0, 1.0,
         1.0, 2.0;
    Eigen::MatrixXd L;
    Eigen::VectorXd d;
    ASSERT_TRUE(MlambdaTestAccess::ldl(Q, L, d));

    // Convention is Q = Lᵀ·D·L (not L·D·Lᵀ).
    EXPECT_TRUE(reconstruct(L, d).isApprox(Q, 1e-12));

    // Exact factors from the reverse-loop LDL.
    EXPECT_NEAR(d(0), 1.5, 1e-12);
    EXPECT_NEAR(d(1), 2.0, 1e-12);
    EXPECT_NEAR(L(0, 0), 1.0, 1e-12);
    EXPECT_NEAR(L(1, 1), 1.0, 1e-12);
    EXPECT_NEAR(L(1, 0), 0.5, 1e-12);
    EXPECT_NEAR(L(0, 1), 0.0, 1e-12);  // strictly lower-triangular

    // A correlated 3x3 SPD matrix reconstructs too.
    Eigen::MatrixXd Q3(3, 3);
    Q3 << 4.0, 2.0, 1.0,
          2.0, 3.0, 0.5,
          1.0, 0.5, 2.0;
    ASSERT_TRUE(MlambdaTestAccess::ldl(Q3, L, d));
    EXPECT_TRUE(reconstruct(L, d).isApprox(Q3, 1e-12));
    EXPECT_GT(d.minCoeff(), 0.0);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(L(i, i), 1.0, 1e-12);
        for (int j = i + 1; j < 3; ++j) {
            EXPECT_NEAR(L(i, j), 0.0, 1e-12);
        }
    }
}

// Non-positive-definite inputs are rejected (return false), guarding the ILS
// search against corrupted covariance matrices.
TEST(MlambdaLdlTest, RejectsNonPositiveDefinite) {
    Eigen::MatrixXd L;
    Eigen::VectorXd d;

    Eigen::MatrixXd singular(2, 2);
    singular << 1.0, 1.0,
                1.0, 1.0;  // rank 1
    EXPECT_FALSE(MlambdaTestAccess::ldl(singular, L, d));

    Eigen::MatrixXd indefinite(2, 2);
    indefinite << 1.0, 2.0,
                  2.0, 1.0;  // eigenvalues 3, -1
    EXPECT_FALSE(MlambdaTestAccess::ldl(indefinite, L, d));

    Eigen::MatrixXd negative_diagonal(2, 2);
    negative_diagonal << 1.0,  0.0,
                         0.0, -2.0;
    EXPECT_FALSE(MlambdaTestAccess::ldl(negative_diagonal, L, d));
}

// ---------------------------------------------------------------------------
// reduction (Z-transform decorrelation)
// ---------------------------------------------------------------------------

// The reduction must (a) emit an integer, unimodular Z so the integer lattice
// is preserved, (b) leave factors that represent ZᵀQZ, and (c) reduce the
// off-diagonals to |L(i,j)| <= 0.5.
TEST(MlambdaReductionTest, PreservesLatticeAndDecorrelates) {
    Eigen::MatrixXd Q(3, 3);
    Q << 4.0, 2.0, 1.0,
         2.0, 3.0, 0.5,
         1.0, 0.5, 2.0;

    Eigen::MatrixXd L;
    Eigen::VectorXd d;
    ASSERT_TRUE(MlambdaTestAccess::ldl(Q, L, d));

    Eigen::MatrixXd Z;
    MlambdaTestAccess::reduce(L, d, Z);

    // Z is integer-valued ...
    for (int i = 0; i < Z.rows(); ++i) {
        for (int j = 0; j < Z.cols(); ++j) {
            EXPECT_NEAR(Z(i, j), std::round(Z(i, j)), 1e-9);
        }
    }
    // ... and unimodular (|det Z| = 1) so it maps integers to integers both
    // ways.
    EXPECT_NEAR(std::abs(Z.determinant()), 1.0, 1e-6);

    // The reduced factors decorrelate Q: Lᵀ·D·L == Zᵀ·Q·Z.
    const Eigen::MatrixXd reduced = reconstruct(L, d);
    const Eigen::MatrixXd transformed = Z.transpose() * Q * Z;
    EXPECT_TRUE(reduced.isApprox(transformed, 1e-9));

    // Off-diagonals reduced.
    for (int i = 0; i < L.rows(); ++i) {
        for (int j = 0; j < i; ++j) {
            EXPECT_LE(std::abs(L(i, j)), 0.5 + 1e-6);
        }
    }
}

// ---------------------------------------------------------------------------
// search (integer least squares)
// ---------------------------------------------------------------------------

// 1-D: the two nearest integers to 0.3 are 0 (d=0.09) then 1 (d=0.49).
TEST(MlambdaSearchTest, OneDimReturnsTwoNearestIntegers) {
    Eigen::MatrixXd L(1, 1);
    L << 1.0;
    Eigen::VectorXd d(1);
    d << 1.0;
    Eigen::VectorXd zs(1);
    zs << 0.3;

    Eigen::MatrixXd zn;
    Eigen::VectorXd sq;
    const int found = MlambdaTestAccess::search(L, d, zs, zn, sq, 2);

    ASSERT_EQ(found, 2);
    EXPECT_NEAR(zn(0, 0), 0.0, 1e-12);
    EXPECT_NEAR(zn(0, 1), 1.0, 1e-12);
    EXPECT_NEAR(sq(0), 0.09, 1e-12);
    EXPECT_NEAR(sq(1), 0.49, 1e-12);
}

// 2-D diagonal metric: coordinates round independently.  The two globally
// closest lattice points to (0.1, 0.4) are (0,0) [d=0.17] and (0,1) [d=0.37],
// returned in ascending distance order.
TEST(MlambdaSearchTest, DiagonalTwoDimOrdersByDistance) {
    Eigen::MatrixXd L = Eigen::MatrixXd::Identity(2, 2);
    Eigen::VectorXd d(2);
    d << 1.0, 1.0;
    Eigen::VectorXd zs(2);
    zs << 0.1, 0.4;

    Eigen::MatrixXd zn;
    Eigen::VectorXd sq;
    const int found = MlambdaTestAccess::search(L, d, zs, zn, sq, 2);

    ASSERT_EQ(found, 2);
    EXPECT_LE(sq(0), sq(1));

    // Best = componentwise rounding.
    EXPECT_NEAR(zn(0, 0), 0.0, 1e-12);
    EXPECT_NEAR(zn(1, 0), 0.0, 1e-12);
    EXPECT_NEAR(sq(0), 0.17, 1e-12);

    // Second-best flips the coordinate with the smaller penalty (the 0.4 one).
    EXPECT_NEAR(zn(0, 1), 0.0, 1e-12);
    EXPECT_NEAR(zn(1, 1), 1.0, 1e-12);
    EXPECT_NEAR(sq(1), 0.37, 1e-12);
}

// ---------------------------------------------------------------------------
// mlambda (full pipeline) + bootstrap gate
// ---------------------------------------------------------------------------

// Tight, decoupled float ambiguities fix to their nearest integers, with the
// second candidate strictly farther (a usable ratio) and a near-certain
// bootstrap success rate.
TEST(MlambdaFullTest, DiagonalProblemRoundsToNearestIntegers) {
    Eigen::VectorXd a(3);
    a << 5.05, -2.98, 10.02;
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(3, 3);
    Q.diagonal() << 0.0025, 0.0025, 0.0025;  // sigma = 0.05 cyc each

    Eigen::VectorXd afix;
    Eigen::VectorXd s;
    Eigen::VectorXd d_post;
    ASSERT_TRUE(MlambdaTestAccess::mlambda(a, Q, afix, s, &d_post));

    ASSERT_EQ(afix.size(), 3);
    EXPECT_NEAR(afix(0), 5.0, 1e-9);
    EXPECT_NEAR(afix(1), -3.0, 1e-9);
    EXPECT_NEAR(afix(2), 10.0, 1e-9);

    ASSERT_EQ(s.size(), 2);
    EXPECT_LT(s(0), s(1));  // ratio test has separation

    // d_post is the post-reduction diagonal; tight variances => P_s ~ 1.
    EXPECT_GT(MlambdaTestAccess::bootstrap(d_post), 0.99);
}

// An exactly-integer float vector is a fixed point of the full pipeline for any
// SPD Q: the fix equals the input with zero residual.  This exercises the
// decorrelation + back-transform round trip under strong correlation (where a
// naive per-coordinate round of the *decorrelated* vector still inverts
// correctly).
TEST(MlambdaFullTest, IntegerInputIsFixedPointUnderCorrelation) {
    Eigen::VectorXd a(2);
    a << 3.0, -2.0;
    Eigen::MatrixXd Q(2, 2);
    Q << 4.0, 3.8,
         3.8, 4.0;  // correlation ~0.95, SPD (det = 1.56)

    Eigen::VectorXd afix;
    Eigen::VectorXd s;
    ASSERT_TRUE(MlambdaTestAccess::mlambda(a, Q, afix, s, nullptr));

    EXPECT_NEAR(afix(0), 3.0, 1e-9);
    EXPECT_NEAR(afix(1), -2.0, 1e-9);
    EXPECT_NEAR(s(0), 0.0, 1e-9);  // exact lattice hit
    EXPECT_GT(s(1), 0.0);
}

// Bootstrap success rate = ∏ erf(1/(2·√(2·dᵢ))); for d=0.25 the argument is
// exactly 1/√2.  Plus the documented edge cases and monotonicity.
TEST(MlambdaBootstrapTest, MatchesErfProductAndEdgeCases) {
    const double p_one = std::erf(1.0 / std::sqrt(2.0));  // d = 0.25

    Eigen::VectorXd d1(1);
    d1 << 0.25;
    EXPECT_NEAR(MlambdaTestAccess::bootstrap(d1), p_one, 1e-9);

    // Independent dimensions multiply.
    Eigen::VectorXd d2(2);
    d2 << 0.25, 0.25;
    EXPECT_NEAR(MlambdaTestAccess::bootstrap(d2), p_one * p_one, 1e-9);

    // Empty -> 0.
    EXPECT_DOUBLE_EQ(MlambdaTestAccess::bootstrap(Eigen::VectorXd()), 0.0);

    // Any non-positive variance -> 0.
    Eigen::VectorXd d_neg(2);
    d_neg << 0.25, -1.0;
    EXPECT_DOUBLE_EQ(MlambdaTestAccess::bootstrap(d_neg), 0.0);
    Eigen::VectorXd d_zero(2);
    d_zero << 0.25, 0.0;
    EXPECT_DOUBLE_EQ(MlambdaTestAccess::bootstrap(d_zero), 0.0);

    // Monotonic: tighter variance -> higher success rate.
    Eigen::VectorXd d_small(1);
    d_small << 0.01;
    Eigen::VectorXd d_large(1);
    d_large << 100.0;
    EXPECT_GT(MlambdaTestAccess::bootstrap(d_small), 0.999);
    EXPECT_LT(MlambdaTestAccess::bootstrap(d_large), 0.05);
}

// ---------------------------------------------------------------------------
// KalmanFilter::josephPseudoUpdate — the pure static Joseph-form measurement
// update kernel (public; called directly).  S = H·P·Hᵀ + R, K = P·Hᵀ·S⁻¹,
// x⁺ = x + K·innov, P⁺ = (I−KH)·P·(I−KH)ᵀ + K·R·Kᵀ (symmetrized).
// ---------------------------------------------------------------------------

TEST(KalmanJosephTest, ScalarUpdateMatchesClosedForm) {
    Eigen::VectorXd x(3);
    x << 1.0, 2.0, 3.0;
    Eigen::MatrixXd P = 2.0 * Eigen::MatrixXd::Identity(3, 3);
    Eigen::MatrixXd H(1, 3);
    H << 1.0, 0.0, 0.0;  // measures state 0 only
    Eigen::VectorXd innov(1);
    innov << 0.5;
    Eigen::MatrixXd R(1, 1);
    R << 0.1;

    Eigen::VectorXd x_out;
    Eigen::MatrixXd P_out;
    KalmanFilter::josephPseudoUpdate(x, P, H, innov, R, x_out, P_out);

    // S = P00 + R = 2.1; K0 = P00/S = 2/2.1; x0⁺ = 1 + K0*0.5 = 1 + 1/2.1.
    EXPECT_NEAR(x_out(0), 1.0 + 1.0 / 2.1, 1e-12);
    EXPECT_DOUBLE_EQ(x_out(1), 2.0);  // untouched
    EXPECT_DOUBLE_EQ(x_out(2), 3.0);

    // Posterior var of state 0 = P00*R/S = 2*0.1/2.1 = 0.2/2.1.
    EXPECT_NEAR(P_out(0, 0), 0.2 / 2.1, 1e-12);
    EXPECT_NEAR(P_out(1, 1), 2.0, 1e-12);  // uncorrelated, unchanged
    EXPECT_NEAR(P_out(2, 2), 2.0, 1e-12);
    EXPECT_NEAR(P_out(0, 1), 0.0, 1e-12);
    EXPECT_TRUE(P_out.isApprox(P_out.transpose(), 1e-12));
}

TEST(KalmanJosephTest, ZeroSensitivityLeavesStateAndCovarianceUnchanged) {
    Eigen::VectorXd x(2);
    x << 5.0, -3.0;
    Eigen::MatrixXd P(2, 2);
    P << 4.0, 1.0, 1.0, 9.0;
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, 2);  // H = 0 -> K = 0
    Eigen::VectorXd innov(1);
    innov << 100.0;  // large, but ignored when H = 0
    Eigen::MatrixXd R(1, 1);
    R << 1.0;

    Eigen::VectorXd x_out;
    Eigen::MatrixXd P_out;
    KalmanFilter::josephPseudoUpdate(x, P, H, innov, R, x_out, P_out);
    EXPECT_TRUE(x_out.isApprox(x, 1e-12));
    EXPECT_TRUE(P_out.isApprox(P, 1e-12));
}

TEST(KalmanJosephTest, MeasurementReducesVarianceWithZeroInnovation) {
    Eigen::VectorXd x(1);
    x << 10.0;
    Eigen::MatrixXd P(1, 1);
    P << 4.0;
    Eigen::MatrixXd H(1, 1);
    H << 1.0;
    Eigen::VectorXd innov(1);
    innov << 0.0;  // zero innovation -> state must not move ...
    Eigen::MatrixXd R(1, 1);
    R << 1.0;

    Eigen::VectorXd x_out;
    Eigen::MatrixXd P_out;
    KalmanFilter::josephPseudoUpdate(x, P, H, innov, R, x_out, P_out);
    EXPECT_DOUBLE_EQ(x_out(0), 10.0);
    EXPECT_NEAR(P_out(0, 0), 4.0 * 1.0 / (4.0 + 1.0), 1e-12);  // ... but variance still drops to 0.8
}

TEST(KalmanJosephTest, OutputMayAliasInput) {
    Eigen::VectorXd x(1);
    x << 10.0;
    Eigen::MatrixXd P(1, 1);
    P << 4.0;
    Eigen::MatrixXd H(1, 1);
    H << 1.0;
    Eigen::VectorXd innov(1);
    innov << 2.0;
    Eigen::MatrixXd R(1, 1);
    R << 1.0;

    // Documented: outputs may alias inputs.
    KalmanFilter::josephPseudoUpdate(x, P, H, innov, R, x, P);
    EXPECT_NEAR(x(0), 10.0 + 0.8 * 2.0, 1e-12);  // K = 0.8
    EXPECT_NEAR(P(0, 0), 0.8, 1e-12);
}

TEST(KalmanJosephTest, PreservesSymmetryAndPositiveDefiniteness) {
    Eigen::VectorXd x(2);
    x << 0.0, 0.0;
    Eigen::MatrixXd P(2, 2);
    P << 4.0, 1.5, 1.5, 2.0;  // SPD, correlated
    Eigen::MatrixXd H(1, 2);
    H << 1.0, 1.0;  // measures the sum
    Eigen::VectorXd innov(1);
    innov << 0.3;
    Eigen::MatrixXd R(1, 1);
    R << 0.05;

    Eigen::VectorXd x_out;
    Eigen::MatrixXd P_out;
    KalmanFilter::josephPseudoUpdate(x, P, H, innov, R, x_out, P_out);

    EXPECT_TRUE(P_out.isApprox(P_out.transpose(), 1e-12));
    Eigen::LLT<Eigen::MatrixXd> llt(P_out);
    EXPECT_EQ(llt.info(), Eigen::Success);  // still positive-definite
    EXPECT_LE(P_out(0, 0), P(0, 0) + 1e-12);  // an update never inflates a marginal variance
    EXPECT_LE(P_out(1, 1), P(1, 1) + 1e-12);
}

}  // namespace
}  // namespace GnssProcessor
