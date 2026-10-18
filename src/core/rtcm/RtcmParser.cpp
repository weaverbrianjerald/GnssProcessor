/**
 * @file RtcmParser.cpp
 * @brief Top-level RTCM 3.x dispatcher.
 *
 * Frame structure (RTCM 10403.x sec. 4.1):
 *   - byte 0       : preamble 0xD3
 *   - bytes 1-2    : 6 reserved bits + 10-bit payload length (big-endian)
 *   - bytes 3..N+2 : payload of N bytes
 *   - bytes N+3..  : 24-bit CRC-24Q over (header + payload)
 *
 * The 12-bit message-type field DF002 is the first 12 bits of the payload
 * (bytes 3-4), not part of the 3-byte header.  CRC verification is done by
 * RtcmStreamReader before frames reach this parser, so this layer assumes
 * frame integrity.
 */

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmSsrDecoder.hpp"
#include "rtcm/RtcmMsmDecoder.hpp"
#include "rtcm/RtcmNavDecoder.hpp"
#include "rtcm/RtcmBitReader.hpp"
#include "math/TimeUtils.hpp"
#include "logging/SafeLogger.hpp"
#include <chrono>
#include <fstream>

namespace GnssProcessor {

namespace {

/// Per-call logger for the RTCM parse path.
///
/// Replaces the bare spdlog::warn()/info()/trace() free functions, which
/// dereference a raw borrowed pointer into the registry's default-logger slot
/// and therefore fault once spdlog::drop_all() (LogManager::shutdown()) has
/// emptied it.  Routing is unchanged; see logging/SafeLogger.hpp.
std::shared_ptr<spdlog::logger> rtcmLog() {
    return Logging::safeDefaultLogger();
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

bool isMsm5Type(uint16_t mt) {
    // 1075 GPS, 1085 GLO, 1095 GAL, 1105 SBAS, 1115 QZS, 1125 BDS, 1135 NavIC.
    if (mt < 1071 || mt > 1137) return false;
    return ((mt - 1071) % 10) == 4;  // MSM5 sits at offset 4 within each per-system block.
}

bool isMsm4Type(uint16_t mt) {
    // 1074 GPS, 1084 GLO, 1094 GAL, 1104 SBAS, 1114 QZS, 1124 BDS, 1134 NavIC.
    if (mt < 1071 || mt > 1137) return false;
    return ((mt - 1071) % 10) == 3;  // MSM4 sits at offset 3 within each per-system block.
}

// Framing-integrity check that the MSM decoder's ok() flag does not provide.  The
// bit reader flags only over-reads; a structurally wrong decode (wrong MSM subtype
// width table, a field offset off by N bits, a misframed payload) can under-consume
// the payload, so the per-cell block is read from the wrong bits, and still report
// ok()==true because the running offset never passed the end.  A correct MSM frame
// consumes every defined field; the only allowed slack is the 0-7 trailing bits that
// pad the message to a byte boundary.  payload_bits - bits_consumed >= 8 therefore
// means the decode is misaligned, which can produce plausible but wrong
// observations (for example an L1/L2 split that passes ok()).  The warning keeps
// such a decode from being taken for clean data.
void warnIfUnderConsumed(uint16_t mt, const char* label, std::size_t payload_bytes,
                         std::size_t bits_consumed, bool ok) {
    if (!ok) return;  // truncation already reported by the caller
    const std::size_t payload_bits = payload_bytes * 8u;
    if (payload_bits >= bits_consumed + 8u) {
        rtcmLog()->warn("RTCM {}: {} under-consumed payload — read {} of {} bits "
                        "({} unread; MSM subtype / field-width / framing mismatch?)",
                        mt, label, bits_consumed, payload_bits,
                        payload_bits - bits_consumed);
    }
}

} // namespace

RtcmParser::RtcmParser(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

// ---------------------------------------------------------------------------
// Per-parser stream-time tracker (nearest-continuation promotion; RTB-1(a)).
// ---------------------------------------------------------------------------

double RtcmParser::anchorGpsSeconds() {
    if (time_anchor_gps_s_ > 0.0) return time_anchor_gps_s_;
    // Cold start: the only host-clock consultation on the promotion path.
    // TimeUtils::nowContinuousGpsSeconds honours the realtime.leap_seconds
    // override (RTB-1(c)).
    const double now = TimeUtils::nowContinuousGpsSeconds();
    if (now > 0.0) time_anchor_gps_s_ = now;
    return now;
}

void RtcmParser::updateTimeAnchor(const GnssTime& epoch) {
    // Only epochs with a resolved week advance the tracker: a week-0 epoch is
    // the decoder's "no anchor available" degenerate output, and adopting it
    // would re-cold-start every subsequent frame at January 1980.
    if (epoch.week > 0) {
        time_anchor_gps_s_ = TimeUtils::continuousGpsSeconds(epoch);
        time_anchor_from_stream_ = true;
        releaseDeferredGlonass(time_anchor_gps_s_);
    }
}

bool RtcmParser::deferGlonassEphemeris(const uint8_t* payload, std::size_t len) {
    // Decoded without an anchor, so the payload is validated now and the record
    // keeps its undated time of day.
    std::vector<BroadcastEphemeris> decoded;
    const auto res = RtcmNav::decodeBroadcast(payload, len, 1020, 0.0, decoded);
    if (!res.ok || decoded.empty()) {
        rtcmLog()->warn("RTCM 1020: broadcast-ephemeris decode incomplete (truncated payload?)");
        return false;
    }
    if (deferred_glonass_.size() >= kMaxDeferredGlonass) {
        // No time-bearing frame in a whole broadcast cycle: this input carries
        // navigation messages only, so the host clock is the only date available.
        rtcmLog()->debug("RTCM 1020: {} records held without a stream epoch; dating them "
                         "from the host clock", deferred_glonass_.size());
        releaseDeferredGlonass(TimeUtils::nowContinuousGpsSeconds());
    }
    deferred_glonass_.push_back(DeferredGlonass{std::move(decoded.front()), res.glonass_tod_s});
    return true;
}

void RtcmParser::releaseDeferredGlonass(double anchor_gps_seconds) {
    for (auto& held : deferred_glonass_) {
        held.eph.glonass.toe = TimeUtils::glonassTodToGpsTime(held.tod_s, anchor_gps_seconds);
        broadcast_eph_.push_back(std::move(held.eph));
    }
    deferred_glonass_.clear();
}

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
        rtcmLog()->trace("RTCM parseHeader failed (len={})", len);
        return false;
    }
    last_msg_type_ = static_cast<int>(msg_type);

