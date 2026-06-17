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
#include "rtcm/RtcmNavDecoder.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"

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

    // Per-sat block.  DF419 (extended satellite info, 4 bits) is present for
    // EVERY system in MSM7 — written here as reserved 0 so the frame is
    // spec-correct and the decoder's DF419 consumption stays byte-aligned.
    pw.put(8,  rough_range_int);
    pw.put(4,  0);                             // DF419 extended satellite info
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

// Build a GPS MSM7 payload for G05 with TWO signals — 1C (id 2) and 2W (id 10) —
// including DF419, so the multi-cell per-band reconstruction path is exercised.
// Cell order is sat-major, signal-ascending → cell 0 = 1C (L1), cell 1 = 2W (L2).
static std::vector<uint8_t> buildMsm7GpsTwoSignal(
    int rough_range_int, int fine_pr_1c, int fine_pr_2w)
{
    BitWriter pw;
    pw.put(12, 1077);                          // DF002 GPS MSM7
    pw.put(12, 100);                           // DF003 station id
    pw.put(30, 100000);                        // DF004 GPS epoch ms
    pw.put(1, 0); pw.put(3, 0); pw.put(7, 0);  // multi-msg + IODS + reserved
    pw.put(2, 0); pw.put(2, 0); pw.put(1, 0); pw.put(3, 0); // clk/ext/smooth

    pw.put(64, 1ull << (63 - 4));              // Sat mask: G05 = bit 4
    pw.put(32, (1u << (31 - 1)) | (1u << (31 - 9))); // Sig mask: id2 (1C) + id10 (2W)
    pw.put(2, 0x3);                            // Cell mask 1×2, both present

    // Per-sat block: DF397, DF419 (present!), DF398, DF399.
    pw.put(8,  rough_range_int);
    pw.put(4,  0);                             // DF419
    pw.put(10, 0);                             // DF398
    pw.putSigned(14, 0);                       // DF399

    // Per-cell block, field-major across the 2 cells (1C then 2W).
    pw.putSigned(20, fine_pr_1c);  pw.putSigned(20, fine_pr_2w);   // DF405
    pw.putSigned(24, 0);           pw.putSigned(24, 0);            // DF406
    pw.put(10, 64);                pw.put(10, 64);                 // DF407 lock
    pw.put(1, 0);                  pw.put(1, 0);                   // DF420 half
    pw.put(10, 720);               pw.put(10, 720);                // DF408 cnr
    pw.putSigned(15, 0);           pw.putSigned(15, 0);            // DF404 rate

    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

// Regression test for the DF419 omission: pre-fix the decoder skipped DF419
// (4 bits/sat) for non-GLONASS systems, reading the per-cell block 4 bits early
// and scrambling the L1/L2 split (the live stream's +124 m ionospherically-
// impossible geometry-free).  With DF419 consumed, the two bands reconstruct to
// the encoded values and the geometry-free is the small NEGATIVE value we set.
TEST(RtcmParserDecodeTest, Msm7GpsTwoSignalDf419Alignment) {
    const int fine_2w = 8953;  // ~ +5.0 m on L2 → C2W sits ABOVE C1C (physical)
    const auto payload = buildMsm7GpsTwoSignal(/*rough_int=*/67,
                                               /*fine_1c=*/0, /*fine_2w=*/fine_2w);

    std::vector<RinexObservation> obs;
    const auto res = RtcmMsm::decodeMsm7(payload.data(), payload.size(), 1077, 0, obs);

    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.n_satellites, 1);
    EXPECT_EQ(res.n_signals, 2);
    EXPECT_EQ(res.n_cells, 2);
    // DF419 alignment invariant: the decode consumes the whole payload bar the
    // 0-7 trailing byte-pad bits.  Pre-fix this under-consumed by 4·Nsat bits.
    EXPECT_LT(payload.size() * 8 - res.bits_consumed, 8u);

    ASSERT_EQ(obs.size(), 1u);
    const auto& o = obs.front();
    EXPECT_EQ(o.satellite, "G05");

    const double c     = Constants::SPEED_OF_LIGHT;
    const double scale = 1.0 / (1ull << 29);   // DF405 2^-29 ms
    const double exp_c1c = 67e-3 * c;
    const double exp_c2w = (67e-3 + fine_2w * scale * 1e-3) * c;

    auto it_c1 = o.measurements.find("C1C");
    auto it_c2 = o.measurements.find("C2W");
    ASSERT_NE(it_c1, o.measurements.end());
    ASSERT_NE(it_c2, o.measurements.end());
    EXPECT_NEAR(it_c1->second, exp_c1c, 1e-3);
    EXPECT_NEAR(it_c2->second, exp_c2w, 1e-3);

    // Decisive guard: geometry-free C1-C2 is the small NEGATIVE value encoded,
    // never the +124 m garbage the DF419 mis-alignment produced live.
    EXPECT_NEAR(it_c1->second - it_c2->second, -fine_2w * scale * 1e-3 * c, 1e-3);
    EXPECT_LT(it_c1->second - it_c2->second, 0.0);
}

