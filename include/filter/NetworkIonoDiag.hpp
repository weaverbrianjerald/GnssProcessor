#pragma once
/**
 * @file NetworkIonoDiag.hpp
 * @brief Network producer epoch block: iono / ZWD / anchor-info viz traces (back-substitute local nuisance blocks).
 *
 * Relocated VERBATIM (dedent-only, zero substitution) out of Application::runNetwork
 * at decomposition INC-11b (2026-07-22).  void, self-contained (produces no crossing
 * locals); every carry / trace object is an explicit reference into the caller's
 * state.  The caller keeps the subBegin/subEnd(SB_IONODIAG) brackets.
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
class NetworkCarryVce;      // full type in app/NetworkRunContext.hpp (the .cpp)
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

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
    const std::vector<std::string>& registered_clock_sats,
    double clk_anchor_sigma);

} // namespace GnssProcessor
