/**
 * @file LiveRecord.cpp
 * @brief The live record (network.trace_live_record): the JSON writer of network_live.jsonl,
 *        the notes that collect an epoch's line, the setup builder that opens the stream and
 *        the observer that adds the receivers and satellites.  The format and the order of
 *        the notes are in network/diag/LiveRecord.hpp.
 */

#include "network/diag/LiveRecord.hpp"

#include "network/assembly/RowClassify.hpp"   // rowClassOfKind, rowTagAt
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"           // NetworkEpoch
#include "network/driver/Run.hpp"             // NetworkRun
#include "network/driver/RunTypes.hpp"        // NetStation, StationAssembly, EpochStatusRow
#include "network/driver/SetupResult.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/StationParams.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "config/GnssConfig.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace GnssProcessor {

namespace {

constexpr double kRadToDeg = 57.295779513082320876798;

/// Appends @p v with at most @p digits decimals and no trailing zeros, or @c null.
void appendNumber(std::string& o, double v, int digits) {
    if (!std::isfinite(v)) { o += "null"; return; }
    std::string s = fmt::format("{:.{}f}", v, digits);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    o += s;
}

/// Appends an optional number, @c null when absent.
void appendOptional(std::string& o, const std::optional<double>& v, int digits) {
    if (v) appendNumber(o, *v, digits);
    else   o += "null";
}

/// 1.4826 x the median of @p p (reorders it); no value for an empty pool.
double robustScale(std::vector<double>& p) {
    if (p.empty()) return kLiveNoValue;
    std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(p.size() / 2), p.end());
    return 1.4826 * p[p.size() / 2];
}

/// The A_r column of station state @p sidx this epoch (through h2local), or -1.
Eigen::Index localColumn(const StationAssembly& R, int sidx) {
    if (sidx < 0) return -1;
    const auto hit = R.h2local.find(sidx);
    return (hit == R.h2local.end() || hit->second < 0) ? -1 : static_cast<Eigen::Index>(hit->second);
}

/// A satellite's post-fit code residuals over the receivers, while the stations are walked.
struct SatResiduals {
    double ss{0.0};                 ///< Sum of the squared code residuals (m^2).
    long long n{0};                 ///< Code rows.
    std::set<std::size_t> rx;       ///< Stations with a code row of it.
};

/**
 * One station's values: the post-fit residuals as the post-fit pass forms them
 * (network/diag/FrameDiag.cpp, networkPostfitSigma0), the wet delay and the clock sigma.  The
 * code and phase rows also feed the network pools and the satellites' residuals.
 */
