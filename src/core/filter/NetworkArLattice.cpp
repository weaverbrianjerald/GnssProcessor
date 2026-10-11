/**
 * @file NetworkArLattice.cpp
 * @brief The producer AR search's lattice observatory (card AGT, step AGT-6): the release and
 *        offer step, the seam that writes its two files, and its run-end report.  What it
 *        measures, its order and why it keeps no stamp of its own are in the header's file
 *        banner, include/filter/NetworkArLattice.hpp.
 */

#include "filter/NetworkArLattice.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.carry.ar_lattice, ctx.counters.netar_lattice
#include "config/GnssConfig.hpp"         // network.ambiguity_resolution.trace_lattice
#include "filter/NetworkResponses.hpp"   // latchCauseName: the free-epoch latch's cause names
#include "filter/NetworkRunEnd.hpp"      // NetworkRunEndView
#include "filter/NetworkRunTypes.hpp"    // NetStation
#include "logging/NetworkRunTraces.hpp"  // ar_lattice_trace, ar_lattice_events_trace
#include "math/RobustStats.hpp"          // RobustStats::median
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace GnssProcessor {

namespace {

/// The index of a verdict in the tallies (LatticeVerdict order).
std::size_t verdictIndex(arcgraph::LatticeVerdict v) { return static_cast<std::size_t>(v); }

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

/// How the rows hold @p arc (each row's terms are sorted by arc).
ArcHold holdOf(const arcgraph::CycleLattice& L, const ArcId& arc) {
    ArcHold h;
    for (const arcgraph::Cycle& row : L.rows) {
        const auto it = std::lower_bound(row.terms.begin(), row.terms.end(), arc,
                                         [](const arcgraph::CycleTerm& t, const ArcId& a) { return t.arc < a; });
        if (it == row.terms.end() || !(it->arc == arc)) continue;
        ++h.rows;
        if (it->coeff == 1 || it->coeff == -1) h.unit = true;
    }
    return h;
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

/// The lattice's size after an epoch: its dimension, the arcs it holds and its row lengths.
void latticeFigures(const arcgraph::CycleLattice& L, ArLatticeEpoch& e) {
    e.rows = static_cast<int>(L.rows.size());
    e.arcs = static_cast<int>(latticeArcs(L).size());
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

/// The epoch's row of network_ar_lattice.csv.
void writeEpochRow(std::ofstream& os, const SyncedEpoch& se, const ArEpochFixes& fixes, const ArLatticeEpoch& e) {
    using arcgraph::LatticeVerdict;
    os << fmt::format("{},{:.1f},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{:.3f}\n",
                      se.time.week, se.time.tow, e.rows, e.arcs, fixes.fixed.size(),
                      e.verdicts[verdictIndex(LatticeVerdict::Added)],
                      e.verdicts[verdictIndex(LatticeVerdict::Confirmed)],
                      e.verdicts[verdictIndex(LatticeVerdict::Contradicted)],
                      e.verdicts[verdictIndex(LatticeVerdict::NotPrimitive)], e.releases.size(),
                      e.released_touching, e.dims_lost, e.no_unit,
                      e.released_by[releaseIndex(ArLatticeRelease::Vanished)],
                      e.released_by[releaseIndex(ArLatticeRelease::FirstSeen)],
                      e.released_by[releaseIndex(ArLatticeRelease::Freed)],
                      e.released_by[releaseIndex(ArLatticeRelease::StampChange)], e.max_len, e.mean_len);
}

/// The epoch's rows of network_ar_lattice_events.csv: the releases, then every offer that is not
/// a confirmation (kArLatticeEventsHeader).
void writeEventRows(std::ofstream& os, const SyncedEpoch& se, const std::vector<NetStation>& stations,
                    const ArEpochFixes& fixes, const ArLatticeEpoch& e) {
    using arcgraph::LatticeVerdict;
    for (const ArLatticeReleaseEvent& r : e.releases)
        os << fmt::format("{},{:.1f},release,{},{},,{},,,,,{},{},{},{},{}\n", se.time.week, se.time.tow,
                          bandColumns(r.arc.signal), r.arc.sat.str(), stationName(stations, r.arc.station),
                          arLatticeReleaseName(r.cause), r.rows_touched, r.rows_dropped, r.change,
                          static_cast<int>(r.free_cause));
    for (std::size_t i = 0; i < e.offers.size() && i < fixes.fixed.size(); ++i) {
        const ArLatticeOffer& o = e.offers[i];
        if (o.verdict == LatticeVerdict::Confirmed) continue;
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

}  // namespace

const char* arLatticeReleaseName(ArLatticeRelease r) {
    switch (r) {
    case ArLatticeRelease::Vanished:    return "vanished";
    case ArLatticeRelease::FirstSeen:   return "first_seen";
    case ArLatticeRelease::Freed:       return "freed";
    case ArLatticeRelease::StampChange: return "stamp";
    case ArLatticeRelease::Count:       break;
    }
    return "?";
}

const char* arLatticeVerdictName(arcgraph::LatticeVerdict v) {
    switch (v) {
    case arcgraph::LatticeVerdict::Added:        return "added";
    case arcgraph::LatticeVerdict::Confirmed:    return "confirmed";
    case arcgraph::LatticeVerdict::Contradicted: return "contradicted";
    case arcgraph::LatticeVerdict::NotPrimitive: return "not_primitive";
    }
    return "?";
}

std::vector<ArLatticeReleaseEvent> arLatticeReleases(const arcgraph::CycleLattice& L,
                                                     const std::map<ArcId, ArArcReading>& arcs) {
    std::vector<ArLatticeReleaseEvent> out;
    for (const ArcId& arc : latticeArcs(L)) {
        ArLatticeReleaseEvent ev;
        ev.arc = arc;
        const auto it = arcs.find(arc);
        if (it == arcs.end()) {          // the search did not read it: it left the pool
            ev.cause = ArLatticeRelease::Vanished;
            out.push_back(ev);
            continue;
        }
        const std::optional<ArLatticeRelease> cause = releaseOf(it->second.edge);
        if (!cause) continue;            // the life goes on
        ev.cause      = *cause;
        ev.change     = it->second.change;
        ev.free_cause = it->second.free_cause;
        out.push_back(ev);
    }
    return out;
}

ArLatticeEpoch arLatticeStep(arcgraph::CycleLattice& L, const ArEpochFixes& fixes) {
    ArLatticeEpoch e;
    // 1. Release every arc whose life ended, ascending ArcId.
    e.releases = arLatticeReleases(L, fixes.arcs);
    for (ArLatticeReleaseEvent& r : e.releases) {
        ++e.released_by[releaseIndex(r.cause)];
        const ArcHold hold = holdOf(L, r.arc);
        r.rows_touched = hold.rows;
        r.rows_dropped = arcgraph::latticeEliminateArc(L, r.arc);
        if (hold.rows > 0) ++e.released_touching;
        if (hold.rows > 0 && !hold.unit) ++e.no_unit;
        e.dims_lost += r.rows_dropped;
    }
    // 2. Offer every fix, in basis order.
    e.offers.reserve(fixes.fixed.size());
    for (const ArFixedCycle& f : fixes.fixed) {
        ArLatticeOffer o;
        o.verdict = arcgraph::latticeOffer(L, f.cycle, f.value);
        if (o.verdict == arcgraph::LatticeVerdict::Contradicted)
            o.implied = arcgraph::latticeImplied(L, f.cycle).value_or(0);   // a contradiction leaves L unchanged
        else if (o.verdict == arcgraph::LatticeVerdict::Added)
            o.len = static_cast<int>(L.rows.back().terms.size());
        ++e.verdicts[verdictIndex(o.verdict)];
        e.offers.push_back(o);
    }
    latticeFigures(L, e);
    return e;
}

void arLatticeTally(ArLatticeCounters& c, const ArLatticeEpoch& e, double tow, double ms) {
    ++c.epochs;
    for (std::size_t k = 0; k < kArLatticeVerdictCount; ++k) {
        c.verdicts[k] += e.verdicts[k];
        c.fixes       += e.verdicts[k];
    }
    if (e.verdicts[verdictIndex(arcgraph::LatticeVerdict::Contradicted)] > 0) {
        if (c.contradicted_epochs == 0) c.first_contradicted_tow = tow;
        ++c.contradicted_epochs;
    }
    for (const ArLatticeReleaseEvent& r : e.releases) {
        const std::size_t k = releaseIndex(r.cause);
        ++c.released_by[k];
        if (r.rows_touched == 0) continue;
        ++c.touching_by[k];
        for (int b = 0; b < kArcLifeFieldCount; ++b)
            if (r.change & (1u << b)) ++c.change_by[static_cast<std::size_t>(b)];
        for (int b = 0; b < kArLatticeFreeCauseBits; ++b)
            if (r.free_cause & (1u << b)) ++c.free_by[static_cast<std::size_t>(b)];
    }
    c.released_touching += e.released_touching;
    c.dims_lost         += e.dims_lost;
    c.no_unit           += e.no_unit;
    if (e.dims_lost != e.released_touching) ++c.dims_mismatch_epochs;
    c.max_rows = std::max(c.max_rows, e.rows);
    c.max_len  = std::max(c.max_len, e.max_len);
    c.rows.push_back(static_cast<double>(e.rows));
    c.mean_len.push_back(e.mean_len);
    c.ms.push_back(ms);
}

void networkArLattice(NetworkRunContext& ctx, const GnssConfig& config_,
                      const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
                      const SyncedEpoch& se, const std::vector<NetStation>& stations,
                      const ArEpochFixes& fixes) {
    const auto& netar = config_.getNetworkConfig().ambiguity_resolution;
    if (!netar.enable || !netar.trace_lattice) return;
    ArLatticeCounters& c = ctx.counters.netar_lattice;
    if (!fixes.searched) {           // the search block did not run: nothing to release or offer
        ++c.epochs_skipped;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const ArLatticeEpoch e = arLatticeStep(ctx.carry.ar_lattice.lattice, fixes);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    arLatticeTally(c, e, se.time.tow, ms);
    if (traces.ar_lattice_trace.is_open()) writeEpochRow(traces.ar_lattice_trace, se, fixes, e);
    if (traces.ar_lattice_events_trace.is_open())
        writeEventRows(traces.ar_lattice_events_trace, se, stations, fixes, e);
    const int n_contra = e.verdicts[verdictIndex(arcgraph::LatticeVerdict::Contradicted)];
    if (n_contra > 0)
        log->warn("Network AR lattice (card AGT): TOW {:.1f}: {} offered fix(es) CONTRADICTED the "
                  "lattice of fixed cycles: a wrong fix, or a release the life readings missed "
                  "(network_ar_lattice_events.csv names each; scripts/simnet_ar_lattice_grade.py "
                  "grades them against truth).", se.time.tow, n_contra);
}

// ---- Run-end reports (card NOM step TD1b) ----

// Run-end report: the producer AR search's lattice observatory (card AGT).
int networkArLatticeRunEnd(const NetworkRunEndView& v) {
    const auto& netar = v.config.getNetworkConfig().ambiguity_resolution;
    if (!netar.enable || !netar.trace_lattice) return 0;
    const ArLatticeCounters& C = v.ctx.counters.netar_lattice;
    const arcgraph::CycleLattice& L = v.ctx.carry.ar_lattice.lattice;
    using arcgraph::LatticeVerdict;
    long long released = 0;
    for (long long n : C.released_by) released += n;

    v.log->info("Network (producer AR lattice observatory, card AGT): {} search epoch(s) observed, "
                "{} without a search.  {} fixed DD(s) offered: {} added, {} confirmed, {} "
                "CONTRADICTED ({} epoch(s), first at TOW {:.1f}), {} not primitive.  Lattice "
                "dimension {} at the end (p50 {:.0f}, at most {}); row length mean p50 {:.2f} "
                "arcs, longest {}.",
                C.epochs, C.epochs_skipped, C.fixes, C.verdicts[verdictIndex(LatticeVerdict::Added)],
                C.verdicts[verdictIndex(LatticeVerdict::Confirmed)],
                C.verdicts[verdictIndex(LatticeVerdict::Contradicted)], C.contradicted_epochs,
                C.first_contradicted_tow, C.verdicts[verdictIndex(LatticeVerdict::NotPrimitive)],
                L.rows.size(), RobustStats::median(C.rows), C.max_rows, RobustStats::median(C.mean_len),
                C.max_len);
    v.log->info("Network (producer AR lattice observatory, card AGT): {} lattice arc(s) released "
                "[{}]; {} of them held rows [{}] and cost {} dimension(s) ({} elimination(s) with "
                "no unit row; {} epoch(s) whose dimensions lost differ from the released arcs that "
                "held rows); by stamp field [{}]; by free-epoch cause [{}].  Cost per epoch: p50 "
                "{:.2f} ms, max {:.2f} ms.  Measure-only, and valid only while no hold pulls the "
                "floats the search fixes.",
                released,
                tallyText(C.released_by, [](std::size_t k) { return arLatticeReleaseName(static_cast<ArLatticeRelease>(k)); }),
                C.released_touching,
                tallyText(C.touching_by, [](std::size_t k) { return arLatticeReleaseName(static_cast<ArLatticeRelease>(k)); }),
                C.dims_lost, C.no_unit, C.dims_mismatch_epochs,
                tallyText(C.change_by, [](std::size_t k) { return arcLifeFieldName(static_cast<int>(k)); }),
                tallyText(C.free_by, [](std::size_t k) { return latchCauseName(static_cast<int>(k)); }),
                RobustStats::median(C.ms), maxOf(C.ms));
    if (C.verdicts[verdictIndex(LatticeVerdict::Contradicted)] > 0)
        v.log->warn("Network (producer AR lattice observatory, card AGT): {} offered fix(es) "
                    "CONTRADICTED the lattice: each is a wrong fix or a missed release; grade "
                    "logs/network_ar_lattice_events.csv against truth "
                    "(scripts/simnet_ar_lattice_grade.py).",
                    C.verdicts[verdictIndex(LatticeVerdict::Contradicted)]);
    if (C.dims_mismatch_epochs > 0)
        v.log->warn("Network (producer AR lattice observatory, card AGT): in {} epoch(s) the "
                    "dimensions lost differ from the released arcs that held rows: {} "
                    "elimination(s) found no row holding the arc at +-1 and dropped every row "
                    "through it.", C.dims_mismatch_epochs, C.no_unit);
    if (C.epochs == 0 || C.fixes == 0)
        v.log->warn("Network (producer AR lattice observatory, card AGT): ARMED, but no fixed DD was "
                    "ever offered ({} search epoch(s)); every lattice number above is structurally "
                    "empty (standing invariant 4).", C.epochs);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
