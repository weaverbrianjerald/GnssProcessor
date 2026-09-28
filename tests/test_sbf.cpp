/**
 * @file test_sbf.cpp
 * @brief Unit tests for the Septentrio SBF decoding path.
 *
 * Coverage:
 *   - SbfStreamReader: framing on the "$@" sync, CRC-16/XMODEM validation,
 *     resync past garbage, rejection of corrupted blocks.
 *   - SbfParser MeasEpoch (4027): a one-satellite/one-signal GPS L1CA epoch
 *     reconstructed into a RinexObservation (pseudorange, phase in cycles,
 *     Doppler, CN0 with the +10 offset rule).
 *   - SbfParser GPSNav (5891): Keplerian decode (semicircle→radian) and an
 *     end-to-end circular-orbit position via LiveEphemerisStore.
 *
 * Synthetic blocks are built with correct CRCs by buildSbfBlock().  As with
 * the RTCM tests these validate the decoder against the SBF spec, not against
 * a live receiver (field-offset risks are validated against a real capture).
 */

#include <gtest/gtest.h>

#include "sbf/SbfStreamReader.hpp"
#include "sbf/SbfParser.hpp"
#include "sbf/SbfSignalMap.hpp"
#include "sbf/Meas3Decoder.hpp"
#include "sbf/SubframeAssembler.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "common/Constants.hpp"

#include <cstring>
#include <cstdint>
#include <vector>

