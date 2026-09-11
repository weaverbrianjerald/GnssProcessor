/**
 * @file NetworkTeardown.cpp
 * @brief Implementation of @ref networkTeardown — the run-end readout tail relocated
 *        VERBATIM out of Application::runNetwork at the teardown-tail decomposition
 *        (2026-07-23).  The body is a byte-copy (dedent-0); the only added code is the
 *        alias preamble that byte-copies the run-TU ctx/config/setup/traces aliases.
 */

#include "filter/NetworkInject.hpp"
#include "filter/NetworkTeardown.hpp"

#include "app/NetworkRunContext.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "measurement/MeasurementModel.hpp"   // complete type for NetStation::mm
#include "model/PppModel.hpp"                  // complete type for NetStation::model (wind-up census)
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
#include "filter/NetworkBlockBaarda.hpp"       // BlockFormal LOS-block census (networkBlockBaardaReport)
#include "filter/NetworkCongruence.hpp"        // station congruence screen (networkCongruenceReport)
#include "filter/NetworkMdb.hpp"               // Baarda MDB (networkMdbReport — the run-end body lives there)
#include "filter/NetworkClockAnchor.hpp"       // Card Q3 §3: kCtaWLadder / kCtaELadder for the guard-ladder run-end line
#include "filter/NetworkRedundancyCensus.hpp"  // redundancy census (kCensusMinPoolRows for the run-end line)
#include "filter/NetworkPrefitCensus.hpp"      // prefit census (kNetPrefitMinTypeRows / kNetPrefitPinWeight for the run-end line)
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
    const NetworkPriorResidual& prires_est,
    const SlipQcChannel& slip_qc,
    const NetworkMwWlLayer::Counters& mw_wl_run,
    const std::vector<EwlLane>& ewl_lanes,
    const WlCounters& wl_run,
    const std::map<char, WlCounters>& wl_cons_run,
    const NetworkEmitRebase& emit_rebase_engine,
    const NetworkPbEmitFilter& pb_filter_engine,
    const NetworkClockEmitRebase& clk_rebase_engine,
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
    auto& eq34_cpl_sprev_total = ctx.carry.eq34_cpl_sprev_total;
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
            // INC-C0 D-c viability: short fundamental cycles keep the cycle matrix G sparse,
            // which is NECESSARY BUT NOT SUFFICIENT for a bounded exact info-form carry
            // (SPEC_carry_factored_pz_C2 D-c) — the border term is Gᵀ Σ_ss G, so the BINDING
            // probe is Σ_ss sparsity/rank, not cycle length (that doc's §6.1 corrects the
            // original "sparse iff cycles are short" claim; this counter only screens G).
            log->info("Network (D0/INC-C0) D-c viability screen (G-side only): max "
                      "fundamental-cycle length {} | mean {:.1f} over {} folded chord(s) — {} "
                      "(short cycles ⇒ sparse G: NECESSARY, not sufficient — the binding probe "
                      "is Σ_ss sparsity/rank; long ⇒ dense per-block cost is intrinsic).",
                      cc.max_cycle_len,
                      chords_folded ? static_cast<double>(cc.sum_cycle_len)
                                      / static_cast<double>(chords_folded) : 0.0,
                      chords_folded,
                      cc.max_cycle_len <= 8 ? "SPARSE-G likely" : "DENSE-G risk");
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
        // This is the ranked table the perlos card acts on (SPEC_dev_tier_perf_cards
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
    // RTA-10 gap fix: heartbeats buffered for a v2 writer open that never came.
    // A run that NEVER emits cannot stamp the contract header (no realized
    // datum), so no product file is fabricated — but the withheld era must not
    // vanish silently (invariant #4).
    if (!setup.pending_h_pre_open.empty())
        log->warn("Network (RTA-10): run ended with {} withheld-epoch heartbeat(s) "
                  "never written — the run NEVER EMITTED, the deferred v2 product "
                  "writer never opened, and the product file does not exist.  Every "
                  "epoch was withheld; read the run log / traces, not the wire.",
                  setup.pending_h_pre_open.size());
    // Card DG6 — rank-gate calibration telemetry.  Gated on the tolerance being
    // ARMED (non-default) so a default run's log text is byte-identical, per DG5's
    // "with the trace on, the new FILE is the only artifact delta".  The same
    // numbers reach every trace-armed run through network_estimability.csv's RUN
    // record, so the control arm is not left dark.  Standing invariant #4: an
    // armed calibration whose counters never move is a MISWIRE, not a quiet run.
    if (config_.networkRankGateTol() != NetworkEstimator::kNullSpaceTol)
        log->info("Network (Card DG): rank gate evaluated on {} epoch(s) at tol={:g} — {} "
                  "emitted, {} withheld UNDER-specified (observed > declared: a missing "
                  "datum constraint), {} withheld OVER-specified (observed < declared: a "
                  "declared gauge stopped being counted).  Observed near-null count over "
                  "the run: min {}, max {} (declared dim is the target).",
                  ctx.counters.n_gate_evaluated, config_.networkRankGateTol(),
                  n_emitted, ctx.counters.n_gate_withheld_under,
                  ctx.counters.n_gate_withheld_over,
                  ctx.counters.nullspace_obs_min, ctx.counters.nullspace_obs_max);
    // The OVER-specified verdict has TWO causes that need OPPOSITE fixes, and the old
    // advice ("RE-CALIBRATE, do not chase the datum") named only one of them.  It is
    // provably WRONG for the other: when a pre-gate fold DEPOSITS information along a
    // declared gauge direction, that direction moves INTO the real layer, and no
    // tolerance recovers it — below the filled value it is still not counted as gauge,
    // above it you begin counting genuine parameter directions as gauge.  Measured on
    // simnet24: gauge depth 1e-11, carry-filled direction 9.43e-3, weakest-real
    // phase-bias layer 1.25e-2 — the admissible band is EMPTY, and the placement rule's
    // own "2 decades either side" cannot be met by any cutoff whatsoever.  The
    // invariant-#7 ratio is what tells the two apart, so the advice now branches on it
    // instead of asserting a cause.
    if (ctx.counters.n_gate_withheld_over > 0) {
        if (ctx.counters.ddi_over_breach > 0)
            log->warn("Network (Card DG): {} of the {} OVER-specified withhold(s) landed on an "
                      "epoch where a DECLARED datum direction was already carrying information "
                      "(invariant-#7 ratio >= 1; run max R={:.3e} on {} at TOW {:.1f}).  Those "
                      "are NOT a mis-calibrated tolerance and re-calibrating cannot fix them: "
                      "the direction has moved up into the real-parameter layer, so every "
                      "cutoff below it still fails to count it as gauge and every cutoff above "
                      "it starts counting real directions as gauge.  ATTRIBUTE THE FOLD "
                      "instead.  ⚠ The ratio is measured on the reduced normals AS THE GATE "
                      "SEES THEM, i.e. AFTER the station blocks (which carry the Card-1 "
                      "coupling rows), the clock-constraint rows, the SH prior, the phase hold, "
                      "the pin bridge and any SBC rows — but BEFORE the D2/C1 dense carry fold "
                      "and the clock-osc prior, which are applied later and are therefore NOT "
                      "observable in this number.  So the candidates it CAN indict are the "
                      "Card-1 coupling rows (a truncated family sum — arm "
                      "network.carry_coupling_gauge_project) and the pre-gate datum/constraint "
                      "rows.  The ddi_breach events in network_events.csv name the direction and "
                      "the epoch.",
                      ctx.counters.ddi_over_breach, ctx.counters.n_gate_withheld_over,
                      ctx.counters.ddi_ratio_max, ctx.counters.ddi_ratio_max_dir,
                      ctx.counters.ddi_ratio_max_tow);
        if (ctx.counters.n_gate_withheld_over > ctx.counters.ddi_over_breach)
            log->info("Network (Card DG): {} OVER-specified withhold(s) occurred with every "
                      "declared datum direction still free (invariant-#7 ratio < 1).  THOSE are "
                      "the calibration case — the tolerance sits below this config's own gauge "
                      "depth, so RE-CALIBRATE rather than chasing the datum.  [Placement rule: "
                      "cutoff >= 2 decades above the gauge depth and >= 2 decades below the "
                      "weakest-real layer, read off this config's own network_estimability.csv.]",
                      ctx.counters.n_gate_withheld_over - ctx.counters.ddi_over_breach);
    }
    // Invariant #7's own readout, independent of whether the gate ever withheld: a run
    // whose declared directions all sit decades below the cutoff has a genuinely free
    // datum, and that is worth stating positively rather than inferring from silence.
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs == 0)
        log->warn("Network (invariant #7): the ratio was reached on {} epoch(s) but sampled "
                  "ZERO directions — every declared group was below two registered clocks, or "
                  "its columns fell outside the reduced normals.  The instrument is INERT: "
                  "'0 breaches' says nothing about this run's datum.",
                  ctx.counters.ddi_epochs);
    if (ctx.counters.ddi_epochs > 0 && ctx.counters.ddi_dirs > 0)
        log->info("Network (invariant #7): datum-direction information ratio measured on {} "
                  "epoch(s), {} (epoch x direction) samples — R range [{:.3e}, {:.3e}], max on "
                  "{} at TOW {:.1f}; {} epoch(s) breached R >= 1 across {} edge(s).  R << 1 "
                  "means the datum still owns its gauge directions; R >= 1 means something "
                  "folded PRE-gate is pinning one.",
                  ctx.counters.ddi_epochs, ctx.counters.ddi_dirs,
                  ctx.counters.ddi_ratio_min, ctx.counters.ddi_ratio_max,
                  ctx.counters.ddi_ratio_max_dir.empty() ? "(none)"
                                                         : ctx.counters.ddi_ratio_max_dir,
                  ctx.counters.ddi_ratio_max_tow,
                  ctx.counters.ddi_breach_epochs, ctx.counters.ddi_breach_edges);
    // SBC (network.sbasis_complete) run-end telemetry.  Standing invariant #4
    // with the WE-CI twist: an armed completion that never fired on a
    // multi-constellation run means the deficiency it was built for never
    // materialized (fine) OR the block is miswired — the observed-count
    // envelope in the Card-DG line above tells the two apart (a max above the
    // declared dim with n_sbc_epochs == 0 is the miswire).  Refusals are
    // per-cause because each demands a different fix.
    if (ctx.knobs.sbasis_complete_on) {
        log->info("Network (SBC): S-basis completion constrained {} extra near-null "
                  "direction(s) on {} epoch(s) (max {}/epoch); {} release edge(s) "
                  "(satellite re-observed); {} unseparated direction(s) skipped to "
                  "the drop-and-heal (machine-zero orphan class).  Refused: {} "
                  "capped (> {} dirs — pathological epoch or rank_gate_tol counting "
                  "real directions), {} weight-marginal (K2).  Per-direction "
                  "attribution: network_events.csv tier GATE, events "
                  "sbc_engage/sbc_release/sbc_skip/sbc_refuse (network.trace_events).",
                  ctx.counters.n_sbc_dirs, ctx.counters.n_sbc_epochs,
                  ctx.counters.sbc_max_dirs, ctx.counters.n_sbc_release,
                  ctx.counters.n_sbc_degenerate,
                  ctx.counters.n_sbc_capped, ctx.knobs.sbasis_complete_max,
                  ctx.counters.n_sbc_weight_marginal);
        if (ctx.counters.n_sbc_epochs == 0 && ctx.counters.nullspace_obs_max
                                                  > ctx.counters.nullspace_obs_min)
            log->warn("Network (SBC): ARMED but never fired while the observed "
                      "near-null count moved ({}..{}) — if the max exceeds the "
                      "declared dimension the completion is being bypassed "
                      "(miswire), not idle.",
                      ctx.counters.nullspace_obs_min,
                      ctx.counters.nullspace_obs_max);
    }
    // DATUM GRAPH CENSUS (docs/SPEC_datum_graph_census.md) run-end telemetry.
    // Standing invariant #4 in the form this card owns: the EXPECTED healthy
    // reading is "k = 1 everywhere, zero predictions", which is byte-for-byte
    // what a seam that was never called produces.  So `epochs` and
    // `epochs_quiet` are BOTH printed, and a run that walked zero support edges
    // is called out as a miswire rather than reported as a clean network.
    if (ctx.knobs.dg_on) {
        const auto& dg = ctx.counters.dg;
        log->info("Network (datum graph): census ran {} epoch(s) — {} quiet (k = 1 on "
                  "every constellation AND zero tier-2 predictions), {} fragmented "
                  "(max k = {}), {} admitted support edge(s) walked.  PREDICTED: {} "
                  "tier-1 (k-1)*d deficiencies, {} tier-2 zero-code-key directions over "
                  "{} (sat, family) cell(s) ({} cell(s) had no code-bias layer, both "
                  "predicates held off); companion zero-support predicate {}.  OBSERVED: "
                  "{} extra direction(s) by count, {} with identities.  CONTINGENCY — "
                  "predicted AND observed {}, predicted ONLY {}, observed ONLY {} "
                  "(directions matching a prediction: {}); companion cells {} / {}.  "
                  "Events: {} emitted, {} held.  Cost {:.2f} ms total, {:.3f} ms worst "
                  "epoch (spec budget 0.5, hard limit 2.0).  MEASURE-ONLY: no weight, "
                  "state, gate, SBC or declared dimension was touched.",
                  dg.epochs, dg.epochs_quiet, dg.epochs_frag, dg.max_k, dg.edges,
                  dg.pred_tier1, dg.pred_tier2, dg.cells_tier2, dg.cells_no_cb_layer,
                  dg.pred_support, dg.obs_extras_count, dg.obs_dirs,
                  dg.cell_pred_obs, dg.cell_pred_only, dg.cell_obs_only,
                  dg.cell_obs_matched, dg.support_pred_obs, dg.support_pred_only,
                  dg.events, dg.events_held, dg.ms, dg.ms_max);
        if (dg.epochs == 0)
            log->warn("Network (datum graph): ARMED but never ran a single census pass — "
                      "the seam is not being reached (miswire), which is a different "
                      "finding from a quiet network.");
        else if (dg.edges == 0)
            log->warn("Network (datum graph): ran {} epoch(s) and walked ZERO admitted "
                      "support edges.  Tier 1 saw no graph at all, so its k = 1 reading "
                      "is vacuous — the support source (modeled observations of FOLDED "
                      "stations) is empty, not the network.",
                      dg.epochs);
        if (dg.epochs_count_only > 0)
            log->info("Network (datum graph): {} epoch(s) reconciled at COUNT level only "
                      "(extras observed, no identities) — arm network.sbasis_complete for "
                      "the per-direction attribution the contingency needs; without it "
                      "every prediction falls into `predicted only` for want of a "
                      "right-hand side.",
                      dg.epochs_count_only);
        if (static_cast<std::size_t>(std::max<long long>(dg.epochs, 0))
            < ctx.counters.n_gate_evaluated)
            log->info("Network (datum graph): {} of {} gate-evaluated epoch(s) never "
                      "reached the census — a WITHHELD epoch skips before SB_FRAMEDIAG, "
                      "so the epochs whose datum actually failed are precisely the ones "
                      "with no census row.  Read this line next to the Card-DG withhold "
                      "counts, not on its own.",
                      ctx.counters.n_gate_evaluated - static_cast<std::size_t>(dg.epochs),
                      ctx.counters.n_gate_evaluated);
        if (dg.ms_max > 2.0)
            log->warn("Network (datum graph): worst census pass {:.3f} ms exceeds the "
                      "card's 2.0 ms hard limit (§7) — the implementation is walking "
                      "something it should not.",
                      dg.ms_max);
    }
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
    // --- R0: within-epoch tier response census (SPEC_pivot_visibility_gauge_defect
    // §5.07.5).  Before this, only the WITHHOLD had a first-class run counter; the
    // refloats and exclusions existed solely as per-epoch T2 event rows.  The clock
    // move is the §5.07.2 impulse-channel meter — how far the tier's re-solves
    // moved a satellite clock off the PRIMARY solve — with its identity
    // (invariant #7: counters AND identities).  0 responses on an armed run is a
    // QUIET run, not dead wiring — n_adapt trips are logged per epoch, and the
    // carry_warm gate note on the WE-CI line above applies here identically.
    if (config_.networkRaimWithinEpoch()) {
        const auto& sc = ctx.counters;
        log->info("Network (T2 within-epoch responses): {} whole-sat refloat(s), {} "
                  "isolated-arc refloat(s), {} station exclusion(s) settled over the "
                  "run; {} epoch(s) withheld at the A4 ceiling.  Largest primary→final "
                  "clock move {:.3f} m ({} @ TOW {:.1f}) — the impulse the tier itself "
                  "injected into the emitted clock channel.",
                  sc.we_refloat_sat_total, sc.we_refloat_arc_total,
                  sc.we_exclude_sta_total, sc.n_sigma0_withheld,
                  sc.we_clk_move_max_m,
                  sc.we_clk_move_max_sat.empty() ? "none" : sc.we_clk_move_max_sat,
                  sc.we_clk_move_max_tow);
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
    // --- Adaptive iono q run-end telemetry (network.adaptive_iono_q) ---
    // Standing invariant #4: an armed run with zero boosts is either genuinely quiet
    // iono (feeds high, max multiplier ~1 — the monitor ran and said "calm") or DEAD
    // WIRING (feeds == 0) — the two are distinguished explicitly, never left to look
    // alike.
    if (config_.getNetworkConfig().adaptive_iono_q) {
        long long aiq_fed = 0;
        for (const auto& st : stations) aiq_fed += st.iono_var_epochs_fed;
        const auto& aiq_cfg = config_.getAdaptiveIonoQConfig();
        log->info("Network (adaptive iono q): {} station-epoch monitor feed(s); {} GM "
                  "deviation innovation boost(s) applied, max multiplier {:.1f}x "
                  "(tuning recent/baseline/min/max = {}/{}/{}/{:.0f} from "
                  "estimation.adaptive_iono_process_noise).",
                  aiq_fed, ctx.counters.aiq_boosts_total, ctx.counters.aiq_max_mult,
                  aiq_cfg.recent_window, aiq_cfg.baseline_window, aiq_cfg.min_epochs,
                  aiq_cfg.max_multiplier);
        if (aiq_fed == 0)
            log->warn("Network (adaptive iono q): ARMED but ZERO monitor feeds — the "
                      "wiring never ran (invariant #4: a clean log over dead wiring is "
                      "not 'no effect').");
    }

    // --- TWO-EDGE CONTINUOUS HANDOVER run-end telemetry -------------------------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.1).  Standing invariant #4: the
    // DENOMINATOR is `reattaches` — how many gauge changes the run made at all — and
    // it is reported whether or not the fix is armed, because a run that made zero
    // handovers proves nothing about the mechanism.  The two edges are then split:
    // `handover_continuous` are the frame-preserving ones, `handover_cold` the
    // unavoidable re-realizations, and a cold one is NOT a failure — it is the
    // honest report of a step the consumer must be told about.
    if (ctx.knobs.pd_handover_continuous) {
        const auto& h = ctx.counters.hyg;
        const long long total = h.handover_continuous + h.handover_cold;
        log->info("Network (PB-GAUGE HANDOVER, two-edge): {} anchor handover(s) over the "
                  "run — {} made CONTINUOUS (pinned at the successor's own value in the "
                  "outgoing frame; largest |target| {:.4f} m, which is the size of the "
                  "frame step that was AVOIDED) and {} COLD (no ledger entry for the "
                  "successor — a fresh arc or the first epoch — pinned at 0, so the "
                  "frame DID step there); {} of the cold were REFUSED ledger values "
                  "(conditional variance above the {} m² ceiling — the invariant-#7 "
                  "re-validation, not a miss).",
                  h.reattaches, h.handover_continuous, h.handover_max_abs_m,
                  h.handover_cold, h.handover_refused_var,
                  ctx.knobs.pd_handover_max_var);
        if (ctx.knobs.pd_handover_max_var_ratio > 0.0)
            log->info("Network (PB-GAUGE HANDOVER, ratio ceiling): {} ledger value(s) REFUSED by "
                      "the honest-sigma-scale test var <= {} x var_first (a subset of the cold "
                      "count, disjoint from the {} absolute-ceiling refusals; an unknown "
                      "var_first counts here).",
                      h.handover_refused_ratio, ctx.knobs.pd_handover_max_var_ratio,
                      h.handover_refused_var);
        if (h.reattaches > 0 && total == 0)
            log->warn("Network (PB-GAUGE HANDOVER): DEAD WIRING — {} handover(s) were "
                      "elected but NONE reached the retarget block.  The knob reports "
                      "ARMED and every pin still went in at 0, so every one of those "
                      "gauge changes re-realized the frame.",
                      h.reattaches);
        else if (total > 0 && h.handover_continuous == 0)
            log->warn("Network (PB-GAUGE HANDOVER): {} handover(s) and NOT ONE was "
                      "continuous — every successor was missing from the ledger.  That "
                      "is the harvest not running (the census `qc_amb` is empty, or the "
                      "solve reconstruction failed), not a property of the data.",
                      total);
        else if (h.reattaches == 0)
            log->info("Network (PB-GAUGE HANDOVER): ZERO handovers this run — the anchor "
                      "set never moved, so this arm exercises the mechanism NOT AT ALL.  "
                      "That is a quiet window, not evidence the fix works.");
    }

    // --- QUEUE item 1(d): PHASE_ANCHOR carry re-reference run-end telemetry -----
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  Same invariant-#4
    // discipline as the handover block above: `reattaches` is the denominator, and
    // an armed run whose election handed over but whose set never populated is
    // DEAD WIRING, called out explicitly.
    if (ctx.knobs.pa_carry_reref) {
        const auto& h = ctx.counters.hyg;
        log->info("Network (PHASE_ANCHOR carry reref, 1(d)): {} marked "
                  "(sat,signal) family-epoch(s) — handovers plus incumbent-less "
                  "pins (first sightings / bridge exits; no-ops without a "
                  "surviving carry) — marginalized for one epoch over the run "
                  "({} anchor handover(s) elected); {} phase-leveling local "
                  "carry(ies) erased.",
                  ctx.carry.pa_reref_total, h.reattaches,
                  ctx.carry.pa_reref_erase_total);
        if (h.reattaches > 0 && ctx.carry.pa_reref_total == 0)
            log->warn("Network (PHASE_ANCHOR carry reref): DEAD WIRING — {} "
                      "handover(s) were elected but the re-reference set never "
                      "populated, so every carried PB prior still folded in its "
                      "retired frame (the exact §5.07 defect this knob exists to "
                      "close).",
                      h.reattaches);
        else if (h.reattaches == 0)
            log->info("Network (PHASE_ANCHOR carry reref): ZERO handovers this run — "
                      "the mechanism was never exercised.  A quiet window, not "
                      "evidence.");
    }

    // --- eq34 reattach hygiene run-end telemetry -------------------------------
    // (docs/SPEC_eq34_reattach_hygiene.md).  Standing invariant #4: an armed run whose
    // gate never RAN is a different failure from one that ran and found nothing to
    // block, so the denominators (`epochs`, `arcs_tracked`) are reported first and an
    // all-zero feed is called out explicitly rather than left to look quiet.
    if (ctx.knobs.anchor_hygiene_on) {
        const auto& h = ctx.counters.hyg;
        log->info("Network (eq34 hygiene): {} election pass(es), {} arc-epoch(s) aged; "
                  "{} reattach(es) performed, {} BLOCKED cold (min_arc_ep {}) held for "
                  "{} key-epoch(s), {} released on a converged host; {} unavoidable "
                  "fresh pin(s) taken for connectivity, {} sigma-ramped pin(s).",
                  h.epochs, h.arcs_tracked, h.reattaches, h.reattach_blocked_cold,
                  ctx.knobs.eq34_min_arc_ep, h.blocked_key_epochs, h.blocks_released,
                  h.cold_fallback_pins, h.fresh_pin_ramps);
        log->info("Network (eq34 hygiene): {} cold key-epoch(s) ALLOWED with no fresh "
                  "prior realization (warm-conditioning horizon {:.0f} s) — the "
                  "restart / long-outage path, where a cold pin has no warm memory to "
                  "fight and blocking would only extend the outage.  A cold-start run "
                  "is EXPECTED to show a large number here and zero blocks.",
                  h.reattach_cold_allowed_no_prior,
                  ctx.knobs.prior_realization_max_gap_s);
        if (h.reattach_blocked_cold > 0 && h.blocks_released == 0)
            log->warn("Network (eq34 hygiene): {} hold(s) entered and NONE released — "
                      "every held-out key stayed out for the whole run.  That is only "
                      "correct if those arcs never reached {} continuous epochs; "
                      "otherwise the release path is not firing and the guard is "
                      "silently withholding columns.",
                      h.reattach_blocked_cold, ctx.knobs.eq34_min_arc_ep);
        if (ctx.knobs.pin_bridge_on)
            log->info("Network (eq34 hygiene INC-2): {} bridge(s) opened, {} row-fold(s) "
                      "(sigma {:g} m, margin {:.1f} decades over the rank-gate cutoff, "
                      "max {} epoch(s) each); dropped slip {} / datum {} / stale {} / "
                      "left_set {}, healed {}.",
                      h.bridges_opened, h.bridge_folds, ctx.knobs.pin_bridge_sigma,
                      h.bridge_margin_decades, ctx.knobs.pin_bridge_max_ep,
                      h.bridge_drop_slip, h.bridge_drop_datum, h.bridge_drop_stale,
                      h.bridge_drop_leftset, h.bridge_drop_healed);
        if (h.epochs == 0)
            log->warn("Network (eq34 hygiene): ARMED but the seam ran on ZERO epochs — "
                      "the wiring never executed (invariant #4).  Check that a "
                      "prioritized / eq34 phase datum is actually active.");
        else if (h.arcs_tracked == 0)
            log->warn("Network (eq34 hygiene): ARMED and the seam ran, but it aged ZERO "
                      "arcs — every eligibility answer was the default, so the gate is "
                      "INERT despite looking live (invariant #4).");
        else if (h.reattaches == 0 && h.reattach_blocked_cold == 0)
            log->info("Network (eq34 hygiene): no reattach occurred this run — the "
                      "guard had zero opportunity, which is NOT evidence that it works "
                      "(grade it on a run that contains a handover).");
        if (ctx.knobs.pin_bridge_on && h.bridges_opened == 0)
            log->warn("Network (eq34 hygiene INC-2): ARMED but ZERO bridges opened — "
                      "either no handover needed one, or no solved level was ever "
                      "harvested (pb_level_prev empty).  With {} blocked-cold "
                      "reattach(es) recorded the second is the likely cause.",
                      h.reattach_blocked_cold);
    }

    // --- RAIM POOL RE-HOST run-end telemetry -----------------------------------
    // (docs/SPEC_raim_pool_rehost.md).  Standing invariant #4, stated sharply because a
    // silently-inert tier is the exact defect this card fixes: post-fit RAIM read ARMED
    // in the loader summary on every Card-4 lineage while screening literally nothing.
    // The denominators come FIRST so that four previously-identical-looking outcomes are
    // distinguishable — seam never ran / census empty / nothing flagged / actuator off.
    if (ctx.knobs.raim_ci_on) {
        const auto& rc = ctx.counters.raim_ci;
        const double s0_mean = (rc.sigma0_n > 0)
                                   ? rc.sigma0_sum / static_cast<double>(rc.sigma0_n)
                                   : 0.0;
        log->info("Network (RAIM pool re-host): {} seam pass(es), {} station-epoch(s) with a "
                  "live census, {} ambiguity cell(s) screened.",
                  rc.epochs, rc.pool_stations, rc.pool_cells);
        if (rc.epochs > 0 && rc.pool_cells == 0)
            log->warn("Network (RAIM pool re-host): DEAD WIRING — the seam ran {} time(s) and "
                      "the qc_amb census was EMPTY every time, so the screen still saw "
                      "nothing.  This is the defect the card exists to fix presenting itself "
                      "again: the census walk found no station with an unpinned ambiguity "
                      "column present in A_r, so check the gauge-pin partition and the "
                      "station/signal admission before trusting any RAIM verdict here.",
                      rc.epochs);
        if (rc.sigma0_n > 0)
            log->info("Network (RAIM pool re-host): σ̂0 over {} live-pool epoch(s) — mean "
                      "{:.3f}, max {:.3f}.  FIRST measurement of this statistic on the "
                      "invariant-carry lineage: with the pool empty σ̂0 never left its 1.0 "
                      "initializer, so any previously-quoted RAIM threshold on this lineage "
                      "was calibrated against a constant.",
                      rc.sigma0_n, s0_mean, rc.sigma0_max);
        log->info("Network (RAIM pool re-host): {} satellite-epoch(s) consensus-flagged, {} "
                  "isolated arc-epoch(s) attribution-flagged.",
                  rc.sats_flagged, rc.arcs_flagged);
        if (rc.pool_cells > 0 && rc.sats_flagged == 0 && rc.arcs_flagged == 0)
            log->info("Network (RAIM pool re-host): ZERO OPPORTUNITY — the screen ran on a "
                      "live census and nothing exceeded the gate.  That is a clean window, "
                      "NOT evidence the screen works: its POWER under Card 4 is unproven "
                      "(the local ambiguity column is free each epoch and the time-link "
                      "lives in the DD chord's prior, so a slipped arc's tension may be "
                      "absorbed locally and never reach this residual).  Grade against a "
                      "known-slip population before crediting it.");
        if (ctx.knobs.raim_ci_refloat)
            log->info("Network (RAIM pool re-host): {} arc(s) queued into the deferred funnel "
                      "and {} delivered at the following epoch's head.  NOTE the seam's two "
                      "legacy actions are BOOKKEEPING here, not actuators: the local_carry "
                      "erase drops an entry no Card-4 consumer reads, and the pending-reseed "
                      "mark leaves the reduced shared RHS algebraically unchanged.  The "
                      "funnel drain is the only thing that re-taps a chord.",
                      rc.refloats_queued, rc.refloats_drained);
        else if (rc.sats_flagged > 0 || rc.arcs_flagged > 0)
            log->warn("Network (RAIM pool re-host): the screen flagged {} sat-epoch(s) / {} "
                      "arc-epoch(s) but network.raim_carry_invariant_refloat is OFF — those "
                      "flags reached NO carry consumer.  Measure-only is the intended first "
                      "stage; this line exists so the run is never mistaken for a protected "
                      "one.",
                      rc.sats_flagged, rc.arcs_flagged);
        if (rc.refloats_queued != rc.refloats_drained)
            log->warn("Network (RAIM pool re-host): {} queued vs {} drained — a mismatch means "
                      "the run ended with a queue still pending (expected: at most the final "
                      "epoch's flags) or that a drain was skipped, which would break the "
                      "one-epoch lifetime bound.",
                      rc.refloats_queued, rc.refloats_drained);
    }

    // --- iono-deweight RELEASE hygiene run-end telemetry -----------------------
    // (docs/SPEC_deweight_release_hygiene.md).  Standing invariant #4, with a twist
    // this card has to state out loud: at the event it was SPEC'd against, the
    // station carrying the parked column was never deweighted, so INC-1 / INC-2 have
    // ZERO OPPORTUNITY there by construction.  A run reporting zero releases is
    // therefore NOT evidence that the release response works or fails — the
    // denominators (`epochs`, `gate_advances`, `releases_seen`) are reported first so
    // "the seam never ran", "it ran and the gate never advanced", and "the gate
    // advanced and nothing ever released" are three distinguishable answers.
    if (ctx.knobs.dw_hygiene_on) {
        const auto& d = ctx.counters.dwr;
        log->info("Network (dw release hygiene): {} seam pass(es), {} gate advance(s), "
                  "{} station release(s) seen.",
                  d.epochs, d.gate_advances, d.releases_seen);
        if (ctx.knobs.dw_release_reseed)
            log->info("Network (dw release hygiene INC-1): {} station(s) reseeded — {} "
                      "slant state(s) rewritten from the field, {} deviation stash "
                      "entr(ies) erased, {} ambiguity refloat(s) queued and {} "
                      "delivered into the funnel.  NOTE the slant rewrite is "
                      "BOOKKEEPING, not an actuator: the producer forms every "
                      "innovation against the linearization point, so shifting it "
                      "leaves the reduced shared RHS algebraically unchanged.  INC-1's "
                      "actuators are the stash erase and the refloat.",
                      d.reseed_stations, d.reseed_slants, d.reseed_stash_erased,
                      d.reseed_refloats, d.refloats_drained);
        if (ctx.knobs.dw_park_bound_tecu > 0.0)
            log->info("Network (dw release hygiene INC-2): {} park-bound reset(s) while "
                      "DECOUPLED + {} while the gate was COLD (bound {:g} slant TECU; "
                      "largest reset {:.4g} slant TECU).",
                      d.park_reset_decoupled, d.park_reset_cold,
                      ctx.knobs.dw_park_bound_tecu, d.park_max_tecu);
        if (ctx.knobs.dw_release_validate_k > 0.0)
            log->info("Network (dw release hygiene INC-3): {} tie-row LOS-epoch(s) held "
                      "on the release screen (k = {:g}); {} hold(s) entered, {} released "
                      "on a LOS that finally passed.",
                      d.rows_held, ctx.knobs.dw_release_validate_k,
                      d.rows_held_entered, d.rows_released);
        if (ctx.knobs.dw_admit_validate_k > 0.0) {
            log->info("Network (dw release hygiene INC-3b): {} tie-row LOS-epoch(s) held "
                      "pending admission (k = {:g}), {} LOS admitted, {} REJECTED on a "
                      "harvested deviation over k.  Every rising satellite serves ONE "
                      "quarantine epoch, so `held` scales with the constellation churn; "
                      "only `rejected` is the guarded event.  {} LOS were admitted "
                      "UNSCREENED by the one-shot start/resume seed — on a short leg "
                      "that number being large relative to `admitted` means most of the "
                      "run's tie rows were never screened at all.  {} more were "
                      "admitted with NO deviation ever harvested (decoupled station or "
                      "no local column) — the guard failing open on absent evidence "
                      "rather than starving the row for the whole run.",
                      d.admit_held, ctx.knobs.dw_admit_validate_k,
                      d.admit_admitted, d.admit_rejected, d.admit_seeded,
                      d.admit_nodev);
            if (d.admit_admitted == 0)
                log->warn("Network (dw release hygiene INC-3b): ARMED but ZERO LOS were "
                          "ever admitted — every tie row in the run was withheld.  That "
                          "is a rank-starved solve, not a guard; check k and the "
                          "representativeness sigma (invariant #4).");
            else if (d.admit_rejected == 0)
                log->info("Network (dw release hygiene INC-3b): no LOS was ever rejected "
                          "— the screen had opportunity ({} quarantines) and found "
                          "nothing over k.  Loosen nothing on that basis; grade it on a "
                          "run that contains a known coupling event.", d.admit_held);
        }
        if (d.epochs == 0)
            log->warn("Network (dw release hygiene): ARMED but the seam ran on ZERO "
                      "epochs — the wiring never executed (invariant #4).");
        else if (d.gate_advances == 0 && ctx.knobs.dw_release_reseed)
            log->warn("Network (dw release hygiene): ARMED and the seam ran, but the "
                      "deweight gate NEVER advanced — no release edge can exist, so "
                      "INC-1 is inert regardless of what the other counters say.  Check "
                      "network.iono_constraint_deweight_adaptive.");
        else if (d.releases_seen == 0 && ctx.knobs.dw_release_reseed)
            log->info("Network (dw release hygiene INC-1): the gate advanced on {} "
                      "epoch(s) and no station ever released — the response had ZERO "
                      "OPPORTUNITY this run, which is NOT evidence that it works.  Grade "
                      "it only on a run containing a release (the DOY-129 day had 22 "
                      "engage / 15 release events; the 19:26 break was NOT one of them).",
                      d.gate_advances);
    }

    // --- flicker/gap signal integrity run-end telemetry ------------------------
    // (docs/SPEC_flicker_gap_integrity.md).  Standing invariant #4: the classifier
    // is the ON-path telemetry for this card — a clean-looking run with zero
    // classified lines of sight is DEAD WIRING, not a quiet network, and only the
    // denominators can tell those apart.  The demoted-ROW count is summed from the
    // per-station counters here because the fold runs inside the PARALLEL produce:
    // a live map with zero demoted rows means the map was WRITTEN and never READ,
    // which is the exact live-counters-dead-wiring trap the rx-CB carry card paid
    // for.  The census block reports the INC-0 audit answer explicitly rather than
    // leaving a reader to infer it from two raw counts.
    if (ctx.knobs.sig_integrity_on) {
        auto& s = ctx.counters.sig;
        for (const auto& st : stations) s.demoted_rows += st.si_rows_demoted;
        log->info("Network (signal integrity): {} classifier pass(es), {} line-of-sight "
                  "epoch(s) classified; {} first sighting(s), {} re-entry(ies) "
                  "(gap 1 / 2-5 / >5 = {} / {} / {}), {} LOS disappearance(s) of which "
                  "{} were DUAL at their last epoch.",
                  s.epochs, s.los_epochs, s.new_arcs, s.reentries, s.reentry_gap_1,
                  s.reentry_gap_2_5, s.reentry_gap_gt5, s.los_vanished,
                  s.los_vanished_dual);
        log->info("Network (signal integrity BAND census): {} band loss(es) while the "
                  "LOS stayed present, {} regained inside {} epoch(s) (FLICKER), {} "
                  "dropped from the reference set as permanently gone, {} band "
                  "gain(s); {} degraded LOS-epoch(s); {} code-only LOS-epoch(s).",
                  s.band_lost, s.band_regain, ctx.knobs.sig_flicker_max_ep,
                  s.band_dropped_perm, s.band_gained, s.degraded_epochs,
                  s.no_phase_epochs);
        // THE INC-0 AUDIT ANSWER.  The spec asks whether a DUAL-born arc that
        // degrades mid-arc ever CONTINUES single-frequency (an unmonitorable phase
        // at full weight, no gap registered).  The two numbers are reported together
        // and interpreted in-line, because either one alone is ambiguous.
        log->info("Network (signal integrity AUDIT — spec INC-0): {} LOS-epoch(s) in "
                  "which an EVER-DUAL arc contributed with fewer than two phase "
                  "bands, against {} disappearance(s) of a dual arc.  A large first "
                  "number means the producer CONTINUES a degraded arc "
                  "single-frequency; a large second with a small first means it drops "
                  "the line of sight instead.  Separately, {} arc(s) were never once "
                  "observed dual ({} LOS-epoch(s)) — the standing single-frequency "
                  "class, whose permanent-demotion policy this card deliberately "
                  "leaves open (spec section 7).",
                  s.sf_continuation_epochs, s.los_vanished_dual, s.standing_sf_arcs,
                  s.standing_sf_epochs);
        log->info("Network (signal integrity INC-2): {} unvetted LOS-epoch(s) "
                  "(re-vet window {} epoch(s), {} completion(s)), of which {} were "
                  "EXEMPT as never-converged arcs and {} were carried by the RESUME "
                  "WARMUP; {} deweighted LOS-epoch(s) driving {} PHASE row(s) at "
                  "sigma x {:g}; {} withheld LOS-epoch(s) removing {} phase "
                  "observable(s) and pruning {} phase-bias state(s).  CODE rows were "
                  "never touched.",
                  s.unvetted_los_epochs, ctx.knobs.sig_revet_ep, s.vet_completions,
                  s.birth_exempt_los_epochs, s.resume_warmup_los_epochs,
                  s.demoted_los_epochs, s.demoted_rows, ctx.knobs.sig_unvetted_scale,
                  s.withheld_los_epochs, s.withheld_rows, s.withheld_pb_pruned);
        log->info("Network (signal integrity INC-3): {} flicker-regain refloat(s) -> "
                  "{} funnel insertion(s) + {} pending-reseed(s); re-entry premise "
                  "ASSERTED: {} already refloated by a PASS-A detector vs {} NOT, "
                  "with {} carrying a surviving ambiguity STATE; LLI on a re-entry "
                  "epoch {} of which {} reached a refloat funnel.",
                  s.refloat_regain, s.refloat_regain_ambs, s.refloat_regain_reseeds,
                  s.reentry_already_refloated, s.reentry_unrefloated,
                  s.reentry_amb_state_live, s.lli_at_reentry,
                  s.lli_at_reentry_funnelled);
        // --- invariant #4: distinguish "ran and found nothing" from "never ran" ---
        if (s.epochs == 0)
            log->warn("Network (signal integrity): ARMED but the classifier ran on ZERO "
                      "epochs — the wiring never executed (invariant #4).  Nothing in "
                      "this run is evidence about the card.");
        else if (s.los_epochs == 0)
            log->warn("Network (signal integrity): ARMED and the classifier ran, but it "
                      "classified ZERO lines of sight — every verdict was the default, "
                      "so the census is INERT despite looking live (invariant #4).");
        if (ctx.knobs.sig_unvetted_scale > 1.0 && s.demoted_los_epochs > 0
            && s.demoted_rows == 0)
            log->warn("Network (signal integrity INC-2): {} LOS-epoch(s) published a "
                      "sigma scale and ZERO phase rows were folded at it — the map is "
                      "WRITTEN and never READ.  That is live counters over dead wiring "
                      "(invariant #4), not a quiet run: check that the demotion hook "
                      "in networkSplitStation_ is compiled in and that the satellite "
                      "keys match em.satellite_ids.",
                      s.demoted_los_epochs);
        if (ctx.knobs.sig_refloat_regain && s.refloat_regain > 0
            && s.refloat_regain_ambs == 0 && s.refloat_regain_reseeds == 0)
            log->warn("Network (signal integrity INC-3): {} regain(s) fired and NOT ONE "
                      "ambiguity was reset — neither refloat feed is live (the funnel "
                      "insert needs network.carry_invariant, the pending-reseed needs "
                      "a warm network.phase_leveling).  The regains are being COUNTED "
                      "and nothing is happening.",
                      s.refloat_regain);
        if (s.reentry_amb_state_live > 0)
            log->warn("Network (signal integrity): {} re-entry(ies) arrived with a "
                      "SURVIVING ambiguity state, which should be structurally "
                      "impossible (ensureRequiredStates prunes an ambiguity the epoch "
                      "its phase observable is absent, with no grace).  The card's "
                      "re-entry reasoning assumes that prune; re-derive it before "
                      "trusting any re-entry verdict from this run.",
                      s.reentry_amb_state_live);
        if (s.lli_at_reentry > s.lli_at_reentry_funnelled)
            log->warn("Network (signal integrity INC-3c): {} of {} loss-of-lock "
                      "assertion(s) raised ON a re-entry epoch did NOT reach a refloat "
                      "funnel.  The MW/LLI drain matches an assertion against the "
                      "StateManager's ambiguities, and a re-entering arc's ambiguity "
                      "has just been pruned — so the receiver said 'loss of lock', "
                      "nothing was marked, and any carried invariant-DD chord for that "
                      "arc (keyed independently of the StateManager) was not re-tapped. "
                      "This card MEASURES that hole and deliberately does not close it.",
                      s.lli_at_reentry - s.lli_at_reentry_funnelled, s.lli_at_reentry);
        if (s.reentry_unrefloated > 0)
            log->warn("Network (signal integrity INC-3a): {} of {} re-entry(ies) were "
                      "NOT already refloated by a PASS-A detector.  The spec's premise "
                      "is that a true re-entry already refloats via the funnel (Card G "
                      "measured 0 bridged); this run FALSIFIES it at that rate.  Per "
                      "spec section 2 this card asserts the premise and does not act "
                      "on it — report the number, do not read it as a defect this card "
                      "left unfixed.",
                      s.reentry_unrefloated, s.reentries);
        if (s.arcs_forgotten > 0)
            log->info("Network (signal integrity): {} LOS record(s) forgotten after a "
                      "very long absence (memory bound) — their eventual return is "
                      "classified as a first sighting rather than a re-entry.",
                      s.arcs_forgotten);
    }

    // --- prior-residual census run-end telemetry ------------------------------
    // (docs/SPEC_prior_residual_census.md §6).  Standing invariant #4, ladder
    // edition: THE LADDER IS THE TELEMETRY, and a family with n_rows = 0 must
    // read as "ran, saw nothing" rather than "never ran".  Body in
    // filter/NetworkPriorResidual.cpp, next to the emission that produced it.
    if (ctx.knobs.prires_on)
        networkPriorResidualReport(ctx, config_, log, setup.stations, prires_est);

    // --- redundancy / testability census run-end telemetry --------------------
    // (docs/SPEC_redundancy_census.md).  Standing invariant #4, statistical
    // edition.  The claim this card makes is "the cross-flag has essentially zero
    // false-positive surface", and the ONLY way a reader can check it here is to
    // see the denominators next to the findings: how many passes, how many
    // sweeps, how many ROWS, how many were uncheckable, and how many of THOSE
    // were also hot.  A run reporting 0 hot cells over 0 swept rows says nothing
    // about the network; a run reporting 0 hot cells over 400k swept rows with
    // 2k uncheckable ones is a real negative result.  Reported in that order.
    if (ctx.knobs.redun_census_on) {
        const auto& q = ctx.counters;
        log->info("Network (redundancy census): {} seam pass(es), {} sweep epoch(s) "
                  "({} full / {} targeted, {} forced by a flight-recorder trip), {} "
                  "station-sweep(s), {} escalation(s) taken; {:.1f} ms total wall "
                  "({:.2f} ms per station-sweep).",
                  q.redun_passes, q.redun_sweeps, q.redun_sweeps_full,
                  q.redun_sweeps_targeted, q.redun_sweeps_trip, q.redun_station_sweeps,
                  q.redun_escalations, q.redun_wall_ms,
                  (q.redun_station_sweeps > 0)
                      ? q.redun_wall_ms / static_cast<double>(q.redun_station_sweeps)
                      : 0.0);
        log->info("Network (redundancy census INC-1): {} DATA row(s) examined ({} rows "
                  "total incl. pins/coupling), redundancy mass {:.1f} (mean r = {:.4f}); "
                  "{} UNCHECKABLE row(s) at r < {:g} ({:.3f}% of data rows), of which {} "
                  "ALSO exceeded s > {:g} — the CROSS-FLAG.  {} event(s) emitted after "
                  "the {}-pass per-cell holdoff.",
                  q.redun_rows, q.redun_rows_all, q.redun_mass,
                  (q.redun_rows > 0) ? q.redun_mass / static_cast<double>(q.redun_rows)
                                     : 0.0,
                  q.redun_uncheckable, nc.redundancy_eps,
                  (q.redun_rows > 0) ? 100.0 * static_cast<double>(q.redun_uncheckable)
                                             / static_cast<double>(q.redun_rows)
                                     : 0.0,
                  q.redun_uncheckable_hot, nc.redundancy_hot_sigma, q.redun_events,
                  std::max(0, nc.redundancy_event_holdoff_ep));
        if (q.redun_uncheckable_hot > 0)
            log->warn("Network (redundancy census): WORST cross-flagged cell was {} at "
                      "s = {:.4e} — an UNCHECKABLE row carrying a {:.0f}-sigma centred "
                      "prefit deviation.  Its misfit is invisible to post-fit RAIM, to "
                      "sigma0 and to robust weighting by construction (MDB ~ 1/sqrt(r)), "
                      "so nothing downstream would have flagged it.  This is the parked-"
                      "garbage signature; read it against the topology at that epoch.",
                      q.redun_max_hot_id, q.redun_max_hot_s, q.redun_max_hot_s);
        // The prefer-fallback control.  Uncheckable-but-quiet rows are exactly the
        // healthy single-frequency risers the directive says to KEEP; naming the
        // worst one turns "we counted them and none was hot" into a claim a reader
        // can check rather than one they have to take on faith.
        if (q.redun_uncheckable > q.redun_uncheckable_hot)
            log->info("Network (redundancy census): {} uncheckable row(s) stayed BELOW "
                      "the cross-flag and were counted, never evented and never "
                      "weight-touched (prefer-fallback directive).  Worst of them: {} at "
                      "s = {:.4e}.",
                      q.redun_uncheckable - q.redun_uncheckable_hot,
                      q.redun_max_cold_id.empty() ? std::string("(none finite)")
                                                  : q.redun_max_cold_id,
                      q.redun_max_cold_s);
        log->info("Network (redundancy census INC-2): {} local column(s) examined, {} "
                  "column-epoch(s) below the {:g} x median pivot floor; {} DECOUPLING "
                  "edge(s) evented and {} RE-COUPLING edge(s) counted.",
                  q.redun_pivot_cols, q.redun_pivot_flagged, nc.redundancy_pivot_eps,
                  q.redun_unobs_entered, q.redun_unobs_cleared);
        // Closure: trace(H) over the swept rows may never exceed the free
        // parameters they span.  A negative slack means the covariance or the row
        // stack does not describe the system that was solved, which invalidates
        // every r_i above — so it is reported as a WARN, not buried.
        if (q.redun_closure_any)
            log->info("Network (redundancy census): closure worst slack "
                      "n_free - trace(H) = {:.4e} over {} sweep(s) (must be >= 0); {} "
                      "row(s) hit the kernel's r in [0,1] clamp at 0, and {} centring "
                      "pool(s) were skipped for population < {}.",
                      q.redun_closure_min_slack, q.redun_sweeps, q.redun_r_clamped,
                      q.redun_pool_skips, kCensusMinPoolRows);
        if (q.redun_closure_neg > 0)
            log->warn("Network (redundancy census): {} sweep(s) VIOLATED the closure "
                      "bound.  Every redundancy number in this run is untrusted — do not "
                      "grade the cross-flag on it.", q.redun_closure_neg);
        // --- invariant #4: separate "ran and found nothing" from "never ran" ---
        if (q.redun_passes == 0)
            log->warn("Network (redundancy census): ARMED but the seam ran on ZERO "
                      "epochs — the wiring never executed.  Nothing in this run is "
                      "evidence about the card.");
        else if (q.redun_sweeps == 0)
            log->warn("Network (redundancy census): ARMED and the seam ran on {} "
                      "epoch(s), but NOT ONE sweep completed — with stride {} and "
                      "escalation {}, either no epoch matched the stride phase or every "
                      "rowRedundancy call threw.  The census is INERT despite looking "
                      "live.", q.redun_passes, nc.redundancy_census_stride,
                      (nc.redundancy_escalate_sigma > 0.0) ? "armed" : "off");
        else if (q.redun_rows == 0)
            log->warn("Network (redundancy census): ARMED, {} sweep(s) ran and ZERO DATA "
                      "rows were examined — every row classified as a pin or a coupling "
                      "row, or every station was skipped on a dimension mismatch.  The "
                      "statistic has no domain.", q.redun_sweeps);
        else if (q.redun_uncheckable == 0)
            log->info("Network (redundancy census): {} data row(s) swept and NOT ONE was "
                      "uncheckable at r < {:g}.  That is a genuine negative result — this "
                      "network's topology cross-checked everything it folded — and it is "
                      "the state in which the cross-flag CANNOT fire, so do not read a "
                      "quiet run as validation of the alarm.",
                      q.redun_rows, nc.redundancy_eps);
        // The escalation path needs BOTH numbers to be readable.  "0 escalations"
        // alone is ambiguous and, under stride 1, GUARANTEED — the scheduled sweep
        // already covered every station, so no latch could ever need acting on.
        // Reporting only the action would be a conclusion the counters do not
        // support, which is the failure this project calls a live-counter over
        // dead wiring in reverse.
        if (nc.redundancy_escalate_sigma > 0.0) {
            if (q.redun_escalate_flagged == 0)
                log->info("Network (redundancy census): the free tier ran and NEVER "
                          "flagged a station — no station's centred prefit deviation "
                          "reached {:g} all run.  The targeted path had ZERO "
                          "opportunity, which is NOT evidence that it works.",
                          nc.redundancy_escalate_sigma);
            else if (q.redun_escalations == 0)
                log->info("Network (redundancy census): the free tier raised {} "
                          "station-epoch latch(es) over {:g} and NONE became a targeted "
                          "sweep — expected at stride {} , where the scheduled sweep "
                          "already covers every station.  Grade the targeted path only "
                          "on a stride-0 arm.",
                          q.redun_escalate_flagged, nc.redundancy_escalate_sigma,
                          nc.redundancy_census_stride);
            else
                log->info("Network (redundancy census): the free tier raised {} "
                          "station-epoch latch(es) over {:g}, of which {} became a "
                          "targeted single-station sweep the stride would have missed.",
                          q.redun_escalate_flagged, nc.redundancy_escalate_sigma,
                          q.redun_escalations);
        }
    }

    // --- Baarda reliability numbers / MDB run-end telemetry -------------------
    // (docs/SPEC_redundancy_census.md §11.4).  Standing invariant #4 in its
    // reliability form: the aggregates must distinguish "ran, all rows checkable"
    // from "never ran", and the two are reported by the SAME line only if the
    // denominators come first.  Body in filter/NetworkMdb.cpp, next to the
    // aggregation that produced the numbers.
    if (ctx.knobs.mdb_on)
        networkMdbReport(ctx, config_, log);

    // --- BLOCKFORMAL LOS-block census run-end telemetry ----------------------
    // (docs/SPEC_redundancy_census.md §10.4).  Standing invariant #4 in this card's
    // form: "the block tier found no smearing" and "there was never a block to
    // smear across" produce identical ladders, so the BLOCK-SIZE HISTOGRAM is
    // reported before any occupancy and INC-3's fraction is reported last, with
    // its denominator.  Body in filter/NetworkBlockBaarda.cpp, next to the sweep
    // that produced the numbers.
    if (ctx.knobs.bb_on)
        networkBlockBaardaReport(ctx, log, setup.stations);

    // --- STATION CONGRUENCE run-end telemetry --------------------------------
    // (docs/SPEC_redundancy_census.md §13.7.3).  Standing invariant #4 in this
    // card's form: "every held coordinate on this network is right" and "the
    // screen never swept a station" produce identical silence, so the
    // denominators come first — and then THE MDB TABLE, which is the card's
    // service deliverable rather than a diagnostic: the smallest coordinate fault
    // this network can see, per station, per direction.  Body in
    // filter/NetworkCongruence.cpp, next to the sweep that produced the numbers.
    if (ctx.knobs.cg_on)
        networkCongruenceReport(ctx, log, setup.stations);

    // --- PREFIT census / fuse / seed-birth / topology / flight-recorder --------
    // (docs/SPEC_prefit_census_observability.md §10).  Standing invariant #4 in
    // its sharpest form for this card: a TRIPWIRE that never fires and a tripwire
    // that was never CONNECTED produce identical logs.  Only the denominators
    // (`epochs`, `station_epochs`, `rows`, `rec_consults`) separate them, so they
    // are reported first and every zero-finding line names which one it is.
    {
        const auto& p = ctx.counters.pc;
        if (ctx.knobs.pc_walk_on) {
            log->info("Network (prefit census): {} walk pass(es) over {} station-epoch(s), "
                      "{} DATA row(s) classified; {} (station, epoch, type) median(s) "
                      "skipped for population < {}, {} non-finite row(s) dropped.",
                      p.epochs, p.station_epochs, p.rows, p.type_skips,
                      kNetPrefitMinTypeRows, p.nonfinite_rows);
            if (ctx.knobs.pc_census_on) {
                log->info("Network (prefit census INC-1): {} row-cell exceedance(s) over "
                          "s = {:g} on ESTABLISHED cells; {} event(s) emitted, {} "
                          "suppressed by the {}-epoch per-cell holdoff, {} outside the "
                          "event domain [{}].  Run maximum s = {:.4g}{}.",
                          p.exceedances, ctx.knobs.pc_event_sigma, p.events,
                          p.events_held, ctx.knobs.pc_event_holdoff,
                          p.events_class_skipped,
                          dataClassMaskName(ctx.knobs.pc_event_mask), p.max_s,
                          p.max_s_id.empty()
                              ? std::string()
                              : fmt::format(" on {} at TOW {:.1f}", p.max_s_id,
                                            p.max_s_tow));
                // The hand-off to INC-3 is reported whether or not INC-3 is armed:
                // "the census went quiet" and "the census deferred N births to a
                // DISARMED birth check" are different states and must not read the
                // same (invariant #4 applied to a filter rather than to wiring).
                log->info("Network (prefit census INC-1): {} row(s) in {} born cell(s) "
                          "were DEFERRED to INC-3 rather than counted here — a fresh "
                          "arc is seeded to ~0 innovation against a station level that "
                          "can be 1e5 m, so an established-cell statistic is the only "
                          "one that means anything.  {}",
                          p.rows_born_deferred, p.cells_born_deferred,
                          (ctx.knobs.pc_birth_sigma > 0.0)
                              ? "INC-3 is armed and graded them."
                              : "INC-3 is OFF, so nothing graded them — arm "
                                "network.seed_birth_check_sigma to see what was in "
                                "there.");
                if (p.exceedances == 0 && p.rows > 0)
                    log->info("Network (prefit census INC-1): {} row(s) walked and NOT ONE "
                              "exceeded s = {:g}.  That is a genuine negative — no cell in "
                              "this run carried a fault-scale common-mode-removed "
                              "innovation — and it is also the state in which the tripwire "
                              "cannot demonstrate that it works.  Grade it on a leg with a "
                              "known injector.", p.rows, ctx.knobs.pc_event_sigma);
            }
            if (ctx.knobs.pc_birth_sigma > 0.0) {
                log->info("Network (prefit census INC-3): {} LOS birth(s) over {} row(s), "
                          "{} seed_birth_exceedance event(s) at s > {:g}; {} first-epoch "
                          "presence(s) suppressed (no prior map — a first sighting is not "
                          "a birth).",
                          p.births, p.birth_rows, p.birth_events,
                          ctx.knobs.pc_birth_sigma, p.birth_seed_suppressed);
                if (p.births == 0)
                    log->warn("Network (prefit census INC-3): ARMED but ZERO births were "
                              "detected all run — with satellites rising and setting that "
                              "is a MISWIRE (the presence map never advanced), not a quiet "
                              "network.");
                else if (p.birth_events == 0)
                    log->info("Network (prefit census INC-3): {} birth(s) and NOT ONE fired "
                              "— every self-seeded LOS started consistent with its "
                              "network.  That RULES OUT the seed path for this window; a "
                              "mid-arc poisoning mechanism owns whatever came later.",
                              p.births);
            }
            if (ctx.knobs.pc_topo_on) {
                log->info("Network (prefit census INC-4): topology events — {} SH-tie "
                          "row-count delta(s), {} (station, sat) row-kind change(s), "
                          "{} shared column(s) added / {} removed{}.",
                          p.topo_tie_events, p.topo_kind_events, p.topo_col_add,
                          p.topo_col_rm,
                          p.topo_truncated > 0
                              ? fmt::format(" ({} column event(s) TRUNCATED by the "
                                            "per-epoch cap of {} — the totals here are "
                                            "complete, the event rows are not)",
                                            p.topo_truncated, 64)
                              : std::string());
                if (p.topo_tie_events == 0 && p.epochs > 1)
                    log->info("Network (prefit census INC-4): NOT ONE station's SH-tie row "
                              "set changed all run.  The deweight valve never moved a tie "
                              "row here — so this run contains no instance of the era-2 "
                              "topology event and cannot be used to grade the ledger "
                              "against it.");
            }
            if (p.epochs == 0)
                log->warn("Network (prefit census): ARMED but the walk ran on ZERO epochs "
                          "— the wiring never executed.  Nothing in this run is evidence "
                          "about the card.");
            else if (p.rows == 0)
                log->warn("Network (prefit census): ARMED, {} walk(s) ran and ZERO DATA "
                          "rows were classified — every station was skipped (no solve "
                          "membership or a shared-dimension mismatch).  The census is "
                          "INERT despite looking live.", p.epochs);
        }
        if (ctx.knobs.pc_fuse_sigma > 0.0) {
            log->info("Network (prefit fuse INC-2): {} epoch(s) x {} station-epoch(s), "
                      "{} DATA row(s) screened at S_fuse = {:g}; {} row(s) CAPPED "
                      "(deepest cut w_eff/w = {:.3g}, largest pre-cap s = {:.4g}), "
                      "{} event(s) emitted / {} held.  Domain [{}]: {} row(s) skipped "
                      "out of domain, of which {} WOULD have been capped.  Skipped: {} "
                      "pin-weight row(s) (w >= {:g}), {} row(s) whose station "
                      "type-median was unavailable.",
                      p.fuse_epochs, p.fuse_station_epochs, p.fuse_rows,
                      ctx.knobs.pc_fuse_sigma, p.fuse_capped, p.fuse_min_ratio,
                      p.fuse_max_s, p.fuse_events, p.fuse_events_held,
                      dataClassMaskName(ctx.knobs.pc_fuse_mask), p.fuse_skip_class,
                      p.fuse_would_class,
                      p.fuse_skip_pin_w, kNetPrefitPinWeight, p.fuse_skip_thin);
            if (p.fuse_rows == 0)
                log->warn("Network (prefit fuse INC-2): ARMED but ZERO rows were screened "
                          "— the assembly hook never ran.  The solve in this run is the "
                          "UNFUSED one; do not report it as a fuse arm.");
            else if (p.fuse_capped == 0)
                log->info("Network (prefit fuse INC-2): {} row(s) screened and NOT ONE "
                          "exceeded S_fuse = {:g} — this arm is byte-identical to its "
                          "control by construction, which is the correct null result but "
                          "is NOT evidence the cap works.",
                          p.fuse_rows, ctx.knobs.pc_fuse_sigma);
        }
        if (ctx.knobs.rec_on) {
            log->info("Network (rhs-attrib INC-5): flight recorder — {} trigger-site "
                      "consultation(s), {} era trip(s), {} dump(s) written ({} of them "
                      "retained PREVIOUS-epoch frames); {} epoch(s) retained, last frame "
                      "{:.1f} MB.",
                      p.rec_consults, p.rec_trips, p.rec_dumps, p.rec_prev_dumps,
                      p.rec_frames_retained,
                      static_cast<double>(p.rec_prev_bytes_last) / (1024.0 * 1024.0));
            if (p.rec_consults == 0)
                log->warn("Network (rhs-attrib INC-5): ARMED but NO trigger site ever "
                          "consulted the latch — the four trip sites (SH-gate engage, "
                          "sigma0 withhold, clock runaway, a00 band) are all gated behind "
                          "their own feature toggles, and none of them ran.  The recorder "
                          "is DEAD WIRING in this configuration, not a quiet run.");
            else if (p.rec_trips == 0)
                log->info("Network (rhs-attrib INC-5): {} consultation(s) and NOT ONE era "
                          "tripped — the run stayed inside every QC band.  The recorder "
                          "had ZERO opportunity, which is not evidence that it works.",
                          p.rec_consults);
            if (ctx.knobs.rec_on && nc.rhs_attrib_on_trip_prev
                && p.rec_frames_retained == 0)
                log->warn("Network (rhs-attrib INC-5): previous-epoch retention is ON but "
                          "ZERO frames were captured — every epoch failed the capture "
                          "(no reduced system) or the network exceeded "
                          "network.rhs_attrib_prev_max_stations = {}.  A trip would dump "
                          "the aftermath only.", nc.rhs_attrib_prev_max_stations);
        }
    }

    // --- product-referenced measurement admission run-end telemetry -----------
    // (docs/SPEC_measurement_admission.md).  Standing invariant #4 in its purest
    // form: this card's entire claim is that an ABSOLUTE screen sees a class every
    // differential check passes by construction, and the ONLY way a reader can
    // check that claim from a log is to see the denominators next to the findings.
    // A run reporting 0 rejections over 0 screened channels says nothing about the
    // network; a run reporting 0 rejections over 400k screened channels with a
    // worst |z| of 38 m is a real negative result — and one reporting two
    // rejections at 1e5 m against that same sea is the positive one the card was
    // written for.  Reported in that order: denominators, then the census
    // separation, then the response, then the warns.
    if (ctx.knobs.adm_on) {
        const auto& a = ctx.counters.adm;
        log->info("Network (measurement admission): {} screen pass(es), {} station-epoch "
                  "block(s) with a usable median, {} line-of-sight epoch(s), {} CODE "
                  "channel(s) screened; {} station-epoch(s) SKIPPED below the {}-satellite "
                  "median floor, {} observable(s) skipped as non-finite.{}",
                  a.epochs, a.station_epochs, a.los_epochs, a.channels, a.skips_thin,
                  ctx.knobs.adm_min_sats, a.skips_nonfinite,
                  a.resume_passes > 0
                      ? "  One pass ran the warm-resume RESTORE rule."
                      : "");
        log->info("Network (measurement admission CENSUS): {} channel-epoch(s) over the "
                  "{:g} m bound; run maximum |z| {:.6g} m at {} (TOW {:.1f}).  The bound "
                  "sits >= 10x above the worst legitimate |z| and >= 100x below the "
                  "observed corrupt class, so this number is a SEPARATION, not a "
                  "threshold crossing — read it against the per-epoch "
                  "adm_sta_worst_z column, which is the healthy sea.",
                  a.over_bound, ctx.knobs.adm_bound_m, a.max_z,
                  a.max_z_id.empty() ? std::string("-") : a.max_z_id, a.max_z_tow);
        if (ctx.knobs.adm_screen) {
            log->info("Network (measurement admission INC-1/INC-2): {} birth "
                      "screening(s) -> {} admitted, {} REJECTED AT BIRTH, {} rejected on "
                      "a warm-resume RESTORE; {} mid-arc onset(s), {} heal(s) after {} "
                      "clean epoch(s), {} channel-epoch(s) held while already rejected "
                      "({} of them advancing a heal counter); {} station event(s) "
                      "holding {} channel-epoch(s) off an untrusted median.",
                      a.births, a.admits_birth, a.reject_birth, a.reject_resume,
                      a.onsets, a.heals, ctx.knobs.adm_heal_ep, a.rejects_held,
                      a.heal_progress, a.station_events, a.station_held_channels);
            log->info("Network (measurement admission RESPONSE): {} channel-epoch(s) "
                      "withheld, removing {} observable(s) and pruning {} phase-bias / "
                      "{} code-bias / {} satellite-clock state(s); {} funnel "
                      "insertion(s) + {} pending-reseed(s).  A withhold removes BOTH "
                      "observables of the band — the corruption rides code and phase "
                      "together, which is exactly why no differential test sees it.",
                      a.withheld_channels, a.withheld_obs, a.withheld_pb_pruned,
                      a.withheld_cb_pruned, a.withheld_clk_pruned,
                      a.refloat_ambs, a.refloat_reseeds);
        }
        if (ctx.knobs.adm_revet) {
            log->info("Network (measurement admission INC-4 RE-ENTRY VET): {} re-entry "
                      "screening(s) after >= {} absent screened epoch(s) -> {} admitted, "
                      "{} REFUSED on the return epoch{}; {} channel-epoch(s) inside a vet "
                      "window, {} window(s) run to completion.  Refusals the plain {:g} m "
                      "bound would NOT have made, by trigger: {} from the tighter {:g} m "
                      "VET BOUND (large-on-arrival), {} from the PREFIT CENSUS ({} "
                      "consultation(s), {} with no fresh flag to read) (slow-ramp).",
                      a.reentries, ctx.knobs.adm_revet_gap_ep, a.admits_reentry,
                      a.reject_reentry,
                      ctx.knobs.adm_revet_hold
                          ? fmt::format(", {} clean re-entry channel-epoch(s) WITHHELD by "
                                        "the strict posture", a.revet_hold_ep)
                          : std::string(),
                      a.revet_window_ep, a.revet_expired,
                      ctx.knobs.adm_bound_m,
                      a.revet_bound_refusals, ctx.knobs.adm_revet_bound_m,
                      a.revet_census_refusals, a.revet_census_reads,
                      a.revet_census_absent);
            // The headline field readout of this increment, stated as a ratio rather
            // than left for the reader to compute.  On the FT63 24 h control, 16 of
            // 17 mid-arc onsets were re-entries wearing an onset's label; an armed
            // run should move that population into reject_reentry and leave `onsets`
            // holding only channels that genuinely went bad while continuously
            // present.  If `onsets` is still the larger of the two, the vet is not
            // reaching the class this increment was built for.
            if (a.reject_reentry + a.onsets > 0)
                log->info("Network (measurement admission INC-4): mid-arc withhold split "
                          "-- {} refused as RE-ENTRIES vs {} as true onsets on a "
                          "continuously-present channel.  The control run "
                          "(runs/ft63_24h_conv, INC-4 off) scored 0 vs 17, of which 16 "
                          "were re-entries in everything but the label.",
                          a.reject_reentry, a.onsets);
        }
        log->info("Network (measurement admission): {} ADMISSION event(s) emitted, {} "
                  "suppressed by the per-cell holdoff; {} admission record(s) forgotten "
                  "after a very long absence.",
                  a.events, a.events_held, a.arcs_forgotten);
        // --- invariant #4: distinguish "ran and found nothing" from "never ran" ---
        if (a.epochs == 0)
            log->warn("Network (measurement admission): ARMED but the screen ran on ZERO "
                      "epochs — the wiring never executed (invariant #4).  Nothing in "
                      "this run is evidence about the card.");
        else if (a.channels == 0)
            log->warn("Network (measurement admission): ARMED and the screen ran, but it "
                      "screened ZERO code channels — every station fell under the median "
                      "floor, or no observation carried a `modeled` counterpart.  The "
                      "card is INERT despite looking live (invariant #4); check that the "
                      "producer is on the UNCOMBINED path, where `observed` and "
                      "`modeled` are keyed by raw RINEX-3 codes.");
        else if (a.station_epochs == 0)
            log->warn("Network (measurement admission): {} screen pass(es) and NOT ONE "
                      "station reached the {}-satellite median floor — every verdict was "
                      "skipped, so this run says nothing about any channel.",
                      a.epochs, ctx.knobs.adm_min_sats);
        if (ctx.knobs.adm_screen && a.withheld_channels > 0 && a.withheld_obs == 0)
            log->warn("Network (measurement admission): {} channel-epoch(s) counted as "
                      "withheld and ZERO observables were actually removed — the "
                      "counters are live over dead wiring (invariant #4), not a quiet "
                      "run.  Check that the band predicate matches the observable keys "
                      "this run is using.", a.withheld_channels);
        if (ctx.knobs.adm_screen
            && (a.onsets + a.reject_resume + a.station_events) > 0
            && a.refloat_ambs == 0 && a.refloat_reseeds == 0)
            log->warn("Network (measurement admission): {} response(s) that MUST refloat "
                      "(onset / resume-restore / station event) fired and NOT ONE "
                      "ambiguity was reset — neither feed is live (the funnel insert "
                      "needs network.carry_invariant, the pending-reseed a warm "
                      "network.phase_leveling).  A hole was punched in a stiff "
                      "time-linked carry with no re-tap behind it, which is standing "
                      "invariant #1 violated, not a tuning detail.",
                      a.onsets + a.reject_resume + a.station_events);
        // --- INC-4, invariant #4 -------------------------------------------
        // Every one of these separates "the vet ran and found nothing" from "the vet
        // never got the chance", which no amount of clean log output distinguishes.
        if (ctx.knobs.adm_revet && a.channels > 0 && a.reentries == 0)
            log->warn("Network (measurement admission INC-4): the re-entry vet is ARMED "
                      "and {} channel(s) were screened, yet NOT ONE re-entry was seen. "
                      "A 63-station day carries ~4650 of them, so on anything longer "
                      "than a short leg this means the gap test never fires — check "
                      "network.adm_revet_gap_ep ({}) and that arcs are actually leaving "
                      "the screened set.  The card is INERT despite looking live.",
                      a.channels, ctx.knobs.adm_revet_gap_ep);
        if (ctx.knobs.adm_revet && ctx.knobs.adm_revet_census
            && a.revet_census_reads > 0
            && a.revet_census_absent == a.revet_census_reads)
            log->warn("Network (measurement admission INC-4): the census trigger is ARMED "
                      "and every one of its {} consultation(s) found NO fresh flag to "
                      "read — the prefit walk is off, or its flags never carried the "
                      "previous loop epoch's stamp.  The vet ran on the |z| bound ALONE, "
                      "which is the configuration the four exposed re-entries of the FT63 "
                      "day survived for 2-8 epochs.  Arm one of "
                      "network.trace_prefit_census / seed_birth_check_sigma / "
                      "trace_topology.", a.revet_census_reads);
        if (ctx.knobs.adm_revet && !ctx.knobs.adm_revet_hold && a.reentries > 0
            && a.reject_reentry == 0 && a.revet_census_refusals == 0
            && a.revet_bound_refusals == 0)
            log->info("Network (measurement admission INC-4): {} re-entry screening(s) and "
                      "not one refusal — every arc that came back was inside the bound on "
                      "arrival and was never flagged by the census while vetting.  That is "
                      "the expected reading on clean data (the increment is inert by "
                      "construction on a healthy re-entry: no withhold, no refloat, no "
                      "solve change), NOT evidence that the vet is broken; the "
                      "denominators above are what says it ran.", a.reentries);
        if (ctx.knobs.adm_screen && a.over_bound > 0 && a.withheld_channels == 0)
            log->warn("Network (measurement admission): {} channel-epoch(s) exceeded the "
                      "bound and NOTHING was withheld.  Either every one of them landed "
                      "inside a station event whose channel set was empty, or the "
                      "response path is not reached — investigate before reading this "
                      "run as clean.", a.over_bound);
        if (ctx.knobs.adm_screen && a.station_events > 0)
            log->warn("Network (measurement admission): {} station event(s) fired — more "
                      "than {:g} of a station's screened channels disagreed with the "
                      "products at once, so its MEDIAN was untrusted and every "
                      "per-channel verdict was REFUSED (the IF-TD incoherent-station "
                      "precedent).  That is a station-level data or a-priori-coordinate "
                      "problem, not a channel one; the per-channel counts above exclude "
                      "these epochs entirely.",
                      a.station_events, ctx.knobs.adm_max_frac);
        if (!ctx.knobs.adm_screen && a.over_bound > 0)
            log->warn("Network (measurement admission): the census is MEASURE-ONLY this "
                      "run and found {} channel-epoch(s) over the bound — they were "
                      "folded into the solve exactly as if nothing had been seen.  Arm "
                      "network.adm_screen to act on them.", a.over_bound);
    }

    // --- PD-safe invariant-DD fold run-end telemetry (SPEC_pd_safe_dd_fold.md) ------
    // Standing invariant #4 in its most expensive instance to date.  Before this card the
    // fold's response to a non-PD Pᶻ+B was `M := 0` with NO counter, NO log and NO event:
    // the block's posterior became its prior forever, the D5 gate skipped its rows because
    // Cov(v) inherited the negative diagonals, and dd_sigma_cyc / the AR probe read a
    // covariance that never noticed.  Measured 2026-08-02 on `runs/churn_base` (63 sta,
    // 3 h, DOY-129): 568 of 686 carried rows — 78 % of the state — diffusing behind that
    // silence, in the control AND every arm of the lineage.  INC-0 is always-on, so a
    // carry_invariant run ALWAYS gets a line here, armed or not — the branch below carries
    // the zero-fold case rather than letting it fall through silently (SPEC §3, "Run-end
    // summary always").  Keep those two branches exhaustive over `carry_invariant`.
    if (ctx.knobs.carry_invariant && ctx.counters.ddfold_epochs > 0) {
        const auto& c = ctx.counters;
        const double degen_frac = (c.ddfold_fold_rows > 0)
            ? static_cast<double>(c.ddfold_degen_rows)
              / static_cast<double>(c.ddfold_fold_rows) : 0.0;
        log->info("Network (Card 4 PD-safe fold, INC-0): {} epoch(s) folded {} DD-prior "
                  "block(s) / {} carried row(s); {} block-epoch(s) and {} row-epoch(s) "
                  "({:.2f} % of the carried dimension) folded INERTLY on a failed PD "
                  "screen, over {} epoch(s).  Worst relative pivot d_min/d_max = {:.6g}.  "
                  "Edges: {} healthy->degenerate, {} degenerate->healthy.  D5 rows the "
                  "gate could not test (Cov(v) not positive): {}.",
                  c.ddfold_epochs, c.ddfold_folds, c.ddfold_fold_rows,
                  c.ddfold_degen_blocks, c.ddfold_degen_rows, 100.0 * degen_frac,
                  c.ddfold_degen_epochs, c.ddfold_min_pivot_rel,
                  c.ddfold_transitions, c.ddfold_recoveries,
                  carry_driver.counters().d5_nonpos_var_rows);
        if (ctx.knobs.carry_invariant_pd_safe) {
            log->info("Network (Card 4 PD-safe fold, INC-1/2): ARMED — {} eigen-clamp "
                      "repair(s) over {} row(s) (worst clamped |lambda|/lambda_max = "
                      "{:.6g}; {} fold(s) above the 1e-6 'this is not dust' bar), {} "
                      "block rebirth(s) re-seeding {} carried row(s), {} block(s) "
                      "rebirthed at warm-resume restore.",
                      c.ddfold_repairs, c.ddfold_repair_rows, c.ddfold_repair_worst,
                      c.ddfold_repair_loud, c.ddfold_rebirths, c.ddfold_rebirth_rows,
                      carry_driver.resumeCarryRebirthBlocks());
            if (c.ddfold_repair_loud > 0)
                log->warn("Network (Card 4 PD-safe fold, INC-1c): {} fold(s) needed a "
                          "repair LARGER than 1e-6 x lambda_max.  The repair exists for "
                          "round-off dust; a clamp this size means the block arrived "
                          "genuinely broken.  Do NOT read the repaired covariance as "
                          "healthy — find the upstream defect.", c.ddfold_repair_loud);
            if (c.ddfold_degen_rows > 0 && c.ddfold_rebirths == 0)
                log->warn("Network (Card 4 PD-safe fold, INC-2a): {} row-epoch(s) folded "
                          "inertly yet ZERO blocks were rebirthed — the re-coupling edge "
                          "never fired, so those blocks were decoupled with no bounded "
                          "response (standing invariant #7).  This is a WIRING failure, "
                          "not a quiet run.", c.ddfold_degen_rows);
        } else if (c.ddfold_degen_rows > 0) {
            log->warn("Network (Card 4 PD-safe fold): {} row-epoch(s) ({:.2f} % of the "
                      "carried dimension) folded INERTLY and network."
                      "carry_invariant_pd_safe is OFF — those blocks took NO data and, "
                      "their priors never improving, will not factor again: the "
                      "decoupling is PERMANENT and unbounded.  Anything graded on this "
                      "run's carry is graded on a frozen substrate.",
                      c.ddfold_degen_rows, 100.0 * degen_frac);
        }
    } else if (ctx.knobs.carry_invariant) {
        // SPEC §3: "Run-end summary always."  The gate above is `ddfold_epochs > 0`, and
        // that counter only advances on an ACCEPTED install whose fold saw blocks
        // (NetworkInstallCarry, under `if (fs.blocks > 0)`).  Zero is genuinely reachable
        // with no fault at all — buildCarryDdPriors emits nothing when there is no shared
        // datum or no foldable chord, and the fold early-returns before it ever tallies —
        // so without this branch a carry_invariant run could print NO INC-0 line and take
        // d5_nonpos_var_rows down with it (its only readout is the block above).  The
        // per-epoch trace columns are gated on the same `blocks > 0`, so there is no
        // fallback there either: the run would be silent, and silence reads as "clean".
        log->warn("Network (Card 4 PD-safe fold, INC-0): network.carry_invariant is ON but "
                  "the DD-prior fold ran on ZERO epochs — no accepted install ever reached a "
                  "factorization, so every INC-0 tally is structurally empty and this run is "
                  "evidence about NOTHING in the card (standing invariant #4: inert wiring "
                  "must be VISIBLE — this line is the only place it is).  Expected on a run "
                  "with no shared datum or no foldable chord; unexpected otherwise, and then "
                  "it means the carry never engaged.{}  D5 rows the gate could not test "
                  "(Cov(v) not positive): {}.",
                  ctx.knobs.carry_invariant_pd_safe
                      ? "  network.carry_invariant_pd_safe is ARMED and likewise never "
                        "executed — the arm proves nothing on this run."
                      : "",
                  carry_driver.counters().d5_nonpos_var_rows);
    } else if (ctx.knobs.carry_invariant_pd_safe) {
        log->warn("Network (Card 4 PD-safe fold): ARMED but the DD-prior fold ran on ZERO "
                  "epochs — the wiring never executed (invariant #4).  Nothing in this run "
                  "is evidence about the card.");
    }

    // --- Producer-AR Phase-2 diagnostic run-end readout ------------------------------
    // docs/network_ambiguity_resolution_design.md, the 2026-09-10 note §4.1: "a fix
    // count is not a grade".  This clause is the RUN-level half of that — the per-epoch
    // spdlog line said what one epoch did, and nothing said what the run did, so the
    // only way to read an armed run was to grep 2 879 lines.  Standing invariant #4:
    // `netar_epochs == 0` on an ARMED run means the search NEVER REACHED the ILS and
    // every number below is structurally empty; that case gets its own warn.
    if (nc.ambiguity_resolution.enable) {
        auto& c = ctx.counters;
        auto p50 = [](std::vector<double> v) {
            if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        // ratio / p_succ are collected on ACCEPTED-fix epochs only (a rejected epoch's
        // IlsResult zeroes them), so on a run that never fixed the sample is EMPTY.
        // "n/a" says that; a printed "nan" or "0.00" reads like a measured value.
        auto p50s = [&](const std::vector<double>& v, int prec) {
            return v.empty() ? std::string("n/a")
                             : fmt::format("{:.{}f}", p50(v), prec);
        };
        if (c.netar_epochs > 0) {
            const double fix_pct = 100.0 * static_cast<double>(c.netar_fix_epochs)
                                          / static_cast<double>(c.netar_epochs);
            log->info("Network (producer AR, Phase 2 log-only): {} epochs searched, "
                      "{:.0f} DD p50 / {:.0f} searched p50, fixed epochs {} ({:.1f}%), "
                      "DD fixed total {}, ratio p50 {}, p_succ p50 {}, "
                      "med_DD_sigma p50 {:.4f} cyc; carry-{}; trace {}",
                      c.netar_epochs, p50(c.netar_n_dd), p50(c.netar_n_search),
                      c.netar_fix_epochs, fix_pct, c.netar_dd_fixed,
                      p50s(c.netar_ratio, 2), p50s(c.netar_p_succ, 4), p50(c.netar_med_sigma),
                      c.netar_carry_aware ? "aware" : "blind",
                      nc.ambiguity_resolution.trace ? "on" : "off");
            if (nc.ambiguity_resolution.trace) {
                if (c.netar_rows > 0)
                    log->info("Network (producer AR, Phase 2): {} per-DD trace row(s) "
                              "written to {} — grade with "
                              "scripts/simnet_producer_ar_grade.py (a fix count is not a "
                              "grade; the DD basis is a STAR against one reference "
                              "satellite and one reference station, so report or "
                              "difference away that pair's share).",
                              c.netar_rows, traces.producer_ar_trace_path.string());
                else
                    log->warn("Network (producer AR, Phase 2): the trace is ARMED and the "
                              "search ran {} epoch(s), yet ZERO rows were written — the "
                              "stream never opened (check the warning above) and this run "
                              "carries NO gradeable AR evidence.", c.netar_epochs);
            }
            if (c.netar_fix_epochs > 0 && !c.netar_carry_aware)
                log->warn("Network (producer AR, Phase 2): {} epoch(s) accepted a fix on a "
                          "CARRY-BLIND covariance (network.carry_aware_covariance is OFF), "
                          "so every gate the fix passed — FFR, ratio, the per-pair frac cap "
                          "— was computed from a covariance the SOLVE never used "
                          "(docs/SPEC_carry_aware_covariance.md §4).  A WRONG fix at "
                          "p_succ >= min_success_rate falsifies that covariance as a gate "
                          "input; grade the integers against truth before reading the fix "
                          "rate as a success.", c.netar_fix_epochs);
        } else {
            log->warn("Network (producer AR, Phase 2 log-only): ARMED but the ILS search "
                      "ran on ZERO epochs — min_epochs={} never cleared, the joint "
                      "conditional covariance never returned, or the DD basis never had "
                      "two undifferenced columns.  Every producer-AR number for this run "
                      "is structurally empty (standing invariant #4); this run is evidence "
                      "about NOTHING in the card.", nc.ambiguity_resolution.min_epochs);
        }
    }

    // --- Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md §4) -----
    // ALWAYS on a carry_invariant run, armed or not: the whole point of the card is that a
    // σ published without its covariance mode is unreadable, and the run that most needs
    // to be told its σ is carry-BLIND is the one that did not arm the knob.
    if (ctx.knobs.carry_invariant) {
        const auto& cc = est.carryCovStats();
        if (ctx.knobs.carry_aware_covariance) {
            const double mean_ratio = (cc.ratio_n > 0)
                ? cc.ratio_sum / static_cast<double>(cc.ratio_n) : 1.0;
            log->info("Network (carry-aware covariance): ARMED — {} readback call(s) over "
                      "{} requested (station,column) entr(ies); {} corrected, {} REFUSED "
                      "for a non-positive corrected variance ({} matrix call(s) fell back "
                      "wholesale).  Variance ratio corrected/blind: min {:.6g}, mean "
                      "{:.6g}, max {:.6g}.  dd_sigma_cyc and the AR probe's Q_cyc are the "
                      "estimator's own uncertainty on this run.",
                      cc.calls, cc.rows, cc.corrected_rows, cc.nonpos_rows,
                      cc.refused_calls, cc.ratio_min, mean_ratio, cc.ratio_max);
            if (cc.calls == 0)
                log->warn("Network (carry-aware covariance): ARMED but ZERO readback calls "
                          "reached the correction — no consumer asked, or every epoch was "
                          "chordless.  The wiring never executed (invariant #4); nothing "
                          "in this run is evidence about the card.");
            else if (cc.ratio_n > 0 && cc.ratio_min >= 1.0)
                log->warn("Network (carry-aware covariance): ARMED and the correction moved "
                          "NOTHING (min ratio {:.6g} >= 1).  The carry can only ADD "
                          "information, so a readback it never tightens means the rank-K "
                          "term reached no requested column — dead wiring, not a quiet "
                          "run.", cc.ratio_min);
            if (cc.nonpos_rows > 0)
                log->warn("Network (carry-aware covariance): {} corrected variance(s) came "
                          "out NON-POSITIVE and were refused (never clamped).  This is not "
                          "a readback bug: the FOLD composes one Woodbury update per block "
                          "against the pristine N_rr⁻¹, which is exact only when no station "
                          "is touched by two blocks — and a block is one connected "
                          "component of ONE (const,signal) group.  This counter is the "
                          "first measurement anywhere of that cross-block approximation "
                          "error.  It is a result about the SOLVE; open it its own card.",
                          cc.nonpos_rows);
        } else {
            log->info("Network (carry-aware covariance): OFF — dd_sigma_cyc and the AR "
                      "probe's Q_cyc are CARRY-BLIND on this run (Cov(x_r) from the "
                      "prior-free N_rr⁻¹ while the shared block was carry-aware, i.e. the "
                      "covariance of no estimator).  Read them as an epoch-local float "
                      "bound only; in particular a uniform med|frac| is the EXPECTED "
                      "reading at these σ and is NOT evidence about integer information.");
        }
    }

    // --- SD pre-reduce EWL constraint run-end telemetry (SPEC_sd_ewl_prereduce.md) ---
    // Standing invariant #4: rows_built == 0 on an armed run means the wiring never fired,
    // which is NOT "the feature had no effect" — so the skip tallies are reported too, and
    // an all-zero build is called out explicitly rather than left to look quiet.
    if (config_.getNetworkConfig().sd_ewl.enable) {
        NetStation::SdEwlCounters t;
        for (const auto& st : stations) t += st.sd_ewl_ctr;
        const double mean_var = t.rows_built > 0
                                    ? t.sum_var_m2 / static_cast<double>(t.rows_built)
                                    : 0.0;
        log->info("Network (SD pre-reduce EWL): {} row(s) built over the run from {} "
                  "(station,epoch,satellite) visit(s) on an EWL-tier constellation "
                  "(mean SD variance {:.5f} m^2, i.e. sigma ~ {:.4f} m); {} capped.  "
                  "Skips — {} no-detector station-epoch(s), {} no-pivot (<2 arcs at the "
                  "station), {} NO MW ARC on any candidate signal pair, {} immature arc "
                  "(< {} lagged samples), {} unreconstructable SD frame (group MW pivot "
                  "arc absent), {} mixed BIA frame, {} no ambiguity-column pair, {} "
                  "missing shared PB/CB column, {} arc without code channels, {} bad "
                  "lambda, {} |z| over limit.  FLOAT rows only (increment 1); these fold "
                  "PRE-reduce so they compose exactly with the Card-4 post-reduce DD fold.",
                  t.rows_built, t.sat_seen, mean_var, std::sqrt(std::max(mean_var, 0.0)),
                  t.rows_capped, t.skip_no_detector, t.skip_no_pivot, t.skip_no_arc,
                  t.skip_immature, config_.getNetworkConfig().sd_ewl.min_arc_epochs,
                  t.skip_frame_recon, t.skip_mixed_frame, t.skip_missing_amb_col,
                  t.skip_missing_shared_col, t.skip_missing_code, t.skip_bad_lambda,
                  t.skip_big_z);
        if (t.rows_built == 0) {
            // Name the DOMINANT cause: "zero rows" plus a table of ten tallies is a
            // puzzle, and the three failure classes have entirely different fixes
            // (wiring / signal availability / maturity).
            const char* lead =
                t.sat_seen == 0                                  ? "the appender never saw a single EWL-tier satellite — the hook is not running, or no GPS/Galileo station-epoch reached PASS B"
              : t.skip_missing_amb_col >= t.sat_seen / 2         ? "the dominant cause is NO AMBIGUITY-COLUMN PAIR: the second EWL band (GPS L5 / Galileo E5a) is not being ESTIMATED at these stations — check processing.frequency_mode / signal_priorities, not the detector"
              : t.skip_no_arc >= t.sat_seen / 4                  ? "the dominant cause is NO MW ARC on the (band1,band2) pair: the detector never formed that pair — check mw_frequency_mode (Mirror reads obs.observed, so an unprocessed band yields no pair) and mw_channel_mode"
              : t.skip_immature >= t.sat_seen / 4                ? "the dominant cause is IMMATURE arcs: the run is shorter than sd_ewl.min_arc_epochs, or arcs keep refloating"
              : t.skip_frame_recon >= t.sat_seen / 4             ? "the dominant cause is an UNRECONSTRUCTABLE SD frame: the arcs are BIA-differenced but their group MW pivot arc is missing"
                                                                : "no single cause dominates — read the tallies above";
            log->warn("Network (SD pre-reduce EWL): ARMED but built ZERO rows — the "
                      "feature is INERT on this run.  {}.  (An all-zero no-detector "
                      "count with sat_seen > 0 means the hook DID run; a large "
                      "no-detector count means phase_leveling / cycle_slip.mw_detect "
                      "are off or every station is the clock datum.)", lead);
        }
        // --- increment 2a+2b: the INTEGER path (docs/SPEC_sd_ewl_prereduce.md §8) -----
        if (config_.getNetworkConfig().sd_ewl.fix_enable) {
            const auto& K = ctx.counters;
            log->info("Network (SD EWL integer fix, 2a decision): {} epoch(s), {} "
                      "component(s), {} arc(s) admitted to the integer graph; {} DD "
                      "rounding attempt(s) -> {} usable integer(s), of which {} are DATUM "
                      "values (reference/link/first-sighting frame choices that carry no "
                      "rounding decision and so cannot be wrong).  Rejected — {} on "
                      "|frac| > {:.3f} cyc, {} on FFR < {:.4f}.  Events — {} FLIP "
                      "(disagreed with the previous epoch on a continuous arc: counted "
                      "AND vetoed, the standing-invariant-#1 response), {} arc restart "
                      "(no flip charged), {} reference-station re-election (re-datums a "
                      "whole component's SD frame).  Excluded — {} on a non-consensus "
                      "tracking-channel pair (B^s would NOT cancel in the DD), {} on a "
                      "mixed BIA frame, {} with no admissible link satellite, {} with no "
                      "DD graph.",
                      K.sdewl_epochs, K.sdewl_components, K.sdewl_dd_pairs_seen,
                      K.sdewl_dd_fix_attempts, K.sdewl_dd_fixed, K.sdewl_dd_datum,
                      K.sdewl_dd_reject_frac,
                      config_.getNetworkConfig().sd_ewl.fix_max_frac_cyc,
                      K.sdewl_dd_reject_ffr,
                      config_.getNetworkConfig().sd_ewl.fix_min_ffr,
                      K.sdewl_dd_flip, K.sdewl_dd_restart, K.sdewl_sd_ref_reelected,
                      K.sdewl_dd_skip_channel, K.sdewl_dd_skip_frame,
                      K.sdewl_dd_no_link, K.sdewl_dd_no_graph);
            log->info("Network (SD EWL integer fix, 2b fold): {} FIXED row(s) in {} "
                      "all-fixed whitened group(s) + {} float row(s) (of {} built).  {} "
                      "arc(s) had an integer on a DIFFERENT channel pair than the row "
                      "settled on and went float; {} lone fixed arc(s) had nothing to "
                      "difference against; {} arc(s) were demoted because their group "
                      "held DATUM values only (a frame choice carries no integer "
                      "information, so it does not earn a removed local DOF).  A fixed row carries ZERO local ambiguity "
                      "coefficients — the local DOF is removed, not constrained — and is "
                      "weighted at the per-SAMPLE sigma floor {:.2f} m (amendment A1: "
                      "arcZdMean is arc-CUMULATIVE, so refolding a variance-of-mean as "
                      "white every epoch over-claims information as sqrt(k)).",
                      t.rows_fixed, t.groups_fixed, t.rows_float, t.rows_built,
                      t.fix_pair_mismatch, t.fix_lone_arc, t.fix_datum_only,
                      config_.getNetworkConfig().sd_ewl.fix_sigma_floor_m);
            if (t.rows_fixed == 0) {
                // Invariant #4 again, one level down: the row layer can be healthy and
                // the INTEGER layer still inert, and the two have completely different
                // fixes.  Name the dominant cause rather than leaving nine tallies.
                const char* lead2 =
                    K.sdewl_epochs == 0
                        ? "the 2a decision seam never ran — the call site is not reached "
                          "(every epoch skipped before pre-assembly?)"
                  : K.sdewl_dd_pairs_seen == 0
                        ? "no arc reached the integer graph: the MW accumulator has no "
                          "MATURE EWL arc at any station (raise the run length or lower "
                          "sd_ewl.min_arc_epochs), or no station has both EWL bands "
                          "ESTIMATED"
                  : K.sdewl_dd_no_graph >= K.sdewl_dd_pairs_seen / 2
                        ? "the DD GRAPH is empty: stations do not share two co-visible "
                          "satellites on a consensus channel pair, so no double "
                          "difference can be formed at all"
                  : K.sdewl_dd_skip_channel >= K.sdewl_dd_pairs_seen / 2
                        ? "channel DIVERSITY: most arcs sit on a non-consensus tracking "
                          "pair, so they are excluded to keep the satellite-bias "
                          "cancellation exact — check processing.channel_mode and "
                          "signal_priorities"
                  : K.sdewl_dd_reject_ffr >= K.sdewl_dd_fix_attempts / 2
                        ? "the FFR gate: the DDs are too noisy to round confidently "
                          "(arcs are short, or sd_ewl.fix_min_ffr is set too tight for "
                          "this maturity)"
                  : K.sdewl_dd_reject_frac >= K.sdewl_dd_fix_attempts / 2
                        ? "the |frac| gate: the DDs are NOT sitting on integers, which "
                          "is itself the finding — Route B's cancellation is failing, so "
                          "suspect channel identity or the BIA frame before the gate"
                  : K.sdewl_dd_flip >= K.sdewl_dd_fixed
                        ? "FLIP vetoes: integers are decided but disagree epoch to "
                          "epoch, so none is ever folded.  That is the invariant-#1 "
                          "response doing its job; the integers are not stable enough "
                          "to fold"
                  : t.fix_pair_mismatch > 0
                        ? "CHANNEL MISMATCH between the decision and the row: integers "
                          "exist but the row builder settles on a different pair"
                        : "no single cause dominates — read the 2a tallies above";
                log->warn("Network (SD EWL integer fix): ARMED but folded ZERO fixed "
                          "rows.  {}.", lead2);
            }
        }
    }

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
    // SPEC_slip_relevel_walk §6 run-end readout (invariant #4: either knob
    // armed with its headline counter at 0 on a slip-bearing run is a MISWIRE,
    // not a quiet run — the drains latched but the inject never consulted, or
    // the bridge never met a joint (MW, IF-TD) delta).
    if (config_.networkCycleSlipRelevelFreeEpoch()) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (slip re-level response): {} detector-marked (station, amb) "
                  "free-epoch latches, {} inject-site skips actually taken (the arc had "
                  "a live carry the stale-injection path would otherwise have folded).{}",
                  qcc.relevel_free_marked, ctx.counters.n_relevel_free_skips,
                  config_.networkCycleSlipRelevelDeltaBridge()
                      ? fmt::format("  Bridge v2 (pend-and-replace): {} pend(s) created, "
                                    "{} REPLACED (of which {} paired same-epoch; max |ΔN| "
                                    "{:.1f} cyc) — those arcs recovered their pre-slip "
                                    "level at phase precision; {} corroboration-rejected "
                                    "(kept the free re-level — the §8.1 wrong-bridge "
                                    "guard), {} kernel-rejected, {} expired unpaired, {} "
                                    "voided by re-slip, {} no-carry, {} refsat-frame, {} "
                                    "MW-without-flag.",
                                    qcc.bridge_pends_created, qcc.bridge_pairs_applied,
                                    qcc.bridge_joint, qcc.bridge_max_abs_dn_cyc,
                                    qcc.bridge_replace_corrob_reject,
                                    qcc.bridge_fallback_reject,
                                    qcc.bridge_pend_expired, qcc.bridge_pend_voided,
                                    qcc.bridge_fallback_no_carry,
                                    qcc.bridge_fallback_refsat,
                                    qcc.bridge_fallback_unpaired)
                      : std::string{});
        // Bridge v3 riders (the better second equation): both coupling edges of
        // the new equation menu, run-end (invariant #4 — gf armed with
        // gf_captured=0 on a slip-bearing run is a MISWIRE, as is ewl armed
        // with both fill counters at 0).
        if (config_.networkCycleSlipRelevelBridgeGf()
            || config_.networkCycleSlipRelevelBridgeEwl())
            log->info("Network (bridge v3): GF rows captured {} (E1+E4 float-plane "
                      "applies {}, wrong-WL plane rejects {}); EWL integers filled "
                      "{} same-epoch + {} belated, band-3 replaces {}.",
                      qcc.bridge_gf_captured, qcc.bridge_gf_applied,
                      qcc.bridge_plane_reject, qcc.bridge_ewl_joint,
                      qcc.bridge_ewl_belated, qcc.bridge_band3_applied);
    }
    if (config_.networkCycleSlipRelevelRetapStatistical())
        log->info("Network (slip re-level response / retap): {} isolated-arc RAIM "
                  "refloat(s) KEPT their level (confidence re-tapped to {:.2f} m²), "
                  "{} escalated to the erase on the {}-in-120s streak, {} had no live "
                  "carry.  Whole-sat consensus refloats keep the strong erase.",
                  ctx.counters.n_relevel_retap_kept,
                  config_.networkCycleSlipRelevelRetapVarM2(),
                  ctx.counters.n_relevel_retap_escalated,
                  config_.networkCycleSlipRelevelRetapStreakMax(),
                  ctx.counters.n_relevel_retap_miss);
    // ---- Leveling-carry RETIREMENT census (SPEC_withhold_era_release §8) ----
    // Always on: pure counters, no CSV row, no solve effect.  It exists so a
    // fleet-scale carry loss can be ATTRIBUTED rather than inferred.
    //
    // Reported as FOUR FAMILIES, never as one total: the units are not
    // commensurable.  A frame skip costs one solve of continuity for an arc the
    // same epoch re-stashes; a refloat costs one solve on the next epoch; a
    // departure is permanent; and the inject-side stale skip erases nothing at
    // all yet is the mechanism behind the §1.3 mass re-level.  Summing them
    // would make a lineage with a churning chain datum look like it retires an
    // order of magnitude more carry than one that does not — precisely the
    // inference the §7 promotion argument would misuse.
    {
        const auto& C = ctx.counters;
        const double pl_gap = config_.networkPhaseLevelingMaxGapS();
        // Armed-state disclosure, the same qualifier family [C] already carries.
        // Families [A] and [B] both live on the phase-leveling carry, and [B]
        // additionally sits at an inject site that carry_invariant supersedes —
        // so on those postures a 0 means UNWIRED, not clean.  Saying so is the
        // whole point of the census (invariant #4).
        const bool pl_live    = config_.networkPhaseLeveling();
        const bool cliff_live = pl_live && !config_.networkCarryInvariant();
        log->info("Network (leveling-carry retirement census) [B] THE CLIFF{} — "
                  "inject-side stale skips: {} carried prior(s) NOT injected for "
                  "gap > {:.0f} s over {} epoch(s); worst epoch {} at week {} tow "
                  "{:.1f} across {} station(s).  Nothing is erased by this path — "
                  "the arc simply solves FREE and the harvest re-stashes it from "
                  "one epoch of code, which is why the post-harvest prune below "
                  "cannot see it.  A worst-epoch count near the carried fleet, on "
                  "ONE epoch, spanning MANY stations, is the §1.3 mass re-level; "
                  "the same count concentrated on one station index is that "
                  "station dropping out, which is a different event entirely.",
                  cliff_live ? "" : " ⚠ CHANNEL NOT ARMED on this posture "
                                    "(phase_leveling off or carry_invariant on) — a 0 "
                                    "here means UNWIRED, not clean",
                  C.n_cr_inject_stale, pl_gap, C.n_cr_inject_stale_epochs,
                  C.n_cr_inject_stale_max, C.n_cr_inject_stale_max_week,
                  C.cr_inject_stale_max_tow, C.n_cr_inject_stale_max_sta);
        log->info("Network (leveling-carry retirement census) [A] DEPARTURES{} "
                  "(permanent): {} amb-carry entr(ies) pruned on the {:.0f} s gap "
                  "over {} epoch(s); worst {} of a {}-entry carry ({:.1f} %) at week "
                  "{} tow {:.1f}.  Siblings on the same predicate: {} ZWD, {} "
                  "receiver-PB.  ⚠ The prune runs AFTER the harvest has refreshed "
                  "last_seen_s for every arc it stashed, so its candidates are only "
                  "arcs NOT observed this epoch — read it as departures, never as "
                  "amputation.",
                  pl_live ? "" : " ⚠ CHANNEL NOT ARMED (phase_leveling off) — a 0 "
                                 "here means UNWIRED, not clean",
                  C.n_cr_gap_prune, pl_gap, C.n_cr_gap_prune_epochs,
                  C.n_cr_gap_prune_max, C.n_cr_gap_prune_max_fleet,
                  C.n_cr_gap_prune_max_fleet > 0
                      ? 100.0 * static_cast<double>(C.n_cr_gap_prune_max)
                              / static_cast<double>(C.n_cr_gap_prune_max_fleet)
                      : 0.0,
                  C.n_cr_gap_prune_max_week, C.cr_gap_prune_max_tow,
                  C.n_cr_zwd_gap_prune, C.n_cr_rpb_gap_prune);
        // F1 (network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount) — the
        // inject-side per-LOS variance scale's ON-path telemetry (invariant #4).
        // Printed ONLY when armed, so an OFF log is byte-identical.  Armed on a
        // multi-band posture with 0 rows scaled means the inject never saw two
        // carried bands on one LOS — a MISWIRE, not a quiet run; a histogram
        // concentrated at B_eff = 1 on a triple-frequency bed says the sibling
        // bands were being skipped (freed / stale / reseed), not carried.
        if (ctx.knobs.pl_los_scale)
            log->info("Network (leveling LOS-scale F1){}: {} row(s) scaled over {} LOS "
                      "group(s); B_eff histogram 1:{} 2:{} 3:{} 4+:{}",
                      cliff_live ? "" : " ⚠ CHANNEL NOT ARMED on this posture "
                                        "(phase_leveling off or carry_invariant on) — a 0 "
                                        "here means UNWIRED, not clean",
                      C.n_pl_los_scale_rows,
                      C.n_pl_los_scale_hist[0] + C.n_pl_los_scale_hist[1]
                          + C.n_pl_los_scale_hist[2] + C.n_pl_los_scale_hist[3],
                      C.n_pl_los_scale_hist[0], C.n_pl_los_scale_hist[1],
                      C.n_pl_los_scale_hist[2], C.n_pl_los_scale_hist[3]);
        // F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount) — the
        // station-wide jointly-whitened leveling-carry block's ON-path telemetry
        // (invariant #4).  Printed ONLY when armed, so an OFF log is byte-identical.
        // Armed with 0 blocks means the inject never built one — a MISWIRE
        // (carry_conditional_coupling off, carry_coupling_joint_whiten on, or the
        // carry never warmed), not a quiet run; a histogram sitting below the
        // station's carried-column count says the sibling columns were being
        // marginalized out (freed / stale / reseed), not carried; fallbacks are the
        // station-epochs that got F1/diagonal rows instead of the block.
        if (ctx.knobs.pl_local_block)
            log->info("Network (leveling local-block F3){}: {} block(s) whitened, {} row(s) "
                      "emitted, {} column(s) marginalized at inject, {} pair(s) zeroed "
                      "(harvest-stamp mismatch), {} column(s) refused, {} factorization "
                      "fallback(s); block dim histogram <10:{} 10-24:{} 25-49:{} 50-74:{} "
                      "75+:{} (max {}); within-epoch refloat re-whiten {} ok / {} failed; "
                      "age gate {} ep ({}): {} young column(s) kept out of the block.  "
                      "ZWD in block: {} col(s), {} diagonal fallback(s), {} missing cross "
                      "term(s), {} RAIM-pool admission(s) of the ZWD row{}.  "
                      "Iono in block: {} col(s), {} fallback(s) (block non-SPD ⇒ NOT "
                      "folded; this card has no diagonal iono row by design), {} missing "
                      "cross term(s), {} RAIM-pool admission(s), {} erased by T4, {} "
                      "dropped at gaps; q {:.3e} m² per epoch at L1, gap bound {:.0f} s{}.  "
                      "Rx clock in block: {} col(s) ({} ISB), {} not folded (block non-SPD "
                      "⇒ carried nothing; this card has no diagonal clock row by design), "
                      "{} missing cross term(s), {} RAIM-pool admission(s), {} refused "
                      "(innovation: a clock JUMP at >{:.1f} sigma, carry erased), {} dropped "
                      "at gaps; bias q {:.3e} m² per epoch, drift q {:.3e} m²/s² per epoch "
                      "from {:.3e}, gap bound {:.0f} s{}",
                      (cliff_live && config_.networkCarryConditionalCoupling()
                       && !config_.networkCarryCouplingJointWhiten())
                          ? "" : " ⚠ CHANNEL NOT ARMED on this posture (phase_leveling "
                                 "off, carry_invariant on, carry_conditional_coupling "
                                 "off, or carry_coupling_joint_whiten on) — a 0 here "
                                 "means UNWIRED, not clean",
                      C.n_pl_blk_blocks, C.n_pl_blk_rows, C.n_pl_blk_marg_cols,
                      C.n_pl_blk_pair_missing, C.n_pl_blk_col_refused, C.n_pl_blk_fallback,
                      C.n_pl_blk_hist[0], C.n_pl_blk_hist[1], C.n_pl_blk_hist[2],
                      C.n_pl_blk_hist[3], C.n_pl_blk_hist[4], C.n_pl_blk_max_dim,
                      C.n_pl_blk_refloat_rewhiten, C.n_pl_blk_refloat_fail,
                      config_.networkPhaseLevelingLocalBlockMinAgeEp(),
                      config_.networkPhaseLevelingLocalBlockYoungMode(), C.n_pl_blk_young_cols,
                      // B9-in-block (network.carry_zwd_in_block) — invariant #4: printed on
                      // the F3 line because the block IS the vehicle.  0 columns under an
                      // armed knob is UNWIRED, not clean; a large fallback count says the
                      // ZWD kept taking the diagonal B9 row this card exists to replace
                      // (n_pl_blk_fallback above attributes how many of those were a
                      // non-SPD block vs a station-epoch with no block at all).
                      C.n_pl_blk_zwd_cols, C.n_pl_blk_zwd_fallback_diag,
                      C.n_pl_blk_zwd_missing_cross, C.n_raim_pool_zwd_rows,
                      config_.networkCarryZwdInBlock()
                          ? (C.n_pl_blk_zwd_cols == 0
                                 ? "  ⚠ 0 ZWD columns under an ARMED carry_zwd_in_block — "
                                   "the channel is UNWIRED (no station-epoch reached the "
                                   "block with a live ZWD carry), not quiet."
                                 : "")
                          : " (knob OFF — these are structurally 0)",
                      // I-in-block (network.carry_iono_in_block) — invariant #4, on the F3
                      // line because the block IS the vehicle.  0 columns under an armed
                      // knob is UNWIRED, not clean; a large not-folded count says the
                      // station blocks kept failing to factorize, and since this card has
                      // no diagonal row that means the iono carry did NOTHING there.
                      C.n_pl_blk_iono_cols, C.n_pl_blk_iono_not_folded,
                      C.n_pl_blk_iono_missing_cross, C.n_raim_pool_iono_rows,
                      C.n_iono_blk_erased_t4, C.n_iono_blk_gap_drop,
                      config_.networkCarryIonoInBlockQM2(),
                      config_.networkCarryIonoInBlockMaxGapS(),
                      config_.networkCarryIonoInBlock()
                          ? (C.n_pl_blk_iono_cols == 0
                                 ? "  ⚠ ARMED AND INERT: 0 iono columns under an ARMED "
                                   "carry_iono_in_block — no station-epoch reached the "
                                   "block with a live per-LOS iono carry, which is a "
                                   "MISWIRE, not a quiet run."
                                 : "")
                          : " (knob OFF — these are structurally 0)",
                      // RX-CLK-in-block (network.carry_rx_clock_in_block) — invariant #4,
                      // on the F3 line because the block IS the vehicle.  0 columns under
                      // an armed knob is UNWIRED, not clean.  ⚠ Read `refused` as the
                      // clock-JUMP census: it is the ONLY jump detector the network's
                      // receiver clock has, so a field lineage with 0 refusals over a day
                      // is either a very well behaved receiver fleet or a gate set too
                      // loose — and a large count means the carry is being rebuilt
                      // constantly, i.e. the drift model is not tracking.
                      C.n_pl_blk_rxclk_cols, C.n_pl_blk_rxclk_isb_cols,
                      C.n_pl_blk_rxclk_not_folded, C.n_pl_blk_rxclk_missing_cross,
                      C.n_raim_pool_rxclk_rows, C.n_rxclk_carry_refused,
                      config_.networkCarryRxClockJumpK(), C.n_rxclk_carry_gap_drop,
                      config_.networkCarryRxClockQM2(), config_.networkCarryRxClockRateQ(),
                      config_.networkCarryRxClockRateVar0(),
                      config_.networkCarryRxClockMaxGapS(),
                      config_.networkCarryRxClockInBlock()
                          ? (C.n_pl_blk_rxclk_cols == 0
                                 ? "  ⚠ ARMED AND INERT: 0 receiver-clock columns under an "
                                   "ARMED carry_rx_clock_in_block — no station-epoch reached "
                                   "the block with a live clock carry, which is a MISWIRE, "
                                   "not a quiet run."
                                 : "")
                          : " (knob OFF — these are structurally 0)");
        // The jump gate's own SCALE, printed only when the card is armed (an OFF
        // run's log is byte-identical).  `refused` above is a count; without the
        // metres a report can only quote k, and k alone does not say whether a given
        // event is above or below the gate.  var_pred is per station-epoch (harvested
        // variance + drift extrapolation + q), so the gate is a distribution — mean,
        // min and max.  `max |innov|` is the loudest step the gate actually saw; on a
        // quiet bed with no injected event it is the noise ceiling, and an injected
        // rx_clock_step must clear the gate at its own station to be refused.
        if (config_.networkCarryRxClockInBlock())
            log->info("Network (F3 carry, rx-clock JUMP GATE): k = {:.1f} sigma over {} "
                      "station-epoch test(s) ⇒ gate mean {:.4f} m, min {:.4f} m, max {:.4f} m; "
                      "largest |innovation| seen {:.4f} m; {} refusal(s).  An injected or real "
                      "step BELOW the gate is absorbed by the carried prior (it FIGHTS the "
                      "step); above it the carry is erased and re-born cold.",
                      config_.networkCarryRxClockJumpK(), C.n_rxclk_gate_tests,
                      C.n_rxclk_gate_tests ? C.sum_rxclk_gate_m / C.n_rxclk_gate_tests : 0.0,
                      C.min_rxclk_gate_m, C.max_rxclk_gate_m, C.max_rxclk_innov_m,
                      C.n_rxclk_carry_refused);
        // AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2,
        // SPEC_amb_iono_split_overcount §6.17–§6.18) — the ON-path telemetry
        // (invariant #4).  Printed ONLY when armed, so an OFF log is byte-identical.
        // What to read: 0 inflated column-epochs under an armed knob is ARMED AND
        // INERT — the stamps never reached an inject, not a quiet run; a mean age near
        // the ceiling says the population being inflated is nearly mature and the knob
        // is buying almost nothing; and the stamp split (created vs freed) says which
        // event is actually producing the code-quality levels on this bed.  The
        // block-scaled count read against the inflated count separates "computed" from
        // "actually applied inside Σ" — a gap is columns that took the per-column route
        // (F3 off, factorization fallback, young-leg gate) or were refused by the
        // block's scalar screen.
        if (config_.networkPhaseLevelingYoungInflateVarM2() > 0.0)
            log->info("Network (young-arc inflate): {} column-epoch(s) inflated (V0 {:.4g} m², "
                      "tau {:.4g} ep, max age {} ep), mean age {:.2f} ep, Σ var_add {:.4g} m²; "
                      "{} of them congruence-scaled inside an F3 block; {} stamped "
                      "column-epoch(s) skipped for age > max; {} re-level stamp(s) set "
                      "(created {} / freed {}).{}",
                      C.n_yng_cols,
                      config_.networkPhaseLevelingYoungInflateVarM2(),
                      config_.networkPhaseLevelingYoungInflateTauEp(),
                      config_.networkPhaseLevelingYoungInflateMaxAgeEp(),
                      C.n_yng_cols > 0
                          ? C.yng_age_sum / static_cast<double>(C.n_yng_cols) : 0.0,
                      C.yng_var_add_sum, C.n_pl_blk_yng_scaled, C.n_yng_skip_age,
                      C.n_yng_stamp_create + C.n_yng_stamp_free,
                      C.n_yng_stamp_create, C.n_yng_stamp_free,
                      C.n_yng_cols == 0
                          ? "  ⚠ ARMED AND INERT: 0 column-epochs inflated under an ARMED "
                            "phase_leveling_young_inflate_var_m2 — no carried column ever "
                            "reached an inject inside its age window, which is a MISWIRE "
                            "(no stamps, no leveling inject, or an epoch spacing the run "
                            "never resolved), not a quiet run."
                          : "");
        // Seam-scoped ZWD re-seed (network.zwd_seam_reseed) — the ON-path telemetry
        // (standing invariant #4): an armed knob with 0 folds means the window never
        // armed (no warm resume, or the snapshot carried no Z rows) or every fold
        // was skipped (stale entries / carry_warm cold) — the resume-time ARMED /
        // INERT log line says which.
        // Resume FRAME restore + slip warm-up (§18.10) — ON-path telemetry (invariant
        // #4): the re-coupling edge's identities.  0 anchors under an ARMED knob means
        // the snapshot carried no frame rows (a v1 file) or no resume happened.
        if (config_.networkResumeFrameRestore() || config_.networkResumeSlipWarmupEp() > 0)
            log->info("Network (resume frame restore): {} phase-anchor incumbent(s), {} sticky "
                      "pin target(s), {} clock-anchor sat state(s) ({} anchored), {} CLK-R "
                      "held-frame key(s), {} ledger entr(ies) primed, restored at the seam; MW/LLI drain diagnostic-only "
                      "for {} post-resume epoch(s) (knob {}).{}",
                      C.n_resume_frame_anchors, C.n_resume_frame_targets,
                      C.n_resume_frame_cta, C.n_resume_frame_cta_anchored,
                      C.n_resume_frame_clk_keys, C.n_resume_frame_ledger, C.n_resume_slip_warmup_epochs,
                      config_.networkResumeSlipWarmupEp(),
                      (config_.networkResumeFrameRestore() && C.n_resume_frame_anchors == 0)
                          ? "  ⚠ 0 anchors under an ARMED frame restore — the snapshot "
                            "carried no frame rows (pre-v2 writer?) or no warm resume "
                            "happened; check the resume log for the FRAME line."
                          : "");
        // §18.12 coupling-regressor restore — the RE-COUPLING edge of the seam (standing
        // invariant #7): what came back, and what it actually BOUND TO at the first fold.
        // Restoring (value, var) alone injects a carried leveling row as a PLAIN DIAGONAL
        // prior asserted at the PURE CONDITIONAL variance — a combination no continuous
        // epoch produces — so the row drives the shared border by the full α‖M_d‖ at
        // α ≈ 1/cond_var ≈ 1e6 m⁻², four orders above the Q3 anchored clock prior's
        // 1/9e-5 ≈ 1.1e4 m⁻².  That is the §18.11 first-fold trip count, so "how many rows
        // came back" is NOT the readout — "how many of their terms resolved to a live
        // column" is, and both edges are printed here.
        //
        // The census vectors hold ONE entry per row of the FIRST post-resume harvest (the
        // measure-only observatory in networkHarvestLocalCarries), so they are reduced HERE
        // rather than accumulated online: nth_element is O(n), the run is over, and the
        // observatory stays byte-identical on every solve artifact.  Reduced on COPIES so
        // the readout never permutes carried state.
        double rel_p50 = 0.0, rel_p90 = 0.0, rel_max = 0.0, rhs_p50 = 0.0, rhs_max = 0.0;
        {
            using Diff = std::vector<double>::difference_type;
            std::vector<double> cen_rel = ctx.carry.resume_cpl_census_rel;
            if (!cen_rel.empty()) {
                const std::size_t i50 = cen_rel.size() / 2;
                const std::size_t i90 = (cen_rel.size() * 9) / 10;   // < size for size >= 1
                std::nth_element(cen_rel.begin(),
                                 cen_rel.begin() + static_cast<Diff>(i50),
                                 cen_rel.end());
                rel_p50 = cen_rel[i50];
                std::nth_element(cen_rel.begin(),
                                 cen_rel.begin() + static_cast<Diff>(i90),
                                 cen_rel.end());
                rel_p90 = cen_rel[i90];
                rel_max = *std::max_element(cen_rel.begin(), cen_rel.end());
            }
            std::vector<double> cen_rhs = ctx.carry.resume_cpl_census_rhs;
            if (!cen_rhs.empty()) {
                const std::size_t j50 = cen_rhs.size() / 2;
                std::nth_element(cen_rhs.begin(),
                                 cen_rhs.begin() + static_cast<Diff>(j50),
                                 cen_rhs.end());
                rhs_p50 = cen_rhs[j50];
                rhs_max = *std::max_element(cen_rhs.begin(), cen_rhs.end());
            }
        }
        if (config_.networkResumeCouplingRestore())
            log->info("Network (resume coupling restore): {} L row(s) + {} Z row(s), {} "
                      "regressor(s) over a {}-column dictionary ({} orphan row(s)); {} "
                      "clock reinit seed(s) planted at the seam.  First fold: {} present, "
                      "{} departed/diverted ({:.1f} % present), {} border key(s) folded "
                      "DIAGONALLY (the known one-epoch C1 asymmetry).  First-harvest "
                      "census over {} row(s) / {} matched term(s): |M-M_d|/max|M_d| p50 "
                      "{:.3e}, p90 {:.3e}, max {:.3e}; |delta sum M*s_prev| p50 {:.3e} m, "
                      "max {:.3e} m ({} restored identit(ies) not re-harvested).{}",
                      C.n_resume_cpl_rows, C.n_resume_cpl_zrows, C.n_resume_cpl_terms,
                      C.n_resume_cpl_dict, C.n_resume_cpl_orphan,
                      C.n_resume_cpl_seed_reinit,
                      C.n_resume_cpl_seam_present, C.n_resume_cpl_seam_departed,
                      (C.n_resume_cpl_seam_present + C.n_resume_cpl_seam_departed) > 0
                          ? 100.0 * static_cast<double>(C.n_resume_cpl_seam_present)
                            / static_cast<double>(C.n_resume_cpl_seam_present
                                                + C.n_resume_cpl_seam_departed)
                          : 0.0,
                      C.n_resume_cpl_diag_border,
                      C.n_resume_cpl_census_rows, C.n_resume_cpl_census_terms,
                      rel_p50, rel_p90, rel_max, rhs_p50, rhs_max,
                      C.n_resume_cpl_census_unmatched,
                      C.n_resume_cpl_rows == 0
                          ? "  ⚠ 0 rows under an ARMED coupling restore — the snapshot "
                            "carried no QS/QL records (a pre-v3 writer, or a writer "
                            "without network.state_snapshot_coupling_capture), or no warm "
                            "resume happened: every restored leveling row injected as a "
                            "plain DIAGONAL prior, which IS the §18.11 defect."
                      : (C.n_resume_cpl_seam_present * 10
                             < (C.n_resume_cpl_seam_present
                                + C.n_resume_cpl_seam_departed) * 9)
                          ? "  ⚠ under 90 % of the restored terms resolved to a live column "
                            "at the first warm epoch — the rows were SOFTENED by "
                            "Sum M^2*P_prev, which is rung 3's mechanism by another route; "
                            "grade the h12 consumer before trusting the clock result."
                          : "");
        if (config_.networkResumeCouplingRestore())
            log->info("Network (resume coupling restore, dense border): {} dense-carry key(s) "
                      "restored from the snapshot's QK/QP records ({} dropped: no restored D "
                      "row); {} border key(s) folded DIAGONALLY at the first warm fold.",
                      C.n_resume_cpl_cov_keys, C.n_resume_cpl_cov_dropped,
                      C.n_resume_cpl_diag_border);
        // READMIT continuity (E2 seed-first re-admission + the block re-coupling edge) —
        // BOTH edges printed (standing invariant #7): parked = what left; seeded = what came
        // back; moved / outlier = what the data rejected; the coupling window's diverted-term
        // count is the block edge's own denominator.  0 seeds under an armed horizon with
        // parks > 0 reads as INERT-on-this-bed (invariant #4), and says so.
        if (config_.networkReadmitSeedHorizonS() > 0.0
            || config_.networkReadmitCouplingMargEp() > 0)
            log->info("Network (READMIT continuity): {} re-admission(s) detected; seed horizon {} "
                      "s — {} entr(ies) parked at departures, {} clock + {} code-bias column(s) "
                      "seeded on return ({} clock seed(s) folded ANCHORED), {} parked entr(ies) "
                      "expired, {} dropped at a datum change, {} dropped unconsumed; Q3 trips "
                      "within {} s of a clock seed: {}; stash-side seed checks {} ({} over "
                      "k={}); coupling windows {} epoch(s): {} opened, {} term(s) diverted; {} "
                      "parked entr(ies) excluded from snapshots.{}",
                      C.n_readmit_events, config_.networkReadmitSeedHorizonS(),
                      C.n_readmit_parked, C.n_readmit_seeded_clk, C.n_readmit_seeded_cb,
                      C.n_readmit_seeded_anch, C.n_readmit_expired, C.n_readmit_dropped_datum,
                      C.n_readmit_dropped_stale, kReadmitVerifyWindowS,
                      C.n_readmit_seed_moved, C.n_readmit_seed_check,
                      C.n_readmit_seed_outlier, kReadmitSeedK,
                      config_.networkReadmitCouplingMargEp(), C.n_readmit_marg_windows,
                      C.n_cpl_readmit_diverted, C.n_snap_parked_excluded,
                      (config_.networkReadmitSeedHorizonS() > 0.0 && C.n_readmit_parked > 0
                       && C.n_readmit_seeded_clk + C.n_readmit_seeded_cb == 0)
                          ? "  ⚠ entries were parked but NONE seeded — every return expired or "
                            "was dropped; the horizon is shorter than this bed's absences."
                          : "");
        // §6.12.5 — the DD trace's REFERENCE identity, on both policies.  Printed whenever the
        // trace ran, including under the `lowest` default: the switch rate was always there and
        // always silent, and a grade built on a single reference element has to state that
        // element's behaviour (SPEC_synthetic §13.8).  `age_win` is the invariant-#4 denominator
        // for `oldest` — 0 means the policy never elected anything `lowest` would not have, so the
        // arm is INERT and its A/B proves nothing no matter how clean the file looks.
        if (nc.trace_dd_fractional && C.n_ddfrac_ref_epochs > 0) {
            const bool oldest = nc.trace_dd_fractional_ref == NetworkDdFracRef::Oldest;
            log->info("Network (DD-frac reference, §6.12.5): policy '{}' — {} election(s) over the "
                      "traced epochs, {} reference switch(es){}.  Every chord in "
                      "network_dd_fractional.csv carries the elected satellite's own between-station "
                      "error; grade ref-free (scripts/simnet_dd_reref.py, "
                      "simnet_amb_integer.py --ref-free) whichever policy ran.{}",
                      oldest ? "oldest" : "lowest", C.n_ddfrac_ref_epochs, C.n_ddfrac_ref_switch,
                      oldest ? fmt::format(", {} election(s) differed from 'lowest'",
                                           C.n_ddfrac_ref_age_win)
                             : std::string(),
                      (oldest && C.n_ddfrac_ref_age_win == 0)
                          ? "  ⚠ INERT: 'oldest' never elected a satellite 'lowest' would not have, "
                            "so this run is byte-identical to the default and cannot A/B the policy."
                          : "");
        }
        if (config_.networkZwdSeamReseed())
            log->info("Network (ZWD seam re-seed): {} station-fold(s) over {} epoch(s) of the "
                      "post-resume window (horizon {} epochs).{}",
                      C.n_zwd_seam_folds, C.n_zwd_seam_epochs,
                      config_.networkZwdSeamReseedEpochs(),
                      C.n_zwd_seam_folds == 0
                          ? "  ⚠ 0 folds under an ARMED knob — the wiring was inert this "
                            "run; check the resume log for the ARMED/INERT line."
                          : "");
        log->info("Network (leveling-carry retirement census) [C] ONE-SOLVE "
                  "REFLOATS: {} T4 attribution, {} T3 consensus (quality-selected), and {} "
                  "entr(ies) in {} whole-fleet SH-convergence flush(es) — the largest "
                  "single removal in the producer, but the arcs stay tracked and the "
                  "next harvest re-stashes them, so it costs one solve, not a departure.  "
                  "⚠ These are a run-constant EITHER/OR, not a split — "
                  "network.raim_attribution is {} on this run, so the other channel "
                  "is structurally 0 and its zero means \"path not armed\", not "
                  "\"path found nothing\".  [D] ONE-SOLVE FRAME SKIPS: {} eq34 "
                  "chain-datum re-reference, {} phase-datum family re-reference, {} "
                  "datum-slip re-reference.  [RESPONSE] {} A4 withhold free-erase.",
                  C.n_cr_raim_attrib, C.n_cr_raim_consensus,
                  C.n_cr_sh_conv_flush, C.n_cr_sh_conv_flushes,
                  config_.networkRaimAttribution() ? "ARMED" : "off",
                  C.n_cr_eq34_reref, C.n_cr_phase_datum_reref,
                  slip_qc.counters.datum_slip_carry_erases, C.n_wh_free_erases);
    }

    // ---- Phase wind-up census (estimation.trace_windup_census) -------------
    // The FIRST instrument to reach the network path's wind-up channel:
    // windup_trace.csv is written only from Application::run (one caller,
    // Application.cpp), which runNetwork never reaches, so until now the
    // fractional staircase was measured on exactly one PPP station and scaled
    // by hand to 63.  Read-only — arming this cannot move a byte of the solve.
    if (config_.traceWindUpCensus()) {
        GnssCorrections::WindUpCensus c{};
        int n_sta = 0;
        for (const auto& st : stations) {
            if (!st.model) continue;
            const auto& w = st.model->windUpCensus();
            if (w.steps == 0 && w.restarts == 0 && w.backwards == 0) continue;
            ++n_sta;
            c.steps            += w.steps;
            c.wraps            += w.wraps;
            c.restarts         += w.restarts;
            c.gap_steps        += w.gap_steps;
            c.backwards        += w.backwards;
            c.gap_lt_wu        += w.gap_lt_wu;
            c.gap_wu_to_carry  += w.gap_wu_to_carry;
            c.gap_gt_carry     += w.gap_gt_carry;
            c.sum_abs_step_cyc += w.sum_abs_step_cyc;
            c.max_abs_step_in_cyc  = std::max(c.max_abs_step_in_cyc,  w.max_abs_step_in_cyc);
            c.max_abs_step_gap_cyc = std::max(c.max_abs_step_gap_cyc, w.max_abs_step_gap_cyc);
        }
        const bool acc      = config_.accumulatePhaseWindUp();
        const double wu_gap = config_.phaseWindUpMaxGapS();
        const double cy_gap = config_.networkPhaseLevelingMaxGapS();
        // The exposure narrative is only true when the carry OUTLIVES the
        // accumulator.  At the loader default it does not (120 s vs 300 s), and
        // printing the sentence anyway would let a structural zero read as a
        // clean bill of health — the invariant-#4 failure mode this whole card
        // exists to avoid.
        const bool exposure_ordering = cy_gap > wu_gap;
        log->info("Network (wind-up census, SPEC_phase_windup_sign §10): mode {}, "
                  "horizons wind-up {:.0f} s / leveling carry {:.0f} s ({}).  {} "
                  "station(s), {} forward transitions ({} out-of-order, excluded).  "
                  "IN-horizon: {} transitions, {} step(s) >= 0.9 cyc, mean |step| "
                  "{:.4f} cyc, max {:.3f}.  OUT-of-horizon: {} transitions total "
                  "(attributed: {} between the horizons, {} beyond both — the two "
                  "buckets are only filled when the carry horizon is > 0, so trust "
                  "the TOTAL, not their sum), {} of which stepped >= 0.9 cyc "
                  "(max {:.3f}).  "
                  "Accumulator RESTARTS (its own verdict, not a threshold): {}.",
                  acc ? "ACCUMULATED" : "fractional", wu_gap, cy_gap,
                  exposure_ordering ? "carry outlives the accumulator"
                                    : "⚠ INVERTED — carry expires FIRST, so the "
                                      "between-horizons bucket is structurally empty "
                                      "and says nothing about the invariant-#1 hole",
                  n_sta, c.steps, c.backwards,
                  c.gap_lt_wu, c.wraps,
                  c.gap_lt_wu > 0 ? c.sum_abs_step_cyc / static_cast<double>(c.gap_lt_wu) : 0.0,
                  c.max_abs_step_in_cyc,
                  c.steps - c.gap_lt_wu,
                  c.gap_wu_to_carry, c.gap_gt_carry, c.gap_steps,
                  c.max_abs_step_gap_cyc, c.restarts);
        if (!acc)
            log->info("Network (wind-up census) READING [fractional]: the {} "
                      "in-horizon step(s) >= 0.9 cyc ARE the one-cycle staircase — "
                      "applied mid-pass in good data, band-common (so ΔWL = 0 and MW "
                      "is structurally blind), and invisible to every slip detector.  "
                      "`restarts` is 0 by construction here: there is no accumulator.  "
                      "{}  0 transitions on an armed run = dead wiring (invariant #4).",
                      c.wraps,
                      exposure_ordering
                          ? fmt::format("The {} between-horizon transition(s) are the "
                                        "population that WOULD restart the accumulator "
                                        "while the leveling carry survives — that is the "
                                        "invariant-#1 exposure blocking "
                                        "estimation.accumulate_phase_windup on this "
                                        "lineage, and a response is a prerequisite for "
                                        "any accumulate arm.",
                                        c.gap_wu_to_carry)
                          : fmt::format("The horizon ordering is inverted on this "
                                        "posture, so this run CANNOT measure the "
                                        "invariant-#1 exposure; re-run with "
                                        "network.phase_leveling_max_gap_s > {:.0f} s "
                                        "(the field lineages use 900) to size it.",
                                        wu_gap));
        else
            log->info("Network (wind-up census) READING [accumulated]: `wraps` is 0 by "
                      "CONSTRUCTION in this posture — every in-horizon transition is "
                      "unwrapped into ±0.5 cyc, so no threshold on the applied step can "
                      "see anything; do NOT read it as \"no restarts\".  The restart "
                      "population is the {} counted above, each applying an exact "
                      "integer-cycle step to every signal of its satellite — {}",
                      c.restarts,
                      config_.networkCycleSlipWindupRestartRefloat()
                          ? "the §10.4 refloat response is ARMED (see its line below)."
                          : "with NO ambiguity response armed this run (invariants #1 "
                            "and #7; arm network.cycle_slip.windup_restart_refloat).");
    }
    // §10.4 wind-up restart RESPONSE readout (the wired invariant-#1 response;
    // counters are the ON-path telemetry — invariant #4).
    if (config_.networkCycleSlipWindupRestartRefloat()) {
        const auto& qcc = slip_qc.counters;
        const long long closure = qcc.wu_restart_refloated + qcc.wu_restart_cold
                                + qcc.wu_restart_beyond_carry + qcc.wu_restart_stale;
        log->info("Network (wind-up restart response, SPEC_phase_windup_sign §10.4): "
                  "{} restart event(s) drained → {} RESPONDED ({} arc(s) refloated), "
                  "{} cold (diagnostic), {} beyond the carry horizon (carry died with "
                  "the gap — no response owed), {} STALE refused (ledger from an "
                  "earlier epoch — never respond to history).  CLOSURE {} vs events "
                  "{} ({}).  0 responded on an accumulated run whose census counts "
                  "restarts = dead wiring (invariant #4); the lane observatory grades "
                  "this response under cause=WU.",
                  qcc.wu_restart_events, qcc.wu_restart_refloated,
                  qcc.wu_restart_arcs, qcc.wu_restart_cold,
                  qcc.wu_restart_beyond_carry, qcc.wu_restart_stale,
                  closure, qcc.wu_restart_events,
                  closure == qcc.wu_restart_events ? "OK" : "*** MISWIRE ***");
    }

    // ---- ANCHOR-SLIP HANDOVER (docs/SPEC_event_harness.md §12.7) ------------
    // Invariant #4: these counters ARE the ON-path telemetry.  The card's whole
    // claim is that a slip landing on a GAUGE arc is a different event from one
    // landing anywhere else, so an armed run reporting 0 staged on a lineage whose
    // detectors fired has not proved the network is quiet — it has proved nothing.
    if (config_.networkCycleSlipAnchorSlipHandover()) {
        const auto& C = ctx.counters;
        log->info("Network (anchor-slip handovers, §12.7): {} family(ies) staged over "
                  "{} epoch(s) — {} handed over ({} continuous / {} COLD FALLBACK, the "
                  "successor had no usable carried value so the gauge re-realized "
                  "anyway), {} with NO successor (the freed anchor was the family's only "
                  "tracker — unpinned for that epoch), {} inert (the family was not in "
                  "the epoch's union: gated, bridged or pruned).",
                  C.n_anc_slip_staged, C.n_anc_slip_epochs, C.n_anc_slip_handed,
                  C.n_anc_slip_continuous, C.n_anc_slip_cold,
                  C.n_anc_slip_no_successor, C.n_anc_slip_inert);
        if (C.n_anc_slip_staged == 0)
            log->info("Network (anchor-slip handovers): ZERO staged this run — no slip "
                      "response freed an ANCHOR arc.  On a run whose detectors DID fire "
                      "that is the honest zero (the slips missed the gauge arcs); on one "
                      "where they did not fire at all it says nothing about this card.");
        else if (C.n_anc_slip_handed == 0)
            log->warn("Network (anchor-slip handovers): DEAD WIRING — {} directive(s) "
                      "staged and NOT ONE reached a family (staged {} / inert {} / no "
                      "successor {}).  The ban never met the election; invariant #4.",
                      C.n_anc_slip_staged, C.n_anc_slip_staged, C.n_anc_slip_inert,
                      C.n_anc_slip_no_successor);
    }

    // ---- MW DETECT-ONLY ADMISSION (network.cycle_slip.mw_evidence_refloat) --
    // Printed ALWAYS, armed or not: the drop populations were silent before this
    // card (no counter, no event row), and the would-admit count is what sizes
    // the knob on a lineage without running it armed.  An OFF run reporting
    // would-admit 0 says the card is inert there; an ON run reporting evidence 0
    // with would-admit > 0 on the same leg is dead wiring (invariant #4).
    {
        const auto& C = ctx.counters;
        log->info("Network (MW detect-only admission): legacy {}, evidence {} "
                  "[knob {}; would-admit {}]; dropped loose {} / unknown {}.",
                  C.n_mw_admit_legacy, C.n_mw_evidence_admitted,
                  config_.networkCycleSlipMwEvidenceRefloat() ? "on" : "off",
                  C.n_mw_evidence_would_admit, C.n_mw_drop_loose,
                  C.n_mw_drop_unknown);
    }

    // ---- COMMON-BAND SCOPE (network.cycle_slip.common_band_scope, §14) ------
    // Printed ALWAYS, armed or not, for the same reason as the clause above: the
    // would-narrow count sizes the card on a lineage without running it armed,
    // and an ARMED run reporting narrowed 0 while verdicts > 0 is dead wiring,
    // not a quiet network (invariant #4).  `bands kept` is the headline — under
    // the all-band union every live band of a fired arc would have been freed.
    {
        const auto& C = ctx.counters;
        log->info("Network (common-band scope): verdicts {}, narrowed {} (bands kept "
                  "{}), all-band {} (no exoneration {} / inconsistent {}), IF-TD vetoes "
                  "{}, immature pairs {}, weak silences {} [knob {}{}].",
                  C.n_cb_verdicts, C.n_cb_narrowed, C.n_cb_bands_kept,
                  C.n_cb_all_no_exoneration + C.n_cb_all_inconsistent,
                  C.n_cb_all_no_exoneration, C.n_cb_all_inconsistent,
                  C.n_cb_iftd_veto, C.n_cb_immature_pairs, C.n_cb_weak_silence,
                  config_.networkCycleSlipCommonBandScope() ? "on" : "off",
                  config_.networkCycleSlipCommonBandScope()
                      ? std::string{}
                      : fmt::format("; would-narrow {}", C.n_cb_would_narrow));
    }

    // ---- §Z BAND-SCOPED RESPONSES (docs/SPEC_event_harness.md) --------------
    // Invariant #4: these counters ARE the ON-path telemetry.  Either knob armed
    // with its headline count at 0 on a run whose T4 tier fires (or whose
    // whole-satellite detectors fire) is dead wiring, not a quiet run — the whole
    // point of the card is that the response population is large (~21 T4 arc
    // erases per station-hour on the slipped bed).
    {
        const auto& nc = config_.getNetworkConfig();
        const bool t4_bs = nc.raim_erase_band_scope
                               == NetworkRaimEraseBandScope::MinKeepOne;
        const bool det_bs = nc.cycle_slip_refloat_band_scope
                               == NetworkRefloatBandScope::Implicated;
        if (t4_bs || det_bs) {
            const auto& C = ctx.counters;
            const auto& Q = slip_qc.counters;
            log->info("Network (band-scoped responses, §Z): T4 erases {} cell(s) → "
                      "{} band(s) erased / {} kept ({} kept over gate — every band of "
                      "the cell stood above k·σ̂₀ so the pin is the least-BAD band, "
                      "{} single-band cell(s) skipped outright); detector refloats "
                      "{} arc(s) → {} band(s) freed / {} kept ({} all-band fallback(s) "
                      "— the honest cost line: a whole-satellite loss of lock where a "
                      "truly-slipped band stayed pinned; {} of the pins were chosen on "
                      "RAW MW/GF per-band evidence, the rest on the lexical fallback; "
                      "{} single-band arc(s) freed anyway, nothing to spare; {} "
                      "TOO LATE — every band was already freed by an EARLIER drain "
                      "this epoch, so nothing could be pinned: NONZERO means a "
                      "freeing site upstream is not routed through the scope and "
                      "the per-epoch invariant is not held, invariant #4).  "
                      "Scope: T4={} detector={}.",
                      C.n_bscope_t4_cells, C.n_bscope_t4_erased, C.n_bscope_t4_kept,
                      C.n_bscope_t4_kept_over_gate, C.n_bscope_t4_single_band,
                      Q.bscope_arcs, Q.bscope_freed, Q.bscope_kept,
                      Q.bscope_all_band_fallback, Q.bscope_measured,
                      Q.bscope_single_band, Q.bscope_too_late,
                      t4_bs ? "min_keep_one" : "all",
                      det_bs ? "implicated" : "all");
        }
    }

    // ---- Rung 3 v3: carry-domain lane observatory (SPEC_lane_slip_response §7.3)
    if (config_.networkCycleSlipRelevelLaneSnap()) {
        const auto& C = ctx.counters;
        const long long closure = C.n_lane_measured + C.n_lane_skip_no_row
                                + C.n_lane_skip_no_mw + C.n_lane_skip_unmeasured
                                + C.n_lane_skip_pair_changed
                                + C.n_lane_skip_no_freed_band
                                + C.n_lane_skip_no_pre + C.n_lane_skip_stale_pre
                                + C.n_lane_skip_no_post + C.n_lane_skip_bridged
                                + C.n_lane_skip_bad_lambda;
        const long long hsum = C.n_lane_hist[0] + C.n_lane_hist[1] + C.n_lane_hist[2]
                             + C.n_lane_hist[3] + C.n_lane_hist[4] + C.n_lane_hist[5]
                             + C.n_lane_hist[6] + C.n_lane_hist[7] + C.n_lane_hist[8];
        log->info("Network (lane observatory v3, SPEC_lane_slip_response §7.3): {} "
                  "attempt(s) [(station,sat) with >= 1 freed band]; "
                  "{} MEASURED ({} vertex-wide / {} band-scoped), mean |delta| {:.3f} "
                  "cyc, max {:.3f}.  |delta| histogram (cyc): <0.1 {} | <0.25 {} | "
                  "<0.5 {} | <1 {} | <2 {} | <5 {} | <10 {} | <25 {} | >=25 {}.  "
                  "Skipped: {} no raw row, {} no MW pair, {} MW unmeasured (no adjacent "
                  "predecessor — if this is ~100 % of attempts the QUALIFICATION pass did "
                  "not run and the card is ARMED-BUT-INERT, not null), {} pair changed, "
                  "{} no freed band in THIS pair (a third band was freed), {} no pre-event "
                  "carry, {} stale pre-event carry, {} not RE-STASHED this epoch, {} "
                  "delta-bridged, {} bad wavelength.  CLOSURE {} vs attempts {} ({}); histogram sum {} vs "
                  "measured {} ({}).  READING: this is MEASURE-ONLY — an armed run is "
                  "byte-identical on every solve artifact, so a difference in products "
                  "is a bug, not an effect.  The carry-frame hypothesis predicts the "
                  "mass of |delta| BELOW ~2 cyc (residual eps_L(t+)-eps_L(t-)-eps_MW, "
                  "with eps_MW 0.351 cyc on GPS L1/L2 and 0.051 on L2/L5 EWL); the "
                  "DELETED seed site put 97 % above 25 cyc, which is what a frame "
                  "mismatch looks like.  Mass in the top bins here FALSIFIES the "
                  "carry-domain diagnosis and no apply leg should be built.  0 "
                  "attempts on a slip-bearing armed run = dead wiring (invariant #4).",
                  C.n_lane_attempts, C.n_lane_measured, C.n_lane_both_freed,
                  C.n_lane_one_freed,
                  C.n_lane_measured > 0
                      ? C.lane_abs_delta_sum_cyc / static_cast<double>(C.n_lane_measured)
                      : 0.0,
                  C.lane_abs_delta_max_cyc,
                  C.n_lane_hist[0], C.n_lane_hist[1], C.n_lane_hist[2], C.n_lane_hist[3],
                  C.n_lane_hist[4], C.n_lane_hist[5], C.n_lane_hist[6], C.n_lane_hist[7],
                  C.n_lane_hist[8],
                  C.n_lane_skip_no_row, C.n_lane_skip_no_mw,
                  C.n_lane_skip_unmeasured, C.n_lane_skip_pair_changed,
                  C.n_lane_skip_no_freed_band, C.n_lane_skip_no_pre,
                  C.n_lane_skip_stale_pre,
                  C.n_lane_skip_no_post, C.n_lane_skip_bridged, C.n_lane_skip_bad_lambda,
                  closure, C.n_lane_attempts,
                  closure == C.n_lane_attempts ? "OK" : "*** MISWIRE ***",
                  hsum, C.n_lane_measured,
                  hsum == C.n_lane_measured ? "OK" : "*** MISWIRE ***");
        // §7.6(b)+(c): the band-scoped subset histogram (vertex-wide = bin-wise
        // difference against the line above) and the freeing-cause split.  The
        // cause counts are BIT counts — a pair freed by two detectors lands in
        // two of them, so they sum to >= measured.
        const long long bs_sum = C.n_lane_hist_bandscoped[0] + C.n_lane_hist_bandscoped[1]
                               + C.n_lane_hist_bandscoped[2] + C.n_lane_hist_bandscoped[3]
                               + C.n_lane_hist_bandscoped[4] + C.n_lane_hist_bandscoped[5]
                               + C.n_lane_hist_bandscoped[6] + C.n_lane_hist_bandscoped[7]
                               + C.n_lane_hist_bandscoped[8];
        log->info("Network (lane observatory §7.6 instrument): cause split over "
                  "measured — MW {} / LLI {} / IF-TD {} (bit counts; rows may carry "
                  "several).  BAND-SCOPED |delta| histogram (cyc): <0.1 {} | <0.25 {} | "
                  "<0.5 {} | <1 {} | <2 {} | <5 {} | <10 {} | <25 {} | >=25 {} — sum {} "
                  "vs one-freed {} ({}).  Per-event rows ride the events stream "
                  "(tier=LANE_OBS; arm network.trace_events to keep them).",
                  C.n_lane_meas_mw, C.n_lane_meas_lli, C.n_lane_meas_iftd,
                  C.n_lane_hist_bandscoped[0], C.n_lane_hist_bandscoped[1],
                  C.n_lane_hist_bandscoped[2], C.n_lane_hist_bandscoped[3],
                  C.n_lane_hist_bandscoped[4], C.n_lane_hist_bandscoped[5],
                  C.n_lane_hist_bandscoped[6], C.n_lane_hist_bandscoped[7],
                  C.n_lane_hist_bandscoped[8],
                  bs_sum, C.n_lane_one_freed,
                  bs_sum == C.n_lane_one_freed ? "OK" : "*** MISWIRE ***");
        // §7.6(d): the T4-side census — the SAME delta on the RAIM
        // attribution/consensus erase channel (97.3 % of field freeing,
        // structurally invisible to free_ambs).  Read APART from the lane-cost
        // grade: dMW ≈ 0 there, so a large delta is the carry error the refloat
        // REMOVED — response working, not response damage.  0 captured on a run
        // with nonzero n_cr_raim_attrib/consensus = dead wiring (invariant #4).
        const long long t4_closure = C.n_lane_t4_measured + C.n_lane_t4_skip_no_row
                                   + C.n_lane_t4_skip_no_mw + C.n_lane_t4_skip_unmeasured
                                   + C.n_lane_t4_skip_pair_changed + C.n_lane_t4_skip_gauge
                                   + C.n_lane_t4_skip_no_pre + C.n_lane_t4_skip_stale_pre
                                   + C.n_lane_t4_skip_no_post + C.n_lane_t4_skip_bad_lambda;
        const long long t4_hsum = C.n_lane_t4_hist[0] + C.n_lane_t4_hist[1]
                                + C.n_lane_t4_hist[2] + C.n_lane_t4_hist[3]
                                + C.n_lane_t4_hist[4] + C.n_lane_t4_hist[5]
                                + C.n_lane_t4_hist[6] + C.n_lane_t4_hist[7]
                                + C.n_lane_t4_hist[8];
        log->info("Network (lane observatory T4 census, §7.6(d)): {} arc(s) captured at "
                  "the erase sites; {} attempt(s) [(station,sat) with >= 1 captured band]; "
                  "{} MEASURED, mean |delta| {:.3f} cyc, max {:.3f}.  |delta| histogram "
                  "(cyc): <0.1 {} | <0.25 {} | <0.5 {} | <1 {} | <2 {} | <5 {} | <10 {} | "
                  "<25 {} | >=25 {}.  Skipped: {} no raw row, {} no MW pair, {} MW "
                  "unmeasured, {} pair changed, {} GAUGE (the pair's (sat,σ) was "
                  "eq34-re-referenced in the measuring epoch — the capture predates the "
                  "frame step), {} no captured band, {} stale capture, {} not RE-STASHED, "
                  "{} bad wavelength.  CLOSURE {} vs attempts {} ({}); histogram sum {} vs "
                  "measured {} ({}).",
                  C.n_lane_t4_captured, C.n_lane_t4_attempts, C.n_lane_t4_measured,
                  C.n_lane_t4_measured > 0
                      ? C.lane_t4_abs_delta_sum_cyc / static_cast<double>(C.n_lane_t4_measured)
                      : 0.0,
                  C.lane_t4_abs_delta_max_cyc,
                  C.n_lane_t4_hist[0], C.n_lane_t4_hist[1], C.n_lane_t4_hist[2],
                  C.n_lane_t4_hist[3], C.n_lane_t4_hist[4], C.n_lane_t4_hist[5],
                  C.n_lane_t4_hist[6], C.n_lane_t4_hist[7], C.n_lane_t4_hist[8],
                  C.n_lane_t4_skip_no_row, C.n_lane_t4_skip_no_mw,
                  C.n_lane_t4_skip_unmeasured, C.n_lane_t4_skip_pair_changed,
                  C.n_lane_t4_skip_gauge, C.n_lane_t4_skip_no_pre,
                  C.n_lane_t4_skip_stale_pre, C.n_lane_t4_skip_no_post,
                  C.n_lane_t4_skip_bad_lambda,
                  t4_closure, C.n_lane_t4_attempts,
                  t4_closure == C.n_lane_t4_attempts ? "OK" : "*** MISWIRE ***",
                  t4_hsum, C.n_lane_t4_measured,
                  t4_hsum == C.n_lane_t4_measured ? "OK" : "*** MISWIRE ***");
    }

    // Lane snap (SPEC_lane_slip_response rung 3): the v1/v2/v2.1 SEED-SITE
    // readout is deleted with its mechanism — see the DELETED banner in
    // MeasurementModel::ensureRequiredStates.  Its counters described a write
    // that was a proven exact solve no-op, so a nonzero `snapped` was never
    // evidence of an effect; leaving the line would have kept reporting one.
    // Rung 3 v3 measures the lane in the CARRY domain and reports there.
    if (config_.networkCycleSlipRelevelRetapCorroborate())
        log->info("Network (retap corroboration, SPEC_lane_slip_response): {} keep(s) "
                  "carried positive raw-observable continuity evidence; {} refused — "
                  "raw level STEPPED; {} refused — arc UNMEASURED (no predecessor / "
                  "qualification pass idle).  Refused arcs took the full erase.  "
                  "BOTH refusal counters at 0 on a slip-bearing run = dead wiring "
                  "(invariant #4), not clean data.",
                  ctx.counters.n_relevel_retap_corr_kept,
                  ctx.counters.n_relevel_retap_corr_stepped,
                  ctx.counters.n_relevel_retap_corr_unmeasured);
    // QUEUE 8b slant-iono carry: run-end ON-path telemetry at BOTH coupling edges
    // (invariants #4/#7).  Armed with injected=0 on a slip-bearing run is a
    // MISWIRE.  refused_datum is the DECLARED-ZERO open valve (the code-datum
    // re-realization edge ships bare — see the SPEC's valve inventory entry).
    if (config_.networkCarryIono())
        log->info("Network (slant-iono carry, QUEUE 8b, scope={} source={} q={} "
                  "floor={} m² gap={} s): {} prior(s) injected ({} clamped at the "
                  "var floor), {} kept through freed slips, {} erased at un-freed "
                  "slip-funnel entries, {} erased by T4/consensus refloats, {} "
                  "evicted on gap, {} refused at datum re-realizations "
                  "(DECLARED-ZERO valve).{}",
                  config_.networkIonoCarryScope(), config_.networkIonoCarrySource(),
                  config_.networkIonoCarryQ(),
                  config_.networkIonoCarryVarFloor(), config_.networkIonoCarryMaxGapS(),
                  ctx.counters.n_iono_carry_injected,
                  ctx.counters.n_iono_carry_floor_hits,
                  ctx.counters.n_iono_carry_kept_through_slip,
                  ctx.counters.n_iono_carry_erased_slip,
                  ctx.counters.n_iono_carry_erased_t4,
                  ctx.counters.n_iono_carry_evicted,
                  ctx.counters.n_iono_carry_refused_datum,
                  config_.networkIonoCarrySource() == "smoothed_gf"
                      ? fmt::format("  CS-GF authority: {} blend step(s), {} "
                                    "reset(s) (gap/slip-gate/first-sighting), "
                                    "{} LOS state(s) live at run end.",
                                    ctx.counters.n_iono_cs_blends,
                                    ctx.counters.n_iono_cs_resets,
                                    ctx.carry.iono_cs.size())
                      : std::string{});
    // Withhold-era release policy (SPEC_withhold_era_release §3): both-edge era
    // telemetry per invariant #7.  An armed run whose eras never enter/exit or
    // whose escalation counters stay 0 through a known sick window is a MISWIRE
    // (invariant #4), not a quiet day.  landed + exhausted must sum to escalated.
    if (config_.networkRaimWithholdAdaptEscalate() > 0 ||
        config_.networkRaimWithholdFreeErase())
        log->info("Network (withhold-era release, escalate={} free_erase={}): {} era "
                  "entr{}, {} exit(s), longest {} withheld epoch(s); {} escalated "
                  "epoch(s) = {} landed (emitted through the accepted path) + {} "
                  "exhausted; {} detector-freed carry entr{} erased at the withhold "
                  "branch; {} era(s) ended by the RTA-10 carve-out publishing "
                  "(DECLARED limitation — no escalation ran there).",
                  config_.networkRaimWithholdAdaptEscalate(),
                  config_.networkRaimWithholdFreeErase(),
                  ctx.counters.n_wh_era_entries,
                  ctx.counters.n_wh_era_entries == 1 ? "y" : "ies",
                  ctx.counters.n_wh_era_exits,
                  ctx.counters.n_wh_era_max_len,
                  ctx.counters.n_wh_escalated_epochs,
                  ctx.counters.n_wh_escalate_landed,
                  ctx.counters.n_wh_escalate_exhausted,
                  ctx.counters.n_wh_free_erases,
                  ctx.counters.n_wh_free_erases == 1 ? "y" : "ies",
                  ctx.counters.n_wh_perconst_exits);
    // QUEUE item 4 R2/R4 (SPEC_t3_null_derivation §6): run-end declaration telemetry.
    // An armed R2 run with n_staf_epochs = 0 is a MISWIRE (invariant #4) — the seam
    // never produced a factor (raim off? rowRedundancy throwing fleet-wide?).
    if (config_.networkRaimStationFactor())
        log->info("Network (R2 station factor): {} epoch(s) computed over {} station(s); "
                  "{} depart(s) / {} return(s) at ratio {:.1f} (run-max ratio {:.2f}); "
                  "EWMA τ {:.0f} s, trim at k·σ̂₀.  DECLARED only — no denominator, no "
                  "gate, no response consumed this statistic.",
                  ctx.counters.n_staf_epochs, ctx.carry.staf_ewma.size(),
                  ctx.counters.n_staf_departs, ctx.counters.n_staf_returns,
                  config_.networkRaimStationFactorRatio(),
                  ctx.counters.staf_max_ratio,
                  config_.networkRaimStationFactorTauS());
    if (config_.networkRaimRateHygiene())
        log->info("Network (R4 rate hygiene): {} structurally-untestable T3 cell(s) "
                  "(s_raw = 0) excluded from the opportunity count; T3c opportunities "
                  "counted UNCONDITIONALLY (satellites with cells at >= {} stations).",
                  ctx.counters.n_t3_untestable_cells,
                  config_.networkRaimMinStations());
    // LV-6 census run-end readout (invariant #4: an armed run with 0 census
    // epochs past warmup is a MISWIRE, not a quiet day).
    if (config_.networkLevelAuthorityCsgf()) {
        const auto& c = ctx.counters;
        if (c.n_lvlauth_epochs > 0)
            log->info("Network (LV-6 level-authority census): {} FLEET epoch(s); "
                      "L_cs mean {:.3f} / min {:.3f} / max {:.3f} TECU; {} immature-LOS "
                      "skip(s) (min_n {}).  MEASURE-ONLY — nothing folded, nothing "
                      "gated on this statistic.",
                      c.n_lvlauth_epochs, c.lvlauth_sum / c.n_lvlauth_epochs,
                      c.lvlauth_min, c.lvlauth_max, c.n_lvlauth_immature,
                      config_.networkLevelAuthorityMinN());
        else
            log->warn("Network (LV-6 level-authority census): ARMED but produced ZERO "
                      "fleet epochs — past the min_n warmup this is a MISWIRE "
                      "(invariant #4), not a quiet day.");
    }
    // §9.7.10 datum-station slip detect: run-end ON-path telemetry (invariant #4 —
    // an armed run with injected datum-station slips reporting 0/0 is a MISWIRE,
    // not a quiet day).  Counted separately from Card E: those are non-pivot
    // detections answered by a local refloat; these are the gauge-pinned datum
    // station, answered by a network re-reference.
    if (config_.networkCycleSlipDatumStationDetect()) {
        const auto& qcc = slip_qc.counters;
        log->info("Network (datum-station slip detect): {} MW/LLI detection(s) at the "
                  "datum station, {} network re-reference(s) APPLIED (phase-bias carry "
                  "re-seed + all-station ambiguity reseed + per-sat disc mark).  A "
                  "detection whose re-reference never applies is a stuck queue — check "
                  "withhold runs.", qcc.datum_slip_detections, qcc.datum_slip_rerefs);
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
    // ---- SIM-ONLY ORACLE slip response (include/slip/NetworkSlipOracle.hpp) ----
    // Standing invariant #4: the ONLY way to tell a working oracle from dead
    // wiring is the applied count.  The pre-registered P1 for this card is
    // applied > 1500 on a 6 h campaign arm; a run that reports 0 has not measured
    // a ceiling, it has measured nothing, and the line says so in-band.
    if (ctx.carry.slip_oracle) {
        const auto& o = *ctx.carry.slip_oracle;
        const bool ts_free = (o.true_slip == NetworkSlipOracle::TrueSlip::Free);
        const bool fa_free = (o.false_alarm == NetworkSlipOracle::FalseAlarm::Free);
        // The ACTUATION counter depends on the policy pair: with true_slip 'free'
        // nothing is ever repaired BY CONSTRUCTION, so keying inertness on
        // `applied` there would fire the dead-wiring warning on a correctly
        // working false-alarm-isolation arm — the mirror of the failure that
        // warning exists to catch.  The arm's own actuation is the KEEP.
        const long long acted = ts_free ? o.fa_kept : o.applied;
        log->warn("Network (slip ORACLE): {} detector actuation(s) on carried arcs, {} "
                  "repaired (sum |delta| {:.1f} cyc, max |delta| {:.1f}, {} multi-slip), {} "
                  "true-slip freed, {} false-alarm kept [MW {} | LLI {} | IF-TD {} | "
                  "whole-sat {} | other {}], {} false-alarm freed, {} fell through with no "
                  "carry, {} with no wavelength; noise sigma {:.4f} cyc; POLICY true_slip "
                  "{} / false_alarm {}; detector-latency window {:.0f} s (floored at each "
                  "carry's BIRTH second, so a level erased and re-created by T4 is never "
                  "re-shifted) — lower bound set by the window on {}, by the carry's birth "
                  "on {}, by an earlier repair on {}; truth root '{}'.  SIM-ONLY DIAGNOSTIC "
                  "— this run's products carry a truth-fed response and are NOT a shippable "
                  "posture.{}",
                  o.seen, o.applied, o.sum_abs_cyc, o.max_abs_cyc, o.multi,
                  o.true_slip_freed, o.fa_kept, o.fa_kept_mw, o.fa_kept_lli,
                  o.fa_kept_iftd, o.fa_kept_wusat, o.fa_kept_other,
                  o.fa_freed, o.no_carry, o.no_lambda, o.noise_cyc,
                  ts_free ? "free" : "repair", fa_free ? "free" : "keep",
                  o.window_s, o.bound_window, o.bound_birth, o.bound_applied, o.truth_root,
                  (acted != 0)
                      ? ""
                  : (ts_free && fa_free)
                      ? "  ⚠ NO-OP ARM BY CONFIGURATION: both policies are 'free', so the "
                        "oracle declined every actuation and wrote nothing.  This is the "
                        "byte-identity control, not a measurement."
                  : ts_free
                      ? "  ⚠ ARMED AND INERT: ZERO false alarms were kept — on this policy "
                        "pair (true_slip 'free') the KEEP is the arm's only actuation, so "
                        "this is dead wiring, not a quiet run.  Do not read its ZWD as a "
                        "falsification of the false-alarm channel."
                      : "  ⚠ ARMED AND INERT: ZERO arcs were repaired — this is dead "
                        "wiring, not a quiet run, and the arm measures NOTHING.  Do not "
                        "read its ZWD as a falsification of level preservation.");
        // Closure (the gap_reentry precedent): every offered arc lands in exactly
        // one bucket, so a reader never has to infer a missing cell.
        const long long closure = o.applied + o.true_slip_freed + o.fa_kept + o.fa_freed
                                + o.no_carry + o.no_lambda;
        if (closure != o.seen)
            log->error("Network (slip ORACLE): counter closure BROKEN — {} seen vs {} bucketed "
                       "(applied {} + true_slip_freed {} + fa_kept {} + fa_freed {} + "
                       "no_carry {} + no_lambda {}).",
                       o.seen, closure, o.applied, o.true_slip_freed, o.fa_kept, o.fa_freed,
                       o.no_carry, o.no_lambda);
        // …and the tier split's own closure: an `other` bucket that is nonzero
        // means a call site passed a cause bit this split does not name, i.e. a
        // detector whose false alarms are attributed to nothing.
        const long long tier_sum = o.fa_kept_mw + o.fa_kept_lli + o.fa_kept_iftd
                                 + o.fa_kept_wusat + o.fa_kept_other;
        if (tier_sum != o.fa_kept || o.fa_kept_other != 0)
            log->error("Network (slip ORACLE): false-alarm TIER split BROKEN — {} kept vs {} "
                       "attributed (MW {} + LLI {} + IF-TD {} + whole-sat {} + other {}); a "
                       "nonzero 'other' is a call site passing an unrecognised cause bit.",
                       o.fa_kept, tier_sum, o.fa_kept_mw, o.fa_kept_lli, o.fa_kept_iftd,
                       o.fa_kept_wusat, o.fa_kept_other);
    }
    if (nc.slip_qc.gap_instrument || nc.slip_qc.gap_reentry) {
        const auto& qcc = slip_qc.counters;
        // ---- the RESPONSE clause (network.cycle_slip.gap_reentry_relevel) ----
        // Appended to the Card-G line rather than logged apart, because the G3
        // rule is read off the SHIFTED→bridged number in the same sentence: with
        // the response armed that number is the residual, and the reader needs
        // to see immediately whether it is small because nothing re-entered or
        // small because the response acted.  Invariant #4: an armed response
        // whose freed counts are 0 on a run with resumptions is MISWIRED, not
        // quiet, and the clause says so in-band.
        const std::string response_clause =
            !nc.slip_qc.gap_reentry
                ? std::string("  RESPONSE: gap_reentry_relevel OFF (arcs re-enter with "
                              "whatever carry state they had; the SHIFTED→bridged count "
                              "above is the unmitigated poison population).")
                : fmt::format(
                      "  RESPONSE: gap_reentry_relevel ON — {} re-entry(ies) judged SLIPPED "
                      "(gates: shifted | LLI-on-return {} | unmeasurable {}), {} skipped at a "
                      "gauge-pinned station; {} arc(s) reached with a PROTECTABLE carry "
                      "(a leveling entry still inside phase_leveling_max_gap_s, or the "
                      "carry_invariant vehicle) — {} had NO carry to protect, so the inject "
                      "would not have bridged them either, and {} of the {} reached had no "
                      "ambiguity STATE yet (expected: the column is pruned with the gap and "
                      "PASS B's pre-pass re-creates it after this drain, so the response "
                      "frees on the synthesised key) = {} shifted + {} lli + {} unmeasurable "
                      "arc(s) FREED, {} cold-skipped (carry not warm — nothing stale to "
                      "refloat), {} NOT ACTED because cycle_slip.relevel_free_epoch is off "
                      "(the pending-reseed mark alone is consumed by the PASS-B pre-pass and "
                      "never delivers the free epoch), {} answered by the C3c funnel ALONE "
                      "(carry_invariant: the leveling carry is structurally empty there, so "
                      "nothing is missing); {} ambiguity(ies) fed that funnel.{}",
                      qcc.gap_reentry_verdicts,
                      nc.slip_qc.gap_reentry_lli ? "ON" : "OFF",
                      nc.slip_qc.gap_reentry_unmeasurable ? "ON" : "OFF",
                      qcc.gap_reentry_pivot_skipped, qcc.gap_reentry_arcs,
                      qcc.gap_reentry_no_carry,
                      qcc.gap_reentry_state_absent, qcc.gap_reentry_arcs,
                      qcc.gap_reentry_freed_shifted, qcc.gap_reentry_freed_lli,
                      qcc.gap_reentry_freed_unmeas, qcc.gap_reentry_cold_skipped,
                      qcc.gap_reentry_no_relevel, qcc.gap_reentry_dd_only,
                      qcc.gap_reentry_dd_marked,
                      (qcc.gap_resumptions > 0 && qcc.gap_reentry_arcs == 0)
                          ? "  ⚠ ARMED AND INERT: resumptions exist and ZERO arcs were "
                            "reached — this is dead wiring, not a quiet run."
                          : "");
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
                  "such arcs slipped at re-entry.]{}",
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
                  qcc.d5_retap_ambs, qcc.d5_retap_ambs_stepped,
                  response_clause);
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
            rg.suppressed_warmup      += c.suppressed_warmup;
        }
        log->info("Network (slip-QC card R / IF-TD reference gate): {} legacy candidate(s) "
                  "(|median| > {:.2f}·λ_IF with n≥2) → {} fired ({} of them as an incoherent "
                  "multi-satellite STATION EVENT — reference arc reset, median NOT rebased).  "
                  "Suppressed: {} by the empirical scale / coherence test, {} thin-family "
                  "deferral(s) (< {} DDs), {} by the consecutive-fire cap, {} while the "
                  "empirical scale was still COLD (§6.14.6 item 1 — 0 unless "
                  "if_phase_td_ref_warmup_abstain is armed, in which case it is the "
                  "cold-start burst that no longer reaches the legacy floor); {} forced "
                  "reference re-selection(s).  [candidates − fires is exactly what the "
                  "hardening removed.]",
                  rg.legacy_candidates, nc.slip_qc.ref_floor_frac, rg.fires,
                  rg.fires_station_event, rg.suppressed_scale, rg.suppressed_thin,
                  nc.slip_qc.ref_min_n, rg.suppressed_persistence, rg.suppressed_warmup,
                  rg.reselects);
    }
    if (config_.networkPhaseDatumEq34())
        log->info("Network (Card 3/eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} coupling s_prev reference(s) re-based with them (§14 — on an "
                  "eq34+coupling lineage 0 here with nonzero shifts is the "
                  "wiring-inert alarm), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_cpl_sprev_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    // Chain-pin AUDIT (docs/SPEC_synthetic_minimum_network.md §14 T1 (measured record §12.8)): declared → folded →
    // landed per (constellation,signal).  The election's per-epoch cross-check
    // only proves the election RECORDED Σ (n_c + m_c) pins; foldAmb/foldRpb
    // early-return silently (no state / no live phase row) and a station drop
    // takes its folded pins with it — so "landed" is the number that actually
    // constrained the gauge.  A short group means the chain datum ran
    // under-constrained and the W5 near-null it was meant to remove is still
    // open (the W6+SH n-invariant L2W phase-bias common mode is the motivating
    // case).  Standing invariant #4: report the number, not the absence of a
    // complaint.
    if (config_.networkPhaseDatumEq34()) {
        const auto& pa = ctx.counters.eq34_pin_audit;
        if (pa.epochs == 0) {
            log->warn("Network (eq34 pin audit): ARMED but ZERO epochs audited — "
                      "every epoch aborted before the serial consume finished, or "
                      "the election never ran.  Nothing about the chain datum is "
                      "certified by this run.");
        }
        bool any_short = false;
        for (const auto& [grp, decl] : pa.declared) {
            auto at = [&](const std::map<std::string, long long>& m) -> long long {
                const auto it = m.find(grp); return it == m.end() ? 0 : it->second;
            };
            const int  n_short = [&]{ const auto it = pa.short_epochs.find(grp);
                                      return it == pa.short_epochs.end() ? 0 : it->second; }();
            const int  worst   = [&]{ const auto it = pa.worst_short.find(grp);
                                      return it == pa.worst_short.end() ? 0 : it->second; }();
            const long long bridged = at(pa.bridged);
            const long long landed  = at(pa.landed);
            if (n_short > 0) {
                any_short = true;
                log->warn("Network (eq34 pin audit) {}: SHORT in {} of {} epoch(s) "
                          "(worst −{} pins) — declared {} (−{} bridged), folded {}, "
                          "landed {}; fold refusals: {} no-state, {} no-row; {} "
                          "folded pin(s) lost to dropped stations.  The chain datum "
                          "ran UNDER-CONSTRAINED in those epochs.",
                          grp, n_short, pa.epochs, worst, decl, bridged,
                          at(pa.folded), landed, at(pa.miss_state), at(pa.miss_row),
                          at(pa.dropped_station));
            } else {
                log->info("Network (eq34 pin audit) {}: COMPLETE all {} epoch(s) — "
                          "declared {} (−{} bridged), folded {}, landed {} "
                          "(fold refusals: {} no-state, {} no-row; {} lost to "
                          "dropped stations).",
                          grp, pa.epochs, decl, bridged, at(pa.folded), landed,
                          at(pa.miss_state), at(pa.miss_row),
                          at(pa.dropped_station));
            }
        }
        if (pa.epochs > 0 && !any_short)
            log->info("Network (eq34 pin audit): every (constellation,signal) "
                      "group landed its full declared gauge dimension every "
                      "audited epoch — the chain pins are NOT the open near-null.");
    }
    // SEED-ROUND run tallies (docs/SPEC_eq34_seed_round.md §5).  The health
    // assertion is |pin innovation| ≤ λ/2 ALWAYS — by construction of
    // std::round at both computation sites, so a violation is a logic error
    // (stale λ map entry, mismatched state), never a data property.
    if (config_.networkPhaseDatumEq34SeedRound()) {
        const auto& c = ctx.counters;
        const long long sr_total = c.seed_round_pins + c.seed_round_deferred;
        if (sr_total == 0) {
            log->warn("Network (eq34 seed-round): ARMED but ZERO pins were "
                      "seed-rounded — every elected pin took the carry-round or "
                      "legacy-0 path.  Standing invariant #4: that is INERT, not "
                      "validated.  Expected on a warm resume whose carries all "
                      "survived; on a COLD start it means the knob never engaged "
                      "— check the election wiring.");
        } else {
            log->info("Network (eq34 seed-round): {} pin target(s) seed-rounded "
                      "({} at the election from a live state, {} fold-deferred "
                      "for states born in PASS B, {} FROZEN reuses of a "
                      "previously realized target — INC-1), max |pin innovation| "
                      "{:.4f} m, {} same-station target step(s) (each routed "
                      "through the reref hygiene, never the v2-shift walk).",
                      sr_total, c.seed_round_pins, c.seed_round_deferred,
                      c.seed_round_frozen,
                      c.seed_round_max_abs_y_m, c.seed_round_target_steps);
            if (c.seed_round_max_abs_y_m > 0.13)
                log->warn("Network (eq34 seed-round): max |pin innovation| "
                          "{:.4f} m EXCEEDS λ/2 of every GNSS phase band "
                          "(≤ 0.1274 m) — that is a LOGIC ERROR in the seed-round "
                          "target computation, not a data property.  Do not "
                          "trust this run's datum realization.",
                          c.seed_round_max_abs_y_m);
        }
        // INC-2 (§8.5) — the re-host ledger.  Reported only when armed, so an OFF
        // twin's run-end block stays byte-identical to the pre-card build.
        if (config_.networkPhaseDatumEq34SeedRoundRehostFrame()) {
            const long long rh = c.seed_round_rehost_framed
                               + c.seed_round_rehost_cold;
            if (rh == 0)
                log->warn("Network (eq34 seed-round INC-2): ARMED but NO tree pin "
                          "ever re-hosted without a carry — the frame-continuous "
                          "re-host path never ran.  Standing invariant #4: INERT, "
                          "not validated.");
            else
                log->info("Network (eq34 seed-round INC-2): {} carry-less re-host(s) "
                          "— {} FRAMED (step taken from the new host's own state, "
                          "carries S-transformed, the (sat,signal) kept out of "
                          "eq34_reref) and {} COLD (no allocated state ⇒ legacy "
                          "unknown step + reref).  Max |frame step| {:.4f} m: that "
                          "is bounded by λ/2 only while the new host's float sits on "
                          "the realized frame, so read a large value as evidence the "
                          "solution had drifted off it, NOT as a health violation.",
                          rh, c.seed_round_rehost_framed,
                          c.seed_round_rehost_cold,
                          c.seed_round_rehost_max_step_m);
        }
    }
    if (config_.networkPhaseDatumEq34SlipAwareElection()) {
        const long long ds = ctx.counters.eq34_slip_deseeded;
        const long long ra = ctx.counters.eq34_slip_readmitted;
        log->info("Network (Card 3/4 slip-aware election): {} slipped incumbent(s) DE-SEEDED "
                  "from the sticky tree, {} arcLife demotion(s) (sort comparisons, inert "
                  "without the longevity election), and {} slipped arc(s) became a tree edge "
                  "ANYWAY because the component had no other route to the vertex.  A slipped "
                  "TREE edge refloats every fundamental cycle through it; a slipped CHORD "
                  "costs one DD — so (de-seeded − re-admitted) is the mass-refloat this "
                  "converted to a rank-1 kill, and the re-admitted count is the share the "
                  "graph topology, not the policy, still forces.",
                  ds, ctx.counters.eq34_slip_demoted, ra);
        if (ds == 0)
            log->warn("Network (Card 3/4): the slip-aware election is ARMED but DE-SEEDED "
                      "NOTHING — no slipped arc was ever an incumbent tree edge this run.  "
                      "Standing invariant #4: that is INERT, not proof the policy works.  "
                      "Check that slips reached the funnel at all (the slip-QC lines above); "
                      "with few slips and a dense graph a zero here is genuinely expected.");
        else if (ra >= ds)
            log->warn("Network (Card 3/4): the slip-aware election de-seeded {} arc(s) but {} "
                      "slipped arc(s) still ended up in the tree — the graph, not the "
                      "election, is the binding constraint here and this knob is buying "
                      "little.  A denser observation graph (or a per-component redundancy "
                      "check) is the lever, not this policy.", ds, ra);
    }
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
    // FC-1 (network.fast_clock): the split-cadence fast tier runs on epochs the full
    // pipeline SKIPS, so it leaves no trace in any of the per-epoch readouts above.  These
    // counters ARE its ON-path telemetry (standing invariant #4) — a clean log proves
    // nothing about whether a single tick ever fired.
    if (config_.getNetworkConfig().fast_clock.enable) {
        const auto& fcc = config_.getNetworkConfig().fast_clock;
        const auto& c = ctx.counters;
        const double ms_per_tick =
            c.fc_ticks_driven ? c.fc_wall_ms / static_cast<double>(c.fc_ticks_driven) : 0.0;
        log->info("Network (FC-1 split-cadence fast clock): {} tick(s) driven, {} estimated, "
                  "{} emitted; {} LOS used / {} screened ({} slip-scale); {} sat-below-floor, "
                  "{} datum-withheld tick(s), {} gap-withheld tick(s).",
                  c.fc_ticks_driven, c.fc_ticks_estimated, c.fc_ticks_emitted,
                  c.fc_los_used, c.fc_los_screened, c.fc_screen_slips,
                  c.fc_sat_below_floor, c.fc_datum_withheld_ticks, c.fc_gap_withheld_ticks);
        log->info("Network (FC-1)   tick wall: {:.1f} ms total, {:.3f} ms/tick mean, "
                  "{:.3f} ms max (§7.3 budget: <= 50 ms/tick at 305 stations — a MAX bound, "
                  "since an over-budget tick at an RT cadence is a MISSED epoch, not a "
                  "slower average).",
                  c.fc_wall_ms, ms_per_tick, c.fc_wall_ms_max);
        if (c.fc_ticks_driven == 0)
            log->warn("Network (FC-1): armed but ZERO ticks were driven — the T2e decimation "
                      "hook never fired.  With epoch_decimation_n > 1 that means no epoch was "
                      "decimated out (too few synced epochs); with shadow on it means no full "
                      "epoch was reached.  This is inert WIRING, not a quiet run: do NOT read "
                      "this run as evidence about the fast tier (invariant #4).");
        else if (c.fc_ticks_estimated == 0)
            log->warn("Network (FC-1): {} tick(s) ran but NONE produced a satellite clock "
                      "delta — the anchor never armed (it needs two consecutive EMITTED full "
                      "epochs) or every satellite missed the {}-station floor.  The tier is "
                      "wired but measured nothing.",
                      c.fc_ticks_driven, fcc.min_stations_per_sat);
        else if (fcc.emit && c.fc_ticks_emitted == 0)
            log->warn("Network (FC-1): emit is armed and {} tick(s) estimated, but NOTHING "
                      "reached the product wire — every tick was withheld by the datum guard "
                      "({}) or the R11 emission-gap bound of {:.0f} s ({}).  The consumer saw "
                      "no fast clocks at all.",
                      c.fc_ticks_estimated, c.fc_datum_withheld_ticks, fcc.emit_max_gap_s,
                      c.fc_gap_withheld_ticks);
        if (c.fc_los_used + c.fc_los_screened > 0) {
            const double screen_frac =
                static_cast<double>(c.fc_los_screened)
                / static_cast<double>(c.fc_los_used + c.fc_los_screened);
            if (screen_frac > 0.01)
                log->warn("Network (FC-1): screen rate {:.2f}% of LOS exceeds the 1% quiet-data "
                          "expectation (§6 INC-4) — either the window is genuinely eventful or "
                          "screen_k_sigma ({:.1f}) is mis-scaled against the prediction spread.",
                          100.0 * screen_frac, fcc.screen_k_sigma);
        }
        if (c.fc_wall_ms_max > 50.0)
            log->warn("Network (FC-1): slowest tick {:.1f} ms exceeds the §7.3 50 ms budget — "
                      "profile before threading; the WLS must stay serial-deterministic.",
                      c.fc_wall_ms_max);
    }
    // RTB-7(d) RE-4 / invariant #4: the resume event's wiring is only visible through
    // counters — "0 restored / no shock" must be stated, never inferred from absence.
    if (carry_invariant && !config_.getNetworkConfig().state_snapshot_path.empty()) {
        // RE-1 σ̂₀ SEED PROVENANCE.  The old text asserted a cause the code never checked —
        // it printed "(v1 blob / calibration off)" on the not-seeded branch unconditionally,
        // so a COLD run with calibration ARMED published two false clauses while its own
        // config line said CALIBRATION ON (`runs/pdfix_qz0_deg15/run.log`).  Four distinct
        // conditions suppress the seed; the driver knows which one held, so say THAT
        // (invariant #4, and the same instrument fix as SPEC_pd_safe_dd_fold.md §11).
        using Seed = NetworkInvariantCarryDriver::ResumeSeedState;
        const auto seed_st = carry_driver.resumeSigma0Status();
        std::string sigma0_txt;
        switch (seed_st.state) {
            case Seed::Seeded:
                sigma0_txt = fmt::format("SEEDED {:.4f} from the blob's 'S,' line ({:.0f} "
                                         "row(s)) — the D5 gate trusts the restored "
                                         "inflation immediately",
                                         seed_st.sigma0, seed_st.blob_rows);
                break;
            case Seed::NotRestored:
                sigma0_txt = "NOT seeded — COLD: no carry blob was restored this run, so "
                             "there was no σ̂₀ to seed from; the gate runs raw-formal until "
                             "its warm-up count is met";
                break;
            case Seed::CalibrationOff:
                sigma0_txt = "NOT seeded — network.carry_invariant_innov_calibrate is OFF, "
                             "so no σ̂₀² accumulator exists and any 'S,' line in the blob "
                             "was never consulted; the gate is raw-formal BY CONFIG, not "
                             "pending a warm-up count";
                break;
            case Seed::NoSeedLine:
                sigma0_txt = "NOT seeded — the restored blob carried no 'S,' line (a v1/"
                             "legacy blob, or one written by a calibration-off run); the "
                             "gate runs raw-formal until its warm-up count is met";
                break;
            case Seed::Rejected:
                sigma0_txt = fmt::format(
                    "NOT seeded — the blob's 'S,' line was REJECTED (σ̂₀² = {:.4g}, rows = "
                    "{:.0f}; the seed guard needs both strictly positive).  That is a "
                    "WRITER-side defect, not a legacy blob — the run that wrote this "
                    "snapshot serialized a degenerate accumulator; the gate runs raw-formal "
                    "until its warm-up count is met",
                    seed_st.blob_sigma0_sq, seed_st.blob_rows);
                break;
        }
        log->info("Network (RTB-7d resume event): carry restored {}{}; sigma0 [{}] {}; resume "
                  "shock {}; restored Pᶻ {}.",
                  carry_driver.resumeCarryRestored() ? "YES" : "NO",
                  carry_driver.resumeCarryRestored()
                      ? fmt::format(" ({} carried DD at restore)",
                                    carry_driver.resumeCarryChords())
                      : std::string{},
                  NetworkInvariantCarryDriver::resumeSeedStateName(seed_st.state), sigma0_txt,
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
    // §18.12 coupling CAPTURE, write side — the RTB-7(e) snapshot readout's sibling, but
    // at FUNCTION scope on purpose: the RTB-7(d/e) block above is gated on carry_invariant,
    // while the coupling capture rides the phase-leveling carries
    // (carry_conditional_coupling, which supersedes carry_invariant — the target lineage
    // runs carry_invariant: false), so nesting it there would make the ON-path telemetry
    // structurally INERT on exactly the postures that arm it (standing invariant #4).
    // ~25 B per (id, coeff) pair is the §2.7 size budget's own figure; it is what turns
    // "rows captured" into the disk/IO fact a keep-history leg has to plan for.
    if (config_.getNetworkConfig().state_snapshot_coupling_capture)
        log->info("Network (§18.12 coupling capture): last snapshot carried {} L row(s) "
                  "+ {} Z row(s), {} regressor term(s) over {} shared column(s) "
                  "(~{:.2f} MB of QL/QZ payload); {} row(s) omitted whole for a "
                  "non-finite field over the run.{}",
                  ctx.carry.snap_cpl_rows_last, ctx.carry.snap_cpl_zrows_last,
                  ctx.carry.snap_cpl_terms_last, ctx.carry.snap_cpl_dict_last,
                  static_cast<double>(ctx.carry.snap_cpl_terms_last) * 25.0
                      / (1024.0 * 1024.0),
                  ctx.carry.snap_cpl_rows_dropped,
                  ctx.carry.snap_cpl_rows_last == 0
                      ? "  ⚠ 0 rows under an ARMED capture — carry_conditional_coupling "
                        "off, or every carry harvested EMPTY: the file is v1/v2 and no "
                        "resume can use it."
                      : "");
    if (sh_dev_on) {
        log->info("Network (Iono-2/T5v2 per-LOS deviation carry): {} warm harvest(s), "
                  "{} cold seed(s), {} slip reset(s) (C3c funnel), {} no-information "
                  "harvest skip(s), {} deviation(s) carried at run end (τ={} s, "
                  "σ_ss={} TECU vertical, exact posterior harvest).",
                  ctx.carry.shdev_warm_total, ctx.carry.shdev_cold_total,
                  ctx.carry.shdev_slip_resets_total, ctx.carry.shdev_nodata_total,
                  sh_dev_carry.size(), shc.rep_error_tau_s,
                  shc.representation_sigma_tecu);
        // v3k clause only when armed — the v2/v3 OFF lines stay byte-identical.
        if (shc.rep_error_d5_keep)
            log->info("Network (Iono-2/v3k D5-keep): {} D5-only re-tap(s) KEPT the level "
                      "(variance re-tapped to σ_ss), {} raw-confirmed erase(s) — the erase "
                      "policy split (SPEC_iono2_mechB_v3 addendum).",
                      ctx.carry.shdev_d5_keeps_total, ctx.carry.shdev_slip_resets_total);
        // v3 clause only when armed — the v2 OFF line stays byte-identical.
        if (shc.rep_error_demean)
            log->info("Network (Iono-2/v3 de-mean): global common-mode removed on {} epoch(s) "
                      "(mean |g| = {:.3f} TECU), per-station means removed {} station-epoch(s) "
                      "(anti-ratchet; the a00-twin / receiver-lump quasi-null directions stay "
                      "with the field and receiver states — SPEC_iono2_mechB_v3).",
                      ctx.carry.shdev_demean_epochs_total,
                      ctx.carry.shdev_demean_epochs_total > 0
                          ? ctx.carry.shdev_demean_abs_g_sum
                                / static_cast<double>(ctx.carry.shdev_demean_epochs_total)
                          : 0.0,
                      ctx.carry.shdev_demean_sta_total);
    }
    // rx-CB carry (SPEC_rx_cb_carry): armed-only run-end line, independent of iono2.
    if (config_.networkCarryRxCodeBias())
        log->info("Network (rx-CB carry): {} receiver code-bias prior(s) folded, {} withheld "
                  "by the 10σ innovation gate (receiver-reset response), {} harvest(s); {} "
                  "carried at run end (RW q={} m²/s; harvested outside the phase-leveling "
                  "gate — SPEC_rx_cb_carry).",
                  ctx.carry.rx_cb_injected_total, ctx.carry.rx_cb_innov_skips_total,
                  ctx.carry.rx_cb_harvested_total, ctx.carry.rx_cb_carry.size(),
                  config_.networkRxCbCarryQ());
    // Card LV-2 (docs/SPEC_level_information_budget.md): armed-only run-end line.
    // Standing invariant #4 — "0 demeaned" must be visible, never silent, because a
    // de-mean that never fired is indistinguishable from a healthy one in every
    // other trace.  The skip rate is printed against its own denominator: the whole
    // point of judging |innov − μ| is that a coherent frame move stops
    // mass-triggering the receiver-reset response.
    if (config_.networkRxCbCarryDemean()) {
        const long long lv2_seen = ctx.carry.rx_cb_injected_total
                                 + ctx.carry.rx_cb_innov_skips_total;
        const double skip_pct = lv2_seen > 0
            ? 100.0 * static_cast<double>(ctx.carry.rx_cb_innov_skips_total)
                    / static_cast<double>(lv2_seen)
            : 0.0;
        if (ctx.carry.lv2_mu_epochs == 0)
            log->warn("Network (LV-2 rx-CB de-mean): ARMED but NO epoch ever produced a μ "
                      "— every epoch fell below the {}-station floor (or no carry row ever "
                      "folded), so the carry ran with its FULL common-mode force all run "
                      "(standing invariant #4).  {} μ-freeze event(s).",
                      config_.networkRxCbCarryDemeanMinStations(),
                      ctx.carry.lv2_mu_frozen);
        else
            log->info("Network (LV-2 rx-CB de-mean): μ computed on {} epoch(s), FROZEN {} time(s) "
                      "by the {}-station thin-mean floor (invariant #7 decoupling edge); last μ "
                      "{:+.4f} m, max |μ| {:.4f} m, applied {}.  {}  Σλ(innov−μ) = 0 by "
                      "construction (in DELTA coordinates): the carry transported "
                      "between-station differences at full stiffness and stopped re-asserting "
                      "last epoch's common-mode realization.  It did NOT free that direction — "
                      "in absolute terms the de-meaned targets pin the receiver-DCB common "
                      "mode to mean(x_lin), the frozen first-epoch seed frame (SPEC §13).",
                      ctx.carry.lv2_mu_epochs, ctx.carry.lv2_mu_frozen,
                      config_.networkRxCbCarryDemeanMinStations(),
                      ctx.carry.lv2_last_mu_m, ctx.carry.lv2_max_abs_mu_m,
                      config_.networkRxCbCarryDemeanWithin()
                          ? "WITHIN the epoch (LV-5: no lag)"
                          : "LAG-1 (next epoch's folds)",
                      // The skip figure is only a statement about anything when the
                      // innovation test is still live: under LV-4 it is switched off by
                      // construction, so printing "0.0 %" as if it were an outcome would
                      // read as a result the run did not produce.
                      config_.networkRxCbCarryStepGate()
                          ? std::string("The prefit innovation test is RETIRED on this run "
                                        "(LV-4) — see the invariant-gate line for the erase "
                                        "count and the same-run A/B.")
                          : fmt::format("Inject gate skipped {}/{} rows ({:.1f} %) on the "
                                        "DEMEANED innovation.",
                                        ctx.carry.rx_cb_innov_skips_total, lv2_seen, skip_pct));
    }
    // Card LV-5 (docs/SPEC_level_information_budget.md §12): armed-only run-end line.
    // The load-bearing number is the RESIDUAL — the folded set's own common mode
    // minus the μ that was applied to it.  Near zero means the within-epoch
    // correction actually nulled the force on the frame direction; a residual of the
    // same size as μ means it did not, and the de-mean is decorative.
    if (config_.networkRxCbCarryDemeanWithin()) {
        const double mean_resid = ctx.carry.lv5_resid_n > 0
            ? ctx.carry.lv5_resid_sum / static_cast<double>(ctx.carry.lv5_resid_n)
            : 0.0;
        if (ctx.carry.lv5_within_epochs == 0)
            log->warn("Network (LV-5 within-epoch μ): ARMED but NO epoch reduced a μ "
                      "before PASS B — every epoch fell below the thin-mean floor or the "
                      "carry map was empty, so the folds ran UN-demeaned (standing "
                      "invariant #4).");
        else
            log->info("Network (LV-5 within-epoch μ): μ reduced between PASS A and PASS B "
                      "on {} epoch(s) and folded against the SAME epoch — no lag.  Residual "
                      "common mode measured on the folded set afterwards: mean |resid| "
                      "{:.4f} m, max {:.4f} m over {} family-epoch(s), against max |μ| "
                      "{:.4f} m.  The residual is what the correction LEFT: near zero means "
                      "the de-mean tracked the folded set's own common mode this run (NOT "
                      "that the direction is free — in absolute terms the carry still pins it "
                      "to mean(x_lin), SPEC §13); residual ≈ μ would mean the de-mean was "
                      "decorative.  Removing the lag "
                      "is what makes full carry admission (LV-4) stable — the lag-1 loop "
                      "gain scales with the folded fraction and limit-cycled the level.",
                      ctx.carry.lv5_within_epochs, mean_resid,
                      ctx.carry.lv5_max_abs_resid_m, ctx.carry.lv5_resid_n,
                      ctx.carry.lv2_max_abs_mu_m);
    }
    // Card LV-4 (docs/SPEC_level_information_budget.md §11): armed-only run-end line.
    // The headline is the SAME-RUN A/B — how many folds the retired frame-dependent
    // screen would have withheld on exactly these rows, against how many genuine
    // invariant steps the new screen found.  A large first number with a near-zero
    // second one IS the finding: the old screen was measuring the seed, not the
    // receiver.
    if (config_.networkRxCbCarryStepGate()) {
        const long long lv4_folds = ctx.carry.rx_cb_injected_total;
        const double would_pct = lv4_folds > 0
            ? 100.0 * static_cast<double>(ctx.carry.lv4_frame_gate_would_skip)
                    / static_cast<double>(lv4_folds)
            : 0.0;
        if (ctx.carry.lv4_step_checks == 0)
            log->warn("Network (LV-4 invariant carry gate): ARMED but ZERO step checks ran "
                      "— no carried receiver code bias was ever re-harvested, so the event "
                      "screen never looked at anything (standing invariant #4).");
        else
            log->info("Network (LV-4 invariant carry gate): {} step check(s) at the harvest, "
                      "{} ERASE(s) beyond {}·σ̂ peer-relative (max z {:.2f}, max robust σ̂ "
                      "{:.3f} m), {} epoch-family stand-down(s) below the {}-entry floor; "
                      "{} prior(s) folded.  The RETIRED frame-dependent screen "
                      "(|value_abs − x_lin(cb)|) would have withheld {} of those folds "
                      "({:.1f} %) — measured on the same rows, in this run.  That gap is the "
                      "card: x_lin is a frozen linearization point, so the old statistic "
                      "withheld by where the first-epoch seed landed, and it could not see a "
                      "jump between two epochs at all.  The scale is ROBUST and PEER-RELATIVE "
                      "(1.4826·MAD of this epoch's steps, median removed — a common step is a "
                      "frame move, not N receiver resets); a formal-σ version of this test "
                      "erased 432/777 entries and destabilized the level.  Erase-not-overwrite "
                      "is the invariant-#1 response: the entry goes cold for one epoch and the "
                      "next harvest re-seeds it from data.",
                      ctx.carry.lv4_step_checks, ctx.carry.lv4_step_resets,
                      config_.networkRxCbCarryStepKSigma(), ctx.carry.lv4_max_step_z,
                      ctx.carry.lv4_max_scale_m, ctx.carry.lv4_scale_standdown,
                      config_.networkRxCbCarryDemeanMinStations(),
                      lv4_folds, ctx.carry.lv4_frame_gate_would_skip, would_pct);
    }
    // H-rider (edge-queue §1.6 item 3a): armed-only run-end line.  Invariant #4:
    // a census-fed wire whose pool was empty every epoch shipped 1440 sentinel
    // zeros while reading as "armed and healthy" — the counter is the tell.
    if (config_.networkHSigma0PrefitCensus()) {
        if (ctx.carry.h_sig0_census_nonzero == 0)
            log->warn("Network (H-rider census σ̂₀): ARMED but NO epoch ever produced a "
                      "usable pooled σ̂₀ — the H wire carried the 0.0000 'unavailable' "
                      "sentinel all run (empty pool: check the class mask '{}' against "
                      "the classified row population; standing invariant #4).",
                      config_.networkHSigma0Classes());
        else
            log->info("Network (H-rider census σ̂₀): pooled prefit σ̂₀ on the H wire for "
                      "{} epoch(s) (classes '{}'; hsig=pc advertised on the S contract; "
                      "last value {:.4f}).  The A4 withhold ceiling still read "
                      "sigma0_adapt, unchanged.",
                      ctx.carry.h_sig0_census_nonzero, config_.networkHSigma0Classes(),
                      ctx.carry.ep_h_sigma0_census);
    }
    // Card S2H (docs/SPEC_level_information_budget.md §17 item 3): armed-only
    // run-end line.  Standing invariant #4 — "0 epochs applied" must be visible:
    // a transform that never ran leaves the carry with its full common-mode
    // force, indistinguishable from a healthy run in every other trace.  The
    // headline is mean/max |μ| — the common-mode content removed per epoch — and
    // the thin-skip count (identity epochs, invariant #7's decoupling edge).
    if (config_.networkRxCbHarvestDemean()) {
        const double mean_mu = ctx.carry.s2h_mu_n > 0
            ? ctx.carry.s2h_mu_abs_sum / static_cast<double>(ctx.carry.s2h_mu_n)
            : 0.0;
        if (ctx.carry.s2h_epochs_applied == 0)
            log->warn("Network (S2H S-consistent harvest): ARMED but NO epoch ever "
                      "applied the transform — every family-epoch fell below the "
                      "{}-station vote floor (or the carry map stayed empty), so the "
                      "carried set kept its full common-mode content all run "
                      "(standing invariant #4).  {} thin-skip(s).",
                      config_.networkRxCbCarryDemeanMinStations(),
                      ctx.carry.s2h_thin_skips);
        else
            log->info("Network (S2H S-consistent harvest): transform applied on {} "
                      "epoch(s) over {} family-epoch reduction(s); {} thin-skip "
                      "identity epoch(s) below the {}-station vote floor; {} vote(s) "
                      "excluded by the z={} peer screen.  μ last {:+.4f} m, mean |μ| "
                      "{:.4f} m, max |μ| {:.4f} m — the common-mode content removed "
                      "from the carried set each epoch.  The carried set is zero-mean "
                      "by construction in the datum row's own absolute frame, so the "
                      "carry exerted zero net common-mode force and the level "
                      "authority was the datum + per-epoch data, with the frame "
                      "re-derived every epoch (no frozen pin, no lag-1 feedback — "
                      "SPEC §17 item 3; LASER big-system S-transform, Λ-weighted).",
                      ctx.carry.s2h_epochs_applied, ctx.carry.s2h_mu_n,
                      ctx.carry.s2h_thin_skips,
                      config_.networkRxCbCarryDemeanMinStations(),
                      ctx.carry.s2h_screened_total,
                      config_.networkRxCbHarvestScreenZ(),
                      ctx.carry.s2h_last_mu_m, mean_mu, ctx.carry.s2h_max_abs_mu_m);
    }
    // Card LV-1 (docs/SPEC_level_information_budget.md): armed-only run-end line.
    // The per-station counters live on each station's StateInitializer (the reframe
    // happens inside the PARALLEL produce phase), summed here serially — the same
    // pattern as the SD-EWL and iono_var feed counters.
    if (config_.networkIonoSeedFieldFrame()) {
        long long reframed = 0, fb_raw = 0, fb_field = 0, offsets = 0;
        for (const auto& st : stations) {
            if (!st.mm) continue;
            const auto& s = st.mm->getStateInitializer().ionoSeedFrameStats();
            reframed += s.reframed;
            fb_raw   += s.fallback_raw;
            fb_field += s.field_missing;
            offsets  += s.offsets;
        }
        if (reframed == 0)
            log->warn("Network (LV-1 iono seed frame): ARMED but ZERO seeds were reframed "
                      "({} raw fallback(s) for want of {} usable seeds, {} for want of a field "
                      "value, {} station-epoch offsets) — the wiring is INERT (standing "
                      "invariant #4).  Every per-LOS iono state was born in the receiver's "
                      "bias-in frame.",
                      fb_raw, config_.networkIonoSeedFieldFrameMinSats(), fb_field, offsets);
        else
            log->info("Network (LV-1 iono seed frame): {} per-LOS iono seed(s) born in the "
                      "FIELD's frame ({} station-epoch median offsets; {} raw fallback(s) below "
                      "the {}-seed floor, {} without a field value — degenerate IPP or a "
                      "constellation outside the SH field's GPS/Galileo scope).  The lag-1 field "
                      "was refreshed on {} epoch(s); seeds born before the first refresh used the "
                      "a₀₀ climatology prior mean.  NOTE the absolute network solve is invariant "
                      "to this linearization frame — grade it on the PREFIT consumers it moves "
                      "(the rx-CB inject gate's skip rate, the prefit census), not on a level "
                      "gradient.",
                      reframed, offsets, fb_raw,
                      config_.networkIonoSeedFieldFrameMinSats(), fb_field,
                      ctx.carry.lv1_field_snapshots);
    }
    // RXCB-DATUM: armed-only run-end line.  Standing invariant #4 — the ONLY way to
    // tell a live gauge from an armed-but-inert one is the fold count, so print it
    // even (especially) when it is zero.
    if (config_.networkRxCodeBiasDatum()) {
        if (ctx.counters.n_rxcb_gauges == 0)
            log->warn("Network (RXCB-DATUM): receiver code-bias datum is ARMED but folded ZERO "
                      "prior row(s) over the run — no station ever presented a receiver DCB "
                      "column, so the receiver-DCB common mode was NEVER gauged and this run is "
                      "NOT the posture it claims (standing invariant #4).  Check that "
                      "estimation.estimate_ifb is on and that the DCB states exist.");
        else
            log->info("Network (RXCB-DATUM): {} zero-mean receiver code-bias prior row(s) folded "
                      "over the run (one per station-epoch per constellation, sigma {:.1f} m, "
                      "realized common-mode information 1/sigma^2 = {:.3e} INDEPENDENT of station "
                      "count).  Folded PRE-reduce through the diagonal prior channel, so it "
                      "composes with the Card-4 DD priors that refuse a post-reduce Woodbury "
                      "gauge.  This is the IGS zero-mean-over-receivers DCB convention, replacing "
                      "the datum the satellite code-bias sum-to-zero silently also provided.",
                      ctx.counters.n_rxcb_gauges,
                      config_.networkRxCodeBiasDatumSigmaM(),
                      1.0 / (config_.networkRxCodeBiasDatumSigmaM()
                             * config_.networkRxCodeBiasDatumSigmaM()));
    }
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
    if (carry_innov_gate && config_.networkCarryInvariantInnovCorroborate()) {
        const long long sup = carry_driver.counters().innov_suppressed;
        const long long tripped = carry_innov_flags_total + sup;
        log->info("Network (Card 4/M1 D5 v3 corroboration gate): {} of {} gate trips SUPPRESSED "
                  "({:.1f}%) because the raw observables measured the arc as CONTINUOUS; {} "
                  "re-tapped (raw step, or no raw evidence — absence of evidence never "
                  "suppresses).  Backstop k = {}.",
                  sup, tripped,
                  tripped > 0 ? 100.0 * static_cast<double>(sup) / static_cast<double>(tripped) : 0.0,
                  carry_innov_flags_total,
                  config_.networkCarryInvariantInnovCorroborateBackstopK());
        if (sup == 0)
            log->warn("Network (Card 4/M1 D5 v3): the corroboration gate is ARMED but suppressed "
                      "NOTHING — the flat set never reached the driver.  Standing invariant #4: "
                      "check that a slip-QC DECISION CARD ran the QUALIFY pass this run "
                      "(network.cycle_slip.gap_instrument / .mw_lli_corroborate / "
                      ".relevel_bridge_gf / the CS-GF authority — SlipQcState::cardsArmed()); "
                      "without one the gate is INERT, not permissive.  ⚠ network.trace_slip_raw "
                      "is NOT one of them: it COMPUTES the observables but adjudicates none, so "
                      "every verdict map is empty and nothing is ever taggable.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly()) {
        const long long bs = carry_driver.counters().innov_born_skipped;
        log->info("Network (Card 4/M1 D5 v5 carried-only): {} chord-row(s) NOT gated because "
                  "the chord was BORN this epoch — no carried prior, so Cov(v) is the diffuse "
                  "fresh-chord variance and the standardized ratio is not a hypothesis test.  "
                  "Read against the {} flag(s) that DID fire: on the untreated 3 h reference "
                  "95.3% of all flags were born chords, and a chord re-borned every epoch can "
                  "never accumulate, which is what pins dd_sigma_cyc.  This is counted before "
                  "the threshold test, so it is the whole born population, not just the part "
                  "that would have flagged.",
                  bs, carry_innov_flags_total);
        if (bs == 0)
            log->warn("Network (Card 4/M1 D5 v5): carried-only is ARMED but skipped NOTHING — "
                      "no born chord ever reached the gate.  Standing invariant #4: INERT, not "
                      "benign.  Either the carry never births chords (check the invariant-carry "
                      "line's birth count) or the chord_carried mask is not being populated.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovRetapOnly()) {
        const long long sup = ctx.carry.carry_innov_kill_suppressed;
        log->info("Network (Card 4/M1 D5 v4 re-tap-only): {} carry-arc membership kill(s) "
                  "SUPPRESSED over the run — a D5 flag whose arc the raw observables MEASURED "
                  "as continuous no longer excludes that arc from A_old, so the in-epoch "
                  "re-tap the gate already installed survives instead of being refloated from "
                  "a diffuse prior.  A chord arc has cycle support 1 under a stable tree, so "
                  "no OTHER carried DD was protected.  Stepped / unmeasurable arcs, and any "
                  "arc a raw detector flagged in the same epoch, still killed their cycle.  "
                  "Read this against the {} slip refloat(s) the driver counted: the difference "
                  "is the churn this knob removed, and n_chord_births in "
                  "network_epoch_status.csv is where the chord-lifetime effect shows up.  "
                  "[Counted per arc per build call — a within-epoch RAIM re-solve counts its "
                  "arcs twice, the same convention as chords_born.]",
                  sup, carry_slipkills_total);
        if (sup == 0)
            log->warn("Network (Card 4/M1 D5 v4): re-tap-only is ARMED but suppressed NOTHING — "
                      "no corroborated-flat D5 flag ever reached a carry arc.  Standing "
                      "invariant #4: the knob is INERT this run, not benign.  Check, in order: "
                      "did the D5 gate flag at all (the innovation-gate line above); did a "
                      "slip-QC DECISION CARD run the QUALIFY pass (cardsArmed() — "
                      "gap_instrument / mw_lli_corroborate / relevel_bridge_gf / the CS-GF "
                      "authority; network.trace_slip_raw does NOT qualify, it computes without "
                      "adjudicating, so every flag reads 'no evidence' and none is taggable); "
                      "did the flags resolve through the "
                      "reverse map (carry_innov_map_miss); and is "
                      ".carry_invariant_innov_corroborate also armed, which suppresses the same "
                      "population one level earlier and leaves this correctly at zero.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovCalibrate()) {
        // σ̂₀ CLAMP VISIBILITY.  The old line published the CLAMPED σ̂₀ alone, and a clamped
        // 1.00× is indistinguishable from a calibrated 1.00× — which is how the whole
        // `churn_base2` / `pdfix_off` lineage ran its D5 gate as the raw formal (v1) test
        // for entire campaigns while this line said "calibration armed" (raw σ̂₀² was 0.0099,
        // clamped up to 1; SPEC_pd_safe_dd_fold.md §10 finding 2).  Report the STATE
        // HISTOGRAM, the RAW range, and warn when the calibration was inert for most of the
        // run — standing invariant #4: an armed knob at its neutral value is not benign.
        const auto& cc  = carry_driver.counters();
        const auto  st  = carry_driver.innovInflationStatus();
        const long long den = std::max<long long>(1, cc.d5_calib_epochs);
        const long long inert = cc.d5_calib_warmup_epochs + cc.d5_calib_floored_epochs;
        const double inert_pct = 100.0 * static_cast<double>(inert) / static_cast<double>(den);
        const std::string raw_range =
            (cc.d5_calib_raw_max > 0.0)
                ? fmt::format("{:.4g} … {:.4g}", cc.d5_calib_raw_min, cc.d5_calib_raw_max)
                : std::string{"(never estimated past warm-up)"};
        // NOTE the two σ̂₀² figures are deliberately different quantities: the FINAL one is
        // the post-run accumulator state (it includes the last install's own rows, which no
        // gate ever consulted), while the range is over the values the gate actually USED.
        // The final can therefore sit outside the range — that is correct, not a bug.
        log->info("Network (Card 4/M1 D5 v2 LS-VCE calibration): post-run empirical inflation "
                  "σ̂₀ = {:.2f}× in state '{}' (raw σ̂₀² = {:.4g}, n_eff = {:.0f}; gate threshold "
                  "k·σ̂₀ = {:.1f} formal-σ).  Over {} install(s) the σ̂₀ state was: active {} / "
                  "floored {} / warm-up {} / capped {}.  RAW σ̂₀² the gate CONSULTED past "
                  "warm-up: {} (the post-run value above is one accumulator step later).  "
                  "σ̂₀>1 quantifies how optimistic the formal Cov(v) is — the M2 monitor's "
                  "finding, closing the loop on the gate itself.",
                  st.sigma0, NetworkInvariantCarryDriver::inflationStateName(st.state),
                  st.raw_sigma0_sq, st.n_eff,
                  config_.networkCarryInvariantInnovK() * st.sigma0,
                  cc.d5_calib_epochs, cc.d5_calib_active_epochs, cc.d5_calib_floored_epochs,
                  cc.d5_calib_warmup_epochs, cc.d5_calib_capped_epochs, raw_range);
        if (cc.d5_calib_floored_epochs > 0)
            log->warn("Network (Card 4/M1 D5 v2 LS-VCE calibration): the σ̂₀² FLOOR CLAMP bound "
                      "on {} of {} install(s) ({:.1f}% of the run counting warm-up) — over "
                      "those epochs the calibration was ARMED but the gate ran as the RAW "
                      "FORMAL (v1) test, because the raw σ̂₀² fell below 1 (min {:.4g}) and the "
                      "clamp only RAISES the threshold.  A reported σ̂₀ of 1.00× over a floored "
                      "run is NOT evidence that the formal Cov(v) is honest; it is evidence "
                      "the calibration was inert.  A raw σ̂₀² far below 1 means the "
                      "innovations are far SMALLER than the formal Cov(v) predicts — the "
                      "classic signature of a carry that is not taking data (frozen or "
                      "over-diffuse priors), so read the ddfold counters above before reading "
                      "any D5 conclusion from this run.",
                      cc.d5_calib_floored_epochs, cc.d5_calib_epochs, inert_pct,
                      cc.d5_calib_raw_min);
        else if (cc.d5_calib_active_epochs == 0)
            log->warn("Network (Card 4/M1 D5 v2 LS-VCE calibration): the calibration NEVER "
                      "reached the active state over {} install(s) — {} in warm-up, {} capped. "
                      " Standing invariant #4: the knob is INERT this run, not benign; every "
                      "D5 number below came from a gate that was not calibrated.",
                      cc.d5_calib_epochs, cc.d5_calib_warmup_epochs, cc.d5_calib_capped_epochs);
    }
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
    if (ctx.knobs.clk_frame_hold || ctx.knobs.clk_jump_test) {
        // Card CLK-R run-end telemetry (invariant #4 — the ON-path counters).
        // Σ|μ| is the common-mode churn the wire no longer carries (stage 1);
        // marks are the per-sat re-realizations the wire now DECLARES (stage 2);
        // re-anchors count the leash's marked re-coupling edges (should be 0 on
        // a ≤24 h run — the measured 12 h frame wander is ~23 m vs a 50 m leash).
        const auto& cr = clk_rebase_engine.counters();
        log->info("Network (Card CLK-R / clock emission re-base): {} emit epoch(s), {} row(s); "
                  "frame hold {} — Σ|μ| drained {:.2f} m, max |A_c| {:.2f} m, {} leash "
                  "re-anchor(s); step marks {} — {} mark(s) > {:.2f} m; {} init(s), {} gap "
                  "re-init(s), {} gap-return mark(s) (stage 2b {}).  OUTPUTS-only: solve "
                  "byte-identical; between-sat differences preserved exactly.",
                  cr.epochs, cr.rows,
                  ctx.knobs.clk_frame_hold ? "ON" : "off",
                  cr.sum_abs_mu, cr.max_abs_acc, cr.reanchors,
                  ctx.knobs.clk_jump_test ? "ON" : "off",
                  cr.marks, config_.networkClockDiscJumpM(), cr.inits, cr.gaps,
                  cr.gap_marks,
                  config_.networkClockDiscGapJumpM() > 0.0
                      ? fmt::format("ON at {:.2f} m", config_.networkClockDiscGapJumpM())
                      : std::string("off — a stale return is an UNTESTED first sighting"));
    }
    if (ctx.knobs.cta_on) {
        // Card Q3 run-end telemetry (standing invariant #4 — the ON-path counters
        // ARE the evidence).  `folded_anchored = 0` means the anchor never bound a
        // single row: the mechanism is INERT and every downstream number in the run
        // is the legacy carry, no matter how clean the log reads.  Trips are split
        // by REASON because they are not interchangeable evidence — a run that only
        // ever shows `resume`/`datum` has tested neither the detector nor the
        // staleness guard, and per standing invariant #1 an untested guard is not a
        // guard (the DOY-129 event-window arm is what closes that).
        const auto& c = ctx.counters;
        const long long folds = c.cta_folded_anchored + c.cta_folded_loose;
        const long long trips = c.cta_trip_nis + c.cta_trip_abs + c.cta_trip_gap
                              + c.cta_trip_datum + c.cta_trip_ext;
        log->info("Network (Card Q3 / guarded temporal clock anchor): {} epoch(s); {} clock "
                  "carry fold(s) — {} ANCHORED at q = {:.3e} m²/s (targets √(q·Δt) = {:.2f} cm "
                  "at 30 s), {} LOOSE, {} at the variance floor.  Trips {} = {} nis + {} abs + "
                  "{} gap + {} datum + {} ext; {} in-epoch re-solve(s) ({} failed), {} "
                  "re-coupling edge(s), {} wire mark(s) queued.  Largest |w| {:.2f} (k = {:.2f}) "
                  "on {} at TOW {:.1f}.",
                  c.cta_epochs, folds, c.cta_folded_anchored, ctx.knobs.cta_q,
                  100.0 * std::sqrt(std::max(0.0, ctx.knobs.cta_q) * 30.0),
                  c.cta_folded_loose, c.cta_floor_hits, trips, c.cta_trip_nis,
                  c.cta_trip_abs, c.cta_trip_gap, c.cta_trip_datum, c.cta_trip_ext,
                  c.cta_resolves, c.cta_resolve_failed, c.cta_recouples, c.cta_marks,
                  c.cta_max_w, ctx.knobs.cta_k,
                  c.cta_max_w_sat.empty() ? std::string("(none)") : c.cta_max_w_sat,
                  c.cta_max_w_tow);
        if (ctx.knobs.carry_gauge_free) {
            // §2.1: the gauge-free step's own denominator.  `gf_keys = 0` on an
            // armed run means the downdate never reached a group of ≥ 2 carried
            // clocks, so the anchor is STILL constraining the gauge — the exact
            // failure the step exists to prevent, and invisible without this line.
            log->info("Network (Card Q3 §2.1 / gauge-free fold): {} clock prior key(s) "
                      "gauge-downdated over {} epoch(s); {} epoch(s) INERT (no constellation "
                      "contributed 2+ carried clocks, or UᵀΛU was singular).  Expect the "
                      "CLK-R drained common mode Σ|μ| to RETURN toward the control's level — "
                      "the anchor no longer does the datum's work.",
                      c.cta_gf_keys, c.cta_epochs, c.cta_gf_inert);
            if (c.cta_gf_keys == 0)
                log->warn("Network (Card Q3 §2.1): the gauge-free fold is ARMED but downdated "
                          "ZERO keys — it is INERT for this run and the anchor is folding on "
                          "the RAW state exactly as if the knob were off.  Any A/B against a "
                          "raw-state arm is measuring nothing.");
        }
        if (c.cta_folded_anchored == 0)
            log->warn("Network (Card Q3): the anchor folded ZERO anchored rows — it is INERT for "
                      "this run.  Every satellite stayed LOOSE, so the emitted clock is the "
                      "legacy carry's.  Check that satellites are surviving the re-coupling "
                      "re-validation ({} consecutive in-band epochs) and that the staleness "
                      "bound ({:.0f} s) is above this run's epoch spacing.",
                      ctx.knobs.cta_recouple_ep, ctx.knobs.cta_max_gap_s);
        // Only the FAULT-class trips (nis/abs) are resolve candidates: a gap,
        // datum or ext decouple withdraws the prior BECAUSE the frame or the
        // visibility moved, emits a fresh solve, and has no anchored value to
        // correct.  Counting them here cried "214 warped epochs" on a 12 h quiet
        // arm whose fault-trip count was exactly zero (0 nis + 0 abs + 13 gap +
        // 201 datum, `camp60wu_quiet12h_r2q3`) — the ratio=inf lesson again: a
        // composite denominator makes a healthy run read as a broken one.
        if (const long long fault_trips = c.cta_trip_nis + c.cta_trip_abs;
            ctx.knobs.cta_resolve && c.cta_resolves == 0 && fault_trips > 0)
            log->warn("Network (Card Q3): {} FAULT trip(s) (nis+abs) but ZERO in-epoch "
                      "re-solves — every one was decoupled and marked WITHOUT the same-epoch "
                      "value correction, so each shipped an anchor-warped epoch.  See the "
                      "per-trip warnings for the cause (prior-dominated column, or no "
                      "captured prior to withdraw).", fault_trips);
        // §2.1 — which DENOMINATOR this run's guard and census standardized by.
        // Two arms of the bisection differ ONLY here, so a run-end line that omits
        // it makes the pair indistinguishable in the archived console.
        log->info("Network (Card Q3 §2.1): guard/census denominator — P⁻ {} , P⁺ {} (fold: {}).",
                  ctx.knobs.cta_stat_proj_prior ? "GAUGE-PROJECTED" : "raw per-satellite var_eff",
                  ctx.knobs.cta_stat_proj_post  ? "GAUGE-PROJECTED" : "raw marginal cov(c,c)",
                  ctx.knobs.carry_gauge_free ? "gauge-free" : "raw state");
        // §3's k derivation, per rung.  The denominator √S shrinks as q tightens, so
        // a FIXED k is a different number of METRES at every rung — the ladder below
        // is what makes that visible without a second disarmed run per rung, and the
        // metre ladder is what the ABSOLUTE backstop's rail is read off (the 3 h arm
        // found 0 of 63 nis trips would have been caught by the shipped 2.0 m).
        if (c.cta_w_rows > 0) {
            const double n     = static_cast<double>(c.cta_w_rows);
            const double msq   = c.cta_sum_sqrt_s / n;
            const double s0    = std::sqrt(c.cta_sum_w2 / n);
            std::string wl, el;
            for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
                wl += fmt::format("{}>={:g}:{} ({:.3g}%)", b ? "  " : "", kCtaWLadder[b],
                                  c.cta_w_ge[b], 100.0 * static_cast<double>(c.cta_w_ge[b]) / n);
            for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
                el += fmt::format("{}>={:g}m:{} ({:.3g}%)", b ? "  " : "", kCtaELadder[b],
                                  c.cta_e_ge[b], 100.0 * static_cast<double>(c.cta_e_ge[b]) / n);
            log->info("Network (Card Q3 §3 / guard ladder over {} ANCHORED row(s)): mean √S = "
                      "{:.4f} m ⇒ the armed k = {:.2f} is a {:.3f} m bar at this rung; mean |ê| = "
                      "{:.4f} m; guard σ̂₀ = √(Σw²/n) = {:.3f}.  |w| ladder: {}.  |ê| ladder: {}.",
                      c.cta_w_rows, msq, ctx.knobs.cta_k, ctx.knobs.cta_k * msq,
                      c.cta_sum_abs_e / n, s0, wl, el);
            log->info("Network (Card Q3 §3): read k off the |w| ladder ABOVE, not off a trip "
                      "count — but note this population is TRUNCATED across epochs (a tripped "
                      "satellite goes LOOSE and stops contributing until it re-couples), so the "
                      "upper bins UNDER-report.  The un-truncated null needs an arm with "
                      "network.clock_anchor_k beyond the top rung and network.clock_anchor_abs_m "
                      "= 0 — that is what §3 means by 'with the guard disarmed'.");
        }
        if (ctx.knobs.cta_var_floor > 0.0 && c.cta_floor_hits == 0 && c.cta_folded_anchored > 0)
            log->info("Network (Card Q3): the variance floor never bound ({:.3e} m² vs the armed "
                      "q·Δt) — the consistency cap is armed but inactive at this q, which is the "
                      "expected reading while q·Δt stays above it.", ctx.knobs.cta_var_floor);
        else if (ctx.knobs.cta_var_floor > 0.0 && c.cta_folded_anchored > 0)
            // The floor BINDING at the bottom rungs is the design, not a warning
            // condition (§7.4): below q·Δt ≈ (3 mm)² the anchor would otherwise
            // claim precision no honest correlated-error floor supports, and
            // deviation #1's consistency cap is what stops it.  Say how hard it
            // bound, because "the floor governs" and "the floor is a rounding
            // error" are different postures and only the fraction separates them.
            log->info("Network (Card Q3 / deviation-#1 consistency floor): the floor BOUND on {} "
                      "of {} anchored fold(s) ({:.1f} %) at {:.3e} m² (σ = {:.2f} mm) against the "
                      "armed q·Δt = {:.3e} m² at 30 s.  A binding floor is the DESIGN at these "
                      "rungs, not a fault: it is the cap that keeps an independence-assuming "
                      "carry from accumulating information against a time-correlated error "
                      "floor.  Grade it on the census's clk family and the wire, never on the "
                      "fact that it bound.",
                      c.cta_floor_hits, c.cta_folded_anchored,
                      100.0 * static_cast<double>(c.cta_floor_hits)
                            / static_cast<double>(c.cta_folded_anchored),
                      ctx.knobs.cta_var_floor, 1000.0 * std::sqrt(ctx.knobs.cta_var_floor),
                      std::max(0.0, ctx.knobs.cta_q) * 30.0);
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
    // Card C-GF run-end readout.  Deliberately OUTSIDE the Card-Q3 `cta_on` nest below:
    // the downdate can now be armed by the CARRY (shared_carry_gauge_free) with the Q3
    // temporal anchor off, and the Q3 readout would then print nothing at all — an armed
    // mechanism with zero telemetry, which is exactly the standing-invariant-#4 failure.
    if (ctx.knobs.carry_gauge_free && !ctx.knobs.cta_on) {
        log->info("Network (Card C-GF): gauge-free shared carry active WITHOUT the Q3 clock "
                  "anchor — {} clock prior key(s) gauge-downdated, {} epoch(s) inert, {} "
                  "direction(s) DECLINED for partial support (closure test {}).",
                  ctx.counters.cta_gf_keys, ctx.counters.cta_gf_inert,
                  est.gaugeFreePartialSkipped(),
                  ctx.knobs.carry_gauge_free_closure ? "ON" : "off");
        if (ctx.counters.cta_gf_keys == 0)
            log->warn("Network (Card C-GF): the gauge-free carry fold is ARMED but downdated "
                      "ZERO keys — it is INERT for this run, and the carried block is folding "
                      "information along the datum's own directions exactly as if the knob "
                      "were off.  Any A/B against a raw-carry arm is measuring nothing.");
    }
    if (ctx.knobs.carry_gauge_free_closure && est.gaugeFreePartialSkipped() > 0)
        log->info("Network (Card C-GF): {} datum direction(s) were left folded because their "
                  "support was only PARTLY carried (some of the constellation's registered "
                  "clocks were absent from the carried set).  That is the CORRECT refusal — "
                  "restricted to a subset the direction is no longer the datum's functional, "
                  "and downdating it would delete an estimable carried-vs-uncarried contrast "
                  "rather than gauge.  A count comparable to the downdated-key total means "
                  "the border turns over faster than the carry, so the gauge-free treatment "
                  "is reaching only a minority of epochs.",
                  est.gaugeFreePartialSkipped());
    // Card 1g run-end readout.  Its own denominator — the run-total departed count moves
    // for many reasons and cannot tell armed-and-working from armed-and-inert.
    if (carry_cpl_cond && ctx.knobs.carry_cpl_reinit) {
        log->info("Network (Card 1g): realization-step reinit sources — {} coupling term(s) "
                  "diverted to the departed route by the WIDENED (kind,sat,signal) probe "
                  "(terms the legacy sat-wide probe would have folded as PRESENT, in a "
                  "retired frame).", ctx.counters.n_cpl_reinit_diverted);
        if (ctx.counters.n_cpl_reinit_diverted == 0)
            log->warn("Network (Card 1g): armed but diverted ZERO terms — no realization-step "
                      "mark ever coincided with a live coupling term on the same "
                      "(kind,sat,signal).  The card is INERT for this run: either no "
                      "re-reference occurred, or the feeds are miswired.  Check the "
                      "PBGAUGE / eq34 / datum-slip event counts before reading an A/B as "
                      "'no effect'.");
    }
    // Card 1f run-end readout.  Standing invariant #4: this card acts only on TRUNCATED
    // rows, so "0 held / 0 orphan" with departed terms > 0 is a MISWIRE, not a quiet run —
    // the two are distinguished here rather than left to the reader.
    if (carry_cpl_cond && ctx.knobs.carry_cpl_gauge) {
        log->info("Network (Card 1f): coupling-row datum-gauge projection — {} "
                  "(constellation,kind) family sum(s) HELD at their harvest value against a "
                  "truncated column, {} ORPHANED (truncated mass with no present recipient — "
                  "those rows stayed gauge-dirty).",
                  ctx.counters.n_cpl_gauge_held, ctx.counters.n_cpl_gauge_orphan);
        if (ctx.counters.n_cpl_gauge_held == 0 && ctx.counters.n_cpl_gauge_orphan == 0
            && n_cpl_terms_departed > 0)
            log->warn("Network (Card 1f): {} departed coupling term(s) were injected but the "
                      "gauge projection held NOTHING — the card is armed and inert.  Expected "
                      "causes: every departed term was VtecSh or had an empty satellite id "
                      "(both excluded by design), or the rows never reached the restore.  "
                      "Investigate before reading an A/B as 'no effect'.",
                      n_cpl_terms_departed);
        if (ctx.counters.n_cpl_gauge_orphan > 0) {
            // Orphan ATTRIBUTION (multi-constellation owed item, closed 2026-08-20):
            // per-(constellation,kind) totals so the run-end line names WHERE the
            // gauge-dirty residue lives; per-epoch lines are in the run log
            // ("Card 1f orphan attribution").  Recipient-fallback decision:
            // NONE by design — the sums realize per-constellation.
            std::string fam_detail;
            for (const auto& [fam, o] : ctx.counters.cpl_orphan_fam_total) {
                const char* kind_s =
                    fam.second == static_cast<int>(SharedParamKind::Clock)     ? "CLK" :
                    fam.second == static_cast<int>(SharedParamKind::PhaseBias) ? "PB"  :
                    fam.second == static_cast<int>(SharedParamKind::CodeBias)  ? "CB"  : "SH";
                fam_detail += fmt::format("{}{}|{}: {} (Σ|m| {:.3g})",
                                          fam_detail.empty() ? "" : ", ",
                                          fam.first, kind_s, o.first, o.second);
            }
            log->warn("Network (Card 1f): {} family(ies) had truncated coefficient mass with NO "
                      "present column to carry it — the whole family departed at once, so the "
                      "row cannot be made gauge-invariant this epoch and still observes the "
                      "datum gauge.  This is the residue the card cannot remove; if it is a "
                      "large fraction of `held`, the carry is outliving its border.  "
                      "Attribution: [{}].",
                      ctx.counters.n_cpl_gauge_orphan, fam_detail);
        }
    }
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
    if (ctx.knobs.vce_apply_on) {
        // M2 v2 (docs/SPEC_carry_vce_apply.md §9.4): the ON-path telemetry.  "0
        // steps" means the wiring is inert no matter how clean the log reads —
        // standing invariant #4 — so the step / freeze split is printed against a
        // denominator, per group, and a run that never left warm-up says so in one
        // line rather than being inferred from a missing effect.
        const NetworkCarryVceApply& va = ctx.carry.vce_apply;
        log->info("Network (M2 v2 / LASER 5-95): carry variance-FACTOR summary — "
                  "families='{}', slew {}, ceiling {}, min_rows {}; {} epoch(s) stepped "
                  "the iteration.  alpha2 multiplies the carry fold variance; "
                  "sigma_hat_post = target/alpha2 is the variance of unit weight under "
                  "the weights the solve USED and should approach 1.  Trajectory: "
                  "network_carry_vce_apply.csv.",
                  config_.networkCarryVceApplyFamilies(), va.slew(), va.alpha2Max(),
                  static_cast<long long>(va.minRows()), va.epochs());
        if (va.groups().empty())
            log->warn("  NO GROUPS: the apply path never saw a monitored (family, "
                      "constellation) accumulator — the factor was unity for the whole run "
                      "and this arm is INERT (check network.carry_vce_components and that "
                      "the carry actually folded).");
        for (const auto& kv : va.groups()) {
            const NetworkCarryVceApply::Group& g = kv.second;
            log->info("  [{}|{}] alpha2 = {:.3f} (max {:.3f}, target {:.3f}, "
                      "sigma_hat_post {:.3f}); robust {:.3e} m2 vs hand fold {:.3e} m2; "
                      "{} stepped / {} frozen ({}), {} slew-up / {} slew-down, "
                      "{} ep at ceiling, {} ep at unit; n_eff {:.0f}.",
                      NetworkCarryVceApply::familyName(g.family),
                      g.sys ? std::string(1, g.sys) : std::string("*"),
                      g.alpha2, g.alpha2_max, g.target, g.sigma_hat_post,
                      g.robust_floor, g.hand_fold_var,
                      g.n_steps, g.n_frozen,
                      NetworkCarryVceApply::freezeName(g.freeze),
                      g.n_slew_up, g.n_slew_down, g.n_ceiling_ep, g.n_unit_ep, g.n_eff);
            if (g.n_steps == 0)
                log->warn("    [{}|{}] NEVER STEPPED — the factor stayed at {:.3f} for the "
                          "whole run (last freeze reason '{}').  Report this arm as "
                          "apply-INERT for that group, not as apply-neutral.",
                          NetworkCarryVceApply::familyName(g.family),
                          g.sys ? std::string(1, g.sys) : std::string("*"),
                          g.alpha2, NetworkCarryVceApply::freezeName(g.freeze));
            if (g.n_ceiling_ep > 0)
                log->warn("    [{}|{}] spent {} epoch(s) PINNED at the alpha2 ceiling {} "
                          "(target {:.1f}) — the measured carry variance exceeded what the "
                          "ceiling allows the model to admit, so those epochs folded a "
                          "prior that is still stiffer than the data says.",
                          NetworkCarryVceApply::familyName(g.family),
                          g.sys ? std::string(1, g.sys) : std::string("*"),
                          g.n_ceiling_ep, va.alpha2Max(), g.target);
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

    // Tier T1r (§6.14.5 defect 1 / invariant #4): drain the IF-TD REFERENCE arm's
    // own opportunities and flags out of the per-station detectors.  NOT gated on
    // nc.slip_qc.ref_gate — the LEGACY reference condition runs and acts on every
    // lineage, armed or not, so gating this on the hardening toggle would leave the
    // tier reading 0 on precisely the runs where it is the only reference screen.
    if (ctx.knobs.trace_flag_rates) {
        auto& fr = ctx.counters.fr_if_refgate;
        fr.opp = fr.flag = 0;
        for (const auto& st : stations) {
            if (!st.if_slip) continue;
            const auto& c = st.if_slip->refGateCounters();
            fr.opp  += static_cast<long>(c.gate_opportunities);
            fr.flag += static_cast<long>(c.gate_flags);
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
    // EVENT HARNESS run-end (filter/NetworkInject.cpp): the clause, and the
    // never-fired HARD error.  Read BEFORE the zero-emit guard so an armed-but-
    // silent schedule is reported even on a run that also produced nothing.
    const int inject_rc = networkInjectRunEnd(ctx, log);

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
    return inject_rc;
}

} // namespace GnssProcessor
