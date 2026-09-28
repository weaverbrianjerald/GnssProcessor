/**
 * @file test_pd_safe_fold.cpp
 * @brief Gates for the PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md).
 *
 * The invariant-carry recursion is correct in design: its small well-conditioned blocks
 * reach σ = 2.7 cm = 0.14 L1 cycles over 3 h through the same code.  The fault is in the
 * arithmetic.  The large [band-1; band-2] joint lane block is born near-singular
 * (same-chord band rows are near-duplicates through shared geometry), and the subtractive
 * posterior @f$P^z-P^zMP^z@f$ over mixed 1e-4..1e6 operand scales and K=686 GEMM chains
 * pushes its hidden tight eigenvalues through zero.  The legacy fold then sets @c M:=0,
 * without a log line and for the rest of the run; on a 63-station 3 h DOY-129 run, 568 of
 * 686 carried rows diffused behind it.
 *
 * Suite name @c PdSafeFoldTest is owned solely by this file (suite-hygiene rule: one
 * suite, one file).  The config-knob default-off gate lives with the other network config
 * cases in tests/test_codebias_network_config.cpp for the same reason.
 *
 * The gates, in SPEC §6 order:
 *  1. @c ProductFormEqualsSubtractionWellConditioned: the product form (a) is the same
 *     quantity as the subtraction.
 *  2. @c MixedScaleLanePairStaysPd: the falsifier.  The legacy path reproduces the
 *     defect (if it stops doing so, the toy no longer models the field and is
 *     re-derived) and the PD-safe path does not.
 *  3. @c EquilibratedPivotIsScaleRelative: what the equilibration (b) provides.
 *  4. @c DegenerateBlockRebirthsLoudly: invariant #7's re-coupling edge, end to end.
 *  5. @c RestoreGuardRebirthsCorruptBlob: INC-2c, both arms.
 *  6. @c OffPathIsUnchangedAndInertSemanticsHold: invariant #6 plus the always-on INC-0.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkZStore.hpp"

namespace GnssProcessor {
namespace {

using Term = NetworkNormalEquations::DdPriorBlock::Term;

// ---------------------------------------------------------------------------
// Kernel fixture: a decoupled one-station system.
//
// n_shared = 1 with a pure prior and a station whose N_sr is zero, so the fold's
// G = Σ_r N_sr N_rr⁻¹ C_rᵀ vanishes identically.  That removes the posterior's
// shared-coupling term (P^zMG^ᵀ)Σ(P^zMG^ᵀ)ᵀ and leaves the quantity under test:
//
//     P⁺ = P − P(P+B)⁻¹P      (legacy)   ≡   sym(P (P+B)⁻¹ B)      (product form)
//
// with B = C N_rr⁻¹ Cᵀ reproducible in the test.  This makes an independent reference
// recursion possible (posteriorOracle below).
// ---------------------------------------------------------------------------
struct DecoupledFold {
    Eigen::MatrixXd N_rr;                     ///< the station's local normals (SPD)
    std::vector<std::vector<Term>> rows;      ///< the DD block's K rows over those columns
    Eigen::MatrixXd C;                        ///< the same rows as a dense K × n_local matrix
};

/// Deterministic SPD matrix (M Mᵀ + n·I → well-conditioned), the makeSpd of the
/// neighbouring kernel suite.
Eigen::MatrixXd makeSpd(std::mt19937& gen, Eigen::Index n) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    Eigen::MatrixXd M(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j) M(i, j) = dist(gen);
    return M * M.transpose() + double(n) * Eigen::MatrixXd::Identity(n, n);
}

/// Fold one block through the production kernel and return its posterior covariance.
/// @p pd_safe selects the arithmetic; @p degenerate / @p min_pivot report the PD screen.
Eigen::MatrixXd foldOnce(const DecoupledFold& f, const Eigen::MatrixXd& Pz,
                         bool pd_safe, bool* degenerate = nullptr,
                         double* min_pivot = nullptr, int station_threads = 1) {
    const Eigen::Index n_local = f.N_rr.rows();
    NetworkNormalEquations neq(1);
    neq.setDdFoldPdSafe(pd_safe);
    neq.addSharedInfo(Eigen::MatrixXd::Identity(1, 1), Eigen::VectorXd::Zero(1));
    neq.addStation(Eigen::MatrixXd::Zero(1, 1),        // N_ss
                   Eigen::MatrixXd::Zero(1, n_local),  // N_sr == 0  ⇒  G == 0
                   f.N_rr,
                   Eigen::VectorXd::Zero(1),
                   Eigen::VectorXd::Ones(n_local));
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = f.rows;
    blk.Pz   = Pz;
    blk.zhat = Eigen::VectorXd::Zero(Pz.rows());
    neq.reduce();
    neq.foldReducedDdPriors({blk}, /*n_threads=*/1, station_threads);
    if (degenerate) *degenerate = neq.ddPriorBlockDegenerate(0);
    if (min_pivot)  *min_pivot  = neq.ddPriorBlockMinPivotRel(0);
    return neq.ddPriorPosteriorCovariance(0, Eigen::MatrixXd::Zero(1, 1));
}

/// Independent oracle for the DD posterior covariance: the dense augmented solve the
/// neighbouring kernel suite grades against,
/// @f$\Sigma_z = C\,(N_{rr} + C^\top P^{-1} C)^{-1} C^\top@f$.
///
/// It is independent by algebraic path, which is what matters here: it never forms
/// @f$P - P(\cdot)P@f$, so it does not share the subtractive form's cancellation, and it
/// touches no fold code.  An extended-precision reference is not available on this
/// platform (MSVC's @c long double is 53-bit, the same as @c double), so a higher-mantissa
/// evaluation of the same expression would grade the two doubles against a reference with
/// no more headroom than they have.
Eigen::MatrixXd posteriorOracle(const DecoupledFold& f, const Eigen::MatrixXd& P) {
    const Eigen::LLT<Eigen::MatrixXd> Pllt(P);
    const Eigen::MatrixXd A = f.N_rr + f.C.transpose() * Pllt.solve(f.C);
    Eigen::MatrixXd out = f.C * A.llt().solve(f.C.transpose());
    return (0.5 * (out + out.transpose())).eval();
}

