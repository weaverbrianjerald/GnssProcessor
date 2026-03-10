#pragma once
#ifndef MW_SLIP_DETECTOR_HPP
#define MW_SLIP_DETECTOR_HPP

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
#include "LogManager.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

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
         * Iterates provided observations and returns a vector of ambiguity keys
         * that should be reset due to a large MW discontinuity. Ambiguity keys
         * are formatted to match the StateManager / ambiguity map keys used in
         * the rest of the codebase (for example: "G01_L1C_L2W").
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return std::vector<std::string> Vector of ambiguity keys to reset.
         */
        std::vector<std::string> detectLargeMwResets(const std::vector<PppObservation>& observations);

        /**
         * @brief Set the threshold (in wide-lane cycles) used to flag a large MW reset.
         * @param cycles Threshold in cycles (default previously 10.0).
         */
        void setLargeThresholdCycles(double cycles) { large_mw_threshold_cycles_ = cycles; }

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

        private:
        const GnssConfig& config_;    /**< Reference to processing configuration. */
        LogManager& log_manager_;     /**< Reference to logging manager. */
        PppUtils ppp_utils_;          /**< Utility helpers for frequency/wavelength lookups. */

        /**
         * @brief Per-satellite running statistics for the MW time series.
         *
         * Maintains a recursively updated mean and Welford variance estimate,
         * plus a rolling window of recent MW values used to compute the
         * Median Absolute Deviation (MAD) for robust outlier detection.
         */
        struct MwArcStats {
            double mean{ 0.0 };        /**< Running (Hatch) mean of MW values (meters). */
            double M2{ 0.0 };          /**< Welford accumulator for variance (m^2). */
            size_t count{ 0 };         /**< Number of MW samples accumulated. */
            std::deque<double> window; /**< Rolling window of recent MW values (meters). */
        };

        /** Per-satellite MW arc statistics keyed by satellite identifier. */
        std::map<std::string, MwArcStats> mw_stats_;

        /** Maximum number of recent MW values kept per satellite for MAD computation. */
        static constexpr size_t kMwWindowSize = 20;

        /** Multiplier applied to the MAD-derived sigma for the adaptive threshold. */
        static constexpr double kMadSigmaMultiplier = 4.0;

        /**
         * @brief Minimum number of epochs before the adaptive (MAD-based)
         *        threshold is used.  Until this count is reached the fixed
         *        large_mw_threshold_cycles_ is applied instead.
         */
        static constexpr size_t kMinEpochsForAdaptive = 5;

        /** Minimum slip threshold expressed in wide-lane cycles.
         *  Prevents false alarms during early convergence. */
        static constexpr double kMinThresholdCycles = 0.5;

        /** Large slip threshold expressed in wide-lane cycles (default 10.0).
         *  Used as fallback before adaptive statistics are available. */
        double large_mw_threshold_cycles_ = 10.0;

        /**
         * @brief Compute the MAD-based adaptive threshold for a satellite arc.
         *
         * Returns the threshold in meters.  If the arc does not yet have
         * enough history the method returns the fallback fixed threshold.
         *
         * @param stats  Current arc statistics for the satellite.
         * @param lambda_wl Wide-lane wavelength (meters).
         * @return double Adaptive threshold in meters.
         */
        double computeAdaptiveThreshold(const MwArcStats& stats, double lambda_wl) const;

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
         * @param obs Observation containing raw phase/code values and metadata.
         * @param sig1 First signal identifier (e.g. "L1C").
         * @param sig2 Second signal identifier (e.g. "L2W").
         * @return double MW value in meters. Returns NaN on invalid input.
         */
        double computeMwMeters(const PppObservation& obs,
            const std::string& sig1,
            const std::string& sig2) const;

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
                            bool is_lli) const;
        };

} // namespace GnssProcessor

#endif // MW_SLIP_DETECTOR_HPP