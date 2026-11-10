#pragma once
/**
 * @file ZwdEdge.hpp
 * @brief Both edges of the wet delay's event response on the float vehicle, as one record per
 *        station: why the station's carried wet delay last stopped folding, and its first fold
 *        afterwards (standing invariant 7: a returning station is a re-coupling, never a first
 *        sighting).
 *
 * The record (@ref ZwdCoupleEdge, the station field @c net::StationState::zwd_edge) follows one
 * station's carried wet delay through four moments:
 *
 * | moment                          | writer                                            | record                                   |
 * | ------------------------------- | ------------------------------------------------- | ---------------------------------------- |
 * | decoupling edge (entry removed) | slip-latch erase, RAIM per-arc erase, prior-channel erase, gap prune | @ref zwdEdgeDecouple: cause, second, satellite |
 * | the harvest re-creates the entry | @c networkHarvestZwd                             | @ref zwdEdgeCreate: creation second (a station with no record is a first sighting) |
 * | the entry's first fold          | @c networkHarvestZwd (the fold is PASS B's record) | @ref zwdEdgeFold: the re-coupling, record cleared |
 * | a fold after an absence          | @c networkHarvestZwd                             | @ref zwdEdgeFold: cause @c Return (the entry was kept within the gap horizon) |
 *
 * A re-created entry that is removed again before its first fold never re-coupled: the second
 * decoupling supersedes the first, and @ref zwdEdgeDecouple reports it.  A record still waiting
 * at the run end is counted by @ref zwdEdgePendingByCause.  Every decoupling therefore ends in
 * exactly one of: a re-coupling, a supersession, or a pending record at the run end.
 *
 * Measure-only: nothing on the solve path reads the record.  Written only while the response is
 * armed (NetworkKnobs::zwd_event_on), so an unarmed run leaves the field empty.  Plain value
 * types and free functions; the run-side wiring is in network/carry/ZwdCarry.cpp and
 * network/qc/Raim.cpp.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace GnssProcessor {

/// Why a station's carried wet delay stopped folding (the decoupling edge), or why its next
/// fold is not an ordinary one.
enum class ZwdEdgeCause : std::uint8_t {
    FirstSighting = 0,  ///< The station's first entry: its first fold is a birth, not a re-coupling.
    SlipLatch     = 1,  ///< Erased at a slip latch of the station (networkZwdEventResponse).
    RaimArc       = 2,  ///< Erased at a RAIM per-arc attribution or a prior-channel line-of-sight flag (the arc response of networkPostFitRaim).
    RaimPrior     = 3,  ///< Erased at a prior-channel flag on the wet delay's own block row.
    GapPrune      = 4,  ///< Pruned past the gap horizon: the station was away longer than the horizon.
    Return        = 5,  ///< Not removed: the station came back within the horizon and its entry folds after a gap.
};

/// Number of @ref ZwdEdgeCause values (the size of a per-cause tally).
inline constexpr std::size_t kZwdEdgeCauses = 6;

/// A count per @ref ZwdEdgeCause, indexed by the cause's value.
using ZwdEdgeTally = std::array<long long, kZwdEdgeCauses>;

/**
 * @brief The name of a cause in the events trace and the run-end text.
 * @param c The cause.
 * @return "first_sighting", "slip_latch", "raim_attribution", "raim_prior", "gap_prune" or
 *         "return".
 */
const char* zwdEdgeCauseName(ZwdEdgeCause c);

/// One station's open edge: the last decoupling of its carried wet delay, until the first fold
/// of the entry the harvest re-creates.  Present only between those two moments (and between a
/// first sighting and its first fold).
struct ZwdCoupleEdge {
    ZwdEdgeCause cause{ZwdEdgeCause::FirstSighting};  ///< Why the entry stopped folding (never @c Return: a return keeps its entry).
    double       decoupled_s{0.0};  ///< Continuous-GPS second of the decoupling edge (the creation second for a first sighting).
    double       created_s{0.0};    ///< Second of the harvest that re-created the entry; 0 while the entry is absent.
    std::string  sat;               ///< The satellite of a RAIM per-arc erase; empty for every other cause.
    std::string  via;               ///< The drop cause of a RAIM per-arc erase (@c t4_attrib, @c t4_prior); empty otherwise.
};

