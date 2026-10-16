#pragma once
/**
 * @file NetworkSbasisCompletion.hpp
 * @brief Stage 7 of the network solve (card NOM3 step SG): S-basis completion (SBC,
 *        network.sbasis_complete).  Each separated extra near-null direction of the reduced
 *        system is constrained along its eigenvector at its carried level, with both edges
 *        evented (standing invariant 7).  The completion's run-end report sits beside it.
 */

#include "network/driver/NetworkRunTypes.hpp"   // EpochWorkspace

#include <Eigen/Core>

namespace GnssProcessor {

class NetworkEstimator;
struct NetworkRunContext;
struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunEndView;
struct SolveDatum;   // filter/NetworkSolveGlue.hpp

/**
 * @brief Stage 7: S-basis completion.  Derives the realized near-null dimension from the
 *        matrix state the gate inspects, constrains each separated extra direction at its
 *        carried level, hands the datum graph census its spectral half, and events the engage,
 *        release, skip and refusal edges.
 * @param run   The run: the estimator is folded; ctx carry (dg_spec, sbc_active_sats) and
 *              counters (n_sbc_*) and the events are written.
 * @param ep    The epoch, read: se.
 * @param datum The clock datum (read: plan, datum_changed).
 * @param ws    [in/out] The replay capture (cap_sbc_H, cap_sbc_z).
 */
void networkSbasisCompletion(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                             EpochWorkspace& ws);

/**
 * @brief The completion's continuity targets: the carried shared state mapped into this
 *        epoch's columns (clock, code and phase biases, SH coefficients; the phase-bias level
 *        map; the card-2 kept-epoch code-bias map).  Zero on a datum re-reference.
 * @param ctx           The run context (read: carry).
 * @param est           The estimator (read: the column maps).
 * @param datum_changed A clock-pivot handover was signalled this epoch.
 * @return The targets, one per shared column.
 */
Eigen::VectorXd networkSbcContinuityTargets(const NetworkRunContext& ctx, const NetworkEstimator& est,
                                            bool datum_changed);

/**
 * @brief Run-end report: S-basis completion (SBC).
 *
 * S-basis completion: the near-null directions it constrained, its releases and
 * refusals, and a warning when it never fired while the observed near-null count moved.
 *
 * Gate: returns 0 at once unless `ctx.knobs.sbasis_complete_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSbasisCompletionRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