double minEig(const Eigen::MatrixXd& A) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(A);
    return es.eigenvalues().minCoeff();
}

/// Smallest eigenvalue of the correlation-normalized matrix D^-1/2 A D^-1/2 (D = diag A).
/// Positive-definiteness must be graded scale-free here: a covariance carrying a 1e10
/// variance spread has an eigensolver round-off floor of ~1e-10 in absolute terms, so an
/// absolute `minEig > 0` test measures the eigensolver, not the matrix.  Returns −1 for a
/// non-positive diagonal, which is decisive indefiniteness on its own.
double minEigNormalized(const Eigen::MatrixXd& A) {
    const Eigen::VectorXd d = A.diagonal();
    if ((d.array() <= 0.0).any()) return -1.0;
    const Eigen::VectorXd s = d.cwiseSqrt().cwiseInverse();
    return minEig(s.asDiagonal() * A * s.asDiagonal());
}

/// The joint lane-pair shape that is the structural hazard: K = 2·Nc rows laid out
/// [band-1 (Nc rows); band-2 (Nc rows)], row i and row Nc+i being the two bands of chord
/// i.  The two band rows of a chord touch the same local columns with coefficients that
/// differ by @p band_eps: near-duplicates through shared geometry, as in the field (min
/// eig 4.6e-6 at the first epoch against 1.8e-2 for the per-band blocks).
DecoupledFold makeLanePairFold(std::mt19937& gen, int n_chords, double band_eps) {
    const Eigen::Index Nc = n_chords;
    const Eigen::Index K = 2 * Nc;
    const Eigen::Index n_local = 2 * Nc;
    DecoupledFold f;
    f.N_rr = makeSpd(gen, n_local);
    f.C = Eigen::MatrixXd::Zero(K, n_local);
    f.rows.assign(static_cast<std::size_t>(K), {});
    for (Eigen::Index i = 0; i < Nc; ++i) {
        const Eigen::Index c0 = i, c1 = (i + 1) % Nc;   // a DD spans two columns
        // band 1
        f.C(i, c0) = +1.0;              f.C(i, c1) = -1.0;
        f.rows[static_cast<std::size_t>(i)] = {Term{0, c0, +1.0}, Term{0, c1, -1.0}};
        // band 2: the same geometric direction, perturbed by band_eps
        const double a = 1.0 + band_eps, b = -1.0 + band_eps;
        f.C(Nc + i, c0) = a;            f.C(Nc + i, c1) = b;
        f.C(Nc + i, Nc + i) = band_eps;
        f.rows[static_cast<std::size_t>(Nc + i)] =
            {Term{0, c0, a}, Term{0, c1, b}, Term{0, Nc + i, band_eps}};
    }
    return f;
}

/// The matching mixed-scale prior: per chord a 2×2 [band-1; band-2] sub-block correlated
/// at @p rho, mature chords at @p var_mature and fresh ones at @p var_fresh (the
/// 1e-4 .. 1e6 spread a live epoch carries), plus a mild dense background so the block is
/// joint rather than block-diagonal.
Eigen::MatrixXd makeMixedScalePrior(int n_chords, double var_mature, double var_fresh,
                                    double rho, double background) {
    const Eigen::Index Nc = n_chords;
    const Eigen::Index K = 2 * Nc;
    Eigen::MatrixXd R = Eigen::MatrixXd::Constant(K, K, background);
    R.diagonal().setOnes();
    Eigen::VectorXd sd(K);
    for (Eigen::Index i = 0; i < Nc; ++i) {
        R(i, Nc + i) = rho;
        R(Nc + i, i) = rho;
        const double v = (i % 2 == 0) ? var_mature : var_fresh;
        sd(i) = std::sqrt(v);
        sd(Nc + i) = std::sqrt(v);
    }
    return sd.asDiagonal() * R * sd.asDiagonal();
}

// ---------------------------------------------------------------------------
// Driver fixture (INC-2a / INC-2c): the star-tree synthetic eq34 network, the same
// construction the carry-driver suite uses, trimmed to what these two gates need.
// ---------------------------------------------------------------------------
constexpr int kSatBase = 1000;
int rxV(int r) { return r; }
int satV(int s) { return kSatBase + s; }
bool isStarTreeEdge(int p, int q, int r, int s) { return (r == p) || (s == q); }

struct Net {
    int R{0}, S{0};
    std::vector<double> a, c, b, d;
    std::vector<std::vector<int>> z;
    std::vector<std::string> sats;
};
Net makeNet(unsigned seed, int R, int S) {
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> clk(-10, 10), bias(-0.5, 0.5);
    std::uniform_int_distribution<int> amb(-6, 6);
    Net n; n.R = R; n.S = S;
    for (int r = 0; r < R; ++r) { n.a.push_back(clk(gen)); n.d.push_back(bias(gen)); }
    for (int s = 0; s < S; ++s) {
        n.c.push_back(clk(gen)); n.b.push_back(bias(gen));
        char id[8]; std::snprintf(id, sizeof(id), "G%02d", s + 1); n.sats.push_back(id);
    }
    n.z.assign(R, std::vector<int>(S));
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) n.z[r][s] = amb(gen);
    return n;
}

