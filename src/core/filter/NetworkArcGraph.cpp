/**
 * @file NetworkArcGraph.cpp
 * @brief The network producer's observation graph (see the header): the integer-id layer, the
 *        typed family graph, the two forest rules, the short cycle basis, the integer maps and
 *        the lattice of fixed cycles.
 */

#include "filter/NetworkArcGraph.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace GnssProcessor {
namespace arcgraph {

namespace {

// ============================== shared helpers ==============================

/// An empty forest sized for a graph: no tree arc, no parent, the re-seat tally at zero.
SpanningForest emptyForest(const ArcGraph& g) {
    SpanningForest f;
    f.is_tree_arc.assign(g.rows.size(), 0);
    f.parent_arc.assign(g.stations.size() + g.sats.size(), -1);
    f.reseats.assign(static_cast<std::size_t>(Reseat::Count), 0);
    return f;
}

/// The band-tagged identity of an arc in one family band.
ArcId arcIdOf(const ArcGraph& g, int arc, int band) {
    const std::size_t a = static_cast<std::size_t>(arc);
    return ArcId{g.stations[static_cast<std::size_t>(g.row_station[a])],
                 g.sats[static_cast<std::size_t>(g.row_sat[a])],
                 g.family.bands[static_cast<std::size_t>(band)]};
}

/// True when the arc is present in one band.
bool presentIn(const ArcGraph& g, int arc, int band) {
    return (g.rows[static_cast<std::size_t>(arc)].present & (1u << band)) != 0;
}

/// The mask of every family band.
std::uint8_t fullMask(const ArcGraph& g) {
    return static_cast<std::uint8_t>((1u << g.family.bands.size()) - 1u);
}

/// A sparse accumulator turned into a cycle: sorted by arc, zero terms dropped.
Cycle cycleOf(const std::map<ArcId, long long>& acc) {
    Cycle x;
    x.terms.reserve(acc.size());
    for (const auto& [arc, v] : acc) {
        if (v == 0) continue;
        if (v > std::numeric_limits<int>::max() || v < std::numeric_limits<int>::min())
            throw std::overflow_error("arcgraph: cycle coefficient exceeds int");
        x.terms.push_back(CycleTerm{arc, static_cast<int>(v)});
    }
    return x;
}

/// The coefficient of one arc in a sorted cycle (0 when absent).
long long coeffOf(const Cycle& x, const ArcId& arc) {
    const auto it = std::lower_bound(x.terms.begin(), x.terms.end(), arc,
                                     [](const CycleTerm& t, const ArcId& a) { return t.arc < a; });
    return (it != x.terms.end() && it->arc == arc) ? it->coeff : 0;
}

/// x + s * y over two sorted cycles.
Cycle addScaled(const Cycle& x, long long s, const Cycle& y) {
    std::map<ArcId, long long> acc;
    for (const CycleTerm& t : x.terms) acc[t.arc] += t.coeff;
    for (const CycleTerm& t : y.terms) acc[t.arc] += s * t.coeff;
    return cycleOf(acc);
}

/// The other endpoint of an arc, as a vertex.
int otherEnd(const ArcGraph& g, int arc, int vertex) {
    const int sv = stationVertex(g, g.row_station[static_cast<std::size_t>(arc)]);
    const int kv = satVertex(g, g.row_sat[static_cast<std::size_t>(arc)]);
    return vertex == sv ? kv : sv;
}

/// One popcount of a 64-bit word, as the ARL Prim counts.
int popcount64(std::uint64_t w) { return static_cast<int>(std::bitset<64>(w).count()); }

/// Satellite bitmasks of a set of stations (64 satellites per word).
struct BitRows {
    std::size_t                words{0};  ///< Words per station.
    std::vector<std::uint64_t> bits;      ///< Station-major words.

