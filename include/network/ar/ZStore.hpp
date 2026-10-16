#pragma once
/**
 * @file ZStore.hpp
 * @brief DD cycle-basis z-store: the S-system-invariant quantities that the
 *        generalized-LS network filter time-links (Card 4, M1, phase B).
 *
 * Core numerics of the "invariant-function carry" (design handoff
 * `docs/SPEC_card4_invariant_carry.md`; foundations: Teunissen/Khodabandeh/
 * Psychas 2021 J Geod 95:108, generalized Kalman filter, and Hou & Zhang 2025
 * J Geod 99:3, generalized-LS filter).  The 2021 paper leaves open how to
 * construct the transition @f$\Phi^z@f$ of the time-linked functions
 * @f$z=F^\top x@f$ across a changing linked set.  This file answers it by
 * choosing @f$z@f$ to be the double-differenced (DD) ambiguities, i.e. the
 * **fundamental cycles** of a spanning tree of the receiver–satellite
 * observation graph.  These are S-system invariants: estimable under every
 * datum, integer, constant between cycle slips (@f$q_z=0@f$), and their basis
 * change under any pivot, tree or handover event is a **unimodular integer
 * matrix** (exact, with no information loss).  Basis churn, which defeated the
 * Card-3 stabilization arms, therefore leaves the carried information unchanged
 * by construction.
 *
 * ## Cycle-space identification (why @f$\Phi^z@f$ is trivial)
 *
 * For one @f$(\text{const},\sigma)@f$ observation graph (a bipartite graph whose
 * vertices are the receivers and satellites and whose arcs are the
 * receiver–satellite ambiguities present this epoch), a spanning tree of a
 * connected component with @f$n_c@f$ receivers and @f$m_c@f$ satellites has
 * @f$n_c+m_c-1@f$ tree edges.  The independent DDs are the
 * @f$E-(n_c+m_c-1)@f$ **chords** (non-tree arcs); each chord's DD is the signed
 * sum of ambiguities around its **fundamental cycle** (the chord plus the tree
 * path joining its endpoints).  Because the graph is bipartite every cycle
 * alternates receiver and satellite, so a fundamental-cycle sum is a double (or
 * higher-order alternating) difference: the receiver and satellite phase biases
 * cancel analytically, leaving @f$\pm1@f$ on four or more ambiguity columns
 * only, a **gauge-free** quantity (spec D6).
 *
 * Graph events map to exact z-store operations:
 *
 *   - **chord death** (@ref ZComponent::killChord): drop that z row/col of
 *     @f$P^z@f$.  This is pure marginalization (the covariance sub-block is the
 *     marginal; equivalently the Schur complement of the information form).
 *   - **chord birth** (@ref ZComponent::birthChord): append a z entry
 *     initialized by tap-off @f$\hat z_{\text{new}}=f^\top\hat x@f$ (GKF Eq. 22)
 *     with its conditional variance and cross-covariance, supplied by the caller
 *     (the network solve in phase C, a reference batch in the unit gates).
 *   - **leaf growth** (@ref ZComponent::addLeafArc): a new arc that brings in a
 *     new vertex extends the tree, adds no cycle and touches no z.
 *   - **tree-edge death**: @ref ZComponent::pivot a covering chord into the tree
 *     (a unimodular basis change, @f$z'=Tz,\ P^{z\prime}=TP^zT^\top@f$), then
 *     @ref ZComponent::killChord the dead arc, which is now a chord.  If no chord
 *     covers the dying tree edge it is a cut edge and its death splits the
 *     component in two.  No z entry references a cut edge, so the split only
 *     re-partitions the existing blocks between the two sides (no transform).
 *     The phase-C epoch driver re-derives the component set each epoch (the eq34
 *     union-find), so this file has no split primitive; splitting is left to that
 *     driver.
 *   - **component merge** (@ref ZComponent::mergedWith): union the two z-blocks
 *     block-diagonally (they were independent), promote one bridge arc to the
 *     tree, and tap off any surplus bridges as new chords.  @f$T@f$ is the block
 *     identity on the carried z: the bridge is a cut edge, so every pre-existing
 *     chord's fundamental cycle is unchanged (OPEN-2 of the phase-A scout).
 *
 * ## Scope of this file (Card 4 phase B)
 *
 * Pure numeric leaf, header-only, with no `ApplicationNetwork` / `StateManager` /
 * `NetworkNormalEquations` coupling (following the standalone classes
 * `NetworkCarryVce` / `NeqAccumulator`).  It owns the z-store data structure, the
 * fundamental-cycle algebra, the lifecycle transforms, and a small Kalman
 * predict/update on z.  `tests/network/test_network_zstore.cpp` drives it directly with
 * synthetic components of known truth (unit gates 1–3: CycleBasisUnimodular,
 * ChordLifecycle, FilterEqualsBatch).  Not in this file:
 *   - phase C: wiring the store to the producer solve, i.e. the joint rank-K
 *     per-component Woodbury DD-prior fold on the retained Schur factors (the
 *     generalization of `foldReducedSumZeroGauges`; scout OPEN-1) and the
 *     per-epoch tree hand-off from the eq34 election, plus gate 4 DatumFreedom.
 *     These are in `NetworkInvariantCarryDriver` and
 *     `NetworkNormalEquations::foldReducedDdPriors`;
 *   - phase D: the emit-layer frame re-basing, the run-end counter line, the
 *     config toggles (`network.carry_invariant*`), and the field falsifier.
 *
 * Production use: `gnss_core` includes this header.  @ref ArcKey (defined in
 * ArcGraph.hpp since card AGT, included here) keys the carry
 * maps (NetworkRunContext, NetworkDdPriorBuild, NetworkInstallCarry).  The
 * @ref NetworkZStore container, the lifecycle
 * transforms and the Kalman recursion on z are exercised by the unit gates (the
 * driver's gate uses them as the phase-B oracle), not by the producer.
 *
 * Since card AGT (step AGT-3) the producer builds no @ref ZComponent either:
 * `NetworkInvariantCarryDriver` takes its trees and fundamental cycles from
 * filter/NetworkArcGraph (`arcgraph::singleTreeOf`, `arcgraph::fundamentalCycle`).
 * @ref ZComponent stays here as the phase-B test oracle: of the z-store gates
 * (tests/network/test_network_zstore.cpp), of the driver's gate, and of the arc-graph
 * oracles that prove those two functions equal to its constructor and its
 * @ref ZComponent::fundamentalCycle (tests/network/test_network_arc_graph_oracles.cpp).
 * Two tree-path implementations therefore coexist until the retired lineage that
 * runs the carry is deleted.
 */

