// Unit gates for filter/NetworkArcGraph: suite NetworkArcGraph.
//
// The module's own properties, on random and hand-built graphs; its identity with the legacy
// code it replaces is the separate suite NetworkArcGraphOracle (test_network_arc_graph_oracles.cpp).
//
//   - integer-id layer: ArcKey, the Dsu link rule, the self-loop refusal of singleTreeOf;
//   - family graph: row merging and refusal, component partitions invariant under arc shuffles,
//     roots on satellites in rank order, the legacy chain vertex order;
//   - forests: the chain rule's incumbent drain and second pass; the baseline rule's incumbent
//     links, re-seats by cause, most durable reference, and the stop at the gap;
//   - bases: shortCycleBasis(Full) holds E - V + C rows per band, every row a cycle whose pivot
//     no earlier row of its band holds, every ParentOnly row, identical L1/L2 shapes, band-only
//     completion on partial-L2 graphs, and a Z-basis (every fundamental cycle of another forest
//     has integer coordinates);
//   - maps: exact coordinates, none outside the span, W the identity on an unchanged graph,
//     unimodular across a forced tree swap, re-referencing across a slipped link reference;
//   - lattice: planted integer truth through 10 000 random offers and eliminations, one row per
//     elimination when a unit row holds the arc, ten held DDs on a slipped reference become nine,
//     a planted wrong integer is Contradicted, a non-primitive residual is refused.

#include "network/ar/ArcGraph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using namespace arcgraph;

// ============================== helpers ==============================

SignalId sigOf(const char* code) { return *SignalId::fromObsCode(System::GPS, code); }
ArcFamily pairFamily() { return ArcFamily{System::GPS, {sigOf("L1C"), sigOf("L2W")}}; }
ArcFamily oneBandFamily() { return ArcFamily{System::GPS, {sigOf("L1C")}}; }
SatId gps(int prn) { return SatId{System::GPS, prn}; }
StationId sta(int index) { return StationId{index}; }

int uniform(std::mt19937& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
bool chance(std::mt19937& rng, double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; }

/// One arc row.
ArcRow row(int station, int prn, std::uint8_t present, std::uint8_t slipped = 0, double durability = 0.0) {
    ArcRow r;
    r.station    = sta(station);
    r.sat        = gps(prn);
    r.present    = present;
    r.slipped    = slipped;
    r.durability = durability;
    return r;
}

/// A graph with stations 0..n_sta-1 and satellites G01..G<n_sat> declared in order.
ArcGraph graphOf(const ArcFamily& fam, int n_sta, int n_sat, const std::vector<ArcRow>& rows) {
    std::vector<StationId> so;
    for (int s = 0; s < n_sta; ++s) so.push_back(sta(s));
    std::vector<SatId> ko;
    for (int k = 1; k <= n_sat; ++k) ko.push_back(gps(k));
    return buildArcGraph(fam, so, ko, rows);
}

/// The integer value of a combination of band-tagged arcs under a truth.
long long valueOf(const Cycle& x, const std::map<ArcId, long long>& N) {
    long long v = 0;
    for (const CycleTerm& t : x.terms) v += t.coeff * N.at(t.arc);
    return v;
}

/// A random truth integer for every arc in every band it is present in.
std::map<ArcId, long long> randomTruth(std::mt19937& rng, const ArcGraph& g) {
    std::map<ArcId, long long> N;
    for (std::size_t a = 0; a < g.rows.size(); ++a)
        for (std::size_t b = 0; b < g.family.bands.size(); ++b)
            if (g.rows[a].present & (1u << b))
                N[ArcId{g.stations[static_cast<std::size_t>(g.row_station[a])], g.sats[static_cast<std::size_t>(g.row_sat[a])],
                        g.family.bands[b]}] = uniform(rng, -50, 50);
    return N;
}

/// A random pair graph: stations and satellites in blocks (several components), some arcs on one
/// band only (partial L1/L2 tracking), some stations and satellites on L1 only.
struct GraphSpec {
    int    n_sta{8};
    int    n_sat{8};
    int    n_blocks{1};
    double p_arc{0.7};
    double p_partial{0.1};
    int    n_l1_sta{0};
    int    n_l1_sat{0};
    double p_slip{0.0};
};
ArcGraph randomPairGraph(std::mt19937& rng, const GraphSpec& s) {
    std::vector<ArcRow> rows;
    std::vector<int> sta_block(static_cast<std::size_t>(s.n_sta)), sat_block(static_cast<std::size_t>(s.n_sat));
    for (int& b : sta_block) b = uniform(rng, 0, s.n_blocks - 1);
    for (int& b : sat_block) b = uniform(rng, 0, s.n_blocks - 1);
    const auto slip = [&]() { return static_cast<std::uint8_t>((chance(rng, s.p_slip) ? 1 : 0) | (chance(rng, s.p_slip) ? 2 : 0)); };
    for (int r = 0; r < s.n_sta; ++r)
        for (int k = 0; k < s.n_sat; ++k) {
            if (sta_block[static_cast<std::size_t>(r)] != sat_block[static_cast<std::size_t>(k)] || !chance(rng, s.p_arc)) continue;
            std::uint8_t present = 3;
            if (chance(rng, s.p_partial)) present = chance(rng, 0.7) ? 1 : 2;   // one band only
            rows.push_back(row(r, k + 1, present, slip(), uniform(rng, 0, 5)));
        }
    const int n_sta_all = s.n_sta + s.n_l1_sta, n_sat_all = s.n_sat + s.n_l1_sat;
    for (int r = s.n_sta; r < n_sta_all; ++r)   // L1-only stations
        for (int k = 0; k < n_sat_all; ++k)
            if (chance(rng, 0.4)) rows.push_back(row(r, k + 1, 1, slip(), uniform(rng, 0, 5)));
    for (int k = s.n_sat; k < n_sat_all; ++k)   // L1-only satellites
        for (int r = 0; r < s.n_sta; ++r)
            if (chance(rng, 0.4)) rows.push_back(row(r, k + 1, 1, slip(), uniform(rng, 0, 5)));
    std::shuffle(rows.begin(), rows.end(), rng);
    return graphOf(pairFamily(), n_sta_all, n_sat_all, rows);
}

/// One baseline forest per component of the pair graph, joined.
SpanningForest pairForest(const ArcGraph& g, const ForestRules& rules) {
    std::vector<SpanningForest> parts;
    for (const Component& c : arcComponents(g, rules.band_mask, {}))
        parts.push_back(growBaselineForest(g, c, {}, rules));
    return joinForests(g, parts);
}

/// E - V + C of one band, by an independent union-find.
int bruteLatticeDim(const ArcGraph& g, int band) {
    const int nv = static_cast<int>(g.stations.size() + g.sats.size());
    std::vector<int> parent(static_cast<std::size_t>(nv));
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](int x) { while (parent[static_cast<std::size_t>(x)] != x) x = parent[static_cast<std::size_t>(x)]; return x; };
    std::set<int> vertices;
    int e = 0;
    for (std::size_t a = 0; a < g.rows.size(); ++a) {
        if (!(g.rows[a].present & (1u << band))) continue;
        ++e;
        const int u = g.row_station[a], v = static_cast<int>(g.stations.size()) + g.row_sat[a];
        vertices.insert(u);
        vertices.insert(v);
        parent[static_cast<std::size_t>(find(u))] = find(v);
    }
    std::set<int> roots;
    for (int v : vertices) roots.insert(find(v));
    return e - static_cast<int>(vertices.size()) + static_cast<int>(roots.size());
}

