/**
 * @file test_support_synthetic_nav.cpp
 * @brief The synthetic broadcast records of tests/support/SyntheticNav.hpp, read by the
 *        product code that reads real records.
 *
 * The builders serve later tests of the real-time engine and of the user fixing
 * functions, so a record they make has to behave as a decoded record does in every reader
 * those tests reach: the live store's selection and fit window, the broadcast clock a
 * priori's transmission-time selection and health rule, the network's group-delay latch
 * and health series.  Each case below builds its records only through the builders and
 * checks one reader's answer, with the expected values worked out from the reader's code:
 *   - the defaults (identity, times, group delays, clock, orbit) of a GPS and a Galileo
 *     record, and an orbit of the constellation's radius and a plausible speed;
 *   - the series helper: spacing, issue-of-data wrap, a week crossing and a change that
 *     takes effect at the chosen record;
 *   - the store selects the record closest in toe (the current one on a tie), reports its
 *     group delay, and refuses a record beyond its fit window;
 *   - the broadcast clock a priori selects by transmission time and never selects an
 *     unhealthy record or one without a transmission time;
 *   - the group-delay latch returns the stepped value only in a window that holds it,
 *     sets unhealthy records aside and prefers Galileo F/NAV;
 *   - the health series turns unhealthy at the clock epoch of the first unhealthy record.
 */

// The header under test comes first, so this file also proves it compiles on its own.
#include "support/SyntheticNav.hpp"

#include <gtest/gtest.h>

#include "network/solve/Sbasis.hpp"
#include "products/BrdcClockApriori.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace GnssProcessor;
using namespace gnss_test;

