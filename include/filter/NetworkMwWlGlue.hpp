#pragma once
/**
 * @file NetworkMwWlGlue.hpp
 * @brief The MW-WL / EWL inject step of the PPP-RTK network producer,
 *        relocated unchanged out of networkBuildCarryDdPriors (the second
 *        `if (mw_mach_on)` block) at decomposition INC-13.
 *
 * This is the back half of the WL machinery.  After the census and signal-selection
 * block has populated the shared per-(rx,sat) maps (revmap, lin_of, iono_of, primary,
 * band_top_of) and the invariant DD-carry blocks (Card-4) are built, this
 * step runs the WL-2 EWL FFR-fixed lane, the bridge->WL-integer hold/inject walk and
 * the lane-trace diagnostic, and appends its hold rows to @p blocks.  The
 * `if (mw_mach_on)` guard stays at
 * the call site, so with it off the run is byte-identical.  The step owns no state:
 * every WL run-state instance and the three cross-epoch channels are references into
 * objects owned by the run TU (through ctx or explicit arguments), and the shared maps
 * are read-only inputs filled by the census block.
 */

#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkMwWlLayer.hpp"          // NetworkMwWlLayer (+ ::Config / ::Counters)
#include "filter/NetworkNormalEquations.hpp"    // NetworkNormalEquations::DdPriorBlock
#include "filter/NetworkRunTypes.hpp"           // NetStation
#include "filter/NetworkWlRunTypes.hpp"         // EwlLane / WlInjectPlan / LaneTraceEntry / WlCounters / Rev / PP
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;

/**
 * @brief Build this epoch's EWL pseudo-observation rows and append them to
 *        @p blocks (WL-2 EWL FFR-fixed lane, the bridge->WL-integer hold/inject walk,
 *        and the lane-trace diagnostic).
 *
 * Called under `if (mw_mach_on)` from networkBuildCarryDdPriors, after
 * carry_driver.buildDdPriors.  The shared per-(rx,sat) maps are read-only inputs
 * populated by the census block.  Only the WL hold rows are appended; the EWL lane
 * decides and exposes its integers without folding a row.  See the .cpp for the
 * relocated body.
 *
 * @p carry_slipped_ambs, @p we_culprits and @p n_threads are no longer read: their only
 * readers were the retired WL-3 lane increments.  They stay in the signature until the
 * lane step itself is retired.
 */
void networkMwWlInject(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const std::vector<NetStation>& stations,
    const SyncedEpoch& se,
    std::set<std::pair<int, std::string>>& carry_slipped_ambs,
    const WithinEpochCulprits& we_culprits,
    int n_threads,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    const std::map<std::pair<int, int>, Rev>& revmap,
    const std::map<std::pair<int, int>, double>& lin_of,
    const std::map<std::pair<int, std::string>, double>& iono_of,
    const std::map<std::pair<int, std::string>, PP>& primary,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top_of,
    const std::function<double(const std::string&, const std::string&)>& lam,
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    bool ewl_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets);

} // namespace GnssProcessor
