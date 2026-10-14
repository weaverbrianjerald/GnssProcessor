#pragma once
/**
 * @file NetworkRunGroups.hpp
 * @brief @internal The driver-owned groups of run objects that the network seams share,
 *        and @ref NetworkRunEndInputs, the run-end bundle of them (card NOM step TD1a).
 *
 * Application::runNetwork holds one of each group for the whole run and hands a seam the
 * group it reads instead of one parameter per member:
 *   - @ref NetworkRunEngines: a const view of the run's estimation engines (the run end);
 *   - @ref NetworkEmitEngines: the three emit-layer engines (networkEmitStore, the run end);
 *   - @ref NetworkRunTiming: the epoch-loop wall clock and the D0/D0b section-timer totals
 *     (the driver writes them, the run end reports them);
 *   - @ref ClosedLoopTally: the held-out consumer's integer-recovery tallies
 *     (networkEmitStore writes them, the run end reports them).
 *
 * The groups are not NetworkRunContext members: that header's banner keeps the engines,
 * the section timers and the closed-loop metrics with the driver.  The on/off flags of
 * the emit layers are configuration, resolved once in NetworkKnobs::snapshot, and are not
 * repeated here.
 */

#include "filter/NetworkWlRunTypes.hpp"        // SubBlockTimer / SB_COUNT
#include "products/NetworkClockEmitRebase.hpp" // NetworkClockEmitRebase (card CLK-R)
#include "products/NetworkEmitRebase.hpp"      // NetworkEmitRebase (card 4 phase D)
#include "products/NetworkPbEmitFilter.hpp"    // NetworkPbEmitFilter (M5 prerequisite)

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <vector>

namespace GnssProcessor {

class NetworkEstimator;
class NetworkInvariantCarryDriver;
class NetworkCarryVce;
class NetworkPriorResidual;
class NetworkIonoDeweightGate;
struct SlipQcChannel;

/// A const view of the run's estimation engines for the run-end reports.  The driver
/// builds each engine at its own point of the setup and binds this view once the epoch
/// loop has ended; nothing writes through it.
struct NetworkRunEngines {
    const NetworkEstimator&            est;           ///< The joint estimator: section timers, the rank-gate tolerance, the run's solve counters.
    const NetworkInvariantCarryDriver& carry_driver;  ///< The DD cycle-basis invariant carry (card 4): per-op timers and the carry census.
    const NetworkCarryVce&             carry_vce;     ///< The LS-VCE carry-row variance-component monitor (M2).
    const NetworkPriorResidual&        prires;        ///< The prior-residual census, read by networkPriorResidualRunEnd.
    const SlipQcChannel&               slip_qc;       ///< The slip-QC hardening channel and its run-end counters.
    const NetworkIonoDeweightGate&     iono_deweight; ///< The station-adaptive SH-VTEC-constraint deweight gate.
};

/// The three emit-layer engines.  The driver builds them before the epoch loop,
/// networkEmitStore runs them on every emitted epoch, the warm-resume restore and the
/// state snapshot read and write their held frames, and the run end reports their tallies.
struct NetworkEmitEngines {
    NetworkEmitRebase      rebase;     ///< Emit-layer frame re-base of the emitted phase biases (card 4 phase D); run under NetworkKnobs::emit_rebase_on.
    NetworkPbEmitFilter    pb_filter;  ///< Phase-bias emission-continuity filter on the re-based biases; run under NetworkKnobs::emit_pb_filter_on.
    NetworkClockEmitRebase clk_rebase; ///< Clock emission re-base: the common-mode frame hold and the per-satellite step marks (card CLK-R).
};

/// The run's wall-clock record for the run-end timing report.  The driver writes every
/// member (the loop start, the D0b brackets' subEnd, the per-epoch readout), except the
/// assembly wall, which networkPassB_ sums; every value is wall clock and never reaches a
/// product.
struct NetworkRunTiming {
    std::chrono::steady_clock::time_point loop_begin{};  ///< Wall clock captured just before the estimator is built and the warm resume runs; the D0 "epoch-loop wall" counts from here.
    std::vector<SubBlockTimer> sub_blocks = std::vector<SubBlockTimer>(SB_COUNT); ///< Run totals per D0b sub-block, indexed by SubBlock; summed only under network.trace_section_timers.
    double      asm_wall_ms{0.0};    ///< Run-total driver-side assembly wall, produce plus serial scatter (D0).
    std::size_t profiled_epochs{0};  ///< Epochs that reached the post-solve per-epoch D0 readout.
};

/// The held-out closed-loop consumer's integer-recovery tallies.  networkEmitStore
/// accumulates them on every consumed epoch; the run end prints the closed-loop summary
/// from them.  All zero unless the closed loop is armed.
struct ClosedLoopTally {
    /// The window of the converged-segment RMS: the last this many 3D errors.
    static constexpr std::size_t kRecentWindow = 30;

    std::size_t        epochs{0};          ///< Epochs the consumer processed.
    std::size_t        ar_ran{0};          ///< Epochs on which the consumer attempted ambiguity resolution.
    std::size_t        ar_fixed{0};        ///< Epochs on which that attempt fixed.
    long long          sum_fixed{0};       ///< Sum of the fixed-ambiguity counts over the fixed epochs.
    double             first_fix_tow{-1.0};///< Time of week of the first fixed epoch; -1 before one.
    int                first_fix_week{-1}; ///< GPS week of the first fixed epoch; -1 before one.
    std::deque<double> recent_err3d;       ///< The last @ref kRecentWindow 3D position errors (m), for the converged-segment RMS.
};

/// The run-end inputs of networkTeardown: const references to the four groups.  The
/// driver binds it once, after the epoch loop; the run end only reads through it.
struct NetworkRunEndInputs {
    const NetworkRunEngines&  engines;     ///< The run's estimation engines.
    const NetworkEmitEngines& emit;        ///< The emit-layer engines.
    const NetworkRunTiming&   timing;      ///< The loop wall clock and the section-timer totals.
    const ClosedLoopTally&    closed_loop; ///< The closed-loop consumer's tallies.
};

} // namespace GnssProcessor
