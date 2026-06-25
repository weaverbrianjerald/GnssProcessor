/**
 * @file RinexObsWriter.cpp
 * @brief Implementation of the RINEX-3.04 observation writer.
 *
 * Column positions below are validated against the parser in
 * src/core/rinex/RinexParser.cpp (RinexParser::parse):
 *   - version  : std::stod(line.substr(0, 9))                         [col 0-8]
 *   - label    : line.substr(60)                                      [col 60+]
 *   - obs types: system @ col 0, count @ stoi(substr(3,3)), codes from substr(6,54)
 *   - approx   : three std::stod(substr(0|14|28, 14))                 [F14.4]
 *   - 1st obs  : 5x I6 + F13.7 second, time system @ substr(48,3)
 *   - epoch    : '>' then whitespace-tokenized fields, flag <= 1
 *   - sat line : substr(0,3) id, field j @ offset 3 + j*16 (F14.3 + LLI + SNR)
 */

#include "sim/RinexObsWriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GnssProcessor {
namespace sim {

namespace {

/// A 60-character header content buffer; fields are written at fixed columns
/// and the record label is appended after column 60 by the emit helper.
struct Line60 {
    std::string b = std::string(60, ' ');
    void put(std::size_t col, const std::string& s) {
        for (std::size_t i = 0; i < s.size() && (col + i) < 60; ++i) b[col + i] = s[i];
    }
};

/// Format a floating value into a fixed width with a fixed precision (Fw.p).
std::string fmtF(double v, int width, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%*.*f", width, prec, v);
    return std::string(buf);
}

/// Format an integer right-justified into a fixed width (Iw).
std::string fmtI(long v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%*ld", width, v);
    return std::string(buf);
}

} // namespace

bool RinexObsWriter::open(const std::filesystem::path& path, const ObsHeaderSpec& hdr, std::string* err) {
    const std::string fn = path.filename().string();
    if (fn.find("BRDC") != std::string::npos || fn.find("_MN.") != std::string::npos) {
        if (err) *err = "observation filename must not contain 'BRDC' or '_MN.' (RinexParser would route it to the navigation path)";
        return false;
    }
    for (const auto& sys_codes : hdr.obs_types) {
        if (sys_codes.second.empty()) {
            if (err) *err = std::string("system '") + sys_codes.first + "' has no observation codes";
            return false;
        }
        if (sys_codes.second.size() > 13) {
            if (err) *err = std::string("system '") + sys_codes.first + "' declares more than 13 codes; continuation lines are not yet supported";
            return false;
        }
    }
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    out_.open(path, std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        if (err) *err = "could not open output file: " + path.string();
        return false;
    }
    hdr_ = hdr;
    writeHeader();
    return true;
}

void RinexObsWriter::writeHeader() {
    auto emit = [&](const Line60& l, const char* label) { out_ << l.b << label << "\n"; };

    { // RINEX VERSION / TYPE — version F9.2 @ col 0; "OBSERVATION DATA" @ 20; system 'M' @ 40
        Line60 l;
        l.put(0, fmtF(hdr_.version, 9, 2));
        l.put(20, "OBSERVATION DATA");
        l.put(40, "M");
        emit(l, "RINEX VERSION / TYPE");
    }
    {
        Line60 l;
        l.put(0, hdr_.pgm.substr(0, 20));
        l.put(20, hdr_.run_by.substr(0, 20));
        l.put(40, hdr_.run_date.substr(0, 20));
        emit(l, "PGM / RUN BY / DATE");
    }
    {
        Line60 l;
        l.put(0, hdr_.marker_name);
        emit(l, "MARKER NAME");
    }
    if (!hdr_.marker_number.empty()) {
        Line60 l;
        l.put(0, hdr_.marker_number);
        emit(l, "MARKER NUMBER");
    }
    {
        Line60 l;
        l.put(0, hdr_.marker_type);
        emit(l, "MARKER TYPE");
    }
    {
        Line60 l;
        l.put(0, hdr_.observer.substr(0, 20));
        l.put(20, hdr_.agency);
        emit(l, "OBSERVER / AGENCY");
    }
    {
        Line60 l;
        l.put(0, hdr_.rec_number.substr(0, 20));
        l.put(20, hdr_.rec_type.substr(0, 20));
        l.put(40, hdr_.rec_version.substr(0, 20));
        emit(l, "REC # / TYPE / VERS");
    }
    {
        Line60 l;
        l.put(0, hdr_.ant_number.substr(0, 20));
        l.put(20, hdr_.ant_type.substr(0, 20));
        emit(l, "ANT # / TYPE");
    }
    {
        Line60 l;
        l.put(0, fmtF(hdr_.approx_xyz[0], 14, 4));
        l.put(14, fmtF(hdr_.approx_xyz[1], 14, 4));
        l.put(28, fmtF(hdr_.approx_xyz[2], 14, 4));
        emit(l, "APPROX POSITION XYZ");
    }
    {
        Line60 l;
        l.put(0, fmtF(hdr_.antenna_hen[0], 14, 4));
        l.put(14, fmtF(hdr_.antenna_hen[1], 14, 4));
        l.put(28, fmtF(hdr_.antenna_hen[2], 14, 4));
        emit(l, "ANTENNA: DELTA H/E/N");
    }
    for (const auto& sys_codes : hdr_.obs_types) { // SYS / # / OBS TYPES
        Line60 l;
        l.put(0, std::string(1, sys_codes.first));
        l.put(3, fmtI(static_cast<long>(sys_codes.second.size()), 3));
        std::string codes;
        for (const auto& c : sys_codes.second) codes += " " + c; // " C1C L1C C2W L2W", tokenized from col 6
        l.put(6, codes);
        emit(l, "SYS / # / OBS TYPES");
    }
    {
        Line60 l;
        l.put(0, fmtF(hdr_.interval_s, 10, 3));
        emit(l, "INTERVAL");
    }
    { // TIME OF FIRST OBS — GPS-scale calendar so HH:MM:SS lands on the TOW grid
        int y, m, d, h, mi;
        double s;
        calendarFields(hdr_.time_of_first_obs, y, m, d, h, mi, s);
        Line60 l;
        l.put(0, fmtI(y, 6));
        l.put(6, fmtI(m, 6));
        l.put(12, fmtI(d, 6));
        l.put(18, fmtI(h, 6));
        l.put(24, fmtI(mi, 6));
        l.put(30, fmtF(s, 13, 7));
        l.put(48, hdr_.time_system.substr(0, 3));
        emit(l, "TIME OF FIRST OBS");
    }
    {
        Line60 l;
        emit(l, "END OF HEADER");
    }
}

void RinexObsWriter::calendarFields(const GnssTime& t, int& y, int& m, int& d,
                                    int& h, int& mi, double& sec) const {
    int hh, mm;
    double ss;
    TimeUtils::gpsTimeToGpsScaleCalendar(t, y, m, d, hh, mm, ss);
    // Snap seconds-of-day to the nearest millisecond, then recompose so a true
    // minute/hour boundary that the lossy conversion rendered as HH:MM:59.99999x
    // rolls up cleanly rather than printing a spurious sub-millisecond tag.
    long ms = std::lround((hh * 3600.0 + mm * 60.0 + ss) * 1000.0);
    if (ms < 0) ms = 0;
    h = static_cast<int>(ms / 3600000);
    ms %= 3600000;
    mi = static_cast<int>(ms / 60000);
    ms %= 60000;
    sec = static_cast<double>(ms) / 1000.0;
}

const std::vector<std::string>* RinexObsWriter::codesForSystem(char sys_letter) const {
    for (const auto& sys_codes : hdr_.obs_types) {
        if (sys_codes.first == sys_letter) return &sys_codes.second;
    }
    return nullptr;
}

void RinexObsWriter::writeEpoch(const GnssTime& t, const std::vector<SatObs>& sats, int epoch_flag) {
    if (!out_.is_open()) return;

    // Only satellites carrying at least one observation are written; the epoch
    // count must equal the number of satellite lines that follow.
    std::vector<const SatObs*> present;
    present.reserve(sats.size());
    for (const auto& s : sats) {
        if (!s.values.empty()) present.push_back(&s);
    }

    int y, m, d, h, mi;
    double sec;
    calendarFields(t, y, m, d, h, mi, sec);

    char eb[80];
    std::snprintf(eb, sizeof(eb), "> %04d %02d %02d %02d %02d%11.7f  %1d%3d",
                  y, m, d, h, mi, sec, epoch_flag, static_cast<int>(present.size()));
    out_ << eb << "\n";

    for (const SatObs* sp : present) {
        const SatObs& so = *sp;
        std::string line = so.sat_id;
        const std::vector<std::string>* codes =
            codesForSystem(so.sat_id.empty() ? '?' : so.sat_id[0]);
        if (codes) {
            int last_present = -1;
            for (std::size_t j = 0; j < codes->size(); ++j) {
                if (so.values.count((*codes)[j])) last_present = static_cast<int>(j);
            }
            for (int j = 0; j <= last_present; ++j) {
                const std::string& code = (*codes)[static_cast<std::size_t>(j)];
                auto it = so.values.find(code);
                if (it == so.values.end()) {
                    line += std::string(16, ' '); // interior missing observation
                    continue;
                }
                std::string value = fmtF(it->second, 14, 3);
                if (value.size() > 14) value = std::string(14, '*'); // width guard (should not happen for Earth-surface ranges)

                char lli = ' ';
                auto li = so.lli.find(code);
                if (li != so.lli.end() && li->second > 0 && li->second <= 9) {
                    lli = static_cast<char>('0' + li->second);
                }
                // Non-blank signal-strength digit keeps the final field alive
                // through the parser's trailing-whitespace trim.
                char snr = static_cast<char>('0' + std::min(9, std::max(1, hdr_.default_snr_flag)));
                line += value;
                line += lli;
                line += snr;
            }
        }
        out_ << line << "\n";
    }
}

void RinexObsWriter::close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

} // namespace sim
} // namespace GnssProcessor
