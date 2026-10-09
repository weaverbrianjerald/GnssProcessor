#pragma once




#pragma once

#include "filter/StateManager.hpp"
#include "filter/NetworkParamInventory.hpp"   // StationAbsorbed (the network's withheld shared states)
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
#include <map>

namespace GnssProcessor {

    class SsrCorrectionStore;

    /**
     * @file MeasurementModel.hpp
     * @brief Builds measurement vectors, design matrices and covariance used by the Kalman filter.
     *
     * Converts PPP code and phase observations plus the current PPP state into an
     * @c EpochMeasurement.  Queries @c PppModel for corrected modeled values,
     * applies observation selection and masking, assembles the innovation vector,
     * design matrix H and noise R, and offers outlier/quality validation helpers.
     *
     * Heavy sub-tasks are delegated:
     *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
     *  - PivotManager:  pivot selection, handover, and state re-referencing.
     *  - BssdTransform:     between-satellite single-difference S-transformation.
     */
    class MeasurementModel {
    public:
        /**
         * @brief Construct a MeasurementModel.
         *
         * @param state_manager Kalman filter state manager (owned externally).
         * @param ppp_model     Computes corrections and modeled observables.
         * @param config        Processing configuration.
         * @param log_manager   Diagnostic logging.
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
         * @brief Pre-allocate the shared satellite states owned by the PPP-RTK
         *        network producer's joint estimator (per-satellite clock,
         *        per-(satellite, signal) phase bias and code bias), so this
         *        station's @c H carries their columns this epoch.
         *
         * No-op unless @c ProcessingMode::PPP_RTK_Network and uncombined.  These
         * state families are consume-only in @c buildJacobianRow (the network
         * estimator owns their creation), so without this pre-pass their Jacobian
         * columns would be all zero and the satellites would receive no information
         * from this station.  Each family is allocated only when its estimation
         * toggle is on.  GLONASS is skipped unless the GLONASS network scope is
         * enabled, and then receives a clock column only (no shared phase or code
         * bias under FDMA).
         *
         * The admission gate mirrors the row-assembly loop in
         * @c computeMeasurementUpdate (obs-level geometry and elevation checks plus
         * @c isUsableMeasurement), so the allocated column set is exactly the set
         * @c buildJacobianRow writes and never a superset.  An over-allocated
         * all-zero column would be an unobserved gauge that the null-space rank
         * gate rejects.  States no longer required are pruned, so a dropped
         * satellite or signal leaves no stale column.
         *
         * Every shared state a gate here withholds from the station (GLONASS
         * code-only scope, unmatched antenna, coverage allowlist, pivot allowlist,
         * CB-ADM co-gate) is noted in @ref absorbedSharedStates, which this call
         * clears first: the parameter inventory counts each as an absorbed instance
         * (card NOM N1b, filter/NetworkParamInventory.hpp).
         *
         * @param observations This station's modeled observations for the epoch.
         * @param allowed_phase_keys If non-empty, restrict phase-bias allocation to
         *        these @c "<sat>|<signal>|PB" keys: the datum (pivot) station's
         *        set, so every shared phase-bias column has a pivot-receiver
         *        ambiguity pin fixing its gauge.  Empty allocates for every usable
         *        raw phase signal, which is how the pivot set is seeded.
         * @param cbadm_admitted_code_keys If non-null, a phase bias is allocated
         *        only when its companion code key (@c "<sat>|C<band><attr>") is in
         *        this set, so a signal whose code pair the CB-ADM census has
         *        quarantined also has its shared phase bias withheld.  Null
         *        (feature off or census inactive) is byte-identical.
         */
        void ensureNetworkSharedStates(const std::vector<PppObservation>& observations,
                                       const std::set<std::string>& allowed_phase_keys = {},
                                       const std::set<std::string>* cbadm_admitted_code_keys = nullptr);

