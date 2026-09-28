#pragma once
/**
 * @file NetworkDdFrac.hpp
 * @brief Network producer epoch block: cross-station DD fractional-residual (B5) diagnostic trace.
 *
 * Relocated verbatim (dedent only, no substitution) out of Application::runNetwork
 * (decomposition INC-11b).  Returns void and is self-contained (it produces no
 * locals that cross into the driver); every carry and trace object is an explicit
 * reference into the caller's state.  The caller keeps the subBegin/subEnd(SB_DDFRAC)
 * brackets.
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
 * @brief Emit this epoch's cross-station DD fractional-residual (B5) diagnostic trace.
 *        Forms between-station double differences of the absolute float ambiguities,
 *        each against one reference satellite per (constellation, signal) (the
 *        lexically lowest present at 2 or more stations by default;
 *        network.trace_dd_fractional_ref = lowest | oldest), and reduces each to its
 *        fractional cycle for a network_dd_fractional.csv row.  Also writes the
 *        per-epoch phase-anchor map when that trace is open.  Trace-only: it mutates no
 *        estimation state (only the trace's own reference-age bookkeeping and
 *        counters), and it does nothing unless one of the two traces is open.
 *        The caller keeps the subBegin/subEnd(SB_DDFRAC) brackets.
 */
void networkDdFrac(
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
    const std::unique_ptr<PppUtils>& dd_ppp_utils);

} // namespace GnssProcessor