    const uint8_t* payload = frame + 3;
    const std::size_t pl   = static_cast<std::size_t>(payload_len);

    if (msg_type == 1005 || msg_type == 1006) {
        return decodeStationCoords(payload, pl, msg_type);
    }
    if (msg_type == 1033) {
        return decodeReceiverAntennaDescriptor(payload, pl);
    }
    if (msg_type == 1230) {
        return decodeGlonassBiases(payload, pl);
    }
    if (msg_type == 4076) {
        return decodeIgsSsr4076(payload, pl);
    }
    if (isLegacySsrType(msg_type)) {
        return decodeLegacySsr(payload, pl, msg_type);
    }
    if (RtcmNav::isBroadcastEphemerisType(msg_type)) {
        // A GLONASS reference time is an undated time of day, so it needs the
        // stream epoch itself, not only a week hint: until an MSM or SSR frame
        // has set the tracker the record is held and dated later.
        if (msg_type == 1020 && !time_anchor_from_stream_) {
            return deferGlonassEphemeris(payload, pl);
        }
        // The truncated broadcast week fields (10-bit GPS WN etc.) only need a
        // week-scale hint, and the GLONASS time of day a day-scale one; both come
        // from the stream-time tracker.  Nav content epochs (toe/toc, hours from
        // "now") do not advance the tracker; only the per-second MSM/SSR stream
        // epochs do.
        const double anchor = anchorGpsSeconds();
        const auto res = RtcmNav::decodeBroadcast(payload, pl, msg_type, anchor, broadcast_eph_);
        if (!res.ok) {
            rtcmLog()->warn("RTCM {}: broadcast-ephemeris decode incomplete (truncated payload?)", msg_type);
        }
        return res.ok;
    }
    if (isMsm7Type(msg_type)) {
        const auto res = RtcmMsm::decodeMsm7(payload, pl, msg_type, anchorGpsSeconds(), observations_);
        if (!res.ok) {
            rtcmLog()->warn("RTCM {}: MSM7 decode incomplete (truncated payload?)", msg_type);
        }
        warnIfUnderConsumed(msg_type, "MSM7", pl, res.bits_consumed, res.ok);
        if (res.ok) updateTimeAnchor(res.epoch_time);
        return res.ok;
    }
    if (isMsm5Type(msg_type)) {
        const auto res = RtcmMsm::decodeMsm5(payload, pl, msg_type, anchorGpsSeconds(), observations_);
        if (!res.ok) {
            rtcmLog()->warn("RTCM {}: MSM5 decode incomplete (truncated payload?)", msg_type);
        }
        warnIfUnderConsumed(msg_type, "MSM5", pl, res.bits_consumed, res.ok);
        if (res.ok) updateTimeAnchor(res.epoch_time);
        return res.ok;
    }
    if (isMsm4Type(msg_type)) {
        const auto res = RtcmMsm::decodeMsm4(payload, pl, msg_type, anchorGpsSeconds(), observations_);
        if (!res.ok) {
            rtcmLog()->warn("RTCM {}: MSM4 decode incomplete (truncated payload?)", msg_type);
        }
        warnIfUnderConsumed(msg_type, "MSM4", pl, res.bits_consumed, res.ok);
        if (res.ok) updateTimeAnchor(res.epoch_time);
        return res.ok;
    }

