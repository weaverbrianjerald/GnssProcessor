/**
 * @file test_network_conditional_variance_frame.cpp
 * @brief The border-known conditional variance and the per-solve datum-frame
 *        covariance keep every bit of the arithmetic they replace.
 *
 * Two exact shortcuts in the network estimator's covariance consumers:
 *
 *  - @ref NetworkNormalEquations::stationConditionalVarianceDiag returns
 *    diag(N_rr⁻¹) without the product with an all-zero shared covariance that
 *    @ref NetworkNormalEquations::reconstructStationCovarianceDiag pays.  The old
 *    form, called with a zero matrix, is the oracle, compared bit for bit
 *    (including the zero and NaN cases, where the shortcut takes the old product).
 *  - @ref NetworkEstimator::solve builds the datum-frame shared covariance once;
 *    every consumer that passes the solve's datum reads it.  The oracle is the
 *    frame built by hand from @ref NetworkEstimator::sharedCovariance and fed to
 *    the kernel through @ref NetworkEstimator::normals, again bit for bit, and the
 *    stored frame must not be read after @ref NetworkEstimator::setPhaseSumZeroGauges
 *    or for a datum with other pinned columns.
 */
#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "network/solve/Estimator.hpp"
#include "network/solve/NormalEquations.hpp"

