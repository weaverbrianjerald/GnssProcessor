/**
 * @file NetworkProducerAr.cpp
 * @brief Implementation of @ref networkProducerAr: the log-only producer-side
 *        network-AR (Phase 2) diagnostic and the one-shot NEQ export, relocated
 *        out of Application::runNetwork at decomposition INC-9b.
 *
 * The body started as a copy of the former SB_NETAR block, dedented 4 spaces.  The
 * alias preamble reconstructs the three run-TU aliases the body reads (nc / netar /
 * n_emitted); every other name is a parameter named as the former run-TU local.  See
 * NetworkProducerAr.hpp.
 *
 * Three additive departures from the relocated text, all outside the arithmetic (the
 * trace card; docs/network_ambiguity_resolution_
 * design.md, 2026-09-10 note §4.1, "a fix count is not a grade"):
 *   1. `DDrow` carries the six identity strings a truth join needs;
 *   2. the cap's `order` comes from @ref netar::lowestSigmaOrder so the trace can
 *      invert it (@ref netar::mapFixToBasis); the sort, comparator and subset are
 *      the same, so the search is unchanged;
 *   3. run-end tallies and the per-DD `network_producer_ar.csv` writer.
 * The solve is untouched, and the block is skipped entirely when
 * `network.ambiguity_resolution.enable` is off, so with it off the products are
 * byte-identical.
 */

#include "filter/NetworkProducerAr.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.counters.n_emitted
#include "config/GnssConfig.hpp"         // config_.getNetworkConfig() / .ambiguity_resolution
#include "common/GnssIdentity.hpp"       // systemOfSat, systemFromName, constellationKey, SignalId
#include "common/GnssTypes.hpp"          // System
#include "filter/IlsCore.hpp"            // ils::IlsGateConfig / IlsResult / ilsFix
#include "filter/NetworkArcGraph.hpp"    // arcgraph:: the observation graph and its tree basis (card AGT)
#include "filter/NetworkArCycleBasis.hpp" // the sticky full-coverage cycle basis (card AGT, AGT-5)
#include "filter/NetworkArcLife.hpp"     // the arc life stamp and the search's track of a life (card AGT)
#include "filter/NetworkArSuspect.hpp"   // the suspect-corner mask (card ARL)
#include "filter/StateKeySchema.hpp"     // StateKeys::parseAmbiguityKey
#include "math/TimeUtils.hpp"            // TimeUtils::continuousGpsSeconds (the mask's fold clock)
#include "filter/StateManager.hpp"       // stations[i].sm-> ...
#include "logging/NetworkRunTraces.hpp"  // traces.producer_ar_trace

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <numeric>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include <limits>
#include <memory>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "app/NetworkSetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

namespace netar {

std::vector<int> lowestSigmaOrder(const Eigen::MatrixXd& Q_cyc, int cap) {
    const int n = static_cast<int>(std::min(Q_cyc.rows(), Q_cyc.cols()));
    std::vector<int> order(static_cast<std::size_t>(std::max(0, n)));
    std::iota(order.begin(), order.end(), 0);
    if (cap <= 0 || cap >= n) return order;   // uncapped: identity, un-sorted
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return Q_cyc(a, a) < Q_cyc(b, b);
    });
    order.resize(static_cast<std::size_t>(cap));
    return order;
}

std::vector<int> maskedSearchOrder(const Eigen::MatrixXd& Q_cyc, int cap,
                                   const std::vector<char>& masked) {
    if (std::none_of(masked.begin(), masked.end(), [](char m) { return m != 0; }))
        return lowestSigmaOrder(Q_cyc, cap);
    const int n = static_cast<int>(std::min(Q_cyc.rows(), Q_cyc.cols()));
    std::vector<int> order;
    for (int a = 0; a < n; ++a)
        if (static_cast<std::size_t>(a) >= masked.size() || masked[static_cast<std::size_t>(a)] == 0)
            order.push_back(a);
    if (cap <= 0 || cap >= static_cast<int>(order.size())) return order;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return Q_cyc(a, a) < Q_cyc(b, b);
    });
    order.resize(static_cast<std::size_t>(cap));
    return order;
}

std::vector<DdFixLabel> mapFixToBasis(int n_dd,
                                      const std::vector<int>& search_of,
                                      const std::vector<int>& kept,
                                      const Eigen::VectorXd& afix) {
    std::vector<DdFixLabel> out(static_cast<std::size_t>(std::max(0, n_dd)));
    for (int s : search_of)
        if (s >= 0 && s < n_dd) out[static_cast<std::size_t>(s)].in_subset = true;
    // `kept[m]` indexes the search vector; `afix(m)` is that entry's integer.  The
    // integer goes to basis entry search_of[kept[m]].  Using kept[m] directly as a
    // basis index would attribute it to whichever DD sits at that ordinal in the
    // basis, which on a capped, sigma-sorted search is an unrelated satellite.
    const int n_af = static_cast<int>(afix.size());
    for (std::size_t m = 0; m < kept.size() && static_cast<int>(m) < n_af; ++m) {
        const int s = kept[m];
        if (s < 0 || static_cast<std::size_t>(s) >= search_of.size()) continue;
        const int b = search_of[static_cast<std::size_t>(s)];
        if (b < 0 || b >= n_dd) continue;
        auto& lab = out[static_cast<std::size_t>(b)];
        lab.kept      = true;
        lab.fixed_int = afix(static_cast<Eigen::Index>(m));
    }
    return out;
}

ils::IlsResult blockDecorrelatedFix(const Eigen::VectorXd& y, const Eigen::MatrixXd& Q,
                                    const std::vector<int>& block_of,
                                    const ils::IlsGateConfig& gates,
                                    double determined_sigma_cyc) {
    ils::IlsResult out;
    const int n = static_cast<int>(y.size());
    out.z_total = n;
    out.a_cond = y;
    out.Q_cond = Q;
    if (n <= 0 || static_cast<int>(block_of.size()) != n) return out;
    std::map<int, std::vector<int>> blocks;   // block id -> search rows
    for (int i = 0; i < n; ++i) blocks[block_of[static_cast<std::size_t>(i)]].push_back(i);
    std::vector<std::pair<int, double>> fixed;   // (search row, integer)
    double p_all = 1.0;
    bool any_block = false;
    for (const auto& [id, rows] : blocks) {
        (void)id;
        const int m = static_cast<int>(rows.size());
        Eigen::VectorXd yb(m);
        Eigen::MatrixXd Qb(m, m);
        for (int a = 0; a < m; ++a) {
            yb(a) = y(rows[static_cast<std::size_t>(a)]);
            for (int b = 0; b < m; ++b)
                Qb(a, b) = Q(rows[static_cast<std::size_t>(a)], rows[static_cast<std::size_t>(b)]);
        }
        const ils::IlsResult r = ils::ilsFixDecorrelatedPartial(yb, Qb, gates, determined_sigma_cyc);
        if (r.full_ratio > 0.0)
            out.full_ratio = (out.full_ratio == 0.0) ? r.full_ratio : std::min(out.full_ratio, r.full_ratio);
        if (r.z_fixed <= 0) continue;
        out.z_fixed += r.z_fixed;
        p_all *= r.p_succ;
        out.ratio = any_block ? std::min(out.ratio, r.ratio) : r.ratio;
        any_block = true;
        for (int a = 0; a < m; ++a) {
            const int ra = rows[static_cast<std::size_t>(a)];
            out.a_cond(ra) = r.a_cond(a);
            for (int b = 0; b < m; ++b)
                out.Q_cond(ra, rows[static_cast<std::size_t>(b)]) = r.Q_cond(a, b);
        }
        for (std::size_t k = 0; k < r.kept.size(); ++k) {
            fixed.emplace_back(rows[static_cast<std::size_t>(r.kept[k])],
                               r.afix(static_cast<Eigen::Index>(k)));
            out.max_frac = std::max(out.max_frac, r.max_frac);
        }
    }
    std::sort(fixed.begin(), fixed.end());
    out.afix.resize(static_cast<Eigen::Index>(fixed.size()));
    for (std::size_t k = 0; k < fixed.size(); ++k) {
        out.kept.push_back(fixed[k].first);
        out.afix(static_cast<Eigen::Index>(k)) = fixed[k].second;
    }
    out.p_succ = any_block ? p_all : 0.0;
    out.full_p_succ = out.p_succ;
    out.ok = !out.kept.empty();
    out.partial = static_cast<int>(out.kept.size()) < n;
    if (!any_block) { out.a_cond.resize(0); out.Q_cond.resize(0, 0); }
    return out;
}

