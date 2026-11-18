// Unit tests of the held-first walk by remaining life (include/network/ar/ArcGraph.hpp, ForestRules::held_by_life;
// network.ring_fix.held_first_by_life): among the held arcs from the attached vertices to an unattached one, the
// most durable is taken; on a tie, and without the rule, the first held arc in the walk's order.

#include "network/ar/ArcGraph.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace GnssProcessor {
namespace {

using namespace arcgraph;

SignalId l1c() { return *SignalId::fromObsCode(System::GPS, "L1C"); }

ArcRow arcRow(int station, int prn, double durability) {
    ArcRow r;
    r.station    = StationId{station};
    r.sat        = SatId{System::GPS, prn};
    r.present    = 1;
    r.durability = durability;
    return r;
}

/// Stations A (root), B and C hang from satellite x through the incumbent tree; satellite y is reachable only
/// through the held arcs (B, y) and (C, y), of durabilities @p life_b and @p life_c.  Returns the arc the walk takes
/// to y.
int arcToY(double life_b, double life_c, bool by_life, ArcGraph* graph_out = nullptr) {
    const ArcFamily fam{System::GPS, {l1c()}};
    const ArcGraph g = buildArcGraph(fam, {StationId{0}, StationId{1}, StationId{2}},
                                     {SatId{System::GPS, 1}, SatId{System::GPS, 2}},
                                     {arcRow(0, 1, 50.0), arcRow(1, 1, 50.0), arcRow(2, 1, 50.0),
                                      arcRow(1, 2, life_b), arcRow(2, 2, life_c)});
    const Component c = arcComponents(g, 1, {}).at(0);
    const std::vector<int> incumbent{arcIndex(g, 0, 0), arcIndex(g, 1, 0), arcIndex(g, 2, 0)};
    std::vector<char> held(g.rows.size(), 0);
    held[static_cast<std::size_t>(arcIndex(g, 1, 1))] = 1;
    held[static_cast<std::size_t>(arcIndex(g, 2, 1))] = 1;
    ForestRules rules;
    rules.held_arcs = &held;
    rules.held_by_life = by_life;
    const SpanningForest f = growChainForest(g, c, {stationVertex(g, 0)}, incumbent, rules, {}, {});
    if (graph_out != nullptr) *graph_out = g;
    for (const TreeEdge& e : f.edges)
        if (e.arc == arcIndex(g, 1, 1) || e.arc == arcIndex(g, 2, 1)) {
            EXPECT_TRUE(e.held_first);
            return e.arc;
        }
    ADD_FAILURE() << "no held arc to y in the forest";
    return -1;
}

}  // namespace

TEST(HeldFirstByLife, TheWalkTakesTheFirstHeldArcWithoutTheRule) {
    ArcGraph g;
    const int a = arcToY(10.0, 100.0, /*by_life=*/false, &g);
    EXPECT_EQ(a, arcIndex(g, 1, 1));   // (B, y): B is attached before C
}

TEST(HeldFirstByLife, TheMostDurableHeldArcIsTakenByLife) {
    ArcGraph g;
    const int a = arcToY(10.0, 100.0, /*by_life=*/true, &g);
    EXPECT_EQ(a, arcIndex(g, 2, 1));   // (C, y): the rising arc
}

TEST(HeldFirstByLife, ATieKeepsTheWalksOrder) {
    ArcGraph g;
    const int a = arcToY(40.0, 40.0, /*by_life=*/true, &g);
    EXPECT_EQ(a, arcIndex(g, 1, 1));
}

TEST(HeldFirstByLife, TheFirstHeldArcIsKeptWhenItIsTheMostDurable) {
    ArcGraph g;
    const int a = arcToY(120.0, 30.0, /*by_life=*/true, &g);
    EXPECT_EQ(a, arcIndex(g, 1, 1));
}

}  // namespace GnssProcessor
