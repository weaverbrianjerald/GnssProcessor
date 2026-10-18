/**
 * @file test_neq_sparse_station_normals.cpp
 * @brief Tests for the sparse formation of a station's normal blocks
 *        (`NetworkNormalEquations::formStationNormalsSparse`, `network.sparse_station_normals`).
 *
 * The sparse formation exists for speed and may not change a single bit.  It reproduces the
 * summation order of Eigen's blocked product, which is a property of Eigen's kernel, so the
 * guarantees are tested against Eigen itself:
 *
 *  1. **Bitwise equality with the dense products.**  Whenever the function reports that it formed
 *     the blocks, @f$N_{ss}, N_{sr}, N_{rr}@f$ equal Eigen's dense `A.transpose() * (w.asDiagonal() * A)`
 *     products bit for bit, over every residue of the row count modulo the tile sizes, row counts on
 *     both sides of the 8-row depth peeling, sparse rows and rows with a dense column, and the network's
 *     own shape (about 420 shared columns, 50 locals, 90 rows).  The network's shape must be accepted,
 *     so the test cannot pass by refusing everything.
 *  2. **Refusal outside the proven region.**  A vector result, tiny sizes, a depth that Eigen splits
 *     into several panels, more than one Eigen thread, and non-finite input all return false and
 *     leave the caller on the dense path.
 *  3. **Kernel level.**  Two kernels fed the same stations, one with the option on, hold bitwise equal
 *     shared border, cross blocks, local normals, local right-hand sides, local pivots and reduced
 *     normals; a station the positive-definiteness test rejects is rejected identically and leaves
 *     no trace in either.
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

#include "network/solve/NormalEquations.hpp"

namespace GnssProcessor {
namespace {

/// Eigen threads pinned to one for the scope of a test (the network's epoch loop pins them the same
/// way); the sparse formation refuses to run otherwise.
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

/// Rows of a station: each touches one to four shared columns and up to five locals, as the
/// network's rows do (a clock, a bias, an ambiguity, an ionosphere term), with weights over eight
/// decades.  @p dense_cols adds a shared and a local column present in every row, so some sums have as
/// many terms as there are rows (the receiver clock of a real station), and a large coefficient scale.
/// @p block_rows turns the last rows into block rows as the carried priors make them: @p density of
/// the shared columns and up to eight locals nonzero in each.
Rows randomRows(std::mt19937_64& rng, int m, int n_shared, int n_local, bool dense_cols,
                int block_rows = 0, double density = 0.0) {
    std::uniform_real_distribution<double> U(-1.0, 1.0), W(1e-4, 1e4);
    Rows R;
    R.A_s = Eigen::MatrixXd::Zero(m, n_shared);
    R.A_r = Eigen::MatrixXd::Zero(m, n_local);
    R.w.resize(m);
    R.y.resize(m);
    for (int k = 0; k < m; ++k) {
        const int nzs = 1 + static_cast<int>(rng() % 4), nzr = static_cast<int>(rng() % 6);
        for (int t = 0; t < nzs; ++t) {
            R.A_s(k, static_cast<Eigen::Index>(rng() % n_shared)) = U(rng) * (dense_cols ? 1e3 : 1.0);
        }
        for (int t = 0; t < nzr && n_local > 0; ++t) {
            R.A_r(k, static_cast<Eigen::Index>(rng() % n_local)) = U(rng);
        }
        if (dense_cols) {
            R.A_s(k, 0) = U(rng);
            if (n_shared > 3) R.A_s(k, n_shared - 2) = U(rng);
            if (n_local > 0) R.A_r(k, 0) = U(rng);
            if (n_local > 3) R.A_r(k, n_local - 1) = U(rng);
        }
        if (k >= m - block_rows) {
            for (int c = 0; c < n_shared; ++c) {
                if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < density) R.A_s(k, c) = U(rng);
            }
            for (int t = 0; t < 8 && n_local > 0; ++t) R.A_r(k, static_cast<Eigen::Index>(rng() % n_local)) = U(rng);
        }
        R.w(k) = W(rng);
        R.y(k) = U(rng);
    }
    return R;
}

/// Compares the sparse formation of one random station with the dense products.  Returns whether the
/// function accepted the shape.
bool compareOneShape(std::mt19937_64& rng, int m, int ns, int nr, bool dense_cols,
                     int block_rows = 0, double density = 0.0) {
    const Rows R = randomRows(rng, m, ns, nr, dense_cols, block_rows, density);
    const Eigen::MatrixXd WAs = R.w.asDiagonal() * R.A_s, WAr = R.w.asDiagonal() * R.A_r;
    const Eigen::MatrixXd Nss = R.A_s.transpose() * WAs, Nsr = R.A_s.transpose() * WAr,
                          Nrr = R.A_r.transpose() * WAr;
    Eigen::MatrixXd Nss2 = Eigen::MatrixXd::Zero(ns, ns), Nsr2, Nrr2;
    NetworkNormalEquations::SparseBorder border;
    if (!NetworkNormalEquations::formStationNormalsSparse(R.A_s, R.A_r, R.w, Nss2, Nsr2, Nrr2, border)) {
        EXPECT_TRUE(Nss2.isZero(0.0)) << "a refusal must write nothing";
        return false;
    }
    const std::string shape = "m=" + std::to_string(m) + " ns=" + std::to_string(ns) +
                              " nr=" + std::to_string(nr) + (dense_cols ? " dense columns" : "") +
                              (block_rows ? " block rows " + std::to_string(block_rows) + " at " + std::to_string(density) : "");
    EXPECT_TRUE(bitEqual(Nss, Nss2)) << "N_ss " << shape;
    EXPECT_TRUE(bitEqual(Nsr, Nsr2)) << "N_sr " << shape;
    EXPECT_TRUE(bitEqual(Nrr, Nrr2)) << "N_rr " << shape;
    // The kernel adds and clears the square of the active columns only: the columns are ascending and
    // distinct, and N_ss is zero everywhere else.
    std::vector<char> active(static_cast<std::size_t>(ns), 0);
    Eigen::Index previous = -1;
    for (const Eigen::Index j : border.active) {
        EXPECT_GT(j, previous) << "active columns are not ascending " << shape;
        previous = j;
        active[static_cast<std::size_t>(j)] = 1;
    }
    for (Eigen::Index j = 0; j < ns; ++j) {
        for (Eigen::Index i = 0; i < ns; ++i) {
            if (!active[static_cast<std::size_t>(i)] || !active[static_cast<std::size_t>(j)]) {
                EXPECT_EQ(Nss2(i, j), 0.0) << "nonzero entry outside the active square " << shape;
            }
        }
    }
    // The two numbers of the symmetry check are the ones the full scan takes.
    EXPECT_EQ(border.asymmetry, (Nss2 - Nss2.transpose()).cwiseAbs().maxCoeff()) << shape;
    EXPECT_EQ(border.magnitude, Nss2.cwiseAbs().maxCoeff()) << shape;
    return true;
}

TEST(NeqSparseStationNormalsTest, MatchesTheDenseProductsBitForBit) {
    const OneEigenThread pin;
    std::mt19937_64 rng(20261018);
    int accepted = 0, refused = 0;
    // Row counts of the shared and local dimension over every residue modulo the 4-row and 2-row
    // tiles, depths on both sides of the 8-row peeling, with and without dense columns.
    for (int ns : {23, 41, 42, 43, 90})
        for (int nr : {6, 7, 9, 10, 48})
            for (int m : {5, 8, 9, 16, 17, 31, 56, 57, 93, 130})
                for (int dense = 0; dense < 2; ++dense) {
                    if (compareOneShape(rng, m, ns, nr, dense != 0)) ++accepted; else ++refused;
                }
    EXPECT_GT(accepted, 4 * refused) << accepted << " accepted, " << refused << " refused";
    // The network's own shape: about 420 shared columns, 50 locals and 90 rows.
    for (int dense = 0; dense < 2; ++dense) {
        EXPECT_TRUE(compareOneShape(rng, 93, 418, 48, dense != 0));
        EXPECT_TRUE(compareOneShape(rng, 100, 419, 49, dense != 0));
        EXPECT_TRUE(compareOneShape(rng, 56, 214, 28, dense != 0));
    }
}

TEST(NeqSparseStationNormalsTest, BlockRowsDenseAcrossTheBorderMatchToo) {
    const OneEigenThread pin;
    std::mt19937_64 rng(31);
    // The settled network: a third of the rows are block rows with most shared columns nonzero, among
    // data rows with a few; the dense rows go through the packed vector path, and the rows that hold an
    // entry of a 2-row tile through the split path.
    int accepted = 0;
    for (int ns : {41, 42, 43, 90})
        for (int nr : {9, 10, 48, 49})
            for (int m : {16, 17, 56, 93, 130})
                for (double density : {0.35, 0.9}) {
                    if (compareOneShape(rng, m, ns, nr, false, m / 3, density)) ++accepted;
                    if (compareOneShape(rng, m, ns, nr, true, m / 3, density)) ++accepted;
                }
    EXPECT_GT(accepted, 120);
    EXPECT_TRUE(compareOneShape(rng, 93, 418, 48, false, 30, 0.97));   // the field golden at convergence
    EXPECT_TRUE(compareOneShape(rng, 89, 279, 49, false, 30, 0.97));   // the sim golden
}

TEST(NeqSparseStationNormalsTest, RefusesWhatItCannotReproduce) {
    const OneEigenThread pin;
    std::mt19937_64 rng(7);
    auto run = [&](int m, int ns, int nr, bool poison) {
        Rows R = randomRows(rng, m, ns, nr, false);
        if (poison && m > 0) R.A_s(m / 2, 1) = std::numeric_limits<double>::infinity();
        Eigen::MatrixXd a = Eigen::MatrixXd::Zero(ns, ns), b, c;
        NetworkNormalEquations::SparseBorder border;
        return NetworkNormalEquations::formStationNormalsSparse(R.A_s, R.A_r, R.w, a, b, c, border);
    };
    EXPECT_TRUE(run(100, 275, 48, false));
    EXPECT_FALSE(run(100, 275, 48, true));    // non-finite input: 0 * inf must reach the result as in the dense product
    EXPECT_FALSE(run(100, 275, 1, false));    // a single local column: the dense product is a matrix-vector product
    EXPECT_FALSE(run(3, 4, 3, false));        // tiny: the dense product is evaluated coefficient by coefficient
    EXPECT_FALSE(run(0, 275, 48, false));     // no rows
    EXPECT_FALSE(run(4000, 275, 48, false));  // deeper than one panel of the blocked product
    {   // a non-finite weight
        Rows R = randomRows(rng, 100, 275, 48, false);
        R.w(3) = std::numeric_limits<double>::infinity();
        Eigen::MatrixXd a = Eigen::MatrixXd::Zero(275, 275), b, c;
        NetworkNormalEquations::SparseBorder border;
        EXPECT_FALSE(NetworkNormalEquations::formStationNormalsSparse(R.A_s, R.A_r, R.w, a, b, c, border));
    }
    Eigen::setNbThreads(4);
    EXPECT_FALSE(run(100, 275, 48, false));   // more than one Eigen thread: the product partitions its rows
    Eigen::setNbThreads(1);
    EXPECT_TRUE(run(100, 275, 48, false));
}

TEST(NeqSparseStationNormalsTest, KernelBlocksAreBitwiseEqualWithTheOptionOn) {
    const OneEigenThread pin;
    std::mt19937_64 rng(99);
    const int ns = 275, nr = 48;
    NetworkNormalEquations dense(ns), sparse(ns);
    sparse.setSparseStationNormals(true);
    for (int s = 0; s < 4; ++s) {
        const Rows R = s < 2 ? randomRows(rng, 90 + 7 * s, ns, nr, s == 1)
                             : randomRows(rng, 90 + 7 * s, ns, nr, s == 3, 25, 0.9);
        // A diagonal prior on every local column makes each block positive definite.
        NetworkNormalEquations::StationPrior prior;
        for (int c = 0; c < nr; ++c) prior.cols.push_back(c);
        prior.dNrr = Eigen::VectorXd::Constant(nr, 0.5);
        prior.dur = Eigen::VectorXd::Constant(nr, 0.1);
        const std::size_t a = dense.addStationObservations(R.A_s, R.A_r, R.w, R.y, prior);
        const std::size_t b = sparse.addStationObservations(R.A_s, R.A_r, R.w, R.y, prior);
        ASSERT_EQ(a, b);
        EXPECT_TRUE(bitEqual(dense.stationCrossBlock(a), sparse.stationCrossBlock(b))) << "N_sr, station " << s;
        EXPECT_TRUE(bitEqual(dense.stationLocalNormals(a), sparse.stationLocalNormals(b))) << "N_rr, station " << s;
        EXPECT_TRUE(bitEqual(dense.stationLocalRhs(a), sparse.stationLocalRhs(b))) << "u_r, station " << s;
        EXPECT_TRUE(bitEqual(dense.stationLocalPivots(a), sparse.stationLocalPivots(b))) << "pivots, station " << s;
        EXPECT_TRUE(bitEqual(dense.sharedNormals(), sparse.sharedNormals())) << "border after station " << s;
        EXPECT_TRUE(bitEqual(dense.sharedRhs(), sparse.sharedRhs())) << "border rhs after station " << s;
    }
    // The shared border alone is singular; a ridge makes the reduction well posed for both.
    const Eigen::MatrixXd ridge = Eigen::MatrixXd::Identity(ns, ns);
    dense.addSharedInfo(ridge, Eigen::VectorXd::Zero(ns));
    sparse.addSharedInfo(ridge, Eigen::VectorXd::Zero(ns));
    dense.reduce();
    sparse.reduce();
    EXPECT_TRUE(bitEqual(dense.reducedSharedNormals(), sparse.reducedSharedNormals()));
    EXPECT_TRUE(bitEqual(dense.reducedSharedRhs(), sparse.reducedSharedRhs()));
}

TEST(NeqSparseStationNormalsTest, ARejectedStationLeavesNoTraceWithTheOptionOn) {
    const OneEigenThread pin;
    std::mt19937_64 rng(5);
    const int ns = 120, nr = 60;
    NetworkNormalEquations dense(ns), sparse(ns);
    sparse.setSparseStationNormals(true);
    Rows bad = randomRows(rng, 80, ns, nr, false);
    bad.A_r.col(7).setZero();                    // an unobserved local column: the block is singular
    EXPECT_THROW(dense.addStationObservations(bad.A_s, bad.A_r, bad.w, bad.y), std::invalid_argument);
    EXPECT_THROW(sparse.addStationObservations(bad.A_s, bad.A_r, bad.w, bad.y), std::invalid_argument);
    EXPECT_EQ(dense.numStations(), sparse.numStations());
    EXPECT_EQ(dense.numStations(), 0u);
    EXPECT_TRUE(bitEqual(dense.sharedNormals(), sparse.sharedNormals()));
    NetworkNormalEquations::StationPrior prior;
    for (int c = 0; c < nr; ++c) prior.cols.push_back(c);
    prior.dNrr = Eigen::VectorXd::Constant(nr, 1.0);
    prior.dur = Eigen::VectorXd::Zero(nr);
    const Rows good = randomRows(rng, 80, ns, nr, false);
    const std::size_t a = dense.addStationObservations(good.A_s, good.A_r, good.w, good.y, prior);
    const std::size_t b = sparse.addStationObservations(good.A_s, good.A_r, good.w, good.y, prior);
    EXPECT_EQ(a, b);
    EXPECT_TRUE(bitEqual(dense.sharedNormals(), sparse.sharedNormals()));
    EXPECT_TRUE(bitEqual(dense.stationLocalNormals(a), sparse.stationLocalNormals(b)));
}

} // namespace
} // namespace GnssProcessor
