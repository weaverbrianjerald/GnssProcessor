/**
 * @file test_neq_station_prior_before_pd.cpp
 * @brief Tests for the optional @ref NetworkNormalEquations::StationPrior argument of
 *        `NetworkNormalEquations::addStation`, which folds a station's own datum rows
 *        into @f$N_{rr}/u_r@f$ before the positive-definiteness test instead of
 *        afterwards through @ref addStationPrior
 *        (`network.station_prior_before_pd`).
 *
 * Two guarantees, one per gate:
 *
 *  1. **Admission.**  A station whose observation-only local block is singular in
 *     exactly one direction, the direction its own loose prior row gauges, is
 *     rejected with the prior empty (the behaviour with the knob off; see
 *     `docs/refound/RF-PD_station_rejection_2026-09-15.md`) and admitted with the
 *     same rows handed to `addStation`.  The synthetic block is the design's §5
 *     mechanism in miniature: a code/phase pair per band and satellite over
 *     `{receiver DCB, slant iono, L1 amb, L2 amb}` whose one null direction is the
 *     DCB ↔ iono ↔ ambiguity confound, with the ambiguity/iono mass ratio
 *     @f$1+\gamma^2@f$.
 *
 *  2. **Equivalence.**  For a block that passes either way, prior-before and
 *     prior-after produce the same system: the border cross-block and local RHS
 *     bit-for-bit (same additions, same order), and the local normals, reduced
 *     shared normals and reconstructed locals to about 1e-12 relative (1e-10 for
 *     the locals).  The residual cannot be removed: `Eigen::LDLT::rankUpdate` is a
 *     different algorithm from a fresh factorization of the same matrix, so the
 *     two cached factors differ at ULP level although they represent the same
 *     @f$N_{rr}+\sum_c \Lambda_c e_c e_c^\top@f$.  (rankUpdate also divides by the
 *     existing pivot, which is why the rank-deficient case in gate 1 can be
 *     resolved only inside the factorized matrix.)
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <vector>

#include "network/solve/NormalEquations.hpp"

namespace GnssProcessor {
namespace {

constexpr double kGamma = 1.646944444;  // (f_L1/f_L2)^2, the design's γ

/// One station's raw design rows over locals
/// `[rx DCB, iono_1, N1_1, N2_1, iono_2, N1_2, N2_2, ...]` and a shared border
/// of @p n_shared satellite columns.  Four rows per satellite (C1, L1, C2, L2),
/// each touching one shared column (the satellite's), so the local block carries
/// the whole DCB↔iono↔ambiguity structure the design names.
///
/// With `d1 = 1`, `d2 = γ` (the IGS DCB convention of §5) the observation-only
/// local block is rank-deficient by exactly one: the direction
/// `η=1, a_s=−1, b_s=−1, c_s=−γ` (for every satellite s) is annihilated by all
/// four row types simultaneously.
struct StationRows {
    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
    Eigen::Index dcb_col{0};
};

StationRows buildStation(int n_sat, Eigen::Index n_shared, Eigen::Index shared_base,
                         double d1, double d2, double wet_map_base) {
    const Eigen::Index n_local = 1 + 3 * n_sat;
    const Eigen::Index m = 4 * n_sat;
    StationRows S;
    S.A_s = Eigen::MatrixXd::Zero(m, n_shared);
    S.A_r = Eigen::MatrixXd::Zero(m, n_local);
    S.w = Eigen::VectorXd::Zero(m);
    S.y = Eigen::VectorXd::Zero(m);
    for (int s = 0; s < n_sat; ++s) {
        const Eigen::Index a = 1 + 3 * s;        // slant iono
        const Eigen::Index b = a + 1;            // L1 ambiguity
        const Eigen::Index c = a + 2;            // L2 ambiguity
        const Eigen::Index sh = (shared_base + s) % n_shared;
        const double mp = wet_map_base + 0.17 * s;   // satellite-dependent scale on the shared column
        for (int k = 0; k < 4; ++k) {
            const Eigen::Index r = 4 * s + k;
            // One coefficient per row type, not one per satellite: with a single
            // coefficient the shared column lies in the range of this station's
            // locals and the Schur complement cancels to about 0, which would make
            // the reduced-border comparison below vacuous.
            S.A_s(r, sh) = mp * (1.0 + 0.23 * k);
            S.y(r) = 0.01 * (r + 1) - 0.13 * s;
        }
        // C1 = +a + d1*eta            L1 = -a + b
        // C2 = +gamma*a + d2*eta      L2 = -gamma*a + c
        S.A_r(4 * s + 0, a) = 1.0;      S.A_r(4 * s + 0, 0) = d1;
        S.A_r(4 * s + 1, a) = -1.0;     S.A_r(4 * s + 1, b) = 1.0;
        S.A_r(4 * s + 2, a) = kGamma;   S.A_r(4 * s + 2, 0) = d2;
        S.A_r(4 * s + 3, a) = -kGamma;  S.A_r(4 * s + 3, c) = 1.0;
        S.w(4 * s + 0) = 1.0;    S.w(4 * s + 1) = 10000.0;
        S.w(4 * s + 2) = 1.0;    S.w(4 * s + 3) = 10000.0;
    }
    return S;
}

/// The loose zero-mean RXCB-DATUM row that station assembly builds in its pl_cb walk
/// (ApplicationNetworkAssembly.cpp): one diagonal entry on the receiver-DCB column,
/// Λ = 1/σ², η = Λ·(0 − x_lin).
NetworkNormalEquations::StationPrior dcbDatum(Eigen::Index dcb_col, double sigma,
                                             double x_lin) {
    NetworkNormalEquations::StationPrior p;
    p.cols = {dcb_col};
    p.dNrr = Eigen::VectorXd::Constant(1, 1.0 / (sigma * sigma));
    p.dur = Eigen::VectorXd::Constant(1, (1.0 / (sigma * sigma)) * (0.0 - x_lin));
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1, admission: the block the observation rows alone cannot determine.
// ---------------------------------------------------------------------------

/// Sanity check: the synthetic block is singular in exactly one direction, and that
/// direction is the design's closed-form null (mass ratio 1+γ² of ambiguity to iono).
/// Without this the admission test below could pass on a block that is merely
/// ill-conditioned, which is a different finding.
TEST(NeqStationPriorBeforePdTest, SyntheticBlockCarriesTheCardsRankOneNull) {
    const StationRows S = buildStation(/*n_sat=*/5, /*n_shared=*/6, /*shared_base=*/0,
                                       /*d1=*/1.0, /*d2=*/kGamma, /*wet_map_base=*/1.0);
    const Eigen::MatrixXd N_rr =
        S.A_r.transpose() * S.w.asDiagonal() * S.A_r;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N_rr);
    ASSERT_EQ(es.info(), Eigen::Success);
    const Eigen::VectorXd ev = es.eigenvalues();          // ascending
    const double ev_max = ev.cwiseAbs().maxCoeff();
    EXPECT_LT(std::abs(ev(0)), 1e-12 * ev_max) << "smallest eigenvalue is not numerically zero";
    EXPECT_GT(std::abs(ev(1)), 1e-8 * ev_max) << "the null is not rank ONE";

    // Per-family mass of the null eigenvector, the design's §4(1) census.
    const Eigen::VectorXd v = es.eigenvectors().col(0);
    double m_cbias = v(0) * v(0), m_iono = 0.0, m_amb = 0.0;
    for (int s = 0; s < 5; ++s) {
        const Eigen::Index a = 1 + 3 * s;
        m_iono += v(a) * v(a);
        m_amb += v(a + 1) * v(a + 1) + v(a + 2) * v(a + 2);
    }
    EXPECT_NEAR(m_amb + m_iono + m_cbias, 1.0, 1e-12);
    EXPECT_NEAR(m_amb / m_iono, 1.0 + kGamma * kGamma, 1e-6);  // §5's (b²+c²)/a²
}

