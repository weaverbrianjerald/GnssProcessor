#pragma once
/**
 * @file NetworkClockQc.hpp
 * @brief Post-solve satellite-clock quality control for the PPP-RTK network
 *        producer: the two-state oscillator model + fault test, the decoupled
 *        emit-clock-rate (C1) estimator, and the per-satellite carried-clock
 *        sanity monitor.
 *
 * Runs on the FINAL (post-adaptation) network solution.  Three cooperating pieces:
 *
 *  1. **Two-state oscillator clock** (`network.clock_model`): update each
 *     satellite's (offset, drift) model from this epoch's solved offset and test
 *     the normalized innovation against the model's own tight prediction band, with
 *     the per-constellation MEDIAN (datum common-mode) removed first so a whole-
 *     constellation re-reference is gauged out.  A NIS-flagged satellite is added to
 *     @p clock_refloat_sats and its model loosened so it re-acquires toward the
 *     a-priori anchor instead of freezing.
 *  2. **Emit-clock-rate C1** (`network.emit_clock_rate`, clock_model OFF): finite-
 *     difference the posterior clock to derive the emitted C1 WITHOUT injecting any
 *     prior or driving a refloat (network solution byte-identical to OFF).
 *  3. **Carried-clock sanity monitor** (`network.clock_monitor`, clock_model OFF):
 *     flag any satellite whose per-constellation de-meaned clock exceeds clk_mon_abs
 *     or whose one-epoch jump exceeds clk_mon_jump, and refloat its carried shared
 *     state — the missing detector for the single-satellite multi-hour lock-in the
 *     stiff sat-clock carry hides from the σ̂0 RAIM.
 *
 * Closes with @ref networkRetroUraBookkeeping (the retro-flag / URA-inflate feed).
 *
 * Seeded at decomposition INC-10 (2026-07-22), which relocated the SB_CLOCKUPD block
 * VERBATIM out of Application::runNetwork (see docs/network_decomposition_plan.md
 * §5, INC-10).  The Card-6 re-level (a SEPARATE block in the emit region) is NOT part
 * of this cut.  Owns NO state: every carry object is an explicit reference into the
 * caller's NetworkRunContext.
 *
 * ⚠ ORDERING CONTRACT — this MUTATES persistent + epoch-local carries.  It updates
 * @c ctx.carry.clock_osc / clock_rate_est / prev_clk, and inserts NIS/monitor
 * culprits into @p clock_refloat_sats (consumed by the epoch stash — a refloated
 * sat's carried shared state is dropped/loosened) and @p retro_flag_sats.  Runs on
 * the post-fit @p x_s AFTER the post-fit RAIM (@ref networkPostFitRaim — whose
 * consensus/attribution culprits it reads via @p clock_refloat_sats) and BEFORE the
 * epoch stash/status tail.  The caller keeps the @c subBegin/subEnd(SB_CLOCKUPD)
 * brackets.
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Run the post-solve satellite-clock QC block for this epoch.
 *
 * @param ctx                  [in/out] carry (clock_osc / clock_rate_est / prev_clk),
 *                             counters (fr_clk_model / fr_clk_mon / n_emitted) and
 *                             knobs.  Taken whole (not ~30 references) so a dropped
 *                             `&` cannot silently drop a carry mutation; the body
 *                             opens with an alias preamble that byte-copies the
 *                             former run-TU alias block.
 * @param config_             Reconstructs `nc = config_.getNetworkConfig()`
 *                             (trace_clock_full).
 * @param log                 "main_log": the clock-model / clock-monitor warnings.
 * @param traces              clk_mon_trace + the emitEvent forwarder.
 * @param ws                  EpochWorkspace — read-only source of clk_pred (this
 *                            epoch's two-state predictions).
 * @param se                  Current synced epoch (week/TOW for trace + log rows).
 * @param est                 sharedCovariance / clockColumn (both const).
 * @param x_s                 Post-fit shared solution vector.
 * @param registered_clock_sats  This epoch's registered satellite-clock keys.
 * @param datum_changed       Whether a pivot handover re-referenced the datum.
 * @param changed_consts      Constellations whose pivot changed (scoped wipe).
 * @param clk_model_warm      clock_model armed AND past the warmup horizon.
 * @param emit_rate_warm      emit_clock_rate armed AND past the warmup horizon.
 * @param pl_now_s            Continuous-GPS seconds of this epoch (per-sat drift Δt).
 * @param dt                  Nominal epoch interval (gap/contiguity fallback).
 * @param clock_refloat_sats  [in/out] carries the RAIM culprits IN, adds the NIS /
 *                            monitor culprits (fed to the stash refloat).
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
                    bool emit_rate_warm,
                    double pl_now_s,
                    double dt,
                    std::set<std::string>& clock_refloat_sats,
                    std::set<std::string>& retro_flag_sats,
                    bool retro_feed);

} // namespace GnssProcessor
