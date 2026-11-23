#pragma once
/**
 * @file UnrecordedSatGate.hpp
 * @brief The consumer's rule for satellites the network stream does not correct: such a satellite
 *        is left out of the solution while it lacks its stream records.
 *
 * A consumer of a network product stream corrects most of its satellites with the stream's clock
 * and biases.  A satellite the stream carries no clock record for keeps the a priori clock and no
 * bias, so a term the stream's corrections share (the network's phase level moving against its code
 * level) is, for the consumer, a difference between a corrected and an uncorrected satellite in
 * every single difference that pairs the two.  A satellite with the stream's clock and no phase-bias
 * record for a phase signal the receiver uses is not phase-corrected either: under the emission hold
 * in its phase-clock mode the clock less the phase bias is the constant of a generation and the clock
 * record alone wanders with the network's code-level clock.  An uncorrected satellite can also be
 * elected the reference satellite of its constellation, and its metre-level residuals can trip the
 * filter's complete reset.
 *
 * Two rules, each armed by its own key:
 *  - the clock rule (@c processing.ssr_drop_unrecorded, derived from
 *    @c network.closed_loop.drop_unrecorded): a satellite without a clock record is left out;
 *  - the phase-bias rule (@c processing.ssr_drop_unrecorded_phase_bias, derived from
 *    @c network.closed_loop.drop_unrecorded_phase_bias): a satellite without a clock record, or with
 *    one and without a phase-bias record for one of the phase signals the receiver uses for it, is
 *    left out (the whole satellite, also when only one of its signals lacks the record).
 *
 * This gate holds the decisions and their counters and nothing else: it is pure, so the edges are
 * unit-tested on hand-built epochs (tests/test_unrecorded_sat_gate.cpp,
 * tests/test_unrecorded_sat_gate_edges.cpp).  The engine side (@c RealTimeEngine::dropUnrecorded,
 * RealTimeEnginePrepare.cpp) asks the store for each modelled satellite's records with the freshness
 * tests of the apply sites, erases the satellites the gate leaves out and logs the events.
 *
 * Edges (standing invariant 7: an exclusion is an event at both edges).  A satellite seen without its
 * records is a Drop, which names the missing record (the clock, or the phase bias of a named signal),
 * its cause (a first sighting before any record, a record lost while the satellite was in view, or a
 * return to view on a record that is too old), the age of the last record of that kind and whether the
 * satellite was its constellation's reference at the previous epoch.  A dropped satellite whose
 * records are all back is a Readmit, which carries the length of the gap.  A dropped satellite that is
 * out of view keeps its dropped mark, so its next sighting without its records makes no second Drop,
 * and with them it is a Readmit; a dropped satellite whose missing record changes kind stays out
 * without a second Drop.  At the Drop the model prunes the satellite's ambiguity and ionosphere states,
 * as for a satellite out of view; at the Readmit they are seeded afresh, a new float, so nothing
 * estimated without the stream's corrections enters the stream's frame.
 *
 * The solvable floor.  When fewer satellites keep their records than the floor the engine passes
 * (the corrections-ready gate's @c min_satellites), the epoch has no corrected solution: every
 * satellite is left out of it, and the first and last such epochs of a run of them are the edges
 * NoSolutionBegin and NoSolutionEnd.  A stopped stream is the extreme case.
 */

#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Decisions and counters of the rule for satellites without their stream records.
 */
class UnrecordedSatGate {
public:
    /// @brief The kind of an edge.
    enum class EventKind {
        Drop,             ///< A satellite seen without its records is left out.
        Readmit,          ///< A dropped satellite's records are all back: it is in the solution, its states afresh.
        NoSolutionBegin,  ///< Fewer satellites than the floor keep their records: no corrected solution from here.
        NoSolutionEnd     ///< The floor is met again: corrected solutions resume.
    };

    /// @brief Which of a satellite's stream records is missing at an epoch.
    enum class Missing {
        None,       ///< Every record the rule asks for is present: the satellite is kept.
        Clock,      ///< No clock record.
        PhaseBias   ///< A clock record, and no phase-bias record for a phase signal the receiver uses.
    };

