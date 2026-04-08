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

        /**
         * @brief Get the list of satellites that had ambiguities reset during the last update.
         *
         * After a call to update() that returns NeedsMeasurementRebuild, this method
         * returns the list of satellite IDs (e.g., "G01", "E11") that had their
         * ambiguities reset. The caller can use this to reset phase wind-up state
         * for those satellites.
         *
         * @return Vector of satellite ID strings that were reset.
         */
        const std::vector<std::string>& getLastResetSatellites() const { return last_reset_satellites_; }

        /**
         * @brief Check if a complete filter reset occurred during the last update.
         *
         * @return true if a complete filter reset was applied, false otherwise.
         */
        bool wasCompleteReset() const { return was_complete_reset_; }

    private:
        StateManager& state_manager_;         //!< Reference to external state manager for state storage and access.
        const GnssConfig& config_;            //!< Reference to GNSS configuration (read-only).
        LogManager& log_manager_;             //!< Reference to logging manager for diagnostic output.
        const PppModel& ppp_model_;           //!< Reference to PPP model used for measurement modelling.
        MeasurementModel measurement_model_; //!< Local measurement model helper.
        Eigen::MatrixXd R_;                   //!< Current measurement noise covariance matrix.
        std::unique_ptr<KalmanFilterLogger> logger_;  //!< Optional file logger for Kalman internals (created only in Trace mode).
        
        std::vector<std::string> last_reset_satellites_;  //!< Satellites with ambiguity resets from last update.
        bool was_complete_reset_ = false;                 //!< True if a complete filter reset occurred.

        // -------------------------------------------------------------------
        // Construction helpers
        // -------------------------------------------------------------------

        /**
         * @brief Seed covariance diagonal entries for core states that are uninitialized.
         *
         * Only overrides entries that are non-finite or negative — zero is treated as
         * a deliberate "fixed state" choice.
         */
        void seedCovarianceDiagonals(Eigen::MatrixXd& P, size_t state_size) const;

        // -------------------------------------------------------------------
        // Outlier handling helpers (decomposed from validatePostFitResiduals)
        // -------------------------------------------------------------------

        /**
         * @brief Handle a phase outlier by resetting the offending ambiguity state(s).
         *
         * For BSSD, both the primary and reference satellite ambiguities are reset.
         *
         * @param measurement  Epoch measurement (for observation lookup).
         * @param outlier_sat  Combined satellite ID (may contain '-' for BSSD).
         * @param outlier_sig  Signal type of the outlier.
         * @param x            Pre-update state vector (modified in place).
         * @param P            Pre-update covariance (modified in place).
         * @param epoch_prefix Log prefix.
         */
        void handlePhaseOutlier(const EpochMeasurement& measurement,
            const std::string& outlier_sat, const std::string& outlier_sig,
            Eigen::VectorXd& x, Eigen::MatrixXd& P,
            const std::string& epoch_prefix);

        /**
         * @brief Handle a code outlier by excluding the measurement row.
         *
         * Adds the offending satellite+signal key to the exclusion set and
         * physically removes the row from all measurement vectors/matrices.
         *
         * @param measurement  Epoch measurement (modified in place).
         * @param outlier_sat  Combined satellite ID (may contain '-' for BSSD).
         * @param outlier_sig  Signal type.
         * @param max_idx      Row index of the outlier in the measurement vectors.
         * @param epoch_prefix Log prefix.
         */
        void handleCodeOutlier(EpochMeasurement& measurement,
            const std::string& outlier_sat, const std::string& outlier_sig,
            size_t max_idx, const std::string& epoch_prefix);

        /**
         * @brief Perform a complete filter reset (covariance re-initialization).
         *
         * Zeros P and re-seeds core-state, ISB, iono, and ambiguity diagonal
         * entries to large-variance defaults.
         *
         * @param P            Covariance (zeroed and re-seeded in place).
         * @param epoch_prefix Log prefix.
         */
        void resetFilterCovariance(Eigen::MatrixXd& P, const std::string& epoch_prefix);
    };

} // namespace GnssProcessor