/*!
 * \file TimeUtils.cpp
 * \brief GNSS time conversion utilities.
 *
 * Organization:
 *  - Leap second table
 *  - Julian Day <-> Gregorian
 *  - GPS time <-> Gregorian
 *  - Time-scale conversions (JD / MJD / TT / UTC)
 *  - Calendar helpers
 *  - Formatting
 */

#include "math/TimeUtils.hpp"
#include <cmath>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>

namespace GnssProcessor {

    // =================================================================
    // Leap second table
    // =================================================================

    // Each entry: { GPS_JD at insertion, GPS-UTC offset after insertion }.
    // GPS epoch (1980-01-06) has GPS-UTC = 0.  Source: IERS Bulletin C.
    struct LeapSecondEntry {
        double gps_jd;
        double gps_utc;
    };

    static constexpr LeapSecondEntry LEAP_TABLE[] = {
        { 2444244.5,  0.0 },   // 1980-01-06  GPS epoch
        { 2444786.5,  1.0 },   // 1981-07-01
        { 2445151.5,  2.0 },   // 1982-07-01
        { 2445516.5,  3.0 },   // 1983-07-01
        { 2446247.5,  4.0 },   // 1985-07-01
        { 2447161.5,  5.0 },   // 1988-01-01
        { 2447892.5,  6.0 },   // 1990-01-01
        { 2448257.5,  7.0 },   // 1991-01-01
        { 2448804.5,  8.0 },   // 1992-07-01
        { 2449169.5,  9.0 },   // 1993-07-01
        { 2449534.5, 10.0 },   // 1994-07-01
        { 2450083.5, 11.0 },   // 1996-01-01
        { 2450630.5, 12.0 },   // 1997-07-01
        { 2451179.5, 13.0 },   // 1999-01-01
        { 2453736.5, 14.0 },   // 2006-01-01
        { 2454832.5, 15.0 },   // 2009-01-01
        { 2455927.5, 16.0 },   // 2012-07-01
        { 2457204.5, 17.0 },   // 2015-07-01
        { 2457754.5, 18.0 },   // 2017-01-01
    };
    static constexpr size_t LEAP_TABLE_SIZE = sizeof(LEAP_TABLE) / sizeof(LEAP_TABLE[0]);

    double TimeUtils::leapSecondsGpsUtc(double gps_jd) {
        for (size_t i = LEAP_TABLE_SIZE; i > 0; --i)
            if (gps_jd >= LEAP_TABLE[i - 1].gps_jd)
                return LEAP_TABLE[i - 1].gps_utc;
        return 0.0;
    }

    double TimeUtils::leapSecondsTaiUtc(double gps_jd) {
        return leapSecondsGpsUtc(gps_jd) + TAI_GPS_OFFSET;
    }

    // =================================================================
    // Julian Day <-> Gregorian
    // =================================================================

    double TimeUtils::gregorianToJulianDay(int year, int month, int day,
        int hour, int minute, double second) {
        int y = year, m = month;
        if (m <= 2) { y -= 1; m += 12; }
        int a = y / 100;
        int b = 2 - a + a / 4;
        double jd = std::floor(365.25 * (y + 4716))
                   + std::floor(30.6001 * (m + 1))
                   + day + b - 1524.5;
        jd += (hour * 3600.0 + minute * 60.0 + second) / static_cast<double>(SECONDS_IN_DAY);
        return jd;
    }

    void TimeUtils::julianDayToGregorian(double jd, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        double jd_shifted = jd + 0.5;
        int z = static_cast<int>(std::floor(jd_shifted));
        double f = jd_shifted - z;

        int a = z;
        if (z >= 2299161) {
            int alpha = static_cast<int>(std::floor((z - 1867216.25) / 36524.25));
            a = z + 1 + alpha - static_cast<int>(std::floor(alpha / 4.0));
        }
        int bb = a + 1524;
        int c = static_cast<int>(std::floor((bb - 122.1) / 365.25));
        int d = static_cast<int>(std::floor(365.25 * c));
        int e = static_cast<int>(std::floor((bb - d) / 30.6001));

        day   = bb - d - static_cast<int>(std::floor(30.6001 * e));
        month = e < 14 ? e - 1 : e - 13;
        year  = month > 2 ? c - 4716 : c - 4715;

        double seconds = f * static_cast<double>(SECONDS_IN_DAY);
        hour   = static_cast<int>(seconds / 3600.0);
        seconds -= hour * 3600.0;
        minute = static_cast<int>(seconds / 60.0);
        second = seconds - minute * 60.0;
        if (second < 0.0) second = 0.0;
    }

