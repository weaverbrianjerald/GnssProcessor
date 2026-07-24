#pragma once
/**
 * @file NetworkShHealth.hpp
 * @brief Network producer epoch block: T1a SH-field health probe + fused adaptive-deweight gate + carry LS-VCE.
 *
 * Relocated VERBATIM (dedent-only, zero substitution) out of Application::runNetwork
 * at decomposition INC-11b (2026-07-22).  void, self-contained (produces no crossing
 * locals); every carry / trace object is an explicit reference into the caller's
 * state.  The caller keeps the subBegin/subEnd(SB_T1A) brackets.
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

/**
 * @brief Run the T1a SH-field health probe for this epoch, fold the fused adaptive-
 *        deweight gate into @p iono_deweight, and update the carry LS-VCE monitor.
 *        Probes the co-estimated SH-VTEC field's conditioning and drives the
 *        station-adaptive SH-constraint deweighting; @p first_vce_s / @p last_vce_hour
 *        track the LS-VCE fold cadence.  Keeps the subBegin/subEnd(SB_T1A) brackets.
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
    bool datum_changed,
    double pl_now_s,
    double& first_vce_s,
    int& last_vce_hour);

} // namespace GnssProcessor
