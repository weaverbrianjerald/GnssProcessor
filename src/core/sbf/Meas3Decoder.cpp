/**
 * @file Meas3Decoder.cpp
 * @brief Meas3Ranges (SBF 4109) differential code/phase decoder.
 *
 * Clean-room port of the rtklibexplorer/RTKLIB demo5 decode_meas3ranges()
 * (src/rcv/septentrio.c), re-expressed in the engine's types and validated
 * bit-exactly against the 4027 MeasEpoch blocks co-logged in a live mosaic-X5
 * capture.  See Meas3Decoder.hpp for the format overview.
 *
 * Layout, in brief (offsets into the SBF body, i.e. block + 8):
 *   +0  u4  TOW [ms]            +6 u1 CommonFlags      +7 u1 CumClkJumps
 *   +8  u2  Constellations      +10 u1 Misc            +11 u1 Reserved(version)
 *   +12 ..  per-constellation { header, satellites }
 * Misc: bits0-2 antenna, bit3 PR-rate present, bits4-7 reference-epoch interval
 * index into Meas3_EpochIntervals.  An epoch is a reference epoch when
 * TOW % interval == 0; otherwise it is a delta epoch against the cached one.
 * Each constellation header carries the satellite mask (and GLONASS FCNs / BDS
 * long-range flags / a signal-exclusion mask); a leading 0 byte means "reuse
 * the cached reference-epoch header".  Per satellite, a master record (short /
 * long / short-delta / long-delta) is followed by one slave record (short /
 * long / delta) per slave signal.  All reconstruction constants are transcribed
 * from the reference decoder.
 */

#include "sbf/Meas3Decoder.hpp"
#include "sbf/SbfSignalMap.hpp"
#include "common/Constants.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace GnssProcessor {

