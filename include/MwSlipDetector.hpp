#pragma once
#ifndef MW_SLIP_DETECTOR_HPP
#define MW_SLIP_DETECTOR_HPP

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    class MwSlipDetector {
    public:
        MwSlipDetector(const GnssConfig& config, LogManager& log_manager);

        // Returns ambiguity keys to reset (e.g. "G01_L1C_L2W") for this epoch.
        std::vector<std::string> detectLargeMwResets(const std::vector<PppObservation>& observations);

        void setLargeThresholdCycles(double cycles) { large_mw_threshold_cycles_ = cycles; }

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        // MW history: sat -> last MW-like value [meters]
        std::map<std::string, double> last_mw_m_;

        // Large slip threshold in WL cycles (e.g., 10 cycles)
        double large_mw_threshold_cycles_ = 10.0;

        bool findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const;

        // Split IF label "L1C_L2W" into ("L1C","L2W")
        bool splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const;

        double computeWideLaneWavelengthMeters(const PppObservation& obs, const std::string& sig1, const std::string& sig2) const;

        double computeMwLikeMeters(const PppObservation& obs,
            const std::string& phase_if_key,
            const std::string& code_if_key) const;

        bool hasAnyPhaseLockLoss(const PppObservation& obs) const;
    };

} // namespace GnssProcessor

#endif // MW_SLIP_DETECTOR_HPP