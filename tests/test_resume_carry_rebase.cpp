/**
 * @file test_resume_carry_rebase.cpp
 * @brief The linearization frame of the carry blob (RS-2,
 *        docs/SPEC_event_harness.md §21.4): @c network.state_snapshot_carry_lin
 *        (the @c ZL row) and @c network.resume_carry_rebase (the re-base at the
 *        first post-resume build).
 *
 * ## The case this file pins
 *
 * @ref GnssProcessor::NetworkInvariantCarryDriver stores each chord's mean in the
 * correction frame: @f$\hat z=\sum c\,x_r=\lambda N-\sum c\,x_{\rm lin}@f$.  A resumed
 * run re-seeds every station ambiguity @c x_lin from code at its first epoch (the
 * station layer is not snapshotted, by design), so a restored @f$\hat z@f$ is off by
 * @f$\sum c\,(x_{\rm lin,writer}-x_{\rm lin,now})@f$.  That is metres per chord,
 * folded at the carry's stiffness on the whole branch at once; the D5 gate runs
 * post-solve and can only flag the result.  On the field golden's 60-epoch null legs
 * the first post-resume solve discharges through the `dd_fold` prior class and the
 * SH health gate then withholds every epoch.
 *
 * The repair is a frame translation in two halves, both off by default: the writer
 * stores @f$z_\ell=\sum c\,x_{\rm lin}@f$ per chord (one @c ZL row per block, beside
 * @c Z), and the first DD prior build after a restore adds
 * @f$W z_{\ell,\rm writer}-z_{\ell,\rm now}@f$ to the projected prior means, with
 * @f$W@f$ the cycle-basis projection the build already forms.  @f$P^z@f$ is untouched.
 *
 * ## The cases
 *
 * 1. **RoundTripRebasesToTheSameAbsoluteDd**: the main case.  Two blocks are
 *    harvested against a known @c x_lin, serialized with the @c ZL rows, restored,
 *    and re-based against a shifted @c x_lin.  The shift is placed on one arc, so the
 *    predicted correction is exact and per chord (a chord whose fundamental cycle
 *    misses that arc does not move).  A uniform shift would cancel through
 *    @f$\sum c=0@f$ and would pass even if the re-base did nothing.  @f$P^z@f$ is
 *    asserted byte-equal, and an unshifted control asserts that the re-base is the
 *    identity when the two frames agree.
 * 2. **UnbasableContributorIsMarginalizedNotFolded**: the invariant-#7 edge.  A
 *    restored block that is marked unbased but carries no @c ZL row cannot be
 *    re-based; its chords take the ordinary birth path (diffuse prior, zero prior
 *    mean) instead of folding the writer's frame, and the drop is counted.  Without
 *    the count the behaviour cannot be told apart from a chord that had no
 *    predecessor.
 * 3. **LegacyBlobRestoresUntouchedAndRebasesNothing**: the format-forward contract.
 *    A blob with no @c ZL rows (any snapshot written without the write knob)
 *    restores the stored means unchanged, reports zero re-basable blocks, and
 *    re-bases nothing even with the read knob armed.  The run-end line exists to
 *    make this armed-but-inert case visible (standing invariant #4).
 * 4. **WriteKnobOffEmitsNoZlRow**: the byte-identity half.  With the write knob off
 *    the blob carries no @c ZL row, so a snapshot does not change.
 * 5. **DiagnosticOnlyWindowCountsTheTripButKeepsTheCarry**: the
 *    @c network.resume_d5_warmup_ep window (RS-2b, §21.6 of the spec).  Three arms on one noise
 *    stream (gate off, gate on, gate on and diagnostic-only) are warmed together and
 *    then given the same unsignalled 12-cycle jump.  The main assertion is that the
 *    suppressed arm's carried DD equals the gate-off arm's bit for bit; a diagnostic
 *    gate that still nudged the posterior would pass any looser test.  Also pinned:
 *    the trip is still reported (flags 1) and counted (@c d5_warmup_suppressed 1)
 *    with @c innov_retaps 0;
 *    @ref GnssProcessor::NetworkInvariantCarryDriver::lastInnovWarmupSuppressed marks
 *    it so the caller can keep it out of the slip funnel; the control arm differs
 *    (the test is not vacuous); and an untouched neighbour chord is identical
 *    everywhere, so the flag is not a global mute.
 *
 * Out of scope: the estimator-side fold itself (tests/test_network_invariant_carry_driver.cpp
 * and tests/test_pd_safe_fold.cpp cover it), and the end-to-end resume, which is a
 * harness leg pair: the P1/P2/P3 gates of §21.4 and the window legs of §21.6.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"

namespace GnssProcessor {
namespace {

constexpr int kSatBase = 1000;   // satellite vertex ids live above every receiver id
int rxV(int r) { return r; }
int satV(int s) { return kSatBase + s; }

/// The full bipartite R×S arc set, in a deterministic order.
std::vector<ArcKey> allArcs(int R, int S) {
    std::vector<ArcKey> a;
    for (int r = 0; r < R; ++r)
        for (int s = 0; s < S; ++s) a.emplace_back(rxV(r), satV(s));
    return a;
}

/// Star spanning tree pivoted on (rx 0, sat 0): {(0,s) ∀s} ∪ {(r,0) ∀r≠0}.
bool isTreeEdge(int r, int s) { return r == 0 || s == 0; }

/// Chords of that tree, in the same order the driver will emit them (ArcKey order).
std::vector<ArcKey> chordsOf(int R, int S) {
    std::vector<ArcKey> c;
    for (int r = 1; r < R; ++r)
        for (int s = 1; s < S; ++s) c.emplace_back(rxV(r), satV(s));
    return c;
}

/// One group's arcs for @ref NetworkInvariantCarryDriver::buildDdPriors.  Every arc
/// resolves to a live (station, column) so every chord is foldable; @p lin gives the
/// per-arc linearization value the RS-2 machinery reads.
NetworkInvariantCarryDriver::GroupInput makeGroup(
    int key, int R, int S, const std::map<ArcKey, double>& lin) {
    NetworkInvariantCarryDriver::GroupInput g;
    g.key = key;
    int col = 0;
    for (int r = 0; r < R; ++r) {
        for (int s = 0; s < S; ++s) {
            NetworkInvariantCarryDriver::ArcInput a;
            a.rx_vertex   = rxV(r);
            a.sat_vertex  = satV(s);
            a.tree_edge   = isTreeEdge(r, s);
            a.est_station = r;
            a.local_col   = col++;
            const auto it = lin.find(ArcKey(rxV(r), satV(s)));
            a.x_lin = (it == lin.end()) ? 0.0 : it->second;
            g.arcs.push_back(a);
        }
    }
    return g;
}

/// A per-arc linearization map with a constant base value.
std::map<ArcKey, double> linMap(int R, int S, double base) {
    std::map<ArcKey, double> m;
    for (const ArcKey& a : allArcs(R, S)) m[a] = base;
    return m;
}

NetworkInvariantCarryDriver::Config rs2Cfg(bool write, bool read) {
    NetworkInvariantCarryDriver::Config c;
    c.qz = 0.0;                 // constant DDs: no time update to confound the frame test
    c.carry_lin = write;
    c.carry_rebase = read;
    return c;
}

/// Seed one component of @p group with means @p z (chord order = @ref chordsOf) and a
/// diagonal @f$P^z@f$, then attach @p zl as its linearization DD.
void seedBlock(NetworkInvariantCarryDriver& d, int group, int R, int S,
               const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz,
               const Eigen::VectorXd* zl) {
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    const std::vector<ArcKey> arcs_v = allArcs(R, S);
    d.seedCarriedForTest(group, chordsOf(R, S), z, Pz,
                         verts, std::set<ArcKey>(arcs_v.begin(), arcs_v.end()));
    if (zl) d.seedCarriedLinForTest(group, *zl, /*unbased=*/false);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1.  The round trip: harvested at x_lin = A, restored, re-based at A + δ, and the
