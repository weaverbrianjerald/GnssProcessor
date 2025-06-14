#pragma once

#include "StateManager.hpp"
#include "PppModel.hpp"
#include "GnssConfig.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "RinexParser.hpp"
#include <Eigen/Dense>
#include <vector>

namespace GnssProcessor {

    struct EpochMeasurement {
        Eigen::VectorXd innovations;
        Eigen::MatrixXd H;
        Eigen::VectorXd R_v;
        GnssTime time;
        std::vector<PppObservation> observations; // For cycle-slip detection
    };

    class MeasurementModel {
    public:
        // Constructor
        MeasurementModel(const StateManager& state_manager, const PppModel& ppp_model, const GnssConfig& config, LogManager& log_manager);

        // Compute measurement update
        EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state);

    private:
        const StateManager& state_manager_;
        const PppModel& ppp_model_;
        const GnssConfig& config_;
        LogManager& log_manager_; // Added for logging
        double getFrequency(const std::string& freq_type, System system) const;
    };

} // namespace GnssProcessor

//#endif // MEASUREMENTMODEL_HPP // Fixed include guard