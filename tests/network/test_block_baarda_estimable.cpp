/**
 * @file test_block_baarda_estimable.cpp
 * @brief Tests for the estimable form of the (station, satellite) LOS block test
 *        (network.block_baarda_estimable).
 *
 * The block test inverts the b×b residual cofactor of one line of sight.  On the float
 * vehicle that cofactor is rank-deficient by construction: the line of sight's own slant
 * ionosphere is free, nothing but this line of sight's rows determines it, and it absorbs
 * one direction of the block exactly.  The whole-block test then refuses every block.  The
 * estimable form tests the part of the block the data can test: it whitens the cofactor,
 * keeps the eigen-directions whose redundancy number is at least eps and reads the
 * quadratic form on those with their count as the degrees of freedom.
 *
 * The properties that fail silently when wrong:
 *
 *   1. With a free slant ionosphere the whitened cofactor has exactly one absorbed
 *      direction, the whole-block test refuses the block, and the estimable form keeps the
 *      other three.
 *   2. With a stiff ionosphere prior nothing is absorbed, and the estimable form is the
 *      whole-block statistic.
 *   3. Under the null the statistic is chi-square with the kept degrees of freedom (the
 *      residual is drawn together with the prior noise, so its covariance is the
 *      cofactor itself and the mean is exact).
 *   4. A slip is loud; a fault along the absorbed direction is invisible to every test, and
 *      the test says so rather than pretending otherwise.
 *   5. The accumulator scales a block by its kept degrees of freedom, not its row count.
 *   6. The sweep, driven on a real kernel, turns the refused blocks into tested ones.
 *   7. The knob loads, requires block_baarda, clamps its floor, and is off by default.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/qc/BlockBaarda.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/state/RunContext.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {
namespace {

constexpr double kCrit3 = 16.2662;   ///< chi-square, 3 dof, alpha = 0.001
constexpr double kCrit4 = 18.4668;   ///< chi-square, 4 dof, alpha = 0.001

// ===========================================================================
//  The toy: kR stations x kS satellites, four rows per line of sight
//  (C1, C2, L1, L2).  Shared: the satellite clocks.  Local: the receiver clock,
//  one slant ionosphere per satellite, two ambiguities per satellite.  The
//  ambiguity prior stands in for the leveling carry; the ionosphere prior is
//  optional, because the float vehicle has none.
// ===========================================================================

constexpr int kR = 3;
constexpr int kS = 4;
constexpr Eigen::Index kRowsPerSta = 4 * kS;
constexpr Eigen::Index kLocal = 1 + 3 * kS;
constexpr Eigen::Index kNTot = kS + kR * kLocal;

constexpr double kMu2 = (154.0 / 120.0) * (154.0 / 120.0);
constexpr double kLam1 = 0.190293672798365;
constexpr double kLam2 = 0.244210213424568;
constexpr double kSigCode = 0.3, kSigPhase = 0.003;
constexpr double kSigIonoStiff = 0.002, kSigAmb = 0.005;   ///< a prior stiff enough that nothing is absorbed
constexpr double kPin = 1e14;

enum Kind { kC1 = 0, kC2 = 1, kL1 = 2, kL2 = 3 };

Eigen::Index ionoCol(int k) { return 1 + k; }
Eigen::Index ambCol(int k, int band) { return 1 + kS + 2 * k + band; }
Eigen::Index rowOf(int k, int kind) { return 4 * k + kind; }
Eigen::Index globalRow(int r, int k, int kind) { return r * kRowsPerSta + rowOf(k, kind); }

double sigmaOf(int r, int k, int kind) {
    const double scale = 1.0 + 0.35 * static_cast<double>((r + 2 * k) % 4);
    const double base = (kind == kC1 || kind == kC2) ? kSigCode : kSigPhase;
    const double band = (kind == kC2 || kind == kL2) ? 1.1 : 1.0;
    return base * scale * band;
}

double muOf(int kind) {
    switch (kind) {
        case kC1: return 1.0;
        case kC2: return kMu2;
        case kL1: return -1.0;
        default:  return -kMu2;
    }
}

struct EstNet {
    Eigen::MatrixXd A_s[kR], A_r[kR];
    Eigen::VectorXd w[kR];
    std::size_t slot[kR]{};
    Eigen::MatrixXd cov_free;       ///< datum-frame shared covariance, pinned column zeroed
    // The dense oracle (pinned column dropped outright).
    Eigen::MatrixXd A;              ///< rows x kept parameters
    Eigen::VectorXd wd;             ///< per-row information weight
    Eigen::VectorXd prior;          ///< prior information per kept parameter (0 = none)
    Eigen::MatrixXd cov;            ///< (A'WA + Prior)^-1
    Eigen::Index pin_col{0};
    std::vector<Eigen::Index> keep;

    void block(int r, int k, Eigen::MatrixXd& Bs, Eigen::MatrixXd& Br,
               Eigen::VectorXd& bw) const {
        Bs.resize(4, kS);
        Br.resize(4, kLocal);
        bw.resize(4);
        for (int kind = 0; kind < 4; ++kind) {
            Bs.row(kind) = A_s[r].row(rowOf(k, kind));
            Br.row(kind) = A_r[r].row(rowOf(k, kind));
            bw(kind) = w[r](rowOf(k, kind));
        }
    }
    std::vector<Eigen::Index> denseRows(int r, int k) const {
        return {globalRow(r, k, kC1), globalRow(r, k, kC2),
                globalRow(r, k, kL1), globalRow(r, k, kL2)};
    }
    /// Data residual of an observation vector @p y and a prior observation vector
    /// @p p (one entry per kept parameter, ignored where the prior is zero).
    Eigen::VectorXd residual(const Eigen::VectorXd& y, const Eigen::VectorXd& p) const {
        const Eigen::VectorXd rhs = A.transpose() * (wd.asDiagonal() * y)
                                  + prior.cwiseProduct(p);
        return y - A * (cov * rhs);
    }
    Eigen::VectorXd block4(const Eigen::VectorXd& e, int r, int k) const {
        const std::vector<Eigen::Index> rows = denseRows(r, k);
        Eigen::VectorXd out(4);
        for (int i = 0; i < 4; ++i) out(i) = e(rows[static_cast<std::size_t>(i)]);
        return out;
    }
};

/// Build the toy into @p neq (constructed with kS shared columns).  @p sig_iono_prior
/// <= 0 leaves the slant ionosphere free.
EstNet buildEstNet(NetworkNormalEquations& neq, double sig_iono_prior) {
    EstNet net;
    net.pin_col = 0;
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(kR * kRowsPerSta, kNTot);
    Eigen::VectorXd w(kR * kRowsPerSta);
    Eigen::VectorXd prior = Eigen::VectorXd::Zero(kNTot);

    for (int r = 0; r < kR; ++r) {
        net.A_s[r] = Eigen::MatrixXd::Zero(kRowsPerSta, kS);
        net.A_r[r] = Eigen::MatrixXd::Zero(kRowsPerSta, kLocal);
        net.w[r].resize(kRowsPerSta);
        for (int k = 0; k < kS; ++k) {
            for (int kind = 0; kind < 4; ++kind) {
                const Eigen::Index row = rowOf(k, kind);
                net.A_s[r](row, k) = -1.0;
                net.A_r[r](row, 0) = 1.0;
                net.A_r[r](row, ionoCol(k)) = muOf(kind);
                if (kind == kL1) net.A_r[r](row, ambCol(k, 0)) = kLam1;
                if (kind == kL2) net.A_r[r](row, ambCol(k, 1)) = kLam2;
                const double s = sigmaOf(r, k, kind);
                net.w[r](row) = 1.0 / (s * s);
            }
        }
        std::vector<Eigen::Index> cols;
        std::vector<double> info;
        for (int k = 0; k < kS; ++k) {
            if (sig_iono_prior > 0.0) {
                cols.push_back(ionoCol(k));
                info.push_back(1.0 / (sig_iono_prior * sig_iono_prior));
            }
            for (int band = 0; band < 2; ++band) {
                cols.push_back(ambCol(k, band));
                info.push_back(1.0 / (kSigAmb * kSigAmb));
            }
        }
        const Eigen::VectorXd y = Eigen::VectorXd::Zero(kRowsPerSta);
        net.slot[r] = neq.addStationObservations(net.A_s[r], net.A_r[r], net.w[r], y);
        Eigen::VectorXd dN = Eigen::Map<Eigen::VectorXd>(
            info.data(), static_cast<Eigen::Index>(info.size()));
        neq.addStationPrior(net.slot[r], cols, dN, Eigen::VectorXd::Zero(dN.size()));

        const Eigen::Index base = kS + r * kLocal;
        A.block(r * kRowsPerSta, 0, kRowsPerSta, kS) = net.A_s[r];
        A.block(r * kRowsPerSta, base, kRowsPerSta, kLocal) = net.A_r[r];
        w.segment(r * kRowsPerSta, kRowsPerSta) = net.w[r];
        for (std::size_t i = 0; i < cols.size(); ++i) prior(base + cols[i]) = info[i];
    }

    Eigen::MatrixXd pin_info = Eigen::MatrixXd::Zero(kS, kS);
    pin_info(net.pin_col, net.pin_col) = kPin;
    neq.addSharedInfo(pin_info, Eigen::VectorXd::Zero(kS));
    neq.reduce();
    net.cov_free = neq.reducedSharedNormals().inverse();
    net.cov_free.row(net.pin_col).setZero();
    net.cov_free.col(net.pin_col).setZero();

    for (Eigen::Index c = 0; c < kNTot; ++c)
        if (c != net.pin_col) net.keep.push_back(c);
    net.A.resize(A.rows(), static_cast<Eigen::Index>(net.keep.size()));
    net.prior.resize(static_cast<Eigen::Index>(net.keep.size()));
    for (std::size_t c = 0; c < net.keep.size(); ++c) {
        net.A.col(static_cast<Eigen::Index>(c)) = A.col(net.keep[c]);
        net.prior(static_cast<Eigen::Index>(c)) = prior(net.keep[c]);
    }
    net.wd = w;
    Eigen::MatrixXd N = net.A.transpose() * net.wd.asDiagonal() * net.A;
    N.diagonal() += net.prior;
    net.cov = N.inverse();
    return net;
}

/// The whitened cofactor's eigenvalues of one block, ascending.
Eigen::VectorXd whitenedSpectrum(const Eigen::MatrixXd& Q, const Eigen::VectorXd& bw) {
    const Eigen::VectorXd s = bw.cwiseSqrt();
    const Eigen::MatrixXd Qw = s.asDiagonal() * Q * s.asDiagonal();
    return Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd>(Qw).eigenvalues();
}

struct Rng {
    std::mt19937_64 g{20261017u};
    std::normal_distribution<double> nd{0.0, 1.0};
    double normal() { return nd(g); }
};

/// One draw of the observation noise and the prior noise, in the dense oracle's order.
void drawNoise(const EstNet& net, Rng& rng, Eigen::VectorXd& y, Eigen::VectorXd& p) {
    y.resize(net.wd.size());
    for (Eigen::Index i = 0; i < y.size(); ++i) y(i) = rng.normal() / std::sqrt(net.wd(i));
    p = Eigen::VectorXd::Zero(net.prior.size());
    for (Eigen::Index c = 0; c < p.size(); ++c)
        if (net.prior(c) > 0.0) p(c) = rng.normal() / std::sqrt(net.prior(c));
}

}  // namespace

// ===========================================================================
//  1. The absorbed direction
// ===========================================================================

TEST(BlockBaardaEstimableTest, FreeIonoAbsorbsOneDirectionAndTheWholeBlockTestRefusesIt) {
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, /*sig_iono_prior=*/0.0);
    for (int r = 0; r < kR; ++r) {
        for (int k = 0; k < kS; ++k) {
            Eigen::MatrixXd Bs, Br;
            Eigen::VectorXd bw;
            net.block(r, k, Bs, Br, bw);
            const Eigen::MatrixXd Q = neq.blockResidualCofactor(net.slot[r], Bs, Br, bw, net.cov_free);
            const Eigen::VectorXd lam = whitenedSpectrum(Q, bw);
            ASSERT_EQ(lam.size(), 4);
            EXPECT_LT(std::abs(lam(0)), 1e-8) << "r=" << r << " k=" << k
                << ": the slant ionosphere must absorb one direction exactly";
            EXPECT_GT(lam(1), 0.05) << "r=" << r << " k=" << k
                << ": the other three directions must stay testable";

            bool sing = false, nonfin = false;
            EXPECT_FALSE(blockCofactorIsCheckable(Q, kBlockCofactorRcondEps, &sing, &nonfin));
            EXPECT_TRUE(sing);

            const BlockEstimableForm f = blockEstimableForm(
                Q, bw, Eigen::VectorXd::Zero(4), /*eps=*/0.05);
            EXPECT_FALSE(f.nonfinite);
            EXPECT_EQ(f.dof, 3);
        }
    }
}

