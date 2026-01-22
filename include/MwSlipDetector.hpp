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

    private:
        const GnssConfig& config_;    /**< Reference to processing configuration. */
        LogManager& log_manager_;     /**< Reference to logging manager. */
        PppUtils ppp_utils_;          /**< Utility helpers for frequency/wavelength lookups. */

        /** MW history: maps satellite identifier -> last observed MW-like value (meters). */
        std::map<std::string, double> last_mw_m_;

        /** Large slip threshold expressed in wide-lane cycles (default 10.0). */
        double large_mw_threshold_cycles_ = 10.0;

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
         * @brief Compute an MW-like quantity (in meters) for the provided observation.
         *
         * The MW-like metric is used to detect discontinuities between epochs. It
         * is formed from an ionosphere-free phase/code combination pair and scaled
         * to meters using the appropriate wide-lane wavelength.
         *
         * @param obs Observation containing phase/code IF values and metadata.
         * @param phase_if_key Ionosphere-free phase label (e.g. "L1C_L2W").
         * @param code_if_key Ionosphere-free code label (e.g. "C1C_C2W").
         * @return double MW-like value in meters. Returns NaN or 0.0 on invalid input depending on implementation.
         */
        double computeMwLikeMeters(const PppObservation& obs,
            const std::string& phase_if_key,
            const std::string& code_if_key) const;

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
    };

} // namespace GnssProcessor

#endif // MW_SLIP_DETECTOR_HPP