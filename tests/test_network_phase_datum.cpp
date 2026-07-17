/**
 * @file test_network_phase_datum.cpp
 * @brief Unit tests for the phase-bias↔ambiguity datum maintenance seam
 *        (filter/NetworkPhaseDatum.hpp) — decomposition INC-8.
 *
 * Covers healNearNullPhaseBias, the prioritized-datum spectrum-based
 * drop-and-heal relocated VERBATIM out of the network run loop: a phase-bias
 * column left below the rank-gate cutoff (its only ambiguity anchor dropped,
 * or weak clock separation) is soft-anchored toward 0 at σ = 1 m so one weak
 * column cannot withhold the whole epoch.  The heal MUST use the SAME cutoff
 * as the gate it protects (NetworkEstimator::kNullSpaceTol), which the
 * throw-before / pass-after gate assertions here pin down.
 *
 * This file will ALSO host the INC-8b tests (prioritized anchor election +
 * incumbent carry) when that seam lands — keep the fixture below reusable.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/StateKeySchema.hpp"

namespace GnssProcessor {
namespace {

// Minimal single-station system with an optionally-orphaned phase-bias column:
// shared columns [CLK(G01), PB(G01,L1), PB(G02,L1)], locals [a_r, M1(, M2)].
//   code   P(G01)  = a_r − c                      (anchors CLK via the local clock)
//   phase  Φ(G01)  = a_r − c + b1 + M1            (+ 1e10 pivot pin M1 = 0)
//   phase  Φ(G02)  = a_r     + b2 + M2            (+ 1e10 pivot pin M2 = 0),
//                                                  ONLY when anchor_second
// The clock gauge (δc = δa_r common shift) is pinned by a folded tight
// constraint row (the anchor-free datum realization), so the declared datum
// dimension is 0 and the ONLY possible near-null direction is a phase-bias
// column.  With anchor_second=false, PB(G02,L1) has no rows at all — the pure
// "dropped anchor station" orphan the heal exists for.
struct HealFixture {
    NetworkEstimator est;
    std::set<std::string> keys;   // this epoch's shared phase-bias key set

    void build(bool anchor_second) {
        const Eigen::Index c   = est.registerClock("G01");
        const Eigen::Index pb1 = est.registerPhaseBias("G01", "L1");
        const Eigen::Index pb2 = est.registerPhaseBias("G02", "L1");
        keys = { StateKeys::makeSatPhaseBiasKey("G01", "L1"),
                 StateKeys::makeSatPhaseBiasKey("G02", "L1") };

        est.beginEpoch();
        const Eigen::Index n_local = anchor_second ? 3 : 2;   // [a_r, M1(, M2)]
        const Eigen::Index m       = anchor_second ? 5 : 3;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
        Eigen::VectorXd w   = Eigen::VectorXd::Ones(m);
        Eigen::VectorXd y   = Eigen::VectorXd::Zero(m);
        // code: P = a_r − c
        A_s(0, c) = -1.0; A_r(0, 0) = 1.0;
        // phase: Φ = a_r − c + b1 + M1
        A_s(1, c) = -1.0; A_s(1, pb1) = 1.0; A_r(1, 0) = 1.0; A_r(1, 1) = 1.0;
        // local pivot-ambiguity pin M1 = 0 (fixes the b1↔M1 gauge)
        A_r(2, 1) = 1.0; w(2) = 1e10;
        if (anchor_second) {
            // phase: Φ = a_r + b2 + M2, plus its pivot pin M2 = 0
            A_s(3, pb2) = 1.0; A_r(3, 0) = 1.0; A_r(3, 2) = 1.0;
            A_r(4, 2) = 1.0; w(4) = 1e10;
        }
        est.addStation(A_s, A_r, w, y);

        // Fold the clock gauge as a tight constraint row BEFORE the rank gate
        // (declared dimension 0), like the sum-to-zero clock datum does.
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, est.numShared());
        H(0, c) = 1.0;
        est.addSharedConstraintRows(H, Eigen::VectorXd::Zero(1),
                                    NetworkSbasis::kClockGaugeWeight);
    }
};

// The three guard short-circuits return 0 without touching the spectrum or the
// capture channel: legacy datum (prioritized=false), an empty estimator
// (numShared()==0), and an empty phase-bias key set.
TEST(NetworkPhaseDatumTest, ReturnsZeroWhenNotPrioritizedOrEmpty) {
    std::vector<NetworkEstimator::SharedPrior> cap;

    // (a) prioritized = false: immediate 0 even over a real layout + key set.
    {
        NetworkEstimator est;
        est.registerClock("G01");
        est.registerPhaseBias("G01", "L1");
        const std::set<std::string> keys{ StateKeys::makeSatPhaseBiasKey("G01", "L1") };
        EXPECT_EQ(healNearNullPhaseBias(est, keys, /*prioritized=*/false,
                                        /*armed=*/true, cap), 0);
    }
    // (b) empty estimator (numShared() == 0): 0 before any spectrum work.
    {
        NetworkEstimator est;
        const std::set<std::string> keys{ StateKeys::makeSatPhaseBiasKey("G01", "L1") };
        EXPECT_EQ(healNearNullPhaseBias(est, keys, /*prioritized=*/true,
                                        /*armed=*/true, cap), 0);
    }
    // (c) empty key set: 0.
    {
        NetworkEstimator est;
        est.registerClock("G01");
        est.registerPhaseBias("G01", "L1");
        EXPECT_EQ(healNearNullPhaseBias(est, {}, /*prioritized=*/true,
                                        /*armed=*/true, cap), 0);
    }
    EXPECT_TRUE(cap.empty());   // capture channel untouched in all three
}

