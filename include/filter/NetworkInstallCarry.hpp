#pragma once
/**
 * @file NetworkInstallCarry.hpp
 * @brief Post-solve DD-carry install for the PPP-RTK network producer.  The body was
 *        moved unchanged out of Application::runNetwork (decomposition INC-9b).
 *
 * Runs on the final accepted x_s and datum, after acceptance and outside the
 * within-epoch RAIM block, so a withheld or contaminated epoch never enters the carry.
 * No-op and byte-identical when carry_invariant is off (the caller keeps the enclosing
 * `if (carry_invariant)` gate and the subBegin/subEnd(SB_INSTALL) brackets).  It runs
 * carry_driver.installPosterior (the DD posterior mean and covariance read back into the
 * carry), the PD-safe fold telemetry, and the D5 innovation-flag drain into the next
 * epoch's slip funnel.
 *
 * Owns no state: every carry object is an explicit reference into the caller's
 * NetworkRunContext.  The D5 gate switch (carry_innov_gate) is read from NetworkKnobs
 * (card NOM step TD1a).
 */

#include "filter/NetworkEstimator.hpp"          // NetworkEstimator
#include "filter/NetworkSbasis.hpp"             // NetworkSbasis::Plan
#include "filter/NetworkInvariantCarryDriver.hpp" // NetworkInvariantCarryDriver
#include "filter/NetworkRunTypes.hpp"           // NetStation
#include "filter/NetworkWlRunTypes.hpp"         // CarryEigenScope
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
 * @brief Install the accepted DD posterior for this epoch.
 * @see NetworkInstallCarry.hpp file banner for the ordering contract.
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
    bool carry_par,
    int n_threads,
    int seam_threads);

} // namespace GnssProcessor
