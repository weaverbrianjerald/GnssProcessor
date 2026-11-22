/**
 * @file StationEntry.cpp
 * @brief The park of a rowless local column and the cold entry of a refused station (see
 *        StationEntry.hpp).
 */

#include "network/solve/StationEntry.hpp"

#include "config/GnssConfig.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <tuple>
#include <vector>

namespace GnssProcessor {

namespace {

/// The family of a state label ("RPB" of "RPB:GPS_RPB_L5Q"), as the PD-drop census names it.
std::string labelFamily(const std::string& label) {
    const auto c = label.find(':');
    if (c != std::string::npos) return label.substr(0, c);
    return label == "?" ? std::string("other") : label;
}

/// Remove from @p v every element whose column (read by @p col_of) is in @p parked.
template <class Vec, class ColOf>
void eraseParked(Vec& v, const std::set<Eigen::Index>& parked, ColOf col_of) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const auto& e) { return parked.count(col_of(e)) > 0; }),
            v.end());
}

}  // namespace

int networkStationRowlessPark(NetworkRunContext& ctx, const NetStation& st, StationAssembly& R) {
    if (!ctx.knobs.rowless_park) return 0;
    const Eigen::Index n_cols = R.A_r.cols();
    if (n_cols <= 0) return 0;
    // A column has information when any row touches it or either diagonal channel holds it.
    std::vector<char> informed(static_cast<std::size_t>(n_cols), 0);
    for (Eigen::Index c = 0; c < n_cols; ++c)
        if (R.A_r.rows() > 0 && R.A_r.col(c).cwiseAbs().maxCoeff() > 0.0)
            informed[static_cast<std::size_t>(c)] = 1;
    for (const Eigen::Index c : R.prior_cols)
        if (c >= 0 && c < n_cols) informed[static_cast<std::size_t>(c)] = 1;
    for (const Eigen::Index c : R.carry_cols)
        if (c >= 0 && c < n_cols) informed[static_cast<std::size_t>(c)] = 1;
    std::set<Eigen::Index> parked;
    for (Eigen::Index c = 0; c < n_cols; ++c)
        if (!informed[static_cast<std::size_t>(c)]) parked.insert(c);
    if (parked.empty()) return 0;

    // The parked columns' names, through the assembly's own column map (the PD-drop census's).
    std::vector<std::string> names(static_cast<std::size_t>(n_cols), "?");
    for (const auto& kv : R.h2local) {
        const auto lc = static_cast<std::size_t>(kv.second);
        const auto k  = static_cast<std::size_t>(kv.first);
        if (kv.second >= 0 && lc < names.size() && kv.first >= 0 && k < st.params.slots.size())
            names[lc] = stateLabel(st.params.slots[k], k);
    }

    // The unit prior at the linearization point, merged into the pre-PD channel in column order.
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>> entries;
    entries.reserve(R.prior_cols.size() + parked.size());
    for (std::size_t k = 0; k < R.prior_cols.size(); ++k)
        entries.emplace_back(R.prior_cols[k], R.prior_dNrr(static_cast<Eigen::Index>(k)),
                             R.prior_dur(static_cast<Eigen::Index>(k)),
                             k < R.prior_tags.size() ? R.prior_tags[k] : RowTag{});
    for (const Eigen::Index c : parked)
        entries.emplace_back(c, 1.0, 0.0, RowTag{ContributionKind::PriorRowlessPark, {}, {}});
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    const Eigen::Index n = static_cast<Eigen::Index>(entries.size());
    R.prior_cols.resize(static_cast<std::size_t>(n));
    R.prior_dNrr.resize(n);
    R.prior_dur.resize(n);
    R.prior_tags.resize(static_cast<std::size_t>(n));
    for (Eigen::Index k = 0; k < n; ++k) {
        const auto& e = entries[static_cast<std::size_t>(k)];
        R.prior_cols[static_cast<std::size_t>(k)] = std::get<0>(e);
        R.prior_dNrr(k) = std::get<1>(e);
        R.prior_dur(k)  = std::get<2>(e);
        R.prior_tags[static_cast<std::size_t>(k)] = std::get<3>(e);
    }

    // No harvest reads a parked column: it is in none of the station's harvest lists.
    eraseParked(R.pl_rpb, parked, [](const auto& t) { return std::get<1>(t); });
    eraseParked(R.pl_cb, parked, [](const auto& t) { return std::get<1>(t); });
    eraseParked(R.pl_iono, parked, [](const auto& t) { return std::get<1>(t); });
    eraseParked(R.pl_rxclk, parked, [](const auto& e) { return e.col; });
    eraseParked(R.pl_pc, parked, [](const auto& e) { return e.col; });
    eraseParked(R.pl_amb, parked, [](const auto& e) { return e.col; });
    eraseParked(R.rpb_trace, parked, [](const auto& e) { return e.col; });

    std::set<std::string> fams;
    for (const Eigen::Index c : parked) {
        const std::string fam = labelFamily(names[static_cast<std::size_t>(c)]);
        fams.insert(fam);
        ++ctx.counters.sen_park_family[fam];
    }
    R.parked_cols.assign(parked.begin(), parked.end());
    R.parked_families.clear();
    for (const std::string& f : fams) {
        if (!R.parked_families.empty()) R.parked_families += ' ';
        R.parked_families += f;
    }
    ++ctx.counters.sen_park_station_epochs;
    ctx.counters.sen_park_columns += static_cast<long long>(parked.size());
    return static_cast<int>(parked.size());
}

