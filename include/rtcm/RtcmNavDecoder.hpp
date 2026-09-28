#pragma once
/**
 * @file RtcmNavDecoder.hpp
 * @brief Decoders for RTCM 3.x broadcast-ephemeris messages.
 *
 * Handles the per-constellation broadcast-navigation messages a receiver
 * (e.g. a Septentrio AsteRx-U) emits alongside its MSM observations:
 *   - 1019  GPS LNAV ephemeris
 *   - 1020  GLONASS ephemeris (PZ-90 state)
 *   - 1042  BeiDou D1 ephemeris
 *   - 1044  QZSS ephemeris
 *   - 1046  Galileo I/NAV ephemeris
 *
 * Mirrors @c RtcmSsr: each decoder assumes DF002 (the 12-bit message type)
 * was already consumed by @c RtcmParser::parseHeader, so it skips the first
 * 12 bits and reads the remaining data fields with @c RtcmBitReader.  Field
 * widths and scale factors are transcribed from RTCM Standard 10403.3.
 * Angular elements are converted from semicircles to radians at decode time;
 * BeiDou times are left on the native BDT scale (the @c LiveEphemerisStore
 * consumer converts the query epoch to BDT).
 *
 * Decoders never throw; on a truncated/short payload they leave @p out
 * unchanged and report @c ok = false.
 */

#include "products/BroadcastEphemerisTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace GnssProcessor::RtcmNav {

/// Result of a single broadcast-ephemeris decode.
struct DecodeResult {
    bool ok{false};   ///< False on truncated payload or unsupported message type.
};

/// True for the broadcast-ephemeris message types this decoder handles.
bool isBroadcastEphemerisType(uint16_t msg_type);

/**
 * @brief Decode one broadcast-ephemeris message, appending to @p out.
 *
 * @param payload          Message payload (DF002 included; first 12 bits skipped).
 * @param len_bytes        Payload length in bytes.
 * @param msg_type         The 12-bit DF002 value (1019/1020/1042/1044/1046).
 * @param current_gps_week Current full GPS week, used to resolve the truncated
 *                         10-bit (GPS, QZSS) and 12-bit (Galileo) week fields
 *                         into an unambiguous epoch.  Pass 0 to leave the week
 *                         as the raw (un-rolled) value; Galileo still adds the
 *                         1024-week GST offset.  BeiDou (13-bit BDT week, kept
 *                         raw) and GLONASS (toe anchored on the host clock)
 *                         ignore it.
 * @param out              Destination vector; one record appended on success.
 * @return @c ok = true when the message decoded cleanly.
 */
DecodeResult decodeBroadcast(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<BroadcastEphemeris>& out);

} // namespace GnssProcessor::RtcmNav
