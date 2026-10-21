/**
 * @file SecondCodeScope.hpp
 * @brief Primitives of the second code channel.
 *
 * With one canonical code per station and band, a satellite's C1W code bias is
 * supported only by the stations that latched C1W, so the C1C and C1W satellite
 * families compete for the same stations instead of sharing them.  With
 * `network.second_code_channel` a station also contributes, as code-only rows,
 * every allow-listed band-1 code it reports besides its latched canonical one.
 *
 * Three decisions in that mechanism are pure functions of their inputs, so they
 * are kept here and unit-tested directly (tests/network/test_network_second_code_scope.cpp):
 *
 *  - @ref secondCodeAllowEntryValid: what the loader accepts in
 *    `network.second_code_signals` (a 3-character code type on band 1; band 2 is
 *    refused because it would activate the untested bridging
 *    path of `network.sat_isc_state`);
 *  - @ref secondCodeCompanions: which codes a station contributes for one
 *    satellite, given what it reports, what it already selected, and the
 *    allow-list;
 *  - @ref secondCodeCellEdges: the opening (re-coupling) and closing (decoupling)
 *    edges of the (station, satellite, signal) cells the mechanism opens and closes.
 *
 * None of the three touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, and the network
 * PASS A slip scan in network/slip/PassASlipScan.cpp) supply everything.  The key make /
 * parse pair for the receiver inter-signal state lives with the other state keys
 * in filter/StateKeySchema.hpp.
 */
#ifndef GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Tests whether @p entry is a valid `network.second_code_signals` entry.
 *
 * Pure.  Accepts only a 3-character RINEX observation code of type 'C' whose band
 * digit is '1'.  Every other entry is refused rather than clamped: a phase type
 * belongs to a different mechanism (`network.second_phase_channel`), and a band-2
 * entry would make the `network.sat_isc_state` bridging roster non-empty, which
 * the second code channel defers.
 *
 * @param entry The configured string, e.g. "C1W".
 * @param why   Optional; on refusal, set to a one-clause reason for the log.
 * @return True when the entry may be admitted.
 */
bool secondCodeAllowEntryValid(const std::string& entry, std::string* why);

/**
 * @brief The companion codes one station contributes for one satellite on band 1.
 *
 * Pure.  A companion is an allow-listed code that the station reports for this
 * satellite and that is not the code already selected for the band.  The result
 * follows the allow-list order (not the receiver's reporting order), so the added
 * rows are deterministic across stations and runs, and it never repeats a code.
 *
 * An empty @p selected_code means the band has no selected code this epoch, so
 * there is nothing to accompany and the result is empty.  This keeps a second
 * code from becoming the only code through this path: a code arriving with no
 * canonical code beside it is the fallback case, which the receiver side
 * routes through the inter-signal state and counts, and which is not a companion.
 * (PppModel::selectUncombinedSignals skips the band before calling when the
 * selected code is absent from the epoch's measurements.)
 *
 * @param reported_codes Codes this station reports for this satellite this epoch
 *                       (already filtered by the caller's coverage gate).
 * @param selected_code  The code the band map already selected, or empty.
 * @param allow_list     `network.second_code_signals`, in configured order.
 * @return The companion codes, in allow-list order, without duplicates.
 */
std::vector<std::string> secondCodeCompanions(
    const std::set<std::string>& reported_codes,
    const std::string& selected_code,
    const std::vector<std::string>& allow_list);

/**
 * @brief The opening and closing edges of the open-cell set.
 *
 * Pure.  @p opened receives the cells in @p cur that were not in @p prev (the
 * re-coupling edge: a station-satellite-signal cell starts contributing a
 * companion row); @p closed receives the cells in @p prev that are absent from
 * @p cur (the decoupling edge).  Both outputs are cleared first and come back
 * sorted, since both inputs are ordered sets.  Either pointer may be null.
 *
 * @param prev   Open cells of the previous epoch.
 * @param cur    Open cells of this epoch.
 * @param opened Out; cells that opened.  May be null.
 * @param closed Out; cells that closed.  May be null.
 */
void secondCodeCellEdges(const std::set<std::string>& prev,
                         const std::set<std::string>& cur,
                         std::vector<std::string>* opened,
                         std::vector<std::string>* closed);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the second code channel.
 *
 * The second code channel: companion cells, rows and the columns they opened,
 * with the rows summed from the stations into a local copy of the tallies.
 *
 * Gate: returns 0 at once unless `config_.networkSecondCodeRowsActive()`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSecondCodeScopeRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of SecondCodeScope, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SecondCodeScopeCounters {
    /// Run-end telemetry of the second code channel, including both cell edges.
    /// Everything except @c rows / @c fallback_rows is reduced in the serial post-PASS-A
    /// sweep; those two are summed from the per-station NetStation counters at
    /// teardown.
    ///
    /// @c rows == 0 and @c cells == 0 with the channel armed means the run exercised
    /// nothing (no station reported a second allow-listed band-1 code), and the teardown
    /// warns instead of reporting a clean run.
    struct SecondCode {
        long long cells = 0;          ///< Companion (station, satellite, signal) cells open, summed over epochs.
        long long cell_open = 0;      ///< Cell openings (the re-coupling edge): a cell that was not open last epoch.
        long long cell_close = 0;     ///< Cell closings (the decoupling edge): a cell that was open last epoch and is gone.
        long long rows = 0;           ///< Companion code rows folded into the solve, over the run.
        long long fallback_rows = 0;  ///< Rows routed through the inter-signal state that were not companions (the fallback case).
        long long isc_states_max = 0; ///< Receiver inter-signal states alive network-wide, maximum over epochs.
        long long isc_reseeds = 0;    ///< States created at a station that had held that state before and was not restored by the receiver code-bias carry.
        /// (satellite, signal) columns that reached the >= 2-station registration bar
        /// only through companions, i.e. that would have had fewer than two supporting
        /// stations without the second code channel.  Per epoch, summed.  It measures
        /// the premise directly: zero of these means the second code channel
        /// added rows without ever adding a column the wire did not already carry.
        long long cols_via_companion = 0;
    };
    SecondCode second_code;
};

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
