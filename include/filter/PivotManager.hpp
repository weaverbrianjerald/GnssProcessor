#pragma once






#pragma once

/**
 * @file PivotManager.hpp
 * @brief Between-satellite single-difference pivot selection and handover.
 *
 * PivotManager encapsulates all BSSD pivot satellite management:
 *   - Sticky pivot selection with elevation hysteresis.
 *   - Pivot disturbance detection (cycle slip, outlier corruption).
 *   - Lossless T�P�T' re-referencing of ambiguity, ionosphere, and
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
#include <vector>

namespace GnssProcessor {

class MwSlipDetector;  // forward declaration

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
     * @brief Provide the MW slip detector for quality-based pivot scoring.
     *
     * When set, pivot selection uses MW arc maturity, noise level, and
     * WL-lock status in addition to elevation and ascending preference.
     *
     * @param mw_detector Pointer to the MwSlipDetector (may be nullptr).
     */
    void setMwDetector(const MwSlipDetector* mw_detector) { mw_detector_ = mw_detector; }

    /**
     * @brief Run pivot selection and handover for one epoch.
     *
     * Sticky pivot selection: keeps the current pivot while it remains
     * above the configured elevation threshold and undisturbed.  When a
     * new pivot is needed (disturbance, low elevation, first epoch),
     * candidates are scored using a multi-criteria function that considers
     * MW arc quality, ascending elevation, and elevation magnitude.
     *
     * @param observations   Modeled observations for this epoch.
     * @param state_manager  Filter state (modified on handover).
     * @param epoch_prefix   Log prefix string.
     */
    void handlePivotHandover(
        const std::vector<PppObservation>& observations,
        StateManager& state_manager,
        const std::string& epoch_prefix);

    /**
     * @brief Request a forced pivot handover for a single constellation on
     *        the next call to @c handlePivotHandover.
     *
     * Used by external detectors (the KF post-fit residual analyzer) to
     * signal that the current pivot's observation has slipped even though
     * its filter STATE is intact (so @c isPivotDisturbed wouldn't flag
     * it on its own). On the next @c handlePivotHandover call, the
     * disturbance branch is entered for the requested constellations as
     * if @c isPivotDisturbed had returned true; the same lossless
     * re-referencing logic that handles state-disturbed pivots picks a
     * replacement and updates state. The request is consumed (cleared)
     * after the handover runs, so it must be re-issued each epoch the
     * detector wants the handover repeated.
     *
     * Idempotent — multiple requests for the same constellation in a
     * row collapse to one handover.
     *
     * @param constellation Constellation name (e.g. "GPS", "Galileo").
     * @param reason        Free-text reason logged at warn level.
     */
    void requestForcedHandover(const std::string& constellation,
                                const std::string& reason);

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

    /// Check if BSSD pivots have been initialized.
    bool hasPivots() const { return !pivot_sat_.empty(); }

