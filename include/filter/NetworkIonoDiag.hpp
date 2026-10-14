#pragma once
/**
 * @file NetworkIonoDiag.hpp
 * @brief Network producer epoch block: iono / ZWD / receiver-state / anchor-info viz traces (back-substitutes local nuisance blocks).
 *
 * Moved unchanged (dedent only, no substitution) out of Application::runNetwork at
 * decomposition INC-11b.  Returns void and is self-contained (it produces no locals
 * that later blocks read); every carry / trace object is an explicit reference into
 * the caller's state.  The caller keeps the subBegin/subEnd(SB_IONODIAG) brackets.
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
 * @brief Emit this epoch's iono / ZWD / receiver-state / anchor-info visualization
 *        traces.  Back-substitutes each station's local nuisance blocks (slant iono,
 *        ZWD, receiver clock and biases) from the shared solution and writes the
 *        per-station trace rows.  Diagnostic only: it mutates no estimation state, and
 *        with the viz traces unarmed it writes nothing and the run is byte-identical.
 *        The caller keeps the subBegin/subEnd(SB_IONODIAG) brackets.  The anchor-info
 *        trace reads the resolved clock anchor sigma from @c ctx.knobs.clk_anchor_sigma
 *        (card NOM3 step EP0).
 */
void networkIonoDiag(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const EpochWorkspace& ws,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<std::string>& registered_clock_sats);

} // namespace GnssProcessor