void stationValues(const NetworkEstimator& est, const Eigen::VectorXd& x_s, const NetStation& st,
                   const StationAssembly& R, std::size_t i, std::size_t slot, LiveStationValues& S,
                   std::vector<double>& pool_code, std::vector<double>& pool_phase,
                   std::map<std::string, SatResiduals>& sats) {
    if (!R.ok || R.A_s.cols() != x_s.size() || R.y.size() == 0) return;
    Eigen::VectorXd x_r;
    try { x_r = est.reconstructStation(slot, x_s); }
    catch (...) { return; }
    if (x_r.size() != R.A_r.cols()) return;
    const Eigen::VectorXd v = R.y - R.A_s * x_s - R.A_r * x_r;

    S.present = true;
    double ss_code = 0.0, ss_phase = 0.0, max_norm = -1.0;
    long long n_code = 0, n_phase = 0;
    std::vector<double> norm_code;
    std::set<std::string> sats_seen;
    for (Eigen::Index row = 0; row < v.size(); ++row) {
        const RowTag& tag = rowTagAt(R, row);
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const NetRowClass cls = rowClassOfKind(tag.kind, w);
        const bool code_row = (cls == NetRowClass::ObsCode);
        if (!code_row && cls != NetRowClass::ObsPhase) continue;    // ties, couplings, pins, priors
        const double r = std::abs(v(row)) * std::sqrt(std::max(0.0, w));
        if (!std::isfinite(r)) continue;
        max_norm = std::max(max_norm, r);
        if (code_row) {
            ss_code += v(row) * v(row);
            ++n_code;
            norm_code.push_back(r);
            pool_code.push_back(r);
            if (tag.sat.valid()) {
                const std::string sat = tag.sat.str();
                sats_seen.insert(sat);
                SatResiduals& sr = sats[sat];
                sr.ss += v(row) * v(row);
                ++sr.n;
                sr.rx.insert(i);
            }
        } else {
            ss_phase += v(row) * v(row);
            ++n_phase;
            pool_phase.push_back(r);
        }
    }
    if (n_code > 0)  S.code_rms_m  = std::sqrt(ss_code / static_cast<double>(n_code));
    if (n_phase > 0) S.phase_rms_m = std::sqrt(ss_phase / static_cast<double>(n_phase));
    if (max_norm >= 0.0) S.max_norm = max_norm;
    S.n_sats     = static_cast<int>(sats_seen.size());
    S.code_scale = robustScale(norm_code);

    // The wet delay and the receiver clock, found as the wet-delay and receiver-state traces
    // find them (the parameter set and h2local, never a carry-gated flag).
    const StationParams& params = st.params;
    const Eigen::Index zcol = localColumn(R, params.zwd);
    if (zcol >= 0 && zcol < x_r.size())
        S.zwd_m = params.slots[static_cast<std::size_t>(params.zwd)].x_lin + x_r(zcol);
    const Eigen::Index ccol = localColumn(R, params.clock);
    if (ccol >= 0 && ccol < x_r.size()) {
        try {
            const Eigen::VectorXd var = est.pureConditionalLocalVarianceDiag(slot, {ccol});
            if (var.size() > 0 && var(0) > 0.0 && std::isfinite(var(0))) S.clk_sd_m = std::sqrt(var(0));
        } catch (...) { /* no sigma this epoch */ }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------
// The JSON writer
// ---------------------------------------------------------------------------------------

std::string liveJsonString(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"')        o += "\\\"";
        else if (ch == '\\')  o += "\\\\";
        else if (ch == '\n')  o += "\\n";
        else if (ch == '\r')  o += "\\r";
        else if (ch == '\t')  o += "\\t";
        else if (c < 0x20)    o += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
        else                  o += ch;
    }
    o += '"';
    return o;
}

std::string liveCutUtf8(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    std::size_t n = max_bytes;
    // Back off over continuation bytes (10xxxxxx) so no character is split.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n));
}

std::string formatLiveHeader(const std::vector<LiveStation>& stations) {
    std::string o = "{\"kind\":\"header\",\"format\":\"netview-live/1\",\"stations\":[";
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (i) o += ',';
        o += "{\"name\":";
        o += liveJsonString(stations[i].name);
        o += ",\"lat\":";
        appendNumber(o, stations[i].lat_deg, 5);
        o += ",\"lon\":";
        appendNumber(o, stations[i].lon_deg, 5);
        o += '}';
    }
    o += "],\"rx_fields\":[";
    for (std::size_t k = 0; k < kLiveStationFields.size(); ++k) {
        if (k) o += ',';
        o += liveJsonString(kLiveStationFields[k]);
    }
    o += "],\"sat_fields\":[";
    for (std::size_t k = 0; k < kLiveSatelliteFields.size(); ++k) {
        if (k) o += ',';
        o += liveJsonString(kLiveSatelliteFields[k]);
    }
    o += "]}";
    return o;
}

