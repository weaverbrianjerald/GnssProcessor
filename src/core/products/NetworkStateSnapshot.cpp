/**
 * @file NetworkStateSnapshot.cpp
 * @brief Implementation of the RTB-7 network-producer state snapshot
 *        (serialization, strict parse, atomic file replace, warm/cold gate).
 */

#include "products/NetworkStateSnapshot.hpp"

#include <fmt/format.h>

#include <cerrno>
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

    os << kHeader << '\n';
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
    for (const auto& [sat, o] : snap.clock_rate_est)   // network.emit_clock_rate decoupled C1 state
        put(fmt::format("O,{},{},{},{},{},{},{},{}", sat, o.x0, o.x1, o.p00, o.p01, o.p11,
                        o.t_last, o.init ? 1 : 0));
    for (const auto& [sat, vp] : snap.prev_clk)
        put(fmt::format("J,{},{},{}", sat, vp.first, vp.second));
    for (const auto& [key, l] : snap.local_carry)
        put(fmt::format("L,{},{},{},{},{},{}", std::get<0>(key), std::get<1>(key),
                        std::get<2>(key), l.value_abs, l.var, l.last_seen_s));
    for (const auto& [idx, l] : snap.zwd_carry)
        put(fmt::format("Z,{},{},{},{}", idx, l.value_abs, l.var, l.last_seen_s));
    for (const auto& s : snap.ura_retro_sats) put(fmt::format("R,{}", s));
    for (const auto& s : snap.retro_active)   put(fmt::format("A,{}", s));
    for (const auto& [sat, n] : snap.ura_refloat_horizon)
        put(fmt::format("Y,{},{}", sat, n));
    for (const auto& s : snap.c1_zoh_active)  put(fmt::format("C,{}", s));
    for (const auto& [sat, iode] : snap.relevel_iode)   // Card 6
        put(fmt::format("I,{},{}", sat, iode));
    for (const auto& s : snap.eph_cutover_disc_pending) // Card 6
        put(fmt::format("X,{}", s));
    // Card 4 (M1) invariant carry (RTB-7 branch-faithful resume): the ArcKey vertex
    // numbering, then the z-store blob verbatim (CARRY header .. CEND).  All three are
    // written ONLY when the carry blob is present (network.state_snapshot_carry), so a
    // default snapshot is byte-identical to the pre-feature format.
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
    if (line != kHeader) return fail("unrecognized header: '" + line + "'");

    NetworkStateSnapshot s;
    uint64_t n_lines = 0;
    bool saw_meta = false, saw_counters = false, saw_ramp = false;
    bool saw_end = false, in_emitter = false, emitter_done = false;
    bool in_carry = false, carry_done = false;

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
        if (line.rfind("CARRY,", 0) == 0) {   // Card 4 invariant carry z-store blob
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
        } else if (tag == "O") {   // network.emit_clock_rate decoupled C1 rate state
            if (f.size() != 9) return fail("malformed O line");
            NetworkStateSnapshot::Osc o;
            bool b = false;
            if (!parseD(f[2], o.x0) || !parseD(f[3], o.x1) || !parseD(f[4], o.p00)
                || !parseD(f[5], o.p01) || !parseD(f[6], o.p11) || !parseD(f[7], o.t_last)
                || !parseB(f[8], b))
                return fail("malformed O fields");
            o.init = b;
            s.clock_rate_est[f[1]] = o;
        } else if (tag == "J") {
            if (f.size() != 4) return fail("malformed J line");
            if (!parseD(f[2], d1) || !parseD(f[3], d2)) return fail("malformed J fields");
            s.prev_clk[f[1]] = {d1, d2};
        } else if (tag == "L") {
            if (f.size() != 7) return fail("malformed L line");
            if (!parseI(f[1], i1)) return fail("malformed L station index");
            if (!parseD(f[4], d1) || !parseD(f[5], d2) || !parseD(f[6], d3))
                return fail("malformed L fields");
            s.local_carry[{static_cast<int>(i1), f[2], f[3]}] =
                NetworkStateSnapshot::Local{d1, d2, d3};
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
        } else if (tag == "I") {   // Card 6 eph re-level baseline
            if (f.size() != 3) return fail("malformed I line");
            if (!parseI(f[2], i1)) return fail("malformed I iode");
            s.relevel_iode[f[1]] = static_cast<int>(i1);
        } else if (tag == "X") {   // Card 6 pending eph-cutover disc
            if (f.size() != 2) return fail("malformed X line");
            s.eph_cutover_disc_pending.insert(f[1]);
        } else if (tag == "SV") {  // Card 4: satellite -> carry vertex id
            if (f.size() != 3) return fail("malformed SV line");
            if (!parseI(f[2], i1)) return fail("malformed SV vertex id");
            s.carry_sat_vertex[f[1]] = static_cast<int>(i1);
        } else if (tag == "GG") {  // Card 4: (const,signal) -> carry group id
            if (f.size() != 4 || f[1].size() != 1) return fail("malformed GG line");
            if (!parseI(f[3], i1)) return fail("malformed GG group id");
            s.carry_group_id.emplace_back(f[1][0], f[2], static_cast<int>(i1));
        } else {
            return fail("unknown record tag '" + tag + "'");
        }
    }

    if (in_emitter) return fail("truncated emitter section (no EEND)");
    if (in_carry)   return fail("truncated carry section (no CEND)");
    if (!saw_end)   return fail("missing END terminator (truncated file)");
    if (!saw_meta || !saw_counters || !saw_ramp)
        return fail("missing mandatory metadata (M/N/U) line");

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
    // MoveFileExW(MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) — verified
    // in the toolset's crt/src/stl/filesystem.cpp — so an existing destination
    // is replaced in one step.  COPY_ALLOWED degrades to copy+delete only
    // ACROSS volumes; the temp file lives beside the target (path + ".tmp"),
    // i.e. always the same volume, so the replace is a metadata-only atomic
    // rename here.  ON THIS PRIMARY PATH a crash at any point leaves either the
    // old complete file or the new complete file at @p path — the previous good
    // snapshot is never opened or written in place.
    //
    // The remove+rename fallback below is a portability backstop for an
    // implementation that refuses an existing target; it is NOT reached on MSVC
    // (the replace above already handles an existing destination) and — unlike
    // the primary rename — it is NOT atomic: a crash in the window between the
    // remove and the rename leaves neither file at @p path.  The intact
    // "<path>.tmp" still survives that window (recoverable by hand), and reaching
    // the branch at all already implies the atomic replace itself failed, so we
    // accept the narrow non-atomic gap rather than claim a guarantee it lacks.
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
                                               double carry_horizon_s, std::string* reason) {
    auto no = [&](std::string m) {
        if (reason) *reason = std::move(m);
        return false;
    };
    if (snap.config_fingerprint != live_config_fingerprint)
        return no(fmt::format("config fingerprint mismatch (snapshot {:016x} vs live {:016x} — "
                              "datum-relevant config changed)",
                              snap.config_fingerprint, live_config_fingerprint));
    const double snap_s = static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
    const double age_s  = first_live_epoch_s - snap_s;
    if (!(age_s > 0.0))
        return no(fmt::format("first live epoch is not after the snapshot epoch (age {:.3f} s — "
                              "resuming at or before the snapshotted epoch would double-fold "
                              "its carry)",
                              age_s));
    if (age_s > carry_horizon_s)
        return no(fmt::format("stale: snapshot age {:.1f} s exceeds the carry-validity horizon "
                              "{:.1f} s (every carried key would be dropped/reseeded downstream)",
                              age_s, carry_horizon_s));
    if (reason)
        *reason = fmt::format("age {:.1f} s within horizon {:.1f} s; config fingerprint matches",
                              age_s, carry_horizon_s);
    return true;
}

} // namespace GnssProcessor
