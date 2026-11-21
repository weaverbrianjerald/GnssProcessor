/**
 * @file test_run_traces_open_set.cpp
 * @brief Which trace files NetworkRunTraces::openAll creates, under which arming, and the
 *        exact text it writes into each before the first row.
 *
 * openAll is one long function that opens every trace stream of a network run, each behind
 * its own condition (a network knob, a configuration getter, the closed-loop arming, or a
 * combination), and writes each file's column header, sometimes with columns that depend on
 * further knobs.  The streams are expected to move into a table; these cases record what the
 * function does today so that such a rewrite has to reproduce it file for file and byte for
 * byte, or change this record on purpose.
 *
 * Everything is observed on disk: the set of files present in the log folder after the
 * stream owner is closed and destroyed, and the full content of each file (header-only, since
 * nothing writes rows here).  The stream members themselves are not inspected, so a rewrite
 * may rename or regroup them freely.
 *
 * How the arming is built.  openAll reads two inputs: the configuration object (getters
 * that only the loader can set) and the network knob struct passed beside it.  The
 * configuration is loaded from JSON; the knob struct is the loaded one with every trace gate
 * switched on in the test, because several gates are refused together by the loader
 * (an integer search trace without the search, the fixed-cycle fold without its source) and
 * openAll is a function of the struct it is given.  Three loaded configurations are needed to
 * reach every stream, because their datums exclude one another:
 *  - the sim reference posture with the configuration-gated instruments armed on it (the
 *    smoothed phase-bias hold and the broadcast clock a priori among them);
 *  - the minimal two-station network on the single-receiver chain datum, the only arming of
 *    the four chain files;
 *  - the minimal network with the co-estimated spherical-harmonic ionosphere, the only arming
 *    of the two SH files (the reference posture pins the satellite code bias at the broadcast
 *    group delay, which the loader refuses together with the SH field).
 * The union of the three sets is every stream openAll can open (65 network files plus 5
 * closed-loop files).  The run manifest is not opened by openAll and is not part of the set.
 *
 * Two files are opened whatever the arming: network_trace.csv and network_solution.csv.  The
 * all-off case therefore expects exactly those two, not an empty folder.
 */

#include <gtest/gtest.h>

#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"

#include "ConfigLoadCapture.hpp"          // TestSupport::networkConfig(): the minimal two-station network
#include "../support/TestTempDir.hpp"     // per-process, per-test scratch directory

#include <nlohmann/json.hpp>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// The record: every file openAll can create and the text it writes there.
// ---------------------------------------------------------------------------------------------

// network_epoch_status.csv grows by four optional column groups; each group is kept apart so
// a configuration can assemble the variant its arming selects.
constexpr const char* kEpochStatusBase =
    "week,tow,disposition,sigma0,n_adapt,n_present,"
    "n_clock_sats,clock_median_m,clock_std_m,worst_sat,"
    "worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,"
    "sigma0_trigger,withhold_ceiling,n_arc_refloat,"
    "n_d5_flags,n_d5_retaps,n_chord_births,n_slip_refloats,"
    "n_dw_engaged,n_dw_zero,dw_min_w,dw_mean_w,"
    "sigma0_pf,sigma0_pf_code,sigma0_pf_phase,n_pf_rows,"
    "n_redun_rows,n_uncheckable_rows,uncheckable_max_s,"
    "n_unobservable_local,"
    "pc_code_max_s,pc_code_p99_s,pc_code_n10,pc_code_n1k,"
    "pc_phase_max_s,pc_phase_p99_s,pc_phase_n10,pc_phase_n1k,"
    "pc_other_max_s,pc_other_p99_s,pc_other_n10,pc_other_n1k,"
    "pc_sh_max_s,pc_sh_p99_s,pc_sh_n10,pc_sh_n1k,"
    "ddfold_degen_blocks,ddfold_degen_rows,"
    "ddfold_min_pivot_rel";
constexpr const char* kEpochStatusAdm    = ",adm_sta_worst_z,adm_net_worst_z";
constexpr const char* kEpochStatusPrires = ",prires_rows,prires_rows_resp,prires_rows_frame,"
                                           "prires_n_reported,prires_max_t";
constexpr const char* kEpochStatusS0g    = ",sigma0_global,prior_tension_ratio,s0g_flags";
constexpr const char* kEpochStatusWe     = ",sigma0_first,n_we_refloat_sats,"
                                           "we_clk_move_max_m,we_clk_move_max_sat";

// The provenance line the three self-describing files write above their column header.
constexpr const char* kStationBlockProvenance =
    "# network_station_block.csv - SPEC_event_harness 24.10 "
    "(network.trace_station_block). One row per station per TRACED epoch "
    "(the first 20 EMITTED epochs, then every 60th). The audited object is "
    "the station's LOCAL normal block N_rr = A_r^T W A_r + folded station "
    "priors, with the SHARED states held at their solved values x_s - the "
    "same conditioning as NetworkEstimator::reconstructStation "
    "(x_r = N_rr^-1 (u_r - N_rs x_s)); it is NOT the station's stand-alone "
    "normal matrix. rank = #{eig > 1e-9*eig_max}; cond = eig_max/eig_min "
    "over that non-null subspace. evK_span names the local states carrying "
    "the eigenvector's squared loading as up to 3 'kind=share' items joined "
    "by '|' (kinds clk, zwd, rxcb:<sig>, iono:<sat>, amb:<sat>:<sig>, and "
    "pos/vel/clkrate/phclk/grdn/grde/dzwd/ionorate/isb/satbias/satpb/rxpb/"
    "satclk where the posture allocates them), "
    "truncated once the cumulative share reaches 0.90. zwd_info_eff = "
    "1/(N_rr^+)_zz, the ZWD column's information after eliminating every "
    "OTHER local state (identically N_zz - N_zo N_oo^-1 N_oz); zwd_info_raw "
    "= N_zz = sum w*m_wet^2 over the station's rows; ratio = eff/raw. "
    "x_r_* are from reconstructStation. zero_red_* count DATA rows with "
    "redundancy r_i = 1 - w_i h_i^T N_rr^+ h_i < 1e-6 (h_i the row's LOCAL "
    "partials, pseudo-inverse on the non-null subspace); n_rows is the "
    "count of DATA rows visited (coupling rows excluded).\n";
