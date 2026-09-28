#pragma once
/**
 * @file NetworkInstallCarry.hpp
 * @brief Post-solve DD-carry install and WL zone-4 fold/inject for the PPP-RTK network
 *        producer.  The body was moved unchanged out of Application::runNetwork
 *        (decomposition INC-9b).
 *
 * Runs on the final accepted x_s and datum, after acceptance and outside the
 * within-epoch RAIM block, so a withheld or contaminated epoch never enters the carry.
 * Three parts, all no-op and byte-identical when carry_invariant is off (the caller
 * keeps the enclosing `if (carry_invariant)` gate and the subBegin/subEnd(SB_INSTALL)
 * brackets):
 *   1. carry_driver.installPosterior (DD posterior mean/cov read back into the carry);
 *   2. the M5/WL-1 counter folds (mw_wl_epoch -> mw_wl_run, ewl_lanes[].run, wl_run,
 *      wl_cons_run) and the application of the WL inject plan against the fresh
 *      posterior;
 *   3. the log-only F6 / lane-trace / d5-flags CSV writers (network.mw_wl.*).
 *
 * Owns no state: every carry object is an explicit reference into the caller's
 * NetworkRunContext, and every cross-epoch WL run-state instance (ewl_lanes /
 * wl_inject_epoch / mw_wl_run / mw_wl_f6 / ...) is passed by reference from the run TU.
 */

#include "filter/NetworkEstimator.hpp"          // NetworkEstimator
#include "filter/NetworkSbasis.hpp"             // NetworkSbasis::Plan
#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkRunTypes.hpp"           // NetStation
#include "filter/NetworkWlRunTypes.hpp"         // EwlLane/WlInjectPlan/LaneTraceEntry/WlCounters/F6Capture (+ NetworkMwWlLayer::Counters)
#include "slip/NetworkRawObservables.hpp"       // SlipQcChannel
#include "filter/NetworkZStore.hpp"             // ArcKey (carry_arc_amb_key maps)
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <Eigen/Core>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;

/**
 * @brief Install the accepted DD posterior and fold the WL zone-4 tallies for this epoch.
 * @see NetworkInstallCarry.hpp file banner for the ordering contract.  All WL run-state
 *      parameters are in/out references into the instances owned by the run TU.
 */
void networkInstallCarry(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const SyncedEpoch& se,
    const std::vector<NetStation>& stations,
    SlipQcChannel& slip_qc,
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key,
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key2,
    const std::filesystem::path& log_dir_,
    bool carry_par,
    int n_threads,
    int seam_threads,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool lane_on,
    bool ewl_on,
    bool uwl_on,
    bool wl_inject_on,
    bool lane_fix_on,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump,
    std::vector<EwlLane>& ewl_lanes,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    NetworkMwWlLayer::Counters& mw_wl_run,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    WlCounters& wl_run,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_run,
    std::map<char, WlCounters>& wl_cons_epoch,
    F6Capture& mw_wl_f6);

} // namespace GnssProcessor
