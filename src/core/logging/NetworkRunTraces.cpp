/**
 * @file NetworkRunTraces.cpp
 * @brief Trace-stream ownership + startup open logic for the network run loop
 *        (decomposition INC-3 — verbatim relocation from ApplicationNetwork.cpp;
 *        see docs/network_decomposition_plan.md).
 */

#include "logging/NetworkRunTraces.hpp"

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkRunTypes.hpp"   // NetStation (roster writer), EpochStatusRow
#include "app/NetworkRunContext.hpp"    // NetworkKnobs / NetworkServiceCounters (INC-4)
#include "logging/CsvRotator.hpp"       // maybeRotateCsv (P1 rotation, INC-4)
#include "filter/NetworkPivotHandover.hpp" // formatPivotMap (health heartbeat, INC-4)
#include "filter/NetworkAmbStateTrace.hpp"  // kAmbStateHeader (RF-1 §7.4 carry-state trace)
#include "realtime/ConsumerSatTrace.hpp"    // card CST: ConsumerSatRow + kConsumerSatTraceHeader
#include "math/TimeUtils.hpp"           // continuousGpsSeconds (heartbeat cadence, INC-4)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>
#include <system_error>

namespace GnssProcessor {

NetworkRunTraces::NetworkRunTraces(std::filesystem::path log_dir)
    : log_dir_(std::move(log_dir)),
      trace_path(log_dir_ / "network_trace.csv"),
      soln_path(log_dir_ / "network_solution.csv"),
      iono_path(log_dir_ / "network_ionosphere.csv"),
      vtec_sh_path(log_dir_ / "network_vtec_sh.csv"),
      vtec_sh_health_path(log_dir_ / "network_vtec_sh_health.csv"),
      if_slip_trace_path(log_dir_ / "network_if_slip.csv"),
      raim_trace_path(log_dir_ / "network_raim.csv"),
      raim_attrib_trace_path(log_dir_ / "network_raim_attrib.csv"),
      clk_mon_trace_path(log_dir_ / "network_clock_monitor.csv"),
      anchor_info_trace_path(log_dir_ / "network_anchor_info.csv"),
      dd_frac_trace_path(log_dir_ / "network_dd_fractional.csv"),
      producer_ar_trace_path(log_dir_ / "network_producer_ar.csv"),
      slip_raw_trace_path(log_dir_ / "network_slip_raw.csv"),
      epoch_status_trace_path(log_dir_ / "network_epoch_status.csv"),
      health_trace_path(log_dir_ / "network_service_health.csv"),
      fast_clock_trace_path(log_dir_ / "network_fast_clock.csv"),
      events_trace_path(log_dir_ / "network_events.csv"),
      zwd_trace_path(log_dir_ / "network_zwd.csv"),
      sat_bias_states_trace_path(log_dir_ / "network_sat_bias_states.csv"),
      rx_states_trace_path(log_dir_ / "network_rx_states.csv"),
      station_res_trace_path(log_dir_ / "network_station_residuals.csv"),
      raim_redun_trace_path(log_dir_ / "network_raim_redundancy.csv"),
      station_factor_trace_path(log_dir_ / "network_station_factor.csv"),
      level_authority_trace_path(log_dir_ / "network_level_authority.csv"),
      level_authority_los_trace_path(log_dir_ / "network_level_authority_los.csv"),
      carry_vce_trace_path(log_dir_ / "network_carry_vce.csv"),
      carry_vce_apply_trace_path(log_dir_ / "network_carry_vce_apply.csv"),
      prior_residual_trace_path(log_dir_ / "network_prior_residual.csv"),
      block_baarda_trace_path(log_dir_ / "network_block_baarda.csv"),
      congruence_trace_path(log_dir_ / "network_congruence.csv"),
      mdb_trace_path(log_dir_ / "network_mdb.csv"),
      datum_graph_trace_path(log_dir_ / "network_datum_graph.csv"),
      estimability_trace_path(log_dir_ / "network_estimability.csv"),
      sh_spectrum_trace_path(log_dir_ / "network_sh_spectrum.csv"),
      sh_spectrum_mode_trace_path(log_dir_ / "network_sh_spectrum_mode.csv"),
      pf_keyed_trace_path(log_dir_ / "network_pf_keyed.csv"),
      pf_rows_trace_path(log_dir_ / "network_pf_rows.csv"),
      station_block_trace_path(log_dir_ / "network_station_block.csv"),
      amb_state_trace_path(log_dir_ / "network_amb_state.csv"),
      phase_anchor_trace_path(log_dir_ / "network_phase_anchor.csv"),
      sd_ewl_trace_path(log_dir_ / "network_sd_ewl.csv"),
      sd_ewl_ar_trace_path(log_dir_ / "network_sd_ewl_ar.csv"),
      d5_flags_trace_path(log_dir_ / "network_d5_flags.csv"),
      wl_lane_trace_path(log_dir_ / "network_wl_lane_trace.csv"),
      manifest_path(log_dir_ / "network_run_manifest.json") {}

void NetworkRunTraces::openAll(const GnssConfig& config_, const NetworkConfig& nc,
                               const GlobalVtecShConfig& shc,
                               const SphericalHarmonicIonosphere* sh_model,
                               bool cl_trace_armed,
                               const std::shared_ptr<spdlog::logger>& log) {
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);
    trace.open(trace_path);
    if (trace) {
        trace << kTraceHeader;
    } else {
        log->warn("Network: failed to open trace file {}", trace_path.string());
    }
    soln.open(soln_path);
    if (soln) soln << kSolnHeader;
    else log->warn("Network: failed to open solution trace {}", soln_path.string());

    if (nc.trace_ionosphere) {
        iono_trace.open(iono_path);
        if (iono_trace)
            // ⚠ `iono_m` is the reconstructed CORRECTION x_r only — the per-LOS iono
            // state is SEEDED at allocation, so x_r is NOT the absolute slant
            // ionosphere (CLAUDE.md's frame-broken note).  Kept unchanged so the pyviz
            // consumer is unaffected.  `x_lin_m` is that seed and `iono_total_m` =
            // x_lin_m + iono_m is the absolute slant ionosphere — the only column that
            // may be graded against a truth field (SPEC_t3_null_derivation §7.8).
            iono_trace << "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg,"
                          "x_lin_m,iono_total_m\n";
        else
            log->warn("Network: failed to open ionosphere trace {}", iono_path.string());
    }

    if (config_.estimateNetworkIonoSh() && shc.trace) {
        vtec_sh_trace.open(vtec_sh_path);
        // Iono-2 dual-layer: the coefficient trace gains a `layer` column ONLY
        // when the second shell is armed — the single-layer header (and the
        // whole OFF artifact set) stays byte-identical.
        if (vtec_sh_trace)
            vtec_sh_trace << (shc.layer2_max_degree > 0
                                  ? "week,tow,layer,n,m,trig,value_tecu,std_tecu\n"
                                  : "week,tow,n,m,trig,value_tecu,std_tecu\n");
        else
            log->warn("Network: failed to open SH-VTEC trace {}", vtec_sh_path.string());
        // T1a health trace — one row per emitted epoch.  The per-degree columns are
        // sized from the truncation degree N (deterministic header); the second
        // shell appends its own degree columns (armed-only, same rule as above).
        vtec_sh_health_trace.open(vtec_sh_health_path);
        if (vtec_sh_health_trace && sh_model) {
            std::string hdr = "week,tow,a00_tecu,a00_std_tecu,rms_all_tecu,"
                              "slant_resid_mean_m,slant_resid_rms_m,slant_resid_max_m,"
                              "n_los,n_los_gt3sigma,n_stations,n_stations_gt3sigma,a00_in_band";
            for (int n = 0; n <= sh_model->maxDegree(); ++n)
                hdr += ",rms_deg" + std::to_string(n) + "_tecu";
            for (int n = 0; n <= sh_model->maxDegree(); ++n)
                hdr += ",kaula_ratio_deg" + std::to_string(n);
            if (shc.layer2_max_degree > 0) {
                hdr += ",a00_l2_tecu";
                for (int n = 0; n <= shc.layer2_max_degree; ++n)
                    hdr += ",rms2_deg" + std::to_string(n) + "_tecu";
            }
            hdr += "\n";
            vtec_sh_health_trace << hdr;
        } else if (!vtec_sh_health_trace) {
            log->warn("Network: failed to open SH-VTEC health trace {}",
                      vtec_sh_health_path.string());
        }
    }

    if (cl_trace_armed) {
        const std::filesystem::path cl_path = log_dir_ / "closed_loop_trace.csv";
        cl_trace.open(cl_path);
        if (cl_trace)
            cl_trace << "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
                        "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ\n";  // T2c: +ar_gate; §8.11: +p_succ — the WL-only paths' ONLY confidence number (their `ratio` is the "+inf = no LAMBDA ran" sentinel, which reads as a perfect ratio and is how a 35 m wrong fix passed review)
        else
            log->warn("Network: failed to open closed-loop trace {}", cl_path.string());
        // Card CST (network.trace_consumer_sat): the per-SATELLITE twin of the
        // trace above — EV6 §9 item 1's owed instrument.  Same ownership split and
        // the same arming gate plus the knob, so it can only exist where there is
        // a consumer to read; unarmed, nothing is opened (byte-identical).
        if (nc.trace_consumer_sat) {
            const std::filesystem::path cls_path = log_dir_ / "closed_loop_sat_trace.csv";
            cl_sat_trace.open(cls_path);
            if (cl_sat_trace)
                cl_sat_trace << kConsumerSatTraceHeader;
            else
                log->warn("Network: failed to open closed-loop per-satellite trace {}",
                          cls_path.string());
        }
    }

