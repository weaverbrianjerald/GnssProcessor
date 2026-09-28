/**
 * @file test_raim_carry_invariant_pool.cpp
 * @brief Config-contract tests for the RAIM pool re-host
 *        (docs/SPEC_raim_pool_rehost.md).
 *
 * Without the re-host, post-fit network RAIM does nothing on a Card-4
 * invariant-carry lineage, for two independent reasons, neither of them a
 * tunable threshold:
 *
 *   1. The seam is not called.  Its call site reads `if (raim_on && carry_warm)`,
 *      and `carry_warm` requires network.phase_leveling, so a Card-4 config that
 *      leaves leveling off never executes networkPostFitRaim.
 *   2. With leveling on, the seam's residual pool is empty.  Every RAIM walk keys
 *      on StationAssembly::pl_amb, whose only fill site is nested inside
 *      `do_amb_carry = (…) && !carry_invariant`: Card 4's DD carry supersedes the
 *      phase-leveling ambiguity carry, so the record list RAIM reads is never
 *      built.
 *
 * The re-host moves the QC substrate onto StationAssembly::qc_amb, a census that
 * does not depend on the carry vehicle, and defers the refloat one epoch through
 * NetworkCarryState::raim_refloat_pending (the funnel is consumed at SB_DDBUILD,
 * before RAIM detects at SB_RAIM).
 *
 * This file is the config-contract half: the knobs default off, the inert guards
 * fire on every posture where an arm could not change the output, and the two
 * knobs are strictly ordered (actuator requires screen requires lineage).  The
 * numerical half (census contents, funnel provenance, the A_old membership edge)
 * needs the full assembly harness and is owed as field gates 1-3 in the SPEC.
 *
 * Suite ownership: `RaimCarryInvariantPool` lives only in this file.
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
    const fs::path p = fs::temp_directory_path() / ("raim_ci_pool_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p network_extra is spliced into the
/// network block (each entry must end with a comma).
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

/// The Card-4 lineage: carry_invariant takes effect only with phase_datum_eq34, which
/// is why the runtime knob ANDs both.  Every "armed" posture below builds on this.
constexpr const char* kCard4 =
    "    \"phase_datum_eq34\": true,\n"
    "    \"carry_invariant\": true,\n"
    "    \"raim\": true,\n";

} // namespace

// ---------------------------------------------------------------------------
// Default off / structural byte-identity (standing invariant #6).
// ---------------------------------------------------------------------------

// Neither knob defaults on: many shipped configs (about 200) arm carry_invariant,
// and a default flip would change all of them at once.
TEST(RaimCarryInvariantPool, BothKnobsDefaultOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariantRefloat());
    std::error_code ec; fs::remove(p, ec);
}

// The existing Card-4 + RAIM posture (the one about 70 _ft63_* configs ship) loads
// as before: RAIM nominally on, the re-host off, nothing re-hosted.  This guards
// against the re-host changing any default behaviour.
TEST(RaimCarryInvariantPool, LegacyCard4PostureUnchanged) {
    LogManager log;
    const fs::path p = writeTempConfig("legacy", networkConfig(kCard4));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaim());                       // still nominally armed
    EXPECT_TRUE(cfg.networkCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());        // …and still inert, by default
    EXPECT_FALSE(cfg.networkRaimCarryInvariantRefloat());
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// The armed posture.
// ---------------------------------------------------------------------------

TEST(RaimCarryInvariantPool, ArmsOnCard4Lineage) {
    LogManager log;
    const fs::path p = writeTempConfig("armed",
        networkConfig(std::string(kCard4) + "    \"raim_carry_invariant\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaimCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariantRefloat());  // screen without actuator
    std::error_code ec; fs::remove(p, ec);
}

TEST(RaimCarryInvariantPool, ActuatorArmsOnTopOfScreen) {
    LogManager log;
    const fs::path p = writeTempConfig("actuator",
        networkConfig(std::string(kCard4) +
                      "    \"raim_carry_invariant\": true,\n"
                      "    \"raim_carry_invariant_refloat\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaimCarryInvariant());
    EXPECT_TRUE(cfg.networkRaimCarryInvariantRefloat());
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// Inert guards (standing invariant #4: an arm that cannot change the output is
// disabled at load, not discovered after a field run).
// ---------------------------------------------------------------------------

// Off the Card-4 lineage, pl_amb is already the correct pool and re-hosting would be
// a no-op at best, so the loader disables the knob rather than carrying it silently.
TEST(RaimCarryInvariantPool, ScreenDisabledWithoutCarryInvariant) {
    LogManager log;
    const fs::path p = writeTempConfig("nocarry",
        networkConfig("    \"raim\": true,\n"
                      "    \"phase_leveling\": true,\n"
                      "    \"raim_carry_invariant\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());   // auto-disabled
    std::error_code ec; fs::remove(p, ec);
}

// There is no screen to re-host if RAIM itself is off.
TEST(RaimCarryInvariantPool, ScreenDisabledWithoutRaim) {
    LogManager log;
    const fs::path p = writeTempConfig("noraim",
        networkConfig("    \"phase_datum_eq34\": true,\n"
                      "    \"carry_invariant\": true,\n"
                      "    \"raim_carry_invariant\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRaim());
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());   // auto-disabled
    std::error_code ec; fs::remove(p, ec);
}

// The knobs are strictly ordered.  Arming the actuator alone is the most harmful
// misconfiguration to carry silently, because the seam's two legacy actions look
// like a refloat in the log while changing nothing: the local_carry erase drops an
// entry no Card-4 consumer reads, and the pending-reseed mark leaves the reduced
// shared RHS algebraically unchanged (SPEC_deweight_release_hygiene §7.2).
TEST(RaimCarryInvariantPool, ActuatorDisabledWithoutScreen) {
    LogManager log;
    const fs::path p = writeTempConfig("bareactuator",
        networkConfig(std::string(kCard4) +
                      "    \"raim_carry_invariant_refloat\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariantRefloat());   // auto-disabled
    std::error_code ec; fs::remove(p, ec);
}

// Cascade: withdrawing the lineage withdraws both knobs, not only the screen;
// otherwise the actuator would survive the loss of its own precondition.
TEST(RaimCarryInvariantPool, ActuatorCascadesOffWithLineage) {
    LogManager log;
    const fs::path p = writeTempConfig("cascade",
        networkConfig("    \"raim\": true,\n"
                      "    \"phase_leveling\": true,\n"
                      "    \"raim_carry_invariant\": true,\n"
                      "    \"raim_carry_invariant_refloat\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariant());
    EXPECT_FALSE(cfg.networkRaimCarryInvariantRefloat());
    std::error_code ec; fs::remove(p, ec);
}

// carry_invariant without phase_datum_eq34 is a hard load error, not a silently
// inert posture: the DD cycle-basis carry is defined only on the eq34 chain datum,
// so the loader's "network.carry_invariant requires network.phase_datum_eq34"
// check (GnssConfigLoadNetwork.cpp) refuses the config.
//
// For the re-host, this means the eq34 conjunct in
// `k.raim_ci_on = raim_on && k.carry_invariant && …` never decides anything for a
// loadable config.  `k.carry_invariant` ANDs eq34 in (NetworkKnobs::snapshot in
// NetworkRunContext.cpp) and the knob inherits that, but any config that could
// exercise the difference is already rejected here.  The conjunct is defensive redundancy that matches how
// every other carry_invariant-derived knob resolves, not a live discriminator.  The
// test pins the hard error so that, if the requirement is relaxed to a warning, the
// runtime resolution becomes load-bearing and this case fails.
TEST(RaimCarryInvariantPool, CarryInvariantWithoutEq34IsAHardLoadError) {
    LogManager log;
    const fs::path p = writeTempConfig("noeq34",
        networkConfig("    \"carry_invariant\": true,\n"
                      "    \"raim\": true,\n"
                      "    \"raim_carry_invariant\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));   // rejected: carry_invariant needs eq34
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// Emission-affecting interaction (the risk the SPEC calls out first).
// ---------------------------------------------------------------------------

// A "measure-only" arm is not emission-neutral: with the pool empty, sigma0 stays
// at its 1.0 initializer, so filling the pool makes sigma0 data-driven, and sigma0
// feeds both the A4 withhold and the URA EWMA.  The loader warns; the contract is
// that arming the screen does not silently disarm the withhold (the operator
// decides, not the loader).
TEST(RaimCarryInvariantPool, ScreenDoesNotDisarmWithholdSilently) {
    LogManager log;
    const fs::path p = writeTempConfig("withhold",
        networkConfig(std::string(kCard4) +
                      "    \"raim_carry_invariant\": true,\n"
                      "    \"raim_within_epoch\": true,\n"
                      "    \"raim_withhold_sigma0\": 6.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkRaimCarryInvariant());
    EXPECT_DOUBLE_EQ(cfg.networkRaimWithholdSigma0(), 6.0);   // left alone, deliberately
    std::error_code ec; fs::remove(p, ec);
}
