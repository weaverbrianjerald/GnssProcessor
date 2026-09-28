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
         * @brief Converts a UTC Gregorian date and time to GPS time.
         *
         * Adds the epoch-dependent GPS-UTC leap-second offset from the internal table.
         *
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return GnssTime struct with GPS week and TOW.
         * @throws std::invalid_argument if the date fails isValidGregorianDate().
         */
        static GnssTime gregorianToGpsTime(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Converts a Gregorian date and time already on the GPS time scale
         *        to GPS week and TOW, without any leap-second correction.
         *
         * For timestamps already expressed in GPS time (e.g. SP3 / CLK product
         * epochs).  UTC timestamps go through gregorianToGpsTime(), which adds the
         * leap-second offset.
         *
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @return GnssTime struct with GPS week and TOW.
         * @throws std::invalid_argument if the date fails isValidGregorianDate().
         */
        static GnssTime gregorianGpsToGpsTime(int year, int month, int day, int hour, int minute, double second);

        /**
         * @brief Converts a date and time on a named time system to GPS time.
         *
         * "GPS", "GAL" and an empty string are taken as the GPS scale (no leap
         * seconds); "BDS" adds 14 s (BDT = GPS - 14 s); "GLO" is taken as UTC and
         * gets the leap-second offset.  Any other string is treated as the GPS scale
         * and logged at trace level.  An invalid date logs a warning and returns
         * {0, 0}.
         *
         * @param year Year (e.g., 2023).
         * @param month Month (1-12).
         * @param day Day of month (1-31).
         * @param hour Hour (0-23).
         * @param minute Minute (0-59).
         * @param second Second (0-59.999...).
         * @param time_system Time system string (e.g., "GPS", "GAL", "BDS", "GLO").
         * @return GnssTime struct on the GPS time scale.
         */
        static GnssTime GnssTimeFromDate(int year, int month, int day, int hour, int minute, double second, const std::string& time_system);

        /**
         * @brief Converts GPS time to a UTC Gregorian date and time.
         *
         * Subtracts the epoch-dependent GPS-UTC leap-second offset.  For calendar
         * fields on the GPS scale, see gpsTimeToGpsScaleCalendar().
         *
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
         * @throws std::invalid_argument if week < 0 or tow is outside [0, 604800).
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
         * T = (JD − 2451545.0) / 36525.0
         *
         * @param jd Julian Date on any time scale (the caller chooses the scale
         *           the intended use requires).
         * @return Julian centuries from J2000.0.
         */
        static double jdToJ2000Centuries(double jd);

        /**
         * @brief Converts an MJD to a decimal year.
         *
         * Uses the J2000.0 MJD (51544.5) as the pivot:
         *   decimal_year = 2000.0 + (mjd − 51544.5) / 365.25
         *
         * Intended for mean-pole and other slowly varying models.
         *
         * @param mjd Modified Julian Date (on any time scale).
         * @return Decimal year.
         */
        static double mjdToDecimalYear(double mjd);

        /**
         * @brief Converts day-of-year to month and day.
         *
         * @param year  Calendar year (used for leap-year determination).
         * @param doy   Day of year (1-based, 1–365, or 1–366 in leap years).
         * @param month Output: month (1–12).
         * @param day   Output: day of month (1–31).
         * @return True if conversion succeeded; false if doy is out of range.
         */
        static bool dayOfYearToDate(int year, int doy, int& month, int& day);

        /**
         * @brief Looks up the number of leap seconds (GPS - UTC) for a given GPS JD.
         *
         * Uses a built-in table of leap-second insertions.  The returned value is
         * the number of seconds by which GPS time leads UTC at the given epoch
         * (0 before the first table entry).
         *
         * @param gps_jd Julian Date on the GPS time scale.
         * @return GPS-UTC offset in seconds (integer-valued as double).
         */
        static double leapSecondsGpsUtc(double gps_jd);

        /**
         * @brief Looks up the number of leap seconds (TAI - UTC) for a given GPS JD.
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
         * Includes GPS week/TOW and the UTC calendar time (from gpsTimeToGregorian).
         * @param time The GNSS time to format.
         * @return Formatted string (e.g., "GPS week 1234, TOW 12345.670 (2023-10-01 12:34:56.789 UTC)").
         */
        static std::string formatGnssTime(const GnssTime& time);

        /**
         * @brief Converts (week, tow) to a calendar date on the GPS time scale.
         *
         * Unlike @c gpsTimeToGregorian, this does not subtract the GPS-UTC
         * leap-second offset, so the returned wall-clock fields sit on the TOW
         * grid: a 30-second RINEX file gives HH:MM:00.000 and HH:MM:30.000, not
         * times shifted by the leap-second count (HH:MM:42 and HH:MM:12 at 18 s).
         * Used by log formatters so that every line reads on the same grid as the
         * data.
         */
        static void gpsTimeToGpsScaleCalendar(const GnssTime& time, int& year, int& month,
                                              int& day, int& hour, int& minute, double& second);

        /**
         * @brief Computes the offset from a time system to Terrestrial Time (TT).
         *
         * TT = TAI + 32.184 s and GPS = TAI - 19 s, so TT - GPS = 51.184 s
         * (constant).  "GPS", "GAL" and an empty string use 51.184 s; "BDS" uses
         * 65.184 s (BDT = GPS - 14 s).  Any other string, including "UTC" and
         * "GLO", is treated as UTC: TT - UTC = tai_utc_offset + 32.184 s.
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
         * @brief Converts an MJD on the scale named by @p time_system to TT MJD.
         *
         * Adds offsetToTT(time_system, tai_utc_offset) / 86400 days.
         *
         * @param mjd_gps MJD on the @p time_system scale (the GPS scale for "GPS").
         * @param time_system Time system string.
         * @param tai_utc_offset Current TAI-UTC leap seconds (e.g. 37.0).
         * @return TT MJD.
         */
        static double gpsMjdToTTMjd(double mjd_gps, const std::string& time_system, double tai_utc_offset) {
            double dt = offsetToTT(time_system, tai_utc_offset);
            return mjd_gps + (dt / static_cast<double>(SECONDS_IN_DAY));
        }

        /**
         * @brief Converts a GLONASS time-of-day to full GPS time, anchored on a known epoch.
         *
         * GLONASS broadcasts time on the UTC(SU)+3h ("Moscow") day.  Several RTCM
         * fields carry only the seconds-of-day within that Moscow day, with no
         * year or week: the MSM epoch @c tk (DF034) and the legacy/IGS SSR GLONASS
         * epoch (DF389).  This function places the time-of-day in the Moscow day
         * nearest @p anchor_gps_seconds (resolving the midnight wrap to within
         * ±12 h), then converts it through UTC to GPS time with the
         * epoch-dependent GPS-UTC leap-second offset.  Mirrors RTKLIB's
         * @c adjday_glot.
         *
         * The leap-second offset is looked up once, at the anchor epoch, and used
         * for both the forward (GPS→UTC) and return (UTC→GPS) legs.  It is
         * therefore exact except when the anchor and the target epoch lie on
         * opposite sides of a leap-second insertion; that error lasts one epoch,
         * because the next call uses a later anchor.
         *
         * @param tod_glonass_s      Seconds since the start of the GLONASS (Moscow)
         *                           day, i.e. RTCM @c tk (MSM) or the SSR GLONASS
         *                           epoch field, nominally in [0, 86400).
         * @param anchor_gps_seconds Continuous GPS seconds (week·604800 + tow) of a
         *                           known nearby epoch (the rover's current time),
         *                           used to resolve which GLONASS day the
         *                           time-of-day falls in.
         * @return GnssTime{week, tow} on the GPS time scale.  Returns {0, 0} if
         *         @p anchor_gps_seconds is non-positive (anchor unavailable).
         */
        static GnssTime glonassTodToGpsTime(double tod_glonass_s, double anchor_gps_seconds);

        /**
         * @brief Converts a GNSS time to continuous GPS seconds for sorting / interpolation.
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
         * Reads the host clock (UTC/POSIX, whole seconds) and converts it to
         * continuous GPS seconds (week·604800 + tow), adding @ref currentLeapSeconds
         * for the UTC→GPS offset (the compiled @c LEAP_SECONDS unless overridden
         * via @ref setLeapSecondsOverride).  The real-time engine uses it to
         * measure how far an observation epoch lags real time (obs backlog,
         * skip-to-latest), and the RTCM parser's stream-time tracker uses it as
         * its cold-start anchor (RTB-1(a)).
         *
         * @return Continuous GPS seconds for "now" (≤ 0 if before the GPS epoch).
         */
        static double nowContinuousGpsSeconds();

        /**
         * @brief Overrides the GPS-UTC leap-second count used by the wall-clock
         *        → GPS conversion (RTB-1(c); config key @c realtime.leap_seconds).
         *
         * Affects @ref nowContinuousGpsSeconds and everything built on it: the RT
         * decode paths' cold-start week/day promotion, @ref nowGpsTime product
         * stamps, and RT lag measurement.  The epoch-dependent table lookups
         * (@ref leapSecondsGpsUtc, used for historical epochs, GLONASS day
         * arithmetic at the anchor epoch and UTC formatting) are not overridden:
         * they are already correct per epoch.  The override lets a future
         * leap-second insertion be applied to live streams without a rebuild.
         * Set once at startup (RealTimeEngine::initialize, via
         * armSsrStoreOptions) before the receive threads start.  The value is
         * stored atomically, so a late set is safe, though not atomic with
         * conversions already in flight.
         *
         * Deriving the leap count from the broadcast navigation message
         * (GPS LNAV page-18 ΔtLS and equivalents) is the planned follow-on
         * (RTF scale): it needs the nav decoders to surface the field and a
         * policy for frames that arrive before the first navigation message.
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
         * to stamp a product's time of generation so that a downstream consumer can
         * measure end-to-end latency (received − generated) (RT0-4).  This is the
         * wall clock, not a content-reference epoch (those come from the
         * observation stream).  It is non-deterministic, so it does not belong on
         * any path that must be byte-reproducible.
         *
         * @return Wall-clock "now" as a GnssTime (the {0, 0} "unavailable"
         *         convention is not used).
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

        /// @brief GPS-UTC leap-second count of the current era, valid from 2017-01-01.
        ///        Epoch-dependent lookups use leapSecondsGpsUtc().
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