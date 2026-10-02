/**
 * @file NetworkTeardown.cpp
 * @brief Implementation of @ref networkTeardown, the run-end readout tail of
 *        Application::runNetwork.  The body was moved out of the driver without edits
 *        (dedent 0) at the teardown-tail decomposition; the code added at the move is
 *        the alias preamble, which re-creates the driver's ctx/config/setup/traces
 *        aliases so the moved body reads its original names.
 */

#include "filter/NetworkInject.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp" // run-end counter readout (card CPA)
#include "filter/NetworkTeardown.hpp"

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkChannelAll.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "measurement/MeasurementModel.hpp"   // complete type for NetStation::mm
#include "model/PppModel.hpp"                  // complete type for NetStation::model (wind-up census)
#include "app/Application.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkEmitRebase.hpp"   // emit-layer frame re-base (Card 4 (M1) phase D)
#include "products/NetworkProductEmitter.hpp" // kDiscDecommonMinSats for the run-end line (card JT)
#include "products/NetworkPbEmitFilter.hpp" // PB emission-continuity filter (M5 prerequisite)
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkCarryVce.hpp"        // LS-VCE carry-row variance-component monitor (M2)
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (Card 4 (M1) C3b)
#include "filter/NetworkMwWlLayer.hpp"             // MW-seeded wide-lane pseudo-obs layer (M5 card WL-1)
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "filter/NetworkWlRunTypes.hpp" // WL and RAII types hoisted out of runNetwork() (INC-9b)
#include "filter/NetworkRobustStats.hpp"   // median/MAD robust attribution helpers (INC-2)
#include "filter/NetworkBlockBaarda.hpp"       // BlockFormal LOS-block census (networkBlockBaardaReport)
#include "filter/NetworkCongruence.hpp"        // station congruence screen (networkCongruenceReport)
#include "filter/NetworkMdb.hpp"               // Baarda MDB (networkMdbReport, which holds the run-end body)
#include "filter/NetworkClockAnchor.hpp"       // kCtaWLadder / kCtaELadder for the guard-ladder run-end line (card Q3 §3)
#include "filter/NetworkClockLevel.hpp"        // level re-base run-end readout (card CLV)
#include "filter/NetworkRedundancyCensus.hpp"  // redundancy census (kCensusMinPoolRows for the run-end line)
#include "filter/NetworkPrefitCensus.hpp"      // prefit census (kNetPrefitMinTypeRows / kNetPrefitPinWeight for the run-end line)
#include "app/NetworkSetupResult.hpp"   // setup-phase handoff (setup) and navAvailabilityGpsSeconds (INC-5)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic (B5)
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads, used by the driver to pin Eigen to 1 thread inside its per-station OpenMP region (no nested oversubscription, deterministic result); not called in this file
#include <fmt/format.h>
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp.  Carried over from the driver; this file has no OpenMP code
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
#include <numeric>             // std::iota (netar ILS-cap subset ordering); carried over from the driver, not used in this file
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
    /// Verdict of the joint-block PD-fallback bar, folded into the run's return code at
    /// the tail.  0 until the bar below sets it (RF-1 step 3).
    int amb_block_rc = 0;
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
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // phase-bias carry floor (D2, B4); 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // code-bias carry floor (D2, B4);  0 = off
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // conditional local carry (coupling rows); requires fullcov_on, enforced at load (Card 1)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // joint whitening of the coupling rows (departed cols → M_w); requires carry_cpl_cond, enforced at load (Card 1b)
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // border time update of the coupling rows at injection (κ shrink + M²κq inflation); requires carry_cpl_cond, enforced at load (Card 1d)
    const bool   carry_vce     = ctx.knobs.carry_vce;
    const double contract_rebroadcast_s = ctx.knobs.contract_rebroadcast_s; // S-contract re-broadcast period in product time, s; 0 = off (RTA-5)
    const bool   clk_runaway_on  = ctx.knobs.clk_runaway_on;        // per-constellation clock-runaway quarantine (RT0-5)
    const double clk_runaway_mag = ctx.knobs.clk_runaway_mag;         // ceiling on median |clock − global median|, m (RT0-5)
    const double clk_runaway_rate= ctx.knobs.clk_runaway_rate;      // ceiling on median |drift|, m/s (RT0-5)
    const bool   perconst_on       = ctx.knobs.perconst_on;          // per-constellation carve-out of the A4 σ̂0 withhold (RTA-10)
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

    double loop_ms = 0.0;   // hoisted out of the timing block so the run-summary CSV / manifest can read it (L3/L15)
    {  // Timing: per-epoch scaling measurement (loop wall only; startup excluded)
        loop_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_loop_begin).count();
        log->info("Network[timing]: epoch-loop wall {:.1f} ms over {} epochs = {:.2f} ms/epoch "
                  "| {} stations | {} | threads {}",
                  loop_ms, n_loop_epochs,
                  n_loop_epochs ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0,
                  stations.size(), parallel_on ? "PARALLEL" : "SERIAL", n_threads);
    }

    // === Tier D0: section-timer run totals ===================================
    // Whole-run time breakdown that ranks the Tier-D optimizations.  The accumulators
    // are run-cumulative: the driver's assembly wall and the kernel's reduce, solve,
    // cov-inverse and cov-consumers timers.  `other` is the epoch-loop wall minus all
    // five (QC decision logic, emit, I/O, tracers).  Only the ratios carry meaning;
    // absolute ms depend on the machine and the parallel settings.
    //
    // The RAIM, attribution and clock-monitor tiers have no timer of their own: they are
    // interleaved with, and dominated by, covariance-consumer calls (rowRedundancy /
    // reconstructStation), which are timed under cov-consumers.  Their remaining decision
    // logic (median polish, standardization, thresholding) is cheap and falls into
    // `other`.  The D1-relevant QC cost is therefore already isolated in cov-consumers,
    // and bracketing the scattered QC blocks would count those covariance calls twice.
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
        // Cross-cutting: kernel est.addStation (normal forming and N_rr factorization).  Mostly
        // inside the assembly wall, but it also captures within-epoch RAIM re-solve adds that
        // fall after the wall (in `other`), so it is not a strict sub-part of the assembly wall.
        log->info("Network (D0)   est.addStation {:>8.0f} ms ({:4.1f}%)  [{} adds; cross-cutting — "
                  "overlaps assembly-wall + any re-solve in other]",
                  stmr.add_station_ms, pct(stmr.add_station_ms), stmr.add_station_calls);
        // Internal breakdown of foldReducedDdPriors (serial-fold path only, i.e. under
        // parallel_carry): which sub-part of the DD fold dominates (P0b).
        if (stmr.fold_loop_ms + stmr.fold_minv_ms + stmr.fold_gmg_ms > 0.0)
            log->info("Network (D0)   foldDdPriors: per-station-loop {:>8.0f} ms ({:4.1f}%) | "
                      "M-inverse {:>8.0f} ms ({:4.1f}%) | GMGᵀ {:>7.0f} ms ({:4.1f}%)  [serial-fold path]",
                      stmr.fold_loop_ms, pct(stmr.fold_loop_ms),
                      stmr.fold_minv_ms, pct(stmr.fold_minv_ms),
                      stmr.fold_gmg_ms, pct(stmr.fold_gmg_ms));

        // === Invariant-carry driver: per-op split and census (INC-C0) ===========
        // Splits the SB_DDBUILD / SB_INSTALL wall brackets, which lump the whole driver,
        // into its primitive ops: the dense-K² change-of-basis projection (buildDdPriors)
        // and the posterior read-back (installPosterior), both of which INC-C2's factored
        // (A + U Σ_ss Uᵀ) store removes.  The census separates the two cases of §0 for a
        // large carry that is never marginalized: marginalization has nothing to kill
        // (giant live components, case (a)), or the kill path is silent (case (b)).
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
            // Split of the WL-3 lane fold: the ~10³ per-epoch rank-1/rank-2 covariance updates,
            // which SB_DDBUILD lumps into basis-project (LF-1 INC-0).
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
            // Emitted-component fold-dim distribution.  INC-4's Amdahl ceiling is about the
            // heaviest component's work over the total (SPEC §7.4): a single tall high bucket
            // means one giant G component dominates; a spread means the component-parallel
            // fold balances (LF-1 INC-0).
            log->info("Network (D0/LF-1) K_c histogram (emitted components by fold-dim, log2 "
                      "buckets 0|1|2|4|8|16|32|64|128|256+): {} {} {} {} {} {} {} {} {} {}",
                      cc.comp_kc_hist[0], cc.comp_kc_hist[1], cc.comp_kc_hist[2], cc.comp_kc_hist[3],
                      cc.comp_kc_hist[4], cc.comp_kc_hist[5], cc.comp_kc_hist[6], cc.comp_kc_hist[7],
                      cc.comp_kc_hist[8], cc.comp_kc_hist[9]);
            // D-c viability screen: short fundamental cycles keep the cycle matrix G sparse,
            // which is necessary but not sufficient for a bounded exact information-form
            // carry (SPEC_carry_factored_pz_C2 D-c).  The border term is Gᵀ Σ_ss G, so the
            // binding condition is the sparsity and rank of Σ_ss, not cycle length (that
            // spec's §6.1); this counter screens only G (INC-C0).
            log->info("Network (D0/INC-C0) D-c viability screen (G-side only): max "
                      "fundamental-cycle length {} | mean {:.1f} over {} folded chord(s) — {} "
                      "(short cycles ⇒ sparse G: NECESSARY, not sufficient — the binding probe "
                      "is Σ_ss sparsity/rank; long ⇒ dense per-block cost is intrinsic).",
                      cc.max_cycle_len,
                      chords_folded ? static_cast<double>(cc.sum_cycle_len)
                                      / static_cast<double>(chords_folded) : 0.0,
                      chords_folded,
                      cc.max_cycle_len <= 8 ? "SPARSE-G likely" : "DENSE-G risk");
            // Growth bound, also a robustness assert on 12 h runs (INC-C1): a nonzero peak
            // carry with no marginalization over more than 100 driven epochs means dead chords
            // are being retained, the kill-path defect of §0 case (b).  A large per-chord count
            // instead points to case (a).
            if (marg_total == 0 && cc.max_z_dim > 0 && cc.epochs > 100)
                log->warn("Network (INC-C1 growth bound): {} carried DD(s) at peak with ZERO "
                          "marginalization(s) over {} driven epoch(s) — dead chords are NOT being "
                          "killed (INC-C0 §0 case (b)); K grows unbounded.  Audit the kill path "
                          "(NetworkInvariantCarryDriver::buildDdPriors projection-W column drop).",
                          cc.max_z_dim, cc.epochs);
        }

        // === Tier D0b: `other` sub-block run totals, ranked =====================
        // net = wall − Δ(reduce + solve + cov-inverse + cov-consumers) inside each block, so
        // the recon/solve time already in the five D0 sections above is not counted twice.
        // Σ net against the D0 `other` figure closes the accounting; "unattributed" is loop
        // time that no bracket covers (pre-slip pivot pass, the trace/readout tail, small
        // unbracketed gaps).  This ranked table is gate P0 of SPEC_dev_tier_perf_cards §3
        // (the perlos card); P1/P2 target the measured top blocks.
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

    // The run-wide rank-gate telemetry goes into network_estimability.csv, the one
    // artifact a trace-armed run adds, and not into a new run-end log line: the estim
    // golden arm byte-compares the log text, and an unarmed control run has to keep
    // identical text while still yielding the arm-against-arm numbers the storm
    // analyzer compares.  No-op with the trace off (cards DG5/DG6).
    traces.writeEstimabilityRunRow(ctx.counters, est.rankGateTol());
    traces.closeAll();   // closes every run trace stream; the list is in NetworkRunTraces::closeAll (INC-4)

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate, {} by the A4 σ̂0 ceiling) — "
              "of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, synced.size());
    // --- In-solve station count (card RF-PD-fix) -------------------------------
    // Printed unconditionally, unlike the RF-PD readout below: `n_present` in
    // `network_epoch_status.csv` is not the number of stations in the solve, an unarmed
    // run needs that number as much as an armed one, and no other artifact records it.
    // The denominator is printed so that a minimum such as "min 59" can be told apart
    // from a count that was never recorded (invariant #4).
    {
        const auto& c = ctx.counters;
        if (c.sta_in_solve_epochs > 0) {
            std::vector<int> v = c.sta_in_solve_ep;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                             v.end());
            const int med = v[v.size() / 2];
            const int mx  = *std::max_element(c.sta_in_solve_ep.begin(),
                                              c.sta_in_solve_ep.end());
            log->info("Network (card RF-PD-fix): stations IN THE SOLVE — n_sta_in_solve "
                      "min {} (TOW {:.0f}) / median {} / max {} over {} consume epoch(s); "
                      "n_sta_pd_dropped {} total; {} station re-admission(s) after an "
                      "absence (longest gap {} epoch(s)).  n_present in "
                      "network_epoch_status.csv counts the ROSTER, not this set.",
                      c.sta_in_solve_min, c.sta_in_solve_min_tow, med, mx,
                      c.sta_in_solve_epochs, c.pd_drop_total, c.sta_readmit_events,
                      c.sta_readmit_max_gap_ep);
        } else {
            log->info("Network (card RF-PD-fix): n_sta_in_solve was never recorded — no "
                      "consume epoch completed (every epoch aborted, or the loop never ran).");
        }
    }
    // --- addStation PD drop readout (card RF-PD) ------------------------------
    // When addStation rejects a station's block as not PD, the station leaves the solve
    // (`present[i]=false`) while network_epoch_status.csv still counts it in
    // `n_present`; without these counters the drop shows only as a warn line in the
    // rotating log (invariant #4).  The counters always accumulate; the readout prints
    // only with network.trace_station_pd_null armed, so an unarmed run's log text stays
    // byte-identical (as for the estimability telemetry above).
    if (config_.getNetworkConfig().trace_station_pd_null) {
        const auto& c = ctx.counters;
        const double per_ep = c.pd_drop_epochs
            ? static_cast<double>(c.pd_drop_total) / static_cast<double>(c.pd_drop_epochs) : 0.0;
        std::string fam;
        for (const auto& [k, v] : c.pd_drop_family) {
            if (!fam.empty()) fam += ' ';
            fam += fmt::format("{}={}", k, v);
        }
        std::string famm;
        {
            double tot = 0.0;
            for (const auto& [k, v] : c.pd_drop_family_mass) { (void)k; tot += v; }
            if (tot <= 0.0) tot = 1.0;
            for (const auto& [k, v] : c.pd_drop_family_mass) {
                if (!famm.empty()) famm += ' ';
                famm += fmt::format("{}={:.1f}%", k, 100.0 * v / tot);
            }
        }
        log->info("Network (card RF-PD): addStation PD rejections — n_sta_pd_dropped {} over "
                  "{} consume epoch(s) = {:.1f}/epoch (max {} at TOW {:.0f}); {} named by the "
                  "null eigenvector, {} epoch(s) SUNK by a pivot rejection.  Top-column "
                  "family census: [{}]; null-eigenvector MASS by family: [{}].",
                  c.pd_drop_total, c.pd_drop_epochs, per_ep, c.pd_drop_max_ep,
                  c.pd_drop_max_ep_tow, c.pd_drop_named, c.pd_drop_pivot_abort,
                  fam.empty() ? std::string("none") : fam,
                  famm.empty() ? std::string("none") : famm);
        if (c.pd_drop_total > 0 && c.pd_drop_named == 0)
            log->warn("Network (card RF-PD): {} PD rejection(s) and ZERO named null "
                      "directions — the eigen read is INERT (the kernel's null-trace flag "
                      "never reached addStation, or every throw came from a non-PD guard).  "
                      "The rows in network_events.csv carry no direction.", c.pd_drop_total);
    }
    // Heartbeats buffered for a v2 writer open that never happened.  A run that never
    // emits cannot stamp the contract header (no realized datum), so no product file is
    // created; this warning keeps the withheld era on record (invariant #4; RTA-10 gap
    // fix).
    if (!setup.pending_h_pre_open.empty())
        log->warn("Network (RTA-10): run ended with {} withheld-epoch heartbeat(s) "
                  "never written — the run NEVER EMITTED, the deferred v2 product "
                  "writer never opened, and the product file does not exist.  Every "
                  "epoch was withheld; read the run log / traces, not the wire.",
                  setup.pending_h_pre_open.size());
    // Rank-gate calibration telemetry (card DG6).  Printed only when the tolerance is set
    // to a non-default value, so a default run's log text is byte-identical (DG5: with
    // the trace on, the new file is the only artifact that differs).  The same numbers
    // reach every trace-armed run through the RUN record of network_estimability.csv, so
    // the control arm still gets them.  An armed calibration whose counters never move is
    // a miswire, not a quiet run (invariant #4).
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
    // An over-specified withhold has two causes with opposite fixes.  A tolerance set
    // below the config's own gauge depth is fixed by re-calibrating.  A pre-gate fold that
    // deposits information along a declared gauge direction moves that direction up into
    // the real-parameter layer, and no tolerance recovers it: a cutoff below the filled
    // value still does not count it as gauge, and a cutoff above it starts counting real
    // parameter directions as gauge.  (On simnet24: gauge depth 1e-11, carry-filled
    // direction 9.43e-3, weakest real phase-bias layer 1.25e-2, so no cutoff meets the
    // placement rule's 2 decades on either side.)  The invariant-#7 ratio separates the
    // two cases, so the advice below branches on it.
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
    // SBC (network.sbasis_complete) run-end telemetry.  An armed completion that never
    // fired on a multi-constellation run means either that the deficiency it targets
    // never occurred (fine) or that the block is miswired (invariant #4, read as for
    // WE-CI).  The observed-count envelope in the Card-DG line above separates the two:
    // a max above the declared dimension with n_sbc_epochs == 0 is the miswire.
    // Refusals are counted per cause because each needs a different fix.
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
    // Datum graph census run-end telemetry (docs/SPEC_datum_graph_census.md).  The
    // reading expected when nothing is wrong, "k = 1 everywhere, zero predictions", is
    // also exactly what a seam that was never called produces (invariant #4).  So both
    // `epochs` and `epochs_quiet` are printed, and a run that walked zero support edges
    // is reported as a miswire rather than as a network with no deficiency.
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
    // --- Within-epoch tier response census (SPEC_pivot_visibility_gauge_defect
    // §5.07.5, R0).  Run counters for the tier's refloats and exclusions, which
    // otherwise appear only as per-epoch T2 event rows; the withhold has its own run
    // counter.  The clock move is the §5.07.2 impulse-channel meter: how far the tier's
    // re-solves moved a satellite clock off the primary solve, reported with the
    // satellite's identity (invariant #7: counters and identities).  Zero responses on
    // an armed run means a quiet run, not dead wiring: n_adapt trips are logged per
    // epoch, and the carry_warm gating noted on the WE-CI line above applies here too.
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
        // Failed re-solves (card RPD section 7), on a line of their own so the line above
        // keeps the shape scripts/cm_grade.py parses.  A failure is a replay that could
        // not rebuild a system the primary solve admitted, so any count is a warning.
        if (sc.we_resolve_failed == 0)
            log->info("Network (T2 within-epoch re-solve failures): 0 re-solve(s) failed; "
                      "0 station-epoch(s) dropped by an undo; 0 undo(s) incomplete.");
        else
            log->warn("Network (T2 within-epoch re-solve failures): {} re-solve(s) failed and "
                      "were undone to the last landed state (first at TOW {:.1f}, {}); {} "
                      "station-epoch(s) the undo could not re-add were left out of the final "
                      "solve; {} undo(s) could not complete, each epoch skipped.",
                      sc.we_resolve_failed, sc.we_resolve_fail_tow, sc.we_resolve_fail_at,
                      sc.we_undo_sta_dropped, sc.we_restore_failed);
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
    // An armed run with zero boosts is either a quiet ionosphere (many feeds, max
    // multiplier about 1: the monitor ran and found nothing) or dead wiring (zero
    // feeds).  The feed count is printed, and zero feeds warned, so the two cases do
    // not look alike (invariant #4).
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

    // --- Two-edge continuous handover run-end telemetry --------------------------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.1).  The denominator is
    // `reattaches`, the number of gauge changes the run made at all, which is counted
    // whether or not the fix is armed: a run with zero handovers says nothing about the
    // mechanism (invariant #4).  The two edges are then split: `handover_continuous`
    // counts the frame-preserving handovers and `handover_cold` the unavoidable
    // re-realizations.  A cold handover is not a failure; it reports a frame step the
    // consumer has to be told about.
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

    // --- Cold anchor-fallback run-end telemetry (docs/SPEC_event_harness.md §15) --
    // Three populations together describe what the pass did (invariant #4):
    // usable_elected (the pass elects a usable candidate), rank_changed (that candidate
    // is not the station the coverage sweep takes first; the population the pass
    // targets), and no_usable_fallback (no candidate is usable, so the sweeps' cold path
    // runs).  The four skip counters partition the skipped total by the reason for each
    // refusal, which separates "the ledger is thin" from "the maturity bar is too tight".
    if (ctx.knobs.pd_candidacy_usable) {
        const auto& h = ctx.counters.hyg;
        const long long entered = h.candidacy_usable_elected
                                + h.candidacy_no_usable_fallback;
        log->info("Network (COLD ANCHOR-FALLBACK): the usable-first candidacy pass ran on "
                  "{} handover election(s) — {} elected a candidate whose carry is USABLE "
                  "(of which {} were NOT the station today's sweep would have taken: the "
                  "cold re-realizations this card removed) and {} found NO usable "
                  "candidate and fell through to today's sweeps (the honest cold path).  "
                  "{} (candidate, family) pair(s) were skipped: {} no ledger entry, {} "
                  "refused by the variance/ratio ceiling, {} freed this epoch, {} younger "
                  "than {} epochs.",
                  entered, h.candidacy_usable_elected, h.candidacy_rank_changed,
                  h.candidacy_no_usable_fallback, h.candidacy_skipped_unusable,
                  h.candidacy_skip_no_entry, h.candidacy_skip_var_refused,
                  h.candidacy_skip_freed, h.candidacy_skip_young,
                  ctx.knobs.pd_candidacy_min_age_ep);
        if (entered == 0)
            log->warn("Network (COLD ANCHOR-FALLBACK): ARMED but the pass never ran — no "
                      "election this run reached a handover with an incumbent to hand "
                      "over FROM.  The knob is inert on this window; nothing it reports "
                      "is evidence either way (invariant #4).");
        else if (h.candidacy_rank_changed == 0)
            log->info("Network (COLD ANCHOR-FALLBACK): the pass ran on {} election(s) and "
                      "changed NOT ONE of them — every usable candidate was already the "
                      "station today's coverage sweep takes first.  Byte-identical to OFF "
                      "on this window by construction, not by luck.",
                      entered);
        // Satellite clause, iteration 2 (docs/SPEC_event_harness.md §15.8).  Rule (c):
        // the distinct-anchor census is not recomputed here; it is read from the
        // always-on counters the storm grade also reads (networkSplitReferencedSats, run
        // every epoch whatever the knobs), so the clause and the grade agree by
        // construction.
        if (ctx.knobs.pd_candidacy_satellite) {
            const auto& C = ctx.counters;
            const double ep = C.n_cb_split_epochs > 0
                            ? static_cast<double>(C.n_cb_split_epochs) : 1.0;
            log->info("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): {} satellite-epoch(s) "
                      "resolved jointly over {} family(ies) — {} decided by the held-host "
                      "preference (a PARTIAL handover co-located on the station already "
                      "anchoring the satellite's other families), {} where the family-only "
                      "rule would have picked a DIFFERENT station (the split references this "
                      "clause removed), {} pinned at the joint host through the COLD path (a "
                      "frame step, not a split), {} the joint host does not TRACK (the only "
                      "residual split this clause can leave), and {} satellite-epoch(s) with "
                      "no usable candidate for ANY mover.  Resulting SPLIT-REFERENCED census "
                      "(the storm grade's own numbers): mean {:.2f} / max {} satellites of "
                      "{:.1f} anchored, over {} epoch(s).",
                      h.sat_clause_satellites, h.sat_clause_families,
                      h.sat_clause_colocated, h.sat_clause_split_avoided,
                      h.sat_clause_cold_colocated, h.sat_clause_host_untracked,
                      h.sat_clause_no_usable,
                      static_cast<double>(C.n_cb_split_sats_sum) / ep,
                      C.n_cb_split_sats_max,
                      static_cast<double>(C.n_cb_split_multi_sum) / ep,
                      C.n_cb_split_epochs);
            if (h.sat_clause_families > 0 && h.sat_clause_split_avoided == 0)
                log->info("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): the clause ran on "
                          "{} family(ies) and the family-only rule would have chosen the SAME "
                          "station every time — this window had no split to avoid, which is "
                          "not evidence the clause works.",
                          h.sat_clause_families);
            if (h.sat_clause_host_untracked > 0)
                log->warn("Network (COLD ANCHOR-FALLBACK, SATELLITE clause): {} family(ies) "
                          "could not be co-located because the joint host does not track their "
                          "key — those are the ONLY families that can still split their "
                          "satellite's reference.",
                          h.sat_clause_host_untracked);
        }
    }

    // --- PHASE_ANCHOR carry re-reference run-end telemetry (queue item 1(d)) ------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  As in the handover
    // block above, `reattaches` is the denominator (invariant #4), and an armed run
    // whose elections handed over while the re-reference set never populated is
    // reported as dead wiring.
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
    // (docs/SPEC_eq34_reattach_hygiene.md).  An armed run whose gate never ran is a
    // different failure from one that ran and found nothing to block (invariant #4), so
    // the denominators (`epochs`, `arcs_tracked`) are reported first and an all-zero
    // feed gets a warning rather than looking quiet.
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

    // --- RAIM pool re-host run-end telemetry -----------------------------------
    // (docs/SPEC_raim_pool_rehost.md).  Without the re-host, post-fit RAIM shows as
    // armed in the loader summary on a carry_invariant run while screening nothing, so
    // an inert tier is the failure this readout has to expose (invariant #4).  The
    // denominators come first so that four outcomes that otherwise look identical can
    // be told apart: seam never ran, census empty, nothing flagged, actuator off.
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

    // --- Gross line-of-sight screen run-end telemetry ---------------------------
    // (card GL, docs/SPEC_event_harness.md §24.18.)  These counters are the ON-path
    // telemetry (invariant #4).  The denominators come first so that four outcomes that
    // all look like "the screen found nothing" stay distinguishable: the seam never
    // ran, no code row reached it, nothing cleared the bar, or the evidence never
    // persisted.  The per-station and per-satellite declaration tables come last; they
    // are the identities the grade needs (invariant #7: an edge without an identity is
    // not an event).
    if (ctx.knobs.gl_on) {
        // The fold runs inside the parallel produce phase, so its row count is kept per
        // station (as si_rows_demoted is); it is summed here, once, before the block
        // reads it.
        ctx.counters.gross_los.rows_deweighted = 0;
        for (const auto& st : stations) ctx.counters.gross_los.rows_deweighted += st.gl_rows_deweighted;
        const auto& g = ctx.counters.gross_los;
        log->info("Network (gross-LOS screen): {} seam pass(es), {} station-epoch(s) with "
                  "code rows, {} LOS-epoch(s) screened over {} code row(s).",
                  g.epochs, g.stations_screened, g.los_screened, g.rows_screened);
        if (g.epochs > 0 && g.rows_screened == 0)
            log->warn("Network (gross-LOS screen): DEAD WIRING — the seam ran {} time(s) and "
                      "NO code row ever reached it.  The classification is the same one "
                      "networkPostfitSigma0 uses (a DATA row that is not phase and touches a "
                      "CB: or CLK: shared column); an empty census means the posture "
                      "registers neither, so check the shared-column labels before reading "
                      "this run as clean.",
                      g.epochs);
        log->info("Network (gross-LOS screen): {} LOS-epoch(s) over the {:.1f}σ bar on ≥ {} "
                  "row(s); worst standardized code residual {:.1f}σ, worst |v| {:.2f} m.",
                  g.los_over, config_.getNetworkConfig().gross_los_k,
                  config_.getNetworkConfig().gross_los_min_rows, g.r_max, g.v_abs_max_m);
        if (g.los_screened > 0 && g.los_over == 0)
            log->info("Network (gross-LOS screen): ZERO OPPORTUNITY — it screened real code "
                      "residuals and nothing cleared the bar.  A clean window, not a "
                      "demonstration: the screen's power is unproven until it is graded "
                      "against a window that carries a known gross line of sight.");
        else if (g.los_over > 0 && g.declared == 0)
            log->warn("Network (gross-LOS screen): {} LOS-epoch(s) cleared the bar but NOTHING "
                      "was declared — the evidence never persisted {} consecutive epoch(s).  "
                      "That is the persistence gate doing its job on heavy tails, or it is "
                      "the gate starving a real fault that flickers; the |v|/σ distribution "
                      "in network_pf_rows.csv is what separates the two.",
                      g.los_over, config_.getNetworkConfig().gross_los_epochs);
        log->info("Network (gross-LOS screen) GUARDS: {} LOS-epoch(s) held by the MATURITY "
                  "horizon ({} emitted epochs) and {} by the WHOLE-STATION stand-down, which "
                  "fired on {} station-epoch(s).  A held LOS-epoch is a COMPLETE declaration a "
                  "guard refused — never a silent one.",
                  g.suppressed_warmup, config_.getNetworkConfig().gross_los_warmup_epochs,
                  g.suppressed_station, g.standdowns);
        log->info("Network (gross-LOS screen): {} declaration(s), {} release(s), {} "
                  "expiry(ies) on the absent-satellite edge; {} currently gross at run end, "
                  "{} the largest simultaneous set.",
                  g.declared, g.cleared, g.expired, g.currently_gross, g.max_concurrent);
        log->info("Network (gross-LOS screen): the RESPONSE moved {} carried level entry(ies) "
                  "(erase + pending-reseed), {} per-LOS iono carry entry(ies), and folded {} "
                  "CODE row(s) at σ × {:g}.  Nothing here withheld an epoch — the screen is "
                  "adapt-and-emit by construction.",
                  g.amb_refloated, g.iono_erased, g.rows_deweighted,
                  config_.getNetworkConfig().gross_los_deweight);
        log->info("Network (gross-LOS screen): of those, {} were leveling-carry entries erased "
                  "(retirement census family [C], the same one-solve refloat the T4 attribution "
                  "erase performs — the arc stays tracked and the next harvest re-stashes it).",
                  ctx.counters.n_cr_gross_los);
        // --- Per-rule readout (docs/refound/RF-3_code_row_qc_2026-09-22.md §4 B5,
        //     requirement F6; RF-3b).  Every line prints in both states of the sub-knob
        //     it reports on.  The populations a rule targets (B1's short-row count over
        //     the bar, B2's largest folded |v|, releases and expiries) are counted in both
        //     states, so an arm that leaves a rule off still states the population it
        //     would have reached (invariant #4).  The counts of a rule's own actions (B2
        //     holds, B3 exclusions, B4 station declarations) move only with it on.
        {
            const NetworkConfig& gnc3 = config_.getNetworkConfig();
            const bool mrr_av = gnc3.gross_los_min_rows_rule
                                    == NetworkGrossLosMinRowsRule::Available;
            log->info("Network (gross-LOS, RF-3b B1 min_rows rule = {}): {} LOS-epoch(s) "
                      "cleared the AVAILABLE bar while the line of sight had FEWER than {} "
                      "code rows.  That population is UNREACHABLE under the fixed rule and "
                      "is exactly where four of the six largest episodes of the RF-3 read "
                      "live (a line of sight whose station has lost every signal but one "
                      "band-1 code).  {} of the run's {} declaration(s) were taken at a "
                      "reduced bar.",
                      mrr_av ? "available" : "fixed", g.los_over_short,
                      gnc3.gross_los_min_rows,
                      mrr_av ? "Some" : "None", g.declared);
            log->log((g.expired > 0) ? spdlog::level::warn : spdlog::level::info,
                     "Network (gross-LOS, RF-3b B2 bound = {}): {} release(s) against {} "
                     "expiry(ies) — RELEASE is the intended exit and EXPIRY the exception, "
                     "because an expiry means the clear test could never fire.  The largest "
                     "|v| any FOLDED row reached was {:.3f} m (the F2 alarm: 445.48 m on "
                     "runs/_rap_rf3_gl1 with the bound off).  The hold froze {} carried "
                     "level(s) and {} iono carr(ies) and was re-applied over {} "
                     "LOS-epoch(s).",
                     gnc3.gross_los_bound ? "on" : "off", g.cleared, g.expired,
                     g.folded_v_abs_max_m, g.bound_amb_held, g.bound_iono_held,
                     g.bound_los_epochs);
            log->info("Network (gross-LOS, RF-3b B3 emit_exclude = {}): {} satellite-epoch(s) "
                      "lost a declared line of sight from their contributor set at the "
                      "DECLARING epoch — {} withheld for want of {} contributing stations, "
                      "{} emitted with a per-satellite discontinuity mark.  ⚠ The emitted "
                      "clock and code bias are joint-solve states, so the VALUE is not "
                      "re-solved without the line of sight; what lands on the declaring "
                      "epoch is the contributor-set exclusion and its declaration.",
                      gnc3.gross_los_emit_exclude ? "on" : "off", g.emit_excluded,
                      g.emit_withheld, gnc3.gross_los_sat_min_stations,
                      g.emit_disc_marked);
            log->info("Network (gross-LOS, RF-3b B4 station_tier = {}): {} STATION "
                      "declaration(s), {} release(s), {} station-epoch(s) spent declared, "
                      "against {} whole-station guard firing(s).  With the tier off a guard "
                      "firing only defers: the next epoch declares the same lines of sight "
                      "one by one on the same evidence.",
                      gnc3.gross_los_station_tier ? "on" : "off", g.sta_declared,
                      g.sta_released, g.sta_los_epochs, g.standdowns);
            if (g.declared > 0)
                log->info("Network (gross-LOS, RF-3b B5 latency): per-episode, the "
                          "declaration landed a median-free mean of {:.2f} screened epoch(s) "
                          "after the first row over the bar (max {}).  The wire's own "
                          "latency is {} epoch(s) by construction on this arm.",
                          static_cast<double>(g.declare_latency_sum_ep)
                              / static_cast<double>(g.declared),
                          g.declare_latency_max_ep,
                          gnc3.gross_los_emit_exclude ? 0 : 1);
            if (!g.sta_declared_by_station.empty()) {
                std::string sline;
                for (const auto& [nm, n] : g.sta_declared_by_station)
                    sline += fmt::format("{}{}={}", sline.empty() ? "" : " ", nm, n);
                log->info("Network (gross-LOS, RF-3b B4): station declarations by station: {}",
                          sline);
            }
        }
        if (g.declared > 0 && g.rows_deweighted == 0)
            log->warn("Network (gross-LOS screen): {} declaration(s) but ZERO code rows were "
                      "ever folded at the deweighted σ.  The actuator lives in "
                      "networkSplitStation_ and keys on em.signal_types / em.satellite_ids; a "
                      "zero here means the declared lines of sight never returned, or the "
                      "publish half did not reach the assembly.",
                      g.declared);
        if (g.los_screened > 0) {
            const double frac = 100.0 * static_cast<double>(g.los_over)
                              / static_cast<double>(g.los_screened);
            if (frac > 5.0)
                log->warn("Network (gross-LOS screen): {:.2f} % of LOS-epochs cleared the bar "
                          "— above the 5 % line §24.18 froze as 'the threshold is wrong'.  "
                          "Report the |v|/σ distribution over all lines of sight before "
                          "touching network.gross_los_k; do NOT tune it blind.", frac);
            else
                log->info("Network (gross-LOS screen): {:.3f} % of LOS-epochs cleared the bar "
                          "(§24.18's threshold sanity line is 5 %).", frac);
        }
        // Identity tables: which stations and satellites were declared, and where the
        // screen stood down instead (invariant #7: an edge without an identity is not an
        // event).
        auto topTable = [&](const char* what, const char* axis,
                            const std::map<std::string, long long>& m) {
            if (m.empty()) return;
            std::vector<std::pair<std::string, long long>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](const auto& a2, const auto& b2) {
                return a2.second != b2.second ? a2.second > b2.second : a2.first < b2.first;
            });
            std::string line;
            std::size_t n = 0;
            for (const auto& kv : v) {
                if (n++ >= 12) { line += " …"; break; }
                line += fmt::format("{}{}={}", line.empty() ? "" : " ", kv.first, kv.second);
            }
            log->info("Network (gross-LOS screen): {} by {} ({} distinct): {}",
                      what, axis, v.size(), line);
        };
        // --- Whole-satellite stand-down (§24.19, GL-2) -------------------------
        if (ctx.knobs.gl2_on) {
            log->info("Network (gross-LOS, GL-2 whole-SATELLITE stand-down): {} satellite "
                      "stand-down edge(s) and {} release(s) over {} epoch(s) with at least "
                      "one satellite down ({} the most simultaneously); it refused {} "
                      "per-LOS declaration(s) and RELEASED {} standing one(s), and the wire "
                      "withhold pruned {} satellite-epoch(s) from an emission.",
                      g.sat_standdowns, g.sat_released, g.sat_epochs, g.sat_epoch_max,
                      g.sat_los_suppressed, g.sat_los_released, g.sat_wire_withheld);
            if (g.sat_standdowns == 0)
                log->info("Network (gross-LOS, GL-2): ZERO OPPORTUNITY — no satellite was "
                          "ever over the {:.1f}σ bar at {} or more stations in one epoch.  "
                          "A clean window, not a demonstration: the guard's power is "
                          "unproven until a window that carries a satellite-level fault "
                          "grades it (runs/_gl_on6 at TOW 271200 carries one, G17 at ten "
                          "stations).",
                          config_.getNetworkConfig().gross_los_k,
                          config_.getNetworkConfig().gross_los_sat_min_stations);
            if (g.sat_standdowns > 0 && g.sat_wire_withheld == 0
                && config_.getNetworkConfig().gross_los_sat_wire_withhold)
                log->warn("Network (gross-LOS, GL-2): {} satellite(s) stood down but NOTHING "
                          "was ever pruned from an emission, though the wire withhold is "
                          "armed.  The prune lives in networkSolPrep and keys on "
                          "ctx.carry.gl2_wire_pending; a zero here means the stood-down "
                          "satellites were never in a solution, or the pending set did not "
                          "reach the emit seam.",
                          g.sat_standdowns);
            if (g.sat_epochs > 0) {
                const double per_hour = static_cast<double>(g.sat_standdowns)
                                      / std::max(1.0, static_cast<double>(g.epochs) / 120.0);
                if (per_hour > 2.0)
                    log->warn("Network (gross-LOS, GL-2): {:.2f} satellite stand-down(s) per "
                              "hour — above the 2/hour line §24.19 froze.  Satellite-level "
                              "faults are COMMON on this bed, and the wire's per-satellite "
                              "EMISSION POLICY (not this screen) is the next card; report "
                              "the satellites and the hours.", per_hour);
                else
                    log->info("Network (gross-LOS, GL-2): {:.2f} satellite stand-down(s) per "
                              "hour (§24.19's line is 2/hour).", per_hour);
            }
            topTable("GL-2 satellite stand-downs", "satellite", g.sat_standdown_by_sat);
        }
        topTable("declarations", "station", g.declared_by_station);
        topTable("declarations", "satellite", g.declared_by_sat);
        topTable("stand-down station-epochs", "station", g.standdown_by_station);
    }

    // --- Iono-deweight release hygiene run-end telemetry -----------------------
    // (docs/SPEC_deweight_release_hygiene.md).  At the event the spec was written
    // against, the station carrying the parked column was never deweighted, so INC-1
    // and INC-2 have no opportunity there by construction.  A run reporting zero
    // releases is therefore not evidence that the release response works or fails
    // (invariant #4).  The denominators (`epochs`, `gate_advances`, `releases_seen`) are
    // reported first, so "the seam never ran", "it ran and the gate never advanced" and
    // "the gate advanced and nothing ever released" are three distinguishable answers.
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

    // --- Flicker/gap signal integrity run-end telemetry ------------------------
    // (docs/SPEC_flicker_gap_integrity.md).  The classifier counters are the ON-path
    // telemetry (invariant #4): a run with zero classified lines of sight is dead
    // wiring, not a quiet network, and only the denominators tell the two apart.  The
    // demoted-row count is summed here from the per-station counters because the fold
    // runs inside the parallel produce phase.  A live map with zero demoted rows means
    // the map is written and never read: live counters over dead wiring, the failure
    // the rx-CB carry card documents.  The census block states the INC-0 audit answer
    // directly rather than leaving it to be inferred from two raw counts.
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
        // Audit answer (spec INC-0).  The spec asks whether an arc born dual-frequency
        // that degrades mid-arc ever continues single-frequency (a phase that cannot be
        // monitored, at full weight, with no gap registered).  The two numbers are
        // reported together and interpreted in the line, because either one alone is
        // ambiguous.
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

    // --- Prior-residual census run-end telemetry ------------------------------
    // (docs/SPEC_prior_residual_census.md §6).  The ladder is the telemetry
    // (invariant #4), and a family with n_rows = 0 has to read as "ran, saw nothing"
    // rather than "never ran".  The body is in filter/NetworkPriorResidual.cpp, next
    // to the emission that feeds it.
    if (ctx.knobs.prires_on)
        networkPriorResidualReport(ctx, config_, log, setup.stations, prires_est);

    // --- Redundancy / testability census run-end telemetry --------------------
    // (docs/SPEC_redundancy_census.md).  The census claims that the cross-flag has
    // essentially zero false-positive surface, and a reader can check that only with
    // the denominators next to the findings (invariant #4): passes, sweeps, rows,
    // uncheckable rows, and uncheckable rows that were also hot.  Zero hot cells over
    // zero swept rows says nothing about the network; zero hot cells over 400k swept
    // rows with 2k uncheckable is a real negative result.  The lines follow that order.
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
        // Prefer-fallback control.  Uncheckable rows that stay quiet are the
        // single-frequency risers the prefer-fallback directive keeps; naming the worst
        // of them makes "counted, and none was hot" a claim a reader can check.
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
        // Closure: trace(H) over the swept rows can never exceed the number of free
        // parameters they span.  A negative slack means the covariance or the row stack
        // does not describe the system that was solved, which invalidates every r_i
        // above, so a violation is logged as a warning.
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
        // The escalation path needs both numbers to be readable.  "0 escalations" alone
        // is ambiguous, and under stride 1 it is guaranteed: the scheduled sweep already
        // covers every station, so no latch ever needs acting on.  Reporting only the
        // action would claim more than the counters support (the reverse of live
        // counters over dead wiring).
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
    // (docs/SPEC_redundancy_census.md §11.4).  The aggregates have to distinguish
    // "ran, all rows checkable" from "never ran" (invariant #4); one line can report
    // both only if the denominators come first.  The body is in filter/NetworkMdb.cpp,
    // next to the aggregation that produces the numbers.
    if (ctx.knobs.mdb_on)
        networkMdbReport(ctx, config_, log);

    // --- BlockFormal LOS-block census run-end telemetry ----------------------
    // (docs/SPEC_redundancy_census.md §10.4).  "The block tier found no smearing" and
    // "there was never a block to smear across" produce identical ladders
    // (invariant #4), so the block-size histogram is reported before any occupancy,
    // and INC-3's fraction comes last, with its denominator.  The body is in
    // filter/NetworkBlockBaarda.cpp, next to the sweep that produces the numbers.
    if (ctx.knobs.bb_on)
        networkBlockBaardaReport(ctx, log, setup.stations);

    // --- Station congruence run-end telemetry --------------------------------
    // (docs/SPEC_redundancy_census.md §13.7.3).  "Every held coordinate on this
    // network is right" and "the screen never swept a station" produce identical
    // silence (invariant #4), so the denominators come first.  Then comes the MDB
    // table, a service deliverable rather than a diagnostic: the smallest coordinate
    // fault this network can detect, per station and per direction.  The body is in
    // filter/NetworkCongruence.cpp, next to the sweep that produces the numbers.
    if (ctx.knobs.cg_on)
        networkCongruenceReport(ctx, log, setup.stations);

    // --- Prefit census / fuse / seed-birth / topology / flight-recorder --------
    // (docs/SPEC_prefit_census_observability.md §10).  A tripwire that never fires and
    // a tripwire that was never connected produce identical logs (invariant #4).  Only
    // the denominators (`epochs`, `station_epochs`, `rows`, `rec_consults`) separate
    // them, so they are reported first, and every zero-finding line says which case
    // it is.
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
                // disarmed birth check" are different states and must not read the same
                // (invariant #4, applied to a filter rather than to wiring).
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

    // --- Product-referenced measurement admission run-end telemetry -----------
    // (docs/SPEC_measurement_admission.md).  The screen is absolute: it sees a class of
    // fault that every differential check passes by construction, and a reader can
    // check that from a log only with the denominators next to the findings
    // (invariant #4).  Zero rejections over zero screened channels says nothing about
    // the network; zero rejections over 400k screened channels with a worst |z| of
    // 38 m is a real negative result; two rejections at 1e5 m against that background
    // is the positive result the screen exists for.  Order of the lines: denominators,
    // census separation, response, warnings.
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
            // The split of mid-arc withholds, printed side by side rather than left for
            // the reader to compute.  On the FT63 24 h control, 16 of 17 mid-arc onsets
            // were re-entries counted as onsets; an armed run should move that
            // population into reject_reentry and leave `onsets` holding only channels
            // that went bad while continuously present.  If `onsets` is still the
            // larger of the two, the vet is not reaching the class it targets.
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
        // --- Re-entry vet: ran or never engaged (INC-4, invariant #4) ----------
        // Each of these separates "the vet ran and found nothing" from "the vet never
        // got the chance", which clean log output alone cannot distinguish.
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
    // An inert fold (`M := 0`, the fallback when Pᶻ+B fails the PD screen and is not
    // repaired) leaves the block's posterior equal to its prior; the D5 gate skips its
    // rows because Cov(v) inherits the negative diagonals, and dd_sigma_cyc and the AR
    // probe read a covariance that does not show the failure.  The INC-0 counters are
    // always on, so a carry_invariant run always gets a line here, armed or not; the
    // branch below covers the zero-fold case so that it too prints a line (SPEC §3,
    // "Run-end summary always"; invariant #4).  Keep those two branches exhaustive over
    // `carry_invariant`.
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
        // Zero-fold branch of the run-end summary (SPEC §3, "Run-end summary always").
        // The gate above is `ddfold_epochs > 0`, and that counter advances only on an
        // accepted install whose fold saw blocks (NetworkInstallCarry, under
        // `if (fs.blocks > 0)`).  Zero is reachable with no fault at all: buildCarryDdPriors
        // emits nothing when there is no shared datum or no foldable chord, and the fold
        // returns early before it tallies.  Without this branch a carry_invariant run could
        // print no INC-0 line, and d5_nonpos_var_rows (read out only in the block above)
        // would go unreported.  The per-epoch trace columns are gated on the same
        // `blocks > 0`, so they give no fallback either, and a run with no line reads as
        // "clean".
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
    // Run-level summary of the producer AR search.  The per-epoch spdlog line describes
    // one epoch; this readout describes the run, following §4.1 of the 2026-09-10 note in
    // docs/network_ambiguity_resolution_design.md ("a fix count is not a grade").
    // `netar_epochs == 0` on an armed run means the search never reached the ILS and
    // every number below is structurally empty; that case gets its own warning
    // (invariant #4).
    if (nc.ambiguity_resolution.enable) {
        auto& c = ctx.counters;
        auto p50 = [](std::vector<double> v) {
            if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        // ratio and p_succ are collected on accepted-fix epochs only (a rejected epoch's
        // IlsResult zeroes them), so on a run that never fixed the sample is empty.
        // "n/a" says so; a printed "nan" or "0.00" would read like a measured value.
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
    // Printed on every carry_invariant run, armed or not: a σ published without its
    // covariance mode cannot be read, and the run that most needs to be told its σ is
    // carry-blind is the one that did not arm network.carry_aware_covariance.
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
    // rows_built == 0 on an armed run means the wiring never fired, which is not the same
    // as "the feature had no effect" (invariant #4), so the skip tallies are reported too
    // and an all-zero build gets a warning that names the dominant cause.
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
            // Name the dominant cause: "zero rows" next to a table of ten tallies is hard
            // to read, and the three failure classes (wiring, signal availability,
            // maturity) have entirely different fixes.
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
        // --- Integer path, increments 2a and 2b (docs/SPEC_sd_ewl_prereduce.md §8) ------
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
                // Invariant #4 one level down: the row layer can work while the integer
                // layer stays inert, and the two have different fixes.  Name the dominant
                // cause rather than leaving nine tallies to read.
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
    // All-zero counters on an armed run mean a miswire, not a quiet run (invariant #4).
    // Card E's line prints whenever mw_detect is on and counts detections as well as
    // refloats, because a refloat-only count makes the funnel wiring look dead on a cold
    // run (the L3 defect).
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
    // Slip re-level run-end readout (SPEC_slip_relevel_walk §6).  Either knob armed with
    // its headline counter at 0 on a slip-bearing run is a miswire, not a quiet run
    // (invariant #4): the drains latched but the inject site never consulted them, or
    // the bridge never met a joint (MW, IF-TD) delta.
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
        // Bridge v3 riders (the GF and EWL second equations): both coupling edges at
        // run end.  gf armed with gf_captured = 0 on a slip-bearing run is a miswire, as
        // is ewl armed with both fill counters at 0 (invariant #4).
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
    // ---- Leveling-carry retirement census (SPEC_withhold_era_release §8) ----
    // Always on: counters only, no CSV row, no solve effect.  It lets a fleet-scale
    // carry loss be attributed rather than inferred.
    //
    // Reported as four families and never as one total, because the units are not
    // commensurable.  A frame skip costs one solve of continuity for an arc the same
    // epoch re-stashes; a refloat costs one solve on the next epoch; a departure is
    // permanent; and the inject-side stale skip erases nothing at all, yet it is the
    // mechanism behind the §1.3 mass re-level.  Summed, they would make a lineage with a
    // churning chain datum look like it retires an order of magnitude more carry than
    // one without, which is the inference the §7 promotion argument must not draw.
    {
        const auto& C = ctx.counters;
        const double pl_gap = config_.networkPhaseLevelingMaxGapS();
        // Armed-state disclosure, the same qualifier family [C] carries.  Families [A]
        // and [B] both live on the phase-leveling carry, and [B] also sits at an inject
        // site that carry_invariant supersedes, so on those postures a 0 means unwired,
        // not clean (invariant #4).
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
        // ON-path telemetry of the inject-side per-LOS variance scale (F1,
        // network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount; invariant
        // #4).  Printed only when armed, so a log with the knob off is byte-identical.
        // Armed on a multi-band posture with 0 rows scaled means the inject never saw two
        // carried bands on one LOS: a miswire, not a quiet run.  A histogram concentrated
        // at B_eff = 1 on a triple-frequency bed says the sibling bands were skipped
        // (freed, stale, reseed) rather than carried.
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
        // ON-path telemetry of the station-wide, jointly whitened leveling-carry block
        // (F3, network.phase_leveling_local_block, SPEC_amb_iono_split_overcount;
        // invariant #4).  Printed only when armed, so a log with the knob off is
        // byte-identical.  Armed with 0 blocks means the inject never built one: a
        // miswire (carry_conditional_coupling off, carry_coupling_joint_whiten on, or the
        // carry never warmed), not a quiet run.  A histogram below the station's
        // carried-column count says the sibling columns were marginalized out (freed,
        // stale, reseed) rather than carried; fallbacks are the station-epochs that got
        // F1 or diagonal rows instead of the block.
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
                      // ZWD in the block (B9-in-block, network.carry_zwd_in_block), printed
                      // on the F3 line because the block carries it.  0 columns under an
                      // armed knob means unwired, not clean (invariant #4); a large fallback
                      // count says the ZWD kept taking the diagonal B9 row the block
                      // replaces (n_pl_blk_fallback above splits those into non-SPD blocks
                      // and station-epochs with no block at all).
                      C.n_pl_blk_zwd_cols, C.n_pl_blk_zwd_fallback_diag,
                      C.n_pl_blk_zwd_missing_cross, C.n_raim_pool_zwd_rows,
                      config_.networkCarryZwdInBlock()
                          ? (C.n_pl_blk_zwd_cols == 0
                                 ? "  ⚠ 0 ZWD columns under an ARMED carry_zwd_in_block — "
                                   "the channel is UNWIRED (no station-epoch reached the "
                                   "block with a live ZWD carry), not quiet."
                                 : "")
                          : " (knob OFF — these are structurally 0)",
                      // Iono in the block (I-in-block, network.carry_iono_in_block), on the
                      // F3 line because the block carries it.  0 columns under an armed knob
                      // means unwired, not clean (invariant #4); a large not-folded count
                      // says the station blocks kept failing to factorize, and with no
                      // diagonal iono row the iono carry then contributes nothing there.
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
                      // Receiver clock in the block (RX-CLK-in-block,
                      // network.carry_rx_clock_in_block), on the F3 line because the block
                      // carries it.  0 columns under an armed knob means unwired, not clean
                      // (invariant #4).  `refused` is the clock-jump census and the only
                      // jump detector the network's receiver clock has: 0 refusals over a
                      // field day means either a well-behaved receiver fleet or a gate set
                      // too loose, and a large count means the carry is rebuilt constantly
                      // (the drift model is not tracking).
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
        // The jump gate's scale in metres, printed only with network.carry_rx_clock_in_block
        // armed (with it off the log is byte-identical).  `refused` above is a count, and
        // k alone does not say whether a given event is above or below the gate.  var_pred
        // is per station-epoch (harvested variance + drift extrapolation + q), so the gate
        // is a distribution, reported as mean, min and max.  The largest |innovation| is
        // the largest step the gate saw; on a quiet bed with no injected event it is the
        // noise ceiling, and an injected rx_clock_step must clear the gate at its own
        // station to be refused.
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
        // ON-path telemetry of the age-aware young-arc inflate
        // (network.phase_leveling_young_inflate_var_m2, SPEC_amb_iono_split_overcount
        // §6.17–§6.18; invariant #4).  Printed only when armed, so a log with the knob off
        // is byte-identical.  0 inflated column-epochs under an armed knob means armed and
        // inert (the stamps never reached an inject), not a quiet run.  A mean age near
        // the ceiling says the inflated population is nearly mature and the knob changes
        // little.  The stamp split (created against freed) says which event produces the
        // code-quality levels on this bed.  The block-scaled count against the inflated
        // count separates "computed" from "applied inside Σ": the difference is columns
        // that took the per-column route (F3 off, factorization fallback, young-leg gate)
        // or were refused by the block's scalar screen.
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
        // Resume frame restore and slip warm-up (§18.10): ON-path telemetry (invariant
        // #4), the identities of the re-coupling edge.  0 anchors under an armed knob
        // means the snapshot carried no frame rows (a v1 file) or no resume happened.
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
        // Coupling-regressor restore (§18.12), the re-coupling edge of the seam
        // (invariant #7): what came back, and what it bound to at the first fold.
        // Restoring (value, var) alone injects a carried leveling row as a plain diagonal
        // prior at the pure conditional variance, a combination no continuous epoch
        // produces, so the row drives the shared border by the full α‖M_d‖ at
        // α ≈ 1/cond_var ≈ 1e6 m⁻², about 90 times the Q3 anchored clock prior's
        // 1/9e-5 ≈ 1.1e4 m⁻².  This is the mechanism behind the §18.11 first-fold trips, so
        // the readout is not how many rows came back but how many of their terms resolved
        // to a live column; both edges are printed here.
        //
        // The census vectors hold one entry per row of the first post-resume harvest (the
        // measure-only observatory in networkHarvestLocalCarries), so they are reduced here
        // rather than accumulated online: nth_element is O(n), the run is over, and the
        // observatory stays byte-identical on every solve artifact.  The reduction works on
        // copies so the readout never permutes carried state.
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
        // Run-end readout of the dense-border restore (§21.14, RS-7), and of the C1 border
        // on a plain warm resume that arms neither restore.  Both edges of one instrument
        // (invariant #7): how much of the border came back, and how much of it the first
        // fold still took diagonally.  The Q3 first-fold decouple count is §18.11's trip
        // statistic, the number the restored border should bring back to a continuous
        // run's level.  It is structurally 0 on a lineage that does not arm
        // network.clock_anchor, and the line says so rather than letting a 0 read as a
        // good result (invariant #4).
        {
            const std::string cta_note =
                ctx.knobs.cta_on
                    ? std::string{}
                    : std::string("  (network.clock_anchor is OFF on this lineage — that 0 "
                                  "is structural, not a health signal.)");
            if (config_.networkResumeBorderRestore())
                log->info("Network (RS-7 border restore, dense border): {} dense-carry key(s) "
                          "restored from the snapshot's QK/QP records ({} dropped: no "
                          "restored D row) against {} carried key(s) at the seam; {} border "
                          "key(s) STILL folded DIAGONALLY at the first warm fold.  First "
                          "post-resume epoch: {} Q3 clock-anchor decouple(s).{}{}",
                          C.n_resume_cpl_cov_keys, C.n_resume_cpl_cov_dropped,
                          C.n_resume_dense_carry_keys, C.n_resume_cpl_diag_border,
                          C.n_resume_first_fold_cta_trips, cta_note,
                          C.n_resume_cpl_cov_keys == 0
                              ? "  ⚠ 0 keys restored under an ARMED border restore — the "
                                "snapshot carried no usable QK/QP records (a writer without "
                                "network.state_snapshot_coupling_capture), or no warm resume "
                                "happened; the border re-seeded from the first emitted epoch "
                                "exactly as before."
                              : "");
            else if (C.n_resume_dense_carry_keys > 0
                     && !config_.networkResumeCouplingRestore())
                log->info("Network (C1 dense border, warm resume): {} carried key(s) came "
                          "back with VALUES and per-key variances only — {} of them folded "
                          "DIAGONALLY at the first warm fold, because neither "
                          "network.resume_border_restore nor network.resume_coupling_restore "
                          "is armed, so the block's cross-covariance re-seeds from the first "
                          "emitted epoch.  First post-resume epoch: {} Q3 clock-anchor "
                          "decouple(s).{}",
                          C.n_resume_dense_carry_keys, C.n_resume_cpl_diag_border,
                          C.n_resume_first_fold_cta_trips, cta_note);
        }
        // Re-admission continuity (E2 seed-first re-admission and the block re-coupling
        // edge), both edges printed (invariant #7): parked is what left, seeded is what came
        // back, moved and outlier are what the data rejected, and the coupling window's
        // diverted-term count is the block edge's denominator.  0 seeds under an armed
        // horizon with parks > 0 means inert on this bed, and the line says so
        // (invariant #4).
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
        // Reference identity of the DD trace under either policy (§6.12.5).  Printed whenever
        // the trace ran, including under the `lowest` default: reference switches occur under
        // both policies, and a grade built on a single reference element has to state that
        // element's behaviour (SPEC_synthetic §13.8).  `age_win` is the invariant-#4
        // denominator for `oldest`: 0 means the policy never elected a satellite `lowest`
        // would not have, so the arm is inert and its A/B proves nothing.
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
        // Seam-scoped ZWD re-seed (network.zwd_seam_reseed): ON-path telemetry
        // (invariant #4).  An armed knob with 0 folds means the window never armed (no
        // warm resume, or the snapshot carried no Z rows) or every fold was skipped
        // (stale entries, carry_warm cold); the resume-time ARMED/INERT log line says
        // which.
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
    // Reads the per-station wind-up counters directly, because windup_trace.csv is
    // written only from Application::run (one caller, Application.cpp), which
    // runNetwork never reaches.  Read-only: arming it cannot change a byte of the
    // solve.
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
        // The exposure sentence holds only when the leveling carry outlives the
        // accumulator.  At the loader defaults it does not (120 s against 300 s), and
        // printing the sentence anyway would let a structural zero read as a clean
        // result (invariant #4).
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
    // Wind-up restart response readout (§10.4), the wired invariant-#1 response; its
    // counters are the ON-path telemetry (invariant #4).
    if (config_.networkCycleSlipWindupRestartRefloat()) {
        const auto& qcc = slip_qc.counters;
        const long long closure = qcc.wu_restart_refloated + qcc.wu_restart_no_arc
                                + qcc.wu_restart_cold + qcc.wu_restart_beyond_carry
                                + qcc.wu_restart_stale;
        log->info("Network (wind-up restart response, SPEC_phase_windup_sign §10.4): "
                  "{} restart event(s) drained → {} RESPONDED ({} arc(s) refloated, {} "
                  "of them re-entering arcs reached through the leveling carry), {} "
                  "inside the carry horizon with NO arc to free (no live column, no "
                  "carry entry), {} cold (diagnostic), {} beyond the carry horizon "
                  "(carry died with the gap — no response owed), {} STALE refused "
                  "(ledger from an earlier epoch — never respond to history).  "
                  "CLOSURE {} vs events {} ({}).  0 arcs refloated on an accumulated "
                  "run with in-horizon restarts = dead wiring (invariant #4; card "
                  "WUP); the lane observatory grades this response under cause=WU.",
                  qcc.wu_restart_events, qcc.wu_restart_refloated,
                  qcc.wu_restart_arcs, qcc.wu_restart_carry_arcs,
                  qcc.wu_restart_no_arc, qcc.wu_restart_cold,
                  qcc.wu_restart_beyond_carry, qcc.wu_restart_stale,
                  closure, qcc.wu_restart_events,
                  closure == qcc.wu_restart_events ? "OK" : "*** MISWIRE ***");
        if (qcc.wu_restart_arcs == 0
            && qcc.wu_restart_refloated + qcc.wu_restart_no_arc > 0)
            log->warn("Network (wind-up restart response): {} restart(s) inside the carry "
                      "horizon and ZERO arcs refloated — the response did not act on this "
                      "run (invariant #4).",
                      qcc.wu_restart_refloated + qcc.wu_restart_no_arc);
    }

    // ---- Anchor-slip handover (docs/SPEC_event_harness.md §12.7) ------------
    // These counters are the ON-path telemetry (invariant #4).  A slip that lands on a
    // gauge arc is a different event from one that lands anywhere else, so an armed run
    // reporting 0 staged on a lineage whose detectors fired has not shown the network
    // is quiet; it has shown nothing.
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

    // ---- MW detect-only admission (network.cycle_slip.mw_evidence_refloat) --
    // Printed on every run, armed or not: this line is the readout of the drop
    // populations, and the would-admit count sizes the knob on a lineage without
    // running it armed.  A run with the knob off reporting would-admit 0 says the knob
    // is inert there; a run with it on reporting evidence 0 with would-admit > 0 on the
    // same leg is dead wiring (invariant #4).
    {
        const auto& C = ctx.counters;
        log->info("Network (MW detect-only admission): legacy {}, evidence {} "
                  "[knob {}; would-admit {}]; dropped loose {} / unknown {}.",
                  C.n_mw_admit_legacy, C.n_mw_evidence_admitted,
                  config_.networkCycleSlipMwEvidenceRefloat() ? "on" : "off",
                  C.n_mw_evidence_would_admit, C.n_mw_drop_loose,
                  C.n_mw_drop_unknown);
    }

    // ---- MW pair scope (network.cycle_slip.mw_detect_pairs, card L5P) --------
    // Printed only when the scope is armed; the counter is structurally 0 under `all`.
    // The count is the ON-path telemetry (invariant #4), and an armed run reporting 0
    // on a three-band lineage means the wiring is inert, not that the detector is quiet.
    if (config_.networkCycleSlipMwDetectPairs() == NetworkMwDetectPairs::Primary) {
        const auto& C = ctx.counters;
        log->info("Network (card L5P, MW pair scope = primary): {} detector row(s) NOT "
                  "formed because the pair carried a band outside the constellation's "
                  "ionosphere-free pin pair.  Those bands keep every other slip response "
                  "they had (LLI, gap re-entry, IF-TD, the band scope) — only the "
                  "wide-lane pair is gone.", C.n_mw_pair_scope_skipped);
        if (C.n_mw_pair_scope_skipped == 0)
            log->warn("Network (card L5P, MW pair scope): INERT — the scope was armed and "
                      "refused NOTHING.  Expected on a two-band lineage (there is only the "
                      "primary pair); on a three-band one it means the detector produced no "
                      "extra-band pair at all, so check mw_detect and the pin pair before "
                      "reading this arm as a scoped one.");
    }

    // ---- Extra-band code deweight (network.extra_band_code_sigma_scale, card L5P)
    // Same contract.  The fold runs inside the parallel produce phase, so its row
    // counts are kept per station (as gl_rows_deweighted is); they are summed here,
    // once, before the block reads them.
    if (config_.networkExtraBandCodeSigmaScale() > 1.0) {
        auto& C = ctx.counters;
        C.n_eb_rows_deweighted = 0;
        C.n_eb_rows_no_pin     = 0;
        for (const auto& st : stations) {
            C.n_eb_rows_deweighted += st.eb_rows_deweighted;
            C.n_eb_rows_no_pin     += st.eb_rows_no_pin;
        }
        const double s = config_.networkExtraBandCodeSigmaScale();
        log->info("Network (card L5P, extra-band code sigma x {:.3f}): {} code row(s) folded "
                  "at the scaled sigma (information / {:.3f}); {} code row(s) left unscaled "
                  "because their constellation has no resolvable ionosphere-free pin pair.",
                  s, C.n_eb_rows_deweighted, s * s, C.n_eb_rows_no_pin);
        if (C.n_eb_rows_deweighted == 0)
            log->warn("Network (card L5P, extra-band code sigma): INERT — the scale was armed "
                      "and NO code row was ever extra-band.  Expected on a two-band lineage; "
                      "on a three-band one check the pin pair (a constellation with no "
                      "clock-reference entry answers 'nothing is extra-band' by design).");
    }

    // ---- Second code channel (network.second_code_channel, card SCC) ---------
    // (docs/refound/SCC_second_code_channel_2026-09-21.md, R7.)  Run-end line, printed
    // only when armed.  No other output records what the channel did: a companion code
    // row looks like a canonical one in every trace and on the wire, and the effect is
    // which satellite columns reach the two-station registration bar, so the run states
    // it here (invariant #4).  The row counts accumulate inside the parallel produce
    // phase and are kept per station (as eb_rows_deweighted is); they are summed here,
    // before the block reads them.
    if (config_.networkSecondCodeRowsActive()) {
        auto& C = ctx.counters.second_code;
        C.rows          = 0;
        C.fallback_rows = 0;
        for (const auto& st : stations) {
            C.rows          += st.scc_rows;
            C.fallback_rows += st.scc_fallback_rows;
        }
        if (C.rows == 0 && C.cells == 0) {
            log->warn("Network (SECOND CODE CHANNEL, card SCC): ARMED but NO companion code "
                      "row was ever added — no station reported a second allow-listed band-1 "
                      "code besides its latched canonical one, so this run exercised nothing "
                      "and is byte-equivalent to the unarmed posture (standing invariant #4).  "
                      "A single-band-1-code bed (every simulated file today) reads this way by "
                      "construction; on the field roster it means the allow-list names codes "
                      "the stations do not track.");
        } else {
            log->info("Network (SECOND CODE CHANNEL, card SCC): {} companion cell-epoch(s) over "
                      "{} opening(s) and {} closing(s) (standing invariant #7's two edges), "
                      "folding {} companion code row(s) at sigma x {:.3f}; {} row(s) took the "
                      "FALLBACK path (an allow-listed band-1 code with no latched canonical "
                      "beside it — a finding about the latch, not about this card).  Receiver "
                      "inter-signal states alive at the busiest epoch: {}, of which {} "
                      "RESEED(s) over the run — a reseed is a state a station had held before, "
                      "did not hold last epoch and holds again, i.e. one the receiver "
                      "code-bias carry did not restore, so it re-determined itself from one "
                      "epoch of code.  {} (satellite, signal) column-epoch(s) reached the "
                      "two-station registration bar ONLY through companions: that number is "
                      "the card's thesis, and a zero means the channel added rows without "
                      "ever adding a column the wire did not already carry.",
                      C.cells, C.cell_open, C.cell_close, C.rows,
                      config_.networkSecondCodeSigmaScale(), C.fallback_rows,
                      C.isc_states_max, C.isc_reseeds, C.cols_via_companion);
            if (C.isc_states_max == 0)
                log->warn("Network (SECOND CODE CHANNEL, card SCC): companion rows were folded "
                          "but NO receiver inter-signal state ever existed — every companion "
                          "row therefore ASSERTED a zero receiver inter-signal bias and pushed "
                          "the difference into the satellite column, which is the defect card "
                          "B5D measured at metres on a third band.  Check that "
                          "estimate_ifb / the network satellite code bias are on.");
            if (C.cols_via_companion == 0)
                log->warn("Network (SECOND CODE CHANNEL, card SCC): NO (satellite, signal) "
                          "column reached the two-station bar only through companions — the "
                          "channel added rows to columns the wire already carried and widened "
                          "no family's roster.");
        }
    }

    // ---- Second phase channel (network.second_phase_channel, SID phase 6a) ------
    // (docs/refound/SID_signal_identity_2026-09-22.md section 4, R7.)  Run-end line,
    // printed only when armed, for the same reason as the SCC line: a second-phase row
    // looks like the latched one in every trace and on the wire, and the effect is
    // which satellite families reach the registration bar, so the run states it here
    // (invariant #4).  The row count accumulates inside the parallel produce phase and
    // is kept per station; it is summed here, before the block reads it.
    if (config_.networkSecondPhaseRowsActive()) {
        auto& C = ctx.counters.second_phase;
        C.rows = 0;
        for (const auto& st : stations) C.rows += st.spc_rows;
        if (C.rows == 0 && C.cells == 0) {
            log->warn("Network (SECOND PHASE CHANNEL, card SID phase 6a): ARMED but NO "
                      "second-phase row was ever added — no station reported a second "
                      "allow-listed band-1 phase besides its latched canonical one, so "
                      "this run exercised nothing and is byte-equivalent to the unarmed "
                      "posture (standing invariant #4).  A single-band-1-phase bed (every "
                      "simulated file as delivered) reads this way by construction; on the "
                      "field roster it means the allow-list names phases the stations do "
                      "not track.");
        } else {
            log->info("Network (SECOND PHASE CHANNEL, card SID phase 6a): {} second-phase "
                      "cell-epoch(s) over {} opening(s) and {} closing(s) (standing "
                      "invariant #7's two edges) at {} distinct arc(s), folding {} phase "
                      "row(s) at sigma x {:.3f}; peak {} cell(s) alive at {} station(s) in "
                      "one epoch.  {} row(s) took the FALLBACK path (an allow-listed "
                      "band-1 phase reported with no latched phase of that band beside it "
                      "— NOT added, and a finding about the latch rather than about this "
                      "card).  {} (satellite, signal) column-epoch(s) reached the "
                      "two-station registration bar ONLY through second phases: that "
                      "number is the card's thesis, and a zero means the channel added "
                      "rows without ever widening a family's roster.  Phase-datum anchor "
                      "incumbency-epochs seated ON a second-phase arc: {}.",
                      C.cells, C.cell_open, C.cell_close, C.arcs_born, C.rows,
                      config_.networkSecondPhaseSigmaScale(), C.arcs_live_max,
                      C.stations_max, C.fallback_rows, C.cols_via_second, C.anchors);
            // A second-phase arc needs a wired slip response beyond the post-fit
            // attribution (invariant #1; R4), and this line says whether it has one.  A
            // tier reading zero here while arcs exist does not screen second-phase rows.
            log->info("Network (SECOND PHASE CHANNEL, card SID phase 6a): slip census on "
                      "second-phase arcs — {} arc-epoch(s) freed by any tier: MW {}, LLI "
                      "{}, IF-TD {}, T4 attribution {}, T3 consensus {}, wind-up restart "
                      "{}, gap re-entry {}.  A tier at zero with arcs alive does NOT screen "
                      "these rows (standing invariant #1); the row-level gap tier keys on "
                      "the tracker's ONE signal per band, so the per-signal tier "
                      "(network.cycle_slip.gap_reentry_per_signal, {}) is the only "
                      "row-level gap response a second phase has.",
                      C.slip_arcs, C.slip_mw, C.slip_lli, C.slip_iftd, C.slip_t4,
                      C.slip_t3, C.slip_windup, C.slip_gap,
                      config_.getNetworkConfig().slip_qc.gap_reentry_per_signal
                          ? "armed" : "NOT armed");
            if (C.slip_arcs == 0)
                log->warn("Network (SECOND PHASE CHANNEL, card SID phase 6a): second-phase "
                          "arcs exist and NO tier ever freed one.  Either the bed is quiet "
                          "or every row-level response is blind to them; check the per-tier "
                          "counts against the same tiers' totals for the latched arcs.");
            if (config_.getNetworkConfig().second_phase_warn_no_per_signal)
                log->warn("Network (SECOND PHASE CHANNEL, card SID phase 6a): this run was "
                          "armed WITHOUT network.cycle_slip.gap_reentry_per_signal, so the "
                          "only response a second-phase arc's re-entry had is the post-fit "
                          "T4 attribution (the load-time warning, restated here).");
            if (config_.getNetworkConfig().second_phase_warn_iftd_pairs)
                log->warn("Network (SECOND PHASE CHANNEL, card SID phase 6a): this run was "
                          "armed with the IF-TD tier on and if_phase_td_pairs NOT 'all', so "
                          "that tier's single pair was degenerate for every satellite with "
                          "two band-1 phases (the load-time warning, restated here).");
            // Between-channel witness (R8): observatory only, no action taken.
            log->info("Network (SECOND PHASE CHANNEL, card SID phase 6a, R8): the "
                      "between-channel WITNESS — {} consecutive-epoch pair(s) of the two "
                      "band-1 phases compared, {} with a change above 0.5 cycle(s); largest "
                      "{:.3f} cycle(s) at {}.  A slip on ONE channel is an integer here and "
                      "a slip common to both cancels, so this counts the exact-integer "
                      "single-channel events the latched arc alone cannot see.  Measure "
                      "only: nothing acts on it.",
                      C.witness_pairs, C.witness_jumps, C.witness_max,
                      C.witness_max_id.empty() ? std::string("(none)") : C.witness_max_id);
        }
    }

    // ---- Common-band scope (network.cycle_slip.common_band_scope, §14) ------
    // Printed on every run, armed or not, for the same reason as the MW detect-only
    // admission line: the would-narrow count sizes the knob on a lineage without
    // running it armed, and an armed run reporting narrowed 0 while verdicts > 0 is
    // dead wiring, not a quiet network (invariant #4).  `bands kept` is the main
    // number: under the all-band union every live band of a fired arc would have been
    // freed.
    {
        const auto& C = ctx.counters;
        const double ep = static_cast<double>(std::max<long long>(C.n_cb_split_epochs, 1));
        // Refusals of the reference-arc clause (§22), with the per-chain breakdown
        // inline: each refusal costs one dead zone per chain whose reference the arc
        // is, so the total alone cannot say which chains bore the cost.
        std::string ref_arc_by;
        for (const auto& [pr, n] : C.n_cb_iftd_ref_arc_by_pair)
            ref_arc_by += fmt::format("{}{}: {}", ref_arc_by.empty() ? "" : ", ", pr, n);
        log->info("Network (common-band scope): verdicts {}, narrowed {} (bands kept "
                  "{}), all-band {} (no exoneration {} / inconsistent {} / anchor "
                  "co-locate {}), IF-TD vetoes {}, ref-arc refusals {}{}, "
                  "immature pairs {}, LLI shadows {}, "
                  "LLI-epoch verdicts {} ({} narrowed, {} anchor co-locate), "
                  "weak silences {}; "
                  "split-referenced satellites mean {:.1f} / max {} of {:.1f} anchored "
                  "[knob {}{}].",
                  C.n_cb_verdicts, C.n_cb_narrowed, C.n_cb_bands_kept,
                  C.n_cb_all_no_exoneration + C.n_cb_all_inconsistent
                      + C.n_cb_anchor_colocate,
                  C.n_cb_all_no_exoneration, C.n_cb_all_inconsistent,
                  C.n_cb_anchor_colocate,
                  C.n_cb_iftd_veto,
                  C.n_cb_iftd_ref_arc,
                  ref_arc_by.empty() ? std::string{} : fmt::format(" [{}]", ref_arc_by),
                  C.n_cb_immature_pairs, C.n_cb_lli_shadow,
                  C.n_cb_lli_verdicts, C.n_cb_lli_narrowed,
                  C.n_cb_lli_anchor_colocate,
                  C.n_cb_weak_silence,
                  static_cast<double>(C.n_cb_split_sats_sum) / ep,
                  C.n_cb_split_sats_max,
                  static_cast<double>(C.n_cb_split_multi_sum) / ep,
                  config_.networkCycleSlipCommonBandScope() ? "on" : "off",
                  config_.networkCycleSlipCommonBandScope()
                      ? std::string{}
                      : fmt::format("; would-narrow {}", C.n_cb_would_narrow));
        // Attribute the vetoes to the IF-TD pair kind that made them (§14.10).  The
        // expectation is that the vetoes come from pairs the single-pair detector never
        // watched, and the total alone cannot show that.
        if (!C.n_cb_iftd_veto_by_pair.empty()) {
            std::string by;
            for (const auto& [pr, n] : C.n_cb_iftd_veto_by_pair)
                by += fmt::format("{}{} {}", by.empty() ? "" : ", ", pr, n);
            log->info("Network (common-band scope): IF-TD vetoes by pair — {}.", by);
        }
    }

    // ---- IF-TD pair width (network.cycle_slip.if_phase_td_pairs, §14.10) -----
    // The per-pair-kind tally is the ON-path telemetry (invariant #4).  Printed in
    // either mode, because the `first` line sizes the `all` mode before it is armed:
    // it names the one pair the detector watches and how often that pair fires, so the
    // pairs missing from the list are the unwatched ones.  Under `all`, a pair kind
    // with evaluated >> 0 and flagged 0 over a slipped bed is dead wiring, not a quiet
    // lane.
    if (config_.networkCycleSlipIfPhaseTd()) {
        const auto& C = ctx.counters;
        if (C.n_iftd_pair.empty()) {
            log->info("Network (IF-TD pairs [{}]): no pair was ever evaluated — the "
                      "detector produced no valid double difference all run.",
                      config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All
                          ? "all" : "first");
        } else {
            std::string by;
            long long ev_t = 0, fl_t = 0, rf_t = 0;
            for (const auto& [pr, t] : C.n_iftd_pair) {
                by += fmt::format("{}{} eval {} / flag {} / refloat {}",
                                  by.empty() ? "" : ", ", pr,
                                  t.evaluated, t.flagged, t.refloated);
                ev_t += t.evaluated; fl_t += t.flagged; rf_t += t.refloated;
            }
            log->info("Network (IF-TD pairs [{}]): {} pair kind(s), eval {} / flag {} / "
                      "refloat {} total — {}.",
                      config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All
                          ? "all" : "first",
                      C.n_iftd_pair.size(), ev_t, fl_t, rf_t, by);
        }
    }

    // ---- Band-scoped responses (docs/SPEC_event_harness.md §Z) --------------
    // These counters are the ON-path telemetry (invariant #4).  Either knob armed with
    // its headline count at 0 on a run whose T4 tier fires (or whose whole-satellite
    // detectors fire) is dead wiring, not a quiet run: the response population is large
    // (about 21 T4 arc erases per station-hour on the slipped bed).
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

    // ---- Carry-domain lane observatory, rung 3 v3 (SPEC_lane_slip_response §7.3)
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
        // Band-scoped subset histogram (the vertex-wide one is the bin-wise difference
        // against the line above) and the freeing-cause split (§7.6(b) and (c)).  The
        // cause counts are bit counts: a pair freed by two detectors lands in two of
        // them, so they sum to at least the measured count.
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
        // T4-side census (§7.6(d)): the same delta on the RAIM attribution/consensus
        // erase channel, which carries 97.3 % of field freeing and is structurally
        // invisible to free_ambs.  Read it apart from the lane-cost grade: dMW ≈ 0 there,
        // so a large delta is carry error the refloat removed (the response working, not
        // damage it did).  0 captured on a run with nonzero n_cr_raim_attrib/consensus is
        // dead wiring (invariant #4).
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

    // Lane snap (SPEC_lane_slip_response rung 3): the v1/v2/v2.1 seed-site readout was
    // removed together with its mechanism (see the seed-site lane snap banner in
    // MeasurementModel::ensureRequiredStates).  Its counters described a write that is
    // an exact solve no-op, so a nonzero `snapped` never showed an effect.  Rung 3 v3
    // measures the lane in the carry domain and reports in the observatory block above.
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
    // Slant-iono carry (queue item 8b): run-end ON-path telemetry at both coupling
    // edges (invariants #4 and #7).  Armed with injected = 0 on a slip-bearing run is a
    // miswire.  refused_datum is the declared-zero open valve: the code-datum
    // re-realization edge has no response yet (see the spec's valve inventory entry).
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
    // Withhold-era release policy (SPEC_withhold_era_release §3): era telemetry at both
    // edges (invariant #7).  An armed run whose eras never enter or exit, or whose
    // escalation counters stay 0 through a known degraded window, is a miswire
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
    // Run-end declaration telemetry for R2/R4 (queue item 4, SPEC_t3_null_derivation §6).
    // An armed R2 run with n_staf_epochs = 0 is a miswire (invariant #4): the seam never
    // produced a factor (possible causes: RAIM off, rowRedundancy throwing fleet-wide).
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
    // Level-authority census run-end readout (LV-6).  An armed run with 0 census epochs
    // past warmup is a miswire, not a quiet day (invariant #4).
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
    // Datum-station slip detect (§9.7.10): run-end ON-path telemetry.  An armed run with
    // injected datum-station slips reporting 0/0 is a miswire, not a quiet day
    // (invariant #4).  Counted separately from Card E: those are non-pivot detections
    // answered by a local refloat; these are at the gauge-pinned datum station and are
    // answered by a network re-reference.
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
        // Per-run top-offenders line, so the audit's station list (STK2, ISHI, MCIL,
        // CCJ2) can be reproduced from the log alone, without the CSV traces (M4).
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
    // ---- Sim-only oracle ambiguity pin (include/filter/NetworkOraclePin.hpp) ----
    // The injected-row count is the only way to tell a working pin from dead wiring
    // (invariant #4).  A run reporting 0 has not measured a ceiling on what integer
    // information buys; it has measured an unpinned run, and the line says so in the
    // log so the run is not graded as the former.
    networkOraclePinRunEnd(ctx.carry.oracle_pin.get(), log);
    // ---- Sim-only oracle slip response (include/slip/NetworkSlipOracle.hpp) ----
    // The actuation count (applied, or fa_kept under true_slip 'free'; see below) is
    // the only way to tell a working oracle from dead wiring (invariant #4).  The
    // pre-registered P1 is applied > 1500 on a 6 h campaign arm; a run that reports 0
    // has not measured a ceiling, it has measured nothing, and the line says so in the
    // log.
    if (ctx.carry.slip_oracle) {
        const auto& o = *ctx.carry.slip_oracle;
        const bool ts_free = (o.true_slip == NetworkSlipOracle::TrueSlip::Free);
        const bool fa_free = (o.false_alarm == NetworkSlipOracle::FalseAlarm::Free);
        // The actuation counter depends on the policy pair.  With true_slip 'free'
        // nothing is ever repaired by construction, so keying inertness on `applied`
        // would fire the dead-wiring warning on a correctly working false-alarm
        // isolation arm; that arm's own actuation is the keep (fa_kept).
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
    if (nc.slip_qc.gap_instrument || nc.slip_qc.gap_reentry
        || nc.slip_qc.gap_reentry_per_signal) {
        const auto& qcc = slip_qc.counters;
        // ---- Response clause (network.cycle_slip.gap_reentry_relevel) ----
        // Appended to the Card-G line rather than logged separately, because the G3
        // rule is read off the SHIFTED→bridged number in the same sentence: with the
        // response armed that number is the residual, and the reader needs to see at
        // once whether it is small because nothing re-entered or because the response
        // acted.  An armed response whose freed counts are 0 on a run with resumptions
        // is miswired, not quiet, and the clause says so in the log (invariant #4).
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
        // The two counting levels are labelled at every number (H2, spec-followups N3).
        // Resumptions are per (station, satellite) re-entry; arcs are per (station,
        // satellite, signal).  A dual-band satellite returning once is 1 resumption and
        // 2 independently bridgeable arcs, which is why the arc totals exceed the
        // resumption totals.  Both arc ledgers are closed (bridged + fresh) so no reader
        // has to infer a missing cell.
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
        // ---- Tracker key against the carried key (card GRK) -----------------
        // Reported in both states of network.cycle_slip.tracker_modeled_first.  Unarmed,
        // gap_reentry_unreachable is the share of gap_reentry_no_carry that had a carry
        // the response could not reach, so a nonzero count means a wired response with a
        // gap in its coverage rather than a quiet run, and the line is a warning naming
        // the knob that closes it.  Armed, both counters are 0 by construction and the
        // line says so, which makes the knob's effect readable from the run end alone.
        {
            const std::string grk_line = fmt::format(
                "Network (slip-QC card GRK / tracker key): {} cell(s) [unit: (station, "
                "satellite, epoch, band)] where the arc-level tracker's band signal "
                "DIFFERS from the phase this station LATCHED on that band, and {} "
                "ROW-LEVEL re-entry arc(s) [unit: (station, satellite, signal) of a "
                "whole-satellite return] of the {} counted gap_reentry_no_carry that were "
                "UNREACHABLE (a leveling carry for the same station and satellite existed "
                "on another signal of the SAME band, still inside "
                "phase_leveling_max_gap_s, AND that signal is TRACKED this epoch with its "
                "matching code, so the PASS-B inject folds it across the break while the "
                "response looks it up under the tracker's signal and finds nothing).  Two "
                "populations are deliberately OUTSIDE it: a carried signal simply absent "
                "from the record this epoch has no row here at all, and the per-signal "
                "tier keys on the returning SIGNAL, where a key mismatch cannot occur.  "
                "network.cycle_slip.tracker_modeled_first is {}.",
                qcc.gap_tracker_key_mismatch, qcc.gap_reentry_unreachable,
                qcc.gap_reentry_no_carry,
                nc.slip_qc.tracker_modeled_first ? "ON" : "OFF");
            if (!nc.slip_qc.tracker_modeled_first && qcc.gap_reentry_unreachable > 0)
                log->warn("{}  ARM network.cycle_slip.tracker_modeled_first to close it: "
                          "the tracker then names each band by the LATCHED signal, which is "
                          "the key the carry uses.", grk_line);
            else
                log->info("{}", grk_line);
        }
        // ---- Ledger-driven gap re-entry population (SID phase 3) -------------
        // Printed only when the knob is armed: unarmed, every counter here is zero by
        // construction and a line of zeros would read as a measurement.
        if (slip_qc.params.ledger_reentry) {
            const long long disp = qcc.ledger_reentry_freed + qcc.ledger_reentry_bridged
                                 + qcc.ledger_reentry_no_carry
                                 + qcc.ledger_reentry_pivot_skipped;
            log->info(
                "Network (card SID phase 3 / LEDGER-DRIVEN GAP RE-ENTRY): {} arc(s) "
                "[unit: (station, satellite, signal)] in the ledger's ADMITTED "
                "Gap-to-Reentered population inside phase_leveling_max_gap_s; {} more "
                "admitted return(s) were OUTSIDE that horizon, and {} admitted phase "
                "ROW(s) over the run carried a signal the roster does not call Modelled "
                "(the roster is built upstream of computeModels, so the FIRST epoch of "
                "a constellation has no modelled role and contributes about one epoch's "
                "worth of these); both populations are left out.  "
                "VERDICT LOOKUP: {} arc(s) had one ({} from the ROW-level tier, {} from "
                "the PER-SIGNAL tier) — {} shifted, {} LLI, {} unmeasurable, {} CLEAN — "
                "and {} had NONE (the raw tiers did not see the return at all: raw "
                "Tracking while the admitted side gapped, which is the population phase "
                "1 named).  DISPOSITION ({} of {}): {} FREED, {} BRIDGED, {} reached "
                "with no carry to protect, {} skipped at the gauge-pinned pivot.  {} "
                "arc(s) returned to a RE-CREATED ambiguity column.  {} ARC arc_reentry "
                "event(s) written for the no-verdict half (holdoff-throttled).  "
                "NO-VERDICT POLICY: {}.  Card GRK's gap_reentry_unreachable reads {} — "
                "it CANNOT be fed on this path, because the response key IS the ledger "
                "arc's key.{}",
                qcc.ledger_reentry_arcs, qcc.ledger_reentry_outside_horizon,
                qcc.ledger_reentry_not_modelled,
                qcc.ledger_reentry_with_verdict, qcc.ledger_reentry_row_tier,
                qcc.ledger_reentry_sig_tier, qcc.ledger_reentry_v_shifted,
                qcc.ledger_reentry_v_lli, qcc.ledger_reentry_v_unmeas,
                qcc.ledger_reentry_v_clean, qcc.ledger_reentry_no_verdict,
                disp, qcc.ledger_reentry_arcs,
                qcc.ledger_reentry_freed, qcc.ledger_reentry_bridged,
                qcc.ledger_reentry_no_carry, qcc.ledger_reentry_pivot_skipped,
                qcc.ledger_reentry_state_recreated, qcc.ledger_reentry_events,
                slip_qc.params.ledger_reentry_refloat
                    ? (slip_qc.params.gap_reentry_unmeasurable
                           ? "refloat (a no-verdict re-entry is treated as UNMEASURABLE "
                             "and network.cycle_slip.gap_reentry_relevel_unmeasurable is "
                             "ON, so it is acted on)"
                           : "refloat, BUT INERT: "
                             "network.cycle_slip.gap_reentry_relevel_unmeasurable is OFF, "
                             "so every no-verdict arc is counted and bridged exactly as "
                             "under 'bridge'")
                    : "bridge (a no-verdict re-entry is left to the PASS-B inject and "
                      "only counted and evented)",
                qcc.gap_reentry_unreachable,
                qcc.ledger_reentry_arcs == 0
                    ? "  ⚠ ARMED AND INERT: the admitted population was EMPTY for the "
                      "whole run, so the ledger drove nothing.  Dead wiring, not a "
                      "quiet run."
                    : "");
            // Attribution of the no-verdict population (SID phase 4a, S1): the reason
            // mask of the gap that preceded each return says which screen removed the row
            // while both raw tiers kept seeing the satellite.  Printed only where the
            // writers are armed, so a run without them keeps the phase-3 line unchanged.
            if (!qcc.ledger_reentry_nv_reason.empty()) {
                std::string sp;
                for (int b = 0; b < kArcReasonCount; ++b) {
                    const long long n = qcc.ledger_reentry_nv_reason[
                        static_cast<std::size_t>(b)];
                    if (n == 0) continue;
                    if (!sp.empty()) sp += ", ";
                    sp += fmt::format("{} {}", arcReasonName(1u << b), n);
                }
                log->info(
                    "Network (card SID phase 4a / THE NO-VERDICT POPULATION BY REASON, "
                    "S1): of the {} ledger re-entr(y|ies) that NO raw tier saw, the "
                    "ADMITTED GAP that preceded the return was opened by: {}.  {} "
                    "carried no reason mask at all.  A return may carry more than one "
                    "reason, so these need not sum to the population.{}",
                    qcc.ledger_reentry_no_verdict, sp.empty() ? "nothing" : sp,
                    qcc.ledger_reentry_nv_no_mask,
                    qcc.ledger_reentry_no_verdict == 0
                        ? "  ⚠ EMPTY POPULATION: read it as a denominator."
                        : "");
            }
        }
        // ---- Per-signal tier (card GRS) --------------------------------
        // Reported only when the rider is armed, so an unarmed run's Card-G line is
        // unchanged.  Its populations are disjoint from the row-level ones above (a
        // satellite reported there contributes nothing here), so the two are added,
        // never reconciled.  Resumptions above zero with arcs at zero is dead wiring
        // (invariant #4).
        if (nc.slip_qc.gap_reentry_per_signal) {
            const auto& sg = qcc.gap_sig;
            log->info("Network (slip-QC card GRS / PER-SIGNAL gap re-entry): {} SIGNAL "
                      "re-entry(ies) [unit: (station,satellite,signal) return after ≥1 missed "
                      "epoch while the satellite's raw ROW survived, the population the "
                      "row-level tier is blind to] = {} SHIFTED (geometry-free level against "
                      "the partner moved beyond the empirical GF gate) + {} measured-and-"
                      "unshifted + {} UNMEASURABLE (no partner present on both sides); {} of "
                      "the measured ones used a partner that did NOT track through the gap "
                      "(second-choice witness, so a shifted verdict there names the PAIR, not "
                      "this signal).  RESPONSE (gates: shifted | LLI-on-this-signal {} | "
                      "unmeasurable {}): {} judged SLIPPED, {} skipped at a gauge-pinned "
                      "station; {} arc(s) reached with a PROTECTABLE carry, {} had none, {} "
                      "of the reached had no ambiguity STATE yet = {} shifted + {} lli + {} "
                      "unmeasurable FREED, {} cold-skipped, {} NOT ACTED (relevel_free_epoch "
                      "off), {} answered by the C3c funnel alone; {} ambiguity(ies) fed that "
                      "funnel.  BAND-SCOPED: only the returning signal is freed.{}{}",
                      qcc.gap_sig_resumptions, qcc.gap_sig_shifted,
                      qcc.gap_sig_resumptions - qcc.gap_sig_shifted
                          - qcc.gap_sig_unmeasurable,
                      qcc.gap_sig_unmeasurable, qcc.gap_sig_partner_broken,
                      nc.slip_qc.gap_reentry_lli ? "ON" : "OFF",
                      nc.slip_qc.gap_reentry_per_signal_unmeasurable ? "ON" : "OFF",
                      sg.verdicts, sg.pivot_skipped, sg.arcs, sg.no_carry,
                      sg.state_absent, sg.freed_shifted, sg.freed_lli,
                      sg.freed_unmeas, sg.cold_skipped, sg.no_relevel, sg.dd_only,
                      sg.dd_marked,
                      !nc.slip_qc.gap_reentry
                          ? "  ⚠ INERT BY CONFIGURATION: the rider is armed without "
                            "cycle_slip.gap_reentry_relevel, so the RESPONSE cannot act, and "
                            "with no row-level card armed at all nothing is even recorded (the "
                            "de-duplication list is filled by that card)."
                          : "",
                      (nc.slip_qc.gap_reentry && qcc.gap_sig_resumptions == 0)
                          ? "  ⚠ ARMED AND INERT: ZERO per-signal re-entries on a run with "
                            "multi-signal tracking.  This is dead wiring, not a quiet run."
                          : "");
            // Closure in arcs, as for the row-level tier: every reached arc lands in
            // exactly one bucket, so a reader never infers a missing cell.
            const long long closure = sg.freed_shifted + sg.freed_lli + sg.freed_unmeas
                                    + sg.cold_skipped + sg.no_relevel + sg.dd_only;
            if (closure != sg.arcs)
                log->error("Network (slip-QC card GRS): per-signal arc closure BROKEN: {} "
                           "reached vs {} bucketed (shifted {} + lli {} + unmeas {} + cold {} "
                           "+ no_relevel {} + dd_only {}).",
                           sg.arcs, closure, sg.freed_shifted, sg.freed_lli,
                           sg.freed_unmeas, sg.cold_skipped, sg.no_relevel, sg.dd_only);
        }
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

        // The same tier split per gradient chain (§22.6, REF-FAN).  The aggregate above
        // sums every chain into one number, so it cannot say whether arming the
        // additional chains floods the detector or backs it up.  Printed whenever the
        // ref gate is armed, whether or not ref_arm_all is: with that knob off the
        // additional chains show 0 opportunities, which is the reading that makes the
        // armed number interpretable (invariant #4; the yardstick is the first chains'
        // own count over the same window).
        {
            std::map<std::string, IfPhaseSlipDetector::RefArmChainCounters> per_chain;
            for (const auto& st : stations) {
                if (!st.if_slip) continue;
                for (const auto& [chain, c] : st.if_slip->refArmChainCounters()) {
                    auto& a = per_chain[chain];
                    a.opportunities     += c.opportunities;
                    a.candidates        += c.candidates;
                    a.fires_own         += c.fires_own;
                    a.fires_inherited   += c.fires_inherited;
                    a.fanout_suppressed += c.fanout_suppressed;
                }
            }
            std::string chain_detail;
            long long extra_own = 0, extra_inh = 0, first_fires = 0;
            for (const auto& [chain, c] : per_chain) {
                chain_detail += fmt::format(
                    "{}{} [opp {}, cand {}, own {}, inherited {}, fan-out suppressed {}]",
                    chain_detail.empty() ? "" : "; ", chain,
                    c.opportunities, c.candidates, c.fires_own, c.fires_inherited,
                    c.fanout_suppressed);
                if (chain.find('|') == std::string::npos) first_fires += c.fires_own;
                else { extra_own += c.fires_own; extra_inh += c.fires_inherited; }
            }
            log->info("Network (§22.6 REF-FAN / IF-TD reference arm per CHAIN): "
                      "ref_arm_all is {}.  ADDITIONAL-chain fires: {} own + {} inherited = {}, "
                      "against the FIRST chains' {} over the same window (F1 bar: <= 2x).  "
                      "Per chain — {}.  [ref_arm_all OFF ⇒ every '<sys>|<sig>_<sig>' chain "
                      "reads 0 opportunities: the arm does not run there, which is the §22.6 "
                      "defect, not a clean result.]",
                      nc.slip_qc.ref_arm_all ? "ARMED" : "off",
                      extra_own, extra_inh, extra_own + extra_inh, first_fires,
                      chain_detail.empty() ? std::string{"(no chain reported)"} : chain_detail);
        }
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
    // Chain-pin audit (docs/SPEC_synthetic_minimum_network.md §14 T1, measured record
    // §12.8): declared → folded → landed per (constellation, signal).  The election's
    // per-epoch cross-check only proves that the election recorded Σ (n_c + m_c) pins;
    // foldAmb/foldRpb return early without a message (no state, no live phase row), and
    // a station drop takes its folded pins with it, so "landed" is the number that
    // constrained the gauge.  A short group means the chain datum ran under-constrained
    // and the W5 near-null it is meant to remove is still open (the motivating case is
    // the W6+SH n-invariant L2W phase-bias common mode).  The number is reported rather
    // than inferred from the absence of a warning (invariant #4).
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
    // Seed-round run tallies (docs/SPEC_eq34_seed_round.md §5).  The assertion is
    // |pin innovation| ≤ λ/2 on every pin, which holds by construction of std::round
    // at both computation sites, so a violation is a logic error (stale λ map entry,
    // mismatched state), never a property of the data.
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
        // Re-host ledger (§8.5, INC-2).  Reported only when armed, so a twin with the
        // knob off keeps a run-end block byte-identical to a build without it.
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
    // Split-cadence fast clock (network.fast_clock, FC-1): the fast tier runs on epochs
    // the full pipeline skips, so it leaves no trace in the per-epoch readouts above.
    // These counters are its ON-path telemetry (invariant #4); a clean log says nothing
    // about whether a single tick fired.
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
    // Resume event readout (RTB-7(d), RE-4): its wiring is visible only through counters,
    // so "0 restored / no shock" is stated rather than inferred from absence (invariant #4).
    if (carry_invariant && !config_.getNetworkConfig().state_snapshot_path.empty()) {
        // Provenance of the σ̂₀ seed (RE-1).  Four distinct conditions suppress the seed (no
        // carry restored, calibration off, no 'S,' line, a rejected 'S,' line); the driver
        // reports which one held, and the line names it rather than a generic cause
        // (invariant #4; the same instrument fix as SPEC_pd_safe_dd_fold.md §11).
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
                  // The shape that was restored (RTB-7(e)).  Without it the diagonal arm
                  // of the A/B looks the same as the dense one in every log line and
                  // counter (invariant #4).
                  carry_driver.resumeCarryDiagonal()
                      ? fmt::format("DIAGONAL-ONLY (v3, {} block(s) — cross-covariances "
                                    "started at zero)",
                                    carry_driver.resumeCarryDiagonalBlocks())
                      : std::string("dense (v1/v2) or nothing restored"));
        // Write side (RTB-7(e)): the blob is about 99 % of the snapshot, so its size is the
        // readout that the diagonal knob took effect.
        if (config_.getNetworkConfig().state_snapshot_carry)
            log->info("Network (RTB-7e snapshot write): {} snapshot(s) written; last carry "
                      "blob {} bytes ({:.2f} MB), shape {}.",
                      ctx.carry.snap_writes, ctx.carry.snap_carry_bytes_last,
                      static_cast<double>(ctx.carry.snap_carry_bytes_last) / (1024.0 * 1024.0),
                      ctx.carry.snap_carry_diagonal
                          ? "DIAGONAL-ONLY (v3 — Sum K_c numbers)"
                          : "dense (v2 — Sum K_c^2 numbers)");
        // Linearization-frame halves (SPEC_event_harness §21.4, RS-2).  Both are reported
        // whenever either is armed: an armed reader that re-based nothing and an armed
        // writer that wrote no rows are the two invariant-#4 cases this line exposes, and
        // the bare "off" on the other half says which one it is.
        if (config_.getNetworkConfig().state_snapshot_carry_lin
            || config_.networkResumeCarryRebase()) {
            const auto& cc = carry_driver.counters();
            log->info("Network (RS-2 linearization carry): write {} / read {}; blob carried "
                      "ZL rows on {} restored block(s); re-based {} carried chord row(s) at "
                      "the first post-resume build, max |dz| {:.6f} m; {} row(s) marginalized "
                      "as UN-BASABLE (restored contributors with no ZL row — folding them "
                      "would assert the writer's x_lin frame).",
                      config_.getNetworkConfig().state_snapshot_carry_lin ? "ARMED" : "off",
                      config_.networkResumeCarryRebase() ? "ARMED" : "off",
                      carry_driver.resumeCarryLinBlocks(),
                      cc.resume_rebased, cc.resume_rebase_max_dz,
                      cc.resume_dropped_unbased);
        }
    }
    // Datum-continuity halves (SPEC_event_harness §21.7, RS-3).  At function scope, like
    // the coupling-capture readout, and outside the carry_invariant-gated RTB-7(d/e) block
    // above: the eq34 chain datum and the time-linked code-bias datum run on lineages
    // with and without the invariant branch, so nesting the line there would leave the
    // ON-path telemetry structurally inert on half the postures that arm it (invariant
    // #4).  Printed whenever either half is armed: an armed writer that wrote no rows and
    // an armed reader that restored none are the two invariant-#4 cases this line
    // exposes, and the bare "off" on the other half says which one it is.  The
    // first-post-resume-epoch figures are counted on every warm resume regardless, so an
    // A/B control leg reports them too.
    if (config_.getNetworkConfig().state_snapshot_datum_capture
        || config_.networkResumeDatumRestore()) {
        const auto& C = ctx.counters;
        const long long n_eq34_restored =
            C.n_resume_datum_eq34_rx + C.n_resume_datum_eq34_edges
            + C.n_resume_datum_sat_roots + C.n_resume_datum_prev_targets
            + C.n_resume_datum_prev_el + C.n_resume_datum_retained;
        log->info("Network (RS-3 datum restore): write {} / read {}; last snapshot carried "
                  "{} datum row(s); restored eq34 datum rx {}, rx parents {}, sat roots {}, "
                  "prev targets {} (+{} longevity elevation(s), {} gap-retained edge(s)); "
                  "CB prev {}, seen {}, IF-pin pairs {}; first post-resume epoch: eq34 "
                  "rerefs {}, reattaches {}, CB re-seed discs {}.{}",
                  config_.getNetworkConfig().state_snapshot_datum_capture ? "ARMED" : "off",
                  config_.networkResumeDatumRestore() ? "ARMED" : "off",
                  ctx.carry.snap_datum_rows_last,
                  C.n_resume_datum_eq34_rx, C.n_resume_datum_eq34_edges,
                  C.n_resume_datum_sat_roots, C.n_resume_datum_prev_targets,
                  C.n_resume_datum_prev_el, C.n_resume_datum_retained,
                  C.n_resume_datum_cb_prev, C.n_resume_datum_cb_seen,
                  C.n_resume_datum_cb_pair,
                  C.n_resume_datum_first_reref, C.n_resume_datum_first_reattach,
                  C.n_resume_datum_first_cb_disc,
                  (config_.networkResumeDatumRestore() && n_eq34_restored == 0
                   && C.n_resume_datum_cb_prev == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "EQ* / CB* rows (a writer without "
                        "network.state_snapshot_datum_capture?) or no warm resume "
                        "happened; check the resume log for the RS-3 line."
                      : "");
    }
    // Emit-layer continuity halves (SPEC_event_harness §21.10, RS-5), beside the RS-3 line
    // and at the same function scope for the same invariant-#4 reason: the two engines run
    // on lineages with and without the invariant branch.  Printed whenever either half is
    // armed: an armed writer that wrote no rows and an armed reader that restored none are
    // the two invariant-#4 cases this line exposes, and the bare "off" on the other half
    // says which one it is.
    if (config_.getNetworkConfig().state_snapshot_emit_capture
        || config_.networkResumeEmitRestore()) {
        const auto& C = ctx.counters;
        log->info("Network (RS-5 emit restore): write {} / read {}; last snapshot carried "
                  "{} emit-layer row(s); restored re-base keys {}, filter keys {}, step "
                  "accumulators {}.{}",
                  config_.getNetworkConfig().state_snapshot_emit_capture ? "ARMED" : "off",
                  config_.networkResumeEmitRestore() ? "ARMED" : "off",
                  ctx.carry.snap_emit_rows_last,
                  C.n_resume_emit_rebase_keys, C.n_resume_emit_filter_keys,
                  C.n_resume_emit_step_accums,
                  (config_.networkResumeEmitRestore()
                   && C.n_resume_emit_rebase_keys == 0
                   && C.n_resume_emit_filter_keys == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "ER / PF / PS rows (a writer without "
                        "network.state_snapshot_emit_capture?), neither emit engine is "
                        "running this run, or no warm resume happened; check the resume "
                        "log for the RS-5 line."
                      : "");
    }
    // Station-layer continuity halves (SPEC_event_harness §21.13, RS-6), beside the RS-5
    // line and at the same function scope for the same invariant-#4 reason: the two
    // carries run on lineages that do and do not arm each other.  Printed whenever either
    // half is armed: an armed writer that wrote no rows and an armed reader that restored
    // none are the two invariant-#4 cases this line exposes, and the bare "off" on the
    // other half says which one it is.  The sub-switch states are always named, so an
    // attribution leg that restored one half can be read from the run-end tail alone.
    if (config_.getNetworkConfig().state_snapshot_station_capture
        || config_.networkResumeStationRestore()) {
        const auto& C = ctx.counters;
        log->info("Network (RS-6 station restore): write {} / read {} (rxcb {}, shdev {}); "
                  "last snapshot carried {} station-layer row(s); restored rx-CB entries "
                  "{}, deviation entries {}.{}",
                  config_.getNetworkConfig().state_snapshot_station_capture ? "ARMED" : "off",
                  config_.networkResumeStationRestore() ? "ARMED" : "off",
                  config_.networkResumeStationRestoreRxCb() ? "on" : "off",
                  config_.networkResumeStationRestoreShDev() ? "on" : "off",
                  ctx.carry.snap_station_rows_last,
                  C.n_resume_station_rxcb, C.n_resume_station_shdev,
                  (config_.networkResumeStationRestore()
                   && C.n_resume_station_rxcb == 0 && C.n_resume_station_shdev == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "RC / SD rows (a writer without "
                        "network.state_snapshot_station_capture?), both sub-switches are "
                        "off, neither carry is armed this run, or no warm resume "
                        "happened; check the resume log for the RS-6 line."
                      : "");
    }
    // Canonical-signal latch (SPEC_event_harness §21.16, RS-8), beside the RS-6 line and
    // printed only when armed, as that line is.  The disagree count reads in both
    // directions: > 0 is the defect being prevented (that many restored entries would
    // have latched differently on this run's first epoch), and 0 says the seam epoch
    // happened to agree.  It never says the wiring did nothing; the stations/entries pair
    // reports that (invariant #4).
    if (config_.getNetworkConfig().state_snapshot_signal_capture
        || config_.networkResumeSignalCanonRestore()) {
        auto& C = ctx.counters;
        // The audit runs inside the station models on the first post-resume epoch, so
        // the roster is the only place the result lives; it is summed here.
        long long audited = 0, disagree = 0;
        for (const auto& st : setup.stations) {
            if (!st.model) continue;
            audited  += st.model->canonicalAuditEntries();
            disagree += st.model->canonicalAuditDisagreements();
        }
        C.n_resume_canon_audited  = audited;
        C.n_resume_canon_disagree = disagree;
        log->info("Network (RS-8 canonical restore): write {} / read {}; last snapshot "
                  "carried {} CN row(s); restored {} entr{} across {} station(s); first "
                  "post-resume epoch audited {} of them and {} would have been decided "
                  "DIFFERENTLY here.{}",
                  config_.getNetworkConfig().state_snapshot_signal_capture ? "ARMED" : "off",
                  config_.networkResumeSignalCanonRestore() ? "ARMED" : "off",
                  ctx.carry.snap_canon_rows_last,
                  C.n_resume_canon_entries, C.n_resume_canon_entries == 1 ? "y" : "ies",
                  C.n_resume_canon_stations, audited, disagree,
                  (config_.networkResumeSignalCanonRestore()
                   && C.n_resume_canon_entries == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no CN "
                        "rows (a writer without network.state_snapshot_signal_capture?) "
                        "or no warm resume happened; check the resume log for the RS-8 "
                        "line."
                      : (disagree > 0
                             ? "  Those are the flips the card prevents: without the "
                               "restore each of them re-decides the station's canonical "
                               "per-band channel at the seam."
                             : ""));
    }
    // Code-bias linearization-point half (SPEC_event_harness §21.18, RS-9), beside the
    // RS-6 line for the same invariant-#4 reason: RS-6 restores the rx-CB carry's
    // numerator and this restores the reference its inject gate differences it against,
    // and a run that restored one without the other has to be readable from the run-end
    // tail alone.  Printed whenever either half is armed.
    if (config_.getNetworkConfig().state_snapshot_rxcb_lin_capture
        || config_.networkResumeRxCbLinRestore()) {
        const auto& C = ctx.counters;
        // The applied / uncovered tallies are written station-locally inside the
        // parallel produce phase (one out-slot per station, never resized) and summed
        // here, the only place that needs them, which is serial by construction.
        long long applied = 0, uncovered = 0;
        for (const auto& [a, u] : ctx.carry.rxcb_lin_sta_counts) { applied += a; uncovered += u; }
        // "uncovered" is scoped to the carried keys (the RL rows pair 1:1 with the RC
        // rows): a code-bias family with no carry never reaches the gate and is not a
        // miss, so a nonzero here is a carried key whose reference could not be applied.
        log->info("Network (RS-9 rx-CB frame restore): write {} / read {}; last snapshot "
                  "carried {} RL row(s); restored {} linearization point(s) across {} "
                  "station(s), seeded {}, carried-but-unseeded {}.{}",
                  config_.getNetworkConfig().state_snapshot_rxcb_lin_capture ? "ARMED" : "off",
                  config_.networkResumeRxCbLinRestore() ? "ARMED" : "off",
                  ctx.carry.snap_rxcb_lin_rows_last,
                  C.n_resume_rxcb_lin_entries, C.n_resume_rxcb_lin_stations,
                  applied, uncovered,
                  (config_.networkResumeRxCbLinRestore() && C.n_resume_rxcb_lin_entries == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no RL "
                        "rows (a writer without network.state_snapshot_rxcb_lin_capture?) "
                        "or no warm resume happened; check the resume log for the RS-9 "
                        "line."
                      : ((config_.networkResumeRxCbLinRestore()
                          && C.n_resume_rxcb_lin_entries > 0 && applied == 0)
                             ? "  ⚠ points were STASHED and none was ever SEEDED — no "
                               "station reached its first-epoch code-bias initialization "
                               "with a carried key (estimate_ifb off, or the samples loop "
                               "never saw those families), so the gate still reads this "
                               "leg's own frame."
                             : (uncovered > 0
                                    ? "  ⚠ NONZERO carried-but-unseeded — a key WITH an "
                                      "rx-CB carry could not take its restored reference "
                                      "(non-finite value, or a code-bias state index this "
                                      "run does not hold), so the gate reads this leg's "
                                      "own frame for it and can refuse that station for "
                                      "the whole leg.  A code-bias family with no carry is "
                                      "NOT counted here."
                                    : "")));
    }
    // Declaration half (SPEC_event_harness §21.9, RS-4), beside the RS-3 line and at the
    // same function scope for the same invariant-#4 reason.  The census is taken on every
    // warm resume the knob is armed for, so an armed run that marked nothing reports its
    // tested/over counts and an armed run that never resumed reports zeros; the line
    // tells those two cases apart.  Printed only when the knob is armed: with it off
    // nothing is measured (pbSeamCensus is never called), and a 0/0 line would be a false
    // instrument.
    if (config_.networkResumePbSeamMark()) {
        const auto& C = ctx.counters;
        const double pct = C.n_pb_seam_keys_tested > 0
                               ? 100.0 * static_cast<double>(C.n_pb_seam_keys_over)
                                     / static_cast<double>(C.n_pb_seam_keys_tested)
                               : 0.0;
        log->info("Network (RS-4 PB seam mark): ARMED; keys tested {}, over threshold {} "
                  "({:.1f} %), constellations marked {} (bar {:.0f} %).{}",
                  C.n_pb_seam_keys_tested, C.n_pb_seam_keys_over, pct, C.n_pb_seam_marks,
                  GnssConfig::kResumePbSeamMarkFraction * 100.0,
                  C.n_pb_seam_keys_tested == 0
                      ? "  ⚠ 0 keys tested — no warm resume happened this run, or the "
                        "first post-resume emit found no restored emitter baselines (a "
                        "snapshot without an EMITTER blob); the rule is INERT here."
                      : "");
    }
    // Coupling capture, write side (§18.12): the sibling of the RTB-7(e) snapshot readout,
    // but at function scope on purpose.  The RTB-7(d/e) block above is gated on
    // carry_invariant, while the coupling capture rides the phase-leveling carries
    // (carry_conditional_coupling, which supersedes carry_invariant; the target lineage
    // runs carry_invariant: false), so nesting it there would leave the ON-path telemetry
    // structurally inert on the postures that arm it (invariant #4).  About 25 B per
    // (id, coeff) pair is the §2.7 size budget's figure; it turns "rows captured" into
    // the disk and I/O size a keep-history leg has to plan for.
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
        // v3k clause only when armed, so the v2/v3 lines stay byte-identical with it off.
        if (shc.rep_error_d5_keep)
            log->info("Network (Iono-2/v3k D5-keep): {} D5-only re-tap(s) KEPT the level "
                      "(variance re-tapped to σ_ss), {} raw-confirmed erase(s) — the erase "
                      "policy split (SPEC_iono2_mechB_v3 addendum).",
                      ctx.carry.shdev_d5_keeps_total, ctx.carry.shdev_slip_resets_total);
        // v3 clause only when armed, so the v2 line stays byte-identical with it off.
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
    if (config_.networkCarryRxCodeBias()) {
        // Stations that carried nothing (RS-9 rider, §21.18): printed on every run with the
        // rx-CB carry armed; trace-class, products byte-identical.  The pooled fold/withhold
        // counts in the line below cannot show a station refused on every epoch: the
        // unconditional harvest refreshes it and it is never pruned (< 900 s), so it counts
        // in "N carried at run end" while carrying nothing (invariant #4).  Such stations
        // are named here so the census does not have to be re-derived from the traces.
        constexpr double kRs9WithholdShare = 0.90;
        std::vector<std::string> carried_nothing;
        for (const auto& [name, t] : ctx.carry.rx_cb_sta_tally)
            if (t.first > 0
                && static_cast<double>(t.second) > kRs9WithholdShare * static_cast<double>(t.first))
                carried_nothing.push_back(name);
        std::string nothing_tail;
        if (!carried_nothing.empty()) {
            nothing_tail = fmt::format("; {} station(s) withheld > {:.0f} % of their epochs "
                                       "(carried NOTHING):", carried_nothing.size(),
                                       kRs9WithholdShare * 100.0);
            for (const auto& n : carried_nothing) { nothing_tail += ' '; nothing_tail += n; }
        } else if (!ctx.carry.rx_cb_sta_tally.empty()) {
            nothing_tail = fmt::format("; 0 station(s) withheld > {:.0f} % of their epochs",
                                       kRs9WithholdShare * 100.0);
        }
        log->info("Network (rx-CB carry): {} receiver code-bias prior(s) folded, {} withheld "
                  "by the 10σ innovation gate (receiver-reset response), {} harvest(s); {} "
                  "carried at run end (RW q={} m²/s; harvested outside the phase-leveling "
                  "gate — SPEC_rx_cb_carry){}.",
                  ctx.carry.rx_cb_injected_total, ctx.carry.rx_cb_innov_skips_total,
                  ctx.carry.rx_cb_harvested_total, ctx.carry.rx_cb_carry.size(),
                  config_.networkRxCbCarryQ(), nothing_tail);
        // RXCB-IN-BLOCK (card CTU section 14): the in-block route's own counts.  Zero
        // block columns on an armed run means the knob never acted (invariant #4).
        if (ctx.knobs.rxcb_blk) {
            const auto& C = ctx.counters;
            log->info("Network (RXCB-IN-BLOCK): {} receiver code-bias column(s) admitted into "
                      "station blocks, {} pushed but NOT folded (non-SPD block, no diagonal "
                      "fallback), {} (rx-CB, other) pair(s) zeroed for a missing cross term; "
                      "harvest: {} stashed with a border regression, {} without a block row, "
                      "{} coupling harvest(s) failed, {} erased by the step test (event "
                      "response).",
                      C.n_pl_blk_rxcb_cols, C.n_pl_blk_rxcb_not_folded,
                      C.n_pl_blk_rxcb_missing_cross, C.n_rxcb_blk_harvested,
                      C.n_rxcb_blk_no_block, C.n_rxcb_blk_harvest_fail,
                      C.n_rxcb_blk_step_reset);
            if (C.n_rxcb_blk_restored_skip > 0 || ctx.carry.ctu_switch_on_resume)
                log->info("Network (RXCB-IN-BLOCK / card CTU G5): switch-on warm resume {}; {} "
                          "restored diagonal rx-CB entr(y/ies) kept out of the block and "
                          "re-harvested; exact-update stash stamp rebuilt from the restored "
                          "border {} time(s).",
                          ctx.carry.ctu_switch_on_resume ? "YES" : "no",
                          C.n_rxcb_blk_restored_skip, C.cxu_resume_stash);
            if (C.n_pl_blk_rxcb_cols == 0)
                log->warn("Network (RXCB-IN-BLOCK): ARMED but no receiver code-bias column "
                          "entered a block over the whole run: the knob is inert.");
        }
    }
    // Receiver code-bias de-mean (card LV-2, docs/SPEC_level_information_budget.md):
    // run-end line, printed only when armed.  "0 demeaned" has to be visible, because a
    // de-mean that never fired looks like a working one in every other trace (invariant
    // #4).  The skip rate is printed against its own denominator: judging |innov − μ| is
    // meant to stop a coherent frame move from triggering the receiver-reset response at
    // many stations at once.
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
    // Within-epoch μ (card LV-5, docs/SPEC_level_information_budget.md §12): run-end
    // line, printed only when armed.  The key number is the residual: the folded set's
    // own common mode minus the μ applied to it.  Near zero means the within-epoch
    // correction nulled the force on the frame direction; a residual the size of μ means
    // it did not, and the de-mean had no effect.
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
    // Invariant carry gate (card LV-4, docs/SPEC_level_information_budget.md §11):
    // run-end line, printed only when armed.  The main number is the same-run A/B: how
    // many folds the retired frame-dependent screen would have withheld on these rows,
    // against how many invariant steps the step screen found.  A large first number with
    // a near-zero second one means the retired screen was measuring the seed, not the
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
    // H-rider census σ̂₀ (edge-queue §1.6 item 3a): run-end line, printed only when armed.
    // A census-fed wire whose pool is empty on every epoch ships the 0.0000 sentinel on
    // every record while its configuration reads as armed; the count of epochs with a
    // usable value is what shows it (invariant #4).
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
    // S-consistent harvest (card S2H, docs/SPEC_level_information_budget.md §17 item 3):
    // run-end line, printed only when armed.  "0 epochs applied" has to be visible: a
    // transform that never ran leaves the carry with its full common-mode force, which
    // looks like a working run in every other trace (invariant #4).  The main numbers
    // are mean and max |μ| (the common-mode content removed per epoch) and the thin-skip
    // count (identity epochs, the decoupling edge of invariant #7).
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
    // Iono seed frame (card LV-1, docs/SPEC_level_information_budget.md): run-end line,
    // printed only when armed.  The per-station counters live on each station's
    // StateInitializer (the reframe happens inside the parallel produce phase) and are
    // summed here serially, as the SD-EWL and iono_var feed counters are.
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
    // No-ionosphere-constraint posture (network.w6_iono_constraint = none, RF-0b): run-end
    // line, printed only when armed.  It answers for the satellite half the question the
    // RXCB-DATUM line answers for the receivers: in this posture the sum-to-zero rows are
    // not a gauge beside an ionosphere constraint but the satellite code-bias datum
    // itself, so the fold count is what separates a live datum from an armed-but-inert
    // one (invariant #4).
    if (config_.getNetworkConfig().w6_iono_constraint == NetworkW6IonoConstraint::None) {
        if (ctx.counters.n_w6_iono_none_epochs == 0)
            log->warn("Network (W6-IONO-NONE): the no-ionosphere-constraint posture is ARMED but "
                      "the satellite code-bias datum folded on ZERO epoch(s) — no epoch ever "
                      "presented a registered code-bias column, so the satellite code biases ran "
                      "with NO datum and NO ionosphere constraint at once.  This run is NOT the "
                      "posture it claims (standing invariant #4): check "
                      "network.estimate_sat_code_bias and the signal coverage gate.");
        else if (ctx.counters.n_w6_iono_none_s2z_rows == 0)
            log->warn("Network (W6-IONO-NONE): the datum folded on {} epoch(s) but pinned ZERO "
                      "(constellation, signal) sum-to-zero level(s) — the ionosphere-free pin was "
                      "folded against nothing, so every channel's constellation-wide code-bias "
                      "LEVEL is unpinned on top of the between-satellite split the missing "
                      "ionosphere constraint already left open.  Do not grade the emitted code "
                      "biases from this run.",
                      ctx.counters.n_w6_iono_none_epochs);
        else
            log->info("Network (W6-IONO-NONE): NO ionosphere constraint over the run — the "
                      "satellite code-bias datum folded on {} epoch(s), pinning {} "
                      "(constellation, signal) sum-to-zero level(s) in total (mean {:.2f} per "
                      "epoch) alongside the per-satellite ionosphere-free pin.  Those rows pin "
                      "each channel's constellation-wide LEVEL; the BETWEEN-satellite "
                      "geometry-free directions are left to the OSB prior "
                      "(network.cb_kaula_prior {}), so read the null-space gate's "
                      "observed-vs-declared before grading the emitted code biases, and apply the "
                      "emitted ionosphere and code biases AS A PAIR.",
                      ctx.counters.n_w6_iono_none_epochs,
                      ctx.counters.n_w6_iono_none_s2z_rows,
                      static_cast<double>(ctx.counters.n_w6_iono_none_s2z_rows)
                          / static_cast<double>(ctx.counters.n_w6_iono_none_epochs),
                      config_.networkCbKaulaPrior()
                          ? fmt::format("ON, sigma {:.1f} m", config_.networkCbKaulaSigmaM())
                          : std::string{"OFF"});
    }
    // Satellite geometry-free datum (network.sat_gf_datum, RF-0c): run-end line, printed
    // only when armed.  Under the no-constraint posture this row alone closes each
    // satellite's geometry-free direction, so its fold count separates a datumed product
    // from one whose code biases were shaped by whatever loose prior was armed.
    if (config_.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode) {
        const bool tgd = config_.getNetworkConfig().sat_gf_datum
                             == NetworkSatGfDatum::BroadcastTgd;
        const char* mode = tgd ? "broadcast_tgd" : "zero";
        if (ctx.counters.n_sat_gf_rows == 0)
            log->warn("Network (SAT-GF-DATUM): the hard per-satellite geometry-free datum "
                      "is ARMED at '{}' but folded ZERO row(s) over the run — no satellite "
                      "ever presented a two-distinct-frequency code-bias pair, so every "
                      "satellite's geometry-free direction ran UNPINNED under a free slant "
                      "ionosphere.  This run is NOT the posture it claims (standing "
                      "invariant #4); do not grade its code biases or its ionosphere.",
                      mode);
        else if (tgd && ctx.counters.n_sat_gf_tgd == 0)
            log->warn("Network (SAT-GF-DATUM): '{}' folded {} row(s) on {} epoch(s) but NOT "
                      "ONE carried a non-zero c*T_GD ({} broadcast T_GD = 0, {} fell back "
                      "to zero for want of a navigation record or an L1/L2 pinned pair) — "
                      "the run is numerically identical to sat_gf_datum = zero and must be "
                      "reported as such, not as the physical datum.",
                      mode, ctx.counters.n_sat_gf_rows, ctx.counters.n_sat_gf_epochs,
                      ctx.counters.n_sat_gf_tgd0, ctx.counters.n_sat_gf_fallback);
        else
            log->info("Network (SAT-GF-DATUM): hard per-satellite GEOMETRY-FREE datum '{}' "
                      "— {} pin row(s) over {} epoch(s) (mean {:.2f} satellites per epoch), "
                      "each on (d2-d1)/(mu2-mu1) over that satellite's ionosphere-free pin "
                      "pair, folded as a constraint row BEFORE the null-space gate so the "
                      "declared S-basis dimension is untouched.{}  The estimable slant "
                      "ionosphere absorbs the pinned value (Odijk 2016 Table 8), so the "
                      "emitted ionosphere and code biases must be applied AS A PAIR.",
                      mode, ctx.counters.n_sat_gf_rows, ctx.counters.n_sat_gf_epochs,
                      ctx.counters.n_sat_gf_epochs
                          ? static_cast<double>(ctx.counters.n_sat_gf_rows)
                                / static_cast<double>(ctx.counters.n_sat_gf_epochs)
                          : 0.0,
                      tgd ? fmt::format("  PER-SIGNAL (RF-0d): {} extra row(s) beyond the "
                                        "ionosphere-free pair, of which {} ASSERT an "
                                        "inter-signal correction of ZERO (RINEX navigation "
                                        "carries no GPS ISC — that assertion is the part of "
                                        "this 'broadcast' datum the message does not state); "
                                        "{} same-band column(s) left ESTIMABLE on purpose (no "
                                        "geometry-free combination exists against the "
                                        "reference, and the pure ISC needs no ionosphere to "
                                        "be observable); {} sum-to-zero row(s) SUPPRESSED as "
                                        "channels the datum already pins completely (folding "
                                        "both would fight: the pinned values sum to "
                                        "mu*c*Sum_s T_GD, not to zero).  TARGETS: {} row(s) "
                                        "at a non-zero c*T_GD, {} at a "
                                        "broadcast T_GD of exactly 0, {} FELL BACK to zero "
                                        "(no navigation record, or a pinned pair the "
                                        "navigation message never referenced) — the "
                                        "fallback count is the part of this product that "
                                        "is NOT the physical datum it is named for.",
                                        ctx.counters.n_sat_gf_extra,
                                        ctx.counters.n_sat_gf_isc0,
                                        ctx.counters.n_sat_gf_sameband,
                                        ctx.counters.n_sat_gf_s2z_suppressed,
                                        ctx.counters.n_sat_gf_tgd,
                                        ctx.counters.n_sat_gf_tgd0,
                                        ctx.counters.n_sat_gf_fallback)
                          : fmt::format("  PER-SIGNAL (RF-0d): {} extra row(s) beyond the "
                                        "ionosphere-free pair, {} same-band column(s) left "
                                        "ESTIMABLE, {} sum-to-zero row(s) suppressed as "
                                        "channels the datum already pins completely.",
                                        ctx.counters.n_sat_gf_extra,
                                        ctx.counters.n_sat_gf_sameband,
                                        ctx.counters.n_sat_gf_s2z_suppressed));
    }
    // Level-datum arms (RF-0e): run-end lines, printed only when armed.  Both arms target a
    // measured consumer regression, so whether they folded at all is the first thing any
    // reading of that consumer has to answer (invariant #4).
    if (config_.getNetworkConfig().sat_if_level_datum) {
        if (ctx.counters.n_if_level_rows == 0)
            log->warn("Network (IF-LEVEL-DATUM, RF-0e e1): ARMED but folded ZERO row(s) — no "
                      "satellite ever presented a valid ionosphere-free pin pair, so the "
                      "constellation's ionosphere-free code-bias LEVEL ran UNANCHORED and "
                      "this run did not test the arm it claims to (standing invariant #4).");
        else
            log->info("Network (IF-LEVEL-DATUM, RF-0e e1): {} absolute row(s) folded over the "
                      "run, summing {} satellite-epoch(s) (mean {:.2f} satellites per row) — "
                      "each pinning Sum_s (a1*d1 + a2*d2) = 0 for its constellation at the "
                      "gauge weight.  It constrains the ionosphere-free LEVEL only, which "
                      "the geometry-free pins leave free; under broadcast_tgd the target is "
                      "EXACTLY satisfied by a pinned roster, so the two datum families are "
                      "consistent rather than competing.  The row is STRONG, not gentle: it "
                      "carries one entry per satellite, so at equal gauge weight it beats the "
                      "n single-entry change pins and the realized level lands at "
                      "carried/(n+1) - the LEVEL is re-zeroed each epoch while the "
                      "per-satellite DIFFERENCES stay carried.",
                      ctx.counters.n_if_level_rows, ctx.counters.n_if_level_sats,
                      ctx.counters.n_if_level_rows
                          ? static_cast<double>(ctx.counters.n_if_level_sats)
                                / static_cast<double>(ctx.counters.n_if_level_rows)
                          : 0.0);
    }
    if (config_.getNetworkConfig().sat_gf_datum_time_linked) {
        const std::size_t tot = ctx.counters.n_sat_gf_carried + ctx.counters.n_sat_gf_anchored;
        if (tot == 0)
            log->warn("Network (SAT-GF TIME-LINK, RF-0e e2): ARMED but no geometry-free row "
                      "was ever targeted — the datum folded nothing, so this run did not "
                      "test the arm it claims to (standing invariant #4).");
        else
            log->info("Network (SAT-GF TIME-LINK, RF-0e e2): {} geometry-free row(s) CARRIED "
                      "the previous kept epoch's solved combination and {} were ANCHORED at "
                      "the absolute target ({:.1f} % anchored — first sightings).  The "
                      "anchored rows are the ONLY place the broadcast frame is re-asserted; "
                      "a run with very few of them has carried its frame from ignition and "
                      "never re-checked it, which is the cost of this arm and the thing to "
                      "weigh against the level row e1 folds instead.",
                      ctx.counters.n_sat_gf_carried, ctx.counters.n_sat_gf_anchored,
                      100.0 * static_cast<double>(ctx.counters.n_sat_gf_anchored)
                          / static_cast<double>(tot));
    }
    // Inter-signal correction state (RF-0f): run-end line, printed only when armed.  On the
    // wire an estimated and an asserted inter-signal correction differ only by the isc=
    // token and the missing record, so the run states the split (invariant #4).
    if (config_.getNetworkConfig().sat_isc_state) {
        const std::size_t tot = ctx.counters.n_isc_observed + ctx.counters.n_isc_pinned;
        if (tot == 0)
            log->warn("Network (ISC-STATE, RF-0f): ARMED but no band-2 same-band companion "
                      "column was ever seen — no satellite carried a second band-2 code, so "
                      "this run exercised nothing and is byte-equivalent to the unarmed "
                      "posture (standing invariant #4).");
        else
            log->info("Network (ISC-STATE, RF-0f): {} companion column-epoch(s) left FREE "
                      "because a station tracked both signals of that satellite, {} PINNED "
                      "at 0 for want of one ({:.1f} % pinned).  A free one is an estimated "
                      "inter-signal correction and its bias is emitted with isc=obs; a "
                      "pinned one is the RF-0d assertion of ISC = 0 and its bias is NOT "
                      "emitted, because a consumer cannot tell the two apart from the value.",
                      ctx.counters.n_isc_observed, ctx.counters.n_isc_pinned,
                      100.0 * static_cast<double>(ctx.counters.n_isc_pinned)
                          / static_cast<double>(tot));
    }
    // Extra-band free columns (B5D): run-end line, printed only when armed.  The effect is
    // which columns stop being asserted, and neither the wire nor the trace stream shows
    // it: a freed column looks the same as a pinned one in the B record (invariant #4).
    if (config_.getNetworkConfig().sat_gf_extra_band_free) {
        const std::size_t tot = ctx.counters.n_extra_band_free
                              + ctx.counters.n_extra_band_carried
                              + ctx.counters.n_extra_band_held;
        if (tot == 0)
            log->warn("Network (EXTRA-BAND FREE, B5D): ARMED but no extra-band code column "
                      "was ever seen — no satellite carried a registered code signal "
                      "outside its ionosphere-free pin pair's two bands, so this run "
                      "exercised nothing and is byte-equivalent to the unarmed posture "
                      "(standing invariant #4).  A dual-frequency lineage reads this way "
                      "by construction; on a triple one it means the third band never "
                      "reached the >= 2-station register.");
        else
            log->info("Network (EXTRA-BAND FREE, B5D): {} column-epoch(s) FREED (a station "
                      "carried code on both of the pin pair's bands and on the signal, so "
                      "the data determine the column up to its family constant and the "
                      "channel's sum-to-zero row is that constant's datum), {} CARRIED at "
                      "their own previous solved geometry-free combination because no "
                      "station determined them that epoch, {} HELD at a first sighting with "
                      "nothing to carry ({:.2f} % held) — a held column is pinned by a "
                      "level row in the channel's own frame and its bias is NOT emitted, "
                      "because a value chosen to keep the rank is indistinguishable from "
                      "an estimate.  Every one of these would have been an ASSERTION of a "
                      "zero inter-signal correction under RF-0d alone (counted beside this "
                      "as n_sat_gf_isc_zero = {}), which card TBI measured at metres across "
                      "a band.  The freed channels' LEVEL now re-references whenever the "
                      "veteran link re-seeds: {} sum-to-zero row(s) re-seeded over the run, "
                      "marking {} satellite-epoch(s) for the consumer's discontinuity "
                      "channel (standing invariant #7, the decoupling edge).",
                      ctx.counters.n_extra_band_free, ctx.counters.n_extra_band_carried,
                      ctx.counters.n_extra_band_held,
                      100.0 * static_cast<double>(ctx.counters.n_extra_band_held)
                          / static_cast<double>(tot),
                      ctx.counters.n_sat_gf_isc0,
                      ctx.counters.n_s2z_reseed_rows, ctx.counters.n_s2z_reseed_sats);
    }
    // Receiver code-bias datum (RXCB-DATUM): run-end line, printed only when armed.  The
    // fold count is the only way to tell a live gauge from an armed-but-inert one, so it
    // is printed even when it is zero (invariant #4).
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
        // One line per lane instance (UWL-1): per constellation when the card is armed,
        // since a pooled line hides which lane the rows come from and an armed
        // constellation that decided 0 cycles gets its own 0 line (invariant #4); the
        // legacy pooled line otherwise (lane.cons == 0).
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
        // Per-constellation WL-walk breakdown, never pooled only (SPEC §2; UWL-1).
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
            // Dual-rung and sibling-closure breakdown (increment 5, uwl_dual_top).
            // mean|frac| ≈ 0 means the 2·N(8,5) = N(7,5) identity holds on this data;
            // ≈ 0.5 means a receiver half-cycle convention offsets it, and a constant has
            // to be learned before arming uwl_sib_gate (SPEC reconciliation note 2).
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
        // Lane-coordinate carry run-end line (WL-3; invariant #4).  In the lane shape the
        // numbers to read are the continuous increments, the fold-on-change ratio and the
        // inject retirement count (about 0 expected; the WL-2 fallback path).
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
        // The warm-up clause is printed whenever the knob is armed, including at 0
        // suppressions (RS-2b): "armed and suppressed nothing" is a finding (the window
        // closed before any chord tripped, or nothing was restored to trip), and a clause
        // printed only on a nonzero count could not state it (invariant #4).
        log->info("Network (Card 4/M1 D5 fine-slip innovation gate): {} carried-DD innovation "
                  "flag(s), {} fresh re-tap(s) over the run (k={}; the estimation-side reset for "
                  "the fine slips the coarse MW/LLI/IF-TD layer is relaxed against — each flagged "
                  "chord re-taps data-only + marks its arc slipped for the next epoch).{}",
                  carry_innov_flags_total, carry_innov_retaps_total,
                  config_.networkCarryInvariantInnovK(),
                  config_.networkResumeD5WarmupEp() > 0
                      ? fmt::format("; {} trip(s) suppressed in the {}-epoch post-resume "
                                    "warm-up (counted, never re-tapped, never funnelled) "
                                    "over {} epoch(s) the window was open",
                                    carry_driver.counters().d5_warmup_suppressed,
                                    config_.networkResumeD5WarmupEp(),
                                    ctx.counters.n_resume_d5_warmup_epochs)
                      : std::string());
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
        // σ̂₀ clamp visibility.  A clamped 1.00× looks the same as a calibrated 1.00×, so the
        // clamped σ̂₀ alone cannot show that the D5 gate is running as the raw formal (v1)
        // test (SPEC_pd_safe_dd_fold.md §10 finding 2).  The line reports the state
        // histogram and the raw range, and warns when the calibration was inert for most of
        // the run: an armed knob at its neutral value is not benign (invariant #4).
        const auto& cc  = carry_driver.counters();
        const auto  st  = carry_driver.innovInflationStatus();
        const long long den = std::max<long long>(1, cc.d5_calib_epochs);
        const long long inert = cc.d5_calib_warmup_epochs + cc.d5_calib_floored_epochs;
        const double inert_pct = 100.0 * static_cast<double>(inert) / static_cast<double>(den);
        const std::string raw_range =
            (cc.d5_calib_raw_max > 0.0)
                ? fmt::format("{:.4g} … {:.4g}", cc.d5_calib_raw_min, cc.d5_calib_raw_max)
                : std::string{"(never estimated past warm-up)"};
        // The two σ̂₀² figures are different quantities on purpose: the final one is the
        // post-run accumulator state (it includes the last install's own rows, which no gate
        // consulted), while the range covers the values the gate used.  The final value can
        // therefore sit outside the range; that is correct.
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
    // CB-excursion guard run-end telemetry (SPEC_day129_frame_guards Phase 2.1).
    // "0 trips / 0 reinits" means the guard is inert however clean the run looks
    // (invariant #4); this line is the ON-path evidence, and the validation gate reads it.
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
    // SH health gate run-end telemetry (SPEC_day129_frame_guards Phase 2.2; invariant
    // #4).  The out-of-band count is per epoch: the throttled T1a warning fires far less
    // often than the field leaves its band, so it cannot serve as the count.
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
    // Post-fit σ̂0 run-end telemetry (SPEC_day129_frame_guards P1-3/P1-4).  An
    // instrumented run states how many epochs the instrumentation produced: "0 traced"
    // means the wiring is inert however clean the log looks, and the trigger verdict
    // would rest on an empty file (invariant #4).
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
    if (config_.getNetworkConfig().trace_postfit_rows)
        // "0 rows under an armed trace" logs at warn level, not as a clean line
        // (invariant #4).
        log->log(ctx.carry.pf_rows_written > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (§24.6 post-fit rows): {} SIGNED per-row post-fit residual(s) written "
                 "to network_pf_rows.csv ({} code + {} phase; {} written with an empty "
                 "elev_deg); {} data row(s) the pass visited were attributed to neither "
                 "family and NOT written (rows with NO shared partial at all — pivot pins "
                 "and local prior rows; since the 2026-09-14 CLK fallback a code row with "
                 "no satellite code-bias column is no longer among them).  0 written "
                 "under an ARMED trace means the pass never pooled a station — read the "
                 "P1-4 line.  Card OWD: {} written with an empty signal "
                 "(pf_rows_no_signal), {} whose observation signal differs from the first "
                 "labelled column's (pf_rows_sig_relabel); both 0 expected.",
                 ctx.carry.pf_rows_written, ctx.carry.pf_rows_code, ctx.carry.pf_rows_phase,
                 ctx.carry.pf_rows_no_elev, ctx.carry.pf_rows_unattributed,
                 ctx.carry.pf_rows_no_signal, ctx.carry.pf_rows_sig_relabel);
    {
        // The producer's state-manager re-inflations, summed over the stations (card OWD,
        // O2).  The deweight-release hygiene's ionosphere re-inflation is the only producer
        // caller; a kept count names a variance the rule before OWD would have deflated.
        StateManager::ReinflateCounters sum{};
        std::string kept_at;
        for (const auto& st : stations) {
            if (!st.sm) continue;
            const auto& c = st.sm->reinflateCounters();
            long long kept = 0;
            for (std::size_t k = 0; k < sum.size(); ++k) {
                sum[k].raised += c[k].raised;
                sum[k].kept   += c[k].kept;
                kept          += c[k].kept;
            }
            if (kept > 0)
                kept_at += fmt::format("{}{} {}", kept_at.empty() ? "" : ", ", st.name, kept);
        }
        log->info("Network (card OWD re-inflations (producer, {} station(s))): {}; stations "
                  "with a kept re-inflation: {}.", stations.size(),
                  StateManager::reinflateSummary(sum), kept_at.empty() ? "none" : kept_at);
    }
    if (config_.getNetworkConfig().trace_station_block)
        // "0 rows under an armed trace" logs at warn level, not as a clean line
        // (invariant #4).  The two refusal counts distinguish the two ways the sweep finds
        // nothing: no station reached the audit, or the block could not be decomposed.
        log->log(ctx.carry.sb_rows_written > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (§24.10 card ZP station block): {} station-epoch row(s) written to "
                 "network_station_block.csv over {} traced emission(s) (the first 20 EMITTED "
                 "epochs, then every 60th); {} station-epoch(s) skipped (not in the solve / no "
                 "usable assembly) and {} undecomposable (N_rr unavailable or its eigen-"
                 "decomposition did not converge).  0 written under an ARMED trace means no "
                 "epoch ever reached the wire — read the emission counters.",
                 ctx.carry.sb_rows_written, ctx.carry.sb_epochs_written,
                 ctx.carry.sb_stations_skipped, ctx.carry.sb_decomp_failed);
    // Ambiguity-vehicle run-end readout (docs/refound/RF-1_float_ambiguities_design.md
    // §7.2, RF-1).  Emitted only when the vehicle is non-default or the trace is armed,
    // so an unarmed run's stdout is unchanged (structural byte-identity, invariant #6).
    // The mechanism counters (drops, declared S-transforms and the columns they move,
    // joint-block fallbacks) are printed even at 0: a float_kf run with 0 drops is a
    // miswire, not a quiet run (design §7.4 D3; invariant #4).
    if (ctx.knobs.amb_float_kf || config_.getNetworkConfig().trace_amb_state)
        log->info("Network (RF-1 ambiguity vehicle): vehicle=\"{}\"{}, birth_var_floor={} "
                  "m^2, q={} m^2/s, declared_handover={}.  Mechanism counters: {} drop(s) "
                  "over {} epoch(s), {} declared S-transform(s) moving {} column(s), {} "
                  "joint-block fallback(s).",
                  ctx.knobs.amb_float_kf ? "float_kf" : "level",
                  ctx.knobs.pl_blk_from_vehicle
                      ? " — the joint block was STRUCTURAL (step 3)"
                      : " — the zero-q scalar carry",
                  ctx.knobs.amb_birth_var_floor, ctx.knobs.amb_float_q_m2s,
                  ctx.knobs.amb_float_declared_handover,
                  ctx.counters.n_amb_drop, ctx.counters.n_amb_drop_epochs,
                  ctx.counters.n_amb_stransform, ctx.counters.n_amb_stransform_cols,
                  ctx.counters.n_amb_block_fallback);
    // Drop inventory by cause, and the ON-path evidence of the birth floor (RF-1 step 4,
    // §3.4 and §7.4 D3).  A float_kf run with 0 drops is a miswire, not a quiet run
    // (design §7.4; invariant #4), so 0 logs at warn level, with the population that
    // would have been dropped named beside it.
    if (ctx.knobs.amb_float_kf && ctx.knobs.amb_birth_var_floor > 0.0) {
        const auto& C = ctx.counters;
        log->log(C.n_amb_drop > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 step 4 bounded drop): {} drop(s) over {} epoch(s) — {} "
                 "free-epoch latch, {} stale gap, {} T4 attribution, {} T3 consensus; {} "
                 "call(s) found the arc already at or below birth strength.  Birth floor "
                 "{} m^2 at zenith, elevation-mapped at the code weights' power: {} birth "
                 "stash(es) were RAISED by it.  The ERASE survives for the gap prune "
                 "({} entr(ies)) and re-admission — 'this is a different arc'.  0 drops "
                 "under an armed vehicle is a MISWIRE, not a quiet run: compare against "
                 "the {} free-epoch latch(es) and {} inject-site stale skip(s) the "
                 "detectors and the cliff actually raised.",
                 C.n_amb_drop, C.n_amb_drop_epochs, C.n_amb_drop_relevel_free,
                 C.n_amb_drop_stale_gap, C.n_amb_drop_t4, C.n_amb_drop_t3,
                 C.n_amb_drop_idempotent, ctx.knobs.amb_birth_var_floor,
                 C.n_amb_birth_floored, C.n_cr_gap_prune,
                 C.n_relevel_free_skips, C.n_cr_inject_stale);
    }
    // Card CTU section 14: drops at birth strength that decoupled the arc anyway.
    if (ctx.knobs.amb_drop_decouple_always)
        log->info("Network (card CTU / drop decouple): {} drop(s) found the arc already at "
                  "birth strength and still cleared its border regression and block row "
                  "(network.amb_float_drop_decouple_always).",
                  ctx.counters.n_amb_drop_decoupled_idem);
    // Inventory of the drop-only floor and the walk it targets
    // (docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A2, RF-3a).  Emitted in both knob
    // states: a run with the knob off reports the walk it has, which is what gives the
    // armed run's number a meaning (invariant #4).  The two fold buckets separate the
    // correction absorbed by the first fold after a drop from the walk that follows it:
    // a stiff restored prior lets a 40 m level error through a few metres per epoch for
    // tens of epochs, while a floored one takes it in at once.
    if (ctx.knobs.amb_float_kf) {
        const auto& C = ctx.counters;
        auto stat = [](std::vector<double> v) {
            struct { double max; double med; std::size_t n; } s{0.0, 0.0, v.size()};
            if (v.empty()) return s;
            s.max = *std::max_element(v.begin(), v.end());
            const std::size_t h = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(h), v.end());
            s.med = v[h];
            return s;
        };
        const auto w1 = stat(C.amb_walk_fold1_m);
        const auto w2 = stat(C.amb_walk_fold2_m);
        // The two populations of the warm-up gate (RF-3c), printed in both knob states so a
        // run with no gate says so rather than leaving it to be inferred.
        log->info("Network (RF-3c drop-floor warm-up gate): "
                  "network.amb_float_drop_floor_after_ep = {} ({}).  {} drop(s) were taken "
                  "BEFORE the gate opened and restored max(var_birth, var_first, birth "
                  "floor) — the convergence population the floor is deliberately NOT a "
                  "statement about.  {} drop(s) took the floor.",
                  ctx.knobs.amb_drop_floor_after_ep,
                  ctx.knobs.amb_drop_floor_after_ep > 0
                      ? "ARMED — processed epochs, the gross-LOS warmup's abscissa"
                      : "OFF (no gate: the floor applies from epoch 1)",
                  C.n_amb_drop_pre_gate, C.n_amb_drop_floored);
        log->info("Network (RF-3a drop-only floor): network.amb_float_drop_var_m2 = {} m^2 "
                  "at zenith ({}), elevation-mapped at the code weights' power beside the "
                  "birth floor {} m^2.  {} of {} drop(s) had their restored variance come "
                  "FROM the drop floor.  WALK (measure-only, both knob states): the FIRST "
                  "fold after a drop absorbed max {:.3f} m / median {:.3f} m over {} "
                  "measured drop(s); the SECOND fold absorbed max {:.3f} m / median "
                  "{:.3f} m over {}.  A drop floor of 0 with a large second-fold walk is "
                  "the re-coupling edge this card names: the restored prior is too stiff "
                  "for a level the response has just doubted.",
                  ctx.knobs.amb_drop_var_floor,
                  ctx.knobs.amb_drop_var_floor > 0.0 ? "ARMED" : "OFF (today's behaviour)",
                  ctx.knobs.amb_birth_var_floor, C.n_amb_drop_floored, C.n_amb_drop,
                  w1.max, w1.med, w1.n, w2.max, w2.med, w2.n);
    }
    // Inventory of the declared anchor S-transform, with the refused population beside it
    // (RF-1 step 5, §3.6).  The split marks the limit of the step: a cold handover has no
    // Δ, so its family keeps the erase, and how often that happens is a question for the
    // election policy (RF-2).
    if (ctx.knobs.amb_float_kf && ctx.knobs.amb_float_declared_handover) {
        const auto& C = ctx.counters;
        log->log(C.n_amb_stransform > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 step 5 declared anchor S-transform): {} transform(s) "
                 "shifted {} carried level(s) with variances untouched, {} identity "
                 "(Δ = 0, already in the new basis).  REFUSED: {} cold election(s) (no "
                 "carried level for the successor ⇒ no new basis) and {} ledger/carry "
                 "disagreement(s) — those families kept the network-wide ERASE, which is "
                 "the {} carr(ies) the anchor re-reference still destroyed.  Exactness "
                 "self-check FAILED on {} — a transform defect, never noise.  0 "
                 "transforms under an armed knob means every handover this run was cold: "
                 "read the election, not this line.",
                 C.n_amb_stransform, C.n_amb_stransform_cols, C.n_amb_sts_identity,
                 C.n_amb_sts_refused_cold, C.n_amb_sts_refused_no_level,
                 ctx.carry.pa_reref_erase_total, C.n_amb_sts_selfcheck_fail);
    }
    // Joint-block fallback bar (RF-1 step 3, design risk #3): a fallback rate above a
    // small threshold is a run-end failure, not a note.  Under float_kf a non-SPD block
    // returns those station-epochs to the per-column route the vehicle replaces, so a run
    // that fell back on more than 1 % of its attempted station-epochs did not run the
    // vehicle its manifest claims and is not to be graded as if it had.  A failure sets
    // amb_block_rc, which the tail folds into the run's return code.
    if (ctx.knobs.pl_blk_from_vehicle) {
        const long long attempted = ctx.counters.n_pl_blk_blocks
                                  + ctx.counters.n_pl_blk_fallback;
        const double rate = attempted > 0
            ? static_cast<double>(ctx.counters.n_pl_blk_fallback) / static_cast<double>(attempted)
            : 0.0;
        if (attempted == 0) {
            log->error("Network (RF-1 step 3) RUN-END FAILURE: ambiguity_vehicle = float_kf "
                       "but ZERO joint blocks were attempted over the whole run — the fold "
                       "never took the vehicle's route, so this run carries today's "
                       "over-count under a float_kf manifest (standing invariant #4).  Check "
                       "carry_conditional_coupling, the carry warm-up, and whether any "
                       "station ever had an injectable carried column.");
            amb_block_rc = 1;
        } else if (rate > 0.01) {
            log->error("Network (RF-1 step 3) RUN-END FAILURE: the joint local block fell "
                       "back to per-column rows on {} of {} attempted station-epochs "
                       "({:.2f} %) — above the 1 % bar.  A fallback is a VEHICLE CHANGE on "
                       "that station-epoch (design risk #3), so above the bar the arm is a "
                       "MIXTURE of the two vehicles and its delta is not attributable.  Do "
                       "not grade this run.",
                       ctx.counters.n_pl_blk_fallback, attempted, 100.0 * rate);
            amb_block_rc = 1;
        } else {
            log->info("Network (RF-1 step 3): joint local block folded on {} of {} attempted "
                      "station-epochs; {} PD fallback(s) ({:.3f} %, bar 1 %).",
                      ctx.counters.n_pl_blk_blocks, attempted,
                      ctx.counters.n_pl_blk_fallback, 100.0 * rate);
        }
    }
    // Held S-basis run-end readout (RF-2); the counters are the ON-path telemetry
    // (invariant #4).  Four things to check at a glance: the basis was held
    // (n_held ≫ n_elected); every handover was declared; the network-wide erase is
    // retired (pa_reref_erase_total reads 0); and the held property never slipped
    // (value_drift reads 0).  The line logs at error level when either of the last two
    // is nonzero.
    if (ctx.knobs.pd_held_basis) {
        const auto& C = ctx.counters;
        const bool clean = C.n_pb_basis_value_drift == 0
                           && ctx.carry.pa_reref_erase_total == 0;
        log->log(clean ? spdlog::level::info : spdlog::level::err,
                 "Network (RF-2 held S-basis): {} family-epoch(s) re-pinned at their "
                 "FROZEN basis value; {} new basis value(s) elected, {} of them COLD (the "
                 "emitted phase bias stepped).  DECLARED to the consumer: {} handover(s), "
                 "{} cold.  Election gates refused {} candidate(s) as too young (< {} "
                 "epochs) and {} as recently flagged (< {} epochs); {} election(s) moved "
                 "off the coverage rank (elevation order, plus incumbents deposed by a "
                 "flag).  ⚠ Network-wide anchor-reref ERASES: {} (RETIRED — must be 0).  "
                 "⚠ Mid-tenure basis-value drift REFUSED: {} (must be 0 — the held "
                 "property's own alarm).",
                 C.n_pb_basis_held, C.n_pb_basis_elected, C.n_pb_basis_cold,
                 C.n_pb_basis_declared, C.n_pb_basis_declared_cold,
                 C.n_pb_basis_refuse_young, ctx.knobs.pa_min_age_ep,
                 C.n_pb_basis_refuse_flagged, ctx.knobs.pa_flag_quiet_ep,
                 C.n_pb_basis_rank_changed,
                 ctx.carry.pa_reref_erase_total, C.n_pb_basis_value_drift);
        // Why the datum moved (RF-2b); the line above says only how often.  The three
        // deposition causes partition the incumbent losses; `refuse_unusable` and
        // `elev_fallback` are the composed rank's own evidence: a large fallback count
        // means the ledger screen found nothing to prefer, the failure mode seen on the
        // sim bed, visible at run end rather than after a grade.
        log->info("Network (RF-2b election): ledger screen refused {} candidate "
                  "evaluation(s) as not usable AS A DATUM; {} election(s) fell back to "
                  "elevation-only because NO candidate was usable (each a COLD pin).  "
                  "Incumbents deposed: {} by a flag on their OWN basis column, {} by the "
                  "basis arc's death, {} by their own ledger verdict going out of "
                  "tolerance.  A large fallback count IS the mis-specification — the "
                  "rank had nothing usable to prefer.",
                  C.n_pb_basis_refuse_unusable, C.n_pb_basis_elev_fallback,
                  C.n_pb_basis_depose_flag, C.n_pb_basis_depose_death,
                  C.n_pb_basis_depose_unusable);
    }
    if (config_.getNetworkConfig().trace_amb_state)
        // "0 rows under an armed trace" logs at warn level, not as a clean line
        // (invariant #4).  On this vehicle the trace rides the leveling harvest, so 0 rows
        // means the carry never ran (carry_warm never reached, or carry_invariant) rather
        // than a quiet network.  The erase split is reported beside the total because it
        // is the whole of D3's inventory, and because a run with births and trajectory
        // rows but no erase rows over a 60-station field hour would itself be a finding
        // (the four erase paths are the population §3.4 replaces).
        log->log(ctx.counters.n_amb_state_rows > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 §7.4 carry-state trace): {} row(s) written to "
                 "network_amb_state.csv over {} traced epoch(s) — {} carrying an event, "
                 "of which {} are erase: rows (t4_attrib / t3_consensus / gap_prune / "
                 "anchor_reref, each captured before the entry was deleted).  0 rows "
                 "under an ARMED trace means the leveling harvest never ran — read the "
                 "carry warm-up and the vehicle, not this line.  Reads: "
                 "scripts/amb_state_read.py (D1 the variance trajectory, D2 the level "
                 "trajectory, D3 this inventory, D4 σ̂₀(pl) by arc age).",
                 ctx.counters.n_amb_state_rows, ctx.counters.n_amb_state_epochs,
                 ctx.counters.n_amb_state_events, ctx.counters.n_amb_state_erase_rows);
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
        // Emit-layer re-base telemetry (Card 4 (M1) phase D, spec §3): the integer cycles
        // and common-fractional shift removed onto the previous emission frame (an
        // S-selection on outputs only; user DDs are invariant to it).  For the consumer
        // A/B: high integer/fractional churn with a silent disc counter is the datum
        // freedom the carry buys; a disc step that survives is a genuine event.
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
        // smoothed:reset ratio with a small mean posterior σ is the intended result: the
        // per-epoch noise is absorbed into the filter, so the emitter jump test fires (via
        // the reset raw-value steps) only at genuine events.  Ref-path and gap resets are
        // the structural events, always passed through unsmoothed; innovation resets are
        // the calibrated k·σ̂_step genuine steps.  ON-path counters (invariant #4).
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
        // Clock emission re-base run-end telemetry (card CLK-R; the ON-path counters,
        // invariant #4).  Σ|μ| is the common-mode churn removed from the wire (stage 1);
        // marks are the per-satellite re-realizations the wire declares (stage 2);
        // re-anchors count the leash's marked re-coupling edges, expected 0 on a run of
        // 24 h or less (the measured 12 h frame wander is about 23 m against a 50 m leash).
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
        // The same tallies per constellation, when a second one is active (card GAL).
        if (networkMultiGnssActive(config_)) {
            std::string t;
            for (const auto& [c, k] : clk_rebase_engine.constCounters())
                t += fmt::format("{}{}: {} row(s), sum |mu| {:.2f} m, max |A_c| {:.2f} m, "
                                 "{} leash re-anchor(s), {} step mark(s)",
                                 t.empty() ? "" : " | ", c, k.rows, k.sum_abs_mu,
                                 k.max_abs_acc, k.reanchors, k.marks);
            log->info("Network (card GAL, CLOCK FRAME HOLD per constellation): [{}].",
                      t.empty() ? std::string("none") : t);
        }
    }
    // Level re-base run-end readout (card CLV): one line per constellation with the
    // mechanism's counters and the wire.  Silent unless network.clock_level_carry is armed.
    networkClockLevelTeardown(ctx, setup.emitter ? &*setup.emitter : nullptr, log);
    // Card HLT: broadcast health on the network path (silent unless network.sat_health).
    // Standing invariant #4: the counters are the evidence the rule acted.
    if (ctx.knobs.sat_health != 0) {
        const auto& c = ctx.counters;
        long long removed = 0;
        std::string by_sat;
        for (const auto& [sat, n] : setup.sat_health_removed) {
            removed += n;
            by_sat += fmt::format("{}{} {}", by_sat.empty() ? "" : ", ", sat, n);
        }
        log->info("Network (card HLT): sat_health '{}': {} (satellite, epoch) registered clock "
                  "satellite(s) flagged unhealthy; the clock-pivot election passed over one at "
                  "{} (constellation, epoch) and found none healthy at {}; {} observation(s) "
                  "removed by the setup door inside the window{}{}.",
                  ctx.knobs.sat_health == 1 ? "pivot" : "exclude", c.hlt_unhealthy_registered,
                  c.hlt_pivot_skips, c.hlt_pivot_fallbacks, removed,
                  by_sat.empty() ? "" : ": ", by_sat);
        if (ctx.knobs.sat_health == 1 && c.hlt_pivot_skips == 0)
            log->warn("Network (card HLT): sat_health 'pivot' is armed but no pick passed over an "
                      "unhealthy satellite: INERT for this run.");
        if (ctx.knobs.sat_health == 2 && removed == 0)
            log->warn("Network (card HLT): sat_health 'exclude' is armed but the door removed no "
                      "observation inside the window: INERT for this run.");
    }
    if (ctx.knobs.cta_on) {
        // Guarded temporal clock anchor run-end telemetry (card Q3); the ON-path counters
        // are the evidence (invariant #4).  `folded_anchored = 0` means the anchor never
        // bound a single row: the mechanism is inert and every downstream number in the
        // run is the legacy carry, however clean the log reads.  Trips are split by
        // reason because they are not interchangeable evidence: a run that only shows
        // `datum`/`ext` trips has tested neither the detector (nis, abs) nor the
        // staleness guard (gap), and an untested guard is not a guard (invariant #1; the
        // DOY-129 event-window arm tests them).
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
        if (ctx.knobs.cta_space != 0) {
            // Card CTU section 14: the phase-space statistic beside the clock statistic.
            // In shadow mode the clock statistic decided; in phase mode the phase one did.
            std::string wl, el;
            for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
                wl += fmt::format("{}>={:g}:{}", b ? " " : "", kCtaWLadder[b], c.cta_phi_w_ge[b]);
            for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
                el += fmt::format("{}>={:g}:{}", b ? " " : "", kCtaELadder[b], c.cta_phi_e_ge[b]);
            log->info("Network (Card Q3 / phase-space guard, {}): {} anchored row(s) with the "
                      "ionosphere-free phase clock statistic, {} without one (abs only in phase "
                      "mode), {} at the {:.3f} m floor; sigma0_phi {:.3f}; over k on both {}, on "
                      "the CLOCK statistic only {} (the level's motion), on the PHASE statistic "
                      "only {}; {} decouple(s) decided by the phase statistic.  |w_phi| ladder "
                      "[{}]; |e_phi| ladder [{}].",
                      ctx.knobs.cta_space == 2 ? "DECIDING" : "shadow", c.cta_phi_rows,
                      c.cta_phi_unavail, c.cta_phi_floor_rows, ctx.knobs.cta_phi_floor_m,
                      c.cta_phi_rows > 0 ? std::sqrt(c.cta_phi_sum_w2
                                                     / static_cast<double>(c.cta_phi_rows)) : 0.0,
                      c.cta_x_both, c.cta_x_clk_only, c.cta_x_phi_only, c.cta_trip_phi, wl, el);
        }
        if (ctx.knobs.carry_gauge_free) {
            // Denominator of the gauge-free step (§2.1).  `gf_keys = 0` on an armed run
            // means the downdate never reached a group of ≥ 2 carried clocks, so the
            // anchor still constrains the gauge: the failure the step exists to prevent,
            // which only this line shows.
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
        // Only the fault-class trips (nis, abs) are resolve candidates: a gap, datum or
        // ext decouple withdraws the prior because the frame or the visibility moved,
        // emits a fresh solve, and has no anchored value to correct.  Counting those here
        // would report warped epochs on a quiet run with zero fault trips: a composite
        // denominator makes a working run read as a broken one.
        if (const long long fault_trips = c.cta_trip_nis + c.cta_trip_abs;
            ctx.knobs.cta_resolve && c.cta_resolves == 0 && fault_trips > 0)
            log->warn("Network (Card Q3): {} FAULT trip(s) (nis+abs) but ZERO in-epoch "
                      "re-solves — every one was decoupled and marked WITHOUT the same-epoch "
                      "value correction, so each shipped an anchor-warped epoch.  See the "
                      "per-trip warnings for the cause (prior-dominated column, or no "
                      "captured prior to withdraw).", fault_trips);
        // The denominator this run's guard and census standardized by (§2.1).  Two arms
        // of the bisection differ only here, so without this line the pair cannot be told
        // apart in the archived console.
        log->info("Network (Card Q3 §2.1): guard/census denominator — P⁻ {} , P⁺ {} (fold: {}).",
                  ctx.knobs.cta_stat_proj_prior ? "GAUGE-PROJECTED" : "raw per-satellite var_eff",
                  ctx.knobs.cta_stat_proj_post  ? "GAUGE-PROJECTED" : "raw marginal cov(c,c)",
                  ctx.knobs.carry_gauge_free ? "gauge-free" : "raw state");
        // The k derivation of §3, per rung.  The denominator √S shrinks as q tightens, so
        // a fixed k is a different number of metres at every rung; the ladder below shows
        // that without a second disarmed run per rung, and the metre ladder is what the
        // absolute backstop's rail is read from (on the 3 h arm, none of the 63 nis trips
        // would have been caught by the shipped 2.0 m rail).
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
            // The floor binding at the bottom rungs is the design, not a warning
            // condition (§7.4): below q·Δt ≈ (3 mm)² the anchor would otherwise claim
            // precision that no realistic correlated-error floor supports, and deviation
            // #1's consistency cap stops it.  The line reports how often it bound,
            // because "the floor governs" and "the floor is a rounding error" are
            // different postures and only the fraction separates them.
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
    // Clock-pivot handover baseline and the S-transform's ON-path telemetry (queue item
    // 1(b)).  The handover count is reported whether or not the transform is armed:
    // otherwise the edge appears only as a PIVOT event row and in the E/R records, and a
    // run with the transform off could not state the population an armed run acts on
    // (increment 0).  The sts_* clause follows only when the transform is armed.
    // `edges > 0` with `rows_retained == 0` means the INC-1 fold release is miswired: the
    // transform ran and nothing folded (invariant #4).
    if (!ctx.counters.pivot_handovers.empty() || ctx.knobs.handover_stransform) {
        const auto& sc = ctx.counters;
        std::string per_const;
        long long total = 0;
        for (const auto& kv : sc.pivot_handovers) {
            if (!per_const.empty()) per_const += ", ";
            per_const += fmt::format("{}:{}", kv.first, kv.second);
            total += kv.second;
        }
        if (per_const.empty()) per_const = "(none)";
        if (!ctx.knobs.handover_stransform) {
            log->info("Network (clock-pivot handovers): {} datum re-reference(s) over the run "
                      "[{}] — the constellation(s) whose pinned pivot satellite changed.  "
                      "network.handover_stransform is OFF, so each one DROPPED that "
                      "constellation's carried shared priors for one epoch (the free fall "
                      "SPEC_pivot_visibility_gauge_defect §8.2 names).",
                      total, per_const);
        } else {
            log->info("Network (queue 1(b) handover S-TRANSFORM): {} clock-pivot handover(s) "
                      "over the run [{}]; {} edge(s) TRANSFORMED, {} carried row(s) "
                      "re-expressed along the structural clock null direction, {} row(s) "
                      "RETAINED (folded that the baseline skip would have dropped — this is "
                      "the card's ON-path counter), {} edge(s) no-op (the incoming pivot had "
                      "no carried level), {} self-check failure(s).  |c0| max {:.6f} m, last "
                      "{:.6f} m.{}",
                      total, per_const, sc.sts_edges, sc.sts_applied, sc.sts_rows_retained,
                      sc.sts_noop_no_carry, sc.sts_selfcheck_fail,
                      sc.sts_max_abs_c0, sc.sts_last_c0,
                      (sc.sts_edges > 0 && sc.sts_rows_retained == 0)
                          ? "  ⚠ DEAD WIRING: edges were transformed but NOTHING folded "
                            "— the INC-1 skip release is not reaching the fold sites."
                          : "");
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
    // Attempts of the drop-and-heal, which the success-only line above does not show
    // (card PA2).  Gated on attempts, so a run that never reaches the gate-failure path
    // (every config without that failure, including the whole golden matrix) prints
    // nothing new.
    if (ctx.counters.n_heal_attempt_epochs > 0) {
        // The insufficiency caveat is the reason the attempt counters exist, but it holds
        // only when the drop-and-heal fired more often than it recovered the gate.
        // Printing it when attempts == recoveries (every firing recovered, the PA4 emit
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
    if (ctx.knobs.clm_on) {
        // Card CTU level monitor (section 9.3): the ratio of the stashed block's level
        // variance to the code-only bound, per satellite per kept epoch from its second
        // epoch of code on.  A consistent estimator keeps it at or above 1; the control
        // (the card 1d kappa) reads far below 1, the freeze's truth-free fingerprint.
        const auto& c = ctx.counters;
        std::vector<double> r = c.clm_ratios;
        double med = 0.0, p10 = 0.0;
        if (!r.empty()) {
            std::sort(r.begin(), r.end());
            med = r[r.size() / 2];
            p10 = r[r.size() / 10];
        }
        log->info("Network (card CTU / level monitor): {} epoch(s) ({} skipped at a datum "
                  "change); {} satellite-epoch ratio(s) P_L/(1/I_L): min {:.3e}, p10 {:.3e}, "
                  "median {:.3e}, {} below 1 ({:.1f} %); {} satellite-epoch(s) without a carried "
                  "clock (the pivot).  Bound raise {}: {} raise(s) over {} satellite(s), summed "
                  "{:.4e} m^2, largest {:.4e} m^2.",
                  c.clm_epochs, c.clm_skip_datum, c.clm_rows, c.clm_min_ratio, p10, med,
                  c.clm_below1,
                  c.clm_rows > 0 ? 100.0 * static_cast<double>(c.clm_below1)
                                       / static_cast<double>(c.clm_rows) : 0.0,
                  c.clm_no_clock, ctx.knobs.clm_raise ? "ON" : "off", c.clm_raised,
                  c.clm_raised_by_sat.size(), c.clm_raised_sum, c.clm_raised_max);
        if (c.clm_rows == 0)
            log->warn("Network (card CTU): the level monitor is ARMED but counted no ratio: "
                      "no satellite had both code information and a carried level.");
        // The level innovation instrument (card CTU section 14).  A consistent level reads
        // NIS mean ~1, window T ~1 and NEES_tf ~0.6 to 1 (the split-half cross product over
        // the mean prior variance); a frozen or overconfident one reads NEES_tf >> 1.
        {
            std::string nees, rms, tt, phi;
            for (std::size_t w = 0; w < c.clm_win_nees.size(); ++w) {
                const char* sep = w ? " " : "";
                nees += fmt::format("{}{:.2f}", sep, c.clm_win_nees[w]);
                rms  += fmt::format("{}{:.3f}", sep, c.clm_win_rms[w]);
                tt   += fmt::format("{}{:.2f}", sep, c.clm_win_T[w]);
                phi  += fmt::format("{}{:.2f}", sep, c.clm_win_phi[w]);
            }
            log->info("Network (card CTU / level innovation): {} satellite-epoch innovation(s) "
                      "({} left out, the anchor withdrew the prior); NIS mean {:.3f} over {} after "
                      "epoch 15, {:.1f} % above 6.63; per {}-epoch window: truth-free level NEES "
                      "[{}], rms mean innovation [{}] m, mean T [{}] ({:.1f} % of window "
                      "satellites above 6.63), split-half noise ratio [{}].",
                      c.clm_innov_rows, c.clm_innov_tripped,
                      c.clm_innov_nis_n > 0 ? c.clm_innov_nis_sum
                                                  / static_cast<double>(c.clm_innov_nis_n) : 0.0,
                      c.clm_innov_nis_n,
                      c.clm_innov_nis_n > 0 ? 100.0 * static_cast<double>(c.clm_innov_nis_gt)
                                                  / static_cast<double>(c.clm_innov_nis_n) : 0.0,
                      ctx.knobs.clm_innov_window_ep, nees, rms, tt,
                      c.clm_win_Tn > 0 ? 100.0 * static_cast<double>(c.clm_win_Tgt)
                                             / static_cast<double>(c.clm_win_Tn) : 0.0,
                      phi);
        }
    }
    if (ctx.knobs.pv_on) {
        // The phase-visible consistency instrument (card PVN).  An honest covariance of the
        // ionosphere-free phase-visible clock reads NIS mean ~1 and a window ratio ~1 (the
        // rms update over the rms predicted update sd); `rows 0` means the instrument never
        // formed a row (no capture, or no satellite with two phase-bias bands).
        const auto& c = ctx.counters;
        const auto med = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        std::string wn, wr;
        for (std::size_t w = 0; w < c.pv_win_nis.size(); ++w) {
            const char* sep = w ? " " : "";
            wn += fmt::format("{}{:.2f}", sep, c.pv_win_nis[w]);
            wr += fmt::format("{}{:.2f}", sep, c.pv_win_ratio[w]);
        }
        log->info("Network (card PVN / phase-visible consistency): {} epoch(s) ({} without a "
                  "capture), {} satellite-epoch row(s) ({} with S- - S+ not positive); NIS mean "
                  "{:.3f} over {} after epoch 15, {:.1f} % above 6.63; |update| p50 {:.4f} m, "
                  "predicted update sd p50 {:.4f} m, stated posterior sd p50 {:.4f} m; per "
                  "{}-epoch window: mean NIS [{}], rms update / rms predicted sd [{}].",
                  c.pv_epochs, c.pv_epochs_nocap, c.pv_rows, c.pv_su_nonpos,
                  c.pv_nis_n > 0 ? c.pv_nis_sum / static_cast<double>(c.pv_nis_n) : 0.0,
                  c.pv_nis_n,
                  c.pv_nis_n > 0 ? 100.0 * static_cast<double>(c.pv_nis_gt)
                                       / static_cast<double>(c.pv_nis_n) : 0.0,
                  med(c.pv_abs_e), med(c.pv_sqrt_su), med(c.pv_sqrt_sp),
                  ctx.knobs.pv_window_ep, wn, wr);
        if (c.pv_rows == 0)
            log->warn("Network (card PVN): the phase-visible instrument is ARMED but formed no "
                      "row.");
    }
    if (ctx.knobs.carry_cpl_exact) {
        // Card CTU run-end telemetry (invariant 4).  `rows exact = 0` means the rule never
        // ran and every row took card 1d's kappa, however clean the rest reads.  The
        // mirror line is the check that the rows used the inflation the fold applied: a
        // nonzero `set` or `q` count outside handover epochs is a mirror defect.
        const auto& c  = ctx.counters;
        const auto& X  = c.cxu;
        using Rt = CouplingExactDRoute;
        const auto d = [&](Rt r) { return X.terms_d[static_cast<int>(r)]; };
        const auto kd = [&](Rt r) { return c.cxu_keys_d[static_cast<int>(r)]; };
        log->info("Network (card CTU / exact coupling time update): {} epoch(s), {} built, "
                  "{} unfactored, {} without a valid stash; keys {} in S, D {} departed / {} "
                  "reinit / {} reref / {} skipped; {} negative inflation(s) clamped.",
                  c.cxu_epochs, c.cxu_epochs_built, c.cxu_epochs_unfactored,
                  c.cxu_epochs_nostash, c.cxu_keys_s, kd(Rt::Departed), kd(Rt::Reinit),
                  kd(Rt::Reref), kd(Rt::Skipped), c.cxu_q_negative);
        log->info("Network (card CTU): rows {} exact, {} stale (card 1d route), {} on an "
                  "unbuilt epoch; terms {} updated in S, re-attributed {} departed / {} reinit "
                  "/ {} reref / {} skipped, {} pinned, {} scalar outside the block, {} "
                  "marginalized outside, {} SH; {} coefficient(s) written; clock-family "
                  "deficit {} to the pivot, {} spread over present clocks (pivot left), {} ORPHAN; inflation "
                  "mass {:.4e} m^2 ({} clamped); |M - M_new| mass {:.4e}; s_prev mismatches {}.",
                  X.rows_exact, X.rows_stale, X.rows_unfactored, X.terms_s,
                  d(Rt::Departed), d(Rt::Reinit), d(Rt::Reref), d(Rt::Skipped), X.terms_pinned,
                  X.terms_out_scalar, X.terms_out_departed, X.terms_out_sh, X.terms_emitted,
                  X.gauge_held, X.gauge_spread, X.gauge_orphan, X.var_add_sum, X.var_negative,
                  X.shrink_abs_sum, X.sprev_mismatch);
        log->info("Network (card CTU): fold check — {} key(s) checked, {} set mismatch(es), "
                  "{} inflation mismatch(es) (largest |dQ| {:.3e} m^2), {} handover epoch(s) "
                  "the mirror could not see, {} dense-fold diagonal fallback(s), {} epoch(s) "
                  "with no recorded clock pivot.",
                  c.cxu_fold_checked, c.cxu_fold_mismatch_set, c.cxu_fold_mismatch_q,
                  c.cxu_fold_max_dq, c.cxu_fold_handover_epochs, c.cxu_fold_diag_fallback,
                  c.cxu_pivot_missing);
        if (X.rows_exact == 0)
            log->warn("Network (card CTU): network.carry_coupling_exact_update is ARMED but no "
                      "row took the exact route: the card is INERT for this run.");
        if (ctx.knobs.carry_cpl_exact_cross) {
            // Card WSX: the within-station bracket, its reach and its size.
            const auto hist = [&X](int cls) {
                std::string s;
                for (int b = 0; b < CouplingExactCounters::kCrossRhoBins; ++b)
                    s += (b ? " " : "") + std::to_string(X.cross_rho_hist[cls][b]);
                return s;
            };
            log->info("Network (card WSX / within-station bracket): {} block(s), {} pair(s) took "
                      "M_i'BM_k, {} pair(s) skipped (a row off the exact route); |M_i'BM_k| mass "
                      "{:.4e} m^2; |rho| against the pair's diagonal by bin (<0.01 <0.03 <0.1 <0.2 "
                      "<0.3 <0.5 <0.8 >=0.8): ambiguity pairs [{}] max {:.3f}, pairs with a "
                      "receiver code bias [{}] max {:.3f}.",
                      X.cross_blocks, X.cross_pairs, X.cross_pairs_skipped, X.cross_abs_sum,
                      hist(0), X.cross_rho_max[0], hist(1), X.cross_rho_max[1]);
            if (X.cross_pairs == 0)
                log->warn("Network (card WSX): network.carry_coupling_exact_cross is ARMED but no "
                          "pair took the term (no whitened local block, or no two exact rows in "
                          "one): the knob is INERT for this run.");
        }
        if (c.cxu_fold_mismatch_set > 0 || c.cxu_fold_mismatch_q > 0)
            log->warn("Network (card CTU): the fold disagreed with the rows' mirror on {} key "
                      "disposition(s) and {} inflation(s); outside handover epochs this is a "
                      "mirror defect, and those rows were not conditioned on the fold's Q.",
                      c.cxu_fold_mismatch_set, c.cxu_fold_mismatch_q);
    }
    // Gauge-free shared carry readout (card C-GF), placed outside the Card-Q3 `cta_on`
    // block above on purpose: the downdate can be armed by the carry
    // (shared_carry_gauge_free) with the Q3 temporal anchor off, and the Q3 readout would
    // then print nothing, leaving an armed mechanism with no telemetry (invariant #4).
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
    // Realization-step reinit readout (Card 1g), with its own denominator: the run-total
    // departed count moves for many reasons and cannot tell armed-and-working from
    // armed-and-inert.
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
    // Coupling-row datum-gauge projection readout (Card 1f).  The projection acts only on
    // truncated rows, so "0 held / 0 orphan" with departed terms > 0 is a miswire, not a
    // quiet run; the line distinguishes the two (invariant #4).
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
            // Orphan attribution: per-(constellation, kind) totals, so the run-end line
            // names where the gauge-dirty residue lives; per-epoch lines are in the run log
            // ("Card 1f orphan attribution").  There is no recipient fallback, by design:
            // the sums are realized per constellation.
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
        // Carry-variance summary (M2, LS-VCE): the trailing-window measured carry-variance
        // floor σ̂ against the hand floor, per (family, constellation).  ratio > 1 means
        // the hand floor is optimistic (the real carry disagreement exceeds it); ratio < 1
        // means it is conservative.  The per-hour series is in network_carry_vce.csv.
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
        // ON-path telemetry of the carry variance factor (M2 v2,
        // docs/SPEC_carry_vce_apply.md §9.4).  "0 steps" means the wiring is inert however
        // clean the log reads (invariant #4), so the step / freeze split is printed against
        // a denominator, per group, and a run that never left warm-up says so in one line
        // rather than leaving it to be inferred from a missing effect.
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
    // Drop edge of the same gate (card EV7 §6), reported in both knob states
    // (invariant #4): with the knob off this line gives the size of the class, the number
    // the fix is judged against.
    if (ctx.counters.n_pd_gate_drop_edges > 0) {
        if (config_.networkPhaseDatumGateDropErase())
            log->info("Network (EV7 gate-drop erase): {} coverage-gate DROP EDGE(s) — a "
                      "(sat,signal) column removed while it still held an anchor incumbent — "
                      "of which {} handled (family added to the epoch's re-reference set), "
                      "erasing {} carried ambiguity(ies) before assembly.  Unhandled edges "
                      "left their surviving station's CARRIED float to absorb the removed "
                      "bias whole (EV7 §5.3).",
                      ctx.counters.n_pd_gate_drop_edges,
                      ctx.counters.n_pd_gate_drop_handled,
                      ctx.counters.n_pd_gate_drop_arcs);
        else
            log->info("Network (EV7 gate-drop erase): {} coverage-gate DROP EDGE(s) went "
                      "UNHANDLED — network.phase_datum_gate_drop_erase is off, so at each of "
                      "them the surviving station's CARRIED ambiguity absorbed the removed "
                      "phase bias in one epoch against its own prior (EV7 §5.3).  This count "
                      "is the size of the class on this run.",
                      ctx.counters.n_pd_gate_drop_edges);
    }
    // Marker to antenna-reference-point roster frame (card UPO), reported in both knob
    // states (invariant #4).  With the knob off this line gives the size of the modelling
    // error the run carried, the number the fix is judged against; armed, it states that
    // the roster is held at the antenna reference points rather than the published
    // monument coordinates, which a reader of the products has to know.
    if (ctx.counters.n_marker_arp_ecc > 0 || ctx.counters.n_marker_arp_no_record > 0) {
        if (config_.networkStationMarkerToArp())
            log->info("Network (card UPO): marker -> ARP roster reduction ON — {} station(s) "
                      "carried an eccentricity ({} above 0.01 m) and {} were reduced to their "
                      "antenna reference point; largest H {:+.4f} m at '{}'; {} station(s) had "
                      "no header record and were left at their configured coordinate.",
                      ctx.counters.n_marker_arp_ecc, ctx.counters.n_marker_arp_over_1cm,
                      ctx.counters.n_marker_arp_reduced, ctx.counters.marker_arp_max_h,
                      ctx.counters.marker_arp_max_h_station.empty()
                          ? "<none>" : ctx.counters.marker_arp_max_h_station,
                      ctx.counters.n_marker_arp_no_record);
        else
            log->info("Network (card UPO): marker -> ARP roster reduction OFF — {} station(s) "
                      "carried an eccentricity ({} above 0.01 m; largest H {:+.4f} m at '{}') "
                      "and were held at their MONUMENT coordinate for the whole run, while "
                      "their observations refer to the antenna; {} station(s) had no header "
                      "record. This count is the size of the class on this run "
                      "(network.station_marker_to_arp).",
                      ctx.counters.n_marker_arp_ecc, ctx.counters.n_marker_arp_over_1cm,
                      ctx.counters.marker_arp_max_h,
                      ctx.counters.marker_arp_max_h_station.empty()
                          ? "<none>" : ctx.counters.marker_arp_max_h_station,
                      ctx.counters.n_marker_arp_no_record);
    }
    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);

    // The parameter model as armed, run-end tail (card PRM phase 1, section 6): the
    // registry's table again, then the carried instances of each kind at the run's end,
    // counted from the carry maps that hold them.
    {
        std::map<SharedParamKind, long long> nb;
        for (const CarryPrior& cp : ctx.carry.prev_carry) ++nb[cp.kind];
        for (const auto& line : ctx.params.modelTable())
            log->info("Network (param model, run end): {}", line);
        log->info("Network (param model, run end): carried instances: sat_clock {}, "
                  "sat_phase_bias {}, sat_code_bias {}, vtec_sh {}, ambiguity {}, zwd {}, "
                  "slant_iono {} (diagonal) + {} (in block), rx_clock {}, rx_phase_bias {}, "
                  "rx_code_bias {}",
                  nb[SharedParamKind::Clock], nb[SharedParamKind::PhaseBias],
                  nb[SharedParamKind::CodeBias], nb[SharedParamKind::VtecSh],
                  ctx.carry.local_carry.size(), ctx.carry.zwd_carry.size(),
                  ctx.carry.iono_carry.size(), ctx.carry.iono_blk_carry.size(),
                  ctx.carry.rx_clk_carry.size(), ctx.carry.rpb_carry.size(),
                  ctx.carry.rx_cb_carry.size());
        // Card PRM phase 1b: what the fold applied to the carried border over the run, per
        // kind and disposition (key-epochs), decided once before PASS B; the same counts go
        // to the run manifest as a `param_dispositions` record.
        const auto& C = ctx.counters;
        static const char* const kKindName[4] = {"sat_clock", "sat_code_bias", "sat_phase_bias",
                                                 "vtec_sh"};
        std::string js = "{\"record\":\"param_dispositions\",\"decide_epochs\":"
                       + std::to_string(C.prm_decide_epochs) + ",\"kinds\":[";
        for (int kk = 0; kk < 4; ++kk) {
            std::string line;
            std::string jk = fmt::format("{}{{\"kind\":\"{}\",\"departed\":{}", kk ? "," : "",
                                         kKindName[kk], C.prm_departed[kk]);
            long long tot = 0;
            for (int dd = 1; dd < kCarryDispositions; ++dd) {
                const long long n = C.prm_disp[kk][dd];
                jk += fmt::format(",\"{}\":{}", carryDispositionName(static_cast<CarryDisposition>(dd)), n);
                tot += n;
                if (n > 0)
                    line += fmt::format("{}{} {}", line.empty() ? "" : ", ",
                                        carryDispositionName(static_cast<CarryDisposition>(dd)), n);
            }
            js += jk + "}";
            if (tot > 0)
                log->info("Network (param model, run end): {} fold dispositions over the run: {}"
                          " ({} of the folded with the column departed).",
                          kKindName[kk], line, C.prm_departed[kk]);
        }
        js += fmt::format("],\"reexpressed_after_decision\":{},\"reexpressed_epochs\":{},"
                          "\"reexpressed_max_dvar_m2\":{:.6e},\"undecided_at_fold\":{}}}",
                          C.prm_reexpressed, C.prm_reexpressed_epochs,
                          C.prm_reexpressed_max_dvar, C.prm_undecided_at_fold);
        traces.appendRunManifestRecord(config_.getNetworkConfig(), js);
        log->info("Network (card PRM phase 1b): the border's fold decided once before PASS B "
                  "over {} epoch(s); {} key-epoch(s) re-expressed between the decision and the "
                  "fold in {} epoch(s) (largest variance change {:.3e} m^2: a clock-pivot "
                  "handover's S-transform, after the coupling rows were built); {} entr(ies) "
                  "met undecided at the fold.",
                  C.prm_decide_epochs, C.prm_reexpressed, C.prm_reexpressed_epochs,
                  C.prm_reexpressed_max_dvar, C.prm_undecided_at_fold);
        log->info("Network (card PRM phase 1b check): the decision against card CTU's mirror, "
                  "{} key(s) checked, {} mismatch(es); against the fold, {} variance(s) "
                  "checked, {} variance mismatch(es), {} disposition mismatch(es) (bit for "
                  "bit).",
                  C.prm_chk_rows, C.prm_chk_rows_mismatch, C.prm_chk_fold,
                  C.prm_chk_fold_var_mismatch, C.prm_chk_fold_disp_mismatch);
        if (C.prm_chk_rows_mismatch > 0 || C.prm_chk_fold_var_mismatch > 0
            || C.prm_chk_fold_disp_mismatch > 0 || C.prm_undecided_at_fold > 0)
            log->warn("Network (card PRM phase 1b check): the decided-once disposition disagrees "
                      "with the mirror or the fold; the decision is not yet the fold's.");
    }
    // Card LGM: the phase-colour state's edges (invariant #4: the run-end counters are the
    // ON-path telemetry).
    if (ctx.knobs.pc_on) {
        const auto& C = ctx.counters;
        const double n = static_cast<double>(C.n_pc_stash);
        log->info("Network (card LGM): phase-colour state: {} birth row(s), {} carried fold(s) "
                  "through the block, {} by the per-column fallback, {} backstop birth row(s); "
                  "{} entries harvested (rms estimate {:.2f} mm, mean |x| {:.2f} mm), {} harvest "
                  "refusal(s); {} erased by the RAIM attribution, {} by the gross line-of-sight "
                  "screen, {} pruned past {:.0f} s; {} live at the end.",
                  C.n_pc_birth_rows, C.n_pc_block_folds, C.n_pc_fallback_folds,
                  C.n_pc_backstop_rows, C.n_pc_stash,
                  n > 0.0 ? 1e3 * std::sqrt(C.pc_sq_sum / n) : 0.0,
                  n > 0.0 ? 1e3 * C.pc_abs_sum / n : 0.0, C.n_pc_stash_fail,
                  C.n_pc_erase_raim, C.n_pc_erase_gross, C.n_pc_gap_prune,
                  ctx.knobs.pc_max_gap_s, ctx.carry.pc_carry.size());
        if (C.n_pc_stash > 0 && C.n_pc_block_folds + C.n_pc_fallback_folds == 0)
            log->warn("Network (card LGM): the phase-colour state was harvested but never "
                      "folded a carried prior: it acted only through its birth rows, i.e. as "
                      "white noise.  Read the run as the state OFF, not as its test.");
        if (C.n_pc_backstop_rows > 0)
            log->warn("Network (card LGM): {} planned carried phase-colour column(s) did not "
                      "fold (refused by the block builder or the per-column route) and fell "
                      "back to the birth prior.", C.n_pc_backstop_rows);
    }

    // End-of-run manifest record (L15); the body is in NetworkRunTraces (INC-4).
    traces.writeRunManifestComplete(nc, ctx.counters, synced.size(), loop_ms,
                                    networkMultiGnssManifestJson(ctx, config_));

    // --- Serialized-contract round-trip self-check (B6, P1-1) --------------------
    // Close the writer (flush the header), read the file back, and cross-check the parsed
    // S-basis contract against the one serialized.  This checkContract call guards the
    // write → read → consume replay path a product consumer takes: a regression in header
    // serialization or parsing fails here with warnings instead of mis-declaring the
    // datum to a downstream user.
    if (contract_pending && !prod_path.empty()) {
        // Zero epochs emitted: the deferred writer never opened, and the round-trip
        // self-check below is skipped.  The previous run's file was removed at staging
        // unless that removal failed (locked or read-only), in which case the previous
        // run's products still sit at the path; the code checks rather than assumes.
        if (std::filesystem::exists(prod_path))
            log->warn("Network[B6]: no epochs were emitted AND a stale product file from a "
                      "PREVIOUS run remains at '{}' (removal failed at startup) — do NOT "
                      "consume it; it is not this run's output.", prod_path.string());
        else
            log->warn("Network[B6]: no epochs were emitted — the deferred contract writer never "
                      "opened; no product file exists at '{}' (round-trip self-check skipped).",
                      prod_path.string());
        // Record the "writer never opened" outcome, so an absent contract-check CSV means
        // only "the check never ran" (L14).  The contract is on every wire, so the record
        // is unconditional (card OWD).
        {
            std::ofstream cc(log_dir_ / "network_contract_check.csv");
            if (cc) cc << "check,status,detail\n"
                          "overall,FAIL,writer never opened (zero epochs emitted)\n";
        }
    }
    if (product_writer && !prod_path.empty()) {
        // The check runs on every wire and re-reads the whole product file, so its wall
        // time is measured and reported in the line after the verdict (card OWD).
        const auto b6_t0 = std::chrono::steady_clock::now();
        product_writer.reset();   // flush + close so the reader sees the complete header
        SbasisContract parsed;
        bool           rok = false;
        const std::vector<NetworkProductRecord> check_recs =
            NetworkProductReader::read(prod_path.string(), &parsed, &rok);
        // With re-broadcast armed, checkContract runs on every S receipt (each record's
        // contract_refresh), not once: that per-receipt behaviour is what a late-joining
        // consumer relies on (RTA-5).  The per-receipt expectation drops the pivot map
        // (pivots evolve across re-broadcasts; the final pivot map is checked separately
        // below against the last-written contract) but keeps every run-constant convention
        // field and the run's single generation id.  Gated on the toggle, so a run with it
        // off keeps byte-identical check output.
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
        {
            std::error_code sz_ec;
            const auto sz = std::filesystem::file_size(prod_path, sz_ec);
            log->info("Network[B6] (card OWD): the round-trip self-check re-read {} product "
                      "epoch(s) of '{}' ({} bytes) in {:.2f} s.",
                      check_recs.size(), prod_path.filename().string(),
                      sz_ec ? 0ULL : static_cast<unsigned long long>(sz),
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - b6_t0)
                          .count());
        }
        // Machine-readable check result (L14).  The pivot-map round-trip is checked
        // separately (checkContract compares only the expected-side pivots, so a
        // realized-pivot mismatch is not in `mism`) and also fails 'overall'.  Written on
        // PASS as well: an absent file means the check never ran (a crash, for example),
        // which is distinct from a recorded FAIL; zero emitted epochs write their own FAIL
        // row in the branch above.
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
            // Per-receipt contract-check summary across every positional S line in the file
            // (RTA-5; gated on the toggle, so a run with it off keeps a byte-identical CSV).
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

    // Collect the IF-TD reference arm's own opportunities and flags from the per-station
    // detectors (flag-rate tier T1r, §6.14.5 defect 1; invariant #4).  Not gated on
    // nc.slip_qc.ref_gate: the legacy reference condition runs and acts on every
    // lineage, armed or not, so gating this on the hardening toggle would leave the tier
    // reading 0 on the runs where it is the only reference screen.
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

    // Flag-rate empirical-alpha summary (B8) and run-summary CSV (L3); the bodies are in
    // NetworkRunTraces (INC-4), and one trace_flag_rates gate covers both.
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

        // Verdict led by the fix rate.  WL-only AR does not fix the narrow lane, so the
        // post-fix position stays at the WL-float level (metres) even on a good product;
        // the WL fix rate, not the position, is the recovery metric.  A marginal result is
        // flagged with a pointer to the A/B baseline knob, so a degraded run is not read
        // as a clean pass.
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
        // ON-path telemetry of the per-satellite readout (card CST; invariant #4).
        // Printed only when armed, so a run with it off keeps a byte-identical summary.  A
        // run that reports 0 epochs has an instrument that never fired (the consumer
        // deferred every epoch), however clean the log reads.
        if (config_.getNetworkConfig().trace_consumer_sat) {
            log->info("  per-sat consumer trace    : {} rows over {} consumer epoch(s) -> {}",
                      ctx.counters.n_cst_rows, ctx.counters.n_cst_epochs,
                      (log_dir_ / "closed_loop_sat_trace.csv").string());
        }
        // The four declaration classes of the de-commoned phase jump test (card JT), the
        // ON-path telemetry (invariant #4).  Printed only when armed, so a run with it off
        // keeps a byte-identical summary.  `decommon_only` is the population the raw test
        // missed and `raw_only` the population only the raw test declared (suppressed
        // here): 0 in both means the knob is inert on this lineage however clean the run
        // reads.
        if (config_.getNetworkConfig().disc_jump_test_decommon) {
            const auto& k = ctx.counters;
            log->info("  pb jump de-common (JT)    : decl_decommon_only {} (the raw test MISSED "
                      "these) / decl_both {} / raw_only_suppressed {} (the raw test MINTED "
                      "these) / fallback_raw {} (signal population < {}); declared by this test "
                      "{}",
                      k.n_jt_decommon_only, k.n_jt_both, k.n_jt_raw_only, k.n_jt_fallback_raw,
                      NetworkProductEmitter::kDiscDecommonMinSats,
                      k.n_jt_decommon_only + k.n_jt_both);
        }
        // Emit-side phase-bias hold (card PBH): the ON-path telemetry (invariant #4) and
        // both edges (invariant #7), printed only when armed so a run with it off keeps a
        // byte-identical summary.  `held_emissions 0` means the hold never wrote, however
        // clean the wire reads.  The cost of the decoupling edge is the |estimate - held|
        // distribution (what the hold hides) and the coupled-mark clock step (the frame
        // move it does not transfer); the re-coupling edge is the generation starts, each
        // a PB_HOLD/gen_start event the consumer refloats on.
        if (config_.getNetworkConfig().emit_pb_hold) {
            const auto& k = ctx.counters;
            auto        dev = ctx.carry.pb_hold_dev_abs_m;      // copy: run-end readout
            auto        cpl = ctx.carry.pb_hold_coupled_step_m;
            auto q = [](std::vector<double>& v, double f) -> double {
                if (v.empty()) return 0.0;
                const std::size_t i = static_cast<std::size_t>(
                    std::min<double>(f * static_cast<double>(v.size()),
                                     static_cast<double>(v.size()) - 1.0));
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
                return v[i];
            };
            const double d50 = q(dev, 0.50), d90 = q(dev, 0.90);
            const double dmax = dev.empty() ? 0.0 : *std::max_element(dev.begin(), dev.end());
            const double c50 = q(cpl, 0.50), c90 = q(cpl, 0.90);
            const double cmax = cpl.empty() ? 0.0 : *std::max_element(cpl.begin(), cpl.end());
            log->info("  pb hold, emit (PBH)       : keys {} / generations {} (first {} / gap {}, "
                      "max_gap_s {:.1f}) / held emissions {}; |estimate - held| p50 {:.4f} / "
                      "p90 {:.4f} / max {:.4f} m over {} samples (what the hold HIDES)",
                      k.n_pbh_keys, k.n_pbh_gen_first + k.n_pbh_gen_gap, k.n_pbh_gen_first,
                      k.n_pbh_gen_gap, config_.getNetworkConfig().emit_pb_hold_max_gap_s,
                      k.n_pbh_held_emissions, d50, d90, dmax,
                      ctx.carry.pb_hold_dev_abs_m.size());
            log->info("  pb hold, coupled (PBH)    : {} coupled clock+phase-bias mark(s) reached a "
                      "held key; de-commoned satellite clock step at those epochs p50 {:.4f} / "
                      "p90 {:.4f} / max {:.4f} m over {} sample(s) -- the measured cost of NOT "
                      "transferring a coupled frame move",
                      k.n_pbh_coupled_marks, c50, c90, cmax,
                      ctx.carry.pb_hold_coupled_step_m.size());
            // Card LAH (network.emit_pb_hold_phase_clock): the phase-clock mode's ON-path
            // telemetry.  `moved 0` means the mode never wrote.  Under the mode the
            // |estimate - held| line above measures what the hold hides with the level
            // transferred (the phase-visible wander only).
            if (config_.getNetworkConfig().emit_pb_hold_phase_clock
                || config_.getNetworkConfig().emit_pb_hold_smoothed) {
                auto st = ctx.carry.pb_hold_frz_step_abs_m;   // copy: run-end readout
                auto tt = ctx.carry.pb_hold_frz_total_abs_m;
                const double s50 = q(st, 0.50), s90 = q(st, 0.90);
                const double smax = st.empty() ? 0.0 : *std::max_element(st.begin(), st.end());
                const double t50 = q(tt, 0.50), t90 = q(tt, 0.90);
                const double tmax = tt.empty() ? 0.0 : *std::max_element(tt.begin(), tt.end());
                std::string m_final;
                for (const auto& [cc, m] : ctx.carry.pb_hold_frz_M)
                    m_final += fmt::format("{}{} {:.4f}", m_final.empty() ? "" : ", ", cc, m);
                log->info("  pb hold, phase clock (LAH): {} held emission(s) moved by the clock "
                          "transfer (> 1 um), {} on an epoch without the satellite's clock (last "
                          "transfer reused), {} of a generation started without one (held "
                          "plainly); M advanced on {} epoch(s), final per constellation [{}] m; "
                          "|transfer step| p50 {:.4f} / p90 {:.4f} / max {:.4f} m over {} "
                          "sample(s); |transfer| p50 {:.4f} / p90 {:.4f} / max {:.4f} m over {} "
                          "sample(s); {} coupled mark(s) transferred",
                          k.n_pbh_frz_moved, k.n_pbh_frz_noclock, k.n_pbh_frz_noref,
                          k.n_pbh_frz_epochs, m_final, s50, s90, smax,
                          ctx.carry.pb_hold_frz_step_abs_m.size(), t50, t90, tmax,
                          ctx.carry.pb_hold_frz_total_abs_m.size(), k.n_pbh_frz_coupled);
            }
            // Card SMH (network.emit_pb_hold_smoothed): the smoothed hold's ON-path
            // telemetry.  `0 update(s)` means no clone was ever advanced (the mode wrote
            // only the phase-clock fallback).  r reads the passed motion against its
            // stated variance; under a consistent covariance it averages 1 (card SMH
            // section 4).  The phase-clock line above counts the fallback's emissions.
            if (config_.getNetworkConfig().emit_pb_hold_smoothed) {
                auto xa = k.smh_xfer_abs;   // copies: run-end readout
                auto da = k.smh_diff_abs;
                auto pa = k.smh_phi_abs;
                static const char* kDeath[] = {"none", "unfactored", "stale", "parent_outside",
                                               "row_failed", "no_stash", "missed", "unlinked",
                                               "withdrawn", "disengaged", "followed", "pair_lost"};
                static_assert(sizeof(kDeath) / sizeof(kDeath[0])
                                  == static_cast<std::size_t>(PbHoldCloneDeath::kCount),
                              "one name per clone death cause");
                std::string deaths;
                for (int d = 1; d < static_cast<int>(PbHoldCloneDeath::kCount); ++d)
                    deaths += fmt::format("{}{} {}", deaths.empty() ? "" : ", ", kDeath[d],
                                          k.smh_deaths[d]);
                std::string rs;
                std::map<char, std::pair<double, long long>> rc;
                for (const auto& [ch, acc] : k.smh_r) {
                    rs += fmt::format("{}{} h{} {:.3f} ({})", rs.empty() ? "" : ", ", ch.first,
                                      ch.second, acc.second > 0 ? acc.first / acc.second : 0.0,
                                      acc.second);
                    rc[ch.first].first  += acc.first;
                    rc[ch.first].second += acc.second;
                }
                std::string rall;
                for (const auto& [cc, acc] : rc)
                    rall += fmt::format("{}{} {:.3f} ({})", rall.empty() ? "" : ", ", cc,
                                        acc.second > 0 ? acc.first / acc.second : 0.0, acc.second);
                const double xmax = xa.empty() ? 0.0 : *std::max_element(xa.begin(), xa.end());
                const double dmax = da.empty() ? 0.0 : *std::max_element(da.begin(), da.end());
                auto we = k.smh_defer_ep;
                const double wmax = we.empty() ? 0.0 : *std::max_element(we.begin(), we.end());
                log->info("  pb hold, smoothed (SMH), the anchor's engagement (section 9): {} "
                          "birth(s) deferred (wait p50 {:.0f} / max {:.0f} epoch(s)), {} re-birth(s) "
                          "after a disengagement; pending births waited {} stash(es) on the anchor "
                          "and {} on a stashed clock, {} dropped with their generation; {} clone(s) "
                          "killed by a withdrawal and {} by a fold off the anchored variance; {} "
                          "switch(es) to the bridge; {} held emission(s) bridged on the phase-clock "
                          "transfer",
                          k.smh_births_deferred, q(we, 0.50), wmax, k.smh_rebirths,
                          k.smh_defer_loose, k.smh_defer_nostash, k.smh_pending_dropped,
                          k.smh_deaths[static_cast<int>(PbHoldCloneDeath::Withdrawn)],
                          k.smh_deaths[static_cast<int>(PbHoldCloneDeath::Disengaged)],
                          k.smh_bridges, k.smh_held_bridged);
                log->info("  pb hold, smoothed (SMH): {} clone(s) born ({} on a pinned clock); "
                          "{} clone-epoch update(s) (terms {} S / {} D / {} "
                          "outside the block; {} orphaned deficit(s)); deaths [{}]; {} key "
                          "revert(s) to the phase-clock transfer; held emissions {} smoothed / "
                          "{} bridged / {} phase-clock after a revert; {} epoch(s) restricted to the folded keys ({} key(s) unfolded); "
                          "|transfer| p50 {:.4f} / p90 {:.4f} / max {:.4f} m; "
                          "|transfer - phase-clock transfer| p50 {:.4f} / p90 {:.4f} / max {:.4f} "
                          "m over {}; |Phi| p50 {:.4f} / p90 {:.4f} m; r = Phi^2 / (W - V_post) "
                          "mean [{}], by hour [{}]; {} emission(s) with W void (the clock left S)",
                          k.smh_births, k.smh_births_pinned, k.smh_updates,
                          k.smh_terms_s, k.smh_terms_d, k.smh_terms_outside,
                          k.smh_deficit_orphan, deaths, k.smh_reverts, k.smh_held_smoothed,
                          k.smh_held_bridged, k.smh_held_lah, k.smh_epochs_restricted, k.smh_keys_unfolded, q(xa, 0.50), q(xa, 0.90), xmax, q(da, 0.50),
                          q(da, 0.90), dmax, k.smh_diff_abs.size(), q(pa, 0.50), q(pa, 0.90),
                          rall, rs, k.smh_w_invalid);
                if (k.smh_updates == 0)
                    log->warn("Network (card SMH): network.emit_pb_hold_smoothed is ARMED but no "
                              "clone was advanced: every held key wrote the phase-clock transfer.");
                // Card SMD: the block clones (carried whenever the smoothed hold is armed).
                // `0 update(s)` under network.emit_pb_hold_smoothed_block means the wire was the
                // phase-clock transfer throughout.
                {
                    auto pa = k.smh_pv_phi_abs;   // copies: run-end readout
                    auto la = k.smh_pv_lvl_abs;
                    std::string pdeaths;
                    for (int d = 1; d < static_cast<int>(PbHoldCloneDeath::kCount); ++d)
                        pdeaths += fmt::format("{}{} {}", pdeaths.empty() ? "" : ", ", kDeath[d],
                                               k.smh_pv_deaths[d]);
                    std::string pairs;
                    for (const auto& [nm, n] : k.smh_pv_pairs)
                        pairs += fmt::format("{}{} {}", pairs.empty() ? "" : ", ", nm, n);
                    std::string rds;
                    for (const auto& [ch, acc] : k.smh_pv_r)
                        rds += fmt::format("{}{} h{} {:.3f} ({})", rds.empty() ? "" : ", ", ch.first,
                                           ch.second, acc.second > 0 ? acc.first / acc.second : 0.0,
                                           acc.second);
                    std::string ush;
                    for (const auto& [h, n] : k.smh_pv_steps_unmarked_hour)
                        ush += fmt::format("{}h{} {}", ush.empty() ? "" : ", ", h, n);
                    const auto fol = [&k](PbHoldFollow f) { return k.smh_pv_follow[static_cast<int>(f)]; };
                    log->info("  pb hold, smoothed, block clones (SMD){}: {} born ({} on a pinned clock, "
                              "{} again within a generation); pending births waited {} stash(es) on the "
                              "anchor, {} on a stashed clock, {} for phase biases on two bands, {} "
                              "dropped with their generation; pairs [{}]; {} update(s); deaths [{}]; "
                              "steps followed: {} re-reference(s), {} new anchor tenure(s), {} emitter "
                              "mark(s); {} re-reference(s) with no phase-channel mark; {} unmarked "
                              "step(s) of D above the pair's smallest emitter bar (largest {:.4f} m; by "
                              "hour [{}]); {} held emission(s) "
                              "written; |Phi_D| p50 {:.4f} / p90 {:.4f} m, level part |Phi_c - Phi_D| "
                              "p50 {:.4f} / p90 {:.4f} m over {}; r_D by hour [{}]",
                              config_.getNetworkConfig().emit_pb_hold_smoothed_block ? ", the wire"
                                                                                    : ", instrument",
                              k.smh_pv_births + k.smh_pv_births_pinned, k.smh_pv_births_pinned,
                              k.smh_pv_rebirths, k.smh_pv_defer_anchor, k.smh_pv_defer_nostash,
                              k.smh_pv_defer_nopair, k.smh_pv_pending_dropped, pairs,
                              k.smh_pv_updates, pdeaths, fol(PbHoldFollow::Reref),
                              fol(PbHoldFollow::Tenure), fol(PbHoldFollow::Mark),
                              k.smh_pv_reref_unmarked, k.smh_pv_steps_unmarked,
                              k.smh_pv_step_unmarked_max, ush, k.smh_pv_held, q(pa, 0.50), q(pa, 0.90),
                              q(la, 0.50), q(la, 0.90), k.smh_pv_lvl_abs.size(), rds);
                    if (config_.getNetworkConfig().emit_pb_hold_smoothed_block && k.smh_pv_updates == 0)
                        log->warn("Network (card SMD): network.emit_pb_hold_smoothed_block is ARMED "
                                  "but no block clone was advanced: every held key wrote the "
                                  "phase-clock transfer.");
                }
            }
        }
        // Per-channel mark scope (card MS): the ON-path telemetry (invariant #4).  Printed
        // only when armed, so a `sat`-scope run keeps a byte-identical summary.
        // `suppressed` is the knob's effect: 0 there means the scope narrowed nothing,
        // however clean the run reads.
        if (config_.getNetworkConfig().disc_mark_scope == "channel") {
            const auto& k = ctx.counters;
            log->info("  disc mark scope (MS)      : applied n_mark_clk {} / n_mark_cb {} / "
                      "n_mark_pb {}; n_mark_suppressed_by_scope {} (clk {} / cb {} / pb {})",
                      k.n_mark_clk, k.n_mark_cb, k.n_mark_pb,
                      k.n_mark_suppressed_by_scope, k.n_mark_suppressed_by_scope_clk,
                      k.n_mark_suppressed_by_scope_cb, k.n_mark_suppressed_by_scope_pb);
        }
        // Declared-step carry counters (card CC (A)), the consumer's ON-path telemetry
        // (invariant #4).  Printed only when the knob is armed, so a run with it off keeps
        // a byte-identical summary.  Closure: carried + resets == events.
        if (consumer->pbDiscCarryArmed()) {
            const auto& c = consumer->pbDiscCarryCounters();
            log->info("  pb-disc carry (card CC)   : events {} (disc bumps {} + freshness flips {}) "
                      "-> pb_disc_carried {} / pb_flip_carried {}; pb_disc_reset_first_sight {} / "
                      "pb_disc_reset_basis {} / pb_disc_reset_bssd {} / "
                      "pb_disc_reset_unknown_key {}; closure {}",
                      c.events, c.disc_bumps, c.flips, c.disc_carried, c.flip_carried,
                      c.reset_first_sight, c.reset_basis, c.reset_bssd, c.reset_unknown_key,
                      c.closureHolds() ? "HOLDS" : "BROKEN");
        }
        // Every-step translation counters (card PBT (A)), the consumer's ON-path telemetry
        // (invariant #4), printed only when the knob is armed so a run with it off keeps a
        // byte-identical summary.  Both edges: the decoupling edge is what the ambiguity
        // absorbed (declared + undeclared, with Sigma|delta|), the re-coupling edge is
        // every cell it refused or reset, and the declared-but-empty count is the
        // population card MS removes.
        if (consumer->pbStepTranslateArmed()) {
            const auto& t = consumer->pbStepTranslateCounters();
            log->info("  pb step translate (PBT)   : cells {} (declared {}) -> "
                      "n_pb_translate_declared {} / n_pb_translate_undeclared {}; "
                      "sum_abs_translate_m {:.3f} (max |d| {:.3f} m); "
                      "n_pb_translate_skipped_eps {} (declared-but-EMPTY {}); "
                      "declared resets first_sight {} / basis {} / bssd {} / unknown_key {}; "
                      "undeclared refused {}; no_pair {}; closure {}",
                      t.cells, t.declared_seen, t.translate_declared, t.translate_undeclared,
                      t.sum_abs_translate_m, t.max_abs_translate_m, t.skipped_eps,
                      t.skipped_eps_declared, t.reset_first_sight, t.reset_basis,
                      t.reset_bssd, t.reset_unknown_key, t.refused_undeclared, t.no_pair,
                      t.closureHolds() ? "HOLDS" : "BROKEN");
        }
        // Ambiguity level check counters (card ALC), the consumer's ON-path telemetry at
        // the re-coupling edge of invariant #7, printed only when the knob is armed so a
        // run with it off keeps a byte-identical summary.  The two sets of totals are read
        // from different places on purpose: the left-hand ones are the engine's own ledger
        // and the right-hand ones are what the harness drained into network_events.csv, so
        // a mismatch means an epoch's events were lost between the two (which the absence
        // of a crash would not show).
        if (consumer->ambLevelCheckArmed()) {
            const auto& a = consumer->ambLevelCheckCounters();
            const auto& k = ctx.counters;
            const bool agree = (static_cast<long long>(a.checks)   == k.n_amb_level_checks)
                            && (static_cast<long long>(a.trips_wl) == k.n_amb_level_trips_wl)
                            && (static_cast<long long>(a.trips_if) == k.n_amb_level_trips_if)
                            && (static_cast<long long>(a.refloats) == k.n_amb_level_refloats);
            log->info("  amb level check (ALC)     : mode '{}' window {} ep k {:.2f}; sats {} "
                      "/ samples {} / window restarts {} -> n_amb_level_checks {}; "
                      "n_amb_level_trips_wl {} / n_amb_level_trips_if {}; "
                      "n_amb_level_refloats {}; drained {}/{}/{}/{} -> {}",
                      config_.getAmbLevelCheckMode(), config_.getAmbLevelCheckWindowEp(),
                      config_.getAmbLevelCheckK(), a.sats, a.samples, a.restarts,
                      a.checks, a.trips_wl, a.trips_if, a.refloats,
                      k.n_amb_level_checks, k.n_amb_level_trips_wl,
                      k.n_amb_level_trips_if, k.n_amb_level_refloats,
                      agree ? "AGREE" : "MISMATCH");
        }
        // The consumer's half of the hold contract (card PBH).  Printed only when a held
        // entry was seen, so an unheld run keeps a byte-identical summary.  With the
        // producer's hold armed, the absence of this line (cells 0) means the generation
        // never reached the store, a failure a clean log would not otherwise show.
        {
            const auto& p = consumer->pbHoldCounters();
            if (p.cells > 0)
                log->info("  pb hold, consumer (PBH)   : cells {} over {} held key(s) -> "
                          "first_sight {} / held {} / refloats {}; disc-counter changes "
                          "IGNORED {} (no value step under a hold); closure {}",
                          p.cells, p.keys, p.first_sight, p.held, p.refloats, p.disc_ignored,
                          p.closureHolds() ? "HOLDS" : "BROKEN");
        }
        // The consumer's half of the declaration (SID phase 5): the latch against the
        // declared roster and the attributed freshness flips.  The harness never calls
        // RealTimeEngine::flush(), so the census is printed here; silent on a wire that
        // carries no declaration.
        consumer->logRosterConsumerSummary("closed loop");
        consumer->logPbHoldPresenceSummary("closed loop");   // held-key presence edges, silent unless armed (card PBF)
        consumer->logReinflateSummary("closed loop");   // consumer re-inflation summary (card OWD, O2)
        // Consumer snapshots written beside the network history copies (card CC (B));
        // 0 written on an armed seed means no warm start is on offer.
        if (cl.state_snapshot)
            log->info("  consumer snapshots (CC-B) : {} written beside network history copies "
                      "(matching tow), {} skipped (consumer had not processed that tow)",
                      ctx.counters.cl_snap_written, ctx.counters.cl_snap_skipped);
        log->info("==============================================================");
    }
    // Event harness run-end (filter/NetworkInject.cpp): the clause, and the hard error
    // for a schedule that never fired.  Read before the zero-emit guard so an armed but
    // silent schedule is reported even on a run that also produced nothing.
    const int inject_rc = networkInjectRunEnd(ctx, log);
    // Canonical-pair admission run-end (card CPA,
    // filter/NetworkCanonicalPairAdmission.cpp): the withheld census and both edges, in
    // one line.  Silent when the rule is disarmed; the ARMED AND INERT clause covers an
    // armed run whose counters never moved (invariant #4).  Read before the zero-emit
    // guard for the same reason as the injector's clause: a rule that withheld the whole
    // network has to be visible on a run that also produced nothing.
    networkCanonicalPairRunEnd(ctx, log);
    // Signal-roster observatory run-end (SID phase 1, app/SignalRoster.cpp): the roster's
    // build census with its bar-0 self-check, every disagreement class with its
    // denominator, and the arc ledger's transitions, horizon and cost.  Silent when the
    // knob is disarmed.  Read here, before the zero-emit guard, for the reason the two
    // clauses above give: the observatory's table matters most on a run that produced
    // nothing.
    networkSignalRosterRunEnd(ctx, log);
    // All-channel family census and cross-channel exposure (SID phase 6b), beside the
    // roster's table because the two answer the same question at different
    // granularities.  Returns on its first line when the widening is not active.
    networkChannelAllRunEnd(ctx, config_, log,
                            slip_qc.counters.gap_reentry_extra_channel);
    // Second-constellation lines (card GAL): the inter-system bias; per constellation,
    // the clock S-basis, the geometry-free datum and the clock anchor; and the family
    // census on a single-channel posture.  Silent unless a constellation other than GPS
    // is active.
    networkMultiGnssRunEnd(ctx, config_, log);
    // Ledger-driven emission run-end (SID phase 5a, filter/NetworkLedgerEmit.cpp): the
    // emitter's bookkeeping against the ledger, both generation classes with their
    // denominators, the first-sighting split, and the declared roster's wire census.
    // Silent when the knob is disarmed; read here for the reason the clauses above give.
    networkLedgerEmitRunEnd(ctx, log);

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
    // The PD-fallback bar is a run-end failure, not a note (RF-1 step 3, design risk #3).
    // Folded last, so a run that also produced zero epochs reports that first.
    return inject_rc != 0 ? inject_rc : amb_block_rc;
}

} // namespace GnssProcessor
