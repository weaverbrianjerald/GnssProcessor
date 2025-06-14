#pragma once

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include "ProductManager.hpp"
#include "ProductParser.hpp"
#include "StateManager.hpp"
#include "Types.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <vector>
#include <deque>

namespace GnssProcessor {

    class PppModel {
    public:
        PppModel(const GnssConfig& config, ProductManager& product_manager, LogManager& log_manager,
            const RinexParser& rinex_parser, StateManager& state_manager);
        bool computeModels(const std::vector<RinexObservation>& observations,
            PppState& state,
            std::vector<PppObservation>& modeled_obs);
        double getWavelength(const std::string& type, System system) const;
        double computeIonosphericDelay(const GnssTime& time,
            const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos,
            const std::string& freq_type,
            System system,
            const std::map<std::string, double>& measurements) const;
        Eigen::Vector3d getSatellitePosition(const std::string& satellite,
            const GnssTime& time) const;
        double computeElevation(const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;
        double computeGeometricRange(const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;
        double getAntennaCorrection(const std::string& satellite,
            const std::string& freq_type,
            System system,
            const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos,
            double elevation,
            double azimuth,
            const std::string& rx_antenna_type,
            const GnssTime& time) const;
        double getBiasCorrection(const std::string& satellite,
            const std::string& freq_type,
            const GnssTime& time) const;

    private:
        void loadProducts();
        double computeTroposphericDelay(double elevation, double zenith_delay) const;
        double computeKlobucharDelay(const GnssTime& time,
            const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos,
            double frequency) const;
        double computeIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system,
            const std::string& freq_type) const;
        double computeTripleIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system,
            const std::string& freq_type) const;
        double getSatelliteClockCorrection(const std::string& satellite,
            const GnssTime& time) const;
        Eigen::Vector3d interpolateOrbit(const std::string& satellite,
            const GnssTime& time,
            const std::vector<OrbitData>& orbit_data) const;
        double interpolateClock(const std::string& satellite,
            const GnssTime& time,
            const std::vector<ClockData>& clock_data) const;
        double computeRelativisticCorrection(const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel) const;
        double computeShapiroDelay(const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;
        double interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
            double zenith,
            double azimuth,
            double dazi,
            double dzen) const;
        double computePhaseWindUp(const std::string& satellite,
            System system,
            const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos,
            double wavelength,
            const GnssTime& time) const;
        void computeSatelliteAttitude(System system,
            const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel,
            Eigen::Vector3d& d_s,
            Eigen::Vector3d& y_s,
            const GnssTime& time) const;
        Eigen::Vector3d computePoleTideCorrection(const Eigen::Vector3d& rx_pos,
            const GnssTime& time) const;
        Eigen::Vector3d computeOceanTideCorrection(const Eigen::Vector3d& rx_pos,
            const GnssTime& time) const;
        double getFrequency(const std::string& freq_type, System system) const;
        std::vector<std::string> selectObservationTypes(const std::map<std::string, double>& measurements,
            System system,
            const std::string& system_str) const;
        double computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;
        double computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
            const GnssTime& time) const;
        Eigen::Vector3d interpolateSunPosition(const GnssTime& time) const;
        double computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const;

        struct MultipathHistory {
            std::deque<double> cmc_values;
            std::deque<GnssTime> timestamps;
            static constexpr size_t WINDOW_SIZE = 5;
            static constexpr double TIME_GAP_THRESHOLD = 30.0;
        };
        double computeMultipathVarianceWeight(const PppObservation& obs, const std::string& type, double elevation) const;
        static constexpr double MULTIPATH_CMC_THRESHOLD = 5.0;
        static constexpr double ELEVATION_MIN_WEIGHT = 0.1;

        const GnssConfig& config_;
        ProductManager& product_manager_;
        LogManager& log_manager_;
        ProductParser parser_;
        const RinexParser& rinex_parser_;
        StateManager& state_manager_;
        std::map<std::string, std::vector<std::string>> signal_priorities_;
        double elevation_threshold_;
        std::vector<OrbitData> orbits_;
        std::vector<ClockData> clocks_;
        std::vector<AntennaData> antennas_;
        std::vector<BiasData> biases_;
        std::vector<ErpData> erps_;
        std::vector<OceanTideData> tides_;
        std::vector<double> klobuchar_coeffs_;
        std::vector<SunEphemeris> sun_ephemeris_;
        mutable std::map<std::string, MultipathHistory> multipath_histories_;
    };

} // namespace GnssProcessor

//#endif // PPPMODEL_HPP