TEST(BlockBaardaEstimableTest, StiffIonoPriorKeepsEveryDirectionAndMatchesTheWholeBlockStatistic) {
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, kSigIonoStiff);
    for (int r = 0; r < kR; ++r)
        for (int k = 0; k < kS; ++k) {
            Eigen::MatrixXd Bs, Br;
            Eigen::VectorXd bw;
            net.block(r, k, Bs, Br, bw);
            const Eigen::MatrixXd Q = neq.blockResidualCofactor(net.slot[r], Bs, Br, bw, net.cov_free);
            EXPECT_GT(whitenedSpectrum(Q, bw)(0), 0.1) << "r=" << r << " k=" << k
                << ": with a stiff ionosphere prior no direction is absorbed";
        }
    Rng rng;
    for (int draw = 0; draw < 20; ++draw) {
        Eigen::VectorXd y, p;
        drawNoise(net, rng, y, p);
        const Eigen::VectorXd e = net.residual(y, p);
        const int r = draw % kR, k = draw % kS;
        Eigen::MatrixXd Bs, Br;
        Eigen::VectorXd bw;
        net.block(r, k, Bs, Br, bw);
        const Eigen::MatrixXd Q = neq.blockResidualCofactor(net.slot[r], Bs, Br, bw, net.cov_free);
        const Eigen::VectorXd eb = net.block4(e, r, k);

        bool sing = false, nonfin = false;
        ASSERT_TRUE(blockCofactorIsCheckable(Q, kBlockCofactorRcondEps, &sing, &nonfin));
        const double whole = eb.dot(Q.ldlt().solve(eb));
        const BlockEstimableForm f = blockEstimableForm(Q, bw, eb, 0.05);
        ASSERT_EQ(f.dof, 4);
        EXPECT_NEAR(f.quad, whole, 1e-8 * std::max(1.0, std::abs(whole)));
        double row_max = 0.0;
        for (int i = 0; i < 4; ++i) row_max = std::max(row_max, eb(i) * eb(i) / Q(i, i));
        EXPECT_NEAR(f.max_row_T, row_max, 1e-8 * std::max(1.0, row_max));
        EXPECT_EQ(f.rows_testable, 4);
    }
}