namespace GnssProcessor {
namespace {

// CRC-16/CCITT-XMODEM (poly 0x1021, init 0x0000), the same CRC SbfStreamReader checks.
uint16_t crc16Xmodem(const uint8_t* d, std::size_t n) {
    uint16_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(d[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// Assemble a complete SBF block from a body (bytes from TOW onward).
std::vector<uint8_t> buildSbfBlock(int blockNumber, int revision, std::vector<uint8_t> body) {
    std::size_t total = 8 + body.size();
    while (total % 4 != 0) { body.push_back(0); total = 8 + body.size(); }
    const uint16_t id = static_cast<uint16_t>((blockNumber & 0x1FFF) | (revision << 13));
    std::vector<uint8_t> block(8, 0);
    block[0] = 0x24; block[1] = 0x40;
    block[4] = id & 0xFF;          block[5] = (id >> 8) & 0xFF;
    block[6] = total & 0xFF;       block[7] = (total >> 8) & 0xFF;
    block.insert(block.end(), body.begin(), body.end());
    const uint16_t crc = crc16Xmodem(block.data() + 4, total - 4);
    block[2] = crc & 0xFF; block[3] = (crc >> 8) & 0xFF;
    return block;
}

// Little-endian writers into a fixed-size body at an offset.
void wr_u8 (std::vector<uint8_t>& b, std::size_t o, uint8_t  v) { b[o] = v; }
void wr_u16(std::vector<uint8_t>& b, std::size_t o, uint16_t v) { std::memcpy(b.data()+o, &v, 2); }
void wr_u32(std::vector<uint8_t>& b, std::size_t o, uint32_t v) { std::memcpy(b.data()+o, &v, 4); }
void wr_i32(std::vector<uint8_t>& b, std::size_t o, int32_t  v) { std::memcpy(b.data()+o, &v, 4); }
void wr_f32(std::vector<uint8_t>& b, std::size_t o, float    v) { std::memcpy(b.data()+o, &v, 4); }
void wr_f64(std::vector<uint8_t>& b, std::size_t o, double   v) { std::memcpy(b.data()+o, &v, 8); }

// ============================================================================
// SbfStreamReader
// ============================================================================
TEST(SbfStreamReaderTest, ExtractsValidBlock) {
    std::vector<uint8_t> body(16, 0);
    wr_u32(body, 0, 100000);   // TOW
    wr_u16(body, 4, 2300);     // WNc
    const auto block = buildSbfBlock(5914, 0, body);   // arbitrary block

    SbfStreamReader r;
    r.feed(block);
    auto f = r.nextFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->size(), block.size());
    EXPECT_EQ(r.framesExtracted(), 1u);
    EXPECT_FALSE(r.nextFrame().has_value());            // nothing left
}

TEST(SbfStreamReaderTest, ResyncsPastGarbage) {
    std::vector<uint8_t> body(16, 0);
    wr_u32(body, 0, 1); wr_u16(body, 4, 2300);
    const auto block = buildSbfBlock(4027, 1, body);

    SbfStreamReader r;
    r.feed(std::vector<uint8_t>{0x11, 0x24, 0x99, 0x40});  // garbage incl. lone sync bytes
    r.feed(block);
    auto f = r.nextFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_GE(r.bytesDiscarded(), 4u);
}

TEST(SbfStreamReaderTest, RejectsCorruptedBlock) {
    std::vector<uint8_t> body(16, 0);
    wr_u32(body, 0, 1); wr_u16(body, 4, 2300);
    auto block = buildSbfBlock(5914, 0, body);
    block[10] ^= 0xFF;                                   // corrupt a body byte -> CRC fails

    SbfStreamReader r;
    r.feed(block);
    EXPECT_FALSE(r.nextFrame().has_value());             // no valid frame recovered
    EXPECT_GE(r.bytesDiscarded(), 1u);
}

// ============================================================================
// SbfSignalMap: the corrected signal numbering
// ============================================================================
TEST(SbfSignalMapTest, CorrectedSignalNumbers) {
    EXPECT_EQ(SbfSignalMap::signalInfo(0).suffix, "1C");   // GPS L1CA
    EXPECT_EQ(SbfSignalMap::signalInfo(0).system, System::GPS);
    EXPECT_EQ(SbfSignalMap::signalInfo(17).suffix, "1C");  // GAL E1 (not E5a)
    EXPECT_EQ(SbfSignalMap::signalInfo(17).system, System::Galileo);
    EXPECT_EQ(SbfSignalMap::signalInfo(20).suffix, "5Q");  // GAL E5a
    EXPECT_EQ(SbfSignalMap::signalInfo(13).suffix, "1P");  // BDS B1C (not B1I)
    EXPECT_EQ(SbfSignalMap::signalInfo(28).suffix, "2I");  // BDS B1I (RINEX 3 code 2I)
    EXPECT_EQ(SbfSignalMap::signalInfo(28).system, System::BeiDou);

    System sys;
    EXPECT_EQ(SbfSignalMap::svidToPrn(5,   sys), "G05"); EXPECT_EQ(sys, System::GPS);
    EXPECT_EQ(SbfSignalMap::svidToPrn(81,  sys), "E11"); EXPECT_EQ(sys, System::Galileo);
    EXPECT_EQ(SbfSignalMap::svidToPrn(141, sys), "C01"); EXPECT_EQ(sys, System::BeiDou);
}

// ============================================================================
// MeasEpoch (4027): one GPS satellite, one signal (L1CA, Type1 only)
// ============================================================================
TEST(SbfMeasEpochTest, DecodesSingleGpsL1ca) {
    std::vector<uint8_t> body(32, 0);
    wr_u32(body, 0, 100000);   // TOW ms
    wr_u16(body, 4, 2300);     // WNc
    wr_u8 (body, 6, 1);        // N1
    wr_u8 (body, 7, 20);       // SB1Length
    wr_u8 (body, 8, 12);       // SB2Length
    wr_u8 (body, 9, 0);        // CommonFlags
    wr_u8 (body, 10, 0);       // CumClkJumps (rev>=1)
    wr_u8 (body, 11, 0);       // Reserved
    // Type1 sub-block at body offset 12 (abs 20).
    wr_u8 (body, 12, 0);       // RxChannel
    wr_u8 (body, 13, 0);       // Type -> signal 0 (GPS L1CA)
    wr_u8 (body, 14, 5);       // SVID -> G05
    wr_u8 (body, 15, 4);       // Misc -> CodeMSB = 4
    wr_u32(body, 16, 2820130816u);  // CodeLSB  (PR = (4*2^32 + this)*1e-3 = 20,000,000 m)
    wr_i32(body, 20, -25000000);    // Doppler  (-2500 Hz)
    wr_u16(body, 24, 0);       // CarrierLSB
    wr_u8 (body, 26, 0);       // CarrierMSB
    wr_u8 (body, 27, 160);     // CN0  -> 160*0.25 + 10 = 50 dB-Hz (sig 0 gets +10)
    wr_u16(body, 28, 100);     // LockTime
    wr_u8 (body, 30, 0);       // ObsInfo
    wr_u8 (body, 31, 0);       // N2

    const auto block = buildSbfBlock(4027, 1, body);
    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(block));
    EXPECT_EQ(parser.lastBlockNumber(), 4027);
    ASSERT_EQ(parser.getObservations().size(), 1u);

    const auto& obs = parser.getObservations().front();
    EXPECT_EQ(obs.satellite, "G05");
    EXPECT_EQ(obs.system, System::GPS);
    EXPECT_EQ(obs.time.week, 2300);
    EXPECT_NEAR(obs.time.tow, 100.0, 1e-9);

    ASSERT_NE(obs.measurements.find("C1C"), obs.measurements.end());
    EXPECT_NEAR(obs.measurements.at("C1C"), 20000000.0, 1e-3);

    const double expected_phase = 20000000.0 / Constants::GPS_L1_WAVELENGTH;
    ASSERT_NE(obs.measurements.find("L1C"), obs.measurements.end());
    EXPECT_NEAR(obs.measurements.at("L1C"), expected_phase, 1.0);

    ASSERT_NE(obs.measurements.find("D1C"), obs.measurements.end());
    EXPECT_NEAR(obs.measurements.at("D1C"), -2500.0, 1e-6);

    ASSERT_NE(obs.measurements.find("S1C"), obs.measurements.end());
    EXPECT_NEAR(obs.measurements.at("S1C"), 50.0, 1e-9);
}

// ============================================================================
// GPSNav (5891): Keplerian decode and circular-orbit PV via LiveEphemerisStore
// ============================================================================
TEST(SbfNavTest, DecodesGpsNavAndComputesOrbit) {
    std::vector<uint8_t> body(132, 0);
    wr_u32(body, 0, 100000);   // TOW
    wr_u16(body, 4, 2300);     // WNc
    wr_u8 (body, 6, 5);        // PRN -> G05
    wr_u16(body, 8, 300);      // WN (10-bit)
    wr_u8 (body, 12, 0);       // health
    wr_u8 (body, 16, 42);      // IODE2
    wr_f32(body, 20, 1e-9f);   // Tgd
    wr_u32(body, 24, 7200);    // toc
    wr_f32(body, 36, 1e-4f);   // af0
    wr_f64(body, 48, 0.1);     // M0     (semicircles -> 0.1π rad)
    wr_f64(body, 60, 0.0);     // e      (circular)
    wr_f64(body, 72, 5153.6);  // sqrtA
    wr_u32(body, 80, 7200);    // toe
    wr_f64(body, 88, 0.2);     // Omega0 (semicircles)
    wr_f64(body, 100, 0.3);    // i0     (semicircles -> ~0.94 rad)
    wr_f64(body, 112, 0.05);   // omega  (semicircles)
    wr_u16(body, 128, 300);    // WNt_oc
    wr_u16(body, 130, 300);    // WNt_oe

    const auto block = buildSbfBlock(5891, 0, body);
    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(block));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);