/// The band index of a cycle's signal in a family.
int bandOfSignal(const ArcGraph& g, const SignalId& s) {
    for (std::size_t b = 0; b < g.family.bands.size(); ++b) if (g.family.bands[b] == s) return static_cast<int>(b);
    return -1;
}

/// The coefficient of one arc in a cycle.
long long coeffIn(const Cycle& x, const ArcId& a) {
    for (const CycleTerm& t : x.terms) if (t.arc == a) return t.coeff;
    return 0;
}

/// The pivot arc of a basis cycle.
ArcId pivotOf(const ArcGraph& g, const BasisCycle& bc) {
    return ArcId{g.stations[static_cast<std::size_t>(g.row_station[static_cast<std::size_t>(bc.pivot_arc)])],
                 g.sats[static_cast<std::size_t>(g.row_sat[static_cast<std::size_t>(bc.pivot_arc)])],
                 g.family.bands[static_cast<std::size_t>(bc.band)]};
}

/// The double difference (r, p; j, k) in one signal.
Cycle dd(int r, int p, int j, int k, const SignalId& s) {
    std::map<ArcId, long long> acc;
    acc[ArcId{sta(r), gps(j), s}] += 1;
    acc[ArcId{sta(r), gps(k), s}] -= 1;
    acc[ArcId{sta(p), gps(j), s}] -= 1;
    acc[ArcId{sta(p), gps(k), s}] += 1;
    Cycle x;
    for (const auto& [a, v] : acc) if (v) x.terms.push_back(CycleTerm{a, static_cast<int>(v)});
    return x;
}

// ============================== 1. integer ids ==============================

TEST(NetworkArcGraph, ArcKeyIsCanonical) {
    const ArcKey a(7, 3);
    EXPECT_EQ(a.u, 3);
    EXPECT_EQ(a.v, 7);
    EXPECT_EQ(a, ArcKey::make(3, 7));
    EXPECT_EQ(a.other(3), 7);
    EXPECT_EQ(a.other(7), 3);
    EXPECT_TRUE(ArcKey(1, 9) < ArcKey(2, 3));
}

TEST(NetworkArcGraph, DsuLinksTheFirstRootUnderTheSecond) {
    Dsu d;
    dsuUnite(d, 1, 100);   // station 1 under satellite 100
    EXPECT_EQ(dsuFind(d, 1), 100);
    dsuUnite(d, 2, 101);
    dsuUnite(d, 1, 101);   // root(1) = 100 links under root(101) = 101
    EXPECT_EQ(dsuFind(d, 100), 101);
    EXPECT_EQ(dsuFind(d, 2), 101);
    EXPECT_EQ(dsuFind(d, 55), 55);   // a new id is a singleton
}

TEST(NetworkArcGraph, SingleTreeRefusesASelfLoopAndCycles) {
    EXPECT_FALSE(singleTreeOf({ArcKey(3, 3)}).has_value());   // declared difference from ZComponent
    EXPECT_FALSE(singleTreeOf({ArcKey(1, 2), ArcKey(2, 3), ArcKey(1, 3)}).has_value());
    EXPECT_FALSE(singleTreeOf({ArcKey(1, 2), ArcKey(3, 4)}).has_value());
    const auto t = singleTreeOf({ArcKey(1, 2), ArcKey(2, 3)});
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->vertices, (std::set<int>{1, 2, 3}));
    EXPECT_TRUE(singleTreeOf({}).has_value());
}

TEST(NetworkArcGraph, ChordRestrictionReadsTheOldChords) {
    const std::map<ArcKey, int> cyc{{ArcKey(1, 10), 1}, {ArcKey(2, 10), -1}, {ArcKey(2, 11), 1}, {ArcKey(1, 11), -1}};
    EXPECT_EQ(chordRestriction(cyc, {ArcKey(2, 11), ArcKey(3, 12), ArcKey(2, 10)}), (std::vector<int>{1, 0, -1}));
}

// ============================== 2. family graph ==============================

TEST(NetworkArcGraph, BuildArcGraphMergesRowsAndRefusesUndeclared) {
    std::vector<ArcRow> rows{row(0, 1, 1, 0, 5.0), row(0, 2, 3), row(0, 1, 2, 2, 3.0), row(9, 1, 1), row(1, 7, 1)};
    rows[0].eligible_host = true;
    rows[2].eligible_host = false;
    int refused = -1;
    const ArcGraph g = buildArcGraph(pairFamily(), {sta(0), sta(1), sta(0)}, {gps(2), gps(1)}, rows, &refused);
    EXPECT_EQ(refused, 2);                 // station 9 and satellite G07 are undeclared
    ASSERT_EQ(g.stations.size(), 2u);      // the repeated declaration keeps its first slot
    ASSERT_EQ(g.rows.size(), 2u);
    EXPECT_EQ(g.row_sat[0], 1);            // G01 is satellite slot 1 (declared second)
    EXPECT_EQ(g.rows[0].present, 3);       // merged in the first row's position
    EXPECT_EQ(g.rows[0].slipped, 2);
    EXPECT_EQ(g.rows[0].durability, 3.0);  // the weaker band
    EXPECT_FALSE(g.rows[0].eligible_host);
    EXPECT_EQ(arcIndex(g, 0, 1), 0);
    EXPECT_EQ(arcIndex(g, 0, 0), 1);
    EXPECT_EQ(arcIndex(g, 1, 0), -1);
    EXPECT_TRUE(usable(g, 0, 3));
    EXPECT_TRUE(slippedIn(g, 0, 2));
    EXPECT_FALSE(slippedIn(g, 0, 1));
    EXPECT_EQ(satVertex(g, 1), 3);
    EXPECT_TRUE(isSatVertex(g, 2));
    EXPECT_FALSE(isSatVertex(g, 1));
    EXPECT_EQ(g.incident[static_cast<std::size_t>(stationVertex(g, 0))], (std::vector<int>{0, 1}));
}

TEST(NetworkArcGraph, ArcComponentsPartitionIsInvariantUnderArcShuffles) {
    std::mt19937 rng(101u);
    for (int it = 0; it < 300; ++it) {
        GraphSpec s;
        s.n_sta = uniform(rng, 2, 14);
        s.n_sat = uniform(rng, 2, 14);
        s.n_blocks = uniform(rng, 1, 4);
        s.p_arc = 0.5;
        ArcGraph g = randomPairGraph(rng, s);
        const auto partition = [](const ArcGraph& gg) {
            std::set<std::pair<std::vector<int>, std::vector<int>>> p;
            for (const Component& c : arcComponents(gg, 1, {})) {
                std::vector<int> st, sa;
                for (int x : c.stations) st.push_back(gg.stations[static_cast<std::size_t>(x)].index);
                for (int k : c.sats) sa.push_back(gg.sats[static_cast<std::size_t>(k)].prn);
                std::sort(st.begin(), st.end());
                std::sort(sa.begin(), sa.end());
                p.insert({st, sa});
            }
            return p;
        };
        const auto p0 = partition(g);
        int dims = 0, n_comps = 0;
        for (const Component& c : arcComponents(g, 1, {})) { dims += cycleDimension(c); ++n_comps; }
        EXPECT_EQ(dims, bruteLatticeDim(g, 0));
        std::vector<ArcRow> rows = g.rows;
        std::shuffle(rows.begin(), rows.end(), rng);
        const ArcGraph h = buildArcGraph(g.family, g.stations, g.sats, rows);
        ASSERT_EQ(partition(h), p0);
        const Component w = wholeGraph(g, 1);
        std::size_t arcs = 0;
        for (const Component& c : arcComponents(g, 1, {})) arcs += c.arcs.size();
        EXPECT_EQ(w.arcs.size(), arcs);
        (void)n_comps;
    }
}

