#pragma once

/**
 * @file StochasticModel.hpp
 * @brief Measurement stochastic (weighting) model for GNSS observables.
 *
 * Encapsulates the three weighting strategies available for code and
 * carrier-phase observations:
 *  - **Constant**  — use the a-priori ?² directly, no geometry scaling.
 *  - **Elevation** — classic  ?² / sin²(el)  weighting.
 *  - **SNR**       — SIGMA-? model:  ? = a + b · 10^(?SNR/10),
 *                     falls back to constant when no SNR observable is
 *                     available for a given measurement.
 *
 * The class is configured once from GnssConfig and then called by PppModel
 * for every individual variance computation.  It replaces the ad-hoc
 * variance logic that was previously scattered across PppModel methods.
 */

#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include <map>
#include <string>

namespace GnssProcessor {

    /**
     * @class StochasticModelWeight
     * @brief Computes observation variances according to a configurable weighting model.
     *
     * Usage:
     * @code
     *   StochasticModelWeight sm(config, log_manager);
     *   double var = sm.computeVariance("C1W", obs.measurements, elevation_deg, default_code_var);
     * @endcode
     */
    class StochasticModelWeight {
    public:
        /**
         * @brief Construct from global configuration.
         *
         * Reads the stochastic model type, default noise values and
         * SNR model parameters from @p config.
         *
         * @param config  Read-only global GNSS processing configuration.
         * @param log_mgr Logging manager for trace/debug output.
         */
        StochasticModelWeight(const GnssConfig& config, LogManager& log_mgr);

        /**
         * @brief Compute the measurement variance for a single observable.
         *
         * Depending on the configured StochasticModelType the variance is:
         *  - **Constant**:  @p default_var  (no scaling).
         *  - **Elevation**:  @p default_var / sin²(@p elevation_deg).
         *  - **SNR**: (a + b · 10^(?SNR/10))² using the matching RINEX 'S'
         *    observable from @p measurements; if no SNR is found, falls back
         *    to the Constant result.
         *
         * For Doppler ('D') and SNR ('S') observable types the returned
         * variance is always the @p default_var (possibly elevation-scaled
         * when the elevation model is active) because the SIGMA-? model is
         * defined only for code and phase observables.
         *
         * @param obs_type      RINEX observable key (e.g. "C1W", "L1C", "D1C").
         * @param measurements  All measurements for the current satellite/epoch
         *                      (used to look up 'S' observables for SNR model).
         * @param elevation_deg Satellite elevation angle in degrees.
         * @param default_var   A-priori variance (m²) from the default noise
         *                      parameters.
         * @return Computed variance in m² (or Hz² / (m/s)² for Doppler).
         */
        double computeVariance(const std::string& obs_type,
                               const std::map<std::string, double>& measurements,
                               double elevation_deg,
                               double default_var) const;

    private:
        /** @brief Compute variance under the Constant model (passthrough). */
        double computeConstant(double default_var) const;

        /** @brief Compute variance under the Elevation model. */
        double computeElevation(double default_var, double elevation_deg) const;

        /**
         * @brief Compute variance under the SNR (SIGMA-?) model.
         *
         * Falls back to computeConstant() when no matching SNR observable
         * is available.
         */
        double computeSnr(const std::string& obs_type,
                          const std::map<std::string, double>& measurements,
                          double default_var) const;

        StochasticModelType model_type_;    ///< Active weighting strategy.
        SnrModelParams      snr_params_;    ///< Parameters for the SNR model.
        LogManager&         log_manager_;   ///< Logging manager reference.
    };

} // namespace GnssProcessor
