/**
 * @file NetworkStateSnapshot.cpp
 * @brief Implementation of the RTB-7 network-producer state snapshot
 *        (serialization, strict parse, atomic file replace, warm/cold gate).
 */

#include "products/NetworkStateSnapshot.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <system_error>

namespace GnssProcessor {

namespace {

constexpr const char* kHeader = "# gnss_network_state_snapshot v1";
// Format v2 (SPEC_synthetic_minimum_network §18.10): v1 plus the resume frame records
// (PA/PT phase-anchor incumbent and sticky pin target, CA clock-anchor per-sat state,
// FA/FP/FE CLK-R held frame).  Written only when at least one frame record is
// present, so a run that carries none writes v1 byte-for-byte.  The reader accepts
// both; a v1 file restores empty frame maps, which gives the cold-pin behaviour.
constexpr const char* kHeaderV2 = "# gnss_network_state_snapshot v2";
// Format v3 (SPEC_synthetic_minimum_network §18.12): v2 plus the coupling
// regressors (QS shared-column dictionary, QL/QZ per-row (id, M_rj) lists, QR
// write-epoch clock-refloat set).  Written only when at least one coupling record
// is present, so a frame-only run still writes v2 and a bare run v1 byte-for-byte;
// the reader accepts all three.
constexpr const char* kHeaderV3 = "# gnss_network_state_snapshot v3";

/// Split a line on ','.  Keys/values in this format never contain commas
/// (sat ids, RINEX signals, "sat|signal" keys, ISO timestamps), so a plain
/// split is exact.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(',', start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

/// Strict full-consumption double parse (round-trips fmt's shortest form).
bool parseD(const std::string& s, double& v) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

/// Strict full-consumption signed-integer parse.
bool parseI(const std::string& s, long long& v) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    v = std::strtoll(s.c_str(), &end, 10);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

/// Strict full-consumption unsigned-integer parse.
bool parseU(const std::string& s, unsigned long long& v) {
    if (s.empty() || s[0] == '-') return false;
    errno = 0;
    char* end = nullptr;
    v = std::strtoull(s.c_str(), &end, 10);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

/// Strict 0/1 flag parse.
bool parseB(const std::string& s, bool& v) {
    if (s == "0") { v = false; return true; }
    if (s == "1") { v = true;  return true; }
    return false;
}

/// Strict hex-uint64 parse (the config fingerprint).
bool parseHex64(const std::string& s, uint64_t& v) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    v = std::strtoull(s.c_str(), &end, 16);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

} // namespace

uint64_t NetworkStateSnapshotIO::fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;   // FNV offset basis
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;   // FNV prime
    return h;
}

std::string NetworkStateSnapshotIO::utcNowIso() {
    char utc[32] = "unknown";
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return utc;
}

void NetworkStateSnapshotIO::serialize(std::ostream& os, const NetworkStateSnapshot& snap) {
    // Count every line after the '#' header; the END terminator carries the
    // count as the truncation guard.
    uint64_t n_lines = 0;
    auto put = [&](const std::string& line) {
        os << line << '\n';
        ++n_lines;
    };

    os << (snap.hasCouplingState() ? kHeaderV3
         : snap.hasFrameState()    ? kHeaderV2 : kHeader) << '\n';
    put(fmt::format("M,{},{},{},{},{:016x}", snap.snap_week, snap.snap_tow,
                    snap.wall_utc.empty() ? std::string("unknown") : snap.wall_utc,
                    snap.generation_id, snap.config_fingerprint));
    put(fmt::format("N,{},{},{},{},{},{},{},{},{},{},{},{}", snap.n_emitted, snap.n_skipped,
                    snap.n_gate_withheld, snap.n_sigma0_withheld, snap.n_runaway_epochs,
                    snap.n_perconst_partial, snap.n_perconst_diffuse, snap.iod,
                    snap.prev_week, snap.prev_tow, snap.last_emit_content_s,
                    snap.last_health_write_s));
    put(fmt::format("U,{},{},{},{}", snap.ura_sigma0_ewma, snap.ura_ewma_updates,
                    snap.ramp_lifted ? 1 : 0, snap.first_emit_content_s));
    for (const auto& d : snap.shared_carry)
        put(fmt::format("D,{},{},{},{},{},{},{},{},{}", d.kind, d.sat, d.signal, d.sh_n,
                        d.sh_m, d.sh_trig, d.value, d.var, d.inflate_q ? 1 : 0));
    for (const auto& [c, p] : snap.prev_pivot) put(fmt::format("V,{},{}", c, p));
    for (const auto& [c, p] : snap.last_pivot) put(fmt::format("W,{},{}", c, p));
    for (const auto& [sat, o] : snap.clock_osc)
        put(fmt::format("K,{},{},{},{},{},{},{},{}", sat, o.x0, o.x1, o.p00, o.p01, o.p11,
                        o.t_last, o.init ? 1 : 0));
    for (const auto& [sat, vp] : snap.prev_clk)
        put(fmt::format("J,{},{},{}", sat, vp.first, vp.second));
    for (const auto& [key, l] : snap.local_carry)
        put(fmt::format("L,{},{},{},{},{},{}{}", std::get<0>(key), std::get<1>(key),
                        std::get<2>(key), l.value_abs, l.var, l.last_seen_s,
                        // network.phase_datum_handover_max_var_ratio: the arc's first-stash
                        // variance is an optional 7th field, written only when nonzero;
                        // without it the L row is byte-identical to every earlier format.
                        l.var_first != 0.0 ? fmt::format(",{}", l.var_first)
                                           : std::string{}));
    for (const auto& [idx, l] : snap.zwd_carry)
        put(fmt::format("Z,{},{},{},{}", idx, l.value_abs, l.var, l.last_seen_s));
    // Resume frame records (format v2, §18.10); absent on a run without them.
    for (const auto& [amb, sta] : snap.anchor_incumbent)
        put(fmt::format("PA,{},{}", amb, sta));
    for (const auto& [amb, tgt] : snap.anchor_pin_target)
        put(fmt::format("PT,{},{}", amb, tgt));
    for (const auto& [sat, ca] : snap.clock_anchor)
        put(fmt::format("CA,{},{},{},{},{},{}", sat, ca.t_last, ca.last_value,
                        ca.last_var_eff, ca.in_band, ca.anchored ? 1 : 0));
    for (const auto& [c, acc] : snap.clk_frame_acc)
        put(fmt::format("FA,{},{}", std::string(1, c), acc));
    for (const auto& [sat, hp] : snap.clk_frame_prev)
        put(fmt::format("FP,{},{},{}", sat, hp.first, hp.second));
    if (snap.clk_frame_epochs > 0) put(fmt::format("FE,{}", snap.clk_frame_epochs));
    // Coupling regressors (format v3, §18.12): the shared-column dictionary
    // first, then one packed line per carried row holding only (dictionary id, M_rj)
    // in harvest order, then the write epoch's clock-refloat set.  Emitted before the
    // CARRY/EMITTER blobs so the verbatim blob parsers never consume them.  The
    // driver leaves these containers empty unless
    // network.state_snapshot_coupling_capture is armed, so a run without them writes
    // the v1/v2 bytes unchanged.
    for (std::size_t qi = 0; qi < snap.coupling_dict.size(); ++qi) {
        const NetworkStateSnapshot::CouplingCol& q = snap.coupling_dict[qi];
        put(fmt::format("QS,{},{},{},{},{},{},{},{},{},{}", qi, q.kind, q.sat, q.signal,
                        q.sh_n, q.sh_m, q.sh_trig, q.sh_layer, q.s_prev, q.p_prev));
    }
    for (const auto& [key, ents] : snap.coupling_rows) {
        std::string ln = fmt::format("QL,{},{},{},{}", std::get<0>(key), std::get<1>(key),
                                     std::get<2>(key), ents.size());
        for (const auto& [id, mj] : ents) ln += fmt::format(",{},{}", id, mj);
        put(ln);
    }
    for (const auto& [idx, ents] : snap.coupling_zrows) {
        std::string ln = fmt::format("QZ,{},{}", idx, ents.size());
        for (const auto& [id, mj] : ents) ln += fmt::format(",{},{}", id, mj);
        put(ln);
    }
    for (const std::string& s : snap.clock_refloat_sats) put(fmt::format("QR,{}", s));
    // Dense D2 covariance side-car (§18.12 (2a)): keys in row order, then the lower
    // triangle per row with an explicit length (the truncation guard the QL rows use).
    for (std::size_t i = 0; i < snap.carry_cov_keys.size(); ++i)
        put(fmt::format("QK,{},{}", i, snap.carry_cov_keys[i]));
    for (std::size_t i = 0; i < snap.carry_cov_rows.size(); ++i) {
        std::string ln = fmt::format("QP,{},{}", i, snap.carry_cov_rows[i].size());
        for (double v : snap.carry_cov_rows[i]) ln += fmt::format(",{}", v);
        put(ln);
    }
    // Datum records (SPEC_event_harness §21.7; RS-3): the eq34 chain datum's last-epoch
    // spanning tree, then the time-linked code-bias datum memory.  Written only under
    // network.state_snapshot_datum_capture (the driver leaves the containers empty
    // otherwise, so a run without it writes the previous bytes unchanged), and without
    // a header version bump (see the format note in the header).  Emitted before the
    // CARRY / EMITTER blobs, like the Q* group, so the verbatim blob parsers cannot
    // consume them.
    for (const auto& [key, sta] : snap.eq34_datum_rx) put(fmt::format("EQD,{},{}", key, sta));
    for (const auto& [key, sat] : snap.eq34_rx_parent) put(fmt::format("EQP,{},{}", key, sat));
    for (const auto& [amb, sta] : snap.eq34_sat_root)  put(fmt::format("EQS,{},{}", amb, sta));
    for (const auto& [key, tgt] : snap.eq34_prev_target)
        put(fmt::format("EQT,{},{},{}", key.first, key.second, tgt));
    for (const auto& [key, el] : snap.eq34_prev_el)
        put(fmt::format("EQE,{},{},{}", key.first, key.second, el));
    for (const auto& [idx, amb] : snap.eq34_retained_edges)
        put(fmt::format("EQR,{},{}", idx, amb));
    for (const auto& [fam, per_sat] : snap.code_bias_prev)
        for (const auto& [sat, v] : per_sat)
            put(fmt::format("CBP,{},{},{}", fam, sat, v));
    for (const auto& s : snap.code_bias_sat_seen) put(fmt::format("CBS,{}", s));
    for (const auto& [sat, pr] : snap.code_bias_ifpin_pair)
        put(fmt::format("CBI,{},{},{}", sat, pr.first, pr.second));
    // Emit-layer records (SPEC_event_harness §21.10; RS-5): the eq34 emit re-base's
    // emission frame.  Written only under
    // network.state_snapshot_emit_capture (the driver leaves the containers empty
    // otherwise, so a run without it writes the previous bytes unchanged), under the
    // existing header like the RS-3 group, and before the CARRY / EMITTER blobs so the
    // verbatim blob parsers cannot consume them.  The keys carry a '|' and the format
    // splits on ',' alone, so they need no escaping.
    for (const auto& [key, v] : snap.emit_rebase_last)
        put(fmt::format("ER,{},{}", key, v));
    // Station-layer records (SPEC_event_harness §21.13; RS-6): the rx-CB carry, then
    // the T5 per-LOS deviation carry.  Written only under
    // network.state_snapshot_station_capture (the driver leaves both containers empty
    // otherwise, so a run without it writes the previous bytes unchanged), under the
    // existing header like the RS-3 / RS-5 groups, and before the CARRY / EMITTER
    // blobs so the verbatim blob parsers cannot consume them.  The RC key's CB family
    // ("GPS_DCB", "GPS_IFB5", …) and the SD key's '|' carry no comma, so neither needs
    // escaping.  An RC row has the Z row's field set, so the restore runs through the
    // same makeRestoredLocalCarry as the L / Z rows.
    for (const auto& [key, l] : snap.rx_cb_carry)
        put(fmt::format("RC,{},{},{},{},{}", key.first, key.second,
                        l.value_abs, l.var, l.last_seen_s));
    for (const auto& [key, d] : snap.sh_dev_carry)
        put(fmt::format("SD,{},{},{},{},{}", key, d.dev, d.var, d.var_ss, d.t_s));
    // The level monitor's contrast-frame code information per satellite
    // (LB rows); empty unless the writer ran the monitor, so a run without it writes the
    // previous bytes unchanged.
    for (const auto& [sat, in] : snap.level_info)
        put(fmt::format("LB,{},{},{}", sat, in.first, in.second));
    // Canonical-signal records (SPEC_event_harness §21.16; RS-8): the per-station
    // per-(constellation, band, obs-type) latch that PppModel locks from one epoch's
    // satellite counts.  Written only under network.state_snapshot_signal_capture
    // (the driver leaves the container empty otherwise, so a run without it writes
    // the previous bytes unchanged), under the existing header like the RS-3 / RS-5
    // / RS-6 groups, and before the CARRY / EMITTER blobs so the verbatim blob
    // parsers cannot consume them.  Constellation / band / signal are RINEX tokens
    // and the obs-type is one char, so none of the fields can carry a comma.
    for (const auto& [idx, rows] : snap.canonical_signals)
        for (const auto& c : rows)
            put(fmt::format("CN,{},{},{},{},{}", idx, c.constellation, c.band,
                            c.obs_type, c.signal));
    // Code-bias linearization point beside each RC row (SPEC_event_harness §21.18;
    // RS-9), under its own knob (network.state_snapshot_rxcb_lin_capture) so a recipe
    // can carry the carry's numerator without its reference, or both.  Same key shape
    // as RC, (station roster index, CB state key), and the same escaping argument (the
    // CB family carries no comma).  Written under the existing header like every group
    // above; a reader that finds no RL rows behaves as without the knob.
    for (const auto& [key, xl] : snap.rx_cb_lin)
        put(fmt::format("RL,{},{},{}", key.first, key.second, xl));
    for (const auto& s : snap.ura_retro_sats) put(fmt::format("R,{}", s));
    for (const auto& s : snap.retro_active)   put(fmt::format("A,{}", s));
    for (const auto& [sat, n] : snap.ura_refloat_horizon)
        put(fmt::format("Y,{},{}", sat, n));
    for (const auto& s : snap.c1_zoh_active)  put(fmt::format("C,{}", s));
    for (const auto& [sat, iode] : snap.relevel_iode)   // eph-cutover re-level IODE
        put(fmt::format("I,{},{}", sat, iode));
    for (const auto& s : snap.eph_cutover_disc_pending) // pending eph-cutover disc
        put(fmt::format("X,{}", s));
    // Invariant carry for a branch-faithful resume (RTB-7): the ArcKey
    // vertex numbering (SV, GG), then the z-store blob verbatim (CARRY header .. CEND).
    // All three are written only when the carry blob is present
    // (network.state_snapshot_carry), so a default snapshot is byte-identical to the
    // format without them.
    if (!snap.carry_state.empty()) {
        for (const auto& [sat, vid] : snap.carry_sat_vertex)
            put(fmt::format("SV,{},{}", sat, vid));
        for (const auto& [cn, sig, gid] : snap.carry_group_id)
            put(fmt::format("GG,{},{},{}", cn, sig, gid));
        std::istringstream cs(snap.carry_state);
        std::string cl;
        while (std::getline(cs, cl)) put(cl);
    }
    // Emitter blob: verbatim, already newline-terminated lines from
    // NetworkProductEmitter::serializeState (EMITTER header .. EEND).
    {
        std::istringstream es(snap.emitter_state);
        std::string line;
        while (std::getline(es, line)) put(line);
    }
    os << "END," << n_lines << '\n';
}

bool NetworkStateSnapshotIO::parse(std::istream& is, NetworkStateSnapshot& out,
                                   std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };

