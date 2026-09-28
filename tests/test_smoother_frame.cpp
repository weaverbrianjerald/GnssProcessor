/**
 * @file test_smoother_frame.cpp
 * @brief Characterization tests for the RTS smoother's BSSD-frame / slip
 *        compensation (audit findings F1 / P2-1).
 *
 * Two layers:
 *   1. The pure inverse-reference operator (Smoother::inverseReReferenceGroup):
 *      it is the exact inverse of PivotManager's forward re-reference and
 *      transforms the full covariance (ambiguity<->core cross terms), not a
 *      group-local block.
 *   2. The backward pass over hand-built snapshots: a clean pivot handover does
 *      not leak the metre-scale frame offset into the preceding epoch, a pivot
 *      change without a clean-handover record segments the constellation, the
 *      reset-satellite segmentation matches on the satellite key boundary (a
 *      reset of "G2" does not segment "G21"), and a gap teardown that shrinks the
 *      a-priori dimension is a segment boundary.
 *
 * The snapshot fields (pivot_satellites, clean_handovers) and the public static
 * operator are set and called directly, so no friend shim is needed.
 */

#include <gtest/gtest.h>

#include "filter/Smoother.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr size_t kNone = std::numeric_limits<size_t>::max();

// The forward BSSD re-reference has the same structure as the inverse operator,
// anchored on the new pivot (p1) instead of the old one, so it is exercised
// through the same public entry point.
void reReferenceTo(const std::vector<Eigen::Index>& group, Eigen::Index datum,
                   Eigen::VectorXd& x, Eigen::MatrixXd& P) {
    Smoother::inverseReReferenceGroup(group, datum, x, P);
}

// ============================================================================
// 1. Pure operator: exact-inverse round trip (with ambiguity<->core coupling)
// ============================================================================

TEST(SmootherFrame, InverseReReferenceExactlyUndoesForward) {
    // State: [pos(0), p0_amb(1), a_amb(2), p1_amb(3), b_amb(4)].
    // Start in the p0 frame: p0 (idx 1) is the frozen datum (value 0, row/col 0).
    const std::vector<Eigen::Index> group{ 1, 2, 3, 4 };
    const Eigen::Index p0 = 1, p1 = 3;

    Eigen::VectorXd x0(5);
    x0 << 10.0, 0.0 /*p0 frozen*/, 5.0, 8.0, -3.0;

    // Symmetric P with the p0 row/col zeroed (frozen) and rich off-diagonals,
    // including ambiguity<->position (idx 0) coupling (audit finding R1).
    Eigen::MatrixXd P0(5, 5);
    P0 << 0.50, 0.0,  0.10, 0.20, -0.05,
          0.0,  0.0,  0.0,  0.0,   0.0,
          0.10, 0.0,  0.40, 0.07,  0.03,
          0.20, 0.0,  0.07, 0.60, -0.02,
         -0.05, 0.0,  0.03,-0.02,  0.45;

    Eigen::VectorXd x = x0;
    Eigen::MatrixXd P = P0;

    // Forward to the p1 frame, then inverse back to the p0 frame.
    reReferenceTo(group, p1, x, P);
    // Sanity: the forward leg actually moved the state (else the test is vacuous).
    EXPECT_GT((x - x0).cwiseAbs().maxCoeff(), 1.0);

    Smoother::inverseReReferenceGroup(group, p0, x, P);

    EXPECT_LT((x - x0).cwiseAbs().maxCoeff(), 1e-9)
        << "inverse re-reference did not recover the original state";
    EXPECT_LT((P - P0).cwiseAbs().maxCoeff(), 1e-9)
        << "inverse re-reference did not recover the original covariance "
           "(ambiguity<->position coupling must round-trip)";
}

