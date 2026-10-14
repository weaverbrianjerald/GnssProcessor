/**
 * @file test_network_invariant_carry_driver.cpp
 * @brief Tests for the live DD invariant-carry driver (card 4 (M1), phase C3b and later).
 *
 * The five phase-C3b gates, each with a negative or independent control so that a
 * pass is not vacuous:
 *
 *  1. **ProjectionMatchesFromScratchAndZStore**: the carry reconcile is a change of
 *     basis.  A known DD estimate is seeded in one fundamental-cycle basis and
 *     reconciled to a different tree; the projected prior equals the from-scratch
 *     cycle-matrix result @f$C_2 N,\ C_2\,\mathrm{Cov}(N)\,C_2^\top@f$ (the phase-B
 *     CycleBasisUnimodular oracle) to 1e-9.  A negative control shows the projection
 *     is not the identity, so the transform is load-bearing.
 *  2. **CarryAccumulatesAndConverges**: over epochs the carried DD variance shrinks
 *     monotonically and the mean converges to the true integer DD.
 *  3. **ProductionSeamDatumFreedom**: the same noisy epoch stream, driven under two
 *     different per-epoch tree realizations, yields an identical estimable double
 *     difference (the basis choice does not leak), while the realizations' chord sets
 *     differ and the carry is load-bearing.
 *  4. **MergeSplitAndFreshChords**: component merges and fresh-chord births are
 *     counted, and the surviving DDs are preserved across the topology change (the
 *     test drives a merge only).
 *  5. **ResetPredictAndOff**: reset() cold-starts, qz inflates the carry, and an
 *     empty epoch emits no blocks (the off path).
 *
 * The later tests in this file cover the snapshot blob, the warm-resume event, the
 * slip and innovation-gate responses and the counters.
 *
 * The synthetic network is the eq34 chain-datum model (a per-receiver RPB δ_r makes
 * the DD integer): code P = a_r − c_s, phase Φ = a_r − c_s + b_s + δ_r + z_{r,s}, with a
 * spanning-tree ambiguity datum (pre-reduce pin rows, which compose exactly with the
 * post-reduce DD fold), the RPB pin of r1 and a shared clock pin.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkZStore.hpp"
#include "filter/NetworkEstimator.hpp"

namespace GnssProcessor {
namespace {

constexpr int kSatBase = 1000;   // satellite vertex ids live above every receiver id
int rxV(int r) { return r; }
int satV(int s) { return kSatBase + s; }

// Spanning tree of the full bipartite R×S graph, "star" pivoted on receiver p and
// satellite q: edges {(p,s): all s} ∪ {(r,q): r≠p}  (R+S−1 edges).
std::vector<ArcKey> starTree(int p, int q, int R, int S) {
    std::vector<ArcKey> e;
    for (int s = 0; s < S; ++s) e.emplace_back(rxV(p), satV(s));
    for (int r = 0; r < R; ++r) if (r != p) e.emplace_back(rxV(r), satV(q));
    return e;
}
bool isStarTreeEdge(int p, int q, int r, int s) {
    return (r == p) || (s == q);
}

// ---- synthetic eq34 network (integer ambiguities z, non-zero RPB δ) -----------
struct Net {
    int R, S;
    std::vector<double> a, c, b, d;               // rx clock, sat clock, sat bias, rx RPB
    std::vector<std::vector<int>> z;              // integer ambiguities [R][S]
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

// Assemble one station's rows for the estimator: 2S observation rows (with optional
// per-(r,s) phase noise), the spanning-tree ambiguity pins for this station, and, for
// r1, the RPB pin δ_{r1} = 0.  No δ_r prior is added (see below).
struct StationCols { Eigen::Index a{0}, d{1}; std::vector<Eigen::Index> z; };
StationCols ambLayout(int S) { StationCols c; c.z.resize(S); for (int s = 0; s < S; ++s) c.z[s] = 2 + s; return c; }

std::size_t addNetStation(NetworkEstimator& est, const Net& n, int r,
                          const std::vector<Eigen::Index>& clkCol,
                          const std::vector<Eigen::Index>& pbCol, int p, int q,
                          const std::vector<double>* noise /*len S, phase*/, int r1) {
    const int S = n.S;
    const StationCols col = ambLayout(S);
    std::vector<int> pins;                      // tree-edge ambiguity cols pinned to 0
    for (int s = 0; s < S; ++s) if (isStarTreeEdge(p, q, r, s)) pins.push_back(col.z[s]);
    const bool rpbPin = (r == r1);
    // Each station has at least one tree-edge pin, which fixes its local δ_r ≡ Σ_s z_{r,s}
    // gauge (N_rr stays PD), so no loose δ prior is needed.  A loose δ prior would add
    // toward-zero information that, combined with the datum-dependent set of pinned
    // ambiguities, breaks the δ–z cancellation differently per tree and removes the
    // DD's datum invariance (checked by the DiagnosticNetworkDatumInvariance probe).
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
        y(row) = n.a[r] - n.c[s] + n.b[s] + n.d[r] + n.z[r][s] + (noise ? (*noise)[s] : 0.0);
        ++row;
    }
    for (int pc : pins) { A_r(row, pc) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    if (rpbPin) { A_r(row, col.d) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    return est.addStation(A_s, A_r, w, y);
}

// Build the driver's one (const,signal) group for a full-vis epoch under star(p,q).
// @p slipped (optional): the (r,s) arcs to tag slipped this epoch (Card 4 C3c feed).
NetworkInvariantCarryDriver::GroupInput
makeGroup(const Net& n, const std::vector<std::size_t>& ids, int p, int q,
          const std::set<std::pair<int, int>>* slipped = nullptr) {
    NetworkInvariantCarryDriver::GroupInput g; g.key = 0;
    const StationCols col = ambLayout(n.S);
    for (int r = 0; r < n.R; ++r)
        for (int s = 0; s < n.S; ++s) {
            const bool sl = slipped && slipped->count({r, s}) != 0;
            g.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(p, q, r, s),
                              static_cast<int>(ids[r]), static_cast<int>(col.z[s]), sl});
        }
    return g;
}

// One full driven epoch under star(p,q); returns the reconstructed ambiguities.
// @p slipped (optional): the (r,s) arcs signaled as cycle slips this epoch.
std::vector<Eigen::VectorXd> driveEpoch(NetworkInvariantCarryDriver& drv, const Net& n,
                                        int p, int q, const std::vector<std::vector<double>>* noise,
                                        double dt,
                                        const std::set<std::pair<int, int>>* slipped = nullptr) {
    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(n.S), pbCol(n.S);
    for (int s = 0; s < n.S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(n.R);
    for (int r = 0; r < n.R; ++r)
        ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q,
                               noise ? &(*noise)[r] : nullptr, /*r1=*/p);
    drv.beginEpoch(dt);
    est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q, slipped)}));
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    drv.installPosterior(est, x_s, datum);
    std::vector<Eigen::VectorXd> xr(n.R);
    for (int r = 0; r < n.R; ++r) xr[r] = est.reconstructStation(ids[r], x_s);
    return xr;
}

// The (r,s) arcs the innovation gate flagged at the last install.  The caller
// (ApplicationNetwork) feeds these into the next epoch's slipped set (the C3c re-tap).
// Mirrors the production plumbing: chord ArcKey(rxV(r), satV(s)) → (r, s).  (card 4 D5)
std::set<std::pair<int, int>> innovSlips(const NetworkInvariantCarryDriver& drv) {
    std::set<std::pair<int, int>> out;
    for (const auto& [g, key] : drv.lastInnovFlags()) out.insert({key.u, key.v - kSatBase});
    return out;
}

// A fixed estimable double difference from reconstructed ambiguities (gauge-free).
double estimableDD(const std::vector<Eigen::VectorXd>& xr, int r, int s, int r0, int s0) {
    const StationCols col = ambLayout(static_cast<int>(xr[0].size()) - 2);
    return xr[r](col.z[s]) - xr[r](col.z[s0]) - xr[r0](col.z[s]) + xr[r0](col.z[s0]);
}

// Solve one epoch under star(p,q) with no DD carry (empty priors).  Datum-invariance
// probe: the estimable DD does not depend on the tree-pin choice.
double solveNoCarryDD(const Net& n, int p, int q, const std::vector<std::vector<double>>* noise) {
    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(n.S), pbCol(n.S);
    for (int s = 0; s < n.S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(n.R);
    for (int r = 0; r < n.R; ++r)
        ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, noise ? &(*noise)[r] : nullptr, p);
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    std::vector<Eigen::VectorXd> xr(n.R);
    for (int r = 0; r < n.R; ++r) xr[r] = est.reconstructStation(ids[r], x_s);
    return estimableDD(xr, 2, 3, 0, 0);
}

TEST(NetworkInvariantCarryDriverTest, DiagnosticNetworkDatumInvariance) {
    const Net n = makeNet(2027, 4, 4);
    std::mt19937 gen(555);
    std::normal_distribution<double> noiseDist(0.0, 0.03);
    std::vector<std::vector<double>> noise(4, std::vector<double>(4));
    for (int r = 0; r < 4; ++r) for (int s = 0; s < 4; ++s) noise[r][s] = noiseDist(gen);
    const double d00 = solveNoCarryDD(n, 0, 0, &noise);
    const double d10 = solveNoCarryDD(n, 1, 0, &noise);
    const double d01 = solveNoCarryDD(n, 0, 1, &noise);
    EXPECT_NEAR(d00, d10, 1e-7) << "estimable DD must be datum-invariant (star00 vs star10)";
    EXPECT_NEAR(d00, d01, 1e-7) << "estimable DD must be datum-invariant (star00 vs star01)";
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1: the reconcile is a change of basis that matches the from-scratch result.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, ProjectionMatchesFromScratchAndZStore) {
    const int R = 3, S = 3;
    // Fixed arc order over the full bipartite graph.
    std::vector<ArcKey> arc_order;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) arc_order.emplace_back(rxV(r), satV(s));
    std::set<int> verts; for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::set<ArcKey> arcs(arc_order.begin(), arc_order.end());

    // Ground-truth ambiguities N (a value per arc) and their covariance CovN.
    std::mt19937 gen(4);
    std::uniform_real_distribution<double> u(-3, 3);
    const Eigen::Index A = static_cast<Eigen::Index>(arc_order.size());
    Eigen::VectorXd N(A); for (Eigen::Index i = 0; i < A; ++i) N(i) = u(gen);
    Eigen::MatrixXd Mrand(A, A); for (Eigen::Index i = 0; i < A; ++i) for (Eigen::Index j = 0; j < A; ++j) Mrand(i, j) = u(gen);
    const Eigen::MatrixXd CovN = Mrand * Mrand.transpose() + A * Eigen::MatrixXd::Identity(A, A);

    // Basis 1: star(0,0).  Chords Ch1 = {(r,s): r>=1, s>=1}.
    const std::vector<ArcKey> t1 = starTree(0, 0, R, S);
    std::vector<ArcKey> ch1; for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch1.emplace_back(rxV(r), satV(s));
    ZComponent zc1(t1);
    zc1.setEstimate(ch1, Eigen::VectorXd::Zero(static_cast<Eigen::Index>(ch1.size())),
                    Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(ch1.size()), static_cast<Eigen::Index>(ch1.size())));
    const Eigen::MatrixXd C1 = zc1.cycleMatrix(arc_order);
    const Eigen::VectorXd z1 = C1 * N;
    const Eigen::MatrixXd Pz1 = C1 * CovN * C1.transpose();

    NetworkInvariantCarryDriver drv;
    drv.seedCarriedForTest(0, ch1, z1, Pz1, verts, arcs);

    // Reconcile to basis 2: star(1,1).  All arcs still present ⇒ every chord carried.
    NetworkInvariantCarryDriver::GroupInput g; g.key = 0;
    const StationCols col = ambLayout(S);
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s)
        g.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(1, 1, r, s), r, static_cast<int>(col.z[s])});
    const auto blocks = drv.buildDdPriors({g});
    ASSERT_EQ(blocks.size(), 1u);
    const std::vector<ArcKey>& ch2 = drv.blockChordsForTest(0);

    // From-scratch reference: C2 in basis 2 over the same chord order.
    ZComponent zc2(starTree(1, 1, R, S));
    zc2.setEstimate(ch2, Eigen::VectorXd::Zero(static_cast<Eigen::Index>(ch2.size())),
                    Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(ch2.size()), static_cast<Eigen::Index>(ch2.size())));
    const Eigen::MatrixXd C2 = zc2.cycleMatrix(arc_order);
    const Eigen::VectorXd z2_ref = C2 * N;
    const Eigen::MatrixXd Pz2_ref = C2 * CovN * C2.transpose();

    EXPECT_TRUE(blocks[0].zhat.isApprox(z2_ref, 1e-9))
        << "projected mean\n" << blocks[0].zhat.transpose() << "\nvs from-scratch\n" << z2_ref.transpose();
    EXPECT_TRUE(blocks[0].Pz.isApprox(Pz2_ref, 1e-9)) << "projected covariance mismatch";
    // Negative control: the transform is genuinely non-trivial (basis 2 ≠ basis 1).
    EXPECT_GT((blocks[0].Pz - Pz1).cwiseAbs().maxCoeff(), 1e-3)
        << "the two tree bases must differ — else the projection is a vacuous identity";
    EXPECT_EQ(drv.counters().chords_carried, static_cast<long long>(ch2.size()));
    EXPECT_EQ(drv.counters().chords_born, 0);
}

