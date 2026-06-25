#pragma once




#pragma once

#include "filter/StateManager.hpp"
#include "model/PppModel.hpp"
#include "config/GnssConfig.hpp"
#include "common/Constants.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/PivotManager.hpp"
#include "filter/DdPivotManager.hpp"
#include "measurement/StateInitializer.hpp"
#include "measurement/BssdTransform.hpp"
#include "measurement/MeasurementBuilder.hpp"
#include "aiding/AidingTypes.hpp"
#include <Eigen/Dense>
#include <memory>
#include <vector>
#include <string>
#include <set>

namespace GnssProcessor {

    /**
     * @file MeasurementModel.hpp
     * @brief Builds measurement vectors, design matrices and covariance used by the Kalman filter.
     *
     * The MeasurementModel is responsible for converting a set of PPP observations
     * (code and phase) together with the current PPP state into an @c EpochMeasurement
     * suitable for Kalman filter processing. It queries the high-level @c PppModel for
     * corrected modeled values and diagnostics, applies observation selection and masking,
     * assembles the innovation vector, design matrix H and measurement noise R, and provides
     * validation helpers for outlier and quality checks.
     *
     * Heavy-lifting sub-tasks are delegated to focused components:
     *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
     *  - PivotManager:  pivot selection, handover, and state re-referencing.
     *  - BssdTransform:     between-satellite single-difference S-transformation.
     */
    class MeasurementModel {
    public:
        /**
         * @brief Construct a MeasurementModel.
         *
         * @param state_manager Reference to the Kalman filter state manager (owned externally).
         * @param ppp_model     Const reference to the PPP model that computes corrections and modeled observables.
         * @param config        Const reference to processing configuration.
         * @param log_manager   Reference to the logging manager used for diagnostics.
         */
        MeasurementModel(StateManager& state_manager,
            const PppModel& ppp_model,
            const GnssConfig& config,
            LogManager& log_manager);

        /**
         * @brief Compute the measurement update for a single epoch.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements = {});

        /**
         * @brief Compute a DD measurement update from two per-receiver
         *        PppModel outputs (baseline mode).
         *
         * Used when @c config.isBaselineMode() is true.  Intersects the
         * satellites observed at both receivers, picks per-constellation
         * pivots (sticky hysteresis across epochs), and forms double-
         * differenced innovations + full R matrix from the per-receiver
         * residuals and variances supplied by @c PppModel::computeModels.
         *
         * The DD construction reuses the EXISTING per-receiver correction
         * stack (satellite clock, emission time, Sagnac, Shapiro, solid
         * tide, antenna PCO/PCV, stochastic model), so the baseline
         * measurement is held to the same quality standards as PPP.
         *
         * DD phase ambiguity states ("DD|cons|sat_i|code|pivot") are
         * added to @c state_manager_ lazily as new phase rows appear,
         * and removed when their pivot retires.
         *
         * @param rov_modeled  Rover PppModel output (one PppObservation per satellite).
         * @param ref_modeled  Reference PppModel output.
         * @param current_state Current filter state vector (for amb subtraction on phase rows).
         * @param excluded_measurements Row signatures (format
         *        @c "sat_i_<signal_token>", where @c signal_token matches
         *        the format written to @c EpochMeasurement::signal_types
         *        — @c rov_code when rov/ref agree, else @c rov/ref) to
         *        skip during row construction.  Carried across rebuild
         *        passes by the caller so a row flagged on pass N stays
         *        out on pass N+1.  Default empty for first-pass calls.
         * @return EpochMeasurement ready for KalmanFilter::update().  Empty
         *         (innovations.size()==0) when insufficient DDs could be formed.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(
            const std::vector<PppObservation>& rov_modeled,
            const std::vector<PppObservation>& ref_modeled,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements = {});

        /**
         * @brief Forget DD pivot bookkeeping.  Call on baseline-mode filter
         *        reset.  No-op for PPP / PPP-RTK.  Forwards to
         *        @c DdPivotManager::reset() — the sticky map and handover
         *        machinery now live on the dedicated manager class.
         */
        void resetBaselinePivots() { dd_pivot_manager_.reset(); }

        /**
         * @brief Force a DD pivot re-rank for one constellation (KF slip).
         *
         * Routed from the baseline driver when @c KalmanFilter's coherent
         * pivot-slip detector flags @p cons.  The next @c buildDdMeasurement
         * re-ranks that constellation's pivot, excluding @p exclude_sats (the
         * offset pivot plus any co-disturbed satellites) so the new pivot is
         * clean; existing DD ambiguities are S-transformed onto it by
         * @c DdPivotManager (integer cycles preserved).  No-op for PPP / PPP-RTK.
         */
        void requestBaselinePivotRerank(const std::string& cons,
                                        const std::set<std::string>& exclude_sats) {
            dd_pivot_manager_.requestForcedRerank(cons, exclude_sats);
        }

