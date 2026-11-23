/**
 * @file StationEntry.cpp
 * @brief The single-band receiver phase-bias park and the cold entry of a refused station (see
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

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <optional>
#include <cstddef>
#include <set>
#include <tuple>
#include <vector>

namespace GnssProcessor {

std::set<System> networkSingleBandConstellations(const std::vector<std::string>& satellites,
                                                 const std::vector<std::string>& signals,
                                                 const std::vector<char>& excluded) {
    std::map<System, std::map<std::string, std::set<char>>> digits;
    const std::size_t n = std::min(satellites.size(), signals.size());
    for (std::size_t r = 0; r < n; ++r) {
        if (r < excluded.size() && excluded[r]) continue;
        const std::string& code = signals[r];
        if (code.size() < 2 || code[0] != 'L') continue;      // phase rows only
        const std::optional<SatId> sat = SatId::parse(satellites[r]);
        if (!sat) continue;
        digits[sat->sys][satellites[r]].insert(code[1]);
    }
    std::set<System> single;
    for (const auto& [sys, sats] : digits) {
        bool dual = false;
        for (const auto& [sat, ds] : sats) {
            (void)sat;
            if (ds.size() >= 2) { dual = true; break; }
        }
        if (!dual) single.insert(sys);
    }
    return single;
}

int networkStationNullDiagnostic(NetworkRunContext& ctx, const NetStation& st,
                                 const StationAssembly& R, NetworkRunTraces* traces,
                                 const GnssTime* t) {
    if (!ctx.knobs.single_band_park) return 0;
    const Eigen::Index n_cols = R.A_r.cols();
    if (n_cols <= 0) return 0;
    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(n_cols, n_cols);
    if (R.A_r.rows() > 0 && R.w.size() == R.A_r.rows())
        N = R.A_r.transpose() * R.w.asDiagonal() * R.A_r;
    for (std::size_t k = 0; k < R.prior_cols.size(); ++k) {
        const Eigen::Index c = R.prior_cols[k];
        if (c >= 0 && c < n_cols) N(c, c) += R.prior_dNrr(static_cast<Eigen::Index>(k));
    }
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N);
    if (es.info() != Eigen::Success) return 0;
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    const double ev_max = ev.cwiseAbs().maxCoeff();
    const double cut = 1e-12 * ev_max;
    double shared_scale = 0.0;
    if (R.A_s.rows() == R.A_r.rows() && R.A_s.cols() > 0)
        shared_scale = (R.A_s.transpose() * R.w.asDiagonal() * R.A_s).diagonal().cwiseAbs().maxCoeff();
    int n = 0;
    for (Eigen::Index k = 0; k < ev.size(); ++k) {
        if (std::abs(ev(k)) >= cut) continue;
        ++n;
        ++ctx.counters.sen_null_diag_directions;
        if (traces == nullptr || t == nullptr) continue;
        // One row per direction below the bar: its eigenvalue and gap to the next, what a pin
        // along it would pass to the border, and its five largest columns.  Measure only.
        const Eigen::VectorXd v = es.eigenvectors().col(k);
        const Eigen::VectorXd c = R.A_s.cols() > 0 && R.A_s.rows() == R.A_r.rows()
                                      ? Eigen::VectorXd(R.A_s.transpose() * R.w.cwiseProduct(R.A_r * v))
                                      : Eigen::VectorXd();
        const double injected = c.size() > 0 ? c.squaredNorm() / std::max(std::abs(ev(k)), 1e-300) : 0.0;
        const double gap = (k + 1 < ev.size()) ? std::abs(ev(k + 1) - ev(k)) : 0.0;
        std::vector<Eigen::Index> ord(static_cast<std::size_t>(v.size()));
        for (Eigen::Index j = 0; j < v.size(); ++j) ord[static_cast<std::size_t>(j)] = j;
        const std::size_t topn = std::min<std::size_t>(5, ord.size());
        std::partial_sort(ord.begin(), ord.begin() + static_cast<std::ptrdiff_t>(topn), ord.end(),
                          [&](Eigen::Index a, Eigen::Index b) {
                              const double fa = std::abs(v(a)), fb = std::abs(v(b));
                              return fa != fb ? fa > fb : a < b;
                          });
        std::string cols;
        for (std::size_t j = 0; j < topn; ++j) {
            std::string nm = "?";
            for (const auto& kv : R.h2local)
                if (kv.second == ord[j] && kv.first >= 0
                    && static_cast<std::size_t>(kv.first) < st.params.slots.size())
                    nm = stateLabel(st.params.slots[static_cast<std::size_t>(kv.first)],
                                    static_cast<std::size_t>(kv.first));
            if (!cols.empty()) cols += ' ';
            cols += fmt::format("{}={:.4f}", nm, v(ord[j]));
        }
        traces->emitEvent(*t, "TOPOLOGY", "sta_null_direction", "", st.name, injected,
                          shared_scale, "measure",
                          fmt::format("ev={:.6e};gap={:.6e};ev_max={:.6e};cols={}", ev(k), gap,
                                      ev_max, cols));
    }
    return n;
}

long long networkStationSingleBandReturn(net::NetworkEntities& entities, int station,
                                         const std::vector<std::string>& rpb_keys) {
    long long dropped = 0;
    const StationId sid{station};
    std::set<System> systems;
    for (const std::string& k : rpb_keys) {
        dropped += static_cast<long long>(
            net::drop(entities.stations, sid, &net::StationState::signals, k,
                      &net::StationSignalState::rpb));
        const auto u = k.find("_RPB_");
        if (u != std::string::npos) systems.insert(systemFromName(k.substr(0, u)));
    }
    for (const System sys : systems) {
        const std::string name = systemName(sys);
        // The inter-system offset: the receiver-clock family entry of kind <system name>.
        dropped += static_cast<long long>(
            net::drop(entities.stations, sid, &net::StationState::signals, name,
                      &net::StationSignalState::rx_clk));
        // The slant ionospheres of the constellation's satellites at the station.
        for (auto it = entities.los.lower_bound(LosId{sid, SatId{}});
             it != entities.los.end() && it->first.station == sid; ++it) {
            if (it->first.sat.sys != sys) continue;
            if (it->second.iono_blk) { it->second.iono_blk.reset(); ++dropped; }
        }
    }
    return dropped;
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
                                          const std::string& refuse_family,
                                          const std::vector<std::string>& parked_keys) {
    StationEntryEdges e;
    // The park episode, on admitted epochs only.
    if (in_solve && parked > 0) {
        if (rec.park_epochs == 0) {
            e.park_opened = true;
            rec.park_families = families;
        }
        for (const std::string& k : parked_keys)
            if (std::find(rec.park_keys.begin(), rec.park_keys.end(), k) == rec.park_keys.end())
                rec.park_keys.push_back(k);
        ++rec.park_epochs;
    } else if (in_solve && rec.park_epochs > 0) {
        e.park_closed = true;
        e.park_epochs = rec.park_epochs;
        e.unparked_keys = rec.park_keys;
        rec.park_epochs = 0;
        rec.park_families.clear();
        rec.park_keys.clear();
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
                              const std::vector<std::string>& families_of,
                              const std::vector<std::vector<std::string>>& keys_of) {
    NetworkRunContext& ctx = run.ctx;
    if (!(ctx.knobs.single_band_park || ctx.knobs.entry_cold)) return;
    // ---- From the run and the epoch ----
    net::NetworkEntities& entities = ctx.entities;   // written: all
    net::StationTable& sta_rows = entities.stations;   // written: stations
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
        if (parked > 0) {
            ++c.sen_park_station_epochs;
            c.sen_park_columns += parked;
            c.sen_park_family["RPB"] += parked;
        }
        const auto f = c.sen_refuse_family_ep.find(name);
        const std::string refuse_family =
            f != c.sen_refuse_family_ep.end() ? f->second : std::string();
        const bool was_in_solve = rec.ever_in_solve;
        const StationEntryEdges e = networkStationEntryStep(
            rec, in_solve, refused, parked,
            i < families_of.size() ? families_of[i] : std::string(), se.time.tow, refuse_family,
            i < keys_of.size() ? keys_of[i] : std::vector<std::string>{});
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
            // The second band returned: the free direction is reborn loose as one re-coupling
            // (B's rule): the held receiver phase biases' carries, and any carried inter-system
            // offset and slant ionosphere of the same constellation at the station.
            c.sen_park_unpark_drops += networkStationSingleBandReturn(entities, static_cast<int>(i),
                                                                      e.unparked_keys);
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
    if (!(ctx.knobs.single_band_park || ctx.knobs.entry_cold)) return 0;
    const auto& c = ctx.counters;
    std::string fam;
    for (const auto& [k, n] : c.sen_park_family) {
        if (!fam.empty()) fam += ' ';
        fam += fmt::format("{}={}", k, n);
    }
    if (ctx.knobs.single_band_park)
        v.log->info("Network (station single-band park): {} station-epoch(s) with a constellation "
                    "on one band, {} receiver phase-bias column(s) held at their carried value, "
                    "{} of the station-epochs admitted; {} park episode(s) opened, {} closed, "
                    "{} carried bias(es) dropped at a close (reborn loose); measure-only: {} free "
                    "direction(s) counted in refused stations.  0 parked is no single-band "
                    "constellation (or inert wiring).",
                    c.sen_park_station_epochs, c.sen_park_columns, c.sen_park_kept,
                    c.sen_park_episodes, c.sen_park_episodes_closed, c.sen_park_unpark_drops,
                    c.sen_null_diag_directions);
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
