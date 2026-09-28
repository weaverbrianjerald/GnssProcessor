#pragma once
/**
 * @file RtcmMsmDecoder.hpp
 * @brief Decoders for RTCM 3.x Multi-Signal Messages (MSM).
 *
 * Implemented: MSM4, MSM5 and MSM7 for GPS/GLO/GAL/SBAS/QZS/BDS.
 * Message type → system mapping (MSM7 numbers; MSM4 and MSM5 end in 4 and 5):
 *   1077 GPS     1087 GLONASS  1097 Galileo
 *   1107 SBAS    1117 QZSS     1127 BeiDou   1137 NavIC
 * NavIC (113x) follows the numbering but is not decoded; its messages return
 * @c ok=false.
 *
 * Output: one @c RinexObservation per (satellite, epoch) with all
 * present (signal, observable) cells populated under RINEX-3 obs codes
 * (e.g. "C1C", "L1C", "S1C", "D1C").  The decoded per-cell lock time is
 * also carried on @c RinexObservation::lock_time_ms (keyed like the LLI
 * channel, "L1C" etc.) so the epoch-assembly layer can derive loss-of-lock
 * LLI bits statefully (see @c LockTimeTracker); the decoder holds no
 * history.  The payload includes the 12-bit DF002 message-type field, which
 * @c RtcmParser::parseFrame reads for dispatch; the decoder skips it and
 * starts reading at bit 12 of the payload.
 *
 * MSM4/MSM5/MSM7 share the same overall structure and differ only in
 * field widths and capability flags, parameterised by a small variant
 * struct in the implementation.  MSM6 (high precision, no Doppler) is not
 * implemented; no stream consumed by this project emits it.
 */

#include "rtcm/RtcmTypes.hpp"
#include "rinex/RinexParser.hpp"   // RinexObservation
#include "common/GnssTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace GnssProcessor::RtcmMsm {

/**
 * @brief Result of a single MSM message decode.
 */
struct MsmDecodeResult {
    bool      ok{false};
    System    system{System::Unknown};
    int       msm_subtype{0};            ///< 4, 5 or 7 for MSM4, MSM5, MSM7.
    int       station_id{0};
    GnssTime  epoch_time{0, 0.0};
    bool      multi_message{false};
    int       n_satellites{0};
    int       n_signals{0};
    int       n_cells{0};                ///< popcount of the cell mask.

    /// Bits consumed by the decoder from the payload (the bit reader position
    /// at decode end).  A structurally correct MSM frame consumes every defined
    /// field; the only slack is the 0-7 trailing pad bits that round the
    /// message to a byte boundary.  If @c payload_len*8 exceeds this by 8 bits
    /// or more, the decoder under-consumed (a width, field-order or MSM subtype
    /// mismatch shifted the per-cell block) even though @c ok stays true: the
    /// bit reader flags over-read only, never under-read.  Callers can assert
    /// @c (payload_len*8 - bits_consumed) < 8 as a framing-integrity check that
    /// @c ok alone does not provide.
    std::size_t bits_consumed{0};
};

/**
 * @brief Decode an MSM7 message (RTCM 1077/1087/1097/1107/1117/1127).
 *
 * @param payload          Pointer to the message payload (DF002 included).
 * @param len_bytes        Payload length in bytes.
 * @param msg_type         The DF002 message-type value (used to dispatch system).
 * @param anchor_gps_seconds Continuous GPS seconds (week·604800 + tow) of a
 *                         known nearby epoch.  Used to promote the 30-bit
 *                         ms-of-week epoch field into a full @c GnssTime by
 *                         nearest continuation (the week minimizing |Δt| to
 *                         the anchor, as RTKLIB @c adjweek), and to anchor the
 *                         GLONASS TOD→GPS day resolution
 *                         (@c TimeUtils::glonassTodToGpsTime, ±12 h).  The
 *                         caller (@c RtcmParser) keeps this as a per-parser
 *                         last-decoded-epoch tracker, so a skewed host clock
 *                         matters only at cold start (RTB-1(a)).  Pass ≤ 0
 *                         to leave @c epoch_time.week zero (GLONASS epochs
 *                         then stay {0,0}, since there is no anchor to resolve
 *                         the day).
 * @param[out] observations  One @c RinexObservation appended per satellite
 *                         that has a valid rough range and at least one
 *                         decoded measurement.  Existing entries are not touched.
 *
 * @return Populated result struct; @c ok=true iff the message type matched the
 *         variant and the entire message was decoded without truncation.
 */
MsmDecodeResult decodeMsm7(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations);

/**
 * @brief Decode an MSM4 message (RTCM 1074/1084/1094/1104/1114/1124).
 *
 * MSM4 is a lower-precision variant of MSM7: 15-bit fine pseudorange
 * (vs 20), 22-bit fine phase (vs 24), 4-bit lock time (vs 10), 6-bit
 * integer-dB-Hz CNR (vs 10-bit 1/16 dB-Hz), and no range-rate fields
 * (no Doppler observable).  Septentrio receivers commonly emit MSM4 on
 * bandwidth-constrained NTRIP links: the same observation set as MSM7 at
 * lower precision.  Parameters as @ref decodeMsm7.
 */
MsmDecodeResult decodeMsm4(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations);

/**
 * @brief Decode an MSM5 message (RTCM 1075/1085/1095/1105/1115/1125) (RTB-2(c)).
 *
 * MSM5 = MSM4-precision ranges plus the Doppler fields (RTCM 10403.3
 * Table 3.5-75): per-cell DF400 fine pseudorange (15 bits, 2⁻²⁴ ms),
 * DF401 fine phase range (22 bits, 2⁻²⁹ ms), DF402 lock time (4-bit basic
 * table), DF420 half-cycle, DF403 CNR (6 bits, whole dB-Hz), DF404 fine
 * phase range rate (15 bits, 0.0001 m/s); per-satellite DF397/DF398 rough
 * ranges, DF399 rough range rate (14 bits, m/s), and DF419 extended
 * satellite info (present in MSM5/MSM7 for every system; GLONASS FCN).
 * Emits the same observable set as MSM7 (C/L/S/D) at MSM4 precision.
 * Parameters as @ref decodeMsm7.
 */
MsmDecodeResult decodeMsm5(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations);

} // namespace GnssProcessor::RtcmMsm