std::vector<PairLane> pairLanes(const std::vector<DdPairKey>& keys,
                                const std::vector<int>& search_of, int n_search,
                                const Eigen::MatrixXd& Q_search,
                                const Eigen::VectorXd& a_cond,
                                const Eigen::MatrixXd& Q_cond) {
    const int n_dd = static_cast<int>(keys.size());
    std::vector<PairLane> out(static_cast<std::size_t>(n_dd));
    // Basis index -> search row, for the searched DDs only.
    std::vector<int> row_of(static_cast<std::size_t>(n_dd), -1);
    for (int s = 0; s < n_search && s < static_cast<int>(search_of.size()); ++s) {
        const int b = search_of[static_cast<std::size_t>(s)];
        if (b >= 0 && b < n_dd) row_of[static_cast<std::size_t>(b)] = s;
    }
    std::map<std::string, std::vector<int>> by_key;   // pair key -> searched basis DDs
    for (int b = 0; b < n_dd; ++b)
        if (row_of[static_cast<std::size_t>(b)] >= 0) by_key[keys[static_cast<std::size_t>(b)].key].push_back(b);
    const bool have_cond = a_cond.size() == n_search && Q_cond.rows() == n_search;
    constexpr double kC = 299792458.0;
    for (const auto& [key, members] : by_key) {
        (void)key;
        if (members.size() != 2) continue;
        int b1 = members[0], b2 = members[1];
        const double l1 = keys[static_cast<std::size_t>(b1)].lambda;
        const double l2 = keys[static_cast<std::size_t>(b2)].lambda;
        if (!(l1 > 0.0 && l2 > 0.0) || l1 == l2) continue;
        if (l1 > l2) std::swap(b1, b2);                      // band 1 = shorter wavelength
        const int i = row_of[static_cast<std::size_t>(b1)], j = row_of[static_cast<std::size_t>(b2)];
        const double f1 = kC / keys[static_cast<std::size_t>(b1)].lambda;
        const double f2 = kC / keys[static_cast<std::size_t>(b2)].lambda;
        const double c1 = f1 / (f1 - f2), c2 = -f2 / (f1 - f2);   // narrow-lane cycles
        auto wlVar = [&](const Eigen::MatrixXd& Q) { return Q(i, i) + Q(j, j) - 2.0 * Q(i, j); };
        auto nlVar = [&](const Eigen::MatrixXd& Q) {
            return c1 * c1 * Q(i, i) + c2 * c2 * Q(j, j) + 2.0 * c1 * c2 * Q(i, j);
        };
        PairLane p;
        p.wl_float_sigma = std::sqrt(std::max(0.0, wlVar(Q_search)));
        p.have_cond      = have_cond;
        if (have_cond) {
            p.wl_cond       = a_cond(i) - a_cond(j);
            p.wl_cond_sigma = std::sqrt(std::max(0.0, wlVar(Q_cond)));
            p.nl_cond_sigma = std::sqrt(std::max(0.0, nlVar(Q_cond)));
        }
        p.partner = b2;
        out[static_cast<std::size_t>(b1)] = p;
        p.partner = b1;
        out[static_cast<std::size_t>(b2)] = p;
    }
    return out;
}

std::vector<std::string> pairLaneText(const std::vector<PairLane>& lanes) {
    std::vector<std::string> out(lanes.size(), ",,,,");
    for (std::size_t b = 0; b < lanes.size(); ++b) {
        const PairLane& p = lanes[b];
        if (p.partner < 0) continue;
        std::string s = fmt::format(",{:.6f}", p.wl_float_sigma);
        if (p.have_cond)
            s += fmt::format(",{:.6f},{:.6f},{:.6f}", p.wl_cond, p.wl_cond_sigma, p.nl_cond_sigma);
        else
            s += ",,,";
        out[b] = s;
    }
    return out;
}

std::vector<std::string> pairLaneColumns(const std::vector<DdPairKey>& keys,
                                         const std::vector<int>& search_of, int n_search,
                                         const Eigen::MatrixXd& Q_search,
                                         const Eigen::VectorXd& a_cond,
                                         const Eigen::MatrixXd& Q_cond) {
    return pairLaneText(pairLanes(keys, search_of, n_search, Q_search, a_cond, Q_cond));
}

ArSuspectReentry reentryOutcome(const std::vector<char>& reentry,
                                const std::vector<DdFixLabel>& labels,
                                const std::vector<PairLane>& lanes, double det_sigma_cyc) {
    ArSuspectReentry out;
    const std::size_t n = std::min(reentry.size(), labels.size());
    for (std::size_t a = 0; a < n; ++a) {
        if (reentry[a] == 0) continue;
        ++out.dd;
        if (labels[a].kept) ++out.dd_fixed;
        // Each pair once, from its lower basis index, when both of its DDs re-entered.
        if (a >= lanes.size()) continue;
        const int p = lanes[a].partner;
        if (p < 0 || static_cast<std::size_t>(p) <= a || static_cast<std::size_t>(p) >= n
            || reentry[static_cast<std::size_t>(p)] == 0)
            continue;
        ++out.pairs;
        if (labels[a].kept && labels[static_cast<std::size_t>(p)].kept)
            ++out.pairs_singles;
        else if (lanes[a].have_cond && lanes[a].wl_cond_sigma <= det_sigma_cyc)
            ++out.pairs_wl_only;
    }
    return out;
}

std::string searchBlockKey(const std::string& cons, const std::string& sta_r, const std::string& ref_sta,
                           bool per_station) {
    return per_station ? cons + "|" + sta_r : cons + "|" + sta_r + "|" + ref_sta;
}

} // namespace netar

