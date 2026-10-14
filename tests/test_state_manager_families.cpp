/**
 * @file test_state_manager_families.cpp
 * @brief The family table's behaviour columns against StateManager's code (card UOM
 *        steps U6e and U6f).
 *
 * kStateFamilies declares, per dynamic family, its re-inflation counter, its gap
 * response, the rule that names the satellite of a key and its process-noise field
 * (U6e), and its birth gate and initial-variance source (U6f).  The lists that read the
 * table (the gap edit, the frozen-pivot Q skip, the satellite random walks, the run-end
 * names) are exercised here, and every declared value that the code does not read from
 * the table is compared with what the code does, so the table cannot drift from the
 * code unnoticed.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <map>
#include <string>

using namespace GnssProcessor;

namespace {

constexpr double kDt = 30.0;

/// Default configuration (PPP, no BSSD, no uncombined processing).
struct Harness {
    LogManager   log_manager;
    GnssConfig   config;
    StateManager sm{ 5, log_manager, config };
};

/// Add one state of @p f for satellite G05 (or the receiver) and return its key.
std::string addOne(StateManager& sm, StateFamily f) {
    switch (f) {
        case StateFamily::Ionosphere:        sm.addIonosphereState("G05", 0.0);          return "G05";
        case StateFamily::IonosphereRate:    return "";   // needs enable_iono_rate
        case StateFamily::Ambiguity:         sm.addAmbiguity("G05_L1C", 0.0);            return "G05_L1C";
        case StateFamily::ReceiverCodeBias:  sm.addCodeBiasState("GPS_IFB5", 0.0);       return "GPS_IFB5";
        case StateFamily::SatBias:           sm.addSatBiasState("G05|C1W", 0.0);         return "G05|C1W";
        case StateFamily::SatPhaseBias:      sm.addSatPhaseBiasState("G05|L1C|PB", 0.0); return "G05|L1C|PB";
        case StateFamily::ReceiverPhaseBias: sm.addReceiverPhaseBiasState("GPS_RPB_L1C", 0.0); return "GPS_RPB_L1C";
        case StateFamily::PhaseColour:       sm.addPhaseColourState("G05", 0.25);        return "G05";
        case StateFamily::SatClock:          sm.addSatClockState("G05|SC", 0.0);         return "G05|SC";
    }
    return "";
}

}  // namespace

TEST(StateManagerFamiliesTest, SatelliteOfKeyReadsEachKeyForm) {
    EXPECT_EQ(satelliteOfKey(SatOfKey::Bare, "G05"), std::optional<std::string>("G05"));
    EXPECT_EQ(satelliteOfKey(SatOfKey::BeforeUnderscore, "G05_L1C_L2W"), std::optional<std::string>("G05"));
    EXPECT_FALSE(satelliteOfKey(SatOfKey::BeforeUnderscore, "DD|GPS|G05|L1C|G01").has_value());
    EXPECT_EQ(satelliteOfKey(SatOfKey::BeforePipe, "G05|C1W"), std::optional<std::string>("G05"));
    EXPECT_EQ(satelliteOfKey(SatOfKey::BeforePipe, "G05"), std::optional<std::string>("G05"));
    EXPECT_EQ(satelliteOfKey(SatOfKey::SatClockSuffix, "G05|SC"), std::optional<std::string>("G05"));
    EXPECT_EQ(satelliteOfKey(SatOfKey::SatClockSuffix, "G05"), std::optional<std::string>("G05"));
    EXPECT_FALSE(satelliteOfKey(SatOfKey::None, "GPS_IFB5").has_value());
    // The phase-bias family reads its satellite with the code-bias split; the
    // schema's own phase-bias split is the same function.
    for (const char* k : { "G05|L1C|PB", "E11|L5Q|PB", "G05", "|x|PB" })
        EXPECT_EQ(StateKeys::satelliteFromSatBiasKey(k), StateKeys::satelliteFromSatPhaseBiasKey(k)) << k;
}

TEST(StateManagerFamiliesTest, RunEndNamesAreTheReinflationNames) {
    const std::string s = StateManager::reinflateSummary(StateManager::ReinflateCounters{});
    EXPECT_EQ(s, "code_bias raised=0 kept=0 | isb raised=0 kept=0 | iono raised=0 kept=0 | "
                 "iono_rate raised=0 kept=0 | sat_bias raised=0 kept=0 | "
                 "sat_phase_bias raised=0 kept=0 | sat_clock raised=0 kept=0");
}

TEST(StateManagerFamiliesTest, EachReinflationSlotIsTheCounterTheFamilyRaises) {
    Harness h;
    addOne(h.sm, StateFamily::Ionosphere);
    addOne(h.sm, StateFamily::ReceiverCodeBias);
    addOne(h.sm, StateFamily::SatBias);
    addOne(h.sm, StateFamily::SatPhaseBias);
    addOne(h.sm, StateFamily::SatClock);
    h.sm.reinflateIonosphereVariance("G05", 1.0e6);
    h.sm.reinflateCodeBiasVariance("GPS_IFB5", 1.0e6);
    h.sm.reinflateSatBiasVariancesForSat("G05", 1.0e6);
    h.sm.reinflateSatPhaseBiasVariancesForSat("G05", 1.0e6);
    h.sm.reinflateSatClockVarianceForSat("G05", 1.0e9);
    for (StateFamily f : { StateFamily::Ionosphere, StateFamily::ReceiverCodeBias,
                           StateFamily::SatBias, StateFamily::SatPhaseBias, StateFamily::SatClock }) {
        const auto slot = static_cast<std::size_t>(specOf(f).reinflation);
        ASSERT_LT(slot, h.sm.reinflateCounters().size()) << specOf(f).name;
        EXPECT_EQ(h.sm.reinflateCounters()[slot].raised, 1) << specOf(f).name;
    }
    EXPECT_EQ(specOf(StateFamily::Ambiguity).reinflation, kNoReinflation);
}

TEST(StateManagerFamiliesTest, AGapDecouplesExactlyTheDeclaredFamilies) {
    Harness h;
    std::map<StateFamily, Eigen::Index> idx;
    for (const auto& s : kStateFamilies) {
        const std::string key = addOne(h.sm, s.family);
        if (!key.empty()) idx[s.family] = static_cast<Eigen::Index>(h.sm.indicesOf(s.family).at(key));
    }
    Eigen::MatrixXd P = h.sm.getCovariance();
    for (const auto& [f, i] : idx) { P(0, i) = 0.5; P(i, 0) = 0.5; }
    h.sm.setCovariance(P);
    h.sm.zeroPositionCrossCovariancesToSatStates();
    const Eigen::MatrixXd Pz = h.sm.getCovariance();
    for (const auto& [f, i] : idx)
        EXPECT_EQ(Pz(0, i), specOf(f).gap_decouple ? 0.0 : 0.5) << specOf(f).name;
}

TEST(StateManagerFamiliesTest, DeclaredRandomWalkIsWhatTheBuilderWrites) {
    Harness h;
    std::map<StateFamily, std::string> keys;
    for (const auto& s : kStateFamilies) keys[s.family] = addOne(h.sm, s.family);
    const Eigen::MatrixXd Q = h.sm.getProcessNoiseMatrix(kDt);
    const auto& pn = h.config.getProcessNoise();
    for (const auto& s : kStateFamilies) {
        if (s.q_rule != QRule::RandomWalk) continue;
        const auto i = static_cast<Eigen::Index>(h.sm.indicesOf(s.family).at(keys[s.family]));
        EXPECT_DOUBLE_EQ(Q(i, i), pn.*s.q_psd * kDt) << s.name;
    }
    for (const auto& s : kStateFamilies) {
        if (s.q_rule != QRule::None || keys[s.family].empty()) continue;
        const auto i = static_cast<Eigen::Index>(h.sm.indicesOf(s.family).at(keys[s.family]));
        EXPECT_EQ(Q(i, i), 0.0) << s.name;
    }
}

TEST(StateManagerFamiliesTest, FrozenPivotSkipFollowsTheSatelliteOfKeyRule) {
    Harness h;
    std::map<StateFamily, std::string> keys;
    for (const auto& s : kStateFamilies) keys[s.family] = addOne(h.sm, s.family);
    h.sm.setFrozenPivotSatellites({ "G05" });
    const Eigen::MatrixXd Q = h.sm.getProcessNoiseMatrix(kDt);
    const auto& pn = h.config.getProcessNoise();
    for (const auto& s : kStateFamilies) {
        if (s.q_rule != QRule::RandomWalk) continue;
        const auto i = static_cast<Eigen::Index>(h.sm.indicesOf(s.family).at(keys[s.family]));
        const bool skipped = s.sat_of_key != SatOfKey::None;
        EXPECT_EQ(Q(i, i), skipped ? 0.0 : pn.*s.q_psd * kDt) << s.name;
    }
}
