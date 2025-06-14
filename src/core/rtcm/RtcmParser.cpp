#include "RtcmParser.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <stdexcept>
#include <cstdint>
using namespace GnssProcessor;

namespace GnssProcessor {

RtcmParser::RtcmParser(const std::filesystem::path& file_path)
    : file_path_(file_path) {
    spdlog::info("Initialized RtcmParser for file: {}", file_path_.string());
}

bool RtcmParser::parse() {
    std::ifstream stream(file_path_, std::ios::binary);
    if (!stream.is_open()) {
        spdlog::error("Failed to open RTCM file: {}", file_path_.string());
        throw std::runtime_error("Failed to open RTCM file");
    }

    std::vector<uint8_t> buffer;
    while (stream.good()) {
        uint8_t byte;
        stream.read(reinterpret_cast<char*>(&byte), 1);
        if (stream.eof()) break;

        if (byte == 0xD3) { // RTCM message preamble
            buffer.clear();
            buffer.push_back(byte);
            // Read header (2 more bytes for length and message type)
            for (int i = 0; i < 2; ++i) {
                stream.read(reinterpret_cast<char*>(&byte), 1);
                if (stream.eof()) {
                    spdlog::warn("Incomplete RTCM message header");
                    return false;
                }
                buffer.push_back(byte);
            }

            // Parse header to get message length
            uint32_t msg_type, length;
            if (!parseHeader(buffer, msg_type, length)) {
                spdlog::warn("Invalid RTCM message header");
                continue;
            }

            // Read message body and CRC
            buffer.resize(3 + length + 3); // Header (3) + body + CRC (3)
            stream.read(reinterpret_cast<char*>(buffer.data() + 3), length + 3);
            if (stream.gcount() != static_cast<std::streamsize>(length + 3)) {
                spdlog::warn("Incomplete RTCM message body");
                continue;
            }

            // Parse message
            if (!parseMessage(buffer)) {
                spdlog::warn("Failed to parse RTCM message type {}", msg_type);
            }
        }
    }

    spdlog::info("Parsed {} RTCM observations, {} SSR corrections",
                 observations_.size(), ssr_corrections_.size());
    return !observations_.empty() || !ssr_corrections_.empty();
}

bool RtcmParser::parseMessage(const std::vector<uint8_t>& message) {
    uint32_t msg_type, length;
    if (!parseHeader(message, msg_type, length)) {
        return false;
    }

    // Check message type
    if (msg_type >= 1071 && msg_type <= 1077) { // MSM4 (1074 for GPS)
        //return parseMsm4Message(message, msg_type);
        return NULL; // TODO: Fix this using line above
    } else if (msg_type >= 1264 && msg_type <= 1270) { // SSR (1264 for orbit/clock)
        return parseSsrMessage(message, msg_type);
    } else {
        spdlog::debug("Unsupported RTCM message type: {}", msg_type);
        return false;
    }
}

const std::vector<RtcmObservation>& RtcmParser::getObservations() const {
    return observations_;
}

const std::vector<RtcmSsrCorrection>& RtcmParser::getSsrCorrections() const {
    return ssr_corrections_;
}

bool RtcmParser::parseHeader(const std::vector<uint8_t>& msg, uint32_t& msg_type, uint32_t& length) {
    if (msg.size() < 3 || msg[0] != 0xD3) {
        return false;
    }

    // Extract message type (12 bits) and length (10 bits)
    msg_type = ((msg[1] & 0x0F) << 8) | msg[2];
    length = ((msg[1] & 0xF0) >> 4) | ((msg[2] & 0xC0) << 2);
    spdlog::trace("Parsed RTCM header: type {}, length {}", msg_type, length);
    return true;
}

//bool RtcmParser::parseMsm4Message(const std::vector<uint8_t>& message, uint32_t msg_type) {
//    // Simplified MSM4 parsing (placeholder for full implementation)
//    if (message.size() < 12) { // Minimum header size
//        spdlog::warn("Invalid MSM4 message size: {}", message.size());
//        return false;
//    }
//
//    // Determine system based on message type
//    System system;
//    switch (msg_type) {
//        case 1074: system = System::GPS; break;
//        case 1084: system = System::Galileo; break;
//        case 1094: system = System::GLONASS; break;
//        case 1124: system = System::BeiDou; break;
//        case 1134: system = System::QZSS; break;
//        default: system = System::Unknown;
//    }
//
//    // Extract TOW (6 bytes, 1 ms resolution)
//    uint32_t tow_ms = (message[3] << 24) | (message[4] << 16) | (message[5] << 8) | message[6];
//    uint16_t week = 0; // Placeholder; needs full parsing of header for week
//    GnssTime time = convertToGpsTime(tow_ms, week);
//
//    // Parse number of satellites
//    uint8_t num_sats = message[10];
//    spdlog::trace("MSM4 msg {}: {} satellites, TOW {}ms", msg_type, num_sats, tow_ms);
//
//    // Parse observations (simplified)
//    size_t offset = 12; // After header
//    for (uint8_t i = 0; i < num_sats && offset < message.size(); ++i) {
//        RtcmObservation obs;
//        obs.system = system;
//        obs.time = time;
//        obs.satellite = std::string(1, system == System::GPS ? 'G' : system == System::Galileo ? 'E' : 'R') +
//                        std::to_string(i + 1);
//
//        // Placeholder: Assume C1C observation
//        uint8_t signal_id = 2; // Example: GPS C1C
//        std::string obs_type = signalIdToObsType(system, signal_id);
//        if (!obs_type.empty()) {
//            // Read measurement (simplified: assume 8-byte value)
//            if (offset + 8 <= message.size()) {
//                int64_t raw_value = 0;
//                for (int j = 0; j < 8; ++j) {
//                    raw_value = (raw_value << 8) | message[offset + j];
//                }
//                obs.measurements[obs_type] = raw_value * 0.0001; // Scale to meters (example)
//                obs.variances[obs_type] = 0.01; // Placeholder variance (m^2)
//                offset += 8;
//                observations_.push_back(obs);
//                spdlog::trace("Parsed observation: {} {} = {}", obs.satellite, obs_type, obs.measurements[obs_type]);
//            }
//        }
//    }
//
//    return true;
//}

bool RtcmParser::parseSsrMessage(const std::vector<uint8_t>& message, uint32_t msg_type) {
    // Simplified SSR parsing (placeholder for full implementation)
    if (message.size() < 12) {
        spdlog::warn("Invalid SSR message size: {}", message.size());
        return false;
    }

    // Determine system
    System system = System::GPS; // Placeholder; needs parsing
    // Extract TOW
    uint32_t tow_ms = (message[3] << 24) | (message[4] << 16) | (message[5] << 8) | message[6];
    uint16_t week = 0; // Placeholder
    GnssTime time = RtcmParser::convertToGpsTime(tow_ms, week);

    // Parse correction
    RtcmSsrCorrection corr;
    corr.system = system;
    corr.time = time;
    corr.satellite = "G01"; // Placeholder
    corr.correction_type = (msg_type == 1264) ? "ORBIT" : "CLOCK";
    corr.value = 0.0; // Placeholder
    corr.std_dev = 0.01; // Placeholder
    ssr_corrections_.push_back(corr);

    spdlog::trace("Parsed SSR correction: {} {} = {}", corr.satellite, corr.correction_type, corr.value);
    return true;
}

GnssTime RtcmParser::convertToGpsTime(uint32_t tow_ms, uint16_t week) const {
    double tow = tow_ms / 1000.0; // Convert ms to seconds
    return GnssTime{static_cast<int>(week), tow};
}

std::string RtcmParser::signalIdToObsType(System system, uint8_t signal_id) const {
    // Simplified mapping (placeholder)
    static const std::map<std::pair<System, uint8_t>, std::string> signal_map = {
        {{System::GPS, 2}, "C1C"},
        {{System::GPS, 17}, "L1C"},
        {{System::Galileo, 2}, "C1B"},
        {{System::Galileo, 12}, "L1B"}
        // Add more mappings as needed
    };
    auto key = std::make_pair(system, signal_id);
    auto it = signal_map.find(key);
    return it != signal_map.end() ? it->second : "";
}

} // namespace GnssProcessor
//#endif