/**
 * @file test_ar_qb_condition.cpp
 * @brief Unit tests for the conditioning probe of the user resolver's search covariance
 *        (@ref qbCondition) and the rebuild of the cycle-domain DD covariance from a full
 *        state covariance (@ref rebuildQbCycles).
 *
 * Both feed the QBCOND line that every fix and every refused partial search writes.  The
 * probe reports the eigenvalue spread of the matrix MLAMBDA searches; the rebuild is the
 * matrix the probe reads after a fix.  The cases:
 *   - the identity gives condition 1 and unit sigmas;
 *   - a diagonal matrix gives the ratio of its largest to its smallest entry and their
 *     square roots;
 *   - an input that is not symmetric is read through its symmetric part;
 *   - a rank-deficient matrix gives an unbounded or very large condition and a zero or
 *     near-zero tightest sigma;
 *   - an empty or non-finite matrix gives all zeros;
 *   - the rebuild differences the state covariance between the two satellites of each
 *     pair and divides by the two wavelengths, symmetric by construction.
 *
 * Suite name @c ArQbCondition is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"

#include <cmath>
#include <limits>
#include <vector>

using namespace GnssProcessor;

namespace {

/// A pair as rebuildQbCycles reads it: only the two state indices.
struct IdxPair {
    int state_idx_i;
    int state_idx_ref;
};

TEST(ArQbCondition, TheIdentityHasConditionOneAndUnitSigmas) {
    const QbCond c = qbCondition(Eigen::MatrixXd::Identity(3, 3));
    EXPECT_DOUBLE_EQ(c.cond, 1.0);
    EXPECT_DOUBLE_EQ(c.lo, 1.0);
    EXPECT_DOUBLE_EQ(c.hi, 1.0);
    EXPECT_DOUBLE_EQ(c.sig_lo, 1.0);
    EXPECT_DOUBLE_EQ(c.sig_hi, 1.0);
}

TEST(ArQbCondition, ADiagonalMatrixGivesTheRatioOfItsExtremeEntries) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(3, 3);
    Q(0, 0) = 4.0;
    Q(1, 1) = 0.01;
    Q(2, 2) = 0.25;
    const QbCond c = qbCondition(Q);
    EXPECT_NEAR(c.cond, 400.0, 1e-9);
    EXPECT_NEAR(c.lo, 0.01, 1e-15);
    EXPECT_NEAR(c.hi, 4.0, 1e-12);
    EXPECT_NEAR(c.sig_lo, 0.1, 1e-14);
    EXPECT_NEAR(c.sig_hi, 2.0, 1e-12);
}

TEST(ArQbCondition, ACorrelatedPairGivesTheEigenvaluesOfTheMatrix) {
    // [[2, 1], [1, 2]] has eigenvalues 1 and 3.
    Eigen::MatrixXd Q(2, 2);
    Q << 2.0, 1.0, 1.0, 2.0;
    const QbCond c = qbCondition(Q);
    EXPECT_NEAR(c.lo, 1.0, 1e-12);
    EXPECT_NEAR(c.hi, 3.0, 1e-12);
    EXPECT_NEAR(c.cond, 3.0, 1e-12);
    EXPECT_NEAR(c.sig_hi, std::sqrt(3.0), 1e-12);
}

TEST(ArQbCondition, AnAsymmetricInputIsReadThroughItsSymmetricPart) {
    // Symmetric part of [[2, 0], [2, 2]] is [[2, 1], [1, 2]].
    Eigen::MatrixXd Q(2, 2);
    Q << 2.0, 0.0, 2.0, 2.0;
    const QbCond c = qbCondition(Q);
    EXPECT_NEAR(c.lo, 1.0, 1e-12);
    EXPECT_NEAR(c.hi, 3.0, 1e-12);
}

TEST(ArQbCondition, ARankDeficientMatrixHasNoUsefulTightestDirection) {
    Eigen::MatrixXd Q(2, 2);
    Q << 1.0, 1.0, 1.0, 1.0;   // eigenvalues 0 and 2
    const QbCond c = qbCondition(Q);
    EXPECT_NEAR(c.hi, 2.0, 1e-12);
    EXPECT_LT(std::abs(c.lo), 1e-12);
    EXPECT_LT(c.sig_lo, 1e-6);
    EXPECT_TRUE(!std::isfinite(c.cond) || c.cond > 1e12);
}

TEST(ArQbCondition, ANegativeSmallestEigenvalueGivesAnUnboundedConditionAndZeroSigma) {
    Eigen::MatrixXd Q(2, 2);
    Q << 1.0, 0.0, 0.0, -0.5;
    const QbCond c = qbCondition(Q);
    EXPECT_DOUBLE_EQ(c.lo, -0.5);
    EXPECT_FALSE(std::isfinite(c.cond));
    EXPECT_DOUBLE_EQ(c.sig_lo, 0.0);
    EXPECT_DOUBLE_EQ(c.sig_hi, 1.0);
}

TEST(ArQbCondition, AnEmptyMatrixGivesZeros) {
    const QbCond c = qbCondition(Eigen::MatrixXd(0, 0));
    EXPECT_EQ(c.cond, 0.0);
    EXPECT_EQ(c.lo, 0.0);
    EXPECT_EQ(c.hi, 0.0);
    EXPECT_EQ(c.sig_lo, 0.0);
    EXPECT_EQ(c.sig_hi, 0.0);
}

TEST(ArQbCondition, ANonFiniteMatrixGivesZeros) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(2, 2);
    Q(1, 0) = std::numeric_limits<double>::quiet_NaN();
    const QbCond c = qbCondition(Q);
    EXPECT_EQ(c.cond, 0.0);
    EXPECT_EQ(c.sig_lo, 0.0);
    EXPECT_EQ(c.sig_hi, 0.0);
}

TEST(ArQbCondition, TheRebuildDifferencesTheStateCovarianceAndScalesByTheWavelengths) {
    // Four states; pair 0 = state 1 against state 0, pair 1 = state 2 against state 0,
    // pair 2 = state 3 against state 2 (a second reference).
    Eigen::MatrixXd P(4, 4);
    P << 4.0, 1.0, 0.5, 0.2,
         1.0, 3.0, 0.3, 0.1,
         0.5, 0.3, 2.0, 0.4,
         0.2, 0.1, 0.4, 5.0;
    const std::vector<IdxPair> pairs = { { 1, 0 }, { 2, 0 }, { 3, 2 } };
    const std::vector<double> lam = { 0.19, 0.24, 0.25 };
    const Eigen::MatrixXd Q = rebuildQbCycles(P, pairs, lam);
    ASSERT_EQ(Q.rows(), 3);
    ASSERT_EQ(Q.cols(), 3);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int ii = pairs[i].state_idx_i, ir = pairs[i].state_idx_ref;
            const int ji = pairs[j].state_idx_i, jr = pairs[j].state_idx_ref;
            const double want = (P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr)) / (lam[i] * lam[j]);
            EXPECT_NEAR(Q(i, j), want, 1e-12) << i << "," << j;
            EXPECT_DOUBLE_EQ(Q(i, j), Q(j, i));
        }
    }
    // Pair 0 alone: var(x1 - x0) = 3 + 4 - 2 = 5, over 0.19^2.
    EXPECT_NEAR(Q(0, 0), 5.0 / (0.19 * 0.19), 1e-9);
}

TEST(ArQbCondition, TheRebuildOfAnUncorrelatedStateWithOneReferenceCorrelatesThePairs) {
    // Unit variances, no correlation: two pairs on one reference share its variance.
    const Eigen::MatrixXd P = Eigen::MatrixXd::Identity(3, 3);
    const std::vector<IdxPair> pairs = { { 1, 0 }, { 2, 0 } };
    const std::vector<double> lam = { 1.0, 1.0 };
    const Eigen::MatrixXd Q = rebuildQbCycles(P, pairs, lam);
    EXPECT_DOUBLE_EQ(Q(0, 0), 2.0);
    EXPECT_DOUBLE_EQ(Q(1, 1), 2.0);
    EXPECT_DOUBLE_EQ(Q(0, 1), 1.0);
    const QbCond c = qbCondition(Q);
    EXPECT_NEAR(c.lo, 1.0, 1e-12);
    EXPECT_NEAR(c.hi, 3.0, 1e-12);
}

} // namespace
