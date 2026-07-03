/**
 * @file test_filter_core.cpp
 * @brief Unit tests for StateManager dynamic state-vector bookkeeping.
 *
 * StateManager is, per CLAUDE.md, the single source of truth for state
 * layout — every piece of dynamic state (ambiguities, per-satellite
 * ionosphere, code biases, sat biases, system time offsets) goes through
 * its add / remove / prune APIs and the index maps it owns.  Drift in
 * those maps silently corrupts the filter, so the surface is worth
 * pinning down with explicit assertions.
 *
 * Scope (smoke + key invariants):
 *   - Construction: state vector / covariance sized correctly under the
 *     default GnssConfig, expected core indices.
 *   - State-vector / covariance round-trips.
 *   - addAmbiguity / removeAmbiguity / pruneAmbiguitiesExcept: state size
 *     changes by ±1, indices reflow correctly after row removal, key→
 *     index lookups are consistent.
 *   - Same for per-satellite ionosphere, receiver code-bias, and sat-bias
 *     state categories.
 *   - getProcessNoiseMatrix / getTransitionMatrix shape and finite-ness.
 *
 * NOT covered here (intentionally deferred):
 *   - KalmanFilter::predict / update.  The KalmanFilter constructor
 *     requires a PppModel reference, and PppModel pulls in
 *     ProductParser + RinexParser + their full setup.  That construction
 *     chain is too heavy for the "smoke + key invariants" tier; the
 *     predict() math is itself a thin wrapper over StateManager's F / Q
 *     getters (x = F·x, P = F·P·Fᵀ + Q), which are tested here directly.
 *     A future integration-test pass can build the full chain.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <set>
#include <string>

namespace GnssProcessor {
namespace {

// PPP core size used by Application::run when not in baseline mode.
constexpr size_t kPppCoreSize = 5;

// Construct a StateManager backed by a default-initialized GnssConfig and
// a fresh LogManager.  Tests own the lifetime of the underlying refs via
// this thin wrapper so the StateManager doesn't outlive them.
struct StateManagerHarness {
    LogManager log_manager;
    GnssConfig config;            // Default-initialized: PPP mode, all 6 active systems.
    StateManager state_manager{ kPppCoreSize, log_manager, config };

    // Default config has 6 active systems (GPS, Galileo, GLONASS, BeiDou,
    // QZSS, SBAS).  StateManager's ctor adds one inter-system time-offset
    // state per non-GPS system, so the initial core-only state vector is
    // 5 + 5 = 10 elements long.
    static constexpr size_t kExpectedInitialSize = kPppCoreSize + 5;
};

// ============================================================================
// Construction
// ============================================================================

TEST(StateManagerTest, ConstructionSizesStateVectorAndCovariance) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    EXPECT_EQ(sm.getStateSize(), StateManagerHarness::kExpectedInitialSize);

    Eigen::VectorXd x = sm.getStateVector();
    Eigen::MatrixXd P = sm.getCovariance();
    EXPECT_EQ(static_cast<size_t>(x.size()),
              StateManagerHarness::kExpectedInitialSize);
    EXPECT_EQ(static_cast<size_t>(P.rows()),
              StateManagerHarness::kExpectedInitialSize);
    EXPECT_EQ(static_cast<size_t>(P.cols()),
              StateManagerHarness::kExpectedInitialSize);
}

TEST(StateManagerTest, SsrDiscontinuityBumpDetection) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    EXPECT_FALSE(sm.noteSsrDiscontinuity("G01|L1C", 3));   // first sighting: record only
    EXPECT_FALSE(sm.noteSsrDiscontinuity("G01|L1C", 3));   // unchanged
    EXPECT_TRUE (sm.noteSsrDiscontinuity("G01|L1C", 4));   // incremented -> bump
    EXPECT_FALSE(sm.noteSsrDiscontinuity("G01|L1C", 4));   // steady at new value
    EXPECT_TRUE (sm.noteSsrDiscontinuity("G01|L1C", 0));   // any change (4-bit wrap) -> bump
    EXPECT_FALSE(sm.noteSsrDiscontinuity("G02|L1C", 7));   // independent key, first sighting
}

TEST(StateManagerTest, CoreIndicesArePositionClockTropo) {
    StateManagerHarness h;
    EXPECT_EQ(h.state_manager.getPositionIndex(), 0u);
    EXPECT_EQ(h.state_manager.getClockIndex(), 3u);
    EXPECT_EQ(h.state_manager.getTropoIndex(), 4u);
}

TEST(StateManagerTest, ConstructionInitiallyHasNoDynamicStates) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    EXPECT_TRUE(sm.getAmbiguities().empty());
    EXPECT_TRUE(sm.getIonosphereStates().empty());
    EXPECT_TRUE(sm.getCodeBiasStates().empty());
    EXPECT_TRUE(sm.getSatBiasStates().empty());

    // Lookup of a non-existent ambiguity returns -1.
    EXPECT_EQ(sm.getAmbiguityIndex("G01_L1C"), -1);
    EXPECT_EQ(sm.getCodeBiasIndex("GPS_C2W"), -1);
    EXPECT_EQ(sm.getSatBiasIndex("G01|C1W"), -1);
}

// ============================================================================
// State / covariance round-trip
// ============================================================================

TEST(StateManagerTest, StateVectorAndCovarianceRoundTrip) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t n = sm.getStateSize();
    Eigen::VectorXd x = Eigen::VectorXd::LinSpaced(static_cast<Eigen::Index>(n),
                                                   1.0, static_cast<double>(n));
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(n, n) * 7.5;

    sm.setStateVector(x);
    sm.setCovariance(P);

    EXPECT_TRUE(sm.getStateVector().isApprox(x, 1e-12));
    EXPECT_TRUE(sm.getCovariance().isApprox(P, 1e-12));
}

TEST(StateManagerTest, EpochTimeRoundTrip) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    GnssTime t{ 2313, 259200.0 };
    sm.setStateEpochTime(t);
    EXPECT_EQ(sm.getStateEpochTime().week, t.week);
    EXPECT_DOUBLE_EQ(sm.getStateEpochTime().tow, t.tow);
}

// ============================================================================
// Ambiguity bookkeeping
// ============================================================================

TEST(StateManagerTest, AddAmbiguityGrowsStateAndAssignsIndex) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t before = sm.getStateSize();
    const size_t idx = sm.addAmbiguity("G01_L1C", /*initial=*/12.5);

    EXPECT_EQ(sm.getStateSize(), before + 1);
    EXPECT_EQ(idx, before);  // Newly appended state goes at the end.
    EXPECT_EQ(sm.getAmbiguityIndex("G01_L1C"), static_cast<int>(idx));

    // The newly-added state value should be visible in the state vector.
    Eigen::VectorXd x = sm.getStateVector();
    EXPECT_NEAR(x(static_cast<Eigen::Index>(idx)), 12.5, 1e-12);
}

