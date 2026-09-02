/**
 * @file test_amb_seam_reinflate_config.cpp
 * @brief Config-contract tests for the seam-scoped ambiguity-carry variance
 *        re-inflation knob (@c network.amb_seam_reinflate_var_m2, STABILITY
 *        target 2 leg C, SPEC_synthetic_minimum_network §18).
 *
 * What these tests lock: the default (0.0 = OFF, byte-identical), a plain
 * positive value parsing, the hard rejection of a negative variance ADD, and
 * that the warn-only inert postures (no snapshot path / no phase-leveling)
 * still LOAD — wiring-inert, not illegal.  The seam mechanics (one-shot
 * variance ADD on the restored leveling carries at the resume site, values
 * kept as prior means) are graded on the sim fixture ladder, not here.
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
    const fs::path p = fs::temp_directory_path() / ("ambseam_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal loadable PPP-RTK network config; @p extra splices into the network
// block (the zwd-seam test file's pattern).
std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
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

TEST(AmbSeamReinflateConfig, DefaultIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkAmbSeamReinflateVarM2(), 0.0);
}

TEST(AmbSeamReinflateConfig, ArmedHappyPathSurvivesLoad) {
    // The validation-ladder posture: leveling + snapshot path + the knob.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"amb_seam_reinflate_var_m2\": 0.0025,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkAmbSeamReinflateVarM2(), 0.0025);
}

TEST(AmbSeamReinflateConfig, NegativeVarianceIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negative",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"amb_seam_reinflate_var_m2\": -0.001,\n"), cfg));
}

TEST(AmbSeamReinflateConfig, InertPosturesStillLoad) {
    // No snapshot path: warn-inert (a run that can never resume), not illegal.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("no_snap",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"amb_seam_reinflate_var_m2\": 0.0025,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkAmbSeamReinflateVarM2(), 0.0025);
    // No phase-leveling: warn-inert (no leveling carries to re-inflate).
    GnssConfig cfg2;
    ASSERT_TRUE(loadInto("no_leveling",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"amb_seam_reinflate_var_m2\": 0.0025,\n"), cfg2));
    EXPECT_DOUBLE_EQ(cfg2.networkAmbSeamReinflateVarM2(), 0.0025);
}
