#pragma once
/**
 * @file NetworkPbEmitFilter.hpp
 * @brief Emission-frame continuity filter for the network's per-(satellite,signal)
 *        phase biases: a slip-gated, per-key scalar Kalman smoother on the
 *        post-rebase emitted values plus a noise-calibrated jump gate
 *        (M5 prerequisite).
 *
 * ## Purpose
 *
 * @ref NetworkEmitRebase removes the per-epoch datum churn (the common
 * per-component fractional part and the per-satellite integer×λ), so the emitted
 * @f$b_s@f$ is frame-continuous.  It does not remove the ordinary per-epoch
 * estimation noise (cm scale) on each emitted value.  The emitter's phase-bias
 * jump test (@ref NetworkProductEmitter, a hand-set 0.05 m threshold) sits inside
 * that noise distribution: with a measured median per-epoch step of about 0.08 m,
 * about 40 % of keys raise a false discontinuity every epoch (a floor of about 200
 * discontinuities per epoch that scales with key count).  Only about 50 of 12 000
 * steps exceed 0.5 m; those are the genuine, mostly Galileo, cycle-level events
 * that the slip-aware machinery preserves.  The estimable frame is sound; the
 * emitted values are unfiltered and the alarm threshold is miscalibrated.  A
 * consumer cannot resolve integers while it is reset about 200 times per epoch, so
 * this floor blocks wire-side ambiguity resolution, and removing it is a
 * prerequisite for the network-AR milestone.
 *
 * ## Where the filter runs (quasi-static PB, emission frame only)
 *
 * True satellite phase biases are quasi-static (they drift over hours).  The raw
 * phase-bias columns in the solve are datum-dependent: the free eq34 datum
 * re-realization under the invariant DD carry re-expresses them every epoch.
 * Time-linking them in the estimator would put a stale stiff prior on a quantity
 * that jumps, which warps the whole solution past the jump (the Card-4 failure), so
 * that option is rejected.  After @ref NetworkEmitRebase the per-key emitted value
 * is continuous by construction up to noise and genuine events, so a per-key filter
 * in the emission frame is outputs-only and S-transparent: it never touches the
 * solve, and every user double difference and the producer's dd-fraction grading
 * are byte-unchanged.
 *
 * ## The slip rule
 *
 * A cycle slip is an integer jump in one (receiver,satellite) arc's ambiguity and
 * resets only that ambiguity.  The emitted PB lump steps only when the slipped arc
 * is part of the lump's datum definition (an eq34 tree or pinned arc).  That is the
 * class @ref NetworkEmitRebase is handed as @c Entry::slipped (from
 * @c carry_slipped_ambs ∩ @c pinned_amb_targets) and preserves as a genuine
 * reference-path slip step.  This filter therefore keeps smoothing through ordinary
 * slips (the PB is untouched by construction) and resets, emitting the raw value so
 * its step survives to the emitter's jump test, only on
 *   (a) a reference-path slip flag for the key,
 *   (b) an innovation beyond the calibrated gate below
 *       (@f$|z-\hat\mu| > k\,\hat\sigma_{\text{step}}@f$ in v1;
 *       @f$|z-\hat\mu| > k\,\sqrt{P_{\text{pred}}+R}@f$ in v2), or
 *   (c) an arc gap longer than @c max_gap_s (a re-appearance is a first sighting,
 *       not a continued arc).
 * Aligning each key to its last value would drive every step to zero and hide a
 * real slip, a standing invariant #1 failure.  This filter does not do that: a
 * reset passes the raw value through, so the genuine step reaches the (unchanged)
 * emitter jump test.
 *
 * ## Effect on the discontinuity floor (no emitter change)
 *
 * On a smooth epoch the filter emits the posterior mean, whose step from the
 * previous emitted value is a small fraction of the innovation (Kalman gain
 * @f$K = P/(P+R) \ll 1@f$ in steady state), i.e. millimetres, far below the
 * emitter's 0.05 m gate, so the emitter does not fire.  On a reset epoch it emits
 * the raw value, whose step is the full event magnitude, so the emitter fires.
 * The emitter's jump test stays the sole discontinuity authority, unchanged and
 * replay-reproducible, and now fires only at genuine events.  Live runs and a
 * @c NetworkProductIO replay run the emitter on the same baked (filtered) values,
 * so the discontinuity counters reproduce byte-for-byte; like the re-base, the
 * filter bakes its transform into the emitted product and is not re-run on replay.
 *
 * ## The calibrated jump gate (self-tuning, per constellation)
 *
 * @f$\hat\sigma_{\text{step}}@f$ is a robust running estimate of the per-key step
 * noise: the tail-safe geometric mean of the squared innovations with the
 * @f$\chi^2_1@f$ correction that @ref NetworkCarryVce uses
 * (σ̂² = geo_mean(ê²)/(e^{-γ}/2)).  The same @f$\hat\sigma_{\text{step}}@f$ is also
 * the measurement noise R of the filter (a self-tuning adaptive KF), so the filter
 * needs no hand-set R.  The solve's formal phase-bias σ would be a poor R: it is
 * entangled with the datum-dependent level, and the absolute emitted σ overstates
 * the consumer-relevant per-epoch magnitude (M4 contract work).  Two gate
 * generations are selected by @c Config::gate_v2:
 *
 * **v1 (default, @c gate_v2 == false).**  The reset threshold is
 * @f$k\,\hat\sigma_{\text{step}}@f$, and @f$\hat\sigma_{\text{step}}@f$ is fed
 * only from smoothed (accepted) rows; reset rows are withheld, as in the M2
 * monitor's clean gate.  That censoring is safe only when the true per-epoch step
 * is well below the gate.  When the step is at gate scale (measured at 300 s: a
 * 5–10 cm step against a k·(0.25·seed) = 4·0.0125 m = 5 cm floor), the withheld
 * rows bias @f$\hat\sigma_{\text{step}}@f$ low, the gate tightens and more rows
 * are rejected, a self-reinforcing collapse (92 % of resets were false-alarm
 * innovations in a 300 s run).  A per-cadence hand retune (@c jump_k 8.0 with seed
 * 0.10 m, the retired v1 tuning carried by one lineage; the code default of
 * @c jump_k is 4.0) does not transfer across cadences: at 30 s its floor of
 * 8·0.25·0.10 m = 20 cm straddles a single-cycle L1 step.
 *
 * **v2 (@c gate_v2 == true): data- and model-driven, rate-transferable.**  Three
 * coupled changes remove the hand retune:
 *   1. **Censoring-free feed.**  Every row with a valid prior whose reset is not
 *      structural feeds @f$\hat\sigma_{\text{step}}@f$: smoothed rows and
 *      innovation-gated rows.  kInit and kGap have no per-epoch prior, and
 *      kRefPathSlip is an exogenously flagged genuine event, so those stay out.
 *      The @f$\chi^2_1@f$ geometric mean is tail-safe at the measured contamination
 *      (<1 % genuine events at 30 s, ~0 % at 300 s), so feeding the gate-scale
 *      steps recovers the true noise floor instead of censoring it away, and the
 *      v1 collapse cannot start.
 *   2. **Model-scaled gate.**  The threshold is @f$k\,\sqrt{P_{\text{pred}}+R}@f$,
 *      the predicted innovation σ (the same @f$S@f$ the Kalman gain uses).  The
 *      epoch spacing enters through @f$P_{\text{pred}} = P +
 *      q\cdot\text{gap}@f$, so dt is in the model, as @f$q_z\,dt@f$ is inside
 *      D5's formal Cov(v) (@ref NetworkCarryVce lineage).  One @c jump_k therefore
 *      serves both 30 s and 300 s cadences, which is the purpose of v2.
 *   3. **Seed as a numerical guard.**  With all such rows feeding, structural
 *      collapse cannot occur, so @c meas_sigma_m keeps only its weak
 *      first-epochs prior (@ref kPriorRows), and the 0.25×seed floor is replaced
 *      by a small absolute guard (@ref kSigmaFloorAbsM).  @c jump_k returns to the
 *      universal 4.0, which gives a stated false-alarm rate (α = erfc(k/√2) ≈
 *      6·10⁻⁵ per key per epoch, about 0.03 alarms per epoch over about 420 keys,
 *      at either cadence).
 *
 * One σ̂ serves both rates because the step σ is dominated by solve noise, not by
 * drift accumulated over the gap, so v2a's lumped σ̂ already transfers.  Its level
 * is regime-dependent, not a constant: the design estimate is 6–8 cm, and σ̂ has
 * been measured at 8.1 cm at 300 s and 20 cm at 30 s.  The 30 s value is at
 * single-L1-cycle scale because the unconverged DD churn is present in the 30 s
 * wire and therefore, correctly, in the censoring-free feed.  The transfer does not
 * require a dt-independent σ̂: k is a true Nσ against whatever σ̂ the run measures.
 * A two-component R(dt) = σ̂²_solve + q̂_drift·dt (the M2 Floor/Rate robust fit,
 * with its condition guard for the fixed-cadence unidentifiable case) is a deferred
 * v2b refinement, not required for the transfer.
 *
 * ## Residual-risk envelope (v2 and v1 as designed)
 *
 * The miss band scales with the measured σ̂: with k=4 the gate sits at
 * @f$4\hat\sigma_{\text{step}}@f$ ≈ 25–30 cm when σ̂ is at the 6–8 cm design
 * level, but ≈ 80 cm in a churn-dominated regime such as 30 s cadence
 * (σ̂ ≈ 20 cm), and the envelope below widens in proportion.  An unflagged
 * reference-path slip whose emitted step lands between about 2σ and
 * @f$k\,\hat\sigma_{\text{step}}@f$ does not reset.  A missed single-cycle L1 step
 * (about 19 cm) is an example, and at the churn-regime σ̂ it lies well inside the
 * miss band, not near its edge.  Such a step is smoothed through, and the filter
 * converges to the new level over about 1/K epochs (K the steady-state Kalman
 * gain).  The result is a bounded, sub-gate transient that the consumer never sees
 * as a discontinuity; the slip is not hidden, because the value is not snapped back
 * and tracks to the true new level.  This risk class is intrinsic to any
 * @f$k\,\hat\sigma@f$ gate and exists identically in v1 as designed.  Upstream slip
 * coverage (the reference-path flag, which does not depend on this gate) makes the
 * miss class rare, and v2 neither adds to it nor removes it.  The envelope shrinks
 * back to the design level as DD convergence improves (the R6/M5 prerequisite),
 * since the churn that feeds σ̂ is the churn that work targets.
 *
 * ## Scope and conventions
 *
 * Pure numeric leaf; no dependency on the estimator, the emitter, or
 * ApplicationNetwork.  ApplicationNetwork constructs the engine; the caller
 * (NetworkEmitStore, inside the re-base block) supplies each re-based emitted
 * (sat,signal) value with its reference-path slip flag and the epoch's continuous
 * time.  The leaf rewrites the value in place to the filtered value and reports
 * the posterior σ (the caller may write it onto the emitted product) and the reset
 * cause (for the run-end telemetry).  GLONASS (FDMA) keys are not passed in and
 * keep their raw emitted value.  Byte-identity with the filter off is structural:
 * the engine is constructed in every run, but filterEpoch() is called only under
 * `network.emit_pb_filter` (itself gated inside the re-base block).  Tested
 * directly in @c tests/test_pb_emit_filter.cpp with synthetic streams of known
 * truth, following the standalone-leaf pattern of @ref NetworkEmitRebase and
 * @ref NetworkCarryVce.
 */

