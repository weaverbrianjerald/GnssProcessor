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
 *  2. **Result struct (INC-11pre).** The 17 loop-body locals that the block produces
 *     and the downstream seams (RAIM, ClockQc, SolPrep, EmitStore) consume cross the
 *     seam boundary in @ref NetworkSolveGlueResult (moved in at the end of the body);
 *     the caller re-aliases each with `auto& x = sg_.x;`, so downstream code is
 *     unchanged.
 *
 * The within-epoch re-solve rebuilds the DD-prior blocks by calling their builder
 * directly over the run and the epoch (networkBuildCarryDdPriors, card NOM3 step EP1);
 * the solve's own parameters name what that replay writes.  The seam owns no state;
 * every carry object is a reference into ctx.
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

/// What the post-fit tiers hand the epoch stash: seeded by the solve (the within-epoch tier's
/// refloats), extended by networkPostFitRaim and networkClockQc, read by networkStashStatus.
/// With prep.sol, the only epoch members a seam writes after the solve (card NOM3 step EP1).
struct NetworkPostFitHandoff {
    std::set<std::string> clock_refloat_sats;      ///< Satellites the stash refloats: the T2 and RAIM culprits, then the clock NIS and monitor culprits.
    std::set<std::string> retro_flag_sats;         ///< The retro-bump feed (networkRetroUraBookkeeping, inside networkClockQc).
    int                   n_arc_refloat_epoch{0};  ///< Isolated-arc refloats this epoch (the epoch-status row).
};

/**
 * @brief The 17 loop-body locals SB_SOLVEGLUE produces that downstream epoch seams
 *        consume, plus the epoch disposition.  On a SkipEpoch return the value fields
 *        are default (the caller ends the epoch before reading them).
 */
struct NetworkSolveGlueResult {
    Eigen::VectorXd                                  x_s;
    NetworkSbasis::Plan                              plan;
    std::set<char>                                   changed_consts;
    bool                                             datum_changed{false};
    bool                                             clk_model_warm{false};
    NetworkPostFitHandoff                            handoff;   ///< The post-fit hand-off (clock_refloat_sats, retro_flag_sats, n_arc_refloat_epoch), which RAIM and clock QC extend (card NOM3 step EP1).
    bool                                             retro_feed{false};
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

struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochDdBuild;            // app/NetworkEpoch.hpp (card NOM3 step EP1)
struct NetworkSlipFunnel;       // app/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief The datum build, the solve and the within-epoch RAIM adaptation of this epoch.
 * @see the file banner for the EpochAction and result-struct contract.  The within-epoch
 *      tier's arming and the resolved clock anchor sigma are read from @c ctx.knobs
 *      (within_epoch_armed, clk_anchor_sigma; card NOM3 step EP0).
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run: the estimator is folded and solved (and rebuilt by each T2 replay); ctx
 *            carry and counters, the traces, the setup's writer and consumer are written as
 *            before. Written through non-const bindings: run.ctx (ctx), run.traces (traces),
 *            run.setup (setup), run.engines.est (est). Read: run.config (config_), run.log
 *            (log).
 * @param ep The epoch, read: se, clock, pass_a, columns, assembly (asm_res, total_pins).
 *           Bindings: ep.se (se), ep.assembly.asm_res (asm_res), ep.pass_a.present (present),
 *           ep.pass_a.shared_phase_keys (shared_phase_keys), ep.columns.registered_clock_sats
 *           (registered_clock_sats), ep.columns.registered_code_keys (registered_code_keys),
 *           ep.columns.col_desc (col_desc), ep.columns.n_clock (n_clock), ep.columns.n_pb
 *           (n_pb), ep.columns.n_cb (n_cb), ep.columns.n_present (n_present),
 *           ep.assembly.total_pins (total_pins), ep.clock.carry_warm (carry_warm), ep.clock.dt
 *           (dt), ep.clock.pl_now_s (pl_now_s).
 * @param ws [in/out] The capture channel the replay re-folds, the clock maps, and the
 *     within-epoch culprit sets the T2 block adapts and clears (card WCF).
 * @param est_index_of [in/out] ep.assembly.est_index_of, rebuilt by each replay.
 * @param dd_build [in/out] ep.dd_build, rewritten by each replay's DD rebuild.
 * @param funnel [in/out] ep.funnel, which the DD rebuild declares writable (it only reads it;
 *     step EP1c makes both const and removes this parameter).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx.
 *   - @p config_ = run.config.
 *   - @p log = run.log.
 *   - @p traces = run.traces.
 *   - @p setup = run.setup.
 *   - @p est = run.engines.est.
 *   - @p ws = the output parameter itself (ep.ws at the spine line).
 *   - @p se = ep.se.
 *   - @p asm_res = ep.assembly.asm_res.
 *   - @p est_index_of = the output parameter itself (ep.assembly.est_index_of at the spine
 *     line).
 *   - @p present = ep.pass_a.present.
 *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys.
 *   - @p registered_clock_sats = ep.columns.registered_clock_sats.
 *   - @p registered_code_keys = ep.columns.registered_code_keys.
 *   - @p col_desc = ep.columns.col_desc.
 *   - @p n_clock = ep.columns.n_clock.
 *   - @p n_pb = ep.columns.n_pb.
 *   - @p n_cb = ep.columns.n_cb.
 *   - @p n_present = ep.columns.n_present.
 *   - @p total_pins = ep.assembly.total_pins.
 *   - @p carry_warm = ep.clock.carry_warm.
 *   - @p dt = ep.clock.dt.
 *   - @p pl_now_s = ep.clock.pl_now_s.
 *   - @p buildCarryDdPriors = (removed: direct call in the T2 replay).
 *
 * @return The solve's result; action is SkipEpoch at the five exits (bookkeeping done here).
 */
NetworkSolveGlueResult networkSolveGlue(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws,
                                        std::vector<int>& est_index_of, EpochDdBuild& dd_build,
                                        NetworkSlipFunnel& funnel);

} // namespace GnssProcessor
