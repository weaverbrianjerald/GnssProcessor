#pragma once

/**
 * @file CycleSlipCounters.hpp
 * @brief Run-end counters of the post-processing slip layer
 *        (@c CycleSlipManager), and the table they print.
 *
 * Standing invariant 4 (run-end counters are the ON-path telemetry):
 * @c CycleSlipManager accumulates one
 * @c CycleSlipCounters over the run and prints it when the manager is destroyed
 * at run end (nothing is printed for a manager that processed no epoch).
 *
 * Every counter is defined at its increment site in the slip layer; the units
 * are part of each row's label, because they differ (result entries, ambiguity
 * keys, satellites, constellations).
 */

#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief The slip layer's counters over one post-processing run.
 */
struct CycleSlipCounters {
    /// Epochs processed (@c CycleSlipManager::detectAndRepair calls).
    long long epochs{ 0 };
    /// Detections per built detector, in the registry's order (MW first): the
    /// detector's token (@c toString(SlipDetectorKind)) and its count.  MW
    /// counts its result entries (one per mandated ambiguity key of a flagged
    /// pair, and one per key on a receiver loss of lock); GRAPHIC and EWL count
    /// their slip events.  One row per detector the registry built, so a
    /// detector that never fired prints 0.
    std::vector<std::pair<std::string, long long>> detections;
    /// Distinct mandated ambiguity keys, summed over satellites and epochs.
    long long mw_mandates{ 0 };
    /// Covariance-preserving integer repairs applied (x += dN·λ): the
    /// pre-handover repairs (culprits, the in-place pivot frame, TurboEdit
    /// GF-only) and the post-handover deferred repairs.  Soft-LLI bridge
    /// restores are counted under @c soft_lli_bridge_repairs.
    long long integer_repairs{ 0 };
    /// Calls of the BSSD-aware ambiguity reset (one per ambiguity key; for a
    /// BSSD pivot the call inflates the variance to force a handover).
    long long ambiguity_resets{ 0 };
    /// Satellites that took the full-reset fall-through (every mandated key
    /// reset, then the LLI arc handling and the satellite-wide re-inflation).
    long long full_satellite_resets{ 0 };
    /// Satellites whose held ambiguities the soft-LLI bridge integer-repaired.
    long long soft_lli_bridge_repairs{ 0 };
    /// Mandated ambiguity keys left untouched: adjudicated by TurboEdit as
    /// needing no state action, or held while a WL-settle deferral is pending.
    long long suppressed_mandates{ 0 };
    /// Constellations whose pivot a forced handover changed (a refused
    /// handover is not counted; its deferred repairs fall back to resets).
    long long forced_pivot_handovers{ 0 };
    /// Constellations whose pivot frame was repaired in place (pivot kept,
    /// peers shifted by the relative integer), avoiding a handover.
    long long pivot_frames_repaired_in_place{ 0 };
};

/**
 * @brief The run-end table of a @c CycleSlipCounters value, one line per row:
 *        a title with the epoch count, the detection rows in detector order,
 *        then the other counters in declaration order.
 * @param counters The run's counters.
 * @return The lines, without trailing newlines; the labels state each row's
 *         unit.
 */
std::vector<std::string> cycleSlipCounterLines(const CycleSlipCounters& counters);

} // namespace GnssProcessor
