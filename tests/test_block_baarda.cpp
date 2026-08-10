/**
 * @file test_block_baarda.cpp
 * @brief BLOCKFORMAL — the (station, satellite) LOS block outlier census
 *        (docs/SPEC_block_baarda.md), INC-0: the residual-cofactor kernel.
 *
 * LASER Guide v2.7 slide 5-98 states the gap this card exists to close: there is
 * "not necessarily a one-to-one correspondence between large residuals and
 * outliers".  ONE physical fault on ONE line of sight — multipath, a wet radome
 * lobe, an unflagged slip — lands on every band's code and phase row at once and
 * is then SMEARED by the adjustment across all of them.  The producer's tiers
 * straddle exactly that unit: per-row w-tests sit below it, station-level median
 * polish sits above it, and nothing in between can say WHICH LOS carried a fault.
 * That is the gap that made the 78-station mass refloat unattributable.
 *
 * INC-0 lands the one primitive the middle tier needs and nothing else:
 * @ref NetworkNormalEquations::blockResidualCofactor, the b×b residual cofactor
 * @f$Q_{\tilde e,B} = (P^{-1} - A N^{-} A^\top)_{BB}@f$ — @ref rowRedundancy with
 * the off-diagonals KEPT.  What is worth testing is therefore not "does the
 * arithmetic run" but the four claims that fail silently if they are wrong:
 *
 *   1. It is the REAL cofactor — against an independent DENSE GLS oracle, not
 *      against the kernel's own Schur reduction.  A wrong marginalization still
 *      returns a plausible symmetric matrix and nobody can tell.
 *   2. A BLOCK'S cofactor does not depend on how many of the station's rows were
 *      handed over with it.  The sweep passes one LOS at a time; a kernel that
 *      re-derived normals from the rows it was given would be wrong in exactly
 *      the way that never shows up on a full-station call.
 *   3. It is DATUM-INVARIANT (standing invariant 5 / SPEC §10.5).  Residual
 *      quadratic forms must not care which clock got pinned, and the datum frame
 *      enters this kernel through a caller-supplied covariance — the one place a
 *      frame error could hide.
 *   4. THE SMEARING IS REAL AND THE OFF-DIAGONALS ARE WHERE IT LIVES.  This is
 *      the card's entire thesis, and if it does not hold on a hand-built network
 *      it holds nowhere: an unflagged half-cycle slip on both bands leaves EVERY
 *      w-test in the network silent while the block statistic is 2.4× over its
 *      threshold — and the same slip with the opposite sign on band 2 is caught
 *      by the row tier at 24σ.  Identical fault magnitude, opposite verdicts,
 *      and the only thing that distinguishes them is the off-diagonal structure
 *      this kernel returns.
 *
 * Plus the contract that keeps the census honest: a fully-leveraged block comes
 * back SINGULAR rather than clamped, because that singularity IS the consumer's
 * UNCHECKABLE test (SPEC §3).  @ref rowRedundancy clamps @f$r_i@f$ into [0,1] and
 * must; this method must not, or an uninvertible block gets silently inverted
 * through instead of counted.
 *
 * NOT tested here, because INC-0 wires nothing: the sweep (INC-1), the
 * corroboration join (INC-2), the smearing fraction (INC-3), the config knobs.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/NetworkBlockBaarda.hpp"
#include "filter/NetworkNormalEquations.hpp"

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
//  Critical values — REFERENCE POINTS ONLY.
//
//  These tabular numbers are exact for the constructions below (Gaussian by
//  fiat, sigma0 known = 1), which is what makes them usable inside a unit test.
//  They are NOT an endorsement of tabular critical values in the sweep: SPEC
//  rule §2.1 is "empirical critical values, never tabular", because D5 measured
//  the Gaussian tail wrong by orders of magnitude on real residuals.  INC-1
//  ships an empirical exceedance ladder per family; nothing here imports
//  Snow & Schaffrin's F(alpha, b, r).
// ---------------------------------------------------------------------------
constexpr double kCrit1D = 3.2905;      ///< |w| critical value, alpha = 0.001
constexpr double kCritChi2B4 = 18.4668; ///< chi2 with 4 dof, alpha = 0.001

// ===========================================================================
//  Dense GLS oracle — deliberately independent of the arrowhead kernel.
// ===========================================================================

/// Stacked system + its residual cofactor Q = P^-1 - A (A'WA + Prior)^-1 A'.
struct DenseGls {
    Eigen::MatrixXd A;      ///< rows × params
    Eigen::VectorXd w;      ///< per-row information weight (1/sigma^2)
    Eigen::MatrixXd cov;    ///< (A'WA + Prior)^-1
    Eigen::MatrixXd Q;      ///< residual cofactor over ALL rows

    /// The least-squares residuals of an observation vector.
    Eigen::VectorXd residuals(const Eigen::VectorXd& y) const {
        const Eigen::VectorXd x = cov * (A.transpose() * (w.asDiagonal() * y));
        return y - A * x;
    }
};

/// Assemble and invert.  @p prior_diag is the diagonal prior information over
/// the parameter vector (empty = none).
DenseGls finishDense(const Eigen::MatrixXd& A, const Eigen::VectorXd& w,
                     const Eigen::VectorXd& prior_diag = Eigen::VectorXd()) {
    DenseGls d;
    d.A = A;
    d.w = w;
    Eigen::MatrixXd N = A.transpose() * w.asDiagonal() * A;
    if (prior_diag.size() == N.rows()) N.diagonal() += prior_diag;
    d.cov = N.inverse();
    d.Q = Eigen::MatrixXd(
        Eigen::MatrixXd(Eigen::VectorXd(w.array().inverse()).asDiagonal()) -
        A * d.cov * A.transpose());
    return d;
}

/// The (rows × rows) sub-block of a dense quantity.
Eigen::MatrixXd subBlock(const Eigen::MatrixXd& M,
                         const std::vector<Eigen::Index>& rows) {
    const Eigen::Index b = static_cast<Eigen::Index>(rows.size());
    Eigen::MatrixXd out(b, b);
    for (Eigen::Index i = 0; i < b; ++i)
        for (Eigen::Index j = 0; j < b; ++j) out(i, j) = M(rows[i], rows[j]);
    return out;
}

/// Largest |difference|, relative to the larger matrix's own scale.
double relDiff(const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
    const double scale = std::max(a.cwiseAbs().maxCoeff(), b.cwiseAbs().maxCoeff());
    return (a - b).cwiseAbs().maxCoeff() / std::max(scale, 1e-300);
}

// ===========================================================================
//  Toy A — the census's two-station system (small, well-conditioned, no gauge).
// ===========================================================================

struct TwoStationToy {
    static constexpr Eigen::Index kNs = 2, kNl = 2, kM = 4;
    Eigen::MatrixXd A_s[2], A_r[2];
    Eigen::VectorXd w[2];
    std::size_t slot[2]{};
    Eigen::MatrixXd cov_free;   ///< the shared block of the dense inverse
    DenseGls dense;
};

/// Build toy A into @p neq (which must have been constructed with kNs shared).
TwoStationToy buildTwoStation(NetworkNormalEquations& neq) {
    TwoStationToy t;
    constexpr Eigen::Index ns = TwoStationToy::kNs, nl = TwoStationToy::kNl,
                           m = TwoStationToy::kM;
    t.A_s[0].resize(m, ns); t.A_s[0] << 1.0,  0.0,
                                        0.0,  1.0,
                                        1.0,  1.0,
                                        0.5, -0.3;
    t.A_r[0].resize(m, nl); t.A_r[0] << 1.0,  0.0,
                                        1.0,  0.2,
                                        0.0,  1.0,
                                        0.4,  0.7;
    t.A_s[1].resize(m, ns); t.A_s[1] << 1.0,  0.5,
                                       -0.2,  1.0,
                                        0.9, -0.1,
                                        0.3,  0.3;
    t.A_r[1].resize(m, nl); t.A_r[1] << 1.0,  0.1,
                                        0.0,  1.0,
                                        0.6,  0.6,
                                        1.0, -0.4;
    t.w[0].resize(m); t.w[0] << 1.0, 2.0, 0.5, 4.0;
    t.w[1].resize(m); t.w[1] << 3.0, 1.0, 1.5, 0.8;

    const Eigen::VectorXd y = Eigen::VectorXd::Zero(m);
    for (int k = 0; k < 2; ++k)
        t.slot[k] = neq.addStationObservations(t.A_s[k], t.A_r[k], t.w[k], y);
    neq.reduce();

    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(2 * m, ns + 2 * nl);
    Eigen::VectorXd w(2 * m);
    for (int k = 0; k < 2; ++k) {
        A.block(k * m, 0, m, ns) = t.A_s[k];
        A.block(k * m, ns + k * nl, m, nl) = t.A_r[k];
        w.segment(k * m, m) = t.w[k];
    }
    t.dense = finishDense(A, w);
    // The kernel's datum-frame shared covariance IS the shared block of the
    // dense inverse: no gauge here, so nothing is pinned or zeroed.
    t.cov_free = t.dense.cov.topLeftCorner(ns, ns);
    return t;
}

// ===========================================================================
//  Toy B — a physically-shaped network with a real rank-1 clock gauge.
//
//  kR stations × kS satellites, four rows per LOS (code/phase × two bands),
//  which is the block this card enumerates.  Shared = per-satellite clock;
//  local = receiver clock, per-satellite slant iono, per-(satellite, band)
//  ambiguity.  The iono and ambiguity priors stand in for the SH tie and the
//  phase-leveling carry — without them the LOS block is rank-deficient on its
//  own and every phase row is uncheckable, which is a different card.
//
//  Its null space is the classic one: adding a constant to every receiver clock
//  AND every satellite clock leaves every row unchanged, so the shared block is
//  rank-deficient by exactly one and needs a datum pin.
// ===========================================================================

constexpr int kR = 3;                         ///< stations
constexpr int kS = 4;                         ///< satellites
constexpr Eigen::Index kRowsPerSta = 4 * kS;  ///< four rows per LOS
constexpr Eigen::Index kLocal = 1 + 3 * kS;   ///< dt, I_k, (N_k0, N_k1)
constexpr Eigen::Index kNTot = kS + kR * kLocal;

constexpr double kMu2 = (154.0 / 120.0) * (154.0 / 120.0);  ///< L2 iono coefficient
constexpr double kLam1 = 0.190293672798365;                 ///< L1 wavelength (m)
constexpr double kLam2 = 0.244210213424568;                 ///< L2 wavelength (m)
constexpr double kSigCode = 0.3, kSigPhase = 0.003;         ///< row sigmas (m)
constexpr double kSigIono = 0.01, kSigAmb = 0.005;          ///< prior sigmas (m)
constexpr double kPin = 1e14;                               ///< Dirichlet datum pin

/// Row kinds within one LOS block, in the order the block carries them.
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

/// Iono coefficient: code is delayed, phase is advanced.
double muOf(int kind) {
    switch (kind) {
        case kC1: return 1.0;
        case kC2: return kMu2;
        case kL1: return -1.0;
        default:  return -kMu2;
    }
}

struct LosNet {
    Eigen::MatrixXd A_s[kR], A_r[kR];
    Eigen::VectorXd w[kR];
    std::size_t slot[kR]{};
    Eigen::MatrixXd cov_free;   ///< datum-frame shared cov, pinned col zeroed
    DenseGls dense;             ///< the pinned column DROPPED outright
    Eigen::Index pin_col{0};
    std::vector<Eigen::Index> keep;  ///< dense parameter columns retained

    /// The four rows of one LOS, as the sweep would hand them over.
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

    /// The same LOS's rows in the DENSE stack.
    std::vector<Eigen::Index> denseRows(int r, int k) const {
        return {globalRow(r, k, kC1), globalRow(r, k, kC2),
                globalRow(r, k, kL1), globalRow(r, k, kL2)};
    }
};

/// Build toy B into @p neq (constructed with kS shared columns), realizing the
/// clock datum by pinning satellite clock @p pin_col.
LosNet buildLosNet(NetworkNormalEquations& neq, Eigen::Index pin_col) {
    LosNet net;
    net.pin_col = pin_col;

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
                net.A_s[r](row, k) = -1.0;                  // satellite clock
                net.A_r[r](row, 0) = 1.0;                   // receiver clock
                net.A_r[r](row, ionoCol(k)) = muOf(kind);   // slant iono
                if (kind == kL1) net.A_r[r](row, ambCol(k, 0)) = kLam1;
                if (kind == kL2) net.A_r[r](row, ambCol(k, 1)) = kLam2;
                const double s = sigmaOf(r, k, kind);
                net.w[r](row) = 1.0 / (s * s);
            }
        }
        // Local priors: the SH tie on slant iono, the leveling carry on ambiguity.
        std::vector<Eigen::Index> cols;
        std::vector<double> info;
        for (int k = 0; k < kS; ++k) {
            cols.push_back(ionoCol(k));
            info.push_back(1.0 / (kSigIono * kSigIono));
            for (int band = 0; band < 2; ++band) {
                cols.push_back(ambCol(k, band));
                info.push_back(1.0 / (kSigAmb * kSigAmb));
            }
        }
        const Eigen::VectorXd y = Eigen::VectorXd::Zero(kRowsPerSta);
        net.slot[r] = neq.addStationObservations(net.A_s[r], net.A_r[r], net.w[r], y);
        Eigen::VectorXd dN = Eigen::Map<Eigen::VectorXd>(
            info.data(), static_cast<Eigen::Index>(info.size()));
        neq.addStationPrior(net.slot[r], cols,
                            dN, Eigen::VectorXd::Zero(dN.size()));

        // ...and the same system, densely.
        const Eigen::Index base = kS + r * kLocal;
        A.block(r * kRowsPerSta, 0, kRowsPerSta, kS) = net.A_s[r];
        A.block(r * kRowsPerSta, base, kRowsPerSta, kLocal) = net.A_r[r];
        w.segment(r * kRowsPerSta, kRowsPerSta) = net.w[r];
        for (std::size_t i = 0; i < cols.size(); ++i) prior(base + cols[i]) = info[i];
    }

    // The datum: a Dirichlet pin on one satellite clock, exactly as the driver
    // realizes it.  The kernel then treats that column as EXACT by zeroing its
    // row/col in the shared covariance — which is what the dense oracle does by
    // DROPPING the column outright.  The two agree in the pin -> infinity limit,
    // and kPin sits far enough above the normals' scale to make that exact to
    // ~1e-13 relative.
    Eigen::MatrixXd pin_info = Eigen::MatrixXd::Zero(kS, kS);
    pin_info(pin_col, pin_col) = kPin;
    neq.addSharedInfo(pin_info, Eigen::VectorXd::Zero(kS));
    neq.reduce();
    net.cov_free = neq.reducedSharedNormals().inverse();
    net.cov_free.row(pin_col).setZero();
    net.cov_free.col(pin_col).setZero();

    for (Eigen::Index c = 0; c < kNTot; ++c)
        if (c != pin_col) net.keep.push_back(c);
    Eigen::MatrixXd Ad(A.rows(), static_cast<Eigen::Index>(net.keep.size()));
    Eigen::VectorXd pd(static_cast<Eigen::Index>(net.keep.size()));
    for (std::size_t c = 0; c < net.keep.size(); ++c) {
        Ad.col(static_cast<Eigen::Index>(c)) = A.col(net.keep[c]);
        pd(static_cast<Eigen::Index>(c)) = prior(net.keep[c]);
    }
    net.dense = finishDense(Ad, w, pd);
    return net;
}

/// Baarda's scalar w-test on every row of a dense fit: |e_i| / sqrt(Q_ii).
Eigen::VectorXd wTests(const DenseGls& d, const Eigen::VectorXd& e) {
    Eigen::VectorXd t(e.size());
    for (Eigen::Index i = 0; i < e.size(); ++i)
        t(i) = std::abs(e(i)) / std::sqrt(d.Q(i, i));
    return t;
}

/// The block statistic's numerator, T_B * b = e_B' Q_B^-1 e_B (SPEC §3).
double blockStatistic(const Eigen::MatrixXd& Q_block, const Eigen::VectorXd& e_block) {
    return e_block.dot(Q_block.ldlt().solve(e_block));
}

}  // namespace

// ===========================================================================
//  1. It is the real residual cofactor (dense GLS oracle) — SPEC §7.1
// ===========================================================================

TEST(BlockBaardaTest, BlockCofactorMatchesDenseGls) {
    NetworkNormalEquations neq(TwoStationToy::kNs);
    const TwoStationToy t = buildTwoStation(neq);
    constexpr Eigen::Index m = TwoStationToy::kM;

    for (int k = 0; k < 2; ++k) {
        const Eigen::MatrixXd Q =
            neq.blockResidualCofactor(t.slot[k], t.A_s[k], t.A_r[k], t.w[k], t.cov_free);
        ASSERT_EQ(Q.rows(), m);
        ASSERT_EQ(Q.cols(), m);
        for (Eigen::Index i = 0; i < m; ++i)
            for (Eigen::Index j = 0; j < m; ++j)
                EXPECT_NEAR(Q(i, j), t.dense.Q(k * m + i, k * m + j), 1e-10)
                    << "station " << k << " entry (" << i << "," << j << ")";
        // Exactly symmetric, not merely symmetric to tolerance: the consumer
        // factorizes this matrix, and an LDL^T of an asymmetric input is a
        // silently wrong quadratic form rather than an error.
        for (Eigen::Index i = 0; i < m; ++i)
            for (Eigen::Index j = 0; j < m; ++j)
                EXPECT_EQ(Q(i, j), Q(j, i));
    }
}

// ===========================================================================
//  2. A block's cofactor does not depend on what was handed over WITH it
// ===========================================================================

TEST(BlockBaardaTest, SubsetOfRowsReturnsExactlyThatSubBlock) {
    // The sweep passes ONE LOS at a time.  The covariance comes from the cached
    // factors, which already hold every row of every station, so a two-row call
    // must return the same numbers as the corresponding corner of the four-row
    // call.  A kernel that re-derived normals from the rows it was handed would
    // pass every full-station test and fail only here — where it matters.
    NetworkNormalEquations neq(TwoStationToy::kNs);
    const TwoStationToy t = buildTwoStation(neq);

    const Eigen::MatrixXd full =
        neq.blockResidualCofactor(t.slot[0], t.A_s[0], t.A_r[0], t.w[0], t.cov_free);

    const std::vector<Eigen::Index> pick{1, 3};
    Eigen::MatrixXd Bs(2, TwoStationToy::kNs), Br(2, TwoStationToy::kNl);
    Eigen::VectorXd bw(2);
    for (int i = 0; i < 2; ++i) {
        Bs.row(i) = t.A_s[0].row(pick[i]);
        Br.row(i) = t.A_r[0].row(pick[i]);
        bw(i) = t.w[0](pick[i]);
    }
    const Eigen::MatrixXd part =
        neq.blockResidualCofactor(t.slot[0], Bs, Br, bw, t.cov_free);
    ASSERT_EQ(part.rows(), 2);
    EXPECT_LT(relDiff(part, subBlock(full, pick)), 1e-14);
    EXPECT_LT(relDiff(part, subBlock(t.dense.Q, pick)), 1e-10);

    // b = 1 is the scalar w-test's denominator — the row tier is this method's
    // special case, not a parallel instrument.
    for (Eigen::Index i = 0; i < TwoStationToy::kM; ++i) {
        const Eigen::MatrixXd one = neq.blockResidualCofactor(
            t.slot[0], t.A_s[0].row(i), t.A_r[0].row(i),
            t.w[0].segment(i, 1), t.cov_free);
        ASSERT_EQ(one.rows(), 1);
        EXPECT_NEAR(one(0, 0), full(i, i), 1e-14);
    }
}

// ===========================================================================
//  3. The diagonal IS rowRedundancy — the two tiers are one instrument
// ===========================================================================

TEST(BlockBaardaTest, DiagonalReproducesRowRedundancy) {
    NetworkNormalEquations neq(TwoStationToy::kNs);
    const TwoStationToy t = buildTwoStation(neq);

    for (int k = 0; k < 2; ++k) {
        const Eigen::VectorXd r =
            neq.rowRedundancy(t.slot[k], t.A_s[k], t.A_r[k], t.w[k], t.cov_free);
        const Eigen::MatrixXd Q =
            neq.blockResidualCofactor(t.slot[k], t.A_s[k], t.A_r[k], t.w[k], t.cov_free);
        for (Eigen::Index i = 0; i < TwoStationToy::kM; ++i) {
            // r_i = w_i * Q_ii.  Every r_i here is interior to [0,1], so
            // rowRedundancy's clamp is inactive and the identity is exact.
            ASSERT_GT(r(i), 1e-3);
            ASSERT_LT(r(i), 1.0 - 1e-3);
            EXPECT_NEAR(t.w[k](i) * Q(i, i), r(i), 1e-12) << "row " << i;
        }
    }
}

// ===========================================================================
//  4. Rank-deficient datum: matches the dense fit with the pin DROPPED
// ===========================================================================

TEST(BlockBaardaTest, RankDeficientDatumMatchesDenseGls) {
    // Toy B's shared block is rank-deficient by one (the receiver/satellite
    // clock gauge).  The kernel sees a Dirichlet pin plus a zeroed covariance
    // row/col; the oracle sees that parameter removed from the model entirely.
    // Those are the same estimator, and the block cofactor must agree across all
    // twelve LOS blocks — this is the only test where the datum machinery is
    // between the kernel and the truth.
    NetworkNormalEquations neq(kS);
    const LosNet net = buildLosNet(neq, /*pin_col=*/0);

    for (int r = 0; r < kR; ++r) {
        for (int k = 0; k < kS; ++k) {
            Eigen::MatrixXd Bs, Br;
            Eigen::VectorXd bw;
            net.block(r, k, Bs, Br, bw);
            const Eigen::MatrixXd Q =
                neq.blockResidualCofactor(net.slot[r], Bs, Br, bw, net.cov_free);
            const Eigen::MatrixXd ref = subBlock(net.dense.Q, net.denseRows(r, k));
            EXPECT_LT(relDiff(Q, ref), 1e-10)
                << "station " << r << " satellite " << k;
        }
    }
}

