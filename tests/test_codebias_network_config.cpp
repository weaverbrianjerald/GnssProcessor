/**
 * @file test_codebias_network_config.cpp
 * @brief Config-contract tests for satellite code-bias (OSB) estimation in the
 *        PPP-RTK network producer (W6).
 *
 * network.estimate_sat_code_bias is the single switch for the coupled W6 slice:
 * the network estimates its own per-(satellite,signal) code biases and emits
 * them so the user can skip the external .BIA.  The satellite DCB is observable
 * only with the receiver DCB, the GIM ionosphere constraint and the satellite
 * code-bias state all present, so the flag forces them on (and forces .BIA off so
 * the network bias is the sole code-bias source).  These tests pin that contract,
 * and that the flag is off by default (the clock+phase-bias-only V1 network is
 * unchanged).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <spdlog/sinks/ostream_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// The derived closed-loop consumer config consumes the OSB and does not estimate it:
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

// Default network config (no estimate_sat_code_bias): code bias is deferred and
// .BIA stays on, so the clock+phase-bias-only V1 network is byte-identical.
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

// bias_coverage_requires_apply: parses (default false), is not overwritten by the
// estimate_sat_code_bias coupling that forces enable_code_phase_bias off, and is
// inherited by the derived closed-loop consumer.  The toggle exists for this
// combination: the coupling disables .BIA application while the loaded .BIA
// still reports coverage to the "already corrected?" gates (sat-bias absorber
// allocation and no-BIA code downweight).  With the toggle on, those gates
// consult applied coverage instead.
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
    // application off and the toggle survives it unchanged, so a loaded but
    // unapplied .BIA reads as uncovered.
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
// reference selection is byte-identical), and, armed alongside the W6 slice it
// modifies, survives the estimate_sat_code_bias coupling.  The toggle holds the
// per-satellite clock↔code-bias IF-pin reference pair fixed in time; it is
// meaningful only when the network estimates its own code biases, so it has to
// coexist with that flag rather than be overwritten by the coupling.
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
    // and the hold toggle survives it.  Both are true, so the emitted (δtˢ, dˢ)
    // split is estimated (W6) and held to a fixed signal basis in time.
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
// coupling (the datum it re-times exists only in W6, so it has to coexist with that
// flag rather than be overwritten).
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
    // the time-linked toggle survives it, so the datum is imposed as a time datum.
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

// code_datum_time_linked and clock_signal_basis_hold both re-target the per-satellite
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
// contributes to phase-bias emission, byte-identical), and arms independently of
// the W6 code-bias slice (it gates only which stations feed the satellite phase
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
    // Armed on its own (no code-bias slice): the toggle is honoured, so a station
    // with an unmatched receiver antenna is withheld from satellite phase-bias emission.
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
// whether the network estimates code biases).  Reason for the toggle: on the
// network path nothing solves the receiver clock before the first ambiguity
// seed, so without it a free-running receiver's whole clock offset leaks
// uniformly into every cold ambiguity seed.
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
    // Armed on its own (no code-bias slice): the toggle is honoured and does not
    // turn the W6 prerequisites on; it is orthogonal to code-bias estimation.
    {
        const fs::path p = writeTempConfig("coldclk_armed",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"cold_clock_seed\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkColdClockSeed());
        EXPECT_FALSE(cfg.estimateNetworkSatCodeBias());   // orthogonal: no coupling
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed alongside the W6 code-bias slice: both survive (the pre-seed helps the
    // cold ambiguity seeds that feed the code-bias/iono slice W6 estimates).
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
// Network-consumer profile (processing.network_consumer, RTA-7)
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

// The standalone profile applies the same couplings as the closed-loop
// derivation (applyNetworkConsumerCouplings is the shared source of truth).
// Full mode (network clock consumed), undifferenced user model.
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

// Bias-only mode (precise clock kept, network bias consumed): the
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
// the standalone processing.ar_ignore_mw_wl key (the config key for the
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

    // Defaults: profile off, tri-state unset, keys false/empty (the off path).
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

// The profile arms estimation.estimate_ifb when consuming network code biases:
// a standalone consumer has no producer config to inherit it from, and without
// receiver code-bias states nothing absorbs a code-bias datum re-reference.  On
// this path the engine's one-shot A5 warning therefore does not apply.
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

// Key parsing: network.product_flush_per_epoch and
// realtime.network_products_path land in their config structs (RTA-6).
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
// orbit_source profile field + per-stream corrections_accept ingest key (RTA-8)
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

// processing.network_consumer.orbit_clock_max_skew_s defaults to 0 (echo skew
// check and withhold off), parses a positive budget, and clamps a negative value
// back to 0 with a warning (Card 7, RTF-2).
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
// Bias-gate arming under consumption + clock stale-hold / fallback keys (RTA-9)
// ===========================================================================

// The profile arms processing.enable_ssr_bias_max_age when bias consumption is
// on and the config has no such key; an explicit false is respected and an
// explicit true is kept (RTA-9(a)).
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

// Processing keys: ssr_clock_stale_hold_factor defaults to 0 (rung off), parses
// a widening factor, and clamps (0,1] to 1 (the factor can only widen the clock
// apply window); ssr_bias_band_fallback defaults to false (RTA-9(b), RTA-13).
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

// network.service_mode arms the coherent RT-service producer toggle set (RTB-8).
// The bundle wins: an explicit non-zero numeric is kept, but an explicit false
// on an armed bool (or an explicit 0 numeric) is overridden with a warning
// (see BundleOverridesExplicitDisables below).
TEST(NetworkServiceProfile, ServiceModeArmsCoherentSet) {
    LogManager log;

    // Default (no service_mode): every armed toggle stays at its factory default
    // (the off-path byte-identity proxy, including every knob added in P2).
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
    // The knobs added in P2 stay at their defaults without service_mode.
    EXPECT_FALSE(c0.networkPerConstWithhold());
    EXPECT_FALSE(c0.networkEmitEpochStatusRecord());
    EXPECT_FALSE(c0.networkPivotVanishReturnSignal());
    EXPECT_FALSE(c0.networkStickyPivot());
    EXPECT_FALSE(c0.networkScopedHandoverResponse());
    EXPECT_DOUBLE_EQ(c0.getNetworkConfig().trace_rotate_mb, 0.0);

    // service_mode on → the full coherent producer set is armed.
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
    EXPECT_TRUE(c1.getNetworkConfig().product_flush_per_epoch);
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().health_interval_s, 30.0);
    // Additions from P2: withhold carve-out, heartbeat, datum-signalling and
    // handover controls, trace rotation (keep stays at its parsed default 3).
    EXPECT_TRUE(c1.networkPerConstWithhold());
    EXPECT_TRUE(c1.networkEmitEpochStatusRecord());
    EXPECT_TRUE(c1.networkPivotVanishReturnSignal());
    EXPECT_TRUE(c1.networkStickyPivot());
    EXPECT_TRUE(c1.networkScopedHandoverResponse());
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().trace_rotate_mb, 256.0);
    EXPECT_EQ(c1.getNetworkConfig().trace_rotate_keep, 3);
    // Not armed: user-path / policy numerics.
    EXPECT_TRUE(c1.getNetworkConfig().state_snapshot_path.empty());
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().warmup_ura_floor_m, 0.0);
    EXPECT_FALSE(c1.networkClockRunawayCheck());

    // An explicit non-zero numeric wins over the profile default.
    const fs::path p2 = writeTempConfig("svc_override",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"contract_rebroadcast_s\": 10.0,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.networkContractRebroadcastS(), 10.0);    // explicit kept
    EXPECT_TRUE(c2.networkEmitUra());                            // rest still armed

    std::error_code ec2; fs::remove(p0, ec2); fs::remove(p1, ec2); fs::remove(p2, ec2);
}

// network.emit_clock_rate: the decoupled C1 (clock-rate) emission toggle round-trips,
// is off by default (byte-identical), and is not armed by service_mode (it is the
// alternative to clock_model, which service_mode arms instead).  It emits the rate
// without carrying it; clock_model's two-state carry is a no-go (see
// docs/CLOCK_HADAMARD_CALIB).
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

    // service_mode does not arm emit_clock_rate (clock_model carries C1 under the bundle).
    const fs::path psvc = writeTempConfig("emit_rate_svc",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig csvc;
    ASSERT_TRUE(csvc.loadFromJson(psvc, log));
    EXPECT_FALSE(csvc.networkEmitClockRate());
    EXPECT_TRUE(csvc.networkClockModel());

    std::error_code ec; fs::remove(pdef, ec); fs::remove(pon, ec); fs::remove(psvc, ec);
}

// The bundle wins: an explicit `false` on an armed bool, an explicit 0 numeric,
// and a negative numeric (clamped to 0 at parse) are all overridden by the
// bundle.  Each such override emits one warning at load.  This test checks the
// resulting config only; it does not capture the warning (the
// loadCapturingConfigLog helper further down does that for other tests).
// (RTB-8/P1)
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

    // Explicit negative numeric: clamped to 0 at parse, then re-armed by the
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

// The pathological-withhold clamp (R11/R1) runs at parse, before the arming
// block, so it does not see a service-armed withhold of 2×trigger.  With a small
// explicit trigger that withhold is 2×0.4 = 0.8 <= 1, and since σ̂0 is floored at
// 1 it would withhold nearly every warm epoch.  The clamp re-runs after arming:
// the withhold comes out disabled (0), not armed at <= 1, and the rest of the
// bundle stays armed.  (RTB-8/P3(a) regression)
TEST(NetworkServiceProfile, ServiceArmedPathologicalWithholdIsReclamped) {
    LogManager log;

    const fs::path p1 = writeTempConfig("svc_small_trigger",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"service_mode\": true,\n"
                      "    \"raim_sigma0_trigger\": 0.4,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_DOUBLE_EQ(c1.networkRaimWithholdSigma0(), 0.0);       // re-clamped, not 0.8
    EXPECT_TRUE(c1.networkRaimWithinEpoch());                    // rest of the bundle unaffected
    EXPECT_TRUE(c1.networkRaim());

    // The non-service parse-site clamp is unchanged (idempotency anchor: an
    // explicit pathological ceiling is disabled at parse, and the post-arming
    // re-run then sees an already-valid 0 and does not fire again).
    const fs::path p2 = writeTempConfig("nosvc_patho_withhold",
        networkConfig(kGpsGal, kPriosGps,
                      "    \"raim_withhold_sigma0\": 0.5,\n"));
    GnssConfig c2;
    ASSERT_TRUE(c2.loadFromJson(p2, log));
    EXPECT_DOUBLE_EQ(c2.networkRaimWithholdSigma0(), 0.0);       // parse-site clamp
    EXPECT_FALSE(c2.networkRaimWithinEpoch());                   // clamped before the coupling

    // P3(b) companion: a stray invalid clock_model_q2 with clock_model unset is
    // not validated at parse (the restore gates on the pre-arming flag); the
    // service bundle arms clock_model, so the post-arming re-run restores it.
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

// service_mode with an empty network.product_output_path parses: the load emits
// one coherence warning listing the inert file-side knobs (not captured here),
// and the armed set is unchanged (the knobs stay armed and have no file to write
// to until a path is configured).  (RTB-8/P4(b))
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
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);     // still armed

    std::error_code ec; fs::remove(p1, ec);
}

// The service-observability knobs parse with the sibling-convention clamps:
// health_interval_s < 0 → 0 (heartbeat off), trace_rotate_mb < 0 → 0 (rotation
// off), trace_rotate_keep < 1 → 1 (at least one rolled generation).  Defaults:
// 0 / 0 / 3 (heartbeat and rotation off ⇒ byte-identical).  (RTB-6 P4(d) + P1)
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

    // Clamps (each logs a warning; the load still succeeds).
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
// Contract identity keys + SIF roster coordinates + ARP blunder gate (RTA-12)
// ===========================================================================

#ifndef GNSS_TEST_DATA_DIR
#define GNSS_TEST_DATA_DIR ""
#endif

// network.{frame_id,coord_epoch,use_sif_coordinates,coord_age_warn_years},
// the processing.network_consumer.expect_{frame_id,atx_id,orbit_ref} pins and
// realtime.arp_blunder_gate_m parse; everything defaults empty/off (the
// byte-identical off path).
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

// Network config with a station that carries no position_ecef (splice hooks as
// networkConfigRt), for the SIF-roster scenarios (RTA-12).
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

// The pre-RTA-12 hard error is unchanged when use_sif_coordinates is off (the
// default): a roster station without position_ecef fails the load.  With the
// toggle on but no SIF registry loaded, the load still fails (the relaxation
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
    // ...but not the un-propagated reference coordinate: the 4.35-year velocity
    // propagation (~ -0.0286 m/yr in X alone) has to be applied.
    EXPECT_GT(std::abs(sts[1].position_ecef[0] - (-2491490.58952111)), 0.05);
    // coord_epoch defaulted to the propagation date (decimal year 2024.35...).
    EXPECT_EQ(c1.getNetworkConfig().coord_epoch.rfind("2024.35", 0), 0u);

    // Unknown station: the load fails rather than running without coordinates.
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
// realtime.use_precise_orbits: the "ULT orbits + network clocks" pin (RTB-4)
// ===========================================================================

// The key parses, defaults off, and splits the orbit gate from the clock gate
// as documented: with the default the two gates are equal (every
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
    EXPECT_FALSE(c2.realtimeUsesLiveOrbitSource());        // SP3 orbits are loaded
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
// network.apriori_source + network.apriori_clock_anchor_sigma_m (RTF-1 Card 1)
// ===========================================================================

// Default network config: the a-priori source is Sp3 (precise SP3/CLK identity,
// the byte-identical behaviour) and the anchor sigma is 0.0 (auto).
TEST(NetworkAprioriSourceConfig, DefaultsToSp3AutoAnchor) {
    LogManager log;
    const fs::path p = writeTempConfig("apri_default", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
    EXPECT_DOUBLE_EQ(cfg.networkAprioriClockAnchorSigmaM(), 0.0);
    std::error_code ec; fs::remove(p, ec);
}

// "brdc_rts" parses to BrdcRts (case-insensitive) and, on its own with the
// clock+phase-bias-only V1 slice (no code bias), loads without error.  The
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

// Validation 1 (hard error): brdc_rts + estimate_sat_code_bias without
// estimate_iono_sh.  The external GIM iono constraint is post-processing-only,
// so the real-time code-bias slice would have no iono constraint; the load fails.
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

// Validation 1 legal path: brdc_rts + estimate_iono_sh (which forces the code
// bias slice on) is the supported real-time W6 combination; it loads with both
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

// service_mode does not arm apriori_source (an input-source selection, orthogonal
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
// resolveNetworkAprioriClockAnchorSigmaM(): the mode-aware auto resolution the
// producer folds.  Pure config function; the driver's < 100 m binding gate is
// not applied here.  (RTF-1 Card 5, plan D6)
// ---------------------------------------------------------------------------

// sp3 + auto (default): resolves to initial_std.sat_clock unchanged, which is
// the byte-identity guarantee.  The loose default (>100 m) keeps the driver
// anchor off, so every existing sp3 network config is unaffected.
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
// driver anchor binds), not the loose initial_std.sat_clock.
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

// An explicit positive sigma wins in both modes (overrides the auto), sp3 case:
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

// An explicit positive sigma wins in both modes, brdc_rts case: 5 m overrides the
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

// network.clock_datum_broadcast_anchored defaults off (byte-identical).  The
// datum soft-anchors each satellite clock to its own a-priori, so the a-priori
// in force decides what the toggle means:
//   - brdc_rts: the target is the broadcast clock and the auto sigma resolves to
//     the binding 3.0 m broadcast grade -> legal with no extra keys;
//   - sp3: the target is the precise product clock, i.e. the external-clock hold
//     rung of the external-constraint ladder.  Legal only with an explicit
//     sigma: the sp3 auto resolution is initial_std.sat_clock (default 1000 m),
//     which is not a binding anchor, so an omitted key there is always a mistake
//     and the load fails rather than running with an unrealized clock gauge.
TEST(NetworkClockDatumConfig, BroadcastAnchoredDefaultsOffAndSigmaPolicyByAprioriSource) {
    LogManager log;

    // Default: key absent -> false (the off path: no anchors folded).
    {
        const fs::path p = writeTempConfig("ba_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkClockDatumBroadcastAnchored());
        std::error_code ec; fs::remove(p, ec);
    }
    // sp3 (default a-priori) with no explicit sigma: hard error.  The auto value
    // is initial_std.sat_clock = 1000 m, far past the binding gate.
    {
        const fs::path p = writeTempConfig("ba_sp3_auto",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    // sp3 + an explicit binding sigma: the external-clock hold posture.  Loads,
    // and the resolved sigma is the explicit value (an explicit sigma wins in
    // both modes).
    {
        const fs::path p = writeTempConfig("ba_sp3_explicit",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_datum_broadcast_anchored\": true,\n"
                          "    \"apriori_clock_anchor_sigma_m\": 0.02,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkClockDatumBroadcastAnchored());
        EXPECT_EQ(cfg.networkAprioriSource(), NetworkAprioriSource::Sp3);
        EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(), 0.02);
        std::error_code ec; fs::remove(p, ec);
    }
    // sp3 + an explicit non-binding sigma still fails: the explicit-value gate
    // and the < 100 m binding gate are independent, and both must pass.
    {
        const fs::path p = writeTempConfig("ba_sp3_weak",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"clock_datum_broadcast_anchored\": true,\n"
                          "    \"apriori_clock_anchor_sigma_m\": 150.0,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    // Legal combination: brdc_rts a-priori (whose auto anchor sigma resolves to
    // the binding 3.0 m broadcast-grade default) -> loads with no sigma key.
    {
        const fs::path p = writeTempConfig("ba_brdc",
            networkConfig(kGpsGal, kPriosGps,
                          "    \"apriori_source\": \"brdc_rts\",\n"
                          "    \"clock_datum_broadcast_anchored\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkClockDatumBroadcastAnchored());
        EXPECT_DOUBLE_EQ(cfg.resolveNetworkAprioriClockAnchorSigmaM(), 3.0);
        std::error_code ec; fs::remove(p, ec);
    }
}

// Only one clock-gauge realization may be selected: sum-to-zero and the
// broadcast anchors both close the same rank deficiency, so together they
// over-constrain, and the rank gate cannot detect it (both look valid to it).
// The anchors are the gauge: a non-binding explicit sigma (>= 100 m) leaves the
// clock common-mode eigenvalue at or below the rank-gate cutoff, an undefined
// datum.  Both misconfigurations fail the load.
TEST(NetworkClockDatumConfig, BroadcastAnchoredRejectsSumToZeroAndNonBindingSigma) {
    LogManager log;

    // Both clock gauges armed (brdc_rts satisfied so the mutual exclusion is
    // the only failing constraint): hard error.
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
// re-reference", which holds only for the sum-to-zero gauge (the pivot pin
// holds while the pivot is up; the broadcast anchors never move).  Armed under
// any other datum it would cause spurious consumer resets, the churn class
// (defect 3) this trigger exists to signal, so the load fails.  With s2z it
// loads and auto-arms network.emit_clock_disc: the set-change trigger reaches
// the wire only through the clock discontinuity channel.
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
    // auto-armed so the set-change counter bump travels on the wire.
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

// network.code_bias_disc_jump_test (emitter-side value-jump disc trigger for
// weak-separation code-bias wander) and estimation.clock_disc_isb_reinflate_std
// (consumer-side ISB covariance reinflation on a clock disc bump) parse with
// inert defaults.  Both are default-off additions on the producer-wire-defect
// path and leave every existing config unchanged (the off byte-identity proxy).
TEST(NetworkDiscSignalConfig, JumpTestAndIsbReinflateParseWithInertDefaults) {
    LogManager log;

    // Defaults: false / 0.0 (nothing armed, nothing reinflated).
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
    // network.estimate_sat_code_bias is inert (warning only; no code biases are
    // emitted), and the load still succeeds.
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
// derived config when network products are consumed and the config has no such
// key.  Bias consumption without the staleness gate applies arbitrarily stale
// biases across producer withhold gaps (the A5/RTA-9(a) defect class that the
// standalone profile guards; this is its closed-loop counterpart).
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

    // Explicit false in the producer JSON is respected: the derived config stays off.
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

    // Consumption off (the A/B baseline consumer: precise SP3/CLK + .BIA only):
    // nothing is consumed, so there is nothing to age-gate and nothing is armed.
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

// network.phase_datum_eq34 parses off by default, on in isolation, and with the
// two sub-knobs (preferred datum receiver, pin-to-round).  (Card 3)
TEST(CodeBiasNetworkConfig, Eq34PhaseDatumParses) {
    LogManager log;
    // Default off, byte-identical.
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
    // On in isolation, with sub-knobs.
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

// Mutual exclusion: eq34 + ensemble are two realizations of the same gauge, so
// the load fails.  eq34 + prioritized is allowed (eq34 supersedes).  (Card 3)
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

// network.carry_invariant_innov_gate parses off by default (byte-identical), on with
// carry_invariant (which itself needs eq34), and reads the k knob.  (Card 4 (M1) D5)
TEST(CodeBiasNetworkConfig, CarryInvariantInnovGateParses) {
    LogManager log;
    // Default off, k defaults to 4.0.
    {
        const fs::path p = writeTempConfig("d5_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovGate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovK(), 4.0);
        std::error_code ec; fs::remove(p, ec);
    }
    // On atop carry_invariant + eq34, with a custom k.
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

// The innovation gate is defined only on the carried DDs, so arming it without
// carry_invariant is an incoherent config and fails the load.  (Card 4 (M1) D5)
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

// The LS-VCE calibration parses off by default (byte-identical), on atop the innovation
// gate with a window, and is rejected without the gate (nothing to calibrate).
// (Card 4 (M1) D5 v2)
TEST(CodeBiasNetworkConfig, CarryInvariantInnovCalibrateParsesAndRequiresGate) {
    LogManager log;
    // Default off, window 60.
    {
        const fs::path p = writeTempConfig("d5v2_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovCalibrate());
        EXPECT_DOUBLE_EQ(cfg.networkCarryInvariantInnovCalibrateWindow(), 60.0);
        std::error_code ec; fs::remove(p, ec);
    }
    // On atop carry_invariant + eq34 + gate, with a custom window.
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
    // Calibrate without the gate → the load fails.
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

// network.carry_invariant_innov_retap_only parses off by default (byte-identical), on
// atop the innovation gate, and, unlike calibrate (a hard load rejection), is forced off
// with a warning when the gate is absent.  The asymmetry is deliberate: calibrate changes
// the gate's threshold (an unannounced different gate gives a wrong answer), while
// retap_only only decides what happens to flags that do not exist without the gate.  It
// has the same fail-safe shape as the v3 corroboration knob beside it.
// (Card 4 (M1) D5 v4)
TEST(CodeBiasNetworkConfig, CarryInvariantInnovRetapOnlyParsesAndNeedsGate) {
    LogManager log;
    // Default off.
    {
        const fs::path p = writeTempConfig("d5v4_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovRetapOnly());
        std::error_code ec; fs::remove(p, ec);
    }
    // On atop carry_invariant + eq34 + gate.
    {
        const fs::path p = writeTempConfig("d5v4_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_innov_gate\": true,\n"
                "    \"carry_invariant_innov_retap_only\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariantInnovRetapOnly());
        std::error_code ec; fs::remove(p, ec);
    }
    // Without the gate the load succeeds but the knob is forced off (inert, not permissive).
    {
        const fs::path p = writeTempConfig("d5v4_no_gate",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_innov_retap_only\": true,\n"));   // no gate
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantInnovRetapOnly())
            << "retap_only without the innovation gate must be forced OFF, not left armed.";
        std::error_code ec; fs::remove(p, ec);
    }
}

// network.phase_datum_eq34_slip_aware_election keeps a slipped arc out of the eq34
// spanning tree (a slipped tree edge refloats every fundamental cycle through it; a
// slipped chord costs one DD).  Default off, on atop phase_datum_eq34, and forced off
// without it, since there is then no spanning tree.  (Card 3/4)
TEST(CodeBiasNetworkConfig, PhaseDatumEq34SlipAwareElectionParsesAndNeedsEq34) {
    LogManager log;
    {
        const fs::path p = writeTempConfig("slipelect_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkPhaseDatumEq34SlipAwareElection());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("slipelect_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"phase_datum_eq34_slip_aware_election\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkPhaseDatumEq34SlipAwareElection());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("slipelect_no_eq34",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34_slip_aware_election\": true,\n"));   // no eq34
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkPhaseDatumEq34SlipAwareElection())
            << "slip-aware election without phase_datum_eq34 must be forced OFF.";
        std::error_code ec; fs::remove(p, ec);
    }
}

// PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md §4): default off (the fold's
// arithmetic is byte-identical), and inert without network.carry_invariant, where there
// is no DD-prior fold to make PD-safe.  The response is a warning plus disarm, not a load
// failure, on purpose: a config that carries the knob for a later leg is not an error.
// The check lives in the cross-knob validation pass, so this test is also the regression
// test for loader ordering (a check placed in the parse block would read
// network_carry_invariant_ before it is parsed and disarm every run).
TEST(CodeBiasNetworkConfig, CarryInvariantPdSafeContract) {
    LogManager log;
    // Default: absent → off.
    {
        const fs::path p = writeTempConfig("pdsafe_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantPdSafe());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed without carry_invariant → loads, but disarmed (inert, not an error).
    {
        const fs::path p = writeTempConfig("pdsafe_no_carry",
            networkConfig(kGpsGal, kPriosGps,
                "    \"carry_invariant_pd_safe\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryInvariantPdSafe())
            << "without carry_invariant there is no DD-prior fold — the knob must disarm.";
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed with carry_invariant (+eq34) → stays armed.  This is the loader-ordering
    // regression: the knob is parsed in loadNetworkDatumSignalling_ and its prerequisite
    // test runs later, in the cross-knob pass, where every flag is settled.
    {
        const fs::path p = writeTempConfig("pdsafe_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_pd_safe\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariant());
        EXPECT_TRUE(cfg.networkCarryInvariantPdSafe());
        std::error_code ec; fs::remove(p, ec);
    }
}

// Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md §6): default off
// (every readback expression is the pre-card one, verbatim) and inert without
// network.carry_invariant (with no folded DD priors the rank-K correction is identically
// zero).  The prerequisite check lives in the cross-knob pass for the same loader-ordering
// reason as the PD-safe knob above.  Unlike that knob, this one is not fingerprinted: its
// consumers (dd_sigma_cyc, the log-only AR probe) feed nothing back, so no stored state
// changes meaning when it flips.
TEST(CodeBiasNetworkConfig, CarryAwareCovarianceContract) {
    LogManager log;
    // Default: absent → off.
    {
        const fs::path p = writeTempConfig("cacov_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryAwareCovariance());
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed without carry_invariant → loads, but disarmed (inert, not an error).
    {
        const fs::path p = writeTempConfig("cacov_no_carry",
            networkConfig(kGpsGal, kPriosGps,
                "    \"carry_aware_covariance\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCarryAwareCovariance())
            << "without carry_invariant there are no folded DD priors — the correction is "
               "identically zero, so the knob must disarm rather than pretend to arm.";
        std::error_code ec; fs::remove(p, ec);
    }
    // Armed with carry_invariant (+eq34) → stays armed.
    {
        const fs::path p = writeTempConfig("cacov_armed",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_aware_covariance\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariant());
        EXPECT_TRUE(cfg.networkCarryAwareCovariance());
        std::error_code ec; fs::remove(p, ec);
    }
    // Composes with the PD-safe knob rather than depending on it: the correction reads
    // back whatever fold ran, PD-safe or legacy.
    {
        const fs::path p = writeTempConfig("cacov_with_pdsafe",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_datum_eq34\": true,\n"
                "    \"carry_invariant\": true,\n"
                "    \"carry_invariant_pd_safe\": true,\n"
                "    \"carry_aware_covariance\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkCarryInvariantPdSafe());
        EXPECT_TRUE(cfg.networkCarryAwareCovariance());
        std::error_code ec; fs::remove(p, ec);
    }
}

// Per-sample bridge iono de-bias (the 60-ep age-bias fix): defaults off
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

// ===========================================================================
// network.external_sat_code_bias (EXT-CB): the satellite code and phase biases
// come from the external .BIA instead of being estimated (the third satellite-
// bias posture, alongside V1 slant-absorption and W6 own-OSB)
// ===========================================================================

namespace {

// A network config that can also carry estimation / model_corrections blocks.
// The EXT-CB contract spans all three sections: it suppresses a network force
// chain whose observable effect is on model_corrections.enable_code_phase_bias
// and estimation.estimate_ifb.
std::string extCbConfig(const std::string& network_extra,
                        const std::string& estimation_block = "",
                        const std::string& model_corr_block = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": " + kGpsGal + ",\n"
        "    \"signal_priorities\": " + kPriosGps + "\n"
        "  },\n"
        + estimation_block + model_corr_block +
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

// estimate_iono_sh force-chains estimate_sat_code_bias on, which force-sets
// enable_code_phase_bias off, so without EXT-CB "SH-VTEC ionosphere with external
// satellite biases" (the classical known-DCB global-iono setup) is unreachable.
// Arming external_sat_code_bias suppresses that force and nothing else about the
// SH slice.
TEST(NetworkExternalSatCodeBiasConfig, SuppressesTheShToW6ForceChain) {
    LogManager log;

    // Default: absent -> false, and nothing about the existing postures moves.
    {
        const fs::path p = writeTempConfig("extcb_default", extCbConfig(""));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkExternalSatCodeBias());
        std::error_code ec; fs::remove(p, ec);
    }
    // Control: estimate_iono_sh alone still force-chains W6 and still turns the
    // .BIA off.  EXT-CB leaves this behaviour untouched.
    {
        const fs::path p = writeTempConfig("extcb_control",
            extCbConfig("    \"estimate_iono_sh\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.estimateNetworkIonoSh());
        EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());
        EXPECT_FALSE(cfg.getModelCorrections().enable_code_phase_bias);
        std::error_code ec; fs::remove(p, ec);
    }
    // EXT-CB + SH: the SH field stays co-estimated, the W6 slice is not armed,
    // the .BIA stays applied, and the receiver DCB is forced on (trap #4: on
    // undifferenced rows the per-(rx,constellation) GF code lump needs an
    // explicit state, or it is absorbed into the slant iono and conflicts with
    // the field).
    {
        const fs::path p = writeTempConfig("extcb_sh",
            extCbConfig("    \"external_sat_code_bias\": true,\n"
                        "    \"estimate_iono_sh\": true,\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkExternalSatCodeBias());
        EXPECT_TRUE(cfg.estimateNetworkIonoSh());
        EXPECT_FALSE(cfg.estimateNetworkSatCodeBias())
            << "EXT-CB must suppress the estimate_iono_sh -> W6 force chain.";
        EXPECT_TRUE(cfg.getModelCorrections().enable_code_phase_bias)
            << "the .BIA is the satellite bias SOURCE in this posture.";
        EXPECT_TRUE(cfg.estimateIfb());
        EXPECT_FALSE(cfg.estimateSatBiases())
            << "no satellite code-bias state — the product supplies it.";
        std::error_code ec; fs::remove(p, ec);
    }
}

// Two satellite code-bias sources cannot both be selected: W6 estimates its own
// OSB and skips the .BIA entirely, EXT-CB consumes the .BIA and creates no
// state.  Arming both would ask for a state on rows the product already
// corrected.  An explicit enable_code_phase_bias=false under EXT-CB leaves the
// satellite code bias neither applied nor estimated (the V1 slant-absorption
// posture under EXT-CB's name).  Both configurations fail the load.
TEST(NetworkExternalSatCodeBiasConfig, RejectsW6AndAnExplicitBiaOff) {
    LogManager log;

    {
        const fs::path p = writeTempConfig("extcb_w6",
            extCbConfig("    \"external_sat_code_bias\": true,\n"
                        "    \"estimate_sat_code_bias\": true,\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("extcb_nobia",
            extCbConfig("    \"external_sat_code_bias\": true,\n",
                        /*estimation_block=*/"",
                        "  \"model_corrections\": { \"enable_code_phase_bias\": false },\n"));
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
    }
}

