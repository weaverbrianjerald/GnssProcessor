/**
 * @file test_carry_aware_covariance.cpp
 * @brief Gates for the carry-aware (rank-K) covariance readback
 *        (docs/SPEC_carry_aware_covariance.md).
 *
 * @c foldReducedDdPriors is a per-block rank-K Woodbury update on the pristine per-station
 * factors, i.e. exactly the Schur complement of
 * @f$\tilde A^{-1}=N_{rr}^{-1}-\sum_b Y_bM_bY_b^\top@f$.  @c reconstructStation reads that
 * model back on the mean.  The carry-blind covariance readbacks use @f$N_{rr}^{-1}@f$,
 * while @c NetworkEstimator's shared covariance, inverted from the folded reduced normals,
 * is carry-aware.  A σ published from the blind readbacks is therefore a hybrid: the
 * covariance of no estimator.  The carry-aware readbacks use @f$\tilde A^{-1}@f$.
 *
 * Suite name @c CarryAwareCovarianceTest is owned solely by this file (suite-hygiene rule:
 * one suite, one file).  The config-knob default-off gate lives with the other network
 * config cases in tests/test_codebias_network_config.cpp for the same reason.
 *
 * The oracle throughout is an independent dense augmented solve (build the full bordered
 * normal matrix, add @f$C^\top (P^z)^{-1} C@f$, invert, and read the block wanted), the same
 * reference @c FoldReducedDdPriorsMatchesDenseAugmentedSolve uses for the mean.
 *
 * The gates, in SPEC §7 order:
 *  1. @c LocalCovarianceMatchesDenseAugmentedSolve: all four readbacks equal the dense
 *     augmented inverse, and the blind path keeps failing (the falsifier).
 *  2. @c CrossStationLocalTermIsRecovered: the term @f$N_{rr}^{-1}@f$ cannot express.
 *  3. @c MatureCarryCollapsesDdSigma: the quantitative statement of the defect.
 *  4. @c OverSubtractionIsRefusedAndCounted: §5b, and the fold's cross-block error.
 *  5. @c DegenerateBlockContributesNothing: the inert fold carries through with no
 *     special case.
 *  6. @c BlindPathIsIdenticalFoldOrNot: invariant #6 and the defect, in one assertion.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <random>
#include <vector>

#include "filter/NetworkNormalEquations.hpp"

namespace GnssProcessor {
namespace {

using Term = NetworkNormalEquations::DdPriorBlock::Term;

// ---------------------------------------------------------------------------
// Local fixture: a small bordered network, built twice, once densely (the
// independent oracle) and once through the Schur kernel under test.
// ---------------------------------------------------------------------------
struct StationData {
    Eigen::MatrixXd A_s, A_r, W;
    Eigen::VectorXd y;
};

Eigen::MatrixXd makeRand(std::mt19937& gen, Eigen::Index r, Eigen::Index c) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    Eigen::MatrixXd A(r, c);
    for (Eigen::Index i = 0; i < r; ++i)
        for (Eigen::Index j = 0; j < c; ++j) A(i, j) = dist(gen);
    return A;
}

std::vector<StationData> makeStations(std::mt19937& gen, Eigen::Index n_shared,
                                      const std::vector<Eigen::Index>& n_locals,
                                      const std::vector<Eigen::Index>& m_obs) {
    std::vector<StationData> out;
    for (std::size_t r = 0; r < n_locals.size(); ++r) {
        StationData s;
        const Eigen::Index m = m_obs[r];
        s.A_s = makeRand(gen, m, n_shared);
        s.A_r = makeRand(gen, m, n_locals[r]);
        s.y   = makeRand(gen, m, 1);
        s.W   = Eigen::MatrixXd(Eigen::VectorXd::LinSpaced(m, 0.5, 2.0).asDiagonal());
        out.push_back(std::move(s));
    }
    return out;
}

constexpr double kSharedPrior = 1e-2;
constexpr double kLocalPrior  = 1e-2;

/// The dense bordered system: x = [x_s | x_0 | x_1 | …].
struct Dense {
    Eigen::MatrixXd N;                        ///< full bordered normals (+ isotropic priors)
    Eigen::MatrixXd N_sr_full;                ///< n_shared × P_local border, all stations
    Eigen::MatrixXd N_local;                  ///< blockdiag(N_rr) over stations
    std::vector<Eigen::Index> off;            ///< each station's column offset in N
    Eigen::Index n_shared{0}, n_local_all{0};
};

Dense denseBuild(const std::vector<StationData>& st, Eigen::Index n_shared) {
    Eigen::Index M = 0, P = n_shared;
    std::vector<Eigen::Index> off, mrows;
    for (const auto& s : st) {
        off.push_back(P);
        P += s.A_r.cols();
        mrows.push_back(s.A_s.rows());
        M += s.A_s.rows();
    }
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(M, P);
    Eigen::MatrixXd W = Eigen::MatrixXd::Zero(M, M);
    Eigen::Index row = 0;
    for (std::size_t r = 0; r < st.size(); ++r) {
        const Eigen::Index m = mrows[r];
        A.block(row, 0, m, n_shared) = st[r].A_s;
        A.block(row, off[r], m, st[r].A_r.cols()) = st[r].A_r;
        W.block(row, row, m, m) = st[r].W;
        row += m;
    }
    Dense d;
    d.N = A.transpose() * W * A;
    d.N.topLeftCorner(n_shared, n_shared) +=
        kSharedPrior * Eigen::MatrixXd::Identity(n_shared, n_shared);
    for (std::size_t r = 0; r < st.size(); ++r) {
        const Eigen::Index n_r = st[r].A_r.cols();
        d.N.block(off[r], off[r], n_r, n_r) +=
            kLocalPrior * Eigen::MatrixXd::Identity(n_r, n_r);
    }
    d.off = off;
    d.n_shared = n_shared;
    d.n_local_all = P - n_shared;
    d.N_sr_full = d.N.topRightCorner(n_shared, d.n_local_all);
    d.N_local = d.N.bottomRightCorner(d.n_local_all, d.n_local_all);
    // The kernel's N_rr is per-station; the dense N_local's off-diagonal station blocks are
    // zero by construction (stations share no observation), which is what makes
    // blockdiag(N_rr) and this bottom-right corner the same matrix.
    return d;
}

/// Feed the same raw data + priors into the Schur kernel.
void schurBuild(NetworkNormalEquations& neq, const std::vector<StationData>& st,
                Eigen::Index n_shared) {
    neq.addSharedInfo(kSharedPrior * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : st) {
        const Eigen::MatrixXd WA_s = s.W * s.A_s, WA_r = s.W * s.A_r;
        const Eigen::Index n_r = s.A_r.cols();
        Eigen::MatrixXd N_rr =
            s.A_r.transpose() * WA_r + kLocalPrior * Eigen::MatrixXd::Identity(n_r, n_r);
        neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                       WA_s.transpose() * s.y, WA_r.transpose() * s.y);
    }
}

/// C (K × P) in the dense layout, from a block's (station, local_col, coeff) rows.
Eigen::MatrixXd denseC(const NetworkNormalEquations::DdPriorBlock& blk, const Dense& d,
                       Eigen::Index P) {
    const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
    Eigen::MatrixXd C = Eigen::MatrixXd::Zero(K, P);
    for (Eigen::Index k = 0; k < K; ++k)
        for (const auto& t : blk.rows[static_cast<std::size_t>(k)])
            C(k, d.off[t.station] + t.local_col) += t.coeff;
    return C;
}

/// A three-station block: one chord spans stations 0 and 1, a second chord stations 1 and
/// 2.  This is the shape a field DD carry has: one block, several stations.
NetworkNormalEquations::DdPriorBlock makeBlock(double p00, double p11) {
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = {
        {Term{0, 0, +1.0}, Term{1, 0, -1.0}, Term{1, 1, +1.0}, Term{0, 1, -1.0}},
        {Term{1, 0, +1.0}, Term{2, 0, -1.0}},
    };
    blk.Pz = (Eigen::MatrixXd(2, 2) << p00, 0.0, 0.0, p11).finished();
    blk.zhat = (Eigen::VectorXd(2) << 1.5, -0.7).finished();
    return blk;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Gate 1 (SPEC §7.1): all four readbacks equal the dense augmented inverse when armed,
// and the blind path does not.  The second half is the falsifier: if the carry-blind form
// starts matching, this fixture no longer models the defect and is re-derived.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, LocalCovarianceMatchesDenseAugmentedSolve) {
    std::mt19937 gen(20260803);
    const Eigen::Index n_shared = 4;
    const auto st = makeStations(gen, n_shared, {3, 3, 3}, {10, 11, 12});
    const Dense d = denseBuild(st, n_shared);
    const Eigen::Index P = d.N.rows();

    const auto blk = makeBlock(0.04, 0.09);
    const Eigen::MatrixXd C = denseC(blk, d, P);
    const Eigen::MatrixXd PzInv = blk.Pz.inverse();
    const Eigen::MatrixXd Sigma_aug = (d.N + C.transpose() * PzInv * C).inverse();
    const Eigen::MatrixXd cov_free = Sigma_aug.topLeftCorner(n_shared, n_shared);
    // The purely local augmented inverse: the frame conditional on the whole border that
    // stationRegression reports (Ã⁻¹, no Σ_ss propagation).
    const Eigen::MatrixXd C_loc = C.rightCols(d.n_local_all);
    const Eigen::MatrixXd A_loc_inv =
        (d.N_local + C_loc.transpose() * PzInv * C_loc).inverse();

    NetworkNormalEquations neq(n_shared);
    schurBuild(neq, st, n_shared);
    neq.reduce();
    neq.foldReducedDdPriors({blk});
    ASSERT_EQ(neq.numDdPriorBlocks(), 1u);
    ASSERT_TRUE(neq.hasCarryCovCorrection());

    for (std::size_t r = 0; r < st.size(); ++r) {
        const Eigen::Index n_r = st[r].A_r.cols();
        const Eigen::MatrixXd ref = Sigma_aug.block(d.off[r], d.off[r], n_r, n_r);
        const Eigen::MatrixXd aware = neq.reconstructStationCovariance(r, cov_free, true);
        const Eigen::MatrixXd blind = neq.reconstructStationCovariance(r, cov_free, false);
        EXPECT_TRUE(aware.isApprox(ref, 1e-9))
            << "station " << r << " carry-aware\n" << aware << "\nref\n" << ref;
        EXPECT_FALSE(blind.isApprox(ref, 1e-6))
            << "station " << r << ": the CARRY-BLIND covariance matched the augmented "
               "inverse — the fixture no longer reproduces the defect (SPEC §7.1)";
        // The direction: the carry only adds information to the local block.
        for (Eigen::Index c = 0; c < n_r; ++c)
            EXPECT_LT(aware(c, c), blind(c, c)) << "station " << r << " col " << c;
    }

    // Diagonal form, same numbers, cheap path.
    for (std::size_t r = 0; r < st.size(); ++r) {
        const Eigen::Index n_r = st[r].A_r.cols();
        std::vector<Eigen::Index> cols;
        for (Eigen::Index c = 0; c < n_r; ++c) cols.push_back(c);
        const Eigen::VectorXd aware =
            neq.reconstructStationCovarianceDiag(r, cols, cov_free, true);
        for (Eigen::Index c = 0; c < n_r; ++c)
            EXPECT_NEAR(aware(c), Sigma_aug(d.off[r] + c, d.off[r] + c),
                        1e-9 * std::abs(Sigma_aug(d.off[r] + c, d.off[r] + c)) + 1e-12);
    }

    // Joint form, including cross-station entries.
    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {
        {0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}};
    const Eigen::MatrixXd J = neq.jointConditionalCovariance(reqs, cov_free, true);
    ASSERT_EQ(J.rows(), static_cast<Eigen::Index>(reqs.size()));
    for (std::size_t a = 0; a < reqs.size(); ++a)
        for (std::size_t b = 0; b < reqs.size(); ++b) {
            const double ref = Sigma_aug(d.off[reqs[a].first] + reqs[a].second,
                                         d.off[reqs[b].first] + reqs[b].second);
            EXPECT_NEAR(J(a, b), ref, 1e-9 * std::abs(ref) + 1e-12)
                << "joint(" << a << "," << b << ")";
        }

    // Regression form: cond_var = (Ã⁻¹)_rr and row = N_sr Ã⁻¹ e_r.  The row picks up other
    // stations' locals through the chord, which the blind form cannot.
    for (std::size_t r = 0; r < st.size(); ++r) {
        const auto reg = neq.stationRegression(r, 0, true);
        const Eigen::Index g = d.off[r] - n_shared;   // column in the local-only layout
        EXPECT_NEAR(reg.cond_var, A_loc_inv(g, g), 1e-9 * std::abs(A_loc_inv(g, g)) + 1e-12);
        const Eigen::VectorXd row_ref = d.N_sr_full * A_loc_inv.col(g);
        EXPECT_TRUE(reg.row.isApprox(row_ref, 1e-9))
            << "station " << r << " regression row\n" << reg.row.transpose()
            << "\nref\n" << row_ref.transpose();
    }
}

// ---------------------------------------------------------------------------------------
// Gate 2 (SPEC §7.2): the cross-station local term.  N_rr⁻¹ is block-diagonal, so the
// blind form can only couple two stations through Σ_ss; Ã⁻¹ is not block-diagonal, and a
// DD chord spans two stations by construction.  A between-station DD σ is meaningful only
// with this term, and jointConditionalCovariance's "same local space only" branch
// (NetworkNormalEquations.cpp) cannot express it.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, CrossStationLocalTermIsRecovered) {
    std::mt19937 gen(20260804);
    const Eigen::Index n_shared = 3;
    const auto st = makeStations(gen, n_shared, {2, 2, 2}, {9, 9, 9});
    const Dense d = denseBuild(st, n_shared);

    const auto blk = makeBlock(1e-4, 1e-4);   // a mature carry: the chords are tight
    const Eigen::MatrixXd C = denseC(blk, d, d.N.rows());
    const Eigen::MatrixXd Sigma_aug =
        (d.N + C.transpose() * blk.Pz.inverse() * C).inverse();
    const Eigen::MatrixXd cov_free = Sigma_aug.topLeftCorner(n_shared, n_shared);

    NetworkNormalEquations neq(n_shared);
    schurBuild(neq, st, n_shared);
    neq.reduce();
    neq.foldReducedDdPriors({blk});

    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {{0, 0}, {1, 0}};
    const Eigen::MatrixXd blind = neq.jointConditionalCovariance(reqs, cov_free, false);
    const Eigen::MatrixXd aware = neq.jointConditionalCovariance(reqs, cov_free, true);

    const double ref01 = Sigma_aug(d.off[0] + 0, d.off[1] + 0);
    EXPECT_NEAR(aware(0, 1), ref01, 1e-9 * std::abs(ref01) + 1e-12);
    // The blind form is not merely less accurate here: it lacks a whole term, and a tight
    // chord makes the two stations strongly correlated.
    EXPECT_GT(std::abs(aware(0, 1) - blind(0, 1)), 1e-3 * std::abs(ref01));
    EXPECT_FALSE(blind.isApprox(aware, 1e-6));
}

// ---------------------------------------------------------------------------------------
// Gate 3 (SPEC §7.3): the quantitative statement of the defect.  A DD whose chord the
// carry holds at 1e-8 m² reads as tight; the blind form reports the epoch-local float
// uncertainty instead, so a dd_sigma_cyc taken from it stays at the float level however
// mature the carry.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, MatureCarryCollapsesDdSigma) {
    std::mt19937 gen(20260805);
    const Eigen::Index n_shared = 3;
    const auto st = makeStations(gen, n_shared, {2, 2}, {9, 9});
    const Dense d = denseBuild(st, n_shared);

    // K=1: z = x_{0,0} − x_{1,0}, held at σ² = 1e-8 m².
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = {{Term{0, 0, +1.0}, Term{1, 0, -1.0}}};
    blk.Pz = (Eigen::MatrixXd(1, 1) << 1e-8).finished();
    blk.zhat = (Eigen::VectorXd(1) << 0.25).finished();

    const Eigen::MatrixXd C = denseC(blk, d, d.N.rows());
    const Eigen::MatrixXd Sigma_aug =
        (d.N + C.transpose() * blk.Pz.inverse() * C).inverse();
    const Eigen::MatrixXd cov_free = Sigma_aug.topLeftCorner(n_shared, n_shared);

    NetworkNormalEquations neq(n_shared);
    schurBuild(neq, st, n_shared);
    neq.reduce();
    neq.foldReducedDdPriors({blk});

    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {{0, 0}, {1, 0}};
    const Eigen::Vector2d g(+1.0, -1.0);   // the DD combination
    const double var_blind = g.dot(neq.jointConditionalCovariance(reqs, cov_free, false) * g);
    const double var_aware = g.dot(neq.jointConditionalCovariance(reqs, cov_free, true) * g);

    EXPECT_GT(var_blind, 0.0);
    EXPECT_GT(var_aware, 0.0);
    // The carried variance is the ceiling: folding data can only tighten it further.
    EXPECT_LE(var_aware, blk.Pz(0, 0) * (1.0 + 1e-9));
    // The blind reading is orders of magnitude above it: the defect, as a number.
    EXPECT_LT(var_aware, 1e-3 * var_blind)
        << "carry-aware DD variance " << var_aware << " vs blind " << var_blind;
}

// ---------------------------------------------------------------------------------------
// Gate 4 (SPEC §7.4 / §3): the fold composes one Woodbury update per block against the
// pristine N_rr⁻¹, which is exact only when no station is touched by two blocks.  Two
// blocks carrying the same tight chord are the extreme case: each subtracts nearly the
// whole information, so the sum over-subtracts and Ã⁻¹ leaves the PSD cone.  The readback
// refuses (never clamps), falls back, and counts; the counter is the only measurement of
// the fold's cross-block approximation error.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, OverSubtractionIsRefusedAndCounted) {
    std::mt19937 gen(20260806);
    const Eigen::Index n_shared = 3;
    const auto st = makeStations(gen, n_shared, {2, 2}, {9, 9});
    const Dense d = denseBuild(st, n_shared);

    NetworkNormalEquations::DdPriorBlock b1;
    b1.rows = {{Term{0, 0, +1.0}, Term{1, 0, -1.0}}};
    b1.Pz = (Eigen::MatrixXd(1, 1) << 1e-12).finished();
    b1.zhat = (Eigen::VectorXd(1) << 0.0).finished();
    NetworkNormalEquations::DdPriorBlock b2 = b1;   // the same chord, a second time

    NetworkNormalEquations neq(n_shared);
    schurBuild(neq, st, n_shared);
    neq.reduce();
    neq.foldReducedDdPriors({b1, b2});
    ASSERT_EQ(neq.numDdPriorBlocks(), 2u);
    neq.resetCarryCovStats();

    const Eigen::MatrixXd cov_free = Eigen::MatrixXd::Zero(n_shared, n_shared);
    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {{0, 0}, {1, 0}};
    const Eigen::MatrixXd blind = neq.jointConditionalCovariance(reqs, cov_free, false);
    const Eigen::MatrixXd aware = neq.jointConditionalCovariance(reqs, cov_free, true);

    const auto& s = neq.carryCovStats();
    EXPECT_GT(s.nonpos_rows, 0) << "the duplicated tight chord did not over-subtract — "
                                   "re-derive the fixture, the guard is untested";
    EXPECT_EQ(s.refused_calls, 1);
    // Refused means the blind matrix comes back untouched, not a clamped hybrid.
    EXPECT_TRUE(aware.isApprox(blind, 0.0));
    // No consumer is ever handed a non-positive variance.
    for (Eigen::Index i = 0; i < aware.rows(); ++i) EXPECT_GT(aware(i, i), 0.0);

    // The per-column form refuses per column and keeps the blind value there.  Which
    // column refuses is a property of the fixture, not of the guard: with the two
    // duplicated blocks the downdate on station r's column is 2a²/(Pz+a+b) (a, b the two
    // stations' (N_rr⁻¹)_00), so it exceeds the blind a exactly on the station with the
    // larger (N_rr⁻¹)_00.  The test asserts over both rather than pinning which one.
    neq.resetCarryCovStats();
    const std::vector<Eigen::Index> cols = {0};
    long long refused = 0;
    for (std::size_t r = 0; r < st.size(); ++r) {
        const Eigen::VectorXd db =
            neq.reconstructStationCovarianceDiag(r, cols, cov_free, false);
        const long long before = neq.carryCovStats().nonpos_rows;
        const Eigen::VectorXd da =
            neq.reconstructStationCovarianceDiag(r, cols, cov_free, true);
        const bool refused_here = neq.carryCovStats().nonpos_rows > before;
        EXPECT_GT(da(0), 0.0) << "station " << r;   // never a non-positive variance, ever
        if (refused_here) {
            ++refused;
            EXPECT_EQ(da(0), db(0)) << "station " << r << ": a refused column must keep the "
                                       "BLIND value, not a clamped one";
        } else {
            EXPECT_LT(da(0), db(0)) << "station " << r;   // accepted ⇒ information was added
        }
    }
    EXPECT_GT(refused, 0) << "no column over-subtracted — re-derive the fixture, the "
                             "per-column guard is untested";
}

// ---------------------------------------------------------------------------------------
// Gate 5 (SPEC §7.5): a degenerate block folded inertly (M = 0, the parent card's INC-0)
// contributes nothing to the correction, with no special case in the readback.  The
// inert-fold semantics (posterior == the untouched prior) carry through unchanged.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, DegenerateBlockContributesNothing) {
    std::mt19937 gen(20260807);
    const Eigen::Index n_shared = 3;
    const auto st = makeStations(gen, n_shared, {2, 2, 2}, {9, 9, 9});
    const Dense d = denseBuild(st, n_shared);
    (void)d;

    auto bad = makeBlock(0.04, 0.09);
    // Far from SPD: Pᶻ+B has to stay indefinite after the data information B is added,
    // or the screen legitimately passes and the fixture tests nothing (−1.0 is not enough
    // at this fixture's weighting; B covers it).
    bad.Pz(0, 0) = -1e6;   // ⇒ the PD screen trips ⇒ M = 0 ⇒ the fold is inert

    NetworkNormalEquations neq(n_shared);
    schurBuild(neq, st, n_shared);
    neq.reduce();
    neq.foldReducedDdPriors({bad});
    ASSERT_TRUE(neq.ddPriorBlockDegenerate(0));
    neq.resetCarryCovStats();

    const Eigen::MatrixXd cov_free =
        0.5 * Eigen::MatrixXd::Identity(n_shared, n_shared);
    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {{0, 0}, {1, 0}, {2, 0}};
    const Eigen::MatrixXd blind = neq.jointConditionalCovariance(reqs, cov_free, false);
    const Eigen::MatrixXd aware = neq.jointConditionalCovariance(reqs, cov_free, true);
    // isApprox, not bitwise: the correction is exactly zero, but the armed body evaluates
    // (Nrr⁻¹ − 0) + (M − 0)Σ(M − 0)ᵀ as two statements where the blind body is one fused
    // expression, so the two may differ in the last ulp.  The model is identical.
    EXPECT_TRUE(aware.isApprox(blind, 1e-14)) << aware << "\nvs\n" << blind;
    EXPECT_EQ(neq.carryCovStats().nonpos_rows, 0);
    // The correction ran (the block exists) and moved nothing: the one case where a
    // ratio of exactly 1 is the correct reading rather than a sign of inert wiring.
    EXPECT_EQ(neq.carryCovStats().calls, 1);
    EXPECT_NEAR(neq.carryCovStats().ratio_min, 1.0, 1e-12);
}

// ---------------------------------------------------------------------------------------
// Gate 6 (SPEC §7.6): invariant #6 and the defect statement, in one assertion.  The blind
// readback does not depend on the fold at all: with or without the DD priors folded, each
// of the four returns bit-for-bit the same matrix.  That is both (a) the structural
// off-path guarantee the card needs and (b) the plainest statement of what "carry-blind"
// means: the covariance cannot see the carry even in principle.
// ---------------------------------------------------------------------------------------
TEST(CarryAwareCovarianceTest, BlindPathIsIdenticalFoldOrNot) {
    std::mt19937 gen(20260808);
    const Eigen::Index n_shared = 4;
    const auto st = makeStations(gen, n_shared, {3, 3, 3}, {10, 11, 12});
    const auto blk = makeBlock(0.04, 0.09);
    const Eigen::MatrixXd cov_free =
        0.25 * Eigen::MatrixXd::Identity(n_shared, n_shared);

    NetworkNormalEquations folded(n_shared), unfolded(n_shared);
    schurBuild(folded, st, n_shared);
    schurBuild(unfolded, st, n_shared);
    folded.reduce();
    folded.foldReducedDdPriors({blk});
    unfolded.reduce();
    ASSERT_TRUE(folded.hasCarryCovCorrection());
    ASSERT_FALSE(unfolded.hasCarryCovCorrection());
    folded.resetCarryCovStats();

    const std::vector<std::pair<std::size_t, Eigen::Index>> reqs = {
        {0, 0}, {1, 0}, {1, 1}, {2, 0}};
    auto exact = [](const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
        return a.rows() == b.rows() && a.cols() == b.cols()
               && (a - b).cwiseAbs().maxCoeff() == 0.0;
    };
    EXPECT_TRUE(exact(folded.jointConditionalCovariance(reqs, cov_free),
                      unfolded.jointConditionalCovariance(reqs, cov_free)));
    for (std::size_t r = 0; r < st.size(); ++r) {
        EXPECT_TRUE(exact(folded.reconstructStationCovariance(r, cov_free),
                          unfolded.reconstructStationCovariance(r, cov_free)))
            << "station " << r;
        const std::vector<Eigen::Index> cols = {0, 1};
        EXPECT_TRUE(exact(folded.reconstructStationCovarianceDiag(r, cols, cov_free),
                          unfolded.reconstructStationCovarianceDiag(r, cols, cov_free)))
            << "station " << r;
        const auto rf = folded.stationRegression(r, 0);
        const auto ru = unfolded.stationRegression(r, 0);
        EXPECT_EQ(rf.cond_var, ru.cond_var) << "station " << r;
        EXPECT_TRUE(exact(rf.row, ru.row)) << "station " << r;
    }
    // A blind call does not touch the telemetry either: an instrument that counts work it
    // did not do would misreport the carry-aware path (invariant #4).
    EXPECT_EQ(folded.carryCovStats().calls, 0);
    EXPECT_EQ(folded.carryCovStats().rows, 0);
}

}  // namespace GnssProcessor
