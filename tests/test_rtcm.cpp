/**
 * @file test_rtcm.cpp
 * @brief Unit tests for the RTCM 3.x decoding foundation and SSR overlay.
 *
 * Coverage:
 *   - RtcmBitReader: unsigned, two's-complement signed, sign-magnitude;
 *     MSB-first bit ordering; over-read detection.
 *   - RtcmSignalMap: known per-system signal-ID values; round-trip.
 *   - RtcmParser::parseHeader: payload-length and DF002 message-type
 *     extraction with the correct bit layout.
 *   - End-to-end legacy SSR decode (RTCM 1058 GPS clock + RTCM 1005
 *     station ARP) using hand-built frames.
 *   - SsrCorrectionStore: push/get round-trip, staleness rejection,
 *     RAC-to-ECEF orbit-correction transform, clock-bias polynomial.
 *
 * The synthetic frames carry zero CRC bytes; this layer assumes CRC is
 * verified by RtcmStreamReader before frames reach the parser, so the
 * three trailing bytes are not inspected.
 */

#include <gtest/gtest.h>

#include "rtcm/RtcmBitReader.hpp"
#include "rtcm/RtcmSignalMap.hpp"
#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmTypes.hpp"
#include "rtcm/RtcmMsmDecoder.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GnssProcessor {
namespace {

// ============================================================================
// Local helper: MSB-first bit writer used to hand-construct RTCM payloads.
// ============================================================================
class BitWriter {
public:
    void put(int n, uint64_t value) {
        for (int i = n - 1; i >= 0; --i) {
            const bool bit = ((value >> i) & 0x1ull) != 0;
            if ((bit_count_ & 7u) == 0) bytes_.push_back(0u);
            if (bit) bytes_.back() |= static_cast<uint8_t>(1u << (7 - (bit_count_ & 7u)));
            ++bit_count_;
        }
    }
    void putSigned(int n, int64_t value) {
        const uint64_t mask = (n == 64) ? ~0ull : ((1ull << n) - 1ull);
        put(n, static_cast<uint64_t>(value) & mask);
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::size_t bitCount() const { return bit_count_; }
private:
    std::vector<uint8_t> bytes_;
    std::size_t          bit_count_{0};
};

// Build a complete RTCM3 frame: 0xD3 | length(10) | payload | CRC(0,0,0).
std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(3 + payload.size() + 3);
    const auto pl = static_cast<uint16_t>(payload.size());
    frame.push_back(0xD3);
    frame.push_back(static_cast<uint8_t>((pl >> 8) & 0x03u));
    frame.push_back(static_cast<uint8_t>(pl & 0xFFu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0); frame.push_back(0); frame.push_back(0); // CRC, unchecked
    return frame;
}

// ============================================================================
// RtcmBitReader
// ============================================================================
TEST(RtcmBitReaderTest, UnsignedReadsMsbFirst) {
    // Bytes 1010 1010 0101 0101 → first 4 bits = 0xA, next 4 = 0xA, etc.
    const uint8_t buf[] = {0xAAu, 0x55u};
    RtcmBitReader r(buf, sizeof(buf));
    EXPECT_EQ(r.u(4), 0xAu);
    EXPECT_EQ(r.u(4), 0xAu);
    EXPECT_EQ(r.u(4), 0x5u);
    EXPECT_EQ(r.u(4), 0x5u);
    EXPECT_TRUE(r.ok());
}

TEST(RtcmBitReaderTest, UnsignedAcrossByteBoundary) {
    const uint8_t buf[] = {0xFFu, 0x80u}; // 1111 1111 1000 0000
    RtcmBitReader r(buf, sizeof(buf));
    EXPECT_EQ(r.u(9), 0x1FFu);            // top 9 bits = all ones
    EXPECT_EQ(r.u(7), 0u);                // remaining 7 bits = 0
    EXPECT_TRUE(r.ok());
}

TEST(RtcmBitReaderTest, TwosComplementSigned) {
    // 22-bit signed pattern for -1 = 0x3FFFFF, padded with zeros after.
    BitWriter w; w.putSigned(22, -1); w.put(2, 0); // 24 bits total
    const auto& b = w.bytes();
    RtcmBitReader r(b.data(), b.size());
    EXPECT_EQ(r.s(22), -1);
}

TEST(RtcmBitReaderTest, TwosComplementSignedPositive) {
    BitWriter w; w.putSigned(22, 100); w.put(2, 0);
    const auto& b = w.bytes();
    RtcmBitReader r(b.data(), b.size());
    EXPECT_EQ(r.s(22), 100);
}

TEST(RtcmBitReaderTest, SignMagnitude) {
    // 5-bit sign-magnitude: sign(1) + magnitude(4).  -3 = 1 0011.
    BitWriter w; w.put(1, 1); w.put(4, 3); w.put(3, 0); // pad to byte
    const auto& b = w.bytes();
    RtcmBitReader r(b.data(), b.size());
    EXPECT_EQ(r.sm(5), -3);
}

TEST(RtcmBitReaderTest, OverrunFlagsError) {
    const uint8_t buf[] = {0xFFu};
    RtcmBitReader r(buf, sizeof(buf));
    EXPECT_EQ(r.u(8), 0xFFu);
    EXPECT_TRUE(r.ok());
    (void)r.u(1);                         // 9th bit, only 8 available
    EXPECT_FALSE(r.ok());
}

TEST(RtcmBitReaderTest, U64Wide) {
    BitWriter w; w.put(40, 0x1122334455ull); w.put(0, 0);
    const auto& b = w.bytes();
    // Pad to a full byte.
    auto padded = b; while ((padded.size() * 8) < 40) padded.push_back(0);
    RtcmBitReader r(padded.data(), padded.size());
    EXPECT_EQ(r.u64(40), 0x1122334455ull);
}

// ============================================================================
// RtcmSignalMap
// ============================================================================
TEST(RtcmSignalMapTest, GpsKnownEntries) {
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::GPS, 2),  "1C");
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::GPS, 10), "2W");
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::GPS, 24), "5X");
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::GPS, 0),  "");   // reserved
}

