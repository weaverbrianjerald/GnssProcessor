/**
 * @file test_lane_observatory_config.cpp
 * @brief Config-contract tests for the carry-domain lane observatory
 *        (@c network.cycle_slip.relevel_lane_snap; `SPEC_lane_slip_response.md` §7.3).
 *
 * Background.  The knob's first three mechanisms (rung 3 v1/v2/v2.1) acted at the
 * per-band ambiguity seed and were deleted after the write proved to be an exact solve
 * no-op on the network producer.  Their loader comment stated that the mechanism
 * worked with or without phase_leveling and needed no interlock; that statement let an
 * inert knob be armed in two configs and graded as a null result.  These tests keep
 * the interlocks in place.
 *
 * The v3 mechanism reads @c LocalCarry::value_abs (the leveling carry's absolute
 * levels) over the freed set, so it has two hard requirements and one hard exclusion,
 * each with a distinct failure mode:
 *
 *  - without @c phase_leveling there is no leveling carry to read at all;
 *  - without @c cycle_slip.relevel_free_epoch the freed set is empty, so every
 *    epoch short-circuits and the counters report a structural zero that is
 *    indistinguishable from dead wiring (standing invariant #4);
 *  - under @c carry_invariant the leveling local carry is superseded and
 *    structurally empty, as it is for the free-epoch response the observatory
 *    rides on.
 *
 * All three are warn-and-disarm rather than load errors, matching the sibling relevel
 * knobs.  The free-epoch rider is placed after the free_epoch disarm so that it reads
 * that flag's settled value.  The ordering is not separately observable through the
 * public getters, because every posture that settles free_epoch to false also disarms
 * the observatory by another route.  These tests check the end state; the ordering
 * rests on code placement and on the comment at the interlock.
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
    const fs::path p = fs::temp_directory_path() / ("laneobs_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

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

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// The armed happy path: leveling on, the free-epoch response on, observatory on.
const char* kArmed =
    "    \"phase_leveling\": true,\n"
    "    \"cycle_slip\": {\n"
    "      \"mw_detect\": true,\n"
    "      \"relevel_free_epoch\": true,\n"
    "      \"relevel_lane_snap\": true\n"
    "    },\n";

}  // namespace

TEST(LaneObservatoryConfig, DefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(""), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelLaneSnap());
}

TEST(LaneObservatoryConfig, ArmsOnTheSupportedPosture) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed", networkConfig(kArmed), cfg));
    EXPECT_TRUE(cfg.networkCycleSlipRelevelLaneSnap());
}

TEST(LaneObservatoryConfig, DisarmsWithoutPhaseLeveling) {
    // No leveling carry => no absolute per-band levels => nothing to difference.
    GnssConfig cfg;
    const char* extra =
        "    \"phase_leveling\": false,\n"
        "    \"cycle_slip\": {\n"
        "      \"relevel_free_epoch\": true,\n"
        "      \"relevel_lane_snap\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("noleveling", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelLaneSnap());
}

TEST(LaneObservatoryConfig, DisarmsWithoutFreeEpoch) {
    // free_ambs is the population; without it every counter is a structural
    // zero, which reads identically to a miswire.
    GnssConfig cfg;
    const char* extra =
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": {\n"
        "      \"mw_detect\": true,\n"
        "      \"relevel_free_epoch\": false,\n"
        "      \"relevel_lane_snap\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("nofree", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelLaneSnap());
}

TEST(LaneObservatoryConfig, DisarmsUnderCarryInvariant) {
    // The leveling local carry is superseded and structurally empty here, which is
    // also why relevel_free_epoch itself is disarmed on this posture.
    GnssConfig cfg;
    // carry_invariant is a hard load error without phase_datum_eq34 (the DD cycle
    // basis it carries is defined only in that datum), so the posture is spelled out
    // completely; otherwise the test would assert on a failed load rather than on the
    // interlock under test.
    const std::string extra =
        std::string(kArmed) +
        "    \"carry_invariant\": true,\n"
        "    \"phase_datum_eq34\": true,\n";
    ASSERT_TRUE(loadInto("carryinv", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelLaneSnap());
    // Its base dies on the same posture; if this ever changes, the rider's
    // ordering assumption needs re-checking.
    EXPECT_FALSE(cfg.networkCycleSlipRelevelFreeEpoch());
}

TEST(LaneObservatoryConfig, DisarmsWhenFreeEpochIsJsonTrueButUnsupported) {
    // relevel_free_epoch is set true in JSON but is disarmed for lack of
    // phase_leveling; the observatory is off either way.
    //
    // Scope: this case does not isolate the rider's ordering.  The phase_leveling
    // interlock disarms the observatory directly, before the free-epoch rider is
    // consulted, so the first interlock alone satisfies this case, and it would still
    // pass if the rider read the raw parsed value.  The ordering is enforced by code
    // placement (the rider sits after the free_epoch disarm) and is not separately
    // observable through the public getters: every posture that settles free_epoch
    // to false also disarms the observatory by another route.  The case is an
    // end-state contract, not an ordering proof; DisarmsWithoutFreeEpoch is the case
    // that exercises the rider.
    GnssConfig cfg;
    const char* extra =
        "    \"phase_leveling\": false,\n"
        "    \"cycle_slip\": {\n"
        "      \"relevel_free_epoch\": true,\n"
        "      \"relevel_lane_snap\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("settled", networkConfig(extra), cfg));
    EXPECT_FALSE(cfg.networkCycleSlipRelevelFreeEpoch());
    EXPECT_FALSE(cfg.networkCycleSlipRelevelLaneSnap());
}
