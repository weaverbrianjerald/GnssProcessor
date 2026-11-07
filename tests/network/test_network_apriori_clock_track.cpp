/**
 * @file test_network_apriori_clock_track.cpp
 * @brief The tracked broadcast a priori (`network.apriori_clock_track`): the pure update
 *        `aprioriClockTrackApply` and the loader's refusals.
 *
 * Each epoch a satellite's track U moves by minus its IF-TD column (the satellite-common
 * between-epoch change of its phase against the a priori), and every modelled observation of
 * the satellite at every station is lowered by U, so the solve works against a + U.
 *
 * Covered:
 *   - a satellite seen for the first time starts its track at minus its column, and every
 *     modelled value of it (code and phase, every station) moves by that amount;
 *   - columns accumulate over epochs; a satellite without a column holds its track, is counted
 *     paused, and resumes with the number of epochs it was paused;
 *   - a satellite never tracked is not moved;
 *   - the increment ladder and the largest |U|;
 *   - the loader: off by default, refused without sp3_brdc_clock, refused without the two-way
 *     centring.
 */

#include <gtest/gtest.h>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/AprioriClockTrack.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"

using namespace GnssProcessor;

namespace {

PppObservation obs(const std::string& sat, double code, double phase) {
    PppObservation o{};
    o.satellite = sat;
    o.modeled["C1C"] = code;
    o.modeled["L1C"] = phase;
    return o;
}

/// Two stations, each seeing G05 and G07 (station 1 also G09).
std::vector<std::vector<PppObservation>> epochRows() {
    return { { obs("G05", 100.0, 10.0), obs("G07", 200.0, 20.0) },
             { obs("G05", 101.0, 11.0), obs("G07", 201.0, 21.0), obs("G09", 300.0, 30.0) } };
}

}  // namespace

TEST(AprioriClockTrack, AFirstColumnStartsTheTrackAndMovesEveryRowOfTheSatellite) {
    std::map<std::string, double> track;
    std::map<std::string, int> paused;
    auto rows = epochRows();
    const auto rec = aprioriClockTrackApply({ { "G05", -0.03 } }, track, paused, rows);
    ASSERT_EQ(track.count("G05"), 1u);
    EXPECT_DOUBLE_EQ(track.at("G05"), 0.03);
    EXPECT_EQ(rec.applied, 1);
    ASSERT_EQ(rec.started.size(), 1u);
    EXPECT_EQ(rec.started[0], "G05");
    // A satellite clock larger by U lowers the modelled range by U, code and phase alike.
    EXPECT_DOUBLE_EQ(rows[0][0].modeled.at("C1C"), 100.0 - 0.03);
    EXPECT_DOUBLE_EQ(rows[0][0].modeled.at("L1C"), 10.0 - 0.03);
    EXPECT_DOUBLE_EQ(rows[1][0].modeled.at("C1C"), 101.0 - 0.03);
    EXPECT_DOUBLE_EQ(rows[1][0].modeled.at("L1C"), 11.0 - 0.03);
    EXPECT_DOUBLE_EQ(rows[0][1].modeled.at("C1C"), 200.0) << "G07 has no track";
    EXPECT_DOUBLE_EQ(rows[1][2].modeled.at("L1C"), 30.0) << "G09 has no track";
    EXPECT_EQ(rec.rows_moved, 4);
}

TEST(AprioriClockTrack, ColumnsAccumulateAndAPausedSatelliteHoldsItsTrackThenResumes) {
    std::map<std::string, double> track;
    std::map<std::string, int> paused;
    auto r1 = epochRows();
    aprioriClockTrackApply({ { "G05", -0.03 }, { "G07", 0.01 } }, track, paused, r1);
    auto r2 = epochRows();
    const auto e2 = aprioriClockTrackApply({ { "G05", -0.02 } }, track, paused, r2);
    EXPECT_NEAR(track.at("G05"), 0.05, 1e-15);
    EXPECT_NEAR(track.at("G07"), -0.01, 1e-15) << "held without a column";
    EXPECT_EQ(e2.applied, 1);
    EXPECT_EQ(e2.paused, 1);
    EXPECT_NEAR(r2[0][1].modeled.at("C1C"), 200.0 + 0.01, 1e-12) << "a held track still moves its rows";
    auto r3 = epochRows();
    aprioriClockTrackApply({ { "G05", 0.0 } }, track, paused, r3);
    auto r4 = epochRows();
    const auto e4 = aprioriClockTrackApply({ { "G07", 0.02 } }, track, paused, r4);
    ASSERT_EQ(e4.resumed.count("G07"), 1u);
    EXPECT_EQ(e4.resumed.at("G07"), 2) << "paused over epochs 2 and 3";
    EXPECT_NEAR(track.at("G07"), -0.03, 1e-15);
    EXPECT_TRUE(e4.started.empty());
}

TEST(AprioriClockTrack, TheLadderAndTheLargestTrack) {
    std::map<std::string, double> track;
    std::map<std::string, int> paused;
    auto rows = epochRows();
    const auto rec = aprioriClockTrackApply(
        { { "G01", 0.002 }, { "G02", -0.007 }, { "G03", 0.015 }, { "G04", -0.03 }, { "G05", 0.07 },
          { "G06", -0.15 } },
        track, paused, rows);
    for (std::size_t k = 0; k < rec.inc_hist.size(); ++k) EXPECT_EQ(rec.inc_hist[k], 1) << k;
    EXPECT_DOUBLE_EQ(rec.max_abs_track_m, 0.15);
}

namespace {

namespace fs = std::filesystem;

std::string trackConfig(const std::string& network_extra, const std::string& slip_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"cycle_slip\": { \"if_phase_td\": true" + slip_extra + " },\n"
          + network_extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("apriori_track_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    bool ok = false;
    try {
        ok = cfg.loadFromJson(p, log);
    } catch (const std::exception&) {
        ok = false;
    }
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

}  // namespace

TEST(AprioriClockTrack, ConfigDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", trackConfig("", ""), cfg));
    EXPECT_FALSE(cfg.networkAprioriClockTrack());
}

TEST(AprioriClockTrack, ConfigRefusesTheTrackWithoutTheBroadcastAPriori) {
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("no_brdc", trackConfig("    \"apriori_clock_track\": true,\n",
                                                   ", \"if_phase_td_sat_centre\": true"), cfg));
}

TEST(AprioriClockTrack, ConfigRefusesTheTrackWithoutTheTwoWayCentring) {
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("no_centre",
        trackConfig("    \"apriori_source\": \"sp3_brdc_clock\",\n"
                    "    \"emit_pb_hold\": true, \"emit_pb_hold_smoothed\": true,\n"
                    "    \"apriori_clock_track\": true,\n", ""), cfg));
}
