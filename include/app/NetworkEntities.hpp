#pragma once
/**
 * @file NetworkEntities.hpp
 * @brief The network producer's cross-epoch run state as entity tables (card NOM step N4).
 *
 * Read this header first.  An **entity** is one thing the network carries state about from
 * one epoch to the next: a constellation, a satellite, a satellite signal, a station, a line
 * of sight (station, satellite) or an arc (station, satellite, signal).  Each entity has one
 * **table**, an ordered map from the entity's typed identity (common/GnssIdentity.hpp) to
 * a **row**, a plain struct whose members are the entity's carried fields.  The six tables
 * are the members of @ref net::NetworkEntities, held by the run context as
 * @c NetworkRunContext::entities beside the flat @c NetworkCarryState they replace one
 * entity per sub-step (scripts/nom3_designs/N4_design.md section 6).
 *
 * **Presence.**  A field that was a map entry is a @c std::optional in the row, present
 * exactly when the old map held the key; a set membership is a @c bool.  A row is created
 * by the first write of any of its fields and is never erased (decision D8): erasing an
 * entry resets the field, and the row stays.  The free functions below are the only access
 * (no member behaviour): @ref get and @ref put replace @c find / @c operator[], @ref drop
 * and @ref dropAll replace @c erase / @c clear, @ref present replaces @c size, and
 * @ref forEach replaces a range loop.  A field keyed below the entity (a station's signals)
 * lives in a nested map of the row, with the same functions taking the sub-key.
 *
 * **Iteration order** (design section 4).  A table iterates in the order of its identity,
 * and every identity's comparator reproduces the string order of the key it replaced
 * (tests/test_gnss_identity.cpp), so @ref forEach visits the present rows of a field in the
 * order the old map did.  Constellations order by RINEX letter (@ref ByLetter).  A member
 * that was keyed by station NAME is iterated through @ref byStationName, which visits rows
 * in name order; a member whose order no typed key reproduces keeps its old key inside the
 * row that owns it (O4).  No loop is fused across members (O7).
 *
 * **Persistence.**  Every moved field has one @ref FieldDecl row in its entity's field table
 * (@ref kStationFields and its siblings, src/core/app/NetworkEntities.cpp): its name, the
 * @c NetworkCarryState member it was, how it survives a restart (@ref Persist) and, for a
 * snapshot record, the two functions that fill the record from the table and the table
 * from the record.  The snapshot writer and reader call those functions where the old copy
 * blocks stood; the reader keeps today's restore order (@ref kRestoreOrder).
 *
 * **Lifecycle.**  The satellite, station and arc rows declare a @ref Lifecycle (decision
 * D4): a return is never read as a first sighting (standing invariant 7).  It is filled
 * where an owner computes it; a row whose owner does not yet compute it stays
 * @ref Life::Absent.
 *
 * **Parallel access.**  The station produce runs in parallel over stations and only reads
 * the tables (@ref get on a const table is a @c std::map::find, safe against concurrent
 * readers).  Rows are inserted only by serial code (the harvests and the serial seams), so
 * no insert can race a read (verifier check V5, scripts/nom_n4_verify.py).
 *
 * **The reach table** ("entities as carried", run end): per declared field its maximum and
 * final present rows, with its persistence and record, and per identity converter its
 * round-trip failures.  It is the reader's view of the run state and the evidence that an
 * arm reached a field.  Its log lines are trace class.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "common/GnssIdentity.hpp"

namespace GnssProcessor {
    struct NetworkRunContext;
    struct NetworkStateSnapshot;
    struct NetworkRun;
    struct NetworkEpoch;
    struct NetworkRunEndView;
}

namespace GnssProcessor::net {

// ===========================================================================
// Declarations: persistence, lifecycle, field rows
// ===========================================================================

/**
 * @brief How a field survives a restart (card NOM section 3.5): the persistence column of
 *        scripts/nom3_designs/N4_fields.tsv.
 */
enum class Persist : std::uint8_t {
    Record,           ///< Written to and restored from a snapshot record.
    ResumeSeeded,     ///< Not a record; seeded by the warm resume from other records.
    ColdStart,        ///< Starts empty on every run, warm or cold.
    Pending,          ///< A hand-off consumed within a few epochs of being set.
    RunConst,         ///< Set once at setup and read for the run.
    WriterTelemetry,  ///< The snapshot writer's own counters.
};

/// The name of a persistence class, as the reach table prints it.
const char* persistName(Persist p);

/**
 * @brief An entity's lifecycle state.  A return is never read as a first sighting
 *        (standing invariant 7).
 */
enum class Life : std::uint8_t {
    Absent,    ///< Never seen (or not computed by any owner yet).
    Present,   ///< Seen at the latest epoch, continuously since its first sighting.
    Departed,  ///< Seen before, not at the latest epoch.
    Returned,  ///< Seen again after a departure.
};

