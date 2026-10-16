/**
 * @file DatumGraph.cpp
 * @brief The datum graph census.  The design argument is in DatumGraph.hpp
 *        (docs/SPEC_datum_graph_census.md).
 *
 * Measure-only.  Nothing here writes a weight, a state, a datum row or a
 * decision: the file reads the epoch's registration sets and admitted support,
 * predicts a rank deficiency from topology alone, joins the prediction to the
 * spectral observation the solve glue handed forward, and writes rows, events
 * and counters.
 *
 * ## Order independence
 *
 * Every container walked here is either an ordered associative container or is
 * sorted before use, and the union-find result is reduced through a sort with an
 * explicit tie-break.  The census therefore produces the same CSV bytes for the
 * same epoch regardless of thread count and of the order in which the driver
 * assembled the stations, so the diagnostic is as reproducible as the solve.
 */

#include "network/solve/DatumGraph.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "network/ar/ArcGraph.hpp"   // arcgraph::Dsu (card AGT)
#include "filter/StateKeySchema.hpp"
#include "network/diag/RunTraces.hpp"

// The run-end reports at the end of this file (card NOM step TD1b):
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunTypes.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "network/driver/Epoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/Run.hpp"     // NetworkRun (card NOM3 step EP1)
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

#include "network/driver/Epoch.hpp"        // NetworkEpoch (card NOM3 step SG: the solve stages)
#include "network/driver/Run.hpp"          // NetworkRun
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum, NetworkPostFitHandoff
#include "network/diag/RunTraces.hpp"