        /**
         * @brief The shared satellite states withheld from this station this epoch
         *        (network producer only; card NOM N1b).
         *
         * Cleared and refilled by @ref ensureNetworkSharedStates in PASS A, then
         * extended by the serial gates that prune a state the station had allocated
         * (@ref noteAbsorbedSharedState).  Read by @c recordStationParams after the
         * station's split.  Empty in every other mode: nothing writes it there.
         *
         * @return The station's list for the epoch.
         */
        const StationAbsorbed& absorbedSharedStates() const { return absorbed_shared_; }

        /**
         * @brief Note a shared satellite state a network gate withheld from this
         *        station after allocation (the anchored-union coverage gate, the eq34
         *        hygiene block): appended to @ref absorbedSharedStates.
         *
         * @param container @c StateContainer::SatPhaseBiasStates or @c SatBiasStates.
         * @param key       The state's key in the station's state manager.
         * @param reason    The gate.
         */
        void noteAbsorbedSharedState(StateContainer container, std::string_view key,
                                     AbsorbedReason reason) {
            noteAbsorbed(absorbed_shared_, container, key, reason);
        }

        /**
         * @brief Install a network coverage allowlist for satellite phase-bias states.
         *
         * System display name (e.g. "GPS") -> the phase signals permitted a
         * per-(sat,signal) phase-bias state.  An absent constellation or empty map
         * permits every phase signal (the byte-identical default).  Where a
         * constellation is listed, an unlisted signal is skipped in
         * @ref ensureNetworkSharedStates: no phase-bias state is allocated, so its
         * float ambiguity absorbs the bias and the signal is not AR-eligible.  The
         * network producer populates this from per-station coverage
         * (network.phase_bias_min_station_fraction).
         */
        void setPhaseBiasSignalAllowlist(std::map<std::string, std::set<std::string>> allowlist) {
            phase_bias_signal_allowlist_ = std::move(allowlist);
        }

        /**
         * @brief Withhold all of this station's satellite phase-bias contributions.
         *
         * The per-station analogue of @ref setPhaseBiasSignalAllowlist.  When true,
         * @ref ensureNetworkSharedStates allocates no phase-bias state for this
         * station, so its carrier phase writes no shared phase-bias Jacobian,
         * scatters no phase-bias column into the border, and the station cannot be
         * elected a phase-bias anchor.  The observations are still admitted: the
         * station still feeds the satellite clock, code bias and ionosphere (its
         * position is held at the roster coordinate in the network, never
         * estimated), and only the shared phase-bias coupling is removed, with the
         * local float ambiguity absorbing each arc.  Each withheld state is noted in
         * @ref absorbedSharedStates (reason unmatched_antenna).
         *
         * The network producer uses this to keep a station with an unmatched
         * receiver antenna (no receiver PCV, so uncalibrated phase) out of the
         * emitted satellite phase biases.  Default false: byte-identical.
         */
        void setPhaseBiasEmissionExcluded(bool excluded) {
            phase_bias_emission_excluded_ = excluded;
        }

        /**
         * @brief Seed the model's frequency helper with GLONASS FCN slot numbers.
         *
         * GLONASS is FDMA, so the carrier frequency, and with it the iono
         * coefficient and wavelength @c buildJacobianRow uses, depends on each
         * satellite's channel.  This model's private @c PppUtils is constructed
         * un-seeded, so any caller processing GLONASS uncombined (the network
         * driver, per station) must seed it from the station's RINEX
         * "GLONASS SLOT / FRQ #" header; otherwise the Jacobian falls back to the
         * default table instead of the actual channel.
         *
         * @param slots satellite-id -> FCN channel number.
         */
        void setGlonassSlots(const std::map<std::string, int>& slots);

        /**
         * @brief Carrier frequency (Hz) of an observation code on a satellite.
         *
         * Exposes the private @c PppUtils lookup so the network driver can form the
         * iono-free (clock-preserving) combination weights for the satellite
         * code-bias IF datum pin: the OSB's GF/DCB part is emitted while its IF
         * part lives in the estimable clock.  GLONASS uses the seeded FCN slots
         * (@ref setGlonassSlots); GPS/Galileo are per-signal constants.  Returns 0
         * for an unknown code/satellite.
         */
        double carrierFrequency(const std::string& obs_code,
                                const std::string& satellite) const;

