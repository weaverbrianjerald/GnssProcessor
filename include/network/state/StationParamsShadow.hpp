#pragma once
/**
 * @file StationParamsShadow.hpp
 * @brief Temporary: a station's parameter set built from its state manager and checked
 *        against it, so that the set can be proved right on every run before any code reads it.
 *
 * ### What it does
 *
 * After a station's measurement pre-pass and split, @ref networkShadowStationParams builds
 * the station's @ref StationParams from the state manager through the one classifier
 * (@c listStateColumns, @c classifyStateColumn) and the split's dispositions, and then checks
 * the set against the state manager and the inventory (@ref SetMismatch):
 *  - the set has one slot per state column;
 *  - each slot's parameter resolves, through the single-receiver lookup
 *    (@c UserParamView), to its own column, and the slot's linearization point is the state's
 *    value there, bit for bit;
 *  - each slot's disposition is the split's, and an ambiguity's re-seed mark is the state
 *    manager's;
 *  - the set's own typed lookup (@ref findParam) finds each slot at its own index;
 *  - the parameter inventory's record of each column and of each withheld state is the set's
 *    slot, converted back to an instance;
 *  - each typed map holds the keys of the state manager's map of its family, in the same
 *    order and at the same columns (the comparators of the identity types reproduce the
 *    string order).
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

/**
 * @brief What a check of the set against the state manager can find.
 */
enum class SetMismatch : std::uint8_t {
    Size = 0,     ///< The set holds a different number of slots than the state has columns.
    Identity,     ///< A slot's parameter does not resolve to its own column.
    XLin,         ///< A slot's linearization point is not the state's value at its column (any bit).
    Disposition,  ///< A slot's disposition is not the split's for its column.
    Reseed,       ///< An ambiguity's re-seed mark differs from the state manager's.
    Lookup,       ///< The set's typed lookup does not find a slot at its own index.
    Inventory,    ///< The inventory's record of a column or a withheld state differs from the set's.
    TypedCount,   ///< A typed map holds a different number of keys than the state manager's map of its family.
    TypedOrder,   ///< A typed map iterates in a different order, or to a different column, than the state manager's map.
};

/// Number of @ref SetMismatch values.
inline constexpr std::size_t kSetMismatches = 9;

/**
 * @brief The name of a finding, as the run-end table prints it.
 * @param m The finding.
 * @return "size", "identity", "x_lin", "disposition", "reseed", "lookup", "inventory",
 *         "typed count" or "typed order".
 */
const char* setMismatchName(SetMismatch m);

/**
 * @brief One station's tally of the checks over the run.  Lives on @c NetStation, written only
 *        by the station's own build inside the parallel produce phase.
 */
struct StationParamsCheck {
    long long builds{0};      ///< Station-epochs the set was built and checked.
    long long incomplete{0};  ///< Station-epochs with a column the classifier refused: no set was built.
    long long failed{0};      ///< Station-epochs whose build or check threw (the set is not trusted).
    long long slots{0};       ///< Slots checked, summed over the builds.
    long long wall_ns{0};      ///< Wall time of the builds and checks, summed over the station-epochs (ns): what the shadow costs.
    /// Findings per kind (see @ref SetMismatch).
    std::array<long long, kSetMismatches> mismatch{};
    std::string first;        ///< The first finding, "TOW <tow>: <kind>: <detail>"; empty when none.

    /**
     * @brief Every finding of every kind.
     * @return The sum of @ref mismatch.
     */
    long long mismatches() const;
};

/**
 * @brief What the build found besides the set.
 */
struct ShadowBuild {
    /// Every column was classified: the set holds one slot per column.  False leaves the set empty.
    bool complete{true};
    /// "<container>:<key>" of every column the classifier refused.
    std::vector<std::string> unclassified;
    /// Columns past the end of the disposition vector (given @c Unobserved in the set).
    long long no_disposition{0};
};

/**
 * @brief Build a station's parameter set from its state manager.
 *
 * Lists the columns (@c listStateColumns), classifies each (@c classifyStateColumn), and
 * appends a slot per column in column order with the value of the state vector, the split's
 * disposition and the ambiguity's re-seed mark; the withheld shared states that the inventory
 * keeps (a classifiable key whose absorbing column the station holds) become
 * @ref StationShared::absorbed.  A column the classifier refuses leaves the set empty and
 * is listed in the result.
 *
 * @param sm       The station's state manager, after its pre-pass.
 * @param col_disp The split's disposition per state index.
 * @param absorbed The shared states the gates withheld from the station this epoch.
 * @param out      The set (overwritten).
 * @return What the build found.
 */
ShadowBuild buildStationParams(const StateManager& sm, const std::vector<ParamDisposition>& col_disp,
                               const StationAbsorbed& absorbed, StationParams& out);

/**
 * @brief Check a station's set against its state manager and the inventory's record, adding
 *        to a tally.
 *
 * @param set       The set (built from @p sm by @ref buildStationParams).
 * @param sm        The station's state manager.
 * @param col_disp  The split's disposition per state index.
 * @param inventory The station's slot of the parameter inventory for the epoch
 *                  (@c recordStationParams), complete.
 * @param station   The station's roster index (the inventory's records carry it).
 * @param tow       GPS time of week of the epoch (s), for the message of a first finding.
 * @param out       The tally (added to).
 */
void checkStationParams(const StationParams& set, const StateManager& sm,
                        const std::vector<ParamDisposition>& col_disp,
                        const StationParamSlot& inventory, int station, double tow,
                        StationParamsCheck& out);

/**
 * @brief Build and check a station's parameter set after its split (inside the parallel produce
 *        phase).
 *
 * Writes only the station's own @c params and @c params_check, and reads its state manager,
 * its measurement model's withheld-state list and the inventory slot of the epoch.  Never
 * throws: a failure is counted in @c StationParamsCheck::failed so that the shadow cannot
 * change what the station does.
 *
 * @param st        The station (@c params and @c params_check written).
 * @param station   The station's roster index.
 * @param col_disp  The split's disposition per state index.
 * @param inventory The station's inventory slot of the epoch (@c recordStationParams has run).
 * @param tow       GPS time of week of the epoch (s).
 */
void networkShadowStationParams(NetStation& st, int station,
                                const std::vector<ParamDisposition>& col_disp,
                                const StationParamSlot& inventory, double tow);

/**
 * @brief The run-end table of the stations' parameter sets.
 *
 * A title, the station and build counts, one line per parameter family (what it is, where the
 * set holds it, the order of its key, its slots and stations in each station's last build),
 * the findings of the checks, the builds that did not happen (a refused column, an error), and
 * one WARNING per station with a finding (its first), at most five.
 *
 * @param stations The run's stations.
 * @return The lines, without trailing newlines and without the log prefix.
 */
std::vector<std::string> stationParamsReport(const std::vector<NetStation>& stations);

} // namespace GnssProcessor
