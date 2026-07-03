/**
 * @file test_network_normal_equations.cpp
 * @brief Unit tests for the arrowhead Schur-reduction kernel
 *        (NetworkNormalEquations) — W4-S1 of the PPP-RTK network producer.
 *
 * The headline guarantee is EXACTNESS: per-station local normals Schur-reduced
 * onto the shared satellite border, solved, and back-substituted reproduce a
 * dense stacked normal-equation solve to machine precision.  The dense
 * reference is assembled completely independently (one big bordered N = AᵀWA
 * built from the raw design rows, dense LDLᵀ solve) — it never touches the
 * Schur code — so agreement is a genuine cross-check of the marginalization
 * identities, including a non-diagonal (correlated) measurement weight.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <random>
#include <vector>

#include "filter/NetworkNormalEquations.hpp"

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
// Synthetic-station fixture: raw linearized design rows + a correlated weight.
// ---------------------------------------------------------------------------
struct StationData {
    Eigen::MatrixXd A_s;  // m × n_shared  (Jacobian wrt shared satellite params)
    Eigen::MatrixXd A_r;  // m × n_local   (Jacobian wrt this station's locals)
    Eigen::MatrixXd W;    // m × m         (SPD measurement weight, correlated)
    Eigen::VectorXd y;    // m             (prefit residual)
};

// Deterministic SPD matrix of size n (W = M Mᵀ + n·I → well-conditioned).
Eigen::MatrixXd makeSpd(std::mt19937& gen, Eigen::Index n) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    Eigen::MatrixXd M(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j) M(i, j) = dist(gen);
    return M * M.transpose() + double(n) * Eigen::MatrixXd::Identity(n, n);
}

Eigen::MatrixXd makeRand(std::mt19937& gen, Eigen::Index r, Eigen::Index c) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    Eigen::MatrixXd A(r, c);
    for (Eigen::Index i = 0; i < r; ++i)
        for (Eigen::Index j = 0; j < c; ++j) A(i, j) = dist(gen);
    return A;
}

// Solve the full bordered system densely, INDEPENDENTLY of the Schur kernel.
// Layout: x = [x_s | x_0 | x_1 | ...]; station r's rows carry A_s in the shared
// columns and A_r in station r's local columns.  Adds an isotropic prior
// (shared_prior on the shared block, local_prior on every local block) so the
// system is full-rank — the same priors fed to the Schur path.
struct DenseSolution {
    Eigen::VectorXd x;                       // full stacked estimate
    Eigen::MatrixXd N;                        // full bordered normal matrix
    Eigen::VectorXd u;                        // full RHS
    std::vector<Eigen::Index> local_offset;  // column offset of each station block
    Eigen::Index n_shared{0};
};

DenseSolution denseStackedSolve(const std::vector<StationData>& stations,
                                Eigen::Index n_shared, double shared_prior,
                                double local_prior) {
    Eigen::Index M = 0, P = n_shared;
    std::vector<Eigen::Index> local_offset, m_rows;
    for (const auto& s : stations) {
        local_offset.push_back(P);
        P += s.A_r.cols();
        m_rows.push_back(s.A_s.rows());
        M += s.A_s.rows();
    }

    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(M, P);
    Eigen::MatrixXd W = Eigen::MatrixXd::Zero(M, M);
    Eigen::VectorXd y = Eigen::VectorXd::Zero(M);
    Eigen::Index row = 0;
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const auto& s = stations[r];
        const Eigen::Index m = m_rows[r];
        A.block(row, 0, m, n_shared) = s.A_s;
        A.block(row, local_offset[r], m, s.A_r.cols()) = s.A_r;
        W.block(row, row, m, m) = s.W;  // block-diagonal: stations uncorrelated
        y.segment(row, m) = s.y;
        row += m;
    }

    Eigen::MatrixXd N = A.transpose() * W * A;
    Eigen::VectorXd u = A.transpose() * W * y;
    // Priors (target 0 → no RHS contribution).
    N.topLeftCorner(n_shared, n_shared) +=
        shared_prior * Eigen::MatrixXd::Identity(n_shared, n_shared);
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        N.block(local_offset[r], local_offset[r], n_r, n_r) +=
            local_prior * Eigen::MatrixXd::Identity(n_r, n_r);
    }

    DenseSolution out;
    out.x = N.ldlt().solve(u);
    out.N = N;
    out.u = u;
    out.local_offset = local_offset;
    out.n_shared = n_shared;
    return out;
}

// Build the Schur kernel from the same raw data + priors, then solve.
void schurSolve(const std::vector<StationData>& stations, Eigen::Index n_shared,
                double shared_prior, double local_prior, Eigen::VectorXd& x_s,
                std::vector<Eigen::VectorXd>& x_r) {
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(shared_prior * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations) {
        const Eigen::MatrixXd WA_s = s.W * s.A_s;
        const Eigen::MatrixXd WA_r = s.W * s.A_r;
        const Eigen::Index n_r = s.A_r.cols();
        Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r;
        N_rr += local_prior * Eigen::MatrixXd::Identity(n_r, n_r);
        neq.addStation(s.A_s.transpose() * WA_s,  // N_ss
                       s.A_s.transpose() * WA_r,  // N_sr
                       N_rr,                       // N_rr (+ prior)
                       WA_s.transpose() * s.y,     // u_s
                       WA_r.transpose() * s.y);    // u_r
    }
    x_s = neq.solveShared();
    x_r.clear();
    for (std::size_t r = 0; r < stations.size(); ++r)
        x_r.push_back(neq.reconstructStation(r, x_s));
}

std::vector<StationData> makeStations(std::mt19937& gen, Eigen::Index n_shared,
                                      const std::vector<Eigen::Index>& n_locals,
                                      const std::vector<Eigen::Index>& m_obs,
                                      bool correlated) {
    std::vector<StationData> stations;
    for (std::size_t r = 0; r < n_locals.size(); ++r) {
        StationData s;
        const Eigen::Index m = m_obs[r];
        s.A_s = makeRand(gen, m, n_shared);
        s.A_r = makeRand(gen, m, n_locals[r]);
        s.y = makeRand(gen, m, 1);
        s.W = correlated ? makeSpd(gen, m)
                         : Eigen::MatrixXd(Eigen::VectorXd::LinSpaced(m, 0.5, 2.0)
                                               .asDiagonal());
        stations.push_back(std::move(s));
    }
    return stations;
}

// ---------------------------------------------------------------------------
// Hand-computed oracle: 1 shared, 1 station with 1 local → a 2×2 system.
//   N = [[a, b],[b, c]],  u = [us, ur]
//   Schur reduce onto shared:  Ñ = a − b²/c,  ũ = us − b·ur/c
//   x_s = ũ/Ñ,   x_r = (ur − b·x_s)/c
// ---------------------------------------------------------------------------
TEST(NetworkNormalEquationsTest, HandComputedScalarOracle) {
    const double a = 4.0, b = 1.5, c = 3.0, us = 2.0, ur = -1.0;

    NetworkNormalEquations neq(1);
    Eigen::MatrixXd N_ss(1, 1); N_ss << a;
    Eigen::MatrixXd N_sr(1, 1); N_sr << b;
    Eigen::MatrixXd N_rr(1, 1); N_rr << c;
    Eigen::VectorXd u_s(1); u_s << us;
    Eigen::VectorXd u_r(1); u_r << ur;
    neq.addStation(N_ss, N_sr, N_rr, u_s, u_r);
    neq.reduce();

    EXPECT_NEAR(neq.reducedSharedNormals()(0, 0), a - b * b / c, 1e-12);
    EXPECT_NEAR(neq.reducedSharedRhs()(0), us - b * ur / c, 1e-12);

    const Eigen::VectorXd x_s = neq.solveShared();
    const double xs_expected = (us - b * ur / c) / (a - b * b / c);
    EXPECT_NEAR(x_s(0), xs_expected, 1e-12);

    const Eigen::VectorXd x_r = neq.reconstructStation(0, x_s);
    EXPECT_NEAR(x_r(0), (ur - b * xs_expected) / c, 1e-12);

    // Cross-check against the dense 2×2 solve.
    Eigen::Matrix2d N; N << a, b, b, c;
    Eigen::Vector2d u; u << us, ur;
    const Eigen::Vector2d x_dense = N.ldlt().solve(u);
    EXPECT_NEAR(x_s(0), x_dense(0), 1e-12);
    EXPECT_NEAR(x_r(0), x_dense(1), 1e-12);
}

// Headline exactness test: 2 stations, correlated (full SPD) weights.
TEST(NetworkNormalEquationsTest, MatchesDenseStackedSolve_TwoStations_Correlated) {
    std::mt19937 gen(12345);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, /*n_locals=*/{3, 4},
                                       /*m_obs=*/{10, 12}, /*correlated=*/true);
    const double sp = 1e-2, lp = 1e-2;

    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);
    Eigen::VectorXd x_s;
    std::vector<Eigen::VectorXd> x_r;
    schurSolve(stations, n_shared, sp, lp, x_s, x_r);

    EXPECT_TRUE(x_s.isApprox(dense.x.head(n_shared), 1e-9))
        << "x_s=\n" << x_s.transpose() << "\nref=\n"
        << dense.x.head(n_shared).transpose();
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::VectorXd ref = dense.x.segment(dense.local_offset[r], n_r);
        EXPECT_TRUE(x_r[r].isApprox(ref, 1e-9))
            << "station " << r << " x_r=\n" << x_r[r].transpose()
            << "\nref=\n" << ref.transpose();
    }
}

