#pragma once
/**
 * @file NetworkWlRunTypes.hpp
 * @brief Run-loop-local helper types taken out of Application::runNetwork so the
 *        solve-region seams (NetworkInstallCarry / NetworkDdPriorBuild /
 *        NetworkSolveGlue) can name them: EwlLane (owns a NetworkMwWlLayer), the
 *        WL counter, inject and lane-trace PODs, and the
 *        Eigen-thread-pin RAII.  The definitions are field-for-field the former
 *        runNetwork() ones (INC-9b); the run TU keeps the instances (ewl_lanes /
 *        wl_inject_epoch / ...) and includes this header.
 *
 * CarryEigenScope is a general Eigen-threading RAII, not WL-specific; it is here
 * because it was also a runNetwork()-local type that the seams need.  The D0b
 * section-timer types (SubBlockTimer, the SubBlock enum and kSubBlockNames) are here for
 * the same reason.
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
};

struct WlInjectPlan {
    int    group_l1{-1}, group_l2{-1};
    ArcKey chord{0, 0};
    double w{0.0};                 ///< integer WL DD target ∇ΔN_WL (cyc).
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lambda1{0.0}, lambda2{0.0};
};

struct LaneTraceEntry {
    ArcKey chord{0, 0};
    int    g1{-1}, g2{-1};          ///< the two bands' carry group ids.
    double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
    double lam1{0.0}, lam2{0.0};
    std::string sat;                ///< chord satellite (chord.u = station index).
};

struct EwlLane {
    NetworkMwWlLayer layer;
    NetworkMwWlLayer::Counters run, epoch;
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
// that the run-timing report (networkRunTimingRunEnd) needs; the run totals are
// NetworkRunTiming::sub_blocks (filter/NetworkRunGroups.hpp).
struct SubBlockTimer {
    double      wall_ms{0.0};    // driver wall time bracketing the block (run cumulative)
    double      timed_ms{0.0};   // Δ(reduce+solve+cov-inv+cov-consumers) inside it (subtract for net)
    double      covcon_ms{0.0};  // cov-consumers share of timed_ms, reported separately (item D1)
    std::size_t covcalls{0};     // cov-consumer calls inside it
};

/// The D0b sub-blocks of the epoch loop (network.trace_section_timers): the disjoint
/// brackets the driver opens and closes with subBegin / subEnd, which tile the loop body
/// between the assembly wall and the emitted-epoch readout.  The value indexes
/// NetworkRunTiming::sub_blocks and @ref kSubBlockNames.  Moved here from runNetwork with
/// its names (card NOM step TD1a) so the driver's per-epoch line and the run-end ranking
/// read one table.
enum SubBlock {
    SB_SLIP = 0,     // block 1: PASS-A slip scans (MW refloat + IF-TD jumps), pre-assembly
    SB_PREASM,       //          pre-assembly measurement build (post-slip → assembly wall)
    SB_GAUGEBUILD,   //          buildEnsembleGauges (Σz=0 gauges); the ensemble datum is off in the tier
    SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors
                     //          (W·Pᶻ·Wᵀ dense DD-cov projection over K carried chords) (card 4)
    SB_SOLVEGLUE,    //          pre-solve prep + clock predict + solve + within-epoch re-solve
                     //          (net removes the internal solve/reduce)
    SB_INSTALL,      //          carry_driver.installPosterior (DD posterior mean/cov readback) (card 4)
    SB_NETAR,        //          producer-side network AR, phase 2 (log-only; off in the tier)
    SB_SOLPREP,      //          toSolutionEpoch + per-sat discontinuity signalling
    SB_EMITSTORE,    // block 7: emit re-base + PB-emit filter + store push + solution CSV
    SB_IONODIAG,     // block 4: iono / ZWD / anchor-info viz traces (per-station recon sweeps)
    SB_DDFRAC,       // block 6: cross-station DD fractional-residual (B5) trace
    SB_SHDEV_VTEC,   // block 4: T5 SH-deviation carry KF + co-estimated VTEC coeff trace
    SB_T1A,          // block 3: T1a SH-field health probe + fused H1 adaptive-deweight gate
    SB_LEVELING,     // block 5: phase-leveling carry harvest + D2/C1 stash + T1d SH-conv
    SB_RAIM,         // block 2: post-fit network RAIM pass-1/2 + median-polish attribution
    SB_CLOCKUPD,     // block 8: two-state clock update + fault QC + retro-flag feed
    SB_FRAMEDIAG,    // shared-normal spectrum + post-fit sigma0, both off by default (P1-3/P1-4)
    SB_STASH_STATUS, // block 5/8: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
    SB_ROSTER,       // signal-roster build and arc-ledger advance at the top of the
                     //       epoch, and the disagreement pass after the clock update
                     //       (card SID phase 1).  Both brackets accumulate into one
                     //       entry: they are the same observatory, and the cost of
                     //       interest is what the card adds to an epoch.
    SB_FASTCLOCK,    // fast-clock anchor harvest + the shadow-mode grading tick (FC-1).
                     //       The tick on decimated-out epochs is outside the loop and
                     //       untimed here; it has its own ctx.counters.fc_wall_ms
                     //       accumulator, because a tick is not a loop epoch and must
                     //       not enter the D0 wall.
    SB_COUNT
};

/// The printed name of each @ref SubBlock, in enum order: the driver's per-epoch D0b line
/// and the run-end D0b ranking.
inline constexpr const char* kSubBlockNames[SB_COUNT] = {
    "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
    "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
    "t1a-health", "leveling", "raim", "clock-upd", "frame-diag", "stash-status",
    "roster", "fast-clock"
};
} // namespace GnssProcessor
