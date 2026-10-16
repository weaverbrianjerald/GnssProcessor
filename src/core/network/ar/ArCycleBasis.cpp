/**
 * @file ArCycleBasis.cpp
 * @brief The cycle-basis search's families, forest and basis, its map against the stored
 *        basis, its tallies and its run-end clause (step AGT-5).  The design is in
 *        the header's file banner, include/network/ar/ArCycleBasis.hpp; the seam that reads
 *        the pool, emits the search rows and stores the memory is networkProducerAr
 *        (network/ar/ProducerAr.cpp).
 */

#include "network/ar/ArCycleBasis.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace GnssProcessor {

namespace {

/// Every band of a family, as a bit mask.
std::uint8_t familyMask(const arcgraph::ArcFamily& f) {
    return static_cast<std::uint8_t>((1u << f.bands.size()) - 1u);
}

/// "G L1C": a band's label in the tallies.
std::string bandLabel(const ArFamilySpec& fam, std::size_t band) {
    return std::string(1, traitsOf(fam.sys).letter) + " " + fam.signals[band];
}

/// The upper median of a sample (element n / 2 of the sorted copy); NaN when empty.
double upperMedian(std::vector<double> v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/// The largest element of a sample; NaN when empty.
double maxOf(const std::vector<double>& v) {
    return v.empty() ? std::numeric_limits<double>::quiet_NaN() : *std::max_element(v.begin(), v.end());
}

/// True when the cycle has a term on an invalidated arc.
bool touches(const arcgraph::Cycle& x, const std::set<ArcId>& invalidated) {
    for (const arcgraph::CycleTerm& t : x.terms)
        if (invalidated.count(t.arc)) return true;
    return false;
}

}  // namespace

std::vector<ArFamilySpec> arCycleFamilies(System sys,
                                          const std::vector<std::pair<std::string, std::size_t>>& signal_arcs) {
    std::vector<std::string> names;
    std::vector<SignalId>    sigs;
    std::vector<std::size_t> counts;
    for (const auto& [name, n] : signal_arcs) {
        const std::optional<SignalId> sig = SignalId::fromObsCode(sys, name);
        if (!sig) continue;
        names.push_back(name);
        sigs.push_back(*sig);
        counts.push_back(n);
    }
    std::vector<ArFamilySpec> out;
    if (names.empty()) return out;
    // The pair: the two signals with the most arcs (pool order on ties), kept in pool order.
    std::vector<std::size_t> by_count(names.size());
    std::iota(by_count.begin(), by_count.end(), std::size_t{0});
    std::stable_sort(by_count.begin(), by_count.end(),
                     [&](std::size_t a, std::size_t b) { return counts[a] > counts[b]; });
    std::vector<std::size_t> pair(by_count.begin(),
                                  by_count.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, by_count.size())));
    std::sort(pair.begin(), pair.end());
    ArFamilySpec first;
    first.sys = sys;
    for (std::size_t i : pair) {
        first.signals.push_back(names[i]);
        first.bands.push_back(sigs[i]);
    }
    out.push_back(first);
    // Every further signal alone, in pool order.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (std::find(pair.begin(), pair.end(), i) != pair.end()) continue;
        ArFamilySpec alone;
        alone.sys = sys;
        alone.signals.push_back(names[i]);
        alone.bands.push_back(sigs[i]);
        out.push_back(alone);
    }
    return out;
}

arcgraph::ForestRules arCycleForestRules(std::uint8_t band_mask) {
    arcgraph::ForestRules rules;
    rules.band_mask            = band_mask;  // a shared arc: present in every family band
    rules.min_common           = 2;          // two common satellites first ...
    rules.stop_at_gap          = false;      // ... then one, so the forest spans the component
    rules.refuse_slipped_first = true;       // a slipped arc is no new edge or reference while another route exists
    rules.by_durability        = true;       // the most durable reference satellite
    return rules;
}

