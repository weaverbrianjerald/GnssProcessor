#pragma once






#pragma once

/**
 * @file PivotManager.hpp
 * @brief Between-satellite single-difference pivot selection and handover.
 *
 * PivotManager encapsulates all BSSD pivot satellite management:
 *   - Sticky pivot selection with elevation hysteresis.
 *   - Pivot disturbance detection (cycle slip, outlier corruption).
 *   - Lossless T·P·T' re-referencing of ambiguity, ionosphere, and
 *     satellite bias states during pivot handovers.
 *   - Pivot covariance freezing after handover.
 *
 * The pivot map is shared with MeasurementModel via a const accessor
 * so the S-transformation can zero the correct H-columns.
 */

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @class PivotManager
 * @brief Manages BSSD pivot satellite selection, handover, and state re-referencing.
 */
class PivotManager {
public:
    /**
     * @brief Construct the manager.
     * @param config        Processing configuration.
     * @param log_manager   Logging facility.
     */
    PivotManager(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Run pivot selection and handover for one epoch.
     *
     * Sticky pivot selection: the current pivot is kept while it remains
     * above the configured elevation threshold, is not excluded, has no
     * outlier-stale broadcast ephemeris, and is undisturbed.  When a new
     * pivot is needed (disturbance, low elevation, exclusion, first epoch),
     * candidates are ranked by lexicographic (coverage, score) ordering:
     * full signal coverage is a hard tier above any partial-coverage
     * candidate, and within a tier candidates rank by float-ambiguity
     * convergence and elevation.
     *
     * @param observations   Modeled observations for this epoch.
     * @param state_manager  Filter state (modified on handover).
     * @param epoch_prefix   Log prefix string.
     * @param forced_only    When true, the implicit @c isPivotDisturbed
     *                       branch is skipped and only constellations
     *                       with a pending @c requestForcedHandover are
     *                       handed over.  Used by the same-epoch re-entry
     *                       path so that a rebuild pass after a
     *                       KF-detected pivot slip applies the handover
     *                       before the rebuild's measurement update.  The
     *                       default (first-pass behaviour) runs both the
     *                       forced and the disturbance-detection paths.
     */
    void handlePivotHandover(
        const std::vector<PppObservation>& observations,
        StateManager& state_manager,
        const std::string& epoch_prefix,
        bool forced_only = false,
        const std::set<std::string>& exclude_sats = {});

    /**
     * @brief Whether any constellation has a pending forced-handover
     *        request queued via @c requestForcedHandover.
     *
     * Used by MeasurementModel's same-epoch re-entry gate: when a
     * rebuild pass discovers a forced request, the gate fires
     * @c handlePivotHandover with @c forced_only=true, letting the
     * handover happen in the same epoch as the post-fit detection
     * instead of being deferred to the next epoch.
     */
    bool hasPendingForcedHandover() const {
        return !forced_handover_requests_.empty();
    }

    /**
     * @brief Request a forced pivot handover for a single constellation on
     *        the next call to @c handlePivotHandover.
     *
     * Used by external detectors (the KF post-fit residual analyzer) to
     * signal that the current pivot's observation has slipped while its
     * filter state is intact, so @c isPivotDisturbed would not flag it.
     * On the next @c handlePivotHandover call, the disturbance branch is
     * entered for the requested constellations as if @c isPivotDisturbed
     * had returned true; the lossless re-referencing logic that handles
     * state-disturbed pivots picks a replacement and updates state.  The
     * request is consumed (cleared) when @c handlePivotHandover processes
     * the constellation, so the detector re-issues it each epoch it still
     * wants a handover.
     *
     * Idempotent: repeated requests for the same constellation collapse to
     * one handover.  Repeated requests accumulate their @p exclude_sats
     * (set union), so each rebuild pass can add newly flagged satellites to
     * the exclusion.
     *
     * @param constellation Constellation name (e.g. "GPS", "Galileo").
     * @param reason        Free-text reason logged at warn level.
     * @param exclude_sats  Satellites that are not to be elected as the
     *                      replacement pivot for this constellation (the
     *                      offset pivot plus the post-fit detector's
     *                      within-group residual outliers).  Stashed and
     *                      unioned into @c handlePivotHandover's effective
     *                      exclusion so the forced handover lands on a
     *                      consensus satellite instead of another offset
     *                      satellite.  Consumed with the request.
     * @param reinflate_sats Non-pivot satellites whose ionosphere / sat-bias
     *                      state absorbed a coherent pivot-side code bias.
     *                      Their covariance is re-inflated after the lossless
     *                      re-reference, because the re-reference conserves the
     *                      filter's information and cannot remove the absorbed
     *                      offset by itself.  This is the code-only analogue of
     *                      the phase path's @c was_disturbed re-seed.  Unioned
     *                      per constellation and consumed with the request.
     */
    void requestForcedHandover(const std::string& constellation,
                                const std::string& reason,
                                const std::set<std::string>& exclude_sats = {},
                                const std::set<std::string>& reinflate_sats = {});

    /**
     * @brief Freeze pivot ambiguity covariances to zero.
     *
     * Must be called after ensureRequiredStates() so that newly created
     * pivot ambiguity states are frozen.
     */
    void freezePivotAmbiguityCovariance(StateManager& state_manager);

    /**
     * @brief Freeze pivot ionosphere and satellite bias covariances to zero.
     *
     * Freezes per-pivot ionosphere states and any per-pivot satellite bias
     * states.  Also informs StateManager which iono satellites are frozen
     * so that process noise is suppressed.
     */
    void freezePivotIonoAndBiasCovariance(StateManager& state_manager);

    /// Get the current pivot satellite map (constellation name -> satellite ID).
    const std::map<std::string, std::string>& getPivotSatellites() const {
        return pivot_sat_;
    }

    /**
     * @brief Clean (elevation / quality) pivot handovers performed this epoch.
     *
     * Maps constellation name -> {old_pivot, new_pivot} for handovers that were
     * pure datum changes on an undisturbed, continuous old pivot, that is,
     * neither forced (KF post-fit / slip-phase) nor triggered by a disturbed
     * pivot.  For these handovers the SD re-reference is a lossless, invertible
     * frame change, so the RTS smoother can inverse-reference the affected
     * ambiguity states back to the old pivot instead of segmenting them
     * (audit P2-1).
     *
     * Disturbed or forced handovers (the old pivot's own ambiguity slipped and
     * is discontinuous across the epoch) are not recorded here, so the smoother
     * segments at them.
     *
     * Reset at the start of each epoch's first-pass handover
     * (@c forced_only == false) and populated as clean legs fire; read by
     * @c Application at posterior-snapshot time.
     */
    const std::map<std::string, std::pair<std::string, std::string>>&
    getCleanHandovers() const {
        return clean_handovers_;
    }

    /// Check if BSSD pivots have been initialized.
    bool hasPivots() const { return !pivot_sat_.empty(); }

    /**
     * @brief Forget all sticky pivots and unfreeze sat-specific states.
     *
     * Used by @c Application::handleGap on long-gap detection so the
     * post-gap epoch picks fresh pivots instead of re-referencing
     * against stale pre-gap incumbents.  After this call:
     *   - @c pivot_sat_ is empty (the next @c handlePivotHandover treats
     *     the epoch as a fresh BSSD start)
     *   - @c forced_handover_requests_ and its companion exclude and
     *     re-inflate sets are dropped (they refer to pre-gap
     *     constellations that may no longer exist), and the per-epoch
     *     retired-pivot and clean-handover maps are cleared
     *   - @c StateManager's frozen-pivot set is cleared so iono and
     *     sat-bias process noise applies to all satellites again
     *
     * The iono / sat-bias / ambiguity state values of the old pivot are
     * not modified here; they remain at value=0 with variance=0 (the
     * frozen-pivot signature).  The caller (handleGap) is expected to drop
     * those states via the relevant
     * @c StateManager::pruneXxxStatesExcept({}) calls before the
     * next ensureRequiredStates() pass rebuilds them around the
     * newly-elected pivots.
     *
     * @param state_manager  Filter state (frozen-pivot set is cleared).
     */
    void dropAllPivots(StateManager& state_manager);

private:
    // -------------------------------------------------------------------
    // Per-constellation epoch snapshot (built once per handlePivotHandover)
    // -------------------------------------------------------------------

    /** Per-satellite observation snapshot used during pivot selection. */
    struct SatInfo {
        std::string constellation;   ///< e.g. "GPS"
        double      elevation{0.0};
        size_t      phase_sig_count{0}; ///< Number of distinct phase signal types (L1W, L2W, L5X, etc.).
        /// Worst-case float-ambiguity variance (m²) across this sat's
        /// signals.  Negative sentinel means no ambiguity state yet
        /// (first BSSD epoch, freshly appeared sat).
        double      max_amb_variance{-1.0};
        /// Signed broadcast-ephemeris age (s, obs epoch − toe) for this sat;
        /// |value| is the extrapolation magnitude.  0 in post-processing (no
        /// live ephemeris).  The selector bars a satellite from pivot election
        /// when its |age| exceeds its constellation's freshest qualified peer by
        /// more than kPivotToeSpreadMaxS (1200 s, in PivotManager.cpp).
        double      eph_toe_age_s{0.0};
    };

    /** Per-constellation aggregated info for the current epoch. */
    struct ConstellationInfo {
        std::string best_sat;      ///< Best candidate (max coverage, score tie-break).
        double      best_score{-1e9};
        size_t      best_coverage{0}; ///< Coverage of @c best_sat (lex ordering with score).
        std::string highest_sat;   ///< Satellite with highest elevation (defensive fallback).
        double      highest_el{-1.0};
        size_t      max_phase_sigs{0}; ///< Maximum phase signal count across all satellites.
    };

    /**
     * @brief Build per-satellite and per-constellation maps from observations.
     *
     * Single pass over observations.  Skips GLONASS satellites unless
     * @c enable_glonass_bssd is set.  Reads each satellite's worst-case
     * float-ambiguity variance from @p state_manager so the scoring step
     * can prefer pivots whose filter states are well-converged (the new
     * pivot's variance propagates into every same-group SD state via the
     * T·P·T' re-reference).
     *
     * For each constellation the best candidate is selected by
     * lexicographic ordering on (coverage, score): full-coverage
     * candidates rank above any partial-coverage candidate regardless of
     * score.  This is a hard tier instead of an additive penalty because
     * the BSSD frame requires the pivot to track every signal any other
     * satellite tracks, so a candidate below maximum coverage is not
     * acceptable while a full-coverage alternative exists.  The fallback
     * to highest-available coverage is implicit: when no satellite has
     * full coverage, the comparator picks the maximum coverage count it
     * sees.
     *
     * @param observations  Epoch observations.
     * @param state_manager State manager (read-only; used for variance lookups).
     * @param[out] sat_info Per-satellite info (constellation, elevation, variance).
     * @param[out] cons_info Per-constellation info (best candidate, max coverage).
     */
    void buildEpochMaps(
        const std::vector<PppObservation>& observations,
        const StateManager& state_manager,
        std::map<std::string, SatInfo>& sat_info,
        std::map<std::string, ConstellationInfo>& cons_info);

    /**
     * @brief Compute a pivot suitability score for a candidate satellite.
     *
     * Signal coverage is handled outside this function as a hard tier in
     * @c buildEpochMaps (lexicographic (coverage, score) ordering), so
     * this function only ranks *within* a coverage tier.  Two criteria:
     *
     *   - State convergence: a satellite whose own float-ambiguity state
     *     is poorly known propagates that variance into every same-group
     *     SD ambiguity via the T·P·T' re-reference on handover.  Penalty
     *     is −min(1.0, √var) on the worst per-signal ambiguity variance,
     *     with a flat −1.0 sentinel when no state exists yet.
     *
     *   - Geometry: elevation magnitude (0..1) as a smooth bonus, with
     *     a −0.5 hard penalty below 20° to discourage near-horizon
     *     pivots regardless of convergence (troposphere and multipath
     *     dominate at low elevation).
     *
     * On the first BSSD epoch no candidate has a state yet, so the
     * convergence term is constant within a coverage tier and elevation
     * decides; no separate initial-election path is needed.
     *
     * @param sat Satellite identifier (currently unused; retained for
     *            future per-sat hooks like elevation-rate or lock-loss
     *            counters).
     * @param si  SatInfo for this satellite.
     * @return Pivot suitability score (higher = better candidate).
     */
    double scorePivotCandidate(const std::string& sat,
                               const SatInfo& si) const;

    /**
     * @brief Collect the set of phase signal types from observations.
     */
    static std::set<std::string> collectPhaseSignals(
        const std::vector<PppObservation>& observations);

    /**
     * @brief Select pivot candidates with sticky hysteresis.
     *
     * Keeps the current pivot if it is still above the minimum elevation,
     * not in @p exclude_sats, and not outlier-stale in broadcast-ephemeris
     * age.  Otherwise selects the best (coverage, score) satellite among
     * those passing the same three filters, ranked with
     * scorePivotCandidate(), and falls back to the precomputed best or
     * highest satellite only when none passes.  Pivot disturbance is not
     * checked here; handlePivotHandover() handles it afterwards.
     *
     * @param cons_info Per-constellation info from buildEpochMaps().
     * @param sat_info  Per-satellite info from buildEpochMaps().
     * @return Proposed pivot map (constellation -> satellite).
     */
    std::map<std::string, std::string> selectPivotCandidates(
        const std::map<std::string, ConstellationInfo>& cons_info,
        const std::map<std::string, SatInfo>& sat_info,
        const std::set<std::string>& exclude_sats = {}) const;

    /**
     * @brief Handle a disturbed pivot for one constellation.
     *
     * Searches the constellation for a replacement (any satellite other
     * than the disturbed pivot and not in @p exclude_sats, ranked by
     * (coverage, score)).  If one is found, updates @p pivot and returns
     * true.  If none exists, re-zeroes the disturbed pivot's phase
     * ambiguity states in place (value and covariance row/column set to 0)
     * and returns false.
     *
     * @param cons        Constellation name.
     * @param[in,out] pivot  Current pivot (updated to replacement on success).
     * @param sat_info    Per-satellite info.
     * @param phase_sigs  Phase signal set.
     * @param state_manager State manager.
     * @param[in,out] x   State vector (modified on in-place re-zero).
     * @param[in,out] P   Covariance (modified on in-place re-zero).
     * @param epoch_prefix Log prefix.
     * @return true if a replacement was found, false if re-zeroed in place.
     */
    bool handleDisturbedPivot(
        const std::string& cons,
        std::string& pivot,
        const std::map<std::string, SatInfo>& sat_info,
        const std::set<std::string>& phase_sigs,
        const StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix,
        const std::set<std::string>& exclude_sats = {});

    // -------------------------------------------------------------------
    // Disturbance detection
    // -------------------------------------------------------------------

    /**
     * @brief Check whether a pivot satellite's frozen state is disturbed.
     *
     * A pivot's ambiguity state is expected at (0, 0).  The pivot is
     * reported disturbed when any of its phase ambiguity states has
     * |x| > 1e-6 or P > 1e-12, which indicates external corruption
     * (cycle-slip reset, filter reset, etc.).
     */
    bool isPivotDisturbed(
        const std::string& sat,
        const std::set<std::string>& phase_sigs,
        const StateManager& state_manager,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P) const;

    // -------------------------------------------------------------------
    // Re-referencing helpers
    // -------------------------------------------------------------------

    /**
     * @brief Apply lossless T·P·T' re-referencing for ambiguity states.
     */
    void reReferenceAmbiguities(
        const std::string& cons,
        const std::string& pivot,
        const std::set<std::string>& phase_sigs,
        StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix);

    /**
     * @brief Apply lossless T·P·T' re-referencing for ionosphere states.
     */
    void reReferenceIonosphere(
        const std::string& cons,
        const std::string& pivot,
        StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix);

    /**
     * @brief Apply lossless T·P·T' re-referencing for satellite bias states.
     */
    void reReferenceSatBiases(
        const std::string& cons,
        const std::string& pivot,
        StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix);

    /**
     * @brief Apply the T·P·T' covariance transform for a group of states.
     *
     * Implements the common row-subtract / col-subtract / zero-pivot pattern
     * shared by ambiguity, ionosphere, and satellite bias re-referencing.
     *
     * For non-pivot indices i:
     *   P'(i,:) = P(i,:) - P(pivot,:)
     *   P'(:,i) = P(:,i) - P(:,pivot)
     * For the pivot index:
     *   P'(pivot,:) = 0,  P'(:,pivot) = 0
     *
     * @param group_indices All state indices in the group (including pivot).
     * @param pivot_idx     The pivot's state index.
     * @param[in,out] P     Covariance matrix (modified in place).
     */
    static void applyCovarianceReReference(
        const std::vector<Eigen::Index>& group_indices,
        Eigen::Index pivot_idx,
        Eigen::MatrixXd& P);

    // -------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------

    const GnssConfig& config_;
    LogManager& log_manager_;

    /// Current pivot satellite per constellation (e.g. "GPS" -> "G01").
    std::map<std::string, std::string> pivot_sat_;

    /// Constellations whose pivot is flagged for forced handover on the next
    /// @c handlePivotHandover call.  Populated by @c requestForcedHandover and
    /// consumed (erased) inside @c handlePivotHandover when its loop reaches
    /// the constellation, before the disturbance branch runs.
    std::set<std::string> forced_handover_requests_;

    /// Per-constellation replacement-pivot exclusions carried alongside a
    /// forced-handover request (offset pivot + post-fit residual outliers).
    /// Unioned into the effective @c exclude_sats in @c handlePivotHandover
    /// and erased when the matching request is consumed.  This keeps the
    /// forced handover from re-anchoring on another offset satellite.
    std::map<std::string, std::set<std::string>> forced_handover_exclude_;

    /// Per-constellation non-pivot satellites whose ionosphere / sat-bias
    /// state is re-inflated after a code-detected forced handover, carried
    /// alongside @c forced_handover_requests_.  Applied in @c handlePivotHandover
    /// after the lossless re-reference and after the re-referenced covariance
    /// is committed, via @c StateManager::reinflateIonosphereVariance /
    /// @c reinflateSatBiasVariancesForSat, and only when the slip-state
    /// re-inflation config is enabled.  The post-handover SD frame can then
    /// re-estimate the states that the information-conserving re-reference
    /// cannot correct.  Empty unless a coherent pivot code bias (code-only
    /// style) was detected.
    std::map<std::string, std::set<std::string>> forced_handover_reinflate_;

    /// Satellites already retired as a pivot during the current epoch's
    /// handover sequence, keyed by constellation.  Reset at each epoch's
    /// first-pass handover (@c forced_only == false) and accumulated across
    /// the rebuild loop's forced re-entries, then treated as additional
    /// exclusions.  This guarantees that the post-fit handover sequence
    /// terminates: a satellite handed off this epoch cannot be re-elected
    /// within the epoch, so each forced handover moves the anchor to a
    /// satellite not yet used.
    std::map<std::string, std::set<std::string>> retired_pivots_this_epoch_;

    /// Clean (undisturbed, non-forced) pivot handovers performed this epoch,
    /// constellation -> {old_pivot, new_pivot}.  Reset on each epoch's
    /// first-pass handover (@c forced_only == false), populated as clean legs
    /// fire, and read by the RTS smoother via @c getCleanHandovers to decide
    /// inverse-reference vs. segmentation across a handover boundary (P2-1).
    std::map<std::string, std::pair<std::string, std::string>> clean_handovers_;
};

} // namespace GnssProcessor