#include "common/GnssIdentity.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class NetworkPbEmitFilter
 * @brief Per-(sat,signal) scalar Kalman continuity filter on the emitted phase
 *        biases, with a self-tuning k·σ̂_step reset gate.
 */
class NetworkPbEmitFilter {
public:
    /// Outcome of a key's filter step this epoch; every cause except kSmooth emits
    /// the raw value.  Reported in @ref Entry::reset_cause and in the run-end
    /// telemetry breakdown.
    enum class Cause : int {
        kSmooth     = 0,  ///< posterior mean emitted (the common case).
        kInit       = 1,  ///< first sighting: seeded, value passed through (not a disc).
        kRefPathSlip= 2,  ///< reference-path (tree/pinned) arc slipped ⇒ raw value passed through.
        kInnovation = 3,  ///< innovation beyond the gate (v1 k·σ̂_step, v2 k·√(Ppred+R)) ⇒ treated as a genuine event, raw value passed through.
        kGap        = 4   ///< arc gap > max_gap_s ⇒ re-init, value passed through.
    };

    /// Filter tunables (all from `network.emit_pb_filter_*`; see GnssConfigLoad).
    struct Config {
        double q_m2_s        = 3.0e-7; ///< process noise (random walk) of the emitted bias (m²/s); ≈(3 mm)² per 30 s epoch, the quasi-static drift class.
        double jump_k        = 4.0;    ///< reset gate multiplier (Nσ; matches the D5 innov_k default).  v1: |innovation| > @c jump_k · σ̂_step.  v2 (@ref gate_v2): |innovation| > @c jump_k · √(Ppred+R), a true rate-transferable Nσ, so k is universal (4.0) at every cadence.
        double meas_sigma_m  = 0.05;   ///< σ̂_step seed (m): the a-priori per-epoch step noise before samples accrue; a non-positive value is replaced by 0.05 in the constructor.  v1: also the scale the robust estimate is regularised toward and floored below (0.25×).  v2: a seed only (the floor becomes a small absolute numerical guard, @ref kSigmaFloorAbsM), since the censoring-free feed makes structural collapse impossible.
        double max_gap_s     = 600.0;  ///< arc gap (s) beyond which a returning key is a first sighting (re-init, no smoothing across the gap).
        double window_epochs = 60.0;   ///< exponential-forgetting half-life (emitted epochs) of the σ̂_step estimator; ≤0 ⇒ accumulate the whole run.
        bool   split_by_const= true;   ///< σ̂_step per constellation letter (true) or one pooled estimate (false).
        bool   gate_v2       = false;  ///< v2 data-driven gate (`network.emit_pb_filter_gate_v2`).  Three coupled changes vs v1: (a) censoring-free σ̂_step feed: kInnovation rows (a valid prior, a legitimate step sample) also feed the estimator, which removes the v1 collapse where withheld gate-scale steps biased σ̂ low and tightened the gate; (b) model-scaled gate `jump_k · √(Ppred+R)`, so the epoch spacing enters through the prior's `q·gap` term (rate-transferable, no per-cadence k retune); (c) the seed is a numerical guard only.  Off ⇒ v1, bit-for-bit.
    };

