/**
 * @file test_lane_observatory_config.cpp
 * @brief Config-contract tests for the carry-domain lane observatory
 *        (@c network.cycle_slip.relevel_lane_snap; `SPEC_lane_slip_response.md` §7.3).
 *
 * History this locks in place.  The knob's first three mechanisms (rung 3
 * v1/v2/v2.1) lived at the per-band ambiguity SEED and were deleted once the
 * write was shown to be an exact solve no-op on the network producer.  The
 * loader comment that shipped with them said the mechanism "works with or
 * without phase_leveling (the seed path is universal), so no interlock" — and
 * that sentence is precisely what let an inert knob be armed in two configs and
 * graded as a null result.
 *
 * The v3 mechanism reads @c LocalCarry::value_abs (the LEVELING carry's
 * absolute levels) over the freed set, so it has two hard requirements and one
 * hard exclusion.  Each has a distinct failure mode if it is missing:
 *
 *  - without @c phase_leveling there is no leveling carry to read at all;
 *  - without @c cycle_slip.relevel_free_epoch the freed set is empty, so every
 *    epoch short-circuits and the counters report a STRUCTURAL zero that is
 *    indistinguishable from dead wiring (standing invariant #4);
 *  - under @c carry_invariant the leveling local carry is superseded and
 *    structurally empty, exactly as for the free-epoch response it rides on.
 *
 * All three are warn-and-disarm rather than load errors, matching the sibling
 * relevel knobs.  The free-epoch rider is deliberately placed AFTER the
 * free_epoch disarm so it reads that flag's SETTLED value — but note that
 * ordering is not separately observable through the public getters, because
 * every posture that settles free_epoch to false also disarms the observatory by
 * another route.  These tests pin the END STATE; the ordering rests on code
 * placement and on the comment at the interlock.
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
    // free_ambs IS the population; without it every counter is a structural
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
    // The leveling local carry is superseded and structurally empty here — the
    // same reason relevel_free_epoch itself is disarmed on this posture.
    GnssConfig cfg;
    // carry_invariant is a hard load ERROR without phase_datum_eq34 (the DD
    // cycle basis it carries is defined only in that datum), so the posture has
    // to be spelled completely — otherwise this would assert on a failed load
    // rather than on the interlock it is here to pin.
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
    // relevel_free_epoch is set TRUE in JSON but cannot survive for want of
    // phase_leveling; the observatory must be off either way.
    //
    // ⚠ HONEST SCOPE.  This does NOT isolate the rider's ordering: the
    // phase_leveling interlock disarms the observatory directly, before the
    // free-epoch rider is ever consulted, so this case is satisfied by the first
    // interlock alone and would still pass if the rider read the RAW parsed
    // value.  The ordering is enforced by code placement (the rider sits after
    // the free_epoch disarm) and is not separately observable through the public
    // getters — every posture that settles free_epoch to false also disarms the
    // observatory by another route.  Kept as an end-state contract, not as an
    // ordering proof; DisarmsWithoutFreeEpoch is the case that actually
    // exercises the rider.
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
