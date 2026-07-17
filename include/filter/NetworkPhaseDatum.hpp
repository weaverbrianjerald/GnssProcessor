#pragma once
/**
 * @file NetworkPhaseDatum.hpp
 * @brief Phase-bias↔ambiguity datum maintenance for the PPP-RTK network
 *        producer: the prioritized-datum spectrum-based drop-and-heal.
 *
 * The prioritized union registers a phase-bias column for every (sat,signal)
 * that any two stations track.  Two things can leave such a column below the
 * rank-gate cutoff and would withhold the WHOLE epoch (every well-anchored
 * satellite lost to one bad column): (1) its ONLY ambiguity pin lived in an
 * anchor station that was DROPPED in the serial consume (a rank-deficient
 * local block) — a pure near-null orphan; (2) the joint W6 code-bias
 * estimation leaves a marginally-tracked satellite's phase bias weakly
 * separated from its clock — a near-null-but-real column.  The heal
 * soft-anchors exactly those columns toward 0 (σ = 1 m: far above the cutoff
 * so the residual gauge is resolved, negligible against the 1e10 ambiguity
 * pins so well-anchored columns are untouched) — a bounded, signalled
 * one-epoch degradation (the emitter bias-jump disc bumps when the proper
 * ambiguity anchor returns).
 *
 * Seeded at decomposition INC-8 (2026-07-17), which relocated
 * healNearNullPhaseBias VERBATIM out of ApplicationNetwork.cpp; the anchor
 * election + incumbent carry complete the seam at INC-8b (see
 * docs/network_decomposition_addendum_2026-07-16.md).  These functions own NO
 * state: every carry/workspace object is an explicit reference into the
 * caller's NetworkRunContext / EpochWorkspace.
 */

#include "filter/NetworkEstimator.hpp"

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Identify near-null phase-bias directions of the reduced shared
 *        normals and soft-anchor exactly those columns toward 0 (σ = 1 m), so
 *        one weak column cannot withhold the whole epoch.
 *
 * The cutoff is @ref NetworkEstimator::kNullSpaceTol · max(λmax, 1) —
 * deliberately the SAME cutoff as the rank gate this heal protects.  A
 * direction counts as a heal target when a phase-bias column spans it with
 * |component| > 0.3 (a clock-gauge direction has ~zero phase-bias weight, so
 * its minor PB participants stay below the fraction and are left alone).
 *
 * ★ PERF: this does a FULL eigenVECTOR decomposition of the reduced normals
 * (O(n_shared³) — ~1156³ at 305 stations), so invoke it ONLY from the
 * gate-failure path, never every epoch.  A healthy network passes the cheap
 * eigenvalue-only gate on the first try and never pays this cost.
 *
 * @param est                     Joint estimator (non-const:
 *                                reducedSharedNormals lazily reduces;
 *                                addSharedPrior folds the heal anchors).
 * @param shared_phase_keys       This epoch's shared phase-bias key set.
 * @param phase_datum_prioritized network.phase_datum_prioritized; false ⇒
 *                                immediate 0 (the legacy datum heals nothing).
 * @param within_epoch_armed      network.raim_within_epoch: mirror the heal
 *                                anchors into @p cap_priors for the replay.
 * @param cap_priors              RAIM-replay capture channel (ws.cap_priors —
 *                                see the EpochWorkspace writer-site contract in
 *                                filter/NetworkRunTypes.hpp).
 * @return Number of phase-bias columns soft-anchored (0 = nothing healed /
 *         not prioritized / eigensolve failed).
 */
int healNearNullPhaseBias(NetworkEstimator& est,
                          const std::set<std::string>& shared_phase_keys,
                          bool phase_datum_prioritized,
                          bool within_epoch_armed,
                          std::vector<NetworkEstimator::SharedPrior>& cap_priors);

} // namespace GnssProcessor