TEST(NetworkArcGraph, ArcComponentsRootIsASatelliteAndFollowsTheRank) {
    // Two components: {0, 1} x {G01, G02} and {2} x {G03}.
    const ArcGraph g = graphOf(oneBandFamily(), 3, 3, {row(0, 1, 1), row(1, 1, 1), row(1, 2, 1), row(2, 3, 1)});
    const std::vector<Component> natural = arcComponents(g, 1, {});
    ASSERT_EQ(natural.size(), 2u);
    for (const Component& c : natural) EXPECT_TRUE(isSatVertex(g, c.root));
    EXPECT_LT(natural[0].root, natural[1].root);
    std::vector<int> reversed(6);
    for (int v = 0; v < 6; ++v) reversed[static_cast<std::size_t>(v)] = 6 - v;
    const std::vector<Component> ranked = arcComponents(g, 1, reversed);
    EXPECT_EQ(ranked[0].root, natural[1].root);
    EXPECT_EQ(natural[0].stations, (std::vector<int>{0, 1}));
    EXPECT_EQ(natural[0].sats, (std::vector<int>{0, 1}));
    EXPECT_EQ(cycleDimension(natural[0]), 0);
}

TEST(NetworkArcGraph, LegacyChainVertexRankOrdersRosterIndicesAsStrings) {
    const ArcGraph g = buildArcGraph(oneBandFamily(), {sta(2), sta(10), sta(1)}, {gps(5), gps(3)}, {});
    const std::vector<int> rank = legacyChainVertexRank(g);
    // "R1" < "R10" < "R2", then the satellites by slot.
    EXPECT_EQ(rank, (std::vector<int>{2, 1, 0, 3, 4}));
}

// ============================== 3. forests ==============================

TEST(NetworkArcGraph, ChainForestDrainsTheIncumbentBeforeAnyNewEdge) {
    // A and B both track x and y.  Without an incumbent, B joins through (B, x), the first new
    // edge from x; with the incumbent arc (B, y) it joins through that arc.
    const ArcGraph g = graphOf(oneBandFamily(), 2, 2, {row(0, 1, 1), row(0, 2, 1), row(1, 1, 1), row(1, 2, 1)});
    const Component c = arcComponents(g, 1, {}).at(0);
    ForestRules rules;
    const SpanningForest plain = growChainForest(g, c, {stationVertex(g, 0)}, {}, rules, {}, {});
    ASSERT_EQ(plain.edges.size(), 3u);
    EXPECT_EQ(plain.edges[2].arc, arcIndex(g, 1, 0));
    EXPECT_FALSE(plain.edges[2].from_incumbent);
    const SpanningForest sticky = growChainForest(g, c, {stationVertex(g, 0)}, {arcIndex(g, 1, 1)}, rules, {}, {});
    ASSERT_EQ(sticky.edges.size(), 3u);
    EXPECT_EQ(sticky.edges[2].arc, arcIndex(g, 1, 1));
    EXPECT_TRUE(sticky.edges[2].from_incumbent);
    EXPECT_EQ(sticky.station_order, (std::vector<int>{0, 1}));
}

TEST(NetworkArcGraph, ChainForestSecondPassAdmitsWhatTheFirstRefused) {
    // B reaches the tree only through its slipped arc (B, x).
    const ArcGraph g = graphOf(oneBandFamily(), 2, 1, {row(0, 1, 1), row(1, 1, 1, /*slipped=*/1)});
    const Component c = arcComponents(g, 1, {}).at(0);
    ForestRules rules;
    rules.refuse_slipped_first = true;
    const SpanningForest f = growChainForest(g, c, {stationVertex(g, 0)}, {}, rules, {}, {});
    ASSERT_EQ(f.edges.size(), 2u);
    EXPECT_EQ(f.edges[0].pass, 1);
    EXPECT_EQ(f.edges[1].pass, 2);
    EXPECT_TRUE(f.edges[1].slipped);
    EXPECT_FALSE(f.edges[1].attaches_sat);
}

/// A connected pair graph with every station tracking satellites 1..n_sat (both bands) unless
/// a cell is removed.
ArcGraph denseGraph(int n_sta, int n_sat, const std::set<std::pair<int, int>>& removed = {},
                    const std::map<std::pair<int, int>, std::uint8_t>& slipped = {},
                    const std::map<std::pair<int, int>, double>& durability = {}) {
    std::vector<ArcRow> rows;
    for (int r = 0; r < n_sta; ++r)
        for (int k = 1; k <= n_sat; ++k) {
            if (removed.count({r, k})) continue;
            const auto si = slipped.find({r, k});
            const auto di = durability.find({r, k});
            rows.push_back(row(r, k, 3, si == slipped.end() ? 0 : si->second, di == durability.end() ? 1.0 : di->second));
        }
    return graphOf(pairFamily(), n_sta, n_sat, rows);
}

TEST(NetworkArcGraph, BaselineForestKeepsValidIncumbentLinks) {
    std::mt19937 rng(202u);
    for (int it = 0; it < 100; ++it) {
        GraphSpec s;
        s.n_sta = uniform(rng, 2, 20);
        s.n_sat = uniform(rng, 3, 16);
        s.p_arc = 0.7;
        const ArcGraph g = randomPairGraph(rng, s);
        ForestRules rules;
        rules.band_mask = 3;
        for (const Component& c : arcComponents(g, 3, {})) {
            const SpanningForest f1 = growBaselineForest(g, c, {}, rules);
            const std::vector<IncumbentLink> inc = incumbentLinksOf(g, f1);
            const SpanningForest f2 = growBaselineForest(g, c, inc, rules);
            ASSERT_EQ(f2.links.size(), f1.links.size());
            for (std::size_t q = 0; q < f1.links.size(); ++q) {
                EXPECT_EQ(f2.links[q].station, f1.links[q].station);
                EXPECT_EQ(f2.links[q].parent, f1.links[q].parent);
                EXPECT_EQ(f2.links[q].ref_sat, f1.links[q].ref_sat);
                EXPECT_TRUE(f2.links[q].from_incumbent);
            }
            for (int n : f2.reseats) EXPECT_EQ(n, 0);
            EXPECT_TRUE(f2.unreached.empty());
        }
    }
}

