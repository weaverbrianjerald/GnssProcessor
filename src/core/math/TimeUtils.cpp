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
#include <atomic>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include "logging/SafeLogger.hpp"
#include <sstream>
#include <iomanip>

namespace GnssProcessor {

    namespace {
        /// Per-call logger for the time-conversion path.  Replaces the bare
        /// spdlog free functions, which dereference a raw borrowed pointer into
        /// the registry default-logger slot and fault once that slot has been
        /// emptied.  Both call sites sit on error/unknown branches a well-formed
        /// file never reaches, so the registry lookup costs nothing on the normal
        /// path.  See logging/SafeLogger.hpp.
        std::shared_ptr<spdlog::logger> timeLog() {
            return Logging::safeDefaultLogger();
        }

        // GPS-UTC leap seconds used by the wall-clock → GPS conversion
        // (nowContinuousGpsSeconds).  Defaults to the compiled constant;
        // realtime.leap_seconds overrides it at RT startup, so a future
        // leap-second insertion does not require a rebuild.  Atomic: set on the
        // main thread, read from the receive and processing threads (RTB-1(c)).
        std::atomic<double> g_leap_seconds_override{ TimeUtils::LEAP_SECONDS };
    } // namespace

    void TimeUtils::setLeapSecondsOverride(double leap_s) {
        g_leap_seconds_override.store(leap_s, std::memory_order_relaxed);
    }

    double TimeUtils::currentLeapSeconds() {
        return g_leap_seconds_override.load(std::memory_order_relaxed);
    }

    // =================================================================
    // Leap second table
    // =================================================================

    // Each entry: { JD of 0h UTC on the insertion date, GPS-UTC offset from then on }.
    // The GPS epoch (1980-01-06) has GPS-UTC = 0.  Source: IERS Bulletin C.
    // The dates are UTC.  A UTC-scale JD compares with them directly; a GPS-scale JD is
    // first moved onto UTC, because the new offset takes effect when UTC reaches the
    // date, which is that many seconds (the new GPS-UTC) after 0 h on the GPS clock.
    struct LeapSecondEntry {
        double utc_jd;       ///< JD of 0 h UTC on the day the offset takes effect.
        double gps_utc;      ///< GPS - UTC in seconds from then on.
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
        { 2456109.5, 16.0 },   // 2012-07-01
        { 2457204.5, 17.0 },   // 2015-07-01
        { 2457754.5, 18.0 },   // 2017-01-01
    };
    static constexpr size_t LEAP_TABLE_SIZE = sizeof(LEAP_TABLE) / sizeof(LEAP_TABLE[0]);

    double TimeUtils::leapSecondsAtUtcJd(double utc_jd) {
        for (size_t i = LEAP_TABLE_SIZE; i > 0; --i)
            if (utc_jd >= LEAP_TABLE[i - 1].utc_jd)
                return LEAP_TABLE[i - 1].gps_utc;
        return 0.0;
    }

