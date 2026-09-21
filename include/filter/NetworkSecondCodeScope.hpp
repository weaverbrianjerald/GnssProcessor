/**
 * @file NetworkSecondCodeScope.hpp
 * @brief The SECOND CODE CHANNEL primitives of card SCC
 *        (docs/refound/SCC_second_code_channel_2026-09-21.md).
 *
 * One canonical code per station and band means a satellite's C1W code bias is
 * supported only by the stations that latched C1W, so the C1C and C1W satellite
 * families compete for the same roster instead of sharing it.  With
 * `network.second_code_channel` a station contributes, as CODE-ONLY rows, every
 * allow-listed band-1 code it reports besides its latched canonical one.
 *
 * Three decisions in that mechanism are pure functions of their inputs, and they
 * are the ones a defect would hide in, so they live here and are unit-tested
 * directly (tests/test_network_second_code_scope.cpp):
 *
 *  - @ref secondCodeAllowEntryValid — what the loader accepts in
 *    `network.second_code_signals` (a 3-character code type on band 1; band 2 is
 *    a follow-up card because it would wake the untested bridging path of
 *    `network.sat_isc_state`);
 *  - @ref secondCodeCompanions — which codes a station contributes for one
 *    satellite, given what it reports, what it already selected, and the
 *    allow-list;
 *  - @ref secondCodeCellEdges — the two edges of standing invariant #7 on the
 *    (station, satellite, signal) cells the mechanism opens and closes.
 *
 * Nothing here touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, and the network
 * teardown) supply everything.  The key make / parse pair for the receiver
 * inter-signal state lives with every other state key, in
 * filter/StateKeySchema.hpp §4d.
 */
#ifndef GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Is @p entry a legal `network.second_code_signals` entry for card SCC?
 *
 * Pure.  Accepts exactly a 3-character RINEX observation code of type 'C' whose
 * band digit is '1'.  Everything else is refused rather than clamped: a phase
 * type is a different card (the exact-integer slip witness), and a band-2 entry
 * would make the `network.sat_isc_state` bridging roster non-empty for the first
 * time, which this card explicitly defers.
 *
 * @param entry The configured string, e.g. "C1W".
 * @param why   Optional; on refusal, set to a one-clause reason for the log.
 * @return True when the entry may be admitted.
 */
bool secondCodeAllowEntryValid(const std::string& entry, std::string* why);

/**
 * @brief The companion codes one station contributes for one satellite on band 1.
 *
 * Pure.  A companion is an allow-listed code the station REPORTS for this
 * satellite that is not the code already selected for the band.  The result is
 * in ALLOW-LIST order (not the order the receiver happens to report), so the
 * added rows are deterministic across stations and across runs, and it never
 * repeats a code.
 *
 * An empty @p selected_code means the band has no selected code this epoch: the
 * caller is then adding rows with nothing to companion, so the answer is empty
 * and the caller counts the case.  That keeps "a second code" from silently
 * becoming "the only code" through this path — a code arriving with no canonical
 * beside it is the FALLBACK case, which the receiver side (R2) still routes
 * through the inter-signal state but which is not a companion.
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
 * RE-COUPLING edge: a satellite-station-signal cell starts contributing a
 * companion row), @p closed the cells in @p prev that are gone from @p cur (the
 * DECOUPLING edge).  Both outputs are cleared first and come back sorted, since
 * both inputs are ordered sets.  Either pointer may be null.
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

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_CODE_SCOPE_HPP
