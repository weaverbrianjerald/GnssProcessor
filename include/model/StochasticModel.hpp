#pragma once

/**
 * @file StochasticModel.hpp
 * @brief Measurement stochastic (weighting) model for GNSS observables.
 *
 * Encapsulates the three weighting strategies available for code and
 * carrier-phase observations:
 *  - **Constant**  � use the a-priori ?� directly, no geometry scaling.
 *  - **Elevation** � classic  ?� / sin�(el)  weighting.
 *  - **SNR**       � SIGMA-? model:  ? = a + b � 10^(?SNR/10),
 *                     falls back to elevation-based weighting when no
 *                     SNR observable is available for a given measurement.
 *
 * The class is configured once from GnssConfig and then called by PppModel
 * for every individual variance computation.  It replaces the ad-hoc
 * variance logic that was previously scattered across PppModel methods.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <map>
#include <string>
#include <vector>

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
         *  - **Elevation**:  @p default_var / sin�(@p elevation_deg).
         *  - **SNR**: (a + b � 10^(?SNR/10))� using the matching RINEX 'S'
         *    observable from @p measurements; if no SNR is found, falls back
         *    to the Elevation result.
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
         * @param default_var   A-priori variance (m�) from the default noise
         *                      parameters.
         * @return Computed variance in m� (or Hz� / (m/s)� for Doppler).
         */
        double computeVariance(const std::string& obs_type,
                               const std::map<std::string, double>& measurements,
                               double elevation_deg,
                               double default_var) const;

        /**
         * @brief Compute measurement variance with satellite block awareness.
         *
         * When the active model is SatelliteBlock, uses @p satellite and
         * @p antennas to look up the ATX block type and derive a
         * block/frequency-dependent zenith noise, then applies 1/sin�(el)
         * scaling.  For other model types the additional parameters are
         * ignored and the result is identical to the 4-argument overload.
         *
         * @param obs_type      RINEX observable key (e.g. "C1W", "L1C").
         * @param measurements  All measurements for this satellite/epoch.
         * @param elevation_deg Satellite elevation angle in degrees.
         * @param default_var   Fallback variance (m�) when block lookup fails.
         * @param satellite     Satellite identifier (e.g. "G04").
         * @param antennas      Parsed ATX antenna entries for block lookup.
         * @return Computed variance in m�.
         */
        double computeVariance(const std::string& obs_type,
                               const std::map<std::string, double>& measurements,
                               double elevation_deg,
                               double default_var,
                               const std::string& satellite,
                               const std::vector<AntennaData>& antennas) const;

    private:
        /** @brief Compute variance under the Constant model (passthrough). */
        double computeConstant(double default_var) const;

        /** @brief Compute variance under the Elevation model. */
        double computeElevation(double default_var, double elevation_deg) const;

        /**
         * @brief Elevation-aware wrapper that respects the
         *        @c phaseElevationWeighting() config flag.
         *
         * Behaves exactly like @c computeElevation() except that when
         * @p obs_type is a phase observable ("L...") and phase elevation
         * weighting is disabled, the unscaled @p default_var is returned.
         * Code and non-measurement observables continue to receive the
         * standard @c 1/sin²(el) scaling.
         */
        double elevationScale(const std::string& obs_type,
                              double default_var,
                              double elevation_deg) const;

        /**
         * @brief Compute variance under the SNR (SIGMA-?) model.
         *
         * Falls back to computeElevation() when no matching SNR observable
         * is available.
         */
        double computeSnr(const std::string& obs_type,
                          const std::map<std::string, double>& measurements,
                          double elevation_deg,
                          double default_var) const;

        /**
         * @brief Compute variance under the SatelliteBlock model.
         *
         * Looks up the satellite's ATX block type, selects a zenith noise
         * based on block and frequency band, then applies 1/sin�(el).
         */
        double computeSatBlock(const std::string& obs_type,
                               double elevation_deg,
                               double default_var,
                               const std::string& satellite,
                               const std::vector<AntennaData>& antennas) const;

        StochasticModelType model_type_;    ///< Active weighting strategy.
        SnrModelParams      snr_params_;    ///< Parameters for the SNR model.
        SatBlockNoiseParams sat_block_params_; ///< Parameters for the satellite-block model.
        bool phase_elevation_weighting_{ true }; ///< Apply 1/sin²(el) to phase observables.
        LogManager&         log_manager_;   ///< Logging manager reference.
    };

} // namespace GnssProcessor
