#pragma once
/**
 * @file NetworkPivotHandover.hpp
 * @brief Pure detector for a PPP-RTK network clock-pivot datum re-reference (A1/A2).
 *
 * The network's per-constellation clock gauge is realized by pinning one satellite's
 * clock per constellation (@ref NetworkSbasis::buildSharedDatum).  When that pinned
 * pivot satellite changes, the whole constellation's estimable clock / phase-bias /
 * code-bias solution re-references to a new datum, and the emitted product must
 * signal a discontinuity so that a consumer resets the ambiguities that integrated
 * the old frame.
 *
 * The driver detects this by comparing this epoch's `clock_pivot` map (constellation
 * char → pivot sat) against the previous epoch's.  An adjacent-epoch check alone
 * misses a constellation that drops out of the registered set for one epoch and
 * returns with a different pivot: against the immediately-previous map it reads as a
 * first sighting and raises no signal (A1).  This helper adds an opt-in second arm
 * that consults a persistent last-ever-pivot map, and records which constellations
 * changed so that the emit bump can be scoped per constellation (A2) instead of
 * applying to every constellation.
 *
 * Pure (only std::map / std::set of strings/chars), so the detection logic is
 * unit-testable without a network run and lives here, not inline in the driver.
 */

#include "common/GnssIdentity.hpp"   // letterOfSat

#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief Detect clock-pivot datum re-references this epoch and record which
 *        constellations changed.
 *
 * Two arms:
 *  - **(a) adjacent-epoch (always on):** a constellation present last epoch whose
 *    pivot differs from @p prev_pivot.
 *  - **(b) vanish-return (opt-in via @p check_last_ever):** a constellation absent
 *    last epoch that returns with a pivot != its last-ever pivot (@p last_pivot, the
 *    persistent merge-updated last-ever-pivot map).  A first-ever sighting (absent
 *    from both @p prev_pivot and @p last_pivot) is not a change.
 *
 * @param clock_pivot     This epoch's constellation-char → pivot-sat map.
 * @param prev_pivot      The immediately-previous epoch's pivot map.
 * @param last_pivot      Persistent last-ever pivot per constellation (never erased).
 * @param check_last_ever Enable arm (b) (@c network.pivot_vanish_return_signal).
 * @param[out] changed_consts Leading char of every constellation whose pivot changed.
 * @return true iff any constellation's clock pivot re-referenced this epoch.
 */
inline bool pivotHandoverSignalled(
        const std::map<std::string, std::string>& clock_pivot,
        const std::map<std::string, std::string>& prev_pivot,
        const std::map<std::string, std::string>& last_pivot,
        bool check_last_ever,
        std::set<char>& changed_consts) {
    bool changed = false;
    for (const auto& kv : clock_pivot) {
        bool this_changed = false;
        const auto it = prev_pivot.find(kv.first);
        if (it != prev_pivot.end()) {
            if (it->second != kv.second) this_changed = true;          // arm (a)
        } else if (check_last_ever) {
            const auto lit = last_pivot.find(kv.first);
            if (lit != last_pivot.end() && lit->second != kv.second)
                this_changed = true;                                   // arm (b)
        }
        if (this_changed) {
            changed = true;
            if (!kv.first.empty()) changed_consts.insert(kv.first.front());
        }
    }
    return changed;
}

/**
 * @brief Classify whether a carried key belongs to the scope of this epoch's
 *        datum re-reference (RTA-2, @c network.scoped_handover_response).
 *
 * On a pivot handover the producer invalidates carried cross-epoch state (skips
 * the D2 shared-carry priors, clears the two-state clock oscillators, suppresses
 * the clock-monitor jump baseline).  The legacy response is global; the scoped
 * response applies it only to keys of the constellation(s) whose datum changed,
 * using the same @p changed_consts set that @ref pivotHandoverSignalled records
 * for the A2 emit-bump scoping (one source, never re-derived).
 *
 * A key's constellation is read from its satellite id (@c systemOfSat, compared as its
 * RINEX letter).  A key with no satellite (empty @p sat, e.g. a receiver-side or other
 * constellation-less shared column, should one ever be carried) is treated as
 * affected even when scoping is on, matching the legacy global invalidation for
 * anything whose datum membership is unknown.
 *
 * Callers must also gate on "a datum change happened this epoch": this helper
 * answers only the scope question.  With @p scoped false it returns true
 * unconditionally, so `datum_changed && datumChangeAffectsKey(...)` reproduces
 * the legacy global response byte-identically when the toggle is off.
 *
 * @param sat            Satellite id of the carried key ("" = constellation-less).
 * @param changed_consts Leading chars of the constellations whose pivot changed
 *                       this epoch (from @ref pivotHandoverSignalled).
 * @param scoped         @c network.scoped_handover_response; false = global
 *                       response (always affected).
 * @return true iff the key's carried state must be treated as retired-datum.
 */
inline bool datumChangeAffectsKey(const std::string& sat,
                                  const std::set<char>& changed_consts,
                                  bool scoped) {
    if (!scoped) return true;       // legacy global response
    if (sat.empty()) return true;   // constellation-less key: conservative
    return changed_consts.count(letterOfSat(sat)) > 0;
}

/**
 * @brief Format a per-constellation pivot map as one compact CSV-safe field (RTB-6 P2).
 *
 * Renders a constellation → pivot-sat map (e.g. the driver's emit-baseline
 * @c last_pivot, the datum of the last emitted product) as
 * <tt>"G:G05;E:E11"</tt>: entries joined with ';' in the map's deterministic
 * key order, each <tt>constellation:pivot_sat</tt>.  Contains no commas, so it
 * embeds directly as a single CSV field (the @c network_service_health.csv
 * @c pivot column).  An empty map (no product emitted yet) renders as the empty
 * string, which the health record uses to mean "no datum realized yet".
 *
 * @param pivot Constellation (e.g. "G") → pivot satellite (e.g. "G05") map.
 * @return Semicolon-joined <tt>const:sat</tt> string; "" for an empty map.
 */
inline std::string formatPivotMap(const std::map<std::string, std::string>& pivot) {
    std::string out;
    for (const auto& kv : pivot) {
        if (!out.empty()) out += ';';
        out += kv.first;
        out += ':';
        out += kv.second;
    }
    return out;
}

}  // namespace GnssProcessor