namespace {

/// One entry of the producer AR pool: a station's absolute float ambiguity on one satellite and
/// signal, its (est id, A_r column) for the covariance request, and `suspect`, the corner's state
/// under the suspect-corner mask at this search (card ARL): its live cause bits and whether this
/// search releases it; clean while the mask is off.
struct ArRec { double value_m; std::size_t est_id; Eigen::Index col; ArSuspectCornerState suspect; };

/// One (constellation, signal) pool of the search: satellite -> station index -> entry.
using ArSatPool = std::map<std::string, std::map<std::size_t, ArRec>>;

/// The tree basis of one pool (card ARL tree_basis).
struct ArTreeBasis {
    int                  n_stations{0};  ///< Stations in the pool (a basis needs two).
    int                  n_refused{0};   ///< Pool satellites left out: the name does not round-trip through SatId (0 expected).
    arcgraph::CycleBasis basis;          ///< One DD (r, p; j, k) per cycle, in the search's order.
};

/// Card ARL's tree_basis, grown by the observation-graph module (card AGT, step AGT-2).
///
/// The pool becomes a one-band graph: stations by ascending index, satellites in the pool's string
/// order (every tie breaks by these slots), one arc per entry.  A baseline forest is grown with the
/// tree_basis rules: each outside station joins the in-tree station it shares the most satellites
/// with (lowest index on ties) through their first common satellite, until no outside station
/// shares two.  The parent-only closure then gives, for each link r -> p with reference k, one DD
/// (r, p; j, k) per further common satellite j.  Every link brings in a new station's ambiguities,
/// so the set is independent by construction.
ArTreeBasis arTreeBasis(System sys, const SignalId& sig, const ArSatPool& pool) {
    ArTreeBasis out;
    std::set<std::size_t>         station_set;
    std::vector<SatId>            sat_order;
    std::vector<arcgraph::ArcRow> rows;
    for (const auto& [sat, by_station] : pool) {
        for (const auto& [i, rec] : by_station) { (void)rec; station_set.insert(i); }
        const std::optional<SatId> id = SatId::parse(sat);
        if (!id || id->str() != sat) { ++out.n_refused; continue; }
        sat_order.push_back(*id);
        for (const auto& [i, rec] : by_station) {
            (void)rec;
            arcgraph::ArcRow row;
            row.station = StationId{static_cast<int>(i)};
            row.sat     = *id;
            row.present = 1;
            rows.push_back(row);
        }
    }
    out.n_stations = static_cast<int>(station_set.size());
    if (out.n_stations < 2) return out;
    std::vector<StationId> station_order;
    for (std::size_t i : station_set) station_order.push_back(StationId{static_cast<int>(i)});

    arcgraph::ForestRules rules;      // the tree_basis rules
    rules.band_mask   = 1;            // the one band
    rules.min_common  = 2;            // a link needs two common satellites ...
    rules.stop_at_gap = true;         // ... and the tree stops when none reaches two
    const arcgraph::ArcGraph g =
        arcgraph::buildArcGraph(arcgraph::ArcFamily{sys, {sig}}, station_order, sat_order, rows);
    const arcgraph::SpanningForest forest =
        arcgraph::growBaselineForest(g, arcgraph::wholeGraph(g, rules.band_mask), {}, rules);
    out.basis = arcgraph::shortCycleBasis(g, forest, arcgraph::BasisCoverage::ParentOnly);
    return out;
}

/// The search's pools: (constellation, signal) -> satellite -> station index -> entry.
using ArPools = std::map<std::pair<std::string, std::string>, ArSatPool>;

/// A pool entry's index in the covariance request (its (est id, A_r column), added on first use).
using ArRequestOf = std::function<int(const ArRec&)>;

/// One search row: the double difference (r, p; j, k) = (r,j) - (r,k) - (p,j) + (p,k) of four
/// pool entries (their covariance request indices), its float (m) and its wavelength.
///
/// The identity fields (cons, signal, sat_j, ref_sat, sta_r, ref_sta) let a traced DD be joined
/// to truth_ambiguities.csv and the star reference be differenced away
/// (SPEC_amb_iono_split_overcount §6.12); without them the trace gives only a fix count.  They
/// are filled unconditionally: the block is already gated on `netar.enable`, and six short
/// strings per DD cost little next to the joint conditional covariance.
/// `suspect`: the four corners' mask states combined (card ARL); a DD with any cause bit set
/// stays in the basis and the trace but is not searched.
/// The cycle-basis fields (card AGT) are set only under cycle_basis: the first four are the
/// trace's columns, the last two what the lattice observatory is offered when the DD is fixed.
struct DDrow {
    int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda;
    std::string cons, signal, sat_j, ref_sat, sta_r, ref_sta;
    ArSuspectCornerState suspect{};
    arcgraph::CycleKind cycle_kind{arcgraph::CycleKind::PartnerDd};  ///< How the DD closes its chord.
    bool       band_only{false};                 ///< The chord exists in this band only.
    long long  basis_age_ep{0};                  ///< Epochs since this DD identity entered the basis without a break.
    ArMapState map_state{ArMapState::FreshNew};  ///< The DD's cycle against the last search's basis.
    arcgraph::Cycle cycle;                       ///< The DD as its basis cycle (step AGT-6).
    ArDdIdentity    cycle_id{};                  ///< Its typed identity (step AGT-6).
};

/// The per-(constellation, signal) basis of card ARL: tree_basis (see arTreeBasis), else the star.
///
/// The star, per (constellation, signal): a reference satellite (tracked at the most stations)
/// and a reference station (tracking the reference satellite and the most satellites); one DD per
/// (other station r, other satellite j) that both r and the reference station track.  Appends one
/// row per DD; returns the (constellation, signal) pools that took part.
int arSignalBasisRows(bool tree_basis, ArPools& ar_pool, const SyncedEpoch& se,
                      const std::vector<NetStation>& stations, const ArRequestOf& reqOf,
                      std::vector<DDrow>& dds, const std::shared_ptr<spdlog::logger>& log) {
    int n_const_signal = 0;
    for (auto& [cs, satmap] : ar_pool) {
        if (tree_basis) {
            // Card ARL tree_basis (see arTreeBasis): a greedy spanning tree of station
            // baselines, one DD per further common satellite of each link.
            const System t_sys = systemFromName(cs.first);
            const std::optional<SignalId> tsig = SignalId::fromObsCode(t_sys, cs.second);
            const double t_lambda = tsig ? tsig->wavelengthM(0) : 0.0;
            if (!(std::isfinite(t_lambda) && t_lambda > 0.0)) continue;
            const ArTreeBasis tree = arTreeBasis(t_sys, *tsig, satmap);
            if (tree.n_refused > 0)
                log->warn("Network AR tree basis: TOW {:.1f} {}/{}: {} satellite(s) left out of the "
                          "basis, their names do not round-trip through SatId",
                          se.time.tow, cs.first, cs.second, tree.n_refused);
            if (tree.n_stations < 2) continue;
            ++n_const_signal;
            // Each cycle (r, p; j, k) is the DD (r,j) - (r,k) - (p,j) + (p,k).
            for (const arcgraph::BasisCycle& bc : tree.basis.cycles) {
                const std::string sat_j = bc.sat.str(), ref_sat = bc.ref_sat.str();
                const std::size_t r = static_cast<std::size_t>(bc.station.index);
                const std::size_t p = static_cast<std::size_t>(bc.partner.index);
                const ArRec& rj = satmap.at(sat_j).at(r);
                const ArRec& rp = satmap.at(ref_sat).at(r);
                const ArRec& fj = satmap.at(sat_j).at(p);
                const ArRec& fp = satmap.at(ref_sat).at(p);
                dds.push_back(DDrow{ reqOf(rj), reqOf(rp), reqOf(fj), reqOf(fp),
                                     (rj.value_m - rp.value_m) - (fj.value_m - fp.value_m),
                                     t_lambda, cs.first, cs.second, sat_j, ref_sat,
                                     stations[r].name, stations[p].name });
                dds.back().suspect =
                    arSuspectCombine(rj.suspect, rp.suspect, fj.suspect, fp.suspect);
            }
            continue;
        }
        std::string ref_sat; std::size_t best = 0;
        for (auto& [sat, sm] : satmap)
            if (sm.size() > best) { best = sm.size(); ref_sat = sat; }
        if (ref_sat.empty() || satmap[ref_sat].size() < 2) continue;
        std::size_t ref_i = 0, ref_cnt = 0; bool have_ref = false;
        for (auto& [sta_i, rec] : satmap[ref_sat]) {
            (void)rec;
            std::size_t cnt = 0;
            for (auto& [sat, sm] : satmap) if (sm.count(sta_i)) ++cnt;
            if (!have_ref || cnt > ref_cnt) { ref_cnt = cnt; ref_i = sta_i; have_ref = true; }
        }
        if (!have_ref) continue;
        const System sys = systemFromName(cs.first);   // "G" or "E"
        const std::optional<SignalId> amb_sig = SignalId::fromObsCode(sys, cs.second);
        const double lambda = amb_sig ? amb_sig->wavelengthM(0) : 0.0;   // CDMA: no FDMA slot
        if (!(std::isfinite(lambda) && lambda > 0.0)) continue;
        const ArRec& fp = satmap[ref_sat].at(ref_i);   // ref station, ref sat
        ++n_const_signal;
        for (auto& [sat, sm] : satmap) {
            if (sat == ref_sat) continue;
            const auto fj_it = sm.find(ref_i);          // ref station, sat j
            if (fj_it == sm.end()) continue;
            for (const auto& [sta_i, rj] : sm) {
                if (sta_i == ref_i) continue;
                const auto rp_it = satmap[ref_sat].find(sta_i);   // station r, ref sat
                if (rp_it == satmap[ref_sat].end()) continue;
                const double dd_m = (rj.value_m - rp_it->second.value_m)
                                  - (fj_it->second.value_m - fp.value_m);
                dds.push_back(DDrow{ reqOf(rj), reqOf(rp_it->second),
                                     reqOf(fj_it->second), reqOf(fp), dd_m, lambda,
                                     cs.first, cs.second, sat, ref_sat,
                                     stations[sta_i].name, stations[ref_i].name });
                dds.back().suspect = arSuspectCombine(rj.suspect, rp_it->second.suspect,
                                                      fj_it->second.suspect, fp.suspect);
            }
        }
    }
    return n_const_signal;
}

// ---- Card AGT, step AGT-5: the cycle basis (filter/NetworkArCycleBasis.hpp) ----------------------

/// One family's build at this search, held until it becomes the family's memory after the search.
struct ArCycleFamilyEpoch {
    System                            sys{System::Unknown};  ///< The family's constellation (its memory's row).
    ArFamilyKey                       key;       ///< The family's bands.
    ArCycleBuild                      build;     ///< Its graph, forest and basis.
    std::map<ArcId, ArcLifeTrack>     arcs;      ///< Its band-arcs' tracks after this search.
    std::map<ArDdIdentity, long long> first_ep;  ///< Its DD identities' first epochs in the basis.
};

/// One family's rows read from the pools (step 2 of the NetworkArCycleBasis.hpp banner): per
/// (satellite, station) the bands present, the bands whose life reading is slipped, and the
/// weaker band's durability (epochs since its life began).  Every band-arc's life reading is
/// tallied and its new track kept in @p tracks, and, for the lattice observatory (step AGT-6),
/// the reading itself in @p readings unless it is null.  Slots: stations by ascending roster
/// index, satellites in the pools' string order; a name that does not round-trip through SatId
/// holds no slot (none expected: pool names come from state keys, as for tree_basis).
ArCycleInput arCycleInputOf(const NetworkRunContext& ctx, const ArFamilySpec& fam, const std::string& cons,
                            const ArPools& pools, const ArBasisMemory& mem, long long now_ep,
                            std::map<ArcId, ArcLifeTrack>& tracks, ArCycleBasisCounters& c,
                            std::map<ArcId, ArArcReading>* readings) {
    ArCycleInput in;
    in.family = arcgraph::ArcFamily{fam.sys, fam.bands};
    std::set<std::size_t>      station_set;
    std::map<std::string, int> sat_slot;   // pool name -> satellite slot (string order)
    for (const std::string& sig : fam.signals) {
        const auto pit = pools.find({cons, sig});
        if (pit == pools.end()) continue;
        for (const auto& [sat, by_station] : pit->second) {
            const std::optional<SatId> id = SatId::parse(sat);
            if (!id || id->str() != sat) continue;
            sat_slot.emplace(sat, 0);
            for (const auto& [i, rec] : by_station) { (void)rec; station_set.insert(i); }
        }
    }
    for (auto& [name, slot] : sat_slot) {
        slot = static_cast<int>(in.sats.size());
        in.sats.push_back(*SatId::parse(name));
    }
    std::vector<int> station_slot(station_set.empty() ? 0 : *station_set.rbegin() + 1, -1);
    for (std::size_t i : station_set) {
        station_slot[i] = static_cast<int>(in.stations.size());
        in.stations.push_back(StationId{static_cast<int>(i)});
    }

    // One row per (satellite, station), dense; each band-arc's keys are built once and serve the
    // free-latch lookup and the stamp.
    const std::size_t n_sta = in.stations.size();
    std::vector<arcgraph::ArcRow> dense(in.sats.size() * n_sta);
    for (std::size_t k = 0; k < fam.signals.size(); ++k) {
        const auto pit = pools.find({cons, fam.signals[k]});
        if (pit == pools.end()) continue;
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << k);
        for (const auto& [sat, by_station] : pit->second) {
            const auto ks = sat_slot.find(sat);
            if (ks == sat_slot.end()) continue;
            const SatId& sat_id = in.sats[static_cast<std::size_t>(ks->second)];
            for (const auto& [i, rec] : by_station) {
                (void)rec;
                const ArcId arc{StationId{static_cast<int>(i)}, sat_id, fam.bands[k]};
                const ArcLifeKeys keys{std::make_tuple(static_cast<int>(i), sat, fam.signals[k]),
                                       {static_cast<int>(i), StateKeys::makeAmbiguityKey(sat, fam.signals[k])}};
                const bool freed = ctx.carry.relevel.free_ambs.count(keys.amb) > 0;
                const auto last = mem.arcs.find(arc);
                const ArcLifeStep step =
                    arcLifeStep(last == mem.arcs.end() ? nullptr : &last->second, mem.last_search_ep,
                                arcLifeStamp(ctx, keys), freed, now_ep);
                arCycleTallyLife(c, step);
                tracks.emplace_hint(tracks.end(), arc, step.track);
                if (readings) {
                    const auto fc = ctx.carry.relevel.free_cause.find(keys.amb);
                    readings->emplace(arc, ArArcReading{step.edge, step.change,
                                                        fc == ctx.carry.relevel.free_cause.end()
                                                            ? std::uint8_t{0} : fc->second});
                }
                arcgraph::ArcRow& row =
                    dense[static_cast<std::size_t>(ks->second) * n_sta + static_cast<std::size_t>(station_slot[i])];
                if (row.present == 0) {
                    row.station    = arc.station;
                    row.sat        = arc.sat;
                    row.durability = std::numeric_limits<double>::infinity();
                }
                row.present = static_cast<std::uint8_t>(row.present | bit);
                if (step.slipped()) row.slipped = static_cast<std::uint8_t>(row.slipped | bit);
                row.durability =
                    std::min(row.durability, static_cast<double>(now_ep - step.track.life_start));
            }
        }
    }
    for (const arcgraph::ArcRow& row : dense)
        if (row.present != 0) in.rows.push_back(row);
    return in;
}

