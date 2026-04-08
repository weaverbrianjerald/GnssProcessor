#pragma once

#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include "StateManager.hpp"
#include "LogManager.hpp"
#include "PppModel.hpp"
#include "Constants.hpp"
#include <map>
#include <vector>
#include <string>
#include <cmath>
#include <sstream>

namespace GnssProcessor {

    class CycleSlipDetector {
    private:
        LogManager& log_manager_;
        const GnssConfig& config_;
        PppModel& ppp_model_;
        double mw_threshold_cycles_;  // Threshold for ΔMW in WL cycles
        double gf_threshold_meters_;  // Threshold for Δ²GF in meters
        double c_;  // Speed of light

        // History buffers: key (sat_MW or sat_GF) -> vector of values (meters, last 5 epochs max)
        std::map<std::string, std::vector<double>> mw_history_;
        std::map<std::string, std::vector<double>> gf_history_;

        std::string getSystemFromSatellite(const std::string& satellite) const;
        std::string getSatFromKey(const std::string& key) const;
        double getFrequency(const std::string& system, const std::string& satellite, const std::string& signal) const;
        bool findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const;
        bool extractSignalsFromLabel(const std::string& label, std::string& sig1, std::string& sig2) const;
        bool hasRequiredRawData(const PppObservation& obs, const std::string& sig_p1, const std::string& sig_p2,
            const std::string& sig_c1, const std::string& sig_c2) const;
        double computeMW(const PppObservation& obs, double f1, double f2,
            const std::string& sig_p1, const std::string& sig_p2,
            const std::string& sig_c1, const std::string& sig_c2);
        double computeGF(const PppObservation& obs, const std::string& sig_p1, const std::string& sig_p2);
        bool detectSlipMW(const std::string& key, double current_mw, double lambda);
        bool detectSlipGF(const std::string& key, double current_gf);
        void resetAmbiguities(const std::string& satellite, const std::string& sig_p1, const std::string& sig_p2,
            StateManager& state_manager);  // Placeholder for repair

    public:
        CycleSlipDetector(const GnssConfig& config, LogManager& log_manager, PppModel& ppp_model);
        ~CycleSlipDetector() = default;
        void detectAndReset(std::vector<PppObservation>& observations, StateManager& state_manager);
    };

} // namespace GnssProcessor