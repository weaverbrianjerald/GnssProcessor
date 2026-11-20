/**
 * @file test_sbf_galileo_bgd.cpp
 * @brief The two Galileo broadcast group delays land in the documented slots on every reader.
 *
 * KeplerianEphemeris stores Galileo BGD(E1, E5a) in @c tgd0 and BGD(E1, E5b) in @c tgd1
 * (include/products/BroadcastEphemerisTypes.hpp).  The SBF readers once stored them the
 * other way round, while the RINEX navigation parser and the RTCM 1046 decoder followed the
 * documentation, so one satellite carried different group delays in one slot depending on
 * the format it arrived in.  These cases build each input with two distinct delays and check
 * the slots:
 *   - SBF GALNav (4002): BGD_L1E5a at block offset 136, BGD_L1E5b at 140 (f4, seconds), as
 *     the Septentrio reference guide lays the block out; an F/NAV-sourced block writes the
 *     f4 Do-Not-Use value (-2e10) in the E1-E5b field, which is stored as an absent delay (0);
 *   - SBF GALRawINAV (4023) through SubframeAssembler: I/NAV word type 5 carries
 *     BGD(E1, E5a) at word bits 47..56 and BGD(E1, E5b) at 57..66, 10-bit two's complement
 *     at 2^-32 s (Galileo OS SIS ICD, word type 5);
 *   - the cross-reader case: one pair of delays through RINEX navigation (broadcast orbit 6,
 *     fields 3 and 4), RTCM 1046 (DF312, DF313), SBF GALNav and the I/NAV word 5 gives the
 *     same two slots on all four.  RTCM 1045 (F/NAV) is not decoded by this project, so it has
 *     no reader to compare.
 *
 * The delays are integer multiples of 2^-32 s, exact in the f4 field, in the 10-bit field and
 * in the RTCM fields; the RINEX text carries 13 significant digits, so the RINEX comparison
 * uses a tolerance far below the 2^-32 s step.
 */

#include <gtest/gtest.h>

#include "products/BroadcastEphemerisTypes.hpp"
#include "products/RinexNavParser.hpp"
#include "rtcm/RtcmNavDecoder.hpp"
#include "sbf/SbfParser.hpp"
#include "sbf/SubframeAssembler.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// The two group delays every case uses, as counts of the 2^-32 s step.  Distinct and of
// opposite sign, so a swap or a lost sign cannot pass.
constexpr int kBgdE5aCounts = 13;     // BGD(E1, E5a) =  13 * 2^-32 s (about  3.03 ns)
constexpr int kBgdE5bCounts = -21;    // BGD(E1, E5b) = -21 * 2^-32 s (about -4.89 ns)
const double kBgdE5a = std::ldexp(static_cast<double>(kBgdE5aCounts), -32);
const double kBgdE5b = std::ldexp(static_cast<double>(kBgdE5bCounts), -32);

constexpr int    kWnc   = 2420;       // GPS week of every record
constexpr double kTowS  = 108000.0;   // seconds of week of every record
constexpr double kToeS  = 106800.0;   // toe = toc
constexpr int    kIodNav = 51;
constexpr double kSqrtA = 5440.607572555542;

// ---------------------------------------------------------------------------
// SBF framing (the same CRC and layout SbfStreamReader checks)
// ---------------------------------------------------------------------------

// CRC-16/CCITT-XMODEM (poly 0x1021, init 0x0000).
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

// A complete SBF block from a body (the bytes from TOW onward, block offset 8).
std::vector<uint8_t> buildSbfBlock(int blockNumber, std::vector<uint8_t> body) {
    std::size_t total = 8 + body.size();
    while (total % 4 != 0) { body.push_back(0); total = 8 + body.size(); }
    const uint16_t id = static_cast<uint16_t>(blockNumber & 0x1FFF);
    std::vector<uint8_t> block(8, 0);
    block[0] = 0x24; block[1] = 0x40;
    block[4] = id & 0xFF;          block[5] = (id >> 8) & 0xFF;
    block[6] = total & 0xFF;       block[7] = (total >> 8) & 0xFF;
    block.insert(block.end(), body.begin(), body.end());
    const uint16_t crc = crc16Xmodem(block.data() + 4, total - 4);
    block[2] = crc & 0xFF; block[3] = (crc >> 8) & 0xFF;
    return block;
}

