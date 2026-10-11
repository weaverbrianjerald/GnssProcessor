#pragma once
/**
 * @file NetworkArCycleBasis.hpp
 * @brief The producer AR search on a sticky, L1/L2-consistent, full-coverage cycle basis (card
 *        AGT, step AGT-5; @c network.ambiguity_resolution.cycle_basis, log-only).
 *
 * ## What it replaces
 *
 * Card ARL's @c tree_basis (through filter/NetworkArcGraph since AGT-2) grows one Prim tree per
 * (constellation, signal), from scratch at every search, and closes only the chords whose
 * satellite the link parent tracks.  On the AGT-0 references that covers 344 of the 362
 * cycle directions per band at 60 stations and 2 017 of 2 052 at 300, keeps 97.7 and 95.0 %
 * of the DD identities from one epoch to the next (p50), and keeps as few as 42.9 and 82.1 %
 * of one epoch's fixed DDs in the next epoch's basis (card AGT section 7).  A hold that carries
 * integers across epochs needs a basis whose cycles outlive the epoch.
 *
 * ## One family's basis at one search
 *
 *  1. **Family** (@ref arCycleFamilies): per constellation, the two signals with the most arcs
 *     share one graph (a pair family, bands in the pool's string order); every further signal
 *     is a one-band family.
 *  2. **Rows** (built by the seam): one arc per (station, satellite) with a present bit per
 *     band, a slipped bit per band (@ref ArcLifeStep::slipped: this epoch's free-epoch latch,
 *     or a change of the arc's life stamp, filter/NetworkArcLife.hpp) and a durability (the
 *     epochs since the weaker band's life began).
 *  3. **Forest** (@ref arCycleBuild): per component of the arcs present in every band,
 *     @ref arcgraph::growBaselineForest under @ref arCycleForestRules (two common satellites,
 *     then one; slipped arcs refused in the first pass; the most durable reference satellite),
 *     seeded with the last search's links (the incumbent) unless @c cycle_basis_sticky is
 *     false.  A station whose incumbent link waits for its parent rejoins after that parent,
 *     so a re-seat moves one link and not its subtree.  The component forests are joined.
 *  4. **Basis**: @ref arcgraph::shortCycleBasis with Full coverage: one cycle per chord of
 *     every band, a DD against the link parent (PartnerDd) or against an earlier station
 *     sharing the most satellites (BorrowedDd); band-only arcs complete each band's forest and
 *     close in their own band; a chord no DD can close is a Fundamental fallback, counted and
 *     never searched.  Each band then holds its lattice dimension, E - V + C, in cycles.  When
 *     sticky, a chord the last basis borrowed for keeps that partner and reference while they
 *     still close it, so a borrowed DD does not flip each time a common-satellite count does.
 *  5. **Map** (@ref arCycleMapStates): @ref arcgraph::basisChangeMap against the stored basis,
 *     the slipped band-arcs invalidated.  Each cycle is the same cycle as a stored one, an
 *     integer combination of stored cycles, fresh through an invalidated arc, or fresh outside
 *     the stored span (a new arc, or no stored basis).
 *
 * The seam (networkProducerAr, filter/NetworkProducerAr.cpp) turns each PartnerDd and
 * BorrowedDd cycle into one search row, searches one block per station
 * (@ref netar::searchBlockKey), and stores the family's memory after the search.
 *
 * ## Memory
 *
 * @c ctx.carry.ar_basis, one @ref ArBasisMemory per family, keyed by the family's bands.  Not
 * snapshotted and not fingerprinted: the search is log-only, and a resumed leg rebuilds (a
 * build with no stored basis, counted).
 *
 * ## What one search hands the lattice observatory (step AGT-6)
 *
 * @ref ArEpochFixes: whether the cycle basis was built, the single-signal DDs the search
 * determined (each as its cycle, with its integer and identity), and every band-arc's life
 * reading of this search (@ref ArArcReading).  The observatory (filter/NetworkArLattice.hpp)
 * releases a lattice arc on that reading, so the search and the observatory share one rule of
 * what ends a life (@ref arcLifeStep, its echo rule included) and keep no second record of it.
 * Filled only under @c network.ambiguity_resolution.trace_lattice.
 */