#include <Eigen/Dense>

#include "network/ar/ArcGraph.hpp"   // ArcKey (moved there verbatim by card AGT, AGT-1)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief One connected @f$(\text{const},\sigma)@f$ component's z-block: its chord
 *        set (the DD basis), the carried DD means @f$\hat z@f$ and covariance
 *        @f$P^z@f$, and the spanning tree that defines the fundamental cycles.
 *
 * Invariant: @ref chords_ .size() == @ref z_ .size() == @ref Pz_ rows == cols,
 * and the tree edges (@ref treeAdj_) plus the chords are exactly the component's
 * arcs.  The tree is stored as undirected adjacency; every arc's `ArcKey`
 * (hence its sign) is recovered from a vertex pair, so no separate edge list is
 * needed.  The z-vector index of a chord is its position in @ref chords_ .
 *
 * @note The phase-B test oracle since card AGT step AGT-3: no production code
 *       constructs it (the carry driver's trees and cycles come from
 *       filter/NetworkArcGraph); see the file banner.
 */
class ZComponent {
public:
    /// Lifecycle event tallies (spec D5; the run-end line itself is phase D).
    struct Counters {
        long long chords_born{0};       ///< tap-off births (@ref birthChord).
        long long chords_died{0};       ///< marginalizations (@ref killChord).
        long long tree_swaps{0};        ///< unimodular pivots (@ref pivot).
        long long tapoffs{0};           ///< z entries initialized by tap-off (births + merge surplus bridges).
        long long leaf_growths{0};      ///< tree extensions with no new cycle (@ref addLeafArc).
        long long predicts{0};          ///< @ref predict invocations.  A per-chord prediction-only
                                        ///  tally belongs to the phase-C epoch driver: the store
                                        ///  cannot see which chords lacked a phase row this epoch.
    };

    ZComponent() = default;

    /**
     * @brief Seed a component from a spanning tree (edge list), with no chords.
     *
     * The @p tree_edges must form a connected acyclic subgraph; the vertex set is
     * their union.  Chords are then added one at a time (@ref birthChord) as arcs
     * that close cycles appear, the way phase C grows the store from the
     * election's tree.  A single-vertex or empty component is legal (no edges).
     *
     * @param tree_edges The spanning-tree arcs.
     * @throws std::invalid_argument if the edges are not a forest joining one
     *         component (a cycle among them, or a disconnected set).
     */
    explicit ZComponent(const std::vector<ArcKey>& tree_edges) {
        for (const ArcKey& e : tree_edges) addTreeEdgeChecked(e);
        if (!isConnectedTree())   // vacuously true for the empty component
            throw std::invalid_argument("ZComponent: tree_edges are not a single connected tree");
    }

    // --- structure queries --------------------------------------------------

    Eigen::Index zDim() const { return static_cast<Eigen::Index>(chords_.size()); }
    const std::vector<ArcKey>& chords() const { return chords_; }
    const Eigen::VectorXd& z() const { return z_; }
    const Eigen::MatrixXd& Pz() const { return Pz_; }
    Eigen::VectorXd& zMutable() { return z_; }
    Eigen::MatrixXd& PzMutable() { return Pz_; }
    const std::set<int>& vertices() const { return vertices_; }
    const Counters& counters() const { return counters_; }

