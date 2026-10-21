/**
 * @file SecondPhaseScope.hpp
 * @brief Primitives of the second phase channel.
 *
 * With one carrier phase per station and band, a satellite's L1W phase bias is
 * supported only by the stations that latched L1W, so the L1C and L1W satellite
 * families split the station roster between them instead of sharing it.  With
 * `network.second_phase_channel` a station contributes, as ordinary
 * float-ambiguity phase rows, every allow-listed band-1 phase it reports besides
 * its latched canonical one.
 *
 * This is the phase counterpart of the second code channel.  The decisions below are pure
 * functions of their inputs and are unit-tested directly
 * (tests/network/test_second_phase_channel.cpp):
 *
 *  - @ref secondPhaseAllowEntryValid: what the loader accepts in
 *    `network.second_phase_signals` (a 3-character phase type on band 1; band-2
 *    phases come in through `processing.channel_mode: all` instead);
 *  - @ref secondPhaseCompanions: which phases a station contributes for one
 *    satellite, given what it reports, what its latch already selected, and the
 *    allow-list;
 *  - @ref secondPhaseCellEdges: the opening and closing edges of the
 *    (station, satellite, signal) cells the mechanism opens and closes;
 *  - @ref secondPhaseWitness: the per-line-of-sight slip witness.
 *
 * No receiver inter-signal phase state is added, by design.  The second code channel needs a
 * receiver inter-signal code state because C1C and C1W both resolve to the
 * per-band receiver DCB, and a second code without it would assert a zero
 * inter-signal bias.  On the phase side a float ambiguity absorbs any constant
 * per arc, so a receiver inter-signal phase state would lie in the null space of
 * the normal equations.
 *
 * Nothing here touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, the PASS A slip scan
 * in network/slip/PassASlipScan.cpp, and the epoch observer table) supply
 * everything.
 */
#ifndef GNSS_PROCESSOR_NETWORK_SECOND_PHASE_SCOPE_HPP
#define GNSS_PROCESSOR_NETWORK_SECOND_PHASE_SCOPE_HPP

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetStation;

/**
 * @brief Is @p entry a legal `network.second_phase_signals` entry?
 *
 * Pure.  Accepts only a 3-character RINEX observation code of type 'L' whose
 * band digit is '1'.  Anything else is refused, not clamped: a code type belongs
 * to the second code channel, and a band-2 entry is left to `processing.channel_mode: all`.
 *
 * @param entry The configured string, e.g. "L1W".
 * @param why   Optional; on refusal, set to a one-clause reason for the log.
 * @return True when the entry may be admitted.
 */
bool secondPhaseAllowEntryValid(const std::string& entry, std::string* why);

/**
 * @brief The second phases one station contributes for one satellite on band 1.
 *
 * Pure.  A second phase is an allow-listed phase the station reports for this
 * satellite that is not the phase its latch already selected for the band.  The
 * result is in allow-list order (not the receiver's reporting order), so the
 * added rows are deterministic across stations and runs, and it contains no
 * signal twice.
 *
 * An empty @p selected_phase means the band has no latched phase present this
 * epoch; the result is then empty and the caller counts the case as a fallback.
 * The latch defines "second": a phase arriving with no canonical phase beside it
 * is not added, because it would become the only phase of the band and would be
 * seeded against a different signal's arc history.
 *
 * @param reported_phases Phases this station reports for this satellite this
 *                        epoch (already filtered by the caller's coverage
 *                        allow-list).
 * @param selected_phase  The phase the band map already selected, or empty.
 * @param allow_list      `network.second_phase_signals`, in configured order.
 * @return The second phases, in allow-list order, without duplicates.
 */
std::vector<std::string> secondPhaseCompanions(
    const std::set<std::string>& reported_phases,
    const std::string& selected_phase,
    const std::vector<std::string>& allow_list);

/**
 * @brief The opening and closing edges of the open-cell set.
 *
 * Pure.  @p opened receives the cells in @p cur that were not in @p prev (the
 * re-coupling edge: a station-satellite-signal cell starts contributing a second
 * phase); @p closed receives the cells in @p prev that are absent from @p cur
 * (the decoupling edge).  Both outputs are cleared first and come back sorted,
 * since both inputs are ordered sets.  Either pointer may be null.
 *
 * @param prev   Open cells of the previous epoch.
 * @param cur    Open cells of this epoch.
 * @param opened Out; cells that opened.  May be null.
 * @param closed Out; cells that closed.  May be null.
 */
void secondPhaseCellEdges(const std::set<std::string>& prev,
                          const std::set<std::string>& cur,
                          std::vector<std::string>* opened,
                          std::vector<std::string>* closed);

/**
 * @brief The witness statistic on one (station, satellite) band-1 phase pair.
 *
 * Pure.  Given the two band-1 phases of one line of sight in cycles, returns
 * their difference.  A slip on one channel moves it by that channel's integer; a
 * slip common to both leaves it unchanged, so its epoch-to-epoch change is an
 * exact-integer slip witness.  The two phases share a frequency, so no wavelength
 * enters and the difference is in cycles of that band.
 *
 * @param latched_cyc The latched phase's raw observation, in cycles.
 * @param second_cyc  The second phase's raw observation, in cycles.
 * @return latched_cyc - second_cyc, in cycles.
 */
