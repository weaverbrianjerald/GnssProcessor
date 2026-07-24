#pragma once
/**
 * @file NetworkWlRunTypes.hpp
 * @brief Run-loop-local helper types promoted out of Application::runNetwork so the
 *        decomposed SOLVE-REGION seams (NetworkInstallCarry / NetworkDdPriorBuild /
 *        NetworkSolveGlue) can name them.  These were the INC-2-deferred
 *        function-local structs (EwlLane owns a NetworkMwWlLayer; the WL/UWL counter
 *        + inject + lane-trace + F6-capture PODs; and the Eigen-thread-pin RAII).
 *        Hoisted VERBATIM at decomposition INC-9b (2026-07-22) — field-for-field
 *        byte-identical to the former runNetwork() definitions; the run TU keeps the
 *        instances (ewl_lanes / wl_inject_epoch / mw_wl_f6 / ...) and includes this.
 *
 * NOTE: CarryEigenScope is a general Eigen-threading RAII (not WL-specific); it is
 * colocated here because it, too, was a runNetwork()-local type the seams now need.
 */

#include "filter/NetworkMwWlLayer.hpp"   // EwlLane: NetworkMwWlLayer (by value) + ::Counters
#include "filter/NetworkZStore.hpp"      // ArcKey (WlInjectPlan / LaneTraceEntry chords)

#include <Eigen/Core>                    // CarryEigenScope: nbThreads / setNbThreads

#include <cstddef>
#include <string>
#include <vector>

