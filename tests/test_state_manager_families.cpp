/**
 * @file test_state_manager_families.cpp
 * @brief The family table's behaviour columns against StateManager's code.
 *
 * kStateFamilies declares, per dynamic family, its re-inflation counter, its gap
 * response, the rule that names the satellite of a key and its process-noise field,
 * and its birth gate and initial-variance source.  The lists that read the
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
#include <filesystem>
#include <fstream>
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

TEST(StateManagerFamiliesTest, ArmedTableHasATitleAndOneLinePerFamily) {
    Harness h;
    const auto lines = h.sm.familiesAsArmed();
    ASSERT_EQ(lines.size(), 1 + kStateFamilyCount);
    EXPECT_EQ(lines[0].rfind("State families as armed", 0), 0u);
    for (std::size_t i = 0; i < kStateFamilyCount; ++i) {
        const StateFamilySpec& s = kStateFamilies[i];
        const std::string& line = lines[1 + i];
        EXPECT_NE(line.find(std::string(s.name) + ":"), std::string::npos) << line;
        EXPECT_NE(line.find(std::string("label ") + s.label_prefix), std::string::npos) << line;
        EXPECT_NE(line.find(std::string("snapshot ") + (s.snapshot_tag ? s.snapshot_tag : "none")),
                  std::string::npos) << line;
    }
}

TEST(StateManagerFamiliesTest, ArmedTableReportsTheBirthGateAndTheEvents) {
    Harness h;   // default configuration: not the network producer
    const auto lines = h.sm.familiesAsArmed();
    auto lineOf = [&](StateFamily f) { return lines[1 + static_cast<std::size_t>(f)]; };
    EXPECT_NE(lineOf(StateFamily::Ambiguity).find("born yes"), std::string::npos);
    EXPECT_NE(lineOf(StateFamily::SatPhaseBias).find("born no"), std::string::npos);
    EXPECT_NE(lineOf(StateFamily::SatClock).find("born no"), std::string::npos);
    EXPECT_NE(lineOf(StateFamily::Ionosphere).find(h.config.isUncombinedEnabled() ? "born yes" : "born no"),
              std::string::npos);
    EXPECT_NE(lineOf(StateFamily::SatBias).find("re-inflation sat_bias"), std::string::npos);
    EXPECT_NE(lineOf(StateFamily::SatBias).find("prune (to the warm-restart cache)"), std::string::npos);
    EXPECT_NE(lineOf(StateFamily::Ambiguity).find("frozen-pivot Q skip"), std::string::npos);
    EXPECT_EQ(lineOf(StateFamily::ReceiverCodeBias).find("frozen-pivot Q skip"), std::string::npos);
    EXPECT_EQ(lineOf(StateFamily::PhaseColour).find("RT snapshot"), std::string::npos);
}

TEST(StateManagerFamiliesTest, DeclaredInitialStdIsTheVarianceTheAddFunctionsWrite) {
    Harness h;
    std::map<StateFamily, std::string> keys;
    for (const auto& s : kStateFamilies) keys[s.family] = addOne(h.sm, s.family);
    const Eigen::MatrixXd P = h.sm.getCovariance();
    const auto& init = h.config.getInitialStd();
    for (const auto& s : kStateFamilies) {
        if (keys[s.family].empty()) continue;
        const auto i = static_cast<Eigen::Index>(h.sm.indicesOf(s.family).at(keys[s.family]));
        switch (s.initial_std_rule) {
            case InitialStdRule::ConfigField: {
                const double sd = init.*s.initial_std;
                EXPECT_DOUBLE_EQ(P(i, i), sd * sd) << s.name;
                break;
            }
            case InitialStdRule::AmbiguityStd: {
                const double sd = h.config.getAmbiguityStd();
                EXPECT_DOUBLE_EQ(P(i, i), sd * sd) << s.name;
                break;
            }
            case InitialStdRule::CallerSupplied:
                EXPECT_DOUBLE_EQ(P(i, i), 0.25) << s.name;   // addOne passes 0.25
                break;
        }
    }
}

namespace {

/// A configuration loaded from @p json through the loader, from a file in a directory
/// unique to this process and test (so concurrent test runs cannot collide).
GnssConfig loadConfig(const std::string& json, const std::string& tag) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path()
        / ("uom_families_" + tag + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "config.json";
    { std::ofstream out(file); out << json; }
    LogManager log;
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(file.string(), log)) << tag;
    fs::remove_all(dir, ec);
    return cfg;
}

/// The WARNING lines of a table.
std::vector<std::string> warnings(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (const auto& l : lines) if (l.rfind("WARNING: ", 0) == 0) out.push_back(l);
    return out;
}

}  // namespace

TEST(StateManagerFamiliesTest, ArmedTableWarnsWhenABornFamilyGetsNoProcessNoise) {
    // PPP with uncombined processing: the slant ionosphere is born and observed, but its
    // Q applies in PPP_RTK mode only.  The table states it.
    LogManager lm;
    const GnssConfig ppp = loadConfig(
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true}})", "ppp_unc");
    StateManager sm_ppp{ 5, lm, ppp };
    const auto lines = sm_ppp.familiesAsArmed();
    const auto warn = warnings(lines);
    ASSERT_EQ(warn.size(), 1u);
    EXPECT_NE(warn[0].find("slant ionosphere"), std::string::npos) << warn[0];
    EXPECT_NE(lines[1 + static_cast<std::size_t>(StateFamily::Ionosphere)].find("NOT applied on this run"),
              std::string::npos);

    // The same in PPP_RTK mode: the Q applies and nothing is flagged.
    const GnssConfig rtk = loadConfig(
        R"({"processing": {"mode": "PPP-RTK", "uncombined_enabled": true}})", "rtk_unc");
    StateManager sm_rtk{ 5, lm, rtk };
    const auto rtk_lines = sm_rtk.familiesAsArmed();
    EXPECT_TRUE(warnings(rtk_lines).empty());
    EXPECT_NE(rtk_lines[1 + static_cast<std::size_t>(StateFamily::Ionosphere)].find(", applied on this run"),
              std::string::npos);

    // Baseline: the DD layout never creates ionosphere states, so nothing is flagged and
    // the ionosphere family is reported as not born, whatever uncombined_enabled says.
    const GnssConfig dd = loadConfig(
        R"({"baseline_fixture": "IGS_Pasadena",
                "processing": {"mode": "Baseline-Single", "uncombined_enabled": true},
                "baseline": {"reference": {"name": "", "rinex_path": "", "fixed": true},
                             "rover": {"name": "", "rinex_path": ""}}})", "dd_unc");
    ASSERT_TRUE(dd.isBaselineMode());
    StateManager sm_dd{ 3, lm, dd };
    const auto dd_lines = sm_dd.familiesAsArmed();
    EXPECT_TRUE(warnings(dd_lines).empty());
    EXPECT_NE(dd_lines[1 + static_cast<std::size_t>(StateFamily::Ionosphere)].find("born no"),
              std::string::npos);
}
