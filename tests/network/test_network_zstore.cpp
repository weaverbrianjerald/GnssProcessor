// Unit gates for the DD cycle-basis z-store (include/network/ar/ZStore.hpp), the
// S-system-invariant carry that the generalized-LS network filter time-links
// (Card 4 (M1) phase B).  Each gate drives the standalone ZComponent /
// NetworkZStore directly with a synthetic bipartite component of known raw
// ambiguities N and covariance Cov(N).  The carried z / Pz are checked against the
// from-scratch cycle-basis reference z = C·N, Pz = C·Cov·Cᵀ, which is independent
// of the incremental transforms.  Standalone-class precedent:
// test_network_carry_vce / test_neq_accumulator.  No ApplicationNetwork coupling.
//
//   Gate 1  CycleBasisUnimodular: removing a tree edge (a pivot) applies an
//           integer T with |det T| = 1, and z' / Pz' equal a from-scratch
//           re-solve in the new basis (T·C_old == C_new).
//   Gate 2  ChordLifecycle: birth (tap-off == the conditional mean/var/cross),
//           prediction-only (predict is an exact identity at q=0; an unmeasured
//           chord is retained, not deleted), death (row/col drop == the Schur
//           marginalization of the information form).
//   Gate 3  FilterEqualsBatch: a 3-epoch recursion with a mid-stream pivot
//           equals the single batch WLS with ambiguity constancy (the GKF
//           equivalence); a negative control shows that the basis transform is
//           required.
//   Extra   MergeBlockIdentity: two independent components unify block-
//           diagonally (OPEN-2: the bridge is a cut edge, carried z is verbatim).

#include "network/ar/ZStore.hpp"

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <algorithm>
#include <random>
#include <stdexcept>
#include <vector>