// removeStateAtIndex must decrement the indices of EVERY dynamic map sitting
// above the removed slot — ambiguity, code-bias, sat-bias, sat-PHASE-bias (W2),
// sat-CLOCK (W4-S2b), and ionosphere.  A missing decrement loop silently
// corrupts the survivor->index mapping.  Seed each map with a distinct sentinel
// value, remove the FIRST dynamic state (forcing every other dynamic state to
// shift down by one), and assert each survivor's key still resolves to the slot
// holding its sentinel.
TEST(StateManagerTest, RemovalDecrementsAllSixDynamicMaps) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t base = sm.getStateSize();

    // Add the to-be-removed phase-bias state FIRST so removing it shifts all
    // five other dynamic maps (and the surviving phase-bias entry) downward.
    const size_t victim = sm.addSatPhaseBiasState("G05|L1C|PB", 500.0);
    EXPECT_EQ(victim, base);

    sm.addAmbiguity("G01_L1C", 100.0);
    sm.addIonosphereState("G05", 200.0);
    sm.addCodeBiasState("GPS_C2W", 300.0);
    sm.addSatBiasState("G05|C1W", 400.0);
    const size_t survivor_pb = sm.addSatPhaseBiasState("G07|L1C|PB", 600.0);
    const size_t survivor_sc = sm.addSatClockState("G05|SC", 700.0);

    EXPECT_EQ(sm.getStateSize(), base + 7);
    EXPECT_EQ(survivor_pb, base + 5);
    EXPECT_EQ(survivor_sc, base + 6);

    // Remove the first dynamic state via the public phase-bias prune.
    sm.pruneSatPhaseBiasStatesExcept({ "G07|L1C|PB" });

    EXPECT_EQ(sm.getStateSize(), base + 6);
    EXPECT_EQ(sm.getSatPhaseBiasIndex("G05|L1C|PB"), -1);

    // Every survivor's key must now resolve to the slot holding its sentinel.
    const Eigen::VectorXd x = sm.getStateVector();
    auto valAt = [&](int idx) { return x(static_cast<Eigen::Index>(idx)); };

    ASSERT_GE(sm.getAmbiguityIndex("G01_L1C"), 0);
    EXPECT_NEAR(valAt(sm.getAmbiguityIndex("G01_L1C")), 100.0, 1e-12);
    ASSERT_GE(sm.getIonosphereIndex("G05"), 0);
    EXPECT_NEAR(valAt(sm.getIonosphereIndex("G05")), 200.0, 1e-12);
    ASSERT_GE(sm.getCodeBiasIndex("GPS_C2W"), 0);
    EXPECT_NEAR(valAt(sm.getCodeBiasIndex("GPS_C2W")), 300.0, 1e-12);
    ASSERT_GE(sm.getSatBiasIndex("G05|C1W"), 0);
    EXPECT_NEAR(valAt(sm.getSatBiasIndex("G05|C1W")), 400.0, 1e-12);
    ASSERT_GE(sm.getSatPhaseBiasIndex("G07|L1C|PB"), 0);
    EXPECT_NEAR(valAt(sm.getSatPhaseBiasIndex("G07|L1C|PB")), 600.0, 1e-12);
    ASSERT_GE(sm.getSatClockIndex("G05|SC"), 0);
    EXPECT_NEAR(valAt(sm.getSatClockIndex("G05|SC")), 700.0, 1e-12);

    // The surviving phase-bias and sat-clock states each shifted down one slot.
    EXPECT_EQ(sm.getSatPhaseBiasIndex("G07|L1C|PB"), static_cast<int>(survivor_pb - 1));
    EXPECT_EQ(sm.getSatClockIndex("G05|SC"), static_cast<int>(survivor_sc - 1));
}

