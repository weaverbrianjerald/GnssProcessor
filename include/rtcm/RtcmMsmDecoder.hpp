#pragma once
/**
 * @file RtcmMsmDecoder.hpp
 * @brief Decoders for RTCM 3.x Multi-Signal Messages (MSM).
 *
 * Phase 1: MSM7 (full precision, with doppler) for GPS/GLO/GAL/QZS/BDS/SBAS.
 * Message type → system mapping:
 *   1077 GPS     1087 GLONASS  1097 Galileo
 *   1107 SBAS    1117 QZSS     1127 BeiDou   1137 NavIC
 *
 * Output: one @c RinexObservation per (satellite, epoch) with all
 * present (signal, observable) cells populated under RINEX-3 obs codes
 * (e.g. "C1C", "L1C", "S1C", "D1C").  The 12-bit DF002 message-type
 * field is consumed by @c RtcmParser::parseFrame before this decoder
 * runs, so the bit reader starts at offset 12 of the payload.
 *
 * MSM4/MSM5/MSM6 variants share the same overall structure but have
 * narrower per-signal fields; they are not yet implemented and will
 * be added as the same code parameterised by a small variant struct.
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
    int       msm_subtype{0};            ///< 4-7 for MSM4..MSM7.
    int       station_id{0};
    GnssTime  epoch_time{0, 0.0};
    bool      multi_message{false};
    int       n_satellites{0};
    int       n_signals{0};
    int       n_cells{0};                ///< popcount of the cell mask.

    /// Bits consumed by the decoder from the payload (i.e. the bit reader
    /// position at decode end).  A structurally-correct MSM frame consumes
    /// every defined field; the only slack is the 0-7 trailing pad bits that
    /// round the message to a byte boundary.  If @c payload_len*8 exceeds this
    /// by >= 8 bits the decoder under-consumed — a width / field-order / MSM
    /// subtype mismatch shifted the per-cell block — even though @c ok stays
    /// true (the bit reader only flags over-read, never under-read).  Callers
    /// can assert @c (payload_len*8 - bits_consumed) < 8 as a framing-integrity
    /// guard that @c ok() alone does not provide.
    std::size_t bits_consumed{0};
};

/**
 * @brief Decode an MSM7 message (RTCM 1077/1087/1097/1107/1117/1127).
 *
 * @param payload          Pointer to the message payload (DF002 included).
 * @param len_bytes        Payload length in bytes.
 * @param msg_type         The DF002 message-type value (used to dispatch system).
 * @param current_gps_week GPS week used to promote the 30-bit ms-of-week
 *                         epoch field into a full @c GnssTime.  Pass 0
 *                         to leave @c epoch_time.week zero.
 * @param[out] observations  One @c RinexObservation appended per active
 *                         satellite.  Existing entries are not touched.
 *
 * @return Populated result struct; @c ok=true iff the entire message was
 *         decoded without truncation.
 */
MsmDecodeResult decodeMsm7(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<RinexObservation>& observations);

/**
 * @brief Decode an MSM4 message (RTCM 1074/1084/1094/1104/1114/1124).
 *
 * MSM4 is a narrower-precision variant of MSM7: 15-bit fine pseudorange
 * (vs 20), 22-bit fine phase (vs 24), 4-bit lock time (vs 10), 6-bit
 * integer-dB-Hz CNR (vs 10-bit 1/16 dB-Hz), and no range-rate fields
 * (no Doppler observable).  Septentrio receivers commonly emit MSM4 on
 * bandwidth-constrained NTRIP links — same observation set as MSM7,
 * just at lower precision.
 */
MsmDecodeResult decodeMsm4(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<RinexObservation>& observations);

} // namespace GnssProcessor::RtcmMsm
