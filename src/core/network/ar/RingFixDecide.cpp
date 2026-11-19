/**
 * @file RingFixDecide.cpp
 * @brief The ring fix's decision after the solve as named stages (see network/ar/RingFixDecide.hpp
 *        for their order; networkRingFixDecide in RingFixSeams.cpp calls them).
 */

#include "network/ar/RingFixDecide.hpp"

#include "common/GnssIdentity.hpp"            // constellationKey, systemOfSat
#include "network/ar/ArcGraph.hpp"            // arcgraph::Dsu, CycleTerm, CycleLattice
#include "network/assembly/Contributions.hpp" // ContributionKind (the phase rows of a residual)
#include "network/assembly/RowClassify.hpp"   // rowTagAt, rowClassOfKind, rowObservationCode (the guard's code rows)
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"        // NetStation, StationAssembly, RelevelEpochState
#include "network/solve/ChainChordTable.hpp"  // chainAmbColumns, chainStationChordRows, chainChordFloats, chainWavelengthM
#include "network/solve/ChainFrame.hpp"       // ChainEpochObs, ChainChordCycle
#include "network/solve/Estimator.hpp"
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"           // SyncedEpoch

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace GnssProcessor {

namespace {

/// The post-fit residual (m) of the observation phase row on an ambiguity column, if one exists.
std::optional<double> phaseResidual(const StationAssembly& R, const Eigen::VectorXd& resid, Eigen::Index col) {
    if (col < 0 || col >= R.A_r.cols()) return std::nullopt;
    const Eigen::Index n = std::min<Eigen::Index>(R.A_r.rows(), static_cast<Eigen::Index>(R.row_tags.size()));
    for (Eigen::Index k = 0; k < n && k < resid.size(); ++k) {
        const ContributionKind kind = R.row_tags[static_cast<std::size_t>(k)].kind;
        if (kind != ContributionKind::DataPhase && kind != ContributionKind::DataPhaseSecond) continue;
        if (std::abs(R.A_r(k, col)) > 0.5) return resid(k);
    }
    return std::nullopt;
}

const char* auditName(RingFixAudit a) {
    switch (a) {
        case RingFixAudit::Right:    return "right";
        case RingFixAudit::Wrong:    return "wrong";
        case RingFixAudit::Unlisted: return "unlisted";
    }
    return "?";
}

std::string cellOf(double v, int digits) {
    if (!std::isfinite(v)) return std::string();
    return fmt::format("{:.{}f}", v, digits);
}

// ---- The re-attachment of untied satellites (network.ring_fix.reattach) ----

/// The parents in one band's trees this epoch, for the test that a station hangs below a satellite.
struct BandParents {
    std::map<std::string, int> sat_parent;       ///< Satellite -> the station its tree edge hangs it under.
    std::map<int, std::string> station_parent;   ///< Station (roster index) -> the satellite its tree edge hangs it under.
};

BandParents bandParents(const ChainEpochObs& obs, const std::string& ck, const std::string& signal) {
    BandParents p;
    for (const ChainEdgeObs& e : obs.edges) {
        if (e.signal != signal || constellationKey(systemOfSat(e.sat)) != ck) continue;
        if (e.sat_is_child) p.sat_parent[e.sat] = e.station;
        else                p.station_parent[e.station] = e.sat;
    }
    return p;
}

/// Whether a station hangs below a satellite in one band's trees: its chain of parents reaches the satellite.
bool hangsBelow(const BandParents& p, int station, const std::string& sat) {
    int cur = station;
    for (int step = 0; step < 1024; ++step) {
        const auto ps = p.station_parent.find(cur);
        if (ps == p.station_parent.end()) return false;
        if (ps->second == sat) return true;
        const auto pp = p.sat_parent.find(ps->second);
        if (pp == p.sat_parent.end()) return false;
        cur = pp->second;
    }
    return false;
}

/// This epoch's refused keeps (a re-host onto a held link whose entry check refused it) as events; cleared.
void reportKeepRefusals(NetworkRingFix& rf, const std::vector<NetStation>& stations, const GnssTime& t,
                        NetworkRunTraces& traces) {
    for (const auto& [id, v] : rf.keep_refusals) {
        const std::string sta = id.station >= 0 && static_cast<std::size_t>(id.station) < stations.size()
                                    ? stations[static_cast<std::size_t>(id.station)].name : std::string("none");
        const RingFixLink* L = nullptr;
        if (const auto it = rf.links.find(id); it != rf.links.end()) L = &it->second;
        traces.emitEvent(t, "RINGFIX", "keep_refused", id.sat, sta, std::nullopt, std::nullopt,
                         ringFixEntryVerdictName(v),
                         fmt::format("sig1={} sig2={} n1={} n2={}", id.sig1, id.sig2, L ? L->n1 : 0, L ? L->n2 : 0));
    }
    rf.keep_refusals.clear();
}

/// The election's outcomes of this epoch's moves as events, the applied moves into the watch, and every move the
/// election did not reach (its family not elected, or the satellite gone) counted as missed.
void reportUntiedOutcomes(NetworkRingFix& rf, const std::vector<NetStation>& stations, double now_s,
                          const GnssTime& t, NetworkRunTraces& traces) {
    const auto name = [&](int s) {
        return s >= 0 && static_cast<std::size_t>(s) < stations.size() ? stations[static_cast<std::size_t>(s)].name
                                                                       : std::string("none");
    };
    for (const RingFixUntiedOutcome& o : rf.untied_outcomes) {
        traces.emitEvent(t, "RINGFIX", o.applied ? "untied_applied" : "untied_missed", o.sat, name(o.station),
                         std::nullopt, std::nullopt, "chain",
                         fmt::format("family={} sig={} taken={}", o.family, o.signal, name(o.taken)));
        if (o.applied) rf.untied_moved_s[{o.family, o.sat}] = now_s;
    }
    rf.untied_outcomes.clear();
    for (const auto& [key, m] : rf.untied_moves) {
        ++rf.counters.untied_missed;
        traces.emitEvent(t, "RINGFIX", "untied_missed", key.second, name(m.station), std::nullopt, std::nullopt,
                         "chain", fmt::format("family={} sig={} taken=none (not elected)", m.family, key.first));
    }
    rf.untied_moves.clear();
}

/// One held family's re-attachment at one epoch, after its settled set: the watch on the moved satellites, the
/// dwell of the untied ones, and a move for every untied satellite past the dwell whose candidates agree.
/// @p rule_set is the settled rule's set before the wait (its components are the flagged piece), @p flagged the
/// set after the wait (the stream's flags).
void requestUntiedMoves(NetworkRingFix& rf, const std::string& ck, const std::string& s1, const std::string& s2,
                        const std::vector<std::pair<int, std::string>>& certified,
                        const std::map<std::string, int>& root_of_sat, const std::set<std::string>& rule_set,
                        const std::set<std::string>& flagged, const std::vector<RingFixDecideObs>& links,
                        const ChainEpochObs& obs, const std::vector<NetStation>& stations, double now_s,
                        const GnssTime& t, NetworkRunTraces& traces) {
    const std::string fam = ck + ":" + s1 + "/" + s2;
    for (auto it = rf.untied_moved_s.begin(); it != rf.untied_moved_s.end();) {
        if (it->first.first != fam) {
            ++it;
        } else if (flagged.count(it->first.second) != 0) {
            ++rf.counters.untied_flagged_after;
            it = rf.untied_moved_s.erase(it);
        } else if (now_s - it->second >= kRingFixReattachWatchS) {
            ++rf.counters.untied_unflagged_after;
            it = rf.untied_moved_s.erase(it);
        } else {
            ++it;
        }
    }
    // The ties and the flagged piece over the settled rule's graph (stations keep their roster index as id).
    arcgraph::Dsu dsu;
    std::map<std::string, int> sat_id;
    std::set<int> tied_stations;
    int next_id = 1 << 20;
    for (const auto& [sta, sat] : certified) {
        auto f = sat_id.find(sat);
        if (f == sat_id.end()) f = sat_id.emplace(sat, next_id++).first;
        arcgraph::dsuUnite(dsu, sta, f->second);
        tied_stations.insert(sta);
    }
    std::set<int> piece;
    for (const std::string& s : rule_set)
        if (const auto f = sat_id.find(s); f != sat_id.end()) piece.insert(arcgraph::dsuFind(dsu, f->second));
    const auto inPiece = [&](int sta) {
        return tied_stations.count(sta) != 0 && piece.count(arcgraph::dsuFind(dsu, sta)) != 0;
    };
    const BandParents p1 = bandParents(obs, ck, s1);
    const BandParents p2 = bandParents(obs, ck, s2);
    for (auto it = rf.untied_since_s.begin(); it != rf.untied_since_s.end();)
        it = it->first.first == fam && root_of_sat.count(it->first.second) == 0 ? rf.untied_since_s.erase(it)
                                                                                 : std::next(it);
    for (const auto& [sat, root] : root_of_sat) {
        (void)root;
        const std::pair<std::string, std::string> key{fam, sat};
        if (rule_set.count(sat) != 0 || sat_id.count(sat) != 0) {   // settled, or tied
            rf.untied_since_s.erase(key);
            continue;
        }
        auto since = rf.untied_since_s.emplace(key, now_s).first;
        if (now_s - since->second < kRingFixReattachDwellS) continue;
        if (const auto lm = rf.untied_last_move_s.find(key);
            lm != rf.untied_last_move_s.end() && now_s - lm->second < kRingFixReattachHoldoffS) {
            ++rf.counters.untied_held_off;   // moved within the hold-off: its links' window and persistence first
            continue;
        }
        std::vector<RingFixReattachCand> cands;
        for (const RingFixDecideObs& o : links) {
            if (o.id.sat != sat || o.id.sig1 != s1 || o.id.sig2 != s2 || !o.have_float) continue;
            if (!inPiece(o.id.station) || hangsBelow(p1, o.id.station, sat) || hangsBelow(p2, o.id.station, sat))
                continue;
            const auto L = rf.links.find(o.id);
            if (L == rf.links.end()) continue;
            const auto lt = rf.last_tree_s.find({o.id.station, o.id.sat});
            const double last_tree =
                lt == rf.last_tree_s.end() ? -std::numeric_limits<double>::infinity() : lt->second;
            if (!ringFixWindowOk(L->second, o, last_tree, now_s)) continue;
            const RingFixGate g = ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2);
            if (!g.valid || g.sigma_if > kRingFixMaxSigmaCyc) continue;
            cands.push_back({o.id.station, g.n_if - static_cast<double>(g.n1),
                             (o.f1 - o.f2) - static_cast<double>(g.wl)});
        }
        const RingFixReattachPick pk = ringFixReattachChoice(cands);
        if (!pk.ok) {
            ++(pk.n < kRingFixReattachMinLinks ? rf.counters.untied_few
               : pk.converging                 ? rf.counters.untied_converging
                                               : rf.counters.untied_spread);
            continue;
        }
        const int station = cands[static_cast<std::size_t>(pk.index)].station;
        rf.untied_moves[{s1, sat}] = RingFixUntiedMove{station, fam};
        rf.untied_moves[{s2, sat}] = RingFixUntiedMove{station, fam};
        since->second = now_s;   // the dwell restarts after a move
        rf.untied_last_move_s[key] = now_s;
        ++rf.counters.untied_requests;
        const auto host = [&](const BandParents& p) {
            const auto h = p.sat_parent.find(sat);
            return h == p.sat_parent.end() ? std::string("-") : stations[static_cast<std::size_t>(h->second)].name;
        };
        traces.emitEvent(t, "RINGFIX", "untied_move", sat, stations[static_cast<std::size_t>(station)].name, std::nullopt,
                         std::nullopt, "chain",
                         fmt::format("family={} from={}/{} n={} med_if={:+.3f} med_wl={:+.3f} sd_if={:.3f} "
                                     "sd_wl={:.3f}",
                                     fam, host(p1), host(p2), pk.n, pk.med_if, pk.med_wl, pk.sd_if, pk.sd_wl));
    }
}

/// One family's settled set before and after the wait (settledSetOf).
struct SettledPick {
    std::set<std::string> rule_set;   ///< The rule's set (root or largest), before the wait.
    std::set<std::string> settled;    ///< The flagged set, after the wait.
    bool      switched{false};        ///< Rule largest: another component replaced a non-empty incumbent.
    long long side_changes{0};        ///< Rule largest, at a switch: the satellites in one of the two sets only.
};

/// The settled set of one family, the one rule of the pair and the cascade families: its tree's satellites tied to their
/// root (rule root) or the largest tied set (rule largest, followed through the incumbent under @p key), then the wait
/// on the raise (network.ring_fix.settled_wait_emissions, its run counts under @p key).
SettledPick settledSetOf(NetworkRingFix& rf, const std::string& key, const std::vector<std::pair<int, std::string>>& ties,
                         const std::map<std::string, int>& root_of_sat) {
    SettledPick out;
    if (!rf.settled_largest) {
        out.rule_set = ringFixSettledSats(ties, root_of_sat);
    } else {
        std::set<std::string> tree_sats;
        for (const auto& [sat, root] : root_of_sat) { (void)root; tree_sats.insert(sat); }
        std::set<std::string>& inc = rf.settled_incumbent[key];
        RingFixLargestSet pick = ringFixLargestSettledSet(ties, tree_sats, inc, kRingFixSettledMinSats,
                                                          kRingFixSettledHysteresis);
        out.switched = pick.switched;
        out.side_changes = pick.side_changes;
        inc = pick.sats;
        out.rule_set = std::move(pick.sats);
    }
    out.settled = rf.settled_wait > 0 ? ringFixSettledWait(out.rule_set, rf.settled_run[key], rf.settled_wait)
                                      : out.rule_set;
    return out;
}

// ---- The cascades (network.ring_fix.cascade) ----

/// The cascade rows of one station: one per cascade and pair link held in this epoch's solve whose third and
/// reference signals have columns at the station and whose third band's arc is no tree edge, with the cascade and
/// the cascade link's identity beside each row (@p meta).
void cascadeStationRows(const NetworkRingFix& rf, int ii, const std::string& station_name, const ChainAmbColumns& cols,
                        const std::map<RingFixLinkId, bool>& held_before, const std::set<net::ArcTuple>& tree_arcs,
                        std::vector<ChainChordRow>& rows,
                        std::vector<std::pair<const RingFixCascade*, RingFixLinkId>>& meta) {
    for (const RingFixCascade& c : rf.cascades)
        for (const auto& [pid, P] : rf.links) {
            if (pid.station != ii || pid.sig1 != c.s1 || pid.sig2 != c.s2 || !P.held) continue;
            if (pid.sat.empty() || pid.sat[0] != c.letter) continue;
            const auto hb = held_before.find(pid);
            if (hb == held_before.end() || !hb->second) continue;   // held in this epoch's solve
            if (cols.count({pid.sat, c.s3}) == 0 || cols.count({pid.sat, c.ref}) == 0) continue;
            if (tree_arcs.count({ii, pid.sat, c.s3}) != 0) continue;   // a tree edge: the chain pins it
            ChainChordRow r;
            r.family = c.name;
            r.sig1 = c.ref;
            r.sig2 = c.s3;
            r.station = ii;
            r.station_name = station_name;
            r.sat = pid.sat;
            rows.push_back(r);
            meta.emplace_back(&c, RingFixLinkId{ii, pid.sat, c.ref, c.s3});
        }
}

/// Held cascade links not read this epoch: a tree edge of the third band leaves the held set (the chain pins it),
/// any other is absent (its gap runs).
void cascadeLinksNotRead(NetworkRingFix& rf, NetworkRunTraces& traces, const std::vector<NetStation>& stations,
                         const GnssTime& t, double now_s, const std::set<net::ArcTuple>& tree_arcs,
                         const std::set<RingFixLinkId>& cascade_seen) {
    for (auto& [id, CL] : rf.cascade_links) {
        if (!CL.held || cascade_seen.count(id) != 0) continue;
        const RingFixCascade* c = rf.cascadeOf(id);
        if (c == nullptr) continue;
        if (tree_arcs.count({id.station, id.sat, id.sig2}) != 0) {
            CL.held = false;
            ++rf.cascade_counters[c->name].to_tree;
            if (rf.settled_flags_on) rf.certified_edges.insert({id.station, id.sat, id.sig2});
            traces.emitEvent(t, "RINGFIX", "cascade_to_tree", id.sat, stations[static_cast<std::size_t>(id.station)].name,
                             std::nullopt, std::nullopt, "chain",
                             fmt::format("cascade={} n3={}", c->name, CL.n3));
            continue;
        }
        if (CL.absent_since_s < 0.0) CL.absent_since_s = now_s;
    }
}

}  // namespace

