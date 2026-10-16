#pragma once
/**
 * @file ArcLife.hpp
 * @brief The life stamp of one band-tagged arc: the interim identity of an arc's integer
 *        ambiguity until a generation is put on @ref ArcId.
 *
 * ## What a life is
 *
 * An arc (station, satellite, signal) carries one integer ambiguity for as long as nothing
 * breaks its phase or resets its float.  Four owners record the events that end a life, and
 * the stamp reads each of them in place:
 *
 * | field            | owner                                  | written where                              | event |
 * |------------------|----------------------------------------|--------------------------------------------|-------|
 * | `adm_generation` | the arc ledger's admitted side         | the roster audit pass, after SB_NETAR      | a birth (first admission, or after a death) |
 * | `adm_run_start`  | the arc ledger's admitted side         | the same pass                              | a re-entry after a gap (a bridged gap is a new life) |
 * | `has_carry`      | @c LocalCarry (ctx.entities.arcs)  | the leveling harvest; the erases           | an erase (T4 / T3, gap prune) |
 * | `birth_ep`       | @c LocalCarry                          | the harvest's first stash                  | an erase and rebirth |
 * | `dropped_ep`     | @c LocalCarry                          | the float vehicle's drop (SB_PREASM; RAIM) | a drop |
 * | `relevel_s`      | @c LocalCarry                          | the first stash, a drop, the harvest re-stash of a latched arc | a re-level |
 * | `flag_ep`        | @c NetworkCarryState::amb_flag_ep      | the free set, T3 / T4, gross-LOS           | a screen flag (written only under @c network.phase_datum_held_basis) |
 *
 * Equal stamps mean the integer is continuous as far as these owners know.  The stamp is
 * read, never stored here: a caller that compares lives keeps the earlier stamp (the
 * cycle-basis search keeps one per arc in its family memory, network/ar/ArCycleBasis.hpp).
 * It is live at the defaults: the ledger is built on every network run, and the carry fields
 * exist whenever the leveling carry does.
 *
 * ## One reader of the carry stamps
 *
 * The arc-graph design binds the life stamp and the search's suspect-corner mask
 * (network/ar/ArSuspect.hpp) to one reader of the @c LocalCarry life fields, so that
 * neither keeps a second record of an arc's life.  That reader is @ref carryLifeOf: the
 * stamp reads it here, and the mask's per-epoch fold reads it for its drop and re-level
 * causes.
 *
 * ## The search's track of a life, and the echo of an event
 *
 * The search reads the stamp at SB_NETAR.  Two of the owners act later in the same epoch:
 * the ledger's audit pass admits the epoch's rows after the search, and the leveling
 * harvest stashes the carry after it.  So the life a search begins (an arc it meets for the
 * first time, an arc freed this epoch, an arc whose stamp changed) shows up once more at the
 * next search, as the ledger catching up with the run and the harvest stashing the new
 * carry.  @ref arcLifeStep reads that one change as the event's echo and re-baselines the
 * stamp: at the search right after the one that began a life, a change confined to the
 * ledger fields, @c birth_ep, @c relevel_s and a carry that appears is not a second life.
 * An erase (a carry that disappears), a drop or a flag is never an echo.  So each ledger
 * birth or re-entry and each carry erase, drop, re-level or flag gives exactly one life
 * edge.  The blind spot left: a carry re-level that lands, for an unrelated reason, in the
 * one epoch after a life began is absorbed into that life.
 */