#include "common/GnssIdentity.hpp"     // System, StationId, SatId, SignalId, ArcId
#include "filter/NetworkArcGraph.hpp"  // arcgraph:: the graph, the forest, the basis, the map
#include "filter/NetworkArcLife.hpp"   // ArcLifeTrack, ArcLifeStep

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

/// The key of one family's memory: its bands, in band order.
using ArFamilyKey = std::vector<SignalId>;

/// A search DD's identity: (signal, sta_r, ref_sta, sat_j, ref_sat).
using ArDdIdentity = std::tuple<SignalId, StationId, StationId, SatId, SatId>;

/// One family's memory between searches (@c ctx.carry.ar_basis; file banner).
struct ArBasisMemory {
    long long                            last_search_ep{0};  ///< Loop epoch of the search that stored it (0 = never).
    std::vector<arcgraph::IncumbentLink> links;      ///< That search's links in attachment order (the incumbent).
    arcgraph::CycleBasis                 basis;      ///< That search's basis (typed by ArcId, so it outlives the epoch's slots).
    std::map<ArcId, ArcLifeTrack>        arcs;       ///< Each band-arc that search met: its stamp and the epoch its life began.
    std::map<ArDdIdentity, long long>    first_ep;   ///< Each DD of that basis: the first epoch of its unbroken run in the basis.
};

/// One search family: a constellation's bands and the pool signal name of each.
struct ArFamilySpec {
    System                   sys{System::Unknown};  ///< Constellation.
    std::vector<std::string> signals;               ///< Pool signal (phase observation code) per band.
    std::vector<SignalId>    bands;                 ///< The same signals, typed.
};

/**
 * @brief The families of one constellation.
 *
 * The two signals with the most arcs form a pair family (ties broken by the pool's string
 * order; the pair's bands in that order); every further signal is a one-band family, in
 * pool order.  A signal @ref SignalId::fromObsCode refuses is left out.
 *
 * @param sys          The constellation.
 * @param signal_arcs  Per pool signal, in the pool's string order: its arc count.
 * @return The families (empty when no signal parses).
 */
std::vector<ArFamilySpec> arCycleFamilies(System sys,
                                          const std::vector<std::pair<std::string, std::size_t>>& signal_arcs);

/**
 * @brief The forest rules of the cycle basis: every family band in the mask, two common
 *        satellites then one (no stop at a gap), slipped arcs refused in the first pass, the
 *        most durable reference satellite.
 * @param band_mask The family's bands.
 * @return The rules.
 */
arcgraph::ForestRules arCycleForestRules(std::uint8_t band_mask);

/// One family's arcs at one search, as the seam read them from its pool.
struct ArCycleInput {
    arcgraph::ArcFamily           family;    ///< Constellation and bands.
    std::vector<StationId>        stations;  ///< Station slots: ascending roster index.
    std::vector<SatId>            sats;      ///< Satellite slots: the pool's string order.
    std::vector<arcgraph::ArcRow> rows;      ///< One per (station, satellite): present, slipped, durability.
};

/// One family's basis at one search.
struct ArCycleBuild {
    arcgraph::ArcGraph               graph;        ///< The family graph.
    std::vector<arcgraph::Component> components;   ///< Components of the arcs present in every band (root order).
    arcgraph::SpanningForest         forest;       ///< The component forests, joined.
    arcgraph::CycleBasis             basis;        ///< The Full short basis.
    std::set<ArcId>                  invalidated;  ///< The slipped band-arcs (the map's invalidated set).
    int                              n_band_only_arcs{0};  ///< Arcs present in some band of the family but not all.
};

