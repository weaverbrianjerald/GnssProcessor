#include "TimeUtils.hpp"
#include <cmath>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>

namespace GnssProcessor {

    // -----------------------------------------------------------------------
    // Leap second table (GPS - UTC offsets)
    //
    // Each entry is { GPS_JD_of_insertion, GPS_UTC_offset_after }.
    // GPS epoch is 1980-01-06 00:00:00 UTC with 0 leap seconds.
    // At that instant TAI - UTC = 19 s, and GPS = TAI - 19 = UTC.
    //
    // GPS_JD = UTC_JD + (GPS-UTC)/86400, but since leap seconds are
    // inserted at UTC midnight the GPS JD values below are computed as
    // UTC_JD_midnight + previous_GPS_UTC_offset / 86400 for consistency.
    //
    // Source: IERS Bulletin C / USNO.
    // -----------------------------------------------------------------------
    struct LeapSecondEntry {
        double gps_jd;       // GPS JD at which this offset becomes valid
        double gps_utc;      // GPS - UTC after this insertion (seconds)
    };

    // Table of leap second insertions since GPS epoch (1980-01-06).
    // Before GPS epoch, GPS-UTC = 0 by definition.
    static constexpr LeapSecondEntry LEAP_TABLE[] = {
        // GPS epoch: 1980-01-06.  GPS-UTC = 0 at start.
        { 2444244.5,  0.0 },   // 1980-01-06: GPS epoch, GPS = UTC
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
        // Walk backwards through the table to find the applicable offset
        for (size_t i = LEAP_TABLE_SIZE; i > 0; --i) {
            if (gps_jd >= LEAP_TABLE[i - 1].gps_jd) {
                return LEAP_TABLE[i - 1].gps_utc;
            }
        }
        return 0.0; // Before GPS epoch
    }

    double TimeUtils::leapSecondsTaiUtc(double gps_jd) {
        return leapSecondsGpsUtc(gps_jd) + TAI_GPS_OFFSET;
    }