// ---------------------------------------------------------------------------
// The branch-faithful snapshot blob: serializeCarry ⇄ restoreCarry round-trips every
// carried DD (mean, variance, cross-covariance, structure) bit for bit across several
// components and groups.  (RTB-7(c))
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, SerializeRestoreRoundTripIsExact) {
    const int R = 3, S = 3;
    std::vector<ArcKey> arc_order;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) arc_order.emplace_back(rxV(r), satV(s));
    std::set<int> verts; for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::set<ArcKey> arcs(arc_order.begin(), arc_order.end());
    std::vector<ArcKey> ch; for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    std::mt19937 gen(7);
    std::uniform_real_distribution<double> u(-2, 2);
    Eigen::VectorXd z(K); for (Eigen::Index i = 0; i < K; ++i) z(i) = u(gen);
    Eigen::MatrixXd M(K, K); for (Eigen::Index i = 0; i < K; ++i) for (Eigen::Index j = 0; j < K; ++j) M(i, j) = u(gen);
    const Eigen::MatrixXd Pz = M * M.transpose() + K * Eigen::MatrixXd::Identity(K, K);

    NetworkInvariantCarryDriver src;
    src.seedCarriedForTest(0, ch, z, Pz, verts, arcs);
    src.seedCarriedForTest(7, ch, (2.0 * z).eval(), (0.5 * Pz).eval(), verts, arcs);  // second group

    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_NE(blob.find("CARRY,v2"), std::string::npos);   // blob version 2 (RE-1)
    EXPECT_NE(blob.find("\nCEND\n"), std::string::npos);

    NetworkInvariantCarryDriver dst;
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << "restore failed: " << err;
    EXPECT_EQ(dst.totalZDim(), src.totalZDim());

    for (int grp : {0, 7}) {
        for (const ArcKey& c : ch) {
            double zs = 0, vs = 0, zd = 0, vd = 0;
            ASSERT_TRUE(src.carriedDdForTest(grp, c, zs, vs));
            ASSERT_TRUE(dst.carriedDdForTest(grp, c, zd, vd)) << "chord not restored, group " << grp;
            EXPECT_DOUBLE_EQ(zs, zd) << "mean mismatch, group " << grp;
            EXPECT_DOUBLE_EQ(vs, vd) << "variance mismatch, group " << grp;
        }
        double cs = 0, cd = 0;
        ASSERT_TRUE(src.carriedCrossForTest(grp, ch[0], ch[1], cs));
        ASSERT_TRUE(dst.carriedCrossForTest(grp, ch[0], ch[1], cd));
        EXPECT_DOUBLE_EQ(cs, cd) << "cross-covariance mismatch, group " << grp;
    }

    // Empty-carry round-trip: a driver with no carry serializes a valid header/terminator
    // that restores to an empty carry (the cold-start-equivalent blob).
    NetworkInvariantCarryDriver empty_src, empty_dst;
    std::ostringstream eos; empty_src.serializeCarry(eos);
    std::istringstream eis(eos.str());
    ASSERT_TRUE(empty_dst.restoreCarry(eis, &err)) << err;
    EXPECT_EQ(empty_dst.totalZDim(), 0);
}

// ---------------------------------------------------------------------------
// Resume event: v2/v1 blob, reset before restore and binding validation.
// (RTB-7(d) G1)
// ---------------------------------------------------------------------------
namespace {
/// Minimal seeded driver: one component, K chords, known z/Pz.  Returns the chords.
std::vector<ArcKey> seedSimple(NetworkInvariantCarryDriver& d, int group,
                               Eigen::VectorXd& z, Eigen::MatrixXd& Pz) {
    const int R = 3, S = 3;
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::vector<ArcKey> all;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) all.emplace_back(rxV(r), satV(s));
    std::set<ArcKey> arcs(all.begin(), all.end());
    std::vector<ArcKey> ch;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    z.resize(K);
    for (Eigen::Index i = 0; i < K; ++i) z(i) = 0.5 * static_cast<double>(i + 1);
    Pz = Eigen::MatrixXd::Identity(K, K) * 2.0;
    for (Eigen::Index i = 0; i + 1 < K; ++i) { Pz(i, i + 1) = 0.3; Pz(i + 1, i) = 0.3; }
    d.seedCarriedForTest(group, ch, z, Pz, verts, arcs);
    return ch;
}
}  // namespace

TEST(NetworkInvariantCarryDriverTest, ResumeBlobV2SeedsSigma0AndV1IsLegacy) {
    NetworkInvariantCarryDriver::Config cfg;
    cfg.innov_gate = true;
    cfg.innov_calibrate = true;          // allocates the sigma0 accumulator
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver src(cfg);
    seedSimple(src, 0, z, Pz);
    std::ostringstream os; src.serializeCarry(os);
    const std::string v2_noS = os.str();
    EXPECT_NE(v2_noS.find("CARRY,v2"), std::string::npos) << "header must be v2";

    // (a) v2 blob without an S line: restores, sigma0 is not seeded (legacy behaviour).
    {
        NetworkInvariantCarryDriver d(cfg);
        std::istringstream is(v2_noS); std::string err;
        ASSERT_TRUE(d.restoreCarry(is, &err)) << err;
        EXPECT_TRUE(d.resumeCarryRestored());
        EXPECT_FALSE(d.resumeSigma0Seeded());
        EXPECT_DOUBLE_EQ(d.resumeSigma0Value(), 1.0);
    }
    // (b) v2 blob with an S line: sigma0 is seeded, value = sqrt(sigma0_sq).
    {
        const double s2 = 2.25, rows = 512.0;
        std::string blob = v2_noS;
        const std::size_t nl = blob.find('\n');
        blob.insert(nl + 1, "S," + std::to_string(s2) + "," + std::to_string(rows) + "\n");
        NetworkInvariantCarryDriver d(cfg);
        std::istringstream is(blob); std::string err;
        ASSERT_TRUE(d.restoreCarry(is, &err)) << err;
        EXPECT_TRUE(d.resumeSigma0Seeded());
        EXPECT_NEAR(d.resumeSigma0Value(), std::sqrt(s2), 1e-12);
        // the gate uses the seeded value (the seeded row count is past the warm-up)
        EXPECT_NEAR(d.innovInflationSigma0(), std::sqrt(s2), 1e-6)
            << "seeded accumulator must clear the warm-up gate, not fall back to raw formal";
    }
    // (c) legacy v1 header still accepted.
    {
        std::string v1 = v2_noS;
        v1.replace(v1.find("CARRY,v2"), 8, "CARRY,v1");
        NetworkInvariantCarryDriver d(cfg);
        std::istringstream is(v1); std::string err;
        ASSERT_TRUE(d.restoreCarry(is, &err)) << err;
        EXPECT_FALSE(d.resumeSigma0Seeded());
    }
}

// ---------------------------------------------------------------------------
// σ̂₀ seed reason.  `resumeSigma0Seeded()` is a bare bool; resumeSigma0Status() names
// which of four conditions suppressed the seed (cold run, calibration off, no S line,
// seed rejected by the >0 guard), since each calls for a different action.  A fixed
// "(v1 blob / calibration off)" text on the not-seeded branch is wrong on a cold run
// with calibration armed.  The status is a pure refactor of the bool it explains and
// never disagrees with it about whether the seed was taken.  Same pattern as the σ̂₀
// clamp-visibility gate (SPEC_pd_safe_dd_fold.md §11).  (RTB-7(d) RE-1)
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, ResumeSigma0SeedReasonIsNamedNotAsserted) {
    using Seed = NetworkInvariantCarryDriver::ResumeSeedState;
    NetworkInvariantCarryDriver::Config cfg_on;
    cfg_on.innov_gate = true;
    cfg_on.innov_calibrate = true;            // allocates the σ̂₀² accumulator
    NetworkInvariantCarryDriver::Config cfg_off = cfg_on;
    cfg_off.innov_calibrate = false;          // no accumulator exists at all

    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver src(cfg_on);
    seedSimple(src, 0, z, Pz);
    std::ostringstream os; src.serializeCarry(os);
    const std::string no_s = os.str();        // v2 header, no S line (accumulator empty)
    ASSERT_EQ(no_s.find("\nS,"), std::string::npos) << "fixture must carry no S line";

    auto withS = [&](const std::string& s2, const std::string& rows) {
        std::string blob = no_s;
        blob.insert(blob.find('\n') + 1, "S," + s2 + "," + rows + "\n");
        return blob;
    };
    auto restoreInto = [](NetworkInvariantCarryDriver& d, const std::string& blob) {
        std::istringstream is(blob); std::string err;
        EXPECT_TRUE(d.restoreCarry(is, &err)) << err;
    };
    // The status explains the bool; it does not decide it a second time.
    auto consistent = [](const NetworkInvariantCarryDriver& d) {
        const auto st = d.resumeSigma0Status();
        EXPECT_EQ(st.state == Seed::Seeded, d.resumeSigma0Seeded())
            << "the state must be a pure refactor of resumeSigma0Seeded(), not a "
               "second opinion";
        EXPECT_DOUBLE_EQ(st.sigma0, d.resumeSigma0Value());
    };

    // --- (a) The field case: cold run, calibration armed.  Neither "v1 blob" nor
    //         "calibration off" holds here. ---
    {
        NetworkInvariantCarryDriver d(cfg_on);
        const auto st = d.resumeSigma0Status();
        EXPECT_FALSE(d.resumeCarryRestored());
        EXPECT_EQ(st.state, Seed::NotRestored)
            << "a cold run must report COLD — not a blob-version claim and not a config "
               "claim, neither of which was checked";
        EXPECT_STREQ(NetworkInvariantCarryDriver::resumeSeedStateName(st.state), "cold");
        consistent(d);
    }
    // --- (b) A blob that carries a seed, restored by a calibration-off run: nothing
    //         reads the seed, and the gate is v1 by configuration, not pending a
    //         warm-up count.  The ignored seed stays reportable. ---
    {
        NetworkInvariantCarryDriver d(cfg_off);
        restoreInto(d, withS("2.25", "512"));
        const auto st = d.resumeSigma0Status();
        EXPECT_EQ(st.state, Seed::CalibrationOff);
        EXPECT_TRUE(d.resumeCarryRestored());
        EXPECT_DOUBLE_EQ(st.blob_sigma0_sq, 2.25)
            << "the offer the run ignored must stay reportable — that IS the diagnosis";
        consistent(d);
    }
    // --- (c) The legacy case: restored, no S line. ---
    {
        NetworkInvariantCarryDriver d(cfg_on);
        restoreInto(d, no_s);
        const auto st = d.resumeSigma0Status();
        EXPECT_EQ(st.state, Seed::NoSeedLine);
        EXPECT_DOUBLE_EQ(st.blob_sigma0_sq, 0.0);
        consistent(d);
    }
    // --- (d) An S line that parses but fails the >0 seed guard.  This is a writer-side
    //         defect, distinct from an old blob. ---
    for (const auto& bad : {withS("0", "512"), withS("2.25", "0")}) {
        NetworkInvariantCarryDriver d(cfg_on);
        restoreInto(d, bad);
        const auto st = d.resumeSigma0Status();
        EXPECT_EQ(st.state, Seed::Rejected)
            << "a rejected seed is not a legacy blob — the writing run serialized a "
               "degenerate accumulator";
        consistent(d);
    }
    // --- (e) The seeded path, unchanged. ---
    {
        NetworkInvariantCarryDriver d(cfg_on);
        restoreInto(d, withS("2.25", "512"));
        const auto st = d.resumeSigma0Status();
        EXPECT_EQ(st.state, Seed::Seeded);
        EXPECT_NEAR(st.sigma0, std::sqrt(2.25), 1e-12);
        EXPECT_DOUBLE_EQ(st.blob_rows, 512.0);
        EXPECT_STREQ(NetworkInvariantCarryDriver::resumeSeedStateName(st.state), "seeded");
        consistent(d);
    }
    // --- Every state has a distinct tag, so the log can be searched by state. ---
    std::set<std::string> tags;
    for (Seed s : {Seed::NotRestored, Seed::CalibrationOff, Seed::NoSeedLine,
                   Seed::Rejected, Seed::Seeded})
        tags.insert(NetworkInvariantCarryDriver::resumeSeedStateName(s));
    EXPECT_EQ(tags.size(), 5u) << "every ResumeSeedState needs its own greppable tag";
}

TEST(NetworkInvariantCarryDriverTest, RestoreCarryResetsPriorSideState) {
    // restoreCarry is a general install API and does not inherit stale per-epoch side
    // state (block layout, counters) from earlier driver work.  (RE-4)
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver d;
    const std::vector<ArcKey> ch = seedSimple(d, 0, z, Pz);
    NetworkInvariantCarryDriver::GroupInput g; g.key = 0;
    const StationCols col = ambLayout(3);
    for (int r = 0; r < 3; ++r) for (int s = 0; s < 3; ++s)
        g.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(0, 0, r, s), r, static_cast<int>(col.z[s])});
    ASSERT_FALSE(d.buildDdPriors({g}).empty());
    ASSERT_GT(d.numBlocks(), 0u);
    ASSERT_GT(d.counters().epochs, 0);

    NetworkInvariantCarryDriver src;
    seedSimple(src, 0, z, Pz);
    std::ostringstream os; src.serializeCarry(os);
    std::istringstream is(os.str()); std::string err;
    ASSERT_TRUE(d.restoreCarry(is, &err)) << err;
    EXPECT_EQ(d.numBlocks(), 0u)        << "stale block layout survived restoreCarry";
    EXPECT_EQ(d.counters().epochs, 0)   << "stale counters survived restoreCarry";
    EXPECT_EQ(d.counters().chords_born, 0)
        << "restoreCarry is the ONE place the run tallies are dropped — installing a "
           "FOREIGN carry starts a fresh instrument.  Plain reset() must PRESERVE them "
           "(MidRunResetPreservesRunCumulativeCounters); this is the contrast.";
}