namespace GnssProcessor {
namespace {

using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::Index;

// A deterministic dense SPD matrix of order n (= M Mᵀ + I), the stand-in for the
// raw ambiguity covariance Cov(N), so the DD covariance Pz = C·Cov·Cᵀ is dense
// (the regime the spec names, where independent per-chord folds would be inexact).
MatrixXd makeSpd(Index n, unsigned seed, double scale = 1.0) {
    std::mt19937 gen(seed);
    std::normal_distribution<double> nrm(0.0, 1.0);
    MatrixXd M(n, n);
    for (Index i = 0; i < n; ++i)
        for (Index j = 0; j < n; ++j) M(i, j) = nrm(gen);
    return scale * (M * M.transpose() + MatrixXd::Identity(n, n));
}

// The canonical phase-B fixture: 3 receivers (ids 0,1,2) × 3 satellites
// (ids 100,101,102), fully connected (9 arcs).  The spanning tree (5 edges) is
// chosen so the pivot below exercises both T-row cases (a chord whose cycle
// contains the dying edge and one whose cycle does not).  Tree: rx0 fans to all 3
// sats; rx1 and rx2 hang off sat100.
struct Fx {
    std::vector<ArcKey> tree;      // 5 tree edges
    std::vector<ArcKey> chords;    // 4 chords, the z order
    std::vector<ArcKey> arcOrder;  // all 9 arcs (column order for C and N)
    ArcKey dyingEdge{1, 100};      // pivot: this tree edge dies ...
    ArcKey enteringChord{1, 101};  // ... replaced by this chord (its cycle covers it)
};
Fx makeFx() {
    Fx f;
    f.tree = {ArcKey(0, 100), ArcKey(0, 101), ArcKey(0, 102),
              ArcKey(1, 100), ArcKey(2, 100)};
    f.chords = {ArcKey(1, 101), ArcKey(1, 102), ArcKey(2, 101), ArcKey(2, 102)};
    f.arcOrder = {ArcKey(0, 100), ArcKey(0, 101), ArcKey(0, 102),
                  ArcKey(1, 100), ArcKey(1, 101), ArcKey(1, 102),
                  ArcKey(2, 100), ArcKey(2, 101), ArcKey(2, 102)};
    return f;
}

// Register a chord set on a tree-seeded component and return its cycle matrix C
// (chords × arcOrder), the from-scratch DD basis.
MatrixXd registerAndCycle(ZComponent& c, const std::vector<ArcKey>& chords,
                          const std::vector<ArcKey>& arcOrder) {
    c.setEstimate(chords, VectorXd::Zero(static_cast<Index>(chords.size())),
                  MatrixXd::Identity(static_cast<Index>(chords.size()),
                                     static_cast<Index>(chords.size())));
    return c.cycleMatrix(arcOrder);
}

// The signed vertex×arc incidence (boundary) matrix, built only from raw arc
// endpoints: the column of arc (u,v) [canonical u<v, reference orientation u→v]
// is −1 at u, +1 at v.  A genuine cycle χ satisfies B·χ = 0.  This matrix is
// independent of ZComponent::fundamentalCycle, so it validates that primitive's
// sign logic and keeps the C = cycleMatrix references used by the other gates
// from being circular.
MatrixXd incidence(const std::vector<ArcKey>& arcOrder) {
    std::set<int> vs;
    for (const ArcKey& a : arcOrder) { vs.insert(a.u); vs.insert(a.v); }
    std::map<int, Index> row;
    Index r = 0;
    for (int v : vs) row[v] = r++;
    MatrixXd B = MatrixXd::Zero(static_cast<Index>(vs.size()),
                               static_cast<Index>(arcOrder.size()));
    for (Index j = 0; j < static_cast<Index>(arcOrder.size()); ++j) {
        B(row[arcOrder[static_cast<std::size_t>(j)].u], j) = -1.0;
        B(row[arcOrder[static_cast<std::size_t>(j)].v], j) = +1.0;
    }
    return B;
}

MatrixXd dropRC(const MatrixXd& M, Index p) {
    const Index n = M.rows();
    MatrixXd out(n - 1, n - 1);
    for (Index i = 0, oi = 0; i < n; ++i) {
        if (i == p) continue;
        for (Index j = 0, oj = 0; j < n; ++j) {
            if (j == p) continue;
            out(oi, oj) = M(i, j);
            ++oj;
        }
        ++oi;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitive validator: every fundamental cycle is a genuine cycle (zero boundary
// under the independent incidence matrix), before and after a pivot.  This is the
// non-circular anchor for the cycleMatrix references the gates below use.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, FundamentalCyclesAreGenuineCycles) {
    const Fx fx = makeFx();
    const MatrixXd B = incidence(fx.arcOrder);

    ZComponent c(fx.tree);
    const MatrixXd C_old = registerAndCycle(c, fx.chords, fx.arcOrder);
    // Entries live in {−1,0,+1} and each row is a bona-fide cycle (B·Cᵀ = 0).
    EXPECT_FALSE((C_old.array().abs() > 1.5).any()) << "cycle incidence must be ±1/0";
    EXPECT_LT((C_old - C_old.array().round().matrix()).cwiseAbs().maxCoeff(), 1e-12)
        << "cycle incidence must be integer";
    EXPECT_LT((B * C_old.transpose()).cwiseAbs().maxCoeff(), 1e-12)
        << "each pre-pivot fundamental cycle must have zero boundary";

    // After a pivot the new basis is still genuine cycles.
    c.setEstimate(fx.chords, VectorXd::Zero(4), MatrixXd::Identity(4, 4));
    c.pivot(fx.dyingEdge, fx.enteringChord);
    const MatrixXd C_new = c.cycleMatrix(fx.arcOrder);
    EXPECT_LT((B * C_new.transpose()).cwiseAbs().maxCoeff(), 1e-12)
        << "each post-pivot fundamental cycle must have zero boundary";
}

// ---------------------------------------------------------------------------
// Gate 1: CycleBasisUnimodular.  The pivot's T is integer, |det T| = 1, and the
// carried z / Pz equal a from-scratch cycle re-derivation in the new basis.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, CycleBasisUnimodular) {
    const Fx fx = makeFx();
    ZComponent c(fx.tree);
    const MatrixXd C_old = registerAndCycle(c, fx.chords, fx.arcOrder);

    // Known raw ambiguities N and dense covariance Cov(N); seed z = C·N, Pz = C·Cov·Cᵀ.
    VectorXd N(9);
    N << 2, -3, 5, 1, -7, 4, -2, 6, -1;   // integer arc ambiguities (arcOrder)
    const MatrixXd Cov = makeSpd(9, 4242u, 0.01);
    const VectorXd z0 = C_old * N;
    const MatrixXd Pz0 = C_old * Cov * C_old.transpose();
    c.setEstimate(fx.chords, z0, Pz0);

    // The event: tree edge (1,100) dies → pivot chord (1,101) into the tree.
    const MatrixXd T = c.pivot(fx.dyingEdge, fx.enteringChord);

    // (a) T is integer with unit determinant (a lossless, integer-preserving basis change).
    EXPECT_LT((T - T.array().round().matrix()).cwiseAbs().maxCoeff(), 1e-12)
        << "the pivot transform must be an INTEGER matrix";
    EXPECT_NEAR(std::abs(T.determinant()), 1.0, 1e-9)
        << "the pivot transform must be UNIMODULAR (|det T| = 1)";

    // (b) The from-scratch cycle basis of the new tree.
    const MatrixXd C_new = c.cycleMatrix(fx.arcOrder);
    EXPECT_LT((T * C_old - C_new).cwiseAbs().maxCoeff(), 1e-9)
        << "T must carry the OLD fundamental-cycle basis onto the NEW one (T·C_old = C_new)";

    // (c) The carried estimate equals the from-scratch re-solve, exactly.
    const VectorXd z_ref = C_new * N;
    const MatrixXd Pz_ref = C_new * Cov * C_new.transpose();
    EXPECT_LT((c.z() - z_ref).cwiseAbs().maxCoeff(), 1e-9)
        << "z' must equal the DD of N in the new basis";
    EXPECT_LT((c.Pz() - Pz_ref).cwiseAbs().maxCoeff(), 1e-9)
        << "Pz' must equal C_new·Cov·C_newᵀ (= T·Pz·Tᵀ)";

    // The pivot changed neither the arc set nor the z-dimension (a pure basis
    // change); it swapped which arc is a tree edge and counted one tree swap.
    EXPECT_EQ(c.zDim(), 4);
    EXPECT_TRUE(c.isTreeEdge(fx.enteringChord));
    EXPECT_TRUE(c.isChord(fx.dyingEdge));
    EXPECT_EQ(c.counters().tree_swaps, 1);

    // Non-vacuity guard: this fixture's T is not the identity; a pivot that
    // changed nothing would make (b) and (c) meaningless.
    EXPECT_GT((T - MatrixXd::Identity(4, 4)).cwiseAbs().maxCoeff(), 0.5)
        << "the fixture lost its discrimination — T is (near) identity";
}

// ---------------------------------------------------------------------------
// Gate 2: ChordLifecycle: birth (tap-off), prediction-only, death (Schur).
// ---------------------------------------------------------------------------
TEST(NetworkZStore, ChordLifecycleBirthMatchesConditional) {
    const Fx fx = makeFx();
    // Start with only the first three chords carried; the 4th is born by tap-off.
    const std::vector<ArcKey> sub(fx.chords.begin(), fx.chords.begin() + 3);
    const ArcKey newChord = fx.chords[3];   // (2,102)

    ZComponent c(fx.tree);
    const MatrixXd C_sub = registerAndCycle(c, sub, fx.arcOrder);   // 3×9

    VectorXd N(9);
    N << 2, -3, 5, 1, -7, 4, -2, 6, -1;
    const MatrixXd Cov = makeSpd(9, 909u, 0.02);
    c.setEstimate(sub, C_sub * N, C_sub * Cov * C_sub.transpose());

    // The full basis (all 4 chords) is the from-scratch reference.
    ZComponent ref(fx.tree);
    const MatrixXd C_full = registerAndCycle(ref, fx.chords, fx.arcOrder);   // 4×9
    const VectorXd z_full = C_full * N;
    const MatrixXd Pz_full = C_full * Cov * C_full.transpose();

    // Tap-off of the new DD = its conditional mean/var + cross-cov to the carried z.
    const VectorXd c_f = C_full.row(3).transpose();          // the new chord's cycle
    const double zhat = c_f.dot(N);                          // fᵀx̂
    const double var = c_f.transpose() * Cov * c_f;          // fᵀ P_x f
    const VectorXd cross = C_sub * Cov * c_f;                // fᵀ P_x C_oldᵀ (length 3)

    c.birthChord(newChord, zhat, var, cross);

    EXPECT_EQ(c.zDim(), 4);
    EXPECT_EQ(c.counters().chords_born, 1);
    EXPECT_EQ(c.counters().tapoffs, 1);
    EXPECT_LT((c.z() - z_full).cwiseAbs().maxCoeff(), 1e-12)
        << "the tapped-off z must equal the full-basis DD means";
    EXPECT_LT((c.Pz() - Pz_full).cwiseAbs().maxCoeff(), 1e-12)
        << "the tapped-off covariance (var + cross) must equal C_full·Cov·C_fullᵀ";
}

TEST(NetworkZStore, ChordLifecyclePredictionOnly) {
    const Fx fx = makeFx();
    ZComponent c(fx.tree);
    const MatrixXd C = registerAndCycle(c, fx.chords, fx.arcOrder);
    VectorXd N(9); N << 2, -3, 5, 1, -7, 4, -2, 6, -1;
    const MatrixXd Cov = makeSpd(9, 7u, 0.03);
    c.setEstimate(fx.chords, C * N, C * Cov * C.transpose());

    const VectorXd z_before = c.z();
    const MatrixXd Pz_before = c.Pz();

    // A q_z = 0 predict is an exact identity (ambiguities are random constants).
    c.predict(0.0, 30.0);
    EXPECT_LT((c.z() - z_before).cwiseAbs().maxCoeff(), 0.0 + 1e-300);
    EXPECT_LT((c.Pz() - Pz_before).cwiseAbs().maxCoeff(), 1e-300);
    EXPECT_EQ(c.zDim(), 4) << "predict must never delete a chord";

    // A partial measurement (only chords 0,2 have a phase row this epoch) leaves
    // the unmeasured chords 1,3 retained: prediction-only, not deleted.
    MatrixXd H(2, 4); H.setZero(); H(0, 0) = 1.0; H(1, 2) = 1.0;
    VectorXd o(2); o << c.z()(0) + 0.01, c.z()(2) - 0.01;
    MatrixXd R = 0.02 * 0.02 * MatrixXd::Identity(2, 2);
    c.update(H, o, R);
    EXPECT_EQ(c.zDim(), 4) << "an unmeasured chord must persist across the update";
    EXPECT_TRUE(c.isChord(fx.chords[1]));
    EXPECT_TRUE(c.isChord(fx.chords[3]));

    // A nonzero q_z inflates only the diagonal and leaves z unchanged.
    const MatrixXd Pz_pre_q = c.Pz();
    const VectorXd z_pre_q = c.z();
    c.predict(1e-6, 60.0);
    EXPECT_LT((c.z() - z_pre_q).cwiseAbs().maxCoeff(), 1e-300);
    const MatrixXd dP = c.Pz() - Pz_pre_q;
    EXPECT_NEAR(dP.diagonal().minCoeff(), 1e-6 * 60.0, 1e-15);
    MatrixXd offdiag = dP; offdiag.diagonal().setZero();
    EXPECT_LT(offdiag.cwiseAbs().maxCoeff(), 1e-15) << "q_z must touch only the diagonal";
}

TEST(NetworkZStore, ChordLifecycleDeathMatchesSchur) {
    const Fx fx = makeFx();
    ZComponent c(fx.tree);
    const MatrixXd C = registerAndCycle(c, fx.chords, fx.arcOrder);
    VectorXd N(9); N << 2, -3, 5, 1, -7, 4, -2, 6, -1;
    const MatrixXd Cov = makeSpd(9, 31337u, 0.05);
    const MatrixXd Pz = C * Cov * C.transpose();
    c.setEstimate(fx.chords, C * N, Pz);

    const ArcKey dying = fx.chords[1];       // marginalize out chord index 1
    const Index p = 1;
    c.killChord(dying);

    EXPECT_EQ(c.zDim(), 3);
    EXPECT_EQ(c.counters().chords_died, 1);
    EXPECT_FALSE(c.isChord(dying));

    // Covariance-form marginalization = drop the row/col.
    const MatrixXd Pz_drop = dropRC(Pz, p);
    EXPECT_LT((c.Pz() - Pz_drop).cwiseAbs().maxCoeff(), 1e-12);

    // ... which equals the Schur-complement marginalization of the information
    // form: I = Pz⁻¹; I_kept = I_kk − I_kf·I_ff⁻¹·I_fk; (I_kept)⁻¹ == the sub-block.
    const MatrixXd I = Pz.inverse();
    const MatrixXd I_kk = dropRC(I, p);
    VectorXd I_kf(3);
    for (Index i = 0, oi = 0; i < 4; ++i) { if (i == p) continue; I_kf(oi++) = I(i, p); }
    const double I_ff = I(p, p);
    const MatrixXd I_marg = I_kk - (I_kf * I_kf.transpose()) / I_ff;
    const MatrixXd Pz_schur = I_marg.inverse();
    EXPECT_LT((c.Pz() - Pz_schur).cwiseAbs().maxCoeff(), 1e-9)
        << "covariance-drop must equal the info-form Schur marginalization";
}

// ---------------------------------------------------------------------------
// Gate 3: FilterEqualsBatch.  A 3-epoch recursion (with a mid-stream unimodular
// pivot) equals the single batch WLS with ambiguity constancy.  This is the GKF
// equivalence; the pivot inside the filter is the Φᶻ that the 2021 paper leaves
// open.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, FilterEqualsBatch) {
    const Fx fx = makeFx();
    ZComponent c(fx.tree);
    registerAndCycle(c, fx.chords, fx.arcOrder);   // register the 4 chords (basis 0)

    // Constant true DDs (integer) and a proper (dense) prior in the old basis.
    VectorXd z_true(4); z_true << 3, -2, 5, -1;
    VectorXd m0(4);    m0 << 2.6, -1.7, 4.5, -0.6;
    const MatrixXd P0 = makeSpd(4, 2718u, 0.25);
    c.setEstimate(fx.chords, m0, P0);

    // Deterministic observation noise for all epochs.
    std::mt19937 gen(90210u);
    std::normal_distribution<double> nrm(0.0, 1.0);
    auto noise = [&](Index m, double sd) {
        VectorXd e(m);
        for (Index i = 0; i < m; ++i) e(i) = sd * nrm(gen);
        return e;
    };

    // Epoch 1 (old basis): observe all 4 DDs.
    const MatrixXd H1 = MatrixXd::Identity(4, 4);
    const MatrixXd R1 = 0.020 * 0.020 * MatrixXd::Identity(4, 4);
    const VectorXd o1 = H1 * z_true + noise(4, 0.020);
    c.update(H1, o1, R1);

    // The lifecycle event: tree edge (1,100) dies → pivot (1,101) into the tree.
    const MatrixXd T = c.pivot(fx.dyingEdge, fx.enteringChord);
    const VectorXd z_true_new = T * z_true;   // truth transforms with the basis

    // Epoch 2 (new basis): partial; observe only DDs 0 and 2 (prediction-only 1,3).
    MatrixXd H2(2, 4); H2.setZero(); H2(0, 0) = 1.0; H2(1, 2) = 1.0;
    const MatrixXd R2 = 0.030 * 0.030 * MatrixXd::Identity(2, 2);
    const VectorXd o2 = H2 * z_true_new + noise(2, 0.030);
    c.update(H2, o2, R2);

    // Epoch 3 (new basis): observe all 4.
    const MatrixXd H3 = MatrixXd::Identity(4, 4);
    const MatrixXd R3 = 0.025 * 0.025 * MatrixXd::Identity(4, 4);
    const VectorXd o3 = H3 * z_true_new + noise(4, 0.025);
    c.update(H3, o3, R3);

    const VectorXd z_rec = c.z();
    const MatrixXd Pz_rec = c.Pz();

    // Batch reference, expressed entirely in the new basis.  The prior and epoch-1
    // observation transform: m0' = T·m0, P0' = T·P0·Tᵀ, and o1 = H1·z_old =
    // (H1·T⁻¹)·z_new ⇒ design H1' = H1·T⁻¹.  Epochs 2,3 are already new-basis.
    const MatrixXd Tinv = T.inverse();
    const MatrixXd H1p = H1 * Tinv;
    const VectorXd m0p = T * m0;
    const MatrixXd P0p = T * P0 * T.transpose();
    const MatrixXd Lam0 = P0p.inverse();
    const MatrixXd R1i = R1.inverse(), R2i = R2.inverse(), R3i = R3.inverse();

    const MatrixXd Lam = Lam0
        + H1p.transpose() * R1i * H1p
        + H2.transpose()  * R2i * H2
        + H3.transpose()  * R3i * H3;
    const VectorXd b = Lam0 * m0p
        + H1p.transpose() * R1i * o1
        + H2.transpose()  * R2i * o2
        + H3.transpose()  * R3i * o3;
    const VectorXd z_batch = Lam.ldlt().solve(b);
    const MatrixXd Pz_batch = Lam.inverse();

    EXPECT_LT((z_rec - z_batch).cwiseAbs().maxCoeff(), 1e-8)
        << "the recursion (with a mid-stream pivot) must equal the batch WLS";
    EXPECT_LT((Pz_rec - Pz_batch).cwiseAbs().maxCoeff(), 1e-8)
        << "the recursion covariance must equal the batch covariance";

    // Negative control: a batch that ignores the basis change (uses H1, not H1·T⁻¹)
    // does not match, which shows the unimodular transform is required, not a no-op.
    const MatrixXd Lam_naive = Lam0
        + H1.transpose() * R1i * H1
        + H2.transpose() * R2i * H2
        + H3.transpose() * R3i * H3;
    const VectorXd b_naive = Lam0 * m0p
        + H1.transpose() * R1i * o1
        + H2.transpose() * R2i * o2
        + H3.transpose() * R3i * o3;
    const VectorXd z_naive = Lam_naive.ldlt().solve(b_naive);
    EXPECT_GT((z_rec - z_naive).cwiseAbs().maxCoeff(), 1e-3)
        << "forgetting the pivot's basis change must visibly break the match";
}

// ---------------------------------------------------------------------------
// Extra (OPEN-2): MergeBlockIdentity.  Two independent components join via a
// bridge (a cut edge); carried z is verbatim, Pz block-diagonal, pre-existing
// cycles unchanged, and a surplus bridge is tapped off as a new chord.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, MergeBlockIdentity) {
    // Component A: rx {0,1} × sat {100,101}; tree {(0,100),(0,101),(1,100)}, chord (1,101).
    ZComponent A(std::vector<ArcKey>{ArcKey(0, 100), ArcKey(0, 101), ArcKey(1, 100)});
    A.setEstimate({ArcKey(1, 101)}, (VectorXd(1) << 4.0).finished(),
                  (MatrixXd(1, 1) << 0.02).finished());
    // Component B: rx {2,3} × sat {102,103}; tree {(2,102),(2,103),(3,102)}, chord (3,103).
    ZComponent B(std::vector<ArcKey>{ArcKey(2, 102), ArcKey(2, 103), ArcKey(3, 102)});
    B.setEstimate({ArcKey(3, 103)}, (VectorXd(1) << -1.5).finished(),
                  (MatrixXd(1, 1) << 0.05).finished());

