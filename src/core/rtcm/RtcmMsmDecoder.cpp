/**
 * @file RtcmMsmDecoder.cpp
 * @brief MSM4/MSM5/MSM7 decoder for GPS/GLO/GAL/QZS/BDS/SBAS.
 *
 * MSM7 layout (RTCM 10403.3 sec. 3.5.16 + Annex A):
 *
 *   Header
 *     DF002 message type                      12 bits  (skipped; read by the dispatcher)
 *     DF003 reference station ID              12
 *     System-specific epoch (DF004/16/248/...) 30      (GLONASS: 3-bit day of week
 *                                                       + 27-bit time of day)
 *     DF393 multiple message bit               1
 *     DF409 IODS                               3
 *     reserved                                 7
 *     DF411 clock steering indicator           2
 *     DF412 external clock indicator           2
 *     DF417 smoothing type                     1
 *     DF418 smoothing interval                 3
 *     DF394 sat mask                          64
 *     DF395 sig mask                          32
 *     DF396 cell mask                  Nsat × Nsig
 *
 *   Per satellite (each field packed as one block over all Nsat satellites):
 *     DF397 rough range int (ms)               8
 *     DF419 ext sat info (all systems in MSM5/7) 4   (GLONASS: freq channel)
 *     DF398 rough range mod 1 ms (units 2^-10 ms) 10
 *     DF399 rough phase range rate (m/s)      14  signed
 *
 *   Per cell (Ncell = popcount(cell_mask); each field packed as one block over all cells):
 *     DF405 fine pseudorange    20 signed   units 2^-29 ms
 *     DF406 fine phase range    24 signed   units 2^-31 ms
 *     DF407 lock time           10          extended-range table
 *     DF420 half-cycle amb       1
 *     DF408 CNR                 10          units 1/16 dB-Hz
 *     DF404 fine phase range rate 15 signed units 0.0001 m/s
 *
 *   MSM4 and MSM5 use DF400 (15 bits, 2^-24 ms), DF401 (22 bits, 2^-29 ms),
 *   DF402 (4-bit lock time) and DF403 (6-bit CNR, whole dB-Hz); MSM4 has no
 *   DF419, DF399 or DF404 (see kMsm4 / kMsm5).
 *
 * Reconstruction:
 *   range_ms = rough_int + rough_mod·2^-10 + fine·2^-29
 *   pseudorange (m) = range_ms · 1e-3 · c
 *   phase     (m)   = (rough_int + rough_mod·2^-10 + fine_phase·2^-31) · 1e-3 · c
 *   phase     (cyc) = phase (m) / wavelength
 *   doppler   (Hz)  = -range_rate (m/s) / wavelength
 *     where range_rate = rough_phase_range_rate (m/s) + fine_phase_range_rate (m/s)
 */

#include "rtcm/RtcmMsmDecoder.hpp"
#include "rtcm/RtcmBitReader.hpp"
#include "rtcm/RtcmSatId.hpp"
#include "rtcm/RtcmSignalMap.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include "logging/SafeLogger.hpp"
#include <bitset>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace GnssProcessor::RtcmMsm {