TEST(StateManagerTest, AddAmbiguityIndependentKeysGetDistinctIndices) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t i_a = sm.addAmbiguity("G01_L1C", 1.0);
    const size_t i_b = sm.addAmbiguity("G02_L1C", 2.0);
    const size_t i_c = sm.addAmbiguity("G03_L1C", 3.0);

    EXPECT_EQ(i_b, i_a + 1);
    EXPECT_EQ(i_c, i_b + 1);
    EXPECT_EQ(sm.getAmbiguities().size(), 3u);
}

TEST(StateManagerTest, RemoveAmbiguityShrinksStateAndReflowsLaterIndices) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 1.0);
    const size_t i_b = sm.addAmbiguity("G02_L1C", 2.0);
    const size_t i_c = sm.addAmbiguity("G03_L1C", 3.0);
    const size_t before = sm.getStateSize();

    // Remove the middle ambiguity.  Per StateManager::removeStateAtIndex
    // contract, all ambiguity_indices_ entries with index > rem_idx are
    // decremented by one.
    ASSERT_TRUE(sm.removeAmbiguity("G02_L1C"));

    EXPECT_EQ(sm.getStateSize(), before - 1);
    EXPECT_EQ(sm.getAmbiguityIndex("G02_L1C"), -1);
    EXPECT_EQ(sm.getAmbiguityIndex("G01_L1C"), static_cast<int>(i_b - 1));
    EXPECT_EQ(sm.getAmbiguityIndex("G03_L1C"), static_cast<int>(i_c - 1));
}