void networkRingFixDecideReports(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the untied moves and outcomes, the refused keeps
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX untied and keep_refused events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    if (rf.reattach) reportUntiedOutcomes(rf, stations, now_s, t, traces);
    if (!rf.keep_refusals.empty()) reportKeepRefusals(rf, stations, t, traces);
}

RingFixDecideChain networkRingFixDecideChain(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the last tree times
    const ChainEpochObs& obs = *ep.events.phase_datum.chain;
    const RelevelEpochState& relevel = ep.events.slip.relevel;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    RingFixDecideChain chain;
    std::set<net::ArcTuple>& tree_arcs = chain.tree_arcs;
    std::set<std::string>& chain_signals = chain.chain_signals;
    std::map<std::string, std::map<std::string, std::string>>& root_of = chain.root_of;
    std::map<net::ArcTuple, const ChainChordCycle*>& cyc_of = chain.cyc_of;
    std::map<std::string, RingFixTree> trees;
    for (const ChainEdgeObs& e : obs.edges) {
        tree_arcs.insert({e.station, e.sat, e.signal});
        chain_signals.insert(e.sys + "|" + e.signal);
        trees[e.group].edges.emplace_back(e.child, e.parent, e.depth);
        root_of[e.group][e.child] = e.r1;
        root_of[e.group][e.parent] = e.r1;
        rf.last_tree_s[{e.station, e.sat}] = now_s;
    }
    for (const ChainChordCycle& c : obs.chords) {
        cyc_of[{c.station, c.sat, c.signal}] = &c;
        trees[ringFixGroupOf(c.sat, c.signal)].chords.emplace_back(chainVertexKey(c.station), chainVertexKey(c.sat));
    }
    std::map<std::string, std::set<std::pair<std::string, std::string>>> held_by_group;
    for (const auto& [id, L] : rf.links)
        if (L.held)
            held_by_group[ringFixGroupOf(id.sat, id.sig1)].insert({chainVertexKey(id.station), chainVertexKey(id.sat)});
    for (const auto& [g, tree] : trees) chain.rings[g] = ringFixRings(tree, held_by_group[g]);
    chain.latched = ringFixLatchedArcs(relevel);
    return chain;
}

