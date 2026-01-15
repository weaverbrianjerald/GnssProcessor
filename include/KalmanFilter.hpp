#pragma once

#include "StateManager.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "PppModel.hpp"
#include "MeasurementModel.hpp"
#include <Constants.hpp>
#include <Eigen/Dense>
#include <map>
#include <deque>
#include <string>
#include <vector>
#include <KalmanFilterLogger.hpp>

namespace GnssProcessor {

    class KalmanFilter {
    public:
        enum class UpdateResult {
            Applied,                 // update applied without forced measurement rebuild
            NeedsMeasurementRebuild, // ambiguity reset / R change / full reset happened
            Skipped                  // invalid measurement / no-op
        };

        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);
        ~KalmanFilter();

        // Predict state for time step dt
        void predict(double dt);

        // Update state with measurement
        UpdateResult update(EpochMeasurement& measurement);

        // Residual check for potential outliers
        std::pair<bool, bool> validatePostFitResiduals(EpochMeasurement& meas_update, Eigen::VectorXd& x_prev, Eigen::MatrixXd& P_prev);

    private:
        StateManager& state_manager_;
        const GnssConfig& config_;
        LogManager& log_manager_;
        const PppModel& ppp_model_;
        MeasurementModel measurement_model_;
        Eigen::MatrixXd R_;
        KalmanFilterLogger logger_;
    };

} // namespace GnssProcessor