void wrU8 (std::vector<uint8_t>& b, std::size_t o, uint8_t  v) { b[o] = v; }
void wrU16(std::vector<uint8_t>& b, std::size_t o, uint16_t v) { std::memcpy(b.data() + o, &v, 2); }
void wrU32(std::vector<uint8_t>& b, std::size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
void wrF32(std::vector<uint8_t>& b, std::size_t o, float    v) { std::memcpy(b.data() + o, &v, 4); }
void wrF64(std::vector<uint8_t>& b, std::size_t o, double   v) { std::memcpy(b.data() + o, &v, 8); }

/// A GALNav (4002) block of E11 with the two group delays given as f4 values.  Body
/// offsets are block offsets minus 8.
std::vector<uint8_t> galNavBlock(float bgd_e5a, float bgd_e5b) {
    std::vector<uint8_t> body(144, 0);                   // block offsets 8..151
    wrU32(body, 0, static_cast<uint32_t>(kTowS * 1000.0));   // TOW [ms]       (block 8)
    wrU16(body, 4, static_cast<uint16_t>(kWnc));             // WNc            (block 12)
    wrU8 (body, 6, 81);                                      // SVID 81 -> E11 (block 14)
    wrU8 (body, 7, 2);                                       // source: I/NAV  (block 15)
    wrF64(body, 8, kSqrtA);                                  // sqrt(A)        (block 16)
    wrU32(body, 92, static_cast<uint32_t>(kToeS));           // toe            (block 100)
    wrU32(body, 96, static_cast<uint32_t>(kToeS));           // toc            (block 104)
    wrU16(body, 120, static_cast<uint16_t>(kIodNav));        // IODnav         (block 128)
    wrF32(body, 128, bgd_e5a);                               // BGD_L1E5a      (block 136)
    wrF32(body, 132, bgd_e5b);                               // BGD_L1E5b      (block 140)
    return buildSbfBlock(4002, body);
}

// ---------------------------------------------------------------------------
// Galileo I/NAV pages (GALRawINAV 4023 NAVBits) for SubframeAssembler
// ---------------------------------------------------------------------------

// Write @p len bits of @p value MSB-first at bit @p pos (two's complement for a negative value).
void setBits(uint8_t* buf, int pos, int len, int64_t value) {
    const uint64_t v = static_cast<uint64_t>(value);
    for (int i = 0; i < len; ++i) {
        const int bit = pos + i;
        const bool on = ((v >> (len - 1 - i)) & 1u) != 0;
        if (on) buf[bit / 8] |=  static_cast<uint8_t>(1u << (7 - bit % 8));
        else    buf[bit / 8] &= static_cast<uint8_t>(~(1u << (7 - bit % 8)));
    }
}
int getBit(const uint8_t* buf, int pos) { return (buf[pos / 8] >> (7 - pos % 8)) & 1; }

/// The NAVBits of one nominal I/NAV page holding the 128-bit word @p word, in the layout
/// SubframeAssembler reads: even part (even/odd 0, page type 0, word bits 0..111) at page
/// bits 0..113, odd part (even/odd 1, page type 0, word bits 112..127) from page bit 114.
RawNavMessage inavPage(const uint8_t (&word)[16]) {
    uint8_t page[32] = {0};
    setBits(page, 0, 1, 0);
    setBits(page, 1, 1, 0);
    for (int j = 0; j < 112; ++j) setBits(page, 2 + j, 1, getBit(word, j));
    setBits(page, 114, 1, 1);
    setBits(page, 115, 1, 0);
    for (int j = 0; j < 16; ++j) setBits(page, 116 + j, 1, getBit(word, 112 + j));

    RawNavMessage m;
    m.sat        = "E11";
    m.system     = System::Galileo;
    m.signal     = 17;                                   // E1-B
    m.crc_passed = true;
    m.wnc        = kWnc;
    m.tow_s      = kTowS;
    for (int k = 0; k < 8; ++k) {
        uint32_t w = 0;
        for (int j = 0; j < 4; ++j) w = (w << 8) | page[4 * k + j];
        m.words.push_back(w);
    }
    return m;
}

/// I/NAV word types 1..4 of one IODnav (the fields the assembler needs to emit), and
/// word type 5 with the two group delays in 2^-32 s counts.
RawNavMessage inavWord(int type, int bgd_e5a_counts = 0, int bgd_e5b_counts = 0) {
    uint8_t w[16] = {0};
    setBits(w, 0, 6, type);
    if (type >= 1 && type <= 4) setBits(w, 6, 10, kIodNav);
    if (type == 1) {
        setBits(w, 16, 14, static_cast<int64_t>(kToeS / 60.0));                       // toe
        setBits(w, 94, 32, static_cast<int64_t>(std::ldexp(kSqrtA, 19)));             // sqrt(A)
    }
    if (type == 4) setBits(w, 54, 14, static_cast<int64_t>(kToeS / 60.0));           // toc
    if (type == 5) {
        setBits(w, 47, 10, bgd_e5a_counts);                                           // BGD(E1, E5a)
        setBits(w, 57, 10, bgd_e5b_counts);                                           // BGD(E1, E5b)
    }
    return inavPage(w);
}

/// Feed word type 5 then 1..4 to a fresh assembler; returns the ephemeris word 4 completes.
std::optional<BroadcastEphemeris> assembleInav(int bgd_e5a_counts, int bgd_e5b_counts) {
    SubframeAssembler a;
    std::optional<BroadcastEphemeris> eph = a.consume(inavWord(5, bgd_e5a_counts, bgd_e5b_counts));
    for (int t = 1; t <= 4; ++t) {
        if (eph) return std::nullopt;                    // completed before word type 4
        eph = a.consume(inavWord(t));
    }
    return eph;
}

// ---------------------------------------------------------------------------
// RTCM 1046 (Galileo I/NAV ephemeris) and RINEX navigation text
// ---------------------------------------------------------------------------

// MSB-first bit writer for an RTCM payload.
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

/// The payload of an RTCM 1046 message of E11 (DF002 included) with the two delays.
std::vector<uint8_t> rtcm1046Payload(int bgd_e5a_counts, int bgd_e5b_counts) {
    BitWriter pw;
    pw.put(12, 1046);                                    // DF002
    pw.put(6, 11);                                       // DF252 satellite -> E11
    pw.put(12, kWnc - 1024);                             // DF289 GST week
    pw.put(10, kIodNav);                                 // DF290 IODnav
    pw.put(8, 0);                                        // DF291 SISA
    pw.putSigned(14, 0);                                 // DF292 IDOT
    pw.put(14, static_cast<uint64_t>(kToeS / 60.0));     // DF293 toc
    pw.putSigned(6, 0);                                  // DF294 af2
    pw.putSigned(21, 0);                                 // DF295 af1
    pw.putSigned(31, 0);                                 // DF296 af0
    pw.putSigned(16, 0);                                 // DF297 Crs
    pw.putSigned(16, 0);                                 // DF298 delta n
    pw.putSigned(32, 0);                                 // DF299 M0
    pw.putSigned(16, 0);                                 // DF300 Cuc
    pw.put(32, 0);                                       // DF301 e
    pw.putSigned(16, 0);                                 // DF302 Cus
    pw.put(32, static_cast<uint64_t>(std::ldexp(kSqrtA, 19)));   // DF303 sqrt(A)
    pw.put(14, static_cast<uint64_t>(kToeS / 60.0));     // DF304 toe
    pw.putSigned(16, 0);                                 // DF305 Cic
    pw.putSigned(32, 0);                                 // DF306 Omega0
    pw.putSigned(16, 0);                                 // DF307 Cis
    pw.putSigned(32, 0);                                 // DF308 i0
    pw.putSigned(16, 0);                                 // DF309 Crc
    pw.putSigned(32, 0);                                 // DF310 omega
    pw.putSigned(24, 0);                                 // DF311 Omega dot
    pw.putSigned(10, bgd_e5a_counts);                    // DF312 BGD E1/E5a
    pw.putSigned(10, bgd_e5b_counts);                    // DF313 BGD E1/E5b
    pw.put(2, 0);                                        // DF314 E5b signal health
    pw.put(1, 0);                                        // DF315 E5b data validity
    pw.put(2, 0);                                        // DF316 E1-B signal health
    pw.put(1, 0);                                        // DF317 E1-B data validity
    while (pw.bitCount() % 8) pw.put(1, 0);
    return pw.bytes();
}

// A 19-column RINEX D-field ("% .12E"), as the RINEX navigation parser tests write it.
std::string navField(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "% .12E", v);
    std::string s(buf);
    if (s.size() > 19) {
        const auto e = s.find_first_of("Ee");
        if (e != std::string::npos && s.size() - e == 5 && s[e + 2] == '0')
            s.erase(e + 2, 1);                           // "E+002" -> "E+02"
    }
    if (s.size() < 19) s = std::string(19 - s.size(), ' ') + s;
    return s.substr(0, 19);
}
std::string navLine(std::initializer_list<double> vals) {
    std::string s = "    ";
    for (double v : vals) s += navField(v);
    return s + "\n";
}