// Same exactness with 3 stations and diagonal weights, driven through the
// addStationObservations diagonal-weight convenience path.
TEST(NetworkNormalEquationsTest, MatchesDenseStackedSolve_ThreeStations_Diagonal) {
    std::mt19937 gen(67890);
    const Eigen::Index n_shared = 5;
    const auto stations = makeStations(gen, n_shared, /*n_locals=*/{2, 3, 4},
                                       /*m_obs=*/{9, 11, 13}, /*correlated=*/false);
    const double sp = 1e-3, lp = 5e-3;

    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);

    // Schur path via addStationObservations(diagonal) + local prior pseudo-obs.
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(sp * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations) {
        const Eigen::Index n_r = s.A_r.cols();
        // Append n_r prior pseudo-obs rows (A_r = I, weight = lp) so the local
        // prior is exercised through the public observation API.
        const Eigen::Index m = s.A_s.rows();
        Eigen::MatrixXd A_s_aug(m + n_r, n_shared);
        A_s_aug << s.A_s, Eigen::MatrixXd::Zero(n_r, n_shared);
        Eigen::MatrixXd A_r_aug(m + n_r, n_r);
        A_r_aug << s.A_r, Eigen::MatrixXd::Identity(n_r, n_r);
        Eigen::VectorXd y_aug(m + n_r);
        y_aug << s.y, Eigen::VectorXd::Zero(n_r);
        Eigen::VectorXd w_aug(m + n_r);
        w_aug << s.W.diagonal(), lp * Eigen::VectorXd::Ones(n_r);
        neq.addStationObservations(A_s_aug, A_r_aug, w_aug, y_aug);
    }
    const Eigen::VectorXd x_s = neq.solveShared();

    EXPECT_TRUE(x_s.isApprox(dense.x.head(n_shared), 1e-9))
        << "x_s=\n" << x_s.transpose() << "\nref=\n"
        << dense.x.head(n_shared).transpose();
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::VectorXd ref = dense.x.segment(dense.local_offset[r], n_r);
        EXPECT_TRUE(neq.reconstructStation(r, x_s).isApprox(ref, 1e-9));
    }
}

