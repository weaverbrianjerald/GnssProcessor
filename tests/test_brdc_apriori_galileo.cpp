/**
 * @file test_brdc_apriori_galileo.cpp
 * @brief Galileo under the native broadcast clock a priori (`network.apriori_source
 *        sp3_brdc_clock`): `BrdcClockApriori::aprioriRecords` keeps the F/NAV records, and the
 *        selector built from them switches, stays continuous and never takes an I/NAV clock.
 *
 * Galileo transmits two clocks under one IODnav: F/NAV for the (E1, E5a) pair the network
 * models, I/NAV for (E1, E5b).  A transmission-time rule over both would alternate between
 * them, so the a priori takes F/NAV only, for the producer and for the consumer replay alike.
 *
 * Covered:
 *   - GPS records are kept; Galileo F/NAV records are kept only with Galileo armed; I/NAV,
 *     a Galileo record without a data-source word and every other constellation are dropped;
 *   - on synthetic records the selector switches between two F/NAV records at the first node
 *     after the second's transmission time, keeps the a priori continuous there, and never
 *     selects the I/NAV record of the same IODnav;
 *   - on the DOY-129 navigation file (fixture-gated) every Galileo record kept is an F/NAV
 *     one, and every Galileo satellite with a healthy F/NAV record has a selection at every
 *     30 s node of the first six hours.
 */

#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include "products/BrdcClockApriori.hpp"
#include "products/RinexNavParser.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

GnssTime fromContinuous(double s) {
    const double wk = std::floor(s / 604800.0);
    return GnssTime{ static_cast<int>(wk), s - wk * 604800.0 };
}

/// 2024-05-08 00:00:00 GPS, continuous seconds.
double day129() {
    return TimeUtils::continuousGpsSeconds(TimeUtils::gregorianGpsToGpsTime(2024, 5, 8, 0, 0, 0.0));
}

/// A Keplerian record of @p sat with clock (af0, af1) at toc = toe = @p toc_s, transmitted at
/// @p ttom_s, with the Galileo data-source word @p source (-1 for none).
BroadcastEphemeris record(const std::string& sat, System sys, int iode, double toc_s, double ttom_s,
                          double af0, double af1, int source, int health = 0) {
    BroadcastEphemeris e;
    e.satellite = sat;
    e.system    = sys;
    e.iode      = iode;
    e.health    = health;
    e.data_source = source;
    e.kepler.toc = fromContinuous(toc_s);
    e.kepler.toe = fromContinuous(toc_s);
    e.kepler.af0 = af0;
    e.kepler.af1 = af1;
    e.transmission_time     = fromContinuous(ttom_s);
    e.has_transmission_time = true;
    return e;
}

constexpr int kFnav = 258;   // bits 1 (F/NAV E5a-I) and 8 (clock for E5a, E1)
constexpr int kInav = 517;   // bits 0, 2 (I/NAV E1-B, E5b-I) and 9 (clock for E5b, E1)

}  // namespace

TEST(BrdcAprioriGalileo, RecordSetKeepsGpsAndGalileoFnavOnly) {
    const double d = day129();
    const std::vector<BroadcastEphemeris> nav = {
        record("G05", System::GPS, 10, d, d - 7200.0, 1e-4, 0.0, -1),
        record("E11", System::Galileo, 50, d, d - 600.0, 2e-4, 0.0, kFnav),
        record("E11", System::Galileo, 50, d, d - 590.0, 2e-4 + 3e-9, 0.0, kInav),
        record("E12", System::Galileo, 51, d, d - 600.0, 3e-4, 0.0, -1),
        record("C08", System::BeiDou, 1, d, d - 600.0, 1e-4, 0.0, -1) };
    const auto with = BrdcClockApriori::aprioriRecords(nav, /*galileo=*/true);
    ASSERT_EQ(with.size(), 2u);
    EXPECT_EQ(with[0].satellite, "G05");
    EXPECT_EQ(with[1].satellite, "E11");
    EXPECT_EQ(with[1].data_source, kFnav);
    const auto without = BrdcClockApriori::aprioriRecords(nav, /*galileo=*/false);
    ASSERT_EQ(without.size(), 1u);
    EXPECT_EQ(without[0].satellite, "G05");
}