    if (nc.trace_slip_events) {
        if_slip_trace.open(if_slip_trace_path);
        if (if_slip_trace)
            // dd = between-sat/between-epoch IF ΔΔφ (m); med = network pool median; s_pool
            // = elevation-normalized pooled scale; ewma_scale/count = this station-pair's
            // own EWMA scale + clean-sample count (decision-time, pre-update); w/s_eff =
            // shrinkage weight + effective scale; elev_map = 1/sin el; thr/floor = gate;
            // abs_dev = |dd−med|; flagged/carry_warm = decision + whether it refloated.
            // §22 is_ref = the detector's own IfJumpInfo::is_reference: this sample is
            // the CHAIN'S REFERENCE satellite, whose dd is identically 0 by
            // construction, so `flagged` is structurally false however large the slip.
            // Carried so a reference arc is read off the detector's flag instead of
            // being inferred from abs_dev_m == 0.
            if_slip_trace << "week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,"
                             "s_pool_m,ewma_scale_m,count,w_shrink,s_eff_m,elev_map,"
                             "thr_m,floor_m,abs_dev_m,flagged,carry_warm,is_ref\n";
        else
            log->warn("Network: failed to open IF-slip trace {}", if_slip_trace_path.string());
        raim_trace.open(raim_trace_path);
        if (raim_trace)
            // One row per (sat, station) whose normalized post-fit residual spiked, or any
            // station of a refloated sat: s_max_raw = v·√w (measurement-standardized), s_max_norm
            // = that ÷ the robust a-posteriori variance factor sigma0, vs raim_k; the per-sat
            // spiked-station count vs the min_sta consensus; and whether the sat was refloated.
            raim_trace << "week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,"
                          "n_sta_spiked,min_sta,refloated\n";
        else
            log->warn("Network: failed to open RAIM trace {}", raim_trace_path.string());
        raim_attrib_trace.open(raim_attrib_trace_path);
        if (raim_attrib_trace)
            // Prominence attribution (LOG-ONLY, does not change the refloat action):
            // robust 2-way median polish of the (station × sat) post-fit residual table
            // each RAIM epoch → per-dimension effect + robust-z prominence.  dim ∈
            // {SAT, STA, SIG, ARC}; acted = whether today's per-sat consensus already
            // refloats that entity (SAT/ARC only).  Only entities above the report
            // prominence are emitted, so quiet epochs cost nothing.
            raim_attrib_trace << "week,tow,dim,entity,effect,prominence,n_cells,overall,"
                                 "sigma0,acted\n";
        else
            log->warn("Network: failed to open RAIM attribution trace {}",
                      raim_attrib_trace_path.string());
    }
    if (nc.trace_slip_events && (config_.networkClockMonitor() || config_.networkClockModel())) {
        clk_mon_trace.open(clk_mon_trace_path);
        if (clk_mon_trace)
            // clk = solved Δδtˢ (m); median = network median (gauge); dev = clk−median;
            // prev_clk/jump = previous adjacent-epoch value + step (blank if unavailable);
            // flag_abs/flag_jump = this monitor's own bounds; flag_ext = already flagged by
            // the within-epoch / post-fit RAIM; refloated = carried clock reinitialized.
            clk_mon_trace << "week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,"
                             "abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated,"
                             "nis,mode\n";   // L4: nis,mode appended (trailing-column format break)
        else
            log->warn("Network: failed to open clock-monitor trace {}",
                      clk_mon_trace_path.string());
    }
    if (nc.trace_anchor_info) {
        anchor_info_trace.open(anchor_info_trace_path);
        if (anchor_info_trace)
            // anchor_info = constant 1/initial_std.sat_clock² (m⁻²), the a-priori
            // CODE-product Tikhonov pull; total_info = reduced shared-normals diagonal
            // on this sat's clock column (data + carry prior + datum pin for the pivot);
            // carry_info = the carried clock-prior info folded this epoch (scalar D2 or
            // the two-state model); data_info = total − carry − anchor (pivot pin still
            // in it, hence is_pivot); anchor_frac_data = anchor/(anchor+data) — the
            // HONEST data-vs-anchor fraction, INDEPENDENT of carry stiffness (P1-4);
            // carry_frac = carry/total exposes a carry-dominated lock-in (high ⇒ the
            // clock is defined by its own past, not by data); frac = the legacy
            // anchor/total (kept for continuity; misleads for a locked-in clock).
            anchor_info_trace << "week,tow,sat,anchor_info,carry_info,data_info,total_info,"
                                 "anchor_frac_data,carry_frac,frac,is_pivot\n";
        else
            log->warn("Network: failed to open anchor-info trace {}",
                      anchor_info_trace_path.string());
    }
    if (nc.trace_dd_fractional) {
        dd_frac_trace.open(dd_frac_trace_path);
        if (dd_frac_trace) {
            // Between-station DD of undifferenced float ambiguities (m): sat_i minus the
            // per-(const,signal) reference sat, differenced across station pair (sta_a,
            // sta_b).  P1-2: lambda_m / dd_cyc / frac_cyc are now computed here (frac_cyc =
            // dd_cyc − round(dd_cyc)) so the CSV is directly a fractional-cycle histogram —
            // the go/no-go instrument for float-bias user AR.  GLONASS (FDMA) and the pivot
            // station (pinned ambiguities) are excluded from the pool below.
            // sigma_carry_aware APPENDED (readers read by name): 1 = dd_sigma_cyc came from
            // the carry-aware readback (docs/SPEC_carry_aware_covariance.md), 0 = the
            // carry-blind N_rr⁻¹ form every pre-card run emitted.  Without it a reader
            // cannot tell a 0.14-cyc σ that means something from a 3.9-cyc σ that means
            // "this covariance never saw the carry".
            dd_frac_trace << "week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,"
                             "dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc,"
                             "sigma_carry_aware\n";  // T2a: +dd_sigma_cyc
            // (B5 wavelength provider dd_ppp_utils is constructed by the CALLER,
            //  gated on this stream being open — it needs the LogManager.)
        } else {
            log->warn("Network: failed to open DD-fractional trace {}",
                      dd_frac_trace_path.string());
        }
    }
    // Producer-AR Phase-2 per-DD trace.  Gated on BOTH knobs on purpose: the rows are
    // a by-product of the search, so `trace` without `enable` could only ever produce a
    // header-only file that reads like "the search found nothing" (the loader warns).
    if (nc.ambiguity_resolution.trace && nc.ambiguity_resolution.enable) {
        producer_ar_trace.open(producer_ar_trace_path);
        if (producer_ar_trace) {
            producer_ar_trace << kProducerArHeader;
        } else {
            log->warn("Network: failed to open producer-AR trace {}",
                      producer_ar_trace_path.string());
        }
    }
    if (nc.trace_slip_raw) {
        slip_raw_trace.open(slip_raw_trace_path);
        if (slip_raw_trace) {
            // Per-(station,sat) RAW slip observables for the primary dual-freq pair, every
            // epoch, computed straight from raw_observed (phase in cycles, code in metres) —
            // NO dependence on any detector being armed.  mw_m/gf_m/l1_c1_m are absolute (the
            // WL slip step is λ_WL≈0.86 m/cyc in mw_m, an L1/L2 slip jumps gf_m); lli1/lli2
            // are the RINEX loss-of-lock flags for sig1/sig2 (bit 0 = slip).  Diff adjacent
            // epochs offline for the slip signature.  Pivot station included (diagnostic).
            slip_raw_trace << "week,tow,station,sat,sig1,sig2,elev_deg,"
                              "mw_m,gf_m,l1_c1_m,lli1,lli2\n";
            // (slip_raw_ppp_utils likewise constructed by the caller.)
        } else {
            log->warn("Network: failed to open raw-slip trace {}",
                      slip_raw_trace_path.string());
        }
    }
    // Measurement-admission census schema flag (docs/SPEC_measurement_admission.md
    // INC-3) — decided ONCE, here, so the header below and every row written by
    // emitEpochStatus agree by construction (the two writers are 300 lines apart).
    // Screening implies the columns, per spec §5.
    es_adm_columns_ = nc.adm_trace || nc.adm_screen;
    // Prior-residual census schema flag (docs/SPEC_prior_residual_census.md) —
    // same one-decision-point discipline, same STRUCTURAL byte-identity reason.
    es_prires_columns_ = nc.prior_residual_census;
    // Constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14) — its OWN
    // schema flag, so a census-only run keeps exactly the columns it had before
    // this rider existed and the census's §9.1 A/B stays a valid comparison.
    es_s0g_columns_ = nc.prior_residual_census && nc.prior_residual_global_sigma0;
    // R0 within-epoch tier observability (SPEC_pivot_visibility_gauge_defect
    // §5.07.5) — present iff the tier is armed (the adm/prires precedent).  NOTE
    // the loader force-enables the tier whenever raim_withhold_sigma0 > 0, so
    // this flag reflects the RESOLVED arming, not what the user typed.
    es_we_columns_ = config_.networkRaimWithinEpoch();
    if (nc.trace_epoch_status) {
        epoch_status_trace.open(epoch_status_trace_path);
        if (epoch_status_trace)
            // ONE row per PROCESSED epoch, written before each `continue` (skip) and at
            // the emit site.  disposition classifies WHY the epoch was skipped; sigma0 /
            // n_adapt / n_refloat_sats / n_retro_applied / datum_changed capture the QC
            // actions; clock_median_m/std_m/worst_sat/worst_dev_m are the clock-Δ spread
            // (median+std+worst-sat convention).  BLANK (not 0) fields = not-in-scope at
            // that disposition site.  sigma0_trigger/withhold_ceiling are config echoes so
            // viewers draw honest threshold lines.  Note: fast-forward (start_offset_hours)
            // and max-duration epochs are uncounted by design and write NO row.
            // P1-1/P1-2/P1-4 (SPEC_day129_frame_guards) append four carry-churn,
            // four iono-deweight and four post-fit-σ̂0 columns AFTER n_arc_refloat —
            // positions of every pre-existing column preserved (T2d precedent), so
            // an old reader keeps working and a new one gets the trigger channels.
            epoch_status_trace << "week,tow,disposition,sigma0,n_adapt,n_present,"
                                  "n_clock_sats,clock_median_m,clock_std_m,worst_sat,"
                                  "worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,"
                                  "sigma0_trigger,withhold_ceiling,n_arc_refloat,"   // T2d
                                  "n_d5_flags,n_d5_retaps,n_chord_births,n_slip_refloats,"
                                  "n_dw_engaged,n_dw_zero,dw_min_w,dw_mean_w,"
                                  "sigma0_pf,sigma0_pf_code,sigma0_pf_phase,n_pf_rows,"
                                  // docs/SPEC_redundancy_census.md INC-1/INC-2:
                                  // blank on epochs the census did not sweep.
                                  "n_redun_rows,n_uncheckable_rows,uncheckable_max_s,"
                                  "n_unobservable_local,"
                                  // docs/SPEC_prefit_census_observability.md INC-1:
                                  // per DATA class, the COMMON-MODE-REMOVED
                                  // standardized prefit deviation s.  Blank on
                                  // epochs the census did not walk.
                                  "pc_code_max_s,pc_code_p99_s,pc_code_n10,pc_code_n1k,"
                                  "pc_phase_max_s,pc_phase_p99_s,pc_phase_n10,pc_phase_n1k,"
                                  "pc_other_max_s,pc_other_p99_s,pc_other_n10,pc_other_n1k,"
                                  "pc_sh_max_s,pc_sh_p99_s,pc_sh_n10,pc_sh_n1k,"
                                  // docs/SPEC_pd_safe_dd_fold.md INC-0: the DD-prior
                                  // fold's PD screen.  UNCONDITIONAL (the redundancy /
                                  // prefit precedent, not the adm one) because INC-0 is
                                  // an always-on instrument, not an armed card: the
                                  // branch it observes — `M := 0` on a non-PD Pᶻ+B —
                                  // had no counter, no log and no event, and 78 % of
                                  // the carried state was measured diffusing behind it.
                                  // Blank on epochs the invariant carry did not fold on.
                                  "ddfold_degen_blocks,ddfold_degen_rows,"
                                  "ddfold_min_pivot_rel"
                               // docs/SPEC_measurement_admission.md INC-3: the
                               // ABSOLUTE product-referenced |z| census.  Appended
                               // ONLY when the card is armed — unlike the two
                               // censuses above, whose unconditional columns changed
                               // every unarmed run's artifact (the deweight card's
                               // gate-1 record had to say so: "29 -> 48 -> 49 columns
                               // over the session").  This card's OFF contract is
                               // STRUCTURAL byte-identity, and its own §7 gates are
                               // same-build A/B comparisons of this very file.
                               // adm_sta_worst_z = the MEDIAN over screened stations
                               // of each station's worst |z| (the healthy sea);
                               // adm_net_worst_z = the network MAXIMUM (the extreme,
                               // whose identity goes to the ADMISSION events).
                               << (es_adm_columns_
                                       ? ",adm_sta_worst_z,adm_net_worst_z" : "")
                               // docs/SPEC_prior_residual_census.md: the per-epoch
                               // face of the carry-prior census.  Appended ONLY
                               // when the card is armed (the adm precedent, not
                               // the redundancy/prefit one) because this card's
                               // OFF contract is STRUCTURAL byte-identity and its
                               // §9.1 gate compares this file across arms.
                               // prires_rows is the DENOMINATOR — a raw
                               // "12 rows over 4-sigma" is uninterpretable without
                               // it — and prires_rows_resp / _frame partition the
                               // populations the card refuses to pool.
                               << (es_prires_columns_
                                       ? ",prires_rows,prires_rows_resp,prires_rows_frame,"
                                         "prires_n_reported,prires_max_t"
                                       : "")
                               // docs/SPEC_prior_residual_census.md §14: the
                               // constraint-aware global variance component.
                               // sigma0_global is the LASER 5-74 scale (prior
                               // residuals in the numerator, constraints in the
                               // redundancy); prior_tension_ratio is the headline
                               // — how much of the epoch's weighted misfit lives
                               // in the CARRY, which the robust median sigma0 is
                               // structurally blind to.  s0g_flags is the in-band
                               // degradation bitmask: a degraded number SAYS SO
                               // in its own row, never silently.
                               << (es_s0g_columns_
                                       ? ",sigma0_global,prior_tension_ratio,s0g_flags"
                                       : "")
                               // R0 (SPEC_pivot_visibility_gauge_defect §5.07.5):
                               // the within-epoch tier's own observability, present
                               // iff the tier is armed.  sigma0_first is the
                               // pre-adaptation σ̂₀ — the null sample QUEUE item 4
                               // needs (previously it existed only inside a trip
                               // event's detail span); n_we_refloat_sats is the
                               // tier's OWN whole-sat refloat count (n_refloat_sats
                               // is the clock-COUPLE set, empty unless
                               // clock_monitor_couple_raim); we_clk_move_* is the
                               // §5.07.2 impulse-channel meter — how far the
                               // within-epoch re-solves moved a satellite clock off
                               // the PRIMARY solve.  Blank on epochs the tier did
                               // not assess.
                               << (es_we_columns_
                                       ? ",sigma0_first,n_we_refloat_sats,"
                                         "we_clk_move_max_m,we_clk_move_max_sat\n"
                                       : "\n");
        else
            log->warn("Network: failed to open epoch-status trace {}",
                      epoch_status_trace_path.string());
    }
    if (nc.trace_sh_spectrum) {
        // P1-3.  ev1..ev5 are the FIVE SMALLEST eigenvalues (ascending); the mass_*
        // columns partition the SMALLEST direction's |eigenvector| mass (Σ = 1) over
        // the shared column families, and span_* count how many columns of each
        // family carry >= 1% of that mass.  A joint clock↔iono slide shows up as
        // mass_clk and mass_sh both substantial in the SAME row.
        sh_spectrum_trace.open(sh_spectrum_trace_path);
        if (sh_spectrum_trace)
            sh_spectrum_trace << "week,tow,n_shared,n_clk,n_cb,n_pb,n_sh,"
                                 "ev_min,ev_max,cond,ev1,ev2,ev3,ev4,ev5,"
                                 "mass_clk,mass_cb,mass_pb,mass_sh,"
                                 "span_clk,span_cb,span_pb,span_sh,cutoff\n";
        else
            log->warn("Network: failed to open SH-spectrum trace {}",
                      sh_spectrum_trace_path.string());
        if (nc.trace_sh_spectrum_stride > 0) {
            sh_spectrum_mode_trace.open(sh_spectrum_mode_trace_path);
            if (sh_spectrum_mode_trace)
                sh_spectrum_mode_trace << "week,tow,mode_rank,ev,col_label,component\n";
            else
                log->warn("Network: failed to open SH-spectrum mode trace {}",
                          sh_spectrum_mode_trace_path.string());
        }
    }
    if (nc.trace_postfit_keyed) {
        // §4.3 RHS localization: one row per (kind, key) per emitted epoch from
        // the P1-4 residual pass.  kind ∈ {STA, SAT, SIG}; s0 = 1.4826·median of
        // the measurement-standardized |v|√w pool (the sigma0_pf convention);
        // phase pools are ≡0 under free ambiguities — their information is in
        // n_phase vs the code pools, exactly as P1-4 documents.
        pf_keyed_trace.open(pf_keyed_trace_path);
        if (pf_keyed_trace)
            pf_keyed_trace << "week,tow,kind,key,n_code,s0_code,rms_code,max_code,"
                              "n_phase,s0_phase,rms_phase,max_phase\n";
        else
            log->warn("Network: failed to open keyed post-fit trace {}",
                      pf_keyed_trace_path.string());
    }
    if (nc.trace_postfit_rows) {
        // SPEC_event_harness §24.6: the SIGNED per-row dump off the P1-4 pass.  One
        // line per attributed DATA row per emitted epoch; resid_m is v (metres, NOT
        // standardized), w the row weight, elev_deg empty when the station has no
        // modeled observation of that satellite this epoch.
        pf_rows_trace.open(pf_rows_trace_path);
        if (pf_rows_trace)
            pf_rows_trace << "week,tow,station,sat,signal,kind,resid_m,w,elev_deg\n";
        else
            log->warn("Network: failed to open per-row post-fit trace {}",
                      pf_rows_trace_path.string());
    }
    if (nc.trace_station_block) {
        // SPEC_event_harness §24.10 card ZP: the per-station LOCAL-block audit.  One
        // `#` provenance line naming the object and its conditioning, then the column
        // header (scripts/zp_station_block_read.py skips `#`).  The provenance line is
        // load-bearing: every number below describes N_rr with the SHARED states held
        // at their solved values, which is a different matrix from the station's
        // stand-alone normal equations and from the reduced shared system.
        station_block_trace.open(station_block_trace_path);
        if (station_block_trace)
            station_block_trace
                << "# network_station_block.csv - SPEC_event_harness 24.10 card ZP "
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
                   "count of DATA rows visited (coupling rows excluded).\n"
                << "week,tow,station,n_rows_code,n_rows_phase,n_local,rank,cond,"
                   "ev1_val,ev1_span,ev2_val,ev2_span,ev3_val,ev3_span,"
                   "zwd_info_eff,zwd_info_raw,zwd_info_ratio,x_r_norm,x_r_zwd,"
                   "zero_red_code,zero_red_phase,n_rows\n";
        else
            log->warn("Network: failed to open station-block audit trace {}",
                      station_block_trace_path.string());
    }
    if (nc.trace_amb_state) {
        // RF-1 §7.4: the per-arc carry-state trace.  One `#` provenance line naming the
        // object, the fold predicate and the event vocabulary, then the pre-registered
        // header (scripts/amb_state_read.py skips `#` and parses by column NAME).  The
        // provenance line is load-bearing twice over: `fold_var_m2` is the variance the
        // fold ACTUALLY used (not the hand model), and the four `drop:` causes on the
        // LEVEL vehicle are today's response paths recorded in the vocabulary RF-1's
        // bounded drop will use — which is what makes the RF-0 baseline and an RF-1 arm
        // comparable row for row.
        amb_state_trace.open(amb_state_trace_path);
        if (amb_state_trace)
            amb_state_trace
                << "# network_amb_state.csv - RF-1 7.4 (docs/refound/"
                   "RF-1_float_ambiguities_design.md, network.trace_amb_state). One row "
                   "per CARRIED (station, satellite, signal) per traced epoch, written "
                   "after that epoch's leveling harvest, plus one 'erase:' row per "
                   "entry deleted this epoch captured immediately BEFORE deletion. "
                   "value_abs_m / var_m2 are POST-harvest (this epoch's solve); "
                   "var_first_m2 is the arc's unfloored first-stash variance and "
                   "var_birth_m2 the same floored by network.amb_float_birth_var_m2 "
                   "(equal when the floor is 0). fold_var_m2 is the variance the PASS-B "
                   "inject ACTUALLY folded with (after q*gap, the F1 B_eff scale, the "
                   "var floor and the M2 VCE factor), 0 when folded=0; route is the "
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
                   "family folds.\n"
                << kAmbStateHeader;
        else
            log->warn("Network: failed to open ambiguity carry-state trace {}",
                      amb_state_trace_path.string());
    }
    if (nc.trace_phase_anchor) {
        // S1 anchor partition (docs/SPEC_lane_spectrometer.md §3.3).  One row per
        // gauge-PINNED ambiguity: `anchor_sta` is the station whose ambiguity pin
        // realizes the phase datum for that (constellation, signal, satellite)
        // freedom.  Under the legacy single-pivot datum every freedom is pinned at the
        // pivot, so the column is constant and the partition degenerates — which is
        // itself the honest answer (it exonerates the election mechanism by
        // construction, exactly as the SPEC says).  Under the prioritized / eq34
        // realizations the column varies and the same/cross-anchor split becomes a
        // real test.
        phase_anchor_trace.open(phase_anchor_trace_path);
        if (phase_anchor_trace)
            phase_anchor_trace << "week,tow,cons,signal,sat,anchor_sta\n";
        else
            log->warn("Network: failed to open phase-anchor trace {}",
                      phase_anchor_trace_path.string());
    }
    if (nc.sd_ewl.trace) {
        // SPEC_sd_ewl_prereduce §6: one row per (station, epoch, satellite) the SD
        // pre-reduce EWL appender considered.  `status` names the outcome in the
        // appender's own vocabulary (BUILT / PIVOT / NO_ARC / IMMATURE /
        // FRAME_RECON / NO_AMB_COL / NO_SHARED_COL / NO_CODE / BAD_LAMBDA / BIG_Z /
        // MIXED_FRAME / CAPPED), so the file is a direct per-satellite expansion of
        // the run-end skip tallies — the only way to see WHICH (band1,band2) signal
        // pair carries an arc rather than just how many did not.  sig1/sig2 are the
        // ELECTED phase pair (empty when none had both ambiguity columns);
        // code1/code2 are the code channels the arc's MW actually consumed.
        sd_ewl_trace.open(sd_ewl_trace_path);
        if (sd_ewl_trace)
            // mean_m = the arc's ZD MW mean (raw, hundreds of metres — it carries
            // λ_EWL·N_EWL); pred_m = this arc's half of H·x_lin; resid_m = the
            // CORRECTION-frame value actually folded, (mean_s−mean_p)−(pred_s−pred_p).
            // resid_m is the row's residual and the quantity to judge: it should sit at
            // the metre scale, and a per-satellite offset in it is a frame problem.
            // Increment 2b appends three columns ONLY when the integer path is armed,
            // so an increment-1 artifact set stays byte-identical (the same armed-only
            // header rule the dual-layer SH coefficient trace uses above).
            // n_fix_cyc = the folded SD integer (cycles of lambda_EWL, 0 for a float
            // row); fixed = whether THIS row folded the integer; grp_fixed = whether
            // the row's whitened group was the all-fixed group.
            sd_ewl_trace << "week,tow,station,cons,sat,pivot_sat,status,"
                            "sig1,sig2,code1,code2,n_cand_pairs,arc_count,bia,"
                            "lambda_ewl_m,mean_m,pred_m,resid_m,sigma_m"
                         << (nc.sd_ewl.fix_enable ? ",n_fix_cyc,fixed,grp_fixed\n"
                                                  : "\n");
        else
            log->warn("Network: failed to open SD-EWL trace {}",
                      sd_ewl_trace_path.string());
    }
    if (nc.sd_ewl.trace && nc.sd_ewl.fix_enable) {
        // Increment 2a's own decision trace.  Kept SEPARATE from network_sd_ewl.csv
        // because it is a different unit of observation: that file has one row per
        // ROW-BUILD attempt (station x satellite x epoch, in the parallel produce
        // phase), this one has one row per INTEGER DECISION (the serial pre-assembly
        // seam), and the two do not correspond one-to-one — a satellite can get an
        // integer and still fail to build a row, or vice versa.
        sd_ewl_ar_trace.open(sd_ewl_ar_trace_path);
        if (sd_ewl_ar_trace)
            // status ∈ {R0 (the component reference, integers ≡ 0 by definition),
            // FIXED, LINK (the per-station gauge integer — inherited, not measured),
            // REJ_FRAC, REJ_FFR, FLIP (disagreed with the previous epoch on a
            // continuous arc — counted and vetoed), NO_LINK, SKIP_CHANNEL (arc on a
            // different tracking pair than the network-common election), SKIP_FRAME,
            // NO_GRAPH}.  dd_cyc is the rounded DD's pre-rounding value in cycles.
            sd_ewl_ar_trace << "week,tow,cons,sat,station,status,sig1,sig2,r0,parent,"
                               "link_sat,arc_count,dd_cyc,frac_cyc,sigma_cyc,ffr,"
                               "n_cyc,n_prev_cyc\n";
        else
            log->warn("Network: failed to open SD-EWL AR trace {}",
                      sd_ewl_ar_trace_path.string());
    }
    if (nc.health_interval_s > 0.0) {
        // RTB-6: periodic operator health heartbeat.  Cumulative epoch tallies +
        // σ̂0 EWMA + windowed ms/epoch + last-emit age + stations/clock-sats +
        // realized pivots, appended every network.health_interval_s of content
        // time.  A service supervisor (or a human) polls this instead of the
        // exit-only summary.
        health_trace.open(health_trace_path);
        if (health_trace)
            health_trace << kHealthHeader;
        else
            log->warn("Network: failed to open service-health trace {}",
                      health_trace_path.string());
        // P4(a): the σ̂0 EWMA updates ONLY inside the warm post-fit RAIM block
        // (emit_ura && raim && phase_leveling); with any of them unarmed the
        // column would silently report its frozen 1.0 init.  It emits the −1
        // "unavailable" sentinel instead — warn once so the operator knows the
        // quality channel of this heartbeat is dark, not clean.
        if (!(config_.networkEmitUra() && config_.networkRaim()
              && config_.networkPhaseLeveling()))
            log->warn("Network (RTB-6): health heartbeat is armed but the σ̂0 EWMA's "
                      "feeding tiers are not (needs network.emit_ura + network.raim + "
                      "network.phase_leveling; ura={}, raim={}, phase_leveling={}) — "
                      "the sigma0_ewma column will report the -1 'unavailable' "
                      "sentinel for this run.",
                      config_.networkEmitUra(), config_.networkRaim(),
                      config_.networkPhaseLeveling());
    }
    if (nc.fast_clock.enable && nc.fast_clock.trace) {
        // FC-1: the per-tick fast-clock trace.  Gated on the tier master as well as its
        // own knob — the tick tier runs on epochs the full pipeline skips, so a trace
        // armed without the tier would produce an empty file that reads like a quiet run
        // rather than an unarmed one (standing invariant #4).
        fast_clock_trace.open(fast_clock_trace_path);
        if (fast_clock_trace)
            fast_clock_trace << kFastClockHeader;
        else
            log->warn("Network (FC-1): failed to open fast-clock trace {}",
                      fast_clock_trace_path.string());
    }
    if (config_.networkCarryVce()) {
        carry_vce_trace.open(carry_vce_trace_path);
        if (carry_vce_trace)
            carry_vce_trace << kCarryVceHeader;
        else
            log->warn("Network (M2/LS-VCE): failed to open carry-VCE trace {}",
                      carry_vce_trace_path.string());
    }
    if (config_.networkCarryVceApply()) {
        // M2 v2 — the variance-factor trajectory.  Gated on the apply knob itself
        // rather than on a trace_* knob: the file IS the mechanism's telemetry
        // (α², its clamps, the σ̂²→1 convergence readout), and an armed apply with
        // no trajectory is an ungradable run, not a quiet one.  The loader
        // guarantees carry_vce, so the monitor digest next to it is always open too.
        carry_vce_apply_trace.open(carry_vce_apply_trace_path);
        if (carry_vce_apply_trace)
            carry_vce_apply_trace << kCarryVceApplyHeader;
        else
            log->warn("Network (M2 v2): failed to open carry-VCE-apply trace {}",
                      carry_vce_apply_trace_path.string());
    }
    if (nc.prior_residual_census) {
        // docs/SPEC_prior_residual_census.md INC-1 — the exceedance ladder, the
        // card's headline product.  ⚠ Durability: only health_trace flushes
        // mid-run (see rotateServiceCsvs / writeHealthRow), so a long armed run
        // should also carry network.health_interval_s if the ladder has to survive
        // a kill.  The seam flushes this stream itself on the content-hour roll.
        prior_residual_trace.open(prior_residual_trace_path);
        if (prior_residual_trace)
            prior_residual_trace << kPriorResidualHeader;
        else
            log->warn("Network (prior-residual census): failed to open trace {}",
                      prior_residual_trace_path.string());
    }
    if (nc.block_baarda) {
        // docs/SPEC_redundancy_census.md BB-INC-1 — the per-(family x label) exceedance
        // ladder over t_B, the card's headline product.  Same durability caveat
        // as the prior-residual ladder: only health_trace flushes mid-run, so a
        // long armed run should also carry network.health_interval_s if these
        // rows have to survive a kill.
        block_baarda_trace.open(block_baarda_trace_path);
        if (block_baarda_trace)
            block_baarda_trace << kBlockBaardaHeader;
        else
            log->warn("Network (block-Baarda census): failed to open trace {}",
                      block_baarda_trace_path.string());
    }
    if (nc.congruence_screen) {
        // docs/SPEC_redundancy_census.md CG-INC-1 — one row per SWEPT STATION per
        // epoch.  This file IS the card's headline product: SPEC §7.2's perturbed
        // arm is graded on a per-station run-median of `d_e` and §7.3's MDB table
        // is built from `mdb_*`, neither of which survives aggregation into a
        // ladder.  Same durability caveat as the ladders above: only health_trace
        // flushes mid-run, so a long armed run should carry
        // network.health_interval_s if these rows have to survive a kill.
        congruence_trace.open(congruence_trace_path);
        if (congruence_trace)
            congruence_trace << kCongruenceHeader;
        else
            log->warn("Network (station congruence): failed to open trace {}",
                      congruence_trace_path.string());
    }
    if (nc.mdb) {
        // docs/SPEC_redundancy_census.md MDB-INC-1 — the per-(constellation x
        // class) MDB distribution, the card's headline product.  Same durability
        // caveat as the ladder above: only health_trace flushes mid-run, so a long
        // armed run should carry network.health_interval_s if these rows have to
        // survive a kill.
        mdb_trace.open(mdb_trace_path);
        if (mdb_trace)
            mdb_trace << kNetworkMdbHeader;
        else
            log->warn("Network (MDB): failed to open reliability trace {}",
                      mdb_trace_path.string());
    }
    if (nc.datum_graph_census) {
        // docs/SPEC_datum_graph_census.md INC-2 — the predicted-vs-observed
        // S-basis dimension contingency, the card's headline product.  Same
        // durability caveat as the files above: only health_trace flushes
        // mid-run, so a long armed run should carry network.health_interval_s if
        // these rows have to survive a kill.
        datum_graph_trace.open(datum_graph_trace_path);
        if (datum_graph_trace)
            datum_graph_trace << kNetworkDatumGraphHeader;
        else
            log->warn("Network (datum graph): failed to open census trace {}",
                      datum_graph_trace_path.string());
    }
    if (nc.trace_estimability) {
        // Card DG5: the machine-readable twin of the Network[estimability] WARN
        // dump.  ONE long-format file, three record kinds (EPOCH / DIR / RUN) —
        // see the member Doxygen.  The col1..col5 fields are "label=weight"
        // strings in the log line's order; a trailing empty field just means the
        // direction had fewer than 5 shared columns.
        estimability_trace.open(estimability_trace_path);
        if (estimability_trace)
            // col1..col5 carry a DIR row's ≤5 spanning columns (the dump emits at
            // most five); col6..col8 exist so the RUN record's key=value payload
            // has headroom without hijacking a numeric column.  A reader must key
            // off `kind`, never off column position alone.
            estimability_trace << "kind,week,tow,n_shared,declared_dim,ev_max,cutoff,"
                                  "n_below,ev,col1,col2,col3,col4,col5,col6,col7,col8\n";
        else
            log->warn("Network (Card DG): failed to open estimability trace {}",
                      estimability_trace_path.string());
    }
    if (nc.trace_events) {
        events_trace.open(events_trace_path);
        if (events_trace)
            // Long format: one row per discrete QC action.  tier in {T1_if_slip,
            // T2_within_epoch, T3_raim, T4_attrib, T5_clock_model, T6_clock_monitor,
            // GATE, SIGMA0, PIVOT, RETRO}.  statistic/threshold blank where not in
            // scope; detail is comma-sanitized (',' -> ';').  Rows are written even on
            // epochs that are subsequently WITHHELD — that is the disposition story.
            events_trace << "week,tow,tier,event,sat,station,statistic,threshold,"
                            "action,detail\n";
        else
            log->warn("Network: failed to open events trace {}", events_trace_path.string());
    }
    if (nc.trace_zwd) {
        zwd_trace.open(zwd_trace_path);
        if (zwd_trace)
            // zwd_m = a-priori state + reconstructed correction (the estimated correction
            // about the modeled a-priori); std_m = datum-conditioned formal σ; carried = a
            // ZWD carry prior was actually injected this epoch.
            zwd_trace << "week,tow,station,zwd_m,std_m,carried\n";
        else
            log->warn("Network: failed to open ZWD trace {}", zwd_trace_path.string());
    }
    if (nc.trace_sat_bias_states) {
        sat_bias_states_trace.open(sat_bias_states_trace_path);
        if (sat_bias_states_trace)
            // One row per emitted (satellite, code signal) each emitted epoch.
            // code_bias_m is the WIRE value (the same number the B record carries, after
            // the S-transform), sigma_m its σ̂0-scaled formal σ from the gauge-nulled
            // shared covariance, s0 the σ̂0 EWMA that scaled it (so a grader can undo the
            // scaling), and pinned marks a column that is a datum pin this epoch — such a
            // column's variance is a gauge artefact, so it is reported with an EMPTY
            // sigma_m rather than a misleadingly tiny one (blank = "not emitted", never 0).
            sat_bias_states_trace << "week,tow,sat,signal,code_bias_m,sigma_m,s0,pinned\n";
        else
            log->warn("Network: failed to open satellite bias-state trace {}",
                      sat_bias_states_trace_path.string());
    }
    if (nc.trace_rx_states) {
        rx_states_trace.open(rx_states_trace_path);
        if (rx_states_trace)
            // One row per (station, receiver state) each emitted epoch.  kind is CLK or
            // CB; key is the RINEX code for a CB row and empty for the clock.  value_m =
            // a-priori state + reconstructed correction (the same x_lin + x_r convention
            // network_zwd.csv uses); std_m = datum-conditioned formal σ.
            rx_states_trace << "week,tow,station,kind,key,value_m,std_m\n";
        else
            log->warn("Network: failed to open receiver-state trace {}",
                      rx_states_trace_path.string());
    }
    if (nc.trace_station_residuals) {
        station_res_trace.open(station_res_trace_path);
        if (station_res_trace)
            // One row per post-fit RAIM cell: non-pivot station (pivot never appears — it is
            // skipped in the cell loop) x carried-ambiguity sat.  s_raw = measurement-
            // standardized residual (B2-normalized when raim_redundancy_normalize on),
            // s_norm = s_raw / σ̂0, sigma0 = the epoch robust variance factor.  s values are
            // ABSOLUTE (non-negative).  Rows exist only for RAIM epochs (raim && carry_warm).
            station_res_trace << "week,tow,station,sat,s_raw,s_norm,sigma0\n";
        else
            log->warn("Network: failed to open station-residual trace {}",
                      station_res_trace_path.string());
    }
    if (nc.trace_raim_redundancy) {
        raim_redun_trace.open(raim_redun_trace_path);
        if (raim_redun_trace)
            // SPEC_t3_null_derivation §7.1.  One row per post-fit RAIM PHASE ROW —
            // one level FINER than the cell, keyed (station, sat, signal, row).  The
            // cell is a max OVER signals, so station_res_trace structurally cannot
            // answer "is r_i signal-dependent"; this can.
            //   r_i      Baarda redundancy on the gauge-nulled free covariance.  EMPTY
            //            when rowRedundancy threw for this station — recorded rather
            //            than silently dropped, so a station that never yields r_i is
            //            visible instead of just absent.
            //   z_meas   |v|·√w — what the SHIPPED statistic pools.
            //   z_baarda z_meas/√r_i — what it WOULD pool under
            //            raim_redundancy_normalize, emitted here WITHOUT arming it, so
            //            both standardizations are comparable on ONE solve.
            //   below_floor  1 if the normalization would DROP this row (r_i ≤ the
            //            Baarda redundancy floor) — the population it silently removes.
            raim_redun_trace << "week,tow,station,sat,signal,row,w,r_i,resid_m,"
                                "z_meas,z_baarda,below_floor\n";
        else
            log->warn("Network: failed to open RAIM redundancy census {}",
                      raim_redun_trace_path.string());
    }
    if (config_.networkRaimStationFactor()) {
        station_factor_trace.open(station_factor_trace_path);
        if (station_factor_trace)
            // QUEUE item 4 R2 (SPEC_t3_null_derivation §6/§7.5).  factor = the
            // per-epoch trimmed variance component Σw·v²/Σr_i (−1 = no checkable
            // redundancy this epoch — "unavailable is not 0"); ewma = the slow
            // declared channel; ratio = ewma / network median of ewmas (the
            // depart statistic); n_rows/n_trim = the trim census; departed = the
            // station is currently past the depart edge (hysteresis state).
            station_factor_trace << "week,tow,station,factor,ewma,ratio,"
                                    "n_rows,n_trim,departed\n";
        else
            log->warn("Network: failed to open station-factor trace {}",
                      station_factor_trace_path.string());
    }
    if (config_.networkLevelAuthorityCsgf()) {
        level_authority_trace.open(level_authority_trace_path);
        if (level_authority_trace)
            // LV-6 (SPEC_level_authority_internal §3).  FLEET rows: level =
            // the two-tier reduction (per-station info-weighted means → fleet
            // median), disp = 1.4826·MAD across station means (never a formal
            // σ — the kernel variances are documented optimistic), a00 filled
            // only when the SH channel exists.  STA rows: the tier-1 station
            // mean and its LOS count (level column reused; disp/n_sta/a00
            // blank).  All values vertical TECU in the pair-exact conversion.
            level_authority_trace << "week,tow,kind,station,level_tecu,"
                                     "disp_tecu,n_sta,n_los,n_immature,a00_tecu\n";
        else
            log->warn("Network: failed to open level-authority census {}",
                      level_authority_trace_path.string());
        // LV-6 G2 (§4): the census's OWN sampling, so an offline reference field
        // is reduced through the identical IPP set and the identical weights
        // instead of a re-derived all-visible one.  Separate file on purpose —
        // the census CSV above must stay byte-identical for G3.
        if (config_.networkLevelAuthorityLosDump()) {
            level_authority_los_trace.open(level_authority_los_trace_path);
            if (level_authority_los_trace)
                level_authority_los_trace
                    << "week,tow,station,sat,ipp_lat_deg,ipp_lon_deg,mapping,"
                       "el_deg,v_tecu,weight,n_cs\n";
            else
                log->warn("Network: failed to open level-authority LOS dump {}",
                          level_authority_los_trace_path.string());
        }
    }