RingFixDecideReadings networkRingFixDecideReadings(NetworkRun& run, const NetworkEpoch& ep,
                                                   const RingFixDecideChain& chain) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the excluded-chord and audit-file counters
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const net::ArcTable& arc_rows = run.ctx.entities.arcs;
    const net::SatTable& sat_rows = run.ctx.entities.sats;
    const arcgraph::CycleLattice& lattice = run.ctx.carry.ar_lattice.lattice;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::set<net::ArcTuple>& tree_arcs = chain.tree_arcs;
    const std::set<std::string>& chain_signals = chain.chain_signals;
    const std::map<std::string, std::map<std::string, std::string>>& root_of = chain.root_of;
    const std::map<net::ArcTuple, const ChainChordCycle*>& cyc_of = chain.cyc_of;
    const auto& rings = chain.rings;
    const std::set<net::ArcTuple>& latched = chain.latched;
    // ---- End of the bindings ----
    RingFixDecideReadings out;
    std::vector<RingFixDecideObs>& links = out.links;
    std::map<RingFixLinkId, RingFixGate>& gate_of = out.gate_of;   // the trace's gate reading of every link with a float
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const int ii = static_cast<int>(i);
        const StationAssembly& R = asm_res[i];
        const ChainAmbColumns cols = chainAmbColumns(R);
        std::vector<ChainChordRow> rows = chainStationChordRows(ii, stations[i].name, cols, tree_arcs, chain_signals,
                                                                rf.form_configured_pairs ? &rf.configured_pairs : nullptr);
        if (rows.empty()) continue;
        std::vector<char> filled;
        Eigen::VectorXd xr;
        const bool station_ok = chainChordFloats(est, static_cast<std::size_t>(est_index_of[i]), x_s,
                                                 plan.shared_datum, cols, rows, filled, &xr);
        Eigen::VectorXd resid;
        bool resid_formed = false;
        // The near-null guard's code rows (network.ring_fix.nn_guard): (satellite, band digit) -> post-fit O - C (m).
        std::map<std::pair<std::string, char>, double> code_resid;
        bool code_formed = false;
        for (std::size_t r_i = 0; r_i < rows.size(); ++r_i) {
            const ChainChordRow& r = rows[r_i];
            if (!rf.pairConsidered(constellationKey(systemOfSat(r.sat)), r.sig1, r.sig2)) {
                ++rf.counters.excluded_link_epochs;   // never decided, held or traced
                continue;
            }
            RingFixDecideObs o;
            o.id = {ii, r.sat, r.sig1, r.sig2};
            o.lambda1 = chainWavelengthM(r.sat, r.sig1);
            o.lambda2 = chainWavelengthM(r.sat, r.sig2);
            o.have_float = station_ok && filled[r_i] != 0;
            if (o.have_float) {
                o.f1 = r.float_cyc_1; o.f2 = r.float_cyc_2;
                o.q11 = r.q11; o.q12 = r.q12; o.q22 = r.q22;
                gate_of[o.id] = ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2,
                                            rf.sigma_scale);
            }
            const std::string g = ringFixGroupOf(r.sat, r.sig1);
            const auto rg = rings.find(g);
            if (rg != rings.end()) {
                const auto ri = rg->second.find({chainVertexKey(ii), chainVertexKey(r.sat)});
                if (ri != rg->second.end()) { o.ring = ri->second.ring; o.candidate = ri->second.candidate; }
            }
            const auto rt = root_of.find(g);
            if (rt != root_of.end()) {
                const auto v = rt->second.find(chainVertexKey(ii));
                if (v != rt->second.end()) o.root = v->second;
            }
            const auto cy1 = cyc_of.find({ii, r.sat, r.sig1});
            if (cy1 != cyc_of.end()) o.have_lat1 = chainLatticeChordInteger(lattice, *cy1->second, o.lat1);
            const auto cy2 = cyc_of.find({ii, r.sat, r.sig2});
            if (cy2 != cyc_of.end()) o.have_lat2 = chainLatticeChordInteger(lattice, *cy2->second, o.lat2);
            if (rf.truth) {
                long long n1 = 0, n2 = 0;
                const auto trueOf = [&](const std::string& sig, long long& n) {
                    const auto cy = cyc_of.find({ii, r.sat, sig});
                    if (cy == cyc_of.end()) return false;
                    std::vector<std::tuple<int, std::string, std::string, int>> terms;
                    for (const arcgraph::CycleTerm& term : cy->second->cycle.terms)
                        terms.emplace_back(term.arc.station.index, term.arc.sat.str(), sig, term.coeff);
                    return ringFixTruthFrameInteger(*rf.truth, terms, cy->second->pin_sum_cyc,
                                                    cy->second->pins_integer, now_s, t.tow, n);
                };
                const bool h1 = trueOf(r.sig1, n1), h2 = trueOf(r.sig2, n2);
                o.have_truth = h1 && h2;
                o.truth1 = n1;
                o.truth2 = n2;
                if (!rf.audit.empty()) {
                    const auto check = [&](bool have, const std::string& sig, long long n) {
                        if (!have) return;
                        const auto f = rf.audit.find({stations[i].name, r.sat, sig});
                        if (f == rf.audit.end()) return;
                        ++rf.counters.truth_file_checked;
                        if (f->second != n) ++rf.counters.truth_file_mismatch;
                    };
                    check(h1, r.sig1, n1);
                    check(h2, r.sig2, n2);
                }
            }
            const LocalCarry* lc1 = net::getArc(arc_rows, net::ArcTuple{ii, r.sat, r.sig1}, &net::ArcState::amb);
            const LocalCarry* lc2 = net::getArc(arc_rows, net::ArcTuple{ii, r.sat, r.sig2}, &net::ArcState::amb);
            o.carry_ok = lc1 != nullptr && lc2 != nullptr;
            o.relevel1_s = lc1 != nullptr ? lc1->relevel_s : 0.0;
            o.relevel2_s = lc2 != nullptr ? lc2->relevel_s : 0.0;
            o.latched = latched.count({ii, r.sat, r.sig1}) > 0 || latched.count({ii, r.sat, r.sig2}) > 0;
            if (const double* ce = net::getSat(sat_rows, r.sat, &net::SatelliteState::code_ref_event_s))
                o.code_event_s = *ce;
            const auto held_it = rf.links.find(o.id);
            if (held_it != rf.links.end() && held_it->second.held && station_ok
                && R.A_s.cols() == x_s.size() && xr.size() == R.A_r.cols()) {
                if (!resid_formed) {
                    resid = R.y - R.A_s * x_s - R.A_r * xr;   // post-fit O - C
                    resid_formed = true;
                }
                const std::optional<double> r1 = phaseResidual(R, resid, cols.at({r.sat, r.sig1}).first);
                const std::optional<double> r2 = phaseResidual(R, resid, cols.at({r.sat, r.sig2}).first);
                if (r1 && r2 && o.lambda2 > o.lambda1 && o.lambda1 > 0.0) {
                    const double l1s = o.lambda1 * o.lambda1, l2s = o.lambda2 * o.lambda2;
                    o.have_resid = true;
                    o.resid_if_m = (l2s * *r1 - l1s * *r2) / (l2s - l1s);
                }
                if (rf.nn_guard || rf.entry_revalidate || rf.nn_monitor) {
                    if (!code_formed) {
                        for (Eigen::Index row = 0; row < resid.size(); ++row) {
                            const RowTag& tag = rowTagAt(R, row);
                            const double w = row < R.w.size() ? R.w(row) : 1.0;
                            if (rowClassOfKind(tag.kind, w) != NetRowClass::ObsCode || !tag.sat.valid()) continue;
                            const std::string code = rowObservationCode(tag);
                            if (code.size() < 2) continue;
                            code_resid.emplace(std::make_pair(tag.sat.str(), code[1]), resid(row));
                        }
                        code_formed = true;
                    }
                    const auto c1 = code_resid.find({r.sat, r.sig1.size() > 1 ? r.sig1[1] : '?'});
                    const auto c2 = code_resid.find({r.sat, r.sig2.size() > 1 ? r.sig2[1] : '?'});
                    if (c1 != code_resid.end() && c2 != code_resid.end() && o.lambda2 > o.lambda1 && o.lambda1 > 0.0) {
                        const double g = (o.lambda2 * o.lambda2) / (o.lambda1 * o.lambda1);   // f1^2 / f2^2
                        o.have_code = true;
                        o.code_gf_m = c1->second - c2->second;
                        o.code_if_m = (g * c1->second - c2->second) / (g - 1.0);
                    }
                }
            }
            links.push_back(o);
        }
    }
    return out;
}

