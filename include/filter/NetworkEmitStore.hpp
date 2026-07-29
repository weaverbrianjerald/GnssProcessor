#pragma once
/**
 * @file NetworkEmitStore.hpp
 * @brief Emit / store / product-write + closed-loop consume for the PPP-RTK network
 *        producer — the BACK half of the emit pipeline (the front half, solution
 *        preparation, is @ref networkSolPrep).
 *
 * Consumes the prepared @ref NetworkSolutionEpoch (and the datum-bump / withheld /
 * retro state @ref networkSolPrep returned) and finalizes the epoch:
 *   - Card-4 phase-D emit-layer frame RE-BASE (+ M5 PB emission-continuity filter)
 *     on the emitted phase biases, baked into @c sol before the store push so a
 *     NetworkProductIO replay reproduces the emitter disc counters;
 *   - @c emitToStore + the RTA-5 periodic S-contract re-broadcast;
 *   - the RTA-10 per-epoch heartbeat + the on-disk product write (+ RTA-6 flush);
 *   - the emitted-epoch bookkeeping (n_emitted, grade-metric / last-emit anchors,
 *     the R12 within-epoch T2 flag-rate commit);
 *   - the CLOSED-LOOP consume: supply this epoch's SH-VTEC slant iono to the
 *     held-out consumer, run it against the fresh store, accumulate the AR / 3D-error
 *     metrics; and the per-satellite solution CSV.
 *
 * Seeded at decomposition INC-11 (2026-07-22), which relocated the SB_EMITSTORE
 * block VERBATIM out of Application::runNetwork (dedent-only, zero substitution).
 * Produces NO crossing locals (void); every carry / emit-side object / run
 * accumulator is an explicit reference into the caller's state.
 *
 * ⚠ ORDERING CONTRACT — runs AFTER @ref networkSolPrep (consumes its result) and
 * BEFORE the SB_IONODIAG/RAIM diagnostics + the epoch stash.  It MUTATES @c sol (the
 * re-base), the emitter / product_writer (store push + on-disk write), persistent
 * carries (n_emitted / first_emit_content_s / last_emit_content_s / the pivot
 * refresh) and the run-level closed-loop accumulators.  The caller keeps the
 * @c subBegin/subEnd(SB_EMITSTORE) brackets.
 */

#include "app/NetworkSetupResult.hpp"        // NetworkSetupResult
#include "common/PppObservation.hpp"         // PppObservation (modeled)
#include "filter/NetworkEstimator.hpp"       // NetworkEstimator
#include "products/NetworkEmitRebase.hpp"    // NetworkEmitRebase
#include "products/NetworkPbEmitFilter.hpp"  // NetworkPbEmitFilter
#include "products/NetworkProductEmitter.hpp"// NetworkSolutionEpoch
#include "rinex/MultiRinexSync.hpp"          // SyncedEpoch

#include <Eigen/Core>

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Emit, store, write and closed-loop-consume this epoch's solution.  See the
 *        file banner for the full step list and the ordering contract.  Parameters
 *        beyond ctx/config_/log/traces/setup carry the epoch-local assembly state,
 *        the five @ref NetworkSolPrepResult locals (@c sol is re-based in place), the
 *        emit-rebase / PB-filter engines, and the run-level closed-loop accumulators.
 */
void networkEmitStore(
    NetworkRunContext& ctx,
                     const GnssConfig& config_,
                     const std::shared_ptr<spdlog::logger>& log,
                     NetworkRunTraces& traces,
                     NetworkSetupResult& setup,
                     const SyncedEpoch& se,
                     const NetworkEstimator& est,
                     const Eigen::VectorXd& x_s,
                     double pl_now_s,
                     bool datum_changed,
                     double sigma0_adapt,
                     NetworkSolutionEpoch& sol,
                     const std::set<char>& epoch_withheld_consts,
                     bool global_bump,
                     const std::set<char>& serialized_consts,
                     const std::set<std::string>& retro_emit_sats,
                     bool emit_rebase_on,
                     bool emit_pb_filter_on,
                     bool pb_filter_emit_sigma,
                     NetworkEmitRebase& emit_rebase_engine,
                     NetworkPbEmitFilter& pb_filter_engine,
                     const std::vector<bool>& present,
                     const std::vector<int>& est_index_of,
                     const std::vector<std::vector<PppObservation>>& modeled,
                     const std::set<std::string>& shared_phase_keys,
                     const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                     bool we_t2_opp,
                     bool we_t2_flag,
                     std::size_t& cl_epochs,
                     std::size_t& cl_ar_ran,
                     std::size_t& cl_ar_fixed,
                     long long& cl_sum_fixed,
                     double& cl_first_fix_tow,
                     int& cl_first_fix_week,
                     std::deque<double>& cl_recent_err3d);

} // namespace GnssProcessor
