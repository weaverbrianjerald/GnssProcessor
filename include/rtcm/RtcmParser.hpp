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
#include <map>
#include <utility>
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

    /// @brief Convenience overload accepting std::vector.
    ///
    /// @param frame One complete, CRC-verified frame (preamble 0xD3, 10-bit length, payload,
    ///        24-bit CRC-24Q); its size is the frame length passed to the pointer overload.
    /// @return As for the pointer overload: true if the frame was recognised and decoded.
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

    /// @brief Drop all accumulated decoded outputs and reset lastMessageType().  The
    /// stream-time tracker, the last SSR header and the GLONASS ephemerides still
    /// waiting for a stream epoch are kept.
    void clear();

    // ---------------------------------------------------------------------
    // Decoded outputs.  All are append-only across calls to parseFrame()
    // until clear() is invoked.
    // ---------------------------------------------------------------------

    /**
     * @brief Observations decoded from Multi-Signal Messages (MSM4, MSM5 and MSM7 of every
     *        system, for example 1074 / 1075 / 1077 for GPS).
     * @return One @c RinexObservation per (satellite, epoch) that has a valid rough range and at
     *         least one measurement, in decode order, under RINEX 3 observation codes ("C1C",
     *         "L1C", "S1C", "D1C").  Empty when none decoded.  The reference stays valid until the
     *         next @c parseFrame or @c clear().
     */
    const std::vector<RinexObservation>&      getObservations()      const { return observations_; }
    /**
     * @brief SSR orbit corrections: legacy RTCM 1057 (GPS), 1063 (GLONASS), 1240 (Galileo),
     *        1246 (QZSS), 1252 (SBAS), 1258 (BeiDou) and the IGS-SSR 4076 orbit sub-types.
     * @return One record per satellite in decode order: radial / along / cross delta in metres
     *         (22-bit field at 0.1 mm, 20-bit fields at 0.4 mm) and its rate in m/s, relative to
     *         the broadcast ephemeris named by the record's IODE.  Legacy records refer to the
     *         antenna phase center, 4076 records to the center of mass.  Empty when none decoded.
     *         Valid until the next @c parseFrame or @c clear().
     */
    const std::vector<SsrOrbitCorrection>&    getSsrOrbits()         const { return ssr_orbits_; }
    /**
     * @brief SSR clock corrections: legacy RTCM 1058 (GPS), 1064 (GLONASS), 1241 (Galileo),
     *        1247 (QZSS), 1253 (SBAS), 1259 (BeiDou) and the IGS-SSR 4076 clock sub-types.
     * @return One record per satellite in decode order: the polynomial c0 (22-bit, 0.1 mm), c1
     *         (21-bit, 0.001 mm/s) and c2 (27-bit, 0.00002 mm/s^2), converted to metres,
     *         m/s and m/s^2.  Empty when none decoded.  Valid until the next @c parseFrame or
     *         @c clear().
     */
    const std::vector<SsrClockCorrection>&    getSsrClocks()         const { return ssr_clocks_; }
    /**
     * @brief SSR combined orbit-and-clock corrections: legacy RTCM 1060 (GPS), 1066 (GLONASS),
     *        1243 (Galileo), 1249 (QZSS), 1255 (SBAS), 1261 (BeiDou) and the IGS-SSR 4076
     *        combined sub-types.
     * @return One record per satellite in decode order, carrying the orbit fields of
     *         @c getSsrOrbits and the clock polynomial of @c getSsrClocks under one IOD-SSR.
     *         Empty when none decoded.  Valid until the next @c parseFrame or @c clear().
     */
    const std::vector<SsrCombinedCorrection>& getSsrCombined()       const { return ssr_combined_; }
    /**
     * @brief SSR code biases: legacy RTCM 1059 (GPS), 1065 (GLONASS), 1242 (Galileo), 1248 (QZSS),
     *        1254 (SBAS), 1260 (BeiDou) and the IGS-SSR 4076 code-bias sub-types.
     * @return One record per satellite in decode order, holding a bias per signal keyed by RINEX 3
     *         code ("C1C", ...), in metres (14-bit field, 0.01 m per count).  The decoder negates
     *         the wire value, so the stored bias is the one present in the pseudorange.  Empty
     *         when none decoded.  Valid until the next @c parseFrame or @c clear().
     */
    const std::vector<SsrCodeBias>&           getSsrCodeBiases()     const { return ssr_code_biases_; }
    /**
     * @brief SSR phase biases: legacy RTCM 1265 (GPS), 1266 (GLONASS), 1267 (Galileo), 1268 (QZSS),
     *        1269 (SBAS), 1270 (BeiDou) and the IGS-SSR 4076 phase-bias sub-types.
     * @return One record per satellite in decode order, holding a bias per signal keyed by RINEX 3
     *         code ("L1C", ...), in metres (20-bit field, 0.1 mm per count, wire value negated),
     *         with the integer and discontinuity indicators and the satellite yaw angle (9 bits,
     *         180/256 degree per count) and yaw rate (8 bits, 180/8192 degree/s per count).
     *         Empty when none decoded.  Valid until the next @c parseFrame or @c clear().
     */
    const std::vector<SsrPhaseBias>&          getSsrPhaseBiases()    const { return ssr_phase_biases_; }
    /**
     * @brief SSR user range accuracy: legacy RTCM 1061 (GPS), 1067 (GLONASS), 1244 (Galileo),
     *        1250 (QZSS), 1256 (SBAS), 1262 (BeiDou) and the IGS-SSR 4076 URA sub-types.
     * @return One record per satellite in decode order, with the 6-bit URA index decoded to a
     *         1-sigma value in metres (0 for index 0).  Empty when none decoded.  Valid until the
     *         next @c parseFrame or @c clear().
     */
    const std::vector<SsrUra>&                getSsrUras()           const { return ssr_uras_; }
    /**
     * @brief SSR high-rate clock corrections: legacy RTCM 1062 (GPS), 1068 (GLONASS), 1245 (Galileo),
     *        1251 (QZSS), 1257 (SBAS), 1263 (BeiDou) and the IGS-SSR 4076 high-rate sub-types.
     * @return One record per satellite in decode order, with the additive clock delta in metres
     *         (22-bit field, 0.1 mm per count).  Empty when none decoded.  Valid until the next
     *         @c parseFrame or @c clear().
     */
    const std::vector<SsrHighRateClock>&      getSsrHighRateClocks() const { return ssr_hrclocks_; }
    /**
     * @brief Reference-station coordinates from RTCM 1005 and 1006.
     * @return One record per decoded message: station id, antenna reference point in ECEF metres
     *         (three 38-bit signed fields, 0.0001 m per count) and, for 1006 only, the antenna
     *         height (16-bit, 0.0001 m per count).  Empty when none decoded.  Valid until the
     *         next @c parseFrame or @c clear().
     */
    const std::vector<RtcmStationCoords>&     getStationCoords()     const { return station_coords_; }
    /**
     * @brief Broadcast ephemerides from RTCM 1019 (GPS), 1020 (GLONASS), 1042 (BeiDou),
     *        1044 (QZSS) and 1046 (Galileo I/NAV).
     * @return One record per decoded message, in decode order.  A GLONASS 1020 record that arrived
     *         before the stream time was known is not listed until the frame that sets the
     *         tracker dates it.  Empty when none decoded.  Valid until the next @c parseFrame or
     *         @c clear().
     */
    const std::vector<BroadcastEphemeris>&    getBroadcastEphemeris() const { return broadcast_eph_; }
    /// @brief Decoded 1033 receiver and antenna descriptor blocks (RTB-2(d)).  Exposed for
    /// a future SIF (station-info) cross-check; not consumed anywhere yet.
    /// @return One record per decoded 1033 message: station id, antenna descriptor and serial,
    ///         setup id, and receiver type, firmware and serial strings.  Empty when none decoded.
    ///         Valid until the next @c parseFrame or @c clear().
    const std::vector<RtcmReceiverAntennaDescriptor>& getReceiverDescriptors() const { return receiver_descriptors_; }
    /// @brief Decoded 1230 GLONASS code-phase bias blocks (RTB-2(d)).  Exposed only; not
    /// wired into the GLONASS IFB path (see RtcmGlonassBias).
    /// @return One record per decoded 1230 message: station id, alignment indicator, signal mask
    ///         and the L1 C/A, L1 P, L2 C/A and L2 P biases in metres (16-bit fields, 0.02 m per
    ///         count).  Empty when none decoded.  Valid until the next @c parseFrame or @c clear().
    const std::vector<RtcmGlonassBias>&       getGlonassBiases()     const { return glonass_biases_; }

    /// @brief The message type of the most recently decoded frame, or -1 if none.
    /// @return The 12-bit DF002 message number of the last frame whose header parsed, whether or
    ///         not its body decoded; -1 before any frame and after @c clear().
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
     *
     * @return Continuous GPS seconds (week * 604800 + time of week) of the tracker; negative
     *         until the first time-bearing frame has been parsed.
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
     *
     * @return Reference to the parser's stored header (SSR epoch time, update-interval index,
     *         IOD-SSR, provider and solution ids, system); it changes with the next SSR frame.
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

    /// Update-interval indicator last logged per SSR message kind, keyed by
    /// (message type, IGS-SSR message number; 0 for legacy SSR).  The interval
    /// sets each correction's reference time, so it is logged at info when a
    /// kind is first seen and whenever it changes.
    std::map<std::pair<int, int>, int> ssr_update_interval_logged_;

    /// Log the update interval of a decoded SSR message on first sight of its
    /// kind and on any change (see @c ssr_update_interval_logged_).
    void noteSsrUpdateInterval(int msg_type, const char* body, int igs_message_number,
                               const SsrHeader& header);

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