namespace GnssProcessor {

namespace {

// ===========================================================================
//  Small shared helpers
// ===========================================================================

/// Constellation letter of a "SAT/FAMILY" identity, or of a bare satellite id.
/// Returns '?' when the identity does not open with a satellite id (a letter
/// followed by two digits) or when its letter names no constellation
/// (@c systemOfSat).  An `observed only` direction named by its column label rather
/// than by an identity lands there on purpose: its constellation was not parsed, so
/// assigning one would be a guess.
char consOfIdentity(const std::string& id) {
    if (id.size() < 3 || !std::isdigit(static_cast<unsigned char>(id[1]))
        || !std::isdigit(static_cast<unsigned char>(id[2])))
        return '?';
    return letterOfSat(id);
}

/// Join identity tokens for a single CSV field, `;` separated: the file's
/// delimiter is `,`, and `emitEvent` already replaces commas in details.  Tokens
/// beyond @p cap collapse to a `+N` token.
std::string joinIds(const std::vector<std::string>& v, std::size_t cap = 12) {
    std::string s;
    std::size_t n = 0;
    for (const auto& t : v) {
        if (n >= cap) {
            s += fmt::format(";+{}", v.size() - cap);
            break;
        }
        if (!s.empty()) s += ';';
        s += t;
        ++n;
    }
    return s;
}

/// Per-identity event rate limit, in processed epochs (the PREFIT / MDB /
/// block-Baarda convention).  A persistent finding re-emits its identity once per
/// holdoff instead of once at the start of the run.  A holdoff of 0 allows every
/// event.
bool holdoffAllows(NetworkRunContext& ctx, const std::string& key, long long seq,
                   long long holdoff) {
    auto& last = ctx.carry.dg_event_last;
    const auto it = last.find(key);
    if (it != last.end() && holdoff > 0 && (seq - it->second) < holdoff) return false;
    last[key] = seq;
    return true;
}

/// Render one constellation's component roster for the CSV, under the
/// "≤ 4 listed + count" contract (the count k is a column of its own and is
/// never truncated).
std::string renderComps(const NetDatumGraphConsCensus& t1,
                        const std::vector<NetStation>& stations) {
    std::string s;
    int listed = 0;
    for (const auto& c : t1.comps) {
        if (listed >= kDatumGraphCompsListed) {
            s += fmt::format(";+{} more",
                             static_cast<int>(t1.comps.size()) - kDatumGraphCompsListed);
            break;
        }
        if (!s.empty()) s += ';';
        std::string sta = "?";
        if (c.sample_station >= 0
            && static_cast<std::size_t>(c.sample_station) < stations.size())
            sta = stations[static_cast<std::size_t>(c.sample_station)].name;
        s += fmt::format("{}sta/{}sat@{}+{}", c.n_stations, c.n_sats, sta,
                         c.sample_sat.empty() ? "?" : c.sample_sat);
        ++listed;
    }
    return s;
}

} // namespace

// ===========================================================================
//  Tier 1: component multiplicity
// ===========================================================================

std::map<char, NetDatumGraphConsCensus>
netDatumGraphComponents(const std::vector<NetDatumGraphEdge>& edges) {
    // Per constellation: vertex ids for the stations and the satellites that
    // carry an edge.  A vertex with no edge belongs to no component; counting an
    // untracked satellite or a station that folded nothing would inflate k with
    // vertices that carry no datum.
    struct Build {
        arcgraph::Dsu              dsu;     // the shared union-find (card AGT)
        std::map<int, int>         sta_v;   // station index  -> vertex
        std::map<std::string, int> sat_v;   // satellite id   -> vertex
        int                        n_vertices{0};
        int                        n_edges{0};
    };
    std::map<char, Build> build;

    for (const auto& e : edges) {
        if (e.sat.empty() || e.station < 0) continue;
        Build& b = build[letterOfSat(e.sat)];
        auto its = b.sta_v.find(e.station);
        if (its == b.sta_v.end()) its = b.sta_v.emplace(e.station, b.n_vertices++).first;
        auto itv = b.sat_v.find(e.sat);
        if (itv == b.sat_v.end()) itv = b.sat_v.emplace(e.sat, b.n_vertices++).first;
        arcgraph::dsuUnite(b.dsu, its->second, itv->second);   // station, then satellite
        ++b.n_edges;
    }

    std::map<char, NetDatumGraphConsCensus> out;
    for (auto& [cons, b] : build) {
        NetDatumGraphConsCensus t1;
        t1.cons       = cons;
        t1.n_stations = static_cast<int>(b.sta_v.size());
        t1.n_sats     = static_cast<int>(b.sat_v.size());
        t1.n_edges    = b.n_edges;

        // Reduce to components.  Both source maps are ordered, so the first
        // station / satellite reaching a root is the smallest one in it and the
        // sample identities are deterministic without a second pass.  Every value
        // depends on the partition alone, never on which vertex is the root.
        std::map<int, NetDatumGraphComponent> comp;
        for (const auto& [sta, v] : b.sta_v) {
            NetDatumGraphComponent& c = comp[arcgraph::dsuFind(b.dsu, v)];
            ++c.n_stations;
            if (c.sample_station < 0) c.sample_station = sta;
        }
        for (const auto& [sat, v] : b.sat_v) {
            NetDatumGraphComponent& c = comp[arcgraph::dsuFind(b.dsu, v)];
            ++c.n_sats;
            if (c.sample_sat.empty()) c.sample_sat = sat;
        }
        t1.k = static_cast<int>(comp.size());
        t1.comps.reserve(comp.size());
        for (auto& [root, c] : comp) {
            (void)root;
            t1.comps.push_back(std::move(c));
        }
        std::sort(t1.comps.begin(), t1.comps.end(),
                  [](const NetDatumGraphComponent& a, const NetDatumGraphComponent& b2) {
                      const int sa = a.n_stations + a.n_sats;
                      const int sb = b2.n_stations + b2.n_sats;
                      if (sa != sb) return sa > sb;              // largest first
                      return a.sample_sat < b2.sample_sat;       // deterministic tie-break
                  });
        out.emplace(cons, std::move(t1));
    }
    return out;
}

// ===========================================================================
//  Tier 2: channel-family code support
// ===========================================================================

std::string netDatumGraphFamily(std::string_view signal) {
    if (signal.size() < 3) return kDatumGraphNoFamily;
    const unsigned char a = static_cast<unsigned char>(signal[2]);
    if (!std::isalpha(a)) return kDatumGraphNoFamily;
    return std::string(1, signal[2]);
}

std::vector<NetDatumGraphPrediction> netDatumGraphChannelFamilies(
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::map<std::string, int>& code_support) {
    // Accumulate every (sat, family) cell touched by either registration set.
    // Cells with no phase bias cannot host a b<->I trade and are dropped at the
    // end, but they are accumulated first because a cell's code count and its
    // phase count arrive from different sources.
    std::map<std::string, NetDatumGraphPrediction> cell;
    std::map<System, int> cb_by_cons;   // the code-bias layer precondition

    const auto touch = [&](const std::string& sat, const std::string& family)
        -> NetDatumGraphPrediction& {
        const std::string key = netDatumGraphSupportKey(sat, family);
        auto it = cell.find(key);
        if (it == cell.end()) {
            NetDatumGraphPrediction p;
            p.sat    = sat;
            p.family = family;
            it = cell.emplace(key, std::move(p)).first;
        }
        return it->second;
    };

    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts || parts->satellite.empty()) continue;   // never guess a key
        ++touch(parts->satellite, netDatumGraphFamily(parts->signal)).n_pb;
    }
    for (const auto& [sat, sig] : registered_code_keys) {
        if (sat.empty()) continue;
        ++touch(sat, netDatumGraphFamily(sig)).n_cb;
        ++cb_by_cons[systemOfSat(sat)];
    }

    std::vector<NetDatumGraphPrediction> out;
    out.reserve(cell.size());
    for (auto& [key, p] : cell) {
        if (p.n_pb <= 0) continue;                          // not a cell of this census
        const auto sup = code_support.find(key);
        p.n_code_support = (sup == code_support.end()) ? 0 : sup->second;
        p.cb_layer       = !p.sat.empty() && cb_by_cons[systemOfSat(p.sat)] > 0;
        // Both predicates need the code-bias layer: with no code column anywhere
        // in the constellation the code observations pin the slant ionosphere
        // directly and the trade direction does not exist, while the literal
        // predicate would fire on every cell in the run.
        p.zero_keys    = p.cb_layer && p.n_cb == 0;
        p.zero_support = p.cb_layer && p.n_code_support == 0;
        out.push_back(std::move(p));
    }
    // `cell` is ordered by "SAT|FAMILY", which is the same order as
    // "SAT/FAMILY" for every id built here, so `out` is already ascending by
    // id().  The unit suite asserts this.
    return out;
}

