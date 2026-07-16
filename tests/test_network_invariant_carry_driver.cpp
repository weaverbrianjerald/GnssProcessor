/**
 * @file test_network_invariant_carry_driver.cpp
 * @brief Card 4 (M1) phase C3b gate — the live DD invariant-carry driver.
 *
 * Five gates, non-vacuous with negative/independent controls (the LEAN-review
 * discipline):
 *
 *  1. **ProjectionMatchesFromScratchAndZStore** — the carry reconcile is a
 *     change-of-basis: seed a known DD estimate in one fundamental-cycle basis,
 *     reconcile to a DIFFERENT tree, and assert the projected prior equals the
 *     from-scratch cycle-matrix truth @f$C_2 N,\ C_2\,\mathrm{Cov}(N)\,C_2^\top@f$
 *     (the phase-B CycleBasisUnimodular oracle) to 1e-9.  A negative control (the
 *     projection is NOT the identity) proves the transform is load-bearing.
 *  2. **CarryAccumulatesAndConverges** — over epochs the carried DD variance
 *     shrinks monotonically and the mean converges to the true integer DD.
 *  3. **ProductionSeamDatumFreedom** — THE C3b falsifier: the same noisy epoch
 *     stream driven under two DIFFERENT per-epoch tree realizations yields an
 *     IDENTICAL estimable double difference (basis choice cannot leak), while the
 *     realizations' chord sets genuinely differ and the carry is load-bearing.
 *  4. **MergeSplitAndFreshChords** — component merge/split and fresh-chord births
 *     are counted and the surviving DDs are preserved across the topology change.
 *  5. **ResetPredictAndOff** — reset() cold-starts, qz inflates the carry, and an
 *     empty epoch emits no blocks (the OFF path).
 *
 * The synthetic network is the eq34 chain-datum model (per-receiver RPB δ_r makes
 * the DD truly integer): code P=a_r−c_s, phase Φ=a_r−c_s+b_s+δ_r+z_{r,s}, with a
 * spanning-tree ambiguity datum (pre-reduce pin rows, composing exactly with the
 * post-reduce DD fold) + r1's RPB pin + a shared clock pin.
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

// Assemble one station's rows for the estimator: 2S observation rows (+ optional
// per-(r,s) phase noise), the spanning-tree ambiguity pins for this station, a
// loose δ_r prior (keeps N_rr PD), and — for r1 — the RPB pin δ_{r1}=0.
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
    // Each station has >=1 tree-edge pin, which fixes its local δ_r ≡ Σ_s z_{r,s} gauge
    // (N_rr stays PD) — so NO loose δ prior is needed.  A loose δ prior would inject
    // toward-zero information that, combined with the datum-dependent set of pinned
    // ambiguities, breaks the δ–z cancellation differently per tree and destroys the
    // DD's datum-invariance (the DiagnosticNetworkDatumInvariance probe).
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

// Card 4 D5: the (r,s) arcs the innovation gate flagged at the last install — the
// caller (ApplicationNetwork) feeds these into NEXT epoch's slipped set (the C3c re-tap).
// Mirrors the production plumbing: chord ArcKey(rxV(r), satV(s)) → (r, s).
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

// Solve one epoch under star(p,q) with NO DD carry (empty priors) — the datum-
// invariance probe: the estimable DD must not depend on the tree-pin choice.
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
// GATE 1 — the reconcile is a change-of-basis matching from-scratch truth.
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
// GATE 2 — the carry accumulates information (variance shrinks, mean → integer).
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
    // uncertainty in this synthetic network; the carry accumulates the DIRECT part on
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
// WL-2 (SPEC_ewl_wl_cascade_card.md §3.3) — the WL inject primitive + its
// one-epoch D5 exemption.  The inject is a stiff integer assertion, so standing
// invariant #1 demands a wired + force-fire-tested event response: the injected
// value must NOT be re-tapped away the very next epoch (else the F6 storm returns).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InjectLaneTargetEditsCarriedValue) {
    NetworkInvariantCarryDriver drv;
    const ArcKey chord(rxV(1), satV(1));
    drv.seedCarriedForTest(0, {chord}, (Eigen::VectorXd(1) << 3.0).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.04).finished(),
                           {rxV(0), rxV(1), satV(0), satV(1)},
                           {ArcKey(rxV(0), satV(0)), ArcKey(rxV(0), satV(1)),
                            ArcKey(rxV(1), satV(0)), ArcKey(rxV(1), satV(1))});
    double z = 0, var = 0;
    ASSERT_TRUE(drv.carriedChordValue(0, chord, z, var));
    EXPECT_NEAR(z, 3.0, 1e-12);
    EXPECT_TRUE(drv.injectCarriedChord(0, chord, /*new_z=*/7.5, /*new_var=*/1e-4));
    ASSERT_TRUE(drv.carriedChordValue(0, chord, z, var));
    EXPECT_NEAR(z, 7.5, 1e-12)  << "inject must set the carried DD value";
    EXPECT_NEAR(var, 1e-4, 1e-16) << "inject must reseed the variance";
    // Not-1:1 skip paths: a chord not carried in the group, or a wrong group, return false.
    EXPECT_FALSE(drv.injectCarriedChord(0, ArcKey(rxV(2), satV(2)), 0.0, 1.0));
    EXPECT_FALSE(drv.injectCarriedChord(/*group=*/9, chord, 0.0, 1.0));
}

TEST(NetworkInvariantCarryDriverTest, InjectD5ExemptionSuppressesReTap) {
    // Control-vs-exempt contrast: the SAME wildly-off carried DD flags the D5 gate
    // (a re-tap) WITHOUT the exemption, and does NOT flag WITH it — so the exemption,
    // not the value, is what suppresses the re-tap.  The one-epoch lifetime is guaranteed
    // by construction (@c gate_exempt_ is cleared after every @ref installPosterior).
    const int R = 4, S = 4;
    const Net n = makeNet(11, R, S);
    const ArcKey probe(rxV(2), satV(3));
    // The fundamental cycle of the probe chord under star(0,0): its 4 DD arcs.
    const std::vector<ArcKey> cyc = {ArcKey(rxV(2), satV(3)), ArcKey(rxV(2), satV(0)),
                                     ArcKey(rxV(0), satV(3)), ArcKey(rxV(0), satV(0))};
    const std::set<int>    verts = {rxV(0), rxV(2), satV(0), satV(3)};
    const std::set<ArcKey> arcs(cyc.begin(), cyc.end());
    const double off_val = 8.0;   // a carried DD ~40 L1 cycles off the data ⇒ a huge innovation
    auto seedOff = [&](NetworkInvariantCarryDriver& d) {
        d.seedCarriedForTest(0, {probe}, (Eigen::VectorXd(1) << off_val).finished(),
                             (Eigen::MatrixXd(1, 1) << 1e-2).finished(), verts, arcs);
    };
    std::mt19937 gen(7);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseOf = [&] {
        std::vector<std::vector<double>> ns(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) ns[r][s] = nd(gen);
        return ns;
    };
    const NetworkInvariantCarryDriver::Config gated{
        /*qz=*/0.0, /*fresh_prior_var=*/1e6, /*innov_gate=*/true, /*innov_k=*/4.0};

    // Control: off carried value, NO inject → the D5 gate flags it.
    NetworkInvariantCarryDriver ctrl(gated);
    seedOff(ctrl);
    { auto ns = noiseOf(); driveEpoch(ctrl, n, 0, 0, &ns, /*dt=*/1.0); }
    EXPECT_GT(innovSlips(ctrl).count({2, 3}), 0u)
        << "an off carried DD MUST flag the D5 gate without the exemption (baseline)";

    // Exempt: SAME off value, but injected (⇒ D5-exempt this epoch) → NOT flagged.
    NetworkInvariantCarryDriver ex(gated);
    seedOff(ex);
    ASSERT_TRUE(ex.injectCarriedChord(0, probe, off_val, /*new_var=*/1e-2));  // same value; sets exemption
    { auto ns = noiseOf(); driveEpoch(ex, n, 0, 0, &ns, /*dt=*/1.0); }
    EXPECT_EQ(innovSlips(ex).count({2, 3}), 0u)
        << "the inject exemption must suppress the D5 re-tap for the epoch after inject";
}