    const auto& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "G05");
    EXPECT_EQ(eph.system, System::GPS);
    EXPECT_EQ(eph.iode, 42);
    const auto& k = eph.kepler;
    EXPECT_NEAR(k.sqrt_a, 5153.6, 1e-6);
    EXPECT_NEAR(k.e, 0.0, 1e-12);
    EXPECT_NEAR(k.toe.tow, 7200.0, 1e-6);
    EXPECT_NEAR(k.m0, 0.1 * Constants::PI, 1e-9);   // semicircle -> radian
    EXPECT_NEAR(k.i0, 0.3 * Constants::PI, 1e-9);

    // End-to-end: feed the live store and compute the orbit at toe.
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(eph);
    auto pv = store.computeSatellitePV("G05", GnssTime{eph.kepler.toe.week, 7200.0});
    ASSERT_TRUE(pv.has_value());
    const double a = 5153.6 * 5153.6;               // circular -> |pos| == a
    EXPECT_NEAR(pv->position.norm(), a, 1.0);
    EXPECT_EQ(pv->iode, 42);
    store.clear();
}

// ============================================================================
// RawNavBits container (GPSRawCA 4017) + SubframeAssembler gating
// ============================================================================
TEST(SbfRawNavTest, GpsRawCaContainer) {
    std::vector<uint8_t> body(52, 0);
    wr_u32(body, 0, 493182000);   // TOW
    wr_u16(body, 4, 2421);        // WNc
    wr_u8 (body, 6, 5);           // SVID -> G05
    wr_u8 (body, 7, 1);           // CRCPassed
    wr_u8 (body, 8, 0);           // ViterbiCnt
    wr_u8 (body, 9, 0);           // Source (GPS L1CA)
    wr_u8 (body, 10, 0);          // FreqNr
    wr_u8 (body, 11, 7);          // RxChannel
    for (int i = 0; i < 10; ++i) wr_u32(body, 12 + 4 * i, 0x10000000u + i);  // NavBits
    const auto block = buildSbfBlock(4017, 0, body);

    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(block));
    EXPECT_EQ(parser.lastBlockNumber(), 4017);
    ASSERT_EQ(parser.getRawNavMessages().size(), 1u);

    const auto& m = parser.getRawNavMessages().front();
    EXPECT_EQ(m.sat, "G05");
    EXPECT_EQ(m.system, System::GPS);
    EXPECT_TRUE(m.crc_passed);
    EXPECT_EQ(m.wnc, 2421);
    EXPECT_NEAR(m.tow_s, 493182.0, 1e-6);
    ASSERT_EQ(m.words.size(), 10u);
    EXPECT_EQ(m.words[0], 0x10000000u);
    EXPECT_EQ(m.words[9], 0x10000009u);
}

