#pragma once

/**
* @file CycleSlipManager.hpp
* @brief Cycle-slip detection and reset coordinator over a config-selected
*        set of @c ISlipDetector instances.
*
* CycleSlipManager owns the detector set built by @c SlipDetectorRegistry
* from @c GnssConfig::getCycleSlipConfig and coordinates cycle slip
* detection, multi-frequency signal identification, ambiguity repair and
* ambiguity reset.  The Melbourne-Wübbena detector (when enabled) is the
* primary: it drives the cross-consistency and BSSD-aware repair/handover
* pipeline and is the source of WL integers for AR and the BSSD pivot
* manager.  The auxiliary detectors the registry builds (GRAPHIC, EWL) run
* through the uniform polymorphic contract and trigger BSSD-aware resets.
* The registry registers no GF detector (its source is not compiled) and no
* IF-phase detector (that one is built directly on the network path).  The
* manager also runs the optional TurboEdit MW+GF joint repair and the
* soft-LLI gap bridge.
*
* Design:
*   - Owns the detector set; @c mw_ is a non-owning handle to the MW detector.
*   - detectAndRepair() is the single entry point called per epoch.
*   - The MwSlipDetector forms *all* dual-frequency MW pairs for each
*     satellite (L1-L2, L1-L5, L2-L5, etc.), including EWL pairs with
*     very long wavelengths (~5-10 m).  This provides unified slip
*     detection across all frequency pairs.
*   - Multi-frequency MW cross-consistency: when 3+ signals are
*     available, overlapping MW pairs are compared.  If L1W_L2W slips
*     but L1W_L5Q does not, then L2W slipped and only that ambiguity
*     is reset (rather than resetting all ambiguities for the satellite).
*   - A culprit signal with a cross-consistent integer is repaired in the
*     filter state (x += dN·λ, covariance preserved).  The manager does not
*     shift the MW arc: its statistics are observation-space and are not
*     affected by the state-level integer step.
*   - Falls back to full ambiguity reset when targeted identification
*     is not possible.
*/

#include "slip/ISlipDetector.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"
#include "slip/SoftLliBridge.hpp"
#include "filter/StateManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Epoch-level result of cycle slip processing.
 */
struct CycleSlipEpochResult {
    /// Slips handled per signal: additive integer repair, targeted
    /// single-signal reset when the repair is refused, or a pivot-deferred
    /// repair/reset; also TurboEdit GF-only repairs.  Exception in units: the
    /// soft-LLI bridge adds one per repaired SATELLITE, not per signal
    /// (@c SoftLliBridge::resolveHolds).  Clean-peer shifts of the in-place
    /// pivot-frame repair are not counted.
    int n_repaired{ 0 };
    /// Slips handled by ambiguity reset rather than a per-signal repair (full
    /// reset, residual mandated resets, auxiliary-detector resets, TurboEdit
    /// reset fallback), one per ambiguity key; the soft-LLI bridge adds one
    /// per key it resets on a restore failure or a timeout.
    int n_reset{ 0 };
    /// Satellites whose MW mandates were suppressed by the dedicated
    /// code-event veto branch (GF affirmatively stable; no state touched
    /// for the adjudicated signals).  Counted per satellite, not per pair,
    /// and only for the veto branch: a code-event satellite that the culprit
    /// branch handled through clean_signals adjudication is not counted here.
    /// See TurboEditResolver's "code-domain veto" header note.
    int n_code_events{ 0 };
    bool state_modified{ false }; ///< True if any filter state was changed.

    /// Satellites whose ambiguity state was reset or reseeded this epoch by the
    /// slip phase: SD-aware reset, pivot-disturbance variance inflation, a
    /// soft-LLI bridge restore (absolute value overwrite), or a post-handover
    /// deferred repair.  These carry a fresh, large-variance (or
    /// covariance-rebuilt) prior that holds no information backward, so the RTS
    /// smoother segments them: their ambiguities are excluded from the backward
    /// common subspace at this epoch boundary.  See @c Smoother and the
    /// slip-repair audit finding F1.
    std::set<std::string> reset_satellites;

