// PppTypes.hpp
#pragma once

#include <map>
#include <string>
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "RinexParser.hpp"  // for System

namespace GnssProcessor {

    struct PppObservation {
        std::string satellite;
        System system;
        GnssTime time;
        std::map<std::string, double> observed;
        std::map<std::string, double> modeled;
        std::map<std::string, double> residuals;
        std::map<std::string, double> variances;
        std::map<std::string, int> lli_flags;
    };

    struct PppState {
        Eigen::Vector3d position;
        double clock_bias;
        double troposphere_zenith_delay;
        double ionosphere_zenith_delay;
        std::map<std::string, double> ionosphere_delays;
        std::map<std::string, double> ambiguities;
        Eigen::MatrixXd covariance;
        GnssTime epoch_time;
    };

    struct EpochMeasurement {
        Eigen::VectorXd innovations;                      // Measurement innovations (z - h(x)), in meters
        Eigen::MatrixXd H;                                // Measurement Jacobian (partial derivatives)
        Eigen::VectorXd R_v;                              // Measurement covariance diagonal (m^2)
        GnssTime time;                                    // Epoch time
        std::vector<PppObservation> observations;         // Observations for cycle-slip detection
        std::vector<std::string> satellite_ids;           // Satellite IDs (e.g., "G01") for each measurement
        std::vector<std::string> signal_types;            // Signal types (e.g., "L1C", "L1C_L2W_L5Q") for each measurement
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