// addStationObservations(full W) must form exactly the same normals as feeding
// the hand-formed AᵀWA blocks to addStation.
TEST(NetworkNormalEquationsTest, ObservationApiFormsSameNormals) {
    std::mt19937 gen(42);
    const Eigen::Index n_shared = 3, n_local = 2, m = 7;
    const Eigen::MatrixXd A_s = makeRand(gen, m, n_shared);
    const Eigen::MatrixXd A_r = makeRand(gen, m, n_local);
    const Eigen::MatrixXd W = makeSpd(gen, m);
    const Eigen::VectorXd y = makeRand(gen, m, 1);

    NetworkNormalEquations a(n_shared), b(n_shared);
    a.addStationObservations(A_s, A_r, W, y);
    const Eigen::MatrixXd WA_s = W * A_s, WA_r = W * A_r;
    b.addStation(A_s.transpose() * WA_s, A_s.transpose() * WA_r,
                 A_r.transpose() * WA_r, WA_s.transpose() * y,
                 WA_r.transpose() * y);

    EXPECT_TRUE(a.sharedNormals().isApprox(b.sharedNormals(), 1e-12));
    EXPECT_TRUE(a.sharedRhs().isApprox(b.sharedRhs(), 1e-12));
    a.reduce(); b.reduce();
    EXPECT_TRUE(a.reducedSharedNormals().isApprox(b.reducedSharedNormals(), 1e-12));
    EXPECT_TRUE(a.reducedSharedRhs().isApprox(b.reducedSharedRhs(), 1e-12));
}

// A station with no private nuisance state contributes its border directly and
// reconstructs to an empty vector.
TEST(NetworkNormalEquationsTest, ZeroLocalStationContributesBorderDirectly) {
    NetworkNormalEquations neq(2);
    Eigen::MatrixXd N_ss(2, 2); N_ss << 3, 1, 1, 4;
    Eigen::MatrixXd N_sr(2, 0);
    Eigen::MatrixXd N_rr(0, 0);
    Eigen::VectorXd u_s(2); u_s << 1, 2;
    Eigen::VectorXd u_r(0);
    const std::size_t id = neq.addStation(N_ss, N_sr, N_rr, u_s, u_r);
    EXPECT_EQ(neq.numLocal(id), 0);

    neq.reduce();
    EXPECT_TRUE(neq.reducedSharedNormals().isApprox(N_ss, 1e-12));
    EXPECT_TRUE(neq.reducedSharedRhs().isApprox(u_s, 1e-12));

    const Eigen::VectorXd x_s = neq.solveShared();
    EXPECT_TRUE(x_s.isApprox(N_ss.ldlt().solve(u_s), 1e-12));
    EXPECT_EQ(neq.reconstructStation(id, x_s).size(), 0);
}

// Dimension guards reject mismatched blocks.
TEST(NetworkNormalEquationsTest, RejectsDimensionMismatch) {
    NetworkNormalEquations neq(3);
    Eigen::MatrixXd bad_N_ss(2, 2); bad_N_ss.setZero();
    Eigen::MatrixXd N_sr(3, 1); N_sr.setZero();
    Eigen::MatrixXd N_rr(1, 1); N_rr << 1.0;
    Eigen::VectorXd u_s(3); u_s.setZero();
    Eigen::VectorXd u_r(1); u_r.setZero();
    EXPECT_THROW(neq.addStation(bad_N_ss, N_sr, N_rr, u_s, u_r),
                 std::invalid_argument);
}

// reduce() recomputes from the immutable accumulated blocks: calling it twice
// (no add* between) is idempotent — guards against an in-place mutation regression.
TEST(NetworkNormalEquationsTest, ReduceIsIdempotent) {
    std::mt19937 gen(2024);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, {3, 2}, {8, 9}, true);
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations)
        neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);

    neq.reduce();
    const Eigen::MatrixXd N1 = neq.reducedSharedNormals();
    const Eigen::VectorXd u1 = neq.reducedSharedRhs();
    neq.reduce();
    EXPECT_TRUE(neq.reducedSharedNormals().isApprox(N1, 1e-15));
    EXPECT_TRUE(neq.reducedSharedRhs().isApprox(u1, 1e-15));
    // sharedNormals() (the pre-reduction accumulator) must be untouched by reduce.
    NetworkNormalEquations fresh(n_shared);
    fresh.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                        Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations)
        fresh.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    EXPECT_TRUE(neq.sharedNormals().isApprox(fresh.sharedNormals(), 1e-15));
}

// solveShared() lazily reduces on first call and is stable across repeated calls.
TEST(NetworkNormalEquationsTest, SolveSharedLazyReduce) {
    std::mt19937 gen(99);
    const Eigen::Index n_shared = 3;
    const auto stations = makeStations(gen, n_shared, {2, 2}, {7, 7}, false);
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations)
        neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);

    EXPECT_FALSE(neq.isReduced());
    const Eigen::VectorXd x1 = neq.solveShared();  // lazily reduces
    EXPECT_TRUE(neq.isReduced());
    const Eigen::VectorXd x2 = neq.solveShared();  // no recompute needed
    EXPECT_TRUE(x1.isApprox(x2, 1e-15));
    // Explicit reduce() before solve must give the same answer.
    NetworkNormalEquations eager(n_shared);
    eager.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                        Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations)
        eager.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    eager.reduce();
    EXPECT_TRUE(eager.solveShared().isApprox(x1, 1e-12));
}

// Scale test: exactness must hold for many stations (the design targets "dozens").
TEST(NetworkNormalEquationsTest, MatchesDenseStackedSolve_ManyStations) {
    std::mt19937 gen(31337);
    const Eigen::Index n_shared = 8;
    std::vector<Eigen::Index> n_locals, m_obs;
    for (int r = 0; r < 24; ++r) {
        n_locals.push_back(1 + (r % 5));                 // 1..5 locals
        m_obs.push_back(n_shared + n_locals.back() + 4); // overdetermined
    }
    const auto stations = makeStations(gen, n_shared, n_locals, m_obs, true);
    const double sp = 1e-2, lp = 1e-2;

    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);
    Eigen::VectorXd x_s;
    std::vector<Eigen::VectorXd> x_r;
    schurSolve(stations, n_shared, sp, lp, x_s, x_r);

    EXPECT_TRUE(x_s.isApprox(dense.x.head(n_shared), 1e-8))
        << "max abs diff = "
        << (x_s - dense.x.head(n_shared)).cwiseAbs().maxCoeff();
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        EXPECT_TRUE(x_r[r].isApprox(dense.x.segment(dense.local_offset[r], n_r), 1e-8));
    }
}