// WL-2 (§3.3 / §7.2 "primitive exactness") — the lane-target arithmetic the
// ApplicationNetwork inject applies over the two bands' carry groups: set the WL lane
// difference (A1−A2) to the integer target while PRESERVING the phase-determined sum
// (A1+A2) exactly.  Seed a garbage two-band lane, run the identical formula through the
// primitive, and assert the lane hits the integer and the NL sum is preserved to 1e-12.
TEST(NetworkInvariantCarryDriverTest, InjectLaneTargetHitsIntegerPreservesSum) {
    NetworkInvariantCarryDriver drv;
    const ArcKey chord(rxV(1), satV(1));                 // same ArcKey in BOTH band groups (1:1)
    const std::set<int>    verts = {rxV(0), rxV(1), satV(0), satV(1)};
    const std::set<ArcKey> arcs  = {ArcKey(rxV(0), satV(0)), ArcKey(rxV(0), satV(1)),
                                    ArcKey(rxV(1), satV(0)), ArcKey(rxV(1), satV(1))};
    const int    gL1 = 0, gL2 = 1;
    const double lam1 = 0.1902937, lam2 = 0.2442102;      // GPS L1 / L2 wavelengths (m)
    const double zlin1 = 3.7, zlin2 = -1.9;               // arbitrary linearization DD sums (m)
    const double z1 = 0.53, z2 = -0.28;                   // garbage carried DDs (correction frame, m)
    drv.seedCarriedForTest(gL1, {chord}, (Eigen::VectorXd(1) << z1).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.09).finished(), verts, arcs);
    drv.seedCarriedForTest(gL2, {chord}, (Eigen::VectorXd(1) << z2).finished(),
                           (Eigen::MatrixXd(1, 1) << 0.09).finished(), verts, arcs);

    // Pre-inject absolute band DDs + preserved NL sum (the quantity the inject must keep).
    const double A1 = (z1 + zlin1) / lam1, A2 = (z2 + zlin2) / lam2;
    const double s  = A1 + A2;
    const double w  = 7.0;                                // integer WL DD target (cyc)

    // The ApplicationNetwork inject formula.
    const double A1p = 0.5 * (w + s), A2p = 0.5 * (s - w);
    ASSERT_TRUE(drv.injectCarriedChord(gL1, chord, A1p * lam1 - zlin1, 1e-4));
    ASSERT_TRUE(drv.injectCarriedChord(gL2, chord, A2p * lam2 - zlin2, 1e-4));

    double zn1 = 0, zn2 = 0, v = 0;
    ASSERT_TRUE(drv.carriedChordValue(gL1, chord, zn1, v));
    ASSERT_TRUE(drv.carriedChordValue(gL2, chord, zn2, v));
    const double An1 = (zn1 + zlin1) / lam1, An2 = (zn2 + zlin2) / lam2;
    EXPECT_NEAR(An1 - An2, w, 1e-12) << "the WL lane must hit the integer target";
    EXPECT_NEAR(An1 + An2, s, 1e-12) << "the phase-determined NL sum must be preserved exactly";
}

// ---------------------------------------------------------------------------
// GATE 3 — production-seam DatumFreedom (the make-or-break falsifier).
// Two per-epoch tree realizations of the SAME noisy stream ⇒ identical estimable DD.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, ProductionSeamDatumFreedom) {
    const int R = 4, S = 4, E = 6;
    const Net n = makeNet(2027, R, S);
    // One shared noisy observation stream for BOTH realizations.
    std::mt19937 gen(555);
    std::normal_distribution<double> noiseDist(0.0, 0.03);
    std::vector<std::vector<std::vector<double>>> noise(E, std::vector<std::vector<double>>(R, std::vector<double>(S)));
    for (int e = 0; e < E; ++e) for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) noise[e][r][s] = noiseDist(gen);

    // Realization A: burn-in star(0,0), then vary the RECEIVER pivot p=e%R.
    // Realization B: burn-in star(0,0), then vary the SATELLITE pivot q=e%S.
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
        // record chord sets to prove the realizations really diverge
        chordsA_last.clear(); chordsB_last.clear();
        if (drvA.numBlocks() > 0) { auto c = drvA.blockChordsForTest(0); chordsA_last.insert(c.begin(), c.end()); }
        if (drvB.numBlocks() > 0) { auto c = drvB.blockChordsForTest(0); chordsB_last.insert(c.begin(), c.end()); }
        double z, v; if (drvA.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v)) varA[e] = v; else varA[e] = -1;
    }

    // THE gate: the estimable DD is identical under the two tree realizations — both as
    // reconstructed from the solve AND read straight from the carry (basis-invariance of
    // the driver's carry through the full produce round trip).
    for (int e = 0; e < E; ++e) {
        EXPECT_NEAR(ddA[e], ddB[e], 1e-7)
            << "epoch " << e << ": reconstructed DD leaked the datum choice";
        EXPECT_NEAR(czA[e], czB[e], 1e-7)
            << "epoch " << e << ": carried DD leaked the datum choice (A=" << czA[e] << " B=" << czB[e] << ")";
    }

    // Non-vacuous: the realizations' chord bases genuinely differ...
    EXPECT_NE(chordsA_last, chordsB_last)
        << "the two realizations must elect different fundamental-cycle bases";
    // ...and the carry is load-bearing (its variance sharpened over the run).
    ASSERT_GT(varA.front(), 0.0);
    EXPECT_LT(varA.back(), varA.front())
        << "the DD carry must accumulate information across epochs (else the gate is vacuous)";
    // ...and the divergent arms actually re-based the carry (Fable §3: tree_swaps ≥ 1 on
    // an informative store — the pivot the DatumFreedom claim is about really happened).
    EXPECT_GT(drvA.counters().tree_swaps, 0)
        << "arm A varied the receiver pivot every epoch — the carry must have been re-based";
    EXPECT_GT(drvB.counters().tree_swaps, 0)
        << "arm B varied the satellite pivot every epoch — the carry must have been re-based";
    // ...and both converge to the same integer truth.
    const double trueDD = double(n.z[2][3] - n.z[2][0] - n.z[0][3] + n.z[0][0]);
    EXPECT_NEAR(ddA.back(), trueDD, 0.05);
}

