/**
 * @file test_redundancy_census.cpp
 * @brief The redundancy / testability census (docs/SPEC_redundancy_census.md): the
 *        statistical monitor for parked bad measurements.
 *
 * A measurement can be arbitrarily wrong and at the same time invisible to every
 * residual test.  In the era-2 discharge a code row sat at 2.6·10⁵σ for several
 * epochs while every post-fit statistic read clean, because the deweight gate had
 * cut the station's SH ties and left one observation feeding one otherwise
 * unconstrained slant state.  Such a row has local redundancy r ≈ 0, and its
 * Baarda minimal detectable bias δ₀σ/√r → ∞: no residual-based test can see it,
 * however large it gets.
 *
 * The tests therefore target the four claims the card makes, each of which fails
 * silently if it is wrong:
 *
 *   1. r_i is the Baarda number 1 − w·(HΣHᵀ)_ii, checked against an independent
 *      dense oracle rather than the kernel's own reduction.  A wrong leverage path
 *      still produces plausible numbers in [0,1].
 *   2. r ≈ 0 means "uncheckable", and coupling a second row to the same state
 *      raises it.  This is the physical claim; if it does not hold on a hand-built
 *      system it holds nowhere.
 *   3. Σ(1−r) ≤ n_free, the closure bound the run-end line reports.  It is the
 *      sweep's only self-check.
 *   4. The cross-flag needs both halves.  Uncheckable-and-quiet is a healthy
 *      single-frequency riser and is never evented (the prefer-fallback
 *      directive); large-and-checkable is a storm extreme the network already
 *      localizes.  A cross-flag reduced to either half alone would look like a
 *      working alarm until it fired on real data.
 *
 * Also covered: the mechanisms that keep the instrument affordable and correct,
 * namely the stride/escalation gate, the common-mode-removed magnitude metric (a
 * receiver clock step cancels, a single parked cell does not), the pivot census's
 * un-permutation (Eigen LDLT pivots symmetrically, so a naive vectorD() would name
 * the wrong column), and structural inertness when off.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRedundancyCensus.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "filter/NetworkRunTypes.hpp"

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
//  Dense oracle: build the full bordered normal matrix from raw design rows and
//  invert it.  Independent of the Schur kernel by design, so agreement is a
//  cross-check of the marginalization identities, not a tautology.
// ---------------------------------------------------------------------------
struct DenseSystem {
    Eigen::MatrixXd A;     // m x (n_shared + Σ n_local), rows stacked
    Eigen::VectorXd w;     // per-row information weight
    Eigen::MatrixXd cov;   // (AᵀWA)⁻¹
};

/// r_i = 1 − w_i a_iᵀ Cov a_i from the dense system.
Eigen::VectorXd denseRowRedundancy(const DenseSystem& d) {
    Eigen::VectorXd r(d.A.rows());
    for (Eigen::Index i = 0; i < d.A.rows(); ++i) {
        const Eigen::VectorXd a = d.A.row(i).transpose();
        r(i) = 1.0 - d.w(i) * a.dot(d.cov * a);
    }
    return r;
}

/// Assemble AᵀWA and invert it.
void finishDense(DenseSystem& d) {
    Eigen::MatrixXd N = d.A.transpose() * d.w.asDiagonal() * d.A;
    d.cov = N.inverse();
}

/// A minimal StationAssembly carrying just a row stack (what the census reads).
StationAssembly makeAssembly(const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                             const Eigen::VectorXd& w, const Eigen::VectorXd& y) {
    StationAssembly R;
    R.ok = true;
    R.A_s = A_s;
    R.A_r = A_r;
    R.w = w;
    R.y = y;
    return R;
}

}  // namespace

// ===========================================================================
//  1. The Baarda number is the real one (dense oracle)
// ===========================================================================

TEST(RedundancyCensusTest, RowRedundancyMatchesDenseHatMatrix_TwoStations) {
    // Two stations, 2 shared columns, 2 local columns each, 4 rows each.  The
    // shared border couples them; the local blocks are private.
    constexpr Eigen::Index ns = 2, nl = 2, m = 4;
    NetworkNormalEquations neq(ns);

    Eigen::MatrixXd As[2], Ar[2];
    Eigen::VectorXd w[2], y[2];
    As[0].resize(m, ns); As[0] << 1.0, 0.0,
                                  0.0, 1.0,
                                  1.0, 1.0,
                                  0.5, -0.3;
    Ar[0].resize(m, nl); Ar[0] << 1.0, 0.0,
                                  1.0, 0.2,
                                  0.0, 1.0,
                                  0.4, 0.7;
    As[1].resize(m, ns); As[1] << 1.0, 0.5,
                                  -0.2, 1.0,
                                  0.9, -0.1,
                                  0.3, 0.3;
    Ar[1].resize(m, nl); Ar[1] << 1.0, 0.1,
                                  0.0, 1.0,
                                  0.6, 0.6,
                                  1.0, -0.4;
    w[0].resize(m); w[0] << 1.0, 2.0, 0.5, 4.0;
    w[1].resize(m); w[1] << 3.0, 1.0, 1.5, 0.8;
    y[0] = Eigen::VectorXd::Zero(m);
    y[1] = Eigen::VectorXd::Zero(m);

    // Kernel: the arrowhead path.
    std::size_t slot[2];
    for (int k = 0; k < 2; ++k) {
        const Eigen::MatrixXd W = w[k].asDiagonal();
        slot[k] = neq.addStation(As[k].transpose() * W * As[k],
                                 As[k].transpose() * W * Ar[k],
                                 Ar[k].transpose() * W * Ar[k],
                                 As[k].transpose() * W * y[k],
                                 Ar[k].transpose() * W * y[k]);
    }
    neq.reduce();
    Eigen::VectorXd x_s;
    ASSERT_NO_THROW(x_s = neq.solveShared());

    // Dense oracle: one stacked system over [shared | local0 | local1].
    DenseSystem d;
    const Eigen::Index n_tot = ns + 2 * nl;
    d.A = Eigen::MatrixXd::Zero(2 * m, n_tot);
    d.w.resize(2 * m);
    for (int k = 0; k < 2; ++k) {
        d.A.block(k * m, 0, m, ns) = As[k];
        d.A.block(k * m, ns + k * nl, m, nl) = Ar[k];
        d.w.segment(k * m, m) = w[k];
    }
    finishDense(d);
    const Eigen::VectorXd r_dense = denseRowRedundancy(d);

    // The kernel's covariance is the shared block of the dense inverse.
    const Eigen::MatrixXd cov_free = d.cov.topLeftCorner(ns, ns);
    for (int k = 0; k < 2; ++k) {
        const Eigen::VectorXd r_k =
            neq.rowRedundancy(slot[k], As[k], Ar[k], w[k], cov_free);
        ASSERT_EQ(r_k.size(), m);
        for (Eigen::Index i = 0; i < m; ++i)
            EXPECT_NEAR(r_k(i), r_dense(k * m + i), 1e-10)
                << "station " << k << " row " << i;
    }
}

// ===========================================================================
//  2. r ≈ 0 means "uncheckable", and coupling a second row raises it
// ===========================================================================

TEST(RedundancyCensusTest, OneObsOneFreeStateIsUncheckableAndCouplingRaisesIt) {
    // The era-2 topology in miniature.  Station 0 is a well-observed anchor that
    // pins the shared column.  Station 1 has one row feeding one private local
    // state (plus a negligible prior to keep N_rr factorizable): the "one
    // observation, one otherwise-unconstrained state" case.  Its row comes back
    // with r ≈ 0 because nothing cross-checks it.
    constexpr Eigen::Index ns = 1;
    constexpr double kPrior = 1e-6;   // 6 decades under the data

    auto buildAndMeasure = [&](bool second_row) {
        NetworkNormalEquations neq(ns);
        // Anchor station: 6 rows straight onto the shared column, no locals.
        Eigen::MatrixXd A0(6, ns);
        A0.setOnes();
        Eigen::VectorXd w0 = Eigen::VectorXd::Constant(6, 1.0);
        neq.addStation(A0.transpose() * w0.asDiagonal() * A0,
                       Eigen::MatrixXd::Zero(ns, 0), Eigen::MatrixXd::Zero(0, 0),
                       Eigen::VectorXd::Zero(ns), Eigen::VectorXd::Zero(0));
        // Suspect station: 1 (or 2) rows, each touching shared + its own local.
        const Eigen::Index m = second_row ? 2 : 1;
        Eigen::MatrixXd A1s(m, ns), A1r(m, 1);
        A1s.setOnes();
        A1r.setOnes();
        Eigen::VectorXd w1 = Eigen::VectorXd::Constant(m, 1.0);
        Eigen::MatrixXd Nrr = A1r.transpose() * w1.asDiagonal() * A1r;
        Nrr(0, 0) += kPrior;
        const std::size_t s1 =
            neq.addStation(A1s.transpose() * w1.asDiagonal() * A1s,
                           A1s.transpose() * w1.asDiagonal() * A1r, Nrr,
                           Eigen::VectorXd::Zero(ns), Eigen::VectorXd::Zero(1));
        neq.reduce();
        Eigen::VectorXd x_s = neq.solveShared();
        // No datum pins on this toy system, so the datum-frame shared covariance
        // is the inverse of the reduced normals (what NetworkEstimator caches).
        const Eigen::MatrixXd cov = neq.reducedSharedNormals().inverse();
        return neq.rowRedundancy(s1, A1s, A1r, w1, cov);
    };

    const Eigen::VectorXd r_one = buildAndMeasure(false);
    ASSERT_EQ(r_one.size(), 1);
    // Uncheckable: the single row's misfit is absorbed by its own free state.
    EXPECT_LT(r_one(0), 1e-4) << "a one-obs-one-free-state cell must read r ~ 0";
    EXPECT_GE(r_one(0), 0.0);

    const Eigen::VectorXd r_two = buildAndMeasure(true);
    ASSERT_EQ(r_two.size(), 2);
    // Two rows on the same local state now check each other: r rises well above 0.
    EXPECT_GT(r_two(0), 0.4) << "a coupled second row must make the cell checkable";
    EXPECT_GT(r_two(0), r_one(0));
}

// ===========================================================================
//  3. The closure bound the run-end line reports
// ===========================================================================

TEST(RedundancyCensusTest, TraceOfHatMatrixIsBoundedByFreeParameterCount) {
    // Σ(1 − r_i) = trace(H) = tr((AᵀWA)(AᵀWA + P)⁻¹) ≤ n_free, with equality iff
    // no prior information is folded.  Both directions are checked: an
    // unprior'd system attains the bound; adding a prior strictly lowers it.
    constexpr Eigen::Index ns = 2, nl = 2, m = 6;
    Eigen::MatrixXd As(m, ns), Ar(m, nl);
    As << 1, 0,  0, 1,  1, 1,  0.5, -0.3,  -0.2, 0.8,  0.7, 0.7;
    Ar << 1, 0,  1, 0.2,  0, 1,  0.4, 0.7,  0.9, -0.1,  0.3, 0.3;
    Eigen::VectorXd w(m); w << 1.0, 2.0, 0.5, 4.0, 1.5, 0.9;

    auto traceH = [&](double prior) {
        NetworkNormalEquations neq(ns);
        Eigen::MatrixXd Nrr = Ar.transpose() * w.asDiagonal() * Ar;
        Nrr.diagonal().array() += prior;
        Eigen::MatrixXd Nss = As.transpose() * w.asDiagonal() * As;
        Nss.diagonal().array() += prior;   // keep the shared block invertible alone
        const std::size_t s =
            neq.addStation(Nss, As.transpose() * w.asDiagonal() * Ar, Nrr,
                           Eigen::VectorXd::Zero(ns), Eigen::VectorXd::Zero(nl));
        neq.reduce();
        Eigen::VectorXd x_s = neq.solveShared();
        const Eigen::VectorXd r = neq.rowRedundancy(
            s, As, Ar, w, neq.reducedSharedNormals().inverse());
        double t = 0.0;
        for (Eigen::Index i = 0; i < r.size(); ++i) t += 1.0 - r(i);
        return t;
    };

    const double n_free = static_cast<double>(ns + nl);
    // A negligible prior only makes each block invertible on its own; the bound is
    // attained to within that prior.
    const double t_tight = traceH(1e-9);
    EXPECT_LE(t_tight, n_free + 1e-6);
    EXPECT_NEAR(t_tight, n_free, 1e-4)
        << "with negligible prior information trace(H) must attain n_free";
    // Real prior information strictly consumes leverage from the data.
    const double t_prior = traceH(5.0);
    EXPECT_LT(t_prior, n_free);
    EXPECT_LT(t_prior, t_tight);
}

// ===========================================================================
//  4. The pivot census, including the un-permutation (INC-2)
// ===========================================================================

TEST(RedundancyCensusTest, StationLocalPivotsAreInOriginalColumnOrder) {
    // Eigen's LDLT pivots symmetrically, so vectorD() is indexed in the permuted
    // frame.  A naive accessor would name the wrong local column.  The census
    // exists to name columns, so that error would leave every count correct and
    // every identity wrong.  A diagonal N_rr with an out-of-order magnitude
    // sequence forces a non-trivial permutation.
    constexpr Eigen::Index ns = 1, nl = 4;
    NetworkNormalEquations neq(ns);
    Eigen::VectorXd diag(nl);
    diag << 3.0, 100.0, 7.5e-9, 25.0;   // the small one is column 2, not last
    Eigen::MatrixXd Nrr = diag.asDiagonal();
    Eigen::MatrixXd Nss(ns, ns); Nss << 10.0;
    const std::size_t s = neq.addStation(Nss, Eigen::MatrixXd::Zero(ns, nl), Nrr,
                                         Eigen::VectorXd::Zero(ns),
                                         Eigen::VectorXd::Zero(nl));
    const Eigen::VectorXd piv = neq.stationLocalPivots(s);
    ASSERT_EQ(piv.size(), nl);
    for (Eigen::Index k = 0; k < nl; ++k)
        EXPECT_NEAR(piv(k), diag(k), 1e-12 * std::max(1.0, diag(k)))
            << "pivot " << k << " landed on the wrong column";

    // And the census names exactly that column.
    const std::vector<Eigen::Index> bad = unobservableLocalColumns(piv, 1e-8);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0], 2);
}

TEST(RedundancyCensusTest, PivotCensusIsRelativeAndFlagsOnlyTheOrphan) {
    Eigen::VectorXd piv(5);
    // A block whose absolute scale is tiny: an absolute floor would flag every
    // column here.  The census is relative to the median, so it flags none.
    piv << 1e-6, 2e-6, 1.5e-6, 3e-6, 1e-6;
    EXPECT_TRUE(unobservableLocalColumns(piv, 1e-8).empty());

    // The floor is relative, so "how far down" is measured against the median
    // (1e-6 here), not against 1.  A column six decades down is not unobservable
    // at eps = 1e-8: it is weak, and flagging it would put every low-elevation
    // slant state in the census.  A column ten decades down is unobservable.
    piv(3) = 1.5e-12;                                          // 6 decades
    EXPECT_TRUE(unobservableLocalColumns(piv, 1e-8).empty());
    EXPECT_EQ(unobservableLocalColumns(piv, 1e-5).size(), 1u); // flagged at eps = 1e-5
    piv(3) = 1.5e-16;                                          // 10 decades
    const std::vector<Eigen::Index> bad = unobservableLocalColumns(piv, 1e-8);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0], 3);

    // Non-positive / non-finite pivots are findings in their own right, and they
    // are kept out of the median reference.
    Eigen::VectorXd sick(4);
    sick << 1.0, -0.5, 2.0, std::numeric_limits<double>::quiet_NaN();
    const std::vector<Eigen::Index> bad2 = unobservableLocalColumns(sick, 1e-8);
    ASSERT_EQ(bad2.size(), 2u);
    EXPECT_EQ(bad2[0], 1);
    EXPECT_EQ(bad2[1], 3);

    // A disarmed floor flags nothing rather than everything.
    EXPECT_TRUE(unobservableLocalColumns(piv, 0.0).empty());
}

// ===========================================================================
//  5. The cross-flag needs both halves
// ===========================================================================

TEST(RedundancyCensusTest, CrossFlagRequiresBothUncheckableAndHot) {
    constexpr double eps = 0.05, hot = 1000.0;
    // The era-2 cell: uncheckable and enormous.
    EXPECT_TRUE(isUncheckableHotCell(1e-9, 2.6e5, eps, hot));
    // A healthy single-frequency riser: uncheckable and quiet.  Keeping it is the
    // prefer-fallback directive; eventing it would be a false positive on every
    // such riser.
    EXPECT_FALSE(isUncheckableHotCell(1e-9, 3.2, eps, hot));
    // A storm extreme on a well-checked cell: enormous and checkable.  The
    // redundant network localizes it; post-fit QC already sees it.
    EXPECT_FALSE(isUncheckableHotCell(0.62, 2.6e5, eps, hot));
    // Neither.
    EXPECT_FALSE(isUncheckableHotCell(0.62, 3.2, eps, hot));
    // Exactly on the boundaries: strict, so a threshold is never a flag.
    EXPECT_FALSE(isUncheckableHotCell(eps, 2.6e5, eps, hot));
    EXPECT_FALSE(isUncheckableHotCell(1e-9, hot, eps, hot));
    // An unmeasurable magnitude half is never a flag (the centring pool was
    // skipped this epoch), no matter how uncheckable the row is.
    EXPECT_FALSE(isUncheckableHotCell(
        0.0, std::numeric_limits<double>::quiet_NaN(), eps, hot));
}

// ===========================================================================
//  6. The stride / escalation gate
// ===========================================================================

TEST(RedundancyCensusTest, StrideAndTripGating) {
    // Pass 0 always sweeps: a run must not wait `stride` epochs for its first
    // measurement.
    EXPECT_TRUE(censusSweepsThisEpoch(0, 10, false));
    EXPECT_FALSE(censusSweepsThisEpoch(1, 10, false));
    EXPECT_FALSE(censusSweepsThisEpoch(9, 10, false));
    EXPECT_TRUE(censusSweepsThisEpoch(10, 10, false));
    EXPECT_TRUE(censusSweepsThisEpoch(20, 10, false));
    // Stride 1 = every epoch (the diagnostic leg).
    for (long long e = 0; e < 5; ++e) EXPECT_TRUE(censusSweepsThisEpoch(e, 1, false));
    // Stride 0 = escalation-only: no scheduled sweep ever.
    for (long long e = 0; e < 5; ++e) EXPECT_FALSE(censusSweepsThisEpoch(e, 0, false));
    // A flight-recorder trip forces a full sweep whatever the stride phase, so the
    // attribution dump and the census describe the same epoch.
    EXPECT_TRUE(censusSweepsThisEpoch(7, 10, true));
    EXPECT_TRUE(censusSweepsThisEpoch(3, 0, true));
}

// ===========================================================================
//  7. The magnitude half: common-mode removal
// ===========================================================================

TEST(RedundancyCensusTest, CensusMetricCancelsAClockStepAndKeepsAParkedCell) {
    // Eight code rows at one station.  A clean 1 ms receiver clock step puts
    // ~3e5 m on every row at once; the white receiver-clock state absorbs it
    // exactly, and a raw |y|·√w census would flag all eight.  One row is also
    // parked 1.88e5 m off on its own.  The centred metric cancels the first and
    // keeps the second.
    constexpr Eigen::Index m = 8;
    const double step = 2.9979e5;    // 1 ms of light
    Eigen::VectorXd y(m), w(m);
    for (Eigen::Index i = 0; i < m; ++i) {
        y(i) = step + 0.1 * static_cast<double>(i);   // common mode + dm-level spread
        w(i) = 1.0;                                    // sigma = 1 m
    }
    y(5) += 1.88e5;                                    // the parked cell

    StationAssembly R = makeAssembly(Eigen::MatrixXd::Zero(m, 1),
                                     Eigen::MatrixXd::Zero(m, 1), w, y);
    const std::vector<NetRowClass> cls(static_cast<std::size_t>(m),
                                       NetRowClass::ObsCode);
    const StationPrefitCensus c = stationPrefitCensus(cls, R);

    EXPECT_EQ(c.n[0], static_cast<int>(m));
    EXPECT_EQ(c.n_pool_skipped, 0);
    EXPECT_EQ(c.n_data_rows, static_cast<int>(m));
    // The clock step is gone: every non-parked row sits at decimetre scale.
    for (Eigen::Index i = 0; i < m; ++i) {
        if (i == 5) continue;
        EXPECT_LT(c.s[static_cast<std::size_t>(i)], 1.0) << "row " << i;
    }
    // The parked cell keeps essentially its full magnitude, and it is the max.
    EXPECT_EQ(c.max_row, 5);
    EXPECT_GT(c.max_s, 1.87e5);
    EXPECT_LT(c.max_s, 1.89e5);
}

TEST(RedundancyCensusTest, CensusSkipsThinPoolsAndExcludesPinsAndCoupling) {
    // A pool of three is too small for a median (spec: n >= 4), and a skipped
    // pool yields NaN rather than a value centred on noise.
    constexpr Eigen::Index m = 3;
    Eigen::VectorXd y(m), w(m);
    y << 10.0, 10.2, 400.0;
    w << 1.0, 1.0, 1.0;
    StationAssembly R = makeAssembly(Eigen::MatrixXd::Zero(m, 1),
                                     Eigen::MatrixXd::Zero(m, 1), w, y);
    const std::vector<NetRowClass> cls(static_cast<std::size_t>(m),
                                       NetRowClass::ObsCode);
    const StationPrefitCensus thin = stationPrefitCensus(cls, R);
    EXPECT_EQ(thin.n_pool_skipped, 1);
    EXPECT_EQ(thin.n_data_rows, 0);
    EXPECT_EQ(thin.max_row, -1);
    for (Eigen::Index i = 0; i < m; ++i)
        EXPECT_TRUE(std::isnan(thin.s[static_cast<std::size_t>(i)]));

    // Pins and coupling rows are excluded outright: a healthy pivot pin runs at
    // ~1e7 by design and would dominate any statistic it entered.
    constexpr Eigen::Index m2 = 6;
    Eigen::VectorXd y2 = Eigen::VectorXd::Constant(m2, 1.0);
    Eigen::VectorXd w2 = Eigen::VectorXd::Constant(m2, 1.0);
    y2(0) = 1e7;                      // the pin's "innovation"
    w2(0) = 1e10;                     // at pin weight
    StationAssembly R2 = makeAssembly(Eigen::MatrixXd::Zero(m2, 1),
                                      Eigen::MatrixXd::Zero(m2, 1), w2, y2);
    std::vector<NetRowClass> cls2(static_cast<std::size_t>(m2), NetRowClass::ObsCode);
    cls2[0] = NetRowClass::PivotPin;
    cls2[1] = NetRowClass::CplRow;
    const StationPrefitCensus c2 = stationPrefitCensus(cls2, R2);
    EXPECT_TRUE(std::isnan(c2.s[0]));
    EXPECT_TRUE(std::isnan(c2.s[1]));
    EXPECT_EQ(c2.n_data_rows, 4);
    EXPECT_LT(c2.max_s, 1e-9);        // the four surviving rows are identical

    // An iono_sh_row carries no receiver clock and is standardized raw.  Its
    // magnitude survives un-centred even when it is the only such row.
    std::vector<NetRowClass> cls3(static_cast<std::size_t>(m2), NetRowClass::ObsCode);
    cls3[0] = NetRowClass::IonoShRow;
    Eigen::VectorXd y3 = Eigen::VectorXd::Constant(m2, 2.0);
    y3(0) = 50.0;
    Eigen::VectorXd w3 = Eigen::VectorXd::Constant(m2, 4.0);   // sqrt(w) = 2
    StationAssembly R3 = makeAssembly(Eigen::MatrixXd::Zero(m2, 1),
                                      Eigen::MatrixXd::Zero(m2, 1), w3, y3);
    const StationPrefitCensus c3 = stationPrefitCensus(cls3, R3);
    EXPECT_NEAR(c3.s[0], 100.0, 1e-9);   // |50| * 2, no median subtracted
    EXPECT_EQ(c3.max_row, 0);
}

// ===========================================================================
//  8. Structural inertness when off
// ===========================================================================

TEST(RedundancyCensusTest, OffIsStructurallyInert) {
    // The defaults are off and the master predicate the driver reads is false,
    // so an unarmed run never enters the census translation unit.
    GnssConfig cfg;
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_EQ(nc.redundancy_census_stride, 0);
    EXPECT_DOUBLE_EQ(nc.redundancy_escalate_sigma, 0.0);
    EXPECT_FALSE(nc.redundancy_census_stride > 0
                 || nc.redundancy_escalate_sigma > 0.0);
    // The documented defaults, which the field gates and the run-end line quote.
    EXPECT_DOUBLE_EQ(nc.redundancy_eps, 0.05);
    EXPECT_DOUBLE_EQ(nc.redundancy_hot_sigma, 1000.0);
    EXPECT_DOUBLE_EQ(nc.redundancy_pivot_eps, 1e-8);
    EXPECT_EQ(nc.redundancy_event_holdoff_ep, 20);

    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.redun_census_on);

    // A fresh carry state allocates nothing for the census.
    NetworkCarryState carry;
    EXPECT_EQ(NetworkServiceCounters{}.redun_epochs, 0);   // instrument state (card NOM step N4g)
    EXPECT_TRUE(carry.redun_escalate_pending.empty());
    EXPECT_EQ(net::present(net::NetworkEntities{}.los, &net::LosState::events,
                           &net::LosEventState::redun_event_last), 0u);
    EXPECT_EQ(net::present(net::NetworkEntities{}.stations,
                           &net::StationState::redun_unobs_active), 0u);
    EXPECT_DOUBLE_EQ(carry.redun_ep_tow, -1.0);

    // The epoch-status row leaves the four census columns blank, so a run with the
    // census off is byte-identical in network_epoch_status.csv apart from the
    // (empty) trailing fields the header declares.
    EpochStatusRow es;
    EXPECT_FALSE(es.n_redun_rows.has_value());
    EXPECT_FALSE(es.n_uncheckable_rows.has_value());
    EXPECT_FALSE(es.uncheckable_max_s.has_value());
    EXPECT_FALSE(es.n_unobservable_local.has_value());
}

}  // namespace GnssProcessor