    /// @brief How a Drop began.
    enum class DropCause {
        FirstSighting,  ///< The satellite was never seen with its records.
        RecordLost,     ///< The satellite was in the previous epoch's set and its record stopped.
        BackOnStale     ///< The satellite returns to view while its only record is too old.
    };

    /// @brief Number of record-age bins of the Drop counters (none, up to 60 s, 5 min, 30 min, longer).
    static constexpr std::size_t kAgeBins = 5;
    /// @brief Number of gap bins of the Readmit counters (up to 60 s, 5 min, 30 min, longer).
    static constexpr std::size_t kGapBins = 4;

    /**
     * @brief One satellite of an epoch as the engine sees it.
     */
    struct SatState {
        std::string sat;                      ///< The satellite ("E02").
        Missing     missing{ Missing::None }; ///< The missing record, or None.
        std::string signal;                   ///< PhaseBias only: the first phase signal without a record ("L5Q").
        double      record_age_s{ -1.0 };     ///< Age of the last record of the missing kind (s); negative: none.
        bool        was_reference{ false };   ///< The satellite was its constellation's reference at the previous epoch.
    };

    /**
     * @brief One edge of the rule, for the log and the tests.
     */
    struct Event {
        EventKind   kind{ EventKind::Drop };  ///< The edge.
        std::string sat;                      ///< Drop, Readmit: the satellite ("G12").
        bool        first_sighting{ false };  ///< Drop only: the satellite had never been seen with its records.
        Missing     missing{ Missing::None }; ///< Drop only: the record whose absence dropped it.
        std::string signal;                   ///< Drop for a missing phase bias only: the signal without a record.
        DropCause   cause{ DropCause::FirstSighting };  ///< Drop only: how it began.
        double      record_age_s{ -1.0 };     ///< Drop only: age of the last record of the missing kind; negative: none.
        bool        was_reference{ false };   ///< Drop only: the satellite was its constellation's reference.
        double      gap_s{ -1.0 };            ///< Readmit only: time since the Drop (s); negative when no epoch time was given.
        std::size_t kept{ 0 };                ///< NoSolutionBegin, NoSolutionEnd: satellites that kept their records.
    };

    /**
     * @brief Run counters (the run-end line).
     */
    struct Counters {
        std::size_t epochs{ 0 };               ///< Epochs the gate decided.
        std::size_t drops{ 0 };                ///< Drop events.
        std::size_t drops_first_sighting{ 0 }; ///< Of them, satellites never seen with their records.
        std::size_t drops_no_phase_bias{ 0 };  ///< Of them, dropped for a missing phase-bias record.
        std::size_t readmits{ 0 };             ///< Readmit events.
        std::size_t sat_epochs_out{ 0 };       ///< Satellite-epochs left out.
        std::size_t sat_epochs_out_no_phase_bias{ 0 };  ///< Of them, left out for a missing phase-bias record.
        std::size_t sat_epochs_seen{ 0 };      ///< Satellite-epochs decided (left out or kept).
        std::size_t drops_record_lost{ 0 };    ///< Drops whose record stopped while the satellite was in view.
        std::size_t drops_back_on_stale{ 0 };  ///< Drops of a satellite back in view on a record that is too old.
        std::size_t references_lost{ 0 };      ///< Drops of a satellite that was its constellation's reference.
        std::array<std::size_t, kAgeBins> drops_by_age{};   ///< Drops by record age: none, <=60 s, <=5 min, <=30 min, longer.
        std::array<std::size_t, kGapBins> readmits_by_gap{};///< Readmits by gap: <=60 s, <=5 min, <=30 min, longer.
        std::size_t epochs_no_solution{ 0 };   ///< Epochs with fewer satellites than the floor keeping their records.
        std::size_t no_solution_spells{ 0 };   ///< Runs of such epochs (NoSolutionBegin events).
    };