StationColdDrop networkStationColdDrop(net::NetworkEntities& entities, int station,
                                       const std::string& name) {
    StationColdDrop d;
    const StationId sid{station};
    // The station's own carries and its receiver-side parent edges.
    d.carries += static_cast<long long>(net::drop(entities.stations, sid, &net::StationState::zwd));
    if (auto it = entities.stations.find(sid); it != entities.stations.end()) {
        for (auto& [key, sig] : it->second.signals) {
            (void)key;
            if (sig.rx_clk) { sig.rx_clk.reset(); ++d.carries; }
            if (sig.rx_clk_rate) { sig.rx_clk_rate.reset(); ++d.carries; }
            if (sig.rpb) { sig.rpb.reset(); ++d.carries; }
            if (sig.rx_cb) { sig.rx_cb.reset(); ++d.carries; }
            if (sig.eq34_rx_parent) { sig.eq34_rx_parent.reset(); ++d.chain_cells; }
        }
    }
    // Its lines of sight: the slant ionosphere and the phase colour.
    for (auto it = entities.los.lower_bound(LosId{sid, SatId{}});
         it != entities.los.end() && it->first.station == sid; ++it) {
        net::LosState& L = it->second;
        if (L.iono_blk) { L.iono_blk.reset(); ++d.carries; }
        if (L.gl_hold_iono_blk) { L.gl_hold_iono_blk.reset(); ++d.carries; }
        if (L.pc) { L.pc.reset(); ++d.carries; }
    }
    // Its arcs: the leveling ambiguities and the chain's pin targets.
    for (auto it = net::arcsOfStation(entities.arcs, station);
         it != entities.arcs.end() && it->first.station == sid; ++it) {
        net::ArcState& A = it->second;
        if (A.amb) { A.amb.reset(); ++d.carries; }
        if (A.gl_hold_amb) { A.gl_hold_amb.reset(); ++d.carries; }
        if (A.eq34_prev_target) { A.eq34_prev_target.reset(); ++d.chain_cells; }
        if (A.eq34_seed_realized) { A.eq34_seed_realized.reset(); ++d.chain_cells; }
    }
    // The satellites whose parent edge the station held.
    for (auto& [key, S] : entities.sat_signals) {
        (void)key;
        if (S.anchor_incumbent && *S.anchor_incumbent == name) {
            S.anchor_incumbent.reset();
            ++d.chain_cells;
        }
    }
    return d;
}

void networkStationEntryCold(NetworkRun& run, const NetworkEpoch& ep) {
    NetworkRunContext& ctx = run.ctx;
    if (!ctx.knobs.entry_cold) return;
    // ---- From the run and the epoch ----
    net::NetworkEntities& entities = ctx.entities;   // written: all
    const std::vector<NetStation>& stations = run.setup.stations;
    (void)ep;
    // ---- End of the bindings ----
    auto& c = ctx.counters;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        const net::StationEntryRecord* rec =
            net::get(entities.stations, StationId{static_cast<int>(i)}, &net::StationState::entry);
        if (rec == nullptr || rec->out_epochs <= 0) continue;
        ++c.sen_cold_station_epochs;
        const StationColdDrop d =
            networkStationColdDrop(entities, static_cast<int>(i), stations[i].name);
        c.sen_cold_carries_dropped += d.carries;
        c.sen_cold_chain_cells += d.chain_cells;
    }
}

StationEntryEdges networkStationEntryStep(net::StationEntryRecord& rec, bool in_solve, bool refused,
                                          int parked, const std::string& families, double tow,
                                          const std::string& refuse_family) {
    StationEntryEdges e;
    // The park episode, on admitted epochs only.
    if (in_solve && parked > 0) {
        if (rec.park_epochs == 0) {
            e.park_opened = true;
            rec.park_families = families;
        }
        ++rec.park_epochs;
    } else if (in_solve && rec.park_epochs > 0) {
        e.park_closed = true;
        e.park_epochs = rec.park_epochs;
        rec.park_epochs = 0;
        rec.park_families.clear();
    }
    // The refusal and the return.
    if (refused) {
        if (rec.out_epochs == 0) {
            e.refused = true;
            rec.refused_at_tow = tow;
            rec.family = refuse_family.empty() ? std::string("unnamed") : refuse_family;
        }
        ++rec.out_epochs;
    } else if (in_solve) {
        if (rec.out_epochs > 0) {
            e.returned = true;
            e.first_entry = !rec.ever_in_solve;
            e.out_epochs = rec.out_epochs;
        }
        rec.ever_in_solve = true;
    }
    return e;
}