constexpr const char* kAmbStateProvenance =
    "# network_amb_state.csv - RF-1 7.4 (docs/refound/"
    "RF-1_float_ambiguities_design.md, network.trace_amb_state). One row "
    "per CARRIED (station, satellite, signal) per traced epoch, written "
    "after that epoch's leveling harvest, plus one 'erase:' row per "
    "entry deleted this epoch captured immediately BEFORE deletion. "
    "value_abs_m / var_m2 are POST-harvest (this epoch's solve); "
    "var_first_m2 is the arc's unfloored first-stash variance and "
    "var_birth_m2 the same floored by network.amb_float_birth_var_m2 "
    "(equal when the floor is 0). fold_var_m2 is the variance the PASS-B "
    "inject ACTUALLY folded with (after q*gap, the var floor and the M2 "
    "VCE factor), 0 when folded=0; route is the "
    "route the inject took (diag | block | coupling-scalar), empty when "
    "folded=0. n_folds counts only epochs a prior ACTED, so it is not "
    "age_ep: age_ep counts loop epochs since the first stash (-1 = "
    "warm-resume-restored, 0 = unknown) and age_since_relevel_ep epochs "
    "since the last re-level (-1 = unstamped => mature). event is '' | "
    "birth | drop:<cause> | erase:<cause> | stransform; on the LEVEL "
    "vehicle the drop causes are the response paths that skip an inject "
    "(relevel_free, stale_gap, reseed) and the erase causes are the "
    "paths that delete the entry (t4_attrib, t3_consensus, gap_prune, "
    "anchor_reref). The prior residual D4 standardizes is formed offline "
    "from CONSECUTIVE rows of one arc: e = value_abs(t-1) - value_abs(t) "
    "over fold_var_m2(t), the same pair network_prior_residual.csv's pl "
    "family folds.\n";
constexpr const char* kArcLedgerProvenance =
    "# network_arc_ledger.csv - signal identity phase 1 (docs/refound/"
    "SID_signal_identity_2026-09-22.md, network.signal_roster). One row "
    "per (station, satellite, signal) arc TRANSITION, never one per "
    "record: Absent->Tracking is a birth, Tracking->Gap opens a gap, "
    "Gap->Reentered is a return inside the readers' longest gap window, "
    "Gap->Dead is a death past it, and Dead->Tracking is a birth whose "
    "generation says the arc had been here before. An arc whose STATION "
    "reported nothing at an epoch is frozen, never gapped. Not "
    "holdoff-throttled (the ARC tier of network_events.csv is). Signal identity "
    "phase 2: `side` says which lifecycle from/to/generation/gap_ep "
    "describe - raw (the receiver's record, upstream of the elevation "
    "mask, the allow list and every admission screen) or adm (the "
    "station's MODELLED rows, the population local_carry and the "
    "ambiguity states are keyed from); adm_state and adm_gap_ep report "
    "the admitted side on every row, and adm_gap_ep is -1 when the arc "
    "has never been admitted at all.\n";

// Degree of the SH model handed to openAll and the second-shell degree of the SH knobs in the
// armed SH case; the health header's per-degree columns are sized from them.
constexpr int kShDegree       = 2;
constexpr int kShLayer2Degree = 1;

/// The epoch-status header for one arming of its optional column groups.
std::string epochStatusHeader(bool adm, bool prires, bool s0g, bool we) {
    std::string h = kEpochStatusBase;
    if (adm) h += kEpochStatusAdm;
    if (prires) h += kEpochStatusPrires;
    if (s0g) h += kEpochStatusS0g;
    if (we) h += kEpochStatusWe;
    return h + "\n";
}

/**
 * Every file openAll can create, with its full content when every column modifier is on
 * (the producer-AR search decorrelated, with the suspect mask and the cycle basis; the SD-EWL
 * integer path; the second SH shell; the epoch-status admission, prior-residual and global
 * sigma0 groups).  @p we selects the epoch-status within-epoch group, which follows the
 * configuration rather than a knob.
 */