TEST(NetworkArcGraph, BaselineForestReseatsIncumbentLinksByCause) {
    // Station 0 tracks G01 G02 G03 G06; station 1 lacks G01; station 3 tracks G03 G04 G05;
    // stations 2 and 4 track G01..G06.  Incumbent: 0 root; 1 <- 0 via G01 (its arc is gone);
    // 2 <- 1 via G02 (its parent is re-seated, and then it is kept: card AGT, AGT-5); 3 <- 0 via
    // G03 (one common satellite while station 4 offers three); 4 <- 9 via G04 (the parent left).
    const std::vector<IncumbentLink> inc{
        {sta(0), std::nullopt, SatId{}}, {sta(1), sta(0), gps(1)}, {sta(2), sta(1), gps(2)},
        {sta(3), sta(0), gps(3)},        {sta(4), sta(9), gps(4)}};
    ForestRules rules;
    rules.band_mask            = 3;
    rules.refuse_slipped_first = true;
    const ArcGraph g0 = denseGraph(5, 6, {{0, 4}, {0, 5}, {1, 1}, {3, 1}, {3, 2}, {3, 6}});
    const Component c = arcComponents(g0, 3, {}).at(0);
    const SpanningForest f = growBaselineForest(g0, c, inc, rules);
    EXPECT_EQ(f.reseats[static_cast<std::size_t>(Reseat::ArcGone)], 1);       // 1 <- 0 via G01
    EXPECT_EQ(f.reseats[static_cast<std::size_t>(Reseat::ParentGone)], 1);    // 4 <- 9
    EXPECT_EQ(f.reseats[static_cast<std::size_t>(Reseat::TooFewCommon)], 1);  // 3 <- 0
    EXPECT_EQ(f.reseats[static_cast<std::size_t>(Reseat::Slipped)], 0);
    ASSERT_EQ(f.links.size(), 5u);
    EXPECT_EQ(f.links[0].station, 0);
    EXPECT_TRUE(f.links[0].from_incumbent);
    // Station 2 waits while 4 and then 1 (its re-seated parent) join, so it keeps its link to 1:
    // the subtree survives the re-seat instead of reversing under it.
    for (std::size_t q = 1; q < f.links.size(); ++q) {
        EXPECT_EQ(f.links[q].from_incumbent, f.links[q].station == 2) << "link " << q;
        if (f.links[q].station == 2) EXPECT_EQ(f.links[q].parent, 1);
    }
    EXPECT_EQ(f.links[1].station, 4);
    EXPECT_EQ(f.links[2].station, 1);
    EXPECT_EQ(f.links[3].station, 2);
    // A slipped reference: 2 <- 0 via G02 with (2, G02) slipped on L1.
    const ArcGraph g1 = denseGraph(3, 4, {}, {{{2, 2}, 1}});
    const std::vector<IncumbentLink> inc2{{sta(0), std::nullopt, SatId{}}, {sta(2), sta(0), gps(2)}};
    const SpanningForest f2 = growBaselineForest(g1, arcComponents(g1, 3, {}).at(0), inc2, rules);
    EXPECT_EQ(f2.reseats[static_cast<std::size_t>(Reseat::Slipped)], 1);
    bool seen2 = false;
    for (const StationLink& L : f2.links)
        if (L.station == 2) {
            seen2 = true;
            EXPECT_FALSE(L.from_incumbent);
            EXPECT_EQ(L.ref_sat, 0);   // re-seated off G02 onto the first clean common satellite
        }
    EXPECT_TRUE(seen2);
}

TEST(NetworkArcGraph, BaselineForestPicksTheMostDurableReference) {
    // Two stations share G01..G04; the weaker arc of G03 is the most durable.
    const ArcGraph g = denseGraph(2, 4, {}, {}, {{{0, 1}, 9.0}, {{1, 1}, 1.0}, {{0, 3}, 6.0}, {{1, 3}, 5.0}});
    const Component c = arcComponents(g, 3, {}).at(0);
    ForestRules rules;
    rules.band_mask = 3;
    EXPECT_EQ(growBaselineForest(g, c, {}, rules).links.at(1).ref_sat, 0);   // first common
    rules.by_durability = true;
    EXPECT_EQ(growBaselineForest(g, c, {}, rules).links.at(1).ref_sat, 2);   // G03: min(6, 5) = 5
}

TEST(NetworkArcGraph, BaselineForestFinishesThroughOneCommonSatelliteUnlessStopAtGap) {
    // Station 2 shares only G03 with the others.
    const ArcGraph g = graphOf(pairFamily(), 3, 4, {row(0, 1, 3), row(0, 2, 3), row(0, 3, 3), row(1, 1, 3), row(1, 2, 3),
                                                    row(2, 3, 3), row(2, 4, 3)});
    const Component c = arcComponents(g, 3, {}).at(0);
    ForestRules rules;
    rules.band_mask = 3;
    const SpanningForest full = growBaselineForest(g, c, {}, rules);
    EXPECT_EQ(full.station_order.size(), 3u);
    EXPECT_TRUE(full.unreached.empty());
    rules.stop_at_gap = true;
    const SpanningForest cut = growBaselineForest(g, c, {}, rules);
    EXPECT_EQ(cut.station_order.size(), 2u);
    EXPECT_EQ(cut.unreached, (std::vector<int>{stationVertex(g, 2), satVertex(g, 3)}));
}

// ============================== 4. bases ==============================