// ===========================================================================
//  2. The null distribution
// ===========================================================================

TEST(BlockBaardaEstimableTest, NullStatisticIsChiSquareWithTheKeptDof) {
    struct Case { double sig_iono; int dof; double crit; };
    for (const Case c : {Case{0.0, 3, kCrit3}, Case{kSigIonoStiff, 4, kCrit4}}) {
        NetworkNormalEquations neq(kS);
        const EstNet net = buildEstNet(neq, c.sig_iono);
        Rng rng;
        // The cofactor of every block, once.
        std::vector<Eigen::MatrixXd> Qs;
        std::vector<Eigen::VectorXd> ws;
        for (int r = 0; r < kR; ++r)
            for (int k = 0; k < kS; ++k) {
                Eigen::MatrixXd Bs, Br;
                Eigen::VectorXd bw;
                net.block(r, k, Bs, Br, bw);
                Qs.push_back(neq.blockResidualCofactor(net.slot[r], Bs, Br, bw, net.cov_free));
                ws.push_back(bw);
            }
        const int n_draws = 1500;
        double sum = 0.0, sum2 = 0.0;
        long long n = 0, over = 0;
        for (int draw = 0; draw < n_draws; ++draw) {
            Eigen::VectorXd y, p;
            drawNoise(net, rng, y, p);
            const Eigen::VectorXd e = net.residual(y, p);
            for (int r = 0; r < kR; ++r)
                for (int k = 0; k < kS; ++k) {
                    const std::size_t i = static_cast<std::size_t>(r * kS + k);
                    const BlockEstimableForm f =
                        blockEstimableForm(Qs[i], ws[i], net.block4(e, r, k), 0.05);
                    ASSERT_EQ(f.dof, c.dof);
                    sum += f.quad; sum2 += f.quad * f.quad; ++n;
                    if (f.quad > c.crit) ++over;
                }
        }
        const double mean = sum / static_cast<double>(n);
        const double var = sum2 / static_cast<double>(n) - mean * mean;
        // chi-square: mean = dof, variance = 2 dof.  18000 draws: the standard error
        // of the mean is 0.02 (dof 3), so 0.08 is four standard errors.
        EXPECT_NEAR(mean, c.dof, 0.08) << "dof " << c.dof;
        EXPECT_NEAR(var, 2.0 * c.dof, 0.6) << "dof " << c.dof;
        // alpha = 0.001: about 18 exceedances in 18000.
        EXPECT_LT(static_cast<double>(over) / static_cast<double>(n), 0.003) << "dof " << c.dof;
    }
}

