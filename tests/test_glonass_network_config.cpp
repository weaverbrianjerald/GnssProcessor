/**
 * @file test_glonass_network_config.cpp
 * @brief Config-contract tests for GLONASS in the PPP-RTK network producer.
 *
 * The network admits GLONASS (code-only: satellite clock + per-FCN-slot receiver
 * IFB) via network.estimate_glonass.  Because the per-slot IFB is the mechanism
 * that keeps GLONASS observations from pinning the receiver clock, the flag MUST
 * force its prerequisites (estimate_ifb + enable_glonass_ifb) on and MUST require
 * GLONASS in active_systems.  These tests lock that contract.
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

// Write @p body to a unique temp file and return its path.  @p tag disambiguates
// concurrent test files (Date/random helpers are unavailable in this harness).
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() /
                       ("glonass_net_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// A minimal but loadable PPP-RTK network config.  @p active is the active_systems
// JSON array literal and @p network_extra is spliced into the network block.
std::string networkConfig(const std::string& active,
                          const std::string& signal_priorities,
                          const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": " + active + ",\n"
        "    \"signal_priorities\": " + signal_priorities + "\n"
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

constexpr const char* kGpsGalGlo = "[ \"GPS\", \"Galileo\", \"GLONASS\" ]";
constexpr const char* kGpsGal    = "[ \"GPS\", \"Galileo\" ]";
constexpr const char* kPriosGlo  =
    "{ \"GPS\": \"WC\", \"Galileo\": \"CB\", \"GLONASS\": \"CP\" }";
constexpr const char* kPriosGps  = "{ \"GPS\": \"WC\", \"Galileo\": \"CB\" }";

} // namespace

// estimate_glonass=true with GLONASS active forces the per-slot IFB prerequisites
// on (the network needs them to model GLONASS without corrupting GPS/Galileo).
TEST(GlonassNetworkConfig, EstimateGlonassForcesIfbPrerequisites) {
    LogManager log;
    const fs::path p = writeTempConfig("force",
        networkConfig(kGpsGalGlo, kPriosGlo, "    \"estimate_glonass\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.enableGlonassNetwork());
    EXPECT_TRUE(cfg.estimateIfb());        // forced on
    EXPECT_TRUE(cfg.enableGlonassIfb());   // forced on
    EXPECT_TRUE(cfg.isUncombinedEnabled()); // network family forces uncombined
    std::error_code ec; fs::remove(p, ec);
}

// estimate_glonass=true but GLONASS missing from active_systems is a hard error:
// GLONASS observations would never arrive, or would arrive with no shared clock
// column and pin the receiver clock — fail loud rather than silently misconfigure.
TEST(GlonassNetworkConfig, EstimateGlonassRequiresGlonassInActiveSystems) {
    LogManager log;
    const fs::path p = writeTempConfig("missing",
        networkConfig(kGpsGal, kPriosGps, "    \"estimate_glonass\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// estimate_glonass=true with GLONASS in active_systems but NO GLONASS
// signal_priorities entry is a hard error: PppModel would find no priority string
// and silently skip every GLONASS satellite at signal selection, so the network
// would emit ZERO GLONASS products while looking nominal.  Fail loud at load
// (the multi-GNSS signal_priorities silent-drop failure mode).
TEST(GlonassNetworkConfig, EstimateGlonassRequiresGlonassSignalPriorities) {
    LogManager log;
    const fs::path p = writeTempConfig("noprio",
        networkConfig(kGpsGalGlo, kPriosGps, "    \"estimate_glonass\": true,\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// Default network config (no estimate_glonass): GLONASS is OFF and the per-slot
// IFB prerequisites are NOT force-enabled — the GPS+Galileo network is unchanged.
TEST(GlonassNetworkConfig, DefaultNetworkHasGlonassOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default",
        networkConfig(kGpsGal, kPriosGps, ""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.enableGlonassNetwork());
    EXPECT_FALSE(cfg.estimateIfb());        // not forced
    EXPECT_FALSE(cfg.enableGlonassIfb());   // not forced
    std::error_code ec; fs::remove(p, ec);
}