/// The main case.  Same station, same rows, same PD test: rejected with no
/// prior, admitted when the station's own datum row is folded in first.
TEST(NeqStationPriorBeforePdTest, RejectedWithoutPriorAdmittedWithPriorBeforePd) {
    const StationRows S = buildStation(5, 6, 0, 1.0, kGamma, 1.0);
    const auto prior = dcbDatum(S.dcb_col, /*sigma=*/10.0, /*x_lin=*/0.37);

    // (a) Knob-off order: the PD test sees the observation rows alone → rejected.
    {
        NetworkNormalEquations neq(6);
        neq.setStationPdMedianScale(true);   // the pin-aware test the golden arms
        EXPECT_THROW(neq.addStationObservations(S.A_s, S.A_r, S.w, S.y),
                     std::invalid_argument);
        EXPECT_EQ(neq.numStations(), 0u) << "a rejected station must not be committed";
    }
    // (b) The same rows, with the datum row inside the factorized block →
    //     admitted, and the pivot that was about 0 now carries the prior
    //     (RF-PD-fix).
    {
        NetworkNormalEquations neq(6);
        neq.setStationPdMedianScale(true);
        std::size_t idx = 0;
        ASSERT_NO_THROW(idx = neq.addStationObservations(S.A_s, S.A_r, S.w, S.y, prior));
        EXPECT_EQ(neq.numStations(), 1u);
        const Eigen::VectorXd d = neq.stationLocalPivots(idx);
        EXPECT_GT(d.minCoeff(), 0.0) << "the admitted block must be positive-definite";
        // The prior's information is in the block: Λ = 1/σ² = 0.01 is the only
        // difference between the two arms, and the smallest pivot is now of that
        // order rather than of order 1e-12 · block scale.
        EXPECT_GT(d.minCoeff(), 1e-4);
    }
    // (c) The legacy post-test path cannot admit it, because the station was
    //     never committed; addStationPrior throws std::out_of_range.  This is why
    //     the fix sits at the addStation seam.
    {
        NetworkNormalEquations neq(6);
        neq.setStationPdMedianScale(true);
        EXPECT_THROW(neq.addStationObservations(S.A_s, S.A_r, S.w, S.y),
                     std::invalid_argument);
        EXPECT_THROW(neq.addStationPrior(0, prior.cols, prior.dNrr, prior.dur),
                     std::out_of_range);
    }
}

