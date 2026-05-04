#pragma once

/**
 * @file ResidualHistoryTracker.hpp
 * @brief Per-(satellite, signal) post-fit residual history with EWMA
 *        statistics, used to corroborate the spatial pivot-slip
 *        detector with a temporal "is this row anomalous against its
 *        own track record" signal.
 *
 * @section motivation Motivation
 *
 * The Kalman filter's standard post-fit residual check at
 * @c KalmanFilter::checkPostFitResiduals examines residuals
 * **across satellites at one epoch** — a coherent offset on most
 * non-pivot rows of a BSSD constellation/band group is the spatial
 * signature of a pivot slip.  But spatial coherence alone has two
 * failure modes:
 *  - A genuine atmospheric event (storm front, scintillation) that
 *    shifts many sats simultaneously can mimic the spatial pattern.
 *  - A pivot slip that happens to coincide with one anomalous
 *    satellite can still produce coherent-looking residuals on the
 *    remaining rows even though the pivot is the actual problem.
 *
 * @c ResidualHistoryTracker adds a temporal axis: each
 * (satellite, signal) row's post-fit residual is fed into an
 * exponentially-weighted moving mean and variance.  When the
 * spatial detector identifies a candidate pivot-slip group, the
 * tracker is queried to see what fraction of the group's rows are
 * **also** historically anomalous (z-score above threshold against
 * each row's own EWMA statistics).  Spatial AND temporal evidence
 * agreeing → high-confidence pivot slip; spatial alone → medium
 * confidence.
 *
 * @section design Design
 *
 * The statistics are computed in the EWMA-Welford style
 * (Finch 2009) to avoid storing a full ring buffer:
 *
 *   δ      = r_k − μ_{k-1}
 *   μ_k    = μ_{k-1} + α · δ
 *   σ²_k   = (1 − α) · (σ²_{k-1} + α · δ²)
 *
 * with @c α the forgetting factor (≈ 0.05 → ≈30-epoch effective
 * window).  The implementation tracks a sample count @c n in
 * addition to (μ, σ²) so a "warm-up" guard can suppress the
 * historical test on cold starts and immediately after resets,
 * where the statistics aren't yet meaningful.
 *
 * @section resets Resets
 *
 * Three event classes invalidate per-(sat, signal) history and
 * must trigger a tracker reset:
 *  - **Cycle slip / ambiguity reset.**  Post-slip residuals are
 *    relative to a freshly-seeded ambiguity, not comparable to
 *    pre-slip values.  Reset that single (sat, signal).
 *  - **Pivot handover.**  Every BSSD residual in the affected
 *    constellation/signal pair is now relative to a different
 *    pivot.  Reset the whole constellation/signal group.
 *  - **Satellite acquisition.**  Implicitly handled — first-time
 *    record() creates a fresh entry with @c n = 1.
 *
 * Reset hooks are exposed publicly; the higher-level driver
 * (Application or KalmanFilter) calls them in lockstep with the
 * existing slip and pivot machinery.
 */

