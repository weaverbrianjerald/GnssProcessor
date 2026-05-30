#pragma once

#include "filter/StateManager.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "filter/ImuIntegrator.hpp"
#include "filter/ResidualHistoryTracker.hpp"
#include "common/Constants.hpp"
#include "logging/KalmanFilterLogger.hpp"
#include <Eigen/Dense>
#include <map>
#include <deque>
#include <set>
#include <string>
#include <vector>
#include <memory>

namespace GnssProcessor {

    class CycleSlipManager;

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
         * @brief IMU-aided prediction between two GNSS epochs.
         *
         * When IMU integration is enabled, this replaces the standard
         * predict() call.  It runs the strapdown mechanization from
         * @p t_start to @p t_end using IMU data, propagates the full
         * state vector (position, velocity, attitude, biases) and injects
         * the corresponding INS error-state transition and process noise
         * into the Kalman filter covariance.  Non-INS states (clock,
         * tropo, ambiguities, etc.) are predicted with the standard
         * random-walk model.
         *
         * @param t_start Previous GNSS epoch time.
         * @param t_end   Current GNSS epoch time.
         * @param imu     Reference to the ImuIntegrator holding loaded data.
         */
        void predictImu(const GnssTime& t_start, const GnssTime& t_end,
                        const ImuIntegrator& imu);

        /**
         * @brief Get the current body-to-ECEF attitude matrix.
         *
         * Maintained by predictImu() and updated after GNSS measurement
         * updates (via the attitude error state feedback).
         *
         * @return Current C_b^e rotation matrix.
         */
        const Eigen::Matrix3d& getAttitude() const { return C_b_e_; }

        /**
         * @brief Set the body-to-ECEF attitude matrix.
         * @param C New attitude rotation matrix.
         */
        void setAttitude(const Eigen::Matrix3d& C) { C_b_e_ = C; }

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
         * @brief Apply a linear pseudo-observation to the filter state.
         *
         * Performs a Joseph-form Kalman update of the StateManager-owned
         * state vector and covariance for a synthetic linear observation
         * with design matrix @p H, innovation @p innovation and noise
         * covariance @p R.  Used for soft constraints that are not RINEX
         * measurements — e.g. the ionosphere-weighting GIM constraint.
         *
         * @param H          Design matrix (m x n) over the full state.
         * @param innovation Observed-minus-predicted vector (m).
         * @param R          Measurement noise covariance (m x m).
         */
        void applyPseudoObservation(const Eigen::MatrixXd& H,
                                    const Eigen::VectorXd& innovation,
                                    const Eigen::MatrixXd& R);

        /**
         * @brief Joseph-form pseudo-observation update kernel (pure, static).
         *
         * Applies a linear measurement update — Kalman gain, state
         * correction and Joseph-form covariance update with explicit
         * symmetrization — to @p x / @p P, writing the result to
         * @p x_out / @p P_out.  The outputs may alias the inputs.
         *
         * @param x          Prior state vector (n).
         * @param P          Prior covariance (n x n).
         * @param H          Design matrix (m x n).
         * @param innovation Observed-minus-predicted vector (m).
         * @param R          Measurement noise covariance (m x m).
         * @param[out] x_out Posterior state vector (n).
         * @param[out] P_out Posterior covariance (n x n).
         */
        static void josephPseudoUpdate(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                       const Eigen::MatrixXd& H, const Eigen::VectorXd& innovation,
                                       const Eigen::MatrixXd& R,
                                       Eigen::VectorXd& x_out, Eigen::MatrixXd& P_out);

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
         * ambiguities reset, so the caller can clear dependent per-satellite
         * state such as cycle-slip detector history.
         *
         * @return Vector of satellite ID strings that were reset.
         */
        const std::vector<std::string>& getLastResetSatellites() const { return last_reset_satellites_; }

