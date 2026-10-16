#pragma once

#include "filter/StateManager.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "filter/ResidualHistoryTracker.hpp"
#include "filter/KalmanQcCounters.hpp"
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

    namespace kalman {
        // The post-fit detector table's types (include/filter/KalmanPostFitQc.hpp).
        struct PivotDetectorSpec;
        struct PivotRowGroup;
        struct PivotRowGroups;
        struct CoherentOffset;
    }

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
     *
     * The member functions are defined in five files by concern, and each declaration below that is not in the driver names its file:
     *   - src/core/filter/KalmanFilter.cpp: the driver (construction, predict,
     *     update, the IEKF step, the pseudo-observation update, the drains and
     *     the per-epoch hooks);
     *   - src/core/filter/KalmanKernels.cpp: the algebra (the sparse-H products of
     *     include/filter/KalmanKernels.hpp, the aided-predict splice, the dense
     *     Joseph update);
     *   - src/core/filter/KalmanResidualQc.cpp: the Baarda statistics;
     *   - src/core/filter/KalmanPostFitQc.cpp: the post-fit outlier screen;
     *   - src/core/filter/KalmanOutlierResponse.cpp: the responses to an outlier
     *     and the initial covariance seed.
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
         * @brief Prediction composed from the aiding sources' contributions.
         *
         * Each contribution supplies a Phi/Q sub-block over the full-state
         * indices it owns plus an optional nominal write-back.  Blocks are
         * spliced (ASSIGNED, never accumulated) onto an Identity-seeded
         * transition and the StateManager process-noise baseline, then P is
         * propagated once.
         *
         * With no valid contribution this delegates to predict(dt), preserving
         * GNSS-only behavior byte-for-byte.  With an INS contribution the
         * assembly is the strapdown INS predict (the kernel of the retired
         * IMU-only predict): Identity-seeded F
         * (non-INS means are not propagated) and baseline Q over the
         * contribution's integrated interval rather than the GNSS dt.
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
         * prediction) and by attitude error-state feedback after GNSS updates.
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
         *
         * Defined in src/core/filter/KalmanKernels.cpp.
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
         * An ordered driver over the private stages S0 to S9 declared below,
         * which share one working value; include/filter/KalmanPostFitQc.hpp
         * lists them with the four exits.
         *
         * @return {outlier found, complete reset}.  A complete reset also sets
         *         the first flag.  On an outlier, @ref update returns
         *         UpdateResult::NeedsMeasurementRebuild.
         *
         * Defined in src/core/filter/KalmanPostFitQc.cpp.
         */
        std::pair<bool, bool> validatePostFitResiduals(EpochMeasurement& meas_update, Eigen::VectorXd& x_prev, Eigen::MatrixXd& P_prev);

        /// Per-row Baarda reliability and the variance factor of one update
        /// (the output of @ref residualQcKernel).
        struct ResidualQcStats {
            Eigen::VectorXd redundancy;  ///< rᵢ = (Q_v·P)ᵢᵢ, clamped to [0,1].
            Eigen::VectorXd wtest;       ///< Baarda wᵢ = (P·v)ᵢ / √(P·Q_v·P)ᵢᵢ.
            Eigen::VectorXd mdb;         ///< MDBᵢ = δ₀ / √(P·Q_v·P)ᵢᵢ (m); +∞ when uncheckable.
            double wssr = 0.0;           ///< vᵀ·P·v, the weighted post-fit square sum.
            double dof  = 0.0;           ///< Σ rᵢ, the redundancy of the observation group.
        };

        /**
         * @brief Pure kernel of the statistical QC: redundancy numbers, the Baarda
         *        w-test, the MDB and the variance-factor sums of one update.
         *
         * With the weight matrix P = R⁻¹ (σ₀² = 1, absolute units) and the post-fit
         * residual cofactor Q_v = Sr = R − H·P⁺·Hᵀ (Snow, Adjustment Computations,
         * ch. 9; NGS LASER Guide v2.7 slides 5-97..5-100):
         *
         *   - rᵢ  = (Q_v·P)ᵢᵢ, dof = tr(Q_v·P), wssr = vᵀ·P·v, so wssr/dof is the
         *     group's variance factor (E{vᵀPv} = tr(Q_v P));
         *   - wᵢ  = (P·v)ᵢ / √(P·Q_v·P)ᵢᵢ, standard normal under H₀ and the most
         *     powerful test of a single blunder in observation i;
         *   - MDBᵢ = δ₀ / √(P·Q_v·P)ᵢᵢ, so a blunder of that size in row i shifts
         *     the expectation of wᵢ by exactly δ₀.
         *
         * In a Kalman update P·v = S⁻¹·z and P·Q_v·P = S⁻¹ (z the innovation, S its
         * covariance), so wᵢ is also the innovation w-test (S⁻¹z)ᵢ/√(S⁻¹)ᵢᵢ.  With
         * @p Sr_full null, R is taken as diagonal and the formulas reduce to
         * rᵢ = Sr_ii/Rᵢᵢ, wᵢ = vᵢ/√Sr_ii and MDBᵢ = √Rᵢᵢ·δ₀/√rᵢ.  With a full Sr and a
         * diagonal R the two branches agree.
         *
         * Static and stateless, so it is unit-testable without a KalmanFilter
         * (tests/test_residual_qc_wtest.cpp checks it against a batch Gauss-Markov
         * oracle).
         *
         * @param v       Post-fit residuals v = z − H·x⁺ (m).
         * @param R       Working measurement-noise covariance (possibly inflated).
         * @param sr_diag Diagonal of Sr = R − H·P⁺·Hᵀ.
         * @param Sr_full Optional full Sr (m×m) for the correlation-aware form;
         *                nullptr selects the diagonal form.  Ignored, with the
         *                diagonal form used instead, when R is not positive
         *                definite.
         * @param delta_0 Baarda non-centrality (@c baardaDelta0).
         * @return The per-row statistics and the variance-factor sums.
         *
         * Defined in src/core/filter/KalmanResidualQc.cpp.
         */
        static ResidualQcStats residualQcKernel(const Eigen::VectorXd& v,
                                                const Eigen::MatrixXd& R,
                                                const Eigen::VectorXd& sr_diag,
                                                const Eigen::MatrixXd* Sr_full,
                                                double delta_0);

        /**
         * @brief Compute per-epoch statistical QC diagnostics.
         *
         * Forms the a-posteriori variance factor σ̂₀² = vᵀR⁻¹v/dof over
         * dof = Σ redundancy numbers, plus the Baarda data-snooping triple
         * (redundancy rᵢ, w-test wᵢ and MDBᵢ) by @ref residualQcKernel.  Results
         * are stored on @p meas.  Touches neither state nor covariance; the w-test
         * gates the outlier dispatch only when estimation.enable_data_snooping is on.
         *
         * @param meas    Epoch measurement; QC fields filled in place.
         * @param R       Working measurement-noise covariance (possibly inflated).
         * @param sr_diag Diagonal of Sr = R − H·P⁺·Hᵀ.
         * @param Sr_full Optional full Sr for the correlation-aware form;
         *                nullptr selects the diagonal form.
         *
         * Defined in src/core/filter/KalmanResidualQc.cpp.
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
         * @brief Log cumulative per-phase timing totals, then reset them; then
         *        log and reset the QC event counters (@ref logQcCounters).
         *
         * Each update() accumulates microseconds in the four hottest phases
         * (innovation covariance, gain, Joseph covariance, post-fit residuals).
         * Counters reset only on this call.  Called at the run end by
         * Application and the baseline driver.
         */
        void logKalmanFilterTimings();

        /**
         * @brief Log the post-fit QC event counters to main_log as one block (the
         *        rows of kKalmanQcCounterRows), then reset them.
         *
         * Called at the end of @ref logKalmanFilterTimings.  RealTimeEngine (and
         * with it the network's closed-loop consumer) calls neither, so its
         * counters are not printed yet.
         */
        void logQcCounters();

        /** @brief The post-fit QC event counters since the last print. */
        const KalmanQcCounters& getQcCounters() const { return qc_counters_; }

        /**
         * @brief Drain the constellations whose pivot needs a forced handover.
         *
         * Populated when validatePostFitResiduals sees a coherent residual
         * pattern across every SD pair against one pivot.  The Application
         * driver drains this after NeedsMeasurementRebuild and forwards it to
         * PivotManager::requestForcedHandover (EpochUpdateSupport.cpp) for the
         * next rebuild pass.  Returning by value clears the set.
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
        /// by the separate EWMA-based inflation.  Only Application::run calls
        /// beginEpoch: under RealTimeEngine (and the network consumer) and the
        /// baseline driver the queue is never cleared.
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

        /// Post-fit QC event counters (include/filter/KalmanQcCounters.hpp), each
        /// incremented where its event happens; printed and reset by logQcCounters.
        KalmanQcCounters qc_counters_;

        // --- Construction helpers ---

        /**
         * @brief Seed covariance diagonals for uninitialized core states.
         *
         * Overrides only non-finite or negative entries; zero means a
         * deliberately fixed state.
         *
         * Defined in src/core/filter/KalmanOutlierResponse.cpp.
         */
        void seedCovarianceDiagonals(Eigen::MatrixXd& P, size_t state_size) const;

        /**
         * @brief Splice aiding contributions onto an Identity-seeded transition
         *        and propagate the covariance.
         *
         * The aided-predict assembly as a pure static kernel: writes each
         * contribution's nominal values into @p x (skipping sentinel or
         * out-of-range indices), ASSIGNS its Phi/Q sub-block over the indices it
         * owns, then propagates P = F·P·Fᵀ + Q_full and symmetrizes.
         *
         * @param x            State vector (nominal write-backs applied in place).
         * @param P            Covariance (propagated in place).
         * @param Q_full       Baseline process noise (contribution Q assigned in place).
         * @param state_size   Full state dimension (skip guard for owned indices).
         * @param contributions Predict contributions to splice.
         *
         * Defined in src/core/filter/KalmanKernels.cpp.
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
         *
         * Defined in src/core/filter/KalmanOutlierResponse.cpp.
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
         *
         * Defined in src/core/filter/KalmanOutlierResponse.cpp.
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
         *
         * Defined in src/core/filter/KalmanOutlierResponse.cpp.
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
         *
         * Defined in src/core/filter/KalmanOutlierResponse.cpp.
         */
        void resetFilterCovariance(Eigen::MatrixXd& P, const std::string& epoch_prefix);

        // --- The post-fit outlier screen's stages (src/core/filter/KalmanPostFitQc.cpp) ---
        //
        // validatePostFitResiduals drives them in order over one working value; the
        // stage list and its four exits are in include/filter/KalmanPostFitQc.hpp
        //.  Each is defined in src/core/filter/KalmanPostFitQc.cpp.

        /// The values the post-fit stages share for one pass; defined in
        /// include/filter/KalmanPostFitQc.hpp.
        struct PostFitQcWork;

        /**
         * @brief S0: read the pass's thresholds, bind the residual references and count
         *        the rows.
         *
         * @param measurement The pass's measurement.
         * @param x           Pre-update state (later stages edit it in place).
         * @param P           Pre-update covariance (later stages edit it in place).
         * @return The work value the later stages share.
         */
        PostFitQcWork beginPostFitQc_(EpochMeasurement& measurement,
                                      Eigen::VectorXd& x, Eigen::MatrixXd& P);

        /**
         * @brief S1: log the per-row residual table (at trace, or at debug on a pass with
         *        an outlier), find the worst row and log the "Residual summary" line.
         *
         * @param w Work value; sets max_idx, max_sres, residual_summary_level and the
         *          formal position sigmas.
         */
        void reportPostFitResiduals_(PostFitQcWork& w);

        /**
         * @brief S2: log the variance-factor and the data-snooping summary lines, each when
         *        armed.  Diagnostics only.
         *
         * @param w Work value (read only).
         */
        void reportStatisticalQc_(PostFitQcWork& w);

        /**
         * @brief S3: the coherent pivot-offset detectors over the single-difference rows,
         *        the rows of kalman::kPivotDetectors: phase per signal, phase across
         *        bands, code per signal, code across bands.
         *
         * A firing group requests a forced pivot handover for its constellation, records
         * the satellites the handover must not elect (and, for code, the satellites whose
         * ionosphere is re-inflated after it), and excludes the group's rows from the
         * rebuild.
         *
         * @param w Work value; adds exclusions to the measurement.
         * @return True if any detector fired.
         */
        bool detectPivotOffsets_(PostFitQcWork& w);

        /**
         * @brief S3, grouping: the non-pivot single-difference rows of one observable,
         *        grouped by (pivot, full signal) and pooled by pivot across bands.
         *
         * Skips pivot rows, rows whose ATX correction was frequency-substituted
         * (@ref rowAtxSubstituted_) and, for code, combined (ionosphere-free) signals.
         *
         * @param w        Work value (read only).
         * @param obs_type 'L' for phase rows, 'C' for code rows.
         * @return The groups and the pools, each in map order.
         */
        kalman::PivotRowGroups groupPivotRows_(const PostFitQcWork& w, char obs_type) const;

        /**
         * @brief True if the ATX correction of @p signal was frequency-fallback-substituted
         *        on @p sat or on @p pivot_sat.
         *
         * The substituted PCV adds a small structural per-satellite bias that, pooled over a
         * pivot's rows, mimics a pivot slip; such rows are kept out of the groups.
         *
         * @param sat       Non-pivot satellite ID.
         * @param pivot_sat Pivot satellite ID.
         * @param signal    Signal of the row.
         * @return True if either satellite's correction was substituted.
         */
        bool rowAtxSubstituted_(const std::string& sat, const std::string& pivot_sat,
                                const std::string& signal) const;

        /**
         * @brief S3, one detector row on one group: test, report and respond.
         *
         * A pooled row skips a constellation whose handover is already requested.  A
         * coherent group (kalman::testCoherentOffset) that clears the row's wavelength
         * floor requests a forced handover for the constellation, records the replacement
         * exclusions (and, for code, the satellites to re-inflate) and excludes the
         * group's rows.
         *
         * @param w         Work value; adds exclusions to the measurement.
         * @param spec      The detector row.
         * @param pivot_sat The group's pivot.
         * @param signal    The group's signal (empty for a pool).
         * @param g         The group.
         * @return True if the group fired.
         */
        bool testPivotGroup_(PostFitQcWork& w, const kalman::PivotDetectorSpec& spec,
                             const std::string& pivot_sat, const std::string& signal,
                             const kalman::PivotRowGroup& g);

        /**
         * @brief True if a coherent group's median is below the wavelength floor
         *        (estimation.pivot_slip_min_cycles times the pivot band's wavelength) and
         *        its cluster is not tight; logs the suppression at debug.
         *
         * @param w         Work value (read only).
         * @param spec      The detector row (its tight-cluster MAD ratio).
         * @param pivot_sat The group's pivot.
         * @param signal    The group's signal; its band gives the wavelength.
         * @param t         The group's statistics.
         * @return True if the group is suppressed.
         */
        bool belowPivotWavelengthFloor_(const PostFitQcWork& w,
                                        const kalman::PivotDetectorSpec& spec,
                                        const std::string& pivot_sat,
                                        const std::string& signal,
                                        const kalman::CoherentOffset& t) const;

        /**
         * @brief The residual history's temporal confidence of a phase pivot slip, for the
         *        report line: HIGH when the fraction of warmed-up rows over the history's
         *        z threshold reaches its fraction threshold, else MEDIUM.  Diagnostic only.
         *
         * @param w Work value (read only).
         * @param g The firing group.
         * @return The tag, e.g. "MEDIUM (spatial-only)" with the history off.
         */
        std::string pivotSlipConfidence_(const PostFitQcWork& w,
                                         const kalman::PivotRowGroup& g) const;

        /**
         * @brief Log the warning of a firing group, in the format of its row (phase or
         *        code, per signal or across bands).
         *
         * @param w         Work value (read only).
         * @param spec      The detector row.
         * @param cons      The pivot's constellation name.
         * @param pivot_sat The group's pivot.
         * @param signal    The group's signal (empty for a pool).
         * @param g         The group.
         * @param t         The group's statistics.
         */
        void logPivotDetection_(const PostFitQcWork& w, const kalman::PivotDetectorSpec& spec,
                                const std::string& cons, const std::string& pivot_sat,
                                const std::string& signal, const kalman::PivotRowGroup& g,
                                const kalman::CoherentOffset& t) const;

        /**
         * @brief Record the satellites a forced handover for @p cons must not elect: the
         *        offset pivot, and every row of the group outside the robust band
         *        max(kHandoverExcludeMadK * mad, kHandoverExcludeFloorM) around the median.
         *
         * @param w         Work value (read only).
         * @param grp       The firing group.
         * @param med       Its median residual (m).
         * @param mad       Its MAD (m).
         * @param pivot_sat The offset pivot.
         * @param cons      Its constellation name.
         */
        void recordHandoverExcludes_(const PostFitQcWork& w, const kalman::PivotRowGroup& grp,
                                     double med, double mad, const std::string& pivot_sat,
                                     const std::string& cons);

        /**
         * @brief Record every non-pivot satellite of a firing code group for ionosphere
         *        re-inflation after the handover (they absorbed the pivot's code bias).
         *
         * @param w    Work value (read only).
         * @param grp  The firing group.
         * @param cons Its constellation name.
         */
        void recordIonoReinflate_(const PostFitQcWork& w, const kalman::PivotRowGroup& grp,
                                  const std::string& cons);

        /**
         * @brief S3e: a non-pivot satellite whose phase rows on two or more bands carry
         *        the same non-zero integer number of cycles: its ambiguities are marked for
         *        re-seed with their covariance re-inflated, and its rows are excluded.
         *
         * @param w Work value; edits P and the measurement's exclusions.
         * @return True if any satellite was reset.
         */
        bool detectNonPivotSlips_(PostFitQcWork& w);

        /**
         * @brief E2: a group detector rejected rows; count one bad residual and request a
         *        rebuild.  Returns before the complete-reset backstop.
         *
         * @param w Work value.
         * @return {true, false}.
         */
        std::pair<bool, bool> finishGroupRejection_(PostFitQcWork& w);

        /**
         * @brief S4: the Huber pass: queue an R target for every sub-cycle phase row whose
         *        |standardized residual| exceeds the Huber threshold (the post-fit IRLS
         *        scheme, or the innovation one-shot scheme when armed).
         *
         * @param w Work value (read only); writes pending_r_inflations_.
         * @return True if a target was queued or raised.
         */
        bool queueHuberInflations_(PostFitQcWork& w);

        /**
         * @brief E3: Huber targets were queued; request a rebuild.  Not counted as a bad
         *        residual, and returns before the complete-reset backstop.
         *
         * @param w Work value.
         * @return {true, false}.
         */
        std::pair<bool, bool> finishHuberRebuild_(PostFitQcWork& w);

        /**
         * @brief S5: choose the gate: the largest |standardized residual| against the
         *        threshold or, with data snooping armed, the largest |w| over the checkable
         *        rows against the critical value.
         *
         * @param w Work value; sets gate_stat, gate_idx and gate_crit.
         */
        void selectGateRow_(PostFitQcWork& w);

        /**
         * @brief S6: when the gate fires, respond to the gated row (the phase response, or
         *        the code row's exclusion) and count one bad residual.
         *
         * @param w Work value; sets is_outlier_found.
         */
        void dispatchWorstRow_(PostFitQcWork& w);

        /**
         * @brief S6a: the MW tiebreak.  Among the gated constellation's phase rows over the
         *        threshold and within 5 % of the largest |standardized residual|, choose the
         *        satellite with the largest MW deviation score.  Only with a
         *        CycleSlipManager supplied (setCycleSlipManager) and a phase worst row.
         *
         * @param w Work value (read only).
         * @return The row to dispatch: gate_idx, or the MW-chosen row.
         */
        size_t chooseOutlierRow_(PostFitQcWork& w);

        /**
         * @brief S6b: the phase-row response: under the sub-cycle floor the row is only
         *        excluded and the ambiguity kept, else the ambiguity is reset; then the same
         *        for the satellite's other phase rows over the threshold.
         *
         * @param w           Work value; edits x, P and the measurement's exclusions.
         * @param chosen_idx  The row being dispatched.
         * @param outlier_sat Its satellite ID (the pair "G01-G05" under BSSD).
         * @param outlier_sig Its signal.
         * @param outlier_res Its post-fit residual (m), captured before the dispatch.
         */
        void respondToPhaseOutlier_(PostFitQcWork& w, size_t chosen_idx,
                                    const std::string& outlier_sat,
                                    const std::string& outlier_sig, double outlier_res);

        /**
         * @brief S7: the complete reset, when the epoch's bad-residual count exceeds the
         *        configured fraction of the rows the epoch started with.
         *
         * @param w Work value; may reset P; sets is_outlier_found and is_complete_reset.
         */
        void completeResetBackstop_(PostFitQcWork& w);

        /**
         * @brief S8: log the pass's verdict line (debug).
         *
         * @param w Work value (read only).
         */
        void logPostFitResult_(PostFitQcWork& w);

        /**
         * @brief S9: on a clean pass with residual history armed, feed the accepted
         *        residuals to the history, run the stuck-code-bias scan and log the
         *        history digest.
         *
         * @param w Work value (read only).
         */
        void recordCleanCommit_(PostFitQcWork& w);

        /**
         * @brief S9a: with estimation.stuck_code_bias and satellite biases armed, queue a
         *        reset for every non-pivot satellite bias the history reports stuck (a
         *        rejection streak or a drifted mean); update() applies them after the
         *        commit.
         *
         * @param w Work value (read only); writes pending_sat_bias_resets_.
         */
        void queueStuckCodeBiasResets_(PostFitQcWork& w);

    };

} // namespace
