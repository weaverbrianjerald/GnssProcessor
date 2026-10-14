#pragma once
/**
 * @file NetworkArcGraph.hpp
 * @brief The network producer's observation graph (receivers x satellites joined by ambiguity
 *        arcs): components, two sticky spanning-forest rules, short cycle bases, the exact integer
 *        map between two bases, and a saturated lattice of fixed cycles (card AGT).
 *
 * One owner serves every caller:
 *   - the chain datum: growChainForest;
 *   - the Card-4 carry: the integer-id layer;
 *   - the datum-graph census and the emit re-base: Dsu;
 *   - the producer AR search: growBaselineForest, shortCycleBasis, basisChangeMap;
 *   - the hold of card ARL task 2: CycleLattice.
 *
 * Plain value types and free functions.  No state: callers keep incumbents in
 * NetworkCarryState.  No estimator or run-context access.
 *
 * The arc set is always an input, because the callers run at three different times:
 *   - pre-solve, from phase-bias keys;
 *   - at SB_DDBUILD, from assembly columns;
 *   - post-solve, from live columns.
 *
 * Every tie breaks by the caller's declared slot order, never by an identity's own operator<.
 *
 * ## Layers
 *
 *   1. **Integer ids** (opaque vertex ids, the Card-4 driver's space): @ref ArcKey, @ref arcgraph::Dsu,
 *      @ref arcgraph::singleTreeOf, @ref arcgraph::fundamentalCycle,
 *      @ref arcgraph::promoteBridgingArcs, @ref arcgraph::chordRestriction.
 *   2. **Typed family graph**: one constellation with one band or an L1/L2 pair, stations and
 *      satellites in dense slots (@ref arcgraph::ArcGraph), and its components.
 *   3. **Spanning forests**, two named rules with one output type: the eq34 chain walk
 *      (@ref arcgraph::growChainForest) and the baseline Prim of card ARL
 *      (@ref arcgraph::growBaselineForest).
 *   4. **Cycles and bases**: @ref arcgraph::fundamentalBasis and the unit-triangular short basis
 *      @ref arcgraph::shortCycleBasis.
 *   5. **Integer maps**: @ref arcgraph::cycleCoordinates, @ref arcgraph::basisChangeMap,
 *      @ref arcgraph::combineCycles.
 *   6. **The saturated lattice of fixed cycles**: @ref arcgraph::CycleLattice.
 *
 * ## Sign convention
 *
 * A typed cycle term is +1 when the arc is traversed station to satellite, so the four-arc cycle
 * of a chord (r, j) closed against the reference satellite k on the baseline (r, p) is the
 * classical double difference (N_rj - N_rk) - (N_pj - N_pk).  The integer-id layer keeps the
 * Card-4 convention: +1 when an arc is traversed from its lower id to its higher id.
 *
 * ## Legacy orders
 *
 * Two helpers exist only to reproduce pre-migration orders byte for byte:
 * @ref arcgraph::legacyChainVertexRank (the eq34 string vertex order) and the
 * @ref arcgraph::ForestRules::stop_at_gap / @ref arcgraph::BasisCoverage::ParentOnly pair (card
 * ARL tree_basis).  They are deleted when the retired eq34 lineage and tree_basis retire.
 */
#include "common/GnssIdentity.hpp"   // System, StationId, SatId, SignalId, ArcId

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief An undirected arc of the observation graph, identified by its two
 *        vertex ids in a canonical (ascending) orientation.
 *
 * Vertices are opaque integer ids in one flat id space per @f$(\text{const},
 * \sigma)@f$ group; the store does not distinguish receiver ids from satellite
 * ids (the cycle-space algebra is identical for any bipartite graph).  The
 * caller allocates disjoint id ranges for receivers and satellites (phase C maps
 * a receiver pool index and a satellite label to two ids).  There is at most one
 * arc between any (receiver, satellite) pair, so the unordered vertex pair is a
 * unique arc identity; @ref u < @ref v is enforced so equal arcs compare equal.
 * The +1 reference orientation of an arc is @ref u → @ref v (min → max); every
 * signed cycle sum uses that convention (the physical DD sign matters only to the
 * phase-D emit re-basing, which uses the same convention on both sides).
 *
 * Moved verbatim from NetworkZStore.hpp (card AGT, AGT-1), which includes this header.
 */