// ===========================================================================
//  5. Datum invariance — standing invariant 5 / SPEC §10.5
// ===========================================================================

TEST(BlockBaardaTest, BlockCofactorIsDatumInvariant) {
    // Residual quadratic forms are gauge-free: which satellite clock realizes
    // the datum must not move a single digit of the cofactor.  The datum enters
    // this kernel through a caller-supplied covariance, so a frame error would
    // land HERE and nowhere else — and it would look like a plausible matrix.
    NetworkNormalEquations neq_a(kS), neq_b(kS);
    const LosNet a = buildLosNet(neq_a, /*pin_col=*/0);
    const LosNet b = buildLosNet(neq_b, /*pin_col=*/2);

    for (int r = 0; r < kR; ++r) {
        for (int k = 0; k < kS; ++k) {
            Eigen::MatrixXd Bs, Br;
            Eigen::VectorXd bw;
            a.block(r, k, Bs, Br, bw);
            const Eigen::MatrixXd Qa =
                neq_a.blockResidualCofactor(a.slot[r], Bs, Br, bw, a.cov_free);
            const Eigen::MatrixXd Qb =
                neq_b.blockResidualCofactor(b.slot[r], Bs, Br, bw, b.cov_free);
            EXPECT_LT(relDiff(Qa, Qb), 1e-9) << "station " << r << " satellite " << k;
        }
    }
}

