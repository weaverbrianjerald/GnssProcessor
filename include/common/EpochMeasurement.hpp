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
        Eigen::MatrixXd residuals_covariance;        /**< Covariance of residuals (diagonal Sr by default; full Sr when enableFullRKernel() and a dense R is supplied) */
        Eigen::VectorXd residuals_standardized;      /**< Standardized (normalized) residuals for outlier detection */
        // --- Statistical QC diagnostics (item #9 / M-PAGES NgsLsq + Baarda).
        //     Populated only when enableVarianceFactor() or enableDataSnooping()
        //     is on; otherwise left empty or at their neutral defaults, so the
        //     output with both off is byte-identical. ---
        Eigen::VectorXd residual_redundancy;         /**< Per-row redundancy number rᵢ = Sr_ii/Rᵢᵢ ∈ [0,1] (Baarda). */
        Eigen::VectorXd residual_wtest;              /**< Per-row Baarda w-test statistic wᵢ = vᵢ/√Sr_ii. */
        Eigen::VectorXd residual_mdb;                /**< Per-row minimal detectable bias MDBᵢ (m). */
        double variance_factor{ 1.0 };               /**< A-posteriori variance factor σ̂₀² = (Σ vᵢ²/Rᵢᵢ)/dof (1.0 when not computed). */
        double variance_dof{ 0.0 };                  /**< Effective redundancy / degrees of freedom = Σ rᵢ used for σ̂₀². */
        size_t bad_residual_count{ 0 };              /**< Count of residuals flagged as bad/outliers */
        std::set<std::string> excluded_measurements; /**< Set of "satellite_signaltype" keys (primary satellite of a BSSD pair) excluded from the rebuild: code and phase outliers, rows of a satellite whose ambiguities were reset, and rows of a pivot-slip group (persists across rebuild passes) */
    };

} // namespace GnssProcessor
