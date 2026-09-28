
#pragma once

/**
 * @file GfSlipDetector.hpp
 * @brief Geometry-free (GF) cycle slip detector using second-order time-differencing.
 *
 * @note Status: staged and incomplete; not compiled and not wired into the
 *       pipeline.  The detector is excluded from the build (see CMakeLists.txt)
 *       and has no instantiation path in Application.  It is kept for a planned
 *       use: high-rate (>=1 Hz) real-time cycle-slip repair alongside the MW
 *       detector.  The high-rate requirement is intrinsic: at low rates the
 *       slowly varying ionospheric residual contaminates the GF combination and
 *       the second-order time difference no longer isolates the slip.  Slip
 *       detection is provided by the MW detector, so this remains future work.
 *       The GF math here is also used by slip/IonoVariabilityMonitor, which is
 *       compiled and active.
 *
 * The GF observable (L1 - L2 in meters) is geometry-free but contains the
 * ionospheric delay, which varies slowly over time.  A second-order time
 * difference removes the constant (ambiguity) and linear terms and reduces a
 * quadratic trend to a constant:
 *
 *   TD2_GF(t) = GF(t) - 2·GF(t-1) + GF(t-2) ≈ noise + slip
 *
 * This avoids the need for a pivot satellite (BSSD) and its associated
 * complexity (pivot selection, pivot changes, constellation coupling).
 *
 * The detector maintains per-satellite GF arc statistics with a
 * MAD-based adaptive threshold, similar to the MW detector.
 *
 * Safeguards against false-positive cascades:
 *   - Ionospheric variability index (IVI): per constellation and epoch,
 *     median(|TD2|) plus the MAD-derived sigma of |TD2| across satellites.
 *     When the IVI reaches @c kIviThreshold the ionosphere is deemed active
 *     and GF detections are suppressed (traced and logged, but not returned
 *     as slips).
 *   - Anti-echo cooldown: after a detected slip the arc restarts, and the
 *     first @c kCooldownEpochs TD2 tests on the new arc are suppressed so the
 *     TD2 echo signature of the prior event cannot retrigger.
 *   - Oscillation guard: a detection whose TD2 is opposite in sign and
 *     similar in magnitude to the previous slip on the same arc (within
 *     @c kOscillationTolerance) is suppressed as a likely echo artifact or
 *     ionospheric scintillation.
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
        std::string signal;            ///< Signal that slipped (e.g. "L2W"), empty if ambiguous (always empty from detect(), which reports both signals of the pair).
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
     * This cancels the ambiguity and the linear part of the ionospheric
     * delay (a quadratic part leaves a constant), leaving mainly noise and
     * any cycle-slip discontinuity.
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

        /** Minimum TD2 window entries before the adaptive threshold is used;
         *  no detection is made on an arc before then. */
        static constexpr size_t kMinEpochsForAdaptive = 5;

        /** MAD sigma multiplier for adaptive threshold. */
        static constexpr double kMadSigmaMultiplier = 6.0;

        /** Minimum slip threshold in meters (about half an L1 wavelength, 0.19 m). */
        static constexpr double kMinThresholdM = 0.10;

        /** Maximum inter-epoch gap (seconds) before arc reset. */
        static constexpr double kMaxGapSeconds = 120.0;

        /** Anti-echo cooldown after a slip detection (epochs). */
        static constexpr size_t kCooldownEpochs = 3;

        /** Ionospheric variability index threshold (meters).
         *  When a constellation's IVI (median plus MAD-derived sigma of |TD2|
         *  across its satellites) reaches this value, the ionosphere is deemed
         *  active and that constellation's GF detections are suppressed. */
        static constexpr double kIviThreshold = 0.06;

        /** Oscillation guard tolerance on the ratio of the current TD2 to the
         *  last slip's TD2.  If  td2_gf / last_slip_td2 ∈ (-1.0-tol, -1.0+tol)
         *  the detection is likely an echo and is suppressed. */
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
         * Takes the current-epoch TD2 values of the arcs in one constellation
         * and returns median(|TD2|) plus the MAD-derived sigma of |TD2|.  A high
         * IVI means the ionosphere is active across multiple satellites, which
         * cycle slips are unlikely to cause.
         *
         * @param constellation_td2 TD2 values (signed; the absolute value is
         *                          taken inside) for all arcs in a constellation
         *                          this epoch.
         * @return IVI in meters, or 0.0 when fewer than 3 values are given.
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