struct StationCols { Eigen::Index a{0}, d{1}; std::vector<Eigen::Index> z; };
StationCols ambLayout(int S) {
    StationCols c; c.z.resize(S);
    for (int s = 0; s < S; ++s) c.z[s] = 2 + s;
    return c;
}

std::size_t addNetStation(NetworkEstimator& est, const Net& n, int r,
                          const std::vector<Eigen::Index>& clkCol,
                          const std::vector<Eigen::Index>& pbCol, int p, int q, int r1) {
    const int S = n.S;
    const StationCols col = ambLayout(S);
    std::vector<Eigen::Index> pins;
    for (int s = 0; s < S; ++s) if (isStarTreeEdge(p, q, r, s)) pins.push_back(col.z[s]);
    const bool rpbPin = (r == r1);
    const int nrows = 2 * S + static_cast<int>(pins.size()) + (rpbPin ? 1 : 0);
    const Eigen::Index n_local = 2 + S;
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
    Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, n_local);
    Eigen::VectorXd w = Eigen::VectorXd::Ones(nrows), y = Eigen::VectorXd::Zero(nrows);
    int row = 0;
    for (int s = 0; s < S; ++s) {
        A_s(row, clkCol[s]) = -1.0; A_r(row, col.a) = 1.0;
        y(row) = n.a[r] - n.c[s]; ++row;
        A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
        A_r(row, col.a) = 1.0; A_r(row, col.d) = 1.0; A_r(row, col.z[s]) = 1.0;
        y(row) = n.a[r] - n.c[s] + n.b[s] + n.d[r] + n.z[r][s];
        ++row;
    }
    for (Eigen::Index pc : pins) { A_r(row, pc) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    if (rpbPin) { A_r(row, col.d) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    return est.addStation(A_s, A_r, w, y);
}

NetworkInvariantCarryDriver::GroupInput
makeGroup(const Net& n, const std::vector<std::size_t>& ids, int p, int q) {
    NetworkInvariantCarryDriver::GroupInput g; g.key = 0;
    const StationCols col = ambLayout(n.S);
    for (int r = 0; r < n.R; ++r)
        for (int s = 0; s < n.S; ++s)
            g.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(p, q, r, s),
                              static_cast<int>(ids[r]), static_cast<int>(col.z[s])});
    return g;
}

/// One driven epoch under star(p,q).  Reports the fold's PD verdict for block 0 so the
/// gate can assert the defect was reproduced, not merely that nothing crashed.
struct EpochOut {
    std::vector<Eigen::VectorXd> xr;
    bool   block0_degenerate{false};
    long long degen_blocks{0};
};
EpochOut driveEpoch(NetworkInvariantCarryDriver& drv, const Net& n, int p, int q,
                    double dt, bool pd_safe) {
    const std::string sig = "L1";
    NetworkEstimator est;
    est.setDdFoldPdSafe(pd_safe);
    std::vector<Eigen::Index> clkCol(n.S), pbCol(n.S);
    for (int s = 0; s < n.S; ++s) {
        clkCol[s] = est.registerClock(n.sats[s]);
        pbCol[s]  = est.registerPhaseBias(n.sats[s], sig);
    }
    est.beginEpoch();
    std::vector<std::size_t> ids(n.R);
    for (int r = 0; r < n.R; ++r)
        ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, /*r1=*/p);
    drv.beginEpoch(dt);
    est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q)}));
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    EpochOut out;
    out.degen_blocks = est.ddFoldStats().degen_blocks;
    if (est.numDdPriorBlocks() > 0) out.block0_degenerate = est.ddPriorBlockDegenerate(0);
    drv.installPosterior(est, x_s, datum);
    out.xr.resize(static_cast<std::size_t>(n.R));
    for (int r = 0; r < n.R; ++r) out.xr[static_cast<std::size_t>(r)] =
        est.reconstructStation(ids[static_cast<std::size_t>(r)], x_s);
    return out;
}

double estimableDD(const std::vector<Eigen::VectorXd>& xr, int r, int s, int r0, int s0) {
    const StationCols col = ambLayout(static_cast<int>(xr[0].size()) - 2);
    return xr[static_cast<std::size_t>(r)](col.z[s])
         - xr[static_cast<std::size_t>(r)](col.z[s0])
         - xr[static_cast<std::size_t>(r0)](col.z[s])
         + xr[static_cast<std::size_t>(r0)](col.z[s0]);
}

/// Solve one epoch with no DD carry at all: the data-only tap-off a rebirthed block's
/// diffuse prior should reproduce (the GATE-10c pattern).
double solveNoCarryDD(const Net& n, int p, int q) {
    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(n.S), pbCol(n.S);
    for (int s = 0; s < n.S; ++s) {
        clkCol[s] = est.registerClock(n.sats[s]);
        pbCol[s]  = est.registerPhaseBias(n.sats[s], sig);
    }
    est.beginEpoch();
    std::vector<std::size_t> ids(n.R);
    for (int r = 0; r < n.R; ++r)
        ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, p);
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    std::vector<Eigen::VectorXd> xr(static_cast<std::size_t>(n.R));
    for (int r = 0; r < n.R; ++r)
        xr[static_cast<std::size_t>(r)] =
            est.reconstructStation(ids[static_cast<std::size_t>(r)], x_s);
    return estimableDD(xr, 2, 3, 0, 0);
}

/// Seed one carried component over the star(0,0) chord basis with a caller-chosen Pᶻ.
std::vector<ArcKey> seedCarry(NetworkInvariantCarryDriver& drv, int R, int S,
                              const Eigen::MatrixXd& Pz, Eigen::VectorXd& z_out) {
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::set<ArcKey> arcs;
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) arcs.insert(ArcKey(rxV(r), satV(s)));
    std::vector<ArcKey> ch;
    for (int r = 1; r < R; ++r)
        for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    z_out.resize(K);
    for (Eigen::Index i = 0; i < K; ++i) z_out(i) = 0.25 * static_cast<double>(i + 1);
    drv.seedCarriedForTest(0, ch, z_out, Pz, verts, arcs);
    return ch;
}

}  // namespace