//     absolute DD (ẑ + zl) is unchanged.  δ is per arc, so the correction is
//     chord-specific and a do-nothing implementation cannot pass.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, RoundTripRebasesToTheSameAbsoluteDd) {
    constexpr int R = 2, S = 3;                 // 2 chords: (1,1) and (1,2)
    const std::vector<ArcKey> ch = chordsOf(R, S);
    ASSERT_EQ(ch.size(), 2u);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    const double kBase = 4.0;                   // the writer's x_lin on every arc
    const double kDelta = 1.75;                 // the shift, on one arc only

    // --- writer: two blocks (two groups), means and Pᶻ known, ZL captured ------
    Eigen::VectorXd z0(K);  z0 << 0.25, -0.75;
    Eigen::VectorXd z1(K);  z1 << 1.50,  2.25;
    Eigen::MatrixXd Pz0 = Eigen::MatrixXd::Zero(K, K);
    Pz0.diagonal() << 0.04, 0.09;
    Pz0(0, 1) = Pz0(1, 0) = 0.01;               // a real off-diagonal: Pᶻ must survive verbatim
    const Eigen::MatrixXd Pz1 = 2.0 * Pz0;

    // zl over the writer's frame: every arc at kBase ⇒ Σ c·x_lin = 0 (Σ c = 0 on a DD
    // cycle).  That is the correct writer value here, and it is why the reader's shift
    // has to be non-uniform for this test to discriminate.
    const Eigen::VectorXd zl_w = Eigen::VectorXd::Zero(K);

    NetworkInvariantCarryDriver src(rs2Cfg(/*write=*/true, /*read=*/false));
    seedBlock(src, 0, R, S, z0, Pz0, &zl_w);
    seedBlock(src, 7, R, S, z1, Pz1, &zl_w);

    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_NE(blob.find("\nZL,"), std::string::npos)
        << "the write knob is armed but no ZL row was emitted";

    // --- reader: restore, then build against a shifted linearization -----------
    NetworkInvariantCarryDriver dst(rs2Cfg(/*write=*/false, /*read=*/true));
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << err;
    EXPECT_EQ(dst.resumeCarryLinBlocks(), 2);

    // δ on the chord arc (1,1) only.  Its own fundamental cycle carries it at +1
    // (ZComponent::fundamentalCycle seeds the chord in reference orientation), and the
    // cycle of (1,2) does not contain it at all, so the prediction is
    //     chord (1,1): ẑ -= δ        chord (1,2): unchanged.
    std::map<ArcKey, double> lin_now = linMap(R, S, kBase);
    lin_now[ArcKey(rxV(1), satV(1))] = kBase + kDelta;

    std::vector<NetworkInvariantCarryDriver::GroupInput> groups{
        makeGroup(0, R, S, lin_now), makeGroup(7, R, S, lin_now)};
    const auto blocks = dst.buildDdPriors(groups);
    ASSERT_EQ(blocks.size(), 2u);

    const auto& cc = dst.counters();
    EXPECT_EQ(cc.resume_rebased, 4);            // 2 chords × 2 blocks
    EXPECT_EQ(cc.resume_dropped_unbased, 0);
    EXPECT_NEAR(cc.resume_rebase_max_dz, kDelta, 1e-12);

    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const Eigen::VectorXd& want = (b == 0) ? z0 : z1;
        const Eigen::MatrixXd& wantP = (b == 0) ? Pz0 : Pz1;
        ASSERT_EQ(blocks[b].zhat.size(), K);
        EXPECT_NEAR(blocks[b].zhat(0), want(0) - kDelta, 1e-12)
            << "block " << b << ": the shifted arc's chord was not re-based";
        EXPECT_NEAR(blocks[b].zhat(1), want(1), 1e-12)
            << "block " << b << ": a chord whose cycle misses the shifted arc moved";
        // Pᶻ is invariant under a frame translation, and the projection here is the
        // identity (the tree and chord set are unchanged), so Pᶻ comes through
        // verbatim, off-diagonal included.
        for (Eigen::Index i = 0; i < K; ++i)
            for (Eigen::Index j = 0; j < K; ++j)
                EXPECT_DOUBLE_EQ(blocks[b].Pz(i, j), wantP(i, j))
                    << "block " << b << " Pz(" << i << "," << j << ") moved";
    }

    // Control: the same restore built against the writer's own linearization is the
    // identity.  A re-base that shifts an already-correct frame is as wrong as one that
    // does not shift a stale one.
    NetworkInvariantCarryDriver ctl(rs2Cfg(false, true));
    std::istringstream is2(blob);
    ASSERT_TRUE(ctl.restoreCarry(is2, &err)) << err;
    std::vector<NetworkInvariantCarryDriver::GroupInput> g_same{
        makeGroup(0, R, S, linMap(R, S, kBase)), makeGroup(7, R, S, linMap(R, S, kBase))};
    const auto ctl_blocks = ctl.buildDdPriors(g_same);
    ASSERT_EQ(ctl_blocks.size(), 2u);
    for (Eigen::Index i = 0; i < K; ++i) {
        EXPECT_DOUBLE_EQ(ctl_blocks[0].zhat(i), z0(i));
        EXPECT_DOUBLE_EQ(ctl_blocks[1].zhat(i), z1(i));
    }
}