    std::string line;
    if (!std::getline(is, line)) return fail("empty file");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kHeader && line != kHeaderV2 && line != kHeaderV3)
        return fail("unrecognized header: '" + line + "'");

    NetworkStateSnapshot s;
    uint64_t n_lines = 0;
    bool saw_meta = false, saw_counters = false, saw_ramp = false;
    bool saw_end = false, in_emitter = false, emitter_done = false;
    bool in_carry = false, carry_done = false;
    // The §18.12 coupling records are staged and spliced after the loop, so the record
    // dispatch stays independent of line order (a QL may legally precede its L row).
    // Resolving a QL into s.local_carry inline would make correctness depend on the
    // writer's line order, which nothing else in this format does.
    std::map<std::tuple<int, std::string, std::string>,
             std::vector<std::pair<int, double>>> q_rows;
    std::map<int, std::vector<std::pair<int, double>>> q_zrows;

    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (in_emitter) {
            // Verbatim emitter blob through its EEND terminator (counted lines).
            ++n_lines;
            s.emitter_state += line;
            s.emitter_state += '\n';
            if (line == "EEND") { in_emitter = false; emitter_done = true; }
            continue;
        }
        if (in_carry) {
            // Verbatim invariant-carry z-store blob through its CEND terminator.
            ++n_lines;
            s.carry_state += line;
            s.carry_state += '\n';
            if (line == "CEND") { in_carry = false; carry_done = true; }
            continue;
        }
        if (line.rfind("END,", 0) == 0) {
            unsigned long long expect = 0;
            if (!parseU(line.substr(4), expect)) return fail("malformed END line");
            if (expect != n_lines)
                return fail(fmt::format("line-count mismatch (END says {}, saw {})",
                                        expect, n_lines));
            saw_end = true;
            break;
        }
        ++n_lines;
        if (line.rfind("EMITTER,", 0) == 0) {
            if (emitter_done) return fail("duplicate emitter section");
            in_emitter = true;
            s.emitter_state += line;
            s.emitter_state += '\n';
            continue;
        }
        if (line.rfind("CARRY,", 0) == 0) {   // invariant-carry z-store blob
            if (carry_done) return fail("duplicate carry section");
            in_carry = true;
            s.carry_state += line;
            s.carry_state += '\n';
            continue;
        }
        const std::vector<std::string> f = splitCsv(line);
        if (f.empty() || f[0].empty()) return fail("blank/untagged line");
        const std::string& tag = f[0];
        long long i1 = 0, i2 = 0;
        unsigned long long u = 0;
        double d1 = 0, d2 = 0, d3 = 0;
        if (tag == "M") {
            if (f.size() != 6) return fail("malformed M line");
            if (!parseI(f[1], i1) || !parseD(f[2], d1)) return fail("malformed M time");
            s.snap_week = static_cast<int>(i1);
            s.snap_tow  = d1;
            s.wall_utc  = f[3];
            if (!parseU(f[4], u)) return fail("malformed M generation id");
            s.generation_id = static_cast<uint32_t>(u);
            if (!parseHex64(f[5], s.config_fingerprint)) return fail("malformed M fingerprint");
            saw_meta = true;
        } else if (tag == "N") {
            if (f.size() != 13) return fail("malformed N line");
            unsigned long long uc[7];
            for (int k = 0; k < 7; ++k)
                if (!parseU(f[1 + k], uc[k])) return fail("malformed N counter");
            s.n_emitted          = uc[0];
            s.n_skipped          = uc[1];
            s.n_gate_withheld    = uc[2];
            s.n_sigma0_withheld  = uc[3];
            s.n_runaway_epochs   = uc[4];
            s.n_perconst_partial = uc[5];
            s.n_perconst_diffuse = uc[6];
            if (!parseI(f[8], i1) || !parseI(f[9], i2)) return fail("malformed N iod/week");
            s.iod       = static_cast<int>(i1);
            s.prev_week = static_cast<int>(i2);
            if (!parseD(f[10], s.prev_tow) || !parseD(f[11], s.last_emit_content_s)
                || !parseD(f[12], s.last_health_write_s))
                return fail("malformed N times");
            saw_counters = true;
        } else if (tag == "U") {
            if (f.size() != 5) return fail("malformed U line");
            bool b = false;
            if (!parseD(f[1], s.ura_sigma0_ewma) || !parseU(f[2], u) || !parseB(f[3], b)
                || !parseD(f[4], s.first_emit_content_s))
                return fail("malformed U fields");
            s.ura_ewma_updates = u;
            s.ramp_lifted      = b;
            saw_ramp = true;
        } else if (tag == "D") {
            if (f.size() != 10) return fail("malformed D line");
            NetworkStateSnapshot::SharedCarry c;
            bool b = false;
            if (!parseI(f[1], i1)) return fail("malformed D kind");
            if (i1 < 0 || i1 > 3) return fail("D kind out of range");
            c.kind   = static_cast<int>(i1);
            c.sat    = f[2];
            c.signal = f[3];
            long long n = 0, m = 0, tr = 0;
            if (!parseI(f[4], n) || !parseI(f[5], m) || !parseI(f[6], tr))
                return fail("malformed D sh fields");
            c.sh_n = static_cast<int>(n);
            c.sh_m = static_cast<int>(m);
            c.sh_trig = static_cast<int>(tr);
            if (!parseD(f[7], c.value) || !parseD(f[8], c.var) || !parseB(f[9], b))
                return fail("malformed D value/var/flag");
            c.inflate_q = b;
            s.shared_carry.push_back(std::move(c));
        } else if (tag == "V" || tag == "W") {
            if (f.size() != 3) return fail("malformed pivot line");
            (tag == "V" ? s.prev_pivot : s.last_pivot)[f[1]] = f[2];
        } else if (tag == "K") {
            if (f.size() != 9) return fail("malformed K line");
            NetworkStateSnapshot::Osc o;
            bool b = false;
            if (!parseD(f[2], o.x0) || !parseD(f[3], o.x1) || !parseD(f[4], o.p00)
                || !parseD(f[5], o.p01) || !parseD(f[6], o.p11) || !parseD(f[7], o.t_last)
                || !parseB(f[8], b))
                return fail("malformed K fields");
            o.init = b;
            s.clock_osc[f[1]] = o;
        } else if (tag == "J") {
            if (f.size() != 4) return fail("malformed J line");
            if (!parseD(f[2], d1) || !parseD(f[3], d2)) return fail("malformed J fields");
            s.prev_clk[f[1]] = {d1, d2};
        } else if (tag == "L") {
            if (f.size() != 7 && f.size() != 8) return fail("malformed L line");
            if (!parseI(f[1], i1)) return fail("malformed L station index");
            if (!parseD(f[4], d1) || !parseD(f[5], d2) || !parseD(f[6], d3))
                return fail("malformed L fields");
            NetworkStateSnapshot::Local L{d1, d2, d3};
            if (f.size() == 8) {   // optional first-stash variance (handover ratio ceiling)
                if (!parseD(f[7], L.var_first) || !std::isfinite(L.var_first)
                    || L.var_first < 0.0)
                    return fail("malformed L var_first");
            }
            s.local_carry[{static_cast<int>(i1), f[2], f[3]}] = L;
        } else if (tag == "Z") {
            if (f.size() != 5) return fail("malformed Z line");
            if (!parseI(f[1], i1)) return fail("malformed Z station index");
            if (!parseD(f[2], d1) || !parseD(f[3], d2) || !parseD(f[4], d3))
                return fail("malformed Z fields");
            s.zwd_carry[static_cast<int>(i1)] = NetworkStateSnapshot::Local{d1, d2, d3};
        } else if (tag == "R") {
            if (f.size() != 2) return fail("malformed R line");
            s.ura_retro_sats.insert(f[1]);
        } else if (tag == "A") {
            if (f.size() != 2) return fail("malformed A line");
            s.retro_active.insert(f[1]);
        } else if (tag == "Y") {
            if (f.size() != 3) return fail("malformed Y line");
            if (!parseI(f[2], i1)) return fail("malformed Y count");
            s.ura_refloat_horizon[f[1]] = static_cast<int>(i1);
        } else if (tag == "C") {
            if (f.size() != 2) return fail("malformed C line");
            s.c1_zoh_active.insert(f[1]);
        } else if (tag == "I") {   // eph re-level baseline IODE
            if (f.size() != 3) return fail("malformed I line");
            if (!parseI(f[2], i1)) return fail("malformed I iode");
            s.relevel_iode[f[1]] = static_cast<int>(i1);
        } else if (tag == "X") {   // pending eph-cutover disc
            if (f.size() != 2) return fail("malformed X line");
            s.eph_cutover_disc_pending.insert(f[1]);
        } else if (tag == "SV") {  // satellite -> carry vertex id
            if (f.size() != 3) return fail("malformed SV line");
            if (!parseI(f[2], i1)) return fail("malformed SV vertex id");
            s.carry_sat_vertex[f[1]] = static_cast<int>(i1);
        } else if (tag == "GG") {  // (const,signal) -> carry group id
            if (f.size() != 4 || f[1].size() != 1) return fail("malformed GG line");
            if (!parseI(f[3], i1)) return fail("malformed GG group id");
            s.carry_group_id.emplace_back(f[1][0], f[2], static_cast<int>(i1));
        } else if (tag == "PA") {   // frame record, phase-anchor incumbent (§18.10)
            if (f.size() != 3) return fail("malformed PA line");
            s.anchor_incumbent[f[1]] = f[2];
        } else if (tag == "PT") {   // frame record, sticky pin target (§18.10)
            if (f.size() != 3) return fail("malformed PT line");
            if (!parseD(f[2], d1)) return fail("malformed PT target");
            s.anchor_pin_target[f[1]] = d1;
        } else if (tag == "CA") {   // frame record, clock-anchor per-sat state (§18.10)
            if (f.size() != 7) return fail("malformed CA line");
            NetworkStateSnapshot::ClockAnchorSat ca;
            if (!parseD(f[2], ca.t_last) || !parseD(f[3], ca.last_value)
                || !parseD(f[4], ca.last_var_eff) || !parseI(f[5], i1) || !parseI(f[6], i2))
                return fail("malformed CA fields");
            ca.in_band  = static_cast<int>(i1);
            ca.anchored = (i2 != 0);
            s.clock_anchor[f[1]] = ca;
        } else if (tag == "FA") {   // frame record, CLK-R accumulator (§18.10)
            if (f.size() != 3 || f[1].size() != 1) return fail("malformed FA line");
            if (!parseD(f[2], d1)) return fail("malformed FA value");
            s.clk_frame_acc[f[1][0]] = d1;
        } else if (tag == "FP") {   // frame record, CLK-R per-sat baseline (§18.10)
            if (f.size() != 4) return fail("malformed FP line");
            if (!parseD(f[2], d1) || !parseD(f[3], d2)) return fail("malformed FP fields");
            s.clk_frame_prev[f[1]] = {d1, d2};
        } else if (tag == "FE") {   // frame record, CLK-R epoch count (§18.10)
            if (f.size() != 2 || !parseI(f[1], i1)) return fail("malformed FE line");
            s.clk_frame_epochs = static_cast<long long>(i1);
        } else if (tag == "QS") {   // coupling shared-column dictionary entry (§18.12)
            if (f.size() != 11) return fail("malformed QS line");
            long long id = 0, kd = 0, shn = 0, shm = 0, trg = 0, lay = 0;
            if (!parseI(f[1], id) || !parseI(f[2], kd) || !parseI(f[5], shn)
                || !parseI(f[6], shm) || !parseI(f[7], trg) || !parseI(f[8], lay)
                || !parseD(f[9], d1) || !parseD(f[10], d2))
                return fail("malformed QS fields");
            if (id != static_cast<long long>(s.coupling_dict.size()))
                return fail(fmt::format("QS ids must be dense and ascending "
                                        "(expected {}, saw {})",
                                        s.coupling_dict.size(), id));
            if (kd < 0 || kd > 3) return fail("QS kind out of range");
            // p_prev == 0 is valid (a datum-pinned or ensemble-gauge column, known
            // exactly); only a negative or non-finite value is corrupt.  fmt
            // writes "nan"/"inf" and parseD consumes them fully, so without this screen a
            // corrupt value round-trips and then hits buildCplRow's non-finite RHS guard
            // (Coupling.cpp), which skips the whole row and so loses the arc's
            // level, not just its coupling.
            if (!std::isfinite(d1) || !std::isfinite(d2) || d2 < 0.0)
                return fail("QS ŝ_prev/P_prev not finite, or P_prev < 0");
            NetworkStateSnapshot::CouplingCol q;
            q.kind = static_cast<int>(kd);
            q.sat = f[3];  q.signal = f[4];        // may be empty (VtecSh / Clock)
            q.sh_n = static_cast<int>(shn);
            q.sh_m = static_cast<int>(shm);
            q.sh_trig = static_cast<int>(trg);
            q.sh_layer = static_cast<int>(lay);
            q.s_prev = d1;  q.p_prev = d2;
            s.coupling_dict.push_back(std::move(q));
        } else if (tag == "QL" || tag == "QZ") {   // one carried row's regression (§18.12)
            const bool zrow = (tag == "QZ");
            const std::size_t head = zrow ? 3u : 5u;   // tag, idx [, sat, signal], k
            if (f.size() < head) return fail("malformed " + tag + " line");
            long long k = 0;
            if (!parseI(f[head - 1], k) || k <= 0)
                return fail("malformed " + tag + " entry count");
            // The parser enforces row completeness: a partially restored coefficient
            // vector breaks the exact gauge identity and is worse than no
            // coupling at all.  The explicit k makes this checkable, because a line that
            // lost one whole (id, M) pair still has even arity.
            if (f.size() != head + 2u * static_cast<std::size_t>(k))
                return fail(fmt::format("{} entry-count mismatch (says {}, saw {} field(s))",
                                        tag, k, f.size() - head));
            if (!parseI(f[1], i1)) return fail("malformed " + tag + " station index");
            std::vector<std::pair<int, double>> ents;
            ents.reserve(static_cast<std::size_t>(k));
            for (long long e = 0; e < k; ++e) {
                const std::size_t b = head + 2u * static_cast<std::size_t>(e);
                long long eid = 0;
                if (!parseI(f[b], eid) || !parseD(f[b + 1u], d1))
                    return fail("malformed " + tag + " entry");
                if (!std::isfinite(d1)) return fail(tag + " coefficient not finite");
                ents.emplace_back(static_cast<int>(eid), d1);
            }
            const bool fresh = zrow
                ? q_zrows.emplace(static_cast<int>(i1), std::move(ents)).second
                : q_rows.emplace(std::make_tuple(static_cast<int>(i1), f[2], f[3]),
                                 std::move(ents)).second;
            if (!fresh) return fail("duplicate " + tag + " row");
        } else if (tag == "QR") {   // write-epoch clock-monitor refloat set (§18.12)
            if (f.size() != 2) return fail("malformed QR line");
            s.clock_refloat_sats.insert(f[1]);
        } else if (tag == "QK") {   // dense-carry key, row order (§18.12 (2a))
            if (f.size() != 3) return fail("malformed QK line");
            long long id = 0;
            if (!parseI(f[1], id) || id != static_cast<long long>(s.carry_cov_keys.size()))
                return fail("QK ids must be dense and ascending");
            if (f[2].empty()) return fail("empty QK key");
            s.carry_cov_keys.push_back(f[2]);
        } else if (tag == "QP") {   // dense-carry covariance row, lower triangle (§18.12 (2a))
            if (f.size() < 3) return fail("malformed QP line");
            long long id = 0, n = 0;
            if (!parseI(f[1], id) || id != static_cast<long long>(s.carry_cov_rows.size()))
                return fail("QP ids must be dense and ascending");
            if (!parseI(f[2], n) || n != id + 1
                || f.size() != 3u + static_cast<std::size_t>(n))
                return fail("QP row length mismatch");
            std::vector<double> row(static_cast<std::size_t>(n));
            for (long long e = 0; e < n; ++e) {
                if (!parseD(f[3u + static_cast<std::size_t>(e)], d1) || !std::isfinite(d1))
                    return fail("QP entry not finite");
                row[static_cast<std::size_t>(e)] = d1;
            }
            s.carry_cov_rows.push_back(std::move(row));
        } else if (tag == "EQD") {  // eq34 sticky datum receiver per (cons,signal) (RS-3)
            if (f.size() != 3 || f[1].empty() || f[2].empty())
                return fail("malformed EQD line");
            s.eq34_datum_rx[f[1]] = f[2];
        } else if (tag == "EQP") {  // receiver's parent-edge satellite (RS-3)
            if (f.size() != 3 || f[1].empty() || f[2].empty())
                return fail("malformed EQP line");
            s.eq34_rx_parent[f[1]] = f[2];
        } else if (tag == "EQS") {  // satellite's component datum receiver (RS-3)
            if (f.size() != 3 || f[1].empty() || f[2].empty())
                return fail("malformed EQS line");
            s.eq34_sat_root[f[1]] = f[2];
        } else if (tag == "EQT") {  // last epoch's per-(station, amb) pin target (RS-3)
            if (f.size() != 4 || f[2].empty()) return fail("malformed EQT line");
            if (!parseI(f[1], i1) || !parseD(f[3], d1)) return fail("malformed EQT fields");
            s.eq34_prev_target[{static_cast<int>(i1), f[2]}] = d1;
        } else if (tag == "EQE") {  // last epoch's per-(station, sat) arc elevation (RS-3)
            if (f.size() != 4 || f[2].empty()) return fail("malformed EQE line");
            if (!parseI(f[1], i1) || !parseD(f[3], d1)) return fail("malformed EQE fields");
            s.eq34_prev_el[{static_cast<int>(i1), f[2]}] = d1;
        } else if (tag == "EQR") {  // gap-retained tree edge (RS-3)
            if (f.size() != 3 || f[2].empty()) return fail("malformed EQR line");
            if (!parseI(f[1], i1)) return fail("malformed EQR station index");
            s.eq34_retained_edges.emplace(static_cast<int>(i1), f[2]);
        } else if (tag == "CBP") {  // time-linked code-bias previous value (RS-3)
            if (f.size() != 4 || f[1].empty() || f[2].empty())
                return fail("malformed CBP line");
            if (!parseD(f[3], d1)) return fail("malformed CBP value");
            // The map holds a finite previous realization to difference against;
            // strtod accepts nan/inf, which would then corrupt the
            // veteran-referenced sum-to-zero RHS without any error.
            if (!std::isfinite(d1)) return fail("CBP value not finite");
            s.code_bias_prev[f[1]][f[2]] = d1;
        } else if (tag == "CBS") {  // code-bias ever-harvested satellite (RS-3)
            if (f.size() != 2 || f[1].empty()) return fail("malformed CBS line");
            s.code_bias_sat_seen.insert(f[1]);
        } else if (tag == "CBI") {  // code-bias IF-pin reference pair (RS-3)
            if (f.size() != 4 || f[1].empty() || f[2].empty() || f[3].empty())
                return fail("malformed CBI line");
            s.code_bias_ifpin_pair[f[1]] = {f[2], f[3]};
        } else if (tag == "ER") {   // eq34 emit re-base emission frame (RS-5)
            if (f.size() != 3 || f[1].empty()) return fail("malformed ER line");
            if (!parseD(f[2], d1)) return fail("malformed ER value");
            // The restored value is the frame the next epoch differences against; a
            // nan/inf, which strtod accepts, would propagate into every re-based
            // emitted bias for that key (same screen as CBP).
            if (!std::isfinite(d1)) return fail("ER value not finite");
            s.emit_rebase_last[f[1]] = d1;
        } else if (tag == "RC") {   // receiver code-bias (rx-CB) carry entry (RS-6)
            if (f.size() != 6 || f[2].empty()) return fail("malformed RC line");
            if (!parseI(f[1], i1)) return fail("malformed RC station index");
            if (!parseD(f[3], d1) || !parseD(f[4], d2) || !parseD(f[5], d3))
                return fail("malformed RC fields");
            // The restored value_abs is the prior the next fold's innovation
            // differences against and `var` is its weight, so a nan/inf (which strtod
            // accepts; same screen as ER / CBP) would corrupt every later fold of that
            // (station, family).  A non-positive variance is refused as well, since
            // the harvest only stashes var > 0.
            if (!std::isfinite(d1) || !std::isfinite(d2) || !std::isfinite(d3)
                || !(d2 > 0.0))
                return fail("RC field not finite (or a non-positive variance)");
            s.rx_cb_carry[{static_cast<int>(i1), f[2]}] =
                NetworkStateSnapshot::Local{d1, d2, d3};
        } else if (tag == "LB") {   // contrast-frame level information
            if (f.size() != 4 || f[1].empty()) return fail("malformed LB line");
            if (!parseD(f[2], d1) || !parseI(f[3], i1)) return fail("malformed LB fields");
            if (!std::isfinite(d1) || d1 < 0.0 || i1 < 0)
                return fail("LB field not finite (or negative)");
            s.level_info[f[1]] = {d1, static_cast<int>(i1)};
        } else if (tag == "SD") {   // T5 / Iono-2 per-LOS deviation carry entry (RS-6)
            if (f.size() != 6 || f[1].empty()) return fail("malformed SD line");
            if (!parseD(f[2], d1) || !parseD(f[3], d2) || !parseD(f[4], d3))
                return fail("malformed SD fields");
            double d4 = 0.0;
            if (!parseD(f[5], d4)) return fail("malformed SD t_s");
            if (!std::isfinite(d1) || !std::isfinite(d2) || !std::isfinite(d3)
                || !std::isfinite(d4) || d2 < 0.0 || d3 < 0.0)
                return fail("SD field not finite (or a negative variance)");
            s.sh_dev_carry[f[1]] = NetworkStateSnapshot::ShDev{d1, d2, d3, d4};
        } else if (tag == "CN") {   // canonical-signal latch entry (RS-8)
            // Refused at the parse like the RC / SD rows: the restored signal becomes
            // the station's locked per-band channel for the whole leg, so a malformed
            // row is not skipped.  An obs-type other than 'C' or 'L' would install an
            // entry no lookup can match; that band then acts as a cold latch and is
            // re-decided on the resume epoch, which this record exists to prevent.
            if (f.size() != 6 || f[2].empty() || f[3].empty() || f[5].empty())
                return fail("malformed CN line");
            if (!parseI(f[1], i1) || i1 < 0) return fail("malformed CN station index");
            if (f[4].size() != 1 || (f[4][0] != 'C' && f[4][0] != 'L'))
                return fail("CN obs-type must be exactly 'C' or 'L'");
            s.canonical_signals[static_cast<int>(i1)].push_back(
                NetworkStateSnapshot::CanonSignal{f[2], f[3], f[4][0], f[5]});
        } else if (tag == "RL") {   // code-bias linearization point, x_lin (RS-9)
            if (f.size() != 4 || f[2].empty()) return fail("malformed RL line");
            if (!parseI(f[1], i1)) return fail("malformed RL station index");
            if (!parseD(f[3], d1)) return fail("malformed RL x_lin");
            // Refused on the same ground as RC: this value becomes the station's
            // code-bias linearization point for the whole run, so a nan/inf (which
            // strtod accepts) would make every later innovation of that (station,
            // family) non-finite and, through computeRawResidual, corrupt the first
            // epoch's iono and ambiguity seeds.  No sign or magnitude screen: a
            // receiver DCB can have either sign, and the seed path's |est| < 200 m
            // sanity bound applies to the median estimator, not to a value the writer
            // already solved for.
            if (!std::isfinite(d1)) return fail("RL x_lin not finite");
            s.rx_cb_lin[{static_cast<int>(i1), f[2]}] = d1;
        } else {
            return fail("unknown record tag '" + tag + "'");
        }
    }

    if (in_emitter) return fail("truncated emitter section (no EEND)");
    if (in_carry)   return fail("truncated carry section (no CEND)");
    if (!saw_end)   return fail("missing END terminator (truncated file)");
    if (!saw_meta || !saw_counters || !saw_ramp)
        return fail("missing mandatory metadata (M/N/U) line");

    // Bind the staged coupling rows to their L / Z carries (§18.12 splice).  Every
    // failure here is a hard parse failure, never a partial restore.  The caller
    // (networkTryWarmResume in WarmResume.cpp) treats a parse failure as a
    // cold start, byte-identical to a run without a snapshot, or refuses the run
    // under network.state_snapshot_require.  A file
    // whose regressors cannot be read whole must not resume with truncated rows
    // (Amputation: Mᵀg_s ≠ −g_r(c), so every coupling row observes the clock
    // gauge and the W5 null-space gate withholds each epoch that carries rows).
    const auto ids_ok = [&](const std::vector<std::pair<int, double>>& v, int& bad) {
        for (const auto& e : v)
            if (e.first < 0
                || static_cast<std::size_t>(e.first) >= s.coupling_dict.size()) {
                bad = e.first;
                return false;
            }
        return true;
    };
    for (auto& [key, ents] : q_rows) {
        int bad = 0;
        if (!ids_ok(ents, bad))
            return fail(fmt::format("QL dictionary id {} out of range (dict size {})",
                                    bad, s.coupling_dict.size()));
        if (s.local_carry.find(key) == s.local_carry.end())
            return fail(fmt::format("QL,{},{},{} has no matching L row",
                                    std::get<0>(key), std::get<1>(key), std::get<2>(key)));
        s.coupling_rows[key] = std::move(ents);
    }
    for (auto& [idx, ents] : q_zrows) {
        int bad = 0;
        if (!ids_ok(ents, bad))
            return fail(fmt::format("QZ dictionary id {} out of range (dict size {})",
                                    bad, s.coupling_dict.size()));
        if (s.zwd_carry.find(idx) == s.zwd_carry.end())
            return fail(fmt::format("QZ,{} has no matching Z row", idx));
        s.coupling_zrows[idx] = std::move(ents);
    }
    // The dense-carry side-car (§18.12 (2a)) must be whole (n keys and n rows; row i's
    // length i+1 is checked per row above) or absent.  A partial block is a hard failure.
    if (s.carry_cov_rows.size() != s.carry_cov_keys.size())
        return fail(fmt::format("QK/QP count mismatch ({} key(s), {} row(s))",
                                s.carry_cov_keys.size(), s.carry_cov_rows.size()));

    out = std::move(s);
    return true;
}

