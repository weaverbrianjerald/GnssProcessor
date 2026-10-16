// Frozen oracles for filter/NetworkArcGraph (AGT-1 to AGT-4): suite NetworkArcGraphOracle.
//
// The module replaces five pieces of legacy graph code.  Before any caller moves (AGT-2 to
// AGT-4), each replacement is proven decision-identical against a frozen verbatim copy of the
// code it replaces, taken from main b5592b6 (the BEGIN FROZEN / END FROZEN blocks below; every
// block is the named line range with only the line endings normalized):
//
//   - the eq34 chain walk, ChainDatum.cpp 351 to 806 (the string union-find, the r1
//     election and the two-queue, two-pass walk), with the lines it reads from earlier in the
//     same function (arcSlipped 135-145, cov 151-154, the vertex names 267-271, arcLife 273-301,
//     the incumbent seed 303-349);
//   - the ARL Prim, ProducerAr.cpp 386 to 478 (the ar_pool loop head and the tree_basis
//     branch) with the DDrow struct 379 to 385, ArRec 323 and reqOf 362 to 370;
//   - the Card-4 driver's UnionFind (InvariantCarryDriver.cpp 73 to 90) and its pair-group
//     tree completion (674 to 689);
//   - added at AGT-3, when those owners move onto the shared Dsu: the datum-graph census's
//     union-by-size UnionFind and its component reduction (DatumGraph.cpp 54 to 85 and
//     166 to 229, the whole of netDatumGraphComponents), and the emit re-base's string union-find
//     and component ids (EmitStore.cpp 197 to 240).  Their live replacements are called
//     directly: netDatumGraphComponents and networkEmitRebaseComponents;
//   - added at AGT-4, when the chain datum moves onto the module: the whole of the election,
//     networkElectChainDatum (ChainDatum.cpp 99 to 1057), which compiles against the real
//     run context.  The frozen and the live election run side by side over random multi-epoch
//     lineages, and every output is compared after every epoch (the pins, the incumbent maps, the
//     best datum receiver, the re-references, the carry erasures and shifts, the counters).
//
// The live ZComponent (ZStore.hpp) is the oracle of singleTreeOf and fundamentalCycle.
//
// The AGT-1 to AGT-3 frozen blocks compile against stand-ins for the run context (FakeCtx,
// ChainStation, FakeHygiene).  The stand-ins also observe the walk without editing it: a shared tape records,
// in order, each new_q pop (the operator[] reads of `arcs` and `satrx`), each eligibility test,
// each cold-fallback and slip-readmission count and each pin write (one per recordEdge call).  An
// edge is a new edge when the eligibility test just before it named the same arc; it is a pass-2
// edge when a vertex popped twice before it (pass 2 re-pushes the whole visited set).

#include "network/ar/ArcGraph.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "network/ar/ZStore.hpp"      // ZComponent, the live phase-B oracle
#include "network/ar/ArSuspect.hpp"   // ArSuspectCornerState, arSuspectCombine (the frozen DDrow)
#include "network/solve/DatumGraph.hpp"  // netDatumGraphComponents and its types (the census, AGT-3)
#include "filter/StateKeySchema.hpp"     // StateKeys (the frozen chain walk and emit re-base)
#include "network/emit/EmitRebase.hpp" // networkEmitRebaseComponents (the emit re-base, AGT-3)
#include "common/GnssIdentity.hpp"
// The chain election end to end (AGT-4): the real run context, config, census and hygiene.
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/AnchorHygiene.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/carry/Coupling.hpp"
#include "network/assembly/Responses.hpp"
#include "network/driver/RunTypes.hpp"
#include "logging/LogManager.hpp"

#include <gtest/gtest.h>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using namespace arcgraph;

// =====================================================================================
// The Card-4 driver's union-find and pair completion (frozen).
// =====================================================================================

// ---- BEGIN FROZEN src/core/filter/NetworkInvariantCarryDriver.cpp lines 73-90 @ b5592b6 (verbatim) ----
/// A tiny path-compressing union-find over the (small, per-group) integer vertex set.
class UnionFind {
public:
    int find(int x) {
        auto it = parent_.find(x);
        if (it == parent_.end()) { parent_[x] = x; return x; }
        int r = x;
        while (parent_[r] != r) r = parent_[r];
        while (parent_[x] != r) { int nx = parent_[x]; parent_[x] = r; x = nx; }
        return r;
    }
    void unite(int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb) parent_[ra] = rb;
    }
private:
    std::unordered_map<int, int> parent_;
};
// ---- END FROZEN ----

/// The harness of the frozen pair completion: the group is a pair group and the counter is local.
struct FrozenPairGroup { bool pair{true}; };
struct FrozenDriverCounters { long long pair_tree_promotions{0}; };

/// Run the frozen pair completion on one component's tree edges and chords.
long long frozenPairCompletion(std::vector<ArcKey>& tree_edges, std::vector<ArcKey>& chords) {
    const FrozenPairGroup g;
    FrozenDriverCounters counters_;
// ---- BEGIN FROZEN src/core/filter/NetworkInvariantCarryDriver.cpp lines 674-689 @ b5592b6 (verbatim) ----
            if (g.pair && !chords.empty()) {
                UnionFind tf;
                for (const ArcKey& t : tree_edges) tf.unite(t.u, t.v);
                std::vector<ArcKey> kept;
                kept.reserve(chords.size());
                for (const ArcKey& c : chords) {
                    if (tf.find(c.u) != tf.find(c.v)) {
                        tf.unite(c.u, c.v);
                        tree_edges.push_back(c);   // promoted: completes the forest
                        ++counters_.pair_tree_promotions;
                    } else {
                        kept.push_back(c);
                    }
                }
                chords.swap(kept);
            }
// ---- END FROZEN ----
    return counters_.pair_tree_promotions;
}

/// The roots the chain datum's string union-find gives after a union sequence.
std::map<std::string, std::string> frozenStringUfRoots(
    const std::vector<std::pair<std::string, std::string>>& seq, const std::vector<std::string>& vertices) {
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 352-363 @ b5592b6 (verbatim) ----
        std::map<std::string, std::string> uf;
        auto find = [&](std::string v) -> std::string {
            auto it = uf.find(v);
            while (it != uf.end() && it->second != v) { v = it->second; it = uf.find(v); }
            return v;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            if (!uf.count(a)) uf[a] = a;
            if (!uf.count(b)) uf[b] = b;
            const std::string ra = find(a), rb = find(b);
            if (ra != rb) uf[ra] = rb;
        };
// ---- END FROZEN ----
    for (const auto& [a, b] : seq) unite(a, b);
    std::map<std::string, std::string> roots;
    for (const auto& v : vertices) roots[v] = find(v);
    return roots;
}

// =====================================================================================
// The datum-graph census and the emit re-base (frozen at AGT-3).
// =====================================================================================

/// The frozen census (union by size, the reduction to components and their sort), in a namespace
/// of its own so that its UnionFind and its netDatumGraphComponents meet neither the driver's
/// frozen UnionFind above nor the live function.
namespace frozen_census {
// ---- BEGIN FROZEN src/core/filter/NetworkDatumGraph.cpp lines 54-85 @ b5592b6 (verbatim) ----
class UnionFind {
public:
    int make() {
        parent_.push_back(static_cast<int>(parent_.size()));
        size_.push_back(1);
        return static_cast<int>(parent_.size()) - 1;
    }
    int find(int v) {
        while (parent_[static_cast<std::size_t>(v)] != v) {
            // Path halving: same amortized bound as full compression, one pass,
            // no recursion (the graph can carry a few thousand vertices at 305
            // stations and this runs every epoch).
            parent_[static_cast<std::size_t>(v)] =
                parent_[static_cast<std::size_t>(parent_[static_cast<std::size_t>(v)])];
            v = parent_[static_cast<std::size_t>(v)];
        }
        return v;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (size_[static_cast<std::size_t>(a)] < size_[static_cast<std::size_t>(b)])
            std::swap(a, b);
        parent_[static_cast<std::size_t>(b)] = a;
        size_[static_cast<std::size_t>(a)] += size_[static_cast<std::size_t>(b)];
    }

private:
    std::vector<int> parent_;
    std::vector<int> size_;
};
// ---- END FROZEN ----

// ---- BEGIN FROZEN src/core/filter/NetworkDatumGraph.cpp lines 166-229 @ b5592b6 (verbatim) ----
std::map<char, NetDatumGraphConsCensus>
netDatumGraphComponents(const std::vector<NetDatumGraphEdge>& edges) {
    // Per constellation: vertex ids for the stations and the satellites that
    // carry an edge.  A vertex with no edge belongs to no component; counting an
    // untracked satellite or a station that folded nothing would inflate k with
    // vertices that carry no datum.
    struct Build {
        UnionFind                  uf;
        std::map<int, int>         sta_v;   // station index  -> vertex
        std::map<std::string, int> sat_v;   // satellite id   -> vertex
        int                        n_edges{0};
    };
    std::map<char, Build> build;

    for (const auto& e : edges) {
        if (e.sat.empty() || e.station < 0) continue;
        Build& b = build[letterOfSat(e.sat)];
        auto its = b.sta_v.find(e.station);
        if (its == b.sta_v.end()) its = b.sta_v.emplace(e.station, b.uf.make()).first;
        auto itv = b.sat_v.find(e.sat);
        if (itv == b.sat_v.end()) itv = b.sat_v.emplace(e.sat, b.uf.make()).first;
        b.uf.unite(its->second, itv->second);
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
        // sample identities are deterministic without a second pass.
        std::map<int, NetDatumGraphComponent> comp;
        for (const auto& [sta, v] : b.sta_v) {
            NetDatumGraphComponent& c = comp[b.uf.find(v)];
            ++c.n_stations;
            if (c.sample_station < 0) c.sample_station = sta;
        }
        for (const auto& [sat, v] : b.sat_v) {
            NetDatumGraphComponent& c = comp[b.uf.find(v)];
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
// ---- END FROZEN ----
}  // namespace frozen_census

/// The ambiguity map the frozen emit re-base walks (the type of StateManager::getAmbiguities).
struct FrozenEmitSm {
    std::map<std::string, size_t> ambs;
    const std::map<std::string, size_t>& getAmbiguities() const { return ambs; }
};
/// A station as the frozen emit re-base reads it (`stations[i].sm->getAmbiguities()`).
struct FrozenEmitStation { std::shared_ptr<FrozenEmitSm> sm; };
/// The inputs step (b) of the emit re-base reads in one epoch.
struct EmitRebaseEpoch {
    std::vector<FrozenEmitStation> stations;
    std::vector<bool>              present;
    std::vector<int>               est_index_of;
    std::set<std::string>          shared_phase_keys;
};

/// Run the frozen step (b) of the emit re-base on one epoch: "sat|signal" -> component id.
std::map<std::string, int> frozenEmitRebaseComponents(const EmitRebaseEpoch& ep) {
    const std::vector<FrozenEmitStation>& stations = ep.stations;
    const std::vector<bool>&              present = ep.present;
    const std::vector<int>&               est_index_of = ep.est_index_of;
    const std::set<std::string>&          shared_phase_keys = ep.shared_phase_keys;
// ---- BEGIN FROZEN src/core/filter/NetworkEmitStore.cpp lines 197-240 @ b5592b6 (verbatim) ----
        // (b) Connected-component partition of the (receiver,satellite) bipartite
        //     graph per (const,signal): the common fractional δ is shared only within
        //     a component (each elects its own datum receiver r1).  Union-find over
        //     this epoch's shared arcs, the same walk buildCarryDdPriors does.
        std::map<std::pair<char, std::string>, std::map<std::string, std::string>> dsu;
        auto findRoot = [](const std::map<std::string, std::string>& p,
                           std::string v) -> std::string {
            auto it = p.find(v);
            while (it != p.end() && it->second != v) { v = it->second; it = p.find(v); }
            return v;
        };
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || est_index_of[i] < 0) continue;
            for (const auto& [amb, sidx] : stations[i].sm->getAmbiguities()) {
                (void)sidx;
                const auto parts = StateKeys::parseAmbiguityKey(amb);
                if (!parts || parts->satellite.empty()
                    || systemOfSat(parts->satellite) == System::GLONASS)
                    continue;   // GLONASS FDMA excluded (no common-λ integer DD)
                if (!shared_phase_keys.count(
                        StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal)))
                    continue;   // not an eq34 graph arc (gated/legacy lump)
                auto& p = dsu[{letterOfSat(parts->satellite), parts->signal}];
                const std::string rv = "R" + std::to_string(i);
                const std::string sv = "S" + parts->satellite;
                if (!p.count(rv)) p[rv] = rv;
                if (!p.count(sv)) p[sv] = sv;
                p[findRoot(p, rv)] = findRoot(p, sv);   // union
            }
        }
        // (sat,signal) -> global component id (1-based; 0 = unassigned sentinel).
        std::map<std::string, int> comp_of;
        int next_comp = 0;
        for (auto& [g, p] : dsu) {
            std::map<std::string, int> root2id;
            for (const auto& [v, par] : p) {
                (void)par;
                if (v.empty() || v[0] != 'S') continue;   // satellite vertices only
                const std::string root = findRoot(p, v);
                int& id = root2id[root];
                if (id == 0) id = ++next_comp;
                comp_of[v.substr(1) + "|" + g.second] = id;
            }
        }
// ---- END FROZEN ----
    return comp_of;
}

/// The live step (b) up to its call: the same walk and filters as the frozen block (unchanged in
/// EmitStore.cpp at AGT-3), gathering the arcs that networkEmitRebaseComponents partitions.
EmitRebaseArcs liveEmitRebaseArcs(const EmitRebaseEpoch& ep) {
    EmitRebaseArcs arcs;
    for (std::size_t i = 0; i < ep.stations.size(); ++i) {
        if (!ep.present[i] || ep.est_index_of[i] < 0) continue;
        for (const auto& [amb, sidx] : ep.stations[i].sm->getAmbiguities()) {
            (void)sidx;
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            if (!parts || parts->satellite.empty() || systemOfSat(parts->satellite) == System::GLONASS) continue;
            if (!ep.shared_phase_keys.count(StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal)))
                continue;
            arcs[{letterOfSat(parts->satellite), parts->signal}].emplace_back(static_cast<int>(i), parts->satellite);
        }
    }
    return arcs;
}

// =====================================================================================
// The ARL Prim (frozen).
// =====================================================================================

/// The stations the frozen Prim names.
struct FrozenArStation { std::string name; };
/// The ambiguity-resolution config the frozen Prim reads.
struct FrozenNetar { bool tree_basis{true}; };
/// One frozen DD row, as the trace would print its identity.
struct FrozenDd {
    std::string sta_r, ref_sta, sat_j, ref_sat;
    double      dd_m{0.0};
};

/// Run the frozen Prim on one (constellation, signal): @p vis is satellite -> station index ->
/// absolute float ambiguity.
std::vector<FrozenDd> frozenPrim(const std::map<std::string, std::map<std::size_t, double>>& vis,
                                 const std::vector<FrozenArStation>& stations, int* n_const_signal_out) {
    const FrozenNetar netar;
// ---- BEGIN FROZEN src/core/filter/NetworkProducerAr.cpp lines 323-323 @ b5592b6 (verbatim) ----
        struct ArRec { double value_m; std::size_t est_id; Eigen::Index col; ArSuspectCornerState suspect; };
// ---- END FROZEN ----
    std::map<std::pair<std::string, std::string>,
             std::map<std::string, std::map<std::size_t, ArRec>>> ar_pool;
    {
        Eigen::Index col = 0;
        for (const auto& [sat, by_sta] : vis)
            for (const auto& [sta, value] : by_sta)
                ar_pool[{"G", "L1C"}][sat][sta] = ArRec{value, sta, col++, ArSuspectCornerState{}};
    }
// ---- BEGIN FROZEN src/core/filter/NetworkProducerAr.cpp lines 362-370 @ b5592b6 (verbatim) ----
        std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
        std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
        auto reqOf = [&](const ArRec& r) -> int {
            const std::pair<std::size_t, Eigen::Index> k{r.est_id, r.col};
            auto it = req_idx.find(k);
            if (it != req_idx.end()) return it->second;
            const int idx = static_cast<int>(reqs.size());
            reqs.push_back(k); req_idx[k] = idx; return idx;
        };
// ---- END FROZEN ----
// ---- BEGIN FROZEN src/core/filter/NetworkProducerAr.cpp lines 379-478 @ b5592b6 (verbatim) ----
        struct DDrow {
            int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda;
            std::string cons, signal, sat_j, ref_sat, sta_r, ref_sta;
            ArSuspectCornerState suspect{};
        };
        std::vector<DDrow> dds;
        int n_const_signal = 0;
        for (auto& [cs, satmap] : ar_pool) {
            if (netar.tree_basis) {
                // Card ARL: a greedy spanning tree of station baselines.  Each station is
                // attached to the in-tree station it shares the most satellites with; the
                // baseline contributes one DD per common satellite against the first common
                // one.  Every baseline brings in a new station's ambiguities, so the set is
                // independent by construction.
                const std::optional<SignalId> tsig =
                    SignalId::fromObsCode(systemFromName(cs.first), cs.second);
                const double t_lambda = tsig ? tsig->wavelengthM(0) : 0.0;
                if (!(std::isfinite(t_lambda) && t_lambda > 0.0)) continue;
                // Prim's algorithm on satellite bitmasks: every outside station keeps its
                // best in-tree partner (most common satellites, then the lowest station
                // index), updated once per attachment, so a tree costs O(stations^2)
                // popcounts.  Ties resolve as an exhaustive scan in (station, partner)
                // order would.
                std::vector<std::string> sat_names;                  // lexical (satmap order)
                for (auto& [sat, sm] : satmap) { (void)sm; sat_names.push_back(sat); }
                const std::size_t n_sat = sat_names.size();
                const std::size_t n_words = (n_sat + 63) / 64;
                std::vector<std::size_t> sta_ids;                    // ascending station index
                std::map<std::size_t, std::size_t> pos_of_sta;
                for (auto& [sat, sm] : satmap)
                    for (auto& [sta_i, rec] : sm) { (void)rec; pos_of_sta.emplace(sta_i, 0); }
                for (auto& [sta_i, pp] : pos_of_sta) { pp = sta_ids.size(); sta_ids.push_back(sta_i); }
                const std::size_t n_sta = sta_ids.size();
                if (n_sta < 2) continue;
                std::vector<std::uint64_t> mask(n_sta * n_words, 0);
                std::vector<const ArRec*> rec_of(n_sta * n_sat, nullptr);
                std::vector<std::size_t> n_seen(n_sta, 0);
                {
                    std::size_t k = 0;
                    for (auto& [sat, sm] : satmap) {
                        (void)sat;
                        for (auto& [sta_i, rec] : sm) {
                            const std::size_t r = pos_of_sta[sta_i];
                            mask[r * n_words + k / 64] |= (std::uint64_t{1} << (k % 64));
                            rec_of[r * n_sat + k] = &rec;
                            ++n_seen[r];
                        }
                        ++k;
                    }
                }
                auto n_common = [&](std::size_t a, std::size_t b) {
                    std::size_t c = 0;
                    for (std::size_t w = 0; w < n_words; ++w)
                        c += std::bitset<64>(mask[a * n_words + w] & mask[b * n_words + w]).count();
                    return c;
                };
                std::size_t root = 0;
                for (std::size_t r = 1; r < n_sta; ++r)
                    if (n_seen[r] > n_seen[root]) root = r;
                std::vector<char> in_tree(n_sta, 0);
                std::vector<std::size_t> best_n(n_sta, 0), best_p(n_sta, root);
                in_tree[root] = 1;
                for (std::size_t r = 0; r < n_sta; ++r)
                    if (!in_tree[r]) best_n[r] = n_common(r, root);
                ++n_const_signal;
                for (std::size_t added = 1; added < n_sta; ++added) {
                    std::size_t r_pick = n_sta, n_pick = 0;
                    for (std::size_t r = 0; r < n_sta; ++r)
                        if (!in_tree[r] && best_n[r] > n_pick) { n_pick = best_n[r]; r_pick = r; }
                    if (r_pick == n_sta || n_pick < 2) break;   // the rest share fewer than two satellites
                    const std::size_t p_pick = best_p[r_pick];
                    in_tree[r_pick] = 1;
                    std::vector<std::size_t> c;                  // common satellites, lexical
                    for (std::size_t k = 0; k < n_sat; ++k)
                        if (rec_of[r_pick * n_sat + k] && rec_of[p_pick * n_sat + k]) c.push_back(k);
                    const std::size_t tref = c.front();
                    for (std::size_t q = 1; q < c.size(); ++q) {
                        const ArRec& rj = *rec_of[r_pick * n_sat + c[q]];
                        const ArRec& rp = *rec_of[r_pick * n_sat + tref];
                        const ArRec& fj = *rec_of[p_pick * n_sat + c[q]];
                        const ArRec& fp = *rec_of[p_pick * n_sat + tref];
                        dds.push_back(DDrow{ reqOf(rj), reqOf(rp), reqOf(fj), reqOf(fp),
                                             (rj.value_m - rp.value_m) - (fj.value_m - fp.value_m),
                                             t_lambda, cs.first, cs.second, sat_names[c[q]],
                                             sat_names[tref], stations[sta_ids[r_pick]].name,
                                             stations[sta_ids[p_pick]].name });
                        dds.back().suspect =
                            arSuspectCombine(rj.suspect, rp.suspect, fj.suspect, fp.suspect);
                    }
                    for (std::size_t s2 = 0; s2 < n_sta; ++s2) {
                        if (in_tree[s2]) continue;
                        const std::size_t cn = n_common(s2, r_pick);
                        if (cn > best_n[s2] || (cn == best_n[s2] && r_pick < best_p[s2])) {
                            best_n[s2] = cn;
                            best_p[s2] = r_pick;
                        }
                    }
                }
                continue;
            }
// ---- END FROZEN ----
    }   // HARNESS: closes the ar_pool loop (the star path, 479 onward, is not copied).
    std::vector<FrozenDd> out;
    for (const DDrow& d : dds) out.push_back(FrozenDd{d.sta_r, d.ref_sta, d.sat_j, d.ref_sat, d.dd_m});
    if (n_const_signal_out) *n_const_signal_out = n_const_signal;
    return out;
}

// =====================================================================================
// The chain walk (frozen) and its stand-ins.
// =====================================================================================

/// One observed event of the frozen walk (see the file banner).
struct TapeEvent {
    char        kind{'?'};  ///< 'P' pop, 'E' eligibility test, 'C' cold fallback, 'R' slip readmission, 'T' pin write, 'X' component end.
    std::size_t ri{0};      ///< Station index ('E', 'T').
    std::string key;        ///< Vertex name ('P') or ambiguity key ('E', 'T').
};
struct Tape { std::vector<TapeEvent> ev; };