ArCycleBuild arCycleBuild(const ArCycleInput& in, const std::vector<arcgraph::IncumbentLink>& incumbent,
                          const arcgraph::CycleBasis* sticky) {
    ArCycleBuild b;
    b.graph = arcgraph::buildArcGraph(in.family, in.stations, in.sats, in.rows);
    const std::uint8_t full = familyMask(in.family);
    const arcgraph::ForestRules rules = arCycleForestRules(full);
    b.components = arcgraph::arcComponents(b.graph, full, {});
    std::vector<arcgraph::SpanningForest> parts;
    parts.reserve(b.components.size());
    for (const arcgraph::Component& c : b.components)
        parts.push_back(arcgraph::growBaselineForest(b.graph, c, incumbent, rules));
    b.forest = arcgraph::joinForests(b.graph, parts);
    b.basis  = arcgraph::shortCycleBasis(b.graph, b.forest, arcgraph::BasisCoverage::Full, sticky);
    // The map's invalidated set: every slipped band-arc.
    for (std::size_t a = 0; a < b.graph.rows.size(); ++a) {
        const arcgraph::ArcRow& row = b.graph.rows[a];
        if (row.present != 0 && row.present != full) ++b.n_band_only_arcs;
        for (std::size_t k = 0; k < in.family.bands.size(); ++k) {
            const unsigned bit = 1u << k;
            if ((row.present & bit) && (row.slipped & bit))
                b.invalidated.insert(ArcId{b.graph.stations[static_cast<std::size_t>(b.graph.row_station[a])],
                                           b.graph.sats[static_cast<std::size_t>(b.graph.row_sat[a])],
                                           in.family.bands[k]});
        }
    }
    return b;
}

const char* arMapStateName(ArMapState s) {
    switch (s) {
    case ArMapState::Same:      return "same";
    case ArMapState::Mapped:    return "mapped";
    case ArMapState::FreshLife: return "fresh_life";
    case ArMapState::FreshNew:  return "fresh_new";
    }
    return "?";
}

std::vector<ArMapState> arCycleMapStates(const arcgraph::CycleBasis& stored, const arcgraph::CycleBasis& basis,
                                         const std::set<ArcId>& invalidated) {
    std::vector<ArMapState> out(basis.cycles.size(), ArMapState::FreshNew);
    if (stored.cycles.empty()) return out;
    const std::vector<std::optional<arcgraph::Coordinates>> W =
        arcgraph::basisChangeMap(stored, basis, invalidated);
    for (std::size_t i = 0; i < W.size(); ++i) {
        if (!W[i]) {
            out[i] = touches(basis.cycles[i].cycle, invalidated) ? ArMapState::FreshLife : ArMapState::FreshNew;
            continue;
        }
        // Exact coordinates: one stored cycle at coefficient 1 is that cycle itself.
        const arcgraph::Coordinates& w = *W[i];
        out[i] = (w.size() == 1 && w[0].second == 1) ? ArMapState::Same : ArMapState::Mapped;
    }
    return out;
}

const char* arCycleKindName(arcgraph::CycleKind k) {
    switch (k) {
    case arcgraph::CycleKind::PartnerDd:   return "partner";
    case arcgraph::CycleKind::BorrowedDd:  return "borrowed";
    case arcgraph::CycleKind::Fundamental: return "fundamental";
    }
    return "?";
}