// ============================================================================
// IGS-SSR (RTCM 4076) — IM-based dispatch reuses legacy body decoders
// ============================================================================

// Helper: build the bytes of an IGS-SSR 4076 GPS Clock (IM=22) payload
// with one satellite (G05) and a polynomial (c0, c1=0, c2=0).
// IGS message numbers: GPS = 21-27 (clock = 22), per IGS-SSR Format v1.00.
static std::vector<uint8_t> buildIgsSsr4076GpsClock(double c0_m, int iod_ssr) {
    BitWriter pw;
    pw.put(12, 4076);                       // DF002 message type
    pw.put(3,  1);                          // IGS-SSR version
    pw.put(8,  22);                         // IM: GPS Clock (IGM02)
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
    // IM = 61 = Galileo Orbit (IGM01) per IGS-SSR Format v1.00 (GAL = 61-67).
    // The IGS-SSR satellite block uses a UNIFORM layout for every constellation:
    // IDF011 6-bit sat id + IDF012 8-bit GNSS IOD + 6×delta fields (NO 24-bit
    // IODCRC).  This is NARROWER than the legacy 1240-series Galileo orbit, which
    // uses a 10-bit IODE — the bug this test now guards against.
    BitWriter pw;
    pw.put(12, 4076);
    pw.put(3, 1);
    pw.put(8, 61);                          // IM = Galileo Orbit (IGM01)
    pw.put(20, 0);                          // epoch
    pw.put(4, 0);                           // update interval
    pw.put(1, 0);                           // multi-msg
    pw.put(1, 0);                           // datum (orbit messages have this)
    pw.put(4, 3);                           // IOD-SSR
    pw.put(16, 0);                          // provider
    pw.put(4, 0);                           // solution
    pw.put(6, 1);                           // num sats
    pw.put(6, 11);                          // sat id (E11)
    pw.put(8, 42);                          // GNSS IOD (IGS-SSR 4076: 8 bits for all)
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

// ============================================================================
// Broadcast ephemeris (RTCM 1019/1042/1046) + LiveEphemerisStore orbit model
// ============================================================================

// Raw (pre-scale) DF integer values for a GPS 1019 message.  Only fields the
// tests assert on get meaningful values; the rest are zero.
struct Gps1019Raw {
    int     sat = 5;
    int     iode = 42;
    int     af0 = 0;
    uint32_t e = 0;
    uint32_t sqrtA = 0;
    int     toe = 0;
    int64_t i0 = 0;
    int     omdot = 0;
};

// Encode a GPS 1019 payload in exact RTCM field order.
static std::vector<uint8_t> buildGps1019Payload(const Gps1019Raw& v) {
    BitWriter pw;
    pw.put(12, 1019);              // DF002
    pw.put(6,  v.sat);             // DF009
    pw.put(10, 200);               // DF076 week
    pw.put(4,  0);                 // DF077 URA
    pw.put(2,  0);                 // DF078 code on L2
    pw.putSigned(14, 0);           // DF079 IDOT
    pw.put(8,  v.iode);            // DF071
    pw.put(16, 0);                 // DF081 toc
    pw.putSigned(8,  0);           // DF082 af2
    pw.putSigned(16, 0);           // DF083 af1
    pw.putSigned(22, v.af0);       // DF084 af0
    pw.put(10, 0);                 // DF085 IODC
    pw.putSigned(16, 0);           // DF086 Crs
    pw.putSigned(16, 0);           // DF087 dn
    pw.putSigned(32, 0);           // DF088 M0
    pw.putSigned(16, 0);           // DF089 Cuc
    pw.put(32, v.e);               // DF090 e
    pw.putSigned(16, 0);           // DF091 Cus
    pw.put(32, v.sqrtA);           // DF092 sqrtA
    pw.put(16, v.toe);             // DF093 toe
    pw.putSigned(16, 0);           // DF094 Cic
    pw.putSigned(32, 0);           // DF095 Om0
    pw.putSigned(16, 0);           // DF096 Cis
    pw.putSigned(32, v.i0);        // DF097 i0
    pw.putSigned(16, 0);           // DF098 Crc
    pw.putSigned(32, 0);           // DF099 omega
    pw.putSigned(24, v.omdot);     // DF100 Omdot
    pw.putSigned(8,  0);           // DF101 Tgd
    pw.put(6,  0);                 // DF102 health
    pw.put(1,  0);                 // DF103 L2 P data flag
    pw.put(1,  0);                 // DF137 fit interval
    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

TEST(RtcmNavDecodeTest, Gps1019RoundTrip) {
    Gps1019Raw v;
    v.sat = 5; v.iode = 42;
    v.af0   = 12345;
    v.e     = 400000u;
    v.sqrtA = 2702000000u;         // sqrt_a ≈ 5154.4  (a ≈ 26.56e6 m)
    v.toe   = 450;                 // 450 * 16 = 7200 s
    v.i0    = 656000000;           // ≈ 0.9598 rad after × 2^-31 × π
    v.omdot = -2000;
    const auto frame = buildFrame(buildGps1019Payload(v));

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    EXPECT_EQ(parser.lastMessageType(), 1019);
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);

    const auto& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "G05");
    EXPECT_EQ(eph.system, System::GPS);
    EXPECT_EQ(eph.iode, 42);

    const auto& k = eph.kepler;
    EXPECT_NEAR(k.af0,       12345 * std::ldexp(1.0, -31), 1e-15);
    EXPECT_NEAR(k.e,         400000u * std::ldexp(1.0, -33), 1e-15);
    EXPECT_NEAR(k.sqrt_a,    2702000000u * std::ldexp(1.0, -19), 1e-6);
    EXPECT_NEAR(k.toe.tow,   7200.0, 1e-6);
    EXPECT_NEAR(k.i0,        656000000 * std::ldexp(1.0, -31) * Constants::PI, 1e-9);
    EXPECT_NEAR(k.omega_dot, -2000 * std::ldexp(1.0, -43) * Constants::PI, 1e-18);
}

