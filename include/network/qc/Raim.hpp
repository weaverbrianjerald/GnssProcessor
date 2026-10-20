#pragma once
/**
 * @file Raim.hpp
 * @brief Post-fit network RAIM (Receiver Autonomous Integrity Monitoring) for the
 *        PPP-RTK network producer.
 *
 * After the joint network solve, each station is reconstructed once and its post-fit
 * carrier-phase residuals are standardized by a robust network-wide a-posteriori
 * variance factor σ̂0.  Two screens then look for slips the pre-processor missed:
 *  - Consensus: a satellite whose normalized residual spikes at
 *    @c network.raim_min_stations or more stations has a slip leaking into its shared
 *    clock.  Its carried ambiguities are refloated network-wide.
 *  - Attribution (@c network.raim_attribution): a robust two-way median polish of the
 *    (station x satellite) residual table assigns the misfit to a bad satellite (column
 *    effect), a bad station (row effect, never acted on; see Raim.cpp) or an isolated
 *    arc (interaction).  When on, the attribution drives the refloat instead of the
 *    consensus.
 *
 * σ̂0 is also folded into an exponentially weighted moving average (EWMA) that scales
 * the per-satellite URA (user range accuracy) quality channel.  The satellites the
 * screens flag feed the retro-bump, when armed (the consumer resets the ambiguity that
 * integrated the contaminated epoch), and, with
 * @c network.clock_monitor_couple_raim, the clock refloat.
 *
 * This file covers only the post-fit pass.  The within-epoch RAIM adaptation (the
 * σ̂0-triggered culprit drop and re-solve, and the per-constellation withhold) runs
 * earlier, inside the solve (RaimWithinEpoch.hpp).  The pass owns no state: every carry
 * object is a reference into the run's NetworkRunContext.
 *
 * Ordering contract.  networkPostFitRaim mutates persistent and per-epoch state:
 *  - It erases entries of @c ctx.entities.arcs and marks the matching parameter-set
 *    ambiguities pending-reseed, for every refloated (station, satellite, signal).
 *  - It inserts consensus and whole-satellite culprits into @c clock_refloat_sats and
 *    @c retro_flag_sats of the post-fit hand-off.
 *  - It folds σ̂0 into @c ctx.carry.ura_sigma0_ewma (read one epoch later at the emit
 *    site) and advances @c net::LosState::arc_spike_streak and @c n_arc_refloat_epoch.
 *
 * It therefore runs:
 *  - after the joint solve and the carry harvest: it operates on the post-fit @c x_s
 *    and the freshly stashed carry, and erasing an entry is the refloat;
 *  - before networkClockQc, the consumer of the two culprit sets, and before the epoch
 *    stash/status tail.
 *
 * The caller brackets the call with networkSubBegin/networkSubEnd(SB_RAIM) and the
 * arming guard `if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))`.  The function is
 * the guarded body.
 */

#include "network/assembly/Contributions.hpp"  // ContributionKind, kContributionKinds (RaimPoolAdmissionTally)
#include "network/solve/Estimator.hpp"      // NetworkEstimator + NetworkEstimator::Datum
#include "network/qc/PriorResidual.hpp"  // NetworkPriorResidual, the data-side feed of the global σ̂0
#include "network/driver/RunTypes.hpp"       // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"         // MultiRinexSync::SyncedEpoch

#include <Eigen/Core>

#include <array>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