/// A RINEX 3.04 navigation file holding one Galileo I/NAV record of E11 whose broadcast
/// orbit 6 carries the two delays (field 3 BGD E5a/E1, field 4 BGD E5b/E1).  The record's
/// clock epoch is 2026-05-25 05:40:00 GPS, which is week 2420, 106800 s.
std::string rinexNav(double bgd_e5a, double bgd_e5b) {
    std::string s =
        "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
        "                                                            END OF HEADER\n";
    s += "E11 2026 05 25 05 40 00" + navField(0.0) + navField(0.0) + navField(0.0) + "\n";
    s += navLine({ static_cast<double>(kIodNav), 0.0, 0.0, 0.0 });   // ORBIT-1: IODnav, Crs, dn, M0
    s += navLine({ 0.0, 0.0, 0.0, kSqrtA });                         // ORBIT-2: Cuc, e, Cus, sqrt(A)
    s += navLine({ kToeS, 0.0, 0.0, 0.0 });                          // ORBIT-3: toe, Cic, Omega0, Cis
    s += navLine({ 0.0, 0.0, 0.0, 0.0 });                            // ORBIT-4: i0, Crc, omega, Omega dot
    s += navLine({ 0.0, 517.0, static_cast<double>(kWnc), 0.0 });    // ORBIT-5: IDOT, data source, week
    s += navLine({ 3.12, 0.0, bgd_e5a, bgd_e5b });                   // ORBIT-6: SISA, health, BGDs
    s += navLine({ kTowS, 0.0 });                                    // ORBIT-7: transmission time
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// SBF GALNav (4002): offset 136 is BGD(E1, E5a) and lands in tgd0, offset 140 is
// BGD(E1, E5b) and lands in tgd1.  Fails if the slots are swapped again.
// ---------------------------------------------------------------------------
TEST(SbfGalileoBgd, GalNavBlockStoresE1E5aInTgd0AndE1E5bInTgd1) {
    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(galNavBlock(static_cast<float>(kBgdE5a),
                                              static_cast<float>(kBgdE5b))));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const BroadcastEphemeris& eph = parser.getBroadcastEphemeris().front();
    EXPECT_EQ(eph.satellite, "E11");
    EXPECT_EQ(eph.iode, kIodNav);
    EXPECT_DOUBLE_EQ(eph.kepler.tgd0, kBgdE5a);
    EXPECT_DOUBLE_EQ(eph.kepler.tgd1, kBgdE5b);
}