TEST(RtcmNavDecodeTest, Beidou1042ScalesAndBdtWeek) {
    BitWriter pw;
    pw.put(12, 1042);
    pw.put(6, 8);                  // sat -> C08
    pw.put(13, 900);               // DF489 BDS week (BDT)
    pw.put(4, 0);                  // URAI
    pw.putSigned(14, 0);           // IDOT
    pw.put(5, 21);                 // AODE
    pw.put(17, 0);                 // toc
    pw.putSigned(11, 0);           // a2
    pw.putSigned(22, 0);           // a1
    pw.putSigned(24, 1000);        // a0 -> 1000 * 2^-33
    pw.put(5, 0);                  // AODC
    pw.putSigned(18, -64);         // Crs -> -64 * 2^-6 = -1.0
    pw.putSigned(16, 0);           // dn
    pw.putSigned(32, 0);           // M0
    pw.putSigned(18, 0);           // Cuc
    pw.put(32, 400000u);           // e
    pw.putSigned(18, 0);           // Cus
    pw.put(32, 2702000000u);       // sqrtA
    pw.put(17, 900);               // toe -> 900 * 8 = 7200 s
    pw.putSigned(18, 0);           // Cic
    pw.putSigned(32, 0);           // Om0
    pw.putSigned(18, 0);           // Cis
    pw.putSigned(32, 0);           // i0
    pw.putSigned(18, 0);           // Crc
    pw.putSigned(32, 0);           // omega
    pw.putSigned(24, 0);           // Omdot
    pw.putSigned(10, 0);           // TGD1
    pw.putSigned(10, 0);           // TGD2
    pw.put(1, 0);                  // health
    while (pw.bitCount() % 8) pw.put(1, 0);
    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const auto& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "C08");
    EXPECT_EQ(eph.system, System::BeiDou);
    EXPECT_EQ(eph.iode, 21);
    EXPECT_EQ(eph.kepler.toe.week, 900);                       // native BDT week stored as-is
    EXPECT_NEAR(eph.kepler.toe.tow, 7200.0, 1e-6);            // 900 * 8 s (BDS toe scale)
    EXPECT_NEAR(eph.kepler.crs, -64 * std::ldexp(1.0, -6), 1e-12);    // BDS Crs scale 2^-6
    EXPECT_NEAR(eph.kepler.af0, 1000 * std::ldexp(1.0, -33), 1e-15);
}