#include "logging/LogManager.hpp"
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

    /**
     * @class ResidualHistoryTracker
     * @brief EWMA-based per-row residual statistics with anomaly queries.
     */
    class ResidualHistoryTracker {
    public:
        /**
         * @brief Configuration for the residual-history tracker.
         *
         * Mirrors the @c estimation.residual_history JSON section.
         * When @c enabled is false, all record/evaluate calls become
         * no-ops and queries return non-warmed-up evaluations so the
         * existing pivot-slip path proceeds unchanged.
         */
        struct Config {
            bool   enabled            = false; ///< Master toggle.  Default off — opt in via config.
            double alpha              = 0.05;  ///< EWMA forgetting factor; ≈30-epoch effective window.
            std::size_t warmup_n      = 20;    ///< Min samples before z-tests are emitted.
            double z_threshold        = 4.0;   ///< |z| above this counts as a temporal outlier.
            double fraction_threshold = 0.5;   ///< Group-level fraction above which the tracker corroborates a pivot-slip request.
        };

        /**
         * @brief Result of evaluating one observation against its history.
         *
         * @c warmed_up is false (and @c is_outlier always false) when
         * the tracker is disabled or the per-row sample count hasn't
         * reached @c warmup_n yet.
         */
        struct Evaluation {
            bool   warmed_up = false; ///< True when the per-row history has @c n ≥ @c warmup_n.
            double z_score   = 0.0;   ///< (r − μ) / σ ; valid only when @c warmed_up.
            bool   is_outlier= false; ///< True when @c warmed_up and |@c z_score| > @c z_threshold.
        };

        /**
         * @brief Per-row diagnostic statistics.
         */
        struct Stats {
            std::size_t n;       ///< Sample count (number of record() calls since last reset).
            double      mean;    ///< EWMA mean (m).
            double      std_dev; ///< EWMA standard deviation (m); 0 when n < 2.
        };

        ResidualHistoryTracker(const Config& cfg, LogManager& log_mgr);

        /// Whether the tracker is configured-enabled.  Hot-path callers
        /// can short-circuit on this without touching the maps.
        bool isEnabled() const { return cfg_.enabled; }

        /// Push a post-fit residual into the (@p sat, @p signal) history.
        /// No-op when the tracker is disabled.
        void record(const std::string& sat, const std::string& signal, double residual_m);

        /// Evaluate @p residual_m against the current (@p sat, @p signal)
        /// statistics.  Returns @c warmed_up=false / @c is_outlier=false
        /// when the tracker is disabled or the row hasn't accumulated
        /// enough samples.
        Evaluation evaluate(const std::string& sat,
                            const std::string& signal,
                            double residual_m) const;

        /// Reset all (@p sat, ?) entries — used on a satellite-wide
        /// reset.  No-op for unknown satellites.
        void resetSatellite(const std::string& sat);

        /// Reset a single (@p sat, @p signal) entry — used after a
        /// per-(sat, signal) cycle slip.  No-op for unknown rows.
        void resetSatSignal(const std::string& sat, const std::string& signal);

        /// Reset every (sat, signal) entry whose @c sat starts with the
        /// given constellation prefix character ('G', 'E', 'C', 'R',
        /// 'J', 'S') AND whose signal matches @p signal.  Called on
        /// pivot handover where every BSSD residual in the affected
        /// constellation/signal group is now relative to a new pivot.
        ///
        /// Pass an empty @p signal to match every signal — the
        /// constellation-wide nuke option, useful when a handover
        /// affects all bands of the constellation simultaneously.
        void resetConstellationSignal(char constellation_prefix,
                                      const std::string& signal);

        /// Drop everything.
        void resetAll();

        /// Get current statistics for one row, if any have accumulated.
        std::optional<Stats> getStats(const std::string& sat,
                                       const std::string& signal) const;

        /**
         * @brief Aggregate digest across every tracked (sat, signal).
         *
         * Empty when nothing has been recorded yet.  Used by
         * @c KalmanFilter to emit a once-per-epoch info-level
         * summary on clean commits, so users can see at a glance
         * whether the tracker is warmed up and what the typical
         * residual scale across rows looks like.
         */
        struct Summary {
            std::size_t entry_count = 0; ///< Number of (sat, signal) entries currently tracked.
            std::size_t warmed_up   = 0; ///< Of those, how many have @c n ≥ @c warmup_n.
            double      median_std  = 0.0; ///< Median of per-entry σ across all entries (m).
            double      max_std     = 0.0; ///< Max per-entry σ across all entries (m).
        };
        Summary summarize() const;

        /// Read-only access to the configured group-level fraction
        /// threshold; the consumer combines it with its own
        /// group-membership data.
        double fractionThreshold() const { return cfg_.fraction_threshold; }

    private:
        struct Entry {
            std::size_t n = 0;
            double      mean = 0.0;
            double      var  = 0.0;
        };

        static std::string makeKey(const std::string& sat, const std::string& signal);

        Config       cfg_;
        LogManager&  log_manager_;
        std::map<std::string, Entry> entries_;
    };

} // namespace GnssProcessor
