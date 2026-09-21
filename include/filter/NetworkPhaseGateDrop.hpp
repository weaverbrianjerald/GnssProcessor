#pragma once
/**
 * @file NetworkPhaseGateDrop.hpp
 * @brief Card EV7 §6 — the COVERAGE-GATE DROP EDGE predicate
 *        (@c network.phase_datum_gate_drop_erase).
 *
 * The defect this closes (docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md
 * §5.3, measured).  The coverage gate @c network.phase_datum_min_stations removes
 * a (satellite, signal) phase-bias column when fewer than k present stations track
 * it.  The gate's own comment states its assumption: "A gated key's PHASE rows
 * still serve the satellite clock: each station's float ambiguity absorbs the
 * bias".  On the FLOAT vehicle the surviving station's ambiguity is a CARRIED
 * state, so it absorbs the whole removed bias in ONE epoch against a centimetre
 * prior, with no refloat, no re-seed and no variance inflation: VOIM00MDG G30 L1W
 * moved +15.829 m at TOW 278520 against a variance of 2.8e-4 m^2, and every
 * emitted satellite clock tripped the Q3 anchor gate in the same epoch.
 *
 * The RETURN edge is already handled — a family with no incumbent is inserted into
 * the election's re-reference set and @ref networkApplyPhaseAnchorCarryReref erases
 * every station's carried ambiguity of it.  Only the DROP edge has no response
 * (standing invariant #7: the decoupling edge must bound what the decoupled states
 * can accumulate).  This header holds that edge's rule, and nothing else:
 *
 *   drop edge = a key the coverage gate excluded THIS epoch
 *               AND that had an anchor incumbent from the PREVIOUS epoch.
 *
 * The second clause is what makes it an EDGE rather than a state.  The election
 * swaps @c anchor_incumbent at the end of every epoch and a gated key is never
 * elected (the gate erases it from @c union_keys before the election's loop), so
 * on the epochs AFTER the edge the key has no incumbent and the predicate is false
 * by construction — the re-born ambiguity absorbs the bias once and is then
 * carried normally, which is requirement F1's "at the edge only".
 *
 * Kept a free, pure function over two plain containers — no context, no config, no
 * estimator — so the unit suite can state the edge rule directly and the caller
 * stays the only scope that knows about traces, counters and the erase set.
 */

#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief One coverage-gate DROP EDGE, as the election hands it back to the driver.
 *
 * Identities on BOTH sides of the edge (standing invariant #7): what left, who was
 * holding its datum, and who is left carrying it.
 */
struct PhaseGateDropEvent {
    std::string sat;             ///< satellite id (parsed from the ambiguity key)
    std::string signal;          ///< RINEX signal id
    std::string prev_anchor;     ///< station that anchored the family in the PREVIOUS epoch
    std::string survivors;       ///< '|'-joined names of the present stations still tracking it
    int         n_trackers{0};   ///< how many present stations still track it (< min_stations)
    bool        handled{false};  ///< the knob was ON and the family entered the erase set
};

/**
 * @brief The DROP-EDGE predicate: which gated keys were alive last epoch.
 *
 * Pure.  Neither argument is modified and nothing else is read.
 *
 * @param gated_keys        This epoch's coverage-gate exclusions, as ambiguity keys
 *                          ("<sat>_<signal>") — @c PhaseDatumCandidates::gated_keys.
 *                          Empty whenever @c network.phase_datum_min_stations is 1.
 * @param anchor_incumbent  The PREVIOUS epoch's anchor map, ambiguity key -> station
 *                          name, read BEFORE the election's incumbent swap.  A key
 *                          present here was a live, anchored family last epoch.
 * @return The gated keys that had an incumbent, in the same "<sat>_<signal>" form.
 *         Empty when either input is empty, which is the whole OFF path's cost.
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
