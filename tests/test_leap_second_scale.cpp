/**
 * @file test_leap_second_scale.cpp
 * @brief Unit tests for the time scale of the leap-second lookup in TimeUtils.
 *
 * The leap table is dated in UTC (the day an insertion takes effect, at 0 h UTC).  A lookup
 * keyed by a GPS-scale Julian date has to move the GPS instant onto the UTC scale before it
 * compares with those dates; comparing the raw GPS date makes the offset switch early, by
 * the new GPS-UTC offset in seconds, at every insertion.  A lookup keyed by a UTC-scale date
 * (the conversion from a calendar UTC time) compares directly.
 *
 * The tests walk the seconds around every insertion in the table and check that
 * UTC -> GPS -> UTC returns the calendar time it started from.
 */

#include <gtest/gtest.h>

#include "math/TimeUtils.hpp"

#include <array>
#include <cmath>

namespace GnssProcessor {
namespace {

/// One insertion: the UTC date it takes effect on, and the GPS-UTC offset from then on.
struct Insertion { int year, month, day; double offset_s; };

constexpr std::array<Insertion, 18> kInsertions = {{
    {1981, 7, 1, 1.0},  {1982, 7, 1, 2.0},  {1983, 7, 1, 3.0},  {1985, 7, 1, 4.0},
    {1988, 1, 1, 5.0},  {1990, 1, 1, 6.0},  {1991, 1, 1, 7.0},  {1992, 7, 1, 8.0},
    {1993, 7, 1, 9.0},  {1994, 7, 1, 10.0}, {1996, 1, 1, 11.0}, {1997, 7, 1, 12.0},
    {1999, 1, 1, 13.0}, {2006, 1, 1, 14.0}, {2009, 1, 1, 15.0}, {2012, 7, 1, 16.0},
    {2015, 7, 1, 17.0}, {2017, 1, 1, 18.0}}};

double gpsJdOf(const GnssTime& t) { return TimeUtils::gpsTimeToJulianDate(t); }

}  // namespace

// Just before and just after the insertion, on the GPS scale: the new offset takes effect
// 'offset' seconds into the GPS day (when UTC reaches 0 h), not at 0 h GPS.
TEST(LeapSecondScaleTest, GpsLookupSwitchesWhenUtcReachesTheDate) {
    for (const Insertion& ins : kInsertions) {
        // GPS time of 0 h UTC on the insertion date, from the UTC-to-GPS conversion.
        const GnssTime at = TimeUtils::gregorianToGpsTime(ins.year, ins.month, ins.day, 0, 0, 0.0);
        const double switch_s = TimeUtils::continuousGpsSeconds(at);
        auto leapAt = [&](double gps_s) {
            const int week = static_cast<int>(gps_s / TimeUtils::SECONDS_IN_WEEK);
            return TimeUtils::leapSecondsGpsUtc(
                gpsJdOf(GnssTime{week, gps_s - week * static_cast<double>(TimeUtils::SECONDS_IN_WEEK)}));
        };
        EXPECT_DOUBLE_EQ(leapAt(switch_s + 0.5), ins.offset_s) << ins.year << "-" << ins.month;
        EXPECT_DOUBLE_EQ(leapAt(switch_s - 0.5), ins.offset_s - 1.0) << ins.year << "-" << ins.month;
        // The instants the lookup used to get wrong: from 0 h GPS of the date (the old switch)
        // to the true switch, 'offset' seconds later.
        EXPECT_DOUBLE_EQ(leapAt(switch_s - ins.offset_s + 0.5), ins.offset_s - 1.0)
            << ins.year << "-" << ins.month;
    }
}

TEST(LeapSecondScaleTest, UtcLookupSwitchesAtTheDate) {
    for (const Insertion& ins : kInsertions) {
        const double jd = TimeUtils::gregorianToJulianDay(ins.year, ins.month, ins.day, 0, 0, 0.0);
        EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsAtUtcJd(jd + 1.0 / 86400.0), ins.offset_s);
        EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsAtUtcJd(jd - 1.0 / 86400.0), ins.offset_s - 1.0);
    }
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsAtUtcJd(TimeUtils::GPS_EPOCH_JD), 0.0);
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsAtUtcJd(0.0), 0.0);
}