// ===========================================================================
//  3. What it sees and what it cannot
// ===========================================================================

TEST(BlockBaardaEstimableTest, SlipIsLoudAndTheAbsorbedDirectionIsInvisible) {
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, /*sig_iono_prior=*/0.0);
    constexpr int kSta = 0, kSat = 3;
    Eigen::MatrixXd Bs, Br;
    Eigen::VectorXd bw;
    net.block(kSta, kSat, Bs, Br, bw);
    const Eigen::MatrixXd Q = neq.blockResidualCofactor(net.slot[kSta], Bs, Br, bw, net.cov_free);
    const Eigen::VectorXd no_prior_noise = Eigen::VectorXd::Zero(net.prior.size());

    auto quadOf = [&](const Eigen::VectorXd& y) {
        const Eigen::VectorXd e = net.residual(y, no_prior_noise);
        return blockEstimableForm(Q, bw, net.block4(e, kSta, kSat), 0.05);
    };
    const std::vector<Eigen::Index> rows = net.denseRows(kSta, kSat);
    const Eigen::Index n = net.A.rows();

    // A half-cycle slip of opposite sign on the two bands.  A slip of the same sign on both
    // bands looks like a change of the slant ionosphere (the phase rows move in the
    // iono pattern, 1 : 1.65), which the free column absorbs, so it is the opposite sign
    // that the free ionosphere cannot take.
    Eigen::VectorXd slip = Eigen::VectorXd::Zero(n);
    slip(rows[kL1]) += 0.5 * kLam1;
    slip(rows[kL2]) -= 0.5 * kLam2;
    const BlockEstimableForm fs = quadOf(slip);
    EXPECT_EQ(fs.dof, 3);
    EXPECT_GT(fs.quad, 5.0 * kCrit3) << "an opposite-sign half-cycle slip must be loud";

    // Six metres on one code row (the code noise of the toy is 0.3 to 0.6 m).
    Eigen::VectorXd code = Eigen::VectorXd::Zero(n);
    code(rows[kC1]) += 6.0;
    EXPECT_GT(quadOf(code).quad, kCrit3);

    // A same-sign half-cycle slip is mostly the ionosphere: weak, and the test does not
    // pretend otherwise.
    Eigen::VectorXd same = Eigen::VectorXd::Zero(n);
    same(rows[kL1]) += 0.5 * kLam1;
    same(rows[kL2]) += 0.5 * kLam2;
    EXPECT_LT(quadOf(same).quad, 0.5 * fs.quad);

    // A fault along the ionosphere's own direction is the slant ionosphere: absorbed,
    // however large.  No test of this block can see it, and none claims to.
    Eigen::VectorXd iono = Eigen::VectorXd::Zero(n);
    for (int kind = 0; kind < 4; ++kind) iono(rows[static_cast<std::size_t>(kind)]) += 25.0 * muOf(kind);
    EXPECT_LT(quadOf(iono).quad, 1e-6);
}

