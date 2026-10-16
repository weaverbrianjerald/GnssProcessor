/**
 * @file test_producer_ar_cycle_basis.cpp
 * @brief Unit tests of the producer AR search's cycle basis (card AGT, step AGT-5;
 *        include/network/ar/NetworkArCycleBasis.hpp).
 *
 * Suite @c ProducerArCycleBasis:
 *  - the families: the two signals with the most arcs share one graph, every further signal
 *    stands alone;
 *  - on random pair graphs (partial L2, slips, several components, a sticky chain of searches):
 *    each band holds its lattice dimension in DD rows plus fundamental fallbacks, every row is a
 *    cycle, and the two bands of a shared chord are adjacent copies of one shape with one search
 *    block (card AGT section 4: "L1 and L2 rows share shape and block key");
 *  - a slipped link reference re-seats the link, and the station's partner DDs map onto the
 *    stored basis instead of going fresh (card AGT section 4);
 *  - stickiness: an unchanged graph keeps every link and every cycle; a changed one keeps the
 *    links that are still valid where a fresh forest would move them;
 *  - the map states and their causes, and the trace names.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "network/ar/NetworkArCycleBasis.hpp"
#include "network/ar/NetworkArcGraph.hpp"
#include "network/ar/NetworkProducerAr.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using arcgraph::BasisCycle;
using arcgraph::CycleKind;

SignalId sig(const char* code) { return *SignalId::fromObsCode(System::GPS, code); }

SatId gps(int prn) {
    SatId s;
    s.sys = System::GPS;
    s.prn = prn;
    return s;
}

std::string staName(const StationId& s) { return "S" + std::to_string(s.index); }

/// A GPS L1C/L2W family over stations 0..n-1 and satellites G01..Gm: present[s][k] holds the
/// band bits of arc (s, k), slipped[s][k] its slipped bits, durability[s][k] its durability.
struct PairBed {
    std::vector<std::vector<std::uint8_t>> present;
    std::vector<std::vector<std::uint8_t>> slipped;
    std::vector<std::vector<double>>       durability;

    PairBed(int n_sta, int n_sat)
        : present(static_cast<std::size_t>(n_sta), std::vector<std::uint8_t>(static_cast<std::size_t>(n_sat), 0)),
          slipped(present),
          durability(static_cast<std::size_t>(n_sta), std::vector<double>(static_cast<std::size_t>(n_sat), 10.0)) {}

    ArCycleInput input() const {
        ArCycleInput in;
        in.family = arcgraph::ArcFamily{System::GPS, {sig("L1C"), sig("L2W")}};
        for (std::size_t s = 0; s < present.size(); ++s) in.stations.push_back(StationId{static_cast<int>(s)});
        for (std::size_t k = 0; k < present[0].size(); ++k) in.sats.push_back(gps(static_cast<int>(k) + 1));
        for (std::size_t k = 0; k < present[0].size(); ++k)
            for (std::size_t s = 0; s < present.size(); ++s) {
                if (present[s][k] == 0) continue;
                arcgraph::ArcRow row;
                row.station    = StationId{static_cast<int>(s)};
                row.sat        = gps(static_cast<int>(k) + 1);
                row.present    = present[s][k];
                row.slipped    = slipped[s][k];
                row.durability = durability[s][k];
                in.rows.push_back(row);
            }
        return in;
    }
};

/// A complete graph: every station tracks every satellite on both bands.
PairBed completeBed(int n_sta, int n_sat) {
    PairBed bed(n_sta, n_sat);
    for (auto& row : bed.present) std::fill(row.begin(), row.end(), std::uint8_t{3});
    return bed;
}

/// A random pair bed: one to three visibility regions, partial L2, slips, random durability.
PairBed randomBed(std::mt19937& rng) {
    std::uniform_int_distribution<int> nsta(2, 25), nsat(4, 14), ncomp(1, 3);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const int n_sta = nsta(rng), n_sat = nsat(rng), n_comp = ncomp(rng);
    PairBed bed(n_sta, n_sat);
    for (int s = 0; s < n_sta; ++s)
        for (int k = 0; k < n_sat; ++k) {
            if ((s % n_comp) != (k % n_comp)) continue;   // regions: one component each
            if (u(rng) > 0.75) continue;
            std::uint8_t bits = 3;
            if (u(rng) < 0.10) bits = 1;   // L1 only
            else if (u(rng) < 0.03) bits = 2;   // L2 only
            bed.present[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] = bits;
            if (u(rng) < 0.05) bed.slipped[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] = bits;
            bed.durability[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] =
                static_cast<double>(static_cast<int>(u(rng) * 40.0));
        }
    return bed;
}

/// Rows and fallbacks per band against the lattice dimension.
void expectFullCoverage(const ArCycleBuild& b, std::size_t n_bands) {
    std::vector<int> rows(n_bands, 0);
    for (const BasisCycle& bc : b.basis.cycles) ++rows[static_cast<std::size_t>(bc.band)];
    for (std::size_t k = 0; k < n_bands; ++k)
        EXPECT_EQ(rows[k], b.basis.lattice_dim[k]) << "band " << k;
}

// ------------------------------------------------------------------ families

TEST(ProducerArCycleBasis, FamiliesPairTheTwoSignalsWithTheMostArcs) {
    std::vector<ArFamilySpec> f = arCycleFamilies(System::GPS, {{"L1C", 100}, {"L2W", 90}, {"L5Q", 95}});
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].signals, (std::vector<std::string>{"L1C", "L5Q"}));   // the two largest, pool order
    EXPECT_EQ(f[0].bands, (std::vector<SignalId>{sig("L1C"), sig("L5Q")}));
    EXPECT_EQ(f[1].signals, (std::vector<std::string>{"L2W"}));
    // Ties go to the pool's order.
    f = arCycleFamilies(System::GPS, {{"L1C", 50}, {"L2W", 50}, {"L5Q", 50}});
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].signals, (std::vector<std::string>{"L1C", "L2W"}));
    EXPECT_EQ(f[1].signals, (std::vector<std::string>{"L5Q"}));
    // One signal: a one-band family.  A code SignalId refuses is left out.
    f = arCycleFamilies(System::GPS, {{"X9", 7}, {"L1C", 3}});
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].signals, (std::vector<std::string>{"L1C"}));
    EXPECT_TRUE(arCycleFamilies(System::GPS, {}).empty());
}

TEST(ProducerArCycleBasis, TheForestRulesAreTheCycleBasisRules) {
    const arcgraph::ForestRules r = arCycleForestRules(3);
    EXPECT_EQ(r.band_mask, 3);
    EXPECT_EQ(r.min_common, 2);
    EXPECT_FALSE(r.stop_at_gap);
    EXPECT_TRUE(r.refuse_slipped_first);
    EXPECT_TRUE(r.by_durability);
}

// ------------------------------------------------------------------ coverage and pair shapes

TEST(ProducerArCycleBasis, L1AndL2RowsShareShapeAndBlockKey) {
    std::mt19937 rng(20261011);
    long long n_shared = 0, n_band_only = 0, n_borrowed = 0, n_fund = 0, n_multi = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const PairBed bed = randomBed(rng);
        // A sticky chain of two searches: the second is seeded with the first's links.
        const ArCycleBuild first = arCycleBuild(bed.input(), {});
        const ArCycleBuild b =
            arCycleBuild(bed.input(), arcgraph::incumbentLinksOf(first.graph, first.forest));
        if (b.components.size() > 1) ++n_multi;
        expectFullCoverage(b, 2);
        const auto& cyc = b.basis.cycles;
        for (std::size_t i = 0; i < cyc.size(); ++i) {
            const BasisCycle& bc = cyc[i];
            EXPECT_TRUE(arcgraph::isCycle(bc.cycle));
            if (bc.kind == CycleKind::Fundamental) { ++n_fund; continue; }
            EXPECT_EQ(bc.cycle.terms.size(), 4u);
            if (bc.kind == CycleKind::BorrowedDd) ++n_borrowed;
            if (bc.band_only) { ++n_band_only; continue; }
            if (bc.band != 0) continue;
            // A shared chord: its band-1 copy follows at once, with the same shape and block.
            ++n_shared;
            ASSERT_LT(i + 1, cyc.size());
            const BasisCycle& c2 = cyc[i + 1];
            EXPECT_EQ(c2.band, 1);
            EXPECT_EQ(c2.kind, bc.kind);
            EXPECT_EQ(c2.station, bc.station);
            EXPECT_EQ(c2.partner, bc.partner);
            EXPECT_EQ(c2.sat, bc.sat);
            EXPECT_EQ(c2.ref_sat, bc.ref_sat);
            EXPECT_EQ(c2.pivot_arc, bc.pivot_arc);
            ASSERT_EQ(c2.cycle.terms.size(), bc.cycle.terms.size());
            for (std::size_t t = 0; t < bc.cycle.terms.size(); ++t) {
                EXPECT_EQ(c2.cycle.terms[t].arc.station, bc.cycle.terms[t].arc.station);
                EXPECT_EQ(c2.cycle.terms[t].arc.sat, bc.cycle.terms[t].arc.sat);
                EXPECT_EQ(c2.cycle.terms[t].coeff, bc.cycle.terms[t].coeff);
                EXPECT_EQ(c2.cycle.terms[t].arc.signal, sig("L2W"));
            }
            EXPECT_EQ(netar::searchBlockKey("G", staName(bc.station), staName(bc.partner), true),
                      netar::searchBlockKey("G", staName(c2.station), staName(c2.partner), true));
        }
    }
    // The population is not degenerate.
    EXPECT_GT(n_shared, 1000);
    EXPECT_GT(n_band_only, 50);
    EXPECT_GT(n_borrowed, 50);
    EXPECT_GT(n_multi, 30);
    (void)n_fund;
}

TEST(ProducerArCycleBasis, TheSearchBlockIsTheStationUnderTheCycleBasis) {
    EXPECT_EQ(netar::searchBlockKey("G", "S1", "S2", false), "G|S1|S2");   // card ARL: per baseline
    EXPECT_EQ(netar::searchBlockKey("G", "S1", "S2", true), "G|S1");       // card AGT: per station
    EXPECT_EQ(netar::searchBlockKey("G", "S1", "S3", true), netar::searchBlockKey("G", "S1", "S2", true));
}

// ------------------------------------------------------------------ the slipped reference

TEST(ProducerArCycleBasis, ASlippedLinkReferenceReseatsAndItsPartnerDdsMap) {
    // Four stations and six satellites, all tracked on both bands: root S0; S1, S2, S3 link to
    // S0 through G01 (equal durability: the first common satellite).
    PairBed bed = completeBed(4, 6);
    const ArCycleBuild first = arCycleBuild(bed.input(), {});
    ASSERT_EQ(first.forest.links.size(), 4u);
    const arcgraph::StationLink& l1 = first.forest.links[1];
    ASSERT_EQ(first.graph.stations[static_cast<std::size_t>(l1.station)], StationId{1});
    ASSERT_EQ(first.graph.stations[static_cast<std::size_t>(l1.parent)], StationId{0});
    ASSERT_EQ(first.graph.sats[static_cast<std::size_t>(l1.ref_sat)], gps(1));
    expectFullCoverage(first, 2);

    // Next search: (S1, G01) slipped on both bands.
    bed.slipped[1][0]    = 3;
    bed.durability[1][0] = 0.0;
    const ArCycleBuild b = arCycleBuild(bed.input(), arcgraph::incumbentLinksOf(first.graph, first.forest));
    expectFullCoverage(b, 2);
    EXPECT_EQ(b.forest.reseats[static_cast<std::size_t>(arcgraph::Reseat::Slipped)], 1);
    int kept = 0;
    for (const arcgraph::StationLink& L : b.forest.links) {
        if (L.parent < 0) continue;
        if (L.from_incumbent) { ++kept; continue; }
        // The re-seated link: S1 again, now through another reference.
        EXPECT_EQ(b.graph.stations[static_cast<std::size_t>(L.station)], StationId{1});
        EXPECT_NE(b.graph.sats[static_cast<std::size_t>(L.ref_sat)], gps(1));
    }
    EXPECT_EQ(kept, 2);   // S2 and S3 keep theirs
    const ArcId slipped_l1{StationId{1}, gps(1), sig("L1C")}, slipped_l2{StationId{1}, gps(1), sig("L2W")};
    EXPECT_EQ(b.invalidated, (std::set<ArcId>{slipped_l1, slipped_l2}));

    const std::vector<ArMapState> st = arCycleMapStates(first.basis, b.basis, b.invalidated);
    const std::vector<std::optional<arcgraph::Coordinates>> W =
        arcgraph::basisChangeMap(first.basis, b.basis, b.invalidated);
    int s1_mapped = 0, fresh_life = 0;
    for (std::size_t i = 0; i < b.basis.cycles.size(); ++i) {
        const BasisCycle& bc = b.basis.cycles[i];
        bool through_slip = false;
        for (const auto& t : bc.cycle.terms) through_slip |= (t.arc == slipped_l1 || t.arc == slipped_l2);
        if (through_slip) {
            EXPECT_EQ(st[i], ArMapState::FreshLife);
            EXPECT_FALSE(W[i].has_value());
            ++fresh_life;
            continue;
        }
        EXPECT_NE(st[i], ArMapState::FreshNew);
        EXPECT_NE(st[i], ArMapState::FreshLife);
        ASSERT_TRUE(W[i].has_value());
        if (bc.station == StationId{1}) {
            // Re-referenced: S1's DDs against its new reference map by an integer combination.
            EXPECT_EQ(st[i], ArMapState::Mapped);
            ++s1_mapped;
        } else {
            EXPECT_EQ(st[i], ArMapState::Same);
        }
    }
    EXPECT_EQ(fresh_life, 2);   // the chord (S1, G01), one copy per band
    EXPECT_EQ(s1_mapped, 8);    // S1's four other partner DDs, two bands
}

// ------------------------------------------------------------------ stickiness

TEST(ProducerArCycleBasis, AnUnchangedGraphKeepsEveryLinkAndEveryCycle) {
    std::mt19937 rng(5);
    for (int trial = 0; trial < 100; ++trial) {
        PairBed bed = randomBed(rng);
        for (auto& row : bed.slipped) std::fill(row.begin(), row.end(), std::uint8_t{0});
        const ArCycleBuild first = arCycleBuild(bed.input(), {});
        const ArCycleBuild b =
            arCycleBuild(bed.input(), arcgraph::incumbentLinksOf(first.graph, first.forest), &first.basis);
        for (const arcgraph::StationLink& L : b.forest.links)
            if (L.parent >= 0) EXPECT_TRUE(L.from_incumbent);
        for (int r : b.forest.reseats) EXPECT_EQ(r, 0);
        for (ArMapState s : arCycleMapStates(first.basis, b.basis, {})) EXPECT_EQ(s, ArMapState::Same);
    }
}

TEST(ProducerArCycleBasis, StickyBorrowedClosuresChurnLessAsArcsComeAndGo) {
    // A chain of searches on a bed whose arcs rise and set a few at a time.  With the last basis
    // passed in, a chord keeps its borrowed partner and reference while they still close it; a
    // basis built without it re-picks the station sharing the most satellites each time.
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    long long changed_sticky = 0, changed_fresh = 0, borrowed = 0;
    const auto closures = [](const ArCycleBuild& b) {
        std::map<std::tuple<int, StationId, SatId>, std::pair<StationId, SatId>> m;
        for (const BasisCycle& bc : b.basis.cycles)
            if (bc.kind == CycleKind::BorrowedDd) m[{bc.band, bc.station, bc.sat}] = {bc.partner, bc.ref_sat};
        return m;
    };
    for (int trial = 0; trial < 60; ++trial) {
        PairBed bed = randomBed(rng);
        for (auto& row : bed.slipped) std::fill(row.begin(), row.end(), std::uint8_t{0});
        ArCycleBuild last_sticky = arCycleBuild(bed.input(), {});
        ArCycleBuild last_fresh  = last_sticky;
        for (int step = 0; step < 6; ++step) {
            // One or two arcs set (removed) and one rises (added) somewhere.
            for (auto& row : bed.present)
                for (auto& bits : row)
                    if (bits != 0 && u(rng) < 0.03) bits = 0;
                    else if (bits == 0 && u(rng) < 0.01) bits = 3;
            const ArCycleBuild sticky = arCycleBuild(
                bed.input(), arcgraph::incumbentLinksOf(last_sticky.graph, last_sticky.forest), &last_sticky.basis);
            const ArCycleBuild fresh = arCycleBuild(
                bed.input(), arcgraph::incumbentLinksOf(last_fresh.graph, last_fresh.forest));
            expectFullCoverage(sticky, 2);
            expectFullCoverage(fresh, 2);
            const auto count_changed = [&](const ArCycleBuild& before, const ArCycleBuild& now) {
                long long n = 0;
                const auto a = closures(before), b = closures(now);
                for (const auto& [chord, ek] : b) {
                    const auto it = a.find(chord);
                    if (it != a.end() && it->second != ek) ++n;
                }
                return n;
            };
            changed_sticky += count_changed(last_sticky, sticky);
            changed_fresh  += count_changed(last_fresh, fresh);
            borrowed       += static_cast<long long>(closures(sticky).size());
            last_sticky = sticky;
            last_fresh  = fresh;
        }
    }
    EXPECT_GT(borrowed, 200);
    EXPECT_GT(changed_fresh, 0);
    EXPECT_LT(changed_sticky, changed_fresh);
}

TEST(ProducerArCycleBasis, AStickyForestKeepsTheLinksAFreshForestWouldMove) {
    // Search 1: complete graph, root S0.  Search 2: S0 loses G05 and G06, so a fresh forest
    // roots at S1 (the most arcs); the sticky one keeps S0 and every link (each still shares at
    // least two satellites with its parent through its reference G01).
    PairBed bed = completeBed(4, 6);
    const ArCycleBuild first = arCycleBuild(bed.input(), {});
    bed.present[0][4] = 0;
    bed.present[0][5] = 0;
    const ArCycleBuild sticky = arCycleBuild(bed.input(), arcgraph::incumbentLinksOf(first.graph, first.forest));
    const ArCycleBuild fresh  = arCycleBuild(bed.input(), {});
    expectFullCoverage(sticky, 2);
    expectFullCoverage(fresh, 2);
    int kept = 0;
    for (const arcgraph::StationLink& L : sticky.forest.links)
        if (L.parent >= 0 && L.from_incumbent) ++kept;
    EXPECT_EQ(kept, 3);
    EXPECT_EQ(sticky.graph.stations[static_cast<std::size_t>(sticky.forest.links[0].station)], StationId{0});
    EXPECT_EQ(fresh.graph.stations[static_cast<std::size_t>(fresh.forest.links[0].station)], StationId{1});
    const auto count_same = [&](const ArCycleBuild& b) {
        int n = 0;
        for (ArMapState s : arCycleMapStates(first.basis, b.basis, {})) n += (s == ArMapState::Same);
        return n;
    };
    EXPECT_GT(count_same(sticky), count_same(fresh));
    // No cycle of either leaves the stored span: the lost arcs are in no new cycle.
    for (ArMapState s : arCycleMapStates(first.basis, sticky.basis, {})) EXPECT_NE(s, ArMapState::FreshNew);
}

// ------------------------------------------------------------------ the map and the names

TEST(ProducerArCycleBasis, MapStatesNameTheirCause) {
    PairBed bed = completeBed(3, 5);
    const ArCycleBuild b = arCycleBuild(bed.input(), {});
    for (ArMapState s : arCycleMapStates(arcgraph::CycleBasis{}, b.basis, {}))
        EXPECT_EQ(s, ArMapState::FreshNew);   // no stored basis
    const ArcId hit{StationId{2}, gps(3), sig("L2W")};
    const std::vector<ArMapState> st = arCycleMapStates(b.basis, b.basis, {hit});
    int n_life = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
        bool touches = false;
        for (const auto& t : b.basis.cycles[i].cycle.terms) touches |= (t.arc == hit);
        EXPECT_EQ(st[i], touches ? ArMapState::FreshLife : ArMapState::Same);
        n_life += touches;
    }
    EXPECT_GT(n_life, 0);
    EXPECT_STREQ(arMapStateName(ArMapState::Same), "same");
    EXPECT_STREQ(arMapStateName(ArMapState::Mapped), "mapped");
    EXPECT_STREQ(arMapStateName(ArMapState::FreshLife), "fresh_life");
    EXPECT_STREQ(arMapStateName(ArMapState::FreshNew), "fresh_new");
    EXPECT_STREQ(arCycleKindName(CycleKind::PartnerDd), "partner");
    EXPECT_STREQ(arCycleKindName(CycleKind::BorrowedDd), "borrowed");
}

TEST(ProducerArCycleBasis, TheTallyCountsCoverageLinksAndTheMap) {
    PairBed bed = completeBed(4, 6);
    bed.present[3][5] = 1;   // one L1-only arc
    const ArFamilySpec fam{System::GPS, {"L1C", "L2W"}, {sig("L1C"), sig("L2W")}};
    const ArCycleBuild first = arCycleBuild(bed.input(), {});
    const std::vector<arcgraph::IncumbentLink> inc = arcgraph::incumbentLinksOf(first.graph, first.forest);
    const ArCycleBuild b = arCycleBuild(bed.input(), inc);
    ArCycleBasisCounters c;
    arCycleTally(c, fam, first, arCycleMapStates({}, first.basis, first.invalidated), {}, true);
    arCycleTally(c, fam, b, arCycleMapStates(first.basis, b.basis, b.invalidated), inc, false);
    EXPECT_EQ(c.families, 2);
    EXPECT_EQ(c.first_builds, 1);
    EXPECT_EQ(c.dim_mismatch, 0);
    EXPECT_EQ(c.lattice_dim.at("G L1C").size(), 2u);
    EXPECT_EQ(c.lattice_dim.at("G L1C")[1], static_cast<double>(b.basis.lattice_dim[0]));
    EXPECT_EQ(c.links, 6);
    EXPECT_EQ(c.links_kept, 3);
    EXPECT_EQ(c.arcs_band_only, 2);
    EXPECT_EQ(c.rows_band_only, 2);   // the L1-only chord, in each search
    const long long n_rows = c.rows_partner + c.rows_borrowed;
    EXPECT_EQ(c.map_same + c.map_mapped + c.map_fresh_life + c.map_fresh_new, n_rows);
    EXPECT_EQ(c.map_same, n_rows / 2);   // the second search keeps every cycle
}

}  // namespace
}  // namespace GnssProcessor