// An F/NAV-sourced GALNav block carries no E1-E5b delay: the receiver writes the f4
// Do-Not-Use value (-2e10) there, which is stored as an absent delay (0), never as a
// group delay of -2e10 s.
TEST(SbfGalileoBgd, GalNavDoNotUseDelayIsStoredAsAbsent) {
    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(galNavBlock(static_cast<float>(kBgdE5a), -2.0e10f)));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const BroadcastEphemeris& eph = parser.getBroadcastEphemeris().front();
    EXPECT_DOUBLE_EQ(eph.kepler.tgd0, kBgdE5a);
    EXPECT_EQ(eph.kepler.tgd1, 0.0);
}

// ---------------------------------------------------------------------------
// I/NAV word type 5 through SubframeAssembler: bits 47..56 are BGD(E1, E5a) and land in
// tgd0, bits 57..66 are BGD(E1, E5b) and land in tgd1, both signed.
// ---------------------------------------------------------------------------
TEST(SbfGalileoBgd, InavWordFiveStoresE1E5aInTgd0AndE1E5bInTgd1) {
    const std::optional<BroadcastEphemeris> eph = assembleInav(kBgdE5aCounts, kBgdE5bCounts);
    ASSERT_TRUE(eph.has_value());
    EXPECT_EQ(eph->satellite, "E11");
    EXPECT_EQ(eph->iode, kIodNav);
    EXPECT_DOUBLE_EQ(eph->kepler.tgd0, kBgdE5a);
    EXPECT_DOUBLE_EQ(eph->kepler.tgd1, kBgdE5b);
    EXPECT_DOUBLE_EQ(eph->kepler.sqrt_a, kSqrtA);        // the synthetic set decoded as written
    EXPECT_DOUBLE_EQ(eph->kepler.toe.tow, kToeS);
}