    bool hasVertex(int w) const { return vertices_.count(w) != 0; }
    bool isTreeEdge(const ArcKey& e) const {
        auto it = treeAdj_.find(e.u);
        return it != treeAdj_.end() && it->second.count(e.v) != 0;
    }
    bool isChord(const ArcKey& c) const {
        return std::find(chords_.begin(), chords_.end(), c) != chords_.end();
    }
    /// Position of chord @p c in the z-vector (throws if not a chord).
    Eigen::Index chordIndex(const ArcKey& c) const {
        auto it = std::find(chords_.begin(), chords_.end(), c);
        if (it == chords_.end()) throw std::invalid_argument("ZComponent::chordIndex: not a chord");
        return static_cast<Eigen::Index>(it - chords_.begin());
    }
    /// The tree edges (undirected), each returned once in canonical orientation.
    std::vector<ArcKey> treeEdges() const {
        std::vector<ArcKey> out;
        for (const auto& [x, nbrs] : treeAdj_)
            for (int y : nbrs)
                if (x < y) out.emplace_back(x, y);
        return out;
    }

    /**
     * @brief The signed fundamental cycle of an arc: @f$\chi_c(\text{arc})\in
     *        \{-1,0,+1\}@f$, the incidence used to form @f$z_c=\sum_a\chi_c(a)N_a@f$.
     *
     * For a chord @f$c=(u,v)@f$ (canonical @f$u<v@f$) the cycle traverses the
     * chord @f$u\to v@f$ (coefficient +1) then the tree path @f$v\to u@f$; each
     * tree edge on the path contributes +1 when traversed in its reference
     * orientation (min→max) and −1 otherwise.  Works for any arc whose endpoints
     * are both in the component (a chord or, for a hypothetical arc, its would-be
     * cycle); the arc's endpoints must be tree-connected.
     *
     * @param arc The arc whose fundamental cycle is wanted.
     * @return Map arc → signed coefficient (nonzero entries only; includes @p arc
     *         itself with +1).
     * @throws std::invalid_argument if the endpoints are not tree-connected.
     */
    std::map<ArcKey, int> fundamentalCycle(const ArcKey& arc) const {
        std::map<ArcKey, int> cyc;
        cyc[arc] += 1;                       // chord in reference orientation (+1)
        accumulateSignedPath(arc.v, arc.u, cyc);   // close the loop through the tree
        return cyc;
    }

    // --- lifecycle: births --------------------------------------------------

    /**
     * @brief Grow the tree by an arc that introduces a new vertex (leaf growth);
     *        adds no cycle and no z entry.
     *
     * One endpoint must already be in the component and the other must be new;
     * the arc becomes a tree edge.  On an empty component the first arc is
     * accepted with both endpoints new.  (An arc whose endpoints are both present
     * closes a cycle: use @ref birthChord.  An arc joining two separate
     * components is a merge bridge: use @ref mergedWith.)
     *
     * @param arc The new tree arc (one new endpoint).
     * @throws std::invalid_argument if neither or both endpoints are new
     *         (non-empty component).
     */
    void addLeafArc(const ArcKey& arc) {
        const bool hu = hasVertex(arc.u), hv = hasVertex(arc.v);
        if (vertices_.empty()) { addTreeEdgeRaw(arc); ++counters_.leaf_growths; return; }
        if (hu == hv)
            throw std::invalid_argument("ZComponent::addLeafArc: arc must have exactly one new endpoint");
        addTreeEdgeRaw(arc);
        ++counters_.leaf_growths;
    }

    /**
     * @brief Tap-off birth of a new chord: append its DD mean and covariance
     *        (variance + cross-covariance to the existing z), GKF Eq. 22.
     *
     * Both endpoints must already be tree-connected in the component (the arc
     * closes a cycle).  The existing z entries are unchanged (their fundamental
     * cycles do not involve a new chord); only the appended entry is new.  The
     * supplied @p zhat / @p var / @p cross are the conditional distribution of
     * this DD given the current data, @f$\hat z_{\text{new}}=f^\top\hat x@f$,
     * @f$\mathrm{Var}=f^\top P_x f@f$, @f$\mathrm{Cov}(z_{\text{new}},z_{\text{old}})
     * =f^\top P_x C_{\text{old}}^\top@f$, computed by the caller from the solve
     * (phase C) or the reference batch (the unit gates); the store only appends.
     *
     * @param chord The new chord arc (both endpoints already present).
     * @param zhat  Tap-off DD mean.
     * @param var   Tap-off DD variance (> 0).
     * @param cross Cross-covariance to the existing z entries (length @ref zDim();
     *              an empty vector is treated as zero).
     * @throws std::invalid_argument on a bad arc, non-positive variance, or a
     *         @p cross size mismatch.
     */
    void birthChord(const ArcKey& chord, double zhat, double var,
                    const Eigen::VectorXd& cross = Eigen::VectorXd()) {
        if (!hasVertex(chord.u) || !hasVertex(chord.v))
            throw std::invalid_argument("ZComponent::birthChord: endpoints not in component");
        if (isTreeEdge(chord) || isChord(chord))
            throw std::invalid_argument("ZComponent::birthChord: arc already present");
        if (!(var > 0.0))
            throw std::invalid_argument("ZComponent::birthChord: variance must be positive");
        const Eigen::Index n = zDim();
        if (cross.size() != 0 && cross.size() != n)
            throw std::invalid_argument("ZComponent::birthChord: cross size mismatch");

        chords_.push_back(chord);
        arcs_.insert(chord);
        Eigen::VectorXd znew(n + 1);
        znew.head(n) = z_;
        znew(n) = zhat;
        z_ = std::move(znew);
        Eigen::MatrixXd Pnew(n + 1, n + 1);
        Pnew.topLeftCorner(n, n) = Pz_;
        if (n > 0) {
            const Eigen::VectorXd c = (cross.size() == n) ? cross : Eigen::VectorXd::Zero(n);
            Pnew.topRightCorner(n, 1) = c;
            Pnew.bottomLeftCorner(1, n) = c.transpose();
        }
        Pnew(n, n) = var;
        Pz_ = std::move(Pnew);
        ++counters_.chords_born;
        ++counters_.tapoffs;
    }