RingFixDecideAudit networkRingFixDecideAudit(NetworkRun& run, const RingFixDecideReadings& readings) {
    // ---- From the run ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links' audit flags, the audit counters
    const std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<RingFixDecideObs>& links = readings.links;
    // ---- End of the bindings ----
    RingFixDecideAudit out;
    std::map<RingFixLinkId, RingFixAudit>& truth_of = out.truth_of;
    std::map<RingFixLinkId, RingFixAudit>& audit_of = out.audit_of;
    std::map<RingFixLinkId, bool>& held_before = out.held_before;
    if (rf.truth) {
        std::map<RingFixLinkId, const RingFixDecideObs*> obs_of;
        for (const RingFixDecideObs& o : links) obs_of[o.id] = &o;
        for (auto& [id, L] : rf.links) {
            if (!L.held) continue;
            const auto o = obs_of.find(id);
            if (o == obs_of.end() || !o->second->have_truth) { ++rf.counters.truth_none; continue; }
            const bool right = L.n1 == o->second->truth1 && L.n2 == o->second->truth2;
            truth_of[id] = right ? RingFixAudit::Right : RingFixAudit::Wrong;
            if (right) ++rf.counters.truth_right;
            else ++rf.counters.truth_wrong;
        }
    }
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        const std::string name = ringFixStationName(stations, id.station);
        const RingFixAudit a = rf.audit.empty() ? RingFixAudit::Unlisted
                                                : ringFixAuditLink(rf.audit, name, id, L.n1, L.n2);
        audit_of[id] = a;
        L.wrong_now = a == RingFixAudit::Wrong;
        if (a == RingFixAudit::Right) ++rf.counters.audit_right;
        else if (a == RingFixAudit::Wrong) ++rf.counters.audit_wrong;
        else ++rf.counters.audit_unlisted;
    }
    for (const RingFixDecideObs& o : links) {
        const auto it = rf.links.find(o.id);
        held_before[o.id] = it != rf.links.end() && it->second.held;
    }
    return out;
}