// ---------------------------------------------------------------------------
// One pair of delays through every reader that can be driven without a data file: RINEX
// navigation, RTCM 1046, SBF GALNav and the SBF I/NAV word 5 give the same two slots.
// ---------------------------------------------------------------------------
TEST(SbfGalileoBgd, EveryReaderStoresTheDelaysInTheSameSlots) {
    // RINEX navigation (broadcast orbit 6, fields 3 and 4).
    const std::vector<BroadcastEphemeris> rnx =
        RinexNavParser::parseString(rinexNav(kBgdE5a, kBgdE5b));
    ASSERT_EQ(rnx.size(), 1u);
    ASSERT_EQ(rnx.front().satellite, "E11");

    // RTCM 1046 (DF312, DF313), anchored on the record's own week.
    std::vector<BroadcastEphemeris> rtcm;
    const std::vector<uint8_t> payload = rtcm1046Payload(kBgdE5aCounts, kBgdE5bCounts);
    const double anchor_s = static_cast<double>(kWnc) * 604800.0 + kTowS;
    ASSERT_TRUE(RtcmNav::decodeBroadcast(payload.data(), payload.size(), 1046, anchor_s, rtcm).ok);
    ASSERT_EQ(rtcm.size(), 1u);
    ASSERT_EQ(rtcm.front().satellite, "E11");
    // RTCM 1045 (Galileo F/NAV) has no decoder here, so it has no slot to compare.
    EXPECT_FALSE(RtcmNav::isBroadcastEphemerisType(1045));

    // SBF GALNav and the SBF I/NAV word 5.
    SbfParser parser;
    ASSERT_TRUE(parser.parseFrame(galNavBlock(static_cast<float>(kBgdE5a),
                                              static_cast<float>(kBgdE5b))));
    ASSERT_EQ(parser.getBroadcastEphemeris().size(), 1u);
    const BroadcastEphemeris galnav = parser.getBroadcastEphemeris().front();
    const std::optional<BroadcastEphemeris> inav = assembleInav(kBgdE5aCounts, kBgdE5bCounts);
    ASSERT_TRUE(inav.has_value());

    struct Reader { const char* name; const BroadcastEphemeris* eph; double tol; };
    const Reader readers[] = {
        { "RINEX navigation", &rnx.front(), 1e-20 },     // 13 significant digits of text
        { "RTCM 1046",        &rtcm.front(), 0.0 },
        { "SBF GALNav",       &galnav,       0.0 },
        { "SBF I/NAV word 5", &*inav,        0.0 },
    };
    for (const Reader& r : readers) {
        SCOPED_TRACE(r.name);
        EXPECT_NEAR(r.eph->kepler.tgd0, kBgdE5a, r.tol) << "tgd0 is BGD(E1, E5a)";
        EXPECT_NEAR(r.eph->kepler.tgd1, kBgdE5b, r.tol) << "tgd1 is BGD(E1, E5b)";
    }
}

}  // namespace GnssProcessor