TEST(BlockBaardaEstimableTest, FloorDecidesWhichDirectionsAreKept) {
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, 0.0);
    Eigen::MatrixXd Bs, Br;
    Eigen::VectorXd bw;
    net.block(1, 2, Bs, Br, bw);
    const Eigen::MatrixXd Q = neq.blockResidualCofactor(net.slot[1], Bs, Br, bw, net.cov_free);
    const Eigen::VectorXd lam = whitenedSpectrum(Q, bw);
    const Eigen::VectorXd e = Eigen::VectorXd::Constant(4, 1e-3);

    // A floor just above each eigenvalue drops it and everything below.
    for (int drop = 0; drop <= 3; ++drop) {
        const double eps = (drop < 3) ? 0.5 * (lam(drop) + lam(drop + 1)) : 2.0;
        if (!(eps > 0.0) || eps >= 1.0) continue;
        const BlockEstimableForm f = blockEstimableForm(Q, bw, e, eps);
        int expect = 0;
        for (Eigen::Index i = 0; i < 4; ++i) if (lam(i) >= eps) ++expect;
        EXPECT_EQ(f.dof, expect) << "eps " << eps;
    }
    // A floor above every eigenvalue leaves nothing to test: the block is refused.
    const BlockEstimableForm none = blockEstimableForm(Q, bw, e, 1.5);
    EXPECT_EQ(none.dof, 0);
    EXPECT_FALSE(none.nonfinite);
}

TEST(BlockBaardaEstimableTest, RowTierReadsOnlyTheRowsItCanTest) {
    // Row 0 is fully absorbed (Q_00 ~ 0) with a residual that would read as a huge
    // standardized value if it were divided by Q_00: the row tier cannot test it, so it
    // does not enter the row statistic.
    Eigen::MatrixXd Q(3, 3);
    Q << 1e-14, 0.0, 0.0,
         0.0,   0.5,  0.0,
         0.0,   0.0,  0.25;
    Eigen::VectorXd w(3);
    w << 1.0, 2.0, 4.0;
    Eigen::VectorXd e(3);
    e << 1e-6, 0.1, 0.05;
    const BlockEstimableForm f = blockEstimableForm(Q, w, e, 0.05);
    EXPECT_EQ(f.dof, 2);
    EXPECT_EQ(f.rows_testable, 2);
    EXPECT_NEAR(f.max_row_T, std::max(0.1 * 0.1 / 0.5, 0.05 * 0.05 / 0.25), 1e-12);
    // quad = sum e^2 / Q over the kept (diagonal) directions.
    EXPECT_NEAR(f.quad, 0.1 * 0.1 / 0.5 + 0.05 * 0.05 / 0.25, 1e-12);
}