std::vector<RingFixEvent> networkRingFixDecideLinks(NetworkRun& run, const NetworkEpoch& ep,
                                                    const RingFixDecideReadings& readings) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links, the decision counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX hold and would_hold events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const std::vector<RingFixDecideObs>& links = readings.links;
    // ---- End of the bindings ----
    std::vector<RingFixEvent> events = ringFixDecideStep(rf, links, now_s);
    for (const RingFixEvent& ev : events) {
        if (ev.kind != RingFixEvent::Kind::Hold) continue;
        const std::string name = ringFixStationName(stations, ev.id.station);
        const RingFixAudit a = rf.audit.empty() ? RingFixAudit::Unlisted
                                                : ringFixAuditLink(rf.audit, name, ev.id, ev.gate.n1, ev.gate.n2);
        if (!ev.log_only) {
            if (a == RingFixAudit::Wrong) ++rf.counters.decided_wrong;
            else if (a == RingFixAudit::Right) ++rf.counters.decided_right;
        }
        std::string truth_tok;
        if (rf.truth) {
            for (const RingFixDecideObs& o : links)
                if (o.id < ev.id || ev.id < o.id) continue;
                else {
                    const bool off = o.have_truth && (o.truth1 != ev.gate.n1 || o.truth2 != ev.gate.n2);
                    if (ev.log_only) {
                        if (o.have_truth) ++(off ? rf.counters.log_truth_wrong : rf.counters.log_truth_right);
                    } else if (off) {
                        ++rf.counters.decided_truth_wrong;
                    }
                    truth_tok = o.have_truth ? ((o.truth1 == ev.gate.n1 && o.truth2 == ev.gate.n2) ? " truth=right"
                                                                                                 : " truth=WRONG")
                                             : " truth=none";
                    break;
                }
        }
        traces.emitEvent(t, "RINGFIX", ev.log_only ? "would_hold" : "hold", ev.id.sat, name, ev.gate.sigma_if,
                         kRingFixMaxSigmaCyc, ev.oracle ? "oracle" : "decide",
                         fmt::format("sig1={} sig2={} ring={} n1={} n2={} wl={} frac_if={:.3f} sigma_if={:.3f} "
                                     "frac_wl={:.3f} rule={} audit={}",
                                     ev.id.sig1, ev.id.sig2, rf.links.at(ev.id).ring, ev.gate.n1, ev.gate.n2,
                                     ev.gate.wl, ev.gate.frac_if, ev.gate.sigma_if, ev.gate.frac_wl,
                                     ringFixRuleName(rf.rule), auditName(a))
                             + (rf.order == RingFixOrder::Certain ? " order=certain" : "")
                             + (rf.gate_rule == RingFixGateRule::Odds
                                    ? fmt::format(" gate=odds log_odds_if={:.2f} log_odds_wl={:.2f}",
                                                  ev.gate.log_odds_if, ev.gate.log_odds_wl)
                                    : std::string())
                             + truth_tok);
    }
    return events;
}

