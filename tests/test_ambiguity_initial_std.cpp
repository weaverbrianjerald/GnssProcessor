/**
 * @file test_ambiguity_initial_std.cpp
 * @brief One source for the float-ambiguity initial standard deviation.
 *
 * @c GnssConfig::getAmbiguityStd() once returned a hard-coded 1000 m with no JSON
 * key, read by the births and most resets, while the pivot reseeds, the DD births
 * and the KalmanFilter's non-pivot reset read @c filter.initial_std.ambiguity.
 * A config setting 100 m therefore ran two ambiguity scales in one run.  Guarded
 * here: the getter returns the configured value, the loader
 * default is unchanged (1000 m, so a config without the key is unaffected), and a
 * birth without an override takes the configured variance.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal loadable PPP config; @p filter_block is spliced in as the "filter" section.
std::string pppConfig(const std::string& filter_block) {
    std::string body = "{\n"
                       "  \"processing\": { \"mode\": \"ppp\", \"active_systems\": [ \"GPS\" ] }";
    if (!filter_block.empty()) body += ",\n  \"filter\": " + filter_block;
    return body + "\n}\n";
}

/// Write @p body to a per-tag temporary file, load it into @p cfg and remove the file.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("ambiguity_std_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

}  // namespace

// The configured value is the getter's value: one ambiguity scale per run.
TEST(AmbiguityInitialStd, GetterReturnsTheConfiguredValue) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("configured",
        pppConfig("{ \"initial_std\": { \"ambiguity\": 100.0 } }"), cfg));
    EXPECT_DOUBLE_EQ(cfg.getInitialStd().ambiguity, 100.0);
    EXPECT_DOUBLE_EQ(cfg.getAmbiguityStd(), 100.0);
}

// Without the key the loader default (1000 m) applies to both reads, as before the step.
TEST(AmbiguityInitialStd, DefaultIsUnchanged) {
    GnssConfig loaded;
    ASSERT_TRUE(loadInto("default", pppConfig(""), loaded));
    EXPECT_DOUBLE_EQ(loaded.getInitialStd().ambiguity, 1000.0);
    EXPECT_DOUBLE_EQ(loaded.getAmbiguityStd(), 1000.0);

    const GnssConfig constructed;
    EXPECT_DOUBLE_EQ(constructed.getAmbiguityStd(), 1000.0);
}

// A birth without an override takes the configured variance, the scale the pivot
// reseeds and the DD births use.
TEST(AmbiguityInitialStd, BirthWithoutOverrideUsesTheConfiguredVariance) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("birth",
        pppConfig("{ \"initial_std\": { \"ambiguity\": 100.0 } }"), cfg));
    LogManager lm;
    StateManager sm{ 5, lm, cfg };
    const std::string key = StateKeys::makeAmbiguityKey("G01", "L1C");
    const std::size_t idx = sm.addAmbiguity(key, 0.0);
    EXPECT_DOUBLE_EQ(sm.getStateVariance(static_cast<int>(idx)), 100.0 * 100.0);

    // An explicit override still wins (the substituted-ATX seed).
    const std::string key2 = StateKeys::makeAmbiguityKey("G02", "L1C");
    const std::size_t idx2 = sm.addAmbiguity(key2, 0.0, 5000.0);
    EXPECT_DOUBLE_EQ(sm.getStateVariance(static_cast<int>(idx2)), 5000.0 * 5000.0);
}