void networkStationEntryTrack(NetworkRun& run, const NetworkEpoch& ep,
                              const std::vector<char>& in_solve_of,
                              const std::vector<char>& refused_of,
                              const std::vector<int>& parked_of,
                              const std::vector<std::string>& families_of) {
    NetworkRunContext& ctx = run.ctx;
    if (!(ctx.knobs.rowless_park || ctx.knobs.entry_cold)) return;
    // ---- From the run and the epoch ----
    net::StationTable& sta_rows = ctx.entities.stations;   // written: stations
    const std::vector<NetStation>& stations = run.setup.stations;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const bool topo = run.config.getNetworkConfig().trace_topology;
    // ---- End of the bindings ----
    auto& c = ctx.counters;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        net::StationEntryRecord& rec = net::put(sta_rows, StationId{static_cast<int>(i)},
                                           &net::StationState::entry);
        const std::string& name = stations[i].name;
        const bool in_solve = i < in_solve_of.size() && in_solve_of[i];
        const bool refused  = i < refused_of.size() && refused_of[i];
        const int parked    = i < parked_of.size() ? parked_of[i] : 0;
        const auto f = c.sen_refuse_family_ep.find(name);
        const std::string refuse_family =
            f != c.sen_refuse_family_ep.end() ? f->second : std::string();
        const bool was_in_solve = rec.ever_in_solve;
        const StationEntryEdges e = networkStationEntryStep(
            rec, in_solve, refused, parked,
            i < families_of.size() ? families_of[i] : std::string(), se.time.tow, refuse_family);
        if (in_solve && parked > 0) ++c.sen_park_kept;
        if (e.park_opened) {
            ++c.sen_park_episodes;
            if (topo)
                traces.emitEvent(se.time, "TOPOLOGY", "sta_column_parked", "", name,
                                 static_cast<double>(parked), std::nullopt, "park",
                                 fmt::format("columns={};families={}", parked, rec.park_families));
        }
        if (e.park_closed) {
            ++c.sen_park_episodes_closed;
            if (topo)
                traces.emitEvent(se.time, "TOPOLOGY", "sta_column_unparked", "", name,
                                 static_cast<double>(e.park_epochs), std::nullopt, "unpark",
                                 fmt::format("epochs_parked={}", e.park_epochs));
        }
        if (e.refused) {
            ++c.sen_refused_events;
            if (topo)
                traces.emitEvent(se.time, "TOPOLOGY", "sta_refused", "", name, std::nullopt,
                                 std::nullopt, "decouple",
                                 fmt::format("family={};ever_in_solve={}", rec.family,
                                             was_in_solve ? 1 : 0));
        }
        if (e.returned) {
            ++c.sen_returned_events;
            if (e.first_entry) ++c.sen_returned_first_entry;
            c.sen_out_epochs_sum += e.out_epochs;
            c.sen_out_epochs_max = std::max(c.sen_out_epochs_max, e.out_epochs);
            if (topo)
                traces.emitEvent(se.time, "TOPOLOGY", "sta_returned", "", name,
                                 static_cast<double>(e.out_epochs), std::nullopt, "recouple",
                                 fmt::format("epochs_out={};refused_at={:.1f};family={};"
                                             "first_entry={};cold={}",
                                             e.out_epochs, rec.refused_at_tow, rec.family,
                                             e.first_entry ? 1 : 0, ctx.knobs.entry_cold ? 1 : 0));
            rec.out_epochs = 0;
            rec.family.clear();
        }
    }
    c.sen_refuse_family_ep.clear();
}

int networkStationEntryRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    if (!(ctx.knobs.rowless_park || ctx.knobs.entry_cold)) return 0;
    const auto& c = ctx.counters;
    std::string fam;
    for (const auto& [k, n] : c.sen_park_family) {
        if (!fam.empty()) fam += ' ';
        fam += fmt::format("{}={}", k, n);
    }
    if (ctx.knobs.rowless_park)
        v.log->info("Network (station rowless-column park): {} station-epoch(s) with a parked "
                    "column ({} column(s): [{}]), {} of them admitted; {} park episode(s) "
                    "opened, {} closed.  0 parked is inert wiring or no rowless column.",
                    c.sen_park_station_epochs, c.sen_park_columns,
                    fam.empty() ? std::string("none") : fam, c.sen_park_kept,
                    c.sen_park_episodes, c.sen_park_episodes_closed);
    v.log->info("Network (station entry): {} refusal(s) (decoupling edges), {} return(s) "
                "(re-coupling edges; {} first entries of a station refused from its first "
                "epoch), epochs out per return mean {:.1f} max {}; cold entry {}: {} "
                "station-epoch(s), {} carried entr(y/ies) dropped, {} chain memory cell(s) "
                "forgotten.",
                c.sen_refused_events, c.sen_returned_events, c.sen_returned_first_entry,
                c.sen_returned_events > 0
                    ? static_cast<double>(c.sen_out_epochs_sum) / static_cast<double>(c.sen_returned_events)
                    : 0.0,
                c.sen_out_epochs_max, ctx.knobs.entry_cold ? "armed" : "off",
                c.sen_cold_station_epochs, c.sen_cold_carries_dropped, c.sen_cold_chain_cells);
    return 0;
}

}  // namespace GnssProcessor