RingFixDecideCascades networkRingFixDecideCascades(NetworkRun& run, const NetworkEpoch& ep,
                                                   const RingFixDecideChain& chain, const RingFixDecideAudit& audit) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the cascade links and counters, the certified edges
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX cascade_hold and cascade_to_tree events
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const net::ArcTable& arc_rows = run.ctx.entities.arcs;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::set<net::ArcTuple>& tree_arcs = chain.tree_arcs;
    const std::map<net::ArcTuple, const ChainChordCycle*>& cyc_of = chain.cyc_of;
    const std::map<RingFixLinkId, bool>& held_before = audit.held_before;
    // ---- End of the bindings ----
    RingFixDecideCascades out;
    std::vector<RingFixCascadeTraceRow>& cascade_trace = out.cascade_trace;
    std::set<RingFixLinkId>& cascade_seen = out.cascade_seen;
    if (rf.cascades.empty()) return out;
    const auto trueOf3 = [&](int sta, const std::string& sat, const std::string& sig, long long& n) {
        const auto cy = cyc_of.find({sta, sat, sig});
        if (!rf.truth || cy == cyc_of.end()) return false;
        std::vector<std::tuple<int, std::string, std::string, int>> terms;
        for (const arcgraph::CycleTerm& term : cy->second->cycle.terms)
            terms.emplace_back(term.arc.station.index, term.arc.sat.str(), sig, term.coeff);
        return ringFixTruthFrameInteger(*rf.truth, terms, cy->second->pin_sum_cyc, cy->second->pins_integer,
                                        now_s, t.tow, n);
    };
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const int ii = static_cast<int>(i);
        const StationAssembly& R = asm_res[i];
        const ChainAmbColumns cols = chainAmbColumns(R);
        std::vector<ChainChordRow> rows;
        std::vector<std::pair<const RingFixCascade*, RingFixLinkId>> meta;
        cascadeStationRows(rf, ii, stations[i].name, cols, held_before, tree_arcs, rows, meta);
        if (rows.empty()) continue;
        std::vector<char> filled;
        Eigen::VectorXd xr;
        const bool ok = chainChordFloats(est, static_cast<std::size_t>(est_index_of[i]), x_s, plan.shared_datum,
                                         cols, rows, filled, &xr);
        Eigen::VectorXd resid;
        bool resid_formed = false;
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const RingFixCascade& c = *meta[k].first;
            const RingFixLinkId& id = meta[k].second;
            cascade_seen.insert(id);
            RingFixCascadeLink& CL = rf.cascade_links[id];
            RingFixCascadeCounters& CC = rf.cascade_counters[c.name];
            const RingFixLink& P = rf.links.at(RingFixLinkId{ii, id.sat, c.s1, c.s2});
            RingFixCascadeTraceRow tr;
            tr.id = id;
            tr.n_ref = c.ref == c.s1 ? P.n1 : P.n2;
            CL.absent_since_s = -1.0;
            long long truth3 = 0;
            const bool have_truth = trueOf3(ii, id.sat, c.s3, truth3);
            tr.have_float = ok && filled[k] != 0;
            if (tr.have_float) {
                tr.f_ref = rows[k].float_cyc_1;
                tr.f3 = rows[k].float_cyc_2;
                tr.gate = ringFixCascadeGate(tr.f_ref, tr.f3, rows[k].q11, rows[k].q12, rows[k].q22, rf.sigma_scale);
            }
            if (CL.held) {
                ++CC.held_link_epochs;
                tr.state = "held";
                tr.n3 = CL.n3;
                tr.have_n3 = true;
                if (ok && R.A_s.cols() == x_s.size() && xr.size() == R.A_r.cols()) {
                    if (!resid_formed) {
                        resid = R.y - R.A_s * x_s - R.A_r * xr;   // post-fit O - C
                        resid_formed = true;
                    }
                    const std::optional<double> r3 = phaseResidual(R, resid, cols.at({id.sat, c.s3}).first);
                    if (r3) {
                        tr.have_resid = true;
                        tr.resid_m = *r3;
                        if (std::abs(*r3) > kRingFixMonitorBoundM) {
                            if (++CL.monitor_bad >= kRingFixMonitorEpochs) CL.monitor_trip = true;
                        } else {
                            CL.monitor_bad = 0;
                        }
                    }
                }
                if (have_truth) {
                    ++(truth3 == CL.n3 ? CC.truth_right : CC.truth_wrong);
                    tr.audit = truth3 == CL.n3 ? "right" : "WRONG";
                }
            } else if (!tr.have_float || !tr.gate.valid) {
                CL.streak = 0;
                ++CC.refused_no_float;
                tr.state = "refused";
                tr.cause = "no_float";
            } else if (!tr.gate.pass) {
                CL.streak = 0;
                const bool frac_out = tr.gate.frac > kRingFixCascadeMaxFracCyc;
                ++(frac_out ? CC.refused_fraction : CC.refused_odds);
                tr.state = "refused";
                tr.cause = frac_out ? "fraction" : "odds";
            } else {
                const long long n3 = tr.n_ref - tr.gate.d;
                if (CL.streak > 0 && CL.streak_n3 == n3) ++CL.streak;
                else { CL.streak = 1; CL.streak_n3 = n3; }
                tr.n3 = n3;
                tr.have_n3 = true;
                if (CL.streak >= kRingFixPersistEpochs && rf.cascade_log) {
                    // The log mode: the decision is traced at every epoch it stands, never held.
                    ++CC.would_hold;
                    if (have_truth && truth3 != n3) ++CC.would_truth_wrong;
                    tr.state = "would_hold";
                    if (have_truth) tr.audit = truth3 == n3 ? "right" : "WRONG";
                } else if (CL.streak >= kRingFixPersistEpochs) {
                    CL.held = true;
                    CL.n3 = n3;
                    CL.n_ref = tr.n_ref;
                    CL.lambda3 = chainWavelengthM(id.sat, c.s3);
                    CL.decided_s = now_s;
                    CL.streak = 0;
                    CL.monitor_bad = 0;
                    CL.monitor_trip = false;
                    const LocalCarry* lc3 = net::getArc(arc_rows, net::ArcTuple{ii, id.sat, c.s3}, &net::ArcState::amb);
                    CL.stamp3 = lc3 != nullptr ? lc3->relevel_s : 0.0;
                    ++CC.decided;
                    if (have_truth && truth3 != n3) ++CC.decided_truth_wrong;
                    tr.state = "decided";
                    if (have_truth) tr.audit = truth3 == n3 ? "right" : "WRONG";
                    traces.emitEvent(t, "RINGFIX", "cascade_hold", id.sat, stations[i].name, tr.gate.sigma,
                                     kRingFixMaxSigmaCyc, "cascade",
                                     fmt::format("cascade={} ref={} n_ref={} n3={} d={} frac={:.3f} sigma={:.3f} "
                                                 "log_odds={:.2f}{}", c.name, c.ref, tr.n_ref, n3, tr.gate.d,
                                                 tr.gate.frac, tr.gate.sigma, tr.gate.log_odds,
                                                 have_truth ? (truth3 == n3 ? " truth=right" : " truth=WRONG")
                                                            : std::string()));
                } else {
                    ++CC.refused_persist;
                    tr.state = "refused";
                    tr.cause = "persist";
                }
            }
            tr.streak = CL.streak;
            cascade_trace.push_back(std::move(tr));
        }
    }
    cascadeLinksNotRead(rf, traces, stations, t, now_s, tree_arcs, cascade_seen);
    return out;
}

