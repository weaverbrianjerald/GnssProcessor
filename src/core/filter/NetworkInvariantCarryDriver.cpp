/**
 * @file NetworkInvariantCarryDriver.cpp
 * @brief Card 4 (M1) phase C3b — implementation of the DD invariant-carry driver.
 *        See NetworkInvariantCarryDriver.hpp for the design rationale.
 */

#include "filter/NetworkInvariantCarryDriver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace GnssProcessor {

namespace {

/// INC-C0: RAII wall-clock accumulator (mirrors NetworkNormalEquations' ScopedAccum) —
/// adds the elapsed ms to @p acc on scope exit iff @p on.  No clock reads when off.
struct ScopedAccum {
    bool on;
    double* acc;
    std::chrono::steady_clock::time_point t0;
    ScopedAccum(bool on_, double* acc_) : on(on_), acc(acc_) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~ScopedAccum() {
        if (on)
            *acc += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
    }
};

/// WL-3 §3.1: the unit-normalized (NL, WL) lane ROTATION for a band pair — rows are
/// orthonormal lane directions in (z₁, z₂) meter space.  Row 0 is the phase-determined
/// (NL) direction, row 1 the WL lane = the unit-normalized WL functional (1/λ₁, −1/λ₂).
/// RᵀR = I exactly (up to fp), so Rᵀ·diag(q_nl, q_wl)·R with q_nl == q_wl reduces to the
/// isotropic diagonal — the §6.1 equivalence shape needs no special case here.
inline Eigen::Matrix2d laneRotation(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    const double n = std::sqrt(a * a + b * b);
    Eigen::Matrix2d R;
    R << b / n,  a / n,    // NL (⊥ the WL functional)
         a / n, -b / n;    // WL lane
    return R;
}

/// WL-3: one WL cycle expressed in the (unit-normalized) lane COORDINATE is 1/‖(1/λ₁,−1/λ₂)‖
/// state-meters — the σ conversion for @c Config::sigma_wl_fresh_cyc.
inline double laneMetersPerCycle(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    return 1.0 / std::sqrt(a * a + b * b);
}

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

}  // namespace

void NetworkInvariantCarryDriver::reset() {
    carried_.clear();
    block_meta_.clear();
    last_innov_flags_.clear();
    last_innov_sigmas_.clear();
    gate_exempt_.clear();
    lane_fix_w_.clear();       // WL-3: the fold-on-change memory dies with the carry
    lane_fix_staged_.clear();
    counters_ = Counters{};
}

void NetworkInvariantCarryDriver::beginEpoch(double dt_s, double content_s, bool clean) {
    // D5 v2: advance the σ̂₀² accumulator's exponential forgetting (once per epoch, BEFORE
    // this epoch's rows are folded in installPosterior) — independent of q_z.
    if (inflation_) inflation_->beginEpoch(content_s, clean);
    if (cfg_.qz <= 0.0 || dt_s <= 0.0) return;   // constant DDs (default) → no q_z op
    ScopedAccum _tu(perf_timing_, &perf_timers_.time_update_ms);   // INC-C0
    const double add = cfg_.qz * dt_s;
    // WL-3 §3.1: lane-aware process noise on pair blocks — Q = Rᵀ·diag(q_nl, q_wl)·R per
    // chord (q_nl = qz, q_wl = q_wl_scale·qz).  At q_wl_scale == 1 this is EXACTLY the
    // isotropic diagonal, so the plain add is used (byte-equal to the per-band shape).
    const bool lane_q = cfg_.lane_joint && cfg_.q_wl_scale != 1.0;
    for (auto& cb : carried_) {
        if (cb.Pz.rows() == 0) continue;
        if (!lane_q || !cb.pair || !(cb.lambda1 > 0.0 && cb.lambda2 > 0.0)) {
            cb.Pz.diagonal().array() += add;
            continue;
        }
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        if (cb.Pz.rows() != 2 * K) { cb.Pz.diagonal().array() += add; continue; }  // defensive
        const Eigen::Matrix2d R = laneRotation(cb.lambda1, cb.lambda2);
        const Eigen::Matrix2d Q = R.transpose()
            * Eigen::Vector2d(cfg_.qz, cfg_.qz * cfg_.q_wl_scale).asDiagonal()
            * R * dt_s;
        for (Eigen::Index k = 0; k < K; ++k) {
            cb.Pz(k, k)         += Q(0, 0);
            cb.Pz(K + k, K + k) += Q(1, 1);
            cb.Pz(k, K + k)     += Q(0, 1);
            cb.Pz(K + k, k)     += Q(1, 0);
        }
    }
}

double NetworkInvariantCarryDriver::innovInflationSigma0() const {
    if (!inflation_) return 1.0;
    const NetworkCarryVce::Estimate est = inflation_->estimate(kInflationFamily, kInflationSat);
    if (!(est.n_eff >= kInflationWarmupRows) || !(est.robust_floor > 0.0)) return 1.0;  // warm-up ⇒ raw formal (v1)
    double f2 = est.robust_floor;                       // σ̂₀² = geo_mean(sᵢ²)/χ²₁-geomean
    f2 = std::min(std::max(f2, 1.0), kInflationFactorCap);  // only RAISE the threshold; cap a diverged σ̂₀
    return std::sqrt(f2);
}

Eigen::Index NetworkInvariantCarryDriver::totalZDim() const {
    Eigen::Index t = 0;
    for (const auto& cb : carried_) t += cb.z.size();
    return t;
}