    /// One emitted (sat,signal) phase bias to filter this epoch.
    struct Entry {
        std::string sat;              ///< RINEX satellite id ("G01","E11").
        std::string signal;           ///< RINEX phase obs code ("L1C").
        double      value_m{0.0};     ///< IN: raw post-rebase emitted bias (m).  OUT: the filtered value the emitter and product carry.
        bool        slipped{false};   ///< The emission-reference (tree/pinned) arc slipped this epoch (from the slip funnel) ⇒ reset; the raw value reaches the jump test unchanged and is never smoothed over.
        double      post_sigma_m{0.0};///< OUT: posterior 1σ (m) = √P (shrunk on smooth, = √R on reset/init).  The caller may stamp it onto the emitted phase-bias σ (`network.emit_pb_filter_emit_sigma`).
        Cause       reset_cause{Cause::kSmooth}; ///< OUT: why this key smoothed / reset (run-end telemetry).
    };

    /// Running tallies for the run-end telemetry line.
    struct Counters {
        long long epochs{0};            ///< @ref filterEpoch invocations with >=1 entry.
        long long inits{0};             ///< fresh first-sighting seeds (not discs).
        long long smoothed{0};          ///< posterior-mean emits.
        long long resets_refpath{0};    ///< reference-path slip resets.
        long long resets_innovation{0}; ///< calibrated k·σ̂_step innovation resets.
        long long resets_gap{0};        ///< gap re-inits.
        long long fed_rows{0};          ///< rows fed to the σ̂_step estimator (v1: = @ref smoothed; v2: @ref smoothed + @ref resets_innovation, the censoring-free feed; n_fed_all_rows telemetry).
        double    post_sigma_sum{0.0};  ///< Σ posterior σ over smoothed rows (mean = /@ref smoothed).
    };

