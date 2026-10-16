#pragma once
/**
 * @file ArSuspect.hpp
 * @brief The producer AR search's suspect-corner mask (slip-ladder defect 3):
 *        a run-scope ledger of the (station, satellite) corners whose ambiguity a slip
 *        response or a slip witness touched in the last few epochs, the rule that keeps
 *        every double difference touching such a corner out of the integer search, and
 *        the accounting of both edges of that exclusion.
 *
 * ## The defect it closes
 *
 * The producer AR search (network/ar/ProducerAr.hpp) reads the post-fit floats and
 * their covariance and nothing else.  A slip that no pre-solve detector caught (ladder
 * rung S8, a (4,5) slip: one wide-lane cycle, an IF step of 5 cm, a geometry-free step
 * the free slant ionosphere absorbs) leaves the float formal sigma, the bootstrap rate
 * and the ratio exactly where the clean control has them, so every gate of the search
 * passes and it fixes the wrong integer (docs/refound/ARL_integer_ambiguity_ladder_
 * 2026-10-09.md, section 12).  The slip layer does hold evidence about such arcs, from
 * the slip epoch or a few epochs later, but the search never reads it.
 *
 * ## The ledger
 *
 * One owner, @ref ArSuspectLedger in @c NetworkCarryState (@c ctx.carry.ar_suspect),
 * keyed by @ref LosId.  Per corner it keeps, for each cause, the stamp of its last event:
 * the loop epoch (@c n_loop_epochs) of the FIRST SEARCH THAT CAN ACT ON IT.  A writer that
 * runs before the epoch's search (SB_NETAR) stamps this epoch; a writer that runs after it
 * stamps the next one (@ref arSuspectPostSearchEpoch).  Every cause therefore masks the
 * same number of searches.  The writers, in epoch order:
 *
 * | cause                    | written where                     | stamp        | source |
 * |--------------------------|-----------------------------------|--------------|--------|
 * | @ref kArSuspectMwUnknown | PASS A, the MW admission reduce   | this epoch   | an MW verdict dropped as an unknown integer |
 * | @ref kArSuspectGfJump    | PASS A, after the GF jump drain   | this epoch   | an arc GF jump flag, whatever its action |
 * | @ref kArSuspectDrop      | SB_NETAR, @ref arSuspectFoldEpoch | this epoch   | a @c LocalCarry::dropped_ep no earlier fold has seen: the float vehicle's pre-search drops (free-epoch latch, stale gap; SB_PREASM) |
 * | @ref kArSuspectRelevel   | SB_NETAR, @ref arSuspectFoldEpoch | its epoch    | a @c LocalCarry::relevel_s no other cause owns (below) |
 * | @ref kArSuspectFree      | SB_NETAR, @ref arSuspectFoldEpoch | this epoch   | this epoch's free-epoch latch (@c RelevelEpochState::free_ambs), any detector cause |
 * | @ref kArSuspectRaim      | SB_RAIM, the T4 and T3 sites      | next epoch   | a RAIM attribution or consensus drop / erase |
 *
 * The RAIM drop is the one post-search writer of @c dropped_ep; the fold leaves it to the
 * RAIM row, whose stamp is already the next epoch's.  @c relevel_s has three writers, and
 * each belongs to another row: a first stash (the arc is born with a code-quality level;
 * a newborn arc has no stale value for a slip to hide behind, and @c relevel_s equals
 * @c birth_s), the float vehicle's drop (which writes @c dropped_ep in the same epoch: the
 * drop or the RAIM row), and the leveling harvest's re-stash of an arc in that epoch's
 * latch (the free row, same epoch).  The fold still reads it, as a safety net: a re-level
 * none of these explains is stamped as @ref kArSuspectRelevel at its own epoch, so a
 * non-zero relevel tally names a writer the ledger does not know.  An erase-and-rebirth
 * is a birth to the carry; the RAIM erase is caught at its own site instead.
 *
 * One event often carries several causes (a latch free is also a drop on the float
 * vehicle; a GF flag also frees).  The counters keep the stamps by cause and, apart, the
 * distinct (corner, stamp) events, and of those the events whose corner met a search
 * while suspect: a stale-gap drop of an arc that has set is stamped but never meets one.
 *
 * ## The rule
 *
 * A stamp e masks the searches e, e + 1, ..., e + W - 1, where W is
 * @c network.ambiguity_resolution.suspect_epochs: a corner is suspect at loop epoch E when
 * some cause holds a stamp e with 0 <= E - e < W.  A DD is masked when any of its four
 * corners is suspect: it stays in the basis and in the trace (in_subset 0, its float and
 * sigma written, @c suspect_bits the OR of its corners' cause bits), and the integer
 * search does not see it.  A suspect reference corner therefore masks every DD of its
 * baseline; that is the price of a per-corner rule on a basis whose DDs share a reference
 * satellite.
 *
 * ## The release edge (standing invariant 7)
 *
 * The exclusion is an event at both edges.  The decoupling edge is the stamp, counted by
 * cause and per search.  The release is TIME-ONLY: a corner whose newest stamp is W
 * searches old is searchable again, and nothing re-validates its arc first.  It is counted,
 * not judged: a corner met suspect in a search pool and later met clean in one is a
 * release (@ref ArSuspectLedger::masking holds the corners still awaiting theirs), the DDs
 * it brings back to the search are the re-entries, and their first search is tallied
 * (integers kept, L1/L2 pairs whose wide lane is determined while the singles are not) on
 * the epoch's log line, in the trace's @c reentry column and in the run-end report.
 *
 * A slip no cause names outlives the window when its arc has not re-converged by then:
 * on rung S8 with the GF trigger off, the one MW stamp masks ten searches and the stale
 * wide lane is determined again at the first search after the release.  The mask bounds
 * the exposure to the window; it does not end an undetected slip.  It is therefore
 * FIELD-BLOCKING as the guard of a solve-affecting search: today's search is log-only,
 * and a successor that fixes into the solve needs a release keyed on the arc's own
 * re-convergence (or a float-against-integer check at re-entry) first.
 *
 * ## Off
 *
 * The search is log-only, so the mask changes no product.  With the window at 0 nothing
 * is written or read, the trace carries no mask columns and the search is byte-identical.
 */

