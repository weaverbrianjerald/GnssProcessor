/**
 * @file test_service_mode_clock_refusals.cpp
 * @brief The clock_model refusals judge the effective configuration, service_mode included.
 *
 * network.service_mode force-arms network.clock_model late in the network loader. The
 * refusals of network.clock_pivot_frame_q and network.carry_coupling_clock_clone used to
 * read the raw JSON key at their parse sites, so a service_mode config that pairs either
 * knob with the oscillator the profile arms was accepted and ran a combination the
 * loader refuses everywhere else. Both refusals now run after the profile has settled
 * the oscillator.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"

namespace GnssProcessor {
namespace {

using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

}  // namespace

// Control: the explicit pairing is refused, with the original wording.
TEST(ServiceModeClockRefusals, ExplicitClockModelWithPivotFrameQIsRefused) {
    const LoadedConfig r = loadNetworkConfigCapturing("pq_explicit",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"clock_pivot_frame_q\": true,\n"
        "    \"clock_model\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text,
        "network.clock_pivot_frame_q refuses network.clock_model (the clock model carries no "
        "satellite clock in the shared border)."))
        << r.text;
}

// service_mode arms the oscillator itself; the refusal must still fire.
TEST(ServiceModeClockRefusals, ServiceModeArmedClockModelWithPivotFrameQIsRefused) {
    const LoadedConfig r = loadNetworkConfigCapturing("pq_service",
        "    \"service_mode\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"clock_pivot_frame_q\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text, "network.clock_pivot_frame_q refuses network.clock_model"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "force-armed by network.service_mode")) << r.text;
}

TEST(ServiceModeClockRefusals, ServiceModeArmedClockModelWithClockCloneIsRefused) {
    const LoadedConfig r = loadNetworkConfigCapturing("clone_service",
        "    \"service_mode\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text, "network.carry_coupling_clock_clone refuses network.clock_model"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "force-armed by network.service_mode")) << r.text;
}

// Controls for the clone config above: without the oscillator it loads, and the explicit
// pairing is refused with the original wording.
TEST(ServiceModeClockRefusals, ClockCloneConfigLoadsWithoutClockModel) {
    const LoadedConfig r = loadNetworkConfigCapturing("clone_plain",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n");
    EXPECT_TRUE(r.ok) << r.text;
}

TEST(ServiceModeClockRefusals, ExplicitClockModelWithClockCloneIsRefused) {
    const LoadedConfig r = loadNetworkConfigCapturing("clone_explicit",
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"carry_coupling_clock_clone\": true,\n"
        "    \"clock_model\": true,\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(contains(r.text,
        "network.carry_coupling_clock_clone refuses network.clock_model (the clock model "
        "carries no satellite clock in the dense border, so there is nothing to clone)."))
        << r.text;
}

// service_mode alone, with neither refused knob, still loads (the profile is untouched).
TEST(ServiceModeClockRefusals, ServiceModeAloneStillLoads) {
    const LoadedConfig r = loadNetworkConfigCapturing("svc_alone", "    \"service_mode\": true,\n");
    EXPECT_TRUE(r.ok) << r.text;
}

}  // namespace GnssProcessor