// ---------------------------------------------------------------------------
// 2.  A restored block that is marked unbased but carries no ZL row cannot be
//     re-based.  Its chords take the birth path (prior mean 0, diffuse Pᶻ) rather
//     than fold the writer's frame, and the drop is counted.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, UnbasableContributorIsMarginalizedNotFolded) {
    constexpr int R = 2, S = 3;
    const std::vector<ArcKey> ch = chordsOf(R, S);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    Eigen::VectorXd z(K);  z << 0.25, -0.75;
    Eigen::MatrixXd Pz = Eigen::MatrixXd::Zero(K, K);
    Pz.diagonal() << 0.04, 0.09;

    NetworkInvariantCarryDriver d(rs2Cfg(/*write=*/false, /*read=*/true));
    // Unbased with no zl: the state a reader would be in if the blob's frame were
    // asserted but its rows were missing.  seedCarriedLinForTest with an empty vector is
    // the only way to reach it from a test.  restoreCarry does not produce it (it sets
    // unbased only when it parsed a full ZL row); the guard covers any other path.
    std::set<int> verts;
    for (int r = 0; r < R; ++r) verts.insert(rxV(r));
    for (int s = 0; s < S; ++s) verts.insert(satV(s));
    const std::vector<ArcKey> arcs_v = allArcs(R, S);
    d.seedCarriedForTest(0, ch, z, Pz, verts,
                         std::set<ArcKey>(arcs_v.begin(), arcs_v.end()));
    d.seedCarriedLinForTest(0, Eigen::VectorXd(), /*unbased=*/true);

    std::vector<NetworkInvariantCarryDriver::GroupInput> groups{
        makeGroup(0, R, S, linMap(R, S, 4.0))};
    const auto blocks = d.buildDdPriors(groups);
    ASSERT_EQ(blocks.size(), 1u);

    const auto& cc = d.counters();
    EXPECT_EQ(cc.resume_rebased, 0);
    EXPECT_EQ(cc.resume_dropped_unbased, K)
        << "the un-re-basable chords were not counted as marginalized";
    EXPECT_EQ(cc.chords_born, K) << "they must take the ordinary birth path";
    EXPECT_EQ(cc.chords_carried, 0);
    for (Eigen::Index i = 0; i < K; ++i) {
        EXPECT_DOUBLE_EQ(blocks[0].zhat(i), 0.0)
            << "chord " << i << " folded a prior mean it had no frame for";
        EXPECT_GT(blocks[0].Pz(i, i), 1.0e3) << "chord " << i << " is not diffuse";
    }
}

