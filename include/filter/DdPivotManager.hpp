#pragma once

/**
 * @file DdPivotManager.hpp
 * @brief Per-constellation pivot management for the DD baseline path.
 *
 * Symmetric counterpart to @c PivotManager, which serves the PPP-RTK
 * SD pipeline.  @c DdPivotManager owns the per-constellation sticky
 * pivot map for baseline DD processing, performs the sticky-or-rerank
 * selection each epoch, and applies the S-transformation to existing
 * DD ambiguity states when the pivot changes (so cycle-integer
 * information is preserved across handovers instead of being reseeded).
 *
 * The two manager classes do not share an interface: their inputs
 * differ (SD per-receiver observations against DD shared-satellite
 * candidates) and so does their handover math (BSSD on SD against
 * DD direct).  Both have the same responsibility: own the sticky pivot
 * state, handle pivot changes, and expose the current pivots.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/StateManager.hpp"
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

    /**
     * @class DdPivotManager
     * @brief Sticky DD pivot selection and handover S-transform.
     *
     * Holds the per-constellation pivot map across epochs.  Each call
     * to @c selectPivots takes the current epoch's candidate set
     * (sat → ranking-info) per constellation, keeps the incumbent when
     * it is still in the candidate set or re-ranks otherwise, and
     * applies the DD pivot-handover S-transform when the chosen pivot
     * differs from the previous epoch's.  The handover is gated by
     * @c BaselineConfig::enable_pivot_handover.
     *
     * The ranking score is average SNR (primary) plus 0.01 times the
     * number of shared signals (tiebreak).  The highest score wins.
     *
     * State and covariance in @c StateManager are mutated only inside
     * @c selectPivots(), and only when the handover branch runs.  The
     * class holds no reference to the filter state between calls, so it
     * can be used from any rover context.
     */
    class DdPivotManager {
    public:
        /**
         * @brief Per-(cons, sat) ranking information for pivot selection.
         *
         * Slim view over @c MeasurementModel's @c SharedInfo so the
         * pivot manager does not depend on the BRSD / row-builder data
         * types.  The two fields are the inputs of the ranking score.
         */
        struct CandidateInfo {
            double avg_snr{ 0.0 };  ///< Mean SNR across rover-side signals (dB-Hz).
            size_t num_sigs{ 0 };   ///< Count of shared (band, obs_type) signals after BRSD.
        };

        /// Per-constellation candidate set: cons → sat → info.
        using CandidateMap = std::map<std::string, std::map<std::string, CandidateInfo>>;

        /**
         * @brief Construct with handles to config (for the handover toggle)
         *        and the shared log manager.
         */
        DdPivotManager(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Choose pivots for the current epoch and apply handover.
         *
         * For each constellation in @p candidates:
         *   - If the previous epoch's sticky pivot is still in the
         *     candidate set, keep it.  No re-rank, no handover.
         *   - Otherwise, re-rank candidates by
         *     (avg_snr + 0.01 * num_sigs) and pick the highest.  When
         *     the new choice differs from the previous incumbent and
         *     @c BaselineConfig::enable_pivot_handover is true, the
         *     S-transform is applied to the DD ambiguity sub-vector
         *     so cycle-integer information is preserved.
         *
         * Updates the internal sticky map and returns the new pivot
         * map by value (callers usually want both).
         *
         * @param candidates    Per-constellation candidate set for this epoch.
         * @param state_manager Mutable filter state, used by the
         *                      handover branch to read DD ambiguities,
         *                      apply @c x_new = T·x and @c P_new = T·P·Tᵀ,
         *                      and rename keys on the surviving rows.
         * @return New pivot map: cons → pivot satellite.
         */
        std::map<std::string, std::string> selectPivots(
            const CandidateMap& candidates,
            StateManager& state_manager);

        /**
         * @brief Current sticky pivot map.
         *
         * Returns the result of the most recent @c selectPivots() call,
         * or an empty map before the first call (or after @c reset()).
         */
        const std::map<std::string, std::string>& getPivots() const { return sticky_; }

        /**
         * @brief Forget all sticky pivots.
         *
         * Called by the baseline epoch loop on a hard gap (so the
         * post-gap epoch picks fresh pivots instead of inheriting the
         * pre-gap incumbent) and after an MW slip, and by the
         * application's heavy filter reset.  With no previous pivot, the
         * next @c selectPivots applies no handover.  Each rover gets its
         * own @c MeasurementModel, so pivots never carry across rovers.
         */
        void reset() { sticky_.clear(); forced_rerank_.clear(); }

        /**
         * @brief Force a pivot re-rank for one constellation next epoch.
         *
         * Routed from the baseline driver when @c KalmanFilter's coherent
         * pivot-slip detector flags @p cons.  On the next @c selectPivots the
         * sticky incumbent for @p cons is dropped even if it is still a
         * candidate, and the re-rank skips @p exclude_sats (the offset pivot
         * plus any co-disturbed satellites the KF identified) so the new pivot
         * lands on a clean satellite.  The previous-pivot snapshot is kept, so
         * with @c enable_pivot_handover on, the handover S-transform
         * re-references existing DD ambiguities onto the new pivot (integer
         * cycles preserved) instead of resetting them.  The request is
         * one-shot: it is cleared by the next @c selectPivots.
         *
         * @param cons         Constellation name (e.g. "GPS").
         * @param exclude_sats Satellites to exclude from the re-rank candidates.
         */
        void requestForcedRerank(const std::string& cons,
                                 const std::set<std::string>& exclude_sats) {
            forced_rerank_[cons].insert(exclude_sats.begin(), exclude_sats.end());
        }

    private:
        const GnssConfig& config_;
        LogManager&       log_manager_;
        std::map<std::string, std::string> sticky_;

        /// Pending KF-driven forced re-ranks: cons → sats to exclude from the
        /// re-rank candidate set.  Set via @c requestForcedRerank, consumed and
        /// cleared by the next @c selectPivots.
        std::map<std::string, std::set<std::string>> forced_rerank_;

        /**
         * @brief Apply the DD pivot-handover S-transform.
         *
         * For each constellation whose pivot changed between
         * @p previous_pivots and @p new_pivots, walks every DD
         * ambiguity in the state with the old-pivot suffix, groups by
         * RINEX code, and:
         *
         *   - If the (cons, code) group has an ambiguity for the new
         *     pivot, builds the corresponding rows of an S-transform
         *     matrix T such that @c T·x maps DDs from the old-pivot
         *     basis to the new-pivot basis:
         *       new_DD(Si) = old_DD(Si) - old_DD(Pnew)   for Si != Pnew
         *       new_DD(Pold) = -old_DD(Pnew)             (Pold becomes a non-pivot)
         *     The covariance is updated via @c P_new = T·P·Tᵀ.  Keys
         *     are renamed in place (@c StateManager::renameAmbiguity).
         *
         *   - If the new pivot has no existing ambiguity in the (cons, code)
         *     group, the transform is skipped for that group and the
         *     downstream expected-keys prune erases the stale rows so
         *     the row builder reseeds on the new pivot.
         *
         * All changed (cons, code) groups form one full-state transform
         * T, identity outside the groups.  T is never materialized: it is
         * applied in place as a row pass (x and P) followed by a column
         * pass (P), and the result goes through the shared PSD guard.
         */
        void handleHandover(
            StateManager& state_manager,
            const std::map<std::string, std::string>& previous_pivots,
            const std::map<std::string, std::string>& new_pivots);
    };

} // namespace GnssProcessor
