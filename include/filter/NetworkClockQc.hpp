#pragma once
/**
 * @file NetworkClockQc.hpp
 * @brief Post-solve satellite-clock quality control for the PPP-RTK network
 *        producer: the two-state oscillator model + fault test and the
 *        per-satellite carried-clock sanity monitor.
 *
 * Runs on the final (post-adaptation) network solution.  Two cooperating pieces:
 *
 *  1. **Two-state oscillator clock** (`network.clock_model`): update each
 *     satellite's (offset, drift) model from this epoch's solved offset and test
 *     the normalized innovation against the model's own tight prediction band, with
 *     the per-constellation median (the datum common mode) removed first so that a
 *     whole-constellation re-reference cancels.  A NIS-flagged satellite is added to
 *     @p clock_refloat_sats and its model is loosened, so it re-acquires toward the
 *     a-priori anchor instead of freezing.
 *  2. **Carried-clock sanity monitor** (`network.clock_monitor`, clock_model off):
 *     flag any satellite whose per-constellation de-meaned clock exceeds clk_mon_abs
 *     or whose one-epoch jump exceeds clk_mon_jump, and refloat its carried shared
 *     state.  This detects a single satellite whose clock locks onto a wrong value
 *     for hours, which the stiff sat-clock carry keeps invisible to the σ̂0 RAIM.
 *
 * It then calls @ref networkRetroUraBookkeeping (the retro-flag / URA-inflate feed),
 * and last runs the network-wide common-mode clock-excursion detector
 * (nc.cb_excursion_reinit, SPEC_day129_frame_guards Phase 2.1), which reinitializes
 * the pivot's code-bias carry when the median one-epoch clock change exceeds
 * nc.cb_excursion_median_m.
 *
 * Relocated from the SB_CLOCKUPD block of Application::runNetwork at decomposition
 * INC-10 (see docs/network_decomposition_plan.md §5, INC-10).  The Card-6 re-level,
 * a separate block in the emit region, is not part of this seam.  Owns no state:
 * every carry object is an explicit reference into the caller's NetworkRunContext.
 *
 * Ordering contract: this mutates persistent and epoch-local carries.  It updates
 * @c ctx.carry.clock_osc / prev_clk and the excursion detector's
 * cb_exc_* state, cb_excursion_sats and cb_excursion_pending_bump, and inserts
 * NIS/monitor culprits into @p clock_refloat_sats (consumed by the epoch stash,
 * which drops or loosens a refloated satellite's carried shared state) and
 * @p retro_flag_sats.  Call it on the post-fit @p x_s after the post-fit RAIM
 * (@ref networkPostFitRaim, whose consensus/attribution culprits arrive in
 * @p clock_refloat_sats) and before the epoch stash/status tail.  The caller keeps
 * the @c subBegin/subEnd(SB_CLOCKUPD) brackets.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator
#include "filter/NetworkRunTypes.hpp"    // EpochWorkspace / ClockOsc / ClockOscPred
#include "rinex/MultiRinexSync.hpp"      // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <Eigen/Core>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Run the post-solve satellite-clock QC block for this epoch.
 *
 * @param ctx                  [in/out] carry (clock_osc / prev_clk),
 *                             counters (fr_clk_model / fr_clk_mon / n_emitted) and
 *                             knobs.  Passed whole rather than as ~30 references, so a
 *                             missing `&` cannot silently discard a carry mutation;
 *                             the body opens with an alias preamble copied from the
 *                             former run-TU alias block.
 * @param config_             Reconstructs `nc = config_.getNetworkConfig()`
 *                             (trace_clock_full).
 * @param log                 "main_log": the clock-model / clock-monitor warnings.
 * @param traces              clk_mon_trace + the emitEvent forwarder.
 * @param ws                  EpochWorkspace: read-only source of clk_pred (this
 *                            epoch's two-state predictions).
 * @param se                  Current synced epoch (week/TOW for trace + log rows).
 * @param est                 sharedCovariance / clockColumn (both const).
 * @param x_s                 Post-fit shared solution vector.
 * @param registered_clock_sats  This epoch's registered satellite-clock keys.
 * @param datum_changed       Whether a pivot handover re-referenced the datum.
 * @param changed_consts      Constellations whose pivot changed (scoped wipe).
 * @param clk_model_warm      clock_model armed and past the warmup horizon.
 * @param pl_now_s            Continuous-GPS seconds of this epoch (per-sat drift Δt).
 * @param dt                  Nominal epoch interval (gap/contiguity fallback).
 * @param clock_refloat_sats  [in/out] holds the RAIM culprits on entry; the NIS and
 *                            monitor culprits are added (fed to the stash refloat).
 * @param retro_flag_sats     [in/out] retro-bump feed (networkRetroUraBookkeeping).
 * @param retro_feed          Whether the retro-bump path is armed this run.
 */
void networkClockQc(NetworkRunContext& ctx,
                    const GnssConfig& config_,
                    const std::shared_ptr<spdlog::logger>& log,
                    NetworkRunTraces& traces,
                    const EpochWorkspace& ws,
                    const SyncedEpoch& se,
                    const NetworkEstimator& est,
                    const Eigen::VectorXd& x_s,
                    const std::vector<std::string>& registered_clock_sats,
                    bool datum_changed,
                    const std::set<char>& changed_consts,
                    bool clk_model_warm,
                    double pl_now_s,
                    double dt,
                    std::set<std::string>& clock_refloat_sats,
                    std::set<std::string>& retro_flag_sats,
                    bool retro_feed);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the CB-excursion guard (Phase 2.1).
 *
 * The CB-excursion guard (Phase 2.1): trips, code-bias carry reinits and discontinuity
 * bumps.
 *
 * Gate: returns 0 at once unless `config_.getNetworkConfig().cb_excursion_reinit`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkCbExcursionRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