/// A counter whose increments are taped.
struct TapeCounter {
    long long n{0};
    Tape*     tape{nullptr};
    char      kind{'?'};
    TapeCounter& operator++() {
        ++n;
        if (tape) tape->ev.push_back(TapeEvent{kind, 0, {}});
        return *this;
    }
};

/// A map whose operator[] reads (the walk's new_q pops) are taped.
template <class K, class V>
struct PopTapedMap : std::map<K, V> {
    Tape*       tape{nullptr};
    std::string (*vertex_name)(const K&) = nullptr;
    V& operator[](const K& k) {
        if (tape) tape->ev.push_back(TapeEvent{'P', 0, vertex_name(k)});
        return std::map<K, V>::operator[](k);
    }
};
std::string stationVertexName(const std::size_t& i) { return "R" + std::to_string(i); }
std::string satVertexName(const std::string& s) { return "S" + s; }

/// A pin-target map whose writes (one per recordEdge call) are taped.
struct PinTapedMap {
    std::map<std::string, double> m;
    Tape*       tape{nullptr};
    std::size_t ri{0};
    double& operator[](const std::string& amb) {
        if (tape) tape->ev.push_back(TapeEvent{'T', ri, amb});
        return m[amb];
    }
};

/// The station manager the frozen recordEdge reads under seed-round (never armed here).
struct FakeSm {
    Eigen::VectorXd x;
    int getAmbiguityIndex(const std::string&) const { return -1; }
    const Eigen::VectorXd& getStateVector() const { return x; }
};

/// The NetStation fields the frozen walk reads and writes.
struct ChainStation {
    std::string                   name;
    std::set<std::string>         pinned_rpb_keys;
    PinTapedMap                   pinned_amb_targets;
    std::map<std::string, double> pinned_amb_sigma_scale;
    std::map<std::string, double> pinned_amb_seed_lambda;
    std::shared_ptr<FakeSm>       sm = std::make_shared<FakeSm>();
};

/// AnchorHygiene's predicates, with the eligibility test taped.
struct FakeHygiene {
    bool                                       active{false};
    int                                        min_arc_ep{0};
    std::set<std::string>                      bridged;
    std::map<std::pair<int, std::string>, int> age;
    Tape*                                      tape{nullptr};

    int arcAge(std::size_t sta, const std::string& amb) const {
        const auto it = age.find({static_cast<int>(sta), amb});
        return it == age.end() ? 0 : it->second;
    }
    bool eligibleNoTape(std::size_t sta, const std::string& amb) const {
        return !active || min_arc_ep <= 0 || arcAge(sta, amb) >= min_arc_ep;
    }
    bool eligibleHost(std::size_t sta, const std::string& amb) const {
        if (tape) tape->ev.push_back(TapeEvent{'E', sta, amb});
        return eligibleNoTape(sta, amb);
    }
    bool isBridged(const std::string& amb) const { return active && bridged.count(amb) != 0; }
    double pinSigmaScale(std::size_t, const std::string&) const { return 1.0; }
};

/// The run-context counters and carry the frozen walk touches.
struct FakeHygCounters {
    long long   reattaches{0};
    long long   fresh_pin_ramps{0};
    TapeCounter cold_fallback_pins;
};
struct FakeCounters {
    TapeCounter     eq34_slip_readmitted;
    TapeCounter     eq34_slip_demoted;
    long long       eq34_slip_deseeded{0};
    long long       seed_round_frozen{0};
    long long       seed_round_pins{0};
    double          seed_round_max_abs_y_m{0.0};
    long long       seed_round_rehost_framed{0};
    double          seed_round_rehost_max_step_m{0.0};
    long long       seed_round_rehost_cold{0};
    long long       seed_round_target_steps{0};
    FakeHygCounters hyg;
};
struct FakeCarry {
    std::map<std::string, int> eq34_epoch_declared;
    std::map<std::string, int> eq34_epoch_bridged;
};
struct FakeCtx {
    FakeCounters counters;
    FakeCarry    carry;
};
struct FakeLocalCarry { double value_abs{0.0}; };

/// One random (constellation, signal) group of the chain datum.
struct ChainGroupInput {
    std::vector<std::string>                       names;          ///< Roster index -> station name.
    std::map<std::size_t, std::vector<std::string>> arcs;          ///< Station index -> satellites, in the group's push order.
    std::set<std::string>                          sats;           ///< The group's satellites.
    std::set<std::pair<int, std::string>>          slipped;        ///< carry_slipped_ambs.
    bool                                           slip_aware{false};
    bool                                           longevity{false};
    bool                                           extend_only{false};
    std::vector<std::map<std::string, double>>     sat_el;         ///< Per roster index.
    std::map<std::pair<size_t, std::string>, double> prev_el;
    std::map<char, std::map<std::size_t, int>>     cover;
    FakeHygiene                                    hyg;
    std::map<std::string, std::string>             anchor_incumbent;
    std::map<std::string, std::string>             rx_parent;
    std::map<std::string, std::string>             sat_root;
    std::map<std::string, std::string>             datum_rx;
    std::string                                    pref_rx;
    std::set<std::pair<int, std::string>>          retained;
};

/// One component of the frozen walk.
struct FrozenChainComponent {
    std::string                                      root;     ///< The union-find root's name.
    std::size_t                                      r1{0};    ///< The elected datum receiver.
    std::vector<std::pair<std::string, std::string>> edges;    ///< (child, parent) in attachment order.
    std::size_t                                      visited{0};
};
struct FrozenChainOut {
    std::vector<FrozenChainComponent> comps;
    std::string                       best_datum_rx;
    long long                         cold_fallback_pins{0};
    long long                         slip_readmitted{0};
    long long                         slip_deseeded{0};
};