TEST(RtcmNavDecodeTest, Galileo1046ScalesAndWeek) {
    BitWriter pw;
    pw.put(12, 1046);
    pw.put(6, 11);                 // sat -> E11
    pw.put(12, 1200);              // DF289 Galileo week (GST)
    pw.put(10, 77);                // IODnav
    pw.put(8, 0);                  // SISA
    pw.putSigned(14, 0);           // IDOT
    pw.put(14, 100);               // toc -> 100 * 60 = 6000 s
    pw.putSigned(6, 0);            // af2
    pw.putSigned(21, 0);           // af1
    pw.putSigned(31, 99999);       // af0 -> 99999 * 2^-34
    pw.putSigned(16, 0);           // Crs
    pw.putSigned(16, 0);           // dn
    pw.putSigned(32, 0);           // M0
    pw.putSigned(16, 0);           // Cuc
    pw.put(32, 400000u);           // e
    pw.putSigned(16, 0);           // Cus
    pw.put(32, 2702000000u);       // sqrtA
    pw.put(14, 120);               // toe -> 120 * 60 = 7200 s
    pw.putSigned(16, 0);           // Cic
    pw.putSigned(32, 0);           // Om0
    pw.putSigned(16, 0);           // Cis
    pw.putSigned(32, 0);           // i0
    pw.putSigned(16, 0);           // Crc
    pw.putSigned(32, 0);           // omega
    pw.putSigned(24, 0);           // Omdot
    pw.putSigned(10, 0);           // BGD E1/E5a
    pw.putSigned(10, 0);           // BGD E1/E5b
    pw.put(2, 0);                  // E5b signal health
    pw.put(1, 0);                  // E5b data validity
    pw.put(2, 0);                  // E1-B signal health
    pw.put(1, 0);                  // E1-B data validity
    while (pw.bitCount() % 8) pw.put(1, 0);
    const auto frame = buildFrame(pw.bytes());

    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const auto& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "E11");
    EXPECT_EQ(eph.system, System::Galileo);
    EXPECT_EQ(eph.iode, 77);
    EXPECT_NEAR(eph.kepler.toe.tow, 7200.0, 1e-6);                  // 120 * 60 (GAL toe scale)
    EXPECT_NEAR(eph.kepler.af0, 99999 * std::ldexp(1.0, -34), 1e-15);  // GAL af0 scale 2^-34
    EXPECT_GT(eph.kepler.toe.week, 2000);                          // GST week + 1024 -> GPS week
}

TEST(LiveEphemerisStoreTest, GpsCircularOrbitRadiusAndMotion) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();

    BroadcastEphemeris eph;
    eph.satellite = "G07";
    eph.system    = System::GPS;
    eph.iode      = 10;
    auto& k = eph.kepler;
    k.toe   = GnssTime{2300, 7200.0};
    k.sqrt_a = 5153.6;             // a ≈ 26.56e6 m
    k.e      = 0.0;                // circular -> r == a at all tk
    k.i0 = 0.96; k.omega0 = 0.5; k.omega = 0.3; k.m0 = 0.2;
    store.insert(eph);

    const double a = k.sqrt_a * k.sqrt_a;

    auto pv = store.computeSatellitePV("G07", GnssTime{2300, 7200.0});
    ASSERT_TRUE(pv.has_value());
    EXPECT_NEAR(pv->position.norm(), a, 1.0);     // circular orbit radius == a
    EXPECT_GT(pv->velocity.norm(), 1000.0);       // plausible ECEF speed band
    EXPECT_LT(pv->velocity.norm(), 6000.0);
    EXPECT_EQ(pv->iode, 10);

    // 30 minutes on: still on the sphere, but materially displaced.
    auto pv2 = store.computeSatellitePV("G07", GnssTime{2300, 7200.0 + 1800.0});
    ASSERT_TRUE(pv2.has_value());
    EXPECT_NEAR(pv2->position.norm(), a, 1.0);
    EXPECT_GT((pv2->position - pv->position).norm(), 1000.0);

    EXPECT_FALSE(store.computeSatellitePV("G31", GnssTime{2300, 7200.0}).has_value());
    store.clear();
}