bool NetworkStateSnapshotIO::write(const std::filesystem::path& path,
                                   const NetworkStateSnapshot& snap, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };

    const std::filesystem::path tmp(path.string() + ".tmp");
    {
        // Binary mode: '\n' only, so the on-disk bytes match serialize()'s output
        // exactly on every platform (no CRLF translation).
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return fail("cannot open temp file '" + tmp.string() + "'");
        serialize(os, snap);
        os.flush();
        if (!os) {
            std::error_code ec_rm;
            std::filesystem::remove(tmp, ec_rm);
            return fail("write to temp file '" + tmp.string() + "' failed");
        }
    }   // closed before the replace

    // Atomic replace.  MSVC's std::filesystem::rename maps to
    // MoveFileExW(MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) (see the
    // toolset's crt/src/stl/filesystem.cpp), so an existing destination is
    // replaced in one step.  COPY_ALLOWED degrades to copy+delete only across
    // volumes; the temp file lives beside the target (path + ".tmp"), on the same
    // volume, so the replace is a metadata-only atomic rename.  On this primary
    // path a crash at any point leaves either the old complete file or the new
    // complete file at @p path; the previous snapshot is never opened or written
    // in place.
    //
    // The remove+rename fallback below is a portability backstop for an
    // implementation that refuses an existing target.  It is not reached on MSVC
    // (the rename above already replaces an existing destination) and, unlike the
    // primary rename, it is not atomic: a crash between the remove and the rename
    // leaves no file at @p path.  The intact "<path>.tmp" survives that window
    // (recoverable by hand), and the branch runs only after the atomic replace has
    // already failed, so the narrow non-atomic gap is accepted.
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec_rm;
        std::filesystem::remove(path, ec_rm);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec_rm);
            return fail("atomic replace of '" + path.string() + "' failed: " + ec.message());
        }
    }
    return true;
}

