/**
 * @file test_station_factor_config.cpp
 * @brief Config-contract + kernel tests for QUEUE item 4 R2/R4
 *        (@c network.raim_station_factor + @c raim_rate_hygiene;
 *        `SPEC_t3_null_derivation.md` §6/§7.5).
 *
 * Both knobs are measure-only and default-OFF; the loader side locks the
 * defaults, the reject bands (τ ≤ 0, ratio ≤ 1), and the raim /
 * trace_flag_rates interlocks.  The kernel side pins the §7.5 trimmed
 * variance component: the E[w·v²] = r_i identity, the trim's both-sums
 * semantics, and the no-checkable-redundancy sentinel ("unavailable is
 * not 0").  The declaration behavior (EWMA, depart/return) is graded on
 * the pareref_on fixture against the t3_null_fit reference, not here.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkRobustStats.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("staf_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// ---------------------------------------------------------------- loader ----

TEST(StationFactorConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkRaimStationFactor());
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorTauS(), 1800.0);
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorRatio(), 6.0);
    EXPECT_FALSE(cfg.networkRaimRateHygiene());
}

TEST(StationFactorConfig, ArmedHappyPathSurvivesLoad) {
    GnssConfig cfg;
    const std::string extra =
        "    \"raim\": true,\n"
        "    \"trace_flag_rates\": true,\n"
        "    \"raim_station_factor\": true,\n"
        "    \"raim_station_factor_tau_s\": 900.0,\n"
        "    \"raim_station_factor_ratio\": 4.0,\n"
        "    \"raim_rate_hygiene\": true,\n";
    ASSERT_TRUE(loadInto("armed", networkConfig(extra), cfg));
    EXPECT_TRUE(cfg.networkRaimStationFactor());
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorTauS(), 900.0);
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorRatio(), 4.0);
    EXPECT_TRUE(cfg.networkRaimRateHygiene());
}

TEST(StationFactorConfig, FactorDisarmsWithoutRaim) {
    GnssConfig cfg;
    const std::string extra = "    \"raim_station_factor\": true,\n";
    ASSERT_TRUE(loadInto("noraim", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkRaimStationFactor());
}

TEST(StationFactorConfig, HygieneDisarmsWithoutRaim) {
    GnssConfig cfg;
    const std::string extra =
        "    \"trace_flag_rates\": true,\n"
        "    \"raim_rate_hygiene\": true,\n";
    ASSERT_TRUE(loadInto("hyg_noraim", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkRaimRateHygiene());
}

TEST(StationFactorConfig, HygieneDisarmsWithoutFlagRates) {
    GnssConfig cfg;
    const std::string extra =
        "    \"raim\": true,\n"
        "    \"raim_rate_hygiene\": true,\n";
    ASSERT_TRUE(loadInto("hyg_nofr", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkRaimRateHygiene());
}

TEST(StationFactorConfig, RejectBandsResetToDefaults) {
    GnssConfig cfg;
    const std::string extra =
        "    \"raim\": true,\n"
        "    \"raim_station_factor\": true,\n"
        "    \"raim_station_factor_tau_s\": -5.0,\n"
        "    \"raim_station_factor_ratio\": 0.5,\n";
    ASSERT_TRUE(loadInto("rej", networkConfig(extra), cfg));
    EXPECT_TRUE(cfg.networkRaimStationFactor());
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorTauS(), 1800.0);
    EXPECT_DOUBLE_EQ(cfg.networkRaimStationFactorRatio(), 6.0);
}

// ---------------------------------------------------------------- kernel ----

TEST(StationFactorKernel, HonestModelReadsUnity) {
    // E[w·v²] = r_i: rows drawn exactly at the identity give factor 1.
    std::vector<StationFactorRow> rows;
    for (int k = 0; k < 20; ++k) rows.push_back({0.8, 0.8, 0.9});
    EXPECT_NEAR(stationVarianceFactor(rows, 0.0), 1.0, 1e-12);
}

TEST(StationFactorKernel, ScaleInflationReadsAsFactor) {
    // A station whose residuals run 3x the model in σ shows a 9x variance factor.
    std::vector<StationFactorRow> rows;
    for (int k = 0; k < 20; ++k) rows.push_back({9.0 * 0.7, 0.7, 2.0});
    EXPECT_NEAR(stationVarianceFactor(rows, 0.0), 9.0, 1e-12);
}

TEST(StationFactorKernel, TrimRemovesSpikeFromBothSums) {
    // One slip-class spike must not set the slow scale: with the spike trimmed
    // from BOTH sums the factor returns to the clean rows' value (the §7.5
    // "robust variant is not cosmetic" requirement).
    std::vector<StationFactorRow> rows;
    for (int k = 0; k < 10; ++k) rows.push_back({1.0, 1.0, 1.0});
    rows.push_back({400.0, 1.0, 20.0});   // the spike (r_std 20)
    const double untrimmed = stationVarianceFactor(rows, 0.0);
    const double trimmed   = stationVarianceFactor(rows, 6.0);
    EXPECT_GT(untrimmed, 30.0);
    EXPECT_NEAR(trimmed, 1.0, 1e-12);
}

TEST(StationFactorKernel, NoCheckableRedundancyIsSentinelNotZero) {
    // All rows uncheckable (r_i ~ 0): the factor is UNAVAILABLE, never 0 — a 0
    // would read as "perfect station" to any consumer of the trace.
    std::vector<StationFactorRow> rows;
    for (int k = 0; k < 5; ++k) rows.push_back({1e-9, 1e-12, 0.5});
    EXPECT_LT(stationVarianceFactor(rows, 0.0), 0.0);
    EXPECT_LT(stationVarianceFactor({}, 0.0), 0.0);
}

TEST(StationFactorKernel, LowRedundancyRowCannotBiasTheRatio) {
    // The s0g argument, pinned: an r_i ~ 0 row contributes ~0 to numerator AND
    // denominator, so including it barely moves the factor.
    std::vector<StationFactorRow> rows;
    for (int k = 0; k < 10; ++k) rows.push_back({2.0, 1.0, 1.4});
    const double base = stationVarianceFactor(rows, 0.0);
    rows.push_back({1e-8, 1e-8, 0.1});
    EXPECT_NEAR(stationVarianceFactor(rows, 0.0), base, 1e-6);
}
