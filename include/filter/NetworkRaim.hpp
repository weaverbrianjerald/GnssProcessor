#pragma once
/**
 * @file NetworkRaim.hpp
 * @brief Post-fit network RAIM (Receiver Autonomous Integrity Monitoring) for the
 *        PPP-RTK network producer.
 *
 * After the joint network solve, reconstruct each station once, form its post-fit
 * carrier-phase residuals, and standardize them by a robust network-wide
 * a-posteriori variance factor σ̂0.  A per-satellite CROSS-STATION CONSENSUS
 * (a satellite whose normalized residual spikes at ≥ @c network.raim_min_stations
 * stations leaked a pre-processor-missed slip into its SHARED clock) refloats the
 * offending carried ambiguities network-wide.  A robust 2-way MEDIAN POLISH of the
 * (station × satellite) residual table optionally ATTRIBUTES the misfit to a bad
 * satellite (column effect), a bad station (row effect — deliberately never acted,
 * see the body) or an isolated arc (interaction), and — when
 * @c network.raim_attribution is on — drives the refloat instead of the consensus.
 * σ̂0 folds into the URA EWMA (B1) and feeds the retro-bump / clock-refloat coupling.
 *
 * Seeded at decomposition INC-9 (2026-07-22), which relocated the SB_RAIM block
 * VERBATIM out of Application::runNetwork (see docs/network_decomposition_plan.md
 * §5, INC-9).  The WITHIN-EPOCH RAIM adaptation (the σ̂0-triggered culprit-drop +
 * re-solve, and the per-const withhold) stays in the run TU's solve region for now
 * — it is entangled with the still-uncommitted solve-region cards; this seam owns
 * only the POST-FIT pass.  Owns NO state: every carry object is an explicit
 * reference into the caller's NetworkRunContext.
 *
 * ⚠ ORDERING CONTRACT — this MUTATES persistent + epoch-local carries.  It erases
 * @c ctx.carry.local_carry entries and marks StateManager ambiguities
 * pending-reseed for every refloated (station,sat,σ); inserts consensus / whole-sat
 * culprits into @c clock_refloat_sats and @c retro_flag_sats (consumed downstream by
 * the SB_CLOCKUPD two-state clock update + retro-flag feed); folds σ̂0 into
 * @c ctx.carry.ura_sigma0_ewma (read one epoch later at the emit site) and advances
 * @c ctx.carry.arc_spike_streak / @c n_arc_refloat_epoch.  Therefore it MUST run:
 *   - AFTER  the joint solve + carry harvest (it operates on the post-fit @p x_s and
 *     the just-restashed @c local_carry — erasing an entry is the refloat);
 *   - BEFORE SB_CLOCKUPD (the sole consumer of @p clock_refloat_sats /
 *     @p retro_flag_sats populated here) and the epoch stash/status tail.
 * The caller keeps the @c subBegin/subEnd(SB_RAIM) brackets and the
 * `if (raim_on && carry_warm)` arming guard; this function is the guarded body.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator + NetworkEstimator::Datum
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"      // MultiRinexSync::SyncedEpoch

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
 * @brief Run the post-fit network RAIM pass for this epoch (Pass 1 residual scan +
 *        Pass 2 consensus + median-polish attribution + the resulting refloats).
 *
 * @param ctx                [in/out] carry + counters + knobs.  Taken as the whole
 *                           context rather than ~24 individual references on
 *                           purpose: the body value-mutates local_carry /
 *                           ura_sigma0_ewma / arc_spike_streak, so a single dropped
 *                           `&` in an enumerated parameter list would compile clean
 *                           and silently discard a refloat or the URA fold.  The
 *                           body opens with an alias preamble that is a byte-copy of
 *                           the former run-TU alias block.
 * @param config_            networkRaimAttribution / networkRaimSigma0Trigger /
 *                           networkRaimWithinEpoch.
 * @param log                "main_log": the refloat summary + within-epoch warning.
 * @param traces             raim_trace / raim_attrib_trace / station_res_trace /
 *                           events_trace + the emitEvent forwarder.
 * @param se                 Current synced epoch (week/TOW for the trace + log rows).
 * @param stations           [in/out] StateManager markAmbiguityPendingReseed on
 *                           refloat; name / is_pivot reads.
 * @param asm_res            Per-station assembly (pl_amb / A_s / A_r / y / w).
 * @param est_index_of       Station → reduced-system index (< 0 = absent).
 * @param present            This epoch's presence mask (attribution refloat scope).
 * @param x_s                Post-fit shared solution vector.
 * @param est                reconstructStation / rowRedundancy (both const).
 * @param shared_datum       The shared hard-pin datum (Baarda redundancy input;
 *                           was `plan.shared_datum` in the run TU — the sole
 *                           substitution vs the verbatim body).
 * @param clock_refloat_sats [in/out] consensus / whole-sat culprits whose SHARED
 *                           clock also refloats (fed to SB_CLOCKUPD).
 * @param retro_flag_sats    [in/out] culprits whose emitted correction was
 *                           contaminated (retro-bump feed, SB_CLOCKUPD).
 * @param retro_feed         Whether the retro-bump path is armed this run
 *                           (ura_retro || ura_refloat_inflate > 1.0).
 * @param n_arc_refloat_epoch [in/out] this epoch's isolated-arc refloat count
 *                           (heartbeat churn metric, read later in the epoch).
 */
void networkPostFitRaim(NetworkRunContext& ctx,
                        const GnssConfig& config_,
                        const std::shared_ptr<spdlog::logger>& log,
                        NetworkRunTraces& traces,
                        const SyncedEpoch& se,
                        std::vector<NetStation>& stations,
                        const std::vector<StationAssembly>& asm_res,
                        const std::vector<int>& est_index_of,
                        const std::vector<bool>& present,
                        const Eigen::VectorXd& x_s,
                        const NetworkEstimator& est,
                        const NetworkEstimator::Datum& shared_datum,
                        std::set<std::string>& clock_refloat_sats,
                        std::set<std::string>& retro_flag_sats,
                        bool retro_feed,
                        int& n_arc_refloat_epoch);

} // namespace GnssProcessor
