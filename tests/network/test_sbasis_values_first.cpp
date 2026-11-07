/**
 * @file test_sbasis_values_first.cpp
 * @brief Tests for `network.sbasis_values_first`: the S-basis completion decides on the eigenvalues
 *        alone and computes eigenvectors only on an epoch that needs them, and the rank gate reads
 *        those eigenvalues instead of decomposing the same matrix again.
 *
 * The key exists for speed and may not change a bit:
 *
 *  1. **The eigenvalues.**  Eigen's self-adjoint solver gives the same eigenvalues, bit for bit, with
 *     and without the eigenvectors, on near-singular matrices of the reduced normals' shape.  The whole
 *     stage rests on this; the test fails if the kernel changes.
 *  2. **The completion.**  With the key on and off it returns the same count, flags, rows, targets and
 *     attribution on every outcome: nothing to complete, an over-specified epoch, extras to fold, an
 *     unseparated extra it skips, and a burst over the cap.  The eigenvectors are computed only on the
 *     outcomes that read them, so the test cannot pass by computing them every time.
 *  3. **The gate.**  With the known eigenvalues it reports the same count and largest eigenvalue and
 *     throws the same message as with its own decomposition, and it ignores a vector of another length.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "network/solve/Sbasis.hpp"

namespace GnssProcessor {
namespace {

bool bitEqual(const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
    return a.rows() == b.rows() && a.cols() == b.cols() &&
           (a.size() == 0 ||
            std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0);
}

bool bitEqualScalar(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

/// A symmetric matrix with a chosen spectrum in random directions: @p low holds its smallest
/// eigenvalues, and the rest are spread over decades up to @p top.
Eigen::MatrixXd withSpectrum(std::mt19937_64& rng, Eigen::Index n, const std::vector<double>& low, double top) {
    std::normal_distribution<double> N(0.0, 1.0);
    Eigen::MatrixXd A(n, n);
    for (Eigen::Index c = 0; c < n; ++c)
        for (Eigen::Index r = 0; r < n; ++r) A(r, c) = N(rng);
    const Eigen::HouseholderQR<Eigen::MatrixXd> qr(A);
    const Eigen::MatrixXd Q = qr.householderQ() * Eigen::MatrixXd::Identity(n, n);
    Eigen::VectorXd d(n);
    const Eigen::Index nl = static_cast<Eigen::Index>(low.size());
    for (Eigen::Index i = 0; i < nl; ++i) d(i) = low[static_cast<std::size_t>(i)];
    for (Eigen::Index i = nl; i < n; ++i)
        d(i) = top * std::pow(10.0, -3.0 * static_cast<double>(n - 1 - i) / static_cast<double>(n));
    Eigen::MatrixXd S = Q * d.asDiagonal() * Q.transpose();
    return 0.5 * (S + S.transpose()).eval();
}

std::string columnLabel(Eigen::Index c) {
    return "PB:G" + std::to_string(c % 32 + 1) + ":L1C";
}

void expectSameDirs(const std::vector<NetworkSbasis::GaugeCompletion::Dir>& a,
                    const std::vector<NetworkSbasis::GaugeCompletion::Dir>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        EXPECT_TRUE(bitEqualScalar(a[k].ev, b[k].ev));
        EXPECT_TRUE(bitEqualScalar(a[k].target, b[k].target));
        EXPECT_TRUE(bitEqualScalar(a[k].top_span, b[k].top_span));
        EXPECT_EQ(a[k].top_label, b[k].top_label);
        EXPECT_EQ(a[k].sats, b[k].sats);
        EXPECT_EQ(a[k].detail, b[k].detail);
    }
}

/// Runs the completion with the key off and on on one matrix; returns the on result.
NetworkSbasis::GaugeCompletion compareBoth(const Eigen::MatrixXd& N, int declared, const Eigen::VectorXd& x_prev,
                                           int max_extra) {
    const NetworkSbasis sb;
    const auto off = sb.completeSbasisDimension(N, declared, 1e-6, x_prev, columnLabel, max_extra);
    const auto on = sb.completeSbasisDimension(N, declared, 1e-6, x_prev, columnLabel, max_extra, true);
    EXPECT_EQ(off.observed, on.observed);
    EXPECT_EQ(off.n_extra, on.n_extra);
    EXPECT_EQ(off.n_skipped, on.n_skipped);
    EXPECT_EQ(off.capped, on.capped);
    EXPECT_EQ(off.degenerate, on.degenerate);
    EXPECT_EQ(off.weight_marginal, on.weight_marginal);
    EXPECT_TRUE(bitEqual(off.H, on.H));
    EXPECT_TRUE(bitEqual(off.z, on.z));
    expectSameDirs(off.dirs, on.dirs);
    expectSameDirs(off.skipped, on.skipped);
    EXPECT_TRUE(off.vectors_computed) << "the key off computes them on every epoch";
    return on;
}

TEST(SbasisValuesFirstTest, EigenvaluesMatchWithAndWithoutTheVectors) {
    std::mt19937_64 rng(101);
    for (const Eigen::Index n : {Eigen::Index(40), Eigen::Index(97), Eigen::Index(160)}) {
        const Eigen::MatrixXd S = withSpectrum(rng, n, {0.0, 1e-15, 3e-9, 2e-7}, 1e4);
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> vec(S, Eigen::ComputeEigenvectors);
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> val(S, Eigen::EigenvaluesOnly);
        ASSERT_EQ(vec.info(), Eigen::Success);
        ASSERT_EQ(val.info(), Eigen::Success);
        EXPECT_TRUE(bitEqual(vec.eigenvalues(), val.eigenvalues())) << "order " << n;
    }
}

TEST(SbasisValuesFirstTest, TheCompletionIsTheSameOnEveryOutcome) {
    std::mt19937_64 rng(103);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    const Eigen::Index n = 60;
    Eigen::VectorXd x_prev(n);
    for (Eigen::Index i = 0; i < n; ++i) x_prev(i) = U(rng);

    // Nothing to complete: the near-null count is the declared dimension.
    const Eigen::MatrixXd clean = withSpectrum(rng, n, {0.0, 0.0}, 1e3);
    auto on = compareBoth(clean, 2, x_prev, 6);
    EXPECT_EQ(on.observed, 2);
    EXPECT_FALSE(on.vectors_computed) << "an epoch with nothing to complete reads no eigenvector";
    EXPECT_EQ(on.eigenvalues.size(), n) << "the gate reads these";
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> clean_es(clean, Eigen::EigenvaluesOnly);
    EXPECT_TRUE(bitEqual(on.eigenvalues, clean_es.eigenvalues()));

    // Over-specified: fewer near-null directions than declared; the gate's verdict to make.
    on = compareBoth(clean, 3, x_prev, 6);
    EXPECT_EQ(on.observed, 2);
    EXPECT_FALSE(on.vectors_computed);

    // Two separated extras above the machine-zero cluster and below the cutoff: rows to fold.
    const Eigen::MatrixXd extras = withSpectrum(rng, n, {0.0, 0.0, 1e-6, 5e-6}, 1e3);
    on = compareBoth(extras, 2, x_prev, 6);
    EXPECT_EQ(on.n_extra, 2);
    EXPECT_TRUE(on.vectors_computed);
    EXPECT_EQ(on.eigenvalues.size(), 0) << "rows will be folded, so the gate's matrix is another";

    // An extra inside the machine-zero cluster: skipped with its attribution.
    const Eigen::MatrixXd orphan = withSpectrum(rng, n, {0.0, 0.0, 1e-13}, 1e3);
    on = compareBoth(orphan, 2, x_prev, 6);
    EXPECT_EQ(on.n_skipped, 1);
    EXPECT_TRUE(on.vectors_computed);
    EXPECT_EQ(on.eigenvalues.size(), 0);

    // A burst over the cap: no rows, the flag raised.
    on = compareBoth(extras, 2, x_prev, 1);
    EXPECT_TRUE(on.capped);
    EXPECT_EQ(on.n_extra, 0);
    EXPECT_EQ(on.eigenvalues.size(), 0);
}

TEST(SbasisValuesFirstTest, TheGateReadsTheKnownEigenvalues) {
    std::mt19937_64 rng(107);
    const Eigen::Index n = 50;
    const NetworkSbasis sb;
    const Eigen::MatrixXd N = withSpectrum(rng, n, {0.0, 0.0}, 1e3);
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N, Eigen::EigenvaluesOnly);
    const Eigen::VectorXd known = es.eigenvalues();

    int obs_a = -1, obs_b = -1;
    double ev_a = -1.0, ev_b = -1.0;
    sb.assertNullSpaceRank(N, 2, 1e-6, &obs_a, &ev_a);
    sb.assertNullSpaceRank(N, 2, 1e-6, &obs_b, &ev_b, &known);
    EXPECT_EQ(obs_a, 2);
    EXPECT_EQ(obs_a, obs_b);
    EXPECT_TRUE(bitEqualScalar(ev_a, ev_b));

    // The same throw, the same message, the same reports on the throwing path.
    std::string what_a, what_b;
    obs_a = obs_b = -1;
    try { sb.assertNullSpaceRank(N, 3, 1e-6, &obs_a, &ev_a); } catch (const std::runtime_error& e) { what_a = e.what(); }
    try { sb.assertNullSpaceRank(N, 3, 1e-6, &obs_b, &ev_b, &known); } catch (const std::runtime_error& e) { what_b = e.what(); }
    EXPECT_FALSE(what_a.empty());
    EXPECT_EQ(what_a, what_b);
    EXPECT_EQ(obs_a, obs_b);

    // A vector of another length is ignored, not read.
    const Eigen::VectorXd wrong = Eigen::VectorXd::Constant(n - 1, std::numeric_limits<double>::quiet_NaN());
    obs_b = -1;
    sb.assertNullSpaceRank(N, 2, 1e-6, &obs_b, &ev_b, &wrong);
    EXPECT_EQ(obs_b, 2);
    EXPECT_TRUE(bitEqualScalar(ev_a, ev_b));
}

}  // namespace
}  // namespace GnssProcessor