    void init(std::size_t n_sta, std::size_t n_sat) {
        words = (n_sat + 63) / 64;
        bits.assign(n_sta * words, 0);
    }
    void set(std::size_t r, std::size_t k) { bits[r * words + k / 64] |= (std::uint64_t{1} << (k % 64)); }
    bool test(std::size_t r, std::size_t k) const {
        return (bits[r * words + k / 64] >> (k % 64)) & 1u;
    }
    int common(std::size_t a, std::size_t b) const {
        int c = 0;
        for (std::size_t w = 0; w < words; ++w) c += popcount64(bits[a * words + w] & bits[b * words + w]);
        return c;
    }
};

// ============================== tree index ==============================

/// Parent arcs, depths and tree ids of the trees formed by a set of tree arcs.
struct TreeIndex {
    std::vector<int> parent_arc;  ///< Per vertex (-1 at a tree's first vertex).
    std::vector<int> depth;       ///< Per vertex.
    std::vector<int> tree_id;     ///< Per vertex; a vertex with no tree arc is a tree of its own.
};

/// Index the trees of the given tree arcs (breadth first from the lowest vertex of each tree).
TreeIndex indexTrees(const ArcGraph& g, const std::vector<char>& is_tree) {
    const int nv = static_cast<int>(g.stations.size() + g.sats.size());
    TreeIndex t;
    t.parent_arc.assign(static_cast<std::size_t>(nv), -1);
    t.depth.assign(static_cast<std::size_t>(nv), 0);
    t.tree_id.assign(static_cast<std::size_t>(nv), -1);
    int next_id = 0;
    for (int s = 0; s < nv; ++s) {
        if (t.tree_id[static_cast<std::size_t>(s)] >= 0) continue;
        t.tree_id[static_cast<std::size_t>(s)] = next_id;
        std::deque<int> q{s};
        while (!q.empty()) {
            const int x = q.front();
            q.pop_front();
            for (int a : g.incident[static_cast<std::size_t>(x)]) {
                if (!is_tree[static_cast<std::size_t>(a)]) continue;
                const int y = otherEnd(g, a, x);
                if (t.tree_id[static_cast<std::size_t>(y)] >= 0) continue;
                t.tree_id[static_cast<std::size_t>(y)] = next_id;
                t.parent_arc[static_cast<std::size_t>(y)] = a;
                t.depth[static_cast<std::size_t>(y)] = t.depth[static_cast<std::size_t>(x)] + 1;
                q.push_back(y);
            }
        }
        ++next_id;
    }
    return t;
}

/// The chord plus its tree path in one band (station to satellite +1), or none when the chord's
/// endpoints lie in different trees.
std::optional<Cycle> treePathCycle(const ArcGraph& g, const TreeIndex& t, int chord, int band) {
    int a = satVertex(g, g.row_sat[static_cast<std::size_t>(chord)]);         // walks up from the satellite
    int b = stationVertex(g, g.row_station[static_cast<std::size_t>(chord)]); // walks up from the station
    if (t.tree_id[static_cast<std::size_t>(a)] != t.tree_id[static_cast<std::size_t>(b)]) return std::nullopt;
    std::map<ArcId, long long> acc;
    acc[arcIdOf(g, chord, band)] += 1;   // the chord, station to satellite
    // The cycle runs station -> chord -> satellite -> (up to the meeting vertex) -> (down to the
    // station).  Going up from the satellite side an arc is traversed child to parent; going down to
    // the station side it is traversed parent to child.  +1 whenever the traversal leaves a station.
    while (a != b) {
        if (t.depth[static_cast<std::size_t>(a)] >= t.depth[static_cast<std::size_t>(b)]) {
            const int pa = t.parent_arc[static_cast<std::size_t>(a)];
            acc[arcIdOf(g, pa, band)] += isSatVertex(g, a) ? -1 : +1;
            a = otherEnd(g, pa, a);
        } else {
            const int pb = t.parent_arc[static_cast<std::size_t>(b)];
            acc[arcIdOf(g, pb, band)] += isSatVertex(g, b) ? +1 : -1;
            b = otherEnd(g, pb, b);
        }
    }
    return cycleOf(acc);
}

/// The double difference (r, p; j, k) in one band: +(r,j) -(r,k) -(p,j) +(p,k).  The four terms
/// are sorted directly (a map per cycle dominated the 300-station basis build, card AGT section
/// 9); equal arcs merge and a zero drops, as through cycleOf.
Cycle ddCycle(const ArcGraph& g, int a_rj, int a_rk, int a_pj, int a_pk, int band) {
    std::array<CycleTerm, 4> t{{{arcIdOf(g, a_rj, band), +1}, {arcIdOf(g, a_rk, band), -1},
                                {arcIdOf(g, a_pj, band), -1}, {arcIdOf(g, a_pk, band), +1}}};
    std::sort(t.begin(), t.end(), [](const CycleTerm& x, const CycleTerm& y) { return x.arc < y.arc; });
    Cycle x;
    x.terms.reserve(4);
    for (const CycleTerm& term : t) {
        if (!x.terms.empty() && x.terms.back().arc == term.arc) x.terms.back().coeff += term.coeff;
        else x.terms.push_back(term);
    }
    x.terms.erase(std::remove_if(x.terms.begin(), x.terms.end(), [](const CycleTerm& u) { return u.coeff == 0; }),
                  x.terms.end());
    return x;
}

}  // namespace

// ============================== 1. integer-id layer ==============================

int dsuFind(Dsu& d, int v) {
    const auto it = d.parent.find(v);
    if (it == d.parent.end()) {
        d.parent.emplace(v, v);
        return v;
    }
    int r = v;
    while (d.parent[r] != r) r = d.parent[r];
    while (d.parent[v] != r) {
        const int nv = d.parent[v];
        d.parent[v] = r;
        v = nv;
    }
    return r;
}

void dsuUnite(Dsu& d, int a, int b) {
    const int ra = dsuFind(d, a), rb = dsuFind(d, b);
    if (ra != rb) d.parent[ra] = rb;
}

std::optional<IntTree> singleTreeOf(const std::vector<ArcKey>& tree_edges) {
    IntTree t;
    Dsu d;
    for (const ArcKey& e : tree_edges) {
        // Same root: the edge closes a cycle (a duplicate or a self-loop included).
        if (dsuFind(d, e.u) == dsuFind(d, e.v)) return std::nullopt;
        dsuUnite(d, e.u, e.v);
        t.adj[e.u].insert(e.v);
        t.adj[e.v].insert(e.u);
        t.vertices.insert(e.u);
        t.vertices.insert(e.v);
    }
    if (!t.vertices.empty()) {
        const int r = dsuFind(d, *t.vertices.begin());
        for (int w : t.vertices)
            if (dsuFind(d, w) != r) return std::nullopt;   // more than one fragment
    }
    return t;
}

std::map<ArcKey, int> fundamentalCycle(const IntTree& tree, const ArcKey& arc) {
    std::map<ArcKey, int> cyc;
    cyc[arc] += 1;   // the arc in its reference orientation
    // Breadth-first parent walk from v towards u; the tree path is unique, so the neighbour order
    // does not change the result.
    const int from = arc.v, to = arc.u;
    std::map<int, int> parent;
    parent[from] = from;
    std::deque<int> q{from};
    bool found = (from == to);
    while (!q.empty() && !found) {
        const int x = q.front();
        q.pop_front();
        const auto it = tree.adj.find(x);
        if (it == tree.adj.end()) continue;
        for (int y : it->second) {
            if (parent.count(y)) continue;
            parent[y] = x;
            if (y == to) { found = true; break; }
            q.push_back(y);
        }
    }
    if (!found) throw std::invalid_argument("arcgraph::fundamentalCycle: endpoints not tree-connected");
    // Each step of the path from v to u is traversed parent -> child of the walk.
    for (int cur = to; cur != from;) {
        const int par = parent.at(cur);
        cyc[ArcKey(par, cur)] += (par < cur) ? +1 : -1;
        cur = par;
    }
    return cyc;
}

long long promoteBridgingArcs(std::vector<ArcKey>& tree_edges, std::vector<ArcKey>& chords) {
    Dsu fragments;
    for (const ArcKey& t : tree_edges) dsuUnite(fragments, t.u, t.v);
    std::vector<ArcKey> kept;
    kept.reserve(chords.size());
    long long promoted = 0;
    for (const ArcKey& c : chords) {
        if (dsuFind(fragments, c.u) != dsuFind(fragments, c.v)) {
            dsuUnite(fragments, c.u, c.v);
            tree_edges.push_back(c);
            ++promoted;
        } else {
            kept.push_back(c);
        }
    }
    chords.swap(kept);
    return promoted;
}

std::vector<int> chordRestriction(const std::map<ArcKey, int>& cycle, const std::vector<ArcKey>& old_chords) {
    std::vector<int> w(old_chords.size(), 0);
    for (std::size_t j = 0; j < old_chords.size(); ++j) {
        const auto it = cycle.find(old_chords[j]);
        if (it != cycle.end()) w[j] = it->second;
    }
    return w;
}

// ============================== 2. typed family graph ==============================

ArcGraph buildArcGraph(const ArcFamily& family, const std::vector<StationId>& station_order,
                       const std::vector<SatId>& sat_order, const std::vector<ArcRow>& rows,
                       int* n_refused) {
    ArcGraph g;
    g.family = family;
    std::map<StationId, int> station_slot;   // identity lookup only; slots follow the declared order
    std::map<SatId, int>     sat_slot;
    for (const StationId& s : station_order)
        if (station_slot.emplace(s, static_cast<int>(g.stations.size())).second) g.stations.push_back(s);
    for (const SatId& k : sat_order)
        if (sat_slot.emplace(k, static_cast<int>(g.sats.size())).second) g.sats.push_back(k);

    int refused = 0;
    const std::size_t n_sat = g.sats.size();
    std::vector<int> arc_of(g.stations.size() * n_sat, -1);   // (station slot, satellite slot) -> arc
    for (const ArcRow& row : rows) {
        const auto si = station_slot.find(row.station);
        const auto ki = sat_slot.find(row.sat);
        if (si == station_slot.end() || ki == sat_slot.end()) { ++refused; continue; }
        int& slot = arc_of[static_cast<std::size_t>(si->second) * n_sat + static_cast<std::size_t>(ki->second)];
        if (slot < 0) {
            slot = static_cast<int>(g.rows.size());
            g.rows.push_back(row);
            g.row_station.push_back(si->second);
            g.row_sat.push_back(ki->second);
            continue;
        }
        ArcRow& m = g.rows[static_cast<std::size_t>(slot)];   // a second row of one arc: merge
        m.present       = static_cast<std::uint8_t>(m.present | row.present);
        m.slipped       = static_cast<std::uint8_t>(m.slipped | row.slipped);
        m.durability    = std::min(m.durability, row.durability);
        m.eligible_host = m.eligible_host && row.eligible_host;
    }
    g.incident.assign(g.stations.size() + g.sats.size(), {});
    for (std::size_t a = 0; a < g.rows.size(); ++a) {
        g.incident[static_cast<std::size_t>(stationVertex(g, g.row_station[a]))].push_back(static_cast<int>(a));
        g.incident[static_cast<std::size_t>(satVertex(g, g.row_sat[a]))].push_back(static_cast<int>(a));
    }
    if (n_refused) *n_refused = refused;
    return g;
}

int stationVertex(const ArcGraph& g, int station_slot) {
    (void)g;
    return station_slot;
}

int satVertex(const ArcGraph& g, int sat_slot) { return static_cast<int>(g.stations.size()) + sat_slot; }

bool isSatVertex(const ArcGraph& g, int vertex) { return vertex >= static_cast<int>(g.stations.size()); }

int arcIndex(const ArcGraph& g, int station_slot, int sat_slot) {
    if (station_slot < 0 || station_slot >= static_cast<int>(g.stations.size())) return -1;
    for (int a : g.incident[static_cast<std::size_t>(station_slot)])
        if (g.row_sat[static_cast<std::size_t>(a)] == sat_slot) return a;
    return -1;
}

bool usable(const ArcGraph& g, int arc, std::uint8_t band_mask) {
    return band_mask != 0 && (g.rows[static_cast<std::size_t>(arc)].present & band_mask) == band_mask;
}

bool slippedIn(const ArcGraph& g, int arc, std::uint8_t band_mask) {
    return (g.rows[static_cast<std::size_t>(arc)].slipped & band_mask) != 0;
}

std::vector<Component> arcComponents(const ArcGraph& g, std::uint8_t band_mask, const std::vector<int>& vertex_rank) {
    const int n_arcs = static_cast<int>(g.rows.size());
    Dsu d;
    for (int a = 0; a < n_arcs; ++a)   // the legacy union: station under satellite, in arc order
        if (usable(g, a, band_mask))
            dsuUnite(d, stationVertex(g, g.row_station[static_cast<std::size_t>(a)]),
                     satVertex(g, g.row_sat[static_cast<std::size_t>(a)]));
    std::map<int, std::size_t> index_of_root;
    std::vector<Component> comps;
    for (int a = 0; a < n_arcs; ++a) {
        if (!usable(g, a, band_mask)) continue;
        const int s = g.row_station[static_cast<std::size_t>(a)], k = g.row_sat[static_cast<std::size_t>(a)];
        const int root = dsuFind(d, stationVertex(g, s));
        const auto [it, fresh] = index_of_root.emplace(root, comps.size());
        if (fresh) {
            comps.emplace_back();
            comps.back().root = root;
        }
        Component& c = comps[it->second];
        c.arcs.push_back(a);
        c.stations.push_back(s);
        c.sats.push_back(k);
    }
    for (Component& c : comps) {
        std::sort(c.stations.begin(), c.stations.end());
        c.stations.erase(std::unique(c.stations.begin(), c.stations.end()), c.stations.end());
        std::sort(c.sats.begin(), c.sats.end());
        c.sats.erase(std::unique(c.sats.begin(), c.sats.end()), c.sats.end());
    }
    const auto rankOf = [&](int root) {
        return vertex_rank.empty() ? root : vertex_rank[static_cast<std::size_t>(root)];
    };
    std::sort(comps.begin(), comps.end(),
              [&](const Component& x, const Component& y) { return rankOf(x.root) < rankOf(y.root); });
    return comps;
}

Component wholeGraph(const ArcGraph& g, std::uint8_t band_mask) {
    Component c;
    for (int a = 0; a < static_cast<int>(g.rows.size()); ++a) {
        if (!usable(g, a, band_mask)) continue;
        c.arcs.push_back(a);
        c.stations.push_back(g.row_station[static_cast<std::size_t>(a)]);
        c.sats.push_back(g.row_sat[static_cast<std::size_t>(a)]);
    }
    std::sort(c.stations.begin(), c.stations.end());
    c.stations.erase(std::unique(c.stations.begin(), c.stations.end()), c.stations.end());
    std::sort(c.sats.begin(), c.sats.end());
    c.sats.erase(std::unique(c.sats.begin(), c.sats.end()), c.sats.end());
    return c;
}

int cycleDimension(const Component& c) {
    return static_cast<int>(c.arcs.size()) - static_cast<int>(c.stations.size())
         - static_cast<int>(c.sats.size()) + 1;
}

std::vector<int> legacyChainVertexRank(const ArcGraph& g) {
    const std::size_t nst = g.stations.size();
    std::vector<int> by_name(nst);
    for (std::size_t s = 0; s < nst; ++s) by_name[s] = static_cast<int>(s);
    std::sort(by_name.begin(), by_name.end(), [&](int a, int b) {
        return std::to_string(g.stations[static_cast<std::size_t>(a)].index)
             < std::to_string(g.stations[static_cast<std::size_t>(b)].index);
    });
    std::vector<int> rank(nst + g.sats.size());
    for (std::size_t i = 0; i < nst; ++i) rank[static_cast<std::size_t>(by_name[i])] = static_cast<int>(i);
    for (std::size_t k = 0; k < g.sats.size(); ++k) rank[nst + k] = static_cast<int>(nst + k);
    return rank;
}

// ============================== 3a. the chain rule ==============================

namespace {

/// The new-edge candidates of a popped vertex, in the order the chain walk tries them: its usable
/// arcs by slot or, under longevity, satellites by durability and stations by rank then
/// durability (slot on every tie).
std::vector<int> chainCandidates(const ArcGraph& g, int v, const ForestRules& rules,
                                 const std::vector<int>& station_rank) {
    const auto dur = [&](int a) { return g.rows[static_cast<std::size_t>(a)].durability; };
    const auto rankOf = [&](int s) { return station_rank.empty() ? 0 : station_rank[static_cast<std::size_t>(s)]; };
    std::vector<int> arcs;
    for (int a : g.incident[static_cast<std::size_t>(v)])
        if (usable(g, a, rules.band_mask)) arcs.push_back(a);
    if (!isSatVertex(g, v)) {
        std::sort(arcs.begin(), arcs.end(), [&](int x, int y) {
            if (rules.by_durability && dur(x) != dur(y)) return dur(x) > dur(y);
            return g.row_sat[static_cast<std::size_t>(x)] < g.row_sat[static_cast<std::size_t>(y)];
        });
    } else {
        std::sort(arcs.begin(), arcs.end(), [&](int x, int y) {
            const int sx = g.row_station[static_cast<std::size_t>(x)], sy = g.row_station[static_cast<std::size_t>(y)];
            if (rules.by_durability) {
                if (rankOf(sx) != rankOf(sy)) return rankOf(sx) > rankOf(sy);
                if (dur(x) != dur(y)) return dur(x) > dur(y);
            }
            return sx < sy;
        });
    }
    return arcs;
}

/// The pass-2 frontier: every attached vertex, in @p frontier_rank order (vertex id when empty).
std::vector<int> chainFrontier(const std::vector<char>& visited, const std::vector<int>& frontier_rank) {
    std::vector<int> frontier;
    for (std::size_t v = 0; v < visited.size(); ++v)
        if (visited[v]) frontier.push_back(static_cast<int>(v));
    std::sort(frontier.begin(), frontier.end(), [&](int x, int y) {
        return frontier_rank.empty() ? x < y
                                     : frontier_rank[static_cast<std::size_t>(x)] < frontier_rank[static_cast<std::size_t>(y)];
    });
    return frontier;
}

}  // namespace

SpanningForest growChainForest(const ArcGraph& g, const Component& c, const std::vector<int>& roots,
                               const std::vector<int>& incumbent_arcs, const ForestRules& rules,
                               const std::vector<int>& frontier_rank, const std::vector<int>& station_rank) {
    const std::uint8_t mask = rules.band_mask;
    const int nv = static_cast<int>(g.stations.size() + g.sats.size());
    SpanningForest f = emptyForest(g);

    // The incumbent adjacency, per vertex in seed order.
    std::vector<std::vector<int>> inc_arcs(static_cast<std::size_t>(nv));
    for (int a : incumbent_arcs) {
        if (a < 0 || a >= static_cast<int>(g.rows.size()) || !usable(g, a, mask)) continue;
        inc_arcs[static_cast<std::size_t>(stationVertex(g, g.row_station[static_cast<std::size_t>(a)]))].push_back(a);
        inc_arcs[static_cast<std::size_t>(satVertex(g, g.row_sat[static_cast<std::size_t>(a)]))].push_back(a);
    }

    std::vector<char> visited(static_cast<std::size_t>(nv), 0);
    std::size_t n_visited = 0;
    std::deque<int> inc_q, new_q;
    int pass = 1;
    const auto attach = [&](int v) {
        visited[static_cast<std::size_t>(v)] = 1;
        ++n_visited;
        inc_q.push_back(v);
        new_q.push_back(v);
        if (!isSatVertex(g, v)) f.station_order.push_back(v);
    };
    const auto record = [&](int a, int child, bool from_incumbent) {
        TreeEdge e;
        e.arc             = a;
        e.attaches_sat    = isSatVertex(g, child);
        e.from_incumbent  = from_incumbent;
        e.ineligible_host = !from_incumbent && !g.rows[static_cast<std::size_t>(a)].eligible_host;
        e.slipped         = slippedIn(g, a, mask);
        e.pass            = pass;
        f.edges.push_back(e);
        f.is_tree_arc[static_cast<std::size_t>(a)] = 1;
        f.parent_arc[static_cast<std::size_t>(child)] = a;
    };
    for (int r : roots)
        if (r >= 0 && r < nv && !visited[static_cast<std::size_t>(r)]) attach(r);

    const std::size_t comp_vertices = c.stations.size() + c.sats.size();
    bool allow_ineligible = !rules.refuse_ineligible_first;
    bool allow_slipped    = !rules.refuse_slipped_first;
    const auto newEdgeOk = [&](int a) {
        if (!allow_slipped && slippedIn(g, a, mask)) return false;
        return g.rows[static_cast<std::size_t>(a)].eligible_host || allow_ineligible;
    };
    for (int p = 0; p < 2; ++p) {
        if (p == 1) {
            // Pass 2: only when a refusal is armed and a vertex is still unreached; the frontier is
            // every attached vertex in frontier_rank order, and everything is admitted.
            if ((allow_ineligible && allow_slipped) || n_visited >= comp_vertices) break;
            allow_ineligible = true;
            allow_slipped    = true;
            pass             = 2;
            for (int v : chainFrontier(visited, frontier_rank)) new_q.push_back(v);
        }
        while (!inc_q.empty() || !new_q.empty()) {
            if (!inc_q.empty()) {   // the incumbent queue drains before any new edge
                const int v = inc_q.front();
                inc_q.pop_front();
                for (int a : inc_arcs[static_cast<std::size_t>(v)]) {
                    const int u = otherEnd(g, a, v);
                    if (visited[static_cast<std::size_t>(u)]) continue;
                    record(a, u, /*from_incumbent=*/true);
                    attach(u);
                }
            } else {
                const int v = new_q.front();
                new_q.pop_front();
                for (int a : chainCandidates(g, v, rules, station_rank)) {
                    const int u = otherEnd(g, a, v);
                    if (visited[static_cast<std::size_t>(u)]) continue;
                    if (!newEdgeOk(a)) continue;
                    record(a, u, /*from_incumbent=*/false);
                    attach(u);
                }
            }
        }
    }
    return f;
}

// ============================== 3b. the baseline rule ==============================

namespace {

/// One incumbent link as the growth sees it.
struct IncumbentEntry {
    enum class State : std::uint8_t { Ignored, Pending, Kept, Failed };
    State  state{State::Ignored};
    int    station{-1};               ///< Local station.
    int    parent{-1};                ///< Local parent station.
    int    ref{-1};                   ///< Local reference satellite.
    Reseat cause{Reseat::ArcGone};    ///< Failed: why.
    bool   parent_seen{false};        ///< The parent was in the tree when the station joined otherwise.
};

/// The working tables of one baseline growth: the component in local indices.
struct BaselineGrowth {
    const ArcGraph&    g;
    const ForestRules& rules;
    SpanningForest&    f;
    std::vector<int>   station_slot;   ///< Local station -> station slot (ascending).
    std::vector<int>   sat_slot;       ///< Local satellite -> satellite slot (ascending).
    std::vector<int>   local_station;  ///< Station slot -> local station (-1 outside).
    std::vector<int>   local_sat;      ///< Satellite slot -> local satellite (-1 outside).
    std::vector<int>   arc_at;         ///< Local (station, satellite) -> usable arc (-1 none).
    BitRows            usable_bits;    ///< Usable arcs.
    BitRows            clean_bits;     ///< Usable arcs that did not slip (= usable_bits without the refusal).
    std::vector<char>  in_tree;        ///< Per local station.
    std::vector<char>  sat_in;         ///< Per local satellite.
    std::vector<IncumbentEntry> incumbent;  ///< One entry per incumbent link, in its order.
    std::vector<int>   entry_of;       ///< Local station -> its incumbent entry (-1 none).
    std::vector<int>   best_n;         ///< Per outside station: common satellites with its best in-tree partner.
    std::vector<int>   best_p;         ///< Per outside station: that partner (nSta() when none).