    // Bridge (1,102) joins A's vertex 1 to B's vertex 102 → one tree edge.  A
    // surplus bridge (0,102) also joins the two sides → it closes a cycle → chord.
    ZComponent::SurplusBridge sb;
    sb.arc = ArcKey(0, 102);
    sb.zhat = 2.25;
    sb.var = 0.03;
    sb.cross = VectorXd::Zero(2);   // cross to the merged [z_A; z_B] (both independent here)

    const ZComponent M = A.mergedWith(B, ArcKey(1, 102), {sb});

    // Carried z is the verbatim concatenation; Pz is block-diagonal (they were
    // independent), then the surplus chord appended.
    ASSERT_EQ(M.zDim(), 3);
    EXPECT_NEAR(M.z()(0), 4.0, 1e-15);
    EXPECT_NEAR(M.z()(1), -1.5, 1e-15);
    EXPECT_NEAR(M.z()(2), 2.25, 1e-15);
    EXPECT_NEAR(M.Pz()(0, 0), 0.02, 1e-15);
    EXPECT_NEAR(M.Pz()(1, 1), 0.05, 1e-15);
    EXPECT_NEAR(M.Pz()(2, 2), 0.03, 1e-15);
    EXPECT_NEAR(M.Pz()(0, 1), 0.0, 1e-15) << "the two merged sides start uncorrelated";

