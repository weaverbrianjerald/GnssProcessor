#pragma once
/**
 * @file InvariantCarryDriver.hpp
 * @brief Per-epoch driver that time-links the DD cycle-basis carry (`NetworkZStore`)
 *        through the network producer solve (card 4, M1, phase C3b).
 *
 * The driver is the factored, unit-drivable core of the "invariant-function carry"
 * (design handoff `docs/SPEC_card4_invariant_carry.md`).  Phase B built the z-store
 * data structure, the fundamental-cycle algebra and the lifecycle transforms
 * (`ZStore.hpp`, gates 1–3); phase C built the producer-side fold
 * (`NetworkNormalEquations::foldReducedDdPriors` with `ddPriorPosteriorMean` /
 * `ddPriorPosteriorCovariance`, gate 4 DatumFreedom).  This driver runs between the
 * two each epoch:
 *
 *   1. It receives the eq34 election's spanning-tree/chord partition of this epoch's
 *      (const,signal) observation graph: the @ref GroupInput list, one resolvable arc
 *      per present ambiguity, each tagged tree edge or chord and resolved to its
 *      estimator (station, local ambiguity column).
 *   2. It reconciles the carried DD estimate to this epoch's basis with one
 *      change-of-basis projection @f$W@f$ per connected component, which subsumes the
 *      phase-B lifecycle events (tree pivot = unimodular @f$T@f$, chord death =
 *      marginalization, chord birth = tap-off, component merge/split = block
 *      re-partition), and emits one @ref NetworkNormalEquations::DdPriorBlock per
 *      component for the producer to fold (@ref buildDdPriors).
 *   3. After the solve it installs the posterior (mean and covariance, the closed form
 *      added in phase C) back into the carry (@ref installPosterior).
 *
 * ## The reconcile is a projection, validated against the phase-B lifecycle
 *
 * In phase B, carrying @f$\hat z@f$ across a basis change is a sequence of unimodular
 * pivots.  This driver computes the composed transform directly.  A new chord
 * @f$c@f$'s fundamental cycle (with respect to this epoch's tree) lies in the previous
 * epoch's cycle space iff every arc of the cycle was present last epoch; its carried
 * mean is then @f$\hat z^{\text{new}}_c=\sum_j w_j\,\hat
 * z^{\text{old}}_j@f$ with @f$w_j=\chi_c(\text{arc of old chord }j)@f$, which needs the
 * old chord arcs only, not the old tree.  Assembling @f$W@f$ over the chords of the
 * contributing old components gives @f$\hat z^{\text{prior}}=W\hat
 * z^{\text{old}}@f$ and @f$P^{z,\text{prior}}=W P^{z,\text{old}} W^\top@f$:
 *
 *   - a pure tree change (same arcs, no slip): every new chord is carried and @f$W@f$ is
 *     the composed unimodular pivot (basis change, exact, integer);
 *   - a dead chord's arc vanishes: its column of @f$W@f$ is zero and it is marginalized
 *     (= @ref ZComponent::killChord, the covariance sub-block);
 *   - a chord whose cycle uses a newly born arc is fresh: it gets a diffuse prior (no
 *     information added to the solve; its posterior is the data-only tap-off, GKF
 *     Eq. 22);
 *   - a component merge gathers the independent blocks of both sides block-diagonally,
 *     and a split re-partitions one old block across the new components; both follow
 *     from the vertex-overlap contributor gather with no special case.
 *
 * The gate `tests/network/test_network_invariant_carry_driver.cpp` cross-checks this projection
 * against a hand-sequenced `NetworkZStore` lifecycle (the validated phase-B oracle) and
 * against from-scratch cycle-matrix truth, and runs the production-seam
 * **DatumFreedom** falsifier (two different per-epoch tree realizations of the same
 * graph ⇒ identical estimable output).
 *
 * ## Scope and conventions
 *
 * Pure numeric leaf that depends only on `NetworkArcGraph` (the cycle algebra: the
 * union-find, the tree, the fundamental cycles and the W rows; it took
 * them over from `NetworkZStore`) and the
 * `NetworkEstimator` kernel (the fold and the posterior readout).  It does not depend on
 * `ApplicationNetwork`: the caller (the live epoch loop or a unit test) resolves each
 * arc to a stable (rx_vertex, sat_vertex) pair (disjoint id ranges, receiver <
 * satellite, so the `ArcKey` orientation is receiver→satellite) and to the current
 * epoch's estimator (station, local_col).  GLONASS (FDMA) has no common-wavelength
 * integer DD, so the caller must exclude it (it never enters the CDMA (const,signal)
 * groups).  With the carry off the output is byte-identical by construction: the driver
 * is constructed and called only under `network.carry_invariant`.
 *
 * @note v1 simplifications, left for the phase-D follow-up: a carried chord whose
 * fundamental cycle has an unresolvable arc this epoch (a dropped station or a missing
 * local column) is dropped from the carry and re-tapped off when it returns, rather than
 * kept as a phase-B "prediction-only" chord.  Fresh chords use a diffuse (finite) prior
 * rather than the exact zero-information tap-off, so their per-epoch basis covariance
 * holds only to @f$O(B/\text{fresh\_prior\_var})@f$ (immaterial for the field grade; the
 * DatumFreedom gate compares carried chords, which are basis-exact).
 */

#include "network/ar/ArcGraph.hpp"        // ArcKey + the cycle algebra
#include "network/solve/Estimator.hpp"       // DdPriorBlock, Pin/Datum, posterior readout
#include "network/carry/CarryVce.hpp"        // LS-VCE robust inflation of Cov(v) (card 4, D5 v2)

#include <Eigen/Dense>