    BaselineGrowth(const ArcGraph& g_, const Component& c, const ForestRules& r, SpanningForest& f_)
        : g(g_), rules(r), f(f_), station_slot(c.stations), sat_slot(c.sats) {
        const std::size_t n = station_slot.size(), m = sat_slot.size();
        local_station.assign(g.stations.size(), -1);
        local_sat.assign(g.sats.size(), -1);
        for (std::size_t i = 0; i < n; ++i) local_station[static_cast<std::size_t>(station_slot[i])] = static_cast<int>(i);
        for (std::size_t k = 0; k < m; ++k) local_sat[static_cast<std::size_t>(sat_slot[k])] = static_cast<int>(k);
        arc_at.assign(n * m, -1);
        usable_bits.init(n, m);
        clean_bits.init(n, m);
        for (int a : c.arcs) {
            if (!usable(g, a, rules.band_mask)) continue;
            const int i = local_station[static_cast<std::size_t>(g.row_station[static_cast<std::size_t>(a)])];
            const int k = local_sat[static_cast<std::size_t>(g.row_sat[static_cast<std::size_t>(a)])];
            if (i < 0 || k < 0) continue;
            arc_at[static_cast<std::size_t>(i) * m + static_cast<std::size_t>(k)] = a;
            usable_bits.set(static_cast<std::size_t>(i), static_cast<std::size_t>(k));
            if (!(rules.refuse_slipped_first && slippedIn(g, a, rules.band_mask)))
                clean_bits.set(static_cast<std::size_t>(i), static_cast<std::size_t>(k));
        }
        in_tree.assign(n, 0);
        sat_in.assign(m, 0);
        entry_of.assign(n, -1);
        best_n.assign(n, 0);
        best_p.assign(n, static_cast<int>(n));
    }

