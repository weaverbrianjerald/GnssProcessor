#pragma once
/**
 * @file NetworkTeardown.hpp
 * @brief The post-loop run-end READOUT tail of the PPP-RTK network producer: epoch-loop
 *        timing, the Tier-D0/D0b section-timer run totals + carry-driver census, the full
 *        run-end telemetry sweep (rank-gate / KP / CB-ADM / slip-QC / eq34 / carry / SH-dev
 *        / MW-WL / EWL / UWL / lane / D5 / iono-deweight / emit-rebase / PB-filter / clock-
 *        runaway / heal / coupling / VCE / per-const), the end-of-run manifest + estimability
 *        run row + trace close, the B6 serialized-contract round-trip self-check, and the
 *        closed-loop integer-recovery harness summary.
 *
 * Relocated VERBATIM out of Application::runNetwork at the teardown-tail decomposition
 * (2026-07-23) — the last major run-TU block after the SOLVE-region (INC-9b) and Card-6
 * (NetworkRelevel) cuts.  Returns the process exit code (the former `return 1;` zero-emit
 * guard + `return 0;`), so the run-TU tail becomes `return networkTeardown(...);`.
 *
 * A NEW seam (NOT folded into NetworkRunTraces): the tail reads estimation objects
 * (`est` / `carry_driver` / `slip_qc` / `carry_vce_est` / `consumer`), so hosting it in the
 * logging unit would invert the logging-unit dependency (the INC-7 P3 anti-pattern).
 *
 * Owns NO state.  The body is a byte-copy (dedent-0 — runNetwork's body is 4-space
 * indented); the only added code is the alias preamble that byte-copies the run-TU
 * ctx/config/setup/traces aliases so the body reads the original names.  Read-only apart
 * from the trace/product finalization (traces.closeAll / product_writer.reset).
 */

#include "app/NetworkSetupResult.hpp"     // NetworkSetupResult (consumer / product_writer / ...)
#include "filter/NetworkEstimator.hpp"    // NetworkEstimator
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkCarryVce.hpp"     // NetworkCarryVce
#include "filter/NetworkMwWlLayer.hpp"    // NetworkMwWlLayer::Counters
#include "filter/NetworkWlRunTypes.hpp"   // EwlLane / WlCounters / SubBlockTimer
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
 * @see the file banner for the readout inventory + the dependency-inversion rationale.
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
    const SlipQcChannel& slip_qc,
    const NetworkMwWlLayer::Counters& mw_wl_run,
    const std::vector<EwlLane>& ewl_lanes,
    const WlCounters& wl_run,
    const std::map<char, WlCounters>& wl_cons_run,
    const NetworkEmitRebase& emit_rebase_engine,
    const NetworkPbEmitFilter& pb_filter_engine,
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
