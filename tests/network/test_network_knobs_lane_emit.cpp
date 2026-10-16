/**
 * @file test_network_knobs_lane_emit.cpp
 * @brief The invariant-carry gate and emit-layer switches of NetworkKnobs (card NOM step
 *        TD1a) equal the conjunctions runNetwork computed before they moved.
 *
 * Card NOM step TD1a moved 24 derived switches (carry_innov_gate, the WL/EWL/UWL lane
 * flags, the dump flags, emit_rebase_on, and the two PB emission-filter switches) out of
 * the driver and into NetworkKnobs::snapshot, so the driver, the DD-prior build, the
 * install, the emit store and the run end read one bool each.  The lane and dump flags
 * went with the retired MW/WL lane keys (cards WL-1, WL-2, WL-3, UWL-1), the two filter
 * switches with the PB emission-continuity filter.  The conjunctions here are the
 * driver's, copied from ApplicationNetwork.cpp as it stood at main 3c44046; every posture
 * checks each knob against them, and each armed posture also checks that the switches it
 * targets are on, so a posture the loader quietly disarms cannot pass by reading
 * all-false on both sides.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal two-station GPS network configuration with @p network_extra in its network
/// section.
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

/// Write @p body to a temporary file and load it into @p cfg.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("knobs_lane_emit_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The invariant-carry posture: the eq34 datum, the carry, and the leveling carry with its
/// slip detector (carry_invariant is a load error without eq34).
const char* kCarryInvariant =
    "    \"phase_datum_eq34\": true,\n"
    "    \"carry_invariant\": true,\n"
    "    \"phase_leveling\": true,\n"
    "    \"cycle_slip\": { \"mw_detect\": true },\n";

/// The switches still in the snapshot, as the driver computed them before card NOM step
/// TD1a (the MW/WL lane and dump switches and the two PB emission-filter switches went
/// with their keys).
struct DriverSwitches {
    bool carry_innov_gate, emit_rebase_on;
};

DriverSwitches driverSwitches(const GnssConfig& config_) {
    // The driver read carry_invariant from the knob snapshot, which resolves it as below.
    const bool carry_invariant = config_.networkPhaseDatumEq34() && config_.networkCarryInvariant();
    DriverSwitches d{};
    d.carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    d.emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    return d;
}

/// Every switch of the snapshot equals the driver's conjunction; returns the snapshot.
NetworkKnobs expectDriverSwitches(const GnssConfig& cfg) {
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    const DriverSwitches d = driverSwitches(cfg);
    EXPECT_EQ(k.carry_innov_gate, d.carry_innov_gate);
    EXPECT_EQ(k.emit_rebase_on, d.emit_rebase_on);
    return k;
}

}  // namespace

TEST(NetworkKnobsLaneEmit, DefaultsAreAllOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(""), cfg));
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_FALSE(k.carry_innov_gate);
    EXPECT_FALSE(k.emit_rebase_on);
}

TEST(NetworkKnobsLaneEmit, CarryInvariantArmsTheEmitReBaseByDefault) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ci", networkConfig(kCarryInvariant), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.emit_rebase_on);   // on by default under carry_invariant (spec D7)
}

TEST(NetworkKnobsLaneEmit, InnovationGatePostureArmsTheGate) {
    // The armed counterpart of the gate switch: without it the mirror compares false with
    // false on every posture, and a loader that quietly disarmed the gate would pass.
    GnssConfig cfg;
    const std::string extra = std::string(kCarryInvariant) +
        "    \"carry_invariant_innov_gate\": true,\n";
    ASSERT_TRUE(loadInto("innov_gate", networkConfig(extra), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    ASSERT_TRUE(cfg.networkCarryInvariantInnovGate());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.carry_innov_gate);
    EXPECT_TRUE(k.emit_rebase_on);
}