/// Run the frozen chain walk on one group, taping its events.
FrozenChainOut frozenChainWalk(const ChainGroupInput& in, Tape& tape) {
    // HARNESS: the aliases and group locals the frozen blocks read, as networkElectChainDatum
    // declares them (the shift walk and everything after the two passes are not copied).
    FakeCtx ctx;
    ctx.counters.hyg.cold_fallback_pins.tape = &tape;
    ctx.counters.hyg.cold_fallback_pins.kind = 'C';
    ctx.counters.eq34_slip_readmitted.tape   = &tape;
    ctx.counters.eq34_slip_readmitted.kind   = 'R';
    FakeHygiene hyg_taped = in.hyg;
    hyg_taped.tape = &tape;
    const FakeHygiene& hyg = hyg_taped;
    const bool eq34_slip_aware  = in.slip_aware;
    const bool eq34_longevity   = in.longevity;
    const bool eq34_extend_only = in.extend_only;
    const bool eq34_round       = false;
    const bool eq34_seed_round  = false;
    const bool eq34_seed_rehost = false;
    const std::set<std::pair<int, std::string>>& carry_slipped_ambs = in.slipped;
    const std::set<std::pair<int, std::string>>& eq34_retained_edges = in.retained;
    long long eq34_gap_repin_total = 0;
    const std::map<std::string, std::string>& anchor_incumbent = in.anchor_incumbent;
    const std::map<std::string, std::string>& eq34_rx_parent   = in.rx_parent;
    const std::map<std::string, std::string>& eq34_sat_root    = in.sat_root;
    const std::map<std::string, std::string>& eq34_datum_rx    = in.datum_rx;
    const std::map<std::pair<size_t, std::string>, double>& eq34_prev_el = in.prev_el;
    std::map<std::tuple<int, std::string, std::string>, FakeLocalCarry> local_carry;
    std::map<std::pair<int, std::string>, double> eq34_prev_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::map<std::string, std::string> new_rx_parent, new_sat_root;
    std::map<std::pair<int, std::string>, double> new_prev_target;
    const std::string eq34_pref_rx = in.pref_rx;
    int eq34_n_pins = 0, eq34_gauge_dim = 0;
    std::vector<ChainStation> stations(in.names.size());
    for (std::size_t i = 0; i < in.names.size(); ++i) {
        stations[i].name                    = in.names[i];
        stations[i].pinned_amb_targets.tape = &tape;
        stations[i].pinned_amb_targets.ri   = i;
    }
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 135-145 @ b5592b6 (verbatim) ----
    // Slip-aware election (Card 3/4): true when this arc slipped this epoch.  The
    // funnel is complete here by the ordering contract (every PASS-A drain, signal
    // integrity and measurement admission run before the election).  The key is
    // (station, ambiguity), the same as the C3c membership test, so the election and
    // the DD-prior build agree on which arcs are slipped.
    auto arcSlipped = [&](std::size_t ri, const std::string& sat,
                          const std::string& sig) -> bool {
        return eq34_slip_aware
            && carry_slipped_ambs.count({static_cast<int>(ri),
                                         StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
    };
// ---- END FROZEN ----
    const auto& sat_el = in.sat_el;
    const auto& cover  = in.cover;
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 151-154 @ b5592b6 (verbatim) ----
    auto cov = [&](System sys, size_t i) -> int {
        auto it = cover.find(rinexLetter(sys)); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };
// ---- END FROZEN ----
    const System      grp_sys    = System::GPS;
    const std::string grp_sig    = "L1C";
    const bool        roundable  = false;
    const double      grp_lambda = 0.0;
    const std::string rpb_key    = "RPB|L1C";
    const std::string dr_key     = constellationKey(grp_sys) + "|" + grp_sig;
    const std::set<std::string>& sats = in.sats;
    PopTapedMap<std::size_t, std::vector<std::string>> arcs;
    PopTapedMap<std::string, std::vector<std::size_t>> satrx;
    for (const auto& [i, slist] : in.arcs)
        for (const auto& s : slist) {   // untaped fill, in the group's push order
            static_cast<std::map<std::size_t, std::vector<std::string>>&>(arcs)[i].push_back(s);
            static_cast<std::map<std::string, std::vector<std::size_t>>&>(satrx)[s].push_back(i);
        }
    arcs.tape         = &tape;
    arcs.vertex_name  = &stationVertexName;
    satrx.tape        = &tape;
    satrx.vertex_name = &satVertexName;
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 267-271 @ b5592b6 (verbatim) ----
        // Graph vertices are named "R<station index>" (a receiver) and "S<sat>" (a
        // satellite); the prefix is the vertex kind, not a constellation letter.
        auto rV = [](size_t i){ return "R" + std::to_string(i); };
        auto sV = [](const std::string& s){ return "S" + s; };
        auto isSatVertex = [](const std::string& v) { return v[0] == 'S'; };
// ---- END FROZEN ----
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 273-301 @ b5592b6 (verbatim) ----
        // Longevity score of arc (station ri, sat) (I4): higher means a longer
        // expected life and so a better new tree edge (fewer future deaths,
        // fewer b_s re-references).  A rising arc (elevation grew since last
        // epoch, tracked in eq34_prev_el) will not set soon, so it is lifted
        // above every setting arc (+90°, one quadrant); within a class the
        // higher elevation wins.  Low-elevation setting arcs cause the churn
        // and rank last.  Called only under eq34_longevity (sat_el and
        // eq34_prev_el are valid here).
        auto arcLife = [&](size_t ri, const std::string& sat) -> double {
            auto ei = sat_el[ri].find(sat);
            if (ei == sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
            const double el = ei->second;
            auto pe = eq34_prev_el.find({ri, sat});
            const bool rising = (pe != eq34_prev_el.end() && el > pe->second + 1e-6);
            // Slip-aware (Card 3/4): an arc that slipped this epoch is the worst new
            // tree edge at any elevation.  As a tree edge it invalidates every
            // fundamental cycle that closes through it, so the whole dependent chord set
            // refloats instead of the one DD the slip affects.  Demote it below every
            // clean arc (the clean band is [0,180]) and keep the ordering within the
            // slipped set, so that if the graph leaves no alternative the completion
            // picks the most durable slipped arc.  The counter increments on every
            // evaluation, including repeated comparator calls during a sort.
            const double life = el + (rising ? 90.0 : 0.0);
            if (arcSlipped(ri, sat, grp_sig)) {
                ++ctx.counters.eq34_slip_demoted;
                return life - 1000.0;
            }
            return life;
        };
// ---- END FROZEN ----
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 303-349 @ b5592b6 (verbatim) ----
        // Sticky-tree seed (spec D2 step 3): the incumbent tree edges
        // (sat-side parents in anchor_incumbent, "sat_signal" → station;
        // receiver-side parents in eq34_rx_parent, "station|c|σ" → sat)
        // that are still live this epoch (station present in this group's
        // arcs and still tracking the satellite).  Both maps are replaced
        // wholesale each election, so together they hold last epoch's tree
        // with each edge in one map.  Any live subset of a tree is a
        // forest, so seeding it cannot close a cycle.
        std::set<std::pair<size_t, std::string>> arc_set;
        std::map<std::string, size_t> name2idx;
        for (const auto& [i, slist] : arcs) {
            name2idx[stations[i].name] = i;
            for (const auto& s : slist) arc_set.insert({i, s});
        }
        std::map<std::string, std::vector<std::string>> inc_adj;
        auto seedEdge = [&](const std::string& sta, const std::string& sat) {
            auto ni = name2idx.find(sta);
            if (ni == name2idx.end() || !arc_set.count({ni->second, sat})) return;
            // Slip-aware (Card 3/4): a slipped incumbent loses its sticky seed.  The
            // incumbent queue drains before the new-edge walk, so a seeded edge is in
            // effect unconditional, and an incumbent that slipped this epoch should
            // leave the tree: every chord whose fundamental cycle runs through it is
            // lost with it.  Dropping the seed does not discard the edge; it moves it to
            // the new-edge walk, where the first pass refuses it (and arcLife ranks it
            // last under longevity), so it is re-admitted only if the component has no
            // other route to the vertex.  The resulting mass refloat is then forced by
            // the topology.
            if (arcSlipped(ni->second, sat, grp_sig)) {
                ++ctx.counters.eq34_slip_deseeded;
                return;
            }
            inc_adj[rV(ni->second)].push_back(sV(sat));
            inc_adj[sV(sat)].push_back(rV(ni->second));
        };
        for (const auto& [amb, sta] : anchor_incumbent) {
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a || systemOfSat(a->satellite) != grp_sys || a->signal != grp_sig) continue;
            seedEdge(sta, a->satellite);
        }
        const std::string rx_suffix =
            "|" + constellationKey(grp_sys) + "|" + grp_sig;
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (rxkey.size() <= rx_suffix.size()
                || rxkey.compare(rxkey.size() - rx_suffix.size(),
                                 rx_suffix.size(), rx_suffix) != 0) continue;
            seedEdge(rxkey.substr(0, rxkey.size() - rx_suffix.size()), sat);
        }
// ---- END FROZEN ----
    FrozenChainOut out;
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 351-806 @ b5592b6 (verbatim) ----
        // --- Union-find over vertices "R<idx>" / "S<sat>". ---
        std::map<std::string, std::string> uf;
        auto find = [&](std::string v) -> std::string {
            auto it = uf.find(v);
            while (it != uf.end() && it->second != v) { v = it->second; it = uf.find(v); }
            return v;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            if (!uf.count(a)) uf[a] = a;
            if (!uf.count(b)) uf[b] = b;
            const std::string ra = find(a), rb = find(b);
            if (ra != rb) uf[ra] = rb;
        };
        for (const auto& [i, slist] : arcs)
            for (const auto& s : slist) unite(rV(i), sV(s));

        std::map<std::string, std::vector<size_t>>      comp_rx;
        std::map<std::string, std::vector<std::string>> comp_sat;
        for (const auto& [i, slist] : arcs)  comp_rx[find(rV(i))].push_back(i);
        for (const auto& s : sats)           comp_sat[find(sV(s))].push_back(s);

        std::string best_datum_rx; size_t best_comp = 0;
        for (auto& [root, rx_list] : comp_rx) {
            std::sort(rx_list.begin(), rx_list.end());   // determinism

            // Elect r1: config pref → per-component sticky (sat-root
            // vote) → group incumbent → highest coverage.
            size_t r1 = static_cast<size_t>(-1);
            if (!eq34_pref_rx.empty())
                for (size_t i : rx_list)
                    if (stations[i].name == eq34_pref_rx) { r1 = i; break; }
            // Per-component stickiness: the component's sats remember
            // last epoch's root (eq34_sat_root); re-elect the station most
            // of them remember, if it is still in this component.  The
            // group-level eq34_datum_rx slot below remembers only the
            // largest component's r1, so without this vote small fragments
            // would re-elect by coverage every epoch, each change a full
            // fragment re-level.  Vote ties break by station name.
            if (r1 == static_cast<size_t>(-1)) {
                std::map<std::string, int> root_votes;
                for (const auto& s : comp_sat[root]) {
                    auto rt = eq34_sat_root.find(StateKeys::makeAmbiguityKey(s, grp_sig));
                    if (rt != eq34_sat_root.end()) ++root_votes[rt->second];
                }
                int best_v = 0; std::string best_name;
                for (const auto& [nm, v] : root_votes)
                    if (v > best_v || (v == best_v && nm < best_name)) {
                        best_v = v; best_name = nm;
                    }
                if (best_v > 0)
                    for (size_t i : rx_list)
                        if (stations[i].name == best_name) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                auto it = eq34_datum_rx.find(dr_key);
                if (it != eq34_datum_rx.end())
                    for (size_t i : rx_list)
                        if (stations[i].name == it->second) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                int best = -1;
                for (size_t i : rx_list) {
                    const int cv = cov(grp_sys, i);
                    if (cv > best
                        || (cv == best && stations[i].name < stations[r1].name)) {
                        best = cv; r1 = i;
                    }
                }
            }
            if (r1 == static_cast<size_t>(-1)) continue;   // empty (unreachable)

            if (rx_list.size() > best_comp) {
                best_comp = rx_list.size();
                best_datum_rx = stations[r1].name;
            }

            // r1's RPB pin: the receiver half of the gauge (δ_{r1,σ}=0).
            stations[r1].pinned_rpb_keys.insert(rpb_key);
            ++eq34_n_pins;
            eq34_gauge_dim += static_cast<int>(rx_list.size()
                            + comp_sat[root].size());   // n_c + m_c
            ctx.carry.eq34_epoch_declared[dr_key] +=
                static_cast<int>(rx_list.size() + comp_sat[root].size());

            // Sticky spanning tree (two-queue BFS from r1).  The
            // incumbent queue drains completely before any new graph edge
            // is explored, so a surviving old subtree, even one detached
            // upstream, is re-attached through its own old edges (one new
            // edge per detached fragment) and is not re-wired vertex by
            // vertex: a tree-edge pin moves only when its arc died (or,
            // under slip-aware election, slipped).  One ambiguity pin per
            // tree edge; each attachment records (child, parent,
            // edge-changed) for the dirty walk below.
            //
            // recordEdge records one tree edge (r,s): pin ambiguity z^s_r
            // at station r.  Target = 0 on a first-sighting edge (unless
            // seed-round applies, below); on a re-pin of an already-lit
            // (sat,σ) (pin-to-round), the |·|-minimizing integer
            // λ·round(carried float/λ), so the emitted-bias frame step is
            // ≈ 0 cyc.  sat_is_child selects which endpoint the edge
            // attaches: the sat (parent edge → this_epoch_anchor) or the
            // receiver (parent edge → new_rx_parent).
            std::set<std::string> visited;
            std::deque<std::string> inc_q, new_q;
            // (child, parent, pinmove, known, frame_shiftable): pinmove =
            // this pin's exact displacement of its pinned float vs last
            // epoch's frame; known = false where the step is not computable
            // (re-pin without a carry; root handover, a common fractional
            // step that no integer transform can express);
            // frame_shiftable (INC-2) = this step came from the new host's
            // own state and may be S-transformed even with the global
            // carry-shift knob off (always false unless INC-2 is armed).
            std::vector<std::tuple<std::string, std::string, double, bool, bool>>
                order;
            auto recordEdge = [&](size_t ri, const std::string& sat,
                                  bool sat_is_child) {
                const std::string amb = StateKeys::makeAmbiguityKey(sat, grp_sig);
                // Slip-aware (Card 3/4): this arc is becoming a tree edge.  If it slipped
                // this epoch, the election could not avoid it (the component has no other
                // route to the vertex), and every cycle through it refloats.  Counted
                // separately so that "deseeded > 0" is not read as "no slipped arc ended
                // up in the tree".
                if (arcSlipped(ri, sat, grp_sig)) ++ctx.counters.eq34_slip_readmitted;
                // Gap re-pin metric (I2): this edge was retained across a gap
                // (its carry outlived the row absence) and is resuming now.
                // The incumbent seed made it a recognized edge, so the
                // pin-to-round below re-pins it to its pre-gap integer
                // (step ~0) instead of the first-sighting 0 a wholesale
                // replace would have used.  Counts these re-pins.
                if (eq34_extend_only
                    && eq34_retained_edges.count({static_cast<int>(ri), amb}))
                    ++eq34_gap_repin_total;
                // Pin target: 0 on first sighting; else λ·round(carry/λ)
                // at the (possibly new) edge station.  carry_val also
                // feeds the pinmove computation below.
                double target = 0.0;
                double carry_val = std::numeric_limits<double>::quiet_NaN();
                if (eq34_round && anchor_incumbent.count(amb)) {
                    auto lc = local_carry.find(std::make_tuple(
                        static_cast<int>(ri), sat, grp_sig));
                    if (lc != local_carry.end() && roundable) {
                        const double lam = grp_lambda;
                        if (std::isfinite(lam) && lam > 0.0) {
                            carry_val = lc->second.value_abs;
                            target = lam * std::round(carry_val / lam);
                        }
                    }
                }
                // Seed-round (docs/SPEC_eq34_seed_round.md) applies when the
                // carry-round above did not engage (first sighting; incumbent whose
                // carry is dead; or every epoch under carry_invariant, where
                // eq34_round is off by the Card-4 design).  The target would then be
                // the cold absolute 0, which puts the seeded phase−code offset into
                // the datum frame (km-scale on the synthetic fixture; SPEC §12.8).
                // Instead, round the value the state holds, so |pin innovation| ≤ λ/2
                // by construction.  When the state does not exist yet (cold start or
                // new arc; PASS B's pre-pass allocates it), defer the rounding to the
                // fold via pinned_amb_seed_lambda; the realized target flows back into
                // eq34_prev_target through the serial consume.  `known` is cleared
                // below for deferred pins.
                bool seed_deferred = false, seed_rounded = false;
                // The new host's pre-pin float (INC-2, SPEC §8.5).  Finite only when
                // the state exists and INC-2 is armed; on a re-host it is this pin's
                // exact frame-step baseline (the pin moves the float from here to
                // `target`, so Δb_s = −(target − seed_x) exactly).
                double seed_x = std::numeric_limits<double>::quiet_NaN();
                bool   frame_shiftable = false;
                auto ambStateValue = [&]() -> double {
                    const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                    if (sidx < 0) return std::numeric_limits<double>::quiet_NaN();
                    return stations[ri].sm->getStateVector()(sidx);
                };
                if (eq34_seed_round && !std::isfinite(carry_val) && roundable) {
                    // Freeze after first realization (INC-1, SPEC §8.5): a frame is
                    // realized once.  Reuse this (station, amb)'s previously realized
                    // target unchanged (on the same host: pinmove 0, no step, no
                    // reref) and round only on a first realization.  Re-rounding
                    // every epoch ratchets the invariant carry (§13.7).
                    auto ptf = eq34_prev_target.find({static_cast<int>(ri), amb});
                    if (ptf != eq34_prev_target.end()) {
                        target = ptf->second;
                        seed_rounded = true;
                        ++ctx.counters.seed_round_frozen;
                        // A frozen target can still land on a re-hosted edge (the
                        // entry survived through the I2 retention or the fold
                        // feedback), and that step is equally computable, so read the
                        // pre-pin float here too (INC-2).  Only when armed, so with
                        // INC-2 off the freeze branch keeps its single map lookup.
                        if (eq34_seed_rehost) seed_x = ambStateValue();
                    } else {
                    const double lam = grp_lambda;
                    if (std::isfinite(lam) && lam > 0.0) {
                        const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                        if (sidx >= 0) {
                            const double x =
                                stations[ri].sm->getStateVector()(sidx);
                            if (std::isfinite(x)) {
                                if (eq34_seed_rehost) seed_x = x;   // re-host step baseline (INC-2)
                                target = lam * std::round(x / lam);
                                seed_rounded = true;
                                ++ctx.counters.seed_round_pins;
                                const double y = std::abs(target - x);
                                if (y > ctx.counters.seed_round_max_abs_y_m)
                                    ctx.counters.seed_round_max_abs_y_m = y;
                            }
                        } else {
                            stations[ri].pinned_amb_seed_lambda[amb] = lam;
                            seed_deferred = true;
                        }
                    }
                    }   // end of the freeze else-arm (INC-1)
                }
                double pinmove = 0.0;
                bool known = true;
                bool edge_changed = false;
                if (sat_is_child) {
                    auto ai = anchor_incumbent.find(amb);
                    edge_changed = (ai != anchor_incumbent.end()
                                    && ai->second != stations[ri].name);
                    this_epoch_anchor[amb] = stations[ri].name;
                    // Root handover: the component frame steps by the
                    // datum receivers' fractional δ difference, which is
                    // not an integer and not computable here, so unknown.
                    auto rt = eq34_sat_root.find(amb);
                    if (rt != eq34_sat_root.end()
                        && rt->second != stations[r1].name) known = false;
                    new_sat_root[amb] = stations[r1].name;
                } else {
                    const std::string rxkey = stations[ri].name + rx_suffix;
                    auto rp = eq34_rx_parent.find(rxkey);
                    edge_changed = (rp != eq34_rx_parent.end()
                                    && rp->second != sat);
                    new_rx_parent[rxkey] = sat;
                }
                if (edge_changed) {
                    // Re-pin at a new station: the float there sat at
                    // carry_val and the pin drives it to target, an exact,
                    // known displacement iff the carry exists.
                    if (std::isfinite(carry_val)) pinmove = target - carry_val;
                    // No carry, but under seed-round (INC-2, SPEC §8.5) the
                    // station's own ambiguity state is the pre-pin value, and it
                    // is exact: it is the estimator's own number, not a leveling
                    // carry that can drift (the I1c mechanism that made carry_shift
                    // harmful in the field does not apply).  The frame step is
                    // therefore computable, and the edge stays shiftable even with
                    // the global carry-shift knob off.  That keeps the (sat,σ) out
                    // of eq34_reref, so its carried PB prior is folded
                    // (S-transformed) instead of marginalized for the epoch.
                    else if (eq34_seed_rehost && std::isfinite(seed_x)) {
                        pinmove = target - seed_x;
                        frame_shiftable = true;
                        ++ctx.counters.seed_round_rehost_framed;
                        const double st_abs = std::abs(pinmove);
                        if (st_abs > ctx.counters.seed_round_rehost_max_step_m)
                            ctx.counters.seed_round_rehost_max_step_m = st_abs;
                    } else {
                        known = false;
                        // Counted only when armed, so that with INC-2 off the
                        // run-end block stays byte-identical.
                        if (eq34_seed_rehost) ++ctx.counters.seed_round_rehost_cold;
                    }
                } else {
                    // Same station: its float was hard-pinned at last
                    // epoch's target, so the displacement is exactly the
                    // target difference (no carry noise involved).
                    auto pt = eq34_prev_target.find(
                        {static_cast<int>(ri), amb});
                    pinmove = target
                        - (pt != eq34_prev_target.end() ? pt->second : 0.0);
                }
                // Seed-round bookkeeping: a deferred pin's target is decided at
                // the fold, so no frame step is computable here (the serial
                // consume feeds the realized target back as next epoch's
                // baseline).  An election-side seed target that moved since last
                // epoch (the float crossed a half-integer, or a dropped station
                // lost last epoch's deferred feedback) is a frame re-realization,
                // not a transformable shift: it goes through the validated reref
                // hygiene, not the v2-shift walk.
                // INC-2 exempts the framed re-host: its step is an exactly
                // computed frame move, not a same-station target change, so it
                // takes the shift walk (and its own counter) instead of the reref
                // hygiene.
                if (seed_deferred) known = false;
                else if (seed_rounded && pinmove != 0.0 && !frame_shiftable) {
                    known = false;
                    ++ctx.counters.seed_round_target_steps;
                }
                new_prev_target[{static_cast<int>(ri), amb}] = target;
                stations[ri].pinned_amb_targets[amb] = target;
                ++eq34_n_pins;
                // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md): this
                // pin's host arc is (ri, amb) whichever endpoint the edge attaches.
                // Count the reattach (the sat-side host moved; NetworkSolPrep traces
                // this event as PHASE_DATUM/eq34_reattach) and, when the eligible
                // pass could not avoid a fresh host, give the pin fold an INC-3 σ
                // multiplier so the datum row is soft enough for the data to pull
                // the cold ambiguity instead of re-levelling the component onto its
                // code-seeded value.  Inert (no map entry, no counter) when the card
                // is off.
                if (hyg.active) {
                    if (edge_changed && sat_is_child) ++ctx.counters.hyg.reattaches;
                    const double sc = hyg.pinSigmaScale(ri, amb);
                    if (sc > 1.0) {
                        stations[ri].pinned_amb_sigma_scale[amb] = sc;
                        ++ctx.counters.hyg.fresh_pin_ramps;
                    }
                }
                order.emplace_back(sat_is_child ? sV(sat) : rV(ri),
                                   sat_is_child ? rV(ri) : sV(sat),
                                   pinmove, known, frame_shiftable);
            };
            auto attach = [&](const std::string& v) {
                visited.insert(v);
                inc_q.push_back(v);
                new_q.push_back(v);
            };
            attach(rV(r1));
            // eq34 reattach hygiene INC-2: a bridged satellite's gauge is realized on
            // the shared side, by a soft pseudo-obs row holding b_s at its last solved
            // level (networkInjectPinBridgePrior).  It enters the walk already
            // grounded: no parent edge, no ambiguity pin, and the walk may pass
            // through it to reach receivers.  The spanning structure is then a forest
            // rooted at {r1} ∪ {bridged sats}, and the rank arithmetic still closes:
            // 1 RPB pin + B bridge rows + (n_c + m_c − 1 − B) tree edges = n_c + m_c.
            // Bridge rows are counted as pins for that cross-check.
            std::vector<std::string> bridged_roots;
            if (hyg.active && !hyg.bridged.empty()) {
                for (const auto& s : comp_sat[root]) {
                    if (!hyg.isBridged(StateKeys::makeAmbiguityKey(s, grp_sig))) continue;
                    if (visited.count(sV(s))) continue;
                    attach(sV(s));
                    bridged_roots.push_back(sV(s));
                    ++eq34_n_pins;
                    // Audit: a bridge pin is realized on the shared side, so the
                    // station-side fold expectation is declared − bridged.
                    ++ctx.carry.eq34_epoch_bridged[dr_key];
                }
            }
            // A new tree edge is grown only onto an arc whose ambiguity has converged
            // (age ≥ eq34_anchor_min_arc_ep; INC-1).  The walk runs twice: the
            // first pass admits eligible edges only, so a mature host wins over a
            // fresh one regardless of the I4/longevity rank order; if that leaves
            // vertices unreachable (their only connection to the component runs
            // through a fresh arc), the second pass admits everything and counts
            // each unavoidable cold pin.  The incumbent queue is not filtered:
            // keeping the incumbent is not a reattach, and blocking it would cause
            // the handover this card is meant to prevent.
            const std::size_t comp_vertices =
                rx_list.size() + comp_sat[root].size();
            bool allow_cold = !hyg.active || hyg.min_arc_ep <= 0;
            // Slip-aware (Card 3/4): a slipped arc is refused in the first pass on the
            // same terms as a cold host, and admitted in the second.  The refusal is
            // here and not only in the arcLife ranking because arcLife is read only
            // under the longevity election; relying on it would leave slip avoidance
            // inactive wherever longevity is off (its default).
            // Avoiding a slipped tree edge is a correctness preference, not a longevity
            // heuristic, so it has its own second-chance flag.  Without this refusal,
            // de-seeding an incumbent (above) would be worse than no action: the walk
            // would hand the edge straight back and pay a datum handover for it.
            bool allow_slipped = !eq34_slip_aware;
            auto newEdgeOk = [&](size_t ri, const std::string& sat) {
                if (!allow_slipped && arcSlipped(ri, sat, grp_sig)) return false;
                if (hyg.eligibleHost(ri, StateKeys::makeAmbiguityKey(sat, grp_sig))) return true;
                if (!allow_cold) return false;
                ++ctx.counters.hyg.cold_fallback_pins;
                return true;
            };
            for (int pass = 0; pass < 2; ++pass) {
                if (pass == 1) {
                    // Everything reachable through eligible edges is in; anything
                    // left needs the cold fallback.  `visited` is an ordered set, so
                    // the rebuilt frontier is deterministic.  Both refusals share the
                    // pass: a vertex still unreached may be behind a cold host, a
                    // slipped arc, or both, and there is no third pass to separate them.
                    if ((allow_cold && allow_slipped) || visited.size() >= comp_vertices)
                        break;
                    allow_cold = true;
                    allow_slipped = true;
                    for (const auto& v : visited) new_q.push_back(v);
                }
                while (!inc_q.empty() || !new_q.empty()) {
                if (!inc_q.empty()) {
                    const std::string v = inc_q.front(); inc_q.pop_front();
                    auto ia = inc_adj.find(v);
                    if (ia == inc_adj.end()) continue;
                    for (const auto& u : ia->second) {
                        if (visited.count(u)) continue;
                        if (isSatVertex(u))
                            recordEdge(static_cast<size_t>(
                                           std::stoul(v.substr(1))),
                                       u.substr(1), /*sat_is_child=*/true);
                        else
                            recordEdge(static_cast<size_t>(
                                           std::stoul(u.substr(1))),
                                       v.substr(1), /*sat_is_child=*/false);
                        attach(u);
                    }
                } else {
                    const std::string v = new_q.front(); new_q.pop_front();
                    if (!isSatVertex(v)) {
                        const size_t ri =
                            static_cast<size_t>(std::stoul(v.substr(1)));
                        std::vector<std::string> ss = arcs[ri];
                        // Longevity (I4): attach this receiver's
                        // longest-lived unvisited sats first (rising or
                        // high-elevation), so the new edge is picked to
                        // outlive the epoch.  Off: legacy sat-name order
                        // (byte-identical).
                        if (eq34_longevity)
                            std::sort(ss.begin(), ss.end(),
                                      [&](const std::string& a, const std::string& b) {
                                          const double la = arcLife(ri, a),
                                                       lb = arcLife(ri, b);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(ss.begin(), ss.end());
                        for (const auto& s : ss) {
                            if (visited.count(sV(s))) continue;
                            if (!newEdgeOk(ri, s)) continue;   // pass 1 refuses a fresh host or slipped arc (INC-1)
                            recordEdge(ri, s, /*sat_is_child=*/true);
                            attach(sV(s));
                        }
                    } else {
                        const std::string s = v.substr(1);
                        std::vector<size_t> rr = satrx[s];
                        // Longevity (I4): attach this sat through its most
                        // durable unvisited tracker first: highest
                        // per-constellation coverage (the robust receiver,
                        // spec D2 step 3), then the arc's own longevity at
                        // that station.
                        // Off: legacy station-index order (byte-identical).
                        if (eq34_longevity)
                            std::sort(rr.begin(), rr.end(),
                                      [&](size_t a, size_t b) {
                                          const int ca = cov(grp_sys, a),
                                                    cb = cov(grp_sys, b);
                                          if (ca != cb) return ca > cb;
                                          const double la = arcLife(a, s),
                                                       lb = arcLife(b, s);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(rr.begin(), rr.end());
                        for (size_t ri : rr) {
                            if (visited.count(rV(ri))) continue;
                            if (!newEdgeOk(ri, s)) continue;   // pass 1 refuses a fresh host or slipped arc (INC-1)
                            recordEdge(ri, s, /*sat_is_child=*/false);
                            attach(rV(ri));
                        }
                    }
                }
                }
            }
// ---- END FROZEN ----
            // HARNESS: read the component's walk, then close the component loop.
            FrozenChainComponent fc;
            fc.root    = root;
            fc.r1      = r1;
            fc.visited = visited.size();
            for (const auto& e : order) fc.edges.emplace_back(std::get<0>(e), std::get<1>(e));
            out.comps.push_back(std::move(fc));
            tape.ev.push_back(TapeEvent{'X', 0, {}});
            (void)bridged_roots;
        }
    out.best_datum_rx      = best_datum_rx;
    out.cold_fallback_pins = ctx.counters.hyg.cold_fallback_pins.n;
    out.slip_readmitted    = ctx.counters.eq34_slip_readmitted.n;
    out.slip_deseeded      = ctx.counters.eq34_slip_deseeded;
    (void)eq34_n_pins; (void)eq34_gauge_dim; (void)eq34_gap_repin_total;
    return out;
}

// =====================================================================================
// Shared generators.
// =====================================================================================

/// "G01".."G99".
std::string gpsSat(int prn) {
    std::string s = "G";
    if (prn < 10) s += "0";
    return s + std::to_string(prn);
}

/// Uniform integer in [lo, hi].
int uniform(std::mt19937& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
/// True with probability p.
bool chance(std::mt19937& rng, double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; }

/// A random tree over the given labels (each new vertex hangs off a random earlier one).
std::vector<ArcKey> randomTree(std::mt19937& rng, const std::vector<int>& labels) {
    std::vector<ArcKey> edges;
    for (std::size_t i = 1; i < labels.size(); ++i)
        edges.emplace_back(labels[i], labels[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(i) - 1))]);
    return edges;
}

/// n distinct labels drawn from [0, range).
std::vector<int> randomLabels(std::mt19937& rng, int n, int range) {
    std::vector<int> all(static_cast<std::size_t>(range));
    std::iota(all.begin(), all.end(), 0);
    std::shuffle(all.begin(), all.end(), rng);
    all.resize(static_cast<std::size_t>(n));
    return all;
}

// =====================================================================================
// Union-find.
// =====================================================================================

TEST(NetworkArcGraphOracle, DsuRootsMatchTheStringAndDriverUnionFinds) {
    std::mt19937 rng(20261010u);
    for (int it = 0; it < 1000; ++it) {
        // A bipartite union sequence (station, satellite), as the chain datum and the driver unite,
        // with interleaved finds (path compression must not move a root).
        const int n_sta = uniform(rng, 1, 25), n_sat = uniform(rng, 1, 20), n_ops = uniform(rng, 0, 80);
        Dsu d;
        UnionFind uf;
        std::vector<std::pair<std::string, std::string>> sseq;
        for (int op = 0; op < n_ops; ++op) {
            const int s = uniform(rng, 0, n_sta - 1), k = uniform(rng, 0, n_sat - 1);
            dsuUnite(d, s, 1000 + k);
            uf.unite(s, 1000 + k);
            sseq.emplace_back("R" + std::to_string(s), "S" + gpsSat(k + 1));
            if (chance(rng, 0.3)) {
                const int v = chance(rng, 0.5) ? uniform(rng, 0, n_sta - 1) : 1000 + uniform(rng, 0, n_sat - 1);
                ASSERT_EQ(dsuFind(d, v), uf.find(v));
            }
        }
        std::vector<std::string> names;
        std::map<int, std::string> name_of;
        for (int s = 0; s < n_sta; ++s) name_of[s] = "R" + std::to_string(s);
        for (int k = 0; k < n_sat; ++k) name_of[1000 + k] = "S" + gpsSat(k + 1);
        for (const auto& [id, nm] : name_of) names.push_back(nm);
        const auto sroots = frozenStringUfRoots(sseq, names);
        for (const auto& [id, nm] : name_of) {
            const int r = dsuFind(d, id);
            ASSERT_EQ(r, uf.find(id)) << "iteration " << it;
            ASSERT_EQ(name_of.at(r), sroots.at(nm)) << "iteration " << it;
        }

        // A general union sequence over one id space.
        Dsu d2;
        UnionFind uf2;
        std::vector<std::pair<std::string, std::string>> gseq;
        const int n_v = uniform(rng, 1, 30);
        for (int op = 0, n = uniform(rng, 0, 60); op < n; ++op) {
            const int a = uniform(rng, 0, n_v - 1), b = uniform(rng, 0, n_v - 1);
            dsuUnite(d2, a, b);
            uf2.unite(a, b);
            gseq.emplace_back("V" + std::to_string(a), "V" + std::to_string(b));
        }
        std::vector<std::string> gnames;
        for (int v = 0; v < n_v; ++v) gnames.push_back("V" + std::to_string(v));
        const auto groots = frozenStringUfRoots(gseq, gnames);
        for (int v = 0; v < n_v; ++v) {
            const int r = dsuFind(d2, v);
            ASSERT_EQ(r, uf2.find(v));
            ASSERT_EQ("V" + std::to_string(r), groots.at("V" + std::to_string(v)));
        }
    }
}

// =====================================================================================
// Trees: singleTreeOf and fundamentalCycle against the live ZComponent.
// =====================================================================================

TEST(NetworkArcGraphOracle, SingleTreeAcceptsAndRejectsAsTheZComponentConstructor) {
    std::mt19937 rng(7u);
    int accepted = 0, rejected = 0;
    for (int it = 0; it < 2000; ++it) {
        const int n_v = uniform(rng, 1, 12);
        const std::vector<int> labels = randomLabels(rng, n_v, 60);
        std::vector<ArcKey> edges;
        switch (it % 6) {
            case 0: edges = randomTree(rng, labels); break;                       // a tree
            case 1:                                                                 // a tree plus one more edge: a cycle
                edges = randomTree(rng, labels);
                if (n_v >= 2)
                    edges.emplace_back(labels[0], labels[static_cast<std::size_t>(uniform(rng, 1, n_v - 1))]);
                break;
            case 2:                                                                 // a duplicate edge
                edges = randomTree(rng, labels);
                if (!edges.empty()) edges.push_back(edges[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(edges.size()) - 1))]);
                break;
            case 3: {                                                               // several fragments
                const std::size_t cut = static_cast<std::size_t>(uniform(rng, 1, std::max(1, n_v - 1)));
                const std::vector<int> a(labels.begin(), labels.begin() + static_cast<std::ptrdiff_t>(std::min(cut, labels.size())));
                const std::vector<int> b(labels.begin() + static_cast<std::ptrdiff_t>(std::min(cut, labels.size())), labels.end());
                edges = randomTree(rng, a);
                const std::vector<ArcKey> eb = randomTree(rng, b);
                edges.insert(edges.end(), eb.begin(), eb.end());
                break;
            }
            case 4: break;                                                          // empty
            default:                                                                // random edges
                for (int e = 0, n = uniform(rng, 1, 2 * n_v); e < n && n_v >= 2; ++e) {
                    const int a = uniform(rng, 0, n_v - 1);
                    int b = uniform(rng, 0, n_v - 2);
                    if (b >= a) ++b;
                    edges.emplace_back(labels[static_cast<std::size_t>(a)], labels[static_cast<std::size_t>(b)]);
                }
                break;
        }
        std::shuffle(edges.begin(), edges.end(), rng);
        bool z_ok = true;
        ZComponent z;
        try { z = ZComponent(edges); } catch (const std::invalid_argument&) { z_ok = false; }
        const std::optional<IntTree> t = singleTreeOf(edges);
        ASSERT_EQ(t.has_value(), z_ok) << "iteration " << it;
        if (!z_ok) { ++rejected; continue; }
        ++accepted;
        ASSERT_EQ(t->vertices, z.vertices());
        std::set<ArcKey> te;
        for (const auto& [x, nbrs] : t->adj)
            for (int y : nbrs) te.insert(ArcKey(x, y));
        const std::vector<ArcKey> ze = z.treeEdges();
        ASSERT_EQ(te, std::set<ArcKey>(ze.begin(), ze.end()));
    }
    EXPECT_GT(accepted, 500);
    EXPECT_GT(rejected, 500);
    std::cout << "[ NetworkArcGraphOracle ] single tree: accepted " << accepted << ", rejected " << rejected << "\n";
}

TEST(NetworkArcGraphOracle, FundamentalCycleEqualsZComponent) {
    std::mt19937 rng(11u);
    for (int it = 0; it < 500; ++it) {
        const int n_v = uniform(rng, 2, 14);
        const std::vector<int> labels = randomLabels(rng, n_v, 100);
        const std::vector<ArcKey> tree = randomTree(rng, labels);
        const ZComponent z(tree);
        const std::optional<IntTree> t = singleTreeOf(tree);
        ASSERT_TRUE(t.has_value());
        for (int a : labels)
            for (int b : labels) {
                if (a >= b) continue;
                const ArcKey arc(a, b);   // a chord, or a tree edge (held at 0 by both)
                ASSERT_EQ(fundamentalCycle(*t, arc), z.fundamentalCycle(arc)) << "iteration " << it;
            }
        // An endpoint outside the tree: both refuse.
        const ArcKey outside(labels[0], 1000);
        EXPECT_THROW(fundamentalCycle(*t, outside), std::invalid_argument);
        EXPECT_THROW(z.fundamentalCycle(outside), std::invalid_argument);
    }
}

// =====================================================================================
// Pair completion.
// =====================================================================================

TEST(NetworkArcGraphOracle, PromoteBridgingArcsEqualsTheFrozenPairCompletion) {
    std::mt19937 rng(13u);
    long long promoted_total = 0;
    for (int it = 0; it < 3000; ++it) {
        const int n_v = uniform(rng, 2, 16);
        const std::vector<int> labels = randomLabels(rng, n_v, 64);
        // A partial forest: a random tree with some edges removed (its fragments).
        std::vector<ArcKey> tree = randomTree(rng, labels);
        tree.erase(std::remove_if(tree.begin(), tree.end(), [&](const ArcKey&) { return chance(rng, 0.4); }), tree.end());
        std::vector<ArcKey> chords;
        for (int e = 0, n = uniform(rng, 0, 3 * n_v); e < n; ++e) {
            const int a = uniform(rng, 0, n_v - 1);
            int b = uniform(rng, 0, n_v - 2);
            if (b >= a) ++b;
            chords.emplace_back(labels[static_cast<std::size_t>(a)], labels[static_cast<std::size_t>(b)]);
        }
        std::vector<ArcKey> t1 = tree, c1 = chords, t2 = tree, c2 = chords;
        const long long n1 = frozenPairCompletion(t1, c1);
        const long long n2 = promoteBridgingArcs(t2, c2);
        ASSERT_EQ(n1, n2) << "iteration " << it;
        ASSERT_EQ(t1, t2);
        ASSERT_EQ(c1, c2);
        promoted_total += n2;
    }
    EXPECT_GT(promoted_total, 1000);
}

// =====================================================================================
// The ARL Prim.
// =====================================================================================

/// One random visibility matrix: satellites and stations on a circle, a station seeing the
/// satellites within its footprint (with dropouts); some stations are duplicated (count ties) and
/// some see a few random satellites only (disconnected tails).
struct PrimCase {
    int                                                    n_sta{0};
    std::vector<std::size_t>                               index;   ///< Station position -> roster index.
    std::vector<FrozenArStation>                           roster;  ///< Roster index -> name.
    std::map<std::string, std::map<std::size_t, double>>   vis;     ///< Satellite -> roster index -> value.
};

PrimCase randomPrimCase(std::mt19937& rng, int it) {
    PrimCase pc;
    const int n_sta = (it < 10) ? 300 : (it < 40) ? 2 : (it % 50 == 0) ? uniform(rng, 41, 300) : uniform(rng, 2, 40);
    const int n_sat = uniform(rng, 1, 40);
    pc.n_sta = n_sta;
    std::uniform_real_distribution<double> ang(0.0, 6.283185307179586), val(-1000.0, 1000.0);
    std::vector<double> sat_pos(static_cast<std::size_t>(n_sat));
    for (double& a : sat_pos) a = ang(rng);
    const double width = std::uniform_real_distribution<double>(0.3, 2.5)(rng);
    const double drop  = std::uniform_real_distribution<double>(0.0, 0.3)(rng);
    std::vector<std::vector<char>> sees(static_cast<std::size_t>(n_sta), std::vector<char>(static_cast<std::size_t>(n_sat), 0));
    for (int r = 0; r < n_sta; ++r) {
        if (r > 0 && chance(rng, 0.1)) {   // a duplicated station: forced count ties
            sees[static_cast<std::size_t>(r)] = sees[static_cast<std::size_t>(uniform(rng, 0, r - 1))];
            continue;
        }
        if (chance(rng, 0.08)) {           // a disconnected tail: a few random satellites
            for (int q = 0, n = uniform(rng, 0, 2); q < n; ++q)
                sees[static_cast<std::size_t>(r)][static_cast<std::size_t>(uniform(rng, 0, n_sat - 1))] = 1;
            continue;
        }
        const double pos = ang(rng);
        for (int k = 0; k < n_sat; ++k) {
            double dlt = std::abs(pos - sat_pos[static_cast<std::size_t>(k)]);
            dlt = std::min(dlt, 6.283185307179586 - dlt);
            if (dlt < width && !chance(rng, drop)) sees[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] = 1;
        }
    }
    std::size_t idx = static_cast<std::size_t>(uniform(rng, 0, 3));
    for (int r = 0; r < n_sta; ++r) {
        pc.index.push_back(idx);
        idx += static_cast<std::size_t>(uniform(rng, 1, 3));   // roster gaps: a slot is not a roster index
    }
    pc.roster.resize(idx + 1);
    for (std::size_t i = 0; i < pc.roster.size(); ++i) pc.roster[i].name = "STA" + std::to_string(1000 + i);
    for (int r = 0; r < n_sta; ++r)
        for (int k = 0; k < n_sat; ++k)
            if (sees[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)])
                pc.vis[gpsSat(k + 1)][pc.index[static_cast<std::size_t>(r)]] = val(rng);
    return pc;
}

TEST(NetworkArcGraphOracle, GrowBaselineForestThenParentOnlyReproducesTheFrozenPrim) {
    std::mt19937 rng(17u);
    const ArcFamily family{System::GPS, {*SignalId::fromObsCode(System::GPS, "L1C")}};
    ForestRules legacy;   // card ARL tree_basis: min_common 2, stop at the gap, first common, no incumbent
    legacy.band_mask   = 1;
    legacy.min_common  = 2;
    legacy.stop_at_gap = true;
    long long n_dd = 0, n_cut = 0;
    for (int it = 0; it < 5000; ++it) {
        const PrimCase pc = randomPrimCase(rng, it);
        int n_const_signal = 0;
        const std::vector<FrozenDd> frozen = frozenPrim(pc.vis, pc.roster, &n_const_signal);

        std::vector<StationId> station_order;
        for (std::size_t i : pc.index) station_order.push_back(StationId{static_cast<int>(i)});
        std::vector<SatId> sat_order;
        std::vector<ArcRow> rows;
        std::map<std::pair<int, std::string>, double> value;
        for (const auto& [sat, by_sta] : pc.vis) {
            sat_order.push_back(*SatId::parse(sat));
            for (const auto& [sta, v] : by_sta) value[{static_cast<int>(sta), sat}] = v;
        }
        for (const auto& [key, v] : value) {   // rows in (station, satellite) order
            ArcRow row;
            row.station = StationId{key.first};
            row.sat     = *SatId::parse(key.second);
            row.present = 1;
            rows.push_back(row);
        }
        const ArcGraph g = buildArcGraph(family, station_order, sat_order, rows);
        const SpanningForest f = growBaselineForest(g, wholeGraph(g, 1), {}, legacy);
        const CycleBasis b = shortCycleBasis(g, f, BasisCoverage::ParentOnly);

        ASSERT_EQ(b.cycles.size(), frozen.size()) << "iteration " << it;
        for (std::size_t q = 0; q < frozen.size(); ++q) {
            const BasisCycle& bc = b.cycles[q];
            ASSERT_EQ(pc.roster[static_cast<std::size_t>(bc.station.index)].name, frozen[q].sta_r) << "iteration " << it << " dd " << q;
            ASSERT_EQ(pc.roster[static_cast<std::size_t>(bc.partner.index)].name, frozen[q].ref_sta);
            ASSERT_EQ(bc.sat.str(), frozen[q].sat_j);
            ASSERT_EQ(bc.ref_sat.str(), frozen[q].ref_sat);
            ASSERT_EQ(bc.kind, CycleKind::PartnerDd);
            // The cycle is that DD: +(r,j) -(r,ref) -(p,j) +(p,ref).
            ASSERT_EQ(bc.cycle.terms.size(), 4u);
            double dd = 0.0;
            for (const CycleTerm& t : bc.cycle.terms) {
                const bool at_r = t.arc.station == bc.station, at_j = t.arc.sat == bc.sat;
                ASSERT_EQ(t.coeff, (at_r == at_j) ? +1 : -1);
                dd += t.coeff * value.at({t.arc.station.index, t.arc.sat.str()});
            }
            ASSERT_NEAR(dd, frozen[q].dd_m, 1e-9 * (1.0 + std::abs(frozen[q].dd_m)));
        }
        n_dd += static_cast<long long>(frozen.size());
        std::set<std::size_t> seen;
        for (const auto& [sat, by_sta] : pc.vis)
            for (const auto& [sta, v] : by_sta) seen.insert(sta);
        if (seen.size() >= 2 && f.station_order.size() < seen.size()) ++n_cut;
    }
    EXPECT_GT(n_dd, 100000);
    EXPECT_GT(n_cut, 100);   // the stop point was exercised
    std::cout << "[ NetworkArcGraphOracle ] Prim: " << n_dd << " DDs, " << n_cut << " matrices stopped at the gap\n";
}

// =====================================================================================
// The chain walk.
// =====================================================================================

ChainGroupInput randomChainGroup(std::mt19937& rng, int it) {
    ChainGroupInput in;
    const int roster = uniform(rng, 4, 27);
    std::set<std::string> used;
    while (static_cast<int>(in.names.size()) < roster) {
        std::string nm;
        for (int c = 0; c < 4; ++c) nm += static_cast<char>('A' + uniform(rng, 0, 25));
        if (used.insert(nm).second) in.names.push_back(nm);
    }
    in.sat_el.assign(static_cast<std::size_t>(roster), {});
    const bool forced_pass2 = (it % 5 == 0);
    in.slip_aware  = forced_pass2 ? chance(rng, 0.7) : chance(rng, 0.5);
    in.longevity   = chance(rng, 0.5);
    in.extend_only = chance(rng, 0.25);
    in.hyg.active     = forced_pass2 ? true : chance(rng, 0.5);
    in.hyg.min_arc_ep = forced_pass2 ? 3 : uniform(rng, 0, 3);

    // Group stations and satellites, split into one to three blocks (components).
    std::vector<std::size_t> group;
    for (int i = 0; i < roster; ++i) if (chance(rng, 0.7)) group.push_back(static_cast<std::size_t>(i));
    if (group.empty()) group.push_back(static_cast<std::size_t>(uniform(rng, 0, roster - 1)));
    std::vector<int> prns = randomLabels(rng, uniform(rng, 1, 12), 32);
    const int n_blocks = uniform(rng, 1, 3);
    std::map<int, std::vector<std::string>> block_sats;
    for (int prn : prns) block_sats[uniform(rng, 0, n_blocks - 1)].push_back(gpsSat(prn + 1));
    const double p_arc = std::uniform_real_distribution<double>(0.35, 0.95)(rng);
    for (std::size_t i : group) {
        auto bit = block_sats.find(uniform(rng, 0, n_blocks - 1));
        if (bit == block_sats.end()) bit = block_sats.begin();
        std::vector<std::string> mine;
        for (const std::string& s : bit->second) if (chance(rng, p_arc)) mine.push_back(s);
        if (mine.empty()) mine.push_back(bit->second[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(bit->second.size()) - 1))]);
        std::shuffle(mine.begin(), mine.end(), rng);   // the group's push order fixes the union order
        in.arcs[i] = mine;
        for (const std::string& s : mine) in.sats.insert(s);
    }

    const double p_slip = forced_pass2 ? 0.3 : 0.1;
    for (const auto& [i, slist] : in.arcs)
        for (const std::string& s : slist) {
            const std::string amb = StateKeys::makeAmbiguityKey(s, "L1C");
            if (chance(rng, p_slip)) in.slipped.insert({static_cast<int>(i), amb});
            if (chance(rng, 0.95)) {
                const double el = 10.0 * uniform(rng, 1, 4);   // few values: forced life ties
                in.sat_el[i][s] = el;
                if (chance(rng, 0.5)) in.prev_el[{i, s}] = el + 5.0 * uniform(rng, -1, 1);
            }
            in.hyg.age[{static_cast<int>(i), amb}] = forced_pass2 ? (chance(rng, 0.6) ? 0 : 5) : uniform(rng, 0, 4);
            if (chance(rng, 0.2)) in.retained.insert({static_cast<int>(i), amb});
        }
    for (std::size_t i : group) in.cover['G'][i] = uniform(rng, 2, 4);
    if (in.hyg.active)
        for (const std::string& s : in.sats) if (chance(rng, 0.15)) in.hyg.bridged.insert(StateKeys::makeAmbiguityKey(s, "L1C"));

    // Incumbents: live, stale (a station that does not track the satellite, or no station), slipped,
    // and entries of another constellation or signal (skipped by the seed).
    for (const std::string& s : in.sats) {
        if (!chance(rng, 0.6)) continue;
        std::vector<std::size_t> trackers;
        for (const auto& [i, slist] : in.arcs)
            if (std::find(slist.begin(), slist.end(), s) != slist.end()) trackers.push_back(i);
        std::string sta;
        const int roll = uniform(rng, 0, 9);
        if (roll < 7 && !trackers.empty()) sta = in.names[trackers[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(trackers.size()) - 1))]];
        else if (roll < 9) sta = in.names[static_cast<std::size_t>(uniform(rng, 0, roster - 1))];
        else sta = "ZZZZZ";
        in.anchor_incumbent[StateKeys::makeAmbiguityKey(s, "L1C")] = sta;
    }
    in.anchor_incumbent["E05_L1C"] = in.names[0];
    in.anchor_incumbent[StateKeys::makeAmbiguityKey(*in.sats.begin(), "L2W")] = in.names[0];
    for (const auto& [i, slist] : in.arcs) {
        if (!chance(rng, 0.5)) continue;
        const std::string sat = chance(rng, 0.8) ? slist[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(slist.size()) - 1))]
                                                 : *std::next(in.sats.begin(), uniform(rng, 0, static_cast<int>(in.sats.size()) - 1));
        in.rx_parent[in.names[i] + "|G|L1C"] = sat;
        if (chance(rng, 0.2)) in.rx_parent[in.names[i] + "|E|L1C"] = "E07";
    }
    for (const std::string& s : in.sats)
        if (chance(rng, 0.4)) in.sat_root[StateKeys::makeAmbiguityKey(s, "L1C")] = in.names[group[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(group.size()) - 1))]];
    if (chance(rng, 0.5)) in.datum_rx["G|L1C"] = in.names[group[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(group.size()) - 1))]];
    if (chance(rng, 0.15)) in.pref_rx = in.names[group[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(group.size()) - 1))]];
    return in;
}

