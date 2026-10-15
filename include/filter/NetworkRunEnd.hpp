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
 * One report per subsystem (card NOM2 step TD2): a subsystem, the file a report lives in,
 * has one row.  A subsystem that prints several blocks writes each as a file-local section
 * (`static int networkXxxRunEnd(const NetworkRunEndView& v)`, gated on its own knob) and
 * its row's report lists them in print order with @ref networkRunEndSections.  The walk
 * (@ref networkRunEndCallRow) prints one heading line, `Network[run-end] <row name>`, ahead
 * of the lines of every row that prints, so the run-end log reads by subsystem.
 *
 * Adding a run-end line: it goes in its subsystem's report, or in a new section of it,
 * beside the subsystem's code.  Adding a report (a subsystem with no row yet):
 *   1. Write the function in the subsystem's .cpp and declare it, with its Doxygen (what it
 *      reports, the knob that gates it, what it returns), in the subsystem's header.  When
 *      the subsystem's state lives in one of the large files (app/NetworkLocalCarryHarvest.cpp,
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
 *     Since TD2 the readers that took first or last matches read the run-end block in
 *     message order (scripts/grade_slip_agreement.py, scripts/refound_grade.py).
 *   - A subsystem whose sections must sit at two places in this order (the run artifacts:
 *     the trace close first, the manifest's complete record after every record, the run
 *     summary last) keeps one row per place; of the run artifacts only the run summary
 *     prints.
 */

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
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
inline constexpr std::size_t kNetworkRunEndReportCount = 82;

/// The run-end reports in print order (defined in filter/NetworkRunEnd.cpp).
extern const std::array<NetworkRunEndRow, kNetworkRunEndReportCount> kNetworkRunEndReports;

/**
 * @brief A subsystem's run-end report as its sections in print order (card NOM2 step TD2).
 *
 * Calls every section in order with the same view and combines their verdicts by the
 * table's rule: a later non-zero verdict replaces an earlier one.
 *
 * @param v        The run at its end.
 * @param sections The subsystem's file-local sections, in print order.
 * @return 0, or the last non-zero verdict of the sections.
 */
int networkRunEndSections(const NetworkRunEndView& v,
                          std::initializer_list<NetworkRunEndReport> sections);

/**
 * @brief Runs one report with its heading line (card NOM2 step TD2).
 *
 * The lines @p report writes through @p lg are held while it runs (the logger's sinks are
 * swapped for a holding sink) and are then written to the logger's own sinks, unchanged,
 * after one heading line `Network[run-end] <name>` (info, stamped with the first held line's
 * time and thread).  A report that writes nothing gets no heading.  Lines written through
 * another logger (the consumer's rt_engine summaries) are not held and get no heading.  On an
 * exception the held lines are written before it propagates.
 *
 * @param lg     The logger whose lines are headed (the run end's main_log logger).
 * @param name   The heading text after `Network[run-end] `.
 * @param report The report to run.
 * @return The report's verdict.
 */
int networkRunEndWithHeading(spdlog::logger& lg, const char* name,
                             const std::function<int()>& report);

/**
 * @brief Calls one row of the table with its heading line: @ref networkRunEndWithHeading on
 *        the view's logger with the row's name.
 *
 * @param row The row.
 * @param v   The run at its end.
 * @return The row's verdict.
 */
int networkRunEndCallRow(const NetworkRunEndRow& row, const NetworkRunEndView& v);

} // namespace GnssProcessor
