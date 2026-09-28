/**
 * @file test_sat_health.cpp
 * @brief Card HLT: broadcast satellite health on the network path (network.sat_health).
 *
 * The health series (a record takes force at its clock epoch; each Galileo message source
 * keeps its own record in force; the health word is masked to the processed bands), the
 * clock-pivot pick that passes over an unhealthy satellite, and the synced-epoch door that
 * removes an unhealthy satellite's observations.
 */
#include <gtest/gtest.h>

#include "filter/NetworkSbasis.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr int    kWeek = 2313;
constexpr double kDay  = 3.0 * 86400.0;   // Wednesday 2024-05-08 00:00 GPS, seconds of week

double gpsSeconds(double sow) { return kWeek * 604800.0 + sow; }

BroadcastEphemeris rec(const std::string& sat, double toc_sow, int health,
                       int data_source = -1) {
    BroadcastEphemeris e;
    e.satellite = sat;
    e.system = sat[0] == 'E'   ? System::Galileo
             : sat[0] == 'R'   ? System::GLONASS
             : sat[0] == 'C'   ? System::BeiDou
                               : System::GPS;
    e.health = health;
    e.data_source = data_source;
    e.kepler.toc = GnssTime{kWeek, toc_sow};
    e.kepler.toe = GnssTime{kWeek, toc_sow};
    return e;
}

constexpr int kFnav = 258;          // F/NAV E5a-I, clock (E5a, E1)
constexpr int kInav = 517;          // I/NAV E1-B + E5b-I, clock (E5b, E1)
constexpr int kE1E5aMask = 0x03F;   // the dual-frequency Galileo posture's bands

RinexObservation obsOf(const std::string& sat) {
    RinexObservation o;
    o.satellite = sat;
    o.system = sat[0] == 'E' ? System::Galileo : System::GPS;
    return o;
}

}  // namespace

// A record takes force at its clock epoch: before the first record the earliest record
// decides, and each later record decides from its own time on.
TEST(SatHealth, RecordInForceDecidesTheStatus) {
    const std::vector<BroadcastEphemeris> nav = {
        rec("G05", kDay + 7200.0, 0), rec("G05", kDay + 14400.0, 1),
        rec("G05", kDay + 21600.0, 0)};
    const auto H = NetworkSbasis::satHealthSeries(nav, kE1E5aMask);
    ASSERT_EQ(H.sats.at("G05").size(), 3u);
    EXPECT_FALSE(H.unhealthyAt("G05", gpsSeconds(kDay)));
    EXPECT_FALSE(H.unhealthyAt("G05", gpsSeconds(kDay + 14399.0)));
    EXPECT_TRUE(H.unhealthyAt("G05", gpsSeconds(kDay + 14400.0)));
    EXPECT_TRUE(H.unhealthyAt("G05", gpsSeconds(kDay + 21599.0)));
    EXPECT_FALSE(H.unhealthyAt("G05", gpsSeconds(kDay + 21600.0)));
    EXPECT_EQ(H.records, 3);
    EXPECT_EQ(H.records_unhealthy, 1);
}

// DOY 129's GPS pivot: health 63 in every record is one unhealthy step for all time.
TEST(SatHealth, AllDayUnhealthySatelliteIsOneStep) {
    std::vector<BroadcastEphemeris> nav;
    for (int k = 0; k < 12; ++k) nav.push_back(rec("G01", kDay + 7200.0 * k, 63));
    const auto H = NetworkSbasis::satHealthSeries(nav, kE1E5aMask);
    ASSERT_EQ(H.sats.at("G01").size(), 1u);
    EXPECT_TRUE(H.unhealthyAt("G01", gpsSeconds(kDay - 3600.0)));
    EXPECT_TRUE(H.unhealthyAt("G01", gpsSeconds(kDay + 86399.0)));
}