    // =================================================================
    // GPS time <-> Gregorian
    // =================================================================

    // Shared helper: convert continuous GPS seconds to week + TOW.
    static GnssTime gpsSecondsToGnssTime(double gps_seconds) {
        int week  = static_cast<int>(gps_seconds / TimeUtils::SECONDS_IN_WEEK);
        double tow = gps_seconds - static_cast<double>(week) * TimeUtils::SECONDS_IN_WEEK;
        if (tow < 0.0) { tow += TimeUtils::SECONDS_IN_WEEK; week -= 1; }
        return GnssTime{ week, tow };
    }

    GnssTime TimeUtils::gregorianToGpsTime(int year, int month, int day,
        int hour, int minute, double second) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second))
            throw std::invalid_argument("Invalid Gregorian date");

        double jd_utc = gregorianToJulianDay(year, month, day, hour, minute, second);
        double utc_since_epoch = (jd_utc - GPS_EPOCH_JD) * SECONDS_IN_DAY;
        double ls = leapSecondsGpsUtc(jd_utc);
        return gpsSecondsToGnssTime(utc_since_epoch + ls);
    }

    GnssTime TimeUtils::gregorianGpsToGpsTime(int year, int month, int day,
        int hour, int minute, double second) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second))
            throw std::invalid_argument("Invalid Gregorian date");

        double jd_gps = gregorianToJulianDay(year, month, day, hour, minute, second);
        double gps_seconds = (jd_gps - GPS_EPOCH_JD) * SECONDS_IN_DAY;
        return gpsSecondsToGnssTime(gps_seconds);
    }

    GnssTime TimeUtils::GnssTimeFromDate(int year, int month, int day,
        int hour, int minute, double second,
        const std::string& time_system) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second)) {
            spdlog::warn("Invalid Gregorian date for {} time system: {}-{}-{} {}:{}:{:.3f}",
                time_system, year, month, day, hour, minute, second);
            return GnssTime{ 0, 0.0 };
        }

        // GPS / Galileo: already on GPS time scale  no leap second correction
        if (time_system == "GPS" || time_system == "GAL" || time_system.empty()) {
            return gregorianGpsToGpsTime(year, month, day, hour, minute, second);
        }

        // BDT = GPS - 14 s  ?  add 14 s to get GPS time
        if (time_system == "BDS") {
            GnssTime t = gregorianGpsToGpsTime(year, month, day, hour, minute, second);
            t.tow += 14.0;
            if (t.tow >= SECONDS_IN_WEEK) { t.tow -= SECONDS_IN_WEEK; t.week += 1; }
            return t;
        }

        // GLONASS: RINEX timestamps are UTC  apply leap seconds
        if (time_system == "GLO") {
            return gregorianToGpsTime(year, month, day, hour, minute, second);
        }

        // Unknown  assume GPS scale
        spdlog::trace("Unsupported time system {}, assuming GPS time scale", time_system);
        return gregorianGpsToGpsTime(year, month, day, hour, minute, second);
    }

    void TimeUtils::gpsTimeToGregorian(const GnssTime& gpsTime, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        double gps_seconds = static_cast<double>(gpsTime.week) * static_cast<double>(SECONDS_IN_WEEK) + gpsTime.tow;
        double gps_jd = GPS_EPOCH_JD + gps_seconds / static_cast<double>(SECONDS_IN_DAY);
        double utc_seconds = gps_seconds - leapSecondsGpsUtc(gps_jd);
        double jd_utc = GPS_EPOCH_JD + utc_seconds / static_cast<double>(SECONDS_IN_DAY);
        julianDayToGregorian(jd_utc, year, month, day, hour, minute, second);
    }

    // =================================================================
    // Time-scale conversions (JD / MJD / TT / UTC)
    // =================================================================

    double TimeUtils::gpsTimeToJulianDate(int week, double tow) {
        if (week < 0 || tow < 0.0 || tow >= static_cast<double>(SECONDS_IN_WEEK))
            throw std::invalid_argument("Invalid GPS time: week " + std::to_string(week) + ", tow " + std::to_string(tow));
        return GPS_EPOCH_JD + (static_cast<double>(week) * SECONDS_IN_WEEK + tow) / static_cast<double>(SECONDS_IN_DAY);
    }

    double TimeUtils::gpsTimeToJulianDate(const GnssTime& time) {
        return gpsTimeToJulianDate(time.week, time.tow);
    }

    double TimeUtils::gpsTimeToMjd(const GnssTime& time) {
        return gpsTimeToJulianDate(time) - MJD_OFFSET;
    }

    double TimeUtils::gpsTimeToUtcMjd(const GnssTime& time) {
        return gpsJdToUtcJd(gpsTimeToJulianDate(time)) - MJD_OFFSET;
    }

    double TimeUtils::gpsJdToUtcJd(double gps_jd) {
        return gps_jd - leapSecondsGpsUtc(gps_jd) / static_cast<double>(SECONDS_IN_DAY);
    }

    double TimeUtils::gpsJdToTtJd(double gps_jd) {
        return gps_jd + TT_GPS_OFFSET / static_cast<double>(SECONDS_IN_DAY);
    }

    double TimeUtils::gpsTimeToTtCenturies(const GnssTime& time) {
        return jdToJ2000Centuries(gpsJdToTtJd(gpsTimeToJulianDate(time)));
    }

    double TimeUtils::jdToJ2000Centuries(double jd) {
        return (jd - J2000_JD) / DAYS_PER_JULIAN_CENTURY;
    }

    double TimeUtils::mjdToDecimalYear(double mjd) {
        return 2000.0 + (mjd - J2000_MJD) / 365.25;
    }

    double TimeUtils::continuousGpsSeconds(const GnssTime& t) {
        return static_cast<double>(t.week) * static_cast<double>(SECONDS_IN_WEEK) + t.tow;
    }

    // =================================================================
    // Calendar helpers
    // =================================================================

    bool TimeUtils::isValidGregorianDate(int year, int month, int day,
        int hour, int minute, double second) {
        if (year < 1980 || month < 1 || month > 12 || day < 1 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            second < 0.0 || second >= 60.0)
            return false;
        return day <= daysInMonth(year, month);
    }

    int TimeUtils::daysInMonth(int year, int month) {
        switch (month) {
        case 2:  return isLeapYear(year) ? 29 : 28;
        case 4: case 6: case 9: case 11: return 30;
        default: return 31;
        }
    }

    bool TimeUtils::isLeapYear(int year) {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    bool TimeUtils::dayOfYearToDate(int year, int doy, int& month, int& day) {
        if (doy < 1) return false;
        int max_doy = isLeapYear(year) ? 366 : 365;
        if (doy > max_doy) return false;

        month = 1;
        day = doy;
        while (day > daysInMonth(year, month)) {
            day -= daysInMonth(year, month);
            ++month;
        }
        return true;
    }

    // =================================================================
    // Formatting
    // =================================================================

    std::string TimeUtils::formatGnssTime(const GnssTime& time) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
        double second = 0.0;
        gpsTimeToGregorian(time, year, month, day, hour, minute, second);

        std::ostringstream oss;
        oss << "GPS week " << time.week
            << ", TOW " << std::fixed << std::setprecision(3) << time.tow
            << " (" << std::setfill('0')
            << std::setw(4) << year << "-"
            << std::setw(2) << month << "-"
            << std::setw(2) << day << " "
            << std::setw(2) << hour << ":"
            << std::setw(2) << minute << ":"
            << std::setw(6) << std::fixed << std::setprecision(3) << second
            << " UTC)";
        return oss.str();
    }

    void TimeUtils::gpsTimeToGpsScaleCalendar(const GnssTime& gpsTime, int& year, int& month,
                                              int& day, int& hour, int& minute, double& second) {
        const double gps_seconds = static_cast<double>(gpsTime.week)
                                 * static_cast<double>(SECONDS_IN_WEEK) + gpsTime.tow;
        const double jd_gps = GPS_EPOCH_JD + gps_seconds / static_cast<double>(SECONDS_IN_DAY);
        // No leap-second subtraction — the calendar fields end up on the
        // same grid as TOW (e.g. TOW=281400 → 06:10:00.000, not 06:09:42).
        julianDayToGregorian(jd_gps, year, month, day, hour, minute, second);
    }

} // namespace GnssProcessor