void arCycleTally(ArCycleBasisCounters& c, const ArFamilySpec& fam, const ArCycleBuild& b,
                  const std::vector<ArMapState>& states, const std::vector<arcgraph::IncumbentLink>& incumbent,
                  bool no_stored) {
    ++c.families;
    if (no_stored) ++c.first_builds;
    // Coverage per band: rows plus fallbacks against the lattice dimension.
    const std::size_t n_bands = fam.bands.size();
    std::vector<long long> rows(n_bands, 0), fund(n_bands, 0);
    for (const arcgraph::BasisCycle& bc : b.basis.cycles) {
        const std::size_t k = static_cast<std::size_t>(bc.band);
        if (k >= n_bands) continue;
        if (bc.kind == arcgraph::CycleKind::Fundamental) { ++fund[k]; continue; }
        ++rows[k];
        if (bc.kind == arcgraph::CycleKind::PartnerDd) ++c.rows_partner;
        else                                          ++c.rows_borrowed;
        if (bc.band_only) ++c.rows_band_only;
    }
    for (std::size_t k = 0; k < n_bands; ++k) {
        const std::string label = bandLabel(fam, k);
        const int dim = k < b.basis.lattice_dim.size() ? b.basis.lattice_dim[k] : 0;
        c.lattice_dim[label].push_back(static_cast<double>(dim));
        c.rows[label].push_back(static_cast<double>(rows[k]));
        c.fundamental[label] += fund[k];
        c.lattice_total      += dim;
        c.rows_fundamental   += fund[k];
        if (rows[k] + fund[k] != dim) ++c.dim_mismatch;
    }
    c.max_len = std::max(c.max_len, b.basis.max_len);
    c.components += static_cast<long long>(b.components.size());
    c.max_components = std::max(c.max_components, static_cast<int>(b.components.size()));
    c.arcs_band_only += b.n_band_only_arcs;
    // The links: kept from the incumbent, re-seated by cause, or gone with their station.
    for (const arcgraph::StationLink& L : b.forest.links) {
        if (L.parent < 0) continue;
        ++c.links;
        if (L.from_incumbent) ++c.links_kept;
    }
    for (std::size_t r = 0; r < c.reseats.size() && r < b.forest.reseats.size(); ++r)
        c.reseats[r] += b.forest.reseats[r];
    for (const arcgraph::IncumbentLink& L : incumbent)
        if (L.parent && std::find(b.graph.stations.begin(), b.graph.stations.end(), L.station) == b.graph.stations.end())
            ++c.links_station_gone;
    // The map.
    for (std::size_t i = 0; i < states.size() && i < b.basis.cycles.size(); ++i) {
        if (b.basis.cycles[i].kind == arcgraph::CycleKind::Fundamental) continue;   // never a search row
        switch (states[i]) {
        case ArMapState::Same:      ++c.map_same;       break;
        case ArMapState::Mapped:    ++c.map_mapped;     break;
        case ArMapState::FreshLife: ++c.map_fresh_life; break;
        case ArMapState::FreshNew:  ++c.map_fresh_new;  break;
        }
    }
}

void arCycleTallyLife(ArCycleBasisCounters& c, const ArcLifeStep& step) {
    ++c.arc_reads;
    switch (step.edge) {
    case ArcLifeEdge::FirstSeen:   ++c.life_first_seen; break;
    case ArcLifeEdge::Freed:       ++c.life_freed;      break;
    case ArcLifeEdge::Echo:        ++c.life_echo;       break;
    case ArcLifeEdge::StampChange:
        ++c.life_stamp;
        for (int k = 0; k < kArcLifeFieldCount; ++k)
            if (step.change & (1u << k)) ++c.life_stamp_by[static_cast<std::size_t>(k)];
        break;
    case ArcLifeEdge::None: break;
    }
}