std::vector<NetworkNormalEquations::DdPriorBlock>
NetworkInvariantCarryDriver::buildDdPriors(const std::vector<GroupInput>& groups) {
    std::vector<NetworkNormalEquations::DdPriorBlock> out;
    block_meta_.clear();
    lane_fix_staged_.clear();   // WL-3: per-build staging (a RAIM re-solve re-derives it)
    ++counters_.epochs;
    if (perf_timing_) ++perf_timers_.build_epochs;   // INC-C0

    // The carry we project FROM this epoch; `carried_` is rebuilt in installPosterior.
    const std::vector<CarriedBlock>& prev = carried_;
    // Track, per previous block, how many NEW components it contributes to (split ⇒ >1).
    std::vector<int> prev_hit(prev.size(), 0);

    for (const GroupInput& g : groups) {
        // --- (1) Assemble this epoch's graph for the group. -------------------
        struct ArcInfo { int est_station; int local_col; int local_col2; bool tree_edge; };
        std::map<ArcKey, ArcInfo> arcinfo;
        std::set<ArcKey>          slipped_arcs;   // C3c: arcs whose carried ambiguity slipped this epoch
        UnionFind uf;
        std::vector<ArcKey> group_tree_edges;   // for per-component tree extraction
        std::set<ArcKey> slipped2_arcs;   // WL-3 pair: band-2's OWN slips (see ArcInput::slipped2)
        for (const ArcInput& a : g.arcs) {
            if (a.rx_vertex == a.sat_vertex) continue;         // degenerate (defensive)
            const ArcKey k(a.rx_vertex, a.sat_vertex);
            arcinfo[k] = {a.est_station, a.local_col, a.local_col2, a.tree_edge};
            uf.unite(a.rx_vertex, a.sat_vertex);
            if (a.tree_edge) group_tree_edges.push_back(k);
            if (a.slipped) slipped_arcs.insert(k);
            if (g.pair && a.slipped2) slipped2_arcs.insert(k);
        }
        if (arcinfo.empty()) continue;
        // WL-3: a pair chord tallies like the TWO per-band DDs it co-locates (§6.1 keeps
        // the lifecycle counters comparable across the regroup).
        const long long tally = g.pair ? 2 : 1;

        // --- (2) Partition arcs into connected components (root → arcs). ------
        std::map<int, std::vector<ArcKey>> comp_arcs;
        std::map<int, std::set<int>>       comp_vertices;
        for (const auto& [k, info] : arcinfo) {
            const int root = uf.find(k.u);
            comp_arcs[root].push_back(k);
            comp_vertices[root].insert(k.u);
            comp_vertices[root].insert(k.v);
        }

        // Previous blocks of THIS group, for the vertex-overlap contributor gather.
        std::vector<std::size_t> group_prev;
        for (std::size_t p = 0; p < prev.size(); ++p)
            if (prev[p].group == g.key) group_prev.push_back(p);

        // --- (3) Reconcile each new component. --------------------------------
        for (auto& [root, arcs] : comp_arcs) {
            const std::set<int>& verts = comp_vertices[root];

            // Tree edges / chords of this component.
            std::vector<ArcKey> tree_edges, chords;
            for (const ArcKey& k : arcs)
                (arcinfo.at(k).tree_edge ? tree_edges : chords).push_back(k);
            // WL-3: a pair group has no upstream election of its own — its arc set is
            // the INTERSECTION of two per-band graphs, and the caller's (band-1) tree
            // tags need not span this subgraph (per-(rx,sat) channel heterogeneity
            // fragments the primary pairs across groups; measured ~7% tag mismatch at
            // 50 stations).  Prefer the tags, then COMPLETE the spanning forest
            // deterministically (ArcKey order) by promoting chords that connect
            // disjoint tree fragments.  Homogeneous regions keep the eq34 basis
            // EXACTLY (the §6.1 equivalence); fragmented regions stay carried instead
            // of being dropped wholesale by the tree guards below.
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
            if (chords.empty()) continue;   // spanning tree only → no DD to carry

            // The cycle algebra: build a phase-B ZComponent from the tree.  A
            // malformed (non-spanning / cyclic) tree from the election is not
            // representable — skip the component rather than fold a wrong cycle.
            ZComponent zc;
            try {
                zc = ZComponent(tree_edges);
            } catch (const std::exception&) {
                continue;
            }
            // Every chord's endpoints must be tree-connected (guard fundamentalCycle).
            bool tree_ok = true;
            for (const ArcKey& c : chords)
                if (!zc.hasVertex(c.u) || !zc.hasVertex(c.v)) { tree_ok = false; break; }
            if (!tree_ok) continue;

            // Gather contributing previous blocks (any shared vertex), concatenated
            // block-diagonally into the old basis (they were independent components).
            std::vector<ArcKey> old_chords;
            std::set<ArcKey>    A_old;
            std::vector<const CarriedBlock*> contributors;
            for (std::size_t p : group_prev) {
                const CarriedBlock& ob = prev[p];
                if (ob.pair != g.pair) continue;   // WL-3 defensive: shape mismatch cannot project
                bool overlap = false;
                for (int v : ob.vertices) if (verts.count(v)) { overlap = true; break; }
                if (!overlap) continue;
                contributors.push_back(&ob);
                ++prev_hit[p];
                old_chords.insert(old_chords.end(), ob.chords.begin(), ob.chords.end());
                A_old.insert(ob.arcs.begin(), ob.arcs.end());
            }
            if (contributors.size() > 1) counters_.merges += tally;
            const Eigen::Index n_old = static_cast<Eigen::Index>(old_chords.size());
            // Block-diagonal old covariance / mean over the contributors.  WL-3 pair
            // blocks are stored [band-1; band-2], so the concatenation keeps the two
            // band halves contiguous: contributor j's band-1 lands at [off, off+k) and
            // its band-2 at [n_old+off, n_old+off+k) (with the four Pᶻ sub-blocks placed
            // accordingly, cross-band included — zero unless the lane shape retained it).
            const Eigen::Index zdim_old = g.pair ? 2 * n_old : n_old;
            Eigen::VectorXd z_old(zdim_old);
            Eigen::MatrixXd Pz_old = Eigen::MatrixXd::Zero(zdim_old, zdim_old);
            {
                Eigen::Index off = 0;
                for (const CarriedBlock* ob : contributors) {
                    const Eigen::Index k = static_cast<Eigen::Index>(ob->chords.size());
                    if (k == 0) continue;
                    if (!g.pair) {
                        z_old.segment(off, k) = ob->z;
                        Pz_old.block(off, off, k, k) = ob->Pz;
                    } else {
                        z_old.segment(off, k)         = ob->z.head(k);
                        z_old.segment(n_old + off, k) = ob->z.tail(k);
                        Pz_old.block(off, off, k, k)                 = ob->Pz.topLeftCorner(k, k);
                        Pz_old.block(n_old + off, n_old + off, k, k) = ob->Pz.bottomRightCorner(k, k);
                        Pz_old.block(off, n_old + off, k, k)         = ob->Pz.topRightCorner(k, k);
                        Pz_old.block(n_old + off, off, k, k)         = ob->Pz.bottomLeftCorner(k, k);
                    }
                    off += k;
                }
            }
            // Map an old chord arc → its column index in the old basis.
            std::map<ArcKey, Eigen::Index> old_col;
            for (Eigen::Index j = 0; j < n_old; ++j) old_col[old_chords[static_cast<std::size_t>(j)]] = j;

            // --- Per new chord: fundamental cycle, foldability, carried vs fresh.
            std::vector<ArcKey>                              fold_chords;
            std::vector<char>                                fold_carried;  // D5 v2: 1 = carried prior, 0 = born.  WL-3 pair: the BAND-1 mask.
            std::vector<char>                                fold_carried2; // WL-3 pair: the BAND-2 carried mask (== band-1 under the lane shape's §3.4 union; per-band under the §6.1 equivalence shape).
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows;
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows2;  // WL-3 pair: band-2 rows (same cycle, band-2 columns)
            std::vector<Eigen::VectorXd>                     Wrows;   // over old basis (empty ⇒ fresh)
            for (const ArcKey& c : chords) {
                const std::map<ArcKey, int> cyc = zc.fundamentalCycle(c);

                // Foldable ⇔ every cycle arc resolves to a live (station, local col) —
                // for a WL-3 pair group, in BOTH bands (membership requires band-2, §2).
                bool foldable = true;
                std::vector<NetworkNormalEquations::DdPriorBlock::Term> terms, terms2;
                terms.reserve(cyc.size());
                if (g.pair) terms2.reserve(cyc.size());
                for (const auto& [arc, coeff] : cyc) {
                    const ArcInfo& ai = arcinfo.at(arc);   // every cycle arc is in this component's graph
                    if (ai.est_station < 0 || ai.local_col < 0
                        || (g.pair && ai.local_col2 < 0)) { foldable = false; break; }
                    terms.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col,
                                     static_cast<double>(coeff)});
                    if (g.pair)
                        terms2.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col2,
                                          static_cast<double>(coeff)});
                }
                if (!foldable) { counters_.chords_dropped += tally; continue; }

                // INC-C0 (D-c viability): the fundamental-cycle length is the tree-path span of
                // this DD; short cycles ⇒ sparse DD information matrix J = Pᶻ⁻¹ ⇒ the exact
                // info-form carry is bounded, long cycles ⇒ dense J ⇒ it is not.  Measured on
                // FOLDABLE chords (the ones that enter the carry).
                {
                    const long long clen = static_cast<long long>(cyc.size());
                    if (clen > counters_.max_cycle_len) counters_.max_cycle_len = clen;
                    counters_.sum_cycle_len += tally * clen;
                }

                // Carried ⇔ the whole cycle lived last epoch (⊆ A_old) AND no cycle arc
                // slipped this epoch; then the DD is a linear combination of the old
                // chords with w_j = χ_c(arc of old chord j).  A cycle slip shifts the
                // true DD by an integer, so the q_z=0 carry pins a stiff WRONG prior
                // (Card 4 C3c) — EXCLUDE the slipped arc from the A_old membership so a
                // cycle through it re-taps fresh (data-only tap-off).  This is exact in
                // the projection-W architecture: for a surviving carried chord the
                // slipped-arc terms cancel from the sum (its cycle avoids the slipped
                // arc), so keeping the (zero-weighted) old chords needs no explicit drop.
                bool would_carry = n_old > 0;
                if (would_carry)
                    for (const auto& [arc, coeff] : cyc)
                        if (!A_old.count(arc)) { would_carry = false; break; }
                bool slip_in_cycle = false;
                if (!slipped_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped_arcs.count(arc)) { slip_in_cycle = true; break; }

                if (!g.pair) {
                    const bool carried = would_carry && !slip_in_cycle;
                    Eigen::VectorXd w;
                    if (carried) {
                        w = Eigen::VectorXd::Zero(n_old);
                        for (Eigen::Index j = 0; j < n_old; ++j) {
                            auto it = cyc.find(old_chords[static_cast<std::size_t>(j)]);
                            if (it != cyc.end()) w(j) = static_cast<double>(it->second);
                        }
                        ++counters_.chords_carried;
                    } else {
                        ++counters_.chords_born;
                        if (would_carry && slip_in_cycle) ++counters_.slip_kills;  // a carried DD lost to a slip
                    }
                    fold_chords.push_back(c);
                    fold_carried.push_back(carried ? 1 : 0);
                    rows.push_back(std::move(terms));
                    Wrows.push_back(std::move(w));   // empty vector marks a fresh chord
                    continue;
                }

                // ---- WL-3 pair chord: PER-BAND slip semantics.  Under the LANE shape a
                // slip on either band refloats BOTH bands (§3.4 — they share the graph
                // edge and the joint 2×2 coupling makes a half-stale prior poisonous);
                // under the §6.1 EQUIVALENCE shape each band refloats on its OWN slips,
                // exactly like the two per-band groups it must reproduce to ≤1e-9.
                bool slip2_in_cycle = false;
                if (!slipped2_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped2_arcs.count(arc)) { slip2_in_cycle = true; break; }
                const bool union_slip = slip_in_cycle || slip2_in_cycle;
                const bool c1 = would_carry && !(cfg_.lane_joint ? union_slip : slip_in_cycle);
                const bool c2 = would_carry && !(cfg_.lane_joint ? union_slip : slip2_in_cycle);
                Eigen::VectorXd w;
                if (would_carry) {
                    w = Eigen::VectorXd::Zero(n_old);
                    for (Eigen::Index j = 0; j < n_old; ++j) {
                        auto it = cyc.find(old_chords[static_cast<std::size_t>(j)]);
                        if (it != cyc.end()) w(j) = static_cast<double>(it->second);
                    }
                }
                counters_.chords_carried += (c1 ? 1 : 0) + (c2 ? 1 : 0);
                counters_.chords_born    += (c1 ? 0 : 1) + (c2 ? 0 : 1);
                if (would_carry && !c1) ++counters_.slip_kills;
                if (would_carry && !c2) ++counters_.slip_kills;
                // WL-3 §3.4: a reborn pair chord must RE-EARN its FFR fix — erase the
                // fold-on-change memory here (idempotent, so the RAIM re-solve rebuild
                // stays deterministic: carried_ is unchanged between within-epoch builds).
                if (!(c1 && c2)) lane_fix_w_.erase({g.key, c});
                fold_chords.push_back(c);
                fold_carried.push_back(c1 ? 1 : 0);
                fold_carried2.push_back(c2 ? 1 : 0);
                rows.push_back(std::move(terms));
                rows2.push_back(std::move(terms2));
                Wrows.push_back(std::move(w));   // sized ⇔ the GRAPH projection exists (per-band masks above)
            }
            if (fold_chords.empty()) continue;   // nothing foldable → carry nothing here

            // --- Assemble the DD-prior block(s) (carried joint + fresh diffuse). --
            const Eigen::Index K = static_cast<Eigen::Index>(fold_chords.size());
            if (static_cast<long long>(tally * K) > counters_.max_component_kc)
                counters_.max_component_kc = static_cast<long long>(tally * K);   // INC-C0 census (pair joint fold cost is 2K)
            Eigen::MatrixXd W;
            {   // INC-C0: time the dense-K² change-of-basis projection (W · Pᶻ_old · Wᵀ, W·z_old)
                // — the materialization the factored (A+U S Uᵀ) store of INC-C2 removes.
                // (The projection W is GRAPH-level: for a WL-3 pair component it applies to
                // each band's half unchanged — blkdiag(W, W) on the joint [b1; b2] storage.)
                ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                W = Eigen::MatrixXd::Zero(K, n_old);
                for (Eigen::Index i = 0; i < K; ++i)
                    if (Wrows[static_cast<std::size_t>(i)].size() == n_old && n_old > 0)
                        W.row(i) = Wrows[static_cast<std::size_t>(i)].transpose();

                // Tree swap: the carry was re-expressed in a genuinely different cycle basis
                // iff some carried chord's projection row mixes ≥2 old chords (W is not a
                // signed permutation) — the projection analogue of a phase-B unimodular pivot.
                for (const Eigen::VectorXd& wr : Wrows) {
                    if (wr.size() != n_old || n_old == 0) continue;   // fresh chord
                    if ((wr.array() != 0.0).count() >= 2) { counters_.tree_swaps += tally; break; }
                }

                // INC-C0 census: an old chord no surviving fundamental cycle references (its W
                // column is all-zero) is marginalized PER-CHORD — the drop the whole-block
                // chords_marginalized (prev_hit==0) misses.  Answers §0 (a)-vs-(b): large ⇒ the
                // projection kill path is firing (arcs churn); ~0 while max_z_dim grows ⇒ dead
                // chords are retained in giant live components.
                for (Eigen::Index j = 0; j < n_old; ++j)
                    if (W.col(j).isZero(0.0)) counters_.chords_marg_perchord += tally;
            }

            if (!g.pair) {
                // ---- Per-band group: today's single-block emission (byte-identical). ----
                NetworkNormalEquations::DdPriorBlock blk;
                {
                    ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                    blk.rows = std::move(rows);
                    blk.zhat = (n_old > 0) ? (W * z_old).eval() : Eigen::VectorXd::Zero(K);
                    // Carried–carried joint = W Pᶻ_old Wᵀ (fresh rows/cols are zero from W=0);
                    // fresh chords get an independent diffuse prior on the diagonal.
                    blk.Pz = (n_old > 0) ? (W * Pz_old * W.transpose()).eval()
                                         : Eigen::MatrixXd::Zero(K, K);
                    for (Eigen::Index i = 0; i < K; ++i)
                        if (Wrows[static_cast<std::size_t>(i)].size() != n_old || n_old == 0)
                            blk.Pz(i, i) += cfg_.fresh_prior_var;
                    blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
                }

                Eigen::VectorXd blk_zhat = blk.zhat;   // D5: the prior means, for the install-time innovation gate
                out.push_back(std::move(blk));
                block_meta_.push_back(BlockMeta{g.key, std::move(fold_chords), verts,
                                                std::set<ArcKey>(arcs.begin(), arcs.end()),
                                                std::move(blk_zhat), std::move(fold_carried)});
                ++counters_.components;
            } else {
                // ---- WL-3 pair component (SPEC_lane_carry_card.md §2): the projection
                // applies PER BAND on the joint [band-1; band-2] storage; fresh chords get
                // the lane-basis prior (§3.1) under the lane shape, the isotropic diffuse
                // prior otherwise.  Emission: ONE joint 2K-row block (lane shape) or TWO
                // per-band K-row blocks (the §6.1 equivalence shape, information-identical
                // to today's two per-band groups).
                ++counters_.pair_components;
                Eigen::VectorXd zhat1, zhat2;
                Eigen::MatrixXd P11, P22, P12;
                {
                    ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                    if (n_old > 0) {
                        // The projection applies PER BAND with that band's carried mask —
                        // a band whose chord refloated projects NOTHING (fresh row/col).
                        Eigen::MatrixXd W1 = W, W2 = W;
                        for (Eigen::Index i = 0; i < K; ++i) {
                            if (!fold_carried[static_cast<std::size_t>(i)])  W1.row(i).setZero();
                            if (!fold_carried2[static_cast<std::size_t>(i)]) W2.row(i).setZero();
                        }
                        zhat1 = W1 * z_old.head(n_old);
                        zhat2 = W2 * z_old.tail(n_old);
                        P11 = W1 * Pz_old.topLeftCorner(n_old, n_old) * W1.transpose();
                        P22 = W2 * Pz_old.bottomRightCorner(n_old, n_old) * W2.transpose();
                        P12 = W1 * Pz_old.topRightCorner(n_old, n_old) * W2.transpose();
                    } else {
                        zhat1 = Eigen::VectorXd::Zero(K);
                        zhat2 = Eigen::VectorXd::Zero(K);
                        P11 = Eigen::MatrixXd::Zero(K, K);
                        P22 = Eigen::MatrixXd::Zero(K, K);
                        P12 = Eigen::MatrixXd::Zero(K, K);
                    }
                    // Fresh rows: the lane-basis prior Rᵀ·diag(σ_nl², σ_wl²)·R (σ_nl = the
                    // diffuse √fresh_prior_var, σ_wl = the honest code-seed lane scale) — or
                    // the isotropic diffuse when the lane prior is not armed (§6.1 shape).
                    // The 2×2 lane prior applies only when BOTH bands are fresh (the lane
                    // shape's union refloat guarantees it); a per-band fresh row under the
                    // equivalence shape gets the plain diffuse diagonal.
                    const bool lane_prior = cfg_.lane_joint && cfg_.sigma_wl_fresh_cyc > 0.0
                                            && g.lambda1 > 0.0 && g.lambda2 > 0.0;
                    Eigen::Matrix2d Pf = Eigen::Matrix2d::Zero();
                    if (lane_prior) {
                        const double sig_wl_m =
                            cfg_.sigma_wl_fresh_cyc * laneMetersPerCycle(g.lambda1, g.lambda2);
                        const Eigen::Matrix2d R = laneRotation(g.lambda1, g.lambda2);
                        Pf = R.transpose()
                           * Eigen::Vector2d(cfg_.fresh_prior_var, sig_wl_m * sig_wl_m).asDiagonal()
                           * R;
                    }
                    for (Eigen::Index i = 0; i < K; ++i) {
                        const bool f1 = !fold_carried[static_cast<std::size_t>(i)];
                        const bool f2 = !fold_carried2[static_cast<std::size_t>(i)];
                        if (!f1 && !f2) continue;   // carried in both bands
                        if (lane_prior && f1 && f2) {
                            P11(i, i) += Pf(0, 0);
                            P22(i, i) += Pf(1, 1);
                            P12(i, i) += Pf(0, 1);
                        } else {
                            if (f1) P11(i, i) += cfg_.fresh_prior_var;
                            if (f2) P22(i, i) += cfg_.fresh_prior_var;
                        }
                    }
                }
                const std::set<ArcKey> arcs_set(arcs.begin(), arcs.end());
                if (cfg_.lane_joint) {
                    // ONE joint 2K-row block: rows = K band-1 cycles then K band-2 cycles,
                    // Pᶻ the joint 2K×2K — its fold's cross-band Woodbury term B₁₂ is EXACT
                    // (the per-band shape drops it), which is part of the lane design.
                    NetworkNormalEquations::DdPriorBlock blk;
                    blk.rows = std::move(rows);
                    blk.rows.insert(blk.rows.end(), std::make_move_iterator(rows2.begin()),
                                    std::make_move_iterator(rows2.end()));
                    blk.zhat.resize(2 * K);
                    blk.zhat << zhat1, zhat2;
                    blk.Pz.resize(2 * K, 2 * K);
                    blk.Pz.topLeftCorner(K, K)     = P11;
                    blk.Pz.bottomRightCorner(K, K) = P22;
                    blk.Pz.topRightCorner(K, K)    = P12;
                    blk.Pz.bottomLeftCorner(K, K)  = P12.transpose();
                    blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
                    Eigen::VectorXd blk_zhat = blk.zhat;
                    out.push_back(std::move(blk));
                    BlockMeta bm{g.key, std::move(fold_chords), verts, arcs_set,
                                 std::move(blk_zhat), std::move(fold_carried)};
                    bm.pair_band = 3;
                    bm.lambda1 = g.lambda1;
                    bm.lambda2 = g.lambda2;
                    block_meta_.push_back(std::move(bm));
                    ++counters_.components;
                } else {
                    // TWO per-band K-row blocks (band-2 adjacent at b+1) — the fold sees
                    // exactly today's information (block-diagonal, no B₁₂), so the §6.1
                    // equivalence gate can hold to summation order.
                    NetworkNormalEquations::DdPriorBlock b1, b2;
                    b1.rows = std::move(rows);
                    b1.zhat = zhat1;
                    b1.Pz   = (0.5 * (P11 + P11.transpose())).eval();
                    b2.rows = std::move(rows2);
                    b2.zhat = zhat2;
                    b2.Pz   = (0.5 * (P22 + P22.transpose())).eval();
                    out.push_back(std::move(b1));
                    out.push_back(std::move(b2));
                    BlockMeta m1{g.key, fold_chords, verts, arcs_set,
                                 std::move(zhat1), std::move(fold_carried)};
                    m1.pair_band = 1;
                    m1.lambda1 = g.lambda1;
                    m1.lambda2 = g.lambda2;
                    BlockMeta m2{g.key, std::move(fold_chords), verts, arcs_set,
                                 std::move(zhat2), std::move(fold_carried2)};
                    m2.pair_band = 2;
                    m2.lambda1 = g.lambda1;
                    m2.lambda2 = g.lambda2;
                    block_meta_.push_back(std::move(m1));
                    block_meta_.push_back(std::move(m2));
                    counters_.components += 2;
                }
            }
        }
    }

    // Splits: a previous block spread across ≥2 new components.  Marginalized: a
    // previous block that fed nothing this epoch (its arcs all died / re-referenced).
    // (WL-3: a pair block tallies 2× — it holds two per-band DDs per chord.)
    for (std::size_t p = 0; p < prev.size(); ++p) {
        const long long ptally = prev[p].pair ? 2 : 1;
        if (prev_hit[p] >= 2) counters_.splits += ptally;
        if (prev_hit[p] == 0)
            counters_.chords_marginalized += ptally * static_cast<long long>(prev[p].chords.size());
    }
    return out;
}