// W5 seam: apply a datum constraint to a COPY of the reduced shared system, then
// solveSharedConstrained — must equal the dense solve with the same constraint
// applied to the full shared block (a pin on the shared block commutes with the
// Schur complement).  The kernel's own reduced system stays pristine.
TEST(NetworkNormalEquationsTest, SolveSharedConstrained_W5Seam) {
    std::mt19937 gen(7);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, {3, 2}, {10, 9}, true);
    const double sp = 1e-2, lp = 1e-2;

    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(sp * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations) {
        const Eigen::MatrixXd WA_s = s.W * s.A_s, WA_r = s.W * s.A_r;
        const Eigen::Index n_r = s.A_r.cols();
        Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r;
        N_rr += lp * Eigen::MatrixXd::Identity(n_r, n_r);
        neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                       WA_s.transpose() * s.y, WA_r.transpose() * s.y);
    }
    neq.reduce();

    // Round-trip: constrained-solve on the UNMODIFIED reduced system == solveShared.
    EXPECT_TRUE(neq.solveSharedConstrained(neq.reducedSharedNormals(),
                                           neq.reducedSharedRhs())
                    .isApprox(neq.solveShared(), 1e-12));

    // Snapshot the kernel's reduced system; solveSharedConstrained is const and
    // must not mutate it.
    const Eigen::MatrixXd reduced_before = neq.reducedSharedNormals();
    const Eigen::VectorXd reduced_rhs_before = neq.reducedSharedRhs();

    // Hard-pin shared param 0 to a target via a large-weight pseudo-obs, applied
    // identically to a copy of the reduced system and to the dense full system.
    const double kBig = 1e8, target = 0.37;
    Eigen::MatrixXd Nc = neq.reducedSharedNormals();
    Eigen::VectorXd uc = neq.reducedSharedRhs();
    Nc(0, 0) += kBig;
    uc(0) += kBig * target;
    const Eigen::VectorXd x_s = neq.solveSharedConstrained(Nc, uc);

    Eigen::MatrixXd Np = dense.N;
    Eigen::VectorXd up = dense.u;
    Np(0, 0) += kBig;
    up(0) += kBig * target;
    const Eigen::VectorXd x_dense_pinned = Np.ldlt().solve(up);

    EXPECT_NEAR(x_s(0), target, 1e-6);
    EXPECT_TRUE(x_s.isApprox(x_dense_pinned.head(n_shared), 1e-7))
        << "x_s=\n" << x_s.transpose() << "\nref=\n"
        << x_dense_pinned.head(n_shared).transpose();
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::VectorXd ref =
            x_dense_pinned.segment(dense.local_offset[r], n_r);
        EXPECT_TRUE(neq.reconstructStation(r, x_s).isApprox(ref, 1e-6));
    }
    // The kernel's stored reduced system was not mutated by the constrained solve.
    EXPECT_TRUE(neq.reducedSharedNormals().isApprox(reduced_before, 0.0));
    EXPECT_TRUE(neq.reducedSharedRhs().isApprox(reduced_rhs_before, 0.0));
}

// Moderately ill-conditioned local block (cond ~1e6) still reduces accurately.
TEST(NetworkNormalEquationsTest, IllConditionedLocalNormalsStaysAccurate) {
    std::mt19937 gen(555);
    const Eigen::Index n_shared = 3;
    auto stations = makeStations(gen, n_shared, {3, 3}, {12, 12}, false);
    // Scale station-0 local columns by widely different magnitudes → its N_rr
    // picks up a ~1e6 condition number while staying SPD.
    for (Eigen::Index j = 0; j < stations[0].A_r.cols(); ++j)
        stations[0].A_r.col(j) *= std::pow(10.0, -3.0 * double(j));
    const double sp = 1e-3, lp = 1e-6;

    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);
    Eigen::VectorXd x_s;
    std::vector<Eigen::VectorXd> x_r;
    schurSolve(stations, n_shared, sp, lp, x_s, x_r);

    EXPECT_TRUE(x_s.isApprox(dense.x.head(n_shared), 1e-6))
        << "max abs diff = "
        << (x_s - dense.x.head(n_shared)).cwiseAbs().maxCoeff();
}

// A grossly asymmetric local/shared block is rejected (would otherwise be
// silently half-used by LDLᵀ / the final symmetrization).
TEST(NetworkNormalEquationsTest, RejectsAsymmetricInput) {
    NetworkNormalEquations neq(2);
    Eigen::MatrixXd N_ss(2, 2); N_ss << 3, 1, 1, 4;
    Eigen::MatrixXd N_sr(2, 2); N_sr.setZero();
    Eigen::MatrixXd N_rr(2, 2); N_rr << 5, 2, 9, 5;  // asymmetric
    Eigen::VectorXd u_s(2); u_s.setZero();
    Eigen::VectorXd u_r(2); u_r.setZero();
    EXPECT_THROW(neq.addStation(N_ss, N_sr, N_rr, u_s, u_r), std::invalid_argument);
}

// A non-positive-definite (rank-deficient) local block is rejected at addStation,
// where the diagnosis is unambiguous — not silently pseudo-inverted.
TEST(NetworkNormalEquationsTest, RejectsRankDeficientLocalBlock) {
    NetworkNormalEquations neq(2);
    Eigen::MatrixXd N_ss(2, 2); N_ss << 3, 1, 1, 4;
    Eigen::MatrixXd N_sr(2, 2); N_sr.setZero();
    // Rank-1 (singular) 2×2 symmetric block: [[1,1],[1,1]] has eigenvalues {2,0}.
    Eigen::MatrixXd N_rr(2, 2); N_rr << 1, 1, 1, 1;
    Eigen::VectorXd u_s(2); u_s.setZero();
    Eigen::VectorXd u_r(2); u_r.setZero();
    EXPECT_THROW(neq.addStation(N_ss, N_sr, N_rr, u_s, u_r), std::invalid_argument);
}

