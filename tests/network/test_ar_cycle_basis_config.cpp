/**
 * @file test_ar_cycle_basis_config.cpp
 * @brief Loader tests of the producer AR search's cycle basis (card AGT, step AGT-5):
 *        @c network.ambiguity_resolution.cycle_basis and @c .cycle_basis_sticky.
 *
 * Suite @c ArCycleBasisConfig:
 *  - both knobs default to the unarmed posture (off, sticky);
 *  - the interlocks: the cycle basis needs tree_basis, decorrelated_partial and block_search,
 *    and the loader forces it off when any of the three is missing;
 *  - the trace's four armed-only columns, in the order the seam writes them.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

std::string arConfig(const std::string& ar_body) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"ambiguity_resolution\": { " + ar_body + " },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// Load @p ar_body as network.ambiguity_resolution and return the parsed block.
NetworkArConfig load(const std::string& tag, const std::string& ar_body) {
    LogManager log;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / ("ar_cycle_basis_cfg_" + tag + ".json");
    { std::ofstream f(p); f << arConfig(ar_body); }
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec;
    fs::remove(p, ec);
    return cfg.getNetworkConfig().ambiguity_resolution;
}

const std::string kPrereq =
    "\"enable\": true, \"tree_basis\": true, \"decorrelated_partial\": true, \"block_search\": true";

TEST(ArCycleBasisConfig, DefaultsAreOffAndSticky) {
    const NetworkArConfig ar = load("default", "\"enable\": true");
    EXPECT_FALSE(ar.cycle_basis);
    EXPECT_TRUE(ar.cycle_basis_sticky);
}

TEST(ArCycleBasisConfig, ArmsWithItsThreePrerequisites) {
    const NetworkArConfig ar = load("armed", kPrereq + ", \"cycle_basis\": true");
    EXPECT_TRUE(ar.cycle_basis);
    EXPECT_TRUE(ar.cycle_basis_sticky);
    const NetworkArConfig ctl =
        load("control", kPrereq + ", \"cycle_basis\": true, \"cycle_basis_sticky\": false");
    EXPECT_TRUE(ctl.cycle_basis);
    EXPECT_FALSE(ctl.cycle_basis_sticky);
}

TEST(ArCycleBasisConfig, IsForcedOffWithoutAnyPrerequisite) {
    const char* const prereqs[] = {"tree_basis", "decorrelated_partial", "block_search"};
    for (const char* missing : prereqs) {
        std::string body = "\"enable\": true, \"cycle_basis\": true";
        for (const char* k : prereqs)
            if (std::string(k) != missing) body += std::string(", \"") + k + "\": true";
        const NetworkArConfig ar = load(std::string("no_") + missing, body);
        EXPECT_FALSE(ar.cycle_basis) << "without " << missing;
    }
    // The prerequisites alone do not arm it.
    EXPECT_FALSE(load("prereq_only", kPrereq).cycle_basis);
}

TEST(ArCycleBasisConfig, TheTraceGainsFourArmedOnlyColumns) {
    EXPECT_STREQ(kProducerArCycleBasisColumns, ",cycle_kind,band_only,basis_age_ep,map_state");
    // Appended after the suspect-mask columns, before the newline, on either header.
    std::string h = kProducerArHeaderDecorrelated;
    h.insert(h.size() - 1, kProducerArSuspectColumns);
    h.insert(h.size() - 1, kProducerArCycleBasisColumns);
    EXPECT_NE(h.find("nl_cond_sigma_cyc,suspect_bits,reentry,cycle_kind,band_only,basis_age_ep,map_state\n"),
              std::string::npos);
}

}  // namespace
}  // namespace GnssProcessor
