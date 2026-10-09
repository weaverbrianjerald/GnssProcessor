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
 * Inputs (card NOM step TD1a): the run (context, configuration, logger, traces, setup)
 * and one const bundle, @ref NetworkRunEndInputs, of the driver-owned groups (the
 * engines, the emit engines, the lane run, the timing record and the closed-loop tally,
 * filter/NetworkRunGroups.hpp).  The lane and emit-layer switches are read from
 * NetworkKnobs; the values the driver used to pass twice are read where they live
 * (the synced epoch list from setup.sync, the consumer truth from setup.cl_truth_arp, the
 * log directory from traces).
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
#include "filter/NetworkWlRunTypes.hpp"   // EwlLane / WlCounters / SubBlockTimer / kSubBlockNames
#include "filter/NetworkRunGroups.hpp"    // NetworkRunEndInputs and its groups (card NOM step TD1a)
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
 * @param ctx     The run context: knobs, carry state and service counters.
 * @param config_ The run configuration.
 * @param log     The main_log logger every run-end line goes to (three consumer summaries
 *                go to rt_engine).
 * @param traces  The run's trace streams; closed here (closeAll), and the run manifest's
 *                run-end records are appended through it.
 * @param setup   The setup handoff; the product writer is closed here (the B6 round trip).
 * @param run     The driver-owned run groups the reports read.
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
    const NetworkRunEndInputs& run);

} // namespace GnssProcessor