/// The prior is validated with the addStationPrior contract, before anything reaches
/// the shared border, so a malformed prior does not leave a half-added station.
TEST(NeqStationPriorBeforePdTest, RejectsMalformedPriorAndLeavesTheBorderClean) {
    const StationRows S = buildStation(3, 4, 0, 1.0, kGamma, 1.0);
    const Eigen::Index n_local = S.A_r.cols();
    auto attempt = [&](const NetworkNormalEquations::StationPrior& p) {
        NetworkNormalEquations neq(4);
        EXPECT_THROW(neq.addStationObservations(S.A_s, S.A_r, S.w, S.y, p),
                     std::invalid_argument);
        EXPECT_EQ(neq.numStations(), 0u);
        neq.reduce();
        EXPECT_TRUE(neq.reducedSharedNormals().isZero(0.0))
            << "a rejected station must leave the shared border untouched";
    };
    {   // column out of range
        NetworkNormalEquations::StationPrior p;
        p.cols = {n_local};
        p.dNrr = Eigen::VectorXd::Constant(1, 1.0);
        p.dur = Eigen::VectorXd::Constant(1, 0.0);
        attempt(p);
    }
    {   // negative information
        NetworkNormalEquations::StationPrior p;
        p.cols = {0};
        p.dNrr = Eigen::VectorXd::Constant(1, -1.0);
        p.dur = Eigen::VectorXd::Constant(1, 0.0);
        attempt(p);
    }
    {   // non-finite RHS
        NetworkNormalEquations::StationPrior p;
        p.cols = {0};
        p.dNrr = Eigen::VectorXd::Constant(1, 1.0);
        p.dur = Eigen::VectorXd::Constant(1, std::numeric_limits<double>::quiet_NaN());
        attempt(p);
    }
    {   // size mismatch
        NetworkNormalEquations::StationPrior p;
        p.cols = {0, 1};
        p.dNrr = Eigen::VectorXd::Constant(1, 1.0);
        p.dur = Eigen::VectorXd::Constant(1, 0.0);
        attempt(p);
    }
}

