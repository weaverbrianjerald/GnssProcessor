#pragma once
/**
 * @file NetworkWithinEpochCulprits.hpp
 * @brief The within-epoch RAIM tier's culprit sets and their hand-off to the DD-carry
 *        rebuild (card C3c section 3; card WCF): the set type, the run counters of both
 *        ends of the hand-off, the verdict that reads them, and the run-end report.
 *
 * ## The hand-off
 *
 * When the post-solve sigma0 of an epoch trips the within-epoch tier (T2,
 * @c network.raim_within_epoch, inside networkSolveGlue), the tier picks a culprit and
 * re-solves the epoch without it, up to a few times: a satellite whose ambiguities refloat
 * at every station, one satellite at one station (an isolated arc), or a whole station,
 * which the re-solve leaves out of the estimator.  Each re-solve rebuilds the DD-carry
 * priors (networkBuildCarryDdPriors, and the MW wide-lane rows it appends,
 * networkMwWlInject).  A culprit satellite or arc invalidates its carried double
 * differences, so the rebuild treats every carried ambiguity a culprit names as slipped
 * and re-taps the chord, instead of folding the stale carried value again.
 *
 * The sets travel through the epoch: one @ref WithinEpochCulprits lives in the epoch's
 * workspace (@c EpochWorkspace::we_culprits, filter/NetworkRunTypes.hpp), the T2 block is
 * its only writer, and the driver's rebuild callback reads that member of the same
 * workspace.  The block clears the sets when it starts and when it ends (after copying the
 * landed state to @c NetworkSolveGlueResult::we_landed for the fixed-cycle ledger), so the
 * primary build of every epoch withholds nothing.
 *
 * ## The defect it closes
 *
 * The sets used to reach the rebuild through two pointer parameters.  The solve-glue cut
 * of 2026-07-24 (dee2824) took the pointers by value and pointed its own copies at the
 * sets, while the rebuild callback read the driver's pointers, which stayed null: no
 * rebuild withheld a culprit from then until card WCF.  Nothing counted the withhold, so
 * the dead wiring was invisible (standing invariant 4).  Both ends are now counted
 * (@ref WithinEpochCulpritTally) and the run end reads them against each other
 * (@ref withinEpochCulpritVerdict).
 *
 * ## Off
 *
 * The rebuild runs only under @c network.carry_invariant, and the tier only with
 * @c network.raim_within_epoch and a warm phase-leveling carry.  With either off the sets
 * stay empty or unread and the run is unchanged; both goldens run without
 * @c carry_invariant.
 */

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

struct NetworkRunEndView;

/**
 * @brief The culprits of the within-epoch tier's re-solve: what the re-solve refloats or
 *        excludes.
 *
 * Station indices are roster (app) indices, the index of a station in the run's station
 * list.  Two readers: during the T2 block, the DD-carry rebuild of each re-solve reads the
 * epoch workspace's instance (satellites and arcs only: an excluded station is out of the
 * estimator, so no rebuild sees it); after the block, the fixed-cycle ledger reads the
 * landed copy (@c NetworkSolveGlueResult::we_landed, card FCF; networkArLatticeEvidence).
 */
struct WithinEpochCulprits {
    std::set<std::string>                sats;      ///< Satellites refloated at every station.
    std::map<int, std::set<std::string>> arcs;      ///< Station to the satellites refloated at that station only.
    std::set<int>                        stations;  ///< Stations excluded from the re-solve.

    /**
     * @brief The number of (station, satellite) arcs in @ref arcs.
     * @return The sum of the per-station satellite counts.
     */
    std::size_t arcCount() const {
        std::size_t n = 0;
        for (const auto& kv : arcs) n += kv.second.size();
        return n;
    }

    /**
     * @brief Whether a satellite or an arc is named: the culprits whose carried ambiguities
     *        a DD-carry rebuild withholds.
     * @return True when @ref sats or @ref arcs names at least one satellite.
     */
    bool namesAmbiguities() const { return !sats.empty() || arcCount() > 0; }

    /**
     * @brief Whether nothing is named.
     * @return True when no satellite, no arc and no station is named.
     */
    bool empty() const { return !namesAmbiguities() && stations.empty(); }

    /**
     * @brief Whether a culprit names the ambiguities of satellite @p sat at station
     *        @p station: the satellite is named at every station, or the arc
     *        (@p station, @p sat) is named.
     * @param station The station's roster index.
     * @param sat     The satellite identifier, e.g. "G05".
     * @return True when the rebuild treats that arc's carried ambiguities as slipped.
     */
    bool names(int station, const std::string& sat) const {
        if (sats.count(sat)) return true;
        const auto it = arcs.find(station);
        return it != arcs.end() && it->second.count(sat) != 0;
    }

    /// @brief Names nothing again (the start and the end of the T2 block).
    void clear() {
        sats.clear();
        arcs.clear();
        stations.clear();
    }
};

/**
 * @brief Run counters of the culprit hand-off, one per end (card WCF; standing
 *        invariant 4).  In @c NetworkServiceCounters::we_culprit_withhold.
 *
 * The writer's count is taken by the re-solve just before it calls the rebuild, the
 * reader's by the rebuild itself where the withhold takes effect, so a hand-off that stops
 * reaching the rebuild shows as re-solves with a culprit and no rebuild that read one.
 * The MW wide-lane rows read the same set inside one rebuild and count nothing.
 */
struct WithinEpochCulpritTally {
    long long resolves = 0;  ///< Writer: T2 re-solves (undo replays included) that ran with a satellite or arc culprit.
    long long rebuilds = 0;  ///< Reader: DD-carry rebuilds that read a set naming a satellite or arc.
    long long sats     = 0;  ///< Culprit satellites those rebuilds withheld, summed over the rebuilds.
    long long arcs     = 0;  ///< Culprit arcs those rebuilds withheld, summed over the rebuilds.
    long long ambs     = 0;  ///< Carried ambiguities those rebuilds marked slipped for a culprit and not for the slip funnel, each counted once per rebuild.
};

/// What the two ends of the hand-off say about each other at the run end.
enum class WithinEpochCulpritVerdict {
    NoCulprit,    ///< Both counts are 0: no re-solve had a satellite or arc culprit.
    Agree,        ///< The counts agree and are positive: every such re-solve's rebuild read its culprits.
    InertWiring,  ///< Re-solves had culprits and no rebuild read one: the hand-off is dead.
    OutOfStep,    ///< Any other disagreement between the two counts.
};

/// A verdict with its log level and its sentence.
struct WithinEpochCulpritReading {
    WithinEpochCulpritVerdict verdict;  ///< The verdict.
    bool                      warn;     ///< True when the run-end line is logged at warn level.
    const char*               text;     ///< The sentence that closes the run-end line.
};

/**
 * @brief Read the two ends of the culprit hand-off against each other.
 *
 * Pure: depends on @p t alone.  NoCulprit and Agree read at info level; InertWiring and
 * OutOfStep at warn level.
 *
 * @param t The run's tally.
 * @return The verdict, its level and its sentence.
 */
WithinEpochCulpritReading withinEpochCulpritVerdict(const WithinEpochCulpritTally& t);

/**
 * @brief Run-end report: the culprit withhold on the DD carry, both ends of the hand-off
 *        and the verdict (one line).
 *
 * Gate: the DD carry (@c network.carry_invariant, effective, i.e. with the eq34 phase
 * datum) and the within-epoch tier (@c network.raim_within_epoch) both armed; returns 0 at
 * once otherwise.  The row follows the within-epoch RAIM report in the run-end table.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkWithinEpochCulpritWithholdRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
