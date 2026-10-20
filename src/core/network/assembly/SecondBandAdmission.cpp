/**
 * @file SecondBandAdmission.cpp
 * @brief Implementation of the second-band admission (see the header for the rule, the
 *        position in the epoch and the byte-identity contract).
 */
#include "network/assembly/SecondBandAdmission.hpp"

#include "common/GnssIdentity.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "model/PppModel.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>

namespace GnssProcessor {

namespace {

/// Edge log lines written before the rest are only counted. The event rows are not limited.
constexpr long long kEdgeLogLimit = 200;

/// The constellation name of the unit key under station scope is the traits name.
const char* consName(System sys) { return systemName(sys); }

/// Writes one edge log line within the line limit and counts the lines it drops.
void edgeLog(const std::shared_ptr<spdlog::logger>& log, NetworkSecondBandCounters& ctr,
             const std::string& line) {
    if (!log) return;   // a context with no logger (the default logger can be dropped)
    if (ctr.withhold_edges + ctr.readmit_edges > kEdgeLogLimit) {
        ++ctr.log_lines_dropped;
        return;
    }
    log->info("{}", line);
}

/// The candidates of every screened constellation of the run: the active constellations
/// other than the time reference that have a priority list.
std::map<System, SecondBandCandidates> runCandidates(const NetworkRun& run) {
    std::map<System, SecondBandCandidates> out;
    const auto& prio = run.config.getSignalPriorities();
    const std::vector<NetStation>& stations = run.setup.stations;
    for (const std::string& name : run.config.getActiveSystems()) {
        const System sys = systemFromName(name);
        if (sys == System::Unknown || isTimeReference(sys)) continue;
        const auto pit = prio.find(name);
        if (pit == prio.end()) continue;
        // The coverage gate is network wide: every producer model holds the same set, so
        // the first model that exists speaks for the run.
        const std::set<std::string>* admitted = nullptr;
        for (const NetStation& st : stations) {
            if (!st.model) continue;
            const auto& gate = st.model->signalAllowlist();
            const auto git = gate.find(name);
            if (git != gate.end()) admitted = &git->second;
            break;
        }
        out.emplace(sys, secondBandCandidates(sys, pit->second, admitted));
    }
    return out;
}

}  // namespace

SecondBandCandidates secondBandCandidates(System sys, const std::vector<std::string>& priority,
                                          const std::set<std::string>* admitted) {
    SecondBandCandidates c;
    const std::string cons = consName(sys);
    for (const std::string& sig : priority) {
        if (sig.size() < 3) continue;
        const ObsType type = obsTypeOf(sig);
        if (type != ObsType::Code && type != ObsType::Phase) continue;
        if (admitted != nullptr && admitted->count(sig) == 0) continue;
        const FrequencyBand band = classifySignal(cons, sig).band;
        if (band == FrequencyBand::Unknown) continue;
        (type == ObsType::Code ? c.code : c.phase)[sig] = band;
    }
    return c;
}

int secondBandCount(const RinexObservation& obs, const SecondBandCandidates& cand) {
    // One bit per physical band, so a band with several signals counts once.
    static_assert(static_cast<int>(FrequencyBand::IrnS) < 64, "FrequencyBand fits a 64-bit mask");
    std::uint64_t code_bands = 0, phase_bands = 0;
    for (const auto& kv : obs.measurements) {
        const std::string& key = kv.first;
        const auto cit = cand.code.find(key);
        if (cit != cand.code.end()) {
            code_bands |= std::uint64_t{ 1 } << static_cast<int>(cit->second);
            continue;
        }
        const auto pit = cand.phase.find(key);
        if (pit != cand.phase.end())
            phase_bands |= std::uint64_t{ 1 } << static_cast<int>(pit->second);
    }
    std::uint64_t both = code_bands & phase_bands;
    int n = 0;
    for (; both != 0; both &= both - 1) ++n;
    return n;
}

SecondBandStep secondBandStep(std::map<std::string, long long>& since, const std::string& unit,
                              bool admitted, long long epoch_idx) {
    SecondBandStep s;
    const auto it = since.find(unit);
    if (!admitted) {
        if (it == since.end()) {
            since.emplace(unit, epoch_idx);
            s.withhold_edge = true;
        }
        return s;
    }
    if (it != since.end()) {
        s.readmit_edge = true;
        s.gap_ep = epoch_idx - it->second;
        since.erase(it);
    }
    return s;
}

const SyncedEpoch& networkSecondBandAdmission(NetworkRun& run, const SyncedEpoch& se_in,
                                              SyncedEpoch& scratch) {
    NetworkRunContext& ctx = run.ctx;
    if (!ctx.knobs.second_band_admission) return se_in;   // off: no walk, no copy, no row
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const std::vector<NetStation>& stations = run.setup.stations;
    net::StationTable& sta_rows = ctx.entities.stations;   // StationState::sba_since
    NetworkSecondBandCounters& ctr = ctx.counters.sba;
    const bool by_station = ctx.knobs.second_band_scope == SecondBandScope::Station;
    const int min_sats = ctx.knobs.second_band_min_sats;

    // The processed-epoch clock of this seam: one increment per call, so a gap counts every
    // epoch that reached the epoch body whether or not the unit was present.
    const long long epoch_idx = ctr.epochs++;
    const GnssTime& t = se_in.time;

    const std::map<System, SecondBandCandidates> cands = runCandidates(run);
    if (cands.empty()) return se_in;

    // Pass 1 decides on the caller's epoch and records what must go. The copy is taken only
    // if pass 1 found something, so an armed epoch that withholds nothing costs one walk.
    std::vector<std::pair<std::size_t, std::set<std::string>>> kill;   // station index, satellites

    for (std::size_t i = 0; i < stations.size(); ++i) {
        const auto sit = se_in.by_station.find(stations[i].name);
        if (sit == se_in.by_station.end() || sit->second.empty()) continue;
        const std::vector<RinexObservation>& recs = sit->second;

        // The records of each screened constellation, with their count of modelled bands.
        struct Rec { std::size_t k; int bands; };
        std::map<System, std::vector<Rec>> by_cons;
        for (std::size_t k = 0; k < recs.size(); ++k) {
            const auto cit = cands.find(recs[k].system);
            if (cit == cands.end()) continue;
            by_cons[recs[k].system].push_back(Rec{ k, secondBandCount(recs[k], cit->second) });
        }
        if (by_cons.empty()) continue;

        const StationId sid{ static_cast<int>(i) };
        std::set<std::string> drop;
        // The memory row is created on the first edge only, so a station that never has a
        // withheld unit owns no row.
        auto memory = [&]() -> std::map<std::string, long long>& { return sta_rows[sid].sba_since; };

        for (const auto& [sys, rs] : by_cons) {
            const std::string cons = consName(sys);

            if (by_station) {
                int n_dual = 0, n_any = 0;
                for (const Rec& r : rs) {
                    n_dual += (r.bands >= 2) ? 1 : 0;
                    n_any += (r.bands >= 1) ? 1 : 0;
                }
                if (n_any == 0) {
                    // No record of the constellation carries a modelled band: the producer
                    // forms no row from any of them, so there is nothing to screen and the
                    // unit's state is left as it is.
                    ctr.records_unmodelled += static_cast<long long>(rs.size());
                    continue;
                }
                ctr.records_screened += static_cast<long long>(rs.size());
                const bool admitted = n_dual >= min_sats;
                ++ctr.units_screened;
                const auto row = sta_rows.find(sid);
                const bool tracked = row != sta_rows.end() && row->second.sba_since.count(cons) != 0;
                SecondBandStep step;
                if (admitted && !tracked) {
                    // Nothing was withheld and nothing is: no row, no edge.
                } else {
                    step = secondBandStep(memory(), cons, admitted, epoch_idx);
                }
                const double need = static_cast<double>(min_sats);
                if (step.withhold_edge) {
                    ++ctr.withhold_edges;
                    traces.emitEvent(t, "ADMISSION", "second_band", std::string(),
                                     stations[i].name, static_cast<double>(n_dual), need,
                                     "withhold",
                                     fmt::format("scope=station;cons={};sats={};dual={};need={}",
                                                 cons, rs.size(), n_dual, min_sats));
                    edgeLog(log, ctr, fmt::format("Network (second-band admission): TOW {:.1f}: station '{}' WITHHOLD {}: "
                            "{} satellite(s) tracked, {} on two modelled bands, {} needed.",
                            t.tow, stations[i].name, cons, rs.size(), n_dual, min_sats));
                }
                if (step.readmit_edge) {
                    ++ctr.readmit_edges;
                    ctr.max_gap_ep = std::max(ctr.max_gap_ep, step.gap_ep);
                    traces.emitEvent(t, "ADMISSION", "second_band", std::string(),
                                     stations[i].name, static_cast<double>(n_dual), need,
                                     "readmit",
                                     fmt::format("scope=station;cons={};sats={};dual={};need={};"
                                                 "withheld_ep={}",
                                                 cons, rs.size(), n_dual, min_sats, step.gap_ep));
                    edgeLog(log, ctr, fmt::format("Network (second-band admission): TOW {:.1f}: station '{}' READMIT {} "
                            "after {} processed epoch(s): {} of {} satellite(s) on two modelled "
                            "bands.",
                            t.tow, stations[i].name, cons, step.gap_ep, n_dual, rs.size()));
                }
                if (!admitted) {
                    ++ctr.units_withheld;
                    ctr.records_withheld += static_cast<long long>(rs.size());
                    ctr.withheld_by_cons[cons] += static_cast<long long>(rs.size());
                    ctr.affected[cons].insert(stations[i].name);
                    for (const Rec& r : rs) drop.insert(recs[r.k].satellite);
                }
                continue;
            }

            // Satellite scope: each record is its own unit.
            for (const Rec& r : rs) {
                const std::string& sat = recs[r.k].satellite;
                if (r.bands == 0) {
                    ++ctr.records_unmodelled;   // no modelled band: no row is formed, nothing to screen
                    continue;
                }
                ++ctr.records_screened;
                const bool admitted = r.bands >= 2;
                ++ctr.units_screened;
                const auto row = sta_rows.find(sid);
                const bool tracked = row != sta_rows.end() && row->second.sba_since.count(sat) != 0;
                SecondBandStep step;
                if (!(admitted && !tracked)) step = secondBandStep(memory(), sat, admitted, epoch_idx);
                if (step.withhold_edge) {
                    ++ctr.withhold_edges;
                    traces.emitEvent(t, "ADMISSION", "second_band", sat, stations[i].name,
                                     static_cast<double>(r.bands), 2.0, "withhold",
                                     fmt::format("scope=satellite;cons={};bands={};need=2", cons,
                                                 r.bands));
                    edgeLog(log, ctr, fmt::format("Network (second-band admission): TOW {:.1f}: station '{}' WITHHOLD "
                            "{} satellite {}: {} modelled band(s), 2 needed.",
                            t.tow, stations[i].name, cons, sat, r.bands));
                }
                if (step.readmit_edge) {
                    ++ctr.readmit_edges;
                    ctr.max_gap_ep = std::max(ctr.max_gap_ep, step.gap_ep);
                    traces.emitEvent(t, "ADMISSION", "second_band", sat, stations[i].name,
                                     static_cast<double>(r.bands), 2.0, "readmit",
                                     fmt::format("scope=satellite;cons={};bands={};need=2;"
                                                 "withheld_ep={}", cons, r.bands, step.gap_ep));
                    edgeLog(log, ctr, fmt::format("Network (second-band admission): TOW {:.1f}: station '{}' READMIT "
                            "{} satellite {} after {} processed epoch(s): {} modelled bands.",
                            t.tow, stations[i].name, cons, sat, step.gap_ep, r.bands));
                }
                if (!admitted) {
                    ++ctr.units_withheld;
                    ++ctr.records_withheld;
                    ++ctr.withheld_by_cons[cons];
                    ctr.affected[cons].insert(stations[i].name);
                    drop.insert(sat);
                }
            }
        }
        if (!drop.empty()) kill.emplace_back(i, std::move(drop));
    }

    if (kill.empty()) return se_in;   // nothing withheld: no copy this epoch

    // Pass 2: the single copy, then the erase. The whole record leaves the epoch (never a
    // per-signal edit), so every downstream consumer sees what it would see had the RINEX
    // record been missing.
    scratch = se_in;
    ++ctr.epochs_copied;
    for (const auto& [i, drop] : kill) {
        const auto sit = scratch.by_station.find(stations[i].name);
        if (sit == scratch.by_station.end()) continue;
        std::vector<RinexObservation>& v = sit->second;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const RinexObservation& o) { return drop.count(o.satellite) != 0; }),
                v.end());
    }
    return scratch;
}

