/**
 * @file test_cycle_slip_counters.cpp
 * @brief Unit tests for the post-processing slip layer's run-end counters
 *: the printed table and the detector rows a
 *        CycleSlipManager declares for the detectors the registry built.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/CycleSlipCounters.hpp"
#include "slip/CycleSlipManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

/// Load a GnssConfig through the real JSON path from an inline document.
void loadConfig(GnssConfig& config, LogManager& log_manager,
                const std::string& tag, const std::string& json) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / ("gnss_cycle_slip_counters_" + tag + ".json");
    {
        std::ofstream f(p);
        f << json;
    }
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    std::error_code ec;
    fs::remove(p, ec);
}

/// The value printed at the end of a table row (the last space-separated token).
std::string rowValue(const std::string& line) {
    return line.substr(line.find_last_of(' ') + 1);
}

TEST(CycleSlipCounters, TablePrintsEveryRowInDeclaredOrder) {
    CycleSlipCounters c;
    c.epochs = 120;
    c.detections = { { "mw", 41 }, { "graphic", 3 } };
    c.mw_mandates = 38;
    c.integer_repairs = 7;
    c.ambiguity_resets = 29;
    c.full_satellite_resets = 11;
    c.soft_lli_bridge_repairs = 2;
    c.suppressed_mandates = 5;
    c.forced_pivot_handovers = 1;
    c.pivot_frames_repaired_in_place = 4;

    const std::vector<std::string> lines = cycleSlipCounterLines(c);
    ASSERT_EQ(lines.size(), 1u + 2u + 8u);
    EXPECT_NE(lines[0].find("120 epochs"), std::string::npos);

    const std::vector<std::pair<std::string, std::string>> want = {
        { "detections, mw (result entries)", "41" },
        { "detections, graphic (events)", "3" },
        { "MW mandates (ambiguity keys)", "38" },
        { "integer repairs (covariance kept)", "7" },
        { "ambiguity resets (BSSD-aware calls)", "29" },
        { "full satellite resets (satellites)", "11" },
        { "soft-LLI bridge repairs (satellites)", "2" },
        { "suppressed MW mandates (ambiguity keys)", "5" },
        { "forced pivot handovers (constellations)", "1" },
        { "pivot frames repaired in place (constellations)", "4" },
    };
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string& line = lines[i + 1];
        EXPECT_EQ(line.find(want[i].first), 2u) << line;
        EXPECT_EQ(rowValue(line), want[i].second) << line;
    }
    // One value column: every row has the same width.
    for (std::size_t i = 2; i < lines.size(); ++i)
        EXPECT_EQ(lines[i].size(), lines[1].size()) << lines[i];
}

TEST(CycleSlipCounters, ManagerDeclaresOneDetectionRowPerBuiltDetector) {
    {
        LogManager log_manager;
        GnssConfig config;
        loadConfig(config, log_manager, "default", R"({ })");
        const CycleSlipManager manager(config, log_manager);
        const CycleSlipCounters& c = manager.runEndCounters();
        EXPECT_EQ(c.epochs, 0);
        ASSERT_EQ(c.detections.size(), 1u);
        EXPECT_EQ(c.detections[0].first, "mw");
        EXPECT_EQ(c.detections[0].second, 0);
    }
    {
        LogManager log_manager;
        GnssConfig config;
        loadConfig(config, log_manager, "aux",
                   R"({ "estimation": { "cycle_slip": { "graphic": true, "ewl": true } } })");
        const CycleSlipManager manager(config, log_manager);
        const CycleSlipCounters& c = manager.runEndCounters();
        ASSERT_EQ(c.detections.size(), 3u);
        EXPECT_EQ(c.detections[0].first, "mw");
        EXPECT_EQ(c.detections[1].first, "graphic");
        EXPECT_EQ(c.detections[2].first, "ewl");
    }
}

}  // namespace
}  // namespace GnssProcessor