TEST(BlockBaardaEstimableTest, MalformedInputIsNonFiniteNotATest) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(2, 2);
    Eigen::VectorXd w = Eigen::VectorXd::Ones(2), e = Eigen::VectorXd::Ones(2);
    EXPECT_FALSE(blockEstimableForm(Q, w, e, 0.05).nonfinite);

    Eigen::MatrixXd Qn = Q;
    Qn(0, 0) = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(blockEstimableForm(Qn, w, e, 0.05).nonfinite);
    Eigen::VectorXd en = e;
    en(1) = std::nan("");
    EXPECT_TRUE(blockEstimableForm(Q, w, en, 0.05).nonfinite);
    Eigen::VectorXd w0 = w;
    w0(0) = 0.0;
    EXPECT_TRUE(blockEstimableForm(Q, w0, e, 0.05).nonfinite);
    EXPECT_TRUE(blockEstimableForm(Q, Eigen::VectorXd::Ones(3), e, 0.05).nonfinite);
    EXPECT_TRUE(blockEstimableForm(Eigen::MatrixXd(), Eigen::VectorXd(), Eigen::VectorXd(), 0.05).nonfinite);
}

// ===========================================================================
//  4. The accumulator
// ===========================================================================

namespace {

NetworkBlockBaarda::Input drawBlock(std::mt19937_64& g, int b, int dof, double sigma0) {
    std::normal_distribution<double> nd(0.0, 1.0);
    double chi2 = 0.0;
    for (int i = 0; i < dof; ++i) { const double z = nd(g); chi2 += z * z; }
    NetworkBlockBaarda::Input in;
    in.station = 0;
    in.sat = "G01";
    in.b = b;
    in.dof = dof;
    in.T = sigma0 * sigma0 * chi2 / static_cast<double>(dof);
    in.n_code = 2;
    in.n_phase = b - 2;
    return in;
}

}  // namespace

TEST(BlockBaardaEstimableTest, ScaleUsesTheKeptDofNotTheRowCount) {
    constexpr double kSigma0 = 1.7;
    auto run = [&](bool tell_dof) {
        std::mt19937_64 g(7);
        NetworkBlockBaarda acc({"G"}, 0.0, 50.0);
        for (int e = 0; e < 20; ++e) {
            acc.beginEpoch(30.0 * e, 30.0 * e);
            for (int i = 0; i < 300; ++i) {
                NetworkBlockBaarda::Input in = drawBlock(g, /*b=*/4, /*dof=*/3, kSigma0);
                if (!tell_dof) in.dof = 0;
                acc.addBlock(in);
            }
        }
        acc.beginEpoch(1000.0, 1000.0);
        return acc.sigma0(0);
    };
    EXPECT_NEAR(run(true), kSigma0, 0.08) << "with the kept dof the scale is recovered";
    // Without it the same population reads as b = 4 and the scale is biased low by the
    // ratio of the two geometric means, which is the reason the dof is carried.
    EXPECT_LT(run(false), 0.95 * kSigma0);
}

TEST(BlockBaardaEstimableTest, DofZeroInputBehavesAsTheWholeBlock) {
    // The off path: an Input that does not set dof is the b-row block it always was.
    NetworkBlockBaarda a({"G"}, 60.0, 0.0), c({"G"}, 60.0, 0.0);
    a.beginEpoch(0.0, 0.0);
    c.beginEpoch(0.0, 0.0);
    NetworkBlockBaarda::Input in;
    in.station = 2; in.sat = "G05"; in.b = 4; in.T = 3.5; in.max_row_T = 1.0; in.n_code = 2; in.n_phase = 2;
    NetworkBlockBaarda::Input in_dof = in;
    in_dof.dof = 4;   // an explicit dof equal to b is the same block
    const auto sa = a.addBlock(in);
    const auto sc = c.addBlock(in_dof);
    EXPECT_DOUBLE_EQ(sa.t, sc.t);
    EXPECT_EQ(sa.reported, sc.reported);
    EXPECT_DOUBLE_EQ(a.runLadder(0, PriorResidualLabel::Clean).geoT(),
                     c.runLadder(0, PriorResidualLabel::Clean).geoT());
}

