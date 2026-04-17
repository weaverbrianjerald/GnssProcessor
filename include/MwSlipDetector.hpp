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

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include "GnssCorrections.hpp"
#include "LogManager.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
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
     */
    struct MwSlipResult {
        std::string amb_key;           ///< Ambiguity key (e.g. "G01_L1C").
        int    slip_wl_cycles{ 0 };    ///< WL slip dN_WL in cycles (0 if unknown).
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
         * @brief Get all mature, non-slipped MW arc keys.
         *
         * Returns every stats key that has an active MW arc with enough
         * epoch history (diff_window >= kMinEpochsForAdaptive) to have
         * detected a slip if one occurred.  Immature arcs are excluded
         * because their slip-detection is deferred — treating them as
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

        /** Current BSSD pivot satellite per (constellation char + signal-pair key).
         *  Example key: "G_L1C_L2W".  When the pivot changes all SD arcs
         *  for that group are reset because the SD ambiguity changes. */
        std::map<std::string, std::string> sd_pivot_sat_;

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

        /** Multiplier applied to the MAD-derived sigma for the adaptive threshold.
         *  A value of 4.0 corresponds to ~0.006% false alarm rate for Gaussian
         *  noise, while keeping the detector sensitive to slips >= 2 WL cycles. */
        static constexpr double kMadSigmaMultiplier = 8.0;

        /**
         * @brief Minimum number of epochs before the adaptive (MAD-based)
         *        threshold is used.  Until this count is reached a fixed
         *        fallback threshold of kInitialThresholdCycles is applied. */
        static constexpr size_t kMinEpochsForAdaptive = 5;

        /** Fixed slip threshold (in wide-lane cycles) applied during the
         *  first kMinEpochsForAdaptive epochs before the MAD-based adaptive
         *  threshold becomes available.  Set at 1.5 WL cycles to reliably
         *  catch 2-cycle slips on new arcs while staying above the noise
         *  floor of a single MW sample (~0.5 WL cycles RMS from code). */
        static constexpr double kInitialThresholdCycles = 1.5;

        /** Minimum slip threshold expressed in wide-lane cycles.
         *  Prevents the adaptive threshold from becoming unrealistically
         *  tight when MW noise is very low.  A floor of 0.5 WL cycles
         *  reliably detects 1-cycle slips on quiet arcs (the deviation
         *  is ~1.0 * lambda_wl, well above 0.5) while remaining above
         *  typical epoch-to-epoch MW code noise (~0.15-0.3 WL cycles
         *  RMS).  Smaller sub-cycle noise fluctuations are never
         *  flagged; any residual sub-cycle effects are left to the
         *  post-fit standardized residual check which is more
         *  sensitive. */
        static constexpr double kMinThresholdCycles = 0.5;

        /** Maximum inter-epoch gap (seconds) before the arc is reset.
         *  If more than this many seconds elapse between consecutive MW
         *  samples for a satellite the running statistics are discarded
         *  and a fresh arc is started.  This prevents stale pre-gap
         *  statistics from contaminating the post-gap mean / MAD and
         *  avoids the large spurious deviations observed after data
         *  gaps. */
        static constexpr double kMaxGapSeconds = 120.0;

        /** Minimum number of consecutive epochs in a BIA-corrected arc
         *  before the WL integer is locked by rounding.  Locking on the
         *  very first epoch is dangerous because a single MW sample is
         *  dominated by code noise (especially for EWL pairs with large
         *  lambda_wl).  Waiting for a short averaging window lets the
         *  Hatch mean converge to within a fraction of a WL cycle before
         *  the irreversible rounding decision is made. */
        static constexpr size_t kMinEpochsForWlLock = 5;

        /**
         * @brief Compute the MAD-based adaptive threshold for a satellite arc.
         *
         * Uses the rolling window of epoch-to-epoch MW differences to compute
         * the MAD, then scales it to a Gaussian-equivalent sigma and applies
         * a multiplier.  If the arc does not yet have enough history a fixed
         * fallback threshold of kInitialThresholdCycles wide-lane cycles is
         * returned so that large slips are still caught during arc warm-up.
         *
         * @param stats  Current arc statistics for the satellite.
         * @param lambda_wl Wide-lane wavelength (meters).
         * @return double Adaptive threshold in meters.
         */
        double computeAdaptiveThreshold(const MwArcStats& stats, double lambda_wl) const;

        /**
         * @brief Compute the MAD-derived Gaussian-equivalent sigma from an arc's diff window.
         * @param stats  Arc statistics whose diff_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeMadSigma(const MwArcStats& stats);

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
         * @return True if a slip was detected (caller should generate reset keys).
         */
        bool processMwSample(const PppObservation& obs,
            const std::string& stats_key,
            double mw_m, double lambda_wl,
            const std::string& sig1, const std::string& sig2,
            bool bia_corrected, double sat_mw_bias,
            int* out_slip_cycles = nullptr,
            bool* out_wl_known = nullptr,
            double* out_slip_frac = nullptr);

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
         * @brief Compute the Melbourne-Wübbena Wide-Lane (MWWL) observable in meters.
         *
         * Computes the full MW combination:
         *   MW = (f1·L1 - f2·L2)/(f1-f2) - (f1·P1 + f2·P2)/(f1+f2)
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
         *   b_MW = (f1·b_L1 - f2·b_L2)/(f1-f2) - (f1·b_C1 + f2·b_C2)/(f1+f2)
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
         * The signal source depends on GnssConfig::getMwSignalMode():
         *  - Filter (default): uses obs.observed (KF-selected bands only),
         *    keeping the MW detector aligned with the Kalman filter's
         *    signal_mode and avoiding noisy EWL pairs the filter does not
         *    process.
         *  - All: uses obs.raw_observed (every available RINEX band),
         *    giving maximum multi-frequency coverage at the cost of
         *    including noisier wide-lane combinations.
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
                            bool wl_locked = false) const;
        };

} // namespace GnssProcessor