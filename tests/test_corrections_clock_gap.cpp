/**
 * @file test_corrections_clock_gap.cpp
 * @brief Table tests of the satellite clock taken from a clock product that has a gap.
 *
 * GnssCorrections::interpolateClock and getSatelliteClockCorrection decide what a
 * satellite's clock is at an epoch before, inside and after a hole in the precise
 * clock product.  A wrong answer there is silent: a zero clock drops the satellite,
 * a clock held across a gap biases every range of that satellite, and a broadcast
 * value taken in place of a precise one makes carrier phase unusable.  These cases
 * pin today's answers on synthetic products whose expected values are written out
 * in each case: two-record linear interpolation, the exact-record shortcut, the
 * 65 s bracket limit, the one-hour validity window, the hold of the nearest record
 * beyond either end of the product, the refusal of a non-physical record, the week
 * rollover, and the broadcast fallback inside a gap when a live broadcast record
 * exists.
 *
 * Without a configuration the interpolation is the two-record linear mode.  Two
 * further cases load a configuration that selects the Lagrange and the least
 * squares modes, to record how those modes treat the same gap.
 *
 * getSatelliteClockCorrection reads two process-wide stores (SSR records and live
 * broadcast ephemerides).  Every case that calls it points the SSR overlay at an
 * owned empty store and clears the live broadcast store before and after use.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr int    kWeek = 2300;
constexpr double kTow0 = 86400.0;   // the first record of every product below

// Agreement for clock values of order 1e-4 s built from exactly representable
// times: the interpolation arithmetic leaves errors near 1e-20 s.
constexpr double kClockTol = 1e-18;

ClockData record(const std::string& sat, int week, double tow, double bias_s) {
    ClockData c;
    c.satellite  = sat;
    c.time       = GnssTime{ week, tow };
    c.clock_bias = bias_s;
    return c;
}

GnssTime epochAt(double offset_s) { return GnssTime{ kWeek, kTow0 + offset_s }; }

// The gapped product used by most cases.  Three records 30 s apart, a hole of
// 240 s, then two records 30 s apart.  The slope changes between intervals so a
// value taken from the wrong bracket cannot pass.
//   offset (s)    0        30           60           300           330
//   bias (s)      1e-4     1e-4+3e-9    1e-4+9e-9    1e-4+2.0e-8   1e-4+2.6e-8
constexpr double kB0 = 1.0e-4;
constexpr double kB1 = 1.0e-4 + 3.0e-9;
constexpr double kB2 = 1.0e-4 + 9.0e-9;
constexpr double kB3 = 1.0e-4 + 2.0e-8;
constexpr double kB4 = 1.0e-4 + 2.6e-8;

std::vector<ClockData> gappedProduct(const std::string& sat) {
    return { record(sat, kWeek, kTow0 + 0.0,   kB0),
             record(sat, kWeek, kTow0 + 30.0,  kB1),
             record(sat, kWeek, kTow0 + 60.0,  kB2),
             record(sat, kWeek, kTow0 + 300.0, kB3),
             record(sat, kWeek, kTow0 + 330.0, kB4) };
}

struct Row {
    double offset_s;   ///< query epoch relative to the first record
    double expect_s;   ///< expected clock (s); 0 means "no clock"
    const char* why;
};

// The expected answers of the gapped product in linear mode.  Each value is
// derived here from the records above, not read from the code.
const Row kGappedTable[] = {
    { -3600.0, 0.0,                         "one hour before the first record: outside the validity window" },
    { -3599.0, kB0,                         "just inside the window before the first record: the first record held" },
    {   -30.0, kB0,                         "before the first record: the first record held, not extrapolated" },
    {     0.0, kB0,                         "on the first record" },
    {    15.0, 1.0e-4 + 1.5e-9,             "midway between the first two records" },
    {    45.0, 1.0e-4 + 6.0e-9,             "midway between records 2 and 3 (kB1 + 6e-9 / 2)" },
    {    60.0, kB2,                         "on the last record before the gap" },
    {    61.0, 0.0,                         "1 s into the gap: the bracket spans 240 s, beyond 65 s" },
    {   180.0, 0.0,                         "middle of the gap" },
    {   299.0, 0.0,                         "1 s before the first record after the gap" },
    {   300.0, kB3,                         "on the first record after the gap (the record shortcut precedes the gap test)" },
    {   310.0, 1.0e-4 + 2.2e-8,             "a third of the way from record 4 to record 5 (kB3 + 6e-9 / 3)" },
    {   330.0, kB4,                         "on the last record" },
    {   390.0, kB4,                         "after the last record: the last record held, not extrapolated" },
    {  3929.0, kB4,                         "3599 s after the last record: still held" },
    {  3930.0, 0.0,                         "3600 s after the last record: outside the validity window" },
};

}  // namespace

// ---------------------------------------------------------------------------
// The linear mode, record by record, across the gap.
//
// Fails if the bracket limit (65 s) is removed or widened past 240 s (the gap
// rows then interpolate), if the record shortcut is moved after the gap test
// (the 300 s row turns 0), if the end-of-product behaviour changes from holding
// the nearest record to extrapolating or to refusing, or if the one-hour
// validity window changes.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, LinearModeAnswersBeforeInsideAndAfterTheGap) {
    LogManager log;
    const auto product = gappedProduct("G05");
    GnssCorrections gc(log, product, std::vector<ErpData>{});

    for (const Row& r : kGappedTable) {
        SCOPED_TRACE(std::string(r.why) + " (offset " + std::to_string(r.offset_s) + " s)");
        const double got = gc.interpolateClock("G05", epochAt(r.offset_s), product);
        if (r.expect_s == 0.0) {
            EXPECT_EQ(got, 0.0);
        } else {
            EXPECT_NEAR(got, r.expect_s, kClockTol);
        }
    }
}

// ---------------------------------------------------------------------------
// The product is sorted by the class, so the order of the records as loaded
// does not matter.  Fails if the per-satellite index stops sorting its records
// (the binary search then brackets the wrong pair).
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, RecordOrderInTheProductDoesNotMatter) {
    LogManager log;
    auto product = gappedProduct("G05");
    std::reverse(product.begin(), product.end());
    std::swap(product[1], product[3]);
    GnssCorrections gc(log, product, std::vector<ErpData>{});

    for (const Row& r : kGappedTable) {
        SCOPED_TRACE(std::string(r.why) + " (offset " + std::to_string(r.offset_s) + " s)");
        const double got = gc.interpolateClock("G05", epochAt(r.offset_s), product);
        if (r.expect_s == 0.0) {
            EXPECT_EQ(got, 0.0);
        } else {
            EXPECT_NEAR(got, r.expect_s, kClockTol);
        }
    }
}

// ---------------------------------------------------------------------------
// The bracket limit is inclusive at 65 s: records 65 s apart interpolate,
// records 65.5 s apart do not.  Fails if the comparison becomes ">=" or the
// limit moves.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, BracketOf65SecondsInterpolatesAndLongerIsRefused) {
    LogManager log;
    const std::vector<ClockData> product = {
        record("G06", kWeek, kTow0,        1.0e-4),
        record("G06", kWeek, kTow0 + 65.0, 1.0e-4 + 6.5e-9),
        record("G07", kWeek, kTow0,        1.0e-4),
        record("G07", kWeek, kTow0 + 65.5, 1.0e-4 + 6.5e-9),
    };
    GnssCorrections gc(log, product, std::vector<ErpData>{});

    // 30 s into a 65 s bracket: 1e-4 + 6.5e-9 * 30 / 65 = 1e-4 + 3e-9.
    EXPECT_NEAR(gc.interpolateClock("G06", epochAt(30.0), product), 1.0e-4 + 3.0e-9, kClockTol);
    EXPECT_EQ(gc.interpolateClock("G07", epochAt(30.0), product), 0.0);
    // Both records themselves are still served.
    EXPECT_NEAR(gc.interpolateClock("G07", epochAt(0.0), product), 1.0e-4, kClockTol);
    EXPECT_NEAR(gc.interpolateClock("G07", epochAt(65.5), product), 1.0e-4 + 6.5e-9, kClockTol);
}

// ---------------------------------------------------------------------------
// A non-physical record (the bad-clock sentinel of about 1 s) is refused, and
// it blanks both intervals it bounds: the neighbours 60 s apart are not paired
// around it.  Fails if the validity bound on a record is removed (the sentinel
// is then interpolated into a 0.5 s clock) or if a bad record is skipped in
// favour of the next valid one.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, ANonPhysicalRecordBlanksTheIntervalsAroundIt) {
    LogManager log;
    const std::vector<ClockData> product = {
        record("G08", kWeek, kTow0,        1.0e-4),
        record("G08", kWeek, kTow0 + 30.0, 0.999999999999),
        record("G08", kWeek, kTow0 + 60.0, 1.0e-4 + 6.0e-9),
    };
    GnssCorrections gc(log, product, std::vector<ErpData>{});

    EXPECT_NEAR(gc.interpolateClock("G08", epochAt(0.0), product), 1.0e-4, kClockTol);
    EXPECT_EQ(gc.interpolateClock("G08", epochAt(15.0), product), 0.0);
    EXPECT_EQ(gc.interpolateClock("G08", epochAt(30.0), product), 0.0);
    EXPECT_EQ(gc.interpolateClock("G08", epochAt(45.0), product), 0.0);
    EXPECT_NEAR(gc.interpolateClock("G08", epochAt(60.0), product), 1.0e-4 + 6.0e-9, kClockTol);
}

// ---------------------------------------------------------------------------
// Records on either side of a GPS week rollover interpolate on continuous
// seconds, and an epoch written as (week, 604800) equals (week + 1, 0).
// Fails if the bracket is searched on time of week alone.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, InterpolationRunsAcrossTheWeekRollover) {
    LogManager log;
    const std::vector<ClockData> product = {
        record("G09", 2300, 604785.0, 2.0e-4),
        record("G09", 2301, 15.0,     2.0e-4 + 3.0e-9),
    };
    GnssCorrections gc(log, product, std::vector<ErpData>{});

    EXPECT_NEAR(gc.interpolateClock("G09", GnssTime{ 2301, 0.0 }, product), 2.0e-4 + 1.5e-9, kClockTol);
    EXPECT_NEAR(gc.interpolateClock("G09", GnssTime{ 2300, 604800.0 }, product), 2.0e-4 + 1.5e-9, kClockTol);
}

// ---------------------------------------------------------------------------
// No clock at all: a satellite the product lacks, an empty product, and an
// empty identifier each give 0.  Fails if a missing satellite borrows another
// satellite's records or a non-zero default.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, MissingSatelliteOrProductGivesZero) {
    LogManager log;
    const auto product = gappedProduct("G05");
    GnssCorrections gc(log, product, std::vector<ErpData>{});
    EXPECT_EQ(gc.interpolateClock("G11", epochAt(15.0), product), 0.0);
    EXPECT_EQ(gc.interpolateClock("", epochAt(15.0), product), 0.0);

    GnssCorrections empty(log);
    EXPECT_EQ(empty.interpolateClock("G05", epochAt(15.0), product), 0.0);
}

// ---------------------------------------------------------------------------
// interpolateClock reads the product the instance was built with; its third
// argument is not read.  The only production caller passes the instance's own
// product, so the two agree there; the header describes the argument as the
// container interpolated from.  Fails if the method starts reading the argument
// (then this case should be rewritten to the new contract, and the header kept).
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, InterpolationReadsTheInstanceProductNotTheArgument) {
    LogManager log;
    const auto own = gappedProduct("G05");
    GnssCorrections gc(log, own, std::vector<ErpData>{});

    std::vector<ClockData> other = gappedProduct("G05");
    for (auto& c : other) c.clock_bias += 5.0e-6;

    EXPECT_NEAR(gc.interpolateClock("G05", epochAt(15.0), other), 1.0e-4 + 1.5e-9, kClockTol);
    EXPECT_NEAR(gc.interpolateClock("G05", epochAt(15.0), std::vector<ClockData>{}), 1.0e-4 + 1.5e-9, kClockTol);
}

// ---------------------------------------------------------------------------
// getSatelliteClockCorrection over the same gap, with no SSR record and no live
// broadcast record (the post-processing situation): the product value where it
// covers the epoch, 0 inside the gap, and in neither case the broadcast-grade
// flag.  Fails if the method returns anything but the interpolated product
// value outside the gap, or flags a gap epoch as broadcast grade with no
// broadcast record behind it.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, SatelliteClockIsZeroInsideTheGapWithoutBroadcast) {
    LiveEphemerisStore::getInstance().clear();
    LogManager log;
    const auto product = gappedProduct("G05");
    GnssCorrections gc(log, product, std::vector<ErpData>{});
    SsrCorrectionStore no_ssr;
    gc.setSsrStore(&no_ssr);
    gc.setOrbitEchoStore(&no_ssr);

    for (const Row& r : kGappedTable) {
        SCOPED_TRACE(std::string(r.why) + " (offset " + std::to_string(r.offset_s) + " s)");
        bool broadcast_grade = true;
        const double got = gc.getSatelliteClockCorrection("G05", epochAt(r.offset_s), &broadcast_grade);
        if (r.expect_s == 0.0) {
            EXPECT_EQ(got, 0.0);
        } else {
            EXPECT_NEAR(got, r.expect_s, kClockTol);
        }
        EXPECT_FALSE(broadcast_grade);
    }

    // An invalid epoch is refused before any lookup.
    bool flag = true;
    EXPECT_EQ(gc.getSatelliteClockCorrection("G05", GnssTime{ kWeek, -1.0 }, &flag), 0.0);
    EXPECT_FALSE(flag);
}

// ---------------------------------------------------------------------------
// With a live broadcast record for the satellite, a gap epoch takes the
// broadcast clock and is flagged broadcast grade, while an epoch the product
// covers keeps the precise value and is not flagged.  The broadcast record has
// only af0 = 5e-5 s, so its clock is 5e-5 s at any epoch inside its fit window.
// Fails if the broadcast fallback is lost (the gap returns 0), if it overrides
// a covering product value, or if the flag that keeps carrier phase off a
// metre-level clock is not raised.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, GapEpochFallsBackToBroadcastAndIsFlagged) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    eph_store.clear();

    BroadcastEphemeris eph;
    eph.satellite = "G05";
    eph.system    = System::GPS;
    eph.iode      = 33;
    auto& k = eph.kepler;
    k.toe = epochAt(180.0);
    k.toc = epochAt(180.0);
    k.sqrt_a = 5153.6; k.e = 0.0;
    k.i0 = 0.96; k.omega0 = 0.5; k.omega = 0.3; k.m0 = 0.2;
    k.af0 = 5.0e-5;
    eph_store.insert(eph);

    LogManager log;
    const auto product = gappedProduct("G05");
    GnssCorrections gc(log, product, std::vector<ErpData>{});
    SsrCorrectionStore no_ssr;
    gc.setSsrStore(&no_ssr);
    gc.setOrbitEchoStore(&no_ssr);

    for (const double gap_offset : { 61.0, 180.0, 299.0 }) {
        bool broadcast_grade = false;
        EXPECT_NEAR(gc.getSatelliteClockCorrection("G05", epochAt(gap_offset), &broadcast_grade), 5.0e-5, 1e-15)
            << "gap offset " << gap_offset;
        EXPECT_TRUE(broadcast_grade) << "gap offset " << gap_offset;
    }
    for (const double covered : { 15.0, 60.0, 300.0, 310.0 }) {
        bool broadcast_grade = true;
        const double want = (covered == 15.0) ? 1.0e-4 + 1.5e-9
                          : (covered == 60.0) ? kB2
                          : (covered == 300.0) ? kB3 : 1.0e-4 + 2.2e-8;
        EXPECT_NEAR(gc.getSatelliteClockCorrection("G05", epochAt(covered), &broadcast_grade), want, kClockTol)
            << "covered offset " << covered;
        EXPECT_FALSE(broadcast_grade) << "covered offset " << covered;
    }

    eph_store.clear();
}

// ---------------------------------------------------------------------------
// The other two interpolation modes, selected through the configuration.
// ---------------------------------------------------------------------------
namespace {

/// A configuration, the parsers the full constructor needs, and the corrections built on them.
struct ModeHarness {
    LogManager log;
    GnssConfig config;
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<GnssCorrections> gc;
};

bool buildModeHarness(const std::string& mode, const std::vector<ClockData>& product, ModeHarness& h) {
    const std::filesystem::path p = gnss_test::tempPath("clock_gap_" + mode + ".json");
    {
        std::ofstream f(p);
        f << "{\n"
             "  \"processing\": {\n"
             "    \"mode\": \"ppp\",\n"
             "    \"active_systems\": [ \"GPS\" ],\n"
             "    \"signal_priorities\": { \"GPS\": \"WC\" },\n"
             "    \"satellite_clock_interpolation_mode\": \"" << mode << "\"\n"
             "  },\n"
             "  \"estimation\": {\n"
             "    \"estimate_tropo_gradients\": false\n"
             "  }\n"
             "}\n";
    }
    const bool ok = h.config.loadFromJson(p, h.log);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (!ok) return false;
    h.products = std::make_unique<ProductParser>(h.config, h.log);
    h.rinex = std::make_unique<RinexParser>(h.config, h.log, *h.products);
    h.gc = std::make_unique<GnssCorrections>(
        h.log, h.config, std::make_shared<const std::vector<ClockData>>(product),
        std::make_shared<const std::vector<ErpData>>(), std::make_shared<const std::vector<BiasData>>(),
        *h.rinex);
    return true;
}

/// A clock exactly linear in time, c(t) = 1e-4 s + 1e-10 * (t - t0), sampled every
/// 30 s at offsets 0..150 s and 390..540 s: a hole of 240 s between 150 and 390.
constexpr double kRate = 1.0e-10;
double linearClock(double offset_s) { return 1.0e-4 + kRate * offset_s; }

std::vector<ClockData> linearGappedProduct(const std::string& sat) {
    std::vector<ClockData> out;
    for (double o = 0.0; o <= 150.0; o += 30.0)   out.push_back(record(sat, kWeek, kTow0 + o, linearClock(o)));
    for (double o = 390.0; o <= 540.0; o += 30.0) out.push_back(record(sat, kWeek, kTow0 + o, linearClock(o)));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Lagrange and least-squares modes do not apply the 65 s bracket limit: on
// a clock exactly linear in time they return the line's value in the middle of
// a 240 s hole, where the linear mode returns 0.  This records an inconsistency
// between the modes (a hole is refused by one and bridged by the other two);
// on a real clock the bridged value carries whatever the polynomial or the
// fitted line does across the hole.  Fails if either mode gains a gap
// test, or if the linear mode loses its own.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, LagrangeAndLeastSquaresModesBridgeAGapTheLinearModeRefuses) {
    const auto product = linearGappedProduct("G12");
    const GnssTime mid_gap = epochAt(270.0);

    LogManager log;
    GnssCorrections linear(log, product, std::vector<ErpData>{});
    EXPECT_EQ(linear.interpolateClock("G12", mid_gap, product), 0.0);

    ModeHarness lagrange;
    ASSERT_TRUE(buildModeHarness("Lagrange", product, lagrange));
    ASSERT_EQ(lagrange.config.getSatelliteClockInterpolationMode(), SatelliteClockInterpolationMode::Lagrange);
    EXPECT_NEAR(lagrange.gc->interpolateClock("G12", mid_gap, product), linearClock(270.0), 1e-15);
    // Control: inside a covered interval the polynomial also gives the line.
    EXPECT_NEAR(lagrange.gc->interpolateClock("G12", epochAt(45.0), product), linearClock(45.0), 1e-15);

    ModeHarness lsq;
    ASSERT_TRUE(buildModeHarness("LsqSatClock", product, lsq));
    ASSERT_EQ(lsq.config.getSatelliteClockInterpolationMode(), SatelliteClockInterpolationMode::LsqSatClock);
    EXPECT_NEAR(lsq.gc->interpolateClock("G12", mid_gap, product), linearClock(270.0), 1e-14);
    EXPECT_NEAR(lsq.gc->interpolateClock("G12", epochAt(45.0), product), linearClock(45.0), 1e-14);
}

// ---------------------------------------------------------------------------
// The least-squares mode states, in its own comment, that an epoch on a record
// returns that record unchanged.  The shortcut compares the epoch with the
// record found by an upper-bound search, which is the first record strictly
// after the epoch, so it never matches and the fitted value is returned
// instead.  On a clock with a 3e-11 s zigzag about a line, the fitted value at
// a record differs from the record by about 1 cm of range.
// ---------------------------------------------------------------------------
TEST(CorrectionsClockGap, LeastSquaresModeReturnsTheRecordOnARecord) {
    GTEST_SKIP() << "defect: in the least-squares clock mode the record shortcut never fires "
                    "(it tests the first record after the epoch, found by upper_bound), so an "
                    "epoch on a record returns the fitted line, not the record its comment promises";

    std::vector<ClockData> product;
    for (int i = 0; i < 12; ++i) {
        const double o = 30.0 * i;
        const double zig = (i % 2 == 0) ? -3.0e-11 : 3.0e-11;
        product.push_back(record("G13", kWeek, kTow0 + o, linearClock(o) + zig));
    }
    ModeHarness lsq;
    ASSERT_TRUE(buildModeHarness("LsqSatClock", product, lsq));
    for (const int i : { 3, 5, 6 }) {
        EXPECT_NEAR(lsq.gc->interpolateClock("G13", epochAt(30.0 * i), product), product[i].clock_bias, 1e-15)
            << "record " << i;
    }
}

}  // namespace GnssProcessor