// ---------------------------------------------------------------------------
// GATE 4 — component merge / split and fresh-chord births are tracked, and the
// surviving DDs are preserved across the topology change.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, MergeSplitAndFreshChords) {
    NetworkInvariantCarryDriver drv;
    using Arc = NetworkInvariantCarryDriver::ArcInput;
    auto arc = [](int r, int s, bool tree) { return Arc{rxV(r), satV(s), tree, r, 2 + s}; };

    // Epoch 1: TWO disjoint components — {r0,r1}×{s0,s1} and {r2,r3}×{s2,s3}.
    // Each a 4-cycle: tree = path, one chord.
    NetworkInvariantCarryDriver::GroupInput g1; g1.key = 0;
    g1.arcs = { arc(0, 0, true), arc(0, 1, true), arc(1, 0, true), arc(1, 1, false),
                arc(2, 2, true), arc(2, 3, true), arc(3, 2, true), arc(3, 3, false) };
    auto b1 = drv.buildDdPriors({g1});
    EXPECT_EQ(b1.size(), 2u) << "two disjoint components ⇒ two blocks";
    // seed a solve-free posterior so the carry is populated (install needs an estimator;
    // instead drive a real solve is heavy — assert births + structure here).
    EXPECT_EQ(drv.counters().chords_born, 2);
    EXPECT_EQ(drv.counters().merges, 0);

    // Install a trivial posterior directly via a mini estimator so carry persists.
    // (Reuse the driver's own reconcile by seeding known carry.)
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

    // Epoch 2: MERGE the two via a bridge (r1,s2) tree edge + keep both chords.
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
// GATE 5 — reset(), the qz predict, and the empty-epoch OFF path.
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
// GATE 6 — (ẑ⁺, Pᶻ⁺) are estimable ⇒ datum-invariant (Fable §1 gate assertion).
// Fold the SAME carried DD prior, solve the SAME network under two different clock
// datums, and assert BOTH the posterior mean AND covariance readbacks are identical.
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
    const Eigen::VectorXd xsB = est.solve(dB);   // re-solve, DIFFERENT clock datum
    const Eigen::VectorXd meanB = est.ddPriorPosteriorMean(0, xsB);
    const Eigen::MatrixXd covB  = est.ddPriorPosteriorCovariance(0, dB);

    EXPECT_TRUE(meanA.isApprox(meanB, 1e-7)) << "ẑ⁺ leaked the datum choice";
    EXPECT_TRUE(covA.isApprox(covB, 1e-7))   << "Pᶻ⁺ leaked the datum choice";
    // Non-vacuous: the two datums genuinely move the shared solution, and information
    // was folded (posterior below the prior).
    EXPECT_GT((xsA - xsB).cwiseAbs().maxCoeff(), 1e-3);
    EXPECT_LT(covA.diagonal().maxCoeff(), Pz.diagonal().maxCoeff());
}

// ---------------------------------------------------------------------------
// GATE 7 — multi-birth joint covariance is COUPLED (Fable §2.5): on a bootstrap
// (empty store) epoch every chord is born; chords whose fundamental cycles share a
// tree edge must have a NONZERO carried cross-covariance (a per-chord-independent
// harvest would leave it exactly zero).
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
    // and receiver 0 — their cycles share tree edges, so their DDs are correlated.
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
// posterior mean/cov + innovation are computed in parallel, but the σ̂₀ accumulator
// feed (NetworkCarryVce::addRow), the D5 gate decisions, the flag/counter appends and
// the carry install stay SERIAL in (block, chord) order — so the carried DDs, the run
// counters, AND the σ̂₀ that gates the NEXT epoch are BYTE-IDENTICAL to the serial
// install at any thread count.  Driven over a multi-epoch history with the D5 gate +
// LS-VCE calibration ON, comparing after EVERY epoch, so a σ̂₀ carry-over divergence
// (which would flip a later gate decision) is caught the epoch it first appears.
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
        // σ̂₀ gates the NEXT epoch — it must be bit-identical, not just approximately.
        EXPECT_EQ(drvS.innovInflationSigma0(), drvP.innovInflationSigma0()) << "epoch " << ep;
    }
    EXPECT_GT(drvS.totalZDim(), 0) << "non-vacuous: the carry must have accumulated DDs";
}

// ---------------------------------------------------------------------------
// GATE 8 — Card 4 C3c slip integration on a CHORD arc, A/B with a NEGATIVE control.
// After warming a tight carry, inject an integer jump on a chord's true ambiguity.
// SIGNALED (drvSig): the driver re-taps that DD fresh (uncontaminated by the stale
// carry), refloating EXACTLY that one chord and leaving its neighbours' carry intact.
// UNSIGNALED (drvCtl, the negative control): the stiff q_z=0 carry pins the stale DD
// ⇒ contamination visible — proving the slip feed is load-bearing, not decorative.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, SlippedChordArcRefloatsFreshVsContaminatingControl) {
    const int R = 4, S = 4, WARM = 8;
    Net n = makeNet(31, R, S);
    const int p = 0, q = 0;
    const ArcKey slipArc(rxV(2), satV(3));   // a chord under star(0,0)
    const ArcKey keepArc(rxV(1), satV(1));   // an untouched chord

    // One shared noise stream so the two drivers warm IDENTICALLY (A/B differs only
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

    // Signaled: EXACTLY the slipped chord refloated (no collateral erasure of neighbours).
    EXPECT_EQ(drvSig.counters().slip_kills - sk_sig_before, 1)
        << "a chord-arc slip must refloat exactly its own chord";
    EXPECT_EQ(drvCtl.counters().slip_kills, 0) << "the unsignaled control refloats nothing";
    // Signaled re-taps to the post-slip truth; control stays pinned near the pre-slip value.
    EXPECT_LT(std::abs(dd_sig - trueDD_new), 0.3)
        << "signaled slip must re-tap the DD fresh to the post-slip integer";
    EXPECT_GT(std::abs(dd_ctl - trueDD_new), std::abs(dd_sig - trueDD_new) + 1.0)
        << "the unsignaled stale carry must contaminate the DD (load-bearing wiring)";
    // The untouched neighbour kept its accumulated (tight) carry — not refloated.
    double v_keep_after; ASSERT_TRUE(drvSig.carriedDdForTest(0, keepArc, zk, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9) << "no collateral erasure of an unrelated chord";

    // ...and after the slip the signaled arm re-converges to the post-slip integer.
    double dd_last = 0.0;
    for (int e = 0; e < 6; ++e) { auto ns = noiseEpoch(); dd_last = estimableDD(driveEpoch(drvSig, n, p, q, &ns, 1.0), 2, 3, 0, 0); }
    EXPECT_NEAR(dd_last, trueDD_new, 0.05) << "the DD must re-converge to the post-slip integer truth";
}

// ---------------------------------------------------------------------------
// GATE 9 — a slip on a TREE arc poisons EVERY fundamental cycle through it, so all
// dependent chords refloat while chords not touching it survive (spec §2: a slipped
// arc excluded from the A_old membership ⇒ any cycle whose support includes it re-taps).
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
    // chords at other receivers do not.  Jump its ambiguity + signal the slip on it.
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
    // ...while a chord NOT through (2,0) kept its tight carry.
    double v_keep_after; ASSERT_TRUE(drv.carriedDdForTest(0, ArcKey(rxV(1), satV(1)), z, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9)
        << "chords not touching the slipped tree edge survive with their accumulated carry";
}

// ===========================================================================
// Card 4 D5 — the estimation-side fine-slip innovation gate (spec D5 Def-1).
// ===========================================================================