TEST(StateManagerTest, RemoveAmbiguityNonExistentReturnsFalse) {
    StateManagerHarness h;
    EXPECT_FALSE(h.state_manager.removeAmbiguity("does_not_exist"));
    EXPECT_EQ(h.state_manager.getStateSize(),
              StateManagerHarness::kExpectedInitialSize);
}

TEST(StateManagerTest, PruneAmbiguitiesExceptRemovesUnlistedKeys) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 1.0);
    sm.addAmbiguity("G02_L1C", 2.0);
    sm.addAmbiguity("G03_L1C", 3.0);
    sm.addAmbiguity("G04_L1C", 4.0);

    sm.pruneAmbiguitiesExcept({ "G02_L1C", "G04_L1C" });

    EXPECT_EQ(sm.getAmbiguities().size(), 2u);
    EXPECT_GE(sm.getAmbiguityIndex("G02_L1C"), 0);
    EXPECT_GE(sm.getAmbiguityIndex("G04_L1C"), 0);
    EXPECT_EQ(sm.getAmbiguityIndex("G01_L1C"), -1);
    EXPECT_EQ(sm.getAmbiguityIndex("G03_L1C"), -1);
}

TEST(StateManagerTest, RenameAmbiguityRebindsKeyWithoutTouchingState) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t idx = sm.addAmbiguity("G01_L1C", 12.5);
    Eigen::VectorXd x_before = sm.getStateVector();

    ASSERT_TRUE(sm.renameAmbiguity("G01_L1C", "DD|GPS|G05|L1|G01"));

    // The same row index is now keyed by the new name, value unchanged.
    EXPECT_EQ(sm.getAmbiguityIndex("DD|GPS|G05|L1|G01"),
              static_cast<int>(idx));
    EXPECT_EQ(sm.getAmbiguityIndex("G01_L1C"), -1);
    EXPECT_TRUE(sm.getStateVector().isApprox(x_before, 1e-12));
}

TEST(StateManagerTest, RenameAmbiguityRefusesCollisionAndMissingKey) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 1.0);
    sm.addAmbiguity("G02_L1C", 2.0);

    // New key already exists — refuse.
    EXPECT_FALSE(sm.renameAmbiguity("G01_L1C", "G02_L1C"));
    EXPECT_GE(sm.getAmbiguityIndex("G01_L1C"), 0);
    EXPECT_GE(sm.getAmbiguityIndex("G02_L1C"), 0);

    // Old key not present — refuse.
    EXPECT_FALSE(sm.renameAmbiguity("does_not_exist", "G99_L1C"));
}

// ============================================================================
// Ionosphere bookkeeping
// ============================================================================

TEST(StateManagerTest, AddIonosphereStateAndLookup) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t before = sm.getStateSize();
    const size_t idx = sm.addIonosphereState("G01", /*initial=*/2.5);

    EXPECT_EQ(sm.getStateSize(), before + 1);
    EXPECT_EQ(idx, before);
    EXPECT_EQ(sm.getIonosphereIndex("G01"), static_cast<int>(idx));
    EXPECT_EQ(sm.getIonosphereIndex("G99"), -1);
}

TEST(StateManagerTest, PruneIonosphereStatesExceptRemovesUnlisted) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addIonosphereState("G01", 0.5);
    sm.addIonosphereState("G02", 0.5);
    sm.addIonosphereState("G03", 0.5);

    sm.pruneIonosphereStatesExcept({ "G02" });

    EXPECT_EQ(sm.getIonosphereStates().size(), 1u);
    EXPECT_GE(sm.getIonosphereIndex("G02"), 0);
    EXPECT_EQ(sm.getIonosphereIndex("G01"), -1);
    EXPECT_EQ(sm.getIonosphereIndex("G03"), -1);
}

// ============================================================================
// Code-bias / sat-bias bookkeeping
// ============================================================================