    // INC-4: capture the L1/L15 config echoes once (pure getter reads).
    es_sigma0_trigger_   = config_.networkRaimSigma0Trigger();
    es_withhold_ceiling_ = config_.networkRaimWithholdSigma0();
}

// The openAll tail for the two MW/WL dumps.  Split out (not folded into openAll)
// because their arming expressions are derived in the run TU from the WL zone-2
// booleans — carry_invariant, ewl_on, wl_inject_on, lane_fix_on, carry_innov_gate —
// which are established well after openAll runs; re-deriving that chain here would
// bit-rot against the driver.  The LIFECYCLE is what matters and it is now identical
// to every other stream: one truncating open with the header, once per run, before
// the epoch loop, closed in closeAll.
void NetworkRunTraces::openMwWlDumps(bool d5_armed, bool lane_trace_armed,
                                     const std::shared_ptr<spdlog::logger>& log) {
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);
    if (d5_armed) {
        d5_flags_trace.open(d5_flags_trace_path);
        if (d5_flags_trace)
            // raw_stepped is deliberately TRI-STATE (1 / 0 / blank) — see the writer
            // banner in NetworkInstallCarry.cpp.  Blank means "no evidence", which is
            // NOT the same claim as 0 ("a step could be formed and it was flat").
            // The five algebra columns after raw_stepped are the churn card's instrument
            // (2026-08-02): std_innov alone cannot say whether a standing 230σ means the
            // innovation is large or the formal Cov(v) is small.  z_post differenced across
            // epochs for ONE chord is the estimate's measured wander; var_v is the formal
            // claim about exactly that quantity, so the ratio is readable per chord without
            // re-running anything.  dw_w is the station's SH-constraint deweight weight
            // (ctx.carry.topo_prev_w, this epoch — blank unless network.trace_topology is
            // armed), carried here because the chronic-flagger roster was measured to
            // overlap the deweight gate's chronic >3σ SH-misfit roster and the correlation
            // should be gradeable from one file.
            d5_flags_trace << "week,tow,group,chord_u,chord_v,station,amb_key,std_innov,"
                              "ewl_stasat,hold_chord,injected_prev,raw_stepped,"
                              // RS-2b: warmup_suppressed — 1 where the trip fell inside the
                              // post-resume D5 warm-up and was counted without being acted
                              // on.  Appended (never inserted) so an existing reader's
                              // positional columns are unchanged.
                              "zhat,v,var_v,sigma0,z_post,dw_w,warmup_suppressed\n";
        else
            log->warn("Network: failed to open D5 flag dump {}",
                      d5_flags_trace_path.string());
    }
    if (lane_trace_armed) {
        wl_lane_trace.open(wl_lane_trace_path);
        if (wl_lane_trace)
            wl_lane_trace << "week,tow,chord_u,chord_v,station,sat,lane_cyc,frac_cyc,"
                             "w,decided,held,injected\n";
        else
            log->warn("Network: failed to open WL lane trace {}",
                      wl_lane_trace_path.string());
    }
}