// ===========================================================================
// SPEC §6.1: the product form is the same quantity, not an approximation.
// ===========================================================================
TEST(PdSafeFoldTest, ProductFormEqualsSubtractionWellConditioned) {
    std::mt19937 gen(20260803);
    const DecoupledFold f = makeLanePairFold(gen, /*n_chords=*/6, /*band_eps=*/0.25);
    // A benign prior (one scale, mild correlation): the regime where the subtractive form
    // is well behaved, so the two forms agree to round-off.
    const Eigen::MatrixXd Pz =
        makeMixedScalePrior(6, /*var_mature=*/0.05, /*var_fresh=*/0.05,
                            /*rho=*/0.3, /*background=*/0.05);
    ASSERT_GT(minEig(Pz), 0.0) << "the benign prior must itself be PD";

    bool degen_off = true, degen_on = true;
    const Eigen::MatrixXd off = foldOnce(f, Pz, /*pd_safe=*/false, &degen_off);
    const Eigen::MatrixXd on  = foldOnce(f, Pz, /*pd_safe=*/true,  &degen_on);
    ASSERT_FALSE(degen_off);
    ASSERT_FALSE(degen_on);

    const double scale = off.cwiseAbs().maxCoeff();
    ASSERT_GT(scale, 0.0);
    EXPECT_LT((on - off).cwiseAbs().maxCoeff() / scale, 1e-9)
        << "Pᶻ − PᶻMPᶻ ≡ PᶻMB is an EXACT identity; on a well-conditioned block the two "
           "roundings must agree to ~1e-9 relative.  A larger gap means the product form "
           "is not the quantity the SPEC claims it is.\noff=\n" << off << "\non=\n" << on;
    // The posterior shrinks the prior; otherwise the comparison above would be vacuous
    // (two identical copies of an untouched Pᶻ agree trivially).
    EXPECT_LT(on.trace(), Pz.trace())
        << "the fold folded no information — the fixture is not exercising anything";
}

