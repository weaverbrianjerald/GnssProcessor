/**
 * @file test_windup_restart_response_config.cpp
 * @brief Config-contract tests for the wind-up accumulator-restart response
 *        (@c network.cycle_slip.windup_restart_refloat;
 *        `SPEC_phase_windup_sign.md` §10.4).
 *
 * The knob wires the invariant-#1 response to an accumulated-mode restart: an
 * exact integer-cycle step on every signal of the satellite, MW-blind (the
 * step is band-common, so ΔWL = 0), while the carried ambiguity and the
 * zero-q leveling level survive at their pre-gap values.  Two interlocks,
 * each a distinct armed-but-inert posture if missing (standing invariant #4):
 *
 *  - without @c estimation.accumulate_phase_windup there is no accumulator
 *    and therefore no restart verdict — the ledger the drain consumes is
 *    structurally empty (fractional mode WRAPS instead of restarting);
 *  - without @c network.phase_leveling there is no zero-q carried level for
 *    the restart to poison — the response would mark arcs whose stale value
 *    does not exist.
 *
 * Both are warn-and-disarm rather than load errors, matching the sibling
 * relevel knobs.  What is locked here is the END STATE of the flags; the
 * drain's own behaviour is graded on a run (counters + closure line), not
 * here.
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
    const fs::path p = fs::temp_directory_path() / ("wurestart_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& estimation_extra,
                          const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        + estimation_extra +
        "    \"estimate_tropo_gradients\": false\n"
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

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

const char* kNetArmed =
    "    \"phase_leveling\": true,\n"
    "    \"cycle_slip\": {\n"
    "      \"windup_restart_refloat\": true\n"
    "    },\n";

}  // namespace

TEST(WindupRestartResponseConfig, DefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig("", ""), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipWindupRestartRefloat());
}

TEST(WindupRestartResponseConfig, ArmsOnTheSupportedPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"accumulate_phase_windup\": true,\n", kNetArmed), cfg));
    EXPECT_TRUE(cfg.networkCycleSlipWindupRestartRefloat());
    EXPECT_TRUE(cfg.accumulatePhaseWindUp());
}

TEST(WindupRestartResponseConfig, DisarmsWithoutAccumulatedMode) {
    // Fractional wind-up WRAPS instead of restarting: the verdict the drain
    // responds to does not exist, so the armed knob is a structural zero.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("noacc", networkConfig("", kNetArmed), cfg));
    EXPECT_FALSE(cfg.accumulatePhaseWindUp());
    EXPECT_FALSE(cfg.networkCycleSlipWindupRestartRefloat());
}

TEST(WindupRestartResponseConfig, DisarmsWithoutPhaseLeveling) {
    // No zero-q carried level => nothing stale for the restart to poison.
    GnssConfig cfg;
    const char* net =
        "    \"phase_leveling\": false,\n"
        "    \"cycle_slip\": {\n"
        "      \"windup_restart_refloat\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("nolevel",
        networkConfig("    \"accumulate_phase_windup\": true,\n", net), cfg));
    EXPECT_TRUE(cfg.accumulatePhaseWindUp());
    EXPECT_FALSE(cfg.networkCycleSlipWindupRestartRefloat());
}

TEST(WindupRestartResponseConfig, ResponseDoesNotDragAccumulation) {
    // The knob must never arm accumulation as a side effect - the accumulate
    // flip is its own migration card (fingerprint semantics).
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nodrag", networkConfig("", kNetArmed), cfg));
    EXPECT_FALSE(cfg.accumulatePhaseWindUp());
}