TEST(NetworkInvariantCarryDriverTest, ValidateCarryBindingRejectsUnboundVertices) {
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver d;
    seedSimple(d, 0, z, Pz);
    std::set<int> sats; for (int s = 0; s < 3; ++s) sats.insert(satV(s));
    std::set<int> groups{0};
    std::string err;
    EXPECT_TRUE(d.validateCarryBinding(3, sats, groups, &err)) << err;

    err.clear();
    EXPECT_FALSE(d.validateCarryBinding(3, /*sat_vertices=*/{}, groups, &err))
        << "unknown satellite vertices must be rejected, not silently re-bound";
    EXPECT_FALSE(err.empty()) << "rejection must name the offending binding";

    err.clear();
    EXPECT_FALSE(d.validateCarryBinding(3, sats, /*group_ids=*/{}, &err))
        << "unknown group id must be rejected";
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// The diagonal-only carry blob (v3).  (RTB-7(e))
//
// The card's hypothesis (the resumed branch lives in the means, not in the
// cross-covariance) is an estimation claim that only the field A/B can decide.
// These gates check what the serialization promises and nothing more: the means
// and variances survive bit for bit, the cross-covariances are set to zero (and the
// restore reports it), the shape is chosen by the row tag rather than the header
// version so v1, v2 and v3 blobs all read, and the default argument leaves the v2
// writer byte-identical.
// ---------------------------------------------------------------------------
namespace {
/// A denser seed than @ref seedSimple: R×S bipartite, K = (R−1)(S−1) chords with a
/// fully populated PSD @f$P^z@f$, so every off-diagonal is non-zero and a dropped
/// cross-covariance cannot pass by accident.
std::vector<ArcKey> seedDense(NetworkInvariantCarryDriver& d, int group, int R, int S,
                              unsigned seed, Eigen::VectorXd& z, Eigen::MatrixXd& Pz) {
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::vector<ArcKey> all;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) all.emplace_back(rxV(r), satV(s));
    const std::set<ArcKey> arcs(all.begin(), all.end());
    std::vector<ArcKey> ch;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> u(-2.0, 2.0);
    z.resize(K);
    for (Eigen::Index i = 0; i < K; ++i) z(i) = u(gen);
    Eigen::MatrixXd M(K, K);
    for (Eigen::Index i = 0; i < K; ++i)
        for (Eigen::Index j = 0; j < K; ++j) M(i, j) = u(gen);
    Pz = M * M.transpose() + static_cast<double>(K) * Eigen::MatrixXd::Identity(K, K);
    d.seedCarriedForTest(group, ch, z, Pz, verts, arcs);
    return ch;
}
/// Number of lines whose first field equals @p tag.
std::size_t countRows(const std::string& blob, const std::string& tag) {
    std::size_t n = 0, pos = 0;
    const std::string needle = "\n" + tag + ",";
    // the leading "\n" keeps the header line from matching, as intended
    while ((pos = blob.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}
}  // namespace

TEST(NetworkInvariantCarryDriverTest, DiagonalCarryBlobV3DropsOnlyTheCrossCovariance) {
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver src;
    const std::vector<ArcKey> ch = seedDense(src, 3, /*R=*/4, /*S=*/4, /*seed=*/17, z, Pz);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    ASSERT_GE(K, 4);

    std::ostringstream os;
    src.serializeCarry(os, /*diagonal_only=*/true);
    const std::string v3 = os.str();
    EXPECT_NE(v3.find("CARRY,v3"), std::string::npos) << "diagonal blob must announce v3";
    EXPECT_EQ(countRows(v3, "PD"), 1u) << "exactly ONE diagonal row per component";
    EXPECT_EQ(countRows(v3, "P"), 0u)  << "no dense P rows may survive in a v3 blob";

    NetworkInvariantCarryDriver dst;
    std::istringstream is(v3); std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << "v3 restore failed: " << err;
    EXPECT_EQ(dst.totalZDim(), src.totalZDim());

    // (a) The means, where the branch is claimed to live, are bit-exact.
    // (b) The variances are bit-exact.  (c) The cross-covariances are zero, and the
    // source's were non-zero (otherwise (c) would be vacuous).
    for (Eigen::Index i = 0; i < K; ++i) {
        double zs = 0, vs = 0, zd = 0, vd = 0;
        ASSERT_TRUE(src.carriedDdForTest(3, ch[static_cast<std::size_t>(i)], zs, vs));
        ASSERT_TRUE(dst.carriedDdForTest(3, ch[static_cast<std::size_t>(i)], zd, vd));
        EXPECT_DOUBLE_EQ(zd, zs) << "mean must round-trip exactly, chord " << i;
        EXPECT_DOUBLE_EQ(vd, vs) << "variance must round-trip exactly, chord " << i;
        EXPECT_DOUBLE_EQ(vd, Pz(i, i)) << "restored variance must be the ORIGINAL diagonal";
    }
    double any_src_cross = 0.0;
    for (Eigen::Index i = 0; i < K; ++i)
        for (Eigen::Index j = i + 1; j < K; ++j) {
            double cs = 0, cd = 0;
            ASSERT_TRUE(src.carriedCrossForTest(3, ch[static_cast<std::size_t>(i)],
                                                ch[static_cast<std::size_t>(j)], cs));
            ASSERT_TRUE(dst.carriedCrossForTest(3, ch[static_cast<std::size_t>(i)],
                                                ch[static_cast<std::size_t>(j)], cd));
            any_src_cross = std::max(any_src_cross, std::abs(cs));
            EXPECT_DOUBLE_EQ(cd, 0.0) << "v3 restores Pz = diag(...) — cross (" << i << ","
                                      << j << ") must be exactly zero";
        }
    EXPECT_GT(any_src_cross, 1e-6)
        << "control: the source covariance must actually HAVE cross terms to drop";

    // Telemetry (invariant #4): without it, a diagonal restore cannot be told apart.
    EXPECT_TRUE(dst.resumeCarryRestored());
    EXPECT_TRUE(dst.resumeCarryDiagonal());
    EXPECT_EQ(dst.resumeCarryDiagonalBlocks(), 1);

    // Negative control: the same carry through the dense (v2) writer keeps the
    // cross-covariance and reports the dense shape.
    std::ostringstream os2; src.serializeCarry(os2);       // default arg == dense
    NetworkInvariantCarryDriver dense;
    std::istringstream is2(os2.str());
    ASSERT_TRUE(dense.restoreCarry(is2, &err)) << err;
    double cs = 0, cd = 0;
    ASSERT_TRUE(src.carriedCrossForTest(3, ch[0], ch[1], cs));
    ASSERT_TRUE(dense.carriedCrossForTest(3, ch[0], ch[1], cd));
    EXPECT_DOUBLE_EQ(cd, cs) << "the dense path must still round-trip the cross-covariance";
    EXPECT_FALSE(dense.resumeCarryDiagonal());
    EXPECT_EQ(dense.resumeCarryDiagonalBlocks(), 0);
}

TEST(NetworkInvariantCarryDriverTest, DiagonalCarryBlobShrinksAndDefaultIsUnchanged) {
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver src;
    const std::vector<ArcKey> ch = seedDense(src, 0, /*R=*/6, /*S=*/6, /*seed=*/5, z, Pz);
    const std::size_t K = ch.size();
    ASSERT_EQ(K, 25u);

    std::ostringstream a, b, c;
    src.serializeCarry(a);                        // default argument
    src.serializeCarry(b, /*diagonal_only=*/false);
    src.serializeCarry(c, /*diagonal_only=*/true);
    EXPECT_EQ(a.str(), b.str())
        << "the default argument MUST be the pre-RTB-7(e) writer, byte-for-byte";
    EXPECT_NE(a.str().find("CARRY,v2"), std::string::npos) << "default stays v2";

    // The diagonal blob stores K covariance numbers per component against K² for the
    // dense one.  The bar is conservative because the Z/H/X/A structure rows do not
    // shrink.
    const double ratio = static_cast<double>(a.str().size()) / static_cast<double>(c.str().size());
    EXPECT_GT(ratio, 5.0) << "K=" << K << ": dense " << a.str().size() << " B vs diagonal "
                          << c.str().size() << " B — expected roughly K-fold, got " << ratio;
}

TEST(NetworkInvariantCarryDriverTest, RestoreDispatchesOnRowTagNotHeaderVersion) {
    // The reader dispatches on content, so one converge run's artifact can seed both
    // A/B arms and every v1/v2 snapshot stays readable.
    Eigen::VectorXd z; Eigen::MatrixXd Pz;
    NetworkInvariantCarryDriver src;
    const std::vector<ArcKey> ch = seedDense(src, 0, /*R=*/3, /*S=*/3, /*seed=*/2, z, Pz);
    std::ostringstream d_os, f_os;
    src.serializeCarry(d_os, /*diagonal_only=*/true);
    src.serializeCarry(f_os, /*diagonal_only=*/false);
    const std::string diag_blob = d_os.str(), full_blob = f_os.str();

    auto relabel = [](std::string blob, const char* to) {
        blob.replace(blob.find("CARRY,v"), 8, to);
        return blob;
    };
    double cs = 0;
    ASSERT_TRUE(src.carriedCrossForTest(0, ch[0], ch[1], cs));
    ASSERT_GT(std::abs(cs), 1e-6);

    struct Case { std::string blob; const char* what; bool expect_diag; };
    const std::vector<Case> cases = {
        {relabel(diag_blob, "CARRY,v1"), "v1 header, PD rows", true},
        {relabel(diag_blob, "CARRY,v2"), "v2 header, PD rows", true},
        {diag_blob,                      "v3 header, PD rows", true},
        {relabel(full_blob, "CARRY,v1"), "v1 header, P rows (legacy)", false},
        {relabel(full_blob, "CARRY,v2"), "v2 header, P rows (legacy)", false},
        {relabel(full_blob, "CARRY,v3"), "v3 header, P rows", false},
    };
    for (const Case& k : cases) {
        NetworkInvariantCarryDriver d;
        std::istringstream is(k.blob); std::string err;
        ASSERT_TRUE(d.restoreCarry(is, &err)) << k.what << ": " << err;
        EXPECT_EQ(d.totalZDim(), src.totalZDim()) << k.what;
        double cd = 0;
        ASSERT_TRUE(d.carriedCrossForTest(0, ch[0], ch[1], cd)) << k.what;
        EXPECT_EQ(d.resumeCarryDiagonal(), k.expect_diag) << k.what;
        if (k.expect_diag) EXPECT_DOUBLE_EQ(cd, 0.0) << k.what;
        else               EXPECT_DOUBLE_EQ(cd, cs)  << k.what;
    }
}

TEST(NetworkInvariantCarryDriverTest, DiagonalCarryBlobRoundTripsEmptyBlocks) {
    // A degenerate zero-chord component beside a regular one: the dense writer emits no P
    // rows for it, so the diagonal writer emits no PD row either; otherwise the reader
    // desynchronizes.  The regular block keeps its means and variances and drops its
    // off-diagonal like any diagonal blob.
    const int R = 3, S = 3;
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::vector<ArcKey> all;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) all.emplace_back(rxV(r), satV(s));
    const std::set<ArcKey> arcs(all.begin(), all.end());
    std::vector<ArcKey> ch;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    Eigen::VectorXd z(K);
    for (Eigen::Index i = 0; i < K; ++i) z(i) = 0.25 * static_cast<double>(i + 1);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(K, K) * 3.0;
    P(0, 1) = 0.7; P(1, 0) = 0.7;

    NetworkInvariantCarryDriver src;
    src.seedCarriedForTest(1, ch, z, P, verts, arcs);
    src.seedCarriedForTest(2, {}, Eigen::VectorXd(0), Eigen::MatrixXd(0, 0), verts, arcs);

    std::ostringstream os;
    src.serializeCarry(os, /*diagonal_only=*/true);
    NetworkInvariantCarryDriver dst;
    std::istringstream is(os.str()); std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << "regular/empty v3 restore failed: " << err;
    EXPECT_EQ(dst.resumeCarryDiagonalBlocks(), 1)
        << "only the K-chord block has a diagonal to store; the empty one has none";
    for (Eigen::Index i = 0; i < K; ++i) {
        double rz = 0.0, rv = 0.0;
        ASSERT_TRUE(dst.carriedDdForTest(1, ch[static_cast<std::size_t>(i)], rz, rv)) << i;
        EXPECT_DOUBLE_EQ(rz, z(i)) << "means are exact, row " << i;
        EXPECT_DOUBLE_EQ(rv, P(i, i)) << "variances are exact, row " << i;
    }
    double cd = 1.0;
    ASSERT_TRUE(dst.carriedCrossForTest(1, ch[0], ch[1], cd));
    EXPECT_DOUBLE_EQ(cd, 0.0) << "the diagonal blob drops the cross-covariance";
}

TEST(NetworkInvariantCarryDriverTest, RestoreRefusesARetiredLanePairBlock) {
    // The B header keeps the retired WL-3 lane fields (pair flag, λ₁, λ₂) as zeros.  A blob
    // with a non-zero pair flag holds a joint 2K lane block that no reader can interpret as
    // a per-band block, so the restore fails (the resume counts a cold start).
    std::set<int> verts{rxV(0), rxV(1), satV(0), satV(1)};
    const std::set<ArcKey> arcs{ArcKey(rxV(0), satV(0)), ArcKey(rxV(0), satV(1)),
                                ArcKey(rxV(1), satV(0)), ArcKey(rxV(1), satV(1))};
    NetworkInvariantCarryDriver src;
    src.seedCarriedForTest(1, {ArcKey(rxV(1), satV(1))},
                           (Eigen::VectorXd(1) << 0.5).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.02).finished(), verts, arcs);
    std::ostringstream os;
    src.serializeCarry(os, /*diagonal_only=*/false);
    const std::string blob = os.str();
    ASSERT_NE(blob.find("\nB,1,0,0,0,1,1,4,4\n"), std::string::npos)
        << "the per-band header writes the three retired fields as zeros:\n" << blob;
    {
        NetworkInvariantCarryDriver ok;
        std::istringstream is(blob); std::string err;
        EXPECT_TRUE(ok.restoreCarry(is, &err)) << err;
    }
    std::string lane = blob;
    lane.replace(lane.find("\nB,1,0,"), 7, "\nB,1,1,");
    NetworkInvariantCarryDriver dst;
    std::istringstream is(lane); std::string err;
    EXPECT_FALSE(dst.restoreCarry(is, &err)) << "a lane pair block must be refused";
    EXPECT_NE(err.find("lane pair block"), std::string::npos) << err;
}