#include "common/GnssIdentity.hpp"      // LosId, StationId, SatId
#include "network/state/Entities.hpp"      // net::ArcTable
#include "network/driver/RunTypes.hpp"   // LocalCarry, RelevelEpochState

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRunEndView;

/// Why a corner is suspect: one bit per source (the ledger table in the file banner).
/// The trace's @c suspect_bits column is the OR of these over a DD's four corners.
enum ArSuspectCause : std::uint8_t {
    kArSuspectFree      = 1,   ///< In the free-epoch latch (relevel.free_ambs), any detector cause.
    kArSuspectRelevel   = 2,   ///< A @c LocalCarry::relevel_s re-level no other cause owns.
    kArSuspectDrop      = 4,   ///< A pre-search @c LocalCarry::dropped_ep: the float vehicle dropped the arc's prior.
    kArSuspectRaim      = 8,   ///< RAIM T4 attribution or T3 consensus dropped or erased the arc.
    kArSuspectMwUnknown = 16,  ///< An MW verdict on the arc was dropped as an unknown integer.
    kArSuspectGfJump    = 32,  ///< The arc-scoped GF jump trigger flagged the arc.
};

/// The number of @ref ArSuspectCause bits.
inline constexpr int kArSuspectCauseCount = 6;

/**
 * @brief Short name of cause bit @p k (bit position 0 to 5): free, relevel, drop, raim,
 *        mw_unknown, gf_jump.
 * @param k Bit position.
 * @return The name; "?" out of range.
 */
const char* arSuspectCauseName(int k);

/// One corner's entry in the ledger.
struct ArSuspectCorner {
    /// Per cause (bit position), the stamp of its last event: the loop epoch of the first
    /// search that can act on it.  0 = never.
    std::array<long long, kArSuspectCauseCount> last_ep{};
    /// The newest stamp whose event @ref ArSuspectCounters::events_met already counted.
    long long met_ep{0};
};

