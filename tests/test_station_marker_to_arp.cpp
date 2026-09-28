/**
 * @file test_station_marker_to_arp.cpp
 * @brief The roster-site marker -> antenna-reference-point reduction
 *        (`network.station_marker_to_arp`, card UPO).
 *
 * A network station is held fixed at `network.stations[].position_ecef`, a
 * published coordinate (IGS SSC/SINEX, NGS SIF, a surveyed RINEX header) that by
 * convention references the monument.  The observations refer to the antenna,
 * and the ATX phase-centre model is defined relative to the antenna reference
 * point, `ANTENNA: DELTA H/E/N` above the monument.  The reduction is the same
 * `Coordinates::markerToArp` call that reduces the held-out consumer's truth, so
 * the two paths agree exactly for the same inputs and the producer and the
 * grader share one frame.
 *
 * Covered here (spec docs/refound/UPO_consumer_up_offset_2026-09-22.md §4, U4):
 *
 *   1. the roster-site reduction takes the header fields in RINEX (H, E, N)
 *      order, at a real roster site with a large eccentricity (GRAZ00AUT,
 *      1.964 m) and at one with a three-component eccentricity (KRGG00ATF);
 *   2. it is a bitwise no-op at zero eccentricity, which keeps an all-zero
 *      roster (the sim bed) byte-identical when armed;
 *   3. it equals the consumer-truth path for the same inputs, argument for
 *      argument;
 *   4. an absent `ANTENNA: DELTA H/E/N` record is distinguishable from a
 *      recorded 0/0/0 one (`RinexParser::hasAntennaDeltaHen`): the first is a
 *      coordinate that cannot be reduced (and is reported), the second a
 *      reduction that is a no-op;
 *   5. the toggle defaults off and parses from the nested
 *      `network.station_marker_to_arp` key.  The loader also accepts the flat
 *      `network_station_marker_to_arp` spelling, which is not exercised here.
 *
 * Suite: StationMarkerToArpTest (one suite per file, per the suite ownership rule).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

#include <Eigen/Dense>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// GRAZ00AUT (2024-129 field roster): configured ITRF2020 monument coordinate
/// and its RINEX header eccentricity, the largest on the roster.
const Eigen::Vector3d kGrazMonument(4194423.4920, 1162703.0403, 4647245.6154);
const Eigen::Vector3d kGrazHen(1.9640, 0.0000, 0.0000);

/// KRGG00ATF: the second largest, and the largest with all three components
/// populated (so an H/E/N transposition cannot hide behind a pure-up vector).
const Eigen::Vector3d kKrggMonument(1406336.1576, 3918154.5954, -4816172.8992);
const Eigen::Vector3d kKrggHen(1.0332, 0.0034, 0.0004);

/// The reduction exactly as ApplicationNetworkSetup applies it to a roster
/// station: the configured coordinate and the parser's H/E/N, in that order.
Eigen::Vector3d rosterSitePosition(const Eigen::Vector3d& configured,
                                   const Eigen::Vector3d& hen) {
    return Coordinates::markerToArp(configured, hen);
}

/// A minimal RINEX 3.04 header, optionally carrying an `ANTENNA: DELTA H/E/N`
/// record.  One epoch of one satellite so the parse succeeds.
fs::path writeHeaderFixture(const fs::path& dir, const std::string& tag,
                            bool with_delta, const char* delta_line) {
    const auto path = dir / ("marker_arp_" + tag + "_30S_MO.rnx");
    std::ofstream f(path);
    f << "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
         "MARP                                                        MARKER NAME\n"
         "unit test           unit test                               OBSERVER / AGENCY\n"
         "00000               TESTRX              0.0                 REC # / TYPE / VERS\n"
         "00000               TESTANT     NONE                        ANT # / TYPE\n"
         "  4194423.4920  1162703.0403  4647245.6154                  APPROX POSITION XYZ\n";
    if (with_delta) f << delta_line;
    f << "G    2 C1C L1C                                              SYS / # / OBS TYPES\n"
         "    30.000                                                  INTERVAL\n"
         "  2024     5     8     0     0    0.0000000     GPS         TIME OF FIRST OBS\n"
         "                                                            END OF HEADER\n"
         "> 2024 05 08 00 00  0.0000000  0  1\n"
         "G07  25116862.208  131990016.320\n";
    return path;
}

/// A PPP config pointed at `obs`, loaded through the real JSON loader.
void loadObsConfig(GnssConfig& cfg, LogManager& lm, const fs::path& dir,
                   const fs::path& obs, const std::string& tag) {
    const auto jpath = dir / ("marker_arp_" + tag + ".json");
    std::ofstream j(jpath);
    std::string obs_fwd = obs.string();
    for (char& c : obs_fwd) if (c == '\\') c = '/';
    j << "{\n"
      << "  \"processing\": { \"mode\": \"ppp\" },\n"
      << "  \"files\": { \"observation_path\": \"" << obs_fwd << "\" }\n"
      << "}\n";
    j.close();
    ASSERT_TRUE(cfg.loadFromJson(jpath, lm)) << "config load failed";
}

/// A network config carrying `network_extra` inside the `network` object.
fs::path writeNetworkConfig(const std::string& tag, const std::string& network_extra) {
    const fs::path p = fs::temp_directory_path() / ("marker_arp_net_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
         "  },\n"
         "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
      << network_extra <<
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    f.close();
    return p;
}

} // namespace

// (1) Order: the header's first field is up.  A roster site reduced by GRAZ's
// 1.964 m eccentricity gains that much geodetic height and does not move
// horizontally; a transposed (H, E, N) would move it east or north instead.
TEST(StationMarkerToArpTest, RosterSiteReductionTakesHenInRinexOrder) {
    const Eigen::Vector3d arp = rosterSitePosition(kGrazMonument, kGrazHen);

    const Coordinates::Llh m = Coordinates::ecefToLlh(kGrazMonument);
    const Coordinates::Llh a = Coordinates::ecefToLlh(arp);
    EXPECT_NEAR(a.h_m - m.h_m, kGrazHen(0), 1e-6);
    EXPECT_NEAR(a.lat_rad, m.lat_rad, 1e-12);
    EXPECT_NEAR(a.lon_rad, m.lon_rad, 1e-12);
    EXPECT_NEAR((arp - kGrazMonument).norm(), kGrazHen(0), 1e-9);

    // All three components, projected back into the local frame at the monument.
    const Eigen::Vector3d krgg = rosterSitePosition(kKrggMonument, kKrggHen);
    const Eigen::Vector3d enu =
        Coordinates::ecefToEnu(krgg - kKrggMonument, Coordinates::ecefToLlh(kKrggMonument));
    EXPECT_NEAR(enu.x(), kKrggHen(1), 1e-9);   // E is header field 2
    EXPECT_NEAR(enu.y(), kKrggHen(2), 1e-9);   // N is header field 3
    EXPECT_NEAR(enu.z(), kKrggHen(0), 1e-9);   // U is header field 1
}

// (1b) Direction: the antenna sits above the monument, so a station reduced to
// its ARP is held higher, never lower.  This sign sets the direction of the
// consumer's standing Up offset that card UPO addresses.
TEST(StationMarkerToArpTest, ReductionRaisesTheStation) {
    const Eigen::Vector3d arp = rosterSitePosition(kGrazMonument, kGrazHen);
    EXPECT_GT(Coordinates::ecefToLlh(arp).h_m, Coordinates::ecefToLlh(kGrazMonument).h_m);
    EXPECT_GT(arp.norm(), kGrazMonument.norm());
}

// (2) Byte identity: a station whose header records 0/0/0 (every sim-bed
// station) comes out of the armed path bitwise unchanged, so arming the knob
// does not perturb a roster with no eccentricities.
TEST(StationMarkerToArpTest, ZeroEccentricityIsExactNoOp) {
    const Eigen::Vector3d zero = Eigen::Vector3d::Zero();
    const Eigen::Vector3d out = rosterSitePosition(kGrazMonument, zero);
    EXPECT_EQ(out.x(), kGrazMonument.x());
    EXPECT_EQ(out.y(), kGrazMonument.y());
    EXPECT_EQ(out.z(), kGrazMonument.z());
}

// (3) The roster path and the consumer-truth path are the same reduction.  The
// consumer's truth is reduced in ApplicationNetworkSetup's closed-loop block
// with `Coordinates::markerToArp(configured, parser->getAntennaDeltaHen())`; the
// roster reduction is that call, argument for argument, so the producer and its
// grader share one frame.
TEST(StationMarkerToArpTest, AgreesWithTheConsumerTruthPath) {
    for (const auto& c : { std::pair<Eigen::Vector3d, Eigen::Vector3d>{ kGrazMonument, kGrazHen },
                           std::pair<Eigen::Vector3d, Eigen::Vector3d>{ kKrggMonument, kKrggHen },
                           std::pair<Eigen::Vector3d, Eigen::Vector3d>{ kGrazMonument,
                                                                        Eigen::Vector3d::Zero() } }) {
        // The consumer-truth path, verbatim.
        Eigen::Vector3d truth = c.first;
        truth = Coordinates::markerToArp(truth, c.second);
        const Eigen::Vector3d roster = rosterSitePosition(c.first, c.second);
        EXPECT_EQ(roster.x(), truth.x());
        EXPECT_EQ(roster.y(), truth.y());
        EXPECT_EQ(roster.z(), truth.z());
    }
}

// (4) An absent record is not a zero record.  Both leave getAntennaDeltaHen at
// zero, so the reduction cannot tell them apart; hasAntennaDeltaHen lets the
// setup report the station it could not reduce (requirement U1).
TEST(StationMarkerToArpTest, AbsentHeaderRecordIsDistinguishableFromZero) {
    const auto dir = fs::temp_directory_path() / "gnss_marker_arp";
    fs::create_directories(dir);

    struct Case { const char* tag; bool with_delta; const char* line; bool expect_has; };
    const Case cases[] = {
        { "none", false, "", false },
        { "zero", true,
          "        0.0000        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n", true },
        { "graz", true,
          "        1.9640        0.0000        0.0000                  ANTENNA: DELTA H/E/N\n", true },
    };
    for (const auto& c : cases) {
        const auto obs = writeHeaderFixture(dir, c.tag, c.with_delta, c.line);
        LogManager lm;
        GnssConfig cfg;
        loadObsConfig(cfg, lm, dir, obs, c.tag);
        ProductParser pp(cfg, lm);
        RinexParser rp(cfg, lm, pp);
        ASSERT_TRUE(rp.parse()) << c.tag;
        EXPECT_EQ(rp.hasAntennaDeltaHen(), c.expect_has) << c.tag;
        if (!c.expect_has || std::string(c.tag) == "zero") {
            EXPECT_EQ(rp.getAntennaDeltaHen().norm(), 0.0) << c.tag;
            // ...and the reduction is the identity in both of those cases.
            const Eigen::Vector3d out =
                rosterSitePosition(kGrazMonument, rp.getAntennaDeltaHen());
            EXPECT_EQ(out.x(), kGrazMonument.x()) << c.tag;
        } else {
            EXPECT_NEAR(rp.getAntennaDeltaHen()(0), 1.9640, 1e-9);
        }
        std::error_code ec;
        fs::remove(obs, ec);
    }
}

// (5) The toggle: off by default (byte-identical), and it parses from the dotted
// `network.station_marker_to_arp` spelling the configs use.
TEST(StationMarkerToArpTest, ToggleDefaultsOffAndParses) {
    {
        LogManager log;
        const fs::path p = writeNetworkConfig("default", "");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkStationMarkerToArp());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log;
        const fs::path p = writeNetworkConfig("on", "    \"station_marker_to_arp\": true,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkStationMarkerToArp());
        std::error_code ec; fs::remove(p, ec);
    }
}
