#include "TimeUtils.hpp"
#include <cmath>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    GnssTime TimeUtils::gregorianToGpsTime(int year, int month, int day, int hour, int minute, double second) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second)) {
            throw std::invalid_argument("Invalid Gregorian date");
        }

        // Compute Julian Day
        double jd = gregorianToJulianDay(year, month, day, hour, minute, second);

        // Compute days since GPS epoch (1980-01-06 00:00:00 UTC)
        double gps_days = jd - GPS_EPOCH_JD;

        // Compute GPS seconds, accounting for leap seconds (approximate, adjust if precise table available)
        double gps_seconds = gps_days * SECONDS_IN_DAY;
        // Approximate leap seconds correction (e.g., 18 seconds as of 2017)
        constexpr double leap_seconds = 18.0; // Update based on RINEX file or external data
        gps_seconds -= leap_seconds;

        // Compute GPS week and TOW
        int week = static_cast<int>(gps_seconds / SECONDS_IN_WEEK);
        double tow = gps_seconds - week * SECONDS_IN_WEEK;

        // Ensure TOW is non-negative and handles sub-second precision
        if (tow < 0.0) {
            tow += SECONDS_IN_WEEK;
            week -= 1;
        }

        return GnssTime{ week, tow };
    }

    GnssTime TimeUtils::GnssTimeFromDate(int year, int month, int day, int hour, int minute, double second,
        const std::string& time_system) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second)) {
            spdlog::warn("Invalid Gregorian date for {} time system: {}-{}-{} {}:{}:{:.3f}",
                time_system, year, month, day, hour, minute, second);
            return GnssTime{ 0, 0.0 };
        }

        // Convert to GPS time as baseline
        GnssTime gps_time = gregorianToGpsTime(year, month, day, hour, minute, second);

        // Adjust for time system (if not GPS)
        if (time_system == "GAL") {
            // Galileo System Time (GST) offset from GPS time (typically small, ~10-20 ns)
            // For simplicity, assume aligned with GPS time (no leap second offset applied)
            // Actual offset requires leap second data or external correction
            spdlog::trace("Converted to GST for {}-{}-{} {}:{}:{:.3f}, week: {}, tow: {:.3f}",
                year, month, day, hour, minute, second, gps_time.week, gps_time.tow);
        }
        else if (time_system == "BDS") {
            // BeiDou Time (BDT) is offset from GPS by 14 seconds (as of 2006-01-01)
            gps_time.tow -= 14.0;
            if (gps_time.tow < 0.0) {
                gps_time.tow += SECONDS_IN_WEEK;
                gps_time.week -= 1;
            }
            spdlog::trace("Converted to BDT for {}-{}-{} {}:{}:{:.3f}, week: {}, tow: {:.3f}",
                year, month, day, hour, minute, second, gps_time.week, gps_time.tow);
        }
        else if (time_system != "GPS") {
            spdlog::warn("Unsupported time system {}, using GPS time", time_system);
        }

        return gps_time;
    }

    void TimeUtils::gpsTimeToGregorian(const GnssTime& gpsTime, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        int64_t gps_seconds = gpsTime.week * SECONDS_IN_WEEK + static_cast<int64_t>(gpsTime.tow);
        double jd = GPS_EPOCH_JD + gps_seconds / static_cast<double>(SECONDS_IN_DAY);
        julianDayToGregorian(jd, year, month, day, hour, minute, second);
    }

    double TimeUtils::gregorianToJulianDay(int year, int month, int day, int hour, int minute, double second) {
        if (month <= 2) {
            year -= 1;
            month += 12;
        }
        int a = year / 100;
        int b = 2 - a + a / 4;
        double jd = std::floor(365.25 * (year + 4716)) + std::floor(30.6001 * (month + 1)) + day + b - 1524.5;
        jd += (hour * 3600.0 + minute * 60.0 + second) / SECONDS_IN_DAY;
        return jd;
    }

    void TimeUtils::julianDayToGregorian(double jd, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        jd += 0.5;
        int z = std::floor(jd);
        double f = jd - z;

        int a = z;
        if (z >= 2299161) {
            int alpha = std::floor((z - 1867216.25) / 36524.25);
            a = z + 1 + alpha - std::floor(alpha / 4);
        }
        int b = a + 1524;
        int c = std::floor((b - 122.1) / 365.25);
        int d = std::floor(365.25 * c);
        int e = std::floor((b - d) / 30.6001);

        day = b - d - std::floor(30.6001 * e);
        month = e < 14 ? e - 1 : e - 13;
        year = month > 2 ? c - 4716 : c - 4715;

        double seconds = f * SECONDS_IN_DAY;
        hour = seconds / 3600;
        seconds -= hour * 3600;
        minute = seconds / 60;
        second = seconds - minute * 60;
    }

    bool TimeUtils::isValidGregorianDate(int year, int month, int day, int hour, int minute, double second) {
        if (year < 1980 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0 || second >= 60) {
            return false;
        }
        int max_day = daysInMonth(year, month);
        return day <= max_day;
    }

    double TimeUtils::gpsTimeToJulianDate(int week, double tow) {
        if (week < 0 || tow < 0.0 || tow >= 604800.0) {
            throw std::invalid_argument("Invalid GPS time: week " + std::to_string(week) + ", tow " + std::to_string(tow));
        }
        // GPS epoch: 1980-01-06 00:00:00 UTC (JD 2444244.5)
        double seconds_since_gps_epoch = static_cast<double>(week) * SECONDS_IN_WEEK + tow;
        double days_since_gps_epoch = seconds_since_gps_epoch / SECONDS_IN_DAY;
        return GPS_EPOCH_JD + days_since_gps_epoch;
    }

    int TimeUtils::daysInMonth(int year, int month) {
        switch (month) {
        case 2: return isLeapYear(year) ? 29 : 28;
        case 4: case 6: case 9: case 11: return 30;
        default: return 31;
        }
    }

    bool TimeUtils::isLeapYear(int year) {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

} // namespace GnssProcessor