// ===========================================================================
//  The spectral side
// ===========================================================================

std::vector<std::string> netDatumGraphDetailIds(std::string_view detail) {
    std::set<std::string> ids;
    std::size_t pos = 0;
    while (pos <= detail.size()) {
        const std::size_t comma = detail.find(',', pos);
        const std::string_view tok =
            detail.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                              : comma - pos);
        pos = (comma == std::string_view::npos) ? detail.size() + 1 : comma + 1;
        if (tok.empty()) continue;
        // "LABEL:span": the label itself contains colons ("PB:E03:L5X"), so the
        // span is what follows the last colon.
        const std::size_t last = tok.rfind(':');
        if (last == std::string_view::npos) continue;
        const std::string_view label = tok.substr(0, last);
        // Only the two bias kinds carry a signal, and therefore a family; a
        // clock or SH participant contributes no identity.
        if (label.rfind("PB:", 0) != 0 && label.rfind("CB:", 0) != 0) continue;
        const std::size_t p1 = label.find(':');
        const std::size_t p2 = label.find(':', p1 + 1);
        if (p2 == std::string_view::npos) continue;
        const std::string_view sat = label.substr(p1 + 1, p2 - p1 - 1);
        const std::string_view sig = label.substr(p2 + 1);
        if (sat.empty()) continue;
        ids.insert(std::string(sat) + "/" + netDatumGraphFamily(sig));
    }
    return std::vector<std::string>(ids.begin(), ids.end());
}

// ===========================================================================
//  Reconciliation (INC-1)
// ===========================================================================

NetDatumGraphContingency netDatumGraphReconcile(
    const std::vector<std::string>& predicted_ids,
    const std::vector<NetDatumGraphDir>& dirs) {
    NetDatumGraphContingency out;
    // std::set gives both the dedup and the ascending order that the two
    // prediction lists need.
    const std::set<std::string> pred(predicted_ids.begin(), predicted_ids.end());
    std::set<std::string> matched;

    for (const auto& d : dirs) {
        bool any = false;
        for (const auto& id : d.ids) {
            if (!pred.count(id)) continue;
            matched.insert(id);
            any = true;
        }
        if (any) {
            ++out.n_obs_matched;
            continue;
        }
        // Observed only.  Name the direction by its own identities when it has
        // them (they matched no prediction) and by its heaviest column when it
        // has none, so that each entry in this cell, which shows the limit of
        // the predictor, can be read.
        out.obs_only.push_back(d.ids.empty()
                                   ? (d.top_label.empty() ? std::string("unnamed")
                                                          : d.top_label)
                                   : joinIds(d.ids, 4));
    }
    for (const auto& id : pred) {
        if (matched.count(id)) out.pred_observed.push_back(id);
        else                   out.pred_only.push_back(id);
    }
    std::sort(out.obs_only.begin(), out.obs_only.end());
    return out;
}

