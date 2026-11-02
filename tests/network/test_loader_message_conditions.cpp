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
 *
 * The graph family of the eq34 chain (network.phase_datum_eq34_family) is read here too: its value,
 * and the two cases in which "pair" gives way to "signal" (no chain, pin bridge), print what they do.
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

// The graph family of the eq34 chain (network.phase_datum_eq34_family).  The default is "signal"
// and "pair" is accepted with the chain.
TEST(LoaderMessages, Eq34FamilyDefaultsToSignalAndTakesPairWithTheChain) {
    const LoadedConfig d = loadNetworkConfigCapturing("eq34_family_default",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n");
    EXPECT_TRUE(d.ok) << d.text;
    EXPECT_EQ(d.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(d.config.networkPhaseDatumEq34PairFamily());
    EXPECT_FALSE(contains(d.text, "PAIR FAMILY ON")) << d.text;

    const LoadedConfig p = loadNetworkConfigCapturing("eq34_family_pair",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(p.ok) << p.text;
    EXPECT_EQ(p.config.networkPhaseDatumEq34Family(), "pair");
    EXPECT_TRUE(p.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(p.text, "eq34 PAIR FAMILY ON")) << p.text;
}

// Any other value warns and reads "signal"; the match is exact, so a differently cased "Pair" is one.
TEST(LoaderMessages, Eq34FamilyUnknownValueWarnsAndReadsSignal) {
    for (const char* value : {"triple", "Pair", "signal,pair"}) {
        const LoadedConfig r = loadNetworkConfigCapturing(std::string("eq34_family_bad_") + value[0],
            "    \"phase_leveling\": true,\n"
            "    \"phase_datum_eq34\": true,\n"
            "    \"phase_datum_eq34_family\": \"" + std::string(value) + "\",\n");
        EXPECT_TRUE(r.ok) << value << ": " << r.text;
        EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal") << value;
        EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily()) << value;
        EXPECT_TRUE(contains(r.text, "network.phase_datum_eq34_family = '" + std::string(value)
                                         + "' is not one of signal|pair"))
            << value << ": " << r.text;
        EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << value << ": " << r.text;
    }
}

// "pair" without the chain has no election to group by band: it warns that it is inert and reads "signal".
TEST(LoaderMessages, Eq34FamilyPairIsInertWithoutTheChain) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_family_no_chain",
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(r.text, "phase_datum_eq34_family = 'pair' is set but .phase_datum_eq34 is OFF")) << r.text;
    EXPECT_TRUE(contains(r.text, "INERT")) << r.text;
    EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << r.text;
}

// The pin bridge grounds a satellite per (satellite, signal) key, which one shared tree cannot hold:
// "pair" with network.phase_datum_time_linked warns and falls back to "signal".
TEST(LoaderMessages, Eq34FamilyPairFallsBackUnderThePinBridge) {
    const LoadedConfig r = loadNetworkConfigCapturing("eq34_family_bridge",
        "    \"phase_leveling\": true,\n"
        "    \"phase_datum_eq34\": true,\n"
        "    \"eq34_anchor_min_arc_ep\": 3,\n"
        "    \"phase_datum_time_linked\": true,\n"
        "    \"phase_datum_eq34_family\": \"pair\",\n");
    EXPECT_TRUE(r.ok) << r.text;
    EXPECT_EQ(r.config.networkPhaseDatumEq34Family(), "signal");
    EXPECT_FALSE(r.config.networkPhaseDatumEq34PairFamily());
    EXPECT_TRUE(contains(r.text, "phase_datum_eq34_family = 'pair' with network.phase_datum_time_linked")) << r.text;
    EXPECT_TRUE(contains(r.text, "falling back to 'signal'")) << r.text;
    EXPECT_FALSE(contains(r.text, "PAIR FAMILY ON")) << r.text;
    // The bridge itself stays as configured: only the family gave way.
    EXPECT_TRUE(r.config.getNetworkConfig().phase_datum_time_linked);
}

}  // namespace GnssProcessor