void NetworkInvariantCarryDriver::seedCarriedForTest(
    int group, const std::vector<ArcKey>& chords, const Eigen::VectorXd& z,
    const Eigen::MatrixXd& Pz, const std::set<int>& vertices,
    const std::set<ArcKey>& arcs) {
    CarriedBlock cb;
    cb.group = group;
    cb.chords = chords;
    cb.z = z;
    cb.Pz = Pz;
    cb.vertices = vertices;
    cb.arcs = arcs;
    carried_.push_back(std::move(cb));
}

bool NetworkInvariantCarryDriver::carriedDdForTest(
    int group, const ArcKey& chord, double& z, double& var) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index i = it - cb.chords.begin();
        z = cb.z(i);
        var = cb.Pz(i, i);
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::injectCarriedChord(
    int group, const ArcKey& chord, double new_z, double new_var) {
    for (CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index i = it - cb.chords.begin();
        const double keep_var = cb.Pz(i, i);       // fallback if new_var is non-positive
        cb.z(i) = new_z;                            // the lane-targeted correction-frame DD
        cb.Pz.row(i).setZero();                     // fresh independent assertion (PSD-safe)
        cb.Pz.col(i).setZero();
        cb.Pz(i, i) = (new_var > 0.0) ? new_var : keep_var;
        gate_exempt_.insert({ group, chord });      // §3.3: D5-exempt at the NEXT install
        return true;
    }
    return false;   // not a carried chord of `group` (cycle not 1:1) — caller skips + counts
}