    explicit NetworkPbEmitFilter(Config cfg) : cfg_(cfg) {
        phi_ = (cfg_.window_epochs > 0.0) ? std::pow(0.5, 1.0 / cfg_.window_epochs) : 1.0;
        if (!(cfg_.meas_sigma_m > 0.0)) cfg_.meas_sigma_m = 0.05;
    }

    /**
     * @brief Filter one epoch's emitted biases in place.
     *
     * For each entry: predict the per-key random-walk prior, form the innovation
     * against this epoch's raw value, and either reset (a reference-path slip, an
     * innovation beyond the gate, or a gap over @c max_gap_s ⇒ emit the raw value
     * and re-seed the key) or smooth (emit the posterior mean, shrink P, feed the
     * innovation into the σ̂_step estimator).  The gate is @c jump_k·σ̂_step in v1
     * and @c jump_k·√(Ppred+R) in v2; v2 also feeds innovation-reset rows to the
     * estimator.  A first sighting seeds and passes the value through.  @c value_m
     * is read and then overwritten with the value the emitter carries;
     * @c post_sigma_m and @c reset_cause are set.
     *
     * @param entries       The epoch's filterable phase biases (GLONASS excluded
     *                      by the caller).
     * @param epoch_time_s  Continuous epoch time (GPS seconds; week·604800+tow),
     *                      the per-key gap clock (product time, deterministic on
     *                      replay).
     */
    void filterEpoch(std::vector<Entry>& entries, double epoch_time_s);

