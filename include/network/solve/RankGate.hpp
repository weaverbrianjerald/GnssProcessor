#pragma once
/**
 * @file RankGate.hpp
 * @brief Stages 5 and 8 of the network solve: the estimability dump of the
 *        system the gate is about to judge, and the null-space rank gate with the prioritized
 *        drop-and-heal, the withhold accounting, the observed-count envelope, the datum-direction
 *        information ratio and the datum graph census's count-only fallback.  The gate's
 *        run-end report sits beside them.
 */

#include "network/driver/RunTypes.hpp"   // EpochAction, EpochWorkspace
#include <cstddef>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunEndView;
struct SolveDatum;   // network/solve/SolveGlue.hpp

/**
 * @brief Stage 5: the budgeted spectrum of the reduced system the gate is about to judge
 *        (DG5, RF-NS), at the gate's tolerance (DG2); decrements the budget.
 * @param run   The run: the estimability traces and spectrum_diag_budget are written.
 * @param ep    The epoch, read: se, columns (col_desc).
 * @param datum The clock datum (read: plan.declared_dimension).
 */
void networkEstimabilityDump(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum);

/**
 * @brief Stage 8: the null-space rank gate.  On a deficiency, the prioritized drop-and-heal
 *        soft-anchors near-null phase-bias columns and gates once more; then the withhold
 *        accounting (DG6), the observed-count envelope, the datum-direction information ratio
 *        (invariant 7) and the datum graph census's count-only fallback.
 * @param run   The run: the estimator (heal priors), ctx counters and carry, the traces and the
 *              events are written.
 * @param ep    The epoch, read: se, pass_a (shared_phase_keys), columns (registered_clock_sats).
 * @param datum The clock datum (read: plan).
 * @param ws    [in/out] The replay capture the heal priors are recorded in.
 * @return SkipEpoch when the gate withholds the epoch (exit GateWithheld), else Proceed.
 */
EpochAction networkRankGate(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                            EpochWorkspace& ws);

/**
 * @brief Run-end report: the observability rank gate and the datum-direction ratio.
 *
 * The observability rank gate: its calibration line when the tolerance is not
 * the default, the two causes of an over-specified withhold, and the invariant-#7
 * datum-direction information ratio that tells them apart (measured in
 * NetworkFrameDiag).
 *
 * Gate: each section under its own condition, as in the teardown:
 * `config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol`;
 * `ctx.counters.n_gate_withheld_over > 0`; `ctx.counters.ddi_epochs > 0 &&
 * ctx.counters.ddi_dirs == 0`; `ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs >
 * 0`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkRankGateRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of RankGate, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RankGateCounters {
    // Observability of the prioritized-datum drop-and-heal: epochs where the gate threw
    // and the near-null phase-bias soft anchor recovered it, and the total columns
    // anchored.  Without these a run that recovers every epoch would look successful
    // with no trace (the success log line is at debug level).
    size_t n_healed_epochs = 0, n_healed_cols = 0;
    // The same quantities counted on every fired attempt, recovered or not.  The pair
    // above counts successes only, so an attempt that fires every epoch and never
    // rescues the gate would read as "0 healed"; the withhold warning in
    // NetworkSolveGlue then carries "drop-and-heal failed" as a trailing clause.  That
    // is the inert-looking but live wiring standing invariant #4 exists to catch, so
    // the attempt is counted separately: attempts > 0 with recoveries == 0 is the
    // signature of an attempt that aims at the right columns but cannot resolve the
    // residual alone (the insufficiency criterion, PA5).
    size_t n_heal_attempt_epochs = 0, n_heal_attempt_cols = 0;
    int    spectrum_diag_budget = 6;    // estimability spectrum dump: the first N gate evaluations (a budget rather than one shot, because a rows epoch needs a later firing than epoch 1).  Seeded from network.trace_estimability_budget by the NetworkRunContext ctor (default 6 = this constant; RF-NS); under network.trace_estimability_deficit_only a clean epoch does not spend it.

    // Rank-gate calibration telemetry.  The gate is an exact-equality test that throws
    // in both directions, so "withheld" alone hides which failure happened.
    // Under-specified (observed > declared: a missing datum constraint, as in the
    // BeiDou storm) and over-specified (observed < declared: the tolerance pushed below
    // this config's own gauge depth, a mis-calibration that withholds a sound config)
    // need opposite fixes and must be separate numbers.  min/max are over the first gate
    // evaluation of each epoch (one sample per epoch, showing what the datum looked
    // like); a post-heal re-gate is a different question and is counted in
    // n_healed_epochs.
    std::size_t n_gate_evaluated = 0;        ///< epochs on which the gate ran (denominator for the rest)
    std::size_t n_gate_withheld_under = 0;   ///< withheld with observed > declared (missing datum constraint)
    std::size_t n_gate_withheld_over  = 0;   ///< withheld with observed < declared (tolerance below the gauge depth)
};

} // namespace GnssProcessor