    rtcmLog()->trace("RTCM message type {} not yet decoded", msg_type);
    return false;
}

bool RtcmParser::decodeStationCoords(const uint8_t* payload, std::size_t len, uint16_t msg_type) {
    RtcmStationCoords s;
    if (!RtcmSsr::decodeStationCoords(payload, len, msg_type, s)) {
        rtcmLog()->warn("RTCM {}: station coordinates decode failed", msg_type);
        return false;
    }
    station_coords_.push_back(std::move(s));
    return true;
}

bool RtcmParser::decodeLegacySsr(const uint8_t* payload, std::size_t len, uint16_t msg_type) {
    const auto result = RtcmSsr::decodeLegacy(
        payload, len, msg_type, anchorGpsSeconds(),
        ssr_orbits_, ssr_clocks_, ssr_combined_, ssr_code_biases_,
        ssr_phase_biases_, ssr_uras_, ssr_hrclocks_);
    last_ssr_header_ = result.header;
    if (!result.ok) {
        rtcmLog()->warn("RTCM {}: legacy SSR decode incomplete (truncated payload?)", msg_type);
    }
    if (result.ok) updateTimeAnchor(result.header.epoch_time);
    return result.ok;
}

bool RtcmParser::decodeIgsSsr4076(const uint8_t* payload, std::size_t len) {
    const auto result = RtcmSsr::decodeIgs4076(
        payload, len, anchorGpsSeconds(),
        ssr_orbits_, ssr_clocks_, ssr_combined_, ssr_code_biases_,
        ssr_phase_biases_, ssr_uras_, ssr_hrclocks_);
    last_ssr_header_ = result.header;
    if (!result.ok) {
        rtcmLog()->trace("RTCM 4076: IGS-SSR decode incomplete or unsupported sub-type");
    }
    if (result.ok) updateTimeAnchor(result.header.epoch_time);
    return result.ok;
}

// ---------------------------------------------------------------------------
// RTCM 1033: receiver + antenna descriptors (RTB-2(d)).
//
// Layout (RTCM 10403.3 sec. 3.5.9): five counted ASCII strings + the antenna
// setup id, all 8-bit fields.  Decoded into RtcmReceiverAntennaDescriptor and
// exposed on the parser output; logged once per parser at info.  Kept for a
// future SIF (station-info) cross-check and not consumed elsewhere.
// ---------------------------------------------------------------------------
bool RtcmParser::decodeReceiverAntennaDescriptor(const uint8_t* payload, std::size_t len) {
    RtcmBitReader r(payload, len);
    r.skip(12);                                   // DF002 message type
    RtcmReceiverAntennaDescriptor d;
    d.reference_station_id = static_cast<int>(r.u(12));   // DF003

    // Counted ASCII string: 8-bit count N (DF029/DF032/DF227/DF229/DF231)
    // followed by N 8-bit characters.  Non-printable bytes are replaced with
    // '.' so a malformed frame cannot inject control bytes into the logs.
    auto readString = [&r](std::string& out) {
        const int n = static_cast<int>(r.u(8));
        out.clear();
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto c = static_cast<char>(r.u(8));
            out.push_back((c >= 0x20 && c < 0x7f) ? c : '.');
        }
    };