        /**
         * @brief Compute a DD measurement update from two per-receiver
         *        PppModel outputs (baseline mode).
         *
         * Used when @c config.isBaselineMode() is true.  Intersects the satellites
         * observed at both receivers, picks per-constellation pivots with sticky
         * hysteresis across epochs, and forms double-differenced innovations plus a
         * full R matrix from the per-receiver residuals and variances.
         *
         * The construction reuses the existing per-receiver correction stack
         * (satellite clock, emission time, Sagnac, Shapiro, solid tide, antenna
         * PCO/PCV, stochastic model), holding the baseline measurement to the same
         * standard as PPP.  DD phase ambiguity states
         * ("DD|cons|sat_i|code|pivot") are added lazily as phase rows appear and
         * removed when their pivot retires.
         *
         * @param rov_modeled  Rover output, one PppObservation per satellite.
         * @param ref_modeled  Reference output.
         * @param current_state Filter state (for amb subtraction on phase rows).
         * @param excluded_measurements Row signatures to skip, formatted
         *        @c "sat_i_<signal_token>" where the token matches
         *        @c EpochMeasurement::signal_types (@c rov_code when rov/ref agree,
         *        else @c rov/ref).  The caller carries these across rebuild passes
         *        so a row flagged on pass N stays out on pass N+1.
         * @return EpochMeasurement ready for KalmanFilter::update(); empty when too
         *         few DDs could be formed.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(
            const std::vector<PppObservation>& rov_modeled,
            const std::vector<PppObservation>& ref_modeled,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements = {});

        /**
         * @brief Forget DD pivot bookkeeping.  Call on baseline-mode filter
         *        reset.  No-op for PPP / PPP-RTK.  Forwards to
         *        @c DdPivotManager::reset(), which owns the sticky map and the
         *        handover logic.
         */
        void resetBaselinePivots() { dd_pivot_manager_.reset(); }

        /**
         * @brief Force a DD pivot re-rank for one constellation (KF slip).
         *
         * Called by the baseline driver when the coherent pivot-slip detector in
         * @c KalmanFilter flags @p cons.  The next @c buildDdMeasurement re-ranks
         * that constellation's pivot, excluding @p exclude_sats (the offset pivot
         * plus any co-disturbed satellites) so the new pivot is clean; existing DD
         * ambiguities are S-transformed onto it by @c DdPivotManager (integer
         * cycles preserved).  No-op for PPP / PPP-RTK.
         */
        void requestBaselinePivotRerank(const std::string& cons,
                                        const std::set<std::string>& exclude_sats) {
            dd_pivot_manager_.requestForcedRerank(cons, exclude_sats);
        }

        /**
         * @brief DD ambiguity keys whose float will not round to integer.
         *
         * A DD ambiguity is integer-cycle only when all four observations
         * (rov_i, rov_p, ref_i, ref_p) use the same RINEX code; in that
         * configuration both the receiver phase bias and the satellite phase
         * bias terms cancel pairwise.  Any other combination (rover/reference
         * mismatch on a row, rover code switching between sat_i and pivot, or
         * reference code switching) leaves a residual IFB / DCB term that the
         * float ambiguity absorbs as a non-integer offset.
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
         * @param f_b_corrected Bias-corrected specific force in body frame (m/s²).
         */
        void setImuState(const Eigen::Matrix3d& C_b_e,
                         const Eigen::Vector3d& f_b_corrected);