// The IODE the store reports to the SSR overlay gate must be expressed in the
// IGS-SSR (RTCM 4076) "GNSS IOD" matching convention, NOT the raw broadcast
// field — otherwise the IODE-match gate never passes for Galileo/BeiDou and
// their SSR clocks are silently dropped (the GAL/BDS "stuck at broadcast grade"
// bug).  Per IGS-SSR Format v1.00 IDF012:
//   GPS    : the 8-bit LNAV IODE (passthrough),
//   Galileo: the low 8 bits of the 10-bit I/NAV IODnav,
//   BeiDou : mod(toe_sow / 720, 240)  (NOT the raw 5-bit AODE).
TEST(LiveEphemerisStoreTest, SsrMatchIodeConventionPerConstellation) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();

    // Galileo: full IODnav 300 -> low 8 bits = 300 & 0xFF = 44.
    BroadcastEphemeris e;
    e.satellite = "E11"; e.system = System::Galileo; e.iode = 300;
    e.kepler.toe = GnssTime{2300, 7200.0}; e.kepler.toc = GnssTime{2300, 7200.0};
    store.insert(e);
    const auto gal = store.computeSatelliteClock("E11", GnssTime{2300, 7200.0});
    ASSERT_TRUE(gal.has_value());
    EXPECT_EQ(gal->iode, 44);

    // BeiDou: toe_sow = 7200 -> mod(7200/720, 240) = 10, NOT the raw AODE (21).
    // toe.week is the NATIVE BDT week; the query is GPS week 2300, so the toe
    // must sit at BDT week 2300 - 1356 = 944 (BDT week 0 = GPS week 1356) for the
    // epochs to coincide.  This also guards the staleness path: it lifts the
    // BeiDou toe to GPS scale via toeGpsContinuous, so a raw-week comparison
    // would read ~1356 weeks stale and drop the clock (has_value() == false).
    BroadcastEphemeris c;
    c.satellite = "C08"; c.system = System::BeiDou; c.iode = 21;   // raw AODE, must be ignored
    c.kepler.toe = GnssTime{944, 7200.0}; c.kepler.toc = GnssTime{944, 7200.0};
    store.insert(c);
    const auto bds = store.computeSatelliteClock("C08", GnssTime{2300, 7200.0});
    ASSERT_TRUE(bds.has_value());
    EXPECT_EQ(bds->iode, 10);

    // GPS: 8-bit LNAV IODE passes through unchanged.
    BroadcastEphemeris g;
    g.satellite = "G07"; g.system = System::GPS; g.iode = 200;
    g.kepler.toe = GnssTime{2300, 7200.0}; g.kepler.toc = GnssTime{2300, 7200.0};
    store.insert(g);
    const auto gps = store.computeSatelliteClock("G07", GnssTime{2300, 7200.0});
    ASSERT_TRUE(gps.has_value());
    EXPECT_EQ(gps->iode, 200);

    store.clear();
}

// ============================================================================
// SSR header propagation (M3): parser exposes it, store caches it
// ============================================================================
TEST(RtcmParserDecodeTest, SsrHeaderExposedByParser) {
    const auto frame = buildFrame(buildIgsSsr4076GpsClock(/*c0_m=*/0.025, /*iod=*/7));
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(frame));

    const auto& h = parser.getLastSsrHeader();
    EXPECT_EQ(h.iod_ssr, 7);
    EXPECT_EQ(h.system, System::GPS);
    EXPECT_NEAR(h.epoch_time.tow, 100.0, 1e-9);   // DF385 epoch field = 100 s into week
}

TEST(SsrCorrectionStoreTest, HeaderEpochAndIodCached) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();

    SsrHeader h;
    h.epoch_time = GnssTime{2300, 100.0};
    h.iod_ssr    = 7;
    SsrClockCorrection c;
    c.sat = "G05";
    c.c0  = 0.025;
    store.pushClock(c, h);

    EXPECT_EQ(store.getSsrIod("G05"), 7);
    auto e = store.getSsrEpoch("G05");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->week, 2300);
    EXPECT_NEAR(e->tow, 100.0, 1e-9);

    EXPECT_EQ(store.getSsrIod("G31"), -1);                 // unknown sat
    EXPECT_FALSE(store.getSsrEpoch("G31").has_value());
    store.clear();
}

// ============================================================================
// GLONASS time-of-day -> GPS time conversion (TimeUtils::glonassTodToGpsTime).
//
// The two correctness gates the real-time GLONASS path depends on: the GPS-UTC
// leap-second offset, and reconstruction of the full GPS week/day from a bare
// time-of-day anchored on the rover clock.  Expected values are produced by the
// same UTC->GPS path the RINEX "GLO" reader uses (gregorianToGpsTime), so a
// mismatch flags the converter, not the table.
// ============================================================================

