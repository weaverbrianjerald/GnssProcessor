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

#include "filter/NetworkEstimator.hpp"      // NetworkEstimator + NetworkEstimator::Datum
#include "filter/NetworkPriorResidual.hpp"  // the constraint-aware global σ̂₀ data-side feed
#include "filter/NetworkRunTypes.hpp"       // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"         // MultiRinexSync::SyncedEpoch

#include <Eigen/Core>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor { struct RawSlipEpoch; }   // slip/NetworkRawObservables.hpp

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Run-end telemetry for the RAIM POOL RE-HOST (docs/SPEC_raim_pool_rehost.md).
 *
 * Standing invariant #4: run-end counters are the ON-path telemetry, and this card
 * exists precisely because a tier read ARMED in the log for months while contributing
 * nothing.  The DENOMINATORS are therefore the point — they make four states
 * distinguishable that all previously looked like "RAIM found nothing":
 *
 *  - @c epochs == 0                      → the seam never ran (call-site gate).
 *  - @c epochs > 0, @c pool_cells == 0   → it ran and the census was EMPTY (dead wiring —
 *                                          the exact defect this card fixes).
 *  - @c pool_cells > 0, @c sats_flagged == 0
 *                                        → it screened real residuals and nothing was bad
 *                                          (zero opportunity, not zero capability).
 *  - @c sats_flagged > 0, @c refloats_queued == 0
 *                                        → it flagged but the actuator is disarmed.
 *
 * Every member stays at zero unless @c ctx.knobs.raim_ci_on, so an OFF run allocates
 * nothing and reports nothing.  Modelled on @c NetworkIonoDwReleaseTallies.
 */
struct NetworkRaimCiTallies {
    long long epochs = 0;            ///< Seam passes with the re-hosted pool armed.
    long long pool_stations = 0;     ///< Station-epochs whose qc_amb census was non-empty.
    long long pool_cells = 0;        ///< Total carried-ambiguity cells screened (the census size).
    long long sats_flagged = 0;      ///< Consensus-flagged satellite-epochs.
    long long arcs_flagged = 0;      ///< Attribution-flagged isolated (station,sat) arc-epochs.
    long long refloats_queued = 0;   ///< Arcs queued into raim_refloat_pending (actuator armed only).
    long long refloats_drained = 0;  ///< Queued arcs actually delivered into the C3c funnel next epoch.
    double    sigma0_max = 0.0;      ///< Largest σ̂0 seen — the number that was never measurable before.
    double    sigma0_sum = 0.0;      ///< Σ σ̂0 over epochs with a live pool (mean = sigma0_sum / sigma0_n).
    long long sigma0_n = 0;          ///< Epochs contributing to sigma0_sum.
};

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
 * @param prires             [in/out] the prior-residual census accumulator, here
 *                           ONLY as the DATA-SIDE feed of the constraint-aware
 *                           global variance component
 *                           (docs/SPEC_prior_residual_census.md §14): the Pass-1
 *                           row walk already holds every row's @c (w, v, r_i), so
 *                           the feed is a multiply and two adds inside a loop
 *                           that already runs.  Inert unless
 *                           @c network.prior_residual_global_sigma0 is armed, and
 *                           MEASURE-ONLY in every case — this seam's decisions
 *                           (the σ̂0 pool, the consensus, the attribution, every
 *                           refloat) read NOTHING back from it.  Ordering: the
 *                           accumulator is drained in SB_STASH_STATUS, which runs
 *                           after this seam, so the epoch's data side is complete
 *                           when the column is written.
 * @param slip_ep            THIS epoch's pre-solve slip-QC raw-observable evidence
 *                           (nullable).  Consulted ONLY by the corroborated retap
 *                           (network.cycle_slip.relevel_retap_corroborate): the
 *                           keep branch requires the (station, sat) to be measured
 *                           AND not stepped — the same tri-state the D5-v3
 *                           corroboration gate consumes (SPEC_lane_slip_response
 *                           §3).  nullptr / !computed means no positive evidence
 *                           exists, so every corroborated keep refuses to the
 *                           erase (standing invariant #1: a slip response is never
 *                           suppressed on ABSENCE of evidence).
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
                        int& n_arc_refloat_epoch,
                        NetworkPriorResidual& prires,
                        const RawSlipEpoch* slip_ep = nullptr);

} // namespace GnssProcessor
