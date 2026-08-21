/**
 * @file test_iono_carry_config.cpp
 * @brief Config-contract tests for the QUEUE-8b slant-iono carry knob set
 *        (@c network.carry_iono and riders).
 *
 * The carry is solve-affecting and default-OFF; what these tests lock is the
 * loader's side of the safety story: the defaults, the four warn-and-disarm
 * interlocks (carry_invariant / no-phase-leveling / estimate_iono_sh /
 * mech-B rep_error_tau_s), the scope=relevel ↔ relevel_free_epoch dependency,
 * the hard rejections (negative q / negative floor / unknown scope), and the
 * @c NetworkQClass::IonoCarry table routing.  The estimator-side behavior is
 * graded on the sim fixture, not here.
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
    const fs::path p = fs::temp_directory_path() / ("ionocarry_cfg_" + tag + ".json");
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

// The armed happy-path network block: leveling + the relevel response + carry.
const char* kArmedBlock =
    "    \"phase_leveling\": true,\n"
    "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
    "    \"carry_iono\": true,\n";

}  // namespace

TEST(IonoCarryConfig, DefaultsAreOffWithDocumentedRiders) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryQ(), 1e-4);
    EXPECT_EQ(cfg.networkIonoCarryScope(), "relevel");
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryVarFloor(), 0.04);
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryMaxGapS(), 300.0);
}

TEST(IonoCarryConfig, ArmedHappyPathSurvivesLoad) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed", networkConfig(kArmedBlock), cfg));
    EXPECT_TRUE(cfg.networkCarryIono());
    EXPECT_TRUE(cfg.networkPhaseLeveling());
    EXPECT_TRUE(cfg.networkCycleSlipRelevelFreeEpoch());
}

TEST(IonoCarryConfig, ScopeAlwaysParsesAndSurvivesWithoutRelevelFreeEpoch) {
    // scope=always does not key on the free set, so it must stay armed with the
    // relevel response off (the interlock is scope=relevel-specific).
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("always",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"carry_iono\": true,\n"
                      "    \"iono_carry_scope\": \"always\",\n"), cfg));
    EXPECT_TRUE(cfg.networkCarryIono());
    EXPECT_EQ(cfg.networkIonoCarryScope(), "always");
}

TEST(IonoCarryConfig, ScopeRelevelDisarmsWithoutRelevelFreeEpoch) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("norelevel",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"carry_iono\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(IonoCarryConfig, DisarmedWithoutPhaseLeveling) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nopl",
        networkConfig("    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
                      "    \"carry_iono\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(IonoCarryConfig, DisarmedUnderCarryInvariant) {
    GnssConfig cfg;
    // carry_invariant is defined only on the eq34 chain datum (loader hard
    // requirement), so the interlock is exercised on that valid pairing.
    ASSERT_TRUE(loadInto("cinv",
        networkConfig(std::string(kArmedBlock) +
                      "    \"phase_datum_eq34\": true,\n"
                      "    \"carry_invariant\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(IonoCarryConfig, DisarmedUnderEstimateIonoSh) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("sh",
        networkConfig(std::string(kArmedBlock) +
                      "    \"estimate_iono_sh\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(IonoCarryConfig, MutexWithMechBDeviationCarry) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("mechb",
        networkConfig(std::string(kArmedBlock),
                      "  \"estimation\": { \"global_vtec_sh\": { \"rep_error_tau_s\": 900.0 } },\n"),
        cfg));
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(IonoCarryConfig, NegativeQRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negq",
        networkConfig(std::string(kArmedBlock) +
                      "    \"iono_carry_q\": -1e-4,\n"), cfg));
}

TEST(IonoCarryConfig, NegativeFloorRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("negfloor",
        networkConfig(std::string(kArmedBlock) +
                      "    \"iono_carry_var_floor\": -0.04,\n"), cfg));
}

TEST(IonoCarryConfig, UnknownScopeRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("badscope",
        networkConfig(std::string(kArmedBlock) +
                      "    \"iono_carry_scope\": \"sometimes\",\n"), cfg));
}

TEST(IonoCarryConfig, QResolvesThroughProcessNoiseTable) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("qtable",
        networkConfig(std::string(kArmedBlock) +
                      "    \"process_noise\": { \"iono_carry\": 5e-4 },\n"), cfg));
    EXPECT_TRUE(cfg.networkCarryIono());
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryQ(), 5e-4);
}

TEST(IonoCarryConfig, ExplicitLegacyKeyBeatsTable) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("legacywins",
        networkConfig(std::string(kArmedBlock) +
                      "    \"process_noise\": { \"iono_carry\": 5e-4 },\n"
                      "    \"iono_carry_q\": 2e-4,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryQ(), 2e-4);
}

// --- Bridge v3 rider interlocks (same loader region; parked here rather than
// --- in the kernel file to keep that file's include set pure).
TEST(IonoCarryConfig, BridgeV3RidersRequireDeltaBridge) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3nodb",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"cycle_slip\": { \"mw_detect\": true, \"if_phase_td\": true,\n"
                      "                      \"relevel_free_epoch\": true,\n"
                      "                      \"relevel_bridge_gf\": true,\n"
                      "                      \"relevel_bridge_ewl\": true },\n"), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelBridgeGf());
    EXPECT_FALSE(cfg.networkCycleSlipRelevelBridgeEwl());
}

TEST(IonoCarryConfig, BridgeV3RidersSurviveWithDeltaBridge) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3ok",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"cycle_slip\": { \"mw_detect\": true, \"if_phase_td\": true,\n"
                      "                      \"relevel_free_epoch\": true,\n"
                      "                      \"relevel_delta_bridge\": true,\n"
                      "                      \"relevel_bridge_gf\": true,\n"
                      "                      \"relevel_bridge_ewl\": true },\n"), cfg));
    EXPECT_TRUE(cfg.networkCycleSlipRelevelBridgeGf());
    EXPECT_TRUE(cfg.networkCycleSlipRelevelBridgeEwl());
    EXPECT_DOUBLE_EQ(cfg.networkCycleSlipRelevelBridgeGfSigmaFloorM(), 0.01);
    EXPECT_DOUBLE_EQ(cfg.networkCycleSlipRelevelBridgeEwlSigmaFloorCyc(), 0.05);
}

// --- CS-GF authority (QUEUE 8b successor): source knob + the pure recursion.
#include "slip/NetworkRawObservables.hpp"

TEST(IonoCarryConfig, SourceDefaultsToSolveAndParsesSmoothed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("srcdef", networkConfig(kArmedBlock), cfg));
    EXPECT_EQ(cfg.networkIonoCarrySource(), "solve");
    GnssConfig cfg2;
    ASSERT_TRUE(loadInto("srcsm",
        networkConfig(std::string(kArmedBlock) +
                      "    \"iono_carry_source\": \"smoothed_gf\",\n"), cfg2));
    EXPECT_EQ(cfg2.networkIonoCarrySource(), "smoothed_gf");
}

TEST(IonoCarryConfig, UnknownSourceRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("srcbad",
        networkConfig(std::string(kArmedBlock) +
                      "    \"iono_carry_source\": \"oracle\",\n"), cfg));
}

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
    // Slip: the empirical gate tripped — re-seed from the fresh code sample.
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
