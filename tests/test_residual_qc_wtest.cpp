// Unit tests for KalmanFilter::residualQcKernel: redundancy numbers, the Baarda
// w-test, the minimal detectable bias and the variance-factor sums of one update.
//
// The oracle is independent of the Kalman algebra.  The same update is posed as a
// batch Gauss-Markov model with the prior as stochastic constraints (Snow,
// Adjustment Computations, ch. 6): y = [z; x_bar], A = [H; I], Sigma =
// blkdiag(R, P0).  Its least-squares residual e~ and cofactor Q_e = Sigma - A N^-1 A^T
// give Baarda's statistics for observation i directly (Snow ch. 9; NGS LASER Guide
// v2.7 slides 5-97..5-100):
//
//     r_i = (Q_e Sigma^-1)_ii,  w_i = (Sigma^-1 e~)_i / sqrt((Sigma^-1 Q_e Sigma^-1)_ii),
//     MDB_i = delta_0 / sqrt((Sigma^-1 Q_e Sigma^-1)_ii).
//
//   Gate 1: a correlated R matches the batch oracle, and the pre-2026-10-07 form
//           (Sr^-1 in place of R^-1) does not.
//   Gate 2: with a diagonal R the correlation-aware and the diagonal branches agree.
//   Gate 3: a blunder of one MDB in row k shifts w_k by exactly delta_0, and w_k is
//           the largest |w| (Cauchy-Schwarz on P Q_v P).
//   Gate 4: an R that is not positive definite falls back to the diagonal branch.

#include "filter/BaardaReliability.hpp"
#include "filter/KalmanFilter.hpp"

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <cmath>
#include <random>

