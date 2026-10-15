#pragma once
/**
 * @file NetworkRun.hpp
 * @brief The network run as the epoch loop sees it: the run's references and the groups it
 *        owns (card NOM3 step EP1).
 *
 * Application::runNetwork builds one NetworkRun right after the trace owner, in place, and
 * passes it to Application::networkProcessEpoch_ for every epoch.  It holds:
 *   - seven references to objects runNetwork declares before it and keeps alive to the run
 *     end: the run context, the configuration and its network block, the main logger, the
 *     trace owner, the setup result and the log directory;
 *   - the four driver-owned groups of filter/NetworkRunGroups.hpp, by value: the estimation
 *     engines (NetworkRunEngines, the owner since EP1), the emit engines, the run timing and
 *     the closed-loop tally.
 *
 * Passed as NetworkRun&, never const: C++ does not carry const through a reference member,
 * so a const run would still hand out a writable context, traces and setup, and most seams
 * write run state by design (counters, carry, traces, engines).  What a seam may not write
 * is stated by the epoch (app/NetworkEpoch.hpp) and by each seam's preamble, whose lines keep
 * the frozen declarators (`const NetworkEstimator& est = run.engines.est;`).
 *
 * Neither copyable nor movable: two corroborators that runNetwork installs (the
 * prior-residual and the block-Baarda resolvers) capture the address of engines.slip_qc, and
 * the run end binds const references to the groups.
 */
#include "filter/NetworkRunGroups.hpp"   // NetworkRunEngines, NetworkEngineSpec, the TD1a groups
#include "filter/NetworkWlRunTypes.hpp"  // SubBlock
#include <filesystem>
#include <memory>
#include <type_traits>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkConfig;
struct NetworkKnobs;
struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkSetupResult;

struct NetworkRun {
    /// Binds the references and builds the owned groups from @p spec, each member at its
    /// final address (no engine is copied or moved).
    NetworkRun(NetworkRunContext& ctx, const GnssConfig& config, const NetworkConfig& nc,
               const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
               NetworkSetupResult& setup, const std::filesystem::path& log_dir,
               const NetworkEngineSpec& spec)
        : ctx(ctx), config(config), nc(nc), log(log), traces(traces), setup(setup),
          log_dir(log_dir), engines(spec),
          emit{ NetworkEmitRebase{}, NetworkClockEmitRebase(spec.clk_rebase_cfg) } {}
    NetworkRun(const NetworkRun&) = delete;
    NetworkRun& operator=(const NetworkRun&) = delete;
    NetworkRun(NetworkRun&&) = delete;
    NetworkRun& operator=(NetworkRun&&) = delete;

    // --- The run's references ----------------------------------------------------------
    NetworkRunContext&                     ctx;      ///< Knobs (const), cross-epoch carry state, service counters.
    const GnssConfig&                      config;   ///< The configuration (Application::config_); never written during the run.
    const NetworkConfig&                   nc;       ///< Its network block, config.getNetworkConfig().
    const std::shared_ptr<spdlog::logger>& log;      ///< "main_log" (runNetwork's local `log`).
    NetworkRunTraces&                      traces;   ///< Every trace stream and the run manifest.
    NetworkSetupResult&                    setup;    ///< Stations, sync, emitter, product writer, consumer; one address for the run.
    const std::filesystem::path&           log_dir;  ///< Application::log_dir_ (the producer AR's export root).
    // --- The groups the run owns (filter/NetworkRunGroups.hpp) ---------------------------
    NetworkRunEngines  engines;      ///< The estimation engines (est, carry_driver, carry_vce, prires, slip_qc).
    NetworkEmitEngines emit;         ///< Emit re-base, clock emission re-base.
    NetworkRunTiming   timing;       ///< Loop start, D0b totals, the bracket scratch, the assembly wall.
    /// The held-out consumer's integer-recovery tallies: accumulated by networkEmitStore
    /// and summarized at the run end.  The consumer is graded against the ARP-referenced truth
    /// setup.cl_truth_arp (setup owns the marker -> ARP reduction), so its ENU error is not
    /// offset by the station's antenna eccentricity.
    ClosedLoopTally    closed_loop;
};

static_assert(!std::is_copy_constructible_v<NetworkRun> && !std::is_move_constructible_v<NetworkRun>);
static_assert(!std::is_copy_assignable_v<NetworkRun> && !std::is_move_assignable_v<NetworkRun>);

/// The engines' constructor arguments, a pure function of the configuration and the knobs
/// (no log line, no trace, no setup read).  Its body is the setup statements that built
/// them, cut character-exact from runNetwork (card NOM3 step EP1); step EP4 splits it into
/// its concerns' builders.  Defined in src/core/app/NetworkRun.cpp.
NetworkEngineSpec networkEngineSpec(const GnssConfig& config_, const NetworkConfig& nc,
                                    const NetworkRunContext& ctx);

/// Opens D0b sub-block @p b: records the wall clock and the estimator's section timers.
/// No-op unless network.trace_section_timers (NetworkKnobs::sec_timers_on).  Formerly the
/// driver closure subBegin; the body is unchanged.
void networkSubBegin(NetworkRun& run, SubBlock b);
/// Closes D0b sub-block @p b: adds its wall and timed deltas to NetworkRunTiming::sub_blocks.
/// Formerly the driver closure subEnd; the body is unchanged.
void networkSubEnd(NetworkRun& run, SubBlock b);

} // namespace GnssProcessor