namespace {

/// The live store is a process-wide singleton: each case starts and ends with it empty.
class SupportSyntheticNav : public ::testing::Test {
protected:
    void SetUp() override { LiveEphemerisStore::getInstance().clear(); }
    void TearDown() override { LiveEphemerisStore::getInstance().clear(); }
};

/// With every harmonic term zero, the orbit radius is a (1 - e cos E): within a e of a.
void expectRadiusOnTheEllipse(const BroadcastEphemeris& e, const GnssTime& t) {
    const auto s = LiveEphemerisStore::evaluateRecord(e, t);
    ASSERT_TRUE(s.has_value()) << e.satellite;
    const double a = e.kepler.sqrt_a * e.kepler.sqrt_a;
    const double r = s->position.norm();
    EXPECT_GE(r, a * (1.0 - e.kepler.e) - 1e-3) << e.satellite;
    EXPECT_LE(r, a * (1.0 + e.kepler.e) + 1e-3) << e.satellite;
}

/// The a priori's selection as a record index, -1 when none is eligible.
int selected(const BrdcClockApriori& ap, const std::string& sat, double t_s) {
    const std::optional<int> i = ap.select(sat, t_s);
    return i ? *i : -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// The defaults
// ---------------------------------------------------------------------------

TEST_F(SupportSyntheticNav, GpsRecordDefaults) {
    const double d = syntheticNavDayStart();
    const BroadcastEphemeris e = gpsNav(5, d + 7200.0, 37).build();
    EXPECT_EQ(e.satellite, "G05");
    EXPECT_EQ(e.system, System::GPS);
    EXPECT_EQ(e.iode, 37);
    EXPECT_EQ(e.iodc, 37);
    EXPECT_EQ(e.health, 0);
    EXPECT_EQ(e.data_source, -1);
    EXPECT_EQ(e.kepler.toc.week, kSyntheticNavWeek);
    EXPECT_DOUBLE_EQ(e.kepler.toc.tow, kSyntheticNavDaySow + 7200.0);
    EXPECT_TRUE(e.kepler.toe == e.kepler.toc);
    ASSERT_TRUE(e.has_transmission_time);
    EXPECT_DOUBLE_EQ(brdcTocGpsSeconds(e) - brdcTransmissionGpsSeconds(e), kSyntheticGpsLeadS);
    EXPECT_DOUBLE_EQ(e.kepler.tgd0, kSyntheticGpsTgdS);
    EXPECT_DOUBLE_EQ(e.kepler.tgd1, 0.0);
    EXPECT_EQ(LiveEphemerisStore::ssrIode(e), 37);

    // The clock polynomial about toc, as the store evaluates it.
    const auto c = LiveEphemerisStore::evaluateRecord(e, syntheticNavTime(d + 7300.0));
    ASSERT_TRUE(c.has_value());
    EXPECT_DOUBLE_EQ(c->clock_bias_s, kSyntheticAf0S + kSyntheticAf1 * 100.0);

    expectRadiusOnTheEllipse(e, e.kepler.toe);
    expectRadiusOnTheEllipse(e, syntheticNavTime(d + 7200.0 + 5400.0));
    const double a = e.kepler.sqrt_a * e.kepler.sqrt_a;
    EXPECT_NEAR(a, 26.56e6, 0.01e6);
}

TEST_F(SupportSyntheticNav, GalileoRecordDefaults) {
    const double d = syntheticNavDayStart();
    const BroadcastEphemeris e = galileoNav(11, d, 300).build();
    EXPECT_EQ(e.satellite, "E11");
    EXPECT_EQ(e.system, System::Galileo);
    EXPECT_EQ(e.iode, 300);
    EXPECT_EQ(e.iodc, -1);
    EXPECT_EQ(e.data_source, kSyntheticGalFnav);
    EXPECT_NE(e.data_source & BrdcClockApriori::kGalileoFnavBit, 0);
    EXPECT_DOUBLE_EQ(e.kepler.tgd0, kSyntheticGalBgdE5aS);
    EXPECT_DOUBLE_EQ(e.kepler.tgd1, kSyntheticGalBgdE5bS);
    EXPECT_DOUBLE_EQ(brdcTocGpsSeconds(e) - brdcTransmissionGpsSeconds(e), kSyntheticGalLeadS);
    // The store reports the low 8 bits of IODnav to the correction gate.
    EXPECT_EQ(LiveEphemerisStore::ssrIode(e), 300 & 0xFF);

    expectRadiusOnTheEllipse(e, e.kepler.toe);
    const double a = e.kepler.sqrt_a * e.kepler.sqrt_a;
    EXPECT_NEAR(a, 29.60e6, 0.01e6);

    // Setters change only their field.
    const BroadcastEphemeris i = galileoNav(11, d, 300).dataSource(kSyntheticGalInav)
                                     .groupDelay(-3.0e-9).health(16).noTransmissionTime().build();
    EXPECT_EQ(i.data_source, kSyntheticGalInav);
    EXPECT_DOUBLE_EQ(i.kepler.tgd0, -3.0e-9);
    EXPECT_DOUBLE_EQ(i.kepler.tgd1, kSyntheticGalBgdE5bS);
    EXPECT_EQ(i.health, 16);
    EXPECT_FALSE(i.has_transmission_time);
    EXPECT_EQ(i.kepler.sqrt_a, e.kepler.sqrt_a);
}

// Two satellites never share a position, and the store's orbit query moves the satellite
// at a speed an Earth-fixed MEO orbit can have (the inertial speed is about 3.9 km/s and
// the Earth's rotation removes at most about 2.2 km/s at this radius).
TEST_F(SupportSyntheticNav, OrbitIsPlausibleAndSpreadByPrn) {
    const double d = syntheticNavDayStart();
    const GnssTime t = syntheticNavTime(d + 600.0);
    const auto g5 = LiveEphemerisStore::evaluateRecord(gpsNav(5, d).build(), t);
    const auto g6 = LiveEphemerisStore::evaluateRecord(gpsNav(6, d).build(), t);
    const auto g11 = LiveEphemerisStore::evaluateRecord(gpsNav(11, d).build(), t);
    ASSERT_TRUE(g5 && g6 && g11);
    EXPECT_GT((g5->position - g6->position).norm(), 1.0e6);
    EXPECT_GT((g5->position - g11->position).norm(), 1.0e6);

    auto& store = LiveEphemerisStore::getInstance();
    store.insert(gpsNav(5, d).build());
    store.insert(galileoNav(11, d).build());
    for (const char* sat : {"G05", "E11"}) {
        const auto pv = store.computeSatellitePV(sat, t);
        ASSERT_TRUE(pv.has_value()) << sat;
        EXPECT_GT(pv->velocity.norm(), 1500.0) << sat;
        EXPECT_LT(pv->velocity.norm(), 6000.0) << sat;
    }
}

// ---------------------------------------------------------------------------
// The series helper
// ---------------------------------------------------------------------------

TEST_F(SupportSyntheticNav, SeriesSpacesTheUploadsAndChangesFromTheChosenRecord) {
    const double d = syntheticNavDayStart();
    const auto s = syntheticNavSeries(gpsNav(3, d, 254).receivedAt(d - 7000.0).build(), 4, 7200.0,
                                      2, setGroupDelay(-6.0e-9));
    ASSERT_EQ(s.size(), 4u);
    const int iode[4] = {254, 255, 0, 1};
    const int iodc[4] = {254, 255, 256, 257};
    for (std::size_t k = 0; k < s.size(); ++k) {
        SCOPED_TRACE(k);
        EXPECT_EQ(s[k].satellite, "G03");
        const double dk = 7200.0 * static_cast<double>(k);
        EXPECT_DOUBLE_EQ(brdcTocGpsSeconds(s[k]), d + dk);
        EXPECT_DOUBLE_EQ(syntheticNavSeconds(s[k].kepler.toe), d + dk);
        EXPECT_DOUBLE_EQ(brdcTransmissionGpsSeconds(s[k]), d - kSyntheticGpsLeadS + dk);
        EXPECT_DOUBLE_EQ(syntheticNavSeconds(s[k].received_time), d - 7000.0 + dk);
        EXPECT_EQ(s[k].iode, iode[k]);
        EXPECT_EQ(s[k].iodc, iodc[k]);
        EXPECT_DOUBLE_EQ(s[k].kepler.tgd0, k < 2 ? kSyntheticGpsTgdS : -6.0e-9);
        EXPECT_EQ(s[k].health, 0);
    }

    // Galileo IODnav wraps at 10 bits; no change without a change index.
    const auto g = syntheticNavSeries(galileoNav(4, d, 1022).build(), 3, 600.0, -1, setHealth(1));
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(g[0].iode, 1022);
    EXPECT_EQ(g[1].iode, 1023);
    EXPECT_EQ(g[2].iode, 0);
    for (const auto& e : g) {
        EXPECT_EQ(e.health, 0);
        EXPECT_EQ(e.iodc, -1);
        EXPECT_EQ(e.received_time.week, 0);   // unset stays unset
    }

    // A series across the end of the week rolls the week number.
    const double sat_late = (kSyntheticNavWeek + 1) * 604800.0 - 3600.0;
    const auto w = syntheticNavSeries(gpsNav(7, sat_late).build(), 2, 7200.0);
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[1].kepler.toc.week, kSyntheticNavWeek + 1);
    EXPECT_DOUBLE_EQ(w[1].kepler.toc.tow, 3600.0);
}

// ---------------------------------------------------------------------------
// The live store
// ---------------------------------------------------------------------------

// The store keeps the current and the previous record and answers with the one whose toe
// is closer to the epoch (the current one on a tie): the group delay it hands out is the
// stepped value only once the stepped record is the closer one.
TEST_F(SupportSyntheticNav, StoreSelectsTheRecordClosestInToeAndItsGroupDelay) {
    const double d = syntheticNavDayStart();
    auto& store = LiveEphemerisStore::getInstance();
    for (const auto& e : syntheticNavSeries(gpsNav(9, d, 10).build(), 3, 7200.0, 2,
                                            setGroupDelay(-6.0e-9)))
        store.insert(e);
    EXPECT_EQ(store.size(), 1u);

    const auto before = store.selectedRecord("G09", syntheticNavTime(d + 7200.0 + 3000.0));
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->eph.iode, 11);
    EXPECT_DOUBLE_EQ(before->eph.kepler.tgd0, kSyntheticGpsTgdS);