        /**
         * @brief DD ambiguity keys whose float will not round to integer.
         *
         * A DD ambiguity is integer-cycle only when all four observations
         * (rov_i, rov_p, ref_i, ref_p) use the same RINEX code; that's
         * the configuration in which both the receiver phase bias and
         * the satellite phase bias terms cancel pairwise.  Any other
         * combination — rover/reference mismatch on a row, OR rover
         * code switching between sat_i and pivot, OR reference code
         * switching — leaves a residual IFB / DCB term that the float
         * ambiguity absorbs as a non-integer offset.
         *
         * Populated each epoch by the DD overload of
         * @c computeMeasurementUpdate.  The set is rebuilt from scratch
         * every call; do not cache it across calls.  Consumed by
         * @c AmbiguityResolver::resolveDd to exclude these keys from
         * the integer-fixing candidate set.
         *
         * @return Const reference to the set of DD ambiguity keys that
         *         must remain float (key format
         *         @c "DD|cons|sat_i|rov_code|pivot").
         */
        const std::set<std::string>& getNonIntegerCapableDdAmbiguityKeys() const {
            return non_integer_capable_dd_ambiguity_keys_;
        }

        /**
         * @brief Validate an @c EpochMeasurement before Kalman processing.
         */
        bool validateMeasurementUpdate(const EpochMeasurement& measurement) const;

        /**
         * @brief Set the body-to-ECEF attitude matrix for tightly-coupled Jacobians.
         *
         * @param C_b_e Current body-to-ECEF rotation matrix.
         * @param f_b_corrected Bias-corrected specific force in body frame (m/s�).
         */
        void setImuState(const Eigen::Matrix3d& C_b_e,
                         const Eigen::Vector3d& f_b_corrected);

        /**
         * @brief Append one independent aiding measurement row (ZUPT/baro/...).
         *
         * Grows the finalized @p update's innovation/R_v/H by one row and
         * push-backs the row's @c label / @c signal into satellite_ids /
         * signal_types so the dimension invariant holds and KF outlier keys
         * never collide with GNSS rows.  The row's jacobian must be over the
         * FULL state (size == H columns / state size).
         *
         * HARD-ERRORS (throws) if @p update.R_full is populated (BSSD/DD active):
         * a diagonal append after R_full is set would silently drop the SD
         * cross-covariance via the KF's R_v fallback.  Row-contributing aiding
         * sources are therefore restricted to undifferenced PPP / loosely-coupled
         * GNSS until a block-diagonal R_full augmentation is added.
         *
         * @param update Finalized epoch measurement (grown in place).
         * @param row    Aiding row (jacobian over full state, innovation, variance, label).
         */
        static void appendAidingRow(EpochMeasurement& update,
                                    const AidingMeasurementRow& row);

        /**
         * @brief Register ambiguity keys that must survive the per-epoch
         *        prune even when their signal produced no observation.
         *
         * @c ensureRequiredStates deletes any ambiguity state whose key is
         * absent from this epoch's observations (prune-on-first-miss).  A
         * phase ambiguity HELD across a short tracking outage by the
         * soft-LLI / gap bridge (@c SoftLliBridge) pending a TurboEdit
         * @c snapshot + dN·λ repair must be exempt: deleting it (and
         * reseeding from the post-gap — possibly slipped — carrier on
         * return) destroys exactly the pre-gap state the repair restores.
         * The protected keys are unioned into the prune keep-set, and their
         * satellites into the ionosphere-state keep-set (uncombined mode),
         * each epoch until the hold resolves.
         *
         * The caller refreshes this set every epoch from
         * @c SoftLliBridge::heldKeys() — the same source that seeds the KF
         * measurement exclusion — so protection and exclusion can never
         * diverge.  An empty set (the default, and whenever nothing is
         * held) restores the plain prune behavior.
         *
         * @param keys Ambiguity keys (e.g. @c "G03_L2W") to protect.
         */
        void setProtectedAmbiguityKeys(std::set<std::string> keys) {
            protected_ambiguity_keys_ = std::move(keys);
        }

        /**
         * @brief Get the current BSSD pivot satellite for each constellation.
         */
        const std::map<std::string, std::string>& getBssdPivotSatellites() const {
            return pivot_manager_.getPivotSatellites();
        }

        /// Access the pivot manager (for external components that need pivot info).
        PivotManager& getPivotManager() { return pivot_manager_; }
        const PivotManager& getPivotManager() const { return pivot_manager_; }

        /// Access the state initializer (for shared helpers like resolveReceiverBias).
        StateInitializer& getStateInitializer() { return state_initializer_; }
        const StateInitializer& getStateInitializer() const { return state_initializer_; }

