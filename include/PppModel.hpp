#pragma once
#ifndef PPPMODEL_HPP
#define PPPMODEL_HPP

#include "GnssConfig.hpp"
#include "ProductParser.hpp"
#include "LogManager.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppTypes.hpp"
#include "Planetary.hpp"
#include "Ionosphere.hpp"
#include "Antenna.hpp"
#include <Eigen/Dense>
#include <vector>
#include <map>
#include <string>
#include <deque>

namespace GnssProcessor {

    class PppModel {
    public:
        // Constructor: Initializes PppModel with configuration, product parser, logger, RINEX parser, and state manager
        PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
            const RinexParser& rinex_parser, StateManager& state_manager);

        // Initializes the PPP state (position, clock, troposphere, ionosphere, covariance)
        void initializeState(PppState& state);

        bool computeModels(const std::vector<RinexObservation>& observations, PppState& state, std::vector<PppObservation>& modeled_obs);

        double computeIonosphericDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, const std::string& freq_type,
            System system, const std::map<std::string, double>& measurements) const;

        double rescaleGlonassObservation(double observed_data, const std::string& freq_type,
            System system, const std::string& satellite) const;

        const RinexParser& getRinexParser() const;

        double computeIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system, const std::string& signal1, const std::string& signal2) const;

        ~PppModel() = default;

    private:
        const GnssConfig& config_;
        ProductParser& product_parser_;
        LogManager& log_manager_;
        const RinexParser& rinex_parser_;
        StateManager& state_manager_;
        PppUtils ppp_utils_;
        Ionosphere ionosphere_;
        Geometry geometry_;
        Antenna antenna_;
        GnssCorrections gnss_corrections_;

        const std::map<std::string, std::vector<std::string>> signal_priorities_;
        std::vector<double> klobuchar_coeffs_;

        bool selectSignals(const RinexObservation& obs, const std::string& system_str,
            std::vector<std::pair<std::string, std::string>>& selected_signals,
            bool& is_iono_free, std::string& comb_type);

        std::vector<std::string> selectObservationTypes(const std::map<std::string, double>& measurements,
            System system, const std::string& system_str) const;

        bool processIonoFreeObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>> selected_signals,
            const std::string& comb_type, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
            const std::string& rx_antenna_type, double range, double tropo_delay,
            double sat_clock, double shapiro_delay, double sagnac_corr,
            double atm_loading_corr, double relativistic_corr, double solid_tide);

    };

} // namespace GnssProcessor

#endif // PPPMODEL_HPP