// Records sharing a clock epoch keep file order and the last one decides.
TEST(SatHealth, LastRecordAtOneEpochDecides) {
    const std::vector<BroadcastEphemeris> nav = {
        rec("G07", kDay, 0), rec("G07", kDay + 7200.0, 0), rec("G07", kDay + 7200.0, 4)};
    const auto H = NetworkSbasis::satHealthSeries(nav, kE1E5aMask);
    EXPECT_FALSE(H.unhealthyAt("G07", gpsSeconds(kDay + 7199.0)));
    EXPECT_TRUE(H.unhealthyAt("G07", gpsSeconds(kDay + 7200.0)));
}

// E14 and E18 on DOY 129: F/NAV health 16 (E5a) and I/NAV health 130 (E1-B, E5b).  Each
// message source keeps its own record in force, so the E1-B bits that only I/NAV carries
// are read even though F/NAV records exist.
TEST(SatHealth, GalileoSourcesAreReadSeparately) {
    std::vector<BroadcastEphemeris> nav;
    for (int k = 0; k < 6; ++k) {
        nav.push_back(rec("E14", kDay + 600.0 * k, 16, kFnav));
        nav.push_back(rec("E14", kDay + 600.0 * k, 130, kInav));
        nav.push_back(rec("E30", kDay + 600.0 * k, 0, kFnav));
        nav.push_back(rec("E30", kDay + 600.0 * k, 0x180, kInav));   // E5b out of service
        nav.push_back(rec("E31", kDay + 600.0 * k, 0, kFnav));
        nav.push_back(rec("E31", kDay + 600.0 * k, 0x002, kInav));   // E1-B out of service
    }
    const auto H = NetworkSbasis::satHealthSeries(nav, kE1E5aMask);
    EXPECT_TRUE(H.unhealthyAt("E14", gpsSeconds(kDay + 1200.0)));
    EXPECT_FALSE(H.unhealthyAt("E30", gpsSeconds(kDay + 1200.0)));   // E5b is not processed
    EXPECT_TRUE(H.unhealthyAt("E31", gpsSeconds(kDay + 1200.0)));    // only I/NAV says so
    const auto all = NetworkSbasis::satHealthSeries(nav, 0x1FF);
    EXPECT_TRUE(all.unhealthyAt("E30", gpsSeconds(kDay + 1200.0)));
}

TEST(SatHealth, GalileoMaskFollowsTheProcessedBands) {
    EXPECT_EQ(NetworkSbasis::galileoHealthMask({"C1C", "C1X", "L1C", "C5Q", "L5Q"}), 0x03F);
    EXPECT_EQ(NetworkSbasis::galileoHealthMask({"C1C", "C7Q"}), 0x1C7);
    EXPECT_EQ(NetworkSbasis::galileoHealthMask({"C8Q"}), 0x1F8);
    EXPECT_EQ(NetworkSbasis::galileoHealthMask({"C6C"}), 0);
    EXPECT_EQ(NetworkSbasis::galileoHealthMask({}), 0);
}

TEST(SatHealth, NoRecordMeansHealthyAndGlonassIsSkipped) {
    const std::vector<BroadcastEphemeris> nav = {rec("R01", kDay, 1), rec("G02", kDay, 0)};
    const auto H = NetworkSbasis::satHealthSeries(nav, kE1E5aMask);
    EXPECT_EQ(H.sats.count("R01"), 0u);
    EXPECT_EQ(H.records, 1);
    EXPECT_FALSE(H.unhealthyAt("G99", gpsSeconds(kDay)));
    EXPECT_FALSE(H.unhealthyAt("R01", gpsSeconds(kDay)));
}

// With no ineligible set the pick is the rule as it was: declared, sticky incumbent,
// lexical first.
TEST(ClockPivotElection, WithoutHealthTheRuleIsUnchanged) {
    const std::vector<std::string> sats = {"G01", "G02", "G03"};
    EXPECT_EQ(NetworkSbasis::electClockPivot(sats, "", "", nullptr).sat, "G01");
    EXPECT_EQ(NetworkSbasis::electClockPivot(sats, "", "G03", nullptr).sat, "G03");
    EXPECT_EQ(NetworkSbasis::electClockPivot(sats, "", "G09", nullptr).sat, "G01");
    EXPECT_EQ(NetworkSbasis::electClockPivot(sats, "G02", "G03", nullptr).sat, "G02");
    EXPECT_EQ(NetworkSbasis::electClockPivot(sats, "G09", "", nullptr).sat, "G01");
    const auto p = NetworkSbasis::electClockPivot(sats, "", "", nullptr);
    EXPECT_FALSE(p.health_skipped);
    EXPECT_FALSE(p.health_fallback);
}