        /// Per-satellite NL reference band map for the
        /// @c StateAmbiguityBasis::NlWl path.  Used by @c BssdTransform to
        /// look up the pivot's NL/WL state columns when building the SD
        /// S-transform.  Empty when basis == PerBand.
        const std::map<std::string, char>& getNlWlReferenceBands() const {
            return nlwl_ref_band_;
        }

    private:
        // -------------------------------------------------------------------
        // Core dependencies (non-owning references)
        // -------------------------------------------------------------------

        StateManager& state_manager_;
        const PppModel& ppp_model_;
        const GnssConfig& config_;
        LogManager& log_manager_;

        // -------------------------------------------------------------------
        // Delegate components
        // -------------------------------------------------------------------

        /// First-epoch state seeding (ISB, phase clock, DCB/IFB).
        StateInitializer state_initializer_;

        /// BSSD pivot satellite manager (selection, handover, re-referencing).
        PivotManager pivot_manager_;

        /// BSSD S-transformation (differencing operator).  Used by both
        /// PPP-RTK (apply() in-place) and baseline DD (differenceBrsd()
        /// static method consumed by the row builder).
        BssdTransform bssd_transform_;

        /// Baseline-mode pivot manager.  Owns the per-constellation
        /// sticky map, performs sticky-or-rerank selection each epoch,
        /// and applies the DD pivot-handover S-transform on changes.
        /// The SD path's @c PivotManager is the symmetric counterpart
        /// for PPP-RTK.
        DdPivotManager dd_pivot_manager_;
        /// DD ambiguity keys (format "DD|cons|sat_i|rov_code|pivot") for
        /// rows that fail strict four-way RINEX-code equality across the
        /// four observations in the DD.  Those rows carry residual
        /// receiver IFB and/or satellite DCB terms that prevent the
        /// float from being integer-cycle.  Rebuilt every DD epoch from
        /// the current row set so the snapshot is always consistent
        /// with the active state.  Consumed by AmbiguityResolver::resolveDd
        /// to keep these ambiguities out of the integer-fixing candidate
        /// set.
        std::set<std::string> non_integer_capable_dd_ambiguity_keys_;

        /// Ambiguity keys exempt from the @c ensureRequiredStates prune
        /// while their signal is in a tracking outage — held by the
        /// soft-LLI / gap bridge pending a TurboEdit integer repair.
        /// Refreshed each epoch via @c setProtectedAmbiguityKeys; empty
        /// unless something is held.
        std::set<std::string> protected_ambiguity_keys_;

        /// Measurement-differencing strategy (None / BetweenSatellite /
        /// DoubleDifference), selected once from config via
        /// @c MeasurementBuilderFactory.  Drives the mode-specific pre/post
        /// passes through polymorphic hooks instead of inline config-flag
        /// branches.  The strategies call back into the (private) building
        /// blocks of this class, so they are declared @c friend below.
        std::unique_ptr<MeasurementDifferencingStrategy> differencing_;

        friend class UndifferencedMeasurementBuilder;
        friend class BssdMeasurementBuilder;
        friend class DdMeasurementBuilder;

        // -------------------------------------------------------------------
        // Per-instance helpers
        // -------------------------------------------------------------------

        PppUtils ppp_utils_;  ///< Frequency/wavelength helpers.

        /// Epoch at which the BSSD pivot was last selected.  Used to
        /// prevent re-running pivot handover during measurement rebuilds
        /// within the same epoch (outlier-triggered rebuilds must reuse
        /// the original pivot to avoid cascading SD-ambiguity invalidation).
        GnssTime last_pivot_epoch_{};

        /// Per-constellation set of full RINEX-3 signal codes covered by the
        /// BSSD pivot.  Populated at the start of each computeMeasurementUpdate()
        /// call from the pivot satellite's observed signals.
        ///
        /// Per-signal (not per-band) granularity is required by the BSSD
        /// per-signal grouping in @c BssdTransform: a non-pivot's L1C is
        /// only SD-eligible if the pivot ALSO tracks L1C, even when the
        /// pivot tracks L1W on the same band.  A band-level filter would
        /// admit the non-pivot's L1C, leave it singleton at SD time, and
        /// strand the ambiguity state with no measurement update.
        ///
        /// Key: constellation ("GPS"), Value: set of signals
        /// ({"L1W","L1C","C1W","C1C","L2W","C2W",...}).
        mutable std::map<std::string, std::set<std::string>> bssd_pivot_signals_;