        /**
         * @brief Append one independent aiding measurement row (ZUPT/baro/...).
         *
         * Grows the finalized @p update's innovation/R_v/H by one row and
         * appends the row's @c label / @c signal to satellite_ids /
         * signal_types, so the dimension invariant holds and KF outlier keys
         * never collide with GNSS rows.  The row's jacobian must span the full
         * state (size == H columns); a size mismatch throws
         * @c std::invalid_argument.  When H has no columns yet, the row's size
         * sets the width.
         *
         * Throws @c std::runtime_error if @p update.R_full is populated (BSSD/DD
         * active): a diagonal append after R_full is set would silently drop the
         * SD cross-covariance via the KF's R_v fallback.  Row-contributing aiding
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
         * absent from this epoch's observations (prune on first miss).  A
         * phase ambiguity held across a short tracking outage by the
         * soft-LLI / gap bridge (@c SoftLliBridge), pending a TurboEdit
         * @c snapshot + dN·λ repair, must be exempt: deleting it and reseeding
         * from the post-gap (possibly slipped) carrier on return would discard
         * the pre-gap state the repair restores.  The protected keys are
         * unioned into the prune keep-set, and their satellites into the
         * ionosphere-state keep-set (uncombined mode), each epoch until the
         * hold resolves.
         *
         * The caller refreshes this set every epoch from
         * @c SoftLliBridge::heldKeys(), the same source that seeds the KF
         * measurement exclusion, so protection and exclusion stay consistent.
         * An empty set (the default, and whenever nothing is held) gives the
         * plain prune behavior.
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

        /**
         * @brief Inject a non-owning SSR-correction store override (DI seam for
         *        the PPP-RTK closed-loop harness).
         *
         * The SSR satellite code/phase-bias term in @c buildJacobianRow and the
         * @c ssrCoversCodeSignal presence check default to the process-wide
         * @c SsrCorrectionStore singleton (empty in post-processing, so no SSR is
         * applied).  The network closed-loop consumer points this at the store
         * owned by the producer, so a held-out station consumes the network's
         * freshly emitted satellite phase bias.  @c nullptr (the default)
         * restores the singleton; every non-harness caller is byte-identical.
         *
         * @param store Non-owning store pointer (must outlive this object) or
         *        @c nullptr for the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) { ssr_store_override_ = store; }

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

        /// Network phase-bias coverage allowlist (see @ref setPhaseBiasSignalAllowlist).
        /// system → admitted phase signals.  Empty (default) = allow every signal.
        std::map<std::string, std::set<std::string>> phase_bias_signal_allowlist_;

        /// Per-station phase-bias emission exclusion (see @ref setPhaseBiasEmissionExcluded).
        /// True = allocate no satellite phase-bias state for this station's observations
        /// (all signals).  False (default) = contribute normally; byte-identical.
        bool phase_bias_emission_excluded_{false};

        /// The shared states withheld from this station this epoch (see
        /// @ref absorbedSharedStates).  Network producer only.
        StationAbsorbed absorbed_shared_;

        /// Whether @p signal (a raw phase code, e.g. "L1C") for @p system may get a
        /// satellite phase-bias state under the coverage allowlist.  True when the
        /// feature is off (empty) or the system is not gated; else set membership.
        bool phaseBiasAdmitted(const std::string& system, const std::string& signal) const {
            if (phase_bias_signal_allowlist_.empty()) return true;
            auto it = phase_bias_signal_allowlist_.find(system);
            if (it == phase_bias_signal_allowlist_.end()) return true;
            return it->second.count(signal) > 0;
        }

        /// Non-owning SSR store override; nullptr -> process-wide singleton.
        /// Set via setSsrStore() for the closed-loop consumer (see header).
        const SsrCorrectionStore* ssr_store_override_{ nullptr };

        /// Resolve the active SSR store (override when set, else the singleton).
        /// Defined in the .cpp where the full type is visible.
        const SsrCorrectionStore& ssrStore() const;

        // -------------------------------------------------------------------
        // Delegate components
        // -------------------------------------------------------------------

        /// First-epoch state seeding (ISB, phase clock, DCB/IFB).
        StateInitializer state_initializer_;

        /// BSSD pivot satellite manager (selection, handover, re-referencing).
        PivotManager pivot_manager_;

        /// BSSD S-transformation (differencing operator).  Used by both
        /// PPP-RTK (apply() in place) and baseline DD (the static
        /// differenceBrsd(), called by the row builder).
        BssdTransform bssd_transform_;

        /// Baseline-mode pivot manager.  Owns the per-constellation
        /// sticky map, performs sticky-or-rerank selection each epoch,
        /// and applies the DD pivot-handover S-transform on changes.
        /// The SD path's @c PivotManager is the counterpart for PPP-RTK.
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
        /// while their signal is in a tracking outage, held by the
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

        /// Epoch at which the BSSD pivot handover last ran.  Rebuild passes
        /// within the same epoch (outlier-triggered) keep the pivot fixed so
        /// the SD ambiguities stay consistent; the one exception is a pending
        /// forced handover from the KF post-fit residual detector, which
        /// re-enters the handover in forced-only mode.
        GnssTime last_pivot_epoch_{};

        /// Per-constellation set of full RINEX-3 signal codes covered by the
        /// BSSD pivot.  Rebuilt at the start of each computeMeasurementUpdate()
        /// call (in the BSSD pre-pass) from the pivot satellite's observed
        /// signals.
        ///
        /// Per-signal (not per-band) granularity is required by the BSSD
        /// per-signal grouping in @c BssdTransform: a non-pivot's L1C is
        /// SD-eligible only if the pivot also tracks L1C, even when the pivot
        /// tracks L1W on the same band.  A band-level filter would admit the
        /// non-pivot's L1C, leave it singleton at SD time, and leave the
        /// ambiguity state with no measurement update.
        ///
        /// Key: constellation ("GPS"), Value: set of signals
        /// ({"L1W","L1C","C1W","C1C","L2W","C2W",...}).
        mutable std::map<std::string, std::set<std::string>> bssd_pivot_signals_;

        /// Per-satellite NL reference band for the @c StateAmbiguityBasis::NlWl
        /// path.  Cleared and rebuilt by @c ensureRequiredStates every epoch as
        /// the lowest-numbered band observed that epoch, and consumed by
        /// @c buildJacobianRow (and by @c BssdTransform via a getter) to resolve
        /// which NL state a satellite's phase observation projects onto.  The
        /// NL/WL state keys contain the reference band, so a change of reference
        /// band creates new keys and the old states are pruned.  Empty when
        /// basis == PerBand.  Key: satellite ID; Value: RINEX band digit (e.g. '1').
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
         *        (satellite, raw signal), i.e. whether @c ssr_bias_term will
         *        correct the row in @c buildJacobianRow.
         *
         * Matches the apply-site activation: false in the network producer mode
         * (which estimates satellite biases as states rather than importing
         * them); otherwise a presence check via
         * @c SsrCorrectionStore::hasCodeBias.  When the A5 staleness gate is on,
         * the check is age-gated with the same effective stale-hold limit as the
         * apply site: a bias the gate has dropped no longer defines this row's
         * datum, so the sat-bias state and the no-BIA downweight apply again.
         * Used to suppress redundant per-(satellite, signal) sat-bias state
         * allocation for signals the stream already corrects.  Post-processing
         * leaves the SSR store empty, so this is always false there and the
         * state layout is byte-identical.
         *
         * @param obs_time Observation epoch for the staleness comparison
         *        (ignored unless @c enableSsrBiasMaxAge).
         *
         * @note Raw signals only.  IF-combined rows (key contains '_') get
         *       @c ssr_bias_term = 0 (SSR code biases are per raw signal), so
         *       their sat-bias state stays the only correction and must not be
         *       suppressed; pass raw signal keys here.
         */
        bool ssrCoversCodeSignal(const std::string& satellite,
                                 const std::string& signal,
                                 const GnssTime& obs_time) const;

