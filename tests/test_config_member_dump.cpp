/**
 * @file test_config_member_dump.cpp
 * @brief The member dump of a loaded configuration (tests/support/ConfigDump.hpp) over the
 *        configuration files of the repository, and the properties that make it a proof tool.
 *
 * The loader decides more than it reads (it refuses, clamps, derives and forces values), so a
 * change to the loader is proved by comparing the loaded objects, member by member, before and
 * after.  The dump is that comparison's text form.  Locked here:
 *
 *  1. Every JSON file directly under config/ and config/golden/ is loaded and dumped.  When the
 *     environment variable GNSS_CONFIG_DUMP_DIR is set, one text file per configuration is
 *     written there (two builds are then compared with a text diff); otherwise the files go to
 *     the test's temporary folder.  The files that do not load are recorded in
 *     _not_loaded.txt and printed, not failed: only the three reference configurations must
 *     load.  Each dump has one "name=value" line per member, sorted, with no name twice.
 *  2. Two loads of one file give the same text, so a difference between two dumps is a
 *     difference of the loaded objects and never noise of the loader or of the dump.
 *  3. The reference configuration's dump is large (more than 500 lines) and lists every
 *     station, so the generated member list is in use.
 *  4. Changing one plain key in a copy of the reference configuration changes exactly the line
 *     of the member that key sets, with the value printed in full precision.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "support/ConfigDump.hpp"      // gnss_test::configMemberDump
#include "support/TestTempDir.hpp"     // per-process, per-test scratch directory

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// The three reference configurations (the starting point of every network run); each must load.
const std::vector<std::string> kReferenceConfigs = {
    "golden/golden_field_refound.json",
    "golden/golden_sim_refound.json",
    "golden/golden_field_bds_refound.json",
};

/// A configuration file and the label it is reported under ("ppp_internal.json",
/// "golden/golden_sim_refound.json").
struct ConfigFile {
    fs::path path;
    std::string label;
};

/// Every *.json file directly under config/ and config/golden/, in label order.
std::vector<ConfigFile> configFiles() {
    const fs::path root = fs::path(GNSS_PROJECT_ROOT) / "config";
    std::vector<ConfigFile> out;
    for (const auto& [dir, prefix] : {std::make_pair(root, std::string()),
                                      std::make_pair(root / "golden", std::string("golden/"))}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".json") continue;
            out.push_back({e.path(), prefix + e.path().filename().string()});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ConfigFile& a, const ConfigFile& b) { return a.label < b.label; });
    return out;
}

/// Load @p path into @p cfg; on failure @p why says how it failed (false return or exception).
bool loadQuiet(const fs::path& path, GnssConfig& cfg, std::string& why) {
    try {
        LogManager log;
        if (cfg.loadFromJson(path, log)) return true;
        why = "loadFromJson returned false";
    } catch (const std::exception& e) {
        why = std::string("loadFromJson threw: ") + e.what();
    } catch (...) {
        why = "loadFromJson threw a non-standard exception";
    }
    return false;
}

/// The dump of the file at @p path, or an empty string when it does not load.
std::string dumpOf(const fs::path& path, std::string& why) {
    auto cfg = std::make_unique<GnssConfig>();
    if (!loadQuiet(path, *cfg, why)) return {};
    return gnss_test::configMemberDump(*cfg);
}

/// The output folder: GNSS_CONFIG_DUMP_DIR when set, else the test's temporary folder.
fs::path dumpDir() {
    if (const char* d = std::getenv("GNSS_CONFIG_DUMP_DIR")) {
        if (*d) {
            const fs::path p(d);
            std::error_code ec;
            fs::create_directories(p, ec);
            return p;
        }
    }
    return gnss_test::tempDir();
}

/// The dump file name of a label: "golden/x.json" becomes "golden__x.txt".
std::string dumpFileName(const std::string& label) {
    std::string s = label.substr(0, label.size() - std::string(".json").size());
    std::string out;
    for (const char c : s) {
        if (c == '/') out += "__";
        else out += c;
    }
    return out + ".txt";
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

/// The lines of a dump, in order.
std::vector<std::string> linesOf(const std::string& dump) {
    std::vector<std::string> out;
    std::istringstream in(dump);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

/// A dump as name -> value (the name ends at the first '=': no name contains one).
std::map<std::string, std::string> membersOf(const std::string& dump) {
    std::map<std::string, std::string> out;
    for (const std::string& l : linesOf(dump)) {
        const std::size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        out[l.substr(0, eq)] = l.substr(eq + 1);
    }
    return out;
}

/// The members whose line differs between two dumps, with the value in @p b ("<absent>" when
/// the member is in @p a only).
std::map<std::string, std::string> changedMembers(const std::string& a, const std::string& b) {
    const auto ma = membersOf(a);
    const auto mb = membersOf(b);
    std::map<std::string, std::string> out;
    for (const auto& [name, value] : mb) {
        const auto it = ma.find(name);
        if (it == ma.end() || it->second != value) out[name] = value;
    }
    for (const auto& [name, value] : ma)
        if (mb.find(name) == mb.end()) out[name] = "<absent>";
    return out;
}

/// The reference configuration as JSON (comments allowed, as the loader allows them).
nlohmann::json referenceJson() {
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_field_refound.json";
    std::ifstream in(src);
    return nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
}

/// The dump of @p j written to @p path (one path for the base and the edited copy, so a member
/// that depended on the file's location could not show up as a change).
std::string dumpOfJson(const nlohmann::json& j, const fs::path& path) {
    writeText(path, j.dump(2));
    std::string why;
    const std::string d = dumpOf(path, why);
    EXPECT_FALSE(d.empty()) << path << ": " << why;
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------
//  1. Every configuration file is loaded and dumped
// ---------------------------------------------------------------------------

TEST(ConfigMemberDump, EveryConfigurationFileIsLoadedAndDumped) {
    const std::vector<ConfigFile> files = configFiles();
    ASSERT_FALSE(files.empty()) << "no configuration file found under " << GNSS_PROJECT_ROOT;
    const fs::path out_dir = dumpDir();

    std::set<std::string> loaded;
    std::string not_loaded;
    for (const ConfigFile& f : files) {
        std::string why;
        const std::string dump = dumpOf(f.path, why);
        if (dump.empty()) {
            not_loaded += f.label + ": " + why + "\n";
            continue;
        }
        loaded.insert(f.label);
        writeText(out_dir / dumpFileName(f.label), dump);

        // One "name=value" line per member, sorted, no name twice.
        const std::vector<std::string> lines = linesOf(dump);
        EXPECT_TRUE(std::is_sorted(lines.begin(), lines.end())) << f.label;
        std::set<std::string> names;
        for (const std::string& l : lines) {
            const std::size_t eq = l.find('=');
            ASSERT_NE(eq, std::string::npos) << f.label << ": line without '=': " << l;
            ASSERT_GT(eq, 0u) << f.label << ": line without a name: " << l;
            EXPECT_TRUE(names.insert(l.substr(0, eq)).second)
                << f.label << ": member printed twice: " << l.substr(0, eq);
        }
    }
    writeText(out_dir / "_not_loaded.txt", not_loaded);
    std::cout << "[ConfigMemberDump] " << loaded.size() << " of " << files.size()
              << " configuration files loaded; dumps in " << out_dir.string() << "\n";
    if (!not_loaded.empty()) std::cout << "[ConfigMemberDump] not loaded:\n" << not_loaded;
    RecordProperty("loaded", static_cast<int>(loaded.size()));
    RecordProperty("not_loaded", static_cast<int>(files.size() - loaded.size()));

    for (const std::string& ref : kReferenceConfigs)
        EXPECT_EQ(loaded.count(ref), 1u) << ref << " did not load:\n" << not_loaded;
}

// ---------------------------------------------------------------------------
//  2. Two loads of one file give the same text
// ---------------------------------------------------------------------------

TEST(ConfigMemberDump, TwoLoadsOfOneFileGiveIdenticalText) {
    std::size_t compared = 0;
    for (const ConfigFile& f : configFiles()) {
        std::string why1, why2;
        const std::string first = dumpOf(f.path, why1);
        if (first.empty()) continue;   // recorded by the case above
        const std::string second = dumpOf(f.path, why2);
        ASSERT_FALSE(second.empty()) << f.label << " loaded once and then failed: " << why2;
        ++compared;
        if (first == second) continue;
        std::string diff;
        for (const auto& [name, value] : changedMembers(first, second))
            diff += "  " + name + " -> " + value + "\n";
        ADD_FAILURE() << f.label << ": two loads differ in\n" << diff;
    }
    EXPECT_GE(compared, kReferenceConfigs.size());
}

// ---------------------------------------------------------------------------
//  3. The reference dump is large and lists every station
// ---------------------------------------------------------------------------

TEST(ConfigMemberDump, TheReferenceDumpHasMoreThan500LinesAndEveryStation) {
    for (const std::string& ref : {std::string("golden_field_refound.json"),
                                   std::string("golden_sim_refound.json")}) {
        const fs::path path = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / ref;
        auto cfg = std::make_unique<GnssConfig>();
        std::string why;
        ASSERT_TRUE(loadQuiet(path, *cfg, why)) << ref << ": " << why;
        const std::string dump = gnss_test::configMemberDump(*cfg);
        const std::vector<std::string> lines = linesOf(dump);
        EXPECT_GT(lines.size(), 500u) << ref;

        const auto members = membersOf(dump);
        const auto& stations = cfg->getNetworkConfig().stations;
        ASSERT_GT(stations.size(), 1u) << ref;
        const auto size_it = members.find("getNetworkConfig.stations.size");
        ASSERT_NE(size_it, members.end()) << ref;
        EXPECT_EQ(size_it->second, std::to_string(stations.size())) << ref;
        for (std::size_t i = 0; i < stations.size(); ++i) {
            const std::string key = "getNetworkConfig.stations[" + std::to_string(i) + "].name";
            const auto it = members.find(key);
            ASSERT_NE(it, members.end()) << ref << ": " << key;
            EXPECT_EQ(it->second, stations[i].name) << ref << ": " << key;
        }
        // A plain member read straight from the file, as a spot check of the getter lines.
        const auto mode = members.find("getProcessingMode");
        ASSERT_NE(mode, members.end()) << ref;
        EXPECT_EQ(mode->second, std::to_string(static_cast<long long>(cfg->getProcessingMode())));
    }
}

// ---------------------------------------------------------------------------
//  4. One plain key changes exactly the line of its member
// ---------------------------------------------------------------------------

TEST(ConfigMemberDump, ChangingOnePlainKeyChangesExactlyTheLineOfItsMember) {
    struct Edit {
        const char* what;
        std::function<void(nlohmann::json&)> apply;
        const char* member;   ///< the dump name of the member the key sets
        const char* value;    ///< its dump text after the edit
    };
    const std::vector<Edit> edits = {
        // A floating-point member of the network struct; 0.1 shows the 17 significant digits.
        {"network.epoch_sync_tolerance_s = 0.1",
         [](nlohmann::json& j) { j["network"]["epoch_sync_tolerance_s"] = 0.1; },
         "getNetworkConfig.epoch_sync_tolerance_s", "0.10000000000000001"},
        // A bool member of the network struct, absent from the file (default false).
        {"network.product_flush_per_epoch = true",
         [](nlohmann::json& j) { j["network"]["product_flush_per_epoch"] = true; },
         "getNetworkConfig.product_flush_per_epoch", "true"},
        // A floating-point member of the configuration class, read through its getter.
        {"processing.elevation_mask_deg = 12.5",
         [](nlohmann::json& j) { j["processing"]["elevation_mask_deg"] = 12.5; },
         "getElevationMaskDeg", "12.5"},
    };

    const nlohmann::json base_json = referenceJson();
    const fs::path path = gnss_test::tempPath("reference_copy.json");
    const std::string base = dumpOfJson(base_json, path);
    ASSERT_FALSE(base.empty());

    for (const Edit& e : edits) {
        nlohmann::json j = base_json;
        e.apply(j);
        const std::string edited = dumpOfJson(j, path);
        ASSERT_FALSE(edited.empty()) << e.what;
        const auto changed = changedMembers(base, edited);
        std::string listed;
        for (const auto& [name, value] : changed) listed += "  " + name + "=" + value + "\n";
        ASSERT_EQ(changed.size(), 1u) << e.what << " changed:\n" << listed;
        EXPECT_EQ(changed.begin()->first, e.member) << e.what;
        EXPECT_EQ(changed.begin()->second, e.value) << e.what;
    }
}
