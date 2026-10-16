/**
 * @file test_group_delay_latch.cpp
 * @brief The broadcast group delay latched over the processing window (card OWD, O3).
 *
 * Each satellite's group delay is latched for the run (replacing the RF-0c latch, which
 * took the first navigation record) as the low median of the group delays of its healthy
 * records whose clock reference epoch lies in the run's processing window, with Galileo's
 * F/NAV (E1, E5a) records preferred when present.  A satellite with no such record takes
 * the healthy record in force at the window start; a satellite with no healthy record at
 * all is latched by the same rule over all its records; zeros are real values.  Also
 * tested: the RINEX parser keeps the Galileo data-source word the rule reads.
 */
#include <gtest/gtest.h>

#include "network/solve/Sbasis.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/RinexNavParser.hpp"

#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr int    kWeek = 2313;
constexpr double kDay  = 3.0 * 86400.0;   // Wednesday 2024-05-08 00:00 GPS, seconds of week

double gpsSeconds(double sow) { return kWeek * 604800.0 + sow; }

BroadcastEphemeris rec(const std::string& sat, double toc_sow, double tgd_ns, int health = 0,
                       int data_source = -1) {
    BroadcastEphemeris e;
    e.satellite = sat;
    e.system = sat[0] == 'E' ? System::Galileo : sat[0] == 'C' ? System::BeiDou : System::GPS;
    e.health = health;
    e.data_source = data_source;
    e.kepler.toc = GnssTime{kWeek, toc_sow};
    e.kepler.toe = GnssTime{kWeek, toc_sow};
    e.kepler.tgd0 = tgd_ns * 1e-9;
    return e;
}

constexpr int kFnav = 258;   // F/NAV E5a-I, clock (E5a, E1)
constexpr int kInav = 517;   // I/NAV E1-B + E5b-I, clock (E5b, E1)

}  // namespace

// An upload inside the window: the value in force for most of the window wins (the low
// median), and the file's later value never enters.
TEST(GroupDelayLatch, WindowMedianIgnoresRecordsAfterTheRun) {
    std::vector<BroadcastEphemeris> nav;
    for (int k = 0; k < 30; ++k) nav.push_back(rec("E21", kDay + 600.0 * k, 0.0, 0, kFnav));
    for (int k = 30; k < 36; ++k) nav.push_back(rec("E21", kDay + 600.0 * k, -0.233, 0, kFnav));
    for (int k = 102; k < 110; ++k) nav.push_back(rec("E21", kDay + 600.0 * k, -2.328, 0, kFnav));
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 21600.0));
    const auto& g = L.sats.at("E21");
    EXPECT_DOUBLE_EQ(g.value_s, 0.0);                 // a zero is a real value, kept
    EXPECT_EQ(g.reason, NetworkSbasis::GroupDelayLatchReason::Window);
    EXPECT_EQ(g.n_used, 36);
    EXPECT_EQ(L.outside_window, 8);
    EXPECT_DOUBLE_EQ(g.first_s, 0.0);
    EXPECT_NEAR(g.spread_file_s, 2.328e-9, 1e-15);
}

// A window after the upload takes the post-upload value, where the first record would not.
TEST(GroupDelayLatch, LateWindowTakesTheValueBroadcastDuringIt) {
    std::vector<BroadcastEphemeris> nav;
    for (int k = 0; k < 10; ++k) nav.push_back(rec("E24", kDay + 3600.0 * k, -8.615, 0, kFnav));
    for (int k = 17; k < 24; ++k) nav.push_back(rec("E24", kDay + 3600.0 * k, -6.286, 0, kFnav));
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay + 64800.0),
                                                            gpsSeconds(kDay + 86400.0));
    EXPECT_NEAR(L.sats.at("E24").value_s, -6.286e-9, 1e-18);
    EXPECT_NEAR(L.sats.at("E24").first_s, -8.615e-9, 1e-18);
}

// No record inside the window: the record in force at the start (the latest at or before
// it), else the earliest after it.
TEST(GroupDelayLatch, NoRecordInTheWindowTakesTheOneInForceAtTheStart) {
    std::vector<BroadcastEphemeris> nav = {
        rec("G03", kDay - 16.0, 1.397), rec("G03", kDay + 7200.0, 1.397),   // 23:59:44, 02:00
        rec("G25", kDay + 7200.0, 5.122),                                   // only after
    };
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 7200.0));
    EXPECT_NEAR(L.sats.at("G03").value_s, 1.397e-9, 1e-18);
    EXPECT_EQ(L.sats.at("G03").reason, NetworkSbasis::GroupDelayLatchReason::InForceAtStart);
    EXPECT_EQ(L.sats.at("G03").n_used, 1);
    EXPECT_NEAR(L.sats.at("G25").value_s, 5.122e-9, 1e-18);
    EXPECT_EQ(L.n_in_force, 2);
}