std::map<std::string, std::string> recordedContent(bool we) {
    std::map<std::string, std::string> t;
    // Opened whatever the arming.
    t["network_trace.csv"] =
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,"
        "gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins\n";
    t["network_solution.csv"] = "week,tow,sat,kind,signal,value_m,disc_counter\n";
    // Network knobs.
    t["network_ionosphere.csv"] =
        "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg,x_lin_m,iono_total_m\n";
    t["network_if_slip.csv"] =
        "week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,"
        "s_pool_m,ewma_scale_m,count,w_shrink,s_eff_m,elev_map,"
        "thr_m,floor_m,abs_dev_m,flagged,carry_warm,is_ref\n";
    t["network_raim.csv"] =
        "week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,"
        "n_sta_spiked,min_sta,refloated\n";
    t["network_raim_attrib.csv"] =
        "week,tow,dim,entity,effect,prominence,n_cells,overall,sigma0,acted\n";
    t["network_anchor_info.csv"] =
        "week,tow,sat,anchor_info,carry_info,data_info,total_info,"
        "anchor_frac_data,carry_frac,frac,is_pivot\n";
    t["network_dd_fractional.csv"] =
        "week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,"
        "dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc,sigma_carry_aware\n";
    t["network_producer_ar.csv"] =
        "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
        "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed,"
        "wl_float_sigma_cyc,wl_cond_cyc,wl_cond_sigma_cyc,nl_cond_sigma_cyc"
        ",suspect_bits,reentry"
        ",cycle_kind,band_only,basis_age_ep,map_state\n";
    t["network_ar_lattice.csv"] =
        "week,tow,rows,arcs,fixes,added,confirmed,contradicted,not_primitive,released,"
        "released_touching,dims_lost,no_unit,rel_vanished,rel_first_seen,rel_freed,rel_stamp,"
        "max_len,mean_len,waiting,rel_replay,rel_gross,rel_contradiction,rel_revalidation,"
        "rel_acceptance,rel_degenerate,rel_seam_gap,units,units_failed\n";
    t["network_ar_lattice_events.csv"] =
        "week,tow,event,cons,signal,sat_j,ref_sat,sta_r,ref_sta,value,implied,len,cause,"
        "rows_touched,rows_dropped,change_bits,free_cause\n";
    t["network_fixed_cycles.csv"] =
        "week,tow,cons,sig1,sig2,sat_j,ref_sat,sta_r,ref_sta,n1,n2,y1_cyc,y2_cyc,sd1_cyc,sd2_cyc,"
        "rho,t_unit,block,block_k,block_chi2,block_chi2_expect,block_bar,pass,action\n";
    t["network_fixed_cycle_shift.csv"] = "week,tow,sat,kind,signal,float_m,held_m,delta_m\n";
    t["network_fixed_cycle_events.csv"] =
        "week,tow,event,cons,sig1,sig2,sat_j,ref_sat,sta_r,ref_sta,n1,n2,run_ep,cause,detail\n";
    t["network_slip_raw.csv"] =
        "week,tow,station,sat,sig1,sig2,elev_deg,mw_m,gf_m,l1_c1_m,lli1,lli2\n";
    t["network_epoch_status.csv"] = epochStatusHeader(true, true, true, we);
    t["network_sh_spectrum.csv"] =
        "week,tow,n_shared,n_clk,n_cb,n_pb,n_sh,"
        "ev_min,ev_max,cond,ev1,ev2,ev3,ev4,ev5,"
        "mass_clk,mass_cb,mass_pb,mass_sh,"
        "span_clk,span_cb,span_pb,span_sh,cutoff\n";
    t["network_sh_spectrum_mode.csv"] = "week,tow,mode_rank,ev,col_label,component\n";
    t["network_pf_keyed.csv"] =
        "week,tow,kind,key,n_code,s0_code,rms_code,max_code,"
        "n_phase,s0_phase,rms_phase,max_phase\n";
    t["network_pf_rows.csv"] = "week,tow,station,sat,signal,kind,resid_m,w,elev_deg\n";
    t["network_pb_hold_clone.csv"] =
        "week,tow,sat,sig,gen,age_s,xfer_m,xfer_lah_m,phi_m,w_m2,"
        "vpost_m2,var_m2,r,pd0_m2,f0,member,b_age_s,b_phic_m,"
        "b_phid_m,b_lvl_m,b_wd_m2,b_vpostd_m2,b_rd,b_pair,"
        "b_revd_m,b_sd_m,b_vd0_m2,b_vd0b_m2,b_pd_m2,b_dwd_m2\n";
    t["network_station_block.csv"] =
        std::string(kStationBlockProvenance) +
        "week,tow,station,n_rows_code,n_rows_phase,n_local,rank,cond,"
        "ev1_val,ev1_span,ev2_val,ev2_span,ev3_val,ev3_span,"
        "zwd_info_eff,zwd_info_raw,zwd_info_ratio,x_r_norm,x_r_zwd,"
        "zero_red_code,zero_red_phase,n_rows\n";
    t["network_amb_state.csv"] =
        std::string(kAmbStateProvenance) +
        "week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,"
        "fold_var_m2,n_folds,age_ep,age_since_relevel_ep,folded,route,event\n";
    t["network_amb_if.csv"] =
        "week,tow,station,sat,age_ep,n_los,prior_var_m2,post_var_m2,"
        "prior_var_dm_m2,post_var_dm_m2\n";
    t["network_phase_colour.csv"] = "week,tow,station,sat,elev_deg,x_m,sd_m,carried\n";
    t["network_phase_anchor.csv"] = "week,tow,cons,signal,sat,anchor_sta\n";
    t["network_sd_ewl.csv"] =
        "week,tow,station,cons,sat,pivot_sat,status,"
        "sig1,sig2,code1,code2,n_cand_pairs,arc_count,bia,"
        "lambda_ewl_m,mean_m,pred_m,resid_m,sigma_m"
        ",n_fix_cyc,fixed,grp_fixed\n";
    t["network_sd_ewl_ar.csv"] =
        "week,tow,cons,sat,station,status,sig1,sig2,r0,parent,"
        "link_sat,arc_count,dd_cyc,frac_cyc,sigma_cyc,ffr,"
        "n_cyc,n_prev_cyc\n";
    t["network_service_health.csv"] =
        "week,tow,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,"
        "n_runaway_epochs,sigma0_ewma,n_present,n_clock_sats,"
        "ms_per_epoch_window,last_emit_age_s,pivot,cold_start_grade_s\n";
    t["network_prior_residual.csv"] =
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
    t["network_block_baarda.csv"] =
        "week,tow,family,label,n_blocks,n_rows,n_uncheckable,n_raw_formal,"
        "n_reported,n_block_only,"
        "sigma0,sigma0_warm,sigma0_neff,geo_cal,"
        "n_gt_1,n_gt_2,n_gt_3,n_gt_4,n_gt_6,n_gt_8,n_gt_12,n_gt_20,n_gt_50,"
        "n_gt_100,n_gt_200,"
        "max_t,max_T,max_row_t,max_b,max_station,max_sat,max_block_only,"
        "c_hi_noev,c_hi_flat,c_hi_step,c_lo_noev,c_lo_flat,c_lo_step\n";
    t["network_congruence.csv"] =
        "week,tow,station,label,b,n_code,n_phase,q,null_geom,"
        "d_e,d_n,d_u,T_raw,t,sigma0,raw_formal,sigma0_warm,sigma0_neff,"
        "mdb_e,mdb_n,mdb_u,mdb_s_e,mdb_s_n,mdb_s_u,"
        "eig_min,eig_max,null_e,null_n,null_u,block_flag,reported\n";
    t["network_mdb.csv"] =
        "week,tow,const,class,n_rows,n_inf,n_finite,n_over,"
        "mdb_med_m,mdb_p95_m,mdb_max_m,sigma0_f,sigma0_src,"
        "mdb_scaled_med_m,mdb_scaled_p95_m,mdb_scaled_max_m,"
        "worst_station,worst_sat,worst_r,worst_sigma_m\n";
    t["network_datum_graph.csv"] =
        "week,tow,cons,datum_mode,k,d,pred_k1,n_sta,n_sat,n_edges,comps,"
        "n_cells,no_layer,pred_t2,pred_sup,"
        "obs_src,declared,observed,obs_extras,obs_dirs,"
        "c_pred_obs,c_pred_only,c_obs_only,c_obs_matched,s_pred_obs,s_pred_only,"
        "kp_armed,pred_obs_ids,pred_only_ids,obs_only_ids,ms\n";
    t["network_estimability.csv"] =
        "kind,week,tow,n_shared,declared_dim,ev_max,cutoff,"
        "n_below,ev,col1,col2,col3,col4,col5,col6,col7,col8\n";
    t["network_events.csv"] =
        "week,tow,tier,event,sat,station,statistic,threshold,action,detail\n";
    t["network_zwd.csv"] = "week,tow,station,zwd_m,std_m,carried\n";
    t["network_tropo_gradient.csv"] =
        "week,tow,station,north_m,east_m,north_std_m,east_std_m\n";
    t["network_sat_bias_states.csv"] = "week,tow,sat,signal,code_bias_m,sigma_m,s0,pinned\n";
    t["network_rx_states.csv"] = "week,tow,station,kind,key,value_m,std_m\n";
    t["network_pb_prehold.csv"] =
        "week,tow,sat,signal,est_pb_m,est_pb_sigma_m,est_clk_m,"
        "phase_visible_m,emit_pb_m,gen,role,comp,r1,attach_sta,depth\n";
    t["network_windup_arcs.csv"] = "week,tow,station,sat,kind,cycles,start_tow\n";
    t["network_station_residuals.csv"] = "week,tow,station,sat,s_raw,s_norm,sigma0\n";
    t["network_raim_redundancy.csv"] =
        "week,tow,station,sat,signal,row,w,r_i,resid_m,"
        "z_meas,z_baarda,below_floor\n";
    // The closed-loop consumer's files (armed by the run, plus their own knobs).
    t["closed_loop_trace.csv"] =
        "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
        "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ,"
        "sig_e_m,sig_n_m,sig_u_m,sig_3d_m,"
        "search_ratio,n_pairs,n_held,held_joined,held_left\n";
    t["closed_loop_sat_trace.csv"] =
        "week,tow,sat,sig,code_sig,elev_deg,n_sig,"
        "clk_m,cb_m,pb_m,"
        "clk_disc,cb_disc,pb_disc,"
        "ev_pb,ev_cb,ev_clk,"
        "innov_code_m,innov_phase_m,"
        "res_code_m,res_phase_m,"
        "amb_m,amb_var,iono_m,iono_var\n";
    t["closed_loop_ar_fixes.csv"] =
        "week,tow,system,signal,sat,ref_sat,cmp,float_cyc,cond_sigma_cyc,fixed_int,determined,kept,"
        "z_fixed,z_total,ratio,p_s,gen_sat,gen_ref\n";
    t["closed_loop_ar_epoch.csv"] =
        "week,tow,n_pairs,n_wl_det,n_band_det,fix_e,fix_n,fix_u,float_e,float_n,float_u\n";
    t["closed_loop_isb.csv"] = "week,tow,system,isb_m,isb_std_m,clock_m\n";
    // Configuration getters (some combined with a network knob).
    t["network_clock_monitor.csv"] =
        "week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,"
        "abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated,"
        "nis,mode\n";
    t["network_anchor_guard.csv"] =
        "week,tow,sat,anchored,e_clk,sqrtS_clk,w_clk,e_phi,sqrtS_phi,"
        "w_phi,n_pb,decision,"
        "sigs,dclk,clk_sd,w1,dpb1,pb1_sd,w2,dpb2,pb2_sd\n";
    t["network_level_bound.csv"] = "week,tow,sat,p_level_m2,inv_info_m2,ratio,raised_m2,n_code_ep\n";
    t["network_level_innov.csv"] =
        "week,tow,sat,n_pairs,z_m,dclk_m,nu_m,r_m2,p_prior_m2,s0sq,nis\n";
    t["network_level_innov_win.csv"] =
        "week,tow_end,sat,k,nubar_m,nubar_a_m,nubar_b_m,var_nubar_m2,"
        "pbar_m2,T\n";
    t["network_phase_visible.csv"] =
        "week,tow,sat,sigs,n_pb,d_hat_m,sqrt_sp_m,e_hat_m,sqrt_su_m,nis\n";
    t["network_brdc_offset.csv"] =
        "week,tow,sat,iode,c_est_m,offset_m,c_wire_m,brdc_m,held,switched\n";
    t["network_phase_frame.csv"] = "week,tow,cons,voters,forced,step_m,g_m,event\n";
    t["network_arc_ledger.csv"] =
        std::string(kArcLedgerProvenance) +
        "week,tow,station,sat,signal,from,to,generation,gap_ep,reason,role,"
        "side,adm_state,adm_gap_ep\n";
    t["network_carry_vce.csv"] =
        "week,tow,hour,family,const,n_rows,n_eff,robust_floor_m2,mean_floor_m2,"
        "mean_floor_std,rate_m2ps,rate_std,cond,rate_frozen,cfg_floor_m2,"
        "robust_ratio,mean_ratio,floor_clamp,rate_clamp\n";
    t["network_carry_vce_apply.csv"] =
        "week,tow,family,const,n_eff,robust_floor_m2,hand_fold_var_m2,target,"
        "alpha2,sigma_hat_post,frozen,at_ceiling,at_unit,"
        "n_steps,n_frozen,n_slew_up,n_slew_down,n_ceiling_ep,n_unit_ep,alpha2_max,"
        "robust_var_r_m2,hand_fold_var_r_m2,rbar,n_eff_r\n";
    t["network_vcf.csv"] =
        "week,tow,class,sys,signal,kind,n,sum_wv2,sum_r,rho,f,g,ewma_f,ewma_g,"
        "identifiable,n_dw,n_unchk\n";
    t["network_station_factor.csv"] =
        "week,tow,station,factor,ewma,ratio,"
        "n_rows,n_trim,departed\n";
    t["network_level_authority.csv"] =
        "week,tow,kind,station,level_tecu,"
        "disp_tecu,n_sta,n_los,n_immature,a00_tecu\n";
    t["network_level_authority_los.csv"] =
        "week,tow,station,sat,ipp_lat_deg,ipp_lon_deg,mapping,"
        "el_deg,v_tecu,weight,n_cs\n";
    // The single-receiver chain datum.
    t["network_chain_edges.csv"] =
        "week,tow,system,family,band,comp,r1,edge_sta,edge_sat,child,"
        "parent,depth,target_m,target_cyc,prev_target_m,pinmove_m,known,"
        "legacy_cause,row,row_armed,certified,cert_source,flipped,"
        "returning,life_stamp\n";
    t["network_chain_frame.csv"] =
        "week,tow,family,arc_sta,arc_sat,float_cyc_1,float_cyc_2,q11,q12,"
        "q22,int_1,int_2,source,conflict,lattice_int\n";
    t["network_ring_fix.csv"] =
        "week,tow,station,sat,sig1,sig2,ring,candidate,state,refused,f1,f2,n_if,sigma_if,"
        "frac_if,wl,frac_wl,n1,n2,lat1,lat2,resid_if_m,streak,audit\n";
    t["network_rpb_states.csv"] =
        "week,tow,station,system,signal,value_m,sigma_m,pinned,floor_w,"
        "timelink_var_m2,parent_sat,depth,comp,event,shift_m\n";
    // The co-estimated spherical-harmonic ionosphere (second shell armed, model degree 2).
    t["network_vtec_sh.csv"] = "week,tow,layer,n,m,trig,value_tecu,std_tecu\n";
    t["network_vtec_sh_health.csv"] =
        "week,tow,a00_tecu,a00_std_tecu,rms_all_tecu,"
        "slant_resid_mean_m,slant_resid_rms_m,slant_resid_max_m,"
        "n_los,n_los_gt3sigma,n_stations,n_stations_gt3sigma,a00_in_band"
        ",rms_deg0_tecu,rms_deg1_tecu,rms_deg2_tecu"
        ",kaula_ratio_deg0,kaula_ratio_deg1,kaula_ratio_deg2"
        ",a00_l2_tecu,rms2_deg0_tecu,rms2_deg1_tecu\n";
    return t;
}

