#pragma once

#include <cstdint>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Represents a GNSS time consisting of GPS week and time of week (TOW).
     *
     * This struct encapsulates the GPS week number and the seconds into that week.
     * It provides comparison operators for ordering and equality checks.
     */
    struct GnssTime {
        int week{ 0 }; ///< GPS week number (weeks since GPS epoch, January 6, 1980).
        double tow{ 0.0 }; ///< Time of week in seconds (0 to 604799.999...).

        /**
         * @brief Equality operator.
         * @param other The other GnssTime to compare with.
         * @return True if both week and tow are equal, false otherwise.
         */
        inline bool operator==(const GnssTime& other) const {
            return week == other.week && tow == other.tow;
        }

        /**
         * @brief Less-than operator for ordering.
         * @param other The other GnssTime to compare with.
         * @return True if this time is earlier than the other, false otherwise.
         */
        inline bool operator<(const GnssTime& other) const {
            return (week < other.week) || (week == other.week && tow < other.tow);
        }
    };

    /**
     * @brief Utility class for GNSS time conversions and operations.
     *
     * This class provides static methods for converting between various time systems,
     * such as Gregorian dates, GPS time, Julian days, and more. It also includes
     * validation and formatting utilities.
     */
    class TimeUtils {
    public:
        /**
         * @brief Converts Gregorian date and time to GPS time.
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return GnssTime struct with GPS week and TOW.
         */
        static GnssTime gregorianToGpsTime(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Converts Gregorian date and time already on the GPS time scale
         *        to GPS week and TOW � without applying any leap-second correction.
         *
         * Use this for timestamps that are already expressed in GPS time (e.g.
         * SP3 / CLK product epochs).  For UTC timestamps use gregorianToGpsTime()
         * which adds the appropriate leap-second offset.
         *
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return GnssTime struct with GPS week and TOW.
         */
        static GnssTime gregorianGpsToGpsTime(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Converts date and time to GNSS time for a specified time system.
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @param time_system Time system string (e.g., "GPS", "GAL", "BDS").
         * @return GnssTime struct adjusted for the specified system.
         */
        static GnssTime GnssTimeFromDate(int year, int month, int day, int hour, int minute, double second, const std::string& time_system);

        /**
         * @brief Converts GPS time to Gregorian date and time.
         * @param gpsTime The GPS time to convert.
         * @param year Output: Year.
         * @param month Output: Month (1-12).
         * @param day Output: Day of month (1-31).
         * @param hour Output: Hour (0-23).
         * @param minute Output: Minute (0-59).
         * @param second Output: Second (0-59.999...).
         */
        static void gpsTimeToGregorian(const GnssTime& gpsTime, int& year, int& month, int& day,
            int& hour, int& minute, double& second);

        /**
         * @brief Converts Gregorian date and time to Julian Day.
         * @param year Year.
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return Julian Day as a double.
         */
        static double gregorianToJulianDay(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Converts Julian Day to Gregorian date and time.
         * @param jd Julian Day.
         * @param year Output: Year.
         * @param month Output: Month (1-12).
         * @param day Output: Day of month (1-31).
         * @param hour Output: Hour (0-23).
         * @param minute Output: Minute (0-59).
         * @param second Output: Second (0-59.999...).
         */
        static void julianDayToGregorian(double jd, int& year, int& month, int& day,
            int& hour, int& minute, double& second);

        /**
         * @brief Converts GPS week and TOW to Julian Date (GPS time scale).
         *
         * The returned JD is on the GPS time scale (not UTC). To obtain a UTC JD
         * subtract the appropriate leap second offset:
         *   JD_UTC = JD_GPS - leapSeconds(JD_GPS) / 86400.0
         *
         * @param week GPS week.
         * @param tow Time of week in seconds.
         * @return Julian Date as a double (GPS time scale).
         */
        static double gpsTimeToJulianDate(int week, double tow);

        /**
         * @brief Converts GPS time to Modified Julian Date (GPS time scale).
         *
         * MJD = JD - 2400000.5. The result is on the GPS time scale.
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Modified Julian Date (days, GPS time scale).
         */
        static double gpsTimeToMjd(const GnssTime& time);

        /**
         * @brief Converts GPS time to Modified Julian Date (UTC time scale).
         *
         * Subtracts the GPS-UTC leap second offset from the GPS MJD to
         * produce an MJD on the UTC time scale. Uses the leap second table.
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Modified Julian Date (days, UTC time scale).
         */
        static double gpsTimeToUtcMjd(const GnssTime& time);

        /**
         * @brief Converts GPS time to Julian Date on the GPS time scale.
         *
         * Convenience overload that accepts a GnssTime struct. Delegates to
         * gpsTimeToJulianDate(int week, double tow).
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Julian Date (days, GPS time scale).
         */
        static double gpsTimeToJulianDate(const GnssTime& time);

        /**
         * @brief Converts a GPS-scale Julian Date to a UTC-scale Julian Date.
         *
         * Subtracts the GPS-UTC leap second offset (looked up from the internal
         * leap second table) from the GPS JD.
         *
         * @param gps_jd Julian Date on the GPS time scale.
         * @return Julian Date on the UTC time scale.
         */
        static double gpsJdToUtcJd(double gps_jd);

        /**
         * @brief Converts a GPS-scale Julian Date to a TT-scale Julian Date.
         *
         * TT = GPS + 51.184 s (constant).
         *
         * @param gps_jd Julian Date on the GPS time scale.
         * @return Julian Date on the TT (Terrestrial Time) time scale.
         */
        static double gpsJdToTtJd(double gps_jd);

        /**
         * @brief Converts GPS time to TT Julian centuries from J2000.0.
         *
         * Computes the number of Julian centuries of Terrestrial Time (TT)
         * elapsed since J2000.0 (2000-01-01 12:00:00 TT). This is the
         * standard time argument for precession, nutation and astronomical
         * series.
         *
         * @param time GNSS epoch (GPS week + TOW).
         * @return Julian centuries of TT from J2000.0.
         */
        static double gpsTimeToTtCenturies(const GnssTime& time);

        /**
         * @brief Converts a Julian Date to Julian centuries from J2000.0.
         *
         * T = (JD ? 2451545.0) / 36525.0
         *
         * @param jd Julian Date (on any time scale � caller is responsible
         *           for ensuring the correct scale for the intended use).
         * @return Julian centuries from J2000.0.
         */
        static double jdToJ2000Centuries(double jd);

        /**
         * @brief Converts an MJD to a decimal year.
         *
         * Uses the J2000.0 MJD (51544.5) as the pivot:
         *   decimal_year = 2000.0 + (mjd ? 51544.5) / 365.25
         *
         * Useful for mean-pole and other slow-varying models.
         *
         * @param mjd Modified Julian Date (on any time scale).
         * @return Decimal year.
         */
        static double mjdToDecimalYear(double mjd);

        /**
         * @brief Converts day-of-year to month and day.
         *
         * @param year  Calendar year (used for leap-year determination).
         * @param doy   Day of year (1-based, 1�365 or 1�366 for leap years).
         * @param month Output: month (1�12).
         * @param day   Output: day of month (1�31).
         * @return True if conversion succeeded; false if doy is out of range.
         */
        static bool dayOfYearToDate(int year, int doy, int& month, int& day);

        /**
         * @brief Look up the number of leap seconds (GPS - UTC) for a given GPS JD.
         *
         * Uses a built-in table of leap second insertions. The returned value
         * is the number of seconds by which GPS time leads UTC at the given epoch.
         *
         * @param gps_jd Julian Date on the GPS time scale.
         * @return GPS-UTC offset in seconds (integer-valued as double).
         */
        static double leapSecondsGpsUtc(double gps_jd);

        /**
         * @brief Look up the number of leap seconds (TAI - UTC) for a given GPS JD.
         *
         * TAI - UTC = (GPS - UTC) + 19, since GPS = TAI - 19.
         *
         * @param gps_jd Julian Date on the GPS time scale.
         * @return TAI-UTC offset in seconds.
         */
        static double leapSecondsTaiUtc(double gps_jd);

        /**
         * @brief Validates a Gregorian date and time.
         * @param year Year.
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return True if valid, false otherwise.
         */
        static bool isValidGregorianDate(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Formats GNSS time as a human-readable string.
         *
         * Includes GPS week/TOW and UTC calendar time.
         * @param time The GNSS time to format.
         * @return Formatted string (e.g., "Week 1234, TOW 12345.67 (2023-10-01 12:34:56.789)").
         */
        static std::string formatGnssTime(const GnssTime& time);

        /**
         * @brief Convert (week, tow) to a GPS-time-scale calendar date.
         *
         * Unlike @c gpsTimeToGregorian, this does NOT subtract the
         * GPS-UTC leap-second offset, so the returned wall-clock fields
         * align exactly with the TOW grid (a 30-second RINEX file produces
         * times at HH:MM:00.000 and HH:MM:30.000, never a 42-second offset).
         * Used by log formatters that want every line to read on the same
         * grid as the data.
         */
        static void gpsTimeToGpsScaleCalendar(const GnssTime& time, int& year, int& month,
                                              int& day, int& hour, int& minute, double& second);

        /**
         * @brief Computes the offset from a time system to Terrestrial Time (TT).
         *
         * TT = TAI + 32.184 s. GPS = TAI - 19 s, so TT - GPS = 51.184 s (constant).
         * For UTC-based systems: TT - UTC = TAI_UTC_offset + 32.184 s.
         *
         * @param time_system Time system string (e.g., "GPS", "GAL", "UTC").
         * @param tai_utc_offset Current TAI-UTC leap second offset (e.g. 37.0).
         * @return Offset in seconds to add to the given time system to get TT.
         */
        static double offsetToTT(const std::string& time_system, double tai_utc_offset) {
            constexpr double TT_TAI_OFFSET = 32.184;     // TT - TAI = 32.184 s (constant)
            constexpr double GPS_TAI_OFFSET = 19.0;       // TAI - GPS = 19 s (GPS = TAI - 19)

            if (time_system == "GPS" || time_system == "GAL" || time_system.empty()) {
                // GPS and Galileo System Time are steered to within ns of each other
                // TT - GPS = (TT - TAI) + (TAI - GPS) = 32.184 + 19.0 = 51.184 s
                return TT_TAI_OFFSET + GPS_TAI_OFFSET;   // 51.184 s
            }
            if (time_system == "BDS") {
                // BDT = GPS - 14 s, so TT - BDT = 51.184 + 14.0 = 65.184 s
                return TT_TAI_OFFSET + GPS_TAI_OFFSET + 14.0;
            }
            // UTC or unknown: TT - UTC = (TT - TAI) + (TAI - UTC)
            return TT_TAI_OFFSET + tai_utc_offset;
        }

        /**
         * @brief Converts GPS Modified Julian Date (MJD) to TT MJD.
         * @param mjd_gps GPS MJD.
         * @param time_system Time system string.
         * @param tai_utc_offset Current TAI-UTC leap seconds (e.g. 37.0).
         * @return TT MJD.
         */
        static double gpsMjdToTTMjd(double mjd_gps, const std::string& time_system, double tai_utc_offset) {
            double dt = offsetToTT(time_system, tai_utc_offset);
            return mjd_gps + (dt / static_cast<double>(SECONDS_IN_DAY));
        }

        /**
         * @brief Convert a GLONASS time-of-day to full GPS time, anchored on a known epoch.
         *
         * GLONASS broadcasts time on the UTC(SU)+3h ("Moscow") day.  Several RTCM
         * fields carry only the seconds-of-day within that Moscow day with no
         * year/week: the MSM epoch @c tk (DF034) and the legacy/IGS SSR GLONASS
         * epoch (DF389).  This reconstructs the full @c GnssTime on the GPS scale
         * by framing the time-of-day in the Moscow day nearest
         * @p anchor_gps_seconds (resolving the midnight wrap to within ±12 h),
         * then carrying it back through UTC to GPS time (applying the
         * epoch-dependent GPS-UTC leap-second offset).  Mirrors RTKLIB's
         * @c adjday_glot.
         *
         * The leap-second offset is looked up once at the anchor epoch and reused
         * for the forward (GPS→UTC) and return (UTC→GPS) legs, so it cancels
         * exactly except across a leap-second insertion boundary, where the
         * single-epoch error self-corrects on the next epoch.
         *
         * @param tod_glonass_s      Seconds since the start of the GLONASS (Moscow)
         *                           day, i.e. RTCM @c tk (MSM) or the SSR GLONASS
         *                           epoch field, nominally in [0, 86400).
         * @param anchor_gps_seconds Continuous GPS seconds (week·604800 + tow) of a
         *                           known nearby epoch — the rover's current time —
         *                           used to resolve which GLONASS day the
         *                           time-of-day falls in.
         * @return GnssTime{week, tow} on the GPS time scale.  Returns {0, 0} if
         *         @p anchor_gps_seconds is non-positive (anchor unavailable).
         */
        static GnssTime glonassTodToGpsTime(double tod_glonass_s, double anchor_gps_seconds);

        /**
         * @brief Convert a GNSS time to continuous GPS seconds for sorting / interpolation.
         *
         * Builds a monotonic scalar time value by converting the GPS week + time-of-week
         * representation into seconds since the GPS epoch:
         *   continuous_seconds = week * 604800 + tow
         *
         * @param t GNSS time containing `week` and `tow` (seconds into the week).
         * @return Continuous GPS seconds as a double.
         */
        static double continuousGpsSeconds(const GnssTime& t);

        /**
         * @brief Current system wall-clock time as continuous GPS seconds.
         *
         * Reads the host clock (UTC/POSIX) and converts to continuous GPS
         * seconds (week·604800 + tow), adding @ref currentLeapSeconds for the
         * UTC→GPS offset (the compiled @c LEAP_SECONDS unless overridden via
         * @ref setLeapSecondsOverride).  Used by the real-time engine to
         * measure how far an observation epoch lags real time (obs-backlog /
         * skip-to-latest), and as the COLD-START anchor of the RTCM parser's
         * stream-time tracker (RTB-1(a)).
         *
         * @return Continuous GPS seconds for "now" (≤ 0 if before the GPS epoch).
         */
        static double nowContinuousGpsSeconds();

        /**
         * @brief Override the GPS-UTC leap-second count used by the wall-clock
         *        → GPS conversion (RTB-1(c); config key @c realtime.leap_seconds).
         *
         * Affects @ref nowContinuousGpsSeconds (and therefore everything built
         * on it: the RT decode paths' cold-start week/day promotion,
         * @ref nowGpsTime product stamps, RT lag measurement).  The
         * epoch-dependent TABLE lookups (@ref leapSecondsGpsUtc — used for
         * historical epochs, GLONASS day arithmetic at the anchor epoch, UTC
         * formatting) are deliberately NOT overridden: they are already
         * correct per epoch; this override exists so a future leap-second
         * insertion doesn't strand the compiled constant on live streams
         * before a rebuild.  Set once at startup (RealTimeEngine::initialize)
         * before the receive threads spin up; the value is stored atomically
         * so a late set is safe, just not atomic with in-flight conversions.
         *
         * Deriving the leap count from the broadcast navigation message
         * (GPS LNAV page-18 ΔtLS and equivalents) is the principled follow-on
         * — RTF-scale, since it needs the nav decoders to surface the field
         * and a policy for pre-first-nav frames.
         *
         * @param leap_s GPS-UTC offset in seconds (e.g. 18.0).
         */
        static void setLeapSecondsOverride(double leap_s);

        /**
         * @brief The GPS-UTC leap-second count currently in effect for
         *        wall-clock conversions.
         *
         * The compiled @c LEAP_SECONDS (18) unless @ref setLeapSecondsOverride
         * replaced it (config @c realtime.leap_seconds).
         */
        static double currentLeapSeconds();

        /**
         * @brief Current system wall-clock time as a @c GnssTime (GPS week + TOW).
         *
         * Wraps @ref nowContinuousGpsSeconds and splits it into (week, tow).  Used
         * to stamp a product's TIME-OF-GENERATION so a downstream consumer can
         * measure end-to-end latency (received − generated) — RT0-4.  This is the
         * wall clock, NOT a content-reference epoch (those come from the observation
         * stream); it is non-deterministic by nature, so never use it on a path that
         * must be byte-reproducible.
         *
         * @return Wall-clock "now" as a GnssTime (week 0, tow 0 semantics unused).
         */
        static GnssTime nowGpsTime();

        // -----------------------------------------------------------------------
        // Time system constants
        // -----------------------------------------------------------------------

        static constexpr double GPS_EPOCH_JD = 2444244.5;     ///< Julian Day of GPS epoch (1980-01-06 00:00:00 UTC).
        static constexpr double MJD_OFFSET = 2400000.5;       ///< JD - MJD offset.
        static constexpr double GPS_EPOCH_MJD = 44244.0;      ///< MJD of GPS epoch (GPS_EPOCH_JD - MJD_OFFSET).
        static constexpr double J2000_JD = 2451545.0;         ///< Julian Day of J2000.0 epoch (2000-01-01 12:00:00 TT).
        static constexpr double J2000_MJD = 51544.5;          ///< MJD of J2000.0 epoch.
        static constexpr double TT_TAI_OFFSET = 32.184;       ///< TT - TAI offset in seconds (constant).
        static constexpr double TAI_GPS_OFFSET = 19.0;        ///< TAI - GPS offset in seconds (TAI = GPS + 19).
        static constexpr double TT_GPS_OFFSET = 51.184;       ///< TT - GPS offset in seconds (constant, 32.184 + 19).

        static constexpr int64_t SECONDS_IN_WEEK = 604800;    ///< Seconds in a GPS week.
        static constexpr int64_t SECONDS_IN_DAY = 86400;      ///< Seconds in a day.
        static constexpr double DAYS_PER_JULIAN_CENTURY = 36525.0; ///< Days in a Julian century.

        /// @brief Current-era GPS-UTC leap second count. Use leapSecondsGpsUtc() for
        ///        epoch-dependent lookups. This value (18) is valid from 2017-01-01 onwards.
        static constexpr double LEAP_SECONDS = 18.0;

    private:

        /**
         * @brief Gets the number of days in a given month.
         * @param year Year.
         * @param month Month (1-12).
         * @return Number of days in the month.
         */
        static int daysInMonth(int year, int month);

        /**
         * @brief Checks if a year is a leap year.
         * @param year Year.
         * @return True if leap year, false otherwise.
         */
        static bool isLeapYear(int year);
    };

} // namespace GnssProcessor