#pragma once

#include <cstdint>
#include <string>

namespace GnssProcessor {

    struct GnssTime {
        int week{ 0 };
        double tow{ 0.0 };

        // Inline operator definitions
        inline bool operator==(const GnssTime& other) const {
            return week == other.week && tow == other.tow;
        }

        inline bool operator<(const GnssTime& other) const {
            return (week < other.week) || (week == other.week && tow < other.tow);
        }
    };

    class TimeUtils {
    public:
        static GnssTime gregorianToGpsTime(int year, int month, int day, int hour, int minute, double second);
        static GnssTime GnssTimeFromDate(int year, int month, int day, int hour, int minute, double second, const std::string& time_system);
        static void gpsTimeToGregorian(const GnssTime& gpsTime, int& year, int& month, int& day,
            int& hour, int& minute, double& second);
        static double gregorianToJulianDay(int year, int month, int day, int hour, int minute, double second);
        static void julianDayToGregorian(double jd, int& year, int& month, int& day,
            int& hour, int& minute, double& second);
        static double gpsTimeToJulianDate(int week, double tow);
        static bool isValidGregorianDate(int year, int month, int day, int hour, int minute, double second);

        // Human-readable epoch string (GPS week/TOW plus UTC calendar time)
        static std::string formatGnssTime(const GnssTime& time);

        static double offsetToTT(const std::string& time_system, double leap_seconds) {
            constexpr double TT_TAI_OFFSET = 32.184;
            constexpr double GPS_TAI_OFFSET = -19.0;  // GPS lags TAI by 19s
            if (time_system == "GPS" || time_system.empty()) {
                return TT_TAI_OFFSET - GPS_TAI_OFFSET;  // 51.184s
            }
            // Assume UTC for others (e.g., GAL GST is approximately equal to UTC, BDS BDT = UTC +14s but adjust if needed)
            return TT_TAI_OFFSET + leap_seconds;  // e.g., 32.184 + 37 = 69.184s
        }

        static double gpsMjdToTTMjd(double mjd_gps, const std::string& time_system, double leap_seconds) {
            double dt = offsetToTT(time_system, leap_seconds);
            //double seconds_in_day = mjd_gps * 86400.0 + dt;  // Add offset to seconds-of-day
            return mjd_gps + (dt / 86400.0);     // Convert back to fractional MJD
        }

    private:
        static constexpr double GPS_EPOCH_JD = 2444244.5; // Julian Day for GPS epoch
        static constexpr int64_t SECONDS_IN_WEEK = 604800;
        static constexpr int64_t SECONDS_IN_DAY = 86400;

        static int daysInMonth(int year, int month);
        static bool isLeapYear(int year);
    };

} // namespace GnssProcessor

//#endif // TIMEUTILS_HPP