bool NetworkStateSnapshotIO::read(const std::filesystem::path& path, NetworkStateSnapshot& out,
                                  std::string* err) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        if (err) *err = "cannot open '" + path.string() + "'";
        return false;
    }
    return parse(is, out, err);
}

bool NetworkStateSnapshotIO::warmResumeAllowed(const NetworkStateSnapshot& snap,
                                               uint64_t live_config_fingerprint,
                                               double first_live_epoch_s,
                                               double carry_horizon_s, std::string* reason,
                                               WarmResumeRefusal* why) {
    if (why) *why = WarmResumeRefusal::None;
    auto no = [&](WarmResumeRefusal w, std::string m) {
        if (reason) *reason = std::move(m);
        if (why) *why = w;
        return false;
    };
    if (snap.config_fingerprint != live_config_fingerprint)
        return no(WarmResumeRefusal::Fingerprint,
                  fmt::format("config fingerprint mismatch (snapshot {:016x} vs live {:016x} — "
                              "datum-relevant config changed)",
                              snap.config_fingerprint, live_config_fingerprint));
    const double snap_s = static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
    const double age_s  = first_live_epoch_s - snap_s;
    if (!(age_s > 0.0))
        return no(WarmResumeRefusal::NonPositiveAge,
                  fmt::format("first live epoch is not after the snapshot epoch (age {:.3f} s — "
                              "resuming at or before the snapshotted epoch would double-fold "
                              "its carry)",
                              age_s));
    if (age_s > carry_horizon_s)
        return no(WarmResumeRefusal::Stale,
                  fmt::format("stale: snapshot age {:.1f} s exceeds the carry-validity horizon "
                              "{:.1f} s (every carried key would be dropped/reseeded downstream)",
                              age_s, carry_horizon_s));
    if (reason)
        *reason = fmt::format("age {:.1f} s within horizon {:.1f} s; config fingerprint matches",
                              age_s, carry_horizon_s);
    return true;
}

