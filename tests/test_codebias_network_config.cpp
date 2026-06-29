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