void networkRingFixDecideTrace(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideReadings& readings,
                               const RingFixDecideAudit& audit, const std::vector<RingFixEvent>& decisions,
                               const RingFixDecideCascades& cascades) {
    // ---- From the run and the epoch ----
    const NetworkRingFix& rf = *run.ctx.carry.ring_fix;
    NetworkRunTraces& traces = run.traces;          // written: network_ring_fix.csv
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const std::vector<RingFixDecideObs>& links = readings.links;
    const std::map<RingFixLinkId, RingFixGate>& gate_of = readings.gate_of;
    const std::map<RingFixLinkId, RingFixAudit>& truth_of = audit.truth_of;
    const std::map<RingFixLinkId, RingFixAudit>& audit_of = audit.audit_of;
    const std::map<RingFixLinkId, bool>& held_before = audit.held_before;
    const std::vector<RingFixCascadeTraceRow>& cascade_trace = cascades.cascade_trace;
    // ---- End of the bindings ----
    if (!traces.ring_fix_trace.is_open()) return;
    std::map<RingFixLinkId, const RingFixEvent*> event_of;
    for (const RingFixEvent& ev : decisions) event_of[ev.id] = &ev;
    for (const RingFixDecideObs& o : links) {
        const RingFixLink& L = rf.links.at(o.id);
        const auto ev = event_of.find(o.id);
        const char* state = "refused";
        std::string cause;
        if (held_before.at(o.id)) { state = "held"; }
        else if (ev != event_of.end() && ev->second->kind == RingFixEvent::Kind::Hold) {
            state = ev->second->log_only ? "would_hold" : "decided";
        }
        else if (ev != event_of.end()) { cause = ringFixRefuseName(ev->second->refuse); }
        const auto g = gate_of.find(o.id);
        const RingFixGate gr = g != gate_of.end() ? g->second : RingFixGate{};
        const auto au = audit_of.find(o.id);
        const auto tr = truth_of.find(o.id);
        std::string audit_cell;
        if (tr != truth_of.end()) audit_cell = auditName(tr->second);
        else if (au != audit_of.end()) audit_cell = auditName(au->second);
        else if (state[0] == 'd' && !rf.audit.empty())
            audit_cell = auditName(ringFixAuditLink(rf.audit, stations[static_cast<std::size_t>(o.id.station)].name,
                                                    o.id, L.n1, L.n2));
        traces.ring_fix_trace
            << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
            << stations[static_cast<std::size_t>(o.id.station)].name << ',' << o.id.sat << ',' << o.id.sig1
            << ',' << o.id.sig2 << ',' << o.ring << ','
            << ((rf.order == RingFixOrder::Certain ? o.have_float : o.candidate) ? 1 : 0) << ',' << state << ','
            << cause << ',' << (o.have_float ? cellOf(o.f1, 4) : std::string()) << ','
            << (o.have_float ? cellOf(o.f2, 4) : std::string()) << ','
            << (gr.valid ? cellOf(gr.n_if, 4) : std::string()) << ','
            << (gr.valid ? cellOf(gr.sigma_if, 4) : std::string()) << ','
            << (gr.valid ? cellOf(gr.frac_if, 4) : std::string()) << ','
            << (gr.valid ? std::to_string(gr.wl) : std::string()) << ','
            << (gr.valid ? cellOf(gr.frac_wl, 4) : std::string()) << ','
            << ((L.held || gr.valid) ? std::to_string(L.held ? L.n1 : gr.n1) : std::string()) << ','
            << ((L.held || gr.valid) ? std::to_string(L.held ? L.n2 : gr.n2) : std::string()) << ','
            << (o.have_lat1 ? std::to_string(o.lat1) : std::string()) << ','
            << (o.have_lat2 ? std::to_string(o.lat2) : std::string()) << ','
            << (o.have_resid ? cellOf(o.resid_if_m, 5) : std::string()) << ',' << L.streak << ','
            << audit_cell << '\n';
    }
    for (const RingFixCascadeTraceRow& tr : cascade_trace) {
        traces.ring_fix_trace
            << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
            << stations[static_cast<std::size_t>(tr.id.station)].name << ',' << tr.id.sat << ',' << tr.id.sig1
            << ',' << tr.id.sig2 << ",-1,1," << tr.state << ',' << tr.cause << ','
            << (tr.have_float ? cellOf(tr.f_ref, 4) : std::string()) << ','
            << (tr.have_float ? cellOf(tr.f3, 4) : std::string()) << ",,"
            << (tr.gate.valid ? cellOf(tr.gate.sigma, 4) : std::string()) << ",,"
            << (tr.gate.valid ? std::to_string(tr.gate.d) : std::string()) << ','
            << (tr.gate.valid ? cellOf(tr.gate.frac, 4) : std::string()) << ','
            << std::to_string(tr.n_ref) << ','
            << (tr.have_n3 ? std::to_string(tr.n3) : std::string()) << ",,,"
            << (tr.have_resid ? cellOf(tr.resid_m, 5) : std::string()) << ',' << tr.streak << ','
            << tr.audit << '\n';
    }
}