TEST(RtcmSignalMapTest, GalileoKnownEntries) {
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::Galileo, 2),  "1C");
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::Galileo, 24), "5X");
    EXPECT_EQ(RtcmSignalMap::toRinexSuffix(System::Galileo, 14), "7I");
}

TEST(RtcmSignalMapTest, RoundTrip) {
    for (int sig = 1; sig < 32; ++sig) {
        const auto suffix = RtcmSignalMap::toRinexSuffix(System::GPS, sig);
        if (suffix.empty()) continue;
        EXPECT_EQ(RtcmSignalMap::fromRinexSuffix(System::GPS, suffix), sig)
            << "round-trip failed for GPS signal " << sig << " -> " << suffix;
    }
}

// ============================================================================
// RtcmParser::parseHeader
// ============================================================================
TEST(RtcmParserHeaderTest, LengthAndMessageType) {
    // Build a frame whose payload starts with DF002 = 1058.
    BitWriter pw;
    pw.put(12, 1058);                     // DF002
    pw.put(20, 0);                        // pad rest of payload (won't be inspected)
    while (pw.bitCount() < 18 * 8) pw.put(8, 0);
    const auto frame = buildFrame(pw.bytes());

    uint16_t plen = 0, mt = 0;
    EXPECT_TRUE(RtcmParser::parseHeader(frame.data(), frame.size(), plen, mt));
    EXPECT_EQ(plen, pw.bytes().size());
    EXPECT_EQ(mt,   1058);
}

TEST(RtcmParserHeaderTest, RejectsBadPreamble) {
    std::vector<uint8_t> frame{0xAB, 0x00, 0x05, 1, 2, 3, 4, 5, 0, 0, 0};
    uint16_t plen = 0, mt = 0;
    EXPECT_FALSE(RtcmParser::parseHeader(frame.data(), frame.size(), plen, mt));
}

TEST(RtcmParserHeaderTest, RejectsTooShort) {
    std::vector<uint8_t> frame{0xD3, 0x00};
    uint16_t plen = 0, mt = 0;
    EXPECT_FALSE(RtcmParser::parseHeader(frame.data(), frame.size(), plen, mt));
}

