/**
 * @file StochasticModel.cpp
 * @brief Implementation of the StochasticModelWeight class.
 */

#include "model/StochasticModel.hpp"
#include "common/Constants.hpp"
#include <cmath>
#include <algorithm>
#include <cctype>

namespace GnssProcessor {

    // ===================================================================
    // Construction
    // ===================================================================

    StochasticModelWeight::StochasticModelWeight(const GnssConfig& config, LogManager& log_mgr)
        : model_type_(config.getStochasticModel()),
          snr_params_(config.getSnrModelParams()),
          sat_block_params_(config.getSatBlockNoiseParams()),
          code_elevation_power_(config.getCodeElevationPower()),
          phase_elevation_power_(config.getPhaseElevationPower()),
          log_manager_(log_mgr) {

        const char* name = "unknown";
        switch (model_type_) {
        case StochasticModelType::Constant:       name = "Constant";       break;
        case StochasticModelType::Elevation:      name = "Elevation";      break;
        case StochasticModelType::SNR:            name = "SNR";            break;
        case StochasticModelType::SatelliteBlock: name = "SatelliteBlock"; break;
        }
        log_manager_.getLogger("stochastic_model")->info(
            "StochasticModelWeight initialised: type={}, elev_power=(code={:.2f}, phase={:.2f})",
            name, code_elevation_power_, phase_elevation_power_);
    }

    // ===================================================================
    // Public interface
    // ===================================================================

    double StochasticModelWeight::computeVariance(
        const std::string& obs_type,
        const std::map<std::string, double>& measurements,
        double elevation_deg,
        double default_var) const {

        if (obs_type.empty()) return default_var;

        const char mt = obs_type[0];
        const bool is_code_or_phase = (mt == 'C' || mt == 'L');

        switch (model_type_) {
        case StochasticModelType::Constant:
            return computeConstant(default_var);

        case StochasticModelType::Elevation:
            return computeElevation(obs_type, default_var, elevation_deg);

        case StochasticModelType::SNR:
            if (is_code_or_phase) {
                return computeSnr(obs_type, measurements, elevation_deg, default_var);
            }
            return computeElevation(obs_type, default_var, elevation_deg);

        case StochasticModelType::SatelliteBlock:
            // Without satellite info, fall back to elevation weighting.
            return computeElevation(obs_type, default_var, elevation_deg);
        }

        return default_var;
    }

    double StochasticModelWeight::computeVariance(
        const std::string& obs_type,
        const std::map<std::string, double>& measurements,
        double elevation_deg,
        double default_var,
        const std::string& satellite,
        const std::vector<AntennaData>& antennas) const {

        if (model_type_ != StochasticModelType::SatelliteBlock || obs_type.empty()) {
            return computeVariance(obs_type, measurements, elevation_deg, default_var);
        }

        const char mt = obs_type[0];
        if (mt == 'C') {
            // Code: full block + freq + 1/sin²(el) pipeline.
            return computeSatBlock(obs_type, elevation_deg, default_var, satellite, antennas);
        }
        if (mt == 'L') {
            // Phase under SatelliteBlock intentionally bypasses the
            // block table and freq_band_scale.  Per-block carrier-
            // loop noise differential is below the multipath /
            // atmospheric noise floor in practice (see
            // docs/report/observation_equations.tex §4 for the PLL-
            // physics argument).  The chip-rate freq scaling is
            // also not applicable to phase post-correlation.  So
            // phase reduces to just measurement_noise.phase_std_m
            // (≡ sat_block_params_.default_phase_std_m at config-
            // load time) amplified by sin^(-2·phase_elevation_power).
            const double phase_zen = sat_block_params_.default_phase_std_m;
            return computeElevation(obs_type, phase_zen * phase_zen, elevation_deg);
        }
        // Doppler ('D') and other non-{code,phase} types: plain
        // elevation amplification of the supplied default_var (uses
        // code_elevation_power).
        return computeElevation(obs_type, default_var, elevation_deg);
    }