std::string NetworkStateSnapshotIO::warmResumeRefusalMessage(bool require,
                                                             WarmResumeRefusal why,
                                                             const std::string& detail) {
    if (!require || why == WarmResumeRefusal::None) return {};

    const char* what = "the warm resume did not complete";
    switch (why) {
        case WarmResumeRefusal::MissingFile:
            what = "the snapshot file does not exist"; break;
        case WarmResumeRefusal::ParseFailure:
            what = "the snapshot file did not parse"; break;
        case WarmResumeRefusal::NoLiveEpochs:
            what = "there is no live epoch to resume against"; break;
        case WarmResumeRefusal::Fingerprint:
            what = "the snapshot's config fingerprint does not match this run"; break;
        case WarmResumeRefusal::NonPositiveAge:
            what = "the snapshot is NOT OLDER than the first epoch this run would process"; break;
        case WarmResumeRefusal::Stale:
            what = "the snapshot is older than the carry-validity horizon"; break;
        case WarmResumeRefusal::EmitterRestore:
            what = "the snapshot's EMITTER section failed to restore"; break;
        default: break;
    }

    // The one refusal with a harness-specific remedy.  A leg overwrites its own
    // seed at its first emitted epoch (state_snapshot_interval_s does not suppress
    // that write), so relaunching the exe on an already-run leg.json puts the
    // first live epoch at the snapshot epoch (age 0.000 s).
    const char* remedy =
        (why == WarmResumeRefusal::NonPositiveAge)
            ? "  This is what an EVENT-HARNESS leg relaunched IN PLACE looks like: the "
              "previous run overwrote its own seed at its first emitted epoch, so the "
              "snapshot now IS the epoch you are resuming to.  Re-seed from the pristine "
              "snapshot with `python scripts/event_leg.py rerun <legdir>` "
              "(docs/SPEC_event_harness.md §6b) — never relaunch the exe on an existing "
              "leg.json."
            : "  Fix the snapshot/config pair, or clear network.state_snapshot_require to "
              "accept a cold start.";

    return fmt::format(
        "Network (RTB-7d/HB-1): network.state_snapshot_require is ARMED and {} — {}.  "
        "The run is REFUSED BEFORE ITS FIRST EPOCH; it does NOT fall through to a cold "
        "start (that is exactly what `require` means, and a cold leg graded as a warm one "
        "is the failure this guard exists to prevent).{}",
        what, detail, remedy);
}

} // namespace GnssProcessor
