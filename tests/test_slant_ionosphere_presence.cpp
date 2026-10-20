/**
 * @file test_slant_ionosphere_presence.cpp
 * @brief One predicate answers "this run carries slant-ionosphere states".
 *
 * The measurement model creates the per-satellite slant-ionosphere states on every
 * run with uncombined processing outside the baseline modes. Four places must agree
 * with that creation: the slant-ionosphere block of the process noise, the run-start
 * state-family table, the export of the states into PppState (and the write-back),
 * and the complete reset of the filter covariance (tests/test_kalman_complete_reset.cpp).
 * All of them ask GnssConfig::carriesSlantIonosphere. These cases build every mode
 * that can create the states and compare the answers of the process noise, the table
 * and the PppState export with the predicate, so a mode test added to one of them
 * alone breaks a case here. The case that motivated the predicate: PPP with
 * uncombined processing carried the states and gave them no process noise.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppState.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

constexpr double kDt          = 30.0;
constexpr double kElevationDeg = 30.0;   // sin^2 = 0.25
constexpr double kSeedValue    = 2.5;    // slant ionosphere written into the state, metres
constexpr double kWrittenBack  = 7.0;    // value a PppState write-back tries to set, metres

/// A configuration loaded from @p json through the loader, from a file in a directory
/// unique to this process and test.
GnssConfig loadConfig(const std::string& json, const std::string& tag) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / ("ionosphere_presence_" + tag);
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

/// One mode that can create (or must not create) the slant-ionosphere states.
struct ModeCase {
    const char* name;
    std::string json;                        ///< loaded through the loader
    std::optional<ProcessingMode> mode;      ///< set after the load (modes whose section needs inputs)
    bool carries;                            ///< the expected answer of the predicate
};

/// Every mode, with the uncombined flag the loader leaves it with.
std::vector<ModeCase> allModes() {
    const std::string ppp_unc = R"({"processing": {"mode": "PPP", "uncombined_enabled": true}})";
    return {
        { "PPP, uncombined",          ppp_unc, std::nullopt, true },
        { "PPP, ionosphere-free",     R"({"processing": {"mode": "PPP"}})", std::nullopt, false },
        { "PPP-RTK",                  R"({"processing": {"mode": "PPP-RTK"}})", std::nullopt, true },
        { "PPP-RTK, flag set false",  R"({"processing": {"mode": "PPP-RTK", "uncombined_enabled": false}})",
                                      std::nullopt, true },
        { "PPP-RTK-User",             R"({"processing": {"mode": "PPP-RTK-User"}})", std::nullopt, true },
        // The loader sections of the network and baseline modes need inputs a unit case
        // does not have, so the mode is set on a loaded uncombined configuration; the
        // loader forces the flag on in both, which is the state this reproduces.
        { "PPP-RTK-Network",          ppp_unc, ProcessingMode::PPP_RTK_Network, true },
        { "Baseline-Single",          ppp_unc, ProcessingMode::BaselineSingle,  false },
        { "Baseline-Multi",           ppp_unc, ProcessingMode::BaselineMulti,   false },
    };
}

GnssConfig buildConfig(const ModeCase& c) {
    GnssConfig cfg = loadConfig(c.json, "mode");
    if (c.mode) cfg.setProcessingMode(*c.mode);
    return cfg;
}

/// The slant-ionosphere row of a family table.
std::string ionosphereLine(const std::vector<std::string>& table) {
    return table.at(1 + static_cast<std::size_t>(StateFamily::Ionosphere));
}

/// The level Q the obliquity random walk writes for a satellite at kElevationDeg.
double expectedLevelQ(const GnssConfig& cfg) {
    const double s = std::sin(kElevationDeg * Constants::PI / 180.0);
    return (cfg.getProcessNoise().ionosphere / (s * s)) * kDt;
}

}  // namespace

TEST(SlantIonospherePresenceTest, ThePredicateIsTheUncombinedFlagOutsideTheBaselineModes) {
    for (const ModeCase& c : allModes()) {
        SCOPED_TRACE(c.name);
        const GnssConfig cfg = buildConfig(c);
        EXPECT_EQ(cfg.carriesSlantIonosphere(), c.carries);
        // Stated from the two inputs: the flag the loader leaves, and the baseline modes.
        EXPECT_EQ(cfg.carriesSlantIonosphere(), cfg.isUncombinedEnabled() && !cfg.isBaselineMode());
    }
}

TEST(SlantIonospherePresenceTest, TheProcessNoiseTheTableAndTheExportAgreeInEveryMode) {
    for (const ModeCase& c : allModes()) {
        SCOPED_TRACE(c.name);
        const GnssConfig cfg = buildConfig(c);
        ASSERT_GT(cfg.getProcessNoise().ionosphere, 0.0);

        LogManager lm;
        StateManager sm{ 5, lm, cfg };
        sm.setIonosphereElevation("G05", kElevationDeg);
        const size_t idx = sm.addIonosphereState("G05", kSeedValue);

        // The process noise: the obliquity random walk on a carried state, nothing otherwise.
        const Eigen::MatrixXd Q = sm.getProcessNoiseMatrix(kDt);
        if (c.carries) EXPECT_DOUBLE_EQ(Q(idx, idx), expectedLevelQ(cfg));
        else           EXPECT_EQ(Q(idx, idx), 0.0);

        // The run-start table: the family is born exactly when the rule applies, and the
        // table flags nothing.
        const auto table = sm.familiesAsArmed();
        const std::string line = ionosphereLine(table);
        EXPECT_NE(line.find(c.carries ? "born yes" : "born no"), std::string::npos) << line;
        EXPECT_NE(line.find(c.carries ? ", applied on this run" : ", NOT applied on this run"),
                  std::string::npos) << line;
        for (const std::string& l : table) EXPECT_NE(l.rfind("WARNING: ", 0), 0u) << l;

        // The PppState export and its write-back. The baseline layout has no clock or
        // troposphere state, and no driver exports a baseline state into a PppState.
        if (cfg.isBaselineMode()) continue;
        PppState ps;
        sm.updatePppStateFromVector(sm.getStateVector(), ps, sm);
        if (c.carries) {
            ASSERT_EQ(ps.slant_ionosphere.count("G05"), 1u);
            EXPECT_DOUBLE_EQ(ps.slant_ionosphere.at("G05"), kSeedValue);
        } else {
            EXPECT_TRUE(ps.slant_ionosphere.empty());
        }
        ps.slant_ionosphere["G05"] = kWrittenBack;
        sm.syncPppStateToStateVector(ps);
        EXPECT_DOUBLE_EQ(sm.getStateVector()(static_cast<Eigen::Index>(idx)),
                         c.carries ? kWrittenBack : kSeedValue);
    }
}

TEST(SlantIonospherePresenceTest, UncombinedPppGivesTheSlantIonosphereTheSameQAsPppRtk) {
    // The two runs differ only in the processing mode; the Q block of every state, not
    // only the ionosphere, is equal bit for bit.
    const GnssConfig ppp = loadConfig(
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true}})", "ppp_q");
    const GnssConfig rtk = loadConfig(
        R"({"processing": {"mode": "PPP-RTK", "uncombined_enabled": true}})", "rtk_q");
    LogManager lm;
    StateManager sm_ppp{ 5, lm, ppp };
    StateManager sm_rtk{ 5, lm, rtk };
    const std::map<std::string, double> elevations_deg{ { "G05", 80.0 }, { "G12", 50.0 }, { "E11", 12.0 } };
    for (StateManager* sm : { &sm_ppp, &sm_rtk }) {
        for (const auto& [sat, el] : elevations_deg) {
            sm->setIonosphereElevation(sat, el);
            sm->addIonosphereState(sat, 1.0);
        }
        sm->addAmbiguity("G05_L1C", 0.0);
        sm->addCodeBiasState("GPS_IFB5", 0.0);
    }
    const Eigen::MatrixXd Qp = sm_ppp.getProcessNoiseMatrix(kDt);
    const Eigen::MatrixXd Qr = sm_rtk.getProcessNoiseMatrix(kDt);
    ASSERT_EQ(Qp.rows(), Qr.rows());
    EXPECT_TRUE(Qp == Qr);
    for (const auto& [sat, el] : elevations_deg) {
        const auto i = static_cast<Eigen::Index>(sm_ppp.getIonosphereIndex(sat));
        EXPECT_GT(Qp(i, i), 0.0) << sat;
    }
}

TEST(SlantIonospherePresenceTest, TheRateStateGetsItsConstantVelocityBlockWhenTheRunCarriesTheStates) {
    // enable_iono_rate adds a companion rate state per satellite; its CWNA block (level
    // q*dt^3/3, cross q*dt^2/2, rate q*dt, the acceleration density over sin^2) is
    // written under the same predicate as the level random walk.
    const GnssConfig ppp = loadConfig(
        R"({"processing": {"mode": "PPP", "uncombined_enabled": true},
            "estimation": {"enable_iono_rate": true}})", "ppp_rate");
    ASSERT_TRUE(ppp.carriesSlantIonosphere());
    ASSERT_TRUE(ppp.enableIonoRate());
    LogManager lm;
    StateManager sm{ 5, lm, ppp };
    sm.setIonosphereElevation("G05", kElevationDeg);
    const size_t level = sm.addIonosphereState("G05", kSeedValue);
    const int rate_i = sm.getIonosphereRateIndex("G05");
    ASSERT_GE(rate_i, 0);
    const auto rate = static_cast<Eigen::Index>(rate_i);

    const Eigen::MatrixXd Q = sm.getProcessNoiseMatrix(kDt);
    const double s = std::sin(kElevationDeg * Constants::PI / 180.0);
    const double q_rate = ppp.getProcessNoise().iono_rate_psd / (s * s);
    EXPECT_DOUBLE_EQ(Q(level, level), q_rate * kDt * kDt * kDt / 3.0);
    EXPECT_DOUBLE_EQ(Q(level, rate),  q_rate * kDt * kDt / 2.0);
    EXPECT_DOUBLE_EQ(Q(rate, level),  q_rate * kDt * kDt / 2.0);
    EXPECT_DOUBLE_EQ(Q(rate, rate),   q_rate * kDt);

    // The family table states the rate rule as applied on this run.
    const auto table = sm.familiesAsArmed();
    const std::string rate_line = table.at(1 + static_cast<std::size_t>(StateFamily::IonosphereRate));
    EXPECT_NE(rate_line.find("born yes"), std::string::npos) << rate_line;
    EXPECT_NE(rate_line.find(", applied on this run"), std::string::npos) << rate_line;

    // The export carries the rate state like the level state.
    PppState ps;
    sm.updatePppStateFromVector(sm.getStateVector(), ps, sm);
    EXPECT_EQ(ps.slant_ionosphere.count("G05"), 1u);
    EXPECT_EQ(ps.slant_ionosphere_rate.count("G05"), 1u);
}

TEST(SlantIonospherePresenceTest, AFrozenPivotSatelliteKeepsNoQInEveryCarryingMode) {
    for (const ModeCase& c : allModes()) {
        if (!c.carries) continue;
        SCOPED_TRACE(c.name);
        const GnssConfig cfg = buildConfig(c);
        LogManager lm;
        StateManager sm{ 5, lm, cfg };
        for (const char* sat : { "G05", "G12" }) {
            sm.setIonosphereElevation(sat, kElevationDeg);
            sm.addIonosphereState(sat, kSeedValue);
        }
        sm.setFrozenPivotSatellites({ "G05" });
        const Eigen::MatrixXd Q = sm.getProcessNoiseMatrix(kDt);
        const auto frozen = static_cast<Eigen::Index>(sm.getIonosphereIndex("G05"));
        const auto live   = static_cast<Eigen::Index>(sm.getIonosphereIndex("G12"));
        EXPECT_EQ(Q(frozen, frozen), 0.0);
        EXPECT_DOUBLE_EQ(Q(live, live), expectedLevelQ(cfg));
    }
}