TEST(NetworkArcGraph, ShortBasisFullCoversTheCycleLatticeOfEveryBand) {
    std::mt19937 rng(303u);
    long long n_rows = 0, n_band_only = 0, n_borrowed = 0, n_fund = 0, n_partner = 0;
    for (int it = 0; it < 300; ++it) {
        GraphSpec s;
        s.n_sta     = uniform(rng, 2, 16);
        s.n_sat     = uniform(rng, 3, 14);
        s.n_blocks  = uniform(rng, 1, 3);
        s.p_arc     = std::uniform_real_distribution<double>(0.4, 0.95)(rng);
        s.p_partial = (it % 3 == 0) ? 0.0 : 0.15;
        s.n_l1_sta  = uniform(rng, 0, 2);
        s.n_l1_sat  = uniform(rng, 0, 2);
        s.p_slip    = (it % 2) ? 0.1 : 0.0;
        const ArcGraph g = randomPairGraph(rng, s);
        ForestRules rules;
        rules.band_mask            = 3;
        rules.refuse_slipped_first = chance(rng, 0.5);
        rules.by_durability        = chance(rng, 0.5);
        const SpanningForest f = pairForest(g, rules);
        const CycleBasis b = shortCycleBasis(g, f, BasisCoverage::Full);

        // Rows per band equal E - V + C.
        std::vector<int> per_band(2, 0);
        for (const BasisCycle& bc : b.cycles) ++per_band[static_cast<std::size_t>(bc.band)];
        for (int band = 0; band < 2; ++band) {
            ASSERT_EQ(b.lattice_dim[static_cast<std::size_t>(band)], bruteLatticeDim(g, band)) << "iteration " << it;
            ASSERT_EQ(per_band[static_cast<std::size_t>(band)], b.lattice_dim[static_cast<std::size_t>(band)]) << "iteration " << it;
        }
        // Every row is a cycle of its band whose pivot (+1) no earlier row of the band holds.
        for (std::size_t i = 0; i < b.cycles.size(); ++i) {
            const BasisCycle& bc = b.cycles[i];
            ASSERT_TRUE(isCycle(bc.cycle));
            for (const CycleTerm& t : bc.cycle.terms) ASSERT_EQ(bandOfSignal(g, t.arc.signal), bc.band);
            const ArcId pv = pivotOf(g, bc);
            ASSERT_EQ(coeffIn(bc.cycle, pv), 1);
            for (std::size_t j = 0; j < i; ++j)
                ASSERT_EQ(coeffIn(b.cycles[j].cycle, pv), 0) << "iteration " << it << " row " << i << " held by " << j;
            ASSERT_EQ(bc.band_only, !usable(g, bc.pivot_arc, 3));
            if (bc.kind != CycleKind::Fundamental) ASSERT_EQ(bc.cycle.terms.size(), 4u);
            n_band_only += bc.band_only;
            n_borrowed += bc.kind == CycleKind::BorrowedDd;
            n_partner += bc.kind == CycleKind::PartnerDd;
        }
        n_fund += b.n_fundamental;
        n_rows += static_cast<long long>(b.cycles.size());
        // Every ParentOnly row is in the Full basis.
        const CycleBasis po = shortCycleBasis(g, f, BasisCoverage::ParentOnly);
        for (const BasisCycle& pc : po.cycles) {
            const auto hit = std::find_if(b.cycles.begin(), b.cycles.end(), [&](const BasisCycle& bc) {
                return bc.band == pc.band && bc.pivot_arc == pc.pivot_arc && bc.kind == CycleKind::PartnerDd
                    && bc.partner == pc.partner && bc.ref_sat == pc.ref_sat && bc.cycle.terms.size() == pc.cycle.terms.size()
                    && std::equal(bc.cycle.terms.begin(), bc.cycle.terms.end(), pc.cycle.terms.begin(),
                                  [](const CycleTerm& x, const CycleTerm& y) { return x.arc == y.arc && x.coeff == y.coeff; });
            });
            ASSERT_NE(hit, b.cycles.end()) << "iteration " << it;
        }
        // L1 and L2 rows of a shared chord have identical shapes, the L1 copy first.
        for (std::size_t i = 0; i < b.cycles.size(); ++i) {
            const BasisCycle& x = b.cycles[i];
            if (x.band_only || x.band != 0) continue;
            ASSERT_LT(i + 1, b.cycles.size());
            const BasisCycle& y = b.cycles[i + 1];
            ASSERT_EQ(y.band, 1);
            ASSERT_EQ(y.pivot_arc, x.pivot_arc);
            ASSERT_EQ(y.kind, x.kind);
            ASSERT_EQ(y.partner, x.partner);
            ASSERT_EQ(y.ref_sat, x.ref_sat);
            ASSERT_EQ(y.cycle.terms.size(), x.cycle.terms.size());
            for (std::size_t q = 0; q < x.cycle.terms.size(); ++q) {
                ASSERT_EQ(y.cycle.terms[q].arc.station, x.cycle.terms[q].arc.station);
                ASSERT_EQ(y.cycle.terms[q].arc.sat, x.cycle.terms[q].arc.sat);
                ASSERT_EQ(y.cycle.terms[q].coeff, x.cycle.terms[q].coeff);
            }
        }
        // A Z-basis: every fundamental cycle of a chain forest of each band has integer
        // coordinates, and they rebuild it.
        for (int band = 0; band < 2; ++band) {
            const std::uint8_t m = static_cast<std::uint8_t>(1u << band);
            std::vector<SpanningForest> parts;
            for (const Component& c : arcComponents(g, m, {})) {
                ForestRules cr;
                cr.band_mask = m;
                parts.push_back(growChainForest(g, c, {c.root}, {}, cr, {}, {}));
            }
            const CycleBasis fb = fundamentalBasis(g, joinForests(g, parts), band);
            ASSERT_EQ(fb.lattice_dim[static_cast<std::size_t>(band)], b.lattice_dim[static_cast<std::size_t>(band)]);
            for (const BasisCycle& fc : fb.cycles) {
                const std::optional<Coordinates> x = cycleCoordinates(b, fc.cycle);
                ASSERT_TRUE(x.has_value()) << "iteration " << it;
                const Cycle back = combineCycles(b, *x);
                ASSERT_EQ(back.terms.size(), fc.cycle.terms.size());
                for (std::size_t q = 0; q < back.terms.size(); ++q) {
                    ASSERT_EQ(back.terms[q].arc, fc.cycle.terms[q].arc);
                    ASSERT_EQ(back.terms[q].coeff, fc.cycle.terms[q].coeff);
                }
            }
        }
    }
    EXPECT_GT(n_rows, 5000);
    EXPECT_GT(n_band_only, 100);
    EXPECT_GT(n_borrowed, 50);
    EXPECT_GT(n_partner, 1000);
    std::cout << "[ NetworkArcGraph ] Full rows " << n_rows << ", partner " << n_partner << ", borrowed " << n_borrowed
              << ", fundamental " << n_fund << ", band-only " << n_band_only << "\n";
}

TEST(NetworkArcGraph, ShortBasisCompletesPartialL2WithBandOnlyArcs) {
    // Stations 0..2 track G01..G04 on both bands; station 3 tracks G01, G02 on L1 only; G05 is
    // tracked on L1 only, by stations 0 and 1; station 1's G04 lacks L2.
    std::vector<ArcRow> rows;
    for (int r = 0; r < 3; ++r)
        for (int k = 1; k <= 4; ++k) rows.push_back(row(r, k, (r == 1 && k == 4) ? 1 : 3));
    rows.push_back(row(3, 1, 1));
    rows.push_back(row(3, 2, 1));
    rows.push_back(row(0, 5, 1));
    rows.push_back(row(1, 5, 1));
    const ArcGraph g = graphOf(pairFamily(), 4, 5, rows);
    ForestRules rules;
    rules.band_mask = 3;
    const SpanningForest f = pairForest(g, rules);
    const CycleBasis b = shortCycleBasis(g, f, BasisCoverage::Full);
    // L1: 16 arcs, 9 vertices, 1 component -> 8.  L2: 11 arcs, 7 vertices -> 5.
    EXPECT_EQ(b.lattice_dim, (std::vector<int>{8, 5}));
    int l1 = 0, l2 = 0, band_only = 0;
    for (const BasisCycle& bc : b.cycles) {
        (bc.band == 0 ? l1 : l2) += 1;
        if (bc.band_only) {
            ++band_only;
            EXPECT_EQ(bc.band, 0);
            EXPECT_NE(bc.kind, CycleKind::Fundamental);   // every band-only chord closes as a DD here
        }
    }
    EXPECT_EQ(l1, 8);
    EXPECT_EQ(l2, 5);
    EXPECT_EQ(band_only, 3);   // (1, G04), one of station 3's arcs, one of G05's arcs
    EXPECT_EQ(b.n_fundamental, 0);
}