// ============================================================================
// Meas3Ranges (4109): real mosaic-X5 capture bytes, cross-checked against the
// co-logged MeasEpoch (4027) block at the same epoch.  The reference epoch
// (TOW 434750000) is self-contained; the delta epoch (434751000) exercises the
// stateful differential path against the cached reference.  Expected values are
// the 4027-decoded observables for the same (satellite, code); agreement is at
// the shared 1 mm / 0.001 cycle resolution.  Covers GPS master+slave (L1C/L2W),
// Galileo (E1/E5a), BeiDou B1I (long-range PR base) and GLONASS L1CA (FDMA).
// ============================================================================
namespace {
// Find the observation for a satellite in a decoded epoch, or nullptr.
const RinexObservation* findSat(const std::vector<RinexObservation>& v, const std::string& prn) {
    for (const auto& o : v) if (o.satellite == prn) return &o;
    return nullptr;
}
void expectObs(const std::vector<RinexObservation>& epoch, const std::string& prn,
               const std::string& band, double code_m, double phase_cyc) {
    const RinexObservation* o = findSat(epoch, prn);
    ASSERT_NE(o, nullptr) << prn << " missing";
    const auto c = o->measurements.find("C" + band);
    const auto l = o->measurements.find("L" + band);
    ASSERT_NE(c, o->measurements.end()) << prn << " C" << band << " missing";
    ASSERT_NE(l, o->measurements.end()) << prn << " L" << band << " missing";
    EXPECT_NEAR(c->second, code_m,    0.02) << prn << " C" << band;   // 4027 vs 4109 ~1 mm
    EXPECT_NEAR(l->second, phase_cyc, 0.02) << prn << " L" << band;   // ~0.001 cycle
}
} // namespace