/**
 * @brief One family's forest and basis (steps 3 and 4 of the file banner).
 * @param in        The family's arcs.
 * @param incumbent The last search's links (empty: none, or the non-sticky control).
 * @param sticky    The last search's basis, whose borrowed closures are kept while they still
 *                  close their chords (@ref arcgraph::shortCycleBasis); null for none, or the
 *                  non-sticky control.
 * @return The graph, components, joined forest, basis and invalidated arcs.
 */
ArCycleBuild arCycleBuild(const ArCycleInput& in, const std::vector<arcgraph::IncumbentLink>& incumbent,
                          const arcgraph::CycleBasis* sticky = nullptr);

/// How a basis cycle relates to the stored basis (step 5 of the file banner).
enum class ArMapState : std::uint8_t {
    Same,       ///< Identical to one stored cycle.
    Mapped,     ///< An integer combination of stored cycles (re-referenced).
    FreshLife,  ///< Fresh: it runs through an invalidated arc.
    FreshNew    ///< Fresh: outside the stored span (a new arc, or no stored basis).
};

/**
 * @brief The trace name of a map state: same, mapped, fresh_life, fresh_new.
 * @param s The state.
 * @return The name.
 */
const char* arMapStateName(ArMapState s);

/**
 * @brief The map state of every cycle of @p basis against @p stored.
 * @param stored      The last search's basis (empty: every cycle is FreshNew).
 * @param basis       This search's basis.
 * @param invalidated The arcs whose integer may have changed since @p stored.
 * @return One state per cycle of @p basis.
 */
std::vector<ArMapState> arCycleMapStates(const arcgraph::CycleBasis& stored, const arcgraph::CycleBasis& basis,
                                         const std::set<ArcId>& invalidated);

/**
 * @brief The trace name of a searched cycle's kind: partner, borrowed (fundamental for a
 *        fallback, which is never a search row).
 * @param k The kind.
 * @return The name.
 */
const char* arCycleKindName(arcgraph::CycleKind k);

/// Run tallies of the cycle basis (standing invariant 4: an armed basis that never kept a link
/// or built a row says so).  All zero unless @c cycle_basis is on.
struct ArCycleBasisCounters {
    long long epochs{0};           ///< Searches that built the cycle basis.
    long long families{0};         ///< Family builds, summed over searches.
    long long first_builds{0};     ///< ...of those, with no stored basis (the first search, a new family, a resumed leg).
    std::map<std::string, std::vector<double>> lattice_dim;  ///< Per band (cons and signal): E - V + C per build.
    std::map<std::string, std::vector<double>> rows;         ///< Per band: DD rows (PartnerDd and BorrowedDd) per build.
    std::map<std::string, long long>           fundamental;  ///< Per band: fundamental fallbacks, summed.
    long long lattice_total{0};    ///< Lattice dimensions summed over band builds.
    long long rows_fundamental{0}; ///< Fundamental fallbacks summed over band builds.
    long long dim_mismatch{0};     ///< Band builds whose rows plus fallbacks differ from the lattice dimension (0 expected).
    long long rows_partner{0};     ///< PartnerDd rows, summed.
    long long rows_borrowed{0};    ///< BorrowedDd rows, summed.
    long long rows_band_only{0};   ///< ...of the rows, band-only ones.
    int       max_len{0};          ///< Longest cycle of any build (arcs; a DD has four).
    long long components{0};       ///< Components, summed over builds.
    int       max_components{0};   ///< Most components in one build.
    long long arcs_band_only{0};   ///< Arcs present in some family band but not all, summed over builds.
    // --- the links
    long long links{0};            ///< Non-root links built, summed.
    long long links_kept{0};       ///< ...of those, incumbent links kept.
    std::array<long long, static_cast<std::size_t>(arcgraph::Reseat::Count)> reseats{};  ///< Incumbent links not kept, by cause.
    long long links_station_gone{0};  ///< Incumbent links whose station left the family graph.
    // --- the map against the stored basis
    long long map_same{0};         ///< Cycles identical to a stored one.
    long long map_mapped{0};       ///< Cycles mapped by an integer combination.
    long long map_fresh_life{0};   ///< Fresh through an invalidated arc.
    long long map_fresh_new{0};    ///< Fresh outside the stored span.
    // --- the lives
    long long arc_reads{0};        ///< Band-arc stamp reads, summed.
    long long life_first_seen{0};  ///< New lives: an arc the last search did not meet.
    long long life_freed{0};       ///< New lives: the free-epoch latch.
    long long life_stamp{0};       ///< New lives: a stamp change.
    std::array<long long, kArcLifeFieldCount> life_stamp_by{};  ///< ...by changed field (a change can carry several).
    long long life_echo{0};        ///< Stamp changes read as the echo of the life the last search began.
    // --- the cost, per search (ms)
    std::vector<double> stamp_ms;  ///< Reading the stamps and building the rows.
    std::vector<double> build_ms;  ///< The graph, the forests and the basis (the module).
    std::vector<double> map_ms;    ///< The map against the stored basis.
};

