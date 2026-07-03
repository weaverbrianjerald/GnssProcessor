#pragma once
/**
 * @file RtcmParser.hpp
 * @brief Top-level dispatcher for decoded RTCM 3.x messages.
 *
 * Accepts framed RTCM3 messages (preamble byte 0xD3 + 10-bit payload length
 * + payload + 24-bit CRC-Q24) and delegates the payload to per-message
 * decoders.  Decoded outputs are appended to per-type vectors that the
 * caller can drain and pass to higher-level consumers (the realtime engine,
 * the SSR correction store, the reference-station coordinate cache).
 *
 * Frame integrity is the framer's job (@c RtcmStreamReader checks CRC).
 * This parser assumes @p frame is a verified, complete frame.
 */

#include "rtcm/RtcmTypes.hpp"
#include "products/BroadcastEphemerisTypes.hpp"   // BroadcastEphemeris
#include "rinex/RinexParser.hpp"   // RinexObservation
#include <filesystem>
#include <vector>
#include <cstdint>

namespace GnssProcessor {

/**
 * @class RtcmParser
 * @brief Dispatches RTCM 3.x frames to typed decoders.
 *
 * Construction is cheap; instances may be reused across frames.  Outputs
 * accumulate in per-type vectors until @c clear() is called.
 *
 * @par Time continuity (RTB-1(a))
 * The parser keeps a per-instance continuous-GPS-seconds tracker of the last
 * decoded stream epoch and promotes every mod-week / time-of-day epoch field
 * (MSM ms-of-week, SSR 20-bit TOW, GLONASS TOD, broadcast-nav truncated
 * weeks) by NEAREST CONTINUATION to that tracker (the week minimizing |Δt| —
 * RTKLIB @c adjweek semantics).  The host clock is consulted ONLY at cold
 * start (the first time-bearing frame after construction), so a skewed host
 * clock or a promotion race at a week rollover cannot re-week an established
 * stream.  @c clear() drops decoded outputs but deliberately PRESERVES the
 * tracker — reuse one parser instance per stream/engine (as
 * @c RealTimeEngine now does) to benefit; a parser-per-frame caller
 * degenerates to the legacy host-clock-per-frame behaviour.
 */
class RtcmParser {
public:
    /**
     * @brief Construct a parser.
     * @param file_path Optional file path used only by @c parse() for the
     *                  binary-dump replay path.  Leave empty when feeding
     *                  individual frames via @c parseFrame.
     */
    explicit RtcmParser(std::filesystem::path file_path = {});

    /**
     * @brief Decode a single RTCM3 frame.
     * @param frame Pointer to the first byte (must equal 0xD3 preamble).
     * @param len   Total frame length in bytes (header 3 + payload + CRC 3).
     * @return true if the frame was recognised and decoded; false on bad
     *         layout, unknown / unsupported message type, or decode error.
     *         Unsupported message types are NOT errors at the framer level
     *         — they're logged at @c trace and return false here.
     */
    bool parseFrame(const uint8_t* frame, std::size_t len);

    /// Convenience overload accepting std::vector.
    bool parseFrame(const std::vector<uint8_t>& frame) {
        return parseFrame(frame.data(), frame.size());
    }

    /**
     * @brief Walk a binary RTCM3 dump file frame-by-frame.
     * @return true if at least one frame produced decoded output.
     *
     * Used by tests and by the file-replay realtime mode.  Live streaming
     * uses @c parseFrame directly per frame returned by @c RtcmStreamReader.
     */
    bool parse();

    /// Drop all accumulated decoded outputs.
    void clear();

    // ---------------------------------------------------------------------
    // Decoded outputs.  All are append-only across calls to parseFrame()
    // until clear() is invoked.
    // ---------------------------------------------------------------------

    const std::vector<RinexObservation>&      getObservations()      const { return observations_; }
    const std::vector<SsrOrbitCorrection>&    getSsrOrbits()         const { return ssr_orbits_; }
    const std::vector<SsrClockCorrection>&    getSsrClocks()         const { return ssr_clocks_; }
    const std::vector<SsrCombinedCorrection>& getSsrCombined()       const { return ssr_combined_; }
    const std::vector<SsrCodeBias>&           getSsrCodeBiases()     const { return ssr_code_biases_; }
    const std::vector<SsrPhaseBias>&          getSsrPhaseBiases()    const { return ssr_phase_biases_; }
    const std::vector<SsrUra>&                getSsrUras()           const { return ssr_uras_; }
    const std::vector<SsrHighRateClock>&      getSsrHighRateClocks() const { return ssr_hrclocks_; }
    const std::vector<RtcmStationCoords>&     getStationCoords()     const { return station_coords_; }
    const std::vector<BroadcastEphemeris>&    getBroadcastEphemeris() const { return broadcast_eph_; }
    /// RTB-2(d): decoded 1033 receiver+antenna descriptor blocks.  Surfaced
    /// for a future SIF (station-info) cross-check; not consumed anywhere yet.
    const std::vector<RtcmReceiverAntennaDescriptor>& getReceiverDescriptors() const { return receiver_descriptors_; }
    /// RTB-2(d): decoded 1230 GLONASS code-phase bias blocks.  Surfaced only;
    /// deliberately NOT wired into the GLONASS IFB path (see RtcmGlonassBias).
    const std::vector<RtcmGlonassBias>&       getGlonassBiases()     const { return glonass_biases_; }