    // The bridge is a cut edge ⇒ every pre-existing chord's fundamental cycle is
    // unchanged (T = block-identity).  A's chord (1,101) does not traverse the bridge.
    const auto cyc = M.fundamentalCycle(ArcKey(1, 101));
    EXPECT_EQ(cyc.count(ArcKey(1, 102)), 0u) << "a pre-existing cycle must not cross the bridge";
    const auto cycA = A.fundamentalCycle(ArcKey(1, 101));
    EXPECT_EQ(cyc.size(), cycA.size()) << "the pre-existing cycle is verbatim after merge";

    EXPECT_TRUE(M.isTreeEdge(ArcKey(1, 102)));
    EXPECT_TRUE(M.isChord(ArcKey(0, 102)));
    EXPECT_EQ(M.counters().chords_born, 1);   // only the surplus bridge is a birth
}

// ---------------------------------------------------------------------------
// Container smoke: the NetworkZStore aggregates components + counters and drives
// a merge through the min-id canonical contract.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, ContainerMergeAndCounters) {
    NetworkZStore store;
    store.create(0, {ArcKey(0, 100), ArcKey(0, 101), ArcKey(1, 100)});
    store.component(0).setEstimate({ArcKey(1, 101)}, (VectorXd(1) << 4.0).finished(),
                                   (MatrixXd(1, 1) << 0.02).finished());
    store.create(1, {ArcKey(2, 102), ArcKey(2, 103), ArcKey(3, 102)});
    store.component(1).setEstimate({ArcKey(3, 103)}, (VectorXd(1) << -1.5).finished(),
                                   (MatrixXd(1, 1) << 0.05).finished());
    EXPECT_EQ(store.numComponents(), 2u);
    EXPECT_EQ(store.totalZDim(), 2);