namespace GnssProcessor {
namespace {

constexpr double kDelta0 = 3.29 + kBaardaPowerQuantile;   // the default critical value

/// One linear Kalman update and its post-fit quantities.
struct KfUpdate {
    Eigen::VectorXd v;        ///< post-fit residual z - H x+
    Eigen::MatrixXd Sr;       ///< R - H P+ H^T
    Eigen::VectorXd sr_diag;  ///< diagonal of Sr
};

KfUpdate kalmanUpdate(const Eigen::MatrixXd& H, const Eigen::MatrixXd& R,
                      const Eigen::MatrixXd& P0, const Eigen::VectorXd& x_bar,
                      const Eigen::VectorXd& z) {
    const Eigen::Index m = P0.rows();
    const Eigen::MatrixXd S = H * P0 * H.transpose() + R;
    const Eigen::MatrixXd K = P0 * H.transpose() * S.inverse();
    const Eigen::VectorXd x_post = x_bar + K * (z - H * x_bar);
    const Eigen::MatrixXd P_post = (Eigen::MatrixXd::Identity(m, m) - K * H) * P0;
    KfUpdate u;
    u.v = z - H * x_post;
    u.Sr = R - H * P_post * H.transpose();
    u.Sr = 0.5 * (u.Sr + u.Sr.transpose());
    u.sr_diag = u.Sr.diagonal();
    return u;
}

/// Baarda's statistics for the observation rows of the stacked batch model.
struct BatchOracle {
    Eigen::VectorXd r, w, mdb;
    double wssr = 0.0, dof = 0.0;
};

BatchOracle batchOracle(const Eigen::MatrixXd& H, const Eigen::MatrixXd& R,
                        const Eigen::MatrixXd& P0, const Eigen::VectorXd& x_bar,
                        const Eigen::VectorXd& z) {
    const Eigen::Index n = H.rows(), m = H.cols();
    Eigen::MatrixXd A(n + m, m);
    A << H, Eigen::MatrixXd::Identity(m, m);
    Eigen::MatrixXd Sigma = Eigen::MatrixXd::Zero(n + m, n + m);
    Sigma.topLeftCorner(n, n) = R;
    Sigma.bottomRightCorner(m, m) = P0;
    Eigen::VectorXd y(n + m);
    y << z, x_bar;

    const Eigen::MatrixXd W = Sigma.inverse();
    const Eigen::MatrixXd N = A.transpose() * W * A;
    const Eigen::MatrixXd Ninv = N.inverse();
    const Eigen::VectorXd x_hat = Ninv * A.transpose() * W * y;
    const Eigen::VectorXd e = y - A * x_hat;
    const Eigen::MatrixXd Qe = Sigma - A * Ninv * A.transpose();
    const Eigen::MatrixXd WQW = W * Qe * W;
    const Eigen::VectorXd We = W * e;

    BatchOracle o;
    o.r.resize(n); o.w.resize(n); o.mdb.resize(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        o.r(i)   = (Qe * W)(i, i);
        o.w(i)   = We(i) / std::sqrt(WQW(i, i));
        o.mdb(i) = kDelta0 / std::sqrt(WQW(i, i));
        o.dof   += o.r(i);
    }
    // The observation and prior blocks are uncorrelated, so the observation part of
    // e~^T W e~ is v^T R^-1 v.
    const Eigen::VectorXd e_obs = e.head(n);
    o.wssr = e_obs.dot(R.inverse() * e_obs);
    return o;
}

/// A reproducible small problem: n observations of m states, correlated R.
struct Problem {
    Eigen::MatrixXd H, R, P0;
    Eigen::VectorXd x_bar, z;
};

Problem makeProblem(unsigned seed, Eigen::Index n, Eigen::Index m, bool correlated) {
    std::mt19937 gen(seed);
    std::normal_distribution<double> nrm(0.0, 1.0);
    Problem p;
    p.H = Eigen::MatrixXd(n, m);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < m; ++j) p.H(i, j) = nrm(gen);
    if (correlated) {
        // A shared term (as a common pivot leaves in single differences) plus
        // row-specific noise: strongly correlated, well conditioned.
        Eigen::MatrixXd B(n, n);
        for (Eigen::Index i = 0; i < n; ++i)
            for (Eigen::Index j = 0; j < n; ++j) B(i, j) = 0.3 * nrm(gen);
        p.R = B * B.transpose() + Eigen::MatrixXd::Constant(n, n, 0.5)
            + Eigen::MatrixXd::Identity(n, n);
    } else {
        p.R = Eigen::MatrixXd::Zero(n, n);
        for (Eigen::Index i = 0; i < n; ++i) p.R(i, i) = 0.5 + 0.25 * i;
    }
    p.P0 = Eigen::MatrixXd::Zero(m, m);
    for (Eigen::Index j = 0; j < m; ++j) p.P0(j, j) = 2.0 + j;
    p.x_bar = Eigen::VectorXd(m);
    for (Eigen::Index j = 0; j < m; ++j) p.x_bar(j) = nrm(gen);
    p.z = Eigen::VectorXd(n);
    for (Eigen::Index i = 0; i < n; ++i) p.z(i) = nrm(gen);
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1: correlated R.  The kernel's w-test, MDB, redundancy and variance-factor
// sums equal the batch oracle.  The statistic the branch used before 2026-10-07,
// (Sr^-1 v)_i / sqrt((Sr^-1)_ii), is computed alongside to show that this problem
// separates the two forms; it is a different statistic, not a rounding variant.
// ---------------------------------------------------------------------------
TEST(ResidualQcKernelTest, CorrelatedRMatchesTheBatchGaussMarkovOracle) {
    const Problem p = makeProblem(20261007u, /*n=*/6, /*m=*/3, /*correlated=*/true);
    const KfUpdate u = kalmanUpdate(p.H, p.R, p.P0, p.x_bar, p.z);
    const BatchOracle o = batchOracle(p.H, p.R, p.P0, p.x_bar, p.z);

    const auto q = KalmanFilter::residualQcKernel(u.v, p.R, u.sr_diag, &u.Sr, kDelta0);
    ASSERT_EQ(q.wtest.size(), 6);
    for (Eigen::Index i = 0; i < 6; ++i) {
        ASSERT_GT(o.r(i), 0.0);   // inside the clamp, so the comparison is exact
        ASSERT_LT(o.r(i), 1.0);
        EXPECT_NEAR(q.redundancy(i), o.r(i), 1e-9) << "row " << i;
        EXPECT_NEAR(q.wtest(i), o.w(i), 1e-9) << "row " << i;
        EXPECT_NEAR(q.mdb(i), o.mdb(i), 1e-9 * o.mdb(i)) << "row " << i;
    }
    EXPECT_NEAR(q.dof, o.dof, 1e-9);
    EXPECT_NEAR(q.wssr, o.wssr, 1e-9 * std::max(1.0, o.wssr));

    const Eigen::MatrixXd Srinv = u.Sr.inverse();
    const Eigen::VectorXd Srinv_v = Srinv * u.v;
    double max_gap = 0.0;
    for (Eigen::Index i = 0; i < 6; ++i)
        max_gap = std::max(max_gap,
                           std::abs(Srinv_v(i) / std::sqrt(Srinv(i, i)) - o.w(i)));
    EXPECT_GT(max_gap, 0.05) << "the problem does not separate the old Sr^-1 form";
}

// ---------------------------------------------------------------------------
// Gate 2: with a diagonal R the full-Sr branch and the diagonal branch are the
// same statistic (the reduction stated in docs/report/stochastic_qc.tex).
// ---------------------------------------------------------------------------
TEST(ResidualQcKernelTest, DiagonalRBranchesAgree) {
    const Problem p = makeProblem(7u, /*n=*/7, /*m=*/4, /*correlated=*/false);
    const KfUpdate u = kalmanUpdate(p.H, p.R, p.P0, p.x_bar, p.z);
    ASSERT_GT((u.Sr - Eigen::MatrixXd(u.Sr.diagonal().asDiagonal())).cwiseAbs().maxCoeff(),
              1e-3) << "Sr is never diagonal in a Kalman update; the test needs that";

    const auto full = KalmanFilter::residualQcKernel(u.v, p.R, u.sr_diag, &u.Sr, kDelta0);
    const auto diag = KalmanFilter::residualQcKernel(u.v, p.R, u.sr_diag, nullptr, kDelta0);
    for (Eigen::Index i = 0; i < 7; ++i) {
        EXPECT_NEAR(full.redundancy(i), diag.redundancy(i), 1e-12) << "row " << i;
        EXPECT_NEAR(full.wtest(i), diag.wtest(i), 1e-9) << "row " << i;
        EXPECT_NEAR(full.mdb(i), diag.mdb(i), 1e-9 * diag.mdb(i)) << "row " << i;
    }
    EXPECT_NEAR(full.dof, diag.dof, 1e-12);
    EXPECT_NEAR(full.wssr, diag.wssr, 1e-9 * std::max(1.0, diag.wssr));

    // And the diagonal branch is itself the batch oracle.
    const BatchOracle o = batchOracle(p.H, p.R, p.P0, p.x_bar, p.z);
    for (Eigen::Index i = 0; i < 7; ++i)
        EXPECT_NEAR(diag.wtest(i), o.w(i), 1e-9) << "row " << i;
}

// ---------------------------------------------------------------------------
// Gate 3: the MDB and the w-test are one convention.  With noise-free data and a
// blunder b = MDB_k in row k, w_k = b * sqrt((P Q_v P)_kk) = delta_0 exactly, and by
// Cauchy-Schwarz on P Q_v P every other |w_i| is at most w_k: the test points at the
// faulty row, which is what data snooping relies on.
// ---------------------------------------------------------------------------
TEST(ResidualQcKernelTest, BlunderOfOneMdbShiftsItsWTestByDelta0) {
    Problem p = makeProblem(99u, /*n=*/6, /*m=*/3, /*correlated=*/true);
    Eigen::VectorXd x_true(3);
    x_true << 0.4, -1.2, 2.0;
    p.x_bar = x_true;                       // an exact prior
    const Eigen::VectorXd z_clean = p.H * x_true;

    // The MDB depends on geometry and R only, not on the residuals.
    const KfUpdate u0 = kalmanUpdate(p.H, p.R, p.P0, p.x_bar, z_clean);
    const auto q0 = KalmanFilter::residualQcKernel(u0.v, p.R, u0.sr_diag, &u0.Sr, kDelta0);

    for (Eigen::Index k = 0; k < 6; ++k) {
        Eigen::VectorXd z = z_clean;
        z(k) += q0.mdb(k);
        const KfUpdate u = kalmanUpdate(p.H, p.R, p.P0, p.x_bar, z);
        const auto q = KalmanFilter::residualQcKernel(u.v, p.R, u.sr_diag, &u.Sr, kDelta0);
        EXPECT_NEAR(q.wtest(k), kDelta0, 1e-9) << "blunder row " << k;
        for (Eigen::Index i = 0; i < 6; ++i)
            EXPECT_LE(std::abs(q.wtest(i)), q.wtest(k) + 1e-9)
                << "row " << i << " outranks the blunder row " << k;
    }
}

// ---------------------------------------------------------------------------
// Gate 4: an R that is not positive definite has no weight matrix.  The kernel
// then uses the diagonal form rather than an invented P.
// ---------------------------------------------------------------------------
TEST(ResidualQcKernelTest, NonPositiveDefiniteRFallsBackToTheDiagonalForm) {
    Eigen::MatrixXd R(2, 2);
    R << 1.0, 2.0,
         2.0, 1.0;                           // eigenvalues -1 and 3
    Eigen::VectorXd v(2);
    v << 0.3, -0.2;
    Eigen::VectorXd sr_diag(2);
    sr_diag << 0.6, 0.5;
    Eigen::MatrixXd Sr(2, 2);
    Sr << 0.6, 0.1,
          0.1, 0.5;

    const auto full = KalmanFilter::residualQcKernel(v, R, sr_diag, &Sr, kDelta0);
    const auto diag = KalmanFilter::residualQcKernel(v, R, sr_diag, nullptr, kDelta0);
    for (Eigen::Index i = 0; i < 2; ++i) {
        EXPECT_DOUBLE_EQ(full.wtest(i), diag.wtest(i));
        EXPECT_DOUBLE_EQ(full.redundancy(i), diag.redundancy(i));
        EXPECT_DOUBLE_EQ(full.mdb(i), diag.mdb(i));
    }
    EXPECT_DOUBLE_EQ(full.wssr, diag.wssr);
    EXPECT_DOUBLE_EQ(full.dof, diag.dof);
}

}  // namespace GnssProcessor
