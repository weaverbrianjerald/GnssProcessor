/**
 * @file test_d5_corroborate_autoarm.cpp
 * @brief Loader-contract tests for the D5 v3/v4 evidence interlock: the auto-arm that
 *        keeps the corroboration knobs from being silently inert.
 *
 * The D5 v3 suppression set and the v4 pending-flat set (both in
 * @c InstallCarry.cpp) are built from @c slip_qc.epoch.verdict / @c .stepped.
 * The only writer of those is the slip-QC qualify pass, which
 * @c ApplicationNetworkAssembly.cpp runs only when a decision consumer is armed:
 * @c SlipQcState::decisionInstrumentArmed() = @c lli_corroborate || @c gap_instrument ||
 * @c bridge_gf || @c iono_cs.
 *
 * @c network.trace_slip_raw satisfies @c SlipQcParams::anyConsumer but not
 * @c decisionInstrumentArmed().  It arms the compute pass only, which sets @c epoch.computed = true
 * and sizes @c epoch.verdict to n_sta with empty maps.  The consumers gate on
 * @c epoch.computed, so with the trace alone they run, every per-satellite lookup
 * misses, and the suppression set is empty: the gate logs as armed and does nothing
 * (standing invariant #4).  The gap is also latent: the sets fill as soon as any decision consumer
 * arms (the LV-6 census arming the CS advance does that), which changes the solve
 * through a knob that was not touched.  The loader therefore does not count
 * @c trace_slip_raw as a sufficient consumer.  See
 * @c docs/SPEC_level_authority_internal.md §5.3.
 *
 * Assertions are on post-load state, not on log text: @c loadFromJson takes a
 * @c LogManager& and obtains its own logger internally, and a default-constructed
 * @c LogManager returns a fresh console-only logger per call, so there is no registry
 * entry a test could attach a sink to.  The sibling auto-arm test
 * (@c tests/network/test_block_baarda.cpp, @c BlockBaardaSweepTest) makes the same choice.
 *
 * The gap instrument (@c gap_instrument) is instrumentation only (counters and trace rows, verified
 * byte-identical), which is why it, and not the @c mw_lli_corroborate decision consumer,
 * is the one that gets auto-armed.
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
    const fs::path p =
        fs::temp_directory_path() / ("d5_autoarm_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal but loadable PPP-RTK network config; @p network_extra splices into the
/// network block.
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

/// The D5 gate is defined only on the carried DDs, so every arm here needs the
/// invariant carry and its eq34 chain datum or the load fails loud
/// (@c CodeBiasNetworkConfig.CarryInvariantInnovGateRequiresCarryInvariant).
const char* kCarryBase =
    "    \"phase_datum_eq34\": true,\n"
    "    \"carry_invariant\": true,\n"
    "    \"carry_invariant_innov_gate\": true,\n";

bool loadInto(const std::string& tag, const std::string& network_extra, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, networkConfig(network_extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

// ============================================================================
//  Direction 1: corroborate (+ trace_slip_raw) with no decision consumer armed: the auto-arm fires.
// ============================================================================

// Regression case: trace_slip_raw is not a sufficient evidence source.  If the loader
// skipped the auto-arm here, decisionInstrumentArmed() would stay false, the qualify pass would not
// run and the suppression set would be structurally empty.
TEST(D5CorroborationAutoArm, TraceSlipRawAloneStillAutoArmsTheEvidenceInstrument) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3_trace_only",
        std::string(kCarryBase) +
        "    \"carry_invariant_innov_corroborate\": true,\n"
        "    \"trace_slip_raw\": true,\n", cfg));

    EXPECT_TRUE(cfg.networkCarryInvariantInnovCorroborate())
        << "the gate must survive load — it is the population under test";
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.gap_instrument)
        << "trace_slip_raw arms only the COMPUTE pass (anyConsumer), never the QUALIFY "
           "pass (decisionInstrumentArmed) that fills epoch.verdict — so the corroboration gate must "
           "still auto-arm a DECISION instrument or it is silently inert";
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.lli_corroborate)
        << "a DECISION instrument that changes the measured population must never be auto-armed";
}

// The v4 twin: re-tap-only reads the same verdicts through the same qualify pass.
TEST(D5CorroborationAutoArm, RetapOnlyWithTraceSlipRawAlsoAutoArms) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v4_trace_only",
        std::string(kCarryBase) +
        "    \"carry_invariant_innov_retap_only\": true,\n"
        "    \"trace_slip_raw\": true,\n", cfg));

    EXPECT_TRUE(cfg.networkCarryInvariantInnovRetapOnly());
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.gap_instrument)
        << "v4 tags flags flat from the same verdict maps v3 does";
}

// With neither the trace nor any decision consumer, the auto-arm also fires.  This baseline case
// guards against a future edit regressing it.
TEST(D5CorroborationAutoArm, BareCorroborateAutoArmsTheEvidenceInstrument) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3_bare",
        std::string(kCarryBase) +
        "    \"carry_invariant_innov_corroborate\": true,\n", cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.gap_instrument);
}

// ============================================================================
//  Direction 2: a decision consumer is already armed, so there is nothing to auto-arm.
// ============================================================================

// Explicit gap_instrument on is the posture that every committed corroborate config
// resolves to (census of 242 configs), so the interlock does not fire on the committed
// corpus.
TEST(D5CorroborationAutoArm, ExplicitGapInstrumentIsLeftExactlyAsWritten) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3_gap",
        std::string(kCarryBase) +
        "    \"carry_invariant_innov_corroborate\": true,\n"
        "    \"cycle_slip\": { \"gap_instrument\": true },\n", cfg));

    EXPECT_TRUE(cfg.networkCarryInvariantInnovCorroborate());
    EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.gap_instrument);
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.lli_corroborate);
}

// The CS-GF authority (network.level_authority_csgf, the LV-6 census) arms iono_cs, the
// fourth decisionInstrumentArmed() term, so decisionInstrumentArmed() is true on its own, the qualify pass
// already runs, and gap_instrument is not force-armed on top.  This keeps a census A/B
// from acquiring a second variable.
TEST(D5CorroborationAutoArm, CsGfAuthorityAlreadySatisfiesTheEvidenceRequirement) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3_lv6",
        std::string(kCarryBase) +
        "    \"carry_invariant_innov_corroborate\": true,\n"
        "    \"level_authority_csgf\": true,\n", cfg));

    EXPECT_TRUE(cfg.networkLevelAuthorityCsgf());
    EXPECT_TRUE(cfg.networkCarryInvariantInnovCorroborate());
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.gap_instrument)
        << "level_authority_csgf arms SlipQcParams::iono_cs, so decisionInstrumentArmed() is already "
           "true and the qualify pass already runs — auto-arming the gap instrument on top would add "
           "counters the A/B partner does not have";
}

// ============================================================================
//  Non-interference: the interlock must not reach configs that never armed D5.
// ============================================================================

TEST(D5CorroborationAutoArm, UnarmedCarryInvariantNeverAutoArmsAnything) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("no_d5",
        "    \"phase_datum_eq34\": true,\n"
        "    \"carry_invariant\": true,\n", cfg));

    EXPECT_FALSE(cfg.networkCarryInvariantInnovCorroborate());
    EXPECT_FALSE(cfg.networkCarryInvariantInnovRetapOnly());
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.gap_instrument)
        << "the evidence interlock is scoped to the two D5 knobs that consume verdicts";
}

// The knobs are force-disabled without the innovation gate (there are no re-taps to
// corroborate), so the interlock must find nothing to do and leave the gap instrument alone.
TEST(D5CorroborationAutoArm, CorroborateWithoutTheGateIsDisarmedAndArmsNoInstrument) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("v3_nogate",
        "    \"phase_datum_eq34\": true,\n"
        "    \"carry_invariant\": true,\n"
        "    \"carry_invariant_innov_corroborate\": true,\n"
        "    \"trace_slip_raw\": true,\n", cfg));

    EXPECT_FALSE(cfg.networkCarryInvariantInnovCorroborate())
        << "corroborate without carry_invariant_innov_gate is force-disabled at load";
    EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.gap_instrument)
        << "a disarmed gate consumes no verdicts, so nothing needs auto-arming";
}
