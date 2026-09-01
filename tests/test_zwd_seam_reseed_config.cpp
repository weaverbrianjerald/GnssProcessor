/**
 * @file test_zwd_seam_reseed_config.cpp
 * @brief Config-contract tests for the seam-scoped ZWD re-seed knob set
 *        (@c network.zwd_seam_reseed + @c network.zwd_seam_reseed_epochs,
 *        SPEC_synthetic_minimum_network §17 carry_zwd re-base arms, finding 3).
 *
 * What these tests lock is the loader's side of the safety story: the defaults
 * (OFF, 480-epoch horizon), the hard rejection of a non-positive horizon and a
 * negative RW q, and that the warn-only inert postures (no snapshot path / no
 * phase-leveling / carry_zwd also armed) still LOAD — they are wiring-inert,
 * not illegal, and the runtime logs which.  The seam-window mechanics (arm at
 * resume, rolling B9 fold, retire at horizon) and the unconditional Z-row
 * capture are graded on the sim fixture ladder, not here.
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

// Write @p body to a unique temp file and return its path.
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("zwdseam_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal loadable PPP-RTK network config; @p extra splices into the network
// block (the iono-carry test file's pattern).
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

TEST(ZwdSeamReseedConfig, DefaultsAreOffWithDocumentedHorizon) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkZwdSeamReseed());
    EXPECT_EQ(cfg.networkZwdSeamReseedEpochs(), 480);
    // The seam card leaves the falsified continuous carry exactly where it was.
    EXPECT_FALSE(cfg.networkCarryZwd());
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
}

TEST(ZwdSeamReseedConfig, ArmedHappyPathSurvivesLoad) {
    // The validation-ladder posture: leveling + snapshot path + the seam knob.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkZwdSeamReseed());
    EXPECT_EQ(cfg.networkZwdSeamReseedEpochs(), 480);
    EXPECT_FALSE(cfg.networkCarryZwd());   // the seam does NOT arm B9
}

TEST(ZwdSeamReseedConfig, ExplicitHorizonParses) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("horizon",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"
                      "    \"zwd_seam_reseed_epochs\": 720,\n"), cfg));
    EXPECT_EQ(cfg.networkZwdSeamReseedEpochs(), 720);
}

TEST(ZwdSeamReseedConfig, NonPositiveHorizonIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zero_horizon",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"
                      "    \"zwd_seam_reseed_epochs\": 0,\n"), cfg));
    EXPECT_FALSE(loadInto("negative_horizon",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"
                      "    \"zwd_seam_reseed_epochs\": -5,\n"), cfg));
}

TEST(ZwdSeamReseedConfig, NegativeCarryQIsRejectedWhenSeamArmed) {
    // carry_zwd off would otherwise leave a negative zwd_carry_q unvalidated;
    // the seam window folds var + q*dt, so it re-runs the q >= 0 check.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("neg_q",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"
                      "    \"zwd_carry_q\": -1e-9,\n"), cfg));
}

TEST(ZwdSeamReseedConfig, InertPosturesStillLoad) {
    // No snapshot path: no resume can happen — warn-inert, not illegal.
    GnssConfig cfg1;
    ASSERT_TRUE(loadInto("no_snap",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"zwd_seam_reseed\": true,\n"), cfg1));
    EXPECT_TRUE(cfg1.networkZwdSeamReseed());
    // No phase-leveling: the inject rides the carry_warm gate — warn-inert.
    GnssConfig cfg2;
    ASSERT_TRUE(loadInto("no_pl",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"zwd_seam_reseed\": true,\n"), cfg2));
    EXPECT_TRUE(cfg2.networkZwdSeamReseed());
}

TEST(ZwdSeamReseedConfig, CoexistsWithContinuousCarry) {
    // B9 + seam both armed: B9 subsumes the window (info-logged), still legal —
    // this is exactly the (cz) reference posture plus the seam knob.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("with_b9",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"carry_zwd\": true,\n"
                      "    \"zwd_seam_reseed\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkCarryZwd());
    EXPECT_TRUE(cfg.networkZwdSeamReseed());
}