// ---------------------------------------------------------------------------
// Gate 2, equivalence on a block that passes either way.
// ---------------------------------------------------------------------------

/// `d2 != gamma*d1` breaks the exact null (§5's collapse needs the IGS
/// convention), so this block is full-rank on its observations alone and both
/// orders admit it.  The two resulting systems then agree to the tolerances below.
TEST(NeqStationPriorBeforePdTest, AdmittedEitherWayGivesTheSameSystem) {
    const Eigen::Index n_shared = 5;
    const StationRows S0 = buildStation(4, n_shared, 0, /*d1=*/1.0, /*d2=*/0.4, 1.0);
    const StationRows S1 = buildStation(4, n_shared, 2, /*d1=*/1.0, /*d2=*/0.55, 1.3);
    const auto p0 = dcbDatum(S0.dcb_col, 10.0, 0.37);
    const auto p1 = dcbDatum(S1.dcb_col, 10.0, -0.21);

    // Arm A: prior before the PD test (network.station_prior_before_pd on).
    NetworkNormalEquations before(n_shared);
    const std::size_t a0 = before.addStationObservations(S0.A_s, S0.A_r, S0.w, S0.y, p0);
    const std::size_t a1 = before.addStationObservations(S1.A_s, S1.A_r, S1.w, S1.y, p1);
    before.reduce();

    // Arm B: prior after, through addStationPrior (the knob-off path).
    NetworkNormalEquations after(n_shared);
    const std::size_t b0 = after.addStationObservations(S0.A_s, S0.A_r, S0.w, S0.y);
    const std::size_t b1 = after.addStationObservations(S1.A_s, S1.A_r, S1.w, S1.y);
    after.addStationPrior(b0, p0.cols, p0.dNrr, p0.dur);
    after.addStationPrior(b1, p1.cols, p1.dNrr, p1.dur);
    after.reduce();

    ASSERT_EQ(before.numStations(), 2u);
    ASSERT_EQ(after.numStations(), 2u);

    // (i) The border cross-block and the local RHS are bit-identical: N_sr is
    //     untouched by a diagonal local prior, and u_r receives the same
    //     increments onto the same base in the same ascending column order.
    for (std::size_t k = 0; k < 2; ++k) {
        EXPECT_TRUE(before.stationCrossBlock(k) == after.stationCrossBlock(k))
            << "N_sr must be bit-identical for station " << k;
        EXPECT_TRUE(before.stationLocalRhs(k) == after.stationLocalRhs(k))
            << "u_r must be bit-identical for station " << k;
    }

    // (ii) The local normal block, the Schur-reduced border and the
    //      back-substituted locals agree to about 1e-12 relative (1e-10 for the
    //      locals).  They are not bit-exact by construction: LDLT::rankUpdate and a
    //      fresh LDLT of the same summed matrix are different algorithms.
    //
    //      The reference scale is that of the operands, not of the result.  The
    //      reduced border is a small difference of large numbers (N_ss − Σ N_sr
    //      N_rr⁻¹ N_rs cancels about 6 orders here), so its accuracy is bounded by
    //      the scale the arithmetic runs at; measured against its own small maximum
    //      the comparison would measure cancellation noise.
    double scale_rr = 0.0, scale_ur = 0.0;
    for (std::size_t k = 0; k < 2; ++k) {
        const Eigen::MatrixXd nb = before.stationLocalNormals(k);
        const Eigen::MatrixXd na = after.stationLocalNormals(k);
        ASSERT_EQ(nb.rows(), na.rows());
        scale_rr = std::max(scale_rr, na.cwiseAbs().maxCoeff());
        scale_ur = std::max(scale_ur, after.stationLocalRhs(k).cwiseAbs().maxCoeff());
        EXPECT_LT((nb - na).cwiseAbs().maxCoeff(),
                  1e-12 * na.cwiseAbs().maxCoeff())
            << "N_rr disagrees for station " << k;
    }
    ASSERT_GT(scale_rr, 0.0);
    const Eigen::MatrixXd& Nb = before.reducedSharedNormals();
    const Eigen::MatrixXd& Na = after.reducedSharedNormals();
    // Guard against a degenerate border: if the Schur complement cancelled to
    // exactly 0 the comparison below would be vacuous.
    ASSERT_GT(Na.cwiseAbs().maxCoeff(), 1e-3);
    EXPECT_LT((Nb - Na).cwiseAbs().maxCoeff(), 1e-12 * scale_rr)
        << "reduced N_ss disagrees (operand scale " << scale_rr << ")";
    EXPECT_LT((before.reducedSharedRhs() - after.reducedSharedRhs()).cwiseAbs().maxCoeff(),
              1e-12 * std::max(1.0, scale_ur))
        << "reduced u_s disagrees (operand scale " << scale_ur << ")";

    const Eigen::VectorXd x_s =
        Eigen::VectorXd::LinSpaced(n_shared, 0.11, 0.73);
    for (std::size_t k = 0; k < 2; ++k) {
        const Eigen::VectorXd rb = before.reconstructStation(k, x_s);
        const Eigen::VectorXd ra = after.reconstructStation(k, x_s);
        ASSERT_EQ(rb.size(), ra.size());
        EXPECT_LT((rb - ra).cwiseAbs().maxCoeff(),
                  1e-10 * std::max(1.0, ra.cwiseAbs().maxCoeff()))
            << "reconstructed locals disagree for station " << k;
    }
    (void)a0; (void)a1;
}

