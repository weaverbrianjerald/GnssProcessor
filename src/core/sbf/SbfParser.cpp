/**
 * @file SbfParser.cpp
 * @brief SBF block dispatcher + MeasEpoch/Nav decoders.
 *
 * SBF is little-endian; fields are read with memcpy into typed values
 * (alignment-safe, native byte order on x64/MSVC).  Reconstruction formulas
 * and field offsets follow the Septentrio SBF Reference Guide (mosaic-X5
 * v4.8.0), cross-checked against RTKLIB septentrio.c.  Keplerian angular
 * elements are broadcast in SEMICIRCLES and converted to radians here (× π)
 * to match the engine's BroadcastEphemeris convention; BeiDou toe/toc are
 * kept on the native BDT week scale (LiveEphemerisStore converts the query).
 *
 * Offsets for the Nav blocks are RTKLIB-derived and carry validate-against-
 * capture risk (see the SBF research notes); MeasEpoch is high-confidence.
 */

#include "sbf/SbfParser.hpp"
#include "sbf/SbfSignalMap.hpp"
#include "sbf/Meas3Decoder.hpp"
#include "common/Constants.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <cstring>

namespace GnssProcessor {

namespace {

// ---- little-endian typed reads at a byte pointer ----
inline uint8_t  u1(const uint8_t* p) { return p[0]; }
inline uint16_t u2(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t u4(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline int8_t   i1(const uint8_t* p) { return static_cast<int8_t>(p[0]); }
inline int32_t  i4(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
inline float    f4(const uint8_t* p) { float  v; std::memcpy(&v, p, 4); return v; }
inline double   f8(const uint8_t* p) { double v; std::memcpy(&v, p, 8); return v; }

constexpr double kSemiToRad = Constants::PI;   // semicircles -> radians
constexpr uint32_t kDnuU4 = 0xFFFFFFFFu;
constexpr uint16_t kDnuU2 = 0xFFFFu;

// Current GPS week from the system clock (fallback when WNc is Do-Not-Use).
int systemGpsWeek() {
    using namespace std::chrono;
    const auto s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    constexpr int64_t kGpsEpochUnix = 315964800;
    const int64_t g = s - kGpsEpochUnix;
    return (g <= 0) ? 0 : static_cast<int>(g / (7 * 24 * 3600));
}

// Epoch GnssTime from the standard block time stamp (TOW@8, WNc@12).
GnssTime blockTime(const uint8_t* b) {
    const uint32_t tow_ms = u4(b + 8);
    const uint16_t wnc    = u2(b + 12);
    const double tow = (tow_ms == kDnuU4) ? 0.0 : tow_ms * 1e-3;
    const int    week = (wnc == kDnuU2) ? systemGpsWeek() : static_cast<int>(wnc);
    return GnssTime{week, tow};
}

// GPS-frame week for a toe, nearest to the block time (handles week rollover).
int resolveToeWeek(int wnc, double tow_s, double toe_s) {
    if (wnc <= 0) wnc = systemGpsWeek();
    int week = wnc;
    const double now = static_cast<double>(wnc) * 604800.0 + tow_s;
    const double toe = static_cast<double>(week) * 604800.0 + toe_s;
    if (toe - now > 302400.0) week -= 1;
    else if (now - toe > 302400.0) week += 1;
    return week;
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

bool SbfParser::parseFrame(const uint8_t* block, std::size_t len) {
    if (len < 14 || !(block[0] == 0x24 && block[1] == 0x40)) return false;
    const uint16_t id       = u2(block + 4);
    const int block_number  = id & 0x1FFF;
    const int revision      = id >> 13;
    last_block_number_      = block_number;

    switch (block_number) {
    case 4027: return decodeMeasEpoch(block, len, revision);
    case 4109: return decodeMeas3Ranges(block, len); // Meas3Ranges (compact code+phase)
    case 5891: return decodeGpsQzsNav(block, len);   // GPSNav
    case 4095: return decodeGpsQzsNav(block, len);   // QZSNav (same layout)
    case 4002: return decodeGalNav(block, len);      // GALNav
    case 4081: return decodeBdsNav(block, len);      // BDSNav
    case 4004: return decodeGlonav(block, len);      // GLONav
    case 4017: return decodeRawNav(block, len);      // GPSRawCA  (GPS LNAV subframe)
    case 4023: return decodeRawNav(block, len);      // GALRawINAV (Galileo I/NAV page)
    case 4047: return decodeRawNav(block, len);      // BDSRaw     (BeiDou D1/D2 subframe)
    case 4026: return decodeRawNav(block, len);      // GLORawCA   (GLONASS string)
    case 4007: return decodePvtGeodetic(block, len); // PVTGeodetic (receiver a-priori)
    default:
        spdlog::trace("SBF block {} not decoded", block_number);
        return false;
    }
}

// ---------------------------------------------------------------------------
// MeasEpoch (4027) -> RinexObservation
// ---------------------------------------------------------------------------

bool SbfParser::decodeMeasEpoch(const uint8_t* b, std::size_t len, int revision) {
    if (len < 20) return false;
    const GnssTime epoch = blockTime(b);

    const int         n1   = u1(b + 14);
    const std::size_t sb1  = u1(b + 15);
    const std::size_t sb2  = u1(b + 16);
    const uint8_t commonFlags = u1(b + 17);
    if (commonFlags & 0x80) {            // bit7: measurements scrambled (no permission)
        spdlog::warn("SBF MeasEpoch: measurements scrambled (Measurement-Availability not granted)");
        return false;
    }
    if (sb1 < 20 || (n1 > 0 && sb2 < 12)) return false;

    const double c = Constants::SPEED_OF_LIGHT;
    std::size_t p = (revision >= 1) ? 20 : 19;   // CumClkJumps byte present iff rev>=1

    for (int i = 0; i < n1; ++i) {
        if (p + sb1 > len) break;        // truncated

        const uint8_t  type1       = u1(b + p + 1);
        const uint8_t  svid        = u1(b + p + 2);
        const uint8_t  misc        = u1(b + p + 3);
        const uint32_t codeLSB     = u4(b + p + 4);
        const int32_t  doppler1    = i4(b + p + 8);
        const uint16_t carrierLSB1 = u2(b + p + 12);
        const int8_t   carrierMSB1 = i1(b + p + 14);
        const uint8_t  cn0_1       = u1(b + p + 15);
        const uint8_t  obsInfo1    = u1(b + p + 18);
        const int      n2          = u1(b + p + 19);

        System sys;
        const std::string prn = SbfSignalMap::svidToPrn(svid, sys);

        const int sigLo1  = type1 & 0x1F;
        const int sigNum1 = (sigLo1 != 31) ? sigLo1 : 32 + ((obsInfo1 >> 3) & 0x1F);
        const int gloK    = ((obsInfo1 >> 3) & 0x1F) - 8;   // GLONASS FreqNr-8 (sig 8-11)
        const auto info1  = SbfSignalMap::signalInfo(sigNum1);
        const double f1   = info1.glonass_fdma_band
            ? SbfSignalMap::glonassFdmaFreqHz(info1.glonass_fdma_band, gloK) : info1.freq_hz;

        RinexObservation obs;
        obs.satellite = prn;
        obs.system    = sys;
        obs.time      = epoch;

        // Store one signal's observables (shared by Type1 + Type2).
        auto store = [&](const std::string& suffix, int signalNumber,
                         double pr_m, bool pr_v, double ph_cyc, bool ph_v,
                         double dopp_hz, bool dopp_v, int cn0_raw, bool cn0_v, uint8_t obsinfo) {
            if (suffix.empty()) return;
            if (pr_v)   obs.measurements["C" + suffix] = pr_m;
            if (ph_v) { obs.measurements["L" + suffix] = ph_cyc;
                        obs.lli_flags["L" + suffix] = (obsinfo & 0x04) ? 0x2 : 0; }
            if (dopp_v) obs.measurements["D" + suffix] = dopp_hz;
            if (cn0_v) {
                const double db = (signalNumber == 1 || signalNumber == 2)
                                  ? cn0_raw * 0.25 : cn0_raw * 0.25 + 10.0;
                obs.measurements["S" + suffix] = db;
            }
        };

        // --- Type1 (master / absolute) ---
        const int    codeMSB1 = misc & 0x0F;
        const bool   pr1_v    = !(codeMSB1 == 0 && codeLSB == 0);
        const double pr1_m    = (static_cast<double>(codeMSB1) * 4294967296.0 + codeLSB) * 0.001;
        const double lam1     = (f1 > 0.0) ? c / f1 : 0.0;
        const bool   carr1_v  = pr1_v && lam1 > 0.0 && !(carrierMSB1 == -128 && carrierLSB1 == 0);
        const double ph1      = (pr1_v && lam1 > 0.0)
            ? pr1_m / lam1 + (static_cast<double>(carrierMSB1) * 65536.0 + carrierLSB1) * 0.001 : 0.0;
        const bool   dopp1_v  = (doppler1 != static_cast<int32_t>(0x80000000));
        const double dopp1_hz = doppler1 * 0.0001;
        if (!prn.empty())
            store(info1.suffix, sigNum1, pr1_m, pr1_v, ph1, carr1_v,
                  dopp1_hz, dopp1_v, cn0_1, cn0_1 != 0xFF, obsInfo1);

        // --- Type2 (slave / delta vs Type1) ---
        for (int j = 0; j < n2 && !prn.empty(); ++j) {
            const std::size_t q = p + sb1 + static_cast<std::size_t>(j) * sb2;
            if (q + sb2 > len) break;

            const uint8_t  type2       = u1(b + q + 0);
            const uint8_t  cn0_2       = u1(b + q + 2);
            const uint8_t  offsetsMSB  = u1(b + q + 3);
            const int8_t   carrierMSB2 = i1(b + q + 4);
            const uint8_t  obsInfo2    = u1(b + q + 5);
            const uint16_t codeOffLSB  = u2(b + q + 6);
            const uint16_t carrierLSB2 = u2(b + q + 8);
            const uint16_t doppOffLSB  = u2(b + q + 10);

            const int sigLo2  = type2 & 0x1F;
            const int sigNum2 = (sigLo2 != 31) ? sigLo2 : 32 + ((obsInfo2 >> 3) & 0x1F);
            const auto info2  = SbfSignalMap::signalInfo(sigNum2);
            const double f2   = info2.glonass_fdma_band
                ? SbfSignalMap::glonassFdmaFreqHz(info2.glonass_fdma_band, gloK) : info2.freq_hz;
            const double lam2 = (f2 > 0.0) ? c / f2 : 0.0;

            int codeOffMSB = offsetsMSB & 0x07;          if (codeOffMSB >= 4)  codeOffMSB -= 8;
            int doppOffMSB = (offsetsMSB >> 3) & 0x1F;   if (doppOffMSB >= 16) doppOffMSB -= 32;

            const bool   pr2_v  = pr1_v && !(codeOffMSB == -4 && codeOffLSB == 0);
            const double pr2_m  = pr1_m + (static_cast<double>(codeOffMSB) * 65536.0 + codeOffLSB) * 0.001;
            const bool   carr2_v = pr2_v && lam2 > 0.0 && !(carrierMSB2 == -128 && carrierLSB2 == 0);
            const double ph2    = (pr2_v && lam2 > 0.0)
                ? pr2_m / lam2 + (static_cast<double>(carrierMSB2) * 65536.0 + carrierLSB2) * 0.001 : 0.0;
            const bool   dopp2_v = dopp1_v && f1 > 0.0 && !(doppOffMSB == -16 && doppOffLSB == 0);
            const double dopp2_hz = (f1 > 0.0)
                ? dopp1_hz * (f2 / f1) + (static_cast<double>(doppOffMSB) * 65536.0 + doppOffLSB) * 1e-4 : 0.0;

            store(info2.suffix, sigNum2, pr2_m, pr2_v, ph2, carr2_v,
                  dopp2_hz, dopp2_v, cn0_2, cn0_2 != 0xFF, obsInfo2);
        }

        if (!obs.measurements.empty()) observations_.push_back(std::move(obs));
        p += sb1 + static_cast<std::size_t>(n2) * sb2;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Meas3Ranges (4109) -> RinexObservation
//
// The compact, differentially-encoded replacement for MeasEpoch.  Decoding a
// delta epoch depends on a previously-seen reference epoch, so the cross-block
// state lives in the process-wide Meas3Decoder (the format and the state
// machine are documented there); this method is a thin adapter that drains the
// decoded per-satellite observations into the parser's output vector.  Returns
// true whenever the block is structurally a Meas3Ranges block — an empty result
// (auxiliary-antenna set, or a delta epoch whose reference was not yet seen) is
// not an error.
// ---------------------------------------------------------------------------
bool SbfParser::decodeMeas3Ranges(const uint8_t* b, std::size_t len) {
    if (len < 20) return false;
    for (auto& obs : Meas3Decoder::instance().decode(b, len))
        observations_.push_back(std::move(obs));
    return true;
}

// ---------------------------------------------------------------------------
// GPSNav (5891) / QZSNav (4095) — identical Keplerian layout
// ---------------------------------------------------------------------------

bool SbfParser::decodeGpsQzsNav(const uint8_t* b, std::size_t len) {
    if (len < 140) return false;
    System sys;
    const std::string prn = SbfSignalMap::svidToPrn(u1(b + 14), sys);
    if (prn.empty() || (sys != System::GPS && sys != System::QZSS)) return false;

    const double tow_s = (u4(b + 8) == kDnuU4) ? 0.0 : u4(b + 8) * 1e-3;
    const int    wnc   = (u2(b + 12) == kDnuU2) ? 0 : static_cast<int>(u2(b + 12));

    BroadcastEphemeris eph;
    eph.satellite     = prn;
    eph.system        = sys;
    eph.iode          = u1(b + 24);                 // IODE2
    eph.health        = u1(b + 20);
    eph.received_time = GnssTime{wnc > 0 ? wnc : systemGpsWeek(), tow_s};

    KeplerianEphemeris& k = eph.kepler;
    const double toe = u4(b + 88);
    const double toc = u4(b + 32);
    const int week = resolveToeWeek(wnc, tow_s, toe);
    k.toe = GnssTime{week, toe};
    k.toc = GnssTime{week, toc};
    k.tgd0      = f4(b + 28);
    k.af2 = f4(b + 36); k.af1 = f4(b + 40); k.af0 = f4(b + 44);
    k.crs       = f4(b + 48);
    k.delta_n   = f4(b + 52) * kSemiToRad;
    k.m0        = f8(b + 56) * kSemiToRad;
    k.cuc       = f4(b + 64);
    k.e         = f8(b + 68);
    k.cus       = f4(b + 76);
    k.sqrt_a    = f8(b + 80);
    k.cic       = f4(b + 92);
    k.omega0    = f8(b + 96) * kSemiToRad;
    k.cis       = f4(b + 104);
    k.i0        = f8(b + 108) * kSemiToRad;
    k.crc       = f4(b + 116);
    k.omega     = f8(b + 120) * kSemiToRad;
    k.omega_dot = f4(b + 128) * kSemiToRad;
    k.idot      = f4(b + 132) * kSemiToRad;
    broadcast_eph_.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// GALNav (4002) — double-first I/NAV layout
// ---------------------------------------------------------------------------

bool SbfParser::decodeGalNav(const uint8_t* b, std::size_t len) {
    if (len < 144) return false;
    System sys;
    const std::string prn = SbfSignalMap::svidToPrn(u1(b + 14), sys);
    if (prn.empty() || sys != System::Galileo) return false;

    const double tow_s = (u4(b + 8) == kDnuU4) ? 0.0 : u4(b + 8) * 1e-3;
    const int    wnc   = (u2(b + 12) == kDnuU2) ? 0 : static_cast<int>(u2(b + 12));

    BroadcastEphemeris eph;
    eph.satellite     = prn;
    eph.system        = sys;
    eph.iode          = u2(b + 128);                // IODnav
    eph.received_time = GnssTime{wnc > 0 ? wnc : systemGpsWeek(), tow_s};

    KeplerianEphemeris& k = eph.kepler;
    k.sqrt_a    = f8(b + 16);
    k.m0        = f8(b + 24) * kSemiToRad;
    k.e         = f8(b + 32);
    k.i0        = f8(b + 40) * kSemiToRad;
    k.omega     = f8(b + 48) * kSemiToRad;
    k.omega0    = f8(b + 56) * kSemiToRad;
    k.omega_dot = f4(b + 64) * kSemiToRad;
    k.idot      = f4(b + 68) * kSemiToRad;
    k.delta_n   = f4(b + 72) * kSemiToRad;
    k.cuc = f4(b + 76); k.cus = f4(b + 80);
    k.crc = f4(b + 84); k.crs = f4(b + 88);
    k.cic = f4(b + 92); k.cis = f4(b + 96);
    const double toe = u4(b + 100);
    const double toc = u4(b + 104);
    k.af2 = f4(b + 108); k.af1 = f4(b + 112); k.af0 = f8(b + 116);  // af0 is f64 per Septentrio
    const int week = resolveToeWeek(wnc, tow_s, toe);
    k.toe = GnssTime{week, toe};
    k.toc = GnssTime{week, toc};
    k.tgd1 = f4(b + 136);    // BGD E1-E5a
    k.tgd0 = f4(b + 140);    // BGD E1-E5b
    broadcast_eph_.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// BDSNav (4081) — native BDT scale (validate early-header offsets vs capture)
// ---------------------------------------------------------------------------

bool SbfParser::decodeBdsNav(const uint8_t* b, std::size_t len) {
    if (len < 140) return false;
    System sys;
    const std::string prn = SbfSignalMap::svidToPrn(u1(b + 14), sys);
    if (prn.empty() || sys != System::BeiDou) return false;

    const double tow_s = (u4(b + 8) == kDnuU4) ? 0.0 : u4(b + 8) * 1e-3;
    const int    wnc   = (u2(b + 12) == kDnuU2) ? 0 : static_cast<int>(u2(b + 12));

    BroadcastEphemeris eph;
    eph.satellite     = prn;
    eph.system        = sys;
    eph.iode          = u1(b + 21);                 // AODE
    eph.health        = u1(b + 19);                 // SatH1
    eph.received_time = GnssTime{wnc > 0 ? wnc : systemGpsWeek(), tow_s};

    KeplerianEphemeris& k = eph.kepler;
    const int    bdt_week = static_cast<int>(u2(b + 16));   // native BDT week
    const double toe = u4(b + 88);
    const double toc = u4(b + 32);
    k.toe = GnssTime{bdt_week, toe};                // native BDT (LiveEphemerisStore converts query)
    k.toc = GnssTime{bdt_week, toc};
    k.tgd0 = f4(b + 24); k.tgd1 = f4(b + 28);       // TGD1 (B1I), TGD2 (B2I)
    k.af2 = f4(b + 36); k.af1 = f4(b + 40); k.af0 = f4(b + 44);
    k.crs       = f4(b + 48);
    k.delta_n   = f4(b + 52) * kSemiToRad;
    k.m0        = f8(b + 56) * kSemiToRad;
    k.cuc       = f4(b + 64);
    k.e         = f8(b + 68);
    k.cus       = f4(b + 76);
    k.sqrt_a    = f8(b + 80);
    k.cic       = f4(b + 92);
    k.omega0    = f8(b + 96) * kSemiToRad;
    k.cis       = f4(b + 104);
    k.i0        = f8(b + 108) * kSemiToRad;
    k.crc       = f4(b + 116);
    k.omega     = f8(b + 120) * kSemiToRad;
    k.omega_dot = f4(b + 128) * kSemiToRad;
    k.idot      = f4(b + 132) * kSemiToRad;
    broadcast_eph_.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// GLONav (4004) -> GlonassEphemeris (PZ-90 state).  GLONASS is excluded from
// active_systems for now; decoded so the store is complete, offsets best-effort.
// ---------------------------------------------------------------------------

bool SbfParser::decodeGlonav(const uint8_t* b, std::size_t len) {
    if (len < 80) return false;
    System sys;
    const std::string prn = SbfSignalMap::svidToPrn(u1(b + 14), sys);
    if (prn.empty() || sys != System::GLONASS) return false;

    const int    freqNr = u1(b + 15);
    const double tow_s  = (u4(b + 8) == kDnuU4) ? 0.0 : u4(b + 8) * 1e-3;
    const int    wnc    = (u2(b + 12) == kDnuU2) ? 0 : static_cast<int>(u2(b + 12));

    BroadcastEphemeris eph;
    eph.satellite     = prn;
    eph.system        = sys;
    eph.iode          = -1;                          // GLONASS has no IODE
    eph.received_time = GnssTime{wnc > 0 ? wnc : systemGpsWeek(), tow_s};

    GlonassEphemeris& g = eph.glonass;
    g.pos = Eigen::Vector3d{f8(b + 16) * 1000.0, f8(b + 24) * 1000.0, f8(b + 32) * 1000.0};
    g.vel = Eigen::Vector3d{f4(b + 40) * 1000.0, f4(b + 44) * 1000.0, f4(b + 48) * 1000.0};
    g.acc = Eigen::Vector3d{f4(b + 52) * 1000.0, f4(b + 56) * 1000.0, f4(b + 60) * 1000.0};
    g.gamma_n      = f4(b + 64);
    g.tau_n        = f4(b + 68);
    g.freq_channel = freqNr - 8;
    const double toe = u4(b + 76);
    g.toe = GnssTime{resolveToeWeek(wnc, tow_s, toe), toe};
    broadcast_eph_.push_back(std::move(eph));
    return true;
}

// ---------------------------------------------------------------------------
// RawNavBits container (4017 GPSRawCA / 4023 GALRawINAV / 4047 BDSRaw /
// 4026 GLORawCA).  Layout confirmed against a real AsteRx-U capture:
//   body@14: SVID u8, CRCPassed u8, ViterbiCnt u8, Source u8 (SBF signal),
//            FreqNr u8, RxChannel u8, then NAVBits = (body_len-6)/4 LE u32.
// For GPS/BeiDou each u32 holds one 30-bit nav word right-justified (bits
// 29..0).  Subframe/page assembly into an ephemeris is done downstream by
// SubframeAssembler; here we only lift the words verbatim.
// ---------------------------------------------------------------------------
bool SbfParser::decodeRawNav(const uint8_t* b, std::size_t len) {
    constexpr std::size_t kBody = 14;
    constexpr std::size_t kHdr  = 6;
    if (len < kBody + kHdr + 4) return false;

    System sys;
    const std::string prn = SbfSignalMap::svidToPrn(u1(b + kBody + 0), sys);
    if (prn.empty() || sys == System::Unknown) return false;

    RawNavMessage m;
    m.sat        = prn;
    m.system     = sys;
    m.crc_passed = u1(b + kBody + 1) != 0;
    m.signal     = u1(b + kBody + 3);
    m.freq_nr    = u1(b + kBody + 4);
    m.wnc        = (u2(b + 12) == kDnuU2) ? 0 : static_cast<int>(u2(b + 12));
    m.tow_s      = (u4(b + 8) == kDnuU4) ? 0.0 : u4(b + 8) * 1e-3;

    const std::size_t nav_off = kBody + kHdr;
    const std::size_t nwords  = (len - nav_off) / 4;
    m.words.reserve(nwords);
    for (std::size_t k = 0; k < nwords; ++k)
        m.words.push_back(u4(b + nav_off + 4 * k));

    raw_nav_.push_back(std::move(m));
    return true;
}

// ---------------------------------------------------------------------------
// PVTGeodetic (4007) -> receiver ECEF a-priori
//
// The receiver's own PVT solution: Mode@14 / Error@15, then geodetic
// Latitude@16 / Longitude@24 (f8, radians) and ellipsoidal Height@32 (f8, m).
// Error==0 means a valid fix; the Do-Not-Use sentinel for SBF f8 is −2·10^10.
// Converted to WGS84 ECEF so the realtime engine can seed the filter a-priori
// on a live stream that carries no RINEX header (offsets per the SBF Reference
// Guide, cross-checked against RTKLIB septentrio.c decode_pvtgeodetic).
// ---------------------------------------------------------------------------
bool SbfParser::decodePvtGeodetic(const uint8_t* b, std::size_t len) {
    if (len < 40) return false;                  // header(8)+TOW/WNc(6)+Mode/Error(2)+lat/lon/hgt(24)
    const uint8_t error = u1(b + 15);            // PVT error code (0 = no error)
    if (error != 0) return false;                // no valid PVT in this block

    const double lat = f8(b + 16);               // [rad]
    const double lon = f8(b + 24);               // [rad]
    const double hgt = f8(b + 32);               // [m] above the WGS84 ellipsoid
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(hgt)) return false;
    if (lat < -1.0e9 || lon < -1.0e9 || hgt < -1.0e9) return false;   // Do-Not-Use sentinel

    // WGS84 geodetic -> ECEF (same constants as math/Coordinates::llhToEcef).
    constexpr double a  = 6378137.0;
    constexpr double e2 = 6.69437999014e-3;
    const double sinLat = std::sin(lat), cosLat = std::cos(lat);
    const double N = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
    receiver_position_ecef_ = Eigen::Vector3d{
        (N + hgt) * cosLat * std::cos(lon),
        (N + hgt) * cosLat * std::sin(lon),
        (N * (1.0 - e2) + hgt) * sinLat
    };
    return true;
}

void SbfParser::clear() {
    last_block_number_ = -1;
    observations_.clear();
    broadcast_eph_.clear();
    raw_nav_.clear();
    receiver_position_ecef_.reset();
}

} // namespace GnssProcessor