/// The two files openAll opens whatever the arming.
const std::set<std::string>& unconditionalFiles() {
    static const std::set<std::string> s = {"network_trace.csv", "network_solution.csv"};
    return s;
}

/// The files every fully armed configuration opens: the two unconditional ones, the files of
/// the network knobs, and the closed-loop files (with a second constellation active).
std::set<std::string> commonArmedFiles() {
    std::set<std::string> s = unconditionalFiles();
    for (const char* f : {
             "network_ionosphere.csv", "network_if_slip.csv", "network_raim.csv",
             "network_raim_attrib.csv", "network_anchor_info.csv", "network_dd_fractional.csv",
             "network_producer_ar.csv", "network_ar_lattice.csv", "network_ar_lattice_events.csv",
             "network_fixed_cycles.csv", "network_fixed_cycle_shift.csv",
             "network_fixed_cycle_events.csv", "network_slip_raw.csv", "network_epoch_status.csv",
             "network_sh_spectrum.csv", "network_sh_spectrum_mode.csv", "network_pf_keyed.csv",
             "network_pf_rows.csv", "network_pb_hold_clone.csv", "network_station_block.csv",
             "network_amb_state.csv", "network_amb_if.csv", "network_phase_colour.csv",
             "network_phase_anchor.csv", "network_sd_ewl.csv", "network_sd_ewl_ar.csv",
             "network_service_health.csv", "network_prior_residual.csv",
             "network_block_baarda.csv", "network_congruence.csv", "network_mdb.csv",
             "network_datum_graph.csv", "network_estimability.csv", "network_events.csv",
             "network_zwd.csv", "network_tropo_gradient.csv", "network_sat_bias_states.csv",
             "network_rx_states.csv", "network_pb_prehold.csv", "network_windup_arcs.csv",
             "network_station_residuals.csv", "network_raim_redundancy.csv",
             "closed_loop_trace.csv", "closed_loop_sat_trace.csv", "closed_loop_ar_fixes.csv",
             "closed_loop_ar_epoch.csv", "closed_loop_isb.csv"})
        s.insert(f);
    return s;
}