// ============================================================================
// RtcmParser end-to-end: GPS clock SSR (1058) and station coords (1005)
// ============================================================================
TEST(RtcmParserDecodeTest, GpsClockSsr1058) {
    // Build a 1058 message with a single GPS satellite (G05, C0=0.01 m).
    BitWriter pw;
    pw.put(12, 1058);                     // DF002 message type
    pw.put(20, 100);                      // DF385 GPS epoch (TOW seconds, mod week)
    pw.put(4,  0);                        // DF391 update interval index
    pw.put(1,  0);                        // DF388 multi-message
    pw.put(4,  5);                        // DF413 IOD-SSR
    pw.put(16, 0);                        // DF414 provider id
    pw.put(4,  0);                        // DF415 solution id
    pw.put(6,  1);                        // DF387 num satellites
    // Per-satellite body
    pw.put(6,  5);                        // DF068 GPS sat id (G05)
    pw.putSigned(22, 100);                // DF376 C0 = 100 * 1e-4 = 0.01 m
    pw.putSigned(21, 0);                  // DF377 C1 = 0
    pw.putSigned(27, 0);                  // DF378 C2 = 0
    while (pw.bitCount() % 8) pw.put(1, 0);   // pad to byte boundary

    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1058);
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);
    const auto& c = parser.getSsrClocks().front();
    EXPECT_EQ(c.sat, "G05");
    EXPECT_NEAR(c.c0, 0.01, 1e-9);
    EXPECT_DOUBLE_EQ(c.c1, 0.0);
    EXPECT_DOUBLE_EQ(c.c2, 0.0);
    EXPECT_EQ(c.iod_ssr, 5);
}

TEST(RtcmParserDecodeTest, StationCoords1005) {
    // ECEF position (X=-2700000, Y=-4300000, Z=3850000) m.
    const int64_t X_RAW = static_cast<int64_t>(-2700000.0 / 1e-4); // 1e-4 m units
    const int64_t Y_RAW = static_cast<int64_t>(-4300000.0 / 1e-4);
    const int64_t Z_RAW = static_cast<int64_t>( 3850000.0 / 1e-4);

    BitWriter pw;
    pw.put(12, 1005);                     // DF002
    pw.put(12, 3210);                     // DF003 station id
    pw.put(6,  0);                        // ITRF year
    pw.put(1,  1); pw.put(1, 1); pw.put(1, 0); pw.put(1, 0); // GPS/GLO/Gal/Ref-station bits
    pw.putSigned(38, X_RAW);
    pw.put(1, 0); pw.put(1, 0);           // Single-receiver osc + reserved
    pw.putSigned(38, Y_RAW);
    pw.put(2, 0);                         // Quarter-cycle indicator
    pw.putSigned(38, Z_RAW);
    while (pw.bitCount() % 8) pw.put(1, 0);

    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1005);
    ASSERT_EQ(parser.getStationCoords().size(), 1u);
    const auto& s = parser.getStationCoords().front();
    EXPECT_EQ(s.reference_station_id, 3210);
    EXPECT_NEAR(s.arp_ecef.x(), -2700000.0, 1e-3);
    EXPECT_NEAR(s.arp_ecef.y(), -4300000.0, 1e-3);
    EXPECT_NEAR(s.arp_ecef.z(),  3850000.0, 1e-3);
    EXPECT_FALSE(s.has_height);
}

// ============================================================================
// SsrCorrectionStore — push/get + staleness
// ============================================================================
TEST(SsrCorrectionStoreTest, PushAndQuery) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();

    SsrClockCorrection clk;
    clk.sat            = "G07";
    clk.c0             = 0.05;
    clk.c1             = 1e-4;
    clk.c2             = 0.0;
    clk.received_time  = GnssTime{2300, 100.0};
    store.pushClock(clk, SsrHeader{});

    auto q = store.getOrbitClock("G07", GnssTime{2300, 110.0}, 60.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_FALSE(q->has_orbit);
    EXPECT_NEAR(q->c0, 0.05, 1e-12);
    EXPECT_NEAR(q->c1, 1e-4, 1e-12);

    store.clear();
}

TEST(SsrCorrectionStoreTest, StalenessRejected) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();

    SsrClockCorrection clk;
    clk.sat           = "G07";
    clk.c0            = 0.05;
    clk.received_time = GnssTime{2300, 100.0};
    store.pushClock(clk, SsrHeader{});

    auto q = store.getOrbitClock("G07", GnssTime{2300, 200.0}, /*max_age_s=*/30.0);
    EXPECT_FALSE(q.has_value());

    store.clear();
}