    // ===================================================================
    // Private strategy implementations
    // ===================================================================

    double StochasticModelWeight::computeConstant(double default_var) const {
        return default_var;
    }

    double StochasticModelWeight::computeElevation(const std::string& obs_type,
                                                    double default_var,
                                                    double elevation_deg) const {
        const double sin_el = std::sin(elevation_deg * Constants::PI / 180.0);
        if (sin_el <= 1e-6) {
            return default_var; // near-zero elevation — don't amplify to infinity
        }
        // var_actual = var_zen / sin^(2·power)(elev).  Power = 1.0 on
        // code (gives legacy csc²(θ) scaling); power = 0.5 on phase
        // (gives csc(θ), gentler at low elevation).  Empty obs_type
        // (rare; fallback paths) uses the code power.
        const double power = (!obs_type.empty() && obs_type[0] == 'L')
            ? phase_elevation_power_
            : code_elevation_power_;
        return default_var * std::pow(sin_el, -2.0 * power);
    }

    double StochasticModelWeight::computeSnr(
        const std::string& obs_type,
        const std::map<std::string, double>& measurements,
        double elevation_deg,
        double default_var) const {

        if (obs_type.empty()) return default_var;

        // -------- (1) Resolve CN0 ---------------------------------
        // Build the matching SNR key: replace leading C/L with S.
        // Missing/non-positive S obs falls back to a synthesised CN0
        // from elevation rather than a different functional form, so
        // the noise curve stays continuous at the boundary.
        std::string snr_key = obs_type;
        snr_key[0] = 'S';
        bool synthetic = false;
        double snr_dbhz = 0.0;
        auto it = measurements.find(snr_key);
        if (it != measurements.end() && it->second > 0.0) {
            snr_dbhz = it->second;
        } else {
            snr_dbhz = synthesizeCn0FromElevation(elevation_deg);
            synthetic = true;
        }

        // Clamp to configured working range.
        if (snr_dbhz < snr_params_.snr_floor_dbhz) snr_dbhz = snr_params_.snr_floor_dbhz;
        if (snr_dbhz > snr_params_.snr_ceil_dbhz)  snr_dbhz = snr_params_.snr_ceil_dbhz;

        // -------- (2) Variance-additive base ----------------------
        // Hadas & Bosy 2015: σ² = a² + b² · 10^(-CN0/10).  This is
        // mathematically the right composition for two independent
        // noise contributions (the high-SNR floor a and the SNR-
        // dependent term b).  The legacy σ-additive form
        // σ = a + b·10^(-CN0/10) generated an unphysical 2ab cross-
        // term on squaring.
        const bool is_code = (obs_type[0] == 'C');
        const double a = is_code ? snr_params_.code_a : snr_params_.phase_a;
        const double b = is_code ? snr_params_.code_b : snr_params_.phase_b;
        const double cn0_factor = std::pow(10.0, -snr_dbhz / 10.0);
        double var = a * a + b * b * cn0_factor;

        // -------- (3) Per-band scaling (code only) ----------------
        // Chip-rate-driven scaling reduces code σ on wider-bandwidth
        // signals (L5/E5a at 10× the L1 chip rate get ~√10 less
        // correlator-noise σ).  Reuse the SatelliteBlock model's
        // freq_band_scale table so both models stay calibrated to
        // the same per-band factors.  Phase σ has no chip-rate
        // dependence post-correlation (PLL noise is set by carrier-
        // loop bandwidth and SNR only) — leave it band-flat.
        if (is_code && obs_type.size() >= 2) {
            const char band = obs_type[1];
            auto fit = sat_block_params_.freq_band_scale.find(band);
            if (fit != sat_block_params_.freq_band_scale.end()) {
                const double s = fit->second;
                var *= s * s;
            }
        }

        log_manager_.getLogger("stochastic_model")->trace(
            "SNR: obs={}, cn0={:.1f} dBHz{}, sigma_pre_elev={:.4f} m",
            obs_type, snr_dbhz, synthetic ? " (synthesised)" : "", std::sqrt(var));

        // -------- (4) Wieser hybrid elevation amplification -------
        // σ²_final = σ²_SNR / sin^(2·power)(el).  Captures the
        // elevation-correlated multipath that pure SNR weighting
        // under-weights (the carrier loop tracks fine even when
        // multipath is corrupting the range).  computeElevation
        // applies code_elevation_power for 'C…' rows and
        // phase_elevation_power for 'L…' rows.
        return computeElevation(obs_type, var, elevation_deg);
    }