TEST(NetworkArcGraph, FundamentalBasisClosesEveryChordOfTheForest) {
    std::mt19937 rng(404u);
    for (int it = 0; it < 100; ++it) {
        GraphSpec s;
        s.n_sta = uniform(rng, 2, 12);
        s.n_sat = uniform(rng, 2, 12);
        const ArcGraph g = randomPairGraph(rng, s);
        std::vector<SpanningForest> parts;
        for (const Component& c : arcComponents(g, 1, {})) parts.push_back(growChainForest(g, c, {c.root}, {}, ForestRules{}, {}, {}));
        const SpanningForest f = joinForests(g, parts);
        const CycleBasis fb = fundamentalBasis(g, f, 0);
        ASSERT_EQ(static_cast<int>(fb.cycles.size()), bruteLatticeDim(g, 0));
        for (const BasisCycle& bc : fb.cycles) {
            ASSERT_TRUE(isCycle(bc.cycle));
            int chords = 0;
            for (const CycleTerm& t : bc.cycle.terms) {
                const int a = arcIndex(g, t.arc.station.index, t.arc.sat.prn - 1);
                chords += f.is_tree_arc[static_cast<std::size_t>(a)] ? 0 : 1;
            }
            ASSERT_EQ(chords, 1);
        }
    }
}

TEST(NetworkArcGraph, IsCycleChecksEveryBand) {
    const SignalId l1 = sigOf("L1C"), l2 = sigOf("L2W");
    EXPECT_TRUE(isCycle(dd(0, 1, 2, 3, l1)));
    Cycle mixed = dd(0, 1, 2, 3, l1);
    mixed.terms[0].arc.signal = l2;   // one term moved to another band: open in both
    std::sort(mixed.terms.begin(), mixed.terms.end(), [](const CycleTerm& x, const CycleTerm& y) { return x.arc < y.arc; });
    EXPECT_FALSE(isCycle(mixed));
    Cycle cross = dd(0, 1, 2, 3, l1);
    for (const CycleTerm& t : dd(0, 1, 2, 3, l2).terms) cross.terms.push_back(CycleTerm{t.arc, -t.coeff});
    EXPECT_TRUE(isCycle(cross));   // a cross-band combination of two cycles
}

// ============================== 5. maps ==============================

TEST(NetworkArcGraph, CycleCoordinatesAreExactAndRefuseOutsideTheSpan) {
    std::mt19937 rng(505u);
    for (int it = 0; it < 100; ++it) {
        GraphSpec s;
        s.n_sta = uniform(rng, 3, 12);
        s.n_sat = uniform(rng, 3, 12);
        s.p_partial = 0.1;
        const ArcGraph g = randomPairGraph(rng, s);
        ForestRules rules;
        rules.band_mask = 3;
        const CycleBasis b = shortCycleBasis(g, pairForest(g, rules), BasisCoverage::Full);
        if (b.cycles.empty()) continue;
        Coordinates want;
        for (int i = 0; i < static_cast<int>(b.cycles.size()); ++i)
            if (chance(rng, 0.3)) want.emplace_back(i, uniform(rng, -3, 3));
        want.erase(std::remove_if(want.begin(), want.end(), [](const auto& p) { return p.second == 0; }), want.end());
        const Cycle x = combineCycles(b, want);
        const std::optional<Coordinates> got = cycleCoordinates(b, x);
        ASSERT_TRUE(got.has_value());
        ASSERT_EQ(*got, want);
        // A single arc is not a cycle; nor is a cycle of arcs outside the graph.
        Cycle one;
        one.terms.push_back(b.cycles[0].cycle.terms[0]);
        EXPECT_FALSE(cycleCoordinates(b, one).has_value());
        EXPECT_FALSE(cycleCoordinates(b, dd(90, 91, 70, 71, sigOf("L1C"))).has_value());
    }
}

TEST(NetworkArcGraph, BasisChangeMapIsTheIdentityOnAnUnchangedGraph) {
    std::mt19937 rng(606u);
    GraphSpec s;
    s.n_sta = 12;
    s.n_sat = 10;
    s.p_partial = 0.1;
    const ArcGraph g = randomPairGraph(rng, s);
    ForestRules rules;
    rules.band_mask = 3;
    const CycleBasis b = shortCycleBasis(g, pairForest(g, rules), BasisCoverage::Full);
    ASSERT_FALSE(b.cycles.empty());
    const auto W = basisChangeMap(b, b, {});
    ASSERT_EQ(W.size(), b.cycles.size());
    for (std::size_t i = 0; i < W.size(); ++i) {
        ASSERT_TRUE(W[i].has_value());
        ASSERT_EQ(*W[i], (Coordinates{{static_cast<int>(i), 1}}));
    }
}

TEST(NetworkArcGraph, BasisChangeMapIsUnimodularAcrossAForcedTreeSwap) {
    std::mt19937 rng(707u);
    for (int it = 0; it < 60; ++it) {
        GraphSpec s;
        s.n_sta = uniform(rng, 4, 12);
        s.n_sat = uniform(rng, 4, 10);
        s.p_arc = 0.8;
        s.p_partial = 0.1;
        ArcGraph g = randomPairGraph(rng, s);
        for (ArcRow& r : g.rows) r.durability = uniform(rng, 0, 20);
        ForestRules a;
        a.band_mask = 3;
        ForestRules bb = a;
        bb.by_durability = true;
        bb.min_common    = 1;   // another tree: another root order, other references
        const CycleBasis A = shortCycleBasis(g, pairForest(g, a), BasisCoverage::Full);
        // Force another root: the last station of each component's forest becomes the root.
        std::vector<SpanningForest> parts;
        for (const Component& c : arcComponents(g, 3, {})) {
            std::vector<IncumbentLink> inc{{g.stations[static_cast<std::size_t>(c.stations.back())], std::nullopt, SatId{}}};
            parts.push_back(growBaselineForest(g, c, inc, bb));
        }
        const CycleBasis B = shortCycleBasis(g, joinForests(g, parts), BasisCoverage::Full);
        ASSERT_EQ(A.cycles.size(), B.cycles.size());
        const auto W  = basisChangeMap(A, B, {});   // B's cycles in A's coordinates
        const auto Wi = basisChangeMap(B, A, {});   // A's cycles in B's coordinates
        const std::size_t n = A.cycles.size();
        std::vector<std::vector<long long>> M(n, std::vector<long long>(n, 0)), Mi = M;
        for (std::size_t i = 0; i < n; ++i) {
            ASSERT_TRUE(W[i].has_value());
            ASSERT_TRUE(Wi[i].has_value());
            for (const auto& [j, c] : *W[i]) M[i][static_cast<std::size_t>(j)] = c;
            for (const auto& [j, c] : *Wi[i]) Mi[i][static_cast<std::size_t>(j)] = c;
        }
        // M * Mi = I exactly: both are integer, so |det M| = 1.
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t k = 0; k < n; ++k) {
                long long v = 0;
                for (std::size_t j = 0; j < n; ++j) v += M[i][j] * Mi[j][k];
                ASSERT_EQ(v, i == k ? 1 : 0) << "iteration " << it;
            }
        // W maps a planted integer truth exactly.
        const auto N = randomTruth(rng, g);
        for (std::size_t i = 0; i < n; ++i) {
            long long v = 0;
            for (const auto& [j, c] : *W[i]) v += c * valueOf(A.cycles[static_cast<std::size_t>(j)].cycle, N);
            ASSERT_EQ(v, valueOf(B.cycles[i].cycle, N));
        }
    }
}

