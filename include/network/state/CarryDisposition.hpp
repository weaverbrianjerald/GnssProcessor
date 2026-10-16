#pragma once
/**
 * @file CarryDisposition.hpp
 * @brief The decided-once fold disposition of the carried border (see
 *        docs/refound/PRM_parameter_registry_2026-09-29.md sections 4.3, 4.4 and 15).
 *
 * The D2 fold (`networkInjectD2CarryPrior`) decides, for every carried border entry, whether
 * it folds, at which process noise, and under which rule: a frame re-reference, the clock
 * oscillator, the READMIT park and seed, the clock monitor's reinit, the Q3 anchor, the B4
 * floors, the VCE factor, the switch-on seam's level noise.  The
 * coupling rows of the exact coupling update are built in PASS B, before the fold runs, and
 * need the same decision, so until now they read a mirror of the fold, clause for
 * clause, beside it.  @ref networkCarryDecide makes the decision once, before PASS B, and
 * records it on each entry (@ref CarryPrior::fold); the rows and the fold read the record.
 *
 * The record holds the route and the process-noise increment, not the folded variance.
 * Between the decision and the fold, a clock-pivot handover re-expresses the changed
 * constellation's carried clocks (the S-transform adds @f$v_i^2\sigma_q^2@f$ to each
 * variance and moves each value) after the rows are built; @ref carryFoldVarEff forms the
 * variance from the carried variance its caller holds, so the rows and the fold apply one
 * decision to the variance each of them sees, and the re-expression is counted, not hidden.
 * The fold keeps the one clause no earlier point can know (the handover's retired datum),
 * and the anchor's per-satellite bookkeeping is committed at the fold
 * (@ref clockAnchorFoldCommit), so an epoch that ends before the fold advances nothing.
 */

#include "network/driver/RunTypes.hpp"   // CarryPrior, CarryFoldDecision, CarryDisposition

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct ClockAnchorFold;     // network/solve/ClockAnchor.hpp
class  NetworkEstimator;
struct GnssTime;

/**
 * @brief Whether a disposition folds the entry at a variance the consumer forms
 *        (@ref carryFoldVarEff): Folded, Anchored, Loose, Reinit and Seeded.
 */
inline bool carryDispositionFolds(CarryDisposition d) {
    return d == CarryDisposition::Folded || d == CarryDisposition::Anchored
        || d == CarryDisposition::Loose  || d == CarryDisposition::Reinit
        || d == CarryDisposition::Seeded;
}

/**
 * @brief Whether a carried border entry's shared column is in this epoch's layout (a
 *        satellite clock, phase bias or code bias; false for any other kind).
 *
 * The one predicate for the decision's departed flag and READMIT's park and seed; the
 * estimator's column lookups throw on an absent column.
 */
bool carryColumnPresent(const NetworkEstimator& est, const CarryPrior& cp);

/**
 * @brief Decide one carried border entry's fold for this epoch, without side effects.
 *
 * The clauses in the fold's order: a VtecSh entry is @c Held (folded by the SH inject,
 * before the gate); a phase bias whose frame was re-referenced this epoch (eq34 or the
 * anchored election) is @c Reref; a clock under the clock oscillator model is
 * @c OnOscillator; a parked entry is @c Parked unless its column
 * is back inside the seed horizon (@c Seeded, at q over the gap since the park); a clock the
 * monitor flagged is @c Reinit; otherwise the entry folds at q of its kind over the driver's
 * step, a clock under the armed anchor at the anchor's increment (@c Anchored or @c Loose,
 * through @ref clockAnchorFoldDecide).  The handover's retired-datum skip is not decided
 * here (it is known only after PASS B; the fold applies it and records @c Retired), and the
 * non-positive screen is the consumer's, on the variance it forms.
 * @param ctx           run context (read only)
 * @param est           this epoch's estimator, layout registered (column presence)
 * @param cp            the carried entry
 * @param dt            the driver's inter-epoch step, the value the fold receives
 * @param cta_now_s     continuous GPS seconds of the epoch (the anchor's clock), or 0 off
 * @param readmit_now_s continuous GPS seconds of the epoch (the READMIT clock), or 0 off
 * @return the decision, with the carried value and variance it was made on.
 * @param events The epoch's events, read: the chain election's re-referenced (sat, signal) set.
 */
CarryFoldDecision carryFoldDecide(const NetworkRunContext& ctx, const NetworkEstimator& est,
                                  const CarryPrior& cp, double dt, double cta_now_s,
                                  double readmit_now_s, const EpochEvents& events);