/// The files only the configuration getters of the reference posture case open.
const std::vector<std::string>& referencePostureExtraFiles() {
    static const std::vector<std::string> v = {
        "network_clock_monitor.csv", "network_anchor_guard.csv", "network_level_bound.csv",
        "network_level_innov.csv", "network_level_innov_win.csv", "network_phase_visible.csv",
        "network_brdc_offset.csv", "network_phase_frame.csv", "network_arc_ledger.csv",
        "network_carry_vce.csv", "network_carry_vce_apply.csv", "network_vcf.csv",
        "network_station_factor.csv", "network_level_authority.csv",
        "network_level_authority_los.csv"};
    return v;
}

const std::vector<std::string>& chainExtraFiles() {
    static const std::vector<std::string> v = {"network_chain_edges.csv", "network_chain_frame.csv",
                                               "network_ring_fix.csv", "network_rpb_states.csv"};
    return v;
}

const std::vector<std::string>& shExtraFiles() {
    static const std::vector<std::string> v = {"network_vtec_sh.csv", "network_vtec_sh_health.csv"};
    return v;
}

// ---------------------------------------------------------------------------------------------
// Arming and observation helpers.
// ---------------------------------------------------------------------------------------------

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("run_traces_open_set_test", sink);
}

/// Every network-knob gate of openAll switched on, with every header modifier on as well.
void armEveryTraceGate(NetworkConfig& nc) {
    nc.trace_ionosphere = true;
    nc.trace_consumer_sat = true;
    nc.closed_loop.ar_observe = true;
    nc.closed_loop.trace_ar_fixes = true;
    nc.trace_slip_events = true;
    nc.trace_anchor_info = true;
    nc.trace_dd_fractional = true;
    NetworkArConfig& ar = nc.ambiguity_resolution;
    ar.enable = true;
    ar.trace = true;
    ar.decorrelated_partial = true;
    ar.suspect_epochs = 3;
    ar.cycle_basis = true;
    ar.trace_lattice = true;
    ar.fixed_cycle_trace = true;
    ar.fixed_cycle_fold = FixedCycleFold::Observe;
    ar.fixed_cycle_source = FixedCycleSource::Lattice;
    nc.trace_slip_raw = true;
    nc.adm_trace = true;
    nc.adm_screen = true;
    nc.prior_residual_census = true;
    nc.prior_residual_global_sigma0 = true;
    nc.trace_epoch_status = true;
    nc.trace_sh_spectrum = true;
    nc.trace_sh_spectrum_stride = 60;
    nc.trace_postfit_keyed = true;
    nc.trace_postfit_rows = true;
    nc.trace_events = true;
    nc.emit_pb_hold = true;
    nc.emit_pb_hold_smoothed = true;
    nc.trace_station_block = true;
    nc.trace_amb_state = true;
    nc.trace_amb_if = true;
    nc.phase_colour_state = true;
    nc.trace_phase_colour = true;
    nc.trace_phase_anchor = true;
    nc.sd_ewl.trace = true;
    nc.sd_ewl.fix_enable = true;
    nc.health_interval_s = 300.0;
    nc.block_baarda = true;
    nc.congruence_screen = true;
    nc.mdb = true;
    nc.datum_graph_census = true;
    nc.trace_estimability = true;
    nc.trace_zwd = true;
    nc.estimate_tropo_gradients = true;
    nc.trace_sat_bias_states = true;
    nc.trace_rx_states = true;
    nc.trace_chain_datum = true;
    nc.ring_fix.enable = true;
    nc.trace_pb_prehold = true;
    nc.trace_rpb_states = true;
    nc.trace_windup_arcs = true;
    nc.trace_station_residuals = true;
    nc.trace_raim_redundancy = true;
}