    std::size_t nSta() const { return station_slot.size(); }
    std::size_t nSat() const { return sat_slot.size(); }
    int arc(std::size_t i, std::size_t k) const { return arc_at[i * nSat() + k]; }
    double durability(std::size_t i, std::size_t k) const {
        return g.rows[static_cast<std::size_t>(arc(i, k))].durability;
    }
    int localStationOf(const StationId& s) const {
        const auto it = std::find(g.stations.begin(), g.stations.end(), s);
        return it == g.stations.end() ? -1 : local_station[static_cast<std::size_t>(it - g.stations.begin())];
    }
    int localSatOf(const SatId& k) const {
        const auto it = std::find(g.sats.begin(), g.sats.end(), k);
        return it == g.sats.end() ? -1 : local_sat[static_cast<std::size_t>(it - g.sats.begin())];
    }

    /// Record one tree edge and the vertex it attaches.
    void addEdge(int a, bool attaches_sat, bool from_incumbent, int pass) {
        TreeEdge e;
        e.arc            = a;
        e.attaches_sat   = attaches_sat;
        e.from_incumbent = from_incumbent;
        e.slipped        = slippedIn(g, a, rules.band_mask);
        e.pass           = pass;
        f.edges.push_back(e);
        f.is_tree_arc[static_cast<std::size_t>(a)] = 1;
        const int child = attaches_sat ? satVertex(g, g.row_sat[static_cast<std::size_t>(a)])
                                       : stationVertex(g, g.row_station[static_cast<std::size_t>(a)]);
        f.parent_arc[static_cast<std::size_t>(child)] = a;
    }

    /// Attach the station's satellites not yet in the tree, ascending; pass 1 reads clean arcs only.
    void attachSatsOf(std::size_t i, int pass) {
        const BitRows& bits = (pass == 1) ? clean_bits : usable_bits;
        for (std::size_t k = 0; k < nSat(); ++k) {
            if (sat_in[k] || !bits.test(i, k)) continue;
            sat_in[k] = 1;
            addEdge(arc(i, k), /*attaches_sat=*/true, /*from_incumbent=*/false, pass);
        }
    }

    /// A station joins: note whether its incumbent parent was already in the tree (the re-seat
    /// cause, should its incumbent link not be the one that attached it).
    void join(std::size_t i) {
        in_tree[i] = 1;
        f.station_order.push_back(station_slot[i]);
        const int q = entry_of[i];
        if (q >= 0) {
            IncumbentEntry& e = incumbent[static_cast<std::size_t>(q)];
            e.parent_seen = e.parent >= 0 && in_tree[static_cast<std::size_t>(e.parent)];
        }
    }

    /// Place the root station.
    void placeRoot(std::size_t i, bool from_incumbent) {
        join(i);
        f.links.push_back(StationLink{station_slot[i], -1, -1, 0, from_incumbent});
        attachSatsOf(i, 1);
    }

    /// Link station i to the in-tree station p through the reference satellite k.
    void link(std::size_t i, std::size_t p, std::size_t k, bool from_incumbent, int pass) {
        join(i);
        f.links.push_back(StationLink{station_slot[i], station_slot[p], sat_slot[k],
                                      usable_bits.common(i, p), from_incumbent});
        addEdge(arc(i, k), /*attaches_sat=*/false, from_incumbent, pass);
        attachSatsOf(i, pass);
    }

    /// Each outside station's best in-tree partner over @p bits (lowest slot on ties): the result
    /// an exhaustive scan in (station, partner) order would give.
    void initBest(const BitRows& bits) {
        const std::size_t n = nSta();
        for (std::size_t r = 0; r < n; ++r) {
            best_n[r] = 0;
            best_p[r] = static_cast<int>(n);
            if (in_tree[r]) continue;
            for (std::size_t p = 0; p < n; ++p) {
                if (!in_tree[p]) continue;
                const int cn = bits.common(r, p);
                if (cn > best_n[r] || (cn == best_n[r] && static_cast<int>(p) < best_p[r])) {
                    best_n[r] = cn;
                    best_p[r] = static_cast<int>(p);
                }
            }
        }
    }

    /// The new in-tree station @p j may be a better partner for every outside station.
    void updateBest(std::size_t j, const BitRows& bits) {
        for (std::size_t s = 0; s < nSta(); ++s) {
            if (in_tree[s]) continue;
            const int cn = bits.common(s, j);
            if (cn > best_n[s] || (cn == best_n[s] && static_cast<int>(j) < best_p[s])) {
                best_n[s] = cn;
                best_p[s] = static_cast<int>(j);
            }
        }
    }