// The diagonal-weight overload throws cleanly on a dimension mismatch instead of
// tripping an Eigen assertion when forming w.asDiagonal()*A_r.
TEST(NetworkNormalEquationsTest, DiagonalWeightDimMismatchThrows) {
    NetworkNormalEquations neq(2);
    Eigen::MatrixXd A_s(5, 2); A_s.setOnes();
    Eigen::MatrixXd A_r(4, 1); A_r.setOnes();  // wrong row count (4 != 5)
    Eigen::VectorXd w(5); w.setOnes();
    Eigen::VectorXd y(5); y.setZero();
    EXPECT_THROW(neq.addStationObservations(A_s, A_r, w, y), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// reconstructStationCovariance (producer-side network AR, Phase 1) — the #1
// correctness risk.  Validate the marginal-covariance identity against a fully
// independent dense full-inverse, both unpinned and with a shared param treated
// as exactly known (the datum-as-known frame the DD ambiguities live in).
// ---------------------------------------------------------------------------

// Build the Schur kernel from raw station data + isotropic priors and reduce.
NetworkNormalEquations buildReducedNeq(const std::vector<StationData>& stations,
                                       Eigen::Index n_shared, double sp, double lp) {
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(sp * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations) {
        const Eigen::MatrixXd WA_s = s.W * s.A_s, WA_r = s.W * s.A_r;
        const Eigen::Index n_r = s.A_r.cols();
        Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r;
        N_rr += lp * Eigen::MatrixXd::Identity(n_r, n_r);
        neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                       WA_s.transpose() * s.y, WA_r.transpose() * s.y);
    }
    neq.reduce();
    return neq;
}

// Unpinned: Cov(x_r) from reconstructStationCovariance must equal the (r,r)
// block of the full bordered inverse, with the full shared covariance fed in.
TEST(NetworkNormalEquationsTest, ConditionalCovarianceMatchesFullInverseBlock) {
    std::mt19937 gen(20260629);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, {3, 2}, {10, 9}, true);
    const double sp = 1e-2, lp = 1e-2;
    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);

    // Brute-force marginal covariance of ALL params = inverse of the full
    // bordered normal matrix (assembled independently of the Schur kernel).
    const Eigen::MatrixXd cov_full = dense.N.inverse();

    NetworkNormalEquations neq = buildReducedNeq(stations, n_shared, sp, lp);
    // Full shared covariance = inverse of the reduced shared normals (= the
    // (s,s) block of cov_full by the Schur-complement identity).
    const Eigen::MatrixXd cov_xs = neq.reducedSharedNormals().inverse();

    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::MatrixXd cov_rr = neq.reconstructStationCovariance(r, cov_xs);
        const Eigen::MatrixXd ref =
            cov_full.block(dense.local_offset[r], dense.local_offset[r], n_r, n_r);
        EXPECT_TRUE(cov_rr.isApprox(ref, 1e-8))
            << "station " << r << " cov_rr=\n" << cov_rr << "\nref=\n" << ref;
        EXPECT_TRUE(cov_rr.isApprox(cov_rr.transpose(), 1e-12));  // symmetric
    }
}

// Pinned-as-known: feeding cov_free with shared param 0's row/col zeroed must
// reproduce the (r,r) block of the inverse of the system CONDITIONED on x_0
// exact (its row/col removed) — the datum-as-known frame.  Using the full
// (un-zeroed) shared covariance instead would inflate the DD ambiguity Q.
TEST(NetworkNormalEquationsTest, ConditionalCovarianceTreatsPinnedSharedAsKnown) {
    std::mt19937 gen(424242);
    const Eigen::Index n_shared = 3;  // index 0 pinned (exact); 1..2 free
    const auto stations = makeStations(gen, n_shared, {2, 3}, {9, 11}, true);
    const double sp = 1e-2, lp = 1e-2;
    const DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);

    // Reference: condition on shared param 0 KNOWN by removing its row/col from
    // the full normal, then invert.  (Index 0 first → removal = bottom-right.)
    const Eigen::Index P = dense.N.rows();
    const Eigen::MatrixXd cov_cond = dense.N.bottomRightCorner(P - 1, P - 1).inverse();

    NetworkNormalEquations neq = buildReducedNeq(stations, n_shared, sp, lp);
    // cov_free: free-shared block = inverse of the reduced normals with row/col 0
    // removed; pinned column 0 left zero (treated as exact).
    const Eigen::MatrixXd reduced = neq.reducedSharedNormals();
    Eigen::MatrixXd cov_free = Eigen::MatrixXd::Zero(n_shared, n_shared);
    cov_free.bottomRightCorner(n_shared - 1, n_shared - 1) =
        reduced.bottomRightCorner(n_shared - 1, n_shared - 1).inverse();

    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::MatrixXd cov_rr = neq.reconstructStationCovariance(r, cov_free);
        // Station r's block offset shifts down by 1 in the conditioned system.
        const Eigen::MatrixXd ref = cov_cond.block(
            dense.local_offset[r] - 1, dense.local_offset[r] - 1, n_r, n_r);
        EXPECT_TRUE(cov_rr.isApprox(ref, 1e-8))
            << "station " << r << " cov_rr=\n" << cov_rr << "\nref=\n" << ref;
    }
}

