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
        /// SPEC_day129_frame_guards §4.3 (@c network.trace_postfit_keyed): keyed
        /// robust post-fit pools from the P1-4 residual pass — one row per
        /// (kind ∈ {STA, SAT, SIG}, key) per emitted epoch, {code, phase} pools
        /// each (n, σ̂0 = 1.4826·median, rms, max of |v|√w).  The RHS-localization
        /// instrument: WHERE the onset misfit sits — which stations (geography),
        /// which satellites, which signals (dispersive split).
        std::ofstream pf_keyed_trace;
        const std::filesystem::path pf_keyed_trace_path;
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

        /// L2: one network_events.csv row for a discrete QC action (no-op OFF).
        /// Commas in @p detail are sanitized to semicolons (single CSV field).
        void emitEvent(const GnssTime& t, const char* tier, const char* event,
                       const std::string& sat, const std::string& station,
                       std::optional<double> statistic, std::optional<double> threshold,
                       const char* action, const std::string& detail);

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