void arCycleBasisRunEndClause(const ArCycleBasisCounters& c, bool sticky,
                              const std::shared_ptr<spdlog::logger>& log) {
    // Coverage per band.
    std::string bands;
    for (const auto& [label, dims] : c.lattice_dim) {
        const auto rit = c.rows.find(label);
        const std::vector<double> no_rows;
        const std::vector<double>& rows = (rit == c.rows.end()) ? no_rows : rit->second;
        const auto fit = c.fundamental.find(label);
        const long long fund = (fit == c.fundamental.end()) ? 0 : fit->second;
        double row_sum = 0.0;
        for (double r : rows) row_sum += r;
        const double fb_pct = (row_sum + static_cast<double>(fund)) > 0.0
                                  ? 100.0 * static_cast<double>(fund) / (row_sum + static_cast<double>(fund))
                                  : 0.0;
        bands += fmt::format("{}{}: lattice dimension p50 {:.0f}, DD rows p50 {:.0f}, {} fundamental "
                             "fallback(s) ({:.2f} %)",
                             bands.empty() ? "" : "; ", label, upperMedian(dims), upperMedian(rows),
                             fund, fb_pct);
    }
    log->info("Network (producer AR cycle basis): {} search(es), {} family build(s) ({} with "
              "no stored basis).  Coverage per band: {}.  Rows plus fallbacks differ from the "
              "lattice dimension in {} band build(s).  DD rows: {} partner, {} borrowed ({} "
              "band-only); longest cycle {} arcs; {} component(s) over the builds (at most {}); "
              "{} band-only arc reading(s).",
              c.epochs, c.families, c.first_builds, bands.empty() ? std::string("none") : bands,
              c.dim_mismatch, c.rows_partner, c.rows_borrowed, c.rows_band_only, c.max_len,
              c.components, c.max_components, c.arcs_band_only);
    log->info("Network (producer AR cycle basis): {} link(s) built, {} kept from the "
              "incumbent ({:.2f} %, {}); re-seats: {} arc gone, {} slipped, {} parent gone, {} "
              "too few common; {} incumbent link(s) whose station left the family.  Map against "
              "the stored basis (search rows): {} same, {} mapped, {} fresh through an "
              "invalidated arc, {} fresh outside the stored span.",
              c.links, c.links_kept,
              c.links > 0 ? 100.0 * static_cast<double>(c.links_kept) / static_cast<double>(c.links) : 0.0,
              sticky ? "sticky" : "NON-sticky control",
              c.reseats[static_cast<std::size_t>(arcgraph::Reseat::ArcGone)],
              c.reseats[static_cast<std::size_t>(arcgraph::Reseat::Slipped)],
              c.reseats[static_cast<std::size_t>(arcgraph::Reseat::ParentGone)],
              c.reseats[static_cast<std::size_t>(arcgraph::Reseat::TooFewCommon)],
              c.links_station_gone, c.map_same, c.map_mapped, c.map_fresh_life, c.map_fresh_new);
    std::string by_field;
    for (int k = 0; k < kArcLifeFieldCount; ++k) {
        const long long n = c.life_stamp_by[static_cast<std::size_t>(k)];
        if (n == 0) continue;
        by_field += fmt::format("{}{} {}", by_field.empty() ? "" : ", ", arcLifeFieldName(k), n);
    }
    log->info("Network (producer AR cycle basis): {} band-arc life reading(s); new lives: "
              "{} first seen, {} freed, {} stamp change(s) [{}]; {} echo(es) re-baselined.  Cost "
              "per search, p50 / max: stamps and rows {:.2f} / {:.2f} ms, graph, forest and basis "
              "{:.2f} / {:.2f} ms, map {:.2f} / {:.2f} ms.",
              c.arc_reads, c.life_first_seen, c.life_freed, c.life_stamp,
              by_field.empty() ? std::string("none") : by_field, c.life_echo,
              upperMedian(c.stamp_ms), maxOf(c.stamp_ms), upperMedian(c.build_ms), maxOf(c.build_ms),
              upperMedian(c.map_ms), maxOf(c.map_ms));
    if (c.epochs == 0 || c.rows_partner + c.rows_borrowed == 0)
        log->warn("Network (producer AR cycle basis): ARMED, but no search row was ever "
                  "built ({} search(es)); every cycle-basis number above is structurally empty "
                  "(standing invariant 4).", c.epochs);
    else if (sticky && c.links_kept == 0)
        log->warn("Network (producer AR cycle basis): ARMED and sticky, but no incumbent "
                  "link was ever kept over {} search(es): the basis was rebuilt every search "
                  "(standing invariant 4).", c.epochs);
    if (c.dim_mismatch > 0)
        log->warn("Network (producer AR cycle basis): {} band build(s) whose DD rows plus "
                  "fundamental fallbacks differ from the lattice dimension: the basis did not cover "
                  "the cycle lattice.", c.dim_mismatch);
}

}  // namespace GnssProcessor
