#pragma once
/**
 * @file Raim.hpp
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
 * erases @c ctx.entities.arcs entries and marks parameter-set ambiguities
 * pending-reseed for every refloated (station,sat,σ); inserts consensus / whole-sat
 * culprits into @c clock_refloat_sats and @c retro_flag_sats (consumed downstream by
 * the SB_CLOCKUPD two-state clock update and retro-flag feed); folds σ̂0 into
 * @c ctx.carry.ura_sigma0_ewma (read one epoch later at the emit site) and advances
 * @c ctx.carry.arc_spike_streak / @c n_arc_refloat_epoch.  It therefore runs:
 *   - after the joint solve and carry harvest (it operates on the post-fit @p x_s and
 *     the just-restashed @c local_carry; erasing an entry is the refloat);
 *   - before SB_CLOCKUPD (the only consumer of @p clock_refloat_sats /
 *     @p retro_flag_sats filled here) and the epoch stash/status tail.
 * The caller keeps the @c networkSubBegin/End(SB_RAIM) brackets and the arming guard
 * `if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))`; this function is the
 * guarded body.
 */

#include "network/assembly/Contributions.hpp"  // ContributionKind, kContributionKinds (RaimPoolAdmissionTally)
#include "network/solve/Estimator.hpp"      // NetworkEstimator + NetworkEstimator::Datum
#include "network/qc/PriorResidual.hpp"  // the constraint-aware global σ̂₀ data-side feed
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
 *  - @c sats_flagged > 0                 → it flagged; the re-host is measure-only, so
 *                                          no carry consumer acted on the flag.
 *
 * Every member stays at zero unless @c ctx.knobs.raim_ci_on, and the run-end readout
 * is written only then, so an off run reports nothing.
 */
struct NetworkRaimCiTallies {
    long long epochs = 0;            ///< Seam passes with the re-hosted pool armed.
    long long pool_stations = 0;     ///< Station-epochs whose qc_amb census was non-empty.
    long long pool_cells = 0;        ///< Total carried-ambiguity cells screened (the census size).
    long long sats_flagged = 0;      ///< Consensus-flagged satellite-epochs.
    long long arcs_flagged = 0;      ///< Attribution-flagged isolated (station,sat) arc-epochs.
    double    sigma0_max = 0.0;      ///< Largest σ̂0 seen over epochs with a non-empty pool.
    double    sigma0_sum = 0.0;      ///< Σ σ̂0 over epochs with a non-empty pool (mean = sigma0_sum / sigma0_n).
    long long sigma0_n = 0;          ///< Epochs contributing to sigma0_sum.
};

