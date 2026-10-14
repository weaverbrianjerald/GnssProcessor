/**
 * @file test_network_knobs_lane_emit.cpp
 * @brief The invariant-carry gate, lane and emit-layer switches of NetworkKnobs (card NOM
 *        step TD1a) equal the conjunctions runNetwork computed before they moved.
 *
 * Card NOM step TD1a moved 24 derived switches (carry_innov_gate, the WL/EWL/UWL lane
 * flags, the dump flags, emit_rebase_on, emit_pb_filter_on, pb_filter_emit_sigma) out of
 * the driver and into NetworkKnobs::snapshot (the driver's ewl_carry_exclude was the same
 * conjunction as ewl_on and is read as ewl_on), so the driver, the DD-prior build, the
 * install, the emit store and the run end read one bool each.  The conjunctions here are
 * the driver's, copied from ApplicationNetwork.cpp as it stood at main 3c44046; every
 * posture checks each knob against them, and each armed posture also checks that the
 * switches it targets are on, so a posture the loader quietly disarms cannot pass by
 * reading all-false on both sides.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
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

/// The invariant-carry posture the lane cards run on: the eq34 datum, the carry, and the
/// leveling carry with its slip detector (carry_invariant is a load error without eq34).
const char* kCarryInvariant =
    "    \"phase_datum_eq34\": true,\n"
    "    \"carry_invariant\": true,\n"
    "    \"phase_leveling\": true,\n"
    "    \"cycle_slip\": { \"mw_detect\": true },\n";

/// The switches still in the snapshot, as the driver computed them before card NOM step
/// TD1a (the WL-1 switches mw_wl_on and mw_wl_f6_dump went with their keys).
struct DriverSwitches {
    bool carry_innov_gate, lane_on, lane_joint, ewl_on, uwl_on, uwl_cold_on,
         bridge_debias_on, uwl_mid_on, uwl_dual_on, uwl_sib_gate_on, uwl_family_on,
         wl_inject_on, lane_meas_on, lane_bridge_on, lane_fix_on, lane_parallel_components,
         mw_d5_dump, mw_lane_trace, mw_tagsets, emit_rebase_on,
         emit_pb_filter_on, pb_filter_emit_sigma;
};

DriverSwitches driverSwitches(const GnssConfig& config_) {
    // The driver read carry_invariant from the knob snapshot, which resolves it as below.
    const bool carry_invariant = config_.networkPhaseDatumEq34() && config_.networkCarryInvariant();
    const auto& lane_cfg = config_.getNetworkConfig().lane_carry;
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    DriverSwitches d{};
    d.carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    d.lane_on = carry_invariant && lane_cfg.enable;
    d.lane_joint = d.lane_on
        && (lane_cfg.q_wl_scale != 1.0 || lane_cfg.sigma_wl_fresh_cyc > 0.0
            || lane_cfg.lane_meas || lane_cfg.bridge_meas);
    d.ewl_on = carry_invariant && nmw.ewl_enable;
    d.uwl_on = d.ewl_on && nmw.uwl_enable;
    d.uwl_cold_on = d.uwl_on && nmw.uwl_cold_fix;
    d.bridge_debias_on = d.ewl_on && nmw.bridge_sample_debias;
    d.uwl_mid_on = d.uwl_on && nmw.uwl_mid_enable;
    d.uwl_dual_on = d.uwl_on && nmw.uwl_dual_top;
    d.uwl_sib_gate_on = d.uwl_dual_on && nmw.uwl_sib_gate;
    d.uwl_family_on = d.uwl_on && d.lane_on && nmw.uwl_family_pairs;
    d.wl_inject_on = d.ewl_on && nmw.wl_inject_enable && !d.lane_on;
    d.lane_meas_on = d.lane_on && d.lane_joint && lane_cfg.lane_meas;
    d.lane_bridge_on = d.lane_on && d.lane_joint && lane_cfg.bridge_meas;
    d.lane_fix_on = d.lane_on && d.lane_joint;
    d.lane_parallel_components = d.lane_on && d.lane_joint && lane_cfg.parallel_components;
    d.mw_d5_dump = (d.ewl_on || d.wl_inject_on || d.lane_on) && nmw.d5_dump;
    d.mw_lane_trace = (d.wl_inject_on || d.lane_fix_on) && nmw.lane_trace;
    d.mw_tagsets = d.mw_d5_dump || d.mw_lane_trace;
    d.emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    d.emit_pb_filter_on = d.emit_rebase_on && config_.networkEmitPbFilter();
    d.pb_filter_emit_sigma = config_.networkEmitPbFilterEmitSigma();
    return d;
}

/// Every switch of the snapshot equals the driver's conjunction; returns the snapshot.
NetworkKnobs expectDriverSwitches(const GnssConfig& cfg) {
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    const DriverSwitches d = driverSwitches(cfg);
    EXPECT_EQ(k.carry_innov_gate, d.carry_innov_gate);
    EXPECT_EQ(k.lane_on, d.lane_on);
    EXPECT_EQ(k.lane_joint, d.lane_joint);
    EXPECT_EQ(k.ewl_on, d.ewl_on);
    EXPECT_EQ(k.uwl_on, d.uwl_on);
    EXPECT_EQ(k.uwl_cold_on, d.uwl_cold_on);
    EXPECT_EQ(k.bridge_debias_on, d.bridge_debias_on);
    EXPECT_EQ(k.uwl_mid_on, d.uwl_mid_on);
    EXPECT_EQ(k.uwl_dual_on, d.uwl_dual_on);
    EXPECT_EQ(k.uwl_sib_gate_on, d.uwl_sib_gate_on);
    EXPECT_EQ(k.uwl_family_on, d.uwl_family_on);
    EXPECT_EQ(k.wl_inject_on, d.wl_inject_on);
    EXPECT_EQ(k.lane_meas_on, d.lane_meas_on);
    EXPECT_EQ(k.lane_bridge_on, d.lane_bridge_on);
    EXPECT_EQ(k.lane_fix_on, d.lane_fix_on);
    EXPECT_EQ(k.lane_parallel_components, d.lane_parallel_components);
    EXPECT_EQ(k.mw_d5_dump, d.mw_d5_dump);
    EXPECT_EQ(k.mw_lane_trace, d.mw_lane_trace);
    EXPECT_EQ(k.mw_tagsets, d.mw_tagsets);
    EXPECT_EQ(k.emit_rebase_on, d.emit_rebase_on);
    EXPECT_EQ(k.emit_pb_filter_on, d.emit_pb_filter_on);
    EXPECT_EQ(k.pb_filter_emit_sigma, d.pb_filter_emit_sigma);
    return k;
}

}  // namespace

TEST(NetworkKnobsLaneEmit, DefaultsAreAllOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(""), cfg));
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_FALSE(k.carry_innov_gate);
    EXPECT_FALSE(k.lane_on);
    EXPECT_FALSE(k.ewl_on);
    EXPECT_FALSE(k.emit_rebase_on);
    EXPECT_FALSE(k.emit_pb_filter_on);
}

TEST(NetworkKnobsLaneEmit, CarryInvariantArmsTheEmitReBaseByDefault) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("ci", networkConfig(kCarryInvariant), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.emit_rebase_on);   // on by default under carry_invariant (spec D7)
    EXPECT_FALSE(k.ewl_on);
    EXPECT_FALSE(k.lane_on);
}

TEST(NetworkKnobsLaneEmit, EwlUwlInjectAndEmitFilterPosture) {
    GnssConfig cfg;
    const std::string extra = std::string(kCarryInvariant) +
        "    \"carry_invariant_innov_gate\": true,\n"
        "    \"emit_pb_filter\": true,\n"
        "    \"emit_pb_filter_emit_sigma\": true,\n"
        "    \"mw_wl\": {\n"
        "      \"ewl_enable\": true,\n"
        "      \"uwl_enable\": true,\n"
        "      \"uwl_cold_fix\": true,\n"
        "      \"bridge_sample_debias\": true,\n"
        "      \"uwl_mid_enable\": true,\n"
        "      \"uwl_dual_top\": true,\n"
        "      \"uwl_sib_gate\": true,\n"
        "      \"wl_inject_enable\": true,\n"
        "      \"d5_dump\": true,\n"
        "      \"lane_trace\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("inject", networkConfig(extra), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.carry_innov_gate);
    EXPECT_TRUE(k.ewl_on);
    EXPECT_TRUE(k.uwl_on);
    EXPECT_TRUE(k.wl_inject_on);
    EXPECT_FALSE(k.lane_on);
    EXPECT_TRUE(k.mw_tagsets);
    EXPECT_TRUE(k.emit_pb_filter_on);
    EXPECT_TRUE(k.pb_filter_emit_sigma);
}

TEST(NetworkKnobsLaneEmit, LaneCarrySupersedesTheInject) {
    GnssConfig cfg;
    const std::string extra = std::string(kCarryInvariant) +
        "    \"mw_wl\": {\n"
        "      \"ewl_enable\": true,\n"
        "      \"uwl_enable\": true,\n"
        "      \"uwl_family_pairs\": true,\n"
        "      \"wl_inject_enable\": true,\n"
        "      \"lane_trace\": true\n"
        "    },\n"
        "    \"lane_carry\": {\n"
        "      \"enable\": true,\n"
        "      \"lane_meas\": true,\n"
        "      \"bridge_meas\": true,\n"
        "      \"parallel_components\": true\n"
        "    },\n";
    ASSERT_TRUE(loadInto("lane", networkConfig(extra), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.lane_on);
    EXPECT_TRUE(k.lane_joint);
    EXPECT_TRUE(k.lane_fix_on);
    EXPECT_TRUE(k.uwl_family_on);
    EXPECT_FALSE(k.wl_inject_on);   // WL-3 supersedes the WL-2 inject
}

TEST(NetworkKnobsLaneEmit, LaneCarryAtItsSentinelsIsNotTheJointShape) {
    // Every lane feature at its isotropic or off sentinel: the §6.1 equivalence shape,
    // so lane_on holds and the joint shape and its fix walk do not.
    GnssConfig cfg;
    const std::string extra = std::string(kCarryInvariant) +
        "    \"mw_wl\": { \"ewl_enable\": true },\n"
        "    \"lane_carry\": {\n"
        "      \"enable\": true,\n"
        "      \"q_wl_scale\": 1.0,\n"
        "      \"sigma_wl_fresh_cyc\": 0.0,\n"
        "      \"lane_meas\": false,\n"
        "      \"bridge_meas\": false\n"
        "    },\n";
    ASSERT_TRUE(loadInto("sentinel", networkConfig(extra), cfg));
    ASSERT_TRUE(cfg.networkCarryInvariant());
    const NetworkKnobs k = expectDriverSwitches(cfg);
    EXPECT_TRUE(k.lane_on);
    EXPECT_FALSE(k.lane_joint);
    EXPECT_FALSE(k.lane_fix_on);
}