    const auto tie = store.selectedRecord("G09", syntheticNavTime(d + 10800.0));
    ASSERT_TRUE(tie.has_value());
    EXPECT_EQ(tie->eph.iode, 12);

    const auto after = store.selectedRecord("G09", syntheticNavTime(d + 7200.0 + 4000.0));
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->eph.iode, 12);
    EXPECT_DOUBLE_EQ(after->eph.kepler.tgd0, -6.0e-9);

    const auto clk = store.computeSatelliteClock("G09", syntheticNavTime(d + 7200.0 + 3000.0));
    ASSERT_TRUE(clk.has_value());
    EXPECT_EQ(clk->iode, 11);
    EXPECT_DOUBLE_EQ(clk->bias, kSyntheticAf0S + kSyntheticAf1 * 3000.0);

    // GPS fit window: 4 h from the newest record's toe, inclusive.
    EXPECT_TRUE(store.selectedRecord("G09", syntheticNavTime(d + 14400.0 + 14400.0)).has_value());
    EXPECT_FALSE(store.selectedRecord("G09", syntheticNavTime(d + 14400.0 + 14401.0)).has_value());
    EXPECT_FALSE(store.computeSatellitePV("G09", syntheticNavTime(d + 14400.0 + 14401.0)).has_value());
}