namespace GnssProcessor {

/// (rx app index, satellite, signal) identity of an (est_station,col) ambiguity
/// column — the WL machinery's reverse map value type.  Hoisted out of
/// networkBuildCarryDdPriors at INC-13 so the NetworkMwWlGlue seam can name it.
struct Rev { int rx; std::string sat; std::string signal; };

/// The estimator primary pair per (rx,sat): the two lowest DISTINCT bands
/// (ties lexicographic).  Hoisted out of networkBuildCarryDdPriors at INC-13.
struct PP { std::string sig1, sig2; int col1{ -1 }, col2{ -1 }; bool ok{ false }; };

struct WlCounters {
    long long rows_seen{0};          ///< FFR-fixed EWL rows inspected by the bridge.
    long long cycles_decided{0};     ///< bridge cycles that passed the WL FFR gate.
    long long holds_folded{0};       ///< hold rows folded (carry already lane-consistent).
    long long injects{0};            ///< injects applied post-solve (carry moved to the integer).
    double    inject_abs_sum{0.0};   ///< Σ |w − carried lane| at inject (median-ish |Δ| via /injects).
    long long skip_bridge_missing{0};///< an arc lacked a mature GF_b bridge arc.
    long long skip_no_iono{0};       ///< §3.4 v2: an arc lacked a slant-iono estimate ⇒ can't de-bias.
    long long skip_bridge_wide{0};   ///< bridge σ_row (incl. iono variance) > bridge_max_sigma_cyc.
    long long skip_not_fixed{0};     ///< bridge FFR failed (σ or |frac|).
    long long skip_closure{0};       ///< MW(1,2) cross-check disagreed with the bridge integer.
    long long skip_quarantine{0};    ///< an arc's |x_lin| exceeded lane_max_abs_xlin_m.
    long long skip_basis{0};         ///< inject: the chord is not 1:1 across BOTH bands' carry groups.
    double    sum_bridge_R{0.0};     ///< Σ decided rows' bridge R (cyc²) for the mean-R report.
    // --- WL-3 lane-carry telemetry (SPEC_lane_carry_card.md §3.2/§3.3). ---
    long long lane_mw_rows{0};       ///< per-epoch MW(1,2) lane increments folded into pair blocks.
    long long lane_bridge_rows{0};   ///< per-epoch bridge lane increments folded (EWL-fixed cycles).
    long long lane_fix_folded{0};    ///< fold-on-change FFR fixes FOLDED (decision new/changed).
    long long lane_fix_unchanged{0}; ///< FFR decisions seen with an unchanged integer (no fold — the F-A once-only discipline).
    long long lane_skip_meas{0};     ///< lane-increment cycles skipped (immature/stale/mixed-frame/slipped arc/quarantine).
    long long lane_pinned_skips{0};  ///< LF-1 INC-2: MW/bridge increments skipped because the chord's lane is committed-pinned (laneIncrementSkippable).  0 ⇒ the lever is inert (invariant #4).
    // --- UWL-1 cold-fix telemetry (SPEC_uwl_card.md §3.3). ---
    long long cold_bridge_cycles{0}; ///< decided cycles whose bridge read was the UNLAGGED cold serve (post-slip early re-pin).
    // --- E6 mid-ladder hop telemetry (uwl_mid_enable). ---
    long long mid_decided{0};        ///< decided cycles whose WL statistic came through the two-hop mid chain.
    long long mid_fallback{0};       ///< cycles with a live mid chain whose mid FFR failed ⇒ fell back to the direct hop.
    long long mid_closure{0};        ///< mid integers blocked by the MW(6,5) closure ⇒ fell back to the direct hop.
    long long mid_disagree{0};       ///< two-hop and direct integers BOTH FFR-credible yet different ⇒ cycle skipped (wrong-integer backstop).
    // --- Increment 5: dual-top tier telemetry (uwl_dual_top). ---
    long long top2_decided{0};       ///< decided cycles that came through the FALLBACK rung ('7' — the preferred '8' could not decide them).
    long long dual_dedupe{0};        ///< fallback-rung entries skipped because the preferred rung already decided the (block,row) this build.
    long long sib_checked{0};        ///< decided cycles whose untaken-sibling MW statistic was measurable across every arc.
    long long sib_unavail{0};        ///< decided cycles whose sibling statistic was NOT measurable (an arc lacks the sibling band / MW arc).
    long long sib_int_off{0};        ///< checked cycles whose sibling residual rounds to a NONZERO integer (wrong-integer signal — or a convention offset; measure first).
    long long sib_blocked{0};        ///< decisions blocked by the armed sibling gate (uwl_sib_gate; the cycle falls to the fallback rung the same epoch).
    double    sib_abs_frac_sum{0.0}; ///< Σ |frac(sibling residual)| over checked cycles (mean = the convention-offset dial: ~0 clean, ~0.5 half-cycle convention).
};

struct WlInjectPlan {
    int    group_l1{-1}, group_l2{-1};
    ArcKey chord{0, 0};
    double w{0.0};                 ///< integer WL DD target ∇ΔN_WL (cyc).
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lambda1{0.0}, lambda2{0.0};
    bool   pair{false};            ///< WL-3: group_l1 (== group_l2) is a PAIR group — read/edit via the pair accessors (the fallback path; ~0 in steady state).
    char   cons{'\0'};             ///< UWL-1: the chord's constellation (per-cons inject telemetry).
};

struct LaneTraceEntry {
    ArcKey chord{0, 0};
    int    g1{-1}, g2{-1};          ///< the two bands' carry group ids.
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lam1{0.0}, lam2{0.0};
    std::string sat;                ///< chord satellite (chord.u = station index).
};

struct EwlLane {
    char cons;                        ///< cons filter (0 = all — the legacy single lane).
    char top;                         ///< UWL-1: the instance's top-tier band digit ('\0' = legacy single lane).
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
    std::size_t          wl_block_index{0};
    std::vector<F6Row>   rows;
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


// SubBlockTimer: the D0b per-sub-block timing accumulator (run-cumulative wall vs.
// timed).  Colocated here (like CarryEigenScope) as a runNetwork()-local type the
// NetworkTeardown readout seam now needs; the run TU keeps the sec_sub instances.
struct SubBlockTimer {
    double      wall_ms{0.0};    // driver wall bracketing the block (run cumulative)
    double      timed_ms{0.0};   // Δ(reduce+solve+cov-inv+cov-consumers) inside it (subtract for net)
    double      covcon_ms{0.0};  // of which cov-consumers (reported alongside; the D1 lever)
    std::size_t covcalls{0};     // cov-consumer calls inside it
};
} // namespace GnssProcessor
