#pragma once
/**
 * @file StationParamsShadow.hpp
 * @brief Temporary: a station's parameter set built from its state manager, the inventory
 *        recorded from it, and both checked against the state manager, so that the readers of
 *        the set can be proved right on every run.
 *
 * ### What it does
 *
 * After a station's measurement pre-pass and split, @ref networkShadowStationParams builds the
 * station's @ref StationParams from the state manager through the one classifier
 * (network/state/StateColumns.hpp) and the split's dispositions, records the parameter
 * inventory's slot from the set (@c recordStationParams), and then checks the set and the
 * record against the state manager (@ref SetMismatch):
 *  - the set has one slot per state column;
 *  - each slot's parameter resolves, through the single-receiver lookup
 *    (@c UserParamView), to its own column, and the slot's linearization point is the state's
 *    value there, bit for bit; a column the classifier refused is an unnamed slot with the
 *    state's value;
 *  - each slot's disposition is the split's, an ambiguity's re-seed and unresolvable marks are
 *    the state manager's, and a phase colour's elevation is the state manager's, bit for bit;
 *  - the set's own typed lookup (@ref findParam) finds each slot at its own index;
 *  - the inventory recorded from the set is the inventory the state manager alone gives
 *    (@ref recordStationParamsFromState: every record, the unclassified list and the two
 *    counters);
 *  - each typed map holds the keys of the state manager's map of its family, in the same
 *    order and at the same columns (the comparators of the identity types reproduce the
 *    string order).
 *
 * A second check runs at the start of each epoch's PASS A (@ref networkCheckStationParamsFresh):
 * the set still holds the station's local columns as its state manager does.  The readers that
 * run before a station's next split (the slip drains, the admissions, the anchor election) read
 * the set of the previous epoch, so they are right exactly when that holds.  A station whose
 * assembly ends before its split (no innovations, an H/state mismatch) or fails (a step threw)
 * has its set rebuilt without dispositions (@ref networkRefreshStationParams), because its
 * measurement pre-pass has already changed its columns.
 *
 * The counts are kept per station (the check runs inside the parallel produce phase, so each
 * station writes only its own) and printed once at the run end as "the station parameter set"
 * (@ref stationParamsReport).  A nonzero count is a defect of the set, never a property of the
 * data.
 *
 * Removed when the station no longer has a state manager; the set and its lookups stay.
 *
 * Plain value types and free functions.
 */

#include "network/state/StationParams.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;
struct NetStation;
struct StationAbsorbed;
struct StationParamSlot;

/**
 * @brief What a check of the set against the state manager can find.
 */
enum class SetMismatch : std::uint8_t {
    Size = 0,     ///< The set holds a different number of slots than the state has columns.
    Identity,     ///< A slot's parameter does not resolve to its own column.
    XLin,         ///< A slot's linearization point is not the state's value at its column (any bit).
    Disposition,  ///< A slot's disposition is not the split's for its column.
    Reseed,       ///< An ambiguity's re-seed mark differs from the state manager's.
    Unresolvable, ///< An ambiguity's unresolvable mark (left out of the integer search) differs from the state manager's.
    Elevation,    ///< A phase colour's elevation is not the state manager's (any bit).
    Lookup,       ///< The set's typed lookup does not find a slot at its own index.
    Inventory,    ///< The inventory recorded from the set differs from the one the state manager alone gives.
    TypedCount,   ///< A typed map holds a different number of keys than the state manager's map of its family.
    TypedOrder,   ///< A typed map iterates in a different order, or to a different column, than the state manager's map.
    Stale,        ///< At the start of an epoch a local family of the set differs from the state manager's (the set was not rebuilt after a change).
};

/// Number of @ref SetMismatch values.
inline constexpr std::size_t kSetMismatches = 12;

/**
 * @brief The name of a finding, as the run-end table prints it.
 * @param m The finding.
 * @return "size", "identity", "x_lin", "disposition", "reseed", "unresolvable", "elevation",
 *         "lookup", "inventory", "typed count", "typed order" or "stale".
 */