TEST_F(SupportSyntheticNav, FitWindowIsFourHoursForGpsAndTwoForGalileo) {
    const double d = syntheticNavDayStart();
    const BroadcastEphemeris g = gpsNav(1, d).build();
    const BroadcastEphemeris e = galileoNav(1, d).build();
    EXPECT_TRUE(LiveEphemerisStore::withinFitWindow(g, syntheticNavTime(d + 14400.0)));
    EXPECT_FALSE(LiveEphemerisStore::withinFitWindow(g, syntheticNavTime(d + 14401.0)));
    EXPECT_TRUE(LiveEphemerisStore::withinFitWindow(g, syntheticNavTime(d - 14400.0)));
    EXPECT_TRUE(LiveEphemerisStore::withinFitWindow(e, syntheticNavTime(d + 7200.0)));
    EXPECT_FALSE(LiveEphemerisStore::withinFitWindow(e, syntheticNavTime(d + 7201.0)));
    EXPECT_FALSE(LiveEphemerisStore::withinFitWindow(e, syntheticNavTime(d - 7201.0)));
}

// ---------------------------------------------------------------------------
// The broadcast clock a priori
// ---------------------------------------------------------------------------

// Uploads at d, d + 2 h, d + 4 h, each transmitted 2 h before its clock epoch: at d + 1 h
// the latest transmitted is the second; with the second and third flagged unhealthy, the
// first stays selected until its fit window ends; a record without a transmission time is
// never selected.
TEST_F(SupportSyntheticNav, AprioriSelectsByTransmissionTimeAndHonoursHealth) {
    const double d = syntheticNavDayStart();
    const BroadcastEphemeris first = gpsNav(7, d, 10).build();

    const BrdcClockApriori healthy(syntheticNavSeries(first, 3, 7200.0), d, d + 21600.0);
    EXPECT_EQ(healthy.stats().n_staged, 3u);
    EXPECT_EQ(selected(healthy, "G07", d + 3600.0), 1);
    EXPECT_EQ(selected(healthy, "G07", d - 1.0), 0);
    const auto ev = healthy.evaluate("G07", syntheticNavTime(d + 3600.0));
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->record, 1);
    EXPECT_EQ(ev->iode, 11);
    EXPECT_DOUBLE_EQ(ev->brdc_s, healthy.clockSeconds(1, d + 3600.0));

    const BrdcClockApriori sick(syntheticNavSeries(first, 3, 7200.0, 1, setHealth(63)), d,
                                d + 21600.0);
    EXPECT_EQ(sick.stats().n_unhealthy, 2u);
    EXPECT_EQ(sick.stats().n_staged, 1u);
    EXPECT_EQ(selected(sick, "G07", d + 3600.0), 0);
    EXPECT_EQ(selected(sick, "G07", d + 14400.0), 0);
    EXPECT_EQ(selected(sick, "G07", d + 14401.0), -1);

    const BrdcClockApriori untimed(std::vector<BroadcastEphemeris>{gpsNav(8, d).noTransmissionTime().build()},
                                   d, d + 3600.0);
    EXPECT_EQ(untimed.stats().n_no_ttom, 1u);
    EXPECT_EQ(selected(untimed, "G08", d + 60.0), -1);
}

TEST_F(SupportSyntheticNav, AprioriRecordSetKeepsTheGalileoDefaultAndDropsInav) {
    const double d = syntheticNavDayStart();
    const std::vector<BroadcastEphemeris> nav = {
        gpsNav(5, d).build(), galileoNav(11, d).build(),
        galileoNav(11, d).dataSource(kSyntheticGalInav).build()};
    const auto with = BrdcClockApriori::aprioriRecords(nav, /*galileo=*/true);
    ASSERT_EQ(with.size(), 2u);
    EXPECT_EQ(with[1].satellite, "E11");
    EXPECT_EQ(with[1].data_source, kSyntheticGalFnav);
    EXPECT_EQ(BrdcClockApriori::aprioriRecords(nav, /*galileo=*/false).size(), 1u);
}