/**
 * @brief Run-end telemetry of the RAIM pool re-host.
 *
 * The re-host (@c network.raim_carry_invariant) builds the post-fit RAIM pool from a
 * carry-neutral census of ambiguity columns (@c qc_amb) instead of the phase-leveling
 * carry's list, so the screen also runs on invariant-carry runs.  The "ci" in the
 * names stands for carry-invariant.
 *
 * The counters tell apart four outcomes that would otherwise all read as "RAIM found
 * nothing":
 *  - @c epochs == 0: networkPostFitRaim never ran (call-site gate closed).
 *  - @c epochs > 0 and @c pool_cells == 0: it ran and the census was empty (dead
 *    wiring, the defect the re-host fixes).
 *  - @c pool_cells > 0 and @c sats_flagged == 0: it screened real residuals and nothing
 *    was bad (no opportunity, which is not the same as no capability).
 *  - @c sats_flagged > 0: it flagged.  The re-host is measure-only, so no carry
 *    consumer acted on the flag.
 *
 * Every member stays at zero unless @c ctx.knobs.raim_ci_on, and the run-end readout is
 * written only then, so an off run reports nothing.
 */
struct NetworkRaimCiTallies {
    long long epochs = 0;            ///< Post-fit RAIM passes with the re-hosted pool armed.
    long long pool_stations = 0;     ///< Station-epochs whose qc_amb census was non-empty.
    long long pool_cells = 0;        ///< Total carried-ambiguity cells screened (the census size).
    long long sats_flagged = 0;      ///< Consensus-flagged satellite-epochs.
    long long arcs_flagged = 0;      ///< Attribution-flagged isolated (station,sat) arc-epochs.
    double    sigma0_max = 0.0;      ///< Largest σ̂0 seen over epochs with a non-empty pool.
    double    sigma0_sum = 0.0;      ///< Σ σ̂0 over epochs with a non-empty pool (mean = sigma0_sum / sigma0_n).
    long long sigma0_n = 0;          ///< Epochs contributing to sigma0_sum.
};

/**
 * @brief Admissions of one carried-phase RAIM pool over a run, per contribution kind.
 *
 * Which row kinds a pool admits is decided by the row classifier
 * (@c raimPoolAdmitsKind, network/assembly/RowClassify.hpp).  This struct counts what
 * the rule admitted.  The post-fit pool (@ref networkPostFitRaim) and the within-epoch
 * assess pool (RaimWithinEpoch.cpp) each keep one in @c NetworkRunContext::counters,
 * printed at run end by networkRaimPoolRunEnd.
 *
 * A pool walks the rows of each station once per ambiguity column of its pool, so a row
 * whose coefficient clears 0.5 on two columns is visited twice and enters the pool's
 * sample twice.
 *  - @c visits counts every admitted (row, column) pair.
 *  - @c rows counts each admitted row once per station-epoch walk.
 *  - @c refused counts the (row, column) visits whose coefficient cleared 0.5 but whose
 *    kind the rule does not admit, by kind (@c Unset in the last slot).  Zero is
 *    expected.  A non-zero count is a row that the coefficient test alone would have
 *    admitted.
 */
struct RaimPoolAdmissionTally {
    long long walks{0};  ///< Pool walks: post-fit epochs, or within-epoch assess calls.
    std::array<long long, kContributionKinds + 1> visits{};   ///< Admitted (row, column) visits, by kind.
    std::array<long long, kContributionKinds + 1> rows{};     ///< Admitted rows, each once per station-epoch walk, by kind.
    std::array<long long, kContributionKinds + 1> refused{};  ///< Visits refused by the kind rule, by kind (@c Unset last).
    /// Data-rows-only pool (@c network.raim_pool_data_rows_only on): (observation phase row, column)
    /// pairs whose coefficient exceeds 0.5 on a column the row's tag does not name.  Each
    /// is also a refused visit.  0 expected: a phase row carries +1 on its own column only.
    long long own_col_cleared_other{0};
    /// Data-rows-only pool, knob on: (observation phase row, column) pairs whose tag names the column
    /// while the coefficient is at or below 0.5 (admitted by the tag).  0 expected.
    long long own_col_not_cleared{0};
    /// Data-rows-only pool, knob on: (observation phase row, column) pairs where the tag and the
    /// coefficient test agree that the column is the row's own (the admitted phase visits).
    long long own_col_agree{0};