TEST(StateManagerTest, AddCodeBiasStateAndLookup) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t idx = sm.addCodeBiasState("GPS_IFB2", 0.0);
    EXPECT_EQ(sm.getCodeBiasIndex("GPS_IFB2"), static_cast<int>(idx));
    EXPECT_EQ(sm.getCodeBiasIndex("Galileo_DCB"), -1);
}

TEST(StateManagerTest, PruneCodeBiasStatesExcept) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addCodeBiasState("GPS_IFB2", 0.0);
    sm.addCodeBiasState("GPS_IFB5", 0.0);
    sm.addCodeBiasState("Galileo_DCB", 0.0);

    sm.pruneCodeBiasStatesExcept({ "GPS_IFB5" });

    EXPECT_EQ(sm.getCodeBiasStates().size(), 1u);
    EXPECT_GE(sm.getCodeBiasIndex("GPS_IFB5"), 0);
    EXPECT_EQ(sm.getCodeBiasIndex("GPS_IFB2"), -1);
    EXPECT_EQ(sm.getCodeBiasIndex("Galileo_DCB"), -1);
}

// The network code-bias consumer re-inflation (H1): on a consumed satellite
// code-bias datum discontinuity, the receiver code-bias state must keep its
// value (the hardware delay didn't change) while its variance is reset and its
// cross-covariance with other states is broken, so the next update re-absorbs
// the common-mode step in one epoch.
TEST(StateManagerTest, ReinflateCodeBiasVariancePreservesValueBreaksCovariance) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const int idx_a = static_cast<int>(sm.addCodeBiasState("GPS_DCB", 0.0));
    const int idx_b = static_cast<int>(sm.addCodeBiasState("GPS_IFB5", 0.0));

    Eigen::VectorXd x = sm.getStateVector();
    Eigen::MatrixXd P = sm.getCovariance();
    x(idx_a)            = 0.42;
    P(idx_a, idx_a)     = 4.0;
    P(idx_b, idx_b)     = 9.0;
    P(idx_a, idx_b)     = 1.5;
    P(idx_b, idx_a)     = 1.5;
    sm.setStateVector(x);
    sm.setCovariance(P);

    sm.reinflateCodeBiasVariance("GPS_DCB", 0.25);

    const Eigen::VectorXd x2 = sm.getStateVector();
    const Eigen::MatrixXd P2 = sm.getCovariance();
    EXPECT_DOUBLE_EQ(x2(idx_a), 0.42);        // value preserved
    EXPECT_DOUBLE_EQ(P2(idx_a, idx_a), 0.25); // diagonal reset
    EXPECT_DOUBLE_EQ(P2(idx_a, idx_b), 0.0);  // cross-covariance broken
    EXPECT_DOUBLE_EQ(P2(idx_b, idx_a), 0.0);
    EXPECT_DOUBLE_EQ(P2(idx_b, idx_b), 9.0);  // neighbour untouched
}

TEST(StateManagerTest, ReinflateCodeBiasVarianceRefusesFrozenAndAbsent) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const int idx = static_cast<int>(sm.addCodeBiasState("GPS_DCB", 0.0));
    Eigen::VectorXd x = sm.getStateVector();
    Eigen::MatrixXd P = sm.getCovariance();
    x(idx)        = 0.7;
    P(idx, idx)   = 0.0;   // frozen row (guards a BSSD-held / GLONASS per-slot column)
    sm.setStateVector(x);
    sm.setCovariance(P);

    sm.reinflateCodeBiasVariance("GPS_DCB", 0.25);   // refused
    EXPECT_DOUBLE_EQ(sm.getCovariance()(idx, idx), 0.0);
    EXPECT_DOUBLE_EQ(sm.getStateVector()(idx), 0.7);

    const size_t sz = sm.getStateSize();
    sm.reinflateCodeBiasVariance("Galileo_IFB7", 0.25);  // absent key: no-op, no throw
    EXPECT_EQ(sm.getStateSize(), sz);
}

