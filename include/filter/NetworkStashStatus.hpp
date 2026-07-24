#pragma once
/**
 * @file NetworkStashStatus.hpp
 * @brief Network producer epoch block: D2/C1/SH carry re-stash + LS-VCE fold + epoch-status QC row.
 *
 * Relocated VERBATIM (dedent-only, zero substitution) out of Application::runNetwork
 * at decomposition INC-11b (2026-07-22).  void, self-contained (produces no crossing
 * locals); every carry / trace object is an explicit reference into the caller's
 * state.  The caller keeps the subBegin/subEnd(SB_STASH_STATUS) brackets.
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
 * @brief Re-stash the D2/C1/SH carried quantities, fold this epoch into the carry
 *        LS-VCE monitor, and emit the epoch-status QC row.  This is the per-epoch
 *        producer bookkeeping tail (run after the solve/emit) that persists the carries
 *        for the next epoch's prior and records the epoch's QC census (present / clock /
 *        PB / CB / dropped-sat counts).  Keeps the subBegin/subEnd(SB_STASH_STATUS) brackets.
 */
void networkStashStatus(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    NetworkCarryVce& carry_vce_est,
    const std::set<std::string>& clock_refloat_sats,
    int n_arc_refloat_epoch,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::set<std::string>& retro_emit_sats,
    const std::set<std::string>& shared_phase_keys,
    double sigma0_adapt,
    int total_pins,
    bool vce_wants_cb,
    bool vce_wants_pb,
    double dt,
    bool datum_changed,
    const std::set<char>& changed_consts,
    int n_clock,
    int n_present,
    int n_pb,
    int n_cb,
    int n_dropped_sats,
    int n_adapt);

} // namespace GnssProcessor