// ============================================================================
// Apply helpers — RAC frame + clock polynomial
// ============================================================================
TEST(SsrApplyTest, RadialOnlyTransform) {
    // Sat at +X axis at GEO altitude, moving in +Y; radial frame:
    //   e_along  = +Y, e_cross = (+X × +Y) / |...| = +Z, e_radial = +Y × +Z = +X.
    // A purely-radial RAC delta (1, 0, 0) m should reduce ECEF X by 1 m.
    const Eigen::Vector3d pos{42164e3, 0.0, 0.0};
    const Eigen::Vector3d vel{0.0, 3074.0, 0.0};
    const Eigen::Vector3d delta_rac{1.0, 0.0, 0.0};
    const Eigen::Vector3d delta_dot{0.0, 0.0, 0.0};

    const auto out = SsrCorrectionStore::applyOrbitToPosition(pos, vel, delta_rac, delta_dot, 0.0);
    EXPECT_NEAR(out.x(), 42164e3 - 1.0, 1e-6);
    EXPECT_NEAR(out.y(), 0.0,           1e-6);
    EXPECT_NEAR(out.z(), 0.0,           1e-6);
}

TEST(SsrApplyTest, AlongTrackOnlyTransform) {
    // Same geometry; along-track delta of 1 m should reduce ECEF Y by 1 m
    // (e_along is +Y, applied with subtraction).
    const Eigen::Vector3d pos{42164e3, 0.0, 0.0};
    const Eigen::Vector3d vel{0.0, 3074.0, 0.0};
    const Eigen::Vector3d delta_rac{0.0, 1.0, 0.0};
    const Eigen::Vector3d delta_dot = Eigen::Vector3d::Zero();

    const auto out = SsrCorrectionStore::applyOrbitToPosition(pos, vel, delta_rac, delta_dot, 0.0);
    EXPECT_NEAR(out.y(), 0.0 - 1.0, 1e-6);
}

TEST(SsrApplyTest, RatePropagation) {
    // delta_rac_dot · dt should add into the correction.  Radial dot of
    // 0.1 m/s applied for dt=10s = 1 m radial; expect ECEF X reduces by 1 m.
    const Eigen::Vector3d pos{42164e3, 0.0, 0.0};
    const Eigen::Vector3d vel{0.0, 3074.0, 0.0};
    const Eigen::Vector3d delta_rac     = Eigen::Vector3d::Zero();
    const Eigen::Vector3d delta_rac_dot{0.1, 0.0, 0.0};

    const auto out = SsrCorrectionStore::applyOrbitToPosition(pos, vel, delta_rac, delta_rac_dot, 10.0);
    EXPECT_NEAR(out.x(), 42164e3 - 1.0, 1e-6);
}

TEST(SsrApplyTest, ClockPolynomial) {
    // c0 = 0.03 m, c1 = 1e-3 m/s, c2 = 0, hr = 0, dt = 5 s.
    // Expect (0.03 + 5e-3) m / c.
    const double dt = 5.0;
    const double sec = SsrCorrectionStore::applyClockToBias(0.03, 1e-3, 0.0, 0.0, dt);
    const double expected = (0.03 + 1e-3 * dt) / Constants::SPEED_OF_LIGHT;
    EXPECT_NEAR(sec, expected, 1e-18);
}

TEST(SsrApplyTest, ClockHighRateAdds) {
    // hr clock should add directly in metres before the / c conversion.
    const double sec = SsrCorrectionStore::applyClockToBias(0.0, 0.0, 0.0, 0.02, 0.0);
    EXPECT_NEAR(sec, 0.02 / Constants::SPEED_OF_LIGHT, 1e-18);
}

// ============================================================================
// IODE match semantics — gates the SSR overlay for the realtime PPP-RTK path.
// ============================================================================
TEST(SsrIodeMatchTest, BothUnknownPasses) {
    // Post-processing case: SP3-derived OrbitData carries iode=-1.
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(-1, -1));
}

TEST(SsrIodeMatchTest, OneSideUnknownPasses) {
    // Permissive on either side missing — preserves current behaviour
    // until the broadcast-nav parser populates the broadcast IODE.
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(-1, 42));
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(42, -1));
}

TEST(SsrIodeMatchTest, EqualPasses) {
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(42, 42));
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(0, 0));
}