void NetworkRunTraces::writeStationRoster(const std::filesystem::path& log_dir,
                                          const std::vector<NetStation>& stations,
                                          const StationConfig* held_out,
                                          const std::shared_ptr<spdlog::logger>& log) {
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    const std::filesystem::path roster_path = log_dir / "network_stations.csv";
    std::ofstream roster(roster_path);
    if (roster) {
        roster << "station,role,ecef_x,ecef_y,ecef_z\n";
        for (const auto& st : stations)
            roster << st.name << ',' << (st.is_pivot ? "datum" : "network") << ','
                   << fmt::format("{:.4f}", st.ecef.x()) << ','
                   << fmt::format("{:.4f}", st.ecef.y()) << ','
                   << fmt::format("{:.4f}", st.ecef.z()) << '\n';
        if (held_out) {
            const auto& p = held_out->position_ecef;
            roster << held_out->name << ",held_out,"
                   << fmt::format("{:.4f}", p[0]) << ','
                   << fmt::format("{:.4f}", p[1]) << ','
                   << fmt::format("{:.4f}", p[2]) << '\n';
        }
        log->info("Network: wrote station roster {}", roster_path.string());
    } else {
        log->warn("Network: failed to open station roster {}", roster_path.string());
    }
}

// L1: write one network_epoch_status.csv row for this epoch (guarded; no-op when
// OFF).  Relocated VERBATIM from the runNetwork() lambda (decomposition INC-4).
std::size_t NetworkRunTraces::writeConsumerSatRows(const GnssTime& t,
                                                   const std::vector<ConsumerSatRow>& rows) {
    // Card CST.  The stream exists only when the harness AND network.trace_consumer_sat
    // are both armed, so an unarmed run never reaches the loop and an armed run with a
    // deferred consumer epoch writes nothing (the engine hands back an empty buffer —
    // invariant #4's "0 rows" is a real reading, not a missing file).
    if (!cl_sat_trace.is_open()) return 0;
    for (const auto& r : rows) cl_sat_trace << formatConsumerSatRow(t.week, t.tow, r);
    return rows.size();
}

void NetworkRunTraces::emitEpochStatus(const GnssTime& t, const char* disposition,
                                       const EpochStatusRow& r) {
    // Config echoes captured at openAll (single-sourced with the L15 manifest).
    const double es_sigma0_trigger   = es_sigma0_trigger_;
    const double es_withhold_ceiling = es_withhold_ceiling_;
        if (!epoch_status_trace.is_open()) return;
        auto& o = epoch_status_trace;
        o << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
          << disposition << ',';
        if (r.sigma0)          o << fmt::format("{:.4f}", *r.sigma0);
        o << ',';
        if (r.n_adapt)         o << *r.n_adapt;
        o << ',';
        if (r.n_present)       o << *r.n_present;
        o << ',';
        if (r.n_clock_sats)    o << *r.n_clock_sats;
        o << ',';
        if (r.clock_median_m)  o << fmt::format("{:.4f}", *r.clock_median_m);
        o << ',';
        if (r.clock_std_m)     o << fmt::format("{:.4f}", *r.clock_std_m);
        o << ',';
        o << r.worst_sat << ',';
        if (r.worst_dev_m)     o << fmt::format("{:.4f}", *r.worst_dev_m);
        o << ',';
        if (r.n_refloat_sats)  o << *r.n_refloat_sats;
        o << ',';
        if (r.n_retro_applied) o << *r.n_retro_applied;
        o << ',';
        if (r.datum_changed)   o << *r.datum_changed;
        o << ',';
        o << fmt::format("{:.4f}", es_sigma0_trigger) << ','
          << fmt::format("{:.4f}", es_withhold_ceiling) << ',';
        if (r.n_arc_refloat)   o << *r.n_arc_refloat;   // T2d
        // P1-1 carry churn / P1-2 deweight state / P1-4 post-fit σ̂0.  BLANK (not 0)
        // when not in scope at this disposition — the file's existing convention.
        o << ',';
        if (r.n_d5_flags)      o << *r.n_d5_flags;
        o << ',';
        if (r.n_d5_retaps)     o << *r.n_d5_retaps;
        o << ',';
        if (r.n_chord_births)  o << *r.n_chord_births;
        o << ',';
        if (r.n_slip_refloats) o << *r.n_slip_refloats;
        o << ',';
        if (r.n_dw_engaged)    o << *r.n_dw_engaged;
        o << ',';
        if (r.n_dw_zero)       o << *r.n_dw_zero;
        o << ',';
        if (r.dw_min_w)        o << fmt::format("{:.4f}", *r.dw_min_w);
        o << ',';
        if (r.dw_mean_w)       o << fmt::format("{:.4f}", *r.dw_mean_w);
        o << ',';
        if (r.sigma0_pf)       o << fmt::format("{:.4f}", *r.sigma0_pf);
        o << ',';
        if (r.sigma0_pf_code)  o << fmt::format("{:.4f}", *r.sigma0_pf_code);
        o << ',';
        if (r.sigma0_pf_phase) o << fmt::format("{:.4f}", *r.sigma0_pf_phase);
        o << ',';
        if (r.n_pf_rows)       o << *r.n_pf_rows;
        o << ',';
        if (r.n_redun_rows)         o << *r.n_redun_rows;
        o << ',';
        if (r.n_uncheckable_rows)   o << *r.n_uncheckable_rows;
        o << ',';
        if (r.uncheckable_max_s)    o << fmt::format("{:.6e}", *r.uncheckable_max_s);
        o << ',';
        if (r.n_unobservable_local) o << *r.n_unobservable_local;
        // INC-1 prefit census: four columns per DATA class, in NetRowClass order.
        // Absent ⇒ 16 blank fields (the census did not walk this epoch), so the
        // row width is constant and an old reader that stops at the previous
        // column keeps working.
        for (int t = 0; t < 4; ++t) {
            o << ',';
            if (r.prefit_census) o << fmt::format("{:.6e}", r.prefit_census->max_s[t]);
            o << ',';
            if (r.prefit_census) o << fmt::format("{:.6e}", r.prefit_census->p99_s[t]);
            o << ',';
            if (r.prefit_census) o << r.prefit_census->n_gt_10[t];
            o << ',';
            if (r.prefit_census) o << r.prefit_census->n_gt_1000[t];
        }
        // PD-safe fold INC-0: three UNCONDITIONAL columns (see the header write in
        // openAll).  Blank on an epoch the invariant carry did not fold on — "zero
        // degenerate blocks" and "the carry never ran" are different statements, and
        // conflating them is exactly how the frozen 78 % stayed invisible.
        o << ',';
        if (r.ddfold_degen_blocks) o << *r.ddfold_degen_blocks;
        o << ',';
        if (r.ddfold_degen_rows) o << *r.ddfold_degen_rows;
        o << ',';
        if (r.ddfold_min_pivot_rel) o << fmt::format("{:.6e}", *r.ddfold_min_pivot_rel);
        // INC-3 measurement-admission census: two columns, present in the schema
        // only when the card is armed (see the header write in openAll).  Blank on
        // an epoch the screen did not run — the file's "unavailable is not 0"
        // convention, which matters here because a genuine 0.0 worst-|z| and a
        // skipped epoch are very different statements.
        if (es_adm_columns_) {
            o << ',';
            if (r.adm_sta_worst_z) o << fmt::format("{:.6e}", *r.adm_sta_worst_z);
            o << ',';
            if (r.adm_net_worst_z) o << fmt::format("{:.6e}", *r.adm_net_worst_z);
        }
        // Prior-residual census: five columns, present in the schema only when the
        // card is armed.  Blank on an epoch the census did not run — a genuine
        // 0-row epoch (every carry family absent) and an un-run epoch say very
        // different things, and standing invariant #4 turns on being able to tell
        // "ran, saw nothing" from "never ran".
        if (es_prires_columns_) {
            o << ',';
            if (r.prires_rows)        o << *r.prires_rows;
            o << ',';
            if (r.prires_rows_resp)   o << *r.prires_rows_resp;
            o << ',';
            if (r.prires_rows_frame)  o << *r.prires_rows_frame;
            o << ',';
            if (r.prires_n_reported)  o << *r.prires_n_reported;
            o << ',';
            if (r.prires_max_t)       o << fmt::format("{:.6e}", *r.prires_max_t);
        }
        // Constraint-aware global σ̂₀: three columns, present only when the rider
        // is armed.  sigma0_global is BLANK (not 0) on an epoch whose Σr was
        // non-positive — "not formed" and "formed at zero" are different states,
        // and s0g_flags says which.  s0g_flags itself is written whenever the
        // epoch ran, INCLUDING when it is 0: a reader must be able to tell a
        // clean epoch from an un-run one without cross-referencing another file.
        if (es_s0g_columns_) {
            o << ',';
            if (r.s0g_sigma0)  o << fmt::format("{:.6e}", *r.s0g_sigma0);
            o << ',';
            if (r.s0g_tension) o << fmt::format("{:.6e}", *r.s0g_tension);
            o << ',';
            if (r.s0g_flags)   o << *r.s0g_flags;
        }
        // R0 within-epoch tier observability: four columns, present in the schema
        // only when the tier is armed (see the header write in openAll).  Blank on
        // an epoch the tier did not assess — "unavailable is not 0", and here the
        // distinction is load-bearing: a genuine σ̂₀_first row IS the null sample
        // QUEUE item 4 fits, so a fabricated 0 would poison the fit.
        if (es_we_columns_) {
            o << ',';
            if (r.sigma0_first)      o << fmt::format("{:.4f}", *r.sigma0_first);
            o << ',';
            if (r.n_we_refloat_sats) o << *r.n_we_refloat_sats;
            o << ',';
            if (r.we_clk_move_max_m) o << fmt::format("{:.4f}", *r.we_clk_move_max_m);
            o << ',';
            o << r.we_clk_move_max_sat;
        }
        o << '\n';
}

// L2: write one network_events.csv row for a discrete QC action (guarded; no-op
// OFF).  Relocated VERBATIM (decomposition INC-4).
void NetworkRunTraces::emitEvent(const GnssTime& t, const char* tier, const char* event,
                                 const std::string& sat, const std::string& station,
                                 std::optional<double> statistic, std::optional<double> threshold,
                                 const char* action, const std::string& detail) {
        if (!events_trace.is_open()) return;
        std::string d = detail;
        for (char& c : d) if (c == ',') c = ';';   // sanitize: keep the CSV single-field
        auto& o = events_trace;
        o << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
          << tier << ',' << event << ',' << sat << ',' << station << ',';
        if (statistic) o << fmt::format("{:.6f}", *statistic);
        o << ',';
        if (threshold) o << fmt::format("{:.6f}", *threshold);
        o << ',' << action << ',' << d << '\n';
}

void NetworkRunTraces::writeCarryVceApplyRows(const GnssTime& t,
                                              const NetworkCarryVceApply& apply,
                                              int& hour_state) {
    if (!carry_vce_apply_trace.is_open()) return;
    auto& o = carry_vce_apply_trace;
    for (const auto& kv : apply.groups()) {
        const NetworkCarryVceApply::Group& g = kv.second;
        o << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
          << NetworkCarryVceApply::familyName(g.family) << ','
          << (g.sys ? std::string(1, g.sys) : std::string()) << ','
          << fmt::format("{:.1f}", g.n_eff) << ','
          << fmt::format("{:.6e}", g.robust_floor) << ','
          << fmt::format("{:.6e}", g.hand_fold_var) << ','
          << fmt::format("{:.6e}", g.target) << ','
          << fmt::format("{:.6e}", g.alpha2) << ','
          // The LASER 5-95 convergence readout.  Printed at full precision because
          // "is it 1.0?" is the whole question and a 4-digit render of 1.0003 vs
          // 1.3 is not the same reading.
          << fmt::format("{:.6e}", g.sigma_hat_post) << ','
          << NetworkCarryVceApply::freezeName(g.freeze) << ','
          << (g.at_ceiling ? 1 : 0) << ',' << (g.at_unit ? 1 : 0) << ','
          << g.n_steps << ',' << g.n_frozen << ','
          << g.n_slew_up << ',' << g.n_slew_down << ','
          << g.n_ceiling_ep << ',' << g.n_unit_ep << ','
          << fmt::format("{:.6e}", g.alpha2_max) << '\n';
    }
    // Durability: nothing flushes this stream on its own (only health_trace does),
    // and it is the card's graded artifact — an hourly flush bounds what a killed
    // or WER-zombied run can lose to one content hour.
    const int hour = static_cast<int>(t.tow / 3600.0);
    if (hour != hour_state) { o.flush(); hour_state = hour; }
}

void NetworkRunTraces::writeEstimabilityEpochRow(const GnssTime& t, double tow, int n_shared,
                                                 int declared_dim, double ev_max, double cutoff,
                                                 int n_below) {
    if (!estimability_trace.is_open()) return;
    // {:.9e} on ev_max/cutoff (not the log's {:.3e}): the CSV is the analyzer's
    // input, and DG3's placement rule is a DECADE argument — throwing away
    // mantissa digits that cost nothing here would be a false economy.
    estimability_trace << "EPOCH," << t.week << ',' << fmt::format("{:.3f}", tow) << ','
                       << n_shared << ',' << declared_dim << ','
                       << fmt::format("{:.9e}", ev_max) << ','
                       << fmt::format("{:.9e}", cutoff) << ','
                       << n_below << ",,,,,,,,,\n";   // ev + col1..col8 empty
}

void NetworkRunTraces::writeEstimabilityDirRow(
        const GnssTime& t, double ev,
        const std::vector<std::pair<std::string, double>>& top) {
    if (!estimability_trace.is_open()) return;
    estimability_trace << "DIR," << t.week << ',' << fmt::format("{:.3f}", t.tow)
                       << ",,,,,," << fmt::format("{:.9e}", ev);
    for (int i = 0; i < 8; ++i) {
        estimability_trace << ',';
        if (i < static_cast<int>(top.size())) {
            // Labels are driver-built ("CLK|G01", "PB|C26|L7D", "SH|2|1|C") and
            // contain no comma by construction; sanitize anyway so a future label
            // format cannot silently shift every downstream column.
            std::string lab = top[static_cast<size_t>(i)].first;
            for (char& c : lab) if (c == ',') c = ';';
            estimability_trace << lab << '='
                               << fmt::format("{:.4f}", top[static_cast<size_t>(i)].second);
        }
    }
    estimability_trace << '\n';
}

