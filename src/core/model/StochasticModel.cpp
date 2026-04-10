/**
 * @file StochasticModel.cpp
 * @brief Implementation of the StochasticModelWeight class.
 */

#include "StochasticModel.hpp"
#include "Constants.hpp"
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Construction
    // ===================================================================

    StochasticModelWeight::StochasticModelWeight(const GnssConfig& config, LogManager& log_mgr)
        : model_type_(config.getStochasticModel()),
          snr_params_(config.getSnrModelParams()),
          log_manager_(log_mgr) {

        const char* name = "unknown";
        switch (model_type_) {
        case StochasticModelType::Constant:  name = "Constant";  break;
        case StochasticModelType::Elevation: name = "Elevation"; break;
        case StochasticModelType::SNR:       name = "SNR";       break;
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
            // For Doppler/SNR observables the SIGMA-? model is not
            // applicable — fall back to elevation-based weighting.
            return computeElevation(default_var, elevation_deg);
        }

        return default_var; // unreachable — keeps compilers happy
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

} // namespace GnssProcessor
