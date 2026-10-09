#pragma once
/**
 * @file NetworkRaim.hpp
 * @brief Post-fit network RAIM (Receiver Autonomous Integrity Monitoring) for the
 *        PPP-RTK network producer.
 *
 * After the joint network solve, each station is reconstructed once, its post-fit
 * carrier-phase residuals are formed, and they are standardized by a robust
 * network-wide a-posteriori variance factor σ̂0.  A per-satellite cross-station
 * consensus refloats the offending carried ambiguities network-wide: a satellite
 * whose normalized residual spikes at ≥ @c network.raim_min_stations stations has a
 * slip the pre-processor missed leaking into its shared clock.  A robust 2-way
 * median polish of the (station × satellite) residual table optionally attributes
 * the misfit to a bad satellite (column effect), a bad station (row effect; never
 * acted on, see the body) or an isolated arc (interaction).  With
 * @c network.raim_attribution on, the attribution drives the refloat instead of the
 * consensus.  σ̂0 folds into the URA EWMA (B1) and feeds the retro-bump /
 * clock-refloat coupling.
 *
 * The seam holds the former SB_RAIM block, moved out of Application::runNetwork at
 * decomposition INC-9 (docs/network_decomposition_plan.md §5, INC-9).  It covers
 * only the post-fit pass: the within-epoch RAIM adaptation (the σ̂0-triggered
 * culprit drop and re-solve, and the per-constellation withhold) runs in the run
 * TU's solve region, before this seam.  The seam owns no state: every carry object
 * is an explicit reference into the caller's NetworkRunContext.
 *
 * Ordering contract.  This function mutates persistent and epoch-local carries.  It
 * erases @c ctx.carry.local_carry entries and marks StateManager ambiguities
 * pending-reseed for every refloated (station,sat,σ); inserts consensus / whole-sat
 * culprits into @c clock_refloat_sats and @c retro_flag_sats (consumed downstream by
 * the SB_CLOCKUPD two-state clock update and retro-flag feed); folds σ̂0 into
 * @c ctx.carry.ura_sigma0_ewma (read one epoch later at the emit site) and advances
 * @c ctx.carry.arc_spike_streak / @c n_arc_refloat_epoch.  It therefore runs:
 *   - after the joint solve and carry harvest (it operates on the post-fit @p x_s and
 *     the just-restashed @c local_carry; erasing an entry is the refloat);
 *   - before SB_CLOCKUPD (the only consumer of @p clock_refloat_sats /
 *     @p retro_flag_sats filled here) and the epoch stash/status tail.
 * The caller keeps the @c subBegin/subEnd(SB_RAIM) brackets and the arming guard
 * `if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))`; this function is the
 * guarded body.
 */

#include "filter/NetworkContributions.hpp"  // ContributionKind, kContributionKinds (RaimPoolAdmissionTally)
#include "filter/NetworkEstimator.hpp"      // NetworkEstimator + NetworkEstimator::Datum
#include "filter/NetworkPriorResidual.hpp"  // the constraint-aware global σ̂₀ data-side feed
#include "filter/NetworkRunTypes.hpp"       // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"         // MultiRinexSync::SyncedEpoch

#include <Eigen/Core>

