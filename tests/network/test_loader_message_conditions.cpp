/**
 * @file test_loader_message_conditions.cpp
 * @brief Loader messages state the behaviour the code has, under the conditions it has it.
 *
 * Two printed statements were wrong:
 *  - The eq34 hygiene warnings ("phase_datum_eq34_pin_round / carry_shift / rpb_q_m2s is
 *    INERT under carry_invariant") printed for every eq34 run. Those knobs are gated off only
 *    when the invariant carry is armed; on the float vehicle they are live.
 *  - The network.sd_ewl.enable line said "no integer fixing", although
 *    network.sd_ewl.fix_enable provides the integer decision.
 */

#include <gtest/gtest.h>

#include "ConfigLoadCapture.hpp"

namespace GnssProcessor {
namespace {

using TestSupport::contains;
using TestSupport::LoadedConfig;
using TestSupport::loadNetworkConfigCapturing;

}  // namespace

// The eq34 hygiene knobs are live on the float vehicle: no "INERT under carry_invariant".
TEST(LoaderMessages, Eq34HygieneNotInertWithoutInvariantCarry) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_float",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"phase_datum_eq34_pin_round\": true,\n"
        "    \"phase_datum_eq34_carry_shift\": true,\n"
        "    \"phase_datum_eq34_rpb_q_m2s\": 3e-6,\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "Eq-34 SINGLE-RECEIVER-CHAIN phase-bias datum ON")) << r.text;
    EXPECT_FALSE(contains(r.text, "INERT under carry_invariant")) << r.text;
}

// Under the invariant carry the same knobs are gated off, and the warnings stay.
TEST(LoaderMessages, Eq34HygieneInertUnderInvariantCarry) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_inv",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"carry_invariant\": true,\n"
        "    \"carry_invariant_qz\": 1e-6,\n"
        "    \"phase_datum_eq34_pin_round\": true,\n"
        "    \"phase_datum_eq34_carry_shift\": true,\n"
        "    \"phase_datum_eq34_rpb_q_m2s\": 3e-6,\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_pin_round is INERT under carry_invariant"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_carry_shift is INERT under carry_invariant"))
        << r.text;
    EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_rpb_q_m2s is INERT under carry_invariant"))
        << r.text;
}

// The sd_ewl.enable line points at fix_enable instead of denying integer fixing.
TEST(LoaderMessages, SdEwlEnableLineDoesNotDenyFixing) {
    const LoadedConfig r = loadNetworkConfigCapturing("sd_ewl",
        "    \"sd_ewl\": { \"enable\": true },\n");
    EXPECT_TRUE(contains(r.text, "SD PRE-REDUCE EWL constraint ON")) << r.text;
    EXPECT_TRUE(contains(r.text, "integer decisions come only from network.sd_ewl.fix_enable"))
        << r.text;
    EXPECT_FALSE(contains(r.text, "no integer fixing")) << r.text;
}

}  // namespace GnssProcessor
