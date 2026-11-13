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
 *  - @ref GnssProcessor::secondCodeAllowEntryValid "secondCodeAllowEntryValid": what the loader accepts in
 *    `network.second_code_signals` (a 3-character code type on band 1; band 2 is
 *    refused because it would activate the untested bridging
 *    path of `network.sat_isc_state`);
 *  - @ref GnssProcessor::secondCodeCompanions "secondCodeCompanions": which codes a station contributes for one
 *    satellite, given what it reports, what it already selected, and the
 *    allow-list;
 *  - @ref GnssProcessor::secondCodeCellEdges "secondCodeCellEdges": the opening (re-coupling) and closing (decoupling)
 *    edges of the (station, satellite, signal) cells the mechanism opens and closes;
 *  - @ref GnssProcessor::secondCodeIscRows "secondCodeIscRows": which of a station's code rows route through a receiver
 *    inter-signal state, the rows the station split scales, tags and counts
 *    (tests/network/test_second_code_system_aware.cpp).
 *
 * None of the four touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, the network
 * PASS A slip scan in network/slip/PassASlipScan.cpp and the station split in
 * network/assembly/StationSplit.cpp) supply everything.  The key make /
 * parse pair for the receiver inter-signal state lives with the other state keys
 * in filter/StateKeySchema.hpp.
 */
#ifndef GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP

#include "common/ParamRef.hpp"   // RxCbKey, the receiver code-bias map's key

#include <cstddef>
#include <map>
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

/**
 * @brief The code rows of one station that route through a receiver inter-signal state.
 *
 * Pure.  The receiver bias resolver gives a non-canonical allow-listed code its own
 * state `<system>_ISC_<signal>` (form @c ParamForm::Isc of @p rx_cb), keyed on the
 * constellation and the signal.  The station split reads the states rather than
 * re-deriving them, and this function is that read: a row is returned when it is a code
 * row with a satellite and its signal matches an inter-signal state the station holds.
 *
 * The match has two forms:
 *  - @p system_aware false (the default posture): the observation code alone.  A row of
 *    another constellation with the same code (a Galileo C1C row at a station holding
 *    `GPS_ISC_C1C`) is returned although it routes through no inter-signal state.
 *  - @p system_aware true: the (constellation, observation code) pair, the identity of
 *    the state itself, so only the rows of the state's own constellation are returned.
 *
 * The walk is the one the station split has always made: rows up to
 * min(@p n_rows, @p signal_types.size()), phase and empty signals skipped, the walk
 * stopping at the first matched row beyond @p satellite_ids, and a matched row without a
 * satellite id skipped.  The aware form only removes rows from the unaware result, so with
 * @p system_aware false the result is exactly the historic row set.
 *
 * @param rx_cb          The station's receiver code-bias map (only its Isc keys are read).
 * @param satellite_ids  Satellite id of each row ("G05", "E11").
 * @param signal_types   Observation code of each row ("C1C", "L1C").
 * @param n_rows         Rows in the station's design matrix.
 * @param system_aware   Match on (constellation, code) instead of the code alone.
 * @param other_system   Optional; incremented once per row the code-alone match takes and
 *                       the (constellation, code) match refuses, in both forms, so an
 *                       unarmed run reports what the aware form would leave unscaled.
 * @return The matched row indices, ascending.
 */
std::vector<std::size_t> secondCodeIscRows(const std::map<RxCbKey, int>& rx_cb,
                                           const std::vector<std::string>& satellite_ids,
                                           const std::vector<std::string>& signal_types,
                                           std::size_t n_rows,
                                           bool system_aware,
                                           long long* other_system);

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
        /// Code rows whose observation code matches a station's inter-signal state but whose
        /// constellation holds no such state (a Galileo C1C row beside `GPS_ISC_C1C`),
        /// summed from the stations at teardown like @c rows.  Counted in both forms of
        /// the row test: with `network.second_code_system_aware` off these rows are scaled,
        /// tagged and counted (as fallback) like a second code; on, they keep their own
        /// weight.
        long long other_system_rows = 0;
        long long isc_states_max = 0; ///< Receiver inter-signal states alive network-wide, maximum over epochs.
        long long isc_reseeds = 0;    ///< States created at a station that had held that state before and was not restored by the receiver code-bias carry.
        /// (satellite, signal) columns that reached the >= 2-station registration bar
        /// only through companions, i.e. that would have had fewer than two supporting
        /// stations without the second code channel.  Per epoch, summed.  It measures
        /// the premise directly: zero of these means the second code channel
        /// added rows without ever adding a column the wire did not already carry.
        long long cols_via_companion = 0;
    };
    SecondCode second_code;  ///< The run tallies of the second code channel.
};

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