/// One tree edge as the frozen walk produced it (read from `order` and the tape).
struct WalkEdge {
    std::size_t ri{0};
    std::string sat;
    bool        attaches_sat{false};
    bool        from_incumbent{false};
    bool        ineligible_host{false};
    bool        slipped{false};
    int         pass{1};
    bool operator==(const WalkEdge& o) const {
        return ri == o.ri && sat == o.sat && attaches_sat == o.attaches_sat && from_incumbent == o.from_incumbent
            && ineligible_host == o.ineligible_host && slipped == o.slipped && pass == o.pass;
    }
};
std::ostream& operator<<(std::ostream& os, const WalkEdge& e) {
    return os << "(R" << e.ri << ",S" << e.sat << (e.attaches_sat ? " sat" : " rx") << " inc=" << e.from_incumbent
              << " cold=" << e.ineligible_host << " slip=" << e.slipped << " pass=" << e.pass << ")";
}

/// The frozen walk's edges per component, flags read from the tape.
std::vector<std::vector<WalkEdge>> frozenEdges(const FrozenChainOut& out, const Tape& tape) {
    std::vector<std::vector<WalkEdge>> comps;
    std::size_t e = 0;
    for (const FrozenChainComponent& fc : out.comps) {
        std::vector<WalkEdge> edges;
        std::set<std::string> popped;
        bool pass2 = false, cold = false, readmitted = false;
        std::optional<std::pair<std::size_t, std::string>> last_test;
        std::size_t q = 0;
        for (; e < tape.ev.size() && tape.ev[e].kind != 'X'; ++e) {
            const TapeEvent& ev = tape.ev[e];
            if (ev.kind == 'P') { if (!popped.insert(ev.key).second) pass2 = true; }
            else if (ev.kind == 'E') last_test = std::make_pair(ev.ri, ev.key);
            else if (ev.kind == 'C') cold = true;
            else if (ev.kind == 'R') readmitted = true;
            else if (ev.kind == 'T') {
                WalkEdge w;
                const auto& [child, parent] = fc.edges.at(q++);
                w.attaches_sat    = child[0] == 'S';
                w.ri              = static_cast<std::size_t>(std::stoul((w.attaches_sat ? parent : child).substr(1)));
                w.sat             = (w.attaches_sat ? child : parent).substr(1);
                w.from_incumbent  = !(last_test && last_test->first == ev.ri && last_test->second == ev.key);
                w.ineligible_host = cold;
                w.slipped         = readmitted;
                w.pass            = pass2 ? 2 : 1;
                EXPECT_EQ(ev.ri, w.ri);
                EXPECT_EQ(ev.key, StateKeys::makeAmbiguityKey(w.sat, "L1C"));
                edges.push_back(w);
                last_test.reset();
                cold = readmitted = false;
            }
        }
        ++e;   // the component's 'X'
        EXPECT_EQ(q, fc.edges.size());
        comps.push_back(std::move(edges));
    }
    return comps;
}