TEST(BrdcAprioriGalileo, TheSelectorSwitchesBetweenFnavRecordsAndNeverTakesInav) {
    const double d = day129();
    std::vector<BroadcastEphemeris> nav = {
        record("E11", System::Galileo, 60, d, d - 600.0, 2.0e-4, 1.0e-12, kFnav),
        record("E11", System::Galileo, 61, d + 600.0, d + 605.0, 2.0e-4 + 2e-10, 1.1e-12, kFnav),
        // The I/NAV record of IODnav 61 is transmitted later and carries a clock 1 ns away.
        record("E11", System::Galileo, 61, d + 600.0, d + 615.0, 2.0e-4 + 1.2e-9, 1.1e-12, kInav) };
    const BrdcClockApriori ap(BrdcClockApriori::aprioriRecords(nav, true), d, d + 3600.0);
    EXPECT_EQ(ap.stats().n_staged, 2u);
    EXPECT_EQ(ap.stats().n_switches, 1u);
    const auto* segs = ap.segments("E11");
    ASSERT_NE(segs, nullptr);
    ASSERT_EQ(segs->size(), 2u);
    EXPECT_DOUBLE_EQ((*segs)[1].start_s, d + 630.0) << "the first node at or after 00:10:05";
    const double t_sw = d + 630.0;
    const double a_old = ap.clockSeconds((*segs)[0].record, t_sw) + (*segs)[0].offset_s;
    const double a_new = ap.clockSeconds((*segs)[1].record, t_sw) + (*segs)[1].offset_s;
    EXPECT_NEAR(a_new, a_old, 1e-18) << "continuous across the cut-over";
    for (double t = d; t < d + 3600.0; t += 30.0) {
        const auto ev = ap.evaluate("E11", fromContinuous(t));
        ASSERT_TRUE(ev.has_value());
        EXPECT_EQ(ap.record(ev->record).data_source, kFnav);
    }
}

TEST(BrdcAprioriGalileo, Doy129FnavCoversEveryHealthyGalileoSatellite) {
    const char* kNav = "BRDC00IGS_R_20241290000_01D_MN.rnx";
    fs::path nav_path;
    for (const fs::path& c : { fs::path(GNSS_PROJECT_ROOT) / "data" / "2024-129" / "products" / kNav,
                               fs::path("C:/GnssProcessor/data/2024-129/products") / kNav })
        if (fs::exists(c)) { nav_path = c; break; }
    if (nav_path.empty()) GTEST_SKIP() << "DOY-129 navigation file not present";
    const auto all = RinexNavParser::parse(nav_path.string());
    const auto kept = BrdcClockApriori::aprioriRecords(all, true);
    std::set<std::string> gal_fnav_healthy;
    std::size_t n_gal = 0;
    for (const auto& e : kept) {
        if (e.system != System::Galileo) continue;
        ++n_gal;
        EXPECT_NE(e.data_source & BrdcClockApriori::kGalileoFnavBit, 0) << e.satellite;
        if (e.health == 0 && e.has_transmission_time) gal_fnav_healthy.insert(e.satellite);
    }
    EXPECT_GT(n_gal, 100u);
    EXPECT_GE(gal_fnav_healthy.size(), 20u);
    const double d = day129();
    const BrdcClockApriori ap(kept, d, d + 6.0 * 3600.0);
    for (const auto& sat : gal_fnav_healthy) {
        int missing = 0;
        for (double t = d + 3600.0; t < d + 6.0 * 3600.0; t += 30.0)
            if (!ap.evaluate(sat, fromContinuous(t))) ++missing;
        EXPECT_EQ(missing, 0) << sat;
    }
}