/**
 * @brief The admissions of one carried-phase RAIM pool over a run, per contribution
 *        kind (the instrumentation gap).
 *
 * The admission rule itself is a statement about row kinds and lives with the row
 * classifier (network/assembly/RowClassify.hpp, @c raimPoolAdmitsKind); this counter of what
 * the rule admitted is RAIM telemetry and lives here.  The post-fit pool
 * (@ref networkPostFitRaim) and the within-epoch assess pool (SolveGlue.cpp) each
 * keep one in @c NetworkRunContext::counters, read at run end by networkRaimPoolRunEnd.
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
 * @brief Run the post-fit network RAIM pass for this epoch (Pass 1 residual scan,
 *        Pass 2 consensus, median-polish attribution, and the resulting refloats).
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.setup.stations (stations), run.engines.prires (prires).
 *            Read: run.config (config_), run.log (log), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
 *           ep.assembly.asm_res (asm_res), ep.assembly.est_index_of (est_index_of),
 *           ep.pass_a.present (present), ep.solve.x_s (x_s), ep.solve.plan.shared_datum
 *           (shared_datum), ep.solve.retro_feed (retro_feed).
 * @param handoff [in/out] The post-fit hand-off (ep.solve.handoff), written:
 *     handoff.clock_refloat_sats (clock_refloat_sats), handoff.retro_flag_sats
 *     (retro_flag_sats), handoff.n_arc_refloat_epoch (n_arc_refloat_epoch).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: [in/out] carry + counters + knobs. The whole context is passed instead
 *     of ~24 individual references because the body mutates local_carry / ura_sigma0_ewma /
 *     arc_spike_streak by value: one missing `&` in an enumerated parameter list would compile
 *     and discard a refloat or the URA fold without any diagnostic. The body opens with an
 *     alias preamble copied byte for byte from the former run-TU alias block.
 *   - @p config_ = run.config: RAIM getters, among them networkRaimAttribution /
 *     networkRaimSigma0Trigger / networkRaimWithinEpoch.
 *   - @p log = run.log: "main_log": the refloat summary + within-epoch warning.
 *   - @p traces = run.traces: raim_trace / raim_attrib_trace / station_res_trace /
 *     events_trace + the emitEvent forwarder.
 *   - @p se = ep.se: Current synced epoch (week/TOW for the trace + log rows).
 *   - @p stations = run.setup.stations: [in/out] the parameter set's pending-reseed mark on
 *     refloat; name / is_pivot reads.
 *   - @p asm_res = ep.assembly.asm_res: Per-station assembly (pl_amb / A_s / A_r / y / w).
 *   - @p est_index_of = ep.assembly.est_index_of: Station → reduced-system index (< 0 =
 *     absent).
 *   - @p present = ep.pass_a.present: This epoch's presence mask (attribution refloat scope).
 *   - @p x_s = ep.solve.x_s: Post-fit shared solution vector.
 *   - @p est = run.engines.est: reconstructStation / rowRedundancy (both const).
 *   - @p shared_datum = ep.solve.plan.shared_datum: The shared hard-pin datum (Baarda
 *     redundancy input). The run TU passes `plan.shared_datum`; this is the only substitution
 *     relative to the moved body.
 *   - @p clock_refloat_sats = handoff.clock_refloat_sats: [in/out] consensus / whole-sat
 *     culprits whose shared clock also refloats (fed to SB_CLOCKUPD).
 *   - @p retro_flag_sats = handoff.retro_flag_sats: [in/out] culprits whose emitted correction
 *     was contaminated (retro-bump feed, SB_CLOCKUPD).
 *   - @p retro_feed = ep.solve.retro_feed: Whether the retro-bump path is armed this run
 *     (ura_retro || ura_refloat_inflate > 1.0).
 *   - @p n_arc_refloat_epoch = handoff.n_arc_refloat_epoch: [in/out] this epoch's isolated-arc
 *     refloat count (heartbeat churn metric, read later in the epoch).
 *   - @p prires = run.engines.prires: [in/out] the prior-residual census accumulator, used
 *     here only as the data-side feed of the constraint-aware global variance component
 *     (docs/SPEC_prior_residual_census.md §14). The Pass-1 row walk already holds every row's
 *     @c (w, v, r_i), so the feed costs a multiply and two adds inside an existing loop. Inert
 *     unless @c network.prior_residual_global_sigma0 is armed, and measure-only in every case:
 *     none of this seam's decisions (the σ̂0 pool, the consensus, the attribution, the
 *     refloats) read from it. The accumulator is drained in SB_STASH_STATUS, which runs after
 *     this seam, so the epoch's data side is complete when the column is written.
 */