TEST(SsrIodeMatchTest, MismatchRejects) {
    // The whole point: when both sides are valid and differ, the
    // SSR record must NOT be applied to the (wrong) broadcast baseline.
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(42, 43));
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(0, 1));
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(255, 0));
}

// ============================================================================
// MSM7 — GPS L1C single-satellite single-signal frame
// ============================================================================

// Helper: build the bytes of an MSM7 GPS payload with one satellite (G05)
// and one signal (RTCM signal id 2 → "1C").  Caller chooses field values.
static std::vector<uint8_t> buildMsm7GpsPayload(
    uint32_t epoch_ms,
    int      rough_range_int,
    int      rough_range_mod,
    int      rough_range_rate,
    int      fine_pseudorange,
    int      fine_phaserange,
    int      lock_time,
    int      half_amb,
    int      cnr,
    int      fine_rate)
{
    BitWriter pw;
    pw.put(12, 1077);                          // DF002 GPS MSM7
    pw.put(12, 100);                           // DF003 station id
    pw.put(30, epoch_ms);                      // DF004 GPS epoch ms
    pw.put(1,  0);                             // DF393 multi-message
    pw.put(3,  0);                             // DF409 IODS
    pw.put(7,  0);                             // reserved
    pw.put(2,  0); pw.put(2, 0);               // clock steer + ext clock
    pw.put(1,  0); pw.put(3, 0);               // smoothing flag + interval

    // Sat mask: G05 = bit 4 (0-indexed from MSB) → bit 59 in 64-bit (LSB-indexed).
    pw.put(64, 1ull << (63 - 4));
    // Sig mask: signal id 2 ("1C") = bit 1 (0-indexed from MSB) → bit 30 in 32-bit.
    pw.put(32, 1u << (31 - 1));
    // Cell mask: 1×1 = 1 bit, present.
    pw.put(1, 1);

    // Per-sat block (no GLONASS ext info on GPS).
    pw.put(8,  rough_range_int);
    pw.put(10, rough_range_mod);
    pw.putSigned(14, rough_range_rate);

    // Per-cell block.
    pw.putSigned(20, fine_pseudorange);
    pw.putSigned(24, fine_phaserange);
    pw.put(10, lock_time);
    pw.put(1,  half_amb);
    pw.put(10, cnr);
    pw.putSigned(15, fine_rate);

    while (pw.bitCount() % 8) pw.put(1, 0);    // pad to byte boundary
    return pw.bytes();
}

TEST(RtcmParserDecodeTest, Msm7GpsSingleCellPseudorange) {
    // rough_range_int = 67 ms, everything else zero.
    // Pseudorange = 67e-3 · c m.
    const auto payload = buildMsm7GpsPayload(
        /*epoch_ms=*/100000,
        /*rough_int=*/67, /*rough_mod=*/0, /*rough_rate=*/-100,
        /*fine_ps=*/0,    /*fine_ph=*/0,
        /*lock=*/64, /*half=*/0, /*cnr=*/720, /*fine_rate=*/0);
    const auto frame = buildFrame(payload);

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1077);
    ASSERT_EQ(parser.getObservations().size(), 1u);

    const auto& obs = parser.getObservations().front();
    EXPECT_EQ(obs.satellite, "G05");
    EXPECT_EQ(obs.system, System::GPS);

    const double expected_pr = 67e-3 * Constants::SPEED_OF_LIGHT;
    auto it_c = obs.measurements.find("C1C");
    ASSERT_NE(it_c, obs.measurements.end());
    EXPECT_NEAR(it_c->second, expected_pr, 1e-3);
}

TEST(RtcmParserDecodeTest, Msm7GpsPhaseInCycles) {
    const auto payload = buildMsm7GpsPayload(
        /*epoch_ms=*/100000,
        /*rough_int=*/67, /*rough_mod=*/0, /*rough_rate=*/-100,
        /*fine_ps=*/0,    /*fine_ph=*/0,
        /*lock=*/64, /*half=*/0, /*cnr=*/720, /*fine_rate=*/0);
    const auto frame = buildFrame(payload);

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& obs = parser.getObservations().front();

    const double phase_m  = 67e-3 * Constants::SPEED_OF_LIGHT;
    const double expected = phase_m / Constants::GPS_L1_WAVELENGTH;

    auto it = obs.measurements.find("L1C");
    ASSERT_NE(it, obs.measurements.end());
    EXPECT_NEAR(it->second, expected, 1e-3);
}