/// Write @p j to the test's scratch folder and load it; a refusal by exception reads as false.
bool loadInto(const nlohmann::json& j, const std::string& tag, GnssConfig& cfg) {
    const fs::path p = gnss_test::tempDir() / ("open_set_cfg_" + tag + ".json");
    {
        std::ofstream out(p);
        out << j.dump(2);
    }
    LogManager log;
    bool ok = false;
    try {
        ok = cfg.loadFromJson(p, log);
    } catch (const std::exception&) {
        ok = false;
    }
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The sim reference posture of the float vehicle with the phase-bias hold (the frozen lineage
/// golden_sim_rf0hv18 since 2026-11-19; the integer-fixing reference runs the hold off and the chain
/// on), parsed (comments allowed, as the loader allows them).
nlohmann::json simReference() {
    const fs::path src =
        fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_sim_rf0hv18.json";
    std::ifstream in(src);
    return nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
}

/// The minimal two-station GPS + Galileo network with @p extra spliced into its network block.
nlohmann::json minimalNetwork(const std::string& extra) {
    return nlohmann::json::parse(TestSupport::networkConfig(extra));
}

/// Open every stream @p cfg / @p nc / @p shc arm into @p dir, close and destroy the owner (so
/// every stream is flushed), and return the names of the regular files left in @p dir.
std::set<std::string> openAndList(const fs::path& dir, const GnssConfig& cfg,
                                  const NetworkConfig& nc, const GlobalVtecShConfig& shc,
                                  const SphericalHarmonicIonosphere* sh_model, bool cl_trace_armed) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    {
        NetworkRunTraces traces(dir);
        traces.openAll(cfg, nc, shc, sh_model, cl_trace_armed, nullLogger());
        traces.closeAll();
    }
    std::set<std::string> names;
    if (!fs::exists(dir)) return names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        EXPECT_TRUE(entry.is_regular_file()) << "unexpected non-file entry " << entry.path();
        names.insert(entry.path().filename().string());
    }
    return names;
}

/// The whole content of @p path, with line ends normalized to "\n".
std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

/// Readable difference of two file sets for a failure message.
std::string setDifference(const std::set<std::string>& expected, const std::set<std::string>& got) {
    std::ostringstream os;
    for (const auto& f : expected)
        if (!got.count(f)) os << "\n  missing: " << f;
    for (const auto& f : got)
        if (!expected.count(f)) os << "\n  extra:   " << f;
    return os.str();
}

/// Every file of @p names holds exactly the content @p table records for it.
void expectRecordedContent(const fs::path& dir, const std::set<std::string>& names,
                           const std::map<std::string, std::string>& table) {
    for (const auto& name : names) {
        const auto it = table.find(name);
        if (it == table.end()) {
            ADD_FAILURE() << name << " has no recorded content";
            continue;
        }
        EXPECT_EQ(readAll(dir / name), it->second) << "content of " << name;
    }
}

// ---------------------------------------------------------------------------------------------
// (1) Every gate armed: the exact set and the exact content of each file.
// ---------------------------------------------------------------------------------------------

// The reference posture with the configuration-gated instruments armed reaches every stream
// except the chain and SH files.  The getters are asserted first so that a load that quietly
// disarmed one of them fails here, not as a missing file below.
TEST(NetworkRunTracesOpenSet, EveryGateOnTheReferencePostureOpensItsSetWithTheRecordedHeaders) {
    nlohmann::json j = simReference();
    nlohmann::json& n = j["network"];
    // The smoothed phase-bias hold, which the broadcast clock a priori requires.
    n["emit_pb_hold_phase_clock"] = false;
    n["emit_pb_hold_smoothed"] = true;
    n["apriori_source"] = "sp3_brdc_clock";
    n["brdc_select"] = "ttom";
    n["emit_phase_frame_hold"] = true;
    n["clock_monitor"] = true;
    n["clock_level_monitor"] = true;
    n["phase_visible_monitor"] = true;
    n["signal_roster"] = true;
    n["carry_vce_apply"] = true;
    n["raim_station_factor"] = true;
    n["level_authority_csgf"] = true;
    n["level_authority_los_dump"] = true;
    GnssConfig cfg;
    ASSERT_TRUE(loadInto(j, "reference", cfg));
    ASSERT_TRUE(cfg.networkAprioriBrdcClock());
    ASSERT_TRUE(cfg.networkEmitPhaseFrameHold());
    ASSERT_TRUE(cfg.networkClockMonitor());
    ASSERT_TRUE(cfg.networkClockLevelMonitor());
    ASSERT_TRUE(cfg.networkPhaseVisibleMonitor());
    ASSERT_TRUE(cfg.networkSignalRoster());
    ASSERT_TRUE(cfg.networkCarryVce());
    ASSERT_TRUE(cfg.networkCarryVceApply());
    ASSERT_TRUE(cfg.networkVcf());
    ASSERT_TRUE(cfg.networkRaimStationFactor());
    ASSERT_TRUE(cfg.networkLevelAuthorityCsgf());
    ASSERT_TRUE(cfg.networkLevelAuthorityLosDump());
    ASSERT_NE(cfg.networkClockAnchorGuardSpace(), 0);
    ASSERT_TRUE(cfg.networkRaimWithinEpoch());   // the epoch-status within-epoch columns
    ASSERT_FALSE(cfg.networkPhaseDatumEq34());
    ASSERT_FALSE(cfg.estimateNetworkIonoSh());

    NetworkConfig nc = cfg.getNetworkConfig();
    armEveryTraceGate(nc);
    GlobalVtecShConfig shc = cfg.getGlobalVtecShConfig();
    shc.trace = true;
    shc.layer2_max_degree = kShLayer2Degree;
    const SphericalHarmonicIonosphere sh(kShDegree);

    const fs::path dir = gnss_test::tempDir() / "reference_traces";
    const std::set<std::string> got = openAndList(dir, cfg, nc, shc, &sh, /*cl_trace_armed=*/true);

    std::set<std::string> expected = commonArmedFiles();
    for (const auto& f : referencePostureExtraFiles()) expected.insert(f);
    EXPECT_EQ(expected.size(), 64u);
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    expectRecordedContent(dir, got, recordedContent(/*we=*/true));
}