void networkPostFitRaim(NetworkRun& run, const NetworkEpoch& ep, NetworkPostFitHandoff& handoff);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: RAIM: the carried-phase pools and the re-host, the withhold era, the R2
 *        station factor, the R4 rate hygiene.
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in Raim.cpp, print in this order, each gated on its own knob:
 *   - networkRaimPoolRunEnd: the carried-phase RAIM pools: admissions and the re-host.
 *   - networkRaimWithholdRunEnd: the withhold era, the R2 station factor, the R4 rate hygiene.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkRaimRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations (generated by scripts/nom_pb1b_cut.py) ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief The RAIM pool re-host's carry-neutral checkable-column census.
 *
 * Step `raim_columns` of kStationBuildSteps (network/assembly/StationBuild.hpp); its kinds: none
 * (it records only). It returns at once unless `raim_ci_on || pd_handover_continuous`. Records:
 * qc_amb. The frozen text of networkPassB_'s produce loop below its bindings.
 *
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkRaimColumnCensus(StationBuild& build);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of Raim, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct RaimCounters {
    /// Run-end telemetry of the RAIM pool re-host.  All zero unless
    /// ctx.knobs.raim_ci_on; the denominators separate "seam never ran" from "census
    /// empty" from "nothing flagged" (standing invariant #4).
    NetworkRaimCiTallies raim_ci;

    // Ambiguity-vehicle telemetry (RF-1, docs/refound/RF-1_float_ambiguities_design.md
    //     §7.2 / §7.4).  Standing invariant #4: these counters are the on-path evidence,
    //     and design §7.4's D3 states the rule in the other direction too: a float_kf
    //     run with 0 drops is a miswire, not a quiet run.  The trace counters
    //     (@c n_amb_state_rows / @c n_amb_state_events / @c n_amb_state_epochs), the
    //     drop counters (step 4) and the S-transform counters (step 5) are all reported
    //     at run end, so an arm that arms a mechanism and moves none of them is visible
    //     at once rather than after a grade.
    long long n_amb_drop              = 0;  ///< carried priors dropped back to their birth prior, value_abs kept (RF-1 step 4).
    long long n_amb_drop_t4           = 0;  ///< …of which from the RAIM prominence-attribution refloat (the level vehicle erases the entry instead).
    long long n_amb_drop_t3           = 0;  ///< …of which from the RAIM per-satellite consensus refloat (the level vehicle erases the entry instead).
    // Telemetry of the drop-only floor (RF-3a,
    //     docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A1/A2), maintained in both knob
    //     states so an off run reports the walk it suffers (standing invariant #4).
    long long n_amb_drop_floored      = 0;  ///< drops whose restored variance came from network.amb_float_drop_var_m2, i.e. the drop floor at this LOS exceeded max(var_birth, var_first, birth floor).  0 by construction with the knob at 0.
    long long n_amb_drop_pre_gate     = 0;  ///< drops taken before network.amb_float_drop_floor_after_ep opened, with an armed drop floor: the population the gate withheld the floor from (RF-3c).  0 with no gate, and 0 with no floor.
    long long n_iono_blk_erased_t4  = 0;             ///< in-block iono carries erased by a T4/consensus arc refloat (invariant #1: the T4 statistic reads level and iono error as a sum, so the LOS re-draws both)
    long long n_raim_pool_iono_rows = 0;             ///< post-fit RAIM pool admissions (station-epochs × rows) whose admitted row is one of the iono columns' whitened block rows: the §7 item 4 interaction measured for the I half, as n_raim_pool_zwd_rows measures it for the ρ half.  Counted only; the pool rule is unchanged.
    long long n_amb_drop_decoupled_idem = 0;         ///< drops at birth strength that cleared a regression / block row (network.amb_float_drop_decouple_always)
    long long n_raim_pool_rxclk_rows = 0;            ///< post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is one of the clock-family columns' whitened block rows: the SPEC_amb_iono_split_overcount §7 item 4 interaction measured for the rho half, as n_raim_pool_zwd_rows / n_raim_pool_iono_rows measure it for theirs.  Counted only; the pool rule is unchanged.
    long long n_raim_pool_zwd_rows  = 0;             ///< post-fit RAIM pool admissions (station-epochs) whose admitted row is the ZWD's whitened block row: the §7 item 4 interaction, measured (the pool admits a row whose coefficient on the ambiguity column exceeds 0.5 and whose kind the declared rule admits, filter/NetworkRowClassify @c raimPoolAdmitsKind; a ZWD block row is a carried-prior block kind the rule admits, and it mixes into ambiguity columns through L⁻¹).  Counted only; the pool rule is unchanged.
    /// The carried-phase RAIM pools' admissions per contribution kind (
    /// filter/NetworkRowClassify @c raimPoolAdmitsKind): the post-fit pool
    /// (Raim.cpp; one walk per epoch it runs) and the within-epoch assess pool
    /// (SolveGlue.cpp; one walk per assess call, the first and each replay's).
    /// Read at run end; the declared rule admits the observation phase rows, the
    /// coupling and block carried-prior rows and the SD-EWL constraint rows, so the
    /// non-data rows' share is on record for the later science decision.
    RaimPoolAdmissionTally raim_pool_postfit;
    RaimPoolAdmissionTally raim_pool_within;   ///< As @c raim_pool_postfit, for the within-epoch assess pool.

    // (C) One-solve refloats: the entry is erased after the harvest, so the arc
    //     refloats on the next epoch.  Selected by quality.
    long long n_cr_raim_attrib        = 0;  ///< attribution refloat, T4 (live only when network.raim_attribution is armed)
    long long n_cr_raim_consensus     = 0;  ///< consensus refloat, T3 (live only when raim_attribution is not armed; the two are a run-constant either/or, never a split)
    long long n_pc_erase_raim         = 0;  ///< entries erased by the RAIM attribution erase of their line of sight

    // Declaration telemetry for both edges (invariant #7) and hygiene accounting
    // (QUEUE item 4 R2/R4).
    long long n_staf_departs          = 0;  ///< depart events (EWMA ratio > threshold; R2)
    long long n_staf_returns          = 0;  ///< return events (hysteresis 0.8×; R2)
    double    staf_max_ratio          = 0.0;///< run-max EWMA ratio to the network median
    long long n_staf_epochs           = 0;  ///< epochs the factor was computed on
    long long n_t3_untestable_cells   = 0;  ///< s_raw==0 cells excluded from the T3 opportunities (R4)

    // Timing: the processed-epoch count and the T2e decimation counter (relocated from
    // the loop-timing cluster; t_loop_begin stayed a run-TU local, and is
    // run.timing.loop_begin now).
    std::size_t n_loop_epochs = 0;
};

} // namespace GnssProcessor
