/**
 * @file test_kf_pseudo_update_repeat.cpp
 * @brief Unit tests for the Joseph-form pseudo-observation kernel
 *        (KalmanFilter::josephPseudoUpdate) applied once and applied repeatedly with
 *        zero innovation, the way the fix-and-hold constraint meets a float state that
 *        already sits at its held integers.
 *
 * The hold writes the row "float DD = held DD" with variance R every epoch.  When
 * the float already equals the held value the innovation is zero: the state must not
 * move at all, and only the covariance contracts.  Applied k times, k independent
 * observations of the same combination h x with variance R add k h^T h / R to the
 * information, so (Sherman-Morrison)
 *
 *     P_k = P_0 - P_0 h^T h P_0 * k / (R + k s_0),   s_0 = h P_0 h^T,
 *
 * and the variance of the held combination is s_k = s_0 R / (R + k s_0), which tends
 * to R / k once k s_0 is much larger than R.  The repeated hold therefore keeps
 * tightening the held combination; these tests pin that arithmetic so a change of
 * the kernel (or a caller relying on a different law) shows.  Directions the row
 * does not see, those uncorrelated with h x under P_0, keep their variance.
 *
 * The single-application cases of tests/test_estimation_core.cpp (scalar closed
 * form, zero sensitivity, aliasing, symmetry) are not repeated here.
 *
 * Suite name @c KfPseudoUpdateRepeat is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/KalmanFilter.hpp"

#include <Eigen/Dense>

namespace {

using GnssProcessor::KalmanFilter;

/// Three ambiguity states in metres: i, ref, and a third satellite correlated with
/// both through a common term.
Eigen::VectorXd floatState() {
    Eigen::VectorXd x(3);
    x << 3.217, 1.104, -0.733;
    return x;
}

Eigen::MatrixXd priorCovariance() {
    Eigen::MatrixXd P(3, 3);
    P << 0.50, 0.12, 0.05,
         0.12, 0.30, 0.04,
         0.05, 0.04, 0.20;
    return P;
}

/// The hold row of the DD (i, ref): +1 on i, -1 on ref.
Eigen::MatrixXd ddRow() {
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, 3);
    H(0, 0) = 1.0;
    H(0, 1) = -1.0;
    return H;
}

/// The closed form above for one row h after k zero-innovation applications.
Eigen::MatrixXd closedFormAfterK(const Eigen::MatrixXd& P0, const Eigen::MatrixXd& h,
                                 double R, int k) {
    const double s0 = (h * P0 * h.transpose())(0, 0);
    const Eigen::MatrixXd Ph = P0 * h.transpose();
    return P0 - Ph * Ph.transpose() * (k / (R + k * s0));
}

/// Apply the kernel k times in place (outputs alias the inputs, as the hold does).
void applyK(Eigen::VectorXd& x, Eigen::MatrixXd& P, const Eigen::MatrixXd& H,
            const Eigen::MatrixXd& R, int k) {
    const Eigen::VectorXd v = Eigen::VectorXd::Zero(H.rows());
    for (int j = 0; j < k; ++j) KalmanFilter::josephPseudoUpdate(x, P, H, v, R, x, P);
}

} // namespace

TEST(KfPseudoUpdateRepeat, ZeroInnovationOnceKeepsTheStateAndContractsOnlyTheCovariance) {
    const Eigen::VectorXd x0 = floatState();
    const Eigen::MatrixXd P0 = priorCovariance();
    const Eigen::MatrixXd H  = ddRow();
    const double R = 1e-4;
    Eigen::MatrixXd Rm(1, 1);
    Rm << R;
    const Eigen::VectorXd v = Eigen::VectorXd::Zero(1);

    Eigen::VectorXd x1;
    Eigen::MatrixXd P1;
    KalmanFilter::josephPseudoUpdate(x0, P0, H, v, Rm, x1, P1);

    // x + K * 0 is x to the last bit.
    for (int i = 0; i < 3; ++i) EXPECT_DOUBLE_EQ(x1(i), x0(i)) << "state " << i;

    // s_0 = 0.50 + 0.30 - 2 * 0.12 = 0.56; s_1 = s_0 R / (s_0 + R).
    const double s0 = 0.56;
    const double s1 = (H * P1 * H.transpose())(0, 0);
    EXPECT_NEAR(s1, s0 * R / (s0 + R), 1e-12);
    EXPECT_TRUE(P1.isApprox(closedFormAfterK(P0, H, R, 1), 1e-10));
    EXPECT_TRUE(P1.isApprox(P1.transpose(), 0.0));
}

TEST(KfPseudoUpdateRepeat, ZeroInnovationKTimesKeepsTheStateAndFollowsTheClosedForm) {
    const Eigen::MatrixXd P0 = priorCovariance();
    const Eigen::MatrixXd H  = ddRow();
    const double R = 1e-4;
    Eigen::MatrixXd Rm(1, 1);
    Rm << R;
    const double s0 = 0.56;

    Eigen::VectorXd x = floatState();
    Eigen::MatrixXd P = P0;
    double s_prev = s0;
    for (int k = 1; k <= 50; ++k) {
        applyK(x, P, H, Rm, 1);
        SCOPED_TRACE(k);
        const double sk = (H * P * H.transpose())(0, 0);
        // s_k = s_0 R / (R + k s_0): strictly shrinking, positive.
        EXPECT_NEAR(sk / (s0 * R / (R + k * s0)), 1.0, 1e-7);
        EXPECT_LT(sk, s_prev);
        EXPECT_GT(sk, 0.0);
        s_prev = sk;
        EXPECT_TRUE(P.isApprox(closedFormAfterK(P0, H, R, k), 1e-9));
    }
    const Eigen::VectorXd x0 = floatState();
    for (int i = 0; i < 3; ++i) EXPECT_DOUBLE_EQ(x(i), x0(i)) << "state " << i;
}

TEST(KfPseudoUpdateRepeat, HeldVarianceTendsToROverKWhenThePriorIsLoose) {
    // A loose prior (k s_0 much larger than R): after k holds the held combination
    // carries R / k, not the hold variance R the row was written with.
    // Float ambiguities of 10 cm and a hold of 0.3 mm: s_0 = 0.02, R = 1e-7, and
    // R / (k s_0) = 5e-8 at k = 100, so R / k is the closed form to 1e-7.
    const Eigen::MatrixXd P0 = 0.01 * Eigen::MatrixXd::Identity(3, 3);
    const Eigen::MatrixXd H = ddRow();
    const double R = 1e-7;
    Eigen::MatrixXd Rm(1, 1);
    Rm << R;
    const double s0 = 0.02;

    Eigen::VectorXd x = floatState();
    Eigen::MatrixXd P = P0;
    const int k = 100;
    applyK(x, P, H, Rm, k);
    const double sk = (H * P * H.transpose())(0, 0);
    EXPECT_NEAR(sk / (s0 * R / (R + k * s0)), 1.0, 1e-6);
    EXPECT_NEAR(sk / (R / k), 1.0, 1e-6);
}

TEST(KfPseudoUpdateRepeat, DirectionsTheRowDoesNotSeeKeepTheirVariance) {
    // With P_0 = sigma^2 I on (i, ref) the sum x_i + x_ref is uncorrelated with the
    // difference: k holds of the difference leave the sum, and the third state,
    // exactly where they were.
    Eigen::MatrixXd P0 = Eigen::MatrixXd::Zero(3, 3);
    P0(0, 0) = 0.25;
    P0(1, 1) = 0.25;
    P0(2, 2) = 0.09;
    const Eigen::MatrixXd H = ddRow();
    Eigen::MatrixXd Rm(1, 1);
    Rm << 1e-4;

    Eigen::VectorXd x = floatState();
    Eigen::MatrixXd P = P0;
    applyK(x, P, H, Rm, 20);

    Eigen::MatrixXd sum = Eigen::MatrixXd::Zero(1, 3);
    sum(0, 0) = 1.0;
    sum(0, 1) = 1.0;
    EXPECT_NEAR((sum * P * sum.transpose())(0, 0), 0.50, 1e-12);
    EXPECT_NEAR(P(2, 2), 0.09, 1e-15);
    EXPECT_NEAR(P(0, 2), 0.0, 1e-15);
    EXPECT_NEAR(P(1, 2), 0.0, 1e-15);
    // The difference: s_0 = 0.5, s_20 = 0.5 R / (R + 20 * 0.5).
    EXPECT_NEAR((H * P * H.transpose())(0, 0) / (0.5 * 1e-4 / (1e-4 + 10.0)), 1.0, 1e-8);
}

TEST(KfPseudoUpdateRepeat, KHoldsOfTwoRowsEqualOneHoldWithRDividedByK) {
    // Two DD rows sharing the reference, R = r I: k applications add k H^T H / r to
    // the information, the same as one application with r / k.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2, 3);
    H(0, 0) = 1.0;
    H(0, 1) = -1.0;
    H(1, 2) = 1.0;
    H(1, 1) = -1.0;
    const double r = 2e-4;
    const int k = 12;
    const Eigen::MatrixXd P0 = priorCovariance();
    const Eigen::VectorXd x0 = floatState();

    Eigen::VectorXd xa = x0;
    Eigen::MatrixXd Pa = P0;
    applyK(xa, Pa, H, r * Eigen::MatrixXd::Identity(2, 2), k);

    Eigen::VectorXd xb = x0;
    Eigen::MatrixXd Pb = P0;
    applyK(xb, Pb, H, (r / k) * Eigen::MatrixXd::Identity(2, 2), 1);

    EXPECT_TRUE(Pa.isApprox(Pb, 1e-9));
    // The information form directly: P_k^-1 = P_0^-1 + k H^T H / r.
    const Eigen::MatrixXd info = P0.inverse() + (k / r) * H.transpose() * H;
    EXPECT_TRUE(Pa.isApprox(info.inverse(), 1e-9));
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(xa(i), x0(i)) << "state " << i;
        EXPECT_DOUBLE_EQ(xb(i), x0(i)) << "state " << i;
    }
}
