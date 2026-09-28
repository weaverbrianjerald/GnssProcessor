#pragma once
/**
 * @file NetworkWlRunTypes.hpp
 * @brief Run-loop-local helper types taken out of Application::runNetwork so the
 *        solve-region seams (NetworkInstallCarry / NetworkDdPriorBuild /
 *        NetworkSolveGlue) can name them: EwlLane (owns a NetworkMwWlLayer), the
 *        WL/UWL counter, inject, lane-trace and F6-capture PODs, and the
 *        Eigen-thread-pin RAII.  The definitions are field-for-field the former
 *        runNetwork() ones (INC-9b); the run TU keeps the instances (ewl_lanes /
 *        wl_inject_epoch / mw_wl_f6 / ...) and includes this header.
 *
 * CarryEigenScope is a general Eigen-threading RAII, not WL-specific; it is here
 * because it was also a runNetwork()-local type that the seams need.
 */

#include "filter/NetworkMwWlLayer.hpp"   // EwlLane: NetworkMwWlLayer (by value) + ::Counters
#include "filter/NetworkZStore.hpp"      // ArcKey (WlInjectPlan / LaneTraceEntry chords)

#include <Eigen/Core>                    // CarryEigenScope: nbThreads / setNbThreads

#include <cstddef>
#include <string>
#include <vector>

