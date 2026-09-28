/**
 * @file test_rinex_zero_sentinel.cpp
 * @brief RINEX 3 written-0.0 sentinel guard (docs/SPEC_rinex_zero_sentinel.md).
 *
 * Per RINEX 3.0x §6, missing observations are written "as 0.0 or blanks"; some
 * receivers (Trimble ALLOY on DOY-129: GOPE/GOP6) write literal 0.0 phase
 * fields alongside valid code for weak-signal tracking.  The 2.x branch skips
 * zeros; the 3.x branch ingests them as real 0-cycle phases unless the guard
 * (@c processing.rinex_skip_zero_values) is on, which makes it match the
 * standard.  The guard is off by default, so legacy output stays byte-identical.
 *
 * Suite: RinexZeroSentinelTest (one suite per file, per the ownership rule).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

using namespace GnssProcessor;

namespace {

/// Minimal RINEX 3.04 observation file: two GPS satellites, two epochs.
/// G05 carries a written-0.0 L1C (missing-phase sentinel) with valid C1C;
/// G07 is fully valid.  Observation slots are 16 chars wide (F14.3 value + LLI +
/// SNR), built programmatically so the column positions stay correct.
std::filesystem::path writeFixture(const std::filesystem::path& dir) {
    const auto path = dir / "zero_sentinel_G_30S_MO.rnx";
    auto fld = [](double v) {
        char b[24];
        std::snprintf(b, sizeof b, "%14.3f  ", v);
        return std::string(b);   // 16 chars: value + blank LLI + blank SNR
    };
    auto obsline = [&](const char* sat, double c1, double l1, double c2, double l2) {
        return std::string(sat) + fld(c1) + fld(l1) + fld(c2) + fld(l2) + "\n";
    };
    std::ofstream f(path);
    f << "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
         "ZERO                                                        MARKER NAME\n"
         "unit test           unit test                               OBSERVER / AGENCY\n"
         "00000               TESTRX              0.0                 REC # / TYPE / VERS\n"
         "00000               TESTANT     NONE                        ANT # / TYPE\n"
         "  3967283.1000   996406.2000  4970111.4000                  APPROX POSITION XYZ\n"
         "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n"
         "G    4 C1C L1C C2W L2W                                      SYS / # / OBS TYPES\n"
         "    30.000                                                  INTERVAL\n"
         "  2024     5     8    19    25    0.0000000     GPS         TIME OF FIRST OBS\n"
         "                                                            END OF HEADER\n"
         "> 2024 05 08 19 25  0.0000000  0  2\n"
      << obsline("G05", 20917532.875, 0.0, 20917538.965, 85654164.390)
      << obsline("G07", 25116862.208, 131990016.320, 25116868.561, 102849366.694)
      << "> 2024 05 08 19 25 30.0000000  0  2\n"
      << obsline("G05", 20898954.773, 0.0, 20898961.656, 85578091.808)
      << obsline("G07", 25091025.407, 131854233.571, 25091029.026, 102743562.187);
    return path;
}

/// Build a config whose observation path is the fixture, with the guard armed
/// or not.  Loaded through the real JSON loader so the toggle wiring is tested.
void loadConfig(GnssConfig& cfg, LogManager& lm, const std::filesystem::path& dir,
                const std::filesystem::path& obs, bool guard) {
    const auto jpath = dir / (guard ? "zero_on.json" : "zero_off.json");
    std::ofstream j(jpath);
    std::string obs_fwd = obs.string();
    for (char& c : obs_fwd) if (c == '\\') c = '/';
    j << "{\n"
      << "  \"processing\": { \"mode\": \"ppp\", \"rinex_skip_zero_values\": "
      << (guard ? "true" : "false") << " },\n"
      << "  \"files\": { \"observation_path\": \"" << obs_fwd << "\" }\n"
      << "}\n";
    j.close();
    ASSERT_TRUE(cfg.loadFromJson(jpath, lm)) << "config load failed";
}

const RinexObservation* findObs(const std::vector<RinexObservation>& v,
                                const std::string& sat, int epoch_idx) {
    int seen = -1;
    GnssTime last{};
    for (const auto& o : v) {
        if (o.time.week != last.week || o.time.tow != last.tow) {
            ++seen;
            last = o.time;
        }
        if (seen == epoch_idx && o.satellite == sat) return &o;
    }
    return nullptr;
}

} // namespace

TEST(RinexZeroSentinelTest, LegacyPathIngestsWrittenZeros) {
    const auto dir = std::filesystem::temp_directory_path() / "gnss_zero_sentinel";
    std::filesystem::create_directories(dir);
    const auto obs = writeFixture(dir);
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs, /*guard=*/false);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    ASSERT_TRUE(rp.parse());
    const auto* g05 = findObs(rp.getObservations(), "G05", 0);
    ASSERT_NE(g05, nullptr);
    // Guard off: the written 0.0 arrives as a real measurement (the defect).
    ASSERT_TRUE(g05->measurements.count("L1C"));
    EXPECT_EQ(g05->measurements.at("L1C"), 0.0);
    EXPECT_TRUE(g05->measurements.count("C1C"));
}

TEST(RinexZeroSentinelTest, GuardSkipsZerosKeepsValidPartners) {
    const auto dir = std::filesystem::temp_directory_path() / "gnss_zero_sentinel";
    std::filesystem::create_directories(dir);
    const auto obs = writeFixture(dir);
    LogManager lm;
    GnssConfig cfg;
    loadConfig(cfg, lm, dir, obs, /*guard=*/true);
    ProductParser pp(cfg, lm);
    RinexParser rp(cfg, lm, pp);
    ASSERT_TRUE(rp.parse());
    const auto& v = rp.getObservations();
    for (int ep = 0; ep < 2; ++ep) {
        const auto* g05 = findObs(v, "G05", ep);
        ASSERT_NE(g05, nullptr) << "epoch " << ep;
        EXPECT_FALSE(g05->measurements.count("L1C"))
            << "epoch " << ep << ": written 0.0 must be treated as missing";
        EXPECT_TRUE(g05->measurements.count("C1C"));   // valid partner survives
        EXPECT_TRUE(g05->measurements.count("L2W"));   // valid phase survives
    }
    // Fully-valid satellite untouched, values identical to the file.
    const auto* g07 = findObs(v, "G07", 0);
    ASSERT_NE(g07, nullptr);
    ASSERT_TRUE(g07->measurements.count("L1C"));
    EXPECT_NEAR(g07->measurements.at("L1C"), 131990016.320, 1e-3);
}