#include <iosfwd>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
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
        double fresh_prior_var{1.0e6};  ///< diffuse prior variance for a fresh chord (state units²); large, so the prior carries almost no information and the chord's posterior is the data-only tap-off.
        bool   innov_gate{false};       ///< enable the estimation-side fine-slip innovation test on the DD chords (@ref installPosterior).  Off: the posterior is installed unconditionally, byte-identical to C3c (card 4, D5).
        double innov_k{4.0};            ///< per-chord flag threshold: a chord is flagged when |v_i|/√Cov(v)_ii > innov_k·σ̂₀ (σ̂₀ = 1 unless @c innov_calibrate), and a flagged chord is re-tapped fresh.  Inert unless @c innov_gate (D5).
        bool   innov_calibrate{false};  ///< scale the test by the LS-VCE estimate of the empirical innovation variance σ̂₀² (robust geometric mean of the standardized innovations) instead of the raw formal Cov(v), which makes @c innov_k an Nσ threshold (the formal Cov(v) measured 10-30× optimistic).  Off: the raw formal test (v1).  Inert unless @c innov_gate (D5 v2).
        double innov_calibrate_window{60.0};  ///< exponential-forgetting half-life (epochs) of the σ̂₀² accumulator.  Inert unless @c innov_calibrate (D5 v2).
        /**
         * @brief Require raw-observable corroboration before a flagged chord is re-tapped
         *        (D5 v3).
         *
         * Measured: 2.1 % of D5 resets coincide with a raw MW/GF step, against 95 % on the
         * same run's gap-instrumentation arc resumptions (known real discontinuities), and 97.4 % of
         * inter-reset gaps are one epoch.  The gate therefore fires on a persistent
         * model-versus-data disagreement rather than on slips, and re-taps continuous arcs
         * indefinitely.  With this armed, a flagged chord is re-tapped unless the raw
         * observables positively show that its arc was continuous.
         *
         * Suppression requires positive evidence of continuity.  The raw-step tag is
         * tri-state, and only the measured-and-flat state suppresses:
         *   - stepped            ⇒ re-tap (a real discontinuity)
         *   - measured and flat  ⇒ suppress (the arc did not step; the innovation is model error)
         *   - no evidence at all ⇒ re-tap (arc start, gap, single band, unresolved key)
         * Missing evidence never suppresses, so the slip response required by standing
         * invariant #1 is narrowed, never removed: the coarse layer (MW/LLI/IF-TD) is
         * untouched and D5 still fires on every corroborated chord.
         *
         * Fed per epoch through @ref setInnovFlatChords.  Inert unless @c innov_gate, and
         * inert (empty flat set, nothing suppressed) unless a slip-QC consumer computed the
         * raw observables that epoch.  Off: byte-identical to D5 v2.
         */
        bool   innov_corroborate{false};
        /**
         * @brief Optional magnitude backstop for @c innov_corroborate, in multiples of the gate
         *        threshold: re-tap regardless of corroboration when |v_i|/√Cov(v)_ii exceeds
         *        this × (@c innov_k · σ̂₀).  0 (default) = inert, pure corroboration.
         *
         * Off by default because corroboration does not improve with innovation size: it
         * measured 2.1 % below 25σ, 0.0 % at 100-200σ and 1.2 % above 200σ, so a magnitude
         * backstop re-admits the population with the least evidence of a real slip.  The
         * knob exists for sweeps, not as a recommended setting.
         */
        double innov_corroborate_backstop_k{0.0};
        /**
         * @brief Gate only chords whose prior carries information (D5 v5).
         *
         * The gate tests @f$|v_i| > k\hat\sigma_0\sqrt{Cov(v)_{ii}}@f$, where @f$Cov(v)@f$
         * is the H0 covariance of the innovation about the carried prior.  A chord whose
         * prior is the diffuse @c fresh_prior_var has no H0 to test, and its "reset" is a
         * no-op (a diffuse chord's posterior already is the data-only tap-off); the only
         * lasting effect of the flag is the funnel entry that makes the chord born fresh
         * again next epoch, where it is flagged again.  Measured (63 stations, 3 h): 95.3 %
         * of all D5 flags sat on diffuse-prior chords at @f$\sqrt{var}=1000@f$ m, and the
         * carry never accumulated.
         *
         * A row is skipped when either condition holds (the first alone was measured
         * insufficient):
         *  - topologically born: the driver's @c chord_carried mask is 0 (the same mask that
         *    keeps born chords out of the σ̂₀ accumulator, v2);
         *  - informationally diffuse: @f$Cov(v)_{ii}\ge\tfrac12@f$ @c fresh_prior_var.
         * @c chord_carried is topological (cycle ⊆ A_old, no slip) and says nothing about
         * information: a chord carried from a predecessor that was itself diffuse inherits a
         * diffuse projected prior and still passes it.  Keying on the mask alone left 80.4 %
         * of the surviving flags at √Cov(v) = 1000 m.
         *
         * Off: byte-identical.  Inert unless @c innov_gate.
         */
        bool   innov_carried_only{false};
        // --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md §5). ---
        /**
         * @brief PD-safe carry policy (@c network.carry_invariant_pd_safe), driver half.
         *
         * The kernel half (@ref NetworkNormalEquations::setDdFoldPdSafe) makes the fold's
         * arithmetic scale-following and repairs round-off.  This half owns the two edges
         * that standing invariant #7 requires when a block still cannot factor:
         *  - Re-coupling edge (INC-2a): @ref installPosterior rebirths a degenerate block.
         *    The means @f$\hat z@f$ are kept as the seed and @f$P^z@f$ is reset to the fresh
         *    diffuse diagonal, so next epoch's diffuse prior takes the data-only tap-off.
         *    This loses one bounded epoch of information, the same semantics as a chord
         *    birth, instead of decoupling the block permanently without a signal.
         *  - Restore guard (INC-2c): @ref restoreCarry screens each parsed block and
         *    rebirths a non-PD one.  Snapshots written before the PD-safe card can hold a
         *    corrupt (non-PD) block, which a warm resume would otherwise re-import into a
         *    build that has the fix.
         *
         * A rebirth is a prior reset, not a slip: the slip funnel and the D5 flag list are
         * not touched.  Off: byte-identical.
         */
        bool   pd_safe{false};
        // --- Linearization-frame carry (docs/SPEC_event_harness.md §21.4; RS-2). ---
        /**
         * @brief Capture each chord's linearization DD @f$z_\ell=\sum c\,x_{\rm lin}@f$
         *        (@c network.state_snapshot_carry_lin), the write half.
         *
         * The carried @f$\hat z@f$ is the correction-frame DD, so it has no meaning apart
         * from the linearization it was harvested against.  When armed, @ref buildDdPriors
         * computes @f$z_\ell@f$ per emitted chord from @ref ArcInput::x_lin over the same
         * fundamental cycle that builds the fold rows, @ref installPosterior stores it in
         * @ref CarriedBlock::zl, and @ref serializeCarry writes one @c ZL row per block.
         * Nothing in the solve reads it, so turning it on is byte-identical on every
         * product and trace; only the snapshot grows.
         */
        bool   carry_lin{false};
        /**
         * @brief Run the D5 gate diagnostic-only (@c network.resume_d5_warmup_ep, RS-2b).
         *
         * Set per epoch by the install seam from the post-resume countdown, not fixed at
         * construction, because the window is a property of the seam rather than of the
         * run.  While true, a chord that trips k·σ̂₀ (and passes the v3 corroboration gate)
         * is recorded as a flag and counted in @ref Counters::innov_flags and
         * @ref Counters::d5_warmup_suppressed, but it is not re-tapped: its posterior mean
         * and its @f$P^z@f$ row and column stay as installed,
         * and @ref lastInnovWarmupSuppressed marks it so the caller keeps it out of the next
         * epoch's slip funnel.  The σ̂₀ accumulator feed comes before the decision and is
         * unaffected (the D5 v2 contract: rows above the threshold keep feeding calibration
         * whether or not the re-tap happens).
         *
         * Purpose: a re-tap discards the carried value the gate is testing.  Holding the
         * branch through the window shows whether the data disagrees with the branch or
         * with the cold station layer.
         */
        bool   innov_diagnostic_only{false};
    };

    /// One present, resolvable ambiguity arc this epoch (the caller has already
    /// excluded GLONASS and any arc it cannot resolve to a live column).
    struct ArcInput {
        int  rx_vertex{0};       ///< stable receiver vertex id (disjoint low range).
        int  sat_vertex{0};      ///< stable satellite vertex id (disjoint high range; > every rx id).
        bool tree_edge{false};   ///< election spanning-tree edge (gauge pin) vs chord (a carried DD).
        int  est_station{-1};    ///< estimator station index of this arc's ambiguity (−1 = unresolvable this epoch).
        int  local_col{-1};      ///< A_r local ambiguity column of this arc (−1 = unresolvable).
        bool slipped{false};     ///< a cycle slip or refloat invalidated this arc's carried ambiguity this epoch (its pre-slip DD would be a stiff wrong prior).  The caller sets it from the network's slip funnel (MW wide-lane, LLI, IF-phase TD, RAIM culprit).  The arc is treated as absent from A_old, so any new cycle whose support includes it is re-tapped fresh (data-only), the birth path; a carried chord lost this way is counted in @ref Counters::slip_kills (card 4, C3c).
        double x_lin{0.0};       ///< this arc's ambiguity linearization value (m), the value the kernel takes its corrections about.  Read only under @ref Config::carry_lin; otherwise left 0 and unused, so the off path costs nothing (RS-2, docs/SPEC_event_harness.md §21.4).
    };

    /// One (const,signal) group's arcs this epoch.  Groups are independent z-store
    /// partitions; a DD never crosses a group.  @c key is a caller-stable id for the
    /// group (e.g. a hash of constellation+signal) used only to bucket the carry.
    struct GroupInput {
        int key{0};
        std::vector<ArcInput> arcs;
    };

    /// Lifecycle tallies for the run-end line (spec D5).  Tallies made in
    /// @ref buildDdPriors count every build, so a within-epoch RAIM re-solve counts them
    /// twice; tallies made in @ref installPosterior count once per accepted epoch.
    struct Counters {
        long long epochs{0};              ///< @ref buildDdPriors calls (a RAIM re-solve rebuild counts again).
        long long components{0};          ///< DD-prior blocks emitted (components with ≥1 foldable chord), summed over builds.
        long long chords_carried{0};      ///< chords whose DD was carried from the previous epoch (basis-projected).
        long long chords_born{0};         ///< fresh chords (diffuse prior → data-only tap-off).
        long long chords_marginalized{0}; ///< chords of previous blocks that fed no component this epoch (whole-block marginalization, e.g. every arc died); per-chord drops are in @ref chords_marg_perchord.
        long long chords_dropped{0};      ///< chords skipped this epoch because a cycle arc was unresolvable (a data-gap refloat, distinct from a slip).
        long long slip_kills{0};          ///< chords that would have been carried but were refloated fresh because a cycle slip or RAIM culprit invalidated an arc of their fundamental cycle (about the slip rate the carry saw) (card 4, C3c).
        long long innov_flags{0};         ///< chord rows flagged by the D5 gate: post-solve standardized innovation above @c innov_k·σ̂₀ and not skipped or suppressed (a fine slip the coarse layer missed).  Born chords are tested too unless @c innov_carried_only; flags inside the post-resume warm-up window are included (card 4, D5).
        long long innov_retaps{0};        ///< flagged chord rows re-tapped fresh at install: @c innov_flags minus @ref d5_warmup_suppressed (card 4, D5).
        long long innov_suppressed{0};    ///< chord rows that tripped the gate but were not re-tapped because the raw observables showed the arc continuous (@c innov_corroborate).  This counter is the only evidence that the corroboration gate acts (invariant #4): 0 with the knob armed means the flat set never reached it, usually because no slip-QC decision card ran the qualify pass that fills the verdicts (SlipQcState::cardsArmed(); network.trace_slip_raw alone computes the observables but adjudicates none, so every verdict map is empty) (D5 v3).
        long long innov_born_skipped{0};  ///< chord rows the gate did not evaluate under @c innov_carried_only because the chord was born this epoch or its Cov(v)_ii is at least half of @c fresh_prior_var.  Counted before the threshold test, so it is the whole skipped population, not only the part that would have flagged: the measure of how much of the gate had no hypothesis to test (D5 v5).
        long long merges{0};              ///< components formed from ≥2 previous components.
        long long splits{0};              ///< previous components spread across ≥2 new components.
        long long tree_swaps{0};          ///< components whose carry was re-expressed in a different fundamental-cycle basis: some carried chord's projection row mixes two or more old chords, so W is not a signed permutation (the analogue of the phase-B unimodular pivot).
        long long z_dim{0};               ///< total carried DD dimension at the end of the last epoch.
        // --- INC-C0 census (perf-restructure reconciliation, log-only) --------------
        long long max_component_kc{0};    ///< largest single-component carried DD dimension K_c emitted, the dense-Pᶻ cost driver: the K in the O(K³) fold and read-back (INC-C0).
        long long max_z_dim{0};           ///< peak total carried DD dimension over the run.  Growth bound: unbounded monotone growth means the kill path is not firing (§0 case (b)) (INC-C0).
        long long chords_marg_perchord{0};///< old chords dropped from a surviving component (their projection-W column is all zero: no new fundamental cycle references them).  This is the per-chord marginalization that @ref chords_marginalized (whole blocks only) does not count; the two together are the kill rate.  Near zero while @ref max_z_dim grows means dead arcs are retained (§0 case (b)); large means the kill path is exact and has churn to kill (§0 case (a)) (INC-C0).
        long long max_cycle_len{0};       ///< longest fundamental cycle (tree-path length + 1) folded this run; a G-side screen only for D-c viability.  Short cycles (adjacent chords share few tree edges) keep the cycle matrix G sparse, which is necessary but not sufficient for the exact info-form carry (SPEC_carry_factored_pz_C2 D-c) to be bounded: the border term is Gᵀ Σ_ss G, so the binding probe is the sparsity and rank of Σ_ss, which this counter does not measure.  §6.1 of that doc corrects the earlier claim that J = Pᶻ⁻¹ is sparse iff cycles are short (the probe read short and sparse at both tiers, yet D-c remains unconfirmed because global co-visibility makes a dense Σ_ss the likely case).  Long cycles still imply a dense J, so a dense-G reading is decisive against D-c while a sparse-G reading is not decisive for it (INC-C0).
        long long sum_cycle_len{0};       ///< Σ fundamental-cycle lengths over all folded chords this run (mean = sum / (chords_carried + chords_born)) (INC-C0).
        // --- Post-resume D5 warm-up (RS-2b): a run total. ---
        long long d5_warmup_suppressed{0};      ///< gate trips counted but not acted on because the post-resume warm-up window (@c network.resume_d5_warmup_ep) was open: the chord's posterior was installed as usual (no re-tap), and the caller keeps its arc out of the slip funnel.  0 with the knob armed means the window closed before any chord tripped, or nothing was restored to trip (invariant #4) (RS-2b).
        // --- Fold-dimension distribution of the emitted components (SPEC §7.4; LF-1
        //     INC-0).  It answers the INC-4 Amdahl question: one giant G component that sets
        //     the parallel ceiling, or a spread the component-parallel fold can balance.
        //     Bucketed once per component that emits blocks, by fold dimension d = K
        //     (matching @ref max_component_kc).  Always on, like the
        //     census counters (the solve never reads it, so output is byte-identical); the
        //     run-end line reads it only when the timers are armed. ---
        long long comp_kc_hist[10]{};     ///< #emitted components with fold-dim d in log2 buckets: [0]=0, [1]=1, [2]=2..3, [3]=4..7, [4]=8..15, [5]=16..31, [6]=32..63, [7]=64..127, [8]=128..255, [9]=≥256.
        // --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md) ---------------
        /**
         * @brief Chord rows the D5 gate could not test because @f$\mathrm{Cov}(v)_{ii}@f$
         *        was not strictly positive: the @c if(!(var>0.0)) skip in both gate bodies
         *        (INC-0).
         *
         * The skip itself leaves no trace, and it is where a degenerate block drops out of
         * view: with @f$M=0@f$ the fold caches the raw non-PD @f$P^z+B@f$ as @c Minv, so
         * @f$\mathrm{Cov}(v)=M^{-1}-G^\top\Sigma G@f$ inherits its negative diagonals and
         * every affected row is skipped.  Counted always (invariant #4); non-zero with
         * @c innov_gate armed means part of the carry was not gated, whatever the flag
         * totals say.
         */
        long long d5_nonpos_var_rows{0};
        /// Blocks rebirthed at install because their fold stayed degenerate after the
        /// PD-safe repair (means kept, @f$P^z@f$ reset to the fresh diffuse diagonal).
        /// 0 unless @c Config::pd_safe.  This is the re-coupling edge of invariant #7; the
        /// decoupling edge is the fold's own degenerate counter and the seam's WARN (INC-2a).
        long long ddfold_rebirths{0};
        /// Chord rows in those rebirthed blocks, i.e. the carried dimension re-seeded (INC-2a).
        long long ddfold_rebirth_rows{0};
        // --- σ̂₀ clamp visibility (D5 v2; see InflationState) ---------------------------
        /**
         * @brief @ref installPosterior calls, each of which reads the σ̂₀ state: the
         *        denominator for the five state tallies below.
         *
         * Tallied always (an instrument, like the INC-0 counters), whether or not
         * @c innov_gate is armed, so a run with @c innov_calibrate off reports
         * @c d5_calib_off_epochs equal to this and the reader can tell "v1 by config" from
         * "v2 that never engaged".  Counted once per install, which the epoch loop makes
         * once per accepted epoch; a within-epoch RAIM re-solve does not add a count (the
         * build-time tallies such as @c chords_born count every build).
         */
        long long d5_calib_epochs{0};
        long long d5_calib_off_epochs{0};     ///< of those, @ref InflationState::Off (calibration not armed).
        long long d5_calib_warmup_epochs{0};  ///< of those, @ref InflationState::WarmUp: armed, gate running v1.
        long long d5_calib_floored_epochs{0}; ///< of those, @ref InflationState::Floored: armed and warm, but the gate still runs v1, and σ̂₀ alone does not show it.
        long long d5_calib_capped_epochs{0};  ///< of those, @ref InflationState::Capped: σ̂₀² pinned at the cap.
        long long d5_calib_active_epochs{0};  ///< of those, @ref InflationState::Active: calibrated.
        /// Smallest raw (unclamped) σ̂₀² estimated past warm-up.  A value below 1 with a
        /// reported σ̂₀ of 1.00 shows the floor clamp was in effect.
        double d5_calib_raw_min{std::numeric_limits<double>::infinity()};
        /// Largest raw (unclamped) σ̂₀² estimated past warm-up.
        double d5_calib_raw_max{0.0};
    };

    /// Per-op wall-clock accumulators (ms), armed by @ref setPerfTiming
    /// (@c network.trace_section_timers) (INC-C0).  They split the driver's own cost,
    /// which the D0b SB_DDBUILD / SB_INSTALL wall brackets lump together, into the
    /// primitive ops so the perf-restructure card can rank them (dense Pᶻ basis
    /// projection, posterior read-back, D5 gate).  Whole-run: not zeroed by @ref reset,
    /// as with @c NetworkNormalEquations::SectionTimers.  Byte-identical when off (no clock
    /// reads; the accumulators touch nothing the solution depends on).  Only the serial
    /// install path is timed: the parallel install would accumulate into one double from
    /// many threads, and @c parallel_carry already routes the install through the serial
    /// path.
    struct PerfTimers {
        double time_update_ms{0.0};   ///< @ref beginEpoch: the q_z·Δt inflate of the carried Pᶻ (diagonal).
        double basis_project_ms{0.0}; ///< @ref buildDdPriors: construction of the change-of-basis projection W plus W·Pᶻ_old·Wᵀ and W·z_old per emitted component (the dense K² materialization that INC-C2 targets).
        double post_mean_ms{0.0};     ///< @ref installPosterior: the @c ddPriorPosteriorMean read-back (K-vector shrink).
        double post_cov_ms{0.0};      ///< @ref installPosterior: the @c ddPriorPosteriorCovariance read-back (the dense K×K posterior install, the other dense K² materialization).
        double d5_gate_ms{0.0};       ///< @ref installPosterior: the D5 @c ddPriorInnovation plus the per-chord standardized gate (0 unless @c Config::innov_gate).
        long long build_epochs{0};    ///< #@ref buildDdPriors calls, counted at entry, so a chordless epoch that never reaches the timed projection still counts (for per-epoch averages).
        long long install_epochs{0};  ///< #serial @ref installPosterior calls timed.
    };

    explicit NetworkInvariantCarryDriver(Config cfg = {}) : cfg_(cfg) {
        // One pooled "dd" accumulator (no per-constellation split) whose robust
        // geometric-mean floor of the standardized innovations is σ̂₀²: the innovations
        // carry formal variance 1, so the floor is the empirical inflation.  Seed floor
        // 1.0 = the H0 expectation.  Allocated only when calibration is armed (D5 v2).
        if (cfg_.innov_calibrate)
            inflation_.emplace(std::vector<std::string>{"dd"}, cfg_.innov_calibrate_window,
                               /*split_by_const=*/false, /*seed_floor_m2=*/1.0);
    }

    /**
     * @brief Drop all carry (a toggle-flip cold start; the DDs re-tap off next epoch).
     *
     * The @ref Counters are a run instrument and survive this call; only @c z_dim is
     * zeroed, because it is a live dimension rather than a tally and the carry it measured
     * is gone.  reset() is reachable mid-run from the install seam's exception handler
     * (@c NetworkInstallCarry, around the @c installPosterior call).  Zeroing the tallies
     * there would make the run-end line under-report every total and drive the seam's
     * per-epoch churn deltas (differences of these cumulative totals) negative
     * (invariant #4).
     *
     * @ref restoreCarry is the exception: installing a foreign carry starts the instrument
     * fresh (RE-4), so it zeroes the counters itself after calling this.
     */
    void reset();

    /**
     * @brief Serialize and restore the carried z-store for a branch-faithful warm resume
     *        (RTB-7): the @c carried_ blocks, which hold the whole DD-ambiguity branch
     *        (means, dense @f$P^z@f$, tree/chord/vertex structure per component).
     *
     * @ref serializeCarry writes a self-delimited text blob from a @c "CARRY,v2,<n>"
     * header (@c "CARRY,v3,<n>" with @p diagonal_only) to @c "CEND".  Each component is a
     * @c B header, a @c Z mean row, an optional @c ZL row (@ref CarriedBlock::zl), one
     * @c P covariance row per carried DD (one @c PD row in v3), and @c H chord, @c X vertex
     * and @c A arc index rows; the format mirrors the emitter blob that the snapshot
     * already stores verbatim.  @ref restoreCarry consumes it and replaces the current
     * carry, round-tripping every field exactly (fmt shortest form ⇄ strtod is lossless).
     * The caller must also restore the ArcKey vertex numbering the blob references
     * (@c net::SatelliteState::carry_vertex and @c carry_group_id); otherwise the reconstructed
     * chords point at the wrong satellites or groups next epoch.  Those maps are ctx
     * state, not driver state.
     *
     * Blob versions: @c v1 = carry blocks only.  @c v2 (RTB-7(d) RE-1) adds one
     * @c "S,<sigma0_sq>,<eff_rows>" line with the D5 σ̂₀² inflation state, written only
     * when calibration is armed and the accumulator holds a positive estimate.  @c v3
     * (RTB-7(e)) stores each block's @f$P^z@f$ as its diagonal only (@p diagonal_only).
     * @ref restoreCarry accepts all three: a v1 blob (or a v2 blob with no @c S line)
     * leaves the accumulator at its seed, the legacy behaviour, so old snapshots stay
     * readable.
     *
     * ## v3: the diagonal-only carry blob (@p diagonal_only)
     *
     * The dense per-component @f$P^z@f$ is @f$\sum_c K_c^2@f$ doubles against @f$\sum_c
     * K_c@f$ means, and it dominates the snapshot: measured at 305 stations, 13,086,007
     * covariance numbers against 7,869 means, 99% of a 258 MB file, with 48 KB for all
     * other layers combined.  Storing only the diagonal cuts the covariance numbers by a
     * factor of about 1663 (258 MB → ~160 KB).  This rests on the hypothesis that the
     * resumed branch (the integer/lane realization the phase side lands on) lives in the
     * means, and that the cross-covariance is re-earned from data within an epoch or two
     * (the C1 dense shared border already resumes diagonally with no measured harm).  An
     * A/B tests that hypothesis.
     *
     * The restored @f$P^z=\operatorname{diag}(\cdot)@f$ is PD when the stored variances are
     * positive (with @ref Config::pd_safe the restore guard rebirths a block that fails the
     * PD screen), but it is not a uniform inflation.  Dropping a positive correlation
     * tightens the common-mode direction (for
     * @f$P=\begin{psmallmatrix}1&\rho\\\rho&1\end{psmallmatrix}@f$ the @f$(1,1)@f$
     * direction goes @f$1+\rho \to 1@f$) and loosens the coordinate directions relative to
     * the true information.  A diagonal resume is therefore anti-conservative along the
     * tightly coupled chord combinations that share tree edges, which is what the A/B has
     * to price.
     *
     * The write side follows the knob (@c network.state_snapshot_carry_diagonal); the read
     * side follows the content.  A block stores either @c zdim @c "P" rows (v1/v2, full)
     * or one @c "PD" row (v3, diagonal), and @ref restoreCarry dispatches on the row tag,
     * not on the header version.  Either blob therefore restores under either knob
     * setting, so the two arms of the A/B differ only in the bytes they were seeded from.
     * For the same reason the knob is not part of the warm-resume config fingerprint: it
     * is a serialization representation, not a datum-relevant rule.
     *
     * @ref restoreCarry begins with @ref reset (RE-4) and then zeroes the counters: it is a
     * general "install this carry" API, so it must not leave stale innovation side-state
     * from earlier driver work.  A block whose header carries a
     * non-zero pair flag (a lane pair block of the retired WL-3 lane carry) is refused.
     *
     * Not serialized (they re-warm): the run-end counters.
     * The per-station filter layer is excluded by the RTB-7 design (restoring it would be
     * full process checkpointing).
     *
     * @param os            Destination stream.
     * @param diagonal_only v3: write each block's @f$P^z@f$ diagonal (one @c PD row)
     *        instead of its @c zdim dense @c P rows.  Default false writes the v2 blob,
     *        byte-identical to the format before RTB-7(e).
     */
    void serializeCarry(std::ostream& os, bool diagonal_only = false) const;
    bool restoreCarry(std::istream& is, std::string* err = nullptr);

    /**
     * @brief Check that every restored chord and arc binds to a live vertex and group
     *        before the carry is trusted (RTB-7(d) RE-4).
     *
     * A carry blob is numbered in the writing run's id space: receiver vertices are
     * station roster indices, satellite vertices come from that run's
     * @c carry_sat_vertex map, and blocks are keyed by its @c carry_group_id map.  A blob
     * paired with maps it was not written with re-binds chords to the wrong satellites
     * without any error, which is worse than a cold start and does not show in the
     * products until the branch is already corrupted.  A failed check lets the caller take
     * the same logged degrade path as a parse failure.
     *
     * @param n_stations   roster size; an endpoint in [0, n_stations) binds as a receiver.
     * @param sat_vertices the live @c carry_sat_vertex values.
     * @param group_ids    the live @c carry_group_id values.
     * @param err          set to the first offending binding when the check fails.
     * @return true when every block's group is live and every chord and arc endpoint is a
     *         roster index or a live satellite vertex.
     */
    bool validateCarryBinding(int n_stations, const std::set<int>& sat_vertices,
                              const std::set<int>& group_ids, std::string* err) const;

    // --- Resume telemetry (RTB-7(d) RE-4).  Inert wiring is visible only through ---
    // --- counters (invariant #4); the run-end line reports these verbatim. ----------
    bool   resumeCarryRestored() const { return resume_restored_; }        ///< a carry blob was installed this run.
    long long resumeCarryChords() const { return resume_chords_; }         ///< carried DD count at restore (0 if none).
    bool   resumeSigma0Seeded()  const { return resume_sigma0_seeded_; }   ///< the σ̂₀² accumulator was seeded from the blob's `S,` line (v2 or later).
    double resumeSigma0Value()   const { return resume_sigma0_value_; }    ///< the seeded σ̂₀ (1.0 when not seeded).
    /// Blocks restored from a diagonal-only @f$P^z@f$ (@c PD row, v3): their chord
    /// cross-covariances were not in the blob and start at zero.  0 on a v1/v2
    /// (full-@f$P^z@f$) blob.  Without this count a diagonal resume would not be visible:
    /// the run looks warm and the counters look normal while the branch geometry is gone
    /// (invariant #4; RTB-7(e)).
    long long resumeCarryDiagonalBlocks() const { return resume_carry_diag_blocks_; }
    /// True when any restored block came from a diagonal-only blob (RTB-7(e)).
    bool   resumeCarryDiagonal() const { return resume_carry_diag_blocks_ > 0; }
    /// Blocks rebirthed at @ref restoreCarry because the blob's @f$P^z@f$ failed the
    /// equilibrated PD screen: the means were kept and the covariance reset to the fresh
    /// diffuse diagonal.  0 unless @c Config::pd_safe.  Snapshots written before the
    /// PD-safe card can hold a corrupt block, so a non-zero count on the first
    /// resume from one is expected; a non-zero count on a resume from a pd_safe-armed
    /// writer indicates a new defect (PD-safe INC-2c).
    long long resumeCarryRebirthBlocks() const { return resume_carry_rebirth_blocks_; }
    /// Blocks restored with a @c ZL row: the writer captured its linearization DD.  0 on a
    /// blob whose writer computed no linearization DDs (@c network.state_snapshot_carry_lin
    /// off, or written before the row existed) (RS-2).
    long long resumeCarryLinBlocks() const { return resume_carry_lin_blocks_; }

    /**
     * @brief Why the σ̂₀² accumulator was or was not seeded from the resumed blob
     *        (RTB-7(d) RE-1).
     *
     * Same problem as @ref InflationState, one layer up: @ref resumeSigma0Seeded is a bare
     * bool, and a bare @c false does not say why.  The obvious reading ("v1 blob /
     * calibration off") is wrong on a cold run with calibration armed, where both clauses
     * are false.
     *
     * Four distinct conditions suppress the seed, and each calls for a different action:
     * a cold start needs none, a legacy blob is a lineage fact, a calibration-off run is a
     * config fact, and a rejected `S,` line is a writer-side defect.  The instrument
     * reports the condition it measured, not a plausible one (standing invariant #4).
     */
    enum class ResumeSeedState {
        NotRestored,     ///< no carry blob was installed this run (cold start), so there was nothing to seed from.  @ref resumeCarryRestored is false.
        CalibrationOff,  ///< @c Config::innov_calibrate is false, so no σ̂₀² accumulator exists and any `S,` line in the blob was not consulted.  The gate is raw-formal by config, not pending a warm-up count.
        NoSeedLine,      ///< a blob was restored but carried no `S,` line: a v1/legacy blob, or a v2/v3 blob whose writer had calibration off or no positive σ̂₀² estimate yet.
        Rejected,        ///< the blob's `S,` line parsed but failed the @c seed_sigma0_sq>0 && @c seed_rows>0 guard in @ref restoreCarry: a writer-side defect, not a legacy blob.
        Seeded,          ///< the accumulator was seeded; @ref resumeSigma0Value is the seeded σ̂₀.
    };
    /// The seed outcome plus the numbers behind it (@ref resumeSigma0Status) (RTB-7(d) RE-1).
    struct ResumeSeedStatus {
        ResumeSeedState state{ResumeSeedState::NotRestored};
        double sigma0{1.0};          ///< the σ̂₀ actually seeded, identical to @ref resumeSigma0Value (1.0 unless @c Seeded).
        double blob_sigma0_sq{0.0};  ///< the σ̂₀² the restored blob's `S,` line carried (0 when it carried none).
        double blob_rows{0.0};       ///< the n_eff that `S,` line carried (0 when it carried none).
    };
    /// @ref resumeSigma0Seeded with its provenance (RTB-7(d) RE-1).  Report this rather than
    /// the bare bool: @c false alone cannot say which of the four suppressing conditions held.
    ResumeSeedStatus resumeSigma0Status() const;
    /// Human-readable @ref ResumeSeedState tag for logs
    /// (`cold` / `calib-off` / `no-S-line` / `rejected` / `seeded`).
    static const char* resumeSeedStateName(ResumeSeedState s);

    /**
     * @brief Time update: inflate the carried @f$P^z@f$ by @f$q_z\,\Delta t@f$ on the
     *        diagonal (default @f$q_z=0@f$: a no-op, DDs are constant between slips).
     *        Call once at the top of the epoch, before @ref buildDdPriors.
     *
     * Independently of @f$q_z@f$, the call advances the forgetting of the σ̂₀²
     * accumulator once per epoch when calibration is armed.
     * @param dt_s Elapsed time since the previous epoch (s); no inflation when ≤ 0.
     * @param content_s Epoch content time (continuous GPS s), the D5 v2 σ̂₀² reporting
     *        bucket (ignored unless @c Config::innov_calibrate).
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
     * @return One DD-prior block per emitted component; empty when nothing carries.
     */
    std::vector<NetworkNormalEquations::DdPriorBlock>
        buildDdPriors(const std::vector<GroupInput>& groups);

    /**
     * @brief Install the posterior (mean and covariance) back into the carry: the new
     *        @f$\hat z,P^z@f$ that the next epoch projects forward.  Call after
     *        @ref NetworkEstimator::solve with the same shared estimate and datum.
     *
     * @param est   The estimator that folded @ref buildDdPriors's blocks this epoch.
     * @param x_s   The solved shared estimate.
     * @param datum The minimum-constraint datum passed to @ref NetworkEstimator::solve.
     * @param n_threads Optional deterministic fan-out (@c network.parallel_seam;
     *        default 1 = serial, byte-identical).  With @p n_threads > 1 the per-block
     *        posterior mean and covariance and the innovation (all const kernel readouts)
     *        are computed into per-block slots in parallel, while the order-sensitive steps
     *        (σ̂₀ accumulator feed, gate decision, flag and counter appends, carry install)
     *        stay serial in (block, chord) order.  σ̂₀ (which gates the next epoch), the
     *        counters and @c carried_ are therefore bit-identical at any thread count.
     */
    void installPosterior(const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                          const NetworkEstimator::Datum& datum, int n_threads = 1);

    const Counters& counters() const { return counters_; }

    /// Arm or disarm the per-op timers (@c network.trace_section_timers) (INC-C0).  Set
    /// once before the epoch loop; the setting survives @ref reset, like the kernel's
    /// @c setPerfTiming.  Default off: no clock reads, byte-identical.
    void setPerfTiming(bool on) { perf_timing_ = on; }
    /// @return Whether the per-op timers are armed (@ref setPerfTiming).
    bool perfTiming() const { return perf_timing_; }
    /// @return The per-op wall-clock accumulators (cumulative since construction or the
    ///         last @ref resetPerfTimers; not zeroed by @ref reset).  Meaningful only when
    ///         armed.
    const PerfTimers& perfTimers() const { return perf_timers_; }
    /// Zero the per-op timer accumulators.
    void resetPerfTimers() { perf_timers_ = PerfTimers{}; }

    /// The (group, chord arc) pairs the innovation gate flagged as slipped at the last
    /// @ref installPosterior, one entry per flagged row.  The caller maps each back to its
    /// (station, ambiguity) and feeds it into the next epoch's slip funnel (the C3c
    /// membership-exclusion re-tap), except the entries that
    /// @ref lastInnovWarmupSuppressed marks.  Empty unless @c Config::innov_gate;
    /// refreshed every @ref installPosterior (card 4, D5).
    const std::vector<std::pair<int, ArcKey>>& lastInnovFlags() const { return last_innov_flags_; }

    /**
     * @brief The terms behind each flag in @ref lastInnovFlags, index-aligned, so the
     *        standardized innovation can be decomposed (D5 diagnostic, churn card).
     *
     * No production path reads them since the D5 flag dump that wrote them out was
     * retired with the MW/WL lane layer; the unit suite reads @c var_v to check that
     * every flag the carried-only gate lets through has an informative prior.
     *
     * The churn decomposition measured per-chord standardized innovations (`std_innov`)
     * that stayed large and nearly flat for hours (median 76σ, 98.9 % of inter-flag gaps
     * one epoch): a population that is neither slips (1.5 % raw-step coincidence) nor
     * noise.  A standing value that large says the formal @f$Cov(v)@f$ and the actual
     * epoch-to-epoch wander of the estimate disagree by orders of magnitude, but
     * `std_innov` alone cannot say which factor is wrong.  These fields can: @c z_post
     * differenced across epochs for one chord is the measured wander, and @c var_v is the
     * formal claim about it.
     *
     * Fields, all in carried-DD state units (m = λ·N) except @c var_v (state units²):
     *  - @c zhat   the carried prior mean the fold used, before this epoch's data;
     *  - @c v      the post-fit innovation @f$z_{free}-G^\top x_s-\hat z@f$ (already
     *              shrunk by the fold's border absorption, so it understates a true jump);
     *  - @c var_v  the formal @f$Cov(v)_{ii}=(M^{-1}-G^\top\Sigma G)_{ii}@f$;
     *  - @c sigma0 the σ̂₀ in force at this install (through the previous epoch, the
     *              predict-then-update contract), captured here because
     *              @ref innovInflationSigma0 includes this epoch by the time it is read;
     *  - @c z_post the value installed for the chord: @c zhat + @c v for a re-tapped flag,
     *              the ordinary posterior mean for a flag the post-resume warm-up suppressed.
     * Populated for flagged rows only (a row suppressed by corroboration or below the
     * threshold appends nothing; a warm-up flag does append), refreshed every
     * @ref installPosterior.
     */
    struct InnovTerms { double zhat{0.0}, v{0.0}, var_v{0.0}, sigma0{1.0}, z_post{0.0}; };
    const std::vector<InnovTerms>& lastInnovFlagTerms() const { return last_innov_terms_; }

    /**
     * @brief One block rebirthed at the last @ref installPosterior: the identity payload
     *        of the @c DD_FOLD_REBIRTH event the seam writes (PD-safe INC-2a).
     *
     * The fold's @f$M:=0@f$ branch decouples a block from the data, and the rebirth is the
     * re-coupling edge.  Standing invariant #7 requires both edges to be visible; without
     * them a degenerate block stays frozen with no signal.
     */
    struct FoldRebirth {
        std::size_t block{0};        ///< emitted DD-prior block index (== the fold's slot index).
        int         group{0};        ///< the (const,signal) group id.
        long long   k{0};            ///< carried state dimension re-seeded.
        double      min_pivot_rel{0.0}; ///< the fold's relative pivot for the block (≤ 0 ⇒ indefinite).
        ArcKey      chord0{};        ///< the block's first chord, a stable-enough identity for the ledger.
    };
    /// The blocks rebirthed at the last @ref installPosterior (empty unless
    /// @c Config::pd_safe, and empty on an epoch with no degenerate block).  Refreshed
    /// every install (PD-safe INC-2a).
    const std::vector<FoldRebirth>& lastFoldRebirths() const { return last_fold_rebirths_; }

    /// The current empirical innovation-inflation σ̂₀ (= √σ̂₀², the factor the standardized
    /// innovation is divided by), clamped to [1, 1000] (σ̂₀² ∈ [1, @ref kInflationFactorCap]);
    /// 1.0 when calibration is off, still in warm-up, or floored.  Run-end telemetry: how far the
    /// formal Cov(v) was off (D5 v2).
    ///
    /// @warning A returned 1.0 is ambiguous: it does not say which state produced it.
    ///          Telemetry reports @ref innovInflationStatus instead; do not publish this
    ///          value alone.
    double innovInflationSigma0() const;

    /**
     * @brief Why @ref innovInflationSigma0 returned what it returned (D5 v2).
     *
     * Four states can report σ̂₀ = 1.00, and only one of them (Active with a raw estimate
     * of 1) means the calibration ran and found the formal Cov(v) consistent.  In the other
     * three the gate runs the raw formal (v1) test.  A run with calibration armed can
     * report 1.00 this way: a frozen carry block's innovations are small against its
     * diffuse Cov(v), which drove the raw σ̂₀² to 0.0099 in one measured lineage, and the
     * floor paired with @ref kInflationFactorCap clamped it back up to 1, so the gate ran
     * v1 without the run-end line showing it (docs/SPEC_pd_safe_dd_fold.md §10, finding 2).
     *
     * Standing invariant #4: an armed knob that reports its neutral value may be inert
     * rather than benign, and the instrument has to say which.
     */
    enum class InflationState {
        Off,      ///< @c Config::innov_calibrate is false: the gate is v1 by config.
        WarmUp,   ///< armed, but n_eff < @ref kInflationWarmupRows (or no positive floor), so v1 is in effect.
        Floored,  ///< armed and warm, but the raw σ̂₀² < 1 and the clamp raised it to 1, so v1 is in effect.
        Capped,   ///< armed and warm; the raw σ̂₀² > @ref kInflationFactorCap and the clamp lowered it to the cap.
        Active,   ///< armed, warm and unclamped: the reported σ̂₀ is the estimate.  The only calibrated state.
    };
    /// The σ̂₀ readout plus the state that produced it (@ref InflationState) (D5 v2).
    struct InflationStatus {
        InflationState state{InflationState::Off};
        double raw_sigma0_sq{0.0};  ///< the unclamped σ̂₀² the accumulator holds (0 when Off or with no estimate).
        double sigma0{1.0};         ///< the σ̂₀ in force, identical to @ref innovInflationSigma0.
        double n_eff{0.0};          ///< forgetting-weighted effective row count behind the estimate.
    };
    /// @ref innovInflationSigma0 with its provenance (D5 v2).  Report this rather than the
    /// bare σ̂₀.
    InflationStatus innovInflationStatus() const;
    /// Human-readable @ref InflationState tag for logs (`off` / `warmup` / `floored` / `capped` / `active`).
    static const char* inflationStateName(InflationState s);

    /// Total carried DD dimension right now (across all components).
    Eigen::Index totalZDim() const;
    /// Number of DD-prior blocks emitted by the last @ref buildDdPriors.
    std::size_t numBlocks() const { return block_meta_.size(); }

    /**
     * @brief Set this epoch's chords whose underlying arc the raw observables measured as
     *        continuous (no MW/GF step): the suppression set for
     *        @c Config::innov_corroborate (D5 v3).
     *
     * One-epoch state, consumed and cleared by @ref installPosterior, so a stale set cannot
     * suppress a later epoch's re-tap.  Membership must
     * mean "measured and flat", never "no evidence".  The caller owns that distinction
     * (@c SlipQcEpoch::verdict[].measured && !@c SlipQcEpoch::stepped): an unmeasured arc
     * passed here would disable the slip response on it without any signal (standing
     * invariant #1).
     *
     * Leaving this unset is safe and inert: an empty set suppresses nothing, so the gate
     * falls back to D5 v2 rather than to no gate.
     */
    void setInnovFlatChords(std::set<std::pair<int, ArcKey>> flat) {
        innov_flat_ = std::move(flat);
    }

    /**
     * @brief Arm or disarm the diagnostic-only D5 gate for the next install
     *        (@ref Config::innov_diagnostic_only, RS-2b).
     *
     * Per-epoch state, set by the install seam from the post-resume countdown, for the
     * same reason @ref setInnovFlatChords is per epoch: the window belongs to the seam,
     * not to the run, and a construction-time policy could not be retired mid-run.
     * Writes the Config field so both gate bodies read one flag.  Unlike the flat set, the
     * flag is not cleared at install: it holds until the next call, and the seam sets it
     * before every install.
     */
    void setInnovDiagnosticOnly(bool on) { cfg_.innov_diagnostic_only = on; }

    /**
     * @brief Per flag, whether the post-resume warm-up suppressed the trip, index-aligned
     *        with @ref lastInnovFlags (1 = suppressed, 0 = acted on) (RS-2b).
     *
     * Consult this before folding a flag into the next epoch's slip funnel: a suppressed
     * trip is a measurement, and marking its arc slipped would refloat the chord the
     * warm-up exists to hold.  Empty when the gate is off.
     */
    const std::vector<char>& lastInnovWarmupSuppressed() const {
        return last_innov_warmup_;
    }

    /// The carried DD mean and variance of @p chord in @p group (false if not carried), read
    /// by the driver's gap-resumption classifier.  Same as @ref carriedDdForTest, named for
    /// production use.
    bool carriedChordValue(int group, const ArcKey& chord, double& z, double& var) const {
        return carriedDdForTest(group, chord, z, var);
    }

    /// The (const,signal) group id of DD-prior block @p b from the last @ref buildDdPriors.
    int blockGroup(std::size_t b) const { return block_meta_.at(b).group; }
    /// The first chord of DD-prior block @p b, a per-block identity for the PD-safe fold's
    /// degenerate/recovery ledger: block indices change as components merge and split, and
    /// the first chord is stable enough to follow a block (a large block keeps its leading
    /// chord).  A default @ref ArcKey for a chordless block.
    ArcKey blockFirstChord(std::size_t b) const {
        const auto& c = block_meta_.at(b).chords;
        return c.empty() ? ArcKey{} : c.front();
    }

    // --- test seams (numeric leaf; the NetworkZStore::setEstimate precedent) -----

    /// Directly install one component's carry: the gate's seed for a known
    /// @f$\hat z,P^z@f$ over a chord basis (bypasses a solve).
    void seedCarriedForTest(int group, const std::vector<ArcKey>& chords,
                            const Eigen::VectorXd& z, const Eigen::MatrixXd& Pz,
                            const std::set<int>& vertices, const std::set<ArcKey>& arcs);
    /// Test hook: attach a linearization DD @p zl (the value @ref serializeCarry writes as
    /// the block's @c ZL row) to the most recently seeded block of @p group.  Production
    /// never calls this; the harvest (@ref buildDdPriors → @ref installPosterior) owns @c zl
    /// end to end (RS-2).
    void seedCarriedLinForTest(int group, const Eigen::VectorXd& zl);
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
    /// Independent of any network solve: the basis-invariance probe.
    double carriedEstimableForTest(int group, const std::map<ArcKey, int>& target_cycle) const;

