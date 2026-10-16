#pragma once
/**
 * @file NetworkRunGroups.hpp
 * @brief @internal The driver-owned groups of run objects that the network seams share,
 *        and @ref NetworkRunEndInputs, the run-end bundle of them (card NOM step TD1a).
 *
 * The run (app/NetworkRun.hpp, card NOM3 step EP1) owns one of each group for the whole
 * run, and a seam reads the group it needs instead of one parameter per member:
 *   - @ref NetworkRunEngines: the run's estimation engines, owned since card NOM3 step EP1
 *     (built in place inside NetworkRun from a @ref NetworkEngineSpec);
 *   - @ref NetworkEmitEngines: the two emit-layer engines (networkEmitStore, the run end);
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
#include "filter/NetworkCarryVce.hpp"             // NetworkCarryVce
#include "filter/NetworkEstimator.hpp"            // NetworkEstimator
#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver (+ ::Config)
#include "filter/NetworkPriorResidual.hpp"        // NetworkPriorResidual
#include "slip/NetworkRawObservables.hpp"         // SlipQcChannel

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <type_traits>

namespace GnssProcessor {

class NetworkEstimator;
class NetworkInvariantCarryDriver;
class NetworkCarryVce;
class NetworkPriorResidual;
struct SlipQcChannel;

/// The constructor arguments of the run's engines and groups (card NOM3 step EP1), computed
/// once by networkEngineSpec (app/NetworkRun.hpp) from the configuration and the knobs.  A
/// field is either the setup local it replaced, with that local's name and type, or one
/// engine constructor argument the setup computed inline.
struct NetworkEngineSpec {
    std::vector<std::string>            vce_families;            ///< The VCE monitor's families: empty unless NetworkKnobs::carry_vce.
    double                              vce_window_epochs{0};    ///< The VCE monitor's window, in epochs.
    double                              vce_seed_floor{0};       ///< The VCE seed floor (the phase-leveling variance floor).
    double                              vce_seed_rate{0};        ///< The VCE seed rate (the phase-leveling ambiguity q).
    std::vector<std::string>            prires_families;         ///< The prior-residual census's families: empty unless NetworkKnobs::prires_on.
    double                              prires_window_epochs{0}; ///< The census window, in epochs.
    double                              prires_min_rows{0};      ///< The census's minimum row count.
    double                              prires_report_sigma{0};  ///< The census's report threshold.
    bool                                prires_corrob{false};    ///< The census corroborates against the raw slip evidence.
    bool                                prires_s0g{false};       ///< The constraint-aware global variance component.
    NetworkInvariantCarryDriver::Config carry_cfg;               ///< The DD-carry driver's configuration.
    NetworkClockEmitRebase::Config      clk_rebase_cfg;          ///< The clock emission re-base's configuration.
};

/// The run's estimation engines, owned (card NOM3 step EP1; card NOM2 step TD1a made this name
/// a const view, so every run-end report's `engines.X` reads unchanged).  Built in place inside
/// NetworkRun from a NetworkEngineSpec; setup then configures the members through references
/// (slip-QC parameters, corroborators, release tracking, estimator settings, warm resume).
struct NetworkRunEngines {
    explicit NetworkRunEngines(const NetworkEngineSpec& s)
        : slip_qc(),
          carry_vce(s.vce_families, s.vce_window_epochs, /*split_by_const=*/true,
                    /*seed_floor=*/s.vce_seed_floor, /*seed_rate=*/s.vce_seed_rate),
          prires(s.prires_families, s.prires_window_epochs, s.prires_min_rows,
                 s.prires_report_sigma, s.prires_corrob, s.prires_s0g),
          carry_driver(s.carry_cfg),
          est() {}
    NetworkRunEngines(const NetworkRunEngines&) = delete;
    NetworkRunEngines& operator=(const NetworkRunEngines&) = delete;

    /// The slip-QC channel.  Declared first, so it is destroyed last: two corroborators capture its address.
    SlipQcChannel               slip_qc;
    NetworkCarryVce             carry_vce;    ///< The LS-VCE carry-row monitor (M2); was carry_vce_est.
    NetworkPriorResidual        prires;       ///< The prior-residual census; was prires_est.
    NetworkInvariantCarryDriver carry_driver; ///< The DD cycle-basis invariant carry (card 4).
    NetworkEstimator            est;          ///< The joint estimator: one instance, reset and reused every epoch (its station-block pool).
};
static_assert(!std::is_copy_constructible_v<NetworkRunEngines> && !std::is_move_constructible_v<NetworkRunEngines>);

/// The two emit-layer engines.  The driver builds them before the epoch loop,
/// networkEmitStore runs them on every emitted epoch, the warm-resume restore and the
/// state snapshot read and write their held frames, and the run end reports their tallies.
struct NetworkEmitEngines {
    NetworkEmitRebase      rebase;     ///< Emit-layer frame re-base of the emitted phase biases (card 4 phase D); run under NetworkKnobs::emit_rebase_on.
    NetworkClockEmitRebase clk_rebase; ///< Clock emission re-base: the common-mode frame hold and the per-satellite step marks (card CLK-R).
};

/// The run's wall-clock record for the run-end timing report.  The driver writes every
/// member (the loop start, the D0b brackets' networkSubEnd, the per-epoch readout), except the
/// assembly wall, which networkPassB_ sums; every value is wall clock and never reaches a
/// product.
struct NetworkRunTiming {
    std::chrono::steady_clock::time_point loop_begin{};  ///< Wall clock captured just before the estimator is configured and the warm resume runs; the D0 "epoch-loop wall" counts from here.
    std::vector<SubBlockTimer> sub_blocks = std::vector<SubBlockTimer>(SB_COUNT); ///< Run totals per D0b sub-block, indexed by SubBlock; summed only under network.trace_section_timers.
    std::vector<SubBlockTimer> sec_sub_ep0 = std::vector<SubBlockTimer>(SB_COUNT);  ///< D0b: the epoch-top copy of sub_blocks the readout differences against (card NOM3 step EP1).
    std::vector<std::chrono::steady_clock::time_point> sub_wall0 =
        std::vector<std::chrono::steady_clock::time_point>(SB_COUNT);              ///< networkSubBegin's wall stamp per sub-block.
    std::vector<NetworkNormalEquations::SectionTimers> sub_tmr0 =
        std::vector<NetworkNormalEquations::SectionTimers>(SB_COUNT);              ///< networkSubBegin's section-timer stamp per sub-block.
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