// ===========================================================================
//  The seam
// ===========================================================================

void networkDatumGraphCensus(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<std::vector<PppObservation>>& modeled = ep.pass_a.modeled;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys =
        ep.columns.registered_code_keys;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    bool datum_changed = ep.solve.datum_changed;
    // ---- End of the bindings ----
    if (!ctx.knobs.dg_on) return;
    const auto t_begin = std::chrono::steady_clock::now();

    auto&           ctr     = ctx.counters.dg;
    const long long seq     = static_cast<long long>(ctx.counters.n_loop_epochs);
    const long long holdoff = ctx.knobs.dg_event_holdoff;
    const bool      kp_on   = config_.networkCbKaulaPrior();
    const NetDatumGraphSpectral& spec = ctx.carry.dg_spec;
    ++ctr.epochs;

    // --- the era boundary ---------------------------------------------------
    // A datum re-reference retires the frame every carried quantity is expressed
    // in (SBC re-seeds its continuity targets there for the same reason), so the
    // `predicted only` roster starts over.  Each era then reports its own
    // identities (the KP-load-bearing map when KP is armed) instead of each
    // identity appearing once per run, and the map's stability across the day
    // (spec §6.4) can be read.
    if (datum_changed) ctx.carry.dg_pred_only_era.clear();

    // ------------------------------------------------------------------------
    //  Support: the admitted (station, satellite) edges and the per-cell code
    //  support, in one pass over `modeled`.  The header banner explains why this
    //  source is used and not the assembled row stacks.
    // ------------------------------------------------------------------------
    std::set<std::string> reg_cb;
    for (const auto& [sat, sig] : registered_code_keys)
        reg_cb.insert(netDatumGraphSupportKey(sat, sig));

    std::vector<NetDatumGraphEdge> edges;
    std::map<std::string, int>     code_support;
    edges.reserve(modeled.size() * 24);
    for (std::size_t i = 0; i < modeled.size(); ++i) {
        // A station that did not fold into the normals this epoch contributes no
        // information to the reduced system and therefore no edge: its
        // observations exist, but no datum rests on them.
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        for (const auto& ob : modeled[i]) {
            if (ob.satellite.empty()) continue;
            bool supported = false;
            for (const auto& kv : ob.observed) {
                const std::string& code = kv.first;
                // An observable with no modeled counterpart produces no row; the
                // measurement model would have nothing to difference against.
                if (!ob.modeled.count(code)) continue;
                supported = true;
                if (obsTypeOf(code) != ObsType::Code) continue;
                // Only observables backing a registered code column count as
                // support; that restriction makes zero_keys imply zero_support
                // exactly (DatumGraph.hpp).
                if (!reg_cb.count(netDatumGraphSupportKey(ob.satellite, code))) continue;
                ++code_support[netDatumGraphSupportKey(ob.satellite,
                                                       netDatumGraphFamily(code))];
            }
            if (supported) edges.push_back(NetDatumGraphEdge{static_cast<int>(i),
                                                             ob.satellite});
        }
    }
    ctr.edges += static_cast<long long>(edges.size());

    // ------------------------------------------------------------------------
    //  Tier 1: component multiplicity, and the per-constellation gauge count d
    // ------------------------------------------------------------------------
    const std::map<char, NetDatumGraphConsCensus> tier1 = netDatumGraphComponents(edges);

    std::map<char, int> d_of;
    const char* datum_mode = "pin";
    if (!plan.clock_pivot.empty()) {
        // Pin mode: one satellite-clock pin per constellation, and
        // declared_dimension counts them.
        for (const auto& [cons, sat] : plan.clock_pivot) {
            (void)sat;
            if (!cons.empty()) d_of[rinexLetter(systemFromName(cons))] = 1;
        }
    } else if (config_.networkClockDatumBroadcastAnchored()) {
        // Every satellite carries its own anchor, so a disjoint component is
        // still gauged and fragmentation predicts nothing extra.  d = 0 is a
        // statement about this datum, not an omission.
        datum_mode = "brdc_anchor";
    } else {
        // Sum-to-zero (every remaining case): one all-ones gauge row per
        // constellation, folded before the gate.  The multiplicity statement is
        // the same as in pin mode (one gauge per constellation, assumed to serve
        // one component) although declared_dimension reads 0; the loader warns
        // about this when the census is armed under the sum-to-zero datum.
        datum_mode = "sum_to_zero";
        for (const auto& s : registered_clock_sats)
            if (!s.empty()) d_of[letterOfSat(s)] = 1;
    }

    long long pred_k1_total = 0;
    int       max_k_epoch   = 0;
    for (const auto& [cons, t1] : tier1) {
        max_k_epoch = std::max(max_k_epoch, t1.k);
        const auto dit = d_of.find(cons);
        const int  d   = (dit == d_of.end()) ? 0 : dit->second;
        if (t1.k > 1) pred_k1_total += static_cast<long long>(t1.k - 1) * d;
    }
    ctr.max_k = std::max(ctr.max_k, max_k_epoch);
    ctr.pred_tier1 += pred_k1_total;
    if (max_k_epoch > 1) ++ctr.epochs_frag;

    // ------------------------------------------------------------------------
    //  Tier 2: channel-family code support
    // ------------------------------------------------------------------------
    const std::vector<NetDatumGraphPrediction> cells =
        netDatumGraphChannelFamilies(shared_phase_keys, registered_code_keys, code_support);
    ctr.cells_tier2 += static_cast<long long>(cells.size());

    std::vector<std::string> pred_ids, support_ids;
    for (const auto& c : cells) {
        if (!c.cb_layer) ++ctr.cells_no_cb_layer;
        if (c.zero_keys)    pred_ids.push_back(c.id());
        if (c.zero_support) support_ids.push_back(c.id());
    }
    ctr.pred_tier2   += static_cast<long long>(pred_ids.size());
    ctr.pred_support += static_cast<long long>(support_ids.size());

    // ------------------------------------------------------------------------
    //  Reconciliation (INC-1)
    // ------------------------------------------------------------------------
    const char* obs_src = "absent";
    if (!spec.valid) {
        // The epoch never reached a rank evaluation.  A gate withhold skips the
        // epoch before SB_FRAMEDIAG, so withheld epochs do not reach this seam at
        // all and are not counted here either; the teardown line reports that
        // gap against n_gate_evaluated.
        ++ctr.epochs_no_spectral;
    } else if (!spec.dirs.empty()) {
        obs_src = "sbc";
    } else if (spec.extras() > 0) {
        // Extras exist but nothing named them: SBC disarmed, or armed and
        // refused before it could attribute.  Reconciliation is count-level
        // only: every prediction then lands in `predicted only` for want of a
        // right-hand side, which reflects the arming rather than the predictor,
        // and the obs_src column records which case applies.
        obs_src = "count";
        ++ctr.epochs_count_only;
    } else {
        obs_src = spec.from_sbc ? "sbc" : "count";
    }
    if (spec.valid) ctr.obs_extras_count += spec.extras();
    ctr.obs_dirs += static_cast<long long>(spec.dirs.size());

    const NetDatumGraphContingency cont    = netDatumGraphReconcile(pred_ids, spec.dirs);
    const NetDatumGraphContingency cont_sp = netDatumGraphReconcile(support_ids, spec.dirs);
    ctr.cell_pred_obs    += static_cast<long long>(cont.pred_observed.size());
    ctr.cell_pred_only   += static_cast<long long>(cont.pred_only.size());
    ctr.cell_obs_only    += static_cast<long long>(cont.obs_only.size());
    ctr.cell_obs_matched += cont.n_obs_matched;
    ctr.support_pred_obs  += static_cast<long long>(cont_sp.pred_observed.size());
    ctr.support_pred_only += static_cast<long long>(cont_sp.pred_only.size());

    const bool quiet = (max_k_epoch <= 1) && pred_ids.empty() && support_ids.empty();
    if (quiet) ++ctr.epochs_quiet;

    // ------------------------------------------------------------------------
    //  Events, DATUMGRAPH tier (INC-2)
    // ------------------------------------------------------------------------
    const auto emit = [&](const char* event, const std::string& key,
                          const std::string& sat, std::optional<double> stat,
                          std::optional<double> thr, const char* action,
                          const std::string& detail) {
        if (!holdoffAllows(ctx, key, seq, holdoff)) { ++ctr.events_held; return; }
        ++ctr.events;
        traces.emitEvent(se.time, "DATUMGRAPH", event, sat, "", stat, thr, action, detail);
    };

    for (const auto& [cons, t1] : tier1) {
        if (t1.k <= 1) continue;
        const auto dit = d_of.find(cons);
        const int  d   = (dit == d_of.end()) ? 0 : dit->second;
        emit("dg_fragmented", fmt::format("frag|{}", cons), "",
             static_cast<double>(t1.k), 1.0, "predict",
             fmt::format("cons={} k={} d={} predicted={} mode={} sta={} sat={} comps={}",
                         cons, t1.k, d, (t1.k - 1) * d, datum_mode, t1.n_stations,
                         t1.n_sats, renderComps(t1, stations)));
    }
    // `predicted and observed` and `observed only`: an identity with no holdoff
    // entry yet always emits, and a persisting one re-emits once per holdoff, so
    // its duration is visible without one event per epoch.
    for (const auto& id : cont.pred_observed)
        emit("dg_pred_observed", "po|" + id, id.substr(0, id.find('/')),
             std::nullopt, std::nullopt, "explain",
             fmt::format("id={} src={} observed={} declared={}", id, obs_src,
                         spec.observed, spec.declared));
    for (const auto& id : cont.obs_only)
        emit("dg_observed_only", "oo|" + id, "", std::nullopt, std::nullopt,
             "unexplained",
             fmt::format("id={} src={} observed={} declared={} predictions={}", id,
                         obs_src, spec.observed, spec.declared, pred_ids.size()));
    // `predicted only`: one event at the first sighting of each (sat, family) per
    // era, per spec §3, not subject to the holdoff.  With KP armed this is the
    // KP-load-bearing map; without it, over-prediction.
    for (const auto& id : cont.pred_only) {
        if (!ctx.carry.dg_pred_only_era.insert(id).second) continue;
        ++ctr.events;
        traces.emitEvent(se.time, "DATUMGRAPH", netDatumGraphPredOnlyEvent(kp_on),
                         id.substr(0, id.find('/')), "", std::nullopt, std::nullopt,
                         netDatumGraphPredOnlyAction(kp_on),
                         fmt::format("id={} src={} observed={} declared={} kp={}", id,
                                     obs_src, spec.observed, spec.declared,
                                     kp_on ? "on" : "off"));
    }

    // ------------------------------------------------------------------------
    //  network_datum_graph.csv, one row per epoch per constellation (INC-2)
    // ------------------------------------------------------------------------
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t_begin).count();
    ctr.ms += ms;
    ctr.ms_max = std::max(ctr.ms_max, ms);

    if (traces.datum_graph_trace.is_open()) {
        // Attribute every per-identity list to its constellation so one row can
        // be read on its own.  A constellation appears when tier 1 saw it or any
        // list names it; '?' collects the directions whose participants carry no
        // satellite (SH-only modes), so that they still appear in the trace.
        struct Row {
            const NetDatumGraphConsCensus* t1{nullptr};
            std::vector<std::string> pred, pred_obs, pred_only, obs_only, sup;
            int cells{0}, no_layer{0};
        };
        std::map<char, Row> rows;
        for (const auto& [cons, t1] : tier1) rows[cons].t1 = &t1;
        for (const auto& c : cells) {
            Row& r = rows[letterOfSat(c.sat)];
            ++r.cells;
            if (!c.cb_layer) ++r.no_layer;
            if (c.zero_keys)    r.pred.push_back(c.id());
            if (c.zero_support) r.sup.push_back(c.id());
        }
        for (const auto& id : cont.pred_observed) rows[consOfIdentity(id)].pred_obs.push_back(id);
        for (const auto& id : cont.pred_only)     rows[consOfIdentity(id)].pred_only.push_back(id);
        for (const auto& id : cont.obs_only)      rows[consOfIdentity(id)].obs_only.push_back(id);

        for (const auto& [cons, r] : rows) {
            const int k  = r.t1 ? r.t1->k : 0;
            const auto dit = d_of.find(cons);
            const int  d   = (dit == d_of.end()) ? 0 : dit->second;
            traces.datum_graph_trace
                << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow) << ','
                << cons << ',' << datum_mode << ',' << k << ',' << d << ','
                << (k > 1 ? (k - 1) * d : 0) << ','
                << (r.t1 ? r.t1->n_stations : 0) << ',' << (r.t1 ? r.t1->n_sats : 0) << ','
                << (r.t1 ? r.t1->n_edges : 0) << ','
                << (r.t1 ? renderComps(*r.t1, stations) : std::string{}) << ','
                << r.cells << ',' << r.no_layer << ','
                << r.pred.size() << ',' << r.sup.size() << ','
                << obs_src << ',' << spec.declared << ',' << spec.observed << ','
                << spec.extras() << ',' << spec.dirs.size() << ','
                << r.pred_obs.size() << ',' << r.pred_only.size() << ','
                << r.obs_only.size() << ',' << cont.n_obs_matched << ','
                << cont_sp.pred_observed.size() << ',' << cont_sp.pred_only.size() << ','
                << (kp_on ? 1 : 0) << ','
                << joinIds(r.pred_obs) << ',' << joinIds(r.pred_only) << ','
                << joinIds(r.obs_only) << ',' << fmt::format("{:.3f}", ms) << '\n';
        }
    }

    // ------------------------------------------------------------------------
    //  One-shot arming line (the wiring proof invariant #4 asks for at epoch 1,
    //  not only at run end).
    // ------------------------------------------------------------------------
    if (ctr.epochs == 1)
        log->info("Network (datum graph): census ARMED — TOW {:.1f} walked {} admitted "
                  "support edge(s) over {} constellation(s) (max k = {}, datum mode {}), "
                  "examined {} (sat, family) cell(s) and predicted {} tier-2 "
                  "direction(s) ({} on the companion support predicate).  Spectral side: "
                  "{} (observed {} vs declared {}).  MEASURE-ONLY: nothing here feeds the "
                  "gate, SBC or the plan.",
                  se.time.tow, edges.size(), tier1.size(), max_k_epoch, datum_mode,
                  cells.size(), pred_ids.size(), support_ids.size(), obs_src,
                  spec.observed, spec.declared);
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the datum graph census.
int networkDatumGraphRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // Datum graph census run-end telemetry (docs/SPEC_datum_graph_census.md).  The
    // reading expected when nothing is wrong, "k = 1 everywhere, zero predictions", is
    // also exactly what a seam that was never called produces (invariant #4).  So both
    // `epochs` and `epochs_quiet` are printed, and a run that walked zero support edges
    // is reported as a miswire rather than as a network with no deficiency.
    if (!ctx.knobs.dg_on) return 0;
    const auto& dg = ctx.counters.dg;
    log->info("Network (datum graph): census ran {} epoch(s) — {} quiet (k = 1 on "
              "every constellation AND zero tier-2 predictions), {} fragmented "
              "(max k = {}), {} admitted support edge(s) walked.  PREDICTED: {} "
              "tier-1 (k-1)*d deficiencies, {} tier-2 zero-code-key directions over "
              "{} (sat, family) cell(s) ({} cell(s) had no code-bias layer, both "
              "predicates held off); companion zero-support predicate {}.  OBSERVED: "
              "{} extra direction(s) by count, {} with identities.  CONTINGENCY — "
              "predicted AND observed {}, predicted ONLY {}, observed ONLY {} "
              "(directions matching a prediction: {}); companion cells {} / {}.  "
              "Events: {} emitted, {} held.  Cost {:.2f} ms total, {:.3f} ms worst "
              "epoch (spec budget 0.5, hard limit 2.0).  MEASURE-ONLY: no weight, "
              "state, gate, SBC or declared dimension was touched.",
              dg.epochs, dg.epochs_quiet, dg.epochs_frag, dg.max_k, dg.edges,
              dg.pred_tier1, dg.pred_tier2, dg.cells_tier2, dg.cells_no_cb_layer,
              dg.pred_support, dg.obs_extras_count, dg.obs_dirs,
              dg.cell_pred_obs, dg.cell_pred_only, dg.cell_obs_only,
              dg.cell_obs_matched, dg.support_pred_obs, dg.support_pred_only,
              dg.events, dg.events_held, dg.ms, dg.ms_max);
    if (dg.epochs == 0)
        log->warn("Network (datum graph): ARMED but never ran a single census pass — "
                  "the seam is not being reached (miswire), which is a different "
                  "finding from a quiet network.");
    else if (dg.edges == 0)
        log->warn("Network (datum graph): ran {} epoch(s) and walked ZERO admitted "
                  "support edges.  Tier 1 saw no graph at all, so its k = 1 reading "
                  "is vacuous — the support source (modeled observations of FOLDED "
                  "stations) is empty, not the network.",
                  dg.epochs);
    if (dg.epochs_count_only > 0)
        log->info("Network (datum graph): {} epoch(s) reconciled at COUNT level only "
                  "(extras observed, no identities) — arm network.sbasis_complete for "
                  "the per-direction attribution the contingency needs; without it "
                  "every prediction falls into `predicted only` for want of a "
                  "right-hand side.",
                  dg.epochs_count_only);
    if (static_cast<std::size_t>(std::max<long long>(dg.epochs, 0))
        < ctx.counters.n_gate_evaluated)
        log->info("Network (datum graph): {} of {} gate-evaluated epoch(s) never "
                  "reached the census — a WITHHELD epoch skips before SB_FRAMEDIAG, "
                  "so the epochs whose datum actually failed are precisely the ones "
                  "with no census row.  Read this line next to the Card-DG withhold "
                  "counts, not on its own.",
                  ctx.counters.n_gate_evaluated - static_cast<std::size_t>(dg.epochs),
                  ctx.counters.n_gate_evaluated);
    if (dg.ms_max > 2.0)
        log->warn("Network (datum graph): worst census pass {:.3f} ms exceeds the "
                  "card's 2.0 ms hard limit (§7) — the implementation is walking "
                  "something it should not.",
                  dg.ms_max);
    return 0;
}