// ---------------------------------------------------------------------------
// 3.  Format-forward: a blob with no ZL rows restores the stored means untouched,
//     reports nothing re-basable, and re-bases nothing even with the read knob armed.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, LegacyBlobRestoresUntouchedAndRebasesNothing) {
    constexpr int R = 2, S = 3;
    const std::vector<ArcKey> ch = chordsOf(R, S);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    Eigen::VectorXd z(K);  z << 0.25, -0.75;
    Eigen::MatrixXd Pz = Eigen::MatrixXd::Zero(K, K);
    Pz.diagonal() << 0.04, 0.09;

    NetworkInvariantCarryDriver src(rs2Cfg(/*write=*/false, /*read=*/false));   // legacy writer
    seedBlock(src, 0, R, S, z, Pz, /*zl=*/nullptr);
    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_EQ(blob.find("\nZL,"), std::string::npos) << "a legacy blob must carry no ZL row";

    NetworkInvariantCarryDriver dst(rs2Cfg(/*write=*/false, /*read=*/true));
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << err;
    EXPECT_EQ(dst.resumeCarryLinBlocks(), 0);
    for (Eigen::Index i = 0; i < K; ++i) {
        double zd = 0, vd = 0;
        ASSERT_TRUE(dst.carriedDdForTest(0, ch[static_cast<std::size_t>(i)], zd, vd));
        EXPECT_DOUBLE_EQ(zd, z(i)) << "a legacy restore must not touch the means";
    }

    // The build against a different linearization still carries the means unchanged:
    // armed, but with nothing to re-base from, the unarmed behaviour.
    std::map<ArcKey, double> lin_now = linMap(R, S, 4.0);
    lin_now[ArcKey(rxV(1), satV(1))] = 9.0;
    std::vector<NetworkInvariantCarryDriver::GroupInput> groups{makeGroup(0, R, S, lin_now)};
    const auto blocks = dst.buildDdPriors(groups);
    ASSERT_EQ(blocks.size(), 1u);
    const auto& cc = dst.counters();
    EXPECT_EQ(cc.resume_rebased, 0);
    EXPECT_EQ(cc.resume_dropped_unbased, 0);
    EXPECT_EQ(cc.chords_carried, K);
    for (Eigen::Index i = 0; i < K; ++i)
        EXPECT_DOUBLE_EQ(blocks[0].zhat(i), z(i));
}

