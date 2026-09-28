#pragma once

/**
 * @file StochasticModel.hpp
 * @brief Measurement stochastic (weighting) model for GNSS observables.
 *
 * Encapsulates the four weighting strategies available for code and
 * carrier-phase observations:
 *  - **Constant**: the a-priori σ² is used directly, with no geometry scaling.
 *  - **Elevation**: σ² / sin^(2p)(el), with p the code or phase elevation
 *                     power (p = 1 gives the classic σ² / sin²(el)).
 *  - **SNR**: variance-additive form σ² = a² + b²·10^(-CN0/10),
 *                     with code-only chip-rate per-band scaling, Wieser
 *                     hybrid elevation amplification on top, and a
 *                     synthesised CN0 fallback when the receiver-reported
 *                     S-observable is missing.  See SnrModelParams in
 *                     GnssConfig.hpp for the parameters and references.
 *  - **SatelliteBlock**: zenith σ from the satellite's ATX block type,
 *                     scaled per frequency band and tracking attribute,
 *                     then elevation-amplified.
 *
 * The class is configured once from GnssConfig; PppModel holds one instance
 * and calls it for every individual variance computation.
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
         * Reads the stochastic model type, the SNR and satellite-block model
         * parameters and the code and phase elevation powers from @p config.
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
         *  - **Elevation**:  @p default_var / sin^(2p)(@p elevation_deg), with p
         *    the phase elevation power for 'L' rows and the code elevation
         *    power otherwise.
         *  - **SNR**: a² + b²·10^(-CN0/10), with CN0 read from the matching
         *    RINEX 'S' observable in @p measurements (synthesised from
         *    elevation when absent or non-positive) and clamped to the
         *    configured range, scaled per band for code, then divided by
         *    sin^(2p)(el) as in the Elevation model.
         *  - **SatelliteBlock**: without satellite information this overload
         *    returns the Elevation result.
         *
         * An empty @p obs_type returns @p default_var.  Under the SNR model,
         * Doppler ('D') and SNR ('S') observable types get the Elevation
         * result for @p default_var, because the SIGMA-ε model is defined
         * only for code and phase observables.
         *
         * @param obs_type      RINEX observable key (e.g. "C1W", "L1C", "D1C").
         * @param measurements  All measurements for the current satellite/epoch
         *                      (used to look up 'S' observables for SNR model).
         * @param elevation_deg Satellite elevation angle in degrees.
         * @param default_var   A-priori variance (m²) from the default noise
         *                      parameters.
         * @return Computed variance in m² (for Doppler, in the units of
         *         @p default_var: Hz² or (m/s)²).
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
         * block/frequency-dependent zenith noise, then applies the elevation
         * amplification (1/sin²(el) at the default power of 1.0).  Code
         * always takes this path; phase takes it only when
         * SatBlockNoiseParams::apply_to_phase is set, and otherwise uses
         * default_phase_std_m² with elevation amplification; other types get
         * elevation amplification of @p default_var.  For other model types
         * the additional parameters are ignored and the result is identical
         * to the 4-argument overload.
         *
         * @param obs_type      RINEX observable key (e.g. "C1W", "L1C").
         * @param measurements  All measurements for this satellite/epoch.
         * @param elevation_deg Satellite elevation angle in degrees.
         * @param default_var   Variance (m²) used for non-code, non-phase
         *                      types and by the other model types.
         * @param satellite     Satellite identifier (e.g. "G04").
         * @param antennas      Parsed ATX antenna entries for block lookup.
         * @return Computed variance in m².
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
         * with @c power = @c phase_elevation_power_ for @c 'L…' rows and
         * @c code_elevation_power_ for everything else (code, Doppler, and
         * an empty obs_type).  When sin(elev) ≤ 1e-6 the variance is returned
         * unscaled rather than amplified toward infinity; observations below
         * the elevation mask are expected to be filtered upstream.
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
         *     S-observable is absent or non-positive.
         *  2. Clamp CN0 to @c [snr_floor_dbhz, snr_ceil_dbhz].
         *  3. Compute base variance:
         *       σ² = a² + b²·10^(-CN0/10)
         *     using the (a, b) pair appropriate to code or phase.
         *  4. Apply chip-rate per-band scaling on code only
         *     (reusing @c SatBlockNoiseParams::code::freq_band_scale).
         *     Phase σ² in this model is band-flat, because chip rate does
         *     not enter the PLL noise formula.  Per-band phase structure
         *     lives in the SatelliteBlock model only, under
         *     @c SatBlockNoiseParams::phase::freq_band_scale.
         *  5. Apply Wieser hybrid elevation amplification through
         *     computeElevation: σ²_final = σ² / sin^(2p)(el), with the code
         *     or phase power (σ² / sin²(el) at the default p = 1.0).
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
         * for a given measurement.  CN0 drops linearly in (1 - sin θ)
         * from a zenith maximum, following textbook antenna patterns
         * (Misra & Enge 2011 §6.2):
         *   CN0(θ) ≈ CN0_max - K·(1 - sin θ)
         * with @c CN0_max = 50 dBHz and @c K = 15 dB hard-coded as
         * receiver-class-typical defaults (sin θ is clipped at 0).  The
         * noise model therefore stays continuous between satellites that
         * report S observables and those that do not, instead of switching
         * to a different functional form.
         */
        double synthesizeCn0FromElevation(double elevation_deg) const;

        /**
         * @brief Compute variance under the SatelliteBlock model.
         *
         * Looks up the satellite's ATX block type, selects a zenith noise
         * based on block type, scales it by the frequency-band and
         * tracking-attribute factors, then applies the elevation
         * amplification of computeElevation (1/sin²(el) at the default
         * power of 1.0).
         */
        double computeSatBlock(const std::string& obs_type,
                               double elevation_deg,
                               double default_var,
                               const std::string& satellite,
                               const std::vector<AntennaData>& antennas) const;

        StochasticModelType model_type_;    ///< Active weighting strategy.  Each model defines its own per-observable handling for phase vs code (see GnssConfig::getStochasticModel() docstring).
        SnrModelParams      snr_params_;    ///< Parameters for the SNR model.
        SatBlockNoiseParams sat_block_params_; ///< Parameters for the satellite-block model.
        double code_elevation_power_;       ///< σ-power for code elevation amplification (default 1.0 = csc²(θ) on variance).
        double phase_elevation_power_;      ///< σ-power for phase elevation amplification (default 1.0 = csc²(θ) on variance, as for code; 0.5 selects the gentler csc(θ)).
        LogManager&         log_manager_;   ///< Logging manager reference.
    };

} // namespace GnssProcessor