void NetworkRunTraces::writeEstimabilityRunRow(const NetworkServiceCounters& counters,
                                               double rank_gate_tol) {
    if (!estimability_trace.is_open()) return;
    // DG6 run totals, carried inside the ONE artifact a trace-armed run is
    // allowed to add (DG5) rather than as a new run-end log line — the estim
    // golden arm byte-gates the log text.  n_shared/declared_dim/ev_max are
    // reused as the evaluated/under/over counts and n_below/ev as the observed
    // min/max: a RUN record is a different record shape, and the `kind` column
    // is what tells the reader so.  Column meanings for kind=RUN:
    //   n_shared     -> n_gate_evaluated       declared_dim -> n_gate_withheld_under
    //   ev_max       -> n_gate_withheld_over   cutoff       -> the RESOLVED tolerance
    //   n_below      -> n_emitted              ev           -> n_gate_withheld (total)
    // col1..col8 are self-describing key=value tokens, so a reader never has to
    // rely on their position:
    //   obs_min, obs_max, heal_attempt_epochs, heal_attempt_cols, healed_epochs,
    //   healed_cols, cbadm_phase_orphan_key_epochs.
    // heal ATTEMPTS are carried alongside the recoveries deliberately: attempts
    // with zero recoveries is a live heal that cannot resolve the residual, and
    // the success-only pair alone reports that as a dead one (PA2).
    estimability_trace << "RUN,0,0.000,"
                       << counters.n_gate_evaluated << ','
                       << counters.n_gate_withheld_under << ','
                       << counters.n_gate_withheld_over << ','
                       << fmt::format("{:.9e}", rank_gate_tol) << ','
                       << counters.n_emitted << ','
                       << counters.n_gate_withheld << ','
                       << "obs_min=" << counters.nullspace_obs_min << ','
                       << "obs_max=" << counters.nullspace_obs_max << ','
                       << "heal_attempt_epochs=" << counters.n_heal_attempt_epochs << ','
                       << "heal_attempt_cols=" << counters.n_heal_attempt_cols << ','
                       << "healed_epochs=" << counters.n_healed_epochs << ','
                       << "healed_cols=" << counters.n_healed_cols << ','
                       << "cbadm_phase_orphan_key_epochs="
                       << counters.n_cbadm_phase_orphan_key_epochs << ",\n";
}

void NetworkRunTraces::rotateServiceCsvs(const NetworkConfig& nc,
                                         const std::shared_ptr<spdlog::logger>& log) {
    // P1: size-based rotation of the unbounded service CSVs
    // (network.trace_rotate_mb > 0).  Checked once per processed epoch,
    // BEFORE this epoch's rows land, so a file overshoots by at most one
    // epoch of rows.  Each roll shifts file.csv.k → file.csv.(k+1) (oldest
    // deleted), retires the live file to file.csv.1, and reopens fresh with
    // the header re-written — every rolled generation parses standalone.
    // OFF (default 0) ⇒ maybeRotateCsv touches nothing (byte-identical).
    if (nc.trace_rotate_mb > 0.0) {
        const struct {
            std::ofstream*               os;
            const std::filesystem::path* path;
            const char*                  header;
        } rotating[] = {
            {&trace,        &trace_path,        kTraceHeader},
            {&soln,         &soln_path,         kSolnHeader},
            {&health_trace, &health_trace_path, kHealthHeader},
            // FC-1: a fast tick writes a row per DECIMATED-OUT epoch, so at an RT 1 s
            // cadence this stream grows faster than any other here — it belongs in the
            // rotation set by construction, not as an afterthought.  maybeRotateCsv is a
            // strict no-op while the stream is closed (unarmed tier).
            {&fast_clock_trace, &fast_clock_trace_path, kFastClockHeader},
        };
        for (const auto& r : rotating)
            if (maybeRotateCsv(*r.os, *r.path, r.header,
                               nc.trace_rotate_mb, nc.trace_rotate_keep))
                log->info("Network (P1): rotated {} (live file exceeded {:.1f} MiB; "
                          "keeping {} rolled generation(s)).",
                          r.path->filename().string(), nc.trace_rotate_mb,
                          nc.trace_rotate_keep);
    }
}

void NetworkRunTraces::writeHealthRow(const GnssTime& t, const NetworkConfig& nc,
                                      NetworkServiceCounters& counters,
                                      const std::chrono::steady_clock::time_point& t_loop_begin,
                                      bool ura_on, bool raim_on, bool pl_on,
                                      double ura_sigma0_ewma,
                                      const std::map<std::string, std::string>& last_pivot,
                                      double grade_metric_s) {
    // Alias block (INC-4 migration mechanic): the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& last_health_write_s      = counters.last_health_write_s;
    auto& health_win_ms_anchor     = counters.health_win_ms_anchor;
    auto& health_win_epochs_anchor = counters.health_win_epochs_anchor;
    const auto& n_emitted          = counters.n_emitted;
    const auto& n_skipped          = counters.n_skipped;
    const auto& n_gate_withheld    = counters.n_gate_withheld;
    const auto& n_sigma0_withheld  = counters.n_sigma0_withheld;
    const auto& n_runaway_epochs   = counters.n_runaway_epochs;
    const auto& last_n_present     = counters.last_n_present;
    const auto& last_n_clock_sats  = counters.last_n_clock_sats;
    const auto& last_emit_content_s = counters.last_emit_content_s;
    const auto& n_loop_epochs      = counters.n_loop_epochs;
    // RTB-6: operator health heartbeat every network.health_interval_s of content
    // time.  Reports cumulative state THROUGH the previous epoch (this epoch's
    // per-present/emit outcomes are computed below), which is exactly the "is the
    // service alive, emitting, and how contaminated/fast" signal a supervisor
    // polls.  Flushed per write so a live tail sees it immediately.  Guarded ⇒
    // OFF (default) writes nothing (byte-identical).
    if (health_trace.is_open()) {
        const double now_s = TimeUtils::continuousGpsSeconds(t);
        if (last_health_write_s < 0.0
            || (now_s - last_health_write_s) >= nc.health_interval_s) {
            const double loop_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_loop_begin).count();
            // P4(b): mean wall ms per loop epoch over the window since the
            // PREVIOUS health row (first row: since loop start) — a windowed
            // "is it slowing down NOW" signal, not a cumulative run mean.
            const size_t win_epochs = n_loop_epochs - health_win_epochs_anchor;
            const double ms_per_epoch_window =
                win_epochs > 0
                    ? (loop_ms - health_win_ms_anchor) / static_cast<double>(win_epochs)
                    : 0.0;
            const double emit_age =
                last_emit_content_s >= 0.0 ? (now_s - last_emit_content_s) : -1.0;
            // P4(a): σ̂0-EWMA availability — the EWMA updates only inside the
            // warm post-fit RAIM block (emit_ura && raim && phase_leveling);
            // with any of them unarmed the column emits the −1 "unavailable"
            // sentinel, never the frozen 1.0 init.  (During the phase-leveling
            // warmup it still reads 1.0 — updates begin once carry_warm.)
            const bool ewma_avail = ura_on && raim_on && pl_on;
            health_trace << t.week << ',' << fmt::format("{:.3f}", t.tow)
                         << ',' << n_emitted << ',' << n_skipped << ',' << n_gate_withheld
                         << ',' << n_sigma0_withheld << ',' << n_runaway_epochs << ','
                         << (ewma_avail ? fmt::format("{:.4f}", ura_sigma0_ewma)
                                        : std::string("-1"))
                         << ',' << last_n_present << ',' << last_n_clock_sats
                         << ',' << fmt::format("{:.3f}", ms_per_epoch_window)
                         << ',' << fmt::format("{:.3f}", emit_age)
                         // P2: realized per-constellation pivots of the last EMITTED
                         // product (the emit-baseline last_pivot map, merge-updated
                         // only at the emit site) — empty before the first emit.
                         << ',' << formatPivotMap(last_pivot)
                         // RTB-7(c): cold-start-to-product-grade (s of product time
                         // from first emit to the quality-ramp lift).  −1 before the
                         // lift or when the ramp is off; 0 = warm resume.
                         << ',' << (grade_metric_s >= 0.0
                                        ? fmt::format("{:.1f}", grade_metric_s)
                                        : std::string("-1"))
                         << '\n';
            health_trace.flush();   // a supervisor tails this live
            last_health_write_s      = now_s;
            health_win_ms_anchor     = loop_ms;
            health_win_epochs_anchor = n_loop_epochs;
        }
    }
}