/// One band-arc as one search read its life (the lattice observatory's release input).
struct ArArcReading {
    ArcLifeEdge  edge{ArcLifeEdge::None};  ///< @ref ArcLifeStep::edge: a new life (FirstSeen, Freed, StampChange) or not (None, Echo).
    unsigned     change{0};                ///< @ref ArcLifeStep::change: the @ref ArcLifeField bits that changed since the last search.
    std::uint8_t free_cause{0};            ///< @c RelevelEpochState::free_cause bits of the arc's ambiguity key this epoch (0 unless freed).
};

/// One single-signal DD the search determined, as a cycle.
struct ArFixedCycle {
    arcgraph::Cycle cycle;     ///< +(r,j) - (r,k) - (p,j) + (p,k) in its band (the basis cycle).
    long long       value{0};  ///< The integer the search determined (cycles; the trace's fixed_int).
    ArDdIdentity    id{};      ///< (signal, sta_r, ref_sta, sat_j, ref_sat).
};

/// What one search hands the lattice observatory (file banner).  Empty (searched false) unless
/// @c network.ambiguity_resolution.trace_lattice, which requires the cycle basis.
struct ArEpochFixes {
    bool                          searched{false};  ///< The cycle basis was built at this search (the search block ran).
    std::vector<ArFixedCycle>     fixed;            ///< The determined DDs (the trace's kept rows), in basis order.
    std::map<ArcId, ArArcReading> arcs;             ///< Every band-arc this search read, with its life reading.
};

/**
 * @brief Tally one family build.
 * @param c         The run's counters.
 * @param fam       The family.
 * @param b         Its build.
 * @param states    Its map states.
 * @param incumbent The incumbent links it was seeded with.
 * @param no_stored True when the family had no stored basis.
 */
void arCycleTally(ArCycleBasisCounters& c, const ArFamilySpec& fam, const ArCycleBuild& b,
                  const std::vector<ArMapState>& states, const std::vector<arcgraph::IncumbentLink>& incumbent,
                  bool no_stored);

/**
 * @brief Tally one band-arc's life reading.
 * @param c    The run's counters.
 * @param step The reading.
 */
void arCycleTallyLife(ArCycleBasisCounters& c, const ArcLifeStep& step);

/**
 * @brief The run-end clause of the cycle basis (a clause of the producer AR run-end report):
 *        coverage per band against the lattice dimension, rows by kind, links kept and
 *        re-seats by cause, the map, the lives and the cost; a warning when the basis is armed
 *        and never built a row, or (sticky) never kept a link.
 * @param c      The run's counters.
 * @param sticky @c network.ambiguity_resolution.cycle_basis_sticky.
 * @param log    The run-end logger.
 */
void arCycleBasisRunEndClause(const ArCycleBasisCounters& c, bool sticky,
                              const std::shared_ptr<spdlog::logger>& log);

}  // namespace GnssProcessor
