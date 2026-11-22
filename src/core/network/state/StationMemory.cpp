/**
 * @file StationMemory.cpp
 * @brief The station memory section of the snapshot: its fingerprint, its writer and its restore
 *        (network.state_snapshot_station_memory_capture, network.resume_station_memory_restore).  The format is
 *        described in the header.
 */

#include "network/state/StationMemory.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "model/PppModel.hpp"
#include "network/driver/Run.hpp"
#include "network/slip/GfArcJump.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkStateSnapshot.hpp"   // NetworkStateSnapshotIO::fnv1a64
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace GnssProcessor {

namespace {

/// The section's format version, the second field of its header line.
constexpr const char* kStationMemoryVersion = "v2";

/// The fields of one comma-separated line (empty fields kept).
std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (const char c : line) {
        if (c == ',') {
            f.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(cur);
    return f;
}

/// A double read from the whole of @p s, or false: the exact inverse of the writer's shortest form.
bool parseExact(const std::string& s, double& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last && std::isfinite(out);
}

/// The section fingerprint as the header writes it: 16 lower-case hexadecimal digits.
std::string fingerprintText(std::uint64_t fp) { return fmt::format("{:016x}", fp); }

}  // namespace

std::vector<std::string> networkStationMemoryFingerprintTokens(const GnssConfig& config) {
    std::vector<std::string> t;
    // The wind-up model and its posture: an accumulated angle exists only with both on.
    t.push_back(fmt::format("windup={},{}", config.getModelCorrections().enable_phase_windup,
                            config.accumulatePhaseWindUp()));
    // The continuity horizon decides which saved entries continue and which restart.
    t.push_back(fmt::format("windup_gap={}", config.phaseWindUpMaxGapS()));
    // The frame the angle is built in: the Sun-referenced frame and the noon-turn attitude.
    t.push_back(fmt::format("windup_frame={},{}", config.enableSunFrameCorrection(),
                            config.enableNoonTurnAttitude()));
    // The gap bridge, which may continue an entry across a gap past the horizon.
    t.push_back(fmt::format("windup_bridge={},{},{},{}", config.phaseWindUpGapBridge(),
                            config.phaseWindUpBridgeMaxGapS(), config.phaseWindUpBridgeStepS(),
                            config.getEclipseShadowScale()));
    // The GF arc-jump trigger: its arcs' references, windows and flag runs mean what these settings make them.
    const auto& sq = config.getNetworkConfig().slip_qc;
    t.push_back(fmt::format("gf_arc_jump={},{},{},{},{}", sq.gf_arc_jump, sq.gf_arc_jump_m, sq.gf_arc_jump_k,
                            sq.gf_arc_jump_window, sq.gf_arc_jump_min_count));
    return t;
}

std::uint64_t networkStationMemoryFingerprint(const GnssConfig& config) {
    std::string text;
    for (const std::string& token : networkStationMemoryFingerprintTokens(config)) {
        text += token;
        text += '\x1f';
    }
    return NetworkStateSnapshotIO::fnv1a64(text);
}

std::string networkStationMemoryWrite(const NetworkRun& run, long long& n_records) {
    const GnssConfig& config = run.config;
    const std::vector<NetStation>& stations = run.setup.stations;
    const SlipQcChannel& qc = run.engines.slip_qc;
    // ---- End of the bindings ----
    n_records = 0;
    std::ostringstream os;
    os << "STMEM," << kStationMemoryVersion << ',' << fingerprintText(networkStationMemoryFingerprint(config))
       << '\n';
    for (const NetStation& st : stations) {
        if (!st.model) continue;
        for (const GnssCorrections::WindUpRecord& r : st.model->windUpRecords()) {
            os << fmt::format("WU,{},{},{},{},{}\n", st.name, r.sat, r.angle_rad, r.epoch_s, r.start_epoch_s);
            ++n_records;
        }
    }
    // The slip channel's processed-epoch counter, so the arcs' epoch stamps below keep their meaning in the resumed
    // run (its counter continues instead of restarting at 0).
    os << fmt::format("SQE,{}\n", qc.epoch_idx);
    // The GF arc-jump trigger's arcs (network.cycle_slip.gf_arc_jump), by station and satellite: the last sample, the
    // reference step, the flag run and the clean residuals of the robust scale.
    if (qc.params.gf_jump.armed) {
        for (std::size_t i = 0; i < qc.hist.size() && i < stations.size(); ++i) {
            for (const auto& [sat, a] : qc.hist[i].gf_arcs) {
                os << fmt::format("GA,{},{},{},{},{},{},{},{},{},{},{},{}", stations[i].name, sat, a.last_epoch,
                                  a.last_t_s, a.sig1, a.sig2, a.gf_m, a.has_ref ? 1 : 0, a.ref_step_m, a.ref_span_s,
                                  a.flag_run, a.clean_r.size());
                for (const double r : a.clean_r) os << fmt::format(",{}", r);
                os << '\n';
                ++n_records;
            }
        }
    }
    os << "SMEND\n";
    return os.str();
}

void networkStationMemoryRestore(NetworkRun& run) {
    const GnssConfig& config = run.config;
    std::vector<NetStation>& stations = run.setup.stations;        // written: the stations' wind-up accumulators
    SlipQcChannel& qc = run.engines.slip_qc;                        // written: the epoch counter and the GF arcs
    std::string& staged = run.ctx.carry.resume.station_memory;     // written: consumed here
    auto& counters = run.ctx.counters;                              // written: the restore's counters
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (staged.empty()) return;
    const std::string text = staged;
    staged.clear();
    // The header: the format version and the fingerprint of the keys that give the angles their meaning.
    const std::string header = text.substr(0, text.find('\n'));
    const std::vector<std::string> h = splitFields(header);
    const std::string live = fingerprintText(networkStationMemoryFingerprint(config));
    std::string refusal;
    if (h.size() != 3 || h[0] != "STMEM" || h[1] != kStationMemoryVersion)
        refusal = fmt::format("its header '{}' is not this build's ('STMEM,{},<fingerprint>')", header,
                              kStationMemoryVersion);
    else if (h[2] != live)
        refusal = fmt::format("its fingerprint {} does not match this run's {}: the keys that give the saved wind-up "
                              "angles their meaning (the wind-up posture, its continuity horizon, frame, noon-turn "
                              "attitude or gap bridge) differ", h[2], live);
    if (!refusal.empty()) {
        ++counters.station_memory_refused;
        log->warn("Network (station memory restore): the snapshot's station memory section is REFUSED: {}.  Nothing "
                  "of it is restored, and the warm resume goes on without it (every station's wind-up accumulator "
                  "starts from its first epoch's raw angle).", refusal);
        return;
    }
    ++counters.station_memory_restored;
    // Group the records by station, in the section's order.
    std::map<std::string, std::vector<GnssCorrections::WindUpRecord>> by_station;
    std::map<std::string, std::map<std::string, GfArcJumpArc>> gf_by_station;
    long long epoch_idx = -1;
    long long n_malformed = 0;
    std::string first_malformed;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line == "SMEND" || line.rfind("STMEM,", 0) == 0) continue;
        const std::vector<std::string> f = splitFields(line);
        if (!f.empty() && f[0] == "SQE") {
            double v = 0.0;
            if (f.size() == 2 && parseExact(f[1], v) && v >= 0.0) {
                epoch_idx = static_cast<long long>(v);
            } else if (n_malformed++ == 0) {
                first_malformed = line;
            }
            continue;
        }
        if (!f.empty() && f[0] == "GA") {
            GfArcJumpArc a;
            double last_epoch = 0.0, has_ref = 0.0, flag_run = 0.0, n = -1.0;
            bool ok = f.size() >= 13 && !f[1].empty() && !f[2].empty() && parseExact(f[3], last_epoch)
                   && parseExact(f[4], a.last_t_s) && parseExact(f[7], a.gf_m) && parseExact(f[8], has_ref)
                   && parseExact(f[9], a.ref_step_m) && parseExact(f[10], a.ref_span_s)
                   && parseExact(f[11], flag_run) && parseExact(f[12], n) && n >= 0.0
                   && f.size() == 13 + static_cast<std::size_t>(n);
            for (std::size_t k = 13; ok && k < f.size(); ++k) {
                double r = 0.0;
                ok = parseExact(f[k], r);
                a.clean_r.push_back(r);
            }
            if (!ok) {
                if (n_malformed++ == 0) first_malformed = line;
                continue;
            }
            a.last_epoch = static_cast<long long>(last_epoch);
            a.sig1 = f[5];
            a.sig2 = f[6];
            a.has_ref = has_ref != 0.0;
            a.flag_run = static_cast<int>(flag_run);
            gf_by_station[f[1]][f[2]] = std::move(a);
            continue;
        }
        GnssCorrections::WindUpRecord r;
        const bool ok = f.size() == 6 && f[0] == "WU" && !f[1].empty() && !f[2].empty()
                     && parseExact(f[3], r.angle_rad) && parseExact(f[4], r.epoch_s)
                     && parseExact(f[5], r.start_epoch_s);
        if (!ok) {
            if (n_malformed++ == 0) first_malformed = line;
            continue;
        }
        r.sat = f[2];
        by_station[f[1]].push_back(r);
    }
    // Put each station's records back into its model.
    long long n_restored = 0, n_unknown = 0, n_fractional = 0, n_stations = 0;
    std::string first_unknown;
    std::map<std::string, NetStation*> by_name;
    for (NetStation& st : stations)
        if (st.model) by_name[st.name] = &st;
    for (const auto& [name, records] : by_station) {
        const auto it = by_name.find(name);
        if (it == by_name.end()) {
            n_unknown += static_cast<long long>(records.size());
            if (first_unknown.empty()) first_unknown = name;
            continue;
        }
        if (!it->second->model->restoreWindUpRecords(records)) {
            n_fractional += static_cast<long long>(records.size());
            continue;
        }
        n_restored += static_cast<long long>(records.size());
        ++n_stations;
    }
    // The slip channel's counter continues the writer's, and each station's GF arcs are put back by name.
    long long n_gf = 0, n_gf_unknown = 0;
    if (epoch_idx >= 0) qc.epoch_idx = epoch_idx;
    if (!gf_by_station.empty()) {
        if (qc.hist.size() < stations.size()) qc.hist.resize(stations.size());
        std::map<std::string, std::size_t> index_of;
        for (std::size_t i = 0; i < stations.size(); ++i) index_of[stations[i].name] = i;
        for (auto& [name, arcs] : gf_by_station) {
            const auto it = index_of.find(name);
            if (it == index_of.end()) {
                n_gf_unknown += static_cast<long long>(arcs.size());
                continue;
            }
            n_gf += static_cast<long long>(arcs.size());
            qc.hist[it->second].gf_arcs = std::move(arcs);
        }
    }
    counters.station_memory_gf_arcs_restored += n_gf;
    n_unknown += n_gf_unknown;
    counters.station_memory_records_restored += n_restored;
    counters.station_memory_refused_station += n_unknown;
    counters.station_memory_refused_malformed += n_malformed;
    counters.station_memory_refused_fractional += n_fractional;
    log->info("Network (station memory restore): the station memory section restored at the resume (fingerprint "
              "{}): {} wind-up accumulator(s) at {} station(s) continue the writer's unwrapped angles; {} GF arc-jump "
              "arc(s) restored; the slip channel's epoch counter {}; refused: {} for a station not in this run, {} "
              "malformed, {} at a station without an accumulator.",
              live, n_restored, n_stations, n_gf,
              epoch_idx >= 0 ? fmt::format("continues at {}", epoch_idx) : std::string("not in the section"),
              n_unknown, n_malformed, n_fractional);
    if (n_unknown > 0)
        log->warn("Network (station memory restore): {} record(s) name a station this run does not hold (the first: "
                  "'{}'); those stations' accumulators start from their first epoch.", n_unknown, first_unknown);
    if (n_malformed > 0)
        log->warn("Network (station memory restore): {} line(s) of the section are malformed and not read (the "
                  "first: '{}').", n_malformed, first_malformed);
}

}  // namespace GnssProcessor
