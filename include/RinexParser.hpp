#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "TimeUtils.hpp" // For GnssTime
#include <Eigen/Dense>

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

    struct RinexObservation {
        std::string satellite;           // Satellite ID (e.g., "G01")
        System system;                   // GNSS system
        GnssTime time;                   // Epoch time
        std::map<std::string, double> measurements; // Observed measurements
        std::map<std::string, double> variances;    // Measurement variances
        std::map<std::string, int> lli_flags;       // Loss-of-lock indicators
        std::string receiver_id; // Network processing index
    };

    struct RinexHeader {
        double version{ 0.0 };           // RINEX version (e.g., 3.04)
        std::string time_system{ "GPS" }; // Time system (e.g., GPS, GAL)
        std::map<System, std::vector<std::string>> obs_types; // Observation types per system
        std::optional<double> interval; // Measurement interval in seconds
        std::string receiver_type;      // Receiver type
        std::string antenna_type;       // Antenna type
        std::array<double, 3> approx_position{ 0.0, 0.0, 0.0 }; // Approx. receiver position (ECEF)
    };

    class RinexParser {
    public:
        explicit RinexParser(const GnssConfig& config);

        // Parse the RINEX file
        bool parse();

        // Get parsed header
        const RinexHeader& getHeader() const { return header_; }

        // Get parsed observations
        //const std::vector<SSystemObservation>& getObservations() const { return observations_; }
        //const std::vector<RinexObservation>& getObservations() const { return observations_; }
        const std::vector<RinexObservation>& getObservations() const;

        // Validate measurement rate (30-sec, 1-Hz, 100-Hz max, 300-sec min)
        bool validateMeasurementRate() const;

        // Receiver antenna type
        std::string getReceiverAntennaType() const { return header_.antenna_type; }

        Eigen::Vector3d getAntennaDeltaHen() const { return antenna_delta_hen_; }
        std::string getReceiverType() const { return header_.receiver_type; }
        std::string getReceiverSerialNo() const { return receiver_serial_no_; }

        Eigen::Vector3d getApproxPositionXyz() const { return Eigen::Vector3d(header_.approx_position[0], header_.approx_position[1], header_.approx_position[2]); }
        std::string getMarkerName() const { return marker_name_; }
        GnssTime getTimeOfFirstObs() const { return time_of_first_obs_; }

        const std::map<std::string, int>& getGlonassFreqSlots() const { return glonass_freq_slots_; }

    private:
        const GnssConfig& config_;
        LogManager log_manager_; // Changed to non-reference, initialized in constructor
        RinexHeader header_;
        std::vector<RinexObservation> observations_;
        std::string receiver_antenna_type_; // Added
        Eigen::Vector3d antenna_delta_hen_; // Added
        std::string receiver_type_; // Added
        std::string receiver_serial_no_; // Added
        std::string marker_name_;
        GnssTime time_of_first_obs_; // Time of first observation
        Eigen::Vector3d approx_position_xyz_ = Eigen::Vector3d::Zero();
        std::map<std::string, int> glonass_freq_slots_; // Satellite (e.g., "R01") -> frequency channel (k)

        // Parse header section
        bool parseHeader(std::ifstream& stream);

        // Parse observation data
        bool parseObservations(std::ifstream& stream);

        // Validate RINEX 3.x observation code
        std::optional<std::string> validateObsCode(const std::string& code, System system) const;

    };

} // namespace GnssProcessor

//#endif // RINEXPARSER_HPP