#pragma once
/**
 * @file RankGate.hpp
 * @brief Stages 5 and 8 of the network solve (card NOM3 step SG): the estimability dump of the
 *        system the gate is about to judge, and the null-space rank gate with the prioritized
 *        drop-and-heal, the withhold accounting, the observed-count envelope, the datum-direction
 *        information ratio and the datum graph census's count-only fallback.  The gate's
 *        run-end report sits beside them.
 */

#include "network/driver/RunTypes.hpp"   // EpochAction, EpochWorkspace

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
 * The observability rank gate (card DG): its calibration line when the tolerance is not
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

} // namespace GnssProcessor