/// The run-scope ledger (file banner).  Not snapshotted: a resumed leg starts with every
/// corner clean, which is the direction the warm-resume contract already takes for the
/// slip layer (its detectors restart cold).
struct ArSuspectLedger {
    /// The recently stamped corners.  A corner whose newest stamp has left the window is
    /// erased at the next fold.
    std::map<LosId, ArSuspectCorner> corners;
    /// (loop epoch, continuous-GPS second) of the most recent folds, oldest first, at most
    /// the window long.  It maps a @c LocalCarry::relevel_s second onto its loop epoch.
    std::deque<std::pair<long long, double>> recent;
    /// The release edge's population: corners met suspect in a search pool and not met
    /// clean in one since.  A corner leaves it only at its release, so a corner whose
    /// window expires while its arc is out of the pool is released when it is next met.
    std::set<LosId> masking;
};

/// Run-end counters of the mask, both edges (standing invariants 4 and 7: an armed mask
/// that stamped nothing, never met a search, or released without a trace, says so).
struct ArSuspectCounters {
    // --- the decoupling edge
    std::array<long long, kArSuspectCauseCount> stamps{};   ///< Stamps written, by cause (one event can carry several causes).
    long long events{0};          ///< Distinct (corner, stamp) events, whatever their causes.
    long long events_met{0};      ///< ...of those, events whose corner met a search pool while suspect.
    long long epochs{0};          ///< Search epochs with the mask armed.
    long long epochs_masked{0};   ///< ...of those, epochs that masked at least one DD.
    long long dd_total{0};        ///< DDs in the basis over those epochs.
    long long dd_masked{0};       ///< DDs kept out of the search.
    std::array<long long, kArSuspectCauseCount> dd_masked_by{};  ///< ...by cause (a DD counts once per bit its corners carry).
    long long corners{0};         ///< Suspect corners present in the search pool, summed over epochs.
    std::array<long long, kArSuspectCauseCount> corners_by{};    ///< ...by cause.
    long long max_dd_masked{0};   ///< Most DDs masked in one epoch.
    double    max_dd_masked_tow{0.0};  ///< Its time of week (s).
    // --- the release edge (time-only)
    long long corners_released{0};    ///< Corners met clean in a search pool after masking in an earlier one.
    long long epochs_released{0};     ///< Search epochs with at least one release.
    long long dd_reentered{0};        ///< DDs back in the search through a corner released at that search.
    long long dd_reentered_fixed{0};  ///< ...of those, given an integer (kept) at that first search.
    long long pairs_reentered{0};     ///< L1/L2 pairs among the re-entered DDs (both bands searched).
    long long pairs_reentered_singles{0};  ///< ...with both single-signal integers kept.
    long long pairs_reentered_wl_only{0};  ///< ...with the wide lane determined and the singles not both kept.
};

/**
 * @brief The stamp of an event written after the epoch's search (SB_RAIM): the next
 *        epoch's, the first search that can act on it.
 * @param now_ep This epoch's @c n_loop_epochs.
 * @return @p now_ep + 1.
 */
inline long long arSuspectPostSearchEpoch(long long now_ep) { return now_ep + 1; }

/**
 * @brief Stamp a corner with a cause.
 *
 * Keeps the later stamp when the corner already holds one of that cause; a stamp that
 * does not advance it is not counted, so re-reading a carry stamp every epoch counts it
 * once.  A counted stamp is a new event unless another cause of the same corner holds
 * the same stamp.
 *
 * @param L      The ledger.
 * @param los    The corner.
 * @param ep     The stamp: the loop epoch of the first search that can act on the event
 *               (> 0; @ref arSuspectPostSearchEpoch for a post-search writer).
 * @param cause  One @ref ArSuspectCause bit.
 * @param c      Counters (@ref ArSuspectCounters::stamps, @ref ArSuspectCounters::events);
 *               may be null.
 */
void arSuspectNote(ArSuspectLedger& L, const LosId& los, long long ep, std::uint8_t cause,
                   ArSuspectCounters* c);

/**
 * @brief @ref arSuspectNote for a (roster index, RINEX satellite id) pair.
 * @param L      The ledger.
 * @param sta    Station roster index.
 * @param sat    Satellite id ("G05"); a string @ref SatId::parse refuses is skipped.
 * @param ep     The stamp (as for the @ref LosId overload).
 * @param cause  One @ref ArSuspectCause bit.
 * @param c      Counters; may be null.
 * @return False when @p sat did not parse (nothing written).
 */