TEST(RtcmParserDecodeTest, Msm7GpsSnrFromCnrField) {
    const auto payload = buildMsm7GpsPayload(
        100000, 67, 0, -100, 0, 0, 64, 0, /*cnr=*/720, 0);
    const auto frame = buildFrame(payload);

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& obs = parser.getObservations().front();

    auto it = obs.measurements.find("S1C");
    ASSERT_NE(it, obs.measurements.end());
    EXPECT_NEAR(it->second, 720.0 / 16.0, 1e-9);   // 1/16 dB-Hz units
}

// ============================================================================
// IGS-SSR (RTCM 4076) — IM-based dispatch reuses legacy body decoders
// ============================================================================

// Helper: build the bytes of an IGS-SSR 4076 GPS Clock (IM=12) payload
// with one satellite (G05) and a polynomial (c0, c1=0, c2=0).
static std::vector<uint8_t> buildIgsSsr4076GpsClock(double c0_m, int iod_ssr) {
    BitWriter pw;
    pw.put(12, 4076);                       // DF002 message type
    pw.put(3,  1);                          // IGS-SSR version
    pw.put(8,  12);                         // IM: GPS Clock
    pw.put(20, 100);                        // DF385 epoch ms (= 100 s into week)
    pw.put(4,  0);                          // DF391 update interval
    pw.put(1,  0);                          // DF388 multi-message
    pw.put(4,  iod_ssr);                    // DF413 IOD-SSR
    pw.put(16, 0);                          // DF414 provider id
    pw.put(4,  0);                          // DF415 solution id
    pw.put(6,  1);                          // DF387 num satellites
    pw.put(6,  5);                          // DF068 sat id (G05)
    pw.putSigned(22, static_cast<int>(c0_m / 1e-4));  // DF376 C0 in 1e-4 m units
    pw.putSigned(21, 0);                    // DF377 C1
    pw.putSigned(27, 0);                    // DF378 C2
    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

TEST(RtcmParserDecodeTest, IgsSsr4076GpsClock) {
    const auto frame = buildFrame(buildIgsSsr4076GpsClock(/*c0_m=*/0.025, /*iod=*/7));

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 4076);
    ASSERT_EQ(parser.getSsrClocks().size(), 1u);

    const auto& c = parser.getSsrClocks().front();
    EXPECT_EQ(c.sat, "G05");
    EXPECT_NEAR(c.c0, 0.025, 1e-9);
    EXPECT_DOUBLE_EQ(c.c1, 0.0);
    EXPECT_DOUBLE_EQ(c.c2, 0.0);
    EXPECT_EQ(c.iod_ssr, 7);
}

TEST(RtcmParserDecodeTest, IgsSsr4076GalileoOrbit) {
    // IM = 41 = Galileo Orbit.  Body identical to legacy 1240 orbit
    // (DF068 6-bit sat id + DF071 10-bit IODE + 6×delta fields).
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3, 1);
    pw.put(8, 41);                          // IM = Galileo Orbit
    pw.put(20, 0);                          // epoch
    pw.put(4, 0);                           // update interval
    pw.put(1, 0);                           // multi-msg
    pw.put(1, 0);                           // datum (orbit messages have this)
    pw.put(4, 3);                           // IOD-SSR
    pw.put(16, 0);                          // provider
    pw.put(4, 0);                           // solution
    pw.put(6, 1);                           // num sats
    pw.put(6, 11);                          // sat id (E11)
    pw.put(10, 42);                         // IODE (Galileo: 10 bits)
    pw.putSigned(22, 1500);                 // delta radial = 0.15 m
    pw.putSigned(20, -800);                 // delta along  = -0.32 m
    pw.putSigned(20, 200);                  // delta cross  = 0.08 m
    pw.putSigned(21, 0);                    // delta-dot radial
    pw.putSigned(19, 0);                    // delta-dot along
    pw.putSigned(19, 0);                    // delta-dot cross
    while (pw.bitCount() % 8) pw.put(1, 0);
    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getSsrOrbits().size(), 1u);

    const auto& o = parser.getSsrOrbits().front();
    EXPECT_EQ(o.sat, "E11");
    EXPECT_EQ(o.iode, 42);
    EXPECT_NEAR(o.delta_rac.x(),  1500 * 1e-4, 1e-9);
    EXPECT_NEAR(o.delta_rac.y(),  -800 * 4e-4, 1e-9);
    EXPECT_NEAR(o.delta_rac.z(),   200 * 4e-4, 1e-9);
    EXPECT_EQ(o.iod_ssr, 3);
}

