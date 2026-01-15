#include "CycleSlipDetector.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>
#include <regex>

namespace GnssProcessor {

    CycleSlipDetector::CycleSlipDetector(const GnssConfig& config, LogManager& log_manager, PppModel& ppp_model)
        : config_(config), log_manager_(log_manager), ppp_model_(ppp_model),
        mw_threshold_cycles_(0.25), gf_threshold_meters_(2.0), c_(Constants::SPEED_OF_LIGHT) {
        auto logger = log_manager_.getLogger("cycle_slip_detector");
        logger->info("CycleSlipDetector initialized: MW thresh={:.2f} cycles, GF thresh={:.2f} m",
            mw_threshold_cycles_, gf_threshold_meters_);
    }

    std::string CycleSlipDetector::getSystemFromSatellite(const std::string& satellite) const {
        if (satellite.empty() || satellite.size() < 3) return "Unknown";
        char prn_type = satellite[0];
        if (prn_type == 'G') return "GPS";
        if (prn_type == 'E') return "Galileo";
        if (prn_type == 'R') return "GLONASS";
        if (prn_type == 'C') return "BeiDou";
        if (prn_type == 'J') return "QZSS";
        return "Unknown";
    }

    std::string CycleSlipDetector::getSatFromKey(const std::string& key) const {
        size_t pos = key.rfind('_');
        if (pos != std::string::npos && pos >= 3) {
            return key.substr(0, pos);
        }
        return "";
    }

    double CycleSlipDetector::getFrequency(const std::string& system, const std::string& satellite, const std::string& signal) const {
        return -999.0;
        //return ppp_model_.getFrequency(signal, system, satellite);
    }

    bool CycleSlipDetector::findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const {
        phase_if_key.clear();
        code_if_key.clear();
        std::regex if_pattern(R"(^[LC]\d+W_[LC]\d+W$)");  // Matches L1W_L2W, L1W_L5W, etc.
        for (const auto& [key, val] : obs.observed) {
            if (std::regex_match(key, if_pattern)) {
                if (key[0] == 'L') {
                    phase_if_key = key;
                }
                else if (key[0] == 'C') {
                    code_if_key = key;
                }
                if (!phase_if_key.empty() && !code_if_key.empty()) {
                    return true;
                }
            }
        }
        return !phase_if_key.empty() && !code_if_key.empty();
    }