// ---------------------------------------------------------------------------
// The group-delay latch and the health series
// ---------------------------------------------------------------------------

// Twelve uploads 2 h apart, T_GD stepping at the third: the low median over the day is the
// stepped value (ten of twelve); a window [d, d + 4 h) holds only the first two (the window
// is half-open) and a window from d + 4 h only stepped records.
TEST_F(SupportSyntheticNav, LatchReturnsTheGroupDelayOfTheWindow) {
    const double d = syntheticNavDayStart();
    const auto nav = syntheticNavSeries(gpsNav(12, d).build(), 12, 7200.0, 2,
                                        setGroupDelay(-6.0e-9));
    const auto day = NetworkSbasis::latchBroadcastGroupDelays(nav, d, d + 86400.0);
    const auto& g = day.sats.at("G12");
    EXPECT_DOUBLE_EQ(g.value_s, -6.0e-9);
    EXPECT_EQ(g.reason, NetworkSbasis::GroupDelayLatchReason::Window);
    EXPECT_EQ(g.n_used, 12);
    EXPECT_DOUBLE_EQ(g.first_s, kSyntheticGpsTgdS);
    EXPECT_NEAR(g.spread_file_s, 1.0e-9, 1e-18);

    const auto early = NetworkSbasis::latchBroadcastGroupDelays(nav, d, d + 14400.0);
    EXPECT_DOUBLE_EQ(early.sats.at("G12").value_s, kSyntheticGpsTgdS);
    EXPECT_EQ(early.sats.at("G12").n_used, 2);

    const auto late = NetworkSbasis::latchBroadcastGroupDelays(nav, d + 14400.0, d + 86400.0);
    EXPECT_DOUBLE_EQ(late.sats.at("G12").value_s, -6.0e-9);
    EXPECT_EQ(late.sats.at("G12").n_used, 10);
}

TEST_F(SupportSyntheticNav, LatchSetsUnhealthyRecordsAsideAndPrefersFnav) {
    const double d = syntheticNavDayStart();
    const auto nav = syntheticNavSeries(gpsNav(12, d).build(), 12, 7200.0, 2,
                                        [](BroadcastEphemeris& e) {
                                            e.health = 1;
                                            e.kepler.tgd0 = -6.0e-9;
                                        });
    const auto L = NetworkSbasis::latchBroadcastGroupDelays(nav, d, d + 86400.0);
    EXPECT_DOUBLE_EQ(L.sats.at("G12").value_s, kSyntheticGpsTgdS);
    EXPECT_EQ(L.sats.at("G12").n_used, 2);
    EXPECT_EQ(L.dropped_unhealthy, 10);

    const std::vector<BroadcastEphemeris> gal = {
        galileoNav(11, d, 1).build(),
        galileoNav(11, d + 300.0, 1).dataSource(kSyntheticGalInav).groupDelay(-3.0e-9).build(),
        galileoNav(11, d + 600.0, 2).build(),
        galileoNav(11, d + 900.0, 2).dataSource(kSyntheticGalInav).groupDelay(-3.0e-9).build()};
    const auto G = NetworkSbasis::latchBroadcastGroupDelays(gal, d, d + 7200.0);
    EXPECT_DOUBLE_EQ(G.sats.at("E11").value_s, kSyntheticGalBgdE5aS);
    EXPECT_EQ(G.dropped_inav, 2);
}

// A record takes force at its clock epoch: the satellite turns unhealthy exactly at the
// clock epoch of the first unhealthy record of the series.
TEST_F(SupportSyntheticNav, HealthSeriesStepsAtTheChangedRecord) {
    const double d = syntheticNavDayStart();
    const auto nav = syntheticNavSeries(gpsNav(20, d).build(), 6, 7200.0, 2, setHealth(63));
    const auto H = NetworkSbasis::satHealthSeries(nav, 0x03F);
    EXPECT_FALSE(H.unhealthyAt("G20", d + 14399.0));
    EXPECT_TRUE(H.unhealthyAt("G20", d + 14400.0));
    EXPECT_TRUE(H.unhealthyAt("G20", d + 40000.0));
    EXPECT_EQ(H.records, 6);
    EXPECT_EQ(H.records_unhealthy, 4);
}