        /// Per-satellite NL reference band for the @c StateAmbiguityBasis::NlWl
        /// path.  Populated by @c ensureRequiredStates each epoch and consumed
        /// by @c buildJacobianRow (and by @c BssdTransform via a getter) to
        /// resolve which NL state a satellite's phase observation projects
        /// onto.  Frozen for the lifetime of the satellite's arc — once a
        /// reference band is chosen it is not migrated.  Empty when basis ==
        /// PerBand.  Key: satellite ID; Value: RINEX band digit (e.g. '1').
        std::map<std::string, char> nlwl_ref_band_;

        // Tightly-coupled IMU state (set per-epoch via setImuState())
        Eigen::Matrix3d tc_C_b_e_ = Eigen::Matrix3d::Identity();
        Eigen::Vector3d tc_f_b_ = Eigen::Vector3d::Zero();

        // -------------------------------------------------------------------
        // Product coverage checks
        // -------------------------------------------------------------------

        /**
         * @brief Check whether both component signals of an IF combination
         *        have BIA product coverage.
         */
        bool ifCombHasFullBia(const std::string& satellite,
                              const std::string& if_key) const;

        /**
         * @brief Whether the live SSR stream supplies a code bias for this exact
         *        (satellite, raw signal) — i.e. whether @c ssr_bias_term will
         *        correct the row in @c buildJacobianRow.
         *
         * Mirrors the apply-site activation exactly: false in the network
         * producer mode (which ESTIMATES satellite biases as states rather than
         * importing them), and a pure presence check via
         * @c SsrCorrectionStore::hasCodeBias otherwise.  Used to suppress
         * redundant per-(satellite, signal) sat-bias STATE allocation for
         * signals the stream already corrects.  Post-processing leaves the SSR
         * store empty, so this is always false there and the state layout is
         * byte-identical.
         *
         * @note Raw signals only.  IF-combined rows (key contains '_') get
         *       @c ssr_bias_term = 0 (SSR code biases are per raw signal), so
         *       their sat-bias state stays the only correction and must NOT be
         *       suppressed — pass raw signal keys here.
         */
        bool ssrCoversCodeSignal(const std::string& satellite,
                                 const std::string& signal) const;

        /**
         * @brief Check whether both component signals of an IF combination
         *        have receiver antenna ATX coverage (PCO/PCV).
         */
        bool ifCombHasFullRxAtx(const std::string& if_key, System system) const;

        // -------------------------------------------------------------------
        // State pre-pass
        // -------------------------------------------------------------------

        /**
         * @brief Ensure that all ambiguity, ionosphere, and code-bias states
         *        required by the current epoch exist in the StateManager.
         *
         * Also prunes stale states that are no longer needed.
         */
        void ensureRequiredStates(const std::vector<PppObservation>& observations,
            const std::string& epoch_prefix);

        // -------------------------------------------------------------------
        // Measurement filtering
        // -------------------------------------------------------------------

        /**
         * @brief Determine whether a given observation/ref_type pair is usable.
         */
        bool isUsableMeasurement(const PppObservation& obs,
            const std::string& ref_type,
            const std::set<std::string>& excluded,
            const std::string& epoch_prefix) const;

        // -------------------------------------------------------------------
        // Jacobian + innovation assembly
        // -------------------------------------------------------------------

        /**
         * @brief Build one Jacobian row and compute the predicted observable.
         */
        bool buildJacobianRow(const PppObservation& obs,
            const std::string& ref_type,
            const Eigen::VectorXd& x,
            size_t state_size,
            Eigen::VectorXd& jacobian,
            double& predicted) const;

        // -------------------------------------------------------------------
        // BSSD delegation
        // -------------------------------------------------------------------

        void handleBssdPivotHandover(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix,
                                      bool forced_only = false);

        void freezePivotAmbiguityCovariance();

        void freezePivotIonoAndBiasCovariance();

        // -------------------------------------------------------------------
        // Double-difference (baseline) assembly
        // -------------------------------------------------------------------

        /**
         * @brief Assemble the double-differenced measurement (baseline mode).
         *
         * Builds DD rows: @c BrsdTransform::apply per shared (sat, band,
         * obs_type) → @c BssdTransform::differenceBrsd over the
         * @c DdPivotManager pivots.  Invoked via
         * @c DdMeasurementBuilder::buildDoubleDifference; the public two-vector
         * @c computeMeasurementUpdate delegates here through the differencing
         * strategy.  See that overload's documentation for parameter/return
         * semantics.
         */
        EpochMeasurement buildDdMeasurement(
            const std::vector<PppObservation>& rov_modeled,
            const std::vector<PppObservation>& ref_modeled,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements);

        // DD pivot selection and S-transform handover live on
        // @c DdPivotManager (member @c dd_pivot_manager_).  See its
        // header for the API.  This class no longer owns the sticky
        // map or the handover logic — kept purely as orchestrator.
    };

} // namespace GnssProcessor