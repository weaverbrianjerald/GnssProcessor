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
#include "filter/NetworkCarryVce.hpp"        // Card 4 D5 v2: LS-VCE robust inflation of Cov(v)

#include <Eigen/Dense>

#include <map>
#include <optional>
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
        bool   innov_gate{false};       ///< Card 4 D5: enable the estimation-side fine-slip innovation test on the carried DDs (@ref installPosterior). OFF = the posterior is installed unconditionally (byte-identical to C3c).
        double innov_k{4.0};            ///< D5 per-chord standardized flag threshold |v_i|/√Cov(v)_ii; a flagged chord is re-tapped fresh. INERT unless @c innov_gate.
        bool   innov_calibrate{false};  ///< D5 v2: standardize by the LS-VCE-estimated EMPIRICAL innovation variance σ̂₀² (robust geo-mean of the standardized innovations), not the raw formal Cov(v) — makes @c innov_k a true Nσ gate (the formal Cov(v) is 10-30× optimistic). OFF = raw formal (v1). INERT unless @c innov_gate.
        double innov_calibrate_window{60.0};  ///< D5 v2: exponential-forgetting half-life (epochs) of the σ̂₀² accumulator. INERT unless @c innov_calibrate.
        // --- M5 card WL-3: lane-coordinate carry (docs/SPEC_lane_carry_card.md §3.1). ---
        // All three are INERT unless the caller feeds PAIR groups (@ref GroupInput::pair).
        double q_wl_scale{1.0};         ///< WL-3 §3.1: q along the WL lane = this × @c qz (lane basis, per pair chord).  1 = isotropic (== today's per-band q; the §6.1 equivalence shape).  INERT unless @c lane_joint.
        double sigma_wl_fresh_cyc{0.0}; ///< WL-3 §3.1: fresh pair-chord lane prior σ along the WL direction (WL cycles — converted to lane-coordinate state units via the group's (λ₁,λ₂)).  0 = isotropic diffuse @c fresh_prior_var (the §6.1 equivalence shape).  INERT unless @c lane_joint.
        bool   lane_joint{false};       ///< WL-3 master shape switch for pair groups: true = the LANE shape (ONE joint 2K-row DdPriorBlock per component, cross-band posterior coupling retained, lane prior/q armed, a D5 exemption spans both band rows).  false = the §6.1 EQUIVALENCE shape (per pair component TWO K-row per-band blocks whose fold is information-identical to today's two per-band groups; cross-band coupling zeroed at install) — the structural safety net for the regroup.
        // --- LF-1 INC-1: blocked/sparse lane fold (docs/SPEC_lane_fold_perf_card.md §3.2). ---
        bool   fold_blocked{false};     ///< LF-1 INC-1: when true, the per-epoch lane fold is applied in the OPTIMIZED shape — @ref applyLaneFix uses the O(K) sparse lane-coordinate SET (the dense rank-2 touches only rows/cols {k,K+k}) instead of the dense O(K²) form, and the per-row @f$0.5(P+P^\top)@f$ re-symmetrization is HOISTED out of every primitive to ONE @ref finalizeLaneBatch pass per block per epoch.  This changes the OPERATION ORDER (Woodbury/reassociation), so it is the @c parallel_carry EQUIVALENCE class, NOT bit-identical to the sequential fold (@c fold_blocked=false) — its ON runs carry the pcarry usage rule until the §5.3 physically-null gate passes.  INERT unless @c lane_joint (lane rows only exist on the joint shape).
        // --- LF-1 INC-2: pinned-cycle increment skip (docs/SPEC_lane_fold_perf_card.md §3.3). ---
        bool   skip_pinned_increments{false}; ///< LF-1 INC-2: skip the per-epoch MW/bridge lane increments for a chord whose FFR fix is COMMITTED (in @c lane_fix_w_) and was re-decided within @c skip_pinned_within_n epochs — a σ≈0.06-0.2 cyc increment folded into a lane already pinned at σ=0.01 moves nothing.  Any funnel event (slip / D5 flag / re-tap / rebirth) erases the fix memory ⇒ increments resume the SAME epoch (invariant #1).  Reads COMMITTED state only (never the per-build staged decision), so the RAIM re-solve is deterministic.  Equivalence class (the increment is near-inert but not exactly null); INERT unless @c lane_joint.  Query: @ref laneIncrementSkippable.
        long long skip_pinned_within_n{3};    ///< LF-1 INC-2: the grace window (epochs).  A committed-decided chord keeps skipping increments for this many epochs after its LAST re-decision; if it goes undecided longer (a genuinely drifting lane the FFR gate stopped trusting), increments resume to re-converge it.  INERT unless @c skip_pinned_increments.
    };

    /// One present, resolvable ambiguity arc this epoch (the caller has already
    /// excluded GLONASS and any arc it cannot resolve to a live column).
    struct ArcInput {
        int  rx_vertex{0};       ///< stable receiver vertex id (disjoint low range).
        int  sat_vertex{0};      ///< stable satellite vertex id (disjoint high range; > every rx id).
        bool tree_edge{false};   ///< election spanning-tree edge (gauge pin) vs chord (a carried DD).
        int  est_station{-1};    ///< estimator station index of this arc's ambiguity (−1 = unresolvable this epoch).
        int  local_col{-1};      ///< A_r local ambiguity column of this arc (−1 = unresolvable).
        bool slipped{false};     ///< Card 4 C3c: a cycle slip / refloat invalidated this arc's carried ambiguity this epoch (its pre-slip DD is a stiff WRONG prior). The caller sets it from the network's authoritative slip funnel (MW wide-lane, LLI, IF-phase TD, RAIM culprit). Excluded from the A_old membership so any new cycle whose support includes it is re-tapped FRESH (data-only), exactly the birth path; a carried chord it refloats is counted in @ref Counters::slip_kills.  WL-3 §3.4: for a PAIR arc the caller sets the UNION of both bands' slip states — a slip on either band refloats the chord's both bands (they share the graph edge).
        int  local_col2{-1};     ///< WL-3 (pair groups only): the band-2 ambiguity's A_r local column at the SAME (rx, sat).  −1 in a pair group marks the arc unresolvable (defensive; the caller routes band-1-only arcs to the residual per-band group instead).  Ignored for per-band groups.
        bool slipped2{false};    ///< WL-3 (pair groups only): the BAND-2 ambiguity's own slip state (@c slipped holds band-1's).  Under the LANE shape (@ref Config::lane_joint) the driver unions them — a slip on either band refloats the chord's both bands (§3.4); under the §6.1 EQUIVALENCE shape each band refloats on its OWN slips only, exactly like the two per-band groups it must reproduce.  Ignored for per-band groups.
    };

    /// One (const,signal) group's arcs this epoch.  Groups are independent z-store
    /// partitions; a DD never crosses a group.  @c key is a caller-stable id for the
    /// group (e.g. a hash of constellation+signal) used only to bucket the carry.
    ///
    /// WL-3 (SPEC_lane_carry_card.md §2): a PAIR group replaces the two per-band
    /// groups of a constellation's primary signal pair.  Its arcs are the (rx, sat)
    /// simple graph BOTH bands share (band-1 column in @ref ArcInput::local_col,
    /// band-2 in @ref ArcInput::local_col2); each graph chord then carries the
    /// 2-vector (z₁, z₂) of the two bands' cycle DDs with a joint per-component
    /// covariance.  Storage stays per-band (the reconcile projection W applies per
    /// band unchanged); only the process model / fresh prior / lane measurement
    /// rows use the (NL, WL) lane basis, and only under @ref Config::lane_joint.
    struct GroupInput {
        int key{0};
        bool pair{false};        ///< WL-3: this is a primary-pair group (arcs carry @c local_col2).
        double lambda1{0.0};     ///< WL-3 pair groups: band-1 wavelength (m) — constant per group (CDMA; GLONASS never enters).
        double lambda2{0.0};     ///< WL-3 pair groups: band-2 wavelength (m).
        std::vector<ArcInput> arcs;
    };

    /// Lifecycle tallies for the run-end line (spec D5).
    struct Counters {
        long long epochs{0};              ///< @ref buildDdPriors invocations.
        long long components{0};          ///< DD-prior blocks emitted (components with ≥1 foldable chord), summed over epochs.
        long long chords_carried{0};      ///< chords whose DD was carried from the previous epoch (basis-projected).
        long long chords_born{0};         ///< fresh chords (diffuse prior → data-only tap-off).
        long long chords_marginalized{0}; ///< old carried chords not carried forward (dead arc).
        long long chords_dropped{0};      ///< chords skipped this epoch (an unresolvable cycle arc — a data-gap refloat, distinct from a slip).
        long long slip_kills{0};          ///< Card 4 C3c: chords that would have been carried but were refloated FRESH because a cycle slip / RAIM culprit invalidated an arc of their fundamental cycle (≈ the slip rate the carry saw).
        long long innov_flags{0};         ///< Card 4 D5: carried chords whose post-solve standardized innovation exceeded @c innov_k (a fine slip the coarse layer missed).
        long long innov_retaps{0};        ///< Card 4 D5: flagged chords actually re-tapped fresh at install (== @c innov_flags in v1; a distinct counter for the v2 refold route).
        long long merges{0};              ///< components formed from ≥2 previous components.
        long long splits{0};              ///< previous components spread across ≥2 new components.
        long long tree_swaps{0};          ///< components whose carry was re-expressed in a genuinely different fundamental-cycle basis (a projection W that is not a signed permutation — the analogue of the phase-B unimodular pivot).
        long long z_dim{0};               ///< total carried DD dimension at the end of the last epoch.
        // --- INC-C0 census (perf-restructure reconciliation, log-only) --------------
        long long max_component_kc{0};    ///< INC-C0: the largest single-component carried DD dimension K_c ever emitted (the dense-Pᶻ cost driver — the K in the O(K³) fold/read-back).
        long long max_z_dim{0};           ///< INC-C0: peak total carried DD dimension over the run (growth-bound: monotone-unbounded growth ⇒ the kill path is not firing — §0 case (b)).
        long long chords_marg_perchord{0};///< INC-C0: old chords dropped from a SURVIVING component (its projection-W column is all-zero — no new fundamental cycle references it).  This is the PER-CHORD marginalization @ref chords_marginalized (whole-block only) misses; the two together are the true kill rate.  Near-zero while @ref max_z_dim grows ⇒ dead arcs are being retained (§0 case (b)); large ⇒ the kill path is exact and simply has churn to kill (§0 case (a)).
        long long max_cycle_len{0};       ///< INC-C0 (D-c viability): the longest fundamental cycle (tree-path length + 1) folded this run.  The DD information matrix J = Pᶻ⁻¹ is sparse iff cycles are short (adjacent chords share few tree edges) ⇒ the exact info-form carry (SPEC_carry_factored_pz_C2 D-c) is BOUNDED; long cycles ⇒ dense J ⇒ D-c gives no bound (the choice the perf card cannot make without this number).
        long long sum_cycle_len{0};       ///< INC-C0: Σ fundamental-cycle lengths over all folded chords this run (mean = sum / (chords_carried + chords_born)).
        // --- WL-3 lane-coordinate carry (per-band-DD counting keeps the §6.1 gate strict:
        //     a pair chord tallies 2 in the chords_* / cycle-length counters, exactly what
        //     the two per-band groups would have tallied; `components` counts EMITTED
        //     blocks, so it halves under the joint lane shape — structural, documented). ---
        long long pair_components{0};     ///< WL-3: pair components reconciled (summed over epochs; each is 1 joint or 2 split emitted blocks).
        long long pair_tree_promotions{0};///< WL-3: chords promoted to tree edges to COMPLETE a pair component's spanning forest (the caller's band-1 election tags don't span the pair intersection subgraph there — fragmented-primary regions; 0 ⇒ the eq34 basis was kept exactly everywhere).
        // --- LF-1 INC-0: the emitted-component fold-dimension distribution (SPEC §7.4 — the
        //     INC-4 Amdahl question: one giant G component that dominates the parallel ceiling,
        //     vs a spread the component-parallel fold can balance).  Bucketed once per emitted
        //     component by fold-dim d = tally·K (2K for a joint pair block, matching
        //     @ref max_component_kc).  Always-on like the census counters (never read by the
        //     solve ⇒ byte-identical); the run-end line reads it only when the timers are armed. ---
        long long comp_kc_hist[10]{};     ///< #emitted components with fold-dim d in log2 buckets: [0]=0, [1]=1, [2]=2..3, [3]=4..7, [4]=8..15, [5]=16..31, [6]=32..63, [7]=64..127, [8]=128..255, [9]=≥256.
    };

    /// INC-C0 per-op wall-clock accumulators (ms), armed by @ref setPerfTiming
    /// (@c network.trace_section_timers).  Splits the driver's own cost — which the D0b
    /// SB_DDBUILD / SB_INSTALL wall brackets lump — into the primitive ops so the
    /// perf-restructure card ranks them (dense Pᶻ basis projection vs posterior read-back
    /// vs the D5 gate).  Whole-run (NOT zeroed by @ref reset, mirroring
    /// @c NetworkNormalEquations::SectionTimers); byte-identical when off (no clock reads,
    /// the accumulators touch nothing the solution depends on).  The serial install path
    /// is the only one timed (the parallel install accumulates from many threads and would
    /// race a single double — and @c parallel_carry already routes install serially).
    struct PerfTimers {
        double time_update_ms{0.0};   ///< @ref beginEpoch: the q_z·Δt diagonal inflate of the carried Pᶻ (factored path: +B·Δt on the A blocks).
        double basis_project_ms{0.0}; ///< @ref buildDdPriors: the change-of-basis projection W construction + W·Pᶻ_old·Wᵀ + W·z_old per emitted component (the dense-K² materialization INC-C2 factors).
        double post_mean_ms{0.0};     ///< @ref installPosterior: the @c ddPriorPosteriorMean read-back (K-vector shrink).
        double post_cov_ms{0.0};      ///< @ref installPosterior: the @c ddPriorPosteriorCovariance read-back (the dense K×K posterior INstall — the other dense-K² materialization).
        double d5_gate_ms{0.0};       ///< @ref installPosterior: the D5 @c ddPriorInnovation + the per-chord standardized gate (0 unless @c Config::innov_gate).
        // --- LF-1 INC-0: the WL-3 lane-fold split the SB_DDBUILD wall lumps.  These are the
        //     ~10³ per-epoch rank-1/rank-2 covariance touches the perf card exists to shrink —
        //     splitting them from basis_project_ms tells the later increments which lever pays. ---
        double lane_meas_ms{0.0};     ///< @ref applyLaneMeasurement: cumulative wall of the per-epoch MW/bridge lane increment folds (dense rank-1 downdate, O(K²)/row — the irreducible measurement work).
        double lane_fix_ms{0.0};      ///< @ref applyLaneFix: cumulative wall of the idempotent lane-coordinate SET on decided cycles (the ~65%-decided re-pins; INC-1 makes this O(K²)→O(K)).
        long long lane_meas_calls{0}; ///< #@ref applyLaneMeasurement rows that reached the fold (matches wl_run.lane_mw_rows + lane_bridge_rows on the accepted build).
        long long lane_fix_calls{0};  ///< #@ref applyLaneFix rows that reached the SET (rc∈{0,1}; matches lane_fix_folded + lane_fix_unchanged).
        long long build_epochs{0};    ///< #@ref buildDdPriors calls (counted at entry — a chordless epoch that never reaches the timed projection still counts; for per-epoch averages).
        long long install_epochs{0};  ///< #serial @ref installPosterior calls timed.
    };

    explicit NetworkInvariantCarryDriver(Config cfg = {}) : cfg_(cfg) {
        // D5 v2: one pooled "dd" accumulator (no per-constellation split) whose robust
        // geo-mean floor of the STANDARDIZED innovations is σ̂₀² (they carry formal
        // variance 1, so the floor IS the empirical inflation).  Seed floor 1.0 = the H0
        // expectation.  Only allocated when calibration is armed.
        if (cfg_.innov_calibrate)
            inflation_.emplace(std::vector<std::string>{"dd"}, cfg_.innov_calibrate_window,
                               /*split_by_const=*/false, /*seed_floor_m2=*/1.0);
    }

    /// Drop all carry (a toggle-flip cold start; the DDs re-tap-off next epoch).
    void reset();

    /**
     * @brief Time update: inflate the carried @f$P^z@f$ by @f$q_z\,\Delta t@f$ on
     *        the diagonal (default @f$q_z=0@f$ ⇒ no-op — DDs are constant between
     *        slips).  Call once at the top of the epoch, before @ref buildDdPriors.
     * @param dt_s Elapsed time since the previous epoch (s).
     * @param content_s Epoch content time (continuous GPS s) — D5 v2 σ̂₀² reporting bucket
     *        (ignored unless @c Config::innov_calibrate).
     * @param clean False marks a contaminated epoch whose innovations are excluded from the
     *        σ̂₀² accumulator (D5 v2; ignored unless @c innov_calibrate).
     */
    void beginEpoch(double dt_s, double content_s = 0.0, bool clean = true);

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
     * @param n_threads Optional deterministic fan-out (@c network.parallel_seam;
     *        default 1 = serial, byte-identical).  With @p n_threads > 1 the per-block
     *        posterior mean/covariance + innovation (all const kernel readouts) are
     *        computed into per-block slots in parallel, while the order-sensitive σ̂₀
     *        accumulator feed, gate decision, flag/counter appends and the carry install
     *        stay SERIAL in the exact (block, chord) order — so σ̂₀ (which gates the next
     *        epoch), the counters, and @c carried_ are BIT-IDENTICAL at any thread count.
     */
    void installPosterior(const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                          const NetworkEstimator::Datum& datum, int n_threads = 1);

    const Counters& counters() const { return counters_; }

    /// INC-C0: arm/disarm the per-op timers (@c network.trace_section_timers).  Set once
    /// before the epoch loop (survives @ref reset, like the kernel's @c setPerfTiming).
    /// Default off ⇒ no clock reads, byte-identical.
    void setPerfTiming(bool on) { perf_timing_ = on; }
    /// @return Whether the per-op timers are armed (@ref setPerfTiming).
    bool perfTiming() const { return perf_timing_; }
    /// @return The per-op wall-clock accumulators (cumulative since construction / the last
    ///         @ref resetPerfTimers; NOT zeroed by @ref reset).  Meaningful only when armed.
    const PerfTimers& perfTimers() const { return perf_timers_; }
    /// Zero the per-op timer accumulators.
    void resetPerfTimers() { perf_timers_ = PerfTimers{}; }

    /// Card 4 D5: the (group, chord arc) pairs the innovation gate flagged as slipped at
    /// the last @ref installPosterior — the caller maps each back to its (station, ambiguity)
    /// and feeds it into the NEXT epoch's slip funnel (the C3c membership-exclusion re-tap).
    /// Empty unless @c Config::innov_gate; refreshed every @ref installPosterior.
    const std::vector<std::pair<int, ArcKey>>& lastInnovFlags() const { return last_innov_flags_; }

    /// D5 diagnostic (WL-2 F-A attribution): the FORMAL-standardized innovation
    /// |v_i|/√Cov(v)_ii of each flag in @ref lastInnovFlags, index-aligned (compare
    /// against k·σ̂₀ for the gate margin).  Refreshed with the flags every
    /// @ref installPosterior.
    const std::vector<double>& lastInnovFlagSigmas() const { return last_innov_sigmas_; }

    /// D5 v2: the current empirical innovation-inflation σ̂₀ (= √σ̂₀², the factor the
    /// standardized innovation is divided by), or 1.0 when calibration is off / still in
    /// warm-up.  The run-end telemetry (how far the formal Cov(v) was off).
    double innovInflationSigma0() const;

    /// Total carried DD dimension right now (across all components).
    Eigen::Index totalZDim() const;
    /// Number of DD-prior blocks emitted by the last @ref buildDdPriors.
    std::size_t numBlocks() const { return block_meta_.size(); }

    /**
     * @brief M5 card WL-2 (SPEC_ewl_wl_cascade_card.md §3.3): edit a carried DD VALUE —
     *        the WL inject primitive's low-level half.
     *
     * Sets the carried mean of @p chord in @p group to @p new_z (the CORRECTION-frame DD,
     * state units = λ·N; the caller computes it from the lane target minus @f$\sum c\,x_{lin}@f$)
     * and reseeds its variance to @p new_var (row/col cross-terms zeroed to stay PSD — the
     * injected value is a fresh, independent assertion), then marks the chord D5-EXEMPT for the
     * NEXT @ref installPosterior (one epoch): the inject deliberately moves the value by up to
     * half the garbage lane gap, so the per-epoch code-limited innovation would otherwise trip
     * the D5 gate and re-tap it away (the F6 storm this card exists to avoid).  A subsequent
     * epoch's data agrees (the hold row pins the lane) so no exemption is needed then.
     *
     * The caller pairs the two bands' chords (same @c ArcKey, different group) and injects
     * both; a chord that is not currently carried in @p group (the cycle is not 1:1 across the
     * two groups' bases) returns false and the caller skips + counts it.
     *
     * @return true if @p chord was found + edited; false if it is not a carried chord of @p group.
     */
    bool injectCarriedChord(int group, const ArcKey& chord, double new_z, double new_var);

    /// The carried DD mean+variance of @p chord in @p group (false if not carried) — the read
    /// half of the WL inject (compute the current lane value before deciding to inject).  Same
    /// as @ref carriedDdForTest but named for production use.  On a WL-3 PAIR block this
    /// returns the BAND-1 half (use @ref carriedPairValue for both bands).
    bool carriedChordValue(int group, const ArcKey& chord, double& z, double& var) const {
        return carriedDdForTest(group, chord, z, var);
    }

    // ---- M5 card WL-3: lane-coordinate carry (docs/SPEC_lane_carry_card.md) ---------

    /**
     * @brief WL-3 §3.2: fold ONE per-epoch WL-lane measurement into an emitted PAIR
     *        block's prior — the closed-form KF update that makes the carry itself the
     *        code-coherence accumulator (each epoch's MW / bridge increment enters ONCE).
     *
     * The measurement is the WL lane functional of chord @p k in CORRECTION-frame WL
     * cycles: @f$y = z_1/\lambda_1 - z_2/\lambda_2 + e,\ e\sim N(0,R)@f$ — the caller
     * forms @f$y@f$ from this epoch's (lagged) MW(1,2) or iono-de-biased bridge cycle DD
     * minus the linearization lane @f$\sum(c/\lambda_1)x_{lin,1}-(c/\lambda_2)x_{lin,2}@f$.
     * Updates BOTH the caller's block (what @c setDdPriors folds) and the stashed
     * @c BlockMeta prior (what the D5 gate standardizes against), so the innovation frame
     * stays consistent.  Only valid on a JOINT pair block (@ref Config::lane_joint); call
     * serially in (block, chord) order — the deterministic seam (§8.2).
     *
     * @param blocks The vector returned by the LAST @ref buildDdPriors (pre-@c setDdPriors).
     * @param b      Emitted block index (must be a joint pair block).
     * @param k      Chord index within the block (row @c k = band-1, row @c K+k = band-2).
     * @param y_cyc  Correction-frame lane measurement (WL cycles).
     * @param r_cyc2 Measurement variance (WL cycles²), > 0 (caller floors it).
     * @return true if folded; false if (b, k) is not a joint pair chord or inputs are degenerate.
     */
    bool applyLaneMeasurement(std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
                              std::size_t b, std::size_t k, double y_cyc, double r_cyc2);

    /**
     * @brief WL-3 §3.3: the FFR integer fix — an IDEMPOTENT lane-coordinate SET on the
     *        emitted pair block, applied every DECIDED epoch.
     *
     * Re-centers chord @p k's WL lane COORDINATE on the correction-frame integer target
     * @p y_cyc and re-collapses its variance to @p sigma_cyc², zeroing the coordinate's
     * cross-covariances (a fresh independent assertion, PSD-safe — the WL-2 inject
     * semantics realized on the lane coordinate, with the NL direction preserved by
     * construction).  A measurement FOLD here would accumulate per-epoch decisions that
     * are not independent information (the F-A once-only lesson); the set is
     * information-idempotent, and re-applying it on an UNCHANGED decision is what keeps
     * the lane pinned against q_wl re-loosening + the noisy MW increments (measured:
     * fold-on-change alone let decided lanes drift back to MW quality).  The decision
     * value is STAGED per build and committed at @ref installPosterior (a within-epoch
     * RAIM re-solve re-derives identical sets); the memory is erased when the chord
     * re-taps (slip / D5 flag / rebirth) — the re-entered loose lane re-earns its fix
     * (§3.4 self-heal).
     *
     * @return +1 = the integer moved / first assertion, 0 = re-pinned (decision
     *         unchanged), −1 = not applicable (not a joint pair chord / degenerate).
     */
    int applyLaneFix(std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
                     std::size_t b, std::size_t k, long long w, double y_cyc, double sigma_cyc);

    /**
     * @brief LF-1 INC-1 (SPEC_lane_fold_perf_card.md §3.2): re-symmetrize every joint pair
     *        block ONCE, after all of this epoch's lane rows have been applied.
     *
     * A no-op unless @ref Config::fold_blocked.  Under @c fold_blocked the per-row
     * @f$0.5(P+P^\top)@f$ re-symmetrization is skipped inside @ref applyLaneMeasurement /
     * @ref applyLaneFix (it was a full O(K²) pass + a 2K×2K temp on each of the ~10³ rows/epoch,
     * and the per-row sparse/dense updates hold symmetry only to ULP); this single pass restores
     * the exact symmetry the sequential path maintained per-row — hoisting ~10³ symmetrizations
     * down to one per component.  Because the intra-epoch folds then run on a Pᶻ carrying up to
     * ~row-count ULPs of asymmetry (vs the per-row-symmetrized sequential Pᶻ), the result is the
     * pcarry EQUIVALENCE class (≤1e-12 vs sequential, NOT bit-identical).  Call after the MW /
     * bridge / fix walks and before @c NetworkEstimator::setDdPriors.  Idempotent across a RAIM
     * re-solve (the blocks are rebuilt each build).
     *
     * @param blocks The vector returned by the LAST @ref buildDdPriors (pre-@c setDdPriors).
     */
    void finalizeLaneBatch(std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) const;

    /**
     * @brief LF-1 INC-2 (SPEC_lane_fold_perf_card.md §3.3): may the caller SKIP this epoch's
     *        MW/bridge lane increment for chord @p k of emitted block @p b?
     *
     * True iff @ref Config::skip_pinned_increments AND @p b is a joint pair block AND the chord's
     * FFR fix is COMMITTED (present in @c lane_fix_w_ as of the last @ref installPosterior) AND was
     * re-decided within @c Config::skip_pinned_within_n epochs.  A pinned lane sits at σ = 0.01 cyc;
     * an increment at σ ≈ 0.06-0.2 folded into it is information-free, so skipping it saves the
     * O(K²) downdate without moving the decision.  Reads ONLY committed state (never the per-build
     * @c lane_fix_staged_), so every RAIM re-solve of the epoch skips the SAME rows — deterministic.
     * Any funnel event (slip / D5 flag / re-tap / rebirth) erases the fix memory, so a disturbed
     * chord returns false and resumes increments the same epoch (invariant #1).  Always false when
     * @c skip_pinned_increments is off (⇒ byte-identical).
     *
     * @param b Emitted block index (from the last @ref buildDdPriors).
     * @param k Chord (graph-chord) index within the block.
     */
    bool laneIncrementSkippable(std::size_t b, std::size_t k) const;

    /// WL-3: the carried PAIR chord's two band values+variances (false if @p group is not
    /// a pair group carrying @p chord).  The read half of the pair inject fallback.
    bool carriedPairValue(int group, const ArcKey& chord, double& z1, double& var1,
                          double& z2, double& var2) const;

    /**
     * @brief WL-3: the pair-block analogue of @ref injectCarriedChord — edit BOTH band
     *        values of a carried pair chord (fallback path; should fire ~never in steady
     *        state, telemetry proves it).  Cross-terms of both rows are zeroed (PSD-safe,
     *        fresh independent assertion) and the chord is D5-exempt for ONE install.
     * @return true if @p chord is a carried pair chord of @p group and was edited.
     */
    bool injectCarriedChordPair(int group, const ArcKey& chord,
                                double new_z1, double new_var1,
                                double new_z2, double new_var2);

    /// WL-3: the pair-emission shape of emitted block @p b — 0 = plain per-band group
    /// block, 1/2 = band-1/band-2 half of a split (equivalence-shape) pair emission,
    /// 3 = joint 2K lane-shape pair block.  Lane measurements apply only to shape 3.
    int blockPairBand(std::size_t b) const { return block_meta_.at(b).pair_band; }

    // --- test seams (numeric leaf; the NetworkZStore::setEstimate precedent) -----

    /// Directly install one component's carry — the gate's seed for a known
    /// @f$\hat z,P^z@f$ over a chord basis (bypasses a solve).
    void seedCarriedForTest(int group, const std::vector<ArcKey>& chords,
                            const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz,
                            const std::set<int>& vertices, const std::set<ArcKey>& arcs);
    /// WL-3: seed one PAIR component's carry (@p z is 2K = [band-1; band-2], @p Pz 2K×2K).
    void seedCarriedPairForTest(int group, const std::vector<ArcKey>& chords,
                                const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz,
                                double lambda1, double lambda2,
                                const std::set<int>& vertices, const std::set<ArcKey>& arcs);
    /// WL-3: dump the carried PAIR block of @p group (false if none) — chords + the full
    /// joint [band-1; band-2] mean/covariance, for the lane-battery unit gates.
    bool carriedPairBlockForTest(int group, std::vector<ArcKey>& chords,
                                 Eigen::VectorXd& z, Eigen::MatrixXd& Pz) const;
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
    /// WL-3: a PAIR component stores BOTH bands jointly — @c z is 2K ([band-1; band-2]),
    /// @c Pz 2K×2K (cross-band blocks zero unless @c Config::lane_joint retained them).
    struct CarriedBlock {
        int group{0};
        std::vector<ArcKey> chords;   ///< z index → chord arc (the DD basis; K graph chords).
        Eigen::VectorXd z;            ///< carried DD means (K, or 2K for a pair block).
        Eigen::MatrixXd Pz;           ///< carried DD covariance (dense, per component).
        std::set<int> vertices;       ///< the component's vertices.
        std::set<ArcKey> arcs;        ///< every arc present (tree ∪ chords) — A_old for the carried test.
        bool pair{false};             ///< WL-3: joint two-band pair component.
        double lambda1{0.0};          ///< WL-3 pair: band-1 wavelength (m) — the lane basis.
        double lambda2{0.0};          ///< WL-3 pair: band-2 wavelength (m).
    };
    /// Per-emitted-block metadata (parallel to the returned blocks) for the install.
    struct BlockMeta {
        int group{0};
        std::vector<ArcKey> chords;   ///< fold-row order = the new carried block's chord order.
        std::set<int> vertices;       ///< the component's vertices this epoch.
        std::set<ArcKey> arcs;        ///< A_new for this component (becomes next epoch's A_old).
        Eigen::VectorXd  zhat;        ///< D5: the block's prior means (== fold zhat) so the innovation
                                      ///< gate can install a flagged chord's data-only mean d = v + ẑ.
                                      ///< WL-3: 2K for a joint pair block; K otherwise.
        std::vector<char> chord_carried; ///< D5 v2: 1 = chord carried a tight prior this epoch, 0 = born
                                      ///< fresh (diffuse).  Only carried chords feed the σ̂₀² accumulator
                                      ///< (a born chord's diffuse Cov(v) would corrupt the inflation).
                                      ///< WL-3: per GRAPH chord (K) in every shape — membership is band-uniform.
        int pair_band{0};             ///< WL-3 emission shape: 0 = per-band group block; 1/2 = band-1/band-2
                                      ///< half of a SPLIT pair emission (§6.1 equivalence shape; siblings are
                                      ///< adjacent, band-2 at b+1); 3 = joint 2K lane-shape block.
        double lambda1{0.0};          ///< WL-3 pair: band wavelengths (lane functional coefficients).
        double lambda2{0.0};
    };

    Config cfg_;
    std::vector<CarriedBlock> carried_;      ///< the carry as of the previous @ref installPosterior.
    std::vector<BlockMeta>    block_meta_;   ///< layout of the blocks returned by the last @ref buildDdPriors.
    std::vector<std::pair<int, ArcKey>> last_innov_flags_;  ///< D5: (group, chord) flagged at the last install.
    std::vector<double> last_innov_sigmas_;  ///< D5 diagnostic: |v|/√Cov(v) per flag, aligned with last_innov_flags_.
    std::set<std::pair<int, ArcKey>> gate_exempt_;  ///< WL-2 §3.3: (group, chord) injected this epoch — D5-exempt at the NEXT @ref installPosterior (consumed + cleared there).  WL-3: a pair chord's exemption spans BOTH band rows.
    std::map<std::pair<int, ArcKey>, long long> lane_fix_w_;      ///< WL-3 §3.3: the last COMMITTED FFR integer per (pair group, chord) — the fold-on-change memory.  Committed at @ref installPosterior; erased when the chord re-taps (slip / D5 flag / rebirth).
    std::map<std::pair<int, ArcKey>, long long> lane_fix_staged_; ///< WL-3: this build's @ref applyLaneFix decisions, staged so a within-epoch RAIM re-solve re-derives identical folds; merged into @ref lane_fix_w_ at install, cleared at each @ref buildDdPriors.
    std::map<std::pair<int, ArcKey>, long long> lane_fix_epoch_;  ///< LF-1 INC-2: the @c install_epoch_index_ at which each @c lane_fix_w_ key was LAST re-decided.  Committed/pruned/erased in lockstep with @c lane_fix_w_; drives the @ref laneIncrementSkippable grace window.  Empty unless @c Config::skip_pinned_increments.
    long long install_epoch_index_{0};  ///< LF-1 INC-2: monotone count of accepted @ref installPosterior calls (the epoch clock for the skip grace window; survives RAIM re-solves — only the accepted install advances it).
    std::optional<NetworkCarryVce> inflation_;  ///< D5 v2: the σ̂₀² accumulator (only when Config::innov_calibrate).
    Counters counters_;
    bool      perf_timing_{false};   ///< INC-C0: per-op timers armed (survives reset()).
    PerfTimers perf_timers_;         ///< INC-C0: per-op wall-clock accumulators (whole-run).

    /// D5 v2: the "dd" family/sat key (pooled — split_by_const=false, sat ignored).
    static constexpr const char* kInflationFamily = "dd";
    static constexpr const char* kInflationSat    = "x";   ///< non-empty (const-split off ⇒ ignored).
    /// σ̂₀² is trusted only after this many accumulated (forgetting-weighted) rows; below it
    /// the gate is the raw formal test (= v1), so v2 starts as v1 and eases into calibration.
    static constexpr double kInflationWarmupRows  = 200.0;
    /// σ̂₀² ∈ [1, cap]: calibration only RAISES the threshold (never over-sensitizes below
    /// formal); the cap keeps a diverged σ̂₀ from silently disabling the gate forever.
    static constexpr double kInflationFactorCap   = 1.0e6;
};

}  // namespace GnssProcessor