TEST(BlockBaardaEstimableTest, RankHistogramAndSpectrumAreKeptPerBlockSize) {
    NetworkBlockBaarda acc({"G"}, 60.0, 0.0, 4.0, false, /*estimable_eps=*/0.05);
    EXPECT_TRUE(acc.estimable());
    EXPECT_DOUBLE_EQ(acc.estimableEps(), 0.05);
    acc.beginEpoch(0.0, 0.0);

    NetworkBlockBaarda::Input in;
    in.station = 0; in.sat = "G01"; in.b = 4; in.dof = 3; in.T = 1.0; in.n_code = 2; in.n_phase = 2;
    in.n_lambda = 4;
    in.lambda = {0.0, 0.3, 0.6, 0.9};
    acc.addBlock(in);
    in.lambda = {0.0, 0.5, 0.8, 1.0};
    acc.addBlock(in);
    NetworkBlockBaarda::Input none = in;
    none.dof = 0; none.uncheckable = true;
    none.lambda = {0.0, 0.0, 0.0, 0.0};
    acc.addBlock(none);

    EXPECT_EQ(acc.rankHistogram(4, 3), 2);
    EXPECT_EQ(acc.rankHistogram(4, 0), 1);
    EXPECT_EQ(acc.rankHistogram(4, 4), 0);
    EXPECT_NEAR(acc.spectrumMean(4, 1), (0.3 + 0.5 + 0.0) / 3.0, 1e-12);
    EXPECT_NEAR(acc.spectrumMean(4, 3), (0.9 + 1.0 + 0.0) / 3.0, 1e-12);
    // A block size that was never seen has no spectrum.
    EXPECT_EQ(acc.spectrumCount(5), 0);

    // An unarmed accumulator keeps no rank statistics.
    NetworkBlockBaarda off({"G"}, 60.0, 0.0);
    EXPECT_FALSE(off.estimable());
    off.beginEpoch(0.0, 0.0);
    off.addBlock(in);
    EXPECT_EQ(off.rankHistogram(4, 3), 0);
    EXPECT_EQ(off.spectrumCount(4), 0);
}

// ===========================================================================
//  5. The sweep on a real kernel
// ===========================================================================

namespace {

/// The rows of one station as the census hands them to the sweep.
struct SweepInput {
    StationAssembly R;
    std::vector<NetRowClass> cls;
    std::vector<std::string> sat;
};

SweepInput sweepInput(const EstNet& net, int r) {
    SweepInput s;
    s.R.A_s = net.A_s[r];
    s.R.A_r = net.A_r[r];
    s.R.w = net.w[r];
    s.R.y = Eigen::VectorXd::Zero(kRowsPerSta);
    for (int k = 0; k < kS; ++k)
        for (int kind = 0; kind < 4; ++kind) {
            s.cls.push_back((kind == kC1 || kind == kC2) ? NetRowClass::ObsCode
                                                         : NetRowClass::ObsPhase);
            s.sat.push_back(std::string("G0") + static_cast<char>('1' + k));
        }
    return s;
}

}  // namespace

TEST(BlockBaardaEstimableTest, SweepTurnsRefusedBlocksIntoTestedOnes) {
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, /*sig_iono_prior=*/0.0);
    Rng rng;
    Eigen::VectorXd y, p;
    drawNoise(net, rng, y, p);
    const Eigen::VectorXd e = net.residual(y, p);
    const SweepInput in = sweepInput(net, 0);
    const Eigen::VectorXd resid = e.segment(0, kRowsPerSta);

    for (const bool estimable : {false, true}) {
        NetworkBlockBaarda acc({"G"}, 60.0, 0.0, 4.0, false, estimable ? 0.05 : 0.0);
        acc.beginEpoch(0.0, 0.0);
        NetworkBlockBaardaTallies tallies;
        std::vector<BlockBaardaHit> hits;
        networkBlockBaardaSweepStation(acc, neq, net.slot[0], 0, in.R, in.cls, in.sat, resid,
                                       net.cov_free, std::set<std::string>{}, false, tallies, hits);
        EXPECT_EQ(tallies.blocks, kS);
        EXPECT_EQ(tallies.rows, 4 * kS);
        if (!estimable) {
            EXPECT_EQ(tallies.uncheckable, kS) << "the whole-block test refuses every block";
            EXPECT_EQ(tallies.singular, kS);
        } else {
            EXPECT_EQ(tallies.uncheckable, 0);
            EXPECT_EQ(acc.rankHistogram(4, 3), kS);
            EXPECT_EQ(acc.rankHistogram(4, 0), 0);
            EXPECT_EQ(acc.runLadder(0, PriorResidualLabel::Clean).n_blocks, kS);
            EXPECT_GT(acc.runLadder(0, PriorResidualLabel::Clean).max_T, 0.0);
        }
    }
}

