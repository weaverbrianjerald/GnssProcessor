#pragma once
/**
 * @file Teardown.hpp
 * @brief The run end of the PPP-RTK network producer: the walk of the declared run-end
 *        reports and the process exit code.
 *
 * Application::runNetwork ends with `return networkTeardown(...);`.  The function measures
 * the epoch-loop wall once, builds the @ref NetworkRunEndView every report reads, calls each
 * row of @ref kNetworkRunEndReports in order (network/runend/RunEnd.hpp: the table, how to
 * add a report, the order constraints), and returns the exit code.  Each report lives in the
 * file of the subsystem it reports; a new run-end line goes in its
 * subsystem's report, and a new report is one row of the table.
 *
 * Owns no state.  Apart from the trace and product finalization (the first row closes the
 * trace streams, the product contract round trip closes the product writer) the run end does
 * not modify the run.
 */

#include <memory>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkSetupResult;
struct NetworkRunEndInputs;

/**
 * @brief Run the network producer's run-end reports and return the exit code.
 * @param ctx     The run context: knobs, carry state and service counters (read only).
 * @param config_ The run configuration.
 * @param log     The main_log logger every run-end line goes to (the consumer's three
 *                summaries go to rt_engine).
 * @param traces  The run's trace streams, closed by the first report; the run manifest's
 *                run-end records are appended through it.
 * @param setup   The setup handoff; the product writer is closed by the contract round trip.
 * @param run     The driver-owned run groups the reports read (network/driver/RunGroups.hpp).
 * @return 1 when no epoch was emitted; otherwise the last non-zero verdict of the reports in
 *         table order: the injection verdict when it is non-zero, else the joint-block
 *         PD-fallback bar (0 or 1).
 */
int networkTeardown(const NetworkRunContext& ctx,
                    const GnssConfig& config_,
                    const std::shared_ptr<spdlog::logger>& log,
                    NetworkRunTraces& traces,
                    NetworkSetupResult& setup,
                    const NetworkRunEndInputs& run);

} // namespace GnssProcessor