    /// The reference satellite of a new link among the common satellites of @p bits: the first
    /// one, or under by_durability the one whose weaker arc is the most durable (first on ties).
    std::size_t reference(std::size_t i, std::size_t p, const BitRows& bits) const {
        std::size_t best = nSat();
        double best_life = 0.0;
        for (std::size_t k = 0; k < nSat(); ++k) {
            if (!bits.test(i, k) || !bits.test(p, k)) continue;
            if (!rules.by_durability) return k;
            const double life = std::min(durability(i, k), durability(p, k));
            if (best == nSat() || life > best_life) { best = k; best_life = life; }
        }
        return best;
    }
};

/// Read the incumbent links: a link whose reference arc is gone or slipped, or whose parent left
/// the component, fails at once; the others wait for their parent and enough common satellites.
void baselineReadIncumbent(BaselineGrowth& w, const std::vector<IncumbentLink>& links) {
    using State = IncumbentEntry::State;
    w.incumbent.assign(links.size(), IncumbentEntry{});
    for (std::size_t q = 0; q < links.size(); ++q) {
        const IncumbentLink& L = links[q];
        IncumbentEntry& e = w.incumbent[q];
        if (!L.parent) continue;   // the root entry (read by baselinePlaceRoot)
        e.station = w.localStationOf(L.station);
        if (e.station < 0 || w.entry_of[static_cast<std::size_t>(e.station)] >= 0) continue;   // not in this component
        w.entry_of[static_cast<std::size_t>(e.station)] = static_cast<int>(q);
        e.parent = w.localStationOf(*L.parent);
        e.ref    = w.localSatOf(L.ref_sat);
        const std::size_t i = static_cast<std::size_t>(e.station);
        const bool has_parent = e.parent >= 0;
        const std::size_t p = has_parent ? static_cast<std::size_t>(e.parent) : 0;
        e.state = State::Failed;
        if (e.ref < 0 || w.arc(i, static_cast<std::size_t>(e.ref)) < 0
            || (has_parent && w.arc(p, static_cast<std::size_t>(e.ref)) < 0))
            e.cause = Reseat::ArcGone;
        else if (!w.clean_bits.test(i, static_cast<std::size_t>(e.ref))
                 || (has_parent && !w.clean_bits.test(p, static_cast<std::size_t>(e.ref))))
            e.cause = Reseat::Slipped;
        else if (!has_parent)
            e.cause = Reseat::ParentGone;
        else
            e.state = State::Pending;
    }
}

/// The root: the incumbent root when it is in the component, else the station with the most
/// usable arcs (lowest slot on ties).
void baselinePlaceRoot(BaselineGrowth& w, const std::vector<IncumbentLink>& links) {
    if (w.nSta() == 0) return;
    for (const IncumbentLink& L : links) {
        if (L.parent) continue;
        const int i = w.localStationOf(L.station);
        if (i < 0) continue;
        w.placeRoot(static_cast<std::size_t>(i), /*from_incumbent=*/true);
        return;
    }
    std::size_t root = 0;
    int root_seen = -1;
    for (std::size_t i = 0; i < w.nSta(); ++i) {
        const int seen = w.usable_bits.common(i, i);
        if (seen > root_seen) { root_seen = seen; root = i; }
    }
    w.placeRoot(root, /*from_incumbent=*/false);
}

/// Keep, in their order, the waiting incumbent links whose parent is in the tree and whose
/// stations share at least @p threshold satellites in @p bits.
void baselineKeepIncumbent(BaselineGrowth& w, const BitRows& bits, int threshold, int pass) {
    for (IncumbentEntry& e : w.incumbent) {
        if (e.state != IncumbentEntry::State::Pending) continue;
        const std::size_t i = static_cast<std::size_t>(e.station), p = static_cast<std::size_t>(e.parent),
                          k = static_cast<std::size_t>(e.ref);
        if (w.in_tree[i] || !w.in_tree[p] || !w.sat_in[k] || bits.common(i, p) < threshold) continue;
        e.state = IncumbentEntry::State::Kept;
        w.link(i, p, k, /*from_incumbent=*/true, pass);
        w.updateBest(i, bits);
    }
}

/// True when station @p i still has an incumbent link waiting for its parent.
bool baselineWaiting(const BaselineGrowth& w, std::size_t i) {
    const int q = w.entry_of[i];
    return q >= 0 && w.incumbent[static_cast<std::size_t>(q)].state == IncumbentEntry::State::Pending;
}

/// One Prim round on @p bits: before every pick the waiting incumbent links are kept first; then
/// the outside station with the most common satellites with its best partner (lowest slot on
/// ties) joins it, until no outside station reaches @p threshold.  A station whose incumbent link
/// still waits is picked only when no other outside station reaches the threshold: a re-seated
/// station is re-attached before its former children, which then keep their links (the subtree
/// survives the re-seat instead of reversing under it).  Without an incumbent no station waits.
void baselinePrimRound(BaselineGrowth& w, const BitRows& bits, int threshold, int pass) {
    const std::size_t n = w.nSta();
    w.initBest(bits);
    while (true) {
        baselineKeepIncumbent(w, bits, threshold, pass);
        std::size_t r_pick = n;
        int n_pick = 0;
        for (int with_waiting = 0; with_waiting < 2 && (r_pick == n || n_pick < threshold); ++with_waiting) {
            r_pick = n;
            n_pick = 0;
            for (std::size_t r = 0; r < n; ++r)
                if (!w.in_tree[r] && (with_waiting || !baselineWaiting(w, r)) && w.best_n[r] > n_pick) {
                    n_pick = w.best_n[r];
                    r_pick = r;
                }
        }
        if (r_pick == n || n_pick < threshold) return;   // the rest share too few satellites with the tree
        const std::size_t p_pick = static_cast<std::size_t>(w.best_p[r_pick]);
        w.link(r_pick, p_pick, w.reference(r_pick, p_pick, bits), /*from_incumbent=*/false, pass);
        w.updateBest(r_pick, bits);
    }
}

/// Pass 2 of a slip-refusing growth: the satellites the in-tree stations reach only through
/// slipped arcs join with the first station in attachment order, then Prim runs on every usable
/// arc.
void baselineSecondPass(BaselineGrowth& w) {
    const std::vector<int> order = w.f.station_order;
    for (int slot : order) w.attachSatsOf(static_cast<std::size_t>(w.local_station[static_cast<std::size_t>(slot)]), 2);
    baselinePrimRound(w, w.usable_bits, std::max(1, w.rules.min_common), 2);
    baselinePrimRound(w, w.usable_bits, 1, 2);
}

/// Count the incumbent links not kept, by cause.
void baselineCountReseats(BaselineGrowth& w) {
    for (const IncumbentEntry& e : w.incumbent) {
        Reseat cause;
        if (e.state == IncumbentEntry::State::Failed) {
            cause = e.cause;
        } else if (e.state == IncumbentEntry::State::Pending) {
            const bool seen = w.in_tree[static_cast<std::size_t>(e.station)]
                                  ? e.parent_seen
                                  : static_cast<bool>(w.in_tree[static_cast<std::size_t>(e.parent)]);
            cause = seen ? Reseat::TooFewCommon : Reseat::ParentGone;
        } else {
            continue;
        }
        ++w.f.reseats[static_cast<std::size_t>(cause)];
    }
}

/// The component's vertices left out of the tree (stations, then satellites).
void baselineCollectUnreached(BaselineGrowth& w) {
    for (std::size_t i = 0; i < w.nSta(); ++i)
        if (!w.in_tree[i]) w.f.unreached.push_back(stationVertex(w.g, w.station_slot[i]));
    for (std::size_t k = 0; k < w.nSat(); ++k)
        if (!w.sat_in[k]) w.f.unreached.push_back(satVertex(w.g, w.sat_slot[k]));
}

}  // namespace

SpanningForest growBaselineForest(const ArcGraph& g, const Component& c,
                                  const std::vector<IncumbentLink>& incumbent, const ForestRules& rules) {
    SpanningForest f = emptyForest(g);
    BaselineGrowth w(g, c, rules, f);
    baselineReadIncumbent(w, incumbent);
    baselinePlaceRoot(w, incumbent);
    baselinePrimRound(w, w.clean_bits, std::max(1, rules.min_common), 1);
    if (!rules.stop_at_gap) {
        baselinePrimRound(w, w.clean_bits, 1, 1);   // finish through one common satellite
        if (rules.refuse_slipped_first) baselineSecondPass(w);
    }
    baselineCountReseats(w);
    baselineCollectUnreached(w);
    return f;
}

std::vector<IncumbentLink> incumbentLinksOf(const ArcGraph& g, const SpanningForest& f) {
    std::vector<IncumbentLink> out;
    out.reserve(f.links.size());
    for (const StationLink& L : f.links) {
        IncumbentLink x;
        x.station = g.stations[static_cast<std::size_t>(L.station)];
        if (L.parent >= 0) x.parent = g.stations[static_cast<std::size_t>(L.parent)];
        if (L.ref_sat >= 0) x.ref_sat = g.sats[static_cast<std::size_t>(L.ref_sat)];
        out.push_back(x);
    }
    return out;
}

SpanningForest joinForests(const ArcGraph& g, const std::vector<SpanningForest>& parts) {
    SpanningForest out = emptyForest(g);
    for (const SpanningForest& p : parts) {
        out.edges.insert(out.edges.end(), p.edges.begin(), p.edges.end());
        out.station_order.insert(out.station_order.end(), p.station_order.begin(), p.station_order.end());
        out.links.insert(out.links.end(), p.links.begin(), p.links.end());
        out.unreached.insert(out.unreached.end(), p.unreached.begin(), p.unreached.end());
        for (std::size_t a = 0; a < p.is_tree_arc.size() && a < out.is_tree_arc.size(); ++a)
            if (p.is_tree_arc[a]) out.is_tree_arc[a] = 1;
        for (std::size_t v = 0; v < p.parent_arc.size() && v < out.parent_arc.size(); ++v)
            if (p.parent_arc[v] >= 0) out.parent_arc[v] = p.parent_arc[v];
        for (std::size_t r = 0; r < p.reseats.size() && r < out.reseats.size(); ++r) out.reseats[r] += p.reseats[r];
    }
    return out;
}

// ============================== 4. cycles and bases ==============================

bool isCycle(const Cycle& x) {
    std::map<std::pair<SignalId, int>, long long> at_station;
    std::map<std::pair<SignalId, SatId>, long long> at_sat;
    for (const CycleTerm& t : x.terms) {
        at_station[{t.arc.signal, t.arc.station.index}] += t.coeff;
        at_sat[{t.arc.signal, t.arc.sat}] -= t.coeff;
    }
    for (const auto& [v, s] : at_station) if (s != 0) return false;
    for (const auto& [v, s] : at_sat) if (s != 0) return false;
    return true;
}

CycleBasis fundamentalBasis(const ArcGraph& g, const SpanningForest& f, int band) {
    CycleBasis b;
    b.lattice_dim.assign(g.family.bands.size(), 0);
    std::vector<char> tree(g.rows.size(), 0);
    for (std::size_t a = 0; a < g.rows.size(); ++a)
        tree[a] = static_cast<char>(f.is_tree_arc[a] && presentIn(g, static_cast<int>(a), band));
    const TreeIndex t = indexTrees(g, tree);
    for (int a = 0; a < static_cast<int>(g.rows.size()); ++a) {
        if (tree[static_cast<std::size_t>(a)] || !presentIn(g, a, band)) continue;
        std::optional<Cycle> x = treePathCycle(g, t, a, band);
        if (!x) continue;   // its endpoints are not in one tree: not a chord of this forest
        BasisCycle bc;
        bc.pivot_arc = a;
        bc.band      = band;
        bc.band_only = !usable(g, a, fullMask(g));
        bc.kind      = CycleKind::Fundamental;
        bc.station   = g.stations[static_cast<std::size_t>(g.row_station[static_cast<std::size_t>(a)])];
        bc.sat       = g.sats[static_cast<std::size_t>(g.row_sat[static_cast<std::size_t>(a)])];
        bc.cycle     = std::move(*x);
        b.max_len    = std::max(b.max_len, static_cast<int>(bc.cycle.terms.size()));
        b.cycles.push_back(std::move(bc));
        ++b.n_fundamental;
        ++b.lattice_dim[static_cast<std::size_t>(band)];
    }
    return b;
}

namespace {

/// The working tables of one short-basis build.
struct ShortBasisBuild {
    const ArcGraph&       g;
    const SpanningForest& f;
    CycleBasis&           out;
    std::size_t           n_bands;
    std::uint8_t          full;           ///< Every family band: a "shared" arc is present in all of them.
    std::vector<int>      order;          ///< Closing order: forest stations in attachment order, then the rest by slot.
    std::vector<int>      position;       ///< Station slot -> position in @ref order (-1 absent).
    std::vector<int>      link_parent;    ///< Station slot -> link parent slot (-1 none).
    std::vector<int>      link_ref;       ///< Station slot -> link reference satellite slot (-1 none).
    std::vector<int>      arc_at;         ///< Station slot x satellite slot -> arc (-1 none).
    BitRows               shared_bits;    ///< Per station slot: satellites with a shared arc.
    std::vector<BitRows>  band_bits;      ///< Per band, per station slot: satellites with an arc in that band.
    std::vector<std::vector<char>> tree;  ///< Per band, per arc: a tree arc of that band.
    std::vector<std::vector<char>> closed;///< Per band, per arc: the pivot of an emitted cycle.
    std::vector<TreeIndex> index;         ///< Per band: the band's trees.
    /// The sticky basis's borrowed closures: (band, or -1 for a shared chord; chord station slot;
    /// chord satellite slot) -> (partner slot, reference slot).  Empty without a sticky basis.
    std::map<std::tuple<int, int, int>, std::pair<int, int>> prev_borrowed;