// ---------------------------------------------------------------------------
// Gate 2: the carry accumulates information (variance shrinks, mean → integer).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, CarryAccumulatesAndConverges) {
    const int R = 4, S = 4;
    const Net n = makeNet(11, R, S);
    std::mt19937 gen(99);
    std::normal_distribution<double> noiseDist(0.0, 0.05);

    NetworkInvariantCarryDriver drv;
    const ArcKey probe(rxV(2), satV(3));   // a chord under star(0,0)
    const int p0 = 0, q0 = 0;
    const double trueDD = double(n.z[2][3] - n.z[2][q0] - n.z[p0][3] + n.z[p0][q0]);

    // The DD's posterior variance has a floor set by the (uncarried) shared-parameter
    // uncertainty in this synthetic network; the carry accumulates the direct part on
    // top of that floor, so the variance is monotone non-increasing and strictly below
    // its first-epoch value, while the mean converges to the integer truth.
    double var_prev = 1e30, z_final = 0.0, var_first = 0.0, var_final = 0.0;
    for (int e = 0; e < 12; ++e) {
        std::vector<std::vector<double>> noise(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) noise[r][s] = noiseDist(gen);
        driveEpoch(drv, n, p0, q0, &noise, /*dt=*/1.0);
        double z = 0, var = 0;
        ASSERT_TRUE(drv.carriedDdForTest(0, probe, z, var)) << "probe chord not carried at epoch " << e;
        EXPECT_LT(var, var_prev + 1e-12) << "carried variance must not grow (epoch " << e << ")";
        if (e == 0) var_first = var;
        var_prev = var; z_final = z; var_final = var;
    }
    EXPECT_LT(var_final, var_first - 1e-4) << "the carry must accumulate information (variance strictly drops)";
    EXPECT_NEAR(z_final, trueDD, 0.05) << "carried DD mean must converge to the integer truth";
}

// ---------------------------------------------------------------------------
// Gate 3: production-seam DatumFreedom, the falsifier for phase C3b.
// Two per-epoch tree realizations of the same noisy stream ⇒ identical estimable DD.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, ProductionSeamDatumFreedom) {
    const int R = 4, S = 4, E = 6;
    const Net n = makeNet(2027, R, S);
    // One shared noisy observation stream for both realizations.
    std::mt19937 gen(555);
    std::normal_distribution<double> noiseDist(0.0, 0.03);
    std::vector<std::vector<std::vector<double>>> noise(E, std::vector<std::vector<double>>(R, std::vector<double>(S)));
    for (int e = 0; e < E; ++e) for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) noise[e][r][s] = noiseDist(gen);

    // Realization A: burn-in star(0,0), then vary the receiver pivot p=e%R.
    // Realization B: burn-in star(0,0), then vary the satellite pivot q=e%S.
    auto pivotA = [&](int e) { return std::pair<int,int>(e == 0 ? 0 : (e % R), 0); };
    auto pivotB = [&](int e) { return std::pair<int,int>(0, e == 0 ? 0 : (e % S)); };

    // The target estimable DD as a cycle over arcs (chord (2,3) w.r.t. star(0,0)).
    ZComponent zcTarget(starTree(0, 0, R, S));
    zcTarget.setEstimate({ArcKey(rxV(2), satV(3))}, Eigen::VectorXd::Zero(1), Eigen::MatrixXd::Identity(1, 1));
    const std::map<ArcKey, int> target = zcTarget.fundamentalCycle(ArcKey(rxV(2), satV(3)));

    NetworkInvariantCarryDriver drvA, drvB;
    std::vector<double> ddA(E), ddB(E), varA(E), czA(E), czB(E);
    std::set<ArcKey> chordsA_last, chordsB_last;
    for (int e = 0; e < E; ++e) {
        const auto [pa, qa] = pivotA(e);
        const auto [pb, qb] = pivotB(e);
        const auto xrA = driveEpoch(drvA, n, pa, qa, &noise[e], 1.0);
        const auto xrB = driveEpoch(drvB, n, pb, qb, &noise[e], 1.0);
        ddA[e] = estimableDD(xrA, 2, 3, 0, 0);
        ddB[e] = estimableDD(xrB, 2, 3, 0, 0);
        czA[e] = drvA.carriedEstimableForTest(0, target);   // carry, solve-independent
        czB[e] = drvB.carriedEstimableForTest(0, target);
        // record chord sets to show that the realizations diverge
        chordsA_last.clear(); chordsB_last.clear();
        if (drvA.numBlocks() > 0) { auto c = drvA.blockChordsForTest(0); chordsA_last.insert(c.begin(), c.end()); }
        if (drvB.numBlocks() > 0) { auto c = drvB.blockChordsForTest(0); chordsB_last.insert(c.begin(), c.end()); }
        double z, v; if (drvA.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v)) varA[e] = v; else varA[e] = -1;
    }

    // The gate: the estimable DD is identical under the two tree realizations, both as
    // reconstructed from the solve and as read from the carry (basis invariance of the
    // driver's carry through the full produce round trip).
    for (int e = 0; e < E; ++e) {
        EXPECT_NEAR(ddA[e], ddB[e], 1e-7)
            << "epoch " << e << ": reconstructed DD leaked the datum choice";
        EXPECT_NEAR(czA[e], czB[e], 1e-7)
            << "epoch " << e << ": carried DD leaked the datum choice (A=" << czA[e] << " B=" << czB[e] << ")";
    }

    // Non-vacuous: the realizations' chord bases differ...
    EXPECT_NE(chordsA_last, chordsB_last)
        << "the two realizations must elect different fundamental-cycle bases";
    // ...and the carry is load-bearing (its variance sharpened over the run).
    ASSERT_GT(varA.front(), 0.0);
    EXPECT_LT(varA.back(), varA.front())
        << "the DD carry must accumulate information across epochs (else the gate is vacuous)";
    // ...and both arms re-based the carry (Fable §3: tree_swaps ≥ 1 on an informative
    // store, so the pivot change the DatumFreedom claim is about took place).
    EXPECT_GT(drvA.counters().tree_swaps, 0)
        << "arm A varied the receiver pivot every epoch — the carry must have been re-based";
    EXPECT_GT(drvB.counters().tree_swaps, 0)
        << "arm B varied the satellite pivot every epoch — the carry must have been re-based";
    // ...and both converge to the same integer truth.
    const double trueDD = double(n.z[2][3] - n.z[2][0] - n.z[0][3] + n.z[0][0]);
    EXPECT_NEAR(ddA.back(), trueDD, 0.05);
}

// ---------------------------------------------------------------------------
// Gate 4: component merges and fresh-chord births are counted, and the surviving DDs
// are preserved across the topology change.  (The test drives a merge; no split.)
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, MergeSplitAndFreshChords) {
    NetworkInvariantCarryDriver drv;
    using Arc = NetworkInvariantCarryDriver::ArcInput;
    auto arc = [](int r, int s, bool tree) { return Arc{rxV(r), satV(s), tree, r, 2 + s}; };

    // Epoch 1: two disjoint components, {r0,r1}×{s0,s1} and {r2,r3}×{s2,s3}.
    // Each is a 4-cycle: tree = path, one chord.
    NetworkInvariantCarryDriver::GroupInput g1; g1.key = 0;
    g1.arcs = { arc(0, 0, true), arc(0, 1, true), arc(1, 0, true), arc(1, 1, false),
                arc(2, 2, true), arc(2, 3, true), arc(3, 2, true), arc(3, 3, false) };
    auto b1 = drv.buildDdPriors({g1});
    EXPECT_EQ(b1.size(), 2u) << "two disjoint components ⇒ two blocks";
    // Installing a posterior needs an estimator and a solve; this epoch checks only the
    // births and the block structure.
    EXPECT_EQ(drv.counters().chords_born, 2);
    EXPECT_EQ(drv.counters().merges, 0);

    // Populate the carry without a solve: reset, then seed a known carry directly so the
    // next epoch runs through the driver's own reconcile.
    drv.reset();
    // Seed component A carry (chord (1,1)) and component B carry (chord (3,3)).
    drv.seedCarriedForTest(0, {ArcKey(rxV(1), satV(1))}, (Eigen::VectorXd(1) << 4.0).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.01).finished(),
                           {rxV(0), rxV(1), satV(0), satV(1)},
                           {ArcKey(rxV(0), satV(0)), ArcKey(rxV(0), satV(1)), ArcKey(rxV(1), satV(0)), ArcKey(rxV(1), satV(1))});
    drv.seedCarriedForTest(0, {ArcKey(rxV(3), satV(3))}, (Eigen::VectorXd(1) << -2.0).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.02).finished(),
                           {rxV(2), rxV(3), satV(2), satV(3)},
                           {ArcKey(rxV(2), satV(2)), ArcKey(rxV(2), satV(3)), ArcKey(rxV(3), satV(2)), ArcKey(rxV(3), satV(3))});

    // Epoch 2: merge the two through a bridge tree edge (r1,s2) and keep both chords.
    NetworkInvariantCarryDriver::GroupInput g2; g2.key = 0;
    g2.arcs = { arc(0, 0, true), arc(0, 1, true), arc(1, 0, true), arc(1, 1, false),
                arc(2, 2, true), arc(2, 3, true), arc(3, 2, true), arc(3, 3, false),
                arc(1, 2, true) };                       // the bridge
    auto b2 = drv.buildDdPriors({g2});
    EXPECT_EQ(b2.size(), 1u) << "the bridge merges the two components into one block";
    EXPECT_EQ(drv.counters().merges, 1);
    // Both carried DDs survive the merge (block-identity union).
    ASSERT_EQ(b2[0].zhat.size(), 2);
    std::set<double> means{b2[0].zhat(0), b2[0].zhat(1)};
    EXPECT_TRUE(means.count(4.0) && means.count(-2.0)) << "carried DD means must transfer verbatim through a merge";
}

// ---------------------------------------------------------------------------
// Gate 5: reset(), the qz predict, and the empty-epoch off path.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, ResetPredictAndOff) {
    NetworkInvariantCarryDriver::Config cfg; cfg.qz = 0.5;
    NetworkInvariantCarryDriver drv(cfg);
    drv.seedCarriedForTest(0, {ArcKey(rxV(1), satV(1))}, (Eigen::VectorXd(1) << 1.0).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.10).finished(),
                           {rxV(0), rxV(1), satV(0), satV(1)},
                           {ArcKey(rxV(0), satV(0)), ArcKey(rxV(0), satV(1)), ArcKey(rxV(1), satV(0)), ArcKey(rxV(1), satV(1))});
    double z, v0; ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v0));
    drv.beginEpoch(/*dt=*/2.0);
    double v1; ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v1));
    EXPECT_NEAR(v1, v0 + cfg.qz * 2.0, 1e-12) << "qz·dt must inflate the carried variance";

    EXPECT_GT(drv.totalZDim(), 0);
    drv.reset();
    EXPECT_EQ(drv.totalZDim(), 0) << "reset() must cold-start the carry";
    EXPECT_TRUE(drv.buildDdPriors({}).empty()) << "an empty epoch emits no blocks (OFF path)";
    EXPECT_EQ(drv.numBlocks(), 0u);
}

