#include "MwSlipDetector.hpp"
#include <cmath>
#include <limits>

namespace GnssProcessor {

    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager) {
        auto logger = log_manager_.getLogger("mw_slip_detector");
        logger->info("MwSlipDetector initialized (large MW only): thresh={:.2f} WL cycles", large_mw_threshold_cycles_);
    }

    bool MwSlipDetector::findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const {
        phase_if_key.clear();
        code_if_key.clear();

        // Find first iono-free phase+code combos produced by PppModel ("L..._L..." and "C..._C...")
        for (const auto& [k, v] : obs.observed) {
            if (k.find('_') == std::string::npos) continue;
            if (k.empty()) continue;

            if (k[0] == 'L' && phase_if_key.empty()) phase_if_key = k;
            if (k[0] == 'C' && code_if_key.empty()) code_if_key = k;

            if (!phase_if_key.empty() && !code_if_key.empty()) return true;
        }
        return (!phase_if_key.empty() && !code_if_key.empty());
    }

    bool MwSlipDetector::splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    double MwSlipDetector::computeWideLaneWavelengthMeters(const PppObservation& obs,
        const std::string& sig1,
        const std::string& sig2) const {

        // Use per-system frequency mapping (and per-sat if GLONASS ever becomes channel-aware in PppUtils)
        const double f1 = ppp_utils_.getFrequency(sig1, obs.system, obs.satellite);
        const double f2 = ppp_utils_.getFrequency(sig2, obs.system, obs.satellite);

        if (!(std::isfinite(f1) && std::isfinite(f2) && f1 > 0.0 && f2 > 0.0)) {
            log_manager_.getLogger("mw_slip_detector")->debug(
                "Invalid frequencies for {} {}: sig1={} f1={:.3f} MHz, sig2={} f2={:.3f} MHz",
                obs.satellite, PppUtils::getSystemString(obs.system),
                sig1, f1 / 1e6, sig2, f2 / 1e6);
            return 0.0;
        }

        const double df = std::abs(f1 - f2);
        if (!(std::isfinite(df) && df > 0.0)) return 0.0;

        const double lambda_wl = Constants::SPEED_OF_LIGHT / df;
        return (std::isfinite(lambda_wl) && lambda_wl > 0.0) ? lambda_wl : 0.0;
    }

    double MwSlipDetector::computeMwLikeMeters(const PppObservation& obs,
        const std::string& phase_if_key,
        const std::string& code_if_key) const {

        auto itL = obs.observed.find(phase_if_key);
        auto itC = obs.observed.find(code_if_key);
        if (itL == obs.observed.end() || itC == obs.observed.end()) return std::numeric_limits<double>::quiet_NaN();

        const double lif_m = itL->second; // meters (iono-free phase combo in meters)
        const double cif_m = itC->second; // meters (iono-free code combo in meters)
        if (!std::isfinite(lif_m) || !std::isfinite(cif_m)) return std::numeric_limits<double>::quiet_NaN();

        // MW-like: IF phase minus IF code
        return lif_m - cif_m;
    }

    bool MwSlipDetector::hasAnyPhaseLockLoss(const PppObservation& obs) const {
        for (const auto& [type, lli] : obs.lli_flags) {
            if (!type.empty() && type[0] == 'L' && lli != 0) return true;
        }
        return false;
    }

    std::vector<std::string> MwSlipDetector::detectLargeMwResets(const std::vector<PppObservation>& observations) {
        std::vector<std::string> reset_keys;
        reset_keys.reserve(observations.size());

        auto logger = log_manager_.getLogger("mw_slip_detector");

        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;

            std::string phase_if_key, code_if_key;
            if (!findIonoFreeLabels(obs, phase_if_key, code_if_key)) continue;

            // Split the IF phase label into its component signals to compute WL wavelength
            std::string s1, s2;
            if (!splitIfLabel(phase_if_key, s1, s2)) continue;

            // Immediate reset on lock loss
            if (hasAnyPhaseLockLoss(obs)) {
                const std::string amb_key = obs.satellite + "_" + phase_if_key;
                reset_keys.push_back(amb_key);
                logger->warn("LLI lock loss detected: scheduling ambiguity reset for {}", amb_key);
                last_mw_m_.erase(obs.satellite);
                continue;
            }

            const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
            if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) continue;

            const double mw_m = computeMwLikeMeters(obs, phase_if_key, code_if_key);
            if (!std::isfinite(mw_m)) continue;

            const double thresh_m = large_mw_threshold_cycles_ * lambda_wl;

            auto it = last_mw_m_.find(obs.satellite);
            if (it == last_mw_m_.end()) {
                last_mw_m_[obs.satellite] = mw_m;
                continue;
            }

            const double delta_mw = mw_m - it->second;
            it->second = mw_m;

            if (std::abs(delta_mw) > thresh_m) {
                const std::string amb_key = obs.satellite + "_" + phase_if_key;
                reset_keys.push_back(amb_key);

                logger->warn(
                    "Large MW slip detected for {} ({}): ΔMW={:.3f} m > {:.3f} m (λWL={:.3f} m, ~{:.1f} WL cycles); reset {}",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    delta_mw, thresh_m, lambda_wl, std::abs(delta_mw) / lambda_wl, amb_key);

                last_mw_m_.erase(obs.satellite);
            }
        }

        return reset_keys;
    }

} // namespace GnssProcessor