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
         * @brief Converts GPS week and TOW to Julian Date.
         * @param week GPS week.
         * @param tow Time of week in seconds.
         * @return Julian Date as a double.
         */
        static double gpsTimeToJulianDate(int week, double tow);

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
         * @brief Computes the offset from a time system to Terrestrial Time (TT).
         * @param time_system Time system string (e.g., "GPS").
         * @param leap_seconds Current leap seconds.
         * @return Offset in seconds to TT.
         */
        static double offsetToTT(const std::string& time_system, double leap_seconds) {
            constexpr double TT_TAI_OFFSET = 32.184;
            constexpr double GPS_TAI_OFFSET = -19.0;  // GPS lags TAI by 19s
            if (time_system == "GPS" || time_system.empty()) {
                return TT_TAI_OFFSET - GPS_TAI_OFFSET;  // 51.184s
            }
            // Assume UTC for others (e.g., GAL GST is approximately equal to UTC, BDS BDT = UTC +14s but adjust if needed)
            return TT_TAI_OFFSET + leap_seconds;  // e.g., 32.184 + 37 = 69.184s
        }

        /**
         * @brief Converts GPS Modified Julian Date (MJD) to TT MJD.
         * @param mjd_gps GPS MJD.
         * @param time_system Time system string.
         * @param leap_seconds Current leap seconds.
         * @return TT MJD.
         */
        static double gpsMjdToTTMjd(double mjd_gps, const std::string& time_system, double leap_seconds) {
            double dt = offsetToTT(time_system, leap_seconds);
            //double seconds_in_day = mjd_gps * 86400.0 + dt;  // Add offset to seconds-of-day
            return mjd_gps + (dt / TimeUtils::SECONDS_IN_DAY);     // Convert back to fractional MJD
        }

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

        static constexpr double GPS_EPOCH_JD = 2444244.5; ///< Julian Day for GPS epoch (January 6, 1980).
        static constexpr int64_t SECONDS_IN_WEEK = 604800; ///< Seconds in a GPS week.
        static constexpr int64_t SECONDS_IN_DAY = 86400; ///< Seconds in a day.

        // GPS time = UTC + leap_seconds
        // As of 2017-01-01, there are 18 leap seconds between UTC and GPS
        // (This should ideally be looked up based on the date, not hardcoded)
        static constexpr double LEAP_SECONDS = 18.0; ///< Valid until updated by IERS Bulletin C

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

//#endif // TIMEUTILS_HPP