// The .BIA is a daily post-processing bias product, and the self-sufficient
// real-time service does not have one (the same reasoning that makes the
// external-GIM code-bias path post-processing-only).  A real-time producer
// takes its satellite biases from an SSR stream, a different mechanism with its
// own age/continuity semantics.
TEST(NetworkExternalSatCodeBiasConfig, RejectsBrdcRtsApriori) {
    LogManager log;
    const fs::path p = writeTempConfig("extcb_brdc",
        extCbConfig("    \"external_sat_code_bias\": true,\n"
                    "    \"estimate_iono_sh\": true,\n"
                    "    \"apriori_source\": \"brdc_rts\",\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// The invariant DD carry's event response (standing invariant #1, Card 4 (M1)).
// ---------------------------------------------------------------------------
// The carried z is stiff (Φ = I, and q_z = 0 by default ⇒ a random constant), so
// invariant #1 requires a wired erase/refloat/re-tap before an on field run.  For
// this carry that means a pre-fit raw detector (network.cycle_slip.mw_detect /
// .if_phase_td, which drain into carry_slipped_ambs) or the post-fit D5 gate
// (network.carry_invariant_innov_gate).  The interlock only warns and changes no
// state, so the solve stays byte-identical and the interlock can be asserted only
// through the log.  These tests attach an in-memory sink to the loader's own
// "gnss_config" logger for that.
namespace {

/// Result of a load performed with the loader's warning stream captured.
struct CapturedLoad {
    bool        ok{false};   ///< What loadFromJson returned.
    std::string text;        ///< Everything the "gnss_config" logger emitted.
    GnssConfig  cfg;         ///< The loaded config, for state-change assertions.
};

/// Load @p p, capturing the "gnss_config" logger into a string.
///
/// getLogger() before initialize() returns a transient console-only logger
/// (LogManager.hpp:110-126), so a sink attached to it would not be the one the
/// loader writes through; the manager is initialized first.
/// @p tag must be unique per call: initialize() opens gnss_config.log in @p dir, and
/// re-initializing into a directory whose previous handle is still open throws on
/// Windows, so each load gets its own directory.  The manager is not shut down
/// (shutdown() calls spdlog::drop_all(); see below); the helper drops the two
/// loggers it created instead.
CapturedLoad loadCapturingConfigLog(const fs::path& p, const std::string& tag) {
    CapturedLoad out;
    LogManager log;
    const fs::path dir = fs::temp_directory_path() / ("codebias_net_cfg_caplog_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // Targeted registry cleanup.  Another test in the same binary may have left a
    // "gnss_processor" logger registered, and initialize() throws on the duplicate
    // ("logger with name 'gnss_processor' already exists"); without this drop these
    // tests pass in isolation and fail in the full suite.
    //
    // Do not use spdlog::drop_all() (or LogManager::shutdown(), which calls it): it
    // removes loggers other live objects still hold, and doing so faults
    // RtcmParserDecodeTest with 0xC0000005 (the dangling-logger defect class that
    // CLAUDE.md records from the VS 2026 migration).  Only the two names this helper
    // is about to create are dropped, here and again on the way out.
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);          // warnings pass this level
    auto cfg_log = log.getLogger("gnss_config");
    std::ostringstream oss;
    cfg_log->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(oss));
    out.ok = out.cfg.loadFromJson(p, log);
    cfg_log->flush();
    out.text = oss.str();                          // capture before dropping the logger
    // Leave the registry without these two names; other loggers are not touched.
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    return out;
}

/// The invariant-#1 "no response at all" warning's stable identifying phrase.
constexpr const char* kNoResponse = "NO event response is wired";
/// The softer "pre-fit detector only, on a q_z = 0 carry" advisory's phrase.
constexpr const char* kDetectorOnly = "ONLY event response on the carry is the pre-fit";

} // namespace

// carry_invariant with neither a pre-fit detector nor the D5 gate is the posture
// invariant #1 forbids.  The loader warns and does not arm anything in response,
// because auto-arming a detector here would change the solve.
TEST(CodeBiasNetworkConfig, CarryInvariantWithoutEventResponseWarnsAndChangesNothing) {
    const fs::path p = writeTempConfig("ci_inv1_none",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "ci_inv1_none");
    ASSERT_TRUE(r.ok) << "the interlock is advisory — it must not fail the load.";
    EXPECT_NE(r.text.find(kNoResponse), std::string::npos)
        << "carry_invariant with no detector and no D5 gate must warn under invariant #1.\n"
        << r.text;
    // Byte-identity: the warning is the only effect.  Nothing is armed.
    EXPECT_FALSE(r.cfg.networkCycleSlipMwDetect());
    EXPECT_FALSE(r.cfg.networkCycleSlipIfPhaseTd());
    EXPECT_FALSE(r.cfg.networkCarryInvariantInnovGate());
    EXPECT_DOUBLE_EQ(r.cfg.networkCarryInvariantQz(), 0.0);
    EXPECT_TRUE(r.cfg.networkCarryInvariant()) << "carry_invariant itself must stay armed.";
    std::error_code ec; fs::remove(p, ec);
}

// A pre-fit detector is an event response (it feeds carry_slipped_ambs from epoch 1,
// gated on carry_invariant alone), so the kNoResponse warning does not fire.  The
// softer q_z=0 advisory still does: detected slips are covered, model error is not.
TEST(CodeBiasNetworkConfig, CarryInvariantWithPrefitDetectorDropsToTheSofterAdvisory) {
    const fs::path p = writeTempConfig("ci_inv1_mw",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"
            "    \"cycle_slip\": { \"mw_detect\": true },\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "ci_inv1_mw");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.cfg.networkCycleSlipMwDetect()) << "fixture must actually arm mw_detect.";
    EXPECT_EQ(r.text.find(kNoResponse), std::string::npos)
        << "a pre-fit detector satisfies invariant #1; the loud warning must not fire.\n"
        << r.text;
    EXPECT_NE(r.text.find(kDetectorOnly), std::string::npos)
        << "q_z=0 with the D5 gate off is still detector-only and must be flagged.\n"
        << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// The D5 innovation gate is the response that fires on model error rather than on
// a detected raw slip, so on its own it silences both tiers.
TEST(CodeBiasNetworkConfig, CarryInvariantWithInnovGateIsQuiet) {
    const fs::path p = writeTempConfig("ci_inv1_d5",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"
            "    \"carry_invariant_innov_gate\": true,\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "ci_inv1_d5");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.cfg.networkCarryInvariantInnovGate());
    EXPECT_EQ(r.text.find(kNoResponse), std::string::npos) << r.text;
    EXPECT_EQ(r.text.find(kDetectorOnly), std::string::npos) << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// A nonzero q_z is not an event response: it inflates Pᶻ's diagonal without moving
// the carried mean, so it does not suppress the kNoResponse warning.
TEST(CodeBiasNetworkConfig, CarryInvariantNonzeroQzIsNotAnEventResponse) {
    const fs::path p = writeTempConfig("ci_inv1_qz",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"
            "    \"carry_invariant_qz\": 1e-4,\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "ci_inv1_qz");
    ASSERT_TRUE(r.ok);
    ASSERT_DOUBLE_EQ(r.cfg.networkCarryInvariantQz(), 1e-4) << "fixture must actually set q_z.";
    EXPECT_NE(r.text.find(kNoResponse), std::string::npos)
        << "q_z > 0 is decay, not erase/refloat/re-tap; the warning must still fire.\n"
        << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// turbo_edit satisfies the phase-leveling invariant-#1 auto-arm, but under
// carry_invariant it takes the repair arm instead of collecting MW refloat
// candidates and repairs only the superseded leveling local carry, so it feeds
// nothing into carry_slipped_ambs.  It does not silence this warning.
TEST(CodeBiasNetworkConfig, CarryInvariantTurboEditAloneStillWarns) {
    const fs::path p = writeTempConfig("ci_inv1_turbo",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"
            "    \"cycle_slip\": { \"turbo_edit\": true },\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "ci_inv1_turbo");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.cfg.networkCycleSlipTurboEdit()) << "fixture must actually arm turbo_edit.";
    if (r.cfg.networkCycleSlipMwDetect() || r.cfg.networkCycleSlipIfPhaseTd())
        GTEST_SKIP() << "turbo_edit auto-armed a raw detector; the trap does not exist.";
    EXPECT_NE(r.text.find(kNoResponse), std::string::npos)
        << "turbo_edit contributes nothing to carry_slipped_ambs under carry_invariant.\n"
        << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// network.phase_leveling_los_scale (F1; docs/SPEC_amb_iono_split_overcount.md).
//
// The knob multiplies each carried leveling-ambiguity row's variance by B_eff (the
// carried columns injected on the same station-satellite LOS this epoch) so the
// B rows of one LOS assert one unit of iono-direction precision instead of B.
// Solve-affecting when on, byte-identical when off.  The loader's contract has three
// parts, each visible only through the "gnss_config" log:
//   1. the default is off, and an off lineage prints neither line;
//   2. on under a live leveling carry, it prints the armed info line (kLosScaleArmed);
//   3. on with no leveling carry to scale (leveling off, or carry_invariant
//      superseding the leveling ambiguity carry so `do_amb_carry` is false), it
//      warns as inert (kLosScaleInert) and is not disarmed (standing invariant #4:
//      the run-end 'Network (leveling LOS-scale F1)' counter line is the on-path
//      telemetry; this warning is its pre-run counterpart).
// The B_eff arithmetic itself is pinned at the row-build seam in
// test_network_coupling.cpp (LosScaledVarianceWeighsBandsAtOneThirdAndOffIsLegacyExact).
// ---------------------------------------------------------------------------
namespace {

/// Stable phrase of the armed info line (GnssConfigLoadNetwork.cpp, emitted inside
/// the `if (network_phase_leveling_)` post-parse block).
constexpr const char* kLosScaleArmed = "per-LOS variance scale ARMED";
/// Stable phrase of the inert warning (the post-parse guard that needs
/// network_carry_invariant_ populated; fires on !phase_leveling || carry_invariant).
constexpr const char* kLosScaleInert =
    "network.phase_leveling_los_scale is on but the phase-leveling";

} // namespace

// Default off, and silent, both on a bare network block and on a leveling lineage
// that never wrote the key (the byte-identity posture of every pre-F1 leveling run).
TEST(NetworkPhaseLevelingConfig, LosScaleDefaultsOffAndIsSilent) {
    std::error_code ec;
    {
        const fs::path p = writeTempConfig("pl_los_default",
            networkConfig(kGpsGal, kPriosGps, ""));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_los_default");
        ASSERT_TRUE(r.ok);
        EXPECT_FALSE(r.cfg.networkPhaseLeveling());
        EXPECT_FALSE(r.cfg.networkPhaseLevelingLosScale());
        EXPECT_EQ(r.text.find(kLosScaleArmed), std::string::npos) << r.text;
        EXPECT_EQ(r.text.find(kLosScaleInert), std::string::npos) << r.text;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_los_default_pl",
            networkConfig(kGpsGal, kPriosGps, "    \"phase_leveling\": true,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_los_default_pl");
        ASSERT_TRUE(r.ok);
        ASSERT_TRUE(r.cfg.networkPhaseLeveling());
        EXPECT_FALSE(r.cfg.networkPhaseLevelingLosScale())
            << "leveling ON must not imply the scale — F1 is an explicit arm.";
        EXPECT_EQ(r.text.find(kLosScaleArmed), std::string::npos) << r.text;
        EXPECT_EQ(r.text.find(kLosScaleInert), std::string::npos) << r.text;
        fs::remove(p, ec);
    }
}

// On under a live leveling carry: parses true, prints the armed line, no inert
// warning, and moves no sibling leveling knob (the scale is applied at inject and
// only loosens).
TEST(NetworkPhaseLevelingConfig, LosScaleParsesTrueAndAnnouncesArmedUnderLeveling) {
    std::error_code ec;
    const fs::path p_off = writeTempConfig("pl_los_armed_ref",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_leveling\": true,\n"));
    const fs::path p_on = writeTempConfig("pl_los_armed",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_leveling\": true,\n"
            "    \"phase_leveling_los_scale\": true,\n"));
    CapturedLoad ref = loadCapturingConfigLog(p_off, "pl_los_armed_ref");
    CapturedLoad r   = loadCapturingConfigLog(p_on,  "pl_los_armed");
    ASSERT_TRUE(ref.ok);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.cfg.networkPhaseLeveling());
    EXPECT_TRUE(r.cfg.networkPhaseLevelingLosScale());
    EXPECT_NE(r.text.find(kLosScaleArmed), std::string::npos)
        << "an armed F1 must announce itself on the config log.\n" << r.text;
    EXPECT_EQ(r.text.find(kLosScaleInert), std::string::npos)
        << "phase_leveling ON without carry_invariant is the live posture — not INERT.\n"
        << r.text;
    // Sibling leveling state is untouched by the arm (compare against the same
    // lineage with the key absent rather than against a hard-coded default).
    EXPECT_DOUBLE_EQ(r.cfg.networkPhaseLevelingQAmb(), ref.cfg.networkPhaseLevelingQAmb());
    EXPECT_EQ(r.cfg.networkCarryInvariant(), ref.cfg.networkCarryInvariant());
    EXPECT_EQ(r.cfg.networkCycleSlipMwDetect(), ref.cfg.networkCycleSlipMwDetect());
    fs::remove(p_off, ec);
    fs::remove(p_on, ec);
}

// On with leveling off: the inject site the scale acts on does not exist, so the
// guard warns (naming both predicates) and the armed line, which lives inside the
// phase_leveling block, does not print.  The parsed value is kept as written; the
// warning is the only effect (changing the value would be a state change that the
// byte-identity comparison cannot see).
TEST(NetworkPhaseLevelingConfig, LosScaleWithoutLevelingWarnsInertAndIsNotSilentlyDisarmed) {
    const fs::path p = writeTempConfig("pl_los_nopl",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_leveling_los_scale\": true,\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "pl_los_nopl");
    ASSERT_TRUE(r.ok) << "the guard is advisory — it must not fail the load.";
    EXPECT_FALSE(r.cfg.networkPhaseLeveling());
    EXPECT_TRUE(r.cfg.networkPhaseLevelingLosScale())
        << "the loader keeps the value as written; the warn is the mechanism.";
    EXPECT_NE(r.text.find(kLosScaleInert), std::string::npos)
        << "F1 with phase_leveling off scales nothing — it must warn INERT.\n" << r.text;
    EXPECT_NE(r.text.find("phase_leveling=false"), std::string::npos)
        << "the warn must name the predicate that made it inert.\n" << r.text;
    EXPECT_EQ(r.text.find(kLosScaleArmed), std::string::npos)
        << "the ARMED line lives inside the phase_leveling block; it must not print.\n"
        << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// On with leveling on but carry_invariant superseding the leveling ambiguity carry
// as a solve input (Card 4's DD carry; `do_amb_carry` is false, the inject is
// skipped): the same inert warning, naming carry_invariant.  carry_invariant
// is legal only on the eq34 chain datum, so the posture carries phase_datum_eq34.
TEST(NetworkPhaseLevelingConfig, LosScaleUnderCarryInvariantWarnsInert) {
    const fs::path p = writeTempConfig("pl_los_cinv",
        networkConfig(kGpsGal, kPriosGps,
            "    \"phase_leveling\": true,\n"
            "    \"phase_datum_eq34\": true,\n"
            "    \"carry_invariant\": true,\n"
            "    \"phase_leveling_los_scale\": true,\n"));
    CapturedLoad r = loadCapturingConfigLog(p, "pl_los_cinv");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.cfg.networkPhaseLeveling());
    ASSERT_TRUE(r.cfg.networkCarryInvariant()) << "fixture must actually arm carry_invariant.";
    EXPECT_TRUE(r.cfg.networkPhaseLevelingLosScale());
    EXPECT_NE(r.text.find(kLosScaleInert), std::string::npos)
        << "under carry_invariant the leveling carry is not the solve vehicle — INERT.\n"
        << r.text;
    EXPECT_NE(r.text.find("carry_invariant=true"), std::string::npos)
        << "the warn must name the predicate that made it inert.\n" << r.text;
    std::error_code ec; fs::remove(p, ec);
}

// Block admission age gate for F3 (network.phase_leveling_local_block_min_age_ep,
// SPEC_amb_iono_split_overcount §6.12): default 0 = off (byte-identical), parses,
// a negative value is refused, and setting it without the block warns as inert
// (there is no block to keep a young leg out of) rather than disarming.
TEST(NetworkPhaseLevelingConfig, BlockMinAgeDefaultsOffParsesAndWarnsInertWithoutTheBlock) {
    std::error_code ec;
    {
        const fs::path p = writeTempConfig("pl_blk_age_default",
            networkConfig(kGpsGal, kPriosGps, "    \"phase_leveling\": true,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_default");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.cfg.networkPhaseLevelingLocalBlockMinAgeEp(), 0);
        EXPECT_EQ(r.text.find("F3 age gate"), std::string::npos) << r.text;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_blk_age_on",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_leveling\": true,\n"
                "    \"shared_carry_full_cov\": true,\n"
                "    \"carry_conditional_coupling\": true,\n"
                "    \"phase_leveling_local_block\": true,\n"
                "    \"phase_leveling_local_block_min_age_ep\": 30,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_on");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.cfg.networkPhaseLevelingLocalBlockMinAgeEp(), 30);
        EXPECT_EQ(r.text.find("F3 age gate"), std::string::npos)
            << "armed under the block: no INERT warn.\n" << r.text;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_blk_age_noblock",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_leveling\": true,\n"
                "    \"phase_leveling_local_block_min_age_ep\": 30,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_noblock");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.cfg.networkPhaseLevelingLocalBlockMinAgeEp(), 30)
            << "not silently disarmed — the warn is the contract";
        EXPECT_NE(r.text.find("F3 age gate"), std::string::npos) << r.text;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_blk_age_neg",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_leveling_local_block_min_age_ep\": -1,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_neg");
        EXPECT_FALSE(r.ok) << "a negative gate must be refused";
        fs::remove(p, ec);
    }
    // The gate's response: "free" by default (young legs not injected), "diagonal" kept
    // for the record and warned as measured-harmful, anything else refused.
    {
        const fs::path p = writeTempConfig("pl_blk_age_mode_default",
            networkConfig(kGpsGal, kPriosGps, "    \"phase_leveling\": true,\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_mode_default");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.cfg.networkPhaseLevelingLocalBlockYoungMode(), "free");
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_blk_age_mode_diag",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_leveling\": true,\n"
                "    \"shared_carry_full_cov\": true,\n"
                "    \"carry_conditional_coupling\": true,\n"
                "    \"phase_leveling_local_block\": true,\n"
                "    \"phase_leveling_local_block_min_age_ep\": 30,\n"
                "    \"phase_leveling_local_block_young_mode\": \"diagonal\",\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_mode_diag");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.cfg.networkPhaseLevelingLocalBlockYoungMode(), "diagonal");
        EXPECT_NE(r.text.find("MEASURED HARMFUL"), std::string::npos) << r.text;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeTempConfig("pl_blk_age_mode_bad",
            networkConfig(kGpsGal, kPriosGps,
                "    \"phase_leveling_local_block_young_mode\": \"sideways\",\n"));
        CapturedLoad r = loadCapturingConfigLog(p, "pl_blk_age_mode_bad");
        EXPECT_FALSE(r.ok) << "an unknown mode must be refused";
        fs::remove(p, ec);
    }
}
