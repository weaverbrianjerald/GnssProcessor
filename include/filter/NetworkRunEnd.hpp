#pragma once
/**
 * @file NetworkRunEnd.hpp
 * @brief The network producer's run end as a declared table of reports (card NOM step TD1b).
 *
 * When the epoch loop has ended, networkTeardown (filter/NetworkTeardown.cpp) measures the
 * epoch-loop wall, builds one @ref NetworkRunEndView and calls every row of
 * @ref kNetworkRunEndReports in order.  A row is one report:
 *
 *     int networkXxxRunEnd(const NetworkRunEndView& v);
 *
 * A report lives in the file of the subsystem whose state it reports, declares at its top
 * the few names it reads from the view, gates itself on the knob that arms its subsystem
 * (returning 0 at once when that subsystem is off), prints its lines, and returns 0, or a
 * verdict for the process exit code.  The table in filter/NetworkRunEnd.cpp lists every
 * row with what it reports and its home file, so it reads as the run end's table of
 * contents.
 *
 * Adding a report:
 *   1. Write the function in the subsystem's .cpp and declare it, with its Doxygen (what it
 *      reports, the knob that gates it, what it returns), in the subsystem's header.  When
 *      the subsystem's state lives in one of the large files (app/NetworkRunContext.cpp,
 *      app/ApplicationNetworkAssembly.cpp, app/ApplicationNetwork.cpp,
 *      filter/NetworkSolveGlue.cpp) or has no file of its own, the report goes in a
 *      <Subsystem>RunEnd.cpp beside it, added to CMakeLists.txt.
 *   2. Add one row to kNetworkRunEndReports where its lines belong in the run end, and
 *      raise kNetworkRunEndReportCount by one.
 * A report is const on the run: it reads the context and writes only its log lines, its
 * own run-manifest record, and the files named below.  A per-station sum it prints is
 * summed into a local, never into the run context.
 *
 * The order constraints (the table is walked top to bottom):
 *   - The first row writes the estimability RUN row and closes every trace stream.  No
 *     later report writes to a trace stream: a closed stream drops a write silently.  The
 *     run manifest has its own append path.
 *   - Every run-manifest record (parameters as armed, contributions as armed, signals as
 *     planned, param_dispositions, param_sites, param_responses) is appended before the
 *     manifest's `complete` record, which stays the manifest's last line.
 *   - The product contract round trip closes the product writer, so it comes before the
 *     closed-loop summary; the run-summary report's shared-product line, which
 *     scripts/pc_gate_verdict.py reads as the run's done marker, comes after every
 *     artifact is written.
 *   - The injection, canonical-pair, roster, all-channel, multi-GNSS and ledger-emit
 *     reports are the last rows, before networkTeardown's zero-emit guard, so they are on
 *     record for a run that produced nothing.
 *   - The exit code: a later non-zero verdict replaces an earlier one.  The joint-block
 *     PD-fallback bar (networkAmbVehicleRunEnd) precedes the injection verdict
 *     (networkInjectRunEnd), so the injection verdict wins when it is non-zero; a run that
 *     emitted nothing returns 1 whatever the rows returned.
 *   - Scripts read the run-end text by its order as well as its wording (first or last
 *     match, windows of N lines, done markers), so a row moves only with its readers.
 */

#include <array>
#include <cstddef>
#include <memory>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkSetupResult;
struct NetworkRunEndInputs;

/// What a run-end report reads: the run at its end.  networkTeardown builds one and hands
/// it to every row; a report declares at its top the names it reads from it.
struct NetworkRunEndView {
    const NetworkRunContext&               ctx;     ///< The run context: knobs, carry state, service counters.
    const GnssConfig&                      config;  ///< The run configuration.
    const std::shared_ptr<spdlog::logger>& log;     ///< The main_log logger every run-end line goes to (the consumer's three summaries go to rt_engine).
    NetworkRunTraces&                      traces;  ///< The trace streams (closed by the first row) and the run manifest's append path.
    NetworkSetupResult&                    setup;   ///< The setup handoff: stations, the product writer (closed by the contract round trip), the consumer.
    const NetworkRunEndInputs&             run;     ///< The driver-owned groups: engines, emit engines, lanes, timing, closed-loop tally.
    double                                 loop_ms; ///< The epoch-loop wall, ms, measured once by networkTeardown before the walk.
};

/// A run-end report: prints its lines and returns 0, or a verdict for the exit code.
using NetworkRunEndReport = int (*)(const NetworkRunEndView& v);

/// One row of the run-end table.
struct NetworkRunEndRow {
    const char*         name;    ///< What the report prints, in a few words.
    const char*         home;    ///< The file the report lives in (under src/core/).
    NetworkRunEndReport report;  ///< The report.
};

/// The number of rows of @ref kNetworkRunEndReports.
inline constexpr std::size_t kNetworkRunEndReportCount = 113;

/// The run-end reports in print order (defined in filter/NetworkRunEnd.cpp).
extern const std::array<NetworkRunEndRow, kNetworkRunEndReportCount> kNetworkRunEndReports;

} // namespace GnssProcessor