    store.merge(0, 1, ArcKey(1, 102));   // merged canonical id = min(0,1) = 0
    EXPECT_EQ(store.numComponents(), 1u);
    EXPECT_EQ(store.merges(), 1);
    ASSERT_NE(store.find(0), nullptr);
    EXPECT_EQ(store.find(1), nullptr);
    EXPECT_EQ(store.find(0)->zDim(), 2);
    EXPECT_EQ(store.totalZDim(), 2);
}

// ---------------------------------------------------------------------------
// Lifecycle coverage: leaf growth (a new vertex extends the tree with no cycle)
// and its endpoint guard.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, LeafGrowthAndGuard) {
    ZComponent c(std::vector<ArcKey>{ArcKey(0, 100), ArcKey(0, 101), ArcKey(1, 100)});
    ASSERT_EQ(c.zDim(), 0);
    // Sat 102 rises at rx1: exactly one new endpoint → leaf growth, no cycle, no z.
    c.addLeafArc(ArcKey(1, 102));
    EXPECT_EQ(c.zDim(), 0) << "leaf growth adds no cycle → no z entry";
    EXPECT_TRUE(c.isTreeEdge(ArcKey(1, 102)));
    EXPECT_TRUE(c.hasVertex(102));
    EXPECT_EQ(c.counters().leaf_growths, 1);
    // Both endpoints already present ⇒ closes a cycle ⇒ must go through birthChord.
    EXPECT_THROW(c.addLeafArc(ArcKey(1, 101)), std::invalid_argument);
    // Both endpoints new ⇒ a disconnected floating edge ⇒ rejected.
    EXPECT_THROW(c.addLeafArc(ArcKey(5, 500)), std::invalid_argument);
    // The rejected calls mutated neither the counter nor the graph.
    EXPECT_EQ(c.counters().leaf_growths, 1);
    EXPECT_FALSE(c.hasVertex(500));
}