/// An empty prior is the knob-off path and equals the legacy call: same admission,
/// and a system bit-identical to the no-argument overload.
TEST(NeqStationPriorBeforePdTest, EmptyPriorIsTheLegacyCallBitForBit) {
    const Eigen::Index n_shared = 4;
    const StationRows S = buildStation(4, n_shared, 1, 1.0, 0.4, 1.0);

    NetworkNormalEquations legacy(n_shared);
    legacy.addStationObservations(S.A_s, S.A_r, S.w, S.y);
    legacy.reduce();

    NetworkNormalEquations empty(n_shared);
    empty.addStationObservations(S.A_s, S.A_r, S.w, S.y,
                                 NetworkNormalEquations::StationPrior{});
    empty.reduce();

    ASSERT_EQ(legacy.numStations(), empty.numStations());
    EXPECT_TRUE(legacy.stationCrossBlock(0) == empty.stationCrossBlock(0));
    EXPECT_TRUE(legacy.stationLocalRhs(0) == empty.stationLocalRhs(0));
    EXPECT_TRUE(legacy.stationLocalPivots(0) == empty.stationLocalPivots(0));
    EXPECT_TRUE(legacy.reducedSharedNormals() == empty.reducedSharedNormals());
    EXPECT_TRUE(legacy.reducedSharedRhs() == empty.reducedSharedRhs());
}

/// A prior on a station with no local block is a caller error and throws; it is not
/// ignored.
TEST(NeqStationPriorBeforePdTest, PriorOnZeroLocalStationThrows) {
    NetworkNormalEquations neq(3);
    Eigen::MatrixXd N_ss = Eigen::MatrixXd::Identity(3, 3);
    Eigen::MatrixXd N_sr(3, 0);
    Eigen::MatrixXd N_rr(0, 0);
    Eigen::VectorXd u_s = Eigen::VectorXd::Zero(3);
    Eigen::VectorXd u_r(0);
    NetworkNormalEquations::StationPrior p;
    p.cols = {0};
    p.dNrr = Eigen::VectorXd::Constant(1, 1.0);
    p.dur = Eigen::VectorXd::Constant(1, 0.0);
    EXPECT_THROW(neq.addStation(N_ss, N_sr, N_rr, u_s, u_r, p), std::invalid_argument);
    EXPECT_EQ(neq.numStations(), 0u);
}

}  // namespace GnssProcessor
