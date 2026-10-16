#pragma once
/**
 * @file PhaseGateDrop.hpp
 * @brief Predicate for the drop edge of the phase-datum coverage gate
 *        (@c network.phase_datum_gate_drop_erase, card EV7 §6).
 *
 * The coverage gate @c network.phase_datum_min_stations removes a (satellite,
 * signal) phase-bias column when fewer than k present stations track it.  The gate
 * assumes that the phase rows of a gated key still serve the satellite clock because
 * each station's float ambiguity absorbs the bias.  On the float vehicle the
 * surviving station's ambiguity is a carried state at a centimetre prior, so it
 * absorbs the whole removed bias in one epoch with no refloat, no re-seed and no
 * variance inflation, and the remainder reaches the emitted satellite clocks
 * (measured in docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md §5.3).
 *
 * The return edge already has a response: a family with no incumbent is inserted
 * into the election's re-reference set, and @ref networkApplyPhaseAnchorCarryReref
 * erases every station's carried ambiguity of it.  The drop edge is the decoupling
 * edge of standing invariant #7, which must bound what the decoupled states can
 * accumulate.  This header holds only that edge's rule:
 *
 *   drop edge = a key the coverage gate excluded in this epoch
 *               and that had an anchor incumbent from the previous epoch.
 *
 * The second clause makes the rule an edge rather than a state.  The election swaps
 * @c anchor_incumbent at the end of every epoch, and a gated key is never elected
 * (the gate erases it from @c union_keys before the election loop), so on the epochs
 * after the edge the key has no incumbent and the predicate is false by
 * construction.  The re-born ambiguity absorbs the bias once and is then carried
 * normally (requirement F1, "at the edge only").
 *
 * The rule is a free, pure function over two plain containers (no context, config
 * or estimator), so the unit suite can test it directly and the caller remains the
 * only scope that handles traces, counters and the erase set.
 */

#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief One coverage-gate drop edge, as the election returns it to the driver.
 *
 * Carries the identities on both sides of the edge (standing invariant #7): what
 * left, which station held its datum, and which stations still carry it.
 */
struct PhaseGateDropEvent {
    std::string sat;             ///< satellite id (parsed from the ambiguity key)
    std::string signal;          ///< RINEX signal id
    std::string prev_anchor;     ///< station that anchored the family in the previous epoch
    std::string survivors;       ///< '|'-joined names of the present stations still tracking it
    int         n_trackers{0};   ///< how many present stations still track it (< min_stations)
    bool        handled{false};  ///< the knob was on and the family entered the erase set
};

/**
 * @brief Drop-edge predicate: returns the gated keys that were anchored last epoch.
 *
 * Pure.  Neither argument is modified and nothing else is read.
 *
 * @param gated_keys        This epoch's coverage-gate exclusions, as ambiguity keys
 *                          ("<sat>_<signal>"), from @c PhaseDatumCandidates::gated_keys.
 *                          Empty whenever @c network.phase_datum_min_stations is 1.
 * @param anchor_incumbent  The previous epoch's anchor map, ambiguity key -> station
 *                          name, read before the election swaps its incumbents.  A key
 *                          present here was a live, anchored family last epoch.
 * @return The gated keys that had an incumbent, in the same "<sat>_<signal>" form.
 *         Empty when either input is empty, so with the gate off the call costs two
 *         emptiness tests.
 */
inline std::set<std::string> networkPhaseGateDropEdges(
        const std::set<std::string>& gated_keys,
        const std::map<std::string, std::string>& anchor_incumbent) {
    std::set<std::string> out;
    if (gated_keys.empty() || anchor_incumbent.empty()) return out;
    for (const std::string& amb : gated_keys)
        if (anchor_incumbent.count(amb)) out.insert(amb);
    return out;
}

} // namespace GnssProcessor