    // --- lifecycle: deaths --------------------------------------------------

    /**
     * @brief Marginalize out a dead chord: drop its z row/col from
     *        @f$\hat z@f$ / @f$P^z@f$.
     *
     * For a joint Gaussian the marginal over the kept entries is the
     * corresponding covariance sub-block; no Schur step is needed in covariance
     * form (it equals the Schur-complement marginalization of the information
     * matrix, by the block-inverse identity the unit gate checks).  The chord's
     * arc leaves the graph; its endpoints stay tree-connected, so no vertex is
     * orphaned.
     *
     * @param chord The dying chord arc.
     * @throws std::invalid_argument if @p chord is not a current chord.
     */
    void killChord(const ArcKey& chord) {
        const Eigen::Index p = chordIndex(chord);
        const Eigen::Index n = zDim();
        chords_.erase(chords_.begin() + p);
        arcs_.erase(chord);
        z_ = dropElem(z_, p);
        Pz_ = dropRowCol(Pz_, p);
        (void)n;
        ++counters_.chords_died;
    }

    // --- lifecycle: the unimodular pivot ------------------------------------

    /**
     * @brief Swap a chord into the tree in place of a dying tree edge: the exact
     *        unimodular basis change @f$z'=Tz,\ P^{z\prime}=TP^zT^\top@f$.
     *
     * This is the @f$\Phi^z@f$ the 2021 paper leaves open, made trivial by the
     * cycle basis.  @p dying_tree_edge @f$e@f$ must be a tree edge and
     * @p entering_chord @f$f@f$ a chord whose fundamental cycle contains @f$e@f$
     * (so removing @f$e@f$ and adding @f$f@f$ still spans).  Writing
     * @f$\sigma=\chi_f(e)=\pm1@f$ and @f$\tau_c=\chi_c(e)@f$ for every other chord
     * @f$c@f$, the new DDs are
     * @f[
     *   z'_e=\sigma\,z_f,\qquad z'_c=z_c-\tau_c\,\sigma\,z_f\ (c\neq f),
     * @f]
     * i.e. @f$T@f$ is the identity except that column @f$p_f@f$ holds
     * @f$-\tau_c\sigma@f$ in every other row, and row @f$p_f@f$ (now @f$e@f$) is
     * @f$\sigma\,e_{p_f}^\top@f$.  Expanding along that row gives
     * @f$\det T=\sigma=\pm1@f$: an integer matrix with unit determinant, so the
     * transform is lossless and keeps the DDs integer.  The new chord list is the
     * old one with @f$f@f$ replaced by @f$e@f$ at the same index, so unrelated z
     * entries keep their slot.  This is a pure basis change and the arc set is
     * unchanged; call @ref killChord on @f$e@f$ afterwards to model the death of
     * @f$e@f$'s arc.
     *
     * @param dying_tree_edge The tree edge to demote to a chord.
     * @param entering_chord  The chord to promote to a tree edge (must cover @p dying_tree_edge).
     * @return The applied unimodular @f$T@f$ (square, size @ref zDim()), for the
     *         gate's integrality/determinant check.
     * @throws std::invalid_argument if @p dying_tree_edge is not a tree edge,
     *         @p entering_chord is not a chord, or the chord does not cover the edge.
     */
    Eigen::MatrixXd pivot(const ArcKey& dying_tree_edge, const ArcKey& entering_chord) {
        if (!isTreeEdge(dying_tree_edge))
            throw std::invalid_argument("ZComponent::pivot: dying_tree_edge is not a tree edge");
        if (!isChord(entering_chord))
            throw std::invalid_argument("ZComponent::pivot: entering_chord is not a chord");
        const Eigen::Index n = zDim();
        const Eigen::Index pf = chordIndex(entering_chord);

        // sigma = sign of the dying edge in the entering chord's cycle (must be ±1).
        const std::map<ArcKey, int> cyc_f = fundamentalCycle(entering_chord);
        auto sit = cyc_f.find(dying_tree_edge);
        const int sigma = (sit != cyc_f.end()) ? sit->second : 0;
        if (sigma != 1 && sigma != -1)
            throw std::invalid_argument("ZComponent::pivot: entering_chord does not cover the dying edge");

        Eigen::MatrixXd T = Eigen::MatrixXd::Identity(n, n);
        for (Eigen::Index i = 0; i < n; ++i) {
            if (i == pf) continue;                       // e-row filled below
            const std::map<ArcKey, int> cyc_c = fundamentalCycle(chords_[static_cast<std::size_t>(i)]);
            auto tit = cyc_c.find(dying_tree_edge);
            const int tau = (tit != cyc_c.end()) ? tit->second : 0;
            T(i, i) = 1.0;
            T(i, pf) = -static_cast<double>(tau) * static_cast<double>(sigma);
        }
        T.row(pf).setZero();
        T(pf, pf) = static_cast<double>(sigma);          // z'_e = sigma * z_f

        // Apply the transform before mutating the graph (the cycles read the old tree).
        z_ = T * z_;
        Pz_ = T * Pz_ * T.transpose();
        Pz_ = 0.5 * (Pz_ + Pz_.transpose());             // symmetrize against round-off

        // Mutate the graph: e leaves the tree and becomes the chord at pf; f enters the tree.
        removeTreeEdgeRaw(dying_tree_edge);
        addTreeEdgeRaw(entering_chord);
        arcs_.insert(dying_tree_edge);                   // e is still an arc (now a chord)
        chords_[static_cast<std::size_t>(pf)] = dying_tree_edge;
        ++counters_.tree_swaps;
        return T;
    }

