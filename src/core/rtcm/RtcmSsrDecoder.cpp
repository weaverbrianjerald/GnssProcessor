/**
 * @file RtcmSsrDecoder.cpp
 * @brief Bit-level decoders for legacy RTCM SSR (1057-1068, 1240-1270).
 *
 * Field widths and scale factors transcribed from RTCM Standard 10403.3,
 * Annex A.  Per-system differences are confined to:
 *   - Satellite ID width (DF068 / DF384 / DF386 / etc.):
 *       GPS 6, GLO 5, GAL 6, QZS 4, SBAS 6, BDS 6
 *   - IODE width (DF071 / equivalents):
 *       GPS 8, GLO 8, GAL 10, QZS 8, SBAS 9, BDS 10
 *   - Epoch field width (DF385 GPS TOW = 20 bits, DF389 GLO TOD = 17 bits)
 *
 * Everything else (orbit/clock/bias scale factors, per-signal layout, yaw
 * fields, etc.) is system-independent.
 */

#include "rtcm/RtcmSsrDecoder.hpp"
#include "rtcm/RtcmBitReader.hpp"
#include "rtcm/RtcmSignalMap.hpp"
#include "math/TimeUtils.hpp"
#include <chrono>
#include <cstdio>

namespace GnssProcessor::RtcmSsr {

namespace {

// ---------------------------------------------------------------------------
// Per-system table.  Sequenced indices (system, satellite ID width, IODE
// width, satellite letter prefix, epoch field width).
// ---------------------------------------------------------------------------
struct SystemDescriptor {
    System system;
    int    sat_id_bits;
    int    iode_bits;
    char   sat_prefix;
    int    epoch_bits;     ///< DF385 (20) for GPS-time-anchored, DF389 (17) for GLO TOD.
    bool   uses_glo_tod;   ///< True for GLONASS only; epoch is TOD not GPS TOW.
};

constexpr SystemDescriptor kGps     {System::GPS,     6,  8, 'G', 20, false};
constexpr SystemDescriptor kGlonass {System::GLONASS, 5,  8, 'R', 17, true };
constexpr SystemDescriptor kGalileo {System::Galileo, 6, 10, 'E', 20, false};
constexpr SystemDescriptor kQzss    {System::QZSS,    4,  8, 'J', 20, false};
constexpr SystemDescriptor kSbas    {System::SBAS,    6,  9, 'S', 20, false};
constexpr SystemDescriptor kBeidou  {System::BeiDou,  6, 10, 'C', 20, false};

// Within each per-system block of 6 messages, the offset selects the body type.
enum class LegacyBody {
    Orbit       = 0,
    Clock       = 1,
    CodeBias    = 2,
    Combined    = 3,
    Ura         = 4,
    HighRate    = 5,
    PhaseBias,
    Unknown
};

struct LegacyClassification {
    SystemDescriptor sys{kGps};
    LegacyBody       body{LegacyBody::Unknown};
};

LegacyClassification classify(uint16_t msg_type) {
    // Phase-bias block (1265-1270): one message per system, no mod-6 layout.
    switch (msg_type) {
    case 1265: return {kGps,     LegacyBody::PhaseBias};
    case 1266: return {kGlonass, LegacyBody::PhaseBias};
    case 1267: return {kGalileo, LegacyBody::PhaseBias};
    case 1268: return {kQzss,    LegacyBody::PhaseBias};
    case 1269: return {kSbas,    LegacyBody::PhaseBias};
    case 1270: return {kBeidou,  LegacyBody::PhaseBias};
    default:   break;
    }
    // Six-message per-system blocks.
    auto withOffset = [](const SystemDescriptor& sys, int base_msg, uint16_t mt) {
        const int offset = static_cast<int>(mt) - base_msg;
        if (offset < 0 || offset > 5) return LegacyClassification{};
        return LegacyClassification{sys, static_cast<LegacyBody>(offset)};
    };
    if (msg_type >= 1057 && msg_type <= 1062) return withOffset(kGps,     1057, msg_type);
    if (msg_type >= 1063 && msg_type <= 1068) return withOffset(kGlonass, 1063, msg_type);
    if (msg_type >= 1240 && msg_type <= 1245) return withOffset(kGalileo, 1240, msg_type);
    if (msg_type >= 1246 && msg_type <= 1251) return withOffset(kQzss,    1246, msg_type);
    if (msg_type >= 1252 && msg_type <= 1257) return withOffset(kSbas,    1252, msg_type);
    if (msg_type >= 1258 && msg_type <= 1263) return withOffset(kBeidou,  1258, msg_type);
    return {};
}

// Format an SVN-style satellite identifier from a system and the raw
// per-satellite ID field.  SBAS PRNs are offset by 119 (PRN 120 = id 1).
std::string formatSat(const SystemDescriptor& sys, int raw_id) {
    char buf[8];
    if (sys.system == System::SBAS) {
        std::snprintf(buf, sizeof(buf), "S%02d", raw_id + 119);
    } else {
        std::snprintf(buf, sizeof(buf), "%c%02d", sys.sat_prefix, raw_id);
    }
    return std::string(buf);
}

// Decode a 6-bit URA index per RTCM 10403.3 Table A-15:
//     URA = 3^Q * (1 + V/4) - 1   [units: mm]
// where Q is the upper 3 bits (class) and V is the lower 3 bits (value).
// Returns 1-sigma URA in metres.  An index of 0 is special-cased as 0.
double decodeUra(uint8_t ura_index) {
    if (ura_index == 0) return 0.0;
    const int q = (ura_index >> 3) & 0x7;
    const int v = ura_index       & 0x7;
    double ura_mm = 1.0;
    for (int i = 0; i < q; ++i) ura_mm *= 3.0;
    ura_mm *= (1.0 + v / 4.0);
    ura_mm -= 1.0;
    return ura_mm * 1e-3;  // mm -> m
}

// Promote a mod-604800 GPS-TOW-second epoch into a full GnssTime using the
// caller-supplied current week.  No leap seconds applied (GPS time scale).
GnssTime promoteGpsEpoch(int current_week, uint32_t tow_s) {
    if (current_week <= 0) {
        return GnssTime{0, static_cast<double>(tow_s)};
    }
    return GnssTime{current_week, static_cast<double>(tow_s)};
}

// Promote a GLONASS time-of-day field (DF389, seconds since the start of the
// GLONASS UTC(SU)+3h day) into a GnssTime on the GPS scale.  Anchors on the
// rover's current GPS time (the same wall-clock anchor the MSM observation and
// 1020-ephemeris paths use) and carries the time-of-day back through UTC with
// the epoch's leap seconds — see TimeUtils::glonassTodToGpsTime.  Without this
// the SSR epoch lands at {0,0} and every GLONASS SSR record is dropped from the
// store as stale / IOD-mismatched.
GnssTime promoteGloTod(int /*current_week*/, uint32_t tod_s) {
    return TimeUtils::glonassTodToGpsTime(static_cast<double>(tod_s),
                                          TimeUtils::nowContinuousGpsSeconds());
}

// ---------------------------------------------------------------------------
// Shared SSR header (DF002 already consumed by RtcmParser).
// Returns false if the BitReader saturates.
// ---------------------------------------------------------------------------
bool decodeSsrHeader(RtcmBitReader& r, const SystemDescriptor& sys,
                     bool is_orbit_message, int current_gps_week,
                     SsrHeader& hdr, int& num_sats, int& datum) {
    const uint32_t epoch_raw   = r.u(sys.epoch_bits);
    const int      upd_idx     = static_cast<int>(r.u(4));
    const bool     multi_msg   = r.boolean();
    if (is_orbit_message) {
        datum = static_cast<int>(r.u(1));
    } else {
        datum = 0;
    }
    const int iod_ssr     = static_cast<int>(r.u(4));
    const int provider_id = static_cast<int>(r.u(16));
    const int sol_id      = static_cast<int>(r.u(4));
    num_sats              = static_cast<int>(r.u(6));

    if (!r.ok()) return false;

    hdr.epoch_time           = sys.uses_glo_tod
                                ? promoteGloTod(current_gps_week, epoch_raw)
                                : promoteGpsEpoch(current_gps_week, epoch_raw);
    hdr.update_interval_index = upd_idx;
    hdr.multi_message         = multi_msg;
    hdr.iod_ssr               = iod_ssr;
    hdr.provider_id           = provider_id;
    hdr.solution_id           = sol_id;
    hdr.system                = sys.system;
    return true;
}

// ---------------------------------------------------------------------------
// Receive-time stamp (system clock at decode).  Used to flag staleness.
// ---------------------------------------------------------------------------
GnssTime nowReceiveTime() {
    using namespace std::chrono;
    const auto now      = system_clock::now();
    const auto since    = duration_cast<seconds>(now.time_since_epoch()).count();
    // GPS epoch (Jan 6 1980) = 315964800 Unix; 18 leap seconds drift ignored
    // here — this is only used for staleness and ordering, not arithmetic.
    constexpr int64_t kGpsEpochUnix = 315964800;
    const int64_t since_gps = since - kGpsEpochUnix;
    if (since_gps <= 0) return GnssTime{0, 0.0};
    constexpr int kSecPerWeek = 7 * 24 * 3600;
    return GnssTime{
        static_cast<int>(since_gps / kSecPerWeek),
        static_cast<double>(since_gps % kSecPerWeek)
    };
}

// ---------------------------------------------------------------------------
// Per-body decoders.  Each consumes exactly one satellite block from @p r
// and appends one record to the output vector.
// ---------------------------------------------------------------------------

void decodeOrbitBody(RtcmBitReader& r, const SystemDescriptor& sys,
                     const SsrHeader& hdr, const GnssTime& rx,
                     std::vector<SsrOrbitCorrection>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const int iode    = static_cast<int>(r.u(sys.iode_bits));
    const double dr   = r.s(22) * 1e-4;                 // 0.1 mm
    const double da   = r.s(20) * 4e-4;                 // 0.4 mm
    const double dc   = r.s(20) * 4e-4;                 // 0.4 mm
    const double drd  = r.s(21) * 1e-6;                 // 0.001 mm/s
    const double dad  = r.s(19) * 4e-6;                 // 0.004 mm/s
    const double dcd  = r.s(19) * 4e-6;                 // 0.004 mm/s
    if (!r.ok()) return;

    SsrOrbitCorrection o;
    o.sat            = formatSat(sys, sat_raw);
    o.iode           = iode;
    o.delta_rac      = Eigen::Vector3d{dr, da, dc};
    o.delta_rac_dot  = Eigen::Vector3d{drd, dad, dcd};
    o.received_time  = rx;
    o.epoch_time     = hdr.epoch_time;     // content reference epoch (orbit dt base)
    o.iod_ssr        = hdr.iod_ssr;
    out.push_back(std::move(o));
}

void decodeClockBody(RtcmBitReader& r, const SystemDescriptor& sys,
                     const SsrHeader& hdr, const GnssTime& rx,
                     std::vector<SsrClockCorrection>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const double c0   = r.s(22) * 1e-4;                 // 0.1 mm
    const double c1   = r.s(21) * 1e-6;                 // 0.001 mm/s
    const double c2   = r.s(27) * 2e-8;                 // 0.00002 mm/s^2
    if (!r.ok()) return;

    SsrClockCorrection c;
    c.sat            = formatSat(sys, sat_raw);
    c.c0             = c0;
    c.c1             = c1;
    c.c2             = c2;
    c.received_time  = rx;
    c.epoch_time     = hdr.epoch_time;     // content reference epoch (clock polynomial dt base)
    c.iod_ssr        = hdr.iod_ssr;
    out.push_back(std::move(c));
}

void decodeCombinedBody(RtcmBitReader& r, const SystemDescriptor& sys,
                        const SsrHeader& hdr, const GnssTime& rx,
                        std::vector<SsrCombinedCorrection>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const int iode    = static_cast<int>(r.u(sys.iode_bits));
    const double dr   = r.s(22) * 1e-4;
    const double da   = r.s(20) * 4e-4;
    const double dc   = r.s(20) * 4e-4;
    const double drd  = r.s(21) * 1e-6;
    const double dad  = r.s(19) * 4e-6;
    const double dcd  = r.s(19) * 4e-6;
    const double c0   = r.s(22) * 1e-4;
    const double c1   = r.s(21) * 1e-6;
    const double c2   = r.s(27) * 2e-8;
    if (!r.ok()) return;

    SsrCombinedCorrection cb;
    cb.sat            = formatSat(sys, sat_raw);
    cb.iode           = iode;
    cb.delta_rac      = Eigen::Vector3d{dr, da, dc};
    cb.delta_rac_dot  = Eigen::Vector3d{drd, dad, dcd};
    cb.c0             = c0;
    cb.c1             = c1;
    cb.c2             = c2;
    cb.received_time  = rx;
    cb.epoch_time     = hdr.epoch_time;    // content reference epoch (orbit + clock dt base)
    cb.iod_ssr        = hdr.iod_ssr;
    out.push_back(std::move(cb));
}

void decodeCodeBiasBody(RtcmBitReader& r, const SystemDescriptor& sys,
                        const SsrHeader& hdr, const GnssTime& rx,
                        std::vector<SsrCodeBias>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const int n_sigs  = static_cast<int>(r.u(5));

    SsrCodeBias cb;
    cb.sat           = formatSat(sys, sat_raw);
    cb.received_time = rx;
    cb.iod_ssr       = hdr.iod_ssr;

    for (int i = 0; i < n_sigs; ++i) {
        const int    sig_id = static_cast<int>(r.u(5));
        const double bias_m = r.s(14) * 0.01;           // 0.01 m
        if (!r.ok()) return;
        const std::string suffix = RtcmSignalMap::toRinexSuffix(sys.system, sig_id);
        if (!suffix.empty()) {
            cb.bias_per_signal["C" + suffix] = bias_m;
        }
    }
    out.push_back(std::move(cb));
}

void decodeUraBody(RtcmBitReader& r, const SystemDescriptor& sys,
                   const SsrHeader& hdr, const GnssTime& rx,
                   std::vector<SsrUra>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const int idx     = static_cast<int>(r.u(6));
    if (!r.ok()) return;

    SsrUra u;
    u.sat            = formatSat(sys, sat_raw);
    u.ura_m          = decodeUra(static_cast<uint8_t>(idx));
    u.received_time  = rx;
    u.iod_ssr        = hdr.iod_ssr;
    out.push_back(std::move(u));
}

void decodeHighRateBody(RtcmBitReader& r, const SystemDescriptor& sys,
                        const SsrHeader& hdr, const GnssTime& rx,
                        std::vector<SsrHighRateClock>& out) {
    const int sat_raw = static_cast<int>(r.u(sys.sat_id_bits));
    const double dc   = r.s(22) * 1e-4;                 // 0.1 mm
    if (!r.ok()) return;

    SsrHighRateClock h;
    h.sat            = formatSat(sys, sat_raw);
    h.dc             = dc;
    h.received_time  = rx;
    h.iod_ssr        = hdr.iod_ssr;
    out.push_back(std::move(h));
}

void decodePhaseBiasBody(RtcmBitReader& r, const SystemDescriptor& sys,
                         const SsrHeader& hdr, const GnssTime& rx,
                         std::vector<SsrPhaseBias>& out) {
    const int sat_raw         = static_cast<int>(r.u(sys.sat_id_bits));
    const int dispersive_flag = static_cast<int>(r.u(1));
    const int mw_flag         = static_cast<int>(r.u(1));
    const int n_sigs          = static_cast<int>(r.u(5));
    const double yaw_deg      = r.u(9) * 180.0 / 256.0;        // 180/256 deg/LSB
    const double yaw_rate_dps = r.s(8) * 180.0 / 8192.0;       // 180/8192 deg/s/LSB

    SsrPhaseBias pb;
    pb.sat                          = formatSat(sys, sat_raw);
    pb.dispersive_bias_consistency  = dispersive_flag;
    pb.mw_consistency               = mw_flag;
    pb.yaw_angle_deg                = yaw_deg;
    pb.yaw_rate_deg_per_s           = yaw_rate_dps;
    pb.received_time                = rx;
    pb.iod_ssr                      = hdr.iod_ssr;

    for (int i = 0; i < n_sigs; ++i) {
        const int    sig_id      = static_cast<int>(r.u(5));
        const bool   sig_int     = r.boolean();
        const int    wl_int      = static_cast<int>(r.u(2));
        const int    disc        = static_cast<int>(r.u(4));
        const double bias_m      = r.s(20) * 1e-4;             // 0.1 mm
        if (!r.ok()) return;
        const std::string suffix = RtcmSignalMap::toRinexSuffix(sys.system, sig_id);
        if (suffix.empty()) continue;
        SsrPhaseBiasEntry e;
        e.bias_m                = bias_m;
        e.signal_integer        = sig_int;
        e.wide_lane_integer     = wl_int;
        e.discontinuity_counter = disc;
        pb.bias_per_signal["L" + suffix] = e;
    }
    out.push_back(std::move(pb));
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

DecodeResult decodeLegacy(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<SsrOrbitCorrection>&    orbits,
    std::vector<SsrClockCorrection>&    clocks,
    std::vector<SsrCombinedCorrection>& combined,
    std::vector<SsrCodeBias>&           code_biases,
    std::vector<SsrPhaseBias>&          phase_biases,
    std::vector<SsrUra>&                uras,
    std::vector<SsrHighRateClock>&      hrclocks)
{
    DecodeResult result{};

    const auto cls = classify(msg_type);
    if (cls.body == LegacyBody::Unknown || cls.sys.system == System::Unknown) {
        return result;
    }

    RtcmBitReader r(payload, len_bytes);
    r.skip(12); // DF002 already extracted by parseHeader.

    const bool is_orbit_msg = (cls.body == LegacyBody::Orbit ||
                               cls.body == LegacyBody::Combined);
    int num_sats = 0;
    int datum    = 0;
    if (!decodeSsrHeader(r, cls.sys, is_orbit_msg, current_gps_week,
                         result.header, num_sats, datum)) {
        return result;
    }

    const GnssTime rx = nowReceiveTime();

    for (int i = 0; i < num_sats; ++i) {
        switch (cls.body) {
        case LegacyBody::Orbit:     decodeOrbitBody(r, cls.sys, result.header, rx, orbits);      break;
        case LegacyBody::Clock:     decodeClockBody(r, cls.sys, result.header, rx, clocks);      break;
        case LegacyBody::CodeBias:  decodeCodeBiasBody(r, cls.sys, result.header, rx, code_biases); break;
        case LegacyBody::Combined:  decodeCombinedBody(r, cls.sys, result.header, rx, combined); break;
        case LegacyBody::Ura:       decodeUraBody(r, cls.sys, result.header, rx, uras);          break;
        case LegacyBody::HighRate:  decodeHighRateBody(r, cls.sys, result.header, rx, hrclocks); break;
        case LegacyBody::PhaseBias: decodePhaseBiasBody(r, cls.sys, result.header, rx, phase_biases); break;
        case LegacyBody::Unknown:   break;
        }
        if (!r.ok()) return result; // header populated, body truncated
    }

    result.ok = r.ok();
    return result;
}

// ---------------------------------------------------------------------------
// IGS-SSR (RTCM 4076) — wrapper around legacy per-body decoders.
// ---------------------------------------------------------------------------

namespace {

// Map IGS Message Number to (system descriptor, body type).
// IGS-SSR encodes the constellation in the TENS digit and the body in the ONES
// digit: GPS 21-27, GLONASS 41-47, Galileo 61-67, QZSS 81-87, BeiDou 101-107,
// SBAS 121-127  (so im/10 = 2,4,6,8,10,12 respectively); ones digit 1=Orbit …
// 7=URA.  Returns false for unsupported IM (NavIC 141-147, VTEC 201, etc.).
bool classifyIgsIm(int im, const SystemDescriptor*& sys_out, LegacyBody& body_out) {
    if (im < 21) return false;
    const int sys_idx  = im / 10;
    const int body_off = im % 10;
    if (body_off < 1 || body_off > 7) return false;

    switch (sys_idx) {
    case 2:  sys_out = &kGps;     break;   // IM 21-27
    case 4:  sys_out = &kGlonass; break;   // IM 41-47
    case 6:  sys_out = &kGalileo; break;   // IM 61-67
    case 8:  sys_out = &kQzss;    break;   // IM 81-87
    case 10: sys_out = &kBeidou;  break;   // IM 101-107
    case 12: sys_out = &kSbas;    break;   // IM 121-127
    default: return false;                 // 14 = NavIC/IRNSS, 20 = VTEC (IM 201), etc.
    }

    // IGS-SSR offset within each per-system block:
    //   1=Orbit, 2=Clock, 3=Combined, 4=HighRate, 5=CodeBias,
    //   6=PhaseBias, 7=URA.  This differs from legacy SSR's offset
    //   ordering — caller must NOT reuse the legacy mapping.
    switch (body_off) {
    case 1: body_out = LegacyBody::Orbit;     break;
    case 2: body_out = LegacyBody::Clock;     break;
    case 3: body_out = LegacyBody::Combined;  break;
    case 4: body_out = LegacyBody::HighRate;  break;
    case 5: body_out = LegacyBody::CodeBias;  break;
    case 6: body_out = LegacyBody::PhaseBias; break;
    case 7: body_out = LegacyBody::Ura;       break;
    default: return false;
    }
    return true;
}

} // namespace

DecodeResult decodeIgs4076(
    const uint8_t* payload,
    std::size_t    len_bytes,
    int            current_gps_week,
    std::vector<SsrOrbitCorrection>&    orbits,
    std::vector<SsrClockCorrection>&    clocks,
    std::vector<SsrCombinedCorrection>& combined,
    std::vector<SsrCodeBias>&           code_biases,
    std::vector<SsrPhaseBias>&          phase_biases,
    std::vector<SsrUra>&                uras,
    std::vector<SsrHighRateClock>&      hrclocks)
{
    DecodeResult result{};

    RtcmBitReader r(payload, len_bytes);
    r.skip(12);                                    // DF002 (= 4076)
    /*const int igs_version =*/ r.u(3);            // IGS-SSR version (DF002IGS)
    const int im = static_cast<int>(r.u(8));       // IGS Message Number
    if (!r.ok()) return result;

    const SystemDescriptor* sys_ptr = nullptr;
    LegacyBody body = LegacyBody::Unknown;
    if (!classifyIgsIm(im, sys_ptr, body)) return result;

    // IGS-SSR (RTCM 4076) uses a UNIFORM per-satellite layout for every
    // constellation, which differs from the legacy per-system SSR widths
    // transcribed into the SystemDescriptor table.  Per IGS-SSR Format v1.00
    // (cross-checked against RTKLIB's subtype>0 override and cssrlib's
    // msgtype==4076 path):
    //   IDF011 GNSS Satellite ID = uint6   (6 bits for ALL; legacy GLO=5/QZS=4),
    //   IDF012 GNSS IOD          = bit(8)   (8 bits for ALL; legacy GAL=10/BDS=10/
    //                                        SBAS=9) and there is NO IODCRC field —
    //                                        the 24-bit IODCRC is a legacy-RTCM-only
    //                                        BDS/SBAS field, absent from 4076,
    //   IDF003 SSR Epoch Time    = uint20 GPS-aligned TOW for ALL systems,
    //                              including GLONASS (no 17-bit TOD path).
    // Reusing the legacy widths here over-read the Galileo/BeiDou IOD by 2 bits,
    // corrupting the IOD AND shifting every subsequent orbit/clock field, so the
    // GAL/BDS SSR clock never IODE-matched and those constellations were stuck at
    // broadcast grade.  Apply the 4076 widths on a LOCAL copy so the legacy path
    // (decodeLegacy) keeps the per-system widths it still needs.
    SystemDescriptor sys = *sys_ptr;
    sys.sat_id_bits  = 6;
    sys.iode_bits    = 8;
    sys.epoch_bits   = 20;
    sys.uses_glo_tod = false;

    const bool is_orbit_msg = (body == LegacyBody::Orbit ||
                               body == LegacyBody::Combined);
    int num_sats = 0;
    int datum    = 0;
    if (!decodeSsrHeader(r, sys, is_orbit_msg, current_gps_week,
                         result.header, num_sats, datum)) {
        return result;
    }

    const GnssTime rx = nowReceiveTime();

    for (int i = 0; i < num_sats; ++i) {
        switch (body) {
        case LegacyBody::Orbit:     decodeOrbitBody(r, sys, result.header, rx, orbits);      break;
        case LegacyBody::Clock:     decodeClockBody(r, sys, result.header, rx, clocks);      break;
        case LegacyBody::CodeBias:  decodeCodeBiasBody(r, sys, result.header, rx, code_biases); break;
        case LegacyBody::Combined:  decodeCombinedBody(r, sys, result.header, rx, combined); break;
        case LegacyBody::Ura:       decodeUraBody(r, sys, result.header, rx, uras);          break;
        case LegacyBody::HighRate:  decodeHighRateBody(r, sys, result.header, rx, hrclocks); break;
        case LegacyBody::PhaseBias: decodePhaseBiasBody(r, sys, result.header, rx, phase_biases); break;
        case LegacyBody::Unknown:   break;
        }
        if (!r.ok()) return result;
    }

    result.ok = r.ok();
    return result;
}

// ---------------------------------------------------------------------------
// Station coordinates (1005/1006)
// ---------------------------------------------------------------------------

bool decodeStationCoords(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    RtcmStationCoords& out)
{
    if (msg_type != 1005 && msg_type != 1006) return false;

    RtcmBitReader r(payload, len_bytes);
    r.skip(12);                                 // DF002 message type
    const int station_id = static_cast<int>(r.u(12));
    r.skip(6);                                  // ITRF realization year (DF021)
    r.skip(1);                                  // GPS indicator (DF022)
    r.skip(1);                                  // GLONASS indicator (DF023)
    r.skip(1);                                  // Reserved (Galileo bit; DF024 in 3.4)
    r.skip(1);                                  // Reference-station indicator (DF141)
    const int64_t x_raw = r.s64(38);            // 0.0001 m units
    r.skip(1);                                  // Single-receiver oscillator (DF142)
    r.skip(1);                                  // Reserved
    const int64_t y_raw = r.s64(38);
    r.skip(2);                                  // Quarter-cycle indicator (DF364)
    const int64_t z_raw = r.s64(38);

    if (!r.ok()) return false;

    out.reference_station_id = station_id;
    out.arp_ecef = Eigen::Vector3d{
        static_cast<double>(x_raw) * 1e-4,
        static_cast<double>(y_raw) * 1e-4,
        static_cast<double>(z_raw) * 1e-4
    };

    if (msg_type == 1006) {
        const uint32_t height_raw = r.u(16);    // 0.0001 m units
        if (!r.ok()) return false;
        out.antenna_height = static_cast<double>(height_raw) * 1e-4;
        out.has_height     = true;
    } else {
        out.antenna_height = 0.0;
        out.has_height     = false;
    }

    return true;
}

} // namespace GnssProcessor::RtcmSsr