// ---------------------------------------------------------------------------
// 4.  Byte-identity half: with the write knob off the harvest captures no zl, so the
//     blob a run writes does not change.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, WriteKnobOffEmitsNoZlRow) {
    constexpr int R = 2, S = 3;
    std::map<ArcKey, double> lin = linMap(R, S, 4.0);
    lin[ArcKey(rxV(1), satV(1))] = 11.0;

    for (const bool write : {false, true}) {
        NetworkInvariantCarryDriver d(rs2Cfg(write, /*read=*/false));
        std::vector<NetworkInvariantCarryDriver::GroupInput> groups{makeGroup(0, R, S, lin)};
        const auto blocks = d.buildDdPriors(groups);
        ASSERT_EQ(blocks.size(), 1u);
        // A first epoch carries nothing, so every chord is born with a zero prior mean in
        // both arms; the write knob only captures and does not touch the fold.
        for (Eigen::Index i = 0; i < blocks[0].zhat.size(); ++i)
            EXPECT_DOUBLE_EQ(blocks[0].zhat(i), 0.0) << "write=" << write;
        EXPECT_EQ(d.counters().resume_rebased, 0) << "write=" << write;
    }
}

// ---------------------------------------------------------------------------
// 5.  The diagnostic-only D5 window (RS-2b, §21.6 of SPEC_event_harness).  A chord
//     that trips the gate while Config::innov_diagnostic_only is set is counted and
//     reported (so it reaches the epoch's n_d5_flags) and
//     otherwise left alone: prior mean kept, Pᶻ row/col kept, re-tap counter not
//     incremented, and marked in lastInnovWarmupSuppressed so the caller keeps it out
//     of the slip funnel.  With the flag clear the same chord re-taps as usual.  The
//     arms share one setup, so the flags are the only difference.
// ---------------------------------------------------------------------------
namespace {

// A synthetic eq34 network just large enough for the D5 gate to have something to
// catch: integer ambiguities, a per-receiver RPB so the DD is truly integer, a
// spanning-tree ambiguity datum pinned per station and r0's RPB + a shared clock pin.
// A local copy rather than a shared header: these helpers are the fixture, and a
// fixture shared between suites changes under whichever suite edits it.
struct Net {
    int R, S;
    std::vector<double> a, c, b, d;      // rx clock, sat clock, sat bias, rx RPB
    std::vector<std::vector<int>> z;     // integer ambiguities [R][S]
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

std::size_t addNetStation(NetworkEstimator& est, const Net& n, int r,
                          const std::vector<Eigen::Index>& clkCol,
                          const std::vector<Eigen::Index>& pbCol,
                          const std::vector<double>* noise) {
    const int S = n.S;
    const Eigen::Index colA = 0, colD = 1;                 // rx clock, rx RPB
    std::vector<Eigen::Index> colZ(S);
    for (int s = 0; s < S; ++s) colZ[s] = 2 + s;
    std::vector<Eigen::Index> pins;                        // tree-edge ambiguities, pinned
    for (int s = 0; s < S; ++s) if (isTreeEdge(r, s)) pins.push_back(colZ[s]);
    const bool rpbPin = (r == 0);
    const int nrows = 2 * S + static_cast<int>(pins.size()) + (rpbPin ? 1 : 0);
    const Eigen::Index n_local = 2 + S;
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(nrows, est.numShared());
    Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(nrows, n_local);
    Eigen::VectorXd w = Eigen::VectorXd::Ones(nrows), y = Eigen::VectorXd::Zero(nrows);
    int row = 0;
    for (int s = 0; s < S; ++s) {
        A_s(row, clkCol[s]) = -1.0; A_r(row, colA) = 1.0;
        y(row) = n.a[r] - n.c[s]; ++row;
        A_s(row, clkCol[s]) = -1.0; A_s(row, pbCol[s]) = 1.0;
        A_r(row, colA) = 1.0; A_r(row, colD) = 1.0; A_r(row, colZ[s]) = 1.0;
        y(row) = n.a[r] - n.c[s] + n.b[s] + n.d[r] + n.z[r][s]
                 + (noise ? (*noise)[static_cast<std::size_t>(s)] : 0.0);
        ++row;
    }
    for (Eigen::Index pc : pins) { A_r(row, pc) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    if (rpbPin) { A_r(row, colD) = 1.0; y(row) = 0.0; w(row) = 1e10; ++row; }
    return est.addStation(A_s, A_r, w, y);
}

/// One driven epoch: build the group, fold, solve, install.  The star(0,0) tree is the
/// same one @ref isTreeEdge / @ref chordsOf describe, so the chord order matches.
void driveEpoch(NetworkInvariantCarryDriver& drv, const Net& n,
                const std::vector<std::vector<double>>* noise) {
    NetworkEstimator est;
    std::vector<Eigen::Index> clkCol(static_cast<std::size_t>(n.S));
    std::vector<Eigen::Index> pbCol(static_cast<std::size_t>(n.S));
    for (int s = 0; s < n.S; ++s) {
        clkCol[static_cast<std::size_t>(s)] = est.registerClock(n.sats[static_cast<std::size_t>(s)]);
        pbCol[static_cast<std::size_t>(s)] =
            est.registerPhaseBias(n.sats[static_cast<std::size_t>(s)], "L1");
    }
    est.beginEpoch();
    std::vector<std::size_t> ids(static_cast<std::size_t>(n.R));
    for (int r = 0; r < n.R; ++r)
        ids[static_cast<std::size_t>(r)] = addNetStation(
            est, n, r, clkCol, pbCol,
            noise ? &(*noise)[static_cast<std::size_t>(r)] : nullptr);
    NetworkInvariantCarryDriver::GroupInput g;
    g.key = 0;
    for (int r = 0; r < n.R; ++r)
        for (int s = 0; s < n.S; ++s) {
            NetworkInvariantCarryDriver::ArcInput a;
            a.rx_vertex = rxV(r);
            a.sat_vertex = satV(s);
            a.tree_edge = isTreeEdge(r, s);
            a.est_station = static_cast<int>(ids[static_cast<std::size_t>(r)]);
            a.local_col = 2 + s;
            g.arcs.push_back(a);
        }
    drv.beginEpoch(1.0);
    est.setDdPriors(drv.buildDdPriors({g}));
    const NetworkEstimator::Datum datum = {{est.clockColumn(n.sats[0]), 0.0}};
    const Eigen::VectorXd x_s = est.solve(datum);
    drv.installPosterior(est, x_s, datum);
}

NetworkInvariantCarryDriver::Config gateCfg(bool gate, bool diagnostic) {
    NetworkInvariantCarryDriver::Config c;
    c.qz = 0.0;
    c.innov_gate = gate;
    c.innov_k = 4.0;
    c.innov_diagnostic_only = diagnostic;
    return c;
}

}  // namespace

TEST(ResumeCarryRebaseTest, DiagnosticOnlyWindowCountsTheTripButKeepsTheCarry) {
    constexpr int R = 4, S = 4, WARM = 8;
    Net n = makeNet(53, R, S);
    const ArcKey slip_chord(rxV(2), satV(3));   // a chord under star(0,0)
    const ArcKey keep_chord(rxV(1), satV(1));   // an untouched neighbour

    // Three arms on one shared noise stream, so they differ only in the two flags:
    //   ref:  gate off, the fold posterior, untouched by any gate decision;
    //   ctl:  gate on, the ordinary behaviour, the flagged chord re-taps data-only;
    //   diag: gate on and diagnostic-only, the trip is counted and nothing else.
    // The main assertion is diag == ref bit for bit on the flagged chord: a diagnostic
    // gate that still nudged the posterior would pass a looser test.
    NetworkInvariantCarryDriver ref(gateCfg(false, false));
    NetworkInvariantCarryDriver ctl(gateCfg(true, false));
    NetworkInvariantCarryDriver diag(gateCfg(true, true));

    std::mt19937 gen(17);
    std::normal_distribution<double> nd(0.0, 0.02);
    std::vector<std::vector<std::vector<double>>> warm;
    for (int e = 0; e < WARM; ++e) {
        std::vector<std::vector<double>> v(R, std::vector<double>(S));
        for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) v[r][s] = nd(gen);
        warm.push_back(std::move(v));
    }
    for (int e = 0; e < WARM; ++e) {
        driveEpoch(ref,  n, &warm[static_cast<std::size_t>(e)]);
        driveEpoch(ctl,  n, &warm[static_cast<std::size_t>(e)]);
        driveEpoch(diag, n, &warm[static_cast<std::size_t>(e)]);
    }
    ASSERT_EQ(ctl.counters().innov_flags, 0) << "no arm may fire on clean warm-up epochs";
    ASSERT_EQ(diag.counters().innov_flags, 0);