    // --- lifecycle: merge (OPEN-2: T = block identity) ----------------------

    struct SurplusBridge {
        ArcKey arc;
        double zhat{0.0};
        double var{1.0};
        Eigen::VectorXd cross;   ///< cross-cov to the merged z as it stands when this bridge is folded.
    };
    /**
     * @brief Merge another component into this one via a bridge arc: a
     *        block-identity union (the bridge is a cut edge).
     *
     * The bridge joins a vertex of @c *this to a vertex of @p other.  Since it is a
     * cut edge, every pre-existing chord's fundamental cycle on either side is
     * unchanged, so the carried @f$\hat z@f$ / @f$P^z@f$ of both sides transfer
     * verbatim: the merged z is the concatenation @f$[\hat z_A;\hat z_B]@f$ and the
     * merged covariance is block-diagonal @f$\mathrm{diag}(P^z_A,P^z_B)@f$ (the two
     * were independent, with zero cross-covariance).  The bridge becomes a tree
     * edge (no z).  Any @p surplus_bridges (additional arcs also joining the two
     * former components) close cycles and are tapped off as new chords with the
     * supplied (mean, var, cross-to-the-merged-z); cross is ordered as the merged
     * z, @c *this 's entries first, then @p other 's.  Lifecycle counters of both
     * sides are summed into the result.
     *
     * @param other          The component to absorb (must be vertex-disjoint from @c *this).
     * @param bridge          The arc promoted to a tree edge (one endpoint in each side).
     * @param surplus_bridges Extra joining arcs, each with its tap-off
     *                        {mean, var, cross}; cross has length
     *                        (this.zDim()+other.zDim()) growing as each is added.
     * @return The merged component.
     * @throws std::invalid_argument on overlapping vertices or a bridge not
     *         spanning the two sides.
     */
    ZComponent mergedWith(const ZComponent& other, const ArcKey& bridge,
                          const std::vector<SurplusBridge>& surplus_bridges = {}) const {
        for (int w : other.vertices_)
            if (vertices_.count(w))
                throw std::invalid_argument("ZComponent::mergedWith: components share a vertex");
        const bool a_u = hasVertex(bridge.u), a_v = hasVertex(bridge.v);
        const bool b_u = other.hasVertex(bridge.u), b_v = other.hasVertex(bridge.v);
        const bool spans = (a_u && b_v) || (a_v && b_u);
        if (!spans)
            throw std::invalid_argument("ZComponent::mergedWith: bridge does not join the two components");

        ZComponent m;
        // vertices + tree edges from both sides, then the bridge.
        m.vertices_ = vertices_;
        m.vertices_.insert(other.vertices_.begin(), other.vertices_.end());
        m.treeAdj_ = treeAdj_;
        for (const auto& [x, nbrs] : other.treeAdj_)
            for (int y : nbrs) m.treeAdj_[x].insert(y);
        m.arcs_ = arcs_;
        m.arcs_.insert(other.arcs_.begin(), other.arcs_.end());
        m.addTreeEdgeRaw(bridge);

        // chords: this side's first, then other's (block identity, so copied verbatim).
        m.chords_ = chords_;
        m.chords_.insert(m.chords_.end(), other.chords_.begin(), other.chords_.end());
        const Eigen::Index na = zDim(), nb = other.zDim();
        m.z_.resize(na + nb);
        if (na > 0) m.z_.head(na) = z_;
        if (nb > 0) m.z_.tail(nb) = other.z_;
        m.Pz_ = Eigen::MatrixXd::Zero(na + nb, na + nb);
        if (na > 0) m.Pz_.topLeftCorner(na, na) = Pz_;
        if (nb > 0) m.Pz_.bottomRightCorner(nb, nb) = other.Pz_;
        m.counters_ = counters_;
        m.counters_.chords_born += other.counters_.chords_born;
        m.counters_.chords_died += other.counters_.chords_died;
        m.counters_.tree_swaps += other.counters_.tree_swaps;
        m.counters_.tapoffs += other.counters_.tapoffs;
        m.counters_.leaf_growths += other.counters_.leaf_growths;
        m.counters_.predicts += other.counters_.predicts;

        for (const SurplusBridge& sb : surplus_bridges)
            m.birthChord(sb.arc, sb.zhat, sb.var, sb.cross);
        return m;
    }

