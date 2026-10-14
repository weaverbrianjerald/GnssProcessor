/**
 * @file test_resume_carry_rebase.cpp
 * @brief The linearization frame of the carry blob (RS-2,
 *        docs/SPEC_event_harness.md §21.4): @c network.state_snapshot_carry_lin
 *        (the @c ZL row), and the post-resume D5 warm-up window (RS-2b).
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
 * The repair was a frame translation in two halves.  The writer half stays (off by
 * default): it stores @f$z_\ell=\sum c\,x_{\rm lin}@f$ per chord, one @c ZL row per
 * block beside @c Z.  The reader half (the re-base at the first post-resume build,
 * @c network.resume_carry_rebase) was retired; a restored @c ZL row is parsed and
 * counted, and the branch folds in the writer's frame.
 *
 * ## The cases
 *
 * 1. **ZlRowsRoundTripThroughTheBlob**: two blocks harvested with the write knob armed
 *    serialize their @c ZL rows, and a restore counts both blocks and keeps the means
 *    and @f$P^z@f$ (off-diagonal included) verbatim.
 * 2. **LegacyBlobRestoresUntouchedAndRebasesNothing**: the format-forward contract.
 *    A blob with no @c ZL rows (any snapshot written without the write knob)
 *    restores the stored means unchanged, reports zero @c ZL blocks, and a build
 *    against a different linearization carries the means unchanged.
 * 3. **WriteKnobOffEmitsNoZlRow**: the byte-identity half.  With the write knob off
 *    the blob carries no @c ZL row, so a snapshot does not change.
 * 4. **DiagnosticOnlyWindowCountsTheTripButKeepsTheCarry**: the
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
/// per-arc linearization value the RS-2 write half reads.
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

NetworkInvariantCarryDriver::Config rs2Cfg(bool write) {
    NetworkInvariantCarryDriver::Config c;
    c.qz = 0.0;                 // constant DDs: no time update to confound the frame test
    c.carry_lin = write;
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
    if (zl) d.seedCarriedLinForTest(group, *zl);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1.  The write half round trip: harvested with the write knob armed, the blob
//     carries one ZL row per block, and the restore counts them and keeps the
//     means and Pᶻ verbatim.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, ZlRowsRoundTripThroughTheBlob) {
    constexpr int R = 2, S = 3;                 // 2 chords: (1,1) and (1,2)
    const std::vector<ArcKey> ch = chordsOf(R, S);
    ASSERT_EQ(ch.size(), 2u);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    Eigen::VectorXd z0(K);  z0 << 0.25, -0.75;
    Eigen::VectorXd z1(K);  z1 << 1.50,  2.25;
    Eigen::MatrixXd Pz0 = Eigen::MatrixXd::Zero(K, K);
    Pz0.diagonal() << 0.04, 0.09;
    Pz0(0, 1) = Pz0(1, 0) = 0.01;               // a real off-diagonal: Pᶻ must survive verbatim
    const Eigen::MatrixXd Pz1 = 2.0 * Pz0;
    const Eigen::VectorXd zl_w = Eigen::VectorXd::Zero(K);

    NetworkInvariantCarryDriver src(rs2Cfg(/*write=*/true));
    seedBlock(src, 0, R, S, z0, Pz0, &zl_w);
    seedBlock(src, 7, R, S, z1, Pz1, &zl_w);

    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_NE(blob.find("\nZL,"), std::string::npos)
        << "the write knob is armed but no ZL row was emitted";

    NetworkInvariantCarryDriver dst(rs2Cfg(/*write=*/false));
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << err;
    EXPECT_EQ(dst.resumeCarryLinBlocks(), 2);
    for (int grp : {0, 7}) {
        const Eigen::VectorXd& want = (grp == 0) ? z0 : z1;
        const Eigen::MatrixXd& wantP = (grp == 0) ? Pz0 : Pz1;
        for (Eigen::Index i = 0; i < K; ++i) {
            double zd = 0, vd = 0;
            ASSERT_TRUE(dst.carriedDdForTest(grp, ch[static_cast<std::size_t>(i)], zd, vd));
            EXPECT_DOUBLE_EQ(zd, want(i)) << "group " << grp << " chord " << i;
            EXPECT_DOUBLE_EQ(vd, wantP(i, i)) << "group " << grp << " chord " << i;
        }
        double cd = 0;
        ASSERT_TRUE(dst.carriedCrossForTest(grp, ch[0], ch[1], cd));
        EXPECT_DOUBLE_EQ(cd, wantP(0, 1)) << "group " << grp;
    }
}

