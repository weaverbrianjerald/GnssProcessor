/**
 * @file NetworkSecondCodeScope.hpp
 * @brief Primitives of the second code channel (card SCC,
 *        docs/refound/SCC_second_code_channel_2026-09-21.md).
 *
 * With one canonical code per station and band, a satellite's C1W code bias is
 * supported only by the stations that latched C1W, so the C1C and C1W satellite
 * families compete for the same stations instead of sharing them.  With
 * `network.second_code_channel` a station also contributes, as code-only rows,
 * every allow-listed band-1 code it reports besides its latched canonical one.
 *
 * Three decisions in that mechanism are pure functions of their inputs, so they
 * are kept here and unit-tested directly (tests/test_network_second_code_scope.cpp):
 *
 *  - @ref secondCodeAllowEntryValid: what the loader accepts in
 *    `network.second_code_signals` (a 3-character code type on band 1; band 2 is
 *    left to a follow-up card because it would activate the untested bridging
 *    path of `network.sat_isc_state`);
 *  - @ref secondCodeCompanions: which codes a station contributes for one
 *    satellite, given what it reports, what it already selected, and the
 *    allow-list;
 *  - @ref secondCodeCellEdges: the two edges of standing invariant #7 on the
 *    (station, satellite, signal) cells the mechanism opens and closes.
 *
 * Nothing here touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, and the network
 * assembly in ApplicationNetworkAssembly.cpp) supply everything.  The key make /
 * parse pair for the receiver inter-signal state lives with the other state keys
 * in filter/StateKeySchema.hpp §4d.
 */
#ifndef GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Tests whether @p entry is a valid `network.second_code_signals` entry
 *        (card SCC).
 *
 * Pure.  Accepts only a 3-character RINEX observation code of type 'C' whose band
 * digit is '1'.  Every other entry is refused rather than clamped: a phase type
 * belongs to a different card (the exact-integer slip witness), and a band-2
 * entry would make the `network.sat_isc_state` bridging roster non-empty, which
 * card SCC defers.
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
 * canonical code beside it is the fallback case, which the receiver side (R2)
 * routes through the inter-signal state and counts, and which is not a companion.
 * (PppModel::selectUncombinedSignals skips the band before calling when the
 * selected code is absent from the epoch's measurements.)
 *
 * @param reported_codes Codes this station reports for this satellite this epoch
 *                       (already filtered by the caller's coverage allow-list).
 * @param selected_code  The code the band map already selected, or empty.
 * @param allow_list     `network.second_code_signals`, in configured order.
 * @return The companion codes, in allow-list order, without duplicates.
 */
std::vector<std::string> secondCodeCompanions(
    const std::set<std::string>& reported_codes,
    const std::string& selected_code,
    const std::vector<std::string>& allow_list);

/**
 * @brief The two edges of standing invariant #7 on the open-cell set.
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the second code channel.
 *
 * The second code channel (card SCC): companion cells, rows and the columns they opened,
 * with the rows summed from the stations into a local copy of the tallies.
 *
 * Gate: returns 0 at once unless `config_.networkSecondCodeRowsActive()`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSecondCodeScopeRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