/// The lifecycle of one row: its state and the loop epochs of its first and latest sighting.
struct Lifecycle {
    Life      life{Life::Absent};
    long long ep_first{-1};   ///< Loop epoch of the first sighting.
    long long ep_last{-1};    ///< Loop epoch of the latest sighting.
};

struct NetworkEntities;

/**
 * @brief One declared field of an entity: one row of the entity's field table.
 *
 * A Record field names its snapshot tag and has both fill functions; every other field
 * has them null.  The fill functions take the run context because a record's copy reads
 * the knobs the old copy block read (the shared-history marginal variance of the Z rows,
 * the restored-entry convention of @c makeRestoredLocalCarry).
 */
struct FieldDecl {
    const char* name;          ///< The member, as spelled in the row struct.
    const char* was;           ///< Its former @c NetworkCarryState member (the G1 trail).
    Persist     persist;       ///< How it survives a restart.
    const char* record;        ///< Snapshot tag ("Z"); "" unless Persist::Record.
    const char* fingerprint;   ///< The knob whose fingerprint makes a toggle cold-start, or "".
    /// Fills the record from the table (Record only; the writer gates the call).
    void (*to_snapshot)(const NetworkRunContext& ctx, NetworkStateSnapshot& snap);
    /// Fills the table from the record (Record only; the reader gates the call).
    void (*from_snapshot)(const NetworkStateSnapshot& snap, NetworkRunContext& ctx);
    /// The number of rows where the field is present (the reach table).
    std::size_t (*rows)(const NetworkEntities& e);
};

/// One entity's field table, named.
struct EntityFields {
    const char*                   entity;  ///< "station", "satellite", ...
    const std::vector<FieldDecl>* fields;  ///< Its declared fields.
};

// ===========================================================================
// The rows
// ===========================================================================

/// Constellations order by RINEX letter, the order of today's char and one-letter keys (O5).
struct ByLetter {
    bool operator()(System a, System b) const { return traitsOf(a).letter < traitsOf(b).letter; }
};

/// One constellation's carried state (step N4b fills it).
struct ConstellationState {};

/// One satellite's carried state (steps N4e1 and N4e2 fill it).
struct SatelliteState {
    Lifecycle seen;  ///< Decision D4; stays Absent until a card needs it.
};

/// One satellite signal's carried state (step N4d fills it).
struct SatSignalState {};

/// One station's carried state (step N4a fills it).
struct StationState {
    Lifecycle seen;  ///< Decision D4.
};

/// One line of sight's carried state (step N4c fills it).
struct LosState {};

/// One arc's carried state (steps N4f1 and N4f2 fill it).
struct ArcState {
    Lifecycle seen;  ///< Decision D4; filled from the arc ledger in step N4f2.
};

/// An entity table: an ordered map from the identity to the row.
template <class Id, class Row, class Cmp = std::less<Id>>
using Table = std::map<Id, Row, Cmp>;

/// The station table.
using StationTable = Table<StationId, StationState>;

// ===========================================================================
// Station names and the identity converters
// ===========================================================================

/**
 * @brief The run's station names in roster order, for the members keyed by station NAME
 *        (design O3).  Filled once at setup and constant for the run.
 */
struct StationNames {
    std::vector<std::string>   names;    ///< Roster order: names[i] is StationId{i}.
    std::vector<int>           by_name;  ///< Roster indices in name order.
    std::map<std::string, int> index;    ///< Name to roster index.
    /// Replaces the roster with @p roster_names (roster order).
    void assign(std::vector<std::string> roster_names);
};

/// The identity converters the reach table counts round-trip failures for (O2).
enum class Converter : std::uint8_t {
    StationName,  ///< A station name to its StationId (name keys, O3).
    Count
};

/// The name of a converter, as the reach table prints it.
const char* converterName(Converter c);

/**
 * @brief The reach table's own instrument state: the maximum present rows of every declared
 *        field over the sampled epochs, and the round-trip failures per converter.  Trace
 *        class: written by the sampler and the converters, read only by the run-end report.
 */
struct EntityReach {
    std::vector<std::size_t> max_rows;  ///< Per declared field, in the order of @ref kEntityFieldTables.
    long long                sampled{0};
    std::array<long long, static_cast<std::size_t>(Converter::Count)> failures{};
};

/**
 * @brief Every entity table of the run, the station names they convert through, and the
 *        reach table's state.
 */