#include "common/GnssIdentity.hpp"   // ArcId

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace GnssProcessor {

struct ArcRecord;           // network/state/ArcLedger.hpp
struct LocalCarry;          // network/driver/RunTypes.hpp
struct NetworkRunContext;   // network/state/RunContext.hpp

/// The life fields of one @c LocalCarry entry (the one reader of them; file banner).
struct CarryLife {
    long long birth_ep{0};    ///< @c LocalCarry::birth_ep: loop epoch of the arc's first stash (restarted by an erase and rebirth).
    double    birth_s{0.0};   ///< @c LocalCarry::birth_s: its continuous-GPS second.
    long long dropped_ep{0};  ///< @c LocalCarry::dropped_ep: loop epoch of the last drop (0 = never).
    double    relevel_s{0.0}; ///< @c LocalCarry::relevel_s: continuous-GPS second of the last re-level (creation, drop, latched re-stash).
};

/**
 * @brief The life fields of a carry entry.
 * @param lc The entry.
 * @return Its four life fields.
 */
CarryLife carryLifeOf(const LocalCarry& lc);

/// An arc's life stamp (file banner).  Equal stamps mean the integer is continuous.
struct ArcLifeStamp {
    int       adm_generation{0};  ///< Arc ledger admitted generation (births and rebirths); 0 = never admitted.
    long long adm_run_start{-1};  ///< Arc ledger admitted run start: the birth epoch, or the epoch after the last re-entry; -1 while the ledger does not hold the arc live (never admitted, in a gap, dead).
    bool      has_carry{false};   ///< A @c LocalCarry entry exists (an erase reads as a new life).
    long long birth_ep{0};        ///< @c LocalCarry::birth_ep (erase and rebirth).
    long long dropped_ep{0};      ///< @c LocalCarry::dropped_ep (the float vehicle's drop).
    double    relevel_s{0.0};     ///< @c LocalCarry::relevel_s (creation, drop, the free-epoch latch's re-stash).
    long long flag_ep{-1};        ///< @c NetworkCarryState::amb_flag_ep (free set, T3 / T4, gross-LOS); -1 = never flagged.
};

/**
 * @brief Member-wise equality of two stamps.
 * @param a First stamp.
 * @param b Second stamp.
 * @return True when every field is equal.
 */
bool operator==(const ArcLifeStamp& a, const ArcLifeStamp& b);

/**
 * @brief Member-wise inequality of two stamps.
 * @param a First stamp.
 * @param b Second stamp.
 * @return @c !(a == b).
 */
bool operator!=(const ArcLifeStamp& a, const ArcLifeStamp& b);

/// One bit per @ref ArcLifeStamp field, in member order (the release attribution).
enum ArcLifeField : unsigned {
    kArcLifeAdmGeneration = 1u << 0,  ///< @ref ArcLifeStamp::adm_generation.
    kArcLifeAdmRunStart   = 1u << 1,  ///< @ref ArcLifeStamp::adm_run_start.
    kArcLifeHasCarry      = 1u << 2,  ///< @ref ArcLifeStamp::has_carry.
    kArcLifeBirthEp       = 1u << 3,  ///< @ref ArcLifeStamp::birth_ep.
    kArcLifeDroppedEp     = 1u << 4,  ///< @ref ArcLifeStamp::dropped_ep.
    kArcLifeRelevelS      = 1u << 5,  ///< @ref ArcLifeStamp::relevel_s.
    kArcLifeFlagEp        = 1u << 6,  ///< @ref ArcLifeStamp::flag_ep.
};

/// The number of @ref ArcLifeField bits.
inline constexpr int kArcLifeFieldCount = 7;

/**
 * @brief Short name of field bit @p k (bit position 0 to 6): adm_generation, adm_run_start,
 *        has_carry, birth_ep, dropped_ep, relevel_s, flag_ep.
 * @param k Bit position.
 * @return The name; "?" out of range.
 */
const char* arcLifeFieldName(int k);

/**
 * @brief The stamp from its three sources, each of which may be absent.
 * @param ledger  The arc's ledger record (null: the ledger does not know the arc).
 * @param carry   Its @c LocalCarry entry (null: none).
 * @param flag_ep Its @c amb_flag_ep entry (null: never flagged).
 * @return The stamp.
 */
ArcLifeStamp arcLifeStampOf(const ArcRecord* ledger, const LocalCarry* carry, const long long* flag_ep);

/// The keys of one arc in the stamp's sources: the arc ledger and the leveling carry are keyed
/// by (station index, satellite, phase observation code), the flag map (and the free-epoch
/// latch) by (station index, ambiguity key "G05_L1C").
struct ArcLifeKeys {
    std::tuple<int, std::string, std::string> arc;  ///< Ledger and carry key.
    std::pair<int, std::string>               amb;  ///< Flag-map and free-latch key.
};

/**
 * @brief The source keys of a band-tagged arc.
 * @param arc The arc.
 * @return Its keys (station index, satellite string, phase observation code; ambiguity key).
 */
ArcLifeKeys arcLifeKeys(const ArcId& arc);

/**
 * @brief The current stamp of one arc, read from the run context by its source keys (a caller
 *        that reads many arcs builds each arc's keys once, @ref arcLifeKeys).
 * @param ctx  The run context.
 * @param keys The arc's keys.
 * @return The stamp.
 */
ArcLifeStamp arcLifeStamp(const NetworkRunContext& ctx, const ArcLifeKeys& keys);

/**
 * @brief The current stamp of one band-tagged arc, read from the run context
 *        (@ref arcLifeStamp of @ref arcLifeKeys).
 * @param ctx The run context.
 * @param arc The arc.
 * @return The stamp.
 */
ArcLifeStamp arcLifeStamp(const NetworkRunContext& ctx, const ArcId& arc);

/**
 * @brief Bit mask of the stamp fields that differ (@ref ArcLifeField bits).
 * @param before The earlier stamp.
 * @param now    The later stamp.
 * @return The bits of the fields that changed; 0 when the stamps are equal.
 */
unsigned arcLifeChange(const ArcLifeStamp& before, const ArcLifeStamp& now);

/// One arc's life as a search tracks it from one search to the next.
struct ArcLifeTrack {
    ArcLifeStamp stamp{};        ///< The stamp the last search read (re-baselined at an echo).
    long long    life_start{0};  ///< Loop epoch of the search that began the current life.
};

/// Why a search began a new life for an arc, or why it did not.
enum class ArcLifeEdge : std::uint8_t {
    None,         ///< The life continues.
    FirstSeen,    ///< The last search did not meet the arc: a new arc, or one back after a gap.
    Freed,        ///< This epoch's free-epoch latch holds the arc.
    StampChange,  ///< The stamp changed, and not as an echo.
    Echo          ///< The stamp changed as the echo of the life the last search began (re-baselined; not a new life).
};

/// One search's reading of one arc (@ref arcLifeStep).
struct ArcLifeStep {
    ArcLifeTrack track{};               ///< The arc's track after this search.
    ArcLifeEdge  edge{ArcLifeEdge::None};  ///< What this search read.
    unsigned     change{0};             ///< @ref arcLifeChange of the two stamps (0 at FirstSeen).
    /// A new life (FirstSeen, Freed, StampChange): its durability restarts.
    bool newLife() const {
        return edge == ArcLifeEdge::FirstSeen || edge == ArcLifeEdge::Freed
            || edge == ArcLifeEdge::StampChange;
    }
    /// The integer may differ from the one the last search met (Freed, StampChange): the
    /// arc is refused as a new tree edge or link reference in the first pass, and every
    /// stored cycle through it is invalidated.
    bool slipped() const { return edge == ArcLifeEdge::Freed || edge == ArcLifeEdge::StampChange; }
};

/**
 * @brief One search's reading of one arc against the track the last search left.
 *
 * In order: an arc the last search did not meet begins a life (FirstSeen); an arc in this
 * epoch's free-epoch latch begins one (Freed); a stamp change at the search right after the
 * one that began the life, confined to the echo fields (the two ledger fields,
 * @c birth_ep, @c relevel_s, and @c has_carry turning true), is that event's echo and
 * re-baselines the stamp (Echo); any other change begins a life (StampChange).
 *
 * @param last            The arc's track at the last search, or null when it did not meet the arc.
 * @param last_search_ep  Loop epoch of the last search (0 = none).
 * @param now             The arc's stamp now.
 * @param freed           The arc is in this epoch's free-epoch latch (@c relevel.free_ambs).
 * @param now_ep          This search's loop epoch.
 * @return The reading and the arc's new track.
 */
ArcLifeStep arcLifeStep(const ArcLifeTrack* last, long long last_search_ep, const ArcLifeStamp& now,
                        bool freed, long long now_ep);

}  // namespace GnssProcessor
