#include "CycleSlipDetector.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    CycleSlipDetector::CycleSlipDetector(double gf_threshold, double doppler_threshold, LogManager& log_manager)
        : gf_threshold_(gf_threshold), doppler_threshold_(doppler_threshold), log_manager_(log_manager) {
        log_manager_.getLogger()->info("Initialized CycleSlipDetector with GF threshold: {}m, Doppler threshold: {} cycles",
            gf_threshold_, doppler_threshold_);
    }

    std::vector<CycleSlipResult> CycleSlipDetector::detectAndRepair(std::vector<RinexObservation>& observations) {
        std::vector<CycleSlipResult> results;
        // Map to store previous observations per satellite
        std::map<std::string, RinexObservation> prev_obs; // Moved outside loop

        if (observations.size() < 2) {
            log_manager_.getLogger()->debug("Insufficient observations for cycle slip detection");
            return results;
        }

        // Frequency pairs for geometry-free detection (configurable per system)
        const std::map<System, std::pair<std::string, std::string>> freq_pairs = {
            {System::GPS, {"L1C", "L2W"}},
            {System::Galileo, {"L1B", "L5Q"}},
            {System::GLONASS, {"L1C", "L2C"}},
            {System::BeiDou, {"L2I", "L7I"}},
            {System::QZSS, {"L1C", "L2S"}},
            {System::SBAS, {"L1C", "L5I"}}
        };

        for (auto& obs : observations) {
            const auto& sat = obs.satellite;
            auto freq_it = freq_pairs.find(obs.system);
            if (freq_it == freq_pairs.end()) {
                log_manager_.getLogger()->debug("No frequency pair defined for system: {}", static_cast<int>(obs.system));
                continue;
            }
            const auto& [l1_type, l2_type] = freq_it->second;

            if (prev_obs.find(sat) != prev_obs.end()) {
                const auto& prev = prev_obs[sat];
                double dt = obs.time.tow - prev.time.tow;
                if (dt < 0.0) {
                    dt += 604800.0; // Handle week rollover
                }
                if (dt < 0.01 || dt > 300.0) {
                    log_manager_.getLogger()->debug("Invalid time difference {}s for satellite {}", dt, sat);
                    prev_obs[sat] = obs;
                    continue;
                }

                // Detect cycle slip using geometry-free combination
                if (hasValidPhaseTypes(obs, l1_type, l2_type) && hasValidPhaseTypes(prev, l1_type, l2_type)) {
                    double gf_curr = computeGeometryFree(obs, l1_type, l2_type);
                    double gf_prev = computeGeometryFree(prev, l1_type, l2_type);
                    double gf_diff = std::abs(gf_curr - gf_prev);

                    if (gf_diff > gf_threshold_) {
                        // Cycle slip detected
                        CycleSlipResult result;
                        result.satellite = sat;
                        result.time = obs.time;
                        result.obs_type = l1_type;
                        result.repaired = false;

                        // Attempt repair using Doppler integration
                        double jump = estimateCycleJump(obs, prev, l1_type, dt);
                        if (std::abs(jump) < doppler_threshold_) {
                            // Repair the slip
                            obs.measurements[l1_type] -= jump;
                            result.cycle_jump = jump;
                            result.repaired = true;
                            log_manager_.getLogger()->info("Repaired cycle slip for {} on {}: jump {:.0f} cycles",
                                sat, l1_type, jump);
                        }
                        else {
                            log_manager_.getLogger()->warn("Cycle slip detected for {} on {}, but repair failed: jump {:.0f} cycles",
                                sat, l1_type, jump);
                        }
                        results.push_back(result);
                    }
                }
            }
            prev_obs[sat] = obs;
        }

        log_manager_.getLogger()->debug("Detected and repaired {} cycle slips", results.size());
        return results;
    }

    double CycleSlipDetector::computeGeometryFree(const RinexObservation& obs, const std::string& l1_type,
        const std::string& l2_type) const {
        double l1 = obs.measurements.at(l1_type) * Constants::GPS_L1_WAVELENGTH;
        double l2 = obs.measurements.at(l2_type) * Constants::GPS_L2_WAVELENGTH;
        return l1 - l2;
    }

    double CycleSlipDetector::estimateCycleJump(const RinexObservation& curr, const RinexObservation& prev,
        const std::string& obs_type, double dt) const {
        // Estimate cycle jump using Doppler (D1C, etc.) and phase difference
        std::string doppler_type = "D" + obs_type.substr(1); // e.g., L1C -> D1C
        if (curr.measurements.count(doppler_type) && prev.measurements.count(doppler_type)) {
            double doppler_avg = (curr.measurements.at(doppler_type) + prev.measurements.at(doppler_type)) / 2.0;
            double phase_diff = curr.measurements.at(obs_type) - prev.measurements.at(obs_type);
            double expected_phase = doppler_avg * dt; // Doppler in cycles/s * time in s
            double jump = phase_diff - expected_phase;
            return std::round(jump); // Nearest integer cycle jump
        }
        return 0.0; // No Doppler data, no repair
    }

    bool CycleSlipDetector::hasValidPhaseTypes(const RinexObservation& obs, const std::string& l1_type,
        const std::string& l2_type) const {
        return obs.measurements.count(l1_type) && obs.measurements.count(l2_type);
    }

} // namespace GnssProcessor