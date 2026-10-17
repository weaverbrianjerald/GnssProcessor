/**
 * @file test_datum_station_slip_config.cpp
 * @brief Config-contract tests for datum-station slip detection
 *        (@c network.cycle_slip.datum_station_detect; SPEC_synthetic_minimum_network §9.7.10).
 *
 * Under the legacy single-pivot phase datum every slip-detection loop starts at
 * i_lo = 1, so the datum station (the one receiver whose ambiguity enters every
 * emitted phase bias) has no slip detection (§9.7.4, shown by roster reorder).
 * The toggle arms MW/LLI detection at stations[0] with a network-wide
 * re-reference response.  These tests check the loader contract: off by default
 * (byte-identical), the mw_detect prerequisite (disabled with a warning when
 * absent), and that the invariant-#1 auto-arm of mw_detect under phase_leveling
 * satisfies the prerequisite (which depends on load order).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p =
        gnss_test::tempDir() / ("datum_slip_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal but loadable PPP-RTK network config; @p network_extra is spliced
// into the network block (same shape as the CodeBiasNetworkConfig helper).
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

} // namespace

// Default: off.  The datum station stays outside the slip loops unless armed, so
// the solve is byte-identical by construction.
TEST(DatumStationSlipDetectConfig, DefaultOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkCycleSlipDatumStationDetect());
    std::error_code ec; fs::remove(p, ec);
}

// Armed with its vehicle (mw_detect): stays armed.
TEST(DatumStationSlipDetectConfig, ArmsWithMwDetect) {
    LogManager log;
    const fs::path p = writeTempConfig("armed", networkConfig(
        "    \"cycle_slip\": { \"mw_detect\": true, \"datum_station_detect\": true },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkCycleSlipMwDetect());
    EXPECT_TRUE(cfg.networkCycleSlipDatumStationDetect());
    std::error_code ec; fs::remove(p, ec);
}

// Armed without mw_detect (and nothing that auto-arms it): disabled.  The MW/LLI
// drain is the detection vehicle; without it the toggle would be armed but inert
// (the §0 error class).
TEST(DatumStationSlipDetectConfig, DisabledWithoutMwDetect) {
    LogManager log;
    const fs::path p = writeTempConfig("novehicle", networkConfig(
        "    \"cycle_slip\": { \"datum_station_detect\": true },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkCycleSlipMwDetect());
    EXPECT_FALSE(cfg.networkCycleSlipDatumStationDetect());
    std::error_code ec; fs::remove(p, ec);
}

// phase_leveling with no detector auto-arms mw_detect (standing invariant #1),
// and that auto-arm satisfies the datum_station_detect prerequisite because the
// toggle is parsed after the auto-arm.  Reordering the two would silently disarm
// the datum path on every leveling config.
TEST(DatumStationSlipDetectConfig, LevelingAutoArmSatisfiesPrerequisite) {
    LogManager log;
    const fs::path p = writeTempConfig("autoarm", networkConfig(
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": { \"datum_station_detect\": true },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkCycleSlipMwDetect());          // auto-armed by leveling
    EXPECT_TRUE(cfg.networkCycleSlipDatumStationDetect()); // prerequisite satisfied
    std::error_code ec; fs::remove(p, ec);
}
