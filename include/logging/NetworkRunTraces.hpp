#pragma once
/**
 * @file NetworkRunTraces.hpp
 * @brief @internal Trace-stream ownership for Application::runNetwork: the
 *        network run loop's CSV streams, their paths and re-open headers, the
 *        startup open logic, the station-roster writer, and the B8 FlagRate
 *        counter POD.
 *
 * The stream members and the startup open logic were moved unchanged out of
 * ApplicationNetwork.cpp (decomposition INC-3; see docs/network_decomposition_plan.md).
 * The writers that read per-epoch or run state (health heartbeat, epoch-status and
 * event rows, the L15 run manifest, the run-end flag-rate and run-summary CSVs)
 * followed in INC-4 and receive that state as NetworkRunContext parts
 * (NetworkKnobs, NetworkServiceCounters) or as explicit values.  The rotation
 * primitive is in logging/CsvRotator.hpp; the epoch loop calls rotateServiceCsvs().
 *
 * P1 rotation policy: the three service CSVs that grow without bound rotate by size
 * when network.trace_rotate_mb > 0.  They are the always-on network_trace.csv and
 * network_solution.csv and the RTB-6 health heartbeat (network_service_health.csv).
 * Their header lines are the named constants below, so a rotation reopen writes the same header as the
 * startup open and every rolled generation parses on its own.  The pyviz viewers
 * read only the live file name and do not discover the rolled generations
 * (file.csv.1 … file.csv.N, N = network.trace_rotate_keep); reading those means
 * concatenating the chain or pointing a viewer at one generation (see
 * docs/rt_service_operations.md).
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

    /// Flag-rate tally for one QC tier (B8): {opportunities, flags}.  Summarized at the
    /// end of the run as the realized rate against the two-sided Gaussian
    /// α = P(|Z| > k) = erfc(k/√2) implied by the tier's configured k (a tier with an
    /// absolute bound reports no α).  Measurement only; never fed back into a decision.
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
    /// network_carry_vce.csv header (M2/LS-VCE hourly rows).
    inline constexpr const char* kCarryVceHeader =
        "week,tow,hour,family,const,n_rows,n_eff,robust_floor_m2,mean_floor_m2,"
        "mean_floor_std,rate_m2ps,rate_std,cond,rate_frozen,cfg_floor_m2,"
        "robust_ratio,mean_ratio,floor_clamp,rate_clamp\n";
    /// network_carry_vce_apply.csv header (docs/SPEC_carry_vce_apply.md, M2 v2).
    /// One row per (carry family x constellation) per emitted epoch: the per-epoch
    /// trajectory of the LASER 5-95 variance factor rather than an hourly digest.
    /// The card's acceptance is graded on this file.
    ///
    /// `sigma_hat_post` = target/alpha2 is LASER's sigma-hat-i-squared: the variance
    /// of unit weight of that group under the weights the solve used.  It is the
    /// convergence readout LASER 5-95 prescribes and goes to 1 when the factor has
    /// reached its target.  `target` = robust_floor/hand_fold_var is the value alpha2
    /// tracks.  It does not depend on alpha2 (both terms come from the monitor
    /// unscaled), so the loop cannot oscillate; see NetworkCarryVceApply.hpp, "Why
    /// the target is lag-free".
    ///
    /// `frozen` holds the freeze reason (warmup, epoch_dirty or no_target; empty when
    /// not frozen) rather than a bare flag, because the reasons differ: groups that
    /// stay in `warmup` for the whole run mean the arm was inert however clean the log
    /// reads (standing invariant #4).  `at_ceiling` and `at_unit` are the two clamps;
    /// their entry and release edges also appear in network_events.csv under tier
    /// VCE_APPLY, with identities.
    inline constexpr const char* kCarryVceApplyHeader =
        "week,tow,family,const,n_eff,robust_floor_m2,hand_fold_var_m2,target,"
        "alpha2,sigma_hat_post,frozen,at_ceiling,at_unit,"
        "n_steps,n_frozen,n_slew_up,n_slew_down,n_ceiling_ep,n_unit_ep,alpha2_max,"
        "robust_var_r_m2,hand_fold_var_r_m2,rbar,n_eff_r\n";
    /// network_prior_residual.csv header (docs/SPEC_prior_residual_census.md INC-1).
    /// One row per (family x label) per censused epoch that saw at least one row.
    ///
    /// `n_gt_*` count the rows whose |t| exceeds each rung of the fixed ladder
    /// {1,2,3,4,6,8,12,20,50,100,200}; the counts are cumulative across rungs (a row
    /// counts in every rung it exceeds).  The ladder never moves, so a family stays
    /// comparable with another family, with D5's own numbers and with itself in a
    /// later run.  `geo_t2` is the chi2-corrected geometric mean of t^2; 1.0 means the
    /// empirical scale calibration has converged.  It can sit at 1.0 while `n_gt_4` is
    /// heavily over-occupied, because a scale calibration cannot flatten a
    /// non-Gaussian tail (the D5 result).  `sigma0_warm` = 0 means the family is below
    /// its warm-up row count and every `t` in the row is the raw formal `s`, so that
    /// ladder is not calibrated.  `n_raw_formal` counts those rows individually.  The
    /// `c_*` columns are the INC-3 contingency table (a/b/c = above the report
    /// threshold, d/e/f = at or below it) and are all zero unless corroboration is
    /// armed; `c_hi_flat` is the b-cell, the table's headline number.
    ///
    /// `corrob_domain` is the mean corroboration-domain size and detects roster
    /// dependence: 1.0 on `pl`, about 12 on the receiver-level families, about
    /// n_stations on `pb`/`cb`.  `corrob_or_step` is what v1's saturating OR would have
    /// called Stepped; `corrob_step` is what the consensus rule used
    /// (@ref priorResidualConsensus); the gap between the two is how far the rule
    /// moved the table.  The consensus can only move rows Stepped → Flat, i.e. only
    /// raise the b-cell, so a b-cell is auditable only together with these three
    /// columns.  `corrob_max_frac` is the sensitivity column: the highest stepped
    /// fraction any domain reached, against the rule's strict-majority bar of 0.5.
    /// When the maximum never approaches 0.5, the bar was out of range of the data and
    /// the consensus count carries no information.
    ///
    /// Rows whose posterior variance exceeds their prior (`post_var > fold_var`, i.e.
    /// r < 0 before the numerical floor: the documented coupling-carry case, where the
    /// harvested posterior is a conditional variance) form a separate rneg class and
    /// are excluded from every column above.  Their |t| is not a standardized
    /// residual: `r` hits the redundancy floor and |t| inflates by √((p+v)/(εp)),
    /// which would put an artifact at the top of every ladder.  The σ̂₀_global rider
    /// drops them as well (RNEG_DROP).
    ///
    /// The rneg class is reported in the `rneg_*` columns appended at the end of the
    /// row, so existing readers keep parsing by position.  `n_r_negative` above is
    /// therefore 0 by construction on a CLEAN/RESPONDED/FRAME row and is kept only for
    /// that compatibility.  Read the rneg class on `rneg_max_abs_e_m` (metres, the only
    /// meaningful quantity for it); its identity columns follow the |ê| winner, not the
    /// |t| winner.  A (family, label) whose only rows this epoch were rneg still gets a
    /// row.
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
    /// §4.1 of the 2026-09-10 note: a fix count is not a grade).
    ///
    /// One row per DD in the Phase-2 basis per searched epoch, so the search can be
    /// joined to truth offline (scripts/simnet_producer_ar_grade.py:
    /// DD_true = [N(sta_r,sat_j) − N(sta_r,ref_sat)] − [N(ref_sta,sat_j) − N(ref_sta,ref_sat)]).
    /// The epoch log line carries only counts, so the per-DD identity columns are what
    /// make the join possible.
    ///
    /// `float_cyc` is the DD float in cycles (dd_m/λ) and `sigma_cyc` = √Q_aa from the
    /// covariance the search consumed (-1 when that diagonal is not positive).  That
    /// covariance is carry-blind unless `network.carry_aware_covariance` is armed
    /// (docs/SPEC_carry_aware_covariance.md §4), one of the two confounds the design
    /// note asks a grader to record.
    ///
    /// `in_subset` = 1 when the DD entered the `ils_max_dim` lowest-σ search subset
    /// (always 1 when uncapped); `kept` = 1 when the ILS returned it.  `fixed_int` is
    /// its integer when `kept` = 1 and 0 otherwise, so read it through `kept`.
    /// `fix.kept` indexes the capped, σ-permuted search vector, not the basis; the
    /// mapping back is @ref netar::mapFixToBasis, unit-tested in
    /// tests/test_producer_ar_trace.cpp because an error there would mislabel which
    /// satellite was fixed without any other symptom.
    ///
    /// `ratio` / `p_succ` / `n_dd` / `n_search` / `n_fixed` are epoch scalars repeated
    /// on every row of that epoch (a long-format join key, not per-DD values).
    /// `ratio` and `p_succ` belong to the accepted fix when `n_fixed` > 0 and to the
    /// full-set attempt when it is 0 (IlsResult zeroes the accepted fields on a
    /// rejection, and those zeros carry no information).  `n_fixed` is the
    /// discriminator: split on it before pooling either column.
    ///
    /// The DD basis is a star against one reference satellite and one reference
    /// station per (constellation, signal), both elected every epoch (the satellite
    /// tracked at the most stations; the station that tracks it and the most
    /// satellites).  Every row inherits that pair's own error, so a grade built on
    /// this file reports the reference's share or differences it away
    /// (SPEC_amb_iono_split_overcount §6.12); `ref_sat` / `ref_sta` are in every row
    /// for that purpose.
    inline constexpr const char* kProducerArHeader =
        "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
        "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n";
    /// The same header with network.ambiguity_resolution.decorrelated_partial armed (card
    /// ARL): four pair columns, equal on the two rows of an L1/L2 pair and empty on a DD
    /// without a partner in the search.  wl_float_sigma_cyc is the pair's wide-lane formal
    /// sigma before the fix; wl_cond_cyc / wl_cond_sigma_cyc its wide lane conditioned on
    /// the fixed decorrelated components; nl_cond_sigma_cyc the conditioned narrow lane
    /// (ionosphere-free part, (f1 a1 - f2 a2) / (f1 - f2) narrow-lane cycles).
    inline constexpr const char* kProducerArHeaderDecorrelated =
        "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
        "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed,"
        "wl_float_sigma_cyc,wl_cond_cyc,wl_cond_sigma_cyc,nl_cond_sigma_cyc\n";
    /// The two columns appended to either header above (before its newline) when
    /// network.ambiguity_resolution.suspect_epochs is armed (card ARL,
    /// filter/NetworkArSuspect.hpp).  suspect_bits is the OR of the DD's four corners'
    /// ArSuspectCause bits (1 free, 2 relevel, 4 drop, 8 raim, 16 mw_unknown, 32 gf_jump):
    /// non-zero means the mask kept the DD out of the search, which tells a masked DD from
    /// one the ils_max_dim cap left out.  reentry is 1 on a searched DD one of whose corners
    /// the mask released at this search (the DD's first search after the release).
    inline constexpr const char* kProducerArSuspectColumns = ",suspect_bits,reentry";
    /// The four columns appended after those (before the header's newline) when
    /// network.ambiguity_resolution.cycle_basis is armed (card AGT, step AGT-5,
    /// filter/NetworkArCycleBasis.hpp).  cycle_kind is how the DD closes its chord: partner
    /// (against the station's link parent and link reference) or borrowed (against an earlier
    /// station sharing the most satellites); a chord no DD closes is a fundamental fallback,
    /// counted at the run end and never a row.  band_only is 1 when the chord exists in this
    /// band only.  basis_age_ep is the epochs since this DD identity (signal, sta_r, ref_sta,
    /// sat_j, ref_sat) entered the basis without a break (0 on its first epoch).  map_state is
    /// the DD's cycle against the last search's basis: same (that very cycle), mapped (an
    /// integer combination of its cycles), fresh_life (through an arc whose integer may have
    /// changed: freed this epoch or a new life stamp) or fresh_new (outside its span: a new arc,
    /// or no stored basis).
    inline constexpr const char* kProducerArCycleBasisColumns = ",cycle_kind,band_only,basis_age_ep,map_state";
    /// network_ar_lattice.csv header (card AGT, step AGT-6, widened by card FCF step FCF-3;
    /// filter/NetworkArLattice.hpp; network.ambiguity_resolution.trace_lattice).  One row per
    /// ledger step (an epoch the search built the cycle basis), after the seam gap, the evidence
    /// releases, the contradictions, the fold's re-validation, the admission and the fold's own
    /// releases, in that order.
    ///
    /// rows / arcs: the lattice dimension and the distinct band-arcs its rows hold, after the
    /// epoch.  fixes: the DDs the search determined (the trace's kept rows); added + confirmed
    /// + contradicted + not_primitive + waiting = fixes (waiting: not yet persist_ep epochs at
    /// one integer).  released: the releases (rel_vanished, the search did not read the arc;
    /// rel_first_seen, the last search did not; rel_freed, this epoch's free-epoch latch;
    /// rel_stamp, a change of the arc's life stamp that is not the echo of a life the last
    /// search began; rel_replay, the landed within-epoch replay's culprits; rel_gross, the
    /// gross-LOS Decide; rel_contradiction, a fix that contradicted the lattice; rel_revalidation,
    /// a unit that failed its float test; rel_acceptance, a unit dropped by held-solve block
    /// acceptance; rel_degenerate, a unit in a block the kernel refused; rel_seam_gap, a skipped
    /// epoch; their sum is released).  released_touching: those that found rows.  dims_lost: the
    /// rows dropped (an arc release costs one when a row holds the arc at +-1, every row
    /// through it otherwise, counted by no_unit; a support release costs its rows).  max_len /
    /// mean_len: the longest and the mean row (arcs) after the epoch.  units / units_failed:
    /// the L1/L2 units the lattice implied and those that failed their float test (the fold,
    /// source lattice).  The evidence is valid only while no hold pulls the floats the search
    /// fixes (card FCF keeps the estimator float).
    inline constexpr const char* kArLatticeHeader =
        "week,tow,rows,arcs,fixes,added,confirmed,contradicted,not_primitive,released,"
        "released_touching,dims_lost,no_unit,rel_vanished,rel_first_seen,rel_freed,rel_stamp,"
        "max_len,mean_len,waiting,rel_replay,rel_gross,rel_contradiction,rel_revalidation,"
        "rel_acceptance,rel_degenerate,rel_seam_gap,units,units_failed\n";
    /// network_ar_lattice_events.csv header (card AGT, step AGT-6; card FCF step FCF-3).  One row
    /// per release, in application order, then one per offer that is neither a confirmation nor
    /// waiting (event `added`, `contradicted` or `not_primitive`); a kept DD of
    /// network_producer_ar.csv at the same epoch with no row here was confirmed or waiting.  An
    /// offer row names the DD (sat_j, ref_sat, sta_r, ref_sta) and its offered integer
    /// (`value`); `implied` is the lattice's integer on a contradiction and `len` the new row's
    /// length on an addition.  An arc release (event `release`) names the arc (sat_j, sta_r,
    /// signal; the reference columns empty), its `cause`, the rows that held it and the rows
    /// dropped, the changed life-stamp fields (`change_bits`, the ArcLifeField bits: 1
    /// adm_generation, 2 adm_run_start, 4 has_carry, 8 birth_ep, 16 dropped_ep, 32 relevel_s,
    /// 64 flag_ep) and the free-epoch latch's cause bits (`free_cause`: 1 MW, 2 LLI, 4 IF-TD,
    /// 8 T4 attribution, 16 T3 consensus, 32 wind-up, 64 gap re-entry, 128 GF jump).  A cycle
    /// release (event `release_cycle`) names the DD whose arcs were released, its cause
    /// (contradiction with `value` and `implied`, revalidation, acceptance, degenerate), the rows
    /// that held any of its arcs and the rows dropped; a seam gap (event `release_all`) the rows
    /// dropped.  Graded against truth by scripts/simnet_ar_lattice_grade.py and
    /// scripts/simnet_fixed_cycle_grade.py.
    inline constexpr const char* kArLatticeEventsHeader =
        "week,tow,event,cons,signal,sat_j,ref_sat,sta_r,ref_sta,value,implied,len,cause,"
        "rows_touched,rows_dropped,change_bits,free_cause\n";
    /// network_fixed_cycles.csv header (card FCF, filter/NetworkFixedCycles.hpp;
    /// network.ambiguity_resolution.fixed_cycles.trace).  One row per unit-epoch: a DD
    /// identity (cons, sat_j, ref_sat, sta_r, ref_sta) the search determined in both bands
    /// (sig1 the shorter wavelength), with its integers n1 / n2, its floats y1_cyc / y2_cyc,
    /// their sigmas sd1_cyc / sd2_cyc and correlation rho from the search's readback, the unit
    /// statistic t_unit = v' Sigma^-1 v (v = y - n, chi-square with 2 dof under right
    /// integers), and its fate: the block it was placed in at the deciding pass (-1 when never
    /// placed), that block's rows block_k, its own statistic block_chi2, the statistic's mean
    /// under right integers block_chi2_expect and its bar chi2_K(1 - gate_alpha) block_bar, the
    /// deciding pass, and the action (folded, unit_refused, unplaced, dropped, left_float,
    /// degenerate, float_mismatch, non_finite, kernel_error, shift_refused; under source lattice
    /// also released and lapsed).  Graded against truth by scripts/simnet_fixed_cycle_grade.py.
    inline constexpr const char* kFixedCyclesHeader =
        "week,tow,cons,sig1,sig2,sat_j,ref_sat,sta_r,ref_sta,n1,n2,y1_cyc,y2_cyc,sd1_cyc,sd2_cyc,"
        "rho,t_unit,block,block_k,block_chi2,block_chi2_expect,block_bar,pass,action\n";
    /// network_fixed_cycle_shift.csv header (card FCF).  One row per emitted quantity of every
    /// epoch with an accepted held solve: kind clock (signal empty), code_bias, phase_bias or
    /// phase_visible (the phase bias minus the clock, the phase side a consumer applies), its
    /// value from the float shared estimate (float_m), from the held one (held_m) and their
    /// difference delta_m = held_m - float_m, in metres, before any emission filter or hold.
    inline constexpr const char* kFixedCycleShiftHeader =
        "week,tow,sat,kind,signal,float_m,held_m,delta_m\n";
    /// network_fixed_cycle_events.csv header (card FCF step FCF-3, source lattice; written under
    /// fixed_cycles.trace).  One row per edge of a folded unit: `onset` (the unit folded at this
    /// epoch and not at the step before, or folded again with other integers), `offset` (folded
    /// at the step before and not now; `cause` is its fate now: a unit action such as
    /// unit_refused, released, lapsed, dropped or left_float, else the release cause that took
    /// one of its arcs, else not_implied or out_of_basis), and `late_t3t4` (an arc of a unit folded at
    /// the step before whose life stamp now carries a post-fit flag: RAIM T3 / T4 or the
    /// gross-LOS Act acted after the emission of that step; the arc in `detail`).  The unit is
    /// its DD identity (cons, sig1 / sig2, sat_j, ref_sat, sta_r, ref_sta) with its integers
    /// n1 / n2; run_ep is the length of the folded run that ended (offset) or 0.
    inline constexpr const char* kFixedCycleEventsHeader =
        "week,tow,event,cons,sig1,sig2,sat_j,ref_sat,sta_r,ref_sta,n1,n2,run_ep,cause,detail\n";
    /// network_block_baarda.csv header (docs/SPEC_redundancy_census.md BB-INC-1..3).
    /// One row per (constellation family x label) per epoch that swept a block.
    ///
    /// The unit of `t_B` is a variance ratio, not a sigma.  T_B carries the 1/b, so
    /// E[t_B] = 1 under H0 for every block size, and the `n_gt_*` rungs mean "this
    /// block carries k times the variance the model allows".  The rungs are the same
    /// as network_prior_residual.csv's so one reader handles both files, but that
    /// file's |t| is in sigma units, so the two occupancy columns measure different
    /// quantities and are not comparable.
    ///
    /// `n_uncheckable` counts blocks whose residual cofactor was singular or
    /// non-finite: they are counted with their identity and never inverted through (a
    /// pseudo-inverted T_B has an unknown effective dof).  `n_block_only` (INC-3)
    /// counts blocks that cleared the threshold while each of their rows sat below it,
    /// the population no row-level tier can see.  The `c_*` columns are the INC-2
    /// contingency table (hi = above the report threshold, lo = at or below), all zero
    /// unless corroboration is armed; `c_hi_flat` is the b-cell.  `geo_cal` is the
    /// calibration indicator (1.0 = the family's sigma0 has converged), not the
    /// geometric mean of t_B: under H0 that mean is 2exp(psi(b/2))/b, which is
    /// 0.281/0.763/0.878 at b = 1/4/8 and would drift with the constellation's band
    /// plan.
    inline constexpr const char* kBlockBaardaHeader =
        "week,tow,family,label,n_blocks,n_rows,n_uncheckable,n_raw_formal,"
        "n_reported,n_block_only,"
        "sigma0,sigma0_warm,sigma0_neff,geo_cal,"
        "n_gt_1,n_gt_2,n_gt_3,n_gt_4,n_gt_6,n_gt_8,n_gt_12,n_gt_20,n_gt_50,"
        "n_gt_100,n_gt_200,"
        "max_t,max_T,max_row_t,max_b,max_station,max_sat,max_block_only,"
        "c_hi_noev,c_hi_flat,c_hi_step,c_lo_noev,c_lo_flat,c_lo_step\n";
    /// network_congruence.csv header (docs/SPEC_redundancy_census.md CG-INC-1..3).
    /// One row per swept station per epoch, not a ladder as in the two censuses
    /// above: SPEC §7.2 grades a per-station run-median of `d_e` and §7.3's MDB table
    /// is a per-station, per-direction quantity, and neither survives aggregation.
    /// The ladders are a run-end readout instead.
    ///
    /// `d_e/d_n/d_u` (metres, ENU) is the error of the held coordinate, not a
    /// correction: +East means the held mark sits that far East of where the data puts
    /// the antenna, so the station is corrected by subtracting it.  A +5 cm East
    /// perturbation injected into a held coordinate reads `d_e = +0.05`.
    ///
    /// The unit of `t` is a variance ratio, not a sigma.  `T_raw` carries the 1/q, so
    /// E[t] = 1 under H0 for every observable dimension.  The run-end ladder shares
    /// its rungs with network_prior_residual.csv and network_block_baarda.csv so one
    /// reader handles all three, but the prior-residual |t| is in sigma units and its
    /// occupancies are not comparable with these.
    ///
    /// `q` is the number of ENU directions that carried information; `q < 3` means the
    /// station clock and ZWD absorbed a direction, and `null_geom = 1` means none was
    /// observable (counted, never inverted through).  `mdb_*` are formal (delta_0 /
    /// sqrt of the pseudo-inverse diagonal, metres) and `mdb_s_*` are those times the
    /// empirical sigma0: the formal/scaled split the MDB card uses, which keeps the
    /// formal model's optimism visible.  Both are written as the literal `inf` when the
    /// direction is not estimable; `inf` marks a category, not a large number to act
    /// on.  `eig_min`/`eig_max` are M's extreme eigenvalues (1/m^2) and `null_e/n/u`
    /// the ENU components of the weakest direction, i.e. the direction the absorption
    /// takes.  `block_flag` (INC-3) joins the block-Baarda census: 1 = it flagged at
    /// least one LOS of this station this epoch, 0 = it did not, -1 = that census was
    /// not armed, so there is no verdict (distinct from a negative one).
    inline constexpr const char* kCongruenceHeader =
        "week,tow,station,label,b,n_code,n_phase,q,null_geom,"
        "d_e,d_n,d_u,T_raw,t,sigma0,raw_formal,sigma0_warm,sigma0_neff,"
        "mdb_e,mdb_n,mdb_u,mdb_s_e,mdb_s_n,mdb_s_u,"
        "eig_min,eig_max,null_e,null_n,null_u,block_flag,reported\n";
    /// network_datum_graph.csv header (docs/SPEC_datum_graph_census.md INC-2).
    /// One row per (epoch × constellation) the census touched: tier 1 saw the
    /// constellation, or a tier-2 cell or a contingency identity named it.  The
    /// pseudo-constellation `?` collects directions whose participants carry no
    /// satellite at all (SH-only modes), so they stay visible without being attributed
    /// to a guessed constellation.
    ///
    /// The two contingency partitions are over different populations.  The join is
    /// many-to-many (one direction can span several (sat, family) cells and one cell
    /// can appear in several directions), so `c_pred_obs + c_pred_only` counts
    /// predictions while `c_obs_only + c_obs_matched` counts directions, and the two
    /// totals are not expected to agree.  Combining them into one 2×2 table would
    /// double-count one side.  `c_pred_*` / `c_obs_only` are per constellation (the
    /// row's own share); `c_obs_matched` and the `s_*` companion cells are epoch-wide
    /// and repeat on every row of the epoch, because a direction is not owned by one
    /// constellation.
    ///
    /// `obs_src` labels the right-hand side of the join.  `sbc` = the directions carry
    /// (sat, family) identities (network.sbasis_complete armed); `count` = the spectrum
    /// reported extras but no identities, so only `obs_extras` is meaningful and every
    /// prediction lands in `predicted only` for lack of anything to match against;
    /// `absent` = the epoch never reached a rank evaluation.  A coverage number read
    /// off a `count` epoch is meaningless.
    ///
    /// `pred_t2` is the SPEC's strict predicate (registered PB columns, zero registered
    /// code keys in the family) and the only one that feeds the contingency.
    /// `pred_sup` is the companion predicate (registered code columns with zero
    /// admitted support): strictly weaker and nested, reported beside it so §6.3's
    /// falsification question is answered from the same run.  `no_layer` counts cells
    /// whose constellation registered no code-bias column at all, where both predicates
    /// are held off by construction.
    ///
    /// `ms` is the epoch's total census cost, repeated on every row of the epoch
    /// (there is one measurement per pass, not one per constellation).
    inline constexpr const char* kNetworkDatumGraphHeader =
        "week,tow,cons,datum_mode,k,d,pred_k1,n_sta,n_sat,n_edges,comps,"
        "n_cells,no_layer,pred_t2,pred_sup,"
        "obs_src,declared,observed,obs_extras,obs_dirs,"
        "c_pred_obs,c_pred_only,c_obs_only,c_obs_matched,s_pred_obs,s_pred_only,"
        "kp_armed,pred_obs_ids,pred_only_ids,obs_only_ids,ms\n";
    /// network_arc_ledger.csv header (card SID phase 1, @c network.signal_roster).
    /// One row per arc transition, never one per record: the ledger holds about
    /// 1 200 live arcs per epoch at 58 stations and about 6 000 at 305, so a
    /// per-record file would grow far faster than the edges the card is about.
    /// @c from and @c to are the state names (absent / tracking / gap / reentered /
    /// dead), @c gap_ep the epochs missed (meaningful at a reentry and at a death),
    /// @c generation the arc's birth count after the transition, @c reason the death
    /// cause (currently only "horizon", else empty) and @c role the roster's role for
    /// that signal (modelled or tracked, or empty).  Unlike the @c ARC event tier in
    /// network_events.csv, this file is not holdoff-throttled, so it holds every edge.
    /// Card SID phase 2 appends three columns and a second family of rows: the
    /// admitted lifecycle's own edges, which are recorded per edge nowhere else.
    /// @c side says which lifecycle the @c from / @c to / @c generation / @c gap_ep
    /// columns describe: "raw" for the receiver's own record (the build seam, upstream
    /// of the elevation mask, the allow list and every admission screen) and "adm" for
    /// the station's modelled rows, the population @c local_carry and the ambiguity
    /// states are keyed from.  A third kind, "carry" (with @c role "carry"), records
    /// the carry's gap prune as an arc death (card SID phase 3).  @c adm_state and
    /// @c adm_gap_ep report the admitted side as it stands on every row, so a raw
    /// re-entry carries both gap lengths; @c adm_gap_ep is -1 when the arc has never
    /// been admitted at all, which is a different statement from a zero-length gap.
    inline constexpr const char* kArcLedgerHeader =
        "week,tow,station,sat,signal,from,to,generation,gap_ep,reason,role,"
        "side,adm_state,adm_gap_ep\n";
    /// network_mdb.csv header (docs/SPEC_redundancy_census.md MDB-INC-1).
    /// One row per (constellation x row class) per epoch that binned a row.
    ///
    /// `n_rows` = `n_inf` + `n_finite`.  The order statistics are over the finite rows
    /// only: a median of a pool containing +infinity is either meaningless or silently
    /// censored depending on the sort, so the uncheckable rows are reported as their
    /// own count and kept out of every quantile.  `n_over` counts finite rows above
    /// `network.mdb_event_m`, the full population the per-cell event stream only
    /// samples.
    ///
    /// `mdb_*` are formal and inherit the optimism of the formal σ, because MDB scales
    /// linearly with σ (card D5 measured that optimism at 10–30x on this data).  The
    /// `mdb_scaled_*` columns are `mdb_* x sigma0_f`, the per-family empirical
    /// inflation from the P1-4 post-fit pass.  They are blank, never back-filled with
    /// the formal value or with 1.0, whenever `sigma0_src` starts with `absent`.
    /// `absent_sigma0_zero` on a phase row is the normal state under free ambiguities
    /// (each phase row is fitted exactly by its own ambiguity) and is itself the
    /// result: those rows carry no redundancy at all.  Quoting `mdb_med` alone to a
    /// consumer repeats the formal-σ optimism of D5 v1.
    ///
    /// A `sigma0_src` ending in `_deflating` means σ̂₀ < 1, so `mdb_scaled_*` is
    /// smaller than the formal `mdb_*`.  That reads as a tighter guarantee, and on the
    /// phase family it is the opposite: the deflated scale shrinks the finite rows of
    /// a bin that can be mostly uncheckable.  The value is reported rather than
    /// clamped (the prior-residual census's rule: the deflation is the measurement),
    /// but a deflating bin is read together with its `n_inf`, never quoted alone.
    inline constexpr const char* kNetworkMdbHeader =
        "week,tow,const,class,n_rows,n_inf,n_finite,n_over,"
        "mdb_med_m,mdb_p95_m,mdb_max_m,sigma0_f,sigma0_src,"
        "mdb_scaled_med_m,mdb_scaled_p95_m,mdb_scaled_max_m,"
        "worst_station,worst_sat,worst_r,worst_sigma_m\n";

    /**
     * @internal Owns every trace stream of one runNetwork() invocation plus its
     * on-disk path.  Member names match the former runNetwork() locals, so the run
     * loop binds `auto& X = traces.X;` references and every writer use-site stays
     * textually unchanged (the INC-3 migration mechanic).  network_trace.csv and
     * network_solution.csv always open; every other stream opens in openAll() only when
     * its arming toggle is set, and a toggle left off opens nothing and writes nothing
     * (byte-identical when off).
     */
    struct NetworkRunTraces {
        /// @param log_dir The run's log directory (streams live directly in it).
        explicit NetworkRunTraces(std::filesystem::path log_dir);

        NetworkRunTraces(const NetworkRunTraces&) = delete;
        NetworkRunTraces& operator=(const NetworkRunTraces&) = delete;

        /// The run's log directory (path base for every stream).
        const std::filesystem::path log_dir_;

        /// Closed-loop consumer metrics trace (closed_loop_trace.csv; opened only
        /// when the held-out consumer is armed; the path is local to openAll()).
        std::ofstream cl_trace;

        /// Per-satellite closed-loop consumer trace (closed_loop_sat_trace.csv; card
        /// CST).  Opened only when the held-out consumer is armed and
        /// @c network.trace_consumer_sat is set; the path is local to openAll().
        /// Same ownership split as @ref cl_trace: the stream lives here, the consumer
        /// engine harvests the rows, and the emit/store seam writes them through
        /// @ref writeConsumerSatRows.
        std::ofstream cl_sat_trace;

        /// The closed-loop consumer's receiver inter-system bias per non-GPS system
        /// (closed_loop_isb.csv; card GAL).  Opened only when the held-out consumer is
        /// armed and a constellation other than GPS is active
        /// (networkMultiGnssActive()), so a GPS-only run opens nothing.  One row per
        /// (epoch, system), stamped with the same (week, tow) as @ref cl_trace.
        std::ofstream cl_isb_trace;

        /// Always-on per-epoch estimability/service trace (P1-rotated).
        std::ofstream trace;
        const std::filesystem::path trace_path;
        /// Per-satellite product trace: the emitted solution (long format, one row per
        /// shared parameter per epoch), so the producer's output can be inspected for
        /// per-satellite clock-Δ continuity, phase-bias stability and discontinuity
        /// bumps.  Always on (P1-rotated).
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
        /// Per-epoch SH-field health trace (T1a; same gate as the coefficient trace):
        /// one row per emitted epoch with the estimated mean VTEC (a₀₀ and its formal
        /// σ), the coefficient RMS by degree, the ratio of that RMS to the per-degree
        /// Kaula prior σ, and the network-wide SH→slant misfit (mean/rms/max |residual|
        /// and the 3σ-exceedance counts).  It makes an a₀₀ divergence, and the
        /// per-(station, sat) misfit absorbed by the ambiguities, visible and
        /// attributable.
        std::ofstream vtec_sh_health_trace;
        const std::filesystem::path vtec_sh_health_path;
        /// Slip and RAIM diagnostics (network.trace_slip_events), three files: the
        /// IF-phase pre-detector's per-(station, sat, pair) ΔΔφ_IF with its full
        /// threshold decision each epoch (network_if_slip.csv); RAIM's per-(sat,
        /// station) post-fit standardized residual and cross-station consensus
        /// (network_raim.csv); and the log-only RAIM prominence attribution
        /// (network_raim_attrib.csv).  Together they show why an IF slip called for a
        /// reset and why RAIM tripped.
        std::ofstream if_slip_trace;
        const std::filesystem::path if_slip_trace_path;
        std::ofstream raim_trace;
        const std::filesystem::path raim_trace_path;
        std::ofstream raim_attrib_trace;
        const std::filesystem::path raim_attrib_trace_path;
        /// Per-satellite clock-monitor diagnostics (network.trace_slip_events with
        /// network.clock_monitor or network.clock_model): one row per satellite near or
        /// over the refloat bound each epoch (every satellite under
        /// network.trace_clock_full), so the abs/jump decision and the resulting reinit
        /// can be checked against the emitted clock series.  The `mode` column names
        /// the tier that wrote the row (monitor or model); model rows reuse the columns
        /// for the model's quantities.
        std::ofstream clk_mon_trace;
        const std::filesystem::path clk_mon_trace_path;
        /// Per-satellite information fractions of the a-priori clock anchor each
        /// emitted epoch (B7): anchor_info against the reduced-normals total_info, with
        /// the data and carry-prior shares split out, so validation can confirm that the
        /// estimable clock is determined by the data and not by the a-priori code
        /// product it is softly anchored to.  Diagnostic only (network.trace_anchor_info).
        std::ofstream anchor_info_trace;
        const std::filesystem::path anchor_info_trace_path;
        /// Cross-station between-satellite DD float ambiguities (from
        /// reconstructStation) per (const, signal), with their fractional-cycle
        /// residual (B5): a sign-convention self-check and the empirical evidence of
        /// < 0.1-cycle support that user AR needs (a gate for producer AR).  Diagnostic
        /// only (network.trace_dd_fractional).
        std::ofstream dd_frac_trace;
        const std::filesystem::path dd_frac_trace_path;
        /// Per-DD trace of the Phase-2 producer-AR search
        /// (network.ambiguity_resolution.trace with network.ambiguity_resolution.enable):
        /// the searched DD basis with its identities, float value, formal σ, subset
        /// membership and the integer the ILS returned.  Written by
        /// filter/NetworkProducerAr.cpp; graded against truth_ambiguities.csv by
        /// scripts/simnet_producer_ar_grade.py.  Columns: @ref kProducerArHeader.
        /// Diagnostic only: the emitted product is byte-identical with it on or off.
        std::ofstream producer_ar_trace;
        const std::filesystem::path producer_ar_trace_path;
        /// The producer AR search's lattice observatory (card AGT, step AGT-6;
        /// network.ambiguity_resolution.trace_lattice with .enable): one row per search epoch
        /// (@ref kArLatticeHeader) and one per release or non-confirming offer
        /// (@ref kArLatticeEventsHeader).  Written by filter/NetworkArLattice.cpp.
        /// Measure-only: the emitted product is byte-identical with it on or off.
        std::ofstream ar_lattice_trace;
        const std::filesystem::path ar_lattice_trace_path;
        std::ofstream ar_lattice_events_trace;
        const std::filesystem::path ar_lattice_events_trace_path;
        /// The fixed-cycle fold (card FCF; network.ambiguity_resolution.fixed_cycles.trace
        /// with the fold armed): one row per unit-epoch (@ref kFixedCyclesHeader) and the held
        /// minus float of every emitted quantity on an accepted epoch
        /// (@ref kFixedCycleShiftHeader).  Written by filter/NetworkFixedCycles.cpp.
        std::ofstream fixed_cycles_trace;
        const std::filesystem::path fixed_cycles_trace_path;
        std::ofstream fixed_cycle_shift_trace;
        const std::filesystem::path fixed_cycle_shift_trace_path;
        /// The fold's edges under source lattice (card FCF step FCF-3,
        /// @ref kFixedCycleEventsHeader).
        std::ofstream fixed_cycle_events_trace;
        const std::filesystem::path fixed_cycle_events_trace_path;
        /// Raw cycle-slip observables per (station, sat) every epoch
        /// (network.trace_slip_raw): Melbourne-Wübbena, geometry-free L1−L2, first-signal
        /// phase-minus-code (all m) and the per-band RINEX LLI, computed from
        /// raw_observed whether or not any detector is armed.  It answers whether any
        /// satellite slipped at a station, and it is the only network CSV that carries
        /// the receiver's LLI flags.  Epoch differences are formed offline.
        std::ofstream slip_raw_trace;
        const std::filesystem::path slip_raw_trace_path;
        /// Per-epoch disposition, σ̂0 and clock-statistics trace (L1;
        /// network.trace_epoch_status).  One row per processed epoch, emitted or
        /// skipped: the disposition timeline the V1/V3/V6 validation cards are graded
        /// on.  Off by default (byte-identical when off).
        std::ofstream epoch_status_trace;
        const std::filesystem::path epoch_status_trace_path;
        /// Operator health heartbeat (RTB-6; network.health_interval_s > 0): a
        /// machine-readable @c network_service_health.csv row appended every
        /// network.health_interval_s of content time, so a long-running service can be
        /// monitored without waiting for the exit summary.  The name says
        /// "service_health" because the pyviz viewer named network_health reads
        /// network_trace.csv, a different surface (per-epoch estimability, not service
        /// liveness).  sigma0_ewma reports −1 ("unavailable") unless its feeding tiers
        /// (emit_ura, raim and phase_leveling) are all armed: the EWMA updates only
        /// inside the warm post-fit RAIM block, so without them it would stay at its
        /// 1.0 initial value.  Off by default (byte-identical when off: no file opened,
        /// no rows written).  Column semantics: docs/rt_service_operations.md.
        std::ofstream health_trace;
        const std::filesystem::path health_trace_path;
        /// Structured per-event QC log (L2; network.trace_events): one long-format row
        /// per discrete QC action across all tiers, the machine-readable alternative to
        /// scraping the spdlog text.  Off by default (byte-identical when off).
        std::ofstream events_trace;
        const std::filesystem::path events_trace_path;
        /// Per-station ZWD trace (L7; network.trace_zwd): estimated ZWD, datum-conditioned
        /// formal σ and the carried flag, per present station each emitted epoch (the V7
        /// instrument).
        std::ofstream zwd_trace;
        const std::filesystem::path zwd_trace_path;
        /// Per-(satellite, code signal) bias state (L14; network.trace_sat_bias_states):
        /// the emitted satellite code bias with its σ̂0-scaled formal σ from the
        /// gauge-nulled shared posterior covariance.  The B product record carries no σ
        /// in any configuration, so this is the only channel from which the code-bias
        /// variance factor can be formed (SPEC_synthetic_minimum_network §4.8).
        std::ofstream sat_bias_states_trace;
        const std::filesystem::path sat_bias_states_trace_path;
        /// Per-station receiver states (L14b; network.trace_rx_states): the receiver
        /// clock and every receiver code-bias (DCB/IFB) state with its datum-conditioned
        /// formal σ, on the zwd_trace pattern.  The receiver half of §4.8: the W6 slice
        /// force-arms estimate_ifb, and those columns are not exported as estimates
        /// anywhere else.
        std::ofstream rx_states_trace;
        const std::filesystem::path rx_states_trace_path;
        /// Per-station residual field (L13; network.trace_station_residuals): every
        /// post-fit RAIM cell (non-pivot station x sat) with s_raw, s_norm and σ̂0.
        /// A heavy, opt-in trace.
        std::ofstream station_res_trace;
        const std::filesystem::path station_res_trace_path;
        /// Per-row Baarda redundancy census (network.trace_raim_redundancy;
        /// SPEC_t3_null_derivation §7.1): one row per post-fit RAIM phase row, keyed
        /// (station x sat x signal x row), with w, r_i, the raw residual and both
        /// standardizations.  The RAIM cell is a maximum over signals, so
        /// `station_res_trace` cannot separate L1C from L2W/L5Q; this trace can.
        /// Solve-neutral: r_i is computed for the trace and never used.  A heavy,
        /// opt-in trace.
        std::ofstream raim_redun_trace;
        const std::filesystem::path raim_redun_trace_path;
        /// The declared per-station variance factor (network.raim_station_factor;
        /// QUEUE item 4 R2): one row per (emitted epoch × present non-pivot station)
        /// with the per-epoch factor, its EWMA, the ratio to the network median of
        /// EWMAs, and the trim census.  Light (≤ n_sta rows per epoch).
        std::ofstream station_factor_trace;
        const std::filesystem::path station_factor_trace_path;
        /// The internal level-authority census (LV-6; network.level_authority_csgf):
        /// one FLEET row per epoch (L_cs, MAD, counts, and a00 when the SH channel
        /// exists) and one STA row per qualifying station (the tier-1 mean: the
        /// d_r,GF-class diagnostic and the sampling-matched grading population).
        /// Light (≤ 1 + n_sta rows per epoch).
        std::ofstream level_authority_trace;
        const std::filesystem::path level_authority_trace_path;
        /// The level-authority census's own per-LOS pierce points (LV-6 G2), in a
        /// separate file so the census CSV stays byte-identical; opened only inside the
        /// census's own gate.  @see GnssConfig::networkLevelAuthorityLosDump
        std::ofstream level_authority_los_trace;
        const std::filesystem::path level_authority_los_trace_path;
        /// Hourly measured carry-variance components (M2, LS-VCE; network.carry_vce):
        /// one row per (family, constellation) whenever the content hour rolls over.
        /// The *_floor_m2 columns are the measured constant-cofactor σ̂ to compare
        /// against the hand var_floor; rate_m2ps is the measured Δt-cofactor σ̂ to
        /// compare against q_amb; the *_ratio columns are floor / configured floor
        /// (> 1 ⇒ the hand floor is optimistic).  Off by default (byte-identical: no
        /// file, no rows).
        std::ofstream carry_vce_trace;
        const std::filesystem::path carry_vce_trace_path;
        /// The LASER 5-95 variance-factor trajectory (M2 v2;
        /// docs/SPEC_carry_vce_apply.md): one row per (family, constellation) per
        /// emitted epoch with α², its target, the σ̂²-under-used-weights convergence
        /// readout, and the clamp and freeze state.  Per-epoch because the hourly
        /// monitor digest beside it cannot show a slew ramp or a clamp excursion, and
        /// the acceptance is argued on those.  Opened only when
        /// @c network.carry_vce_apply is armed (byte-identical otherwise: no file, no
        /// rows).
        std::ofstream carry_vce_apply_trace;
        const std::filesystem::path carry_vce_apply_trace_path;
        /// Prior-residual census (network.prior_residual_census;
        /// docs/SPEC_prior_residual_census.md INC-1), the card's headline product: one
        /// row per (family x label) per censused epoch with the fixed |t| exceedance
        /// ladder, the row population, the chi2-corrected geometric mean of t^2, the
        /// per-family sigma0 with its warm-up state, the worst row's full identity and
        /// (INC-3) the six contingency cells.  It measures what a k*sigma0 gate on a
        /// family would cost before any response is wired to it.  Off by default
        /// (byte-identical when off: no file opened, no rows written).
        std::ofstream prior_residual_trace;
        const std::filesystem::path prior_residual_trace_path;
        std::ofstream block_baarda_trace;                       ///< docs/SPEC_redundancy_census.md BB-INC-1..3 ladder rows.
        const std::filesystem::path block_baarda_trace_path;
        std::ofstream congruence_trace;                         ///< docs/SPEC_redundancy_census.md CG-INC-1..3 per-station coordinate-offset rows (the MDB table's source).
        const std::filesystem::path congruence_trace_path;
        /// Baarda reliability (MDB) census (network.mdb; docs/SPEC_redundancy_census.md
        /// MDB-INC-1), the card's headline product: one row per (constellation x row
        /// class) per epoch that binned a row, with the formal MDB distribution (median
        /// / p95 / max, metres), the count of uncheckable rows whose MDB is +infinity,
        /// the count above the reporting threshold, the worst finite row's full
        /// identity, and the scaled variant together with the per-family empirical σ̂₀
        /// that produced it.  It answers per row, as a fault size, what the redundancy
        /// census answers only yes or no: how large a fault could go undetected here at
        /// this epoch.  Off by default (byte-identical when off: no file opened, no rows
        /// written).
        std::ofstream mdb_trace;
        const std::filesystem::path mdb_trace_path;
        /// Datum-graph census (network.datum_graph_census;
        /// docs/SPEC_datum_graph_census.md INC-2), the card's headline product: one row
        /// per (epoch × constellation) with the tier-1 component multiplicity and its
        /// roster, the tier-2 channel-family predictions, and the predicted-vs-observed
        /// contingency against the epoch's spectral near-null count.  The null-space
        /// gate and SBC report how many null directions there are; this census says
        /// why, in station and satellite identities, before the gate trips.  Off by
        /// default (byte-identical when off: no file opened, no rows written).
        std::ofstream datum_graph_trace;
        const std::filesystem::path datum_graph_trace_path;
        /// The machine-readable twin of the @c Network[estimability] WARN dump (card
        /// DG5; network.trace_estimability).  Long format with a leading @c kind column,
        /// so three record shapes share one file:
        ///   * @c EPOCH: one per dumped epoch, with tow, n_shared, declared_dim, ev_max,
        ///     cutoff and n_below;
        ///   * @c DIR: one per reported direction of that epoch, with its ev and the
        ///     ≤5 heaviest spanning columns as @c "label=weight" pairs (the same
        ///     pairs, in the same order, as the log line);
        ///   * @c RUN: one trailing record with the DG6 run totals: gate evaluations,
        ///     under/over and total withholds, emitted epochs and the resolved
        ///     tolerance in the numeric columns, then key=value tokens (observed
        ///     near-null min/max, heal attempts and heals, CB-admission phase-orphan
        ///     key epochs).  It gives a control arm run-wide gate telemetry without a
        ///     new run-end log line: the estim arm golden-gates the log text, so this
        ///     CSV is the only artifact a trace-armed run may add.
        /// scripts/characterize_storm.py prefers this file over parsing the WARN text;
        /// the text parser remains the fallback for archived runs.
        std::ofstream estimability_trace;
        const std::filesystem::path estimability_trace_path;
        /// Per-epoch spectrum of the reduced shared normals
        /// (@c network.trace_sh_spectrum; SPEC_day129_frame_guards P1-3): the five
        /// smallest eigenvalues, with the smallest direction's eigenvector mass
        /// partitioned across the clock / code-bias / phase-bias / SH-VTEC column
        /// families.  Unlike @ref estimability_trace, a datum-debugging dump, this is a
        /// time series built to test whether an a00 collapse coincides with a smallest
        /// mode that spans both the clocks and the field (the joint near-null slide of
        /// REPORT §3.2).  Nothing is logged, so the estimability WARN text stays golden.
        std::ofstream sh_spectrum_trace;
        const std::filesystem::path sh_spectrum_trace_path;
        /// Detail file for P1-3, written every @c network.trace_sh_spectrum_stride
        /// traced epochs (opened only when that stride is > 0): the heaviest per-column
        /// participations of the smallest mode, so a spanning direction can be named and
        /// not only weighed.
        std::ofstream sh_spectrum_mode_trace;
        const std::filesystem::path sh_spectrum_mode_trace_path;
        /// Keyed robust post-fit pools from the P1-4 residual pass
        /// (@c network.trace_postfit_keyed; SPEC_day129_frame_guards P1-4): one row per
        /// (kind ∈ {STA, SAT, SIG}, key) per emitted epoch, with {code, phase} pools of
        /// (n, σ̂0 = 1.4826·median, rms, max) of |v|√w.  It localizes the RHS misfit at
        /// onset: which stations (geography), which satellites, which signals
        /// (dispersive split).
        std::ofstream pf_keyed_trace;
        const std::filesystem::path pf_keyed_trace_path;
        /// Signed per-row post-fit residuals from the same P1-4 pass
        /// (@c network.trace_postfit_rows; SPEC_event_harness §24.6): one line per data
        /// row the pass visits and attributes to a family (kind ∈ {code, phase}), with
        /// columns @c week,tow,station,sat,signal,kind,resid_m,w,elev_deg.  @c resid_m is
        /// v = y − A_s·x_s − A_r·x_r (observed − predicted, metres, not standardized; the
        /// pass pools |v|·√w), @c w the row's information weight, @c elev_deg the LOS
        /// elevation from the station's modeled observation set (empty when the
        /// satellite is not in that set).  It attributes per LOS, which the keyed pools
        /// cannot: they pool per STA / SAT / SIG separately and drop the sign.
        std::ofstream pf_rows_trace;
        const std::filesystem::path pf_rows_trace_path;
        /// Per-satellite level-filter trace (card CLV; @c network.clock_level_carry,
        /// written under @c network.trace_events): one row per satellite per kept epoch
        /// the level filter acted on, with columns
        /// @c week,tow,sat,v_level_m2,i_level,n_rows_epoch,e_level_m,d_level_m,event.
        /// @c v_level_m2 = max(1 / I_L, floor^2) after the update (the level's code-only
        /// variance), @c i_level the filter's information I_L (1/m^2), @c n_rows_epoch
        /// the (station) ionosphere-free pairs of the update, @c e_level_m the level
        /// residual (the satellite's two-way effect in the post-fit ionosphere-free code
        /// residuals, positive when the clock is high), @c d_level_m the shift applied
        /// along the level direction, @c event one of arm / break / update.  Trace-class
        /// and fingerprint-safe.
        std::ofstream clock_level_trace;
        const std::filesystem::path clock_level_trace_path;
        /// The level monitor of card CTU (@c network.clock_level_monitor under
        /// @c network.trace_events): one row per satellite per kept epoch, @c p_level_m2
        /// the stashed block's variance along the satellite's level direction,
        /// @c inv_info_m2 the code-only bound 1/I_L, @c ratio their quotient (a
        /// consistent estimator keeps it at or above 1), @c raised_m2 the raise applied
        /// (0 when none), @c n_code_ep the epochs of code information in I_L.
        std::ofstream level_bound_trace;
        const std::filesystem::path level_bound_trace_path;
        /// The level innovation instrument of the monitor (card CTU section 14): one row per
        /// satellite per epoch.  @c z_m the post-fit two-way effect (constellation
        /// contrast), @c dclk_m the contrast of this epoch's clock update, @c nu_m their
        /// difference (the prefit innovation of the level), @c r_m2 the fit's variance of the
        /// effect, @c p_prior_m2 the prior clock contrast variance, @c s0sq the fit's unit
        /// variance, @c nis = nu^2 / (P + R).
        /// The Q3 guard's two statistics side by side (card CTU section 14,
        /// @c network.clock_anchor_guard_space not @c clock): one row per tested satellite
        /// per pass-0 epoch.
        std::ofstream anchor_guard_trace;
        const std::filesystem::path anchor_guard_trace_path;
        std::ofstream level_innov_trace;
        const std::filesystem::path level_innov_trace_path;
        /// The instrument's window rows: per satellite @c nubar_m (mean innovation), the two
        /// station halves' means, the white variance of the mean, the mean prior contrast
        /// variance and @c T = nubar^2 / var(nubar).
        std::ofstream level_innov_win_trace;
        const std::filesystem::path level_innov_win_trace_path;
        /// The phase-visible consistency instrument (card PVN,
        /// @c network.phase_visible_monitor): one row per satellite per epoch.
        /// @c sigs the phase-bias signals the functional took (one per band; a change of
        /// signal changes the functional's definition, so a series breaks there),
        /// @c d_hat_m the posterior ionosphere-free phase-visible clock with the constellation
        /// median removed, @c sqrt_sp_m its stated contrast sd, @c e_hat_m the contrast
        /// update against the fold's prior, @c sqrt_su_m the predicted update sd
        /// sqrt(S- - S+) (empty when not positive), @c nis = e^2 / (S- - S+).
        std::ofstream phase_visible_trace;
        const std::filesystem::path phase_visible_trace_path;
        /// The native broadcast clock a priori's wire stamp (card NBP,
        /// @c network.apriori_source @c sp3_brdc_clock): one row per clock record per
        /// emitted epoch.  @c iode the stamped record (-1: none selectable), @c c_est_m the
        /// estimate, @c offset_m the offset O of the stamped record, @c c_wire_m the value
        /// on the wire (c_est_m + offset_m), @c brdc_m c * brdc_k(t), @c held whether the
        /// store held the record at consume, @c switched whether the stamp changed since
        /// the satellite's previous emission.  A grader reading the wire's C forms
        /// C - O (= c_est_m) or brdc_m + c_wire_m (= the a priori plus the estimate).
        std::ofstream brdc_offset_trace;
        const std::filesystem::path brdc_offset_trace_path;
        /// The smoothed hold's clock clones (card SMH, @c network.emit_pb_hold_smoothed):
        /// one row per smoothed held emission.  @c age_s the clone's age, @c xfer_m the
        /// key's transfer (the phase-clock transfer minus phi, card SMH section 10),
        /// @c xfer_lah_m the phase-clock transfer computed in the same pass (empty on an
        /// epoch without the satellite's clock), @c phi_m the frame-free passed motion since
        /// the clone's birth, @c w_m2 the fold inflation applied to the clock since birth,
        /// @c vpost_m2 the posterior variance of the increment, @c var_m2 the row's
        /// residual variance, @c r = phi^2 / (W - V_post), @c pd0_m2 and @c f0 the stated
        /// variance of c - p_j at birth and the clock's share of it (they carry the
        /// float-ambiguity datum and are not G3's prediction inputs, card SMH section 9;
        /// empty on a pinned clock).  @c phi_m, @c w_m2, @c vpost_m2, @c var_m2 and @c r are the
        /// writing member's: @c member @c clk (card SMH's clock clone) or @c pv (card SMD's
        /// block clone, whose Phi_D writes).  The @c b_* columns are the key's block clone
        /// (card SMD) when it is live, on its own birth: its age, its clock row's Phi_c, Phi_D,
        /// the level part Phi_c - Phi_D, the fold inflation along D, the posterior variance of
        /// the D increment, r_D = Phi_D^2 / (W_D - V_D) and the band pair ("L1C+L2W").
        std::ofstream pb_hold_clone_trace;
        const std::filesystem::path pb_hold_clone_trace_path;
        /// Per-station local-block audit (@c network.trace_station_block;
        /// SPEC_event_harness §24.10, card ZP): one row per station per traced epoch (the
        /// first 20 emitted epochs, then every 60th) describing
        /// @f$N_{rr}=A_r^\top W A_r@f$ + folded station priors, the matrix
        /// @c NetworkEstimator::reconstructStation inverts with the shared states held
        /// at their solved values.  Columns: @c week,tow,station, the row counts by
        /// kind, @c n_local, @c rank, @c cond, the three smallest eigenpairs with the
        /// local states that span them, the ZWD column's effective and raw information,
        /// the reconstructed local correction's norm and ZWD element, and the
        /// zero-redundancy row counts; a leading `#` provenance line precedes the
        /// header.  It names which local direction is unresolved, not only that the
        /// block is ill-conditioned.
        std::ofstream station_block_trace;
        const std::filesystem::path station_block_trace_path;
        /// Per-arc ambiguity carry-state trace (@c network.trace_amb_state; RF-1 §7.4):
        /// one row per carried (station, satellite, signal) per traced epoch with what
        /// was carried (@c value_abs / @c var / @c var_first / @c var_birth), what was
        /// folded (@c fold_var, @c folded, @c route) and the arc's events (birth / drop /
        /// erase / stransform), plus an @c erase: row captured at each of the four erase
        /// sites immediately before the entry is deleted.  It traces the carry itself:
        /// the per-LOS code residual is only a proxy for the level, and the
        /// prior-residual census aggregates per (family, label) per epoch, so neither
        /// gives a per-arc trajectory.  Written by @ref GnssProcessor::networkAmbStateTrace
        /// from rows buffered in @c NetworkCarryState::amb_state_rows.  Columns:
        /// @ref GnssProcessor::kAmbStateHeader, after a leading `#` provenance line;
        /// reader: scripts/amb_state_read.py.  Measure-only and off by default (no file,
        /// no rows: byte-identical).
        std::ofstream amb_state_trace;
        const std::filesystem::path amb_state_trace_path;
        /// Per-line-of-sight phase-colour state trace (card LGM,
        /// @c network.trace_phase_colour with @c network.phase_colour_state): one row per
        /// harvested (station, satellite) per epoch, `week,tow,station,sat,elev_deg,x_m,
        /// sd_m,carried`, where @c x_m is the posterior estimate, @c sd_m the square root
        /// of its harvested conditional variance and @c carried 1 when a carried prior
        /// folded this epoch (0: the birth row).  Written by the driver from rows
        /// buffered in @c NetworkCarryState::pc_trace_rows.  Gate G1's instrument against
        /// the sim fixture's @c truth_phase_colour.csv.  Off by default (no file).
        std::ofstream phase_colour_trace;
        const std::filesystem::path phase_colour_trace_path;
        /// Card AIL: the ionosphere-free ambiguity information trace
        /// (@c network.trace_amb_if): `week,tow,station,sat,age_ep,n_los,prior_var_m2,
        /// post_var_m2,prior_var_dm_m2,post_var_dm_m2`, one row per line of sight whose two
        /// bands are carried in the station's block.  `prior` is the block the inject
        /// folded this epoch (NaN when the line of sight was not in it), `post` the harvest
        /// block; `_dm` with the station's common mode over its traced lines of sight of
        /// the same constellation projected out, and `n_los` the number of those lines of
        /// sight.  All conditional on the border.
        std::ofstream amb_if_trace;
        const std::filesystem::path amb_if_trace_path;
        /// One row per gauge-pinned ambiguity per processed epoch (S1;
        /// @c network.trace_phase_anchor): @c week,tow,cons,signal,sat,anchor_sta.  It is
        /// the (constellation, signal, satellite) -> anchoring station map that realizes
        /// the phase datum, which the lane-ladder spectrometer's anchor partition needs
        /// and no other artifact carries (@c network_anchor_info.csv is the a-priori
        /// clock anchor, with no signal and no station, and the DD-fractional trace skips
        /// pinned ambiguities by construction).  Log-only: it reads the election that
        /// already ran, so the solve is identical with it armed.
        std::ofstream phase_anchor_trace;
        const std::filesystem::path phase_anchor_trace_path;
        /// One row per (station, epoch, satellite) visit of the SD pre-reduce EWL
        /// appender (@c network.sd_ewl.trace; SPEC_sd_ewl_prereduce §6): the elected
        /// (band1, band2) phase pair and its code channels, the MW arc outcome
        /// (@c status), the arc's ZD mean and lagged count, and, for a built row, the SD
        /// @f$z@f$ and its floored σ (three integer columns are appended when
        /// @c network.sd_ewl.fix_enable is set).  It shows, per satellite, which signal
        /// pair the detector holds an EWL arc on, which the run-end counters can only
        /// aggregate.  Log-only: the solve is identical with the trace armed.  Rows are
        /// buffered per station in @c StationAssembly::sd_ewl_trace and drained by PASS
        /// B's serial consume, so the file order does not depend on the thread count.
        std::ofstream sd_ewl_trace;
        const std::filesystem::path sd_ewl_trace_path;
        /// One row per (station, satellite) the Route-B integer decision considered
        /// (increment 2a; @c network.sd_ewl.trace and @c network.sd_ewl.fix_enable): the
        /// elected network-common channel pair, the component reference station r0, the
        /// spanning-tree parent and link satellite, the DD it rounded with that DD's
        /// fractional residue, sigma and FFR, the resulting frame integer, and a
        /// @c status naming the outcome.  It is the only place the DD arithmetic behind a
        /// fixed row is visible; the run-end tallies only aggregate it.  Armed only with
        /// the integer path, so an increment-1 artifact set is unchanged.  Written in
        /// place from a serial seam, so no per-station buffering is needed.
        std::ofstream sd_ewl_ar_trace;
        const std::filesystem::path sd_ewl_ar_trace_path;
        /// Arc lifecycle trace (card SID phase 1; @c network.signal_roster): one row per
        /// (station, satellite, signal) transition, written through
        /// @ref writeArcLedgerRow from @c networkBuildSignalRoster (raw rows) and
        /// @c networkSignalRosterAudit (adm and carry rows).  Columns:
        /// @ref kArcLedgerHeader.  The complete edge record that the @c ARC event tier
        /// only samples.  Off by default (byte-identical when off: no file, no rows).
        std::ofstream arc_ledger_trace;
        const std::filesystem::path arc_ledger_trace_path;
        /// Run-provenance manifest path (L15; network_run_manifest.json).
        /// writeRunManifestStartup() writes the startup record and
        /// writeRunManifestComplete() appends the end-of-run record; one member keeps
        /// both on the same file.
        const std::filesystem::path manifest_path;

        /// Config echoes written into every epoch-status row (L1); the L15 manifest
        /// also records the σ̂0 trigger.  Captured once in openAll().
        double es_sigma0_trigger_   = 0.0;
        double es_withhold_ceiling_ = 0.0;
        /// Whether the two product-referenced admission census columns are part of
        /// this run's epoch-status schema (L1; docs/SPEC_measurement_admission.md
        /// INC-3): true when network.adm_trace or network.adm_screen is set.  Captured
        /// in openAll() because @ref emitEpochStatus receives neither the config nor the
        /// run context.  The card's off contract is structural byte-identity: appending
        /// the columns unconditionally, as the redundancy and prefit censuses do, would
        /// change the artifact of every unarmed run and break the same-build A/B
        /// comparisons its §7 gates depend on.
        bool es_adm_columns_ = false;
        /// Whether the five prior-residual census columns are part of this run's
        /// epoch-status schema (L1; docs/SPEC_prior_residual_census.md): true when
        /// network.prior_residual_census is set.  Captured in openAll() for the same
        /// reason as @ref es_adm_columns_: the census's off contract is structural
        /// byte-identity and its §9.1 gate is a same-build A/B comparison of this file,
        /// so the columns must not appear on an unarmed run.
        bool es_prires_columns_ = false;

        /// Whether the three constraint-aware global-σ̂₀ columns are part of this run's
        /// epoch-status schema (L1; docs/SPEC_prior_residual_census.md §14): true when
        /// network.prior_residual_census and network.prior_residual_global_sigma0 are
        /// both set.  A separate flag rather than three more columns under the census's
        /// own arm (@ref es_prires_columns_), because the census's §9.1 gate is a
        /// same-build A/B of this file: a census-only run keeps the census-only schema
        /// byte for byte.
        bool es_s0g_columns_ = false;

        /// Whether the four within-epoch-tier observability columns (sigma0_first,
        /// n_we_refloat_sats, we_clk_move_max_m, we_clk_move_max_sat) are part of this
        /// run's epoch-status schema (L1 / R0; docs/SPEC_pivot_visibility_gauge_defect.md
        /// §5.07.5).  Present iff network.raim_within_epoch is armed after loading (the
        /// loader also enables the tier when network.raim_withhold_sigma0 > 0).  This
        /// follows the adm/prires precedent (a run with the tier off keeps structural
        /// byte-identity), and the columns mean nothing without the tier (σ̂₀_first is
        /// that tier's own first-assess statistic).
        bool es_we_columns_ = false;

        /**
         * @brief Open every armed trace stream and write its header row (the
         *        runNetwork() startup trace-open scope, relocated unchanged).
         *
         * Creates @c log_dir_ first (the L15 manifest writer downstream relies on
         * this).  network_trace.csv and network_solution.csv always open; every other
         * stream opens only when its arming toggle is set.  An open failure logs a
         * warning and leaves the stream closed, so its writers do nothing.  Also
         * captures the epoch-status schema flags (the es_*_columns_ members) and the
         * L1 config echoes.  The B5 and raw-slip PppUtils wavelength providers are not
         * built here: they need the LogManager, which this unit does not hold, so the
         * caller builds them right after, gated on the streams this call opened.
         *
         * @param config_        Full config (getter-level toggle reads; the parameter
         *                       keeps the name of the member it aliased so the
         *                       relocated body is textually unchanged).
         * @param nc             Network config section (trace_* toggles).
         * @param shc            SH-VTEC config (trace gate for the coefficient trace).
         * @param sh_model       Co-estimated SH model, or nullptr; sizes the T1a
         *                       health-trace per-degree header columns (with nullptr
         *                       the health trace is opened without a header).
         * @param cl_trace_armed Closed-loop consumer armed (opens closed_loop_trace.csv).
         * @param log            main_log logger for open-failure warnings.
         */
        void openAll(const GnssConfig& config_, const NetworkConfig& nc,
                     const GlobalVtecShConfig& shc,
                     const SphericalHarmonicIonosphere* sh_model,
                     bool cl_trace_armed,
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
        // State-reading writers (decomposition INC-4), relocated once their
        // state lived in NetworkRunContext (knobs/counters) or arrived as
        // explicit values.  The relocated bodies are unchanged; those that
        // read former runNetwork() locals open with an alias block binding the
        // old names.  Writers added after INC-4 follow in this section.
        // ------------------------------------------------------------------

        /// L15 startup run-provenance manifest record (JSONL, truncating open).
        /// No-op unless nc.write_run_manifest.
        void writeRunManifestStartup(const GnssConfig& config_, const NetworkConfig& nc,
                                     const NetworkKnobs& knobs,
                                     const std::filesystem::path& config_path_,
                                     std::size_t n_stations, int n_threads,
                                     const std::shared_ptr<spdlog::logger>& log);

        /// One extra manifest record (JSONL, appended after the startup record): card
        /// PRM's @c param_model, the parameter registry as armed.  No-op unless
        /// nc.write_run_manifest.
        /// @param nc    The network config (the manifest switch).
        /// @param json  One complete JSON object, without a trailing newline.
        void appendRunManifestRecord(const NetworkConfig& nc, const std::string& json);

        /// L15 end-of-run manifest record (JSONL, appended, so a crashed run keeps
        /// the startup record).  No-op unless nc.write_run_manifest.
        /// @param extra_member Optional JSON member ("key":{...}) written before
        ///        `loop_ms` (card GAL); empty (the default) writes nothing, so the
        ///        record of a caller that passes none is unchanged.
        void writeRunManifestComplete(const NetworkConfig& nc,
                                      const NetworkServiceCounters& counters,
                                      std::size_t n_synced, double loop_ms,
                                      const std::string& extra_member = std::string());

        /// Size-based rotation (P1) of the three unbounded service CSVs (network_trace,
        /// network_solution, network_service_health; a closed stream is skipped).  Call
        /// once per processed epoch, before the epoch's rows are written.  With
        /// network.trace_rotate_mb at 0 (the default) nothing is touched.
        void rotateServiceCsvs(const NetworkConfig& nc,
                               const std::shared_ptr<spdlog::logger>& log);

        /// Operator health heartbeat row (RTB-6), written every nc.health_interval_s
        /// of content time while the stream is open and flushed after each row.
        /// Updates the heartbeat anchors inside @p counters (last_health_write_s and
        /// the P4(b) window anchors).
        void writeHealthRow(const GnssTime& t, const NetworkConfig& nc,
                            NetworkServiceCounters& counters,
                            const std::chrono::steady_clock::time_point& t_loop_begin,
                            bool ura_on, bool raim_on, bool pl_on,
                            double ura_sigma0_ewma,
                            const std::map<std::string, std::string>& last_pivot,
                            double grade_metric_s);

        /// One network_epoch_status.csv row (L1; no-op when the trace is off).
        void emitEpochStatus(const GnssTime& t, const char* disposition,
                             const EpochStatusRow& r);

        /**
         * @brief Write one consumer epoch's per-satellite rows to @ref cl_sat_trace
         *        (card CST; no-op when the stream is not open).
         *
         * Called from the emit/store seam immediately after
         * @c consumer->processEpoch, with the engine's own harvested buffer
         * (@c RealTimeEngine::consumerSatRows).  The epoch stamp is the producer's
         * @c se.time, the same stamp @c closed_loop_trace.csv uses for that consumer
         * epoch, so the two files join on (week, tow) with no tolerance.
         *
         * @param t    Epoch time stamped on every row.
         * @param rows The consumer's rows for this epoch (may be empty).
         * @return Rows written (0 when the stream is closed); the caller folds it
         *         into the run-end counters (invariant #4).
         */
        std::size_t writeConsumerSatRows(const GnssTime& t,
                                         const std::vector<ConsumerSatRow>& rows);

        /**
         * @brief Write one arc transition row (card SID phase 1; no-op when off).
         *
         * @param t          Epoch time (week / tow of the row).
         * @param station    Station name.
         * @param sat        Satellite id.
         * @param signal     RINEX signal id of the arc.
         * @param from       State before the transition.
         * @param to         State after it.
         * @param generation The arc's generation after the transition.
         * @param gap_ep     Epochs missed (meaningful at a reentry and at a death).
         * @param reason     Death cause, or an empty string.
         * @param role       The roster's role for this signal, or an empty string.
         * @param side       Which lifecycle the @p from / @p to / @p generation /
         *                   @p gap_ep columns describe: "raw" or "adm" (card SID
         *                   phase 2), or "carry" for a carry gap-prune death (card
         *                   SID phase 3).
         * @param adm_state  The admitted state after this row.
         * @param adm_gap_ep Epochs the admitted side has missed as of this epoch;
         *                   -1 when the arc has never been admitted.
         */
        void writeArcLedgerRow(const GnssTime& t, const std::string& station,
                               const std::string& sat, const std::string& signal,
                               const char* from, const char* to, int generation,
                               int gap_ep, const char* reason, const char* role,
                               const char* side, const char* adm_state,
                               long long adm_gap_ep);

        /// One network_events.csv row for a discrete QC action (L2; no-op when off).
        /// Commas in @p detail become semicolons, so it stays one CSV field.
        void emitEvent(const GnssTime& t, const char* tier, const char* event,
                       const std::string& sat, const std::string& station,
                       std::optional<double> statistic, std::optional<double> threshold,
                       const char* action, const std::string& detail);

        /**
         * @brief Write this epoch's variance-factor row for every group (M2 v2).
         *
         * One @c kCarryVceApplyHeader row per (family × constellation) known to
         * @p apply, written every emitted epoch the apply path stepped.  No-op when
         * the stream is closed (the apply path unarmed), so the call site needs no
         * second gate.
         *
         * @param t          epoch time (week/tow of the row).
         * @param apply      the factor engine (read-only).
         * @param hour_state caller-owned content-hour bucket; the stream is flushed
         *                   when it rolls.  Nothing else flushes this stream, and it
         *                   is the card's graded artifact, so the hourly flush limits
         *                   what a killed run can lose to one content hour.
         */
        void writeCarryVceApplyRows(const GnssTime& t,
                                    const NetworkCarryVceApply& apply,
                                    int& hour_state);

        /// The @c EPOCH record of one estimability dump (card DG5; no-op when the
        /// trace is off).  Fed from @c NetworkEstimator::dumpEstimabilitySpectrum's
        /// return value, so the row and the WARN line carry the same numbers by
        /// construction.  Takes primitives rather than the nested
        /// @c EstimabilityDump so this header does not need the estimator include,
        /// which pulls in Eigen.
        void writeEstimabilityEpochRow(const GnssTime& t, double tow, int n_shared,
                                       int declared_dim, double ev_max, double cutoff,
                                       int n_below);

        /// One @c DIR record (card DG5): a reported near-null direction's eigenvalue
        /// and its ≤5 heaviest spanning columns as (label, |component|) pairs, in the
        /// log line's order (no-op when the trace is off).
        void writeEstimabilityDirRow(const GnssTime& t, double ev,
                                     const std::vector<std::pair<std::string, double>>& top);

        /// The single trailing @c RUN record with the run-wide rank-gate telemetry
        /// (card DG6; no-op when the trace is off).  Written once at teardown, before
        /// @ref closeAll.
        /// @param rank_gate_tol The resolved tolerance the run used
        ///                      (NetworkEstimator::rankGateTol); the calibration
        ///                      evidence cannot be read without the value it came from.
        void writeEstimabilityRunRow(const NetworkServiceCounters& counters,
                                     double rank_gate_tol);

        /// Flag-rate empirical-α summary (B8; log lines and network_flag_rates.csv)
        /// and the L3 run-summary CSV.  The single knobs.trace_flag_rates gate covers
        /// both, so a run with it off writes neither.  @p fr_if_refgate is tier T1r, the
        /// IF-TD reference arm's flag rate, summed over the station detectors by the
        /// run-summary report (networkRunSummaryRunEnd) just before the call.
        void writeFlagRateAndRunSummary(const GnssConfig& config_, const NetworkKnobs& knobs,
                                        const NetworkServiceCounters& counters,
                                        const FlagRate& fr_if_refgate,
                                        std::size_t n_synced, std::size_t n_stations,
                                        int n_threads, bool parallel_on, double loop_ms,
                                        const std::shared_ptr<spdlog::logger>& log);

        /// Teardown close: the streams of the former inline close list, plus later
        /// members whose tails are graded artifacts and are flushed here rather than
        /// at scope exit.  The remaining streams close in the destructor.
        void closeAll();
    };

} // namespace GnssProcessor
