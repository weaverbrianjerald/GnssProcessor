/**
 * @file test_covariance_rereference.cpp
 * @brief Unit tests for the shared covariance re-reference primitives
 *        (CovReReference::applyBssd / enforcePsd).
 *
 * These characterize the primitives extracted in W6a:
 *   - applyBssd reproduces a hand-computed subtract-and-freeze T·P·Tᵀ
 *     (PivotManager::applyCovarianceReReference delegates to it, so the two
 *     are the same code path; this file does not call PivotManager);
 *   - enforcePsd leaves a PSD matrix's values intact (only symmetrizing) and
 *     repairs an indefinite matrix to PSD via the epsilon ridge, returning a
 *     non-positive min-LDLT-diagonal as the product-quality flag.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>
#include <Eigen/Eigenvalues>
#include <vector>

#include "filter/CovarianceReReference.hpp"
#include "logging/LogManager.hpp"

namespace GnssProcessor {
namespace {

double minEigenvalue(const Eigen::MatrixXd& P) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(P);
    return es.eigenvalues().minCoeff();
}

// applyBssd must reproduce the documented subtract-and-freeze transform.
// Hand-computed oracle: group = {0,1,2}, pivot = 0.
//   P = [[4,1,0],[1,5,2],[0,2,6]]
//   rows 1,2 -= row 0  ->  [[4,1,0],[-3,4,2],[-4,1,6]]
//   cols 1,2 -= col 0  ->  [[4,-3,-4],[-3,7,5],[-4,5,10]]
//   zero pivot row/col ->  [[0,0,0],[0,7,5],[0,5,10]]
TEST(CovReReferenceTest, ApplyBssdMatchesHandComputedOracle) {
    Eigen::MatrixXd P(3, 3);
    P << 4, 1, 0,
         1, 5, 2,
         0, 2, 6;

    Eigen::MatrixXd expected(3, 3);
    expected << 0, 0, 0,
                0, 7, 5,
                0, 5, 10;

    Eigen::MatrixXd got = P;
    CovReReference::applyBssd({0, 1, 2}, /*pivot=*/0, got);

    EXPECT_TRUE(got.isApprox(expected, 1e-12)) << "got=\n" << got;
    EXPECT_TRUE(got.row(0).isZero(0.0));
    EXPECT_TRUE(got.col(0).isZero(0.0));
}

// enforcePsd on a (slightly asymmetric) PSD matrix: symmetrizes, returns a
// positive min diagonal, and leaves the symmetric part's values intact (no ridge).
TEST(CovReReferenceTest, EnforcePsdSymmetrizesAndLeavesPsdUntouched) {
    LogManager lm;
    Eigen::MatrixXd P(2, 2);
    P << 4.0, 1.0,
         1.0 + 1e-9, 4.0;   // tiny FP asymmetry

    const Eigen::MatrixXd before_sym = 0.5 * (P + P.transpose());
    const double min_d = CovReReference::enforcePsd(P, lm, "unit-test");

    EXPECT_GT(min_d, 0.0);                       // PSD -> no ridge fired
    EXPECT_TRUE(P.isApprox(P.transpose(), 0.0)); // symmetric afterwards
    EXPECT_TRUE(P.isApprox(before_sym, 1e-12));  // values are the symmetrized input
}

// enforcePsd on an indefinite matrix flags it (min_d <= 0) and ridges it to PSD.
//   P = [[1,2],[2,1]] has eigenvalues {3, -1}; LDLT vectorD = {1, -3}.
TEST(CovReReferenceTest, EnforcePsdRepairsIndefiniteMatrix) {
    LogManager lm;
    Eigen::MatrixXd P(2, 2);
    P << 1.0, 2.0,
         2.0, 1.0;

    ASSERT_LT(minEigenvalue(P), 0.0);  // precondition: indefinite

    const double min_d = CovReReference::enforcePsd(P, lm, "unit-test");

    EXPECT_LE(min_d, 0.0);                        // flagged as non-PSD
    EXPECT_GE(minEigenvalue(P), -1e-9);           // ridged to (numerically) PSD
    EXPECT_TRUE(P.isApprox(P.transpose(), 0.0));  // still symmetric
}

} // namespace
} // namespace GnssProcessor
