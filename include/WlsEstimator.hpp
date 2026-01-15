#pragma once
#ifndef WLSESTIMATOR_HPP
#define WLSESTIMATOR_HPP

#include "GnssConfig.hpp"
#include "StateManager.hpp"
#include "PppTypes.hpp"
#include "WlsLogger.hpp"
#include "LogManager.hpp"
#include <Eigen/Dense>
#include <vector>
#include <limits>

namespace GnssProcessor {

struct WlsResult {
    Eigen::VectorXd state;
    Eigen::MatrixXd covariance;
    double rms = std::numeric_limits<double>::infinity();
    double max_abs = std::numeric_limits<double>::infinity();
    size_t observation_count = 0;
    int iterations = 0;

    bool success() const { return state.size() > 0 && covariance.size() > 0; }
};

class WlsEstimator {
public:
    WlsEstimator(const GnssConfig& config, StateManager& state_manager, LogManager& log_manager);

    WlsResult estimate(const GnssTime& epoch_time, const std::vector<PppObservation>& observations);

private:
    const GnssConfig& config_;
    StateManager& state_manager_;
    LogManager& log_manager_;
    WlsLogger wls_logger_;
};

} // namespace GnssProcessor

#endif // WLSESTIMATOR_HPP