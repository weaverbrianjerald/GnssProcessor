#pragma once
#ifndef PPPMODEL_HPP
#define PPPMODEL_HPP

#include "GnssConfig.hpp"
#include "ProductParser.hpp"
#include "LogManager.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppTypes.hpp"
#include <Eigen/Dense>
#include <vector>
#include <map>
#include <string>
#include <deque>

namespace GnssProcessor {

    struct MultipathHistory {
        static constexpr size_t WINDOW_SIZE = 10;          // Number of epochs for multipath history
        static constexpr double TIME_GAP_THRESHOLD = 300.0; // Max time gap for continuity (seconds)
        static constexpr double MULTIPATH_CMC_THRESHOLD = 3.0; // Threshold for multipath detection (std devs)
        static constexpr double ELEVATION_MIN_WEIGHT = 0.1; // Minimum elevation weight
        std::deque<double> cmc_values;                    // Code-minus-carrier values
        std::deque<GnssTime> timestamps;                  // Observation timestamps
    };

    class PppModel {
    public:
        // Constructor: Initializes PppModel with configuration, product parser, logger, RINEX parser, and state manager
        PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
            const RinexParser& rinex_parser, StateManager& state_manager);

        // Initializes the PPP state (position, clock, troposphere, ionosphere, covariance)
        void initializeState(PppState& state);

        // Computes observation models for a set of RINEX observations
        bool computeModels(const std::vector<RinexObservation>& observations,
            PppState& state,
            std::vector<PppObservation>& modeled_obs);

        // Interpolates satellite position at a given time using SP3 data
        Eigen::Vector3d getSatellitePosition(const std::string& satellite, const GnssTime& time) const;

        // Computes elevation angle (degrees) from receiver to satellite
        double computeElevation(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        // Computes azimuth angle (degrees) from receiver to satellite
        double computeAzimuth(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        // Computes geometric range (meters) from receiver to satellite
        double computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        // Computes phase wind-up correction (meters) for carrier phase measurements
        double computePhaseWindUp(const std::string& satellite, System system, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double wavelength, const GnssTime& time) const;

        // Computes ionospheric delay (meters) using Klobuchar model or measurements
        double computeIonosphericDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, const std::string& freq_type,
            System system, const std::map<std::string, double>& measurements) const;

        // Returns signal frequency (Hz) for a given signal type and system
        double getFrequency(const std::string& freq_type, System system, const std::string& satellite) const;

        // Returns signal wavelength (meters) for a given signal type and system
        double getWavelength(System system, const std::string& freq_type) const;

        // Returns reference frequency (Hz) for GLONASS signals
        double getReferenceFrequency(const std::string& freq_type, System system) const;

        // Rescales GLONASS observations based on channel-specific frequency
        double rescaleGlonassObservation(double observed_data, const std::string& freq_type,
            System system, const std::string& satellite) const;

        // Computes tropospheric delay (meters) using a mapping function
        double getTroposphericDelay(double elevation, double zenith_delay) const;

        // Computes antenna corrections (PCO, PCV, phase wind-up) in meters
        double getAntennaCorrection(const std::string& satellite, const std::string& freq_type, System system,
            const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos,
            double elevation, double azimuth, const std::string& rx_antenna_type,
            const GnssTime& time) const;

        // Returns bias correction (meters) for a signal type at a given time
        double getBiasCorrection(const std::string& satellite, const std::string& freq_type,
            const GnssTime& time) const;

        // Returns receiver antenna type from RINEX header
        std::string getReceiverAntennaType() const;

        // Provides access to the RinexParser instance
        const RinexParser& getRinexParser() const;

        // Computes satellite geometry and corrections (position, elevation, azimuth, delays)
        bool computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos, Eigen::Vector3d& rx_pos, double& elevation,
            double& azimuth, Eigen::Vector3d& los, double& range, double& tropo_delay,
            double& sat_clock, double& shapiro_delay, double& sagnac_corr,
            double& atm_loading_corr, double& relativistic_corr) const;

        // Computes satellite clock correction (seconds) using CLK data
        double getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const;

