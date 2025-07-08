#pragma once

#include "StateManager.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "PppModel.hpp"
#include <MeasurementModel.hpp>
#include <Constants.hpp>
#include <Eigen/Dense>
#include <map>
#include <deque>
#include <string>
#include <vector>

namespace GnssProcessor {

    class KalmanFilter {
    public:
        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);

        // Predict state for time step dt
        void predict(double dt);

        // Update state with measurement
        void update(const GnssProcessor::EpochMeasurement& measurement);

        // Get current state and covariance
        const Eigen::VectorXd& getState() const { return x_; }
        const Eigen::MatrixXd& getCovariance() const { return P_; }

    private:
        StateManager& state_manager_;
        const GnssConfig& config_;
        LogManager& log_manager_;
        const PppModel& ppp_model_;
        Eigen::VectorXd x_;
        Eigen::MatrixXd P_;
        Eigen::MatrixXd R_;
    };

} // namespace GnssProcessor

//#endif // KALMANFILTER_HPP