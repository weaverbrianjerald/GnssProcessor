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
        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);
        ~KalmanFilter();
        // Predict state for time step dt
        void predict(double dt);

        // Update state with measurement
        void KalmanFilter::update(EpochMeasurement& measurement);

        // Residual check for potential outliers
        bool validatePostFitResiduals(EpochMeasurement& meas_update, Eigen::VectorXd& x_prev, Eigen::MatrixXd& P_prev);

        // Get current state and covariance
        Eigen::VectorXd residuals_;
        Eigen::VectorXd residuals_covariance;

    private:
        StateManager& state_manager_;
        const GnssConfig& config_;
        LogManager& log_manager_;
        const PppModel& ppp_model_;
        MeasurementModel measurement_model_;
        //Eigen::VectorXd& x_; // Reference to StateManager::state_vector_
        //Eigen::MatrixXd& P_; // Reference to StateManager::covariance_
        Eigen::MatrixXd predicted_covariance_;
        Eigen::MatrixXd R_;
        KalmanFilterLogger logger_;
    };

} // namespace GnssProcessor