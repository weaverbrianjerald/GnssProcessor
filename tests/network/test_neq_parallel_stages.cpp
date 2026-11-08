/**
 * @file test_neq_parallel_stages.cpp
 * @brief Tests for the parallel stages of the network normal equations:
 *        `NetworkNormalEquations::formStations` / `commitStation`
 *        (`network.parallel_station_normals`) and the column-split Schur reduce
 *        (`setSchurColumnThreads`, `network.parallel_schur_columns`).
 *
 * Both exist for speed and may not change a single bit, at any thread count:
 *
 *  1. **Form and commit against the serial add.**  Two kernels fed the same stations, one by
 *     `addStationObservations` in station order and one by one `formStations` pass on four
 *     threads followed by `commitStation` in station order, hold bitwise equal shared border,
 *     cross blocks, local normals, right-hand sides, pivots and reduced normals, with the dense
 *     products and with the sparse formation, with and without a station prior.
 *  2. **A rejected station.**  A station the positive-definiteness test rejects throws the same
 *     message from its commit as from the serial add, records the same null-direction trace, and
 *     leaves no trace in the border; a slot cannot be committed twice or after a reset.
 *  3. **Column-split reduce against the serial reduce.**  The reduced normals and right-hand side
 *     are bitwise equal over every residue of the shared size modulo the row tile of either build
 *     (4 rows on SSE2, 12 on AVX2, so modulo 12; the 4-column tile is the same on both), local sizes
 *     on both sides of the 8-deep peeling, and odd and even thread counts; every one of those shapes
 *     must take the split.  The shapes include products Eigen cuts into row panels and into column
 *     panels on either build (at the network's own shape SSE2 cuts the columns at 212 and AVX2 the
 *     rows at 216).  The network's own shape (about 420 shared columns, 50 locals) must take the
 *     split, so the test cannot pass by declining everything; a shape the split declines still
 *     gives the serial result.
 *  4. **The formation counts.**  The serial add and the formed-and-committed kernel count the same
 *     sparse and dense formations, and nothing while the sparse formation is off.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "network/solve/NormalEquations.hpp"

namespace GnssProcessor {
namespace {

/// Eigen threads pinned to one for the scope of a test (the network's epoch loop pins them the same
/// way; both parallel stages require it).
struct OneEigenThread {
    int saved;
    OneEigenThread() : saved(Eigen::nbThreads()) { Eigen::setNbThreads(1); }
    ~OneEigenThread() { Eigen::setNbThreads(saved); }
};

bool bitEqual(const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
    return a.rows() == b.rows() && a.cols() == b.cols() &&
           (a.size() == 0 ||
            std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0);
}

struct Rows {
    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
};

/// A station's rows: each touches one to four shared columns and up to five locals, plus a receiver
/// clock column in every row and a few block rows dense across the border (the network's shape).
Rows randomRows(std::mt19937_64& rng, int m, int n_shared, int n_local) {
    std::uniform_real_distribution<double> U(-1.0, 1.0), W(1e-4, 1e4), P(0.0, 1.0);
    Rows R;
    R.A_s = Eigen::MatrixXd::Zero(m, n_shared);
    R.A_r = Eigen::MatrixXd::Zero(m, n_local);
    R.w.resize(m);
    R.y.resize(m);
    for (int k = 0; k < m; ++k) {
        const int nzs = 1 + static_cast<int>(rng() % 4), nzr = static_cast<int>(rng() % 6);
        for (int t = 0; t < nzs; ++t) R.A_s(k, static_cast<Eigen::Index>(rng() % n_shared)) = U(rng);
        for (int t = 0; t < nzr && n_local > 0; ++t)
            R.A_r(k, static_cast<Eigen::Index>(rng() % n_local)) = U(rng);
        if (n_local > 0) R.A_r(k, 0) = U(rng);
        if (k >= m - 6) {
            for (int c = 0; c < n_shared; ++c) if (P(rng) < 0.5) R.A_s(k, c) = U(rng);
        }
        R.w(k) = W(rng);
        R.y(k) = U(rng);
    }
    return R;
}

/// A diagonal prior on every local column (each block positive definite).
NetworkNormalEquations::StationPrior fullPrior(int n_local, double info) {
    NetworkNormalEquations::StationPrior prior;
    for (int c = 0; c < n_local; ++c) prior.cols.push_back(c);
    prior.dNrr = Eigen::VectorXd::Constant(n_local, info);
    prior.dur = Eigen::VectorXd::Constant(n_local, 0.1);
    return prior;
}

/// Feeds @p n_sta stations to a serial kernel and to a formed-and-committed one, checking every block
/// after every station and the reduced system at the end.
void compareFormAndCommit(bool sparse, bool with_prior, int seed) {
    const OneEigenThread pin;
    std::mt19937_64 rng(seed);
    const int ns = 275, nr = 48, n_sta = 9;
    NetworkNormalEquations serial(ns), formed(ns);
    serial.setSparseStationNormals(sparse);
    formed.setSparseStationNormals(sparse);
    std::vector<Rows> rows;
    std::vector<NetworkNormalEquations::StationPrior> priors;
    for (int s = 0; s < n_sta; ++s) {
        rows.push_back(randomRows(rng, 70 + 9 * s, ns, nr));
        priors.push_back(with_prior ? fullPrior(nr, 0.5) : NetworkNormalEquations::StationPrior{});
        if (!with_prior) rows.back().A_r.diagonal().head(std::min(70 + 9 * s, nr)).array() += 3.0;
    }
    std::vector<NetworkNormalEquations::StationObservations> in;
    for (int s = 0; s < n_sta; ++s)
        in.push_back({&rows[s].A_s, &rows[s].A_r, &rows[s].w, &rows[s].y, &priors[s]});
    formed.formStations(in, 4);
    const std::string what = std::string(sparse ? "sparse" : "dense") + (with_prior ? ", prior" : "");
    for (int s = 0; s < n_sta; ++s) {
        bool threw_a = false, threw_b = false;
        std::size_t a = 0, b = 0;
        try { a = serial.addStationObservations(rows[s].A_s, rows[s].A_r, rows[s].w, rows[s].y, priors[s]); }
        catch (const std::exception&) { threw_a = true; }
        try { b = formed.commitStation(static_cast<std::size_t>(s)); }
        catch (const std::exception&) { threw_b = true; }
        ASSERT_EQ(threw_a, threw_b) << what << ", station " << s;
        if (threw_a) continue;
        ASSERT_EQ(a, b);
        EXPECT_TRUE(bitEqual(serial.stationCrossBlock(a), formed.stationCrossBlock(b))) << "N_sr, " << what << ", station " << s;
        EXPECT_TRUE(bitEqual(serial.stationLocalNormals(a), formed.stationLocalNormals(b))) << "N_rr, " << what << ", station " << s;
        EXPECT_TRUE(bitEqual(serial.stationLocalRhs(a), formed.stationLocalRhs(b))) << "u_r, " << what << ", station " << s;
        EXPECT_TRUE(bitEqual(serial.stationLocalPivots(a), formed.stationLocalPivots(b))) << "pivots, " << what << ", station " << s;
        EXPECT_TRUE(bitEqual(serial.sharedNormals(), formed.sharedNormals())) << "border, " << what << ", station " << s;
        EXPECT_TRUE(bitEqual(serial.sharedRhs(), formed.sharedRhs())) << "border rhs, " << what << ", station " << s;
    }
    EXPECT_EQ(serial.numStations(), formed.numStations());
    EXPECT_GT(serial.numStations(), 0u) << "the stations must be accepted, or the test compares nothing";
    const Eigen::MatrixXd ridge = Eigen::MatrixXd::Identity(ns, ns);
    serial.addSharedInfo(ridge, Eigen::VectorXd::Zero(ns));
    formed.addSharedInfo(ridge, Eigen::VectorXd::Zero(ns));
    serial.reduce();
    formed.reduce();
    EXPECT_TRUE(bitEqual(serial.reducedSharedNormals(), formed.reducedSharedNormals())) << what;
    EXPECT_TRUE(bitEqual(serial.reducedSharedRhs(), formed.reducedSharedRhs())) << what;
    EXPECT_EQ(formed.parallelStageCounts().forms_parallel, n_sta) << "the forms must run in parallel";
    EXPECT_EQ(formed.parallelStageCounts().commits, n_sta);
    // Both kernels count every station's formation once, at the same point; on a build whose kernel
    // the sparse formation does not reproduce, every station is formed densely.
    const auto& cs = serial.stationFormationCounts();
    const auto& cf = formed.stationFormationCounts();
    EXPECT_EQ(cs.sparse, cf.sparse) << what;
    EXPECT_EQ(cs.dense, cf.dense) << what;
    if (!sparse) {
        EXPECT_EQ(cf.sparse + cf.dense, 0) << "nothing is counted while the sparse formation is off";
    } else {
        EXPECT_EQ(cf.sparse + cf.dense, n_sta) << what;
        if (NetworkNormalEquations::sparseFormationOnThisBuild())
            EXPECT_EQ(cf.sparse, n_sta) << "every station of this shape is formed sparse, " << what;
        else
            EXPECT_EQ(cf.dense, n_sta) << "a build with fused multiply-add forms every station densely";
    }
}

TEST(NeqParallelStagesTest, FormAndCommitMatchTheSerialAddDense) {
    compareFormAndCommit(false, true, 11);
}

TEST(NeqParallelStagesTest, FormAndCommitMatchTheSerialAddSparse) {
    compareFormAndCommit(true, true, 12);
}

TEST(NeqParallelStagesTest, FormAndCommitMatchTheSerialAddWithoutAPrior) {
    compareFormAndCommit(true, false, 13);
    compareFormAndCommit(false, false, 14);
}

TEST(NeqParallelStagesTest, ARejectedStationThrowsTheSameFromItsCommit) {
    const OneEigenThread pin;
    std::mt19937_64 rng(5);
    const int ns = 120, nr = 60;
    for (const bool sparse : {false, true}) {
        NetworkNormalEquations serial(ns), formed(ns);
        serial.setSparseStationNormals(sparse);
        formed.setSparseStationNormals(sparse);
        serial.setStationPdNullTrace(true);
        formed.setStationPdNullTrace(true);
        Rows bad = randomRows(rng, 80, ns, nr);
        bad.A_r.col(7).setZero();                    // an unobserved local column: the block is singular
        const Rows good = randomRows(rng, 80, ns, nr);
        const NetworkNormalEquations::StationPrior prior = fullPrior(nr, 1.0);
        const NetworkNormalEquations::StationPrior none;
        std::vector<NetworkNormalEquations::StationObservations> in{
            {&bad.A_s, &bad.A_r, &bad.w, &bad.y, &none},
            {&good.A_s, &good.A_r, &good.w, &good.y, &prior}};
        formed.formStations(in, 2);
        std::string msg_a, msg_b;
        try { serial.addStationObservations(bad.A_s, bad.A_r, bad.w, bad.y); }
        catch (const std::invalid_argument& e) { msg_a = e.what(); }
        try { formed.commitStation(0); }
        catch (const std::invalid_argument& e) { msg_b = e.what(); }
        EXPECT_FALSE(msg_a.empty());
        EXPECT_EQ(msg_a, msg_b);
        const auto& fa = serial.lastStationPdFailure();
        const auto& fb = formed.lastStationPdFailure();
        EXPECT_EQ(fa.valid, fb.valid);
        EXPECT_EQ(fa.n_eig_below, fb.n_eig_below);
        EXPECT_TRUE(bitEqual(fa.null_vec, fb.null_vec));
        EXPECT_EQ(serial.numStations(), 0u);
        EXPECT_EQ(formed.numStations(), 0u);
        EXPECT_TRUE(bitEqual(serial.sharedNormals(), formed.sharedNormals()));
        const std::size_t a = serial.addStationObservations(good.A_s, good.A_r, good.w, good.y, prior);
        const std::size_t b = formed.commitStation(1);
        EXPECT_EQ(a, b);
        EXPECT_TRUE(bitEqual(serial.sharedNormals(), formed.sharedNormals()));
        EXPECT_TRUE(bitEqual(serial.stationLocalNormals(a), formed.stationLocalNormals(b)));
        EXPECT_THROW(formed.commitStation(1), std::out_of_range);   // already committed
        EXPECT_THROW(formed.commitStation(2), std::out_of_range);   // never formed
        formed.reset(ns);
        EXPECT_THROW(formed.commitStation(0), std::out_of_range);   // no slot survives a reset
    }
}

/// Two kernels with the same stations, one reducing serially and one by column blocks on
/// @p threads threads; returns whether the split ran.
bool compareSchur(int ns, int nr, int threads, int n_sta, int seed) {
    const OneEigenThread pin;
    std::mt19937_64 rng(seed);
    NetworkNormalEquations serial(ns), split(ns);
    split.setSchurColumnThreads(threads);
    for (int s = 0; s < n_sta; ++s) {
        const int nr_s = (s % 3 == 2) ? 0 : nr;   // some stations have no locals
        const Rows R = randomRows(rng, std::max(nr_s, 8) + 30, ns, nr_s);
        const NetworkNormalEquations::StationPrior prior = fullPrior(nr_s, 0.7);
        serial.addStationObservations(R.A_s, R.A_r, R.w, R.y, prior);
        split.addStationObservations(R.A_s, R.A_r, R.w, R.y, prior);
    }
    serial.reduce();
    split.reduce();
    const std::string shape = "ns=" + std::to_string(ns) + " nr=" + std::to_string(nr) +
                              " threads=" + std::to_string(threads);
    EXPECT_TRUE(bitEqual(serial.reducedSharedNormals(), split.reducedSharedNormals())) << shape;
    EXPECT_TRUE(bitEqual(serial.reducedSharedRhs(), split.reducedSharedRhs())) << shape;
    return split.parallelStageCounts().schur_split == 1;
}

TEST(NeqParallelStagesTest, ColumnSplitReduceMatchesTheSerialReduceBitForBit) {
    using Traits = Eigen::internal::gebp_traits<double, double>;
    // Every residue of the shared size modulo the row tile of either build (64 to 75: modulo 12, and
    // so modulo 4), the network's border (421: SSE2 cuts it into two column panels, AVX2 into two row
    // panels of 216), local sizes on both sides of the 8-deep peeling, odd and even thread counts.
    // From 64 to 75 Eigen also cuts row panels (5 locals) and column panels (49 locals) on both builds.
    for (const int ns : {64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 421})
        for (const int nr : {5, 49})
            for (const int threads : {3, 20})
                EXPECT_TRUE(compareSchur(ns, nr, threads, 5, ns * 31 + nr * 7 + threads))
                    << "ns=" << ns << " nr=" << nr << " threads=" << threads << " must take the split"
                    << " (Eigen tile mr=" << Traits::mr << ", nr=" << Traits::nr << ")";
    EXPECT_TRUE(compareSchur(418, 49, 20, 12, 3)) << "the network's own shape must take the split";
}

TEST(NeqParallelStagesTest, ADeclinedSplitGivesTheSerialResult) {
    // Too narrow a border to cut, and depths beyond one panel of Eigen's blocking: both decline and
    // both still equal the serial reduce.
    EXPECT_FALSE(compareSchur(12, 4, 8, 5, 1));
    compareSchur(64, 520, 8, 2, 2);   // a depth beyond one panel where L1 is 32 KB
    // Eigen threaded: the split declines (the serial reduce would partition its rows).
    std::mt19937_64 rng(9);
    NetworkNormalEquations split(200);
    split.setSchurColumnThreads(8);
    const Rows R = randomRows(rng, 60, 200, 20);
    split.addStationObservations(R.A_s, R.A_r, R.w, R.y, fullPrior(20, 1.0));
    const int saved = Eigen::nbThreads();
    Eigen::setNbThreads(4);
    split.reduce();
    Eigen::setNbThreads(saved);
    EXPECT_EQ(split.parallelStageCounts().schur_split, 0);
    EXPECT_EQ(split.parallelStageCounts().schur_declined, 1);
}

} // namespace
} // namespace GnssProcessor
