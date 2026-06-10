/**
 * @file RtcmMsmDecoder.cpp
 * @brief MSM7 decoder for GPS/GLO/GAL/QZS/BDS/SBAS.
 *
 * MSM7 layout (RTCM 10403.3 sec. 3.5.16 + Annex A):
 *
 *   Header
 *     DF002 message type                      12 bits  (consumed by dispatcher)
 *     DF003 reference station ID              12
 *     System-specific epoch (DF004/16/248/...) 30 / 27 bits
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
 *   Per satellite (Nsat × ...):
 *     DF397 rough range int (ms)               8
 *     DF419 ext sat info (GLONASS only)        4 / 0
 *     DF398 rough range mod 1 ms (units 2^-10 ms) 10
 *     DF399 rough phase range rate (m/s)      14  signed
 *
 *   Per cell (Ncell = popcount(cell_mask)):
 *     DF405 fine pseudorange    20 signed   units 2^-29 ms
 *     DF406 fine phase range    24 signed   units 2^-31 ms
 *     DF407 lock time           10          extended-range table
 *     DF420 half-cycle amb       1
 *     DF408 CNR                 10          units 1/16 dB-Hz
 *     DF404 fine phase range rate 15 signed units 0.0001 m/s
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
#include "rtcm/RtcmSignalMap.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include <bitset>
#include <cstdio>
#include <cmath>

namespace GnssProcessor::RtcmMsm {

namespace {

// ---------------------------------------------------------------------------
// Per-system descriptor
// ---------------------------------------------------------------------------
struct SystemDesc {
    System system;
    char   sat_prefix;
    int    epoch_bits;        ///< 30 for GPS/GAL/QZS/BDS/SBAS, 27 for GLONASS.
    bool   uses_glo_tod;
    bool   has_ext_sat_info;  ///< DF419 (4 bits) — GLONASS only.
    int    sbas_prn_offset;   ///< 119 for SBAS (PRN = bit + 119), 0 otherwise.
    double bdt_to_gps_offset_s; ///< 14 for BDS (BDT vs GPS), 0 otherwise.
};

constexpr SystemDesc kGps     {System::GPS,     'G', 30, false, false,   0,  0.0};
constexpr SystemDesc kGlonass {System::GLONASS, 'R', 27, true,  true,    0,  0.0};
constexpr SystemDesc kGalileo {System::Galileo, 'E', 30, false, false,   0,  0.0};
constexpr SystemDesc kSbas    {System::SBAS,    'S', 30, false, false, 119,  0.0};
constexpr SystemDesc kQzss    {System::QZSS,    'J', 30, false, false,   0,  0.0};
constexpr SystemDesc kBeidou  {System::BeiDou,  'C', 30, false, false,   0, 14.0};

// ---------------------------------------------------------------------------
// MSM variant (field widths and feature flags).  MSM4 and MSM7 share the
// same overall structure but differ in per-cell and per-sat field widths.
// MSM5/MSM6 sit between them; not yet emitted.
// ---------------------------------------------------------------------------
struct MsmVariant {
    int    msm_subtype;            ///< 4 or 7.
    int    bits_fine_pr;
    int    bits_fine_phase;
    int    bits_lock;
    int    bits_cnr;
    bool   has_range_rate;         ///< Per-sat rough + per-cell fine, only in MSM5/7.
    bool   honour_ext_sat_info;    ///< Read DF419 only when both variant + system require it.
    double scale_fine_pr_ms;       ///< 2^-24 (MSM4) or 2^-29 (MSM7).
    double scale_fine_phase_ms;    ///< 2^-29 (MSM4) or 2^-31 (MSM7).
    double cnr_unit_dbhz;          ///< 1.0 (MSM4) or 1.0/16.0 (MSM7).
    int    sentinel_fine_pr;       ///< Invalid-cell sentinel for fine pseudorange.
    int    sentinel_fine_phase;    ///< Invalid-cell sentinel for fine phase range.
};

// Note: scale factors are computed via `std::ldexp` of -24/-29/-31 to keep
// them constexpr-friendly across compilers.  Equivalent to std::pow(2,-N).
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
// Source: RTCM 10403.3 Table 3.5-101.  >= 64 s after level 6.
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
    return 0.0;  // n >= 512: invalid
}

// MSM4 4-bit lock time (DF402): n=0 → 0 ms; n=1..15 → 32·2^(n-1) ms.
// Range 32 ms .. 524288 ms (~ 8.7 minutes).
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
        // Frequency-dependent (FDMA); slot lookup needed via DF419 or
        // an external slot-frequency map.  Returning 0.0 means the
        // phase observable will be carried in metres of range — the
        // upstream code path can convert later when slot info is plumbed.
        return 0.0;
    default:
        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Sat ID formatting from satellite mask bit.
// ---------------------------------------------------------------------------
std::string formatSat(const SystemDesc& sys, int bit_index) {
    char buf[8];
    if (sys.system == System::SBAS) {
        std::snprintf(buf, sizeof(buf), "S%02d", bit_index + 1 + sys.sbas_prn_offset);
    } else {
        std::snprintf(buf, sizeof(buf), "%c%02d", sys.sat_prefix, bit_index + 1);
    }
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Time conversions
// ---------------------------------------------------------------------------
GnssTime promoteEpoch(const SystemDesc& sys, uint32_t raw, int current_week) {
    if (sys.uses_glo_tod) {
        // 27-bit GLO epoch = 3-bit DOW + 24-bit ms-of-day; conversion to
        // GPS time requires an external date reference.  Leave week at 0
        // and tow at the GLO seconds-of-day until a follow-up plumbs the
        // rover's current date through.
        const uint32_t ms_of_day = raw & 0xFFFFFFu;
        return GnssTime{0, ms_of_day * 1e-3};
    }
    // 30-bit ms-of-week.
    double tow = static_cast<double>(raw) * 1e-3;
    if (sys.bdt_to_gps_offset_s != 0.0) {
        tow += sys.bdt_to_gps_offset_s;             // BDT -> GPS
        if (tow >= TimeUtils::SECONDS_IN_WEEK) {
            tow -= TimeUtils::SECONDS_IN_WEEK;
            ++current_week;
        }
    }
    return GnssTime{current_week, tow};
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
    int               current_gps_week,
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

    res.epoch_time = promoteEpoch(*sys, epoch_raw, current_gps_week);

    // Satellite mask (64 bits) — bit i set => satellite (i+1) present.
    const uint64_t sat_mask_hi = r.u(32);
    const uint64_t sat_mask_lo = r.u(32);
    const uint64_t sat_mask    = (sat_mask_hi << 32) | sat_mask_lo;
    if (!r.ok()) return res;

    // Signal mask (32 bits) — bit i set => RTCM signal id (i+1) present.
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
    if (n_cell_bits <= 0) { res.ok = r.ok(); return res; }
    std::vector<bool> cell_mask(n_cell_bits, false);
    for (int i = 0; i < n_cell_bits; ++i) {
        cell_mask[i] = r.boolean();
        if (cell_mask[i]) ++res.n_cells;
    }
    if (!r.ok()) return res;

    // ------------------ Per-satellite block ------------------
    for (auto& s : sats) s.rough_range_int = static_cast<int>(r.u(8));
    if (V.honour_ext_sat_info && sys->has_ext_sat_info) {
        for (auto& s : sats) s.ext_sat_info = static_cast<int>(r.u(4));
    }
    for (auto& s : sats) s.rough_range_mod  = static_cast<int>(r.u(10));
    if (V.has_range_rate) {
        for (auto& s : sats) s.rough_range_rate = static_cast<int>(r.s(14));
    }
    if (!r.ok()) return res;

    // ------------------ Per-cell block ------------------
    // RTCM packs each field block contiguously across all cells, in cell
    // order (which is row-major sat × sig).  We decode them into parallel
    // vectors and recombine at the end.  Field widths come from the
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
    // (range_int=0xFF, rough_rate=−2¹³) are constant across MSM4-7.
    const int kInvalidFinePseudo = V.sentinel_fine_pr;
    const int kInvalidFinePhase  = V.sentinel_fine_phase;
    constexpr int kInvalidFineRate   = -(1 << 14);   // -16384
    constexpr int kInvalidRangeInt   = 255;          // 0xFF
    constexpr int kInvalidRoughRate  = -(1 << 13);   // -8192

    const double c = Constants::SPEED_OF_LIGHT;

    observations.reserve(observations.size() + sats.size());

    int cell_idx = 0;
    for (int si = 0; si < res.n_satellites; ++si) {
        const auto& sat = sats[si];
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

        // Rough phase range rate (m/s).  Only present in MSM5/MSM7;
        // marked invalid via sentinel inside that range.
        const double rough_rate_mps = (!V.has_range_rate
                                       || sat.rough_range_rate == kInvalidRoughRate)
                                        ? std::nan("")
                                        : static_cast<double>(sat.rough_range_rate);

        RinexObservation obs;
        obs.satellite = sat.sat_id;
        obs.system    = sys->system;
        obs.time      = res.epoch_time;

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
            }
            // Phase (cycles, RINEX-3 convention).
            if (fine_phaserange[idx] != kInvalidFinePhase) {
                const double phase_s = rough_s
                    + static_cast<double>(fine_phaserange[idx]) * V.scale_fine_phase_ms * 1e-3;
                const double phase_m = phase_s * c;
                const double wl      = wavelengthForSuffix(sys->system, sig.suffix);
                obs.measurements["L" + sig.suffix] = (wl > 0.0) ? phase_m / wl : phase_m;
            }
            // CNR (dB-Hz) — DF403 is whole dB-Hz in MSM4, DF408 is 1/16 dB-Hz in MSM7.
            if (cnr[idx] != 0) {
                obs.measurements["S" + sig.suffix] =
                    static_cast<double>(cnr[idx]) * V.cnr_unit_dbhz;
            }
            // Doppler (Hz) — only in MSM5/MSM7.  Combine rough + fine range
            // rate, convert to Hz via the carrier wavelength.
            if (V.has_range_rate
                && fine_rate[idx] != kInvalidFineRate
                && !std::isnan(rough_rate_mps)) {
                const double rate_mps = rough_rate_mps
                    + static_cast<double>(fine_rate[idx]) * 1e-4;
                const double wl = wavelengthForSuffix(sys->system, sig.suffix);
                if (wl > 0.0) {
                    obs.measurements["D" + sig.suffix] = -rate_mps / wl;
                }
            }
            // LLI: bit 1 = half-cycle ambiguity present; bit 2 = lock-time
            // less than the previous observation (proxy for cycle slip).
            int lli = 0;
            if (half_amb[idx]) lli |= 0x2;
            obs.lli_flags["L" + sig.suffix] = lli;
        }

        if (!obs.measurements.empty()) observations.push_back(std::move(obs));
    }

    res.ok = r.ok();
    return res;
}

// ---------------------------------------------------------------------------
// Public entry points.  Both forward to decodeMsmImpl with the matching
// variant.  The two variants share the same body code; the variant struct
// supplies field widths, scale factors, and capability flags.
// ---------------------------------------------------------------------------

MsmDecodeResult decodeMsm7(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<RinexObservation>& observations)
{
    return decodeMsmImpl(payload, len_bytes, msg_type, current_gps_week,
                         kMsm7, observations);
}

MsmDecodeResult decodeMsm4(
    const uint8_t* payload,
    std::size_t    len_bytes,
    uint16_t       msg_type,
    int            current_gps_week,
    std::vector<RinexObservation>& observations)
{
    return decodeMsmImpl(payload, len_bytes, msg_type, current_gps_week,
                         kMsm4, observations);
}

} // namespace GnssProcessor::RtcmMsm