TEST(ClockPivotElection, UnhealthyLexicalFirstIsPassedOver) {
    const std::set<std::string> bad = {"G01"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02", "G03"}, "", "", &bad);
    EXPECT_EQ(p.sat, "G02");
    EXPECT_TRUE(p.health_skipped);
    EXPECT_FALSE(p.health_fallback);
}

TEST(ClockPivotElection, HealthyIncumbentIsKeptAndUnhealthyOneReplaced) {
    const std::vector<std::string> sats = {"G01", "G02", "G03"};
    const std::set<std::string> g01 = {"G01"};
    const auto kept = NetworkSbasis::electClockPivot(sats, "", "G03", &g01);
    EXPECT_EQ(kept.sat, "G03");
    EXPECT_FALSE(kept.health_skipped);
    const std::set<std::string> g01g03 = {"G01", "G03"};
    const auto moved = NetworkSbasis::electClockPivot(sats, "", "G03", &g01g03);
    EXPECT_EQ(moved.sat, "G02");
    EXPECT_TRUE(moved.health_skipped);
}

TEST(ClockPivotElection, DeclaredPinIsKeptWhenUnhealthy) {
    const std::set<std::string> bad = {"G01"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02"}, "G01", "", &bad);
    EXPECT_EQ(p.sat, "G01");
    EXPECT_FALSE(p.health_skipped);
}

TEST(ClockPivotElection, NoHealthySatelliteKeepsTheLexicalFirst) {
    const std::set<std::string> bad = {"G01", "G02"};
    const auto p = NetworkSbasis::electClockPivot({"G01", "G02"}, "", "G02", &bad);
    EXPECT_EQ(p.sat, "G01");
    EXPECT_TRUE(p.health_fallback);
    EXPECT_FALSE(p.health_skipped);
}

// The door removes exactly the selected observations and keeps every station key.
TEST(SatHealthDoor, RemovesOnlyTheFlaggedObservations) {
    std::vector<SyncedEpoch> epochs(2);
    epochs[0].time = GnssTime{kWeek, kDay};
    epochs[1].time = GnssTime{kWeek, kDay + 30.0};
    for (auto& se : epochs) {
        se.by_station["AAAA"] = {obsOf("G01"), obsOf("G02"), obsOf("E14")};
        se.by_station["BBBB"] = {obsOf("G01")};
    }
    NetworkSbasis::SatHealth H;
    H.sats["G01"] = {{gpsSeconds(kDay), false}, {gpsSeconds(kDay + 30.0), true}};
    H.sats["E14"] = {{gpsSeconds(kDay), true}};
    const std::size_t n = MultiRinexSync::removeObservationsIf(
        epochs, [&H](const GnssTime& t, const RinexObservation& o) {
            return H.unhealthyAt(o.satellite, TimeUtils::continuousGpsSeconds(t));
        });
    EXPECT_EQ(n, 4u);   // E14 twice, G01 at two stations in the second epoch
    ASSERT_EQ(epochs[0].by_station.at("AAAA").size(), 2u);
    EXPECT_EQ(epochs[0].by_station.at("AAAA")[0].satellite, "G01");
    EXPECT_EQ(epochs[0].by_station.at("BBBB").size(), 1u);
    ASSERT_EQ(epochs[1].by_station.at("AAAA").size(), 1u);
    EXPECT_EQ(epochs[1].by_station.at("AAAA")[0].satellite, "G02");
    EXPECT_TRUE(epochs[1].by_station.at("BBBB").empty());   // the key stays
}
