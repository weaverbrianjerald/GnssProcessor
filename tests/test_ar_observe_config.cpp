/**
 * @file test_ar_observe_config.cpp
 * @brief The four keys of the observe-mode integer search under @c network.closed_loop:
 *        their defaults, their derivation onto the consumer engine's config, and the load
 *        refusals and warnings.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// A producer config with the given closed_loop object body.
std::string json(const std::string& closed_loop_body) {
    return R"({
      "processing": { "mode": "ppp-rtk-network", "active_systems": ["GPS"],
                      "signal_priorities": { "GPS": "WC" } },
      "network": {
        "clock_datum_station": "AAAA",
        "closed_loop": { )" + closed_loop_body + R"( },
        "stations": [
          { "name": "AAAA", "position_ecef": [ -2493304.99, -4655214.79, 3565497.67 ] },
          { "name": "BBBB", "position_ecef": [ -2281621.75, -1453585.08, 5756964.89 ] }
        ]
      }
    })";
}

/// Result of a load with the loader's loggers captured.
struct CapturedLoad {
    bool        ok{ false };
    std::string text;
    bool logged(const std::string& s) const { return text.find(s) != std::string::npos; }
};

/// Load the config with @p closed_loop_body as its closed_loop object, capturing what the
/// loader logs (the pattern of test_active_system_admission.cpp: an initialized manager, a
/// unique directory, only the two logger names this helper creates dropped before and after).
CapturedLoad load(const std::string& closed_loop_body, GnssConfig& cfg) {
    CapturedLoad out;
    LogManager log;
    const fs::path dir = gnss_test::tempDir() / "ar_observe_config_logs";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    log.getLogger("gnss_config")->sinks().push_back(sink);
    log.getLogger()->sinks().push_back(sink);

    const fs::path path = dir / "ar_observe_config.json";
    {
        std::ofstream f(path);
        f << json(closed_loop_body);
    }
    out.ok = cfg.loadFromJson(path, log);
    log.getLogger("gnss_config")->flush();
    log.getLogger()->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    return out;
}

TEST(ArObserveConfigTest, DefaultsAreOffFullAndFivePercent) {
    GnssConfig cfg;
    ASSERT_TRUE(load("", cfg).ok);
    const ClosedLoopConfig& cl = cfg.getNetworkConfig().closed_loop;
    EXPECT_FALSE(cl.ar_observe);
    EXPECT_EQ(cl.ar_search, "full");
    EXPECT_DOUBLE_EQ(cl.ar_determined_sigma_cyc, 0.05);
    EXPECT_FALSE(cl.trace_ar_fixes);
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_FALSE(consumer.arObserve());
    EXPECT_EQ(consumer.arObserveSearch(), "full");
    EXPECT_DOUBLE_EQ(consumer.arObserveDeterminedSigmaCyc(), 0.05);
}

TEST(ArObserveConfigTest, KeysParseAndAreDerivedOntoTheConsumerOnly) {
    GnssConfig cfg;
    ASSERT_TRUE(load("\"ar_observe\": true, \"ar_search\": \"decorrelated_partial\", "
                     "\"ar_determined_sigma_cyc\": 0.08, \"trace_ar_fixes\": true", cfg).ok);
    const ClosedLoopConfig& cl = cfg.getNetworkConfig().closed_loop;
    EXPECT_TRUE(cl.ar_observe);
    EXPECT_EQ(cl.ar_search, "decorrelated_partial");
    EXPECT_DOUBLE_EQ(cl.ar_determined_sigma_cyc, 0.08);
    EXPECT_TRUE(cl.trace_ar_fixes);
    EXPECT_FALSE(cfg.arObserve()) << "the producer's own config is untouched";
    const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
    EXPECT_TRUE(consumer.arObserve());
    EXPECT_EQ(consumer.arObserveSearch(), "decorrelated_partial");
    EXPECT_DOUBLE_EQ(consumer.arObserveDeterminedSigmaCyc(), 0.08);
}

TEST(ArObserveConfigTest, TheFullSearchNameIsAccepted) {
    GnssConfig cfg;
    ASSERT_TRUE(load("\"ar_observe\": true, \"ar_search\": \"full\"", cfg).ok);
    EXPECT_EQ(cfg.deriveClosedLoopConsumerConfig().arObserveSearch(), "full");
}

TEST(ArObserveConfigTest, AnUnknownSearchNameIsALoadErrorNamingTheKey) {
    GnssConfig cfg;
    const CapturedLoad r = load("\"ar_observe\": true, \"ar_search\": \"bootstrap\"", cfg);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.logged("network.closed_loop.ar_search")) << r.text;
    EXPECT_TRUE(r.logged("bootstrap")) << r.text;
}

TEST(ArObserveConfigTest, ASearchNameIsCheckedEvenWithTheModeOff) {
    // A typo must not wait for the day the mode is switched on.
    GnssConfig cfg;
    EXPECT_FALSE(load("\"ar_search\": \"FULL\"", cfg).ok);
}

TEST(ArObserveConfigTest, ADeterminedBarThatIsNotPositiveIsALoadErrorNamingTheKey) {
    for (const char* bad : { "0.0", "-0.05" }) {
        SCOPED_TRACE(bad);
        GnssConfig cfg;
        const CapturedLoad r =
            load(std::string("\"ar_observe\": true, \"ar_determined_sigma_cyc\": ") + bad, cfg);
        EXPECT_FALSE(r.ok);
        EXPECT_TRUE(r.logged("network.closed_loop.ar_determined_sigma_cyc")) << r.text;
    }
}

TEST(ArObserveConfigTest, TracesWithoutTheSearchWarnAndStayInert) {
    GnssConfig cfg;
    const CapturedLoad r = load("\"trace_ar_fixes\": true", cfg);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.logged("network.closed_loop.trace_ar_fixes is ON but network.closed_loop.ar_observe is off"))
        << r.text;
    EXPECT_FALSE(cfg.deriveClosedLoopConsumerConfig().arObserve());
}

TEST(ArObserveConfigTest, TheSearchWithoutTheHarnessWarns) {
    GnssConfig cfg;
    const CapturedLoad r = load("\"ar_observe\": true", cfg);    // closed_loop.enable is off
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.logged("network.closed_loop.ar_observe is ON but the closed-loop harness is NOT enabled"))
        << r.text;
}

} // namespace