double secondPhaseWitness(double latched_cyc, double second_cyc);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Epoch tail of the second phase channel: the slip census by tier and the
 *        anchor census.
 *
 * Runs late in the epoch, after every slip tier has written its freeing cause and
 * after the anchor election, because those are its inputs.  Read-only; every
 * class is a counter.  Returns on its first line unless
 * @c GnssConfig::networkSecondPhaseRowsActive (the `network.second_phase_channel`
 * knob or `processing.channel_mode: all`), so an unarmed run pays one flag test.
 * The witness itself is advanced in the PASS A slip scan, not here.
 *
 * @param run The run (network/driver/Run.hpp). Written: run.ctx (the carry, the
 *            counters, the cells).  Read: run.config (the knob), run.setup.stations
 *            (the names and the cell sets).
 * @param ep The epoch (network/driver/Epoch.hpp), read only.
 */
void networkSecondPhaseEpochTail(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the second phase channel.
 *
 * The second phase channel: second-phase cells, rows and the
 * families they opened, with the rows summed from the stations into a local copy of the
 * tallies.
 *
 * Gate: returns 0 at once unless `config_.networkSecondPhaseRowsActive()`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSecondPhaseScopeRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of SecondPhaseScope, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SecondPhaseScopeCounters {
    /// Run-end telemetry of the second phase channel, including both edges of the
    /// open-cell set.  The cell classes and the tier census are reduced in serial
    /// sweeps; @c rows is summed from the per-station NetStation counters at teardown,
    /// as for the second code.
    ///
    /// @c rows == 0 and @c cells == 0 with the knob armed means the run exercised
    /// nothing (no station reported a second allow-listed band-1 phase), and the
    /// run-end report warns instead of reporting a clean run.
    struct SecondPhase {
        long long cells = 0;          ///< Second-phase (station, satellite, signal) cells open, summed over epochs.
        long long cell_open = 0;      ///< Cell openings (the re-coupling edge): a cell that was not open last epoch.
        long long cell_close = 0;     ///< Cell closings (the decoupling edge): a cell that was open last epoch and is gone.
        long long rows = 0;           ///< Second-phase rows folded into the solve, over the run.
        /// The fallback case: an allow-listed band-1 phase this station reports for a
        /// satellite with no latched phase of that band beside it.  Those rows are not
        /// added (the latch decides what "second" means); the count is a finding about
        /// the canonical latch, not about the second phase channel.
        long long fallback_rows = 0;
        long long arcs_born = 0;      ///< Distinct (station, satellite, signal) cells the run ever opened.
        long long arcs_live_max = 0;  ///< Cells open network-wide, maximum over epochs.
        long long stations_max = 0;   ///< Stations contributing at least one second phase, maximum over epochs.
        /// Phase-datum anchor incumbencies held on a second-phase arc, summed over
        /// epochs.  A second-phase arc is a candidate like any other, and this says
        /// whether the election ever seats one.
        long long anchors = 0;
        /// (satellite, signal) columns that reached the >= 2-station registration bar
        /// only through second-phase arcs.  Per epoch, summed.  It measures the
        /// premise directly: zero of these means the channel added rows without ever
        /// widening a family's roster.
        long long cols_via_second = 0;
        // --- Slip census on second-phase arcs, by tier ---
        // Keyed on RelevelEpochState::kCause*, the producer's own tier vocabulary, so
        // these cannot drift from what the tiers did.  A tier that reads zero here
        // while its arcs exist does not screen the second-phase rows.
        long long slip_arcs = 0;      ///< Second-phase arcs freed by any tier, summed over epochs.
        long long slip_mw = 0;        ///< ... of those, with the MW wide-lane bit.
        long long slip_lli = 0;       ///< ... with the receiver LLI bit.
        long long slip_iftd = 0;      ///< ... with the IF-phase time-difference bit.
        long long slip_t4 = 0;        ///< ... with the RAIM prominence-attribution (T4) bit.
        long long slip_t3 = 0;        ///< ... with the RAIM per-satellite consensus (T3) bit.
        long long slip_windup = 0;    ///< ... with the wind-up restart bit.
        long long slip_gap = 0;       ///< ... with the gap re-entry bit (either gap tier).
        // --- The between-channel witness (observatory: counts, no action) ---
        long long witness_pairs = 0;  ///< (station, satellite) pair-epochs with both band-1 phases and a previous epoch.
        long long witness_jumps = 0;  ///< ... of those, whose epoch-to-epoch change exceeded 0.5 cycles.
        double    witness_max = 0.0;  ///< The largest absolute change seen, in cycles.
        std::string witness_max_id;   ///< "<station>|<satellite>|<signal>@<tow>" of that change.
    };
    SecondPhase second_phase;
};

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_PHASE_SCOPE_HPP