// ---- End of the run-end reports ----

// ---- Solve stages (card NOM3 step SG; generated by scripts/nom_sg_cut.py) ----
/// The datum graph census's spectral half, with direction identities, before completion (card NOM3 step SG; Doxygen at the declaration).
void networkDatumGraphSpectralHandoff(NetworkRunContext& ctx, const NetworkSbasis::GaugeCompletion& sbc,
                                      const NetworkSbasis::Plan& plan) {
    // --- Datum graph census hand-off (docs/SPEC_datum_graph_census.md) -----
    // The spectral half of the census's contingency, with direction identities,
    // is captured here: this is the only place in the run where the realized
    // near-null count is measured before completion and its directions still
    // carry attribution.  The gate below runs on the completed system and would
    // report `observed == declared`, the completion's own success, which is not
    // the quantity the census reconciles against.  (Runs without SBC get a
    // count-only fallback after the gate.)
    //
    // Measure-only and armed-only: only the datum graph census reads
    // ctx.carry.dg_spec, so the solve is unaffected and an unarmed run pays one
    // predictable branch.  The identities come from parsing `d.detail`, the same
    // string that the sbc_engage / sbc_skip events carry, so the census and the
    // event stream agree on a direction's identity by construction rather than
    // through two derivations that could drift.  Both populations are recorded:
    // the constrained extras and the skipped orphans are equally "observed", and
    // dropping the orphans would empty the `observed only` cell, which measures
    // the limit of this predictor.
    if (ctx.knobs.dg_on) {
        auto& dgs = ctx.carry.dg_spec;
        dgs.valid    = true;
        dgs.from_sbc = true;
        dgs.observed = sbc.observed;
        dgs.declared = plan.declared_dimension;
        // Overwrite, never append: if a within-epoch adaptation ever re-enters
        // this body, the census must see this evaluation's directions, not the
        // union of every attempt.
        dgs.dirs.clear();
        const auto push = [&dgs](const NetworkSbasis::GaugeCompletion::Dir& d,
                                 bool skipped) {
            NetDatumGraphDir g;
            g.ev        = d.ev;
            g.skipped   = skipped;
            g.top_label = d.top_label;
            g.detail    = d.detail;
            g.ids       = netDatumGraphDetailIds(d.detail);
            dgs.dirs.push_back(std::move(g));
        };
        for (const auto& d : sbc.dirs)    push(d, false);
        for (const auto& d : sbc.skipped) push(d, true);
    }
}
// ---- End of the solve stages ----

} // namespace GnssProcessor
