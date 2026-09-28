/**
 * @file RtStateSnapshot.cpp
 * @brief Implementation of the RT filter-state snapshot IO (serialization,
 *        strict parse, atomic file replace).  Format documented in the header.
 */

#include "realtime/RtStateSnapshot.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace GnssProcessor {

namespace {

constexpr const char* kHeader = "# gnss_rt_state_snapshot v1";

/// Split a line on ','.  Keys in this format (SM key-schema strings, RINEX
/// signals, "sat|signal") never contain commas, so a plain split is exact.
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

/// Strict full-consumption double parse.
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

std::string utcNowIso() {
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

} // namespace

void RtStateSnapshotIO::serialize(std::ostream& os, const RtStateSnapshot& snap) {
    uint64_t n_lines = 0;
    auto put = [&](const std::string& line) {
        os << line << '\n';
        ++n_lines;
    };

    os << kHeader << '\n';
    put(fmt::format("M,{},{:.17g},{},{:016x},{},{}", snap.snap_week, snap.snap_tow,
                    snap.wall_utc.empty() ? std::string("unknown") : snap.wall_utc,
                    snap.fingerprint, snap.core_size, snap.state_size));
    put(fmt::format("G,{},{}", snap.generation_id, snap.epoch_count));
    for (const auto& d : snap.dyn)
        put(fmt::format("K,{},{},{}", d.idx, d.kind, d.key));
    for (Eigen::Index i = 0; i < snap.x.size(); ++i)
        put(fmt::format("X,{},{:.17g}", static_cast<long long>(i), snap.x(i)));
    for (Eigen::Index i = 0; i < snap.P.rows(); ++i) {
        std::string row = fmt::format("P,{}", static_cast<long long>(i));
        for (Eigen::Index j = 0; j <= i; ++j)
            row += fmt::format(",{:.17g}", snap.P(i, j));
        put(row);
    }
    for (const auto& [key, ctr] : snap.ssr_disc) put(fmt::format("D,{},{}", key, ctr));
    for (const auto& [key, b] : snap.phase_bias_applied)
        put(fmt::format("F,{},{}", key, b ? 1 : 0));
    for (const auto& [key, b] : snap.code_bias_applied)
        put(fmt::format("C,{},{}", key, b ? 1 : 0));
    os << "END," << n_lines << '\n';
}

bool RtStateSnapshotIO::parse(std::istream& is, RtStateSnapshot& out, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };

    std::string line;
    if (!std::getline(is, line)) return fail("empty file");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kHeader) return fail("unrecognized header: '" + line + "'");

    RtStateSnapshot s;
    uint64_t n_lines = 0;
    bool saw_meta = false, saw_engine = false, saw_end = false;
    std::size_t n_x_seen = 0, n_p_seen = 0;

    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
        const std::vector<std::string> f = splitCsv(line);
        if (f.empty() || f[0].empty()) return fail("blank/untagged line");
        const std::string& tag = f[0];
        long long i1 = 0;
        unsigned long long u = 0;
        double d1 = 0;
        if (tag == "M") {
            if (f.size() != 7) return fail("malformed M line");
            if (!parseI(f[1], i1) || !parseD(f[2], d1)) return fail("malformed M time");
            s.snap_week = static_cast<int>(i1);
            s.snap_tow  = d1;
            s.wall_utc  = f[3];
            if (!parseHex64(f[4], s.fingerprint)) return fail("malformed M fingerprint");
            unsigned long long cs = 0, ss = 0;
            if (!parseU(f[5], cs) || !parseU(f[6], ss)) return fail("malformed M sizes");
            s.core_size  = static_cast<std::size_t>(cs);
            s.state_size = static_cast<std::size_t>(ss);
            if (s.state_size < s.core_size) return fail("M sizes inconsistent");
            s.x = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(s.state_size));
            s.P = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(s.state_size),
                                        static_cast<Eigen::Index>(s.state_size));
            saw_meta = true;
        } else if (tag == "G") {
            if (f.size() != 3) return fail("malformed G line");
            unsigned long long g = 0, e = 0;
            if (!parseU(f[1], g) || !parseU(f[2], e)) return fail("malformed G fields");
            s.generation_id = static_cast<uint32_t>(g);
            s.epoch_count   = e;
            saw_engine = true;
        } else if (tag == "K") {
            if (!saw_meta) return fail("K before M");
            if (f.size() != 4) return fail("malformed K line");
            if (!parseU(f[1], u)) return fail("malformed K index");
            RtStateSnapshot::DynState d;
            d.idx  = static_cast<std::size_t>(u);
            d.kind = f[2];
            d.key  = f[3];
            if (d.idx < s.core_size || d.idx >= s.state_size)
                return fail(fmt::format("K index {} outside the dynamic block", d.idx));
            // Ascending, gapless replay contract: index = core + position.
            if (d.idx != s.core_size + s.dyn.size())
                return fail(fmt::format("K index {} out of order (expected {})", d.idx,
                                        s.core_size + s.dyn.size()));
            if (d.kind.empty() || d.key.empty()) return fail("malformed K kind/key");
            s.dyn.push_back(std::move(d));
        } else if (tag == "X") {
            if (!saw_meta) return fail("X before M");
            if (f.size() != 3) return fail("malformed X line");
            if (!parseU(f[1], u) || !parseD(f[2], d1)) return fail("malformed X fields");
            if (u >= s.state_size) return fail("X index out of range");
            s.x(static_cast<Eigen::Index>(u)) = d1;
            ++n_x_seen;
        } else if (tag == "P") {
            if (!saw_meta) return fail("P before M");
            if (f.size() < 3) return fail("malformed P line");
            if (!parseU(f[1], u)) return fail("malformed P row index");
            const auto i = static_cast<Eigen::Index>(u);
            if (u >= s.state_size) return fail("P row out of range");
            if (f.size() != static_cast<std::size_t>(i) + 3)
                return fail(fmt::format("P row {} has {} values (expected {})", u,
                                        f.size() - 2, i + 1));
            for (Eigen::Index j = 0; j <= i; ++j) {
                if (!parseD(f[static_cast<std::size_t>(j) + 2], d1))
                    return fail("malformed P value");
                s.P(i, j) = d1;
                s.P(j, i) = d1;
            }
            ++n_p_seen;
        } else if (tag == "D") {
            if (f.size() != 3) return fail("malformed D line");
            if (!parseI(f[2], i1)) return fail("malformed D counter");
            s.ssr_disc[f[1]] = static_cast<int>(i1);
        } else if (tag == "F" || tag == "C") {
            if (f.size() != 3) return fail("malformed F/C line");
            bool b = false;
            if (!parseB(f[2], b)) return fail("malformed F/C flag");
            (tag == "F" ? s.phase_bias_applied : s.code_bias_applied)[f[1]] = b;
        } else {
            return fail("unknown record tag '" + tag + "'");
        }
    }

    if (!saw_end)  return fail("missing END terminator (truncated file)");
    if (!saw_meta || !saw_engine) return fail("missing mandatory M/G line");
    if (n_x_seen != s.state_size)
        return fail(fmt::format("X rows {} != state_size {}", n_x_seen, s.state_size));
    if (n_p_seen != s.state_size)
        return fail(fmt::format("P rows {} != state_size {}", n_p_seen, s.state_size));
    if (s.core_size + s.dyn.size() != s.state_size)
        return fail(fmt::format("K rows {} + core {} != state_size {}", s.dyn.size(),
                                s.core_size, s.state_size));

    out = std::move(s);
    return true;
}

bool RtStateSnapshotIO::write(const std::filesystem::path& path,
                              const RtStateSnapshot& snap, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };

    RtStateSnapshot stamped = snap;
    if (stamped.wall_utc.empty()) stamped.wall_utc = utcNowIso();

    const std::filesystem::path tmp(path.string() + ".tmp");
    {
        // Binary mode: '\n' only, so on-disk bytes match serialize() everywhere.
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return fail("cannot open temp file '" + tmp.string() + "'");
        serialize(os, stamped);
        os.flush();
        if (!os) {
            std::error_code ec_rm;
            std::filesystem::remove(tmp, ec_rm);
            return fail("write to temp file '" + tmp.string() + "' failed");
        }
    }
    // Atomic replace (the RTB-7 pattern: MSVC rename maps to
    // MoveFileExW(REPLACE_EXISTING)).  The remove+rename branch is a portability
    // fallback for implementations that refuse an existing target; it is not
    // atomic, since the target is briefly absent between the two calls.
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

bool RtStateSnapshotIO::read(const std::filesystem::path& path, RtStateSnapshot& out,
                             std::string* err) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        if (err) *err = "cannot open '" + path.string() + "'";
        return false;
    }
    return parse(is, out, err);
}

} // namespace GnssProcessor