    ShortBasisBuild(const ArcGraph& g_, const SpanningForest& f_, CycleBasis& out_)
        : g(g_), f(f_), out(out_), n_bands(g_.family.bands.size()), full(fullMask(g_)) {
        const std::size_t nst = g.stations.size(), nsat = g.sats.size();
        position.assign(nst, -1);
        for (int s : f.station_order)
            if (position[static_cast<std::size_t>(s)] < 0) {
                position[static_cast<std::size_t>(s)] = static_cast<int>(order.size());
                order.push_back(s);
            }
        for (std::size_t s = 0; s < nst; ++s)
            if (position[s] < 0 && !g.incident[s].empty()) {
                position[s] = static_cast<int>(order.size());
                order.push_back(static_cast<int>(s));
            }
        link_parent.assign(nst, -1);
        link_ref.assign(nst, -1);
        for (const StationLink& L : f.links)
            if (L.parent >= 0) {
                link_parent[static_cast<std::size_t>(L.station)] = L.parent;
                link_ref[static_cast<std::size_t>(L.station)]    = L.ref_sat;
            }
        arc_at.assign(nst * nsat, -1);
        shared_bits.init(nst, nsat);
        band_bits.assign(n_bands, BitRows{});
        for (BitRows& br : band_bits) br.init(nst, nsat);
        for (std::size_t a = 0; a < g.rows.size(); ++a) {
            const std::size_t s = static_cast<std::size_t>(g.row_station[a]), k = static_cast<std::size_t>(g.row_sat[a]);
            arc_at[s * nsat + k] = static_cast<int>(a);
            if (usable(g, static_cast<int>(a), full)) shared_bits.set(s, k);
            for (std::size_t b = 0; b < n_bands; ++b)
                if (presentIn(g, static_cast<int>(a), static_cast<int>(b))) band_bits[b].set(s, k);
        }
        tree.assign(n_bands, std::vector<char>(g.rows.size(), 0));
        closed.assign(n_bands, std::vector<char>(g.rows.size(), 0));
    }

    bool shared(int a) const { return a >= 0 && usable(g, a, full); }
    bool present(int a, std::size_t b) const { return a >= 0 && presentIn(g, a, static_cast<int>(b)); }
    /// A non-pivot term may be a tree arc of its band or an earlier pivot of that band.
    bool available(int a, std::size_t b) const {
        return present(a, b) && (tree[b][static_cast<std::size_t>(a)] || closed[b][static_cast<std::size_t>(a)]);
    }
    bool availableShared(int a) const {
        if (!shared(a)) return false;
        for (std::size_t b = 0; b < n_bands; ++b) if (!available(a, b)) return false;
        return true;
    }
    bool isChordShared(int a) const {
        if (!shared(a) || f.is_tree_arc[static_cast<std::size_t>(a)]) return false;
        for (std::size_t b = 0; b < n_bands; ++b) if (closed[b][static_cast<std::size_t>(a)]) return false;
        return true;
    }
    int arc(int s, int k) const {
        return (s < 0 || k < 0) ? -1 : arc_at[static_cast<std::size_t>(s) * g.sats.size() + static_cast<std::size_t>(k)];
    }

    /// Emit one cycle and mark its pivot closed in its band.
    void emit(int a_pivot, std::size_t b, bool band_only, CycleKind kind, int partner, int ref, Cycle x) {
        BasisCycle bc;
        bc.pivot_arc = a_pivot;
        bc.band      = static_cast<int>(b);
        bc.band_only = band_only;
        bc.kind      = kind;
        bc.station   = g.stations[static_cast<std::size_t>(g.row_station[static_cast<std::size_t>(a_pivot)])];
        bc.sat       = g.sats[static_cast<std::size_t>(g.row_sat[static_cast<std::size_t>(a_pivot)])];
        if (kind != CycleKind::Fundamental) {
            bc.partner = g.stations[static_cast<std::size_t>(partner)];
            bc.ref_sat = g.sats[static_cast<std::size_t>(ref)];
        } else {
            ++out.n_fundamental;
        }
        bc.cycle   = std::move(x);
        out.max_len = std::max(out.max_len, static_cast<int>(bc.cycle.terms.size()));
        closed[b][static_cast<std::size_t>(a_pivot)] = 1;
        out.cycles.push_back(std::move(bc));
    }

    /// A DD closure (r, e; j, k) in every band of @p bands, the band-0 copy first.
    void emitDd(int r, int e, int j, int k, CycleKind kind, bool band_only, const std::vector<std::size_t>& bands) {
        const int a_rj = arc(r, j), a_rk = arc(r, k), a_ej = arc(e, j), a_ek = arc(e, k);
        for (std::size_t b : bands)
            emit(a_rj, b, band_only, kind, e, k, ddCycle(g, a_rj, a_rk, a_ej, a_ek, static_cast<int>(b)));
    }

    /// The borrowed closure of the chord (r, j): the sticky basis's closure of the same chord when
    /// it still closes it (an earlier station, every term usable), else the earlier station
    /// sharing the most satellites with r in @p bits (lowest slot on ties) that has a valid DD
    /// reference k (r's link reference preferred, else the lowest slot).  @p ok accepts the arcs a
    /// DD term may use; @p band is the chord's band, -1 for a shared chord.  Returns (e, k) or
    /// (-1, -1).
    template <class Ok> std::pair<int, int> borrowed(int r, int j, const BitRows& bits, Ok ok, int band) const {
        const auto kept = prev_borrowed.find({band, r, j});
        if (kept != prev_borrowed.end()) {
            const auto [e, k] = kept->second;
            if (e >= 0 && k >= 0 && k != j && position[static_cast<std::size_t>(e)] >= 0
                && position[static_cast<std::size_t>(e)] < position[static_cast<std::size_t>(r)]
                && ok(arc(e, j)) && ok(arc(r, k)) && ok(arc(e, k)))
                return {e, k};
        }
        std::vector<std::pair<int, int>> cands;   // (-common, slot)
        for (int e : order) {
            if (position[static_cast<std::size_t>(e)] >= position[static_cast<std::size_t>(r)]) break;
            if (!ok(arc(e, j))) continue;
            cands.emplace_back(-bits.common(static_cast<std::size_t>(r), static_cast<std::size_t>(e)), e);
        }
        std::sort(cands.begin(), cands.end());
        for (const auto& [neg, e] : cands) {
            (void)neg;
            const int pref = link_ref[static_cast<std::size_t>(r)];
            if (pref >= 0 && pref != j && ok(arc(r, pref)) && ok(arc(e, pref))) return {e, pref};
            for (int k = 0; k < static_cast<int>(g.sats.size()); ++k)
                if (k != j && ok(arc(r, k)) && ok(arc(e, k))) return {e, k};
        }
        return {-1, -1};
    }
};

/// Per band: the forest's tree arcs, then (Full) the band-only arcs that join two fragments.
void completeTrees(ShortBasisBuild& w, bool full_coverage) {
    for (std::size_t b = 0; b < w.n_bands; ++b) {
        Dsu fragments;
        for (std::size_t a = 0; a < w.g.rows.size(); ++a) {
            if (!w.f.is_tree_arc[a] || !w.present(static_cast<int>(a), b)) continue;
            w.tree[b][a] = 1;
            dsuUnite(fragments, stationVertex(w.g, w.g.row_station[a]), satVertex(w.g, w.g.row_sat[a]));
        }
        if (full_coverage) {
            for (std::size_t a = 0; a < w.g.rows.size(); ++a) {
                if (w.tree[b][a] || !w.present(static_cast<int>(a), b) || w.shared(static_cast<int>(a))) continue;
                const int u = stationVertex(w.g, w.g.row_station[a]), v = satVertex(w.g, w.g.row_sat[a]);
                if (dsuFind(fragments, u) == dsuFind(fragments, v)) continue;
                dsuUnite(fragments, u, v);
                w.tree[b][a] = 1;   // promoted: completes the band's forest
            }
        }
        w.index.push_back(indexTrees(w.g, w.tree[b]));
    }
}

/// Per band: E - V + C over the arcs present in that band (a dense union-find over the graph's
/// vertices: each arc that joins two sets removes one component).
void latticeDimensions(ShortBasisBuild& w) {
    const std::size_t nv = w.g.stations.size() + w.g.sats.size();
    w.out.lattice_dim.assign(w.n_bands, 0);
    std::vector<int>  parent(nv);
    std::vector<char> seen(nv);
    const auto find = [&](int v) {
        while (parent[static_cast<std::size_t>(v)] != v) {
            parent[static_cast<std::size_t>(v)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(v)])];
            v = parent[static_cast<std::size_t>(v)];
        }
        return v;
    };
    for (std::size_t b = 0; b < w.n_bands; ++b) {
        std::iota(parent.begin(), parent.end(), 0);
        std::fill(seen.begin(), seen.end(), 0);
        int n_arcs = 0, n_vertices = 0, n_components = 0;
        for (std::size_t a = 0; a < w.g.rows.size(); ++a) {
            if (!w.present(static_cast<int>(a), b)) continue;
            ++n_arcs;
            const int u = stationVertex(w.g, w.g.row_station[a]), v = satVertex(w.g, w.g.row_sat[a]);
            for (int x : {u, v})
                if (!seen[static_cast<std::size_t>(x)]) { seen[static_cast<std::size_t>(x)] = 1; ++n_vertices; ++n_components; }
            const int ru = find(u), rv = find(v);
            if (ru != rv) { parent[static_cast<std::size_t>(ru)] = rv; --n_components; }
        }
        w.out.lattice_dim[b] = n_arcs - n_vertices + n_components;
    }
}

