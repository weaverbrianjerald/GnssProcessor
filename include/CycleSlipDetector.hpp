#pragma once

#include "LogManager.hpp"
#include "Constants.hpp"
#include "RinexParser.hpp" // For SSystemObservation, System
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    struct CycleSlipResult {
        std::string satellite; // Satellite ID (e.g., "G01")
        GnssTime time;         // Time of cycle slip
        std::string obs_type;  // Observation type (e.g., "L1C")
        double cycle_jump{ 0.0 };// Estimated cycle jump (cycles)
        bool repaired{ false };  // Whether slip was repaired
    };

    class CycleSlipDetector {
    public:
        // Constructor
        CycleSlipDetector(double gf_threshold, double doppler_threshold, LogManager& log_manager);

        // Detect and repair cycle slips
        std::vector<CycleSlipResult> detectAndRepair(std::vector<RinexObservation>& observations);

    private:
        double gf_threshold_;      // Geometry-free threshold (meters)
        double doppler_threshold_; // Doppler-based repair threshold (cycles)
        LogManager& log_manager_;  // Logger reference

        // Compute geometry-free combination (L1 - L2)
        double computeGeometryFree(const RinexObservation& obs, const std::string& l1_type,
            const std::string& l2_type) const;

        // Estimate cycle jump using Doppler
        double estimateCycleJump(const RinexObservation& curr, const RinexObservation& prev,
            const std::string& obs_type, double dt) const;

        // Check if observation has valid phase types
        bool hasValidPhaseTypes(const RinexObservation& obs, const std::string& l1_type,
            const std::string& l2_type) const;
    };

} // namespace GnssProcessor

//#endif // CYCLESLIPDETECTOR_HPP