        /**
         * @brief Get the list of (satellite, signal) pairs whose ambiguities were
         *        reset during the last update.
         *
         * Parallel to @c getLastResetSatellites but at signal granularity.
         * Cleanup of per-(sat, signal) state (e.g. MW arc histories on the
         * specific signal pair that slipped) should consume this list and
         * spare arcs whose other-signal didn't slip.  When a path resets an
         * ambiguity but cannot pin down the signal, it appends only to
         * @c last_reset_satellites_ and the consumer falls back to a sat-wide
         * reset.
         *
         * @return Vector of (satellite, signal) pairs.
         */
        const std::vector<std::pair<std::string, std::string>>& getLastResetSignals() const {
            return last_reset_signals_;
        }

        /**
         * @brief Check if a complete filter reset occurred during the last update.
         *
         * @return true if a complete filter reset was applied, false otherwise.
         */
        bool wasCompleteReset() const { return was_complete_reset_; }

        /**
         * @brief Provide optional access to the CycleSlipManager for MW-based
         *        outlier tiebreaking.
         *
         * When set, validatePostFitResiduals uses MW deviation scores to
         * disambiguate competing phase outliers with nearly equal
         * standardized residuals.
         *
         * @param csm Pointer to the CycleSlipManager (may be nullptr).
         */
        void setCycleSlipManager(CycleSlipManager* csm) { cycle_slip_manager_ = csm; }

        /**
         * @brief Emit cumulative per-phase timing totals to the
         *        @c kalman_filter logger at info level.
         *
         * Each call to @c update() accumulates microseconds spent in the
         * four hottest phases — innovation covariance / Kalman gain / Joseph
         * covariance / post-fit residuals.  Call once at end of run to see
         * where time went.  Counters reset only when this function is
         * called.
         */
        void logKalmanFilterTimings();

        /**
         * @brief Take ownership of the constellations whose pivot the KF
         *        flagged for forced handover during the last @c update().
         *
         * After @c validatePostFitResiduals detects a coherent residual
         * pattern indicating a pivot satellite slipped (every SD pair
         * against pivot X on band B shows a tight, same-sign offset),
         * the constellation name (e.g. "GPS") is added to an internal
         * set. The @c Application driver retrieves this set after the
         * KF returns @c NeedsMeasurementRebuild and forwards it to
         * @c MeasurementModel::requestPivotHandover, which routes it
         * to @c PivotManager. The handover is then performed at the
         * start of the next rebuild pass's @c computeMeasurementUpdate.
         *
         * Returning by value clears the internal set — callers that
         * need to peek without consuming should add a separate getter.
         *
         * @return Set of constellation names (e.g. {"GPS", "Galileo"}).
         */
        std::set<std::string> takeRequestedPivotHandovers();

        /**
         * @brief Take ownership of the per-constellation replacement-pivot
         *        exclusion sets the post-fit detector recorded alongside
         *        @c takeRequestedPivotHandovers.
         *
         * For each flagged constellation the detector records the satellites
         * that must NOT become the new pivot: the offset pivot itself plus
         * any non-pivot SD row whose residual disagrees with the coherent
         * median by more than a robust k·MAD band (those rows are co-disturbed
         * or independently slipping — electing one merely moves the broken
         * SD-frame anchor from one offset sat to another, which is the
         * post-fit handover flip-flop).  The @c Application driver forwards
         * each set to @c PivotManager::requestForcedHandover so the forced
         * handover lands on a consensus satellite.
         *
         * Returning by value clears the internal map.  Call after (or with)
         * @c takeRequestedPivotHandovers each rebuild pass.
         *
         * @return Map of constellation name -> excluded satellite IDs.
         */
        std::map<std::string, std::set<std::string>>
            takeRequestedPivotHandoverExcludeSats();

        /**
         * @brief Mutable access to the post-fit residual history tracker.
         *
         * Exposed to the @c Application driver so external resets
         * (cycle-slip ambiguity resets in @c StateManager, pivot
         * handovers in @c PivotManager, satellite drops) can call
         * @c resetSatSignal / @c resetSatellite /
         * @c resetConstellationSignal in lockstep with the existing
         * reset machinery.  @c isEnabled() returns false when the
         * feature is off in config; callers can short-circuit on
         * that without touching the maps.
         */
        ResidualHistoryTracker& getResidualHistoryTracker() { return residual_history_; }