void NetworkInvariantCarryDriver::seedCarriedPairForTest(
    int group, const std::vector<ArcKey>& chords, const Eigen::VectorXd& z,
    const Eigen::MatrixXd& Pz, double lambda1, double lambda2,
    const std::set<int>& vertices, const std::set<ArcKey>& arcs) {
    CarriedBlock cb;
    cb.group = group;
    cb.chords = chords;
    cb.z = z;
    cb.Pz = Pz;
    cb.vertices = vertices;
    cb.arcs = arcs;
    cb.pair = true;
    cb.lambda1 = lambda1;
    cb.lambda2 = lambda2;
    carried_.push_back(std::move(cb));
}

bool NetworkInvariantCarryDriver::carriedPairBlockForTest(
    int group, std::vector<ArcKey>& chords, Eigen::VectorXd& z, Eigen::MatrixXd& Pz) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        chords = cb.chords;
        z = cb.z;
        Pz = cb.Pz;
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::carriedPairValue(
    int group, const ArcKey& chord, double& z1, double& var1,
    double& z2, double& var2) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        const Eigen::Index i = it - cb.chords.begin();
        if (cb.z.size() != 2 * K) return false;   // defensive
        z1   = cb.z(i);
        var1 = cb.Pz(i, i);
        z2   = cb.z(K + i);
        var2 = cb.Pz(K + i, K + i);
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::injectCarriedChordPair(
    int group, const ArcKey& chord, double new_z1, double new_var1,
    double new_z2, double new_var2) {
    for (CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        const Eigen::Index i = it - cb.chords.begin();
        if (cb.z.size() != 2 * K) return false;   // defensive
        const double keep1 = cb.Pz(i, i), keep2 = cb.Pz(K + i, K + i);
        cb.z(i)     = new_z1;
        cb.z(K + i) = new_z2;
        cb.Pz.row(i).setZero();
        cb.Pz.col(i).setZero();
        cb.Pz.row(K + i).setZero();
        cb.Pz.col(K + i).setZero();
        cb.Pz(i, i)         = (new_var1 > 0.0) ? new_var1 : keep1;
        cb.Pz(K + i, K + i) = (new_var2 > 0.0) ? new_var2 : keep2;
        gate_exempt_.insert({ group, chord });   // §3.3: D5-exempt (both band rows) at the NEXT install
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::applyLaneMeasurement(
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    std::size_t b, std::size_t k, double y_cyc, double r_cyc2) {
    if (b >= block_meta_.size() || b >= blocks.size()) return false;
    BlockMeta& bm = block_meta_[b];
    if (bm.pair_band != 3) return false;   // lane rows exist only on the joint lane shape
    const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
    if (static_cast<Eigen::Index>(k) >= K) return false;
    if (!(r_cyc2 > 0.0) || !std::isfinite(y_cyc)) return false;
    if (!(bm.lambda1 > 0.0 && bm.lambda2 > 0.0)) return false;
    NetworkNormalEquations::DdPriorBlock& blk = blocks[b];
    if (blk.zhat.size() != 2 * K || blk.Pz.rows() != 2 * K || blk.Pz.cols() != 2 * K)
        return false;
    // Scalar KF update on the block prior with the sparse WL functional
    // h = +1/λ₁ at row k, −1/λ₂ at row K+k (correction frame, cycles).
    const double a1 = 1.0 / bm.lambda1, a2 = 1.0 / bm.lambda2;
    const Eigen::Index i1 = static_cast<Eigen::Index>(k), i2 = K + i1;
    const Eigen::VectorXd Ph = a1 * blk.Pz.col(i1) - a2 * blk.Pz.col(i2);
    const double S = a1 * Ph(i1) - a2 * Ph(i2) + r_cyc2;
    if (!(S > 0.0) || !std::isfinite(S)) return false;
    const double innov = y_cyc - (a1 * blk.zhat(i1) - a2 * blk.zhat(i2));
    if (!std::isfinite(innov)) return false;
    blk.zhat += (innov / S) * Ph;
    blk.Pz   -= (Ph * Ph.transpose()) / S;
    blk.Pz    = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
    bm.zhat   = blk.zhat;   // the D5 innovation gate must standardize against the FOLDED prior
    return true;
}

int NetworkInvariantCarryDriver::applyLaneFix(
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    std::size_t b, std::size_t k, long long w, double y_cyc, double sigma_cyc) {
    if (b >= block_meta_.size() || b >= blocks.size()) return -1;
    BlockMeta& bm = block_meta_[b];
    if (bm.pair_band != 3 || k >= bm.chords.size()) return -1;
    if (!(sigma_cyc > 0.0) || !std::isfinite(y_cyc)) return -1;
    if (!(bm.lambda1 > 0.0 && bm.lambda2 > 0.0)) return -1;
    const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
    NetworkNormalEquations::DdPriorBlock& blk = blocks[b];
    if (blk.zhat.size() != 2 * K || blk.Pz.rows() != 2 * K) return -1;

    // The fix is an IDEMPOTENT lane-coordinate SET, not a measurement fold: every
    // decided epoch it re-centers the chord's WL lane COORDINATE on the integer and
    // re-collapses its variance to σ², zeroing the coordinate's cross-covariances
    // (a fresh independent assertion, PSD-safe — WL-2's inject semantics realized on
    // the lane coordinate, NL preserved by construction).  A measurement fold here
    // would ACCUMULATE per-epoch decisions that are not independent information (the
    // F-A once-only discipline); the set is information-idempotent, and re-applying
    // it while the decision is unchanged is what keeps the lane pinned against the
    // q_wl re-loosening + the noisy MW increments (measured Jul-16: fold-on-change
    // alone leaves decided lanes drifting to MW quality, med|f| 0.25-vs-0.14).
    const double a1 = 1.0 / bm.lambda1, a2 = 1.0 / bm.lambda2;
    const double hn = std::sqrt(a1 * a1 + a2 * a2);   // ‖h‖: cycles per lane-coordinate meter
    Eigen::VectorXd u = Eigen::VectorXd::Zero(2 * K); // unit lane-coordinate direction
    u(static_cast<Eigen::Index>(k))     =  a1 / hn;
    u(K + static_cast<Eigen::Index>(k)) = -a2 / hn;
    const double target_coord = y_cyc / hn;           // correction-frame lane target (m)
    const double sig_coord    = sigma_cyc / hn;
    const Eigen::VectorXd Pu = blk.Pz * u;
    const double uPu = u.dot(Pu);
    if (!std::isfinite(uPu) || uPu < 0.0) return -1;
    // z' = z + (t − uᵀz)·u;  P' = (I−uuᵀ)P(I−uuᵀ) + σ²·uuᵀ
    blk.zhat += (target_coord - u.dot(blk.zhat)) * u;
    blk.Pz   += -Pu * u.transpose() - u * Pu.transpose()
              + (uPu + sig_coord * sig_coord) * (u * u.transpose());
    blk.Pz    = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
    bm.zhat   = blk.zhat;   // the D5 gate standardizes against the SET prior

    const std::pair<int, ArcKey> key{ bm.group, bm.chords[k] };
    const auto it = lane_fix_w_.find(key);
    const bool changed = (it == lane_fix_w_.end() || it->second != w);
    lane_fix_staged_[key] = w;   // committed at installPosterior (RAIM re-solve safe)
    return changed ? 1 : 0;      // 1 = integer moved/new, 0 = re-pinned (unchanged)
}

bool NetworkInvariantCarryDriver::carriedCrossForTest(
    int group, const ArcKey& a, const ArcKey& b, double& cov) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto ia = std::find(cb.chords.begin(), cb.chords.end(), a);
        auto ib = std::find(cb.chords.begin(), cb.chords.end(), b);
        if (ia == cb.chords.end() || ib == cb.chords.end()) continue;
        cov = cb.Pz(ia - cb.chords.begin(), ib - cb.chords.begin());
        return true;
    }
    return false;
}