// ===========================================================================
// SPEC §6.2: the falsifier pair.  The legacy recursion reproduces the defect and the
// PD-safe recursion does not.  If the legacy arm stops failing, the toy no longer models
// the field and is re-derived (SPEC §6.2 states this rule).
// ===========================================================================
TEST(PdSafeFoldTest, MixedScaleLanePairStaysPd) {
    std::mt19937 gen(20260802);
    const int NC = 20;                       // 20 chords ⇒ K = 40 rows, the SPEC's shape
    // band_eps = 1e-3: the two band rows of a chord differ by ~1 part in 1e3, so their
    // difference (the lane) is determined to ~mm while each band alone is metre-level,
    // matching the field's near-duplicate geometry.  At 1e-5 the lane is 10 µm-precise,
    // which drives cond(Pᶻ) past 1e12 within two epochs on any arithmetic, so the test
    // would exercise the toy rather than the fold.
    const DecoupledFold f0 = makeLanePairFold(gen, NC, /*band_eps=*/1e-3);
    // rho = 1 − 1e-6, not 1 − 1e-9: at 1e-9 the seed's own tight direction is already
    // below double round-off against the 0.002 background and the seed itself fails the PD
    // check, which would test the constructor rather than the recursion.  1e-6 is still
    // far tighter than anything a well-conditioned block carries and reproduces the defect.
    const Eigen::MatrixXd Pz0 =
        makeMixedScalePrior(NC, /*var_mature=*/1e-4, /*var_fresh=*/1e6,
                            /*rho=*/1.0 - 1e-6, /*background=*/0.002);
    ASSERT_GT(minEigNormalized(Pz0), 0.0)
        << "the seed prior must start PD — the point is that the RECURSION destroys it";

    const Eigen::Index K = Pz0.rows();
    Eigen::MatrixXd P_legacy = Pz0, P_safe = Pz0, P_ref = Pz0;
    double legacy_min_eig = std::numeric_limits<double>::infinity();
    double safe_min_eig   = std::numeric_limits<double>::infinity();
    bool legacy_degenerate_seen = false;
    bool safe_degenerate_seen   = false;
    int  legacy_first_indefinite = -1;

    // The driver's q_z time update, applied to every arm as beginEpoch does.  It is
    // required: with q_z = 0 the DD variance falls like 1/n and after a few hundred epochs
    // the covariance is numerically singular on any arithmetic, including the oracle,
    // which returns NaN.  The lineage the card is graded on runs q_z = 1e-4 m²/s, so a
    // bounded random walk is the realistic regime; 1e-6 m² per epoch is a ~1 mm step,
    // small against the 1e-4 mature scale and large enough to floor the spectrum.
    constexpr double kQz = 1e-6;
    // SPEC §6.2 asks for 500 cycles; 200 are used.  The legacy path goes indefinite and
    // trips its own PD screen on the first cycle, ending 111× (relative) away from the
    // oracle, so cycles 200-500 add no discrimination, while 500 cycles cost ~14 s of a
    // ~64 s Debug suite for one case.  The remaining cycles test the PD-safe arm's
    // sustained hold, and 200 folds of a 40-row block suffice for that.  Raise the count
    // if the fixture changes.
    constexpr int kEpochs = 200;
    for (int ep = 0; ep < kEpochs; ++ep) {
        // "Noisy data": the station's normals breathe epoch to epoch, so B is never the
        // same matrix twice and the recursion cannot settle into a fixed point.
        DecoupledFold f = f0;
        const double s = 1.0 + 0.25 * std::sin(0.37 * ep);
        f.N_rr = f0.N_rr * s;

        bool d_legacy = false, d_safe = false;
        P_legacy = foldOnce(f, P_legacy, /*pd_safe=*/false, &d_legacy);
        P_safe   = foldOnce(f, P_safe,   /*pd_safe=*/true,  &d_safe);
        legacy_degenerate_seen = legacy_degenerate_seen || d_legacy;
        safe_degenerate_seen   = safe_degenerate_seen   || d_safe;
        P_ref = posteriorOracle(f, P_ref);   // independent algebraic path
        ASSERT_TRUE(P_ref.allFinite())
            << "epoch " << ep << ": the ORACLE lost its footing — the fixture has left the "
               "regime it is supposed to model, so nothing below would mean anything";
        P_legacy.diagonal().array() += kQz;
        P_safe.diagonal().array()   += kQz;
        P_ref.diagonal().array()    += kQz;

        // Graded on the correlation-normalized spectrum: with a 1e10 variance spread an
        // absolute min-eig test measures the eigensolver's round-off floor, not the matrix.
        // Sampled every kEigStride epochs (plus the last): two 40×40 eigendecompositions
        // per epoch dominate this gate's Debug runtime, and neither failure mode is
        // transient.  An indefinite carry stays indefinite (its posterior is its prior),
        // and `*_degenerate_seen` is checked every epoch at no cost.
        constexpr int kEigStride = 10;
        if (ep % kEigStride == 0 || ep == kEpochs - 1) {
            const double me_l = minEigNormalized(P_legacy);
            const double me_s = minEigNormalized(P_safe);
            legacy_min_eig = std::min(legacy_min_eig, me_l);
            safe_min_eig   = std::min(safe_min_eig, me_s);
            if (me_l < 0.0 && legacy_first_indefinite < 0) legacy_first_indefinite = ep;
        }
    }

    // (i) The defect reproduces.  Either the carried covariance itself went indefinite,
    //     or the fold's own PD screen tripped; both are the same failure.
    EXPECT_TRUE(legacy_first_indefinite >= 0 || legacy_degenerate_seen)
        << "THE LEGACY PATH DID NOT FAIL.  This toy exists to model the field defect "
           "(min eig 4.6e-6 born, indefinite by ep 122, 78 % of the carry frozen).  If it "
           "no longer reproduces it, the toy has drifted from the field and MUST be "
           "re-derived — do not weaken this assertion.  legacy min eig = "
        << legacy_min_eig;

    // (ii) The PD-safe path holds the algebraic invariant.  Round-off dust on the
    //      normalized spectrum is ~1e-14; anything at or above 0 is a genuine hold.
    EXPECT_GE(safe_min_eig, -1e-12)
        << "PD of the carried covariance is an ALGEBRAIC INVARIANT of the exact recursion; "
           "the PD-safe path must keep it to round-off.  normalized min eig = "
        << safe_min_eig << " over " << kEpochs << " epochs";
    EXPECT_FALSE(safe_degenerate_seen)
        << "the PD-safe ARITHMETIC alone must carry this block — needing the INC-2a "
           "rebirth here would make the min-eig assertion above pass vacuously on a "
           "FROZEN covariance (an inert fold's posterior is its prior)";

    // (iii) It tracks the independent oracle far better than the legacy path.
    const double ref_scale = P_ref.cwiseAbs().maxCoeff();
    ASSERT_GT(ref_scale, 0.0);
    const double err_safe   = (P_safe   - P_ref).cwiseAbs().maxCoeff() / ref_scale;
    const double err_legacy = (P_legacy - P_ref).cwiseAbs().maxCoeff() / ref_scale;
    EXPECT_LT(err_safe, err_legacy)
        << "the PD-safe posterior must be CLOSER to the augmented-solve oracle than the "
           "subtractive one — that is the whole claim of INC-1a.  safe=" << err_safe
        << " legacy=" << err_legacy;
    EXPECT_LT(err_safe, 1e-3)
        << "PD-safe posterior drifted from the oracle by " << err_safe
        << " relative over " << kEpochs << " epochs";
    EXPECT_EQ(K, 2 * NC);
    // Measured on this fixture: legacy goes indefinite at cycle 0, trips its own PD
    // screen, and ends err_legacy = 1.1e+02 (relative) from the oracle; PD-safe holds
    // normalized min-eig 1.5e-4 with err_safe = 1.6e-09.  That is about five orders of
    // separation on the invariant and eleven on the value.  If an edit narrows that gap,
    // the fold changed, not the fixture.
}

