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

// RTB-8: network.service_mode arms the coherent RT-service producer toggle set
// with "fill-if-default" semantics — an explicit value in the same config wins.
TEST(NetworkServiceProfile, ServiceModeArmsCoherentSet) {
    LogManager log;

    // Default (no service_mode): every armed toggle stays at its factory default.
    const fs::path p0 = writeTempConfig("svc_off", networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig c0;
    ASSERT_TRUE(c0.loadFromJson(p0, log));
    EXPECT_FALSE(c0.networkEmitClockDisc());
    EXPECT_FALSE(c0.networkEmitUra());
    EXPECT_FALSE(c0.networkClockModel());
    EXPECT_FALSE(c0.networkRaimWithinEpoch());
    EXPECT_DOUBLE_EQ(c0.networkRaimWithholdSigma0(), 0.0);
    EXPECT_FALSE(c0.networkSerializeDiscCounters());
    EXPECT_DOUBLE_EQ(c0.networkContractRebroadcastS(), 0.0);
    EXPECT_DOUBLE_EQ(c0.getNetworkConfig().health_interval_s, 0.0);
    EXPECT_FALSE(c0.getNetworkConfig().product_flush_per_epoch);

    // service_mode on → the coherent producer set is armed.
    const fs::path p1 = writeTempConfig("svc_on",
        networkConfig(kGpsGal, kPriosGps, "    \"service_mode\": true,\n"));
    GnssConfig c1;
    ASSERT_TRUE(c1.loadFromJson(p1, log));
    EXPECT_TRUE(c1.networkEmitClockDisc());
    EXPECT_TRUE(c1.networkEmitUra());
    EXPECT_TRUE(c1.networkUraRetroBump());
    EXPECT_TRUE(c1.networkClockModel());
    EXPECT_TRUE(c1.networkPhaseLeveling());                      // carry_warm prerequisite
    EXPECT_TRUE(c1.networkRaim());                               // feeds the σ̂0 EWMA → URA scaling
    EXPECT_TRUE(c1.networkRaimAttribution());                    // complementary post-fit tier
    EXPECT_TRUE(c1.networkRaimWithinEpoch());
    EXPECT_GT(c1.networkRaimWithholdSigma0(), 0.0);              // ~2x the trigger
    EXPECT_TRUE(c1.networkEmitGenerationId());
    EXPECT_TRUE(c1.networkSerializeDiscCounters());
    EXPECT_TRUE(c1.getNetworkConfig().serialize_sbasis_contract);
    EXPECT_TRUE(c1.getNetworkConfig().product_flush_per_epoch);
    EXPECT_DOUBLE_EQ(c1.networkContractRebroadcastS(), 30.0);
    EXPECT_DOUBLE_EQ(c1.getNetworkConfig().health_interval_s, 30.0);

    // Explicit value wins over the profile default (fill-if-default).
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