struct NetworkEntities {
    Table<System, ConstellationState, ByLetter> constellations;
    Table<SatId, SatelliteState>                sats;
    Table<SatSignalId, SatSignalState>          sat_signals;
    StationTable                                stations;
    Table<LosId, LosState>                      los;
    Table<ArcId, ArcState>                      arcs;
    StationNames                                names;   ///< Run constant (setup).
    mutable EntityReach                         reach;   ///< Trace class.
};

/**
 * @brief The station of a name, counting a failure when the roster has no such name.
 * @param e    The entities (their names, and the converter tally).
 * @param name A station name.
 * @return The station, or @c std::nullopt (counted under Converter::StationName).
 */
std::optional<StationId> stationOfName(const NetworkEntities& e, const std::string& name);

// ===========================================================================
// Field access: free functions over a table
// ===========================================================================

/// A pointer to @p V, const when @p T is const.
template <class T, class V>
using FieldPtr = std::conditional_t<std::is_const_v<T>, const V*, V*>;

/**
 * @brief The value of field @p f of row @p id, or null when the row or the field is
 *        absent: the old map's @c find.
 */
template <class T, class R, class V>
FieldPtr<T, V> get(T& t, const typename std::remove_const_t<T>::key_type& id,
                   std::optional<V> R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return nullptr;
    return &*(it->second.*f);
}

/**
 * @brief The value of field @p f of row @p id, created with the row when absent: the old
 *        map's @c operator[].  Serial code only (it may insert a row).
 */
template <class T, class R, class V>
V& put(T& t, const typename T::key_type& id, std::optional<V> R::*f) {
    std::optional<V>& o = t[id].*f;
    if (!o) o.emplace();
    return *o;
}

/// Resets field @p f of row @p id; 1 when it was present: the old map's @c erase(key).
template <class T, class R, class V>
std::size_t drop(T& t, const typename T::key_type& id, std::optional<V> R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return 0;
    (it->second.*f).reset();
    return 1;
}

/// Resets field @p f on every row: the old map's @c clear().
template <class T, class R, class V>
void dropAll(T& t, std::optional<V> R::*f) {
    for (auto& kv : t) (kv.second.*f).reset();
}

/// The number of rows where field @p f is present: the old map's @c size().
template <class T, class R, class V>
std::size_t present(const T& t, std::optional<V> R::*f) {
    std::size_t n = 0;
    for (const auto& kv : t) n += (kv.second.*f) ? 1 : 0;
    return n;
}

/**
 * @brief Calls @p fn(id, value) for every row where field @p f is present, in identity
 *        order: the old map's range loop.  @p fn must not insert rows.
 */
template <class T, class R, class V, class Fn>
void forEach(T& t, std::optional<V> R::*f, Fn fn) {
    for (auto& kv : t)
        if (kv.second.*f) fn(kv.first, *(kv.second.*f));
}

/// True when the set-membership field @p f of row @p id is set: the old set's @c count.
template <class T, class R>
bool has(const T& t, const typename T::key_type& id, bool R::*f) {
    const auto it = t.find(id);
    return it != t.end() && it->second.*f;
}

/// Sets field @p f of row @p id; true when it was not set: the old set's @c insert().second.
template <class T, class R>
bool mark(T& t, const typename T::key_type& id, bool R::*f) {
    bool& b = t[id].*f;
    const bool inserted = !b;
    b = true;
    return inserted;
}

/// Clears field @p f of row @p id; 1 when it was set: the old set's @c erase(key).
template <class T, class R>
std::size_t unmark(T& t, const typename T::key_type& id, bool R::*f) {
    const auto it = t.find(id);
    if (it == t.end() || !(it->second.*f)) return 0;
    it->second.*f = false;
    return 1;
}

/// The number of rows where field @p f is set: the old set's @c size().
template <class T, class R>
std::size_t present(const T& t, bool R::*f) {
    std::size_t n = 0;
    for (const auto& kv : t) n += (kv.second.*f) ? 1 : 0;
    return n;
}

// ---- Fields keyed below the entity: a nested map of the row (a station's signals) ----

/// The value of nested field @p f at (@p id, @p k), or null: the old pair-keyed map's @c find.
template <class T, class R, class M, class SR, class V>
FieldPtr<T, V> get(T& t, const typename std::remove_const_t<T>::key_type& id, M R::*sub,
                   const typename M::key_type& k, std::optional<V> SR::*f) {
    const auto it = t.find(id);
    if (it == t.end()) return nullptr;
    const auto jt = (it->second.*sub).find(k);
    if (jt == (it->second.*sub).end() || !(jt->second.*f)) return nullptr;
    return &*(jt->second.*f);
}

/// The value of nested field @p f at (@p id, @p k), created when absent: @c operator[].
template <class T, class R, class M, class SR, class V>
V& put(T& t, const typename T::key_type& id, M R::*sub, const typename M::key_type& k,
       std::optional<V> SR::*f) {
    std::optional<V>& o = (t[id].*sub)[k].*f;
    if (!o) o.emplace();
    return *o;
}

