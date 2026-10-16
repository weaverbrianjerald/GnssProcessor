#pragma once

#include "filter/StateManager.hpp"
#include "measurement/AbsorbedSharedStates.hpp"  // StationAbsorbed (the network's withheld shared states, card NOM N1b)
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
     *
     * The implementation is split by concern (card UOM step U4):
     *  - MeasurementModel.cpp: construction and setters; computeMeasurementUpdate,
     *    the order of an epoch's steps and the row loop; hasValidGeometry;
     *    validateMeasurementUpdate; the BSSD forwarders.
     *  - MeasurementModelStates.cpp: the state pre-pass ensureRequiredStates and
     *    its members, one per state category (the table at ensureRequiredStates).
     *  - MeasurementModelRow.cpp: isUsableMeasurement, buildJacobianRow (the
     *    observation equation) and the product coverage checks.
     *  - MeasurementModelVariance.cpp: rowVariance, the variance of one row.
     *  - MeasurementModelDd.cpp: the baseline double-difference build, as stages.
     *  - MeasurementModelNetwork.cpp: the network-only surface (card NOM).
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
         * @brief Compute the measurement update for a single epoch (single
         *        receiver: PPP, PPP-RTK, and each network station).
         *
         * Runs, in order: the first-epoch inter-system bias and phase-clock
         * seeds (@c StateInitializer), the differencing strategy's pivot pass,
         * the state pre-pass @ref ensureRequiredStates (births, seeds and
         * prunes of every dynamic state category), the first-epoch receiver
         * code-bias seed, the strategy's pivot covariance freeze, then one row
         * per usable (satellite, signal): the Jacobian and predicted observable
         * (@ref buildJacobianRow), the innovation, and the row variance; last,
         * the strategy's differencing transform (BSSD) on the assembled update.
         *
         * @param observations  This epoch's modeled observations (one per satellite).
         * @param current_state Not read: the rows are built against the state
         *        after the pre-pass, read from the @c StateManager.  Kept for the
         *        callers, which pass @c getStateVector() taken before the pre-pass.
         * @param excluded_measurements Row signatures (@c "<sat>_<signal>") that
         *        the Kalman filter's rebuild loop rejected on an earlier pass of
         *        this epoch; copied into the result so the exclusions accumulate.
         * @return The epoch's measurement update.  Its row arrays are empty when
         *         there is no observation, no valid state or no usable signal.
         *         When signals were usable but no row could be built, the
         *         innovation, variance and H arrays keep their allocated size
         *         with no satellite ids, which @ref validateMeasurementUpdate
         *         rejects as a dimension mismatch.
         */
        GnssProcessor::EpochMeasurement computeMeasurementUpdate(const std::vector<PppObservation>& observations,
            const Eigen::VectorXd& current_state,
            const std::set<std::string>& excluded_measurements = {});

        // -------------------------------------------------------------------
        // Network-only surface (the PPP-RTK network producer), owned with
        // card NOM: from ensureNetworkSharedStates to carrierFrequency below,
        // and the private members marked "network only".  The out-of-line
        // definitions are in MeasurementModelNetwork.cpp, except
        // setGlonassSlots (MeasurementModel.cpp).
        // -------------------------------------------------------------------

        /**
         * @brief Pre-allocate the shared satellite states owned by the PPP-RTK
         *        network producer's joint estimator (per-satellite clock,
         *        per-(satellite, signal) phase bias and code bias), so this
         *        station's @c H carries their columns this epoch.
         *
         * No-op unless @c ProcessingMode::PPP_RTK_Network and uncombined.  These
         * state families are consume-only in @c buildJacobianRow (this function is
         * their only creator), so without this pre-pass their Jacobian
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
         * code-only scope, coverage allowlist, pivot allowlist, CB-ADM co-gate) is
         * noted in @ref absorbedSharedStates, which this call clears first: the
         * parameter inventory counts each as an absorbed instance (card NOM N1b; the
         * record is measurement/AbsorbedSharedStates.hpp, the inventory
         * filter/NetworkParamInventory.hpp).
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
                                     AbsorbedReason reason);

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
         * @brief Seed the model's frequency helper with GLONASS FCN slot numbers.
         *
         * GLONASS is FDMA, so the carrier frequency, and with it the iono
         * coefficient and wavelength @c buildJacobianRow uses, depends on each
         * satellite's channel.  This model's private @c PppUtils is constructed
         * un-seeded and falls back to the compiled default channel table.  Only
         * the network driver calls this (per station, from the station's RINEX
         * "GLONASS SLOT / FRQ #" header).  The post-processing and real-time
         * constructions do not, so a GLONASS uncombined user run takes the
         * default table here while @c PppModel uses the header slots (card UOM,
         * defect MM-D2; identical whenever the header equals the table).
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
         *
         * @param obs_code  RINEX observation code (e.g. "C1C").
         * @param satellite Satellite id (e.g. "G05"); its first letter gives the system.
         * @return The carrier frequency in Hz, or 0 when unknown.
         */
        double carrierFrequency(const std::string& obs_code,
                                const std::string& satellite) const;

        // ------------------- end of the network-only surface -------------------

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
         * pruned when no row of the epoch references them (a satellite set, a
         * code switch, or a pivot change without the handover).  With
         * @c baseline.enable_pivot_handover a pivot change S-transforms them onto
         * the new pivot and renames them instead (@c DdPivotManager).
         *
         * @param rov_modeled  Rover output, one PppObservation per satellite.
         * @param ref_modeled  Reference output.
         * @param current_state The caller's state snapshot, taken before this
         *        call.  The phase-row innovation subtracts the ambiguity from it
         *        whenever its size equals the state size after this call's pivot
         *        handover, prune and births, and from the current state
         *        otherwise.  Equal size does not imply equal layout or values
         *        (card UOM, defect DD-D1).
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
         *
         * Checks that the innovation, variance, H, satellite-id and signal-type
         * dimensions agree with each other and with the state size, and that
         * every innovation, variance and H row is finite with a positive
         * variance.  An innovation above 100 m is logged as a warning and does
         * not fail the check.
         *
         * @param measurement The assembled update.
         * @return True when the update may be passed to the Kalman filter.
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

    private:
        // -------------------------------------------------------------------
        // Core dependencies (non-owning references)
        // -------------------------------------------------------------------

        StateManager& state_manager_;
        const PppModel& ppp_model_;
        const GnssConfig& config_;
        LogManager& log_manager_;

        // --- Network only (card NOM; MeasurementModelNetwork.cpp) ---------

        /// Network phase-bias coverage allowlist (see @ref setPhaseBiasSignalAllowlist).
        /// system → admitted phase signals.  Empty (default) = allow every signal.
        std::map<std::string, std::set<std::string>> phase_bias_signal_allowlist_;

        /// The shared states withheld from this station this epoch (see
        /// @ref absorbedSharedStates).  Network producer only.
        StationAbsorbed absorbed_shared_;

        /**
         * @brief Whether @p signal may get a satellite phase-bias state under the
         *        network coverage allowlist (@ref setPhaseBiasSignalAllowlist).
         *
         * @param system System display name (e.g. "GPS").
         * @param signal Raw phase code (e.g. "L1C").
         * @return True when the feature is off (empty allowlist) or the system is
         *         not gated; else whether the allowlist admits the signal.
         */
        bool phaseBiasAdmitted(const std::string& system, const std::string& signal) const;

        // --- End of the network-only members --------------------------------

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
        std::map<std::string, std::set<std::string>> bssd_pivot_signals_;

        /// Per-satellite NL reference band for the @c StateAmbiguityBasis::NlWl
        /// path.  Cleared and rebuilt by @c ensureRequiredStates every epoch as
        /// the lowest-numbered band observed that epoch, and consumed by
        /// @c buildJacobianRow to resolve which NL state a satellite's phase
        /// observation projects onto (@c BssdTransform re-derives the reference
        /// band from the NL state keys and does not read this map).  The
        /// NL/WL state keys contain the reference band, so a change of reference
        /// band creates new keys and the old states are pruned.  Empty when
        /// basis == PerBand.  Key: satellite ID; Value: RINEX band digit (e.g. '1').
        std::map<std::string, char> nlwl_ref_band_;

        // Tightly-coupled IMU state (set per-epoch via setImuState())
        Eigen::Matrix3d tc_C_b_e_ = Eigen::Matrix3d::Identity();
        Eigen::Vector3d tc_f_b_ = Eigen::Vector3d::Zero();

        // -------------------------------------------------------------------
        // Product coverage checks (MeasurementModelRow.cpp)
        // -------------------------------------------------------------------

        /**
         * @brief Check whether both component signals of an IF combination
         *        have BIA product coverage.
         * @param satellite Satellite id.
         * @param if_key    Iono-free pair "a_b"; any other key counts as covered.
         * @return True when both signals have a .BIA entry (or the key is not a pair).
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
         * @param if_key Iono-free pair "a_b"; any other key counts as covered.
         * @param system The satellite's system.
         * @return True when both signals have a receiver ATX entry (or the key is
         *         not a pair).
         */
        bool ifCombHasFullRxAtx(const std::string& if_key, System system) const;

        // -------------------------------------------------------------------
        // State pre-pass (MeasurementModelStates.cpp)
        // -------------------------------------------------------------------

        /**
         * @brief Ensure that every dynamic state the current epoch requires
         *        exists in the StateManager, seeded, and prune the rest.
         *
         * Covers the slant ionosphere, the phase colour (network), the
         * per-band or NL/WL ambiguities (with the BSSD pivot-relative seeds),
         * the receiver code biases and the satellite code biases; also runs
         * the network cold receiver-clock pre-seed, marks the ambiguities AR
         * may not fix, and warns on a band count above the frequency mode.
         * A driver: one member per state category below, called in this
         * order, over one working value (@ref RequiredStates).
         *
         * @par The state pre-pass, in call order
         *
         * Each row is one call of the driver.  "Gate" is what makes the state
         * required this epoch (a required state that does not exist is born; one
         * that is no longer required is pruned by its category's prune); "seed"
         * the value it is born with; "initial variance" the diagonal it is born
         * with.  Rows marked "no state" collect or check and write no state.
         *
         * | # | member                        | category                        | gate                                                        | seed                                                   | initial variance                                   |
         * |---|-------------------------------|---------------------------------|-------------------------------------------------------------|--------------------------------------------------------|----------------------------------------------------|
         * | 1 | collectRequiredStates         | keep sets (no state)            | a code or phase signal the BSSD pivot tracks (a)            |                                                        |                                                    |
         * | 2 | addHoldProtectedKeys          | keep sets (no state)            | a key the soft-LLI / gap bridge holds                       |                                                        |                                                    |
         * | 3 | collectNlWlKeys               | NL/WL keys (no state)           | NL/WL basis                                                 |                                                        |                                                    |
         * | 4 | ensureIonosphereStates        | slant ionosphere                | uncombined; a code or phase row of the satellite            | StateInitializer::computeIonosphereSeed, else 0 (b)    | (initial_std.ionosphere x s)^2 / sin^2(el) (c)     |
         * | 5 | ensurePhaseColourStates       | phase colour (network)          | network.phase_colour_state; an uncombined phase row         | 0                                                      | network.phase_colour_sigma_zen_m^2                 |
         * | 6 | preseedColdReceiverClock      | receiver clock (network)        | network.cold_clock_seed, until a seed succeeds (no birth)   | median GPS code residual (seedColdReceiverClock)       | unchanged                                          |
         * | 7 | computePivotSeeds             | BSSD pivot seeds (no state)     | BSSD                                                        | StateManager::computeAmbiguitySeed per pivot signal    |                                                    |
         * | 8 | ensurePerBandAmbiguities      | ambiguity, per band             | per-band basis; a phase key, new or pending reseed          | computeAmbiguitySeed, pivot-relative under BSSD (d)    | substituted-ATX override, else 1000 m (e)          |
         * | 9 | ensureNlWlAmbiguities         | ambiguity, NL and WL            | NL/WL basis; as row 8                                       | computeAmbiguitySeed / wavelength, in cycles (d)       | as row 8 (the same number, on a state in cycles)   |
         * | - | pruneAmbiguitiesExcept        | ambiguity prune (in the driver) | a key not in the keep set (rows 1 to 3)                     |                                                        |                                                    |
         * |10 | warnAmbiguityBandCount        | band count (no state)           | per-band basis; Dual or Triple                              |                                                        |                                                    |
         * |11 | markUnresolvableAmbiguities   | AR eligibility flags            | per-band basis (f)                                          |                                                        |                                                    |
         * |12 | ensureReceiverCodeBiasStates  | receiver code bias              | estimate_ifb, uncombined; a code row with a bias key (g)    | 0, then StateInitializer::initializeCodeBiases once    | initial_std.code_bias^2                            |
         * |13 | ensureSatCodeBiasStates       | satellite code bias (user path) | estimate_sat_biases, not the network; uncovered code (h)    | the sat-bias cache, else the row's innovation (i)      | the cached variance, else initial_std.sat_bias^2   |
         *
         * Notes.  (a) GLONASS is skipped under BSSD unless enable_glonass_bssd.
         * (b) The geometry-free code pair with .BIA coverage; on the network the
         * LV-1 seed frame reframes it when installed.  (c) s = 1, or 10 when the
         * seed is 0 for want of a covered pair; sin^2(el) when the elevation is
         * known.  (d) The pivot's own state is 0; the others subtract the pivot's
         * seed of the same signal (row 7).  On a reseed the value is replaced and
         * the variance is the one the reset set.  (e)
         * model_corrections.ambiguity_std_substituted_atx when
         * loosen_amb_q_for_substituted_atx is on and the signal's ATX was
         * substituted, else GnssConfig::getAmbiguityStd(), 1000 m with no JSON key
         * (card UOM defect S3).  (f) GLONASS under BSSD; a signal without applied
         * phase-bias coverage; an iono-free pair also without receiver ATX.
         * (g) The key from StateInitializer::resolveReceiverBias (DCB, IFB, ISC,
         * or GLONASS per-slot IFB; the GLONASS network keeps per-slot keys only).
         * (h) A raw code (uncombined) or iono-free code pair without .BIA or SSR
         * coverage that the BSSD pivot tracks; an uncombined GLONASS signal with a
         * per-slot IFB key is excluded.  The network producer allocates its
         * satellite code biases in @ref ensureNetworkSharedStates.  (i) From
         * buildJacobianRow, re-referenced to the pivot's innovation under BSSD.
         *
         * The network producer's own shared states (satellite clock, phase bias
         * and code bias) are not in this table: @ref ensureNetworkSharedStates
         * allocates them in PASS A, before this pre-pass runs.
         *
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix (empty: the log pattern carries the time).
         */
        void ensureRequiredStates(const std::vector<PppObservation>& observations,
            const std::string& epoch_prefix);

        /// The pre-pass's working value: its configuration flags, and the keep
        /// sets and pivot seeds its members collect for the members that follow.
        /// Defined in MeasurementModelStates.cpp, the only file that uses it.
        struct RequiredStates;

        /**
         * @brief Collect what the epoch requires (no state written): the slant
         *        ionosphere satellites, the per-band ambiguity keys, the NL/WL
         *        phase signals and the phase-colour satellites, from the code and
         *        phase signals the BSSD pivot tracks (GLONASS skipped under BSSD
         *        unless enable_glonass_bssd).
         * @param observations This epoch's modeled observations.
         * @param need         The working value (filled).
         */
        void collectRequiredStates(const std::vector<PppObservation>& observations,
                                   RequiredStates& need) const;

        /**
         * @brief Hold-protected ambiguities (soft-LLI / gap bridge): the held keys
         *        join the ambiguity keep set and, uncombined, their satellites the
         *        ionosphere keep set, so the prune keeps the pre-gap states.
         * @param need The working value (keep sets extended).
         */
        void addHoldProtectedKeys(RequiredStates& need) const;

        /**
         * @brief NL/WL ambiguity keys (NL/WL basis): per satellite the reference
         *        band (the lowest band observed, kept in @c nlwl_ref_band_), one
         *        NL key, and one WL key per other band.
         * @param need The working value (ambiguity keep set extended).
         */
        void collectNlWlKeys(RequiredStates& need);

        /**
         * @brief Slant ionosphere (uncombined): born for each satellite with a
         *        code or phase row; seeded from the geometry-free code pair
         *        (@c StateInitializer::computeIonosphereSeed, reframed by the
         *        network's LV-1 seed frame when installed), else 0 with ten times
         *        the initial std; pruned to the keep set.
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void ensureIonosphereStates(const std::vector<PppObservation>& observations,
                                    const std::string& epoch_prefix,
                                    const RequiredStates& need);

        /**
         * @brief Phase colour (network only: network.phase_colour_state, card
         *        LGM): born for each satellite with an uncombined phase row;
         *        seeded 0 with variance phase_colour_sigma_zen_m squared; pruned
         *        to the keep set.
         * @param need The working value.
         */
        void ensurePhaseColourStates(const RequiredStates& need);

        /**
         * @brief Cold receiver-clock pre-seed (network only:
         *        network.cold_clock_seed, once per station): no birth; seeds the
         *        receiver clock from the median GPS code residual
         *        (@c StateInitializer::seedColdReceiverClock) before any
         *        ambiguity is seeded.
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         */
        void preseedColdReceiverClock(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

        /**
         * @brief BSSD pivot seeds (no state written): each pivot's absolute
         *        ambiguity seed per signal (@c StateManager::computeAmbiguitySeed),
         *        in cycles as NL and WL on the NL/WL basis; the non-pivot
         *        ambiguities are seeded relative to them.
         * @param observations This epoch's modeled observations.
         * @param need         The working value (pivot seeds filled).
         */
        void computePivotSeeds(const std::vector<PppObservation>& observations,
                               RequiredStates& need) const;

        /**
         * @brief Per-band ambiguities: born for each phase key of the keep set,
         *        or re-seeded when pending reseed; seeded from
         *        @c StateManager::computeAmbiguitySeed (minus the pivot's seed
         *        under BSSD, 0 for the pivot); initial std from the substituted-ATX
         *        override, else the StateManager default.
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void ensurePerBandAmbiguities(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix,
                                      const RequiredStates& need);

        /**
         * @brief NL/WL ambiguities (NL/WL basis): one NL (cycles on the reference
         *        band) and one WL per other band, born or re-seeded as the
         *        per-band ambiguities; seeded from
         *        @c StateManager::computeAmbiguitySeed over the wavelengths
         *        (pivot-relative under BSSD).
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void ensureNlWlAmbiguities(const std::vector<PppObservation>& observations,
                                   const std::string& epoch_prefix,
                                   const RequiredStates& need);

        /**
         * @brief Band-count check (no state written; per-band basis): warns for a
         *        satellite whose ambiguity keep set has more bands than the
         *        frequency mode admits (Dual 2, Triple 3).
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void warnAmbiguityBandCount(const std::string& epoch_prefix,
                                    const RequiredStates& need) const;

        /**
         * @brief AR eligibility (per-band basis): clears the unresolvable marks,
         *        then marks the ambiguities AR may not fix (GLONASS under BSSD; a
         *        signal without applied phase-bias coverage; an iono-free pair
         *        also without receiver ATX on both signals).
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void markUnresolvableAmbiguities(const std::vector<PppObservation>& observations,
                                         const std::string& epoch_prefix,
                                         const RequiredStates& need);

        /**
         * @brief Receiver code biases (estimate_ifb, uncombined): born for each
         *        code signal whose receiver-bias rule
         *        (@c StateInitializer::resolveReceiverBias) gives a key (the
         *        GLONASS network keeps per-slot IFB keys only); seeded 0, then
         *        once at the first epoch by @c initializeCodeBiases; pruned to the
         *        keep set.
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         */
        void ensureReceiverCodeBiasStates(const std::vector<PppObservation>& observations,
                                          const std::string& epoch_prefix);

        /**
         * @brief Satellite code biases (estimate_sat_biases; user path, the
         *        network producer allocates its own in
         *        @ref ensureNetworkSharedStates): born for each code signal (raw,
         *        or iono-free pair) without .BIA or SSR coverage that the BSSD
         *        pivot tracks; restored from the sat-bias cache, else seeded from
         *        the row's innovation (re-referenced to the pivot under BSSD);
         *        pruned to the keep set, into the cache.
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param need         The working value.
         */
        void ensureSatCodeBiasStates(const std::vector<PppObservation>& observations,
                                     const std::string& epoch_prefix,
                                     const RequiredStates& need);

        // -------------------------------------------------------------------
        // One row: usability, geometry and the observation equation
        // (MeasurementModelRow.cpp; hasValidGeometry in MeasurementModel.cpp)
        // -------------------------------------------------------------------

        /**
         * @brief Whether an observation's signal enters this epoch's update.
         *
         * False for: GLONASS under BSSD unless enable_glonass_bssd; a code,
         * phase or Doppler signal the BSSD pivot does not track; a signal that
         * is neither code nor phase (nor Doppler with velocity estimation); an
         * iono-free pair in uncombined mode, a raw signal in iono-free mode, a
         * Doppler pair; a signal without an observed, variance and modeled
         * value; a row the Kalman filter's rebuild loop excluded
         * (@c "<sat>_<signal>", logged at debug).
         *
         * @param obs          The satellite's modeled observation.
         * @param ref_type     The signal (a RINEX code, or an iono-free pair "a_b").
         * @param excluded     The rebuild loop's excluded row signatures.
         * @param epoch_prefix Log prefix.
         * @return True when the signal is usable.
         */
        bool isUsableMeasurement(const PppObservation& obs,
            const std::string& ref_type,
            const std::set<std::string>& excluded,
            const std::string& epoch_prefix) const;

        /**
         * @brief The row loop's per-satellite geometry gate: a finite satellite
         *        position at least 1.9e7 m from the geocentre, a finite range of
         *        at least 1.5e7 m, an elevation at or above the mask, a finite
         *        line of sight of norm at least 0.5, and a finite positive wet
         *        mapping function.
         *
         * Logs the failing check (warn; debug for the elevation mask).  The
         * network pre-pass @ref ensureNetworkSharedStates repeats the five
         * checks without the logs, so the shared columns it allocates are the
         * ones the rows write.
         *
         * @param obs          The satellite's modeled observation.
         * @param epoch_prefix Log prefix.
         * @return True when the satellite's rows may be built.
         */
        bool hasValidGeometry(const PppObservation& obs,
                              const std::string& epoch_prefix) const;

        /**
         * @brief Build one row of H and the predicted observable: the
         *        single-receiver observation equation.
         *
         * Code and phase rows: the a-priori model value plus the range
         * correction for the position's offset from the linearization point,
         * and one term per estimated or applied quantity: zenith wet delay,
         * receiver clock (no column under BSSD), phase clock (phase rows),
         * troposphere gradients, inter-system bias, slant ionosphere
         * (uncombined; +alpha on code, -alpha on phase), receiver code bias
         * (code, estimate_ifb), satellite code bias (estimate_sat_biases), the
         * network producer's satellite phase bias, receiver phase bias and
         * satellite clock, the SSR satellite bias (no state; not on the network
         * producer), the ambiguity (phase; per band, or lambda_b (NL - s WL) on
         * the NL/WL basis) and the network's phase colour.  Doppler rows:
         * velocity, clock rate, inter-system rate and the tightly coupled IMU
         * partials.  Prints the PHASE-DECOMP and CODE-DECOMP traces at trace
         * level.
         *
         * @param obs        The satellite's modeled observation.
         * @param ref_type   The signal (a RINEX code, or an iono-free pair "a_b").
         * @param x          The state vector the row is evaluated at.
         * @param state_size The number of state columns (the row's length).
         * @param jacobian   Out: the row of H.
         * @param predicted  Out: the predicted observable (m).
         * @return False when the row cannot be built: a Doppler row without a
         *         velocity state, or a phase row whose ambiguity state (or, on
         *         the NL/WL basis, reference band or wavelength) is missing.
         */
        bool buildJacobianRow(const PppObservation& obs,
            const std::string& ref_type,
            const Eigen::VectorXd& x,
            size_t state_size,
            Eigen::VectorXd& jacobian,
            double& predicted) const;

        // -------------------------------------------------------------------
        // One row's variance (MeasurementModelVariance.cpp)
        // -------------------------------------------------------------------

        /**
         * @brief The measurement variance of one row (m^2): the stochastic-model
         *        variance plus the consumer's network-correction terms, times the
         *        no-BIA code downweight.
         *
         * In order: obs.variances[ref_type]; with consume_combined_sigma and a
         * fresh network correction carrying a combined sigma, that sigma
         * (age-inflated) in place of the next two terms; otherwise the network
         * URA with the ZOH clock-staleness term and the held-clock fallback,
         * and on phase rows the network phase-bias sigma; then, without
         * estimate_sat_biases, the no-BIA downweight factor on a code row
         * without .BIA or SSR coverage.  The network producer's own rows get
         * none of the network terms.
         *
         * @param obs      The satellite's modeled observation.
         * @param ref_type The signal (one with a variance: the row loop admits
         *                 only usable signals).
         * @return The row's variance, before the row loop's non-finite guard.
         */
        double rowVariance(const PppObservation& obs,
                           const std::string& ref_type) const;

        // -------------------------------------------------------------------
        // BSSD delegation (forwarders to PivotManager)
        // -------------------------------------------------------------------

        /**
         * @brief Run the BSSD pivot handover for this epoch
         *        (@c PivotManager::handlePivotHandover).
         * @param observations This epoch's modeled observations.
         * @param epoch_prefix Log prefix.
         * @param forced_only  True on a rebuild pass of the same epoch: only a
         *                     pending forced handover may run.
         */
        void handleBssdPivotHandover(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix,
                                      bool forced_only = false);

        /// Freeze the pivots' ambiguity covariance
        /// (@c PivotManager::freezePivotAmbiguityCovariance).
        void freezePivotAmbiguityCovariance();

        /// Freeze the pivots' ionosphere and bias covariance
        /// (@c PivotManager::freezePivotIonoAndBiasCovariance).
        void freezePivotIonoAndBiasCovariance();

        // -------------------------------------------------------------------
        // Double-difference (baseline) assembly (MeasurementModelDd.cpp)
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
         * semantics.  Owns the process-wide first-call diagnostic flag (a
         * function-local static shared by every instance, reset on every normal
         * exit) and runs the stages through @ref ddRunStages.
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

        /// One satellite tracked by both receivers (its between-receiver
        /// signals and pivot-ranking inputs).  Defined in MeasurementModelDd.cpp.
        struct DdSharedSat;
        /// One double-differenced row.  Defined in MeasurementModelDd.cpp.
        struct DdRow;
        /// The DD build's working value (logger, diagnostic flag, shared
        /// satellites, pivots, rows).  Defined in MeasurementModelDd.cpp.
        struct DdBuild;

        /**
         * @brief The DD stages in order; returns at the first that finds nothing
         *        to build (no input, no shared satellite, no pivot, no row, no
         *        kept row), leaving @p em as built so far.
         * @param rov_modeled           Rover output, one PppObservation per satellite.
         * @param ref_modeled           Reference output.
         * @param current_state         The caller's state snapshot (see @ref ddInnovations).
         * @param excluded_measurements The rebuild loop's excluded row signatures.
         * @param dd                    The working value.
         * @param em                    The update being built.
         */
        void ddRunStages(const std::vector<PppObservation>& rov_modeled,
                         const std::vector<PppObservation>& ref_modeled,
                         const Eigen::VectorXd& current_state,
                         const std::set<std::string>& excluded_measurements,
                         DdBuild& dd, EpochMeasurement& em);

        /**
         * @brief First-call diagnostic (only when dd.diag): the two receivers'
         *        satellite counts and the first satellite's observation codes.
         * @param rov_modeled Rover output.
         * @param ref_modeled Reference output.
         * @param dd          The working value.
         */
        void ddLogInputDiagnostic(const std::vector<PppObservation>& rov_modeled,
                                  const std::vector<PppObservation>& ref_modeled,
                                  const DdBuild& dd) const;

        /**
         * @brief The satellites both receivers track (active systems, not
         *        excluded), each with its between-receiver signals
         *        (@c BrsdTransform::apply on the canonical code per band, paired
         *        across tracking attributes when signal_aliasing); keeps the
         *        satellites with at least two.
         * @param rov_modeled Rover output.
         * @param ref_modeled Reference output.
         * @param dd          The working value (satellite maps and by_cons filled).
         * @return False when no satellite is shared.
         */
        bool ddSharedSignals(const std::vector<PppObservation>& rov_modeled,
                             const std::vector<PppObservation>& ref_modeled,
                             DdBuild& dd) const;

        /**
         * @brief State write 1 of 3: the sticky pivot per constellation
         *        (@c DdPivotManager::selectPivots), whose handover S-transforms
         *        and renames the DD ambiguity states on a pivot change.
         * @param dd The working value (pivots filled).
         * @return False when no constellation has a pivot.
         */
        bool ddSelectPivots(DdBuild& dd);

        /**
         * @brief The DD rows: @c BssdTransform::differenceBrsd over the pivots,
         *        without the rebuild loop's excluded rows, each with its
         *        rover-position partial, wet mapping values and R group key.
         * @param excluded_measurements The rebuild loop's excluded row signatures.
         * @param dd                    The working value (rows filled).
         */
        void ddRowsFromBrsd(const std::set<std::string>& excluded_measurements,
                            DdBuild& dd) const;

        /**
         * @brief Iono-free collapse (baseline.use_iono_free): each satellite
         *        pair's per-band rows become one row on the band pair of least
         *        noise amplification K (at most 10), never integer-capable.
         * @param dd The working value (rows replaced).
         */
        void ddCollapseIonoFree(DdBuild& dd) const;

        /**
         * @brief Whether any row remains (with the diagnostic when none does).
         * @param dd The working value.
         * @return False when the build has no row.
         */
        bool ddHasRows(const DdBuild& dd) const;

        /**
         * @brief State write 2 of 3: removes every DD ambiguity state that no row
         *        of this epoch, and no row the rebuild loop excluded under this
         *        epoch's pivot, references.
         * @param excluded_measurements The rebuild loop's excluded row signatures.
         * @param dd                    The working value.
         */
        void ddPruneStaleAmbiguities(const std::set<std::string>& excluded_measurements,
                                     const DdBuild& dd);

        /**
         * @brief The AR exclusion snapshot: the DD ambiguity keys of the phase
         *        rows that are not integer-capable, rebuilt every call
         *        (@ref getNonIntegerCapableDdAmbiguityKeys).
         * @param dd The working value.
         */
        void ddNonIntegerKeys(const DdBuild& dd);

        /**
         * @brief State write 3 of 3 (not code_only): a DD ambiguity state for each
         *        phase row without one, seeded with the row's pre-ambiguity
         *        innovation, its variance filter.initial_std.ambiguity squared
         *        (1e8 when not positive); records each row's state column.
         * @param dd The working value (row columns filled).
         */
        void ddEnsureAmbiguities(DdBuild& dd);

        /**
         * @brief Each row's innovation (a phase row minus its ambiguity) and the
         *        rows kept for the update.
         *
         * The ambiguity is read from @p current_state when its size equals the
         * state size after the three state writes, else from the state: equal
         * size does not imply equal layout or values (card UOM, defect DD-D1,
         * kept unchanged here).
         *
         * @param current_state The caller's state snapshot.
         * @param dd            The working value (innovations and kept filled).
         * @return False when no row is kept or the state cannot be read.
         */
        bool ddInnovations(const Eigen::VectorXd& current_state, DdBuild& dd) const;

        /**
         * @brief H (rover position, the ambiguity column, the differential wet
         *        delay on phase rows), the innovations, the variances and R with
         *        the pivot variance shared by the rows of one group, and the row
         *        identities (@c "sat_i-pivot", the rover code or "rov/ref").
         * @param dd The working value.
         * @param em The update (filled).
         */
        void ddAssemble(const DdBuild& dd, EpochMeasurement& em) const;
    };

} // namespace GnssProcessor