TEST(SmootherFrame, InverseReReferenceFreezesDatumAndIsFullState) {
    // A non-zero ambiguity<->position covariance must be carried through the
    // datum subtraction (full row/col), not dropped.
    const std::vector<Eigen::Index> group{ 1, 2, 3 };
    Eigen::VectorXd x(4);
    x << 2.0, 4.0, 9.0, 1.0;
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(4, 4);
    P(0, 1) = P(1, 0) = 0.3;   // position(0) <-> member(1)
    P(0, 2) = P(2, 0) = 0.2;   // position(0) <-> member(2)

    Smoother::inverseReReferenceGroup(group, /*datum=*/1, x, P);

    // Datum frozen.
    EXPECT_DOUBLE_EQ(x(1), 0.0);
    EXPECT_DOUBLE_EQ(P(1, 1), 0.0);
    EXPECT_DOUBLE_EQ(P.row(1).cwiseAbs().sum(), 0.0);
    EXPECT_DOUBLE_EQ(P.col(1).cwiseAbs().sum(), 0.0);
    // member(2) - datum(1): value 9 - 4 = 5; the position cross term shifts by
    // P(0,2) - P(0,1) = 0.2 - 0.3 = -0.1.
    EXPECT_DOUBLE_EQ(x(2), 5.0);
    EXPECT_NEAR(P(0, 2), -0.1, 1e-12);
    EXPECT_NEAR(P(2, 0), -0.1, 1e-12);
}

// ============================================================================
// 2. Backward pass over hand-built snapshots
// ============================================================================

// Build a minimal snapshot.  Core layout: pos(0..2), tropo(3); clock and all
// other optional core states absent.  Ambiguities follow at the given indices.
SmootherEpochSnapshot makeSnap(int week, double tow,
                               const Eigen::VectorXd& x_pred, const Eigen::MatrixXd& P_pred,
                               const Eigen::VectorXd& x_post, const Eigen::MatrixXd& P_post,
                               const std::map<std::string, size_t>& ambiguities) {
    SmootherEpochSnapshot s;
    s.t.week = week;
    s.t.tow = tow;
    s.dt = 1.0;
    s.F = Eigen::MatrixXd::Identity(x_pred.size(), x_pred.size());
    s.x_pred = x_pred;  s.P_pred = P_pred;
    s.x_post = x_post;  s.P_post = P_post;
    s.pos_idx = 0;
    s.tropo_idx = 3;
    s.clock_idx = kNone;  // BSSD-like: no receiver clock state.
    s.velocity_idx = kNone;
    s.phase_clock_idx = kNone;
    s.gradient_n_idx = kNone;
    s.gradient_e_idx = kNone;
    s.ambiguities = ambiguities;
    s.has_posterior = true;
    s.is_discontinuity = false;
    return s;
}

// A clean pivot handover does not leak the frame offset into the prior epoch:
// the inverse-reference brings k+1 back to epoch k's frame, so a converged
// ambiguity on a third satellite keeps its value instead of being dragged by
// the (large) new-pivot offset.
TEST(SmootherFrame, CleanHandoverDoesNotLeakIntoPriorEpoch) {
    LogManager log;
    // dim = 7: [px,py,pz,tropo, G01(4), G02(5), G03(6)]
    const std::map<std::string, size_t> amb{
        { "G01_L1", 4 }, { "G02_L1", 5 }, { "G03_L1", 6 } };

    auto diagP = [](double core, double g01, double g02, double g03) {
        Eigen::MatrixXd P = Eigen::MatrixXd::Zero(7, 7);
        for (int i = 0; i < 4; ++i) P(i, i) = core;
        P(4, 4) = g01; P(5, 5) = g02; P(6, 6) = g03;
        return P;
    };

    // Epoch 0: pivot = G01 (idx4 frozen, value 0).  G02=5, G03=7.
    Eigen::VectorXd x_post0(7); x_post0 << 1, 2, 3, 0.1, 0.0, 5.0, 7.0;
    SmootherEpochSnapshot s0 = makeSnap(2300, 0.0,
        x_post0, diagP(0.02, 0.0, 0.05, 0.05),
        x_post0, diagP(0.02, 0.0, 0.04, 0.04), amb);
    s0.pivot_satellites = { { "GPS", "G01" } };

    // Epoch 1: clean handover G01 -> G02.
    //   x_pred1 = F*x_post0 (old frame, G01 frozen):  G01=0, G02=5, G03=7.
    //   x_post1 (new frame after handover, G02 frozen): G01=-5, G02=0, G03=2.
    Eigen::VectorXd x_pred1(7); x_pred1 << 1, 2, 3, 0.1, 0.0, 5.0, 7.0;
    Eigen::VectorXd x_post1(7); x_post1 << 1, 2, 3, 0.1, -5.0, 0.0, 2.0;
    SmootherEpochSnapshot s1 = makeSnap(2300, 1.0,
        x_pred1, diagP(0.02, 0.0,  0.05, 0.05),   // P_pred old frame: G01 frozen
        x_post1, diagP(0.02, 0.05, 0.0,  0.05),   // P_post new frame: G02 frozen
        amb);
    s1.pivot_satellites = { { "GPS", "G02" } };
    s1.clean_handovers  = { { "GPS", { "G01", "G02" } } };

    Smoother sm({ s0, s1 }, log);
    ASSERT_TRUE(sm.run());
    const auto& res = sm.results();
    ASSERT_EQ(res.size(), 2u);
    ASSERT_TRUE(res[0].x.allFinite());

    // The prior is reconciled into epoch 1's frame, so the smoothing innovation
    // is ~0 and epoch 0 keeps its posterior.  In particular G03 stays at 7.0; a
    // frame-blind smoother would move it to 7 + gain*(2 - 7) = 3.0 with
    // gain = 0.04/0.05 = 0.8.
    EXPECT_NEAR(res[0].x(6), 7.0, 1e-6) << "G03 leaked the handover offset";
    EXPECT_NEAR(res[0].x(5), 5.0, 1e-6) << "G02 leaked the handover offset";
    EXPECT_LT((res[0].x - x_post0).cwiseAbs().maxCoeff(), 1e-6);
}

