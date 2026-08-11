#pragma once
/**
 * @file NetworkSolPrep.hpp
 * @brief Emit-side solution preparation + discontinuity signalling for the PPP-RTK
 *        network producer — the FRONT half of the emit pipeline (the back half,
 *        store push + product write + closed-loop, stays in SB_EMITSTORE).
 *
 * Maps the post-fit shared estimate to a @ref NetworkSolutionEpoch and prepares it
 * for emission:
 *   - build @c sol via @c est.toSolutionEpoch; stamp per-sat C1 drift (two-state
 *     clock_model or the decoupled emit_clock_rate), generation time/id;
 *   - RT0-5 clock-runaway quarantine + RTA-10 per-constellation σ̂0 carve-out
 *     (prune withheld constellations from @c sol.sats);
 *   - B1 per-sat URA (+ RTB-7 cold-start quality ramp) and B1b per-(sat,signal)
 *     phase-bias σ;
 *   - datum-bump + PIVOT events, the retro-bump / eph-cutover / basis-relatch /
 *     IF-pin-reselect / CB-reseed per-sat DISCONTINUITY landings (all emit-only,
 *     replay-faithful present/pending splits), the prev_pivot/last_pivot +
 *     emitted_anchor emit-baseline advance, the deferred v2 contract-writer open,
 *     and the M4 combined-correction σ (sigc/sigcc/rho).
 *
 * Seeded at decomposition INC-11pre (2026-07-22), which relocated the SB_SOLPREP
 * block VERBATIM out of Application::runNetwork (dedent-only, zero substitution).
 * Owns NO state: every carry / emit-side object is an explicit reference into the
 * caller's NetworkRunContext / NetworkSetupResult.
 *
 * ⚠ CROSSING OUTPUTS — unlike the self-contained QC seams, this block PRODUCES
 * five locals that SB_EMITSTORE (and the epoch heartbeat) consume, returned in
 * @ref NetworkSolPrepResult: @c sol (emitToStore + product write), the withheld-
 * constellation set (heartbeat), the datum-bump serialization (global_bump +
 * serialized_consts, product write), and the applied retro-bump set (heartbeat).
 *
 * ⚠ ORDERING CONTRACT — runs on the post-fit @p x_s AFTER SB_CLOCKUPD (whose
 * @c clock_osc / clock_rate_est drift it publishes as C1) and BEFORE SB_EMITSTORE
 * (which consumes the returned @ref NetworkSolPrepResult).  It MUTATES persistent
 * carries (the disc-pending sets, prev_pivot/last_pivot, emitted_anchor,
 * last_if_pin_sel, ura_retro_sats, the RTB-7 ramp state) and the emit-side objects
 * (@c emitter datum/disc bumps; opens @c product_writer at first emit).  The caller
 * keeps the @c subBegin/subEnd(SB_SOLPREP) brackets.
 */

#include "app/NetworkSetupResult.hpp"        // NetworkSetupResult
#include "filter/NetworkEstimator.hpp"       // NetworkEstimator + PerConstSigma0
#include "filter/NetworkSbasis.hpp"          // NetworkSbasis::Plan
#include "products/NetworkProductEmitter.hpp"// NetworkSolutionEpoch
#include "rinex/MultiRinexSync.hpp"          // SyncedEpoch

#include <Eigen/Core>

#include <map>
#include <memory>
#include <set>
#include <string>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief The five SB_SOLPREP locals SB_EMITSTORE + the heartbeat consume.
 *
 * Returned by value (moved out of the seam's body-locals); the caller re-aliases
 * each member so the unchanged downstream SB_EMITSTORE code reads the old names.
 */
struct NetworkSolPrepResult {
    NetworkSolutionEpoch     sol;                   ///< The prepared solution epoch (post prune/URA/σ stamps).
    std::set<char>           epoch_withheld_consts; ///< RT0-5 / RTA-10 withheld constellations (H heartbeat).
    bool                     global_bump = false;   ///< A global (non-scoped) datum bump this emit.
    std::set<char>           serialized_consts;     ///< The per-constellation datum-bump set (product write).
    std::set<std::string>    retro_emit_sats;       ///< Per-sat disc bumps that LANDED this emit — B1 retro plus the
                                                    ///< folded queue marks (eph-cutover / CB-excursion / clock-anchor /
                                                    ///< basis-relatch / IF-pin reselect / CB-reseed), serialized as the
                                                    ///< R record (replay) + heartbeat.
};

/**
 * @brief Prepare + stamp this epoch's emitted solution and land the emit-only
 *        discontinuity signals.  See the file banner for the full step list and the
 *        ordering contract.
 *
 * @param ctx                  [in/out] carry (disc-pending sets, pivots, anchors,
 *                             URA/ramp state) + counters (iod, generation_id,
 *                             n_emitted, runaway/perconst tallies) + knobs.  Taken
 *                             whole; the body opens with an alias preamble that
 *                             byte-copies the former run-TU alias block so a dropped
 *                             `&` cannot silently drop a carry/emit mutation.
 * @param config_             networkEmitPhaseBiasSigma / networkEmitCombinedSigma +
 *                             nc = getNetworkConfig() (warmup_ura_floor_m/epochs).
 * @param log                 "main_log": generation-id / runaway / withhold /
 *                             contract / anchor-handover lines.
 * @param traces              events_trace + the emitEvent forwarder.
 * @param setup               [in/out] emit-side objects: emitter (datum/disc bumps),
 *                             product_writer (opened at first emit), prod_path,
 *                             pending_contract, contract_pending, brdc_rts.
 * @param se                  Current synced epoch (content time / week / TOW).
 * @param est                 toSolutionEpoch / sharedCovariance / *Column /
 *                             zeroPhaseGaugeColumns (all const).
 * @param x_s                 Post-fit shared solution vector.
 * @param plan                Shared datum (pins) + per-constellation clock pivots.
 * @param pl_now_s            Continuous-GPS seconds of this epoch (per-sat C1 Δt,
 *                             ramp / contract timing).
 * @param datum_changed       Whether a pivot handover re-referenced the datum.
 * @param changed_consts      Constellations whose pivot changed (scoped bump).
 * @param sigma0_adapt        This epoch's final within-epoch σ̂0 (0 = did not run).
 * @param perconst_active     A publishable per-constellation subset exists.
 * @param perconst_publish    Constellations to keep in the emission.
 * @param perconst_dec_withhold  Constellations the σ̂0 decision withheld.
 * @param perconst_stats      Per-constellation σ̂0 stats (events / log).
 * @return the five crossing locals (see @ref NetworkSolPrepResult).
 */
NetworkSolPrepResult networkSolPrep(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    const NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    double pl_now_s,
    bool datum_changed,
    const std::set<char>& changed_consts,
    double sigma0_adapt,
    bool perconst_active,
    const std::set<char>& perconst_publish,
    const std::set<char>& perconst_dec_withhold,
    const std::map<char, NetworkEstimator::PerConstSigma0>& perconst_stats);

} // namespace GnssProcessor
