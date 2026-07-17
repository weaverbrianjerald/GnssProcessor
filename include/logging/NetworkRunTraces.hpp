#pragma once
/**
 * @file NetworkRunTraces.hpp
 * @brief @internal Trace-stream ownership for Application::runNetwork — the
 *        network run loop's CSV streams, their paths and re-open headers, the
 *        startup open logic, the station-roster writer, and the B8 FlagRate
 *        counter POD.
 *
 * Hoisted VERBATIM from ApplicationNetwork.cpp (decomposition INC-3,
 * 2026-07-16; see docs/network_decomposition_addendum_2026-07-16.md).  Scope
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

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetworkConfig;
    struct GlobalVtecShConfig;
    struct StationConfig;
    class SphericalHarmonicIonosphere;
    struct NetStation;

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
    /// network_carry_vce.csv header (M2/LS-VCE hourly rows).
    inline constexpr const char* kCarryVceHeader =
        "week,tow,hour,family,const,n_rows,n_eff,robust_floor_m2,mean_floor_m2,"
        "mean_floor_std,rate_m2ps,rate_std,cond,rate_frozen,cfg_floor_m2,"
        "robust_ratio,mean_ratio,floor_clamp,rate_clamp\n";

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
        /// L2: structured per-event QC log (network.trace_events).  One LONG-format row per
        /// discrete QC action across every tier — the machine-readable replacement for
        /// spdlog regex-scraping.  Off by default (byte-identical when OFF).
        std::ofstream events_trace;
        const std::filesystem::path events_trace_path;
        /// L7: per-station ZWD trace (network.trace_zwd) — estimated ZWD + datum-conditioned
        /// formal σ + carried flag, per present station each emitted epoch (V7 instrument).
        std::ofstream zwd_trace;
        const std::filesystem::path zwd_trace_path;
        /// L13: per-station residual field (network.trace_station_residuals) — EVERY post-fit RAIM
        /// cell (non-pivot station x sat) with s_raw / s_norm / σ̂0.  Heavy opt-in deep trace.
        std::ofstream station_res_trace;
        const std::filesystem::path station_res_trace_path;
        /// M2 (LS-VCE): per-hour measured carry-variance components — one row per
        /// (family, constellation) whenever the content hour rolls over.  floor_m2 is
        /// the honest constant-cofactor σ̂ to compare against the hand var_floor;
        /// rate_m2ps the honest Δt-cofactor σ̂ to compare against q_amb; ratio =
        /// floor_m2 / config floor (>1 ⇒ the hand floor is optimistic).  Off by
        /// default (byte-identical: no file, no rows).
        std::ofstream carry_vce_trace;
        const std::filesystem::path carry_vce_trace_path;

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
    };

} // namespace GnssProcessor
