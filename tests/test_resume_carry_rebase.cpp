/**
 * @file test_resume_carry_rebase.cpp
 * @brief RS-2 (docs/SPEC_event_harness.md §21.4) — the carry blob's LINEARIZATION
 *        frame: @c network.state_snapshot_carry_lin (the @c ZL row) and
 *        @c network.resume_carry_rebase (the re-base at the first post-resume build).
 *
 * ## The defect this file pins
 *
 * @ref GnssProcessor::NetworkInvariantCarryDriver stores each chord's mean in the
 * CORRECTION frame: @f$\hat z=\sum c\,x_r=\lambda N-\sum c\,x_{\rm lin}@f$.  A resumed
 * run re-seeds every station ambiguity @c x_lin from code at its first epoch (the
 * station layer is deliberately not snapshotted), so a restored @f$\hat z@f$ is wrong
 * by @f$\sum c\,(x_{\rm lin,writer}-x_{\rm lin,now})@f$ — metres per chord, folded at
 * the carry's stiffness on the whole branch at once, with the D5 gate post-solve and
 * therefore only able to flag the damage.  Measured on the field golden's 60-epoch null
 * legs: the first post-resume solve discharges through ONE prior class (`dd_fold` RHS
 * norm 7.8e7 against 616 for every code observation of 61 stations) and the SH health
 * gate then withholds all 61 epochs.
 *
 * The repair is a frame TRANSLATION, in two default-OFF halves: the writer stores
 * @f$z_\ell=\sum c\,x_{\rm lin}@f$ per chord (one @c ZL row per block, beside @c Z), and
 * the first DD prior build after a restore adds @f$W z_{\ell,\rm writer}-z_{\ell,\rm now}@f$
 * to the projected prior means, with @f$W@f$ the cycle-basis projection the build already
 * forms.  @f$P^z@f$ is untouched.
 *
 * ## What the cases are, and why each one is here
 *
 * 1. **RoundTripRebasesToTheSameAbsoluteDd** — the load-bearing one.  Two blocks are
 *    harvested against a known @c x_lin, serialized WITH the @c ZL rows, restored, and
 *    re-based against a SHIFTED @c x_lin.  The shift is placed on ONE arc, so the
 *    predicted correction is exact and per-chord (a chord whose fundamental cycle misses
 *    that arc must not move) — a uniform shift would be cancelled by @f$\sum c=0@f$ and
 *    would pass even if the re-base did nothing.  @f$P^z@f$ is asserted byte-equal, and
 *    an UN-shifted control asserts the re-base is the identity when the two frames agree.
 * 2. **UnbasableContributorIsMarginalizedNotFolded** — the invariant-#7 edge.  A restored
 *    block that is UNBASED but carries no @c ZL row cannot be re-based; its chords take
 *    the ordinary birth path (diffuse prior, zero prior mean) instead of folding the
 *    writer's frame, and the drop is COUNTED.  Without the count the behaviour is
 *    indistinguishable from a chord that simply had no predecessor.
 * 3. **LegacyBlobRestoresUntouchedAndRebasesNothing** — the format-forward contract.  A
 *    blob with no @c ZL rows (every snapshot written before this card) restores exactly
 *    today's means, reports zero re-basable blocks, and re-bases nothing even with the
 *    read knob armed.  This is the "armed but inert" case the run-end line exists to make
 *    visible (standing invariant #4).
 * 4. **WriteKnobOffEmitsNoZlRow** — the byte-identity half: with the write knob off the
 *    blob carries no @c ZL row at all, so a snapshot is unchanged by the card's presence.
 *
 * Out of scope: the estimator-side fold itself (tests/test_network_invariant_carry_driver.cpp
 * and tests/test_pd_safe_fold.cpp own that), and the end-to-end resume, which is a harness
 * leg pair — §21.4's P1/P2/P3 gates.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <set>
#include <sstream>
#include <string>
#include <vector>

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

/// Seed ONE component of @p group with means @p z (chord order = @ref chordsOf) and a
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
//     ABSOLUTE DD (ẑ + zl) is unchanged — with a per-arc δ, so the correction is
//     chord-specific and a do-nothing implementation cannot pass.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, RoundTripRebasesToTheSameAbsoluteDd) {
    constexpr int R = 2, S = 3;                 // 2 chords: (1,1) and (1,2)
    const std::vector<ArcKey> ch = chordsOf(R, S);
    ASSERT_EQ(ch.size(), 2u);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    const double kBase = 4.0;                   // the writer's x_lin on every arc
    const double kDelta = 1.75;                 // the shift, on ONE arc only

    // --- writer: two blocks (two groups), means and Pᶻ known, ZL captured ------
    Eigen::VectorXd z0(K);  z0 << 0.25, -0.75;
    Eigen::VectorXd z1(K);  z1 << 1.50,  2.25;
    Eigen::MatrixXd Pz0 = Eigen::MatrixXd::Zero(K, K);
    Pz0.diagonal() << 0.04, 0.09;
    Pz0(0, 1) = Pz0(1, 0) = 0.01;               // a real off-diagonal: Pᶻ must survive verbatim
    const Eigen::MatrixXd Pz1 = 2.0 * Pz0;

    // zl over the writer's frame: every arc at kBase ⇒ Σ c·x_lin = 0 (Σ c = 0 on a DD
    // cycle).  That is the honest writer value here, and it is precisely why the READER's
    // shift has to be non-uniform for this test to discriminate.
    const Eigen::VectorXd zl_w = Eigen::VectorXd::Zero(K);

    NetworkInvariantCarryDriver src(rs2Cfg(/*write=*/true, /*read=*/false));
    seedBlock(src, 0, R, S, z0, Pz0, &zl_w);
    seedBlock(src, 7, R, S, z1, Pz1, &zl_w);

    std::ostringstream os;
    src.serializeCarry(os);
    const std::string blob = os.str();
    EXPECT_NE(blob.find("\nZL,"), std::string::npos)
        << "the write knob is armed but no ZL row was emitted";

    // --- reader: restore, then build against a SHIFTED linearization -----------
    NetworkInvariantCarryDriver dst(rs2Cfg(/*write=*/false, /*read=*/true));
    std::istringstream is(blob);
    std::string err;
    ASSERT_TRUE(dst.restoreCarry(is, &err)) << err;
    EXPECT_EQ(dst.resumeCarryLinBlocks(), 2);

    // δ on the chord arc (1,1) only.  Its own fundamental cycle carries it at +1
    // (ZComponent::fundamentalCycle seeds the chord in reference orientation), and the
    // cycle of (1,2) does not contain it at all — so the prediction is
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
        // Pᶻ is an exact frame TRANSLATION's invariant — the projection here is the
        // identity (the tree and chord set are unchanged), so it must come through
        // verbatim, off-diagonal included.
        for (Eigen::Index i = 0; i < K; ++i)
            for (Eigen::Index j = 0; j < K; ++j)
                EXPECT_DOUBLE_EQ(blocks[b].Pz(i, j), wantP(i, j))
                    << "block " << b << " Pz(" << i << "," << j << ") moved";
    }

    // Control: the SAME restore built against the writer's own linearization must be the
    // identity — a re-base that shifts an already-correct frame is as wrong as one that
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
// 2.  A restored block that is UNBASED but carries no ZL row cannot be re-based.
//     Its chords must take the birth path (prior mean 0, diffuse Pᶻ) rather than fold
//     the writer's frame, and the drop must be counted.
// ---------------------------------------------------------------------------
TEST(ResumeCarryRebaseTest, UnbasableContributorIsMarginalizedNotFolded) {
    constexpr int R = 2, S = 3;
    const std::vector<ArcKey> ch = chordsOf(R, S);
    const Eigen::Index K = static_cast<Eigen::Index>(ch.size());

    Eigen::VectorXd z(K);  z << 0.25, -0.75;
    Eigen::MatrixXd Pz = Eigen::MatrixXd::Zero(K, K);
    Pz.diagonal() << 0.04, 0.09;

    NetworkInvariantCarryDriver d(rs2Cfg(/*write=*/false, /*read=*/true));
    // UNBASED with NO zl — the state a reader would be in if the blob's frame were
    // asserted but its rows were missing.  seedCarriedLinForTest with an EMPTY vector is
    // the only way to reach it from a test; restoreCarry can never produce it (it sets
    // unbased only when it parsed a full ZL row), which is the point of the guard.
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
// 3.  Format-forward: a blob with NO ZL rows restores today's means untouched, reports
//     nothing re-basable, and re-bases nothing even with the read knob armed.
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

    // The build against a DIFFERENT linearization must still carry, unchanged — today's
    // behaviour, which is what "armed but nothing to re-base from" means.
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
// 4.  Byte-identity half: with the write knob OFF the harvest captures no zl, so the
//     blob a run writes is unchanged by this card's presence.
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
        // BOTH arms — the write knob is capture-only and must not touch the fold.
        for (Eigen::Index i = 0; i < blocks[0].zhat.size(); ++i)
            EXPECT_DOUBLE_EQ(blocks[0].zhat(i), 0.0) << "write=" << write;
        EXPECT_EQ(d.counters().resume_rebased, 0) << "write=" << write;
    }
}

}  // namespace GnssProcessor