// The orphan PB(G02,L1) column (its only anchor station "dropped") is a genuine
// null direction: the full-rank gate throws before the heal, the heal
// soft-anchors exactly that one column ({PhaseBias, G02, L1, 0, info 1.0} —
// σ = 1 m — mirrored into cap_priors iff the within-epoch replay is armed), and
// the SAME gate at the SAME cutoff passes afterwards.
TEST(NetworkPhaseDatumTest, HealsOrphanPhaseBiasColumn) {
    for (const bool armed : {true, false}) {
        HealFixture f;
        f.build(/*anchor_second=*/false);
        NetworkSbasis sb;

        // Before: one near-null direction vs declared dimension 0 -> throws.
        EXPECT_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                            NetworkEstimator::kNullSpaceTol),
                     std::runtime_error)
            << "armed=" << armed;

        std::vector<NetworkEstimator::SharedPrior> cap;
        EXPECT_EQ(healNearNullPhaseBias(f.est, f.keys, /*prioritized=*/true,
                                        armed, cap), 1)
            << "armed=" << armed;

        if (armed) {
            ASSERT_EQ(cap.size(), 1u);
            EXPECT_EQ(cap[0].kind, SharedParamKind::PhaseBias);
            EXPECT_EQ(cap[0].sat, "G02");
            EXPECT_EQ(cap[0].signal, "L1");
            EXPECT_EQ(cap[0].value, 0.0);
            EXPECT_EQ(cap[0].info, 1.0);     // σ = 1 m heal anchor
        } else {
            EXPECT_TRUE(cap.empty());        // replay not armed: nothing mirrored
        }

        // After: the residual gauge is resolved — the gate no longer throws.
        EXPECT_NO_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                               NetworkEstimator::kNullSpaceTol))
            << "armed=" << armed;
    }
}

// A fully pinned/informed system (both PB columns anchored by their pivot pins,
// clock gauge folded) has no near-null direction: nothing is healed and no
// prior is folded or captured.
TEST(NetworkPhaseDatumTest, LeavesWellAnchoredColumnsAlone) {
    HealFixture f;
    f.build(/*anchor_second=*/true);
    NetworkSbasis sb;

    // Sanity: the system is full-rank at the gate's own cutoff.
    EXPECT_NO_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                           NetworkEstimator::kNullSpaceTol));

    std::vector<NetworkEstimator::SharedPrior> cap;
    EXPECT_EQ(healNearNullPhaseBias(f.est, f.keys, /*prioritized=*/true,
                                    /*armed=*/true, cap), 0);
    EXPECT_TRUE(cap.empty());
}

} // namespace
} // namespace GnssProcessor