    /**
     * @brief Count one (row, column) visit that cleared 0.5 (or, with the data-rows-only pool knob on,
     *        that the row's tag admitted).
     * @param k        The row's kind.
     * @param admitted Whether the pool's rule (@ref raimPoolAdmitsRow) admitted it.
     * @param first    Whether this is the row's first admitted visit of this station's walk.
     */
    void note(ContributionKind k, bool admitted, bool first);

    /**
     * @brief The own-column check for one (row, column) pair, knob on: does the
     *        column the row's tag names equal the column on which its coefficient exceeds
     *        0.5?  Counts observation phase rows only; every other kind is ignored.
     * @param k      The row's kind.
     * @param clears Whether the row's coefficient on the column exceeds 0.5 in magnitude.
     * @param names  Whether the row's tag names the column (@ref rowTagNamesColumn).
     */
    void noteOwnColumn(ContributionKind k, bool clears, bool names);

    /**
     * @brief The run-end text: the admitted kinds with their visits and rows, the refused
     *        visits by kind.
     * @return One line, without a prefix: "walks N; admitted: <kind> visits V rows R, ...;
     *         refused by kind: none" (or the refused kinds with their visits).
     */
    std::string text() const;

    /**
     * @brief The run-end text of the own-column check (knob on).
     * @return "own column: tag and coefficient agree A, coefficient > 0.5 on another
     *         column B, tag's column at or below 0.5 C".
     */
    std::string ownColumnText() const;
};

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct NetworkPostFitHandoff;   // network/solve/SolveGlue.hpp

/**
 * @brief Run the post-fit network RAIM pass for this epoch: the residual scan, the
 *        cross-station consensus, the median-polish attribution and the resulting
 *        refloats.
 *
 * The ordering contract is in the file header.
 *
 * @param run The run (network/driver/Run.hpp).
 *            Written: @c run.ctx (carry, counters and entity tables; knobs are read),
 *            @c run.traces (RAIM, attribution, station-residual and event traces),
 *            @c run.setup.stations (the parameter set's pending-reseed mark on a
 *            refloat) and @c run.engines.prires (see below).
 *            Read: @c run.config (the RAIM getters, among them
 *            networkRaimAttribution, networkRaimSigma0Trigger and
 *            networkRaimWithinEpoch), @c run.log (the refloat summary and the
 *            within-epoch warning) and @c run.engines.est (reconstructStation and
 *            rowRedundancy, both const).
 * @param ep The epoch (network/driver/Epoch.hpp), read only:
 *           - @c se, the synced epoch (week and TOW for the trace and log rows);
 *           - @c assembly.asm_res, the per-station assembly (ambiguity lists, A_s, A_r,
 *             y, w);
 *           - @c assembly.est_index_of, the station to reduced-system index (negative =
 *             absent);
 *           - @c pass_a.present, this epoch's presence mask (the scope of an attribution
 *             refloat);
 *           - @c solve.x_s, the post-fit shared solution vector;
 *           - @c solve.plan.shared_datum, the shared hard-pin datum (input of the Baarda
 *             redundancy);
 *           - @c solve.retro_feed, whether the retro-bump path is armed
 *             (@c ura_retro_bump or @c ura_refloat_inflate > 1).
 * @param handoff [in/out] The post-fit hand-off (@c ep.solve.handoff):
 *            - @c clock_refloat_sats, consensus and whole-satellite culprits whose
 *              shared clock also refloats (consumed by networkClockQc and the epoch
 *              stash);
 *            - @c retro_flag_sats, culprits whose emitted correction was contaminated
 *              (the retro-bump feed, consumed by networkClockQc);
 *            - @c n_arc_refloat_epoch, this epoch's isolated-arc refloat count (read
 *              later in the epoch by networkStashStatus for the epoch-status row).
 *
 * The prior-residual census accumulator (@c run.engines.prires) is used here only as the
 * data-side feed of the constraint-aware global variance component
 * (@c network.prior_residual_global_sigma0).  The residual walk already holds every
 * row's weight, residual and redundancy, so the feed costs a multiply and two adds
 * inside an existing loop.  It is inert unless that knob is on and measure-only in every
 * case: no decision of this pass (the σ̂0 pool, the consensus, the attribution, the
 * refloats) reads from it.  The accumulator is drained by networkStashStatus, which runs
 * after this pass, so the epoch's data side is complete when the column is written.
 */