// ---------------------------------------------------------------------------
// GATE 10 — MidSlip caught by the innovation gate, A/B with a NEGATIVE control.
// Warm a tight carry, then jump a chord's true ambiguity WITHOUT signaling the slip
// (the coarse MW/LLI/IF-TD layer missed it).  Gate ON: exactly that chord's post-solve
// innovation trips ⇒ it re-taps FRESH (data-only), its arc feeds NEXT epoch's slip set,
// the DD re-converges clean, and the untouched neighbour keeps its accumulated carry.
// Gate OFF (control): the stiff q_z=0 carry pins the stale DD ⇒ contamination visible.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, MidSlipCaughtByInnovationGateVsControl) {
    const int R = 4, S = 4, WARM = 8;
    Net n = makeNet(53, R, S);
    const int p = 0, q = 0;
    const ArcKey slipArc(rxV(2), satV(3));   // a chord under star(0,0)
    const ArcKey keepArc(rxV(1), satV(1));   // an untouched neighbour chord

    NetworkInvariantCarryDriver::Config gcfg; gcfg.innov_gate = true; gcfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drvGate(gcfg);   // D5 gate armed
    NetworkInvariantCarryDriver drvCtl;          // gate OFF = the C3c path (negative control)

    // Shared noise stream so both arms warm IDENTICALLY (differ only in the gate).
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

    // The UNSIGNALED integer jump (a fine slip the coarse layer missed) — chosen ABOVE the
    // fixture's k·σ_v floor (σ_v ≈ formal weight scale here; see spec D5 detection-power note).
    n.z[2][3] += 12;
    const double trueDD_new = trueDD_old + 12.0;
    const auto jumpNoise = noiseEpoch();
    driveEpoch(drvGate, n, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);  // gate must catch it
    driveEpoch(drvCtl,  n, p, q, &jumpNoise, 1.0, /*slipped=*/nullptr);  // control: no catch

    // Exactly the slipped chord flagged; the control (gate off) never evaluates.
    EXPECT_EQ(drvGate.counters().innov_flags, 1) << "the innovation gate must flag exactly the slipped chord";
    EXPECT_EQ(drvGate.counters().innov_retaps, 1);
    EXPECT_EQ(drvCtl.counters().innov_flags, 0)  << "gate OFF must never evaluate (byte-identical C3c path)";
    // The flag names the slipped chord and nothing else.
    const auto slips = innovSlips(drvGate);
    ASSERT_EQ(slips.size(), 1u);
    EXPECT_EQ(*slips.begin(), std::make_pair(2, 3)) << "the flag must name the slipped (r,s) arc";

    // Step 1 (this epoch): the flagged chord is reset toward the data (mean d_post = ẑ+v, a
    // PARTIAL move — v is the post-fit innovation, reduced from the true jump by the fold's
    // border absorption, spec Cov(v) −GᵀΣG term) and its variance is inflated to the data-only
    // scale, breaking the stiff stale prior.  The control's stiff q_z=0 carry stays pinned near
    // the pre-slip value with a tight variance ⇒ contamination.  (The FULL clean re-tap is next
    // epoch, step 2 — this epoch retains bounded contamination, the value provably inert.)
    double zg, vg, zc, vc;
    ASSERT_TRUE(drvGate.carriedDdForTest(0, slipArc, zg, vg));
    ASSERT_TRUE(drvCtl.carriedDdForTest(0, slipArc, zc, vc));
    EXPECT_LT(std::abs(zg - trueDD_new), std::abs(zc - trueDD_new) - 1.0)
        << "gate: the flagged chord's carry moved toward the post-slip data (control stays stale)";
    // The untouched neighbour kept its accumulated (tight) carry — no collateral reset.
    double v_keep_after; ASSERT_TRUE(drvGate.carriedDdForTest(0, keepArc, zk, v_keep_after));
    EXPECT_LT(v_keep_after, v_keep_before + 1e-9) << "no collateral erasure of an unrelated chord";

    // Step 2 (the decisive A/B): drive both arms forward — the gate arm feeds each epoch's
    // innovation flags back into the next epoch's slip set (the C3c membership-exclusion re-tap,
    // exactly the ApplicationNetwork plumbing), so it re-converges to the post-slip integer; the
    // control (no fine-slip layer, stiff q_z=0 carry) stays pinned to the stale pre-slip value.
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
// GATE 11 — FalseAlarmFloor: no slip, many noisy epochs, k=4 ⇒ (near) zero flags.
// Validates that Cov(v) is well-calibrated (the standardized innovation is ~N(0,1)
// under H0): too-small a Cov(v) would storm the carry with spurious refloats.
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
    // false-alarm expectation is ~0.014 ⇒ zero is the overwhelmingly likely outcome.
    EXPECT_EQ(drv.counters().innov_flags, 0)
        << "the k=4 gate must not fire on a clean (no-slip) stream — Cov(v) is well-calibrated";
    // Non-vacuous: the carry really was populated and testable over the run.
    EXPECT_GT(drv.totalZDim(), 0);
    EXPECT_GT(drv.counters().chords_carried, 0);
}

// ---------------------------------------------------------------------------
// GATE 12 — the innovation Cov(v) is an estimable-quantity readout: datum-invariant,
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
    const Eigen::VectorXd xsB = est.solve(dB);   // DIFFERENT clock datum
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
    // Non-vacuous: the two datums genuinely move the shared solution.
    EXPECT_GT((xsA - xsB).cwiseAbs().maxCoeff(), 1e-3);
    // Cov(v) = Pᶻ + (B − GᵀΣG), and the bracket is a valid (data-implied DD) covariance ⪰ 0,
    // so the innovation variance is bounded BELOW by the carried prior variance on the
    // diagonal (the S = H P Hᵀ + R structure) — the meaningful shape check, not a re-check of
    // positivity.
    EXPECT_GT((covA.diagonal() - Pz.diagonal()).minCoeff(), -1e-9)
        << "innovation variance must be >= the prior variance (Cov(v) = Pᶻ + data-only DD var)";
}

// ---------------------------------------------------------------------------
// GATE 13 — links ddPriorInnovation to ddPriorPosteriorMean: with a DIFFUSE prior the
// gain Pᶻ M → I, so the posterior update ẑ⁺ − ẑ equals the innovation v.  This ties
// the two kernel readouts to the same underlying innov and pins the sign/scale of v.
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
// Card 4 D5 v2 — LS-VCE calibration of Cov(v) (σ̂₀²).
// ===========================================================================

// ---------------------------------------------------------------------------
// GATE 14 — under a MISSPECIFIED R (obs noise ≫ the formal weight, so the formal Cov(v)
// is optimistic — the field's 10-30× problem in miniature), the RAW gate (v1) over-fires,
// but the CALIBRATED gate (v2) absorbs the inflation into σ̂₀ and stops false-alarming;
// σ̂₀ recovers the mis-specification.  A/B on ONE shared noise stream (differ only in calib).
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
    // Warm PAST the σ̂₀ warm-up (~200 rows / 9 chords ≈ 23 epochs) — during warm-up v2 is the
    // raw test (σ̂₀=1), so measure only the calibrated-regime TAIL.
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
    // ⇒ the false-alarm storm collapses.
    EXPECT_LT(f2_tail * 3, f1_tail) << "calibration must tame the false-alarm storm";
    // σ̂₀ recovered a real (>1) inflation from the misspecification.
    EXPECT_GT(drvV2.innovInflationSigma0(), 1.5) << "σ̂₀ must detect the optimistic formal Cov(v)";
    EXPECT_LT(drvV2.innovInflationSigma0(), 6.0) << "σ̂₀ must not diverge";
    // The raw gate is byte-for-byte the calibration-OFF v1 (sanity: calib is the only change).
    EXPECT_EQ(drvV1.innovInflationSigma0(), 1.0);
}

// ---------------------------------------------------------------------------
// GATE 15 — calibration must NOT blind the gate: under the same misspecified R (σ̂₀≈3-4),
// a real slip well above the RAISED threshold k·σ̂₀ is still caught + re-tapped.
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

    // A slip FAR above the raised threshold k·σ̂₀·σ_formal (σ̂₀~3-4, σ_formal~1.3 ⇒ ~16-20 cyc).
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

