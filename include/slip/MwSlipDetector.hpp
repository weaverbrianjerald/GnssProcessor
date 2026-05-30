#pragma once

#pragma once

/**
 * @file MwSlipDetector.hpp
 * @brief Detects large wide-lane (MW) resets/slips using ionosphere-free combinations.
 *
 * The MwSlipDetector identifies large changes in MW-like observables between
 * epochs which typically indicate cycle slips or ambiguity resets. The detector
 * produces a list of ambiguity keys (e.g. "G01_L1C_L2W") that should be reset
 * when a large MW discontinuity is detected.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Result of MW slip detection for a single ambiguity.
     *
     * The MW observable measures N_WL = N_1 - N_2.  A WL slip tells us
     * the *difference* changed but cannot distinguish which signal slipped.
     * When slip_wl_known is true, slip_wl_cycles contains an integer
     * estimate of the WL jump, and slip_frac records how far from integer
     * the raw value was before rounding (0 = exact, up to 0.5 = worst).
     *
     * @note slip_wl_cycles is always the satellite's OWN undifferenced
     *       (zero-differenced) WL slip.  Bias-corrected arcs are detected in a
     *       single-differenced frame against the detector's internal MW pivot,
     *       but that pivot's own slip is added back before reporting, so
     *       consumers may difference two satellites' slips directly without
     *       knowing (or sharing) the MW pivot.  When the conversion can't be
     *       made (MW pivot slipped with an unresolved integer) slip_wl_known is
     *       set false so the consumer falls back to a reset.
     */
    struct MwSlipResult {
        std::string amb_key;           ///< Ambiguity key (e.g. "G01_L1C").
        int    slip_wl_cycles{ 0 };    ///< Satellite's OWN (zero-differenced) WL slip dN_WL, cycles (0 if unknown).
        bool   slip_wl_known{ false }; ///< True if slip_wl_cycles is a usable integer.
        double lambda_wl{ 0.0 };       ///< Wide-lane wavelength (meters) for this signal pair.
        std::string sig1;              ///< First signal (higher freq, e.g. "L1C").
        std::string sig2;              ///< Second signal (lower freq, e.g. "L2W").
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        double slip_frac{ 0.0 };       ///< Fractional residual |x - round(x)| before rounding
                                       ///< (0 = exact integer, 0.5 = worst).  Clean pairs and
                                       ///< BIA-locked pairs have ~0; best-effort pairs may be large.
    };

    /**
     * @brief Per-satellite MW arc quality summary for external consumers.
     *
     * Aggregates statistics across all MW pairs for one satellite into a
     * single quality snapshot suitable for pivot selection scoring.
     */
    struct SatelliteMwQuality {
        size_t min_arc_length{ 0 };   ///< Shortest arc (epoch count) across all pairs.
        double worst_sigma{ 1e9 };    ///< Largest MAD-sigma (meters) across all pairs (NaN if immature).
        bool   all_wl_locked{ false };///< True only if every BIA-corrected pair has a locked WL integer.
        size_t num_pairs{ 0 };        ///< Number of active MW pairs for this satellite.
    };

    /**
     * @class MwSlipDetector
     * @brief Detector for large MW (wide-lane / MW-like) discontinuities.
     *
     * The detector inspects per-satellite observations and computes an MW-like
     * quantity in meters. It keeps a short history (last value per satellite)
     * and flags ambiguities for reset when the MW change exceeds a configured
     * threshold (in wide-lane cycles).
     */
    class MwSlipDetector {
    public:
        /**
         * @brief Construct a new MwSlipDetector.
         * @param config Configuration providing thresholds and processing options.
         * @param log_manager Log manager used to emit diagnostic messages.
         */
        MwSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect large MW resets for the provided epoch observations.
         *
         * Iterates provided observations and returns per-ambiguity slip results.
         * Each result contains the ambiguity key and, for arcs where the WL was
         * locked to an integer, the integer slip magnitude dN_WL.  When a
         * satellite has 3+ frequencies, the caller can use multi-frequency
         * MW pair cross-consistency to identify which individual signal
         * slipped and perform a targeted single-signal ambiguity reset.
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return std::vector<MwSlipResult> Slip results for each affected ambiguity.
         */
        std::vector<MwSlipResult> detectLargeMwResets(const std::vector<PppObservation>& observations);

        /**
         * @brief Get all currently locked WL integer ambiguities.
         *
         * Returns a snapshot of every arc that has a locked WL integer value.
         * The key matches the MW arc stats key (e.g. "G01_L1C_L2W" for
         * uncombined). The value is the rounded SD WL integer.
         *
         * Useful for external components (e.g. AmbiguityResolver) that want
         * to constrain or validate narrow-lane ambiguities.
         *
         * @return Map of stats_key -> rounded WL integer.
         */
        std::map<std::string, int> getResolvedWlAmbiguities() const;

        /**
         * @brief Get the worst-case fractional residual of each
         *        currently-locked WL arc, in cycles.
         *
         * Reports @c |mean/λ_WL − rounded_wl| + 3·σ_mean/λ_WL — the same
         * worst-case-frac expression the lock criterion uses, capturing
         * both the observed drift between the live running mean and the
         * locked integer and the statistical uncertainty of the mean.
         *
         * Companion to @c getResolvedWlAmbiguities() used by AR to gate
         * low-confidence locks (see @c AmbiguityResolverConfig::wl_max_frac).
         * Only arcs with @c wl_locked=true and a finite @c lambda_wl are
         * reported.  Frac of 0.0 = mean sits exactly on integer × λ with
         * zero variance; values approaching @c kWlLockDriftMaxCyc (~0.4)
         * indicate the arc is close to drift demotion.  Values cannot
         * exceed @c kWlLockDriftMaxCyc + 3·σ_mean because @c processMwSample
         * demotes any arc whose live drift crosses that threshold.
         *
         * @return Map of stats_key -> worst-case fractional residual (cycles).
         */
        std::map<std::string, double> getResolvedWlFractions() const;

        /**
         * @brief Get all mature, non-slipped MW arc keys.
         *
         * Returns every stats key that has an active MW arc with enough
         * epoch history (diff_window >= kMinEpochsForAdaptive) to have
         * detected a slip if one occurred.  Immature arcs are excluded
         * because their slip-detection is deferred � treating them as
         * "clean" would create false constraints in the multi-frequency
         * solver.  Slipped arcs are excluded because they are erased
         * immediately upon detection.
         *
         * The key format is "SAT_SIG1_SIG2" (e.g. "G01_L1C_L2W").
         * This is used by CycleSlipManager to build clean-pair references
         * (dN_i - dN_j = 0) for multi-frequency cross-consistency analysis,
         * which works whether or not BIA satellite biases are available.
         *
         * @return Vector of mature active arc stat keys.
         */
        std::vector<std::string> getActiveArcKeys() const;

        /**
         * @brief Clear MW arc history for a specific satellite.
         *
         * Call this when an ambiguity is reset by an external mechanism (e.g.,
         * KF outlier handler) so the MW detector starts a fresh arc on the
         * next epoch. Without this call, stale pre-reset MW values would
         * contaminate the running statistics and mask subsequent slips.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite);

        /**
         * @brief Clear MW arc histories that involve a specific (satellite,
         *        signal) — leaving arcs on signal pairs that don't include
         *        this signal intact.
         *
         * Finer-grained alternative to @c resetSatellite for the common case
         * where only one signal slipped on a multi-frequency satellite (e.g.
         * L1C slipped while L2W and L5X are clean — the L2W/L5X arc should
         * survive while L1C/L2W and L1C/L5X arcs are cleared).
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    Signal identifier (e.g. "L1C", "L2W", "L5X").
         */
        void resetSatelliteSignal(const std::string& satellite,
                                   const std::string& signal);

        /**
         * @brief Hint that a satellite should be deprioritized when picking
         *        the next MW pivot.
         *
         * Consumed by the next @c detectLargeMwResets call: the
         * elevation-based pivot selector skips listed satellites unless
         * they are the only candidate in a (constellation, signal-pair)
         * group.  After consumption the hint set is cleared, so the
         * caller must re-issue each epoch the deprioritization should
         * hold.
         *
         * Intended use: when an upstream component (typically the KF
         * post-fit residual detector via @c PivotManager::requestForcedHandover)
         * flags that the current KF pivot has slipped, it forwards the
         * same satellite here.  Without this hint the MW pivot picker
         * (which sees neither the KF residual nor the handover request)
         * can re-elect the slipped sat and propagate stale arcs into
         * the next epoch's MW frame.
         *
         * @param satellite Satellite identifier (e.g. "G27").
         * @param reason    Free-text reason logged at info level.
         */
        void notifyPivotForcedHandover(const std::string& satellite,
                                       const std::string& reason);

        /**
         * @brief Clear all per-arc state.
         *
         * Drops every entry in the running-statistics map and any SD pivot
         * bookkeeping.  Intended for hard-gap restarts: when a data-gap
         * threshold is exceeded, neither pre-gap MW values nor pre-gap
         * SD pivot choices are reliable indicators for the post-gap arcs,
         * so the safest restart is a full clear rather than per-sat resets.
         * The next call to @c detectLargeMwResets() then begins fresh
         * arcs with no slip flags fired against pre-gap history.
         */
        void resetAll();

        /**
         * @brief Enable CSV tracing for MWWL diagnostics.
         *
         * Opens or creates the file "mwwl_trace.csv" in the specified directory.
         * Subsequent calls to detectLargeMwResets() will write per-satellite MW
         * observations to this file for offline visualization.
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened, false otherwise.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Check if tracing is currently enabled.
         * @return true if the trace file is open and writable.
         */
        bool isTracingEnabled() const { return mwwl_trace_.is_open(); }

        /**
         * @brief Provide satellite bias access for WL bias correction.
         *
         * When set, the detector subtracts the satellite MW bias computed
         * from BIA phase+code entries before accumulating the Hatch mean.
         * This makes the bias-corrected MW near-integer in WL cycles,
         * enabling deterministic rounding of the wide-lane ambiguity.
         *
         * @param corrections Pointer to GnssCorrections (may be nullptr to disable).
         */
        void setCorrections(const GnssCorrections* corrections) { corrections_ = corrections; }

        /**
         * @brief Override the inter-epoch gap tolerance for arc resets.
         *
         * Overrides @c kDefaultMaxGapSeconds for this detector instance.
         * Application sets this to the RINEX sampling interval when the
         * data rate is coarser than 5 s and IMU is not available, so any
         * gap longer than the nominal cadence resets the arc rather than
         * silently absorbing a slip-during-gap into the running mean.
         * When IMU bridges gaps the default 120 s is preserved by not
         * calling this setter.
         *
         * @param seconds  New gap tolerance.  Values ≤ 0 are ignored
         *                 (default is preserved).
         */
        void setMaxGapSeconds(double seconds);

        /**
         * @brief Query aggregated MW arc quality for a satellite.
         *
         * Scans all MW arc stats keyed by the given satellite prefix and
         * returns the worst-case sigma, shortest arc length, and WL lock
         * status across all pairs.  Used by the BSSD pivot manager for
         * quality-aware pivot selection.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @return SatelliteMwQuality summary (num_pairs == 0 if no arcs exist).
         */
        SatelliteMwQuality getSatelliteMwQuality(const std::string& satellite) const;

        /**
         * @brief Query the worst MW deviation score for a satellite.
         *
         * Returns the maximum |deviation / sigma| across all MW arcs for
         * the given satellite, using the deviation and MAD-sigma from the
         * most recently processed epoch.  A higher score indicates a
         * stronger MW disturbance, making this satellite a better
         * candidate for ambiguity reset when multiple phase outliers
         * compete for the worst standardized residual.
         *
         * Returns 0 when no mature arcs exist for the satellite.
         *
         * @param satellite Satellite identifier (e.g. "E18").
         * @return Worst-case |dev/sigma| score (dimensionless).
         */
        double getWorstMwScore(const std::string& satellite) const;

        private:
        const GnssConfig& config_;    /**< Reference to processing configuration. */
        LogManager& log_manager_;     /**< Reference to logging manager. */
        PppUtils ppp_utils_;          /**< Utility helpers for frequency/wavelength lookups. */
        const GnssCorrections* corrections_{ nullptr }; /**< Optional bias lookup (owned externally). */

        /// Effective max-gap tolerance.  Overridable via @c setMaxGapSeconds;
        /// initialized to @c kDefaultMaxGapSeconds.
        double max_gap_seconds_{ kDefaultMaxGapSeconds };

        /**
         * @brief Per-satellite running statistics for the MW time series.
         *
         * Maintains a recursively updated mean and Welford variance estimate,
         * plus a rolling window of recent epoch-to-epoch MW differences used
         * to compute the Median Absolute Deviation (MAD) for robust outlier
         * detection. Using differences instead of raw values ensures the MAD
         * measures the actual noise level rather than spread around the
         * (arbitrary) ambiguity-dominated mean.
         */
        struct MwArcStats {
            double mean{ 0.0 };            /**< Running (Hatch) mean of MW values (meters). */
            double M2{ 0.0 };              /**< Welford accumulator for variance (m^2). */
            size_t count{ 0 };             /**< Number of MW samples accumulated. */
            double prev_mw{ 0.0 };         /**< Previous epoch MW value for differencing (meters). */
            double last_epoch_s{ 0.0 };    /**< Continuous GPS seconds of the last update (for gap detection). */
            std::deque<double> diff_window; /**< Rolling window of epoch-to-epoch MW differences (meters). */
            bool   bia_corrected{ false };  /**< True if both satellite BIA bias and BSSD receiver bias are removed (integer-ready). */
            int    rounded_wl{ 0 };         /**< Rounded WL integer once locked (only valid when bia_corrected). */
            bool   wl_locked{ false };      /**< True once the WL integer has been locked by rounding. */
            double lambda_wl{ 0.0 };        /**< Wide-lane wavelength (meters) cached for slip repair. */
            double last_dev{ 0.0 };         /**< Deviation from mean at the most recent epoch (meters). */
            double last_sigma{ std::numeric_limits<double>::quiet_NaN() }; /**< MAD-derived sigma at the most recent epoch (meters). */
        };

        /** Per-satellite MW arc statistics keyed by satellite identifier. */
        std::map<std::string, MwArcStats> mw_stats_;

        /** Pre-update snapshot of each arc taken at the start of every
         *  @c processMwSample call (after gap detection, before any
         *  state-modifying processing).  Keyed identically to
         *  @c mw_stats_.  Consumed by @c resetSatellite /
         *  @c resetSatelliteSignal to roll back a single epoch's
         *  contribution when the KF post-fit residual check identifies
         *  a slip MW's own detector missed — preserves the arc's pre-
         *  slip noise statistics (mean, M2, diff_window, count) rather
         *  than wiping the entire arc and forcing a re-warmup.  On
         *  rollback we additionally clear @c wl_locked / @c rounded_wl
         *  since a KF-detected slip means the underlying ambiguity
         *  changed and the previously-locked integer is no longer
         *  valid.
         *
         *  Cleared at the top of @c detectLargeMwResets each epoch so
         *  the map only ever holds current-epoch snapshots.  Consumed
         *  (erased per arc) by reset functions.  Also cleared in
         *  @c resetAll.  When no snapshot exists for an arc the reset
         *  falls back to the legacy full-erase behavior. */
        std::map<std::string, MwArcStats> arc_snapshots_;

        /** Current BSSD pivot satellite per (constellation char + signal-pair key).
         *  Example key: "G_L1C_L2W".  When the pivot changes all SD arcs
         *  for that group are reset because the SD ambiguity changes. */
        std::map<std::string, std::string> sd_pivot_sat_;

        /** One-shot deprioritization hints for the next MW pivot pick.
         *  Populated by @c notifyPivotForcedHandover and consumed/cleared
         *  by the next call to @c detectLargeMwResets. */
        std::set<std::string> deprioritized_pivot_sats_;

        /** Cross-satellite MAD σ per (constellation × signal-pair) group,
         *  keyed by "<system_char>_<sig1>_<sig2>" (e.g. "G_L1C_L2W").
         *  Updated at the end of each @c detectLargeMwResets call from
         *  the previous epoch's pool of per-sat MW deviations.  Consumed
         *  by @c computeAdaptiveThreshold as one of the σ_floor_static
         *  sources — provides statistical strength even when individual
         *  arcs are still warming up (cold start: 10 GPS sats at epoch 2
         *  give MAD with 9 degrees of freedom vs. zero per-arc).
         *  Persists across epochs (last known value); cleared by
         *  @c resetAll.  Lags by one epoch — the current epoch's pool
         *  isn't known until all sats are processed, so the floor uses
         *  the previous epoch's σ.  Lag is acceptable because the
         *  noise environment doesn't change epoch-to-epoch. */
        std::map<std::string, double> cross_sat_sigma_m_;

        /** Per-epoch staging map: collects per-sat MW deviations during
         *  @c processMwSample, finalized into @c cross_sat_sigma_m_ at
         *  the end of @c detectLargeMwResets and then cleared.  Keyed
         *  the same way as @c cross_sat_sigma_m_. */
        std::map<std::string, std::vector<double>> cross_sat_devs_;

        /** Coarser cross-sat σ tiers for the breadth cascade in
         *  @c computeAdaptiveThreshold, derived from the same per-epoch
         *  deviation pool as @c cross_sat_sigma_m_.  @c cross_sat_sigma_cons_
         *  is keyed by single-char constellation ("G") and pools all
         *  band-pairs of that constellation; @c cross_sat_sigma_global_ pools
         *  every constellation EXCEPT GLONASS (its FDMA inter-channel noise is
         *  the least comparable, so it must not contaminate the estimate the
         *  clean constellations fall back on).  A sparse band-pair group falls
         *  back to its constellation tier, then (non-GLONASS only) to the
         *  global tier, then to the gross-slip gate.  Both persist across
         *  epochs and are cleared by @c resetAll. */
        std::map<std::string, double> cross_sat_sigma_cons_;
        double cross_sat_sigma_global_ = std::numeric_limits<double>::quiet_NaN();

        /** Minimum cross-sat pool size before MAD is statistically
         *  meaningful.  Below this we leave the previous epoch's σ in
         *  place (or no entry, if cold start) rather than computing
         *  from a degenerately small pool. */
        static constexpr size_t kMinCrossSatSamples = 5;

        /**
         * @brief Persistent per-(sat, signal-pair) phase-bias estimate.
         *
         * Used when @c GnssConfig::getInhouseBiasEstimation() is enabled
         * for arcs lacking external BIA bias correction.  An arc that
         * matures with its running mean within @c kBiasCommitMaxFrac of
         * an integer cycle, AND with σ_mean below @c kBiasCommitMaxSigmaCyc,
         * commits its residual offset as a stored bias.  The stored bias
         * is subtracted from incoming MW samples on subsequent arcs of
         * the same (satellite, signal-pair) so the new arc accumulates
         * an integer-near mean from epoch one and is eligible for the
         * normal lock criterion.
         *
         * Lifetime is decoupled from @c MwArcStats — the bias survives
         * arc resets (new arc inherits parent's stored bias), survives
         * cycle slips (the integer changed but the bias didn't), and is
         * S-transformed alongside arc means when the MW pivot changes.
         */
        struct EstimatedBias {
            double bias_m{ 0.0 };           /**< Stored bias in meters; subtract from raw mw_m to get integer-near. */
            double bias_var_m2{ 0.0 };      /**< Uncertainty (variance) in m². */
            size_t commit_count{ 0 };       /**< Number of arcs that have contributed via EWMA. */
            double last_update_epoch_s{ 0.0 }; /**< Continuous GPS seconds of the most recent update. */
        };
        std::map<std::string, EstimatedBias> sat_pair_bias_;

        /** Minimum arc count before an unbiased arc is eligible to commit
         *  a bias estimate.  Higher than @c kWlLockMinN because the commit
         *  is a one-shot integer decision (whereas the lock criterion has
         *  the k_σ statistical safety net that scales with √n). */
        static constexpr size_t kBiasCommitMinCount = 20;

        /** Maximum |frac| (in cycles) for an unbiased arc to commit a
         *  bias.  Tighter than @c kWlMaxFrac because we are deciding
         *  which integer the arc converged to without external corroboration;
         *  a sat with frac in the 0.20–0.30 zone may legitimately belong
         *  to either side and we'd rather wait for more evidence (or skip
         *  the satellite) than gamble. */
        static constexpr double kBiasCommitMaxFrac = 0.20;

        /** Maximum σ_mean (cycles) for commit.  Ensures the running mean
         *  is statistically well-determined before its residual is
         *  declared a hardware bias. */
        static constexpr double kBiasCommitMaxSigmaCyc = 0.05;

        /** EWMA blending coefficient for cross-arc bias updates.  A small
         *  value (slow EWMA) reflects that hardware biases drift on
         *  hours-to-days timescales, so a single arc shouldn't move the
         *  estimate much; many arcs aggregating through the EWMA refine it. */
        static constexpr double kBiasEwmaAlpha = 0.20;

        /**
         * @brief Apply the stored bias for a (sat, signal-pair) to an
         *        incoming raw MW sample, if such a bias exists.
         *
         * @param[in,out] mw_m            MW value (meters), modified in place.
         * @param[in,out] bia_corrected   Becomes true when a stored bias is applied.
         * @param stats_key               "<sat>_<sig1>_<sig2>" key.
         */
        void applyEstimatedBias(double& mw_m, bool& bia_corrected,
                                 const std::string& stats_key) const;

        /**
         * @brief If the arc is eligible, commit (or update) an estimated
         *        bias from its current running mean.
         *
         * @param stats         Arc statistics (modified: mean re-anchored to integer on commit).
         * @param stats_key     "<sat>_<sig1>_<sig2>" key.
         * @param lambda_wl     Wide-lane wavelength (meters).
         * @param epoch_s       Continuous GPS seconds of this epoch.
         * @return true if a bias was committed (or updated) this epoch.
         */
        bool tryCommitOrUpdateBias(MwArcStats& stats,
                                    const std::string& stats_key,
                                    double lambda_wl, double epoch_s);

        /** Maximum number of recent MW values kept per satellite for MAD computation. */
        static constexpr size_t kMwWindowSize = 30;

        /** Maximum effective count used in the Hatch (recursive) mean.
         *  Once the arc exceeds this count the weighting denominator is
         *  clamped, giving new samples a minimum weight of 1/kMaxHatchCount.
         *  Without this cap the mean becomes nearly frozen on long arcs and
         *  cannot track level shifts that fall just below the detection
         *  threshold, producing the characteristic exponential-decay
         *  residual pattern reported in MW time-difference diagnostics. */
        static constexpr size_t kMaxHatchCount = 100;

        /** Multiplier applied to the σ floor in @c computeAdaptiveThreshold.
         *  Used in both the pre-MAD warmup and post-MAD branches so the
         *  detection gate is consistent across the transition at
         *  @c kMinEpochsForAdaptive — no discontinuity when MAD takes
         *  over from the cross-sat + Welford floor.  8σ admits
         *  essentially zero Gaussian false alarms while remaining
         *  sensitive to multi-cycle slips above the noise level
         *  represented by @c std::max(σ_MAD, σ_floor_static). */
        static constexpr double kMadSigmaMultiplier = 8.0;

        /**
         * @brief Minimum number of diff_window entries before MAD-σ is
         *        trusted as the adaptive threshold input.  MAD computed
         *        from only 5 diffs is statistically marginal — when
         *        those few diffs happened to be tight by luck the MAD
         *        collapsed to near-zero, producing the 1-cycle → 0.008-
         *        cycle threshold transition observed in MWWL traces.
         *        15 diffs gives MAD enough degrees of freedom to be
         *        representative.  During the warmup phase
         *        @c computeAdaptiveThreshold uses a max of the
         *        cross-sat MAD, the per-arc Welford σ_emp, and the
         *        absolute @c kMinSigmaMeters floor — none of which
         *        require external calibration. */
        static constexpr size_t kMinEpochsForAdaptive = 15;

        /** Lower bound on the MAD-derived sigma (meters) used in
         *  @c computeAdaptiveThreshold.  Prevents the threshold from
         *  collapsing to near-zero when the diff_window happens to be
         *  unnaturally tight (e.g., a brief stretch of nearly-identical
         *  samples on a quiet, short arc).  Set to 1 cm — well below
         *  typical L1/L2 WL MAD (~5–15 cm), so it doesn't change behavior
         *  for the standard pairs, while keeping the threshold
         *  wavelength-independent and useful for ultra-long EWL pairs
         *  (e.g., L5X/L8X at λ ≈ 19.5 m) whose natural MAD can be
         *  millimeters and whose meaningful anomalies live at the
         *  centimeter scale. */
        static constexpr double kMinSigmaMeters = 0.01;

        /** Default maximum inter-epoch gap (seconds) before the arc is
         *  reset.  Override at runtime via @c setMaxGapSeconds — typical
         *  use is to tighten this to the RINEX sampling interval when
         *  IMU is not available, so a brief outage cannot silently
         *  initialize a fresh arc with a slip-corrupted baseline.
         *  When IMU is available and bridging gaps via INS propagation,
         *  the default is preserved so legitimate continuous arcs
         *  survive 60–120 s outages. */
        static constexpr double kDefaultMaxGapSeconds = 120.0;

        /** Effective first-epoch fractional-residual threshold (WL cycles).
         *  Initialized from @c GnssConfig::getMwInitFracThreshold (default
         *  0.4).  Raise it for code-noisy combinations (e.g. L1C-based
         *  MW) where 0.4 is below the single-sample noise floor.  See
         *  the config getter's doc for the noise-vs-threshold reasoning. */
        double init_frac_threshold_;

        // Note: the WL lock criterion is no longer a fixed-N wait.  It's
        // an adaptive 3σ confidence test on the running mean,
        //
        //     |frac| + 3 · σ_mean / λ_WL  <  0.5    AND    |frac| < kWlMaxFrac
        //
        // implemented inline in @c processMwSample.  Clean arcs lock as
        // early as n=3; noisy arcs wait until √n has averaged the noise
        // down enough.  See the inline kWlLockKSigma / kWlLockMinN
        // constants there.

        /** Hard cap on raw fractional residual |mean/λ_WL − round(...)|
         *  for an MW arc to be exposed to AR (or to lock).  The
         *  statistical 3σ test alone can satisfy itself that a non-
         *  integer running mean (e.g. 3.33 with σ_mean = 0.05) is
         *  well-determined and round to a (potentially wrong) integer.
         *  This cap treats a large raw |frac| as a "real bias" signal
         *  that disqualifies the arc regardless of statistical
         *  confidence.  Used both at lock time and as a filter in
         *  @c getResolvedWlAmbiguities / @c getResolvedWlFractions. */
        static constexpr double kWlMaxFrac = 0.30;

        /** Live-mean drift threshold (WL cycles) that demotes a locked
         *  arc back to unlocked.  After lock the Welford / Hatch mean
         *  is no longer pinned to @c rounded_wl × λ_WL — it keeps
         *  tracking, so persistent disagreement between the running
         *  mean and the locked integer manifests as drift.  When
         *  |mean/λ − rounded_wl| exceeds this threshold the lock is
         *  no longer supported by accumulated evidence (either the
         *  initial lock was premature, or the arc has absorbed enough
         *  slipped samples to migrate); demote and let the arc re-
         *  resolve.  0.4 cycles is well inside the rounding basin
         *  (< 0.5) while leaving comfortable headroom over the lock
         *  criterion's kWlMaxFrac=0.30 ceiling, so a freshly locked
         *  arc with frac near the lock cap doesn't immediately
         *  demote itself on the next sample. */
        static constexpr double kWlLockDriftMaxCyc = 0.4;

        /**
         * @brief Compute the adaptive slip-detection threshold for an arc.
         *
         * Threshold = @c kMadSigmaMultiplier × max(σ_MAD, σ_floor_static),
         * with σ_floor_static = max(@c kMinSigmaMeters, cross-sat σ).  The
         * cross-sat σ is resolved by a breadth cascade — band-pair →
         * constellation → global(non-GLONASS) — so a sparse band-pair group
         * borrows from a broader-but-comparable population; the chosen σ is
         * zenith-referenced and re-expanded to this arc's elevation.  During
         * warmup (before the per-arc diff_window has a trustworthy MAD) the
         * per-arc Welford σ_emp is folded into the floor too.  When NO
         * cross-sat tier is populated (true cold start), the result is floored
         * at a gross-slip GATE of min(λ_WL, 1 m) applied directly (not ×k).
         * No stochastic-model-derived σ sources are consulted — empirically
         * they saturated the gate at the cap.
         *
         * @param stats     Current arc statistics for the satellite.
         * @param stats_key Per-arc key used to look up the cross-sat σ
         *                  floor for this (constellation × signal-pair)
         *                  group via @c makeGroupKey.  Pass empty string
         *                  to skip the cross-sat lookup.
         * @param elev_deg  Arc elevation (deg).  Re-expands the
         *                  zenith-referenced cross-sat σ floor to this arc's
         *                  elevation via @c mwElevationWeight (low-el arcs get
         *                  a looser floor, high-el a tighter one).
         * @return Adaptive threshold in meters.
         */
        double computeAdaptiveThreshold(const MwArcStats& stats,
                                         const std::string& stats_key,
                                         double elev_deg) const;

        /**
         * @brief Compute the MAD-derived Gaussian-equivalent sigma from an arc's diff window.
         * @param stats  Arc statistics whose diff_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeMadSigma(const MwArcStats& stats);

        /**
         * @brief Compute MAD-derived Gaussian-equivalent σ from an
         *        arbitrary vector of deviations (in meters).
         *
         * @param devs  Vector of deviations.  Must have at least
         *              @c kMinCrossSatSamples entries to return a finite
         *              value.
         * @return σ in meters, or NaN if the pool is too small.
         */
        static double computeMadOfDevs(std::vector<double> devs);

        /**
         * @brief Derive the cross-sat group key from a per-arc stats key.
         *
         * Reduces the satellite to its constellation prefix AND each signal to
         * its 2-char frequency band (attribute dropped), so all attribute
         * combinations on the same frequency pair pool into one statistic:
         *   "G01_L1C_L2W" → "G_L1_L2"
         *   "G01_L1L_L2L" → "G_L1_L2"  (same group)
         * Distinct frequency pairs (L1/L2 vs L1/L5) remain separate.  This
         * gives sparse attribute combinations enough samples to clear
         * @c kMinCrossSatSamples instead of collapsing to the σ floor.
         *
         * @param stats_key  Per-arc key from @c mw_stats_.
         * @return Group key, or empty string on malformed input.
         */
        static std::string makeGroupKey(const std::string& stats_key);

        /**
         * @brief Finalize cross-sat σ from this epoch's staging pool.
         *
         * Computes MAD × 1.4826 at three breadth tiers from the same staging
         * pool: per (constellation × band-pair) → @c cross_sat_sigma_m_, per
         * constellation → @c cross_sat_sigma_cons_, and global (all
         * constellations except GLONASS) → @c cross_sat_sigma_global_.  Each
         * tier updates only when it clears @c kMinCrossSatSamples; otherwise
         * its previous σ persists (or no entry, if cold-start).  Then clears
         * @c cross_sat_devs_ for the next epoch.
         *
         * Called at the end of @c detectLargeMwResets.
         */
        void finalizeCrossSatSigmas();

        /**
         * @brief Process a single MW sample: update statistics or flag a slip.
         *
         * Handles the first-epoch initialization, adaptive threshold evaluation,
         * trace writing, and Welford/Hatch statistics update for one MW value.
         *
         * For BIA+BSSD-corrected arcs the WL integer is locked on the very
         * first epoch (the MW is near-integer immediately).  When a slip is
         * detected on a locked arc and the new sample is also near-integer,
         * the integer slip magnitude ?N_WL is written to @p out_slip_cycles
         * so the caller can attempt joint MW+IF repair.
         *
         * @param obs            Observation (for tracing metadata).
         * @param stats_key      Key into mw_stats_ for this arc.
         * @param mw_m           Current MW value (meters), already bias-corrected by caller.
         * @param lambda_wl      Wide-lane wavelength (meters).
         * @param sig1           First signal identifier (for tracing).
         * @param sig2           Second signal identifier (for tracing).
         * @param bia_corrected  True if the MW value has had both satellite (BIA)
         *                       and receiver (BSSD) biases removed, making it
         *                       near-integer in WL cycles and eligible for rounding.
         * @param sat_mw_bias    Satellite MW bias that was applied (meters, for tracing).
         * @param[out] out_slip_cycles  If non-null, set to the integer WL slip magnitude.
         * @param[out] out_wl_known     If non-null, set to true when out_slip_cycles is reliable.
         * @param[out] out_slip_frac    If non-null, set to |x - round(x)| of the float value
         *                              before rounding (0 = exact, up to 0.5 = worst).
         * @param pivot_sat      SD pivot satellite for this arc's (constellation,
         *                       signal-pair) group, recorded in the trace's
         *                       @c mw_pivot column.  Empty for undifferenced
         *                       (no-BIA) arcs that are not single-differenced
         *                       against any pivot.
         * @return True if a slip was detected (caller should generate reset keys).
         */
        bool processMwSample(const PppObservation& obs,
            const std::string& stats_key,
            double mw_m, double lambda_wl,
            const std::string& sig1, const std::string& sig2,
            bool bia_corrected, double sat_mw_bias,
            int* out_slip_cycles = nullptr,
            bool* out_wl_known = nullptr,
            double* out_slip_frac = nullptr,
            const std::string& pivot_sat = {});

        /** Output stream for MWWL trace CSV file (mutable for const access). */
        mutable std::ofstream mwwl_trace_;

        /** Current epoch time for tracing (set at start of detectLargeMwResets). */
        mutable GnssTime current_epoch_;

        /**
         * @brief Find ionosphere-free phase/code label pair for an observation.
         *
         * Scans the observation's observable labels and sets @p phase_if_key and
         * @p code_if_key when an ionosphere-free pair is found (for example
         * phase: "L1C_L2W", code: "C1C_C2W"). Returns true on success.
         *
         * @param obs Observation to inspect.
         * @param[out] phase_if_key Output ionosphere-free phase label (if found).
         * @param[out] code_if_key Output ionosphere-free code label (if found).
         * @return true when a matching ionosphere-free pair was found.
         * @return false otherwise.
         */
        bool findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const;

        /**
         * @brief Matched IF phase/code label pair.
         */
        struct IfPair {
            std::string phase_key;  ///< e.g. "L1W_L5Q"
            std::string code_key;   ///< e.g. "C1W_C5Q"
        };

        /**
         * @brief Collect all matched ionosphere-free phase/code pairs.
         *
         * Returns every IF phase label that has a corresponding code label
         * using the same pair of frequency bands.
         *
         * @param obs Observation to inspect.
         * @return Vector of matched pairs (may be empty).
         */
        std::vector<IfPair> collectIonoFreePairs(const PppObservation& obs) const;

        /**
         * @brief Split an IF label of the form "L1C_L2W" into its two component signals.
         *
         * Example: input "L1C_L2W" -> outputs ("L1C","L2W").
         *
         * @param label Input IF label.
         * @param[out] sig1 First signal token.
         * @param[out] sig2 Second signal token.
         * @return true when the label could be split into exactly two tokens.
         * @return false otherwise.
         */
        bool splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const;

        /**
         * @brief Compute the effective wide-lane wavelength (meters) for a given observation and signal pair.
         *
         * Uses PppUtils to obtain nominal frequencies/wavelengths for the signals
         * in @p sig1 and @p sig2, and computes the wide-lane wavelength in meters.
         *
         * @param obs Observation whose system is used to resolve frequencies.
         * @param sig1 First signal identifier (e.g. "L1C").
         * @param sig2 Second signal identifier (e.g. "L2W").
         * @return double Wide-lane wavelength in meters, or 0.0 on error.
         */
        double computeWideLaneWavelengthMeters(const PppObservation& obs, const std::string& sig1, const std::string& sig2) const;

        /**
         * @brief Compute the Melbourne-W�bbena Wide-Lane (MWWL) observable in meters.
         *
         * Computes the full MW combination:
         *   MW = (f1�L1 - f2�L2)/(f1-f2) - (f1�P1 + f2�P2)/(f1+f2)
         *
         * where L1, L2 are phase observables (meters) and P1, P2 are code
         * observables (meters). The result is geometry-free and ionosphere-free,
         * proportional to the wide-lane ambiguity plus noise.
         *
         * Code signals are provided explicitly (from the iono-free code
         * combination) rather than derived from the phase labels, to ensure
         * matched tracking modes on each frequency and avoid inter-signal
         * code biases that would introduce periodic trends.
         *
         * @param obs Observation containing raw phase/code values and metadata.
         * @param sig1 First phase signal identifier (e.g. "L1C").
         * @param sig2 Second phase signal identifier (e.g. "L2W").
         * @param code1 First code signal identifier (e.g. "C1W").
         * @param code2 Second code signal identifier (e.g. "C2W").
         * @return double MW value in meters. Returns NaN on invalid input.
         */
        double computeMwMeters(const PppObservation& obs,
            const std::string& sig1,
            const std::string& sig2,
            const std::string& code1,
            const std::string& code2) const;

        /**
         * @brief Compute the satellite MW bias from BIA phase and code entries.
         *
         * Computes:
         *   b_MW = (f1�b_L1 - f2�b_L2)/(f1-f2) - (f1�b_C1 + f2�b_C2)/(f1+f2)
         *
         * where b_L1/b_L2 and b_C1/b_C2 are phase and code biases (meters)
         * from the BIA product.  The result is in meters and can be subtracted
         * from the raw MW to produce a near-integer WL observable.
         *
         * @param obs   Observation (for satellite, system, time, frequency lookup).
         * @param sig1  First phase signal (e.g. "L1W").
         * @param sig2  Second phase signal (e.g. "L2W").
         * @param code1 First code signal (e.g. "C1W").
         * @param code2 Second code signal (e.g. "C2W").
         * @param[out] bias_m  Computed satellite MW bias in meters.
         * @return True if all four BIA entries were found and the bias is finite.
         */
        bool computeSatMwBias(const PppObservation& obs,
            const std::string& sig1, const std::string& sig2,
            const std::string& code1, const std::string& code2,
            double& bias_m) const;

        /**
         * @brief A dual-frequency pair extracted from uncombined observations.
         *
         * Groups the phase and code signal identifiers for two frequencies
         * so that an MW combination can be formed.
         */
        struct UncombinedPair {
            std::string phase1;  /**< Phase signal on freq 1 (e.g. "L1C"). */
            std::string phase2;  /**< Phase signal on freq 2 (e.g. "L2W"). */
            std::string code1;   /**< Code  signal on freq 1 (e.g. "C1W"). */
            std::string code2;   /**< Code  signal on freq 2 (e.g. "C2W"). */
        };

        /**
         * @brief Collect viable dual-frequency MW pairs from observations.
         *
         * Groups individual code and phase signals by RINEX band number (the
         * digit after the C/L prefix, e.g. "1" from "L1C"), then forms
         * all dual-frequency pairs that have both a code and a phase
         * observable on each band.
         *
         * Source and within-band multiplicity are controlled by the two
         * independent MW knobs @c GnssConfig::getMwFrequencyMode and
         * @c GnssConfig::getMwChannelMode (each resolves through Mirror
         * to the KF's setting):
         *  - mw_frequency_mode = Mirror (default): uses obs.observed
         *    (the KF's active bands), keeping the MW detector aligned
         *    with the Kalman filter.
         *  - mw_frequency_mode = Dual / Triple / All: uses obs.raw_observed
         *    for cross-frequency cross-consistency.
         *  - mw_channel_mode controls whether one canonical tracking
         *    attribute per band (Single) or every tracked channel (All)
         *    is kept.
         *
         * @param obs Observation to inspect.
         * @return std::vector<UncombinedPair> Viable MW pairs (may be empty).
         */
        std::vector<UncombinedPair> collectUncombinedPairs(const PppObservation& obs) const;

        /**
          * @brief Check whether the observation reports any loss-of-phase-lock indicators.
          *
          * Returns true when any of the observables in @p obs have non-zero LLI
          * flags indicating potential cycle slips or loss of lock.
          *
          * @param obs Observation to inspect.
          * @return true If any phase loss-of-lock indicators are present.
          * @return false Otherwise.
         */
        bool hasAnyPhaseLockLoss(const PppObservation& obs) const;

        /**
         * @brief Write a single MW observation record to the trace CSV.
         *
         * Called internally during detectLargeMwResets() when tracing is enabled.
         *
         * @param obs Observation being processed.
         * @param sig1 First signal identifier (e.g. "L1C").
         * @param sig2 Second signal identifier (e.g. "L2W").
         * @param mw_m Current MW value in meters.
         * @param mw_mean Running mean of MW values in meters.
         * @param dev_from_mean Deviation of current MW from running mean (meters).
         * @param mw_sigma MAD-derived sigma estimate (meters, NaN if unavailable).
         * @param arc_count Number of epochs in the current arc.
         * @param lambda_wl Wide-lane wavelength (meters).
         * @param thresh_m Threshold in meters.
         * @param is_slip True if this observation triggered a slip detection.
         * @param is_lli True if loss-of-lock was detected.
         * @param mw_pivot SD pivot satellite for this row's (constellation,
         *                 signal-pair) group; empty for undifferenced arcs.
        */
        void writeTraceRecord(const PppObservation& obs,
                            const std::string& sig1,
                            const std::string& sig2,
                            double mw_m,
                            double mw_mean,
                            double dev_from_mean,
                            double mw_sigma,
                            size_t arc_count,
                            double lambda_wl,
                            double thresh_m,
                            bool is_slip,
                            bool is_lli,
                            bool bia_corrected = false,
                            double sat_mw_bias_m = 0.0,
                            int rounded_wl = 0,
                            bool wl_locked = false,
                            const std::string& mw_pivot = {}) const;
        };

} // namespace GnssProcessor