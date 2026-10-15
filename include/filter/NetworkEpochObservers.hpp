#pragma once
/**
 * @file NetworkEpochObservers.hpp
 * @brief The network producer's measure-only per-epoch instruments as a declared table
 *        (card NOM3 step EP3), the epoch's companion of filter/NetworkRunEnd.hpp.
 *
 * An observer is a seam of the observer form (app/NetworkEpoch.hpp):
 *
 *     void networkXxx(NetworkRun& run, const NetworkEpoch& ep);
 *
 * It reads the run and the epoch and writes only its own instrument state (counters,
 * trace rows, events, run-manifest records, its own carried accumulators): nothing the
 * solve, the wire or a later non-observer stage reads.  It gates itself on the knob that
 * arms it, on its first line after its bindings, so an unarmed observer returns at once.
 *
 * The spine (Application::networkProcessEpoch_, app/ApplicationNetworkEpoch.cpp) calls
 * @ref networkObserveEpoch at each spine point, a @ref NetworkObserverSlot, and that walks
 * the rows of @ref kNetworkEpochObservers in that slot in table order.  The table in
 * filter/NetworkEpochObservers.cpp lists every row with what it measures and its home file,
 * in spine order, so it reads as the epoch's table of instruments.
 *
 * Adding an observer:
 *   1. Write the seam in the observer form in the subsystem's .cpp, gate it on its first
 *      line after the bindings, and declare it with its Doxygen in the subsystem's header.
 *   2. Add one row to kNetworkEpochObservers in the slot where its inputs are final (below),
 *      at the position within the slot its order constraints allow, and raise
 *      kNetworkEpochObserverCount by one.
 *   3. Add its line to tests/test_network_epoch_observers.cpp (which pins every row) and
 *      its observer-form line to tests/test_network_epoch_contract.cpp.
 * A seam that writes the epoch, or anything a later non-observer stage reads, is not an
 * observer: it is an explicit spine call.  Not rows (step EP2's observer-form seams, kept as
 * explicit spine calls): networkGapResumeCensus, networkBufferedTraceDrains.
 *
 * The slots, in spine order, and why each observer sits there:
 *   - Admitted: after networkAdmitEpoch, before the section-timer baseline.  The satellite
 *     ids of no modelled constellation, and of SBAS, are counted on every processed epoch
 *     before any model reads its observations.
 *   - BeforeColumns: after networkReceiverPhaseBiasAllocation, before
 *     networkAssemblyRegisterColumns_.  The level-authority census (LV-6) runs after PASS A's
 *     CS-GF advance, before the registration's SkipEpoch (an empty-system epoch still gets its
 *     census row) and upstream of every withhold decision (it measures through withhold eras).
 *   - AfterClockQc: after SB_CLOCKUPD closes, before SB_ROSTER opens.  The phase-visible
 *     instrument (card PVN) runs after the epoch's last re-solve, while the fold capture
 *     still holds the prior (the stash at SB_STASH_STATUS overwrites it).
 *   - RosterTail: inside SB_ROSTER, after networkSignalRosterAudit.  The ledger emit's fold
 *     half needs the ledger's admitted machine the audit has just advanced (an admitted arc
 *     birth at this epoch does not exist before the audit).  The second phase channel's tail
 *     needs relevel.free_cause with every tier's verdict, the post-solve RAIM ones included,
 *     and the elected phase-datum anchors.  The all-channel and multi-GNSS censuses need
 *     `present`, final here.  The parameters-as-armed and contributions-as-armed tables are
 *     written once, after the first emitted epoch.
 *   - FrameDiag: inside SB_FRAMEDIAG, after networkBlockBaardaPredict and
 *     networkCongruencePredict, before SB_STASH_STATUS.  Predict-then-update: each family's
 *     and station's sigma0 through the previous epoch is frozen before the census's rider
 *     feeds this epoch, so a block cannot standardize itself away.  The stash's epoch-status
 *     row reads the post-fit sigma0 and the prefit-census scalars these rows fill in.
 *   - StashTail: inside SB_STASH_STATUS, after networkClockLevelMonitor, before
 *     networkBufferedTraceDrains.  The carry-state drain is the first point after every row
 *     source has run: the leveling harvest and the gap prune (SB_LEVELING), the T4 / T3
 *     refloats (SB_RAIM) and the phase-anchor carry re-reference (the datum block).
 *
 * The order constraints within a slot (the table is walked top to bottom):
 *   - RosterTail: the parameters-as-armed table before the contributions-as-armed table
 *     (their log tables and run-manifest records are read in that order).  The other four
 *     share no state; their counters, lines and ARC events keep the table's interleaving.
 *   - FrameDiag:
 *     - networkPostfitSigma0 before networkMdbFlush: the MDB's scaled variant needs this
 *       epoch's per-family sigma0 (read earlier it applies the previous epoch's inflation).
 *     - networkShSpectrum and networkStationBlock: the reduced shared-normal spectrum
 *       and its station-side companion, kept beside the post-fit pass.
 *     - networkPrefitCensus before networkRedundancyCensus: the prefit walk is the free tier
 *       the redundancy census escalates from, and it writes the epoch-status scalars.
 *     - networkRedundancyCensus before networkBlockBaardaEmit, networkCongruenceEmit and
 *       networkMdbFlush (it feeds their accumulators, which they drain; the MDB join compares
 *       its per-epoch row counts), and before networkRhsAttribution (a flight-recorder trip
 *       forces a full sweep, and the trip predicate is the attribution's own unconsumed
 *       window test, read before the attribution advances rhs_attrib_done).
 *     - networkBlockBaardaEmit and networkCongruenceEmit: free relative to each other (the
 *       separability join was resolved at the sweep site); kept.
 *     - networkDatumGraphCensus: free within the slot (no shared accumulator); kept last
 *       among the censuses, ahead of the attribution rows that consume the trip predicate.
 *     - networkRhsAttribution, then networkRhsAttribTrip last: the flight recorder consumes a
 *       trip raised anywhere earlier this epoch and retains this epoch as the next trip's
 *       "before" frame, so a trip dump sees the kernel state every other row just read.
 * Every observer's output is byte-identical to the explicit calls it replaced (card NOM3
 * step EP3, gate G-EP), so a row moves only with a gate of its own.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include "app/NetworkEpoch.hpp"   // NetworkEpochObserver

namespace GnssProcessor {

struct NetworkRun;

/// A spine point at which observers run (spine order; see the file comment for each).
enum class NetworkObserverSlot : std::uint8_t {
    Admitted,       ///< After the admission, before the section-timer baseline.
    BeforeColumns,  ///< After the receiver phase-bias allocation, before the column registration.
    AfterClockQc,   ///< After SB_CLOCKUPD closes, before SB_ROSTER opens.
    RosterTail,     ///< Inside SB_ROSTER, after the roster audit.
    FrameDiag,      ///< Inside SB_FRAMEDIAG, after the block-Baarda and congruence predict halves.
    StashTail,      ///< Inside SB_STASH_STATUS, after the clock-level monitor, before the buffered drains.
};

/// One row of the observer table.
struct NetworkEpochObserverRow {
    const char*          name;     ///< The seam's function name.
    const char*          what;     ///< What it measures, in a few words.
    const char*          home;     ///< The file the seam lives in (under src/core/).
    NetworkObserverSlot  slot;     ///< The spine point it runs at.
    NetworkEpochObserver observe;  ///< The seam.
};

/// The number of rows of @ref kNetworkEpochObservers.
inline constexpr std::size_t kNetworkEpochObserverCount = 21;

/// The observers in spine order (defined in filter/NetworkEpochObservers.cpp).
extern const std::array<NetworkEpochObserverRow, kNetworkEpochObserverCount> kNetworkEpochObservers;

/**
 * @brief Runs, in table order, every observer of @p slot.
 * @param run   The run (the observers write their own instrument state in it).
 * @param ep    The epoch, read only.
 * @param slot  The spine point the caller stands at.
 */
void networkObserveEpoch(NetworkRun& run, const NetworkEpoch& ep, NetworkObserverSlot slot);

} // namespace GnssProcessor
