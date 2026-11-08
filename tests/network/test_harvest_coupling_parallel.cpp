/**
 * @file test_harvest_coupling_parallel.cpp
 * @brief Tests for two parallel stages of the network epoch (card PES, continued): the carry
 *        harvest's columns computed ahead of its walk (`network.parallel_carry_harvest`) and the
 *        coupling preparation's inverse over column blocks (`network.parallel_coupling_prepare`).
 *
 * Both exist for speed and may not change a single bit, at any thread count:
 *
 *  1. **The inverse over column blocks against the whole solve.**  `ldltInverseColumns` equals
 *     `LDLT::solve(Identity)` bit for bit over every residue of the order modulo the row tile of
 *     either build (4 rows on SSE2, 12 on AVX2, so modulo 12; the 4-column tile is the same on
 *     both), a network order and thread counts from 3 to 20; on orders from 120 up the split must be
 *     taken, so the test cannot pass by declining.
 *  2. **The exact update's factor.**  `couplingExactFactor` holds the same F with the split as
 *     without it, and reports which path it took.
 *  3. **The coupling row.**  `networkHarvestCouplingRow` keeps the same terms, in the same order and
 *     with the same values, as the walk over the column map it replaced.
 *  4. **The columns ahead.**  `networkHarvestStationColumnsAhead`, run for every station inside a
 *     parallel region, fills each slot with what the serial regression and coupling row give.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "network/carry/CouplingExact.hpp"
#include "network/carry/LocalCarryHarvest.hpp"
#include "network/solve/NormalEquations.hpp"

namespace GnssProcessor {
namespace {

/// Eigen threads pinned to one for the scope of a test (the network's epoch loop pins them the same
/// way; both stages require it).
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

bool bitEqualScalar(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

/// A symmetric positive definite matrix in correlation scaling with a wide spectrum: the shape of
/// the coupling preparation's scaled carried border.
Eigen::MatrixXd randomCorrelation(std::mt19937_64& rng, Eigen::Index n) {
    std::normal_distribution<double> N(0.0, 1.0);
    Eigen::MatrixXd A(n, n + 8);
    for (Eigen::Index c = 0; c < A.cols(); ++c)
        for (Eigen::Index r = 0; r < n; ++r) A(r, c) = N(rng);
    Eigen::MatrixXd S = A * A.transpose();
    S.diagonal().array() += 1e-6 * static_cast<double>(n);
    const Eigen::VectorXd d = S.diagonal().cwiseSqrt().cwiseInverse();
    return d.asDiagonal() * S * d.asDiagonal();
}

TEST(LdltInverseColumnsTest, MatchesTheWholeSolveBitForBit) {
    const OneEigenThread pin;
    std::mt19937_64 rng(20261106);
    using Traits = Eigen::internal::gebp_traits<double, double>;
    // Small orders, then every residue modulo 12 at orders the split takes on either build (120 to 123
    // and 128 to 131: residues 0 to 3 and 8 to 11; 256 to 259: residues 4 to 7; from 121 up both builds
    // cut the depth into panels), each on two thread counts (the Debug suite affords these), then the
    // network's own order on one.
    struct Shape { Eigen::Index n; std::vector<int> threads; };
    std::vector<Shape> shapes;
    for (const Eigen::Index n : {16, 17, 18, 19, 48, 49, 50, 51, 120, 121, 122, 123, 128, 129, 130, 131,
                                 256, 257, 258, 259})
        shapes.push_back({n, {3, 8}});
    shapes.push_back({421, {20}});
    for (const Shape& shape : shapes) {
        const Eigen::Index n = shape.n;
        const Eigen::MatrixXd C = randomCorrelation(rng, n);
        const Eigen::LDLT<Eigen::MatrixXd> f(C);
        ASSERT_EQ(f.info(), Eigen::Success);
        const Eigen::MatrixXd whole = f.solve(Eigen::MatrixXd::Identity(n, n));
        for (const int threads : shape.threads) {
            bool split = false;
            const Eigen::MatrixXd inv = ldltInverseColumns(f, n, threads, &split);
            EXPECT_TRUE(bitEqual(inv, whole)) << "order " << n << ", " << threads << " threads, split "
                                              << split << " (Eigen tile mr=" << Traits::mr
                                              << ", nr=" << Traits::nr << ")";
            // These orders must take the split, or the test passes by declining.
            if (n >= 120) EXPECT_TRUE(split) << "order " << n << ", " << threads << " threads";
        }
        bool split = true;
        EXPECT_TRUE(bitEqual(ldltInverseColumns(f, n, 1, &split), whole)) << "order " << n;
        EXPECT_FALSE(split) << "one thread takes the whole solve";
    }
}

TEST(LdltInverseColumnsTest, DeclinesWithEigenThreadedOrInsideARegion) {
    std::mt19937_64 rng(7);
    const Eigen::Index n = 64;
    const Eigen::MatrixXd C = randomCorrelation(rng, n);
    const Eigen::LDLT<Eigen::MatrixXd> f(C);
    const int saved = Eigen::nbThreads();
    Eigen::setNbThreads(2);
    bool split = true;
    const Eigen::MatrixXd threaded = ldltInverseColumns(f, n, 8, &split);
    Eigen::setNbThreads(saved);
    EXPECT_FALSE(split);
    EXPECT_EQ(threaded.rows(), n);
    const OneEigenThread pin;
    int n_split = 0;
    #pragma omp parallel for num_threads(2) reduction(+ : n_split)
    for (int k = 0; k < 2; ++k) {
        bool inner = true;
        const Eigen::MatrixXd inv = ldltInverseColumns(f, n, 8, &inner);
        if (inner) ++n_split;
    }
#ifdef _OPENMP
    EXPECT_EQ(n_split, 0) << "a nested region takes the whole solve";
#endif
}

/// An exact-update epoch over @p n carried keys of a @p np border, with a seam's off-diagonal noise.
CouplingExactEpoch randomEpoch(std::mt19937_64& rng, Eigen::Index n, Eigen::Index np) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    CouplingExactEpoch ep;
    Eigen::MatrixXd C = randomCorrelation(rng, np);
    Eigen::VectorXd sigma(np);
    for (Eigen::Index i = 0; i < np; ++i) sigma(i) = std::pow(10.0, -5.0 + 4.0 * U(rng));   // 1e-5 to 0.1 m
    ep.P = sigma.asDiagonal() * C * sigma.asDiagonal();
    for (Eigen::Index s = 0; s < n; ++s) {
        ep.p_of_s.push_back(static_cast<int>(s));
        ep.col_of_s.push_back(s);
    }
    ep.q_s = Eigen::VectorXd::Constant(n, 1e-8);
    ep.shat_s = Eigen::VectorXd::Zero(n);
    ep.q_off.emplace_back(0, 1, 1e-9);
    ep.q_off.emplace_back(1, 0, 1e-9);
    return ep;
}

TEST(CouplingExactFactorSplitTest, HoldsTheSameInverseWithTheSplit) {
    const OneEigenThread pin;
    std::mt19937_64 rng(31);
    for (const Eigen::Index n : {Eigen::Index(257), Eigen::Index(258)}) {
        const CouplingExactEpoch base = randomEpoch(rng, n, n + 11);
        CouplingExactEpoch serial = base, split = base;
        ASSERT_TRUE(couplingExactFactor(serial));
        EXPECT_FALSE(serial.inverse_split);
        for (const int threads : {3, 20}) {
            ASSERT_TRUE(couplingExactFactor(split, threads));
            EXPECT_TRUE(split.inverse_split) << "order " << n << ", " << threads << " threads";
            EXPECT_TRUE(bitEqual(serial.F, split.F)) << "order " << n << ", " << threads << " threads";
        }
    }
}

// ---- The carry harvest ----

/// The walk over the column map that networkHarvestCouplingRow replaced: the reference.
std::vector<CoupledShared> couplingRowByMap(const Eigen::VectorXd& row, const HarvestBorder& border,
                                            const Eigen::VectorXd& x_s) {
    std::vector<CoupledShared> cpl;
    const Eigen::Index ns = row.size();
    if (ns > 0) {
        const double maxabs = row.cwiseAbs().maxCoeff();
        const double thr = kCouplingSparsifyRelThresh * maxabs;
        for (Eigen::Index j = 0; j < ns; ++j) {
            const double mj = row(j);
            if (std::abs(mj) <= thr) continue;
            const auto dit = border.col2desc.find(j);
            if (dit == border.col2desc.end()) continue;
            const double pj = (j < border.cov_df.rows() && border.cov_df(j, j) > 0.0)
                                  ? border.cov_df(j, j) : 0.0;
            CoupledShared cs = dit->second;
            cs.coeff  = mj;
            cs.s_prev = x_s(j);
            cs.p_prev = pj;
            cpl.push_back(std::move(cs));
        }
    }
    return cpl;
}

void expectSameCoupling(const std::vector<CoupledShared>& a, const std::vector<CoupledShared>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        EXPECT_EQ(static_cast<int>(a[k].kind), static_cast<int>(b[k].kind));
        EXPECT_EQ(a[k].sat, b[k].sat);
        EXPECT_EQ(a[k].signal, b[k].signal);
        EXPECT_EQ(a[k].n, b[k].n);
        EXPECT_EQ(a[k].m, b[k].m);
        EXPECT_EQ(a[k].trig, b[k].trig);
        EXPECT_EQ(a[k].sh_layer, b[k].sh_layer);
        EXPECT_TRUE(bitEqualScalar(a[k].coeff, b[k].coeff));
        EXPECT_TRUE(bitEqualScalar(a[k].s_prev, b[k].s_prev));
        EXPECT_TRUE(bitEqualScalar(a[k].p_prev, b[k].p_prev));
    }
}

/// A border with described columns (every third column left out), a covariance with some pinned
/// (zero) diagonals, and the lookup the row walk reads.
HarvestBorder randomBorder(std::mt19937_64& rng, Eigen::Index ns) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    HarvestBorder border;
    border.cov_df = Eigen::MatrixXd::Zero(ns, ns);
    for (Eigen::Index j = 0; j < ns; ++j) {
        if (j % 7 != 0) border.cov_df(j, j) = U(rng);
        if (j % 3 == 2) continue;
        CoupledShared d;
        d.kind = (j % 3 == 0) ? SharedParamKind::Clock : SharedParamKind::PhaseBias;
        d.sat = "G" + std::to_string(j % 32 + 1);
        d.signal = (j % 3 == 0) ? std::string{} : "L1C";
        border.col2desc.emplace(j, std::move(d));
    }
    border.desc_of_col.assign(static_cast<std::size_t>(ns), nullptr);
    for (const auto& [col, desc] : border.col2desc) border.desc_of_col[static_cast<std::size_t>(col)] = &desc;
    return border;
}

TEST(HarvestAheadTest, CouplingRowKeepsTheMapWalksTerms) {
    std::mt19937_64 rng(41);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    const Eigen::Index ns = 421;
    const HarvestBorder border = randomBorder(rng, ns);
    Eigen::VectorXd x_s(ns);
    for (Eigen::Index j = 0; j < ns; ++j) x_s(j) = U(rng);
    for (int trial = 0; trial < 6; ++trial) {
        Eigen::VectorXd row(ns);
        for (Eigen::Index j = 0; j < ns; ++j) {
            // Entries on both sides of the sparsification threshold, and exact zeros.
            const double scale = (j % 5 == trial % 5) ? 1e-12 : ((j % 11 == 0) ? 0.0 : 1.0);
            row(j) = scale * U(rng);
        }
        expectSameCoupling(networkHarvestCouplingRow(row, border, x_s), couplingRowByMap(row, border, x_s));
    }
    EXPECT_TRUE(networkHarvestCouplingRow(Eigen::VectorXd(), border, x_s).empty());
    Eigen::VectorXd short_row = Eigen::VectorXd::Constant(ns + 5, 1.0);   // past the described columns
    Eigen::VectorXd long_x = Eigen::VectorXd::Zero(ns + 5);
    expectSameCoupling(networkHarvestCouplingRow(short_row, border, long_x), couplingRowByMap(short_row, border, long_x));
}

/// A kernel with @p n_sta stations of @p nr locals on a @p ns border.
NetworkNormalEquations solvedKernel(std::mt19937_64& rng, int ns, int nr, int n_sta) {
    std::uniform_real_distribution<double> U(-1.0, 1.0), W(1e-2, 1e2);
    NetworkNormalEquations neq(ns);
    for (int s = 0; s < n_sta; ++s) {
        const int m = 2 * nr + 40;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, ns), A_r = Eigen::MatrixXd::Zero(m, nr);
        Eigen::VectorXd w(m), y(m);
        for (int k = 0; k < m; ++k) {
            for (int t = 0; t < 6; ++t) A_s(k, static_cast<Eigen::Index>(rng() % ns)) = U(rng);
            for (int t = 0; t < 5; ++t) A_r(k, static_cast<Eigen::Index>(rng() % nr)) = U(rng);
            w(k) = W(rng);
            y(k) = U(rng);
        }
        A_r.diagonal().array() += 3.0;
        neq.addStationObservations(A_s, A_r, w, y);
    }
    return neq;
}

TEST(HarvestAheadTest, ColumnsAheadMatchTheSerialRegressionInAParallelRegion) {
    const OneEigenThread pin;
    std::mt19937_64 rng(53);
    const int ns = 120, nr = 40, n_sta = 12;
    const NetworkNormalEquations kernel = solvedKernel(rng, ns, nr, n_sta);
    const HarvestBorder border = randomBorder(rng, ns);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    Eigen::VectorXd x_s(ns);
    for (int j = 0; j < ns; ++j) x_s(j) = U(rng);

    // Every other column is carried; odd stations have a block (cross terms), even ones none.
    std::vector<StationHarvestBlock> blocks(static_cast<std::size_t>(n_sta));
    std::vector<std::vector<HarvestColumnAhead>> slots(static_cast<std::size_t>(n_sta));
    for (int s = 0; s < n_sta; ++s) {
        StationHarvestBlock& sb = blocks[static_cast<std::size_t>(s)];
        sb.ok = true;
        for (Eigen::Index c = 0; c < nr; c += 2) sb.cols.push_back(c);
        if (s % 2 == 1) sb.blk = std::make_shared<PlHarvestBlock>();
        slots[static_cast<std::size_t>(s)].resize(static_cast<std::size_t>(nr));
    }
    #pragma omp parallel for schedule(dynamic) num_threads(4)
    for (long long s = 0; s < n_sta; ++s) {
        const std::size_t ss = static_cast<std::size_t>(s);
        networkHarvestStationColumnsAhead(kernel, border, x_s, ss, blocks[ss], slots[ss]);
    }
    for (int s = 0; s < n_sta; ++s) {
        const std::size_t ss = static_cast<std::size_t>(s);
        const StationHarvestBlock& sb = blocks[ss];
        const std::vector<Eigen::Index>* cross_cols = sb.blk ? &sb.cols : nullptr;
        for (Eigen::Index c = 0; c < nr; ++c) {
            const HarvestColumnAhead& a = slots[ss][static_cast<std::size_t>(c)];
            if (c % 2 == 1) {
                EXPECT_FALSE(a.ready) << "a column outside the block is left to the walk";
                continue;
            }
            ASSERT_TRUE(a.ready);
            EXPECT_EQ(a.with_cross, cross_cols != nullptr);
            const auto reg = kernel.stationRegression(ss, c, /*carry_aware=*/false, cross_cols);
            EXPECT_TRUE(bitEqualScalar(a.cond_var, reg.cond_var));
            EXPECT_TRUE(bitEqual(a.cross, reg.cross));
            expectSameCoupling(a.coupling, networkHarvestCouplingRow(reg.row, border, x_s));
        }
    }
}

TEST(HarvestAheadTest, AColumnTheKernelRefusesStaysForTheWalk) {
    const OneEigenThread pin;
    std::mt19937_64 rng(59);
    const int ns = 40, nr = 12;
    const NetworkNormalEquations kernel = solvedKernel(rng, ns, nr, 1);
    const HarvestBorder border = randomBorder(rng, ns);
    const Eigen::VectorXd x_s = Eigen::VectorXd::Zero(ns);
    StationHarvestBlock sb;
    sb.ok = true;
    sb.cols = {0, 1, 2};
    std::vector<HarvestColumnAhead> slots(static_cast<std::size_t>(nr));
    // An estimator slot the kernel does not have: every column stays unset, nothing escapes.
    networkHarvestStationColumnsAhead(kernel, border, x_s, 5, sb, slots);
    for (const HarvestColumnAhead& a : slots) EXPECT_FALSE(a.ready);
}

}  // namespace
}  // namespace GnssProcessor