private:
    // -------------------------------------------------------------------
    // Per-constellation epoch snapshot (built once per handlePivotHandover)
    // -------------------------------------------------------------------

    /** Per-satellite observation snapshot used during pivot selection. */
    struct SatInfo {
        std::string constellation;   ///< e.g. "GPS"
        double      elevation{0.0};
        double      elev_rate{0.0};  ///< Elevation rate (deg/epoch), positive = ascending.
        size_t      phase_sig_count{0}; ///< Number of distinct phase signal types (L1W, L2W, L5X, etc.).
    };

    /** Per-constellation aggregated info for the current epoch. */
    struct ConstellationInfo {
        std::string best_sat;      ///< Satellite with highest pivot score.
        double      best_score{-1e9};
        std::string highest_sat;   ///< Satellite with highest elevation (fallback).
        double      highest_el{-1.0};
        size_t      max_phase_sigs{0}; ///< Maximum phase signal count across all satellites.
    };

    /**
     * @brief Build per-satellite and per-constellation maps from observations.
     *
     * Single pass over observations.  Skips GLONASS satellites.  Computes
     * elevation rate from previous-epoch history when available.
     *
     * @param observations  Epoch observations.
     * @param[out] sat_info Per-satellite info (constellation, elevation, rate).
     * @param[out] cons_info Per-constellation info (best scored sat, highest elev).
     */
    void buildEpochMaps(
        const std::vector<PppObservation>& observations,
        std::map<std::string, SatInfo>& sat_info,
        std::map<std::string, ConstellationInfo>& cons_info);

    /**
     * @brief Compute a pivot suitability score for a candidate satellite.
     *
     * Multi-criteria score combining:
     *   - MW arc maturity and noise stability (when MW detector is available)
     *   - WL integer lock status (BIA-corrected arcs)
     *   - Ascending elevation preference (avoid descending satellites
     *     that will soon drop below the mask angle)
     *   - Elevation magnitude (higher is more stable)
     *
     * Scoring breakdown (higher is better):
     *   - Elevation:   el_deg / 90  (0..1)
     *   - Ascending:   +0.3 if rate > 0 and el > 20 deg
     *   - Signal cov:  +0.6 * (phase_sigs / max_phase_sigs) � prefer
     *                  satellites that track more phase signals so the
     *                  pivot covers all SD pairs in the constellation
     *   - MW mature:   +0.5 if min_arc >= kMinEpochsForAdaptive
     *   - MW noise:    +0.3 if worst_sigma < 0.15 m (well-behaved arc)
     *   - WL locked:   +0.4 if all BIA-corrected pairs are integer-locked
     *   - Low elev:    -0.5 if el < 20 deg (unreliable for pivot)
     *
     * @param sat              Satellite identifier.
     * @param si               SatInfo for this satellite.
     * @param max_phase_sigs   Max phase signal count across the constellation
     *                         (0 to skip signal coverage scoring).
     * @return Pivot suitability score (higher = better candidate).
     */
    double scorePivotCandidate(const std::string& sat,
                               const SatInfo& si,
                               size_t max_phase_sigs = 0) const;

    /**
     * @brief Collect the set of phase signal types from observations.
     */
    static std::set<std::string> collectPhaseSignals(
        const std::vector<PppObservation>& observations);

    /**
     * @brief Select pivot candidates with sticky hysteresis.
     *
     * Keeps the current pivot if it is still above the minimum elevation
     * and undisturbed.  Otherwise selects the best-scoring satellite
     * using scorePivotCandidate().
     *
     * @param cons_info Per-constellation info from buildEpochMaps().
     * @param sat_info  Per-satellite info from buildEpochMaps().
     * @return Proposed pivot map (constellation -> satellite).
     */
    std::map<std::string, std::string> selectPivotCandidates(
        const std::map<std::string, ConstellationInfo>& cons_info,
        const std::map<std::string, SatInfo>& sat_info) const;

    /**
     * @brief Handle a disturbed pivot for one constellation.
     *
     * Searches for a healthy replacement.  If found, updates @p pivot and
     * returns true.  If no replacement exists, re-zeroes the disturbed
     * pivot's states in-place and returns false.
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
        const std::string& epoch_prefix);

    // -------------------------------------------------------------------
    // Disturbance detection
    // -------------------------------------------------------------------

    /**
     * @brief Check whether a pivot satellite's frozen state is disturbed.
     *
     * A pivot's ambiguity state should be exactly (0, 0).  Any deviation
     * indicates external corruption (cycle-slip reset, filter reset, etc.).
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
     * @brief Apply lossless T�P�T' re-referencing for ambiguity states.
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
     * @brief Apply lossless T�P�T' re-referencing for ionosphere states.
     */
    void reReferenceIonosphere(
        const std::string& cons,
        const std::string& pivot,
        StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix);

    /**
     * @brief Apply lossless T�P�T' re-referencing for satellite bias states.
     */
    void reReferenceSatBiases(
        const std::string& cons,
        const std::string& pivot,
        StateManager& state_manager,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P,
        const std::string& epoch_prefix);

    /**
     * @brief Apply the T�P�T' covariance transform for a group of states.
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

    /// Optional MW detector for quality-based pivot scoring (not owned).
    const MwSlipDetector* mw_detector_{ nullptr };

    /// Current pivot satellite per constellation (e.g. "GPS" -> "G01").
    std::map<std::string, std::string> pivot_sat_;

    /// Previous-epoch elevation per satellite for ascending/descending detection.
    std::map<std::string, double> prev_elevation_;

    /// Constellations whose pivot is flagged for forced handover on the next
    /// @c handlePivotHandover call. Populated by @c requestForcedHandover,
    /// consumed (erased) inside @c handlePivotHandover after the disturbance
    /// branch processes each entry.
    std::set<std::string> forced_handover_requests_;
};

} // namespace GnssProcessor