void networkPostFitRaim(NetworkRun& run, const NetworkEpoch& ep, NetworkPostFitHandoff& handoff);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report for RAIM.
 *
 * The subsystem's row of kNetworkRunEndReports.  Its sections, defined beside it in
 * Raim.cpp, print in this order, each gated on its own knob:
 *   - networkRaimPoolRunEnd: the admissions of the carried-phase RAIM pools and the
 *     pool re-host readout.
 *   - networkRaimWithholdRunEnd: the withhold-era release policy, the per-station
 *     variance factor (@c network.raim_station_factor) and the flag-rate hygiene
 *     (@c network.raim_rate_hygiene).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkRaimRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief PASS B station step: the carry-neutral census of checkable ambiguity columns
 *        for the RAIM pool re-host.
 *
 * Row @c raim_columns of kStationBuildSteps (network/assembly/StationBuild.hpp).  PASS B
 * is the epoch's per-station assembly.  The step creates no rows or priors.  It records
 * @c qc_amb on the station's assembly: every ambiguity column present in the station's
 * local block (A_r) this epoch and not gauge-pinned, whichever carry owns it.  Under the
 * invariant carry the leveling list @c pl_amb is never filled, so without this census
 * every post-fit QC tier would see an empty pool.
 *
 * Returns at once unless @c raim_ci_on or @c pd_handover_continuous (the continuous
 * phase-datum handover is a second consumer of the census).
 *
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkRaimColumnCensus(StationBuild& build);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of the RAIM tiers (post-fit and within-epoch pools,
///        per-station factor, rate hygiene) and the processed-epoch count.
///
/// A base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RaimCounters {
    /// Run-end telemetry of the RAIM pool re-host.  All zero unless ctx.knobs.raim_ci_on.
    /// The counters tell "post-fit pass never ran" from "census empty" from "nothing
    /// flagged" (see NetworkRaimCiTallies).
    NetworkRaimCiTallies raim_ci;

    // Ambiguity-vehicle telemetry.  These counters are the on-path evidence for the
    // float ambiguity vehicle (network.ambiguity_vehicle float_kf).  A float_kf run with
    // 0 drops is a miswire, not a quiet run.  Together with the ambiguity-state trace
    // counters (n_amb_state_rows, n_amb_state_events, n_amb_state_epochs) and the
    // S-transform counters, they are reported at run end, so an arm that enables a
    // mechanism and moves none of them is visible at once.
    long long n_amb_drop              = 0;  ///< Carried priors dropped back to their birth prior, absolute value kept.
    long long n_amb_drop_t4           = 0;  ///< Of n_amb_drop, from the attribution refloat (the leveling vehicle erases the entry instead).
    long long n_amb_drop_t3           = 0;  ///< Of n_amb_drop, from the consensus refloat (the leveling vehicle erases the entry instead).
    // Drop-floor telemetry, counted whether or not the floor is armed.
    /// Drops whose restored variance came from network.amb_float_drop_var_m2, that is,
    /// the drop floor at this line of sight exceeded max(var_birth, var_first, birth
    /// floor).  0 by construction with the knob at 0.
    long long n_amb_drop_floored      = 0;
    /// Drops taken before network.amb_float_drop_floor_after_ep opened, with an armed
    /// drop floor: the population the gate withheld the floor from.  0 with no gate and
    /// 0 with no floor.
    long long n_amb_drop_pre_gate     = 0;
    /// In-block iono carries erased by an attribution arc refloat.  The refloat
    /// statistic reads the level error and the iono error as a sum, so an arc whose
    /// level is no longer trusted cannot vouch for the iono estimated with it.  The
    /// line of sight re-draws both.
    long long n_iono_blk_erased_t4  = 0;
    /// Post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is a
    /// whitened block row of the iono columns.  Counted only; the pool rule is
    /// unchanged.  See n_raim_pool_zwd_rows for why block rows enter the pool.
    long long n_raim_pool_iono_rows = 0;
    /// Drops at birth strength that cleared a regression or block row
    /// (network.amb_float_drop_decouple_always).
    long long n_amb_drop_decoupled_idem = 0;
    /// Post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is a
    /// whitened block row of the clock-family columns (receiver clock or ISB, the
    /// inter-system bias).  Counted only; the pool rule is unchanged.  See
    /// n_raim_pool_zwd_rows for why block rows enter the pool.
    long long n_raim_pool_rxclk_rows = 0;
    /// Post-fit RAIM pool admissions (station-epochs) whose admitted row is the ZWD
    /// (zenith wet delay) whitened block row.  The pool admits a row whose coefficient on
    /// the ambiguity column exceeds 0.5 and whose kind @c raimPoolAdmitsKind accepts
    /// (network/assembly/RowClassify.hpp).  A block row is a row of L⁻¹ (L Lᵀ = the
    /// block's prior covariance).  It is lower-triangular, so it carries coefficients on
    /// the ambiguity columns pushed before its own column and can clear 0.5 on them.
    /// Ambiguity columns are pushed first, so a ZWD block row (a carried-prior kind the
    /// rule admits) reaches them.
    /// Counted only; the pool rule is unchanged.
    long long n_raim_pool_zwd_rows  = 0;
    /// The carried-phase RAIM pools' admissions per contribution kind (see
    /// @c raimPoolAdmitsKind in network/assembly/RowClassify.hpp): the post-fit pool
    /// (Raim.cpp, one walk per epoch it runs) and the within-epoch assess pool
    /// (RaimWithinEpoch.cpp, one walk per assess call, the first and each replay's).
    /// Read at run end.  The declared rule admits the observation phase rows, the
    /// coupling and block carried-prior rows and the SD-EWL (single-difference
    /// extra-wide-lane) constraint rows, so the non-data rows' share is on record.
    RaimPoolAdmissionTally raim_pool_postfit;
    RaimPoolAdmissionTally raim_pool_within;   ///< As @c raim_pool_postfit, for the within-epoch assess pool.

    // Refloats that act in one solve: the carried entry is erased (or dropped, on the
    // float vehicle) after the harvest, so the arc refloats on the next epoch.
    long long n_cr_raim_attrib        = 0;  ///< Attribution refloats (live only when network.raim_attribution is on).
    /// Consensus refloats (live only when network.raim_attribution is off).  The two are
    /// a run-constant either/or, never a split.
    long long n_cr_raim_consensus     = 0;
    long long n_pc_erase_raim         = 0;  ///< Phase-colour entries erased with the line of sight by an attribution refloat.

    // Telemetry for the per-station variance factor (network.raim_station_factor) and
    // the flag-rate hygiene (network.raim_rate_hygiene).  A station departs when its
    // EWMA factor, relative to the network median, rises above a threshold, and returns
    // when it falls back below 0.8 times that threshold.  Both edges are counted.
    long long n_staf_departs          = 0;  ///< Depart events (EWMA ratio above the threshold).
    long long n_staf_returns          = 0;  ///< Return events (EWMA ratio below 0.8 x the threshold).
    double    staf_max_ratio          = 0.0;///< Run maximum of the EWMA ratio to the network median.
    long long n_staf_epochs           = 0;  ///< Epochs the factor was computed on.
    /// Cells with no phase row this epoch (raw residual exactly 0), which can never flag.
    /// They are excluded from the cell flag-rate opportunity count (rate hygiene).
    long long n_t3_untestable_cells   = 0;

    // The processed-epoch count (epochs decimated out are not counted).
    std::size_t n_loop_epochs = 0;
};

} // namespace GnssProcessor