/// Shared chords, one copy per band: partner DDs, then (Full) borrowed DDs or tree paths.
void closeSharedChords(ShortBasisBuild& w, bool full_coverage) {
    std::vector<std::size_t> all_bands(w.n_bands);
    for (std::size_t b = 0; b < w.n_bands; ++b) all_bands[b] = b;
    const auto ok = [&](int a) { return w.availableShared(a); };
    for (int r : w.order) {
        const int p = w.link_parent[static_cast<std::size_t>(r)], k = w.link_ref[static_cast<std::size_t>(r)];
        if (p >= 0) {
            for (int j = 0; j < static_cast<int>(w.g.sats.size()); ++j) {
                const int a_rj = w.arc(r, j), a_pj = w.arc(p, j);
                if (j == k || !w.isChordShared(a_rj) || !w.shared(a_pj)) continue;
                // ParentOnly is ARL tree_basis verbatim; Full also checks that every other term
                // is a tree arc or an earlier pivot (it always is for a baseline-rule forest).
                if (full_coverage && !(ok(a_pj) && ok(w.arc(p, k)) && ok(w.arc(r, k)))) continue;
                w.emitDd(r, p, j, k, CycleKind::PartnerDd, /*band_only=*/false, all_bands);
            }
        }
        if (!full_coverage) continue;
        for (int j = 0; j < static_cast<int>(w.g.sats.size()); ++j) {
            const int a_rj = w.arc(r, j);
            if (!w.isChordShared(a_rj)) continue;
            const auto [e, kk] = w.borrowed(r, j, w.shared_bits, ok, -1);
            if (e >= 0) {
                w.emitDd(r, e, j, kk, CycleKind::BorrowedDd, /*band_only=*/false, all_bands);
                continue;
            }
            for (std::size_t b = 0; b < w.n_bands; ++b)
                if (std::optional<Cycle> x = treePathCycle(w.g, w.index[b], a_rj, static_cast<int>(b)))
                    w.emit(a_rj, b, /*band_only=*/false, CycleKind::Fundamental, -1, -1, std::move(*x));
        }
    }
}

/// Band-only chords, in their own band after every shared chord: the link parent's DD, a borrowed
/// DD, else the band's tree path.
void closeBandOnlyChords(ShortBasisBuild& w) {
    for (std::size_t b = 0; b < w.n_bands; ++b) {
        const auto ok = [&](int a) { return w.available(a, b); };
        for (int r : w.order) {
            const int p = w.link_parent[static_cast<std::size_t>(r)], k = w.link_ref[static_cast<std::size_t>(r)];
            for (int j = 0; j < static_cast<int>(w.g.sats.size()); ++j) {
                const int a_rj = w.arc(r, j);
                if (!w.present(a_rj, b) || w.shared(a_rj) || w.tree[b][static_cast<std::size_t>(a_rj)]
                    || w.closed[b][static_cast<std::size_t>(a_rj)])
                    continue;
                if (p >= 0 && j != k && ok(w.arc(p, j)) && ok(w.arc(p, k)) && ok(w.arc(r, k))) {
                    w.emitDd(r, p, j, k, CycleKind::PartnerDd, /*band_only=*/true, {b});
                    continue;
                }
                const auto [e, kk] = w.borrowed(r, j, w.band_bits[b], ok, static_cast<int>(b));
                if (e >= 0) {
                    w.emitDd(r, e, j, kk, CycleKind::BorrowedDd, /*band_only=*/true, {b});
                    continue;
                }
                if (std::optional<Cycle> x = treePathCycle(w.g, w.index[b], a_rj, static_cast<int>(b)))
                    w.emit(a_rj, b, /*band_only=*/true, CycleKind::Fundamental, -1, -1, std::move(*x));
            }
        }
    }
}

/// The sticky basis's borrowed closures in this graph's slots (a closure whose station or
/// satellite holds no slot here is dropped).
void readStickyBorrowed(ShortBasisBuild& w, const CycleBasis& sticky) {
    std::map<StationId, int> station_slot;
    std::map<SatId, int>     sat_slot;
    for (std::size_t s = 0; s < w.g.stations.size(); ++s) station_slot.emplace(w.g.stations[s], static_cast<int>(s));
    for (std::size_t k = 0; k < w.g.sats.size(); ++k) sat_slot.emplace(w.g.sats[k], static_cast<int>(k));
    const auto slotOf = [](const auto& m, const auto& key) {
        const auto it = m.find(key);
        return it == m.end() ? -1 : it->second;
    };
    for (const BasisCycle& bc : sticky.cycles) {
        if (bc.kind != CycleKind::BorrowedDd) continue;
        const int r = slotOf(station_slot, bc.station), e = slotOf(station_slot, bc.partner);
        const int j = slotOf(sat_slot, bc.sat), k = slotOf(sat_slot, bc.ref_sat);
        if (r < 0 || e < 0 || j < 0 || k < 0) continue;
        w.prev_borrowed[{bc.band_only ? bc.band : -1, r, j}] = {e, k};
    }
}

}  // namespace

CycleBasis shortCycleBasis(const ArcGraph& g, const SpanningForest& f, BasisCoverage coverage,
                           const CycleBasis* sticky) {
    CycleBasis out;
    ShortBasisBuild w(g, f, out);
    if (sticky) readStickyBorrowed(w, *sticky);
    const bool full_coverage = (coverage == BasisCoverage::Full);
    completeTrees(w, full_coverage);
    latticeDimensions(w);
    closeSharedChords(w, full_coverage);
    if (full_coverage) closeBandOnlyChords(w);
    return out;
}

// ============================== 5. integer maps ==============================