// ===========================================================================
//  6. THE SMEARING — the reason this card exists, as a unit test (SPEC §7.2)
// ===========================================================================

TEST(BlockBaardaTest, SmearedFaultIsSilentPerRowAndLoudPerBlock) {
    // The motivating fault, physically: an unflagged HALF-CYCLE slip on both
    // bands of one LOS.  It is absorbed almost entirely by that LOS's ambiguity
    // and iono states and smeared over all four of its rows, so no single row
    // stands out — but it violates the near -0.87 correlation the cofactor
    // holds between the two phase rows, and the block sees that immediately.
    //
    // The SPEC's wording is "a fault split evenly across 4 rows"; a real slip is
    // the more faithful version of the same claim and is what gets run here.
    NetworkNormalEquations neq(kS);
    const LosNet net = buildLosNet(neq, /*pin_col=*/0);
    constexpr int kFaultSta = 0, kFaultSat = 3;

    Eigen::MatrixXd Bs, Br;
    Eigen::VectorXd bw;
    net.block(kFaultSta, kFaultSat, Bs, Br, bw);
    const Eigen::MatrixXd Q =
        neq.blockResidualCofactor(net.slot[kFaultSta], Bs, Br, bw, net.cov_free);
    const std::vector<Eigen::Index> rows = net.denseRows(kFaultSta, kFaultSat);

    // Every row of this block is genuinely checkable on its own — the effect
    // below is smearing, NOT an uncheckable cell hiding a fault (that is the
    // redundancy census's tier, and conflating the two would make this test
    // prove the wrong thing).
    for (Eigen::Index i = 0; i < 4; ++i)
        EXPECT_GT(bw(i) * Q(i, i), 0.3) << "row " << i << " is not checkable";

    auto inject = [&](double n1, double n2) {
        Eigen::VectorXd y = Eigen::VectorXd::Zero(net.dense.A.rows());
        y(rows[kL1]) += n1 * kLam1;
        y(rows[kL2]) += n2 * kLam2;
        return net.dense.residuals(y);
    };
    auto blockRes = [&](const Eigen::VectorXd& e) {
        Eigen::VectorXd eb(4);
        for (int i = 0; i < 4; ++i) eb(i) = e(rows[i]);
        return eb;
    };

    // ---- same sign on both bands: EVERY w-test in the network is silent -----
    const Eigen::VectorXd e_same = inject(0.5, 0.5);
    const Eigen::VectorXd w_same = wTests(net.dense, e_same);
    EXPECT_LT(w_same.maxCoeff(), kCrit1D)
        << "the row tier must be blind to this fault for the test to mean anything";
    const double T_same = blockStatistic(Q, blockRes(e_same));
    EXPECT_GT(T_same, kCritChi2B4) << "the block tier must see it";
    // Measured 43.7 against a 18.47 threshold — a 2.4x margin, asserted loosely
    // so the test tracks the claim rather than the toy's last digit.
    EXPECT_GT(T_same, 30.0);

    // ---- opposite sign, IDENTICAL magnitude: the row tier catches it --------
    // Same fault size, same rows, same block cofactor.  The only difference is
    // the sign pattern, which is precisely what the off-diagonals encode: a
    // diagonal-only statistic cannot tell these two cases apart, and it must.
    const Eigen::VectorXd e_opp = inject(0.5, -0.5);
    const Eigen::VectorXd w_opp = wTests(net.dense, e_opp);
    EXPECT_GT(w_opp.maxCoeff(), 5.0 * kCrit1D);  // measured 24.1, i.e. 7.3x
    EXPECT_GT(blockStatistic(Q, blockRes(e_opp)), T_same);

    // ---- the converse: independent faults must NOT pool into a clean block --
    // Three sibling LOS blocks at the SAME station each take a 1 m code fault.
    // The block statistic is local: it must not accumulate its station's total
    // misfit, or every block at a bad station would flag and the tier would be
    // no better than the station-level polish it sits under.
    Eigen::VectorXd y = Eigen::VectorXd::Zero(net.dense.A.rows());
    for (int k = 0; k < 3; ++k) y(globalRow(kFaultSta, k, kC1)) += 1.0;
    const Eigen::VectorXd e_multi = net.dense.residuals(y);
    const double T_clean = blockStatistic(Q, blockRes(e_multi));
    EXPECT_LT(T_clean, 1.0) << "a clean block must stay quiet beside faulted siblings";
    for (int k = 0; k < 3; ++k) {
        Eigen::MatrixXd Fs, Fr;
        Eigen::VectorXd fw;
        net.block(kFaultSta, k, Fs, Fr, fw);
        const Eigen::MatrixXd Qk =
            neq.blockResidualCofactor(net.slot[kFaultSta], Fs, Fr, fw, net.cov_free);
        Eigen::VectorXd ek(4);
        const std::vector<Eigen::Index> rk = net.denseRows(kFaultSta, k);
        for (int i = 0; i < 4; ++i) ek(i) = e_multi(rk[i]);
        EXPECT_GT(blockStatistic(Qk, ek), 10.0 * T_clean)
            << "faulted block " << k << " must dominate the clean one";
    }
}

