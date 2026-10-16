/**
 * @file test_withhold_release_config.cpp
 * @brief Config-contract tests for the A4 withhold-era release policy knob set
 *        (@c network.raim_withhold_adapt_escalate + @c raim_withhold_free_erase;
 *        `SPEC_withhold_era_release.md` §3).
 *
 * Both responders are off by default and byte-identical when off.  These tests
 * cover the loader's side: the defaults, the reject-to-0 band on the escalation
 * budget (an "escalation" at or below the stock within-epoch cap of 5 is a no-op
 * that would read as armed), and the three warn-and-disarm interlocks (each
 * responder needs an armed A4 ceiling; the erase half also needs
 * @c cycle_slip.relevel_free_epoch, whose latch it consumes).  The latch's own
 * disarms (without @c phase_leveling, and under @c carry_invariant) take the erase
 * half with them.  The era mechanics are graded on field runs, not here.
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

// The fully armed case: an A4 ceiling (which auto-arms within_epoch), both
// responders, and the free-epoch response the erase half consumes.  phase_leveling
// is required: relevel_free_epoch frees leveling carries and disarms without it,
// which would disarm free_erase for an unrelated reason.
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
    // The ceiling auto-arms the within-epoch tier, which the policy depends on;
    // checked here so a change to that coupling fails in this file.
    EXPECT_TRUE(cfg.networkRaimWithinEpoch());
}

TEST(WithholdReleaseConfig, EscalateAtOrBelowStockBudgetRejectsToZero) {
    // (0, 5] is a no-op that would read as armed, so the loader zeroes it.
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
    // The latch is live (leveling and free_epoch armed), so the disarm below is
    // caused by the missing ceiling alone.
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
    // Transitive disarm: relevel_free_epoch itself disarms without
    // phase_leveling (it frees leveling carries), and the erase half disarms
    // with it rather than stay armed against a latch that can never fill.
    GnssConfig cfg;
    const std::string extra =
        "    \"raim_withhold_sigma0\": 6.0,\n"
        "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n"
        "    \"raim_withhold_free_erase\": true,\n";
    ASSERT_TRUE(loadInto("erase_nolvl", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelFreeEpoch());
    EXPECT_FALSE(cfg.networkRaimWithholdFreeErase());
}

TEST(WithholdReleaseConfig, FreeEraseDisarmsWhenCarryInvariantDisarmsTheLatch) {
    // The latch's other disarm: under network.carry_invariant the leveling local carry
    // is superseded and structurally empty, so relevel_free_epoch is disarmed (the
    // loader's re-level interlock, which runs after carry_invariant is parsed), and the
    // erase half disarms with it.  This is the only test of that interlock.
    // carry_invariant is a hard load error without phase_datum_eq34 (the DD cycle
    // basis it carries is defined only in that datum), so the posture is spelled out
    // completely; otherwise the case would assert on a failed load.
    GnssConfig cfg;
    const std::string extra =
        std::string(kArmedBlock) +
        "    \"carry_invariant\": true,\n"
        "    \"phase_datum_eq34\": true,\n";
    ASSERT_TRUE(loadInto("erase_carryinv", networkConfig(extra), cfg));
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
    // A ceiling in (0,1] is itself rejected to 0 (the R11/R1 guard); the
    // responders then disarm on the post-reject value, not the configured one.
    // This is why the interlocks sit at the end of the section (parse
    // order).
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
