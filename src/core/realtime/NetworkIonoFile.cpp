#include "realtime/NetworkIonoFile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <tuple>

#include <fmt/format.h>

namespace GnssProcessor {
namespace {

constexpr double kWeekSeconds = 604800.0;

/// Split one CSV line (the network's traces carry no quoted fields) and drop a trailing carriage return.
void splitCsv(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(',', start);
        if (pos == std::string::npos) {
            out.emplace_back(line.substr(start));
            break;
        }
        out.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    if (!out.back().empty() && out.back().back() == '\r') out.back().pop_back();
}

/// A trace opened at its header (comment lines starting with '#' are skipped), with the index of each column.
struct CsvTrace {
    std::ifstream in;
    std::map<std::string, std::size_t> col;
    std::size_t need_width{ 0 };   ///< One past the largest needed column index.

    bool open(const std::string& path, const std::vector<std::string>& needed, std::string& error) {
        in.open(path);
        if (!in) {
            error = "cannot open " + path;
            return false;
        }
        std::string line;
        std::vector<std::string> fields;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            splitCsv(line, fields);
            for (std::size_t i = 0; i < fields.size(); ++i) col[fields[i]] = i;
            break;
        }
        for (const auto& name : needed) {
            const auto it = col.find(name);
            if (it == col.end()) {
                error = path + " has no column '" + name + "'";
                return false;
            }
            need_width = std::max(need_width, it->second + 1);
        }
        return true;
    }
};

/// True when the line mentions one of the receivers: a cheap filter before the split.
bool mentionsAny(const std::string& line, const std::vector<std::string>& stations) {
    for (const auto& s : stations)
        if (line.find(s) != std::string::npos) return true;
    return false;
}

/// Parse a number; @p ok is false for an empty or malformed field.
double toDouble(const std::string& s, bool& ok) {
    ok = false;
    if (s.empty()) return 0.0;
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        ok = used > 0 && std::isfinite(v);
        return v;
    } catch (...) {
        return 0.0;
    }
}

/// Continuous GPS seconds of a row's week and time-of-week fields.
bool rowSeconds(const std::vector<std::string>& f, std::size_t iw, std::size_t it, double& seconds) {
    bool ok_w = false, ok_t = false;
    const double week = toDouble(f[iw], ok_w);
    const double tow = toDouble(f[it], ok_t);
    seconds = week * kWeekSeconds + tow;
    return ok_w && ok_t;
}

}  // namespace

std::int64_t NetworkIonoFile::keyOf(double gps_seconds) {
    return static_cast<std::int64_t>(std::llround(gps_seconds * 10.0));
}