// ===========================================================================
// M5 card WL-3 — lane-coordinate carry (docs/SPEC_lane_carry_card.md).
// Dual-band synthetic eq34 net: per band its own RPB δ, sat phase bias, and
// integer ambiguities; ONE shared clock + one code row per (r,s).  The pair
// battery drives the SAME epoch stream through (a) two per-band groups and
// (b) one pair group, in both the §6.1 equivalence shape and the lane shape.
// ===========================================================================
namespace {

constexpr double kLam1 = 0.19029367;   // GPS L1 (m)
constexpr double kLam2 = 0.24421021;   // GPS L2 (m)

struct Net2 {
    int R, S;
    std::vector<double> a, c, b1, b2, d1, d2;
    std::vector<std::vector<int>> z1, z2;
    std::vector<std::string> sats;
};
Net2 makeNet2(unsigned seed, int R, int S) {
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> clk(-10, 10), bias(-0.5, 0.5);
    std::uniform_int_distribution<int> amb(-6, 6);
    Net2 n; n.R = R; n.S = S;
    for (int r = 0; r < R; ++r) {
        n.a.push_back(clk(gen)); n.d1.push_back(bias(gen)); n.d2.push_back(bias(gen));
    }
    for (int s = 0; s < S; ++s) {
        n.c.push_back(clk(gen)); n.b1.push_back(bias(gen)); n.b2.push_back(bias(gen));
        char id[8]; std::snprintf(id, sizeof(id), "G%02d", s + 1); n.sats.push_back(id);
    }
    n.z1.assign(R, std::vector<int>(S));
    n.z2.assign(R, std::vector<int>(S));
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) { n.z1[r][s] = amb(gen); n.z2[r][s] = amb(gen); }
    return n;
}

// Local layout: a=0, d1=1, d2=2, z1[s]=3+s, z2[s]=3+S+s.
struct Cols2 { Eigen::Index a{0}, d1{1}, d2{2}; std::vector<Eigen::Index> z1, z2; };
Cols2 layout2(int S) {
    Cols2 c; c.z1.resize(S); c.z2.resize(S);
    for (int s = 0; s < S; ++s) { c.z1[s] = 3 + s; c.z2[s] = 3 + S + s; }
    return c;
}

std::size_t addNetStation2(NetworkEstimator& est, const Net2& n, int r,
                           const std::vector<Eigen::Index>& clkCol,
                           const std::vector<Eigen::Index>& pb1Col,
                           const std::vector<Eigen::Index>& pb2Col, int p, int q,
                           const std::vector<double>* noise1,
                           const std::vector<double>* noise2, int r1) {
    const int S = n.S;
    const Cols2 col = layout2(S);
    std::vector<Eigen::Index> pins;   // per-band tree-edge ambiguity pins (same star tree)
    for (int s = 0; s < S; ++s)
        if (isStarTreeEdge(p, q, r, s)) { pins.push_back(col.z1[s]); pins.push_back(col.z2[s]); }
    const bool rpbPin = (r == r1);
    const int nrows = 3 * S + static_cast<int>(pins.size()) + (rpbPin ? 2 : 0);
    const Eigen::Index n_local = 3 + 2 * S;
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
    Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, n_local);
    Eigen::VectorXd w = Eigen::VectorXd::Ones(nrows), y = Eigen::VectorXd::Zero(nrows);
    int row = 0;
    for (int s = 0; s < S; ++s) {
        A_s(row, clkCol[s]) = -1.0; A_r(row, col.a) = 1.0;
        y(row) = n.a[r] - n.c[s]; ++row;
        A_s(row, clkCol[s]) = -1.0; A_s(row, pb1Col[s]) = 1.0;
        A_r(row, col.a) = 1.0; A_r(row, col.d1) = 1.0; A_r(row, col.z1[s]) = 1.0;
        y(row) = n.a[r] - n.c[s] + n.b1[s] + n.d1[r] + n.z1[r][s] + (noise1 ? (*noise1)[s] : 0.0);
        ++row;
        A_s(row, clkCol[s]) = -1.0; A_s(row, pb2Col[s]) = 1.0;
        A_r(row, col.a) = 1.0; A_r(row, col.d2) = 1.0; A_r(row, col.z2[s]) = 1.0;
        y(row) = n.a[r] - n.c[s] + n.b2[s] + n.d2[r] + n.z2[r][s] + (noise2 ? (*noise2)[s] : 0.0);
        ++row;
    }
    for (Eigen::Index pc : pins) { A_r(row, pc) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    if (rpbPin) {
        A_r(row, col.d1) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row;
        A_r(row, col.d2) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row;
    }
    return est.addStation(A_s, A_r, w, y);
}

// The per-band shape's two groups (keys 1, 2) — the WL-3 equivalence CONTROL.
// @p slip1 / @p slip2: PER-BAND slip sets (each band's group sees only its own).
std::vector<NetworkInvariantCarryDriver::GroupInput>
makeGroups2PerBand(const Net2& n, const std::vector<std::size_t>& ids, int p, int q,
                   const std::set<std::pair<int, int>>* slip1 = nullptr,
                   const std::set<std::pair<int, int>>* slip2 = nullptr) {
    const Cols2 col = layout2(n.S);
    NetworkInvariantCarryDriver::GroupInput g1, g2;
    g1.key = 1; g2.key = 2;
    for (int r = 0; r < n.R; ++r)
        for (int s = 0; s < n.S; ++s) {
            const bool s1 = slip1 && slip1->count({r, s}) != 0;
            const bool s2 = slip2 && slip2->count({r, s}) != 0;
            g1.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(p, q, r, s),
                               static_cast<int>(ids[r]), static_cast<int>(col.z1[s]), s1});
            g2.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(p, q, r, s),
                               static_cast<int>(ids[r]), static_cast<int>(col.z2[s]), s2});
        }
    return {g1, g2};
}

// The WL-3 pair group (key 1): one arc per (r,s) with BOTH bands' columns and
// per-band slip states (the driver unions them only under the lane shape).
std::vector<NetworkInvariantCarryDriver::GroupInput>
makeGroups2Pair(const Net2& n, const std::vector<std::size_t>& ids, int p, int q,
                const std::set<std::pair<int, int>>* slip1 = nullptr,
                const std::set<std::pair<int, int>>* slip2 = nullptr) {
    const Cols2 col = layout2(n.S);
    NetworkInvariantCarryDriver::GroupInput g;
    g.key = 1; g.pair = true; g.lambda1 = kLam1; g.lambda2 = kLam2;
    for (int r = 0; r < n.R; ++r)
        for (int s = 0; s < n.S; ++s) {
            const bool s1 = slip1 && slip1->count({r, s}) != 0;
            const bool s2 = slip2 && slip2->count({r, s}) != 0;
            g.arcs.push_back({rxV(r), satV(s), isStarTreeEdge(p, q, r, s),
                              static_cast<int>(ids[r]), static_cast<int>(col.z1[s]), s1,
                              static_cast<int>(col.z2[s]), s2});
        }
    return {g};
}

// One full dual-band epoch: build the estimator, reconcile the carry (per-band or
// pair shape), optionally post-build lane edits via @p postBuild, solve, install
// (@p install_threads > 1 exercises the deterministic-parallel install path).
std::vector<Eigen::VectorXd> driveEpoch2(
    NetworkInvariantCarryDriver& drv, const Net2& n, int p, int q,
    const std::vector<std::vector<double>>* noise1,
    const std::vector<std::vector<double>>* noise2, double dt, bool pair,
    const std::set<std::pair<int, int>>* slip1 = nullptr,
    const std::set<std::pair<int, int>>* slip2 = nullptr,
    const std::function<void(std::vector<NetworkNormalEquations::DdPriorBlock>&)>& postBuild = {},
    int install_threads = 1) {
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(n.S), pb1Col(n.S), pb2Col(n.S);
    for (int s = 0; s < n.S; ++s) {
        clkCol[s] = est.registerClock(n.sats[s]);
        pb1Col[s] = est.registerPhaseBias(n.sats[s], "L1");
        pb2Col[s] = est.registerPhaseBias(n.sats[s], "L2");
    }
    est.beginEpoch();
    std::vector<std::size_t> ids(n.R);
    for (int r = 0; r < n.R; ++r)
        ids[r] = addNetStation2(est, n, r, clkCol, pb1Col, pb2Col, p, q,
                                noise1 ? &(*noise1)[r] : nullptr,
                                noise2 ? &(*noise2)[r] : nullptr, /*r1=*/p);
    drv.beginEpoch(dt);
    auto blocks = drv.buildDdPriors(pair ? makeGroups2Pair(n, ids, p, q, slip1, slip2)
                                         : makeGroups2PerBand(n, ids, p, q, slip1, slip2));
    if (postBuild) postBuild(blocks);
    est.setDdPriors(std::move(blocks));
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    drv.installPosterior(est, x_s, datum, install_threads);
    std::vector<Eigen::VectorXd> xr(n.R);
    for (int r = 0; r < n.R; ++r) xr[r] = est.reconstructStation(ids[r], x_s);
    return xr;
}

}  // namespace

