/**
 * @file NetworkRunTraces.cpp
 * @brief Trace-stream ownership + startup open logic for the network run loop
 *        (decomposition INC-3 — verbatim relocation from ApplicationNetwork.cpp;
 *        see docs/network_decomposition_addendum_2026-07-16.md).
 */

#include "logging/NetworkRunTraces.hpp"

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkRunTypes.hpp"   // NetStation (roster writer)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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
      carry_vce_trace_path(log_dir_ / "network_carry_vce.csv") {}

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
        if (vtec_sh_trace)
            vtec_sh_trace << "week,tow,n,m,trig,value_tecu,std_tecu\n";
        else
            log->warn("Network: failed to open SH-VTEC trace {}", vtec_sh_path.string());
        // T1a health trace — one row per emitted epoch.  The per-degree columns are
        // sized from the truncation degree N (deterministic header).
        vtec_sh_health_trace.open(vtec_sh_health_path);
        if (vtec_sh_health_trace && sh_model) {
            std::string hdr = "week,tow,a00_tecu,a00_std_tecu,rms_all_tecu,"
                              "slant_resid_mean_m,slant_resid_rms_m,slant_resid_max_m,"
                              "n_los,n_los_gt3sigma,n_stations,n_stations_gt3sigma,a00_in_band";
            for (int n = 0; n <= sh_model->maxDegree(); ++n)
                hdr += ",rms_deg" + std::to_string(n) + "_tecu";
            for (int n = 0; n <= sh_model->maxDegree(); ++n)
                hdr += ",kaula_ratio_deg" + std::to_string(n);
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
            epoch_status_trace << "week,tow,disposition,sigma0,n_adapt,n_present,"
                                  "n_clock_sats,clock_median_m,clock_std_m,worst_sat,"
                                  "worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,"
                                  "sigma0_trigger,withhold_ceiling,n_arc_refloat\n";  // T2d: +n_arc_refloat (appended, positions preserved)
        else
            log->warn("Network: failed to open epoch-status trace {}",
                      epoch_status_trace_path.string());
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

} // namespace GnssProcessor
