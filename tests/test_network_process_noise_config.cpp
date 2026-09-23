/**
 * @file test_network_process_noise_config.cpp
 * @brief Config-contract tests for the scoped @c network.process_noise table.
 *
 * The table is a single authoring surface over the twelve time-linked
 * network-owned process-noise rates that were previously spelled across five
 * config sections.  It is ALWAYS live — there is no master enable — so the
 * property that makes it safe is not a default-OFF toggle but an equality:
 * every class's fallback equals the legacy key's own default.  These tests lock
 * that equality (@ref NetworkProcessNoiseConfig.StockConfigResolvesToLegacyDefaults
 * is the byte-identity guard), the precedence rule, and the loader's rejection
 * of the three ways a q table gets silently mis-authored — an unknown class, a
 * negative rate, and a quoted number.
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

using Src = NetworkProcessNoise::Source;

// Write @p body to a unique temp file and return its path.  @p tag disambiguates
// concurrent test files (Date/random helpers are unavailable in this harness).
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("netpn_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal but loadable PPP-RTK network config.  @p extra is spliced into the
// network block; @p top_level is spliced in beside "processing"/"network" so a
// test can also set the legacy keys that live in other sections.
std::string networkConfig(const std::string& extra = "",
                          const std::string& top_level = "") {
    return std::string("{\n") + top_level +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

// Load @p body, expecting success, and hand back the resolved table.
::testing::AssertionResult loadOk(const std::string& tag, const std::string& body,
                                  GnssConfig& cfg, LogManager& log) {
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok ? ::testing::AssertionSuccess()
              : ::testing::AssertionFailure() << "loadFromJson failed for tag " << tag;
}

// Load @p body expecting the loader to REJECT it.
bool loadFails(const std::string& tag, const std::string& body) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return !ok;
}

} // namespace

// THE byte-identity guard.  A config with no network.process_noise block must
// resolve every class to its descriptor fallback, and every fallback must be
// the legacy key's own default — which is what makes an always-live table
// produce today's numbers exactly.  If someone edits a fallback without editing
// the legacy default beside it (or vice versa), this fails and the silent field
// result change is caught at build time instead of in a graded run.
TEST(NetworkProcessNoiseConfig, StockConfigResolvesToLegacyDefaults) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("stock", networkConfig(), cfg, log));
    const NetworkProcessNoise& pn = cfg.getNetworkConfig().process_noise;

    for (std::size_t i = 0; i < kNetworkQClassCount; ++i) {
        const NetworkQDesc& d = kNetworkQClassTable[i];
        const auto c = static_cast<NetworkQClass>(i);
        EXPECT_DOUBLE_EQ(pn.get(c), d.fallback) << "class " << d.name;
        EXPECT_EQ(pn.source(c), Src::Default)   << "class " << d.name;
    }

    // Cross-check the fallbacks against the canonical members the estimator
    // actually reads, so the table cannot drift away from the consumption path.
    EXPECT_DOUBLE_EQ(cfg.getProcessNoise().sat_clock,        1e-2);
    EXPECT_DOUBLE_EQ(cfg.getProcessNoise().sat_phase_bias,   1e-12);
    EXPECT_DOUBLE_EQ(cfg.getProcessNoise().sat_code_bias,    1e-10);
    EXPECT_DOUBLE_EQ(cfg.getGlobalVtecShConfig().q_per_degree,        1.0e-2);
    EXPECT_DOUBLE_EQ(cfg.getGlobalVtecShConfig().layer2_q_per_degree, 1.0e-2);
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(),          1e-9);
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingQAmb(),  0.0);
    EXPECT_DOUBLE_EQ(cfg.networkPhaseDatumEq34RpbQ(), 3e-6);
    EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantQz(),   0.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockModelQ1(),       0.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockModelQ2(),       1.5e-7);
    EXPECT_DOUBLE_EQ(cfg.networkIonoCarryQ(),         1e-4);
}

// A table entry must reach the canonical member the estimator reads.  Covers
// both wiring routes: classes whose legacy key lives in an EARLIER section
// (write-through) and classes parsed LATER in the network section (jval default).
TEST(NetworkProcessNoiseConfig, TableEntryReachesCanonicalMember) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("table", networkConfig(
        "    \"process_noise\": {\n"
        "      \"sat_clock\": 5e-3,\n"          // earlier section -> write-through
        "      \"vtec_sh\": 2.5e-2,\n"          // earlier section -> write-through
        "      \"station_zwd\": 4e-9,\n"        // later in network -> jval default
        "      \"dd_carry\": 1e-4\n"            // later in network -> jval default
        "    },\n"), cfg, log));
    const NetworkProcessNoise& pn = cfg.getNetworkConfig().process_noise;

    EXPECT_DOUBLE_EQ(cfg.getProcessNoise().sat_clock, 5e-3);
    EXPECT_DOUBLE_EQ(cfg.getGlobalVtecShConfig().q_per_degree, 2.5e-2);
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 4e-9);
    EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantQz(), 1e-4);

    EXPECT_EQ(pn.source(NetworkQClass::SatClock),   Src::Table);
    EXPECT_EQ(pn.source(NetworkQClass::VtecSh),     Src::Table);
    EXPECT_EQ(pn.source(NetworkQClass::StationZwd), Src::Table);
    EXPECT_EQ(pn.source(NetworkQClass::DdCarry),    Src::Table);
    // Untouched classes stay at their defaults.
    EXPECT_DOUBLE_EQ(pn.get(NetworkQClass::SatPhaseBias), 1e-12);
    EXPECT_EQ(pn.source(NetworkQClass::SatPhaseBias), Src::Default);
}

// Precedence: an explicitly-set legacy key beats the table.  This is what keeps
// every pre-existing config — the archived campaign set and the golden-0729
// posture configs among them — behaving exactly as before.
TEST(NetworkProcessNoiseConfig, ExplicitLegacyKeyBeatsTable) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("prec", networkConfig(
        "    \"zwd_carry_q\": 7e-9,\n"                       // legacy, network section
        "    \"process_noise\": { \"station_zwd\": 4e-9, \"sat_clock\": 5e-3 },\n",
        "  \"filter\": { \"process_noise\": { \"sat_clock_q\": 3e-2 } },\n"), // legacy, filter section
        cfg, log));
    const NetworkProcessNoise& pn = cfg.getNetworkConfig().process_noise;

    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 7e-9);            // legacy won
    EXPECT_DOUBLE_EQ(cfg.getProcessNoise().sat_clock, 3e-2);   // legacy won
    EXPECT_EQ(pn.source(NetworkQClass::StationZwd), Src::Legacy);
    EXPECT_EQ(pn.source(NetworkQClass::SatClock),   Src::Legacy);
}

// Back-compat: a config that only uses the legacy keys is unaffected by the
// table's existence, and the resolved table reports the legacy values.
TEST(NetworkProcessNoiseConfig, LegacyOnlyConfigStillHonoured) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("legacy", networkConfig(
        "    \"zwd_carry_q\": 7e-9,\n"
        "    \"carry_invariant_qz\": 1e-4,\n"
        "    \"phase_leveling_q_amb\": 2e-11,\n"), cfg, log));
    const NetworkProcessNoise& pn = cfg.getNetworkConfig().process_noise;

    EXPECT_DOUBLE_EQ(pn.get(NetworkQClass::StationZwd),        7e-9);
    EXPECT_DOUBLE_EQ(pn.get(NetworkQClass::DdCarry),           1e-4);
    EXPECT_DOUBLE_EQ(pn.get(NetworkQClass::LevelingAmbiguity), 2e-11);
    EXPECT_EQ(pn.source(NetworkQClass::StationZwd),        Src::Legacy);
    EXPECT_EQ(pn.source(NetworkQClass::DdCarry),           Src::Legacy);
    EXPECT_EQ(pn.source(NetworkQClass::LevelingAmbiguity), Src::Legacy);
}

// The slant-iono carry class (the twelfth) must report the rate the estimator
// reads, through every precedence rung.  finalizeNetworkProcessNoise_ once
// re-read only the first eleven classes, so an explicit network.iono_carry_q
// reached networkIonoCarryQ() while the table, and with it the run manifest
// and the process-noise log line, kept the fallback labelled Default (or the
// shadowed table value labelled Table when both keys were set).
TEST(NetworkProcessNoiseConfig, IonoCarryReportsResolvedRate) {
    struct Case { const char* tag; const char* extra; double q; Src src; };
    const Case cases[] = {
        { "iono_default", "", 1e-4, Src::Default },
        { "iono_legacy",  "    \"iono_carry_q\": 3e-4,\n", 3e-4, Src::Legacy },
        { "iono_table",   "    \"process_noise\": { \"iono_carry\": 5e-4 },\n", 5e-4, Src::Table },
        { "iono_both",    "    \"process_noise\": { \"iono_carry\": 5e-4 },\n"
                          "    \"iono_carry_q\": 2e-4,\n", 2e-4, Src::Legacy },
    };
    for (const Case& k : cases) {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadOk(k.tag, networkConfig(k.extra), cfg, log));
        const NetworkProcessNoise& pn = cfg.getNetworkConfig().process_noise;

        EXPECT_DOUBLE_EQ(cfg.networkIonoCarryQ(), k.q)          << k.tag;  // what the solve reads
        EXPECT_DOUBLE_EQ(pn.get(NetworkQClass::IonoCarry), k.q) << k.tag;  // what the manifest reports
        EXPECT_EQ(pn.source(NetworkQClass::IonoCarry), k.src)   << k.tag;
    }
}

// Ordering trap: network.ura_staleness_q2 DEFAULTS to clock_model_q2 and is
// captured after it, so a table-supplied sat_clock_rwfm must propagate into the
// derived consumer knob.  This is why the table resolves at the TOP of the
// network section rather than at the end.
TEST(NetworkProcessNoiseConfig, DerivedUraStalenessInheritsTableClockQ2) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("derived", networkConfig(
        "    \"process_noise\": { \"sat_clock_rwfm\": 2.5e-7 },\n"), cfg, log));

    EXPECT_DOUBLE_EQ(cfg.networkClockModelQ2(),   2.5e-7);
    EXPECT_DOUBLE_EQ(cfg.networkUraStalenessQ2(), 2.5e-7);  // inherited, not 1.5e-7
}

// A misspelled class must be a hard load error.  Silently ignoring it would
// produce a run that looks armed and is inert — the failure mode run-end
// telemetry exists to catch, caught here instead at config load.
TEST(NetworkProcessNoiseConfig, UnknownClassIsRejected) {
    EXPECT_TRUE(loadFails("unknown", networkConfig(
        "    \"process_noise\": { \"sat_clk\": 5e-3 },\n")));
}

// A negative rate is meaningless (it would shrink a covariance).  The legacy
// zwd_carry_q / phase_leveling_q_amb keys already hard-error on this; the table
// applies the same rule to every class.
TEST(NetworkProcessNoiseConfig, NegativeRateIsRejected) {
    EXPECT_TRUE(loadFails("negative", networkConfig(
        "    \"process_noise\": { \"sat_clock\": -1e-3 },\n")));
}

// A quoted number would otherwise throw out of loadFromJson from deep inside
// nlohmann with no config-path context; the loader names the offending class.
TEST(NetworkProcessNoiseConfig, QuotedNumberIsRejected) {
    EXPECT_TRUE(loadFails("quoted", networkConfig(
        "    \"process_noise\": { \"sat_clock\": \"1e-2\" },\n")));
}

// The layer-2 SH q is required strictly positive by the dual-layer code; the
// estimation section restores a non-positive value before the network section
// runs, so the table's write-through must re-assert the guard rather than
// slipping a zero past it.
TEST(NetworkProcessNoiseConfig, ZeroLayer2VtecQIsRestored) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("layer2", networkConfig(
        "    \"process_noise\": { \"vtec_sh_layer2\": 0.0 },\n"), cfg, log));

    EXPECT_DOUBLE_EQ(cfg.getGlobalVtecShConfig().layer2_q_per_degree, 1.0e-2);
    EXPECT_GT(cfg.getGlobalVtecShConfig().layer2_q_per_degree, 0.0);
}

// The descriptor table is the single source of truth for the loader, the ON
// summary line and the manifest writer; a duplicate or empty name would make one
// of those three silently address the wrong class.
TEST(NetworkProcessNoiseConfig, DescriptorTableIsWellFormed) {
    for (std::size_t i = 0; i < kNetworkQClassCount; ++i) {
        const NetworkQDesc& di = kNetworkQClassTable[i];
        ASSERT_NE(di.name, nullptr);
        EXPECT_NE(std::string(di.name), "")  << "class " << i << " has an empty name";
        EXPECT_NE(std::string(di.units), "") << "class " << di.name << " has no units";
        EXPECT_GE(di.fallback, 0.0)          << "class " << di.name << " has a negative fallback";
        for (std::size_t j = i + 1; j < kNetworkQClassCount; ++j)
            EXPECT_NE(std::string(di.name), std::string(kNetworkQClassTable[j].name))
                << "duplicate class name at " << i << " and " << j;
    }
}
