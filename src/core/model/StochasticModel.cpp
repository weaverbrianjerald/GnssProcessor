/**
 * @file StochasticModel.cpp
 * @brief Implementation of the StochasticModelWeight class.
 */

#include "StochasticModel.hpp"
#include "Constants.hpp"
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
          log_manager_(log_mgr) {

        const char* name = "unknown";
        switch (model_type_) {
        case StochasticModelType::Constant:       name = "Constant";       break;
        case StochasticModelType::Elevation:      name = "Elevation";      break;
        case StochasticModelType::SNR:            name = "SNR";            break;
        case StochasticModelType::SatelliteBlock: name = "SatelliteBlock"; break;
        }
        log_manager_.getLogger("stochastic_model")->info(
            "StochasticModelWeight initialised: type={}", name);
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
            return computeElevation(default_var, elevation_deg);

        case StochasticModelType::SNR:
            if (is_code_or_phase) {
                return computeSnr(obs_type, measurements, elevation_deg, default_var);
            }
            return computeElevation(default_var, elevation_deg);

        case StochasticModelType::SatelliteBlock:
            // Without satellite info, fall back to elevation weighting.
            return computeElevation(default_var, elevation_deg);
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
        if (mt == 'C' || mt == 'L') {
            return computeSatBlock(obs_type, elevation_deg, default_var, satellite, antennas);
        }
        return computeElevation(default_var, elevation_deg);
    }

    // ===================================================================
    // Private strategy implementations
    // ===================================================================

    double StochasticModelWeight::computeConstant(double default_var) const {
        return default_var;
    }

    double StochasticModelWeight::computeElevation(double default_var,
                                                    double elevation_deg) const {
        const double sin_el = std::sin(elevation_deg * Constants::PI / 180.0);
        if (sin_el > 1e-6) {
            return default_var / (sin_el * sin_el);
        }
        return default_var; // near-zero elevation — don't amplify to infinity
    }

    double StochasticModelWeight::computeSnr(
        const std::string& obs_type,
        const std::map<std::string, double>& measurements,
        double elevation_deg,
        double default_var) const {

        // Build the matching SNR key: replace leading C/L with S.
        std::string snr_key = obs_type;
        snr_key[0] = 'S';

        auto it = measurements.find(snr_key);
        if (it == measurements.end() || it->second <= 0.0) {
            // No SNR available — fall back to elevation-based weighting.
            return computeElevation(default_var, elevation_deg);
        }

        double snr_dbhz = it->second;

        // Clamp to configured range.
        if (snr_dbhz < snr_params_.snr_floor_dbhz) snr_dbhz = snr_params_.snr_floor_dbhz;
        if (snr_dbhz > snr_params_.snr_ceil_dbhz)  snr_dbhz = snr_params_.snr_ceil_dbhz;

        // Determine code vs phase parameters.
        const bool is_code = (obs_type[0] == 'C');
        const double a = is_code ? snr_params_.code_a : snr_params_.phase_a;
        const double b = is_code ? snr_params_.code_b : snr_params_.phase_b;

        // SIGMA-? model:  ? = a + b · 10^(?SNR / 10)
        const double sigma = a + b * std::pow(10.0, -snr_dbhz / 10.0);

        log_manager_.getLogger("stochastic_model")->trace(
            "SNR: obs={}, snr_key={}, snr={:.1f} dBHz, sigma={:.4f} m",
            obs_type, snr_key, snr_dbhz, sigma);

        return sigma * sigma;
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

        // Apply elevation weighting on top.
        return computeElevation(var, elevation_deg);
    }

} // namespace GnssProcessor