std::string formatLiveLine(const LiveEpochLine& L) {
    std::string o;
    o.reserve(L.values ? 64 * (L.values->stations.size() + L.values->satellites.size()) + 256 : 256);
    o += "{\"t\":[";
    o += std::to_string(L.t.week);
    o += ',';
    appendNumber(o, L.t.tow, 3);
    o += ']';
    if (!L.disposition.empty()) { o += ",\"disp\":"; o += liveJsonString(L.disposition); }
    if (L.n_present)    { o += ",\"npres\":"; o += std::to_string(*L.n_present); }
    if (L.n_clock_sats) { o += ",\"nclk\":";  o += std::to_string(*L.n_clock_sats); }
    if (L.values) {
        o += ",\"s0\":[";
        appendNumber(o, L.values->s0_code, 4);
        o += ',';
        appendNumber(o, L.values->s0_phase, 4);
        o += ']';
    }
    if (L.user) {
        o += ",\"user\":[";
        for (std::size_t k = 0; k < 4; ++k) {
            if (k) o += ',';
            appendNumber(o, (*L.user)[k], 4);
        }
        o += ']';
    }
    if (L.wall_ms) { o += ",\"wall_ms\":"; appendNumber(o, *L.wall_ms, 1); }
    if (L.values) {
        o += ",\"rx\":[";
        for (std::size_t i = 0; i < L.values->stations.size(); ++i) {
            if (i) o += ',';
            const LiveStationValues& S = L.values->stations[i];
            if (!S.present) { o += "null"; continue; }
            o += '[';
            appendNumber(o, S.code_rms_m, 4);  o += ',';
            appendNumber(o, S.phase_rms_m, 5); o += ',';
            o += std::to_string(S.n_sats);     o += ',';
            appendNumber(o, S.code_scale, 3);  o += ',';
            appendNumber(o, S.max_norm, 2);    o += ',';
            appendNumber(o, S.zwd_m, 4);       o += ',';
            appendNumber(o, S.clk_sd_m, 4);
            o += ']';
        }
        o += "],\"sat\":{";
        for (std::size_t k = 0; k < L.values->satellites.size(); ++k) {
            if (k) o += ',';
            const LiveSatelliteValues& T = L.values->satellites[k];
            o += liveJsonString(T.sat);
            o += ":[";
            appendNumber(o, T.code_rms_m, 4); o += ',';
            o += std::to_string(T.n_rx);      o += ',';
            appendNumber(o, T.clk_m, 4);      o += ',';
            if (T.disc >= 0) o += std::to_string(T.disc);
            else             o += "null";
            o += ']';
        }
        o += '}';
    }
    if (!L.events.empty()) {
        o += ",\"ev\":[";
        for (std::size_t k = 0; k < L.events.size(); ++k) {
            if (k) o += ',';
            const LiveEvent& e = L.events[k];
            o += '[';
            o += liveJsonString(e.tier);    o += ',';
            o += liveJsonString(e.event);   o += ',';
            o += liveJsonString(e.sat);     o += ',';
            o += liveJsonString(e.station); o += ',';
            o += liveJsonString(e.action);  o += ',';
            o += liveJsonString(e.detail);  o += ',';
            appendOptional(o, e.statistic, 6); o += ',';
            appendOptional(o, e.threshold, 6);
            o += ']';
        }
        o += ']';
    }
    o += '}';
    return o;
}

