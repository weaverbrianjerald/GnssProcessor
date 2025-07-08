#pragma once
#ifndef MEASUREMENTMODEL_HPP
#define MEASUREMENTMODEL_HPP

#include "StateManager.hpp"
#include "PppModel.hpp"
#include "GnssConfig.hpp"
#include "Constants.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include <Eigen/Dense>
#include <vector>
#include <string>

namespace GnssProcessor {

    class MeasurementModel {
    public:
        // Constructor
        MeasurementModel(const StateManager& state_manager,
            const PppModel& ppp_model,
            const GnssConfig& config,
            LogManager& log_manager);

        // Compute measurement update for Kalman filter
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state);

        // Validate measurement update before Kalman filter processing
        bool validateMeasurementUpdate(const EpochMeasurement& measurement) const;

        // Convert System enum to string
        static std::string getSystemString(System system);

    private:
        const StateManager& state_manager_;
        const PppModel& ppp_model_;
        const GnssConfig& config_;
        LogManager& log_manager_;
    };

} // namespace GnssProcessor

#endif // MEASUREMENTMODEL_HPP