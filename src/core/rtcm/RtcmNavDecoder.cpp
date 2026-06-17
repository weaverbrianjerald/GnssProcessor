/**
 * @file RtcmNavDecoder.cpp
 * @brief Bit-level decoders for RTCM broadcast-ephemeris messages.
 *
 * Field widths and scale factors transcribed from RTCM Standard 10403.3.
 * Angular elements are broadcast in semicircles and converted to radians
 * (× π) here.  Keplerian systems (GPS/Galileo/BeiDou/QZSS) populate
 * @c BroadcastEphemeris::kepler; GLONASS populates @c glonass (sign-
 * magnitude PZ-90 state, km/km·s/km·s² → SI).
 *
 * Per-message field ORDER differs (notably QZSS 1044 leads with the clock
 * block and carries the week near the end) — the order below follows each
 * message's DF table exactly and must not be reordered.
 */

#include "rtcm/RtcmNavDecoder.hpp"
#include "rtcm/RtcmBitReader.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>

namespace GnssProcessor::RtcmNav {

namespace {

constexpr double kPi = Constants::PI;

// 2^e as a double (e may be negative) — RTCM scale factors.
inline double p2(int e) { return std::ldexp(1.0, e); }

std::string formatSat(char prefix, int id) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d", prefix, id);
    return std::string(buf);
}

// Reconstruct a full week from a truncated field + the current full week
// (in the SAME week numbering as @p wn).  Picks the rollover epoch nearest
// the current week.  Returns @p wn unchanged when the current week is unknown.
int resolveWeek(int current_full_week, int wn, int n_bits) {
    if (current_full_week <= 0) return wn;
    const int rollover = 1 << n_bits;
    int wk = (current_full_week - (current_full_week % rollover)) + wn;
    if (wk < current_full_week - rollover / 2) wk += rollover;
    else if (wk > current_full_week + rollover / 2) wk -= rollover;
    return wk;
}

// System clock as a GPS-scale GnssTime (staleness stamp; leap drift ignored).
GnssTime nowReceiveTime() {
    using namespace std::chrono;
    const auto since = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    constexpr int64_t kGpsEpochUnix = 315964800;
    const int64_t s = since - kGpsEpochUnix;
    if (s <= 0) return GnssTime{0, 0.0};
    constexpr int kSecPerWeek = 7 * 24 * 3600;
    return GnssTime{static_cast<int>(s / kSecPerWeek), static_cast<double>(s % kSecPerWeek)};
}

// ---------------------------------------------------------------------------
// 1019 — GPS LNAV ephemeris
// ---------------------------------------------------------------------------
bool decodeGps1019(RtcmBitReader& r, int current_week, std::vector<BroadcastEphemeris>& out) {
    const int    sat   = static_cast<int>(r.u(6));     // DF009
    const int    wn    = static_cast<int>(r.u(10));    // DF076 GPS week
    r.u(4);                                            // DF077 SV accuracy (URA)
    r.u(2);                                            // DF078 CODE ON L2
    const double idot  = r.s(14) * p2(-43) * kPi;      // DF079
    const int    iode  = static_cast<int>(r.u(8));     // DF071
    const double toc   = r.u(16) * 16.0;               // DF081 (2^4 s)
    const double af2   = r.s(8)  * p2(-55);            // DF082
    const double af1   = r.s(16) * p2(-43);            // DF083
    const double af0   = r.s(22) * p2(-31);            // DF084
    r.u(10);                                           // DF085 IODC
    const double crs   = r.s(16) * p2(-5);             // DF086
    const double dn    = r.s(16) * p2(-43) * kPi;      // DF087
    const double m0    = r.s(32) * p2(-31) * kPi;      // DF088
    const double cuc   = r.s(16) * p2(-29);            // DF089
    const double e     = r.u(32) * p2(-33);            // DF090
    const double cus   = r.s(16) * p2(-29);            // DF091
    const double sqrtA = r.u(32) * p2(-19);            // DF092
    const double toe   = r.u(16) * 16.0;               // DF093
    const double cic   = r.s(16) * p2(-29);            // DF094
    const double om0   = r.s(32) * p2(-31) * kPi;      // DF095
    const double cis   = r.s(16) * p2(-29);            // DF096
    const double i0    = r.s(32) * p2(-31) * kPi;      // DF097
    const double crc   = r.s(16) * p2(-5);             // DF098
    const double w     = r.s(32) * p2(-31) * kPi;      // DF099
    const double omdot = r.s(24) * p2(-43) * kPi;      // DF100
    const double tgd   = r.s(8)  * p2(-31);            // DF101
    const int    health= static_cast<int>(r.u(6));     // DF102
    r.u(1);                                            // DF103 L2 P data flag
    r.u(1);                                            // DF137 fit interval
    if (!r.ok()) return false;

    BroadcastEphemeris eph;
    eph.satellite = formatSat('G', sat);
    eph.system    = System::GPS;
    eph.iode      = iode;
    eph.health    = health;
    eph.received_time = nowReceiveTime();
    KeplerianEphemeris& k = eph.kepler;
    const int gw = resolveWeek(current_week, wn, 10);
    k.toe = GnssTime{gw, toe};
    k.toc = GnssTime{gw, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = om0; k.omega_dot = omdot; k.omega = w; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = tgd;
    out.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// 1044 — QZSS ephemeris (clock block leads; week near the end)
// ---------------------------------------------------------------------------
bool decodeQzss1044(RtcmBitReader& r, int current_week, std::vector<BroadcastEphemeris>& out) {
    const int    sat   = static_cast<int>(r.u(4));     // DF429
    const double toc   = r.u(16) * 16.0;               // DF430
    const double af2   = r.s(8)  * p2(-55);            // DF431
    const double af1   = r.s(16) * p2(-43);            // DF432
    const double af0   = r.s(22) * p2(-31);            // DF433
    const int    iode  = static_cast<int>(r.u(8));     // DF434
    const double crs   = r.s(16) * p2(-5);             // DF435
    const double dn    = r.s(16) * p2(-43) * kPi;      // DF436
    const double m0    = r.s(32) * p2(-31) * kPi;      // DF437
    const double cuc   = r.s(16) * p2(-29);            // DF438
    const double e     = r.u(32) * p2(-33);            // DF439
    const double cus   = r.s(16) * p2(-29);            // DF440
    const double sqrtA = r.u(32) * p2(-19);            // DF441
    const double toe   = r.u(16) * 16.0;               // DF442
    const double cic   = r.s(16) * p2(-29);            // DF443
    const double om0   = r.s(32) * p2(-31) * kPi;      // DF444
    const double cis   = r.s(16) * p2(-29);            // DF445
    const double i0    = r.s(32) * p2(-31) * kPi;      // DF446
    const double crc   = r.s(16) * p2(-5);             // DF447
    const double w     = r.s(32) * p2(-31) * kPi;      // DF448
    const double omdot = r.s(24) * p2(-43) * kPi;      // DF449
    const double idot  = r.s(14) * p2(-43) * kPi;      // DF450
    r.u(2);                                            // DF451 CODE ON L2
    const int    wn    = static_cast<int>(r.u(10));    // DF452 QZSS week
    r.u(4);                                            // DF453 URA
    const int    health= static_cast<int>(r.u(6));     // DF454
    const double tgd   = r.s(8)  * p2(-31);            // DF455
    r.u(10);                                           // DF456 IODC
    r.u(1);                                            // DF457 fit interval
    if (!r.ok()) return false;

    BroadcastEphemeris eph;
    eph.satellite = formatSat('J', sat);
    eph.system    = System::QZSS;
    eph.iode      = iode;
    eph.health    = health;
    eph.received_time = nowReceiveTime();
    KeplerianEphemeris& k = eph.kepler;
    const int gw = resolveWeek(current_week, wn, 10);
    k.toe = GnssTime{gw, toe};
    k.toc = GnssTime{gw, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = om0; k.omega_dot = omdot; k.omega = w; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = tgd;
    out.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// 1046 — Galileo I/NAV ephemeris
// ---------------------------------------------------------------------------
bool decodeGalileo1046(RtcmBitReader& r, int current_week, std::vector<BroadcastEphemeris>& out) {
    const int    sat    = static_cast<int>(r.u(6));    // DF252
    const int    wn     = static_cast<int>(r.u(12));   // DF289 Galileo week (GST)
    const int    iodnav = static_cast<int>(r.u(10));   // DF290
    r.u(8);                                            // DF291 SISA
    const double idot   = r.s(14) * p2(-43) * kPi;     // DF292
    const double toc    = r.u(14) * 60.0;              // DF293 (60 s)
    const double af2    = r.s(6)  * p2(-59);           // DF294
    const double af1    = r.s(21) * p2(-46);           // DF295
    const double af0    = r.s(31) * p2(-34);           // DF296
    const double crs    = r.s(16) * p2(-5);            // DF297
    const double dn     = r.s(16) * p2(-43) * kPi;     // DF298
    const double m0     = r.s(32) * p2(-31) * kPi;     // DF299
    const double cuc    = r.s(16) * p2(-29);           // DF300
    const double e      = r.u(32) * p2(-33);           // DF301
    const double cus    = r.s(16) * p2(-29);           // DF302
    const double sqrtA  = r.u(32) * p2(-19);           // DF303
    const double toe    = r.u(14) * 60.0;              // DF304 (60 s)
    const double cic    = r.s(16) * p2(-29);           // DF305
    const double om0    = r.s(32) * p2(-31) * kPi;     // DF306
    const double cis    = r.s(16) * p2(-29);           // DF307
    const double i0     = r.s(32) * p2(-31) * kPi;     // DF308
    const double crc    = r.s(16) * p2(-5);            // DF309
    const double w      = r.s(32) * p2(-31) * kPi;     // DF310
    const double omdot  = r.s(24) * p2(-43) * kPi;     // DF311
    const double bgd_a  = r.s(10) * p2(-32);           // DF312 BGD E1/E5a
    const double bgd_b  = r.s(10) * p2(-32);           // DF313 BGD E1/E5b
    r.u(2);                                            // DF314 E5b signal health
    r.u(1);                                            // DF315 E5b data validity
    const int    e1bhs  = static_cast<int>(r.u(2));    // DF316 E1-B signal health
    r.u(1);                                            // DF317 E1-B data validity
    if (!r.ok()) return false;

    BroadcastEphemeris eph;
    eph.satellite = formatSat('E', sat);
    eph.system    = System::Galileo;
    eph.iode      = iodnav;
    eph.health    = e1bhs;
    eph.received_time = nowReceiveTime();
    KeplerianEphemeris& k = eph.kepler;
    // Galileo week numbering: GST week 0 = GPS week 1024.
    const int gal_full = resolveWeek(current_week > 0 ? current_week - 1024 : 0, wn, 12);
    const int gw = (current_week > 0) ? gal_full + 1024 : wn + 1024;
    k.toe = GnssTime{gw, toe};
    k.toc = GnssTime{gw, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = om0; k.omega_dot = omdot; k.omega = w; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2; k.tgd0 = bgd_a; k.tgd1 = bgd_b;
    out.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// 1042 — BeiDou D1 ephemeris (native BDT time scale; CGCS2000)
// ---------------------------------------------------------------------------
bool decodeBeidou1042(RtcmBitReader& r, int /*current_week*/, std::vector<BroadcastEphemeris>& out) {
    const int    sat   = static_cast<int>(r.u(6));     // DF488
    const int    wn    = static_cast<int>(r.u(13));    // DF489 BDS week (BDT)
    r.u(4);                                            // DF490 URAI
    const double idot  = r.s(14) * p2(-43) * kPi;      // DF491
    const int    aode  = static_cast<int>(r.u(5));     // DF492
    const double toc   = r.u(17) * 8.0;                // DF493 (2^3 s)
    const double a2    = r.s(11) * p2(-66);            // DF494
    const double a1    = r.s(22) * p2(-50);            // DF495
    const double a0    = r.s(24) * p2(-33);            // DF496
    r.u(5);                                            // DF497 AODC
    const double crs   = r.s(18) * p2(-6);             // DF498
    const double dn    = r.s(16) * p2(-43) * kPi;      // DF499
    const double m0    = r.s(32) * p2(-31) * kPi;      // DF500
    const double cuc   = r.s(18) * p2(-31);            // DF501
    const double e     = r.u(32) * p2(-33);            // DF502
    const double cus   = r.s(18) * p2(-31);            // DF503
    const double sqrtA = r.u(32) * p2(-19);            // DF504
    const double toe   = r.u(17) * 8.0;                // DF505
    const double cic   = r.s(18) * p2(-31);            // DF506
    const double om0   = r.s(32) * p2(-31) * kPi;      // DF507
    const double cis   = r.s(18) * p2(-31);            // DF508
    const double i0    = r.s(32) * p2(-31) * kPi;      // DF509
    const double crc   = r.s(18) * p2(-6);             // DF510
    const double w     = r.s(32) * p2(-31) * kPi;      // DF511
    const double omdot = r.s(24) * p2(-43) * kPi;      // DF512
    const double tgd1  = r.s(10) * 1e-10;              // DF513 (0.1 ns)
    const double tgd2  = r.s(10) * 1e-10;              // DF514 (0.1 ns)
    const int    health= static_cast<int>(r.u(1));     // DF515
    if (!r.ok()) return false;

    BroadcastEphemeris eph;
    eph.satellite = formatSat('C', sat);
    eph.system    = System::BeiDou;
    eph.iode      = aode;                              // BDS AODE used as the IODE gate key
    eph.health    = health;
    eph.received_time = nowReceiveTime();
    KeplerianEphemeris& k = eph.kepler;
    // BeiDou times kept on native BDT week numbering (toe ∈ [0, 604800) BDT).
    k.toe = GnssTime{wn, toe};
    k.toc = GnssTime{wn, toc};
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = om0; k.omega_dot = omdot; k.omega = w; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = a0; k.af1 = a1; k.af2 = a2; k.tgd0 = tgd1; k.tgd1 = tgd2;
    out.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// 1020 — GLONASS ephemeris (sign-magnitude PZ-90 state)
//
// Excluded from active_systems until the MSM time-of-day → GPS-week promotion
// is plumbed (same gap as the MSM week=0 issue), so the toe epoch tagging is
// best-effort.  The PZ-90 state vector itself is decoded correctly.
// ---------------------------------------------------------------------------
bool decodeGlonass1020(RtcmBitReader& r, int current_week, std::vector<BroadcastEphemeris>& out) {
    const int sat  = static_cast<int>(r.u(6));         // DF038 slot number
    const int freq = static_cast<int>(r.u(5));         // DF039 freq channel (0..20 -> k-7)
    r.u(1);                                            // DF040 almanac health
    r.u(1);                                            // DF104 almanac health availability
    r.u(2);                                            // DF105 P1
    r.u(12);                                           // DF106 tk
    r.u(1);                                            // DF107 MSB of Bn (health)
    r.u(1);                                            // DF108 P2
    const int tb   = static_cast<int>(r.u(7));         // DF109 tb (×15 min)
    const double vx = r.sm(24) * p2(-20) * 1000.0;     // DF110 xn' [km/s -> m/s]
    const double x  = r.sm(27) * p2(-11) * 1000.0;     // DF111 xn  [km   -> m]
    const double ax = r.sm(5)  * p2(-30) * 1000.0;     // DF112 xn'' [km/s^2 -> m/s^2]
    const double vy = r.sm(24) * p2(-20) * 1000.0;     // DF113
    const double y  = r.sm(27) * p2(-11) * 1000.0;     // DF114
    const double ay = r.sm(5)  * p2(-30) * 1000.0;     // DF115
    const double vz = r.sm(24) * p2(-20) * 1000.0;     // DF116
    const double z  = r.sm(27) * p2(-11) * 1000.0;     // DF117
    const double az = r.sm(5)  * p2(-30) * 1000.0;     // DF118
    r.u(1);                                            // DF119 P3
    const double gamma = r.sm(11) * p2(-40);           // DF120
    r.u(2);                                            // DF121 P
    r.u(1);                                            // DF122 ln
    const double tau   = r.sm(22) * p2(-30);           // DF123 tau_n [s]
    // Remaining fields (delta tau, En, almanac, time params) are not needed
    // for orbit propagation and are left unread; r.ok() still validates that
    // the fields consumed above fit within the payload.
    if (!r.ok()) return false;

    BroadcastEphemeris eph;
    eph.satellite = formatSat('R', sat);
    eph.system    = System::GLONASS;
    eph.iode      = -1;                                // GLONASS has no IODE; gate stays permissive
    eph.received_time = nowReceiveTime();
    GlonassEphemeris& g = eph.glonass;
    // tb indexes 15-minute marks in the GLONASS (UTC(SU)+3h) day, i.e. a
    // time-of-day with no date.  Promote it to a full GPS-week toe by anchoring
    // on the rover's current GPS time (same wall-clock anchor the MSM obs path
    // uses) so the PZ-90 RK4 integration window (toe → query) stays small.
    // current_week is no longer needed for GLONASS (the converter resolves the
    // day); it remains the week source for the Keplerian decoders.
    (void)current_week;
    g.toe = TimeUtils::glonassTodToGpsTime(static_cast<double>(tb) * 900.0,
                                           TimeUtils::nowContinuousGpsSeconds());
    g.pos = Eigen::Vector3d{x, y, z};
    g.vel = Eigen::Vector3d{vx, vy, vz};
    g.acc = Eigen::Vector3d{ax, ay, az};
    g.tau_n = tau;
    g.gamma_n = gamma;
    g.freq_channel = freq - 7;
    out.push_back(std::move(eph));
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

bool isBroadcastEphemerisType(uint16_t msg_type) {
    return msg_type == 1019 || msg_type == 1020 || msg_type == 1042 ||
           msg_type == 1044 || msg_type == 1046;
}

DecodeResult decodeBroadcast(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<BroadcastEphemeris>& out)
{
    DecodeResult result{};
    RtcmBitReader r(payload, len_bytes);
    r.skip(12);   // DF002 (message type) already consumed by parseHeader.

    switch (msg_type) {
    case 1019: result.ok = decodeGps1019(r, current_gps_week, out);     break;
    case 1044: result.ok = decodeQzss1044(r, current_gps_week, out);    break;
    case 1046: result.ok = decodeGalileo1046(r, current_gps_week, out); break;
    case 1042: result.ok = decodeBeidou1042(r, current_gps_week, out);  break;
    case 1020: result.ok = decodeGlonass1020(r, current_gps_week, out); break;
    default:   result.ok = false;                                       break;
    }
    return result;
}

} // namespace GnssProcessor::RtcmNav