        /**
         * @brief Whether a .BIA entry counts as bias coverage for the
         *        "already corrected?" gates: code rows (sat-bias absorber
         *        allocation, no-BIA code downweight) and phase rows (AR
         *        eligibility via @c phaseBiasCoveredForAr).
         *
         * Legacy semantics (default): a pure @c hasBiasEntry presence check.  A
         * .BIA that is loaded but not applied (@c enable_code_phase_bias false,
         * e.g. forced off by @c network.estimate_iono_sh and inherited by the
         * derived closed-loop consumer) still reports coverage, so the row runs
         * at full code weight with no absorber while carrying the raw satellite
         * OSB content.  With @c model_corrections.bias_coverage_requires_apply
         * the entry counts only while @c enable_code_phase_bias is actually
         * folding it into @c obs.modeled.  The network producer
         * (@c ProcessingMode::PPP_RTK_Network) keeps legacy semantics under
         * either setting: its code rows are corrected by network-estimated
         * shared code-bias states, not by the .BIA.
         */
        bool biaCoversSignal(const std::string& satellite,
                             const std::string& signal) const;

        /**
         * @brief Whether the live SSR stream supplies a phase bias for this
         *        exact (satellite, raw signal), i.e. whether the phase
         *        @c ssr_bias_term will correct the row in @c buildJacobianRow.
         *
         * Phase-channel counterpart of @c ssrCoversCodeSignal: false in the
         * network producer mode (which estimates satellite phase biases as
         * states rather than importing them); otherwise a presence check via
         * @c SsrCorrectionStore::getPhaseBias, age-gated with the same
         * effective stale-hold limit as the apply site when the A5 staleness
         * gate is on.  Post-processing leaves the SSR store empty, so this is
         * always false there.
         *
         * @param obs_time Observation epoch for the staleness comparison
         *        (ignored unless @c enableSsrBiasMaxAge).
         */
        bool ssrCoversPhaseSignal(const std::string& satellite,
                                  const std::string& signal,
                                  const GnssTime& obs_time) const;