double NetworkInvariantCarryDriver::carriedEstimableForTest(
    int group, const std::map<ArcKey, int>& target_cycle) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        // The component must contain the target cycle's arcs (share a vertex).
        bool here = false;
        for (const auto& [arc, coeff] : target_cycle)
            if (cb.vertices.count(arc.u) || cb.vertices.count(arc.v)) { here = true; break; }
        if (!here) continue;
        double val = 0.0;
        for (Eigen::Index j = 0; j < static_cast<Eigen::Index>(cb.chords.size()); ++j) {
            auto it = target_cycle.find(cb.chords[static_cast<std::size_t>(j)]);
            if (it != target_cycle.end()) val += static_cast<double>(it->second) * cb.z(j);
        }
        return val;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void NetworkInvariantCarryDriver::installPosterior(
    const NetworkEstimator& est, const Eigen::VectorXd& x_s,
    const NetworkEstimator::Datum& datum, int n_threads) {
    last_innov_flags_.clear();
    last_innov_sigmas_.clear();
    // Hoist the datum-frame shared covariance (the n_shared² Σ_ss copy) to ONCE per install —
    // every block's posterior-cov AND innovation readout consumes the same matrix.
    const Eigen::MatrixXd cov_free = est.datumFrameSharedCov(datum);
    // D5 v2: the empirical inflation σ̂₀² accumulated THROUGH the previous epoch (predict-
    // then-update — this epoch's standardized innovations are fed AFTER gating, below).  It
    // is 1.0 (= the raw formal test, v1) when calibration is off or still in warm-up, so the
    // gate becomes a TRUE Nσ test: |v_i| > k·σ̂₀·√Cov(v)_ii.
    const double sigma0 = innovInflationSigma0();
    const double k2f    = cfg_.innov_k * cfg_.innov_k * sigma0 * sigma0;   // (k·σ̂₀)²

    // ---- Deterministic-parallel readout (network.parallel_seam) -----------------
    // The per-block posterior mean/cov + innovation are CONST readouts of the folded
    // kernel (each reads only dd_priors_[b] + the shared read-only x_s / cov_free), so
    // they parallelize cleanly.  Everything ORDER-SENSITIVE — the σ̂₀ accumulator feed
    // (NetworkCarryVce::addRow), the gate decision, the flag/counter appends and the
    // carried_ install — stays SERIAL in the exact (block b, chord i) order below, so
    // the σ̂₀ that gates the NEXT epoch, the counters, and carried_ are all BIT-IDENTICAL
    // to the serial install regardless of thread count.  The serial path (n_threads == 1,
    // the default) is left completely UNTOUCHED — keep the two per-chord bodies in lockstep.
    // WL-3: a D5-flagged pair chord loses its fold-on-change memory (it re-taps and must
    // re-earn its FFR fix, §3.4) — shared by all install shapes below.
    auto laneFixErase = [this](int group, const ArcKey& chord) {
        if (!lane_fix_w_.empty())      lane_fix_w_.erase({group, chord});
        if (!lane_fix_staged_.empty()) lane_fix_staged_.erase({group, chord});
    };
    // WL-3 D5 gate over ONE emitted per-band half of a pair component (split shape) or,
    // via offset arithmetic, kept in lockstep with the per-band body below.  Edits the
    // stitched pair block @p cb at rows [off, off+K).
    auto d5GateHalf = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                          const Eigen::MatrixXd& cov_v, CarriedBlock& cb, Eigen::Index off) {
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
        const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
        if (!(v.size() == K && cov_v.rows() == K && cov_v.cols() == K && bm.zhat.size() == K))
            return;
        for (Eigen::Index i = 0; i < K; ++i) {
            const double var = cov_v(i, i);
            if (!(var > 0.0)) continue;
            if (gate_exempt_.count({ bm.group, bm.chords[static_cast<std::size_t>(i)] }))
                continue;
            if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(i)]))
                inflation_->addRow(kInflationFamily, kInflationSat,
                                   v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                   /*post_var=*/0.0, /*fold_var=*/1.0);
            if (v(i) * v(i) <= k2f * var) continue;
            cb.z(off + i) = bm.zhat(i) + v(i);
            cb.Pz.row(off + i).setZero();
            cb.Pz.col(off + i).setZero();
            cb.Pz(off + i, off + i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(i)]);
        }
    };
    // WL-3 D5 gate over a JOINT 2K pair block: per band-ROW exactly as today (row i<K =
    // band-1 of chord i, row K+i = band-2 of chord i); the exemption + σ̂₀-carried flag
    // key by the GRAPH chord (band-uniform membership).
    auto d5GateJoint = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                           const Eigen::MatrixXd& cov_v, CarriedBlock& cb) {
        const Eigen::Index K  = static_cast<Eigen::Index>(bm.chords.size());
        const Eigen::Index K2 = 2 * K;
        const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
        if (!(v.size() == K2 && cov_v.rows() == K2 && cov_v.cols() == K2 && bm.zhat.size() == K2))
            return;
        for (Eigen::Index i = 0; i < K2; ++i) {
            const Eigen::Index c = (i < K) ? i : i - K;
            const double var = cov_v(i, i);
            if (!(var > 0.0)) continue;
            if (gate_exempt_.count({ bm.group, bm.chords[static_cast<std::size_t>(c)] }))
                continue;   // a pair inject's exemption spans both band rows
            if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(c)]))
                inflation_->addRow(kInflationFamily, kInflationSat,
                                   v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                   /*post_var=*/0.0, /*fold_var=*/1.0);
            if (v(i) * v(i) <= k2f * var) continue;
            cb.z(i) = bm.zhat(i) + v(i);
            cb.Pz.row(i).setZero();
            cb.Pz.col(i).setZero();
            cb.Pz(i, i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(c)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(c)]);
        }
    };
    // WL-3: commit this epoch's staged fold-on-change decisions (a D5 flag above already
    // erased its chord from BOTH maps, so flag wins over fix) and purge memory for chords
    // no longer carried as pair chords.  No-op (no allocation, no fp) on the per-band path.
    auto laneFixCommit = [this]() {
        if (lane_fix_staged_.empty() && lane_fix_w_.empty()) return;
        for (const auto& [key, w] : lane_fix_staged_) lane_fix_w_[key] = w;
        lane_fix_staged_.clear();
        std::set<std::pair<int, ArcKey>> live;
        for (const auto& bm : block_meta_)
            if (bm.pair_band == 1 || bm.pair_band == 3)
                for (const auto& c : bm.chords) live.insert({bm.group, c});
        for (auto it = lane_fix_w_.begin(); it != lane_fix_w_.end(); ) {
            if (live.count(it->first)) ++it;
            else it = lane_fix_w_.erase(it);
        }
    };

    if (n_threads > 1) {
        const std::size_t NB = block_meta_.size();
        struct InstallSlot {
            CarriedBlock    cb;
            Eigen::VectorXd v;                 // innovation (only when innov_gate && K > 0)
            Eigen::MatrixXd cov_v;             // its H0 covariance
            bool            has_innov{false};
        };
        std::vector<InstallSlot> slots(NB);

        // (1, PARALLEL over block index) const readouts into per-block slots only.
        // Eigen is pinned to one thread by the driver ⇒ no nested threading.  A pair
        // component's split halves / joint block read exactly like any other block —
        // the stitching stays in the serial phase.
        #pragma omp parallel for schedule(dynamic) num_threads(n_threads)
        for (long long bi = 0; bi < static_cast<long long>(NB); ++bi) {
            const std::size_t b = static_cast<std::size_t>(bi);
            const BlockMeta& bm = block_meta_[b];
            InstallSlot& S = slots[b];
            S.cb.group    = bm.group;
            S.cb.chords   = bm.chords;
            S.cb.vertices = bm.vertices;
            S.cb.arcs     = bm.arcs;
            S.cb.z  = est.ddPriorPosteriorMean(b, x_s);
            S.cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free);
            if (cfg_.innov_gate && static_cast<Eigen::Index>(bm.chords.size()) > 0) {
                est.ddPriorInnovation(b, x_s, cov_free, S.v, S.cov_v);
                S.has_innov = true;
            }
        }

        // (2, SERIAL in (block b, chord i) order) the σ̂₀ feed + gate + flags + install —
        // identical call sequence ⇒ identical σ̂₀ / counters / carried_.
        std::vector<CarriedBlock> next;
        next.reserve(NB);
        for (std::size_t b = 0; b < NB; ++b) {
            const BlockMeta& bm = block_meta_[b];
            if (bm.pair_band == 2) continue;   // WL-3: stitched with its band-1 sibling at b-1
            InstallSlot& S = slots[b];
            const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
            if (bm.pair_band == 0) {
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateHalf(bm, S.v, S.cov_v, S.cb, 0);
                next.push_back(std::move(S.cb));
                continue;
            }
            // WL-3 pair component: stitch the joint carried block.
            CarriedBlock cb;
            cb.group    = bm.group;
            cb.chords   = bm.chords;
            cb.vertices = bm.vertices;
            cb.arcs     = bm.arcs;
            cb.pair     = true;
            cb.lambda1  = bm.lambda1;
            cb.lambda2  = bm.lambda2;
            if (bm.pair_band == 1 && b + 1 < NB && block_meta_[b + 1].pair_band == 2) {
                InstallSlot& S2 = slots[b + 1];
                cb.z.resize(2 * K);
                cb.z << S.cb.z, S2.cb.z;
                cb.Pz = Eigen::MatrixXd::Zero(2 * K, 2 * K);   // §6.1: cross-band coupling ZEROED
                cb.Pz.topLeftCorner(K, K)     = S.cb.Pz;
                cb.Pz.bottomRightCorner(K, K) = S2.cb.Pz;
                if (cfg_.innov_gate && K > 0) {
                    if (S.has_innov)  d5GateHalf(bm, S.v, S.cov_v, cb, 0);
                    if (S2.has_innov) d5GateHalf(block_meta_[b + 1], S2.v, S2.cov_v, cb, K);
                }
            } else {   // pair_band == 3: the joint lane-shape block (coupling retained)
                cb.z  = std::move(S.cb.z);
                cb.Pz = std::move(S.cb.Pz);
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateJoint(bm, S.v, S.cov_v, cb);
            }
            next.push_back(std::move(cb));
        }
        carried_.swap(next);
        gate_exempt_.clear();   // WL-2 §3.3: the one-epoch inject exemption is consumed here
        laneFixCommit();        // WL-3 §3.3: fold-on-change memory commits on the accepted epoch
        counters_.z_dim = static_cast<long long>(totalZDim());
        if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
        return;
    }

    // ---- Serial install (n_threads == 1, the default; byte-identical OFF path) ---
    std::vector<CarriedBlock> next;
    next.reserve(block_meta_.size());
    if (perf_timing_) ++perf_timers_.install_epochs;   // INC-C0
    for (std::size_t b = 0; b < block_meta_.size(); ++b) {
        const BlockMeta& bm = block_meta_[b];
        if (bm.pair_band == 2) continue;   // WL-3: stitched with its band-1 sibling at b-1
        CarriedBlock cb;
        cb.group    = bm.group;
        cb.chords   = bm.chords;
        cb.vertices = bm.vertices;
        cb.arcs     = bm.arcs;
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());

        if (bm.pair_band == 1 && b + 1 < block_meta_.size()
            && block_meta_[b + 1].pair_band == 2) {
            // --- WL-3 §6.1 split pair: stitch two per-band posteriors into the joint
            // [band-1; band-2] carried block with the cross-band coupling ZEROED — the
            // information-identical-to-today equivalence shape.  D5 gates per emitted
            // half in (b, b+1) row order, exactly the per-band call sequence.
            const BlockMeta& bm2 = block_meta_[b + 1];
            cb.pair    = true;
            cb.lambda1 = bm.lambda1;
            cb.lambda2 = bm.lambda2;
            Eigen::VectorXd z1, z2;
            Eigen::MatrixXd P1, P2;
            { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);
              z1 = est.ddPriorPosteriorMean(b, x_s);
              z2 = est.ddPriorPosteriorMean(b + 1, x_s); }
            { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);
              P1 = est.ddPriorPosteriorCovariance(b, cov_free);
              P2 = est.ddPriorPosteriorCovariance(b + 1, cov_free); }
            cb.z.resize(2 * K);
            cb.z << z1, z2;
            cb.Pz = Eigen::MatrixXd::Zero(2 * K, 2 * K);
            cb.Pz.topLeftCorner(K, K)     = P1;
            cb.Pz.bottomRightCorner(K, K) = P2;
            if (cfg_.innov_gate && K > 0) {
                ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);
                Eigen::VectorXd v;
                Eigen::MatrixXd cov_v;
                est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
                d5GateHalf(bm, v, cov_v, cb, 0);
                est.ddPriorInnovation(b + 1, x_s, cov_free, v, cov_v);
                d5GateHalf(bm2, v, cov_v, cb, K);
            }
            next.push_back(std::move(cb));
            continue;
        }
        if (bm.pair_band == 3) {
            // --- WL-3 lane shape: the joint 2K posterior — cross-band coupling RETAINED
            // (the honest jointly-covarianced pair, §2); D5 per band-row as today.
            cb.pair    = true;
            cb.lambda1 = bm.lambda1;
            cb.lambda2 = bm.lambda2;
            { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);
              cb.z  = est.ddPriorPosteriorMean(b, x_s); }
            { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);
              cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free); }
            if (cfg_.innov_gate && K > 0) {
                ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);
                Eigen::VectorXd v;
                Eigen::MatrixXd cov_v;
                est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
                d5GateJoint(bm, v, cov_v, cb);
            }
            next.push_back(std::move(cb));
            continue;
        }

        { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);   // INC-C0
          cb.z  = est.ddPriorPosteriorMean(b, x_s); }
        { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);    // INC-C0 (the dense K×K install)
          cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free); }

        // --- Card 4 D5: estimation-side fine-slip innovation gate ------------------
        // The carried DD's coarse slip layer (MW/LLI/IF-TD) is deliberately relaxed; a
        // finer slip the coarse layer missed inflates the carried DD's post-solve
        // innovation v = z_free − Gᵀx_s − ẑ.  A chord whose standardized innovation exceeds
        // k·σ̂₀ is treated as slipped: install it FRESH here (its data-only tap-off, mean
        // d = v+ẑ and variance σ̂₀²·Cov(v)_ii — the CALIBRATED innovation variance, i.e. the
        // fresh chord's honest uncertainty, NOT the optimistic formal one; cross-terms
        // zeroed), so the stale value does not propagate, and record its (group, chord) so
        // the caller marks its arc into NEXT epoch's slip funnel (the C3c re-tap).  OFF =
        // the posterior is installed unconditionally (byte-identical to the C3c path).
        // (The per-chord body lives in d5GateHalf, offset 0 — kept in lockstep with the
        // parallel path and the WL-3 pair shapes above.)
        if (cfg_.innov_gate && K > 0) {
            ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);   // INC-C0
            Eigen::VectorXd v;
            Eigen::MatrixXd cov_v;
            est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
            d5GateHalf(bm, v, cov_v, cb, 0);
        }
        next.push_back(std::move(cb));
    }
    carried_.swap(next);
    gate_exempt_.clear();   // WL-2 §3.3: the one-epoch inject exemption is consumed here
    laneFixCommit();        // WL-3 §3.3: fold-on-change memory commits on the accepted epoch
    counters_.z_dim = static_cast<long long>(totalZDim());
    if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
}

}  // namespace GnssProcessor