void NetworkRunTraces::writeRunManifestStartup(const GnssConfig& config_,
                                               const NetworkConfig& nc,
                                               const NetworkKnobs& knobs,
                                               const std::filesystem::path& config_path_,
                                               std::size_t n_stations, int n_threads,
                                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4): the relocated body reads the former knob locals
    // (value copies preserve the exact original types for fmt).
    const bool   pl_on = knobs.pl_on;
    const bool   zwd_on = knobs.zwd_on;
    const bool   ura_on = knobs.ura_on;
    const bool   ura_retro = knobs.ura_retro;
    const bool   raim_on = knobs.raim_on;
    const bool   raim_redun = knobs.raim_redun;
    const bool   perconst_on = knobs.perconst_on;
    const bool   clk_mon_on = knobs.clk_mon_on;
    const bool   clk_model_on = knobs.clk_model_on;
    const bool   pivot_vr_signal = knobs.pivot_vr_signal;
    const bool   emit_clock_disc = knobs.emit_clock_disc;
    const bool   sticky_pivot = knobs.sticky_pivot;
    const bool   clock_signal_basis_hold = knobs.clock_signal_basis_hold;
    const bool   scoped_handover = knobs.scoped_handover;
    const bool   pb_excl_unmatched = knobs.pb_excl_unmatched;
    const bool   emit_gen_id = knobs.emit_gen_id;
    const bool   ser_disc_ctrs = knobs.ser_disc_ctrs;
    const bool   emit_epoch_status = knobs.emit_epoch_status;
    const bool   emit_clock_rate_on = knobs.emit_clock_rate_on;
    const double pl_q_amb = knobs.pl_q_amb;
    const double pl_max_gap_s = knobs.pl_max_gap_s;
    const double pl_var_floor = knobs.pl_var_floor;
    const bool   pl_los_scale = knobs.pl_los_scale;   // F1: per-LOS B_eff scale at inject
    const bool   pl_local_block = knobs.pl_local_block; // F3: station-wide whitened local block at inject
    const double pb_var_floor = knobs.pb_var_floor;
    const double cb_var_floor = knobs.cb_var_floor;
    const double zwd_q = knobs.zwd_q;
    const double ura_alpha = knobs.ura_alpha;
    const double ura_refloat_inflate = knobs.ura_refloat_inflate;
    const double raim_k = knobs.raim_k;
    const double raim_withhold = knobs.raim_withhold;
    const double clk_mon_abs = knobs.clk_mon_abs;
    const double clk_mon_jump = knobs.clk_mon_jump;
    const double clk_mon_reinit_sig = knobs.clk_mon_reinit_sig;
    const double clk_model_q1 = knobs.clk_model_q1;
    const double clk_model_q2 = knobs.clk_model_q2;
    const double clk_model_k = knobs.clk_model_k;
    const double clk_model_max_gap = knobs.clk_model_max_gap;
    const double contract_rebroadcast_s = knobs.contract_rebroadcast_s;
    const double q_clock = knobs.q_clock;
    const double q_pb = knobs.q_pb;
    const double q_cb = knobs.q_cb;
    const int    pl_warmup = knobs.pl_warmup;
    const int    raim_min_sta = knobs.raim_min_sta;
    const int    perconst_min_rows = knobs.perconst_min_rows;
    const double es_sigma0_trigger = es_sigma0_trigger_;
    // L15: run-provenance manifest (network.write_run_manifest).  JSONL — a startup record
    // here (config hash + build + full toggle/knob dump) plus an end-of-run record appended
    // after the completion line.  Hand-emitted (no JSON lib); does NOT read git state (the
    // tree is shared across concurrent sessions — config hash + toggle dump is the honest
    // provenance).  create_directories(log_dir_) already ran in the trace-open scope above.
    if (nc.write_run_manifest) {
        auto jesc = [](const std::string& s) {
            std::string o; o.reserve(s.size() + 8);
            for (char c : s) {
                if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
                else if (c == '\n') o += "\\n";
                else if (c == '\r') o += "\\r";
                else o.push_back(c);
            }
            return o;
        };
        uint64_t cfg_hash = 1469598103934665603ULL;   // FNV-1a-64 over the raw config bytes
        {
            std::ifstream cf(config_path_, std::ios::binary);
            char buf[4096];
            while (cf && (cf.read(buf, sizeof buf) || cf.gcount())) {
                const std::streamsize n = cf.gcount();
                for (std::streamsize k = 0; k < n; ++k)
                    cfg_hash = (cfg_hash ^ static_cast<unsigned char>(buf[k])) * 1099511628211ULL;
                if (n < static_cast<std::streamsize>(sizeof buf)) break;
            }
        }
        char utc[32] = "unknown";
        {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
#ifdef _WIN32
            gmtime_s(&tmv, &t);
#else
            gmtime_r(&t, &tmv);
#endif
            std::strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        }
        // Config INHERITANCE provenance.  cfg_hash above covers only the LEAF file's bytes,
        // so once a config can "extends" a base, two leaves with identical bytes and
        // different bases hash the SAME — the manifest would silently claim two different
        // runs were the same configuration.  Record each merged base with its own content
        // hash.  Emitted ONLY when the chain is non-empty, so every manifest written for a
        // config without inheritance (i.e. every pre-existing config) is byte-identical to
        // what this writer produced before, and historical hash comparisons still hold.
        std::string extends_json;
        {
            const auto& chain = config_.configExtendsChain();
            if (!chain.empty()) {
                extends_json = "\"config_extends\":[";
                for (std::size_t i = 0; i < chain.size(); ++i) {
                    uint64_t h = 1469598103934665603ULL;
                    std::ifstream bf(chain[i], std::ios::binary);
                    char buf[4096];
                    while (bf && (bf.read(buf, sizeof buf) || bf.gcount())) {
                        const std::streamsize n = bf.gcount();
                        for (std::streamsize k = 0; k < n; ++k)
                            h = (h ^ static_cast<unsigned char>(buf[k])) * 1099511628211ULL;
                        if (n < static_cast<std::streamsize>(sizeof buf)) break;
                    }
                    extends_json += fmt::format("{}{{\"path\":\"{}\",\"hash_fnv1a64\":\"{:016x}\"}}",
                                                i ? "," : "", jesc(chain[i]), h);
                }
                extends_json += "],";
            }
        }
        std::ofstream mf(manifest_path, std::ios::trunc);
        if (mf) {
            mf << "{\"record\":\"startup\","
               << "\"written_utc\":\"" << utc << "\","
               << "\"config_path\":\"" << jesc(config_path_.string()) << "\","
               << extends_json
               << fmt::format("\"config_hash_fnv1a64\":\"{:016x}\",", cfg_hash)
               << "\"build\":{\"compiled\":\"" << __DATE__ " " __TIME__ << "\"},"
               << "\"network\":{"
               // RTB-8/P5: bundle provenance — records that the run's toggle set
               // came from the service profile rather than hand-assembly.
               << "\"service_mode\":" << (config_.networkServiceMode() ? "true" : "false") << ','
               << "\"stations\":" << n_stations << ','
               << "\"clock_datum_station\":\"" << jesc(nc.clock_datum_station) << "\","
               << "\"clock_datum_satellite\":\"" << jesc(nc.clock_datum_satellite) << "\","
               << fmt::format("\"epoch_sync_tolerance_s\":{},", nc.epoch_sync_tolerance_s)
               << "\"product_output_path\":\"" << jesc(nc.product_output_path) << "\","
               << "\"product_flush_per_epoch\":" << (nc.product_flush_per_epoch ? "true" : "false") << ','
               << "\"parallel_stations\":" << (nc.parallel_stations ? "true" : "false") << ','
               << "\"parallel_min_stations\":" << nc.parallel_min_stations << ','
               << "\"parallel_max_threads\":" << nc.parallel_max_threads << ','
               << "\"parallel_reduce\":" << (nc.parallel_reduce ? "true" : "false") << ','
               << "\"parallel_seam\":" << (nc.parallel_seam ? "true" : "false") << ','
               << "\"parallel_carry\":" << (nc.parallel_carry ? "true" : "false") << ','
               << "\"n_threads\":" << n_threads << ','
               << "\"trace_stations\":" << (nc.trace_stations ? "true" : "false") << ','
               << "\"trace_ionosphere\":" << (nc.trace_ionosphere ? "true" : "false") << ','
               << "\"trace_estimability\":" << (nc.trace_estimability ? "true" : "false") << ','
               << "\"trace_estimability_budget\":" << nc.trace_estimability_budget << ','
               << "\"trace_estimability_deficit_only\":"
               << (nc.trace_estimability_deficit_only ? "true" : "false") << ','
               << "\"trace_slip_events\":" << (nc.trace_slip_events ? "true" : "false") << ','
               << "\"trace_anchor_info\":" << (nc.trace_anchor_info ? "true" : "false") << ','
               << "\"trace_dd_fractional\":" << (nc.trace_dd_fractional ? "true" : "false") << ','
               // Producer-AR Phase-2 provenance.  `ils_max_dim` rides with the two
               // switches because it changes WHICH DDs the search saw — a grade of
               // network_producer_ar.csv that does not know the cap is comparing
               // different populations across runs.  Emitted ONLY when armed, so an
               // unarmed run's manifest bytes are unchanged (same
               // structural-byte-identity rule as the adm_* block below).
               << ((nc.ambiguity_resolution.enable || nc.ambiguity_resolution.trace)
                       ? fmt::format("\"ambiguity_resolution.enable\":{},"
                                     "\"ambiguity_resolution.trace\":{},"
                                     "\"ambiguity_resolution.ils_max_dim\":{},",
                                     nc.ambiguity_resolution.enable ? "true" : "false",
                                     nc.ambiguity_resolution.trace ? "true" : "false",
                                     nc.ambiguity_resolution.ils_max_dim)
                       : std::string())
               << "\"trace_flag_rates\":" << (nc.trace_flag_rates ? "true" : "false") << ','
               << "\"trace_epoch_status\":" << (nc.trace_epoch_status ? "true" : "false") << ','
               << "\"trace_events\":" << (nc.trace_events ? "true" : "false") << ','
               << "\"trace_zwd\":" << (nc.trace_zwd ? "true" : "false") << ','
               << "\"trace_sat_bias_states\":" << (nc.trace_sat_bias_states ? "true" : "false") << ','
               << "\"trace_rx_states\":" << (nc.trace_rx_states ? "true" : "false") << ','
               << "\"trace_clock_full\":" << (nc.trace_clock_full ? "true" : "false") << ','
               << "\"trace_station_residuals\":" << (nc.trace_station_residuals ? "true" : "false") << ','
               << "\"trace_raim_redundancy\":" << (nc.trace_raim_redundancy ? "true" : "false") << ','
               // SPEC_event_harness §24.6 per-row post-fit dump: emitted ONLY when
               // armed, so an unarmed run's manifest bytes are unchanged (the adm_*
               // precedent below).
               << (nc.trace_postfit_rows ? std::string("\"trace_postfit_rows\":true,")
                                         : std::string())
               // SPEC_event_harness §24.10 card ZP station-block audit: same
               // armed-only rule, so an unarmed run's manifest bytes are unchanged.
               << (nc.trace_station_block ? std::string("\"trace_station_block\":true,")
                                          : std::string())
               // Card RF-PD station-PD null-direction read: same armed-only rule, so
               // an unarmed run's manifest bytes are unchanged.
               << (nc.trace_station_pd_null
                       ? std::string("\"trace_station_pd_null\":true,") : std::string())
               // Card CST per-satellite consumer readout: same armed-only rule, so an
               // unarmed run's manifest bytes are unchanged.
               << (nc.trace_consumer_sat
                       ? std::string("\"trace_consumer_sat\":true,") : std::string())
               // Card JT: the de-commoned phase jump test changes the R records on the
               // wire, so an archived logs/ folder must be able to say which declaration
               // test minted the product's discontinuity counters.  Emitted ONLY when
               // armed, so an unarmed run's manifest bytes are unchanged.
               << (nc.disc_jump_test_decommon
                       ? std::string("\"disc_jump_test_decommon\":true,") : std::string())
               // Card RF-PD-fix: SOLVE-AFFECTING (it decides which stations are in the
               // network), so an archived logs/ folder has to be able to say which side
               // of it produced the products.  Emitted ONLY when armed (the
               // trace_station_block / young-inflate rule), so an unarmed run's manifest
               // bytes are unchanged.
               << (nc.station_prior_before_pd
                       ? std::string("\"station_prior_before_pd\":true,") : std::string())
               // RF-0b / RF-0c / RF-0d: the satellite code-bias DATUM is the product's
               // frame, so it belongs in the provenance record — the "zero" and
               // "broadcast_tgd" frames differ by c*T_GD on every emitted code bias, and
               // the no-ionosphere-constraint posture means the emitted ionosphere and
               // the code biases are only meaningful AS A PAIR.  Emitted ONLY when the
               // posture is non-default (the trace_station_block precedent above), so an
               // unarmed run's manifest bytes are unchanged.
               << ((nc.w6_iono_constraint != NetworkW6IonoConstraint::Auto
                    || nc.sat_gf_datum != NetworkSatGfDatum::NoneMode)
                       ? fmt::format("\"w6_iono_constraint\":\"{}\","
                                     "\"sat_gf_datum\":\"{}\",",
                                     nc.w6_iono_constraint == NetworkW6IonoConstraint::None
                                         ? "none" : "auto",
                                     nc.sat_gf_datum == NetworkSatGfDatum::Zero ? "zero"
                                     : nc.sat_gf_datum == NetworkSatGfDatum::BroadcastTgd
                                         ? "broadcast_tgd" : "none")
                       : std::string())
               // B5D: WHICH extra-band code columns the product asserted and which it
               // estimated is part of the frame the two fields above declare — a freed
               // band-5 column stands in its channel's sum-to-zero frame while a pinned
               // one stands in the broadcast one, and the B records look identical.
               // Emitted ONLY when armed (the sat_gf_datum precedent above), so an
               // unarmed run's manifest bytes are unchanged.
               << (nc.sat_gf_extra_band_free
                       ? std::string("\"sat_gf_extra_band_free\":true,") : std::string())
               // Card L5P: both knobs are SOLVE-AFFECTING when armed — the pair
               // scope changes which arcs refloat, the sigma scale changes the
               // normal equations — so an archived logs/ folder has to be able to
               // say which side of them produced the products.  Emitted ONLY when
               // armed (the sat_gf_extra_band_free precedent above), so an unarmed
               // run's manifest bytes are unchanged.
               << ((nc.cycle_slip_mw_detect_pairs != NetworkMwDetectPairs::All
                    || nc.extra_band_code_sigma_scale > 1.0)
                       ? fmt::format("\"mw_detect_pairs\":\"{}\","
                                     "\"extra_band_code_sigma_scale\":{},",
                                     nc.cycle_slip_mw_detect_pairs
                                         == NetworkMwDetectPairs::Primary
                                             ? "primary" : "all",
                                     nc.extra_band_code_sigma_scale)
                       : std::string())
               // RF-1 (docs/refound/RF-1_float_ambiguities_design.md §3.5 / §7.1): the
               // AMBIGUITY VEHICLE is the product's provenance, so the manifest records
               // it — and §3.5's whole reason for the q knob existing is that its 0.0
               // default be EXPLICIT here rather than implied.  Emitted ONLY when the
               // posture is non-default (the trace_postfit_rows / trace_station_block
               // precedent above), so an unarmed run's manifest bytes are unchanged.
               << ((nc.ambiguity_vehicle != NetworkAmbiguityVehicle::Level
                    || nc.trace_amb_state || nc.amb_float_birth_var_m2 > 0.0
                    || nc.amb_float_q_m2s > 0.0 || nc.amb_float_declared_handover)
                       ? fmt::format("\"ambiguity_vehicle\":\"{}\","
                                     "\"amb_float_birth_var_m2\":{},"
                                     "\"amb_float_q_m2s\":{},"
                                     "\"amb_float_declared_handover\":{},"
                                     "\"trace_amb_state\":{},",
                                     nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                                         ? "float_kf" : "level",
                                     nc.amb_float_birth_var_m2, nc.amb_float_q_m2s,
                                     nc.amb_float_declared_handover ? "true" : "false",
                                     nc.trace_amb_state ? "true" : "false")
                       : std::string())
               // docs/SPEC_measurement_admission.md: the arm provenance a §7 grader
               // needs to tell a screened leg from its control without re-reading the
               // config — and, because `adm_screen` is solve-affecting, to know that
               // it is NOT comparing like with like.  Emitted ONLY when armed, so an
               // unarmed run's manifest bytes are unchanged (the same
               // structural-byte-identity rule the epoch-status columns follow; the
               // decomposition golden hashes this file).
               << ((nc.adm_screen || nc.adm_trace)
                       ? fmt::format("\"adm_screen\":{},\"adm_trace\":{},"
                                     "\"adm_bound_m\":{},\"adm_min_sats\":{},"
                                     "\"adm_heal_ep\":{},\"adm_max_frac\":{},",
                                     nc.adm_screen ? "true" : "false",
                                     nc.adm_trace ? "true" : "false",
                                     nc.adm_bound_m, nc.adm_min_sats,
                                     nc.adm_heal_ep, nc.adm_max_frac)
                       : std::string())
               // INC-4 re-entry vet, gated on its OWN arming rather than on the
               // block's: an unarmed-vet run's manifest keeps the exact six fields it
               // had before this increment, so the card's own §9.7 conditional-column
               // contract still holds one level down.
               << (nc.adm_revet
                       ? fmt::format("\"adm_revet\":true,\"adm_revet_gap_ep\":{},"
                                     "\"adm_revet_ep\":{},"
                                     "\"adm_revet_bound_scale\":{},"
                                     "\"adm_revet_census\":{},"
                                     "\"adm_revet_hold\":{},",
                                     nc.adm_revet_gap_ep, nc.adm_revet_ep,
                                     nc.adm_revet_bound_scale,
                                     nc.adm_revet_census ? "true" : "false",
                                     nc.adm_revet_hold ? "true" : "false")
                       : std::string())
               // docs/SPEC_redundancy_census.md: the arm provenance a §11.6 grader
               // needs to tell a reliability leg from its control without re-reading
               // the config.  Emitted ONLY when armed, so an unarmed run's manifest
               // bytes are unchanged (the adm_screen precedent; the decomposition
               // golden hashes this file).
               // EVENT HARNESS: emitted ONLY when armed, so an unarmed run's
               // manifest bytes are unchanged (the mdb / adm_screen precedent —
               // the decomposition golden hashes this file).
               << (nc.inject.enable
                       ? fmt::format("\"inject\":true,\"inject_events\":{},"
                                     "\"inject_kinds\":\"{}\",",
                                     nc.inject.events.size(),
                                     [&] {
                                         std::string k;
                                         for (const auto& e : nc.inject.events)
                                             k += (k.empty() ? "" : "|") + e.kind;
                                         return k;
                                     }())
                       : std::string())
               << (nc.mdb
                       ? fmt::format("\"mdb\":true,\"mdb_event_m\":{},"
                                     "\"mdb_event_holdoff_ep\":{},",
                                     nc.mdb_event_m, nc.mdb_event_holdoff_ep)
                       : std::string())
               // §Z band-scoped responses: BOTH are solve-affecting, so the arm
               // provenance belongs in the manifest — a grader must be able to see
               // that an arm is NOT comparing like with like without re-reading the
               // config.  Emitted ONLY when armed (the mdb / adm_screen precedent —
               // the decomposition golden hashes this file).
               << (nc.raim_erase_band_scope == NetworkRaimEraseBandScope::MinKeepOne
                       ? std::string("\"raim_erase_band_scope\":\"min_keep_one\",")
                       : std::string())
               << (nc.cycle_slip_refloat_band_scope == NetworkRefloatBandScope::Implicated
                       ? std::string("\"cycle_slip_refloat_band_scope\":\"implicated\",")
                       : std::string())
               // §12.7 anchor-slip handover: solve-affecting when armed (it moves
               // which station realizes a family's gauge), so the arm provenance
               // belongs in the manifest — armed-only, the band-scope precedent.
               << (nc.cycle_slip_anchor_slip_handover
                       ? std::string("\"cycle_slip_anchor_slip_handover\":true,")
                       : std::string())
               // MW evidence refloat: solve-affecting when armed (it widens which
               // detections free an arc), so the arm provenance and BOTH gate
               // values belong in the manifest — armed-only, the same precedent.
               << (nc.cycle_slip_mw_evidence_refloat
                       ? fmt::format("\"cycle_slip_mw_evidence_refloat\":true,"
                                     "\"cycle_slip_mw_evidence_ratio_min\":{},"
                                     "\"cycle_slip_mw_evidence_min_arc_epochs\":{},",
                                     nc.cycle_slip_mw_evidence_ratio_min,
                                     nc.cycle_slip_mw_evidence_min_arc_epochs)
                       : std::string())
               // RTB-6/P1: service-observability knobs (health heartbeat + trace rotation).
               << fmt::format("\"health_interval_s\":{},", nc.health_interval_s)
               << fmt::format("\"trace_rotate_mb\":{},", nc.trace_rotate_mb)
               << "\"trace_rotate_keep\":" << nc.trace_rotate_keep << ','
               // FC-1: split-cadence fast clock.  The manifest is how a grader tells a
               // SPLIT arm from a full-rate REF arm without re-reading the config.
               << "\"fast_clock_enable\":" << (nc.fast_clock.enable ? "true" : "false") << ','
               << "\"fast_clock_shadow\":" << (nc.fast_clock.shadow ? "true" : "false") << ','
               << "\"fast_clock_emit\":" << (nc.fast_clock.emit ? "true" : "false") << ','
               << "\"fast_clock_min_stations_per_sat\":" << nc.fast_clock.min_stations_per_sat << ','
               << "\"fast_clock_min_sats_per_station\":" << nc.fast_clock.min_sats_per_station << ','
               << fmt::format("\"fast_clock_screen_k_sigma\":{},", nc.fast_clock.screen_k_sigma)
               << "\"fast_clock_allow_single_band\":" << (nc.fast_clock.allow_single_band ? "true" : "false") << ','
               << fmt::format("\"fast_clock_emit_max_gap_s\":{},", nc.fast_clock.emit_max_gap_s)
               << "\"fast_clock_trace\":" << (nc.fast_clock.trace ? "true" : "false") << ','
               // RTB-7: startup quality ramp + state snapshot / warm resume.
               << fmt::format("\"warmup_ura_floor_m\":{},", nc.warmup_ura_floor_m)
               << "\"warmup_epochs\":" << nc.warmup_epochs << ','
               << "\"state_snapshot_path\":\"" << jesc(nc.state_snapshot_path) << "\","
               << fmt::format("\"state_snapshot_interval_s\":{},", nc.state_snapshot_interval_s)
               // RTB-7(c/d/e): the branch-faithful resume knobs.  Which SHAPE of carry blob
               // a run wrote (dense vs diagonal-only) is the one thing that distinguishes
               // the arms of the RTB-7(e) A/B, and the manifest is where a later forensic
               // pass looks for it — the products themselves are identical by construction.
               << "\"state_snapshot_carry\":" << (nc.state_snapshot_carry ? "true" : "false") << ','
               << "\"state_snapshot_carry_diagonal\":"
                   << (nc.state_snapshot_carry_diagonal ? "true" : "false") << ','
               // RS-2: the linearization-frame halves.  Same reason as the diagonal knob —
               // the write half changes only snapshot bytes and the read half only a
               // resume, so the manifest is the one place an arm is self-describing.
               // Emitted ONLY when armed, so an unarmed run's manifest bytes are unchanged
               // (the inject / adm_screen precedent — the decomposition golden hashes this
               // file).
               << ((nc.state_snapshot_carry_lin || config_.networkResumeCarryRebase())
                       ? fmt::format("\"state_snapshot_carry_lin\":{},"
                                     "\"resume_carry_rebase\":{},",
                                     nc.state_snapshot_carry_lin ? "true" : "false",
                                     config_.networkResumeCarryRebase() ? "true" : "false")
                       : std::string())
               // RS-3 (SPEC_event_harness §21.7): the datum-continuity halves.  Same
               // armed-only discipline as the RS-2 pair directly above — the write half
               // changes only snapshot bytes and the read half only a resume, so an
               // unarmed run's manifest bytes are unchanged (the decomposition golden
               // hashes this file) while an armed arm is self-describing.
               << ((nc.state_snapshot_datum_capture || config_.networkResumeDatumRestore())
                       ? fmt::format("\"state_snapshot_datum_capture\":{},"
                                     "\"resume_datum_restore\":{},",
                                     nc.state_snapshot_datum_capture ? "true" : "false",
                                     config_.networkResumeDatumRestore() ? "true" : "false")
                       : std::string())
               // RS-5 (SPEC_event_harness §21.10): the emit-layer continuity halves, on
               // the same armed-only discipline as the RS-2 / RS-3 pairs above.
               << ((nc.state_snapshot_emit_capture || config_.networkResumeEmitRestore())
                       ? fmt::format("\"state_snapshot_emit_capture\":{},"
                                     "\"resume_emit_restore\":{},",
                                     nc.state_snapshot_emit_capture ? "true" : "false",
                                     config_.networkResumeEmitRestore() ? "true" : "false")
                       : std::string())
               // RS-6 (SPEC_event_harness §21.13): the station-layer continuity halves
               // plus the two diagnostic sub-switches (an attribution leg must be
               // self-describing), on the same armed-only discipline as the pairs above.
               << ((nc.state_snapshot_station_capture
                    || config_.networkResumeStationRestore())
                       ? fmt::format("\"state_snapshot_station_capture\":{},"
                                     "\"resume_station_restore\":{},"
                                     "\"resume_station_restore_rxcb\":{},"
                                     "\"resume_station_restore_shdev\":{},",
                                     nc.state_snapshot_station_capture ? "true" : "false",
                                     config_.networkResumeStationRestore() ? "true" : "false",
                                     config_.networkResumeStationRestoreRxCb() ? "true" : "false",
                                     config_.networkResumeStationRestoreShDev() ? "true" : "false")
                       : std::string())
               // RS-8 (SPEC_event_harness §21.16): the canonical-signal latch halves, on
               // the same armed-only discipline as the pairs above.
               << ((nc.state_snapshot_signal_capture
                    || config_.networkResumeSignalCanonRestore())
                       ? fmt::format("\"state_snapshot_signal_capture\":{},"
                                     "\"resume_signal_canon_restore\":{},",
                                     nc.state_snapshot_signal_capture ? "true" : "false",
                                     config_.networkResumeSignalCanonRestore() ? "true" : "false")
                       : std::string())
               // RS-9 (SPEC_event_harness §21.18): the code-bias linearization-point
               // halves — the reference the rx-CB inject gate judges the RS-6-restored
               // carry against — on the same armed-only discipline as the pairs above.
               << ((nc.state_snapshot_rxcb_lin_capture
                    || config_.networkResumeRxCbLinRestore())
                       ? fmt::format("\"state_snapshot_rxcb_lin_capture\":{},"
                                     "\"resume_rxcb_lin_restore\":{},",
                                     nc.state_snapshot_rxcb_lin_capture ? "true" : "false",
                                     config_.networkResumeRxCbLinRestore() ? "true" : "false")
                       : std::string())
               << "\"state_snapshot_require\":" << (nc.state_snapshot_require ? "true" : "false") << ','
               << fmt::format("\"state_snapshot_resume_shock_m\":{},",
                              nc.state_snapshot_resume_shock_m)
               << "\"serialize_sbasis_contract\":" << (nc.serialize_sbasis_contract ? "true" : "false") << ','
               << "\"estimate_sat_code_bias\":" << (config_.estimateNetworkSatCodeBias() ? "true" : "false") << ','
               << "\"estimate_iono_sh\":" << (config_.estimateNetworkIonoSh() ? "true" : "false") << ','
               << fmt::format("\"iono_constraint_deweight_factor\":{},",
                              config_.getIonoConstraintDeweightConfig().factor)
               << "\"iono_constraint_deweight_adaptive\":"
                   << (config_.getIonoConstraintDeweightConfig().adaptive ? "true" : "false") << ','
               << "\"iono_constraint_deweight_stations\":["
                   << [&] {
                          std::string s; bool f = true;
                          for (const auto& nm : config_.getIonoConstraintDeweightConfig().stations) {
                              if (!f) s += ','; s += '"'; s += jesc(nm); s += '"'; f = false;
                          }
                          return s;
                      }()
                   << "],"
               << "\"phase_leveling\":" << (pl_on ? "true" : "false") << ','
               << fmt::format("\"phase_leveling_q_amb\":{},", pl_q_amb)
               << "\"phase_leveling_warmup_epochs\":" << pl_warmup << ','
               << fmt::format("\"phase_leveling_max_gap_s\":{},", pl_max_gap_s)
               << fmt::format("\"phase_leveling_var_floor_m2\":{},", pl_var_floor)
               << "\"phase_leveling_los_scale\":" << (pl_los_scale ? "true" : "false") << ','
               << "\"phase_leveling_local_block\":" << (pl_local_block ? "true" : "false") << ','
               << fmt::format("\"phase_leveling_local_block_min_age_ep\":{},", config_.networkPhaseLevelingLocalBlockMinAgeEp())
               << "\"phase_leveling_local_block_young_mode\":\"" << config_.networkPhaseLevelingLocalBlockYoungMode() << "\","
               // AGE-AWARE young-arc inflate (SPEC_amb_iono_split_overcount §6.17–§6.18):
               // SOLVE-AFFECTING, so an archived logs/ folder has to be able to say which
               // side of it produced the products, and the two shape knobs ride along
               // because the prior's weight cannot be recovered without them.  Emitted
               // ONLY when armed (the adm_screen / I-in-block rule) so an unarmed run's
               // manifest bytes are unchanged.
               << (config_.networkPhaseLevelingYoungInflateVarM2() > 0.0
                       ? fmt::format("\"phase_leveling_young_inflate_var_m2\":{},"
                                     "\"phase_leveling_young_inflate_tau_ep\":{},"
                                     "\"phase_leveling_young_inflate_max_age_ep\":{},",
                                     config_.networkPhaseLevelingYoungInflateVarM2(),
                                     config_.networkPhaseLevelingYoungInflateTauEp(),
                                     config_.networkPhaseLevelingYoungInflateMaxAgeEp())
                       : std::string{})
               << fmt::format("\"shared_carry_var_floor_pb_m2\":{},", pb_var_floor)
               << fmt::format("\"shared_carry_var_floor_cb_m2\":{},", cb_var_floor)
               // M2 (LS-VCE) monitor + v2 apply.  The apply knobs are in the manifest
               // because they are SOLVE-AFFECTING and fingerprinted: an archived logs/
               // folder has to be able to say which α² regime produced its products.
               << "\"carry_vce\":" << (config_.networkCarryVce() ? "true" : "false") << ','
               << "\"carry_vce_apply\":" << (config_.networkCarryVceApply() ? "true" : "false") << ','
               << "\"carry_vce_apply_families\":\"" << config_.networkCarryVceApplyFamilies() << "\","
               << fmt::format("\"carry_vce_apply_slew\":{},", config_.networkCarryVceApplySlew())
               << fmt::format("\"carry_vce_apply_max\":{},", config_.networkCarryVceApplyMax())
               << "\"carry_vce_apply_min_rows\":" << config_.networkCarryVceApplyMinRows() << ','
               << "\"carry_zwd\":" << (zwd_on ? "true" : "false") << ','
               // B9-in-block: emitted ONLY when armed, so an unarmed run's manifest bytes
               // are unchanged (the adm_screen rule above — the decomposition golden hashes
               // this file).  A grader reading an archived logs/ folder needs to know which
               // VEHICLE the carried ZWD folded through, because it changes the solve.
               << (config_.networkCarryZwdInBlock() ? "\"carry_zwd_in_block\":true," : "")
               // I-in-block: armed-only, same rule (an unarmed run's manifest bytes are
               // unchanged).  q and the gap ride along because a grader reading an
               // archived logs/ folder cannot recover the prior's weight without them —
               // and the q unit here is m² PER EPOCH at L1, not m²/s like zwd_carry_q.
               << (config_.networkCarryIonoInBlock()
                       ? fmt::format("\"carry_iono_in_block\":true,"
                                     "\"carry_iono_in_block_q_m2\":{},"
                                     "\"carry_iono_in_block_max_gap_s\":{},",
                                     config_.networkCarryIonoInBlockQM2(),
                                     config_.networkCarryIonoInBlockMaxGapS())
                       : std::string{})
               // RX-CLK-in-block: armed-only, same rule (an unarmed run's manifest bytes
               // are unchanged).  The whole variance ladder rides along because a grader
               // reading an archived logs/ folder cannot recover the prior's weight
               // without it — and because the DRIFT half is what makes this carry
               // defensible at all, so a run that does not record it cannot be re-read.
               << (config_.networkCarryRxClockInBlock()
                       ? fmt::format("\"carry_rx_clock_in_block\":true,"
                                     "\"carry_rx_clock_q_m2\":{},"
                                     "\"carry_rx_clock_max_gap_s\":{},"
                                     "\"carry_rx_clock_rate_var0_m2ps2\":{},"
                                     "\"carry_rx_clock_rate_q_m2ps2\":{},"
                                     "\"carry_rx_clock_jump_k\":{},"
                                     "\"carry_rx_clock_isb\":{},",
                                     config_.networkCarryRxClockQM2(),
                                     config_.networkCarryRxClockMaxGapS(),
                                     config_.networkCarryRxClockRateVar0(),
                                     config_.networkCarryRxClockRateQ(),
                                     config_.networkCarryRxClockJumpK(),
                                     config_.networkCarryRxClockIsb() ? "true" : "false")
                       : std::string{})
               << fmt::format("\"zwd_carry_q\":{},", zwd_q)
               // Seam-scoped ZWD re-seed (§17 finding 3).  In the manifest so an
               // archived logs/ folder can say whether a resumed leg's first hours
               // rode the seam window; the capture flag records whether this run's
               // snapshots carry Z rows.
               << "\"zwd_seam_reseed\":" << (config_.networkZwdSeamReseed() ? "true" : "false") << ','
               << "\"zwd_seam_reseed_epochs\":" << config_.networkZwdSeamReseedEpochs() << ','
               << "\"zwd_snapshot_capture\":" << (nc.state_snapshot_path.empty() ? "false" : "true") << ','
               // Resume FRAME restore (§18.10): whether a resumed leg continued the
               // pre-seam realization frame, and the post-resume MW/LLI warm-up.
               << "\"state_snapshot_frame_capture\":" << (nc.state_snapshot_frame_capture ? "true" : "false") << ','
               << "\"resume_frame_restore\":" << (config_.networkResumeFrameRestore() ? "true" : "false") << ','
               << "\"resume_slip_warmup_ep\":" << config_.networkResumeSlipWarmupEp() << ','
               // RS-2b: emitted ONLY when armed, so an unarmed run's manifest bytes are
               // unchanged (the inject / adm_screen precedent — the decomposition golden
               // hashes this file).
               << (config_.networkResumeD5WarmupEp() > 0
                       ? fmt::format("\"resume_d5_warmup_ep\":{},",
                                     config_.networkResumeD5WarmupEp())
                       : std::string())
               // Card G RESPONSE (docs/SPEC_slip_qc_hardening.md §G3/W1).  It is
               // SOLVE-AFFECTING, so an archived logs/ folder has to be able to say
               // which side of the response its products came from.  Emitted ONLY
               // when armed — the adm_screen / mdb precedent — so an unarmed run's
               // manifest bytes are unchanged (the decomposition golden hashes this
               // file).  Both sub-gates travel with the master: `_lli` defaults ON,
               // so its value is not inferable from the master alone.
               << (nc.slip_qc.gap_reentry
                       ? fmt::format("\"gap_reentry_relevel\":true,"
                                     "\"gap_reentry_relevel_lli\":{},"
                                     "\"gap_reentry_relevel_unmeasurable\":{},",
                                     nc.slip_qc.gap_reentry_lli ? "true" : "false",
                                     nc.slip_qc.gap_reentry_unmeasurable ? "true" : "false")
                       : std::string())
               // SIM-ONLY ORACLE slip response (include/slip/NetworkSlipOracle.hpp).
               // SOLVE-AFFECTING and truth-fed, so an archived logs/ folder MUST be
               // able to say that its products are a diagnostic ceiling rather than a
               // real posture — and which fixture fed them.  Emitted ONLY when armed
               // (the gap_reentry precedent), so an unarmed run's manifest bytes are
               // unchanged.
               << (nc.slip_qc.oracle_relevel
                       ? fmt::format("\"oracle_relevel\":true,"
                                     "\"oracle_truth_root\":\"{}\","
                                     "\"oracle_noise_cyc\":{},"
                                     "\"oracle_false_alarm\":\"{}\","
                                     "\"oracle_true_slip\":\"{}\","
                                     "\"oracle_window_s\":{},",
                                     jesc(nc.slip_qc.oracle_truth_root),
                                     nc.slip_qc.oracle_noise_cyc,
                                     nc.slip_qc.oracle_false_alarm,
                                     nc.slip_qc.oracle_true_slip,
                                     nc.slip_qc.oracle_window_s)
                       : std::string())
               // §18.12 Card-1 coupling regressors, both halves of the frame restore:
               // the WRITE knob says whether this run's snapshots carry QS/QL/QZ/QR
               // records (format v3), the READ knob whether a resumed leg rebuilt
               // LocalCarry::coupling from them.  STARTUP record only — an archived
               // logs/ folder must be able to say which half this leg ran, and the
               // run-end log line is this group's invariant-#4 counter instrument.
               << "\"state_snapshot_coupling_capture\":" << (nc.state_snapshot_coupling_capture ? "true" : "false") << ','
               << "\"resume_coupling_restore\":" << (config_.networkResumeCouplingRestore() ? "true" : "false") << ','
               // READMIT continuity (E2 seed-first re-admission + the block re-coupling
               // edge) and the honest-sigma-scale handover ceiling — startup record; the
               // run-end 'READMIT continuity' / 'ratio ceiling' lines are the counters.
               << fmt::format("\"readmit_seed_horizon_s\":{},", config_.networkReadmitSeedHorizonS())
               << fmt::format("\"readmit_coupling_marg_ep\":{},", config_.networkReadmitCouplingMargEp())
               << "\"readmit_seed_code_bias\":" << (config_.networkReadmitSeedCodeBias() ? "true" : "false") << ','
               << fmt::format("\"phase_datum_handover_max_var_ratio\":{},", config_.networkPhaseDatumHandoverMaxVarRatio())
               << "\"raim\":" << (raim_on ? "true" : "false") << ','
               << fmt::format("\"raim_k_sigma\":{},", raim_k)
               << "\"raim_min_stations\":" << raim_min_sta << ','
               << fmt::format("\"raim_sigma0_trigger\":{},", es_sigma0_trigger)
               << fmt::format("\"raim_withhold_sigma0\":{},", raim_withhold)
               << "\"per_const_withhold\":" << (perconst_on ? "true" : "false") << ','
               << "\"per_const_withhold_min_rows\":" << perconst_min_rows << ','
               << "\"raim_redundancy_normalize\":" << (raim_redun ? "true" : "false") << ','
               << "\"raim_attribution\":" << (config_.networkRaimAttribution() ? "true" : "false") << ','
               << "\"raim_within_epoch\":" << (config_.networkRaimWithinEpoch() ? "true" : "false") << ','
               << "\"clock_monitor\":" << (clk_mon_on ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_abs_max_m\":{},", clk_mon_abs)
               << fmt::format("\"clock_monitor_jump_max_m\":{},", clk_mon_jump)
               << "\"clock_monitor_couple_raim\":" << (config_.networkClockMonitorCoupleRaim() ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_reinit_sigma_m\":{},", clk_mon_reinit_sig)
               << "\"clock_model\":" << (clk_model_on ? "true" : "false") << ','
               << fmt::format("\"clock_model_q1\":{},", clk_model_q1)
               << fmt::format("\"clock_model_q2\":{},", clk_model_q2)
               << fmt::format("\"clock_model_k_sigma\":{},", clk_model_k)
               << fmt::format("\"clock_model_meas_std_m\":{},", config_.networkClockModelMeasStdM())
               << fmt::format("\"clock_model_max_gap_s\":{},", clk_model_max_gap)
               << "\"emit_clock_rate\":" << (config_.networkEmitClockRate() ? "true" : "false") << ','
               << "\"emit_ura\":" << (ura_on ? "true" : "false") << ','
               << fmt::format("\"ura_ewma_alpha\":{},", ura_alpha)
               << "\"consume_ura\":" << (config_.networkConsumeUra() ? "true" : "false") << ','
               << "\"ura_retro_bump\":" << (ura_retro ? "true" : "false") << ','
               << fmt::format("\"ura_refloat_inflate\":{},", ura_refloat_inflate)
               << fmt::format("\"ura_staleness_q2\":{},", config_.networkUraStalenessQ2())
               << "\"pivot_vanish_return_signal\":" << (pivot_vr_signal ? "true" : "false") << ','
               << "\"emit_clock_disc\":" << (emit_clock_disc ? "true" : "false") << ','
               << "\"sticky_pivot\":" << (sticky_pivot ? "true" : "false") << ','
               << "\"carry_invariant\":" << (config_.networkCarryInvariant() ? "true" : "false") << ','
               << "\"carry_invariant_pd_safe\":"
               << (config_.networkCarryInvariantPdSafe() ? "true" : "false") << ','
               << "\"carry_aware_covariance\":"
               << (config_.networkCarryAwareCovariance() ? "true" : "false") << ','
               << fmt::format("\"carry_invariant_qz\":{},", config_.networkCarryInvariantQz())
               << "\"carry_invariant_emit_rebase\":"
               << (config_.networkCarryInvariantEmitRebase() ? "true" : "false") << ','
               << "\"carry_invariant_honesty\":"
               << (config_.networkCarryInvariantHonesty() ? "true" : "false") << ','
               << "\"carry_invariant_innov_gate\":"
               << (config_.networkCarryInvariantInnovGate() ? "true" : "false") << ','
               << fmt::format("\"carry_invariant_innov_k\":{},", config_.networkCarryInvariantInnovK())
               << "\"carry_invariant_innov_calibrate\":"
               << (config_.networkCarryInvariantInnovCalibrate() ? "true" : "false") << ','
               << "\"mw_wl_enable\":" << (config_.networkMwWlSeed() ? "true" : "false") << ','   // M5 card WL-1
               << fmt::format("\"mw_wl_min_arc_epochs\":{},", config_.getNetworkConfig().mw_wl.min_arc_epochs)
               << fmt::format("\"mw_wl_sigma_floor_cyc\":{},", config_.getNetworkConfig().mw_wl.sigma_floor_cyc)
               << fmt::format("\"mw_wl_max_rows_per_epoch\":{},", config_.getNetworkConfig().mw_wl.max_rows_per_epoch)
               << "\"emit_pb_filter\":" << (config_.networkEmitPbFilter() ? "true" : "false") << ','
               << fmt::format("\"emit_pb_filter_q\":{},", config_.networkEmitPbFilterQ())
               << fmt::format("\"emit_pb_filter_jump_k\":{},", config_.networkEmitPbFilterJumpK())
               << fmt::format("\"emit_pb_filter_meas_sigma_m\":{},", config_.networkEmitPbFilterMeasSigmaM())
               << fmt::format("\"emit_pb_filter_max_gap_s\":{},", config_.networkEmitPbFilterMaxGapS())
               << fmt::format("\"emit_pb_filter_window_epochs\":{},", config_.networkEmitPbFilterWindowEpochs())
               << "\"emit_pb_filter_emit_sigma\":"
               << (config_.networkEmitPbFilterEmitSigma() ? "true" : "false") << ','
               << "\"emit_pb_filter_gate_v2\":"   // PB-gate-v2 merge point (config echo)
               << (config_.networkEmitPbFilterGateV2() ? "true" : "false") << ','
               << "\"clock_signal_basis_hold\":" << (clock_signal_basis_hold ? "true" : "false") << ','
               << "\"scoped_handover_response\":" << (scoped_handover ? "true" : "false") << ','
               << "\"phase_bias_exclude_unmatched_antenna\":" << (pb_excl_unmatched ? "true" : "false") << ','
               << "\"exclude_unmodeled_antenna_stations\":"
               << (config_.networkExcludeUnmodeledAntennaStations() ? "true" : "false") << ','
               << "\"emit_generation_id\":" << (emit_gen_id ? "true" : "false") << ','
               << "\"serialize_disc_counters\":" << (ser_disc_ctrs ? "true" : "false") << ','
               << "\"emit_epoch_status_record\":" << (emit_epoch_status ? "true" : "false") << ','
               << fmt::format("\"contract_rebroadcast_s\":{},", contract_rebroadcast_s)
               << fmt::format("\"emitter_staleness_horizon_s\":{},",
                              config_.networkEmitterStalenessHorizonS())
               << fmt::format("\"start_offset_hours\":{},", config_.networkStartOffsetHours())
               << fmt::format("\"process_noise_sat_clock\":{},", q_clock)
               << fmt::format("\"process_noise_sat_phase_bias\":{},", q_pb)
               << fmt::format("\"process_noise_sat_code_bias\":{},", q_cb)
               << "\"process_noise\":{";
            // L15: the full scoped table (network.process_noise), one object per
            // parameter class, with the RESOLVED rate and the precedence rung that
            // supplied it.  The three flat fields above are kept verbatim so the
            // existing manifest readers (scripts/analyze_net_sh.py, pyviz) keep
            // parsing; this object is the auditable record.  Provenance is the
            // point: a class reading "default" when the operator believed they had
            // set it is exactly the inert-wiring failure that run-end telemetry
            // exists to catch — and it is invisible from the rate alone, because a
            // typo'd override and a documented default look identical in the value.
            for (std::size_t i = 0; i < kNetworkQClassCount; ++i) {
                const NetworkQDesc& d = kNetworkQClassTable[i];
                const auto c = static_cast<NetworkQClass>(i);
                mf << fmt::format(
                    "{}\"{}\":{{\"q\":{},\"source\":\"{}\",\"units\":\"{}\"}}",
                    i ? "," : "", d.name, nc.process_noise.get(c),
                    NetworkProcessNoise::sourceLabel(nc.process_noise.source(c)), d.units);
            }
            mf << "}"
               << "}}\n";
            mf.close();
        } else {
            log->warn("Network: failed to open run-manifest {}", manifest_path.string());
        }
    }
}