// ---------------------------------------------------------------------------
// Gate 6: (ẑ⁺, Pᶻ⁺) are estimable and therefore datum-invariant (Fable §1 gate
// assertion).  The same carried DD prior is folded, the same network is solved under two
// different clock datums, and the posterior mean and covariance readbacks are identical.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, PosteriorMeanAndCovDatumInvariant) {
    const int R = 4, S = 4;
    const Net n = makeNet(88, R, S);
    const int p = 0, q = 0;
    std::vector<ArcKey> chords;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) chords.emplace_back(rxV(r), satV(s));
    std::set<int> verts; std::set<ArcKey> arcs;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) arcs.insert(ArcKey(rxV(r), satV(s)));
    const Eigen::Index K = static_cast<Eigen::Index>(chords.size());
    std::mt19937 g(5); std::uniform_real_distribution<double> u(-1, 1);
    Eigen::VectorXd zc(K); for (Eigen::Index i = 0; i < K; ++i) zc(i) = u(g);
    Eigen::MatrixXd Mr(K, K); for (Eigen::Index i = 0; i < K; ++i) for (Eigen::Index j = 0; j < K; ++j) Mr(i, j) = u(g);
    const Eigen::MatrixXd Pz = 0.02 * (Mr * Mr.transpose()) + 0.3 * Eigen::MatrixXd::Identity(K, K);

    NetworkInvariantCarryDriver drv;
    drv.seedCarriedForTest(0, chords, zc, Pz, verts, arcs);

    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(S), pbCol(S);
    for (int s = 0; s < S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(R);
    for (int r = 0; r < R; ++r) ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, nullptr, p);
    est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q)}));

    const NetworkEstimator::Datum dA = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd xsA = est.solve(dA);
    const Eigen::VectorXd meanA = est.ddPriorPosteriorMean(0, xsA);
    const Eigen::MatrixXd covA  = est.ddPriorPosteriorCovariance(0, dA);

    const NetworkEstimator::Datum dB = {{est.clockColumn(n.sats[1]), 0.0}};
    const Eigen::VectorXd xsB = est.solve(dB);   // re-solve, different clock datum
    const Eigen::VectorXd meanB = est.ddPriorPosteriorMean(0, xsB);
    const Eigen::MatrixXd covB  = est.ddPriorPosteriorCovariance(0, dB);

    EXPECT_TRUE(meanA.isApprox(meanB, 1e-7)) << "ẑ⁺ leaked the datum choice";
    EXPECT_TRUE(covA.isApprox(covB, 1e-7))   << "Pᶻ⁺ leaked the datum choice";
    // Non-vacuous: the two datums move the shared solution, and information
    // was folded (posterior below the prior).
    EXPECT_GT((xsA - xsB).cwiseAbs().maxCoeff(), 1e-3);
    EXPECT_LT(covA.diagonal().maxCoeff(), Pz.diagonal().maxCoeff());
}

// ---------------------------------------------------------------------------
// Gate 7: the multi-birth joint covariance is coupled (Fable §2.5).  On a bootstrap
// (empty store) epoch every chord is born; chords whose fundamental cycles share a tree
// edge have a nonzero carried cross-covariance (a per-chord independent harvest would
// leave it exactly zero).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, MultiBirthJointCovarianceIsCoupled) {
    const int R = 3, S = 3;
    const Net n = makeNet(21, R, S);
    const int p = 0, q = 0;
    NetworkInvariantCarryDriver drv;   // empty store → bootstrap epoch, all chords fresh

    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(S), pbCol(S);
    for (int s = 0; s < S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(R);
    for (int r = 0; r < R; ++r) ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, nullptr, p);
    const auto blocks = drv.buildDdPriors({makeGroup(n, ids, p, q)});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(drv.counters().chords_born, static_cast<long long>(blocks[0].zhat.size()));
    EXPECT_EQ(drv.counters().chords_carried, 0);

    est.setDdPriors(blocks);
    const NetworkEstimator::Datum d = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(d);
    drv.installPosterior(est, x_s, d);

    // Chords (1,1) and (1,2) both close through the star(0,0) tree edges at receiver 1
    // and receiver 0; their cycles share tree edges, so their DDs are correlated.
    double cov = 0.0, v11 = 0.0, v12 = 0.0, z;
    ASSERT_TRUE(drv.carriedCrossForTest(0, ArcKey(rxV(1), satV(1)), ArcKey(rxV(1), satV(2)), cov));
    ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v11));
    ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(2)), z, v12));
    EXPECT_GT(std::abs(cov), 1e-6) << "shared-tree-edge chords must be coupled (joint fold, not per-chord)";
    // A valid covariance: |cross| ≤ √(var·var).
    EXPECT_LE(std::abs(cov), std::sqrt(v11 * v12) + 1e-9);
}

// ---------------------------------------------------------------------------
// Deterministic-parallel installPosterior (network.parallel_seam): the per-block
// posterior mean/cov and innovation are computed in parallel, but the σ̂₀ accumulator
// feed (NetworkCarryVce::addRow), the D5 gate decisions, the flag/counter appends and
// the carry install stay serial in (block, chord) order.  The carried DDs, the run
// counters and the σ̂₀ that gates the next epoch are therefore byte-identical to the
// serial install at any thread count.  The test runs a multi-epoch history with the D5
// gate and LS-VCE calibration on and compares after every epoch, so a σ̂₀ carry-over
// divergence (which would flip a later gate decision) is caught at the epoch it appears.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InstallPosteriorParallelMatchesSerialByteForByte) {
    const int R = 4, S = 4;
    const Net n = makeNet(4242, R, S);
    const int p = 0, q = 0;
    NetworkInvariantCarryDriver::Config cfg;
    cfg.innov_gate = true;
    cfg.innov_calibrate = true;
    cfg.innov_calibrate_window = 20.0;
    cfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drvS(cfg), drvP(cfg);   // serial vs parallel install

    auto oneEpoch = [&](NetworkInvariantCarryDriver& drv, int n_threads,
                        const std::vector<std::vector<double>>& noise) {
        NetworkEstimator est;
        std::vector<Eigen::Index> clkCol(S), pbCol(S);
        for (int s = 0; s < S; ++s) {
            clkCol[s] = est.registerClock(n.sats[s]);
            pbCol[s]  = est.registerPhaseBias(n.sats[s], "L1");
        }
        est.beginEpoch();
        std::vector<std::size_t> ids(R);
        for (int r = 0; r < R; ++r) ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, &noise[r], p);
        drv.beginEpoch(1.0);
        est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q)}));
        const NetworkEstimator::Datum d = {{est.clockColumn(n.sats[0]), 0.0}};
        const Eigen::VectorXd x_s = est.solve(d);
        drv.installPosterior(est, x_s, d, n_threads);
    };

    std::mt19937 g(7);
    std::normal_distribution<double> nz(0.0, 0.02);
    for (int ep = 0; ep < 12; ++ep) {
        std::vector<std::vector<double>> noise(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) noise[r][s] = nz(g);
        oneEpoch(drvS, /*n_threads=*/1, noise);
        oneEpoch(drvP, /*n_threads=*/8, noise);

        ASSERT_EQ(drvS.totalZDim(), drvP.totalZDim()) << "epoch " << ep;
        for (int r = 1; r < R; ++r)
            for (int s = 1; s < S; ++s) {
                double zS, vS, zP, vP;
                const bool okS = drvS.carriedDdForTest(0, ArcKey(rxV(r), satV(s)), zS, vS);
                const bool okP = drvP.carriedDdForTest(0, ArcKey(rxV(r), satV(s)), zP, vP);
                ASSERT_EQ(okS, okP) << "epoch " << ep << " chord " << r << "," << s;
                if (okS) {
                    EXPECT_EQ(zS, zP) << "epoch " << ep << " chord " << r << "," << s;
                    EXPECT_EQ(vS, vP) << "epoch " << ep << " chord " << r << "," << s;
                }
            }
        const auto& cS = drvS.counters();
        const auto& cP = drvP.counters();
        EXPECT_EQ(cS.innov_flags, cP.innov_flags)     << "epoch " << ep;
        EXPECT_EQ(cS.innov_retaps, cP.innov_retaps)   << "epoch " << ep;
        EXPECT_EQ(cS.chords_carried, cP.chords_carried) << "epoch " << ep;
        // σ̂₀ gates the next epoch, so it is compared bit for bit, not approximately.
        EXPECT_EQ(drvS.innovInflationSigma0(), drvP.innovInflationSigma0()) << "epoch " << ep;
    }
    EXPECT_GT(drvS.totalZDim(), 0) << "non-vacuous: the carry must have accumulated DDs";
}

// ---------------------------------------------------------------------------
// Gate 8: slip integration on a chord arc, A/B with a negative control (card 4 C3c).
// After a tight carry is warmed, an integer jump is applied to a chord's true ambiguity.
// Signaled (drvSig): the driver re-taps that DD fresh (free of the stale carry),
// refloating that one chord only and leaving its neighbours' carry intact.
// Unsignaled (drvCtl, the negative control): the stiff q_z=0 carry pins the stale DD and
// the contamination is visible, which shows that the slip feed is load-bearing.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, SlippedChordArcRefloatsFreshVsContaminatingControl) {
    const int R = 4, S = 4, WARM = 8;
    Net n = makeNet(31, R, S);
    const int p = 0, q = 0;
    const ArcKey slipArc(rxV(2), satV(3));   // a chord under star(0,0)
    const ArcKey keepArc(rxV(1), satV(1));   // an untouched chord

    // One shared noise stream so the two drivers warm identically (the A/B differs only
    // in whether the slip is signaled at the jump epoch).
    std::mt19937 gen(7);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    std::vector<std::vector<std::vector<double>>> warmNoise;
    for (int e = 0; e < WARM; ++e) warmNoise.push_back(noiseEpoch());
    const auto jumpNoise = noiseEpoch();

    NetworkInvariantCarryDriver drvSig, drvCtl;
    for (int e = 0; e < WARM; ++e) {
        driveEpoch(drvSig, n, p, q, &warmNoise[e], 1.0);
        driveEpoch(drvCtl, n, p, q, &warmNoise[e], 1.0);
    }
    double zk, v_keep_before; ASSERT_TRUE(drvSig.carriedDdForTest(0, keepArc, zk, v_keep_before));
    const double trueDD_old = double(n.z[2][3] - n.z[2][0] - n.z[0][3] + n.z[0][0]);
    const long long sk_sig_before = drvSig.counters().slip_kills;

    // The integer jump on the chord's ambiguity.
    n.z[2][3] += 5;
    const double trueDD_new = trueDD_old + 5.0;
    const std::set<std::pair<int, int>> slip{{2, 3}};

    const auto xrSig = driveEpoch(drvSig, n, p, q, &jumpNoise, 1.0, &slip);         // signaled
    const auto xrCtl = driveEpoch(drvCtl, n, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr); // control
    const double dd_sig = estimableDD(xrSig, 2, 3, 0, 0);
    const double dd_ctl = estimableDD(xrCtl, 2, 3, 0, 0);

    // Signaled: only the slipped chord refloated (no collateral erasure of neighbours).
    EXPECT_EQ(drvSig.counters().slip_kills - sk_sig_before, 1)
        << "a chord-arc slip must refloat exactly its own chord";
    EXPECT_EQ(drvCtl.counters().slip_kills, 0) << "the unsignaled control refloats nothing";
    // Signaled re-taps to the post-slip truth; control stays pinned near the pre-slip value.
    EXPECT_LT(std::abs(dd_sig - trueDD_new), 0.3)
        << "signaled slip must re-tap the DD fresh to the post-slip integer";
    EXPECT_GT(std::abs(dd_ctl - trueDD_new), std::abs(dd_sig - trueDD_new) + 1.0)
        << "the unsignaled stale carry must contaminate the DD (load-bearing wiring)";
    // The untouched neighbour kept its accumulated (tight) carry and was not refloated.
    double v_keep_after; ASSERT_TRUE(drvSig.carriedDdForTest(0, keepArc, zk, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9) << "no collateral erasure of an unrelated chord";

    // ...and after the slip the signaled arm re-converges to the post-slip integer.
    double dd_last = 0.0;
    for (int e = 0; e < 6; ++e) { auto ns = noiseEpoch(); dd_last = estimableDD(driveEpoch(drvSig, n, p, q, &ns, 1.0), 2, 3, 0, 0); }
    EXPECT_NEAR(dd_last, trueDD_new, 0.05) << "the DD must re-converge to the post-slip integer truth";
}

// ---------------------------------------------------------------------------
// Gate 9: a slip on a tree arc invalidates every fundamental cycle through it, so all
// dependent chords refloat while chords not touching it survive (spec §2: a slipped arc
// is excluded from the A_old membership ⇒ any cycle whose support includes it re-taps).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, SlippedTreeArcRefloatsDependentChordsOnly) {
    const int R = 4, S = 4, WARM = 8;
    Net n = makeNet(41, R, S);
    const int p = 0, q = 0;
    std::mt19937 gen(13);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    NetworkInvariantCarryDriver drv;
    for (int e = 0; e < WARM; ++e) { auto ns = noiseEpoch(); driveEpoch(drv, n, p, q, &ns, 1.0); }
    double z, v_keep_before; ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v_keep_before));

    // Tree edge (2,0) under star(0,0) (s==q).  Chords (2,1),(2,2),(2,3) close through it;
    // chords at other receivers do not.  Jump its ambiguity and signal the slip on it.
    n.z[2][0] += 4;
    const double trueDD_new = double(n.z[2][3] - n.z[2][0] - n.z[0][3] + n.z[0][0]);
    const std::set<std::pair<int, int>> slip{{2, 0}};
    const long long sk_before = drv.counters().slip_kills;
    auto ns = noiseEpoch();
    const auto xr = driveEpoch(drv, n, p, q, &ns, 1.0, &slip);

    // Exactly the three chords through (2,0) refloated (nothing else).
    EXPECT_EQ(drv.counters().slip_kills - sk_before, 3)
        << "a tree-edge slip must refloat every chord whose cycle passes through it (3), no more";
    // A refloated chord (2,3) re-tapped to the post-slip truth...
    EXPECT_LT(std::abs(estimableDD(xr, 2, 3, 0, 0) - trueDD_new), 0.3)
        << "a chord through the slipped tree edge must re-tap fresh";
    // ...while a chord not through (2,0) kept its tight carry.
    double v_keep_after; ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9)
        << "chords not touching the slipped tree edge survive with their accumulated carry";
}

// ===========================================================================
// The estimation-side fine-slip innovation gate (card 4 D5, spec D5 Def-1).
// ===========================================================================

