#pragma once
/**
 * @file NetworkInvariantCarryDriver.hpp
 * @brief Card 4 (M1) phase C3b — the LIVE per-epoch driver that time-links the
 *        DD cycle-basis carry (`NetworkZStore`) through the network producer solve.
 *
 * This is the factored, unit-drivable heart of the "invariant-function carry"
 * (design handoff `docs/HANDOFF_card4_invariant_carry.md`).  Phase B built the
 * z-store data structure + the fundamental-cycle algebra + the lifecycle
 * transforms (`NetworkZStore.hpp`, gates 1–3); phase C built the producer-side
 * fold (`NetworkNormalEquations::foldReducedDdPriors` + `ddPriorPosteriorMean` /
 * `ddPriorPosteriorCovariance`, gate 4 DatumFreedom).  This driver is the seam
 * that runs BETWEEN them each epoch:
 *
 *   1. it is handed the eq34 election's spanning-tree/chord partition of this
 *      epoch's (const,signal) observation graph (the @ref GroupInput list — one
 *      resolvable arc per present ambiguity, each tagged tree-edge or chord and
 *      resolved to its estimator (station, local ambiguity column));
 *   2. it RECONCILES the carried DD estimate to this epoch's basis — a single
 *      change-of-basis projection @f$W@f$ per connected component that subsumes
 *      the phase-B lifecycle events (tree pivot = unimodular @f$T@f$, chord death
 *      = marginalization, chord birth = tap-off, component merge/split = block
 *      re-partition) — and emits one @ref NetworkNormalEquations::DdPriorBlock per
 *      component for the producer to fold (@ref buildDdPriors);
 *   3. after the solve it installs the posterior (mean + covariance, the closed
 *      form phase C added) back into the carry (@ref installPosterior).
 *
 * ## The reconcile is a projection, validated against the phase-B lifecycle
 *
 * Carrying @f$\hat z@f$ across a basis change is, in phase B, a sequence of
 * unimodular pivots.  This driver computes the composed transform directly:
 * a new chord @f$c@f$'s fundamental cycle (w.r.t. THIS epoch's tree) lies in the
 * previous epoch's cycle space iff every arc of the cycle was present last epoch,
 * and then its carried mean is @f$\hat z^{\text{new}}_c=\sum_j w_j\,\hat
 * z^{\text{old}}_j@f$ with @f$w_j=\chi_c(\text{arc of old chord }j)@f$ — needing
 * only the OLD CHORD ARCS, not the old tree.  Assembling @f$W@f$ over the
 * contributing old components' chords yields @f$\hat z^{\text{prior}}=W\hat
 * z^{\text{old}}@f$, @f$P^{z,\text{prior}}=W P^{z,\text{old}} W^\top@f$:
 *
 *   - a pure tree change (same arcs) → every new chord is carried, @f$W@f$ is the
 *     composed unimodular pivot (basis change, exact, integer);
 *   - a dead chord's arc vanishes → its column of @f$W@f$ is zero → it is
 *     marginalized (= @ref ZComponent::killChord, the covariance sub-block);
 *   - a chord whose cycle uses a BORN arc is fresh → a diffuse prior (adds no
 *     information to the solve; its posterior is the data-only tap-off, GKF Eq. 22);
 *   - a component merge gathers both sides' independent blocks block-diagonally;
 *     a split re-partitions one old block across the new components — both fall
 *     out of the vertex-overlap contributor gather with no special case.
 *
 * The `tests/test_network_invariant_carry_driver.cpp` gate cross-checks this
 * projection against a hand-sequenced `NetworkZStore` lifecycle (the phase-B
 * validated oracle) and against from-scratch cycle-matrix truth, and runs the
 * production-seam **DatumFreedom** falsifier (two different per-epoch tree
 * realizations of the same graph ⇒ identical estimable output).
 *
 * ## Scope / conventions
 *
 * Pure numeric leaf that depends only on `NetworkZStore` (cycle algebra) and the
 * `NetworkEstimator` kernel (the fold + posterior readout).  It is
 * `ApplicationNetwork`-INDEPENDENT: the caller (the live epoch loop OR a unit
 * test) resolves each arc to a stable (rx_vertex, sat_vertex) pair — disjoint id
 * ranges, receiver < satellite so the `ArcKey` orientation is receiver→satellite —
 * and to the current epoch's estimator (station, local_col).  GLONASS (FDMA) has
 * no common-wavelength integer DD and must be excluded by the caller (it never
 * enters the CDMA (const,signal) groups).  Byte-identity OFF is structural: the
 * driver is only constructed/called under `network.carry_invariant`.
 *
 * @note v1 simplification (documented for the phase-D follow-up): a carried chord
 * whose fundamental cycle has an UNRESOLVABLE arc this epoch (a dropped station or
 * a missing local column) is dropped from the carry and re-tapped-off when it
 * returns, rather than retained as a phase-B "prediction-only" chord.  Fresh
 * chords use a diffuse (finite) prior rather than the exact zero-information
 * tap-off, so their per-epoch basis-covariance holds only to @f$O(B/\text{fresh\_prior\_var})@f$
 * (immaterial for the field grade; the crisp DatumFreedom gate compares carried
 * chords, which are basis-exact).
 */

