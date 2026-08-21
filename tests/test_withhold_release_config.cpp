/**
 * @file test_withhold_release_config.cpp
 * @brief Config-contract tests for the A4 withhold-era release policy knob set
 *        (@c network.raim_withhold_adapt_escalate + @c raim_withhold_free_erase;
 *        `SPEC_withhold_era_release.md` §3).
 *
 * Both responders are default-OFF and byte-identical when off; what these
 * tests lock is the loader's side of the safety story: the defaults, the
 * reject-to-0 band on the escalation budget (an "escalation" at or below the
 * stock within-epoch cap of 5 is a no-op that would read as armed), and the
 * three warn-and-disarm interlocks (each responder needs an armed A4 ceiling;
 * the erase half additionally needs @c cycle_slip.relevel_free_epoch, whose
 * latch it consumes).  The era mechanics are graded on the fld63 repro pair,
 * not here.
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
    const fs::path p = fs::temp_directory_path() / ("whrelease_cfg_" + tag + ".json");
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

// The armed happy path: an A4 ceiling (which auto-arms within_epoch) + both
// responders + the free-epoch response the erase half consumes.  phase_leveling
// is load-bearing: relevel_free_epoch frees LEVELING carries and disarms
// without it, which would cascade into the free_erase disarm for the wrong
// reason (the fld63 lineage arms leveling in its base chain).
const char* kArmedBlock =
    "    \"phase_leveling\": true,\n"
    "    \"raim_withhold_sigma0\": 6.0,\n"
    "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
    "    \"raim_withhold_adapt_escalate\": 12,\n"
    "    \"raim_withhold_free_erase\": true,\n";

}  // namespace

TEST(WithholdReleaseConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 0);
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase());
}

TEST(WithholdReleaseConfig, ArmedHappyPathSurvivesLoad) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed", networkConfig(kArmedBlock), cfg));
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 12);
    EXPECT_TRUE(cfg.networkRaimWithholdFreeErase());
    // The ceiling auto-arms the within-epoch tier (pre-existing coupling the
    // policy rides on — pinned here so a regression is visible from this file).
    EXPECT_TRUE(cfg.networkRaimWithinEpoch());
}

TEST(WithholdReleaseConfig, EscalateAtOrBelowStockBudgetRejectsToZero) {
    // (0, 5] is a no-op that would read as armed — the loader zeroes it.
    for (int v : {1, 3, 5}) {
        GnssConfig cfg;
        const std::string extra =
            "    \"raim_withhold_sigma0\": 6.0,\n"
            "    \"raim_withhold_adapt_escalate\": " + std::to_string(v) + ",\n";
        ASSERT_TRUE(loadInto("rejlow" + std::to_string(v), networkConfig(extra), cfg));
        EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 0) << "value " << v;
    }
}

TEST(WithholdReleaseConfig, NegativeEscalateRejectsToZero) {
    GnssConfig cfg;
    const std::string extra =
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"raim_withhold_adapt_escalate\": -4,\n";
    ASSERT_TRUE(loadInto("rejneg", networkConfig(extra), cfg));
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 0);
}

TEST(WithholdReleaseConfig, EscalateDisarmsWithoutWithholdCeiling) {
    GnssConfig cfg;
    const std::string extra = "    \"raim_withhold_adapt_escalate\": 12,\n";
    ASSERT_TRUE(loadInto("esc_noceil", networkConfig(extra), cfg));
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 0)
        << "no A4 ceiling => no withhold eras to escalate inside";
}

TEST(WithholdReleaseConfig, FreeEraseDisarmsWithoutWithholdCeiling) {
    GnssConfig cfg;
    const std::string extra =
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("erase_noceil", networkConfig(extra), cfg));
    // The latch is live (leveling + free_epoch armed) — the disarm below is
    // therefore attributable to the missing ceiling alone.
    EXPECT_TRUE(cfg.networkCycleSlipRelevelFreeEpoch());
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase())
        << "the erase site IS the withhold branch";
}

TEST(WithholdReleaseConfig, FreeEraseDisarmsWithoutRelevelFreeEpoch) {
    GnssConfig cfg;
    const std::string extra =
        "    \"phase_leveling\": true,\n"
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("erase_nolatch", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase())
        << "without relevel_free_epoch the latch it consumes is always empty";
}

TEST(WithholdReleaseConfig, FreeEraseDisarmsWhenLevelingDisarmsTheLatch) {
    // The transitive cascade: relevel_free_epoch itself disarms without
    // phase_leveling (it frees LEVELING carries), and the erase half must
    // follow it down rather than stay armed against a latch that can never
    // fill.  This is the defect shape the first version of this suite had.
    GnssConfig cfg;
    const std::string extra =
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("erase_nolvl", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelFreeEpoch());
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase());
}

TEST(WithholdReleaseConfig, EscalateSurvivesWithoutFreeErase) {
    // The two responders are independent (SPEC §3 separability).
    GnssConfig cfg;
    const std::string extra =
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"raim_withhold_adapt_escalate\": 8,\n";
    ASSERT_TRUE(loadInto("esc_only", networkConfig(extra), cfg));
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 8);
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase());
}

TEST(WithholdReleaseConfig, FreeEraseSurvivesWithoutEscalate) {
    GnssConfig cfg;
    const std::string extra =
        "    \"phase_leveling\": true,\n"
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("erase_only", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkRaimWithholdAdaptEscalate() > 0);
    EXPECT_TRUE(cfg.networkRaimWithholdFreeErase());
}

TEST(WithholdReleaseConfig, CeilingRejectBandCascadesIntoResponderDisarm) {
    // A ceiling in (0,1] is itself rejected to 0 (pre-existing R11/R1 guard);
    // the responders must then disarm off the POST-reject value, not the
    // user's raw one — the Card-1g parse-order rationale for the end-of-section
    // interlock site.
    GnssConfig cfg;
    const std::string extra =
        "    \"phase_leveling\": true,\n"
        "    \"raim_withhold_sigma0\": 0.5,\n"
        "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
        "    \"raim_withhold_adapt_escalate\": 12,\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("cascade", networkConfig(extra), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkRaimWithholdSigma0(), 0.0);
    EXPECT_EQ(cfg.networkRaimWithholdAdaptEscalate(), 0);
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase());
}