// ===========================================================================
//  7. A fully-leveraged block comes back SINGULAR, not clamped (SPEC §3)
// ===========================================================================

TEST(BlockBaardaTest, FullyLeveragedBlockIsSingularAndNotClamped) {
    // Two rows, each feeding its OWN otherwise-unconstrained local state: both
    // are absorbed exactly, both have r ~ 0, and the block cofactor is ~0 —
    // singular, uninvertible, UNCHECKABLE.  rowRedundancy clamps r into [0,1]
    // and must; this method must NOT clamp, because the singularity is the only
    // signal the consumer has that the block may never be inverted through.
    constexpr Eigen::Index ns = 1;
    constexpr double kWhisper = 1e-9;  // just enough to factorize N_rr
    NetworkNormalEquations neq(ns);

    // A well-observed anchor pins the shared column.
    Eigen::MatrixXd A0 = Eigen::MatrixXd::Ones(6, ns);
    const Eigen::VectorXd w0 = Eigen::VectorXd::Constant(6, 1.0);
    neq.addStationObservations(A0, Eigen::MatrixXd::Zero(6, 0), w0,
                               Eigen::VectorXd::Zero(6));
    // The suspect: two rows, two private states, one row each.
    Eigen::MatrixXd A1s = Eigen::MatrixXd::Ones(2, ns);
    Eigen::MatrixXd A1r = Eigen::MatrixXd::Identity(2, 2);
    const Eigen::VectorXd w1 = Eigen::VectorXd::Constant(2, 1.0);
    const std::size_t s1 = neq.addStationObservations(A1s, A1r, w1,
                                                      Eigen::VectorXd::Zero(2));
    neq.addStationPrior(s1, {0, 1}, Eigen::VectorXd::Constant(2, kWhisper),
                        Eigen::VectorXd::Zero(2));
    neq.reduce();
    const Eigen::MatrixXd cov_free = neq.reducedSharedNormals().inverse();

    const Eigen::MatrixXd Q = neq.blockResidualCofactor(s1, A1s, A1r, w1, cov_free);
    ASSERT_EQ(Q.rows(), 2);
    // P^-1 is the identity here, so a healthy block would be O(1).  This one is
    // numerically zero: any conditioning test the consumer applies rejects it.
    EXPECT_LT(Q.cwiseAbs().maxCoeff(), 1e-6);
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Q);
    EXPECT_LT(std::abs(es.eigenvalues()(1)), 1e-6)
        << "a fully-leveraged block must be returned SINGULAR";

    // ...and the row tier reads the same cells as uncheckable, so the two tiers
    // agree about which population this is.
    const Eigen::VectorXd r = neq.rowRedundancy(s1, A1s, A1r, w1, cov_free);
    for (Eigen::Index i = 0; i < 2; ++i) EXPECT_LT(r(i), 1e-6);
}

// ===========================================================================
//  8. Shape contract and validation
// ===========================================================================

TEST(BlockBaardaTest, ShapeContractAndValidation) {
    NetworkNormalEquations neq(TwoStationToy::kNs);
    const TwoStationToy t = buildTwoStation(neq);
    constexpr Eigen::Index ns = TwoStationToy::kNs, nl = TwoStationToy::kNl;

    // An empty block is empty, not a throw: the sweep may legitimately enumerate
    // a satellite with no surviving rows this epoch.
    const Eigen::MatrixXd none = neq.blockResidualCofactor(
        t.slot[0], Eigen::MatrixXd(0, ns), Eigen::MatrixXd(0, nl),
        Eigen::VectorXd(0), t.cov_free);
    EXPECT_EQ(none.size(), 0);

    // A non-positive weight puts a non-finite entry on the diagonal ON PURPOSE
    // (header note 1): rowRedundancy would clamp such a row to r = 0, which is
    // indistinguishable from a healthy uncheckable riser, whereas an infinite
    // cofactor entry fails the caller's finiteness test outright.
    Eigen::VectorXd w_bad = t.w[0];
    w_bad(2) = 0.0;
    const Eigen::MatrixXd Q_bad =
        neq.blockResidualCofactor(t.slot[0], t.A_s[0], t.A_r[0], w_bad, t.cov_free);
    EXPECT_FALSE(Q_bad.allFinite());
    EXPECT_TRUE(std::isfinite(Q_bad(0, 1)));  // ...only the offending row

    EXPECT_THROW(neq.blockResidualCofactor(99, t.A_s[0], t.A_r[0], t.w[0], t.cov_free),
                 std::out_of_range);
    EXPECT_THROW(neq.blockResidualCofactor(t.slot[0], t.A_s[0].topRows(2), t.A_r[0],
                                           t.w[0], t.cov_free),
                 std::invalid_argument);
    EXPECT_THROW(neq.blockResidualCofactor(t.slot[0], t.A_s[0], t.A_r[0], t.w[0],
                                           Eigen::MatrixXd::Identity(ns + 1, ns + 1)),
                 std::invalid_argument);
    EXPECT_THROW(neq.blockResidualCofactor(t.slot[0], t.A_s[0],
                                           Eigen::MatrixXd::Zero(TwoStationToy::kM, nl + 1),
                                           t.w[0], t.cov_free),
                 std::invalid_argument);
}