void NetworkRunTraces::writeRunManifestComplete(const NetworkConfig& nc,
                                                const NetworkServiceCounters& counters,
                                                std::size_t n_synced, double loop_ms) {
    const auto& n_emitted                    = counters.n_emitted;
    const auto& n_skipped                    = counters.n_skipped;
    const auto& n_gate_withheld              = counters.n_gate_withheld;
    const auto& n_sigma0_withheld            = counters.n_sigma0_withheld;
    const auto& n_perconst_partial_epochs    = counters.n_perconst_partial_epochs;
    const auto& n_perconst_diffuse_published = counters.n_perconst_diffuse_published;
    // L15: append the end-of-run manifest record (JSONL — a second object on its own line).
    // Reopen-append (never rewrite) so a crashed run keeps the startup record (itself diagnostic).
    if (nc.write_run_manifest) {
        std::ofstream mf(manifest_path, std::ios::app);
        if (mf)
            mf << "{\"record\":\"complete\","
               << "\"n_synced\":" << n_synced << ','
               << "\"n_emitted\":" << n_emitted << ','
               << "\"n_skipped\":" << n_skipped << ','
               << "\"n_gate_withheld\":" << n_gate_withheld << ','
               << "\"n_sigma0_withheld\":" << n_sigma0_withheld << ','
               << "\"n_perconst_partial_epochs\":" << n_perconst_partial_epochs << ','
               << "\"n_perconst_diffuse_published\":" << n_perconst_diffuse_published << ','
               << fmt::format("\"loop_ms\":{:.1f}", loop_ms)
               << "}\n";
    }
}

