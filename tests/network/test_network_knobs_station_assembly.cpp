/**
 * @file test_network_knobs_station_assembly.cpp
 * @brief The six NetworkKnobs fields of card NOM3 step PB1a against the expressions that
 *        networkPassB_ (and, for four of them, the harvest) derived before the step.
 *
 * networkPassB_ computed the live F3 block predicate and the four in-block predicates from
 * other knobs, and read network.cycle_slip.relevel_free_epoch from the configuration, on
 * every epoch; NetworkLocalCarryHarvest derived the four in-block predicates a second time.
 * Step PB1a resolves them once in NetworkKnobs::snapshot.  Each case loads a posture, takes
 * the snapshot, and compares every field with its frozen expression; across the postures
 * every field is seen both on and off, and both goldens are among them.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal two-station GPS network configuration with @p network_extra in its network
/// section (the NetworkKnobsEpochDriver pattern).
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
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

/// Write @p body to a temporary file and load it into @p cfg.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("knobs_station_assembly_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// Load a golden configuration (config/golden/@p name) into @p cfg.
bool loadGolden(const char* name, GnssConfig& cfg) {
    LogManager log;
    return cfg.loadFromJson(fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name, log);
}

/// Each field against the expression networkPassB_ used before step PB1a (the harvest's
/// conjunction for the four in-block predicates is the same set of operands); returns the
/// snapshot.  @p seen collects, per field, which values were observed.
NetworkKnobs expectFrozen(const GnssConfig& cfg, const std::string& what,
                          std::map<std::string, std::vector<bool>>& seen) {
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    const bool pl_blk_on = k.pl_local_block && k.carry_cpl_cond;
    EXPECT_EQ(k.pl_blk_on, pl_blk_on) << what;
    EXPECT_EQ(k.zwd_blk_on, k.zwd_in_block && pl_blk_on) << what;
    EXPECT_EQ(k.iono_blk_on, k.iono_blk && pl_blk_on) << what;
    EXPECT_EQ(k.rxclk_blk_on, k.rxclk_blk && pl_blk_on) << what;
    EXPECT_EQ(k.rxcb_blk_on, k.rxcb_blk && pl_blk_on && k.rx_cb_on) << what;
    // The harvest's former form of the receiver code-bias predicate.
    EXPECT_EQ(k.rxcb_blk_on, k.rxcb_blk && k.rx_cb_on && k.pl_local_block && k.carry_cpl_cond)
        << what;
    EXPECT_EQ(k.relevel_free_on, cfg.networkCycleSlipRelevelFreeEpoch()) << what;
    seen["pl_blk_on"].push_back(k.pl_blk_on);
    seen["zwd_blk_on"].push_back(k.zwd_blk_on);
    seen["iono_blk_on"].push_back(k.iono_blk_on);
    seen["rxclk_blk_on"].push_back(k.rxclk_blk_on);
    seen["rxcb_blk_on"].push_back(k.rxcb_blk_on);
    seen["relevel_free_on"].push_back(k.relevel_free_on);
    return k;
}

/// A level-vehicle posture with the F3 local block and the three in-block carries the float
/// vehicle refuses (design PB1, finding 6), and the free-epoch latch.
const char* kLevelBlock =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_leveling_local_block\": true,\n"
    "    \"carry_zwd\": true,\n"
    "    \"carry_zwd_in_block\": true,\n"
    "    \"carry_iono_in_block\": true,\n"
    "    \"carry_rx_clock_in_block\": true,\n"
    "    \"cycle_slip\": { \"mw_detect\": true, \"relevel_free_epoch\": true },\n";

} // namespace

TEST(NetworkKnobsStationAssembly, DefaultsAreOff) {
    std::map<std::string, std::vector<bool>> seen;
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(""), cfg));
    const NetworkKnobs k = expectFrozen(cfg, "default", seen);
    EXPECT_FALSE(k.pl_blk_on);
    EXPECT_FALSE(k.zwd_blk_on);
    EXPECT_FALSE(k.iono_blk_on);
    EXPECT_FALSE(k.rxclk_blk_on);
    EXPECT_FALSE(k.rxcb_blk_on);
    EXPECT_FALSE(k.relevel_free_on);
}

TEST(NetworkKnobsStationAssembly, LevelVehicleBlockCarries) {
    std::map<std::string, std::vector<bool>> seen;
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("level_block", networkConfig(kLevelBlock), cfg));
    expectFrozen(cfg, "level vehicle, local block", seen);
}

TEST(NetworkKnobsStationAssembly, BothGoldens) {
    std::map<std::string, std::vector<bool>> seen;
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(name, cfg)) << name;
        const NetworkKnobs k = expectFrozen(cfg, name, seen);
        // The float vehicle supplies the local block and the goldens arm the coupling rows.
        EXPECT_TRUE(k.pl_blk_on) << name;
    }
}

// Across the postures every field is seen on and off, so no comparison above is vacuous.
TEST(NetworkKnobsStationAssembly, EachFieldBothWays) {
    std::map<std::string, std::vector<bool>> seen;
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("both_default", networkConfig(""), cfg));
        expectFrozen(cfg, "default", seen);
    }
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("both_level", networkConfig(kLevelBlock), cfg));
        expectFrozen(cfg, "level vehicle, local block", seen);
    }
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(name, cfg)) << name;
        expectFrozen(cfg, name, seen);
    }
    ASSERT_EQ(seen.size(), 6u);
    for (const auto& [field, values] : seen) {
        bool on = false, off = false;
        for (const bool v : values) (v ? on : off) = true;
        EXPECT_TRUE(on) << field << " is never on";
        EXPECT_TRUE(off) << field << " is never off";
    }
}