TEST(RtcmParserDecodeTest, IgsSsr4076UnsupportedImReturnsFalse) {
    // IM = 99 — unrecognised; decoder must reject without producing records.
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3, 1);
    pw.put(8, 99);                          // unsupported IM
    while (pw.bitCount() < 8 * 8) pw.put(8, 0);
    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    EXPECT_FALSE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 4076);
    EXPECT_TRUE(parser.getSsrClocks().empty());
    EXPECT_TRUE(parser.getSsrOrbits().empty());
}

// ============================================================================
// MSM4 — same structure as MSM7 but narrower precision, no Doppler
// ============================================================================
TEST(RtcmParserDecodeTest, Msm4GpsSingleCellPseudorange) {
    // Build a 1074 (GPS MSM4) frame with one satellite (G05) on signal 1C.
    // MSM4 has no rough/fine range rate fields.
    BitWriter pw;
    pw.put(12, 1074);                          // DF002 GPS MSM4
    pw.put(12, 100);                           // station id
    pw.put(30, 100000);                        // GPS epoch ms
    pw.put(1, 0);                              // multi-msg
    pw.put(3, 0);                              // IODS
    pw.put(7, 0);                              // reserved
    pw.put(2, 0); pw.put(2, 0);                // clock steer + ext clock
    pw.put(1, 0); pw.put(3, 0);                // smoothing flag + interval

    pw.put(64, 1ull << (63 - 4));              // sat mask: G05
    pw.put(32, 1u  << (31 - 1));               // sig mask: signal id 2 ("1C")
    pw.put(1, 1);                              // cell mask: 1×1 present

    // Per-sat block: rough range int + rough range mod (no rate in MSM4).
    pw.put(8,  67);                            // rough int (ms)
    pw.put(10, 0);                             // rough mod 1 ms

    // Per-cell block: 15+22+4+1+6 = 48 bits per cell, no fine range rate.
    pw.putSigned(15, 0);                       // fine pseudorange = 0
    pw.putSigned(22, 0);                       // fine phase = 0
    pw.put(4, 5);                              // lock time (4-bit basic)
    pw.put(1, 0);                              // half-cycle ambiguity
    pw.put(6, 45);                             // CNR = 45 dB-Hz (whole units)

    while (pw.bitCount() % 8) pw.put(1, 0);
    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1074);
    ASSERT_EQ(parser.getObservations().size(), 1u);

    const auto& obs = parser.getObservations().front();
    EXPECT_EQ(obs.satellite, "G05");

    auto it_c = obs.measurements.find("C1C");
    ASSERT_NE(it_c, obs.measurements.end());
    EXPECT_NEAR(it_c->second, 67e-3 * Constants::SPEED_OF_LIGHT, 1e-3);

    // MSM4 SNR is in whole dB-Hz, not 1/16.
    auto it_s = obs.measurements.find("S1C");
    ASSERT_NE(it_s, obs.measurements.end());
    EXPECT_NEAR(it_s->second, 45.0, 1e-9);

    // No Doppler in MSM4.
    EXPECT_EQ(obs.measurements.find("D1C"), obs.measurements.end());
}

TEST(RtcmParserDecodeTest, Msm7GpsDopplerFromRangeRate) {
    // rough rate = -100 m/s, fine rate = 0 → range_rate = -100 m/s
    // Doppler = -range_rate / wavelength_L1.
    const auto payload = buildMsm7GpsPayload(
        100000, 67, 0, /*rough_rate=*/-100, 0, 0, 64, 0, 720, 0);
    const auto frame = buildFrame(payload);

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    const auto& obs = parser.getObservations().front();

    const double expected = 100.0 / Constants::GPS_L1_WAVELENGTH;
    auto it = obs.measurements.find("D1C");
    ASSERT_NE(it, obs.measurements.end());
    EXPECT_NEAR(it->second, expected, 1e-3);
}

} // namespace
} // namespace GnssProcessor