        // Computes relativistic clock correction (seconds) for a satellite
        double computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel) const;

        // Computes Shapiro (relativistic path) delay (meters) for a satellite
        double computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;

    private:
        const GnssConfig& config_;                         // Configuration settings
        ProductParser& product_parser_;                   // Product data parser
        LogManager& log_manager_;                         // Logger for debugging and info
        const RinexParser& rinex_parser_;                 // RINEX data parser
        StateManager& state_manager_;                     // State manager for ambiguities
        mutable std::map<std::string, MultipathHistory> multipath_histories_; // Multipath history per satellite
        const double elevation_threshold_;                // Elevation mask (degrees)
        std::vector<OrbitData> orbits_;                   // Satellite orbit data
        std::vector<ClockData> clocks_;                   // Satellite clock data
        std::vector<AntennaData> antennas_;               // Antenna correction data
        std::vector<BiasData> biases_;                    // Bias correction data
        std::vector<ErpData> erps_;                       // Earth rotation parameters
        std::vector<double> klobuchar_coeffs_;            // Klobuchar ionospheric model coefficients
        std::vector<SunEphemeris> sun_ephemeris_;         // Sun ephemeris data
        mutable std::map<std::string, Eigen::Vector3d> last_interpolated_velocity_; // Cached satellite velocities
        const std::map<std::string, std::vector<std::string>> signal_priorities_; // Signal priorities per system

        // Loads SP3, CLK, ATX, BIA, ERP, and ionospheric data
        void loadProducts();

        // Computes Klobuchar ionospheric delay (meters)
        double computeKlobucharDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double frequency) const;

        // Computes ionosphere-free combination for two signals
        double computeIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system, const std::string& signal1, const std::string& signal2) const;

        // Computes triple-frequency ionosphere-free combination
        double computeTripleIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system, const std::string& signal1,
            const std::string& signal2, const std::string& signal3) const;

        // Selects signals for processing based on priorities
        bool selectSignals(const RinexObservation& obs, const std::string& system_str,
            std::vector<std::string>& selected_signals, bool& is_iono_free,
            std::string& comb_type);

        // Selects observation types based on system and priorities
        std::vector<std::string> selectObservationTypes(const std::map<std::string, double>& measurements,
            System system, const std::string& system_str) const;

        // Processes single-frequency observation
        bool processSingleFreqObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::string& type, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
            const std::string& rx_antenna_type, double range, double tropo_delay,
            double sat_clock, double shapiro_delay, double sagnac_corr,
            double atm_loading_corr, double relativistic_corr);

        // Processes ionosphere-free observation
        bool processIonoFreeObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::string>& selected_signals,
            const std::string& comb_type, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
            const std::string& rx_antenna_type, double range, double tropo_delay,
            double sat_clock, double shapiro_delay, double sagnac_corr,
            double atm_loading_corr, double relativistic_corr);

        // Computes Sagnac correction (meters)
        double computeSagnacCorrection(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        // Computes atmospheric loading correction (meters)
        double computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        // Computes satellite antenna attitude
        void computeSatelliteAttitude(System system, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel, Eigen::Vector3d& d_s,
            Eigen::Vector3d& y_s, const GnssTime& time) const;

        Eigen::Vector3d getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const;

        // Interpolates Sun position from ephemeris
        Eigen::Vector3d interpolateSunPosition(const GnssTime& time) const;

        // Computes initial ionospheric delay (meters) for PPP mode
        double computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const;

        // Computes multipath variance weight
        double computeMultipathVarianceWeight(const PppObservation& obs, const std::string& type,
            double elevation) const;

        // Interpolates PCV table for antenna corrections
        double interpolatePcv(const std::vector<std::vector<double>>& pcv_table, double zenith,
            double azimuth, double dazi, double dzen) const;

        // Interpolates clock correction from CLK data
        double interpolateClock(const std::string& satellite, const GnssTime& time,
            const std::vector<ClockData>& clocks) const;

        // Computes pole tide correction (meters) in ECEF coordinates
        Eigen::Vector3d computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        // Computes ocean tide correction (meters) in ECEF coordinates
        Eigen::Vector3d computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;
    };

} // namespace GnssProcessor

#endif // PPPMODEL_HPP