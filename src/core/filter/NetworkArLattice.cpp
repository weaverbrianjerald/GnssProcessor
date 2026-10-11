/**
 * @file NetworkArLattice.cpp
 * @brief The ledger of fixed integer cycles (cards AGT and FCF): the cause table, the evidence,
 *        the one release function, the epoch's steps, the record of its two files, and its
 *        run-end report.  What it holds, its order and why it keeps no stamp of its own are in
 *        the header's file banner, include/filter/NetworkArLattice.hpp.
 */

#include "filter/NetworkArLattice.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.carry.ar_lattice, gl_act_*, ctx.counters.netar_lattice
#include "config/GnssConfig.hpp"         // network.ambiguity_resolution.trace_lattice, fixed_cycles.*
#include "filter/NetworkResponses.hpp"   // latchCauseName: the free-epoch latch's cause names
#include "filter/NetworkRunEnd.hpp"      // NetworkRunEndView
#include "filter/NetworkRunTypes.hpp"    // NetStation, WithinEpochCulprits
#include "logging/NetworkRunTraces.hpp"  // ar_lattice_trace, ar_lattice_events_trace
#include "math/RobustStats.hpp"          // RobustStats::median
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <string>

namespace GnssProcessor {

namespace {

using arcgraph::LatticeVerdict;

/// The cause table (one row per ArLatticeRelease, in enum order; unit-tested).
constexpr std::array<ArLatticeReleaseSpec, kArLatticeReleaseCount> kReleaseTable{{
    {ArLatticeRelease::Vanished, "vanished", "rel_vanished",
     "the search did not read the arc (it left the pool)"},
    {ArLatticeRelease::FirstSeen, "first_seen", "rel_first_seen",
     "the last search did not read it (a gap between two searches)"},
    {ArLatticeRelease::Freed, "freed", "rel_freed",
     "the free-epoch latch holds it (a slip response)"},
    {ArLatticeRelease::StampChange, "stamp", "rel_stamp",
     "its life stamp changed (an erase, a drop, a re-level, a flag)"},
    {ArLatticeRelease::ReplayCulprit, "replay", "rel_replay",
     "the landed within-epoch replay refloated or excluded its line of sight"},
    {ArLatticeRelease::GrossLos, "gross", "rel_gross",
     "the gross-LOS Decide declared its line of sight or its station"},
    {ArLatticeRelease::Contradiction, "contradiction", "rel_contradiction",
     "a fix implied another integer: the cycle's arcs released"},
    {ArLatticeRelease::Revalidation, "revalidation", "rel_revalidation",
     "its unit failed the float test: the arc two failing units share, else both cycles' arcs"},
    {ArLatticeRelease::Acceptance, "acceptance", "rel_acceptance",
     "its unit was dropped by held-solve block acceptance: both cycles' arcs released"},
    {ArLatticeRelease::Degenerate, "degenerate", "rel_degenerate",
     "its unit's block was refused by the kernel's screens: both cycles' arcs released"},
    {ArLatticeRelease::SeamGap, "seam_gap", "rel_seam_gap",
     "a loop epoch since the last step went without one: every row released"},
}};

/// The index of a verdict in the tallies (LatticeVerdict order).
std::size_t verdictIndex(LatticeVerdict v) { return static_cast<std::size_t>(v); }

/// The index of a release cause in the tallies.
std::size_t releaseIndex(ArLatticeRelease r) { return static_cast<std::size_t>(r); }

/// Every band-arc the rows hold, ascending.
std::set<ArcId> latticeArcs(const arcgraph::CycleLattice& L) {
    std::set<ArcId> arcs;
    for (const arcgraph::Cycle& row : L.rows)
        for (const arcgraph::CycleTerm& t : row.terms) arcs.insert(arcs.end(), t.arc);
    return arcs;
}

/// How the rows hold one arc: how many rows, and whether one holds it at +-1 (a unit row, which
/// eliminates it at the cost of one dimension).
struct ArcHold {
    int  rows{0};     ///< Rows that hold the arc.
    bool unit{false}; ///< One of them holds it at +-1.
};

/// The term of @p row on @p arc, or null (a row's terms are sorted by arc).
const arcgraph::CycleTerm* termOn(const arcgraph::Cycle& row, const ArcId& arc) {
    const auto it = std::lower_bound(row.terms.begin(), row.terms.end(), arc,
                                     [](const arcgraph::CycleTerm& t, const ArcId& a) { return t.arc < a; });
    return (it == row.terms.end() || !(it->arc == arc)) ? nullptr : &*it;
}

/// How the rows hold @p arc.
ArcHold holdOf(const arcgraph::CycleLattice& L, const ArcId& arc) {
    ArcHold h;
    for (const arcgraph::Cycle& row : L.rows) {
        const arcgraph::CycleTerm* t = termOn(row, arc);
        if (!t) continue;
        ++h.rows;
        if (t->coeff == 1 || t->coeff == -1) h.unit = true;
    }
    return h;
}

/// The rows that hold any of @p arcs.
int rowsThrough(const arcgraph::CycleLattice& L, const std::set<ArcId>& arcs) {
    int n = 0;
    for (const arcgraph::Cycle& row : L.rows)
        if (std::any_of(arcs.begin(), arcs.end(), [&](const ArcId& a) { return termOn(row, a) != nullptr; })) ++n;
    return n;
}

/// Eliminate one arc (@ref arcgraph::latticeEliminateArc), counting a hold with no unit row.
int eliminateArc(arcgraph::CycleLattice& L, const ArcId& arc, ArLatticeEpoch& e) {
    const ArcHold hold = holdOf(L, arc);
    if (hold.rows > 0 && !hold.unit) ++e.no_unit;
    return arcgraph::latticeEliminateArc(L, arc);
}

/// The release cause of a life reading that ends a life, or none (None, Echo: the life goes on).
std::optional<ArLatticeRelease> releaseOf(ArcLifeEdge edge) {
    switch (edge) {
    case ArcLifeEdge::FirstSeen:   return ArLatticeRelease::FirstSeen;
    case ArcLifeEdge::Freed:       return ArLatticeRelease::Freed;
    case ArcLifeEdge::StampChange: return ArLatticeRelease::StampChange;
    case ArcLifeEdge::None:
    case ArcLifeEdge::Echo:        return std::nullopt;
    }
    return std::nullopt;
}

/// A contradiction's release: the fix's cycle, its integer and the one the lattice implies.
ArLatticeReleaseEvent contradictionOf(const ArFixedCycle& f, long long implied) {
    ArLatticeReleaseEvent r;
    r.cause   = ArLatticeRelease::Contradiction;
    r.scope   = ArLatticeScope::Cycle;
    r.cycle   = f.cycle;
    r.dd      = f.id;
    r.offered = f.value;
    r.implied = implied;
    return r;
}

/// True when any of the DD's four arcs is in @p arcs.
bool throughAny(const ArDdIdentity& id, const std::set<ArcId>& arcs) {
    if (arcs.empty()) return false;
    for (const ArcId& a : arDdArcs(id))
        if (arcs.count(a)) return true;
    return false;
}

/// The lattice's size after an epoch: its dimension, the arcs it holds and its row lengths.
void latticeFigures(const arcgraph::CycleLattice& L, ArLatticeEpoch& e) {
    e.rows    = static_cast<int>(L.rows.size());
    e.arcs    = static_cast<int>(latticeArcs(L).size());
    e.max_len = 0;
    long long total = 0;
    for (const arcgraph::Cycle& row : L.rows) {
        const int len = static_cast<int>(row.terms.size());
        e.max_len = std::max(e.max_len, len);
        total += len;
    }
    e.mean_len = L.rows.empty() ? 0.0 : static_cast<double>(total) / static_cast<double>(L.rows.size());
}

/// A station's name by roster index ("?" outside the roster).
std::string stationName(const std::vector<NetStation>& stations, const StationId& s) {
    return (s.index >= 0 && static_cast<std::size_t>(s.index) < stations.size())
               ? stations[static_cast<std::size_t>(s.index)].name : std::string("?");
}

/// "cons,signal" of a band (the trace's two identity columns).
std::string bandColumns(const SignalId& sig) {
    return constellationKey(sig.sys) + "," + sig.obsCode(ObsType::Phase);
}

/// The epoch's row of network_ar_lattice.csv (kArLatticeHeader: AGT-6's columns, then card
/// FCF's).
void writeEpochRow(std::ofstream& os, const SyncedEpoch& se, const ArEpochFixes& fixes, const ArLatticeEpoch& e) {
    auto rel = [&](ArLatticeRelease r) { return e.released_by[releaseIndex(r)]; };
    os << fmt::format("{},{:.1f},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{:.3f},"
                      "{},{},{},{},{},{},{},{},{},{}\n",
                      se.time.week, se.time.tow, e.rows, e.arcs, fixes.fixed.size(),
                      e.verdicts[verdictIndex(LatticeVerdict::Added)],
                      e.verdicts[verdictIndex(LatticeVerdict::Confirmed)],
                      e.verdicts[verdictIndex(LatticeVerdict::Contradicted)],
                      e.verdicts[verdictIndex(LatticeVerdict::NotPrimitive)], e.releases.size(),
                      e.released_touching, e.dims_lost, e.no_unit, rel(ArLatticeRelease::Vanished),
                      rel(ArLatticeRelease::FirstSeen), rel(ArLatticeRelease::Freed),
                      rel(ArLatticeRelease::StampChange), e.max_len, e.mean_len, e.waiting,
                      rel(ArLatticeRelease::ReplayCulprit), rel(ArLatticeRelease::GrossLos),
                      rel(ArLatticeRelease::Contradiction), rel(ArLatticeRelease::Revalidation),
                      rel(ArLatticeRelease::Acceptance), rel(ArLatticeRelease::Degenerate),
                      rel(ArLatticeRelease::SeamGap), e.units, e.units_failed);
}

/// The epoch's rows of network_ar_lattice_events.csv: the releases in application order, then
/// every offer that is neither a confirmation nor waiting (kArLatticeEventsHeader).
void writeEventRows(std::ofstream& os, const SyncedEpoch& se, const std::vector<NetStation>& stations,
                    const ArEpochFixes& fixes, const ArLatticeEpoch& e) {
    for (const ArLatticeReleaseEvent& r : e.releases) {
        const char* const cause = arLatticeReleaseName(r.cause);
        switch (r.scope) {
        case ArLatticeScope::Arc:
            os << fmt::format("{},{:.1f},release,{},{},,{},,,,,{},{},{},{},{}\n", se.time.week, se.time.tow,
                              bandColumns(r.arc.signal), r.arc.sat.str(), stationName(stations, r.arc.station),
                              cause, r.rows_touched, r.rows_dropped, r.change, static_cast<int>(r.free_cause));
            break;
        case ArLatticeScope::Cycle: {
            const auto& [sig, sta_r, ref_sta, sat_j, ref_sat] = r.dd;
            const bool contra = r.cause == ArLatticeRelease::Contradiction;
            os << fmt::format("{},{:.1f},release_cycle,{},{},{},{},{},{},{},,{},{},{},,\n", se.time.week,
                              se.time.tow, bandColumns(sig), sat_j.str(), ref_sat.str(),
                              stationName(stations, sta_r), stationName(stations, ref_sta),
                              contra ? std::to_string(r.offered) : std::string(),
                              contra ? std::to_string(r.implied) : std::string(), cause, r.rows_touched,
                              r.rows_dropped);
            break;
        }
        case ArLatticeScope::All:
            os << fmt::format("{},{:.1f},release_all,,,,,,,,,,{},{},{},,\n", se.time.week, se.time.tow, cause,
                              r.rows_touched, r.rows_dropped);
            break;
        }
    }
    for (std::size_t i = 0; i < e.offers.size() && i < fixes.fixed.size(); ++i) {
        const ArLatticeOffer& o = e.offers[i];
        if (o.waiting || o.verdict == LatticeVerdict::Confirmed) continue;
        const ArFixedCycle& f = fixes.fixed[i];
        const auto& [sig, sta_r, ref_sta, sat_j, ref_sat] = f.id;
        os << fmt::format("{},{:.1f},{},{},{},{},{},{},{},{},{},,,,,\n", se.time.week, se.time.tow,
                          arLatticeVerdictName(o.verdict), bandColumns(sig), sat_j.str(), ref_sat.str(),
                          stationName(stations, sta_r), stationName(stations, ref_sta), f.value,
                          o.verdict == LatticeVerdict::Contradicted ? std::to_string(o.implied) : std::string(),
                          o.verdict == LatticeVerdict::Added ? std::to_string(o.len) : std::string());
    }
}

/// "name n, ..." over the non-zero entries of a tally, or "none".
template <std::size_t N, typename NameOf>
std::string tallyText(const std::array<long long, N>& counts, NameOf name_of) {
    std::string out;
    for (std::size_t k = 0; k < N; ++k) {
        if (counts[k] == 0) continue;
        out += fmt::format("{}{} {}", out.empty() ? "" : ", ", name_of(k), counts[k]);
    }
    return out.empty() ? std::string("none") : out;
}

/// The largest element of a sample; NaN when empty.
double maxOf(const std::vector<double>& v) {
    return v.empty() ? std::numeric_limits<double>::quiet_NaN() : *std::max_element(v.begin(), v.end());
}

/// A cause's name by tally index.
const char* causeNameAt(std::size_t k) { return arLatticeReleaseName(static_cast<ArLatticeRelease>(k)); }

}  // namespace

const ArLatticeReleaseSpec& arLatticeReleaseSpec(ArLatticeRelease r) {
    const std::size_t k = std::min(releaseIndex(r), kArLatticeReleaseCount - 1);
    return kReleaseTable[k];
}

const char* arLatticeReleaseName(ArLatticeRelease r) {
    return releaseIndex(r) < kArLatticeReleaseCount ? kReleaseTable[releaseIndex(r)].name : "?";
}

const char* arLatticeVerdictName(LatticeVerdict v) {
    switch (v) {
    case LatticeVerdict::Added:        return "added";
    case LatticeVerdict::Confirmed:    return "confirmed";
    case LatticeVerdict::Contradicted: return "contradicted";
    case LatticeVerdict::NotPrimitive: return "not_primitive";
    }
    return "?";
}

bool ArLosScope::covers(const ArcId& a) const {
    return sats.count(a.sat) > 0 || stations.count(a.station) > 0 || los.count(LosId{a.station, a.sat}) > 0;
}

ArLatticeEvidence networkArLatticeEvidence(const NetworkRunContext& ctx, const WithinEpochCulprits& we) {
    ArLatticeEvidence ev;
    auto addLos = [](ArLosScope& s, int station, const std::string& sat) {
        if (const std::optional<SatId> id = SatId::parse(sat)) s.los.insert(LosId{StationId{station}, *id});
    };
    for (const std::string& sat : we.sats)
        if (const std::optional<SatId> id = SatId::parse(sat)) ev.replay.sats.insert(*id);
    for (const auto& [station, sats] : we.arcs)
        for (const std::string& sat : sats) addLos(ev.replay, station, sat);
    for (int station : we.stations) ev.replay.stations.insert(StationId{station});
    // The gross-LOS lists are this epoch's only when the screen's Decide half ran before this
    // seam (network.gross_los_emit_exclude); otherwise they hold the last epoch's, already
    // acted on and read through the life stamp.
    if (ctx.knobs.gl_emit_exclude) {
        for (const auto& [station, sat] : ctx.carry.gl_act_declare) addLos(ev.gross, station, sat);
        for (const auto& [station, declared] : ctx.carry.gl_act_station)
            if (declared) ev.gross.stations.insert(StationId{station});
        for (const auto& [station, sat] : ctx.carry.gl_act_cold_refloat) addLos(ev.gross, station, sat);
    }
    return ev;
}

std::vector<ArLatticeReleaseEvent> arLatticeEvidenceReleases(const arcgraph::CycleLattice& L,
                                                             const std::map<ArcId, ArArcReading>& arcs,
                                                             const ArLatticeEvidence& ev) {
    std::vector<ArLatticeReleaseEvent> out;
    for (const ArcId& arc : latticeArcs(L)) {
        ArLatticeReleaseEvent r;
        r.arc = arc;
        const auto it = arcs.find(arc);
        if (it == arcs.end()) {                       // the search did not read it: it left the pool
            r.cause = ArLatticeRelease::Vanished;
        } else if (const std::optional<ArLatticeRelease> life = releaseOf(it->second.edge)) {
            r.cause      = *life;
            r.change     = it->second.change;
            r.free_cause = it->second.free_cause;
        } else if (ev.replay.covers(arc)) {
            r.cause = ArLatticeRelease::ReplayCulprit;
        } else if (ev.gross.covers(arc)) {
            r.cause = ArLatticeRelease::GrossLos;
        } else {
            continue;                                 // the life goes on
        }
        out.push_back(r);
    }
    return out;
}

std::array<ArcId, 4> arDdArcs(const ArDdIdentity& id) {
    const auto& [sig, sta_r, ref_sta, sat_j, ref_sat] = id;
    return {ArcId{sta_r, sat_j, sig}, ArcId{sta_r, ref_sat, sig}, ArcId{ref_sta, sat_j, sig},
            ArcId{ref_sta, ref_sat, sig}};
}

void arLatticeRelease(ArLatticeState& S, std::vector<ArLatticeReleaseEvent> events, ArLatticeEpoch& e) {
    arcgraph::CycleLattice& L = S.lattice;
    std::set<ArcId> broken;
    bool all = false;
    for (ArLatticeReleaseEvent& r : events) {
        switch (r.scope) {
        case ArLatticeScope::Arc:
            r.rows_touched = holdOf(L, r.arc).rows;
            r.rows_dropped = eliminateArc(L, r.arc, e);
            broken.insert(r.arc);
            break;
        case ArLatticeScope::Cycle: {
            // Every arc of the cycle, not the rows behind it alone (file banner).
            std::set<ArcId> arcs;
            for (const arcgraph::CycleTerm& t : r.cycle.terms) arcs.insert(t.arc);
            r.rows_touched = rowsThrough(L, arcs);
            for (const ArcId& a : arcs) r.rows_dropped += eliminateArc(L, a, e);
            broken.insert(arcs.begin(), arcs.end());
            break;
        }
        case ArLatticeScope::All:
            r.rows_touched = r.rows_dropped = static_cast<int>(L.rows.size());
            L   = arcgraph::CycleLattice{};
            all = true;
            break;
        }
        const std::size_t k = releaseIndex(r.cause);
        ++e.released_by[k];
        e.dims_by[k] += r.rows_dropped;
        if (r.rows_touched > 0) ++e.released_touching;
        e.dims_lost += r.rows_dropped;
        e.releases.push_back(std::move(r));
    }
    // A streak through a released arc restarts: the fix it counts may belong to the old life.
    if (all) {
        S.streaks.clear();
    } else if (!broken.empty()) {
        for (auto it = S.streaks.begin(); it != S.streaks.end();)
            it = throughAny(it->first, broken) ? S.streaks.erase(it) : std::next(it);
    }
    e.broken.insert(broken.begin(), broken.end());
}

ArLatticeEpoch arLatticeBegin(ArLatticeState& S, const ArEpochFixes& fixes, const ArLatticeEvidence& ev,
                              long long ep) {
    ArLatticeEpoch e;
    e.offers.resize(fixes.fixed.size());
    // 1. The seam gap: a skipped epoch's evidence left no stamp, so nothing it held is trusted.
    if (S.last_step_ep >= 0 && ep - S.last_step_ep > 1) {
        ArLatticeReleaseEvent gap;
        gap.cause = ArLatticeRelease::SeamGap;
        gap.scope = ArLatticeScope::All;
        arLatticeRelease(S, {gap}, e);
    }
    // 2. The evidence: new lives as the search read them, the replay's and the gross-LOS lines
    //    of sight.  Every arc they name breaks the streaks through it, held or not.
    for (const auto& [arc, reading] : fixes.arcs)
        if (releaseOf(reading.edge) || ev.replay.covers(arc) || ev.gross.covers(arc)) e.broken.insert(arc);
    arLatticeRelease(S, arLatticeEvidenceReleases(S.lattice, fixes.arcs, ev), e);
    // 3. Contradictions, each released before the next fix is read.
    for (std::size_t i = 0; i < fixes.fixed.size(); ++i) {
        const ArFixedCycle& f = fixes.fixed[i];
        const std::optional<long long> implied = arcgraph::latticeImplied(S.lattice, f.cycle);
        if (!implied || *implied == f.value) continue;
        ArLatticeOffer& o = e.offers[i];
        o.verdict = LatticeVerdict::Contradicted;
        o.implied = *implied;
        o.streak  = 1;
        ++e.verdicts[verdictIndex(LatticeVerdict::Contradicted)];
        arLatticeRelease(S, {contradictionOf(f, *implied)}, e);
    }
    return e;
}

void arLatticeAdmit(ArLatticeState& S, const ArEpochFixes& fixes, int persist_ep, long long ep,
                    ArLatticeEpoch& e) {
    const int need = std::max(1, persist_ep);
    for (std::size_t i = 0; i < fixes.fixed.size(); ++i) {
        const ArFixedCycle& f = fixes.fixed[i];
        ArLatticeOffer& o = e.offers[i];
        if (o.verdict == LatticeVerdict::Contradicted) {   // step 3: its streak restarts here
            S.streaks[f.id] = ArFixStreak{f.value, 1, ep};
            continue;
        }
        ArFixStreak& s = S.streaks[f.id];
        const bool goes_on = s.epochs > 0 && s.value == f.value && s.last_ep == ep - 1
                          && !throughAny(f.id, e.broken);
        s = ArFixStreak{f.value, goes_on ? s.epochs + 1 : 1, ep};
        o.streak = s.epochs;
        if (s.epochs < need) {
            o.waiting = true;
            ++e.waiting;
            continue;
        }
        o.verdict = arcgraph::latticeOffer(S.lattice, f.cycle, f.value, ep);
        ++e.verdicts[verdictIndex(o.verdict)];
        if (o.verdict == LatticeVerdict::Added) {
            o.len = static_cast<int>(S.lattice.rows.back().terms.size());
        } else if (o.verdict == LatticeVerdict::Contradicted) {
            // The epoch's own fixes disagree through a row added at this epoch.
            o.implied = arcgraph::latticeImplied(S.lattice, f.cycle).value_or(0);
            arLatticeRelease(S, {contradictionOf(f, o.implied)}, e);
            S.streaks[f.id] = ArFixStreak{f.value, 1, ep};
        }
    }
    // A streak not continued at this epoch has ended.
    for (auto it = S.streaks.begin(); it != S.streaks.end();)
        it = (it->second.last_ep < ep) ? S.streaks.erase(it) : std::next(it);
}

void arLatticeClose(ArLatticeState& S, long long ep, ArLatticeEpoch& e) {
    latticeFigures(S.lattice, e);
    S.last_step_ep = ep;
}

void arLatticeTally(ArLatticeCounters& c, const ArLatticeEpoch& e, double tow, double ms) {
    ++c.epochs;
    for (std::size_t k = 0; k < kArLatticeVerdictCount; ++k) {
        c.verdicts[k] += e.verdicts[k];
        c.fixes       += e.verdicts[k];
    }
    c.waiting += e.waiting;
    c.fixes   += e.waiting;
    if (e.verdicts[verdictIndex(LatticeVerdict::Contradicted)] > 0) {
        if (c.contradicted_epochs == 0) c.first_contradicted_tow = tow;
        ++c.contradicted_epochs;
    }
    for (const ArLatticeReleaseEvent& r : e.releases) {
        const std::size_t k = releaseIndex(r.cause);
        ++c.released_by[k];
        c.dims_by[k] += r.rows_dropped;
        if (r.rows_touched == 0) continue;
        ++c.touching_by[k];
        if (r.scope != ArLatticeScope::Arc) continue;
        for (int b = 0; b < kArcLifeFieldCount; ++b)
            if (r.change & (1u << b)) ++c.change_by[static_cast<std::size_t>(b)];
        for (int b = 0; b < kArLatticeFreeCauseBits; ++b)
            if (r.free_cause & (1u << b)) ++c.free_by[static_cast<std::size_t>(b)];
    }
    c.released_touching += e.released_touching;
    c.dims_lost         += e.dims_lost;
    c.no_unit           += e.no_unit;
    c.units             += e.units;
    c.units_failed      += e.units_failed;
    c.max_rows = std::max(c.max_rows, e.rows);
    c.max_len  = std::max(c.max_len, e.max_len);
    c.rows.push_back(static_cast<double>(e.rows));
    c.mean_len.push_back(e.mean_len);
    c.ms.push_back(ms);
}

void networkArLatticeRecord(NetworkRunContext& ctx, const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces, const SyncedEpoch& se,
                            const std::vector<NetStation>& stations, const ArEpochFixes& fixes,
                            const ArLatticeEpoch& e, double ms) {
    arLatticeTally(ctx.counters.netar_lattice, e, se.time.tow, ms);
    if (traces.ar_lattice_trace.is_open()) writeEpochRow(traces.ar_lattice_trace, se, fixes, e);
    if (traces.ar_lattice_events_trace.is_open())
        writeEventRows(traces.ar_lattice_events_trace, se, stations, fixes, e);
    const int n_contra = e.verdicts[verdictIndex(LatticeVerdict::Contradicted)];
    if (n_contra > 0)
        log->warn("Network AR lattice (cards AGT, FCF): TOW {:.1f}: {} fix(es) CONTRADICTED the "
                  "lattice of fixed cycles, and the arcs of each cycle were released: a wrong fix, "
                  "or a release the evidence missed (network_ar_lattice_events.csv names each; "
                  "scripts/simnet_ar_lattice_grade.py grades them against truth).",
                  se.time.tow, n_contra);
}

bool arLatticeArmed(const GnssConfig& config_) {
    const auto& ar = config_.getNetworkConfig().ambiguity_resolution;
    return ar.enable && ar.cycle_basis
        && (ar.trace_lattice
            || (ar.fixed_cycle_fold != FixedCycleFold::Off && ar.fixed_cycle_source == FixedCycleSource::Lattice));
}

// ---- Run-end reports (card NOM step TD1b) ----

// Run-end report: the ledger of fixed cycles (cards AGT and FCF).
int networkArLatticeRunEnd(const NetworkRunEndView& v) {
    if (!arLatticeArmed(v.config)) return 0;
    const auto& ar = v.config.getNetworkConfig().ambiguity_resolution;
    const ArLatticeCounters& C = v.ctx.counters.netar_lattice;
    const arcgraph::CycleLattice& L = v.ctx.carry.ar_lattice.lattice;
    long long released = 0;
    for (long long n : C.released_by) released += n;

    v.log->info("Network (producer AR lattice ledger, cards AGT and FCF): {} step(s), {} epoch(s) "
                "without a search.  {} fix(es) met: {} waiting for {} epoch(s) at one integer, {} "
                "added, {} confirmed, {} CONTRADICTED ({} epoch(s), first at TOW {:.1f}), {} not "
                "primitive.  Lattice dimension {} at the end (p50 {:.0f}, at most {}); row length "
                "mean p50 {:.2f} arcs, longest {}.",
                C.epochs, C.epochs_skipped, C.fixes, C.waiting, ar.fixed_cycle_persist_ep,
                C.verdicts[verdictIndex(LatticeVerdict::Added)],
                C.verdicts[verdictIndex(LatticeVerdict::Confirmed)],
                C.verdicts[verdictIndex(LatticeVerdict::Contradicted)], C.contradicted_epochs,
                C.first_contradicted_tow, C.verdicts[verdictIndex(LatticeVerdict::NotPrimitive)],
                L.rows.size(), RobustStats::median(C.rows), C.max_rows, RobustStats::median(C.mean_len),
                C.max_len);
    v.log->info("Network (producer AR lattice ledger, cards AGT and FCF): {} release(s) [{}]; {} of "
                "them found rows [{}] and cost {} dimension(s) [{}] ({} arc elimination(s) with no "
                "unit row); arc releases that found rows by stamp field [{}], by free-epoch cause "
                "[{}].  Units implied {} ({} failing their float test).  Cost per epoch: p50 "
                "{:.2f} ms, max {:.2f} ms.",
                released, tallyText(C.released_by, causeNameAt), C.released_touching,
                tallyText(C.touching_by, causeNameAt), C.dims_lost, tallyText(C.dims_by, causeNameAt),
                C.no_unit,
                tallyText(C.change_by, [](std::size_t k) { return arcLifeFieldName(static_cast<int>(k)); }),
                tallyText(C.free_by, [](std::size_t k) { return latchCauseName(static_cast<int>(k)); }),
                C.units, C.units_failed, RobustStats::median(C.ms), maxOf(C.ms));
    if (C.verdicts[verdictIndex(LatticeVerdict::Contradicted)] > 0)
        v.log->warn("Network (producer AR lattice ledger, cards AGT and FCF): {} fix(es) "
                    "CONTRADICTED the lattice: each is a wrong fix or a missed release; grade "
                    "logs/network_ar_lattice_events.csv against truth "
                    "(scripts/simnet_ar_lattice_grade.py).",
                    C.verdicts[verdictIndex(LatticeVerdict::Contradicted)]);
    if (C.epochs == 0 || C.fixes == C.waiting)
        v.log->warn("Network (producer AR lattice ledger, cards AGT and FCF): ARMED, but no fixed DD "
                    "was ever offered ({} step(s)); every lattice number above is structurally empty "
                    "(standing invariant 4).", C.epochs);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
