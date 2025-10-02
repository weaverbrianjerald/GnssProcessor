// PppTypes.hpp
#pragma once

#include <map>
#include <string>
#include <Eigen/Dense>
#include "TimeUtils.hpp"

namespace GnssProcessor {

    enum class System {
        GPS,
        Galileo,
        GLONASS,
        BeiDou,
        QZSS,
        SBAS,
        Unknown
    };

    struct PppObservation {
        std::string satellite;                     // Satellite ID (e.g., "G01")
        System system;                             // GNSS system (e.g., GPS)
        GnssTime time;                            // Observation time
        std::map<std::string, double> observed;    // Observed measurements (e.g., C1W, L1W, etc.)
        std::map<std::string, double> modeled;     // Modeled measurements
        std::map<std::string, double> residuals;   // Residuals (observed - modeled)
        std::map<std::string, double> variances;   // Measurement variances
        std::map<std::string, int> lli_flags;      // Loss of lock indicator flags
        Eigen::Vector3d sat_pos;                  // Satellite position in ECEF (meters)
        double elevation;                         // Elevation angle to satellite (degrees)
        double azimuth;                           // Azimuth angle to satellite (degrees)
        Eigen::Vector3d los;                      // Line-of-sight unit vector to satellite
        double range;                             // Geometric range to satellite (meters)
    };

    struct PppState {
        Eigen::Vector3d position;                  // Receiver position in ECEF (meters)
        double clock_bias;                        // Receiver clock bias (seconds)
        double troposphere_zenith_delay;          // Tropospheric zenith delay (meters)
        double ionosphere_zenith_delay;           // Ionospheric zenith delay (meters, PPP-RTK only)
        Eigen::MatrixXd covariance;               // State covariance matrix
        std::map<std::string, double> ambiguities; // Phase ambiguities (meters)
        GnssTime epoch_time;                      // Current epoch time
    };

    struct EpochMeasurement {
        Eigen::VectorXd innovations;                      // Measurement innovations (z - h(x)), in meters
        Eigen::MatrixXd H;                                // Measurement Jacobian (partial derivatives)
        Eigen::VectorXd R_v;                              // Measurement covariance diagonal (m^2)
        GnssTime time;                                    // Epoch time
        std::vector<PppObservation> observations;         // Observations for cycle-slip detection
        std::vector<std::string> satellite_ids;           // Satellite IDs (e.g., "G01") for each measurement
        std::vector<std::string> signal_types;            // Signal types (e.g., "L1C", "L1C_L2W_L5Q") for each measurement
        Eigen::VectorXd residuals;                        // Post-fit measurement residuals, in meters
        Eigen::MatrixXd residuals_covariance;             // Post-fit measurement residuals covariance, in abs meters^2
        Eigen::VectorXd residuals_standardized;           // Post-fit measurement residuals (standardized), in standard deviations
        size_t bad_residual_count;                        // Post-fit measurement residual outlier count
    };

    struct OrbitData {
        std::string satellite;         // Satellite identifier (e.g., G01)
        GnssTime time;                // Epoch time
        Eigen::Vector3d position;     // Position in ECEF (meters)
        Eigen::Vector3d velocity;     // Velocity in ECEF (m/s)
        bool position_valid{ false }; // Flag indicating valid position
        bool velocity_valid{ false }; // Flag indicating valid velocity
    };

    struct ClockData {
        std::string satellite;        // Satellite identifier (e.g., G01)
        GnssTime time;                // Epoch time
        GnssTime start_time;          // Validity start time
        GnssTime end_time;            // Validity end time
        double clock_bias{ 0.0 };     // Clock correction (seconds)
    };

    struct AntennaData {
        std::string type;             // Antenna type (e.g., satellite or receiver model)
        std::map<std::string, Eigen::Vector3d> pco; // Phase Center Offset (mm) per frequency
        std::map<std::string, std::vector<std::vector<double>>> pcv; // Phase Center Variation (mm) per frequency
        double dazi{ 0.0 };            // Azimuth increment (degrees)
        double dzen{ 0.0 };            // Zenith increment (degrees)
    };

    struct BiasData {
        std::string satellite;        // Satellite identifier (e.g., G01)
        std::string signal;           // Signal type (e.g., C1C)
        double bias{ 0.0 };           // Bias correction (seconds)
        double std_dev;               // Standard deviation (seconds)
        GnssTime start_time;          // Validity start time
        GnssTime end_time;            // Validity end time
    };

    struct ErpData {
        GnssTime time;                   // Observation time
        double xp{ 0.0 };                  // Pole coordinate x (arcsec)
        double yp{ 0.0 };                  // Pole coordinate y (arcsec)
        double ut1_utc{ 0.0 };             // UT1-UTC difference (seconds)
    };

    struct OceanTideData {
        GnssTime time;                // Epoch time
        Eigen::Vector3d displacement; // Displacement in ECEF (meters)
    };

    struct SunEphemeris {
        GnssTime time;
        Eigen::Vector3d position; // Sun position in ECEF (meters)
    };

}
