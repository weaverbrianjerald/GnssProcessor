#pragma once
/**
 * @file StationEntry.hpp
 * @brief A station refused for a null direction of its local block, and the return of a refused station.
 *
 * Two responses, each behind its own key (both default off, identical when off):
 *
 * - **The null pin** (@c network.station_local_null_pin).  A station's local block that has a
 *   direction no row informs (a column with no row; the Galileo receiver phase bias, ISB and slant
 *   ionosphere of a station whose Galileo lines of sight are all single-frequency) fails the
 *   whole station at the @c addStation positive-definiteness test.  The null pin appends one
 *   row along each such direction at its linearization point, which changes no other estimate
 *   (the direction is invisible to the data and to the shared border), retries the station
 *   once, and removes the direction's columns from every harvest list, so no carried value is
 *   written from them.
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

#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct NetworkRunContext;
struct NetStation;
struct StationAssembly;
struct NetworkRunEndView;
namespace net { struct NetworkEntities; struct StationEntryRecord; }

/**
 * @brief The run-end counters of the park and the cold entry (a base of
 *        @c NetworkServiceCounters).  Nothing in the solve reads them.
 */
struct StationEntryCounters {
    long long sen_park_station_epochs = 0;   ///< Refused station-epochs that the null pin retried (at least one pin row).
    long long sen_null_pin_rows = 0;         ///< Pin rows appended, summed over station-epochs.
    long long sen_null_coupled_skipped = 0;  ///< Directions below the null bar left unpinned because a pin would pass information to the shared border (a datum direction of the network).
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
 * @brief The null pin: make a refused station's local block positive-definite by pinning its null
 *        directions, without changing any other estimate.
 *
 * Gate: returns 0 at once unless @c network.station_local_null_pin.  Called at the drop site
 * after the station's @c addStation failed.  The block judged is the test's own
 * (@f$A_r^\top W A_r@f$ plus the pre-PD prior).  A direction @f$v@f$ with @f$N v = 0@f$ has
 * @f$A_r v = 0@f$, so it is invisible to every row and to the shared border; a row along it at
 * the linearization point fixes it and moves nothing else.  Null directions are the eigenvectors
 * whose eigenvalue is below 1e-12 of the largest (the kernel's own count) and whose pin would pass
 * the shared border at most 1e-14 of the station's shared block (|c|^2 / ev with
 * c = A_s^T W A_r v; a direction coupled to the border is a datum direction of the network and is
 * left alone); each gets one row of weight 1e-6 of the median of the other eigenvalues (tag
 * @c LocalNullPin).  The columns with a weight of at least
 * 0.05 in a pinned direction are parked: removed from every harvest list (a column with a post-PD
 * carry keeps its harvest).  A rowless column is the special case @f$v = e_c@f$.
 *
 * @param ctx The run context (knobs, counters).
 * @param st  The station (its parameter slots name the columns).
 * @param R   The station's assembly, modified in place.  // written
 * @return The number of pin rows appended (0: no null direction, the station stays refused).
 */
int networkStationNullPin(NetworkRunContext& ctx, const NetStation& st, StationAssembly& R);

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
 * @return The edges of this epoch.  On a return the record's absence is closed by the caller
 *         after it has read @c refused_at_tow and @c family.
 */
StationEntryEdges networkStationEntryStep(net::StationEntryRecord& rec, bool in_solve, bool refused,
                                          int parked, const std::string& families, double tow,
                                          const std::string& refuse_family);

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
 */
void networkStationEntryTrack(NetworkRun& run, const NetworkEpoch& ep,
                              const std::vector<char>& in_solve_of,
                              const std::vector<char>& refused_of,
                              const std::vector<int>& parked_of,
                              const std::vector<std::string>& families_of);

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