// addSharedInfo is REDUCTION-PRESERVING: a shared-border delta added AFTER reduce()
// is folded into the cached reduced system in place (reduction stays current),
// yielding the SAME result as adding it before reduce() and recomputing.  This is
// the sentinel for the "stop reducing twice" optimization — a regression that
// instead dropped the post-reduce prior (the driver's D2 carry-forward) would
// surface here as an O(1) mismatch, not merely a ULP one.
TEST(NetworkNormalEquationsTest, AddSharedInfoIsReductionPreservingDiagonal) {
    std::mt19937 gen(123);
    const Eigen::Index n_shared = 5;
    const auto stations = makeStations(gen, n_shared, {3, 2, 4}, {10, 9, 11}, true);
    auto build = [&](NetworkNormalEquations& neq) {
        neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                          Eigen::VectorXd::Zero(n_shared));
        for (const auto& s : stations)
            neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    };

    // Diagonal delta (the D2 carry-forward shape) folded AFTER reduce().
    Eigen::VectorXd dvec(n_shared); dvec << 0.5, 1.0, 0.25, 2.0, 0.1;
    const Eigen::MatrixXd dDiag = Eigen::MatrixXd(dvec.asDiagonal());
    Eigen::VectorXd drhs(n_shared); drhs << 1, -2, 3, 0.5, -1;

    NetworkNormalEquations post(n_shared);
    build(post);
    post.reduce();
    const Eigen::MatrixXd N_before = post.reducedSharedNormals();
    const Eigen::VectorXd u_before = post.reducedSharedRhs();

    post.addSharedInfo(dDiag, drhs);
    EXPECT_TRUE(post.isReduced());  // reduction stays current — no re-reduce needed
    EXPECT_TRUE(post.reducedSharedNormals().isApprox(N_before + dDiag, 1e-12));
    EXPECT_TRUE(post.reducedSharedRhs().isApprox(u_before + drhs, 1e-12));
    // A subsequent reduce() must be a no-op (guarded), preserving the folded delta.
    post.reduce();
    EXPECT_TRUE(post.reducedSharedNormals().isApprox(N_before + dDiag, 1e-12));

    // From-scratch reference: same delta added BEFORE reduce() (full recompute).
    NetworkNormalEquations pre(n_shared);
    build(pre);
    pre.addSharedInfo(dDiag, drhs);
    pre.reduce();
    EXPECT_TRUE(post.reducedSharedNormals().isApprox(pre.reducedSharedNormals(), 1e-12));
    EXPECT_TRUE(post.reducedSharedRhs().isApprox(pre.reducedSharedRhs(), 1e-12));
}

// The in-place fold symmetrizes the increment, so it is also exact for a
// NON-diagonal symmetric delta (e.g. a wHᵀH constraint increment) — guards the
// 0.5·(Δ+Δᵀ) hardening against a future post-reduce non-diagonal caller.
TEST(NetworkNormalEquationsTest, AddSharedInfoIsReductionPreservingSymmetric) {
    std::mt19937 gen(321);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, {2, 3}, {9, 10}, true);
    auto build = [&](NetworkNormalEquations& neq) {
        neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                          Eigen::VectorXd::Zero(n_shared));
        for (const auto& s : stations)
            neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    };
    const Eigen::MatrixXd R = makeRand(gen, n_shared, n_shared);
    const Eigen::MatrixXd Sdelta = 0.5 * (R + R.transpose());   // symmetric
    Eigen::VectorXd drhs = makeRand(gen, n_shared, 1);

    NetworkNormalEquations post(n_shared);
    build(post);
    post.reduce();
    post.addSharedInfo(Sdelta, drhs);

    NetworkNormalEquations pre(n_shared);
    build(pre);
    pre.addSharedInfo(Sdelta, drhs);
    pre.reduce();

    EXPECT_TRUE(post.reducedSharedNormals().isApprox(pre.reducedSharedNormals(), 1e-12));
    EXPECT_TRUE(post.reducedSharedRhs().isApprox(pre.reducedSharedRhs(), 1e-12));
}

#ifdef _OPENMP
// The parallel Schur reduction (reduce(n_threads>1)) matches the serial fold to
// within ULP-reorder tolerance, for every thread count, and yields the same solve.
TEST(NetworkNormalEquationsTest, ParallelReduceMatchesSerial) {
    std::mt19937 gen(4242);
    const Eigen::Index n_shared = 8;
    std::vector<Eigen::Index> n_locals, m_obs;
    for (int r = 0; r < 17; ++r) {            // 17 stations exercises uneven chunks
        n_locals.push_back(1 + (r % 4));
        m_obs.push_back(n_shared + n_locals.back() + 3);
    }
    const auto stations = makeStations(gen, n_shared, n_locals, m_obs, true);
    auto build = [&](NetworkNormalEquations& neq) {
        neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                          Eigen::VectorXd::Zero(n_shared));
        for (const auto& s : stations)
            neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    };

    NetworkNormalEquations serial(n_shared);
    build(serial);
    serial.reduce(1);

    for (int nt : {2, 4, 8}) {
        NetworkNormalEquations par(n_shared);
        build(par);
        par.reduce(nt);
        EXPECT_TRUE(par.reducedSharedNormals().isApprox(serial.reducedSharedNormals(), 1e-10))
            << "threads=" << nt;
        EXPECT_TRUE(par.reducedSharedRhs().isApprox(serial.reducedSharedRhs(), 1e-10))
            << "threads=" << nt;
        EXPECT_TRUE(par.solveShared().isApprox(serial.solveShared(), 1e-9))
            << "threads=" << nt;
    }
}
#endif

// ===========================================================================
// Phase-leveling carried-ambiguity prior (Increment 1): addStationPrior
// ===========================================================================

