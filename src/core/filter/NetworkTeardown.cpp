/**
 * @file NetworkTeardown.cpp
 * @brief Implementation of @ref networkTeardown — the run-end readout tail relocated
 *        VERBATIM out of Application::runNetwork at the teardown-tail decomposition
 *        (2026-07-23).  The body is a byte-copy (dedent-0); the only added code is the
 *        alias preamble that byte-copies the run-TU ctx/config/setup/traces aliases.
 */

#include "filter/NetworkTeardown.hpp"

#include "app/NetworkRunContext.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "app/Application.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkEmitRebase.hpp"   // Card 4 (M1) phase D: emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // M5 prereq: PB emission-continuity filter
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // INC-2: run-loop record types (NetStation / StationAssembly / carry PODs)
#include "filter/NetworkWlRunTypes.hpp" // INC-9b: hoisted runNetwork()-local WL/RAII types
#include "filter/NetworkRobustStats.hpp"   // INC-2: median/MAD robust attribution helpers
#include "app/NetworkSetupResult.hpp"   // INC-5: setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

int networkTeardown(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const NetworkEstimator& est,
    const NetworkInvariantCarryDriver& carry_driver,
    const NetworkCarryVce& carry_vce_est,
    const SlipQcChannel& slip_qc,
    const NetworkMwWlLayer::Counters& mw_wl_run,
    const std::vector<EwlLane>& ewl_lanes,
    const WlCounters& wl_run,
    const std::map<char, WlCounters>& wl_cons_run,
    const NetworkEmitRebase& emit_rebase_engine,
    const NetworkPbEmitFilter& pb_filter_engine,
    const NetworkIonoDeweightGate& iono_deweight,
    const std::vector<SubBlockTimer>& sec_sub,
    const char* const* kSubNames,
    const std::vector<SyncedEpoch>& synced,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool ewl_on,
    bool wl_inject_on,
    bool uwl_on,
    bool lane_on,
    bool sh_dev_on,
    bool idw_on,
    bool emit_rebase_on,
    bool emit_pb_filter_on,
    double sec_asm_wall_ms,
    std::size_t sec_profiled_epochs,
    const std::chrono::steady_clock::time_point& t_loop_begin,
    std::size_t cl_epochs,
    std::size_t cl_ar_ran,
    std::size_t cl_ar_fixed,
    long long cl_sum_fixed,
    double cl_first_fix_tow,
    int cl_first_fix_week,
    const std::deque<double>& cl_recent_err3d,
    const Eigen::Vector3d& cl_truth,
    const Coordinates::Llh& cl_truth_llh,
    const std::filesystem::path& log_dir_) {
    // --- Alias preamble (byte-copies of the run-TU pre-loop alias block). ---
    const auto& nc = config_.getNetworkConfig();
    const ClosedLoopConfig& cl = nc.closed_loop;
    const bool closed_loop_on = setup.closed_loop_on;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& stations = setup.stations;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    auto& product_writer = setup.product_writer;
    auto& prod_path = setup.prod_path;
    auto& pending_contract = setup.pending_contract;
    auto& contract_pending = setup.contract_pending;
    auto& consumer = setup.consumer;
    auto& trace                = traces.trace;
    const auto& trace_path        = traces.trace_path;
    auto& eq34_reref_total = ctx.carry.eq34_reref_total;
    auto& eq34_erase_total = ctx.carry.eq34_erase_total;
    auto& eq34_shift_total = ctx.carry.eq34_shift_total;
    auto& eq34_gap_repin_total = ctx.carry.eq34_gap_repin_total;
    auto& eq34_mismatch_epochs = ctx.carry.eq34_mismatch_epochs;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;  // lock-in guard; 0 = off
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // B4: D2 code-bias carry floor;  0 = off
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // Card 1d: border time update of the coupling rows at injection (κ shrink + M²κq inflation) — requires carry_cpl_cond (enforced at load)
    const bool   carry_vce     = ctx.knobs.carry_vce;
    const double contract_rebroadcast_s = ctx.knobs.contract_rebroadcast_s; // RTA-5: S-contract re-broadcast period (product time, s; 0 = off)
    const bool   clk_runaway_on  = ctx.knobs.clk_runaway_on;        // RT0-5: per-constellation clock-runaway quarantine
    const double clk_runaway_mag = ctx.knobs.clk_runaway_mag;         // RT0-5: median |clock−global median| ceiling (m)
    const double clk_runaway_rate= ctx.knobs.clk_runaway_rate;      // RT0-5: median |drift| ceiling (m/s)
    const bool   perconst_on       = ctx.knobs.perconst_on;          // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& n_sigma0_withheld = ctx.counters.n_sigma0_withheld;
    auto& n_healed_epochs = ctx.counters.n_healed_epochs;
    auto& n_healed_cols = ctx.counters.n_healed_cols;
    auto& n_cpl_joint_fallback = ctx.counters.n_cpl_joint_fallback;
    auto& n_cpl_stash = ctx.counters.n_cpl_stash;
    auto& n_cpl_stash_empty = ctx.counters.n_cpl_stash_empty;
    auto& n_cpl_harvest_fail = ctx.counters.n_cpl_harvest_fail;
    auto& n_cpl_terms_present = ctx.counters.n_cpl_terms_present;
    auto& n_cpl_terms_departed = ctx.counters.n_cpl_terms_departed;
    auto& n_cpl_terms_shrunk = ctx.counters.n_cpl_terms_shrunk;
    auto& n_gated_epochs = ctx.counters.n_gated_epochs;
    auto& n_gated_key_epochs = ctx.counters.n_gated_key_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& lane_pair_arcs = ctx.carry.lane_pair_arcs;
    auto& lane_resid_arcs = ctx.carry.lane_resid_arcs;
    auto& lane_pair_tree_mismatch = ctx.carry.lane_pair_tree_mismatch;
    auto& carry_epochs = ctx.carry.carry_epochs;
    auto& carry_born_total = ctx.carry.carry_born_total;
    auto& carry_marg_total = ctx.carry.carry_marg_total;
    auto& carry_merges_total = ctx.carry.carry_merges_total;
    auto& carry_splits_total = ctx.carry.carry_splits_total;
    auto& carry_swaps_total = ctx.carry.carry_swaps_total;
    auto& carry_zdim_last = ctx.carry.carry_zdim_last;
    auto& carry_slipkills_total = ctx.carry.carry_slipkills_total;
    auto& carry_dropped_total = ctx.carry.carry_dropped_total;
    auto& carry_innov_flags_total = ctx.carry.carry_innov_flags_total;
    auto& carry_innov_retaps_total = ctx.carry.carry_innov_retaps_total;
    auto& carry_innov_map_miss = ctx.carry.carry_innov_map_miss;
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    const IonoConstraintDeweightConfig& idw = config_.getIonoConstraintDeweightConfig();
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    const bool sec_timers_on = nc.trace_section_timers;
    const int SB_COUNT = static_cast<int>(sec_sub.size());  // reconstructed run-TU SubBlock count

    double loop_ms = 0.0;   // L3/L15: hoisted out of the timing block so the run-summary CSV / manifest can read it
    {  // [TIMING] per-epoch scaling measurement (loop wall only; startup excluded)
        loop_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_loop_begin).count();
        log->info("Network[timing]: epoch-loop wall {:.1f} ms over {} epochs = {:.2f} ms/epoch "
                  "| {} stations | {} | threads {}",
                  loop_ms, n_loop_epochs,
                  n_loop_epochs ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0,
                  stations.size(), parallel_on ? "PARALLEL" : "SERIAL", n_threads);
    }

    // === Tier D0: section-timer RUN TOTALS ===================================
    // The whole-run breakdown that decides which Tier-D lever matters.  The kernel
    // accumulators are run-cumulative (assembly / reduce / solve / cov-inverse /
    // cov-consumers); `other` = the epoch-loop wall minus those + the driver
    // assembly wall (QC decision logic, emit, I/O, tracers).  Only the ratios are
    // load-bearing — absolute ms depend on the machine and the parallel settings.
    //
    // On the card's "QC tiers" section: the RAIM / attribution / clock-monitor tiers
    // are not a wall-separable phase — they are interleaved with and dominated by
    // covariance-consumer calls (rowRedundancy / reconstructStation), which ARE timed
    // and attributed to cov-consumers above; their residual decision logic (median
    // polish, standardization, thresholding) is cheap and falls into `other`.  So the
    // D1-relevant QC cost is already isolated in cov-consumers — bracketing the
    // scattered QC blocks would only double-count those covariance calls.
    if (sec_timers_on) {
        const auto& stmr = est.sectionTimers();
        const double other_ms = loop_ms - sec_asm_wall_ms - stmr.reduce_ms - stmr.solve_ms
                                - stmr.cov_inverse_ms - stmr.cov_consumers_ms;
        auto pct = [&](double ms) {
            return loop_ms > 0.0 ? 100.0 * ms / loop_ms : 0.0;
        };
        log->info("Network (D0) section-timer RUN TOTALS over {} profiled / {} loop epochs "
                  "(epoch-loop wall {:.0f} ms):", sec_profiled_epochs, n_loop_epochs, loop_ms);
        log->info("Network (D0)   assembly-wall {:>9.0f} ms ({:4.1f}%)  [primary produce+scatter]",
                  sec_asm_wall_ms, pct(sec_asm_wall_ms));
        log->info("Network (D0)   reduce        {:>9.0f} ms ({:4.1f}%)  [{} recomputes]",
                  stmr.reduce_ms, pct(stmr.reduce_ms), stmr.reduce_recomputes);
        log->info("Network (D0)   solve         {:>9.0f} ms ({:4.1f}%)  [{} solves]",
                  stmr.solve_ms, pct(stmr.solve_ms), stmr.solve_calls);
        log->info("Network (D0)   cov-inverse   {:>9.0f} ms ({:4.1f}%)  [D1 selected-inverse target]",
                  stmr.cov_inverse_ms, pct(stmr.cov_inverse_ms));
        log->info("Network (D0)   cov-consumers {:>9.0f} ms ({:4.1f}%)  [{} calls; D1 per-station target]",
                  stmr.cov_consumers_ms, pct(stmr.cov_consumers_ms), stmr.cov_consumer_calls);
        log->info("Network (D0)   other         {:>9.0f} ms ({:4.1f}%)  [QC / emit / I/O / tracers]",
                  other_ms, pct(other_ms));
        // Cross-cutting: kernel est.addStation (normal-forming + N_rr factorization).  Mostly
        // inside the assembly wall, but also captures within-epoch RAIM re-solve adds that fall
        // after the wall (in `other`), so it is NOT a strict sub-part of the assembly-wall bucket.
        log->info("Network (D0)   est.addStation {:>8.0f} ms ({:4.1f}%)  [{} adds; cross-cutting — "
                  "overlaps assembly-wall + any re-solve in other]",
                  stmr.add_station_ms, pct(stmr.add_station_ms), stmr.add_station_calls);
        // P0b: foldReducedDdPriors internal breakdown (serial-fold path only, i.e. under
        // parallel_carry) — which sub-part of the 49%-of-epoch DD fold dominates.
        if (stmr.fold_loop_ms + stmr.fold_minv_ms + stmr.fold_gmg_ms > 0.0)
            log->info("Network (D0)   foldDdPriors: per-station-loop {:>8.0f} ms ({:4.1f}%) | "
                      "M-inverse {:>8.0f} ms ({:4.1f}%) | GMGᵀ {:>7.0f} ms ({:4.1f}%)  [serial-fold path]",
                      stmr.fold_loop_ms, pct(stmr.fold_loop_ms),
                      stmr.fold_minv_ms, pct(stmr.fold_minv_ms),
                      stmr.fold_gmg_ms, pct(stmr.fold_gmg_ms));

        // === INC-C0: the invariant-carry driver's per-op split + census =========
        // Splits the SB_DDBUILD / SB_INSTALL wall brackets (which lump the whole driver)
        // into the primitive ops the perf-restructure card ranks — the dense-K² change-of-
        // basis projection (buildDdPriors) and posterior read-back (installPosterior) that
        // INC-C2's factored (A + U Σ_ss Uᵀ) store removes.  The census reconciles the
        // "K≈7834 never marginalized" finding (§0): whether marginalization has nothing to
        // kill (giant live components, case (a)) or the kill path is silent (case (b)).
        if (carry_invariant) {
            const auto& dp = carry_driver.perfTimers();
            log->info("Network (D0/INC-C0) carry-driver per-op over {} build / {} serial-install "
                      "epoch(s): basis-project {:>8.0f} ms ({:4.1f}%) | post-mean {:>7.0f} ms "
                      "({:4.1f}%) | post-cov {:>8.0f} ms ({:4.1f}%) | D5-gate {:>6.0f} ms ({:4.1f}%) "
                      "| time-update {:>5.0f} ms  [basis-project + post-cov = the dense-K² ops INC-C2 factors]",
                      dp.build_epochs, dp.install_epochs,
                      dp.basis_project_ms, pct(dp.basis_project_ms),
                      dp.post_mean_ms, pct(dp.post_mean_ms),
                      dp.post_cov_ms, pct(dp.post_cov_ms),
                      dp.d5_gate_ms, pct(dp.d5_gate_ms), dp.time_update_ms);
            // LF-1 INC-0: the WL-3 lane-fold split — the ~10³ per-epoch rank-1/rank-2 covariance
            // touches the perf card exists to shrink, which SB_DDBUILD lumps into basis-project.
            log->info("Network (D0/LF-1) lane-fold per-op: applyLaneMeasurement {:>8.0f} ms ({:4.1f}%) "
                      "over {} row(s) | applyLaneFix {:>8.0f} ms ({:4.1f}%) over {} row(s)  "
                      "[measurement = irreducible O(K²)/row dense downdate; fix = the ~65%-decided "
                      "SET INC-1 makes O(K)].",
                      dp.lane_meas_ms, pct(dp.lane_meas_ms), dp.lane_meas_calls,
                      dp.lane_fix_ms, pct(dp.lane_fix_ms), dp.lane_fix_calls);
            const auto& cc = carry_driver.counters();
            const long long marg_total = cc.chords_marginalized + cc.chords_marg_perchord;
            const long long chords_folded = cc.chords_carried + cc.chords_born;
            log->info("Network (D0/INC-C0) carry census: max K_c {} (single largest component) | "
                      "peak z-dim {} | marginalizations {} ({} per-chord + {} whole-block) | "
                      "births/tap-offs {} | slip refloats {}.",
                      cc.max_component_kc, cc.max_z_dim, marg_total,
                      cc.chords_marg_perchord, cc.chords_marginalized,
                      cc.chords_born, cc.slip_kills);
            // LF-1 INC-0: emitted-component fold-dim distribution (SPEC §7.4 — INC-4's Amdahl
            // ceiling ≈ heaviest component's work / total.  A single tall high bucket ⇒ one
            // giant G component dominates; a spread ⇒ the component-parallel fold balances).
            log->info("Network (D0/LF-1) K_c histogram (emitted components by fold-dim, log2 "
                      "buckets 0|1|2|4|8|16|32|64|128|256+): {} {} {} {} {} {} {} {} {} {}",
                      cc.comp_kc_hist[0], cc.comp_kc_hist[1], cc.comp_kc_hist[2], cc.comp_kc_hist[3],
                      cc.comp_kc_hist[4], cc.comp_kc_hist[5], cc.comp_kc_hist[6], cc.comp_kc_hist[7],
                      cc.comp_kc_hist[8], cc.comp_kc_hist[9]);
            // INC-C0 D-c viability: short fundamental cycles ⇒ sparse DD information matrix
            // J = Pᶻ⁻¹ ⇒ the exact info-form carry (SPEC_carry_factored_pz_C2 D-c) is bounded;
            // long cycles ⇒ dense J ⇒ D-c gives no bound (choose D-b/component-cap instead).
            log->info("Network (D0/INC-C0) D-c viability: max fundamental-cycle length {} | "
                      "mean {:.1f} over {} folded chord(s) — {} (short ⇒ sparse J ⇒ D-c bounded; "
                      "long ⇒ dense J ⇒ dense per-block cost is intrinsic).",
                      cc.max_cycle_len,
                      chords_folded ? static_cast<double>(cc.sum_cycle_len)
                                      / static_cast<double>(chords_folded) : 0.0,
                      chords_folded,
                      cc.max_cycle_len <= 8 ? "SPARSE-J likely" : "DENSE-J risk");
            // INC-C1 growth bound (also a 12 h robustness assert): a large, still-growing carry
            // with ZERO marginalization over a long run means dead chords are being retained —
            // the §0 case (b) kill-path bug.  A large per-chord count instead means case (a).
            if (marg_total == 0 && cc.max_z_dim > 0 && cc.epochs > 100)
                log->warn("Network (INC-C1 growth bound): {} carried DD(s) at peak with ZERO "
                          "marginalization(s) over {} driven epoch(s) — dead chords are NOT being "
                          "killed (INC-C0 §0 case (b)); K grows unbounded.  Audit the kill path "
                          "(NetworkInvariantCarryDriver::buildDdPriors projection-W column drop).",
                          cc.max_z_dim, cc.epochs);
        }

        // === Tier D0b: `other` sub-block RUN TOTALS, ranked =================
        // net = wall − Δ(reduce+solve+cov-inverse+cov-consumers) inside each block, so the
        // recon/solve already in the five D0 sections above is not double-counted.  Σ net vs
        // the D0 `other` figure closes the accounting; "unattributed" = loop regions no
        // bracket covers (pre-slip pivot pass, the trace/readout tail, small comment gaps).
        // This is the ranked table the perlos card acts on (HANDOFF_perlos_parallel_sweeps
        // §3 gate P0): P1/P2 target the measured top blocks, not §2's guesses.
        std::vector<int> d0b_order(SB_COUNT);
        for (int b = 0; b < SB_COUNT; ++b) d0b_order[b] = b;
        std::sort(d0b_order.begin(), d0b_order.end(), [&](int a, int c) {
            return (sec_sub[a].wall_ms - sec_sub[a].timed_ms)
                 > (sec_sub[c].wall_ms - sec_sub[c].timed_ms);
        });
        double d0b_sum_net = 0.0;
        for (const SubBlockTimer& s : sec_sub) d0b_sum_net += s.wall_ms - s.timed_ms;
        log->info("Network (D0b) `other` sub-block RUN TOTALS over {} profiled epoch(s) "
                  "(ranked by net; net excludes recon/solve already in the D0 sections):",
                  sec_profiled_epochs);
        for (int b : d0b_order) {
            const SubBlockTimer& s = sec_sub[b];
            const double net = s.wall_ms - s.timed_ms;
            log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  | wall {:>9.0f} ms "
                      "| cov-consumers {:>8.0f} ms ({} calls)",
                      kSubNames[b], net, pct(net), s.wall_ms, s.covcon_ms, s.covcalls);
        }
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [Σ over blocks]",
                  "TOTAL", d0b_sum_net, pct(d0b_sum_net));
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [D0 `other` − Σ; loop "
                  "regions no bracket covers]", "unattributed",
                  other_ms - d0b_sum_net, pct(other_ms - d0b_sum_net));
    }

    // Card DG5/DG6: the run-wide rank-gate telemetry rides INSIDE the one artifact
    // a trace-armed run is allowed to add (network_estimability.csv), not as a new
    // run-end log line — the estim golden arm byte-gates the log TEXT, and an
    // unarmed control run must stay text-identical while still yielding the
    // arm-vs-arm numbers the storm analyzer compares.  No-op with the trace off.
    traces.writeEstimabilityRunRow(ctx.counters, est.rankGateTol());
    traces.closeAll();   // INC-4: the exact former 14-stream close list

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate, {} by the A4 σ̂0 ceiling) — "
              "of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, synced.size());
    // Card DG6 — rank-gate calibration telemetry.  Gated on the tolerance being
    // ARMED (non-default) so a default run's log text is byte-identical, per DG5's
    // "with the trace on, the new FILE is the only artifact delta".  The same
    // numbers reach every trace-armed run through network_estimability.csv's RUN
    // record, so the control arm is not left dark.  Standing invariant #4: an
    // armed calibration whose counters never move is a MISWIRE, not a quiet run.
    if (config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol)
        log->info("Network (Card DG): rank gate evaluated on {} epoch(s) at tol={:g} — {} "
                  "emitted, {} withheld UNDER-specified (observed > declared: a missing "
                  "datum constraint), {} withheld OVER-specified (observed < declared: the "
                  "tolerance sits below THIS config's gauge depth, so the declared gauges "
                  "stopped being counted — RE-CALIBRATE, do not chase the datum).  Observed "
                  "near-null count over the run: min {}, max {} (declared dim is the "
                  "target).  [Placement rule: cutoff >= 2 decades above the gauge depth and "
                  ">= 2 decades below the weakest-real layer, read off this config's own "
                  "network_estimability.csv.]",
                  ctx.counters.n_gate_evaluated, config_.networkRankGateTol(),
                  n_emitted, ctx.counters.n_gate_withheld_under,
                  ctx.counters.n_gate_withheld_over,
                  ctx.counters.nullspace_obs_min, ctx.counters.nullspace_obs_max);
    if (config_.networkRaimWeCodeIono()) {
        log->info("Network (WE-CI): within-epoch code/iono screening — {} code trip(s), "
                  "{} iono trip(s), {} station exclusion(s), {} satellite code de-weight(s); "
                  "max pool σ̂0 observed: code {:.2f} (trigger {:g}), iono {:.2f} (trigger {:g}).",
                  ctx.counters.we_ci_code_trips, ctx.counters.we_ci_iono_trips,
                  ctx.counters.we_ci_sta_excl, ctx.counters.we_ci_sat_dw,
                  ctx.counters.we_ci_s0_code_max, config_.networkRaimWeCodeTrigger(),
                  ctx.counters.we_ci_s0_iono_max, config_.networkRaimWeIonoTrigger());
        if (ctx.counters.we_ci_code_trips == 0 && ctx.counters.we_ci_iono_trips == 0)
            log->warn("Network (WE-CI): armed but never tripped — do NOT read this run as "
                      "evidence the screening works.  If the max-observed σ̂0 above is 0.00 "
                      "the pools never populated (phase_leveling off gates the whole "
                      "within-epoch block via carry_warm, or no CB/SH columns registered); "
                      "otherwise the window was quiet or the triggers sit above its misfit "
                      "scale (invariant #4).");
    }
    if (config_.networkCbKaulaPrior())
        log->info("Network (Card KP): admitted-OSB physical-bound prior folded on {} epoch(s), "
                  "{} column-epoch(s) total at sigma {:g} m.  [Invariant #4: an ARMED run "
                  "reporting 0 epochs never reached a solve with the prior — check that "
                  "network.estimate_sat_code_bias is on and that code columns registered.]",
                  ctx.counters.n_kp_epochs, ctx.counters.n_kp_cols,
                  config_.networkCbKaulaSigmaM());
    if (config_.networkCbAdmission())
        log->info("Network (CB-ADM): {} code pairs admitted (shared columns), {} "
                  "quarantined at census, {} late-quarantined (first seen post-census) "
                  "— census {} (active={}).  [Invariant #4: a BDS-armed ON run with 0 "
                  "quarantined-at-census is a MISWIRE.]  Card H2 orphan window: {} "
                  "(key, epoch) pair(s) where an ADMITTED code key was per-epoch-withheld "
                  "(< 2 present stations) while its same-signal PHASE column stayed shared "
                  "— the deliberate cost of keeping the phase co-gate at CENSUS "
                  "granularity; expected 0 at 305-station density, and only actionable if "
                  "it is nonzero AND coincident with gate withholds.",
                  ctx.counters.n_cb_admitted, ctx.counters.n_cb_quarantined_census,
                  ctx.counters.n_cb_late_quarantined,
                  ctx.admission.active ? "ran" : "did-not-run", ctx.admission.active,
                  ctx.counters.n_cbadm_phase_orphan_key_epochs);
    // --- Slip-QC hardening run-end telemetry (docs/SPEC_slip_qc_hardening.md) ---
    // Standing invariant #4: a card whose counters are all zero on an armed run
    // is a MISWIRE, not a quiet run.  Card E's line is unconditional under
    // mw_detect because reporting only refloats is the L3 defect it fixes.
    if (config_.networkCycleSlipMwDetect()) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (slip-QC card E / MW-detect): {} wide-lane slip detection(s) "
                  "({} refloated) + {} LLI detection(s) ({} refloated) over the run.  "
                  "Detections are counted whether the carry was warm or COLD — a cold run "
                  "still feeds the Card-4 C3c funnel, and reporting only refloats is what "
                  "made that wiring look dead.  [MW ↔ IF agreement is now readable from "
                  "network_events.csv alone: scripts/grade_slip_agreement.py]",
                  qcc.mw_detections, qcc.mw_refloats,
                  qcc.lli_detections, qcc.lli_refloats);
    }
    if (nc.slip_qc.lli_corroborate) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (slip-QC card L / LLI corroboration): {} LLI assertion(s) seen — "
                  "{} corroborated by an MW step, {} by a GF step, {} with no measurable step, "
                  "{} asserted OFF the primary dual-frequency pair (MW/GF structurally cannot "
                  "witness those — widen the primitive if this is large); {} SUPPRESSED as "
                  "uncorroborated at a non-credible station.  Station-epoch credibility: "
                  "{} credible / {} corroboration-required.  No station, satellite or "
                  "observation was excluded from estimation — only the SLIP ACTION was "
                  "qualified.", qcc.lli_asserted, qcc.lli_corr_mw, qcc.lli_corr_gf,
                  qcc.lli_unmeasurable, qcc.lli_offpair, qcc.lli_suppressed,
                  qcc.lli_credible_epochs, qcc.lli_demoted_epochs);
        // M4: the per-run top-offenders line — the audit's STK2/ISHI/MCIL/CCJ2 list
        // must be reproducible from the log alone, without the CSV traces.
        std::vector<std::pair<long long, std::size_t>> offenders;
        for (std::size_t si = 0; si < slip_qc.hist.size(); ++si)
            if (slip_qc.hist[si].lli_suppressed > 0)
                offenders.emplace_back(slip_qc.hist[si].lli_suppressed, si);
        std::sort(offenders.begin(), offenders.end(),
                  [&](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return stations[a.second].name < stations[b.second].name;
                  });
        if (offenders.empty()) {
            log->info("Network (slip-QC card L): no station's LLI was demoted this run "
                      "(every assertion was corroborated or came from a credible station).");
        } else {
            std::string top;
            for (std::size_t k = 0; k < offenders.size() && k < 8; ++k) {
                const auto& h = slip_qc.hist[offenders[k].second];
                top += fmt::format("{}{} {}/{} suppressed (LLI-frac EWMA {:.2f}, {} demotion(s), "
                                   "{} promotion(s), now {})",
                                   top.empty() ? "" : "; ",
                                   stations[offenders[k].second].name,
                                   h.lli_suppressed, h.lli_asserted, h.lli_frac_ewma,
                                   h.lli_demotions, h.lli_promotions,
                                   h.lli_credible ? "credible" : "corroboration-required");
            }
            log->info("Network (slip-QC card L) TOP LLI OFFENDERS ({} station(s) demoted): {}",
                      offenders.size(), top);
        }
    }
    if (nc.slip_qc.gap_instrument) {
        const auto& qcc = slip_qc.counters;
        // H2 (spec-followups N3): the two counting levels are LABELLED at every
        // number.  RESUMPTIONS are per (station, satellite) re-entry; ARCS are per
        // (station, satellite, signal) — a dual-band satellite returning once is
        // 1 resumption and 2 independently bridgeable arcs, which is why the arc
        // totals exceed the resumption totals.  Both arc ledgers are closed
        // (bridged + fresh) so no reader has to infer a missing cell.
        log->info("Network (slip-QC card G / gap instrumentation): {} RESUMPTION(s) "
                  "[unit: (station,satellite) re-entry after ≥1 missed epoch] = {} SHIFTED "
                  "(raw MW/GF level moved across the break) + {} measured-and-unshifted + {} "
                  "UNMEASURABLE (no pre-gap dual-band level to compare).  Bridge "
                  "classification [unit: ARCS = (station,satellite,signal); a dual-band "
                  "resumption contributes 2]: SHIFTED → {} arc(s) re-entered with BRIDGED "
                  "carry state (THE POISON CASE) + {} fresh = {}; unshifted → {} bridged (the "
                  "intended behaviour) + {} fresh = {}; UNMEASURABLE → {} bridged (state rode "
                  "a break nobody could check — unknown, not clean) + {} fresh = {}.  D5 "
                  "attribution: {} FLAG ROW(s) of which {} coincident with a raw step, = {} "
                  "distinct funnel arc(s) re-tapped ({} raw-step-coincident) — the row count "
                  "double-counts a pair chord flagged on BOTH bands (WL-3 joint block), the "
                  "arc count is what actually re-taps; both use the exact-epoch evidence the "
                  "network_d5_flags.csv raw_stepped column reports.  [G3 decision "
                  "rule: shifted-AND-bridged ≈ 0 on a warm graded run ⇒ the mechanism is "
                  "DEFERRED WITH DATA — record this number, and read it together with the "
                  "unmeasurable-bridged count; nonzero ⇒ spec the follow-up card that marks "
                  "such arcs slipped at re-entry.]",
                  qcc.gap_resumptions, qcc.gap_shifted,
                  qcc.gap_resumptions - qcc.gap_shifted - qcc.gap_unmeasurable,
                  qcc.gap_unmeasurable,
                  qcc.gap_shifted_bridged, qcc.gap_shifted_fresh,
                  qcc.gap_shifted_bridged + qcc.gap_shifted_fresh,
                  qcc.gap_clean_bridged, qcc.gap_clean_fresh,
                  qcc.gap_clean_bridged + qcc.gap_clean_fresh,
                  qcc.gap_unmeas_bridged, qcc.gap_unmeas_fresh,
                  qcc.gap_unmeas_bridged + qcc.gap_unmeas_fresh,
                  qcc.d5_retaps, qcc.d5_retaps_stepped,
                  qcc.d5_retap_ambs, qcc.d5_retap_ambs_stepped);
    }
    if (nc.slip_qc.ref_gate) {
        IfPhaseSlipDetector::RefGateCounters rg;
        for (const auto& st : stations) {
            if (!st.if_slip) continue;
            const auto& c = st.if_slip->refGateCounters();
            rg.legacy_candidates      += c.legacy_candidates;
            rg.fires                  += c.fires;
            rg.suppressed_scale       += c.suppressed_scale;
            rg.suppressed_thin        += c.suppressed_thin;
            rg.suppressed_persistence += c.suppressed_persistence;
            rg.fires_station_event    += c.fires_station_event;
            rg.reselects              += c.reselects;
        }
        log->info("Network (slip-QC card R / IF-TD reference gate): {} legacy candidate(s) "
                  "(|median| > {:.2f}·λ_IF with n≥2) → {} fired ({} of them as an incoherent "
                  "multi-satellite STATION EVENT — reference arc reset, median NOT rebased).  "
                  "Suppressed: {} by the empirical scale / coherence test, {} thin-family "
                  "deferral(s) (< {} DDs), {} by the consecutive-fire cap; {} forced reference "
                  "re-selection(s).  [candidates − fires is exactly what the hardening removed.]",
                  rg.legacy_candidates, nc.slip_qc.ref_floor_frac, rg.fires,
                  rg.fires_station_event, rg.suppressed_scale, rg.suppressed_thin,
                  nc.slip_qc.ref_min_n, rg.suppressed_persistence, rg.reselects);
    }
    if (config_.networkPhaseDatumEq34())
        log->info("Network (Card 3/eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    if (carry_invariant)
        log->info("Network (Card 4/M1 invariant carry): {} epoch(s) driven, {} carried DD(s) at "
                  "run end; {} chord birth(s), {} marginalization(s), {} slip refloat(s), "
                  "{} unresolvable-cycle drop(s), {} tree swap(s) (basis re-expression), "
                  "{} component merge(s), {} split(s) over the run "
                  "(S-system-invariant DDs; q_z={} m²/s).",
                  carry_epochs, carry_zdim_last, carry_born_total, carry_marg_total,
                  carry_slipkills_total, carry_dropped_total,
                  carry_swaps_total, carry_merges_total, carry_splits_total,
                  config_.networkCarryInvariantQz());
    // RTB-7(d) RE-4 / invariant #4: the resume event's wiring is only visible through
    // counters — "0 restored / no shock" must be stated, never inferred from absence.
    if (carry_invariant && !config_.getNetworkConfig().state_snapshot_path.empty()) {
        log->info("Network (RTB-7d resume event): carry restored {}{}; sigma0 {}; resume "
                  "shock {}; restored Pᶻ {}.",
                  carry_driver.resumeCarryRestored() ? "YES" : "NO",
                  carry_driver.resumeCarryRestored()
                      ? fmt::format(" ({} carried DD at restore)",
                                    carry_driver.resumeCarryChords())
                      : std::string{},
                  carry_driver.resumeSigma0Seeded()
                      ? fmt::format("SEEDED {:.4f} (D5 gate trusts the restored inflation "
                                    "immediately)", carry_driver.resumeSigma0Value())
                      : std::string("NOT seeded (v1 blob / calibration off) — the gate runs "
                                    "raw-formal until its warm-up count is met"),
                  carry_driver.resumeShockSigmaM() > 0.0
                      ? fmt::format("APPLIED sigma={:.3f} m", carry_driver.resumeShockSigmaM())
                      : std::string("NONE"),
                  // RTB-7(e): which SHAPE was actually restored.  Without this the
                  // diagonal arm of the A/B is indistinguishable from the dense one in
                  // every log line and counter (invariant #4).
                  carry_driver.resumeCarryDiagonal()
                      ? fmt::format("DIAGONAL-ONLY (v3, {} block(s) — cross-covariances "
                                    "started at zero)",
                                    carry_driver.resumeCarryDiagonalBlocks())
                      : std::string("dense (v1/v2) or nothing restored"));
        // RTB-7(e) WRITE side: the blob is ~99% of the snapshot, so its size IS the
        // readout that the diagonal knob took effect.
        if (config_.getNetworkConfig().state_snapshot_carry)
            log->info("Network (RTB-7e snapshot write): {} snapshot(s) written; last carry "
                      "blob {} bytes ({:.2f} MB), shape {}.",
                      ctx.carry.snap_writes, ctx.carry.snap_carry_bytes_last,
                      static_cast<double>(ctx.carry.snap_carry_bytes_last) / (1024.0 * 1024.0),
                      ctx.carry.snap_carry_diagonal
                          ? "DIAGONAL-ONLY (v3 — Sum K_c numbers)"
                          : "dense (v2 — Sum K_c^2 numbers)");
    }
    if (sh_dev_on)
        log->info("Network (Iono-2/T5v2 per-LOS deviation carry): {} warm harvest(s), "
                  "{} cold seed(s), {} slip reset(s) (C3c funnel), {} no-information "
                  "harvest skip(s), {} deviation(s) carried at run end (τ={} s, "
                  "σ_ss={} TECU vertical, exact posterior harvest).",
                  ctx.carry.shdev_warm_total, ctx.carry.shdev_cold_total,
                  ctx.carry.shdev_slip_resets_total, ctx.carry.shdev_nodata_total,
                  sh_dev_carry.size(), shc.rep_error_tau_s,
                  shc.representation_sigma_tecu);
    if (mw_wl_on) {
        const double mean_R = mw_wl_run.rows_built > 0
            ? mw_wl_run.sum_R / static_cast<double>(mw_wl_run.rows_built) : 0.0;
        log->info("Network (M5/WL-1 MW-seeded wide-lane): {} WL row(s) folded over {} primary "
                  "cycle(s) inspected (mean R = {:.4f} cyc², i.e. σ ≈ {:.3f} cyc); {} capped by "
                  "max_rows_per_epoch.  Skips — {} non-primary group, {} missing L2 band, {} no "
                  "MW arc (channel/pivot), {} young arc (< {} ep), {} bad λ, {} mixed bias "
                  "frame.  Cross-check the KF-WL circR vs scripts/mw_census.py (Phase-A success "
                  "= circR climbing toward ~0.6 as arcs mature; SPEC §7.5).",
                  mw_wl_run.rows_built, mw_wl_run.cycles_seen, mean_R, std::sqrt(mean_R),
                  mw_wl_run.rows_capped, mw_wl_run.skip_not_primary, mw_wl_run.skip_missing_band,
                  mw_wl_run.skip_missing_mw, mw_wl_run.skip_young_arc,
                  config_.getNetworkConfig().mw_wl.min_arc_epochs, mw_wl_run.skip_bad_lambda,
                  mw_wl_run.skip_mixed_frame);
    }
    if (ewl_on) {
        // UWL-1: one line per lane instance — per-constellation when the card is armed
        // (a pooled line hides which lane is storming; an armed constellation that
        // decided 0 cycles is a LOUD 0-line, invariant #4), the legacy pooled line
        // otherwise (lane.cons == 0).
        for (const auto& lane : ewl_lanes) {
            const auto& er = lane.run;
            const double mean_R = er.rows_built > 0
                ? er.sum_R / static_cast<double>(er.rows_built) : 0.0;
            const char* label = lane.cons == '\0' ? "all-cons"
                              : lane.cons == 'G'  ? "GPS EWL (L2,L5) λ 5.861 m"
                              : lane.cons == 'E'
                                  ? (lane.top == '8'
                                         ? (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5,E5a) λ 19.537 m [preferred]"
                                                : "Galileo UWL (E5,E5a) λ 19.537 m")
                                         : (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5b,E5a) λ 9.768 m [fallback]"
                                                : "Galileo UWL (E5b,E5a) λ 9.768 m"))
                                  : "?";
            log->info("Network (M5/WL-2 {} lane): {} row(s) decided ({} INTEGER-FIXED via FFR, {} "
                      "cold-fixed n<2, {} cold-unfixed; rows {} into the solve) over "
                      "{} primary cycle(s) inspected (mean R = {:.4f} cyc²); {} capped.  Skips — {} "
                      "non-primary, {} missing band, {} no MW arc, {} young arc (< {} ep), {} bad "
                      "λ, {} mixed frame, {} x_lin quarantine.  Uncarried top band; the integers feed "
                      "the WL bridge from the STATISTIC; success = fixes by ep ~{}.",
                      label, er.rows_built + er.rows_fixed_cold, er.rows_fixed,
                      er.rows_fixed_cold, er.rows_cold_unfixed,
                      config_.getNetworkConfig().mw_wl.ewl_fold_rows ? "FOLDED" : "NOT folded",
                      er.cycles_seen, mean_R,
                      er.rows_capped, er.skip_not_primary, er.skip_missing_band,
                      er.skip_missing_mw, er.skip_young_arc,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs, er.skip_bad_lambda,
                      er.skip_mixed_frame, er.skip_quarantine,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs + 1);
        }
    }
    if (wl_inject_on) {
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-2 WL lane, phase bridge + inject): {} EWL-fixed row(s) bridged → "
                  "{} WL cycle(s) decided (mean bridge R = {:.4f} cyc², σ ≈ {:.3f} cyc, iono-de-biased); "
                  "{} hold row(s) folded, {} inject(s) (mean |Δlane| = {:.2f} cyc).  Skips — {} no/young "
                  "bridge arc, {} no iono estimate, {} bridge σ>{} guard, {} FFR-unfixed, {} MW(1,2) "
                  "closure, {} x_lin quarantine, {} not-1:1 basis.  Bridge = phase-only, DD-iono removed "
                  "via the network estimate; residual estimate-error gated by FFR |frac| + MW(1,2) "
                  "closure (§3.4 v2); inject moves the carried lane to the integer, NL preserved (§3.3).",
                  wl_run.rows_seen, wl_run.cycles_decided, mean_R, std::sqrt(mean_R),
                  wl_run.holds_folded, wl_run.injects, med_dl,
                  wl_run.skip_bridge_missing, wl_run.skip_no_iono, wl_run.skip_bridge_wide,
                  config_.getNetworkConfig().mw_wl.bridge_max_sigma_cyc,
                  wl_run.skip_not_fixed, wl_run.skip_closure, wl_run.skip_quarantine,
                  wl_run.skip_basis);
    }
    if (uwl_on && (wl_inject_on || lane_on)) {
        // UWL-1: the per-constellation WL-walk breakdown (never pooled-only — SPEC §2).
        for (const auto& [wcons, wr] : wl_cons_run) {
            const double mean_R = wr.cycles_decided > 0
                ? wr.sum_bridge_R / static_cast<double>(wr.cycles_decided) : 0.0;
            const double med_dl = wr.injects > 0
                ? wr.inject_abs_sum / static_cast<double>(wr.injects) : 0.0;
            log->info("Network (M5/UWL-1 '{}' WL walk): {} row(s) bridged → {} decided (mean "
                      "bridge R = {:.4f} cyc², {} via COLD bridge, {} via E6 MID hop), {} hold(s), "
                      "{} lane fix(es) moved + {} re-pinned, {} inject(s) (mean |Δlane| = {:.2f} "
                      "cyc).  Skips — {} no/young bridge, {} no-iono, {} bridge-wide, {} "
                      "FFR-unfixed, {} closure, {} quarantine, {} basis; mid path — {} FFR "
                      "fallback, {} MW(6,5) closure, {} direct-disagree (skipped).",
                      wcons, wr.rows_seen, wr.cycles_decided, mean_R, wr.cold_bridge_cycles,
                      wr.mid_decided,
                      wr.holds_folded, wr.lane_fix_folded, wr.lane_fix_unchanged,
                      wr.injects, med_dl,
                      wr.skip_bridge_missing, wr.skip_no_iono, wr.skip_bridge_wide,
                      wr.skip_not_fixed, wr.skip_closure, wr.skip_quarantine, wr.skip_basis,
                      wr.mid_fallback, wr.mid_closure, wr.mid_disagree);
            // Increment 5 (uwl_dual_top): the dual-rung + sibling-closure breakdown.
            // mean|frac| ≈ 0 ⇒ the 2·N(8,5)=N(7,5) identity holds on this data;
            // ≈ 0.5 ⇒ a receiver half-cycle convention offsets it (learn a constant
            // BEFORE arming uwl_sib_gate — SPEC reconciliation note 2).
            if (nmw.uwl_dual_top
                && (wr.top2_decided || wr.dual_dedupe || wr.sib_checked
                    || wr.sib_unavail || wr.sib_blocked))
                log->info("Network (M5/UWL-1 '{}' dual-top): {} of {} decided via the "
                          "FALLBACK (E5b,E5a) rung, {} fallback row(s) deduped (preferred "
                          "already decided).  Sibling closure: {} checked (mean |frac| = "
                          "{:.3f}), {} off-integer, {} unmeasurable, {} BLOCKED (gate {}).",
                          wcons, wr.top2_decided, wr.cycles_decided, wr.dual_dedupe,
                          wr.sib_checked,
                          wr.sib_checked > 0
                              ? wr.sib_abs_frac_sum / static_cast<double>(wr.sib_checked)
                              : 0.0,
                          wr.sib_int_off, wr.sib_unavail, wr.sib_blocked,
                          nmw.uwl_sib_gate ? "ON" : "off/measure-only");
        }
    }
    if (lane_on) {
        // WL-3 run-end line (invariant #4: counters or it didn't happen).  In the LANE
        // shape the interesting numbers are the continuous increments + fold-on-change
        // ratio + the inject-RETIREMENT count (≈0 expected — the WL-2 fallback path).
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-3 lane-coordinate carry): {} pair component(s) reconciled "
                  "({} pair arc(s), {} residual primary-band arc(s), {} tree-tag mismatch(es), "
                  "{} spanning-forest completion(s)); "
                  "lane increments folded — {} MW(1,2) row(s), {} bridge row(s), {} skipped "
                  "(immature/stale/mixed/slip/quarantine), {} pinned-skipped (LF-1 INC-2).  "
                  "FFR lane fixing: {} decided "
                  "cycle(s) (mean bridge R = {:.4f} cyc²) → {} integer(s) MOVED/new, {} "
                  "re-pinned (idempotent lane SET — information-once-only).  Inject FALLBACK: "
                  "{} fired (mean |Δlane| = {:.2f} cyc; ≈0 expected — continuous tightening "
                  "supersedes the WL-2 injects), {} not-1:1 basis skips.",
                  carry_driver.counters().pair_components,
                  lane_pair_arcs, lane_resid_arcs, lane_pair_tree_mismatch,
                  carry_driver.counters().pair_tree_promotions,
                  wl_run.lane_mw_rows, wl_run.lane_bridge_rows, wl_run.lane_skip_meas,
                  wl_run.lane_pinned_skips,
                  wl_run.cycles_decided, mean_R,
                  wl_run.lane_fix_folded, wl_run.lane_fix_unchanged,
                  wl_run.injects, med_dl, wl_run.skip_basis);
    }
    if (carry_innov_gate)
        log->info("Network (Card 4/M1 D5 fine-slip innovation gate): {} carried-DD innovation "
                  "flag(s), {} fresh re-tap(s) over the run (k={}; the estimation-side reset for "
                  "the fine slips the coarse MW/LLI/IF-TD layer is relaxed against — each flagged "
                  "chord re-taps data-only + marks its arc slipped for the next epoch).",
                  carry_innov_flags_total, carry_innov_retaps_total,
                  config_.networkCarryInvariantInnovK());
    if (carry_innov_gate && config_.networkCarryInvariantInnovCalibrate())
        log->info("Network (Card 4/M1 D5 v2 LS-VCE calibration): final empirical inflation σ̂₀ = "
                  "{:.2f}× (the gate threshold is k·σ̂₀ = {:.1f} formal-σ; σ̂₀>1 quantifies how "
                  "optimistic the formal Cov(v) is — the M2 monitor's finding, now closing the loop "
                  "on the gate itself).", carry_driver.innovInflationSigma0(),
                  config_.networkCarryInvariantInnovK() * carry_driver.innovInflationSigma0());
    if (idw_on) {
        std::string idw_list;
        for (const auto& nm : iono_deweight.everDeweighted()) {
            if (!idw_list.empty()) idw_list += ' ';
            idw_list += nm;
        }
        if (idw.adaptive)
            log->info("Network (iono deweight): {} station(s) ever deweighted (peak {} "
                      "simultaneous), {} adaptive engage / {} release event(s) over the run "
                      "(static w_s={:.3f}; adaptive gate {:.2f}σ↑ / {:.2f}σ↓ → floor {:.3f}; "
                      "network-relative robust z-score, chronic >3σ background removed by the "
                      "median-centering).{}{}",
                      iono_deweight.everDeweighted().size(), iono_deweight.peakDeweighted(),
                      iono_deweight.triggerEvents(), iono_deweight.releaseEvents(),
                      iono_deweight.staticWeight(), idw.sigma_hi, idw.sigma_lo, idw.floor,
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
        else
            log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                      "w_s={:.3f} for the whole run — the SH-iono coupling was relaxed while "
                      "they kept serving the shared clocks/biases/datum/position.{}{}",
                      iono_deweight.staticCount(), iono_deweight.staticWeight(),
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
    }
    // SPEC_day129_frame_guards Phase 2.1 run-end telemetry.  Standing invariant #4:
    // "0 trips / 0 reinits" means the guard is INERT no matter how clean the run
    // looks — this line is the ON-path evidence, and the validation gate reads it.
    if (config_.getNetworkConfig().cb_excursion_reinit) {
        log->info("Network (Phase 2.1 CB-excursion guard): {} trip(s), {} code-bias "
                  "carry reinit(s), {} discontinuity bump(s) landed; largest median "
                  "|Δδtˢ| observed {:.2f} m at TOW {:.1f} (threshold {:.2f} m).  D1 "
                  "evidence: {} (satellite,epoch) NON-pivot case(s) a per-satellite "
                  "rule would have reinitialized — scope is {}.",
                  ctx.carry.cb_exc_trips, ctx.carry.cb_exc_reinits,
                  ctx.carry.cb_exc_bumps, ctx.carry.cb_exc_max_median,
                  ctx.carry.cb_exc_max_median_tow,
                  config_.getNetworkConfig().cb_excursion_median_m,
                  ctx.carry.cb_exc_nonpivot_would,
                  config_.getNetworkConfig().cb_excursion_reinit_all_sats
                      ? "ALL satellites (D1b)" : "clock-PIVOT only (D1)");
        if (ctx.carry.cb_exc_trips == 0)
            log->warn("Network (Phase 2.1): the CB-excursion guard is ARMED but never "
                      "tripped over this run — it is INERT here.  Either the window "
                      "carries no common-mode excursion, or the threshold ({:.2f} m) is "
                      "above anything that occurred (largest observed median {:.2f} m).  "
                      "Do NOT read this run as evidence the guard works.",
                      config_.getNetworkConfig().cb_excursion_median_m,
                      ctx.carry.cb_exc_max_median);
        if (!ctx.carry.cb_excursion_pending_bump.empty())
            log->warn("Network (Phase 2.1): {} queued code-bias discontinuity bump(s) "
                      "never landed (their satellites were absent from every subsequent "
                      "emission) — the reinit stepped a value the product did not signal.",
                      ctx.carry.cb_excursion_pending_bump.size());
    }
    // SPEC_day129_frame_guards Phase 2.2 run-end telemetry (invariant #4): the
    // out-of-band count is per EPOCH — the throttled T1a WARN under-read the
    // reference run 23.8× (9 warnings vs 214 out-of-band epochs), which is the
    // defect this gate replaces.
    if (config_.getNetworkConfig().sh_health_gate) {
        log->info("Network (Phase 2.2 SH health gate): {} out-of-band EPOCH(s), {} "
                  "engagement(s), {} emission(s) withheld (band [{:.1f}, {:.1f}] TECU, "
                  "rms ceiling {}, min_epochs {}).{}",
                  ctx.carry.sh_gate_oob_epochs_total, ctx.carry.sh_gate_engagements,
                  ctx.counters.n_sh_gate_withheld,
                  config_.getGlobalVtecShConfig().a00_warn_low_tecu,
                  config_.getGlobalVtecShConfig().a00_warn_high_tecu,
                  config_.getNetworkConfig().sh_gate_rms_max_tecu > 0.0
                      ? fmt::format("{:.1f} TECU", config_.getNetworkConfig().sh_gate_rms_max_tecu)
                      : std::string{"off"},
                  config_.getNetworkConfig().sh_gate_min_epochs,
                  ctx.carry.sh_gate_engaged
                      ? "  STILL ENGAGED at run end — the era outlived the data."
                      : "");
        if (ctx.carry.sh_gate_oob_epochs_total == 0)
            log->warn("Network (Phase 2.2): the SH health gate is ARMED but the field "
                      "never left its band over this run — the gate is untested here.  "
                      "Do NOT read this run as evidence the gate works.");
    }
    // SPEC_day129_frame_guards P1-3/P1-4 run-end telemetry.  Standing invariant #4:
    // an instrumented run must state how many epochs the instrumentation actually
    // produced — "0 traced" means the wiring is inert no matter how clean the log
    // looks, and the trigger verdict would be built on an empty file.
    if (config_.getNetworkConfig().trace_postfit_sigma0)
        log->info("Network (P1-4 post-fit σ̂0): {} emitted epoch(s) pooled a post-fit "
                  "residual statistic (last epoch: {} rows = {} code + {} phase; σ̂0 "
                  "{:.4f} all / {:.4f} code / {:.4f} phase).  Read the CODE number: "
                  "with free ambiguities every phase row is fitted exactly by its own "
                  "ambiguity, so the phase pool is identically zero and DILUTES the "
                  "pooled statistic — that zero is the finding (no phase-side "
                  "redundancy, hence no phase fault detection without phase-leveling). "
                  "MEASURE-ONLY — the A4 withhold ceiling still reads the within-epoch "
                  "RAIM's sigma0_adapt, so this run's emit decisions are unchanged.",
                  ctx.carry.pf_epochs_total, ctx.carry.ep_pf_rows,
                  ctx.carry.ep_pf_code_rows, ctx.carry.ep_pf_phase_rows,
                  ctx.carry.ep_sigma0_pf, ctx.carry.ep_sigma0_pf_code,
                  ctx.carry.ep_sigma0_pf_phase);
    if (config_.getNetworkConfig().trace_sh_spectrum)
        log->info("Network (P1-3 shared-normal spectrum): {} epoch(s) written to "
                  "network_sh_spectrum.csv (mode detail every {} traced epoch(s)).",
                  ctx.carry.sh_spectrum_epochs,
                  config_.getNetworkConfig().trace_sh_spectrum_stride);
    if (carry_innov_gate && carry_innov_map_miss > 0)
        log->warn("Network (Card 4/M1 D5): {} innovation flag(s) over the run did not resolve in the "
                  "flag→ambiguity reverse map (next-epoch re-tap skipped for them) — a reverse-map/"
                  "block wiring inconsistency; investigate.", carry_innov_map_miss);
    if (emit_rebase_on) {
        // Card 4 (M1) phase D: the emit-layer re-base telemetry (spec §3) — the
        // integer cycles + common-fractional shift removed onto the previous emission
        // frame (an S-selection on OUTPUTS ONLY; user DDs are invariant to it).  Feeds
        // the consumer A/B: high integer/frac churn with a silent disc counter is the
        // datum-freedom the carry buys; a surviving disc step is a genuine event.
        const auto& rb = emit_rebase_engine.counters();
        log->info("Network (Card 4/M1 phase D emit re-base): {} emit epoch(s), {} component "
                  "re-base(s), {} integer cycle(s) removed across {} key(s), {} reference-path "
                  "slip step(s) preserved; mean |common-fractional δ| = {:.4f} m over the run "
                  "(OUTPUTS-only S-selection; DDs invariant).",
                  rb.epochs, rb.components, rb.integer_cycles, rb.keys_rebased,
                  rb.slips_preserved,
                  rb.frac_shift_n ? rb.frac_shift_abs_sum / static_cast<double>(rb.frac_shift_n)
                                  : 0.0);
    }
    if (emit_pb_filter_on) {
        // PB emission-continuity filter telemetry (M5 prerequisite).  A high
        // smoothed:reset ratio with a small mean posterior σ is the win — the
        // per-epoch noise absorbed into the filter, so the emitter jump test fires
        // (via the reset raw-value steps) only at genuine events.  ref-path + gap
        // resets are the structural (never-laundered) events; innovation resets are
        // the calibrated k·σ̂_step genuine steps.  ON-path counters (inv #4).
        const auto& pf = pb_filter_engine.counters();
        const long long resets = pf.resets_refpath + pf.resets_innovation + pf.resets_gap;
        log->info("Network (M5 prereq / PB emission-continuity filter): {} emit epoch(s), "
                  "{} smoothed / {} reset ({} ref-path slip, {} innovation, {} gap) key-epoch(s), "
                  "{} first-sighting init(s); mean posterior σ = {:.4f} m (self-tuning k·σ̂_step "
                  "gate, k={}; OUTPUTS-only — the solve, user DDs and dd-frac grading are "
                  "byte-unchanged; the emitter jump test stays the sole disc authority).",
                  pf.epochs, pf.smoothed, resets, pf.resets_refpath, pf.resets_innovation,
                  pf.resets_gap, pf.inits,
                  pf.smoothed ? pf.post_sigma_sum / static_cast<double>(pf.smoothed) : 0.0,
                  config_.networkEmitPbFilterJumpK());
        if (config_.networkEmitPbFilterGateV2()) {   // PB-gate-v2 merge point (run-end telemetry)
            std::string sig;
            for (const auto& kv : pb_filter_engine.stepSigmas())
                sig += fmt::format("{}{}={:.4f}", sig.empty() ? "" : " ",
                                   kv.first.empty() ? "pool" : kv.first, kv.second);
            log->info("Network (PB filter GATE v2): {} row(s) fed to σ̂_step (censoring-free: "
                      "smoothed + innovation-gated), converged σ̂_step [m] {}; model-scaled gate "
                      "k·√(Ppred+R), k={:.1f} — rate-transferable (one k for 30 s & 300 s).",
                      pf.fed_rows, sig.empty() ? std::string("(none)") : sig,
                      config_.networkEmitPbFilterJumpK());
        }
    }
    if (clk_runaway_on && n_runaway_epochs > 0)
        log->warn("Network (RT0-5): {} epoch(s) dropped at least one constellation for clock "
                  "runaway (magnitude ceiling {:.0f} m / rate ceiling {:.2f} m/s).",
                  n_runaway_epochs, clk_runaway_mag, clk_runaway_rate);
    if (n_healed_epochs > 0)
        log->info("Network: prioritized-datum drop-and-heal recovered the gate on {} epoch(s) "
                  "({} near-null phase-bias column(s) soft-anchored).",
                  n_healed_epochs, n_healed_cols);
    // Card PA2 — the heal's ATTEMPTS, which the success-only line above hides.
    // Gated on attempts so a run that never reaches the gate-failure path (every
    // healthy config, incl. the whole golden matrix) prints nothing new.
    if (ctx.counters.n_heal_attempt_epochs > 0) {
        // The insufficiency caveat is the whole reason the ATTEMPT counters exist,
        // but it is only TRUE when the heal fired more than it rescued.  Printing
        // it when attempts == recoveries (a fully successful heal — the PA4 emit
        // case) would read as a warning on a clean run.
        const bool insufficient = ctx.counters.n_heal_attempt_epochs > n_healed_epochs;
        log->info("Network (Card PA): drop-and-heal FIRED on {} epoch(s), soft-anchoring {} "
                  "near-null phase-bias column(s); {} epoch(s) were RECOVERED by it ({} "
                  "column(s)).{}  Per-column attribution (sat/signal/eigenvalue/span) is in "
                  "network_events.csv under tier GATE, event phase_heal, when "
                  "network.trace_events is armed.",
                  ctx.counters.n_heal_attempt_epochs, ctx.counters.n_heal_attempt_cols,
                  n_healed_epochs, n_healed_cols,
                  insufficient
                      ? "  Attempts > recoveries means the heal is reaching the right columns "
                        "but cannot resolve the residual alone on some epochs — that is PA5's "
                        "insufficiency criterion, not a dead heal."
                      : "  Every fired heal recovered the gate.");
    }
    if (carry_cpl_joint && n_cpl_joint_fallback > 0)
        log->warn("Network (Card 1b): {} station-epoch(s) had a non-SPD joint coupling-row "
                  "covariance after the ridge retry and fell back to the Card-1 per-row scalar "
                  "marginalization for that station (the between-row DD correlation is not "
                  "recovered on those). Investigate if the count is a large fraction of "
                  "station-epochs.", n_cpl_joint_fallback);
    if (carry_cpl_cond)
        log->info("Network (Card 1/1d): coupling-carry volume — stashes: {} with a regression, "
                  "{} EMPTY-regression (diagonal degrade), {} harvest failures; injected terms: "
                  "{} present, {} departed, {} kappa-shrunk (time-update {}).",
                  n_cpl_stash, n_cpl_stash_empty, n_cpl_harvest_fail,
                  n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                  carry_cpl_tu ? "ON" : "OFF");
    if (carry_vce) {
        // M2 (LS-VCE): run-end honesty summary — the trailing-window measured
        // carry-variance FLOOR σ̂ vs the hand floor, per (family, constellation).
        // ratio > 1 ⇒ the hand floor is optimistic (real carry disagreement
        // exceeds it); ratio < 1 ⇒ the hand floor is conservative.  The per-hour
        // series is in network_carry_vce.csv.
        log->info("Network (M2/LS-VCE): carry-variance monitor summary "
                  "(components='{}', window={} ep) — ROBUST (geometric-mean, tail-safe) "
                  "carry-innovation variance vs the hand floor:",
                  config_.networkCarryVceComponents(), config_.networkCarryVceWindowEpochs());
        for (const auto& fc : carry_vce_est.keys()) {
            const auto e = carry_vce_est.estimate(fc.first, fc.second);
            const double cfg_floor = (fc.first == "pb") ? pb_var_floor
                                   : (fc.first == "cb") ? cb_var_floor
                                                        : pl_var_floor;
            const double rrat = (cfg_floor > 0.0) ? e.robust_floor / cfg_floor : 0.0;
            const double mrat = (cfg_floor > 0.0)
                ? e.sigma[NetworkCarryVce::kFloor] / cfg_floor : 0.0;
            log->info("  [{}|{}] robust σ̂ = {:.3e} m² (hand {:.3e}, ratio {:.1f}); "
                      "mean σ̂ = {:.3e} m² (ratio {:.0f}, event-incl.); {} rows.",
                      fc.first, fc.second.empty() ? std::string("*") : fc.second,
                      e.robust_floor, cfg_floor, rrat,
                      e.sigma[NetworkCarryVce::kFloor], mrat, e.n_rows);
        }
    }
    if (n_gated_epochs > 0)
        log->info("Network: anchored-union coverage gate (< {} trackers) excluded thin shared "
                  "phase-bias column(s) on {} epoch(s) ({} column-epoch(s) total; their phase "
                  "rows served the satellite clock through the local blocks).",
                  config_.networkPhaseDatumMinStations(), n_gated_epochs, n_gated_key_epochs);
    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);

    // L15 end-of-run manifest record (body relocated to NetworkRunTraces, INC-4).
    traces.writeRunManifestComplete(nc, ctx.counters, synced.size(), loop_ms);

    // --- B6 (P1-1): serialized-contract round-trip self-check --------------------
    // Close the writer (flush the header), read it back, and cross-check the parsed
    // S-basis contract against what we serialized.  This is a LIVE checkContract call
    // site (previously dead code) that guards the write→read→consume replay path a
    // real product consumer would take: a header serialization/parse regression fails
    // here loudly instead of silently mis-declaring the datum to a downstream user.
    if (contract_pending && !prod_path.empty()) {
        // Zero epochs emitted: the deferred writer never opened, and the round-trip
        // self-check below is skipped.  The stale previous file was removed at
        // staging — unless that removal failed (locked/read-only), in which case a
        // PREVIOUS run's products still sit at the path: check, don't assert.
        if (std::filesystem::exists(prod_path))
            log->warn("Network[B6]: no epochs were emitted AND a stale product file from a "
                      "PREVIOUS run remains at '{}' (removal failed at startup) — do NOT "
                      "consume it; it is not this run's output.", prod_path.string());
        else
            log->warn("Network[B6]: no epochs were emitted — the deferred contract writer never "
                      "opened; no product file exists at '{}' (round-trip self-check skipped).",
                      prod_path.string());
        // L14: record the distinguishable "writer never opened" outcome so an absent
        // contract-check CSV unambiguously means "the check never ran".
        if (nc.serialize_sbasis_contract) {
            std::ofstream cc(log_dir_ / "network_contract_check.csv");
            if (cc) cc << "check,status,detail\n"
                          "overall,FAIL,writer never opened (zero epochs emitted)\n";
        }
    }
    if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty()) {
        product_writer.reset();   // flush + close so the reader sees the complete header
        SbasisContract parsed;
        bool           rok = false;
        const std::vector<NetworkProductRecord> check_recs =
            NetworkProductReader::read(prod_path.string(), &parsed, &rok);
        // RTA-5: with re-broadcast armed, run checkContract on EVERY S receipt (each
        // record's contract_refresh), not once — the per-receipt behaviour a late-
        // joining consumer relies on.  The per-receipt expectation drops the pivot
        // map (pivots legitimately evolve across re-broadcasts; the FINAL pivot map
        // is checked separately below against the last-written contract) but keeps
        // every run-constant convention field AND the run's single generation id.
        // Gated on the toggle so an OFF run's check output is byte-identical.
        std::size_t s_receipts = 0, s_receipt_fails = 0;
        std::vector<std::string> s_receipt_mism;
        if (contract_rebroadcast_s > 0.0) {
            SbasisContract receipt_expect = pending_contract;
            receipt_expect.decl.clock_pivot_sat.clear();
            for (std::size_t ri = 0; ri < check_recs.size(); ++ri) {
                if (!check_recs[ri].contract_refresh.present) continue;
                ++s_receipts;
                std::vector<std::string> rm;
                if (!NetworkProductReader::checkContract(check_recs[ri].contract_refresh,
                                                         receipt_expect, &rm)) {
                    ++s_receipt_fails;
                    for (const auto& m : rm)
                        s_receipt_mism.push_back(fmt::format("receipt {} (epoch {}): {}",
                                                             s_receipts, ri, m));
                }
            }
            if (s_receipt_fails > 0) {
                log->warn("Network[RTA-5]: {} of {} re-broadcast S receipt(s) FAILED the "
                          "per-receipt contract check:", s_receipt_fails, s_receipts);
                for (const auto& m : s_receipt_mism) log->warn("    - {}", m);
            } else {
                log->info("Network[RTA-5]: {} S-contract receipt(s) parsed positionally; every "
                          "one passed the per-receipt contract check.", s_receipts);
            }
        }
        std::vector<std::string> mism;
        if (!parsed.present) {
            log->warn("Network[B6]: the serialized S-basis contract did not parse back from '{}' — "
                      "header round-trip FAILED.", prod_path.string());
        } else if (!NetworkProductReader::checkContract(parsed, pending_contract, &mism)) {
            log->warn("Network[B6]: serialized S-basis contract does not round-trip cleanly "
                      "({} mismatch(es)):", mism.size());
            for (const auto& m : mism) log->warn("    - {}", m);
        } else if (parsed.decl.clock_pivot_sat != pending_contract.decl.clock_pivot_sat) {
            log->warn("Network[B6]: the realized clock-pivot map did not round-trip through the header.");
        } else {
            log->info("Network[B6]: S-basis contract round-trips cleanly "
                      "(write → read → checkContract), {} realized pivot(s) serialized.",
                      parsed.decl.clock_pivot_sat.size());
        }
        // L14: machine-readable check result.  The pivot-map round-trip is checked SEPARATELY
        // (checkContract only compares the expected-side pivots, so a realized-pivot mismatch is
        // NOT in `mism`) and must fail 'overall' too.  Written on PASS as well — an ABSENT file
        // means the check never ran (crash / zero epochs), distinct from a recorded FAIL.
        const bool pivot_ok = parsed.present
            && (parsed.decl.clock_pivot_sat == pending_contract.decl.clock_pivot_sat);
        const bool overall = rok && parsed.present && mism.empty() && pivot_ok
            && (contract_rebroadcast_s <= 0.0 || s_receipt_fails == 0);
        const std::filesystem::path cc_path = log_dir_ / "network_contract_check.csv";
        std::ofstream cc(cc_path);
        if (cc) {
            cc << "check,status,detail\n";
            cc << "file_readable," << (rok ? "PASS" : "FAIL") << ",\n";
            cc << "contract_present," << (parsed.present ? "PASS" : "FAIL") << ",\n";
            for (const auto& m : mism)
                cc << "field_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            // RTA-5 (toggle-gated so an OFF run's CSV is byte-identical): per-receipt
            // contract-check summary across every positional S line in the file.
            if (contract_rebroadcast_s > 0.0) {
                cc << "s_receipts," << (s_receipt_fails == 0 ? "PASS" : "FAIL") << ','
                   << s_receipts << " receipt(s); " << s_receipt_fails << " failed\n";
                for (const auto& m : s_receipt_mism)
                    cc << "s_receipt_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            }
            cc << "pivot_map_roundtrip," << (pivot_ok ? "PASS" : "FAIL") << ",\n";
            cc << "overall," << (overall ? "PASS" : "FAIL") << ','
               << parsed.decl.clock_pivot_sat.size() << " realized pivot(s) serialized\n";
            cc.close();
        } else {
            log->warn("Network[L14]: failed to open contract-check CSV {}", cc_path.string());
        }
    }

    // B8 flag-rate empirical-alpha summary + L3 run-summary CSV (bodies relocated
    // to NetworkRunTraces, INC-4; ONE trace_flag_rates gate covers both).
    traces.writeFlagRateAndRunSummary(config_, ctx.knobs, ctx.counters, synced.size(),
                                      stations.size(), n_threads, parallel_on, loop_ms, log);

    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());

    // --- Closed-loop integer-recovery summary -------------------------------
    if (closed_loop_on && consumer) {
        const double fix_rate = cl_ar_ran ? (100.0 * static_cast<double>(cl_ar_fixed)
                                             / static_cast<double>(cl_ar_ran)) : 0.0;
        const double mean_fixed = cl_ar_fixed ? (static_cast<double>(cl_sum_fixed)
                                                 / static_cast<double>(cl_ar_fixed)) : 0.0;
        // Converged-window robust stats: median (typical accuracy, spike-immune)
        // + max (surfaces wrong-fix excursions the RMS would smear).
        double conv_med = 0.0, conv_max = 0.0;
        if (!cl_recent_err3d.empty()) {
            std::vector<double> w(cl_recent_err3d.begin(), cl_recent_err3d.end());
            std::sort(w.begin(), w.end());
            conv_med = w[w.size() / 2];
            conv_max = w.back();
        }
        const Eigen::Vector3d final_err = consumer->getPosition() - cl_truth;
        const Eigen::Vector3d final_enu = Coordinates::ecefToEnu(final_err, cl_truth_llh);

        const char* product_label = !cl.consume_network_products
            ? "BASELINE (precise products only — network store NOT consumed)"
            : (cl.consume_network_clock ? "FULL (network clock + phase bias)"
                                        : "BIAS-ONLY (precise clock + network phase bias)");
        const std::string mode_label =
            std::string(cl.consumer_undifferenced ? "UNDIFFERENCED" : "BSSD")
            + " user / " + product_label;

        // Honest, fix-rate-led verdict.  WL-only AR does not fix the narrow lane,
        // so the post-fix position stays at the WL-float level (~m) even on a
        // healthy product — the WL fix RATE, not the position, is the recovery
        // metric.  Flag a marginal result and point at the A/B baseline knob so a
        // reader never reads a degraded run as a clean pass.
        std::string verdict;
        if (cl_ar_fixed == 0)
            verdict = "NO integers recovered — inspect closed_loop_trace.csv / SSR coverage";
        else if (fix_rate >= 50.0)
            verdict = fmt::format("RECOVERED INTEGERS — {:.0f}% WL fix rate from our own "
                                  "network products", fix_rate);
        else
            verdict = fmt::format("PARTIAL — {:.0f}% WL fix rate (marginal); compare with the "
                                  "precise-products baseline (network.closed_loop."
                                  "consume_network_products=false)", fix_rate);

        log->info("==== Closed-loop integer-recovery harness — held-out '{}' ====",
                  cl.station.name);
        log->info("  mode                      : {}", mode_label);
        log->info("  consumer epochs processed : {}", cl_epochs);
        log->info("  AR invoked / accepted     : {} / {}  ({:.1f}% fix rate)",
                  cl_ar_ran, cl_ar_fixed, fix_rate);
        log->info("  mean WL integers per fix  : {:.1f}", mean_fixed);
        if (cl_first_fix_tow >= 0.0)
            log->info("  first integer fix at      : week {} tow {:.1f}",
                      cl_first_fix_week, cl_first_fix_tow);
        else
            log->info("  first integer fix         : NONE — no epoch recovered integers");
        log->info("  position error (final)    : {:.3f} m 3D  (E {:.3f}, N {:.3f}, U {:.3f})",
                  final_err.norm(), final_enu.x(), final_enu.y(), final_enu.z());
        log->info("  position (last {} ep)      : median {:.3f} m, max {:.3f} m 3D "
                  "(max >> median ⇒ wrong-fix excursions)",
                  cl_recent_err3d.size(), conv_med, conv_max);
        log->info("  VERDICT                   : {}", verdict);
        log->info("  closed-loop trace         : {}",
                  (log_dir_ / "closed_loop_trace.csv").string());
        log->info("==============================================================");
    }
    if (n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the shared datum is under-specified — "
                   "a near-null W6 code-bias direction withholds every epoch when sparse "
                   "(often BeiDou) satellites register OSB columns with < 2 IF-pinnable "
                   "signals.  Arm the run-latched admission census (network.cb_admission; "
                   "tune network.cb_admission_min_stations / _census_epochs) so those "
                   "pairs stay station-local sponges, and confirm >= 2 overlapping stations.");
        return 1;
    }
    return 0;
}

} // namespace GnssProcessor
