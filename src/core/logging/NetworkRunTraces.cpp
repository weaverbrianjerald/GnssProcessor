/**
 * @file NetworkRunTraces.cpp
 * @brief Trace-stream ownership + startup open logic for the network run loop
 *        (decomposition INC-3 — verbatim relocation from ApplicationNetwork.cpp;
 *        see docs/network_decomposition_addendum_2026-07-16.md).
 */

#include "logging/NetworkRunTraces.hpp"

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkRunTypes.hpp"   // NetStation (roster writer), EpochStatusRow
#include "app/NetworkRunContext.hpp"    // NetworkKnobs / NetworkServiceCounters (INC-4)
#include "logging/CsvRotator.hpp"       // maybeRotateCsv (P1 rotation, INC-4)
#include "filter/NetworkPivotHandover.hpp" // formatPivotMap (health heartbeat, INC-4)
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
      slip_raw_trace_path(log_dir_ / "network_slip_raw.csv"),
      epoch_status_trace_path(log_dir_ / "network_epoch_status.csv"),
      health_trace_path(log_dir_ / "network_service_health.csv"),
      events_trace_path(log_dir_ / "network_events.csv"),
      zwd_trace_path(log_dir_ / "network_zwd.csv"),
      station_res_trace_path(log_dir_ / "network_station_residuals.csv"),
      carry_vce_trace_path(log_dir_ / "network_carry_vce.csv"),
      estimability_trace_path(log_dir_ / "network_estimability.csv"),
      sh_spectrum_trace_path(log_dir_ / "network_sh_spectrum.csv"),
      sh_spectrum_mode_trace_path(log_dir_ / "network_sh_spectrum_mode.csv"),
      pf_keyed_trace_path(log_dir_ / "network_pf_keyed.csv"),
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
            iono_trace << "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg\n";
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
                        "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate\n";  // T2c: +ar_gate
        else
            log->warn("Network: failed to open closed-loop trace {}", cl_path.string());
    }

    if (nc.trace_slip_events) {
        if_slip_trace.open(if_slip_trace_path);
        if (if_slip_trace)
            // dd = between-sat/between-epoch IF ΔΔφ (m); med = network pool median; s_pool
            // = elevation-normalized pooled scale; ewma_scale/count = this station-pair's
            // own EWMA scale + clean-sample count (decision-time, pre-update); w/s_eff =
            // shrinkage weight + effective scale; elev_map = 1/sin el; thr/floor = gate;
            // abs_dev = |dd−med|; flagged/carry_warm = decision + whether it refloated.
            if_slip_trace << "week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,"
                             "s_pool_m,ewma_scale_m,count,w_shrink,s_eff_m,elev_map,"
                             "thr_m,floor_m,abs_dev_m,flagged,carry_warm\n";
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
            dd_frac_trace << "week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,"
                             "dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc\n";  // T2a: +dd_sigma_cyc
            // (B5 wavelength provider dd_ppp_utils is constructed by the CALLER,
            //  gated on this stream being open — it needs the LogManager.)
        } else {
            log->warn("Network: failed to open DD-fractional trace {}",
                      dd_frac_trace_path.string());
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
                                  "sigma0_pf,sigma0_pf_code,sigma0_pf_phase,n_pf_rows\n";
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
    if (config_.networkCarryVce()) {
        carry_vce_trace.open(carry_vce_trace_path);
        if (carry_vce_trace)
            carry_vce_trace << kCarryVceHeader;
        else
            log->warn("Network (M2/LS-VCE): failed to open carry-VCE trace {}",
                      carry_vce_trace_path.string());
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

    // INC-4: capture the L1/L15 config echoes once (pure getter reads).
    es_sigma0_trigger_   = config_.networkRaimSigma0Trigger();
    es_withhold_ceiling_ = config_.networkRaimWithholdSigma0();
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
        std::ofstream mf(manifest_path, std::ios::trunc);
        if (mf) {
            mf << "{\"record\":\"startup\","
               << "\"written_utc\":\"" << utc << "\","
               << "\"config_path\":\"" << jesc(config_path_.string()) << "\","
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
               << "\"trace_slip_events\":" << (nc.trace_slip_events ? "true" : "false") << ','
               << "\"trace_anchor_info\":" << (nc.trace_anchor_info ? "true" : "false") << ','
               << "\"trace_dd_fractional\":" << (nc.trace_dd_fractional ? "true" : "false") << ','
               << "\"trace_flag_rates\":" << (nc.trace_flag_rates ? "true" : "false") << ','
               << "\"trace_epoch_status\":" << (nc.trace_epoch_status ? "true" : "false") << ','
               << "\"trace_events\":" << (nc.trace_events ? "true" : "false") << ','
               << "\"trace_zwd\":" << (nc.trace_zwd ? "true" : "false") << ','
               << "\"trace_clock_full\":" << (nc.trace_clock_full ? "true" : "false") << ','
               << "\"trace_station_residuals\":" << (nc.trace_station_residuals ? "true" : "false") << ','
               // RTB-6/P1: service-observability knobs (health heartbeat + trace rotation).
               << fmt::format("\"health_interval_s\":{},", nc.health_interval_s)
               << fmt::format("\"trace_rotate_mb\":{},", nc.trace_rotate_mb)
               << "\"trace_rotate_keep\":" << nc.trace_rotate_keep << ','
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
               << fmt::format("\"shared_carry_var_floor_pb_m2\":{},", pb_var_floor)
               << fmt::format("\"shared_carry_var_floor_cb_m2\":{},", cb_var_floor)
               << "\"carry_zwd\":" << (zwd_on ? "true" : "false") << ','
               << fmt::format("\"zwd_carry_q\":{},", zwd_q)
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
               << fmt::format("\"process_noise_sat_code_bias\":{}", q_cb)
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
    const auto& fr_we_trigger     = counters.fr_we_trigger;
    const auto& fr_raim_cell      = counters.fr_raim_cell;
    const auto& fr_raim_sat       = counters.fr_raim_sat;
    const auto& fr_attrib         = counters.fr_attrib;
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
        emit("T2",  "within-ep trigger (T2)",  fr_we_trigger, config_.networkRaimSigma0Trigger(),      true);
        emit("T3",  "post-fit RAIM cell (T3)", fr_raim_cell,  config_.networkRaimKSigma(),             true);
        emit("T3c", "RAIM consensus (T3)",     fr_raim_sat,   config_.networkRaimKSigma(),             true);
        emit("T4",  "attribution SAT (T4)",    fr_attrib,     config_.networkRaimKSigma(),             true);
        emit("T5",  "clock-model NIS (T5)",    fr_clk_model,  config_.networkClockModelKSigma(),       true);
        emit("T6",  "clock-monitor (T6)",      fr_clk_mon,    0.0,                                     false);
        log->info("=============================================================================");
        if (fr_csv) fr_csv.close();

        // L3: run-summary CSV (same toggle).  ms_per_epoch uses n_loop_epochs (loop epochs,
        // NOT n_synced — the fast-forward / max-duration epochs are excluded from the loop).
        const std::filesystem::path rs_csv_path = log_dir_ / "network_run_summary.csv";
        std::ofstream rs_csv(rs_csv_path);
        if (rs_csv) {
            rs_csv << "n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,loop_ms,"
                      "ms_per_epoch,n_stations,n_threads,parallel\n";
            const double ms_per_epoch = n_loop_epochs
                ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0;
            rs_csv << n_synced << ',' << n_emitted << ',' << n_skipped << ','
                   << n_gate_withheld << ',' << n_sigma0_withheld << ','
                   << fmt::format("{:.1f}", loop_ms) << ',' << fmt::format("{:.3f}", ms_per_epoch) << ','
                   << n_stations << ',' << n_threads << ',' << (parallel_on ? 1 : 0) << '\n';
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
    if (if_slip_trace.is_open()) if_slip_trace.close();
    if (raim_trace.is_open()) raim_trace.close();
    if (raim_attrib_trace.is_open()) raim_attrib_trace.close();
    if (epoch_status_trace.is_open()) epoch_status_trace.close();
    if (health_trace.is_open()) health_trace.close();
    if (events_trace.is_open()) events_trace.close();
    if (zwd_trace.is_open()) zwd_trace.close();
    if (station_res_trace.is_open()) station_res_trace.close();
    if (carry_vce_trace.is_open()) carry_vce_trace.close();
    if (estimability_trace.is_open()) estimability_trace.close();   // Card DG5
}

} // namespace GnssProcessor
