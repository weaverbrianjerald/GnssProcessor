#pragma once
#ifndef RINEXPARSER_HPP
#define RINEXPARSER_HPP

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "TimeUtils.hpp"
#include "Constants.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {
    // Forward declaration to avoid circular dependency
    class ProductParser;

    enum class System {
        GPS,
        Galileo,
        GLONASS,
        BeiDou,
        QZSS,
        SBAS,
        Unknown
    };

    struct RinexObservation {
        std::string satellite;           // Satellite ID (e.g., "G01")
        System system;                   // GNSS system
        GnssTime time;                   // Epoch time
        std::map<std::string, double> measurements; // Observed measurements (e.g., C1C, L1C)
        std::map<std::string, double> variances;    // Measurement variances (m^2)
        std::map<std::string, int> lli_flags;       // Loss-of-lock indicators
        std::string receiver_id;         // Receiver ID for network processing
    };

    struct RinexHeader {
        double version{ 0.0 };             // RINEX version (e.g., 3.04)
        std::string time_system{ "GPS" };  // Time system (e.g., GPS, GAL)
        std::map<System, std::vector<std::string>> obs_types; // Observation types per system (e.g., C1C, L1C)
        std::optional<double> interval;  // Measurement interval in seconds
        std::string receiver_type;       // Receiver type (e.g., TRIMBLE NETR9)
        std::string antenna_type;        // Antenna type (e.g., TRM59800.00)
        std::array<double, 3> approx_position{ 0.0, 0.0, 0.0 }; // Approx. receiver position (ECEF, meters)
        std::array<double, 3> antenna_delta_hen{ 0.0, 0.0, 0.0 }; // Antenna H/E/N offsets (meters)
        std::string marker_name;         // Marker name (e.g., station ID)
        std::string marker_number;       // Marker number (e.g., IGS station code)
        double antenna_azimuth_deg{ 0.0 }; // Antenna azimuth orientation (degrees)
        double antenna_elevation_deg{ 0.0 }; // Antenna elevation orientation (degrees)
    };

    class RinexParser {
    public:
        explicit RinexParser(const GnssConfig& config, LogManager& log_manager, ProductParser& product_parser);

        // Parse the RINEX file
        bool parse();

        // Get parsed header
        const RinexHeader& getHeader() const { return header_; }

        // Get parsed observations
        const std::vector<RinexObservation>& getObservations() const { return observations_; }

        // Validate measurement rate (30-sec, 1-Hz, 100-Hz max, 300-sec min)
        bool validateMeasurementRate() const;

        // Get receiver metadata
        std::string getReceiverAntennaType() const { return header_.antenna_type; }
        Eigen::Vector3d getAntennaDeltaHen() const {
            return Eigen::Vector3d(header_.antenna_delta_hen[0], header_.antenna_delta_hen[1], header_.antenna_delta_hen[2]);
        }
        double getReceiverAntennaAzimuthDeg() const { return header_.antenna_azimuth_deg; }
        double getReceiverAntennaElevationDeg() const { return header_.antenna_elevation_deg; }
        std::string getReceiverType() const { return header_.receiver_type; }
        std::string getReceiverSerialNo() const { return receiver_serial_no_; }
        std::string getMarkerName() const { return header_.marker_name; }
        std::string getMarkerNumber() const { return header_.marker_number; }
        Eigen::Vector3d getApproxPositionXyz() const {
            return Eigen::Vector3d(header_.approx_position[0], header_.approx_position[1], header_.approx_position[2]);
        }
        GnssTime getTimeOfFirstObs() const { return time_of_first_obs_; }
        const std::map<std::string, int>& getGlonassFreqSlots() const { return glonass_freq_slots_; }
        double rescaleGlonassMeasurement(double observed_value, const std::string& freq_type,
            const std::string& satellite) const;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        ProductParser& product_parser_;
        RinexHeader header_;
        std::vector<RinexObservation> observations_;
        std::string receiver_serial_no_; // Receiver serial number
        GnssTime time_of_first_obs_;    // Time of first observation
        std::map<std::string, int> glonass_freq_slots_; // Satellite (e.g., "R01") -> frequency channel (e.g., -4)

        // Validate RINEX 3.x observation code
        std::optional<std::string> validateObsCode(const std::string& code, System system) const;

        // Parse GLONASS frequency data
        double getGlonassFrequency(const std::string& freq_type, const std::string& satellite) const;
    };

} // namespace GnssProcessor

#endif // RINEXPARSER_HPP