TEST(NetworkArcGraphOracle, GrowChainForestReproducesTheFrozenChainWalk) {
    std::mt19937 rng(19u);
    const ArcFamily family{System::GPS, {*SignalId::fromObsCode(System::GPS, "L1C")}};
    long long n_edges = 0, n_pass2 = 0, n_incumbent = 0, n_cold = 0, n_slipped = 0, n_multi = 0, n_bridged_roots = 0;
    for (int it = 0; it < 10000; ++it) {
        const ChainGroupInput in = randomChainGroup(rng, it);
        Tape tape;
        const FrozenChainOut frozen = frozenChainWalk(in, tape);
        const std::vector<std::vector<WalkEdge>> want = frozenEdges(frozen, tape);

        // The caller's side (AGT-4's adapter): rows in the group's push order, the slipped bit, the
        // longevity score as arcLife computes it, the hygiene verdict.
        const auto slipped = [&](std::size_t i, const std::string& s) {
            return in.slip_aware && in.slipped.count({static_cast<int>(i), StateKeys::makeAmbiguityKey(s, "L1C")}) != 0;
        };
        const auto life = [&](std::size_t i, const std::string& s) {
            const auto ei = in.sat_el[i].find(s);
            if (ei == in.sat_el[i].end()) return -1e9;
            const auto pe = in.prev_el.find({i, s});
            const bool rising = pe != in.prev_el.end() && ei->second > pe->second + 1e-6;
            const double l = ei->second + (rising ? 90.0 : 0.0);
            return slipped(i, s) ? l - 1000.0 : l;
        };
        std::vector<StationId> station_order;
        for (const auto& [i, slist] : in.arcs) station_order.push_back(StationId{static_cast<int>(i)});
        std::vector<SatId> sat_order;
        for (const std::string& s : in.sats) sat_order.push_back(*SatId::parse(s));
        std::vector<ArcRow> rows;
        for (const auto& [i, slist] : in.arcs)
            for (const std::string& s : slist) {
                ArcRow row;
                row.station       = StationId{static_cast<int>(i)};
                row.sat           = *SatId::parse(s);
                row.present       = 1;
                row.slipped       = slipped(i, s) ? 1 : 0;
                row.durability    = life(i, s);
                row.eligible_host = in.hyg.eligibleNoTape(i, StateKeys::makeAmbiguityKey(s, "L1C"));
                rows.push_back(row);
            }
        const ArcGraph g = buildArcGraph(family, station_order, sat_order, rows);
        const std::vector<int> rank = legacyChainVertexRank(g);
        const std::vector<Component> comps = arcComponents(g, 1, rank);
        ASSERT_EQ(comps.size(), frozen.comps.size()) << "iteration " << it;
        if (comps.size() > 1) ++n_multi;

        // The incumbent seed in seed order: anchor_incumbent, then eq34_rx_parent; live and not slipped.
        std::map<std::string, int> slot_of_name;
        for (std::size_t s = 0; s < g.stations.size(); ++s) slot_of_name[in.names[static_cast<std::size_t>(g.stations[s].index)]] = static_cast<int>(s);
        std::vector<int> incumbent;
        long long deseeded = 0;
        const auto seed = [&](const std::string& sta, const std::string& sat) {
            const auto si = slot_of_name.find(sta);
            const std::optional<SatId> k = SatId::parse(sat);
            if (si == slot_of_name.end() || !k) return;
            const auto kit = std::find(g.sats.begin(), g.sats.end(), *k);
            if (kit == g.sats.end()) return;
            const int a = arcIndex(g, si->second, static_cast<int>(kit - g.sats.begin()));
            if (a < 0) return;
            if (g.rows[static_cast<std::size_t>(a)].slipped) { ++deseeded; return; }
            incumbent.push_back(a);
        };
        for (const auto& [amb, sta] : in.anchor_incumbent) {
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            if (!parts || systemOfSat(parts->satellite) != System::GPS || parts->signal != "L1C") continue;
            seed(sta, parts->satellite);
        }
        for (const auto& [rxkey, sat] : in.rx_parent) {
            const std::string suffix = "|G|L1C";
            if (rxkey.size() <= suffix.size() || rxkey.compare(rxkey.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            seed(rxkey.substr(0, rxkey.size() - suffix.size()), sat);
        }
        ASSERT_EQ(deseeded, frozen.slip_deseeded);

        ForestRules rules;
        rules.band_mask               = 1;
        rules.refuse_slipped_first    = in.slip_aware;
        rules.refuse_ineligible_first = in.hyg.active && in.hyg.min_arc_ep > 0;
        rules.by_durability           = in.longevity;
        std::vector<int> station_rank;
        for (const StationId& s : g.stations) station_rank.push_back(in.cover.at('G').at(static_cast<std::size_t>(s.index)));
        long long cold_total = 0, slipped_total = 0;
        for (std::size_t ci = 0; ci < comps.size(); ++ci) {
            const Component& c = comps[ci];
            const FrozenChainComponent& fc = frozen.comps[ci];
            ASSERT_EQ("S" + g.sats[static_cast<std::size_t>(c.root - static_cast<int>(g.stations.size()))].str(), fc.root) << "iteration " << it;
            std::vector<int> roots{stationVertex(g, slot_of_name.at(in.names[fc.r1]))};
            if (in.hyg.active && !in.hyg.bridged.empty())
                for (int k : c.sats)
                    if (in.hyg.isBridged(StateKeys::makeAmbiguityKey(g.sats[static_cast<std::size_t>(k)].str(), "L1C"))) {
                        roots.push_back(satVertex(g, k));
                        ++n_bridged_roots;
                    }
            const SpanningForest f = growChainForest(g, c, roots, incumbent, rules, rank, station_rank);
            std::vector<WalkEdge> got;
            for (const TreeEdge& te : f.edges) {
                WalkEdge w;
                w.ri              = static_cast<std::size_t>(g.stations[static_cast<std::size_t>(g.row_station[static_cast<std::size_t>(te.arc)])].index);
                w.sat             = g.sats[static_cast<std::size_t>(g.row_sat[static_cast<std::size_t>(te.arc)])].str();
                w.attaches_sat    = te.attaches_sat;
                w.from_incumbent  = te.from_incumbent;
                w.ineligible_host = te.ineligible_host;
                w.slipped         = te.slipped;
                w.pass            = te.pass;
                got.push_back(w);
                cold_total += te.ineligible_host ? 1 : 0;
                slipped_total += te.slipped ? 1 : 0;
                n_pass2 += te.pass == 2 ? 1 : 0;
                n_incumbent += te.from_incumbent ? 1 : 0;
            }
            ASSERT_EQ(got, want[ci]) << "iteration " << it << " component " << ci;
            ASSERT_EQ(f.edges.size() + roots.size(), fc.visited);
            n_edges += static_cast<long long>(got.size());
        }
        ASSERT_EQ(cold_total, frozen.cold_fallback_pins) << "iteration " << it;
        ASSERT_EQ(slipped_total, frozen.slip_readmitted) << "iteration " << it;
        n_cold += cold_total;
        n_slipped += slipped_total;
    }
    // Every branch the gate names was exercised.
    EXPECT_GT(n_edges, 50000);
    EXPECT_GT(n_pass2, 1000);
    EXPECT_GT(n_incumbent, 5000);
    EXPECT_GT(n_cold, 500);
    EXPECT_GT(n_slipped, 500);
    EXPECT_GT(n_multi, 1000);
    EXPECT_GT(n_bridged_roots, 500);
    std::cout << "[ NetworkArcGraphOracle ] chain walk: " << n_edges << " edges, pass 2 " << n_pass2 << ", incumbent "
              << n_incumbent << ", ineligible " << n_cold << ", slipped " << n_slipped << ", multi-component groups "
              << n_multi << ", bridged roots " << n_bridged_roots << "\n";
}

// =====================================================================================
// The datum-graph census and the emit re-base on the shared Dsu (AGT-3).
// =====================================================================================

/// A satellite id: a constellation letter and a two-digit PRN.
std::string satName(char cons, int prn) {
    std::string s(1, cons);
    if (prn < 10) s += "0";
    return s + std::to_string(prn);
}

/// Clustered visibility over four constellations (GLONASS included), so that a constellation's
/// graph has one to several components: its satellites are dealt to one to four clusters, each
/// station joins one cluster and sees each of that cluster's satellites with a station-specific
/// probability, and, in half the constellations, rarely a satellite of another cluster (which
/// merges two components).  Returns, per constellation letter, station -> the satellites it sees.
std::map<char, std::vector<std::vector<std::string>>> clusteredVisibility(std::mt19937& rng, int n_sta) {
    std::map<char, std::vector<std::vector<std::string>>> vis;
    for (const char cons : {'G', 'E', 'C', 'R'}) {
        if (!chance(rng, 0.8)) continue;
        const int n_sat = uniform(rng, 1, 14), n_cl = uniform(rng, 1, 4);
        std::vector<std::vector<std::string>> cl_sats(static_cast<std::size_t>(n_cl));
        for (int prn : randomLabels(rng, n_sat, 36))
            cl_sats[static_cast<std::size_t>(uniform(rng, 0, n_cl - 1))].push_back(satName(cons, prn + 1));
        const double cross = chance(rng, 0.5) ? 0.0 : 0.03;
        std::vector<std::vector<std::string>>& per_sta = vis[cons];
        per_sta.resize(static_cast<std::size_t>(n_sta));
        for (std::size_t s = 0; s < per_sta.size(); ++s) {
            const int own = uniform(rng, 0, n_cl - 1);
            const double p = 0.2 + 0.7 * std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            for (int c = 0; c < n_cl; ++c)
                for (const std::string& sat : cl_sats[static_cast<std::size_t>(c)])
                    if (chance(rng, c == own ? p : cross)) per_sta[s].push_back(sat);
            std::shuffle(per_sta[s].begin(), per_sta[s].end(), rng);
        }
    }
    return vis;
}

/// Field-by-field equality of two census results.
void expectSameCensus(const std::map<char, NetDatumGraphConsCensus>& got,
                      const std::map<char, NetDatumGraphConsCensus>& want, int it) {
    ASSERT_EQ(got.size(), want.size()) << "iteration " << it;
    for (const auto& [cons, w] : want) {
        const auto g = got.find(cons);
        ASSERT_NE(g, got.end()) << "iteration " << it;
        const NetDatumGraphConsCensus& c = g->second;
        ASSERT_EQ(c.cons, w.cons);
        ASSERT_EQ(c.k, w.k) << "iteration " << it;
        ASSERT_EQ(c.n_stations, w.n_stations);
        ASSERT_EQ(c.n_sats, w.n_sats);
        ASSERT_EQ(c.n_edges, w.n_edges);
        ASSERT_EQ(c.comps.size(), w.comps.size());
        for (std::size_t i = 0; i < w.comps.size(); ++i) {
            ASSERT_EQ(c.comps[i].n_stations, w.comps[i].n_stations) << "iteration " << it << " component " << i;
            ASSERT_EQ(c.comps[i].n_sats, w.comps[i].n_sats) << "iteration " << it << " component " << i;
            ASSERT_EQ(c.comps[i].sample_sat, w.comps[i].sample_sat) << "iteration " << it << " component " << i;
            ASSERT_EQ(c.comps[i].sample_station, w.comps[i].sample_station) << "iteration " << it << " component " << i;
        }
    }
}

TEST(NetworkArcGraphOracle, DatumGraphCensusOnDsuEqualsTheFrozenUnionBySize) {
    std::mt19937 rng(20261012u);
    long long n_cons = 0, n_multi = 0, n_edges = 0;
    int max_k = 0;
    for (int it = 0; it < 3000; ++it) {
        const int n_sta = uniform(rng, 1, 40);
        std::vector<NetDatumGraphEdge> edges;
        for (const auto& [cons, per_sta] : clusteredVisibility(rng, n_sta))
            for (std::size_t s = 0; s < per_sta.size(); ++s)
                for (const std::string& sat : per_sta[s]) {
                    edges.push_back({static_cast<int>(s), sat});
                    if (chance(rng, 0.05)) edges.push_back({static_cast<int>(s), sat});   // a duplicate edge
                }
        if (chance(rng, 0.2)) edges.push_back({-1, "G01"});   // refused: no station
        if (chance(rng, 0.2)) edges.push_back({0, ""});       // refused: no satellite
        std::shuffle(edges.begin(), edges.end(), rng);
        const std::map<char, NetDatumGraphConsCensus> want = frozen_census::netDatumGraphComponents(edges);
        ASSERT_NO_FATAL_FAILURE(expectSameCensus(GnssProcessor::netDatumGraphComponents(edges), want, it));
        // The census depends on the partition alone: another edge order changes nothing.
        std::shuffle(edges.begin(), edges.end(), rng);
        ASSERT_NO_FATAL_FAILURE(expectSameCensus(GnssProcessor::netDatumGraphComponents(edges), want, it));
        for (const auto& [cons, t] : want) {
            ++n_cons;
            n_multi += t.k > 1 ? 1 : 0;
            max_k = std::max(max_k, t.k);
            n_edges += t.n_edges;
        }
    }
    EXPECT_GT(n_multi, 2000);
    EXPECT_GE(max_k, 4);
    std::cout << "[ NetworkArcGraphOracle ] census: " << n_cons << " constellation censuses, " << n_multi
              << " with k > 1, k up to " << max_k << ", " << n_edges << " edges\n";
}

TEST(NetworkArcGraphOracle, EmitRebaseComponentIdsEqualTheFrozenStringUnionFind) {
    std::mt19937 rng(20261011u);
    const std::map<char, std::vector<std::string>> signals{
        {'G', {"L1C", "L2W", "L5Q"}}, {'E', {"L1C", "L5Q", "L7Q"}}, {'C', {"L2I", "L6I"}}, {'R', {"L1C", "L2C"}}};
    long long n_groups = 0, n_multi = 0, n_keys = 0;
    int max_comps = 0;
    for (int it = 0; it < 3000; ++it) {
        const int n_sta = uniform(rng, 1, 40);
        EmitRebaseEpoch ep;
        ep.stations.resize(static_cast<std::size_t>(n_sta));
        for (FrozenEmitStation& st : ep.stations) st.sm = std::make_shared<FrozenEmitSm>();
        std::set<std::pair<std::string, std::string>> keys_seen;
        std::size_t next_col = 0;
        for (const auto& [cons, per_sta] : clusteredVisibility(rng, n_sta))
            for (std::size_t s = 0; s < per_sta.size(); ++s)
                for (const std::string& sat : per_sta[s])
                    for (const std::string& sig : signals.at(cons))
                        if (chance(rng, 0.8)) {
                            ep.stations[s].sm->ambs[StateKeys::makeAmbiguityKey(sat, sig)] = next_col++;
                            keys_seen.insert({sat, sig});
                        }
        // Keys the walk must refuse: no delimiter, an empty side, an ionosphere-free combination
        // (its phase-bias key is never shared).
        for (FrozenEmitStation& st : ep.stations)
            if (chance(rng, 0.1)) {
                st.sm->ambs["NODELIMITER"] = next_col++;
                st.sm->ambs["_L1C"]        = next_col++;
                st.sm->ambs["G05_"]        = next_col++;
                st.sm->ambs["G07_L1C_L2W"] = next_col++;
            }
        for (int s = 0; s < n_sta; ++s) {
            ep.present.push_back(chance(rng, 0.9));
            ep.est_index_of.push_back(chance(rng, 0.92) ? s : -1);
        }
        for (const auto& [sat, sig] : keys_seen)
            if (chance(rng, 0.9)) ep.shared_phase_keys.insert(StateKeys::makeSatPhaseBiasKey(sat, sig));

        const std::map<std::string, int> want = frozenEmitRebaseComponents(ep);
        EmitRebaseArcs arcs = liveEmitRebaseArcs(ep);
        ASSERT_EQ(networkEmitRebaseComponents(arcs), want) << "iteration " << it;
        // The ids depend on the partition alone: shuffling every group's arcs changes nothing.
        for (auto& [group, group_arcs] : arcs) std::shuffle(group_arcs.begin(), group_arcs.end(), rng);
        ASSERT_EQ(networkEmitRebaseComponents(arcs), want) << "iteration " << it << " (arcs shuffled)";

        std::map<std::pair<char, std::string>, std::set<int>> ids_of_group;
        for (const auto& [key, id] : want) {
            const std::size_t bar = key.find('|');
            ids_of_group[{key[0], key.substr(bar + 1)}].insert(id);
        }
        for (const auto& [group, ids] : ids_of_group) {
            ++n_groups;
            n_multi += ids.size() > 1 ? 1 : 0;
            max_comps = std::max(max_comps, static_cast<int>(ids.size()));
        }
        n_keys += static_cast<long long>(want.size());
    }
    EXPECT_GT(n_multi, 2000);
    EXPECT_GE(max_comps, 4);
    std::cout << "[ NetworkArcGraphOracle ] emit re-base: " << n_groups << " groups, " << n_multi
              << " with more than one component, up to " << max_comps << ", " << n_keys << " keys\n";
}

// =====================================================================================
// The chain election end to end (AGT-4): the live networkElectChainDatum, whose graph,
// components and spanning forest now come from the module, against the whole frozen function.
// =====================================================================================

/// Card NOM step N4a: the receiver phase-bias carry and the receiver-side eq34 parents moved
/// from NetworkCarryState into the station table (network/state/Entities.hpp).  The frozen body
/// keeps their pre-N4a map shapes: this shim converts the tables into those maps at entry, back
/// at exit, and around the one walker call that reads the tables.  Card NOM step N4b moved the
/// eq34 datum receivers into the constellation table's channels, and N4c the eq34 arc elevations
/// into the line-of-sight table; the shim carries their maps too.
struct FrozenStationMaps {
    NetworkRunContext& ctx;
    std::map<std::string, std::string> eq34_datum_rx;                  ///< "<c>|<sigma>" -> receiver (N4b)
    std::map<std::pair<size_t, std::string>, double> eq34_prev_el;     ///< (station, sat) -> elevation (N4c)
    std::map<std::string, std::string> eq34_rx_parent;                 ///< "<name>|<c>|<sigma>" -> sat
    std::map<std::pair<int, std::string>, LocalCarry> rpb_carry;        ///< (station, rpb key)
    std::map<std::string, std::string> anchor_incumbent;               ///< "sat_signal" -> station (N4d)
    std::map<std::string, std::string> eq34_sat_root;                  ///< "sat_signal" -> station (N4e2)
    std::map<net::ArcTuple, LocalCarry> local_carry;                   ///< (station, sat, signal) (N4f1)
    std::map<std::pair<int, std::string>, double> eq34_prev_target;    ///< (station, "sat_signal") (N4f1)
    std::set<std::pair<int, std::string>> eq34_retained_edges;         ///< (station, "sat_signal") (N4f1)
    explicit FrozenStationMaps(NetworkRunContext& c) : ctx(c) { load(); }
    ~FrozenStationMaps() { store(); }
    FrozenStationMaps(const FrozenStationMaps&) = delete;
    FrozenStationMaps& operator=(const FrozenStationMaps&) = delete;
    void load() {
        eq34_rx_parent.clear();
        rpb_carry.clear();
        const net::NetworkEntities& e = ctx.entities;
        eq34_datum_rx = net::channelMap(e.constellations, &net::ChannelState::datum_rx);
        anchor_incumbent = net::sigMap(e.sat_signals, &net::SatSignalState::anchor_incumbent, '_');
        eq34_sat_root = net::sigMap(e.sat_signals, &net::SatSignalState::eq34_sat_root, '_');
        local_carry = net::arcMap(e.arcs, &net::ArcState::amb);
        eq34_prev_target = net::arcAmbMap(e.arcs, &net::ArcState::eq34_prev_target);
        eq34_retained_edges = net::arcAmbSet(e.arcs, &net::ArcState::eq34_retained_edge);
        eq34_prev_el.clear();
        net::forEach(e.los, &net::LosState::eq34_prev_el, [&](const LosId& id, double el) {
            eq34_prev_el[{static_cast<size_t>(id.station.index), net::satKey(id.sat)}] = el;
        });
        net::byStationName(e.stations, e.names, [&](const StationId& id, const net::StationState& row) {
            for (const auto& [family, cell] : row.signals)
                if (cell.eq34_rx_parent)
                    eq34_rx_parent[e.names.names[id.index] + "|" + family] = *cell.eq34_rx_parent;
        });
        net::forEach(e.stations, &net::StationState::signals, &net::StationSignalState::rpb,
                     [&](const StationId& id, const std::string& key, const LocalCarry& lc) {
                         rpb_carry[{id.index, key}] = lc;
                     });
    }
    void store() {
        net::NetworkEntities& e = ctx.entities;
        net::assignSig(e, &net::SatSignalState::anchor_incumbent, anchor_incumbent);
        net::assignSig(e, &net::SatSignalState::eq34_sat_root, eq34_sat_root);
        net::assignArc(e, &net::ArcState::amb, local_carry);
        net::assignArcAmb(e, &net::ArcState::eq34_prev_target, eq34_prev_target);
        net::assignArcAmbFlags(e, &net::ArcState::eq34_retained_edge, eq34_retained_edges);
        net::dropAll(e.los, &net::LosState::eq34_prev_el);
        for (const auto& [key, el] : eq34_prev_el)
            net::putLos(e, static_cast<int>(key.first), key.second, &net::LosState::eq34_prev_el) = el;
        net::dropAll(e.constellations, &net::ConstellationState::channels, &net::ChannelState::datum_rx);
        for (const auto& [key, rx] : eq34_datum_rx) {
            const auto ch = net::channelOfKey(e, key);
            net::put(e.constellations, ch->first, &net::ConstellationState::channels, ch->second,
                     &net::ChannelState::datum_rx) = rx;
        }
        net::dropAll(e.stations, &net::StationState::signals, &net::StationSignalState::eq34_rx_parent);
        net::dropAll(e.stations, &net::StationState::signals, &net::StationSignalState::rpb);
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            const auto p1 = rxkey.rfind('|', rxkey.rfind('|') - 1);
            net::put(e.stations, *net::stationOfName(e, rxkey.substr(0, p1)),
                     &net::StationState::signals,
                     rxkey.substr(p1 + 1), &net::StationSignalState::eq34_rx_parent) = sat;
        }
        for (const auto& [key, lc] : rpb_carry)
            net::put(e.stations, StationId{key.first}, &net::StationState::signals, key.second,
                     &net::StationSignalState::rpb) = lc;
    }
};

// The frozen election compiles against the real run context: nothing in it is stood in but the
// station maps of FrozenStationMaps.
namespace frozen_eq34 {
// ---- BEGIN FROZEN src/core/filter/NetworkChainDatum.cpp lines 99-1057 @ b5592b6 (verbatim) ----
void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            const AnchorHygiene& hyg,
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys,
                            EpochEvents& events) {
auto& prev_carry = ctx.carry.prev_carry;
auto& this_epoch_anchor = events.phase_datum.this_epoch_anchor;
FrozenStationMaps station_maps(ctx);   // card NOM step N4a shim, not in the frozen source
auto& anchor_incumbent = station_maps.anchor_incumbent;   // card NOM step N4d shim
auto& eq34_datum_rx = station_maps.eq34_datum_rx;   // card NOM step N4b shim
auto& eq34_rx_parent = station_maps.eq34_rx_parent;
auto& eq34_sat_root = station_maps.eq34_sat_root;   // card NOM step N4e2 shim
auto& eq34_prev_el = station_maps.eq34_prev_el;   // card NOM step N4c shim
auto& eq34_reref_total = ctx.counters.eq34_reref_total;
auto& eq34_erase_total = ctx.counters.eq34_erase_total;
auto& eq34_shift_total = ctx.counters.eq34_shift_total;
auto& eq34_cpl_sprev_total = ctx.counters.eq34_cpl_sprev_total;
auto& eq34_gap_repin_total = ctx.counters.eq34_gap_repin_total;
auto& eq34_mismatch_epochs = ctx.counters.eq34_mismatch_epochs;
auto& eq34_retained_edges = station_maps.eq34_retained_edges;   // card NOM step N4f1 shim
auto& eq34_prev_target = station_maps.eq34_prev_target;   // card NOM step N4f1 shim
auto& eq34_reref = events.phase_datum.eq34_reref;
auto& local_carry = station_maps.local_carry;   // card NOM step N4f1 shim
auto& rpb_carry = station_maps.rpb_carry;
const bool carry_invariant         = ctx.knobs.carry_invariant;
const bool eq34_shift_on = ctx.knobs.eq34_shift_on;
const bool eq34_longevity = ctx.knobs.eq34_longevity;
const bool eq34_extend_only = ctx.knobs.eq34_extend_only;
const bool eq34_slip_aware = ctx.knobs.eq34_slip_aware;
const bool eq34_seed_round = ctx.knobs.eq34_seed_round;
const bool eq34_seed_rehost = ctx.knobs.eq34_seed_round_rehost;

    // Slip-aware election (Card 3/4): true when this arc slipped this epoch.  The
    // funnel is complete here by the ordering contract (every PASS-A drain, signal
    // integrity and measurement admission run before the election).  The key is
    // (station, ambiguity), the same as the C3c membership test, so the election and
    // the DD-prior build agree on which arcs are slipped.
    auto arcSlipped = [&](std::size_t ri, const std::string& sat,
                          const std::string& sig) -> bool {
        return eq34_slip_aware
            && carry_slipped_ambs.count({static_cast<int>(ri),
                                         StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
    };

    const auto& sat_el      = cnd.sat_el;
    const auto& usable_keys = cnd.usable_keys;
    const auto& union_keys  = cnd.union_keys;
    const auto& cover       = cnd.cover;
    auto cov = [&](System sys, size_t i) -> int {
        auto it = cover.find(rinexLetter(sys)); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };

    // Eq-34 / de Jonge single-receiver-chain datum (Card 3).  Per
    // (constellation,signal) connected component of the bipartite
    // (receiver × satellite) observation graph, elect a datum receiver
    // r1 (its RPB is pinned to 0) and pin the ambiguities on the edges
    // of a spanning tree rooted at r1.  Every between-satellite
    // reference is then an integer combination across the whole
    // component, including satellites r1 does not see, because each
    // chain hop contributes only an integer (the hop receiver's
    // fractional bias is its own estimable RPB state δ_{r,σ} and is not
    // part of the pin).  With full visibility (regional case) this is
    // the Odijk Eq-34 star (CC-R).  The shared set is the ungated union
    // (as in the ensemble datum); the chain pins fix the enlarged
    // (receiver+satellite) gauge.  Per (const,σ) component with n_c
    // stations and m_c sats: (n_c+m_c−1) tree-edge ambiguity pins + 1
    // RPB pin = n_c+m_c, the gauge dimension.  Every RPB-carrying
    // station is a tree vertex, so it gets ≥1 pin and its N_rr stays PD
    // (the rank invariant D3c).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (empty ⇒ no scaling)
        st.pinned_amb_seed_lambda.clear();   // seed-round fold-deferred (empty ⇒ inert)
    }
    this_epoch_anchor.clear();
    // Chain-pin audit (SPEC_synthetic_minimum_network §14 T1): this epoch's per-group
    // declared gauge dimension + shared-side bridge pins, replaced wholesale
    // exactly like the tree maps.  Read after PASS B's serial consume.
    events.phase_datum.eq34_epoch_declared.clear();
    events.phase_datum.eq34_epoch_bridged.clear();
    shared_phase_keys.clear();
    for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
    // This epoch's receiver-side parent edges + per-sat component roots;
    // they replace eq34_rx_parent / eq34_sat_root wholesale after the
    // election, so the maps always describe one epoch's tree.
    std::map<std::string, std::string> new_rx_parent, new_sat_root;
    // This epoch's pin targets (→ eq34_prev_target) and the carry
    // S-transformation shift maps collected by the per-component shift
    // walk: Δb per (sat,σ), Δδ per (station,cons,σ) / (station,rpb_key).
    std::map<std::pair<int, std::string>, double> new_prev_target;
    std::map<std::pair<std::string, std::string>, double> shift_sat;
    std::map<std::tuple<int, char, std::string>, double>  shift_rxg;
    std::map<std::pair<int, std::string>, double>         shift_rpb;

    const std::string eq34_pref_rx = config_.networkPhaseDatumEq34Receiver();
    // Under carry_invariant (Card 4) pin-to-round is off: the frame is re-realized
    // each epoch and continuity is held by the invariant DD carry.  (Seed-round,
    // below, can still round the targets.)
    const bool        eq34_round   = config_.networkPhaseDatumEq34PinRound()
                                  && !carry_invariant;
    // Group the ungated (sat,σ) keys into (constellation,signal)
    // sub-problems.  Recover (sat,σ) from the pbkey value so the
    // "sat_signal" '_' split is unambiguous.  The groups are visited in
    // (RINEX letter, signal) order.
    struct GKey {
        System sys; std::string signal;
        bool operator<(const GKey& o) const {
            const char a = rinexLetter(sys), b = rinexLetter(o.sys);
            return a != b ? a < b : signal < o.signal;
        }
    };
    std::map<GKey, std::set<std::string>>                      grp_sats;
    std::map<GKey, std::map<size_t, std::vector<std::string>>> grp_arcs;  // group -> station -> sats
    std::map<GKey, std::map<std::string, std::vector<size_t>>> grp_satrx; // group -> sat -> stations
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& [amb, pbkey] : usable_keys[i]) {
            if (!union_keys.count(amb)) continue;          // gated out
            const auto p = StateKeys::parseSatPhaseBiasKey(pbkey);
            if (!p || p->satellite.empty()) continue;
            const GKey g{ systemOfSat(p->satellite), p->signal };
            grp_sats[g].insert(p->satellite);
            grp_arcs[g][i].push_back(p->satellite);
            grp_satrx[g][p->satellite].push_back(i);
        }
    }

    // Rank bookkeeping (D3, theory ref §7.1): the recorded pins must
    // equal the gauge dimension Σ_components (n_c + m_c).  A cheap
    // always-on cross-check of the W5 null-space gate; logged below.
    int eq34_n_pins = 0, eq34_gauge_dim = 0;
    // Carry hygiene (D4): (sat,σ) whose chain frame stepped and whose
    // step the shift walk below does not transform (unknown, or nonzero
    // with the carry shift off).  The emitted b_s re-references, so the
    // phase-leveling local carries for it are stale and its D2/C1 shared
    // PB prior belongs to the retired frame.  Collected into the member
    // set (cleared here), erased below before PASS B consumes the local
    // carry, and marginalized at the shared-carry consume.
    eq34_reref.clear();
    for (auto& [g, sats] : grp_sats) {
        // Plain locals rather than the structured bindings g/sats, so the
        // recordEdge lambda below can capture them (C++17 does not allow
        // capturing structured bindings).
        const System      grp_sys  = g.sys;
        const std::string grp_sig  = g.signal;
        // The pin-to-round and the seed-round below apply to the CDMA constellations
        // only: a GLONASS (FDMA) or SBAS group keeps the unrounded target.
        const bool        roundable = grp_sys == System::GPS || grp_sys == System::Galileo
                                   || grp_sys == System::BeiDou || grp_sys == System::QZSS;
        // The group signal's carrier wavelength for that rounding, from the band digit
        // alone as PppUtils::getWavelength reads it (BeiDou digit 1 is B1C there, where
        // SignalId::band reads L1I as B1I); 0 where the signal does not parse or the band
        // is unknown.  No FDMA group is rounded, so the slot is 0.
        double grp_lambda = 0.0;
        if (roundable)
            if (const auto signal = SignalId::fromObsCode(grp_sys, grp_sig))
                grp_lambda = wavelengthM(bandOfDigit(grp_sys, signal->digit), /*glo_slot=*/0);
        const std::string sys_str = systemName(grp_sys);
        const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(sys_str, grp_sig);
        const std::string dr_key  = constellationKey(grp_sys) + "|" + grp_sig;
        auto& arcs  = grp_arcs[g];
        auto& satrx = grp_satrx[g];
        // Graph vertices are named "R<station index>" (a receiver) and "S<sat>" (a
        // satellite); the prefix is the vertex kind, not a constellation letter.
        auto rV = [](size_t i){ return "R" + std::to_string(i); };
        auto sV = [](const std::string& s){ return "S" + s; };
        auto isSatVertex = [](const std::string& v) { return v[0] == 'S'; };

        // Longevity score of arc (station ri, sat) (I4): higher means a longer
        // expected life and so a better new tree edge (fewer future deaths,
        // fewer b_s re-references).  A rising arc (elevation grew since last
        // epoch, tracked in eq34_prev_el) will not set soon, so it is lifted
        // above every setting arc (+90°, one quadrant); within a class the
        // higher elevation wins.  Low-elevation setting arcs cause the churn
        // and rank last.  Called only under eq34_longevity (sat_el and
        // eq34_prev_el are valid here).
        auto arcLife = [&](size_t ri, const std::string& sat) -> double {
            auto ei = sat_el[ri].find(sat);
            if (ei == sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
            const double el = ei->second;
            auto pe = eq34_prev_el.find({ri, sat});
            const bool rising = (pe != eq34_prev_el.end() && el > pe->second + 1e-6);
            // Slip-aware (Card 3/4): an arc that slipped this epoch is the worst new
            // tree edge at any elevation.  As a tree edge it invalidates every
            // fundamental cycle that closes through it, so the whole dependent chord set
            // refloats instead of the one DD the slip affects.  Demote it below every
            // clean arc (the clean band is [0,180]) and keep the ordering within the
            // slipped set, so that if the graph leaves no alternative the completion
            // picks the most durable slipped arc.  The counter increments on every
            // evaluation, including repeated comparator calls during a sort.
            const double life = el + (rising ? 90.0 : 0.0);
            if (arcSlipped(ri, sat, grp_sig)) {
                ++ctx.counters.eq34_slip_demoted;
                return life - 1000.0;
            }
            return life;
        };

        // Sticky-tree seed (spec D2 step 3): the incumbent tree edges
        // (sat-side parents in anchor_incumbent, "sat_signal" → station;
        // receiver-side parents in eq34_rx_parent, "station|c|σ" → sat)
        // that are still live this epoch (station present in this group's
        // arcs and still tracking the satellite).  Both maps are replaced
        // wholesale each election, so together they hold last epoch's tree
        // with each edge in one map.  Any live subset of a tree is a
        // forest, so seeding it cannot close a cycle.
        std::set<std::pair<size_t, std::string>> arc_set;
        std::map<std::string, size_t> name2idx;
        for (const auto& [i, slist] : arcs) {
            name2idx[stations[i].name] = i;
            for (const auto& s : slist) arc_set.insert({i, s});
        }
        std::map<std::string, std::vector<std::string>> inc_adj;
        auto seedEdge = [&](const std::string& sta, const std::string& sat) {
            auto ni = name2idx.find(sta);
            if (ni == name2idx.end() || !arc_set.count({ni->second, sat})) return;
            // Slip-aware (Card 3/4): a slipped incumbent loses its sticky seed.  The
            // incumbent queue drains before the new-edge walk, so a seeded edge is in
            // effect unconditional, and an incumbent that slipped this epoch should
            // leave the tree: every chord whose fundamental cycle runs through it is
            // lost with it.  Dropping the seed does not discard the edge; it moves it to
            // the new-edge walk, where the first pass refuses it (and arcLife ranks it
            // last under longevity), so it is re-admitted only if the component has no
            // other route to the vertex.  The resulting mass refloat is then forced by
            // the topology.
            if (arcSlipped(ni->second, sat, grp_sig)) {
                ++ctx.counters.eq34_slip_deseeded;
                return;
            }
            inc_adj[rV(ni->second)].push_back(sV(sat));
            inc_adj[sV(sat)].push_back(rV(ni->second));
        };
        for (const auto& [amb, sta] : anchor_incumbent) {
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a || systemOfSat(a->satellite) != grp_sys || a->signal != grp_sig) continue;
            seedEdge(sta, a->satellite);
        }
        const std::string rx_suffix =
            "|" + constellationKey(grp_sys) + "|" + grp_sig;
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (rxkey.size() <= rx_suffix.size()
                || rxkey.compare(rxkey.size() - rx_suffix.size(),
                                 rx_suffix.size(), rx_suffix) != 0) continue;
            seedEdge(rxkey.substr(0, rxkey.size() - rx_suffix.size()), sat);
        }

        // --- Union-find over vertices "R<idx>" / "S<sat>". ---
        std::map<std::string, std::string> uf;
        auto find = [&](std::string v) -> std::string {
            auto it = uf.find(v);
            while (it != uf.end() && it->second != v) { v = it->second; it = uf.find(v); }
            return v;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            if (!uf.count(a)) uf[a] = a;
            if (!uf.count(b)) uf[b] = b;
            const std::string ra = find(a), rb = find(b);
            if (ra != rb) uf[ra] = rb;
        };
        for (const auto& [i, slist] : arcs)
            for (const auto& s : slist) unite(rV(i), sV(s));

        std::map<std::string, std::vector<size_t>>      comp_rx;
        std::map<std::string, std::vector<std::string>> comp_sat;
        for (const auto& [i, slist] : arcs)  comp_rx[find(rV(i))].push_back(i);
        for (const auto& s : sats)           comp_sat[find(sV(s))].push_back(s);

        std::string best_datum_rx; size_t best_comp = 0;
        for (auto& [root, rx_list] : comp_rx) {
            std::sort(rx_list.begin(), rx_list.end());   // determinism

            // Elect r1: config pref → per-component sticky (sat-root
            // vote) → group incumbent → highest coverage.
            size_t r1 = static_cast<size_t>(-1);
            if (!eq34_pref_rx.empty())
                for (size_t i : rx_list)
                    if (stations[i].name == eq34_pref_rx) { r1 = i; break; }
            // Per-component stickiness: the component's sats remember
            // last epoch's root (eq34_sat_root); re-elect the station most
            // of them remember, if it is still in this component.  The
            // group-level eq34_datum_rx slot below remembers only the
            // largest component's r1, so without this vote small fragments
            // would re-elect by coverage every epoch, each change a full
            // fragment re-level.  Vote ties break by station name.
            if (r1 == static_cast<size_t>(-1)) {
                std::map<std::string, int> root_votes;
                for (const auto& s : comp_sat[root]) {
                    auto rt = eq34_sat_root.find(StateKeys::makeAmbiguityKey(s, grp_sig));
                    if (rt != eq34_sat_root.end()) ++root_votes[rt->second];
                }
                int best_v = 0; std::string best_name;
                for (const auto& [nm, v] : root_votes)
                    if (v > best_v || (v == best_v && nm < best_name)) {
                        best_v = v; best_name = nm;
                    }
                if (best_v > 0)
                    for (size_t i : rx_list)
                        if (stations[i].name == best_name) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                auto it = eq34_datum_rx.find(dr_key);
                if (it != eq34_datum_rx.end())
                    for (size_t i : rx_list)
                        if (stations[i].name == it->second) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                int best = -1;
                for (size_t i : rx_list) {
                    const int cv = cov(grp_sys, i);
                    if (cv > best
                        || (cv == best && stations[i].name < stations[r1].name)) {
                        best = cv; r1 = i;
                    }
                }
            }
            if (r1 == static_cast<size_t>(-1)) continue;   // empty (unreachable)

            if (rx_list.size() > best_comp) {
                best_comp = rx_list.size();
                best_datum_rx = stations[r1].name;
            }

            // r1's RPB pin: the receiver half of the gauge (δ_{r1,σ}=0).
            stations[r1].pinned_rpb_keys.insert(rpb_key);
            ++eq34_n_pins;
            eq34_gauge_dim += static_cast<int>(rx_list.size()
                            + comp_sat[root].size());   // n_c + m_c
            events.phase_datum.eq34_epoch_declared[dr_key] +=
                static_cast<int>(rx_list.size() + comp_sat[root].size());

            // Sticky spanning tree (two-queue BFS from r1).  The
            // incumbent queue drains completely before any new graph edge
            // is explored, so a surviving old subtree, even one detached
            // upstream, is re-attached through its own old edges (one new
            // edge per detached fragment) and is not re-wired vertex by
            // vertex: a tree-edge pin moves only when its arc died (or,
            // under slip-aware election, slipped).  One ambiguity pin per
            // tree edge; each attachment records (child, parent,
            // edge-changed) for the dirty walk below.
            //
            // recordEdge records one tree edge (r,s): pin ambiguity z^s_r
            // at station r.  Target = 0 on a first-sighting edge (unless
            // seed-round applies, below); on a re-pin of an already-lit
            // (sat,σ) (pin-to-round), the |·|-minimizing integer
            // λ·round(carried float/λ), so the emitted-bias frame step is
            // ≈ 0 cyc.  sat_is_child selects which endpoint the edge
            // attaches: the sat (parent edge → this_epoch_anchor) or the
            // receiver (parent edge → new_rx_parent).
            std::set<std::string> visited;
            std::deque<std::string> inc_q, new_q;
            // (child, parent, pinmove, known, frame_shiftable): pinmove =
            // this pin's exact displacement of its pinned float vs last
            // epoch's frame; known = false where the step is not computable
            // (re-pin without a carry; root handover, a common fractional
            // step that no integer transform can express);
            // frame_shiftable (INC-2) = this step came from the new host's
            // own state and may be S-transformed even with the global
            // carry-shift knob off (always false unless INC-2 is armed).
            std::vector<std::tuple<std::string, std::string, double, bool, bool>>
                order;
            auto recordEdge = [&](size_t ri, const std::string& sat,
                                  bool sat_is_child) {
                const std::string amb = StateKeys::makeAmbiguityKey(sat, grp_sig);
                // Slip-aware (Card 3/4): this arc is becoming a tree edge.  If it slipped
                // this epoch, the election could not avoid it (the component has no other
                // route to the vertex), and every cycle through it refloats.  Counted
                // separately so that "deseeded > 0" is not read as "no slipped arc ended
                // up in the tree".
                if (arcSlipped(ri, sat, grp_sig)) ++ctx.counters.eq34_slip_readmitted;
                // Gap re-pin metric (I2): this edge was retained across a gap
                // (its carry outlived the row absence) and is resuming now.
                // The incumbent seed made it a recognized edge, so the
                // pin-to-round below re-pins it to its pre-gap integer
                // (step ~0) instead of the first-sighting 0 a wholesale
                // replace would have used.  Counts these re-pins.
                if (eq34_extend_only
                    && eq34_retained_edges.count({static_cast<int>(ri), amb}))
                    ++eq34_gap_repin_total;
                // Pin target: 0 on first sighting; else λ·round(carry/λ)
                // at the (possibly new) edge station.  carry_val also
                // feeds the pinmove computation below.
                double target = 0.0;
                double carry_val = std::numeric_limits<double>::quiet_NaN();
                if (eq34_round && anchor_incumbent.count(amb)) {
                    auto lc = local_carry.find(std::make_tuple(
                        static_cast<int>(ri), sat, grp_sig));
                    if (lc != local_carry.end() && roundable) {
                        const double lam = grp_lambda;
                        if (std::isfinite(lam) && lam > 0.0) {
                            carry_val = lc->second.value_abs;
                            target = lam * std::round(carry_val / lam);
                        }
                    }
                }
                // Seed-round (docs/SPEC_eq34_seed_round.md) applies when the
                // carry-round above did not engage (first sighting; incumbent whose
                // carry is dead; or every epoch under carry_invariant, where
                // eq34_round is off by the Card-4 design).  The target would then be
                // the cold absolute 0, which puts the seeded phase−code offset into
                // the datum frame (km-scale on the synthetic fixture; SPEC §12.8).
                // Instead, round the value the state holds, so |pin innovation| ≤ λ/2
                // by construction.  When the state does not exist yet (cold start or
                // new arc; PASS B's pre-pass allocates it), defer the rounding to the
                // fold via pinned_amb_seed_lambda; the realized target flows back into
                // eq34_prev_target through the serial consume.  `known` is cleared
                // below for deferred pins.
                bool seed_deferred = false, seed_rounded = false;
                // The new host's pre-pin float (INC-2, SPEC §8.5).  Finite only when
                // the state exists and INC-2 is armed; on a re-host it is this pin's
                // exact frame-step baseline (the pin moves the float from here to
                // `target`, so Δb_s = −(target − seed_x) exactly).
                double seed_x = std::numeric_limits<double>::quiet_NaN();
                bool   frame_shiftable = false;
                auto ambStateValue = [&]() -> double {
                    const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                    if (sidx < 0) return std::numeric_limits<double>::quiet_NaN();
                    return stations[ri].sm->getStateVector()(sidx);
                };
                if (eq34_seed_round && !std::isfinite(carry_val) && roundable) {
                    // Freeze after first realization (INC-1, SPEC §8.5): a frame is
                    // realized once.  Reuse this (station, amb)'s previously realized
                    // target unchanged (on the same host: pinmove 0, no step, no
                    // reref) and round only on a first realization.  Re-rounding
                    // every epoch ratchets the invariant carry (§13.7).
                    auto ptf = eq34_prev_target.find({static_cast<int>(ri), amb});
                    if (ptf != eq34_prev_target.end()) {
                        target = ptf->second;
                        seed_rounded = true;
                        ++ctx.counters.seed_round_frozen;
                        // A frozen target can still land on a re-hosted edge (the
                        // entry survived through the I2 retention or the fold
                        // feedback), and that step is equally computable, so read the
                        // pre-pin float here too (INC-2).  Only when armed, so with
                        // INC-2 off the freeze branch keeps its single map lookup.
                        if (eq34_seed_rehost) seed_x = ambStateValue();
                    } else {
                    const double lam = grp_lambda;
                    if (std::isfinite(lam) && lam > 0.0) {
                        const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                        if (sidx >= 0) {
                            const double x =
                                stations[ri].sm->getStateVector()(sidx);
                            if (std::isfinite(x)) {
                                if (eq34_seed_rehost) seed_x = x;   // re-host step baseline (INC-2)
                                target = lam * std::round(x / lam);
                                seed_rounded = true;
                                ++ctx.counters.seed_round_pins;
                                const double y = std::abs(target - x);
                                if (y > ctx.counters.seed_round_max_abs_y_m)
                                    ctx.counters.seed_round_max_abs_y_m = y;
                            }
                        } else {
                            stations[ri].pinned_amb_seed_lambda[amb] = lam;
                            seed_deferred = true;
                        }
                    }
                    }   // end of the freeze else-arm (INC-1)
                }
                double pinmove = 0.0;
                bool known = true;
                bool edge_changed = false;
                if (sat_is_child) {
                    auto ai = anchor_incumbent.find(amb);
                    edge_changed = (ai != anchor_incumbent.end()
                                    && ai->second != stations[ri].name);
                    this_epoch_anchor[amb] = stations[ri].name;
                    // Root handover: the component frame steps by the
                    // datum receivers' fractional δ difference, which is
                    // not an integer and not computable here, so unknown.
                    auto rt = eq34_sat_root.find(amb);
                    if (rt != eq34_sat_root.end()
                        && rt->second != stations[r1].name) known = false;
                    new_sat_root[amb] = stations[r1].name;
                } else {
                    const std::string rxkey = stations[ri].name + rx_suffix;
                    auto rp = eq34_rx_parent.find(rxkey);
                    edge_changed = (rp != eq34_rx_parent.end()
                                    && rp->second != sat);
                    new_rx_parent[rxkey] = sat;
                }
                if (edge_changed) {
                    // Re-pin at a new station: the float there sat at
                    // carry_val and the pin drives it to target, an exact,
                    // known displacement iff the carry exists.
                    if (std::isfinite(carry_val)) pinmove = target - carry_val;
                    // No carry, but under seed-round (INC-2, SPEC §8.5) the
                    // station's own ambiguity state is the pre-pin value, and it
                    // is exact: it is the estimator's own number, not a leveling
                    // carry that can drift (the I1c mechanism that made carry_shift
                    // harmful in the field does not apply).  The frame step is
                    // therefore computable, and the edge stays shiftable even with
                    // the global carry-shift knob off.  That keeps the (sat,σ) out
                    // of eq34_reref, so its carried PB prior is folded
                    // (S-transformed) instead of marginalized for the epoch.
                    else if (eq34_seed_rehost && std::isfinite(seed_x)) {
                        pinmove = target - seed_x;
                        frame_shiftable = true;
                        ++ctx.counters.seed_round_rehost_framed;
                        const double st_abs = std::abs(pinmove);
                        if (st_abs > ctx.counters.seed_round_rehost_max_step_m)
                            ctx.counters.seed_round_rehost_max_step_m = st_abs;
                    } else {
                        known = false;
                        // Counted only when armed, so that with INC-2 off the
                        // run-end block stays byte-identical.
                        if (eq34_seed_rehost) ++ctx.counters.seed_round_rehost_cold;
                    }
                } else {
                    // Same station: its float was hard-pinned at last
                    // epoch's target, so the displacement is exactly the
                    // target difference (no carry noise involved).
                    auto pt = eq34_prev_target.find(
                        {static_cast<int>(ri), amb});
                    pinmove = target
                        - (pt != eq34_prev_target.end() ? pt->second : 0.0);
                }
                // Seed-round bookkeeping: a deferred pin's target is decided at
                // the fold, so no frame step is computable here (the serial
                // consume feeds the realized target back as next epoch's
                // baseline).  An election-side seed target that moved since last
                // epoch (the float crossed a half-integer, or a dropped station
                // lost last epoch's deferred feedback) is a frame re-realization,
                // not a transformable shift: it goes through the validated reref
                // hygiene, not the v2-shift walk.
                // INC-2 exempts the framed re-host: its step is an exactly
                // computed frame move, not a same-station target change, so it
                // takes the shift walk (and its own counter) instead of the reref
                // hygiene.
                if (seed_deferred) known = false;
                else if (seed_rounded && pinmove != 0.0 && !frame_shiftable) {
                    known = false;
                    ++ctx.counters.seed_round_target_steps;
                }
                new_prev_target[{static_cast<int>(ri), amb}] = target;
                stations[ri].pinned_amb_targets[amb] = target;
                ++eq34_n_pins;
                // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md): this
                // pin's host arc is (ri, amb) whichever endpoint the edge attaches.
                // Count the reattach (the sat-side host moved; NetworkSolPrep traces
                // this event as PHASE_DATUM/eq34_reattach) and, when the eligible
                // pass could not avoid a fresh host, give the pin fold an INC-3 σ
                // multiplier so the datum row is soft enough for the data to pull
                // the cold ambiguity instead of re-levelling the component onto its
                // code-seeded value.  Inert (no map entry, no counter) when the card
                // is off.
                if (hyg.active) {
                    if (edge_changed && sat_is_child) ++ctx.counters.hyg.reattaches;
                    const double sc = hyg.pinSigmaScale(ri, amb);
                    if (sc > 1.0) {
                        stations[ri].pinned_amb_sigma_scale[amb] = sc;
                        ++ctx.counters.hyg.fresh_pin_ramps;
                    }
                }
                order.emplace_back(sat_is_child ? sV(sat) : rV(ri),
                                   sat_is_child ? rV(ri) : sV(sat),
                                   pinmove, known, frame_shiftable);
            };
            auto attach = [&](const std::string& v) {
                visited.insert(v);
                inc_q.push_back(v);
                new_q.push_back(v);
            };
            attach(rV(r1));
            // eq34 reattach hygiene INC-2: a bridged satellite's gauge is realized on
            // the shared side, by a soft pseudo-obs row holding b_s at its last solved
            // level (networkInjectPinBridgePrior).  It enters the walk already
            // grounded: no parent edge, no ambiguity pin, and the walk may pass
            // through it to reach receivers.  The spanning structure is then a forest
            // rooted at {r1} ∪ {bridged sats}, and the rank arithmetic still closes:
            // 1 RPB pin + B bridge rows + (n_c + m_c − 1 − B) tree edges = n_c + m_c.
            // Bridge rows are counted as pins for that cross-check.
            std::vector<std::string> bridged_roots;
            if (hyg.active && !hyg.bridged.empty()) {
                for (const auto& s : comp_sat[root]) {
                    if (!hyg.isBridged(StateKeys::makeAmbiguityKey(s, grp_sig))) continue;
                    if (visited.count(sV(s))) continue;
                    attach(sV(s));
                    bridged_roots.push_back(sV(s));
                    ++eq34_n_pins;
                    // Audit: a bridge pin is realized on the shared side, so the
                    // station-side fold expectation is declared − bridged.
                    ++events.phase_datum.eq34_epoch_bridged[dr_key];
                }
            }
            // A new tree edge is grown only onto an arc whose ambiguity has converged
            // (age ≥ eq34_anchor_min_arc_ep; INC-1).  The walk runs twice: the
            // first pass admits eligible edges only, so a mature host wins over a
            // fresh one regardless of the I4/longevity rank order; if that leaves
            // vertices unreachable (their only connection to the component runs
            // through a fresh arc), the second pass admits everything and counts
            // each unavoidable cold pin.  The incumbent queue is not filtered:
            // keeping the incumbent is not a reattach, and blocking it would cause
            // the handover this card is meant to prevent.
            const std::size_t comp_vertices =
                rx_list.size() + comp_sat[root].size();
            bool allow_cold = !hyg.active || hyg.min_arc_ep <= 0;
            // Slip-aware (Card 3/4): a slipped arc is refused in the first pass on the
            // same terms as a cold host, and admitted in the second.  The refusal is
            // here and not only in the arcLife ranking because arcLife is read only
            // under the longevity election; relying on it would leave slip avoidance
            // inactive wherever longevity is off (its default).
            // Avoiding a slipped tree edge is a correctness preference, not a longevity
            // heuristic, so it has its own second-chance flag.  Without this refusal,
            // de-seeding an incumbent (above) would be worse than no action: the walk
            // would hand the edge straight back and pay a datum handover for it.
            bool allow_slipped = !eq34_slip_aware;
            auto newEdgeOk = [&](size_t ri, const std::string& sat) {
                if (!allow_slipped && arcSlipped(ri, sat, grp_sig)) return false;
                if (hyg.eligibleHost(ri, StateKeys::makeAmbiguityKey(sat, grp_sig))) return true;
                if (!allow_cold) return false;
                ++ctx.counters.hyg.cold_fallback_pins;
                return true;
            };
            for (int pass = 0; pass < 2; ++pass) {
                if (pass == 1) {
                    // Everything reachable through eligible edges is in; anything
                    // left needs the cold fallback.  `visited` is an ordered set, so
                    // the rebuilt frontier is deterministic.  Both refusals share the
                    // pass: a vertex still unreached may be behind a cold host, a
                    // slipped arc, or both, and there is no third pass to separate them.
                    if ((allow_cold && allow_slipped) || visited.size() >= comp_vertices)
                        break;
                    allow_cold = true;
                    allow_slipped = true;
                    for (const auto& v : visited) new_q.push_back(v);
                }
                while (!inc_q.empty() || !new_q.empty()) {
                if (!inc_q.empty()) {
                    const std::string v = inc_q.front(); inc_q.pop_front();
                    auto ia = inc_adj.find(v);
                    if (ia == inc_adj.end()) continue;
                    for (const auto& u : ia->second) {
                        if (visited.count(u)) continue;
                        if (isSatVertex(u))
                            recordEdge(static_cast<size_t>(
                                           std::stoul(v.substr(1))),
                                       u.substr(1), /*sat_is_child=*/true);
                        else
                            recordEdge(static_cast<size_t>(
                                           std::stoul(u.substr(1))),
                                       v.substr(1), /*sat_is_child=*/false);
                        attach(u);
                    }
                } else {
                    const std::string v = new_q.front(); new_q.pop_front();
                    if (!isSatVertex(v)) {
                        const size_t ri =
                            static_cast<size_t>(std::stoul(v.substr(1)));
                        std::vector<std::string> ss = arcs[ri];
                        // Longevity (I4): attach this receiver's
                        // longest-lived unvisited sats first (rising or
                        // high-elevation), so the new edge is picked to
                        // outlive the epoch.  Off: legacy sat-name order
                        // (byte-identical).
                        if (eq34_longevity)
                            std::sort(ss.begin(), ss.end(),
                                      [&](const std::string& a, const std::string& b) {
                                          const double la = arcLife(ri, a),
                                                       lb = arcLife(ri, b);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(ss.begin(), ss.end());
                        for (const auto& s : ss) {
                            if (visited.count(sV(s))) continue;
                            if (!newEdgeOk(ri, s)) continue;   // pass 1 refuses a fresh host or slipped arc (INC-1)
                            recordEdge(ri, s, /*sat_is_child=*/true);
                            attach(sV(s));
                        }
                    } else {
                        const std::string s = v.substr(1);
                        std::vector<size_t> rr = satrx[s];
                        // Longevity (I4): attach this sat through its most
                        // durable unvisited tracker first: highest
                        // per-constellation coverage (the robust receiver,
                        // spec D2 step 3), then the arc's own longevity at
                        // that station.
                        // Off: legacy station-index order (byte-identical).
                        if (eq34_longevity)
                            std::sort(rr.begin(), rr.end(),
                                      [&](size_t a, size_t b) {
                                          const int ca = cov(grp_sys, a),
                                                    cb = cov(grp_sys, b);
                                          if (ca != cb) return ca > cb;
                                          const double la = arcLife(a, s),
                                                       lb = arcLife(b, s);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(rr.begin(), rr.end());
                        for (size_t ri : rr) {
                            if (visited.count(rV(ri))) continue;
                            if (!newEdgeOk(ri, s)) continue;   // pass 1 refuses a fresh host or slipped arc (INC-1)
                            recordEdge(ri, s, /*sat_is_child=*/false);
                            attach(rV(ri));
                        }
                    }
                }
                }
            }
            // Shift walk: the carry S-transformation (spec D4 "v2 =
            // SHIFT", realized at election, where the step is known, so
            // no emit-site plumbing is needed).  A pin displacement
            // propagates down the chain with alternating sign:
            //   Δb_s = −Δδ_parent − pinmove(edge)
            //   Δδ_r = −Δb_parent − pinmove(edge)
            // so the whole subtree's frame move is exact and every
            // carried quantity can be transformed into the new basis
            // (variances untouched, no information lost) instead of
            // erased.  Erase/skip (eq34_reref) remains only where the
            // step is not transformed: a re-pin with no carry, a root
            // handover (common fractional), and, with the shift off,
            // any nonzero step (see the loop).  `order` is in attachment
            // order, parents strictly first, so one forward pass works.
            std::map<std::string, double> vshift;
            std::map<std::string, bool>   vknown;
            vshift[rV(r1)] = 0.0;
            vknown[rV(r1)] = true;
            // A bridged satellite (INC-2) is a further root whose frame does not move
            // (the bridge row holds b_s at its previous value), so it seeds the walk
            // with a known zero shift.  The subtree below it therefore keeps its
            // carries instead of being re-referenced (no eq34_reref entry, no
            // local-carry erase); this is the purpose of the bridge.
            for (const auto& v : bridged_roots) {
                vshift[v] = 0.0;
                vknown[v] = true;
            }
            for (const auto& [v, p, pinmove, known, shiftable] : order) {
                // Shift off (default): any nonzero pin displacement is
                // treated as unknown, so the erase/skip hygiene handles it
                // (the validated B4/B7-class behavior).  Shift on: exact
                // transforms flow to the apply block below.  INC-2 adds a
                // third case for one edge class only, a seed-round re-host
                // whose step came from the new host's own state, and it
                // composes with the walk unchanged: the nonzero vshift it
                // seeds propagates through the (pinmove == 0) frozen edges
                // below it as it would in a shift-on run.
                const bool   k = known && vknown[p]
                              && (eq34_shift_on || pinmove == 0.0 || shiftable);
                const double s = k ? (-vshift[p] - pinmove) : 0.0;
                vshift[v] = s;
                vknown[v] = k;
                if (isSatVertex(v)) {
                    const std::string sat = v.substr(1);
                    if (!k)          eq34_reref.insert({sat, grp_sig});
                    else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;
                } else {
                    const int idx =
                        static_cast<int>(std::stoul(v.substr(1)));
                    if (!k) {
                        // Unknown frame step: the carried δ̂_{r,σ}
                        // belongs to an unrecoverable retired frame.
                        // Card PRM phase 3a: counted only when an entry was erased.
                        if (rpb_carry.erase({idx, rpb_key}) != 0)
                            prmRespond(ctx.counters.prm_resp, ResponseSite::RpbEq34Erase);
                    } else if (s != 0.0) {
                        shift_rxg[std::make_tuple(idx, rinexLetter(grp_sys),
                                                  grp_sig)] = s;
                        shift_rpb[{idx, rpb_key}] = s;
                    }
                }
            }
        }
        if (!best_datum_rx.empty()) eq34_datum_rx[dr_key] = best_datum_rx;
    }
    // Rank cross-check (D3): recorded pins == Σ (n_c + m_c).  A mismatch
    // means a component was mis-pinned before the W5 gate ever runs.
    if (eq34_n_pins != eq34_gauge_dim) {
        ++eq34_mismatch_epochs;
        log->warn("Network[eq34]: pin/gauge mismatch — recorded {} pins vs "
                  "gauge dimension {} (Σ n_c+m_c over components); the W5 "
                  "null-space gate will catch an under/over-constrained datum.",
                  eq34_n_pins, eq34_gauge_dim);
    } else
        log->debug("Network[eq34]: chain datum — {} pins == gauge dim {} "
                   "(1 RPB + tree edges per component).",
                   eq34_n_pins, eq34_gauge_dim);
    // Carry hygiene (D4): erase the phase-leveling local carries for any
    // re-referenced (sat,σ) at every station, before PASS B consumes the
    // carry.  The b_s frame stepped by an amount the shift walk did not
    // transform, so the carried floats are stale.  They refloat next epoch
    // (safe: the realized wire step is usually 0 under pin-to-round, and
    // erasing an already-integer carry costs one epoch of continuity).
    if (!eq34_reref.empty()) {
        eq34_reref_total += static_cast<long long>(eq34_reref.size());
        // The first post-resume epoch's share of that total, the quantity the
        // datum-continuity card is graded on (RS-3, SPEC_event_harness §21.7).
        // Measure-only; the latch is cleared at the emit site, so a withheld first
        // epoch folds into the window.
        if (ctx.carry.resume_datum_first_ep)
            ctx.counters.n_resume_datum_first_reref +=
                static_cast<long long>(eq34_reref.size());
        int n_erased = 0;
        for (auto it = local_carry.begin(); it != local_carry.end();) {
            const auto& key = it->first;   // (station idx, sat, signal)
            if (eq34_reref.count({std::get<1>(key), std::get<2>(key)})) {
                it = local_carry.erase(it);
                ++n_erased;
                ++ctx.counters.n_cr_eq34_reref;   // carry-retirement census: frame step
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbEq34Erase);   // card PRM phase 3a
            } else {
                ++it;
            }
        }
        eq34_erase_total += n_erased;
        if (n_erased > 0)
            log->debug("Network[eq34]: {} local ambiguity carry(ies) erased for "
                       "{} re-referenced (sat,σ) — refloat next epoch.",
                       n_erased, eq34_reref.size());
    }
    // Carry S-transformation apply: shift every carried quantity into
    // this epoch's frame (Δz = −Δb_s − Δδ_r on the local ambiguity
    // carries; Δδ_r on the RPB carries; Δb_s on the shared PB carried
    // priors).  Variances untouched: this is an exact basis change,
    // not a refloat.  Runs before PASS B consumes any carry.
    //
    // Coupling references move with the carries (docs/SPEC_card1_coupling_carry.md
    // §9): D4 shifts `lc.value_abs` below and also every coupling `s_prev` on a
    // shifted PB column (SPEC_card1_coupling_carry §14).  A Card-1 coupling row's RHS is
    // y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j , and `CoupledShared::s_prev` and
    // `CarryPrior::value` hold the same number for the same identity (both are
    // `x_s(j)` at harvest: the NetworkRunContext.cpp s_prev assignment and the
    // prev_carry stash).  For a gauge-invariant row the two moves cancel under a
    // known frame step; shifting one reference without the other injects the frame
    // step as row bias.  The same rule holds at Relevel.cpp's Card-6 eph
    // cutover: the row's functional u = r + Mᵀs is frame-consistent only if all
    // carried references move together.
    //
    // The coupling walk sits outside the per-carry `d != 0` guard, because a carry
    // whose own (sat,σ) did not move can still hold coupling terms on other
    // satellites' shifted columns.  Only `shift_sat` participates: SharedParamKind
    // spans satellite-side + SH columns, so the receiver-side maps
    // (`shift_rxg`/`shift_rpb`) have no coupling image.  The D4 erase above does not
    // cover these keys: the erase set (`eq34_reref`) and the shift set (`shift_sat`)
    // are mutually exclusive by construction at the population site, so the shifted
    // keys are exactly those whose carries survive.  With coupling off the stashes
    // are empty and the walk leaves the output byte-identical.
    // The walk is the traversal of carried references (card PRM phase 2): the ambiguities,
    // the receiver phase biases and the satellite phase biases, then the s_prev of the
    // phase-bias coupling terms in the ambiguity, receiver phase-bias and wet-delay carries.
    if (!shift_sat.empty() || !shift_rxg.empty()) {
        CarrySiteCounts prm_cc;
        long long n_shifted = 0, n_cpl = 0;
        station_maps.store();   // card NOM step N4a shim, not in the frozen source
        eq34ShiftCarry(ctx.carry, ctx.entities, shift_sat, shift_rxg, shift_rpb, n_shifted, n_cpl, &prm_cc,
                       ctx.knobs.rebase_complete);
        station_maps.load();    // card NOM step N4a shim
        if (!shift_sat.empty()) eq34_cpl_sprev_total += n_cpl;
        eq34_shift_total += n_shifted;
        // Card PRM phase 3b step 2: the held emissions of the shifted families, left in the old
        // frame (PbHold is exempt from the shift).  Measure-only.
        net::forEach(ctx.entities.sat_signals, &net::SatSignalState::pb_hold,   // N4d shim
                     [&](const SatSignalId& id, const net::PbHoldKey&) {
            const auto sh = shift_sat.find({net::satKey(id.sat), net::signalKey(id.signal)});
            if (sh != shift_sat.end() && sh->second != 0.0)
                ++ctx.counters.prm_pbhold_unmoved_at_shift;
        });
        ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::Eq34Shift)];
        carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::Eq34Shift, prm_cc);
    }
    // Sticky baselines for next epoch: this epoch's tree becomes the
    // incumbent forest (sat parents + receiver parents, replaced
    // wholesale so the pair of maps always describes one tree), each
    // sat remembers its component root for the r1-handover check, and
    // each pin remembers its target for the same-station pinmove.
    // anchor_incumbent doubles as the was-lit gate for pin-to-round
    // (every tree sat has exactly one parent edge).
    //
    // I2 (de Jonge §3.5 extend-only): before the wholesale swap, merge
    // back the identity of any tree edge that was not seen this epoch but
    // whose phase-leveling memory (local_carry) is still alive: its
    // sat-side parent (this_epoch_anchor), receiver-side parent
    // (new_rx_parent), component root (new_sat_root) and pinned integer
    // (new_prev_target).  A momentary row gap then does not count as a
    // dead arc.  The edge sits inert in the incumbent maps (never seeded,
    // because the seed admits live arcs only, so the spanning-forest
    // invariant holds), and in the epoch its arc resumes it is a
    // recognized incumbent whose pin-to-round re-pins to the same integer
    // (emitted-bias step ≈ 0) instead of re-leveling from a first-sighting
    // 0.  local_carry liveness serves as the age stamp: it is gap-pruned at
    // phase_leveling_max_gap_s, and a genuine reroute or handover erases
    // it, so an edge past that horizon (or re-referenced) is dropped and
    // refloats as a first sighting, by which point the peer carries are
    // gone too.  Keys are receiver-indexed pins {receiver idx,
    // "sat_signal"}; the two parent maps hold each edge in one direction.
    if (eq34_extend_only) {
        std::map<std::string, size_t> gname2idx;
        for (size_t i = 0; i < stations.size(); ++i)
            gname2idx[stations[i].name] = i;
        auto carryAlive = [&](size_t idx, const std::string& sat,
                              const std::string& sig) -> bool {
            return local_carry.count(std::make_tuple(
                static_cast<int>(idx), sat, sig)) != 0;
        };
        eq34_retained_edges.clear();
        // Sat-side parents (amb → parent station) + their component roots.
        for (const auto& [amb, sta] : anchor_incumbent) {
            if (this_epoch_anchor.count(amb)) continue;    // re-seen live
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a) continue;
            auto gi = gname2idx.find(sta);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, a->satellite, a->signal)) continue;  // dead ⇒ drop
            this_epoch_anchor.emplace(amb, sta);
            eq34_retained_edges.emplace(static_cast<int>(gi->second), amb);
            auto sr = eq34_sat_root.find(amb);
            if (sr != eq34_sat_root.end())
                new_sat_root.emplace(amb, sr->second);
        }
        // Receiver-side parents ("<name>|<c>|<σ>" → sat).
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (new_rx_parent.count(rxkey)) continue;
            const auto p2 = rxkey.rfind('|');
            if (p2 == std::string::npos || p2 == 0) continue;
            const auto p1 = rxkey.rfind('|', p2 - 1);
            if (p1 == std::string::npos) continue;
            const std::string name = rxkey.substr(0, p1);
            const std::string sig  = rxkey.substr(p2 + 1);
            auto gi = gname2idx.find(name);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, sat, sig)) continue;
            new_rx_parent.emplace(rxkey, sat);
            eq34_retained_edges.emplace(static_cast<int>(gi->second),
                                        StateKeys::makeAmbiguityKey(sat, sig));
        }
        // Pinned integers ({receiver idx, "sat_signal"} → target).
        for (const auto& [k, tgt] : eq34_prev_target) {
            if (new_prev_target.count(k)) continue;
            const auto a = StateKeys::parseAmbiguityKey(k.second);
            if (!a) continue;
            if (!carryAlive(static_cast<size_t>(k.first), a->satellite, a->signal)) continue;
            new_prev_target.emplace(k, tgt);
        }
    }
    anchor_incumbent = this_epoch_anchor;
    eq34_rx_parent   = std::move(new_rx_parent);
    eq34_sat_root    = std::move(new_sat_root);
    eq34_prev_target = std::move(new_prev_target);
    // Snapshot this epoch's arc elevations (I4) so next epoch's arcLife
    // can tell a rising arc from a setting one.  Rebuilt wholesale (a
    // pruned arc drops out); skipped when the longevity election is
    // off, so the map stays empty on the B9 baseline.
    if (eq34_longevity) {
        eq34_prev_el.clear();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& [sat, el] : sat_el[i])
                eq34_prev_el[{i, sat}] = el;
        }
    }
}
// ---- END FROZEN ----
}  // namespace frozen_eq34

