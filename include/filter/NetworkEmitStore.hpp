#pragma once
/**
 * @file NetworkEmitStore.hpp
 * @brief Emit / store / product-write + closed-loop consume for the PPP-RTK network
 *        producer: the back half of the emit pipeline (the front half, solution
 *        preparation, is @ref networkSolPrep).
 *
 * Consumes the prepared @ref NetworkSolutionEpoch (and the datum-bump / withheld /
 * retro state @ref networkSolPrep returned) and finalizes the epoch:
 *   - with the SH health gate engaged, writes only the withheld H heartbeat and
 *     returns (no store push, no product write, no closed-loop consume);
 *   - the Card-4 phase-D emit-layer frame re-base on the emitted phase biases, the
 *     CLK-R clock emission re-base, the RS-4 warm-resume phase-bias seam mark and the
 *     PBH phase-bias hold, all baked into @c sol before the store push so a
 *     NetworkProductIO replay reproduces the emitter disc counters;
 *   - @c emitToStore, the card SID signal-roster declaration, the JT and MS event
 *     drains, and the RTA-5 periodic S-contract re-broadcast;
 *   - the RTA-10 per-epoch heartbeat + the on-disk product write (+ RTA-6 flush);
 *   - the emitted-epoch bookkeeping (n_emitted, grade-metric / last-emit anchors,
 *     the R12 within-epoch T2 flag-rate commit);
 *   - the closed-loop consume: supply this epoch's SH-VTEC slant iono to the
 *     held-out consumer, run it against the fresh store, accumulate the AR / 3D-error
 *     metrics and drain the consumer traces; and the per-satellite solution CSV.
 *
 * Relocated from Application::runNetwork at decomposition INC-11 (the SB_EMITSTORE
 * block).  Returns nothing to the caller; every carry / emit-side object / run
 * accumulator is an explicit reference into the caller's state.
 *
 * Ordering contract: runs after @ref networkSolPrep (consumes its result) and
 * before the SB_IONODIAG/RAIM diagnostics + the epoch stash.  It mutates @c sol (the
 * re-bases and the hold), the emitter / product_writer (store push + on-disk write),
 * persistent carries (n_emitted / first_emit_content_s / last_emit_content_s / the
 * contract pivot refresh) and the run-level closed-loop accumulators.  The caller keeps
 * the @c subBegin/subEnd(SB_EMITSTORE) brackets.
 */

#include "app/NetworkSetupResult.hpp"        // NetworkSetupResult
#include "common/PppObservation.hpp"         // PppObservation (modeled)
#include "filter/NetworkEstimator.hpp"       // NetworkEstimator
#include "filter/NetworkRunGroups.hpp"       // NetworkEmitEngines, ClosedLoopTally (card NOM step TD1a)
#include "products/NetworkClockEmitRebase.hpp" // NetworkClockEmitRebase (Card CLK-R)
#include "products/NetworkEmitRebase.hpp"    // NetworkEmitRebase
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp (included in the .cpp only)
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp (included in the .cpp only)

/**
 * @brief Emit, store, write and closed-loop-consume this epoch's solution.  See the
 *        file banner for the full step list and the ordering contract.  Parameters
 *        beyond ctx/config_/log/traces/setup carry the epoch-local assembly state,
 *        the five @ref NetworkSolPrepResult locals (@c sol is re-based in place), the
 *        emit-layer engines (@ref NetworkEmitEngines; their switches are
 *        NetworkKnobs::emit_rebase_on), and the
 *        run-level closed-loop tally (@ref ClosedLoopTally), which this function
 *        accumulates.
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
                     NetworkEmitEngines& emit_engines,
                     const std::vector<bool>& present,
                     const std::vector<int>& est_index_of,
                     const std::vector<std::vector<PppObservation>>& modeled,
                     const std::set<std::string>& shared_phase_keys,
                     const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                     bool we_t2_opp,
                     bool we_t2_flag,
                     ClosedLoopTally& cl_tally);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: producer complete: epochs emitted, skipped and withheld.
 *
 * The "Network producer complete" line: epochs emitted and skipped, the skips withheld
 * by the observability gate and by the A4 sigma0 ceiling, of the synced epochs. Printed
 * on every run; graders read it as the run's headline.
 *
 * Gate: prints always.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkEmitOutcomeRunEnd(const NetworkRunEndView& v);

/**
 * @brief Run-end report: heartbeats of a deferred product writer that never opened.
 *
 * Withheld-epoch heartbeats buffered for the deferred v2 product writer when the writer
 * never opened (card RTA-10): the run never emitted and no product file exists.
 *
 * Gate: prints under `!setup.pending_h_pre_open.empty()`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkDeferredHeartbeatsRunEnd(const NetworkRunEndView& v);

/**
 * @brief Run-end report: the product contract round trip (B6).
 *
 * The serialized-contract round trip (B6): closes the product writer, reads the product
 * back, checks its contract and the per-receipt contracts (RTA-5), and writes
 * network_contract_check.csv; on a run that emitted nothing, the writer-never-opened
 * record. It closes the product writer, so it precedes the closed-loop summary.
 *
 * Gate: each section under its own condition, as in the teardown: `contract_pending &&
 * !prod_path.empty()`; `product_writer && !prod_path.empty()`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkProductContractRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