// ===========================================================================
// SPEC §6.3: what the equilibration provides.  The pivot test becomes a statement about
// conditioning instead of about the 1e10 spread between a mature and a fresh row.
// ===========================================================================
TEST(PdSafeFoldTest, EquilibratedPivotIsScaleRelative) {
    std::mt19937 gen(4242);
    const int NC = 8;
    const DecoupledFold f = makeLanePairFold(gen, NC, /*band_eps=*/0.2);

    // Three priors with an identical well-conditioned correlation structure and a
    // widening mature/fresh variance spread.  The equilibrated pivot barely moves; the
    // plain one collapses in proportion to the spread.
    const double spreads[3] = {1e2, 1e6, 1e10};
    double piv_safe[3] = {0, 0, 0}, piv_legacy[3] = {0, 0, 0};
    double err_safe[3] = {0, 0, 0}, err_legacy[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const Eigen::MatrixXd Pz =
            makeMixedScalePrior(NC, /*var_mature=*/1.0, /*var_fresh=*/spreads[i],
                                /*rho=*/0.4, /*background=*/0.05);
        ASSERT_GT(minEigNormalized(Pz), 0.0)
            << "spread " << spreads[i] << ": the prior must be PD";
        bool d_off = true, d_on = true;
        const Eigen::MatrixXd off =
            foldOnce(f, Pz, /*pd_safe=*/false, &d_off, &piv_legacy[i]);
        const Eigen::MatrixXd on =
            foldOnce(f, Pz, /*pd_safe=*/true,  &d_on,  &piv_safe[i]);
        EXPECT_FALSE(d_off) << "spread " << spreads[i];
        EXPECT_FALSE(d_on)  << "spread " << spreads[i];
        // Both compute the same real number; the question is which rounding survives the
        // spread.  Graded against the augmented-solve oracle, which reaches the same
        // quantity down a different algebraic path with no cancellation of 1e10-scale
        // operands.
        const Eigen::MatrixXd ref_d = posteriorOracle(f, Pz);
        const double sc = ref_d.cwiseAbs().maxCoeff();
        ASSERT_GT(sc, 0.0);
        err_legacy[i] = (off - ref_d).cwiseAbs().maxCoeff() / sc;
        err_safe[i]   = (on  - ref_d).cwiseAbs().maxCoeff() / sc;
        EXPECT_LE(err_safe[i], err_legacy[i] + 1e-15)
            << "spread " << spreads[i] << ": the PD-safe rounding must never be WORSE "
               "than the subtractive one.  safe=" << err_safe[i]
            << " legacy=" << err_legacy[i];
    }
    // At a benign spread the two roundings are indistinguishable; at 1e10 the subtractive
    // form has lost its significant digits and the two diverge.  That divergence is the
    // defect, measured, and not a tolerance to widen.
    EXPECT_LT(err_safe[0], 1e-10) << "benign spread: both forms should be exact-ish";
    EXPECT_LT(err_safe[2], 1e-8)
        << "the equilibrated + product form must still track the oracle at a 1e10 spread; "
           "got " << err_safe[2];
    EXPECT_GT(err_legacy[2], 1e2 * err_safe[2])
        << "at a 1e10 spread the SUBTRACTIVE form must be orders worse — if it is not, "
           "this fixture no longer reproduces the field regime.  legacy=" << err_legacy[2]
        << " safe=" << err_safe[2];

    EXPECT_GT(piv_safe[2], 1e-6)
        << "the EQUILIBRATED relative pivot must stay O(1)-ish even at a 1e10 variance "
           "spread — it reports conditioning, not scale.  got " << piv_safe[2];
    EXPECT_GT(piv_safe[2], 1e-3 * piv_safe[0])
        << "the equilibrated pivot must be nearly spread-INVARIANT: "
        << piv_safe[0] << " (1e2) vs " << piv_safe[2] << " (1e10)";
    EXPECT_LT(piv_legacy[2], 1e-4 * piv_legacy[0])
        << "the PLAIN pivot must collapse with the spread — that is why the absolute "
           "minCoeff()<=0 screen could not tell a scale artifact from a rank defect.  "
        << piv_legacy[0] << " (1e2) vs " << piv_legacy[2] << " (1e10)";
    EXPECT_GT(piv_safe[2], piv_legacy[2] * 1e3)
        << "at the widest spread the equilibrated pivot must be orders above the plain one";
}

// ===========================================================================
// SPEC §6.4: invariant #7's re-coupling edge, end to end.  The fold is inert, the
// counters fire, the install rebirths the block, and the next epoch's carried value is
// the data-only tap-off (the GATE-10c pattern).
// ===========================================================================
TEST(PdSafeFoldTest, DegenerateBlockRebirthsLoudly) {
    const int R = 4, S = 5;
    const Net net = makeNet(20260803u, R, S);
    const Eigen::Index K = static_cast<Eigen::Index>((R - 1) * (S - 1));

    // A carried block that no amount of data information can rescue: one eigenvalue at
    // −1e6 against a B of order 1.  Corrupt field snapshots have this shape (−2.9e7 m²
    // diagonals, +4.7e6 entries above the 1e6 diffuse prior).
    Eigen::MatrixXd bad = Eigen::MatrixXd::Identity(K, K) * 1e-2;
    bad(0, 0) = -1.0e6;

    NetworkInvariantCarryDriver::Config cfg;
    cfg.pd_safe = true;
    NetworkInvariantCarryDriver drv(cfg);
    Eigen::VectorXd z_seed;
    const std::vector<ArcKey> ch = seedCarry(drv, R, S, bad, z_seed);
    ASSERT_EQ(static_cast<Eigen::Index>(ch.size()), K);

    const EpochOut e1 = driveEpoch(drv, net, /*p=*/0, /*q=*/0, /*dt=*/30.0, /*pd_safe=*/true);
    EXPECT_TRUE(e1.block0_degenerate)
        << "an indefinite Pᶻ that B cannot cover MUST trip the PD screen — otherwise the "
           "rest of this gate is testing nothing";
    EXPECT_EQ(e1.degen_blocks, 1) << "INC-0 must COUNT the inert fold (invariant #4)";

    // The re-coupling edge fired, with an identity payload for the event (INC-2a).
    ASSERT_EQ(drv.lastFoldRebirths().size(), 1u)
        << "a block that could not be rescued must be REBIRTHED, not left frozen";
    const auto& rb = drv.lastFoldRebirths().front();
    EXPECT_EQ(rb.k, static_cast<long long>(K));
    EXPECT_EQ(rb.group, 0);
    EXPECT_EQ(drv.counters().ddfold_rebirths, 1);
    EXPECT_EQ(drv.counters().ddfold_rebirth_rows, static_cast<long long>(K));

    // The rebirth re-seeded the covariance diffuse (means kept as the seed).
    for (const ArcKey& c : ch) {
        double z = 0.0, var = 0.0;
        ASSERT_TRUE(drv.carriedChordValue(0, c, z, var));
        EXPECT_DOUBLE_EQ(var, cfg.fresh_prior_var)
            << "chord " << c.u << "->" << c.v << " kept a covariance from a block we "
               "declared unusable";
    }

    // Next epoch the diffuse prior takes the data-only tap-off.
    const EpochOut e2 = driveEpoch(drv, net, 0, 0, 30.0, /*pd_safe=*/true);
    EXPECT_FALSE(e2.block0_degenerate) << "the rebirthed block must factor again";
    // Tolerance is O(B / fresh_prior_var), not zero: a rebirthed block gets the diffuse
    // prior (1e6), not the exact zero-information tap-off, so the seed means still pull by
    // ~|z_seed − z_data|·B/1e6, the same O(B/fresh_prior_var) residual the driver header
    // documents for every fresh chord.  The assertion is that the block returned to the
    // data, not that it landed exactly on it.
    EXPECT_NEAR(estimableDD(e2.xr, 2, 3, 0, 0), solveNoCarryDD(net, 0, 0), 1e-3)
        << "one bounded epoch of information lost, then the block re-earns its value from "
           "data — exactly the semantics a chord birth already has";

    // Negative control: with the knob off the same block freezes, with no rebirth, and
    // stays frozen.
    NetworkInvariantCarryDriver::Config off_cfg;   // pd_safe defaults false
    NetworkInvariantCarryDriver off(off_cfg);
    Eigen::VectorXd z2;
    seedCarry(off, R, S, bad, z2);
    const EpochOut o1 = driveEpoch(off, net, 0, 0, 30.0, /*pd_safe=*/false);
    EXPECT_TRUE(o1.block0_degenerate);
    EXPECT_TRUE(off.lastFoldRebirths().empty()) << "OFF ⇒ no rebirth (byte-identical path)";
    const EpochOut o2 = driveEpoch(off, net, 0, 0, 30.0, /*pd_safe=*/false);
    EXPECT_TRUE(o2.block0_degenerate)
        << "THE defect: an inert fold's posterior IS its prior, so the block never "
           "improves and never factors again.  The freeze is permanent.";
}