bool arSuspectNote(ArSuspectLedger& L, int sta, std::string_view sat, long long ep,
                   std::uint8_t cause, ArSuspectCounters* c);

/**
 * @brief Cause bits of a corner at loop epoch @p now_ep: the causes holding a stamp e
 *        with 0 <= now_ep - e < @p window_ep.
 * @param L          The ledger.
 * @param los        The corner.
 * @param now_ep     The current loop epoch.
 * @param window_ep  The window (network.ambiguity_resolution.suspect_epochs); <= 0 gives 0.
 * @return The OR of the live @ref ArSuspectCause bits; 0 = clean.
 */
std::uint8_t arSuspectBits(const ArSuspectLedger& L, const LosId& los, long long now_ep,
                           int window_ep);

/**
 * @brief The loop epoch of a continuous-GPS second, from the ledger's fold clock.
 *
 * Returns the epoch of the latest fold at or before @p t_s (1 ms tolerance), or 0 when
 * @p t_s precedes the oldest fold the clock still holds (outside the window).
 *
 * @param recent  @ref ArSuspectLedger::recent.
 * @param t_s     The second (a @c LocalCarry::relevel_s).
 * @return The loop epoch, or 0.
 */
long long arSuspectEpochOfSecond(const std::deque<std::pair<long long, double>>& recent,
                                 double t_s);

/**
 * @brief The search seam's per-epoch fold (SB_NETAR, before the search).
 *
 * Advances the fold clock to (@p now_ep, @p now_s), erases the corners whose newest
 * stamp has left the window, and stamps from the sources the seam reads in place, in
 * this order: the carry stamps of every @c LocalCarry entry (@ref kArSuspectDrop for a
 * @c dropped_ep newer than the previous fold, stamped @p now_ep; @ref kArSuspectRelevel
 * for a @c relevel_s that is not a first stash, not the same epoch's drop and not the
 * corner's free stamp of that epoch, stamped at its own epoch), then this epoch's
 * free-epoch latch (@ref kArSuspectFree, @p now_ep).  The carry goes first so that a
 * harvest re-level of an arc latched in the previous epoch still finds that epoch's free
 * stamp.
 *
 * @param L            The ledger.
 * @param now_ep       This epoch's @c n_loop_epochs.
 * @param now_s        This epoch's continuous-GPS second.
 * @param window_ep    The window; <= 0 makes the call a no-op.
 * @param relevel      This epoch's slip response state (free_ambs read).
 * @param arcs         The arc table (the leveling carry's stamps read).
 * @param c            Counters; may be null.
 */
void arSuspectFoldEpoch(ArSuspectLedger& L, long long now_ep, double now_s, int window_ep,
                        const RelevelEpochState& relevel,
                        const net::ArcTable& arcs,
                        ArSuspectCounters* c);

/// A corner's (or, OR-ed, a DD's) state at one search.
struct ArSuspectCornerState {
    std::uint8_t bits{0};     ///< The live @ref ArSuspectCause bits; 0 = clean.
    bool         released{false};  ///< Clean now, and masking at an earlier search: this search releases it.
};

/**
 * @brief A DD's state from its four corners: the OR of their bits and of their release
 *        flags.  The DD is masked when the bits are non-zero, and re-enters the search
 *        when they are zero and a corner is released.
 * @return The combined state.
 */
ArSuspectCornerState arSuspectCombine(ArSuspectCornerState a, ArSuspectCornerState b,
                                      ArSuspectCornerState c, ArSuspectCornerState d);

/// One search epoch's mask: opened by the search seam, filled while it builds its pool and
/// basis, closed once the search has run (@ref arSuspectCloseSearch).  With the window at 0
/// every call is a no-op and every vector stays empty.
struct ArSuspectSearch {
    long long now_ep{0};   ///< This epoch's @c n_loop_epochs.
    int       window{0};   ///< The window; 0 = off.
    std::map<LosId, std::uint8_t> pool_suspect;   ///< Suspect corners met in the pool, with their bits.
    std::set<LosId>               pool_released;  ///< Corners this search releases.
    std::vector<char>         masked;    ///< Per basis DD: kept out of the search.
    std::vector<char>         reentry;   ///< Per basis DD: searched, and a corner released at this search.
    std::vector<std::uint8_t> dd_bits;   ///< Per basis DD: the OR of its corners' bits (the trace's suspect_bits).
    int n_masked{0};       ///< DDs masked.
    int n_reentry{0};      ///< DDs re-entering the search.
    std::array<long long, kArSuspectCauseCount> masked_by{};  ///< Masked DDs by cause.
};

