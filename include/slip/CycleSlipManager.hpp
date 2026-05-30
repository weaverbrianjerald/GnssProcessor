#pragma once

/**
* @file CycleSlipManager.hpp
* @brief Unified MW-only cycle slip detection and reset coordinator.
*
* CycleSlipManager owns the MwSlipDetector and coordinates cycle slip
* detection, multi-frequency signal identification, and ambiguity reset.
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

#include "slip/MwSlipDetector.hpp"
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

    /// Provide bias corrections to the MW detector.
    void setCorrections(const GnssCorrections* corrections);

    /// Enable MW trace logging.
    bool enableMwTracing(const std::filesystem::path& log_dir);

    /// Access the MW detector (for external queries).
    MwSlipDetector& mwDetector() { return mw_detector_; }
    const MwSlipDetector& mwDetector() const { return mw_detector_; }

    /**
     * @brief Cache the last MW slip results from detectAndRepair().
     * @return Map from satellite ID to its MwSlipResult.
     */
    const std::map<std::string, MwSlipResult>& getLastMwSlips() const { return last_mw_slips_; }

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

    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;

    MwSlipDetector mw_detector_;

    /// Cached MW slip results from the last detectAndRepair() call.
    std::map<std::string, MwSlipResult> last_mw_slips_;
};

} // namespace GnssProcessor
