/**
 * @file RinexNavParser.cpp
 * @brief RINEX 3/4 navigation-message parser → @c BroadcastEphemeris.
 *
 * Field layout follows the RINEX 3.04 / 4.00 navigation format (fixed 19-column
 * D19.12 fields).  The Keplerian elements are copied without scaling, because
 * RINEX nav already stores angles in radians and rates in rad/s.  The time
 * conventions match @c RtcmNavDecoder.cpp: GPS/QZSS/Galileo on the GPS-week
 * scale the file carries; BeiDou @c toe/@c toc on the native BDT week numbering,
 * which @c LiveEphemerisStore handles by shifting the query epoch to BDT
 * (@c kBdtToGpsOffsetSec, 1356 weeks + 14 s) before forming t_k.  See
 * @c RinexNavParser.hpp for scope.
 */

#include "products/RinexNavParser.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>   // navLog()'s never-null fallback

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

// Fixed RINEX navigation field width (D19.12).  Continuation-line values start
// at columns 4, 23, 42, 61; the epoch line's three clock terms at 23, 42, 61.
constexpr std::size_t kFieldW = 19;

/// Diagnostic logger for the parser's warn paths.  Resolved on each call and
/// never null.
///
/// The logger is resolved per call and never cached as a raw pointer taken from
/// a by-value `shared_ptr` in a function-local `static`, for two reasons:
///
///  * `spdlog::default_logger()` can return null.  `spdlog::drop_all()` clears
///    the registry's default logger, and both `LogManager::initialize` (on
///    re-initialization) and `LogManager::shutdown` call it.  A cached null is
///    dereferenced on the next warn and faults with an access violation instead
///    of emitting the diagnostic (the `RejectsPreRinex3` and
///    `MissingEndOfHeaderReturnsEmpty` tests exercise these paths).
///  * A choice cached on the first call is frozen, so a "gnss_processor" logger
///    that LogManager registers later would never be picked up.
///
/// Returns a `shared_ptr`, not a raw pointer, so the logger cannot be dropped
/// while the caller's `->warn(...)` runs.  This follows the degrade-rather-than-
/// throw contract `LogManager::getLogger()` documents for pre-initialization
/// call sites.  RinexNavParser is a static utility class, so there is no
/// LogManager to inject (`tailLog()` in NetworkProductIO.cpp has the same shape).
std::shared_ptr<spdlog::logger> navLog() {
    if (auto named = spdlog::get("gnss_processor")) return named;
    if (auto def = spdlog::default_logger()) return def;
    // The registry has no default (after drop_all).  An unregistered console
    // fallback keeps the diagnostic from being lost or faulting.  It is not
    // registered, so repeated calls cannot collide in the global registry.
    static const std::shared_ptr<spdlog::logger> fallback =
        std::make_shared<spdlog::logger>(
            "rinex_nav", std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    return fallback;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Parse one RINEX/Fortran floating field: tolerates 'D'/'d' exponent markers and
// the Fortran habit of omitting the exponent letter entirely (e.g. "1.23-04").
// A blank/whitespace field is 0.0.
double parseRinexDouble(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return 0.0;
    std::string t;
    t.reserve(s.size() + 1);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == 'D' || c == 'd') {
            t.push_back('E');
        } else if ((c == '+' || c == '-') && i > 0) {
            // A sign after position 0 that does not follow an exponent letter
            // (e/E/d/D) marks a missing exponent letter; insert one.
            const char p = s[i - 1];
            if (p != 'e' && p != 'E' && p != 'd' && p != 'D') t.push_back('E');
            t.push_back(c);
        } else {
            t.push_back(c);
        }
    }
    try {
        return std::stod(t);
    } catch (const std::exception&) {
        return 0.0;
    }
}

// One 19-column value field at @p col (0.0 when the line is too short).
double field(const std::string& line, std::size_t col) {
    if (col >= line.size()) return 0.0;
    return parseRinexDouble(line.substr(col, kFieldW));
}