/// The knobs of one chain lineage; the frozen and the live election read the same loaded config.
struct ChainKnobs {
    bool        longevity{false};
    bool        slip_aware{false};
    bool        extend_only{false};
    bool        carry_shift{false};
    bool        pin_round{true};
    std::string preferred;   ///< network.phase_datum_eq34_receiver ("" = none).
};

/// Load a network config on the chain datum with the given knobs, through the production loader.
std::unique_ptr<GnssConfig> loadChainConfig(LogManager& lm, const ChainKnobs& k, int tag) {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    const std::string json = std::string("{\n")
        + "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
        + "                  \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
        + "  \"network\": {\n"
        + "    \"clock_datum_station\": \"AAAA\",\n"
        + "    \"phase_datum_eq34\": true,\n"
        + "    \"phase_datum_eq34_longevity_election\": " + b(k.longevity) + ",\n"
        + "    \"phase_datum_eq34_slip_aware_election\": " + b(k.slip_aware) + ",\n"
        + "    \"phase_datum_eq34_extend_only\": " + b(k.extend_only) + ",\n"
        + "    \"phase_datum_eq34_carry_shift\": " + b(k.carry_shift) + ",\n"
        + "    \"phase_datum_eq34_pin_round\": " + b(k.pin_round) + ",\n"
        + "    \"phase_datum_eq34_receiver\": \"" + k.preferred + "\",\n"
        + "    \"stations\": [\n"
        + "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        + "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        + "    ]\n"
        + "  }\n"
        + "}\n";
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("agt4_chain_oracle_" + std::to_string(tag) + ".json");
    {
        std::ofstream f(p);
        f << json;
    }
    auto cfg = std::make_unique<GnssConfig>();
    const bool ok = cfg->loadFromJson(p, lm);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    if (!ok) return nullptr;
    return cfg;
}

