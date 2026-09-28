#pragma once
/**
 * @file NetworkTeardown.hpp
 * @brief The post-loop run-end readout tail of the PPP-RTK network producer: epoch-loop
 *        timing, the Tier-D0/D0b section-timer run totals and carry-driver census, the
 *        run-end telemetry sweep (rank gate, KP, CB-ADM, slip QC, eq34, carry, SH-dev,
 *        MW-WL, EWL, UWL, lane, D5, iono deweight, emit rebase, PB filter, clock runaway,
 *        heal, coupling, VCE, per-constellation), the end-of-run manifest, the estimability
 *        run row and trace close, the B6 serialized-contract round-trip self-check, and the
 *        closed-loop integer-recovery harness summary.
 *
 * The body was moved out of Application::runNetwork at the teardown-tail decomposition,
 * after the SOLVE-region (INC-9b) and NetworkRelevel (Card-6) cuts.  It returns the
 * process exit code, so the run-TU tail is `return networkTeardown(...);`: 1 when no
 * epoch was emitted, otherwise the injection result when it is nonzero, otherwise the
 * joint-block PD-fallback verdict (0 or 1).
 *
 * The tail is its own seam, not part of NetworkRunTraces, because it reads estimation
 * objects (`est`, `carry_driver`, `slip_qc`, `carry_vce_est`, `consumer`); hosting it in
 * the logging unit would make the logging unit depend on the estimation units (the INC-7
 * P3 anti-pattern).
 *
 * Owns no state.  The body keeps the driver's indentation (dedent 0; runNetwork's body is
 * indented 4 spaces) and opens with an alias preamble that re-creates the run-TU
 * ctx/config/setup/traces aliases so the body reads the original names.  Apart from the
 * trace and product finalization (traces.closeAll, product_writer.reset) it does not
 * modify estimation state.
 */


#include "app/NetworkSetupResult.hpp"     // NetworkSetupResult (consumer / product_writer / ...)
#include "filter/NetworkEstimator.hpp"    // NetworkEstimator
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkCarryVce.hpp"     // NetworkCarryVce
#include "filter/NetworkPriorResidual.hpp" // NetworkPriorResidual (prior-residual census run-end readout)
#include "filter/NetworkMwWlLayer.hpp"    // NetworkMwWlLayer::Counters
#include "filter/NetworkWlRunTypes.hpp"   // EwlLane / WlCounters / SubBlockTimer
#include "products/NetworkClockEmitRebase.hpp" // NetworkClockEmitRebase (card CLK-R)
#include "products/NetworkEmitRebase.hpp"   // NetworkEmitRebase
#include "products/NetworkPbEmitFilter.hpp" // NetworkPbEmitFilter
#include "filter/NetworkIonoDeweightGate.hpp"
#include "slip/NetworkSlipQc.hpp"         // SlipQcChannel
#include "math/Coordinates.hpp"           // Coordinates::Llh
#include "rinex/MultiRinexSync.hpp"       // SyncedEpoch

#include <Eigen/Core>

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;

/**
 * @brief Run the network producer's end-of-run readout tail and return the exit code.
 * @return 1 when no epoch was emitted; otherwise the injection result when nonzero,
 *         otherwise the joint-block PD-fallback verdict (0 or 1).
 * @see The file banner for the readout inventory and the reason this is a separate seam.
 */
int networkTeardown(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const NetworkEstimator& est,
    const NetworkInvariantCarryDriver& carry_driver,
    const NetworkCarryVce& carry_vce_est,
    const NetworkPriorResidual& prires_est,
    const SlipQcChannel& slip_qc,
    const NetworkMwWlLayer::Counters& mw_wl_run,
    const std::vector<EwlLane>& ewl_lanes,
    const WlCounters& wl_run,
    const std::map<char, WlCounters>& wl_cons_run,
    const NetworkEmitRebase& emit_rebase_engine,
    const NetworkPbEmitFilter& pb_filter_engine,
    const NetworkClockEmitRebase& clk_rebase_engine,
    const NetworkIonoDeweightGate& iono_deweight,
    const std::vector<SubBlockTimer>& sec_sub,
    const char* const* kSubNames,
    const std::vector<SyncedEpoch>& synced,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool ewl_on,
    bool wl_inject_on,
    bool uwl_on,
    bool lane_on,
    bool sh_dev_on,
    bool idw_on,
    bool emit_rebase_on,
    bool emit_pb_filter_on,
    double sec_asm_wall_ms,
    std::size_t sec_profiled_epochs,
    const std::chrono::steady_clock::time_point& t_loop_begin,
    std::size_t cl_epochs,
    std::size_t cl_ar_ran,
    std::size_t cl_ar_fixed,
    long long cl_sum_fixed,
    double cl_first_fix_tow,
    int cl_first_fix_week,
    const std::deque<double>& cl_recent_err3d,
    const Eigen::Vector3d& cl_truth,
    const Coordinates::Llh& cl_truth_llh,
    const std::filesystem::path& log_dir_);

} // namespace GnssProcessor
