/**
 * @file test_network_apriori_track_phase_clock_hold.cpp
 * @brief The phase-clock hold on the tracked broadcast a priori
 *        (`network.apriori_clock_track_phase_clock_hold`): the loader's accept and refuse cases.
 *
 * Under `network.apriori_source sp3_brdc_clock` the loader requires the smoothed hold and
 * refuses the phase-clock hold (`network.emit_pb_hold_phase_clock`).  With the tracked a priori
 * the key admits the phase-clock hold in place of the smoothed hold and requires it.
 *
 * Covered (the loader logs a refusal and returns false, so each refusal is paired with a control
 * that differs from it only in the condition under test and loads):
 *   - off by default;
 *   - accepted with the track, the two-way centring and the phase-clock hold;
 *   - refused without the track (on the CODE a priori, whose control loads without the key);
 *   - refused when the hold under the key is not the phase-clock hold;
 *   - without the key the phase-clock hold stays refused under the broadcast a priori.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include "../support/TestTempDir.hpp"

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// A two-station GPS network with the IF-TD test on the a priori @p apriori; @p network_extra
/// and @p slip_extra are spliced into the network section and its cycle_slip block.
std::string networkConfig(const std::string& apriori, const std::string& network_extra,
                          const std::string& slip_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"apriori_source\": \"" + apriori + "\",\n"
          "    \"cycle_slip\": { \"if_phase_td\": true" + slip_extra + " },\n"
          + network_extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The same network on the broadcast a priori.
std::string brdcConfig(const std::string& network_extra, const std::string& slip_extra) {
    return networkConfig("sp3_brdc_clock", network_extra, slip_extra);
}

const std::string kCentre = ", \"if_phase_td_sat_centre\": true";
const std::string kTrack  = "    \"apriori_clock_track\": true,\n";
const std::string kKey    = "    \"apriori_clock_track_phase_clock_hold\": true,\n";
const std::string kLah    = "    \"emit_pb_hold\": true, \"emit_pb_hold_phase_clock\": true,\n";

/// Loads @p body; true on success (the loader logs a refusal and returns false).
bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = gnss_test::tempDir() / ("apriori_track_pc_hold_cfg_" + tag + ".json");
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

TEST(AprioriTrackPhaseClockHold, ConfigDefaultsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("default", networkConfig("sp3", "", ""), cfg));
    EXPECT_FALSE(cfg.networkAprioriClockTrackPhaseClockHold());
}

TEST(AprioriTrackPhaseClockHold, AcceptsThePhaseClockHoldOnTheTrack) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("accept", brdcConfig(kTrack + kKey + kLah, kCentre), cfg));
    EXPECT_TRUE(cfg.networkAprioriClockTrackPhaseClockHold());
    EXPECT_TRUE(cfg.networkAprioriClockTrack());
    EXPECT_TRUE(cfg.getNetworkConfig().emit_pb_hold_phase_clock);
    EXPECT_FALSE(cfg.getNetworkConfig().emit_pb_hold_smoothed);
}

TEST(AprioriTrackPhaseClockHold, RefusesTheKeyWithoutTheTrack) {
    // On the CODE a priori with the phase-clock hold the network loads; the key alone, with no
    // track to admit the hold on, refuses it.
    GnssConfig control, cfg;
    ASSERT_TRUE(loadConfig("no_track_control", networkConfig("sp3", kLah, ""), control));
    EXPECT_FALSE(loadConfig("no_track", networkConfig("sp3", kKey + kLah, ""), cfg));
}

TEST(AprioriTrackPhaseClockHold, RefusesAHoldOtherThanThePhaseClockHold) {
    // The plain hold under the key (the accepted case less emit_pb_hold_phase_clock): the key
    // admits the phase-clock hold in place of the smoothed hold and requires it.
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("plain_hold",
                            brdcConfig(kTrack + kKey + "    \"emit_pb_hold\": true,\n", kCentre), cfg));
}

TEST(AprioriTrackPhaseClockHold, WithoutTheKeyThePhaseClockHoldStaysRefused) {
    // The accepted case less the key.
    GnssConfig cfg;
    EXPECT_FALSE(loadConfig("no_key", brdcConfig(kTrack + kLah, kCentre), cfg));
}