/// The signals of the chain groups: GPS on two signals (the second not at every arc), Galileo on one.
const std::vector<std::pair<char, std::string>> kChainSignals{{'G', "L1C"}, {'G', "L2W"}, {'E', "L1C"}};

/// A lineage's roster and its sky: each station sees the satellites of its region, and the regions
/// overlap or not (one component or several per group).
struct ChainWorld {
    std::vector<std::string>           names;    ///< Roster index -> station name.
    std::vector<int>                   region;   ///< Roster index -> region.
    std::vector<std::set<std::string>> sky;      ///< Region -> satellites in view (they rise and set).
    std::vector<std::string>           pool;     ///< Every satellite the lineage can see.
};

ChainWorld randomChainWorld(std::mt19937& rng) {
    ChainWorld w;
    const int n_sta = uniform(rng, 3, 28);
    std::set<std::string> used;
    while (static_cast<int>(w.names.size()) < n_sta) {
        std::string nm;
        for (int c = 0; c < 4; ++c) nm += static_cast<char>('A' + uniform(rng, 0, 25));
        if (used.insert(nm).second) w.names.push_back(nm);
    }
    const int n_regions = uniform(rng, 1, 3);
    for (int i = 0; i < n_sta; ++i) w.region.push_back(uniform(rng, 0, n_regions - 1));
    for (int prn : randomLabels(rng, uniform(rng, 4, 16), 32)) w.pool.push_back(satName('G', prn + 1));
    for (int prn : randomLabels(rng, uniform(rng, 0, 8), 36)) w.pool.push_back(satName('E', prn + 1));
    w.sky.assign(static_cast<std::size_t>(n_regions), {});
    for (auto& sky : w.sky)
        for (const std::string& s : w.pool)
            if (chance(rng, 0.5)) sky.insert(s);
    return w;
}