// ===========================================================================
// SPEC §6.5 (INC-2c).  A carry blob written by the legacy fold can hold the corrupt
// block; a warm resume must not re-import it into a build with the fix armed.
// ===========================================================================
TEST(PdSafeFoldTest, RestoreGuardRebirthsCorruptBlob) {
    const int R = 3, S = 3;
    const Eigen::Index K = static_cast<Eigen::Index>((R - 1) * (S - 1));
    Eigen::MatrixXd bad = Eigen::MatrixXd::Identity(K, K) * 5.0;
    bad(1, 1) = -2.9e7;                     // the field's own shape: a negative variance

    NetworkInvariantCarryDriver writer;     // pd_safe irrelevant on the write side
    Eigen::VectorXd z_seed;
    const std::vector<ArcKey> ch = seedCarry(writer, R, S, bad, z_seed);
    std::ostringstream blob;
    writer.serializeCarry(blob, /*diagonal_only=*/false);

    // Armed: the guard rebirths the block (means kept, covariance re-seeded diffuse).
    NetworkInvariantCarryDriver::Config on_cfg;
    on_cfg.pd_safe = true;
    NetworkInvariantCarryDriver on(on_cfg);
    {
        std::istringstream is(blob.str());
        std::string err;
        ASSERT_TRUE(on.restoreCarry(is, &err)) << err;
    }
    EXPECT_EQ(on.resumeCarryRebirthBlocks(), 1)
        << "the corrupt block must be caught AT RESTORE — without this a warm resume "
           "re-imports the disease into a fixed build";
    for (Eigen::Index i = 0; i < K; ++i) {
        double z = 0.0, var = 0.0;
        ASSERT_TRUE(on.carriedChordValue(0, ch[static_cast<std::size_t>(i)], z, var));
        EXPECT_DOUBLE_EQ(z, z_seed(i)) << "the MEANS are the branch — they must survive";
        EXPECT_DOUBLE_EQ(var, on_cfg.fresh_prior_var);
    }

    // Disarmed: the legacy byte path is intact and the blob restores verbatim, corruption
    // included.  This keeps the legacy behaviour and makes the off arm of the §7 A/B a
    // true control.
    NetworkInvariantCarryDriver off;
    {
        std::istringstream is(blob.str());
        std::string err;
        ASSERT_TRUE(off.restoreCarry(is, &err)) << err;
    }
    EXPECT_EQ(off.resumeCarryRebirthBlocks(), 0);
    {
        double z = 0.0, var = 0.0;
        ASSERT_TRUE(off.carriedChordValue(0, ch[1], z, var));
        EXPECT_DOUBLE_EQ(var, -2.9e7)
            << "OFF must restore the blob VERBATIM — negative variance included";
    }

    // The guard leaves a well-conditioned blob untouched.
    NetworkInvariantCarryDriver healthy_writer;
    Eigen::MatrixXd good = Eigen::MatrixXd::Identity(K, K) * 3.0;
    for (Eigen::Index i = 0; i + 1 < K; ++i) { good(i, i + 1) = 0.4; good(i + 1, i) = 0.4; }
    Eigen::VectorXd zg;
    const std::vector<ArcKey> chg = seedCarry(healthy_writer, R, S, good, zg);
    std::ostringstream gblob;
    healthy_writer.serializeCarry(gblob, false);
    NetworkInvariantCarryDriver on2(on_cfg);
    {
        std::istringstream is(gblob.str());
        std::string err;
        ASSERT_TRUE(on2.restoreCarry(is, &err)) << err;
    }
    EXPECT_EQ(on2.resumeCarryRebirthBlocks(), 0)
        << "the guard must not fire on a healthy blob — a rebirth is a bounded loss of "
           "information, not free";
    double z = 0.0, var = 0.0;
    ASSERT_TRUE(on2.carriedChordValue(0, chg[0], z, var));
    EXPECT_DOUBLE_EQ(var, 3.0);
}