        /**
         * @brief AR-eligibility phase-bias coverage for an ambiguity's signal:
         *        whether the satellite phase bias on this (satellite, signal)
         *        is removed from the phase observable, so that its float
         *        ambiguity may be fixed to an integer.
         *
         * With @c network.require_signal_integer_for_ar on, a covering network
         * phase bias present in the SSR store (age-gated when the staleness gate
         * is on) that declares @c signal_integer = false vetoes eligibility; for
         * an IF key, either component can veto.  With the toggle off, or with no
         * such bias in the store, the checks below decide unchanged.
         *
         * Legacy semantics (default,
         * @c model_corrections.bias_coverage_requires_apply off): raw .BIA
         * presence (@c ifCombHasFullBia for IF-combined keys, @c hasBiasEntry
         * for raw signals), byte-identical to the original gate.  With the
         * toggle on, a signal is covered iff the .BIA entry is actually applied
         * (@c biaCoversSignal) or a live network/SSR phase bias corrects the row
         * (@c ssrCoversPhaseSignal); for an IF key, both components must be
         * covered by the same source.  This lets the closed-loop PPP-RTK
         * consumer, which inherits @c enable_code_phase_bias=false from the
         * producer, key AR eligibility on the network PB products it applies
         * rather than on a .BIA it does not apply.
         *
         * @param signal Raw phase signal (e.g. "L1C") or IF key ("L1C_L5Q").
         * @param obs_time Observation epoch for the SSR staleness comparison.
         */
        bool phaseBiasCoveredForAr(const std::string& satellite,
                                   const std::string& signal,
                                   const GnssTime& obs_time) const;

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
         * Also prunes states that are no longer needed.
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

        // DD pivot selection and the S-transform handover live on
        // @c DdPivotManager (member @c dd_pivot_manager_); see its header for
        // the API.  This class orchestrates only: it owns neither the sticky
        // map nor the handover logic.
    };

} // namespace GnssProcessor