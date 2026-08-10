#pragma once
/**
 * @file NetworkClockAnchor.hpp
 * @brief Card Q3 — the GUARDED temporal clock anchor
 *        (docs/SPEC_clock_temporal_anchor.md).
 *
 * ## What this card fixes
 *
 * The emitted per-satellite satellite-clock channel is a random walk with
 * robust step σ = **9.23 cm / 30 s** (measured, `runs/clkr_12h_hold`, 45 271
 * rows) against a **~2 cm** physical change (FC-1, carrier-phase measured,
 * corr ≈ 0).  It is not clock physics: the per-satellite σ spans only 1.37×
 * (7.93 … 10.87 cm) across a 32-satellite fleet of three clock technologies,
 * which no physical process does and an estimator's per-epoch noise does
 * exactly.
 *
 * The mechanism is one line of the D2 carry fold.  The satellite clock's
 * carried prior variance is `cp.var + sat_clock_q · dt`; at the lineage's armed
 * `sat_clock_q = 1e-3 m²/s` that is √(1e-3·30) = **17.3 cm**, i.e. ABOVE the
 * data's own per-epoch spread.  A random-walk prior only constrains when its
 * spread falls BELOW the data's; above it the prior is a bystander and the
 * emitted clock is re-drawn from each epoch's data.  Hence the walk.
 *
 * This card makes that prior bind — and supplies the event response without
 * which a binding prior is a standing-invariant-#1 violation.
 *
 * ## The two exact identities it rests on
 *
 * For the scalar carry (prior N(μ, P⁻), data information 1/R, posterior
 * P⁺ = (1/P⁻ + 1/R)⁻¹):
 *
 *  1. `Var(x̂ − μ) = P⁻ − P⁺` — **exactly**.  This is the guard's denominator,
 *     it is the prior-residual census's own `disp`, and it is Teunissen's
 *     generalized predicted residual for a predictable function (the GKF form).
 *     It is also the B3 data-incest fix `NetworkClockQc` already applies to the
 *     two-state model — a general identity, borrowed without borrowing that
 *     (NO-GO) model.
 *  2. Once the carry binds, `Var(x̂_k − x̂_{k−1}) → q·Δt`.  So the emitted walk
 *     is √(q·Δt) and @c network.clock_anchor_q IS the walk target.
 *
 * ## The guard, and why it sits where it sits
 *
 * `w = (x̂ − μ − median_const) / √max(P⁻ − P⁺, σ_floor²)`, tripping at
 * @c network.clock_anchor_k or on an absolute backstop.  The per-constellation
 * median removal is mandatory — the clock common mode is a free per-epoch gauge
 * (standing invariant #5).
 *
 * A trip WITHDRAWS the offending prior and RE-SOLVES the epoch before emission
 * (adapt-and-EMIT; never a withhold, never a de-weight).  That re-solve runs
 * **before the within-epoch RAIM adaptation**, which is load-bearing: a stiff
 * prior fighting a real step dumps the tension into residuals and raises σ̂₀, and
 * RAIM would then attribute a PRIOR fault to a satellite's or a station's DATA
 * and drop it.  A prior fault must be resolved before the measurement-fault
 * machinery looks.
 *
 * Cost: a shared-border information delta carries through the Schur reduction
 * unchanged (@ref NetworkNormalEquations::addSharedInfo's reduction-preserving
 * fast path), so the re-solve is one datum + factorization of the reduced K×K
 * system — no re-reduction, no re-assembly.
 *
 * ## Both edges (standing invariant #7)
 *
 * Per satellite the anchor is a two-state machine:
 *
 *  - **DECOUPLING** (trip / stale carry / resume seam / datum change / an
 *    external tier's refloat) ⇒ the satellite drops to LOOSE, which folds at the
 *    LEGACY `sat_clock_q` — i.e. exactly the pre-card behaviour.  That is what
 *    bounds what the decoupled state can accumulate: it is not unconstrained, it
 *    is on the carry the existing `clock_runaway_check` monitor already bounds.
 *    The edge is evented, counted, and (under @c network.clock_anchor_mark)
 *    WIRE-MARKED per satellite, because its clock just changed both temporal
 *    regime and value.
 *  - **RE-COUPLING** requires @c network.clock_anchor_recouple_epochs
 *    consecutive in-band epochs — the re-validation — and RE-SEEDS (the first
 *    anchored fold uses the LOOSE posterior variance, not a steady-state tight
 *    one).  Evented and counted, deliberately NOT wire-marked: re-engaging a
 *    prior whose mean IS the last posterior adds information without stepping
 *    the emitted value, so there is nothing for a consumer to reset.
 *
 * ## Per-satellite elapsed time
 *
 * The driver's `dt` is the inter-EPOCH gap, and every skip path advances it
 * while the D2 carry survives untouched — DOY-129's SH gate withheld 278
 * consecutive epochs.  A binding prior folded over `dt` would therefore treat a
 * 95-minute-old clock as 30 s old: the invariant-#1 catastrophe, verbatim.  The
 * anchor stamps a per-satellite `t_last` at every fold and inflates over
 * `pl_now_s − t_last`, decoupling beyond @c network.clock_anchor_max_gap_s.
 * Same defect class as the clock model's own P0-3 / R11 fix.
 *
 * ## Covariance treatment (decided consciously)
 *
 * The anchor rides the EXISTING D2 fold rather than adding a second prior, so
 * under `network.shared_carry_full_cov` the clock stays inside the C1 dense
 * block and its off-diagonals are retained: the card does not re-create the
 * independence assumption C1 removed, on the very state it makes stiffest.  The
 * residual inconsistency of any stiff carry is bounded by
 * @c network.clock_anchor_var_floor_m2 — the clock analogue of the B4 phase/code
 * bias floors, whose own comment exempted the clock because its carry never
 * bound.  A refloat MARGINALIZES the key from the carried block (never
 * conditions on it), the treatment @ref NetworkEstimator::addSharedPriorDense
 * already documents as exact for a departed key.
 */

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"   // NetworkSbasis::Plan (the re-solve's datum)

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct GnssTime;
struct NetworkRunContext;
struct NetworkRunTraces;