/**
 * @brief The variance the fold folds for decision @p d on the carried variance @p var.
 *
 * The fold's own chain, in its order: the reinit variance for @c Reinit; otherwise
 * @p var plus the increment, for a clock under the armed anchor through the anchor's floor
 * (@ref clockAnchorFoldAt); the B4 floor of a phase or code bias; the VCE factor; and, on
 * the dense fold only, the switch-on seam's level noise.  The caller screens the result
 * (`!(v > 0)` does not fold).  The coupling rows call it on the variance they were stashed
 * with and the fold on the variance it folds, so one decision gives each its own variance.
 * @param ctx    run context (read only)
 * @param cp     the carried entry (its kind and satellite)
 * @param d      the decision
 * @param var    the carried variance the caller holds (m^2)
 * @param dense  true on the dense fold (shared_carry_full_cov), which adds the seam noise
 * @param anchor when not null and the anchor is armed for a clock, receives the anchor's
 *               fold on @p var (its variance before the B4 floor, VCE and seam), which the
 *               fold commits
 * @return the effective variance (m^2).
 */
double carryFoldVarEff(const NetworkRunContext& ctx, const CarryPrior& cp,
                       const CarryFoldDecision& d, double var, bool dense,
                       ClockAnchorFold* anchor = nullptr);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Decide every carried border entry for this epoch (@ref carryFoldDecide), and
 *        record the decision on the entry.
 *
 * Called by the driver once per epoch, after the shared columns are registered and the
 * switch-on seam's level noise is set, and before PASS B, whose coupling rows read it.
 * Writes every entry of @c prev_carry, parked ones included, so no reader sees a decision
 * from an earlier epoch.  Writes nothing else: the anchor's state and counters advance at
 * the fold.
 *
 * Epoch form; the explicit form and its forwarding adapter
 * retired (the unit tests call this form through
 * tests/support/NetworkTestRun.hpp).
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx).
 *            Read: run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se.time (t), ep.clock.dt
 *           (dt).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: run context (the carry's decisions are written)
 *   - @p est = run.engines.est: this epoch's estimator, layout registered
 *   - @p t = ep.se.time: the epoch
 *   - @p dt = ep.clock.dt: the driver's inter-epoch step
 */
void networkCarryDecide(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Pivot-frame process noise (@c network.clock_pivot_frame_q): add the pivot's walk to the decided
 *        increment of every other folding satellite clock of its constellation.
 *
 * A carried clock is the pivot-frame quantity @f$c_j - c_p@f$ (the clock datum pins one
 * satellite per constellation), so its state equation carries the pivot's walk over its
 * own interval, @f$r_p\,\Delta t_j@f$, and two such clocks share @f$r_p
 * \min(\Delta t_j, \Delta t_k)@f$ (network/carry/PivotFrameQ.hpp).  This pass adds the
 * diagonal half to @c q_applied_m2 and records @f$r_p@f$ on the decision
 * (@c pfq_rate_m2ps); the dense fold and the exact coupling update add the pairwise half.
 * The pivot is the one the stashed posterior was pinned on (@c carry.pfq_pivot).  The
 * stash skips pinned columns, so that pivot is not carried and @f$r_p@f$ is the model's
 * physical per-satellite rate: the anchor's q with the anchor armed, the legacy rate
 * without it (a carried pivot would use its own decided rate).  A reinit and a non-folding entry take no
 * term.  Called by @ref networkCarryDecide after every entry is decided; a no-op with the
 * knob off, so the off path is byte-identical.  Counted in @c counters.pfq.
 * @param ctx run context (decisions of the carried clocks are updated)
 */
void networkCarryPivotFrame(NetworkRunContext& ctx);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the pivot-frame clock noise (PFQ) and the fold's dispositions of the
 *        carried border (PRM 1b).
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in CarryDisposition.cpp, print in this order, each gated on its own knob:
 *   - networkPivotFrameQRunEnd: the pivot-frame clock noise (PFQ).
 *   - networkCarryDispositionTableRunEnd: the fold's dispositions of the carried border (PRM 1b).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkCarryDispositionRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of CarryDisposition, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct CarryDispositionCounters {
    // The decided-once fold disposition (networkCarryDecide before
    // PASS B, read by the rows and the fold).  prm_disp counts key-epochs by
    // (SharedParamKind, the disposition the fold applied); prm_departed the foldable
    // ones whose column had left the layout (the estimator marginalizes them).  A key
    // re-expressed between the decision and the fold (the handover S-transform, the
    // ephemeris cut-over re-level) is counted with its largest variance change: the rows
    // were built on the decided variance.  prm_undecided_at_fold: entries the fold met
    // with no decision this epoch (0 unless a path skips the pass).
    long long prm_decide_epochs = 0;
};

}  // namespace GnssProcessor
