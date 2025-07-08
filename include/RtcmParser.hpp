#pragma once
#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <map>

namespace GnssProcessor {

// Structure for RTCM observation data
struct RtcmObservation {
    std::string satellite; // Satellite ID (e.g., "G01")
    System system;        // GNSS system
    GnssTime time;       // Epoch time
    std::map<std::string, double> measurements; // Observation types (e.g., "C1C", "L1C")
    std::map<std::string, double> variances;    // Measurement variances
};

// Structure for RTCM SSR correction data
struct RtcmSsrCorrection {
    std::string satellite; // Satellite ID
    System system;        // GNSS system
    GnssTime time;       // Epoch time
    std::string correction_type; // e.g., "ORBIT", "CLOCK", "CODE_BIAS"
    double value;        // Correction value (e.g., meters for orbit, seconds for clock)
    double std_dev;      // Standard deviation
};

// RtcmParser class for RTCM 3.x messages
class RtcmParser {
public:
    explicit RtcmParser(const std::filesystem::path& file_path);

    // Parse RTCM file or stream
    bool parse();

    // Parse a single RTCM message (for real-time streaming)
    bool parseMessage(const std::vector<uint8_t>& message);

    // Get parsed observations
    const std::vector<RtcmObservation>& getObservations() const;

    // Get parsed SSR corrections
    const std::vector<RtcmSsrCorrection>& getSsrCorrections() const;

private:
    std::filesystem::path file_path_;
    std::vector<RtcmObservation> observations_;
    std::vector<RtcmSsrCorrection> ssr_corrections_;

    // Parse RTCM header
    bool parseHeader(const std::vector<uint8_t>& message, uint32_t& msg_type, uint32_t& length);

    // Parse MSM4/MSM7 observation message
    bool parseMsmMessage(const std::vector<uint8_t>& message, uint32_t msg_type);

    // Parse SSR correction message
    bool parseSsrMessage(const std::vector<uint8_t>& message, uint32_t msg_type);

    GnssTime convertToGpsTime(uint32_t tow_ms, uint16_t week) const;

    // Convert RTCM time to GnssTime
    GnssTime convertRtcmTime(uint32_t tow_ms, uint16_t week) const;

    // Map RTCM signal ID to RINEX observation type
    std::string signalIdToObsType(System system, uint8_t signal_id) const;
};

} // namespace GnssProcessor
//#endif // RTCM_PARSER_HPP