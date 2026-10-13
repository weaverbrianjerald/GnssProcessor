#pragma once
/**
 * @file NetworkSolveGlue.hpp
 * @brief The per-epoch solve glue of the PPP-RTK network producer: the S-basis datum
 *        build and null-space gate, the estimator solve, the within-epoch RAIM
 *        adaptation (assess, drop culprit, re-solve), and the observability and σ̂0
 *        withhold gates.  Moved out of Application::runNetwork at decomposition INC-9b
 *        with the estimation math unchanged; only the plumbing differs.
 *
 * The block is neither self-contained nor straight-line, so it uses two adaptations of
 * the plain relocation pattern (both used earlier in the decomposition series):
 *
 *  1. **EpochAction (INC-7).** The four sites where the driver loop skipped the epoch
 *     with `continue` (datum-build failure, observability-gate withhold, solve failure,
 *     σ̂0-withhold escape hatch) set `result.action = EpochAction::SkipEpoch` and
 *     return.  The per-skip bookkeeping (n_skipped, emitEpochStatus, product status,
 *     prev_week / prev_tow) stays in the body.  On SkipEpoch the caller returns
 *     `LoopSignal::Next` before subEnd(SB_SOLVEGLUE), which keeps the original
 *     `continue`'s bypass of subEnd.
 *  2. **Result struct (INC-11pre).** The 18 loop-body locals that the block produces
 *     and the downstream seams (RAIM, ClockQc, SolPrep, EmitStore) consume cross the
 *     seam boundary in @ref NetworkSolveGlueResult (moved in at the end of the body);
 *     the caller re-aliases each with `auto& x = sg_.x;`, so downstream code is
 *     unchanged.
 *
 * The two build lambdas (buildEnsembleGauges, buildCarryDdPriors) are passed as
 * std::function so the within-epoch re-solve re-invokes them with byte-identical
 * capture semantics.  The seam owns no state; every carry object is a reference into
 * ctx.
 *
 * The within-epoch re-solve hands its culprit sets to buildCarryDdPriors through the
 * epoch workspace (@ref EpochWorkspace::we_culprits, card WCF): the T2 block adapts the
 * sets in that member, the driver's rebuild lambda reads the member of the same
 * workspace, and the block clears it when it ends.
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
 *        are default (the caller ends the epoch before reading them).
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
    /// The landed within-epoch replay's culprits (card FCF), copied from the epoch
    /// workspace's culprit sets (@ref EpochWorkspace::we_culprits) before the T2 block clears
    /// them; empty without a replay.  Read by the fixed-cycle ledger only (its evidence,
    /// networkArLatticeEvidence).
    WithinEpochCulprits                              we_landed;
};

/**
 * @brief The clock datum's declaration the epoch's election reads
 *        (@ref NetworkSbasis::buildSharedDatum): the reference receiver, the self-described
 *        code-bias and phase-bias frames, and the clock gauge (broadcast-anchored,
 *        sum-to-zero, or a configured pivot satellite).
 *
 * One implementation, read by the election in @ref networkSolveGlue and by the handover
 * preview before PASS B (@ref networkHandoverStransformBeforeRows, card PRM phase 2), so the
 * two elect from the same declaration.
 * @param ctx              the run context (the knobs).
 * @param config_          the configuration (the network block).
 * @param clk_anchor_sigma the resolved broadcast anchor sigma (m).
 * @return the declaration.
 */
SbasisDeclaration networkClockDatumDeclaration(const NetworkRunContext& ctx,
                                               const GnssConfig& config_,
                                               double clk_anchor_sigma);

/**
 * @brief Run the datum build, solve and within-epoch RAIM adaptation for this epoch.
 * @see the file banner for the EpochAction and result-struct contract.  @p buildEnsembleGauges
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
    bool carry_warm,
    bool within_epoch_armed,
    double dt,
    double pl_now_s,
    double clk_anchor_sigma,
    const std::function<void()>& buildEnsembleGauges,
    const std::function<void()>& buildCarryDdPriors);

} // namespace GnssProcessor
