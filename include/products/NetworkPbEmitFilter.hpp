#pragma once
/**
 * @file NetworkPbEmitFilter.hpp
 * @brief Card (M5 prerequisite) — emission-frame CONTINUITY filter for the
 *        network's per-(satellite,signal) phase biases: a slip-gated, per-key
 *        scalar Kalman smoother on the POST-REBASE emitted values plus a
 *        noise-calibrated jump gate.
 *
 * ## Why this exists (the disc-floor forensics)
 *
 * @ref NetworkEmitRebase already removes the per-epoch datum churn (the common
 * per-component fractional + per-satellite integer×λ) so the emitted @f$b_s@f$
 * is *frame*-continuous.  What it does NOT remove is the ordinary per-epoch
 * ESTIMATION NOISE (~cm scale) on each emitted value.  The emitter's phase-bias
 * jump test (@ref NetworkProductEmitter, a hand 0.05 m threshold) sits INSIDE
 * that noise distribution — the Jul-2026 wire forensics measured a median
 * per-epoch step of ~0.08 m against a 0.05 m gate, so ~40 % of keys false-alarm
 * EVERY epoch (a floor of ~200 discontinuities/epoch that scales with key
 * count).  Only a handful (≈50 of ~12 000) of those steps exceed 0.5 m — the
 * genuine, Galileo-dominated cycle-level events the slip-aware machinery
 * correctly preserves.  **The estimable frame is fine; the emitted VALUES are
 * unfiltered and the alarm is miscalibrated.**  That floor silently blocks
 * wire-side ambiguity resolution (a consumer cannot resolve integers while it is
 * reset ~200×/epoch), so removing it is a prerequisite for the network-AR
 * milestone.
 *
 * ## The physics this leverages (quasi-static PB, emission frame only)
 *
 * True satellite phase biases are quasi-static (they drift over hours).  But the
 * RAW phase-bias columns in the solve are DATUM-DEPENDENT — the free eq34 datum
 * re-realization under the invariant DD carry re-expresses them every epoch, so
 * time-linking them IN THE ESTIMATOR is the exact Card-4 disease (a stale stiff
 * prior warps the whole solution past a jump) and is REJECTED.  The place where
 * the quasi-static physics is safely expressible is the **EMISSION FRAME**:
 * after @ref NetworkEmitRebase the per-key emitted value is continuous by
 * construction up to (noise + genuine events), so a per-key filter THERE is
 * outputs-only and S-transparent — it never touches the solve, so every user
 * double difference and the producer's dd-fraction grading are byte-unchanged.
 *
 * ## The slip invariant (the one non-negotiable)
 *
 * A cycle slip is an integer jump in ONE (receiver,satellite) arc's ambiguity;
 * it resets ONLY that ambiguity.  The emitted PB lump steps ONLY when the
 * slipped arc is part of the lump's DATUM definition (an eq34 tree / pinned
 * arc) — exactly the class @ref NetworkEmitRebase is handed as @c Entry::slipped
 * (from @c carry_slipped_ambs ∩ @c pinned_amb_targets) and preserves as a genuine
 * "reference-path slip step".  This filter therefore holds HARD through ordinary
 * slips (the PB is untouched by construction) and RESETS — emitting the raw
 * value so its step survives to the emitter's jump test — only on
 *   (a) a reference-path slip flag for the key,
 *   (b) a genuine innovation event past the calibrated gate below
 *       (@f$|z-\hat\mu| > k\,\hat\sigma_{\text{step}}@f$ in v1;
 *       @f$|z-\hat\mu| > k\,\sqrt{P_{\text{pred}}+R}@f$ in v2), or
 *   (c) an arc gap longer than @c max_gap_s (a re-appearance is a first sighting,
 *       not a continued arc).
 * A blind "align each key to its last value" would drive every step to zero and
 * launder a real slip — the standing invariant #1 failure.  This filter never
 * does that: a reset PASSES THE RAW VALUE THROUGH, so the genuine step reaches
 * the (unchanged) emitter jump test.
 *
 * ## How it clears the floor without any emitter change
 *
 * On a SMOOTH epoch the filter emits the posterior mean, whose step from the
 * previous emitted value is a small fraction of the innovation (Kalman gain
 * @f$K = P/(P+R) \ll 1@f$ in steady state), i.e. millimetres — far below the
 * emitter's 0.05 m gate, so the emitter stays silent.  On a RESET epoch it emits
 * the raw value, whose step is the full event magnitude, so the emitter fires.
 * The emitter's jump test — the SOLE discontinuity authority, unchanged and
 * still the replay-reproducible one — thus fires only at genuine events.  Both
 * live and a @c NetworkProductIO replay run the emitter on the SAME baked
 * (filtered) values, so the discontinuity counters reproduce byte-for-byte; the
 * filter, like the re-base, bakes its transform into the emitted product and is
 * never re-run on replay.
 *
 * ## The calibrated jump gate (self-tuning, per constellation)
 *
 * @f$\hat\sigma_{\text{step}}@f$ is a robust running estimate of the per-key step
 * noise — the tail-safe geometric-mean floor of the innovations, the same
 * @f$\chi^2_1@f$-corrected estimator @ref NetworkCarryVce uses
 * (geo_mean(ê²)/e^{-γ}·½).  The same @f$\hat\sigma_{\text{step}}@f$ is also the
 * measurement noise R of the filter (a self-tuning adaptive KF), so the filter
 * needs no hand R and is robust to the datum-dependent level entanglement of the
 * solve's formal phase-bias σ (documented in the M4 contract work — the absolute
 * emitted σ overstates the consumer-relevant per-epoch magnitude, so it is a
 * poor R).  Two gate generations select on @c Config::gate_v2:
 *
 * **v1 (default, @c gate_v2 == false).**  The reset threshold is
 * @f$k\,\hat\sigma_{\text{step}}@f$ and @f$\hat\sigma_{\text{step}}@f$ is read
 * only from smoothed (accepted) rows — reset rows are withheld, exactly like the
 * M2 monitor's clean gate.  That censoring is safe ONLY when the true per-epoch
 * step is well below the gate; when it sits AT gate scale (measured at 300 s: a
 * ~5–10 cm step against a k=4·(0.25·seed) floor) the withheld rows bias
 * @f$\hat\sigma_{\text{step}}@f$ low, the gate tightens, more rows reject — a
 * documented death spiral (Jul-2026 dev300s: 92 % of resets were false-alarm
 * innovations).  The interim cure was a per-rate HAND retune (k 4→8, seed
 * 0.05→0.10 m) — which does not transfer across cadences (at 30 s the implied
 * 20 cm floor straddles a single-cycle L1 step).
 *
 * **v2 (@c gate_v2 == true) — data/model-driven, rate-transferable.**  Three
 * coupled changes remove the hand retune:
 *   1. **Censoring-free feed.**  Every row with a valid prior whose reset is NOT
 *      structural feeds @f$\hat\sigma_{\text{step}}@f$ — smoothed rows AND
 *      innovation-gated rows (kInit / kGap have no per-epoch prior; kRefPathSlip
 *      is an exogenously flagged genuine event, both stay out).  The
 *      @f$\chi^2_1@f$ geometric mean is tail-safe at the measured contamination
 *      (<1 % genuine events at 30 s, ~0 % at 300 s), so feeding the gate-scale
 *      steps recovers the true noise floor instead of censoring it away — the
 *      spiral cannot start.
 *   2. **Model-scaled gate.**  The threshold is @f$k\,\sqrt{P_{\text{pred}}+R}@f$,
 *      the predicted innovation σ (the same @f$S@f$ the Kalman gain uses one line
 *      below).  The epoch spacing enters through @f$P_{\text{pred}} = P +
 *      q\cdot\text{gap}@f$ — dt is IN THE MODEL, mirroring @f$q_z\,dt@f$ inside
 *      D5's formal Cov(v) (@ref NetworkCarryVce lineage) — so ONE @c jump_k
 *      transfers 30 s ↔ 300 s untouched (the card's reason to exist).
 *   3. **Seed as a numerical guard.**  With all rows feeding, structural
 *      collapse is impossible, so @c meas_sigma_m keeps only its weak
 *      first-epochs prior (@ref kPriorRows) and the 0.25×seed floor is replaced
 *      by a small absolute guard (@ref kSigmaFloorAbsM); @c jump_k returns to a
 *      universal 4.0 — an honest false-alarm rate (α = erfc(k/√2) ≈ 6·10⁻⁵/key,
 *      ~0.03 alarms/epoch over ~420 keys, either cadence).
 *
 * The physics that lets ONE σ̂ serve both rates: the step σ is dominated by solve
 * noise, not a drift accumulated over the gap, so v2a's lumped σ̂ already
 * transfers — but its LEVEL is regime-dependent, not a constant (design estimate
 * ~6–8 cm; the Jul-15 nightrun measured σ̂ = 8.1 cm at 300 s and 20 cm at 30 s,
 * the latter = single-L1-cycle scale because the unconverged DD churn is
 * genuinely in the 30 s wire and therefore, correctly, in the censoring-free
 * feed).  The transfer does not require a dt-independent σ̂: k is a true Nσ
 * against whatever σ̂ the run measures.  A two-component
 * R(dt)=σ̂²_solve + q̂_drift·dt (the M2 Floor/Rate robust fit, with its condition
 * guard for the fixed-cadence unidentifiable case) is a DEFERRED v2b refinement,
 * not required for the transfer.
 *
 * ## Residual-risk envelope (honest, v2 AND v1-as-designed)
 *
 * The miss band SCALES WITH THE MEASURED σ̂: with k=4 the gate sits at
 * @f$4\hat\sigma_{\text{step}}@f$ ≈ 25–30 cm when σ̂ is at the 6–8 cm design
 * level, but ≈ 80 cm in a churn-dominated regime like the Jul-15 30 s arms
 * (σ̂ ≈ 20 cm) — the envelope below widens proportionally there.  An UNFLAGGED
 * reference-path slip whose emitted step lands between ~2σ and
 * @f$k\,\hat\sigma_{\text{step}}@f$ — e.g. a missed single-cycle L1 step, ~19 cm,
 * which is INSIDE the miss band at the churn-regime σ̂, not just near its edge —
 * does NOT reset: it smooths through and the filter converges to the new level
 * over ~1/K epochs (K the steady-state Kalman gain).  That is a bounded,
 * sub-gate transient the consumer never sees as a discontinuity, NOT a laundered
 * slip (the value is not snapped back — it tracks to the true new level).  This
 * risk class is intrinsic to ANY @f$k\,\hat\sigma@f$ gate and exists in
 * v1-as-designed identically; upstream slip coverage (the ref-path flag, which
 * does NOT depend on this gate) makes the miss class rare, and v2 neither adds
 * to nor removes it — it is documented, not pretended away.  The envelope
 * shrinks back to the design level as DD convergence improves (the R6/M5
 * prerequisite), since the churn feeding σ̂ is the same churn under attack there.
 *
 * ## Scope / conventions
 *
 * Pure numeric leaf; no dependency on the estimator, the emitter, or
 * ApplicationNetwork.  The caller (ApplicationNetwork, inside the re-base block)
 * supplies each emitted (sat,signal) value + its reference-path slip flag and
 * the epoch's continuous time; the leaf rewrites the value in place to the
 * filtered value and reports the posterior σ (the caller may write it onto the
 * emitted product) and the reset cause (for the run-end telemetry).  GLONASS
 * (FDMA) is simply not passed in (its keys keep their raw emitted value).
 * Byte-identity OFF is structural: the object is only constructed / called under
 * `network.emit_pb_filter` (itself gated inside the re-base block).  Driven
 * directly from @c tests/test_pb_emit_filter.cpp with synthetic streams of known
 * truth — the standalone-leaf precedent set by @ref NetworkEmitRebase and
 * @ref NetworkCarryVce.
 */

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
    /// Why a key's filter reset (emitted the raw value) this epoch — the emitted
    /// @ref Entry::reset_cause + the run-end telemetry breakdown.
    enum class Cause : int {
        kSmooth     = 0,  ///< posterior mean emitted (the quiet, common case).
        kInit       = 1,  ///< first sighting: seeded, value passed through (not a disc).
        kRefPathSlip= 2,  ///< reference-path (tree/pinned) arc slipped ⇒ raw value passed through.
        kInnovation = 3,  ///< |z-μ̂| > k·σ̂_step ⇒ genuine event, raw value passed through.
        kGap        = 4   ///< arc gap > max_gap_s ⇒ re-init, value passed through.
    };

    /// Filter tunables (all from `network.emit_pb_filter_*`; see GnssConfigLoad).
    struct Config {
        double q_m2_s        = 3.0e-7; ///< process noise (random walk) of the emitted bias (m²/s); ≈(3 mm)² per 30 s epoch — the quasi-static drift class.
        double jump_k        = 4.0;    ///< reset gate multiplier (Nσ; mirrors the D5 innov_k default).  v1: |innovation| > @c jump_k · σ̂_step.  v2 (@ref gate_v2): |innovation| > @c jump_k · √(Ppred+R) — a TRUE rate-transferable Nσ, so k is universal (4.0) at every cadence.
        double meas_sigma_m  = 0.05;   ///< σ̂_step SEED (m) — the a-priori per-epoch step noise before samples accrue.  v1: also the scale the robust estimate is regularised toward AND floored below (0.25×).  v2: demoted to a seed only (the floor becomes a small absolute numerical guard, @ref kSigmaFloorAbsM) since the censoring-free feed makes structural collapse impossible.
        double max_gap_s     = 600.0;  ///< arc gap (s) beyond which a returning key is a first sighting (re-init, no smoothing across the gap).
        double window_epochs = 60.0;   ///< exponential-forgetting half-life (emitted epochs) of the σ̂_step estimator; ≤0 ⇒ accumulate the whole run.
        bool   split_by_const= true;   ///< σ̂_step per constellation letter (true) or one pooled estimate (false).
        bool   gate_v2       = false;  ///< v2 data-driven gate (`network.emit_pb_filter_gate_v2`).  Three coupled changes vs v1: (a) CENSORING-FREE σ̂_step feed — kInnovation rows (a valid prior, a legitimate step sample) feed the estimator too, killing the v1 death-spiral where withheld gate-scale steps biased σ̂ low and tightened the gate; (b) MODEL-SCALED gate `jump_k · √(Ppred+R)` so the epoch spacing enters through the prior's `q·gap` term (rate-transferable, no per-cadence k retune); (c) the seed is a numerical guard only.  OFF ⇒ v1 bit-for-bit.
    };

    /// One emitted (sat,signal) phase bias to filter this epoch.
    struct Entry {
        std::string sat;              ///< RINEX satellite id ("G01","E11").
        std::string signal;           ///< RINEX phase obs code ("L1C").
        double      value_m{0.0};     ///< IN: raw post-rebase emitted bias (m).  OUT: the filtered value the emitter/product must carry.
        bool        slipped{false};   ///< The emission-reference (tree/pinned) arc slipped this epoch (from the slip funnel) ⇒ HARD reset, raw value survives to the jump test.  NEVER laundered.
        double      post_sigma_m{0.0};///< OUT: posterior 1σ (m) = √P (shrunk on smooth, = √R on reset/init).  The caller may stamp it onto the emitted phase-bias σ (`network.emit_pb_filter_emit_sigma`).
        Cause       reset_cause{Cause::kSmooth}; ///< OUT: why this key smoothed / reset (run-end telemetry).
    };

    /// Running tallies for the run-end telemetry line.
    struct Counters {
        long long epochs{0};            ///< @ref filterEpoch invocations with >=1 entry.
        long long inits{0};             ///< fresh first-sighting seeds (not discs).
        long long smoothed{0};          ///< posterior-mean (quiet) emits.
        long long resets_refpath{0};    ///< reference-path slip resets.
        long long resets_innovation{0}; ///< calibrated k·σ̂_step innovation resets.
        long long resets_gap{0};        ///< gap re-inits.
        long long fed_rows{0};          ///< rows fed to the σ̂_step estimator (v1: = @ref smoothed; v2: @ref smoothed + @ref resets_innovation — the censoring-free feed, n_fed_all_rows telemetry).
        double    post_sigma_sum{0.0};  ///< Σ posterior σ over smoothed rows (mean = /@ref smoothed).
    };

    explicit NetworkPbEmitFilter(Config cfg) : cfg_(cfg) {
        phi_ = (cfg_.window_epochs > 0.0) ? std::pow(0.5, 1.0 / cfg_.window_epochs) : 1.0;
        if (!(cfg_.meas_sigma_m > 0.0)) cfg_.meas_sigma_m = 0.05;
    }

    /**
     * @brief Filter one epoch's emitted biases IN PLACE.
     *
     * For each entry: predict the per-key random-walk prior, form the innovation
     * against this epoch's raw value, and either RESET (a reference-path slip, an
     * innovation above @c jump_k·σ̂_step, or a gap over @c max_gap_s ⇒ emit the raw
     * value, re-seed the key) or SMOOTH (emit the posterior mean, shrink P, feed
     * the clean innovation into the σ̂_step estimator).  A first sighting seeds
     * and passes the value through.  @c value_m is read then overwritten with the
     * value the emitter must carry; @c post_sigma_m and @c reset_cause are set.
     *
     * @param entries       The epoch's filterable phase biases (GLONASS excluded
     *                      by the caller).
     * @param epoch_time_s  Continuous epoch time (GPS seconds; week·604800+tow) —
     *                      the per-key gap clock (product time, deterministic on
     *                      replay).
     */
    void filterEpoch(std::vector<Entry>& entries, double epoch_time_s);

    const Counters& counters() const { return counters_; }

    /// Drop all per-key state + the σ̂_step accumulators (a `emit_pb_filter`
    /// toggle-flip cold start; every key first-sights next epoch).  Counters,
    /// like the re-base's, accumulate over the whole run and are left intact.
    void reset() { state_.clear(); step_.clear(); }

    /// Test seam: the current posterior (mean,var) for (sat,signal); false if the
    /// key has no state yet.
    bool stateForTest(const std::string& sat, const std::string& signal,
                      double& mean, double& var) const;

    /// Test seam: the current robust σ̂_step (m) for a constellation letter (""
    /// when pooled) — the regularised, floored estimate the reset gate uses.
    double stepSigmaForTest(const std::string& constellation) const {
        return stepSigma(constellation);
    }

    /// Telemetry seam: current robust σ̂_step (m) for every constellation bucket
    /// that has accumulated ≥1 fed row ("" = pooled) — the v2 run-end line reads
    /// this (n_fed_all_rows companion).  Empty before any row feeds.
    std::map<std::string, double> stepSigmas() const {
        std::map<std::string, double> out;
        for (const auto& kv : step_) out[kv.first] = stepSigma(kv.first);
        return out;
    }