/// One family's search rows (step 4 of the banner): one DD row per PartnerDd or BorrowedDd cycle,
/// in basis order, with its cycle columns; a fundamental fallback is never a row.  Each DD
/// identity's first epoch in the basis goes to @p first_ep.
void arCycleRows(const ArFamilySpec& fam, const std::string& cons, const ArPools& pools,
                 const ArCycleBuild& b, const std::vector<ArMapState>& states, const ArBasisMemory& mem,
                 long long now_ep, const std::vector<NetStation>& stations, const ArRequestOf& reqOf,
                 std::vector<DDrow>& dds, std::map<ArDdIdentity, long long>& first_ep) {
    for (std::size_t i = 0; i < b.basis.cycles.size(); ++i) {
        const arcgraph::BasisCycle& bc = b.basis.cycles[i];
        if (bc.kind == arcgraph::CycleKind::Fundamental) continue;
        const std::size_t k = static_cast<std::size_t>(bc.band);
        const ArSatPool& pool = pools.at({cons, fam.signals[k]});
        const std::string sat_j = bc.sat.str(), ref_sat = bc.ref_sat.str();
        const std::size_t r = static_cast<std::size_t>(bc.station.index);
        const std::size_t p = static_cast<std::size_t>(bc.partner.index);
        // The cycle (r, p; j, k) is the DD (r,j) - (r,k) - (p,j) + (p,k).
        const ArRec& rj = pool.at(sat_j).at(r);
        const ArRec& rp = pool.at(ref_sat).at(r);
        const ArRec& fj = pool.at(sat_j).at(p);
        const ArRec& fp = pool.at(ref_sat).at(p);
        dds.push_back(DDrow{ reqOf(rj), reqOf(rp), reqOf(fj), reqOf(fp),
                             (rj.value_m - rp.value_m) - (fj.value_m - fp.value_m),
                             fam.bands[k].wavelengthM(0), cons, fam.signals[k], sat_j, ref_sat,
                             stations[r].name, stations[p].name });
        DDrow& d = dds.back();
        d.suspect    = arSuspectCombine(rj.suspect, rp.suspect, fj.suspect, fp.suspect);
        d.cycle_kind = bc.kind;
        d.band_only  = bc.band_only;
        d.map_state  = states[i];
        const ArDdIdentity id{fam.bands[k], bc.station, bc.partner, bc.sat, bc.ref_sat};
        const auto f = mem.first_ep.find(id);
        const long long first = (f == mem.first_ep.end()) ? now_ep : f->second;
        first_ep[id]   = first;
        d.basis_age_ep = now_ep - first;
        d.cycle        = bc.cycle;
        d.cycle_id     = id;
    }
}

/// The run totals one epoch's log clause reads as differences.
struct ArCycleEpochFigures {
    long long rows{0}, fundamental{0}, lattice{0}, links{0}, links_kept{0};
    long long same{0}, mapped{0}, fresh_life{0}, fresh_new{0};
};

/// The run totals of the cycle-basis counters.
ArCycleEpochFigures arCycleEpochFiguresOf(const ArCycleBasisCounters& c) {
    ArCycleEpochFigures f;
    f.rows        = c.rows_partner + c.rows_borrowed;
    f.fundamental = c.rows_fundamental;
    f.lattice     = c.lattice_total;
    f.links       = c.links;
    f.links_kept  = c.links_kept;
    f.same        = c.map_same;
    f.mapped      = c.map_mapped;
    f.fresh_life  = c.map_fresh_life;
    f.fresh_new   = c.map_fresh_new;
    return f;
}

/// The cycle basis's search rows for every family of every constellation in the pools (card AGT,
/// step AGT-5; filter/NetworkArCycleBasis.hpp).  Per family: the rows with their life readings,
/// the forest and the basis, the map against the stored basis, the tallies and the search rows.
/// Each family's build is kept in @p epoch for the memory store after the search, and @p clause
/// receives the epoch's log clause; every band-arc's life reading goes to @p readings unless it
/// is null (the lattice observatory, step AGT-6).  Returns the (constellation, signal) pools
/// that took part.
int arCycleBasisRows(NetworkRunContext& ctx, bool sticky, const ArPools& pools, long long now_ep,
                     const std::vector<NetStation>& stations, const ArRequestOf& reqOf,
                     std::vector<DDrow>& dds, std::vector<ArCycleFamilyEpoch>& epoch,
                     std::string& clause, std::map<ArcId, ArArcReading>* readings) {
    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::duration<double, std::milli>;
    ArCycleBasisCounters& c = ctx.counters.netar_cycle;
    const ArCycleEpochFigures before = arCycleEpochFiguresOf(c);
    ++c.epochs;
    // The signals of each constellation, in pool order, with their arc counts.  A signal with no
    // wavelength is left out, as tree_basis leaves it out.
    std::map<std::string, std::vector<std::pair<std::string, std::size_t>>> signals_of;
    for (const auto& [cs, satmap] : pools) {
        const std::optional<SignalId> sig = SignalId::fromObsCode(systemFromName(cs.first), cs.second);
        const double lambda = sig ? sig->wavelengthM(0) : 0.0;
        if (!(std::isfinite(lambda) && lambda > 0.0)) continue;
        std::size_t n_arcs = 0;
        for (const auto& [sat, by_station] : satmap) { (void)sat; n_arcs += by_station.size(); }
        signals_of[cs.first].emplace_back(cs.second, n_arcs);
    }
    const ArBasisMemory none;
    const std::vector<arcgraph::IncumbentLink> no_links;
    int n_pools = 0;
    Ms stamp_ms{0.0}, build_ms{0.0}, map_ms{0.0};
    for (const auto& [cons, sigs] : signals_of) {
        for (const ArFamilySpec& fam : arCycleFamilies(systemFromName(cons), sigs)) {
            ArCycleFamilyEpoch fe;
            fe.sys = systemFromName(cons);
            fe.key = fam.bands;
            const ArBasisMemory* memp = &none;   // the family's memory in its constellation's row
            if (const auto cit = ctx.entities.constellations.find(fe.sys);
                cit != ctx.entities.constellations.end()) {
                const auto mit = cit->second.ar_basis.find(fe.key);
                if (mit != cit->second.ar_basis.end()) memp = &mit->second;
            }
            const ArBasisMemory& mem = *memp;
            const auto t0 = Clock::now();
            const ArCycleInput in = arCycleInputOf(ctx, fam, cons, pools, mem, now_ep, fe.arcs, c, readings);
            const auto t1 = Clock::now();
            stamp_ms += t1 - t0;
            if (in.stations.size() < 2) continue;
            const std::vector<arcgraph::IncumbentLink>& incumbent = sticky ? mem.links : no_links;
            fe.build = arCycleBuild(in, incumbent, sticky ? &mem.basis : nullptr);
            const auto t2 = Clock::now();
            const std::vector<ArMapState> states =
                arCycleMapStates(mem.basis, fe.build.basis, fe.build.invalidated);
            const auto t3 = Clock::now();
            build_ms += t2 - t1;
            map_ms   += t3 - t2;
            arCycleTally(c, fam, fe.build, states, incumbent, mem.basis.cycles.empty());
            arCycleRows(fam, cons, pools, fe.build, states, mem, now_ep, stations, reqOf, dds, fe.first_ep);
            n_pools += static_cast<int>(fam.bands.size());
            epoch.push_back(std::move(fe));
        }
    }
    c.stamp_ms.push_back(stamp_ms.count());
    c.build_ms.push_back(build_ms.count());
    c.map_ms.push_back(map_ms.count());
    const ArCycleEpochFigures now = arCycleEpochFiguresOf(c);
    clause = fmt::format(", cycle basis {} DD + {} fallback(s) of {} lattice direction(s), {}/{} "
                         "link(s) kept, map {} same / {} mapped / {} fresh (life) / {} fresh (new)",
                         now.rows - before.rows, now.fundamental - before.fundamental,
                         now.lattice - before.lattice, now.links_kept - before.links_kept,
                         now.links - before.links, now.same - before.same, now.mapped - before.mapped,
                         now.fresh_life - before.fresh_life, now.fresh_new - before.fresh_new);
    return n_pools;
}

