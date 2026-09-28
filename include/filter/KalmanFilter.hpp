#pragma once

#include "filter/StateManager.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "filter/ImuIntegrator.hpp"
#include "filter/ResidualHistoryTracker.hpp"
#include "aiding/AidingTypes.hpp"
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
     * @brief Kalman filter for PPP/PPP-RTK state estimation.
     */

    /**
     * @class KalmanFilter
     * @brief Prediction and measurement update over StateManager-owned state.
     *
     * Applies time updates with process noise, measurement updates via
     * MeasurementModel, and post-fit residual validation.  Reports whether an
     * update was applied, skipped, or needs a measurement rebuild (ambiguity
     * reset or full reset).
     */
    class KalmanFilter {
    public:
        /**
         * @enum UpdateResult
         * @brief Outcome of a measurement update attempt.
         */
        enum class UpdateResult {
            Applied,                 // update applied without forced measurement rebuild
            NeedsMeasurementRebuild, // ambiguity reset / R change / full reset happened
            Skipped                  // invalid measurement / no-op
        };

        /**
         * @brief Construct a KalmanFilter.
         *
         * @param state_manager Owner of the state vector and covariance.
         * @param config Runtime parameters.
         * @param log_manager Diagnostic logging.
         * @param ppp_model PPP modelling (satellite clocks, biases, ...).
         */
        KalmanFilter(StateManager& state_manager, const GnssConfig& config, LogManager& log_manager, const PppModel& ppp_model);

        /** @brief Destroy the KalmanFilter. */
        ~KalmanFilter();

        /**
         * @brief Predict the state forward by @p dt using the process model.
         * @param dt Time step in seconds.
         */
        void predict(double dt);

        /**
         * @brief IMU-aided prediction replacing predict() when IMU is enabled.
         *
         * Runs the strapdown mechanization over [t_start, t_end], propagating
         * position/velocity/attitude/biases and injecting the INS error-state
         * transition and process noise.  Non-INS states (clock, tropo,
         * ambiguities) use the standard random-walk model.
         *
         * @param t_start Previous GNSS epoch time.
         * @param t_end   Current GNSS epoch time.
         * @param imu     ImuIntegrator holding the loaded data.
         */
        void predictImu(const GnssTime& t_start, const GnssTime& t_end,
                        const ImuIntegrator& imu);

        /**
         * @brief Composable generalization of predictImu over aiding sources.
         *
         * Each contribution supplies a Phi/Q sub-block over the full-state
         * indices it owns plus an optional nominal write-back.  Blocks are
         * spliced (ASSIGNED, never accumulated) onto an Identity-seeded
         * transition and the StateManager process-noise baseline, then P is
         * propagated once.
         *
         * With no valid contribution this delegates to predict(dt), preserving
         * GNSS-only behavior byte-for-byte.  With an INS contribution the
         * assembly mirrors predictImu exactly: Identity-seeded F (non-INS means
         * are not propagated) and baseline Q over the contribution's integrated
         * interval rather than the GNSS dt.
         *
         * The nominal attitude (C_b_e_) is written by the IMU source during
         * contribution assembly, not here.
         *
         * @param dt            GNSS interval (used only on the delegated path).
         * @param contributions Per-source predict contributions for this epoch.
         */
        void predictAided(double dt, const std::vector<PredictContribution>& contributions);

        /**
         * @brief Current body-to-ECEF attitude C_b^e.
         *
         * Written by setAttitude() (the IMU aiding source, after each aided
         * prediction), by predictImu() (which has no callers) and by attitude
         * error-state feedback after GNSS updates.
         */
        const Eigen::Matrix3d& getAttitude() const { return C_b_e_; }

        /** @brief Set the body-to-ECEF attitude matrix. */
        void setAttitude(const Eigen::Matrix3d& C) { C_b_e_ = C; }

        /**
         * @brief Measurement update from the current StateManager prior.
         *
         * Computes the gain, applies the state correction and covariance
         * update, and handles ambiguity resets and invalid measurements.
         *
         * @param measurement Observations, design matrix H, and noise.
         * @return Applied, NeedsMeasurementRebuild, or Skipped.
         */
        UpdateResult update(EpochMeasurement& measurement);

        /**
         * @brief One iterated-EKF (Gauss-Newton) step against a FIXED prior.
         *
         * @ref update treats the current StateManager (x, P) as the prior, so
         * reapplying it to the same information contracts the covariance twice.
         * This anchors on (@p x_prior, @p P_prior) instead:
         *
         *   - gain        K = P⁻·Hᵀ·(H·P⁻·Hᵀ + R)⁻¹              (from the prior)
         *   - innovation  ν = (z − h(xᵢ)) − H·(x⁻ − xᵢ)          (offset-corrected)
         *   - state       x⁺ = x⁻ + K·ν
         *   - covariance  P⁺ = (I − K·H)·P⁻·(…)ᵀ + K·R·Kᵀ        (Joseph, once)
         *
         * xᵢ is the current StateManager state, i.e. the linearization point of
         * @p measurement.  The −H·(x⁻ − xᵢ) term makes this a proper
         * Gauss-Newton step toward the MAP estimate rather than a naive
         * re-update.  Iterating with the SAME prior converges to the IEKF fixed
         * point without accumulating over-confidence.
         *
         * Does no outlier handling and requests no rebuild — the caller must
         * supply an already-cleaned measurement set.  Performs no IMU nominal
         * feedback; the tightly-coupled path keeps its legacy second pass.
         *
         * @param[in,out] measurement Cleaned set; residual fields are filled on
         *        a committed update.
         * @param x_prior Epoch-entry prior state x⁻ (post-predict, pre-update).
         * @param P_prior Epoch-entry prior covariance P⁻.
         * @return Applied on commit; Skipped on a degenerate or non-finite
         *         solve (state left unchanged).
         */
        UpdateResult updateFromPrior(EpochMeasurement& measurement,
                                     const Eigen::VectorXd& x_prior,
                                     const Eigen::MatrixXd& P_prior);

        /**
         * @brief Pre-update prior x⁻ from the last Applied update, as an
         *        @ref updateFromPrior anchor.
         *
         * Captured only when geometry relinearization is enabled on the
         * non-tightly-coupled path.  Being the state the primary update ran on,
         * it already reflects this epoch's ambiguity augmentation and outlier
         * resets, so its dimension matches.  Empty until the first such update.
         */
        const Eigen::VectorXd& getLastUpdatePriorX() const { return last_update_prior_x_; }

        /** @brief Prior covariance P⁻ paired with @ref getLastUpdatePriorX. */
        const Eigen::MatrixXd& getLastUpdatePriorP() const { return last_update_prior_P_; }

        /**
         * @brief Apply a linear pseudo-observation (Joseph form).
         *
         * Used for soft constraints that are not RINEX measurements, e.g. the
         * ionosphere-weighting GIM constraint.
         *
         * @param H          Design matrix (m x n) over the full state.
         * @param innovation Observed-minus-predicted vector (m).
         * @param R          Measurement noise covariance (m x m).
         */
        void applyPseudoObservation(const Eigen::MatrixXd& H,
                                    const Eigen::VectorXd& innovation,
                                    const Eigen::MatrixXd& R);

        /**
         * @brief Pure Joseph-form update kernel; outputs may alias the inputs.
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
         * @brief Screen post-fit residuals for outliers and inconsistency.
         *
         * @param meas_update Measurement used for the update (flags may be set).
         * @param[in,out] x_prev Pre-update state estimate; edited in place on the
         *        outlier path (@ref handlePhaseOutlier).
         * @param[in,out] P_prev Pre-update covariance; edited in place on the
         *        outlier path (@ref handlePhaseOutlier, or a full covariance reset).
         * @return {outlier found, complete reset}.  A complete reset also sets
         *         the first flag.  On an outlier, @ref update returns
         *         UpdateResult::NeedsMeasurementRebuild.
         */
        std::pair<bool, bool> validatePostFitResiduals(EpochMeasurement& meas_update, Eigen::VectorXd& x_prev, Eigen::MatrixXd& P_prev);

        /**
         * @brief Compute per-epoch statistical QC diagnostics.
         *
         * Forms the a-posteriori variance factor σ̂₀² = (Σ vᵢ²/Rᵢᵢ)/dof over
         * dof = Σ redundancy numbers, plus the Baarda data-snooping triple:
         * redundancy rᵢ = Sr_ii/Rᵢᵢ, w-test wᵢ = vᵢ/√Sr_ii, and MDBᵢ.  Results
         * are stored on @p meas.  Pure diagnostics — touches neither state nor
         * covariance.
         *
         * @param meas    Epoch measurement; QC fields filled in place.
         * @param R       Working measurement-noise covariance (possibly inflated).
         * @param sr_diag Diagonal of Sr = R − H·P⁺·Hᵀ.
         * @param Sr_full Optional full Sr for a correlation-aware form via Sr⁻¹;
         *                nullptr selects the diagonal form.
         */
        void computeResidualQc_(EpochMeasurement& meas, const Eigen::MatrixXd& R,
                                const Eigen::VectorXd& sr_diag,
                                const Eigen::MatrixXd* Sr_full) const;

        /**
         * @brief Satellites whose ambiguities were reset in the last update.
         *
         * Valid after update() returns NeedsMeasurementRebuild; lets the caller
         * clear dependent per-satellite state such as cycle-slip history.
         */
        const std::vector<std::string>& getLastResetSatellites() const { return last_reset_satellites_; }

        /**
         * @brief (satellite, signal) pairs reset in the last update.
         *
         * Signal-granular parallel to @ref getLastResetSatellites, so per-(sat,
         * signal) state such as MW arc history is spared on signals that did
         * not slip.  A path that resets without identifying the signal appends
         * only to the satellite list, and consumers fall back to a sat-wide
         * reset.
         */
        const std::vector<std::pair<std::string, std::string>>& getLastResetSignals() const {
            return last_reset_signals_;
        }

        /** @brief True if the last update performed a complete filter reset. */
        bool wasCompleteReset() const { return was_complete_reset_; }

        /**
         * @brief Supply the CycleSlipManager for MW-based outlier tiebreaking.
         *
         * When set, validatePostFitResiduals uses MW deviation scores to
         * separate competing phase outliers with near-equal residuals.
         *
         * @param csm CycleSlipManager, or nullptr.
         */
        void setCycleSlipManager(CycleSlipManager* csm) { cycle_slip_manager_ = csm; }

        /**
         * @brief Log cumulative per-phase timing totals, then reset them.
         *
         * Each update() accumulates microseconds in the four hottest phases
         * (innovation covariance, gain, Joseph covariance, post-fit residuals).
         * Counters reset only on this call.
         */
        void logKalmanFilterTimings();

        /**
         * @brief Drain the constellations whose pivot needs a forced handover.
         *
         * Populated when validatePostFitResiduals sees a coherent residual
         * pattern across every SD pair against one pivot.  The Application
         * driver drains this after NeedsMeasurementRebuild and forwards it to
         * MeasurementModel::requestPivotHandover, which routes it to
         * PivotManager for the next rebuild pass.  Returning by value clears
         * the set.
         *
         * @return Constellation names, e.g. {"GPS", "Galileo"}.
         */
        std::set<std::string> takeRequestedPivotHandovers();

        /**
         * @brief Drain the replacement-pivot exclusion sets recorded alongside
         *        @ref takeRequestedPivotHandovers.
         *
         * Excludes the offset pivot itself plus any SD row disagreeing with the
         * coherent median by more than a robust k·MAD band — those are
         * co-disturbed or independently slipping, and electing one merely moves
         * the broken anchor, which is the handover flip-flop.  Forwarded to
         * PivotManager::requestForcedHandover so the handover lands on a
         * consensus satellite.  Returning by value clears the map.
         *
         * @return Constellation -> excluded satellite IDs.
         */
        std::map<std::string, std::set<std::string>>
            takeRequestedPivotHandoverExcludeSats();

        /**
         * @brief Drain the satellites needing iono re-inflation after a
         *        CODE-detected pivot handover.
         *
         * In code-only uncombined PPP-RTK a coherent pivot-side code bias is
         * absorbed into the weakly-observable per-satellite ionosphere states
         * of the coherent majority of the firing SD-code group.  The handover's
         * T·P·Tᵀ re-reference conserves information and cannot evict it, and
         * the phase scrub path is dead without carrier phase.  Re-inflating
         * these states after re-referencing lets the clean frame re-estimate
         * them, breaking the converged-but-wrong lock that makes the outliers
         * recur every epoch.
         *
         * Populated only by the code-side detector; the phase detectors scrub
         * in the ambiguity domain instead.  Returning by value clears the map.
         *
         * @return Constellation -> satellite IDs to re-inflate.
         */
        std::map<std::string, std::set<std::string>>
            takeRequestedPivotHandoverIonoReinflateSats();

        /**
         * @brief Mutable access to the post-fit residual history tracker.
         *
         * Lets the Application driver reset history in lockstep with external
         * resets (cycle slips, pivot handovers, satellite drops).  isEnabled()
         * is false when the feature is off in config.
         */
        ResidualHistoryTracker& getResidualHistoryTracker() { return residual_history_; }

        /**
         * @brief Start an epoch for concise-log gating.
         *
         * Resets the pass counter and puts the kalman_filter sink in
         * passthrough so pass-1 messages emit immediately.  No-op when concise
         * mode is off; pairs with @ref endEpoch.
         */
        void beginEpoch();

        /**
         * @brief End an epoch's update loop for concise-log gating.
         *
         * Replays the final pass's buffer (update() clears the buffer at the
         * start of each non-first pass) and returns the sink to passthrough.
         * No-op when concise mode is off.
         */
        void endEpoch();

    private:
        // Test seam: KalmanFilterTestAccess (tests/test_aiding_source.cpp) reaches
        // spliceAndPropagate so aided-predict assembly is testable without a full
        // KalmanFilter. Defined only in gnss_tests.
        friend struct KalmanFilterTestAccess;

        StateManager& state_manager_;         //!< Owns the state vector and covariance.
        const GnssConfig& config_;            //!< Runtime configuration (read-only).
        LogManager& log_manager_;             //!< Diagnostic logging.
        const PppModel& ppp_model_;           //!< PPP measurement modelling.
        MeasurementModel measurement_model_; //!< Local measurement model helper.
        Eigen::MatrixXd R_;                   //!< Current measurement noise covariance.
        std::unique_ptr<KalmanFilterLogger> logger_;  //!< File logger for filter internals (Trace mode only).

        std::vector<std::string> last_reset_satellites_;  //!< Satellites with ambiguity resets from the last update.
        std::vector<std::pair<std::string, std::string>> last_reset_signals_;  //!< Same, at (sat, signal) granularity.
        bool was_complete_reset_ = false;                 //!< True if a complete filter reset occurred.
        CycleSlipManager* cycle_slip_manager_ = nullptr;   //!< MW data for outlier tiebreaking; may be null.
        Eigen::Matrix3d C_b_e_ = Eigen::Matrix3d::Identity(); //!< Body-to-ECEF rotation (IMU attitude).

        /// Pre-update prior (x, P) from the last Applied update().  Stashed only
        /// under geometry relinearization on the non-tightly-coupled path, so the
        /// IEKF anchor always matches the post-update state dimension.
        Eigen::VectorXd last_update_prior_x_;
        Eigen::MatrixXd last_update_prior_P_;

        // Concise-log state. Used only when logging.kalman_filter_concise_log is
        // true; otherwise gate_ is null and the per-epoch hooks are no-ops.
        std::shared_ptr<class GatedSink> gate_;  //!< null when concise mode is off.
        int  pass_index_ = 0;                    //!< 0 between epochs, ≥1 during update().

        /// Constellations whose pivot the post-fit slip detector flagged for
        /// handover.  Drained by takeRequestedPivotHandovers.  Empty in steady state.
        std::set<std::string> requested_pivot_handovers_;

        /// Satellites unfit as replacement pivot: the offset pivot plus
        /// within-group residual outliers that disagree with the coherent
        /// consensus.  Drained in lockstep with requested_pivot_handovers_ and
        /// forwarded to PivotManager so a forced handover does not bounce to
        /// another offset sat.  Empty in steady state.
        std::map<std::string, std::set<std::string>> pivot_handover_exclude_sats_;

        /// Non-pivot satellites whose iono / sat-bias state absorbed a coherent
        /// pivot-side CODE bias and must be re-inflated once the handover
        /// re-references the SD frame.  Populated only by the code-side
        /// detector (the coherent firing group, not the exclude-set deviators).
        /// Empty in steady state and in any config carrying carrier phase.
        std::map<std::string, std::set<std::string>> pivot_handover_reinflate_sats_;

        /// Huber R-inflations queued for sub-cycle phase rows whose |sres_post|
        /// exceeds phase_huber_sres_threshold.  Key "<primary_sat>|<signal>",
        /// value the target R(i,i) in m², applied at the start of the next
        /// update().  Persists across rebuild passes but is cleared in
        /// beginEpoch so each epoch decides afresh; long-term biases are covered
        /// by the separate EWMA-based inflation.
        std::map<std::string, double> pending_r_inflations_;

        /// Sat-bias resets queued by the stuck-code-bias detector.  Key
        /// "<sat>|<signal>", value the EWMA mean of post-fit residuals at
        /// trigger time, added to the current bias to snap it to the corrected
        /// estimate.  Applied right after commit in update(); beginEpoch clears
        /// it defensively.
        std::map<std::string, double> pending_sat_bias_resets_;

        /// EWMA mean/variance per (sat, signal), queried by the spatial
        /// pivot-slip detector for temporal corroboration.  No-op when
        /// estimation.residual_history.enabled is false.
        mutable ResidualHistoryTracker residual_history_;

        // Per-phase timing accumulators (microseconds; written by update()).
        long long timing_us_S_  = 0;   //!< H·P + S = HP·Hᵀ + R
        long long timing_us_K_  = 0;   //!< LDLT(S) + solve for K
        long long timing_us_P_  = 0;   //!< Joseph covariance update
        long long timing_us_Sr_ = 0;   //!< Diagonal-only post-fit covariance
        size_t    timing_n_calls_ = 0; //!< Number of update() calls accumulated.

        // --- Construction helpers ---

        /**
         * @brief Seed covariance diagonals for uninitialized core states.
         *
         * Overrides only non-finite or negative entries; zero means a
         * deliberately fixed state.
         */
        void seedCovarianceDiagonals(Eigen::MatrixXd& P, size_t state_size) const;

        /**
         * @brief Splice aiding contributions onto an Identity-seeded transition
         *        and propagate the covariance.
         *
         * The predictImu assembly as a pure static kernel: writes each
         * contribution's nominal values into @p x (skipping sentinel or
         * out-of-range indices), ASSIGNS its Phi/Q sub-block over the indices it
         * owns, then propagates P = F·P·Fᵀ + Q_full and symmetrizes.
         *
         * @param x            State vector (nominal write-backs applied in place).
         * @param P            Covariance (propagated in place).
         * @param Q_full       Baseline process noise (contribution Q assigned in place).
         * @param state_size   Full state dimension (skip guard for owned indices).
         * @param contributions Predict contributions to splice.
         */
        static void spliceAndPropagate(Eigen::VectorXd& x, Eigen::MatrixXd& P,
                                       Eigen::MatrixXd& Q_full, std::size_t state_size,
                                       const std::vector<PredictContribution>& contributions);

        // --- Outlier handling (decomposed from validatePostFitResiduals) ---

        /**
         * @brief Reset the ambiguity state(s) behind a phase outlier.
         *
         * For BSSD both the primary and reference satellite ambiguities reset.
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
         * @brief Baseline-mode (DD) variant of @ref handlePhaseOutlier.
         *
         * DD ambiguities are keyed "DD|<cons>|<sat_i>|<rov_code>|<pivot>", not
         * the PPP/BSSD "<sat>_<sig>" form.  Exactly one exists per (sat_i,
         * rov_code, pivot); the pivot has none and must not be reset.  The
         * handler parses "sat_i-pivot", strips any "/refcode" alias suffix to
         * recover the rover code, re-seeds the float ambiguity at
         * x(amb_idx) + innovation so the next innovation starts near zero, then
         * decorrelates and inflates its covariance row/col.
         *
         * @param measurement  Epoch measurement (read-only; row found by matching
         *                     @p outlier_sat and @p outlier_sig).
         * @param outlier_sat  Combined "sat_i-pivot" identifier.
         * @param outlier_sig  Signal token, possibly aliased (e.g. "L2W/L2C").
         * @param primary_sat  sat_i parsed by the caller (non-pivot).
         * @param ref_sat      Pivot satellite parsed by the caller.
         * @param x            State vector (modified at the DD index).
         * @param P            Covariance (row/col cleared, diagonal reset).
         * @param epoch_prefix Log prefix.
         */
        void handleBaselinePhaseOutlier(const EpochMeasurement& measurement,
            const std::string& outlier_sat, const std::string& outlier_sig,
            const std::string& primary_sat, const std::string& ref_sat,
            Eigen::VectorXd& x, Eigen::MatrixXd& P,
            const std::string& epoch_prefix);

        /**
         * @brief Exclude a code outlier's row from the measurement set.
         *
         * @param measurement  Epoch measurement (row removed in place).
         * @param outlier_sat  Combined satellite ID (may contain '-' for BSSD).
         * @param outlier_sig  Signal type.
         * @param max_idx      Row index of the outlier.
         * @param epoch_prefix Log prefix.
         */
        void handleCodeOutlier(EpochMeasurement& measurement,
            const std::string& outlier_sat, const std::string& outlier_sig,
            size_t max_idx, const std::string& epoch_prefix);

        /**
         * @brief Complete filter reset: zero P, re-seed core / ISB / iono /
         *        ambiguity diagonals to large-variance defaults.
         *
         * @param P            Covariance (zeroed and re-seeded in place).
         * @param epoch_prefix Log prefix.
         */
        void resetFilterCovariance(Eigen::MatrixXd& P, const std::string& epoch_prefix);

    };

} // namespace