TEST(SbfMeas3Test, ReferenceAndDeltaMatchMeasEpoch) {
    // Reference epoch (TOW 434750000, full measurements), length 616 bytes.
    static const uint8_t kRefBlock[] = {
        0x24,0x40,0xd4,0x9d,0x0d,0x10,0x68,0x02,0x30,0xc2,0xe9,0x19,0x75,0x09,0x01,0x40,
        0x1f,0x00,0x58,0x00,0x04,0x07,0x0a,0x00,0xc9,0xb1,0x48,0x96,0x9b,0x53,0x3c,0x2d,
        0x84,0xf0,0xd5,0xa9,0xa6,0x16,0x4f,0xc1,0xc9,0x9d,0x6a,0x4d,0x21,0x97,0xb0,0x16,
        0x8d,0x99,0x3b,0x45,0x86,0xb4,0xef,0xcd,0xa3,0x3c,0x3c,0x11,0x11,0x0f,0x16,0x98,
        0xd9,0x14,0x3e,0xeb,0x4a,0xc4,0xf7,0xf2,0x6a,0x72,0x89,0xf8,0xf6,0x84,0x41,0xd1,
        0x0f,0x26,0x45,0x57,0x1f,0x99,0xd8,0xa0,0xe4,0x04,0x3d,0x04,0xe7,0x9c,0xdc,0x6a,
        0x81,0xac,0x3d,0x88,0x41,0x0e,0x10,0x28,0x8d,0x3a,0x1c,0x94,0x81,0x9c,0x74,0x8f,
        0x23,0xdc,0xcf,0xf1,0x2e,0x65,0x39,0xa3,0xd9,0x6e,0x63,0x41,0x1f,0x26,0x9c,0x94,
        0x6d,0x22,0x9e,0x60,0xe4,0xce,0x8f,0xa7,0xde,0x86,0x49,0x1d,0x06,0x4d,0x79,0x39,
        0x90,0x15,0x8a,0x18,0x0a,0xee,0x4c,0xff,0xe6,0x04,0x5e,0xd5,0x81,0x36,0x8b,0x51,
        0xc9,0x93,0x14,0x5f,0x4e,0x21,0xf5,0xdf,0x15,0x9a,0xdc,0xaa,0xcc,0xc9,0x9c,0xbf,
        0x19,0x11,0x93,0x57,0x91,0xac,0xd8,0x84,0x41,0x7c,0x01,0x2b,0x90,0x95,0x93,0x18,
        0x04,0x1f,0x74,0xfe,0xe8,0x04,0xe2,0xfc,0x9f,0x3a,0xfb,0x5e,0xb9,0x44,0x5c,0x8b,
        0x41,0xdf,0x05,0x36,0x03,0x70,0xe0,0x38,0x49,0x1d,0x78,0xca,0x05,0xf3,0xd5,0x96,
        0x1c,0xe9,0x1a,0xfe,0x8d,0x42,0x06,0xd0,0xea,0x8a,0x41,0x65,0x12,0x25,0x50,0x5a,
        0x96,0x18,0x6c,0x0d,0x36,0x94,0xeb,0x00,0x7d,0xe6,0xfb,0x37,0x03,0x73,0xe1,0x0d,
        0xac,0x16,0x19,0x9a,0x64,0xc0,0xda,0x79,0xc5,0xdb,0xe9,0x18,0x86,0x91,0xd1,0x7a,
        0x9c,0x18,0xcf,0xf6,0xb5,0x0f,0x4d,0xf9,0x1f,0xd1,0x61,0x79,0x71,0x85,0x08,0x96,
        0x1a,0xac,0x40,0x33,0xcd,0x86,0xd3,0x47,0x5c,0x41,0x79,0xa1,0x75,0x68,0x9c,0x17,
        0x5a,0xe6,0x22,0x1f,0xe6,0xb9,0x13,0xd2,0xe5,0x8e,0x79,0xa5,0xeb,0x94,0x1a,0x4a,
        0x75,0xb1,0xb7,0x2f,0x17,0x23,0xfe,0xe2,0x82,0xa9,0x63,0xdf,0x15,0x1e,0x44,0x95,
        0x39,0x3c,0xdb,0xf9,0x7c,0xb1,0x87,0x41,0x0c,0x11,0x27,0xd5,0xb3,0x94,0x1a,0xe9,
        0x4f,0xfc,0xb1,0xd0,0xc2,0x2f,0x88,0x69,0x6f,0x89,0x04,0xc4,0x50,0x00,0x02,0x94,
        0xf0,0x90,0x18,0xaa,0x06,0xe8,0xf0,0xe1,0x01,0xdf,0x00,0x21,0x26,0x49,0x73,0xb9,
        0x49,0x0c,0xf5,0x62,0xc1,0x6f,0xd1,0x17,0x39,0xd8,0x70,0x3c,0xdb,0x85,0xcb,0x9d,
        0x89,0x67,0x82,0xc1,0xe9,0x85,0xd7,0x6e,0xb9,0xfb,0xfb,0x17,0x3d,0x48,0xd0,0x4f,
        0x51,0xcb,0xe5,0x95,0xfa,0x4d,0x5d,0xe9,0xad,0xed,0x65,0x4e,0xf1,0x1b,0x0e,0x97,
        0x3c,0xe4,0x03,0xfe,0x57,0x18,0xe7,0xf5,0x16,0x21,0x6e,0xe9,0xb7,0x23,0xa1,0x5e,
        0xf1,0xe5,0xda,0x96,0x39,0xa6,0x67,0x20,0xd0,0xa2,0x04,0xcf,0x2f,0xfd,0x7f,0xc1,
        0x85,0x38,0x27,0x6d,0xc1,0xe9,0xf2,0x14,0x38,0xb3,0x2a,0xae,0xe9,0x8a,0xca,0x23,
        0xad,0x71,0x65,0xa9,0x2d,0x94,0xc9,0x5a,0xb1,0x05,0x80,0x30,0x30,0x40,0x0a,0x01,
        0x00,0x35,0xc4,0x9c,0x05,0x16,0x0a,0x80,0x71,0x5d,0xe3,0xa7,0x1c,0x9c,0x05,0xf7,
        0x1c,0x05,0x8f,0x4a,0xc9,0xa1,0x81,0x97,0x0d,0x1e,0x68,0x41,0xa1,0xb5,0xd5,0x14,
        0x29,0x93,0x18,0xc3,0xf6,0xdb,0x23,0x6c,0x00,0x44,0xe4,0xc3,0xfc,0x15,0x0c,0x6d,
        0xdd,0xf6,0xcc,0x82,0xff,0x33,0xb1,0x1a,0x07,0x80,0x3e,0xa0,0x40,0xc2,0x0d,0xb9,
        0xe4,0x15,0x0c,0xca,0xf8,0xf9,0xb8,0x02,0xfb,0xd5,0x5f,0x9d,0x09,0xe3,0xbf,0xd9,
        0x1d,0x43,0xe1,0x02,0x00,0xa8,0xc5,0x17,0x90,0x0a,0x4d,0x86,0x45,0xee,0xa0,0xe6,
        0x20,0xe0,0x6f,0x17,0x26,0x95,0x79,0xd8,0x63,0x00,0xa6,0xe6,0xb7,0xc1,0x12,0x0a,
        0x54,0x07,0x4d,0xec,0xac,0xe6,0x00,0x00,
    };
    // Delta epoch (TOW 434751000, differential against the reference), length 296 bytes.
    static const uint8_t kDeltaBlock[] = {
        0x24,0x40,0x39,0xb6,0x0d,0x10,0x28,0x01,0x18,0xc6,0xe9,0x19,0x75,0x09,0x01,0x40,
        0x1f,0x00,0x50,0x00,0x04,0x87,0x0a,0x00,0xc9,0xe6,0xf8,0xc5,0x7d,0x0e,0xa0,0x7d,
        0x32,0xa0,0x7e,0x76,0xf5,0x21,0x80,0x06,0xa0,0x80,0x16,0x08,0x6e,0x78,0x1e,0xa0,
        0x85,0xf2,0x9f,0x84,0x14,0x00,0x00,0x00,0x3f,0x28,0x34,0xf1,0x8a,0x00,0xf6,0x01,
        0x2e,0x83,0x26,0xa2,0x70,0x6e,0x9f,0x7d,0xba,0xfb,0xc5,0x76,0x72,0xa1,0x8c,0xfe,
        0x9d,0x84,0xf2,0x0e,0x2a,0x67,0x6e,0x9c,0x87,0xe6,0x9f,0x85,0xa6,0xfc,0x81,0x7b,
        0xb2,0x9f,0x7f,0x1e,0xa0,0x82,0x86,0x04,0xee,0x7e,0xba,0xdf,0x83,0x0a,0xa0,0x84,
        0x26,0x02,0xb6,0x80,0x36,0xa1,0x81,0x02,0xa0,0x7f,0x00,0x86,0xff,0xb5,0x84,0x7e,
        0x9d,0x84,0x86,0xfd,0xfd,0x81,0xca,0x9e,0x84,0xd6,0x07,0xde,0x76,0x7e,0xa1,0x8b,
        0x4a,0x04,0x76,0x6b,0x5a,0x9c,0x84,0x46,0xfe,0xed,0x74,0x22,0xa1,0x86,0xf6,0x06,
        0x66,0x78,0x12,0x9f,0x83,0x16,0x00,0xfa,0x81,0xa2,0xa1,0x82,0xd6,0xf2,0xc1,0x7e,
        0x6e,0xa0,0x81,0x26,0xfd,0x8d,0x77,0x36,0xa3,0x8f,0x00,0xa6,0x01,0x02,0x82,0x82,
        0xa0,0x7d,0xa2,0x9f,0x78,0x26,0xfe,0x75,0x81,0xde,0x9f,0x86,0x1a,0xa0,0x88,0xa6,
        0xfb,0x45,0x81,0xfa,0x9f,0x80,0xee,0x9f,0x82,0x96,0xf8,0x69,0x81,0xb2,0x9f,0x7e,
        0xca,0x9f,0x7e,0x66,0x05,0xea,0x82,0xc2,0x9f,0x72,0xca,0x9f,0x74,0x36,0xfb,0x99,
        0x83,0x96,0xa0,0x7f,0xfa,0x9f,0x85,0x00,0xe6,0xf3,0x89,0x94,0xb6,0x2c,0x46,0x45,
        0xa6,0x05,0x7a,0x7d,0x22,0x01,0x4a,0x80,0x56,0x05,0x66,0x7e,0x26,0x07,0x8a,0x80,
        0x56,0xf6,0x01,0x86,0x56,0xf6,0x6d,0x7d,0x00,0xb6,0x25,0xca,0x5c,0x96,0xe6,0xad,
        0xb1,0x56,0x05,0x5a,0x89,0x00,0x00,0x00,
    };

    Meas3Decoder::instance().reset();   // start from a clean reference cache

    // --- reference epoch ---
    SbfParser pref;
    ASSERT_TRUE(pref.parseFrame(kRefBlock, sizeof(kRefBlock)));
    EXPECT_EQ(pref.lastBlockNumber(), 4109);
    const auto& ref = pref.getObservations();
    ASSERT_FALSE(ref.empty());
    EXPECT_EQ(ref.front().time.week, 2421);
    EXPECT_NEAR(ref.front().time.tow, 434750.0, 1e-6);

    expectObs(ref, "G01", "1C", 21217557.075, 111499089.826);  // GPS L1CA master
    expectObs(ref, "G01", "2W", 21217556.984,  86882432.440);  // GPS L2P  slave
    expectObs(ref, "E08", "1C", 23364185.160, 122779752.267);  // Galileo E1
    expectObs(ref, "E08", "5Q", 23364187.102,  91686295.675);  // Galileo E5a
    expectObs(ref, "C14", "2I", 22705418.270, 118233186.158);  // BeiDou B1I
    expectObs(ref, "R06", "1C", 19666439.532, 104944043.648);  // GLONASS L1CA (FDMA)

    // --- delta epoch (decoded against the cached reference: a new parser over the
    //     shared singleton state, as in per-frame real-time use) ---
    SbfParser pdelta;
    ASSERT_TRUE(pdelta.parseFrame(kDeltaBlock, sizeof(kDeltaBlock)));
    const auto& dlt = pdelta.getObservations();
    ASSERT_FALSE(dlt.empty());
    EXPECT_NEAR(dlt.front().time.tow, 434751.0, 1e-6);

    expectObs(dlt, "G01", "1C", 21216867.809, 111495467.565);
    expectObs(dlt, "G01", "2W", 21216867.702,  86879609.897);
    expectObs(dlt, "E08", "1C", 23363755.714, 122777495.594);
    expectObs(dlt, "E08", "5Q", 23363757.670,  91684610.498);
    expectObs(dlt, "C14", "2I", 22704725.432, 118229578.207);
    expectObs(dlt, "R06", "1C", 19666021.508, 104941813.119);
}

TEST(SubframeAssemblerTest, IncompleteSetReturnsNullopt) {
    // A single (bogus) GPS subframe can never complete an ephemeris.
    RawNavMessage m;
    m.sat = "G05";
    m.system = System::GPS;
    m.crc_passed = true;
    m.words.assign(10, 0x10000000u);

    SubframeAssembler a;
    EXPECT_FALSE(a.consume(m).has_value());

    // A message whose CRC failed is ignored entirely.
    m.crc_passed = false;
    EXPECT_FALSE(a.consume(m).has_value());
}

} // namespace
} // namespace GnssProcessor
