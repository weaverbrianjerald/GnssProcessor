#pragma once

/**
* @file CycleSlipManager.hpp
* @brief Cycle-slip detection and reset coordinator over a config-selected
*        set of @c ISlipDetector instances.
*
* CycleSlipManager owns the detector set built by @c SlipDetectorRegistry
* from @c GnssConfig::getCycleSlipConfig and coordinates cycle slip
* detection, multi-frequency signal identification, and ambiguity reset.
* The Melbourne-Wübbena detector (when enabled) is the primary: it drives the
* specialized cross-consistency + BSSD-aware repair/handover pipeline and is
* the source of WL integers for AR and the BSSD pivot manager.  Auxiliary
* detectors (GRAPHIC, EWL, and — once compiled — GF / IF) run through the
* uniform polymorphic contract and trigger BSSD-aware resets.
*
* Design:
*   - Owns MwSlipDetector.
*   - detectAndRepair() is the single entry point called per epoch.
*   - The MwSlipDetector forms *all* dual-frequency MW pairs for each
*     satellite (L1-L2, L1-L5, L2-L5, etc.), including EWL pairs with
*     very long wavelengths (~5-10 m).  This provides unified slip
*     detection across all frequency pairs.
*   - Multi-frequency MW cross-consistency: when 3+ signals are
*     available, overlapping MW pairs are compared.  If L1W_L2W slips
*     but L1W_L5Q does not, then L2W slipped and only that ambiguity
*     is reset (rather than resetting all ambiguities for the satellite).
*   - For bias-corrected locked arcs, the MW arc is shifted by the
*     integer slip offset to maintain continuity.
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
#include <vector>

namespace GnssProcessor {

/**
 * @brief Epoch-level result of cycle slip processing.
 */
struct CycleSlipEpochResult {
    int n_repaired{ 0 };       ///< Slips handled by targeted single-signal reset.
    int n_reset{ 0 };          ///< Slips handled by full ambiguity reset.
    /// SATELLITES whose MW mandates were suppressed via the dedicated
    /// code-event veto branch (GF affirmatively stable; no state touched
    /// for the adjudicated signals).  Counted per satellite, not per pair,
    /// and only for the veto branch — a code-event sat that the culprit
    /// branch handled via clean_signals adjudication is not counted here.
    /// See TurboEditResolver's "code-domain veto" header note.
    int n_code_events{ 0 };
    bool state_modified{ false }; ///< True if any filter state was changed.
};

