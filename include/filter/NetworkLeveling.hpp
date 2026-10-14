#pragma once
/**
 * @file NetworkLeveling.hpp
 * @brief Network producer epoch block: phase-leveling carry harvest + D2/C1 stash.
 *
 * Relocated verbatim (dedent only, no substitution) out of Application::runNetwork
 * at decomposition INC-11b.  void and self-contained (it produces no locals that
 * cross the call); every carry / trace object is an explicit reference into the
 * caller's state.  The caller keeps the subBegin/subEnd(SB_LEVELING) brackets.  The block
 * also emits this epoch's eq34_reref rows on the events stream.
 */

#include "app/NetworkSetupResult.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkEstimator.hpp"
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
class NetworkCarryVce;      // full type in filter/NetworkCarryVce.hpp, included in the .cpp only
class NetworkPriorResidual; // full type in filter/NetworkPriorResidual.hpp; passed by reference only
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Harvest this epoch's phase-leveling carry from the post-fit solution, stash the
 *        D2/C1 carried quantities into @p ctx.
 *        This is the feed of the zero-q phase-leveling carry (and the LS-VCE leveling-row
 *        fold); it updates the persistent carries that the next epoch's prior depends on.
 *        The caller keeps the subBegin/subEnd(SB_LEVELING) brackets.
 */
void networkLeveling(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    NetworkCarryVce& carry_vce_est,
    NetworkPriorResidual& prires_est,
    bool carry_warm,
    double pl_now_s,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::set<std::string>& shared_phase_keys);

} // namespace GnssProcessor