// addStationPrior folds a diagonal information increment Λ into one station's
// N_rr and η into u_r — it must reproduce the dense stacked solve with the SAME
// diagonal augmentation applied to that station's local block.
TEST(NetworkNormalEquationsTest, AddStationPriorMatchesDenseAugmentedSolve) {
    std::mt19937 gen(20260630);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, /*n_locals=*/{3, 4},
                                       /*m_obs=*/{10, 12}, /*correlated=*/true);
    const double sp = 1e-2, lp = 1e-2;

    // A carried prior on station 1, local columns {0, 2}.
    const std::size_t pr_sta = 1;
    const std::vector<Eigen::Index> cols{0, 2};
    Eigen::VectorXd dN(2); dN << 3.7, 0.9;     // Λ (info weights)
    Eigen::VectorXd du(2); du << 1.1, -2.3;    // η (info-form RHS)

    // Dense reference: build the full bordered N/u (with the isotropic priors),
    // then add Λ on the diagonal and η on the RHS of station 1's local columns.
    DenseSolution dense = denseStackedSolve(stations, n_shared, sp, lp);
    Eigen::MatrixXd N = dense.N;
    Eigen::VectorXd u = dense.u;
    const Eigen::Index off = dense.local_offset[pr_sta];
    for (Eigen::Index i = 0; i < cols.size(); ++i) {
        N(off + cols[i], off + cols[i]) += dN(i);
        u(off + cols[i]) += du(i);
    }
    const Eigen::VectorXd x_ref = N.ldlt().solve(u);

    // Schur path: addStation each (local prior folded into N_rr), then
    // addStationPrior on station 1, then solve + reconstruct.
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(sp * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations) {
        const Eigen::MatrixXd WA_s = s.W * s.A_s;
        const Eigen::MatrixXd WA_r = s.W * s.A_r;
        const Eigen::Index n_r = s.A_r.cols();
        Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r;
        N_rr += lp * Eigen::MatrixXd::Identity(n_r, n_r);
        neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                       WA_s.transpose() * s.y, WA_r.transpose() * s.y);
    }
    neq.addStationPrior(pr_sta, cols, dN, du);
    const Eigen::VectorXd x_s = neq.solveShared();

    EXPECT_TRUE(x_s.isApprox(x_ref.head(n_shared), 1e-9))
        << "x_s=\n" << x_s.transpose() << "\nref=\n" << x_ref.head(n_shared).transpose();
    for (std::size_t r = 0; r < stations.size(); ++r) {
        const Eigen::Index n_r = stations[r].A_r.cols();
        const Eigen::VectorXd ref = x_ref.segment(dense.local_offset[r], n_r);
        EXPECT_TRUE(neq.reconstructStation(r, x_s).isApprox(ref, 1e-9))
            << "station " << r << " mismatch";
    }
}

// An EMPTY addStationPrior is a genuine no-op: byte-identical to never calling it
// (the OFF-state byte-identity guarantee at the kernel level).
TEST(NetworkNormalEquationsTest, AddStationPriorEmptyIsNoOp) {
    std::mt19937 gen(555);
    const Eigen::Index n_shared = 3;
    const auto stations = makeStations(gen, n_shared, {2, 3}, {8, 9}, false);
    auto build = [&](NetworkNormalEquations& neq) {
        neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                          Eigen::VectorXd::Zero(n_shared));
        for (const auto& s : stations)
            neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    };
    NetworkNormalEquations plain(n_shared);   build(plain);
    NetworkNormalEquations empty(n_shared);   build(empty);
    empty.addStationPrior(0, {}, Eigen::VectorXd(), Eigen::VectorXd());

    const Eigen::VectorXd xp = plain.solveShared();
    const Eigen::VectorXd xe = empty.solveShared();
    EXPECT_EQ((xp - xe).cwiseAbs().maxCoeff(), 0.0);  // exact, not just approx
    EXPECT_TRUE(empty.reducedSharedNormals().isApprox(plain.reducedSharedNormals(), 0.0));
}

// addStationPrior validates its inputs.
TEST(NetworkNormalEquationsTest, AddStationPriorRejectsBadInputs) {
    NetworkNormalEquations neq(2);
    Eigen::MatrixXd N_ss(2, 2); N_ss << 3, 1, 1, 4;
    Eigen::MatrixXd N_sr(2, 2); N_sr.setZero();
    Eigen::MatrixXd N_rr(2, 2); N_rr << 5, 0, 0, 6;
    Eigen::VectorXd u_s(2); u_s << 1, 2;
    Eigen::VectorXd u_r(2); u_r << 0, 0;
    neq.addStation(N_ss, N_sr, N_rr, u_s, u_r);

    Eigen::VectorXd one(1); one << 1.0;
    Eigen::VectorXd neg(1); neg << -1.0;
    EXPECT_THROW(neq.addStationPrior(5, {0}, one, one), std::out_of_range);   // bad station
    EXPECT_THROW(neq.addStationPrior(0, {0}, one, Eigen::VectorXd()),         // size mismatch
                 std::invalid_argument);
    EXPECT_THROW(neq.addStationPrior(0, {7}, one, one), std::invalid_argument);  // col out of range
    EXPECT_THROW(neq.addStationPrior(0, {0}, neg, one), std::invalid_argument);  // negative info
}

// Reduce-once instrumentation: the carried-prior path (addStation → addStationPrior
// → solve) must trigger EXACTLY ONE per-station Schur recompute, and a post-reduce
// addStationPrior must correctly invalidate the cache (forcing a second recompute).
TEST(NetworkNormalEquationsTest, AddStationPriorReduceOnceInstrumentation) {
    std::mt19937 gen(8675309);
    const Eigen::Index n_shared = 3;
    const auto stations = makeStations(gen, n_shared, {2, 2}, {8, 8}, false);
    auto build = [&](NetworkNormalEquations& neq) {
        neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                          Eigen::VectorXd::Zero(n_shared));
        for (const auto& s : stations)
            neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    };

    // Path mirroring the driver: prior injected BEFORE the first reduce → one recompute.
    NetworkNormalEquations neq(n_shared);
    build(neq);
    Eigen::VectorXd one(1); one << 5.0;
    neq.addStationPrior(0, {0}, one, one);
    EXPECT_EQ(neq.reduceRecomputeCount(), 0u);
    neq.solveShared();
    EXPECT_EQ(neq.reduceRecomputeCount(), 1u);
    neq.solveShared();                              // idempotent — no new recompute
    EXPECT_EQ(neq.reduceRecomputeCount(), 1u);

    // A post-reduce prior must mark the cache stale (the design's "stray post-gate
    // injection doubles the serial path" — detectable via the counter).
    neq.addStationPrior(1, {1}, one, one);
    neq.reduce();
    EXPECT_EQ(neq.reduceRecomputeCount(), 2u);

    // reset() clears the counter.
    neq.reset(n_shared);
    EXPECT_EQ(neq.reduceRecomputeCount(), 0u);
}

