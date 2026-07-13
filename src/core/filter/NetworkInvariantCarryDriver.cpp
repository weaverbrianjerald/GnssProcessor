/**
 * @file NetworkInvariantCarryDriver.cpp
 * @brief Card 4 (M1) phase C3b — implementation of the DD invariant-carry driver.
 *        See NetworkInvariantCarryDriver.hpp for the design rationale.
 */

#include "filter/NetworkInvariantCarryDriver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace GnssProcessor {

namespace {

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
    counters_ = Counters{};
}

void NetworkInvariantCarryDriver::beginEpoch(double dt_s, double content_s, bool clean) {
    // D5 v2: advance the σ̂₀² accumulator's exponential forgetting (once per epoch, BEFORE
    // this epoch's rows are folded in installPosterior) — independent of q_z.
    if (inflation_) inflation_->beginEpoch(content_s, clean);
    if (cfg_.qz <= 0.0 || dt_s <= 0.0) return;   // constant DDs (default) → no q_z op
    const double add = cfg_.qz * dt_s;
    for (auto& cb : carried_)
        if (cb.Pz.rows() > 0) cb.Pz.diagonal().array() += add;
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
    ++counters_.epochs;

    // The carry we project FROM this epoch; `carried_` is rebuilt in installPosterior.
    const std::vector<CarriedBlock>& prev = carried_;
    // Track, per previous block, how many NEW components it contributes to (split ⇒ >1).
    std::vector<int> prev_hit(prev.size(), 0);

    for (const GroupInput& g : groups) {
        // --- (1) Assemble this epoch's graph for the group. -------------------
        struct ArcInfo { int est_station; int local_col; bool tree_edge; };
        std::map<ArcKey, ArcInfo> arcinfo;
        std::set<ArcKey>          slipped_arcs;   // C3c: arcs whose carried ambiguity slipped this epoch
        UnionFind uf;
        std::vector<ArcKey> group_tree_edges;   // for per-component tree extraction
        for (const ArcInput& a : g.arcs) {
            if (a.rx_vertex == a.sat_vertex) continue;         // degenerate (defensive)
            const ArcKey k(a.rx_vertex, a.sat_vertex);
            arcinfo[k] = {a.est_station, a.local_col, a.tree_edge};
            uf.unite(a.rx_vertex, a.sat_vertex);
            if (a.tree_edge) group_tree_edges.push_back(k);
            if (a.slipped) slipped_arcs.insert(k);
        }
        if (arcinfo.empty()) continue;

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
                bool overlap = false;
                for (int v : ob.vertices) if (verts.count(v)) { overlap = true; break; }
                if (!overlap) continue;
                contributors.push_back(&ob);
                ++prev_hit[p];
                old_chords.insert(old_chords.end(), ob.chords.begin(), ob.chords.end());
                A_old.insert(ob.arcs.begin(), ob.arcs.end());
            }
            if (contributors.size() > 1) ++counters_.merges;
            const Eigen::Index n_old = static_cast<Eigen::Index>(old_chords.size());
            // Block-diagonal old covariance / mean over the contributors.
            Eigen::VectorXd z_old(n_old);
            Eigen::MatrixXd Pz_old = Eigen::MatrixXd::Zero(n_old, n_old);
            {
                Eigen::Index off = 0;
                for (const CarriedBlock* ob : contributors) {
                    const Eigen::Index k = ob->z.size();
                    if (k == 0) continue;
                    z_old.segment(off, k) = ob->z;
                    Pz_old.block(off, off, k, k) = ob->Pz;
                    off += k;
                }
            }
            // Map an old chord arc → its column index in the old basis.
            std::map<ArcKey, Eigen::Index> old_col;
            for (Eigen::Index j = 0; j < n_old; ++j) old_col[old_chords[static_cast<std::size_t>(j)]] = j;

            // --- Per new chord: fundamental cycle, foldability, carried vs fresh.
            std::vector<ArcKey>                              fold_chords;
            std::vector<char>                                fold_carried;  // D5 v2: 1 = carried prior, 0 = born
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows;
            std::vector<Eigen::VectorXd>                     Wrows;   // over old basis (empty ⇒ fresh)
            for (const ArcKey& c : chords) {
                const std::map<ArcKey, int> cyc = zc.fundamentalCycle(c);

                // Foldable ⇔ every cycle arc resolves to a live (station, local col).
                bool foldable = true;
                std::vector<NetworkNormalEquations::DdPriorBlock::Term> terms;
                terms.reserve(cyc.size());
                for (const auto& [arc, coeff] : cyc) {
                    const ArcInfo& ai = arcinfo.at(arc);   // every cycle arc is in this component's graph
                    if (ai.est_station < 0 || ai.local_col < 0) { foldable = false; break; }
                    terms.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col,
                                     static_cast<double>(coeff)});
                }
                if (!foldable) { ++counters_.chords_dropped; continue; }

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
            }
            if (fold_chords.empty()) continue;   // nothing foldable → carry nothing here

            // --- Assemble the DD-prior block (carried joint + fresh diffuse). --
            const Eigen::Index K = static_cast<Eigen::Index>(fold_chords.size());
            Eigen::MatrixXd W = Eigen::MatrixXd::Zero(K, n_old);
            for (Eigen::Index i = 0; i < K; ++i)
                if (Wrows[static_cast<std::size_t>(i)].size() == n_old && n_old > 0)
                    W.row(i) = Wrows[static_cast<std::size_t>(i)].transpose();

            // Tree swap: the carry was re-expressed in a genuinely different cycle basis
            // iff some carried chord's projection row mixes ≥2 old chords (W is not a
            // signed permutation) — the projection analogue of a phase-B unimodular pivot.
            for (const Eigen::VectorXd& wr : Wrows) {
                if (wr.size() != n_old || n_old == 0) continue;   // fresh chord
                if ((wr.array() != 0.0).count() >= 2) { ++counters_.tree_swaps; break; }
            }

            NetworkNormalEquations::DdPriorBlock blk;
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

            Eigen::VectorXd blk_zhat = blk.zhat;   // D5: the prior means, for the install-time innovation gate
            out.push_back(std::move(blk));
            block_meta_.push_back(BlockMeta{g.key, std::move(fold_chords), verts,
                                            std::set<ArcKey>(arcs.begin(), arcs.end()),
                                            std::move(blk_zhat), std::move(fold_carried)});
            ++counters_.components;
        }
    }

    // Splits: a previous block spread across ≥2 new components.  Marginalized: a
    // previous block that fed nothing this epoch (its arcs all died / re-referenced).
    for (std::size_t p = 0; p < prev.size(); ++p) {
        if (prev_hit[p] >= 2) ++counters_.splits;
        if (prev_hit[p] == 0)
            counters_.chords_marginalized += static_cast<long long>(prev[p].chords.size());
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
        // Eigen is pinned to one thread by the driver ⇒ no nested threading.
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
            InstallSlot& S = slots[b];
            const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
            if (cfg_.innov_gate && K > 0 && S.has_innov) {
                const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
                if (S.v.size() == K && S.cov_v.rows() == K && S.cov_v.cols() == K
                    && bm.zhat.size() == K) {
                    for (Eigen::Index i = 0; i < K; ++i) {
                        const double var = S.cov_v(i, i);
                        if (!(var > 0.0)) continue;
                        if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(i)]))
                            inflation_->addRow(kInflationFamily, kInflationSat,
                                               S.v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                               /*post_var=*/0.0, /*fold_var=*/1.0);
                        if (S.v(i) * S.v(i) <= k2f * var) continue;
                        S.cb.z(i) = bm.zhat(i) + S.v(i);
                        S.cb.Pz.row(i).setZero();
                        S.cb.Pz.col(i).setZero();
                        S.cb.Pz(i, i) = sigma0 * sigma0 * var;
                        last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
                        ++counters_.innov_flags;
                        ++counters_.innov_retaps;
                    }
                }
            }
            next.push_back(std::move(S.cb));
        }
        carried_.swap(next);
        counters_.z_dim = static_cast<long long>(totalZDim());
        return;
    }

    // ---- Serial install (n_threads == 1, the default; byte-identical OFF path) ---
    std::vector<CarriedBlock> next;
    next.reserve(block_meta_.size());
    for (std::size_t b = 0; b < block_meta_.size(); ++b) {
        const BlockMeta& bm = block_meta_[b];
        CarriedBlock cb;
        cb.group    = bm.group;
        cb.chords   = bm.chords;
        cb.vertices = bm.vertices;
        cb.arcs     = bm.arcs;
        cb.z  = est.ddPriorPosteriorMean(b, x_s);
        cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free);

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
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
        if (cfg_.innov_gate && K > 0) {
            Eigen::VectorXd v;
            Eigen::MatrixXd cov_v;
            est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
            const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
            if (v.size() == K && cov_v.rows() == K && cov_v.cols() == K
                && bm.zhat.size() == K) {
                for (Eigen::Index i = 0; i < K; ++i) {
                    const double var = cov_v(i, i);
                    if (!(var > 0.0)) continue;                 // inert/degenerate ⇒ un-testable, skip
                    // Feed the standardized innovation of a CARRIED chord into σ̂₀² (a born
                    // chord's diffuse Cov(v) would drag the empirical inflation to 0).  Fed
                    // AFTER this epoch's factor was read ⇒ it calibrates the NEXT epoch.
                    if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(i)]))
                        inflation_->addRow(kInflationFamily, kInflationSat,
                                           v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                           /*post_var=*/0.0, /*fold_var=*/1.0);
                    if (v(i) * v(i) <= k2f * var) continue;     // |v_i| ≤ k·σ̂₀·√Cov(v)_ii ⇒ no flag
                    // Flagged → reset fresh (data-only mean, calibrated variance, independent).
                    cb.z(i) = bm.zhat(i) + v(i);                // d_i = z_free_i − (Gᵀx_s)_i
                    cb.Pz.row(i).setZero();
                    cb.Pz.col(i).setZero();
                    cb.Pz(i, i) = sigma0 * sigma0 * var;        // σ̂₀²·Cov(v)_ii (=Cov(v)_ii when calib off)
                    last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
                    ++counters_.innov_flags;
                    ++counters_.innov_retaps;
                }
            }
        }
        next.push_back(std::move(cb));
    }
    carried_.swap(next);
    counters_.z_dim = static_cast<long long>(totalZDim());
}

}  // namespace GnssProcessor