TEST(BlockBaardaEstimableTest, SweepFlagsASlipAndNamesItsDof) {   // opposite-sign half-cycle slip
    NetworkNormalEquations neq(kS);
    const EstNet net = buildEstNet(neq, 0.0);
    constexpr int kSta = 1, kSat = 2;
    Eigen::VectorXd y = Eigen::VectorXd::Zero(net.A.rows());
    y(globalRow(kSta, kSat, kL1)) += 0.5 * kLam1;
    y(globalRow(kSta, kSat, kL2)) -= 0.5 * kLam2;
    const Eigen::VectorXd e = net.residual(y, Eigen::VectorXd::Zero(net.prior.size()));
    const SweepInput in = sweepInput(net, kSta);
    const Eigen::VectorXd resid = e.segment(kSta * kRowsPerSta, kRowsPerSta);

    // sigma0 = 1 (the family has not warmed up), so t_B is T_B and the report threshold
    // of 4 is four times the variance the model assigns.
    NetworkBlockBaarda acc({"G"}, 60.0, 0.0, 4.0, false, 0.05);
    acc.beginEpoch(0.0, 0.0);
    NetworkBlockBaardaTallies tallies;
    std::vector<BlockBaardaHit> hits;
    networkBlockBaardaSweepStation(acc, neq, net.slot[kSta], kSta, in.R, in.cls, in.sat, resid,
                                   net.cov_free, std::set<std::string>{}, false, tallies, hits);
    // The slip is on G03.  A slip also moves the receiver clock a little, so a sibling
    // line of sight may read a little loud too; the faulted block must be the worst.
    const auto faulted = std::find_if(hits.begin(), hits.end(),
                                      [](const BlockBaardaHit& h) { return h.sat == "G03"; });
    ASSERT_NE(faulted, hits.end()) << "the slipped line of sight must be reported";
    EXPECT_EQ(faulted->b, 4);
    EXPECT_EQ(faulted->dof, 3);
    EXPECT_GT(faulted->t, 4.0);
    EXPECT_NE(tallies.max_id.find("/G03 "), std::string::npos)
        << "worst block of the sweep: " << tallies.max_id;
    for (const BlockBaardaHit& h : hits) EXPECT_LE(h.t, faulted->t) << h.sat;
}

// ===========================================================================
//  6. The knob
// ===========================================================================

namespace {

namespace bbfs = std::filesystem;

std::string estConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra
        + "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool estLoads(const std::string& tag, const std::string& extra, GnssConfig& cfg) {
    LogManager log;
    const bbfs::path p = gnss_test::tempDir() / ("blockbaarda_est_" + tag + ".json");
    {
        std::ofstream f(p);
        f << estConfig(extra);
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    bbfs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(BlockBaardaEstimableTest, OffByDefaultAndResolvedOnlyWithTheCensus) {
    GnssConfig cfg;
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_FALSE(nc.block_baarda_estimable);
    EXPECT_DOUBLE_EQ(nc.block_baarda_estimable_eps, 0.05);
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.bb_estimable);

    NetworkConfig armed = nc;
    armed.block_baarda_estimable = true;
    EXPECT_FALSE(NetworkKnobs::snapshot(cfg, armed).bb_estimable)
        << "no block census, no estimable form";
    armed.block_baarda = true;
    armed.redundancy_census_stride = 10;
    const NetworkKnobs k2 = NetworkKnobs::snapshot(cfg, armed);
    EXPECT_TRUE(k2.bb_estimable);
    EXPECT_DOUBLE_EQ(k2.bb_estimable_eps, 0.05);
}

TEST(BlockBaardaEstimableTest, LoaderRequiresTheCensusAndRestoresABadFloor) {
    GnssConfig bare;
    EXPECT_FALSE(estLoads("alone", "    \"block_baarda_estimable\": true,\n", bare))
        << "the estimable form of a census that is not armed would do nothing, silently";

    GnssConfig ok;
    ASSERT_TRUE(estLoads("ok", "    \"block_baarda\": true,\n"
                               "    \"redundancy_census_stride\": 10,\n"
                               "    \"block_baarda_estimable\": true,\n"
                               "    \"block_baarda_estimable_eps\": 0.1,\n", ok));
    EXPECT_TRUE(ok.getNetworkConfig().block_baarda_estimable);
    EXPECT_DOUBLE_EQ(ok.getNetworkConfig().block_baarda_estimable_eps, 0.1);

    for (const char* bad : {"0.0", "1.0", "-0.2", "3.0"}) {
        GnssConfig cfg;
        ASSERT_TRUE(estLoads(std::string("bad") + bad,
                             std::string("    \"block_baarda\": true,\n"
                                         "    \"redundancy_census_stride\": 10,\n"
                                         "    \"block_baarda_estimable\": true,\n"
                                         "    \"block_baarda_estimable_eps\": ") + bad + ",\n", cfg));
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().block_baarda_estimable_eps, 0.05) << bad;
    }
}

}  // namespace GnssProcessor
