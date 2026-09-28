#pragma once
/**
 * @file RtcmSsrDecoder.hpp
 * @brief Per-message-body decoders for legacy RTCM SSR (1057-1068, 1240-1270)
 *        and IGS-SSR 4076 sub-types.
 *
 * Bit-level extraction from RTCM 3.x payloads.  @c RtcmStreamReader strips the
 * header bytes (preamble and length) and verifies the CRC.
 * @c RtcmParser::parseHeader reads the 12-bit message-type field (DF002) to
 * select a decoder; each decoder skips those 12 bits and starts reading at bit
 * offset 12 of the payload.
 *
 * Decoders append per-satellite records to caller-owned output vectors
 * and return a @c DecodeResult carrying the shared SSR header info
 * (epoch time, IOD-SSR, multi-message bit, etc.).  They never throw.
 *
 * Field widths and scale factors are transcribed from RTCM Standard 10403.3
 * Annex A (Tables A.1-1 to A.1-6).  SSR does not use sign-magnitude; all
 * signed fields are two's-complement.
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
 * Orbit-bearing records (orbit and combined) are tagged
 * @c SsrReferencePoint::APC: legacy RTCM SSR corrects the broadcast orbit to
 * the satellite antenna phase center (RTCM 10403.3 sec. 3.5.12), whereas
 * IGS-SSR 4076 uses the center of mass (RTB-5(b)).  The legacy Galileo
 * 10-bit IODnav is masked to 8 bits at decode so it matches the broadcast
 * side's @c IODnav&0xFF convention (see @c LiveEphemerisStore::ssrMatchIode;
 * RTB-5(a)).
 *
 * @param payload     Pointer to the message payload (excluding 3-byte header
 *                    and 3-byte CRC).  The first 12 bits are DF002 (message
 *                    type); the decoder skips them.
 * @param len_bytes   Payload length in bytes.
 * @param msg_type    The 12-bit DF002 value extracted from the payload header.
 * @param anchor_gps_seconds  Continuous GPS seconds of a known nearby epoch.
 *                    The 20-bit GPS TOW is promoted into the week that
 *                    minimizes |Δt| to the anchor (nearest continuation,
 *                    RTKLIB adjweek), and the 17-bit GLONASS TOD is placed in
 *                    the GPS day around the anchor.  @c RtcmParser supplies it
 *                    from its per-parser stream-time tracker, which uses the
 *                    host clock only at cold start (RTB-1(a)).  With a value
 *                    ≤ 0, @c epoch_time.week stays zero (for GLONASS the
 *                    whole epoch is {0, 0}).
 *
 * @return @c DecodeResult.ok = true if the entire message decoded without
 *         truncation.  The @c header field is populated whenever the shared
 *         SSR header parsed, even if the per-satellite body decode stopped
 *         partway through.
 */
DecodeResult decodeLegacy(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
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
 * Number) around a body with the same per-satellite fields as the legacy
 * RTCM-SSR per-system messages (1057-1068, 1240-1270), but with uniform
 * widths: 6-bit satellite ID, 8-bit IOD and a 20-bit GPS TOW epoch for every
 * system, GLONASS included.  This function reads the wrapper, dispatches on
 * the IM number to a (system, body type) pair, applies the 4076 widths, and
 * reuses the per-satellite decoders of @c decodeLegacy.
 *
 * IM mapping (per IGS-SSR v1.00):
 *   IM = 10·k + offset, where:
 *     k ∈ {2=GPS, 4=GLO, 6=GAL, 8=QZS, 10=BDS, 12=SBAS}
 *     offset ∈ {1=Orbit, 2=Clock, 3=Combined, 4=HighRate,
 *               5=CodeBias, 6=PhaseBias, 7=URA}
 *
 * VTEC (IM=201) and NavIC (IM=141-147) are not supported and return
 * @c ok=false before the shared header is read.
 *
 * Orbit-bearing records are tagged @c SsrReferencePoint::CoM: IGS-SSR
 * corrects the broadcast orbit to the satellite center of mass (IGS SSR
 * Format v1.00 sec. 4), matching the SP3 convention, so the user side still
 * applies the satellite PCO (RTB-5(b)).  The IOD field is 8 bits for every
 * system, so no masking is needed (see @c decodeLegacy for the legacy
 * Galileo 10-bit case).
 *
 * @param payload          Pointer to the message payload (DF002 included).
 * @param len_bytes        Payload length in bytes.
 * @param anchor_gps_seconds Continuous GPS seconds of a known nearby epoch,
 *                         used for nearest-continuation week promotion of the
 *                         20-bit TOW epoch as in @ref decodeLegacy (RTB-1(a)).
 *
 * @return @c DecodeResult.ok = true if the entire message decoded cleanly.
 */
DecodeResult decodeIgs4076(
    const uint8_t* payload,
    std::size_t    len_bytes,
    double         anchor_gps_seconds,
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