// ===========================================================================
//  INC-1..3 — the sweep, the ladder, the calibration and the smearing fraction
//
//  A separate suite because it drives a different object: everything above tests
//  the KERNEL (an Eigen quantity), everything below tests the ACCUMULATOR (a
//  pure-numeric leaf driven with synthetic blocks of known truth, the
//  NetworkPriorResidual pattern).  Both live in this file because they are one
//  card, and the project's rule is one suite NAME to one file — not one suite
//  per file.
// ===========================================================================

namespace {

/// Deterministic draws.  A fixed seed, because a statistical test that fails one
/// run in twenty is worse than no test at all.
struct Rng {
    std::mt19937_64 g{20260804u};
    std::normal_distribution<double> nd{0.0, 1.0};
    double normal() { return nd(g); }
    double chi2(int k) {
        double s = 0.0;
        for (int i = 0; i < k; ++i) { const double z = normal(); s += z * z; }
        return s;
    }
    /// Student-t with @p nu degrees of freedom — the D5 heavy-tail reality.
    double studentT(int nu) { return normal() / std::sqrt(chi2(nu) / nu); }
};

/// A synthetic block drawn from the NULL at a known scale: T_B = s0^2 * chi2_b/b.
NetworkBlockBaarda::Input nullBlock(Rng& rng, int b, double sigma0,
                                    const char* sat = "G01") {
    NetworkBlockBaarda::Input in;
    in.station   = 0;
    in.sat       = sat;
    in.b         = b;
    in.T         = sigma0 * sigma0 * rng.chi2(b) / static_cast<double>(b);
    in.max_row_T = 0.0;
    in.n_code    = b;
    return in;
}

/// Drive @p n_epochs epochs of @p per_epoch null blocks, then one more
/// beginEpoch so sigma0 is frozen against everything that was fed.
void feedNull(NetworkBlockBaarda& acc, Rng& rng, int b, double sigma0,
              int n_epochs, int per_epoch) {
    for (int e = 0; e < n_epochs; ++e) {
        acc.beginEpoch(30.0 * e, 30.0 * e);
        for (int i = 0; i < per_epoch; ++i) acc.addBlock(nullBlock(rng, b, sigma0));
    }
    acc.beginEpoch(30.0 * n_epochs, 30.0 * n_epochs);
}

}  // namespace

// ---------------------------------------------------------------------------
//  The chi-square-b rescaling — the correction SPEC 3's literal recipe omits
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, Chi2GeoMeanMatchesTheClosedForm) {
    // 2*exp(psi(b/2)).  If this drifts, every sigma0 in the card drifts with it
    // and the whole ladder silently re-scales — so it is pinned against
    // independently computed values, not against its own recurrence.
    EXPECT_NEAR(blockChi2GeoMean(1),  0.2807297417756531,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(2),  1.1229189670878104,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(3),  2.0743278106186143,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(4),  3.0524102230667953,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(6),  5.0325736616727479,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(8),  7.0235223323914848,  1e-9);
    EXPECT_NEAR(blockChi2GeoMean(12), 11.0150756589518970, 1e-8);
    // b = 1 must return NetworkCarryVce's OWN constant bit-for-bit, so the
    // rescaling factor is exactly 1.0 there and the D5 feed is unchanged.
    EXPECT_EQ(blockChi2GeoMean(1), kBlockChi2GeoMean1);
    EXPECT_EQ(blockChi2GeoMean(0), kBlockChi2GeoMean1);   // degenerate input, no NaN
}

TEST(BlockBaardaSweepTest, Sigma0RecoversTheScaleAtEveryBlockSize) {
    // THE REASON THE RESCALING EXISTS.  NetworkCarryVce inverts the geometric
    // mean of a chi-square_1 variate, but T_B*b is chi-square_b.  Without the
    // correction a b = 8 family reports sqrt(7.0235/(8*0.28073)) = 1.77x the true
    // scale — a plausible number, a silently re-scaled ladder, and a bias that
    // MOVES with the constellation's band plan.  Every block size must recover
    // the same scale from the same underlying sigma0.
    constexpr double kSigma0 = 2.0;
    for (int b : {1, 2, 4, 8}) {
        Rng rng;
        NetworkBlockBaarda acc({"G"}, /*window_epochs=*/0.0, /*min_rows=*/50.0);
        feedNull(acc, rng, b, kSigma0, /*n_epochs=*/20, /*per_epoch=*/300);
        ASSERT_TRUE(acc.sigma0Warm(0)) << "b = " << b;
        EXPECT_NEAR(acc.sigma0(0), kSigma0, 0.10)
            << "b = " << b << " recovered sigma0 = " << acc.sigma0(0);
    }
}

TEST(BlockBaardaSweepTest, Sigma0IsUnbiasedOnAMixedBlockSizePopulation) {
    // The real population: one constellation's lines of sight carry 2, 4, 6 or 8
    // rows depending on how many bands each satellite is tracked on.  A
    // correction that only worked one size at a time would still leave THIS
    // biased — and this is the case that actually occurs in the field.
    constexpr double kSigma0 = 1.5;
    Rng rng;
    NetworkBlockBaarda acc({"G"}, 0.0, 50.0);
    const int sizes[] = {1, 2, 3, 4, 6, 8};
    for (int e = 0; e < 25; ++e) {
        acc.beginEpoch(30.0 * e, 30.0 * e);
        for (int i = 0; i < 300; ++i)
            acc.addBlock(nullBlock(rng, sizes[i % 6], kSigma0));
    }
    acc.beginEpoch(1000.0, 1000.0);
    ASSERT_TRUE(acc.sigma0Warm(0));
    EXPECT_NEAR(acc.sigma0(0), kSigma0, 0.08);
}

// ---------------------------------------------------------------------------
//  SPEC 7.3 — the null ladder, and the heavy-tail failure mode it must preserve
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, GaussianNullLadderMatchesTheChiSquareTail) {
    // With sigma0 converged, t_B == T_B == chi-square_b/b, so the ladder
    // occupancies ARE the chi-square tail and nothing else.  Reference values
    // computed independently (4e6 Monte-Carlo draws): for b = 4,
    // P(T > 1) = 0.4060, P(T > 2) = 0.0913, P(T > 3) = 0.0174.
    Rng rng;
    NetworkBlockBaarda acc({"G"}, 0.0, 50.0);
    feedNull(acc, rng, /*b=*/4, /*sigma0=*/1.0, /*n_epochs=*/30, /*per_epoch=*/400);
    ASSERT_TRUE(acc.sigma0Warm(0));
    for (int i = 0; i < 20000; ++i) acc.addBlock(nullBlock(rng, 4, 1.0));
    const NetworkBlockBaarda::Ladder& L =
        acc.epochLadder(0, PriorResidualLabel::Clean);
    ASSERT_EQ(L.n_blocks, 20000);
    EXPECT_NEAR(L.frac(0), 0.4060, 0.02);    // > 1
    EXPECT_NEAR(L.frac(1), 0.0913, 0.01);    // > 2
    EXPECT_NEAR(L.frac(2), 0.0174, 0.006);   // > 3
    // The calibration indicator reads 1.0 when the scale has converged, and it is
    // b-INVARIANT by construction — a raw geometric mean of t_B would read
    // 2*exp(psi(2))/4 = 0.763 here and would move with the band plan.
    EXPECT_NEAR(L.geoT(), 1.0, 0.05);
}

