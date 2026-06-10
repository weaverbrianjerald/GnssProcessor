#pragma once
/**
 * @file RtcmSsrDecoder.hpp
 * @brief Per-message-body decoders for legacy RTCM SSR (1057-1068, 1240-1270)
 *        and IGS-SSR 4076 sub-types.
 *
 * Bit-level extraction from RTCM 3.x payloads.  Header bytes (preamble +
 * length) are stripped by @c RtcmStreamReader; CRC is verified there; the
 * 12-bit message-type field is consumed by @c RtcmParser::parseHeader
 * before invoking the decoders here.  Each decoder therefore starts at
 * bit offset 12 of the payload (i.e. immediately past DF002).
 *
 * Decoders append per-satellite records to caller-owned output vectors
 * and return a @c DecodeResult carrying the shared SSR header info
 * (epoch time, IOD-SSR, multi-message bit, etc.).  They never throw.
 *
 * Field widths and scale factors transcribed from RTCM Standard 10403.3
 * Annex A (Tables A.1-1 to A.1-6).  Sign-magnitude is NOT used by SSR;
 * all signed fields are two's-complement.
 */

#include "rtcm/RtcmTypes.hpp"
#include "common/GnssTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace GnssProcessor::RtcmSsr {

/**
 * @brief Result of a single SSR message decode.
 */
struct DecodeResult {
    bool ok{false};                ///< False on truncated payload, unsupported sub-type, etc.
    SsrHeader header{};            ///< Decoded common header fields.
};

/**
 * @brief Decode a legacy per-system SSR message (RTCM 1057-1068 / 1240-1270).
 *
 * Dispatches on @p msg_type to the appropriate per-system body decoder.
 * Outputs are appended to whichever vector matches the message subtype:
 *   - 1057/1063/1240/1246/1252/1258                → @p orbits
 *   - 1058/1064/1241/1247/1253/1259                → @p clocks
 *   - 1060/1066/1243/1249/1255/1261                → @p combined
 *   - 1059/1065/1242/1248/1254/1260                → @p code_biases
 *   - 1265/1266/1267/1268/1269/1270                → @p phase_biases
 *   - 1061/1067/1244/1250/1256/1262                → @p uras
 *   - 1062/1068/1245/1251/1257/1263                → @p hrclocks
 *
 * @param payload     Pointer to the message payload (excluding 3-byte header
 *                    and 3-byte CRC).  First 12 bits are DF002 (message type)
 *                    and will be consumed by the decoder.
 * @param len_bytes   Payload length in bytes.
 * @param msg_type    The 12-bit DF002 value extracted from the payload header.
 * @param current_gps_week  GPS week at which the frame was received (used to
 *                    promote the 20-bit GPS TOW or 17-bit GLO TOD into a full
 *                    @c GnssTime).  Pass 0 to leave @c epoch_time.week zero.
 *
 * @return @c DecodeResult.ok = true if the entire message decoded without
 *         truncation; the @c header field is always populated when the
 *         shared SSR header parsed successfully, even if per-satellite
 *         body decode hit an issue partway through.
 */
DecodeResult decodeLegacy(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<SsrOrbitCorrection>&    orbits,
    std::vector<SsrClockCorrection>&    clocks,
    std::vector<SsrCombinedCorrection>& combined,
    std::vector<SsrCodeBias>&           code_biases,
    std::vector<SsrPhaseBias>&          phase_biases,
    std::vector<SsrUra>&                uras,
    std::vector<SsrHighRateClock>&      hrclocks);

/**
 * @brief Decode an IGS-SSR (RTCM 4076) message.
 *
 * 4076 wraps an 11-bit prefix (3-bit IGS version + 8-bit IGS Message
 * Number) around a body that is otherwise identical to the legacy
 * RTCM-SSR per-system-per-body messages (1057-1068, 1240-1270).  This
 * function reads the wrapper, dispatches on the IM number to a
 * (system, body type) pair, and reuses the same per-satellite decoders
 * as @c decodeLegacy.
 *
 * IM mapping (per IGS-SSR v1.00):
 *   IM = 10·sys + offset, where:
 *     sys ∈ {1=GPS, 2=GLO, 4=GAL, 5=QZS, 6=BDS, 7=SBAS}
 *     offset ∈ {1=Orbit, 2=Clock, 3=Combined, 4=HighRate,
 *               5=CodeBias, 6=PhaseBias, 7=URA}
 *
 * VTEC (IM=201) and NavIC (IM=81-87) are not yet supported and decoded
 * as @c ok=false.  The shared header is still populated for diagnostics.
 *
 * @param payload          Pointer to the message payload (DF002 included).
 * @param len_bytes        Payload length in bytes.
 * @param current_gps_week GPS week used to promote the 20-bit ms-of-week
 *                         epoch into a full @c GnssTime.
 *
 * @return @c DecodeResult.ok = true if the entire message decoded cleanly.
 */
DecodeResult decodeIgs4076(
    const uint8_t* payload,
    std::size_t    len_bytes,
    int            current_gps_week,
    std::vector<SsrOrbitCorrection>&    orbits,
    std::vector<SsrClockCorrection>&    clocks,
    std::vector<SsrCombinedCorrection>& combined,
    std::vector<SsrCodeBias>&           code_biases,
    std::vector<SsrPhaseBias>&          phase_biases,
    std::vector<SsrUra>&                uras,
    std::vector<SsrHighRateClock>&      hrclocks);

/**
 * @brief Decode an RTCM 1005/1006 stationary reference station ARP.
 *
 * 1005 carries ECEF coordinates only; 1006 adds a 16-bit antenna height
 * (units 0.0001 m).  Only the station ID, ECEF position, and (for 1006)
 * antenna height are populated; receiver-type / station-type / GNSS-bits
 * fields are skipped.
 */
bool decodeStationCoords(
    const uint8_t*       payload,
    std::size_t          len_bytes,
    uint16_t             msg_type,
    RtcmStationCoords&   out);

} // namespace GnssProcessor::RtcmSsr