    /// The message type of the most recently decoded frame, or -1 if none.
    int lastMessageType() const { return last_msg_type_; }

    /**
     * @brief The per-parser stream-time tracker (continuous GPS seconds).
     *
     * The last decoded MSM / SSR epoch, promoted to a full GPS time; every
     * subsequent mod-week epoch field is promoted by nearest continuation to
     * this value (RTB-1(a)).  Negative until the first time-bearing frame
     * (cold start pending).  Exposed for tests and diagnostics.
     */
    double timeAnchorGpsSeconds() const { return time_anchor_gps_s_; }

    /**
     * @brief The SSR header from the most recently decoded SSR message.
     *
     * The realtime path constructs one parser per frame, so after decoding a
     * single SSR frame this is exactly that message's header (epoch time,
     * IOD-SSR, provider/solution id).  Default-constructed until an SSR
     * message has been decoded.
     */
    const SsrHeader& getLastSsrHeader() const { return last_ssr_header_; }

    /**
     * @brief Decode a frame's header.
     *
     * Public for unit tests; production callers should go through @c parseFrame.
     *
     * Layout (RTCM 10403.x, sec 4.1):
     *   - byte 0      : preamble 0xD3
     *   - bytes 1-2   : 6 reserved bits + 10-bit payload length (big-endian)
     *   - bytes 3..   : payload (first 12 bits = DF002 message number)
     *   - last 3 bytes: CRC-24Q over header + payload
     *
     * @param frame       Frame bytes (length must be ≥ 5 to read both header and msg type).
     * @param len         Buffer length.
     * @param payload_len Out: 10-bit payload length from bytes 1-2.
     * @param msg_type    Out: 12-bit DF002 message type from payload bytes 0-1.
     * @return true on success, false if the buffer is too short or the preamble byte is wrong.
     */
    static bool parseHeader(const uint8_t* frame, std::size_t len,
                            uint16_t& payload_len, uint16_t& msg_type);

private:
    // Per-message-type decoders.  Each receives the payload (no header,
    // no CRC) and the decoded message type.  Implementations live in
    // RtcmParser.cpp / RtcmSsrDecoder.cpp.
    bool decodeStationCoords(const uint8_t* payload, std::size_t len, uint16_t msg_type);
    bool decodeLegacySsr(const uint8_t* payload, std::size_t len, uint16_t msg_type);
    bool decodeIgsSsr4076(const uint8_t* payload, std::size_t len);
    bool decodeReceiverAntennaDescriptor(const uint8_t* payload, std::size_t len);  ///< RTCM 1033 (RTB-2(d)).
    bool decodeGlonassBiases(const uint8_t* payload, std::size_t len);              ///< RTCM 1230 (RTB-2(d)).

    /**
     * @brief Anchor for nearest-continuation week promotion (RTB-1(a)).
     *
     * Returns the tracker when set; on cold start (tracker unset) seeds it
     * from the host clock (@c TimeUtils::nowContinuousGpsSeconds) — the ONLY
     * point where the host clock enters the promotion.
     */
    double anchorGpsSeconds();

    /// Advance the tracker to a freshly decoded stream epoch (ignored unless
    /// the epoch carries a resolved week, i.e. week > 0).
    void updateTimeAnchor(const GnssTime& epoch);

    std::filesystem::path file_path_;
    int last_msg_type_{-1};
    SsrHeader last_ssr_header_{};

    /// RTB-1(a): last decoded stream epoch as continuous GPS seconds
    /// (week·604800 + tow); < 0 until the first time-bearing frame.
    /// Preserved across clear() — see the class Doxygen.
    double time_anchor_gps_s_{-1.0};

    bool logged_1033_{false};  ///< RTB-2(d): 1033 content logged once per parser (info).
    bool logged_1230_{false};  ///< RTB-2(d): 1230 content logged once per parser (info).

    std::vector<RinexObservation>      observations_;
    std::vector<SsrOrbitCorrection>    ssr_orbits_;
    std::vector<SsrClockCorrection>    ssr_clocks_;
    std::vector<SsrCombinedCorrection> ssr_combined_;
    std::vector<SsrCodeBias>           ssr_code_biases_;
    std::vector<SsrPhaseBias>          ssr_phase_biases_;
    std::vector<SsrUra>                ssr_uras_;
    std::vector<SsrHighRateClock>      ssr_hrclocks_;
    std::vector<RtcmStationCoords>     station_coords_;
    std::vector<BroadcastEphemeris>    broadcast_eph_;
    std::vector<RtcmReceiverAntennaDescriptor> receiver_descriptors_;  ///< RTCM 1033 (RTB-2(d)).
    std::vector<RtcmGlonassBias>       glonass_biases_;                ///< RTCM 1230 (RTB-2(d)).
};

} // namespace GnssProcessor
