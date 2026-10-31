/**
 * @file test_gps_scale_stamp.cpp
 * @brief Printed epoch stamps carry a rounding into the minute.
 *
 * Every log line's epoch field (%E) printed the second of
 * @c TimeUtils::gpsTimeToGpsScaleCalendar with "%06.3f".  That conversion runs through a
 * Julian-day double (about 40 microseconds of resolution), so a whole minute could come
 * back as HH:MM:59.99996 and print as "HH:MM:60.000" with no minute carry: 60 of 240
 * stamps on a 2 h ionosphere-free arm.  The stamp now comes from
 * @c TimeUtils::gpsTimeToGpsScaleCalendarMs, one rounding to integer milliseconds and
 * integer arithmetic after it.  Guarded here: the carry through the minute, the hour,
 * the date, the year and the week; a stamp that is not on a rounding boundary prints as
 * before; the log formatter itself prints the carried stamp; the UTC stamp of
 * @c formatGnssTime takes the same path.
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GNSS_TEST_GETPID _getpid
#else
#include <unistd.h>
#define GNSS_TEST_GETPID getpid
#endif

using namespace GnssProcessor;

namespace {

/// The stamp text "YYYY-MM-DD HH:MM:SS.mmm" from the millisecond fields.
std::string msStamp(const GnssTime& t) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, ms = 0;
    TimeUtils::gpsTimeToGpsScaleCalendarMs(t, y, mo, d, h, mi, ms);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  y, mo, d, h, mi, ms / 1000, ms % 1000);
    return buf;
}

/// The stamp text as the formatter printed it before the fix (double second, "%06.3f").
std::string doubleStamp(const GnssTime& t) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double s = 0.0;
    TimeUtils::gpsTimeToGpsScaleCalendar(t, y, mo, d, h, mi, s);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%06.3f", y, mo, d, h, mi, s);
    return buf;
}

// GPS week 2313 starts on Sunday 2024-05-05; TOW 259200 is 2024-05-08 00:00:00 (DOY 129).
constexpr int kWeek = 2313;
constexpr double kDoy129 = 259200.0;

}  // namespace

// The epoch the 2 h arm printed as "00:00:60.000" is the next minute's ":00.000".
TEST(GpsScaleStamp, WholeMinuteCarriesIntoTheMinute) {
    EXPECT_EQ(msStamp({ kWeek, kDoy129 + 60.0 }), "2024-05-08 00:01:00.000");
    EXPECT_EQ(msStamp({ kWeek, kDoy129 + 120.0 }), "2024-05-08 00:02:00.000");
}

// The carry reaches the hour, the date, the year and the next GPS week.
TEST(GpsScaleStamp, CarryReachesTheHourDateYearAndWeek) {
    EXPECT_EQ(msStamp({ kWeek, kDoy129 + 3600.0 }), "2024-05-08 01:00:00.000");
    EXPECT_EQ(msStamp({ kWeek, kDoy129 + 86400.0 }), "2024-05-09 00:00:00.000");
    // Half a millisecond short of midnight rounds up into the next day.
    EXPECT_EQ(msStamp({ kWeek, kDoy129 + 86399.9996 }), "2024-05-09 00:00:00.000");
    // Week 2295 starts on Sunday 2023-12-31: the carry crosses the year.
    EXPECT_EQ(msStamp({ 2295, 86399.9996 }), "2024-01-01 00:00:00.000");
    // A TOW at the week length is the next week's Sunday midnight.
    EXPECT_EQ(msStamp({ kWeek, 604800.0 }), "2024-05-12 00:00:00.000");
    // A negative TOW borrows from the previous day.
    EXPECT_EQ(msStamp({ kWeek, -60.0 }), "2024-05-04 23:59:00.000");
}

// Every whole second of DOY 129 prints its own clock time, computed here by integer
// arithmetic; the stamp printed before the fix differs only where it read ":60.000".
// Off a rounding boundary (two fractional offsets of every second) the stamp is the
// one printed before the fix.
TEST(GpsScaleStamp, NonBoundaryStampsAreUnchanged) {
    int carried = 0;
    for (int s = 0; s < 86400; ++s) {
        const GnssTime t{ kWeek, kDoy129 + s };
        char expected[40];
        std::snprintf(expected, sizeof(expected), "2024-05-08 %02d:%02d:%02d.000",
                      s / 3600, (s / 60) % 60, s % 60);
        const std::string after = msStamp(t);
        ASSERT_EQ(after, expected) << "TOW " << t.tow;
        const std::string before = doubleStamp(t);
        if (before != after) {
            ++carried;
            ASSERT_EQ(before.compare(17, 6, "60.000"), 0) << before << " against " << after;
            ASSERT_EQ(s % 60, 0) << "only a whole minute rounds onto :60.000";
        }
        for (const double frac : { 0.25, 0.5004 }) {
            const GnssTime tf{ kWeek, kDoy129 + s + frac };
            ASSERT_EQ(msStamp(tf), doubleStamp(tf)) << "TOW " << tf.tow;
        }
    }
    // The defect is real on this day: the double path rounds some whole minutes down.
    EXPECT_GT(carried, 0);
}

// The log formatter (%E) prints the carried stamp.  Fails before the fix, where this
// line read "(2024-05-08 00:00:60.000)".
TEST(GpsScaleStamp, LogFormatterPrintsTheCarriedStamp) {
    namespace fs = std::filesystem;
    // Per-process directory: concurrent test binaries share the temp directory.
    const fs::path dir = fs::temp_directory_path()
                         / ("stamp_logs_" + std::to_string(GNSS_TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string name = "stamp_carry";
    spdlog::drop("gnss_processor");
    spdlog::drop(name);
    std::string text;
    {
        LogManager lm;
        lm.initialize(dir, LogLevel::Info);
        auto logger = lm.getLogger(name);
        LogManager::setCurrentEpochTime({ kWeek, kDoy129 + 60.0 });
        logger->info("whole-minute epoch");
        LogManager::clearCurrentEpochTime();
        logger->flush();
        spdlog::drop(name);
        spdlog::drop("gnss_processor");
    }
    {
        std::ifstream f(dir / (name + ".log"));
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    fs::remove_all(dir, ec);
    EXPECT_NE(text.find("[2313, 259260.000 (2024-05-08 00:01:00.000)]"), std::string::npos)
        << text;
    EXPECT_EQ(text.find(":60.000"), std::string::npos) << text;
}

// formatGnssTime prints the UTC stamp through the same path: GPS TOW 259278 is UTC
// 00:01:00 on DOY 129 (18 leap seconds).
TEST(GpsScaleStamp, UtcStampTakesTheSamePath) {
    EXPECT_EQ(TimeUtils::formatGnssTime({ kWeek, kDoy129 + 78.0 }),
              "GPS week 2313, TOW 259278.000 (2024-05-08 00:01:00.000 UTC)");
    // The leap-second shift borrows across the date.
    EXPECT_EQ(TimeUtils::formatGnssTime({ kWeek, kDoy129 }),
              "GPS week 2313, TOW 259200.000 (2024-05-07 23:59:42.000 UTC)");
}
