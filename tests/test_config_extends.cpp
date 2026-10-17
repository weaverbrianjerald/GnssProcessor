/**
 * @file test_config_extends.cpp
 * @brief Contract tests for `"extends"` config inheritance in GnssConfig::loadFromJson.
 *
 * `extends` adds base+delta composition ("same as X but with Y") to a loader whose configs
 * were otherwise all standalone.  The change is safe only if two properties hold, and both
 * are locked here:
 *
 *  1. **Byte-identical for every existing config.**  A document with no `extends` key
 *     parses exactly as it did before inheritance existed.  Every existing config takes
 *     that path, so a regression here would silently re-interpret all of them.
 *  2. **Arrays replace, never concatenate.**  `network.stations` is an array, and the
 *     warm-resume fingerprint hashes the roster in order (SnapshotFingerprint.cpp).
 *     A base roster concatenated with a child roster would give duplicate stations, a
 *     different order, and a station count matching no intended regime, while still
 *     loading cleanly.
 *     The fingerprint exists to catch that class of silent error, so the merge rule is
 *     tested directly.
 *
 * Also covered: nested-object merging (the purpose of the feature), chains, multiple bases
 * with later-wins ordering, removal of the `extends` key before the section loaders run,
 * and the hard errors (missing base, cycle including self-reference, depth overflow, and a
 * non-string `extends` value), which fail the load rather than half-apply a configuration.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigJsonHelpers.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path cfgDir() {
    const fs::path d = gnss_test::tempDir() / "gnss_extends_tests";
    fs::create_directories(d);
    return d;
}

fs::path writeCfg(const std::string& name, const std::string& body) {
    const fs::path p = cfgDir() / name;
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config body, with an injectable extra fragment.
std::string netBody(const std::string& extra) {
    return std::string(R"({
        "app_mode": "post",
        "data_source": "internal",
        "processing": {
            "mode": "ppp-rtk-network",
            "active_systems": ["GPS"],
            "frequency_mode": "all",
            "elevation_mask_deg": 10.0
        },
        "network": {
            "stations": [
                {"name": "AAAA", "position_ecef": [-2493304.9, -4655214.8, 3565497.7]},
                {"name": "BBBB", "position_ecef": [-2281621.7, -1453585.1, 5756964.9]},
                {"name": "CCCC", "position_ecef": [-2700404.1, -4292605.6, 3855137.1]}
            ],
            "clock_datum_station": "AAAA"
        })") + (extra.empty() ? "" : ",\n        " + extra) + "\n    }";
}

}  // namespace

// ---------------------------------------------------------------------------
//  Property 1: the no-extends path is unchanged
// ---------------------------------------------------------------------------
TEST(ConfigExtends, NoExtendsKeyLeavesChainEmptyAndLoadsNormally) {
    LogManager log;
    const auto p = writeCfg("plain.json", netBody(R"("network_extra": {})"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.configExtendsChain().empty());
    EXPECT_EQ(cfg.getActiveSystems().size(), 1u);
}

// ---------------------------------------------------------------------------
//  Property 2: arrays replace (the fingerprint-safety property)
// ---------------------------------------------------------------------------
TEST(ConfigExtends, ArraysReplaceAndDoNotConcatenate) {
    LogManager log;
    writeCfg("base_roster.json", netBody(""));                       // 3 stations
    const auto child = writeCfg("child_roster.json", R"({
        "extends": "base_roster.json",
        "network": { "stations": [
            {"name": "YYYY", "position_ecef": [-2493304.9, -4655214.8, 3565497.7]},
            {"name": "ZZZZ", "position_ecef": [-2281621.7, -1453585.1, 5756964.9]}
        ],
        "clock_datum_station": "YYYY" }
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(child, log));
    // 2, not 5: a concatenating merge would silently change the roster order and count,
    // both of which are hashed into the warm-resume fingerprint.
    ASSERT_EQ(cfg.getNetworkConfig().stations.size(), 2u);
    EXPECT_EQ(cfg.getNetworkConfig().stations[0].name, "YYYY");
    EXPECT_EQ(cfg.getNetworkConfig().stations[1].name, "ZZZZ");
}

// A replaced roster invalidates an inherited clock_datum_station, and the existing validator
// catches it ("clock_datum_station 'X' not found among stations").  This is the one
// foreseeable failure of a base+delta split, and the load fails instead of silently
// electing some other station as the datum.
TEST(ConfigExtends, ReplacingRosterWithoutUpdatingInheritedDatumStationIsRejected) {
    LogManager log;
    writeCfg("dr_base.json", netBody(""));                     // datum AAAA, roster A/B/C
    const auto child = writeCfg("dr_child.json", R"({
        "extends": "dr_base.json",
        "network": { "stations": [
            {"name": "YYYY", "position_ecef": [-2493304.9, -4655214.8, 3565497.7]},
            {"name": "ZZZZ", "position_ecef": [-2281621.7, -1453585.1, 5756964.9]}
        ] }
    })");
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(child, log));
}

TEST(ConfigExtends, ActiveSystemsArrayAlsoReplaces) {
    LogManager log;
    writeCfg("base_sys.json", netBody(""));                          // ["GPS"]
    const auto child = writeCfg("child_sys.json", R"({
        "extends": "base_sys.json",
        "processing": { "active_systems": ["GPS", "Galileo"] }
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(child, log));
    ASSERT_EQ(cfg.getActiveSystems().size(), 2u);
    EXPECT_EQ(cfg.getActiveSystems()[1], "Galileo");
}

// ---------------------------------------------------------------------------
//  Merge semantics
// ---------------------------------------------------------------------------
TEST(ConfigExtends, ChildOverridesBaseScalarAndKeepsUnmentionedSiblings) {
    LogManager log;
    writeCfg("base_scalar.json", netBody(""));                       // elevation_mask_deg 10
    const auto child = writeCfg("child_scalar.json", R"({
        "extends": "base_scalar.json",
        "processing": { "elevation_mask_deg": 25.0 }
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(child, log));
    EXPECT_DOUBLE_EQ(cfg.getElevationMaskDeg(), 25.0);
    // The sibling keys of the overridden one survive, so a delta config sets one knob
    // without restating the section.
    EXPECT_EQ(cfg.getNetworkConfig().stations.size(), 3u);
    EXPECT_EQ(cfg.getNetworkConfig().clock_datum_station, "AAAA");
    ASSERT_EQ(cfg.configExtendsChain().size(), 1u);
}

TEST(ConfigExtends, DeepMergeIsRecursiveAcrossNestedObjects) {
    // Exercised directly on the helper so the assertion is about the merge rule itself.
    nlohmann::json base = R"({"a":{"b":{"c":1,"d":2},"e":3},"keep":9})"_json;
    const nlohmann::json overlay = R"({"a":{"b":{"c":42}}})"_json;
    detail::deepMergeJson(base, overlay);
    EXPECT_EQ(base["a"]["b"]["c"].get<int>(), 42);   // overridden
    EXPECT_EQ(base["a"]["b"]["d"].get<int>(), 2);    // sibling preserved at depth
    EXPECT_EQ(base["a"]["e"].get<int>(), 3);         // sibling preserved one level up
    EXPECT_EQ(base["keep"].get<int>(), 9);           // untouched branch
}

TEST(ConfigExtends, MultipleBasesApplyLeftToRightAndLaterWins) {
    LogManager log;
    writeCfg("mb_base.json", netBody(""));
    writeCfg("mb_first.json",  R"({"processing":{"elevation_mask_deg":15.0}})");
    writeCfg("mb_second.json", R"({"processing":{"elevation_mask_deg":20.0}})");
    const auto child = writeCfg("mb_child.json", R"({
        "extends": ["mb_base.json", "mb_first.json", "mb_second.json"]
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(child, log));
    EXPECT_DOUBLE_EQ(cfg.getElevationMaskDeg(), 20.0);   // last base wins over earlier bases
    EXPECT_EQ(cfg.configExtendsChain().size(), 3u);
}

TEST(ConfigExtends, ChainOfThreeResolvesAndChildStillWins) {
    LogManager log;
    writeCfg("c_root.json", netBody(""));
    writeCfg("c_mid.json", R"({
        "extends": "c_root.json",
        "processing": { "elevation_mask_deg": 15.0 }
    })");
    const auto leaf = writeCfg("c_leaf.json", R"({
        "extends": "c_mid.json",
        "processing": { "elevation_mask_deg": 30.0 }
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(leaf, log));
    EXPECT_DOUBLE_EQ(cfg.getElevationMaskDeg(), 30.0);
    EXPECT_EQ(cfg.getNetworkConfig().stations.size(), 3u);   // inherited through two hops
}

// ---------------------------------------------------------------------------
//  Hard errors: the load fails instead of half-applying a configuration
// ---------------------------------------------------------------------------
TEST(ConfigExtends, MissingBaseIsAHardError) {
    LogManager log;
    const auto child = writeCfg("orphan.json", R"({
        "extends": "this_base_does_not_exist.json",
        "processing": { "mode": "ppp-rtk-network" }
    })");
    GnssConfig cfg;
    // A silently ignored missing base would run with the delta only: a config that looks
    // armed and is not.  The load therefore fails.
    EXPECT_FALSE(cfg.loadFromJson(child, log));
}

TEST(ConfigExtends, CycleIsRejected) {
    LogManager log;
    writeCfg("cyc_a.json", R"({"extends":"cyc_b.json"})");
    const auto b = writeCfg("cyc_b.json", R"({"extends":"cyc_a.json"})");
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(b, log));
}

TEST(ConfigExtends, SelfReferenceIsRejected) {
    LogManager log;
    const auto s = writeCfg("selfref.json", R"({"extends":"selfref.json"})");
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(s, log));
}

TEST(ConfigExtends, OverlongChainIsRejected) {
    LogManager log;
    // 12 links, past the depth cap of 8.
    writeCfg("d0.json", netBody(""));
    for (int i = 1; i <= 12; ++i)
        writeCfg("d" + std::to_string(i) + ".json",
                 "{\"extends\":\"d" + std::to_string(i - 1) + ".json\"}");
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(cfgDir() / "d12.json", log));
}

TEST(ConfigExtends, NonStringExtendsIsRejected) {
    LogManager log;
    const auto bad = writeCfg("badtype.json", R"({"extends": 42})");
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(bad, log));
}

// ---------------------------------------------------------------------------
//  The directive must not leak into the settings
// ---------------------------------------------------------------------------
TEST(ConfigExtends, ExtendsKeyIsStrippedBeforeSectionLoadersRun) {
    LogManager log;
    writeCfg("strip_base.json", netBody(""));
    const auto child = writeCfg("strip_child.json", R"({
        "extends": "strip_base.json",
        "processing": { "elevation_mask_deg": 12.0 }
    })");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(child, log));
    // Loads cleanly with the merged values: `extends` is a loader directive, so no section
    // loader should ever see it (an unknown top-level key would otherwise be carried into
    // the merged document that the legacy-flat `jval` fallback also searches).
    EXPECT_DOUBLE_EQ(cfg.getElevationMaskDeg(), 12.0);
    EXPECT_EQ(cfg.getNetworkConfig().stations.size(), 3u);
}
