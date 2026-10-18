#pragma once
/**
 * @file RtcmParser.hpp
 * @brief Top-level dispatcher for decoded RTCM 3.x messages.
 *
 * Accepts framed RTCM3 messages (preamble byte 0xD3 + 10-bit payload length
 * + payload + 24-bit CRC-24Q) and delegates the payload to per-message
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
 * The parser keeps a per-instance tracker of the last decoded stream epoch in
 * continuous GPS seconds, and promotes every mod-week / time-of-day epoch field
 * (MSM ms-of-week, SSR 20-bit TOW, GLONASS TOD, broadcast-nav truncated weeks)
 * by nearest continuation to that tracker (the week that minimizes |Δt|, as in
 * RTKLIB @c adjweek).  The host clock is consulted only at cold start (the first
 * time-bearing frame after construction), so a skewed host clock or a promotion
 * race at a week rollover cannot change the week of an established stream.
 * @c clear() drops decoded outputs but keeps the tracker, so the benefit requires
 * one parser instance per stream or engine (as @c RealTimeEngine does).  A caller
 * that constructs a parser per frame falls back to the legacy behaviour of
 * anchoring every frame on the host clock.
 *
 * @par GLONASS ephemeris dates
 * A GLONASS 1020 message carries only a time of day.  It is dated from the same
 * tracker as the observations (nearest match within ±12 h), so a replay of
 * historical data keeps every GLONASS ephemeris on the day of its observations.
 * A 1020 that arrives before any observation or SSR frame has set the tracker
 * has no stream epoch to be dated from; it is held (at most
 * @c kMaxDeferredGlonass records) and dated, then appended to the decoded
 * ephemerides, by the frame that sets the tracker.  A stream that carries no
 * time-bearing frame at all (navigation messages only) would never release
 * them, so a full hold is dated from the host clock instead.
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
     *         Unsupported message types are not errors at the framer level;
     *         they are logged at @c trace and return false here.
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

    /// Drop all accumulated decoded outputs and reset lastMessageType().  The
    /// stream-time tracker, the last SSR header and the GLONASS ephemerides still
    /// waiting for a stream epoch are kept.
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
    /// Decoded 1033 receiver and antenna descriptor blocks (RTB-2(d)).  Exposed for
    /// a future SIF (station-info) cross-check; not consumed anywhere yet.
    const std::vector<RtcmReceiverAntennaDescriptor>& getReceiverDescriptors() const { return receiver_descriptors_; }
    /// Decoded 1230 GLONASS code-phase bias blocks (RTB-2(d)).  Exposed only; not
    /// wired into the GLONASS IFB path (see RtcmGlonassBias).
    const std::vector<RtcmGlonassBias>&       getGlonassBiases()     const { return glonass_biases_; }

    /// The message type of the most recently decoded frame, or -1 if none.
    int lastMessageType() const { return last_msg_type_; }

    /// Most GLONASS 1020 records held before a stream epoch is known (see the class
    /// documentation).  Twice the 24-slot constellation: a navigation-only input
    /// fills it within the first broadcast cycles.
    static constexpr std::size_t kMaxDeferredGlonass = 48;

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
     * Holds the header (epoch time, IOD-SSR, provider/solution id) of the last
     * legacy or IGS-SSR message this parser decoded.  @c clear() does not reset it,
     * so on the realtime path, which reuses one parser and calls @c clear() before
     * each frame, it still refers to the last SSR frame after a non-SSR frame.
     * Default-constructed until an SSR message has been decoded.
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
     * @return true on success; false if the preamble byte is wrong or the buffer is
     *         shorter than 5 bytes or than the full frame (3 + payload + 3).
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
     * Returns the tracker when set.  On cold start (tracker unset) it seeds the
     * tracker from the host clock (@c TimeUtils::nowContinuousGpsSeconds); this is
     * the only point where the host clock enters the promotion.
     */
    double anchorGpsSeconds();

    /// Advance the tracker to a freshly decoded stream epoch (ignored unless
    /// the epoch carries a resolved week, i.e. week > 0).  The first such epoch
    /// also dates and releases the GLONASS ephemerides held so far.
    void updateTimeAnchor(const GnssTime& epoch);

    /**
     * @brief Hold a GLONASS 1020 record until the stream time is known.
     *
     * Decodes the message without a date (the payload is validated now), keeps the
     * record with its time of day, and when the hold is full dates the whole hold
     * from the host clock (see the class documentation).
     *
     * @return true when the message decoded cleanly.
     */
    bool deferGlonassEphemeris(const uint8_t* payload, std::size_t len);

    /// Date every held GLONASS record from @p anchor_gps_seconds, append the
    /// records to the decoded ephemerides and empty the hold.
    void releaseDeferredGlonass(double anchor_gps_seconds);

    std::filesystem::path file_path_;
    int last_msg_type_{-1};
    SsrHeader last_ssr_header_{};

    /// Last decoded stream epoch as continuous GPS seconds (week·604800 + tow);
    /// < 0 until the first time-bearing frame.  Kept across clear() (see the class
    /// documentation; RTB-1(a)).
    double time_anchor_gps_s_{-1.0};

    /// True once the tracker has been set from a decoded stream epoch (an MSM or
    /// SSR frame), not merely seeded from the host clock at cold start.
    bool time_anchor_from_stream_{false};

    /// A GLONASS 1020 record decoded before the stream time was known, with its
    /// undated time of day.  Kept across clear() like the tracker.
    struct DeferredGlonass {
        BroadcastEphemeris eph;     ///< Record with @c glonass.toe still {0, 0}.
        double             tod_s;   ///< Reference time as seconds of the GLONASS day.
    };
    std::vector<DeferredGlonass> deferred_glonass_;

    bool logged_1033_{false};  ///< 1033 content logged once per parser at info (RTB-2(d)).
    bool logged_1230_{false};  ///< 1230 content logged once per parser at info (RTB-2(d)).

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