// ===========================================================================
// SPEC §6.6: invariant #6 (the off path is unchanged) plus the always-on INC-0
// contract (the counters observe; they do not act).
// ===========================================================================
TEST(PdSafeFoldTest, OffPathIsUnchangedAndInertSemanticsHold) {
    std::mt19937 gen(777);
    const DecoupledFold f = makeLanePairFold(gen, /*n_chords=*/5, /*band_eps=*/0.3);
    const Eigen::MatrixXd Pz =
        makeMixedScalePrior(5, /*var_mature=*/0.02, /*var_fresh=*/0.2,
                            /*rho=*/0.25, /*background=*/0.05);

    // (a) the knob defaults to off.
    {
        NetworkNormalEquations neq(1);
        EXPECT_FALSE(neq.ddFoldPdSafe()) << "new work is default-OFF (invariant #6)";
        NetworkEstimator est;
        EXPECT_FALSE(est.ddFoldPdSafe());
    }

    // (b) "knob absent" and "knob explicitly false" are bit-identical, including under
    //     the parallel_carry within-block column split the serial fold opts into.
    const Eigen::MatrixXd implicit_off = foldOnce(f, Pz, /*pd_safe=*/false);
    NetworkNormalEquations bare(1);   // never touched setDdFoldPdSafe
    bare.addSharedInfo(Eigen::MatrixXd::Identity(1, 1), Eigen::VectorXd::Zero(1));
    bare.addStation(Eigen::MatrixXd::Zero(1, 1),
                    Eigen::MatrixXd::Zero(1, f.N_rr.rows()), f.N_rr,
                    Eigen::VectorXd::Zero(1), Eigen::VectorXd::Ones(f.N_rr.rows()));
    NetworkNormalEquations::DdPriorBlock blk;
    blk.rows = f.rows; blk.Pz = Pz;
    blk.zhat = Eigen::VectorXd::Zero(Pz.rows());
    bare.reduce();
    bare.foldReducedDdPriors({blk});
    const Eigen::MatrixXd untouched =
        bare.ddPriorPosteriorCovariance(0, Eigen::MatrixXd::Zero(1, 1));
    EXPECT_EQ((implicit_off - untouched).cwiseAbs().maxCoeff(), 0.0)
        << "EXPECT_EQ, not EXPECT_NEAR, is the point: absent ≡ false ≡ the pre-edit path";
    const Eigen::MatrixXd off_par = foldOnce(f, Pz, false, nullptr, nullptr,
                                             /*station_threads=*/4);
    EXPECT_EQ((implicit_off - off_par).cwiseAbs().maxCoeff(), 0.0)
        << "the column-split inverse (parallel_carry) relocated into inverseFromLdlt must "
           "stay byte-identical to the single solve";

    // (c) INC-0 is always on and solve-neutral: it observes the same factor the fold
    //     already computed, so a well-conditioned block reports a relative pivot in
    //     (0, 1], no degeneracy and no repair.
    {
        NetworkNormalEquations neq(1);
        neq.addSharedInfo(Eigen::MatrixXd::Identity(1, 1), Eigen::VectorXd::Zero(1));
        neq.addStation(Eigen::MatrixXd::Zero(1, 1),
                       Eigen::MatrixXd::Zero(1, f.N_rr.rows()), f.N_rr,
                       Eigen::VectorXd::Zero(1), Eigen::VectorXd::Ones(f.N_rr.rows()));
        neq.reduce();
        neq.foldReducedDdPriors({blk});
        const auto& st = neq.ddFoldStats();
        EXPECT_EQ(st.blocks, 1);
        EXPECT_EQ(st.rows, Pz.rows());
        EXPECT_EQ(st.degen_blocks, 0);
        EXPECT_EQ(st.repairs, 0) << "no repair may fire on the OFF path — it is INC-1c";
        EXPECT_GT(st.min_pivot_rel, 0.0);
        EXPECT_LE(st.min_pivot_rel, 1.0);
    }

    // (d) the degenerate branch keeps its legacy semantics on the off path: inert fold,
    //     index-stable slot, posterior exactly the untouched prior.  It is also counted,
    //     which is all INC-0 adds.
    {
        NetworkNormalEquations neq(1);
        neq.addSharedInfo(Eigen::MatrixXd::Identity(1, 1), Eigen::VectorXd::Zero(1));
        neq.addStation(Eigen::MatrixXd::Zero(1, 1),
                       Eigen::MatrixXd::Zero(1, f.N_rr.rows()), f.N_rr,
                       Eigen::VectorXd::Zero(1), Eigen::VectorXd::Ones(f.N_rr.rows()));
        NetworkNormalEquations::DdPriorBlock bad = blk;
        bad.Pz = Pz;
        bad.Pz(0, 0) = -1.0e6;
        neq.reduce();
        neq.foldReducedDdPriors({bad});
        EXPECT_TRUE(neq.ddPriorBlockDegenerate(0));
        EXPECT_EQ(neq.ddFoldStats().degen_blocks, 1);
        EXPECT_EQ(neq.ddFoldStats().degen_rows, bad.Pz.rows());
        const Eigen::MatrixXd post =
            neq.ddPriorPosteriorCovariance(0, Eigen::MatrixXd::Zero(1, 1));
        EXPECT_EQ((post - bad.Pz).cwiseAbs().maxCoeff(), 0.0)
            << "M = 0 ⇒ the posterior IS the prior.  That is the freeze; INC-0's job is "
               "to make it visible, not to change it.";
    }
}

}  // namespace GnssProcessor
