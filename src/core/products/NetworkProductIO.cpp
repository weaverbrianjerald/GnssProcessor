/**
 * @file NetworkProductIO.cpp
 * @brief Implementation of the PPP-RTK network product file writer / reader.
 */

#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <map>
#include <stdexcept>

namespace GnssProcessor {

namespace {

/// Split @p line on ',' into raw fields (no trimming; RINEX obs codes and sat
/// IDs never contain commas, so this is lossless for our format).
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string::npos) { out.push_back(line.substr(start)); break; }
        out.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

} // namespace

// ===========================================================================
// NetworkProductWriter
// ===========================================================================

NetworkProductWriter::NetworkProductWriter(const std::string& path)
    : path_(path) {
    // Create the parent directory if it does not exist, so a relative path like
    // "logs/network_products.txt" does not fail the open just because the dir is
    // absent (ofstream does not create directories).
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    out_.open(path, std::ios::out | std::ios::trunc);
    if (out_) {
        out_ << "# gnss_network_product v1\n"
                "# E,week,tow,iod_ssr,provider_id,solution_id,datum_changed\n"
                "# C,sat,clock_c0_m\n"
                "# B,sat,signal,code_bias_m\n"
                "# P,sat,signal,bias_m,signal_integer,wide_lane_integer\n";
    }
}

void NetworkProductWriter::write(const NetworkSolutionEpoch& sol, bool datum_changed) {
    if (!out_) return;

    out_ << "E," << sol.epoch_time.week << ',' << fmt::format("{}", sol.epoch_time.tow)
         << ',' << sol.iod_ssr << ',' << sol.provider_id << ',' << sol.solution_id
         << ',' << (datum_changed ? 1 : 0) << '\n';

    for (const auto& s : sol.sats) {
        if (s.has_clock)
            out_ << "C," << s.sat << ',' << fmt::format("{}", s.clock_c0_m) << '\n';
        for (const auto& [signal, bias_m] : s.code_bias_m)
            out_ << "B," << s.sat << ',' << signal << ',' << fmt::format("{}", bias_m) << '\n';
        for (const auto& [signal, pb] : s.phase_bias)
            out_ << "P," << s.sat << ',' << signal << ',' << fmt::format("{}", pb.bias_m)
                 << ',' << (pb.signal_integer ? 1 : 0) << ',' << pb.wide_lane_integer << '\n';
    }
}

// ===========================================================================
// NetworkProductReader
// ===========================================================================

std::vector<NetworkProductRecord> NetworkProductReader::read(const std::string& path,
                                                             bool* ok) {
    std::vector<NetworkProductRecord> out;
    std::ifstream in(path);
    if (!in) { if (ok) *ok = false; return out; }
    if (ok) *ok = true;

    bool                              have_cur = false;
    NetworkProductRecord              cur;
    std::map<std::string, std::size_t> sat_index;   // sat -> index into cur.epoch.sats

    auto flush = [&]() {
        if (have_cur) out.push_back(std::move(cur));
        cur = NetworkProductRecord{};
        sat_index.clear();
        have_cur = false;
    };

    // Find-or-create the per-satellite solution within the current epoch.  The
    // returned reference is used immediately by the caller before any further
    // push_back, so reallocation cannot invalidate it across uses.
    auto satSolution = [&](const std::string& sat) -> NetworkSatSolution& {
        const auto it = sat_index.find(sat);
        if (it != sat_index.end()) return cur.epoch.sats[it->second];
        sat_index[sat] = cur.epoch.sats.size();
        NetworkSatSolution s;
        s.sat = sat;
        cur.epoch.sats.push_back(std::move(s));
        return cur.epoch.sats.back();
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        if (line.empty() || line[0] == '#') continue;

        const std::vector<std::string> f = splitCsv(line);
        if (f.empty() || f[0].empty()) continue;
        const std::string& tag = f[0];

        try {
            if (tag == "E") {
                if (f.size() < 6) { if (ok) *ok = false; continue; }
                flush();
                cur.epoch.epoch_time.week = std::stoi(f[1]);
                cur.epoch.epoch_time.tow  = std::stod(f[2]);
                cur.epoch.iod_ssr         = std::stoi(f[3]);
                cur.epoch.provider_id     = std::stoi(f[4]);
                cur.epoch.solution_id     = std::stoi(f[5]);
                cur.datum_changed         = (f.size() > 6) && (std::stoi(f[6]) != 0);
                have_cur = true;
            } else if (tag == "C") {
                if (!have_cur || f.size() < 3) { if (ok) *ok = false; continue; }
                NetworkSatSolution& s = satSolution(f[1]);
                s.has_clock  = true;
                s.clock_c0_m = std::stod(f[2]);
            } else if (tag == "B") {
                if (!have_cur || f.size() < 4) { if (ok) *ok = false; continue; }
                satSolution(f[1]).code_bias_m[f[2]] = std::stod(f[3]);
            } else if (tag == "P") {
                if (!have_cur || f.size() < 6) { if (ok) *ok = false; continue; }
                NetworkPhaseBias pb;
                pb.bias_m            = std::stod(f[3]);
                pb.signal_integer    = (std::stoi(f[4]) != 0);
                pb.wide_lane_integer = std::stoi(f[5]);
                satSolution(f[1]).phase_bias[f[2]] = pb;
            } else {
                if (ok) *ok = false;   // unknown tag — flag but keep going
            }
        } catch (const std::exception&) {
            if (ok) *ok = false;       // malformed numeric field — flag, skip line
        }
    }
    flush();
    return out;
}

void NetworkProductReader::replay(const NetworkProductRecord& rec,
                                  SsrCorrectionStore& store,
                                  NetworkProductEmitter& emitter) {
    if (rec.datum_changed) emitter.markDatumChange();
    emitter.emitToStore(rec.epoch, store);
}

} // namespace GnssProcessor
