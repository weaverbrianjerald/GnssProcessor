/**
 * @file test_windup_census_config.cpp
 * @brief Contract tests for the phase wind-up census + continuity-horizon knobs
 *        (@c estimation.trace_windup_census, @c estimation.phase_windup_max_gap_s;
 *        `SPEC_phase_windup_sign.md` §7).
 *
 * Neither knob changes the solution at its default:
 *
 *  - @c phase_windup_max_gap_s replaces a hard-coded 300 s constant with the
 *    same default, so an absent key resolves to exactly 300.0; any other value
 *    would change the restart population of every existing accumulated-mode
 *    lineage.  A non-positive value falls back to the default instead of being
 *    used: a horizon of 0 restarts the accumulator every epoch, which reduces
 *    accumulated mode to fractional while the run still reports accumulated.
 *    That is an armed-but-inert posture (standing invariant #4).
 *  - @c trace_windup_census arms read-only counters.  It is useful because it
 *    can be armed in the fractional posture, which every field lineage runs and
 *    which no other instrument covers on the network path (@c windup_trace.csv
 *    is written only from @c Application::run).  The tests therefore check that
 *    arming the census does not change @c accumulate_phase_windup, and that
 *    arming accumulation does not arm the census.
 *
 * The census arithmetic is graded on a run, not here; these tests check that the
 * knobs cannot resolve to a posture that misreports itself.
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
    const fs::path p = fs::temp_directory_path() / ("wucensus_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// Minimal single-receiver config; the census is not network-specific.
std::string pppConfig(const std::string& estimation_extra = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        + estimation_extra +
        "    \"estimate_tropo_gradients\": false\n"
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

// ---------------------------------------------------------------------------
// Defaults: the behaviour-preserving contract.
// ---------------------------------------------------------------------------

TEST(WindUpCensusConfig, DefaultsAreBehaviourPreserving) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", pppConfig(), cfg));
    // 300.0 is the former hard-coded kMaxGapSeconds.  Changing the default moves
    // the restart population of every accumulated-mode lineage, so its products
    // are no longer comparable to its own history; that requires a migration card.
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 300.0);
    EXPECT_FALSE(cfg.traceWindUpCensus());
    // The posture the census characterises: absent key => fractional.
    EXPECT_FALSE(cfg.accumulatePhaseWindUp());
}

// ---------------------------------------------------------------------------
// The horizon: used when positive, replaced by the default when it would create an
// armed-but-inert posture.
// ---------------------------------------------------------------------------

TEST(WindUpCensusConfig, HorizonIsHonouredWhenPositive) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("horizon_ok",
                         pppConfig("    \"phase_windup_max_gap_s\": 900.0,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 900.0);
}

TEST(WindUpCensusConfig, NonPositiveHorizonRejectsToDefault) {
    // A zero/negative horizon makes `gap < max_gap_s` false for every gap, so
    // the accumulator restarts every epoch: accumulated mode reduces to
    // fractional while the summary line still reports "accumulated".
    for (const char* v : { "0.0", "-1.0", "-900.0" }) {
        GnssConfig cfg;
        const std::string body =
            pppConfig(std::string("    \"phase_windup_max_gap_s\": ") + v + ",\n");
        ASSERT_TRUE(loadInto("horizon_bad", body, cfg)) << v;
        EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 300.0) << v;
    }
}

// ---------------------------------------------------------------------------
// The census flag and the accumulation posture are independent in both
// directions, which makes the census usable as the fractional-mode instrument.
// ---------------------------------------------------------------------------

TEST(WindUpCensusConfig, CensusArmsWithoutTouchingAccumulation) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("census_frac",
                         pppConfig("    \"trace_windup_census\": true,\n"), cfg));
    EXPECT_TRUE(cfg.traceWindUpCensus());
    // If arming the census implied accumulation, the census could not measure
    // the fractional posture it was built for.
    EXPECT_FALSE(cfg.accumulatePhaseWindUp());
}

TEST(WindUpCensusConfig, AccumulationArmsWithoutImplyingCensus) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("acc_only",
                         pppConfig("    \"accumulate_phase_windup\": true,\n"), cfg));
    EXPECT_TRUE(cfg.accumulatePhaseWindUp());
    EXPECT_FALSE(cfg.traceWindUpCensus());
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 300.0);
}

TEST(WindUpCensusConfig, CensusAndAccumulationCoexist) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("both",
                         pppConfig("    \"trace_windup_census\": true,\n"
                                   "    \"accumulate_phase_windup\": true,\n"
                                   "    \"phase_windup_max_gap_s\": 120.0,\n"), cfg));
    EXPECT_TRUE(cfg.traceWindUpCensus());
    EXPECT_TRUE(cfg.accumulatePhaseWindUp());
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 120.0);
}

// ---------------------------------------------------------------------------
// Flat-alias parity.  The loader accepts a flat alias beside every dotted key;
// a knob that resolves under only one spelling breaks in config inheritance
// chains, where sections are merged at different depths.
// ---------------------------------------------------------------------------

TEST(WindUpCensusConfig, FlatAliasesResolve) {
    GnssConfig cfg;
    const std::string body =
        "{\n"
        "  \"processing\": { \"mode\": \"ppp\", \"active_systems\": [ \"GPS\" ],\n"
        "                   \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
        "  \"trace_windup_census\": true,\n"
        "  \"phase_windup_max_gap_s\": 600.0\n"
        "}\n";
    ASSERT_TRUE(loadInto("flat", body, cfg));
    EXPECT_TRUE(cfg.traceWindUpCensus());
    EXPECT_DOUBLE_EQ(cfg.phaseWindUpMaxGapS(), 600.0);
}