// ---------------------------------------------------------------------------
// Gate 10: a mid-arc slip is caught by the innovation gate, A/B with a negative control.
// A tight carry is warmed, then a chord's true ambiguity jumps without a slip signal
// (the coarse MW/LLI/IF-TD layer missed it).  Gate on: that chord's post-solve
// innovation alone trips ⇒ it re-taps fresh (data-only), its arc feeds the next epoch's
// slip set, the DD re-converges, and the untouched neighbour keeps its accumulated carry.
// Gate off (control): the stiff q_z=0 carry pins the stale DD ⇒ contamination visible.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, MidSlipCaughtByInnovationGateVsControl) {
    const int R = 4, S = 4, WARM = 8;
    Net n = makeNet(53, R, S);
    const int p = 0, q = 0;
    const ArcKey slipArc(rxV(2), satV(3));   // a chord under star(0,0)
    const ArcKey keepArc(rxV(1), satV(1));   // an untouched neighbour chord

    NetworkInvariantCarryDriver::Config gcfg; gcfg.innov_gate = true; gcfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drvGate(gcfg);   // D5 gate armed
    NetworkInvariantCarryDriver drvCtl;          // gate off = the C3c path (negative control)

    // Shared noise stream so both arms warm identically (they differ only in the gate).
    std::mt19937 gen(17);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    std::vector<std::vector<std::vector<double>>> warmNoise;
    for (int e = 0; e < WARM; ++e) warmNoise.push_back(noiseEpoch());
    for (int e = 0; e < WARM; ++e) {
        driveEpoch(drvGate, n, p, q, &warmNoise[e], 1.0);
        driveEpoch(drvCtl,  n, p, q, &warmNoise[e], 1.0);
    }
    // No false alarms during a clean warm-up.
    EXPECT_EQ(drvGate.counters().innov_flags, 0) << "the gate must not fire on clean warm-up epochs";
    double zk, v_keep_before; ASSERT_TRUE(drvGate.carriedDdForTest(0, keepArc, zk, v_keep_before));
    const double trueDD_old = double(n.z[2][3] - n.z[2][0] - n.z[0][3] + n.z[0][0]);

    // The unsignaled integer jump (a fine slip the coarse layer missed), chosen above the
    // fixture's k·σ_v floor (σ_v ≈ formal weight scale here; see spec D5 detection-power note).
    n.z[2][3] += 12;
    const double trueDD_new = trueDD_old + 12.0;
    const auto jumpNoise = noiseEpoch();
    driveEpoch(drvGate, n, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);  // gate must catch it
    driveEpoch(drvCtl,  n, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);  // control: no catch

    // Only the slipped chord is flagged; the control (gate off) never evaluates.
    EXPECT_EQ(drvGate.counters().innov_flags, 1) << "the innovation gate must flag exactly the slipped chord";
    EXPECT_EQ(drvGate.counters().innov_retaps, 1);
    EXPECT_EQ(drvCtl.counters().innov_flags, 0)  << "gate OFF must never evaluate (byte-identical C3c path)";
    // The flag names the slipped chord and nothing else.
    const auto slips = innovSlips(drvGate);
    ASSERT_EQ(slips.size(), 1u);
    EXPECT_EQ(*slips.begin(), std::make_pair(2, 3)) << "the flag must name the slipped (r,s) arc";

    // Step 1 (this epoch): the flagged chord is reset toward the data (mean d_post = ẑ+v, a
    // partial move: v is the post-fit innovation, reduced from the true jump by the fold's
    // border absorption, the −GᵀΣG term of the spec's Cov(v)) and its variance is inflated to
    // the data-only scale, which releases the stiff stale prior.  The control's stiff q_z=0
    // carry stays pinned near the pre-slip value with a tight variance ⇒ contamination.  (The
    // complete re-tap happens next epoch, in step 2; this epoch keeps a bounded contamination
    // on a value that is inert.)
    double zg, vg, zc, vc;
    ASSERT_TRUE(drvGate.carriedDdForTest(0, slipArc, zg, vg));
    ASSERT_TRUE(drvCtl.carriedDdForTest(0, slipArc, zc, vc));
    EXPECT_LT(std::abs(zg - trueDD_new), std::abs(zc - trueDD_new) - 1.0)
        << "gate: the flagged chord's carry moved toward the post-slip data (control stays stale)";
    // The untouched neighbour kept its accumulated (tight) carry: no collateral reset.
    double v_keep_after; ASSERT_TRUE(drvGate.carriedDdForTest(0, keepArc, zk, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9) << "no collateral erasure of an unrelated chord";

    // Step 2 (the A/B that decides the gate): both arms are driven forward.  The gate arm feeds
    // each epoch's innovation flags into the next epoch's slip set (the C3c membership-exclusion
    // re-tap, as the ApplicationNetwork plumbing does), so it re-converges to the post-slip
    // integer; the control (no fine-slip layer, stiff q_z=0 carry) stays at the stale value.
    const long long sk_before = drvGate.counters().slip_kills;
    double dd_gate = 0.0, dd_ctl = 0.0;
    std::set<std::pair<int, int>> feed = slips;      // epoch-8 flags re-tap at epoch 9
    for (int e = 0; e < 7; ++e) {
        auto ns = noiseEpoch();
        dd_gate = estimableDD(driveEpoch(drvGate, n, p, q, &ns, 1.0, &feed), 2, 3, 0, 0);
        feed = innovSlips(drvGate);                  // carry this epoch's flags to the next
        dd_ctl = estimableDD(driveEpoch(drvCtl, n, p, q, &ns, 1.0), 2, 3, 0, 0);
    }
    EXPECT_GE(drvGate.counters().slip_kills - sk_before, 1)
        << "the innovation flag must re-tap the chord via the next epoch's slip funnel";
    EXPECT_NEAR(dd_gate, trueDD_new, 0.05) << "the gate arm re-converges clean to the post-slip integer";
    EXPECT_GT(std::abs(dd_ctl - trueDD_new), 1.0)
        << "the control (no fine-slip reset) stays contaminated by the stale carry";
}

// ---------------------------------------------------------------------------
// Gate 10b: what the funnel refloat costs and what it buys (D5 v4,
// network.carry_invariant_innov_retap_only).
//
// Gate 8 fixes the topology: a slip on a chord arc refloats one chord, its own
// (`slip_kills` delta == 1).  So when the D5 gate flags a chord and the caller feeds that
// arc into the next epoch's slipped set, the only DD the exclusion can kill is the one
// installPosterior just reset.  That does not make the feed-back redundant for a genuine
// slip, and this test measures the difference.  Two arms with identical noise and gate
// differ only in whether the flags are fed back (arm A = the ApplicationNetwork plumbing,
// arm B = feed-back suppressed):
//   (1) arm A re-converges to the post-slip integer; arm B does not, because the in-epoch
//       reset is a partial move and the re-established stiff carry keeps the remainder;
//   (2) arm B refloats nothing (slip_kills stays 0), which is the churn the knob removes;
//   (3) arm B's carried variance ends tighter, because the accumulated carry survives.
// (2) and (3) are the gain and (1) is the cost, acceptable only where there is no jump to
// complete.  The knob therefore suppresses the kill only on arcs that the raw observables
// measured as continuous; stepped and unmeasurable arcs refloat as before (invariant #1).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, D5FunnelRefloatCompletesResetAndCostsTheCarry) {
    const int R = 4, S = 4, WARM = 8;
    Net nA = makeNet(53, R, S);
    const int p = 0, q = 0;
    const ArcKey slipArc(rxV(2), satV(3));   // a chord under star(0,0)

    NetworkInvariantCarryDriver::Config gcfg; gcfg.innov_gate = true; gcfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drvFun(gcfg);   // arm A: flags fed into next epoch's slip set
    NetworkInvariantCarryDriver drvRto(gcfg);   // arm B: flags not fed (retap_only)

    std::mt19937 gen(17);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    // Both arms see the same network object and the same noise stream at every epoch, so
    // the only difference between them is the funnel feed.
    std::vector<std::vector<std::vector<double>>> warmNoise;
    for (int e = 0; e < WARM; ++e) warmNoise.push_back(noiseEpoch());
    for (int e = 0; e < WARM; ++e) {
        driveEpoch(drvFun, nA, p, q, &warmNoise[e], 1.0);
        driveEpoch(drvRto, nA, p, q, &warmNoise[e], 1.0);
    }
    const double trueDD_old = double(nA.z[2][3] - nA.z[2][0] - nA.z[0][3] + nA.z[0][0]);

    // The unsignaled integer jump that the coarse layer missed: the gate's own population.
    nA.z[2][3] += 12;
    const double trueDD_new = trueDD_old + 12.0;
    const auto jumpNoise = noiseEpoch();
    driveEpoch(drvFun, nA, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);
    driveEpoch(drvRto, nA, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);
    ASSERT_EQ(drvFun.counters().innov_flags, 1) << "both arms must flag the same single chord";
    ASSERT_EQ(drvRto.counters().innov_flags, 1);
    const long long skFun0 = drvFun.counters().slip_kills;
    const long long skRto0 = drvRto.counters().slip_kills;

    // Arm A feeds each epoch's flags forward (the C3c membership exclusion); arm B does not.
    std::set<std::pair<int, int>> feed = innovSlips(drvFun);
    double ddFun = 0.0, ddRto = 0.0;
    for (int e = 0; e < 7; ++e) {
        auto ns = noiseEpoch();
        ddFun = estimableDD(driveEpoch(drvFun, nA, p, q, &ns, 1.0, &feed), 2, 3, 0, 0);
        feed  = innovSlips(drvFun);
        ddRto = estimableDD(driveEpoch(drvRto, nA, p, q, &ns, 1.0, /*slipped=*/nullptr), 2, 3, 0, 0);
    }

    // (1) For a genuine slip the in-epoch reset is a partial move (v is the post-fit
    //     innovation, reduced from the true jump by the fold's border absorption −GᵀΣG),
    //     and with the stiff carry re-established at σ̂₀²·Cov(v)_ii the residual does not
    //     decay: 0.48 cyc after 7 epochs here, against 0.006 for the arm that refloats.
    //     The kill hits one chord, its own, and that chord's refloat completes the reset.
    //     This is why the production knob is scoped to corroborated-flat arcs (no jump to
    //     complete) and a stepped or unmeasurable arc still refloats (invariant #1).
    EXPECT_NEAR(ddFun, trueDD_new, 0.05)
        << "the funnel arm re-converges to the post-slip integer";
    EXPECT_GT(std::abs(ddRto - trueDD_new), std::abs(ddFun - trueDD_new) + 0.1)
        << "REGRESSION GUARD on the invariant-#1 cost: suppressing the refloat after a REAL "
           "slip must leave a visible residual.  If this ever passes silently, the in-epoch "
           "reset became sufficient and the flat-scoping below can be widened — re-measure "
           "before doing so.";
    // (2) Arm B never refloated, while arm A did.  This delta is the removed churn.
    EXPECT_GE(drvFun.counters().slip_kills - skFun0, 1)
        << "the funnel arm must refloat the chord the gate already reset";
    EXPECT_EQ(drvRto.counters().slip_kills - skRto0, 0)
        << "suppressing the feed-back must refloat NOTHING — the flag's own arc is the only "
           "cycle it could kill (GATE 8's cycle-support-1 property)";
    // (3) Arm B kept accumulating on the reset value; arm A restarted from nothing.
    double zF, vF, zR, vR;
    ASSERT_TRUE(drvFun.carriedDdForTest(0, slipArc, zF, vF));
    ASSERT_TRUE(drvRto.carriedDdForTest(0, slipArc, zR, vR));
    EXPECT_LT(vR, vF)
        << "the un-refloated chord must end TIGHTER — that variance is the chord lifetime the "
           "limit cycle was destroying";
}