#include "filter/NetworkZStore.hpp"          // ArcKey + ZComponent cycle algebra
#include "filter/NetworkEstimator.hpp"       // DdPriorBlock, Pin/Datum, posterior readout

#include <Eigen/Dense>

#include <map>
#include <set>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Drives the DD invariant carry across one network producer epoch: election
 *        tree in → DD-prior blocks out → posterior back into the carry.
 */
class NetworkInvariantCarryDriver {
public:
    /// Tuning (spec D7 `network.carry_invariant_qz`, plus the fresh-chord diffuse prior).
    struct Config {
        double qz{0.0};                 ///< z process noise (state²/s); 0 = constant DDs (correct absent slips).
        double fresh_prior_var{1.0e6};  ///< diffuse prior variance for a fresh chord (state units²) — large ⇒ ~zero prior information; its posterior is the data-only tap-off.
    };

    /// One present, resolvable ambiguity arc this epoch (the caller has already
    /// excluded GLONASS and any arc it cannot resolve to a live column).
    struct ArcInput {
        int  rx_vertex{0};       ///< stable receiver vertex id (disjoint low range).
        int  sat_vertex{0};      ///< stable satellite vertex id (disjoint high range; > every rx id).
        bool tree_edge{false};   ///< election spanning-tree edge (gauge pin) vs chord (a carried DD).
        int  est_station{-1};    ///< estimator station index of this arc's ambiguity (−1 = unresolvable this epoch).
        int  local_col{-1};      ///< A_r local ambiguity column of this arc (−1 = unresolvable).
    };

    /// One (const,signal) group's arcs this epoch.  Groups are independent z-store
    /// partitions; a DD never crosses a group.  @c key is a caller-stable id for the
    /// group (e.g. a hash of constellation+signal) used only to bucket the carry.
    struct GroupInput {
        int key{0};
        std::vector<ArcInput> arcs;
    };

    /// Lifecycle tallies for the run-end line (spec D5).
    struct Counters {
        long long epochs{0};              ///< @ref buildDdPriors invocations.
        long long components{0};          ///< DD-prior blocks emitted (components with ≥1 foldable chord), summed over epochs.
        long long chords_carried{0};      ///< chords whose DD was carried from the previous epoch (basis-projected).
        long long chords_born{0};         ///< fresh chords (diffuse prior → data-only tap-off).
        long long chords_marginalized{0}; ///< old carried chords not carried forward (dead arc).
        long long chords_dropped{0};      ///< chords skipped this epoch (an unresolvable cycle arc — v1 refloat).
        long long merges{0};              ///< components formed from ≥2 previous components.
        long long splits{0};              ///< previous components spread across ≥2 new components.
        long long tree_swaps{0};          ///< components whose carry was re-expressed in a genuinely different fundamental-cycle basis (a projection W that is not a signed permutation — the analogue of the phase-B unimodular pivot).
        long long z_dim{0};               ///< total carried DD dimension at the end of the last epoch.
    };

    explicit NetworkInvariantCarryDriver(Config cfg = {}) : cfg_(cfg) {}

    /// Drop all carry (a toggle-flip cold start; the DDs re-tap-off next epoch).
    void reset();

    /**
     * @brief Time update: inflate the carried @f$P^z@f$ by @f$q_z\,\Delta t@f$ on
     *        the diagonal (default @f$q_z=0@f$ ⇒ no-op — DDs are constant between
     *        slips).  Call once at the top of the epoch, before @ref buildDdPriors.
     * @param dt_s Elapsed time since the previous epoch (s).
     */
    void beginEpoch(double dt_s);

