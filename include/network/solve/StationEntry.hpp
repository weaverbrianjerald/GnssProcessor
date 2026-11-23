#pragma once
/**
 * @file StationEntry.hpp
 * @brief A station refused for a single-band constellation, and the return of a refused station.
 *
 * Two responses, each behind its own key (both default off, identical when off):
 *
 * - **The single-band park** (@c network.station_single_band_park).  When every phase line of
 *   sight of a constellation at a station is on one band, that constellation's receiver phase
 *   bias, inter-system offset and slant ionospheres shift together and no row informs the shift;
 *   the station fails the @c addStation positive-definiteness test and is dropped.  The rule:
 *   the split holds that constellation's receiver phase biases at their seed
 *   (@c ParamDisposition::HeldAtSeed, outside the block; no weight enters, the parameters table
 *   counts them) while the station has no dual-band phase line of sight of it in the rows that
 *   enter the solve; when the second band returns, the free direction is reborn loose as one
 *   re-coupling (networkStationSingleBandReturn).
 * - **The cold entry** (@c network.station_entry_cold).  A station that had data and was not
 *   admitted (refused) is decoupled from the network.  While it is out, and at the epoch it
 *   returns, the chain forgets the station's tree memory (its arcs' pin targets, its own parent
 *   edges, the satellites whose parent edge it held), and its carried states are dropped, so its
 *   edges are elected as first sightings and its states are born loose from the return epoch.
 *
 * Both edges of each are events with the station's identity under @c network.trace_topology
 * (@c sta_column_parked / @c sta_column_unparked, @c sta_refused / @c sta_returned) and run-end
 * counters (@ref GnssProcessor::StationEntryCounters "StationEntryCounters").
 */

#include "common/GnssIdentity.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunContext;
struct NetStation;
struct StationAssembly;
struct NetworkRunEndView;
struct NetworkRunTraces;
struct GnssTime;
namespace net { struct NetworkEntities; struct StationEntryRecord; }

/**
 * @brief The run-end counters of the park and the cold entry (a base of
 *        @c NetworkServiceCounters).  Nothing in the solve reads them.
 */
struct StationEntryCounters {
    long long sen_park_station_epochs = 0;   ///< Admitted station-epochs with at least one receiver phase bias held at its seed.
    long long sen_park_unpark_drops = 0;     ///< Carried entries of the free direction dropped when a park episode closed (reborn loose).
    long long sen_null_diag_directions = 0;  ///< Directions below the null bar counted by the measure-only diagnostic of refused stations.
    long long sen_park_columns = 0;          ///< Columns parked, summed over station-epochs.
    long long sen_park_kept = 0;             ///< Station-epochs with a parked column that then entered the solve.
    long long sen_park_episodes = 0;         ///< Park episodes opened (a station's first parked epoch after one without).
    long long sen_park_episodes_closed = 0;  ///< Park episodes closed (the first admitted epoch without a parked column).
    std::map<std::string, long long> sen_park_family;   ///< Parked columns by family ("RPB", "CBIAS", "ISB", ...).
    long long sen_refused_events = 0;        ///< Decoupling edges: a station with data that was not admitted after an epoch in the solve or at its first epoch.
    long long sen_returned_events = 0;       ///< Re-coupling edges: a refused station admitted again.
    long long sen_returned_first_entry = 0;  ///< …of which the station had never been in the solve (refused from its first epoch).
    long long sen_out_epochs_max = 0;        ///< Longest absence that ended in a return (epochs).
    long long sen_out_epochs_sum = 0;        ///< Epochs out, summed over returns.
    long long sen_cold_station_epochs = 0;   ///< Station-epochs at which the cold entry ran (the station out at the epoch's start).
    long long sen_cold_carries_dropped = 0;  ///< Carried entries dropped by the cold entry.
    long long sen_cold_chain_cells = 0;      ///< Chain memory cells forgotten by the cold entry (pin targets, parent edges, incumbents).
    /// The family that failed at this epoch's refusal of each station (the null eigenvector's top
    /// family when @c network.trace_station_pd_null names it, "unnamed" otherwise, "assembly"
    /// for an assembly failure).  Written at the drop site, read by the track the same epoch.
    std::map<std::string, std::string> sen_refuse_family_ep;
};

/**
 * @brief The constellations whose phase rows entering the solve are all on one band at a station.
 *
 * Read on the rows of the epoch that keep their weight (satellites without a clock and screened
 * rows are not in them): a constellation is single-band when no satellite has phase rows on two
 * frequency digits.  The split holds such a constellation's receiver phase biases at their seed
 * (@c ParamDisposition::HeldAtSeed, outside the block) under @c network.station_single_band_park:
 * its receiver phase bias, inter-system offset and slant ionospheres would otherwise shift
 * together with no row informing the shift, and the station would fail the positive-definiteness
 * test.
 *
 * @param satellites The satellite of each data row ("E02").
 * @param signals    The observation code of each data row ("L1C", "C5Q").
 * @param excluded   Per row: excluded at zero weight (may be empty).
 * @return The single-band constellations (a constellation with no phase row is not listed).
 */
std::set<System> networkSingleBandConstellations(const std::vector<std::string>& satellites,
                                                 const std::vector<std::string>& signals,
                                                 const std::vector<char>& excluded);

/**
 * @brief The return of a single-band constellation's second band: its free direction is reborn
 *        loose as one re-coupling.
 *
 * Drops, for the station, the carries of the held receiver phase biases @p rpb_keys and of any
 * carried inter-system offset (the receiver-clock family entry of the constellation's name) and
 * slant ionosphere of the same constellation.
 *
 * @param entities The entities.  // written
 * @param station  The station's roster index.
 * @param rpb_keys The receiver phase-bias keys held during the episode ("Galileo_RPB_L1C").
 * @return The number of carried entries dropped.
 */
