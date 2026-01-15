#ifndef GNSS_CORRECTIONS_HPP
#define GNSS_CORRECTIONS_HPP

#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include "PppTypes.hpp"
#include <vector>
#include <Eigen/Dense>
#include <string>

namespace GnssProcessor {

    // Forward declarations to avoid header cycles
    class GnssConfig;
    class RinexParser;

    class GnssCorrections {

    public:
        // Minimal constructor (no product data) - used where only logging is available
        explicit GnssCorrections(LogManager& log_mgr);

        // Constructor with clocks and ERPs (used by Geometry/ProductParser)
        GnssCorrections(LogManager& log_mgr,
            const std::vector<ClockData>& clocks,
            const std::vector<ErpData>& erps);

        // Full constructor (optional biases and access to config/rinex)
        GnssCorrections(LogManager& log_mgr,
            const GnssConfig& config,
            const std::vector<ClockData>& clocks,
            const std::vector<ErpData>& erps,
            const std::vector<BiasData>& biases,
            const RinexParser& rinex);

        // Update clocks/erps after construction (avoids reassigning whole object)
        void setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps);

        // Computes satellite clock correction (seconds) using CLK data
        double getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const;

        // Interpolates clock correction from CLK data
        double interpolateClock(const std::string& satellite, const GnssTime& time,
            const std::vector<ClockData>& clocks) const;

        // Computes relativistic clock correction (seconds) for a satellite
        double computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel) const;

        // Computes Shapiro (relativistic path) delay (meters) for a satellite
        double computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;

        // Computes Sagnac correction (meters)
        double computeSagnacCorrection(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        // Computes pole tide correction (meters) in ECEF coordinates
        Eigen::Vector3d computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        // Computes Solid Earth Tide correction
        double computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos) const;

        // Computes ocean tide correction (meters) in ECEF coordinates
        Eigen::Vector3d computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        // Computes atmospheric loading correction (meters)
        double computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        // Returns bias correction (meters) for a signal type at a given time
        double getBiasCorrection(const std::string& satellite, const std::string& freq_type,
            const GnssTime& time) const;

        // Computes phase wind-up correction (meters) for carrier phase measurements
        double computePhaseWindUp(const std::string& satellite, System system, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double wavelength, const GnssTime& time) const;

    private:
        LogManager& log_manager_;                         // Logger for debugging and info
        std::vector<ClockData> clocks_;                   // Satellite clock data
        std::vector<ErpData> erps_;                       // Earth rotation parameters
        std::vector<BiasData> biases_;                    // Bias correction data
        std::vector<OrbitData> orbits_;                   // Satellite orbit data

        // Optional external references (nullable pointers)
        const GnssConfig* config_;                        // Configuration settings (may be nullptr)
        const RinexParser* rinex_parser_;                 // RINEX data parser (may be nullptr)
    };

}
#endif