    /**
     * @brief Reconcile the carry to this epoch's election tree and emit one
     *        DD-prior block per connected component with a foldable chord.
     *
     * Stashes the per-block chord layout for @ref installPosterior; the returned
     * blocks are in the order the caller must pass to
     * @ref NetworkEstimator::setDdPriors (block index @c b then addresses the
     * matching @ref NetworkEstimator::ddPriorPosteriorMean / @c ddPriorPosteriorCovariance).
     *
     * @param groups The GLONASS-excluded (const,signal) groups, each a set of
     *        resolvable tree/chord arcs.
     * @return One DD-prior block per emitted component (empty when nothing carries).
     */
    std::vector<NetworkNormalEquations::DdPriorBlock>
        buildDdPriors(const std::vector<GroupInput>& groups);

    /**
     * @brief Install the posterior (mean + covariance) back into the carry — the
     *        new @f$\hat z,P^z@f$ the next epoch projects forward.  Call after
     *        @ref NetworkEstimator::solve with the SAME shared estimate and datum.
     *
     * @param est   The estimator that folded @ref buildDdPriors's blocks this epoch.
     * @param x_s   The solved shared estimate.
     * @param datum The minimum-constraint datum passed to @ref NetworkEstimator::solve.
     */
    void installPosterior(const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                          const NetworkEstimator::Datum& datum);

    const Counters& counters() const { return counters_; }
    /// Total carried DD dimension right now (across all components).
    Eigen::Index totalZDim() const;
    /// Number of DD-prior blocks emitted by the last @ref buildDdPriors.
    std::size_t numBlocks() const { return block_meta_.size(); }

    // --- test seams (numeric leaf; the NetworkZStore::setEstimate precedent) -----

    /// Directly install one component's carry — the gate's seed for a known
    /// @f$\hat z,P^z@f$ over a chord basis (bypasses a solve).
    void seedCarriedForTest(int group, const std::vector<ArcKey>& chords,
                            const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz,
                            const std::set<int>& vertices, const std::set<ArcKey>& arcs);
    /// The chord order of DD-prior block @p b returned by the last @ref buildDdPriors
    /// (== the fold-row order, so a gate can map block rows to chord arcs).
    const std::vector<ArcKey>& blockChordsForTest(std::size_t b) const {
        return block_meta_.at(b).chords;
    }
    /// The carried DD mean+variance of @p chord in @p group (false if not carried).
    bool carriedDdForTest(int group, const ArcKey& chord, double& z, double& var) const;

    /// The carried DD cross-covariance between @p a and @p b in @p group (false unless
    /// both are chords of the same carried component).  The Fable §2.5 probe that the
    /// multi-birth joint fold coupled them (a per-chord-independent fold gives 0).
    bool carriedCrossForTest(int group, const ArcKey& a, const ArcKey& b, double& cov) const;

    /// Read a gauge-free estimable double difference straight from the carry: the
    /// value @f$\sum_j \chi_{\text{target}}(\text{arc}_j)\,\hat z_j@f$ over the carried
    /// chords of the component that carries @p target_cycle's arcs (NaN if none does).
    /// Independent of any network solve — the basis-invariance probe.
    double carriedEstimableForTest(int group, const std::map<ArcKey, int>& target_cycle) const;

private:
    /// One connected component's carried DD estimate (keyed by chord arc identity).
    struct CarriedBlock {
        int group{0};
        std::vector<ArcKey> chords;   ///< z index → chord arc (the DD basis).
        Eigen::VectorXd z;            ///< carried DD means.
        Eigen::MatrixXd Pz;           ///< carried DD covariance (dense, per component).
        std::set<int> vertices;       ///< the component's vertices.
        std::set<ArcKey> arcs;        ///< every arc present (tree ∪ chords) — A_old for the carried test.
    };
    /// Per-emitted-block metadata (parallel to the returned blocks) for the install.
    struct BlockMeta {
        int group{0};
        std::vector<ArcKey> chords;   ///< fold-row order = the new carried block's chord order.
        std::set<int> vertices;       ///< the component's vertices this epoch.
        std::set<ArcKey> arcs;        ///< A_new for this component (becomes next epoch's A_old).
    };

    Config cfg_;
    std::vector<CarriedBlock> carried_;      ///< the carry as of the previous @ref installPosterior.
    std::vector<BlockMeta>    block_meta_;   ///< layout of the blocks returned by the last @ref buildDdPriors.
    Counters counters_;
};

}  // namespace GnssProcessor