    // An unsignalled integer jump: the fine slip the coarse layer misses.
    n.z[2][3] += 12;
    std::vector<std::vector<double>> jump(R, std::vector<double>(S));
    for (int r = 0; r < R; ++r) for (int s = 0; s < S; ++s) jump[r][s] = nd(gen);
    driveEpoch(ref,  n, &jump);
    driveEpoch(ctl,  n, &jump);
    driveEpoch(diag, n, &jump);

    double z_ref = 0, v_ref = 0, z_ctl = 0, v_ctl = 0, z_diag = 0, v_diag = 0;
    ASSERT_TRUE(ref.carriedDdForTest(0, slip_chord, z_ref, v_ref));
    ASSERT_TRUE(ctl.carriedDdForTest(0, slip_chord, z_ctl, v_ctl));
    ASSERT_TRUE(diag.carriedDdForTest(0, slip_chord, z_diag, v_diag));

    // --- the gate still sees it, in both armed arms -----------------------------
    EXPECT_EQ(ctl.counters().innov_flags, 1);
    EXPECT_EQ(diag.counters().innov_flags, 1) << "the trip must still be REPORTED";
    EXPECT_EQ(ref.counters().innov_flags, 0)  << "gate OFF must never evaluate";

    // --- but only the ordinary arm acts on it ----------------------------------
    EXPECT_EQ(ctl.counters().innov_retaps, 1);
    EXPECT_EQ(ctl.counters().d5_warmup_suppressed, 0);
    EXPECT_EQ(diag.counters().innov_retaps, 0)
        << "flags N / re-taps 0 is the window's whole readout";
    EXPECT_EQ(diag.counters().d5_warmup_suppressed, 1);