// The chain datum is the only arming of the four chain files; on the minimal network nothing
// configuration-gated is on, so the set is the common one plus exactly those four, and the
// epoch-status header has no within-epoch columns.
TEST(NetworkRunTracesOpenSet, EveryGateOnTheChainDatumAddsExactlyTheFourChainFiles) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto(minimalNetwork("    \"phase_leveling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"ambiguity_vehicle\": \"float_kf\",\n"
                                        "    \"phase_datum_eq34\": true,\n"),
                         "chain", cfg));
    ASSERT_TRUE(cfg.networkPhaseDatumEq34());
    ASSERT_FALSE(cfg.estimateNetworkIonoSh());
    ASSERT_FALSE(cfg.networkRaimWithinEpoch());

    NetworkConfig nc = cfg.getNetworkConfig();
    armEveryTraceGate(nc);
    GlobalVtecShConfig shc = cfg.getGlobalVtecShConfig();
    shc.trace = true;
    shc.layer2_max_degree = kShLayer2Degree;
    const SphericalHarmonicIonosphere sh(kShDegree);

    const fs::path dir = gnss_test::tempDir() / "chain_traces";
    const std::set<std::string> got = openAndList(dir, cfg, nc, shc, &sh, /*cl_trace_armed=*/true);

    std::set<std::string> expected = commonArmedFiles();
    for (const auto& f : chainExtraFiles()) expected.insert(f);
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    expectRecordedContent(dir, got, recordedContent(/*we=*/false));
}

// The co-estimated SH ionosphere is the only arming of the two SH files; with the second
// shell and a degree-2 model, both headers carry their layer and per-degree columns.
TEST(NetworkRunTracesOpenSet, EveryGateWithTheShIonosphereAddsExactlyTheTwoShFiles) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto(minimalNetwork("    \"apriori_source\": \"brdc_rts\",\n"
                                        "    \"estimate_iono_sh\": true,\n"),
                         "sh", cfg));
    ASSERT_TRUE(cfg.estimateNetworkIonoSh());
    ASSERT_FALSE(cfg.networkPhaseDatumEq34());
    ASSERT_FALSE(cfg.networkRaimWithinEpoch());

    NetworkConfig nc = cfg.getNetworkConfig();
    armEveryTraceGate(nc);
    GlobalVtecShConfig shc = cfg.getGlobalVtecShConfig();
    shc.trace = true;
    shc.layer2_max_degree = kShLayer2Degree;
    const SphericalHarmonicIonosphere sh(kShDegree);

    const fs::path dir = gnss_test::tempDir() / "sh_traces";
    const std::set<std::string> got = openAndList(dir, cfg, nc, shc, &sh, /*cl_trace_armed=*/true);

    std::set<std::string> expected = commonArmedFiles();
    for (const auto& f : shExtraFiles()) expected.insert(f);
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    expectRecordedContent(dir, got, recordedContent(/*we=*/false));
}

// The three armed cases together reach every file of the record: no stream of the record is
// unreachable, and the record names nothing the cases do not produce.
TEST(NetworkRunTracesOpenSet, TheThreeArmedSetsCoverTheWholeRecord) {
    std::set<std::string> all = commonArmedFiles();
    for (const auto* v : {&referencePostureExtraFiles(), &chainExtraFiles(), &shExtraFiles()})
        for (const auto& f : *v) all.insert(f);
    std::set<std::string> recorded;
    for (const auto& kv : recordedContent(true)) recorded.insert(kv.first);
    EXPECT_EQ(all, recorded) << setDifference(recorded, all);
    EXPECT_EQ(all.size(), 70u);   // 65 network files and 5 closed-loop files
}

// ---------------------------------------------------------------------------------------------
// (2) Every gate off.
// ---------------------------------------------------------------------------------------------

// Default configuration and knobs, no closed loop: only the two unconditional files, each
// with its header.
TEST(NetworkRunTracesOpenSet, EveryGateOffOpensOnlyTheTwoUnconditionalFiles) {
    const GnssConfig cfg;
    const NetworkConfig nc;
    const GlobalVtecShConfig shc;
    const fs::path dir = gnss_test::tempDir() / "off_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    EXPECT_EQ(got, unconditionalFiles()) << setDifference(unconditionalFiles(), got);
    expectRecordedContent(dir, got, recordedContent(false));
}