// ---------------------------------------------------------------------------
// Counter integrity: a merge rolls up both sides' tallies, and the store
// aggregate survives the absorbed component's erasure.  (This gate fails on a
// mergedWith that drops the absorbed side's predict tally.)
// ---------------------------------------------------------------------------
TEST(NetworkZStore, MergeRollsUpCounters) {
    ZComponent A(std::vector<ArcKey>{ArcKey(0, 100), ArcKey(0, 101), ArcKey(1, 100)});
    A.setEstimate({ArcKey(1, 101)}, (VectorXd(1) << 4.0).finished(),
                  (MatrixXd(1, 1) << 0.02).finished());
    ZComponent B(std::vector<ArcKey>{ArcKey(2, 102), ArcKey(2, 103), ArcKey(3, 102)});
    B.setEstimate({ArcKey(3, 103)}, (VectorXd(1) << -1.5).finished(),
                  (MatrixXd(1, 1) << 0.05).finished());
    A.predict(); A.predict();     // A.predicts == 2
    B.predict();                  // B.predicts == 1
    const ZComponent M = A.mergedWith(B, ArcKey(1, 102));
    EXPECT_EQ(M.counters().predicts, 3) << "merge must roll up BOTH sides' predict tally";
    EXPECT_EQ(M.counters().chords_born, 0);   // no surplus bridge here
}