/// One epoch's election inputs.  AnchorHygiene::age is pointed at @ref age by the caller.
struct ChainEpoch {
    std::vector<bool>                          present;
    PhaseDatumCandidates                       cnd;
    std::set<std::pair<int, std::string>>      slipped;
    std::map<std::pair<int, std::string>, int> age;
    net::ArcTable                              age_arcs;   ///< @ref age as the arc table (N4f1)
    AnchorHygiene                              hyg;
};

/// Advance the sky one epoch (a few satellites set, a few rise) and draw the epoch's census.
ChainEpoch nextChainEpoch(std::mt19937& rng, ChainWorld& w, const AnchorHygiene& hyg_shape) {
    for (auto& sky : w.sky)
        for (const std::string& s : w.pool) {
            if (sky.count(s) ? chance(rng, 0.06) : chance(rng, 0.06)) {
                if (sky.count(s)) sky.erase(s);
                else              sky.insert(s);
            }
        }
    ChainEpoch e;
    const std::size_t n = w.names.size();
    e.present.assign(n, true);
    e.cnd.sat_el.resize(n);
    e.cnd.usable_keys.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        e.present[i] = chance(rng, 0.92);
        e.cnd.cover['G'][i] = uniform(rng, 1, 4);   // few values: coverage ties
        e.cnd.cover['E'][i] = uniform(rng, 1, 4);
        for (const std::string& s : w.sky[static_cast<std::size_t>(w.region[i])]) {
            if (!chance(rng, 0.85)) continue;   // a station-local dropout
            e.cnd.sat_el[i][s] = 10.0 * uniform(rng, 1, 6);   // few values: longevity ties
            for (const auto& [cons, sig] : kChainSignals) {
                if (s[0] != cons || (sig == "L2W" && !chance(rng, 0.85))) continue;
                const std::string amb = StateKeys::makeAmbiguityKey(s, sig);
                e.cnd.usable_keys[i][amb] = StateKeys::makeSatPhaseBiasKey(s, sig);
                if (chance(rng, 0.08)) e.slipped.insert({static_cast<int>(i), amb});
                e.age[{static_cast<int>(i), amb}] = uniform(rng, 0, 6);
            }
        }
        if (e.present[i])
            for (const auto& [amb, pbkey] : e.cnd.usable_keys[i])
                if (!chance(rng, 0.03)) e.cnd.union_keys[amb] = pbkey;   // a few keys gated out
    }
    e.hyg = hyg_shape;
    if (e.hyg.active)
        for (const auto& [amb, pbkey] : e.cnd.union_keys)
            if (chance(rng, 0.08)) e.hyg.bridged.insert(amb);
    return e;
}

/// The same random edit applied to both run contexts: carried values, last pin targets, roots,
/// incumbents and parents that the previous epochs did not leave behind, so that every field of
/// the order tuple (pin displacement, known, root handover, re-host) and every carry the shift
/// walk and the D4 erase touch is exercised.  Keys are drawn from @p a; @p b must equal @p a.
void perturbChainCarry(std::mt19937& rng, const ChainWorld& w, const ChainEpoch& e, NetworkRunContext& a,
                       NetworkRunContext& b) {
    const auto both = [&](const std::function<void(NetworkCarryState&)>& f) {
        f(a.carry);
        f(b.carry);
    };
    const auto bothEnt = [&](const std::function<void(net::NetworkEntities&)>& f) {
        f(a.entities);
        f(b.entities);
    };
    for (std::size_t i = 0; i < w.names.size(); ++i) {
        for (const auto& [amb, pbkey] : e.cnd.usable_keys[i]) {
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            const auto key = std::make_tuple(static_cast<int>(i), parts->satellite, parts->signal);
            if (chance(rng, 0.25)) {
                LocalCarry lc{};
                lc.value_abs = std::uniform_real_distribution<double>(-3.0, 3.0)(rng);
                lc.var       = 1e-4;
                bothEnt([&](net::NetworkEntities& c) { net::putArc(c, key, &net::ArcState::amb) = lc; });
            } else if (chance(rng, 0.08)) {
                bothEnt([&](net::NetworkEntities& c) { net::dropArc(c.arcs, key, &net::ArcState::amb); });
            }
            if (chance(rng, 0.1)) {
                const double t = chance(rng, 0.5) ? 0.19 * uniform(rng, -3, 3)
                                                  : std::uniform_real_distribution<double>(-1.0, 1.0)(rng);
                bothEnt([&](net::NetworkEntities& c) {
                    net::putArcAmb(c, static_cast<int>(i), amb, &net::ArcState::eq34_prev_target) = t;
                });
            }
            if (chance(rng, 0.03)) {
                const std::string sta = chance(rng, 0.9) ? w.names[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(w.names.size()) - 1))] : "ZZZZ";
                bothEnt([&](net::NetworkEntities& c) {
                    net::putSig(c, amb, &net::SatSignalState::anchor_incumbent) = sta;
                });
            }
            if (chance(rng, 0.04)) {
                const std::string sta = w.names[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(w.names.size()) - 1))];
                bothEnt([&](net::NetworkEntities& c) {
                    net::putSig(c, amb, &net::SatSignalState::eq34_sat_root) = sta;
                });
            }
        }
        for (const auto& [cons, sig] : kChainSignals) {
            const System sys = cons == 'G' ? System::GPS : System::Galileo;
            const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(systemName(sys), sig);
            if (chance(rng, 0.3)) {
                LocalCarry lc{};
                lc.value_abs = std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
                lc.var       = 1e-4;
                bothEnt([&](net::NetworkEntities& c) {
                    net::put(c.stations, StationId{static_cast<int>(i)}, &net::StationState::signals,
                             rpb_key, &net::StationSignalState::rpb) = lc;
                });
            }
            if (chance(rng, 0.04) && !w.pool.empty()) {
                const std::string sat = w.pool[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(w.pool.size()) - 1))];
                const std::string rxkey = w.names[i] + "|" + constellationKey(sys) + "|" + sig;
                bothEnt([&](net::NetworkEntities& c) {
                    net::put(c.stations, StationId{static_cast<int>(i)}, &net::StationState::signals,
                             constellationKey(sys) + "|" + sig, &net::StationSignalState::eq34_rx_parent) = sat;
                });
            }
        }
    }
    if (chance(rng, 0.05)) {
        const std::string sta = w.names[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(w.names.size()) - 1))];
        bothEnt([&](net::NetworkEntities& c) {
            net::put(c.constellations, System::GPS, &net::ConstellationState::channels, "L1C",
                     &net::ChannelState::datum_rx) = sta;
        });
    }
    const bool first = chance(rng, 0.1);
    both([&](NetworkCarryState& c) { c.resume_datum_first_ep = first; });
}

/// Every output of the election, from both runs, compared.  Returns the first difference (empty: none).
std::string chainElectionDiff(const NetworkRunContext& f, const NetworkRunContext& l,
                              const EpochEvents& ef, const EpochEvents& el,
                              const std::vector<NetStation>& sf, const std::vector<NetStation>& sl,
                              const std::set<std::string>& shf, const std::set<std::string>& shl) {
    for (std::size_t i = 0; i < sf.size(); ++i) {
        if (sf[i].pinned_amb_targets != sl[i].pinned_amb_targets) return "pinned_amb_targets of " + sf[i].name;
        if (sf[i].pinned_rpb_keys != sl[i].pinned_rpb_keys) return "pinned_rpb_keys of " + sf[i].name;
        if (sf[i].pinned_amb_sigma_scale != sl[i].pinned_amb_sigma_scale) return "pinned_amb_sigma_scale of " + sf[i].name;
        if (sf[i].pinned_amb_seed_lambda != sl[i].pinned_amb_seed_lambda) return "pinned_amb_seed_lambda of " + sf[i].name;
    }
    if (shf != shl) return "shared_phase_keys";
    const NetworkCarryState& a = f.carry;
    const NetworkCarryState& b = l.carry;
    if (net::sigMap(f.entities.sat_signals, &net::SatSignalState::anchor_incumbent, '_')
        != net::sigMap(l.entities.sat_signals, &net::SatSignalState::anchor_incumbent, '_'))
        return "anchor_incumbent";
    if (ef.phase_datum.this_epoch_anchor != el.phase_datum.this_epoch_anchor) return "this_epoch_anchor";
    if (net::channelMap(f.entities.constellations, &net::ChannelState::datum_rx)
        != net::channelMap(l.entities.constellations, &net::ChannelState::datum_rx))
        return "eq34_datum_rx (best_datum_rx)";
    const auto cells = [](const NetworkRunContext& c, auto field) {
        std::vector<std::tuple<int, std::string, double, std::string>> out;
        net::forEach(c.entities.stations, &net::StationState::signals, field,
                     [&](const StationId& id, const std::string& k, const auto& v) {
                         if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>)
                             out.emplace_back(id.index, k, 0.0, v);
                         else
                             out.emplace_back(id.index, k, v.value_abs, std::string());
                     });
        return out;
    };
    if (cells(f, &net::StationSignalState::eq34_rx_parent)
        != cells(l, &net::StationSignalState::eq34_rx_parent)) return "eq34_rx_parent";
    if (net::sigMap(f.entities.sat_signals, &net::SatSignalState::eq34_sat_root, '_')
        != net::sigMap(l.entities.sat_signals, &net::SatSignalState::eq34_sat_root, '_'))
        return "eq34_sat_root";
    const auto prevEl = [](const NetworkRunContext& c) {
        std::map<LosId, double> out;
        net::forEach(c.entities.los, &net::LosState::eq34_prev_el,
                     [&](const LosId& id, double el) { out[id] = el; });
        return out;
    };
    if (prevEl(f) != prevEl(l)) return "eq34_prev_el";
    if (ef.phase_datum.eq34_reref != el.phase_datum.eq34_reref) return "eq34_reref";
    if (net::arcAmbMap(f.entities.arcs, &net::ArcState::eq34_prev_target)
        != net::arcAmbMap(l.entities.arcs, &net::ArcState::eq34_prev_target))
        return "eq34_prev_target";
    if (net::arcAmbSet(f.entities.arcs, &net::ArcState::eq34_retained_edge)
        != net::arcAmbSet(l.entities.arcs, &net::ArcState::eq34_retained_edge))
        return "eq34_retained_edges";
    if (ef.phase_datum.eq34_epoch_declared != el.phase_datum.eq34_epoch_declared) return "eq34_epoch_declared";
    if (ef.phase_datum.eq34_epoch_bridged != el.phase_datum.eq34_epoch_bridged) return "eq34_epoch_bridged";
    if (f.counters.eq34_reref_total != l.counters.eq34_reref_total) return "eq34_reref_total";
    if (f.counters.eq34_erase_total != l.counters.eq34_erase_total) return "eq34_erase_total";
    if (f.counters.eq34_shift_total != l.counters.eq34_shift_total) return "eq34_shift_total";
    if (f.counters.eq34_cpl_sprev_total != l.counters.eq34_cpl_sprev_total) return "eq34_cpl_sprev_total";
    if (f.counters.eq34_gap_repin_total != l.counters.eq34_gap_repin_total) return "eq34_gap_repin_total";
    if (f.counters.eq34_mismatch_epochs != l.counters.eq34_mismatch_epochs) return "eq34_mismatch_epochs";
    const auto lca = net::arcMap(f.entities.arcs, &net::ArcState::amb);
    const auto lcb = net::arcMap(l.entities.arcs, &net::ArcState::amb);
    if (lca.size() != lcb.size()) return "local_carry size";
    for (auto ia = lca.begin(), ib = lcb.begin(); ia != lca.end(); ++ia, ++ib)
        if (ia->first != ib->first || ia->second.value_abs != ib->second.value_abs) return "local_carry entry";
    if (cells(f, &net::StationSignalState::rpb) != cells(l, &net::StationSignalState::rpb))
        return "rpb_carry entry";
    const auto& ca = f.counters;
    const auto& cb = l.counters;
    if (ca.eq34_slip_deseeded != cb.eq34_slip_deseeded) return "eq34_slip_deseeded";
    if (ca.eq34_slip_readmitted != cb.eq34_slip_readmitted) return "eq34_slip_readmitted";
    if (ca.hyg.cold_fallback_pins != cb.hyg.cold_fallback_pins) return "hyg.cold_fallback_pins";
    if (ca.hyg.reattaches != cb.hyg.reattaches) return "hyg.reattaches";
    if (ca.hyg.fresh_pin_ramps != cb.hyg.fresh_pin_ramps) return "hyg.fresh_pin_ramps";
    if (ca.n_cr_eq34_reref != cb.n_cr_eq34_reref) return "n_cr_eq34_reref";
    if (ca.n_resume_datum_first_reref != cb.n_resume_datum_first_reref) return "n_resume_datum_first_reref";
    if (ca.seed_round_pins != cb.seed_round_pins || ca.seed_round_frozen != cb.seed_round_frozen
        || ca.seed_round_target_steps != cb.seed_round_target_steps)
        return "seed_round counters";
    if (ca.prm_pbhold_unmoved_at_shift != cb.prm_pbhold_unmoved_at_shift) return "prm_pbhold_unmoved_at_shift";
    for (int k = 0; k < kResponseSites; ++k)
        if (ca.prm_resp[k] != cb.prm_resp[k]) return "prm_resp[" + std::to_string(k) + "]";
    for (int k = 0; k < kCarryMechanisms; ++k)
        if (ca.prm_ref_calls[k] != cb.prm_ref_calls[k]) return "prm_ref_calls[" + std::to_string(k) + "]";
    return {};
}

/// The arcs of this epoch's chain graphs that are slipped: the new eq34_slip_demoted per epoch under
/// the longevity and slip-aware elections.
long long slippedGraphArcs(const ChainEpoch& e) {
    long long n = 0;
    for (std::size_t i = 0; i < e.present.size(); ++i) {
        if (!e.present[i]) continue;
        for (const auto& [amb, pbkey] : e.cnd.usable_keys[i])
            if (e.cnd.union_keys.count(amb) && e.slipped.count({static_cast<int>(i), amb})) ++n;
    }
    return n;
}

/// Per group: its component count and whether the largest components tie on station count (the
/// case in which the component order decides best_datum_rx).  A tally of what the lineages reach.
void tallyChainComponents(const ChainEpoch& e, long long& n_multi, long long& n_size_ties) {
    std::map<std::string, std::vector<std::pair<int, int>>> group_arcs;   // group -> (station, satellite vertex)
    std::map<std::string, int> sat_vertex;
    for (std::size_t i = 0; i < e.present.size(); ++i) {
        if (!e.present[i]) continue;
        for (const auto& [amb, pbkey] : e.cnd.usable_keys[i]) {
            if (!e.cnd.union_keys.count(amb)) continue;
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            const auto v = sat_vertex.emplace(amb, 1000 + static_cast<int>(sat_vertex.size())).first->second;
            group_arcs[std::string(1, parts->satellite[0]) + "|" + parts->signal].push_back({static_cast<int>(i), v});
        }
    }
    for (const auto& [group, arcs] : group_arcs) {
        Dsu d;
        for (const auto& [s, k] : arcs) dsuUnite(d, s, k);
        std::map<int, std::set<int>> stations_of_root;
        for (const auto& [s, k] : arcs) stations_of_root[dsuFind(d, s)].insert(s);
        if (stations_of_root.size() < 2) continue;
        ++n_multi;
        std::vector<std::size_t> sizes;
        for (const auto& [root, sta] : stations_of_root) sizes.push_back(sta.size());
        std::sort(sizes.rbegin(), sizes.rend());
        if (sizes[0] == sizes[1]) ++n_size_ties;
    }
}

TEST(NetworkArcGraphOracle, ChainElectionOnTheModuleEqualsTheFrozenElection) {
    std::mt19937 rng(20261012u);
    LogManager lm;
    const auto quiet = std::make_shared<spdlog::logger>("agt4_chain_oracle", spdlog::sinks_init_list{});
    long long n_epochs = 0, n_pins = 0, n_multi = 0, n_size_ties = 0, n_frozen_demoted = 0, n_live_demoted = 0;
    long long n_bridged = 0;
    for (int lineage = 0; lineage < 48; ++lineage) {
        ChainKnobs kn;
        kn.longevity   = chance(rng, 0.5);
        kn.slip_aware  = chance(rng, 0.5);
        kn.extend_only = chance(rng, 0.4);
        kn.carry_shift = chance(rng, 0.5);
        kn.pin_round   = chance(rng, 0.7);
        ChainWorld w = randomChainWorld(rng);
        if (chance(rng, 0.2)) kn.preferred = w.names[static_cast<std::size_t>(uniform(rng, 0, static_cast<int>(w.names.size()) - 1))];
        const std::unique_ptr<GnssConfig> cfg = loadChainConfig(lm, kn, lineage);
        ASSERT_TRUE(cfg) << "lineage " << lineage;
        NetworkRunContext ctx_f(*cfg, cfg->getNetworkConfig());
        NetworkRunContext ctx_l(*cfg, cfg->getNetworkConfig());
        ctx_f.entities.names.assign(w.names);   // the name keys of the station table (N4a)
        ctx_l.entities.names.assign(w.names);
        ASSERT_EQ(ctx_l.knobs.eq34_longevity, kn.longevity);
        ASSERT_EQ(ctx_l.knobs.eq34_slip_aware, kn.slip_aware);
        ASSERT_EQ(ctx_l.knobs.eq34_extend_only, kn.extend_only);
        ASSERT_EQ(ctx_l.knobs.eq34_shift_on, kn.carry_shift);
        ASSERT_FALSE(ctx_l.knobs.eq34_seed_round);   // stations carry no StateManager here

        AnchorHygiene hyg_shape;   // the lineage's hygiene: inactive, or INC-1 with or without bridges and INC-3
        hyg_shape.active = chance(rng, 0.5);
        if (hyg_shape.active) {
            hyg_shape.min_arc_ep = uniform(rng, 0, 3);
            if (chance(rng, 0.5)) {
                hyg_shape.fresh_pin_scale   = 4.0;
                hyg_shape.fresh_pin_ramp_ep = 3;
            }
        }
        for (int ep = 0; ep < 40; ++ep) {
            ChainEpoch e = nextChainEpoch(rng, w, hyg_shape);
            e.age_arcs = net::arcAmbTableOf(e.age, &net::ArcState::anchor_arc_age);
            e.hyg.age = e.hyg.active ? &e.age_arcs : nullptr;
            perturbChainCarry(rng, w, e, ctx_f, ctx_l);
            std::vector<NetStation> st_f(w.names.size()), st_l(w.names.size());
            for (std::size_t i = 0; i < w.names.size(); ++i) st_f[i].name = st_l[i].name = w.names[i];
            std::set<std::string> sh_f, sh_l;
            const long long demoted_f = ctx_f.counters.eq34_slip_demoted;
            const long long demoted_l = ctx_l.counters.eq34_slip_demoted;
            EpochEvents ev_f, ev_l;   // fresh every epoch, as the epoch builds them
            frozen_eq34::networkElectChainDatum(ctx_f, *cfg, quiet, st_f, e.present, e.cnd, e.hyg, e.slipped, sh_f, ev_f);
            networkElectChainDatum(ctx_l, *cfg, quiet, st_l, e.present, e.cnd, e.hyg, e.slipped, sh_l, ev_l);
            ASSERT_EQ(chainElectionDiff(ctx_f, ctx_l, ev_f, ev_l, st_f, st_l, sh_f, sh_l), "")
                << "lineage " << lineage << " epoch " << ep;
            // The declared move: one count per slipped graph arc and epoch.
            const long long want_demoted = (kn.longevity && kn.slip_aware) ? slippedGraphArcs(e) : 0;
            ASSERT_EQ(ctx_l.counters.eq34_slip_demoted - demoted_l, want_demoted) << "lineage " << lineage << " epoch " << ep;
            n_frozen_demoted += ctx_f.counters.eq34_slip_demoted - demoted_f;
            n_live_demoted += ctx_l.counters.eq34_slip_demoted - demoted_l;
            for (const NetStation& st : st_l) n_pins += static_cast<long long>(st.pinned_amb_targets.size());
            for (const auto& [group, nb] : ev_l.phase_datum.eq34_epoch_bridged) n_bridged += nb;
            tallyChainComponents(e, n_multi, n_size_ties);
            ++n_epochs;
        }
        // Run tallies the lineage reached (summed below).
        const auto& c = ctx_l.counters;
        std::cout << "[ NetworkArcGraphOracle ] chain lineage " << lineage << " (longevity " << kn.longevity
                  << ", slip-aware " << kn.slip_aware << ", extend-only " << kn.extend_only << ", shift "
                  << kn.carry_shift << ", round " << kn.pin_round << ", hygiene " << hyg_shape.active << "/"
                  << hyg_shape.min_arc_ep << "): reref " << ctx_l.counters.eq34_reref_total << ", erased "
                  << ctx_l.counters.eq34_erase_total << ", shifted " << ctx_l.counters.eq34_shift_total << ", gap re-pins "
                  << ctx_l.counters.eq34_gap_repin_total << ", deseeded " << c.eq34_slip_deseeded << ", readmitted "
                  << c.eq34_slip_readmitted << ", cold " << c.hyg.cold_fallback_pins << ", reattaches "
                  << c.hyg.reattaches << ", ramps " << c.hyg.fresh_pin_ramps << "\n";
    }
    EXPECT_GT(n_pins, 50000);
    EXPECT_GT(n_multi, 200);
    EXPECT_GT(n_size_ties, 20);
    EXPECT_GT(n_bridged, 100);
    std::cout << "[ NetworkArcGraphOracle ] chain election: " << n_epochs << " epochs, " << n_pins << " pins, "
              << n_multi << " multi-component groups (" << n_size_ties << " with a tie for the largest), "
              << n_bridged << " bridged roots; eq34_slip_demoted " << n_frozen_demoted
              << " comparator calls (frozen) against " << n_live_demoted << " slipped arcs (live)\n";
}

}  // namespace
}  // namespace GnssProcessor