    double TimeUtils::leapSecondsGpsUtc(double gps_jd) {
        // An entry applies once UTC has reached its date.  The same instant on the GPS
        // scale is the entry's own GPS-UTC later, so the GPS JD is moved back by that
        // offset before the comparison.
        for (size_t i = LEAP_TABLE_SIZE; i > 0; --i) {
            const LeapSecondEntry& e = LEAP_TABLE[i - 1];
            if (gps_jd - e.gps_utc / static_cast<double>(SECONDS_IN_DAY) >= e.utc_jd)
                return e.gps_utc;
        }
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

    GnssTime TimeUtils::glonassTodToGpsTime(double tod_glonass_s, double anchor_gps_seconds) {
        // No usable anchor → leave the epoch unpromoted (caller falls back).
        if (anchor_gps_seconds <= 0.0) return GnssTime{ 0, 0.0 };

        constexpr double MOSCOW_OFFSET = 3.0 * 3600.0;   // GLONASS time = UTC(SU) + 3 h
        const double day_s    = static_cast<double>(SECONDS_IN_DAY);
        const double half_day = 0.5 * day_s;

        // Anchor: continuous GPS seconds → GPS JD → leap seconds → continuous UTC.
        const double gps_jd     = GPS_EPOCH_JD + anchor_gps_seconds / day_s;
        const double ls         = leapSecondsGpsUtc(gps_jd);
        const double utc_anchor = anchor_gps_seconds - ls;

        // Move the anchor into the Moscow-day frame and find that day's midnight.
        const double moscow_anchor = utc_anchor + MOSCOW_OFFSET;
        const double day_start     = std::floor(moscow_anchor / day_s) * day_s;
        const double anchor_tod    = moscow_anchor - day_start;

        // Place the reported time-of-day in the anchor's day, resolving the
        // midnight wrap toward the nearest match (±12 h).
        double tod = tod_glonass_s;
        if      (tod < anchor_tod - half_day) tod += day_s;
        else if (tod > anchor_tod + half_day) tod -= day_s;

        // Full Moscow seconds → UTC → GPS (re-add leap seconds at this epoch).
        const double moscow_full = day_start + tod;
        const double utc_full    = moscow_full - MOSCOW_OFFSET;
        const double gps_full    = utc_full + ls;
        return gpsSecondsToGnssTime(gps_full);
    }

    GnssTime TimeUtils::gregorianToGpsTime(int year, int month, int day,
        int hour, int minute, double second) {
        if (!isValidGregorianDate(year, month, day, hour, minute, second))
            throw std::invalid_argument("Invalid Gregorian date");

        double jd_utc = gregorianToJulianDay(year, month, day, hour, minute, second);
        double utc_since_epoch = (jd_utc - GPS_EPOCH_JD) * SECONDS_IN_DAY;
        double ls = leapSecondsAtUtcJd(jd_utc);
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
            timeLog()->warn("Invalid Gregorian date for {} time system: {}-{}-{} {}:{}:{:.3f}",
                time_system, year, month, day, hour, minute, second);
            return GnssTime{ 0, 0.0 };
        }

        // GPS / Galileo: already on the GPS time scale, no leap-second correction.
        if (time_system == "GPS" || time_system == "GAL" || time_system.empty()) {
            return gregorianGpsToGpsTime(year, month, day, hour, minute, second);
        }

        // BDT = GPS - 14 s, so add 14 s to get GPS time.
        if (time_system == "BDS") {
            GnssTime t = gregorianGpsToGpsTime(year, month, day, hour, minute, second);
            t.tow += 14.0;
            if (t.tow >= SECONDS_IN_WEEK) { t.tow -= SECONDS_IN_WEEK; t.week += 1; }
            return t;
        }

        // GLONASS: RINEX timestamps are UTC, so the leap seconds are applied.
        if (time_system == "GLO") {
            return gregorianToGpsTime(year, month, day, hour, minute, second);
        }

        // Unknown: assume the GPS scale.
        timeLog()->trace("Unsupported time system {}, assuming GPS time scale", time_system);
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

    long long TimeUtils::elapsedMs(double later_s, double earlier_s) {
        return std::llround((later_s - earlier_s) * 1.0e3);
    }

    bool TimeUtils::offsetReached(double now_s, double anchor_s, double offset_s) {
        // The comparison stays in double so an offset beyond the range of the integer
        // grid (never reachable by data) is a plain false instead of an overflow.
        return std::round((now_s - anchor_s) * 1.0e3) >= std::round(offset_s * 1.0e3);
    }

    double TimeUtils::nowContinuousGpsSeconds() {
        using namespace std::chrono;
        // POSIX system_clock is UTC without leap seconds.  GPS time runs ahead of
        // UTC by currentLeapSeconds() (the compiled LEAP_SECONDS unless overridden
        // via realtime.leap_seconds, RTB-1(c)), added once, as in the other
        // UTC→GPS conversions.  Returns continuous GPS seconds since the GPS epoch.
        const auto since = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        constexpr int64_t kGpsEpochUnix = 315964800; // Jan 6 1980 00:00:00 UTC
        return static_cast<double>(since - kGpsEpochUnix) + currentLeapSeconds();
    }

    GnssTime TimeUtils::nowGpsTime() {
        // Split nowContinuousGpsSeconds() into (week, tow), so the wall-clock → GPS
        // conversion and its leap-second offset live in one place.
        const double s = nowContinuousGpsSeconds();
        constexpr double kSecondsPerWeek = 604800.0;
        GnssTime t;
        t.week = static_cast<int>(std::floor(s / kSecondsPerWeek));
        t.tow  = s - static_cast<double>(t.week) * kSecondsPerWeek;
        return t;
    }

    // =================================================================
    // Calendar helpers
    // =================================================================

    bool TimeUtils::isValidGregorianDate(int year, int month, int day,
        int hour, int minute, double second) {
        // Year >= 1970 admits pre-GPS-epoch dates (Block I satellite ATX
        // VALID FROM lines from 1978, etc.).  These convert to negative GPS
        // weeks via gregorianGpsToGpsTime.  The satellite-antenna matcher
        // (Antenna.cpp) reads a valid_from week <= 0 as "no validity
        // information", so such an entry is used only as a fallback when no
        // dated entry covers the epoch.
        if (year < 1970 || month < 1 || month > 12 || day < 1 ||
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
        // No leap-second subtraction: the calendar fields stay on the TOW grid
        // (e.g. TOW=281400 → 06:10:00.000, not 06:09:42).
        julianDayToGregorian(jd_gps, year, month, day, hour, minute, second);
    }

} // namespace GnssProcessor