// A pivot change with no clean-handover record (disturbed or forced) segments
// the constellation: epoch 0 falls back to its forward posterior rather than
// inverse-referencing against a discontinuous old pivot.
TEST(SmootherFrame, DisturbedHandoverSegmentsConstellation) {
    LogManager log;
    const std::map<std::string, size_t> amb{
        { "G01_L1", 4 }, { "G02_L1", 5 }, { "G03_L1", 6 } };
    auto diagP = [](double core, double a, double b, double c) {
        Eigen::MatrixXd P = Eigen::MatrixXd::Zero(7, 7);
        for (int i = 0; i < 4; ++i) P(i, i) = core;
        P(4, 4) = a; P(5, 5) = b; P(6, 6) = c;
        return P;
    };
    Eigen::VectorXd x_post0(7); x_post0 << 1, 2, 3, 0.1, 0.0, 5.0, 7.0;
    SmootherEpochSnapshot s0 = makeSnap(2300, 0.0,
        x_post0, diagP(0.02, 0.0, 0.05, 0.05),
        x_post0, diagP(0.02, 0.0, 0.04, 0.04), amb);
    s0.pivot_satellites = { { "GPS", "G01" } };

    Eigen::VectorXd x_pred1(7); x_pred1 << 1, 2, 3, 0.1, 0.0, 5.0, 7.0;
    Eigen::VectorXd x_post1(7); x_post1 << 1, 2, 3, 0.1, -5.0, 0.0, 2.0;
    SmootherEpochSnapshot s1 = makeSnap(2300, 1.0,
        x_pred1, diagP(0.02, 0.0,  0.05, 0.05),
        x_post1, diagP(0.02, 0.05, 0.0,  0.05), amb);
    s1.pivot_satellites = { { "GPS", "G02" } };
    // No clean_handovers entry -> disturbed/forced -> segment.

    Smoother sm({ s0, s1 }, log);
    ASSERT_TRUE(sm.run());
    const auto& res = sm.results();
    ASSERT_TRUE(res[0].x.allFinite());
    // All GPS ambiguities segmented -> epoch 0 keeps its forward posterior.
    EXPECT_DOUBLE_EQ(res[0].x(4), 0.0);
    EXPECT_DOUBLE_EQ(res[0].x(5), 5.0);
    EXPECT_DOUBLE_EQ(res[0].x(6), 7.0);
}