// GLONASS (Moscow = UTC+3h) seconds-of-day for a given UTC time-of-day.
static double gloTodFromUtc(int h, int m, double s) {
    double sod = (h * 3600.0 + m * 60.0 + s) + 3.0 * 3600.0;   // shift to Moscow day
    while (sod >= 86400.0) sod -= 86400.0;
    return sod;
}
// Anchor (continuous GPS seconds) for a UTC instant.
static double anchorOfUtc(int y, int mo, int d, int h, int m, double s) {
    return TimeUtils::continuousGpsSeconds(TimeUtils::gregorianToGpsTime(y, mo, d, h, m, s));
}

TEST(GlonassTodToGps, MiddayRoundTripWithLeapSeconds) {
    // UTC 2026-06-16 12:00:00 -> Moscow 15:00 -> tod 54000.  Anchored at the
    // same instant.  A converter that dropped the 18 s leap offset would miss
    // the expected tow by exactly 18 s.
    const GnssTime expected = TimeUtils::gregorianToGpsTime(2026, 6, 16, 12, 0, 0);
    const double   anchor   = TimeUtils::continuousGpsSeconds(expected);
    const GnssTime got = TimeUtils::glonassTodToGpsTime(gloTodFromUtc(12, 0, 0.0), anchor);
    EXPECT_EQ(got.week, expected.week);
    EXPECT_NEAR(got.tow, expected.tow, 1e-3);   // 1 ms: well under any time-tag need; JD round-trip noise ~1e-5 s
}

TEST(GlonassTodToGps, MoscowDayBoundaryAfterUtcEvening) {
    // UTC 22:00 is already the NEXT Moscow day (01:00).  The Moscow-day framing
    // must still land on the correct UTC instant.
    const GnssTime expected = TimeUtils::gregorianToGpsTime(2026, 6, 16, 22, 0, 0);
    const double   anchor   = TimeUtils::continuousGpsSeconds(expected);
    const GnssTime got = TimeUtils::glonassTodToGpsTime(gloTodFromUtc(22, 0, 0.0), anchor);
    EXPECT_EQ(got.week, expected.week);
    EXPECT_NEAR(got.tow, expected.tow, 1e-3);   // 1 ms: well under any time-tag need; JD round-trip noise ~1e-5 s
}

TEST(GlonassTodToGps, AnchorJitterDoesNotShiftResult) {
    // The result is the time-of-day's instant, not the anchor's; a +120 s anchor
    // error within the same day must not move it.
    const GnssTime expected = TimeUtils::gregorianToGpsTime(2026, 6, 16, 8, 30, 0);
    const double   anchor   = anchorOfUtc(2026, 6, 16, 8, 32, 0);   // +120 s
    const GnssTime got = TimeUtils::glonassTodToGpsTime(gloTodFromUtc(8, 30, 0.0), anchor);
    EXPECT_EQ(got.week, expected.week);
    EXPECT_NEAR(got.tow, expected.tow, 1e-3);   // 1 ms: well under any time-tag need; JD round-trip noise ~1e-5 s
}

TEST(GlonassTodToGps, WrapResolvesToNearestMoscowDay) {
    // tod = 23:25 Moscow (84300 s) with an anchor 1 h later that has already
    // crossed Moscow midnight (00:25).  The +/-12 h wrap must place the tod in
    // the PREVIOUS Moscow day, i.e. ~1 h before the anchor instant.
    const GnssTime expected = TimeUtils::gregorianToGpsTime(2026, 6, 16, 20, 25, 0);
    const double   anchor   = anchorOfUtc(2026, 6, 16, 21, 25, 0);
    const GnssTime got = TimeUtils::glonassTodToGpsTime(gloTodFromUtc(20, 25, 0.0), anchor);
    EXPECT_EQ(got.week, expected.week);
    EXPECT_NEAR(got.tow, expected.tow, 1e-3);   // 1 ms: well under any time-tag need; JD round-trip noise ~1e-5 s
    EXPECT_LT(TimeUtils::continuousGpsSeconds(got), anchor);   // resolved into the past
}

TEST(GlonassTodToGps, NoAnchorReturnsZero) {
    const GnssTime got = TimeUtils::glonassTodToGpsTime(54000.0, 0.0);
    EXPECT_EQ(got.week, 0);
    EXPECT_NEAR(got.tow, 0.0, 1e-12);
}

} // namespace
} // namespace GnssProcessor