    /**
     * @brief Decide one epoch.
     *
     * @param sats      Each modelled satellite of the epoch with its missing record, if any (a
     *                  satellite listed twice is decided once, by its first entry).
     * @param[out] out  The satellites to leave out of this epoch's solution (cleared first); every
     *                  satellite of @p sats on an epoch below the floor.
     * @param t_s       The epoch's time in continuous seconds, for the gap of a Readmit; NaN (the
     *                  default) leaves the gaps unknown.
     * @param min_kept  The solvable floor: when fewer satellites keep their records, the epoch has no
     *                  corrected solution; 0 (the default) disables the floor.
     * @return The edges of this epoch: the Drops and Readmits in the order of @p sats, then a
     *         NoSolutionBegin or NoSolutionEnd.
     */
    std::vector<Event> step(const std::vector<SatState>& sats, std::set<std::string>& out,
                            double t_s = std::numeric_limits<double>::quiet_NaN(),
                            std::size_t min_kept = 0);

    /**
     * @brief Decide one epoch of the clock rule: each satellite with whether the stream holds a
     *        clock record for it (false is a missing clock).  Equivalent to the @ref SatState form
     *        with no epoch time and no floor.
     *
     * @param sats      Each modelled satellite of the epoch with whether it has a clock record.
     * @param[out] out  The satellites to leave out of this epoch's solution (cleared first).
     * @return The edges of this epoch, in the order of @p sats.
     */
    std::vector<Event> step(const std::vector<std::pair<std::string, bool>>& sats,
                            std::set<std::string>& out);

    /// @brief The run counters.
    /// @return The counters so far.
    const Counters& counters() const { return counters_; }

    /// @brief The satellites currently left out (dropped and not readmitted).
    /// @return The dropped set.
    const std::set<std::string>& dropped() const { return dropped_; }

    /// @brief Whether the last decided epoch was below the floor (no corrected solution).
    /// @return True when every satellite of the last epoch was left out for the floor.
    bool noSolution() const { return no_solution_; }

    /// @brief The epoch time of each dropped satellite's Drop (continuous seconds; NaN when unknown).
    /// @return Satellite to Drop time, one entry per dropped satellite.
    const std::map<std::string, double>& dropTimes() const { return drop_time_; }

    /// @brief The satellites seen with their records at least once.
    /// @return The set (a later loss of a record is no first sighting).
    const std::set<std::string>& everRecorded() const { return ever_recorded_; }

    /**
     * @brief Restore the cross-epoch memory a warm resume carries (the RT state snapshot's
     *        @c U records).  The counters, the previous epoch's set and the floor's open edge are
     *        not carried: the first epoch after the restore decides them again.
     *
     * @param drop_times     Each dropped satellite with its Drop time (NaN when unknown).
     * @param ever_recorded  The satellites seen with their records at least once.
     */
    void restore(const std::map<std::string, double>& drop_times,
                 const std::set<std::string>& ever_recorded);

    /// @brief The age bin of a record age: 0 none (negative), 1 up to 60 s, 2 up to 5 min,
    ///        3 up to 30 min, 4 longer.
    /// @param age_s Record age in seconds; negative means no record.
    /// @return The bin index, below @ref kAgeBins.
    static std::size_t ageBin(double age_s);

    /// @brief The gap bin of a Readmit: 0 up to 60 s, 1 up to 5 min, 2 up to 30 min, 3 longer
    ///        (an unknown gap counts as longer).
    /// @param gap_s Gap in seconds; negative or NaN means unknown.
    /// @return The bin index, below @ref kGapBins.
    static std::size_t gapBin(double gap_s);

    /// @brief The name of a Drop cause for the log ("first sighting", "record lost",
    ///        "back on a stale record").
    /// @param c The cause.
    /// @return A static string.
    static const char* causeName(DropCause c);

private:
    std::set<std::string> dropped_;      ///< Left out, awaiting their records.
    std::map<std::string, double> drop_time_; ///< Drop time of each dropped satellite (NaN when unknown).
    std::set<std::string> ever_recorded_;///< Seen with their records at least once (a later loss is no first sighting).
    std::set<std::string> seen_prev_;    ///< Satellites of the previous decided epoch (in view, left out or kept).
    bool                  no_solution_{ false }; ///< The last decided epoch was below the floor.
    bool                  no_solution_open_{ false }; ///< A run of epochs below the floor is open.
    Counters              counters_;     ///< Run counters.
};

} // namespace GnssProcessor
