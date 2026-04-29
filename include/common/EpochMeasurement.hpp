#pragma once

/**
 * @file EpochMeasurement.hpp
 * @brief Per-epoch measurement bundle handed to the Kalman filter.
 *
 * Aggregates the innovation vector, design matrix @c H, measurement
 * noise (@c R_v diagonal or @c R_full full block when BSSD is active),
 * the row-aligned satellite/signal metadata used for diagnostics, and
 * the post-fit residuals + standardized residuals computed by the KF.
 * The @c excluded_measurements set carries row signatures across
 * rebuild-loop iterations so flagged outliers stay flagged.
 */

#include "common/PppObservation.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Measurements and matrices assembled for a single epoch.
     *
     * This struct contains the innovation vector, design matrix, measurement noise,
     * and residual diagnostics for all observations in an epoch.
     */
    struct EpochMeasurement {
        Eigen::VectorXd innovations;                 /**< Innovation vector (observed - predicted) */
        Eigen::MatrixXd H;                           /**< Design matrix (partial derivatives of observations w.r.t states) */
        Eigen::VectorXd R_v;                         /**< Diagonal of measurement noise variances (or packed vector form) */
        Eigen::MatrixXd R_full;                      /**< Full measurement noise covariance (set by BSSD; empty when unused) */
        GnssTime time;                               /**< Epoch time for these measurements */
        std::vector<PppObservation> observations;    /**< Per-satellite observations at this epoch */
        std::vector<std::string> satellite_ids;      /**< Satellite identifiers ordered to match vectors/matrices */
        std::vector<std::string> signal_types;       /**< Signal/observable types corresponding to measurement rows */
        Eigen::VectorXd residuals;                   /**< Post-fit residuals */
        Eigen::MatrixXd residuals_covariance;        /**< Covariance of residuals */
        Eigen::VectorXd residuals_standardized;      /**< Standardized (normalized) residuals for outlier detection */
        size_t bad_residual_count{ 0 };              /**< Count of residuals flagged as bad/outliers */
        std::set<std::string> excluded_measurements; /**< Set of "satellite_signaltype" keys excluded as code outliers (persists across rebuild passes) */
    };

} // namespace GnssProcessor