    /**
     * @brief Converts Gregorian date and time (UTC) to GPS time.
     *
     * Uses the leap second table to determine the correct GPS-UTC offset
     * for the given date, rather than a single hardcoded value.
     */
    GnssTime TimeUtils::gregorianToGpsTime(int year, int month, int day, int hour, int minute, double second) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second)) {
            throw std::invalid_argument("Invalid Gregorian date");
        }

        // Compute Julian Day (this is on the UTC time scale)
        double jd_utc = gregorianToJulianDay(year, month, day, hour, minute, second);

        // Compute UTC seconds since GPS epoch
        double utc_seconds_since_epoch = (jd_utc - GPS_EPOCH_JD) * SECONDS_IN_DAY;

        // Approximate GPS JD to look up leap seconds (iterate once for precision)
        double approx_gps_jd = jd_utc; // First approximation
        double ls = leapSecondsGpsUtc(approx_gps_jd);

        // Convert UTC to GPS time by adding leap seconds
        double gps_seconds = utc_seconds_since_epoch + ls;

        // Compute GPS week and TOW
        int week = static_cast<int>(gps_seconds / SECONDS_IN_WEEK);
        double tow = gps_seconds - static_cast<double>(week) * SECONDS_IN_WEEK;

        // Ensure TOW is non-negative
        if (tow < 0.0) {
            tow += SECONDS_IN_WEEK;
            week -= 1;
        }

        return GnssTime{ week, tow };
    }

    /**
     * @brief Converts date and time to GNSS time for a specified time system.
     */
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
            // Galileo System Time (GST) is steered to GPS time at the ns level.
            // No adjustment needed for our precision requirements.
            spdlog::trace("Converted to GST for {}-{}-{} {}:{}:{:.3f}, week: {}, tow: {:.3f}",
                year, month, day, hour, minute, second, gps_time.week, gps_time.tow);
        }
        else if (time_system == "BDS") {
            // BeiDou Time (BDT) = GPS - 14 seconds (BDT epoch is 2006-01-01 00:00:00 UTC)
            gps_time.tow -= 14.0;
            if (gps_time.tow < 0.0) {
                gps_time.tow += SECONDS_IN_WEEK;
                gps_time.week -= 1;
            }
            spdlog::trace("Converted to BDT for {}-{}-{} {}:{}:{:.3f}, week: {}, tow: {:.3f}",
                year, month, day, hour, minute, second, gps_time.week, gps_time.tow);
        }
        else if (time_system == "GLO") {
            // GLONASS time = UTC + 3 hours (Moscow time), but RINEX timestamps
            // are typically already in UTC. No adjustment applied here; the caller
            // is expected to provide UTC-referenced inputs.
            spdlog::trace("GLONASS time system: assuming UTC input for {}-{}-{} {}:{}:{:.3f}",
                year, month, day, hour, minute, second);
        }
        else if (time_system != "GPS") {
            spdlog::trace("Unsupported time system {}, using GPS time", time_system);
        }

        return gps_time;
    }

    /**
     * @brief Converts GPS time to Gregorian date and time (UTC).
     *
     * Uses the leap second table to correctly convert GPS seconds back to UTC.
     */
    void TimeUtils::gpsTimeToGregorian(const GnssTime& gpsTime, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        double gps_seconds = static_cast<double>(gpsTime.week) * static_cast<double>(SECONDS_IN_WEEK) + gpsTime.tow;

        // Compute GPS JD for leap second lookup
        double gps_jd = GPS_EPOCH_JD + gps_seconds / static_cast<double>(SECONDS_IN_DAY);
        double ls = leapSecondsGpsUtc(gps_jd);

        // Subtract leap seconds to convert GPS -> UTC
        double utc_seconds_since_epoch = gps_seconds - ls;

        double jd_utc = GPS_EPOCH_JD + utc_seconds_since_epoch / static_cast<double>(SECONDS_IN_DAY);
        julianDayToGregorian(jd_utc, year, month, day, hour, minute, second);
    }

    /**
     * @brief Converts Gregorian date and time to Julian Day.
     *
     * Standard algorithm valid for all Gregorian calendar dates.
     * The returned JD is on whatever time scale the input represents
     * (typically UTC for calendar dates).
     */
    double TimeUtils::gregorianToJulianDay(int year, int month, int day, int hour, int minute, double second) {
        // Adjust year/month for the algorithm (Jan/Feb are months 13/14 of previous year)
        int y = year;
        int m = month;
        if (m <= 2) {
            y -= 1;
            m += 12;
        }
        int a = y / 100;
        int b = 2 - a + a / 4;
        double jd = std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + day + b - 1524.5;
        jd += (hour * 3600.0 + minute * 60.0 + second) / static_cast<double>(SECONDS_IN_DAY);
        return jd;
    }

    /**
     * @brief Converts Julian Day to Gregorian date and time.
     */
    void TimeUtils::julianDayToGregorian(double jd, int& year, int& month, int& day,
        int& hour, int& minute, double& second) {
        // Shift by 0.5 to align with calendar day boundaries
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

        day = bb - d - static_cast<int>(std::floor(30.6001 * e));
        month = e < 14 ? e - 1 : e - 13;
        year = month > 2 ? c - 4716 : c - 4715;

        double seconds = f * static_cast<double>(SECONDS_IN_DAY);
        hour = static_cast<int>(seconds / 3600.0);
        seconds -= hour * 3600.0;
        minute = static_cast<int>(seconds / 60.0);
        second = seconds - minute * 60.0;

        // Clamp tiny negative remainders from floating-point arithmetic
        if (second < 0.0) second = 0.0;
    }

    bool TimeUtils::isValidGregorianDate(int year, int month, int day, int hour, int minute, double second) {
        if (year < 1980 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0.0 || second >= 60.0) {
            return false;
        }
        int max_day = daysInMonth(year, month);
        return day <= max_day;
    }

    /**
     * @brief Converts GPS week and TOW to Julian Date (GPS time scale).
     *
     * The returned JD is on the GPS time scale. GPS seconds are added directly
     * to the GPS epoch JD. Since GPS time is continuous (no leap seconds),
     * the result differs from a UTC JD by GPS-UTC leap seconds / 86400.
     */
    double TimeUtils::gpsTimeToJulianDate(int week, double tow) {
        if (week < 0 || tow < 0.0 || tow >= static_cast<double>(SECONDS_IN_WEEK)) {
            throw std::invalid_argument("Invalid GPS time: week " + std::to_string(week) + ", tow " + std::to_string(tow));
        }
        double seconds_since_gps_epoch = static_cast<double>(week) * SECONDS_IN_WEEK + tow;
        double days_since_gps_epoch = seconds_since_gps_epoch / static_cast<double>(SECONDS_IN_DAY);
        return GPS_EPOCH_JD + days_since_gps_epoch;
    }

    double TimeUtils::gpsTimeToMjd(const GnssTime& time) {
        double jd = gpsTimeToJulianDate(time.week, time.tow);
        return jd - MJD_OFFSET;
    }

    double TimeUtils::gpsTimeToUtcMjd(const GnssTime& time) {
        double gps_jd = gpsTimeToJulianDate(time.week, time.tow);
        double utc_jd = gpsJdToUtcJd(gps_jd);
        return utc_jd - MJD_OFFSET;
    }

    double TimeUtils::gpsTimeToJulianDate(const GnssTime& time) {
        return gpsTimeToJulianDate(time.week, time.tow);
    }

    double TimeUtils::gpsJdToUtcJd(double gps_jd) {
        double ls = leapSecondsGpsUtc(gps_jd);
        return gps_jd - ls / static_cast<double>(SECONDS_IN_DAY);
    }

    double TimeUtils::gpsJdToTtJd(double gps_jd) {
        return gps_jd + TT_GPS_OFFSET / static_cast<double>(SECONDS_IN_DAY);
    }

    double TimeUtils::gpsTimeToTtCenturies(const GnssTime& time) {
        double gps_jd = gpsTimeToJulianDate(time.week, time.tow);
        double tt_jd = gpsJdToTtJd(gps_jd);
        return jdToJ2000Centuries(tt_jd);
    }

    double TimeUtils::jdToJ2000Centuries(double jd) {
        return (jd - J2000_JD) / DAYS_PER_JULIAN_CENTURY;
    }

    double TimeUtils::mjdToDecimalYear(double mjd) {
        return 2000.0 + (mjd - J2000_MJD) / 365.25;
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

    std::string TimeUtils::formatGnssTime(const GnssTime& time) {
        std::ostringstream oss;
        oss << "GPS week " << time.week << ", TOW " << std::fixed << std::setprecision(3) << time.tow;

        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
        double second = 0.0;
        gpsTimeToGregorian(time, year, month, day, hour, minute, second);

        oss << " (" << std::setfill('0')
            << std::setw(4) << year << "-"
            << std::setw(2) << month << "-"
            << std::setw(2) << day << " "
            << std::setw(2) << hour << ":"
            << std::setw(2) << minute << ":"
            << std::setw(6) << std::fixed << std::setprecision(3) << second << " UTC)";
        return oss.str();
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

    double TimeUtils::continuousGpsSeconds(const GnssTime& t) {
        return static_cast<double>(t.week) * static_cast<double>(SECONDS_IN_WEEK) + t.tow;
    }

} // namespace GnssProcessor