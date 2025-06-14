// moved to PppModel
/*
#include <Eigen/Dense>
#include <spdlog/spdlog.h>
#include "Constants.hpp"

// Compute Solid Earth Tide displacement based on IERS conventions (simplified placeholder)
Eigen::Vector3d computeSolidEarthTideDisplacement(
    const Eigen::Vector3d& receiverPos,
    const Eigen::Vector3d& sunPos,
    const Eigen::Vector3d& moonPos,
    double time) {
    Eigen::Vector3d displacement(0.0, 0.0, 0.0);
    // Placeholder: In practice, use IERS formulae with Love numbers and celestial positions
    // Example computation would involve gravitational potential and tidal coefficients
    spdlog::debug("Computed solid earth tide displacement: {}", displacement.transpose());
    return displacement;
}

// Compute Sagnac Correction due to Earth's rotation
double computeSagnacCorrection(
    const Eigen::Vector3d& receiverPos,
    const Eigen::Vector3d& satellitePos) {
    double omega_e = Constants::EARTH_ROTATION_RATE;  // rad/s
    double c = Constants::SPEED_OF_LIGHT;            // m/s
    double delta_t = (omega_e / c) * (receiverPos(0) * satellitePos(1) - receiverPos(1) * satellitePos(0));
    double correction = delta_t * c;  // Convert to meters
    spdlog::debug("Computed Sagnac correction: {}", correction);
    return correction;
}

// Compute Pole Tide displacement based on IERS conventions (simplified placeholder)
Eigen::Vector3d computePoleTideDisplacement(
    const Eigen::Vector3d& receiverPos,
    double meanPoleX,
    double meanPoleY,
    double time) {
    Eigen::Vector3d displacement(0.0, 0.0, 0.0);
    // Placeholder: In practice, use IERS formulae with mean pole position and Love numbers
    // Example computation would involve centrifugal effects of polar motion
    spdlog::debug("Computed pole tide displacement: {}", displacement.transpose());
    return displacement;
}

// Compute Antenna Phase Offset correction
double computeAntennaPhaseOffset(
    int frequency,
    const Eigen::Vector3d& pcoValues,
    const Eigen::Vector3d& satelliteDirection) {
    double pco = pcoValues(frequency);  // PCO value for the given frequency
    // Project PCO onto satellite direction (assuming Z-up convention)
    double correction = pco * satelliteDirection.dot(Eigen::Vector3d(0, 0, 1));
    spdlog::debug("Computed antenna phase offset for frequency {}: {}", frequency, correction);
    return correction;
}

// Compute Antenna Phase Center Variations correction (simplified placeholder)
double computeAntennaPCV(
    int frequency,
    double elevation,
    double azimuth) {
    double correction = 0.0;
    // Placeholder: In practice, interpolate from a PCV table or model based on elevation and azimuth
    spdlog::debug("Computed antenna PCV for frequency {}, elevation {}, azimuth {}: {}",
                  frequency, elevation, azimuth, correction);
    return correction;
}
*/