// Unhealthy records are set aside when a healthy one exists; a satellite with no healthy
// record at all keeps its datum from all its records (the GPS pivot G01 on DOY 129).
TEST(GroupDelayLatch, HealthFilterAndTheAllUnhealthyFallback) {
    std::vector<BroadcastEphemeris> nav = {
        rec("G05", kDay + 0.0, 9.0, /*health=*/1), rec("G05", kDay + 60.0, -10.71),
        rec("G05", kDay + 120.0, -10.71),
        rec("G01", kDay + 0.0, -19.558, 63), rec("G01", kDay + 7200.0, -19.558, 63),
    };
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 21600.0));
    EXPECT_NEAR(L.sats.at("G05").value_s, -10.71e-9, 1e-18);
    EXPECT_EQ(L.dropped_unhealthy, 1);
    EXPECT_NEAR(L.sats.at("G01").value_s, -19.558e-9, 1e-18);
    EXPECT_EQ(L.sats.at("G01").reason, NetworkSbasis::GroupDelayLatchReason::AllUnhealthy);
    EXPECT_EQ(L.n_all_unhealthy, 1);
}

// Galileo prefers its F/NAV (E1, E5a) records; I/NAV stands in only without F/NAV.
TEST(GroupDelayLatch, GalileoPrefersFnavAndFallsBackToInav) {
    std::vector<BroadcastEphemeris> nav = {
        rec("E02", kDay + 0.0, -3.725, 0, kFnav), rec("E02", kDay + 600.0, -4.424, 0, kInav),
        rec("E02", kDay + 1200.0, -4.424, 0, kInav),
        rec("E03", kDay + 0.0, -1.0, 0, kInav), rec("E03", kDay + 600.0, -1.0, 0, kInav),
    };
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 7200.0));
    EXPECT_NEAR(L.sats.at("E02").value_s, -3.725e-9, 1e-18);
    EXPECT_EQ(L.dropped_inav, 2);
    EXPECT_NEAR(L.sats.at("E03").value_s, -1.0e-9, 1e-18);
}

// The low median is always a broadcast value (never the mean of the two middle records).
TEST(GroupDelayLatch, LowMedianIsABroadcastValue) {
    std::vector<BroadcastEphemeris> nav = {
        rec("E25", kDay + 0.0, 0.233, 0, kFnav), rec("E25", kDay + 600.0, 0.0, 0, kFnav),
    };
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 7200.0));
    EXPECT_DOUBLE_EQ(L.sats.at("E25").value_s, 0.0);
}

// The same broadcast value printed with different last digits (a merged file: G02 on DOY 129)
// keeps the first record's exact bits, so an unchanged datum is unchanged to the bit; a real
// step (one broadcast quantum or more) is taken.
TEST(GroupDelayLatch, RepresentationNoiseKeepsTheFirstRecordsBits) {
    const double first = -1.769512891770e-08, other = -1.769512891769e-08;
    std::vector<BroadcastEphemeris> nav = {rec("G02", kDay - 16.0, 0.0)};
    nav[0].kepler.tgd0 = first;
    for (int k = 0; k < 9; ++k) {
        nav.push_back(rec("G02", kDay + 600.0 * k, 0.0));
        nav.back().kepler.tgd0 = other;
    }
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 7200.0));
    EXPECT_EQ(L.sats.at("G02").value_s, first);   // bit-exact

    std::vector<BroadcastEphemeris> stepped = {rec("J02", kDay - 16.0, 1.863),
                                               rec("J02", kDay + 600.0, 1.397),
                                               rec("J02", kDay + 1200.0, 1.397)};
    const auto S = NetworkSbasis::latchBroadcastGroupDelays(stepped, gpsSeconds(kDay),
                                                            gpsSeconds(kDay + 7200.0));
    EXPECT_NEAR(S.sats.at("J02").value_s, 1.397e-9, 1e-18);   // a real step is taken
}

// The RINEX parser keeps the Galileo data-source word (ORBIT-5 field 2): E21's first record
// of BRDC00IGS_R_20241290000_01D_MN.rnx, an F/NAV record (258).
TEST(GroupDelayLatch, ParserKeepsTheGalileoDataSourceWord) {
    const std::string nav =
        "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
        "                                                            END OF HEADER\n"
        "E21 2024 05 07 23 30 00-5.932944477536E-04-2.017941369559E-12 0.000000000000E+00\n"
        "     4.800000000000E+01 2.211250000000E+02 2.588679257453E-09-4.195132334043E-01\n"
        "     1.020170748234E-05 1.092869788408E-04 1.274794340134E-05 5.440622840881E+03\n"
        "     2.574000000000E+05 6.146728992462E-08 3.386102136403E-01-1.173466444016E-07\n"
        "     9.667877342697E-01 6.996875000000E+01 1.986495130044E+00-5.350937173726E-09\n"
        "     5.035924052165E-11 2.580000000000E+02 2.313000000000E+03 0.000000000000E+00\n"
        "     3.120000000000E+00 0.000000000000E+00 0.000000000000E+00 0.000000000000E+00\n"
        "     2.581000000000E+05\n";
    const std::vector<BroadcastEphemeris> recs = RinexNavParser::parseString(nav);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].satellite, "E21");
    EXPECT_EQ(recs[0].data_source, kFnav);
    EXPECT_EQ(recs[0].health, 0);
    EXPECT_DOUBLE_EQ(recs[0].kepler.tgd0, 0.0);
}