// ---------------------------------------------------------------------------
// Gate 10c: the gate does not test a chord it has no prior for (D5 v5,
// network.carry_invariant_innov_carried_only).  Without this, born chords drive the churn.
//
// The test statistic is |v_i| / sqrt(Cov(v)_ii), and Cov(v) is the H0 covariance of the
// innovation about the carried prior.  A chord born this epoch has no carried prior (its
// prior is the diffuse Config::fresh_prior_var, so sqrt(var_v) = sqrt(1e6) = 1000 m), so
// the ratio is not a hypothesis test and the reset it triggers is a no-op (a diffuse
// chord's posterior already is the data-only tap-off).  The flag's one lasting effect is
// the funnel entry that re-births the chord next epoch, where it flags again; a chord
// re-born every epoch never accumulates anything.
//
// The first epoch of any run consists entirely of born chords, which makes it a clean
// probe: with the born-chord skip, the gate flags nothing there, while the unrestricted
// gate flags on nothing but the correction frame's distance from x_lin.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InnovationGateSkipsBornChordsUnderCarriedOnly) {
    const int R = 4, S = 4;
    // A large true ambiguity set: in the correction frame a fresh chord's innovation is
    // its distance from x_lin, and the fixture's x_lin is 0.  This matches the field, where
    // a cold-start ambiguity is tens of km from the linearization point.
    Net n = makeNet(71, R, S);
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) n.z[r][s] += 500 * (r + 1) * (s + 2);
    const int p = 0, q = 0;

    NetworkInvariantCarryDriver::Config gOn;  gOn.innov_gate = true; gOn.innov_k = 4.0;
    NetworkInvariantCarryDriver::Config gCo = gOn; gCo.innov_carried_only = true;
    NetworkInvariantCarryDriver drvAll(gOn), drvCar(gCo);

    std::mt19937 gen(23);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };

    // Epoch 1: every chord is born.
    const auto e1 = noiseEpoch();
    driveEpoch(drvAll, n, p, q, &e1, 1.0);
    driveEpoch(drvCar, n, p, q, &e1, 1.0);
    EXPECT_GT(drvAll.counters().innov_flags, 0)
        << "the unrestricted gate must flag born chords — if it does not, the fixture no "
           "longer reproduces the field shape and the rest of this test proves nothing";
    EXPECT_EQ(drvCar.counters().innov_flags, 0)
        << "carried-only must flag NOTHING in an all-born epoch: there is no carried prior, "
           "so there is no H0 and no test";
    EXPECT_GT(drvCar.counters().innov_born_skipped, 0)
        << "and the skip must be COUNTED (standing invariant #4)";
    // The skip is counted before the threshold test, so it covers the whole born
    // population, including the rows that would have passed anyway.
    EXPECT_GE(drvCar.counters().innov_born_skipped, drvAll.counters().innov_flags);

    // The solve is unchanged: skipping a born chord's gate cannot change the epoch's
    // estimates, because the flag's in-epoch action (install ẑ+v at the data-only
    // variance) equals what a diffuse chord's posterior already is.
    const auto xrAll = driveEpoch(drvAll, n, p, q, &e1, 1.0);
    const auto xrCar = driveEpoch(drvCar, n, p, q, &e1, 1.0);
    EXPECT_NEAR(estimableDD(xrAll, 2, 3, 0, 0), estimableDD(xrCar, 2, 3, 0, 0), 1e-6)
        << "the DD after a born-chord epoch must not depend on whether the gate looked";

    // A carried chord is still gated: warm both arms, then jump one arc unsignaled.
    for (int e = 0; e < 8; ++e) {
        const auto ns = noiseEpoch();
        driveEpoch(drvAll, n, p, q, &ns, 1.0);
        driveEpoch(drvCar, n, p, q, &ns, 1.0);
    }
    const long long carBefore = drvCar.counters().innov_flags;
    n.z[2][3] += 12;
    const auto jump = noiseEpoch();
    driveEpoch(drvCar, n, p, q, &jump, 1.0, /*slipped=*/nullptr);
    EXPECT_GT(drvCar.counters().innov_flags, carBefore)
        << "carried-only must NOT blunt the gate on a genuinely carried chord — that is the "
           "fine-slip response (standing invariant #1) and it is not what this knob touches";

    // Every flag the knob lets through has an informative prior.  chord_carried alone is
    // topological (cycle in A_old, no slip) and says nothing about information: a chord
    // carried from a diffuse predecessor inherits a diffuse projected prior (sqrt(Cov(v))
    // near 1000 m) and is still untestable.  This loop guards against keying on it alone.
    for (const double s : drvCar.lastInnovFlagTerms().empty()
                              ? std::vector<double>{}
                              : [&] {
                                    std::vector<double> out;
                                    for (const auto& t : drvCar.lastInnovFlagTerms())
                                        out.push_back(t.var_v);
                                    return out;
                                }())
        EXPECT_LT(s, 0.5 * gCo.fresh_prior_var)
            << "a flag survived carried-only with a (near-)diffuse prior — there is no H0 "
               "to test against it, so this is the born-chord loop wearing a carried label";
}

// ---------------------------------------------------------------------------
// Gate 11: false-alarm floor.  No slip, many noisy epochs, k=4 ⇒ (near) zero flags.
// Checks that Cov(v) is calibrated (the standardized innovation is ~N(0,1) under H0);
// a Cov(v) that is too small would flood the carry with spurious refloats.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InnovationGateFalseAlarmFloor) {
    const int R = 4, S = 4, E = 25;
    const Net n = makeNet(61, R, S);
    const int p = 0, q = 0;
    NetworkInvariantCarryDriver::Config cfg; cfg.innov_gate = true; cfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drv(cfg);

    std::mt19937 gen(29);
    std::normal_distribution<double> nd(0.0, 0.02);
    for (int e = 0; e < E; ++e) {
        std::vector<std::vector<double>> noise(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) noise[r][s] = nd(gen);
        driveEpoch(drv, n, p, q, &noise, 1.0);
    }
    // (R-1)(S-1)=9 chords × ~24 carried epochs ≈ 216 chord-epochs; at k=4 the per-test
    // false-alarm expectation is ~0.014 ⇒ zero is by far the most likely outcome.
    EXPECT_EQ(drv.counters().innov_flags, 0)
        << "the k=4 gate must not fire on a clean (no-slip) stream — Cov(v) is well-calibrated";
    // Non-vacuous: the carry was populated and testable over the run.
    EXPECT_GT(drv.totalZDim(), 0);
    EXPECT_GT(drv.counters().chords_carried, 0);
}

// ---------------------------------------------------------------------------
// Gate 12: the innovation Cov(v) is an estimable-quantity readout: datum-invariant,
// valid (PSD, positive diagonal), and the innovation v itself is datum-invariant (the
// data-implied DD minus the prior is gauge-free).  Non-vacuous: the datums move x_s.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InnovationCovarianceDatumInvariantAndValid) {
    const int R = 4, S = 4;
    const Net n = makeNet(88, R, S);
    const int p = 0, q = 0;
    std::vector<ArcKey> chords;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) chords.emplace_back(rxV(r), satV(s));
    std::set<int> verts; std::set<ArcKey> arcs;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) arcs.insert(ArcKey(rxV(r), satV(s)));
    const Eigen::Index K = static_cast<Eigen::Index>(chords.size());
    std::mt19937 g(5); std::uniform_real_distribution<double> u(-1, 1);
    Eigen::VectorXd zc(K); for (Eigen::Index i = 0; i < K; ++i) zc(i) = u(g);
    Eigen::MatrixXd Mr(K, K); for (Eigen::Index i = 0; i < K; ++i) for (Eigen::Index j = 0; j < K; ++j) Mr(i, j) = u(g);
    const Eigen::MatrixXd Pz = 0.02 * (Mr * Mr.transpose()) + 0.3 * Eigen::MatrixXd::Identity(K, K);

    NetworkInvariantCarryDriver drv;
    drv.seedCarriedForTest(0, chords, zc, Pz, verts, arcs);

    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(S), pbCol(S);
    for (int s = 0; s < S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(R);
    for (int r = 0; r < R; ++r) ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, nullptr, p);
    est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q)}));

    const NetworkEstimator::Datum dA = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd xsA = est.solve(dA);
    Eigen::VectorXd vA; Eigen::MatrixXd covA; est.ddPriorInnovation(0, xsA, dA, vA, covA);

    const NetworkEstimator::Datum dB = {{est.clockColumn(n.sats[1]), 0.0}};
    const Eigen::VectorXd xsB = est.solve(dB);   // different clock datum
    Eigen::VectorXd vB; Eigen::MatrixXd covB; est.ddPriorInnovation(0, xsB, dB, vB, covB);

    ASSERT_EQ(vA.size(), K);
    ASSERT_EQ(covA.rows(), K);
    EXPECT_TRUE(vA.isApprox(vB, 1e-7))   << "the innovation v (data DD − prior) is estimable ⇒ datum-invariant";
    EXPECT_TRUE(covA.isApprox(covB, 1e-7)) << "Cov(v) leaked the datum choice";
    // Valid covariance: symmetric, positive diagonal, PSD.
    EXPECT_TRUE(covA.isApprox(covA.transpose(), 1e-9));
    EXPECT_GT(covA.diagonal().minCoeff(), 0.0) << "innovation variance must be positive";
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(covA);
    EXPECT_GT(es.eigenvalues().minCoeff(), -1e-9) << "Cov(v) must be PSD (M⁻¹ − GᵀΣG by construction)";
    // Non-vacuous: the two datums move the shared solution.
    EXPECT_GT((xsA - xsB).cwiseAbs().maxCoeff(), 1e-3);
    // Cov(v) = Pᶻ + (B − GᵀΣG), and the bracket is a valid (data-implied DD) covariance ⪰ 0,
    // so the innovation variance is bounded below by the carried prior variance on the
    // diagonal (the S = H P Hᵀ + R structure).  This checks the shape, not only positivity.
    EXPECT_GT((covA.diagonal() - Pz.diagonal()).minCoeff(), -1e-9)
        << "innovation variance must be >= the prior variance (Cov(v) = Pᶻ + data-only DD var)";
}

// ---------------------------------------------------------------------------
// Gate 13: links ddPriorInnovation to ddPriorPosteriorMean.  With a diffuse prior the
// gain Pᶻ M → I, so the posterior update ẑ⁺ − ẑ equals the innovation v.  This ties the
// two kernel readouts to the same underlying innovation and fixes the sign and scale of v.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InnovationMatchesDiffusePriorPosterior) {
    const int R = 3, S = 3;
    const Net n = makeNet(21, R, S);
    const int p = 0, q = 0;
    // A diffuse store: every chord born fresh (fresh_prior_var=1e6 ⇒ Pᶻ M ≈ I).
    NetworkInvariantCarryDriver drv;

    const std::string sig = "L1";
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(S), pbCol(S);
    for (int s = 0; s < S; ++s) { clkCol[s] = est.registerClock(n.sats[s]); pbCol[s] = est.registerPhaseBias(n.sats[s], sig); }
    est.beginEpoch();
    std::vector<std::size_t> ids(R);
    for (int r = 0; r < R; ++r) ids[r] = addNetStation(est, n, r, clkCol, pbCol, p, q, nullptr, p);
    est.setDdPriors(drv.buildDdPriors({makeGroup(n, ids, p, q)}));

    const NetworkEstimator::Datum d = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(d);
    const Eigen::VectorXd mean = est.ddPriorPosteriorMean(0, x_s);   // ẑ⁺ = ẑ + Pᶻ M v
    Eigen::VectorXd v; Eigen::MatrixXd cov_v; est.ddPriorInnovation(0, x_s, d, v, cov_v);
    // Fresh chords have ẑ = 0, so ẑ⁺ ≈ v (Pᶻ M ≈ I for the diffuse prior).
    ASSERT_EQ(mean.size(), v.size());
    ASSERT_GT(mean.size(), 0);
    EXPECT_TRUE(mean.isApprox(v, 1e-3))
        << "diffuse-prior posterior update must equal the innovation\nmean " << mean.transpose()
        << "\n v   " << v.transpose();
}

// ===========================================================================
// LS-VCE calibration of Cov(v) (σ̂₀²) (card 4 D5 v2).
// ===========================================================================

// ---------------------------------------------------------------------------
// Gate 14: under a misspecified R (obs noise ≫ the formal weight, so the formal Cov(v) is
// optimistic, as in the field at 10-30×), the raw gate (v1) over-fires, while the
// calibrated gate (v2) absorbs the inflation into σ̂₀ and stops false-alarming; σ̂₀
// recovers the misspecification.  A/B on one shared noise stream (only calibration differs).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, CalibrationTamesFalseAlarmsAndRecoversSigma0) {
    const int R = 4, S = 4;
    const double SIG_INJ = 4.0;   // inject noise 4× the formal σ (weight=1) ⇒ formal Cov(v) optimistic
    const Net n = makeNet(71, R, S);

    NetworkInvariantCarryDriver::Config c1; c1.innov_gate = true; c1.innov_k = 4.0;      // v1: raw formal
    NetworkInvariantCarryDriver::Config c2 = c1;
    c2.innov_calibrate = true; c2.innov_calibrate_window = 40.0;                          // v2: calibrated
    NetworkInvariantCarryDriver drvV1(c1), drvV2(c2);

    std::mt19937 gen(303);
    std::normal_distribution<double> nd(0.0, SIG_INJ);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    // Warm past the σ̂₀ warm-up (~200 rows / 9 chords ≈ 23 epochs).  During warm-up v2 is the
    // raw test (σ̂₀=1), so only the tail in the calibrated regime is measured.
    const int WARM = 28, TAIL = 20;
    for (int e = 0; e < WARM; ++e) { auto ns = noiseEpoch(); driveEpoch(drvV1, n, 0, 0, &ns, 1.0); driveEpoch(drvV2, n, 0, 0, &ns, 1.0); }
    const long long f1_warm = drvV1.counters().innov_flags, f2_warm = drvV2.counters().innov_flags;
    for (int e = 0; e < TAIL; ++e) { auto ns = noiseEpoch(); driveEpoch(drvV1, n, 0, 0, &ns, 1.0); driveEpoch(drvV2, n, 0, 0, &ns, 1.0); }
    const long long f1_tail = drvV1.counters().innov_flags - f1_warm;
    const long long f2_tail = drvV2.counters().innov_flags - f2_warm;

    // v1 (raw) over-fires: the injected noise sits several formal-σ out, so the k=4 gate trips
    // constantly on ordinary scatter.
    EXPECT_GT(f1_tail, TAIL) << "raw gate must over-fire under a misspecified R (>1 flag/epoch)";
    // v2 (calibrated) absorbs the inflation into σ̂₀ ⇒ the recalibrated innovation is ~N(0,1)
    // ⇒ the false-alarm rate drops.
    EXPECT_LT(f2_tail * 3, f1_tail) << "calibration must tame the false-alarm storm";
    // σ̂₀ recovered a real (>1) inflation from the misspecification.
    EXPECT_GT(drvV2.innovInflationSigma0(), 1.5) << "σ̂₀ must detect the optimistic formal Cov(v)";
    EXPECT_LT(drvV2.innovInflationSigma0(), 6.0) << "σ̂₀ must not diverge";
    // The raw gate is the calibration-off v1 exactly (sanity check: calibration is the only change).
    EXPECT_EQ(drvV1.innovInflationSigma0(), 1.0);
}