std::vector<LiveStation> liveStationsOf(const std::vector<NetStation>& stations) {
    std::vector<LiveStation> out;
    out.reserve(stations.size());
    for (const NetStation& st : stations) {
        const Coordinates::Llh llh = Coordinates::ecefToLlh(st.ecef);
        out.push_back({st.name, llh.lat_rad * kRadToDeg, llh.lon_rad * kRadToDeg});
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// The record: the notes, the epoch line, the stream
// ---------------------------------------------------------------------------------------

void NetworkLiveRecord::open(const std::filesystem::path& path, const std::vector<LiveStation>& stations,
                             const std::shared_ptr<spdlog::logger>& log) {
    out.open(path, std::ios::out | std::ios::trunc);
    if (!out) {
        if (log) log->warn("Network: failed to open the live record {}", path.string());
        return;
    }
    out << formatLiveHeader(stations) << '\n';
    out.flush();
    if (log)
        log->info("Network: live record ENABLED: {} (one line per epoch, flushed at each line, for "
                  "scripts/netview/netview.py live); measure-only.", path.string());
}

LiveEpochLine& NetworkLiveRecord::lineFor(const GnssTime& t) {
    if (pending && !(pending->t == t)) {        // a skipped epoch's line, or a late note's
        writeLine(*pending);
        pending.reset();
    }
    if (!pending) {
        pending.emplace();
        pending->t = t;
    }
    return *pending;
}

void NetworkLiveRecord::noteStatus(const GnssTime& t, const char* disposition, const EpochStatusRow& r) {
    if (!on()) return;
    LiveEpochLine& L = lineFor(t);
    L.disposition = disposition ? disposition : "";
    L.n_present = r.n_present;
    L.n_clock_sats = r.n_clock_sats;
}

void NetworkLiveRecord::noteEvent(const GnssTime& t, const char* tier, const char* event, const std::string& sat,
                                  const std::string& station, std::optional<double> statistic,
                                  std::optional<double> threshold, const char* action, const std::string& detail) {
    if (!on()) return;
    lineFor(t).events.push_back({tier ? tier : "", event ? event : "", sat, station, action ? action : "",
                                 liveCutUtf8(detail, kLiveDetailBytes), statistic, threshold});
}

void NetworkLiveRecord::noteUser(const GnssTime& t, double err3d_m, const Eigen::Vector3d& enu_m) {
    if (!on()) return;
    lineFor(t).user = std::array<double, 4>{err3d_m, enu_m(0), enu_m(1), enu_m(2)};
}

void NetworkLiveRecord::writeEpoch(const GnssTime& t, LiveEpochValues values) {
    if (!on()) return;
    LiveEpochLine& L = lineFor(t);
    L.values = std::move(values);
    const auto now = std::chrono::steady_clock::now();
    if (last_epoch_write)
        L.wall_ms = std::chrono::duration<double, std::milli>(now - *last_epoch_write).count();
    last_epoch_write = now;
    writeLine(L);
    pending.reset();
}

void NetworkLiveRecord::writeLine(const LiveEpochLine& line) {
    out << formatLiveLine(line) << '\n';
    out.flush();
    ++lines_written;
}

void NetworkLiveRecord::close() {
    if (!on()) return;
    if (pending) {
        writeLine(*pending);
        pending.reset();
    }
    out.close();
}

// ---------------------------------------------------------------------------------------
// The setup builder and the observer
// ---------------------------------------------------------------------------------------

void networkLiveRecordOpen(NetworkRun& run) {
    // ---- From the run (the one write is the trace set's live record) ----
    const NetworkConfig& nc = run.nc;
    NetworkRunTraces& traces = run.traces;   // written: traces.live_record
    const std::vector<NetStation>& stations = run.setup.stations;
    const std::filesystem::path& log_dir = run.log_dir;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (!nc.trace_live_record) return;   // arming gate
    traces.live_record.open(log_dir / "network_live.jsonl", liveStationsOf(stations), log);
}

void networkLiveRecord(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch.  Reads bind through `ep`; the one write is the live
    //      record's line, through the trace set. ----
    NetworkRunTraces& traces = run.traces;   // written: traces.live_record
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const std::optional<NetworkProductEmitter>& emitter = run.setup.emitter;
    const SyncedEpoch& se = ep.se;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const NetworkSolutionEpoch& sol = ep.prep.sol;
    const bool emitted = ep.emitted;
    // ---- End of the bindings ----
    if (!traces.live_record.on()) return;   // arming gate: network.trace_live_record

    LiveEpochValues values;
    values.stations.resize(stations.size());
    std::vector<double> pool_code, pool_phase;
    std::map<std::string, SatResiduals> sat_res;
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;   // not in this epoch's solve
        stationValues(est, x_s, stations[i], asm_res[i], i, static_cast<std::size_t>(est_index_of[i]),
                      values.stations[i], pool_code, pool_phase, sat_res);
    }
    values.s0_code  = robustScale(pool_code);
    values.s0_phase = robustScale(pool_phase);

    // The satellites: the residuals' and, on an emitted epoch, the emitted clock and the
    // emitter's discontinuity counters (the counters the solution trace prints).
    std::map<std::string, LiveSatelliteValues> sats;
    for (const auto& [sat, sr] : sat_res) {
        LiveSatelliteValues& T = sats[sat];
        T.sat = sat;
        if (sr.n > 0) T.code_rms_m = std::sqrt(sr.ss / static_cast<double>(sr.n));
        T.n_rx = static_cast<int>(sr.rx.size());
    }
    if (emitted && emitter) {
        for (const NetworkSatSolution& s : sol.sats) {
            LiveSatelliteValues& T = sats[s.sat];
            T.sat = s.sat;
            if (s.has_clock) T.clk_m = s.clock_c0_m;
            long long disc = emitter->clockDiscontinuityCounter(s.sat);
            for (const auto& entry : s.phase_bias) disc += emitter->discontinuityCounter(s.sat, entry.first);
            for (const auto& entry : s.code_bias_m) disc += emitter->codeBiasDiscontinuityCounter(s.sat, entry.first);
            T.disc = disc;
        }
    }
    values.satellites.reserve(sats.size());
    for (auto& entry : sats) values.satellites.push_back(std::move(entry.second));

    traces.live_record.writeEpoch(se.time, std::move(values));
}

} // namespace GnssProcessor