        /**
         * @brief Mark the start of a new epoch for concise-log gating.
         *
         * Resets the internal pass counter and switches the kalman_filter
         * sink to passthrough mode (so pass-1 messages emit immediately).
         * No-op when concise mode is disabled.  Safe to call before every
         * epoch; pairs with @c endEpoch.
         */
        void beginEpoch();

        /**
         * @brief Mark end of an epoch's update loop for concise-log gating.
         *
         * Replays the buffer accumulated during the final pass (which
         * contains the last pass's messages, since @c update clears the
         * buffer at the start of each non-first pass) and returns the
         * sink to passthrough mode.  No-op when concise mode is disabled.
         */
        void endEpoch();

    private:
        StateManager& state_manager_;         //!< Reference to external state manager for state storage and access.
        const GnssConfig& config_;            //!< Reference to GNSS configuration (read-only).
        LogManager& log_manager_;             //!< Reference to logging manager for diagnostic output.
        const PppModel& ppp_model_;           //!< Reference to PPP model used for measurement modelling.
        MeasurementModel measurement_model_; //!< Local measurement model helper.
        Eigen::MatrixXd R_;                   //!< Current measurement noise covariance matrix.
        std::unique_ptr<KalmanFilterLogger> logger_;  //!< Optional file logger for Kalman internals (created only in Trace mode).
        
        std::vector<std::string> last_reset_satellites_;  //!< Satellites with ambiguity resets from last update.
        std::vector<std::pair<std::string, std::string>> last_reset_signals_;  //!< (sat, signal) pairs reset from last update; finer granularity than last_reset_satellites_.
        bool was_complete_reset_ = false;                 //!< True if a complete filter reset occurred.
        CycleSlipManager* cycle_slip_manager_ = nullptr;   //!< Optional access to MW data for outlier tiebreaking.
        Eigen::Matrix3d C_b_e_ = Eigen::Matrix3d::Identity(); //!< Body-to-ECEF rotation matrix (IMU attitude).

        // -------------------------------------------------------------------
        // Concise-log state (kalman_filter logger gate). All only used when
        // logging.kalman_filter_concise_log = true; otherwise gate_ is null
        // and the per-epoch hooks are no-ops.
        // -------------------------------------------------------------------
        std::shared_ptr<class GatedSink> gate_;  //!< null when concise mode is off.
        int  pass_index_ = 0;                    //!< 0 between epochs, ≥1 during update().

        /// Constellations whose pivot was flagged during the last residual
        /// validation as requiring a handover (KF-detected pivot slip).
        /// Populated by @c validatePostFitResiduals's pivot-slip detector,
        /// drained by @c takeRequestedPivotHandovers after the KF returns
        /// NeedsMeasurementRebuild. Empty in steady state.
        std::set<std::string> requested_pivot_handovers_;

        /// Per-constellation set of satellites the post-fit pivot-slip
        /// detector flagged as unfit replacement pivots: the offset pivot
        /// plus within-group residual outliers that disagree with the
        /// coherent consensus (co-disturbed / independently-slipping sats).
        /// Drained by @c takeRequestedPivotHandoverExcludeSats in lockstep
        /// with @c requested_pivot_handovers_ and forwarded to PivotManager
        /// as @c exclude_sats so a forced handover lands on a consensus
        /// satellite instead of bouncing to another offset sat. Empty in
        /// steady state.
        std::map<std::string, std::set<std::string>> pivot_handover_exclude_sats_;

        /// Pending Huber R-inflations queued by the post-fit residual
        /// detector for sub-cycle phase rows whose |sres_post| exceeds
        /// the @c phase_huber_sres_threshold.  Key is "<primary_sat>|<signal>";
        /// value is the target R(i,i) (m²) applied at the start of the
        /// next @c update call.  Persists across rebuild passes within
        /// the same epoch and is cleared in @c beginEpoch so each epoch
        /// makes a fresh decision (long-term persistent biases are
        /// covered by the separate EWMA-based inflation).
        std::map<std::string, double> pending_r_inflations_;

