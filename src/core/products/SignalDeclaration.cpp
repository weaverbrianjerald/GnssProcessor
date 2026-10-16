/**
 * @file SignalDeclaration.cpp
 * @brief Implementation of the declared signal roster and its wire form (the `N`
 *        record).
 *
 * Pure functions: no logging, no I/O, no config.  The writer and the reader
 * share these functions so the two cannot diverge.
 */

#include "products/SignalDeclaration.hpp"

#include <algorithm>

namespace GnssProcessor {

namespace {

/// Join a list on '+', or an empty string when the list is empty.
std::string joinPlus(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += '+';
        out += s;
    }
    return out;
}

/// Split a '+'-joined token value into its entries, dropping empties.
std::vector<std::string> splitPlus(const std::string& v) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= v.size()) {
        const std::size_t plus = v.find('+', start);
        const std::string piece = (plus == std::string::npos)
                                      ? v.substr(start)
                                      : v.substr(start, plus - start);
        if (!piece.empty()) out.push_back(piece);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return out;
}

bool listHas(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

const SignalDeclarationBand* NetworkSignalDeclaration::find(const std::string& cons,
                                                            char band) const {
    const auto cit = bands.find(cons);
    if (cit == bands.end()) return nullptr;
    const auto bit = cit->second.find(band);
    return bit == cit->second.end() ? nullptr : &bit->second;
}

bool NetworkSignalDeclaration::declaresPhase(const std::string& cons,
                                             const std::string& signal) const {
    const auto cit = bands.find(cons);
    if (cit == bands.end()) return false;
    for (const auto& [band, row] : cit->second) {
        (void)band;
        if (listHas(row.phase, signal)) return true;
    }
    return false;
}

bool NetworkSignalDeclaration::declaresCode(const std::string& cons,
                                            const std::string& signal) const {
    const auto cit = bands.find(cons);
    if (cit == bands.end()) return false;
    for (const auto& [band, row] : cit->second) {
        (void)band;
        // A companion is a family the network emits a code bias for, so
        // it answers "does the network carry this code" the same way a modelled
        // code does.  The roles stay separate in the record so a reader that needs
        // the distinction still has it.
        if (listHas(row.code, signal) || listHas(row.companion, signal)) return true;
    }
    return false;
}

std::vector<std::string> signalDeclarationToWire(const NetworkSignalDeclaration& decl) {
    std::vector<std::string> out;
    for (const auto& [cons, per_band] : decl.bands) {
        if (cons.empty()) continue;
        for (const auto& [band, row] : per_band) {
            if (row.empty()) continue;
            std::string line = "N,";
            line += cons;
            line += ',';
            line += band;
            if (!row.phase.empty())     line += ",ph=" + joinPlus(row.phase);
            if (!row.code.empty())      line += ",co=" + joinPlus(row.code);
            if (!row.companion.empty()) line += ",comp=" + joinPlus(row.companion);
            if (!row.datum_phase.empty()) line += ",dph=" + row.datum_phase;
            if (!row.datum_code.empty())  line += ",dco=" + row.datum_code;
            out.push_back(std::move(line));
        }
    }
    return out;
}

bool signalDeclarationParseRecord(const std::vector<std::string>& f,
                                  NetworkSignalDeclaration& out) {
    if (f.size() < 3) return false;
    if (f[1].empty() || f[2].size() != 1) return false;

    SignalDeclarationBand row;
    for (std::size_t k = 3; k < f.size(); ++k) {
        if (f[k].rfind("ph=", 0) == 0)        row.phase     = splitPlus(f[k].substr(3));
        else if (f[k].rfind("co=", 0) == 0)   row.code      = splitPlus(f[k].substr(3));
        else if (f[k].rfind("comp=", 0) == 0) row.companion = splitPlus(f[k].substr(5));
        else if (f[k].rfind("dph=", 0) == 0)  row.datum_phase = f[k].substr(4);
        else if (f[k].rfind("dco=", 0) == 0)  row.datum_code  = f[k].substr(4);
        // Unknown key=value tokens are ignored (forward-compatible), as the
        // C / B / P / S records treat theirs.
    }
    out.present            = true;
    out.bands[f[1]][f[2][0]] = std::move(row);
    return true;
}

const char* flipAttributionName(FlipAttribution a) {
    switch (a) {
    case FlipAttribution::NoDeclaration: return "no_declaration";
    case FlipAttribution::Undeclared:    return "undeclared";
    case FlipAttribution::OffWire:       return "off_wire";
    case FlipAttribution::Stale:         return "stale";
    }
    return "?";
}

FlipAttribution signalDeclarationAttributeFlip(const NetworkSignalDeclaration& decl,
                                               const std::string& cons,
                                               const std::string& signal,
                                               bool is_phase, bool on_wire) {
    if (!decl.present) return FlipAttribution::NoDeclaration;
    const bool declared = is_phase ? decl.declaresPhase(cons, signal)
                                   : decl.declaresCode(cons, signal);
    if (!declared) return FlipAttribution::Undeclared;
    return on_wire ? FlipAttribution::Stale : FlipAttribution::OffWire;
}

std::string signalDeclarationSummary(const NetworkSignalDeclaration& decl) {
    if (!decl.present) return "(absent)";
    std::string s;
    for (const auto& [cons, per_band] : decl.bands) {
        for (const auto& [band, row] : per_band) {
            if (!s.empty()) s += "; ";
            s += cons;
            s += ' ';
            s += band;
            s += " ph[" + joinPlus(row.phase) + "] co[" + joinPlus(row.code) + "]";
            if (!row.companion.empty()) s += " comp[" + joinPlus(row.companion) + "]";
            if (!row.datum_code.empty()) s += " dco=" + row.datum_code;
        }
    }
    return s.empty() ? std::string("(empty)") : s;
}

}  // namespace GnssProcessor
