#pragma once
/**
 * @file NetworkMwWlGlue.hpp
 * @brief The MW-WL / EWL / UWL / lane-carry inject step of the PPP-RTK network producer,
 *        relocated unchanged out of networkBuildCarryDdPriors (the second
 *        `if (mw_mach_on)` block) at decomposition INC-13.
 *
 * This is the back half of the WL machinery.  After the census and signal-selection
 * block has populated the shared per-(rx,sat) maps (revmap, lin_of, iono_of, primary,
 * band_top*_of, band_mid_of) and the invariant DD-carry blocks (Card-4) are built, this
 * step constructs the WL-1 float lane, the WL-2 EWL FFR-fixed lane, the WL-3 per-epoch MW
 * lane increments, the bridge->WL-integer hold/inject walk, and the F6 and lane-trace
 * diagnostics, and appends its rows to @p blocks.  The `if (mw_mach_on)` guard stays at
 * the call site, so with it off the run is byte-identical.  The step owns no state:
 * every WL run-state instance and the three cross-epoch channels are references into
 * objects owned by the run TU (through ctx or explicit arguments), and the shared maps
 * are read-only inputs filled by the census block.
 */

#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkMwWlLayer.hpp"          // NetworkMwWlLayer (+ ::Config / ::Counters)
#include "filter/NetworkNormalEquations.hpp"    // NetworkNormalEquations::DdPriorBlock
#include "filter/NetworkRunTypes.hpp"           // NetStation
#include "filter/NetworkWlRunTypes.hpp"         // EwlLane / WlInjectPlan / LaneTraceEntry / WlCounters / F6Capture / Rev / PP
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
 * @brief Build this epoch's MW-WL / EWL / UWL / lane-carry pseudo-observation rows and
 *        append them to @p blocks (WL-1 float lane, WL-2 EWL FFR-fixed lane, WL-3 MW lane
 *        increments, the bridge->WL-integer hold/inject walk, and the F6 and lane-trace
 *        diagnostics).
 *
 * Called under `if (mw_mach_on)` from networkBuildCarryDdPriors, after
 * carry_driver.buildDdPriors.  The shared per-(rx,sat) maps are read-only inputs
 * populated by the census block.  Rows are appended in the order WL-1, then EWL (only
 * with mw_wl.ewl_fold_rows), then WL hold rows.  See the .cpp for the relocated body.
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
    const std::set<std::string>* carry_we_drop_sats,
    const std::map<int, std::set<std::string>>* carry_we_drop_arcs,
    int n_threads,
    std::vector<EwlLane>& ewl_lanes,
    const NetworkMwWlLayer::Config& ewl_cfg,
    NetworkMwWlLayer& mw_wl_layer,
    F6Capture& mw_wl_f6,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_epoch,
    const std::map<std::pair<int, int>, Rev>& revmap,
    const std::map<std::pair<int, int>, double>& lin_of,
    const std::map<std::pair<int, std::string>, double>& iono_of,
    const std::map<std::pair<int, std::string>, PP>& primary,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top_of,
    const std::map<std::pair<int, std::string>, std::pair<std::string, int>>& band_top2_of,
    const std::map<std::pair<int, std::string>, std::string>& band_mid_of,
    const std::function<double(const std::string&, const std::string&)>& lam,
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    bool mw_wl_on,
    bool ewl_on,
    bool uwl_on,
    bool uwl_cold_on,
    bool uwl_mid_on,
    bool uwl_dual_on,
    bool uwl_sib_gate_on,
    bool wl_inject_on,
    bool bridge_debias_on,
    bool lane_on,
    bool lane_meas_on,
    bool lane_bridge_on,
    bool lane_fix_on,
    bool lane_parallel_components,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump);

} // namespace GnssProcessor