/// The re-coupling one fold closes: what @ref zwdEdgeFold hands to the counters and the trace.
struct ZwdRecouple {
    ZwdEdgeCause cause{ZwdEdgeCause::FirstSighting};  ///< The cause of the edge it closes (@c FirstSighting: a first fold, not a re-coupling).
    double       away_s{0.0};   ///< Seconds from the decoupling edge to this fold (the gap for @c Return; 0 for a first sighting).
    std::string  sat;           ///< The satellite of a RAIM per-arc erase; empty otherwise.
    std::string  via;           ///< The drop cause of a RAIM per-arc erase; empty otherwise.
};

/**
 * @brief The decoupling edge: records why and when the station's carried wet delay was removed.
 *
 * Called by every site that removes the entry (it must have removed one).  A record that was
 * already open with a re-created entry (its @c created_s set) loses its re-coupling: the new
 * decoupling supersedes it.
 *
 * @param edge   The station's record (written).
 * @param cause  Why the entry was removed (not @c FirstSighting, not @c Return).
 * @param now_s  The second of the removal.
 * @param sat    The satellite of a RAIM per-arc erase (empty otherwise).
 * @param via    The drop cause of a RAIM per-arc erase (empty otherwise).
 * @return The superseded record's cause when a re-created entry is removed before its first
 *         fold; empty otherwise.
 */
std::optional<ZwdEdgeCause> zwdEdgeDecouple(std::optional<ZwdCoupleEdge>& edge, ZwdEdgeCause cause,
                                            double now_s, std::string sat = {},
                                            std::string via = {});

/**
 * @brief The harvest creates the station's entry (there was none): stamps the creation second.
 *
 * A station with no open record has never carried a wet delay on this run: its record opens as
 * a first sighting.
 *
 * @param edge  The station's record (written).
 * @param now_s The harvest's second.
 */
void zwdEdgeCreate(std::optional<ZwdCoupleEdge>& edge, double now_s);

/**
 * @brief The harvest of an existing entry: closes the open edge at the entry's first fold, or
 *        recognises a fold after an absence within the horizon.
 *
 * - An open record whose entry was re-created closes when this epoch folded the entry: the
 *   re-coupling (or, for a first sighting, the first fold) is returned and the record cleared.
 *   When this epoch did not fold it, the record stays open for the next fold.
 * - With no open record, a fold whose entry was last stashed more than 1.5 epochs ago is a
 *   station returning within the horizon: a re-coupling of cause @c Return.
 *
 * @param edge   The station's record (written: cleared at the re-coupling).
 * @param folded PASS B folded the station's carried wet delay this epoch.
 * @param gap_s  Seconds since the entry was last stashed (this harvest's second minus its
 *               @c last_seen_s).
 * @param dt_s   Seconds since the previous processed epoch (0 on the first epoch).
 * @param now_s  The harvest's second.
 * @return The re-coupling this fold closes; empty when it closes none.
 */
std::optional<ZwdRecouple> zwdEdgeFold(std::optional<ZwdCoupleEdge>& edge, bool folded,
                                       double gap_s, double dt_s, double now_s);

/**
 * @brief The open records at the run end, by cause: decouplings whose entry never folded again
 *        (the station left, or the run ended first).
 * @tparam StationRange A range of (identity, row) pairs whose row has @c zwd_edge.
 * @param rows The station table.
 * @return The count per cause.
 */
template <class StationRange>
ZwdEdgeTally zwdEdgePendingByCause(const StationRange& rows) {
    ZwdEdgeTally n{};
    for (const auto& kv : rows)
        if (kv.second.zwd_edge)
            ++n[static_cast<std::size_t>(kv.second.zwd_edge->cause)];
    return n;
}

} // namespace GnssProcessor