/// Each family built at this search becomes its memory for the next search (after the search).
void arCycleBasisStore(net::ConstellationTable& memory, std::vector<ArCycleFamilyEpoch>& epoch,
                       long long now_ep) {
    for (ArCycleFamilyEpoch& fe : epoch) {
        ArBasisMemory& m = memory[fe.sys].ar_basis[fe.key];
        m.last_search_ep = now_ep;
        m.links          = arcgraph::incumbentLinksOf(fe.build.graph, fe.build.forest);
        m.basis          = std::move(fe.build.basis);
        m.arcs           = std::move(fe.arcs);
        m.first_ep       = std::move(fe.first_ep);
    }
}

/// Card FCF: every cycle-basis search row with its float, its variance and its covariance with
/// the same DD in the pair's other band (its twin: the same constellation, stations and
/// satellites, the other signal), and whether the search determined it.  A row is a twin only
/// when its identity occurs exactly twice, on two signals; the twins share one search block
/// (cons|sta_r), so their covariance is the readback's own.  Basis order.
std::vector<SearchedCycle> arSearchedCycles(const std::vector<DDrow>& dds, const Eigen::VectorXd& y_cyc,
                                            const Eigen::MatrixXd& Q_cyc,
                                            const std::vector<netar::DdFixLabel>& labels) {
    std::vector<SearchedCycle> out;
    out.reserve(dds.size());
    using PairKey = std::tuple<std::string, StationId, StationId, SatId, SatId>;
    std::map<PairKey, std::vector<int>> by_key;
    for (std::size_t a = 0; a < dds.size(); ++a) {
        const DDrow& d = dds[a];
        const netar::DdFixLabel& lb = labels[a];
        SearchedCycle s;
        s.id       = d.cycle_id;
        s.cycle    = d.cycle;
        s.y_cyc    = y_cyc(static_cast<Eigen::Index>(a));
        s.var_cyc2 = Q_cyc(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(a));
        s.fixed    = lb.kept;
        s.integer  = lb.kept ? static_cast<long long>(std::llround(lb.fixed_int)) : 0LL;
        const auto& [sig, sta_r, ref_sta, sat_j, ref_sat] = d.cycle_id;
        (void)sig;
        by_key[PairKey{d.cons, sta_r, ref_sta, sat_j, ref_sat}].push_back(static_cast<int>(a));
        out.push_back(std::move(s));
    }
    for (const auto& [key, rows] : by_key) {
        (void)key;
        if (rows.size() != 2) continue;
        const int a = rows[0], b = rows[1];
        if (std::get<0>(out[static_cast<std::size_t>(a)].id) == std::get<0>(out[static_cast<std::size_t>(b)].id))
            continue;
        out[static_cast<std::size_t>(a)].twin = b;
        out[static_cast<std::size_t>(b)].twin = a;
        const double c = Q_cyc(a, b);
        out[static_cast<std::size_t>(a)].cov_twin_cyc2 = c;
        out[static_cast<std::size_t>(b)].cov_twin_cyc2 = c;
    }
    return out;
}

}  // namespace