// ---- Run-end report: one row of kNetworkRunEndReports ----

void networkSecondBandAdmissionReport(const NetworkRunContext& ctx,
                                      const std::shared_ptr<spdlog::logger>& log) {
    if (!ctx.knobs.second_band_admission || !log) return;
    const NetworkSecondBandCounters& c = ctx.counters.sba;
    const bool by_station = ctx.knobs.second_band_scope == SecondBandScope::Station;
    log->info("Network (SECOND-BAND ADMISSION, scope {}{}): {} epoch(s) screened {} record(s) in {} "
              "unit(s) ({}), leaving {} record(s) alone that carry no modelled band; WITHHELD {} "
              "record(s) in {} unit-epoch(s), on {} epoch(s). EDGES (invariant 7): {} withhold, "
              "{} readmit, longest withheld run that came back {} epoch(s).{}{}",
              by_station ? "station" : "satellite",
              by_station ? fmt::format(", at least {} satellite(s) on two modelled bands",
                                       ctx.knobs.second_band_min_sats)
                         : std::string(),
              c.epochs, c.records_screened, c.units_screened,
              by_station ? "station-constellation-epochs" : "satellite-epochs",
              c.records_unmodelled, c.records_withheld, c.units_withheld, c.epochs_copied, c.withhold_edges,
              c.readmit_edges, c.max_gap_ep,
              c.log_lines_dropped > 0
                  ? fmt::format("  {} edge log line(s) past the limit of {} were not written; "
                                "the event rows are complete.", c.log_lines_dropped,
                                kEdgeLogLimit)
                  : std::string(),
              c.records_withheld == 0
                  ? (c.records_screened == 0
                         ? "  ARMED AND INERT: no record of a screened constellation carrying a "
                           "modelled band was seen, so the rule could not act. This is dead wiring, "
                           "a roster with no constellation other than the time reference, or a "
                           "priority list that matches no tracked signal, not a quiet run."
                         : "  ARMED AND INERT: the screen ran and every constellation held two "
                           "modelled bands at every station that tracked it. Read this as a "
                           "property of the data, against the screened denominator.")
                  : "");
    for (const auto& [cons, names] : c.affected) {
        std::string list;
        std::size_t n = 0;
        for (const std::string& s : names) {
            if (n++ == 12) { list += fmt::format(", and {} more", names.size() - 12); break; }
            if (!list.empty()) list += ", ";
            list += s;
        }
        const auto wit = c.withheld_by_cons.find(cons);
        log->info("Network (SECOND-BAND ADMISSION): {} withheld at {} station(s) ({}), {} record(s).",
                  cons, names.size(), list, wit == c.withheld_by_cons.end() ? 0 : wit->second);
    }
}

int networkSecondBandAdmissionRunEnd(const NetworkRunEndView& v) {
    networkSecondBandAdmissionReport(v.ctx, v.log);
    return 0;
}

}  // namespace GnssProcessor