namespace {

/// The pivots of a basis: per cycle its pivot arc, and the reverse map.
struct PivotIndex {
    std::vector<ArcId>   arc_of;  ///< Cycle -> pivot arc (its (station, satellite) term).
    std::map<ArcId, int> of;      ///< Pivot arc -> cycle.
};

/// The pivot arc of a basis cycle: its (station, satellite) term.
ArcId pivotOf(const BasisCycle& bc) {
    for (const CycleTerm& t : bc.cycle.terms)
        if (t.arc.station == bc.station && t.arc.sat == bc.sat) return t.arc;
    return ArcId{};
}

PivotIndex pivotIndexOf(const CycleBasis& b) {
    PivotIndex P;
    P.arc_of.reserve(b.cycles.size());
    for (std::size_t i = 0; i < b.cycles.size(); ++i) {
        const ArcId pivot = pivotOf(b.cycles[i]);
        P.arc_of.push_back(pivot);
        P.of.emplace(pivot, static_cast<int>(i));
    }
    return P;
}

/// True when two sorted cycles hold the same terms.
bool sameTerms(const Cycle& x, const Cycle& y) {
    if (x.terms.size() != y.terms.size()) return false;
    for (std::size_t i = 0; i < x.terms.size(); ++i)
        if (x.terms[i].arc != y.terms[i].arc || x.terms[i].coeff != y.terms[i].coeff) return false;
    return true;
}

/// Back-substitution: the highest pending pivot fixes its coefficient, which is subtracted.
std::optional<Coordinates> coordinatesIn(const CycleBasis& b, const PivotIndex& P, const Cycle& x) {
    std::map<ArcId, long long> residual;
    for (const CycleTerm& t : x.terms) residual[t.arc] += t.coeff;
    std::set<int> pending, done;
    for (const auto& [arc, v] : residual) {
        if (v == 0) continue;
        const auto it = P.of.find(arc);
        if (it != P.of.end()) pending.insert(it->second);
    }
    Coordinates out;
    while (!pending.empty()) {
        const int i = *pending.rbegin();
        pending.erase(i);
        if (!done.insert(i).second) return std::nullopt;   // not unit-triangular
        const long long c = residual[P.arc_of[static_cast<std::size_t>(i)]];
        if (c == 0) continue;
        out.emplace_back(i, c);
        for (const CycleTerm& t : b.cycles[static_cast<std::size_t>(i)].cycle.terms) {
            const long long v = (residual[t.arc] -= c * t.coeff);
            if (v == 0) continue;
            const auto it = P.of.find(t.arc);
            if (it != P.of.end()) pending.insert(it->second);
        }
    }
    for (const auto& [arc, v] : residual) if (v != 0) return std::nullopt;   // outside the span
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::optional<Coordinates> cycleCoordinates(const CycleBasis& b, const Cycle& x) {
    return coordinatesIn(b, pivotIndexOf(b), x);
}

std::vector<std::optional<Coordinates>> basisChangeMap(const CycleBasis& old_basis, const CycleBasis& new_basis,
                                                       const std::set<ArcId>& invalidated) {
    const PivotIndex P = pivotIndexOf(old_basis);
    std::vector<std::optional<Coordinates>> W;
    W.reserve(new_basis.cycles.size());
    for (const BasisCycle& bc : new_basis.cycles) {
        bool fresh = false;
        for (const CycleTerm& t : bc.cycle.terms)
            if (invalidated.count(t.arc)) { fresh = true; break; }
        if (fresh) { W.push_back(std::nullopt); continue; }
        // The cycle a sticky basis keeps: its pivot owns an old cycle with the same terms, whose
        // back-substitution is that one cycle at 1 (the old cycle holds no later pivot).
        const auto same = P.of.find(pivotOf(bc));
        if (same != P.of.end() && sameTerms(old_basis.cycles[static_cast<std::size_t>(same->second)].cycle, bc.cycle)) {
            W.push_back(Coordinates{{same->second, 1}});
            continue;
        }
        W.push_back(coordinatesIn(old_basis, P, bc.cycle));
    }
    return W;
}

Cycle combineCycles(const CycleBasis& b, const Coordinates& coeffs) {
    std::map<ArcId, long long> acc;
    for (const auto& [i, c] : coeffs)
        for (const CycleTerm& t : b.cycles[static_cast<std::size_t>(i)].cycle.terms) acc[t.arc] += c * t.coeff;
    return cycleOf(acc);
}

// ============================== 6. the lattice of fixed cycles ==============================

namespace {

/// One entry epoch per row (a lattice filled before card FCF, or by hand, has none: 0).
void alignEntered(CycleLattice& L) {
    if (L.entered_ep.size() != L.rows.size()) L.entered_ep.resize(L.rows.size(), 0);
}

/// A cycle reduced by the pivots: each pivot it holds is removed by its own row (rows hold no
/// other row's pivot, so the order is free).
struct LatticeReduction {
    Cycle            residual;      ///< What no pivot removes (empty: the cycle is in the span).
    long long        implied{0};    ///< The integer the removed part carries.
    std::vector<int> support;       ///< The rows used, in the cycle's arc order.
};

LatticeReduction latticeReduce(const CycleLattice& L, const Cycle& c) {
    std::map<ArcId, long long> held;   // the offered cycle, terms merged
    for (const CycleTerm& t : c.terms) held[t.arc] += t.coeff;
    std::map<ArcId, long long> acc = held;
    LatticeReduction red;
    for (const auto& [arc, coef] : held) {
        if (coef == 0) continue;
        const auto pr = L.pivot_row.find(arc);
        if (pr == L.pivot_row.end()) continue;
        const Cycle& row = L.rows[static_cast<std::size_t>(pr->second)];
        for (const CycleTerm& u : row.terms) acc[u.arc] -= coef * u.coeff;
        red.implied += coef * L.values[static_cast<std::size_t>(pr->second)];
        red.support.push_back(pr->second);
    }
    red.residual = cycleOf(acc);
    return red;
}

/// Drop rows (ascending indices) and re-index the pivots.
void dropRows(CycleLattice& L, const std::vector<int>& drop) {
    alignEntered(L);
    std::vector<int> new_index(L.rows.size(), -1);
    std::vector<Cycle> rows;
    std::vector<long long> values, entered;
    for (std::size_t k = 0; k < L.rows.size(); ++k) {
        if (std::binary_search(drop.begin(), drop.end(), static_cast<int>(k))) continue;
        new_index[k] = static_cast<int>(rows.size());
        rows.push_back(std::move(L.rows[k]));
        values.push_back(L.values[k]);
        entered.push_back(L.entered_ep[k]);
    }
    std::map<ArcId, int> pivots;
    for (const auto& [arc, k] : L.pivot_row)
        if (new_index[static_cast<std::size_t>(k)] >= 0) pivots.emplace(arc, new_index[static_cast<std::size_t>(k)]);
    L.rows       = std::move(rows);
    L.values     = std::move(values);
    L.pivot_row  = std::move(pivots);
    L.entered_ep = std::move(entered);
}

}  // namespace

LatticeVerdict latticeOffer(CycleLattice& L, const Cycle& c, long long value, long long epoch) {
    alignEntered(L);
    LatticeReduction red = latticeReduce(L, c);
    Cycle& residual = red.residual;
    const long long implied = red.implied;
    if (residual.terms.empty()) return implied == value ? LatticeVerdict::Confirmed : LatticeVerdict::Contradicted;
    long long rv = value - implied;
    const auto unit = std::find_if(residual.terms.begin(), residual.terms.end(),
                                   [](const CycleTerm& t) { return t.coeff == 1 || t.coeff == -1; });
    if (unit == residual.terms.end()) return LatticeVerdict::NotPrimitive;
    const ArcId pivot = unit->arc;
    if (unit->coeff == -1) {
        for (CycleTerm& t : residual.terms) t.coeff = -t.coeff;
        rv = -rv;
    }
    for (std::size_t k = 0; k < L.rows.size(); ++k) {   // keep the echelon form reduced
        const long long d = coeffOf(L.rows[k], pivot);
        if (d == 0) continue;
        L.rows[k] = addScaled(L.rows[k], -d, residual);
        L.values[k] -= d * rv;
        L.entered_ep[k] = std::max(L.entered_ep[k], epoch);
    }
    L.rows.push_back(std::move(residual));
    L.values.push_back(rv);
    L.entered_ep.push_back(epoch);
    L.pivot_row[pivot] = static_cast<int>(L.rows.size()) - 1;
    return LatticeVerdict::Added;
}

std::optional<long long> latticeImplied(const CycleLattice& L, const Cycle& c, long long* newest) {
    const LatticeReduction red = latticeReduce(L, c);
    if (!red.residual.terms.empty()) return std::nullopt;
    if (newest) {
        *newest = -1;
        for (int k : red.support) {
            const std::size_t i = static_cast<std::size_t>(k);
            *newest = std::max(*newest, i < L.entered_ep.size() ? L.entered_ep[i] : 0LL);
        }
    }
    return red.implied;
}

int latticeEliminateArc(CycleLattice& L, const ArcId& arc) {
    alignEntered(L);
    std::vector<int> touching;
    for (std::size_t k = 0; k < L.rows.size(); ++k)
        if (coeffOf(L.rows[k], arc) != 0) touching.push_back(static_cast<int>(k));
    if (touching.empty()) return 0;
    // The eliminating row: the pivot row, else the shortest row holding the arc at +-1.
    int k0 = -1;
    const auto pv = L.pivot_row.find(arc);
    if (pv != L.pivot_row.end()) {
        k0 = pv->second;
    } else {
        for (int k : touching) {
            const long long s = coeffOf(L.rows[static_cast<std::size_t>(k)], arc);
            if (s != 1 && s != -1) continue;
            if (k0 < 0 || L.rows[static_cast<std::size_t>(k)].terms.size() < L.rows[static_cast<std::size_t>(k0)].terms.size())
                k0 = k;
        }
    }
    if (k0 < 0) {   // no unit row: drop every row that touches the arc
        dropRows(L, touching);
        return static_cast<int>(touching.size());
    }
    const long long s0 = coeffOf(L.rows[static_cast<std::size_t>(k0)], arc);   // +-1
    for (int k : touching) {
        if (k == k0) continue;
        const long long d = coeffOf(L.rows[static_cast<std::size_t>(k)], arc) * s0;   // d * s0 = d / s0
        L.rows[static_cast<std::size_t>(k)] = addScaled(L.rows[static_cast<std::size_t>(k)], -d, L.rows[static_cast<std::size_t>(k0)]);
        L.values[static_cast<std::size_t>(k)] -= d * L.values[static_cast<std::size_t>(k0)];
        L.entered_ep[static_cast<std::size_t>(k)] =
            std::max(L.entered_ep[static_cast<std::size_t>(k)], L.entered_ep[static_cast<std::size_t>(k0)]);
    }
    dropRows(L, {k0});
    return 1;
}

int latticeShiftArc(CycleLattice& L, const ArcId& arc, long long delta) {
    int touched = 0;
    for (std::size_t k = 0; k < L.rows.size(); ++k) {
        const long long c = coeffOf(L.rows[k], arc);
        if (c == 0) continue;
        L.values[k] += c * delta;
        ++touched;
    }
    return touched;
}

}  // namespace arcgraph
}  // namespace GnssProcessor