    // --- the Kalman recursion on z (Phi = I, q_z = 0 by default) -------------

    /**
     * @brief Time update: @f$\hat z@f$ unchanged (@f$\Phi=I@f$), @f$P^z\mathrel{+}=
     *        q_z\,\Delta t\,I@f$ (default @f$q_z=0@f$: ambiguities are constant
     *        between slips).
     *
     * @param q_z Process-noise PSD on the DDs (m²/s); 0 = exact random constant.
     * @param dt  Elapsed time (s).
     */
    void predict(double q_z = 0.0, double dt = 0.0) {
        if (q_z > 0.0 && dt > 0.0 && zDim() > 0)
            Pz_.diagonal().array() += q_z * dt;
        ++counters_.predicts;
    }

    /**
     * @brief Measurement update of the DDs: the exact recursive-least-squares
     *        (information-preserving) Kalman fold, Joseph-stabilized.
     *
     * Folds an observation @f$o=Hz+\eta,\ \eta\sim N(0,R)@f$ of the current z:
     * @f$S=HP^zH^\top+R@f$, @f$K=P^zH^\top S^{-1}@f$,
     * @f$\hat z\mathrel{+}=K(o-H\hat z)@f$,
     * @f$P^z\leftarrow(I-KH)P^z(I-KH)^\top+KRK^\top@f$.  Equivalent to adding
     * @f$H^\top R^{-1}H@f$ to the information, the property the FilterEqualsBatch
     * gate uses (sequential folds equal one batch WLS).  DDs with a zero column in
     * @p H are updated only through their covariance coupling (a chord with no
     * phase row this epoch is prediction-only, never deleted).  Returns without
     * change when @p o or z is empty.
     *
     * @param H Observation design (m × @ref zDim()).
     * @param o Observation vector (length m).
     * @param R Observation covariance (m × m, symmetric positive-definite).
     * @throws std::invalid_argument on a dimension mismatch.
     */
    void update(const Eigen::MatrixXd& H, const Eigen::VectorXd& o, const Eigen::MatrixXd& R) {
        const Eigen::Index n = zDim(), m = o.size();
        if (H.rows() != m || H.cols() != n || R.rows() != m || R.cols() != m)
            throw std::invalid_argument("ZComponent::update: dimension mismatch");
        if (m == 0 || n == 0) return;
        const Eigen::MatrixXd S = H * Pz_ * H.transpose() + R;
        const Eigen::MatrixXd K = Pz_ * H.transpose() * S.ldlt().solve(Eigen::MatrixXd::Identity(m, m));
        z_ += K * (o - H * z_);
        const Eigen::MatrixXd ImKH = Eigen::MatrixXd::Identity(n, n) - K * H;
        Pz_ = ImKH * Pz_ * ImKH.transpose() + K * R * K.transpose();
        Pz_ = 0.5 * (Pz_ + Pz_.transpose());
    }

    // --- diagnostics / references for the gates -----------------------------

    /**
     * @brief The fundamental-cycle incidence matrix @f$C@f$ (chords × arcs) over a
     *        given arc ordering: the from-scratch reference for the gates.
     *
     * Row @f$i@f$ is @ref fundamentalCycle of @c chords()[i], projected onto
     * @p arc_order (arcs absent from @p arc_order are omitted).  Then @f$z=C\,N@f$
     * and @f$P^z=C\,\mathrm{Cov}(N)\,C^\top@f$ from the raw ambiguity mean and
     * covariance reproduce the carried store independently of the incremental
     * transforms, which is what CycleBasisUnimodular checks.
     */
    Eigen::MatrixXd cycleMatrix(const std::vector<ArcKey>& arc_order) const {
        std::map<ArcKey, Eigen::Index> col;
        for (Eigen::Index j = 0; j < static_cast<Eigen::Index>(arc_order.size()); ++j)
            col[arc_order[static_cast<std::size_t>(j)]] = j;
        Eigen::MatrixXd C = Eigen::MatrixXd::Zero(zDim(), static_cast<Eigen::Index>(arc_order.size()));
        for (Eigen::Index i = 0; i < zDim(); ++i)
            for (const auto& [arc, s] : fundamentalCycle(chords_[static_cast<std::size_t>(i)])) {
                auto it = col.find(arc);
                if (it != col.end()) C(i, it->second) = static_cast<double>(s);
            }
        return C;
    }