TEST(NetworkArcGraph, BasisChangeMapReReferencesAcrossASlippedLinkReference) {
    // Six stations tracking G01..G08 on both bands; station 3's link reference slips.
    const ArcGraph g0 = denseGraph(6, 8);
    ForestRules rules;
    rules.band_mask            = 3;
    rules.refuse_slipped_first = true;
    const Component c0 = arcComponents(g0, 3, {}).at(0);
    const SpanningForest f0 = growBaselineForest(g0, c0, {}, rules);
    const CycleBasis b0 = shortCycleBasis(g0, f0, BasisCoverage::Full);
    const auto link3 = std::find_if(f0.links.begin(), f0.links.end(), [](const StationLink& L) { return L.station == 3; });
    ASSERT_NE(link3, f0.links.end());
    const int ref = link3->ref_sat;   // satellite slot; prn ref + 1
    const ArcGraph g1 = denseGraph(6, 8, {}, {{{3, ref + 1}, 3}});
    const Component c1 = arcComponents(g1, 3, {}).at(0);
    const SpanningForest f1 = growBaselineForest(g1, c1, incumbentLinksOf(g0, f0), rules);
    EXPECT_EQ(f1.reseats[static_cast<std::size_t>(Reseat::Slipped)], 1);
    const CycleBasis b1 = shortCycleBasis(g1, f1, BasisCoverage::Full);
    const std::set<ArcId> slipped{ArcId{sta(3), gps(ref + 1), sigOf("L1C")}, ArcId{sta(3), gps(ref + 1), sigOf("L2W")}};
    const auto W = basisChangeMap(b0, b1, slipped);
    // Truth: the slipped arc's integer changes; every other arc keeps its integer.
    std::mt19937 rng(808u);
    std::map<ArcId, long long> N0 = randomTruth(rng, g0), N1 = N0;
    for (const ArcId& a : slipped) N1[a] += 5;
    int fresh = 0, mapped = 0, mapped_at_3 = 0;
    for (std::size_t i = 0; i < W.size(); ++i) {
        const bool touches = std::any_of(b1.cycles[i].cycle.terms.begin(), b1.cycles[i].cycle.terms.end(),
                                         [&](const CycleTerm& t) { return slipped.count(t.arc) != 0; });
        ASSERT_EQ(!W[i].has_value(), touches) << "row " << i;
        if (!W[i]) { ++fresh; continue; }
        ++mapped;
        if (b1.cycles[i].station == sta(3)) ++mapped_at_3;
        long long v = 0;
        for (const auto& [j, cf] : *W[i]) v += cf * valueOf(b0.cycles[static_cast<std::size_t>(j)].cycle, N0);
        ASSERT_EQ(v, valueOf(b1.cycles[i].cycle, N1));
    }
    EXPECT_EQ(fresh, 2);          // the old reference's own chord, one row per band
    EXPECT_GT(mapped_at_3, 10);   // station 3's other partner DDs re-reference instead
    EXPECT_GT(mapped, 50);
}

TEST(NetworkArcGraph, CombineCyclesSumsTermByTerm) {
    CycleBasis b;
    BasisCycle x, y;
    x.cycle = dd(0, 1, 2, 3, sigOf("L1C"));
    y.cycle = dd(0, 1, 4, 3, sigOf("L1C"));
    b.cycles = {x, y};
    const Cycle z = combineCycles(b, {{0, 1}, {1, -1}});   // = DD(0, 1; 2, 4)
    const Cycle want = dd(0, 1, 2, 4, sigOf("L1C"));
    ASSERT_EQ(z.terms.size(), want.terms.size());
    for (std::size_t q = 0; q < z.terms.size(); ++q) {
        EXPECT_EQ(z.terms[q].arc, want.terms[q].arc);
        EXPECT_EQ(z.terms[q].coeff, want.terms[q].coeff);
    }
}

// ============================== 6. lattice ==============================

/// The reduced echelon invariant: every pivot at +1 in its row and absent from every other row.
void expectReducedEchelon(const CycleLattice& L) {
    ASSERT_EQ(L.rows.size(), L.values.size());
    ASSERT_EQ(L.rows.size(), L.pivot_row.size());
    for (const auto& [arc, k] : L.pivot_row)
        for (std::size_t q = 0; q < L.rows.size(); ++q)
            ASSERT_EQ(coeffIn(L.rows[q], arc), static_cast<int>(q) == k ? 1 : 0);
}

TEST(NetworkArcGraph, LatticeKeepsPlantedTruthThroughRandomOffersAndEliminations) {
    std::mt19937 rng(909u);
    GraphSpec s;
    s.n_sta = 10;
    s.n_sat = 9;
    s.p_arc = 0.8;
    s.p_partial = 0.1;
    const ArcGraph g = randomPairGraph(rng, s);
    ForestRules rules;
    rules.band_mask = 3;
    const CycleBasis b = shortCycleBasis(g, pairForest(g, rules), BasisCoverage::Full);
    ASSERT_GT(b.cycles.size(), 20u);
    std::map<ArcId, long long> N = randomTruth(rng, g);
    std::vector<ArcId> arcs;
    for (const auto& [a, v] : N) arcs.push_back(a);
    CycleLattice L;
    std::map<LatticeVerdict, int> verdicts;
    int unit_eliminations = 0;
    for (int step = 0; step < 10000; ++step) {
        if (uniform(rng, 0, 9) < 9) {   // nine offers per release: the lattice fills, then confirms
            Coordinates co;
            for (int q = 0, n = chance(rng, 0.6) ? 1 : uniform(rng, 2, 3); q < n; ++q) {
                int c = uniform(rng, -2, 2);
                if (c == 0) c = 1;
                co.emplace_back(uniform(rng, 0, static_cast<int>(b.cycles.size()) - 1), c);
            }
            const Cycle x = combineCycles(b, co);
            if (x.terms.empty()) continue;
            const LatticeVerdict v = latticeOffer(L, x, valueOf(x, N));
            ASSERT_NE(v, LatticeVerdict::Contradicted) << "step " << step;
            ++verdicts[v];
        } else {
            // Release an arc a row touches (or any arc), then let its integer slip.
            ArcId a = arcs[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(arcs.size()) - 1))];
            if (!L.rows.empty() && chance(rng, 0.8)) {
                const Cycle& r = L.rows[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(L.rows.size()) - 1))];
                a = r.terms[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(r.terms.size()) - 1))].arc;
            }
            bool unit = false;
            for (const Cycle& r : L.rows) { const long long c = coeffIn(r, a); unit = unit || c == 1 || c == -1; }
            const std::size_t before = L.rows.size();
            const int dropped = latticeEliminateArc(L, a);
            if (unit) {
                ASSERT_EQ(dropped, 1) << "step " << step;
                ASSERT_EQ(L.rows.size(), before - 1);
                ++unit_eliminations;
            }
            for (const Cycle& r : L.rows) ASSERT_EQ(coeffIn(r, a), 0);
            N[a] += uniform(rng, 1, 9);
        }
        if (step % 10 == 0) expectReducedEchelon(L);
        for (std::size_t q = 0; q < L.rows.size(); ++q) ASSERT_EQ(L.values[q], valueOf(L.rows[q], N)) << "step " << step;
    }
    EXPECT_GT(verdicts[LatticeVerdict::Added], 500);
    EXPECT_GT(verdicts[LatticeVerdict::Confirmed], 1000);
    EXPECT_GT(unit_eliminations, 500);
    std::cout << "[ NetworkArcGraph ] lattice: added " << verdicts[LatticeVerdict::Added] << ", confirmed "
              << verdicts[LatticeVerdict::Confirmed] << ", not primitive " << verdicts[LatticeVerdict::NotPrimitive]
              << ", unit eliminations " << unit_eliminations << "\n";
}

