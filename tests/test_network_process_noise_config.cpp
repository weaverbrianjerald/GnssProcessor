/**
 * @file test_network_process_noise_config.cpp
 * @brief Config-contract tests for the scoped @c network.process_noise table.
 *
 * The table is one authoring surface over the eleven time-linked network-owned
 * process-noise rates whose legacy keys are spread across five config sections.
 * It is always live (there is no master enable), so its safety rests on an
 * equality rather than a default-off toggle: every class's fallback equals the
 * legacy key's own default.  These tests lock that equality
 * (@ref NetworkProcessNoiseConfig.StockConfigResolvesToLegacyDefaults is the
 * byte-identity guard), the precedence rule, and the loader's rejection of three
 * silent authoring errors: an unknown class, a negative rate, and a quoted number.
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

// Load @p body, expecting the loader to reject it.
bool loadFails(const std::string& tag, const std::string& body) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return !ok;
}

} // namespace

// Byte-identity guard.  A config with no network.process_noise block resolves every
// class to its descriptor fallback, and every fallback equals the legacy key's own
// default, so the always-live table reproduces the legacy rates exactly.  Editing a
// fallback without the legacy default beside it (or the reverse) fails this test,
// which catches the change in the unit suite instead of in a graded run.
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
}

// A table entry reaches the canonical member the estimator reads.  Covers both
// wiring routes: classes whose legacy key lives in an earlier section (write-through)
// and classes parsed later in the network section (jval default).
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

// Precedence: an explicitly set legacy key beats the table, so every config that
// sets the legacy keys (the archived campaign set and the golden-0729 posture
// configs among them) resolves to the same rates as before the table existed.
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

// Ordering constraint: network.ura_staleness_q2 defaults to clock_model_q2 and is
// captured after it, so a table-supplied sat_clock_rwfm propagates into the derived
// consumer knob.  For this reason the table resolves at the top of the network
// section rather than at the end.
TEST(NetworkProcessNoiseConfig, DerivedUraStalenessInheritsTableClockQ2) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadOk("derived", networkConfig(
        "    \"process_noise\": { \"sat_clock_rwfm\": 2.5e-7 },\n"), cfg, log));

    EXPECT_DOUBLE_EQ(cfg.networkClockModelQ2(),   2.5e-7);
    EXPECT_DOUBLE_EQ(cfg.networkUraStalenessQ2(), 2.5e-7);  // inherited, not 1.5e-7
}

// A misspelled class is a hard load error.  Ignoring it would produce a run that
// looks armed and is inert; the load error catches that at config load instead of
// through run-end telemetry.
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

// The descriptor table is the single source of truth for the loader, the summary
// line and the manifest writer; a duplicate or empty name would make one
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