    readString(d.antenna_descriptor);             // DF029 + DF030
    d.antenna_setup_id = static_cast<int>(r.u(8)); // DF031
    readString(d.antenna_serial);                 // DF032 + DF033
    readString(d.receiver_type);                  // DF227 + DF228
    readString(d.receiver_firmware);              // DF229 + DF230
    readString(d.receiver_serial);                // DF231 + DF232
    if (!r.ok()) {
        rtcmLog()->warn("RTCM 1033: receiver/antenna descriptor decode incomplete (truncated payload?)");
        return false;
    }

    if (!logged_1033_) {
        logged_1033_ = true;
        rtcmLog()->info("RTCM 1033: station {} antenna '{}' (setup {}, s/n '{}'), "
                        "receiver '{}' fw '{}' s/n '{}'",
                        d.reference_station_id, d.antenna_descriptor, d.antenna_setup_id,
                        d.antenna_serial, d.receiver_type, d.receiver_firmware,
                        d.receiver_serial);
    }
    receiver_descriptors_.push_back(std::move(d));
    return true;
}

// ---------------------------------------------------------------------------
// RTCM 1230: GLONASS L1/L2 code-phase biases (RTB-2(d)).
//
// Layout (RTCM 10403.3 sec. 3.5.15): DF003 station id (12), DF421 code-phase
// bias indicator (1), 3 reserved bits, DF422 FDMA signals mask (4, MSB = L1
// C/A … LSB = L2 P), then one signed 16-bit bias (0.02 m units, DF423-DF426)
// per set mask bit, in mask order.  Exposed and logged once only; not wired
// into the GLONASS IFB estimation (see RtcmGlonassBias; wiring it is an
// RTF-scale change).
// ---------------------------------------------------------------------------
bool RtcmParser::decodeGlonassBiases(const uint8_t* payload, std::size_t len) {
    RtcmBitReader r(payload, len);
    r.skip(12);                                   // DF002 message type
    RtcmGlonassBias b;
    b.reference_station_id      = static_cast<int>(r.u(12));  // DF003
    b.code_phase_bias_indicator = r.boolean();                // DF421
    r.skip(3);                                                // reserved
    b.fdma_signal_mask          = static_cast<int>(r.u(4));   // DF422

    double* const slots[4] = { &b.l1_ca_bias_m, &b.l1_p_bias_m,
                               &b.l2_ca_bias_m, &b.l2_p_bias_m };
    for (int bit = 0; bit < 4; ++bit) {
        if (b.fdma_signal_mask & (1 << (3 - bit))) {          // MSB-first mask order
            *slots[bit] = r.s(16) * 0.02;                     // DF423..DF426, 0.02 m
        }
    }
    if (!r.ok()) {
        rtcmLog()->warn("RTCM 1230: GLONASS code-phase bias decode incomplete (truncated payload?)");
        return false;
    }

    if (!logged_1230_) {
        logged_1230_ = true;
        rtcmLog()->info("RTCM 1230: station {} GLONASS code-phase biases (aligned={}, mask=0x{:X}): "
                        "L1CA={:.2f} L1P={:.2f} L2CA={:.2f} L2P={:.2f} m "
                        "(surfaced only; not consumed by the IFB path)",
                        b.reference_station_id, b.code_phase_bias_indicator,
                        b.fdma_signal_mask, b.l1_ca_bias_m, b.l1_p_bias_m,
                        b.l2_ca_bias_m, b.l2_p_bias_m);
    }
    glonass_biases_.push_back(b);
    return true;
}

bool RtcmParser::parse() {
    std::ifstream stream(file_path_, std::ios::binary);
    if (!stream.is_open()) {
        rtcmLog()->error("RtcmParser: cannot open {}", file_path_.string());
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

    rtcmLog()->info("RtcmParser: {} bytes -> {} decoded frames "
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
    receiver_descriptors_.clear();
    glonass_biases_.clear();
    // time_anchor_gps_s_, last_ssr_header_ and the 1033/1230 log-once flags
    // survive clear(): the tracker carries the stream-time continuity
    // (RTB-1(a)), and clear() runs once per frame on the reused engine parser.
}

} // namespace GnssProcessor