/**
* @class CycleSlipManager
* @brief Coordinates MW-only cycle slip detection, multi-frequency
*        signal identification, and ambiguity reset.
*
* Design:
*   Pre-estimation:
*     1. MW detector forms all dual-frequency MW pairs (L1-L2, L1-L5,
*        L2-L5, etc.) and flags WL slips with integer sizing.  For
*        pairs with satellite BIA biases, the MW is fixed to integer;
*        otherwise, the regular Hatch filter is used.
*     2. Multi-frequency cross-consistency: when a satellite has 3+
*        signals, overlapping MW pairs are compared to isolate which
*        individual signal slipped, enabling targeted single-signal
*        ambiguity reset instead of resetting all signals.
*     3. Unidentified slips get a full ambiguity reset.
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
     * @brief Pre-estimation: MW-only cycle slip detection and reset.
     *
     * Pipeline:
     *   1. Run MW/LLI detector to identify WL slips.
     *   2. Multi-frequency cross-consistency to isolate culprit signals.
     *   3. Fall through to full reset for unidentified slips.
     *
     * @param epoch_prefix     Log prefix.
     * @param modeled_obs      Modeled observations for this epoch.
     * @param state_manager    Filter state manager (modified on reset).
     * @param meas_model       Measurement model (for BSSD pivot info).
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

    /// Reset satellite state in all detectors.
    void resetSatellite(const std::string& satellite);

    /// Reset MW arc state for a specific (satellite, signal) only.  Arcs on
    /// signal pairs that don't involve the specified signal are preserved.
    /// Use when the upstream caller knows precisely which signal slipped
    /// (e.g. via the KF's per-(sat, signal) outlier dispatch).
    void resetSatelliteSignal(const std::string& satellite,
                              const std::string& signal);

    /// Forward a one-shot MW-pivot deprioritization hint to the MW detector.
    /// Call this when an upstream component (typically @c PivotManager via
    /// @c requestForcedHandover) flags that the current KF pivot has slipped,
    /// so the MW pivot picker also avoids the same satellite next epoch.
    /// See @c MwSlipDetector::notifyPivotForcedHandover for semantics.
    void notifyPivotForcedHandover(const std::string& satellite,
                                   const std::string& reason);

    /// Provide bias corrections to the MW detector (and the TurboEdit resolver,
    /// if enabled).
    void setCorrections(const GnssCorrections* corrections);

    /// Enable MW trace logging.
    bool enableMwTracing(const std::filesystem::path& log_dir);

    /// Enable the TurboEdit per-pair joint-solve trace (no-op if turbo_edit
    /// is disabled).  Returns true if the trace file was opened.
    bool enableTurboEditTracing(const std::filesystem::path& log_dir);

    /// Provide the effective observation sampling interval (s) used to gate the
    /// high-rate-only TurboEdit GF repair (warns once if coarser than 1 s).
    /// Also tightens the TurboEdit GF-arc gap tolerance to the cadence.
    void setSampleInterval(double interval_s);

    /// Whether the MW+GF TurboEdit joint repair mode is active (config toggle
    /// set AND the MW detector — its source of WL integers — is enabled).
    bool turboEditEnabled() const { return turbo_ != nullptr; }

    /// Access the MW detector for its rich API (WL-lock queries, per-sat
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
     * @brief Record a rolling "last good" snapshot of every ambiguity value /
     *        variance — the pre-gap anchor for the soft-LLI bridge repair.
     *
     * Call once per epoch AFTER the Kalman measurement update (so it captures
     * the converged post-update values).  Only ambiguities whose variance is
     * below a converged threshold update the snapshot, so a fresh reset /
     * pending-reseed does NOT overwrite the last genuinely-converged value —
     * which is exactly the anchor a later soft-LLI hold needs (the value from
     * the epoch BEFORE the signal dropped).  No-op unless
     * @c estimation.cycle_slip.turbo_edit_bridge_lli is set.
     *
     * @param state_manager Filter state to snapshot from.
     * @param epoch_now_s   Continuous GPS seconds of this epoch (recency stamp).
     */
    void recordAmbiguitySnapshot(const StateManager& state_manager, double epoch_now_s);

    /**
     * @brief Phase-measurement keys ("G03_L2W") currently HELD across a soft
     *        LLI gap pending a TurboEdit bridge repair.
     *
     * The caller (@c Application / @c RealTimeEngine) seeds the Kalman update's
     * @c excluded_measurements with these so the held — still-slipped —
     * ambiguities never contaminate shared states across the MW one-epoch
     * confirmation delay.  Empty unless @c turbo_edit_bridge_lli.
     */
    std::set<std::string> getHeldPendingBridgeKeys() const;

    /**
     * @brief Drop all soft-LLI / gap-bridge hold and snapshot state.
     *
     * Call alongside any HARD filter reset that deletes ambiguity states
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
     * GRAPHIC / EWL (and, once compiled, GF / IF) run through the polymorphic
     * @c ISlipDetector::detect contract and emit unified @c SlipEvent records.
     * Unlike the MW pipeline — which owns the WL-lock state, AR coupling, and
     * BSSD pivot machinery and so can repair in place — the auxiliary
     * detectors are independent cross-checks; the safe, in-house-convention
     * response to one of their detections is a BSSD-aware ambiguity *reset*
     * (see "reset over repair for bias-less signals").  Events for a satellite
     * the MW pipeline already acted on this epoch (present in @p handled_sats)
     * are skipped to avoid fighting MW's targeted per-signal repairs.
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
     * are skipped — their dual-frequency culprits were resolved in-loop via the
     * TurboEdit-augmented cross-consistency path and the BSSD pivot machinery.
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

    /// Shared soft-LLI gap-bridge orchestration (snapshot / hold / restore).
    /// Inert unless @c estimation.cycle_slip.turbo_edit_bridge_lli.  The same
    /// type backs the real-time path so the two cannot diverge.
    SoftLliBridge soft_lli_;
};

} // namespace GnssProcessor