/// Resets nested field @p f at (@p id, @p k); 1 when it was present: @c erase(key).
template <class T, class R, class M, class SR, class V>
std::size_t drop(T& t, const typename T::key_type& id, M R::*sub,
                 const typename M::key_type& k, std::optional<V> SR::*f) {
    const auto it = t.find(id);
    if (it == t.end()) return 0;
    const auto jt = (it->second.*sub).find(k);
    if (jt == (it->second.*sub).end() || !(jt->second.*f)) return 0;
    (jt->second.*f).reset();
    return 1;
}

/// Resets nested field @p f everywhere: @c clear().
template <class T, class R, class M, class SR, class V>
void dropAll(T& t, M R::*sub, std::optional<V> SR::*f) {
    for (auto& kv : t)
        for (auto& sk : kv.second.*sub) (sk.second.*f).reset();
}

/// The number of (row, sub-key) cells where nested field @p f is present: @c size().
template <class T, class R, class M, class SR, class V>
std::size_t present(const T& t, M R::*sub, std::optional<V> SR::*f) {
    std::size_t n = 0;
    for (const auto& kv : t)
        for (const auto& sk : kv.second.*sub) n += (sk.second.*f) ? 1 : 0;
    return n;
}

/**
 * @brief Calls @p fn(id, k, value) for every cell where nested field @p f is present, in
 *        (identity, sub-key) order: the old pair-keyed map's range loop.
 */
template <class T, class R, class M, class SR, class V, class Fn>
void forEach(T& t, M R::*sub, std::optional<V> SR::*f, Fn fn) {
    for (auto& kv : t)
        for (auto& sk : kv.second.*sub)
            if (sk.second.*f) fn(kv.first, sk.first, *(sk.second.*f));
}

/**
 * @brief Calls @p fn(id, row) for every station row in station-name order (O3): the order
 *        of a map keyed by the name, or by "<name>|..." when no name is a proper prefix of
 *        another (the reach table's station-name line counts prefix pairs).
 */
template <class T, class Fn>
void byStationName(T& t, const StationNames& names, Fn fn) {
    for (int idx : names.by_name) {
        const auto it = t.find(StationId{idx});
        if (it != t.end()) fn(it->first, it->second);
    }
}

// ===========================================================================
// The field tables, the record functions, the reach table
// ===========================================================================

extern const std::vector<FieldDecl> kConstellationFields;  ///< Step N4b.
extern const std::vector<FieldDecl> kSatelliteFields;      ///< Steps N4e1, N4e2.
extern const std::vector<FieldDecl> kSatSignalFields;      ///< Step N4d.
extern const std::vector<FieldDecl> kStationFields;        ///< Step N4a.
extern const std::vector<FieldDecl> kLosFields;            ///< Step N4c.
extern const std::vector<FieldDecl> kArcFields;            ///< Steps N4f1, N4f2.

/// Every entity's field table, in the order the reach table prints them.
extern const std::array<EntityFields, 6> kEntityFieldTables;

/// The snapshot tags the reader restores, in today's restore order (design section 5);
/// scripts/nom_n4_verify.py check V6 compares it with the reader's call order.
extern const std::vector<const char*> kRestoreOrder;

/**
 * @brief The declared Record row of a snapshot tag.
 * @param tag The tag ("Z").
 * @return The row, or null when no Record row declares the tag.
 */
const FieldDecl* recordRow(const char* tag);

/**
 * @brief The snapshot writer's call for one record: the row's @c to_snapshot.
 * @param tag  The tag (a Record row must declare it; a missing row throws).
 * @param ctx  The run context.
 * @param snap The snapshot being filled.
 */
void recordToSnapshot(const char* tag, const NetworkRunContext& ctx, NetworkStateSnapshot& snap);

/**
 * @brief The snapshot reader's call for one record: the row's @c from_snapshot.
 * @param tag  The tag (a Record row must declare it; a missing row throws).
 * @param snap The snapshot read.
 * @param ctx  The run context.
 */
void recordFromSnapshot(const char* tag, const NetworkStateSnapshot& snap, NetworkRunContext& ctx);

/**
 * @brief The reach sampler, an epoch observer (slot StashTail): raises each declared
 *        field's maximum present rows.  Always armed; it writes only @ref EntityReach.
 * @param run The run.
 * @param ep  The epoch (unread).
 */
void networkEntitiesReachSample(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The run-end reach table ("entities as carried"): per declared field its maximum
 *        and final present rows, persistence and record; per converter its round-trip
 *        failures.  Always printed; trace class.
 * @param v The run-end view.
 * @return 0.
 */
int networkEntitiesRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor::net