void NetworkRunTraces::writeFlagRateAndRunSummary(const GnssConfig& config_,
                                                  const NetworkKnobs& knobs,
                                                  const NetworkServiceCounters& counters,
                                                  std::size_t n_synced, std::size_t n_stations,
                                                  int n_threads, bool parallel_on, double loop_ms,
                                                  const std::shared_ptr<spdlog::logger>& log) {
    const bool  trace_flag_rates  = knobs.trace_flag_rates;
    const auto& fr_if_slip        = counters.fr_if_slip;
    const auto& fr_if_refgate     = counters.fr_if_refgate;
    const auto& fr_we_trigger     = counters.fr_we_trigger;
    const auto& fr_raim_cell      = counters.fr_raim_cell;
    const auto& fr_raim_sat       = counters.fr_raim_sat;
    const auto& fr_attrib         = counters.fr_attrib;
    const auto& fr_attrib_arc     = counters.fr_attrib_arc;
    const auto& fr_clk_model      = counters.fr_clk_model;
    const auto& fr_clk_mon        = counters.fr_clk_mon;
    const auto& n_emitted         = counters.n_emitted;
    const auto& n_skipped         = counters.n_skipped;
    const auto& n_gate_withheld   = counters.n_gate_withheld;
    const auto& n_sigma0_withheld = counters.n_sigma0_withheld;
    const auto& n_loop_epochs     = counters.n_loop_epochs;
    // --- B8: empirical false-alarm-rate summary per QC tier ------------------
    // Realized flag rate vs the implied one-sided Gaussian α for the configured k
    // (magnitude gates are two-sided, so α = P(|Z|>k) = erfc(k/√2)).  Run this on a
    // CLEAN baseline to read each tier's realized false-alarm rate.  Measurement only.
    if (trace_flag_rates) {
        auto gaussAlpha = [](double k) { return std::erfc(k / std::sqrt(2.0)); };  // P(|Z|>k)
        // L3: also stream a machine-readable network_flag_rates.csv alongside the log lines
        // (both sinks gated on the ONE existing trace_flag_rates toggle).  The two log format
        // strings below are byte-identical to before — the CSV row is appended after log->info.
        const std::filesystem::path fr_csv_path = log_dir_ / "network_flag_rates.csv";
        std::ofstream fr_csv(fr_csv_path);
        if (fr_csv)
            fr_csv << "tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha\n";
        else
            log->warn("Network: failed to open flag-rate CSV {}", fr_csv_path.string());
        auto emit = [&](const char* tier, const char* label, const FlagRate& fr, double k,
                        bool has_alpha) {
            const double rate = fr.opp ? static_cast<double>(fr.flag) /
                                         static_cast<double>(fr.opp) : 0.0;
            if (has_alpha)
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (k={:.1f} => implied "
                          "Gaussian a {:.3e})", label, fr.opp, fr.flag, rate, k, gaussAlpha(k));
            else
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (absolute bound; no "
                          "Gaussian a)", label, fr.opp, fr.flag, rate);
            if (fr_csv) {
                fr_csv << tier << ',' << label << ',' << fr.opp << ',' << fr.flag << ','
                       << fmt::format("{:.6e}", rate) << ',' << fmt::format("{:.1f}", k) << ',';
                if (has_alpha) fr_csv << fmt::format("{:.6e}", gaussAlpha(k));
                fr_csv << ',' << (has_alpha ? 1 : 0) << '\n';
            }
        };
        log->info("==== Network flag-rate empirical-alpha summary (network.trace_flag_rates) ====");
        emit("T1",  "IF-phase slip (T1)",      fr_if_slip,    config_.networkCycleSlipIfPhaseTdKMad(), true);
        // T1r — the IF-TD tier's per-station REFERENCE arm, a SEPARATE row on purpose
        // (§6.14.5 defect 1).  Its opportunity is a station-epoch-constellation with a
        // family of n>=2 non-reference DDs, not a per-(station,sat,pair) sample, and its
        // threshold is a different quantity; folding it into T1 would average two rates
        // over incommensurable denominators and re-hide the asymmetry that made this arm
        // invisible.  An implied Gaussian alpha is only meaningful when the hardened
        // Card-R gate is armed (threshold max(k*s_eff, floor_frac*lambda_IF)); on the
        // legacy path the decision is a bare PHYSICAL floor, an absolute bound with no k.
        {
            const auto& sq = config_.getNetworkConfig().slip_qc;
            emit("T1r", "IF-TD ref gate (T1r)",   fr_if_refgate, sq.ref_k, sq.ref_gate);
        }
        emit("T2",  "within-ep trigger (T2)",  fr_we_trigger, config_.networkRaimSigma0Trigger(),      true);
        emit("T3",  "post-fit RAIM cell (T3)", fr_raim_cell,  config_.networkRaimKSigma(),             true);
        emit("T3c", "RAIM consensus (T3)",     fr_raim_sat,   config_.networkRaimKSigma(),             true);
        emit("T4",  "attribution SAT (T4)",    fr_attrib,     config_.networkRaimKSigma(),             true);
        emit("T4a", "attribution ARC (T4)",    fr_attrib_arc, config_.networkRaimKSigma(),             true);
        emit("T5",  "clock-model NIS (T5)",    fr_clk_model,  config_.networkClockModelKSigma(),       true);
        emit("T6",  "clock-monitor (T6)",      fr_clk_mon,    0.0,                                     false);
        log->info("=============================================================================");
        if (fr_csv) fr_csv.close();

        // L3: run-summary CSV (same toggle).  ms_per_epoch uses n_loop_epochs (loop epochs,
        // NOT n_synced — the fast-forward / max-duration epochs are excluded from the loop).
        const std::filesystem::path rs_csv_path = log_dir_ / "network_run_summary.csv";
        std::ofstream rs_csv(rs_csv_path);
        if (rs_csv) {
            // Card RF-PD-fix: four APPENDED columns (the existing ten keep their order
            // and meaning, so a positional reader of the old schema is unaffected and a
            // header-keyed one picks these up).  `n_stations` is the ROSTER; these say
            // how many of it actually entered the arrowhead, which is the number
            // network_epoch_status.csv's `n_present` is NOT.  -1 = never recorded.
            rs_csv << "n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,loop_ms,"
                      "ms_per_epoch,n_stations,n_threads,parallel,"
                      "n_sta_in_solve_min,n_sta_in_solve_med,n_sta_pd_dropped,n_sta_readmitted\n";
            const double ms_per_epoch = n_loop_epochs
                ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0;
            int in_solve_med = -1;
            if (!counters.sta_in_solve_ep.empty()) {
                std::vector<int> v = counters.sta_in_solve_ep;
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                                 v.end());
                in_solve_med = v[v.size() / 2];
            }
            rs_csv << n_synced << ',' << n_emitted << ',' << n_skipped << ','
                   << n_gate_withheld << ',' << n_sigma0_withheld << ','
                   << fmt::format("{:.1f}", loop_ms) << ',' << fmt::format("{:.3f}", ms_per_epoch) << ','
                   << n_stations << ',' << n_threads << ',' << (parallel_on ? 1 : 0) << ','
                   << counters.sta_in_solve_min << ',' << in_solve_med << ','
                   << counters.pd_drop_total << ',' << counters.sta_readmit_events << '\n';
            rs_csv.close();
        } else {
            log->warn("Network: failed to open run-summary CSV {}", rs_csv_path.string());
        }
    }
}

// INC-4: the exact former teardown close list (the remaining streams rely on
// the destructor, as before - do not widen this list).
void NetworkRunTraces::closeAll() {
    if (trace) trace.close();
    if (soln) soln.close();
    if (iono_trace.is_open()) iono_trace.close();
    if (vtec_sh_trace.is_open()) vtec_sh_trace.close();
    if (cl_trace) cl_trace.close();
    if (cl_sat_trace.is_open()) cl_sat_trace.close();   // card CST
    if (if_slip_trace.is_open()) if_slip_trace.close();
    if (raim_trace.is_open()) raim_trace.close();
    if (raim_attrib_trace.is_open()) raim_attrib_trace.close();
    if (producer_ar_trace.is_open()) producer_ar_trace.close();
    if (epoch_status_trace.is_open()) epoch_status_trace.close();
    if (health_trace.is_open()) health_trace.close();
    // FC-1: closed explicitly (not left to the destructor) because it ROTATES — rotation
    // reads tellp() on a live handle, so the stream's lifetime is already coupled to the
    // rotation set, and an unflushed tail here is the tick tier's only evidence.
    if (fast_clock_trace.is_open()) fast_clock_trace.close();
    if (events_trace.is_open()) events_trace.close();
    if (zwd_trace.is_open()) zwd_trace.close();
    if (station_res_trace.is_open()) station_res_trace.close();
    if (raim_redun_trace.is_open()) raim_redun_trace.close();
    if (station_factor_trace.is_open()) station_factor_trace.close();
    if (level_authority_trace.is_open()) level_authority_trace.close();
    if (carry_vce_trace.is_open()) carry_vce_trace.close();
    if (carry_vce_apply_trace.is_open()) carry_vce_apply_trace.close();
    if (block_baarda_trace.is_open()) block_baarda_trace.close();
    // Same rationale as the ladder above, and one more: this file is the ONLY
    // place the per-station MDB table's inputs exist, so an unflushed tail costs
    // exactly the deliverable the card was armed for.
    if (congruence_trace.is_open()) congruence_trace.close();
    if (estimability_trace.is_open()) estimability_trace.close();   // Card DG5
    // The two MW/WL dumps are NEW members, not entries the INC-4 relocation moved off
    // the destructor, so the "do not widen" rule above does not cover them.  Closed
    // here so their tail is flushed at teardown rather than at scope exit — these are
    // the graded artifacts of a D5/lane arm, and a crash after teardown should not
    // cost the last rows.
    if (d5_flags_trace.is_open()) d5_flags_trace.close();
    if (wl_lane_trace.is_open()) wl_lane_trace.close();
    // Same rationale as the two dumps above: a NEW member, and the MDB
    // distribution is the graded artifact of a reliability arm.
    if (mdb_trace.is_open()) mdb_trace.close();
    // Same rationale: a NEW member, and the contingency is the graded artifact
    // of a datum-graph arm.
    if (datum_graph_trace.is_open()) datum_graph_trace.close();
    // Same rationale: a NEW member, and the per-row residual dump is the graded
    // artifact of a §24.6 attribution leg — its tail must be flushed at teardown.
    if (pf_rows_trace.is_open()) pf_rows_trace.close();
    // Same rationale: a NEW member, and the station-block audit is the graded
    // artifact of a §24.10 card-ZP arm — its tail must be flushed at teardown.
    if (station_block_trace.is_open()) station_block_trace.close();
    // RF-1 §7.4: same rationale — a NEW member, and the carry-state trace is the
    // instrument step 2 exists to produce, so its tail must be flushed at teardown.
    if (amb_state_trace.is_open()) amb_state_trace.close();
}

} // namespace GnssProcessor
