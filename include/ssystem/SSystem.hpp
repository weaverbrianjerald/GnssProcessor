/*
#pragma once
#include "rinex/RinexParser.hpp"
#include "sinex/SinexParser.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

// Structure for S-System transformed observation
struct SSystemObservation {
    std::string satellite; // Satellite ID (e.g., "G01")
    System system;        // GNSS system
    GnssTime time;       // Epoch time
    std::map<std::string, double> measurements; // Observed measurements
    std::map<std::string, double> variances;    // Measurement variances
    std::map<std::string, int> lli_flags;       // Loss-of-lock indicators
};

// Structure for S-System parameters
struct SSystemParameters {
    std::map<std::string, Eigen::VectorXd> station_params; // Station-specific parameters (e.g., position, clock)
    std::map<std::string, Eigen::VectorXd> satellite_params; // Satellite-specific parameters (e.g., orbit, clock)
    Eigen::MatrixXd covariance; // Parameter covariance matrix
};

// S-System class for PPP-RTK rank deficiency resolution
class SSystem {
public:
    // Constructor with mode (network or user)
    explicit SSystem(bool is_network_mode = false);

    // Initialize S-System with observation types and frequencies
    void initialize(const std::vector<std::string>& obs_types,
                    const std::vector<std::pair<std::string, std::string>>& freq_pairs);

    // Process observations for S-transformation
    bool processObservations(const std::vector<RinexObservation>& observations,
                            const std::vector<SinexEstimate>& sinex_estimates,
                            std::vector<SSystemObservation>& transformed_obs);

    // Get transformed parameters
    const SSystemParameters& getParameters() const;

    void applyCcRTransformation(std::vector<SSystemObservation>& obs);

    void applyCcSTransformation(std::vector<SSystemObservation>& obs);

private:
    bool is_network_mode_; // True for network, false for user
    std::vector<std::string> obs_types_; // Observation types (e.g., "C1C", "L1C")
    std::vector<std::pair<std::string, std::string>> freq_pairs_; // Frequency pairs (e.g., L1C-L2W)
    SSystemParameters params_; // Current parameters

    // Apply CC-R (Common Clock Receiver) transformation
    void applyCCRTransformation(std::vector<SSystemObservation>& obs);

    // Apply CC-S (Common Clock Satellite) transformation
    void applyCCSTransformation(std::vector<SSystemObservation>& obs);

    // Compute transformation matrix for S-System
    Eigen::MatrixXd computeSTransformationMatrix(size_t num_obs, size_t num_params) const;

    // Update parameters from SINEX estimates
    void updateFromSinex(const std::vector<SinexEstimate>& estimates);
};

} // namespace GnssProcessor
//#endif // S_SYSTEM_HPP
*/