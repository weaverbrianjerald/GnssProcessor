#pragma once
/**
 * @file NetworkShHealth.hpp
 * @brief Network producer epoch block: T1a SH-field health probe + fused adaptive-deweight gate + carry LS-VCE.
 *
 * Moved verbatim (dedent only, no substitutions) out of Application::runNetwork
 * in decomposition step INC-11b.  Returns void and is self-contained (it produces
 * no locals the caller uses afterwards); every carry and trace object is an
 * explicit reference into the caller's state.  The caller keeps the
 * subBegin/subEnd(SB_T1A) brackets.
 */

#include "app/NetworkSetupResult.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Core>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkCarryVce;      // filter/NetworkCarryVce.hpp, included in the .cpp only
class NetworkPriorResidual; // filter/NetworkPriorResidual.hpp, included in the .cpp only
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Run the T1a SH-field health probe for this epoch, fold the fused adaptive-
 *        deweight gate into @p iono_deweight, and update the carry LS-VCE monitor.
 *        Checks the a₀₀ physical band of the co-estimated SH-VTEC field, writes the
 *        SH health trace, drives the station-adaptive SH-constraint deweighting and
 *        advances the prior-residual census (@p prires_est).  @c ctx.carry.first_vce_s and
 *        @c ctx.carry.last_vce_hour track the hourly LS-VCE trace flush.  The caller keeps
 *        the subBegin/subEnd(SB_T1A) brackets.
 */
void networkShHealth(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<std::vector<PppObservation>>& modeled,
    NetworkIonoDeweightGate& iono_deweight,
    NetworkCarryVce& carry_vce_est,
    NetworkPriorResidual& prires_est,
    bool datum_changed,
    double pl_now_s);

} // namespace GnssProcessor