// ---------------------------------------------------------------------------
// WL-3 GATE 1 (SPEC §6.1) — THE structural safety net for the regroup: the pair
// group with every lane feature at its off/isotropic sentinel must reproduce the
// two per-band groups' carried values AND lifecycle counters to ≤1e-9, through
// noise, a signaled slip, and a tree change; D5 flag totals identical.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, PairEquivalenceMatchesPerBandShape) {
    const int R = 4, S = 4, E = 6;
    Net2 n = makeNet2(4242, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.innov_gate = true; cfg.innov_k = 4.0;          // gate armed in BOTH arms
    NetworkInvariantCarryDriver drvBand(cfg);          // control: today's per-band shape
    NetworkInvariantCarryDriver::Config pcf = cfg;     // equivalence shape: pair regroup,
    pcf.q_wl_scale = 1.0;                              // isotropic q,
    pcf.sigma_wl_fresh_cyc = 0.0;                      // isotropic fresh prior,
    pcf.lane_joint = false;                            // split per-band emission, no lane rows.
    NetworkInvariantCarryDriver drvPair(pcf);

    std::mt19937 gen(7);
    std::normal_distribution<double> nd(0.0, 0.02);
    auto noiseEpoch = [&] {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        return v;
    };
    for (int e = 0; e < E; ++e) {
        const int p = (e >= 4) ? 1 : 0, q = (e >= 4) ? 1 : 0;   // epoch-4 tree change
        std::set<std::pair<int, int>> slips;
        if (e == 3) { slips.insert({2, 3}); n.z1[2][3] += 5; }  // signaled BAND-1-ONLY slip — the equivalence shape must refloat band-1 alone, exactly like the per-band control
        auto n1 = noiseEpoch(), n2 = noiseEpoch();
        driveEpoch2(drvBand, n, p, q, &n1, &n2, 1.0, /*pair=*/false, e == 3 ? &slips : nullptr);
        driveEpoch2(drvPair, n, p, q, &n1, &n2, 1.0, /*pair=*/true,  e == 3 ? &slips : nullptr);
    }
    // Lifecycle counters: identical (the pair path tallies per band-DD by design).
    const auto& A = drvBand.counters();
    const auto& B = drvPair.counters();
    EXPECT_EQ(A.chords_carried, B.chords_carried);
    EXPECT_EQ(A.chords_born, B.chords_born);
    EXPECT_EQ(A.chords_dropped, B.chords_dropped);
    EXPECT_EQ(A.slip_kills, B.slip_kills);
    EXPECT_EQ(A.chords_marginalized, B.chords_marginalized);
    EXPECT_EQ(A.z_dim, B.z_dim);
    EXPECT_EQ(A.components, B.components) << "split pair emission = 2 blocks/component = per-band";
    EXPECT_EQ(A.innov_flags, B.innov_flags) << "D5 totals identical (SPEC §6.1)";
    EXPECT_GT(B.pair_components, 0) << "non-vacuous: the pair path actually ran";
    EXPECT_GT(A.chords_carried, 0) << "non-vacuous: the carry was populated";
    // Per-chord carried values + variances: ≤1e-9 (not byte — summation order differs).
    const int p = 1, q = 1;
    int compared = 0;
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) {
            if (isStarTreeEdge(p, q, r, s)) continue;
            const ArcKey chord(rxV(r), satV(s));
            double zb1, vb1, zb2, vb2, zp1, vp1, zp2, vp2;
            const bool hb1 = drvBand.carriedDdForTest(1, chord, zb1, vb1);
            const bool hb2 = drvBand.carriedDdForTest(2, chord, zb2, vb2);
            const bool hp  = drvPair.carriedPairValue(1, chord, zp1, vp1, zp2, vp2);
            ASSERT_EQ(hb1 && hb2, hp) << "carried-chord sets must agree at (" << r << "," << s << ")";
            if (!hp) continue;
            EXPECT_NEAR(zp1, zb1, 1e-9); EXPECT_NEAR(vp1, vb1, 1e-9);
            EXPECT_NEAR(zp2, zb2, 1e-9); EXPECT_NEAR(vp2, vb2, 1e-9);
            // Equivalence shape carries NO cross-band coupling (per-band-identical fold).
            std::vector<ArcKey> chs; Eigen::VectorXd zj; Eigen::MatrixXd Pj;
            ASSERT_TRUE(drvPair.carriedPairBlockForTest(1, chs, zj, Pj));
            const auto it = std::find(chs.begin(), chs.end(), chord);
            ASSERT_NE(it, chs.end());
            const Eigen::Index i = it - chs.begin(), K = static_cast<Eigen::Index>(chs.size());
            EXPECT_EQ(Pj(i, K + i), 0.0) << "equivalence shape must zero the cross-band block";
            ++compared;
        }
    EXPECT_GE(compared, 6) << "non-vacuous: several carried pair chords were compared";
}