// ---------------------------------------------------------------------------
// Gate 15: calibration does not blind the gate.  Under the same misspecified R (σ̂₀≈3-4),
// a real slip well above the raised threshold k·σ̂₀ is still caught and re-tapped.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, CalibrationRetainsDetection) {
    const int R = 4, S = 4, WARM = 30;
    Net n = makeNet(83, R, S);
    const ArcKey slipArc(rxV(2), satV(3));
    NetworkInvariantCarryDriver::Config cfg; cfg.innov_gate = true; cfg.innov_k = 4.0;
    cfg.innov_calibrate = true; cfg.innov_calibrate_window = 40.0;
    NetworkInvariantCarryDriver drv(cfg);

    std::mt19937 gen(97);
    std::normal_distribution<double> nd(0.0, 4.0);   // misspecified R ⇒ σ̂₀ > 1
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    for (int e = 0; e < WARM; ++e) { auto ns = noiseEpoch(); driveEpoch(drv, n, 0, 0, &ns, 1.0); }
    ASSERT_GT(drv.innovInflationSigma0(), 1.5) << "σ̂₀ must be calibrated up before the slip";
    const long long flags_before = drv.counters().innov_flags;

    // A slip far above the raised threshold k·σ̂₀·σ_formal (σ̂₀~3-4, σ_formal~1.3 ⇒ ~16-20 cyc).
    n.z[2][3] += 40;
    auto jumpNoise = noiseEpoch();
    driveEpoch(drv, n, 0, 0, &jumpNoise, 1.0, /*slipped=*/nullptr);

    EXPECT_GT(drv.counters().innov_flags, flags_before)
        << "a slip above k·σ̂₀ must still be caught after calibration raised the threshold";
    // The slipped chord is among the flags (calibration didn't misattribute).
    bool found = false;
    for (const auto& [g, key] : drv.lastInnovFlags()) if (key == slipArc) found = true;
    EXPECT_TRUE(found) << "the slipped chord must be the one flagged";
}

// ---------------------------------------------------------------------------
// Gate 16: σ̂₀ clamp visibility.  A reported σ̂₀ of 1.00× can come from four distinct
// states, and only one of them means "calibrated"; the run-end line has to tell them
// apart (a raw σ̂₀² below 1 is clamped up to 1: SPEC_pd_safe_dd_fold.md §10 finding 2).
//
// Reproduces the field case: innovations far smaller than the formal Cov(v) (the
// signature of a carry that is not taking data) drive the raw σ̂₀² below 1, the floor
// clamp binds and σ̂₀ still reads exactly 1.00.  The state reports `floored`, not
// `active`, and a counter records how long it lasted.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, Sigma0FloorClampIsVisibleNotSilent) {
    using Infl = NetworkInvariantCarryDriver::InflationState;
    const int R = 4, S = 4;
    const Net n = makeNet(71, R, S);

    // (a) calibration off ⇒ Off, σ̂₀ = 1, and the tally records "v1 by configuration".
    NetworkInvariantCarryDriver::Config c_off; c_off.innov_gate = true; c_off.innov_k = 4.0;
    NetworkInvariantCarryDriver drvOff(c_off);
    // (b) calibration on against innovations far below the formal σ ⇒ raw σ̂₀² < 1 ⇒ floored.
    NetworkInvariantCarryDriver::Config c_lo = c_off;
    c_lo.innov_calibrate = true; c_lo.innov_calibrate_window = 40.0;
    NetworkInvariantCarryDriver drvLo(c_lo);
    // (c) the same knobs against a misspecified R (formal Cov(v) optimistic) ⇒ active.
    NetworkInvariantCarryDriver drvHi(c_lo);

    std::mt19937 gen(4211);
    std::normal_distribution<double> quiet(0.0, 0.02);  // ≪ formal σ ⇒ raw σ̂₀² ≪ 1
    std::normal_distribution<double> loud (0.0, 4.0);   // ≫ formal σ ⇒ raw σ̂₀² ≫ 1
    auto epochOf = [&](std::normal_distribution<double>& d) {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = d(gen);
        return v;
    };

    // Warm-up is ~200 rows / 9 chords ≈ 23 epochs; before it ends, the calibrated arms
    // report WarmUp: armed, but the gate is still the raw formal test.
    for (int e = 0; e < 5; ++e) {
        auto q = epochOf(quiet); driveEpoch(drvLo, n, 0, 0, &q, 1.0);
        auto l = epochOf(loud);  driveEpoch(drvHi, n, 0, 0, &l, 1.0);
        auto o = epochOf(quiet); driveEpoch(drvOff, n, 0, 0, &o, 1.0);
    }
    EXPECT_EQ(drvLo.innovInflationStatus().state, Infl::WarmUp);
    EXPECT_EQ(drvLo.innovInflationSigma0(), 1.0) << "warm-up must run the raw formal test";

    for (int e = 0; e < 30; ++e) {
        auto q = epochOf(quiet); driveEpoch(drvLo, n, 0, 0, &q, 1.0);
        auto l = epochOf(loud);  driveEpoch(drvHi, n, 0, 0, &l, 1.0);
        auto o = epochOf(quiet); driveEpoch(drvOff, n, 0, 0, &o, 1.0);
    }

    // --- (a) Off: no state changes, and the counters record configuration, not warm-up. ---
    const auto s_off = drvOff.innovInflationStatus();
    EXPECT_EQ(s_off.state, Infl::Off);
    EXPECT_EQ(s_off.sigma0, 1.0);
    EXPECT_EQ(drvOff.counters().d5_calib_off_epochs, drvOff.counters().d5_calib_epochs);
    EXPECT_EQ(drvOff.counters().d5_calib_floored_epochs, 0);

    // --- (b) The field case: floored, reporting 1.00×, with the raw value recoverable. ---
    const auto s_lo = drvLo.innovInflationStatus();
    EXPECT_EQ(s_lo.state, Infl::Floored)
        << "innovations below the formal Cov(v) must report FLOORED, not a calibrated 1.00×"
        << " (raw σ̂₀² = " << s_lo.raw_sigma0_sq << ")";
    EXPECT_EQ(s_lo.sigma0, 1.0) << "the clamp itself is unchanged — only its visibility";
    EXPECT_LT(s_lo.raw_sigma0_sq, 1.0) << "the raw estimate is what the clamp hid";
    EXPECT_GT(drvLo.counters().d5_calib_floored_epochs, 0)
        << "invariant #4: a clamped run must be COUNTABLE, not merely inspectable";
    EXPECT_EQ(drvLo.counters().d5_calib_active_epochs, 0);
    EXPECT_LT(drvLo.counters().d5_calib_raw_min, 1.0);

    // --- (c) Calibrated: active, σ̂₀ > 1, no floor tally. ---
    const auto s_hi = drvHi.innovInflationStatus();
    EXPECT_EQ(s_hi.state, Infl::Active);
    EXPECT_GT(s_hi.sigma0, 1.0);
    EXPECT_EQ(drvHi.counters().d5_calib_floored_epochs, 0);
    EXPECT_GT(drvHi.counters().d5_calib_active_epochs, 0);

    // --- The status leaves the value unchanged in every state (byte identity). ---
    for (const auto* d : {&drvOff, &drvLo, &drvHi})
        EXPECT_EQ(d->innovInflationSigma0(), d->innovInflationStatus().sigma0);

    // --- The tallies partition the installs exactly (every install lands in one state). ---
    for (const auto* d : {&drvOff, &drvLo, &drvHi}) {
        const auto& c = d->counters();
        EXPECT_EQ(c.d5_calib_epochs,
                  c.d5_calib_off_epochs + c.d5_calib_warmup_epochs + c.d5_calib_floored_epochs
                      + c.d5_calib_capped_epochs + c.d5_calib_active_epochs);
        EXPECT_GT(c.d5_calib_epochs, 0);
    }
}

// ===========================================================================
// reset() is a carry cold start, not an instrument reset (SPEC_pd_safe_dd_fold.md INC-0).
//
// The install seam calls reset() mid-run from its exception handler (the catch around
// installPosterior in NetworkInstallCarry, reachable because the ddPriorPosterior*
// read-backs throw).  Both readers of these counters are run-scoped: networkCarryDdFoldRunEnd's
// run-end INC-0 line reads the live driver counter, and the seam assigns the cumulative
// totals into ctx.carry and differences them against the previous epoch's snapshot.
// Zeroing on reset would under-report the run total and drive the per-epoch churn deltas
// negative (invariant #4: an instrument that loses its own history misreports).
// ===========================================================================
TEST(NetworkInvariantCarryDriverTest, MidRunResetPreservesRunCumulativeCounters) {
    const int R = 4, S = 4;
    const Net n = makeNet(4242, R, S);
    NetworkInvariantCarryDriver drv;

    std::mt19937 gen(9);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    for (int e = 0; e < 4; ++e) { auto ns = noiseEpoch(); driveEpoch(drv, n, 0, 0, &ns, 1.0); }

    const NetworkInvariantCarryDriver::Counters before = drv.counters();
    // Non-vacuous precondition: there is history available to lose.
    ASSERT_GT(before.epochs, 0);
    ASSERT_GT(before.chords_born, 0);
    ASSERT_GT(before.chords_carried, 0);
    ASSERT_GT(before.max_z_dim, 0);
    ASSERT_GT(drv.totalZDim(), 0);

    drv.reset();   // as in the install seam's catch: the carry is dropped mid-run

    // The carry half of the contract is unchanged (gate 5 covers it; it is re-asserted
    // here so that counter survival cannot come from a weaker cold start).
    EXPECT_EQ(drv.totalZDim(), 0) << "reset() must still cold-start the carry";
    EXPECT_EQ(drv.numBlocks(), 0u);
    EXPECT_EQ(drv.counters().z_dim, 0)
        << "z_dim is a live dimension, not a tally — it tracks the carry it describes";

    // The instrument half.
    EXPECT_EQ(drv.counters().epochs, before.epochs)
        << "the run instrument must SURVIVE a mid-run carry reset (invariant #4)";
    EXPECT_EQ(drv.counters().chords_born,        before.chords_born);
    EXPECT_EQ(drv.counters().chords_carried,     before.chords_carried);
    EXPECT_EQ(drv.counters().slip_kills,         before.slip_kills);
    EXPECT_EQ(drv.counters().innov_flags,        before.innov_flags);
    EXPECT_EQ(drv.counters().innov_retaps,       before.innov_retaps);
    EXPECT_EQ(drv.counters().d5_nonpos_var_rows, before.d5_nonpos_var_rows);
    EXPECT_EQ(drv.counters().max_z_dim, before.max_z_dim)
        << "run MAXIMA are history too — a reset must not lower the run's peak";

    // The seam differences these totals epoch over epoch, so the gate is monotonicity
    // across the reset, not only survival of the snapshot.
    { auto ns = noiseEpoch(); driveEpoch(drv, n, 0, 0, &ns, 1.0); }
    EXPECT_GT(drv.counters().chords_born, before.chords_born)
        << "post-reset births must EXTEND the run total, never restart it (a restart "
           "makes NetworkInstallCarry's ep_chord_births delta negative)";
    EXPECT_GE(drv.counters().epochs, before.epochs + 1);
}

// d5_nonpos_var_rows across a mid-run reset (SPEC_pd_safe_dd_fold.md INC-0).  This
// counter is the D5 gate's blind-spot tally and its only readout is the run-end INC-0
// line, so a reset that erased it would leave no second source.  The test drives it as
// the field does: an indefinite carried Pᶻ with pd_safe off leaves the fold inert
// (M := 0), so Cov(v) = Minv − GᵀΣG inherits the negative diagonals and every affected
// row is skipped inside the gate.
TEST(NetworkInvariantCarryDriverTest, D5NonPosVarRowsSurviveMidRunReset) {
    NetworkInvariantCarryDriver::Config cfg;
    cfg.innov_gate = true;    // the gate that owns the skip
    cfg.pd_safe    = false;   // legacy fold: no repair, no rebirth (the inert path)
    NetworkInvariantCarryDriver drv(cfg);

    const int R = 3, S = 3;
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    std::set<ArcKey> arcs;
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) arcs.emplace(rxV(r), satV(s));
    std::vector<ArcKey> ch;
    for (int r = 1; r < R; ++r) for (int s = 1; s < S; ++s) ch.emplace_back(rxV(r), satV(s));
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());
    // One eigenvalue far negative against a B of order 1: the shape of the corrupt field
    // snapshots (the DegenerateBlockRebirthsLoudly fixture, pd_safe off).
    Eigen::MatrixXd Pz = Eigen::MatrixXd::Identity(K, K) * 1.0e-2;
    Pz(0, 0) = -1.0e6;
    drv.seedCarriedForTest(0, ch, Eigen::VectorXd::Zero(K), Pz, verts, arcs);

    const Net n = makeNet(20260803u, R, S);
    std::vector<std::vector<double>> noise(R, std::vector<double>(S, 0.0));
    driveEpoch(drv, n, 0, 0, &noise, 1.0);

    const long long nonpos = drv.counters().d5_nonpos_var_rows;
    ASSERT_GT(nonpos, 0)
        << "precondition: the inert fold must actually yield a non-positive Cov(v) row, "
           "or this gate is asserting 0 == 0 — the exact failure the card exists to end";

    drv.reset();
    EXPECT_EQ(drv.counters().d5_nonpos_var_rows, nonpos)
        << "d5_nonpos_var_rows is reported ONLY by the run-end INC-0 line; a mid-run "
           "carry reset must not erase it (there is no second source)";
}

}  // namespace GnssProcessor
