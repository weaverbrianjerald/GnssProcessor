#pragma once
/**
 * @file NetworkShDevVtec.hpp
 * @brief Network producer epoch block: T5 SH-deviation carry KF and the co-estimated
 *        VTEC-coefficient trace (network_vtec_sh.csv).
 *
 * Moved unchanged (dedent only, no substitutions) out of Application::runNetwork
 * at decomposition INC-11b.  Returns void and is self-contained (it produces no
 * locals the caller reads afterwards); every carry and trace object is an explicit
 * reference into the caller's state.  The caller keeps the
 * subBegin/subEnd(SB_SHDEV_VTEC) brackets.
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Advance the T5 SH-deviation carry Kalman filter and emit the co-estimated
 *        VTEC-coefficient trace for this epoch.  Folds the shared solution's SH-field
 *        deviation into the persistent per-LOS (station|satellite) carry in
 *        @p ctx.carry.sh_dev_carry; that update runs only when @p sh_dev_on, and is a
 *        no-op otherwise.  Independently of @p sh_dev_on, writes one row per SH
 *        coefficient to the VTEC-coefficient trace when that trace is open and an SH
 *        model exists.  The caller keeps the subBegin/subEnd(SB_SHDEV_VTEC) brackets.
 */
void networkShDevVtec(
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
    const std::vector<std::vector<PppObservation>>& modeled,
    NetworkIonoDeweightGate& iono_deweight,
    bool sh_dev_on);

} // namespace GnssProcessor
