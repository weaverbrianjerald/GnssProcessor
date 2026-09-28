#pragma once

/**
 * @file WlsEstimator.hpp
 * @brief Weighted least-squares (WLS) estimator interface used for epoch-level positioning.
 *
 * This header declares the `WlsResult` container and the `WlsEstimator` class which
 * perform a weighted least-squares estimation over a set of `PppObservation` at a
 * given epoch. The estimator is configured via `GnssConfig` and reads its seed state
 * (position, clock, zenith troposphere) from `StateManager`; it does not write
 * filter states. Logging is performed through `LogManager` and optional trace
 * records via `WlsLogger`.
 */

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "common/PppTypes.hpp"
#include "logging/WlsLogger.hpp"
#include "logging/LogManager.hpp"
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
    Eigen::VectorXd state;              /**< Estimated state vector: position (3), clock, zenith wet tropo (all meters). */
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
 * The estimator uses configuration parameters from `GnssConfig` and reads its
 * seed state via `StateManager` (read-only). Diagnostic and trace logging is
 * performed through `LogManager` and `WlsLogger`.
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
     * @param state_manager Reference to the StateManager that supplies the seed state.
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
    // Test seam: lets the WlsEstimator unit tests (tests/test_geometry.cpp)
    // construct the private WlsObs and call the private static predictObs in
    // isolation.  No public-API change; defined only in the test binary.
    friend struct WlsEstimatorTestAccess;

    const GnssConfig& config_;      /**< Reference to configuration parameters. */
    StateManager& state_manager_;   /**< Reference to the StateManager for state queries (read-only). */
    LogManager& log_manager_;       /**< Reference to LogManager for logging. */
    std::unique_ptr<WlsLogger> wls_logger_; /**< Optional WLS logger: created only when debug logging is enabled. */

    static constexpr size_t kCoreSize = 5;  /**< Number of core states: position(3) + clock + tropo. */

    /** @brief Per-observation data collected during filtering. */
    struct WlsObs {
        double observed;            /**< Observed pseudorange (meters). */
        double modeled;             /**< Modeled pseudorange (meters). */
        double variance;            /**< Observation variance (meters²). */
        double mf_w;                /**< Wet troposphere mapping factor. */
        double initial_range;       /**< Geometric range at observation time (meters). */
        double elevation;           /**< Satellite elevation (degrees). */
        Eigen::Vector3d sat_pos;    /**< Satellite ECEF position (meters). */
        std::string satellite;      /**< Satellite identifier. */
        std::string combo_type;     /**< Observation type key. */
    };

    /**
     * @brief Filter and collect valid code observations for WLS.
     * @param observations Per-satellite PPP observations for the epoch.
     * @param epoch_tow    GPS time-of-week (for logging).
     * @return Vector of WlsObs passing system, geometry and elevation checks.
     */
    std::vector<WlsObs> filterObservations(const std::vector<PppObservation>& observations, double epoch_tow) const;

    /**
     * @brief Compute predicted pseudorange for a single observation.
     * @param obs     Filtered observation entry.
     * @param pos     Current receiver position estimate (meters).
     * @param clock   Current receiver clock estimate (meters).
     * @param trop_z  Current zenith wet troposphere estimate (meters).
     * @return Predicted pseudorange (meters).
     */
    static double predictObs(const WlsObs& obs, const Eigen::Vector3d& pos, double clock, double trop_z);

    /**
     * @brief Compute RMS and max-absolute residuals over filtered observations.
     * @param wls_obs Filtered observations.
     * @param pos     Final position estimate.
     * @param clock   Final clock estimate.
     * @param trop_z  Final zenith wet tropo estimate.
     * @param[out] rms     Root-mean-square of residuals.
     * @param[out] max_abs Maximum absolute residual.
     */
    static void computeResiduals(const std::vector<WlsObs>& wls_obs,
        const Eigen::Vector3d& pos, double clock, double trop_z,
        double& rms, double& max_abs);
};

} // namespace GnssProcessor