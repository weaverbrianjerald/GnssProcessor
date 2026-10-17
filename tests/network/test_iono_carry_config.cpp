/**
 * @file test_iono_carry_config.cpp
 * @brief The bridge v3 rider interlocks and the CS-GF authority kernel.
 *
 * The file held the config contract of the diagonal slant-iono carry
 * (@c network.carry_iono, QUEUE 8b), which was retired with its keys.  What stays:
 * the loader's bridge v3 rider interlocks (parked here, beside the kernel) and the
 * pure CS-GF recursion that the LV-6 level-authority census
 * (@c network.level_authority_csgf) advances.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// Write @p body to a unique temp file and return its path.
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("ionocarry_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal loadable PPP-RTK network config; @p extra splices into the network
// block, @p top_level beside "processing"/"network" (the process-noise test
// file's pattern).
std::string networkConfig(const std::string& extra = "",
                          const std::string& top_level = "") {
    return std::string("{\n") + top_level +
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

// --- Bridge v3 rider interlocks (same loader region; parked here rather than
// --- in the kernel file to keep that file's include set pure).
TEST(BridgeV3RiderConfig, BridgeV3RidersRequireDeltaBridge) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3nodb",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"cycle_slip\": { \"mw_detect\": true, \"if_phase_td\": true,\n"
                      "                      \"relevel_free_epoch\": true,\n"
                      "                      \"relevel_bridge_gf\": true },\n"), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelBridgeGf());
}

TEST(BridgeV3RiderConfig, BridgeV3RidersSurviveWithDeltaBridge) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3ok",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"cycle_slip\": { \"mw_detect\": true, \"if_phase_td\": true,\n"
                      "                      \"relevel_free_epoch\": true,\n"
                      "                      \"relevel_delta_bridge\": true,\n"
                      "                      \"relevel_bridge_gf\": true },\n"), cfg));
    EXPECT_TRUE(cfg.networkCycleSlipRelevelBridgeGf());
    EXPECT_DOUBLE_EQ(cfg.networkCycleSlipRelevelBridgeGfSigmaFloorM(), 0.01);
}

// --- CS-GF authority: the pure recursion (network.level_authority_csgf).
#include "network/slip/RawObservables.hpp"

TEST(IonoCsKernel, FirstSightingSeedsFromCodeAtHonestVariance) {
    IonoCsState cs;
    EXPECT_TRUE(networkIonoCsUpdate(cs, 12.5, 0.0, false, false, 0.7));
    EXPECT_DOUBLE_EQ(cs.val, 12.5);
    EXPECT_DOUBLE_EQ(cs.var, 0.49);
    EXPECT_EQ(cs.n, 1);
}

TEST(IonoCsKernel, ConvergesToTheCodeRealizationWhileTrackingTheRamp) {
    // Truth iono ramps 2 cm/epoch; the code channel carries a constant +0.8 m
    // bias content (the DCB realization).  The carrier deltas track the ramp
    // exactly; the blends pull toward code.  After 200 epochs the state must
    // sit at truth + bias (the code realization) to well under a decimetre,
    // with variance far below one code sample's.
    IonoCsState cs;
    const double bias = 0.8, sigma = 0.7;
    double truth = 10.0;
    networkIonoCsUpdate(cs, truth + bias, 0.0, false, false, sigma);
    for (int k = 0; k < 200; ++k) {
        truth += 0.02;
        networkIonoCsUpdate(cs, truth + bias, 0.02, true, false, sigma);
    }
    EXPECT_NEAR(cs.val, truth + bias, 0.05);
    EXPECT_LT(cs.var, 0.05 * sigma * sigma);
    EXPECT_GT(cs.var, 0.0);
}

TEST(IonoCsKernel, SlipGateAndGapBothReset) {
    IonoCsState cs;
    networkIonoCsUpdate(cs, 5.0, 0.0, false, false, 0.7);
    for (int k = 0; k < 50; ++k) networkIonoCsUpdate(cs, 5.0, 0.0, true, false, 0.7);
    ASSERT_GT(cs.n, 10);
    // Slip: the empirical gate tripped, so re-seed from the fresh code sample.
    EXPECT_TRUE(networkIonoCsUpdate(cs, 9.0, 123.0, true, true, 0.7));
    EXPECT_DOUBLE_EQ(cs.val, 9.0);
    EXPECT_EQ(cs.n, 1);
    for (int k = 0; k < 5; ++k) networkIonoCsUpdate(cs, 9.0, 0.0, true, false, 0.7);
    // Gap: adjacency lost.
    EXPECT_TRUE(networkIonoCsUpdate(cs, 9.5, 0.0, false, false, 0.7));
    EXPECT_DOUBLE_EQ(cs.val, 9.5);
}

TEST(IonoCsKernel, UnusableResetSampleLeavesStateUninitialized) {
    IonoCsState cs;
    EXPECT_TRUE(networkIonoCsUpdate(cs, std::nan(""), 0.0, false, false, 0.7));
    EXPECT_EQ(cs.n, 0);
    EXPECT_TRUE(networkIonoCsUpdate(cs, 3.0, 0.0, true, false, 0.0));
    EXPECT_EQ(cs.n, 0);   // zero sigma is unusable too
}
