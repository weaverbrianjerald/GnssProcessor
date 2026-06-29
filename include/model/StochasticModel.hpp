#pragma once

/**
 * @file StochasticModel.hpp
 * @brief Measurement stochastic (weighting) model for GNSS observables.
 *
 * Encapsulates the three weighting strategies available for code and
 * carrier-phase observations:
 *  - **Constant**  � use the a-priori ?� directly, no geometry scaling.
 *  - **Elevation** � classic  ?� / sin�(el)  weighting.
 *  - **SNR**       — variance-additive form: σ² = a² + b²·10^(-CN0/10),
 *                     with code-only chip-rate per-band scaling, Wieser
 *                     hybrid 1/sin²(el) elevation amplification on top,
 *                     and a synthesised CN0 fallback when the receiver-
 *                     reported S-observable is missing.  See
 *                     SnrModelParams in GnssConfig.hpp for the formula
 *                     details and references.
 *
 * The class is configured once from GnssConfig and then called by PppModel
 * for every individual variance computation.  It replaces the ad-hoc
 * variance logic that was previously scattered across PppModel methods.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include "common/ObsValueMap.hpp"
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
                               const ObsValueMap<double>& measurements,
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
                               const ObsValueMap<double>& measurements,
                               double elevation_deg,
                               double default_var,
                               const std::string& satellite,
                               const std::vector<AntennaData>& antennas) const;

    private:
        // Test seam: lets the StochasticModel unit tests call the private
        // computeElevation kernel directly (tests/test_stochastic_model.cpp),
        // independent of the configured model-type dispatch.
        friend struct StochasticModelTestAccess;

        /** @brief Compute variance under the Constant model (passthrough). */
        double computeConstant(double default_var) const;

        /**
         * @brief Apply elevation amplification with a configurable
         *        σ-power that depends on the observable type.
         *
         *   var_actual = var_zen / sin^(2·power)(elev)
         *
         * with @c power = @c code_elevation_power_ for @c 'C…' rows
         * and @c phase_elevation_power_ for @c 'L…' rows (and code's
         * power as a default for everything else).  Empty obs_type
         * uses the code power.  Near-zero elevations are clipped to
         * avoid amplifying to infinity — anything below the
         * configured elevation mask should be filtered upstream of
         * this function anyway.
         */
        double computeElevation(const std::string& obs_type,
                                double default_var,
                                double elevation_deg) const;

        /**
         * @brief Compute variance under the SNR model
         *        (variance-additive form, Wieser hybrid).
         *
         * Pipeline:
         *  1. Resolve CN0: read the matching @c S<band><attr>
         *     observable from @p measurements, or synthesise it from
         *     elevation (see @c synthesizeCn0FromElevation) when the
         *     S-observable is absent / non-positive.
         *  2. Clamp CN0 to @c [snr_floor_dbhz, snr_ceil_dbhz].
         *  3. Compute base variance:
         *       σ² = a² + b²·10^(-CN0/10)
         *     using the (a, b) pair appropriate to code or phase.
         *  4. Apply chip-rate per-band scaling on **code only**
         *     (reusing @c SatBlockNoiseParams::code::freq_band_scale).
         *     Phase σ² in this model is band-flat — chip rate doesn't
         *     enter the PLL noise formula.  Per-band phase structure
         *     lives in the SatelliteBlock model only, under
         *     @c SatBlockNoiseParams::phase::freq_band_scale.
         *  5. Apply Wieser hybrid elevation amplification:
         *     σ²_final = σ² / sin²(el), uniformly to code and phase.
         */
        double computeSnr(const std::string& obs_type,
                          const ObsValueMap<double>& measurements,
                          double elevation_deg,
                          double default_var) const;

        /**
         * @brief Estimate CN0 (dB-Hz) from satellite elevation using
         *        a typical antenna gain pattern.
         *
         * Used when the receiver-reported @c S-observable is missing
         * for a given measurement.  Linear-in-(1 - sin θ) drop from a
         * zenith maximum mirrors textbook antenna patterns
         * (Misra & Enge 2011 §6.2):
         *   CN0(θ) ≈ CN0_max - K·(1 - sin θ)
         * with @c CN0_max ≈ 50 dBHz and @c K ≈ 15 dB hard-coded as
         * receiver-class-typical defaults.  This keeps the noise
         * model coherent at the boundary where some sats have S
         * observables and others don't, instead of discontinuously
         * switching to a different functional form.
         */
        double synthesizeCn0FromElevation(double elevation_deg) const;

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

        StochasticModelType model_type_;    ///< Active weighting strategy.  Each model defines its own per-observable handling for phase vs code (see GnssConfig::getStochasticModel() docstring).
        SnrModelParams      snr_params_;    ///< Parameters for the SNR model.
        SatBlockNoiseParams sat_block_params_; ///< Parameters for the satellite-block model.
        double code_elevation_power_;       ///< σ-power for code elevation amplification (1.0 = legacy csc²(θ) on variance).
        double phase_elevation_power_;      ///< σ-power for phase elevation amplification (0.5 = gentler csc(θ) on variance).
        LogManager&         log_manager_;   ///< Logging manager reference.
    };

} // namespace GnssProcessor