    bool CycleSlipDetector::extractSignalsFromLabel(const std::string& label, std::string& sig1, std::string& sig2) const {
        size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos == label.size() - 1) return false;
        std::string part1 = label.substr(0, pos);
        std::string part2 = label.substr(pos + 1);
        if (part1.size() < 3 || part2.size() < 3 || part1.back() != 'W' || part2.back() != 'W') return false;
        sig1 = part1.substr(0, part1.size() - 1);  // Remove 'W', e.g., L1W -> L1
        sig2 = part2.substr(0, part2.size() - 1);
        return true;
    }

    bool CycleSlipDetector::hasRequiredRawData(const PppObservation& obs, const std::string& sig_p1, const std::string& sig_p2,
        const std::string& sig_c1, const std::string& sig_c2) const {
        return obs.observed.count(sig_p1) && obs.observed.count(sig_p2) &&
            obs.observed.count(sig_c1) && obs.observed.count(sig_c2);
    }

    double CycleSlipDetector::computeMW(const PppObservation& obs, double f1, double f2,
        const std::string& sig_p1, const std::string& sig_p2,
        const std::string& sig_c1, const std::string& sig_c2) {
        auto it_p1 = obs.observed.find(sig_p1);
        auto it_p2 = obs.observed.find(sig_p2);
        auto it_c1 = obs.observed.find(sig_c1);
        auto it_c2 = obs.observed.find(sig_c2);
        if (it_p1 == obs.observed.end() || it_p2 == obs.observed.end() ||
            it_c1 == obs.observed.end() || it_c2 == obs.observed.end()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        double Phi1 = it_p1->second;
        double Phi2 = it_p2->second;
        double P1 = it_c1->second;
        double P2 = it_c2->second;
        if (!std::isfinite(Phi1) || !std::isfinite(Phi2) || !std::isfinite(P1) || !std::isfinite(P2)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        // Standard Melbourne-Wübbena: wide-lane phase minus geometry-free code
        double phi_w = (f1 * Phi1 - f2 * Phi2) / (f1 - f2);
        double p_gf = (f1 * P1 + f2 * P2) / (f1 + f2);
        return phi_w - p_gf;  // MW residual in meters
    }

    double CycleSlipDetector::computeGF(const PppObservation& obs, const std::string& sig_p1, const std::string& sig_p2) {
        auto it_p1 = obs.observed.find(sig_p1);
        auto it_p2 = obs.observed.find(sig_p2);
        if (it_p1 == obs.observed.end() || it_p2 == obs.observed.end()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        double Phi1 = it_p1->second;
        double Phi2 = it_p2->second;
        if (!std::isfinite(Phi1) || !std::isfinite(Phi2)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return Phi1 - Phi2;  // Geometry-free: L1 - L2 in meters
    }

    bool CycleSlipDetector::detectSlipMW(const std::string& key, double current_mw, double lambda) {
        auto& hist = mw_history_[key];
        if (hist.size() < 1) {
            hist.push_back(current_mw);
            return false;
        }
        double delta_mw = current_mw - hist.back();  // First-order difference
        double thresh_m = mw_threshold_cycles_ * lambda;
        if (std::abs(delta_mw) > thresh_m) {
            log_manager_.getLogger("cycle_slip_detector")->warn(
                "MW slip flagged for {}: ΔMW={:.3f} m > {:.3f} m ({} cycles)",
                key, delta_mw, thresh_m, mw_threshold_cycles_);
            hist.clear();
            return true;
        }
        hist.push_back(current_mw);
        if (hist.size() > 5) hist.erase(hist.begin());
        return false;
    }

    bool CycleSlipDetector::detectSlipGF(const std::string& key, double current_gf) {
        auto& hist = gf_history_[key];
        if (hist.size() < 2) {
            hist.push_back(current_gf);
            return false;
        }
        // Second-order difference
        size_t n = hist.size();
        double delta2 = current_gf - 2.0 * hist[n - 1] + hist[n - 2];
        if (std::abs(delta2) > gf_threshold_meters_) {
            log_manager_.getLogger("cycle_slip_detector")->warn(
                "GF slip flagged for {}: Δ²GF={:.3f} m > {:.3f} m",
                key, delta2, gf_threshold_meters_);
            hist.clear();
            return true;
        }
        hist.push_back(current_gf);
        if (hist.size() > 5) hist.erase(hist.begin());
        return false;
    }

    void CycleSlipDetector::resetAmbiguities(const std::string& satellite, const std::string& sig_p1, const std::string& sig_p2,
        StateManager& state_manager) {
        // Placeholder for cycle slip repair using MWWL and GF observables
        // TODO: Implement repair logic, e.g., estimate slip from ΔMW / λ_WL and apply to ambiguities
        // For now, reset to float (zero value, high variance)
        std::string key1 = satellite + "_" + sig_p1;
        std::string key2 = satellite + "_" + sig_p2;
        // Assuming StateManager::resetAmbiguity(key) exists: sets value=0, var=300.0, off-diagonals=0
        //state_manager.resetAmbiguity(key1);
        //state_manager.resetAmbiguity(key2);
        auto logger = log_manager_.getLogger("cycle_slip_detector");
        logger->info("Placeholder repair: Reset ambiguities for {} using MWWL/GF: {} and {}", satellite, key1, key2);
    }

    void CycleSlipDetector::detectAndReset(std::vector<PppObservation>& observations, StateManager& state_manager) {
        auto logger = log_manager_.getLogger("cycle_slip_detector");
        logger->debug("Running cycle slip detection on {} observations", observations.size());

        for (auto& obs : observations) {
            std::string sat = obs.satellite;
            std::string sys = getSystemFromSatellite(sat);
            if (sys == "Unknown" || sys == "SBAS" || sys == "GLONASS") continue;  // Skip non-dual or problematic

            std::string phase_if_key, code_if_key;
            if (!findIonoFreeLabels(obs, phase_if_key, code_if_key)) {
                logger->trace("No iono-free labels found for {}", sat);
                continue;
            }

            std::string sig_p1, sig_p2, sig_c1, sig_c2;
            if (!extractSignalsFromLabel(phase_if_key, sig_p1, sig_p2) ||
                !extractSignalsFromLabel(code_if_key, sig_c1, sig_c2)) {
                logger->warn("Failed to extract signals from labels for {}", sat);
                continue;
            }

            // Fallback for code signals if extraction failed
            if (sig_c1.empty()) sig_c1 = "C" + sig_p1.substr(1);
            if (sig_c2.empty()) sig_c2 = "C" + sig_p2.substr(1);

            if (!hasRequiredRawData(obs, sig_p1, sig_p2, sig_c1, sig_c2)) {
                logger->trace("Missing raw data for signals {} {} {} {} on {}", sig_p1, sig_p2, sig_c1, sig_c2, sat);
                continue;
            }

            // Get frequencies using PppModel (variable per signal/sat)
            double f1 = getFrequency(sys, sat, sig_p1);
            double f2 = getFrequency(sys, sat, sig_p2);
            if (f1 <= 0.0 || f2 <= 0.0) continue;

            // Step 4: Form combinations
            double mw = computeMW(obs, f1, f2, sig_p1, sig_p2, sig_c1, sig_c2);
            double gf = computeGF(obs, sig_p1, sig_p2);

            if (!std::isfinite(mw) || !std::isfinite(gf)) {
                logger->trace("Non-finite MW/GF for {}", sat);
                continue;
            }

            // Compute wide-lane wavelength dynamically
            double lambda = c_ / std::abs(f1 - f2);

            // Append to histories
            std::string mw_key = sat + "_MW";
            std::string gf_key = sat + "_GF";
            mw_history_[mw_key].push_back(mw);
            gf_history_[gf_key].push_back(gf);
            if (mw_history_[mw_key].size() > 5) mw_history_[mw_key].erase(mw_history_[mw_key].begin());
            if (gf_history_[gf_key].size() > 5) gf_history_[gf_key].erase(gf_history_[gf_key].begin());

            // Step 5: Evaluate
            bool mw_slip = detectSlipMW(mw_key, mw, lambda);
            bool gf_slip = detectSlipGF(gf_key, gf);
            bool slip_detected = mw_slip || gf_slip;

            if (slip_detected) {
                // Step 6: Repair placeholder
                resetAmbiguities(sat, sig_p1, sig_p2, state_manager);
                // Optionally flag obs for exclusion, e.g., obs.flagged = true;
            }
        }
    }

} // namespace GnssProcessor