namespace {

// ---- little-endian typed reads ----
inline std::uint8_t  u1(const std::uint8_t* p) { return p[0]; }
inline std::uint16_t u2(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
inline std::uint32_t u4(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline std::int16_t  i2(const std::uint8_t* p) { std::int16_t  v; std::memcpy(&v, p, 2); return v; }

inline int popcount8(std::uint8_t b) {
    int c = 0; while (b) { b &= (b - 1); ++c; } return c;
}

constexpr double kDnuTow = 4294967295.0;
constexpr std::uint16_t kDnuWnc = 0xFFFF;

// Reference-epoch interval index -> milliseconds (Meas3_EpochIntervals).
constexpr std::uint32_t kEpochIntervals[16] =
    {1, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 1, 1, 1, 1, 1, 1};

// Coarse per-constellation pseudorange base [m] added to the master-short code.
constexpr double kPrBase[7] = {19e6, 19e6, 22e6, 20e6, 34e6, 34e6, 34e6};

// Lock-time indicator -> PLL lock time [ms].
constexpr std::uint32_t kLtiToPll[16] =
    {0, 60000, 30000, 15000, 10000, 5000, 2000, 1000, 500, 200, 100, 50, 40, 20, 10, 0};

// Meas3 (navsys, signal index) -> SBF global signal number, so suffix/frequency
// resolve through SbfSignalMap exactly as the 4027 path does (-1 = no signal).
// Rows: GPS, GLONASS, Galileo, BeiDou, SBAS, QZSS, IRNSS.
constexpr int kSig2Sbf[7][16] = {
    { 0,  3,  4,  1,  2,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // GPS  L1CA,L2C,L5,L1P,L2P,L1C
    { 8, 11,  9, 10, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // GLO  L1CA,L2CA,L1P,L2P,L3
    {17, 20, 21, 19, 22, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // GAL  E1,E5a,E5b,E6,E5AltBOC
    {28, 29, 30, 13, 14, 34, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // BDS  B1I,B2I,B3I,B1C,B2a,B2b
    {24, 25, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // SBAS L1CA,L5
    { 6,  7, 26, 27, 32, 33, 39, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // QZS  L1CA,L2C,L5,L6,L1C,L1S,L5S
    {15, 37, 36, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, // IRN  L5,L1,S
};

// navsys -> System + RINEX PRN prefix and base (PRN number = svid + base).
struct NavInfo { System sys; char letter; int prn_base; };
constexpr NavInfo kNav[7] = {
    {System::GPS,     'G',  1},
    {System::GLONASS, 'R',  1},
    {System::Galileo, 'E',  1},
    {System::BeiDou,  'C',  1},
    {System::SBAS,    'S', 20},   // SBAS PRN 120+svid, RINEX number = PRN-100 = 20+svid
    {System::QZSS,    'J',  1},
    {System::Unknown, 'I',  1},   // IRNSS: signals map to empty suffix -> not emitted
};

int systemGpsWeek() {
    using namespace std::chrono;
    const auto s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    constexpr std::int64_t kGpsEpochUnix = 315964800;
    const std::int64_t g = s - kGpsEpochUnix;
    return (g <= 0) ? 0 : static_cast<int>(g / (7 * 24 * 3600));
}

} // namespace

Meas3Decoder& Meas3Decoder::instance() {
    static Meas3Decoder inst;
    return inst;
}

void Meas3Decoder::reset() {
    ref_tow_ms_ = 0;
    std::memset(ref_, 0, sizeof(ref_));
    std::memset(con_hdr_, 0, sizeof(con_hdr_));
    std::memset(con_hdr_len_, 0, sizeof(con_hdr_len_));
    std::memset(lock_run_, 0, sizeof(lock_run_));
    std::memset(lock_ref_, 0, sizeof(lock_ref_));
}

std::vector<RinexObservation> Meas3Decoder::decode(const std::uint8_t* block, std::size_t len) {
    std::vector<RinexObservation> out;
    if (len < 8 + 12) return out;

    const std::uint8_t* b = block + 8;                 // SBF body (TOW @ +0)
    const std::uint32_t tow_ms = u4(b + 0);
    const std::uint16_t wnc    = u2(b + 4);
    const std::uint16_t constellations = static_cast<std::uint8_t>(u2(b + 8));
    const std::uint8_t  misc     = u1(b + 10);
    const std::uint8_t  reserved = u1(b + 11);
    if (reserved > 31) return out;                     // unknown data version

    const bool prr_available = (misc & 0x08) != 0;
    const int  antenna_idx   = misc & 0x07;
    if (antenna_idx != 0) return out;                  // main antenna only
    const std::uint32_t interval = kEpochIntervals[misc >> 4];
    if (interval == 0) return out;

    const bool is_ref = (tow_ms % interval) == 0;
    if (is_ref) {
        std::memset(ref_, 0, sizeof(ref_));
        std::memset(con_hdr_, 0, sizeof(con_hdr_));
        std::memset(con_hdr_len_, 0, sizeof(con_hdr_len_));
        ref_tow_ms_ = tow_ms;
    } else if (ref_tow_ms_ != (tow_ms / interval) * interval) {
        return out;                                    // delta epoch, reference not seen
    }

    // Epoch time stamp (same convention as SbfParser::blockTime).
    const double tow = (tow_ms == kDnuTow) ? 0.0 : tow_ms * 1e-3;
    const int    week = (wnc == kDnuWnc) ? systemGpsWeek() : static_cast<int>(wnc);
    const GnssTime epoch{week, tow};

    const std::uint8_t* data = b + 12;                 // start of per-constellation data
    const std::size_t   data_len = len - (8 + 12);
    std::size_t idx = 0;                               // running offset into data[]

    for (int ns = 0; ns < kSys; ++ns) {
        if (!(constellations & (1u << ns))) continue;

        // --- per-constellation header (a leading 0 byte => reuse cached header) ---
        const bool reuse = (idx < data_len) && (data[idx] == 0);
        const std::uint8_t* h = reuse ? con_hdr_[ns] : data + idx;
        std::size_t hp = 0;

        const std::uint8_t bf1 = h[hp++];
        std::uint8_t nB = bf1 & 0x07;
        const std::uint8_t sig_master_short = (bf1 >> 3) & 0x0F;
        const bool sig_excluded_present = (bf1 >> 7) != 0;
        if (nB == 7) nB = 8;

        std::uint64_t sat_mask = 0;
        int n_sats = 0;
        for (int i = 0; i < nB; ++i) {
            const std::uint8_t mbyte = h[hp + i];
            sat_mask |= static_cast<std::uint64_t>(mbyte) << (i * 8);
            n_sats += popcount8(mbyte);
        }
        hp += nB;

        const std::uint8_t* glo_fncs = nullptr;
        std::uint16_t bds_long_range = 0;
        if (ns == 1) {                                 // GLONASS FCN list, 4 bits each
            glo_fncs = h + hp;
            hp += static_cast<std::size_t>((n_sats + 1) / 2);
        } else if (ns == 3) {                          // BeiDou long-range flags
            bds_long_range = u2(h + hp);
            hp += 2;
        }
        std::uint8_t sig_excluded = 0;
        if (sig_excluded_present) sig_excluded = h[hp++];

        // Cache the header at a reference epoch; advance the stream cursor.
        if (is_ref && hp <= sizeof(con_hdr_[0])) {
            std::memcpy(con_hdr_[ns], data + idx, hp);
            con_hdr_len_[ns] = static_cast<std::uint8_t>(hp);
        }
        idx += reuse ? 1 : hp;

        // Compacted signal table: drop excluded signal indices, re-index densely.
        int sig_slots[kSig];
        int nslot = 0;
        for (int i = 0; i < kSig; ++i)
            if (!((sig_excluded >> i) & 1)) sig_slots[nslot++] = i;
        for (int i = nslot; i < kSig; ++i) sig_slots[i] = -1;

        // compacted signal index + GLONASS FCN -> (RINEX suffix, frequency Hz).
        auto sigInfo = [&](int csig, int glofnc) -> std::pair<std::string, double> {
            if (csig < 0 || csig >= kSig) return {std::string(), 0.0};
            const int orig = sig_slots[csig];
            if (orig < 0) return {std::string(), 0.0};
            const int sbf = kSig2Sbf[ns][orig];
            if (sbf < 0) return {std::string(), 0.0};
            const auto info = SbfSignalMap::signalInfo(sbf);
            const double f = info.glonass_fdma_band
                ? SbfSignalMap::glonassFdmaFreqHz(info.glonass_fdma_band, glofnc) : info.freq_hz;
            return {info.suffix, f};
        };

        const double c = Constants::SPEED_OF_LIGHT;
        int sat_cnt = 0;
        for (int svid = 0; svid < kSat && sat_cnt < n_sats; ++svid) {
            if (!((sat_mask >> svid) & 1)) continue;

            int glofnc = 0;
            if (ns == 1 && glo_fncs)
                glofnc = static_cast<int>((glo_fncs[sat_cnt / 2] >> (4 * (sat_cnt % 2))) & 0x0F) - 8;

            char prnbuf[8];
            std::snprintf(prnbuf, sizeof(prnbuf), "%c%02d", kNav[ns].letter, svid + kNav[ns].prn_base);
            RinexObservation obs;
            obs.satellite = prnbuf;
            obs.system    = kNav[ns].sys;
            obs.time      = epoch;

            SatRef& ref = ref_[ns][svid];

            // store one observable into obs (and refresh the reference cache).
            auto put = [&](int csig, const std::string& suf, double pr, double ph, bool ph_valid,
                           double cn0, std::uint8_t lli) {
                if (suf.empty()) return;
                obs.measurements["C" + suf] = pr;
                if (ph_valid) {
                    obs.measurements["L" + suf] = ph;
                    obs.lli_flags["L" + suf]   = lli;
                }
                obs.measurements["S" + suf] = cn0;
                if (is_ref) {
                    ref.P[csig]   = pr;
                    ref.L[csig]   = ph_valid ? ph : 0.0;
                    ref.S[csig]   = cn0;
                    ref.lli[csig] = lli;
                }
            };

            const double pr_base = (bds_long_range & (1u << sat_cnt)) ? 34e6 : kPrBase[ns];
            const std::uint8_t bt = data[idx];

            int           master_sig = 0;
            std::uint32_t slave_mask = 0;
            std::int16_t  pr_rate    = 0;
            double        P_master = 0.0, L_master = 0.0, freq_master = 0.0;
            std::string   suf_master;

            if ((bt & 1) == 1) {
                // ---- Master short ----
                const std::uint32_t BF1 = u4(data + idx);
                const std::uint32_t pr_lsb = u4(data + idx + 4);
                const std::uint32_t cmc   = (BF1 >> 1)  & 0x3ffff;
                const std::uint32_t pr_msb = (BF1 >> 19) & 1;
                const std::uint32_t lti3  = (BF1 >> 20) & 0x7;
                const std::uint32_t cn0   = (BF1 >> 23) & 0x1f;
                const std::uint32_t siglist = (BF1 >> 28) & 0xf;
                const std::uint32_t lock = kLtiToPll[lti3];
                master_sig = sig_master_short;
                slave_mask = siglist << (master_sig + 1);
                auto [suf, fr] = sigInfo(master_sig, glofnc);
                if (!suf.empty() && fr > 0.0) {
                    const double pr = pr_base + (static_cast<double>(pr_lsb)
                                      + 4294967296.0 * pr_msb) * 0.001;
                    const bool ph_v = cmc != 0;
                    const double ph = pr / (c / fr) - 131.072 + cmc * 0.001;
                    const std::uint8_t lli =
                        ((lock < lock_run_[ns][svid][master_sig]) ? 1 : 0) | ((lti3 == 0) ? 2 : 0);
                    lock_run_[ns][svid][master_sig] = lock;
                    P_master = pr; L_master = ph_v ? ph : 0.0; freq_master = fr; suf_master = suf;
                    put(master_sig, suf, pr, ph, ph_v, cn0 + 24.0, lli);
                }
                pr_rate = prr_available ? i2(data + idx + 8) : 0;
                idx += prr_available ? 10 : 8;
            } else if ((bt & 3) == 0) {
                // ---- Master long ----
                const std::uint32_t BF1 = u4(data + idx);
                const std::uint32_t pr_lsb = u4(data + idx + 4);
                const std::uint16_t BF2 = u2(data + idx + 8);
                const std::uint8_t  BF3 = u1(data + idx + 10);
                const std::uint32_t pr_msb = (BF1 >> 2)  & 0xf;
                const std::uint32_t cmc   = (BF1 >> 6)  & 0x3fffff;
                const std::uint32_t lti4  = (BF1 >> 28) & 0xf;
                const std::uint32_t cn0   = BF2 & 0x3f;
                std::uint32_t signal_mask = (BF2 >> 6) & 0x1ff;
                const std::uint32_t cont  = (BF2 >> 15) & 0x1;
                const std::uint32_t lock = kLtiToPll[lti4];
                if (cont) signal_mask |= static_cast<std::uint32_t>(BF3 & 0x7f) << 9;
                master_sig = 0;
                for (int k = 0; k < 32; ++k) if ((signal_mask >> k) & 1) { master_sig = k; break; }
                slave_mask = signal_mask ^ (1u << master_sig);
                auto [suf, fr] = sigInfo(master_sig, glofnc);
                if (!suf.empty() && fr > 0.0) {
                    const double P = (static_cast<double>(pr_lsb) + 4294967296.0 * pr_msb) * 0.001;
                    const bool ph_v = cmc != 0;
                    const double ph = P / (c / fr) - 2097.152 + cmc * 0.001;
                    const bool gps_p = (ns == 0) && (suf == "1W" || suf == "2W");
                    const std::uint8_t lli =
                        ((lock < lock_run_[ns][svid][master_sig]) ? 1 : 0) | ((lti4 == 0) ? 2 : 0);
                    lock_run_[ns][svid][master_sig] = lock;
                    P_master = P; L_master = ph_v ? ph : 0.0; freq_master = fr; suf_master = suf;
                    put(master_sig, suf, P, ph, ph_v, gps_p ? cn0 : cn0 + 10.0, lli);
                }
                pr_rate = prr_available ? i2(data + idx + 10) : 0;
                idx += (prr_available ? 12 : 10) + cont;
            } else if ((bt & 0xc) == 0xc) {
                // ---- Master long delta ----
                master_sig = ref.master_sig;
                slave_mask = ref.slave_mask;
                auto [suf, fr] = sigInfo(master_sig, glofnc);
                const std::uint8_t  BF1 = u1(data + idx);
                const std::uint32_t BF2 = u4(data + idx + 1);
                const std::uint32_t pr  = (static_cast<std::uint32_t>(BF1 >> 4) << 13) | (BF2 & 0x1fff);
                const std::uint32_t cn0 = (BF2 >> 13) & 0x7;
                const std::uint32_t cmc = BF2 >> 16;
                if (!suf.empty() && fr > 0.0) {
                    const double refP = ref.P[master_sig];
                    const double P = refP + (static_cast<std::int64_t>(ref.pr_rate) * 64
                                     * static_cast<std::int32_t>(tow_ms % interval) / 1000) * 0.001
                                     + pr * 0.001 - 65.536;
                    const bool ph_v = cmc != 0;
                    const double ph = (P - refP) / (c / fr) + ref.L[master_sig] - 32.768 + cmc * 0.001;
                    lock_run_[ns][svid][master_sig] = lock_ref_[ns][svid][master_sig];
                    P_master = P; L_master = ph_v ? ph : 0.0; freq_master = fr; suf_master = suf;
                    put(master_sig, suf, P, ph, ph_v, ref.S[master_sig] - 4.0 + cn0, ref.lli[master_sig]);
                }
                idx += 5;
            } else {
                // ---- Master short delta ----
                const std::uint32_t BF1 = u4(data + idx);
                const std::uint32_t pr  = (BF1 >> 4)  & 0x3fff;
                const std::uint32_t cmc = (BF1 >> 18) & 0x3fff;
                const std::uint32_t cn0 = (BF1 >> 2)  & 0x3;
                master_sig = ref.master_sig;
                slave_mask = ref.slave_mask;
                auto [suf, fr] = sigInfo(master_sig, glofnc);
                if (!suf.empty() && fr > 0.0) {
                    const double refP = ref.P[master_sig];
                    const double P = refP + (static_cast<std::int64_t>(ref.pr_rate) * 64
                                     * static_cast<std::int32_t>(tow_ms % interval) / 1000) * 0.001
                                     + pr * 0.001 - 8.192;
                    const bool ph_v = cmc != 0;
                    const double ph = (P - refP) / (c / fr) + ref.L[master_sig] - 8.192 + cmc * 0.001;
                    lock_run_[ns][svid][master_sig] = lock_ref_[ns][svid][master_sig];
                    P_master = P; L_master = ph_v ? ph : 0.0; freq_master = fr; suf_master = suf;
                    put(master_sig, suf, P, ph, ph_v, ref.S[master_sig] - 1.0 + cn0, ref.lli[master_sig]);
                }
                idx += 4;
            }

            // Persist master metadata so future delta epochs resolve this satellite.
            if (is_ref) {
                ref.master_sig = static_cast<std::uint8_t>(master_sig);
                ref.slave_mask = slave_mask;
                ref.pr_rate    = pr_rate;
                if (!suf_master.empty())
                    lock_ref_[ns][svid][master_sig] = lock_run_[ns][svid][master_sig];
            }
            if (lock_run_[ns][svid][master_sig] > lock_ref_[ns][svid][master_sig])
                lock_ref_[ns][svid][master_sig] = lock_run_[ns][svid][master_sig];

            // ---- slave signals ----
            std::uint32_t ssm = slave_mask;
            for (int slave_sig = 1; slave_sig < kSig && ssm != 0; ++slave_sig) {
                if (!((ssm >> slave_sig) & 1)) continue;
                const std::uint8_t bts = data[idx];
                auto [sufS, frS] = sigInfo(slave_sig, glofnc);

                if ((bts & 1) == 1) {
                    // ---- Slave short ----
                    const std::uint32_t BF1 = u4(data + idx);
                    const std::uint8_t  BF2 = u1(data + idx + 4);
                    const std::uint32_t cmc_res = (BF1 >> 1) & 0xffff;
                    const std::uint32_t pr_rel  = BF1 >> 17;
                    const std::uint32_t lti3 = BF2 & 0x7;
                    const std::uint32_t cn0  = BF2 >> 3;
                    const std::uint32_t lock = kLtiToPll[lti3];
                    if (!sufS.empty() && frS > 0.0 && freq_master > 0.0) {
                        const double P = (freq_master > frS) ? P_master + pr_rel * 0.001 - 10.0
                                                             : P_master - pr_rel * 0.001 + 10.0;
                        const bool ph_v = cmc_res != 0;
                        const double ph = P / (c / frS)
                            + (L_master - P_master / (c / freq_master)) * (freq_master / frS)
                            - 32.768 + cmc_res * 0.001;
                        const bool gps_p = (ns == 0) && (sufS == "1W" || sufS == "2W");
                        const double cn0db = gps_p ? obs.measurements["S" + suf_master] - 3.0 - cn0
                                                   : cn0 + 24.0;
                        const std::uint8_t lli =
                            ((lock < lock_run_[ns][svid][slave_sig]) ? 1 : 0) | ((lti3 == 0) ? 2 : 0);
                        lock_run_[ns][svid][slave_sig] = lock;
                        put(slave_sig, sufS, P, ph, ph_v, cn0db, lli);
                    }
                    idx += 5;
                } else if ((bts & 3) == 0) {
                    // ---- Slave long ----
                    const std::uint32_t BF1 = u4(data + idx);
                    const std::uint16_t pr_lsb_rel = u2(data + idx + 4);
                    const std::uint8_t  BF3 = u1(data + idx + 6);
                    const std::uint32_t cmc  = (BF1 >> 2)  & 0x3fffff;
                    const std::uint32_t lti4 = (BF1 >> 24) & 0xf;
                    const std::uint32_t pr_msb_rel = (BF1 >> 28) & 0x7;
                    const std::uint32_t cn0  = BF3 & 0x3f;
                    const std::uint32_t lock = kLtiToPll[lti4];
                    if (!sufS.empty() && frS > 0.0) {
                        const double P = P_master + (pr_msb_rel * 65536.0 + pr_lsb_rel) * 0.001 - 262.144;
                        const bool ph_v = cmc != 0;
                        const double ph = P / (c / frS) - 2097.152 + cmc * 0.001;
                        const bool gps_p = (ns == 0) && (sufS == "1W" || sufS == "2W");
                        const std::uint8_t lli =
                            ((lock < lock_run_[ns][svid][slave_sig]) ? 1 : 0) | ((lti4 == 0) ? 2 : 0);
                        lock_run_[ns][svid][slave_sig] = lock;
                        put(slave_sig, sufS, P, ph, ph_v, gps_p ? cn0 : cn0 + 10.0, lli);
                    }
                    idx += 7;
                } else {
                    // ---- Slave delta ----
                    const std::uint16_t BF1 = u2(data + idx);
                    const std::uint8_t  dC  = u1(data + idx + 2);
                    const std::uint32_t d_pr = (BF1 >> 2) & 0xfff;
                    const std::uint32_t cn0  = BF1 >> 14;
                    if (!sufS.empty() && frS > 0.0 && freq_master > 0.0) {
                        const double slaveRefL = ref.L[slave_sig];
                        const double slaveRefP = ref.P[slave_sig];
                        const double masterRefL = ref.L[master_sig];
                        const double L = slaveRefL
                            + (L_master - masterRefL) * frS / freq_master - 0.128 + dC * 0.001;
                        const double P = slaveRefP + (L - slaveRefL) * (c / frS) - 2.048 + d_pr * 0.001;
                        lock_run_[ns][svid][slave_sig] = lock_ref_[ns][svid][slave_sig];
                        put(slave_sig, sufS, P, L, /*ph_valid=*/true, ref.S[slave_sig] - 2.0 + cn0,
                            ref.lli[slave_sig]);
                    }
                    idx += 3;
                }

                if (is_ref && !sufS.empty())
                    lock_ref_[ns][svid][slave_sig] = lock_run_[ns][svid][slave_sig];
                if (lock_run_[ns][svid][slave_sig] > lock_ref_[ns][svid][slave_sig])
                    lock_ref_[ns][svid][slave_sig] = lock_run_[ns][svid][slave_sig];

                ssm ^= (1u << slave_sig);
            }

            if (!obs.measurements.empty()) out.push_back(std::move(obs));
            ++sat_cnt;
        }
    }

    return out;
}

} // namespace GnssProcessor
