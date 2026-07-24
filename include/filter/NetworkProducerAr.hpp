#pragma once
/**
 * @file NetworkProducerAr.hpp
 * @brief Producer-side network ambiguity-resolution diagnostic (Phase 2, LOG ONLY)
 *        plus the one-shot full-arrowhead normal-equation export.
 *
 * Runs on the FINAL (post-adaptation) network solution, entirely READ-ONLY — it
 * neither re-pins the datum nor re-solves, so the network solution is byte-identical
 * whether this block runs or not.  Two cooperating pieces:
 *
 *  1. **Producer network AR, Phase 2** (`network.ambiguity_resolution.enable` +
 *     `min_epochs`): reconstructs the per-station float ambiguities, forms an
 *     independent between-station double-difference basis (star topology per
 *     (const,signal), GPS+Galileo only), builds the exact DD covariance from
 *     @ref NetworkEstimator::jointConditionalCovariance, converts to cycles, and runs
 *     @ref ils::ilsFix purely to LOG how many integers the network COULD fix (with the
 *     median DD formal-σ and float-fraction convergence gauges).  The DD ambiguities
 *     are datum-INVARIANT, so the fix attempt is independent of the phase-bias datum.
 *  2. **One-shot NEQ export** (`network.neq_export_epoch`): a post-solve read-only dump
 *     of the assembled arrowhead normal equations (border N_ss raw+reduced + RHS, each
 *     station's N_sr / N_rr / u_r, shared + local column labels) as row-major
 *     little-endian double `.bin` for offline spectral / weak-direction attribution.
 *     Fires on exactly one epoch per run; the solution is untouched.
 *
 * Seeded at decomposition INC-9b (2026-07-22), which relocated the SB_NETAR block
 * VERBATIM out of Application::runNetwork (see docs/network_decomposition_plan.md, the
 * SOLVE-REGION decomposition).  Owns NO state.  The caller keeps the
 * @c subBegin/subEnd(SB_NETAR) brackets.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator
#include "filter/NetworkSbasis.hpp"      // NetworkSbasis::Plan (shared_datum)
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"      // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <Eigen/Core>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class PppUtils;             // common/PppUtils.hpp — only a unique_ptr<>& param here
struct NetworkRunContext;  // app/NetworkRunContext.hpp — included in the .cpp only

/**
 * @brief Run the log-only producer-AR + NEQ-export tail for this epoch.
 *
 * @param ctx           [in] counters (read-only `n_emitted` gate) + config-independent
 *                      run state; taken whole for seam-family uniformity.
 * @param config_       Reconstructs `nc = config_.getNetworkConfig()` and
 *                      `netar = nc.ambiguity_resolution`.
 * @param log           "main_log": the Phase-2 AR summary + NEQ-export info rows.
 * @param se            Current synced epoch (week/TOW for the log rows).
 * @param est           Post-solve estimator (const): reconstructStation /
 *                      jointConditionalCovariance / normals / column labels.
 * @param x_s           Post-fit shared solution vector (gate + non-empty guards).
 * @param stations      This run's network stations (float-amb reconstruction source).
 * @param est_index_of  Per-station estimator-slot map (< 0 = not in this epoch).
 * @param asm_res       Per-station assembly results (h2local for local-column labels).
 * @param plan          S-basis plan (shared_datum for the conditional covariance).
 * @param ar_ppp_utils  Wavelength oracle for the DD cycle conversion (may be null when
 *                      AR is disabled — the block guards on it).
 * @param log_dir_      Output root for the NEQ-export artifacts (log_dir_/neq_export).
 */
void networkProducerAr(NetworkRunContext& ctx,
                       const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       const SyncedEpoch& se,
                       const NetworkEstimator& est,
                       const Eigen::VectorXd& x_s,
                       const std::vector<NetStation>& stations,
                       const std::vector<int>& est_index_of,
                       const std::vector<StationAssembly>& asm_res,
                       const NetworkSbasis::Plan& plan,
                       const std::unique_ptr<PppUtils>& ar_ppp_utils,
                       const std::filesystem::path& log_dir_);

} // namespace GnssProcessor