struct ArcKey {
    int u{0};  ///< lower-id endpoint (reference-orientation tail).
    int v{0};  ///< higher-id endpoint (reference-orientation head).

    ArcKey() = default;
    /// Construct in canonical (ascending) orientation from any endpoint order.
    ArcKey(int a, int b) : u(a < b ? a : b), v(a < b ? b : a) {}
    /// The canonical factory (identical to the two-arg constructor; explicit for call sites).
    static ArcKey make(int a, int b) { return ArcKey(a, b); }

    bool operator==(const ArcKey& o) const { return u == o.u && v == o.v; }
    bool operator!=(const ArcKey& o) const { return !(*this == o); }
    bool operator<(const ArcKey& o) const { return u != o.u ? u < o.u : v < o.v; }
    /// The other endpoint of the arc given one endpoint (undefined if @p w is neither).
    int other(int w) const { return w == u ? v : u; }
};

namespace arcgraph {

// ---- 1. Integer-id layer (opaque vertex ids: the Card-4 driver's space) ------------------------------

/**
 * @brief Disjoint sets over sparse integer ids, path-compressed and unranked, so a root depends
 *        on the link sequence alone.
 *
 * The link rule (@ref dsuUnite links root(a) under root(b)) is the one the eq34 chain datum, the
 * Card-4 driver and the emit re-base already share, so a caller that unites in its legacy order
 * gets its legacy roots.  Path compression changes no root.
 */
struct Dsu {
    std::unordered_map<int, int> parent;  ///< id -> parent id (a root is its own parent).
};

/**
 * @brief The representative of @p v's set; @p v enters as a singleton on first sight.
 * @param d The sets (path-compressed along the way).
 * @param v The id.
 * @return The root id of @p v's set.
 */
int dsuFind(Dsu& d, int v);

/**
 * @brief Join the sets of @p a and @p b by linking root(@p a) under root(@p b).
 *
 * The rule of the chain datum, the Card-4 driver and the emit re-base; callers pass the station
 * first, then the satellite, so every root is a satellite.
 *
 * @param d The sets.
 * @param a The id whose root is linked under the other root.
 * @param b The id whose root becomes the root of the union.
 */
void dsuUnite(Dsu& d, int a, int b);

/**
 * @brief A tree over integer ids as undirected adjacency.
 */
struct IntTree {
    std::map<int, std::set<int>> adj;       ///< Undirected adjacency.
    std::set<int>                vertices;  ///< Every vertex of the tree.
};

/**
 * @brief The tree of the given edges, or none when they close a cycle or form more than one
 *        fragment.
 *
 * This is the ZComponent constructor's contract (NetworkZStore.hpp): a duplicate edge closes a
 * cycle, an empty edge list is the empty tree.  The Card-4 driver skips the component on none.
 * One difference is declared: a self-loop ArcKey(a, a) is refused here (it is a cycle of length
 * one), where ZComponent accepts one whose vertex is new; the driver drops degenerate arcs before
 * its tree (NetworkInvariantCarryDriver.cpp, "degenerate (defensive)"), so no caller can see it.
 *
 * @param tree_edges The candidate tree arcs.
 * @return The tree, or @c std::nullopt.
 */
std::optional<IntTree> singleTreeOf(const std::vector<ArcKey>& tree_edges);

/**
 * @brief The signed fundamental cycle of an arc in a tree (ZComponent's convention).
 *
 * +1 on @p arc (traversed u to v), then the tree path from v back to u, each tree edge +1 when
 * traversed in its reference orientation (lower id to higher id) and -1 otherwise.  For an arc
 * that is itself a tree edge the result holds that arc at 0, as ZComponent's does.
 *
 * @param tree The tree.
 * @param arc  The arc (a chord, or any arc whose endpoints are tree-connected).
 * @return Arc -> signed coefficient.
 * @throws std::invalid_argument when the endpoints are not tree-connected (an endpoint not in the
 *         tree included).
 */
std::map<ArcKey, int> fundamentalCycle(const IntTree& tree, const ArcKey& arc);

/**
 * @brief Move to @p tree_edges, in chord order, every chord that joins two fragments of the
 *        partial tree (the completion the retired Card-4 lane pair groups used).
 *
 * The fragments are those of @p tree_edges; each promoted chord joins two of them, so the result
 * is still a forest.  The chords that stay keep their order.  No production caller remains since
 * the WL-3 lane carry was retired; the function is kept as tested library code (its oracle is in
 * tests/test_network_arc_graph_oracles.cpp).
 *
 * @param tree_edges [in/out] The partial tree; promoted chords are appended.
 * @param chords     [in/out] The candidate chords; promoted ones are removed.
 * @return How many chords moved.
 */
long long promoteBridgingArcs(std::vector<ArcKey>& tree_edges, std::vector<ArcKey>& chords);

/**
 * @brief The coefficient of each old chord in a cycle, in old-chord order: a Card-4 W row (the
 *        chord restriction).
 * @param cycle      The cycle (arc -> coefficient).
 * @param old_chords The old basis's chords.
 * @return One coefficient per old chord (0 where the cycle does not touch it).
 */
std::vector<int> chordRestriction(const std::map<ArcKey, int>& cycle, const std::vector<ArcKey>& old_chords);

// ---- 2. Typed family graph --------------------------------------------------------------------------

/**
 * @brief One graph family: a constellation with one band, or an L1/L2 pair sharing one graph.
 */
struct ArcFamily {
    System                sys{System::Unknown};  ///< Constellation.
    std::vector<SignalId> bands;                 ///< Band b of every bit mask below (one or two entries).
};

/**
 * @brief One (station, satellite) arc of a family with its per-band state, filled by the caller.
 */
struct ArcRow {
    StationId    station{};           ///< Receiver.
    SatId        sat{};               ///< Satellite.
    std::uint8_t present{0};          ///< Bit b: band b carries a live ambiguity.
    std::uint8_t slipped{0};          ///< Bit b: band b's integer may have changed since the caller's incumbent (chain: carry_slipped_ambs; search: free_ambs or a life-stamp change).
    double       durability{0.0};     ///< Caller's expected-life score; higher lives longer (chain: longevity rank; search: epochs of unbroken life).
    bool         eligible_host{true}; ///< May become a new tree edge in the first pass (chain: reattach hygiene).
};

/**
 * @brief One family's graph with dense slots: station slot s is vertex s, satellite slot k is
 *        vertex stations.size() + k.
 */
struct ArcGraph {
    ArcFamily                     family;       ///< Constellation and bands.
    std::vector<StationId>        stations;     ///< Station slot -> station, in the caller's declared order.
    std::vector<SatId>            sats;         ///< Satellite slot -> satellite, in the caller's declared order.
    std::vector<ArcRow>           rows;         ///< Arcs in the caller's order (it fixes the union and adjacency order).
    std::vector<int>              row_station;  ///< Arc -> station slot.
    std::vector<int>              row_sat;      ///< Arc -> satellite slot.
    std::vector<std::vector<int>> incident;     ///< Vertex -> incident arcs, in arc order.
};

/**
 * @brief Build a family graph.
 *
 * Slots follow the declared orders (a repeated declaration keeps its first slot).  Rows keep
 * their order.  Two rows of one (station, satellite) merge into the first one's position: the
 * present and slipped bits are OR-ed, the durability is the lower of the two and the host is
 * eligible only when both are (a merged arc is as durable and as eligible as its weaker band).
 * A row naming an undeclared station or satellite is refused and counted.
 *
 * @param family        Constellation and bands.
 * @param station_order Station slot order.
 * @param sat_order     Satellite slot order.
 * @param rows          The arcs, in the order the caller's legacy union and adjacency read them.
 * @param n_refused     [out, optional] Rows refused.
 * @return The graph.
 */
ArcGraph buildArcGraph(const ArcFamily& family, const std::vector<StationId>& station_order,
                       const std::vector<SatId>& sat_order, const std::vector<ArcRow>& rows,
                       int* n_refused = nullptr);

/**
 * @brief The vertex of a station slot.
 * @param g The graph.
 * @param station_slot The slot.
 * @return The vertex id (the slot itself).
 */
int stationVertex(const ArcGraph& g, int station_slot);

/**
 * @brief The vertex of a satellite slot.
 * @param g The graph.
 * @param sat_slot The slot.
 * @return The vertex id (station count + slot).
 */
int satVertex(const ArcGraph& g, int sat_slot);

/**
 * @brief True for a satellite vertex.
 * @param g The graph.
 * @param vertex The vertex id.
 * @return True when @p vertex is at or past the station range.
 */
bool isSatVertex(const ArcGraph& g, int vertex);

/**
 * @brief The arc joining a station slot and a satellite slot.
 * @param g The graph.
 * @param station_slot The station slot.
 * @param sat_slot The satellite slot.
 * @return The arc index, or -1.
 */
int arcIndex(const ArcGraph& g, int station_slot, int sat_slot);

/**
 * @brief True when the arc is present in every band of the mask.
 * @param g The graph.
 * @param arc The arc.
 * @param band_mask The bands (nonzero).
 * @return Every bit of @p band_mask set in the arc's present bits.
 */
bool usable(const ArcGraph& g, int arc, std::uint8_t band_mask);

/**
 * @brief True when the arc slipped in any band of the mask.
 * @param g The graph.
 * @param arc The arc.
 * @param band_mask The bands.
 * @return Any bit of @p band_mask set in the arc's slipped bits.
 */
bool slippedIn(const ArcGraph& g, int arc, std::uint8_t band_mask);

/**
 * @brief One connected component over the usable arcs.
 */
struct Component {
    int              root{-1};  ///< Dsu root vertex after uniting every usable arc in arc order (station under satellite), so always a satellite vertex; -1 for @ref wholeGraph.
    std::vector<int> stations;  ///< Station slots, ascending.
    std::vector<int> sats;      ///< Satellite slots, ascending.
    std::vector<int> arcs;      ///< Arcs, ascending.
};

/**
 * @brief The components of the arcs usable in a band mask.
 *
 * Every usable arc is united in arc order with @ref dsuUnite (station vertex, satellite vertex),
 * the legacy rule, so each root is the root the legacy union-find would give.
 *
 * @param g           The graph.
 * @param band_mask   The bands an arc must be present in.
 * @param vertex_rank Per vertex: the order of the components by their root; empty orders them by
 *                    root vertex id.
 * @return The components, ordered by the rank of their root.
 */
std::vector<Component> arcComponents(const ArcGraph& g, std::uint8_t band_mask, const std::vector<int>& vertex_rank);

/**
 * @brief Every station and satellite with a usable arc as one component (ARL tree_basis grows
 *        one tree over the whole family).
 * @param g The graph.
 * @param band_mask The bands an arc must be present in.
 * @return The union of every component (root -1).
 */
Component wholeGraph(const ArcGraph& g, std::uint8_t band_mask);

/**
 * @brief Cycle dimension of a component: arcs - stations - satellites + 1.
 * @param c The component (a connected one).
 * @return E - V + 1.
 */
int cycleDimension(const Component& c);

/**
 * @brief Legacy chain vertex order: receivers by the decimal string of their roster index ("R10"
 *        before "R2"), then satellites by slot.
 *
 * Kept for eq34 byte identity until a declared move retires it: the pre-migration walk named its
 * vertices "R<roster index>" and "S<sat>" and rebuilt its pass-2 frontier from a set of those
 * strings.  The caller declares satellites in their string order.
 *
 * @param g The graph.
 * @return Per vertex: its rank.
 */
std::vector<int> legacyChainVertexRank(const ArcGraph& g);

// ---- 3. Spanning forests: two rules, one output type ----------------------------------------------

/**
 * @brief The refusals and orders a growth rule applies (data; read by both rules unless marked).
 */
struct ForestRules {
    std::uint8_t band_mask{1};            ///< An arc is usable only when present in every band of the mask.
    bool refuse_slipped_first{false};     ///< Pass 1 refuses a slipped arc as a new edge or link reference; pass 2 admits it only to reach an otherwise unreached vertex.
    bool refuse_ineligible_first{false};  ///< Pass 1 refuses a non-eligible host in the same way (chain only).
    bool by_durability{false};            ///< Chain: longevity order of new edges.  Baseline: the most durable reference satellite (the maximum over common satellites of the weaker arc's durability).
    int  min_common{2};                   ///< Baseline: common satellites a link needs in the first round.
    bool stop_at_gap{false};              ///< Baseline: stop when no station reaches min_common (ARL tree_basis) instead of finishing through one common satellite.
};

/**
 * @brief One incumbent link of the baseline rule: a station, its parent and its reference
 *        satellite (no parent marks the root).
 */
struct IncumbentLink {
    StationId                station{};  ///< Linked station.
    std::optional<StationId> parent;     ///< Parent station; empty for the root.
    SatId                    ref_sat{};  ///< Reference satellite of the link.
};

/// Why an incumbent link was not kept.
enum class Reseat : std::uint8_t {
    ArcGone,       ///< The reference arc is no longer usable at the station or at the parent.
    Slipped,       ///< The reference arc slipped at the station or at the parent (refuse_slipped_first).
    ParentGone,    ///< The parent left the component, or was not in the tree when the station joined through another link.
    TooFewCommon,  ///< The parent was in the tree, but the two stations shared fewer satellites than the round required while another link was available.
    Count          ///< Number of causes (the size of SpanningForest::reseats).
};

/**
 * @brief One tree edge in attachment order.
 */
struct TreeEdge {
    int  arc{-1};                 ///< The arc.
    bool attaches_sat{false};     ///< The vertex it attached is the satellite (else the station).
    bool from_incumbent{false};   ///< Grown from the incumbent.
    bool ineligible_host{false};  ///< A new edge on a non-eligible host (the legacy hyg.cold_fallback_pins; chain only).
    bool slipped{false};          ///< Its arc slipped (the topology left no other route).
    int  pass{1};                 ///< 2 when admitted only in the second pass.
};

/**
 * @brief One baseline-rule link of this epoch.
 */
struct StationLink {
    int  station{-1};            ///< Station slot.
    int  parent{-1};             ///< Parent station slot (-1: a root).
    int  ref_sat{-1};            ///< Reference satellite slot (-1: a root).
    int  n_common{0};            ///< Satellites the two stations share in the mask.
    bool from_incumbent{false};  ///< The incumbent link was kept.
};

/**
 * @brief A spanning forest of one component (or, through @ref joinForests, of several).
 */
struct SpanningForest {
    std::vector<TreeEdge>    edges;          ///< Attachment order, every parent before its children.
    std::vector<char>        is_tree_arc;    ///< Per arc.
    std::vector<int>         parent_arc;     ///< Per vertex: the arc to its parent (-1: a root or unreached).
    std::vector<int>         station_order;  ///< Station slots in attachment order (the basis order).
    std::vector<StationLink> links;          ///< Baseline rule only, in attachment order.
    std::vector<int>         unreached;      ///< Component vertices left out (ARL stop_at_gap only).
    std::vector<int>         reseats;        ///< Incumbent links not kept, indexed by Reseat.
};

/**
 * @brief The eq34 chain rule: a two-queue BFS from the roots (r1, then bridged satellites).
 *
 * The incumbent queue drains before any new edge, so a surviving old subtree re-attaches through
 * its own old edges.  New edges are tried from the popped vertex in slot order or, under
 * @ref ForestRules::by_durability, satellites by durability then slot and stations by
 * @p station_rank, then durability, then slot.  Pass 1 refuses slipped arcs and non-eligible hosts
 * as new edges when the rules say so; pass 2 runs, when vertices are still unreached, from the
 * frontier rebuilt in @p frontier_rank order and admits everything.  Incumbent arcs are not
 * filtered (the caller de-seeds them).
 *
 * @param g              The graph.
 * @param c              The component to span.
 * @param roots          Root vertices in order (an already-attached root is skipped).
 * @param incumbent_arcs Last epoch's tree arcs still live, in seed order (their adjacency order).
 * @param rules          band_mask, refuse_slipped_first, refuse_ineligible_first, by_durability.
 * @param frontier_rank  Per vertex: the pass-2 frontier order (empty: vertex id).
 * @param station_rank   Per station slot: the longevity tie-break (the chain's coverage count;
 *                       empty: all equal).
 * @return The forest (links empty, reseats all zero).
 */
SpanningForest growChainForest(const ArcGraph& g, const Component& c, const std::vector<int>& roots,
                               const std::vector<int>& incumbent_arcs, const ForestRules& rules,
                               const std::vector<int>& frontier_rank, const std::vector<int>& station_rank);

/**
 * @brief The baseline rule: Prim on satellite bitmasks.
 *
 * The root is the incumbent root when it is in the component, else the station with the most
 * usable arcs (lowest slot on ties).  Incumbent links are kept first: a link whose reference arc
 * is gone or slipped, or whose parent left the component, fails at once; any other is kept, in
 * the incumbent order, as soon as its parent is in the tree and the two stations share the
 * current round's threshold, before every new pick.  A station whose incumbent link still waits is
 * picked only when no other outside station reaches the threshold, so a re-seated station rejoins
 * before its former children, which then keep their links (card AGT, step AGT-5).  A link that is
 * not kept is counted by cause in SpanningForest::reseats.  Then each outside station joins the
 * in-tree station it shares the most satellites with (lowest slot on ties, for both the station
 * and its partner), through the first common satellite or, under
 * @ref ForestRules::by_durability, the most durable one; the first round needs
 * @ref ForestRules::min_common common satellites, the second (unless
 * @ref ForestRules::stop_at_gap) one.  Each satellite joins with the first station that reaches
 * it.  Under @ref ForestRules::refuse_slipped_first the first pass reads only clean arcs (counting,
 * reference and satellite attachment); a second pass then reaches the rest through slipped arcs.
 * With the legacy rules (min_common 2, stop_at_gap, no incumbent, no refusal, first common) this
 * is exactly card ARL's tree_basis Prim.
 *
 * @param g         The graph.
 * @param c         The component to span.
 * @param incumbent Last epoch's links in attachment order (empty: none).
 * @param rules     band_mask, refuse_slipped_first, by_durability, min_common, stop_at_gap.
 * @return The forest with its links.
 */
SpanningForest growBaselineForest(const ArcGraph& g, const Component& c,
                                  const std::vector<IncumbentLink>& incumbent, const ForestRules& rules);

/**
 * @brief The links of a baseline-rule forest in typed form, for the caller to store as next
 *        epoch's incumbent.
 * @param g The graph.
 * @param f The forest.
 * @return One link per entry of SpanningForest::links, in order.
 */
std::vector<IncumbentLink> incumbentLinksOf(const ArcGraph& g, const SpanningForest& f);

/**
 * @brief The forests of several components of one graph as one forest.
 *
 * Edges, station orders, links and unreached vertices are concatenated in the given order; tree
 * arcs and parent arcs are merged; re-seats are summed.  A full-coverage basis of a family with
 * several components reads the joined forest (shortCycleBasis).
 *
 * @param g     The graph.
 * @param parts One forest per component, in component order.
 * @return The joined forest.
 */
SpanningForest joinForests(const ArcGraph& g, const std::vector<SpanningForest>& parts);

// ---- 4. Cycles and bases ----------------------------------------------------------------------------

/**
 * @brief One signed term: a band-tagged arc and its coefficient (+1 = station to satellite).
 */
struct CycleTerm {
    ArcId arc{};    ///< The band-tagged arc.
    int   coeff{0}; ///< Its integer coefficient.
};

/**
 * @brief An integer combination of band-tagged arcs with zero boundary in every band (a DD, a
 *        longer cycle, or a cross-band combination); terms sorted by ArcId, no zero coefficient.
 */
struct Cycle {
    std::vector<CycleTerm> terms;  ///< Sorted by arc.
};

/**
 * @brief True when every vertex of every band has zero signed degree.
 *
 * A term adds its coefficient at its (band, station) and subtracts it at its (band, satellite).
 *
 * @param x The combination.
 * @return Zero boundary everywhere.
 */
bool isCycle(const Cycle& x);

/// How a basis cycle closes its chord.
enum class CycleKind : std::uint8_t {
    PartnerDd,   ///< A DD on the station's link baseline against the link reference.
    BorrowedDd,  ///< A DD on the baseline to an earlier station.
    Fundamental  ///< The chord plus its tree path (long; counted, never searched).
};

/**
 * @brief One basis cycle: the chord it owns, its double-difference identity and its terms.
 */
struct BasisCycle {
    int       pivot_arc{-1};               ///< The chord it owns (+1), absent from every earlier cycle of its band.
    int       band{0};                     ///< Family band index.
    bool      band_only{false};            ///< The chord exists in this band only (no pair partner).
    CycleKind kind{CycleKind::PartnerDd};  ///< Closure kind.
    StationId station{};                   ///< sta_r (the pivot's station).
    StationId partner{};                   ///< ref_sta (unset for Fundamental).
    SatId     sat{};                       ///< sat_j (the pivot's satellite).
    SatId     ref_sat{};                   ///< ref_sat (unset for Fundamental).
    Cycle     cycle;                       ///< Signed band-tagged arcs.
};

/// Which chords a short basis closes.
enum class BasisCoverage : std::uint8_t {
    ParentOnly,  ///< Only chords whose satellite the link parent tracks, against the link reference (ARL tree_basis, decision identity).
    Full         ///< Every chord of every band: the whole cycle lattice.
};

/**
 * @brief A unit-triangular cycle basis.
 *
 * Each cycle owns a chord that no earlier cycle of its band contains, so the set is independent
 * and its integer coordinates exact; under Full it is a Z-basis of the cycle lattice.
 */
struct CycleBasis {
    std::vector<BasisCycle> cycles;           ///< Pivot order: stations in attachment order, shared chords before band-only ones.
    std::vector<int>        lattice_dim;      ///< Per band: E - V + C over the arcs present in that band.
    int                     n_fundamental{0}; ///< Cycles of kind Fundamental (long and imprecise: counted, never searched).
    int                     max_len{0};       ///< Longest cycle (arcs).
};

/**
 * @brief The fundamental-cycle basis of a forest in one band.
 *
 * One cycle per chord: an arc present in @p band, with both endpoints in the forest, that is not a
 * tree arc; ascending arc order.  lattice_dim holds the chord count at @p band (0 elsewhere).
 *
 * @param g    The graph.
 * @param f    The forest (its tree arcs present in @p band).
 * @param band The family band index.
 * @return The basis.
 */
CycleBasis fundamentalBasis(const ArcGraph& g, const SpanningForest& f, int band);

/**
 * @brief The short basis over a baseline-rule forest.
 *
 * Per station in attachment order: the partner DDs (common satellites of the link, ascending, the
 * reference excluded), then each remaining chord closed by a DD on the earlier station sharing the
 * most satellites with it (BorrowedDd; the station's link reference preferred as the DD
 * reference), else by its tree path (Fundamental).  A shared chord (present in every family band)
 * yields one cycle per band with identical shapes, the band-0 copy first.
 *
 * Under Full the forest is first completed per band: band-only arcs that join two fragments are
 * promoted to tree arcs of that band (promoteBridgingArcs, arc order), and the remaining band-only
 * chords are closed in their own band after every shared chord.  The forest must be grown on all
 * family bands and, under Full, span every component of that graph (joinForests); every chord of
 * every band is then closed, so each band holds lattice_dim cycles.
 *
 * With a @p sticky basis (the cycle-basis search's last basis, card AGT step AGT-5), a chord that
 * basis closed with a BorrowedDd keeps that DD's partner and reference while they still close it
 * (the partner earlier in the closing order, every term a tree arc or an earlier pivot), so the
 * borrowed closures do not flip as satellites rise and set; any other chord is closed by the rule
 * above.  Without one (the default, and every caller before AGT-5) the rule is unchanged.
 *
 * @param g        The graph.
 * @param f        The forest.
 * @param coverage ParentOnly or Full.
 * @param sticky   The last basis whose borrowed closures are kept, or null.
 * @return The basis.
 */
CycleBasis shortCycleBasis(const ArcGraph& g, const SpanningForest& f, BasisCoverage coverage,
                           const CycleBasis* sticky = nullptr);

// ---- 5. Integer maps --------------------------------------------------------------------------------

/// Sparse integer coordinates: (basis index, coefficient).
using Coordinates = std::vector<std::pair<int, long long>>;

/**
 * @brief Exact coordinates of a cycle in a unit-triangular basis, by back-substitution on the
 *        pivots.
 *
 * The highest-index pivot present fixes its coefficient, which is subtracted; the residual must
 * vanish.  The pivot of a basis cycle is its (station, satellite) term.
 *
 * @param b The basis.
 * @param x The cycle.
 * @return Coordinates ascending by basis index, or @c std::nullopt outside the span.
 */
std::optional<Coordinates> cycleCoordinates(const CycleBasis& b, const Cycle& x);

/**
 * @brief The change of basis W: for each new cycle, its coordinates in the old basis.
 *
 * A new cycle is fresh (none) when it touches an invalidated arc or leaves the old span, so a
 * slipped reference re-references the cycles that do not run through it.  On an unchanged graph W
 * is unimodular.
 *
 * @param old_basis   The stored basis.
 * @param new_basis   This epoch's basis.
 * @param invalidated Arcs whose integer may have changed (life changes, vanished arcs).
 * @return One entry per new cycle.
 */
std::vector<std::optional<Coordinates>> basisChangeMap(const CycleBasis& old_basis, const CycleBasis& new_basis,
                                                       const std::set<ArcId>& invalidated);

/**
 * @brief The cycle of an integer combination of basis cycles (a fixed decorrelated component, a
 *        lane).
 * @param b      The basis.
 * @param coeffs (basis index, coefficient) pairs.
 * @return The combination, sorted, zero terms removed.
 */
Cycle combineCycles(const CycleBasis& b, const Coordinates& coeffs);

// ---- 6. The saturated lattice of fixed cycles -------------------------------------------------------

/**
 * @brief Fixed cycles and their integers in reduced echelon form: row k owns its pivot arc at +1,
 *        absent from every other row, so the lattice is saturated in Z^E.
 *
 * Saturated: an integer cycle in the rational span of the rows has integer coordinates (its value
 * at each pivot), so the integer it implies is exact.
 */
struct CycleLattice {
    std::vector<Cycle>     rows;       ///< Fixed lattice elements.
    std::vector<long long> values;     ///< Integer of each row (cycles).
    std::map<ArcId, int>   pivot_row;  ///< Pivot arc -> row.
    /// Loop epoch each row entered (card FCF): the epoch passed to @ref latticeOffer; a row
    /// reduced by another row takes the later of the two epochs.  0 for a row offered without
    /// an epoch (every caller before card FCF).
    std::vector<long long> entered_ep;
};

/// Outcome of offering a fixed cycle.
enum class LatticeVerdict : std::uint8_t {
    Added,         ///< A new direction; the lattice grew by one.
    Confirmed,     ///< In the span, with the implied integer.
    Contradicted,  ///< In the span, with another integer (a wrong fix or a missed release).
    NotPrimitive   ///< The reduced residual has no unit entry; refused and counted.
};

/**
 * @brief Reduce a fixed cycle by the pivots, then add, confirm, contradict or refuse it.
 *
 * Added: the residual's first unit entry in arc order becomes the new pivot (the row is negated
 * when that entry is -1) and is eliminated from every other row.  The new row's entry epoch is
 * @p epoch, and every row it reduces takes @p epoch as well.
 *
 * @param L     The lattice.
 * @param c     The fixed cycle.
 * @param value Its integer.
 * @param epoch The loop epoch of the offer (card FCF; 0 for a caller that keeps no epochs).
 * @return The verdict.
 */
LatticeVerdict latticeOffer(CycleLattice& L, const Cycle& c, long long value, long long epoch = 0);

/**
 * @brief The integer the lattice implies for a cycle.
 * @param L      The lattice.
 * @param c      The cycle.
 * @param newest When not null and the cycle is in the span: the latest entry epoch of the rows
 *               the implication uses (its support; card FCF), or -1 when it uses none.
 * @return The integer, or @c std::nullopt outside the lattice's span.
 */
std::optional<long long> latticeImplied(const CycleLattice& L, const Cycle& c, long long* newest = nullptr);

/**
 * @brief Remove all dependence on one arc.
 *
 * When a row holds the arc at +-1 (the shortest such row, lowest index on ties; the pivot row
 * when the arc is a pivot), every other row is reduced by it and it is dropped: exactly one
 * dimension is lost, and the survivors re-reference (ten held DDs on a slipped reference become
 * nine; a reduced survivor takes the later of its own and the eliminating row's entry epochs).
 * When no row holds it at +-1, every row that touches it is dropped.
 *
 * What remains is the part of the lattice that holds the arc at 0, so no integer it implies
 * depends on the arc's old integer: a slip of the arc leaves every remaining value right (card
 * FCF: a cycle whose integer is in question is released through its arcs, never through the rows
 * behind it alone, since other rows can hold one of its arcs as a non-pivot term).
 *
 * @param L   The lattice.
 * @param arc The released arc.
 * @return The rows dropped.
 */
int latticeEliminateArc(CycleLattice& L, const ArcId& arc);

/**
 * @brief Plant a wrong integer on one arc (card FCF's event harness, @c fixed_cycle_plant):
 *        every row's integer moves by @p delta times the row's coefficient on @p arc.
 *
 * The lattice then implies, for every cycle in its span, its integer plus @p delta times the
 * cycle's coefficient on @p arc: the lattice of an arc whose integer it holds @p delta too high.
 *
 * @param L     The lattice.
 * @param arc   The arc.
 * @param delta The integer shift (cycles).
 * @return The rows that hold the arc (0: nothing planted).
 */
int latticeShiftArc(CycleLattice& L, const ArcId& arc, long long delta);

}  // namespace arcgraph
}  // namespace GnssProcessor
