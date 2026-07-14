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
 *   (b) a genuine innovation event @f$|z-\hat\mu| > k\,\hat\sigma_{\text{step}}@f$
 *       (the calibrated gate below), or
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
 * The reset threshold is @f$k\,\hat\sigma_{\text{step}}@f$, where
 * @f$\hat\sigma_{\text{step}}@f$ is a robust running estimate of the per-key step
 * noise — the tail-safe geometric-mean floor of the CLEAN (smoothed-epoch)
 * innovations, the same @f$\chi^2_1@f$-corrected estimator @ref NetworkCarryVce
 * uses (geo_mean(ê²)/e^{-γ}·½).  With the filter ON the residual step noise
 * shrinks, so the estimate is read only from smoothed rows (reset rows are
 * withheld, exactly like the M2 monitor's clean gate) and a small k regains a
 * meaningful test at cm scale — replacing the hand 0.05 m literal with a
 * measured Nσ gate that adapts per constellation and per regime.  The same
 * @f$\hat\sigma_{\text{step}}@f$ is also used as the measurement noise R of the
 * filter (a self-tuning adaptive KF), so the filter needs no hand R and is
 * robust to the datum-dependent level entanglement of the solve's formal
 * phase-bias σ (documented in the M4 contract work — the absolute emitted σ
 * overstates the consumer-relevant per-epoch magnitude, so it is a poor R).
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
        double jump_k        = 4.0;    ///< reset gate: |innovation| > @c jump_k · σ̂_step (Nσ; mirrors the D5 innov_k default).
        double meas_sigma_m  = 0.05;   ///< σ̂_step SEED (m) — the a-priori per-epoch step noise before samples accrue; also the scale the robust estimate is regularised toward and floored below.
        double max_gap_s     = 600.0;  ///< arc gap (s) beyond which a returning key is a first sighting (re-init, no smoothing across the gap).
        double window_epochs = 60.0;   ///< exponential-forgetting half-life (emitted epochs) of the σ̂_step estimator; ≤0 ⇒ accumulate the whole run.
        bool   split_by_const= true;   ///< σ̂_step per constellation letter (true) or one pooled estimate (false).
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

private:
    /// One key's scalar-KF posterior + last-seen clock.
    struct KeyState { double mean{0.0}; double var{0.0}; double last_seen_s{0.0}; };
    /// One (constellation|pooled) robust step-noise accumulator (geo-mean of ê²).
    struct StepAccum { double sum_log_e2{0.0}; double n_eff{0.0}; };

    /// σ̂_step (m) for a constellation bucket: the χ²₁-corrected geometric-mean
    /// floor of the clean innovations, regularised toward the seed by a weak
    /// prior (so it is always defined and cannot collapse) and floored at a
    /// fraction of the seed.  See @ref NetworkCarryVce for the χ²₁ derivation.
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
    static constexpr double kSigmaFloorFrac= 0.25;   ///< σ̂_step never drops below this fraction of the seed.

    Config    cfg_;
    double    phi_{1.0};   ///< per-epoch forgetting factor for the σ̂_step accumulators.
    std::map<std::string, KeyState>  state_;   ///< "sat|signal" -> scalar-KF posterior.
    std::map<std::string, StepAccum> step_;    ///< constellation ("" pooled) -> step-noise accumulator.
    Counters  counters_;
};

}  // namespace GnssProcessor
