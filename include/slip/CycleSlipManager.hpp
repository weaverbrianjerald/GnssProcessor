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

#include "slip/CycleSlipCounters.hpp"
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

struct SlipEpochWork;         // detectAndRepair's per-epoch working value (slip/CycleSlipEpochWork.hpp)
struct SatelliteSlipVerdict;  // one satellite's adjudicated mandates (slip/CycleSlipVerdict.hpp)

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
     * @brief Print the run-end slip counters (@c runEndCounters) to the
     *        "cycle_slip_manager" log, when at least one epoch was processed.
     *
     * Called once by the post-processing driver at run end, beside the
     * KalmanFilter's QC counters; the table is the slip layer's run-end
     * telemetry (standing invariant 4).
     */
    void logRunEndCounters() const;

    /**
     * @brief The slip layer's counters accumulated over the run so far.
     * @return The counters; one detection row per detector the registry built.
     */
    const CycleSlipCounters& runEndCounters() const { return counters_; }

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
     * The body is a short ordered driver over the private section members
     * declared below (S0 to S17), which share one @c SlipEpochWork.
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
    // =======================================================================
    // detectAndRepair, section by section
    // =======================================================================
    // detectAndRepair (CycleSlipManager.cpp) calls these in this order, over one
    // SlipEpochWork.  S3 and S10 declare working sets only, so they are
    // members of SlipEpochWork rather than functions.  S8 to S14 run only when
    // the MW detector reported anything this epoch; S11 runs once per
    // satellite with a mandate.
    //
    //   S0   beginSlipEpoch                       CycleSlipManager.cpp
    //   S1   registerSoftLliGapHolds              CycleSlipDetection.cpp
    //   S2   runAuxiliaryDetectors                CycleSlipDetection.cpp
    //   S4   detectMwSlips                        CycleSlipDetection.cpp
    //   S5   resolveTurboEdit                     CycleSlipDetection.cpp
    //   S6   resolveSoftLliHolds                  CycleSlipDetection.cpp
    //   S7   groupMwPairsBySatellite              CycleSlipDetection.cpp
    //   S8   collectMwMandates                    CycleSlipDetection.cpp
    //   S9   scanPivotCulprits                    CycleSlipPivot.cpp
    //   S11  handleSatelliteMandates              CycleSlipAdjudication.cpp
    //          adjudicateSatellite (the verdict), then one action:
    //          applyCulpritVerdict | applySuppressionVerdict |
    //          resetEveryMandate + holdOrResetLliArcs + reinflateSatelliteStates
    //   S12  repairAcrossSlippedPivotInPlace      CycleSlipPivot.cpp
    //   S13  forcePivotHandoverAndApplyDeferred   CycleSlipPivot.cpp
    //   S14  logMwEpochSummary                    CycleSlipAdjudication.cpp
    //   S15  applyTurboEditGfOnlyResolutions      CycleSlipNonMwPasses.cpp
    //   S16  applyAuxiliarySlipEvents             CycleSlipNonMwPasses.cpp
    //        tallySlipEpoch (run-end counters)    CycleSlipManager.cpp
    //   S17  finishSlipEpoch                      CycleSlipManager.cpp
    //
    // The state primitives every response goes through (resetAmbiguityBssdAware,
    // repairAmbiguityBssdAware) are in CycleSlipRepair.cpp.

    /**
     * @brief S0: clear the per-epoch accumulators (@c last_mw_slips_,
     *        @c epoch_reset_sats_, @c epoch_repairs_), take the epoch time from
     *        the first observation and build the BSSD pivot predicate.
     * @param w Epoch work; writes @c epoch_now_s and @c is_bssd_pivot.
     */
    void beginSlipEpoch(SlipEpochWork& w);

    /**
     * @brief S1: hold the ambiguities of phase signals that dropped out this
     *        epoch with a recent converged snapshot, so the TurboEdit gap
     *        bridge can adjudicate the outage on the signal's return.
     *
     * No-op unless TurboEdit and @c turbo_edit_bridge_lli are on.
     *
     * @param w Epoch work; reads the observations, the state and the pivot
     *          predicate (the holds live in @c soft_lli_).
     */
    void registerSoftLliGapHolds(SlipEpochWork& w);

    /**
     * @brief S2: run the auxiliary detectors (GRAPHIC, EWL) through
     *        @c ISlipDetector::detect and gather their events.
     *
     * The MW detector is skipped here; S4 drives it through its concrete API.
     * The events are applied after the MW path, by S16.
     *
     * @param w Epoch work; writes @c aux_events.
     */
    void runAuxiliaryDetectors(SlipEpochWork& w);

    /**
     * @brief S4: run the MW / LLI detector
     *        (@c MwSlipDetector::detectLargeMwResets).
     * @param w Epoch work; writes @c slip_results (empty when the MW detector
     *          is disabled).
     */
    void detectMwSlips(SlipEpochWork& w);

    /**
     * @brief S5: the TurboEdit MW+GF joint resolution of this epoch: per-signal
     *        culprits and integers, the no-state-action adjudications (code
     *        events, bridged moot, adjudicated clean) and the WL-settle holds.
     *
     * No-op unless TurboEdit is on; the resolver also rolls its GF arcs
     * forward, so it runs on epochs without MW slips too.
     *
     * @param w Epoch work; writes @c te, @c te_culprits_by_sat,
     *          @c te_slips_by_sat and @c te_suppress_by_sat.
     */
    void resolveTurboEdit(SlipEpochWork& w);

    /**
     * @brief S6: resolve the outstanding soft-LLI holds against this epoch's
     *        bridge output (restore with @c snapshot + dN·λ, reset, or time
     *        out).
     *
     * No-op unless TurboEdit is on and a hold is outstanding.
     *
     * @param w Epoch work; writes @c soft_lli_repaired (S11 skips those
     *          satellites), @c reset_sats and the result counters.
     */
    void resolveSoftLliHolds(SlipEpochWork& w);

    /**
     * @brief S7: group the MW results per satellite: the slipped pairs, and
     *        the slipped plus clean pairs (clean ones synthesized from the
     *        detector's mature arcs) that cross-consistency compares.
     * @param w Epoch work; writes @c slips_by_sat and @c all_pairs_by_sat.
     */
    void groupMwPairsBySatellite(SlipEpochWork& w) const;

    /**
     * @brief S8: the mandated ambiguity keys per satellite, the satellites
     *        with a receiver loss of lock, and the per-satellite status cache
     *        @c last_mw_slips_ (the satellite_status trace's "Slip" rows).
     * @param w Epoch work; writes @c reset_keys_by_sat and @c lli_sats.
     */
    void collectMwMandates(SlipEpochWork& w);

    /**
     * @brief S9: find the (constellation, signal) pairs whose BSSD pivot is
     *        itself a culprit; every satellite's repair on such a signal is
     *        then deferred to S12 or S13.  No-op without BSSD.
     * @param w Epoch work; writes @c pivot_affected_cons_sig.
     */
    void scanPivotCulprits(SlipEpochWork& w) const;

    /**
     * @brief S11: one satellite's MW mandates: the verdict
     *        (@c adjudicateSatellite), then the one action it selects.
     *
     * Skip verdicts do nothing; @c RepairCulprits runs
     * @c applyCulpritVerdict; @c SuppressMandates runs
     * @c applySuppressionVerdict; @c FullReset runs @c resetEveryMandate,
     * @c holdOrResetLliArcs and @c reinflateSatelliteStates in that order.
     *
     * @param w        Epoch work; the actions write the state, the result
     *                 counters, @c reset_sats, @c epoch_reset_amb_keys and the
     *                 deferred queues.
     * @param sat_id   Satellite, e.g. "G05".
     * @param amb_keys The satellite's mandated ambiguity keys.
     */
    void handleSatelliteMandates(SlipEpochWork& w,
                                 const std::string& sat_id,
                                 const std::set<std::string>& amb_keys);

    /**
     * @brief S11, the verdict: decide what to do with one satellite's
     *        mandates, without touching the filter state.
     *
     * In order: a satellite the soft-LLI bridge already repaired, or whose
     * TurboEdit WL-settle deferral is pending, is skipped; the culprit signals
     * come from cross-consistency (two or more pairs, one clean), else from the
     * TurboEdit split (never on a loss of lock); a culprit set that contradicts
     * a code-event adjudication is withdrawn with it; culprits select
     * @c RepairCulprits (with the signals the analysis adjudicated), else a
     * TurboEdit no-state-action adjudication selects @c SuppressMandates, else
     * @c FullReset.  Logs its reasoning (the pending skip, the TurboEdit split,
     * the contradiction).
     *
     * @param w      Epoch work (the evidence; @c all_pairs_by_sat and
     *               @c te_slips_by_sat are read through @c operator[]).
     * @param sat_id Satellite, e.g. "G05".
     * @return The verdict.
     */
    SatelliteSlipVerdict adjudicateSatellite(SlipEpochWork& w,
                                             const std::string& sat_id) const;

    /**
     * @brief S11, action @c RepairCulprits: repair each culprit with a known
     *        integer (covariance preserved), reset the others, defer the
     *        pivot-affected ones to S12 / S13, then reset every mandate on a
     *        signal the analysis did not adjudicate.
     * @param w        Epoch work; writes the state, the result counters,
     *                 @c reset_sats, @c epoch_reset_amb_keys and the deferred
     *                 queues.
     * @param sat_id   Satellite.
     * @param amb_keys The satellite's mandated ambiguity keys.
     * @param verdict  Its verdict (culprits, known integers, adjudicated set).
     */
    void applyCulpritVerdict(SlipEpochWork& w,
                             const std::string& sat_id,
                             const std::set<std::string>& amb_keys,
                             const SatelliteSlipVerdict& verdict);

    /**
     * @brief S11, action @c SuppressMandates: leave the TurboEdit-adjudicated
     *        signals untouched and reset only the mandates outside the
     *        adjudication.  Counts the satellite as a code-event veto; marks it
     *        handled only when a reset happened.
     * @param w        Epoch work; writes the state, the result counters,
     *                 @c reset_sats and @c epoch_reset_amb_keys.
     * @param sat_id   Satellite.
     * @param amb_keys The satellite's mandated ambiguity keys.
     * @param verdict  Its verdict (the suppressed signals).
     */
    void applySuppressionVerdict(SlipEpochWork& w,
                                 const std::string& sat_id,
                                 const std::set<std::string>& amb_keys,
                                 const SatelliteSlipVerdict& verdict);

    /**
     * @brief S11, action @c FullReset, first part: reset every mandated key of
     *        the satellite (BSSD-aware) and mark it handled.
     * @param w        Epoch work; writes the state, the result counters,
     *                 @c reset_sats and @c epoch_reset_amb_keys.
     * @param sat_id   Satellite.
     * @param amb_keys The satellite's mandated ambiguity keys.
     * @param verdict  Its verdict (the pair counts of the debug line).
     */
    void resetEveryMandate(SlipEpochWork& w,
                           const std::string& sat_id,
                           const std::set<std::string>& amb_keys,
                           const SatelliteSlipVerdict& verdict);

    /**
     * @brief S11, action @c FullReset, second part: on a receiver loss of lock
     *        with TurboEdit on, hold the satellite for a soft-LLI bridge repair
     *        (GF arc kept) when it qualifies and is not the BSSD pivot, else
     *        reset its TurboEdit GF arcs.  No-op otherwise.
     * @param w        Epoch work (the pivot map and the epoch time).
     * @param sat_id   Satellite.
     * @param amb_keys The satellite's mandated ambiguity keys (the hold's keys).
     * @param verdict  Its verdict (the loss-of-lock flag).
     */
    void holdOrResetLliArcs(SlipEpochWork& w,
                            const std::string& sat_id,
                            const std::set<std::string>& amb_keys,
                            const SatelliteSlipVerdict& verdict);

    /**
     * @brief S11, action @c FullReset, third part: re-inflate the satellite's
     *        slant-ionosphere (and rate) and satellite-bias variances
     *        (@c estimation.slip_state_reinflation), except on the BSSD pivot.
     * @param w      Epoch work; writes the state.
     * @param sat_id Satellite.
     */
    void reinflateSatelliteStates(SlipEpochWork& w, const std::string& sat_id);

    /**
     * @brief Whether a satellite is affirmatively clean on a signal this
     *        epoch: a mature MW arc involves the signal and the signal is not
     *        among its culprits.  A receiver loss of lock never counts.
     * @param w   Epoch work (read only).
     * @param sat Satellite, e.g. "G07".
     * @param sig Signal, e.g. "L2W".
     * @return true if the satellite may take the lossless -dN_pivot shift of
     *         S12; false if it must be reset instead.
     */
    bool confidentlyCleanOnSignal(const SlipEpochWork& w,
                                  const std::string& sat,
                                  const std::string& sig) const;

    /**
     * @brief S12: repair the SD frame in place for every pivot-affected
     *        constellation whose pivot integer is known, then drain those
     *        constellations from the deferred queues and the pivot-affected
     *        set, so S13 sees only what still needs a handover.
     *
     * No-op without BSSD or without a deferred integer repair.
     *
     * @param w Epoch work; writes the state and the deferred queues.
     */
    void repairAcrossSlippedPivotInPlace(SlipEpochWork& w);

    /**
     * @brief S12 for one constellation: when the pivot's own integer is known
     *        on every pivot-affected signal, shift each flagged peer's SD
     *        ambiguity by dN_peer - dN_pivot, each confidently clean peer by
     *        -dN_pivot, reset the unverifiable peers, and keep the pivot.
     * @param w          Epoch work; writes the state and the result.
     * @param cons       Constellation name (as @c PppUtils::getSystemString).
     * @param pivots_now The BSSD pivot per constellation.
     * @return true if the constellation was handled in place (it then needs
     *         no handover); false if its pivot integer is not known on every
     *         pivot-affected signal or it has no pivot.
     */
    bool repairConstellationInPlace(SlipEpochWork& w,
                                    const std::string& cons,
                                    const std::map<std::string, std::string>& pivots_now);

    /**
     * @brief The satellites a forced handover may not elect as the new pivot:
     *        every satellite with slip evidence this epoch (MW pair slips,
     *        receiver loss of lock, TurboEdit culprits and unresolved
     *        satellites).
     * @param w Epoch work (read only).
     * @return The exclude set passed to @c PivotManager::handlePivotHandover.
     */
    std::set<std::string> handoverExcludeSet(const SlipEpochWork& w) const;

    /**
     * @brief S13: force a pivot handover on the constellations still
     *        pivot-affected, then apply the deferred integer repairs in the
     *        new frame (a reset where the handover refused) and the deferred
     *        resets.
     *
     * No-op when both deferred queues are empty.  Writes the measurement
     * model's @c PivotManager through a @c const_cast (the caller owns it as
     * non-const).  The post-handover repair is recorded for the smoother as a
     * reset (segmented), not as a compensated repair.
     *
     * @param w Epoch work; writes the state.
     */
    void forcePivotHandoverAndApplyDeferred(SlipEpochWork& w);

    /**
     * @brief S14: the per-epoch MW warn line (targeted, full-reset and
     *        code-event counts), printed only when the MW path acted.
     *
     * Runs before S15 and S16, so their repairs and resets are not in it.
     *
     * @param w Epoch work (read only).
     */
    void logMwEpochSummary(const SlipEpochWork& w) const;

    /**
     * @brief S15: apply TurboEdit resolutions for GF-only detections.
     *
     * Satellites the MW pipeline already handled this epoch (in
     * @c w.reset_sats) are skipped: their dual-frequency culprits were resolved
     * in the MW loop through the TurboEdit-augmented cross-consistency path
     * and the BSSD pivot machinery.  This pass covers the remainder:
     * satellites flagged purely by the geometry-free jump (MW's
     * @f$\Delta N_1=\Delta N_2@f$ blind spot, where MW reported nothing).
     * Confident per-signal integers are applied via
     * @c repairAmbiguityBssdAware (covariance preserved; BSSD pivot refused,
     * then reset), and @c TurboEditResolver reset-fallback satellites are
     * reset BSSD-aware across all their observed phase signals.  No-op unless
     * TurboEdit is on.
     *
     * @param w Epoch work; reads @c te, writes the state, the result counters
     *          and @c reset_sats (the satellites this pass handled).
     */
    void applyTurboEditGfOnlyResolutions(SlipEpochWork& w);

    /**
     * @brief S16: apply slip events from the auxiliary (non-MW) detectors.
     *
     * GRAPHIC and EWL (the auxiliaries the registry builds) run through the
     * polymorphic @c ISlipDetector::detect contract (S2) and emit unified
     * @c SlipEvent records.  The MW pipeline owns the WL-lock state, AR
     * coupling and BSSD pivot machinery and can therefore repair in place; the
     * auxiliary detectors are independent cross-checks, and the project's
     * response to one of their detections is a BSSD-aware ambiguity *reset*
     * (see "reset over repair for bias-less signals").  Events for a satellite
     * already acted on this epoch (in @c w.reset_sats) are skipped so they do
     * not override MW's targeted per-signal repairs.  No-op without events.
     *
     * @param w Epoch work; reads @c aux_events, writes the state, the result
     *          counters and @c reset_sats.
     */
    void applyAuxiliarySlipEvents(SlipEpochWork& w);

    /**
     * @brief Add this epoch's evidence to the run-end counters: the epoch, the
     *        detections per detector, the MW mandates, the soft-LLI bridge
     *        repairs and the covariance-preserving repairs recorded for the
     *        smoother.  The other counters are incremented where their event
     *        happens (the reset primitive, S11, S12, S13).
     * @param w Epoch work (read only).
     */
    void tallySlipEpoch(const SlipEpochWork& w);

    /**
     * @brief S17: advance the soft-LLI epoch marker and hand the smoother
     *        accumulators (@c epoch_reset_sats_, @c epoch_repairs_) to the
     *        result.
     * @param w Epoch work.
     * @return The epoch's result.
     */
    CycleSlipEpochResult finishSlipEpoch(SlipEpochWork& w);

    // =======================================================================
    // State primitives (CycleSlipRepair.cpp) and culprit analysis
    // (CycleSlipAdjudication.cpp)
    // =======================================================================

    /**
     * @brief Reset a single ambiguity with BSSD awareness.
     *
     * Without BSSD, or without a pivot for the satellite's constellation, a
     * plain @c StateManager::resetAmbiguity.  With BSSD: the pivot's variance
     * is inflated so @c PivotManager hands it over (its value stays 0); a
     * non-pivot ambiguity is reseeded as the SD value
     * seed(sat) - seed(pivot, same band) when both are observed, else its
     * variance is inflated and the reseed deferred to the next epoch.  Every
     * branch records the satellite in @c epoch_reset_sats_ (smoother
     * segmentation).
     *
     * @param amb_key       Ambiguity key, e.g. "G05_L2W".
     * @param sat_id        Satellite of the key.
     * @param signal        Signal of the key.
     * @param modeled_obs   Modeled observations of this epoch (seeds).
     * @param state_manager Filter state (modified).
     * @param meas_model    BSSD pivot map.
     * @param epoch_prefix  Log prefix.
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
     * @brief Satellite of an ambiguity key ("G01_L1C" -> "G01"); the whole key
     *        when it has no underscore.
     * @param key Ambiguity key.
     * @return The satellite identifier.
     */
    static std::string satIdFromAmbiguityKey(const std::string& key);

    /**
     * @brief Signal of an ambiguity key ("G01_L1C" -> "L1C"); empty when the
     *        key has no underscore.
     * @param key Ambiguity key.
     * @return The signal identifier.
     */
    static std::string signalFromAmbiguityKey(const std::string& key);

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

    /// Per-epoch slip-action accumulators for the RTS smoother, cleared by
    /// @c beginSlipEpoch (S0) and copied into the returned
    /// @c CycleSlipEpochResult by @c finishSlipEpoch (S17).  Written by
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

    /// Run-end counters (@c runEndCounters), printed by @ref logRunEndCounters.
    CycleSlipCounters counters_;
};

} // namespace GnssProcessor