        /// Pending sat-bias resets queued by the stuck-code-bias
        /// detector in the clean-commit branch of
        /// @c validatePostFitResiduals.  Key is "<sat>|<signal>" (the
        /// sat-bias state key); value is the EWMA mean of post-fit
        /// residuals for that row at trigger time — added to the
        /// current bias value to snap it to the corrected estimate.
        /// Applied immediately after commit in @c update() so the
        /// next epoch starts with the re-seeded bias state.  Cleared
        /// by @c beginEpoch as a defensive measure; in normal flow
        /// the queue is drained the same epoch it's populated.
        std::map<std::string, double> pending_sat_bias_resets_;

        /// Post-fit residual history tracker.  No-op when
        /// @c estimation.residual_history.enabled is false in config;
        /// otherwise accumulates EWMA mean/variance per (sat, signal)
        /// and is queried by the spatial pivot-slip detector to
        /// corroborate with temporal evidence.
        mutable ResidualHistoryTracker residual_history_;

        // -------------------------------------------------------------------
        // Per-phase timing accumulators (microseconds; written by update())
        // -------------------------------------------------------------------
        long long timing_us_S_  = 0;   //!< H·P + S = HP·Hᵀ + R
        long long timing_us_K_  = 0;   //!< LDLT(S) + solve for K
        long long timing_us_P_  = 0;   //!< Joseph covariance update
        long long timing_us_Sr_ = 0;   //!< Diagonal-only post-fit covariance
        size_t    timing_n_calls_ = 0; //!< Number of update() calls accumulated.

        // -------------------------------------------------------------------
        // Construction helpers
        // -------------------------------------------------------------------

        /**
         * @brief Seed covariance diagonal entries for core states that are uninitialized.
         *
         * Only overrides entries that are non-finite or negative � zero is treated as
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
         * @brief Baseline-mode (DD) variant of @c handlePhaseOutlier.
         *
         * In baseline mode, ambiguities are keyed
         * @c "DD|<cons>|<sat_i>|<rov_code>|<pivot>" — not the PPP/BSSD
         * @c "<sat>_<sig>" format.  Exactly one DD ambiguity exists per
         * (sat_i, rov_code, pivot) triple; the pivot itself has no DD
         * ambiguity and must not be reset.
         *
         * The handler:
         *   - parses @c outlier_sat as @c "sat_i-pivot" (the form emitted by
         *     @c MeasurementModel for DD rows),
         *   - strips a trailing @c "/refcode" suffix from @c outlier_sig
         *     (added by @c MeasurementModel for cross-attribute aliased rows)
         *     to recover the rover RINEX code,
         *   - looks up the DD ambiguity by its full key,
         *   - re-seeds the float ambiguity at @c x(amb_idx) + innovation so
         *     the next epoch's innovation on this row starts near zero, and
         *   - decorrelates and inflates the ambiguity covariance row/col.
         *
         * @param measurement  Epoch measurement (read-only; row index is found
         *                     by matching @c outlier_sat and @c outlier_sig).
         * @param outlier_sat  Combined "sat_i-pivot" identifier from the row.
         * @param outlier_sig  Signal token from the row; possibly aliased
         *                     (e.g. @c "L2W/L2C"); the rover code is taken
         *                     as the substring before any @c "/".
         * @param primary_sat  @c sat_i parsed by the caller (non-pivot).
         * @param ref_sat      Pivot satellite parsed by the caller.
         * @param x            State vector (modified in place at the DD index).
         * @param P            Covariance (row/col cleared, diagonal reset).
         * @param epoch_prefix Log prefix.
         */
        void handleBaselinePhaseOutlier(const EpochMeasurement& measurement,
            const std::string& outlier_sat, const std::string& outlier_sig,
            const std::string& primary_sat, const std::string& ref_sat,
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

} // namespace