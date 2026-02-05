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
#include <memory>

namespace GnssProcessor {

    /**
     * @file KalmanFilter.hpp
     * @brief Kalman filter interface for PPP/PPP-RTK state estimation.
     *
     * This header declares the KalmanFilter class responsible for state
     * prediction and measurement updates used by the GNSS processor.
     */

    /**
     * @class KalmanFilter
     * @brief Encapsulates prediction and update logic for GNSS state estimation.
     *
     * The KalmanFilter coordinates the state vector maintained in a
     * StateManager, applies process noise and time updates (prediction),
     * and performs measurement updates using a MeasurementModel. It also
     * validates post-fit residuals and reports whether an update was applied,
     * skipped, or requires a measurement rebuild (e.g. ambiguity reset or
     * full state reset).
     */
    class KalmanFilter {
    public:
        /**
         * @enum UpdateResult
         * @brief Result of attempting to apply a measurement update.
         *
         * - Applied: The update was applied successfully without forcing a
         *            measurement rebuild.
         * - NeedsMeasurementRebuild: Update caused ambiguity reset, measurement
         *                           noise (R) change, or a full reset requiring
         *                           measurements to be rebuilt.
         * - Skipped: Update was skipped due to invalid measurement or no-op.
         */
        enum class UpdateResult {
            Applied,                 // update applied without forced measurement rebuild
            NeedsMeasurementRebuild, // ambiguity reset / R change / full reset happened
            Skipped                  // invalid measurement / no-op
        };

        /**
         * @brief Construct a KalmanFilter.
         *
         * @param state_manager Reference to the StateManager that stores the state vector and covariance.
         * @param config Reference to the GnssConfig providing runtime parameters and defaults.
         * @param log_manager Reference to the LogManager used for logging diagnostics and warnings.
         * @param ppp_model Reference to the PppModel containing PPP-specific modelling (satellite clocks, biases, etc.).
         */
        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);

        /**
         * @brief Destroy the KalmanFilter.
         */
        ~KalmanFilter();

        /**
         * @brief Predict the state forward by time interval dt.
         *
         * Advances the filter state using the process model and process noise.
         * This typically updates the state vector and covariance in the
         * associated StateManager according to the configured process noise
         * parameters.
         *
         * @param dt Time step in seconds to predict forward.
         */
        void predict(double dt);

        /**
         * @brief Perform a measurement update using the provided epoch measurement.
         *
         * The method computes the Kalman gain, applies the state correction,
         * updates covariances, and handles special cases such as ambiguity
         * resets or invalid measurements. The result indicates whether the
         * update was applied, skipped, or caused a measurement rebuild.
         *
         * @param measurement EpochMeasurement containing observations, design matrix H, and measurement noise.
         * @return UpdateResult describing the outcome (Applied, NeedsMeasurementRebuild, Skipped).
         */
        UpdateResult update(EpochMeasurement& measurement);

        /**
         * @brief Validate post-fit residuals to detect outliers and consistency issues.
         *
         * Computes and evaluates standardized residuals (and other metrics)
         * after an update attempt. This routine can be used to flag bad
         * measurements and determine whether the filter state should be
         * reverted or adjusted.
         *
         * @param meas_update The epoch measurement used for the update (may be modified with flags).
         * @param x_prev Vector containing the previous state estimate (before update).
         * @param P_prev Matrix containing the previous covariance (before update).
         * @return pair<bool, bool> where
         *         - first: true if residuals pass validation (no major outliers),
         *         - second: true if any corrective action (e.g., reweighting or removal) was applied.
         */
        std::pair<bool, bool> validatePostFitResiduals(EpochMeasurement& meas_update, Eigen::VectorXd& x_prev, Eigen::MatrixXd& P_prev);

    private:
        StateManager& state_manager_;         //!< Reference to external state manager for state storage and access.
        const GnssConfig& config_;            //!< Reference to GNSS configuration (read-only).
        LogManager& log_manager_;             //!< Reference to logging manager for diagnostic output.
        const PppModel& ppp_model_;           //!< Reference to PPP model used for measurement modelling.
        MeasurementModel measurement_model_; //!< Local measurement model helper.
        Eigen::MatrixXd R_;                   //!< Current measurement noise covariance matrix.
        std::unique_ptr<KalmanFilterLogger> logger_;  //!< Optional file logger for Kalman internals (created only in Trace mode).
    };

} // namespace GnssProcessor