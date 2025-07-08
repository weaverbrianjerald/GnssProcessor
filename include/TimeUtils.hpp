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
        static bool isValidGregorianDate(int year, int month, int day, int hour, int minute, double second);

    private:
        static constexpr double GPS_EPOCH_JD = 2444244.5; // Julian Day for GPS epoch
        static constexpr int64_t SECONDS_IN_WEEK = 604800;
        static constexpr int64_t SECONDS_IN_DAY = 86400;

        static int daysInMonth(int year, int month);
        static bool isLeapYear(int year);
    };

} // namespace GnssProcessor

//#endif // TIMEUTILS_HPP