private:
    /// One connected component's carried DD estimate (keyed by chord arc identity).
    struct CarriedBlock {
        int group{0};
        std::vector<ArcKey> chords;   ///< z index → chord arc (the DD basis; K graph chords).
        Eigen::VectorXd z;            ///< carried DD means (K).
        Eigen::MatrixXd Pz;           ///< carried DD covariance (dense, per component).
        std::set<int> vertices;       ///< the component's vertices.
        std::set<ArcKey> arcs;        ///< every arc present (tree ∪ chords): A_old for the carried test.
        /// The linearization DD @f$z_\ell=\sum c\,x_{\rm lin}@f$ that the means in @ref z are
        /// relative to: same length and row order as @ref z, over the same fundamental cycle.
        /// Empty unless @ref Config::carry_lin was armed at the install that built the block,
        /// or the block was restored from a blob's @c ZL row; @ref serializeCarry writes the
        /// row when it is sized like @ref z (RS-2).
        Eigen::VectorXd zl;
    };
    /// Per-emitted-block metadata (parallel to the returned blocks) for the install.
    struct BlockMeta {
        int group{0};
        std::vector<ArcKey> chords;   ///< fold-row order = the new carried block's chord order.
        std::set<int> vertices;       ///< the component's vertices this epoch.
        std::set<ArcKey> arcs;        ///< A_new for this component (becomes next epoch's A_old).
        Eigen::VectorXd  zhat;        ///< the block's prior means (== fold zhat), so the innovation
                                      ///< gate can install a flagged chord's data-only mean d = v + ẑ (D5).
        std::vector<char> chord_carried; ///< 1 = the chord's prior was projected from the carry this
                                      ///< epoch (topological: cycle ⊆ A_old, no slip), 0 = born fresh
                                      ///< (diffuse).  Only carried chords feed the σ̂₀² accumulator (a
                                      ///< born chord's diffuse Cov(v) would pull σ̂₀² down).  One entry
                                      ///< per graph chord (K) (D5 v2).
        /// This epoch's linearization DD @f$z_\ell=\sum c\,x_{\rm lin}@f$ per emitted fold row
        /// (same length and row order as @ref zhat), computed over the same fundamental cycle
        /// that built the rows.  @ref installPosterior copies it into @ref CarriedBlock::zl so
        /// the stored means stay self-describing.  Empty unless @ref Config::carry_lin (RS-2).
        Eigen::VectorXd  zl;
    };

    Config cfg_;
    std::vector<CarriedBlock> carried_;      ///< the carry as of the previous @ref installPosterior.
    std::vector<BlockMeta>    block_meta_;   ///< layout of the blocks returned by the last @ref buildDdPriors.
    std::vector<std::pair<int, ArcKey>> last_innov_flags_;  ///< (group, chord) flagged at the last install (D5).
    std::vector<InnovTerms> last_innov_terms_;  ///< the (ẑ, v, Cov(v)_ii, σ̂₀, z_post) behind each flag, aligned with last_innov_flags_; see @ref lastInnovFlagTerms (D5 diagnostic).
    std::vector<char> last_innov_warmup_;       ///< 1 where the flag at the same index was suppressed by the post-resume D5 warm-up (counted, not re-tapped); see @ref lastInnovWarmupSuppressed (RS-2b).
    std::vector<FoldRebirth> last_fold_rebirths_;  ///< blocks rebirthed at the last install; see @ref lastFoldRebirths (PD-safe INC-2a).
    std::set<std::pair<int, ArcKey>> innov_flat_;   ///< (group, chord) whose arc the raw observables measured as continuous this epoch; see @ref setInnovFlatChords.  Consumed and cleared in @ref installPosterior (D5 v3).
    std::optional<NetworkCarryVce> inflation_;  ///< the σ̂₀² accumulator, present only when Config::innov_calibrate (D5 v2).
    // --- Resume telemetry, set by restoreCarry (RTB-7(d) RE-4) ---
    bool      resume_restored_{false};
    long long resume_chords_{0};
    bool      resume_sigma0_seeded_{false};
    double    resume_sigma0_value_{1.0};
    /// The restored blob carried an `S,` line at all (RE-1 provenance, @ref ResumeSeedState).
    /// Distinguishes a v1/legacy blob from one whose seed the guard rejected; the bare
    /// @c resume_sigma0_seeded_ is false in both cases.
    bool      resume_seed_line_{false};
    double    resume_seed_sq_{0.0};    ///< that line's σ̂₀² (0 when there was no line) (RE-1).
    double    resume_seed_rows_{0.0};  ///< that line's n_eff (0 when there was no line) (RE-1).
    long long resume_carry_diag_blocks_{0};   ///< blocks restored from a PD (diagonal) row (RTB-7(e)).
    long long resume_carry_rebirth_blocks_{0}; ///< blocks rebirthed at restore because the blob's Pᶻ was not PD (PD-safe INC-2c).
    long long resume_carry_lin_blocks_{0};    ///< blocks restored with a ZL (linearization-DD) row (RS-2).
    Counters counters_;
    /// The carry-state half of @ref reset (containers and epoch index), without the counter
    /// policy.  @ref reset keeps the run tallies; @ref restoreCarry drops them (RE-4).
    void resetCarryState_();
    bool      perf_timing_{false};   ///< per-op timers armed; survives reset() (INC-C0).
    PerfTimers perf_timers_;         ///< per-op wall-clock accumulators, whole-run (INC-C0).

    /// The "dd" family/sat key of the pooled accumulator (split_by_const = false, so the
    /// sat key is ignored) (D5 v2).
    static constexpr const char* kInflationFamily = "dd";
    static constexpr const char* kInflationSat    = "x";   ///< non-empty (const-split off ⇒ ignored).
    /// σ̂₀² is trusted only after this many accumulated (forgetting-weighted) rows; below it
    /// the gate is the raw formal test (= v1), so v2 starts as v1 and eases into calibration.
    static constexpr double kInflationWarmupRows  = 200.0;
    /// σ̂₀² ∈ [1, cap]: calibration only raises the threshold (never makes the gate more
    /// sensitive than the formal test), and the cap keeps a diverged σ̂₀ from disabling the
    /// gate for good.
    static constexpr double kInflationFactorCap   = 1.0e6;
};

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the invariant carry driver: the PD-safe DD fold, the card 4/M1 carry, the
 *        D5 gate and its map misses.
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in InvariantCarryDriver.cpp, print in this order, each gated on its own knob:
 *   - networkCarryDdFoldRunEnd: the PD-safe invariant-DD fold.
 *   - networkInvariantCarryRunEnd: the card 4/M1 invariant carry.
 *   - networkD5GateRunEnd: the D5 fine-slip innovation gate and its variants.
 *   - networkD5MapMissRunEnd: D5 flags missing from the carry map.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkInvariantCarryDriverRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