/// Disposition of one satellite's clock carry for one epoch, as decided by
/// @ref clockAnchorFoldVariance.
struct ClockAnchorFold {
    double var_eff{0.0};      ///< the variance to fold (already floored)
    bool   anchored{false};   ///< folded at the ANCHOR rate (false = LOOSE / legacy)
    bool   floor_hit{false};  ///< the consistency floor bound this fold
    bool   decoupled{false};  ///< this fold DECOUPLED the satellite (stale carry) — the caller events + marks it
};

/// @brief Card Q3 — decide the satellite clock's carried-prior variance for
///        this epoch, and advance the per-satellite anchor bookkeeping.
///
/// Called from BOTH D2 fold branches (diagonal and the C1 dense block's
/// `cov_eff` diagonal) so the MARGINAL prior on the clock is identical either
/// way — the property that makes the guard statistic path-independent.
///
/// @param ctx        run context (knobs + the per-satellite anchor state, mutated)
/// @param sat        satellite id
/// @param prev_var   the stashed posterior variance (`cp.var`)
/// @param dt_epoch   the driver's inter-epoch gap (the LEGACY inflation basis)
/// @param pl_now_s   continuous GPS seconds of this epoch
/// @param value      the carried value being folded (recorded for the guard)
/// @return the fold decision.  With the anchor disarmed this returns exactly
///         `{prev_var + q_clock·dt_epoch, false, false, false}` — the legacy
///         expression, byte-identical.
ClockAnchorFold clockAnchorFoldVariance(NetworkRunContext& ctx,
                                        const std::string& sat,
                                        double prev_var, double dt_epoch,
                                        double pl_now_s, double value);

/// @brief Card Q3 — mark every satellite as un-folded for a new epoch.
///
/// Called once per epoch before the D2 fold.  Separate from the fold call
/// because a satellite absent from `prev_carry` folds nothing and must not be
/// tested by the guard.
void clockAnchorBeginEpoch(NetworkRunContext& ctx);

/// @brief Card Q3 — the guard: test every anchored satellite's generalized
///        predicted residual, refloat + re-solve + mark the trips, and run the
///        LOOSE→ANCHORED re-validation.
///
/// MUST be called immediately after the primary `est.solve()` and BEFORE the
/// within-epoch RAIM adaptation (see the file banner).  No-op unless
/// `ctx.knobs.cta_on`.
///
/// @param ctx                 run context (state + counters, mutated)
/// @param config_             config (datum for the re-solve)
/// @param log                 logger
/// @param traces              event sink
/// @param se_time             epoch time
/// @param est                 the estimator holding this epoch's solved system
/// @param plan                the S-basis plan (its datum drives the re-solve)
/// @param x_s                 IN/OUT: the shared solution; REPLACED by the
///                            re-solved vector when a trip is resolved
/// @param ws                  epoch workspace — `cap_priors` / `cap_dense_*` are
///                            updated in step so a later RAIM replay cannot
///                            silently re-inject a withdrawn prior
/// @param registered_clock_sats  this epoch's clock columns
/// @param clock_refloat_sats  external QC refloats already known this epoch
///                            (input coupling only — the guard does NOT write
///                            into it, so the CB/PB stash refloat policy is
///                            unchanged)
void networkClockAnchorGuard(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const GnssTime& se_time,
                             NetworkEstimator& est,
                             const NetworkSbasis::Plan& plan,
                             Eigen::VectorXd& x_s, EpochWorkspace& ws,
                             const std::vector<std::string>& registered_clock_sats,
                             const std::set<std::string>& clock_refloat_sats,
                             bool datum_changed,
                             const std::set<char>& changed_consts);

}  // namespace GnssProcessor