namespace GnssProcessor {
namespace {

/// True when the two arrays hold the same bytes (so +0 and -0, and NaN payloads, differ).
bool sameBits(const Eigen::VectorXd& a, const Eigen::VectorXd& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0;
}
bool sameBits(const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
    return a.rows() == b.rows() && a.cols() == b.cols() &&
           std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0;
}

/// A random symmetric positive-definite n x n matrix.
Eigen::MatrixXd randomSpd(Eigen::Index n, std::mt19937& g) {
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd B(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j) B(i, j) = nd(g);
    Eigen::MatrixXd S = B * B.transpose() + static_cast<double>(n) * Eigen::MatrixXd::Identity(n, n);
    return 0.5 * (S + S.transpose());
}

/// Add one station from pre-formed blocks with a random SPD local block.
std::size_t addRandomStation(NetworkNormalEquations& neq, Eigen::Index n_s, Eigen::Index n_r,
                             const Eigen::MatrixXd& N_sr, std::mt19937& g) {
    const Eigen::MatrixXd N_rr = randomSpd(n_r, g);
    const Eigen::MatrixXd N_ss = Eigen::MatrixXd::Zero(n_s, n_s);
    return neq.addStation(N_ss, N_sr, N_rr, Eigen::VectorXd::Zero(n_s), Eigen::VectorXd::Zero(n_r));
}

std::vector<Eigen::Index> allColumns(Eigen::Index n) {
    std::vector<Eigen::Index> cols;
    for (Eigen::Index c = 0; c < n; ++c) cols.push_back(c);
    return cols;
}

TEST(ConditionalVarianceFrameTest, BorderKnownVarianceMatchesZeroMatrixFormBitForBit) {
    std::mt19937 g(29102026u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::Index n_s = 23;
    NetworkNormalEquations neq(n_s);
    const Eigen::MatrixXd zero = Eigen::MatrixXd::Zero(n_s, n_s);
    for (int trial = 0; trial < 6; ++trial) {
        const Eigen::Index n_r = 3 + trial;
        Eigen::MatrixXd N_sr(n_s, n_r);
        for (Eigen::Index i = 0; i < n_s; ++i)
            for (Eigen::Index j = 0; j < n_r; ++j) N_sr(i, j) = nd(g);
        // Signs of the regression entries decide whether 0 * m_c(j) is +0 or -0.
        if (trial % 2 == 1) N_sr = -N_sr;
        const std::size_t st = addRandomStation(neq, n_s, n_r, N_sr, g);
        const std::vector<Eigen::Index> cols = allColumns(n_r);
        const Eigen::VectorXd oracle = neq.reconstructStationCovarianceDiag(st, cols, zero, false);
        const Eigen::VectorXd fast   = neq.stationConditionalVarianceDiag(st, cols);
        EXPECT_TRUE(sameBits(oracle, fast)) << "trial " << trial;
        // A subset, out of order and repeated.
        const std::vector<Eigen::Index> sub = {n_r - 1, 0, n_r - 1};
        EXPECT_TRUE(sameBits(neq.reconstructStationCovarianceDiag(st, sub, zero, false),
                             neq.stationConditionalVarianceDiag(st, sub)));
    }
}

TEST(ConditionalVarianceFrameTest, BorderKnownVarianceKeepsTheNaNOfANonFiniteRegression) {
    std::mt19937 g(7u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::Index n_s = 9, n_r = 4;
    const Eigen::MatrixXd zero = Eigen::MatrixXd::Zero(n_s, n_s);
    const double bad[] = {std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()};
    int reached = 0;
    for (double v : bad) {
        NetworkNormalEquations neq(n_s);
        Eigen::MatrixXd N_sr(n_s, n_r);
        for (Eigen::Index i = 0; i < n_s; ++i)
            for (Eigen::Index j = 0; j < n_r; ++j) N_sr(i, j) = nd(g);
        N_sr(4, 2) = v;   // one non-finite regression entry
        std::size_t st = 0;
        try {
            st = addRandomStation(neq, n_s, n_r, N_sr, g);
        } catch (const std::exception&) {
            continue;     // the kernel refuses the block: the case cannot reach the readback
        }
        const std::vector<Eigen::Index> cols = allColumns(n_r);
        const Eigen::VectorXd oracle = neq.reconstructStationCovarianceDiag(st, cols, zero, false);
        const Eigen::VectorXd fast   = neq.stationConditionalVarianceDiag(st, cols);
        EXPECT_TRUE(sameBits(oracle, fast));
        EXPECT_TRUE(oracle.array().isNaN().any());   // the case is the one the shortcut must not skip
        ++reached;
    }
    EXPECT_GT(reached, 0);   // at least one non-finite regression reaches the readback
}

TEST(ConditionalVarianceFrameTest, BorderKnownVarianceOfAnInfiniteLocalWeight) {
    // An infinite local information weight would give a zero (N_rr⁻¹)_{cc}, the other
    // case the shortcut must not skip.  Whether the kernel admits such a block is its
    // own business; if it does, the two forms must agree bit for bit.
    std::mt19937 g(13u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::Index n_s = 6, n_r = 3;
    NetworkNormalEquations neq(n_s);
    Eigen::MatrixXd N_sr(n_s, n_r);
    for (Eigen::Index i = 0; i < n_s; ++i)
        for (Eigen::Index j = 0; j < n_r; ++j) N_sr(i, j) = nd(g);
    Eigen::MatrixXd N_rr = Eigen::MatrixXd::Identity(n_r, n_r);
    N_rr(1, 1) = std::numeric_limits<double>::infinity();
    std::size_t st = 0;
    try {
        st = neq.addStation(Eigen::MatrixXd::Zero(n_s, n_s), N_sr, N_rr,
                            Eigen::VectorXd::Zero(n_s), Eigen::VectorXd::Zero(n_r));
    } catch (const std::exception&) {
        // The kernel refuses the block (its positive-definiteness test), so a zero
        // variance cannot reach the readback and the shortcut's zero case is defensive.
        SUCCEED();
        return;
    }
    const std::vector<Eigen::Index> cols = allColumns(n_r);
    EXPECT_TRUE(sameBits(neq.reconstructStationCovarianceDiag(st, cols,
                                                             Eigen::MatrixXd::Zero(n_s, n_s), false),
                         neq.stationConditionalVarianceDiag(st, cols)));
}

TEST(ConditionalVarianceFrameTest, BorderKnownVarianceWithNoSharedColumns) {
    std::mt19937 g(11u);
    NetworkNormalEquations neq(0);
    const Eigen::Index n_r = 5;
    const std::size_t st = neq.addStation(Eigen::MatrixXd(0, 0), Eigen::MatrixXd(0, n_r),
                                          randomSpd(n_r, g), Eigen::VectorXd(0),
                                          Eigen::VectorXd::Zero(n_r));
    const std::vector<Eigen::Index> cols = allColumns(n_r);
    EXPECT_TRUE(sameBits(neq.reconstructStationCovarianceDiag(st, cols, Eigen::MatrixXd(0, 0), false),
                         neq.stationConditionalVarianceDiag(st, cols)));
    EXPECT_EQ(neq.stationConditionalVarianceDiag(st, {}).size(), 0);
}

// A small network: per satellite a clock and a phase bias (shared); per station one
// ambiguity per satellite and a receiver clock (local).  Code rows make the clocks
// observable; the datum pins one satellite clock.
struct SmallNetwork {
    static constexpr int kStations = 4;
    static constexpr int kSats = 5;
    std::vector<Eigen::Index> clk, pb;
    std::vector<std::size_t> st;
    NetworkEstimator::Datum datum;

    void registerColumns(NetworkEstimator& est) {
        clk.clear(); pb.clear();
        for (int s = 0; s < kSats; ++s) {
            const std::string sat = "G0" + std::to_string(s + 1);
            clk.push_back(est.registerClock(sat));
            pb.push_back(est.registerPhaseBias(sat, "L1C"));
        }
        datum = {{clk[0], 0.0}};
    }

    void addEpoch(NetworkEstimator& est, unsigned seed) {
        std::mt19937 g(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        const Eigen::Index n_s = est.numShared();
        const Eigen::Index n_r = kSats + 1;   // ambiguities, then the receiver clock
        st.clear();
        for (int r = 0; r < kStations; ++r) {
            const Eigen::Index m = 2 * kSats;
            Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, n_s);
            Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_r);
            Eigen::VectorXd w(m), y(m);
            for (int s = 0; s < kSats; ++s) {
                const Eigen::Index pr = 2 * s, ph = 2 * s + 1;
                A_s(pr, clk[s]) = -1.0; A_r(pr, kSats) = 1.0; w(pr) = 4.0;   y(pr) = nd(g);
                A_s(ph, clk[s]) = -1.0; A_s(ph, pb[s]) = 1.0; A_r(ph, kSats) = 1.0;
                A_r(ph, s) = 1.0;       w(ph) = 100.0; y(ph) = 0.1 * nd(g);
            }
            // A weak prior on every local keeps N_rr positive-definite (the ambiguity
            // and the phase bias are otherwise one direction at a single station).
            NetworkNormalEquations::StationPrior prior;
            for (Eigen::Index c = 0; c < n_r; ++c) prior.cols.push_back(c);
            prior.dNrr = Eigen::VectorXd::Constant(n_r, 1e-2 * (1.0 + 0.1 * r));
            prior.dur  = Eigen::VectorXd::Zero(n_r);
            st.push_back(est.addStation(A_s, A_r, w, y, prior));
        }
    }
};

/// The datum frame built by hand: the oracle for the stored frame.
Eigen::MatrixXd handFrame(const NetworkEstimator& est, const NetworkEstimator::Datum& datum,
                          const std::vector<Eigen::Index>& gauge_cols = {}) {
    Eigen::MatrixXd f = est.sharedCovariance();
    for (const auto& p : datum)
        if (p.column >= 0 && p.column < f.rows()) { f.row(p.column).setZero(); f.col(p.column).setZero(); }
    for (Eigen::Index c : gauge_cols)
        if (c >= 0 && c < f.rows()) { f.row(c).setZero(); f.col(c).setZero(); }
    return f;
}

TEST(ConditionalVarianceFrameTest, StoredFrameMatchesAPerCallBuildForEveryConsumer) {
    NetworkEstimator est;
    SmallNetwork net;
    net.registerColumns(est);
    est.beginEpoch();
    net.addEpoch(est, 101u);
    (void)est.solve(net.datum);

    const Eigen::MatrixXd frame = handFrame(est, net.datum);
    EXPECT_TRUE(sameBits(est.datumFrameSharedCov(net.datum), frame));

    const NetworkNormalEquations& neq = est.normals();
    const std::vector<Eigen::Index> cols = allColumns(SmallNetwork::kSats + 1);
    for (std::size_t si : net.st) {
        EXPECT_TRUE(sameBits(est.conditionalLocalVarianceDiag(si, cols, net.datum),
                             neq.reconstructStationCovarianceDiag(si, cols, frame, false)));
        EXPECT_TRUE(sameBits(est.conditionalLocalCovariance(si, net.datum),
                             neq.reconstructStationCovariance(si, frame, false)));
        EXPECT_TRUE(sameBits(est.pureConditionalLocalVarianceDiag(si, cols),
                             neq.reconstructStationCovarianceDiag(
                                 si, cols, Eigen::MatrixXd::Zero(frame.rows(), frame.cols()), false)));
    }
    const std::vector<std::pair<std::size_t, Eigen::Index>> req = {
        {net.st[0], 0}, {net.st[1], 0}, {net.st[2], 3}, {net.st[3], 1}};
    EXPECT_TRUE(sameBits(est.jointConditionalCovariance(req, net.datum),
                         neq.jointConditionalCovariance(req, frame, false)));
    const std::vector<std::vector<int>> blocks = {{0, 1}, {2, 3}};
    const auto got  = est.blockConditionalCovariances(req, net.datum, blocks);
    const auto want = neq.blockConditionalCovariances(req, frame, blocks);
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t b = 0; b < got.size(); ++b) EXPECT_TRUE(sameBits(got[b], want[b]));
}

TEST(ConditionalVarianceFrameTest, ADatumWithOtherPinnedColumnsBuildsItsOwnFrame) {
    NetworkEstimator est;
    SmallNetwork net;
    net.registerColumns(est);
    est.beginEpoch();
    net.addEpoch(est, 202u);
    (void)est.solve(net.datum);

    const NetworkNormalEquations& neq = est.normals();
    const std::vector<Eigen::Index> cols = allColumns(SmallNetwork::kSats + 1);
    // Another column pinned, and the solve's pin listed twice: both differ from the
    // solve's pinned columns, so neither may read the stored frame.
    const NetworkEstimator::Datum other = {{net.clk[0], 0.0}, {net.pb[2], 0.0}};
    const NetworkEstimator::Datum twice = {{net.clk[0], 0.0}, {net.clk[0], 0.0}};
    ASSERT_FALSE(sameBits(handFrame(est, other), handFrame(est, net.datum)));   // the test can fail
    EXPECT_TRUE(sameBits(est.datumFrameSharedCov(other), handFrame(est, other)));
    EXPECT_TRUE(sameBits(est.datumFrameSharedCov(twice), handFrame(est, net.datum)));
    for (std::size_t si : net.st)
        EXPECT_TRUE(sameBits(est.conditionalLocalVarianceDiag(si, cols, other),
                             neq.reconstructStationCovarianceDiag(si, cols, handFrame(est, other),
                                                                  false)));
}

TEST(ConditionalVarianceFrameTest, GaugesSetAfterTheSolveAreZeroedInTheFrame) {
    NetworkEstimator est;
    SmallNetwork net;
    net.registerColumns(est);
    est.beginEpoch();
    net.addEpoch(est, 303u);
    (void)est.solve(net.datum);

    NetworkNormalEquations::SumZeroGauge gz;
    gz.shared_col = net.pb[1];
    est.setPhaseSumZeroGauges({gz});
    const Eigen::MatrixXd frame = handFrame(est, net.datum, {net.pb[1]});
    EXPECT_TRUE(sameBits(est.datumFrameSharedCov(net.datum), frame));
    const std::vector<Eigen::Index> cols = allColumns(SmallNetwork::kSats + 1);
    const Eigen::MatrixXd no_gauge = handFrame(est, net.datum);
    bool discriminates = false;   // the frame without the gauge gives other variances
    for (std::size_t si : net.st) {
        const Eigen::VectorXd want =
            est.normals().reconstructStationCovarianceDiag(si, cols, frame, false);
        EXPECT_TRUE(sameBits(est.conditionalLocalVarianceDiag(si, cols, net.datum), want));
        discriminates = discriminates ||
            !sameBits(want, est.normals().reconstructStationCovarianceDiag(si, cols, no_gauge, false));
    }
    EXPECT_TRUE(discriminates);
}

TEST(ConditionalVarianceFrameTest, EachEpochReadsItsOwnSolve) {
    NetworkEstimator est;
    SmallNetwork net;
    const std::vector<Eigen::Index> cols = allColumns(SmallNetwork::kSats + 1);
    for (unsigned seed : {404u, 505u}) {
        est.reset();
        net.registerColumns(est);
        est.beginEpoch();
        net.addEpoch(est, seed);
        (void)est.solve(net.datum);
        const Eigen::MatrixXd frame = handFrame(est, net.datum);
        EXPECT_TRUE(sameBits(est.datumFrameSharedCov(net.datum), frame));
        for (std::size_t si : net.st)
            EXPECT_TRUE(sameBits(est.conditionalLocalVarianceDiag(si, cols, net.datum),
                                 est.normals().reconstructStationCovarianceDiag(si, cols, frame,
                                                                                false)));
    }
}

}  // namespace
}  // namespace GnssProcessor