    // --- the funnel mask; without it the slip funnel acts on the trip next epoch --
    ASSERT_EQ(diag.lastInnovWarmupSuppressed().size(), 1u);
    EXPECT_EQ(diag.lastInnovWarmupSuppressed()[0], 1);
    ASSERT_EQ(ctl.lastInnovWarmupSuppressed().size(), 1u);
    EXPECT_EQ(ctl.lastInnovWarmupSuppressed()[0], 0);

    // --- the carry itself -------------------------------------------------------
    EXPECT_DOUBLE_EQ(z_diag, z_ref)
        << "the suppressed chord must keep the plain fold posterior, bit-for-bit";
    EXPECT_DOUBLE_EQ(v_diag, v_ref) << "its Pz must not be refolded either";
    EXPECT_NE(z_ctl, z_ref) << "the control arm must genuinely differ (non-vacuous)";
    EXPECT_GT(v_ctl, v_ref) << "the re-tap installs the inflated data-only variance";

    // --- the neighbour is untouched everywhere: the flag is not a global mute ---
    double z_kr = 0, v_kr = 0, z_kd = 0, v_kd = 0;
    ASSERT_TRUE(ref.carriedDdForTest(0, keep_chord, z_kr, v_kr));
    ASSERT_TRUE(diag.carriedDdForTest(0, keep_chord, z_kd, v_kd));
    EXPECT_DOUBLE_EQ(z_kd, z_kr);
    EXPECT_DOUBLE_EQ(v_kd, v_kr);
}

}  // namespace GnssProcessor