    const Counters& counters() const { return counters_; }

    /// One key's scalar-KF posterior + last-seen clock.  Public because it is the
    /// payload of @ref FilterState (the RS-5 snapshot rows); the filter itself is
    /// the only writer.
    struct KeyState { double mean{0.0}; double var{0.0}; double last_seen_s{0.0}; };
    /// One (constellation|pooled) robust step-noise accumulator (geo-mean of ê²).
    /// Public for the same reason as @ref KeyState.
    struct StepAccum { double sum_log_e2{0.0}; double n_eff{0.0}; };

    /**
     * @brief The engine's cross-epoch filter state, exportable for the RTB-7
     *        snapshot (RS-5 emit-layer continuity, docs/SPEC_event_harness.md
     *        §21.10): every key's posterior and every constellation's
     *        σ̂_step accumulator.
     *
     * Like @ref NetworkEmitRebase this engine is a bare run-local, so without a
     * restore a warm resume cold-starts it: every key first-sights at the seam
     * (seeded at the raw value, @c kInit, @c P = R) and every σ̂_step bucket restarts
     * at the weak seed prior, so the first post-resume epochs run the widest gate of
     * the run and the smoothing carried on the continuous wire is lost.  Exporting
     * both maps lets a resume continue the emission-frame filter it inherited.
     *
     * @c cfg_ and @c phi_ are configuration (they come from the live run's knobs, not
     * from the writer's) and @ref Counters is run telemetry; neither travels.
     */
    struct FilterState {
        std::map<std::string, KeyState>  state;  ///< "sat|signal" -> scalar-KF posterior.
        std::map<std::string, StepAccum> step;   ///< constellation ("" = pooled) -> step-noise accumulator.
    };
    /// Snapshot the filter state (cheap: a few hundred keys + one bucket per constellation).
    FilterState exportState() const { return FilterState{state_, step_}; }
    /**
     * @brief Restore a previously exported filter state (a warm resume that
     *        continues the emission-frame filter).
     *
     * Replaces both maps wholesale (a resume's previous emitted epoch is the
     * writer's last emitted epoch) and leaves @ref counters() and the live @ref
     * Config alone.  A key absent from @p fs first-sights exactly as on a continuous
     * run.  The per-key gap clock (@c KeyState::last_seen_s) is restored with it, so
     * a resume older than @c Config::max_gap_s re-inits that key instead of
     * smoothing across the outage.
     *
     * @param fs State exported by @ref exportState (via the snapshot's @c PF / @c PS rows).
     */
    void restoreState(const FilterState& fs) { state_ = fs.state; step_ = fs.step; }

