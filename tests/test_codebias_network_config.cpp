/**
 * @file test_codebias_network_config.cpp
 * @brief Config-contract tests for satellite CODE-bias (OSB) estimation in the
 *        PPP-RTK network producer (W6).
 *
 * network.estimate_sat_code_bias is the single switch that lands the coupled W6
 * slice: the network estimates its OWN per-(satellite,signal) code biases and
 * emits them so the user can skip the external .BIA.  Because the satellite DCB is
 * unobservable without the receiver DCB, the GIM ionosphere constraint, and the
 * satellite code-bias state all present together, the flag MUST force them on (and
 * force .BIA off so the network bias is the sole code-bias source).  These tests
 * lock that contract, and that it stays OFF by default (no regression to the
 * clock+phase-bias-only V1 network).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("codebias_net_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal but loadable PPP-RTK network config.  @p active is the active_systems
// JSON array literal, @p priorities the signal_priorities object, and
// @p network_extra is spliced into the network block.
std::string networkConfig(const std::string& active,
                          const std::string& priorities,
                          const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": " + active + ",\n"
        "    \"signal_priorities\": " + priorities + "\n"
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

constexpr const char* kGpsGal     = "[ \"GPS\", \"Galileo\" ]";
constexpr const char* kGpsGalGlo  = "[ \"GPS\", \"Galileo\", \"GLONASS\" ]";
constexpr const char* kPriosGps   = "{ \"GPS\": \"WC\", \"Galileo\": \"CB\" }";
constexpr const char* kPriosGlo   = "{ \"GPS\": \"WC\", \"Galileo\": \"CB\", \"GLONASS\": \"CP\" }";

} // namespace

// estimate_sat_code_bias=true forces the whole coupled slice on (and .BIA off).
TEST(CodeBiasNetworkConfig, EstimateSatCodeBiasForcesPrerequisites) {
    LogManager log;
    const fs::path p = writeTempConfig("force",
        networkConfig(kGpsGal, kPriosGps, "    \"estimate_sat_code_bias\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());
    EXPECT_TRUE(cfg.estimateSatBiases());                          // promoted to a state
    EXPECT_TRUE(cfg.promoteSatCodeBias());                         // estimated on every code row
    EXPECT_TRUE(cfg.estimateIfb());                               // receiver DCB on the undiff rows
    EXPECT_TRUE(cfg.getIonosphereWeightingConfig().enabled);      // GIM constraint (load-bearing)
    EXPECT_FALSE(cfg.getModelCorrections().enable_code_phase_bias); // .BIA skipped (no double-correct)
    EXPECT_FALSE(cfg.estimatePhaseClock());                       // hard-off for V1
    EXPECT_TRUE(cfg.isUncombinedEnabled());                       // network family forces uncombined
    std::error_code ec; fs::remove(p, ec);
}

// The derived closed-loop consumer config CONSUMES the OSB (never estimates it):
// the producer-only switch is cleared and .BIA stays off (inherited), so the
// network code bias applies via ssr_bias_term as the sole code-bias source.
TEST(CodeBiasNetworkConfig, DerivedConsumerConsumesNotEstimates) {
    LogManager log;
    const fs::path p = writeTempConfig("consumer",
        networkConfig(kGpsGal, kPriosGps, "    \"estimate_sat_code_bias\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(consumer.estimateNetworkSatCodeBias());           // producer-only switch cleared
    EXPECT_FALSE(consumer.promoteSatCodeBias());
    EXPECT_FALSE(consumer.getModelCorrections().enable_code_phase_bias);  // .BIA still off (inherited)
    std::error_code ec; fs::remove(p, ec);
}

// Default network config (no estimate_sat_code_bias): code bias is DEFERRED and
// .BIA stays on — the clock+phase-bias-only V1 network is byte-identical.
TEST(CodeBiasNetworkConfig, DefaultNetworkHasCodeBiasOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default",
        networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.estimateNetworkSatCodeBias());
    EXPECT_FALSE(cfg.estimateSatBiases());                         // V1 defers code bias
    EXPECT_FALSE(cfg.promoteSatCodeBias());
    EXPECT_FALSE(cfg.getIonosphereWeightingConfig().enabled);
    EXPECT_TRUE(cfg.getModelCorrections().enable_code_phase_bias); // .BIA on (default)
    std::error_code ec; fs::remove(p, ec);
}

// bias_coverage_requires_apply: parses (default false), is NOT clobbered by the
// estimate_sat_code_bias coupling that forces enable_code_phase_bias off, and is
// inherited by the derived closed-loop consumer.  This is the exact defect chain
// the toggle exists for: the coupling silently disables .BIA APPLICATION while
// the loaded .BIA still reports phantom COVERAGE to the "already corrected?"
// gates (sat-bias absorber allocation + no-BIA code downweight) — the toggle
// makes those gates consult applied coverage instead.
TEST(CodeBiasNetworkConfig, BiasCoverageRequiresApplyParsesAndSurvivesCoupling) {
    LogManager log;
    // Default: key absent -> false (legacy presence-only semantics, byte-identical).
    {
        const fs::path p = writeTempConfig("cov_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.getModelCorrections().bias_coverage_requires_apply);
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed alongside the coupled code-bias slice: the coupling forces .BIA
    // application OFF, and the toggle survives it unchanged — together the
    // defect configuration (loaded .BIA, unapplied) now reads as uncovered.
    {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": " + kGpsGal + ",\n"
            "    \"signal_priorities\": " + kPriosGps + "\n"
            "  },\n"
            "  \"model_corrections\": { \"bias_coverage_requires_apply\": true },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            "    \"estimate_sat_code_bias\": true,\n"
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        const fs::path p = writeTempConfig("cov_armed", body);
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.getModelCorrections().enable_code_phase_bias);       // coupling forced .BIA off
        EXPECT_TRUE(cfg.getModelCorrections().bias_coverage_requires_apply);  // toggle survives the coupling
        const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
        EXPECT_TRUE(consumer.getModelCorrections().bias_coverage_requires_apply);  // inherited by the consumer
        std::error_code ec; fs::remove(p, ec);
    }
}

// clock_signal_basis_hold: parses (default false, so the per-epoch IF-pin
// reference selection is byte-identical), and — armed alongside the W6 slice it
// modifies — survives the estimate_sat_code_bias coupling (the toggle holds the
// per-satellite clock↔code-bias IF-pin reference pair fixed in time; it is only
// meaningful when the network estimates its own code biases, so it MUST coexist
// with that flag rather than being clobbered by the coupling).
TEST(CodeBiasNetworkConfig, ClockSignalBasisHoldParsesAndSurvivesCoupling) {
    LogManager log;
    // Default: key absent -> false (per-epoch reference re-selection, byte-identical).
    {
        const fs::path p = writeTempConfig("basis_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkClockSignalBasisHold());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed with the W6 code-bias slice: the coupling forces the prerequisites on,
    // and the hold toggle survives it — both are true, so the emitted (δtˢ, dˢ)
    // split is estimated (W6) AND held to a fixed signal basis in time.
    {
        const fs::path p = writeTempConfig("basis_armed",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"estimate_sat_code_bias\": true,\n"
                          "    \"clock_signal_basis_hold\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());       // coupled slice on
        EXPECT_TRUE(cfg.networkClockSignalBasisHold());      // hold toggle survives the coupling
        std::error_code ec; fs::remove(p, ec);
    }
}

// network.code_datum_time_linked (Card 2): parses (default false = the legacy zero-
// target code-bias datum, byte-identical) and survives the estimate_sat_code_bias
// coupling (the datum it re-times only exists in W6, so it must coexist with that flag
// rather than be clobbered).
TEST(CodeBiasNetworkConfig, CodeDatumTimeLinkedParsesAndSurvivesCoupling) {
    LogManager log;
    // Default: key absent -> false (per-epoch zero-target datum, byte-identical).
    {
        const fs::path p = writeTempConfig("cbtl_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCodeDatumTimeLinked());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed with the W6 code-bias slice: the coupling forces the prerequisites on, and
    // the time-linked toggle survives it — the datum is now imposed as a time datum.
    {
        const fs::path p = writeTempConfig("cbtl_armed",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"estimate_sat_code_bias\": true,\n"
                          "    \"code_datum_time_linked\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());       // coupled slice on
        EXPECT_TRUE(cfg.networkCodeDatumTimeLinked());       // time-linked toggle survives
        std::error_code ec; fs::remove(p, ec);
    }
}

// code_datum_time_linked and clock_signal_basis_hold BOTH re-target the per-satellite
// IF-pin row (the clock↔code split): the time-linked datum subsumes the hold for the
// pin value, so arming both is a hard load error rather than a silent last-wins.
TEST(CodeBiasNetworkConfig, CodeDatumTimeLinkedWithSignalBasisHoldIsError) {
    LogManager log;
    const fs::path p = writeTempConfig("cbtl_conflict",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"estimate_sat_code_bias\": true,\n"
                      "    \"code_datum_time_linked\": true,\n"
                      "    \"clock_signal_basis_hold\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// phase_bias_exclude_unmatched_antenna: parses (default false, so every station
// contributes to phase-bias emission — byte-identical), and arms independently of
// the W6 code-bias slice (it gates only which STATIONS feed the satellite phase
// biases, orthogonal to whether the network also estimates code biases).
TEST(CodeBiasNetworkConfig, PhaseBiasExcludeUnmatchedAntennaParses) {
    LogManager log;
    // Default: key absent -> false (every station contributes, byte-identical).
    {
        const fs::path p = writeTempConfig("pbexcl_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkPhaseBiasExcludeUnmatchedAntenna());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed on its own (no code-bias slice): the toggle is honoured — an unmatched-
    // receiver-antenna station will be withheld from satellite phase-bias emission.
    {
        const fs::path p = writeTempConfig("pbexcl_armed",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"phase_bias_exclude_unmatched_antenna\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkPhaseBiasExcludeUnmatchedAntenna());
        std::error_code ec; fs::remove(p, ec);
    }
}

// cold_clock_seed: parses (default false, so the network state pre-pass never
// pre-seeds the receiver clock and every fixture is byte-identical), and arms
// independently of the W6 code-bias slice (it only pre-writes x(clock) from the
// median GPS code residual before the first ambiguity seed, orthogonal to
// whether the network estimates code biases).  The defect it exists for: on the
// network path nothing solves the receiver clock before the first ambiguity
// seed, so a free-running receiver's whole clock offset leaks uniformly into
// every cold ambiguity seed.
TEST(CodeBiasNetworkConfig, ColdClockSeedParses) {
    LogManager log;
    // Default: key absent -> false (no pre-seed, byte-identical).
    {
        const fs::path p = writeTempConfig("coldclk_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkColdClockSeed());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed on its own (no code-bias slice): the toggle is honoured, and it does
    // NOT drag the W6 prerequisites on — it is orthogonal to code-bias estimation.
    {
        const fs::path p = writeTempConfig("coldclk_armed",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"cold_clock_seed\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkColdClockSeed());
        EXPECT_FALSE(cfg.estimateNetworkSatCodeBias());   // orthogonal — no coupling
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed alongside the W6 code-bias slice: both survive (the pre-seed helps the
    // cold ambiguity seeds that feed the very code-bias/iono slice W6 estimates).
    {
        const fs::path p = writeTempConfig("coldclk_w6",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"estimate_sat_code_bias\": true,\n"
                          "    \"cold_clock_seed\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());
        EXPECT_TRUE(cfg.networkColdClockSeed());
        std::error_code ec; fs::remove(p, ec);
    }
}

// estimate_sat_code_bias and estimate_glonass cannot both be on in V1: admitting
// GLONASS forces the receiver-bias allocation to skip the GPS/Galileo IFB that the
// satellite code-bias separation needs.  Fail loud at load.
TEST(CodeBiasNetworkConfig, CodeBiasWithGlonassIsError) {
    LogManager log;
    const fs::path p = writeTempConfig("glo",
        networkConfig(kGpsGalGlo, kPriosGlo,
                      "    \"estimate_sat_code_bias\": true,\n"
                      "    \"estimate_glonass\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// RTA-7: first-class network-consumer profile (processing.network_consumer)
// ===========================================================================

namespace {

// Same producer skeleton as networkConfig() with splice hooks for the
// processing block (RTA-7 profile keys), the network block, and additional
// top-level sections (e.g. "realtime").
std::string networkConfigRt(const std::string& processing_extra,
                            const std::string& network_extra,
                            const std::string& top_extra = "") {
    return std::string("{\n") + top_extra +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        + processing_extra +
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
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

} // namespace

// The standalone profile applies the SAME couplings as the closed-loop
// derivation (applyNetworkConsumerCouplings is the shared source of truth) —
// full mode (network clock consumed), undifferenced user model.
TEST(NetworkConsumerProfile, MatchesClosedLoopDerivationFullUndifferenced) {
    LogManager log;

    // Closed-loop reference: undifferenced held-out consumer, network clock on.
    const fs::path p1 = writeTempConfig("cl_undiff",
        networkConfigRt("", "    \"closed_loop\": { \"consumer_undifferenced\": true },\n"));
    GnssConfig producer;
    ASSERT_TRUE(producer.loadFromJson(p1, log));
    const GnssConfig derived = producer.deriveClosedLoopConsumerConfig();

    // Standalone profile: same intent through processing.network_consumer.
    const fs::path p2 = writeTempConfig("profile_undiff",
        networkConfigRt("    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig consumer;
    ASSERT_TRUE(consumer.loadFromJson(p2, log));
    ASSERT_TRUE(consumer.getNetworkConsumerConfig().enable);
    consumer.applyNetworkConsumerProfile(log);

    // Field-for-field agreement on every shared coupling.
    EXPECT_EQ(consumer.getProcessingMode(), ProcessingMode::PPP_RTK);
    EXPECT_EQ(consumer.getProcessingMode(),          derived.getProcessingMode());
    EXPECT_EQ(consumer.estimateSatClocks(),          derived.estimateSatClocks());
    EXPECT_EQ(consumer.estimateSatPhaseBiases(),     derived.estimateSatPhaseBiases());
    EXPECT_EQ(consumer.promoteSatCodeBias(),         derived.promoteSatCodeBias());
    EXPECT_EQ(consumer.estimateNetworkSatCodeBias(), derived.estimateNetworkSatCodeBias());
    EXPECT_EQ(consumer.estimateNetworkIonoSh(),      derived.estimateNetworkIonoSh());
    EXPECT_EQ(consumer.estimateSatBiases(),          derived.estimateSatBiases());
    EXPECT_FALSE(consumer.estimateSatBiases());      // network clock consumed -> no residual sat code bias
    EXPECT_DOUBLE_EQ(consumer.noBiaCodeDownweight(), derived.noBiaCodeDownweight());
    EXPECT_DOUBLE_EQ(consumer.noBiaCodeDownweight(), 100.0);
    EXPECT_EQ(consumer.enableBetweenSatelliteSd(),   derived.enableBetweenSatelliteSd());
    EXPECT_FALSE(consumer.enableBetweenSatelliteSd());  // undifferenced: no BSSD machinery
    EXPECT_EQ(consumer.getArWlOnly(),                derived.getArWlOnly());
    EXPECT_FALSE(consumer.getArWlOnly());               // direct per-band LAMBDA
    EXPECT_LE(consumer.getArMaxPositionSigma(), 0.20);  // capped AR sigma gate
    EXPECT_DOUBLE_EQ(consumer.getArMaxPositionSigma(), derived.getArMaxPositionSigma());
    // The closed-loop driver calls setArIgnoreMwWl(!derived.getArWlOnly()); the
    // profile's resolved value must be exactly that.
    EXPECT_EQ(consumer.networkConsumerArIgnoreMwWl(), !derived.getArWlOnly());
    EXPECT_TRUE(consumer.networkConsumerArIgnoreMwWl());

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

// BIAS-ONLY mode (precise clock kept, network bias consumed): the
// estimate_sat_biases coupling flips exactly as the closed-loop derivation's
// consume_network_clock=false branch does.
TEST(NetworkConsumerProfile, BiasOnlyClockKeepsResidualSatBiasEstimation) {
    LogManager log;
    const fs::path p1 = writeTempConfig("cl_biasonly",
        networkConfigRt("", "    \"closed_loop\": { \"consumer_undifferenced\": true, "
                            "\"consume_network_clock\": false },\n"));
    GnssConfig producer;
    ASSERT_TRUE(producer.loadFromJson(p1, log));
    const GnssConfig derived = producer.deriveClosedLoopConsumerConfig();
    EXPECT_TRUE(derived.estimateSatBiases());   // precise clock -> estimate residual sat code bias

    const fs::path p2 = writeTempConfig("profile_biasonly",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, \"clock\": false },\n", ""));
    GnssConfig consumer;
    ASSERT_TRUE(consumer.loadFromJson(p2, log));
    consumer.applyNetworkConsumerProfile(log);
    EXPECT_EQ(consumer.estimateSatBiases(), derived.estimateSatBiases());
    EXPECT_TRUE(consumer.estimateSatBiases());

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

// ar_ignore_mw_wl resolution: an explicit profile key overrides the derivation;
// the standalone processing.ar_ignore_mw_wl key (the previously key-less
// RealTimeEngine::setArIgnoreMwWl seam) works without the profile; everything
// defaults off.
TEST(NetworkConsumerProfile, ArIgnoreMwWlOverrideStandaloneKeyAndDefaults) {
    LogManager log;

    // Explicit profile override beats the undifferenced derivation (true).
    const fs::path p1 = writeTempConfig("ar_override",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, "
                        "\"ar_ignore_mw_wl\": false },\n", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_EQ(c1.getNetworkConsumerConfig().ar_ignore_mw_wl, 0);   // explicit false
    c1.applyNetworkConsumerProfile(log);
    EXPECT_FALSE(c1.networkConsumerArIgnoreMwWl());

    // Standalone key without the profile.
    const fs::path p2 = writeTempConfig("ar_standalone",
        networkConfigRt("    \"ar_ignore_mw_wl\": true,\n", ""));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_TRUE(c2.arIgnoreMwWl());
    EXPECT_TRUE(c2.networkConsumerArIgnoreMwWl());   // profile off -> the standalone key

    // Defaults: profile off, tri-state unset, keys false/empty — the OFF path.
    const fs::path p3 = writeTempConfig("consumer_defaults", networkConfigRt("", ""));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_FALSE(c3.getNetworkConsumerConfig().enable);
    EXPECT_EQ(c3.getNetworkConsumerConfig().ar_ignore_mw_wl, -1);
    EXPECT_TRUE(c3.getNetworkConsumerConfig().clock);
    EXPECT_TRUE(c3.getNetworkConsumerConfig().bias);
    EXPECT_TRUE(c3.getNetworkConsumerConfig().undifferenced);
    EXPECT_FALSE(c3.arIgnoreMwWl());
    EXPECT_FALSE(c3.networkConsumerArIgnoreMwWl());
    EXPECT_FALSE(c3.getNetworkConfig().product_flush_per_epoch);
    EXPECT_TRUE(c3.getRealTimeConfig().network_products_path.empty());

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// The profile arms estimation.estimate_ifb when consuming network code biases —
// a standalone consumer has no producer config to inherit it from, and without
// receiver code-bias states a code-bias datum re-reference has nothing to
// absorb it (renders the engine's one-shot A5 warning moot on this path).
TEST(NetworkConsumerProfile, ArmsEstimateIfbForBiasConsumption) {
    LogManager log;
    const fs::path p = writeTempConfig("profile_ifb",
        networkConfigRt("    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.estimateIfb());   // base config leaves it off
    cfg.applyNetworkConsumerProfile(log);
    EXPECT_TRUE(cfg.estimateIfb());    // armed by the profile (bias=true default)
    std::error_code ec; fs::remove(p, ec);
}

// RTA-6 key parsing: network.product_flush_per_epoch and
// realtime.network_products_path land in their config structs.
TEST(NetworkConsumerProfile, FlushToggleAndProductsPathParse) {
    LogManager log;
    const fs::path p = writeTempConfig("rt_tail_keys",
        networkConfigRt("",
            "    \"product_output_path\": \"logs/np.txt\",\n"
            "    \"product_flush_per_epoch\": true,\n",
            "  \"realtime\": { \"network_products_path\": \"logs/np.txt\" },\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.getNetworkConfig().product_flush_per_epoch);
    EXPECT_EQ(cfg.getRealTimeConfig().network_products_path, "logs/np.txt");
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// RTA-8: orbit_source profile field + per-stream corrections_accept ingest key
// ===========================================================================

// processing.network_consumer.orbit_source: defaults to "external", accepts
// "network", and an unknown value warns + falls back to "external".
TEST(NetworkConsumerProfile, OrbitSourceParsesAndValidates) {
    LogManager log;

    const fs::path p1 = writeTempConfig("orbit_src_default",
        networkConfigRt("    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_EQ(c1.getNetworkConsumerConfig().orbit_source, "external");

    const fs::path p2 = writeTempConfig("orbit_src_network",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, "
                        "\"orbit_source\": \"network\" },\n", ""));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_EQ(c2.getNetworkConsumerConfig().orbit_source, "network");

    const fs::path p3 = writeTempConfig("orbit_src_bogus",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, "
                        "\"orbit_source\": \"sideways\" },\n", ""));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_EQ(c3.getNetworkConsumerConfig().orbit_source, "external");

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// Card 7 (RTF-2): processing.network_consumer.orbit_clock_max_skew_s — defaults
// 0 (echo skew check + withhold off), parses a positive budget, and a negative
// value clamps back to 0 with a warning.
TEST(NetworkConsumerProfile, OrbitClockMaxSkewParsesAndClamps) {
    LogManager log;

    const fs::path p1 = writeTempConfig("skew_default",
        networkConfigRt("    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_DOUBLE_EQ(c1.getNetworkConsumerConfig().orbit_clock_max_skew_s, 0.0);

    const fs::path p2 = writeTempConfig("skew_armed",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, "
                        "\"orbit_clock_max_skew_s\": 30.0 },\n", ""));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.getNetworkConsumerConfig().orbit_clock_max_skew_s, 30.0);

    const fs::path p3 = writeTempConfig("skew_negative",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, "
                        "\"orbit_clock_max_skew_s\": -5.0 },\n", ""));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_DOUBLE_EQ(c3.getNetworkConsumerConfig().orbit_clock_max_skew_s, 0.0);

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// realtime.{corrections,ephemeris}.corrections_accept: defaults "all";
// "orbit_only" parses (case-insensitive); unknown warns + falls back to "all".
TEST(NetworkConsumerProfile, CorrectionsAcceptParsesPerStream) {
    LogManager log;

    const fs::path p1 = writeTempConfig("accept_default",
        networkConfigRt("", "",
            "  \"realtime\": { \"corrections\": { \"enabled\": true } },\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_EQ(c1.getRealTimeConfig().corrections.accept, "all");
    EXPECT_EQ(c1.getRealTimeConfig().ephemeris.accept, "all");   // untouched default

    const fs::path p2 = writeTempConfig("accept_orbit_only",
        networkConfigRt("", "",
            "  \"realtime\": {\n"
            "    \"corrections\": { \"enabled\": true, \"corrections_accept\": \"ORBIT_ONLY\" },\n"
            "    \"ephemeris\":   { \"enabled\": true, \"corrections_accept\": \"orbit_only\" }\n"
            "  },\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_EQ(c2.getRealTimeConfig().corrections.accept, "orbit_only");
    EXPECT_EQ(c2.getRealTimeConfig().ephemeris.accept, "orbit_only");

    const fs::path p3 = writeTempConfig("accept_bogus",
        networkConfigRt("", "",
            "  \"realtime\": { \"corrections\": { \"enabled\": true, "
            "\"corrections_accept\": \"clocks_please\" } },\n"));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_EQ(c3.getRealTimeConfig().corrections.accept, "all");

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// ===========================================================================
// RTA-9: bias-gate arming under consumption + clock stale-hold / fallback keys
// ===========================================================================

// RTA-9(a): the profile arms processing.enable_ssr_bias_max_age when bias
// consumption is on and the user wrote NO key; an explicit false is respected;
// an explicit true is simply kept.
TEST(NetworkConsumerProfile, ArmsBiasMaxAgeUnderConsumption) {
    LogManager log;

    // No key written -> armed by the profile.
    const fs::path p1 = writeTempConfig("bias_age_unset",
        networkConfigRt("    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_FALSE(c1.enableSsrBiasMaxAge());       // load default: off
    c1.applyNetworkConsumerProfile(log);
    EXPECT_TRUE(c1.enableSsrBiasMaxAge());        // armed under consumption

    // Explicit false -> respected (presence check).
    const fs::path p2 = writeTempConfig("bias_age_false",
        networkConfigRt("    \"enable_ssr_bias_max_age\": false,\n"
                        "    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    c2.applyNetworkConsumerProfile(log);
    EXPECT_FALSE(c2.enableSsrBiasMaxAge());

    // Explicit true -> kept.
    const fs::path p3 = writeTempConfig("bias_age_true",
        networkConfigRt("    \"enable_ssr_bias_max_age\": true,\n"
                        "    \"network_consumer\": { \"enable\": true },\n", ""));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    c3.applyNetworkConsumerProfile(log);
    EXPECT_TRUE(c3.enableSsrBiasMaxAge());

    // bias=false consumption -> nothing armed (clock-only consumer).
    const fs::path p4 = writeTempConfig("bias_age_nobias",
        networkConfigRt("    \"network_consumer\": { \"enable\": true, \"bias\": false },\n", ""));
    GnssConfig c4;
    ASSERT_TRUE(c4.loadFromJson(p4, log));
    c4.applyNetworkConsumerProfile(log);
    EXPECT_FALSE(c4.enableSsrBiasMaxAge());

    std::error_code ec;
    fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec); fs::remove(p4, ec);
}

// RTA-9(b)/RTA-13 processing keys: ssr_clock_stale_hold_factor defaults 0 (rung
// off), parses a widening factor, and clamps (0,1] to 1 (the factor can only
// widen the clock apply window); ssr_bias_band_fallback defaults false.
TEST(NetworkConsumerProfile, ClockStaleHoldAndBandFallbackKeysParse) {
    LogManager log;

    const fs::path p1 = writeTempConfig("rung_defaults", networkConfigRt("", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_DOUBLE_EQ(c1.getSsrClockStaleHoldFactor(), 0.0);
    EXPECT_FALSE(c1.ssrBiasBandFallback());

    const fs::path p2 = writeTempConfig("rung_on",
        networkConfigRt("    \"ssr_clock_stale_hold_factor\": 3.0,\n"
                        "    \"ssr_bias_band_fallback\": true,\n", ""));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.getSsrClockStaleHoldFactor(), 3.0);
    EXPECT_TRUE(c2.ssrBiasBandFallback());

    const fs::path p3 = writeTempConfig("rung_clamp",
        networkConfigRt("    \"ssr_clock_stale_hold_factor\": 0.5,\n", ""));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_DOUBLE_EQ(c3.getSsrClockStaleHoldFactor(), 1.0);   // clamped: no hold

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// RTB-8: network.service_mode arms the coherent RT-service producer toggle set.
// BUNDLE-WINS semantics: an explicit non-zero numeric wins, but an explicit
// false on an armed bool (or an explicit 0 numeric) is overridden with a warn
// (see BundleOverridesExplicitDisables below).
TEST(NetworkServiceProfile, ServiceModeArmsCoherentSet) {
    LogManager log;

    // Default (no service_mode): every armed toggle stays at its factory default
    // (the OFF-path byte-identity proxy, incl. every P2-added knob).
    const fs::path p0 = writeTempConfig("svc_off", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig c0;
    ASSERT_TRUE(c0.loadFromJson(p0, log));
    EXPECT_FALSE(c0.networkServiceMode());
    EXPECT_FALSE(c0.networkEmitClockDisc());
    EXPECT_FALSE(c0.networkEmitUra());
    EXPECT_FALSE(c0.networkClockModel());
    EXPECT_FALSE(c0.networkEmitClockRate());   // decoupled C1: off by default (byte-identical)
    EXPECT_FALSE(c0.networkRaimWithinEpoch());
    EXPECT_DOUBLE_EQ(c0.networkRaimWithholdSigma0(), 0.0);
    EXPECT_FALSE(c0.networkSerializeDiscCounters());
    EXPECT_DOUBLE_EQ(c0.networkContractRebroadcastS(), 0.0);
    EXPECT_DOUBLE_EQ(c0.getNetworkConfig().health_interval_s, 0.0);
    EXPECT_FALSE(c0.getNetworkConfig().product_flush_per_epoch);
    // P2 additions stay at their defaults without service_mode.
    EXPECT_FALSE(c0.networkPerConstWithhold());
    EXPECT_FALSE(c0.networkEmitEpochStatusRecord());
    EXPECT_FALSE(c0.networkPivotVanishReturnSignal());
    EXPECT_FALSE(c0.networkStickyPivot());
    EXPECT_FALSE(c0.networkScopedHandoverResponse());
    EXPECT_DOUBLE_EQ(c0.getNetworkConfig().trace_rotate_mb, 0.0);

    // service_mode on → the FULL coherent producer set is armed.
    const fs::path p1 = writeTempConfig("svc_on",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.networkServiceMode());                        // provenance member (P5)
    EXPECT_TRUE(c1.networkEmitClockDisc());
    EXPECT_TRUE(c1.networkEmitUra());
    EXPECT_TRUE(c1.networkUraRetroBump());
    EXPECT_TRUE(c1.networkClockModel());
    EXPECT_TRUE(c1.networkPhaseLeveling());                      // carry_warm prerequisite
    EXPECT_TRUE(c1.networkRaim());                               // feeds the σ̂0 EWMA → URA scaling
    EXPECT_TRUE(c1.networkRaimAttribution());                    // complementary post-fit tier
    EXPECT_TRUE(c1.networkRaimWithinEpoch());
    EXPECT_DOUBLE_EQ(c1.networkRaimWithholdSigma0(), 6.0);       // 2x the default trigger 3.0
    EXPECT_TRUE(c1.networkEmitGenerationId());
    EXPECT_TRUE(c1.networkSerializeDiscCounters());
    EXPECT_TRUE(c1.getNetworkConfig().serialize_sbasis_contract);
    EXPECT_TRUE(c1.getNetworkConfig().product_flush_per_epoch);
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().health_interval_s, 30.0);
    // P2 additions: withhold carve-out, heartbeat, datum-signalling + handover
    // controls, trace rotation (keep stays at its parsed default 3).
    EXPECT_TRUE(c1.networkPerConstWithhold());
    EXPECT_TRUE(c1.networkEmitEpochStatusRecord());
    EXPECT_TRUE(c1.networkPivotVanishReturnSignal());
    EXPECT_TRUE(c1.networkStickyPivot());
    EXPECT_TRUE(c1.networkScopedHandoverResponse());
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().trace_rotate_mb, 256.0);
    EXPECT_EQ(c1.getNetworkConfig().trace_rotate_keep, 3);
    // NOT armed: user-path / policy numerics.
    EXPECT_TRUE(c1.getNetworkConfig().state_snapshot_path.empty());
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().warmup_ura_floor_m, 0.0);
    EXPECT_FALSE(c1.networkClockRunawayCheck());

    // Explicit NON-ZERO numeric wins over the profile default.
    const fs::path p2 = writeTempConfig("svc_override",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"serialize_sbasis_contract\": true,\n"
                      "    \"contract_rebroadcast_s\": 10.0,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.networkContractRebroadcastS(), 10.0);    // explicit kept
    EXPECT_TRUE(c2.networkEmitUra());                            // rest still armed

    std::error_code ec2; fs::remove(p0, ec2); fs::remove(p1, ec2); fs::remove(p2, ec2);
}

// network.emit_clock_rate: the decoupled C1 (clock-rate) emission toggle round-trips,
// is off by default (byte-identical), and is NOT armed by service_mode (it is the
// alternative to clock_model, which service_mode arms instead).  See
// docs/CLOCK_HADAMARD_CALIB — clock_model's two-state CARRY is a no-go; this emits the
// rate WITHOUT carrying.
TEST(NetworkEmitClockRate, TogglesAndDefaults) {
    LogManager log;

    const fs::path pdef = writeTempConfig("emit_rate_def",
        networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cdef;
    ASSERT_TRUE(cdef.loadFromJson(pdef, log));
    EXPECT_FALSE(cdef.networkEmitClockRate());                   // off by default

    const fs::path pon = writeTempConfig("emit_rate_on",
        networkConfig(kGpsGal, kPriosGps, "    \"emit_clock_rate\": true,\n"));
    GnssConfig con;
    ASSERT_TRUE(con.loadFromJson(pon, log));
    EXPECT_TRUE(con.networkEmitClockRate());                     // explicit on parses
    EXPECT_FALSE(con.networkClockModel());                       // independent of clock_model

    // service_mode does NOT arm emit_clock_rate (clock_model carries C1 under the bundle).
    const fs::path psvc = writeTempConfig("emit_rate_svc",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig csvc;
    ASSERT_TRUE(csvc.loadFromJson(psvc, log));
    EXPECT_FALSE(csvc.networkEmitClockRate());
    EXPECT_TRUE(csvc.networkClockModel());

    std::error_code ec; fs::remove(pdef, ec); fs::remove(pon, ec); fs::remove(psvc, ec);
}

// RTB-8/P1: BUNDLE-WINS — an explicit `false` on an armed bool, an explicit 0
// numeric, and a negative numeric (clamped to 0 at parse) are all overridden by
// the bundle.  Each such override emits one WARN at load; the warn itself is
// NOT assertable in this harness (LogManager writes to spdlog file sinks and
// exposes no inspectable capture sink), so these are behavioral asserts only.
TEST(NetworkServiceProfile, BundleOverridesExplicitDisables) {
    LogManager log;

    // Explicit false bools + explicit 0 numeric → bundle overrides all of them.
    const fs::path p1 = writeTempConfig("svc_explicit_false",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"sticky_pivot\": false,\n"
                      "    \"raim\": false,\n"
                      "    \"emit_ura\": false,\n"
                      "    \"contract_rebroadcast_s\": 0.0,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.networkStickyPivot());                        // bundle wins over explicit false
    EXPECT_TRUE(c1.networkRaim());
    EXPECT_TRUE(c1.networkEmitUra());
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);    // explicit 0 = disable → re-armed

    // Explicit NEGATIVE numeric: clamped to 0 at parse, then re-armed by the
    // bundle (the "clamped-then-armed" funnel the override warn covers).
    const fs::path p2 = writeTempConfig("svc_explicit_neg",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"contract_rebroadcast_s\": -1.0,\n"
                      "    \"health_interval_s\": -5.0,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.networkContractRebroadcastS(), 30.0);
    EXPECT_DOUBLE_EQ(c2.getNetworkConfig().health_interval_s, 30.0);

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

// RTB-8/P3(a) regression: the R11/R1 pathological-withhold clamp runs at parse,
// BEFORE the arming block — so a service-armed withhold of 2×trigger with a
// small explicit trigger (2×0.4 = 0.8 <= 1; σ̂0 is floored at 1) used to slip
// through and withhold essentially every warm epoch.  The post-arming re-run
// must catch it: the withhold comes out DISABLED (0), not armed <= 1, and the
// rest of the bundle stays armed.
TEST(NetworkServiceProfile, ServiceArmedPathologicalWithholdIsReclamped) {
    LogManager log;

    const fs::path p1 = writeTempConfig("svc_small_trigger",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"raim_sigma0_trigger\": 0.4,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_DOUBLE_EQ(c1.networkRaimWithholdSigma0(), 0.0);       // re-clamped, NOT 0.8
    EXPECT_TRUE(c1.networkRaimWithinEpoch());                    // rest of the bundle unaffected
    EXPECT_TRUE(c1.networkRaim());

    // Non-service parse-site clamp still behaves as before (idempotency anchor:
    // an explicit pathological ceiling is disabled at parse; the post-arming
    // re-run then sees an already-valid 0 and must NOT fire again).
    const fs::path p2 = writeTempConfig("nosvc_patho_withhold",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"raim_withhold_sigma0\": 0.5,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.networkRaimWithholdSigma0(), 0.0);       // parse-site clamp
    EXPECT_FALSE(c2.networkRaimWithinEpoch());                   // clamped BEFORE the coupling

    // P3(b) companion: a stray invalid clock_model_q2 with clock_model UNSET is
    // unvalidated at parse (the restore gates on the pre-arming flag); the
    // service bundle arms clock_model, so the post-arming re-run must restore it.
    const fs::path p3 = writeTempConfig("svc_bad_q2",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"clock_model_q2\": -1.0,\n"));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_TRUE(c3.networkClockModel());                         // service-armed
    EXPECT_DOUBLE_EQ(c3.networkClockModelQ2(), 1.5e-7);          // restored, not -1

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// RTB-8/P4(b): service_mode with an EMPTY network.product_output_path parses
// fine — the load emits the one coherence warn listing the inert file-side
// knobs (not assertable in this harness, see above) and the armed set is
// unchanged (the knobs stay armed; they simply have no file to write to until
// a path is configured).
TEST(NetworkServiceProfile, EmptyProductPathParsesWithArmedSetUnchanged) {
    LogManager log;

    // networkConfig writes no product_output_path → empty.
    const fs::path p1 = writeTempConfig("svc_no_product_path",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.getNetworkConfig().product_output_path.empty());
    EXPECT_TRUE(c1.getNetworkConfig().product_flush_per_epoch);   // still armed
    EXPECT_TRUE(c1.networkEmitEpochStatusRecord());               // still armed
    EXPECT_TRUE(c1.getNetworkConfig().serialize_sbasis_contract); // still armed
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);     // still armed

    std::error_code ec; fs::remove(p1, ec);
}

// RTB-6 P4(d) + P1: the service-observability knobs parse with sibling-convention
// clamps — health_interval_s < 0 → 0 (heartbeat off), trace_rotate_mb < 0 → 0
// (rotation off), trace_rotate_keep < 1 → 1 (at least one rolled generation).
// Defaults: 0 / 0 / 3 (heartbeat + rotation off ⇒ byte-identical).
TEST(NetworkServiceProfile, HealthAndRotationKnobsParseAndClamp) {
    LogManager log;

    // Defaults.
    const fs::path p0 = writeTempConfig("rot_default", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig c0;
    ASSERT_TRUE(c0.loadFromJson(p0, log));
    EXPECT_DOUBLE_EQ(c0.getNetworkConfig().trace_rotate_mb, 0.0);
    EXPECT_EQ(c0.getNetworkConfig().trace_rotate_keep, 3);

    // Plain parse.
    const fs::path p1 = writeTempConfig("rot_on",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"trace_rotate_mb\": 256.0,\n"
                      "    \"trace_rotate_keep\": 5,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().trace_rotate_mb, 256.0);
    EXPECT_EQ(c1.getNetworkConfig().trace_rotate_keep, 5);

    // Clamps (each logs a WARN; the load still succeeds).
    const fs::path p2 = writeTempConfig("rot_clamp",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"health_interval_s\": -5.0,\n"
                      "    \"trace_rotate_mb\": -1.0,\n"
                      "    \"trace_rotate_keep\": 0,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.getNetworkConfig().health_interval_s, 0.0);
    EXPECT_DOUBLE_EQ(c2.getNetworkConfig().trace_rotate_mb, 0.0);
    EXPECT_EQ(c2.getNetworkConfig().trace_rotate_keep, 1);

    std::error_code ec3;
    fs::remove(p0, ec3); fs::remove(p1, ec3); fs::remove(p2, ec3);
}

// ===========================================================================
// RTA-12: contract identity keys + SIF roster coordinates + ARP blunder gate
// ===========================================================================

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

// network.{frame_id,coord_epoch,use_sif_coordinates,coord_age_warn_years},
// the processing.network_consumer.expect_{frame_id,atx_id,orbit_ref} pins and
// realtime.arp_blunder_gate_m parse; everything defaults empty/off (the
// byte-identical OFF path).
TEST(NetworkIdentityConfig, IdentityAndGateKeysParseWithInertDefaults) {
    LogManager log;

    // Defaults: all empty / off.
    const fs::path p1 = writeTempConfig("identity_defaults", networkConfigRt("", ""));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.getNetworkConfig().frame_id.empty());
    EXPECT_TRUE(c1.getNetworkConfig().coord_epoch.empty());
    EXPECT_FALSE(c1.getNetworkConfig().use_sif_coordinates);
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().coord_age_warn_years, 0.0);
    EXPECT_TRUE(c1.getNetworkConsumerConfig().expect_frame_id.empty());
    EXPECT_TRUE(c1.getNetworkConsumerConfig().expect_atx_id.empty());
    EXPECT_TRUE(c1.getNetworkConsumerConfig().expect_orbit_ref.empty());
    EXPECT_DOUBLE_EQ(c1.getRealTimeConfig().arp_blunder_gate_m, 0.0);

    // Explicit values land in their structs.
    const fs::path p2 = writeTempConfig("identity_explicit",
        networkConfigRt(
            "    \"network_consumer\": { \"enable\": true, "
            "\"expect_frame_id\": \"IGS20\", \"expect_atx_id\": \"igs20.atx\", "
            "\"expect_orbit_ref\": \"CoM\" },\n",
            "    \"frame_id\": \"IGS20\",\n"
            "    \"coord_epoch\": \"2024-05-08\",\n"
            "    \"coord_age_warn_years\": 1.0,\n",
            "  \"realtime\": { \"arp_blunder_gate_m\": 10.0 },\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_EQ(c2.getNetworkConfig().frame_id, "IGS20");
    EXPECT_EQ(c2.getNetworkConfig().coord_epoch, "2024-05-08");
    EXPECT_DOUBLE_EQ(c2.getNetworkConfig().coord_age_warn_years, 1.0);
    EXPECT_EQ(c2.getNetworkConsumerConfig().expect_frame_id, "IGS20");
    EXPECT_EQ(c2.getNetworkConsumerConfig().expect_atx_id, "igs20.atx");
    EXPECT_EQ(c2.getNetworkConsumerConfig().expect_orbit_ref, "CoM");
    EXPECT_DOUBLE_EQ(c2.getRealTimeConfig().arp_blunder_gate_m, 10.0);

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

namespace {

// Network config with a station that carries NO position_ecef (splice hooks as
// networkConfigRt): the RTA-12 SIF-roster scenarios.
std::string networkConfigNoPos(const std::string& station_name,
                               const std::string& processing_extra,
                               const std::string& network_extra,
                               const std::string& top_extra = "") {
    return std::string("{\n") + top_extra +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        + processing_extra +
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"" + station_name + "\" }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

} // namespace

// The pre-RTA-12 hard error is UNTOUCHED when use_sif_coordinates is off (the
// default): a roster station without position_ecef fails the load.  With the
// toggle ON but no SIF registry loaded, the load still fails (the relaxation
// requires an actual coordinate source).
TEST(NetworkIdentityConfig, MissingPositionStaysHardErrorWithoutSif) {
    LogManager log;

    const fs::path p1 = writeTempConfig("nopos_default",
        networkConfigNoPos("BBBB", "", ""));
    GnssConfig c1;
    EXPECT_FALSE(c1.loadFromJson(p1, log));   // toggle off -> pre-RTA-12 hard error

    const fs::path p2 = writeTempConfig("nopos_toggle_no_sif",
        networkConfigNoPos("BBBB", "", "    \"use_sif_coordinates\": true,\n"));
    GnssConfig c2;
    EXPECT_FALSE(c2.loadFromJson(p2, log));   // toggle on, but no SIF registry loaded

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

// Fixture-backed SIF roster resolution (skipped when the bundled igs20 SIF is
// absent): with the toggle on, a run date, and the SIF loaded, a station named
// CIT100USA resolves to its velocity-propagated occupation coordinate; explicit
// JSON ECEF always wins for its neighbour; coord_epoch defaults to the
// propagation date; an unknown station or a missing run date still fails loud.
TEST(NetworkIdentityConfig, SifResolvesRosterCoordinates) {
    const fs::path sif_path =
        fs::path(GNSS_TEST_DATA_DIR) / "static" / "sif" / "igs20_SIF.json";
    if (std::string(GNSS_TEST_DATA_DIR).empty() || !fs::exists(sif_path))
        GTEST_SKIP() << "igs20_SIF.json fixture not present";
    LogManager log;

    // Forward slashes for the JSON string literal.
    std::string sif_str = sif_path.generic_string();
    const std::string sif_files_block =
        "  \"files\": { \"station_info_path\": \"" + sif_str + "\" },\n";
    const std::string date_block =
        "  \"data_download\": { \"year\": 2024, \"doy\": 129 },\n";

    const fs::path p1 = writeTempConfig("sif_resolve",
        networkConfigNoPos("CIT100USA",
            "    \"enable_station_info\": true,\n",
            "    \"use_sif_coordinates\": true,\n",
            sif_files_block + date_block));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    const auto& sts = c1.getNetworkConfig().stations;
    ASSERT_EQ(sts.size(), 2u);
    // Explicit JSON ECEF wins: AAAA untouched, not SIF-resolved.
    EXPECT_FALSE(sts[0].position_from_sif);
    EXPECT_DOUBLE_EQ(sts[0].position_ecef[0], -2493304.99);
    // CIT100USA: SIF occupation, velocity-propagated 2020.0 -> 2024-129 (plate
    // motion is cm/yr, so within 1 m of the reference-epoch coordinate).
    EXPECT_TRUE(sts[1].has_position);
    EXPECT_TRUE(sts[1].position_from_sif);
    EXPECT_NEAR(sts[1].position_ecef[0], -2491490.590, 1.0);
    EXPECT_NEAR(sts[1].position_ecef[1], -4660802.919, 1.0);
    EXPECT_NEAR(sts[1].position_ecef[2],  3559129.086, 1.0);
    // ...but NOT the un-propagated reference coordinate: the 4.35-year velocity
    // propagation (~ -0.0286 m/yr in X alone) must actually be applied.
    EXPECT_GT(std::abs(sts[1].position_ecef[0] - (-2491490.58952111)), 0.05);
    // coord_epoch defaulted to the propagation date (decimal year 2024.35...).
    EXPECT_EQ(c1.getNetworkConfig().coord_epoch.rfind("2024.35", 0), 0u);

    // Unknown station: fail loud, don't silently run without coordinates.
    const fs::path p2 = writeTempConfig("sif_unknown",
        networkConfigNoPos("ZZZZ99XXX",
            "    \"enable_station_info\": true,\n",
            "    \"use_sif_coordinates\": true,\n",
            sif_files_block + date_block));
    GnssConfig c2;
    EXPECT_FALSE(c2.loadFromJson(p2, log));

    // No run date (data_download.year/doy unset): propagation impossible -> fail.
    const fs::path p3 = writeTempConfig("sif_nodate",
        networkConfigNoPos("CIT100USA",
            "    \"enable_station_info\": true,\n",
            "    \"use_sif_coordinates\": true,\n",
            sif_files_block));
    GnssConfig c3;
    EXPECT_FALSE(c3.loadFromJson(p3, log));

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// ===========================================================================
// RTB-4: realtime.use_precise_orbits — the "ULT orbits + network clocks" pin
// ===========================================================================

// The key parses, defaults OFF, and splits the orbit gate from the clock gate
// exactly as documented: with the default the two gates are EQUAL (every
// product-resolution site byte-identical); armed in a live real-time session,
// precise SP3 orbits are loaded (orbit gate false) while satellite clocks stay
// on the live/network source (clock gate true).
TEST(RealtimePreciseOrbitConfig, UsePreciseOrbitsParsesDefaultsOffAndSplitsGates) {
    LogManager log;

    // Default: key absent -> false; live RT session -> both gates live/equal.
    const fs::path p1 = writeTempConfig("upo_default",
        networkConfigRt("", "",
            "  \"app_mode\": \"realtime\",\n"
            "  \"realtime\": { \"input_type\": \"ntrip\" },\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_FALSE(c1.getRealTimeConfig().use_precise_orbits);
    EXPECT_TRUE(c1.realtimeUsesLiveOrbitClockSource());
    EXPECT_EQ(c1.realtimeUsesLiveOrbitSource(), c1.realtimeUsesLiveOrbitClockSource());

    // Armed: precise orbits pinned while the clock gate stays live.
    const fs::path p2 = writeTempConfig("upo_on",
        networkConfigRt("", "",
            "  \"app_mode\": \"realtime\",\n"
            "  \"realtime\": { \"input_type\": \"ntrip\", \"use_precise_orbits\": true },\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_TRUE(c2.getRealTimeConfig().use_precise_orbits);
    EXPECT_FALSE(c2.realtimeUsesLiveOrbitSource());        // SP3 orbits ARE loaded
    EXPECT_TRUE(c2.realtimeUsesLiveOrbitClockSource());    // precise CLK still skipped

    // Post-processing: the key parses but both gates stay false (precise
    // products are always authoritative outside real-time mode).
    const fs::path p3 = writeTempConfig("upo_post",
        networkConfigRt("", "",
            "  \"realtime\": { \"use_precise_orbits\": true },\n"));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    EXPECT_TRUE(c3.getRealTimeConfig().use_precise_orbits);
    EXPECT_FALSE(c3.realtimeUsesLiveOrbitSource());
    EXPECT_FALSE(c3.realtimeUsesLiveOrbitClockSource());

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec);
}

// ===========================================================================
// RTF-1 Card 1: network.apriori_source + network.apriori_clock_anchor_sigma_m
// ===========================================================================

// Default network config: a-priori source is Sp3 (precise SP3/CLK identity —
// today's byte-identical behaviour) and the anchor sigma is 0.0 (auto).
TEST(NetworkAprioriSourceConfig, DefaultsToSp3AutoAnchor) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_default", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
    EXPECT_DOUBLE_EQ(cfg.networkAprioriClockAnchorSigmaM(), 0.0);
    std::error_code ec; fs::remove(p, ec);
}

// "brdc_rts" parses to BrdcRts (case-insensitive) and — on its own, with the
// clock+phase-bias-only V1 slice (no code bias) — loads without error.  The
// anchor sigma parses to its explicit value.
TEST(NetworkAprioriSourceConfig, ParsesBrdcRtsAndAnchorSigma) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_brdc",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"apriori_source\": \"BRDC_RTS\",\n"
                      "    \"apriori_clock_anchor_sigma_m\": 3.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::BrdcRts);
    EXPECT_DOUBLE_EQ(cfg.networkAprioriClockAnchorSigmaM(), 3.0);
    std::error_code ec; fs::remove(p, ec);
}

// A negative anchor sigma is meaningless -> clamped to 0 (auto), load succeeds.
TEST(NetworkAprioriSourceConfig, NegativeAnchorSigmaClampsToAuto) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_negsig",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"apriori_clock_anchor_sigma_m\": -1.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_DOUBLE_EQ(cfg.networkAprioriClockAnchorSigmaM(), 0.0);
    std::error_code ec; fs::remove(p, ec);
}

// An unknown apriori_source warns and falls back to Sp3 (load still succeeds).
TEST(NetworkAprioriSourceConfig, UnknownSourceFallsBackToSp3) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_bogus",
        networkConfig(kGpsGal, kPriosGps, "    \"apriori_source\": \"sideways\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
    std::error_code ec; fs::remove(p, ec);
}

// Validation 1 (hard error): brdc_rts + estimate_sat_code_bias but NOT
// estimate_iono_sh — the external GIM iono constraint is post-processing-only,
// so the real-time code-bias slice has no iono constraint.  Fail loud at load.
TEST(NetworkAprioriSourceConfig, BrdcRtsCodeBiasWithoutIonoShIsError) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_brdc_gim",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"apriori_source\": \"brdc_rts\",\n"
                      "    \"estimate_sat_code_bias\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// Validation 1 legal path: brdc_rts + estimate_iono_sh (which FORCES the code
// bias slice on) is the supported real-time W6 combination — loads fine, both
// flags on.  (The sp3-mode GIM path is unaffected: EstimateSatCodeBiasForces-
// Prerequisites above still loads code bias with the default sp3 source.)
TEST(NetworkAprioriSourceConfig, BrdcRtsWithIonoShIsLegal) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_brdc_sh",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"apriori_source\": \"brdc_rts\",\n"
                      "    \"estimate_iono_sh\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::BrdcRts);
    EXPECT_TRUE(cfg.estimateNetworkIonoSh());
    EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());   // forced on by iono_sh
    std::error_code ec; fs::remove(p, ec);
}

// service_mode does NOT arm apriori_source (an input-source selection, orthogonal
// to the toggle bundle): a service run left at the default stays Sp3, and an
// explicit brdc_rts is preserved through the bundle.
TEST(NetworkAprioriSourceConfig, ServiceModeDoesNotArmAprioriSource) {
    LogManager log;

    const fs::path p1 = writeTempConfig("apri_svc_default",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.networkServiceMode());
    EXPECT_EQ(c1.networkAprioriSource(), NetworkAprioriSource::Sp3);   // untouched by the bundle

    const fs::path p2 = writeTempConfig("apri_svc_brdc",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"apriori_source\": \"brdc_rts\",\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_EQ(c2.networkAprioriSource(), NetworkAprioriSource::BrdcRts);  // explicit preserved
    EXPECT_TRUE(c2.networkEmitUra());   // rest of the bundle still armed

    std::error_code ec; fs::remove(p1, ec); fs::remove(p2, ec);
}

// ---------------------------------------------------------------------------
// RTF-1 Card 5 (plan D6): resolveNetworkAprioriClockAnchorSigmaM() — the
// mode-aware AUTO resolution the producer folds.  Pure config function; the
// driver's < 100 m binding gate is NOT applied here.
// ---------------------------------------------------------------------------

// sp3 + auto (default): resolves to initial_std.sat_clock UNCHANGED — the
// byte-identity guarantee.  The loose default (>100 m) keeps the driver anchor
// OFF, so every existing sp3 network config is unaffected.
TEST(NetworkAprioriSourceConfig, ResolveAnchorSp3AutoIsInitialStdUnchanged) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_res_sp3", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
    EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(),
                     cfg.getInitialStd().sat_clock);
    EXPECT_GT(cfg.getInitialStd().sat_clock, 100.0);   // loose default ⇒ driver anchor stays off
    std::error_code ec; fs::remove(p, ec);
}

// brdc_rts + auto: resolves to the 3.0 m broadcast-grade default (< 100 m ⇒ the
// driver anchor binds), NOT the loose initial_std.sat_clock.
TEST(NetworkAprioriSourceConfig, ResolveAnchorBrdcRtsAutoIsThreeMetres) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_res_brdc",
        networkConfig(kGpsGal, kPriosGps, "    \"apriori_source\": \"brdc_rts\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::BrdcRts);
    EXPECT_DOUBLE_EQ(cfg.networkAprioriClockAnchorSigmaM(), 0.0);   // auto
    EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(), 3.0);
    std::error_code ec; fs::remove(p, ec);
}

// Explicit positive sigma wins in BOTH modes (overrides the auto), sp3 case:
// a user pinning 5 m in sp3 mode gets 5 m, not initial_std.sat_clock.
TEST(NetworkAprioriSourceConfig, ResolveAnchorExplicitWinsInSp3) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_res_sp3_expl",
        networkConfig(kGpsGal, kPriosGps, "    \"apriori_clock_anchor_sigma_m\": 5.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
    EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(), 5.0);
    std::error_code ec; fs::remove(p, ec);
}

// Explicit positive sigma wins in BOTH modes, brdc_rts case: 5 m overrides the
// 3 m auto default.
TEST(NetworkAprioriSourceConfig, ResolveAnchorExplicitWinsInBrdcRts) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_res_brdc_expl",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"apriori_source\": \"brdc_rts\",\n"
                      "    \"apriori_clock_anchor_sigma_m\": 5.0,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    ASSERT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::BrdcRts);
    EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(), 5.0);
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// Producer datum-trajectory keys (wire defects 2-3): broadcast-anchored clock
// datum, sum-to-zero set-change disc, code-bias value jump test, consumer ISB
// reinflation, and the closed-loop bias-age arming counterpart of RTA-9(a)
// ===========================================================================

// network.clock_datum_broadcast_anchored defaults OFF (byte-identical) and is
// only meaningful when the a-priori clock IS the broadcast reference: the datum
// soft-anchors each satellite clock to its OWN a-priori, so under the default
// sp3 a-priori the anchor would merely re-derive the precise product (and the
// loose sp3 auto sigma leaves the gauge unrealized anyway).  Fail loud rather
// than run with an undefined clock datum.
TEST(NetworkClockDatumConfig, BroadcastAnchoredDefaultsOffAndRequiresBrdcRts) {
    LogManager log;

    // Default: key absent -> false (the OFF path — no anchors folded).
    {
        const fs::path p = writeTempConfig("ba_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkClockDatumBroadcastAnchored());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed without apriori_source='brdc_rts' (default sp3): hard error.
    {
        const fs::path p = writeTempConfig("ba_sp3",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    // Legal combination: brdc_rts a-priori (whose auto anchor sigma resolves to
    // the binding 3.0 m broadcast-grade default) -> loads, getter true.
    {
        const fs::path p = writeTempConfig("ba_brdc",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"apriori_source\": \"brdc_rts\",\n"
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkClockDatumBroadcastAnchored());
        std::error_code ec; fs::remove(p, ec);
    }
}

// Exactly one clock-gauge realization may be selected — sum-to-zero and the
// broadcast anchors both close the same rank deficiency, so together they
// over-constrain and the rank gate CANNOT catch it (both look valid to it).
// And the anchors ARE the gauge: a non-binding explicit sigma (>= 100 m)
// leaves the clock common-mode eigenvalue at or below the rank-gate cutoff —
// an undefined datum.  Both misconfigurations fail loud at load.
TEST(NetworkClockDatumConfig, BroadcastAnchoredRejectsSumToZeroAndNonBindingSigma) {
    LogManager log;

    // Both clock gauges armed (brdc_rts satisfied so the mutual exclusion is
    // the ONLY failing constraint): hard error.
    {
        const fs::path p = writeTempConfig("ba_s2z",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"apriori_source\": \"brdc_rts\",\n"
                          "    \"clock_datum_sum_to_zero\": true,\n"
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    // Explicit anchor sigma at the 100 m boundary overrides the binding 3.0 m
    // brdc_rts auto default with a value too weak to realize the datum: error.
    {
        const fs::path p = writeTempConfig("ba_weak_sigma",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"apriori_source\": \"brdc_rts\",\n"
                          "    \"apriori_clock_anchor_sigma_m\": 100.0,\n"
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
}

// network.clock_disc_on_set_change models "membership change = clock-datum
// re-reference", which is true ONLY for the sum-to-zero gauge (the pivot pin
// holds while the pivot is up; the broadcast anchors never move).  Armed under
// any other datum it would mint spurious consumer resets — the exact churn
// class (defect 3) this trigger exists to signal away — so it fails loud.
// With s2z it loads and AUTO-ARMS network.emit_clock_disc: the set-change
// trigger only reaches the wire through the clock discontinuity channel.
TEST(NetworkDiscSignalConfig, ClockDiscOnSetChangeRequiresSumToZeroAndArmsEmit) {
    LogManager log;

    // Default: both the trigger and the disc channel stay off (byte-identical).
    {
        const fs::path p = writeTempConfig("setchg_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkClockDiscOnSetChange());
        EXPECT_FALSE(cfg.networkEmitClockDisc());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed without the sum-to-zero gauge (default pivot datum): hard error.
    {
        const fs::path p = writeTempConfig("setchg_no_s2z",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_disc_on_set_change\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed with s2z: loads, and emit_clock_disc (unset by the user) is
    // auto-armed so the set-change counter bump actually travels on the wire.
    {
        const fs::path p = writeTempConfig("setchg_s2z",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_datum_sum_to_zero\": true,\n"
                          "    \"clock_disc_on_set_change\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkClockDiscOnSetChange());
        EXPECT_TRUE(cfg.networkEmitClockDisc());          // auto-armed by the coupling
        std::error_code ec; fs::remove(p, ec);
    }
}

// network.code_bias_disc_jump_test (emitter-side VALUE-jump disc trigger for
// weak-separation code-bias wander) and estimation.clock_disc_isb_reinflate_std
// (consumer-side ISB covariance reinflation on a clock disc bump) parse with
// inert defaults — both are default-OFF additions on the producer-wire-defect
// path and must not perturb any existing config (the OFF byte-identity proxy).
TEST(NetworkDiscSignalConfig, JumpTestAndIsbReinflateParseWithInertDefaults) {
    LogManager log;

    // Defaults: false / 0.0 — nothing armed, nothing reinflated.
    {
        const fs::path p = writeTempConfig("disc_defaults",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCodeBiasDiscJumpTest());
        EXPECT_DOUBLE_EQ(cfg.clockDiscIsbReinflateStd(), 0.0);
        std::error_code ec; fs::remove(p, ec);
    }
    // Explicit values land in their getters.  The jump test without
    // network.estimate_sat_code_bias is INERT (warn-only, no code biases are
    // emitted) — the load must still succeed.
    {
        const fs::path p = writeTempConfig("disc_armed",
            networkConfigRt("",
                            "    \"code_bias_disc_jump_test\": true,\n",
                            "  \"estimation\": { \"clock_disc_isb_reinflate_std\": 0.25 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCodeBiasDiscJumpTest());
        EXPECT_DOUBLE_EQ(cfg.clockDiscIsbReinflateStd(), 0.25);
        std::error_code ec; fs::remove(p, ec);
    }
}

// deriveClosedLoopConsumerConfig arms processing.enable_ssr_bias_max_age on the
// DERIVED config when network products are consumed and the user wrote no key —
// bias consumption without the staleness gate silently applies unboundedly
// stale biases across producer withhold gaps (the A5/RTA-9(a) defect class the
// standalone profile already guards; this is its closed-loop counterpart).
// The producer's own config is untouched, and an explicit user key (either
// polarity) is respected via the same presence check as the profile path.
TEST(ClosedLoopBiasGateConfig, DerivedConsumerArmsBiasMaxAge) {
    LogManager log;

    // No key written: producer stays off, the derived consumer is armed.
    const fs::path p1 = writeTempConfig("cl_biasage_unset",
        networkConfigRt("",
            "    \"closed_loop\": { \"consume_network_products\": true },\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_FALSE(c1.enableSsrBiasMaxAge());               // producer run untouched
    const GnssConfig d1 = c1.deriveClosedLoopConsumerConfig();
    EXPECT_TRUE(d1.enableSsrBiasMaxAge());                // armed under consumption

    // Explicit false in the producer JSON: respected — the derived stays off.
    const fs::path p2 = writeTempConfig("cl_biasage_false",
        networkConfigRt("    \"enable_ssr_bias_max_age\": false,\n",
            "    \"closed_loop\": { \"consume_network_products\": true },\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    const GnssConfig d2 = c2.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(d2.enableSsrBiasMaxAge());

    // Explicit true: simply kept (no double-arm surprises).
    const fs::path p3 = writeTempConfig("cl_biasage_true",
        networkConfigRt("    \"enable_ssr_bias_max_age\": true,\n",
            "    \"closed_loop\": { \"consume_network_products\": true },\n"));
    GnssConfig c3;
    ASSERT_TRUE(c3.loadFromJson(p3, log));
    const GnssConfig d3 = c3.deriveClosedLoopConsumerConfig();
    EXPECT_TRUE(d3.enableSsrBiasMaxAge());

    // Consumption off (the A/B BASELINE consumer: precise SP3/CLK + .BIA only):
    // nothing consumed, nothing to age-gate — not armed.
    const fs::path p4 = writeTempConfig("cl_biasage_noconsume",
        networkConfigRt("",
            "    \"closed_loop\": { \"consume_network_products\": false },\n"));
    GnssConfig c4;
    ASSERT_TRUE(c4.loadFromJson(p4, log));
    const GnssConfig d4 = c4.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(d4.enableSsrBiasMaxAge());

    std::error_code ec;
    fs::remove(p1, ec); fs::remove(p2, ec); fs::remove(p3, ec); fs::remove(p4, ec);
}

// Card 3 (network.phase_datum_eq34): parses OFF by default, ON in isolation, and
// with the two sub-knobs (preferred datum receiver, pin-to-round).
TEST(CodeBiasNetworkConfig, Eq34PhaseDatumParses) {
    LogManager log;
    // Default OFF, byte-identical.
    {
        const fs::path p = writeTempConfig("eq34_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkPhaseDatumEq34());
        EXPECT_TRUE(cfg.networkPhaseDatumEq34PinRound());        // default true
        EXPECT_TRUE(cfg.networkPhaseDatumEq34Receiver().empty());
        std::error_code ec; fs::remove(p, ec);
    }
    // ON in isolation, with sub-knobs.
    {
        const fs::path p = writeTempConfig("eq34_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"phase_datum_eq34_receiver\": \"BBBB\",\n"
                "    \"phase_datum_eq34_pin_round\": false,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkPhaseDatumEq34());
        EXPECT_FALSE(cfg.networkPhaseDatumEq34PinRound());
        EXPECT_EQ(cfg.networkPhaseDatumEq34Receiver(), "BBBB");
        std::error_code ec; fs::remove(p, ec);
    }
}

// Card 3 mutual exclusion: eq34 + ensemble is two realizations of the SAME gauge
// — the load must FAIL loud.  eq34 + prioritized is allowed (eq34 supersedes).
TEST(CodeBiasNetworkConfig, Eq34EnsembleMutexFailsPrioritizedSupersedes) {
    LogManager log;
    // eq34 && ensemble -> load fails.
    {
        const fs::path p = writeTempConfig("eq34_ens",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"phase_datum_ensemble\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log))
            << "phase_datum_eq34 + phase_datum_ensemble must be rejected at load.";
        std::error_code ec; fs::remove(p, ec);
    }
    // eq34 && prioritized -> loads (eq34 supersedes; both getters reflect config).
    {
        const fs::path p = writeTempConfig("eq34_prio",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"phase_datum_prioritized\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkPhaseDatumEq34());
        EXPECT_TRUE(cfg.networkPhaseDatumPrioritized());
        std::error_code ec; fs::remove(p, ec);
    }
}

// Card 4 (M1) D5 (network.carry_invariant_innov_gate): parses OFF by default (byte-
// identical), ON with carry_invariant (which itself needs eq34), and reads the k knob.
TEST(CodeBiasNetworkConfig, CarryInvariantInnovGateParses) {
    LogManager log;
    // Default OFF, k defaults to 4.0.
    {
        const fs::path p = writeTempConfig("d5_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovGate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovK(), 4.0);
        EXPECT_FALSE(cfg.networkCarryInvariantInnovRefold());
        std::error_code ec; fs::remove(p, ec);
    }
    // ON atop carry_invariant + eq34, with a custom k.
    {
        const fs::path p = writeTempConfig("d5_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_innov_gate\": true,\n"
                "    \"carry_invariant_innov_k\": 5.0,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariantInnovGate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovK(), 5.0);
        std::error_code ec; fs::remove(p, ec);
    }
}

// Card 4 (M1) D5: the innovation gate is defined only on the carried DDs — arming it
// without carry_invariant is an incoherent config and must FAIL loud at load.
TEST(CodeBiasNetworkConfig, CarryInvariantInnovGateRequiresCarryInvariant) {
    LogManager log;
    const fs::path p = writeTempConfig("d5_no_carry",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant_innov_gate\": true,\n"));   // no carry_invariant
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log))
        << "carry_invariant_innov_gate without carry_invariant must be rejected at load.";
    std::error_code ec; fs::remove(p, ec);
}

// Card 4 (M1) D5 v2: the LS-VCE calibration parses OFF by default (byte-identical), ON atop
// the innovation gate with a window, and is REJECTED without the gate (nothing to calibrate).
TEST(CodeBiasNetworkConfig, CarryInvariantInnovCalibrateParsesAndRequiresGate) {
    LogManager log;
    // Default OFF, window 60.
    {
        const fs::path p = writeTempConfig("d5v2_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovCalibrate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovCalibrateWindow(), 60.0);
        std::error_code ec; fs::remove(p, ec);
    }
    // ON atop carry_invariant + eq34 + gate, with a custom window.
    {
        const fs::path p = writeTempConfig("d5v2_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_innov_gate\": true,\n"
                "    \"carry_invariant_innov_calibrate\": true,\n"
                "    \"carry_invariant_innov_calibrate_window\": 30.0,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariantInnovCalibrate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovCalibrateWindow(), 30.0);
        std::error_code ec; fs::remove(p, ec);
    }
    // Calibrate without the gate → load FAILS.
    {
        const fs::path p = writeTempConfig("d5v2_no_gate",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_innov_calibrate\": true,\n"));   // no gate
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log))
            << "carry_invariant_innov_calibrate without the innovation gate must be rejected.";
        std::error_code ec; fs::remove(p, ec);
    }
}

// M5 card WL-1: network.mw_wl.enable rides the Card-4 chord basis, so it REQUIRES
// network.carry_invariant (SPEC §11 Q1 mutex).  Default OFF (byte-identical), and the
// knobs parse when armed alongside carry_invariant.
TEST(CodeBiasNetworkConfig, MwWlSeedContract) {
    LogManager log;
    // Default: absent → OFF (the producer is byte-identical).
    {
        const fs::path p = writeTempConfig("mwwl_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkMwWlSeed());
        std::error_code ec; fs::remove(p, ec);
    }
    // enable=true WITHOUT carry_invariant → load FAILS (fail-loud mutex, not a silent no-op).
    {
        const fs::path p = writeTempConfig("mwwl_no_carry",
            networkConfig(kGpsGal, kPriosGps,
                "    \"mw_wl\": { \"enable\": true },\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log))
            << "network.mw_wl.enable without network.carry_invariant must be rejected.";
        std::error_code ec; fs::remove(p, ec);
    }
    // enable=true WITH carry_invariant (+eq34) → loads; the knobs parse.
    {
        const fs::path p = writeTempConfig("mwwl_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"enable\": true, \"min_arc_epochs\": 15, "
                "\"sigma_floor_cyc\": 0.08, \"max_rows_per_epoch\": 500 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkMwWlSeed());
        EXPECT_EQ(cfg.getNetworkConfig().mw_wl.min_arc_epochs, 15);
        EXPECT_NEAR(cfg.getNetworkConfig().mw_wl.sigma_floor_cyc, 0.08, 1e-12);
        EXPECT_EQ(cfg.getNetworkConfig().mw_wl.max_rows_per_epoch, 500);
        std::error_code ec; fs::remove(p, ec);
    }
    // min_arc_epochs below 2 clamps to 2 (a variance-of-mean needs >=2 samples).
    {
        const fs::path p = writeTempConfig("mwwl_clamp",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"enable\": true, \"min_arc_epochs\": 1 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_EQ(cfg.getNetworkConfig().mw_wl.min_arc_epochs, 2);
        std::error_code ec; fs::remove(p, ec);
    }
}

// M5 card UWL-1 (docs/SPEC_uwl_card.md §5): the per-constellation top tier is the EWL
// machinery run on the lane-tier table, so uwl_enable REQUIRES ewl_enable (fail-loud);
// all knobs default OFF/inert (byte-identical); gal_top_band validates to {7,8}; the
// cold-fix clamps keep the a-priori σ positive and the λ floor above the GPS EWL.
TEST(CodeBiasNetworkConfig, UwlTierContract) {
    LogManager log;
    // Defaults: absent → OFF, top band '7', cold fix off with the SPEC numbers.
    {
        const fs::path p = writeTempConfig("uwl_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_FALSE(mw.uwl_enable);
        EXPECT_FALSE(mw.uwl_cold_fix);
        EXPECT_FALSE(mw.uwl_mid_enable);
        EXPECT_EQ(mw.uwl_gal_top_band, '7');
        EXPECT_NEAR(mw.uwl_cold_sigma_m, 0.7, 1e-12);
        EXPECT_NEAR(mw.uwl_cold_min_lambda_m, 8.0, 1e-12);
        std::error_code ec; fs::remove(p, ec);
    }
    // uwl_enable without ewl_enable → load FAILS (the tier IS the EWL machinery).
    {
        const fs::path p = writeTempConfig("uwl_no_ewl",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"uwl_enable\": true },\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log))
            << "network.mw_wl.uwl_enable without ewl_enable must be rejected.";
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed: knobs parse; gal_top_band 8 selects the E5-AltBOC 19.5 m tier.
    {
        const fs::path p = writeTempConfig("uwl_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"ewl_enable\": true, \"uwl_enable\": true, "
                "\"uwl_gal_top_band\": 8, \"uwl_cold_fix\": true, \"uwl_mid_enable\": true, "
                "\"uwl_cold_sigma_m\": 0.5, \"uwl_cold_min_lambda_m\": 9.0 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_TRUE(mw.uwl_enable);
        EXPECT_TRUE(mw.uwl_cold_fix);
        EXPECT_TRUE(mw.uwl_mid_enable);
        EXPECT_EQ(mw.uwl_gal_top_band, '8');
        EXPECT_NEAR(mw.uwl_cold_sigma_m, 0.5, 1e-12);
        EXPECT_NEAR(mw.uwl_cold_min_lambda_m, 9.0, 1e-12);
        std::error_code ec; fs::remove(p, ec);
    }
    // Validation clamps: bad top band keeps 7; σ≤0 restores 0.7; a λ floor that would
    // admit the GPS 5.861 m EWL clamps to 6.0 (the cold fix is a UWL privilege).
    {
        const fs::path p = writeTempConfig("uwl_clamps",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"ewl_enable\": true, \"uwl_enable\": true, "
                "\"uwl_gal_top_band\": 9, \"uwl_cold_sigma_m\": -1.0, "
                "\"uwl_cold_min_lambda_m\": 5.0 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_EQ(mw.uwl_gal_top_band, '7');
        EXPECT_NEAR(mw.uwl_cold_sigma_m, 0.7, 1e-12);
        EXPECT_NEAR(mw.uwl_cold_min_lambda_m, 6.0, 1e-12);
        std::error_code ec; fs::remove(p, ec);
    }
}

// Increment 5 + v2 (docs/SPEC_uwl_card.md §1b/§7.5, HANDOFF reconciliation): the
// dual-top tier and the per-family pair census.  Defaults OFF (byte-identical);
// dual_top forces ewl_fold_rows OFF (a doubly-decided row must never fold twice);
// family_min_arcs clamps to ≥1; the inert combinations load with warns, not errors
// (the wiring gates them on the masters).
TEST(CodeBiasNetworkConfig, UwlDualTopAndFamilyContract) {
    LogManager log;
    // Defaults: absent → all OFF, min_arcs 8.
    {
        const fs::path p = writeTempConfig("uwl_dual_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_FALSE(mw.uwl_dual_top);
        EXPECT_FALSE(mw.uwl_sib_gate);
        EXPECT_FALSE(mw.uwl_family_pairs);
        EXPECT_EQ(mw.uwl_family_min_arcs, 8);
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed dual + sib gate + family: knobs parse; ewl_fold_rows is FORCED off
    // under dual_top even when explicitly requested (the F-A refold class).
    {
        const fs::path p = writeTempConfig("uwl_dual_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"lane_carry\": { \"enable\": true },\n"
                "    \"mw_wl\": { \"ewl_enable\": true, \"uwl_enable\": true, "
                "\"uwl_dual_top\": true, \"uwl_sib_gate\": true, "
                "\"uwl_family_pairs\": true, \"uwl_family_min_arcs\": 12, "
                "\"ewl_fold_rows\": true },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_TRUE(mw.uwl_dual_top);
        EXPECT_TRUE(mw.uwl_sib_gate);
        EXPECT_TRUE(mw.uwl_family_pairs);
        EXPECT_EQ(mw.uwl_family_min_arcs, 12);
        EXPECT_FALSE(mw.ewl_fold_rows)
            << "uwl_dual_top must force ewl_fold_rows OFF (a cycle decided by both "
               "rungs must never fold two integer rows).";
        std::error_code ec; fs::remove(p, ec);
    }
    // Inert combinations still LOAD (warned): dual/sib/family without their masters.
    // min_arcs clamps to 1.
    {
        const fs::path p = writeTempConfig("uwl_dual_inert",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"ewl_enable\": true, "
                "\"uwl_dual_top\": true, \"uwl_sib_gate\": true, "
                "\"uwl_family_pairs\": true, \"uwl_family_min_arcs\": 0 },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log))
            << "inert dual/sib/family knobs must warn, not reject.";
        const auto& mw = cfg.getNetworkConfig().mw_wl;
        EXPECT_TRUE(mw.uwl_dual_top);      // parsed; wiring gates on uwl_enable
        EXPECT_EQ(mw.uwl_family_min_arcs, 1);
        std::error_code ec; fs::remove(p, ec);
    }
}

// Per-sample bridge iono de-bias (the 60-ep age-bias fix): defaults OFF
// (byte-identical raw accumulation); parses when armed with the bridge;
// inert-warns (loads) without ewl_enable.
TEST(CodeBiasNetworkConfig, BridgeSampleDebiasContract) {
    LogManager log;
    {
        const fs::path p = writeTempConfig("psd_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.getNetworkConfig().mw_wl.bridge_sample_debias);
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("psd_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"mw_wl\": { \"ewl_enable\": true, "
                "\"bridge_sample_debias\": true },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.getNetworkConfig().mw_wl.bridge_sample_debias);
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("psd_inert",
            networkConfig(kGpsGal, kPriosGps,
                "    \"mw_wl\": { \"bridge_sample_debias\": true },\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log))
            << "bridge_sample_debias without ewl_enable must warn, not reject.";
        EXPECT_TRUE(cfg.getNetworkConfig().mw_wl.bridge_sample_debias);
        std::error_code ec; fs::remove(p, ec);
    }
}
