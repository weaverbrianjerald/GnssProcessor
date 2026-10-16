#pragma once
/**
 * @file SbasisCompletion.hpp
 * @brief Stage 7 of the network solve: S-basis completion (SBC,
 *        network.sbasis_complete).  Each separated extra near-null direction of the reduced
 *        system is constrained along its eigenvector at its carried level, with both edges
 *        evented (standing invariant 7).  The completion's run-end report sits beside it.
 */

#include "network/driver/RunTypes.hpp"   // EpochWorkspace

#include <Eigen/Core>
#include <cstddef>

namespace GnssProcessor {

class NetworkEstimator;
struct NetworkRunContext;
struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunEndView;
struct SolveDatum;   // network/solve/SolveGlue.hpp

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
 *        map; the kept-epoch code-bias map).  Zero on a datum re-reference.
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
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSbasisCompletionRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of SbasisCompletion, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SbasisCompletionCounters {
    int nullspace_obs_min = -1;              ///< min observed near-null count over the run (-1 = never evaluated)
    int nullspace_obs_max = -1;              ///< max observed near-null count over the run (-1 = never evaluated)

    // Run-end telemetry of the S-basis completion (SBC, network.sbasis_complete).
    // Standing invariant #4: an armed completion whose counters never move on a GPS+Gal
    // run means the block never saw the deficiency it was built for (a miswire), not a
    // clean run.  Every refusal path is counted separately because each needs a
    // different fix (cap: a pathological epoch or tolerance; degenerate: an orphaned
    // exact gauge, the territory of the soft anchors; weight: re-place rank_gate_tol).
    std::size_t n_sbc_epochs = 0;        ///< epochs on which ≥1 completion row was folded
    std::size_t n_sbc_dirs = 0;          ///< completion rows folded, cumulative
    int         sbc_max_dirs = 0;        ///< max rows folded on one epoch
    std::size_t n_sbc_release = 0;       ///< sbc_release events (satellite re-observed: the re-coupling edge of invariant #7)
    std::size_t n_sbc_capped = 0;        ///< epochs refused: extras beyond sbasis_complete_max
    std::size_t n_sbc_degenerate = 0;    ///< directions skipped as unseparated from the declared-gauge cluster (machine-zero orphans, left to the drop-and-heal; not epoch-fatal)
    std::size_t n_sbc_weight_marginal = 0; ///< epochs refused: completion weight < 2 decades above the cutoff (K2)
};

} // namespace GnssProcessor