private:
    /// One key's scalar-KF posterior + last-seen clock.
    struct KeyState { double mean{0.0}; double var{0.0}; double last_seen_s{0.0}; };
    /// One (constellation|pooled) robust step-noise accumulator (geo-mean of ê²).
    struct StepAccum { double sum_log_e2{0.0}; double n_eff{0.0}; };

    /// σ̂_step (m) for a constellation bucket: the χ²₁-corrected geometric-mean
    /// floor of the fed innovations, regularised toward the seed by a weak prior
    /// (so it is always defined and cannot collapse).  v1 floors it at a fraction
    /// of the seed (@ref kSigmaFloorFrac) — the structural-collapse guard the
    /// censored feed needs; v2 floors it only at a small absolute numerical guard
    /// (@ref kSigmaFloorAbsM), the seed demoted to the weak prior.  See
    /// @ref NetworkCarryVce for the χ²₁ derivation.
    double stepSigma(const std::string& constellation) const;

    /// Bucket key for the step-noise estimate: the constellation letter, or ""
    /// when pooled.
    std::string bucket(const std::string& sat) const {
        return (cfg_.split_by_const && !sat.empty()) ? std::string(1, sat[0]) : std::string();
    }

    // Geometric mean of a χ²₁ variate: exp(E[log χ²₁]) = exp(−γ−ln2) = e^{−γ}/2.
    // geo_mean(ê²) = s·kChi2GeoMean for ê~N(0,s), so s = geo_mean(ê²)/kChi2GeoMean.
    static constexpr double kChi2GeoMean   = 0.2807297418;
    static constexpr double kTinyE2        = 1e-20;  ///< floor on ê² inside the log (m²).
    static constexpr double kPriorRows     = 3.0;    ///< weak seed prior (equivalent clean rows) on σ̂_step.
    static constexpr double kSigmaFloorFrac= 0.25;   ///< v1: σ̂_step never drops below this fraction of the seed (structural-collapse guard for the censored feed).
    static constexpr double kSigmaFloorAbsM= 1e-3;   ///< v2: σ̂_step absolute numerical guard (m, ≈1 mm) — only stops a degenerate all-zeros window; the censoring-free feed makes a fractional-of-seed floor unnecessary.

    Config    cfg_;
    double    phi_{1.0};   ///< per-epoch forgetting factor for the σ̂_step accumulators.
    std::map<std::string, KeyState>  state_;   ///< "sat|signal" -> scalar-KF posterior.
    std::map<std::string, StepAccum> step_;    ///< constellation ("" pooled) -> step-noise accumulator.
    Counters  counters_;
};

}  // namespace GnssProcessor