TEST(BlockBaardaSweepTest, HeavyTailOverOccupiesTheLadderWhileTheScaleReadsConverged) {
    // THE D5 FAILURE MODE, PRESERVED AT BLOCK LEVEL — and the whole reason SPEC
    // 2.1 forbids tabular critical values.  Student-t (nu = 3) rows produce a
    // family whose SCALE calibrates perfectly (geo_cal ~ 1, so every "is it
    // calibrated?" check passes) while the far tail is over-occupied by nearly an
    // order of magnitude against the Gaussian.  A tabular F(alpha, b, r) prices
    // that tail at the Gaussian rate and is wrong by exactly that factor.
    Rng rng;
    NetworkBlockBaarda acc({"G"}, 0.0, 50.0);
    auto tBlock = [&]() {
        NetworkBlockBaarda::Input in;
        in.station = 0; in.sat = "G01"; in.b = 1; in.n_code = 1;
        const double t = rng.studentT(3);
        in.T = t * t;                        // b = 1: T_B is the squared residual
        return in;
    };
    for (int e = 0; e < 30; ++e) {
        acc.beginEpoch(30.0 * e, 30.0 * e);
        for (int i = 0; i < 400; ++i) acc.addBlock(tBlock());
    }
    acc.beginEpoch(1000.0, 1000.0);
    ASSERT_TRUE(acc.sigma0Warm(0));
    for (int i = 0; i < 40000; ++i) acc.addBlock(tBlock());
    const NetworkBlockBaarda::Ladder& L =
        acc.epochLadder(0, PriorResidualLabel::Clean);

    // The scale looks converged...
    EXPECT_NEAR(L.geoT(), 1.0, 0.06);
    // ...and the tail is nothing like Gaussian.  Reference (4e6 draws): t_3 gives
    // P(> 4) = 0.0952 and P(> 8) = 0.0424, against chi-square_1's 0.0454 / 0.0047.
    // Measured here: 2.17x at the 4-rung and ~9x at the 8-rung.  The bound is
    // set below the measurement, not at it — this test exists to catch the
    // correction being LOST, not to pin a Monte-Carlo digit.
    EXPECT_GT(L.frac(3), 1.8 * 0.0454)
        << "the 4-rung must be well over the Gaussian tail";
    EXPECT_GT(L.frac(5), 5.0 * 0.0047)
        << "the 8-rung must be GROSSLY over the Gaussian tail";
}

// ---------------------------------------------------------------------------
//  SPEC 7.4 — label partition, uncheckable counting, predict-then-update
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, LabelPartitionSumsAndUncheckableCounting) {
    Rng rng;
    NetworkBlockBaarda acc({"G"}, 0.0, 50.0);
    acc.beginEpoch(0.0, 0.0);
    const PriorResidualLabel labels[3] = {PriorResidualLabel::Clean,
                                          PriorResidualLabel::Responded,
                                          PriorResidualLabel::Frame};
    long long expect[3] = {0, 0, 0};
    long long expect_unchk = 0, expect_rows = 0;
    for (int i = 0; i < 600; ++i) {
        NetworkBlockBaarda::Input in = nullBlock(rng, 4, 1.0);
        in.label = labels[i % 3];
        if (i % 7 == 0) { in.uncheckable = true; ++expect_unchk; }
        ++expect[i % 3];
        expect_rows += in.b;
        acc.addBlock(in);
    }
    long long sum = 0, sum_unchk = 0, sum_rows = 0;
    for (int l = 0; l < kPriorResidualLabels; ++l) {
        const NetworkBlockBaarda::Ladder& L = acc.epochLadder(0, labels[l]);
        EXPECT_EQ(L.n_blocks, expect[l])
            << "label " << priorResidualLabelName(labels[l]);
        sum       += L.n_blocks;
        sum_unchk += L.n_uncheckable;
        sum_rows  += L.n_rows;
    }
    // The partition is EXACT: every block landed in exactly one label's ladder.
    EXPECT_EQ(sum, 600);
    EXPECT_EQ(acc.epochBlocks(), 600);
    // An uncheckable block is COUNTED with its rows, never inverted, never
    // calibrated (SPEC 3).  It must not vanish from the denominators, or
    // "uncheckable" would read as "absent".
    EXPECT_EQ(sum_unchk, expect_unchk);
    EXPECT_EQ(acc.epochUncheckable(), expect_unchk);
    EXPECT_EQ(sum_rows, expect_rows);

    // A block whose constellation was never requested enters NOTHING — not even
    // a denominator, because it is not part of this run's population.
    NetworkBlockBaarda::Input alien = nullBlock(rng, 4, 1.0, "E11");
    const NetworkBlockBaarda::BlockStat st = acc.addBlock(alien);
    EXPECT_FALSE(st.accepted);
    EXPECT_EQ(st.family, -1);
    EXPECT_EQ(acc.epochBlocks(), 600);
}

TEST(BlockBaardaSweepTest, PredictThenUpdateNeverLetsABlockStandardizeItself) {
    // The D5 v2 ordering contract.  Epoch 0 has no history, so every block in it
    // is RAW FORMAL (sigma0 == 1, t_B == T_B) however many blocks it carries —
    // the calibration those blocks build is visible only from the NEXT epoch.
    // Reversing the order would let a loud epoch inflate its own sigma0 and
    // standardize itself back to quiet, which is exactly how a self-calibrating
    // detector goes blind.
    Rng rng;
    NetworkBlockBaarda acc({"G"}, 0.0, /*min_rows=*/100.0);
    acc.beginEpoch(0.0, 0.0);
    NetworkBlockBaarda::BlockStat last{};
    for (int i = 0; i < 5000; ++i) last = acc.addBlock(nullBlock(rng, 4, 3.0));
    EXPECT_TRUE(last.raw_formal) << "epoch 0 must not standardize by its own rows";
    EXPECT_DOUBLE_EQ(last.sigma0, 1.0);
    EXPECT_FALSE(acc.sigma0Warm(0));

    acc.beginEpoch(30.0, 30.0);
    EXPECT_TRUE(acc.sigma0Warm(0)) << "epoch 1 must see epoch 0's calibration";
    EXPECT_NEAR(acc.sigma0(0), 3.0, 0.15);
    const NetworkBlockBaarda::BlockStat now = acc.addBlock(nullBlock(rng, 4, 3.0));
    EXPECT_FALSE(now.raw_formal);
    EXPECT_NEAR(now.sigma0, 3.0, 0.15);
}

// ---------------------------------------------------------------------------
//  SPEC 3 — the UNCHECKABLE guard on the cofactor
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, CheckabilityGuardRejectsSingularAndNonFiniteBlocks) {
    bool sing = false, nonfin = false;
    Eigen::MatrixXd good(3, 3);
    good << 4.0, 0.5, 0.1,
            0.5, 3.0, 0.2,
            0.1, 0.2, 2.0;
    EXPECT_TRUE(blockCofactorIsCheckable(good, kBlockCofactorRcondEps, &sing, &nonfin));
    EXPECT_FALSE(sing);
    EXPECT_FALSE(nonfin);

    // A fully-leveraged block — the cofactor collapsed to ~0 — must be REJECTED,
    // not inverted.  This is the block analogue of r < eps, and it is the ONLY
    // signal the consumer gets that the block may never be inverted through.
    EXPECT_FALSE(blockCofactorIsCheckable(Eigen::MatrixXd::Zero(2, 2),
                                          kBlockCofactorRcondEps, &sing, &nonfin));
    EXPECT_TRUE(sing);

    // Rank-deficient but not zero: two rows carrying the SAME information.
    Eigen::MatrixXd dup(2, 2);
    dup << 1.0, 1.0,
           1.0, 1.0;
    EXPECT_FALSE(blockCofactorIsCheckable(dup, kBlockCofactorRcondEps, &sing, &nonfin));
    EXPECT_TRUE(sing);

    // A non-positive row weight puts an infinity on the diagonal ON PURPOSE (the
    // kernel's contract), so the finiteness half catches it separately — a
    // different defect from a rank deficiency, and counted separately.
    Eigen::MatrixXd inf = good;
    inf(1, 1) = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(blockCofactorIsCheckable(inf, kBlockCofactorRcondEps, &sing, &nonfin));
    EXPECT_TRUE(nonfin);

    // A NEGATIVE pivot is a definiteness failure, not a small number: the
    // cofactor of a PSD quantity cannot have one, and its quadratic form would
    // not even be a norm.
    Eigen::MatrixXd indef(2, 2);
    indef << 1.0, 3.0,
             3.0, 1.0;
    EXPECT_FALSE(blockCofactorIsCheckable(indef, kBlockCofactorRcondEps, &sing, &nonfin));
    EXPECT_TRUE(sing);

    EXPECT_FALSE(blockCofactorIsCheckable(Eigen::MatrixXd(), kBlockCofactorRcondEps,
                                          &sing, &nonfin));
}