    /// Covariance-preserving additive integer repairs applied this epoch, as
    /// (ambiguity_key, delta_metres) with @c delta = dN·λ.  A repair is a
    /// deterministic, known state jump, not a measurement event, so the
    /// smoother folds @c delta into the stored a-priori @c x_pred (a control
    /// input) instead of letting the backward pass absorb it as an innovation
    /// (audit F1).  Only repairs applied in the pre-handover frame (through
    /// @c repairAmbiguityBssdAware) are recorded here; the post-handover
    /// deferred repair lands in the new SD frame and is recorded as a
    /// @c reset_satellites entry (segmented) instead.
    std::vector<std::pair<std::string, double>> ambiguity_repairs;
};

/**
* @class CycleSlipManager
* @brief Coordinates cycle slip detection (MW primary, registry auxiliaries,
*        optional TurboEdit MW+GF), multi-frequency signal identification,
*        and ambiguity repair or reset.
*
* Design:
*   Pre-estimation:
*     1. MW detector forms all dual-frequency MW pairs (L1-L2, L1-L5,
*        L2-L5, etc.) and flags WL slips with integer sizing.  For
*        pairs with satellite BIA biases, the MW is fixed to integer;
*        otherwise, the regular Hatch filter is used.
*     2. Multi-frequency cross-consistency: when a satellite has 3+
*        signals, overlapping MW pairs are compared to isolate which
*        individual signal slipped, enabling a targeted single-signal
*        repair or reset instead of resetting all signals.
*     3. Unidentified slips get a full ambiguity reset.
*     4. TurboEdit GF-only detections and auxiliary-detector events are
*        applied to satellites the MW path did not handle.
*/
class CycleSlipManager {
public:
    /**
     * @brief Construct the manager.
     * @param config        Processing configuration.
     * @param log_manager   Logging facility.
     */
    CycleSlipManager(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Pre-estimation: cycle slip detection, repair and reset.
     *
     * Pipeline:
     *   1. Register soft-LLI gap holds (when the bridge is enabled).
     *   2. Run the MW/LLI detector to identify WL slips, and the TurboEdit
     *      resolver when enabled.
     *   3. Multi-frequency cross-consistency to isolate culprit signals;
     *      repair or reset them BSSD-aware, deferring pivot-affected ones
     *      to the forced pivot handover (PivotManager::handlePivotHandover).
     *   4. Fall through to full reset for unidentified slips.
     *   5. Apply TurboEdit GF-only resolutions, then auxiliary-detector
     *      resets, to satellites not already handled.
     *
     * @param epoch_prefix     Log prefix.
     * @param modeled_obs      Modeled observations for this epoch.
     * @param state_manager    Filter state manager (modified on reset).
     * @param meas_model       Measurement model: read for the BSSD pivot map;
     *                         its @c PivotManager is also WRITTEN, through a
     *                         @c const_cast, when a slipped pivot forces a
     *                         handover (the caller owns it as non-const).
     * @return Epoch result summary.
     */
    CycleSlipEpochResult detectAndRepair(
        const std::string& epoch_prefix,
        const std::vector<PppObservation>& modeled_obs,
        StateManager& state_manager,
        const MeasurementModel& meas_model);

    /// Get resolved WL ambiguities (forwarded from MW detector).
    std::map<std::string, int> getResolvedWlAmbiguities() const;

    /// Get fractional residuals of currently-locked WL arcs (forwarded from MW detector).
    std::map<std::string, double> getResolvedWlFractions() const;

    /// Per-arc MW wide-lane lock quality (σ of the arc mean + sample count),
    /// the input the WL-only AR acceptance gate needs to bound each arc's
    /// rounding failure rate.  Empty when no MW detector is active.
    std::map<std::string, MwSlipDetector::WlLockQuality> getResolvedWlQuality() const;

    /// Reset satellite state in all detectors and in the TurboEdit resolver.
    void resetSatellite(const std::string& satellite);

    /// Reset arc state for one (satellite, signal) only, in every detector and
    /// in the TurboEdit resolver.  In the MW detector, arcs on signal pairs that
    /// do not involve the specified signal are preserved.  Use when the upstream
    /// caller knows which signal slipped (e.g. via the KF's per-(sat, signal)
    /// outlier dispatch).
    void resetSatelliteSignal(const std::string& satellite,
                              const std::string& signal);

    /// Provide bias corrections to every detector (MW and EWL use them; the
    /// others ignore them) and to the TurboEdit resolver, if enabled.
    void setCorrections(const GnssCorrections* corrections);

    /// Enable MW trace logging.
    bool enableMwTracing(const std::filesystem::path& log_dir);

    /// Enable the TurboEdit per-pair joint-solve trace (no-op if turbo_edit
    /// is disabled).  Returns true if the trace file was opened.
    bool enableTurboEditTracing(const std::filesystem::path& log_dir);

    /// Provide the effective observation sampling interval (s) used to gate the
    /// high-rate-only TurboEdit GF repair (the resolver warns once when the
    /// interval is coarser than 1 s).  Also forwards the interval to the
    /// soft-LLI bridge and sets the TurboEdit GF-arc gap tolerance to 1.5 times
    /// the cadence.
    void setSampleInterval(double interval_s);

    /// Access the MW detector for its wider API (WL-lock queries, per-sat
    /// quality scoring, pivot hints), or nullptr if the MW detector is
    /// disabled via @c estimation.cycle_slip.mw.
    MwSlipDetector* mwDetector() { return mw_; }
    const MwSlipDetector* mwDetector() const { return mw_; }

    /**
     * @brief Cache the last MW slip results from detectAndRepair().
     * @return Map from satellite ID to its MwSlipResult.
     */
    const std::map<std::string, MwSlipResult>& getLastMwSlips() const { return last_mw_slips_; }

    /**
     * @brief Record a rolling "last good" snapshot of every ambiguity value
     *        and variance: the pre-gap anchor for the soft-LLI bridge repair.
     *
     * Call once per epoch after the Kalman measurement update, so it captures
     * the converged post-update values.  Only ambiguities whose variance is
     * below a converged threshold update the snapshot, so a fresh reset or
     * pending-reseed does not overwrite the last converged value.  That value
     * (from the epoch before the signal dropped) is the anchor a later
     * soft-LLI hold needs.  No-op unless
     * @c estimation.cycle_slip.turbo_edit_bridge_lli is set.
     *
     * @param state_manager Filter state to snapshot from.
     * @param epoch_now_s   Continuous GPS seconds of this epoch (recency stamp).
     */
    void recordAmbiguitySnapshot(const StateManager& state_manager, double epoch_now_s);

    /**
     * @brief Phase-measurement keys ("G03_L2W") currently held across a soft
     *        LLI gap pending a TurboEdit bridge repair.
     *
     * The caller (@c Application / @c RealTimeEngine) seeds the Kalman update's
     * @c excluded_measurements with these so the held, still-slipped
     * ambiguities do not contaminate shared states across the MW one-epoch
     * confirmation delay.  Empty unless @c turbo_edit_bridge_lli.
     */
    std::set<std::string> getHeldPendingBridgeKeys() const;

    /**
     * @brief Drop all soft-LLI / gap-bridge hold and snapshot state.
     *
     * Call alongside any hard filter reset that deletes ambiguity states
     * outside the measurement model's protected prune (e.g. the long-gap
     * state teardown in @c Application::handleGap): a hold that outlives
     * the state it anchors would later "restore" a pre-gap value onto a
     * freshly rebuilt ambiguity.  No-op when nothing is held.
     */
    void resetSoftLliBridge();

private:
    /**
     * @brief Reset a single ambiguity with BSSD awareness.
     *
     * When BSSD is active, pivot slips are deferred and non-pivot
     * ambiguities are re-seeded as SD values.
     */
    void resetAmbiguityBssdAware(
        const std::string& amb_key,
        const std::string& sat_id,
        const std::string& signal,
        const std::vector<PppObservation>& modeled_obs,
        StateManager& state_manager,
        const MeasurementModel& meas_model,
        const std::string& epoch_prefix);

    /**
     * @brief Apply an additive integer cycle slip repair to a single
     *        (sat, signal) ambiguity, preserving its covariance.
     *
     * The textbook alternative to a reset when the integer slip magnitude
     * is known with high confidence: instead of inflating the variance
     * and re-seeding from a fresh observation (which loses the converged
     * ambiguity precision), shift the state by @c slip_cycles · λ_signal
     * and leave the covariance untouched.  Refuses to act on the BSSD
     * pivot (its SD ambiguity is frozen at 0; pivot handover is the
     * proper response there) or when the ambiguity isn't in the state
     * vector or the signal wavelength can't be resolved.
     *
     * @param sat_id       Satellite identifier (e.g. "G01").
     * @param signal       Signal identifier (e.g. "L1C").
     * @param slip_cycles  Integer cycle slip in @em this signal's cycles.
     * @param modeled_obs  Current epoch's modeled observations (for system / wavelength lookup).
     * @param state_manager Filter state (modified in place on success).
     * @param meas_model    For BSSD pivot lookup (refusal path).
     * @param epoch_prefix  Log prefix string.
     * @return true if the repair was applied; false if the caller should
     *         fall back to @c resetAmbiguityBssdAware.
     */
    bool repairAmbiguityBssdAware(
        const std::string& sat_id,
        const std::string& signal,
        int slip_cycles,
        const std::vector<PppObservation>& modeled_obs,
        StateManager& state_manager,
        const MeasurementModel& meas_model,
        const std::string& epoch_prefix);

    /**
     * @brief Resolve per-(culprit signal) integer slip magnitudes from
     *        the slipped MW pairs.
     *
     * For each culprit signal identified by cross-consistency, this
     * walks the slipped pairs and attributes the WL slip integer to
     * the culprit alone (since the other signal in the pair is known
     * clean).  When multiple slipped pairs share the same culprit, the
     * resulting per-culprit slip integers must agree across pairs;
     * inconsistency disqualifies repair for that signal (caller should
     * fall back to reset).
     *
     * Pairs without @c slip_wl_known or with both signals identified
     * as culprits cannot attribute the WL slip to a single signal and
     * are skipped as contributors.
     *
     * @param slipped_pairs Slipped MwSlipResult entries for one satellite.
     * @param culprits      Culprit signals from @c identifySlippedSignals.
     * @return Map from culprit signal → integer slip in that signal's
     *         cycles.  Culprits not present in the map should fall back
     *         to reset.
     */
    std::map<std::string, int> computeCulpritSlipIntegers(
        const std::vector<MwSlipResult>& slipped_pairs,
        const std::set<std::string>& culprits) const;

    /**
     * @brief Multi-frequency MW cross-consistency analysis.
     *
     * Given MW slip results for a satellite with 3+ signals, determines
     * which individual signal(s) slipped by examining overlapping MW pairs.
     * For example, if L1W_L2W slipped but L1W_L5Q did not, then L2W is
     * the culprit (L1W is common and clean).
     *
     * @param sat_slips   MW slip results for one satellite (multiple pairs).
     * @param all_results All MW results for this satellite (including non-slipped).
     * @return Set of individual signal identifiers that slipped (e.g. {"L2W"}).
     *         Empty if the analysis is inconclusive.
     */
    std::set<std::string> identifySlippedSignals(
        const std::vector<MwSlipResult>& sat_slips,
        const std::vector<MwSlipResult>& all_results) const;

    /**
     * @brief Apply slip events from the auxiliary (non-MW) detectors.
     *
     * GRAPHIC and EWL (the auxiliaries the registry builds) run through the
     * polymorphic @c ISlipDetector::detect contract and emit unified
     * @c SlipEvent records.  The MW pipeline owns the WL-lock state, AR
     * coupling and BSSD pivot machinery and can therefore repair in place; the
     * auxiliary detectors are independent cross-checks, and the project's
     * response to one of their detections is a BSSD-aware ambiguity *reset*
     * (see "reset over repair for bias-less signals").  Events for a satellite
     * the MW pipeline already acted on this epoch (present in @p handled_sats)
     * are skipped so they do not override MW's targeted per-signal repairs.
     *
     * @param events        Aggregated auxiliary-detector slip events.
     * @param handled_sats  Satellites already reset/repaired by the MW path
     *                      (updated in place as auxiliary resets are applied).
     * @param modeled_obs   Modeled observations for this epoch.
     * @param state_manager Filter state (mutated on reset).
     * @param meas_model    For BSSD pivot lookup.
     * @param epoch_prefix  Log prefix.
     * @param[in,out] result Epoch result counters (n_reset / state_modified).
     */
    void applyAuxiliarySlipEvents(
        const std::vector<SlipEvent>& events,
        std::set<std::string>& handled_sats,
        const std::vector<PppObservation>& modeled_obs,
        StateManager& state_manager,
        const MeasurementModel& meas_model,
        const std::string& epoch_prefix,
        CycleSlipEpochResult& result);

    /**
     * @brief Apply TurboEdit resolutions for GF-only detections.
     *
     * Satellites the MW pipeline already handled this epoch (in @p handled_sats)
     * are skipped: their dual-frequency culprits were resolved in the MW loop
     * through the TurboEdit-augmented cross-consistency path and the BSSD pivot
     * machinery.
     * This pass covers the remainder: satellites flagged purely by the
     * geometry-free jump (MW's @f$\Delta N_1=\Delta N_2@f$ blind spot, where MW
     * reported nothing).  Confident per-signal integers are applied via
     * @c repairAmbiguityBssdAware (covariance preserved; BSSD pivot refused →
     * reset), and @c TurboEditResolver reset-fallback satellites are reset
     * BSSD-aware across all their observed phase signals.
     *
     * @param te            TurboEdit resolution for this epoch.
     * @param handled_sats  Satellites already handled by the MW path (skipped).
     * @param modeled_obs   Modeled observations for this epoch.
     * @param state_manager Filter state (mutated).
     * @param meas_model    For BSSD pivot lookup.
     * @param epoch_prefix  Log prefix.
     * @param[in,out] result Epoch result counters.
     */
    void applyTurboEditGfOnlyResolutions(
        const TurboEditResolver::EpochResolution& te,
        std::set<std::string>& handled_sats,
        const std::vector<PppObservation>& modeled_obs,
        StateManager& state_manager,
        const MeasurementModel& meas_model,
        const std::string& epoch_prefix,
        CycleSlipEpochResult& result);

    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;

    /// Config-selected detector set (built by @c SlipDetectorRegistry).  Owns
    /// every active detector; run order is MW-first then auxiliaries.
    std::vector<std::unique_ptr<ISlipDetector>> detectors_;

    /// Non-owning typed handle to the MW detector inside @c detectors_ (the
    /// primary detector and source of WL integers for AR / BSSD pivots), or
    /// nullptr when the MW detector is disabled via config.
    MwSlipDetector* mw_{ nullptr };

    /// TurboEdit MW+GF joint-repair resolver, constructed only when
    /// @c estimation.cycle_slip.turbo_edit is set and the MW detector is
    /// enabled (it supplies the wide-lane integers the resolver pairs with the
    /// geometry-free jump).  nullptr otherwise.  See @c TurboEditResolver.
    std::unique_ptr<TurboEditResolver> turbo_;

    /// Effective observation sampling interval (s) forwarded from Application;
    /// passed to the resolver for the high-rate gate.  0 = unknown.
    double sample_interval_s_{ 0.0 };

    /// Cached MW slip results from the last detectAndRepair() call.
    std::map<std::string, MwSlipResult> last_mw_slips_;

    /// Per-epoch slip-action accumulators for the RTS smoother, cleared at the
    /// top of @c detectAndRepair and copied into the returned
    /// @c CycleSlipEpochResult before it returns.  Written by
    /// @c resetAmbiguityBssdAware (reset satellites), @c repairAmbiguityBssdAware
    /// (compensatable repairs), the inline post-handover repair (recorded as a
    /// reset, since it lands in the new SD frame), and the soft-LLI bridge
    /// restore path (reset).
    std::set<std::string> epoch_reset_sats_;
    std::vector<std::pair<std::string, double>> epoch_repairs_;

    /// Shared soft-LLI gap-bridge orchestration (snapshot / hold / restore).
    /// Inert unless @c estimation.cycle_slip.turbo_edit_bridge_lli.  The same
    /// type backs the real-time path so the two cannot diverge.
    SoftLliBridge soft_lli_;
};

} // namespace GnssProcessor