TEST(NetworkArcGraph, LatticeTurnsTenHeldDdsOnASlippedReferenceIntoNine) {
    // Station 0 against partner 1, reference G01, satellites G02..G11.
    const SignalId l1 = sigOf("L1C");
    std::mt19937 rng(1001u);
    std::map<ArcId, long long> N;
    for (int r = 0; r < 2; ++r)
        for (int k = 1; k <= 11; ++k) N[ArcId{sta(r), gps(k), l1}] = uniform(rng, -40, 40);
    CycleLattice L;
    for (int j = 2; j <= 11; ++j) {
        const Cycle x = dd(0, 1, j, 1, l1);
        ASSERT_EQ(latticeOffer(L, x, valueOf(x, N)), LatticeVerdict::Added);
    }
    ASSERT_EQ(L.rows.size(), 10u);
    const ArcId ref{sta(0), gps(1), l1};
    EXPECT_EQ(latticeEliminateArc(L, ref), 1);
    ASSERT_EQ(L.rows.size(), 9u);
    N[ref] += 4;   // the slip
    for (std::size_t q = 0; q < L.rows.size(); ++q) {
        EXPECT_EQ(coeffIn(L.rows[q], ref), 0);
        EXPECT_EQ(L.rows[q].terms.size(), 4u);   // re-referenced: still a DD on (0, 1)
        EXPECT_TRUE(isCycle(L.rows[q]));
        EXPECT_EQ(L.values[q], valueOf(L.rows[q], N));
    }
    // A DD between two of the held satellites is implied by the nine.
    const Cycle between = dd(0, 1, 5, 9, l1);
    EXPECT_EQ(latticeOffer(L, between, valueOf(between, N)), LatticeVerdict::Confirmed);
    ASSERT_TRUE(latticeImplied(L, between).has_value());
    EXPECT_EQ(*latticeImplied(L, between), valueOf(between, N));
    EXPECT_FALSE(latticeImplied(L, dd(0, 1, 5, 1, l1)).has_value());   // through the released arc
    expectReducedEchelon(L);
}

TEST(NetworkArcGraph, LatticeContradictsAPlantedWrongInteger) {
    const SignalId l1 = sigOf("L1C");
    CycleLattice L;
    ASSERT_EQ(latticeOffer(L, dd(0, 1, 2, 3, l1), 7), LatticeVerdict::Added);
    ASSERT_EQ(latticeOffer(L, dd(0, 1, 4, 3, l1), -2), LatticeVerdict::Added);
    EXPECT_EQ(latticeOffer(L, dd(0, 1, 2, 3, l1), 7), LatticeVerdict::Confirmed);
    EXPECT_EQ(latticeOffer(L, dd(0, 1, 2, 3, l1), 8), LatticeVerdict::Contradicted);
    EXPECT_EQ(latticeOffer(L, dd(0, 1, 2, 4, l1), 9), LatticeVerdict::Confirmed);      // 7 - (-2)
    EXPECT_EQ(latticeOffer(L, dd(0, 1, 2, 4, l1), 10), LatticeVerdict::Contradicted);
    EXPECT_EQ(L.rows.size(), 2u);
}

TEST(NetworkArcGraph, LatticeKeepsEntryEpochsAndAPlantedShift) {
    // Card FCF's amendments: the entry epoch of each row (a reduced row takes the later one), the
    // newest entry behind an implied integer, and a planted shift on one arc.
    const SignalId l1 = sigOf("L1C");
    CycleLattice L;
    ASSERT_EQ(latticeOffer(L, dd(0, 1, 2, 3, l1), 7, 4), LatticeVerdict::Added);
    ASSERT_EQ(latticeOffer(L, dd(0, 1, 4, 3, l1), -2, 9), LatticeVerdict::Added);
    ASSERT_EQ(L.entered_ep.size(), 2u);
    // The second offer reduced the first row (it held the new pivot), so both rows are from 9.
    EXPECT_EQ(L.entered_ep[0], 9);
    EXPECT_EQ(L.entered_ep[1], 9);
    ASSERT_EQ(latticeOffer(L, dd(2, 1, 2, 3, l1), 5, 12), LatticeVerdict::Added);
    // The third offer's pivot (1, G02) was held by the first row, which takes epoch 12.
    EXPECT_EQ(L.entered_ep, (std::vector<long long>{12, 9, 12}));
    long long newest = 0;
    ASSERT_EQ(latticeImplied(L, dd(0, 1, 2, 4, l1), &newest), 9);
    EXPECT_EQ(newest, 12);   // rows 0 and 2
    ASSERT_EQ(latticeImplied(L, dd(0, 1, 4, 3, l1), &newest), -2);
    EXPECT_EQ(newest, 9);    // row 1 alone
    ASSERT_EQ(latticeImplied(L, dd(2, 0, 2, 3, l1), &newest), 5 - 7);
    EXPECT_EQ(newest, 12);
    expectReducedEchelon(L);
    // A planted shift of +3 on (1, G03), which all three rows hold: a cycle through it at +1
    // moves by 3, and one that misses it keeps its integer.
    EXPECT_EQ(latticeShiftArc(L, ArcId{sta(1), gps(3), l1}, 3), 3);
    EXPECT_EQ(latticeImplied(L, dd(2, 1, 2, 3, l1)), 5 + 3);
    EXPECT_EQ(latticeImplied(L, dd(0, 1, 2, 4, l1)), 9);
    EXPECT_EQ(latticeShiftArc(L, ArcId{sta(7), gps(7), l1}, 3), 0); // an arc no row holds
}

TEST(NetworkArcGraph, LatticeRefusesANonPrimitiveResidual) {
    const SignalId l1 = sigOf("L1C");
    CycleLattice L;
    Cycle twice = dd(0, 1, 2, 3, l1);
    for (CycleTerm& t : twice.terms) t.coeff *= 2;
    EXPECT_EQ(latticeOffer(L, twice, 4), LatticeVerdict::NotPrimitive);
    EXPECT_TRUE(L.rows.empty());
    ASSERT_EQ(latticeOffer(L, dd(0, 1, 2, 3, l1), 2), LatticeVerdict::Added);
    EXPECT_EQ(latticeOffer(L, twice, 4), LatticeVerdict::Confirmed);   // in the span: saturated
    EXPECT_EQ(latticeEliminateArc(L, ArcId{sta(5), gps(5), l1}), 0);  // an arc no row touches
}

}  // namespace
}  // namespace GnssProcessor