#include <array>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor { struct RawSlipEpoch; }   // slip/NetworkRawObservables.hpp

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Run-end telemetry for the RAIM pool re-host (docs/SPEC_raim_pool_rehost.md).
 *
 * Run-end counters are the on-path telemetry (standing invariant #4).  The
 * denominators separate four outcomes that would otherwise all read as "RAIM found
 * nothing":
 *
 *  - @c epochs == 0                      → the seam never ran (call-site gate).
 *  - @c epochs > 0, @c pool_cells == 0   → it ran and the census was empty (dead
 *                                          wiring, the defect the re-host fixes).
 *  - @c pool_cells > 0, @c sats_flagged == 0
 *                                        → it screened real residuals and nothing was bad
 *                                          (zero opportunity, not zero capability).
 *  - @c sats_flagged > 0, @c refloats_queued == 0
 *                                        → it flagged but the actuator is disarmed.
 *
 * Every member stays at zero unless @c ctx.knobs.raim_ci_on, and the run-end readout
 * is written only then, so an off run reports nothing.  Modelled on
 * @c NetworkIonoDwReleaseTallies.
 */
struct NetworkRaimCiTallies {
    long long epochs = 0;            ///< Seam passes with the re-hosted pool armed.
    long long pool_stations = 0;     ///< Station-epochs whose qc_amb census was non-empty.
    long long pool_cells = 0;        ///< Total carried-ambiguity cells screened (the census size).
    long long sats_flagged = 0;      ///< Consensus-flagged satellite-epochs.
    long long arcs_flagged = 0;      ///< Attribution-flagged isolated (station,sat) arc-epochs.
    long long refloats_queued = 0;   ///< Arcs queued into raim_refloat_pending (actuator armed only).
    long long refloats_drained = 0;  ///< Queued arcs delivered into the C3c funnel at the next epoch's head.
    double    sigma0_max = 0.0;      ///< Largest σ̂0 seen over epochs with a non-empty pool.
    double    sigma0_sum = 0.0;      ///< Σ σ̂0 over epochs with a non-empty pool (mean = sigma0_sum / sigma0_n).
    long long sigma0_n = 0;          ///< Epochs contributing to sigma0_sum.
};

/**
 * @brief The admissions of one carried-phase RAIM pool over a run, per contribution
 *        kind (card NOM step N2b; the instrumentation gap of card NOM section 16).
 *
 * The admission rule itself is a statement about row kinds and lives with the row
 * classifier (filter/NetworkRowClassify.hpp, @c raimPoolAdmitsKind); this counter of what
 * the rule admitted is RAIM telemetry and lives here.  The post-fit pool
 * (@ref networkPostFitRaim) and the within-epoch assess pool (NetworkSolveGlue.cpp) each
 * keep one in @c NetworkRunContext::counters, read at run end (NetworkTeardown.cpp).
 *
 * A pool walks the rows of each station once per ambiguity column of its pool, so a row
 * that clears 0.5 on two columns is visited twice and enters the pool's sample twice:
 * @c visits counts every (row, column) admission, @c rows each admitted row once per
 * station-epoch (per walk).  @c refused counts the (row, column) visits whose
 * coefficient cleared 0.5 but whose kind the rule does not admit, by kind (@c Unset in
 * the last slot); 0 is expected, and a non-zero count is a row the coefficient test alone
 * would have admitted.
 */
struct RaimPoolAdmissionTally {
    long long walks{0};  ///< Pool walks: post-fit epochs, or within-epoch assess calls.
    std::array<long long, kContributionKinds + 1> visits{};   ///< Admitted (row, column) visits, by kind.
    std::array<long long, kContributionKinds + 1> rows{};     ///< Admitted rows, each once per station-epoch walk, by kind.
    std::array<long long, kContributionKinds + 1> refused{};  ///< Visits refused by the kind rule, by kind (@c Unset last).

    /**
     * @brief Count one (row, column) visit that cleared 0.5.
     * @param k        The row's kind.
     * @param admitted Whether @ref raimPoolAdmitsKind admitted it.
     * @param first    Whether this is the row's first admitted visit of this station's walk.
     */
    void note(ContributionKind k, bool admitted, bool first);

    /**
     * @brief The run-end text: the admitted kinds with their visits and rows, the refused
     *        visits by kind.
     * @return One line, without a prefix: "walks N; admitted: <kind> visits V rows R, ...;
     *         refused by kind: none" (or the refused kinds with their visits).
     */
    std::string text() const;
};

/**
 * @brief Outcome of the band-scoped T4 erase decision for one cell (§Z).
 *
 * See docs/SPEC_event_harness.md §Z.  @c act false means there is no per-band
 * evidence for this (station, satellite) this epoch; the caller then erases the whole
 * arc as an unscoped run does, because refusing the response on missing evidence is
 * the other failure mode (standing invariant #1).
 */
struct RaimEraseBandScope {
    bool        act{false};            ///< The scope decided this cell.
    bool        single_band{false};    ///< One signal only: @ref erase is empty and the caller skips the erase.
    bool        kept_over_gate{false}; ///< Every band stood above the gate, so @ref keep is the least-bad band, not a clean one.
    std::string keep;                  ///< The signal left pinned as the line of sight's level anchor.
    std::set<std::string> erase;       ///< The signals to free.
};

/**
 * @brief Decide which bands of one T4-flagged cell to erase (§Z).  Pure function.
 *
 * Study 1 (docs/SPEC_event_harness.md §9) measured that the ~0.445-cycle level
 * floor a T4 attribution erase leaves on a healthy arc comes from the band scope
 * alone, not from anything else on the erase path: the same erase restricted to 2
 * of 3 bands leaves 0.050 cycles on every band, including the one not erased.
 * Freeing all bands removes the line of sight's last level anchor, so (ρ, I, N₁..N₃)
 * are re-drawn from one epoch of code, and the station-common iono error that lands
 * in the fresh zero-q level persists (the re-coupling edge of invariant #1).
 *
 * Rule: erase every band whose own post-fit normalized residual exceeds the T4
 * gate, and always keep the band with the smallest |r| (ties go to the lexically
 * lowest signal, so the choice is deterministic).  Two edge cases are deliberate:
 *  - **No band over the gate.**  The cell was flagged by the median-polish
 *    interaction, which is net of the row and column effects, so a per-signal
 *    residual can sit under k·σ̂₀ while the arc still stands out.  Every band except
 *    the pin is erased; a slip response is not suppressed for lack of per-band
 *    evidence.
 *  - **Every band over the gate.**  The pin is then the least-bad band;
 *    @ref RaimEraseBandScope::kept_over_gate marks it, so the cells that keep a
 *    suspect level are visible (invariant #7).
 *
 * @param sig_r One (signal, |r|) pair per carried ambiguity of the cell: the Pass-1
 *              attribution cells before @c cell_max collapses the signal axis.
 *              Empty ⇒ @c act false.
 * @param bar   The T4 gate in the same units as @p sig_r (k·σ̂₀).
 * @return The decision.
 */
RaimEraseBandScope networkRaimEraseBandScope(
    const std::vector<std::pair<std::string, double>>& sig_r, double bar);

/**
 * @brief Run the post-fit network RAIM pass for this epoch (Pass 1 residual scan,
 *        Pass 2 consensus, median-polish attribution, and the resulting refloats).
 *
 * @param ctx                [in/out] carry + counters + knobs.  The whole context is
 *                           passed instead of ~24 individual references because the
 *                           body mutates local_carry / ura_sigma0_ewma /
 *                           arc_spike_streak by value: one missing `&` in an
 *                           enumerated parameter list would compile and discard a
 *                           refloat or the URA fold without any diagnostic.  The body
 *                           opens with an alias preamble copied byte for byte from
 *                           the former run-TU alias block.
 * @param config_            RAIM getters, among them networkRaimAttribution /
 *                           networkRaimSigma0Trigger / networkRaimWithinEpoch.
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
 * @param shared_datum       The shared hard-pin datum (Baarda redundancy input).  The
 *                           run TU passes `plan.shared_datum`; this is the only
 *                           substitution relative to the moved body.
 * @param clock_refloat_sats [in/out] consensus / whole-sat culprits whose shared
 *                           clock also refloats (fed to SB_CLOCKUPD).
 * @param retro_flag_sats    [in/out] culprits whose emitted correction was
 *                           contaminated (retro-bump feed, SB_CLOCKUPD).
 * @param retro_feed         Whether the retro-bump path is armed this run
 *                           (ura_retro || ura_refloat_inflate > 1.0).
 * @param n_arc_refloat_epoch [in/out] this epoch's isolated-arc refloat count
 *                           (heartbeat churn metric, read later in the epoch).
 * @param prires             [in/out] the prior-residual census accumulator, used here
 *                           only as the data-side feed of the constraint-aware
 *                           global variance component
 *                           (docs/SPEC_prior_residual_census.md §14).  The Pass-1
 *                           row walk already holds every row's @c (w, v, r_i), so
 *                           the feed costs a multiply and two adds inside an
 *                           existing loop.  Inert unless
 *                           @c network.prior_residual_global_sigma0 is armed, and
 *                           measure-only in every case: none of this seam's
 *                           decisions (the σ̂0 pool, the consensus, the attribution,
 *                           the refloats) read from it.  The accumulator is drained
 *                           in SB_STASH_STATUS, which runs after this seam, so the
 *                           epoch's data side is complete when the column is written.
 * @param slip_ep            This epoch's pre-solve slip-QC raw-observable evidence
 *                           (nullable).  Read only by the corroborated retap
 *                           (network.cycle_slip.relevel_retap_corroborate): its keep
 *                           branch requires the (station, sat) to be measured and
 *                           not stepped, the same tri-state the D5-v3 corroboration
 *                           gate consumes (SPEC_lane_slip_response §3).  nullptr or
 *                           !computed means there is no positive evidence, so every
 *                           corroborated keep falls back to the erase (standing
 *                           invariant #1: a slip response is not suppressed for lack
 *                           of evidence).
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
