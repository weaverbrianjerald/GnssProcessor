/**
 * @file test_pb_translation_premise.cpp
 * @brief Counterexamples to treating every product update as a datum change.
 *
 * Exercises the production translation ledger. Observations are specified
 * independently using phase = geometry - satellite_clock + bias + ambiguity.
 * These are reduced-model premise tests, not full-engine positioning tests.
 */
#include <gtest/gtest.h>
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "realtime/PbStepTranslateLedger.hpp"
#include <Eigen/Dense>

using namespace GnssProcessor;

namespace {
constexpr const char* kKey = "G12_L1W";
constexpr const char* kLedgerKey = "G12|L1W";
constexpr double kDelta = 0.30;

struct Bed {
    LogManager log;
    GnssConfig config;
    StateManager state{5, log, config};
    PbStepTranslateLedger ledger;
    int index;

    explicit Bed(double ambiguity = 12.0) {
        index = static_cast<int>(state.addAmbiguity(kKey, ambiguity, 0.01));
        ledger.record(kLedgerKey, true, 0.0);
    }
    double ambiguity() const { return state.getStateVector()(index); }
    PbStepTranslateLedger::Decision apply(double bias, bool declared = false) {
        auto result = ledger.react(state, kLedgerKey, kKey, declared, true, bias,
                                   false, false, 0.005, 100.0);
        ledger.record(kLedgerKey, true, bias);
        return result;
    }
};

// All fixed geometry/atmosphere terms have been subtracted from observation
// and prediction. The remaining signs match MeasurementModel's phase row.
double residual(double observation, double ambiguity, double bias, double clock = 0.0) {
    return observation - (-clock + bias + ambiguity);
}
}

TEST(PbTranslationPremiseTest, KnownDatumChangePreservesObservationAndPrediction) {
    Bed b;
    const auto covariance = b.state.getCovariance();
    const auto decision = b.apply(kDelta, true);
    EXPECT_EQ(decision.action, PbStepTranslateLedger::Action::TranslateDeclared);
    EXPECT_NEAR(b.ambiguity(), 12.0 - kDelta, 1e-12);
    EXPECT_NEAR(residual(12.0, b.ambiguity(), kDelta), 0.0, 1e-12);
    EXPECT_TRUE((covariance.array() == b.state.getCovariance().array()).all());
}

TEST(PbTranslationPremiseTest, CorrectPhysicalBiasUpdateNeedsNoAmbiguityTranslation) {
    for (bool declared : {false, true}) {
        Bed b;
        // Hardware bias and observed phase increase together. The true
        // carrier ambiguity and datum stay fixed at 12 m.
        const double observed = 12.0 + kDelta;
        EXPECT_NEAR(residual(observed, b.ambiguity(), kDelta), 0.0, 1e-12);
        b.apply(kDelta, declared);
        EXPECT_NEAR(residual(observed, b.ambiguity(), kDelta), kDelta, 1e-12);
        EXPECT_NEAR(b.ambiguity() - 12.0, -kDelta, 1e-12);
    }
}

TEST(PbTranslationPremiseTest, PhysicalBiasResidualCanMoveGeometryWithAStiffAmbiguity) {
    Bed b;
    b.apply(kDelta);
    // Reduced two-state update: one range/geometry parameter and an ambiguity.
    // Code observes geometry; phase observes geometry + ambiguity. The other
    // GNSS terms are held known. This demonstrates possible leakage, not an
    // ENU prediction for the field fixture.
    Eigen::Matrix2d H; H << 1, 0, 1, 1;
    Eigen::Matrix2d P = Eigen::Matrix2d::Zero(); P(0, 0) = 1; P(1, 1) = 1e-4;
    Eigen::Matrix2d R = Eigen::Matrix2d::Zero(); R(0, 0) = 0.25; R(1, 1) = 25e-6;
    const Eigen::Matrix2d S = H * P * H.transpose() + R;
    const Eigen::Vector2d innovation(0, residual(12.0 + kDelta, b.ambiguity(), kDelta));
    const Eigen::Vector2d update = P * H.transpose() * S.ldlt().solve(innovation);
    EXPECT_NEAR(update(0), 0.2998126171, 1e-9);
    EXPECT_LT(std::abs(update(1)), 0.001);
}

TEST(PbTranslationPremiseTest, ImprovedEstimateCanCorrectErrorThatTranslationPreserves) {
    Bed b; // An independently accurate prior ambiguity, in a fixed datum.
    const double observed = 12.0 + kDelta; // Hardware bias was already 0.30 m.
    EXPECT_NEAR(residual(observed, b.ambiguity(), 0.0), kDelta, 1e-12);
    EXPECT_NEAR(residual(observed, b.ambiguity(), kDelta), 0.0, 1e-12);
    b.apply(kDelta); // The improved product is now accurate.
    EXPECT_NEAR(residual(observed, b.ambiguity(), kDelta), kDelta, 1e-12);
}

TEST(PbTranslationPremiseTest, ImprovedEstimateCanAlsoRemoveBiasAlreadyAbsorbedByAmbiguity) {
    Bed b(12.0 + kDelta); // Old product error was fully absorbed in the float.
    const double observed = 12.0 + kDelta;
    EXPECT_NEAR(residual(observed, b.ambiguity(), 0.0), 0.0, 1e-12);
    b.apply(kDelta);
    EXPECT_NEAR(b.ambiguity(), 12.0, 1e-12);
    EXPECT_NEAR(residual(observed, b.ambiguity(), kDelta), 0.0, 1e-12);
    // The same product delta therefore does not identify the proper response.
}

TEST(PbTranslationPremiseTest, ClockBiasRedistributionNeedsTheCombinedCorrection) {
    Bed b;
    // Clock, code bias and phase bias all increase by delta. Both code and
    // phase totals (-clock + bias) are unchanged; no consumer state need move.
    EXPECT_NEAR(residual(12.0, b.ambiguity(), kDelta, kDelta), 0.0, 1e-12);
    b.apply(kDelta, true);
    EXPECT_NEAR(residual(12.0, b.ambiguity(), kDelta, kDelta), kDelta, 1e-12);
}

TEST(PbTranslationPremiseTest, DeadBandMakesSameDatumTrajectoryDependOnUpdatePartition) {
    Bed small_steps;
    for (int i = 1; i <= 10; ++i) {
        const auto d = small_steps.apply(0.004 * i);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::SkippedEps);
    }
    Bed one_step;
    one_step.apply(0.04);
    EXPECT_NEAR(residual(12.0, small_steps.ambiguity(), 0.04), -0.04, 1e-12);
    EXPECT_NEAR(residual(12.0, one_step.ambiguity(), 0.04), 0.0, 1e-12);
}