TEST(NetworkZStore, AggregateCountersSurviveMerge) {
    NetworkZStore store;
    store.create(0, {ArcKey(0, 100), ArcKey(0, 101), ArcKey(1, 100)});
    store.component(0).setEstimate({ArcKey(1, 101)}, (VectorXd(1) << 4.0).finished(),
                                   (MatrixXd(1, 1) << 0.02).finished());
    store.create(1, {ArcKey(2, 102), ArcKey(2, 103), ArcKey(3, 102)});
    store.component(1).setEstimate({ArcKey(3, 103)}, (VectorXd(1) << -1.5).finished(),
                                   (MatrixXd(1, 1) << 0.05).finished());
    store.component(0).predict();
    store.component(1).predict();
    store.component(1).predict();   // 3 predicts across the two live components
    EXPECT_EQ(store.aggregateCounters().predicts, 3);
    store.merge(0, 1, ArcKey(1, 102));   // erases component 1
    EXPECT_EQ(store.aggregateCounters().predicts, 3)
        << "the aggregate predict tally must survive a merge (no dropped roll-up)";
}

// ---------------------------------------------------------------------------
// Invariant: re-seeding setEstimate with a smaller chord set prunes the displaced
// chord's arc, leaving no stale arc and no all-zero cycle column.
// ---------------------------------------------------------------------------
TEST(NetworkZStore, SetEstimateReseedPrunesArcs) {
    ZComponent c(std::vector<ArcKey>{ArcKey(0, 100), ArcKey(0, 101),
                                     ArcKey(0, 102), ArcKey(1, 100)});
    c.setEstimate({ArcKey(1, 101), ArcKey(1, 102)},
                  (VectorXd(2) << 1.0, 2.0).finished(), MatrixXd::Identity(2, 2));
    {
        const auto a = c.allArcs();
        EXPECT_NE(std::find(a.begin(), a.end(), ArcKey(1, 102)), a.end());
    }
    // Re-seed with a smaller set: (1,102) no longer appears as a graph arc.
    c.setEstimate({ArcKey(1, 101)}, (VectorXd(1) << 1.0).finished(),
                  (MatrixXd(1, 1) << 0.5).finished());
    EXPECT_EQ(c.zDim(), 1);
    EXPECT_FALSE(c.isChord(ArcKey(1, 102)));
    const auto arcs = c.allArcs();
    EXPECT_EQ(std::find(arcs.begin(), arcs.end(), ArcKey(1, 102)), arcs.end())
        << "a displaced chord's arc must be pruned (no phantom arc)";
    EXPECT_EQ(c.cycleMatrix(arcs).cols(), static_cast<Index>(arcs.size()));
    for (const auto& a : arcs)
        EXPECT_TRUE(c.isTreeEdge(a) || c.isChord(a)) << "arcs_ must equal tree ∪ chords";
}

}  // namespace GnssProcessor
