/**
 * @file SubframeAssembler.cpp
 * @brief Raw navigation subframe -> BroadcastEphemeris assembly.
 *
 * Bit primitives (getbitu/getbits/setbitu/getbits2/getbitg) mirror RTKLIB
 * rtkcmn.c exactly; the per-constellation field offsets follow the
 * RTKLIB-grounded research spec (IS-GPS-200, Galileo OS SIS ICD, BDS-SIS-ICD).
 * Angular elements are converted from semicircles to radians (× π) to match
 * the engine's KeplerianEphemeris convention; BeiDou toe/toc stay on native
 * BDT.  The emitted struct is identical in shape to the decoded-Nav decoders,
 * so LiveEphemerisStore consumes it unchanged.
 */

#include "sbf/SubframeAssembler.hpp"
#include "common/Constants.hpp"
#include <cmath>

namespace GnssProcessor {

namespace {

constexpr double kPi = Constants::PI;            // semicircles -> radians
inline double P2(int n) { return std::ldexp(1.0, -n); }   // 2^-n

// --- MSB-first bit primitives over a byte buffer (RTKLIB rtkcmn.c) ---
uint32_t getbitu(const uint8_t* buff, int pos, int len) {
    uint32_t bits = 0;
    for (int i = pos; i < pos + len; ++i)
        bits = (bits << 1) + ((buff[i / 8] >> (7 - i % 8)) & 1u);
    return bits;
}
int32_t getbits(const uint8_t* buff, int pos, int len) {
    const uint32_t bits = getbitu(buff, pos, len);
    if (len <= 0 || 32 <= len || !(bits & (1u << (len - 1))))
        return static_cast<int32_t>(bits);
    return static_cast<int32_t>(bits | (~0u << len));     // two's-complement sign-extend
}
void setbitu(uint8_t* buff, int pos, int len, uint32_t data) {
    uint32_t mask = 1u << (len - 1);
    for (int i = pos; i < pos + len; ++i, mask >>= 1) {
        if (data & mask) buff[i / 8] |=  (1u << (7 - i % 8));
        else             buff[i / 8] &= ~(1u << (7 - i % 8));
    }
}
// Merge two split unsigned fields (high piece first).
uint32_t getbitu2(const uint8_t* buff, int p1, int l1, int p2, int l2) {
    return (getbitu(buff, p1, l1) << l2) + getbitu(buff, p2, l2);
}
// Merge two split signed fields, sign per the high piece over the combined width.
int32_t getbits2(const uint8_t* buff, int p1, int l1, int p2, int l2) {
    if (getbitu(buff, p1, 1))
        return static_cast<int32_t>((getbits(buff, p1, l1) << l2) + getbitu(buff, p2, l2));
    return static_cast<int32_t>(getbitu2(buff, p1, l1, p2, l2));
}

// Resolve a full GPS/GST week from the block WNc + the toe rollover guard.
int resolveWeek(int wnc, double tow_s, double toes) {
    if (wnc <= 0) return 0;
    int week = wnc;
    if (toes - tow_s >  302400.0) week -= 1;
    else if (tow_s - toes > 302400.0) week += 1;
    return week;
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

std::optional<BroadcastEphemeris> SubframeAssembler::consume(const RawNavMessage& msg) {
    if (!msg.crc_passed) return std::nullopt;
    auto& sat = by_sat_[msg.sat];
    switch (msg.system) {
    case System::GPS:
    case System::QZSS:    return consumeGps(msg, sat);
    case System::Galileo: return consumeGal(msg, sat);
    case System::BeiDou:  return consumeBds(msg, sat);
    default:              return std::nullopt;   // GLONASS raw assembly: follow-up
    }
}

// ---------------------------------------------------------------------------
// GPS / QZSS LNAV  (subframes 1+2+3)
// ---------------------------------------------------------------------------

std::optional<BroadcastEphemeris> SubframeAssembler::consumeGps(const RawNavMessage& msg, PerSat& sat) {
    if (msg.words.size() < 10) return std::nullopt;

    // Pack 10 words, stripping the 6 parity bits per word -> 240-bit buffer.
    uint8_t buf[30] = {0};
    for (int i = 0; i < 10; ++i) setbitu(buf, 24 * i, 24, msg.words[i] >> 6);

    const int id = getbitu(buf, 43, 3);          // HOW subframe ID
    if (id < 1 || id > 3) return std::nullopt;    // only SF1/2/3 carry ephemeris
    sat.slots[id].assign(buf, buf + 30);

    if (!sat.slots.count(1) || !sat.slots.count(2) || !sat.slots.count(3))
        return std::nullopt;

    const uint8_t* b1 = sat.slots[1].data();
    const uint8_t* b2 = sat.slots[2].data();
    const uint8_t* b3 = sat.slots[3].data();

    // --- Subframe 1 (clock / health) ---
    const int    svh   = static_cast<int>(getbitu(b1, 64, 6));
    const int    iodc  = static_cast<int>((getbitu(b1, 70, 2) << 8) | getbitu(b1, 168, 8));
    const int    tgdr  = getbits(b1, 160, 8);
    const double tgd0  = (tgdr == -128) ? 0.0 : tgdr * P2(31);
    const double toc   = getbitu(b1, 176, 16) * 16.0;
    const double af2   = getbits(b1, 192, 8)  * P2(55);
    const double af1   = getbits(b1, 200, 16) * P2(43);
    const double af0   = getbits(b1, 216, 22) * P2(31);

    // --- Subframe 2 (Keplerian set 1) ---
    const int    iode2 = static_cast<int>(getbitu(b2, 48, 8));
    const double crs   = getbits(b2, 56, 16)  * P2(5);
    const double dn    = getbits(b2, 72, 16)  * P2(43) * kPi;
    const double m0    = getbits(b2, 88, 32)  * P2(31) * kPi;
    const double cuc   = getbits(b2, 120, 16) * P2(29);
    const double e     = getbitu(b2, 136, 32) * P2(33);
    const double cus   = getbits(b2, 168, 16) * P2(29);
    const double sqrtA = getbitu(b2, 184, 32) * P2(19);
    const double toes  = getbitu(b2, 216, 16) * 16.0;

    // --- Subframe 3 (Keplerian set 2) ---
    const double cic    = getbits(b3, 48, 16)  * P2(29);
    const double omega0 = getbits(b3, 64, 32)  * P2(31) * kPi;
    const double cis    = getbits(b3, 96, 16)  * P2(29);
    const double i0     = getbits(b3, 112, 32) * P2(31) * kPi;
    const double crc    = getbits(b3, 144, 16) * P2(5);
    const double omega  = getbits(b3, 160, 32) * P2(31) * kPi;
    const double omdot  = getbits(b3, 192, 24) * P2(43) * kPi;
    const int    iode3  = static_cast<int>(getbitu(b3, 216, 8));
    const double idot   = getbits(b3, 224, 14) * P2(43) * kPi;

    // Consistency gate (RTKLIB): IODE(SF2)==IODE(SF3)==IODC&0xFF.
    if (iode2 != iode3 || iode2 != (iodc & 0xFF)) return std::nullopt;
    if (iode2 == sat.last_emitted_iode) return std::nullopt;     // already emitted this IODE

    BroadcastEphemeris eph;
    eph.satellite     = msg.sat;
    eph.system        = msg.system;
    eph.iode          = iode2;
    eph.health        = svh;
    eph.received_time = GnssTime{msg.wnc, msg.tow_s};
    const int week = resolveWeek(msg.wnc, msg.tow_s, toes);
    KeplerianEphemeris& k = eph.kepler;
    k.toe = GnssTime{week, toes};
    k.toc = GnssTime{week, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = omega0; k.omega_dot = omdot; k.omega = omega; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = tgd0;

    sat.last_emitted_iode = iode2;
    return eph;
}

// ---------------------------------------------------------------------------
// Galileo E1-B I/NAV  (word types 1+2+3+4, +5 for BGD/health)
// ---------------------------------------------------------------------------
std::optional<BroadcastEphemeris> SubframeAssembler::consumeGal(const RawNavMessage& msg, PerSat& sat) {
    if (msg.words.size() < 8) return std::nullopt;

    // STAGE A: pack the 256-bit page (even 120 + odd 120 + pad) and rebuild the
    // 128-bit logical word = even data (page bits 2..113) ++ odd data (122..137).
    uint8_t page[32] = {0};
    for (int k = 0; k < 8; ++k) setbitu(page, 32 * k, 32, msg.words[k]);
    if (getbitu(page, 1, 1) != 0) return std::nullopt;       // alert page — skip
    const int wt = static_cast<int>(getbitu(page, 2, 6));
    if (wt > 5) return std::nullopt;                          // only word types 0..5 wanted

    uint8_t word[16] = {0};
    for (int j = 0; j < 112; ++j) setbitu(word, j,        1, getbitu(page, 2 + j,   1));
    for (int j = 0; j < 16;  ++j) setbitu(word, 112 + j,  1, getbitu(page, 122 + j, 1));
    sat.slots[wt].assign(word, word + 16);

    if (!sat.slots.count(1) || !sat.slots.count(2) || !sat.slots.count(3) || !sat.slots.count(4))
        return std::nullopt;

    const uint8_t* w1 = sat.slots[1].data();
    const uint8_t* w2 = sat.slots[2].data();
    const uint8_t* w3 = sat.slots[3].data();
    const uint8_t* w4 = sat.slots[4].data();

    // IODnav must be common across WT1..WT4 (offsets are spec_bit − 128·wt).
    const int iodnav = static_cast<int>(getbitu(w1, 6, 10));
    if (static_cast<int>(getbitu(w2, 6, 10)) != iodnav ||
        static_cast<int>(getbitu(w3, 6, 10)) != iodnav ||
        static_cast<int>(getbitu(w4, 6, 10)) != iodnav) return std::nullopt;
    if (iodnav == sat.last_emitted_iode) return std::nullopt;

    const double toes  = getbitu(w1, 16, 14) * 60.0;
    const double m0    = getbits(w1, 30, 32) * P2(31) * kPi;
    const double e     = getbitu(w1, 62, 32) * P2(33);
    const double sqrtA = getbitu(w1, 94, 32) * P2(19);

    const double omega0 = getbits(w2, 16, 32)  * P2(31) * kPi;
    const double i0     = getbits(w2, 48, 32)  * P2(31) * kPi;
    const double omega  = getbits(w2, 80, 32)  * P2(31) * kPi;
    const double idot   = getbits(w2, 112, 14) * P2(43) * kPi;

    const double omdot = getbits(w3, 16, 24)  * P2(43) * kPi;
    const double dn    = getbits(w3, 40, 16)  * P2(43) * kPi;
    const double cuc   = getbits(w3, 56, 16)  * P2(29);
    const double cus   = getbits(w3, 72, 16)  * P2(29);
    const double crc   = getbits(w3, 88, 16)  * P2(5);
    const double crs   = getbits(w3, 104, 16) * P2(5);

    const double cic = getbits(w4, 22, 16) * P2(29);
    const double cis = getbits(w4, 38, 16) * P2(29);
    const double toc = getbitu(w4, 54, 14) * 60.0;
    const double af0 = getbits(w4, 68, 31) * P2(34);
    const double af1 = getbits(w4, 99, 21) * P2(46);
    const double af2 = getbits(w4, 120, 6) * P2(59);

    double tgd0 = 0.0, tgd1 = 0.0;
    int    svh  = 0;
    if (sat.slots.count(5)) {
        const uint8_t* w5 = sat.slots[5].data();
        tgd1 = getbits(w5, 47, 10) * P2(32);   // BGD E1-E5a
        tgd0 = getbits(w5, 57, 10) * P2(32);   // BGD E1-E5b
        const int e5b_hs  = static_cast<int>(getbitu(w5, 67, 2));
        const int e1b_hs  = static_cast<int>(getbitu(w5, 69, 2));
        const int e5b_dvs = static_cast<int>(getbitu(w5, 71, 1));
        const int e1b_dvs = static_cast<int>(getbitu(w5, 72, 1));
        svh = (e5b_hs << 7) | (e5b_dvs << 6) | (e1b_hs << 1) | e1b_dvs;
    }

    BroadcastEphemeris eph;
    eph.satellite     = msg.sat;
    eph.system        = System::Galileo;
    eph.iode          = iodnav;
    eph.health        = svh;
    eph.received_time = GnssTime{msg.wnc, msg.tow_s};
    const int week = resolveWeek(msg.wnc, msg.tow_s, toes);    // GST aligns with GPS week
    KeplerianEphemeris& k = eph.kepler;
    k.toe = GnssTime{week, toes};
    k.toc = GnssTime{week, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = omega0; k.omega_dot = omdot; k.omega = omega; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = tgd0; k.tgd1 = tgd1;

    sat.last_emitted_iode = iodnav;
    return eph;
}

// ---------------------------------------------------------------------------
// BeiDou D1 (MEO/IGSO)  (subframes 1+2+3; native BDT scale)
// ---------------------------------------------------------------------------
std::optional<BroadcastEphemeris> SubframeAssembler::consumeBds(const RawNavMessage& msg, PerSat& sat) {
    if (msg.words.size() < 10) return std::nullopt;

    // Pack the 10 words at a 32-bit stride with NO shift and parity left in
    // place, matching RTKLIB decode_cmpraw — the decode_bds_d1 field offsets
    // below are defined against exactly this 320-bit layout.  (The BeiDou
    // preamble 0x712 lands in bits 0..10; FraID at bit 15.)
    uint8_t buf[40] = {0};
    for (int i = 0; i < 10; ++i) setbitu(buf, 32 * i, 32, msg.words[i]);

    const int id = static_cast<int>(getbitu(buf, 15, 3));     // FraID
    if (id < 1 || id > 3) return std::nullopt;                // D1 ephemeris is SF1/2/3 (GEO D2 differs)
    sat.slots[id].assign(buf, buf + 40);

    if (!sat.slots.count(1) || !sat.slots.count(2) || !sat.slots.count(3))
        return std::nullopt;

    const uint8_t* b1 = sat.slots[1].data();
    const uint8_t* b2 = sat.slots[2].data();
    const uint8_t* b3 = sat.slots[3].data();

    // --- Subframe 1 ---
    const int    svh   = static_cast<int>(getbitu(b1, 42, 1));        // SatH1
    const int    week  = static_cast<int>(getbitu(b1, 60, 13));       // native BDT week
    const double toc   = getbitu2(b1, 73, 9, 90, 8) * 8.0;
    const double tgd1  = getbits(b1, 98, 10) * 0.1e-9;               // TGD1 (B1I)
    const double tgd2  = getbits2(b1, 108, 4, 120, 6) * 0.1e-9;      // TGD2 (B2I)
    const double af2   = getbits(b1, 214, 11) * P2(66);
    const double af0   = getbits2(b1, 225, 7, 240, 17) * P2(33);
    const double af1   = getbits2(b1, 257, 5, 270, 17) * P2(50);
    const int    aode  = static_cast<int>(getbitu(b1, 287, 5));

    // --- Subframe 2 ---
    const double dn    = getbits2(b2, 42, 10, 60, 6) * P2(43) * kPi;
    const double cuc   = getbits2(b2, 66, 16, 90, 2) * P2(31);
    const double m0    = getbits2(b2, 92, 20, 120, 12) * P2(31) * kPi;
    const double e     = getbitu2(b2, 132, 10, 150, 22) * P2(33);
    const double cus   = getbits(b2, 180, 18) * P2(31);
    const double crc   = getbits2(b2, 198, 4, 210, 14) * P2(6);
    const double crs   = getbits2(b2, 224, 8, 240, 10) * P2(6);
    const double sqrtA = getbitu2(b2, 250, 12, 270, 20) * P2(19);
    const uint32_t toe_msb = getbitu(b2, 290, 2);

    // --- Subframe 3 ---
    const uint32_t toe_lsb = getbitu2(b3, 42, 10, 60, 5);
    const double toes  = static_cast<double>((toe_msb << 15) | toe_lsb) * 8.0;
    const double i0    = getbits2(b3, 65, 17, 90, 15) * P2(31) * kPi;
    const double cic   = getbits2(b3, 105, 7, 120, 11) * P2(31);
    const double omdot = getbits2(b3, 131, 11, 150, 13) * P2(43) * kPi;
    const double cis   = getbits2(b3, 163, 9, 180, 9) * P2(31);
    const double idot  = getbits2(b3, 189, 13, 210, 1) * P2(43) * kPi;
    const double omega0= getbits2(b3, 211, 21, 240, 11) * P2(31) * kPi;
    const double omega = getbits2(b3, 251, 11, 270, 21) * P2(31) * kPi;

    // Completeness: the toe spanning SF2(2 MSB)+SF3(15 LSB) must equal toc.
    if (std::abs(toes - toc) > 1.0) return std::nullopt;
    if (aode == sat.last_emitted_iode) return std::nullopt;

    BroadcastEphemeris eph;
    eph.satellite     = msg.sat;
    eph.system        = System::BeiDou;
    eph.iode          = aode;
    eph.health        = svh;
    eph.received_time = GnssTime{msg.wnc, msg.tow_s};
    KeplerianEphemeris& k = eph.kepler;
    k.toe = GnssTime{week, toes};          // native BDT (LiveEphemerisStore converts the query)
    k.toc = GnssTime{week, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = omega0; k.omega_dot = omdot; k.omega = omega; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = tgd1; k.tgd1 = tgd2;

    sat.last_emitted_iode = aode;
    return eph;
}

} // namespace GnssProcessor
