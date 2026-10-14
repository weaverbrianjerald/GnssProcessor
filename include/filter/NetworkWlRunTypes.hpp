#pragma once
/**
 * @file NetworkWlRunTypes.hpp
 * @brief Run-loop-local helper types taken out of Application::runNetwork so the
 *        solve-region seams (NetworkInstallCarry / NetworkSolveGlue) and the run-timing
 *        report can name them: the Eigen-thread-pin RAII (CarryEigenScope) and the D0b
 *        section-timer types (SubBlockTimer, the SubBlock enum and kSubBlockNames).  The
 *        definitions are field-for-field the former runNetwork() ones (INC-9b).
 *
 * The file name is historical: it also held the retired MW/WL lane types (cards WL-1,
 * WL-2, WL-3 and UWL-1), which were deleted with the lane layer.
 */

#include <Eigen/Core>                    // CarryEigenScope: nbThreads / setNbThreads

#include <cstddef>

namespace GnssProcessor {

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
    SB_COUNT
};

/// The printed name of each @ref SubBlock, in enum order: the driver's per-epoch D0b line
/// and the run-end D0b ranking.
inline constexpr const char* kSubBlockNames[SB_COUNT] = {
    "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
    "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
    "t1a-health", "leveling", "raim", "clock-upd", "frame-diag", "stash-status",
    "roster"
};
} // namespace GnssProcessor
