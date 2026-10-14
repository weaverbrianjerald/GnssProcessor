/**
 * @file test_state_manager_process_noise.cpp
 * @brief The blocks of StateManager's process noise matrix (card UOM step U6c).
 *
 * getProcessNoiseMatrix is a driver over one function per block of Q.  These
 * cases pin what each block writes on a default (PPP) configuration: the random
 * walk of each satellite family and of the ambiguities, the per-key ambiguity
 * override, the frozen-pivot skip (the satellite of each family's key), and that
 * a key naming no satellite (a DD ambiguity) is never skipped.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <map>
#include <set>
#include <string>

using namespace GnssProcessor;

namespace {

constexpr double kDt = 30.0;

/// Default configuration (PPP, no BSSD): a manager with one state of each
/// satellite family for G01 and G02 and two ambiguities.
struct Harness {
    LogManager   log_manager;
    GnssConfig   config;
    StateManager sm{ 5, log_manager, config };

    Harness() {
        sm.addSatBiasState("G01|C1W", 0.0);
        sm.addSatBiasState("G02|C1W", 0.0);
        sm.addSatPhaseBiasState("G01|L1C|PB", 0.0);
        sm.addSatPhaseBiasState("G02|L1C|PB", 0.0);
        sm.addSatClockState("G01|SC", 0.0);
        sm.addSatClockState("G02|SC", 0.0);
        sm.addAmbiguity("G01_L1C", 0.0);
        sm.addAmbiguity("G02_L1C", 0.0);
        sm.addCodeBiasState("GPS_IFB5", 0.0);
    }

    double q(const std::map<std::string, size_t>& map, const std::string& key) {
        const Eigen::MatrixXd Q = sm.getProcessNoiseMatrix(kDt);
        const auto idx = static_cast<Eigen::Index>(map.at(key));
        return Q(idx, idx);
    }
};

}  // namespace

TEST(StateManagerProcessNoiseTest, EachSatelliteFamilyGetsItsRandomWalk) {
    Harness h;
    const auto& pn = h.config.getProcessNoise();
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatBiasStates(), "G01|C1W"), pn.sat_bias * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatPhaseBiasStates(), "G01|L1C|PB"), pn.sat_phase_bias * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatClockStates(), "G01|SC"), pn.sat_clock * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getAmbiguities(), "G01_L1C"), pn.ambiguity * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getCodeBiasStates(), "GPS_IFB5"), pn.code_bias * kDt);
}

TEST(StateManagerProcessNoiseTest, AFrozenPivotsSatelliteStatesGetNoQ) {
    Harness h;
    h.sm.setFrozenPivotSatellites({ "G01" });
    const auto& pn = h.config.getProcessNoise();
    EXPECT_EQ(h.q(h.sm.getSatBiasStates(), "G01|C1W"), 0.0);
    EXPECT_EQ(h.q(h.sm.getSatPhaseBiasStates(), "G01|L1C|PB"), 0.0);
    EXPECT_EQ(h.q(h.sm.getSatClockStates(), "G01|SC"), 0.0);
    EXPECT_EQ(h.q(h.sm.getAmbiguities(), "G01_L1C"), 0.0);
    // The other satellite keeps its random walk; the receiver code bias names no
    // satellite and is never skipped.
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatBiasStates(), "G02|C1W"), pn.sat_bias * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatPhaseBiasStates(), "G02|L1C|PB"), pn.sat_phase_bias * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getSatClockStates(), "G02|SC"), pn.sat_clock * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getAmbiguities(), "G02_L1C"), pn.ambiguity * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getCodeBiasStates(), "GPS_IFB5"), pn.code_bias * kDt);
}

TEST(StateManagerProcessNoiseTest, AnAmbiguityKeyWithoutUnderscoreIsNeverSkipped) {
    // A DD key names no satellite before an underscore, so no frozen set skips it.
    Harness h;
    h.sm.addAmbiguity("DD|GPS|G05|L1C|G01", 0.0);
    h.sm.setFrozenPivotSatellites({ "DD|GPS|G05|L1C|G01", "G01", "G05" });
    const auto& pn = h.config.getProcessNoise();
    EXPECT_DOUBLE_EQ(h.q(h.sm.getAmbiguities(), "DD|GPS|G05|L1C|G01"), pn.ambiguity * kDt);
}

TEST(StateManagerProcessNoiseTest, AmbiguityOverrideReplacesTheConfiguredRandomWalk) {
    Harness h;
    h.sm.setAmbiguityProcessNoiseOverrides({ { "G02_L1C", 4.0e-6 } });
    const auto& pn = h.config.getProcessNoise();
    EXPECT_DOUBLE_EQ(h.q(h.sm.getAmbiguities(), "G02_L1C"), 4.0e-6 * kDt);
    EXPECT_DOUBLE_EQ(h.q(h.sm.getAmbiguities(), "G01_L1C"), pn.ambiguity * kDt);
}

TEST(StateManagerProcessNoiseTest, QIsSymmetricAndTheCoreBlocksAreFilled) {
    Harness h;
    const Eigen::MatrixXd Q = h.sm.getProcessNoiseMatrix(kDt);
    EXPECT_TRUE(Q.isApprox(Q.transpose()));
    const auto& pn = h.config.getProcessNoise();
    EXPECT_DOUBLE_EQ(Q(h.sm.getTropoIndex(), h.sm.getTropoIndex()), pn.troposphere * kDt);
    if (h.sm.hasClock() && !h.sm.hasClockRate())
        EXPECT_DOUBLE_EQ(Q(h.sm.getClockIndex(), h.sm.getClockIndex()), pn.clock * kDt);
}