ArEpochFixes networkProducerAr(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::filesystem::path& log_dir_ = run.log_dir;
    // ---- End of the bindings ----
    // --- Alias preamble: byte-copies of the former Application::runNetwork aliases,
    //     so the relocated SB_NETAR body below reads the original names. ----------
    const auto& nc        = config_.getNetworkConfig();
    const auto& netar     = nc.ambiguity_resolution;
    auto&       n_emitted = ctx.counters.n_emitted;
    // Card AGT, step AGT-6: what this search hands the lattice observatory (filled only under
    // trace_lattice, which the loader arms only with the cycle basis).  Card FCF: the same
    // hand-over goes to the fixed-cycle fold, which also receives every searched DD's float
    // and every pooled arc's live column (fold_armed; the loader arms the fold only with the
    // cycle basis).  Neither is read by anything the solve or the emission reads unless the
    // fold is armed.
    ArEpochFixes lattice_fixes;
    const bool   lattice_armed = netar.cycle_basis && netar.trace_lattice;
    const bool   fold_armed    = netar.cycle_basis && netar.fixed_cycle_fold != FixedCycleFold::Off;
    const bool   handover      = lattice_armed || fold_armed;

    // --- Card ARL: the suspect-corner mask's per-epoch fold (filter/NetworkArSuspect.hpp).
    //     It runs ahead of the search gate, so the ledger's clock and stamps stay current
    //     through the epochs before min_epochs.  Window 0 (off): every arSuspect* call
    //     below is a no-op, nothing is read or written, and the search runs as before. --
    const int       suspect_window = ctx.knobs.ar_suspect_epochs;
    const long long suspect_now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    if (suspect_window > 0)
        arSuspectFoldEpoch(ctx.carry.ar_suspect, suspect_now_ep,
                           TimeUtils::continuousGpsSeconds(se.time), suspect_window,
                           ctx.carry.relevel, ctx.carry.local_carry,
                           &ctx.counters.netar_suspect);

    // --- Producer-side network AR (Phase 2: DD fixability attempt, log only) ---
    // The between-station double-difference ambiguities are datum-invariant
    // estimable quantities, so this fix attempt is independent of the phase-bias
    // datum in use.  Phase 2 is read-only (re-pin and re-solve belong to Phase 3):
    // it reconstructs the per-station float ambiguities, forms an independent DD
    // basis (star topology per (const,signal)), builds the exact DD covariance
    // from est.jointConditionalCovariance, converts to cycles, and runs
    // ils::ilsFix to log how many integers the network could fix.  Gated on
    // network.ambiguity_resolution.enable and min_epochs; byte-identical when off.
    if (netar.enable && n_emitted >= static_cast<size_t>(std::max(0, netar.min_epochs))
        && x_s.size() > 0) {
        // (const,signal) -> sat -> station index -> absolute float ambiguity + its
        // (est id, A_r column) for the covariance request (ArRec).  GPS+Galileo only.
        std::map<std::pair<std::string, std::string>, ArSatPool> ar_pool;
        // Card ARL: this search's mask, filled as the pool and the basis are built.
        ArSuspectSearch mask = arSuspectOpenSearch(suspect_now_ep, suspect_window);
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
            catch (...) { continue; }
            const auto& h2l = asm_res[i].h2local;
            const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
            for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                const System sys = systemOfSat(parts->satellite);
                if (sys != System::GPS && sys != System::Galileo)
                    continue;   // GPS+Gal integer-capable; GLONASS FDMA stays float
                const std::string cst = constellationKey(sys);
                if (stations[i].sm->isAmbiguityUnresolvable(key)) continue;
                const int amb_idx = static_cast<int>(sidx);
                if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                const auto hit = h2l.find(amb_idx);
                if (hit == h2l.end() || hit->second < 0
                    || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                const double value_m = x_lin(amb_idx) + x_r(hit->second);
                if (!std::isfinite(value_m)) continue;
                const ArSuspectCornerState suspect = arSuspectPoolCorner(
                    mask, ctx.carry.ar_suspect, static_cast<int>(i), parts->satellite);
                ar_pool[{cst, parts->signal}][parts->satellite][i] =
                    ArRec{value_m, static_cast<std::size_t>(est_index_of[i]),
                          static_cast<Eigen::Index>(hit->second), suspect};
                // Card FCF: the band-arc's live column, where a fixed cycle through it lands.
                if (fold_armed) {
                    const std::optional<SatId>    sat_id = SatId::parse(parts->satellite);
                    const std::optional<SignalId> sig_id = SignalId::fromObsCode(sys, parts->signal);
                    if (sat_id && sig_id)
                        lattice_fixes.columns[ArcId{StationId{static_cast<int>(i)}, *sat_id, *sig_id}] =
                            ArcColumn{static_cast<std::size_t>(est_index_of[i]),
                                      static_cast<std::ptrdiff_t>(hit->second), x_lin(amb_idx),
                                      sig_id->wavelengthM(0)};
                }
            }
        }
        // The covariance request: each pool entry's (est id, A_r column) once, in first-use
        // order.
        std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
        std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
        auto reqOf = [&](const ArRec& r) -> int {
            const std::pair<std::size_t, Eigen::Index> k{r.est_id, r.col};
            auto it = req_idx.find(k);
            if (it != req_idx.end()) return it->second;
            const int idx = static_cast<int>(reqs.size());
            reqs.push_back(k); req_idx[k] = idx; return idx;
        };
        // The DD basis, one search row per DD: card AGT's cycle basis, per family of a
        // constellation's signals (arCycleBasisRows), else card ARL's tree basis or the star,
        // per (constellation, signal) (arSignalBasisRows).  Under the cycle basis each
        // family's build is held in cycle_epoch and stored as its memory after the search.
        std::vector<DDrow> dds;
        std::vector<ArCycleFamilyEpoch> cycle_epoch;
        std::string cycle_text;
        const int n_const_signal =
            netar.cycle_basis
                ? arCycleBasisRows(ctx, netar.cycle_basis_sticky, ar_pool, suspect_now_ep, stations,
                                   reqOf, dds, cycle_epoch, cycle_text,
                                   handover ? &lattice_fixes.arcs : nullptr)
                : arSignalBasisRows(netar.tree_basis, ar_pool, se, stations, reqOf, dds, log);
        lattice_fixes.searched = handover;
        if (!dds.empty() && reqs.size() >= 2) {
            const bool ar_carry_aware = ctx.knobs.carry_aware_covariance;
            const int nd = static_cast<int>(dds.size());
            Eigen::VectorXd y_cyc(nd);
            std::vector<double> lam(nd);
            for (int a = 0; a < nd; ++a) {
                y_cyc(a) = dds[static_cast<std::size_t>(a)].dd_m / dds[static_cast<std::size_t>(a)].lambda;
                lam[static_cast<std::size_t>(a)] = dds[static_cast<std::size_t>(a)].lambda;
            }
            Eigen::MatrixXd Q_cyc;
            bool have_q = false;
            // Card ARL: the block search reads no cross-baseline entry, so in block mode
            // only each baseline's block of the covariance is read back
            // (blockConditionalCovariances) and differenced in place; the entries
            // between blocks stay zero.  Same expression as the uncorrected joint
            // readback inside each block.
            const bool block_cov = netar.decorrelated_partial && netar.block_search
                                && !ar_carry_aware && netar.ils_max_dim <= 0;
            if (block_cov) {
                // baseline (cycle basis: station) -> DDs, the search's own blocks
                std::map<std::string, std::vector<int>> dd_blocks;
                for (int a = 0; a < nd; ++a) {
                    const DDrow& d = dds[static_cast<std::size_t>(a)];
                    dd_blocks[netar::searchBlockKey(d.cons, d.sta_r, d.ref_sta, netar.cycle_basis)]
                        .push_back(a);
                }
                std::vector<std::vector<int>> req_blocks;
                std::vector<std::map<int, int>> pos_of;              // per block: req -> row
                for (const auto& [key, members] : dd_blocks) {
                    (void)key;
                    std::map<int, int> pos;
                    std::vector<int> rb;
                    for (int a : members) {
                        const DDrow& d = dds[static_cast<std::size_t>(a)];
                        for (int r : {d.i_rj, d.i_rp, d.i_fj, d.i_fp})
                            if (pos.emplace(r, static_cast<int>(rb.size())).second) rb.push_back(r);
                    }
                    req_blocks.push_back(std::move(rb));
                    pos_of.push_back(std::move(pos));
                }
                std::vector<Eigen::MatrixXd> covs;
                try { covs = est.blockConditionalCovariances(reqs, plan.shared_datum, req_blocks); }
                catch (...) { covs.clear(); }
                if (covs.size() == req_blocks.size()) {
                    Q_cyc = Eigen::MatrixXd::Zero(nd, nd);
                    std::size_t bi = 0;
                    for (const auto& [key, members] : dd_blocks) {
                        (void)key;
                        const Eigen::MatrixXd& Cb = covs[bi];
                        const std::map<int, int>& pos = pos_of[bi];
                        ++bi;
                        auto terms = [&](int a) {
                            const DDrow& d = dds[static_cast<std::size_t>(a)];
                            return std::array<std::pair<int, double>, 4>{{
                                {pos.at(d.i_rj), 1.0}, {pos.at(d.i_rp), -1.0},
                                {pos.at(d.i_fj), -1.0}, {pos.at(d.i_fp), 1.0}}};
                        };
                        for (std::size_t x = 0; x < members.size(); ++x) {
                            const auto ta = terms(members[x]);
                            for (std::size_t y = x; y < members.size(); ++y) {
                                const auto tb = terms(members[y]);
                                double v = 0.0;
                                for (const auto& [pa, sa] : ta)
                                    for (const auto& [pb, sb] : tb) v += sa * sb * Cb(pa, pb);
                                const int a = members[x], b = members[y];
                                const double q = v / (lam[static_cast<std::size_t>(a)] * lam[static_cast<std::size_t>(b)]);
                                Q_cyc(a, b) = q;
                                Q_cyc(b, a) = q;
                            }
                        }
                    }
                    have_q = true;
                }
            } else {
                Eigen::MatrixXd cov_undiff;
                // Carry-aware readback (docs/SPEC_carry_aware_covariance.md §6).  With the
                // knob off (default), Q_cyc, and with it FFR, ADOP and the partial-AR
                // ordering below, is built from a covariance the solve does not use; that
                // expression is kept unchanged.  With it on, the probe uses the
                // estimator's own covariance.
                try { cov_undiff = est.jointConditionalCovariance(reqs, plan.shared_datum,
                                                                  ar_carry_aware); }
                catch (...) { cov_undiff = Eigen::MatrixXd(); }
                if (cov_undiff.rows() == static_cast<Eigen::Index>(reqs.size())
                    && cov_undiff.rows() > 0) {
                    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(nd, static_cast<int>(reqs.size()));
                    for (int a = 0; a < nd; ++a) {
                        const DDrow& d = dds[static_cast<std::size_t>(a)];
                        D(a, d.i_rj) += 1.0; D(a, d.i_rp) -= 1.0;
                        D(a, d.i_fj) -= 1.0; D(a, d.i_fp) += 1.0;
                    }
                    Eigen::MatrixXd cov_dd_m = D * cov_undiff * D.transpose();
                    Q_cyc.resize(nd, nd);
                    for (int a = 0; a < nd; ++a)
                        for (int b = 0; b < nd; ++b)
                            Q_cyc(a, b) = cov_dd_m(a, b) / (lam[static_cast<std::size_t>(a)] * lam[static_cast<std::size_t>(b)]);
                    Q_cyc = 0.5 * (Q_cyc + Q_cyc.transpose());
                    have_q = true;
                }
            }
            if (have_q) {
                // --- ILS dimension cap (network.ambiguity_resolution.ils_max_dim) ---
                // MLAMBDA's search does not return on the uncapped full-network basis
                // (about 2500 DDs at 305 stations).  When armed (> 0), only the
                // ils_max_dim lowest-formal-σ DDs are searched (the standard partial-AR
                // subset, ascending Q_aa) and the rest stay float.  The summary
                // statistics (med σ / med frac / near-int) use the full basis, so the
                // convergence gauge is unaffected by the cap.
                // Card ARL: the suspect-corner mask.  A DD with a suspect corner is left
                // out of the search before the cap applies; it keeps its trace row
                // (in_subset 0).  mask.masked stays empty while the mask is off.
                if (mask.window > 0) {
                    std::vector<ArSuspectCornerState> dd_state;
                    dd_state.reserve(dds.size());
                    for (const DDrow& d : dds) dd_state.push_back(d.suspect);
                    arSuspectMaskBasis(mask, dd_state);
                }
                const bool capped =
                    netar.ils_max_dim > 0 && nd - mask.n_masked > netar.ils_max_dim;
                // search row -> basis index.  Computed outside the cap branch because
                // the trace has to invert it (see netar::mapFixToBasis).
                const std::vector<int> order =
                    netar::maskedSearchOrder(Q_cyc, netar.ils_max_dim, mask.masked);
                // The search vector and covariance: the basis itself when every DD is
                // searched (the order is then the identity), else the subset copied
                // column by column.  Q_cyc is stored by column and an uncapped masked
                // order is ascending, so the copy reads it nearly in storage order; at
                // 300 stations (about 4000 DDs, a 130 MB matrix) a row-by-row copy costs
                // half a second per epoch.
                const int nd_fix = static_cast<int>(order.size());
                Eigen::VectorXd y_fix;
                Eigen::MatrixXd Q_fix;
                if (nd_fix == nd) {
                    y_fix = y_cyc;
                    Q_fix = Q_cyc;
                } else {
                    y_fix.resize(nd_fix);
                    Q_fix.resize(nd_fix, nd_fix);
                    for (int b = 0; b < nd_fix; ++b) {
                        const int cb = order[static_cast<std::size_t>(b)];
                        y_fix(b) = y_cyc(cb);
                        for (int a = 0; a < nd_fix; ++a)
                            Q_fix(a, b) = Q_cyc(order[static_cast<std::size_t>(a)], cb);
                    }
                }
                ils::IlsGateConfig gates;
                gates.min_success_rate     = netar.min_success_rate;
                gates.ratio_threshold      = netar.ratio_threshold;
                gates.ratio_upper_bound    = netar.ratio_upper_bound;
                gates.nl_max_per_pair_frac = netar.nl_max_per_pair_frac;
                gates.enable_partial_ar    = netar.enable_partial_ar;
                ils::IlsResult fix;
                // An empty search (the mask left nothing) is not run: the result stays
                // the empty IlsResult, the same as a search that threw.
                if (nd_fix > 0) try {
                    // Card ARL: the decorrelated-space partial fix reports the
                    // single-signal DDs its fixed combinations determine.
                    if (netar.decorrelated_partial && netar.block_search) {
                        // One block per baseline (its L1 and L2 DDs): sta_r and ref_sta; under
                        // the cycle basis one block per station (card AGT: its partner and
                        // borrowed DDs in both bands).
                        std::map<std::string, int> block_id;
                        std::vector<int> block_of(static_cast<std::size_t>(nd_fix));
                        for (int s = 0; s < nd_fix; ++s) {
                            const DDrow& d = dds[static_cast<std::size_t>(order[static_cast<std::size_t>(s)])];
                            const auto it = block_id.emplace(
                                netar::searchBlockKey(d.cons, d.sta_r, d.ref_sta, netar.cycle_basis),
                                static_cast<int>(block_id.size())).first;
                            block_of[static_cast<std::size_t>(s)] = it->second;
                        }
                        fix = netar::blockDecorrelatedFix(y_fix, Q_fix, block_of, gates,
                                                          netar.decorrelated_determined_sigma_cyc);
                    } else {
                        fix = netar.decorrelated_partial
                            ? ils::ilsFixDecorrelatedPartial(y_fix, Q_fix, gates,
                                                             netar.decorrelated_determined_sigma_cyc)
                            : ils::ilsFix(y_fix, Q_fix, gates);
                    }
                }
                catch (...) { fix = ils::IlsResult(); }
                // Median DD formal σ (cycles) is the convergence gauge: as the
                // network float tightens toward cm it drops and the DDs become
                // fixable.  The median float fraction and the near-integer count
                // show whether the products are integer-consistent before they are
                // confidently fixable.
                std::vector<double> dsig; dsig.reserve(nd);
                std::vector<double> dfrac; dfrac.reserve(nd);
                int n_near = 0;   // DDs whose float is within 0.15 cyc of an integer
                for (int a = 0; a < nd; ++a) {
                    if (Q_cyc(a, a) > 0.0) dsig.push_back(std::sqrt(Q_cyc(a, a)));
                    const double f = std::abs(y_cyc(a) - std::round(y_cyc(a)));
                    dfrac.push_back(f);
                    if (f <= 0.15) ++n_near;
                }
                auto med = [](std::vector<double>& v) {
                    if (v.empty()) return 0.0;
                    std::sort(v.begin(), v.end());
                    return v[v.size() / 2];
                };
                const double med_sig  = med(dsig);
                const double med_frac = med(dfrac);
                // The search mapped back onto the basis (the trace's columns, and the
                // mask's re-entry outcome).
                const std::vector<netar::DdFixLabel> labels =
                    netar::mapFixToBasis(nd, order, fix.kept, fix.afix);
                // Card AGT, step AGT-6: the determined DDs (the trace's kept rows) as cycles,
                // the lattice observatory's offers.
                if (handover)
                    for (int a = 0; a < nd; ++a) {
                        const netar::DdFixLabel& lb = labels[static_cast<std::size_t>(a)];
                        if (!lb.kept) continue;
                        const DDrow& d = dds[static_cast<std::size_t>(a)];
                        lattice_fixes.fixed.push_back(
                            ArFixedCycle{d.cycle, static_cast<long long>(std::llround(lb.fixed_int)), d.cycle_id});
                    }
                // Card FCF: every searched DD with its float from this readback, the twin in
                // the pair's other band, and whether it was determined.
                if (fold_armed)
                    lattice_fixes.floats = arSearchedCycles(dds, y_cyc, Q_cyc, labels);
                // Card ARL pair lanes (decorrelated_partial only): each L1/L2 pair's wide
                // lane before the fix and its wide lane and narrow lane conditioned on the
                // fixed decorrelated components, so a pair left float can be read as "wide
                // lane fixed, narrow lane not" or "neither".  Built for the trace and for
                // the mask's re-entered pairs.
                std::vector<netar::PairLane> lanes;
                if (netar.decorrelated_partial
                    && (traces.producer_ar_trace.is_open() || mask.n_reentry > 0)) {
                    std::vector<netar::DdPairKey> keys;
                    keys.reserve(dds.size());
                    for (const DDrow& d : dds)
                        keys.push_back({d.cons + "|" + d.sat_j + "|" + d.ref_sat + "|"
                                            + d.sta_r + "|" + d.ref_sta,
                                        d.lambda});
                    lanes = netar::pairLanes(keys, order, nd_fix, Q_fix, fix.a_cond, fix.Q_cond);
                }
                // Card ARL: close the mask's search epoch (both edges tallied, the released
                // corners leave the ledger's masking set) and take its log clause; empty
                // while the mask is off.
                const std::string mask_text = arSuspectCloseSearch(
                    mask, ctx.carry.ar_suspect, ctx.counters.netar_suspect,
                    netar::reentryOutcome(mask.reentry, labels, lanes,
                                          netar.decorrelated_determined_sigma_cyc),
                    se.time.tow);
                // The covariance mode is logged on the same line because med_DD_sigma
                // and every ratio/p_succ derived from the covariance mean different
                // things under the two forms (docs/SPEC_carry_aware_covariance.md §4).
                log->info("Network AR (Phase 2, log-only): TOW {:.1f} — {} DD / {} undiff-amb / "
                          "{} (const,signal); fixed {}{} of {} searched{} (ratio {:.2f}, "
                          "p_succ {:.4f}, med_DD_sigma {:.4f} cyc, med_DD_frac {:.3f}, "
                          "near_int {}/{}), cov {}{}{}{}",
                          se.time.tow, nd, static_cast<int>(reqs.size()), n_const_signal,
                          fix.ok ? static_cast<int>(fix.kept.size()) : 0,
                          fix.partial ? " partial" : "", nd_fix,
                          capped ? " (ILS-capped)" : "", fix.ratio, fix.p_succ,
                          med_sig, med_frac, n_near, nd,
                          ar_carry_aware ? "CARRY-AWARE" : "carry-blind",
                          netar.decorrelated_partial
                              ? fmt::format(", decorrelated {} of {} fixed", fix.z_fixed, fix.z_total)
                              : std::string(),
                          mask_text, cycle_text);
                // --- Run-end tallies (invariant #4): the log line above is per-epoch;
                //     the tallies give the run a denominator, so an armed but inert
                //     search is distinguishable from a quiet one. ---------------------
                auto& c = ctx.counters;
                ++c.netar_epochs;
                c.netar_carry_aware = ar_carry_aware;
                c.netar_n_dd.push_back(static_cast<double>(nd));
                c.netar_n_search.push_back(static_cast<double>(nd_fix));
                c.netar_med_sigma.push_back(med_sig);
                const int n_fixed = fix.ok ? static_cast<int>(fix.kept.size()) : 0;
                if (fix.ok) {
                    ++c.netar_fix_epochs;
                    c.netar_dd_fixed += n_fixed;
                    // ratio / p_succ describe an accepted fix; on a rejected epoch they
                    // are failure diagnostics, and one median over both populations
                    // cannot be interpreted.  Only accepted epochs are tallied.
                    c.netar_ratio.push_back(fix.ratio);
                    c.netar_p_succ.push_back(fix.p_succ);
                }
                // --- Per-DD trace (network.ambiguity_resolution.trace) --------------
                // Every DD in the basis gets a row, not only the searched subset: the
                // fixability ceiling the grader reports (share of DDs whose float is
                // within 0.25 cyc of truth) refers to the population the cap chose
                // from, which a subset-only trace cannot provide.
                if (traces.producer_ar_trace.is_open()) {
                    auto& os = traces.producer_ar_trace;
                    // ratio / p_succ: the accepted fix's values on a fixed epoch, the
                    // full-set attempt's otherwise.  IlsResult zeroes the accepted fields
                    // when nothing was accepted, so on most epochs they would be a column
                    // of zeros; the full-set values are the failure diagnostics.
                    // `n_fixed` on the same row separates the two populations (the
                    // grader splits on it).
                    const double ep_ratio  = fix.ok ? fix.ratio  : fix.full_ratio;
                    const double ep_psucc  = fix.ok ? fix.p_succ : fix.full_p_succ;
                    // The pair columns (decorrelated_partial only; empty vector otherwise).
                    const std::vector<std::string> pair_cols = netar::pairLaneText(lanes);
                    for (int a = 0; a < nd; ++a) {
                        const DDrow& d = dds[static_cast<std::size_t>(a)];
                        const auto& lb = labels[static_cast<std::size_t>(a)];
                        const double sig = Q_cyc(a, a) > 0.0 ? std::sqrt(Q_cyc(a, a)) : -1.0;
                        // Card ARL mask columns (armed only, kProducerArSuspectColumns): the
                        // DD's suspect cause bits and its re-entry flag.
                        const std::string mask_cols =
                            mask.window > 0
                                ? fmt::format(",{},{}",
                                              static_cast<int>(mask.dd_bits[static_cast<std::size_t>(a)]),
                                              static_cast<int>(mask.reentry[static_cast<std::size_t>(a)]))
                                : std::string();
                        // Card AGT cycle-basis columns (armed only, kProducerArCycleBasisColumns).
                        const std::string cycle_cols =
                            netar.cycle_basis
                                ? fmt::format(",{},{},{},{}", arCycleKindName(d.cycle_kind),
                                              d.band_only ? 1 : 0, d.basis_age_ep,
                                              arMapStateName(d.map_state))
                                : std::string();
                        os << fmt::format("{},{:.1f},{},{},{},{},{},{},{:.9f},{:.6f},"
                                          "{:.6f},{},{},{},{:.4f},{:.6f},{},{},{}{}{}{}\n",
                                          se.time.week, se.time.tow,
                                          d.cons, d.signal, d.sat_j, d.ref_sat,
                                          d.sta_r, d.ref_sta, d.lambda,
                                          y_cyc(a), sig,
                                          lb.in_subset ? 1 : 0,
                                          lb.kept ? static_cast<long long>(std::llround(lb.fixed_int)) : 0LL,
                                          lb.kept ? 1 : 0,
                                          ep_ratio, ep_psucc, nd, nd_fix, n_fixed,
                                          pair_cols.empty() ? std::string()
                                                            : pair_cols[static_cast<std::size_t>(a)],
                                          mask_cols, cycle_cols);
                    }
                    c.netar_rows += nd;
                }
            }
        }
        // Card AGT: each family's build becomes its memory for the next search, whether or not
        // this one ran (no covariance, no row): stickiness follows the basis, not the search.
        if (netar.cycle_basis)
            arCycleBasisStore(ctx.entities.constellations, cycle_epoch, suspect_now_ep);
    }

    // --- One-shot full-arrowhead NEQ export (network.neq_export_epoch) -----------
    // Post-solve, read-only dump of the assembled normal equations for offline
    // spectral / weak-direction attribution: border N_ss (raw and reduced) with
    // RHS, every station's N_sr / N_rr (rebuilt from its LDLᵀ) / u_r as row-major
    // little-endian double .bin, plus shared-column and per-station local-column
    // labels.  The solution is untouched; fires on the epoch where
    // n_emitted + 1 == neq_export_epoch, once per run.
    if (nc.neq_export_epoch > 0
        && n_emitted + 1 == static_cast<size_t>(nc.neq_export_epoch)
        && x_s.size() > 0) {
        const std::filesystem::path dir = log_dir_ / "neq_export";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto appendBin = [](std::ofstream& f, const Eigen::MatrixXd& M) {
            // Explicit row-major copy so the offline reader needs no
            // storage-order convention (Eigen's default is column-major).
            const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic,
                                Eigen::RowMajor> R = M;
            f.write(reinterpret_cast<const char*>(R.data()),
                    static_cast<std::streamsize>(sizeof(double) * R.size()));
        };
        auto sanitize = [](std::string s) {
            for (char& ch : s) if (ch == ',') ch = ';';
            return s;
        };
        const auto& kernel = est.normals();
        {
            std::ofstream f(dir / "Nss_raw.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.sharedNormals());
        }
        {
            std::ofstream f(dir / "us_raw.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.sharedRhs());
        }
        {
            std::ofstream f(dir / "Nss_red.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.reducedSharedNormals());
        }
        {
            std::ofstream f(dir / "us_red.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.reducedSharedRhs());
        }
        {
            std::ofstream f(dir / "shared_labels.csv", std::ios::trunc);
            f << "col,label\n";
            for (Eigen::Index c = 0; c < est.numShared(); ++c)
                f << c << ',' << sanitize(est.sharedColumnLabel(c)) << '\n';
        }
        std::ofstream sidx(dir / "stations_index.csv", std::ios::trunc);
        sidx << "est_slot,name,n_local\n";
        std::ofstream slab(dir / "stations_labels.csv", std::ios::trunc);
        slab << "est_slot,local_col,label\n";
        std::ofstream nsr(dir / "stations_Nsr.bin", std::ios::binary | std::ios::trunc);
        std::ofstream nrr(dir / "stations_Nrr.bin", std::ios::binary | std::ios::trunc);
        std::ofstream urf(dir / "stations_ur.bin", std::ios::binary | std::ios::trunc);
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const auto r = static_cast<std::size_t>(est_index_of[i]);
            const Eigen::Index nl = kernel.numLocal(r);
            sidx << r << ',' << stations[i].name << ',' << nl << '\n';
            appendBin(nsr, kernel.stationCrossBlock(r));
            appendBin(nrr, kernel.stationLocalNormals(r));
            appendBin(urf, kernel.stationLocalRhs(r));
            // Local col → StateManager label via the assembly's h2local inverse;
            // columns with no SM mapping (none expected) stay "?".
            std::vector<std::string> names(static_cast<std::size_t>(nl), "?");
            const auto sm_labels = stations[i].sm->getStateLabels();
            for (const auto& kv : asm_res[i].h2local) {
                const auto lc = static_cast<Eigen::Index>(kv.second);
                if (lc >= 0 && lc < nl
                    && static_cast<std::size_t>(kv.first) < sm_labels.size())
                    names[static_cast<std::size_t>(lc)] =
                        sm_labels[static_cast<std::size_t>(kv.first)];
            }
            for (Eigen::Index c = 0; c < nl; ++c)
                slab << r << ',' << c << ','
                     << sanitize(names[static_cast<std::size_t>(c)]) << '\n';
        }
        {
            std::ofstream meta(dir / "meta.csv", std::ios::trunc);
            meta << "week,tow,epoch,n_shared,n_stations\n"
                 << se.time.week << ',' << se.time.tow << ','
                 << nc.neq_export_epoch << ',' << est.numShared() << ','
                 << kernel.numStations() << '\n';
        }
        log->info("Network: NEQ export written to {} (epoch {}, n_shared {}, "
                  "{} station blocks)",
                  dir.string(), nc.neq_export_epoch, est.numShared(),
                  kernel.numStations());
    }
    return lattice_fixes;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the producer ambiguity-resolution search.
int networkProducerArRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;
    const NetworkConfig& nc = v.config.getNetworkConfig();

    // --- Producer-AR Phase-2 diagnostic run-end readout ------------------------------
    // Run-level summary of the producer AR search.  The per-epoch spdlog line describes
    // one epoch; this readout describes the run, following §4.1 of the 2026-09-10 note in
    // docs/network_ambiguity_resolution_design.md ("a fix count is not a grade").
    // `netar_epochs == 0` on an armed run means the search never reached the ILS and
    // every number below is structurally empty; that case gets its own warning
    // (invariant #4).
    if (!nc.ambiguity_resolution.enable) return 0;
    auto& c = ctx.counters;
    auto p50 = [](std::vector<double> v) {
        if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // ratio and p_succ are collected on accepted-fix epochs only (a rejected epoch's
    // IlsResult zeroes them), so on a run that never fixed the sample is empty.
    // "n/a" says so; a printed "nan" or "0.00" would read like a measured value.
    auto p50s = [&](const std::vector<double>& v, int prec) {
        return v.empty() ? std::string("n/a")
                         : fmt::format("{:.{}f}", p50(v), prec);
    };
    if (c.netar_epochs > 0) {
        const double fix_pct = 100.0 * static_cast<double>(c.netar_fix_epochs)
                                      / static_cast<double>(c.netar_epochs);
        log->info("Network (producer AR, Phase 2 log-only): {} epochs searched, "
                  "{:.0f} DD p50 / {:.0f} searched p50, fixed epochs {} ({:.1f}%), "
                  "DD fixed total {}, ratio p50 {}, p_succ p50 {}, "
                  "med_DD_sigma p50 {:.4f} cyc; carry-{}; trace {}",
                  c.netar_epochs, p50(c.netar_n_dd), p50(c.netar_n_search),
                  c.netar_fix_epochs, fix_pct, c.netar_dd_fixed,
                  p50s(c.netar_ratio, 2), p50s(c.netar_p_succ, 4), p50(c.netar_med_sigma),
                  c.netar_carry_aware ? "aware" : "blind",
                  nc.ambiguity_resolution.trace ? "on" : "off");
        if (nc.ambiguity_resolution.trace) {
            if (c.netar_rows > 0)
                log->info("Network (producer AR, Phase 2): {} per-DD trace row(s) "
                          "written to {} — grade with "
                          "scripts/simnet_producer_ar_grade.py (a fix count is not a "
                          "grade; the DD basis is a STAR against one reference "
                          "satellite and one reference station, so report or "
                          "difference away that pair's share).",
                          c.netar_rows, traces.producer_ar_trace_path.string());
            else
                log->warn("Network (producer AR, Phase 2): the trace is ARMED and the "
                          "search ran {} epoch(s), yet ZERO rows were written — the "
                          "stream never opened (check the warning above) and this run "
                          "carries NO gradeable AR evidence.", c.netar_epochs);
        }
        if (c.netar_fix_epochs > 0 && !c.netar_carry_aware)
            log->warn("Network (producer AR, Phase 2): {} epoch(s) accepted a fix on a "
                      "CARRY-BLIND covariance (network.carry_aware_covariance is OFF), "
                      "so every gate the fix passed — FFR, ratio, the per-pair frac cap "
                      "— was computed from a covariance the SOLVE never used "
                      "(docs/SPEC_carry_aware_covariance.md §4).  A WRONG fix at "
                      "p_succ >= min_success_rate falsifies that covariance as a gate "
                      "input; grade the integers against truth before reading the fix "
                      "rate as a success.", c.netar_fix_epochs);
    } else {
        log->warn("Network (producer AR, Phase 2 log-only): ARMED but the ILS search "
                  "ran on ZERO epochs — min_epochs={} never cleared, the joint "
                  "conditional covariance never returned, or the DD basis never had "
                  "two undifferenced columns.  Every producer-AR number for this run "
                  "is structurally empty (standing invariant #4); this run is evidence "
                  "about NOTHING in the card.", nc.ambiguity_resolution.min_epochs);
    }
    // Card AGT: the cycle basis's clause (filter/NetworkArCycleBasis.hpp), armed only.
    if (nc.ambiguity_resolution.cycle_basis)
        arCycleBasisRunEndClause(c.netar_cycle, nc.ambiguity_resolution.cycle_basis_sticky, log);
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