/// The re-entered DDs' first search, as the search seam counted it
/// (@ref netar::reentryOutcome).
struct ArSuspectReentry {
    long long dd{0};             ///< DDs re-entering the search.
    long long dd_fixed{0};       ///< ...given an integer (kept).
    long long pairs{0};          ///< L1/L2 pairs among them with both bands searched.
    long long pairs_singles{0};  ///< ...with both single-signal integers kept.
    long long pairs_wl_only{0};  ///< ...with the wide lane determined and the singles not both kept.
};

/**
 * @brief Open a search epoch's mask.
 * @param now_ep  This epoch's @c n_loop_epochs.
 * @param window  The window (@c knobs.ar_suspect_epochs); <= 0 opens an inert mask.
 * @return The empty search state.
 */
ArSuspectSearch arSuspectOpenSearch(long long now_ep, int window);

/**
 * @brief A pool corner's state at this search; records it as suspect or released.
 * @param S    This search's mask.
 * @param L    The ledger (@ref ArSuspectLedger::masking read for the release).
 * @param sta  Station roster index.
 * @param sat  Satellite id; a string @ref SatId::parse refuses is clean.
 * @return Its bits and release flag; clean while the mask is off.
 */
ArSuspectCornerState arSuspectPoolCorner(ArSuspectSearch& S, const ArSuspectLedger& L,
                                         int sta, std::string_view sat);

/**
 * @brief Split the basis: fill @ref ArSuspectSearch::masked, @c reentry, @c dd_bits and
 *        their counts from one state per basis DD.
 * @param S         This search's mask; a no-op while off.
 * @param dd_state  One combined state per basis DD (@ref arSuspectCombine).
 */
void arSuspectMaskBasis(ArSuspectSearch& S, const std::vector<ArSuspectCornerState>& dd_state);

/**
 * @brief Close a search epoch: tally both edges, move the released corners out of
 *        @ref ArSuspectLedger::masking and the masking ones in, and return the clause the
 *        epoch's log line carries.
 *
 * The clause reads ", suspect mask N DD over K corner(s) [causes], released R corner(s)",
 * followed by "(D DD back: F fixed, W WL-only pair(s))" when R > 0.
 *
 * @param S    This search's mask.
 * @param L    The ledger.
 * @param C    The run's counters.
 * @param re   The re-entered DDs' first search (@ref netar::reentryOutcome).
 * @param tow  This epoch's time of week (s), for the worst-epoch record.
 * @return The clause; empty while the mask is off.
 */
std::string arSuspectCloseSearch(ArSuspectSearch& S, ArSuspectLedger& L,
                                 ArSuspectCounters& C, const ArSuspectReentry& re,
                                 double tow);

/**
 * @brief "free 2, drop 2, mw_unknown 1": the non-zero entries of a per-cause tally.
 * @param by Per-cause counts, indexed by bit position.
 * @return The text; "none" when every entry is 0.
 */
std::string arSuspectCauseSummary(const std::array<long long, kArSuspectCauseCount>& by);

/**
 * @brief Run-end report: the producer AR search's suspect-corner mask, both edges.
 *
 * The decoupling edge: the events and the stamps by cause, the search epochs that masked
 * something, the DDs masked (in total, by cause and at the worst epoch) against the basis
 * size, and the suspect corners met in the search pool.  The release edge: the corners
 * released, the DDs they brought back to the search and what those got at their first
 * search, and the corners still awaiting a release.
 *
 * Gate: @c network.ambiguity_resolution.enable with @c suspect_epochs > 0 (returns 0 at
 * once otherwise).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkArSuspectRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of ArSuspect, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct ArSuspectRunCounters {
    /// The search's suspect-corner mask (network/ar/ArSuspect.hpp), both
    /// edges: events and stamps by cause, DDs masked, corners released and what their
    /// DDs got at the first search after the release.  All zero unless
    /// knobs.ar_suspect_epochs > 0.
    ArSuspectCounters   netar_suspect;
};

} // namespace GnssProcessor
