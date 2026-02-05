#pragma once
#ifndef WLSESTIMATOR_HPP
#define WLSESTIMATOR_HPP

/**
 * @file WlsEstimator.hpp
 * @brief Weighted least-squares (WLS) estimator interface used for epoch-level positioning.
 *
 * This header declares the `WlsResult` container and the `WlsEstimator` class which
 * perform a weighted least-squares estimation over a set of `PppObservation` at a
 * given epoch. The estimator is configured via `GnssConfig` and uses `StateManager`
 * for accessing/augmenting filter states. Logging is performed through `LogManager`
 * and optional trace records via `WlsLogger`.
 */

#include "GnssConfig.hpp"
#include "StateManager.hpp"
#include "PppTypes.hpp"
#include "WlsLogger.hpp"
#include "LogManager.hpp"
#include <Eigen/Dense>
#include <vector>
#include <limits>

namespace GnssProcessor {

/**
 * @struct WlsResult
 * @brief Container for results returned by a WLS estimation.
 *
 * The struct stores the estimated state vector and covariance, basic
 * quality metrics (RMS, maximum absolute residual), counts and the
 * number of iterations performed by any iterative solver used.
 */
struct WlsResult {
    Eigen::VectorXd state;              /**< Estimated state vector (meters and other state units). */
    Eigen::MatrixXd covariance;         /**< Estimated state covariance matrix. */
    double rms = std::numeric_limits<double>::infinity();    /**< Root-mean-square of residuals. */
    double max_abs = std::numeric_limits<double>::infinity();/**< Maximum absolute residual. */
    size_t observation_count = 0;       /**< Number of observations used in the solution. */
    int iterations = 0;                 /**< Iterations performed by the solver (if iterative). */

    /**
     * @brief Check whether the WLS produced a valid result.
     * @return true When both state and covariance are non-empty.
     * @return false Otherwise.
     */
    bool success() const { return state.size() > 0 && covariance.size() > 0; }
};

/**
 * @class WlsEstimator
 * @brief Performs a weighted least-squares solution for a single epoch.
 *
 * The estimator uses configuration parameters from `GnssConfig` and may read
 * or update state information via `StateManager`. Diagnostic and trace logging
 * is performed through `LogManager` and `WlsLogger`.
 */
class WlsEstimator {
public:
    /**
     * @brief Construct a WlsEstimator.
     *
     * The estimator holds references to the provided configuration and state
     * manager; it does not take ownership of those objects.
     *
     * @param config Reference to global GNSS processing configuration.
     * @param state_manager Reference to the StateManager used for state access/updates.
     * @param log_manager Reference to the LogManager for logging.
     */
    WlsEstimator(const GnssConfig& config, StateManager& state_manager, LogManager& log_manager);

    /**
     * @brief Run WLS estimation for a single epoch.
     *
     * The method consumes a vector of `PppObservation` for the specified epoch time
     * and returns a `WlsResult` containing the estimated state, covariance and
     * diagnostic metrics. The estimator is responsible for forming the design
     * matrix, weighting the observations and solving the normal equations.
     *
     * @param epoch_time Epoch time at which to estimate the receiver state.
     * @param observations Vector of per-satellite observations used in the WLS.
     * @return WlsResult Result container indicating success/failure and metrics.
     */
    WlsResult estimate(const GnssTime& epoch_time, const std::vector<PppObservation>& observations);

private:
    const GnssConfig& config_;      /**< Reference to configuration parameters. */
    StateManager& state_manager_;   /**< Reference to the StateManager for state queries/updates. */
    LogManager& log_manager_;       /**< Reference to LogManager for logging. */
    //WlsLogger wls_logger_;          /**< Local logger used to produce WLS traces (file-managed). */
    std::unique_ptr<WlsLogger> wls_logger_; /**< Optional WLS logger: created only when debug logging is enabled. */
};

} // namespace GnssProcessor

#endif // WLSESTIMATOR_HPP