// Knobs that only refine a parent stream open nothing while the parent is off: the consumer
// knobs without the closed loop, the search traces without the search, the fold trace with
// the fold off, the phase-colour trace without the state, the SD-EWL integer path without its
// trace, the smoothed hold without the hold, the chain traces off the chain datum, the SH
// trace without the SH field, and the column-only knobs (admission, global sigma0).
TEST(NetworkRunTracesOpenSet, RefiningKnobsWithoutTheirParentOpenNothing) {
    const GnssConfig cfg;
    NetworkConfig nc;
    nc.trace_consumer_sat = true;
    nc.closed_loop.ar_observe = true;
    nc.closed_loop.trace_ar_fixes = true;
    nc.ambiguity_resolution.trace = true;
    nc.ambiguity_resolution.trace_lattice = true;
    nc.ambiguity_resolution.fixed_cycle_trace = true;
    nc.ambiguity_resolution.fixed_cycle_source = FixedCycleSource::Lattice;
    nc.trace_phase_colour = true;
    nc.sd_ewl.fix_enable = true;
    nc.emit_pb_hold_smoothed = true;
    nc.trace_chain_datum = true;
    nc.ring_fix.enable = true;
    nc.trace_rpb_states = true;
    nc.adm_trace = true;
    nc.adm_screen = true;
    nc.prior_residual_global_sigma0 = true;
    GlobalVtecShConfig shc;
    shc.trace = true;
    const SphericalHarmonicIonosphere sh(kShDegree);
    const fs::path dir = gnss_test::tempDir() / "refining_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, &sh, /*cl_trace_armed=*/false);
    EXPECT_EQ(got, unconditionalFiles()) << setDifference(unconditionalFiles(), got);
}

// ---------------------------------------------------------------------------------------------
// (3) One stream armed alone, from three parts of the function.
// ---------------------------------------------------------------------------------------------

// From the top of the function: the ionosphere trace.
TEST(NetworkRunTracesOpenSet, IonosphereAloneOpensOnlyItsFile) {
    const GnssConfig cfg;
    NetworkConfig nc;
    nc.trace_ionosphere = true;
    const GlobalVtecShConfig shc;
    const fs::path dir = gnss_test::tempDir() / "iono_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    std::set<std::string> expected = unconditionalFiles();
    expected.insert("network_ionosphere.csv");
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    expectRecordedContent(dir, got, recordedContent(false));
}

// From the middle: the epoch-status trace, whose header is the base variant when none of its
// column groups is armed.
TEST(NetworkRunTracesOpenSet, EpochStatusAloneOpensOnlyItsFileWithTheBaseHeader) {
    const GnssConfig cfg;
    NetworkConfig nc;
    nc.trace_epoch_status = true;
    const GlobalVtecShConfig shc;
    const fs::path dir = gnss_test::tempDir() / "epoch_status_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    std::set<std::string> expected = unconditionalFiles();
    expected.insert("network_epoch_status.csv");
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    EXPECT_EQ(readAll(dir / "network_epoch_status.csv"),
              epochStatusHeader(false, false, false, false));
}

// From the end: the RAIM redundancy census.
TEST(NetworkRunTracesOpenSet, RaimRedundancyAloneOpensOnlyItsFile) {
    const GnssConfig cfg;
    NetworkConfig nc;
    nc.trace_raim_redundancy = true;
    const GlobalVtecShConfig shc;
    const fs::path dir = gnss_test::tempDir() / "raim_redundancy_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    std::set<std::string> expected = unconditionalFiles();
    expected.insert("network_raim_redundancy.csv");
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    expectRecordedContent(dir, got, recordedContent(false));
}

// ---------------------------------------------------------------------------------------------
// Header variants and an edge of today's behaviour.
// ---------------------------------------------------------------------------------------------

// With their modifiers off, the producer-AR and SD-EWL headers are the plain variants, the
// fold on the epoch source opens no events file, and a zero spectrum stride opens no mode
// file.
TEST(NetworkRunTracesOpenSet, HeaderVariantsFollowTheirModifiers) {
    const GnssConfig cfg;
    NetworkConfig nc;
    nc.ambiguity_resolution.enable = true;
    nc.ambiguity_resolution.trace = true;
    nc.ambiguity_resolution.fixed_cycle_trace = true;
    nc.ambiguity_resolution.fixed_cycle_fold = FixedCycleFold::Observe;
    nc.ambiguity_resolution.fixed_cycle_source = FixedCycleSource::Epoch;
    nc.sd_ewl.trace = true;
    nc.trace_sh_spectrum = true;
    nc.trace_sh_spectrum_stride = 0;
    const GlobalVtecShConfig shc;
    const fs::path dir = gnss_test::tempDir() / "variant_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    std::set<std::string> expected = unconditionalFiles();
    for (const char* f : {"network_producer_ar.csv", "network_sd_ewl.csv",
                          "network_fixed_cycles.csv", "network_fixed_cycle_shift.csv",
                          "network_sh_spectrum.csv"})
        expected.insert(f);
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    EXPECT_EQ(readAll(dir / "network_producer_ar.csv"),
              "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
              "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n");
    EXPECT_EQ(readAll(dir / "network_sd_ewl.csv"),
              "week,tow,station,cons,sat,pivot_sat,status,"
              "sig1,sig2,code1,code2,n_cand_pairs,arc_count,bia,"
              "lambda_ewl_m,mean_m,pred_m,resid_m,sigma_m\n");
    const auto table = recordedContent(false);
    for (const char* f : {"network_fixed_cycles.csv", "network_fixed_cycle_shift.csv",
                          "network_sh_spectrum.csv"})
        EXPECT_EQ(readAll(dir / f), table.at(f)) << "content of " << f;
}

// Today the SH health file is opened even when no SH model is handed over, and it is left
// empty (its header is sized from the model); the coefficient trace without the second shell
// has the single-layer header.
TEST(NetworkRunTracesOpenSet, ShTracesWithoutAModelLeaveTheHealthFileEmpty) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto(minimalNetwork("    \"apriori_source\": \"brdc_rts\",\n"
                                        "    \"estimate_iono_sh\": true,\n"),
                         "sh_no_model", cfg));
    ASSERT_TRUE(cfg.estimateNetworkIonoSh());
    const NetworkConfig nc;
    GlobalVtecShConfig shc = cfg.getGlobalVtecShConfig();
    shc.trace = true;
    shc.layer2_max_degree = 0;
    const fs::path dir = gnss_test::tempDir() / "sh_no_model_traces";
    const std::set<std::string> got =
        openAndList(dir, cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false);
    std::set<std::string> expected = unconditionalFiles();
    expected.insert("network_vtec_sh.csv");
    expected.insert("network_vtec_sh_health.csv");
    EXPECT_EQ(got, expected) << setDifference(expected, got);
    EXPECT_EQ(readAll(dir / "network_vtec_sh.csv"), "week,tow,n,m,trig,value_tecu,std_tecu\n");
    EXPECT_EQ(readAll(dir / "network_vtec_sh_health.csv"), "");
}

}  // namespace
}  // namespace GnssProcessor