    // ---------------------------------------------------------------
    // Synthetic CN0 fallback (Misra & Enge 2011 §6.2 antenna pattern)
    // ---------------------------------------------------------------
    double StochasticModelWeight::synthesizeCn0FromElevation(double elevation_deg) const {
        constexpr double cn0_zenith_max = 50.0;  // dBHz at zenith
        constexpr double cn0_drop_db    = 15.0;  // dB drop, zenith → horizon
        const double sin_el = std::sin(elevation_deg * Constants::PI / 180.0);
        return cn0_zenith_max - cn0_drop_db * (1.0 - std::max(0.0, sin_el));
    }

    // -----------------------------------------------------------------
    // SatelliteBlock model
    // -----------------------------------------------------------------

    double StochasticModelWeight::computeSatBlock(
        const std::string& obs_type,
        double elevation_deg,
        double default_var,
        const std::string& satellite,
        const std::vector<AntennaData>& antennas) const {

        // --- Find matching ATX type string for this satellite ---
        std::string atx_type;
        for (const auto& a : antennas) {
            if (a.type.find(satellite + "_") == 0 ||
                a.type.find("_" + satellite + "_") != std::string::npos) {
                atx_type = a.type;
                break;
            }
        }

        // Normalise ATX type to uppercase for substring matching.
        std::string atx_upper = atx_type;
        for (auto& ch : atx_upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        // --- Look up zenith noise by block type ---
        const bool is_code = (obs_type[0] == 'C');
        const auto& block_table = is_code
            ? sat_block_params_.block_code_std_m
            : sat_block_params_.block_phase_std_m;

        double zenith_std = is_code
            ? sat_block_params_.default_code_std_m
            : sat_block_params_.default_phase_std_m;

        std::string matched_block;
        for (const auto& [pattern, std_m] : block_table) {
            // Uppercase the pattern for case-insensitive comparison.
            std::string pat_upper = pattern;
            for (auto& ch : pat_upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (!atx_upper.empty() && atx_upper.find(pat_upper) != std::string::npos) {
                zenith_std = std_m;
                matched_block = pattern;
                break;
            }
        }

        // --- Frequency band scale factor ---
        double freq_scale = 1.0;
        if (obs_type.size() >= 2) {
            const char band = obs_type[1];
            auto fit = sat_block_params_.freq_band_scale.find(band);
            if (fit != sat_block_params_.freq_band_scale.end()) {
                freq_scale = fit->second;
            }
        }

        const double scaled_std = zenith_std * freq_scale;
        const double var = scaled_std * scaled_std;

        log_manager_.getLogger("stochastic_model")->trace(
            "SatBlock: sat={}, atx='{}', block='{}', obs={}, zenith_std={:.4f} m, "
            "freq_scale={:.2f}, scaled_std={:.4f} m",
            satellite, atx_type, matched_block, obs_type, zenith_std, freq_scale, scaled_std);

        // Apply elevation amplification.  Only code reaches
        // computeSatBlock under the simplified dispatch — phase
        // takes its own phase_std_m × elevation path in
        // computeVariance — so the per-observable power applied
        // here is always code_elevation_power.
        return computeElevation(obs_type, var, elevation_deg);
    }

} // namespace GnssProcessor