    /// All arcs (tree edges + chords), sorted, for building a @ref cycleMatrix column order.
    std::vector<ArcKey> allArcs() const {
        std::vector<ArcKey> out(arcs_.begin(), arcs_.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    /**
     * @brief Install a chord set and carried estimate directly: the gates' seam for
     *        seeding a component with a known @f$z=CN@f$ / @f$P^z=C\,\mathrm{Cov}\,C^\top@f$.
     *
     * Every arc in @p chord_order must have both endpoints already in the tree and
     * must not be a tree edge (it closes a cycle).  Replaces the current chord
     * list, @f$\hat z@f$ and @f$P^z@f$.  Throws std::invalid_argument on a size
     * mismatch or an invalid chord.
     */
    void setEstimate(const std::vector<ArcKey>& chord_order,
                     const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz) {
        if (chord_order.size() != static_cast<std::size_t>(z.size()) ||
            z.size() != Pz.rows() || Pz.rows() != Pz.cols())
            throw std::invalid_argument("ZComponent::setEstimate: size mismatch");
        for (const ArcKey& c : chord_order) {
            if (!hasVertex(c.u) || !hasVertex(c.v))
                throw std::invalid_argument("ZComponent::setEstimate: chord endpoint not in component");
            if (isTreeEdge(c))
                throw std::invalid_argument("ZComponent::setEstimate: arc is a tree edge, not a chord");
        }
        chords_.assign(chord_order.begin(), chord_order.end());
        // Rebuild the arc set from tree ∪ chords, so a re-seed with a smaller or
        // different chord set leaves no stale arcs (the invariant allArcs() relies on).
        arcs_.clear();
        for (const ArcKey& e : treeEdges()) arcs_.insert(e);
        for (const ArcKey& c : chords_) arcs_.insert(c);
        z_ = z;
        Pz_ = Pz;
    }

private:
    std::vector<ArcKey> chords_;                 ///< z index → chord arc.
    Eigen::VectorXd z_;                          ///< carried DD means.
    Eigen::MatrixXd Pz_;                         ///< carried DD covariance.
    std::map<int, std::set<int>> treeAdj_;       ///< undirected spanning-tree adjacency.
    std::set<ArcKey> arcs_;                      ///< every arc (tree edges ∪ chords).
    std::set<int> vertices_;                     ///< all component vertices.
    Counters counters_;

    // --- tree bookkeeping ---
    void addTreeEdgeRaw(const ArcKey& e) {
        treeAdj_[e.u].insert(e.v);
        treeAdj_[e.v].insert(e.u);
        vertices_.insert(e.u);
        vertices_.insert(e.v);
        arcs_.insert(e);
    }
    void removeTreeEdgeRaw(const ArcKey& e) {
        auto iu = treeAdj_.find(e.u);
        if (iu != treeAdj_.end()) iu->second.erase(e.v);
        auto iv = treeAdj_.find(e.v);
        if (iv != treeAdj_.end()) iv->second.erase(e.u);
    }
    void addTreeEdgeChecked(const ArcKey& e) {
        // A tree edge must not close a cycle among the edges added so far.
        if (hasVertex(e.u) && hasVertex(e.v) && treeConnected(e.u, e.v))
            throw std::invalid_argument("ZComponent: tree_edges contain a cycle");
        addTreeEdgeRaw(e);
    }

    // BFS reachability within the current tree.
    bool treeConnected(int a, int b) const {
        if (a == b) return true;
        std::set<int> seen{a};
        std::deque<int> q{a};
        while (!q.empty()) {
            const int x = q.front(); q.pop_front();
            auto it = treeAdj_.find(x);
            if (it == treeAdj_.end()) continue;
            for (int y : it->second)
                if (seen.insert(y).second) { if (y == b) return true; q.push_back(y); }
        }
        return false;
    }
    bool isConnectedTree() const {
        if (vertices_.empty()) return true;
        const int s = *vertices_.begin();
        std::set<int> seen{s};
        std::deque<int> q{s};
        while (!q.empty()) {
            const int x = q.front(); q.pop_front();
            auto it = treeAdj_.find(x);
            if (it == treeAdj_.end()) continue;
            for (int y : it->second)
                if (seen.insert(y).second) q.push_back(y);
        }
        return seen.size() == vertices_.size();
    }

    // Add the signed tree path from → to into @p acc (each edge ±1 by canonical orientation).
    void accumulateSignedPath(int from, int to, std::map<ArcKey, int>& acc) const {
        // BFS parent walk in the tree.
        std::map<int, int> parent;
        parent[from] = from;
        std::deque<int> q{from};
        bool found = (from == to);
        while (!q.empty() && !found) {
            const int x = q.front(); q.pop_front();
            auto it = treeAdj_.find(x);
            if (it == treeAdj_.end()) continue;
            for (int y : it->second) {
                if (parent.count(y)) continue;
                parent[y] = x;
                if (y == to) { found = true; break; }
                q.push_back(y);
            }
        }
        if (!found)
            throw std::invalid_argument("ZComponent: endpoints not tree-connected");
        // Walk to → from via parents, signing each edge by traversal direction.
        int cur = to;
        while (cur != from) {
            const int par = parent.at(cur);
            // The walk goes backwards, but the path being accumulated runs
            // from → to, so each step is traversed par → cur.
            const ArcKey e(par, cur);
            acc[e] += (par < cur) ? +1 : -1;   // +1 if traversed in reference (min→max) orientation
            cur = par;
        }
    }

    static Eigen::VectorXd dropElem(const Eigen::VectorXd& v, Eigen::Index p) {
        const Eigen::Index n = v.size();
        Eigen::VectorXd out(n - 1);
        if (p > 0) out.head(p) = v.head(p);
        if (p < n - 1) out.tail(n - 1 - p) = v.tail(n - 1 - p);
        return out;
    }
    static Eigen::MatrixXd dropRowCol(const Eigen::MatrixXd& M, Eigen::Index p) {
        const Eigen::Index n = M.rows();
        Eigen::MatrixXd out(n - 1, n - 1);
        for (Eigen::Index i = 0, oi = 0; i < n; ++i) {
            if (i == p) continue;
            for (Eigen::Index j = 0, oj = 0; j < n; ++j) {
                if (j == p) continue;
                out(oi, oj) = M(i, j);
                ++oj;
            }
            ++oi;
        }
        return out;
    }

    friend class NetworkZStore;
};

/**
 * @brief The whole-network z-store: one @ref ZComponent per connected
 *        @f$(\text{const},\sigma)@f$ component, keyed by a per-epoch canonical id,
 *        plus aggregate counters.
 *
 * The canonical id follows scout OPEN-2: the minimum satellite label of the
 * component, anchored on the satellite label because receiver pool indices are
 * reused.  The container provides storage, the merge entry point and counter
 * aggregation; it has no split primitive (a split is a re-partition, see the file
 * header).  Mapping the eq34 election's tree to lifecycle calls each epoch is the
 * phase-C driver's job; `NetworkInvariantCarryDriver` computes the composed
 * change-of-basis projection directly and uses this lifecycle as the reference
 * in its unit gate.  The z-store is keyed by chord identity inside each
 * component, so a component id that changes across epochs is only a block
 * rename; no cross-epoch-stable id is required.
 */
class NetworkZStore {
public:
    using ComponentId = int;

    ZComponent& component(ComponentId id) { return comps_[id]; }
    ZComponent& create(ComponentId id, const std::vector<ArcKey>& tree_edges) {
        return comps_.emplace(id, ZComponent(tree_edges)).first->second;
    }
    const ZComponent* find(ComponentId id) const {
        auto it = comps_.find(id);
        return it == comps_.end() ? nullptr : &it->second;
    }
    bool erase(ComponentId id) { return comps_.erase(id) != 0; }
    std::size_t numComponents() const { return comps_.size(); }
    const std::map<ComponentId, ZComponent>& components() const { return comps_; }

    /**
     * @brief Merge component @p b into @p a via @p bridge, replacing @p a with the
     *        block-identity union and erasing @p b.  The result is stored under
     *        @p a; passing @p a = min(a, b) to keep the canonical id is the
     *        caller's contract.  Throws std::invalid_argument on an unknown id.
     */
    void merge(ComponentId a, ComponentId b, const ArcKey& bridge,
               const std::vector<ZComponent::SurplusBridge>& surplus = {}) {
        auto ia = comps_.find(a), ib = comps_.find(b);
        if (ia == comps_.end() || ib == comps_.end())
            throw std::invalid_argument("NetworkZStore::merge: unknown component id");
        ia->second = ia->second.mergedWith(ib->second, bridge, surplus);
        comps_.erase(ib);
        ++merges_;
    }

    /// Total carried DD dimension across all components (spec D5 z-dim meter).
    Eigen::Index totalZDim() const {
        Eigen::Index t = 0;
        for (const auto& [id, c] : comps_) t += c.zDim();
        return t;
    }

    /// Sum of the lifecycle counters of the components currently held (the
    /// container's merge count is separate: @ref merges).
    ZComponent::Counters aggregateCounters() const {
        ZComponent::Counters t;
        for (const auto& [id, c] : comps_) {
            const auto& k = c.counters();
            t.chords_born += k.chords_born;
            t.chords_died += k.chords_died;
            t.tree_swaps += k.tree_swaps;
            t.tapoffs += k.tapoffs;
            t.leaf_growths += k.leaf_growths;
            t.predicts += k.predicts;
        }
        return t;
    }
    long long merges() const { return merges_; }

private:
    std::map<ComponentId, ZComponent> comps_;
    long long merges_{0};
};

}  // namespace GnssProcessor