TEST(StateManagerTest, AddSatBiasStateAndLookup) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    const size_t idx = sm.addSatBiasState("G01|C1W", 0.0);
    EXPECT_EQ(sm.getSatBiasIndex("G01|C1W"), static_cast<int>(idx));
    EXPECT_EQ(sm.getSatBiasIndex("G99|C1W"), -1);
}

TEST(StateManagerTest, PruneSatBiasStatesExcept) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addSatBiasState("G01|C1W", 0.0);
    sm.addSatBiasState("G02|C1W", 0.0);
    sm.addSatBiasState("E11|C5Q", 0.0);

    sm.pruneSatBiasStatesExcept({ "E11|C5Q", "G01|C1W" });

    EXPECT_EQ(sm.getSatBiasStates().size(), 2u);
    EXPECT_GE(sm.getSatBiasIndex("E11|C5Q"), 0);
    EXPECT_GE(sm.getSatBiasIndex("G01|C1W"), 0);
    EXPECT_EQ(sm.getSatBiasIndex("G02|C1W"), -1);
}

// ============================================================================
// Unresolvable / pending-reseed bookkeeping
// ============================================================================

TEST(StateManagerTest, UnresolvableAmbiguitySetRoundTrip) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    EXPECT_FALSE(sm.isAmbiguityUnresolvable("G01_L1C"));
    sm.markAmbiguityUnresolvable("G01_L1C");
    EXPECT_TRUE(sm.isAmbiguityUnresolvable("G01_L1C"));
    EXPECT_FALSE(sm.isAmbiguityUnresolvable("G02_L1C"));

    sm.clearUnresolvableAmbiguities();
    EXPECT_FALSE(sm.isAmbiguityUnresolvable("G01_L1C"));
}

TEST(StateManagerTest, PendingReseedMarkAndClear) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 0.0);
    EXPECT_FALSE(sm.isAmbiguityPendingReseed("G01_L1C"));

    sm.markAmbiguityPendingReseed("G01_L1C");
    EXPECT_TRUE(sm.isAmbiguityPendingReseed("G01_L1C"));

    sm.clearPendingReseed("G01_L1C");
    EXPECT_FALSE(sm.isAmbiguityPendingReseed("G01_L1C"));
}

// ============================================================================
// Process-noise / transition matrix shape
// ============================================================================

TEST(StateManagerTest, ProcessNoiseMatrixIsSquareAndStateSized) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 0.0);
    sm.addAmbiguity("G02_L1C", 0.0);

    Eigen::MatrixXd Q = sm.getProcessNoiseMatrix(/*dt=*/1.0);
    const Eigen::Index n = static_cast<Eigen::Index>(sm.getStateSize());

    EXPECT_EQ(Q.rows(), n);
    EXPECT_EQ(Q.cols(), n);
    EXPECT_TRUE(Q.allFinite());
    // Diagonal entries must be non-negative (variances).
    for (Eigen::Index i = 0; i < n; ++i) {
        EXPECT_GE(Q(i, i), 0.0) << "Q(" << i << "," << i << ") negative";
    }
}

TEST(StateManagerTest, ProcessNoiseRejectsNonPositiveDt) {
    StateManagerHarness h;
    EXPECT_THROW(h.state_manager.getProcessNoiseMatrix(0.0),
                 std::invalid_argument);
    EXPECT_THROW(h.state_manager.getProcessNoiseMatrix(-1.0),
                 std::invalid_argument);
}

TEST(StateManagerTest, TransitionMatrixIsSquareAndStateSized) {
    StateManagerHarness h;
    auto& sm = h.state_manager;

    sm.addAmbiguity("G01_L1C", 0.0);
    Eigen::MatrixXd F = sm.getTransitionMatrix(/*dt=*/1.0);
    const Eigen::Index n = static_cast<Eigen::Index>(sm.getStateSize());

    EXPECT_EQ(F.rows(), n);
    EXPECT_EQ(F.cols(), n);
    EXPECT_TRUE(F.allFinite());
}

}  // namespace
}  // namespace GnssProcessor