// reconstructStationCovarianceDiag returns exactly the diagonal of the full
// reconstructStationCovariance for the requested columns.
TEST(NetworkNormalEquationsTest, CovarianceDiagMatchesFullDiagonal) {
    std::mt19937 gen(13);
    const Eigen::Index n_shared = 4;
    const auto stations = makeStations(gen, n_shared, {3, 4}, {11, 12}, true);
    NetworkNormalEquations neq(n_shared);
    neq.addSharedInfo(1e-2 * Eigen::MatrixXd::Identity(n_shared, n_shared),
                      Eigen::VectorXd::Zero(n_shared));
    for (const auto& s : stations)
        neq.addStationObservations(s.A_s, s.A_r, s.W, s.y);
    neq.reduce();
    // An arbitrary SPD shared covariance to propagate.
    const Eigen::MatrixXd cov_free = makeSpd(gen, n_shared).inverse();

    const std::size_t sta = 1;
    const Eigen::MatrixXd full = neq.reconstructStationCovariance(sta, cov_free);
    const std::vector<Eigen::Index> cols{0, 2, 3};
    const Eigen::VectorXd diag = neq.reconstructStationCovarianceDiag(sta, cols, cov_free);
    ASSERT_EQ(diag.size(), static_cast<Eigen::Index>(cols.size()));
    for (Eigen::Index i = 0; i < diag.size(); ++i)
        EXPECT_NEAR(diag(i), full(cols[i], cols[i]), 1e-10);
}

// FIREWALL (design §4): a carried prior is reduction-orthogonal to a SHARED gauge
// direction the data leaves free.  A station ambiguity carry must NOT fill an
// unpinned shared-clock gauge — so a marginal / under-determined epoch still trips
// the null-space gate (withholds) with the carry ON.  Here two shared columns are
// made co-linear (analogue of an unpinned per-constellation clock datum): the
// direction e0−e1 is exactly null in the reduced normals, and adding a local carry
// must leave it exactly null while still moving the reconstructed locals.
TEST(NetworkNormalEquationsTest, CarryDoesNotFillSharedGauge_Firewall) {
    std::mt19937 gen(31459);
    const Eigen::Index n_shared = 3;
    auto stations = makeStations(gen, n_shared, /*n_locals=*/{2, 3}, /*m_obs=*/{9, 10},
                                 /*correlated=*/false);
    // Force shared columns 0 and 1 to be IDENTICAL at every station → only their
    // difference is unobservable: v = e0 − e1 is a pure gauge (A v = 0 everywhere).
    for (auto& s : stations) s.A_s.col(1) = s.A_s.col(0);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(n_shared);
    v(0) = 1.0; v(1) = -1.0;

    auto build = [&](NetworkNormalEquations& neq) {
        // NO shared prior on the gauge: leave the deficiency in place (a real
        // unpinned datum); a tiny prior only on column 2 keeps the rest sensible.
        for (const auto& s : stations) {
            const Eigen::MatrixXd WA_s = s.W * s.A_s;
            const Eigen::MatrixXd WA_r = s.W * s.A_r;
            const Eigen::Index n_r = s.A_r.cols();
            Eigen::MatrixXd N_rr = s.A_r.transpose() * WA_r;
            N_rr += 1e-2 * Eigen::MatrixXd::Identity(n_r, n_r);  // PD local block
            neq.addStation(s.A_s.transpose() * WA_s, s.A_s.transpose() * WA_r, N_rr,
                           WA_s.transpose() * s.y, WA_r.transpose() * s.y);
        }
    };

    NetworkNormalEquations base(n_shared);
    build(base);
    base.reduce();
    const double gauge_before = (base.reducedSharedNormals() * v).norm();
    EXPECT_LT(gauge_before, 1e-8) << "gauge should be (near) null before the carry";

    // Same system + a carried prior on station 1's local ambiguity columns.
    NetworkNormalEquations carried(n_shared);
    build(carried);
    Eigen::VectorXd dN(2); dN << 4.0, 2.0;
    Eigen::VectorXd du(2); du << 1.5, -0.5;
    carried.addStationPrior(1, {0, 2}, dN, du);
    carried.reduce();

    // The shared gauge direction stays EXACTLY as null as before — the carry could
    // not pin it (it touches only local columns).  The gate's null-space count is
    // therefore unchanged → an under-determined epoch still withholds.
    const double gauge_after = (carried.reducedSharedNormals() * v).norm();
    EXPECT_LT(gauge_after, 1e-8) << "carry must NOT fill the shared gauge";

    // ...yet the carry is genuinely ACTIVE: it moves the reconstructed locals (so
    // this is a real firewall, not a vacuous no-op).  Solve the constrained system
    // (pin the gauge so a unique solution exists) and compare station-1 locals.
    Eigen::MatrixXd Cb = base.reducedSharedNormals();
    Eigen::MatrixXd Cc = carried.reducedSharedNormals();
    Eigen::VectorXd ub = base.reducedSharedRhs();
    Eigen::VectorXd uc = carried.reducedSharedRhs();
    // Pin column 1 to 0 (resolve the e0−e1 gauge) so a unique solution exists.
    auto pin = [&](Eigen::MatrixXd& C, Eigen::VectorXd& u) {
        C.row(1).setZero(); C.col(1).setZero(); C(1, 1) = 1.0; u(1) = 0.0;
    };
    pin(Cb, ub); pin(Cc, uc);
    const Eigen::VectorXd xb = base.reconstructStation(1, Cb.ldlt().solve(ub));
    const Eigen::VectorXd xc = carried.reconstructStation(1, Cc.ldlt().solve(uc));
    EXPECT_GT((xb - xc).cwiseAbs().maxCoeff(), 1e-6)
        << "the carry should change station 1's locals (active prior)";
}

} // namespace
} // namespace GnssProcessor
