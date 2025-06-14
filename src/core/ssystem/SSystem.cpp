#include "SSystem.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

SSystem::SSystem(bool is_network_mode)
    : is_network_mode_(is_network_mode) {
    spdlog::info("Initialized SSystem in {} mode", is_network_mode ? "network" : "user");
}

void SSystem::initialize(const std::vector<std::string>& obs_types,
                         const std::vector<std::pair<std::string, std::string>>& freq_pairs) {
    obs_types_ = obs_types;
    freq_pairs_ = freq_pairs;
    spdlog::info("SSystem initialized with {} observation types and {} frequency pairs",
                 obs_types_.size(), freq_pairs_.size());
}

bool SSystem::processObservations(const std::vector<RinexObservation>& observations,
                                 const std::vector<SinexEstimate>& sinex_estimates,
                                 std::vector<SSystemObservation>& transformed_obs) {
    if (observations.empty()) {
        spdlog::error("No observations provided for S-System processing");
        return false;
    }

    // Update parameters from SINEX estimates
    updateFromSinex(sinex_estimates);

    // Convert observations to S-System observations
    transformed_obs.clear();
    transformed_obs.reserve(observations.size());
    for (const auto& obs : observations) {
        SSystemObservation s_obs;
        s_obs.satellite = obs.satellite;
        s_obs.system = obs.system;
        s_obs.time = obs.time;
        s_obs.measurements = obs.measurements;
        // Initialize variances (placeholder: 0.01m^2 for code, 0.0001m^2 for phase)
        for (const auto& [type, value] : obs.measurements) {
            s_obs.variances[type] = (type[0] == 'C') ? 0.01 : 0.0001;
        }
        transformed_obs.push_back(s_obs);
    }

    // Apply S-transformations
    applyCcRTransformation(transformed_obs);
    applyCcSTransformation(transformed_obs);

    spdlog::info("Processed {} observations into S-System", transformed_obs.size());
    return true;
}

const SSystemParameters& SSystem::getParameters() const {
    return params_;
}

void SSystem::applyCcRTransformation(std::vector<SSystemObservation>& obs) {
    if (obs.empty()) return;

    // CC-R: Eliminate receiver clock bias by differencing w.r.t. reference receiver
    // For user mode, assume single receiver; for network, select first station
    std::map<std::string, double> ref_clocks; // Placeholder for receiver clocks
    for (auto& o : obs) {
        for (const auto& [type, value] : o.measurements) {
            if (type[0] == 'C' || type[0] == 'L') { // Code or phase
                // Adjust measurements (simplified: subtract estimated clock bias)
                //o.measurements[type] -= ref_clocks.value_or(o.satellite, 0.0); // TODO: Check
            }
        }
    }
    spdlog::debug("Applied CC-R transformation to {} observations", obs.size());
}

void SSystem::applyCcSTransformation(std::vector<SSystemObservation>& obs) {
    if (obs.empty()) return;

    // CC-S: Eliminate satellite clock bias by differencing w.r.t. reference satellite
    std::map<std::string, double> ref_sat_clocks; // Placeholder for satellite clocks
    for (auto& o : obs) {
        for (const auto& [type, value] : o.measurements) {
            if (type[0] == 'C' || type[0] == 'L') { // Code or phase
                // Adjust measurements (simplified: subtract estimated satellite clock)
                o.measurements[type] -= ref_sat_clocks[o.satellite];
            }
        }
    }
    spdlog::debug("Applied CC-S transformation to {} observations", obs.size());
}

Eigen::MatrixXd SSystem::computeSTransformationMatrix(size_t num_obs, size_t num_params) const {
    // Simplified S-transformation matrix (identity for now)
    Eigen::MatrixXd s_matrix = Eigen::MatrixXd::Identity(num_obs, num_params);
    spdlog::debug("Computed S-transformation matrix: {}x{}", num_obs, num_params);
    return s_matrix;
}

void SSystem::updateFromSinex(const std::vector<SinexEstimate>& estimates) {
    for (const auto& est : estimates) {
        if (est.parameter_type.find("STA") == 0) { // Station parameters (e.g., STAX, STAY, STAZ)
            params_.station_params[est.site_code].conservativeResize(3);
            int idx = (est.parameter_type == "STAX") ? 0 : (est.parameter_type == "STAY") ? 1 : 2;
            params_.station_params[est.site_code](idx) = est.value;
        } else if (est.parameter_type.find("ORB") == 0 || est.parameter_type == "CLCK") {
            // Satellite parameters (e.g., ORBX, CLCK)
            params_.satellite_params[est.site_code].conservativeResize(4); // Orbit (3) + clock (1)
            int idx = (est.parameter_type == "ORBX") ? 0 : (est.parameter_type == "ORBY") ? 1 :
                      (est.parameter_type == "ORBZ") ? 2 : 3;
            params_.satellite_params[est.site_code](idx) = est.value;
        }
    }
    // Initialize covariance (placeholder)
    params_.covariance = Eigen::MatrixXd::Identity(params_.station_params.size() + params_.satellite_params.size(),
                                                  params_.station_params.size() + params_.satellite_params.size());
    spdlog::debug("Updated S-System parameters from {} SINEX estimates", estimates.size());
}

} // namespace GnssProcessor
//#endif