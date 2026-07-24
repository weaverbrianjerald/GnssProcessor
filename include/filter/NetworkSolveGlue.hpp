#pragma once
/**
 * @file NetworkSolveGlue.hpp
 * @brief The per-epoch SOLVE GLUE of the PPP-RTK network producer — the S-basis datum
 *        build + null-space gate, the estimator solve, the within-epoch RAIM adaptation
 *        (assess / drop-culprit / re-solve), and the observability / σ̂0 withhold gates —
 *        relocated VERBATIM out of Application::runNetwork at decomposition INC-9b
 *        (2026-07-22).  The estimation math is byte-copied; only the plumbing changes.
 *
 * Two structural adaptations of the pure-relocation pattern (both proven earlier in the
 * series), because this block is neither self-contained nor straight-line:
 *
 *  1. **EpochAction (INC-7).** The block's four epoch-skip `continue`s (datum-build
 *     failure, observability-gate withhold, solve failure, σ̂0-withhold escape-hatch)
 *     become `result.action = EpochAction::SkipEpoch; return result;`; the per-skip
 *     bookkeeping (n_skipped / emitEpochStatus / product-status / prev_week,prev_tow)
 *     stays verbatim in-body.  The caller does `if (action == SkipEpoch) continue;`
 *     BEFORE subEnd(SB_SOLVEGLUE), preserving the original subEnd-bypass semantics.
 *  2. **Result struct (INC-11pre).** The 18 loop-body locals the block PRODUCES and the
 *     downstream seams (RAIM / ClockQc / SolPrep / EmitStore) CONSUME cross the seam
 *     boundary via @ref NetworkSolveGlueResult (std::move epilogue); the caller
 *     re-aliases each with `auto& x = sg_.x;` so downstream code is untouched.
 *
 * The two build lambdas (buildEnsembleGauges / buildCarryDdPriors) are passed as
 * std::function so the within-epoch re-solve re-invokes them with byte-identical
 * capture semantics.  Owns NO state; every carry object is a reference into ctx.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator (+ PerConstSigma0)
#include "filter/NetworkSbasis.hpp"      // NetworkSbasis::Plan
#include "filter/NetworkRunTypes.hpp"    // EpochAction / NetStation / StationAssembly / EpochWorkspace
#include "filter/NetworkWlRunTypes.hpp"  // (transitively) hoisted run-loop types
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "app/NetworkSetupResult.hpp"    // NetworkSetupResult (emitter / product_writer / stations / ...)
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <Eigen/Core>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;

/**
 * @brief The 18 loop-body locals SB_SOLVEGLUE produces that downstream epoch seams
 *        consume, plus the epoch disposition.  On a SkipEpoch return the value fields
 *        are default (the caller `continue`s before reading them).
 */
struct NetworkSolveGlueResult {
    Eigen::VectorXd                                  x_s;
    NetworkSbasis::Plan                              plan;
    std::set<char>                                   changed_consts;
    bool                                             datum_changed{false};
    bool                                             clk_model_warm{false};
    bool                                             emit_rate_warm{false};
    std::set<std::string>                            clock_refloat_sats;
    std::set<std::string>                            retro_flag_sats;
    bool                                             retro_feed{false};
    int                                              n_arc_refloat_epoch{0};
    int                                              n_adapt{0};
    double                                           sigma0_adapt{0.0};
    bool                                             perconst_active{false};
    std::set<char>                                   perconst_publish;
    std::set<char>                                   perconst_dec_withhold;
    std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;
    bool                                             we_t2_opp{false};
    bool                                             we_t2_flag{false};
    EpochAction                                      action{EpochAction::Proceed};
};

/**
 * @brief Run the datum build + solve + within-epoch RAIM adaptation for this epoch.
 * @see the file banner for the EpochAction / result-struct contract.  @p buildEnsembleGauges
 *      and @p buildCarryDdPriors are the run-TU wrapper lambdas (re-invoked on re-solve).
 */
NetworkSolveGlueResult networkSolveGlue(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    NetworkEstimator& est,
    EpochWorkspace& ws,
    const SyncedEpoch& se,
    const std::vector<StationAssembly>& asm_res,
    std::vector<int>& est_index_of,
    const std::vector<bool>& present,
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::vector<std::string>& col_desc,
    int n_clock,
    int n_pb,
    int n_cb,
    int n_present,
    int total_pins,
    const std::set<std::string>* carry_we_drop_sats,
    const std::map<int, std::set<std::string>>* carry_we_drop_arcs,
    bool carry_warm,
    bool within_epoch_armed,
    double dt,
    double pl_now_s,
    double clk_anchor_sigma,
    const std::function<void()>& buildEnsembleGauges,
    const std::function<void()>& buildCarryDdPriors);

} // namespace GnssProcessor