// ---------------------------------------------------------------------------
// 2.  Format-forward: a blob with no ZL rows restores the stored means untouched,
//     reports no ZL block, and the build carries the means unchanged.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, LegacyBlobRestoresUntouchedAndRebasesNothing) {
    constexpr int R = 2, S = 3;
    const std::vector<ArcKey> ch = chordsOf(R, S);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    Eigen::VectorXd z(K);  z << 0.25, -0.75;
    Eigen::MatrixXd Pz = Eigen::MatrixXd::Zero(K, K);
    Pz.diagonal() << 0.04, 0.09;

    NetworkInvariantCarryDriver src(rs2Cfg(/*write=*/false));   // legacy writer
    seedBlock(src, 0, R, S, z, Pz, /*zl=*/nullptr);
    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_EQ(blob.find("\nZL,"), std::string::npos) << "a legacy blob must carry no ZL row";

    NetworkInvariantCarryDriver dst(rs2Cfg(/*write=*/false));
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << err;
    EXPECT_EQ(dst.resumeCarryLinBlocks(), 0);
    for (Eigen::Index i = 0; i < K; ++i) {
        double zd = 0, vd = 0;
        ASSERT_TRUE(dst.carriedDdForTest(0, ch[static_cast<std::size_t>(i)], zd, vd));
        EXPECT_DOUBLE_EQ(zd, z(i)) << "a legacy restore must not touch the means";
    }

    // The build against a different linearization still carries the means unchanged.
    std::map<ArcKey, double> lin_now = linMap(R, S, 4.0);
    lin_now[ArcKey(rxV(1), satV(1))] = 9.0;
    std::vector<NetworkInvariantCarryDriver::GroupInput> groups{makeGroup(0, R, S, lin_now)};
    const auto blocks = dst.buildDdPriors(groups);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(dst.counters().chords_carried, K);
    for (Eigen::Index i = 0; i < K; ++i)
        EXPECT_DOUBLE_EQ(blocks[0].zhat(i), z(i));
}

// ---------------------------------------------------------------------------
// 3.  Byte-identity half: with the write knob off the harvest captures no zl, so the
//     blob a run writes does not change.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, WriteKnobOffEmitsNoZlRow) {
    constexpr int R = 2, S = 3;
    std::map<ArcKey, double> lin = linMap(R, S, 4.0);
    lin[ArcKey(rxV(1), satV(1))] = 11.0;

    for (const bool write : {false, true}) {
        NetworkInvariantCarryDriver d(rs2Cfg(write));
        std::vector<NetworkInvariantCarryDriver::GroupInput> groups{makeGroup(0, R, S, lin)};
        const auto blocks = d.buildDdPriors(groups);
        ASSERT_EQ(blocks.size(), 1u);
        // A first epoch carries nothing, so every chord is born with a zero prior mean in
        // both arms; the write knob only captures and does not touch the fold.
        for (Eigen::Index i = 0; i < blocks[0].zhat.size(); ++i)
            EXPECT_DOUBLE_EQ(blocks[0].zhat(i), 0.0) << "write=" << write;
    }
}

// ---------------------------------------------------------------------------
// 4.  The diagnostic-only D5 window (RS-2b, §21.6 of SPEC_event_harness).  A chord
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