const char* setMismatchName(SetMismatch m);

/**
 * @brief One station's tally of the checks over the run.  Lives on @c NetStation, written only
 *        by the station's own build inside the parallel produce phase.
 */
struct StationParamsCheck {
    long long builds{0};        ///< Station-epochs the set was built, recorded and checked.
    long long refreshed{0};     ///< Station-epochs whose assembly ended before the split or failed, and whose set was rebuilt without dispositions.
    long long fresh_checks{0};  ///< Epoch starts at which the set was checked against the state manager's local columns.
    long long unclassified{0};  ///< Of those, the station-epochs with a column or a withheld state the classifier refused.
    long long failed{0};        ///< Station-epochs whose build or check threw (the set is not trusted).
    long long slots{0};         ///< Slots checked, summed over the builds.
    long long wall_ns{0};       ///< Wall time of the builds and checks, summed over the station-epochs (ns): what the shadow costs.
    /// Findings per kind (see @ref SetMismatch).
    std::array<long long, kSetMismatches> mismatch{};
    std::string first;          ///< The first finding, "TOW <tow>: <kind>: <detail>"; empty when none.

    /**
     * @brief Every finding of every kind.
     * @return The sum of @ref mismatch.
     */
    long long mismatches() const;
};

/**
 * @brief Build a station's parameter set from its state manager.
 *
 * Lists the columns (@c listStateColumns) and appends a slot per column in column order with
 * the value of the state vector, the split's disposition, the ambiguity's re-seed and
 * unresolvable marks and the phase colour's elevation.  A
 * column the classifier refuses (@c classifyStateColumn) becomes an unnamed slot and its
 * "<container>:<key>" is listed in @ref StationParams::unclassified.  The withheld shared
 * states that the inventory keeps (a classifiable key whose absorbing column the station holds)
 * become @ref StationShared::absorbed; one whose absorbing column is gone is counted in
 * @ref StationShared::absorbed_no_absorber, one whose key the classifier refuses is listed
 * after the columns' own as "absorbed <container>:<key>".
 *
 * @param sm       The station's state manager, after its pre-pass.
 * @param col_disp The split's disposition per state index.
 * @param absorbed The shared states the gates withheld from the station this epoch.
 * @param out      The set (overwritten; left as it was when the build throws).
 */
void buildStationParams(const StateManager& sm, const std::vector<ParamDisposition>& col_disp,
                        const StationAbsorbed& absorbed, StationParams& out);

/**
 * @brief The parameter inventory's slot of a station, recorded from its state manager alone.
 *
 * The record the inventory made before it read the set: every column of @p sm classified and
 * paired with the split's disposition, then each withheld state whose absorbing column
 * @p sm holds.  Kept as the reference the recorded-from-the-set inventory is checked against.
 * Pure per-station work: writes only @p slot.
 *
 * @param sm       The station's state manager.
 * @param station  The station's roster index.
 * @param col_disp The split's disposition per state index (a shorter vector than the state is
 *                 counted in @c slot.no_disposition).
 * @param absorbed The station's withheld shared states of this epoch.
 * @param slot     The station's slot (overwritten).
 */
void recordStationParamsFromState(const StateManager& sm, int station,
                                  const std::vector<ParamDisposition>& col_disp,
                                  const StationAbsorbed& absorbed, StationParamSlot& slot);

/**
 * @brief Check a station's set and its recorded inventory against its state manager, adding to
 *        a tally.
 *
 * @param set       The set (built from @p sm by @ref buildStationParams).
 * @param sm        The station's state manager.
 * @param col_disp  The split's disposition per state index.
 * @param absorbed  The shared states the gates withheld from the station this epoch.
 * @param inventory The station's slot of the parameter inventory for the epoch, recorded from
 *                  @p set (@c recordStationParams).
 * @param station   The station's roster index (the inventory's records carry it).
 * @param tow       GPS time of week of the epoch (s), for the message of a first finding.
 * @param out       The tally (added to).
 */
