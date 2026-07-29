#pragma once
/**
 * @file NetworkInstallCarry.hpp
 * @brief Post-solve DD-carry install + WL zone-4 fold/inject for the PPP-RTK network
 *        producer.  Relocated VERBATIM (dedent-8, zero substitution) out of
 *        Application::runNetwork at decomposition INC-9b (2026-07-22).
 *
 * Runs on the FINAL accepted x_s + datum, POST-ACCEPTANCE and OUTSIDE the within-epoch
 * RAIM block, so a withheld/contaminated epoch never poisons the carry.  Three parts,
 * all no-op / byte-identical when carry_invariant is OFF (the caller keeps the wrapping
 * `if (carry_invariant)` gate + the subBegin/subEnd(SB_INSTALL) brackets):
 *   1. carry_driver.installPosterior (DD posterior mean/cov read-back into the carry);
 *   2. the M5/WL-1 counter folds (mw_wl_epoch -> mw_wl_run, ewl_lanes[].run, wl_run,
 *      wl_cons_run) and the D5 inject application against the fresh posterior;
 *   3. the log-only F6 / lane-trace / d5-flags CSV writers (network.mw_wl.*).
 *
 * Owns NO state: every carry object is an explicit reference into the caller's
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
 * @brief Install the accepted DD posterior + fold the WL zone-4 tallies for this epoch.
 * @see NetworkInstallCarry.hpp file banner for the ordering contract.  All WL run-state
 *      params are in/out references into the run-TU-owned instances.
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