NetworkIonoFile NetworkIonoFile::read(const std::string& logs_dir, const std::vector<std::string>& stations,
                                      std::string& error) {
    NetworkIonoFile t;
    error.clear();
    t.stations_ = stations;
    const std::set<std::string> listed(stations.begin(), stations.end());
    using LinkKey = std::tuple<std::int64_t, std::string, std::string>;   // (epoch, receiver, satellite)
    std::set<LinkKey> integer;
    std::map<LinkKey, std::set<std::string>> pinned_bands;
    std::vector<std::string> f;
    std::string line;
    double seconds = 0.0;

    // 1. The ring fix: a held or decided row holds the link's pair at that epoch.
    {
        CsvTrace c;
        if (!c.open(logs_dir + "/network_ring_fix.csv", { "week", "tow", "station", "sat", "state" }, error))
            return {};
        const std::size_t iw = c.col["week"], it = c.col["tow"], ista = c.col["station"], isat = c.col["sat"],
                          istate = c.col["state"];
        while (std::getline(c.in, line)) {
            if (line.empty() || line[0] == '#' || !mentionsAny(line, stations)) continue;
            splitCsv(line, f);
            if (f.size() < c.need_width || !listed.count(f[ista])) continue;
            if (f[istate] != "held" && f[istate] != "decided") continue;
            if (!rowSeconds(f, iw, it, seconds)) continue;
            integer.emplace(keyOf(seconds), f[ista], f[isat]);
            ++t.counts_.ring_fix_rows;
        }
    }
    // 2. The chain: a row with a pin target pins one band of the link; two bands pinned give its integers.
    {
        CsvTrace c;
        if (!c.open(logs_dir + "/network_chain_edges.csv",
                    { "week", "tow", "band", "edge_sta", "edge_sat", "target_cyc" }, error))
            return {};
        const std::size_t iw = c.col["week"], it = c.col["tow"], iband = c.col["band"], ista = c.col["edge_sta"],
                          isat = c.col["edge_sat"], itarget = c.col["target_cyc"];
        while (std::getline(c.in, line)) {
            if (line.empty() || line[0] == '#' || !mentionsAny(line, stations)) continue;
            splitCsv(line, f);
            if (f.size() < c.need_width || !listed.count(f[ista]) || f[itarget].empty()) continue;
            if (!rowSeconds(f, iw, it, seconds)) continue;
            pinned_bands[{ keyOf(seconds), f[ista], f[isat] }].insert(f[iband]);
            ++t.counts_.pin_rows;
        }
    }
    for (const auto& [key, bands] : pinned_bands)
        if (bands.size() >= 2) integer.insert(key);
    // 3. The slant ionosphere of the listed receivers.
    {
        CsvTrace c;
        if (!c.open(logs_dir + "/network_ionosphere.csv",
                    { "week", "tow", "station", "sat", "elevation_deg", "iono_total_m" }, error))
            return {};
        const std::size_t iw = c.col["week"], it = c.col["tow"], ista = c.col["station"], isat = c.col["sat"],
                          iel = c.col["elevation_deg"], iiono = c.col["iono_total_m"];
        while (std::getline(c.in, line)) {
            if (line.empty() || line[0] == '#' || !mentionsAny(line, stations)) continue;
            splitCsv(line, f);
            if (f.size() < c.need_width || !listed.count(f[ista])) continue;
            bool ok_el = false, ok_iono = false;
            const double el = toDouble(f[iel], ok_el);
            const double iono = toDouble(f[iiono], ok_iono);
            if (!ok_el || !ok_iono || !rowSeconds(f, iw, it, seconds)) continue;
            NetworkIonoLink link;
            link.iono_m = iono;
            link.elevation_deg = el;
            link.integer = integer.count({ keyOf(seconds), f[ista], f[isat] }) > 0;
            t.addLink(seconds, f[ista], f[isat], link);
            ++t.counts_.rows;
            if (link.integer) ++t.counts_.rows_integer;
        }
    }
    // 4. The receivers' positions.
    {
        CsvTrace c;
        if (!c.open(logs_dir + "/network_stations.csv", { "station", "ecef_x", "ecef_y", "ecef_z" }, error))
            return {};
        const std::size_t ista = c.col["station"], ix = c.col["ecef_x"], iy = c.col["ecef_y"], iz = c.col["ecef_z"];
        while (std::getline(c.in, line)) {
            if (line.empty() || line[0] == '#') continue;
            splitCsv(line, f);
            if (f.size() < c.need_width || !listed.count(f[ista])) continue;
            bool okx = false, oky = false, okz = false;
            const std::array<double, 3> p{ toDouble(f[ix], okx), toDouble(f[iy], oky), toDouble(f[iz], okz) };
            if (okx && oky && okz) t.setPosition(f[ista], p);
        }
    }
    for (const auto& s : stations) {
        const bool has_rows = std::any_of(t.epochs_.begin(), t.epochs_.end(),
                                          [&](const auto& e) { return e.second.count(s) > 0; });
        if (!has_rows) {
            error = "receiver " + s + " has no row in " + logs_dir + "/network_ionosphere.csv";
            return {};
        }
    }
    t.counts_.epochs = t.epochs_.size();
    return t;
}

void NetworkIonoFile::addLink(double gps_seconds, const std::string& station, const std::string& sat,
                              const NetworkIonoLink& link) {
    epochs_[keyOf(gps_seconds)][station][sat] = link;
}

void NetworkIonoFile::setPosition(const std::string& station, const std::array<double, 3>& ecef) {
    positions_[station] = ecef;
}

const NetworkIonoEpoch* NetworkIonoFile::epochAt(double gps_seconds) const {
    const std::int64_t k = keyOf(gps_seconds);
    const auto it = epochs_.lower_bound(k - 5);
    if (it == epochs_.end() || it->first > k + 5) return nullptr;
    return &it->second;
}

bool NetworkIonoFile::position(const std::string& station, std::array<double, 3>& ecef) const {
    const auto it = positions_.find(station);
    if (it == positions_.end()) return false;
    ecef = it->second;
    return true;
}

std::string NetworkIonoFile::runStartLine() const {
    std::string names;
    for (const auto& s : stations_) names += (names.empty() ? "" : " ") + s;
    const double share = counts_.rows > 0 ? 100.0 * static_cast<double>(counts_.rows_integer)
                                                / static_cast<double>(counts_.rows)
                                          : 0.0;
    return fmt::format("Network iono file: receivers {} ({} with a position); {} epochs, {} links, {} with network "
                       "integers ({:.1f} %); ring-fix rows {}, chain pin rows {}",
                       names, positions_.size(), counts_.epochs, counts_.rows, counts_.rows_integer, share,
                       counts_.ring_fix_rows, counts_.pin_rows);
}

}  // namespace GnssProcessor