void networkRingFixDecideSettled(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideChain& chain,
                                 const RingFixDecideReadings& readings, const RingFixDecideCascades& cascades) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the certified edges, the settled sets, waits and counters,
                                                    // the re-attachment's state
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX untied_move events
    const std::vector<NetStation>& stations = run.setup.stations;
    const ChainEpochObs& obs = *ep.events.phase_datum.chain;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const std::set<net::ArcTuple>& tree_arcs = chain.tree_arcs;
    const std::vector<RingFixDecideObs>& links = readings.links;
    const std::set<RingFixLinkId>& cascade_seen = cascades.cascade_seen;
    // ---- End of the bindings ----
    if (!rf.settled_flags_on) return;
    std::set<RingFixLinkId> considered;
    for (const RingFixDecideObs& o : links) considered.insert(o.id);
    for (auto it = rf.certified_edges.begin(); it != rf.certified_edges.end();)
        it = tree_arcs.count({std::get<0>(*it), std::get<1>(*it), std::get<2>(*it)}) ? std::next(it)
                                                                                : rf.certified_edges.erase(it);
    std::map<std::string, int> index_of;
    for (std::size_t i = 0; i < stations.size(); ++i) index_of[stations[i].name] = static_cast<int>(i);
    std::map<std::pair<std::string, std::string>, bool> now;
    for (const auto& [ck, s1, s2] : rf.pairs) {
        std::vector<std::pair<int, std::string>> certified;
        for (const auto& [id, L] : rf.links)
            if (L.held && considered.count(id) != 0 && id.sig1 == s1 && id.sig2 == s2
                && constellationKey(systemOfSat(id.sat)) == ck)
                certified.emplace_back(id.station, id.sat);
        for (const auto& [sta, sat, sig] : rf.certified_edges)
            if (sig == s1 && constellationKey(systemOfSat(sat)) == ck) certified.emplace_back(sta, sat);
        std::map<std::string, int> root_of_sat;
        for (const ChainEdgeObs& e : obs.edges) {
            if (e.signal != s1 || constellationKey(systemOfSat(e.sat)) != ck) continue;
            const auto r = index_of.find(e.r1);
            if (r != index_of.end()) root_of_sat[e.sat] = r->second;
        }
        const SettledPick sp = settledSetOf(rf, ck + ":" + s1 + "/" + s2, certified, root_of_sat);
        if (sp.switched) {
            ++rf.counters.settled_identity_changes;
            rf.counters.settled_side_changes += sp.side_changes;
        }
        if (rf.settled_wait > 0)
            rf.counters.settled_wait_held += 2 * static_cast<long long>(sp.rule_set.size() - sp.settled.size());
        const std::set<std::string>& settled = sp.settled;
        if (rf.reattach)
            requestUntiedMoves(rf, ck, s1, s2, certified, root_of_sat, sp.rule_set, settled, links, obs, stations,
                               now_s, t, traces);
        for (const auto& [sat, root] : root_of_sat) {
            (void)root;
            const bool f = settled.count(sat) != 0;
            for (const std::string& sig : {s1, s2}) {
                if (rf.flags_per_family) {
                    const auto nw = now.find({sat, sig});
                    if (nw == now.end()) now[{sat, sig}] = f;
                    else nw->second = nw->second && f;   // flagged only while every family flags it
                } else {
                    now[{sat, sig}] = f;
                }
                ++(f ? rf.counters.settled_signal_epochs : rf.counters.unsettled_signal_epochs);
                const auto p0 = rf.settled.find({sat, sig});
                if (p0 != rf.settled.end() && p0->second != f) ++rf.counters.settled_flips;
            }
        }
    }
    // The cascade families: the third signal's set from its held cascade links and the tree edges of its band that
    // entered as held cascade links, by the same rule and wait as the pairs'.
    for (const RingFixCascade& c : rf.cascades) {
        std::vector<std::pair<int, std::string>> certified;
        for (const auto& [id, CL] : rf.cascade_links)
            if (CL.held && cascade_seen.count(id) != 0 && id.sig1 == c.ref && id.sig2 == c.s3
                && !id.sat.empty() && id.sat[0] == c.letter)
                certified.emplace_back(id.station, id.sat);
        for (const auto& [sta, sat, sig] : rf.certified_edges)
            if (sig == c.s3 && !sat.empty() && sat[0] == c.letter) certified.emplace_back(sta, sat);
        std::map<std::string, int> root_of_sat;
        for (const ChainEdgeObs& e : obs.edges) {
            if (e.signal != c.s3 || e.sat.empty() || e.sat[0] != c.letter) continue;
            const auto r = index_of.find(e.r1);
            if (r != index_of.end()) root_of_sat[e.sat] = r->second;
        }
        const std::set<std::string> settled = settledSetOf(rf, c.name, certified, root_of_sat).settled;
        for (const auto& [sat, root] : root_of_sat) {
            (void)root;
            const bool f = settled.count(sat) != 0;
            const auto nw = now.find({sat, c.s3});
            if (nw == now.end()) now[{sat, c.s3}] = f;
            else nw->second = nw->second && f;
            ++(f ? rf.counters.settled_signal_epochs : rf.counters.unsettled_signal_epochs);
        }
    }
    rf.settled = std::move(now);
}

void networkRingFixDecideNearNull(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the moved links, the re-opens, the guard counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX nn_move and nn_release events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    // ---- End of the bindings ----
    if (!rf.nn_guard) return;
    const std::vector<RingFixNearNullIn> nn_in = ringFixNearNullInputs(rf);
    for (const RingFixNearNullVerdict& v : ringFixNearNullVerdicts(nn_in, rf.nn_tol_gf_m, rf.nn_max_if_m)) {
        ++rf.counters.nn_link_checks;
        if (v.refused_if) ++rf.counters.nn_refused_if;
        if (!v.act) continue;
        ++rf.counters.nn_flagged;
        RingFixLink& L = rf.links.at(v.id);
        // The release form (network.ring_fix.nn_guard_release) releases at the first flag; the move form at a
        // second flag during the same hold.
        if (L.nn_moved || rf.nn_guard_release) {
            L.monitor_trip = true;   // the next release step releases it, before PASS B
            ++rf.counters.nn_releases;
            traces.emitEvent(t, "RINGFIX", "nn_release", v.id.sat, ringFixStationName(stations, v.id.station),
                             v.dev_gf_m, rf.nn_tol_gf_m, "release",
                             fmt::format("sig1={} sig2={} n1={} n2={} k={} dev_gf={:.3f} dev_if={:.3f}", v.id.sig1,
                                         v.id.sig2, L.n1, L.n2, v.k, v.dev_gf_m, v.dev_if_m));
            continue;
        }
        const long long o1 = L.n1, o2 = L.n2;
        L.n1 -= 3LL * v.k;
        L.n2 -= 4LL * v.k;
        L.nn_moved = true;
        L.nn_n = 0;
        L.nn_gf_m = 0.0;
        L.nn_if_m = 0.0;
        rf.nn_reopen.push_back(v.id);
        ++rf.counters.nn_moves;
        if (std::abs(v.k) == 1) ++rf.counters.nn_moves_one;
        traces.emitEvent(t, "RINGFIX", "nn_move", v.id.sat, ringFixStationName(stations, v.id.station), v.dev_gf_m,
                         rf.nn_tol_gf_m, "move",
                         fmt::format("sig1={} sig2={} n1={}->{} n2={}->{} k={} dev_gf={:.3f} dev_if={:.3f}", v.id.sig1,
                                     v.id.sig2, o1, L.n1, o2, L.n2, v.k, v.dev_gf_m, v.dev_if_m));
    }
}

} // namespace GnssProcessor
