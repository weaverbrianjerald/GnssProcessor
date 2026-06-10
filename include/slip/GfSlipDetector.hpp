
#pragma once

/**
 * @file GfSlipDetector.hpp
 * @brief Geometry-free (GF) cycle slip detector using second-order time-differencing.
 *
 * @note STATUS: STAGED / INCOMPLETE — not compiled or wired into the pipeline.
 *       This detector is intentionally excluded from the build (see
 *       CMakeLists.txt) and has no instantiation path in Application. It is
 *       kept (not abandoned) for a planned use: high-rate (>=1 Hz) real-time
 *       cycle-slip *repair* alongside the MW detector. The high-rate
 *       requirement is intrinsic — at low rates the slowly varying
 *       ionospheric residual contaminates the GF combination and the
 *       second-order time difference no longer isolates the slip. The MW
 *       detector currently provides slip detection with good performance, so
 *       this remains future work. The GF math here has also been reused by
 *       slip/IonoVariabilityMonitor (which IS compiled and active).
 *
 * The GF observable (L1 - L2 in meters) is geometry-free but contains the
 * ionospheric delay, which varies slowly over time.  A second-order time
 * difference eliminates linear and quadratic trends:
 *
 *   TD2_GF(t) = GF(t) - 2·GF(t-1) + GF(t-2) ? noise + slip
 *
 * This avoids the need for a pivot satellite (BSSD) and its associated
 * complexity (pivot selection, pivot changes, constellation coupling).
 *
 * The detector maintains per-satellite GF arc statistics with a
 * MAD-based adaptive threshold, similar to the MW detector.
 *
 * Robustness features to avoid false-positive cascades:
 *   - Ionospheric variability index (IVI): a cross-satellite MAD of TD2
 *     values is tracked per constellation.  When the IVI exceeds a
 *     threshold the ionosphere is deemed active and GF detections are
 *     suppressed (flagged but not emitted as slips).
 *   - Anti-echo cooldown: after an arc reset, detections are suppressed
 *     for a configurable number of epochs so the TD2 echo signature of a
 *     prior event cannot retrigger.
 *   - Oscillation guard: consecutive opposite-sign detections on the same
 *     arc (equal-but-opposite magnitudes) are suppressed as likely echo
 *     artifacts or ionospheric scintillation.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Result of GF slip detection for a single signal.
     */
    struct GfSlipResult {
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        std::string signal;            ///< Signal that slipped (e.g. "L2W"), empty if ambiguous.
        std::string amb_key;           ///< Ambiguity key to reset (e.g. "G01_L2W").
        bool iono_stable{ true };      ///< True if ionosphere was stable when detected.
    };

    /**
     * @class GfSlipDetector
     * @brief Detects cycle slips via second-order time-differenced geometry-free observables.
     *
     * For each dual-frequency pair the detector computes:
     *   GF(t) = L1(t, meters) - L2(t, meters)
     * per satellite, then forms a second-order time difference:
     *   TD2_GF(t) = GF(t) - 2·GF(t-1) + GF(t-2)
     * This cancels the ionospheric delay up to quadratic order, leaving
     * only noise (and any cycle-slip discontinuity).
     *
     * A MAD-based adaptive threshold flags outliers as slips.  Multiple
     * safeguards prevent false-positive cascades during ionospheric
     * activity (see file-level documentation).
     */
    class GfSlipDetector {
    public:
        /**
         * @brief Construct a new GfSlipDetector.
         * @param config Configuration providing thresholds.
         * @param log_manager Log manager for diagnostic messages.
         */
        GfSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect GF cycle slips for the provided epoch observations.
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return Vector of GF slip results (signals that slipped).
         */
        std::vector<GfSlipResult> detect(const std::vector<PppObservation>& observations);

        /**
         * @brief Clear GF arc history for a specific satellite.
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite);

        /**
         * @brief Enable CSV tracing for GF diagnostics.
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Check if tracing is currently enabled.
         */
        bool isTracingEnabled() const { return trace_file_.is_open(); }

        /**
         * @brief Query whether the ionosphere is currently stable.
         *
         * Returns the global (worst-case across constellations) assessment
         * from the most recent detect() call.  Useful for the CycleSlipManager
         * to decide whether GF results are trustworthy for repair decisions.
         *
         * @return True if ionosphere is assessed as stable.
         */
        bool isIonoStable() const { return iono_stable_global_; }

        /**
         * @brief Get the current ionospheric variability index (IVI).
         *
         * Returns the worst-case (maximum) IVI across all constellations
         * from the most recent detect() call.  Units are meters.
         * A value < kIviThreshold indicates stable ionosphere.
         *
         * @return IVI in meters, or 0.0 if not yet computed.
         */
        double getIonoVariabilityIndex() const { return iono_variability_global_; }

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        /**
         * @brief A dual-frequency pair extracted from uncombined observations.
         */
        struct DualFreqPair {
            std::string phase1;  ///< Phase signal on freq 1 (e.g. "L1C").
            std::string phase2;  ///< Phase signal on freq 2 (e.g. "L2W").
        };

        /**
         * @brief Per-satellite GF arc statistics.
         */
        struct GfArcStats {
            double prev_gf{ 0.0 };           ///< GF value at epoch t-1 (meters).
            double prev_prev_gf{ 0.0 };      ///< GF value at epoch t-2 (meters).
            double last_epoch_s{ 0.0 };      ///< Continuous GPS seconds of last update.
            size_t count{ 0 };               ///< Number of epochs in this arc.
            std::deque<double> diff_window;  ///< Rolling window of TD2 GF values.
            size_t cooldown_remaining{ 0 };  ///< Anti-echo cooldown (epochs to skip detection).
            double last_slip_td2{ 0.0 };     ///< TD2 value of last flagged slip (for oscillation guard).
            bool   had_slip{ false };        ///< Whether this arc has ever flagged a slip.
        };

        /** Per-(satellite + signal-pair) GF arc statistics.
         *  Key: "G01_L1C_L2W" */
        std::map<std::string, GfArcStats> gf_stats_;

        /** Maximum number of recent TD values for MAD computation. */
        static constexpr size_t kGfWindowSize = 30;

        /** Minimum epochs before adaptive threshold is used. */
        static constexpr size_t kMinEpochsForAdaptive = 5;

        /** MAD sigma multiplier for adaptive threshold. */
        static constexpr double kMadSigmaMultiplier = 6.0;

        /** Minimum slip threshold in meters (roughly 1 L1 wavelength). */
        static constexpr double kMinThresholdM = 0.10;

        /** Maximum inter-epoch gap (seconds) before arc reset. */
        static constexpr double kMaxGapSeconds = 120.0;

        /** Anti-echo cooldown after a slip detection (epochs). */
        static constexpr size_t kCooldownEpochs = 3;

        /** Ionospheric Variability Index threshold (meters).
         *  When the cross-satellite MAD of |TD2| exceeds this value the
         *  ionosphere is deemed active and GF detections are suppressed. */
        static constexpr double kIviThreshold = 0.06;

        /** Oscillation guard: ratio of current TD2 to last slip TD2.
         *  If   last_slip_td2 / td2_gf ? [-1.0-tol, -1.0+tol]  the
         *  detection is likely an echo and is suppressed. */
        static constexpr double kOscillationTolerance = 0.3;

        /**
         * @brief Collect viable dual-frequency phase pairs from an observation.
         */
        std::vector<DualFreqPair> collectDualFreqPairs(const PppObservation& obs) const;

        /**
         * @brief Compute GF observable (L1 - L2) in meters.
         */
        double computeGfMeters(const PppObservation& obs,
                               const std::string& sig1,
                               const std::string& sig2) const;

        /**
         * @brief Compute MAD-derived sigma from a diff window.
         */
        static double computeMadSigma(const std::deque<double>& window);

        /**
         * @brief Compute adaptive threshold from arc stats.
         */
        double computeAdaptiveThreshold(const GfArcStats& stats) const;

        /**
         * @brief Compute ionospheric variability index for a constellation.
         *
         * Collects all current-epoch |TD2| values for arcs in the given
         * constellation and returns the MAD of those values.  High IVI
         * means the ionosphere is active across multiple satellites,
         * which is unlikely to be caused by cycle slips.
         *
         * @param constellation_td2 Vector of |TD2| values for all arcs
         *                          in a constellation this epoch.
         * @return IVI in meters, or 0.0 if insufficient data.
         */
        static double computeIvi(const std::vector<double>& constellation_td2);

        /** Output stream for GF trace CSV file. */
        mutable std::ofstream trace_file_;

        /** Current epoch time for tracing. */
        mutable GnssTime current_epoch_;

        /** Global iono-stability flag from last detect() call. */
        bool iono_stable_global_{ true };

        /** Global iono variability index from last detect() call. */
        double iono_variability_global_{ 0.0 };
    };

} // namespace GnssProcessor
