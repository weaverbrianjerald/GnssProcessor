/**
 * @file test_level_authority.cpp
 * @brief Config-contract + kernel tests for LV-6
 *        (@c network.level_authority_csgf; `SPEC_level_authority_internal.md`).
 *
 * The census is measure-only and default-OFF; the loader side locks the
 * defaults, the reject bands, and the arming independence from the
 * (falsified, OFF) iono-carry inject path.  The kernel side pins the
 * two-tier reduction: tier-1 information weighting, the tier-2 median's
 * outlier immunity (the fleet-clustered bias tail), the min_los screen, and
 * the nullopt "unavailable is not 0" sentinel.  The tracking verdict is
 * graded on the regenerated wsh fixture (SPEC §4 G1), not here.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkRobustStats.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("lvlauth_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

using Rows = std::vector<std::vector<std::pair<double, double>>>;

}  // namespace

// ---------------------------------------------------------------- loader ----

TEST(LevelAuthorityConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkLevelAuthorityCsgf());
    EXPECT_EQ(cfg.networkLevelAuthorityMinN(), 10);
    EXPECT_EQ(cfg.networkLevelAuthorityMinLos(), 4);
}

TEST(LevelAuthorityConfig, ArmsWithoutTheIonoCarry) {
    // The census arms the CS advance INDEPENDENTLY of the falsified carry
    // inject path — carry_iono must stay off and unrequired (SPEC §3).
    GnssConfig cfg;
    const std::string extra = "    \"level_authority_csgf\": true,\n";
    ASSERT_TRUE(loadInto("independent", networkConfig(extra), cfg));
    EXPECT_TRUE(cfg.networkLevelAuthorityCsgf());
    EXPECT_FALSE(cfg.networkCarryIono());
}

TEST(LevelAuthorityConfig, RejectBandsResetToDefaults) {
    GnssConfig cfg;
    const std::string extra =
        "    \"level_authority_csgf\": true,\n"
        "    \"level_authority_min_n\": 1,\n"
        "    \"level_authority_min_los\": 0,\n";
    ASSERT_TRUE(loadInto("rej", networkConfig(extra), cfg));
    EXPECT_EQ(cfg.networkLevelAuthorityMinN(), 10);
    EXPECT_EQ(cfg.networkLevelAuthorityMinLos(), 4);
}

// LV-6 G2 support: the per-LOS pierce-point dump exists so a reference field can
// be reduced through the census's OWN sampling (SPEC §4 G2 / §5.2 — an unmatched
// LOS set reintroduces, in miniature, the sampling error that killed the naive
// planetary-mean grade).  Default OFF, and INERT without the census it dumps.
TEST(LevelAuthorityConfig, LosDumpDefaultsOffAndNeedsTheCensus) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("losdump_default", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkLevelAuthorityLosDump());

    // Armed WITHOUT the census: there is no census to dump, so it is
    // warn-and-disarmed rather than left armed-but-inert (invariant #4).
    GnssConfig alone;
    ASSERT_TRUE(loadInto("losdump_alone",
        networkConfig("    \"level_authority_los_dump\": true,\n"), alone));
    EXPECT_FALSE(alone.networkLevelAuthorityLosDump())
        << "the dump must not survive without level_authority_csgf";

    // Armed WITH the census: survives, and does not disturb the census knobs.
    GnssConfig both;
    ASSERT_TRUE(loadInto("losdump_armed",
        networkConfig("    \"level_authority_csgf\": true,\n"
                      "    \"level_authority_los_dump\": true,\n"), both));
    EXPECT_TRUE(both.networkLevelAuthorityCsgf());
    EXPECT_TRUE(both.networkLevelAuthorityLosDump());
    EXPECT_EQ(both.networkLevelAuthorityMinN(), 10);
    EXPECT_EQ(both.networkLevelAuthorityMinLos(), 4);
}

// ---------------------------------------------------------------- kernel ----

TEST(LevelAuthorityKernel, WeightedMeanThenMedian) {
    // Three stations at levels 10/12/40 (one bias-tail outlier): the fleet
    // MEDIAN reads 12 — the outlier never enters (the tier-2 rationale).
    Rows rows(3);
    for (int k = 0; k < 4; ++k) rows[0].push_back({10.0, 1.0});
    for (int k = 0; k < 4; ++k) rows[1].push_back({12.0, 1.0});
    for (int k = 0; k < 4; ++k) rows[2].push_back({40.0, 1.0});
    const auto r = levelAuthorityReduce(rows, 4);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->level_tecu, 12.0);
    EXPECT_EQ(r->n_sta, 3);
    EXPECT_EQ(r->n_los, 12);
}

TEST(LevelAuthorityKernel, InformationWeightingInsideAStation) {
    // One precise LOS at 20 (w=100) + one noisy at 0 (w=1): the station mean
    // sits near 20, not at 10 — tier 1 is information-weighted.
    Rows rows(1);
    rows[0] = {{20.0, 100.0}, {0.0, 1.0}, {20.0, 100.0}, {20.0, 100.0}};
    const auto r = levelAuthorityReduce(rows, 4);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(r->level_tecu, 20.0 * 300.0 / 301.0, 1e-9);
}

TEST(LevelAuthorityKernel, MinLosScreensAStationOut) {
    Rows rows(2);
    rows[0] = {{10.0, 1.0}, {10.0, 1.0}, {10.0, 1.0}, {10.0, 1.0}};
    rows[1] = {{99.0, 1.0}};   // below min_los — must not enter the median
    const auto r = levelAuthorityReduce(rows, 4);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->level_tecu, 10.0);
    EXPECT_EQ(r->n_sta, 1);
}

TEST(LevelAuthorityKernel, UnavailableIsNotZero) {
    EXPECT_FALSE(levelAuthorityReduce({}, 4).has_value());
    Rows rows(1);
    rows[0] = {{5.0, 1.0}};   // one LOS < min_los
    EXPECT_FALSE(levelAuthorityReduce(rows, 4).has_value());
}

TEST(LevelAuthorityKernel, MadIsRobustDispersion) {
    // Five stations, four at 10±1 and one at 60: the MAD ignores the tail
    // where an rms would not.
    Rows rows(5);
    const double levels[5] = {9.0, 10.0, 10.0, 11.0, 60.0};
    for (int s = 0; s < 5; ++s)
        for (int k = 0; k < 4; ++k) rows[s].push_back({levels[s], 1.0});
    const auto r = levelAuthorityReduce(rows, 4);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->level_tecu, 10.0);
    EXPECT_LT(r->mad_tecu, 3.0);
}