// Reset-satellite segmentation matches on the satellite key boundary: a reset of
// "G2" segments "G2_L1" but not "G21_L1".  A substring match would segment both
// and disable smoothing on G21 without any message (audit finding R7).
TEST(SmootherFrame, ResetSegmentationMatchesSatelliteKeyBoundary) {
    LogManager log;
    // dim = 6: [px,py,pz,tropo, G2_L1(4), G21_L1(5)]  (no BSSD; empty pivot map)
    const std::map<std::string, size_t> amb{ { "G2_L1", 4 }, { "G21_L1", 5 } };
    auto diagP = [](double core, double a, double b) {
        Eigen::MatrixXd P = Eigen::MatrixXd::Zero(6, 6);
        for (int i = 0; i < 4; ++i) P(i, i) = core;
        P(4, 4) = a; P(5, 5) = b;
        return P;
    };
    Eigen::VectorXd x_post0(6); x_post0 << 0, 0, 0, 0, 1.0, 2.0;
    SmootherEpochSnapshot s0 = makeSnap(2300, 0.0,
        x_post0, diagP(0.02, 0.04, 0.04),
        x_post0, diagP(0.02, 0.04, 0.04), amb);

    Eigen::VectorXd x_pred1(6); x_pred1 << 0, 0, 0, 0, 1.0, 2.0;
    Eigen::VectorXd x_post1(6); x_post1 << 0, 0, 0, 0, 1.5, 2.5;  // update moved both
    SmootherEpochSnapshot s1 = makeSnap(2300, 1.0,
        x_pred1, diagP(0.02, 0.05, 0.05),
        x_post1, diagP(0.02, 0.03, 0.03), amb);
    s1.reset_satellites = { "G2" };  // reset G2 only

    Smoother sm({ s0, s1 }, log);
    ASSERT_TRUE(sm.run());
    const auto& res = sm.results();
    ASSERT_TRUE(res[0].x.allFinite());

    // G2 (idx4) segmented -> forward posterior retained.
    EXPECT_DOUBLE_EQ(res[0].x(4), 1.0);
    // G21 (idx5) not segmented -> smoothed (gain 0.04/0.05 = 0.8 on a +0.5
    // innovation => +0.4).  A substring match would segment it (== 2.0).
    EXPECT_NEAR(res[0].x(5), 2.4, 1e-9);
}

// A long-gap teardown (Application::handleGap) prunes ambiguity states before
// predict, so epoch k+1's a-priori is captured at a smaller dimension than
// epoch k's posterior, while k+1's posterior is rebuilt at full size.  The
// backward pass treats this as a segment boundary instead of indexing the
// (smaller) a-priori with the (larger) epoch-k indices.  Without the guard this
// is an out-of-bounds read (Debug assert / Release UB).
TEST(SmootherFrame, GapTeardownDimensionMismatchIsSegmented) {
    LogManager log;

    // Epoch 0: full state, dim = 6: [px,py,pz,tropo, G01_L1(4), G02_L1(5)].
    const std::map<std::string, size_t> amb0{ { "G01_L1", 4 }, { "G02_L1", 5 } };
    Eigen::VectorXd x_post0(6); x_post0 << 1, 2, 3, 0.1, 4.0, 6.0;
    Eigen::MatrixXd P0 = Eigen::MatrixXd::Identity(6, 6) * 0.05;
    SmootherEpochSnapshot s0 = makeSnap(2300, 0.0, x_post0, P0, x_post0, P0, amb0);

    // Epoch 1: a long gap tore down the ambiguities before predict, so the
    // a-priori is core-only (dim 4); the update then rebuilt the ambiguities,
    // so the posterior is dim 6 again with the same keys (large indices).
    Eigen::VectorXd x_pred1 = Eigen::VectorXd::Zero(4);   // core only
    Eigen::MatrixXd P_pred1 = Eigen::MatrixXd::Identity(4, 4) * 0.1;
    Eigen::VectorXd x_post1(6); x_post1 << 1, 2, 3, 0.1, 9.0, 11.0;
    Eigen::MatrixXd P_post1 = Eigen::MatrixXd::Identity(6, 6) * 0.05;
    SmootherEpochSnapshot s1;
    s1.t.week = 2300; s1.t.tow = 60.0; s1.dt = 60.0;
    s1.F = Eigen::MatrixXd::Identity(4, 4);   // sized for the (shrunken) a-priori
    s1.x_pred = x_pred1;  s1.P_pred = P_pred1;
    s1.x_post = x_post1;  s1.P_post = P_post1;
    s1.pos_idx = 0; s1.tropo_idx = 3; s1.clock_idx = kNone;
    s1.velocity_idx = kNone; s1.phase_clock_idx = kNone;
    s1.gradient_n_idx = kNone; s1.gradient_e_idx = kNone;
    s1.ambiguities = amb0;        // rebuilt at the same (large) indices 4,5
    s1.has_posterior = true;
    s1.is_discontinuity = false;  // handleGap does not trip wasCompleteReset

    Smoother sm({ s0, s1 }, log);
    ASSERT_TRUE(sm.run());   // must not assert / OOB
    const auto& res = sm.results();
    ASSERT_EQ(res.size(), 2u);
    ASSERT_TRUE(res[0].x.allFinite());
    // Segmented at the teardown boundary -> epoch 0 keeps its forward posterior.
    EXPECT_TRUE(res[0].x.isApprox(x_post0));
}

}  // namespace
}  // namespace GnssProcessor
