#pragma once
/**
 * @file NetworkRunTraces.hpp
 * @brief @internal Trace-stream ownership for Application::runNetwork — the
 *        network run loop's CSV streams, their paths and re-open headers, the
 *        startup open logic, the station-roster writer, and the B8 FlagRate
 *        counter POD.
 *
 * Hoisted VERBATIM from ApplicationNetwork.cpp (decomposition INC-3,
 * 2026-07-16; see docs/network_decomposition_plan.md).  Scope
 * is deliberately narrow: this unit owns what needs ONLY streams, paths, and
 * config — the per-epoch/state-reading writers (health heartbeat,
 * emitEpochStatus/emitEvent bodies, the L15 run manifest, the teardown
 * flag-rate/run-summary CSVs) stay in the run loop and move with the
 * increments that introduce the state they read (INC-4+).  Rotation itself
 * lives in logging/CsvRotator.hpp; the rotation call site stays in the epoch
 * loop and reaches the streams through this struct.
 *
 * P1 rotation policy (moved with the stream decls): the three UNBOUNDED
 * service CSVs (always-on network_trace.csv + network_solution.csv, and the
 * RTB-6 health heartbeat) rotate size-based behind network.trace_rotate_mb.
 * Their header lines are hoisted into the named constants below so the
 * rotation reopen re-writes the IDENTICAL header the startup open wrote —
 * every rolled generation parses standalone.  Rotated .1….keep suffixes are
 * NOT auto-discovered by the pyviz viewers (they read only the live file
 * name); the operator concatenates the chain or points a viewer at one
 * generation — see docs/rt_service_operations.md.
 */

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetworkConfig;
    struct GlobalVtecShConfig;
    struct StationConfig;
    class SphericalHarmonicIonosphere;
    struct NetStation;
    struct GnssTime;
    struct EpochStatusRow;          // filter/NetworkRunTypes.hpp (INC-4 hoist)
    struct NetworkKnobs;            // app/NetworkRunContext.hpp (INC-4)
    struct NetworkServiceCounters;  // app/NetworkRunContext.hpp (INC-4)
    struct ConsumerSatRow;          // realtime/ConsumerSatTrace.hpp (card CST)
    class  NetworkCarryVceApply;    // filter/NetworkCarryVceApply.hpp (M2 v2)

    /// B8: pure observation counter — {opportunities, flags} for one QC tier,
    /// summarized at end of run as realized rate vs the implied one-sided
    /// Gaussian α for the configured k.  Never fed back into any decision.
    struct FlagRate { long opp = 0, flag = 0; };

    /// network_trace.csv header (re-written verbatim on every P1 rotation reopen).
    inline constexpr const char* kTraceHeader =
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,"
        "gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins\n";
    /// network_solution.csv header (re-written verbatim on every P1 rotation reopen).
    inline constexpr const char* kSolnHeader = "week,tow,sat,kind,signal,value_m,disc_counter\n";
    /// network_service_health.csv header (re-written verbatim on every P1 rotation reopen).
    inline constexpr const char* kHealthHeader =
        "week,tow,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,"
        "n_runaway_epochs,sigma0_ewma,n_present,n_clock_sats,"
        "ms_per_epoch_window,last_emit_age_s,pivot,cold_start_grade_s\n";
    /// network_fast_clock.csv header (re-written verbatim on every P1 rotation reopen).
    /// Two row kinds share the stream, discriminated by @c kind: "T" = one per fast tick
    /// (the summary), "S" = per-satellite detail, written only every
    /// @c network.fast_clock.trace_sat_stride ticks.  A per-tick per-satellite row at a
    /// 1 s RT cadence would be an unbounded firehose, hence the stride and the rotation.
    inline constexpr const char* kFastClockHeader =
        "week,tow,kind,dt_s,shadow,n_los,n_screened,n_sat_est,emitted,sat,dc_m,sigma_m\n";
    /// network_carry_vce.csv header (M2/LS-VCE hourly rows).
    inline constexpr const char* kCarryVceHeader =
        "week,tow,hour,family,const,n_rows,n_eff,robust_floor_m2,mean_floor_m2,"
        "mean_floor_std,rate_m2ps,rate_std,cond,rate_frozen,cfg_floor_m2,"
        "robust_ratio,mean_ratio,floor_clamp,rate_clamp\n";
    /// network_carry_vce_apply.csv header (docs/SPEC_carry_vce_apply.md — M2 v2).
    /// One row per (carry family x constellation) per emitted epoch, i.e. the LIVE
    /// trajectory of the LASER 5-95 variance factor, not an hourly digest — this is
    /// the file the card's acceptance is read off.
    ///
    /// `sigma_hat_post` = target/alpha2 IS LASER's sigma-hat-i-squared: the variance
    /// of unit weight of that group under the weights the solve ACTUALLY used.  It
    /// is the convergence readout the slide prescribes and it goes to 1 when the
    /// factor has caught its target.  `target` = robust_floor/hand_fold_var is what
    /// alpha2 is chasing and is deliberately alpha2-INDEPENDENT (both halves come
    /// from the monitor unscaled), which is why the loop cannot ring — see
    /// NetworkCarryVceApply.hpp "why the target is lag-free".
    ///
    /// `frozen` is a REASON string, never a bare flag: warmup / epoch_dirty /
    /// no_target say different things, and a run whose groups sat in `warmup` all
    /// day is an inert arm no matter how clean the log reads (standing invariant
    /// #4).  `at_ceiling` / `at_unit` are the two clamps; their EDGES are also in
    /// network_events.csv under tier VCE_APPLY, with identities.
    inline constexpr const char* kCarryVceApplyHeader =
        "week,tow,family,const,n_eff,robust_floor_m2,hand_fold_var_m2,target,"
        "alpha2,sigma_hat_post,frozen,at_ceiling,at_unit,"
        "n_steps,n_frozen,n_slew_up,n_slew_down,n_ceiling_ep,n_unit_ep,alpha2_max\n";
    /// network_prior_residual.csv header (docs/SPEC_prior_residual_census.md INC-1).
    /// One row per (family x label) per censused epoch that saw at least one row.
    ///
    /// `n_gt_*` are CUMULATIVE exceedances of the FIXED ladder |t| > {1,2,3,4,6,8,
    /// 12,20,50,100,200}; the ladder never moves, which is what makes a family
    /// comparable against another family, against D5's own numbers, and against
    /// itself a month later.  `geo_t2` is the chi2-corrected geometric mean of
    /// t^2 — 1.0 means the empirical SCALE calibration has converged, and it can
    /// sit at 1.0 while `n_gt_4` is grossly over-occupied: a scale calibration
    /// cannot flatten a non-Gaussian tail (that IS the D5 lesson).  `sigma0_warm`
    /// 0 means the family is below its warm-up row count and every `t` in the row
    /// is the RAW FORMAL `s` — never read a cold ladder as a calibrated one.
    /// `n_raw_formal` counts those rows individually.  The `c_*` columns are the
    /// INC-3 contingency table (a/b/c = above the report threshold, d/e/f = at or
    /// below it) and are all zero unless corroboration is armed; `c_hi_flat` is
    /// the b-cell, the number that matters — on the DD family it measured 97 %.
    ///
    /// ⚠ `corrob_domain` is the MEAN corroboration-domain size, and it is the
    /// ROSTER-DEPENDENCE DETECTOR: 1.0 on `pl`, ~12 on the receiver-level
    /// families, ~n_stations on `pb`/`cb`.  `corrob_or_step` is what v1's
    /// saturating OR would have called Stepped; `corrob_step` is what the
    /// consensus rule actually used (@ref priorResidualConsensus).  The gap
    /// between the two is how much the fix moved.  Because the consensus can
    /// only move rows Stepped → Flat — i.e. only RAISE the b-cell — a b-cell
    /// read without these three columns is not auditable.  `corrob_max_frac` is
    /// the SENSITIVITY column: the highest stepped fraction any domain reached,
    /// against the rule's strict-majority bar of 0.5.  A run whose maximum never
    /// approaches 0.5 has a consensus count that is UNINFORMATIVE — the bar was
    /// out of range of the data, not merely unmet.
    /// ⚠ RNEG RE-PARTITION.  Rows whose POSTERIOR EXCEEDS THEIR PRIOR
    /// (`post_var > fold_var`, i.e. r < 0 before the numerical floor — the
    /// documented coupling-carry case, where the harvested posterior is a
    /// CONDITIONAL variance) are NO LONGER in any column above.  Their |t| is
    /// not a standardized residual: `r` slams into the redundancy floor and |t|
    /// inflates by √((p+v)/(εp)), so pooling them put an artifact at the top of
    /// every ladder (measured on `runs/fldf3_pl2`: 927 of the 957 rows above
    /// |t| = 1e4, and a run-end headline of 2.5e8).  The σ̂₀_global rider has
    /// always dropped them (RNEG_DROP); the ladder now does too.
    ///
    /// They are reported in the `rneg_*` columns APPENDED AT THE END of the row,
    /// so every existing reader keeps parsing by position.  `n_r_negative` above
    /// is therefore 0 by construction on a CLEAN/RESPONDED/FRAME row and is kept
    /// only for that compatibility.  Read the rneg class on `rneg_max_abs_e_m`
    /// (metres — the only quantity there that means anything); its identity
    /// columns follow the |ê| winner, NOT the |t| winner.  A row exists for a
    /// (family, label) whose ONLY rows this epoch were rneg.
    inline constexpr const char* kPriorResidualHeader =
        "week,tow,family,label,n_rows,n_folded,n_raw_formal,n_r_clamped,n_r_negative,"
        "n_no_sigma0,"
        "sigma0,sigma0_warm,sigma0_neff,geo_t2,"
        "n_gt_1,n_gt_2,n_gt_3,n_gt_4,n_gt_6,n_gt_8,n_gt_12,n_gt_20,n_gt_50,"
        "n_gt_100,n_gt_200,"
        "max_abs_t,max_station,max_sat,max_signal,max_mech,max_e_m,max_disp_m2,"
        "max_fold_var_m2,max_post_var_m2,max_gap_s,"
        "c_hi_noev,c_hi_flat,c_hi_step,c_lo_noev,c_lo_flat,c_lo_step,"
        "corrob_domain,corrob_or_step,corrob_step,corrob_max_frac,"
        "rneg_n_rows,rneg_n_raw_formal,rneg_n_no_sigma0,rneg_max_abs_e_m,"
        "rneg_station,rneg_sat,rneg_signal,rneg_mech,rneg_max_abs_t,"
        "rneg_fold_var_m2,rneg_post_var_m2,rneg_gap_s\n";
    /// network_producer_ar.csv header (docs/network_ambiguity_resolution_design.md,
    /// the 2026-09-10 note §4.1 — "a fix count is not a grade").
    ///
    /// ONE row per DD in the Phase-2 basis per searched epoch, so the search can be
    /// JOINED TO TRUTH offline (scripts/simnet_producer_ar_grade.py:
    /// DD_true = [N(sta_r,sat_j) − N(sta_r,ref_sat)] − [N(ref_sta,sat_j) − N(ref_sta,ref_sat)]).
    /// The epoch log line carries only counts; without the per-DD IDENTITY there is
    /// nothing to join, which is why the identities are the load-bearing columns here.
    ///
    /// `float_cyc` is the DD float in cycles (dd_m/λ) and `sigma_cyc` = √Q_aa from the
    /// SAME covariance the search consumed — carry-BLIND unless
    /// `network.carry_aware_covariance` is armed (docs/SPEC_carry_aware_covariance.md
    /// §4), which is one of the two confounds the design note tells a grader to record.
    ///
    /// `in_subset` = 1 when the DD entered the `ils_max_dim` lowest-σ search subset
    /// (always 1 when uncapped); `kept` = 1 when the ILS actually returned it, and
    /// `fixed_int` is its integer THEN AND ONLY THEN (0 otherwise — read it through
    /// `kept`, never on its own).  ⚠ `fix.kept` indexes the CAPPED, σ-PERMUTED search
    /// vector, not the basis; the mapping back is @ref netar::mapFixToBasis and is
    /// unit-tested (tests/test_producer_ar_trace.cpp) because getting it wrong
    /// silently mislabels which satellite was fixed.
    ///
    /// `ratio` / `p_succ` / `n_dd` / `n_search` / `n_fixed` are EPOCH scalars repeated
    /// on every row of that epoch (a long-format join key, not per-DD values).
    /// ⚠ `ratio` / `p_succ` are the ACCEPTED fix's when `n_fixed` > 0 and the FULL-SET
    /// ATTEMPT's when it is 0 (IlsResult zeroes the accepted fields on a rejection, and
    /// a column of zeros over the epochs that did not fix says nothing).  `n_fixed` is
    /// the discriminator; never pool the two without splitting on it.
    ///
    /// ⚠ The DD basis is a STAR against ONE reference satellite and ONE reference
    /// station per (constellation, signal), both re-elected every epoch: every row
    /// inherits that pair's own error, so any grade off this file must report the
    /// reference's share or difference it away (SPEC_amb_iono_split_overcount §6.12).
    /// `ref_sat` / `ref_sta` are in every row precisely so a grader can.
    inline constexpr const char* kProducerArHeader =
        "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
        "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n";
    /// network_block_baarda.csv header (docs/SPEC_redundancy_census.md BB-INC-1..3).
    /// One row per (constellation family x label) per epoch that swept a block.
    ///
    /// ⚠ THE UNIT OF `t_B` IS A VARIANCE RATIO, NOT A SIGMA.  T_B carries the
    /// 1/b, so E[t_B] = 1 under H0 for EVERY block size and the `n_gt_*` rungs
    /// mean "this block carries k times the variance the model allows".  The
    /// rungs are deliberately the SAME as network_prior_residual.csv's so one
    /// reader handles both files — but that card's |t| is in sigma units, and
    /// comparing the two occupancy columns as though they were one quantity is a
    /// category error.
    ///
    /// `n_uncheckable` counts blocks whose residual cofactor was singular or
    /// non-finite: counted with their identity, NEVER inverted through (a pinv'd
    /// T_B has an unknown effective dof).  `n_block_only` is INC-3's deliverable
    /// — blocks that cleared the threshold while EVERY one of their rows sat
    /// below it, i.e. the population invisible to every existing tier.  The `c_*`
    /// columns are the INC-2 contingency table (hi = above the report threshold,
    /// lo = at or below), all zero unless corroboration is armed; `c_hi_flat` is
    /// the b-cell.  `geo_cal` is the CALIBRATION indicator (1.0 = the family's
    /// sigma0 has converged), NOT the geometric mean of t_B: under H0 that mean is
    /// 2exp(psi(b/2))/b, which runs 0.281/0.763/0.878 at b = 1/4/8 and would drift
    /// with the constellation's band plan.
    inline constexpr const char* kBlockBaardaHeader =
        "week,tow,family,label,n_blocks,n_rows,n_uncheckable,n_raw_formal,"
        "n_reported,n_block_only,"
        "sigma0,sigma0_warm,sigma0_neff,geo_cal,"
        "n_gt_1,n_gt_2,n_gt_3,n_gt_4,n_gt_6,n_gt_8,n_gt_12,n_gt_20,n_gt_50,"
        "n_gt_100,n_gt_200,"
        "max_t,max_T,max_row_t,max_b,max_station,max_sat,max_block_only,"
        "c_hi_noev,c_hi_flat,c_hi_step,c_lo_noev,c_lo_flat,c_lo_step\n";
    /// network_congruence.csv header (docs/SPEC_redundancy_census.md CG-INC-1..3).
    /// One row per SWEPT STATION per epoch — not a ladder, unlike the two censuses
    /// above: SPEC §7.2 grades a per-station run-median of `d_e` and §7.3's MDB
    /// table is a per-station, per-direction quantity, and neither survives
    /// aggregation.  The ladders are a run-end readout instead.
    ///
    /// ⚠ `d_e/d_n/d_u` (metres, ENU) is the **HELD COORDINATE'S ERROR**, not a
    /// correction: +East means the held mark sits that far East of where the data
    /// puts the antenna, so SUBTRACT it to correct the station.  A +5 cm East
    /// perturbation injected into a held coordinate reads `d_e = +0.05`.
    ///
    /// ⚠ THE UNIT OF `t` IS A VARIANCE RATIO, NOT A SIGMA.  `T_raw` carries the
    /// 1/q, so E[t] = 1 under H0 for every observable dimension.  The run-end
    /// ladder shares its rungs with network_prior_residual.csv and
    /// network_block_baarda.csv so one reader handles all three — but that card's
    /// |t| is in sigma units and comparing occupancies is a category error.
    ///
    /// `q` is the number of ENU directions that carried information; `q < 3` means
    /// the station clock and ZWD absorbed a direction, and `null_geom = 1` means
    /// NONE was observable (counted, never inverted through).  `mdb_*` are FORMAL
    /// (delta_0 / sqrt of the pseudo-inverse diagonal, metres) and `mdb_s_*` are
    /// those times the empirical sigma0 — the formal/scaled split the MDB card
    /// uses, so the formal model's optimism stays auditable.  Both are written as
    /// the literal `inf` when the direction is not estimable: that is a CATEGORY,
    /// never a large number to act on.  `eig_min`/`eig_max` are M's extreme
    /// eigenvalues (1/m^2) and `null_e/n/u` the ENU components of the weakest
    /// direction — which way the absorption points.  `block_flag` is INC-3's join:
    /// 1 = block-Baarda flagged >= 1 LOS of this station this epoch, 0 = it did
    /// not, -1 = that census was not armed, so there is NO verdict (distinct from
    /// a negative one).
    inline constexpr const char* kCongruenceHeader =
        "week,tow,station,label,b,n_code,n_phase,q,null_geom,"
        "d_e,d_n,d_u,T_raw,t,sigma0,raw_formal,sigma0_warm,sigma0_neff,"
        "mdb_e,mdb_n,mdb_u,mdb_s_e,mdb_s_n,mdb_s_u,"
        "eig_min,eig_max,null_e,null_n,null_u,block_flag,reported\n";
    /// network_datum_graph.csv header (docs/SPEC_datum_graph_census.md INC-2).
    /// One row per (epoch × constellation) the census touched — tier 1 saw the
    /// constellation, or a tier-2 cell / a contingency identity named it.  The
    /// pseudo-constellation `?` collects directions whose participants carry no
    /// satellite at all (SH-only modes): they belong somewhere visible, and
    /// attributing them to a constellation nobody parsed would be a guess.
    ///
    /// ⚠ THE TWO CONTINGENCY PARTITIONS ARE OVER DIFFERENT POPULATIONS.  The
    /// join is many-to-many — one direction can span several (sat, family) cells
    /// and one cell can appear in several directions — so
    /// `c_pred_obs + c_pred_only` counts PREDICTIONS while
    /// `c_obs_only + c_obs_matched` counts DIRECTIONS, and the two totals are
    /// not expected to agree.  Adding them into a single 2×2 would require
    /// double-counting one side.  `c_pred_*` / `c_obs_only` are per-CONSTELLATION
    /// (the row's own share); `c_obs_matched` and the `s_*` companion cells are
    /// EPOCH-WIDE and repeat identically on every row of the epoch, because a
    /// direction is not owned by one constellation.
    ///
    /// ⚠ `obs_src` is the honest label on the right-hand side of the join:
    /// `sbc` = the directions carry (sat, family) IDENTITIES (network.sbasis_complete
    /// armed); `count` = the spectrum reported extras but no identities, so only
    /// `obs_extras` is meaningful and every prediction lands in `predicted only`
    /// for lack of anything to match against; `absent` = the epoch never reached
    /// a rank evaluation.  Reading a coverage number off a `count` epoch is a
    /// category error.
    ///
    /// ⚠ `pred_t2` is the SPEC's strict predicate (registered PB columns, zero
    /// registered code KEYS in the family) and is the only one feeding the
    /// contingency.  `pred_sup` is the companion predicate (registered code
    /// columns with zero admitted SUPPORT) — strictly weaker, nested, reported
    /// beside it so §6.3's falsification question is answered from the same run.
    /// `no_layer` counts cells whose constellation registered no code-bias column
    /// at all, where BOTH predicates are held off by construction.
    ///
    /// `ms` is the EPOCH's total census cost, repeated on every row of the epoch
    /// (there is one measurement per pass, not one per constellation).
    inline constexpr const char* kNetworkDatumGraphHeader =
        "week,tow,cons,datum_mode,k,d,pred_k1,n_sta,n_sat,n_edges,comps,"
        "n_cells,no_layer,pred_t2,pred_sup,"
        "obs_src,declared,observed,obs_extras,obs_dirs,"
        "c_pred_obs,c_pred_only,c_obs_only,c_obs_matched,s_pred_obs,s_pred_only,"
        "kp_armed,pred_obs_ids,pred_only_ids,obs_only_ids,ms\n";
    /// network_mdb.csv header (docs/SPEC_redundancy_census.md MDB-INC-1).
    /// One row per (constellation x row class) per epoch that binned a row.
    ///
    /// `n_rows` = `n_inf` + `n_finite`.  The order statistics are over the FINITE
    /// rows only: a median of a pool containing +infinity is either meaningless
    /// or silently censored depending on the sort, so the uncheckable rows are
    /// reported as their own COUNT and kept out of every quantile.  `n_over`
    /// counts finite rows above `network.mdb_event_m` — the full population the
    /// per-cell event stream only samples.
    ///
    /// ⚠ `mdb_*` are FORMAL: they inherit the formal-σ optimism D5 measured at
    /// 10–30x on this data, because MDB scales linearly with σ.  The
    /// `mdb_scaled_*` columns are `mdb_* x sigma0_f`, the per-family empirical
    /// inflation from the P1-4 post-fit pass — and they are BLANK, never
    /// back-filled with the formal value or with 1.0, whenever `sigma0_src` says
    /// `absent*`.  `absent_sigma0_zero` on a phase row is the NORMAL state under
    /// free ambiguities (each phase row is fitted exactly by its own ambiguity),
    /// and it is itself the finding: those rows carry no redundancy at all.
    /// Quoting `mdb_med` alone to a consumer is repeating D5 v1.
    ///
    /// ⚠ `sigma0_src` ending in `_deflating` means σ̂₀ < 1, so `mdb_scaled_*` is
    /// SMALLER than `mdb_formal` — which reads as a TIGHTER guarantee and on the
    /// phase family is the exact opposite.  Measured on the FT63 cold smoke:
    /// σ̂₀,phase = 0.041 pulls the phase median from 4.58 m to 0.19 m on rows that
    /// are 78 % uncheckable.  The value is reported rather than clamped (the
    /// prior-residual census's rule — the deflation IS the measurement), but a
    /// deflating bin must be read together with its `n_inf`, never quoted alone.
    inline constexpr const char* kNetworkMdbHeader =
        "week,tow,const,class,n_rows,n_inf,n_finite,n_over,"
        "mdb_med_m,mdb_p95_m,mdb_max_m,sigma0_f,sigma0_src,"
        "mdb_scaled_med_m,mdb_scaled_p95_m,mdb_scaled_max_m,"
        "worst_station,worst_sat,worst_r,worst_sigma_m\n";

    /**
     * @internal Owns every trace stream of one runNetwork() invocation plus its
     * on-disk path.  Member names match the former runNetwork() locals exactly
     * so the run loop binds `auto& X = traces.X;` references and every writer
     * use-site stays textually unchanged (the INC-3 migration mechanic).
     * Streams open in openAll() gated on their arming toggles; a toggle left
     * OFF opens nothing and writes nothing (byte-identical-when-OFF contract).
     */
    struct NetworkRunTraces {
        /// @param log_dir The run's log directory (streams live directly in it).
        explicit NetworkRunTraces(std::filesystem::path log_dir);

        NetworkRunTraces(const NetworkRunTraces&) = delete;
        NetworkRunTraces& operator=(const NetworkRunTraces&) = delete;

        /// The run's log directory (path base for every stream).
        const std::filesystem::path log_dir_;

        /// Closed-loop consumer metrics trace (closed_loop_trace.csv; opened
        /// only when the held-out consumer is armed — path is openAll-local).
        std::ofstream cl_trace;

        /// Card CST: per-satellite closed-loop consumer trace
        /// (closed_loop_sat_trace.csv; opened only when the held-out consumer is
        /// armed AND @c network.trace_consumer_sat — path is openAll-local).
        /// Same ownership split as @ref cl_trace: the stream lives here, the rows
        /// are harvested by the consumer engine and written from the emit/store
        /// seam via @ref writeConsumerSatRows.
        std::ofstream cl_sat_trace;

        /// Always-on per-epoch estimability/service trace (P1-rotated).
        std::ofstream trace;
        const std::filesystem::path trace_path;
        /// Per-satellite product trace: the actual emitted solution (long format, one
        /// row per shared parameter per epoch) so the producer's output is inspectable
        /// — per-sat clock-Δ continuity, phase-bias stability, and discontinuity bumps.
        std::ofstream soln;
        const std::filesystem::path soln_path;
        /// Per-station, per-satellite back-substituted slant ionosphere (viz): one row
        /// per (station, sat) each emitted epoch, with the measured elevation/azimuth
        /// so the pyviz viewer can map each estimate to its ionospheric pierce point.
        std::ofstream iono_trace;
        const std::filesystem::path iono_path;
        /// Per-epoch co-estimated SH-VTEC coefficient trace (estimate_iono_sh +
        /// global_vtec_sh.trace): one row per (n,m,trig) coefficient with its value and
        /// formal std (TECU), so the estimated field is inspectable / comparable to a GIM.
        std::ofstream vtec_sh_trace;
        const std::filesystem::path vtec_sh_path;
        /// T1a: per-epoch SH-field HEALTH trace (same gate as the coefficient trace) —
        /// one row per emitted epoch summarising the estimated mean VTEC (a₀₀ + formal σ),
        /// the coefficient RMS by degree, the ratio of that RMS to the per-degree Kaula
        /// prior σ, and the network-wide SH→slant misfit (mean/rms/max |residual| plus the
        /// 3σ-exceedance counts).  Makes the a₀₀ divergence (+296 → −91 TECU) and the
        /// per-(station,sat) misfit squeezed into the ambiguities visible + attributable.
        std::ofstream vtec_sh_health_trace;
        const std::filesystem::path vtec_sh_health_path;
        /// Deep-investigation slip/RAIM diagnostics (network.trace_slip_events): the IF-phase
        /// pre-detector's per-(station,sat,pair) ΔΔφ_IF and full threshold decision each epoch,
        /// and RAIM's per-(sat,station) post-fit standardized residual + cross-station consensus.
        /// The pair of files that make an IF-slip "need to reset" and a RAIM trigger inspectable.
        std::ofstream if_slip_trace;
        const std::filesystem::path if_slip_trace_path;
        std::ofstream raim_trace;
        const std::filesystem::path raim_trace_path;
        std::ofstream raim_attrib_trace;
        const std::filesystem::path raim_attrib_trace_path;
        /// Per-satellite clock-monitor diagnostics (network.clock_monitor + trace_slip_events):
        /// one row per satellite near or over the refloat bound each epoch, so the abs/jump
        /// decision and the resulting reinit are inspectable against the emitted clock series.
        std::ofstream clk_mon_trace;
        const std::filesystem::path clk_mon_trace_path;
        /// B7: per-satellite a-priori-clock-anchor information fraction (anchor_info /
        /// reduced-normals total_info) each emitted epoch, so validation can certify the
        /// estimable clock is DATA-dominated, not defined by the a-priori CODE product it
        /// is softly anchored to.  Pure diagnostic (network.trace_anchor_info).
        std::ofstream anchor_info_trace;
        const std::filesystem::path anchor_info_trace_path;
        /// B5: cross-station between-satellite DD float ambiguities (from reconstructStation),
        /// per (const,signal), with their fractional-cycle residual — a sign-convention
        /// self-check + empirical <0.1-cycle-support evidence for user AR (gate for producer
        /// AR).  Pure diagnostic (network.trace_dd_fractional).
        std::ofstream dd_frac_trace;
        const std::filesystem::path dd_frac_trace_path;
        /// Per-DD trace of the Phase-2 producer-AR search
        /// (network.ambiguity_resolution.trace + .enable) — the searched DD basis with
        /// its identities, float value, formal σ, subset membership and the integer the
        /// ILS returned.  Written by filter/NetworkProducerAr.cpp; graded against
        /// truth_ambiguities.csv by scripts/simnet_producer_ar_grade.py.  Columns:
        /// @ref kProducerArHeader.  Pure diagnostic — the emitted product is
        /// byte-identical whether it is on or off.
        std::ofstream producer_ar_trace;
        const std::filesystem::path producer_ar_trace_path;
        /// RAW cycle-slip observables per (station,sat) every epoch (network.trace_slip_raw):
        /// Melbourne-Wübbena, geometry-free L1−L2, first-signal phase-minus-code (all m) and the
        /// per-band RINEX LLI — computed from raw_observed, INDEPENDENT of any detector being on.
        /// The always-available "was there a slip on ANY sat here?" table; in particular the only
        /// network CSV that surfaces the receiver LLI ground truth.  Epoch diffs are formed offline.
        std::ofstream slip_raw_trace;
        const std::filesystem::path slip_raw_trace_path;
        /// L1: per-epoch disposition + σ̂0 + clock-stats trace (network.trace_epoch_status).
        /// ONE row per processed epoch (emitted OR skipped) — the disposition timeline the
        /// V1/V3/V6 validation cards are judged on.  Off by default (byte-identical when OFF).
        std::ofstream epoch_status_trace;
        const std::filesystem::path epoch_status_trace_path;
        /// RTB-6: operator health heartbeat (network.health_interval_s > 0) — a periodic
        /// machine-readable @c network_service_health.csv appended every N s of CONTENT
        /// time so a long-running service is observable without waiting for the exit
        /// summary.  Named "service_health" because the pyviz viewer called
        /// network_health reads network_trace.csv — a DIFFERENT surface (per-epoch
        /// estimability, not service liveness).  sigma0_ewma reports −1 ("unavailable")
        /// unless its feeding tiers (emit_ura + raim + phase_leveling) are all armed —
        /// the EWMA only updates inside the warm post-fit RAIM block, so without them it
        /// would silently freeze at its 1.0 init.  Off by default (byte-identical when
        /// OFF: no file opened, no rows written).  Column semantics:
        /// docs/rt_service_operations.md.
        std::ofstream health_trace;
        const std::filesystem::path health_trace_path;
        /// FC-1: per-tick split-cadence fast-clock trace (network.fast_clock.trace) — one
        /// summary row per fast tick plus strided per-satellite detail rows.  The tick tier
        /// runs on epochs the full pipeline SKIPS, so this stream (with the run-end
        /// counters) is the only evidence the tier ran at all; in shadow mode it is also
        /// the grading instrument, carrying fast-vs-full deltas on FULL epochs.  Rotates
        /// (P1) because a 1 s RT tick makes it unbounded.  Off by default (byte-identical
        /// when OFF: no file opened, no rows written).
        std::ofstream fast_clock_trace;
        const std::filesystem::path fast_clock_trace_path;
        /// L2: structured per-event QC log (network.trace_events).  One LONG-format row per
        /// discrete QC action across every tier — the machine-readable replacement for
        /// spdlog regex-scraping.  Off by default (byte-identical when OFF).
        std::ofstream events_trace;
        const std::filesystem::path events_trace_path;
        /// L7: per-station ZWD trace (network.trace_zwd) — estimated ZWD + datum-conditioned
        /// formal σ + carried flag, per present station each emitted epoch (V7 instrument).
        std::ofstream zwd_trace;
        const std::filesystem::path zwd_trace_path;
        /// L14: per-(satellite, code signal) bias state (network.trace_sat_bias_states) —
        /// the emitted satellite code bias with its σ̂0-scaled formal σ from the
        /// gauge-nulled shared posterior covariance.  The B product record carries NO σ
        /// in any configuration, so this is the only channel from which the code-bias
        /// variance factor can be formed (SPEC_synthetic_minimum_network §4.8).
        std::ofstream sat_bias_states_trace;
        const std::filesystem::path sat_bias_states_trace_path;
        /// L14b: per-station receiver states (network.trace_rx_states) — the receiver clock
        /// and every receiver code-bias (DCB/IFB) state with its datum-conditioned formal σ,
        /// on the zwd_trace pattern.  The receiver half of §4.8: the W6 slice force-arms
        /// estimate_ifb, yet those columns are otherwise never exported as estimates.
        std::ofstream rx_states_trace;
        const std::filesystem::path rx_states_trace_path;
        /// L13: per-station residual field (network.trace_station_residuals) — EVERY post-fit RAIM
        /// cell (non-pivot station x sat) with s_raw / s_norm / σ̂0.  Heavy opt-in deep trace.
        std::ofstream station_res_trace;
        const std::filesystem::path station_res_trace_path;
        /// SPEC_t3_null_derivation §7.1: per-ROW Baarda redundancy census
        /// (network.trace_raim_redundancy) — one row per post-fit RAIM phase row, keyed
        /// (station x sat x SIGNAL x row), with w, r_i, the raw residual and both
        /// standardizations.  Exists because the RAIM cell is a max OVER signals, so
        /// `station_res_trace` structurally cannot separate L1C from L2W/L5Q.  Solve-
        /// neutral: it makes r_i be computed, never used.  Heavy opt-in deep trace.
        std::ofstream raim_redun_trace;
        const std::filesystem::path raim_redun_trace_path;
        /// QUEUE item 4 R2 (network.raim_station_factor): the DECLARED per-station
        /// variance factor — one row per (emitted epoch × present non-pivot station)
        /// with the per-epoch factor, its EWMA, the ratio to the network median of
        /// EWMAs, and the trim census.  Light (≤ n_sta rows/epoch).
        std::ofstream station_factor_trace;
        const std::filesystem::path station_factor_trace_path;
        /// LV-6 (network.level_authority_csgf): the internal level-authority
        /// census — one FLEET row per epoch (L_cs, MAD, counts, a00 when the SH
        /// channel exists) + one STA row per qualifying station (the tier-1
        /// mean — the d_r,GF-class diagnostic and the sampling-matched grading
        /// population).  Light (≤ 1 + n_sta rows/epoch).
        std::ofstream level_authority_trace;
        const std::filesystem::path level_authority_trace_path;
        /// LV-6 G2: the census's own per-LOS pierce points (own file so the
        /// census CSV stays byte-identical).  @see GnssConfig::networkLevelAuthorityLosDump
        std::ofstream level_authority_los_trace;
        const std::filesystem::path level_authority_los_trace_path;
        /// M2 (LS-VCE): per-hour measured carry-variance components — one row per
        /// (family, constellation) whenever the content hour rolls over.  floor_m2 is
        /// the honest constant-cofactor σ̂ to compare against the hand var_floor;
        /// rate_m2ps the honest Δt-cofactor σ̂ to compare against q_amb; ratio =
        /// floor_m2 / config floor (>1 ⇒ the hand floor is optimistic).  Off by
        /// default (byte-identical: no file, no rows).
        std::ofstream carry_vce_trace;
        const std::filesystem::path carry_vce_trace_path;
        /// M2 v2 (docs/SPEC_carry_vce_apply.md): the LASER 5-95 variance-factor
        /// trajectory — one row per (family, constellation) per emitted epoch with
        /// α², its target, the σ̂²-under-used-weights convergence readout, and the
        /// clamp / freeze state.  PER-EPOCH on purpose: the hourly monitor digest
        /// next to it cannot show a slew ramp or a clamp excursion, and those are
        /// what the acceptance is argued on.  Opened only when
        /// @c network.carry_vce_apply is armed (byte-identical: no file, no rows).
        std::ofstream carry_vce_apply_trace;
        const std::filesystem::path carry_vce_apply_trace_path;
        /// PRIOR-RESIDUAL census (docs/SPEC_prior_residual_census.md INC-1): the
        /// card's HEADLINE product — one row per (family x label) per censused
        /// epoch carrying the fixed |t| exceedance ladder, the row population, the
        /// chi2-corrected geometric mean of t^2, the per-family sigma0 with its
        /// warm-up state, the worst row's FULL identity, and (INC-3) the six
        /// contingency cells.  Answers the question D5 took three field cycles to
        /// ask: what would a k*sigma0 gate on this family actually COST, before
        /// anyone wires a response to it?  Off by default (byte-identical when
        /// OFF: no file opened, no rows written).
        std::ofstream prior_residual_trace;
        const std::filesystem::path prior_residual_trace_path;
        std::ofstream block_baarda_trace;                       ///< docs/SPEC_redundancy_census.md BB-INC-1..3 ladder rows.
        const std::filesystem::path block_baarda_trace_path;
        std::ofstream congruence_trace;                         ///< docs/SPEC_redundancy_census.md CG-INC-1..3 per-station coordinate-offset rows (the MDB table's source).
        const std::filesystem::path congruence_trace_path;
        /// BAARDA RELIABILITY / MDB (docs/SPEC_redundancy_census.md MDB-INC-1):
        /// the card's HEADLINE product — one row per (constellation x row class)
        /// per epoch that binned a row, carrying the FORMAL MDB distribution
        /// (median / p95 / max, metres), the count of UNCHECKABLE rows whose MDB
        /// is +infinity, the count above the reporting threshold, the worst finite
        /// row's full identity, and the SCALED variant with the per-family
        /// empirical σ̂₀ that produced it.  Answers, per row and in fault units,
        /// the question the redundancy census only answers yes/no: how large a
        /// fault could hide here right now.  Off by default (byte-identical when
        /// OFF: no file opened, no rows written).
        std::ofstream mdb_trace;
        const std::filesystem::path mdb_trace_path;
        /// DATUM GRAPH CENSUS (docs/SPEC_datum_graph_census.md INC-2): the card's
        /// HEADLINE product — one row per (epoch × constellation) carrying the
        /// tier-1 component multiplicity with its roster, the tier-2 channel-family
        /// predictions, and the predicted-vs-observed contingency against the
        /// epoch's SPECTRAL near-null count.  Answers the question the null-space
        /// gate and SBC cannot: not how many null directions there are, but WHY —
        /// in station and satellite identities, before the gate throws.  Off by
        /// default (byte-identical when OFF: no file opened, no rows written).
        std::ofstream datum_graph_trace;
        const std::filesystem::path datum_graph_trace_path;
        /// Card DG5 (network.trace_estimability): the machine-readable twin of the
        /// @c Network[estimability] WARN dump.  LONG format with a leading @c kind
        /// column so three record shapes share one file:
        ///   * @c EPOCH — one per dumped epoch: tow, n_shared, declared_dim, ev_max,
        ///     cutoff, n_below;
        ///   * @c DIR   — one per reported direction of that epoch: its ev plus the
        ///     ≤5 heaviest spanning columns as @c "label=weight" pairs (the same
        ///     pairs, in the same order, as the log line);
        ///   * @c RUN   — ONE trailing record with the DG6 run totals (gate
        ///     evaluations, under/over withholds, emitted, min/max observed
        ///     near-null count, and the resolved tolerance), so a control arm gets
        ///     run-wide gate telemetry without a new run-end log line (the log text
        ///     is golden-gated by the estim arm — the CSV is the ONLY artifact
        ///     delta a trace-armed run may show).
        /// This is the file scripts/characterize_storm.py prefers over parsing the
        /// WARN text; the text parser stays as the fallback for ARCHIVED runs.
        std::ofstream estimability_trace;
        const std::filesystem::path estimability_trace_path;
        /// SPEC_day129_frame_guards P1-3 (@c network.trace_sh_spectrum): the
        /// PER-EPOCH spectrum of the reduced shared normals, with each small
        /// direction's eigenvector mass partitioned across the clock / code-bias /
        /// phase-bias / SH-VTEC column families.  Distinct from
        /// @ref estimability_trace, which is a one-shot datum-debugging dump: this
        /// is a time series built to test whether an a00 collapse coincides with a
        /// smallest mode that SPANS clocks and the field (the joint near-null
        /// slide of REPORT §3.2).  Nothing is logged — the estimability WARN text
        /// stays golden.
        std::ofstream sh_spectrum_trace;
        const std::filesystem::path sh_spectrum_trace_path;
        /// P1-3 detail file (every @c network.trace_sh_spectrum_stride traced
        /// epochs): the heaviest per-COLUMN participations of the smallest mode,
        /// so a spanning direction can be named, not just weighed.
        std::ofstream sh_spectrum_mode_trace;
        const std::filesystem::path sh_spectrum_mode_trace_path;
        /// SPEC_day129_frame_guards P1-4 (@c network.trace_postfit_keyed): keyed
        /// robust post-fit pools from the P1-4 residual pass — one row per
        /// (kind ∈ {STA, SAT, SIG}, key) per emitted epoch, {code, phase} pools
        /// each (n, σ̂0 = 1.4826·median, rms, max of |v|√w).  The RHS-localization
        /// instrument: WHERE the onset misfit sits — which stations (geography),
        /// which satellites, which signals (dispersive split).
        std::ofstream pf_keyed_trace;
        const std::filesystem::path pf_keyed_trace_path;
        /// SPEC_event_harness §24.6 (@c network.trace_postfit_rows): SIGNED per-row
        /// post-fit residuals from the same P1-4 pass — one line per DATA row the pass
        /// visits and attributes to a family (kind ∈ {code, phase}):
        /// @c week,tow,station,sat,signal,kind,resid_m,w,elev_deg.  @c resid_m is
        /// v = y − A_s·x_s − A_r·x_r (observed − predicted, metres, NOT standardized —
        /// the pass pools |v|·√w), @c w the row's information weight, @c elev_deg the
        /// LOS elevation off the station's modeled observation set (empty when the
        /// station is absent from it).  The per-LOS attribution instrument the keyed
        /// pools cannot be: they pool per STA / SAT / SIG separately and drop the sign.
        std::ofstream pf_rows_trace;
        const std::filesystem::path pf_rows_trace_path;
        /// SPEC_event_harness §24.10 card ZP (@c network.trace_station_block): the
        /// per-station LOCAL-BLOCK audit — one row per station per TRACED epoch (the
        /// first 20 EMITTED epochs, then every 60th) describing
        /// @f$N_{rr}=A_r^\top W A_r@f$ + folded station priors, the matrix
        /// @c NetworkEstimator::reconstructStation inverts with the shared states held
        /// at their solved values.  Columns: @c week,tow,station, the row counts by
        /// kind, @c n_local, @c rank, @c cond, the three smallest eigenpairs with the
        /// local states that span them, the ZWD column's effective / raw information,
        /// the reconstructed local correction's norm + ZWD element, and the
        /// zero-redundancy row counts.  The eyes of card ZP: it names WHICH local
        /// direction is unresolved, not merely that the block is ill-conditioned.
        std::ofstream station_block_trace;
        const std::filesystem::path station_block_trace_path;
        /// RF-1 §7.4 (@c network.trace_amb_state): the per-arc AMBIGUITY CARRY-STATE
        /// trace — one row per carried (station, satellite, signal) per traced epoch
        /// with what was carried (@c value_abs / @c var / @c var_first / @c var_birth),
        /// what was FOLDED (@c fold_var, @c folded, @c route) and the arc's EVENTS
        /// (birth / drop / erase / stransform), plus an @c erase: row captured at each
        /// of the four erase sites immediately before the entry is deleted.  The
        /// instrument R1 did not have: it read the per-LOS CODE RESIDUAL as a proxy for
        /// the level because nothing traced the carry itself, and the prior-residual
        /// census is aggregated per (family, label) per epoch so it cannot give a
        /// per-arc trajectory.  Written by @ref GnssProcessor::networkAmbStateTrace from
        /// rows buffered in @c NetworkCarryState::amb_state_rows.  Columns:
        /// @ref GnssProcessor::kAmbStateHeader; reads: scripts/amb_state_read.py.
        /// MEASURE-ONLY, and default OFF ⇒ no file, no rows (byte-identical).
        std::ofstream amb_state_trace;
        const std::filesystem::path amb_state_trace_path;
        /// S1 (@c network.trace_phase_anchor): one row per GAUGE-PINNED ambiguity per
        /// PROCESSED epoch — @c week,tow,cons,signal,sat,anchor_sta.  The
        /// (constellation, signal, satellite) -> anchoring STATION map that realizes the
        /// phase datum, which the lane-ladder spectrometer's ANCHOR PARTITION needs and
        /// which no other artifact carries (@c network_anchor_info.csv is the a-priori
        /// CLOCK anchor — no signal, no station — and the DD-fractional trace skips
        /// pinned ambiguities by construction).  Log-only: read off the election that
        /// already ran, so the solve is identical with it armed.
        std::ofstream phase_anchor_trace;
        const std::filesystem::path phase_anchor_trace_path;
        /// SPEC_sd_ewl_prereduce §6 (@c network.sd_ewl.trace): one row per
        /// (station, epoch, satellite) visit that the SD pre-reduce EWL appender
        /// considered — the elected (band1, band2) phase pair and its code channels,
        /// the MW arc outcome (@c status), the arc's ZD mean / lagged count, and, for
        /// a built row, the SD @f$z@f$ and its floored σ.  The instrument for the
        /// question the run-end counters can only aggregate: WHICH signal pair the
        /// detector actually holds an EWL arc on, per satellite.  Log-only — the
        /// solve is identical with the trace armed.  Rows are buffered per station in
        /// @c StationAssembly::sd_ewl_trace and drained by PASS B's serial consume, so
        /// the file order is thread-count independent.
        std::ofstream sd_ewl_trace;
        const std::filesystem::path sd_ewl_trace_path;
        /// Increment 2a (@c network.sd_ewl.trace AND @c network.sd_ewl.fix_enable): one
        /// row per (station, satellite) the Route-B integer decision considered — the
        /// elected NETWORK-COMMON channel pair, the component reference station r0, the
        /// spanning-tree parent and link satellite, the DD it rounded with that DD's
        /// fractional residue / sigma / FFR, the resulting frame integer, and a
        /// @c status naming the outcome.  This is the only place the DD arithmetic
        /// behind a fixed row is visible; the run-end tallies can only aggregate it.
        /// Armed only WITH the integer path, so an increment-1 artifact set is
        /// unchanged.  Serial seam — written in place, no per-station buffering needed.
        std::ofstream sd_ewl_ar_trace;
        const std::filesystem::path sd_ewl_ar_trace_path;
        /// Card-4 F-A attribution dump (@c network.mw_wl.d5_dump AND the D5 innovation
        /// gate): one row per D5 innovation flag per epoch with its adjacency tags —
        /// EWL-fixed (station,sat) membership, hold-cycle chord, injected-last-epoch
        /// chord — plus the D2 tri-state @c raw_stepped tag.  Log-only: the solve never
        /// reads it.  Written from @ref networkInstallCarry through this handle.
        ///
        /// Opened by @ref openMwWlDumps, NOT openAll: its arming expression
        /// (@c (ewl_on||wl_inject_on||lane_on) && nmw.d5_dump && carry_innov_gate) is
        /// derived in the run TU well after openAll runs, and re-deriving it here would
        /// bit-rot against the driver.  Same lifecycle either way — one truncating open
        /// with the header, per run, before the epoch loop.
        std::ofstream d5_flags_trace;
        const std::filesystem::path d5_flags_trace_path;
        /// Card WL-2/WL-3 F-B lane trace (@c network.mw_wl.lane_trace): per epoch, the
        /// POST-install carried WL lane value of every primary cycle carried in both
        /// bands, with the decided/held/injected tags — the constrained and
        /// unconstrained populations in one file, so an injected cycle's circular R is
        /// gradable (@c scripts/grade_lane_trace.py).  Log-only.  Opened by
        /// @ref openMwWlDumps for the same reason as @ref d5_flags_trace.
        std::ofstream wl_lane_trace;
        const std::filesystem::path wl_lane_trace_path;
        /// L15: run-provenance manifest path (startup record written by
        /// writeRunManifestStartup, end-of-run record APPENDED by
        /// writeRunManifestComplete — one member so both target the same file).
        const std::filesystem::path manifest_path;

        /// L1 config echoes written into every epoch-status row (and echoed by
        /// the L15 manifest) — captured once in openAll().
        double es_sigma0_trigger_   = 0.0;
        double es_withhold_ceiling_ = 0.0;
        /// L1: are the two product-referenced admission census columns part of this
        /// run's epoch-status schema (docs/SPEC_measurement_admission.md INC-3)?
        /// Captured in openAll() because @ref emitEpochStatus receives neither the
        /// config nor the run context, and the card's OFF contract is structural
        /// byte-identity — appending the columns unconditionally, as the redundancy
        /// and prefit censuses did, would change the artifact of every unarmed run
        /// and break exactly the same-build A/B this card's §7 gates depend on.
        bool es_adm_columns_ = false;
        /// L1: are the five prior-residual census columns part of this run's
        /// epoch-status schema (docs/SPEC_prior_residual_census.md)?  Captured in
        /// openAll() for the same reason as @ref es_adm_columns_ and by the same
        /// precedent: this card's OFF contract is STRUCTURAL byte-identity and its
        /// own §9.1 gate is a same-build A/B comparison of this very file, so the
        /// columns may not appear on an unarmed run.
        bool es_prires_columns_ = false;

        /// L1: are the three constraint-aware global-σ̂₀ columns part of this
        /// run's epoch-status schema (docs/SPEC_prior_residual_census.md §14)?
        /// DELIBERATELY a separate flag from @ref es_prires_columns_ rather than
        /// three more columns under the census's own arm: the census's §9.1 gate
        /// is a same-build A/B of this very file, so a census-only run must keep
        /// byte-for-byte the schema it had before this rider existed.
        bool es_s0g_columns_ = false;

        /// L1 / R0 (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5): are the
        /// four within-epoch-tier observability columns (sigma0_first,
        /// n_we_refloat_sats, we_clk_move_max_m, we_clk_move_max_sat) part of this
        /// run's epoch-status schema?  Present iff network.raim_within_epoch is
        /// armed — the adm/prires precedent (a tier-OFF run keeps structural
        /// byte-identity), and the columns are meaningless without the tier
        /// anyway (σ̂₀_first is that tier's own first-assess statistic).
        bool es_we_columns_ = false;

        /**
         * @brief Open every armed trace stream and write its header row (verbatim
         *        relocation of the runNetwork() startup trace-open scope).
         *
         * Creates @c log_dir_ first (the L15 manifest writer downstream relies on
         * this).  Each stream opens only when its arming toggle is set; open
         * failures WARN and leave the stream closed (writers no-op).  The B5/raw-
         * slip PppUtils wavelength providers are deliberately NOT constructed here
         * — the caller builds them right after, gated on the streams this call
         * opened (they need the LogManager, which this unit does not hold).
         *
         * @param config_        Full config (getter-level toggle reads — parameter
         *                       named as the member it aliased so the relocated
         *                       body is textually unchanged).
         * @param nc             Network config section (trace_* toggles).
         * @param shc            SH-VTEC config (trace gate for the coefficient trace).
         * @param sh_model       Co-estimated SH model, or nullptr — sizes the T1a
         *                       health-trace per-degree header columns.
         * @param cl_trace_armed Closed-loop consumer armed (opens closed_loop_trace.csv).
         * @param log            main_log logger for open-failure warnings.
         */
        void openAll(const GnssConfig& config_, const NetworkConfig& nc,
                     const GlobalVtecShConfig& shc,
                     const SphericalHarmonicIonosphere* sh_model,
                     bool cl_trace_armed,
                     const std::shared_ptr<spdlog::logger>& log);

        /**
         * @brief Open the two MW/WL diagnostic dumps (@ref d5_flags_trace,
         *        @ref wl_lane_trace) with their headers — the openAll tail for the
         *        streams whose arming expression is only known later in setup.
         *
         * Called ONCE from the run TU, before the epoch loop, after the WL zone-2
         * arming booleans are derived.  Both open TRUNCATING like every other trace
         * stream: re-running a config into an already-used log directory replaces the
         * file instead of concatenating onto the previous run's rows.  (They used to
         * be re-opened per epoch in @c std::ios::app from inside networkInstallCarry,
         * which silently glued consecutive runs together — a false D5-divergence
         * generator for any A/B whose two arms had been re-run unequal numbers of
         * times.)  An unarmed dump opens nothing and writes nothing.
         *
         * @param d5_armed         @c mw_d5_dump && carry_innov_gate — the exact
         *                         condition the per-epoch writer tests, so an armed
         *                         run with zero flags yields a header-only file rather
         *                         than an absent one, and an inert gate yields neither.
         * @param lane_trace_armed @c mw_lane_trace.
         * @param log              main_log logger for open-failure warnings.
         */
        void openMwWlDumps(bool d5_armed, bool lane_trace_armed,
                           const std::shared_ptr<spdlog::logger>& log);

        /**
         * @brief Write the one-shot station-roster CSV (network_stations.csv).
         *
         * A tiny static CSV of the stations that actually loaded (id, role, ECEF)
         * so the pyviz worldmap reflects the real run, not just the config.  The
         * held-out closed-loop consumer (if any) is listed with role "held_out".
         * Verbatim relocation of the runNetwork() roster block; the caller keeps
         * the @c nc.trace_stations gate.
         *
         * @param log_dir  The run's log directory.
         * @param stations Loaded network stations (name / datum role / ECEF).
         * @param held_out Held-out consumer station, or nullptr when closed-loop
         *                 is not armed.
         * @param log      main_log logger for the wrote/failed lines.
         */
        static void writeStationRoster(const std::filesystem::path& log_dir,
                                       const std::vector<NetStation>& stations,
                                       const StationConfig* held_out,
                                       const std::shared_ptr<spdlog::logger>& log);

        // ------------------------------------------------------------------
        // Decomposition INC-4: the state-reading writers, relocated now that
        // their state lives in NetworkRunContext (knobs/counters) or arrives
        // as explicit values.  Bodies are verbatim relocations; each opens
        // with an alias block binding the former runNetwork() local names.
        // ------------------------------------------------------------------

        /// L15 startup run-provenance manifest record (JSONL, truncating open).
        /// No-op unless nc.write_run_manifest.
        void writeRunManifestStartup(const GnssConfig& config_, const NetworkConfig& nc,
                                     const NetworkKnobs& knobs,
                                     const std::filesystem::path& config_path_,
                                     std::size_t n_stations, int n_threads,
                                     const std::shared_ptr<spdlog::logger>& log);

        /// L15 end-of-run manifest record (JSONL, APPEND — a crashed run keeps
        /// the startup record).  No-op unless nc.write_run_manifest.
        void writeRunManifestComplete(const NetworkConfig& nc,
                                      const NetworkServiceCounters& counters,
                                      std::size_t n_synced, double loop_ms);

        /// P1 size-based rotation of the three unbounded service CSVs — called
        /// once per processed epoch BEFORE the epoch's rows land.  OFF
        /// (trace_rotate_mb == 0) touches nothing.
        void rotateServiceCsvs(const NetworkConfig& nc,
                               const std::shared_ptr<spdlog::logger>& log);

        /// RTB-6 operator health heartbeat row (every nc.health_interval_s of
        /// content time; guarded on the stream being open).  Mutates the
        /// heartbeat anchors (last_health_write_s + the P4(b) window anchors)
        /// inside @p counters.
        void writeHealthRow(const GnssTime& t, const NetworkConfig& nc,
                            NetworkServiceCounters& counters,
                            const std::chrono::steady_clock::time_point& t_loop_begin,
                            bool ura_on, bool raim_on, bool pl_on,
                            double ura_sigma0_ewma,
                            const std::map<std::string, std::string>& last_pivot,
                            double grade_metric_s);

        /// L1: one network_epoch_status.csv row (no-op when the trace is OFF).
        void emitEpochStatus(const GnssTime& t, const char* disposition,
                             const EpochStatusRow& r);

        /**
         * @brief Card CST: write one consumer epoch's per-satellite rows to
         *        @ref cl_sat_trace (no-op when the stream is not open).
         *
         * Called from the emit/store seam immediately after
         * @c consumer->processEpoch, with the engine's own harvested buffer
         * (@c RealTimeEngine::consumerSatRows).  The epoch stamp is the
         * PRODUCER's @c se.time — the same stamp @c closed_loop_trace.csv uses
         * for that consumer epoch, so the two files join on (week, tow) with no
         * tolerance.
         *
         * @param t    Epoch time stamped on every row.
         * @param rows The consumer's rows for this epoch (may be empty).
         * @return Rows actually written (0 when the stream is closed) — the
         *         caller folds it into the run-end counters (invariant #4).
         */
        std::size_t writeConsumerSatRows(const GnssTime& t,
                                         const std::vector<ConsumerSatRow>& rows);

        /// L2: one network_events.csv row for a discrete QC action (no-op OFF).
        /// Commas in @p detail are sanitized to semicolons (single CSV field).
        void emitEvent(const GnssTime& t, const char* tier, const char* event,
                       const std::string& sat, const std::string& station,
                       std::optional<double> statistic, std::optional<double> threshold,
                       const char* action, const std::string& detail);

        /**
         * @brief M2 v2 — write this epoch's variance-factor row for every group.
         *
         * One @c kCarryVceApplyHeader row per (family × constellation) known to
         * @p apply, written every emitted epoch the apply path stepped.  No-op when
         * the stream is closed (the apply path unarmed), so the call site needs no
         * second gate.
         *
         * @param t          epoch time (week/tow of the row).
         * @param apply      the factor engine (read-only).
         * @param hour_state caller-owned content-hour bucket; the stream is FLUSHED
         *                   when it rolls.  Trace durability is not automatic here —
         *                   only @c health_trace flushes on its own, and this file is
         *                   the card's graded artifact, so a killed run must not lose
         *                   the whole trajectory.
         */
        void writeCarryVceApplyRows(const GnssTime& t,
                                    const NetworkCarryVceApply& apply,
                                    int& hour_state);

        /// Card DG5: the @c EPOCH record of ONE estimability dump (no-op when the
        /// trace is OFF).  Fed straight from
        /// @c NetworkEstimator::dumpEstimabilitySpectrum's return value, so the
        /// row and the WARN line carry the same numbers by construction.  Taken as
        /// primitives rather than the nested @c EstimabilityDump so this header
        /// stays free of the (Eigen-pulling) estimator include.
        void writeEstimabilityEpochRow(const GnssTime& t, double tow, int n_shared,
                                       int declared_dim, double ev_max, double cutoff,
                                       int n_below);

        /// Card DG5: one @c DIR record — a reported near-null direction's eigenvalue
        /// plus its ≤5 heaviest spanning columns as (label, |component|) pairs, in
        /// the log line's order (no-op when the trace is OFF).
        void writeEstimabilityDirRow(const GnssTime& t, double ev,
                                     const std::vector<std::pair<std::string, double>>& top);

        /// Card DG6: the single trailing RUN record with the run-wide rank-gate
        /// telemetry (no-op when the trace is OFF).  Written once at teardown,
        /// before @ref closeAll.
        /// @param rank_gate_tol The RESOLVED tolerance the run actually used
        ///                      (NetworkEstimator::rankGateTol) — the calibration
        ///                      evidence is worthless without the value it came from.
        void writeEstimabilityRunRow(const NetworkServiceCounters& counters,
                                     double rank_gate_tol);

        /// B8 flag-rate empirical-alpha summary (log + network_flag_rates.csv)
        /// + the L3 run-summary CSV — ONE knobs.trace_flag_rates gate covers
        /// both (an OFF run writes neither).
        void writeFlagRateAndRunSummary(const GnssConfig& config_, const NetworkKnobs& knobs,
                                        const NetworkServiceCounters& counters,
                                        std::size_t n_synced, std::size_t n_stations,
                                        int n_threads, bool parallel_on, double loop_ms,
                                        const std::shared_ptr<spdlog::logger>& log);

        /// Teardown close of exactly the streams the former inline list closed
        /// (the rest rely on the destructor, unchanged).
        void closeAll();
    };

} // namespace GnssProcessor
