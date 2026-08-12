/**
 * @file test_datum_station_slip_config.cpp
 * @brief Config-contract tests for datum-station slip detection
 *        (@c network.cycle_slip.datum_station_detect — SPEC_synthetic_minimum_network §9.7.10).
 *
 * Under the legacy single-pivot phase datum every slip-detection loop starts at
 * i_lo = 1, so the datum station — the one receiver whose ambiguity rides inside
 * every emitted phase bias — has zero slip exposure (§9.7.4, proven by roster
 * reorder).  The toggle arms MW/LLI detection at stations[0] with a NETWORK
 * re-reference response.  These tests lock the loader contract: default OFF
 * (byte-identical), the mw_detect prerequisite (hard-disable with a warning when
 * absent), and that the invariant-#1 auto-arm of mw_detect under phase_leveling
 * satisfies the prerequisite (load order matters and must not regress).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p =
        fs::temp_directory_path() / ("datum_slip_cfg_" + tag + ".json");
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

// Default: OFF.  The datum station stays outside the slip loops unless armed —
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

// Armed WITHOUT mw_detect (and nothing that auto-arms it): hard-disabled.  The
// MW/LLI drain is the detection vehicle; without it the toggle would be an
// armed-but-inert tier — exactly the §0 error class this campaign documented.
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
// and that auto-arm must SATISFY the datum_station_detect prerequisite — i.e.
// the toggle is parsed after the auto-arm.  A regression that reorders the two
// silently disarms the datum path on every leveling config.
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
