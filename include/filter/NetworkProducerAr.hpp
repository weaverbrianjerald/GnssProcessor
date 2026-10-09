#pragma once
/**
 * @file NetworkProducerAr.hpp
 * @brief Producer-side network ambiguity-resolution diagnostic (Phase 2, log only)
 *        plus the one-shot full-arrowhead normal-equation export.
 *
 * Runs on the final accepted network solution and only reads it: it neither
 * re-pins the datum nor re-solves, so the network solution is byte-identical
 * whether this block runs or not.  Two cooperating pieces:
 *
 *  1. **Producer network AR, Phase 2** (`network.ambiguity_resolution.enable` +
 *     `min_epochs`): reconstructs the per-station float ambiguities, forms an
 *     independent between-station double-difference basis (star topology per
 *     (const,signal), GPS+Galileo only), builds the exact DD covariance from
 *     @ref NetworkEstimator::jointConditionalCovariance, converts to cycles, and runs
 *     @ref ils::ilsFix only to log how many integers the network could fix (with the
 *     median DD formal-σ and float-fraction convergence gauges).  The DD ambiguities
 *     are datum-invariant, so the fix attempt is independent of the phase-bias datum.
 *     With `network.ambiguity_resolution.trace` also armed it writes every DD of the
 *     basis to `logs/network_producer_ar.csv` (@ref kProducerArHeader) with its
 *     identity, float value, formal σ, subset membership and returned integer, so the
 *     search can be graded against simulator truth offline
 *     (`scripts/simnet_producer_ar_grade.py`); a fix count alone is not a grade.  The
 *     run-end readout is the "Network (producer AR, Phase 2 log-only)" clause of
 *     networkProducerArRunEnd (below), fed by the `ctx.counters.netar_*` tallies.
 *  2. **One-shot NEQ export** (`network.neq_export_epoch`): a post-solve, read-only dump
 *     of the assembled arrowhead normal equations (border N_ss raw and reduced, with
 *     RHS; each station's N_sr / N_rr / u_r; shared and local column labels) as
 *     row-major little-endian double `.bin` for offline spectral / weak-direction
 *     attribution.  Fires on one epoch per run; the solution is untouched.
 *
 * Relocated out of Application::runNetwork's SB_NETAR block at decomposition INC-9b
 * (see docs/network_decomposition_plan.md, the SOLVE-REGION decomposition).  Owns no
 * state.  The caller keeps the @c subBegin/subEnd(SB_NETAR) brackets.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator
#include "filter/NetworkSbasis.hpp"      // NetworkSbasis::Plan (shared_datum)
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"      // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <Eigen/Core>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only
struct NetworkRunContext;  // app/NetworkRunContext.hpp, included in the .cpp only

/**
 * @brief Pure helpers for the Phase-2 ILS cap permutation (unit-tested in
 *        tests/test_producer_ar_trace.cpp).
 *
 * They are separate from @ref networkProducerAr because the ILS receives a
 * capped, re-ordered copy of the DD basis: @c ils::IlsResult::kept indexes the
 * search vector, not the basis, and a trace that maps those indices back
 * directly would label the wrong satellite with the wrong integer while still
 * looking well-formed.  Both steps are pure functions of their inputs, so the
 * mapping can be tested on a constructed case rather than on a 24 h run.
 */
namespace netar {

/**
 * @brief Indices of the @p cap lowest-formal-σ DDs, i.e. the search subset.
 *
 * Sorts 0..n-1 ascending by @f$Q_{aa}@f$ (the standard partial-AR ordering) and
 * truncates to @p cap.  @p cap <= 0 or >= n returns the identity permutation,
 * unsorted: the uncapped search consumes the basis in its original order.
 *
 * @param Q_cyc  DD covariance in cycles²; only the diagonal is read.
 * @param cap    Search-dimension cap (@c network.ambiguity_resolution.ils_max_dim).
 * @return `search_of`: search row a ↦ its index in the full DD basis.
 */
std::vector<int> lowestSigmaOrder(const Eigen::MatrixXd& Q_cyc, int cap);

/// Per-basis-DD outcome of one epoch's search: the content of the trace's
/// @c in_subset / @c fixed_int / @c kept columns.
struct DdFixLabel {
    bool   in_subset = false;  ///< This DD entered the (possibly capped) ILS search.
    bool   kept      = false;  ///< The ILS returned an integer for it.
    double fixed_int = 0.0;    ///< That integer (cycles).  Meaningless unless @c kept.
};

/**
 * @brief Map an @c ils::IlsResult back onto the full DD basis.
 *
 * @param n_dd       Size of the full DD basis (the trace's row population).
 * @param search_of  Search row ↦ basis index (from @ref lowestSigmaOrder).
 * @param kept       @c IlsResult::kept: indices into the search vector.
 * @param afix       @c IlsResult::afix: integers in @p kept order.
 * @return One label per basis DD, indexed 0..n_dd-1.
 *
 * Out-of-range entries in @p search_of / @p kept are dropped, and a @p kept
 * longer than @p afix stops at the length of @p afix, so a mis-sized result
 * loses rows instead of mislabelling them.
 */
std::vector<DdFixLabel> mapFixToBasis(int n_dd,
                                      const std::vector<int>& search_of,
                                      const std::vector<int>& kept,
                                      const Eigen::VectorXd& afix);

} // namespace netar

/**
 * @brief Run the log-only producer-AR and NEQ-export tail for this epoch.
 *
 * @param ctx           [in,out] run context: reads `counters.n_emitted` (the
 *                      min_epochs and NEQ-export gates) and
 *                      `knobs.carry_aware_covariance`, and adds to the
 *                      `counters.netar_*` run-end tallies.  Taken whole for
 *                      seam-family uniformity.
 * @param config_       Reconstructs `nc = config_.getNetworkConfig()` and
 *                      `netar = nc.ambiguity_resolution`.
 * @param log           "main_log": the Phase-2 AR summary and NEQ-export info rows.
 * @param traces        Trace-stream owner; the seam writes @c producer_ar_trace
 *                      (one row per DD, @ref kProducerArHeader) when
 *                      @c network.ambiguity_resolution.trace is armed.
 * @param se            Current synced epoch (week/TOW for the log rows).
 * @param est           Post-solve estimator (const): reconstructStation /
 *                      jointConditionalCovariance / normals / column labels.
 * @param x_s           Post-fit shared solution vector (non-empty guard and the
 *                      per-station reconstruction).
 * @param stations      This run's network stations (float-amb reconstruction source).
 * @param est_index_of  Per-station estimator-slot map (< 0 = not in this epoch).
 * @param asm_res       Per-station assembly results (h2local for local-column labels).
 * @param plan          S-basis plan (shared_datum for the conditional covariance).
 * @param log_dir_      Output root for the NEQ-export artifacts (log_dir_/neq_export).
 */
void networkProducerAr(NetworkRunContext& ctx,
                       const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       NetworkRunTraces& traces,
                       const SyncedEpoch& se,
                       const NetworkEstimator& est,
                       const Eigen::VectorXd& x_s,
                       const std::vector<NetStation>& stations,
                       const std::vector<int>& est_index_of,
                       const std::vector<StationAssembly>& asm_res,
                       const NetworkSbasis::Plan& plan,
                       const std::filesystem::path& log_dir_);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the producer ambiguity-resolution search.
 *
 * The producer ambiguity-resolution search (Phase 2, log-only): epochs searched, the
 * ratio and success-rate medians and the fix counts, and a warning when the search never
 * reached the ILS.
 *
 * Gate: returns 0 at once unless `nc.ambiguity_resolution.enable`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkProducerArRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