long long networkStationSingleBandReturn(net::NetworkEntities& entities, int station,
                                         const std::vector<std::string>& rpb_keys);

/**
 * @brief Measure-only diagnostic of a refused station's free directions.
 *
 * Gate: returns 0 at once unless @c network.station_single_band_park.  Counts the eigenvectors of
 * the block the positive-definiteness test judges whose eigenvalue is below 1e-12 of the largest,
 * and with @p traces writes one @c sta_null_direction row for each: the eigenvalue and its gap to
 * the next, what a pin along it would pass to the shared border, and its five largest columns.
 * Changes nothing.
 *
 * @param ctx    The run context (knobs, counters).
 * @param st     The station.
 * @param R      The refused station's assembly.
 * @param traces The trace streams, or null for counts only.
 * @param t      The epoch time of the rows.
 * @return The number of directions below the bar.
 */
int networkStationNullDiagnostic(NetworkRunContext& ctx, const NetStation& st,
                                 const StationAssembly& R, NetworkRunTraces* traces = nullptr,
                                 const GnssTime* t = nullptr);

/// What the cold entry did to one station.
struct StationColdDrop {
    long long carries{0};      ///< Carried entries dropped (wet delay, receiver clock and drift, receiver code and phase biases, slant ionosphere, phase colour, leveling ambiguities).
    long long chain_cells{0};  ///< Chain memory cells forgotten (arc pin targets, the station's parent edges, the satellites whose parent edge it held).
};

/**
 * @brief Drop one station's carried states and forget its chain memory (the cold entry's work on
 *        one station).
 *
 * @param entities The entities.  // written
 * @param station  The station's roster index.
 * @param name     Its name (the satellites' incumbent parent edges name it).
 * @return What was dropped and forgotten.
 */
StationColdDrop networkStationColdDrop(net::NetworkEntities& entities, int station,
                                       const std::string& name);

/// The edges one epoch's track found for one station.
struct StationEntryEdges {
    bool park_opened{false};      ///< A park episode opened (admitted with a parked column after an epoch without).
    bool park_closed{false};      ///< A park episode closed (admitted without a parked column after an episode).
    long long park_epochs{0};     ///< The closed episode's length (epochs), when @c park_closed.
    bool refused{false};          ///< The decoupling edge: the station refused after an epoch not refused.
    bool returned{false};         ///< The re-coupling edge: the station admitted after a refusal.
    bool first_entry{false};      ///< ...and it had never been in the solve.
    long long out_epochs{0};      ///< The absence that ended, when @c returned (epochs).
    std::vector<std::string> unparked_keys;   ///< The keys of the closed park episode, when @c park_closed.
};

/**
 * @brief Advance one station's entry record by one epoch.
 *
 * @param rec            The station's record.  // written
 * @param in_solve       The station entered the solve this epoch.
 * @param refused        It had data and was not admitted this epoch.
 * @param parked         Its number of parked columns this epoch.
 * @param families       The parked families (kept when an episode opens).
 * @param tow            This epoch's TOW (the refusal's time).
 * @param refuse_family  The family that failed, when @p refused ("unnamed" when empty).
 * @param parked_keys    The keys parked this epoch (gathered into the episode).
 * @return The edges of this epoch.  On a return the record's absence is closed by the caller
 *         after it has read @c refused_at_tow and @c family.
 */
StationEntryEdges networkStationEntryStep(net::StationEntryRecord& rec, bool in_solve, bool refused,
                                          int parked, const std::string& families, double tow,
                                          const std::string& refuse_family,
                                          const std::vector<std::string>& parked_keys = {});

/**
 * @brief The cold entry: forget the chain memory and drop the carries of every station that is
 *        out of the solve at the epoch's start.
 *
 * Gate: returns at once unless @c network.station_entry_cold.  Runs before the chain election
 * and the station build.  A station is out when the track of the previous epoch recorded it
 * refused (@c net::StationEntryRecord::out_epochs > 0).
 *
 * @param run The run (entities, counters).
 * @param ep  The epoch (its time, for the trace).
 */
void networkStationEntryCold(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The track of both responses, after the station loop: opens and closes park episodes,
 *        and writes the refusal and the return of each station with their events.
 *
 * Gate: returns at once unless either key is armed.  Called from the solve track on an epoch
 * whose station loop completed.
 *
 * @param run          The run (entities, counters, traces).
 * @param ep           The epoch (its time).
 * @param in_solve_of  Per station: it entered the solve this epoch.
 * @param refused_of   Per station: it had data and was not admitted this epoch.
 * @param parked_of    Per station: its number of parked columns this epoch.
 * @param families_of  Per station: the parked families, space-separated.
 * @param keys_of      Per station: the parked receiver phase-bias keys.
 */
void networkStationEntryTrack(NetworkRun& run, const NetworkEpoch& ep,
                              const std::vector<char>& in_solve_of,
                              const std::vector<char>& refused_of,
                              const std::vector<int>& parked_of,
                              const std::vector<std::string>& families_of,
                              const std::vector<std::vector<std::string>>& keys_of);

/**
 * @brief Run-end report: the park and the cold entry.
 *
 * Gate: returns 0 at once unless either key is armed.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkStationEntryRunEnd(const NetworkRunEndView& v);

}  // namespace GnssProcessor