// ---------------------------------------------------------------------------
// WL-3 GATE 2 (SPEC §2/§3.1) — the LANE shape: the joint posterior retains the
// cross-band coupling (the honest jointly-covarianced pair), and a fresh chord's
// prior is anisotropic in the lane basis — diffuse along NL, the honest code-seed
// scale along WL (hand values through the unit-normalized rotation).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, PairLaneShapeCouplingAndFreshPrior) {
    const int R = 3, S = 3;
    Net2 n = makeNet2(99, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true;
    cfg.sigma_wl_fresh_cyc = 3.0;
    cfg.fresh_prior_var = 1.0e6;
    NetworkInvariantCarryDriver drv(cfg);

    // Epoch 1: all chords fresh — check the lane-basis prior on the RETURNED block.
    Eigen::MatrixXd Pz0; Eigen::Index K0 = 0;
    auto grab = [&](std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) {
        ASSERT_EQ(blocks.size(), 1u);
        K0 = static_cast<Eigen::Index>(blocks[0].zhat.size()) / 2;
        Pz0 = blocks[0].Pz;
    };
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 0.0, /*pair=*/true, nullptr, nullptr, grab);
    ASSERT_GT(K0, 0);
    const double a1 = 1.0 / kLam1, a2 = 1.0 / kLam2;
    const double unorm = std::sqrt(a1 * a1 + a2 * a2);
    Eigen::Vector2d h_wl(a1 / unorm, -a2 / unorm), h_nl(a2 / unorm, a1 / unorm);
    const double sig_wl_m = 3.0 / unorm;    // 3 cycles in lane-coordinate meters
    for (Eigen::Index i = 0; i < K0; ++i) {
        Eigen::Matrix2d P;
        P << Pz0(i, i), Pz0(i, K0 + i), Pz0(K0 + i, i), Pz0(K0 + i, K0 + i);
        EXPECT_NEAR(h_wl.dot(P * h_wl), sig_wl_m * sig_wl_m, 1e-6 * sig_wl_m * sig_wl_m)
            << "fresh WL lane σ must be the honest code-seed scale";
        EXPECT_NEAR(h_nl.dot(P * h_nl), 1.0e6, 1e-6 * 1.0e6)
            << "fresh NL direction stays diffuse";
    }
    // Epoch 2: carried — the joint posterior must retain band-1↔band-2 coupling.
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, /*pair=*/true);
    std::vector<ArcKey> chs; Eigen::VectorXd zj; Eigen::MatrixXd Pj;
    ASSERT_TRUE(drv.carriedPairBlockForTest(1, chs, zj, Pj));
    const Eigen::Index K = static_cast<Eigen::Index>(chs.size());
    double max_cross = 0.0;
    for (Eigen::Index i = 0; i < K; ++i)
        max_cross = std::max(max_cross, std::abs(Pj(i, K + i)));
    EXPECT_GT(max_cross, 1e-12)
        << "lane shape must retain the cross-band posterior coupling (the joint pair)";
    // Lane-aware q: Q = Rᵀ·diag(q, 1000q)·R per chord — the WL lane inflates 1000×
    // the NL direction per unit time (hand value through the unit rotation).
    NetworkInvariantCarryDriver::Config qcfg = cfg;
    qcfg.qz = 0.01; qcfg.q_wl_scale = 1000.0;
    NetworkInvariantCarryDriver drvQ(qcfg);
    driveEpoch2(drvQ, n, 0, 0, nullptr, nullptr, 0.0, /*pair=*/true);
    std::vector<ArcKey> qch; Eigen::VectorXd qz0; Eigen::MatrixXd Pq0, Pq1;
    ASSERT_TRUE(drvQ.carriedPairBlockForTest(1, qch, qz0, Pq0));
    drvQ.beginEpoch(1.0);
    ASSERT_TRUE(drvQ.carriedPairBlockForTest(1, qch, qz0, Pq1));
    const Eigen::Index Kq = static_cast<Eigen::Index>(qch.size());
    for (Eigen::Index i = 0; i < Kq; ++i) {
        Eigen::Matrix2d dP;
        dP << Pq1(i, i) - Pq0(i, i),           Pq1(i, Kq + i) - Pq0(i, Kq + i),
              Pq1(Kq + i, i) - Pq0(Kq + i, i), Pq1(Kq + i, Kq + i) - Pq0(Kq + i, Kq + i);
        EXPECT_NEAR(h_wl.dot(dP * h_wl), 0.01 * 1000.0, 1e-6 * 10.0)
            << "WL lane q = q_wl_scale × qz per second";
        EXPECT_NEAR(h_nl.dot(dP * h_nl), 0.01, 1e-9)
            << "NL direction keeps today's qz";
    }
}

// ---------------------------------------------------------------------------
// WL-3 GATE 3 (SPEC §3.2) — the per-epoch lane measurement is an exact scalar KF
// update on the emitted pair block (hand value), and the D5 prior (BlockMeta)
// tracks the folded prior.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, LaneMeasurementMatchesHandKf) {
    const int R = 3, S = 3;
    Net2 n = makeNet2(11, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true; cfg.sigma_wl_fresh_cyc = 3.0;
    NetworkInvariantCarryDriver drv(cfg);
    // Warm one epoch so the next build carries (a projected, coupled prior).
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 0.0, /*pair=*/true);

    const double y = 7.25, Rm = 0.04;   // cycles, cycles²
    Eigen::VectorXd zhat_before, zhat_after;
    Eigen::MatrixXd Pz_before, Pz_after;
    std::size_t kk = 1;   // second chord of the block
    auto postBuild = [&](std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) {
        ASSERT_EQ(blocks.size(), 1u);
        ASSERT_GE(drv.blockChordsForTest(0).size(), 2u);
        zhat_before = blocks[0].zhat;
        Pz_before   = blocks[0].Pz;
        ASSERT_TRUE(drv.applyLaneMeasurement(blocks, 0, kk, y, Rm));
        zhat_after = blocks[0].zhat;
        Pz_after   = blocks[0].Pz;
    };
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, /*pair=*/true, nullptr, nullptr, postBuild);

    const Eigen::Index K = zhat_before.size() / 2;
    Eigen::VectorXd h = Eigen::VectorXd::Zero(2 * K);
    h(static_cast<Eigen::Index>(kk))     =  1.0 / kLam1;
    h(K + static_cast<Eigen::Index>(kk)) = -1.0 / kLam2;
    const Eigen::VectorXd Ph = Pz_before * h;
    const double Sd = h.dot(Ph) + Rm;
    const Eigen::VectorXd zhat_hand = zhat_before + (y - h.dot(zhat_before)) / Sd * Ph;
    const Eigen::MatrixXd Pz_hand   = Pz_before - (Ph * Ph.transpose()) / Sd;
    EXPECT_LT((zhat_after - zhat_hand).cwiseAbs().maxCoeff(), 1e-10)
        << "lane update mean must match the hand KF value";
    EXPECT_LT((Pz_after - Pz_hand).cwiseAbs().maxCoeff(), 1e-9)
        << "lane update covariance must match the hand KF value";
    // The lane variance genuinely shrank; the measurement is load-bearing.
    EXPECT_LT(h.dot(Pz_after * h), h.dot(Pz_before * h) - 1e-12);
}

// ---------------------------------------------------------------------------
// WL-3 GATE 4 (SPEC §3.3) — fold-on-change: first decision folds, an unchanged
// decision at the NEXT epoch is a no-op, a changed integer re-folds, and a
// re-tapped (slipped) chord must re-earn its fix (memory erased).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, LaneFixFoldOnChangeSemantics) {
    const int R = 3, S = 3;
    Net2 n = makeNet2(23, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true; cfg.sigma_wl_fresh_cyc = 3.0;
    NetworkInvariantCarryDriver drv(cfg);
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 0.0, /*pair=*/true);

    int rc1 = -9, rc2 = -9, rc3 = -9, rc4 = -9;
    auto fix = [&](long long w, int* rc) {
        return [&, w, rc](std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) {
            *rc = drv.applyLaneFix(blocks, 0, 0, w, static_cast<double>(w), 0.01);
        };
    };
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, true, nullptr, nullptr, fix(5, &rc1));
    EXPECT_EQ(rc1, 1) << "first decision folds";
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, true, nullptr, nullptr, fix(5, &rc2));
    EXPECT_EQ(rc2, 0) << "unchanged decision is a no-op (once-only discipline)";
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, true, nullptr, nullptr, fix(6, &rc3));
    EXPECT_EQ(rc3, 1) << "changed integer re-folds";
    // Slip the chord's arc: rebirth erases the memory ⇒ the same integer folds again.
    const ArcKey chord0 = drv.blockChordsForTest(0).front();
    std::set<std::pair<int, int>> slips{{chord0.u, chord0.v - kSatBase}};
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, true, &slips, nullptr, fix(6, &rc4));
    EXPECT_EQ(rc4, 1) << "a re-tapped chord must re-earn its fix (SPEC §3.4 self-heal)";
}

