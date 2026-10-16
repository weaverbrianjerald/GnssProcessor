/**
 * @file test_active_system_admission.cpp
 * @brief The network admits observations by processing.active_systems.
 *
 * Admission and the inter-system bias allocation both follow
 * @c processing.active_systems: the synced epochs are stripped of every inactive system
 * right after the sync (@c MultiRinexSync::retainSystems), so a signal priority list for
 * an inactive system cannot put rows in the solve without a bias column.  The loader
 * therefore accepts such a list and only warns that it is ignored (the card GAL loader
 * refused it, which made two archived GPS-only configs unloadable).  Covered here: the
 * strip on synthetic epochs, the warning on an inline config, and the two real archived
 * configs loading with the warning (G3 of card OWD).
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

RinexObservation obsOf(const std::string& sat, System sys) {
    RinexObservation o;
    o.satellite = sat;
    o.system    = sys;
    return o;
}

/// Result of a load performed with the "gnss_config" logger captured.
struct CapturedLoad {
    bool        ok{false};
    std::string text;
};

/// Load @p p capturing the loader's "gnss_config" logger (the pattern of
/// test_codebias_network_config.cpp: an initialized manager, a unique directory per
/// call, only the two logger names this helper creates dropped before and after).
CapturedLoad loadCapturing(const fs::path& p, const std::string& tag) {
    CapturedLoad out;
    LogManager log;
    const fs::path dir = fs::temp_directory_path() / ("owd_active_admission_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    auto cfg_log = log.getLogger("gnss_config");
    std::ostringstream oss;
    cfg_log->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(oss));
    GnssConfig cfg;
    out.ok = cfg.loadFromJson(p, log);
    cfg_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    return out;
}

/// The warning's stable phrase (GnssConfigLoadNetwork.cpp, card OWD O4).
constexpr const char* kIgnored = "is IGNORED";

}  // namespace

// The strip keeps the active systems only, reports what it removed per system, and keeps a
// station's key when its vector empties.
TEST(ActiveSystemAdmission, RetainSystemsKeepsOnlyActiveSystems) {
    std::vector<SyncedEpoch> epochs(2);
    epochs[0].by_station["AAAA"] = { obsOf("G05", System::GPS), obsOf("E11", System::Galileo),
                                     obsOf("R07", System::GLONASS) };
    epochs[0].by_station["BBBB"] = { obsOf("E24", System::Galileo) };
    epochs[1].by_station["AAAA"] = { obsOf("G05", System::GPS), obsOf("C23", System::BeiDou) };

    const auto removed = MultiRinexSync::retainSystems(epochs, {"GPS"});
    EXPECT_EQ(removed, (std::map<std::string, std::size_t>{
                           {"BeiDou", 1}, {"GLONASS", 1}, {"Galileo", 2}}));
    ASSERT_EQ(epochs[0].by_station.at("AAAA").size(), 1u);
    EXPECT_EQ(epochs[0].by_station.at("AAAA")[0].satellite, "G05");
    ASSERT_EQ(epochs[0].by_station.count("BBBB"), 1u);          // key kept
    EXPECT_TRUE(epochs[0].by_station.at("BBBB").empty());
    ASSERT_EQ(epochs[1].by_station.at("AAAA").size(), 1u);

    // Everything active: nothing removed, nothing reported.
    EXPECT_TRUE(MultiRinexSync::retainSystems(epochs, {"GPS", "Galileo"}).empty());
}

// A priority list for an inactive system loads, with one warning naming the ignored list.
TEST(ActiveSystemAdmission, ListOfAnInactiveSystemIsIgnoredWithAWarning) {
    const fs::path p = fs::temp_directory_path() / "owd_active_admission_inline.json";
    {
        std::ofstream f(p);
        f << "{\n"
             "  \"processing\": {\n"
             "    \"mode\": \"ppp-rtk-network\",\n"
             "    \"active_systems\": [ \"GPS\" ],\n"
             "    \"signal_priorities\": { \"GPS\": [\"C1C\", \"L1C\", \"C2W\", \"L2W\"],\n"
             "                             \"Galileo\": [\"C1C\", \"L1C\", \"C5Q\", \"L5Q\"] }\n"
             "  },\n"
             "  \"network\": {\n"
             "    \"clock_datum_station\": \"AAAA\",\n"
             "    \"stations\": [\n"
             "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
             "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
             "    ]\n"
             "  }\n"
             "}\n";
    }
    const CapturedLoad r = loadCapturing(p, "inline");
    ASSERT_TRUE(r.ok) << r.text;
    EXPECT_NE(r.text.find("'Galileo' list"), std::string::npos) << r.text;
    EXPECT_NE(r.text.find(kIgnored), std::string::npos) << r.text;
    std::error_code ec;
    fs::remove(p, ec);
}

// The two archived GPS-only configs that card GAL's check refused load, and the log names
// the ignored Galileo list (G3 of card OWD).  Skipped when neither file is present.
TEST(ActiveSystemAdmission, ArchivedGpsOnlyConfigsLoadWithTheWarning) {
    const fs::path root = GNSS_PROJECT_ROOT;
    const fs::path configs[] = {
        root / "config" / "test_suite" / "ppp_rtk_network_global_5min_gpsonly.json",
        root / "config" / "archive" / "test_suite" / "_sdewl_probe_gps10.json",
    };
    int n = 0;
    for (const fs::path& p : configs) {
        if (!fs::exists(p)) continue;
        const CapturedLoad r = loadCapturing(p, "archived_" + std::to_string(n++));
        EXPECT_TRUE(r.ok) << p << "\n" << r.text;
        EXPECT_NE(r.text.find("'Galileo' list"), std::string::npos) << p;
        EXPECT_NE(r.text.find(kIgnored), std::string::npos) << p;
    }
    if (n == 0) GTEST_SKIP() << "neither archived config is present under " << root;
}