// UTC -> GPS -> UTC returns the calendar time, through every second around every insertion.
// (The inserted second itself, 23:59:60, has no calendar input and is not walked.)
TEST(LeapSecondScaleTest, UtcGpsRoundTripAroundEveryInsertion) {
    for (const Insertion& ins : kInsertions) {
        const int prev_year = ins.month == 1 ? ins.year - 1 : ins.year;
        const int prev_month = ins.month == 1 ? 12 : ins.month - 1;
        const int prev_day = ins.month == 1 ? 31 : 30;   // 30 June, 31 December
        for (int s = 40; s >= 1; --s) {
            const int sec = 60 - s;
            const GnssTime gps = TimeUtils::gregorianToGpsTime(prev_year, prev_month, prev_day, 23, 59, sec);
            int y, mo, d, h, mi; double se;
            TimeUtils::gpsTimeToGregorian(gps, y, mo, d, h, mi, se);
            EXPECT_EQ(y, prev_year); EXPECT_EQ(mo, prev_month); EXPECT_EQ(d, prev_day);
            EXPECT_EQ(h, 23); EXPECT_EQ(mi, 59);
            EXPECT_NEAR(se, sec, 1e-3) << ins.year << "-" << ins.month << " 23:59:" << sec;
        }
        for (int sec = 0; sec <= 40; ++sec) {
            const GnssTime gps = TimeUtils::gregorianToGpsTime(ins.year, ins.month, ins.day, 0, 0, sec);
            int y, mo, d, h, mi; double se;
            TimeUtils::gpsTimeToGregorian(gps, y, mo, d, h, mi, se);
            EXPECT_EQ(y, ins.year); EXPECT_EQ(mo, ins.month); EXPECT_EQ(d, ins.day);
            EXPECT_EQ(h, 0); EXPECT_EQ(mi, 0);
            EXPECT_NEAR(se, sec, 1e-3) << ins.year << "-" << ins.month << " 00:00:" << sec;
        }
    }
}

// The window the lookup used to get wrong, on the last insertion (2017-01-01): a GPS time
// 10 s into the day is still 23:59:53 UTC on 31 December, GPS-UTC 17.
TEST(LeapSecondScaleTest, FirstSecondsOf2017AreStillSeventeen) {
    const GnssTime at = TimeUtils::gregorianToGpsTime(2017, 1, 1, 0, 0, 0.0);   // 18 s ahead of GPS 0 h
    const GnssTime ten_s{at.week, at.tow - 8.0};                               // GPS 0 h 00 m 10 s
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsGpsUtc(gpsJdOf(ten_s)), 17.0);
    int y, mo, d, h, mi; double se;
    TimeUtils::gpsTimeToGregorian(ten_s, y, mo, d, h, mi, se);
    EXPECT_EQ(y, 2016); EXPECT_EQ(mo, 12); EXPECT_EQ(d, 31);
    EXPECT_EQ(h, 23); EXPECT_EQ(mi, 59);
    EXPECT_NEAR(se, 53.0, 1e-3);
}

// Away from an insertion nothing moves: the current era is 18 s, TAI-UTC follows by 19 s.
TEST(LeapSecondScaleTest, CurrentEraIsUnchanged) {
    const double jd = gpsJdOf(GnssTime{2313, 259200.0});   // 2024-05-08
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsGpsUtc(jd), 18.0);
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsAtUtcJd(jd), 18.0);
    EXPECT_DOUBLE_EQ(TimeUtils::leapSecondsTaiUtc(jd), 18.0 + TimeUtils::TAI_GPS_OFFSET);
    EXPECT_NEAR(TimeUtils::gpsJdToUtcJd(jd), jd - 18.0 / 86400.0, 1e-12);
}

}  // namespace GnssProcessor