void checkStationParams(const StationParams& set, const StateManager& sm,
                        const std::vector<ParamDisposition>& col_disp, const StationAbsorbed& absorbed,
                        const StationParamSlot& inventory, int station, double tow,
                        StationParamsCheck& out);

/**
 * @brief Build a station's parameter set after its split, record the inventory from it and
 *        check both (inside the parallel produce phase).  Throws when the set cannot be built.
 *
 * Writes only the station's own @c params, @c params_check and @p inventory, and reads its
 * state manager and its measurement model's withheld-state list.  A failed check is counted in
 * @c StationParamsCheck::failed and changes nothing else.  A failed build is counted too, the
 * set is left empty, and the exception propagates: the carried-prior steps read the set, so the
 * station's assembly fails for the epoch (the produce loop records the error and the solve
 * drops the station with a warning) instead of continuing with no parameters.  The produce
 * loop then rebuilds the set from the state manager (@ref networkRefreshStationParams), so
 * the set stays empty only if that build fails as well.
 *
 * @param st        The station (@c params and @c params_check written).
 * @param station   The station's roster index.
 * @param col_disp  The split's disposition per state index.
 * @param inventory The station's inventory slot of the epoch (written).
 * @param tow       GPS time of week of the epoch (s).
 */
void networkShadowStationParams(NetStation& st, int station,
                                const std::vector<ParamDisposition>& col_disp,
                                StationParamSlot& inventory, double tow);

/**
 * @brief Build a station's parameter set from its state manager when the station is
 *        constructed: the core columns (position, clock, wet delay, inter-system biases), with
 *        no dispositions.
 *
 * The set then holds what the state manager holds from the first epoch on, and the first
 * start-of-epoch check finds nothing.  Not counted as a refresh.
 *
 * @param st The station (@c params written; @c sm set).
 */
void networkStartStationParams(NetStation& st);

/**
 * @brief Rebuild a station's parameter set from its state manager when the station's assembly
 *        ends before its split or fails (inside the parallel produce phase).
 *
 * The station's measurement pre-pass has run, so its columns may have changed (a birth, a
 * prune, a reallocated receiver phase bias) while the split, which builds the set, has not
 * (an assembly that ends early, or a step that threw before the set's own build, or in it).
 * The set is rebuilt with no dispositions (nothing decided them) and no inventory record or
 * check.  A failed build is counted in @c StationParamsCheck::failed and leaves the set empty:
 * the readers of the station's next PASS A then find no local column, the start-of-epoch
 * check counts the difference as stale, and the next build or rebuild replaces the set.
 * Nothing is thrown, because the station is dropped for the epoch anyway.
 *
 * @param st The station (@c params and @c params_check written).
 */
void networkRefreshStationParams(NetStation& st);

/**
 * @brief Check, at the start of an epoch's PASS A, that a station's set still holds its local
 *        columns as its state manager does.
 *
 * The inter-system biases, slant ionospheres, phase colours, ambiguities, receiver phase
 * biases and receiver code biases (keys, order and columns) and the wet-delay column.  The
 * border families are not compared: the admissions change them during the epoch, before the
 * split rebuilds them.  A difference is counted as @ref SetMismatch::Stale.  Writes only the
 * station's own tally and never throws.
 *
 * @param st  The station (@c params_check written).
 * @param tow GPS time of week of the epoch (s), for the message of a first finding.
 */
void networkCheckStationParamsFresh(NetStation& st, double tow);

/**
 * @brief The run-end table of the stations' parameter sets.
 *
 * A title, the station and build counts, one line per parameter family (what it is, where the
 * set holds it, the order of its key, its slots and stations in each station's last build),
 * the findings of the checks, the builds with a refused column or an error, and one WARNING
 * per station with a finding (its first), at most five.
 *
 * @param stations The run's stations.
 * @return The lines, without trailing newlines and without the log prefix.
 */
std::vector<std::string> stationParamsReport(const std::vector<NetStation>& stations);

} // namespace GnssProcessor
