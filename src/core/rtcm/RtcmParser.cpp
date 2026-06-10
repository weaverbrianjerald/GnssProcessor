/**
 * @file RtcmParser.cpp
 * @brief Top-level RTCM 3.x dispatcher.
 *
 * Frame structure (RTCM 10403.x sec. 4.1):
 *   - byte 0       : preamble 0xD3
 *   - bytes 1-2    : 6 reserved bits + 10-bit payload length (big-endian)
 *   - bytes 3..N+2 : payload of N bytes
 *   - bytes N+3..  : 24-bit CRC-Q24 over (header + payload)
 *
 * The 12-bit message-type field DF002 lives in the FIRST 12 BITS OF THE
 * PAYLOAD (bytes 3-4), NOT the header.  This is the single most common
 * source of confusion in DIY RTCM decoders.  CRC verification is done by
 * RtcmStreamReader before frames reach this parser, so this layer assumes
 * frame integrity.
 */

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmSsrDecoder.hpp"
#include "rtcm/RtcmMsmDecoder.hpp"
#include "rtcm/RtcmNavDecoder.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <fstream>

namespace GnssProcessor {

namespace {

// Best-effort current GPS week from the system clock.  Used to promote
// the 20-bit GPS-TOW SSR epoch field into a full GnssTime.  In a future
// pass this should come from the rover's most-recently-known time so the
// SSR epoch and the rover epoch share a clock.
int currentGpsWeekFromSystem() {
    using namespace std::chrono;
    const auto now      = system_clock::now();
    const auto since    = duration_cast<seconds>(now.time_since_epoch()).count();
    constexpr int64_t kGpsEpochUnix = 315964800; // Jan 6 1980 00:00:00 UTC
    constexpr int     kSecPerWeek   = 7 * 24 * 3600;
    const int64_t since_gps = since - kGpsEpochUnix;
    if (since_gps <= 0) return 0;
    return static_cast<int>(since_gps / kSecPerWeek);
}

bool isLegacySsrType(uint16_t mt) {
    return (mt >= 1057 && mt <= 1068) ||
           (mt >= 1240 && mt <= 1270);
}

bool isMsm7Type(uint16_t mt) {
    // 1077 GPS, 1087 GLO, 1097 GAL, 1107 SBAS, 1117 QZS, 1127 BDS, 1137 NavIC.
    if (mt < 1071 || mt > 1137) return false;
    return ((mt - 1071) % 10) == 6;  // MSM7 sits at offset 6 within each per-system block.
}

bool isMsm4Type(uint16_t mt) {
    // 1074 GPS, 1084 GLO, 1094 GAL, 1104 SBAS, 1114 QZS, 1124 BDS, 1134 NavIC.
    if (mt < 1071 || mt > 1137) return false;
    return ((mt - 1071) % 10) == 3;  // MSM4 sits at offset 3 within each per-system block.
}

} // namespace

RtcmParser::RtcmParser(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

bool RtcmParser::parseHeader(const uint8_t* frame, std::size_t len,
                             uint16_t& payload_len, uint16_t& msg_type) {
    if (!frame || len < 5) return false;
    if (frame[0] != 0xD3) return false;
    payload_len = static_cast<uint16_t>(((frame[1] & 0x03u) << 8) | frame[2]);
    if (payload_len > 1023) return false;
    if (len < static_cast<std::size_t>(3 + payload_len + 3)) return false;
    // DF002 = first 12 bits of payload (frame[3]..frame[4]).
    msg_type = static_cast<uint16_t>(
        (static_cast<uint16_t>(frame[3]) << 4) |
        (static_cast<uint16_t>(frame[4]) >> 4));
    return true;
}

bool RtcmParser::parseFrame(const uint8_t* frame, std::size_t len) {
    uint16_t payload_len = 0;
    uint16_t msg_type    = 0;
    if (!parseHeader(frame, len, payload_len, msg_type)) {
        spdlog::trace("RTCM parseHeader failed (len={})", len);
        return false;
    }
    last_msg_type_ = static_cast<int>(msg_type);

    const uint8_t* payload = frame + 3;
    const std::size_t pl   = static_cast<std::size_t>(payload_len);

    if (msg_type == 1005 || msg_type == 1006) {
        return decodeStationCoords(payload, pl, msg_type);
    }
    if (msg_type == 4076) {
        return decodeIgsSsr4076(payload, pl);
    }
    if (isLegacySsrType(msg_type)) {
        return decodeLegacySsr(payload, pl, msg_type);
    }
    if (RtcmNav::isBroadcastEphemerisType(msg_type)) {
        const int week = currentGpsWeekFromSystem();
        const auto res = RtcmNav::decodeBroadcast(payload, pl, msg_type, week, broadcast_eph_);
        if (!res.ok) {
            spdlog::warn("RTCM {}: broadcast-ephemeris decode incomplete (truncated payload?)", msg_type);
        }
        return res.ok;
    }
    if (isMsm7Type(msg_type)) {
        const int week = currentGpsWeekFromSystem();
        const auto res = RtcmMsm::decodeMsm7(payload, pl, msg_type, week, observations_);
        if (!res.ok) {
            spdlog::warn("RTCM {}: MSM7 decode incomplete (truncated payload?)", msg_type);
        }
        return res.ok;
    }
    if (isMsm4Type(msg_type)) {
        const int week = currentGpsWeekFromSystem();
        const auto res = RtcmMsm::decodeMsm4(payload, pl, msg_type, week, observations_);
        if (!res.ok) {
            spdlog::warn("RTCM {}: MSM4 decode incomplete (truncated payload?)", msg_type);
        }
        return res.ok;
    }

    spdlog::trace("RTCM message type {} not yet decoded", msg_type);
    return false;
}

bool RtcmParser::decodeStationCoords(const uint8_t* payload, std::size_t len, uint16_t msg_type) {
    RtcmStationCoords s;
    if (!RtcmSsr::decodeStationCoords(payload, len, msg_type, s)) {
        spdlog::warn("RTCM {}: station coordinates decode failed", msg_type);
        return false;
    }
    station_coords_.push_back(std::move(s));
    return true;
}

bool RtcmParser::decodeLegacySsr(const uint8_t* payload, std::size_t len, uint16_t msg_type) {
    const int week = currentGpsWeekFromSystem();
    const auto result = RtcmSsr::decodeLegacy(
        payload, len, msg_type, week,
        ssr_orbits_, ssr_clocks_, ssr_combined_, ssr_code_biases_,
        ssr_phase_biases_, ssr_uras_, ssr_hrclocks_);
    last_ssr_header_ = result.header;
    if (!result.ok) {
        spdlog::warn("RTCM {}: legacy SSR decode incomplete (truncated payload?)", msg_type);
    }
    return result.ok;
}

bool RtcmParser::decodeIgsSsr4076(const uint8_t* payload, std::size_t len) {
    const int week = currentGpsWeekFromSystem();
    const auto result = RtcmSsr::decodeIgs4076(
        payload, len, week,
        ssr_orbits_, ssr_clocks_, ssr_combined_, ssr_code_biases_,
        ssr_phase_biases_, ssr_uras_, ssr_hrclocks_);
    last_ssr_header_ = result.header;
    if (!result.ok) {
        spdlog::trace("RTCM 4076: IGS-SSR decode incomplete or unsupported sub-type");
    }
    return result.ok;
}

bool RtcmParser::parse() {
    std::ifstream stream(file_path_, std::ios::binary);
    if (!stream.is_open()) {
        spdlog::error("RtcmParser: cannot open {}", file_path_.string());
        return false;
    }

    // Read entire file into a buffer; RTCM dumps are typically a few MB.
    stream.seekg(0, std::ios::end);
    const auto fsz = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (fsz <= 0) return false;

    std::vector<uint8_t> buf(static_cast<std::size_t>(fsz));
    stream.read(reinterpret_cast<char*>(buf.data()), fsz);

    std::size_t offset = 0;
    std::size_t frames_decoded = 0;
    while (offset + 3 <= buf.size()) {
        if (buf[offset] != 0xD3) { ++offset; continue; }
        const uint16_t payload_len = static_cast<uint16_t>(
            ((buf[offset + 1] & 0x03u) << 8) | buf[offset + 2]);
        const std::size_t frame_len = 3 + payload_len + 3;
        if (offset + frame_len > buf.size()) break;
        if (parseFrame(buf.data() + offset, frame_len)) ++frames_decoded;
        offset += frame_len;
    }

    spdlog::info("RtcmParser: {} bytes -> {} decoded frames "
                 "(orbits={}, clocks={}, combined={}, codebias={}, phasebias={}, "
                 "ura={}, hrclock={}, station={})",
                 buf.size(), frames_decoded,
                 ssr_orbits_.size(), ssr_clocks_.size(), ssr_combined_.size(),
                 ssr_code_biases_.size(), ssr_phase_biases_.size(),
                 ssr_uras_.size(), ssr_hrclocks_.size(), station_coords_.size());
    return frames_decoded > 0;
}

void RtcmParser::clear() {
    last_msg_type_ = -1;
    observations_.clear();
    ssr_orbits_.clear();
    ssr_clocks_.clear();
    ssr_combined_.clear();
    ssr_code_biases_.clear();
    ssr_phase_biases_.clear();
    ssr_uras_.clear();
    ssr_hrclocks_.clear();
    station_coords_.clear();
    broadcast_eph_.clear();
}

} // namespace GnssProcessor