namespace GnssProcessor {

/// (rx app index, satellite, signal) identity of an (est_station,col) ambiguity
/// column: the value type of the WL machinery's reverse map.  Shared by
/// networkBuildCarryDdPriors and the NetworkMwWlGlue seam (INC-13).
struct Rev { int rx; std::string sat; std::string signal; };

/// The estimator primary pair per (rx,sat): the two lowest distinct bands
/// (ties broken lexicographically).  Shared like Rev (INC-13).
struct PP { std::string sig1, sig2; int col1{ -1 }, col2{ -1 }; bool ok{ false }; };

struct WlCounters {
    long long rows_seen{0};          ///< FFR-fixed EWL rows inspected by the bridge.
    long long cycles_decided{0};     ///< bridge cycles that passed the WL FFR gate.
    long long holds_folded{0};       ///< hold rows folded (carry already lane-consistent).
    long long injects{0};            ///< injects applied post-solve (carry moved to the integer).
    double    inject_abs_sum{0.0};   ///< Σ |w − carried lane| at inject; inject_abs_sum / injects is the mean |Δ|.
    long long skip_bridge_missing{0};///< an arc lacked a mature GF_b bridge arc.
    long long skip_no_iono{0};       ///< an arc lacked a slant-iono estimate, so the bridge cannot be de-biased (§3.4 v2).
    long long skip_bridge_wide{0};   ///< bridge σ_row (incl. iono variance) > bridge_max_sigma_cyc.
    long long skip_not_fixed{0};     ///< bridge FFR failed (σ or |frac|).
    long long skip_closure{0};       ///< MW(1,2) cross-check disagreed with the bridge integer.
    long long skip_quarantine{0};    ///< an arc's |x_lin| exceeded lane_max_abs_xlin_m.
    long long skip_basis{0};         ///< inject skipped: the chord is not 1:1 across both bands' carry groups, or the carry refused the edit.
    double    sum_bridge_R{0.0};     ///< Σ decided rows' bridge R (cyc²) for the mean-R report.
    // --- Lane-carry telemetry (WL-3; SPEC_lane_carry_card.md §3.2/§3.3). ---
    long long lane_mw_rows{0};       ///< per-epoch MW(1,2) lane increments folded into pair blocks.
    long long lane_bridge_rows{0};   ///< per-epoch bridge lane increments folded (EWL-fixed cycles).
    long long lane_fix_folded{0};    ///< fold-on-change FFR fixes folded (decision new or changed).
    long long lane_fix_unchanged{0}; ///< FFR decisions seen with an unchanged integer; not folded, since each decision folds once (F-A).
    long long lane_skip_meas{0};     ///< lane-increment cycles skipped (immature/stale/mixed-frame/slipped arc/quarantine).
    long long lane_pinned_skips{0};  ///< MW/bridge increments skipped because the chord's lane is committed-pinned (laneIncrementSkippable); 0 means the skip never fired (invariant #4).  (LF-1 INC-2)
    // --- Cold-fix telemetry (UWL-1; SPEC_uwl_card.md §3.3). ---
    long long cold_bridge_cycles{0}; ///< decided cycles whose bridge read was the unlagged cold serve (early re-pin after a slip).
    // --- Mid-ladder hop telemetry (E6; uwl_mid_enable). ---
    long long mid_decided{0};        ///< decided cycles whose WL statistic came through the two-hop mid chain.
    long long mid_fallback{0};       ///< cycles with a live mid chain whose mid FFR failed; fell back to the direct hop.
    long long mid_closure{0};        ///< mid integers blocked by the MW(6,5) closure; fell back to the direct hop.
    long long mid_disagree{0};       ///< two-hop and direct integers both pass FFR but differ; cycle skipped (wrong-integer backstop).
    // --- Dual-top tier telemetry (Increment 5; uwl_dual_top). ---
    long long top2_decided{0};       ///< decided cycles that came through the fallback rung ('7'), i.e. rows the preferred '8' rung did not decide.
    long long dual_dedupe{0};        ///< fallback-rung entries skipped because the preferred rung already decided the (block,row) this build.
    long long sib_checked{0};        ///< decided cycles whose untaken-sibling MW statistic was measurable across every arc.
    long long sib_unavail{0};        ///< decided cycles whose sibling statistic was not measurable (an arc lacks the sibling band / MW arc).
    long long sib_int_off{0};        ///< checked cycles whose sibling residual rounds to a nonzero integer: a wrong integer or a convention offset (the counter does not tell which).
    long long sib_blocked{0};        ///< decisions blocked by the armed sibling gate (uwl_sib_gate; the cycle falls to the fallback rung the same epoch).
    double    sib_abs_frac_sum{0.0}; ///< Σ |frac(sibling residual)| over checked cycles; the mean shows a convention offset (about 0 clean, about 0.5 for a half-cycle convention).
};

struct WlInjectPlan {
    int    group_l1{-1}, group_l2{-1};
    ArcKey chord{0, 0};
    double w{0.0};                 ///< integer WL DD target ∇ΔN_WL (cyc).
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lambda1{0.0}, lambda2{0.0};
    bool   pair{false};            ///< group_l1 (== group_l2) is a pair group, read and edited through the pair accessors (the fallback path; about 0 in steady state).  (WL-3)
    char   cons{'\0'};             ///< the chord's constellation, for per-cons inject telemetry (UWL-1).
};

struct LaneTraceEntry {
    ArcKey chord{0, 0};
    int    g1{-1}, g2{-1};          ///< the two bands' carry group ids.
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lam1{0.0}, lam2{0.0};
    std::string sat;                ///< chord satellite (chord.u = station index).
};

struct EwlLane {
    char cons;                        ///< cons filter (0 = all, the legacy single lane).
    char top;                         ///< the instance's top-tier band digit ('\0' = legacy single lane) (UWL-1).
    NetworkMwWlLayer layer;
    NetworkMwWlLayer::Counters run, epoch;
};

struct F6Arc {
    int         est_station{-1};
    int         col1{-1}, col2{-1};
    std::string sat, sig1, sig2;
    double      coeff{0.0};       // signed cycle coefficient c (±1 for a plain DD)
    double      mw_mean_m{0.0};   // ZD-consistent arc MW mean (m)
    double      mw_var_m2{0.0};
    long long   mw_count{0};
    bool        bia_corrected{false};
    double      x_lin1{0.0}, x_lin2{0.0};              // amb linearization values (m)
    double      lambda1{0.0}, lambda2{0.0}, lambda_wl{0.0};
};

struct F6Row {
    std::vector<F6Arc> arcs;
    double z_mw{0.0}, z_lin{0.0}, z_corr{0.0}, zhat_block{0.0};
};

struct F6Capture {
    bool                 pending{false};   // captured this epoch, awaiting the post-solve write
    bool                 written{false};   // one-shot latch (whole run)
    double               week{0.0}, tow{0.0};
    int                  loop_epoch{-1};
    std::size_t          wl_block_index{0};   ///< index of the first WL block in `blocks`.
    std::vector<F6Row>   rows;
    /// Parallel to `rows`: the `blocks` index of each row's WL block.  Under
    /// f6_all_blocks the capture spans several blocks, and the kernel innovation must be
    /// requested per block (ddPriorInnovation is block-scoped), so the write site groups by
    /// this.  Single-block captures leave every entry == wl_block_index.
    std::vector<std::size_t> row_block;
};

struct CarryEigenScope {
    bool active;
    int  saved;
    CarryEigenScope(bool on, int nthreads) : active(on), saved(Eigen::nbThreads()) {
        if (active) Eigen::setNbThreads(nthreads);
    }
    ~CarryEigenScope() { if (active) Eigen::setNbThreads(saved); }
    CarryEigenScope(const CarryEigenScope&) = delete;
    CarryEigenScope& operator=(const CarryEigenScope&) = delete;
};


// SubBlockTimer: per-sub-block timing accumulator (run-cumulative wall against timed
// sections), D0b.  Kept here, like CarryEigenScope, as a former runNetwork()-local type
// that the NetworkTeardown readout seam needs; the run TU keeps the sec_sub instances.
struct SubBlockTimer {
    double      wall_ms{0.0};    // driver wall time bracketing the block (run cumulative)
    double      timed_ms{0.0};   // Δ(reduce+solve+cov-inv+cov-consumers) inside it (subtract for net)
    double      covcon_ms{0.0};  // cov-consumers share of timed_ms, reported separately (item D1)
    std::size_t covcalls{0};     // cov-consumer calls inside it
};
} // namespace GnssProcessor
