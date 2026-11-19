/**
 * @file test_rx_sat_code_bias_config.cpp
 * @brief The configuration keys of the receiver-satellite code-bias family: the defaults (everything off
 *        or inert), the parse of each key, and the runs that cannot carry the family.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// A configuration loaded from @p json through the loader.
GnssConfig load(const std::string& json, const std::string& tag, bool* ok = nullptr) {
    namespace fs = std::filesystem;
    const fs::path dir = gnss_test::tempDir() / ("rscb_config_" + tag);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "config.json";
    { std::ofstream out(file); out << json; }
    LogManager log;
    GnssConfig cfg;
    const bool loaded = cfg.loadFromJson(file.string(), log);
    if (ok != nullptr) *ok = loaded;
    else EXPECT_TRUE(loaded) << tag;
    fs::remove_all(dir, ec);
    return cfg;
}

}  // namespace

TEST(RxSatCodeBiasConfigTest, EveryDefaultIsOffOrInert) {
    const GnssConfig constructed;
    EXPECT_FALSE(constructed.estimateRxSatCodeBias());
    EXPECT_EQ(constructed.getRxSatCodeBiasSystems(), (std::vector<std::string>{ "GPS" }));
    EXPECT_TRUE(constructed.getRxSatCodeBiasStore().empty());
    EXPECT_DOUBLE_EQ(constructed.getInitialStd().rx_sat_code_bias, 0.15);
    EXPECT_DOUBLE_EQ(constructed.getProcessNoise().rx_sat_code_bias, 1.0e-10);

    // A loaded configuration that names none of the keys has the same values.
    const GnssConfig loaded = load(R"({"processing": {"mode": "PPP-RTK"}})", "defaults");
    EXPECT_FALSE(loaded.estimateRxSatCodeBias());
    EXPECT_EQ(loaded.getRxSatCodeBiasSystems(), (std::vector<std::string>{ "GPS" }));
    EXPECT_TRUE(loaded.getRxSatCodeBiasStore().empty());
    EXPECT_DOUBLE_EQ(loaded.getInitialStd().rx_sat_code_bias, 0.15);
    EXPECT_DOUBLE_EQ(loaded.getProcessNoise().rx_sat_code_bias, 1.0e-10);
}

TEST(RxSatCodeBiasConfigTest, EachKeyIsParsed) {
    const GnssConfig c = load(R"({
        "processing": {"mode": "PPP-RTK"},
        "estimation": {"estimate_rx_sat_code_bias": true,
                       "rx_sat_code_bias_systems": ["GPS", "Galileo"],
                       "rx_sat_code_bias_store": "stores/trimble_alloy.json"},
        "filter": {"initial_std": {"rx_sat_code_bias": 0.3},
                   "process_noise": {"rx_sat_code_bias_q": 2.5e-9}}
    })", "parsed");
    EXPECT_TRUE(c.estimateRxSatCodeBias());
    EXPECT_EQ(c.getRxSatCodeBiasSystems(), (std::vector<std::string>{ "GPS", "Galileo" }));
    EXPECT_EQ(c.getRxSatCodeBiasStore(), "stores/trimble_alloy.json");
    EXPECT_DOUBLE_EQ(c.getInitialStd().rx_sat_code_bias, 0.3);
    EXPECT_DOUBLE_EQ(c.getProcessNoise().rx_sat_code_bias, 2.5e-9);
}

TEST(RxSatCodeBiasConfigTest, TheSystemListTakesNamesAsTheCodeSpellsThemAndFallsBackToGps) {
    const auto systems = [](const std::string& list, const std::string& tag) {
        return load(R"({"processing": {"mode": "PPP-RTK"},
                        "estimation": {"estimate_rx_sat_code_bias": true, "rx_sat_code_bias_systems": )"
                    + list + "}}", tag).getRxSatCodeBiasSystems();
    };
    EXPECT_EQ(systems("[\"Galileo\"]", "sys_gal"), (std::vector<std::string>{ "Galileo" }));
    EXPECT_EQ(systems("[\"GPS\", \"GPS\", \"BeiDou\"]", "sys_dup"), (std::vector<std::string>{ "GPS", "BeiDou" }));
    // An unknown name is dropped; nothing left is the default; an empty list is the default.
    EXPECT_EQ(systems("[\"GPS\", \"Atlantis\"]", "sys_unknown"), (std::vector<std::string>{ "GPS" }));
    EXPECT_EQ(systems("[\"Atlantis\"]", "sys_only_unknown"), (std::vector<std::string>{ "GPS" }));
    EXPECT_EQ(systems("[]", "sys_empty"), (std::vector<std::string>{ "GPS" }));
}

TEST(RxSatCodeBiasConfigTest, WithoutUncombinedProcessingTheKeyIsInert) {
    // Ionosphere-free PPP has no raw code row to carry the term: the key is turned off at load.
    const GnssConfig c = load(R"({"processing": {"mode": "PPP", "uncombined_enabled": false},
                                  "estimation": {"estimate_rx_sat_code_bias": true}})", "if_ppp");
    EXPECT_FALSE(c.estimateRxSatCodeBias());
    // The same key under uncombined PPP is kept.
    const GnssConfig u = load(R"({"processing": {"mode": "PPP", "uncombined_enabled": true},
                                  "estimation": {"estimate_rx_sat_code_bias": true}})", "unc_ppp");
    EXPECT_TRUE(u.estimateRxSatCodeBias());
}

TEST(RxSatCodeBiasConfigTest, TheBaselineLayoutCannotCarryTheFamily) {
    const GnssConfig c = load(
        R"({"baseline_fixture": "IGS_Pasadena",
            "processing": {"mode": "Baseline-Single"},
            "estimation": {"estimate_rx_sat_code_bias": true},
            "baseline": {"reference": {"name": "", "rinex_path": "", "fixed": true},
                         "rover": {"name": "", "rinex_path": ""}}})", "baseline");
    ASSERT_TRUE(c.isBaselineMode());
    EXPECT_FALSE(c.estimateRxSatCodeBias());
}