    /// Drop all per-key state + the σ̂_step accumulators (a `emit_pb_filter`
    /// toggle-flip cold start; every key first-sights next epoch).  Counters,
    /// like the re-base's, accumulate over the whole run and are left intact.
    void reset() { state_.clear(); step_.clear(); }

    /// Test seam: the current posterior (mean,var) for (sat,signal); false if the
    /// key has no state yet.
    bool stateForTest(const std::string& sat, const std::string& signal,
                      double& mean, double& var) const;

    /// Test seam: the current robust σ̂_step (m) for a constellation letter (""
    /// when pooled), the regularised, floored estimate the reset gate uses.
    double stepSigmaForTest(const std::string& constellation) const {
        return stepSigma(constellation);
    }

    /// Telemetry seam: current robust σ̂_step (m) for every constellation bucket
    /// that has accumulated ≥1 fed row ("" = pooled); the v2 run-end line reads
    /// this (n_fed_all_rows companion).  Empty before any row feeds.
    std::map<std::string, double> stepSigmas() const {
        std::map<std::string, double> out;
        for (const auto& kv : step_) out[kv.first] = stepSigma(kv.first);
        return out;
    }

private:
    /// σ̂_step (m) for a constellation bucket: the χ²₁-corrected geometric mean
    /// of the fed squared innovations, regularised toward the seed by a weak prior
    /// (so it is always defined and cannot collapse).  v1 floors it at a fraction
    /// of the seed (@ref kSigmaFloorFrac), the structural-collapse guard the
    /// censored feed needs; v2 floors it only at a small absolute numerical guard
    /// (@ref kSigmaFloorAbsM), with the seed kept as the weak prior.  See
    /// @ref NetworkCarryVce for the χ²₁ derivation.
    double stepSigma(const std::string& constellation) const;

    /// Bucket key for the step-noise estimate: the constellation letter, or ""
    /// when pooled.
    std::string bucket(const std::string& sat) const {
        return (cfg_.split_by_const && !sat.empty()) ? constellationKey(systemOfSat(sat))
                                                     : std::string();
    }

    // Geometric mean of a χ²₁ variate: exp(E[log χ²₁]) = exp(−γ−ln2) = e^{−γ}/2.
    // geo_mean(ê²) = σ²·kChi2GeoMean for ê~N(0,σ²), so σ = √(geo_mean(ê²)/kChi2GeoMean).
    static constexpr double kChi2GeoMean   = 0.2807297418;
    static constexpr double kTinyE2        = 1e-20;  ///< floor on ê² inside the log (m²).
    static constexpr double kPriorRows     = 3.0;    ///< weak seed prior (equivalent clean rows) on σ̂_step.
    static constexpr double kSigmaFloorFrac= 0.25;   ///< v1: σ̂_step never drops below this fraction of the seed (structural-collapse guard for the censored feed).
    static constexpr double kSigmaFloorAbsM= 1e-3;   ///< v2: σ̂_step absolute numerical guard (m, ≈1 mm); only stops a degenerate all-zeros window, since the censoring-free feed makes a fractional-of-seed floor unnecessary.

    Config    cfg_;
    double    phi_{1.0};   ///< per-epoch forgetting factor for the σ̂_step accumulators.
    std::map<std::string, KeyState>  state_;   ///< "sat|signal" -> scalar-KF posterior.
    std::map<std::string, StepAccum> step_;    ///< constellation ("" pooled) -> step-noise accumulator.
    Counters  counters_;
};

}  // namespace GnssProcessor