// ---------------------------------------------------------------------------
//  INC-3 — the smearing fraction, the card's unique deliverable
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, BlockOnlyCountsExactlyTheRowInvisiblePopulation) {
    // SPEC 8.4: block_only / blocks_flagged "decides whether a response card is
    // ever worth writing".  The definition has to be exact — a fraction that
    // quietly counted the wrong population would send that decision the wrong
    // way with nothing to notice it by.
    NetworkBlockBaarda acc({"G"}, 0.0, /*min_rows=*/0.0, /*report_sigma=*/4.0);
    acc.beginEpoch(0.0, 0.0);
    auto block = [&](double T, double max_row_T) {
        NetworkBlockBaarda::Input in;
        in.station = 0; in.sat = "G01"; in.b = 4; in.n_code = 2; in.n_phase = 2;
        in.T = T; in.max_row_T = max_row_T;
        return acc.addBlock(in);
    };
    block(1.0, 0.0);                 // one block so the calibration has a row
    acc.beginEpoch(30.0, 30.0);      // ...and freeze a scale: nothing is raw-formal
    ASSERT_TRUE(acc.sigma0Warm(0));
    const double s2 = acc.sigma0(0) * acc.sigma0(0);

    // LOUD block, every row individually quiet — the smeared fault this card
    // exists to find, and the only population no existing tier can see.
    const auto smeared = block(10.0 * s2, 1.0 * s2);
    EXPECT_TRUE(smeared.reported);
    EXPECT_TRUE(smeared.block_only);
    // LOUD block with a loud row — the row tier already sees this one, so it is
    // NOT part of the invisible population and must not inflate the fraction.
    const auto obvious = block(10.0 * s2, 9.0 * s2);
    EXPECT_TRUE(obvious.reported);
    EXPECT_FALSE(obvious.block_only);
    // QUIET block with a loud row: not reported at all (the row tier's business).
    const auto rowonly = block(1.0 * s2, 9.0 * s2);
    EXPECT_FALSE(rowonly.reported);
    EXPECT_FALSE(rowonly.block_only);
    // Exactly ON the threshold is NOT loud — strict, so a threshold never flags.
    const auto edge = block(4.0 * s2, 0.1 * s2);
    EXPECT_FALSE(edge.reported);

    const NetworkBlockBaarda::Ladder& L =
        acc.epochLadder(0, PriorResidualLabel::Clean);
    EXPECT_EQ(L.n_reported, 2);
    EXPECT_EQ(L.n_block_only, 1);
    EXPECT_DOUBLE_EQ(L.blockOnlyFraction(), 0.5);
    EXPECT_EQ(acc.epochReported(), 2);
    EXPECT_EQ(acc.epochBlockOnly(), 1);
    // The worst block keeps its identity AND whether it was row-invisible: a
    // ladder occupancy nobody can chase back to a satellite is not a finding.
    EXPECT_EQ(L.max_sat, "G01");
    EXPECT_EQ(L.max_b, 4);
    EXPECT_NEAR(L.max_t, 10.0, 1e-9);
    EXPECT_TRUE(L.max_block_only);
}

TEST(BlockBaardaSweepTest, SizeHistogramAndCompositionMakeANullResultReadable) {
    // Invariant #4 in this card's sharpest form: a run whose blocks were ALL
    // b = 1 produces a ladder identical to the scalar w-test and an INC-3
    // fraction of 0 BY CONSTRUCTION.  Without the size histogram that reads as
    // "no smearing found" when what actually happened is "there was never a
    // block to smear across".
    NetworkBlockBaarda acc({"G"}, 0.0, 0.0);
    acc.beginEpoch(0.0, 0.0);
    Rng rng;
    for (int i = 0; i < 5; ++i) {
        auto in = nullBlock(rng, 1, 1.0);
        in.n_phase = 0;
        acc.addBlock(in);
    }
    for (int i = 0; i < 3; ++i) {
        auto in = nullBlock(rng, 4, 1.0);
        in.n_code = 2; in.n_phase = 2;
        acc.addBlock(in);
    }
    {
        auto in = nullBlock(rng, 30, 1.0);
        in.n_code = 0; in.n_phase = 30;
        acc.addBlock(in);
    }
    const auto& h = acc.sizeHistogram();
    EXPECT_EQ(h[0], 5);                        // b = 1
    EXPECT_EQ(h[3], 3);                        // b = 4
    EXPECT_EQ(h[kBlockBaardaMaxB], 1);         // overflow: a b > 8 block is VISIBLE
    EXPECT_EQ(acc.runCodeOnlyBlocks(), 5);
    EXPECT_EQ(acc.runMixedBlocks(), 3);
    EXPECT_EQ(acc.runPhaseOnlyBlocks(), 1);
}

// ---------------------------------------------------------------------------
//  INC-2 — the corroboration join and its b-cell
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, ContingencyBCellIsLoudAndRawFlat) {
    // SPEC 8.3 grades the b-cell: blocks the statistic calls loud AND the raw
    // observables say did NOT move.  Its meaning depends entirely on the join
    // landing in the right cell — and every cell is a different conclusion.
    NetworkBlockBaarda acc({"G"}, 0.0, /*min_rows=*/0.0, /*report_sigma=*/4.0,
                           /*corroborate=*/true);
    acc.setCorroborator([](int /*station*/, std::string_view sat) {
        if (sat == "G01") return PriorResidualCorrob::Flat;
        if (sat == "G02") return PriorResidualCorrob::Stepped;
        return PriorResidualCorrob::NoEvidence;
    });
    acc.beginEpoch(0.0, 0.0);
    auto push = [&](const char* sat, double T) {
        NetworkBlockBaarda::Input in;
        in.station = 0; in.sat = sat; in.b = 2; in.n_code = 1; in.n_phase = 1;
        in.T = T;
        acc.addBlock(in);
    };
    push("G01", 1.0);  push("G02", 1.0);  push("G03", 1.0);   // seed the calibration
    acc.beginEpoch(30.0, 30.0);
    ASSERT_TRUE(acc.sigma0Warm(0));
    const double s2 = acc.sigma0(0) * acc.sigma0(0);
    push("G01", 50.0 * s2);   // loud + raw FLAT     -> THE B-CELL
    push("G01", 50.0 * s2);
    push("G02", 50.0 * s2);   // loud + raw stepped  -> corroborated
    push("G03", 50.0 * s2);   // loud + no evidence  -> unjudgeable
    push("G01", 0.1 * s2);    // quiet + raw flat

    const NetworkBlockBaarda::Ladder& L =
        acc.epochLadder(0, PriorResidualLabel::Clean);
    const auto& c = L.contingency;
    EXPECT_EQ(c[0][static_cast<int>(PriorResidualCorrob::Flat)], 2);        // b-cell
    EXPECT_EQ(c[0][static_cast<int>(PriorResidualCorrob::Stepped)], 1);
    EXPECT_EQ(c[0][static_cast<int>(PriorResidualCorrob::NoEvidence)], 1);
    EXPECT_EQ(c[1][static_cast<int>(PriorResidualCorrob::Flat)], 1);
    EXPECT_EQ(c[1][static_cast<int>(PriorResidualCorrob::Stepped)], 0);
    EXPECT_EQ(c[1][static_cast<int>(PriorResidualCorrob::NoEvidence)], 0);

    // Unarmed corroboration leaves the whole table at ZERO rather than silently
    // filling the no_evidence column — "we did not ask" and "we asked and there
    // was no evidence" are different findings and must not share a cell.
    NetworkBlockBaarda off({"G"}, 0.0, 0.0, 4.0, /*corroborate=*/false);
    off.beginEpoch(0.0, 0.0);
    NetworkBlockBaarda::Input in;
    in.station = 0; in.sat = "G01"; in.b = 2; in.T = 100.0;
    off.addBlock(in);
    const auto& c2 = off.epochLadder(0, PriorResidualLabel::Clean).contingency;
    for (int r = 0; r < 2; ++r)
        for (int k = 0; k < kPriorResidualCorrobs; ++k) EXPECT_EQ(c2[r][k], 0);
}