// RINEX system code letter → System (only the Keplerian legacy constellations
// are decoded; everything else, incl. GLONASS 'R' and SBAS 'S', returns Unknown
// and is skipped by the caller).
System keplerianSystem(char c) {
    switch (c) {
    case 'G': return System::GPS;
    case 'E': return System::Galileo;
    case 'C': return System::BeiDou;
    case 'J': return System::QZSS;
    default:  return System::Unknown;
    }
}

// Whitespace-tokenize a substring into doubles (for the epoch calendar).
std::vector<double> tokenize(const std::string& s) {
    std::vector<double> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) {
        try { out.push_back(std::stod(tok)); } catch (const std::exception&) { out.push_back(0.0); }
    }
    return out;
}

// Whitespace-tokenize a line into string tokens (RINEX 4 record-header parsing,
// e.g. "> EPH G07 LNAV" → {">","EPH","G07","LNAV"}).
std::vector<std::string> tokenize_str(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

// RINEX 4 EPH message types whose orbit-line layout matches the RINEX 3 legacy
// 7-line Keplerian record.  CNAV/CNV1/CNV2 (different layout) and FDMA (GLONASS)
// are excluded.
bool isLegacyKeplerMsg(const std::string& msg) {
    return msg == "LNAV" || msg == "INAV" || msg == "FNAV" ||
           msg == "D1"   || msg == "D2";
}

// Decode one Keplerian record: the epoch line (system letter in column 0) plus
// its 7 continuation lines.  Appends to @p out; returns false (record skipped)
// on a malformed block.
bool decodeKeplerRecord(const std::string& epoch,
                        const std::vector<std::string>& cont,
                        std::vector<BroadcastEphemeris>& out) {
    if (epoch.size() < 3 || cont.size() < 7) return false;
    const System sys = keplerianSystem(epoch[0]);
    if (sys == System::Unknown) return false;

    // Epoch calendar (constellation-native time scale: GPS for G/J, GST≈GPS for
    // E, BDT for C); columns [3,23) hold "YYYY MM DD hh mm ss".
    const std::vector<double> cal = tokenize(epoch.substr(3, 20));
    if (cal.size() < 6) return false;
    const int    yy = static_cast<int>(cal[0]);
    const int    mo = static_cast<int>(cal[1]);
    const int    dd = static_cast<int>(cal[2]);
    const int    hh = static_cast<int>(cal[3]);
    const int    mi = static_cast<int>(cal[4]);
    const double ss = cal[5];

    // Clock polynomial (epoch line, three trailing D19.12 fields).
    const double af0 = field(epoch, 23);
    const double af1 = field(epoch, 42);
    const double af2 = field(epoch, 61);

    // Orbit lines (4 fields each at columns 4/23/42/61).
    auto orb = [&](std::size_t line, std::size_t idx) {
        return field(cont[line], 4 + idx * kFieldW);
    };
    // Broadcast orbit 1: IODE, Crs, Δn, M0
    const double iode_d = orb(0, 0);
    const double crs    = orb(0, 1);
    const double dn     = orb(0, 2);
    const double m0     = orb(0, 3);
    // Broadcast orbit 2: Cuc, e, Cus, sqrtA
    const double cuc    = orb(1, 0);
    const double e      = orb(1, 1);
    const double cus    = orb(1, 2);
    const double sqrtA  = orb(1, 3);
    // Broadcast orbit 3: Toe, Cic, Ω0, Cis
    const double toe_s  = orb(2, 0);
    const double cic    = orb(2, 1);
    const double om0    = orb(2, 2);
    const double cis    = orb(2, 3);
    // Broadcast orbit 4: i0, Crc, ω, Ωdot
    const double i0     = orb(3, 0);
    const double crc    = orb(3, 1);
    const double w      = orb(3, 2);
    const double omdot  = orb(3, 3);
    // Broadcast orbit 5: IDOT, codes/dataSrc, Week, flag
    const double idot   = orb(4, 0);
    const double src_d  = orb(4, 1);   // Galileo data sources; GPS: codes on L2 (card OWD)
    const double week_d = orb(4, 2);
    // Broadcast orbit 6: accuracy, health, TGD/BGD-a, TGD2/BGD-b
    const double health = orb(5, 1);
    const double tgd0   = orb(5, 2);
    const double tgd1   = orb(5, 3);

    if (sqrtA <= 0.0) return false;   // uninitialised / non-orbit record

    BroadcastEphemeris eph;
    // Normalize the blank-padded identifier ("G 7" to "G07"), as the RINEX 3
    // observation path does.  An ephemeris keyed "G 7" matches no observation
    // and no product record, and the satellite would drop out of the solution
    // with no warning (card RXP).
    eph.satellite = epoch.substr(0, 3);
    if (eph.satellite[1] == ' ') eph.satellite[1] = '0';
    eph.system    = sys;
    eph.iode      = static_cast<int>(std::lround(iode_d));
    eph.health    = static_cast<int>(std::lround(health));
    // The Galileo data-source word says which message the record is (I/NAV or
    // F/NAV) and which clock and group-delay pair it carries; the network's
    // group-delay latch prefers the F/NAV (E1, E5a) records.  Other systems keep
    // -1.  (Card OWD, O3.)
    if (sys == System::Galileo) eph.data_source = static_cast<int>(std::lround(src_d));
    eph.received_time = GnssTime{0, 0.0};   // file load: no wall-clock receipt stamp

    KeplerianEphemeris& k = eph.kepler;
    k.sqrt_a = sqrtA; k.e = e; k.i0 = i0; k.idot = idot;
    k.omega0 = om0; k.omega_dot = omdot; k.omega = w; k.m0 = m0; k.delta_n = dn;
    k.crs = crs; k.crc = crc; k.cus = cus; k.cuc = cuc; k.cis = cis; k.cic = cic;
    k.af0 = af0; k.af1 = af1; k.af2 = af2;
    k.tgd0 = tgd0;
    // tgd1 is the second group delay (GAL BGD E1-E5b / BDS TGD2).  GPS/QZSS carry
    // one group delay (the fourth field of broadcast orbit 6 is IODC there), so
    // tgd1 stays 0 for them.
    if (sys == System::Galileo || sys == System::BeiDou) k.tgd1 = tgd1;

    // Week: the RINEX field carries the constellation's operative week numbering
    // (GPS week for G/J; for E the GPS-aligned week, since RINEX stores the
    // Galileo week continuous with GPS; native BDT week for C), which is what the
    // RTCM decoders store.  Seconds-of-week are Sunday-relative on every scale,
    // so gregorianGpsToGpsTime (no leap correction) gives the toc SOW for every
    // system; it is paired with the record's week, matching the decoders'
    // single-week toe/toc.  The toe SOW is the Toe field.
    const int week = static_cast<int>(std::lround(week_d));
    double toc_sow = toe_s;   // fallback: broadcast toc==toe when the calendar is unusable
    try {
        toc_sow = TimeUtils::gregorianGpsToGpsTime(yy, mo, dd, hh, mi, ss).tow;
    } catch (const std::exception&) {
        // keep the toe-based fallback
    }
    k.toe = GnssTime{week, toe_s};
    k.toc = GnssTime{week, toc_sow};

    out.push_back(std::move(eph));
    return true;
}

// Collect continuation lines (leading-space, non-'>' ) starting at @p i,
// advancing @p i past them.
std::vector<std::string> collectContinuations(const std::vector<std::string>& lines,
                                              std::size_t& i) {
    std::vector<std::string> cont;
    while (i < lines.size() && !lines[i].empty() &&
           lines[i][0] == ' ') {
        cont.push_back(lines[i]);
        ++i;
    }
    return cont;
}

std::vector<BroadcastEphemeris> parseLines(std::vector<std::string> lines,
                                           const std::string& label) {
    std::vector<BroadcastEphemeris> out;

    // --- Header: RINEX version + END OF HEADER ---
    double version = 0.0;
    std::size_t data_start = std::string::npos;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.find("RINEX VERSION / TYPE") != std::string::npos) {
            const auto toks = tokenize(ln.substr(0, 20));
            if (!toks.empty()) version = toks[0];
        }
        if (ln.find("END OF HEADER") != std::string::npos) {
            data_start = i + 1;
            break;
        }
    }
    if (data_start == std::string::npos) {
        navLog()->warn("RinexNavParser: no 'END OF HEADER' in {} — not a RINEX nav file?", label);
        return out;
    }
    if (version < 3.0) {
        navLog()->warn("RinexNavParser: RINEX version {:.2f} in {} unsupported (need 3 or 4); "
                       "RINEX-2 nav has a different epoch format.", version, label);
        return out;
    }
    const bool v4 = version >= 4.0;

    // --- Records ---
    std::size_t i = data_start;
    while (i < lines.size()) {
        const std::string& ln = lines[i];
        if (trim(ln).empty()) { ++i; continue; }

        if (v4 && ln[0] == '>') {
            // RINEX 4 record header: "> EPH G07 LNAV" (STO/EOP/ION records are
            // skipped).  A space-less ">EPH ..." from a non-conformant writer is
            // normalised to "> EPH ..." before tokenising; otherwise rectype would
            // be read as the satellite and the EPH record dropped without a log line.
            std::string hdr = ln;
            if (hdr.size() > 1 && hdr[1] != ' ') hdr.insert(1, " ");
            const auto toks = tokenize_str(hdr);
            ++i;                                   // move to the body
            if (i >= lines.size() || lines[i].empty() || lines[i][0] == '>')
                continue;                          // empty/degenerate record
            const std::string epoch = lines[i];
            ++i;
            const std::vector<std::string> cont = collectContinuations(lines, i);
            const std::string rectype = toks.size() > 1 ? toks[1] : std::string{};
            const std::string msgtype = toks.size() > 3 ? toks[3] : std::string{};
            if (rectype == "EPH" && isLegacyKeplerMsg(msgtype))
                decodeKeplerRecord(epoch, cont, out);
            // Otherwise (STO/EOP/ION, CNAV, or GLONASS FDMA) the record is skipped.
            continue;
        }

        // RINEX 3 record (or a stray line in a RINEX 4 file): the epoch line
        // leads with the system letter; continuation lines follow, indented by
        // a leading space.
        const std::string epoch = ln;
        ++i;
        const std::vector<std::string> cont = collectContinuations(lines, i);
        decodeKeplerRecord(epoch, cont, out);   // skips R/S/short blocks without logging
    }
    return out;
}

} // namespace

std::vector<BroadcastEphemeris> RinexNavParser::parse(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        navLog()->warn("RinexNavParser: cannot open '{}'", path);
        return {};
    }
    std::vector<std::string> lines;
    std::string ln;
    while (std::getline(in, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();  // CRLF tolerance
        lines.push_back(std::move(ln));
    }
    return parseLines(std::move(lines), path);
}

std::vector<BroadcastEphemeris> RinexNavParser::parseString(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream is(content);
    std::string ln;
    while (std::getline(is, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(std::move(ln));
    }
    return parseLines(std::move(lines), "<string>");
}

std::size_t RinexNavParser::loadIntoLiveStore(LiveEphemerisStore& store, const std::string& path) {
    const std::vector<BroadcastEphemeris> ephs = parse(path);
    for (const auto& e : ephs) store.insert(e, /*from_igs=*/true);
    return ephs.size();
}

} // namespace GnssProcessor