namespace {

/// Per-call logger for the MSM decode path.  logging/SafeLogger.hpp explains
/// why the bare spdlog::xxx() free functions are unsafe here.
std::shared_ptr<spdlog::logger> msmLog() {
    return Logging::safeDefaultLogger();
}

// ---------------------------------------------------------------------------
// Per-system descriptor
// ---------------------------------------------------------------------------
struct SystemDesc {
    System system;
    char   sat_prefix;
    int    epoch_bits;        ///< 30 for all systems.  GLONASS also carries 30
                              ///< bits, but interprets them as 3-bit day-of-week
                              ///< + 27-bit time-of-day (see promoteEpoch), whereas
                              ///< the others are a flat 30-bit ms-of-week.
    bool   uses_glo_tod;
    bool   has_ext_sat_info;  ///< True if this system assigns DF419 a meaning (GLONASS
                              ///< FDMA freq channel).  DF419 is present in MSM5/7
                              ///< for all systems regardless; this flag describes
                              ///< interpretation only, not bitstream consumption.
                              ///< The decode body does not read it; it keys the
                              ///< GLONASS channel on the system.
    double bdt_to_gps_offset_s; ///< 14 for BDS (GPS time = BDT + 14 s), 0 otherwise.
};

constexpr SystemDesc kGps     {System::GPS,     'G', 30, false, false,  0.0};
constexpr SystemDesc kGlonass {System::GLONASS, 'R', 30, true,  true,   0.0};
constexpr SystemDesc kGalileo {System::Galileo, 'E', 30, false, false,  0.0};
constexpr SystemDesc kSbas    {System::SBAS,    'S', 30, false, false,  0.0};
constexpr SystemDesc kQzss    {System::QZSS,    'J', 30, false, false,  0.0};
constexpr SystemDesc kBeidou  {System::BeiDou,  'C', 30, false, false, 14.0};

// ---------------------------------------------------------------------------
// MSM variant (field widths and feature flags).  MSM4/MSM5/MSM7 share the
// same overall structure but differ in per-cell / per-sat field widths and
// capability flags.  MSM6 (high precision, no Doppler) is not implemented.
// ---------------------------------------------------------------------------
struct MsmVariant {
    int    msm_subtype;            ///< 4, 5 or 7.
    int    bits_fine_pr;
    int    bits_fine_phase;
    int    bits_lock;
    int    bits_cnr;
    bool   has_range_rate;         ///< Per-sat rough + per-cell fine, only in MSM5/7.
    bool   honour_ext_sat_info;    ///< DF419 present in the bitstream (MSM5/MSM7):
                                   ///< read 4·Nsat bits for every system, not just GLONASS.
    double scale_fine_pr_ms;       ///< 2^-24 (MSM4/MSM5) or 2^-29 (MSM7).
    double scale_fine_phase_ms;    ///< 2^-29 (MSM4/MSM5) or 2^-31 (MSM7).
    double cnr_unit_dbhz;          ///< 1.0 (MSM4/MSM5) or 1.0/16.0 (MSM7).
    int    sentinel_fine_pr;       ///< Invalid-cell sentinel for fine pseudorange.
    int    sentinel_fine_phase;    ///< Invalid-cell sentinel for fine phase range.
};

// Note: scale factors are written as 1.0 / (1ull << N) for N = 24/29/31, which
// is exact and constexpr on every compiler.  Equivalent to std::pow(2,-N).
constexpr MsmVariant kMsm4 {
    /*msm_subtype=*/4,
    /*bits_fine_pr=*/15,  /*bits_fine_phase=*/22,
    /*bits_lock=*/4,      /*bits_cnr=*/6,
    /*has_range_rate=*/false,
    /*honour_ext_sat_info=*/false,
    /*scale_fine_pr_ms=*/  1.0 / (1ull << 24),
    /*scale_fine_phase_ms=*/1.0 / (1ull << 29),
    /*cnr_unit_dbhz=*/1.0,
    /*sentinel_fine_pr=*/-(1 << 14),     // -16384
    /*sentinel_fine_phase=*/-(1 << 21),  // -2097152
};

// MSM5: MSM4-precision range fields (DF400/DF401, 4-bit DF402 lock,
// whole-dB-Hz DF403 CNR) plus the Doppler fields of MSM7 (per-sat DF399 rough
// and per-cell DF404 fine phase range rate).  As an MSM5/7-family message it
// also carries the per-sat DF419 extended-satellite-info block for every
// constellation (RTCM 10403.3 Table 3.5-75) (RTB-2(c)).
constexpr MsmVariant kMsm5 {
    /*msm_subtype=*/5,
    /*bits_fine_pr=*/15,  /*bits_fine_phase=*/22,
    /*bits_lock=*/4,      /*bits_cnr=*/6,
    /*has_range_rate=*/true,
    /*honour_ext_sat_info=*/true,
    /*scale_fine_pr_ms=*/  1.0 / (1ull << 24),
    /*scale_fine_phase_ms=*/1.0 / (1ull << 29),
    /*cnr_unit_dbhz=*/1.0,
    /*sentinel_fine_pr=*/-(1 << 14),     // -16384
    /*sentinel_fine_phase=*/-(1 << 21),  // -2097152
};

constexpr MsmVariant kMsm7 {
    /*msm_subtype=*/7,
    /*bits_fine_pr=*/20,  /*bits_fine_phase=*/24,
    /*bits_lock=*/10,     /*bits_cnr=*/10,
    /*has_range_rate=*/true,
    /*honour_ext_sat_info=*/true,
    /*scale_fine_pr_ms=*/  1.0 / (1ull << 29),
    /*scale_fine_phase_ms=*/1.0 / (1ull << 31),
    /*cnr_unit_dbhz=*/1.0 / 16.0,
    /*sentinel_fine_pr=*/-(1 << 19),     // -524288
    /*sentinel_fine_phase=*/-(1 << 23),  // -8388608
};

const SystemDesc* descForMsgType(uint16_t mt, int expected_sub, int& subtype_out) {
    // MSM4 messages: 1074, 1084, 1094, 1104, 1114, 1124.
    // MSM5 messages: 1075, 1085, 1095, 1105, 1115, 1125.
    // MSM7 messages: 1077, 1087, 1097, 1107, 1117, 1127.
    if (mt < 1071) return nullptr;
    const int sys_index = (mt - 1071) / 10;
    const int sub       = (mt - 1071) % 10 + 1;
    if (sub != expected_sub) { subtype_out = 0; return nullptr; }
    subtype_out = sub;
    switch (sys_index) {
    case 0: return &kGps;
    case 1: return &kGlonass;
    case 2: return &kGalileo;
    case 3: return &kSbas;
    case 4: return &kQzss;
    case 5: return &kBeidou;
    default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Lock-time decode (DF407, MSM6/MSM7).  Returns lock duration in ms.
// Source: RTCM 10403.3 Table 3.5-101.  n = 0..63 maps to n ms; each following
// group of 32 values starts at double the previous start and doubles the step.
// ---------------------------------------------------------------------------
double lockTimeMsExtended(int n) {
    if (n < 64)  return  static_cast<double>(n);
    if (n < 96)  return  64.0     + (n -  64) *      2.0;
    if (n < 128) return  128.0    + (n -  96) *      4.0;
    if (n < 160) return  256.0    + (n - 128) *      8.0;
    if (n < 192) return  512.0    + (n - 160) *     16.0;
    if (n < 224) return  1024.0   + (n - 192) *     32.0;
    if (n < 256) return  2048.0   + (n - 224) *     64.0;
    if (n < 288) return  4096.0   + (n - 256) *    128.0;
    if (n < 320) return  8192.0   + (n - 288) *    256.0;
    if (n < 352) return  16384.0  + (n - 320) *    512.0;
    if (n < 384) return  32768.0  + (n - 352) *   1024.0;
    if (n < 416) return  65536.0  + (n - 384) *   2048.0;
    if (n < 448) return  131072.0 + (n - 416) *   4096.0;
    if (n < 480) return  262144.0 + (n - 448) *   8192.0;
    if (n < 512) return  524288.0 + (n - 480) *  16384.0;
    // DF407 is a full 10-bit field (0..1023), and the doubling pattern of
    // Table 3.5-101 continues past n=511: n=512..703 are valid long locks
    // (~17.5 min to ~18.4 h).  Mapping them to 0.0 would make a long-held
    // carrier look like a lock reset to LockTimeTracker and raise an LLI slip
    // on every epoch.  n >= 704 saturates at the top level (2^26 ms), so a
    // stable lock reads as equal maxima (RTB-3).
    if (n < 544) return  1048576.0  + (n - 512) *   32768.0;
    if (n < 576) return  2097152.0  + (n - 544) *   65536.0;
    if (n < 608) return  4194304.0  + (n - 576) *  131072.0;
    if (n < 640) return  8388608.0  + (n - 608) *  262144.0;
    if (n < 672) return  16777216.0 + (n - 640) *  524288.0;
    if (n < 704) return  33554432.0 + (n - 672) * 1048576.0;
    return 67108864.0;  // n >= 704: saturate at the top step (~18.6 h)
}

// MSM4/MSM5 4-bit lock time (DF402): n=0 → 0 ms; n=1..15 → 32·2^(n-1) ms.
// Range 32 ms .. 524288 ms (~8.7 minutes).
double lockTimeMsBasic(int n) {
    if (n <= 0 || n > 15) return 0.0;
    return 32.0 * static_cast<double>(1ull << (n - 1));
}

// ---------------------------------------------------------------------------
// Wavelength lookup keyed by RINEX 3 obs-code suffix.
// Returns 0.0 if unknown (caller falls back: phase_m stays in metres).
// Suffix is the two-character RINEX code, e.g. "1C", "5X".
// ---------------------------------------------------------------------------
double wavelengthForSuffix(System sys, const std::string& suffix) {
    if (suffix.empty()) return 0.0;
    const char band = suffix[0];
    switch (sys) {
    case System::GPS:
        if (band == '1') return Constants::GPS_L1_WAVELENGTH;
        if (band == '2') return Constants::GPS_L2_WAVELENGTH;
        if (band == '5') return Constants::GPS_L5_WAVELENGTH;
        return 0.0;
    case System::Galileo:
        if (band == '1') return Constants::GAL_E1_WAVELENGTH;
        if (band == '5') return Constants::GAL_E5A_WAVELENGTH;
        if (band == '7') return Constants::GAL_E5B_WAVELENGTH;
        if (band == '8') return Constants::GAL_E5_WAVELENGTH;
        if (band == '6') return Constants::GAL_E6_WAVELENGTH;
        return 0.0;
    case System::BeiDou:
        if (band == '2') return Constants::BDS_B1_WAVELENGTH;     // B1I
        if (band == '1') return Constants::BDS_B1C_WAVELENGTH;
        if (band == '5') return Constants::BDS_B2A_WAVELENGTH;
        if (band == '7') return Constants::BDS_B2_WAVELENGTH;
        if (band == '8') return Constants::BDS_B2AB_WAVELENGTH;
        if (band == '6') return Constants::BDS_B3_WAVELENGTH;
        return 0.0;
    case System::QZSS:
        if (band == '1') return Constants::QZSS_L1_WAVELENGTH;
        if (band == '2') return Constants::QZSS_L2_WAVELENGTH;
        if (band == '5') return Constants::QZSS_L5_WAVELENGTH;
        if (band == '6') return Constants::QZSS_L6_WAVELENGTH;
        return 0.0;
    case System::SBAS:
        if (band == '1') return Constants::SBAS_L1_WAVELENGTH;
        if (band == '5') return Constants::SBAS_L5_WAVELENGTH;
        return 0.0;
    case System::GLONASS:
        // FDMA: wavelength is per-satellite (depends on the slot channel k).
        // Handled by glonassWavelength() below, which the reconstruction loop
        // calls instead of this for GLONASS; reaching here means no channel was
        // available, so 0.0 keeps the phase in metres (as for an unmapped id).
        return 0.0;
    default:
        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// GLONASS FDMA helpers.  GLONASS L1/L2 are frequency-division: each satellite
// transmits on a slot-specific carrier f = base + k·offset, so the wavelength
// is per-satellite, keyed by the FDMA channel number k (-7..+6).
// ---------------------------------------------------------------------------

// Decode the RTCM DF419 "Extended Satellite Information" (4 bits) into the FDMA
// channel k.  Per RTCM 10403.3 / the RTKLIB reference, value v maps to k = v-7
// for v in [0,13]; 14 and 15 are reserved/unknown.  Returns INT_MIN for a
// reserved value.  When DF419 is absent (MSM4/6) the caller uses INT_MIN directly.
int glonassFcnFromDf419(int ext_sat_info) {
    return (ext_sat_info >= 0 && ext_sat_info <= 13) ? (ext_sat_info - 7) : INT_MIN;
}

// Per-satellite GLONASS wavelength (m) for a RINEX-3 obs-code suffix.  The FDMA
// bands (G1='1', G2='2') need the channel k; the CDMA bands (G3='3', G1a='4',
// G2a='6') are fixed.  Returns 0.0 when the wavelength cannot be resolved
// (unknown band, or an FDMA band with no channel); the caller then carries the
// phase in metres, as for an unmapped signal.
double glonassWavelength(const std::string& suffix, int fcn) {
    if (suffix.empty()) return 0.0;
    switch (suffix[0]) {
    case '1':
        if (fcn == INT_MIN) return 0.0;
        return Constants::SPEED_OF_LIGHT
             / (Constants::GLONASS_G1_BASE_FREQ + fcn * Constants::GLONASS_G1_CHANNEL_OFFSET);
    case '2':
        if (fcn == INT_MIN) return 0.0;
        return Constants::SPEED_OF_LIGHT
             / (Constants::GLONASS_G2_BASE_FREQ + fcn * Constants::GLONASS_G2_CHANNEL_OFFSET);
    case '3': return Constants::GLONASS_G3_WAVELENGTH;    // G3  CDMA (fixed)
    case '4': return Constants::GLONASS_G1A_WAVELENGTH;   // G1a CDMA (fixed)
    case '6': return Constants::GLONASS_G2A_WAVELENGTH;   // G2a CDMA (fixed)
    default:  return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Sat ID formatting from the 0-based satellite mask bit (identifier = bit + 1).
// Most systems use the identifier as the PRN or slot ("G05").  SBAS identifier 1
// is PRN 120, which RINEX and the rest of the project name "S20" (PRN - 100), so
// the first SBAS bit formats as "S20" (see rtcm/RtcmSatId.hpp).
// ---------------------------------------------------------------------------
std::string formatSat(const SystemDesc& sys, int bit_index) {
    if (sys.system == System::SBAS) return RtcmSatId::sbas(bit_index + 1);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d", sys.sat_prefix, bit_index + 1);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Time conversions
// ---------------------------------------------------------------------------

// Nearest-continuation week promotion (RTB-1(a); RTKLIB adjweek).  Given a
// mod-week time @p tow and a continuous-GPS-seconds anchor (the caller's
// last-known stream time, host clock only at cold start), pick the week that
// minimizes |week·604800 + tow − anchor|.  A non-positive anchor returns
// week 0 ("no week available"), which direct decoder calls (tests) rely on.
int nearestWeek(double anchor_gps_seconds, double tow) {
    if (anchor_gps_seconds <= 0.0) return 0;
    const int w = static_cast<int>(std::floor(
        (anchor_gps_seconds - tow) / static_cast<double>(TimeUtils::SECONDS_IN_WEEK) + 0.5));
    return w < 0 ? 0 : w;
}

GnssTime promoteEpoch(const SystemDesc& sys, uint32_t raw, double anchor_gps_seconds) {
    if (sys.uses_glo_tod) {
        // The GLONASS MSM epoch is a 30-bit field: 3-bit day-of-week (high bits)
        // + 27-bit time-of-day in ms (low bits).  decodeMsmImpl reads all 30
        // bits (epoch_bits); reading only 27 would leave the header 3 bits short and misalign the
        // satellite mask and everything after it.
        //
        // GLONASS runs on the UTC(SU)+3h ("Moscow") day, so the ms-of-day is
        // promoted to a full GPS-week GnssTime by anchoring on the caller's
        // last-known continuous GPS time (host clock only at cold start,
        // RTB-1(a)) and converting through UTC (leap seconds); see
        // TimeUtils::glonassTodToGpsTime, which resolves the midnight wrap to
        // within ±12 h of the anchor.  The 3-bit day-of-week is therefore not
        // needed and its value is not used.
        const uint32_t tod_ms = raw & 0x7FFFFFFu;       // DF034 low 27 bits (ms-of-day)
        return TimeUtils::glonassTodToGpsTime(static_cast<double>(tod_ms) * 1e-3,
                                              anchor_gps_seconds);
    }
    // 30-bit ms-of-week, promoted to the week nearest the anchor.  The BDT→GPS
    // +14 s shift can wrap the tow across the week boundary; after re-wrapping
    // into [0, 604800) the nearest-continuation pick lands on the correct
    // (possibly incremented) week automatically.
    double tow = static_cast<double>(raw) * 1e-3;
    if (sys.bdt_to_gps_offset_s != 0.0) {
        tow += sys.bdt_to_gps_offset_s;             // BDT -> GPS
        if (tow >= TimeUtils::SECONDS_IN_WEEK) tow -= TimeUtils::SECONDS_IN_WEEK;
    }
    return GnssTime{nearestWeek(anchor_gps_seconds, tow), tow};
}

// ---------------------------------------------------------------------------
// Cell mask helpers
// ---------------------------------------------------------------------------
struct SatEntry {
    int  bit_index;          ///< 0-63 position in the 64-bit sat mask.
    std::string sat_id;      ///< Pre-formatted "G05" etc.
    int  rough_range_int{0};
    int  ext_sat_info{0};
    int  rough_range_mod{0};
    int  rough_range_rate{0};
};

struct SignalEntry {
    int         signal_id;   ///< 1..32 (1-indexed within RtcmSignalMap).
    std::string suffix;      ///< "1C", "5X", … (may be empty if reserved).
};

} // namespace

// ---------------------------------------------------------------------------
// Shared decode body, parameterised over the MSM variant.
// ---------------------------------------------------------------------------
MsmDecodeResult decodeMsmImpl(
    const uint8_t*    payload,
    std::size_t       len_bytes,
    uint16_t          msg_type,
    double            anchor_gps_seconds,
    const MsmVariant& V,
    std::vector<RinexObservation>& observations)
{
    MsmDecodeResult res{};
    int subtype = 0;
    const SystemDesc* sys = descForMsgType(msg_type, V.msm_subtype, subtype);
    if (!sys) return res;
    res.system      = sys->system;
    res.msm_subtype = subtype;

    RtcmBitReader r(payload, len_bytes);
    r.skip(12);                                     // DF002 msg type

    // ------------------ Header ------------------
    res.station_id      = static_cast<int>(r.u(12));
    const uint32_t epoch_raw = r.u(sys->epoch_bits);
    res.multi_message   = r.boolean();
    /*int iods           =*/ r.u(3);
    /*int reserved       =*/ r.u(7);
    /*int clock_steer    =*/ r.u(2);
    /*int ext_clock      =*/ r.u(2);
    /*bool smoothing     =*/ r.boolean();
    /*int smooth_int     =*/ r.u(3);

    res.epoch_time = promoteEpoch(*sys, epoch_raw, anchor_gps_seconds);

    // Satellite mask (64 bits): bit i (counted from the MSB) set => satellite (i+1) present.
    const uint64_t sat_mask_hi = r.u(32);
    const uint64_t sat_mask_lo = r.u(32);
    const uint64_t sat_mask    = (sat_mask_hi << 32) | sat_mask_lo;
    if (!r.ok()) return res;

    // Signal mask (32 bits): bit i (counted from the MSB) set => RTCM signal id (i+1) present.
    const uint32_t sig_mask = r.u(32);
    if (!r.ok()) return res;

    // Build the satellite list (in mask-bit order, i.e. ascending PRN).
    std::vector<SatEntry> sats;
    sats.reserve(64);
    for (int b = 0; b < 64; ++b) {
        if (sat_mask & (1ull << (63 - b))) {
            SatEntry s;
            s.bit_index = b;
            s.sat_id    = formatSat(*sys, b);
            sats.push_back(std::move(s));
        }
    }
    res.n_satellites = static_cast<int>(sats.size());

    // Build the signal list.
    std::vector<SignalEntry> sigs;
    sigs.reserve(32);
    for (int b = 0; b < 32; ++b) {
        if (sig_mask & (1u << (31 - b))) {
            SignalEntry sg;
            sg.signal_id = b + 1;
            sg.suffix    = RtcmSignalMap::toRinexSuffix(sys->system, sg.signal_id);
            sigs.push_back(std::move(sg));
        }
    }
    res.n_signals = static_cast<int>(sigs.size());

    // Cell mask: Nsat × Nsig bits, row-major (sat-major).
    const int n_cell_bits = res.n_satellites * res.n_signals;
    if (n_cell_bits <= 0) { res.bits_consumed = r.pos(); res.ok = r.ok(); return res; }
    std::vector<bool> cell_mask(n_cell_bits, false);
    for (int i = 0; i < n_cell_bits; ++i) {
        cell_mask[i] = r.boolean();
        if (cell_mask[i]) ++res.n_cells;
    }
    if (!r.ok()) return res;

    // ------------------ Per-satellite block ------------------
    for (auto& s : sats) s.rough_range_int = static_cast<int>(r.u(8));
    // DF419 (Extended Satellite Information, 4 bits/sat) is present in the
    // bitstream for every constellation in MSM5/MSM7, not only GLONASS.  Only
    // GLONASS assigns it a meaning (the FDMA frequency-channel number); for the
    // other systems it is reserved, but the 4·Nsat bits are still there and have
    // to be consumed, or the rough-range-modulo block and the whole per-cell
    // block are read 4·Nsat bits early.  The read is gated on the MSM variant
    // alone (true for MSM5/MSM7, false for MSM4/MSM6), as in RTCM 10403.3 and
    // the cssrlib reference (rtcm.py: `if msm==5 or msm==7`).
    if (V.honour_ext_sat_info) {
        for (auto& s : sats) s.ext_sat_info = static_cast<int>(r.u(4));
    }
    for (auto& s : sats) s.rough_range_mod  = static_cast<int>(r.u(10));
    if (V.has_range_rate) {
        for (auto& s : sats) s.rough_range_rate = static_cast<int>(r.s(14));
    }
    if (!r.ok()) return res;

    // ------------------ Per-cell block ------------------
    // RTCM packs each field block contiguously across all cells, in cell
    // order (which is row-major sat × sig).  The blocks are decoded into
    // parallel vectors and recombined below.  Field widths come from the
    // variant table; range-rate fields are absent in MSM4.
    const int N = res.n_cells;
    std::vector<int>  fine_pseudorange(N, 0);
    std::vector<int>  fine_phaserange (N, 0);
    std::vector<int>  lock_time       (N, 0);
    std::vector<int>  half_amb        (N, 0);
    std::vector<int>  cnr             (N, 0);
    std::vector<int>  fine_rate       (N, 0);

    for (int i = 0; i < N; ++i) fine_pseudorange[i] = static_cast<int>(r.s(V.bits_fine_pr));
    for (int i = 0; i < N; ++i) fine_phaserange [i] = static_cast<int>(r.s(V.bits_fine_phase));
    for (int i = 0; i < N; ++i) lock_time       [i] = static_cast<int>(r.u(V.bits_lock));
    for (int i = 0; i < N; ++i) half_amb        [i] = static_cast<int>(r.u(1));
    for (int i = 0; i < N; ++i) cnr             [i] = static_cast<int>(r.u(V.bits_cnr));
    if (V.has_range_rate) {
        for (int i = 0; i < N; ++i) fine_rate   [i] = static_cast<int>(r.s(15));
    }
    if (!r.ok()) return res;

    // ------------------ Reconstruction ------------------
    // RTCM-defined sentinel values for "invalid".  The fine-pseudorange
    // and fine-phase sentinels come from the variant; per-sat sentinels
    // (range_int=0xFF, rough_rate=−2¹³) and the 15-bit fine-rate sentinel
    // (−2¹⁴) are constant across MSM4-7.
    const int kInvalidFinePseudo = V.sentinel_fine_pr;
    const int kInvalidFinePhase  = V.sentinel_fine_phase;
    constexpr int kInvalidFineRate   = -(1 << 14);   // -16384
    constexpr int kInvalidRangeInt   = 255;          // 0xFF
    constexpr int kInvalidRoughRate  = -(1 << 13);   // -8192

    const double c = Constants::SPEED_OF_LIGHT;

    // Optional one-satellite raw-field diagnostic.  Set GNSS_RTCM_DUMP_SAT=G05
    // (or any sat id) to log at warn level, per epoch, the raw DF397/DF398
    // rough-range integers, the per-signal raw fine-pseudorange integer (DF405,
    // or DF400 in MSM4/MSM5; the log labels it DF405raw), and the reconstructed
    // metres, so the decoded fields can be checked one by one against an
    // independent decoder (RTKLIB / pyrtcm / cssrlib) on the same frame.  The
    // variable is read once (the environment is process-stable); no cost when unset.
    static const char* const kDumpSat = std::getenv("GNSS_RTCM_DUMP_SAT");

    observations.reserve(observations.size() + sats.size());

    int cell_idx = 0;
    for (int si = 0; si < res.n_satellites; ++si) {
        const auto& sat = sats[si];
        const bool dump = kDumpSat && sat.sat_id == kDumpSat;
        if (sat.rough_range_int == kInvalidRangeInt) {
            // Skip cells for this satellite.
            for (int gi = 0; gi < res.n_signals; ++gi) {
                if (cell_mask[si * res.n_signals + gi]) ++cell_idx;
            }
            continue;
        }

        // Rough range in seconds (for both pseudorange and phase reconstruction).
        const double rough_s =
            (static_cast<double>(sat.rough_range_int)
           + static_cast<double>(sat.rough_range_mod) / 1024.0) * 1e-3;

        if (dump) {
            msmLog()->warn("MSM-DUMP {} mt={} sub={} DF397={} DF398={} rough_s={:.12f} "
                           "rough_m={:.3f}",
                           sat.sat_id, msg_type, V.msm_subtype, sat.rough_range_int,
                           sat.rough_range_mod, rough_s, rough_s * c);
        }

        // Rough phase range rate (m/s).  Present only in MSM5/MSM7, where the
        // sentinel −2¹³ marks it invalid; absent or invalid gives NaN.
        const double rough_rate_mps = (!V.has_range_rate
                                       || sat.rough_range_rate == kInvalidRoughRate)
                                        ? std::nan("")
                                        : static_cast<double>(sat.rough_range_rate);

        RinexObservation obs;
        obs.satellite = sat.sat_id;
        obs.system    = sys->system;
        obs.time      = res.epoch_time;

        // GLONASS FDMA channel from DF419.  ext_sat_info is used only when DF419
        // was present in the bitstream (MSM5/MSM7); on MSM4/MSM6 the field stays
        // at its default 0, which would decode to a false k=-7 for every
        // satellite, so INT_MIN (unknown) is used instead.  The channel sets the
        // per-sat wavelength below and is carried on the observation so
        // downstream consumers (PppUtils slot map, MW combinations) can resolve
        // frequency.
        const int glo_fcn = (sys->system == System::GLONASS && V.honour_ext_sat_info)
                              ? glonassFcnFromDf419(sat.ext_sat_info) : INT_MIN;
        if (sys->system == System::GLONASS) obs.glonass_freq_channel = glo_fcn;

        for (int gi = 0; gi < res.n_signals; ++gi) {
            if (!cell_mask[si * res.n_signals + gi]) continue;
            const int idx = cell_idx++;
            const auto& sig = sigs[gi];
            if (sig.suffix.empty()) continue;            // unmapped signal id

            // Pseudorange (m).
            if (fine_pseudorange[idx] != kInvalidFinePseudo) {
                const double rng_s = rough_s
                    + static_cast<double>(fine_pseudorange[idx]) * V.scale_fine_pr_ms * 1e-3;
                obs.measurements["C" + sig.suffix] = rng_s * c;
                if (dump) {
                    const double fine_m =
                        static_cast<double>(fine_pseudorange[idx]) * V.scale_fine_pr_ms * 1e-3 * c;
                    msmLog()->warn("MSM-DUMP {} C{} cell={} DF405raw={} fine_m={:+.3f} "
                                   "C{}={:.3f}",
                                   sat.sat_id, sig.suffix, idx, fine_pseudorange[idx],
                                   fine_m, sig.suffix, rng_s * c);
                }
            }
            // Phase (cycles, RINEX-3 convention).  When no wavelength resolves
            // (wl = 0), the value is stored in metres under the same L code.
            if (fine_phaserange[idx] != kInvalidFinePhase) {
                const double phase_s = rough_s
                    + static_cast<double>(fine_phaserange[idx]) * V.scale_fine_phase_ms * 1e-3;
                const double phase_m = phase_s * c;
                const double wl      = (sys->system == System::GLONASS)
                                         ? glonassWavelength(sig.suffix, glo_fcn)
                                         : wavelengthForSuffix(sys->system, sig.suffix);
                obs.measurements["L" + sig.suffix] = (wl > 0.0) ? phase_m / wl : phase_m;
            }
            // CNR (dB-Hz): DF403 is whole dB-Hz in MSM4/MSM5, DF408 is 1/16 dB-Hz
            // in MSM7.  A zero CNR is treated as absent.
            if (cnr[idx] != 0) {
                obs.measurements["S" + sig.suffix] =
                    static_cast<double>(cnr[idx]) * V.cnr_unit_dbhz;
            }
            // Doppler (Hz), only in MSM5/MSM7: rough + fine range rate, converted
            // to Hz via the carrier wavelength as D = -rate / wl.  No D value is
            // stored when the wavelength is unknown.
            if (V.has_range_rate
                && fine_rate[idx] != kInvalidFineRate
                && !std::isnan(rough_rate_mps)) {
                const double rate_mps = rough_rate_mps
                    + static_cast<double>(fine_rate[idx]) * 1e-4;
                const double wl = (sys->system == System::GLONASS)
                                    ? glonassWavelength(sig.suffix, glo_fcn)
                                    : wavelengthForSuffix(sys->system, sig.suffix);
                if (wl > 0.0) {
                    obs.measurements["D" + sig.suffix] = -rate_mps / wl;
                }
            }
            // LLI: the 0x2 half-cycle bit is set here from DF420.  The 0x1
            // loss-of-lock bit needs history (lock time decreased against the
            // previous epoch), which a stateless decoder cannot supply, so the
            // decoded lock time goes on lock_time_ms below and the epoch-assembly
            // layer (RealTimeEngine + LockTimeTracker, behind
            // realtime.msm_lli_from_locktime) derives the 0x1 bit.
            int lli = 0;
            if (half_amb[idx]) lli |= 0x2;
            obs.lli_flags["L" + sig.suffix] = lli;
            // Decoded lock time (ms) for the assembly-layer LLI derivation:
            // the 4-bit DF402 basic table (MSM4/MSM5) or the 10-bit DF407
            // extended table (MSM7), selected by the variant's field width.
            obs.lock_time_ms["L" + sig.suffix] =
                (V.bits_lock >= 10) ? lockTimeMsExtended(lock_time[idx])
                                    : lockTimeMsBasic(lock_time[idx]);
        }

        if (!obs.measurements.empty()) observations.push_back(std::move(obs));
    }

    res.bits_consumed = r.pos();
    res.ok = r.ok();
    return res;
}

// ---------------------------------------------------------------------------
// Public entry points.  All forward to decodeMsmImpl with the matching
// variant.  The variants share the same body code; the variant struct
// supplies field widths, scale factors, and capability flags.
// ---------------------------------------------------------------------------

MsmDecodeResult decodeMsm7(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations)
{
    return decodeMsmImpl(payload, len_bytes, msg_type, anchor_gps_seconds,
                         kMsm7, observations);
}

MsmDecodeResult decodeMsm5(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations)
{
    return decodeMsmImpl(payload, len_bytes, msg_type, anchor_gps_seconds,
                         kMsm5, observations);
}

MsmDecodeResult decodeMsm4(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    double         anchor_gps_seconds,
    std::vector<RinexObservation>& observations)
{
    return decodeMsmImpl(payload, len_bytes, msg_type, anchor_gps_seconds,
                         kMsm4, observations);
}

} // namespace GnssProcessor::RtcmMsm