// ---------------------------------------------------------------------------
//  SPEC 7.5 — OFF is structurally inert
// ---------------------------------------------------------------------------

TEST(BlockBaardaSweepTest, OffIsStructurallyInert) {
    // The defaults are OFF and the master predicate the driver reads is false, so
    // an unarmed run never enters the sweep or the emission translation unit.
    GnssConfig cfg;
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_FALSE(nc.block_baarda);
    EXPECT_FALSE(nc.block_baarda_corroborate);
    EXPECT_DOUBLE_EQ(nc.block_baarda_report_sigma, 4.0);
    EXPECT_EQ(nc.block_baarda_window_epochs, 60);
    EXPECT_EQ(nc.block_baarda_min_blocks, 200);
    EXPECT_EQ(nc.block_baarda_event_holdoff_ep, 20);
    EXPECT_EQ(nc.block_baarda_families, "G,E,R,C");

    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.bb_on);
    EXPECT_FALSE(k.bb_corrob);

    // The master is the AND of the arm and a LIVE HOST: this card has no
    // scheduler of its own, so arming it behind a dead redundancy census must
    // resolve to OFF rather than to a silent zero-block run.  (The loader
    // hard-errors on that config; this is the belt-and-braces half, and it is
    // what protects a hand-built NetworkConfig in a test.)
    {
        NetworkConfig armed = nc;
        armed.block_baarda = true;
        EXPECT_FALSE(NetworkKnobs::snapshot(cfg, armed).bb_on)
            << "no census host means the sweep must not arm";
        armed.redundancy_census_stride = 10;
        const NetworkKnobs k3 = NetworkKnobs::snapshot(cfg, armed);
        EXPECT_TRUE(k3.bb_on);
        EXPECT_FALSE(k3.bb_corrob);
        armed.block_baarda_corroborate = true;
        EXPECT_TRUE(NetworkKnobs::snapshot(cfg, armed).bb_corrob);
    }

    // A default-constructed accumulator is inert: every entry point is one bool
    // test, and an unarmed run allocates nothing beyond the empty vectors.
    NetworkCarryState carry;
    EXPECT_FALSE(carry.bb.enabled());
    EXPECT_TRUE(carry.bb_hits.empty());
    EXPECT_TRUE(carry.bb_event_last.empty());
    // ...and it accepts nothing, so a mis-wired feed site cannot silently
    // populate a ladder nobody asked for.
    NetworkBlockBaarda::Input in;
    in.station = 0; in.sat = "G01"; in.b = 4; in.T = 1e9;
    carry.bb.beginEpoch(0.0, 0.0);
    EXPECT_FALSE(carry.bb.addBlock(in).accepted);
    EXPECT_EQ(carry.bb.epochBlocks(), 0);

    NetworkServiceCounters counters;
    EXPECT_EQ(counters.bb.passes, 0);
    EXPECT_EQ(counters.bb.blocks, 0);
    EXPECT_EQ(counters.bb.reported, 0);
    EXPECT_EQ(counters.bb.block_only, 0);
}


// ---------------------------------------------------------------------------
//  SPEC 6 — the loader tripwires
//
//  Both are HARD ERRORS, not silent disarms, and that choice is the whole point:
//  a config that asks for this card and gets an empty run is the exact
//  "live counter over dead wiring" failure the project keeps paying for.  A
//  silent disarm would produce a clean summary indistinguishable from "this
//  network has no smeared faults".
// ---------------------------------------------------------------------------

namespace {

namespace bbfs = std::filesystem;

bbfs::path writeBbConfig(const std::string& tag, const std::string& body) {
    const bbfs::path p = bbfs::temp_directory_path() / ("blockbaarda_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config with @p extra spliced into the
/// network block.
std::string bbConfig(const std::string& extra) {
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

bool bbLoads(const std::string& tag, const std::string& extra, GnssConfig& cfg) {
    LogManager log;
    const bbfs::path p = writeBbConfig(tag, bbConfig(extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    bbfs::remove(p, ec);
    return ok;
}

bool bbLoadFails(const std::string& tag, const std::string& extra) {
    GnssConfig cfg;
    return !bbLoads(tag, extra, cfg);
}

}  // namespace

TEST(BlockBaardaSweepTest, LoaderHardErrorsWithoutASweepHost) {
    // THE tripwire.  This card has no scheduler by design (SPEC 4) — it rides the
    // redundancy census's swept stations.  Armed with that census off it would
    // enumerate zero blocks and print a clean summary, so the loader refuses.
    EXPECT_TRUE(bbLoadFails("nohost", "    \"block_baarda\": true,\n"));

    // Either half of the census predicate is a live host: there is no
    // `network.redundancy_census` bool, and a check that tested only the stride
    // would reject a perfectly valid escalation-only lineage.
    {
        GnssConfig cfg;
        ASSERT_TRUE(bbLoads("stride", "    \"block_baarda\": true,\n"
                                      "    \"redundancy_census_stride\": 10,\n", cfg));
        EXPECT_TRUE(cfg.getNetworkConfig().block_baarda);
    }
    {
        GnssConfig cfg;
        ASSERT_TRUE(bbLoads("escal", "    \"block_baarda\": true,\n"
                                     "    \"redundancy_escalate_sigma\": 1000.0,\n", cfg));
        EXPECT_TRUE(cfg.getNetworkConfig().block_baarda);
    }
}

TEST(BlockBaardaSweepTest, LoaderHardErrorsOnCorroborationWithoutTheCard) {
    // The join runs INSIDE the sweep, so arming it alone produces no blocks, no
    // contingency table and no warning at run time.
    EXPECT_TRUE(bbLoadFails("corrob_alone",
                            "    \"block_baarda_corroborate\": true,\n"));
    // An empty family list would drop every block for want of a family and
    // report zero with no warning — the same failure by a different route.
    EXPECT_TRUE(bbLoadFails("nofam", "    \"block_baarda\": true,\n"
                                     "    \"redundancy_census_stride\": 10,\n"
                                     "    \"block_baarda_families\": \"\",\n"));
}

TEST(BlockBaardaSweepTest, LoaderAutoArmsGapInstrumentForTheCorroborationJoin) {
    // INC-2 reads the raw MW/GF step verdicts, and nothing else in a bare config
    // computes them — without gap_instrument every contingency cell would land in
    // 'no_evidence' and SPEC 8.3's b-cell would be structurally unmeasurable.
    // Deliberately NOT mw_lli_corroborate: that is a DECISION card and would
    // perturb the very population being measured.
    GnssConfig cfg;
    ASSERT_TRUE(bbLoads("autoarm", "    \"block_baarda\": true,\n"
                                   "    \"block_baarda_corroborate\": true,\n"
                                   "    \"redundancy_census_stride\": 10,\n", cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_TRUE(nc.block_baarda_corroborate);
    EXPECT_TRUE(nc.slip_qc.gap_instrument)
        << "the corroboration join must auto-arm its own evidence source";
    EXPECT_FALSE(nc.slip_qc.lli_corroborate)
        << "a DECISION card must never be auto-armed by a measure-only census";

    // ...and it does NOT arm it when the join is off: this card buys nothing it
    // does not need.
    GnssConfig plain;
    ASSERT_TRUE(bbLoads("noautoarm", "    \"block_baarda\": true,\n"
                                     "    \"redundancy_census_stride\": 10,\n", plain));
    EXPECT_FALSE(plain.getNetworkConfig().slip_qc.gap_instrument);
}

TEST(BlockBaardaSweepTest, LoaderRestoresAPathologicalReportThreshold) {
    // A non-positive threshold would trace nothing and leave INC-3's fraction
    // undefined, so it is repaired with a WARN rather than obeyed.
    GnssConfig cfg;
    ASSERT_TRUE(bbLoads("badsigma", "    \"block_baarda\": true,\n"
                                    "    \"redundancy_census_stride\": 10,\n"
                                    "    \"block_baarda_report_sigma\": -1.0,\n", cfg));
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().block_baarda_report_sigma, 4.0);
}

}  // namespace GnssProcessor