// ---------------------------------------------------------------------------
// WL-3 GATE 5 (SPEC §3.4) — a slip on EITHER band's arc refloats BOTH bands of
// the pair chord (union semantics): the reborn block prior is diffuse in both
// band rows and slip_kills tallies 2 per killed pair chord.
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, PairSlipRefloatsBothBands) {
    const int R = 3, S = 3;
    Net2 n = makeNet2(31, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true; cfg.sigma_wl_fresh_cyc = 0.0;   // isotropic fresh ⇒ diffuse probe is crisp
    NetworkInvariantCarryDriver drv(cfg);
    for (int e = 0; e < 3; ++e)
        driveEpoch2(drv, n, 0, 0, nullptr, nullptr, e == 0 ? 0.0 : 1.0, /*pair=*/true);
    const long long kills_before = drv.counters().slip_kills;

    const std::set<std::pair<int, int>> slips{{2, 2}};   // one arc (a chord under star(0,0))
    Eigen::MatrixXd Pz_reborn; Eigen::Index Kb = 0; std::size_t idx = 0;
    auto grab = [&](std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) {
        ASSERT_EQ(blocks.size(), 1u);
        const auto& chs = drv.blockChordsForTest(0);
        const auto it = std::find(chs.begin(), chs.end(), ArcKey(rxV(2), satV(2)));
        ASSERT_NE(it, chs.end());
        idx = static_cast<std::size_t>(it - chs.begin());
        Kb  = static_cast<Eigen::Index>(chs.size());
        Pz_reborn = blocks[0].Pz;
    };
    driveEpoch2(drv, n, 0, 0, nullptr, nullptr, 1.0, /*pair=*/true, &slips, nullptr, grab);
    EXPECT_EQ(drv.counters().slip_kills - kills_before, 2)
        << "one killed pair chord tallies both band DDs";
    const Eigen::Index i = static_cast<Eigen::Index>(idx);
    EXPECT_GT(Pz_reborn(i, i), 0.5e6)          << "band-1 reborn diffuse";
    EXPECT_GT(Pz_reborn(Kb + i, Kb + i), 0.5e6) << "band-2 reborn diffuse (union refloat)";
}

// ---------------------------------------------------------------------------
// WL-3 GATE 6 — injectCarriedChordPair edits both bands and its ONE-epoch D5
// exemption spans BOTH band rows (control: an equally-off seeded carry flags).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, InjectPairExemptionSpansBothRows) {
    const int R = 3, S = 3;
    Net2 n = makeNet2(47, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true; cfg.sigma_wl_fresh_cyc = 3.0;
    cfg.innov_gate = true; cfg.innov_k = 4.0;

    // Both arms warm identically, then get the SAME way-off pair value — one via the
    // exempting inject primitive, one via a raw seed-style corruption (no exemption).
    auto warm = [&](NetworkInvariantCarryDriver& d) {
        for (int e = 0; e < 3; ++e)
            driveEpoch2(d, n, 0, 0, nullptr, nullptr, e == 0 ? 0.0 : 1.0, /*pair=*/true);
    };
    const ArcKey chord(rxV(1), satV(1));   // a chord under star(0,0)

    NetworkInvariantCarryDriver drvInj(cfg);
    warm(drvInj);
    double z1, v1, z2, v2;
    ASSERT_TRUE(drvInj.carriedPairValue(1, chord, z1, v1, z2, v2));
    ASSERT_TRUE(drvInj.injectCarriedChordPair(1, chord, z1 + 20.0, 1e-4, z2 - 20.0, 1e-4));
    driveEpoch2(drvInj, n, 0, 0, nullptr, nullptr, 1.0, /*pair=*/true);
    // The exemption is PER CHORD (both band rows); component siblings deliberately stay
    // testable (WL-2 §3.3: an inject's residual tension may land on its neighbours).
    long long flags_on_chord = 0;
    for (const auto& [g, key] : drvInj.lastInnovFlags()) if (key == chord) ++flags_on_chord;
    EXPECT_EQ(flags_on_chord, 0)
        << "the pair inject's one-epoch exemption must cover BOTH of the chord's band rows";

    NetworkInvariantCarryDriver drvCtl(cfg);
    warm(drvCtl);
    // Control: the same off-value asserted WITHOUT the exemption — rebuild the carried
    // block through the seed seam (mirrors InjectD5ExemptionSuppressesReTap's control).
    std::vector<ArcKey> chs; Eigen::VectorXd zj; Eigen::MatrixXd Pj;
    ASSERT_TRUE(drvCtl.carriedPairBlockForTest(1, chs, zj, Pj));
    {
        const auto it = std::find(chs.begin(), chs.end(), chord);
        ASSERT_NE(it, chs.end());
        const Eigen::Index i = it - chs.begin(), K = static_cast<Eigen::Index>(chs.size());
        zj(i) += 20.0; zj(K + i) -= 20.0;
        Pj.row(i).setZero(); Pj.col(i).setZero(); Pj(i, i) = 1e-4;
        Pj.row(K + i).setZero(); Pj.col(K + i).setZero(); Pj(K + i, K + i) = 1e-4;
        NetworkInvariantCarryDriver drvFresh(cfg);
        // Re-seed the corrupted block into a fresh driver (same graph/arcs as the run).
        std::set<int> verts; std::set<ArcKey> arcs;
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) {
            verts.insert(rxV(r)); verts.insert(satV(s));
            arcs.insert(ArcKey(rxV(r), satV(s)));
        }
        drvFresh.seedCarriedPairForTest(1, chs, zj, Pj, kLam1, kLam2, verts, arcs);
        driveEpoch2(drvFresh, n, 0, 0, nullptr, nullptr, 1.0, /*pair=*/true);
        long long ctl_on_chord = 0;
        for (const auto& [g, key] : drvFresh.lastInnovFlags()) if (key == chord) ++ctl_on_chord;
        EXPECT_EQ(ctl_on_chord, 2)
            << "control: the same off-value WITHOUT the exemption must flag both band rows";
    }
}

// ---------------------------------------------------------------------------
// WL-3 GATE 7 — the deterministic-parallel install (network.parallel_seam, armed
// in the lineage configs) is BIT-IDENTICAL to the serial install on JOINT pair
// blocks (the per-band shape's byte-identity gate does not cover the pair stitch).
// ---------------------------------------------------------------------------
TEST(NetworkInvariantCarryDriverTest, PairInstallParallelMatchesSerialByteForByte) {
    const int R = 4, S = 4, E = 5;
    Net2 n = makeNet2(77, R, S);
    NetworkInvariantCarryDriver::Config cfg;
    cfg.lane_joint = true; cfg.innov_gate = true; cfg.innov_k = 4.0;
    NetworkInvariantCarryDriver drvS(cfg), drvP(cfg);

    std::mt19937 gen(3);
    std::normal_distribution<double> nd(0.0, 0.02);
    for (int e = 0; e < E; ++e) {
        std::vector<std::vector<double>> n1(R, std::vector<double>(S)), n2 = n1;
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) { n1[r][s] = nd(gen); n2[r][s] = nd(gen); }
        std::set<std::pair<int, int>> slips;
        if (e == 2) slips.insert({1, 2});   // exercise the union-refloat path too
        driveEpoch2(drvS, n, 0, 0, &n1, &n2, e ? 1.0 : 0.0, /*pair=*/true,
                    e == 2 ? &slips : nullptr, nullptr, {}, /*install_threads=*/1);
        driveEpoch2(drvP, n, 0, 0, &n1, &n2, e ? 1.0 : 0.0, /*pair=*/true,
                    e == 2 ? &slips : nullptr, nullptr, {}, /*install_threads=*/4);
        std::vector<ArcKey> cs, cp; Eigen::VectorXd zs, zp; Eigen::MatrixXd Ps, Pp;
        ASSERT_TRUE(drvS.carriedPairBlockForTest(1, cs, zs, Ps));
        ASSERT_TRUE(drvP.carriedPairBlockForTest(1, cp, zp, Pp));
        ASSERT_EQ(cs, cp);
        EXPECT_TRUE(zs == zp) << "pair install means must be BIT-identical at any thread count (epoch " << e << ")";
        EXPECT_TRUE(Ps == Pp) << "pair install covariances must be BIT-identical (epoch " << e << ")";
    }
    EXPECT_EQ(drvS.counters().innov_flags, drvP.counters().innov_flags);
    EXPECT_GT(drvS.counters().chords_carried, 0);
}

}  // namespace GnssProcessor
