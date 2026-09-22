/**
 * @file NetworkSecondPhaseScope.hpp
 * @brief The SECOND PHASE CHANNEL primitives of card SID phase 6a
 *        (docs/refound/SID_signal_identity_2026-09-22.md section 4).
 *
 * One carrier phase per station and band means a satellite's L1W phase bias is
 * supported only by the stations that latched L1W, so the L1C and L1W satellite
 * families compete for the same roster instead of sharing it: a median of 4
 * carried stations per L1W column-epoch and 23.6 per cent of satellite-epochs
 * with a clock record and no L1W phase bias.  With
 * `network.second_phase_channel` a station contributes, as ordinary
 * float-ambiguity PHASE rows, every allow-listed band-1 phase it reports besides
 * its latched canonical one.
 *
 * This is the PHASE twin of card SCC and the same three decisions are pure
 * functions of their inputs, so they live here and are unit-tested directly
 * (tests/test_second_phase_channel.cpp):
 *
 *  - @ref secondPhaseAllowEntryValid — what the loader accepts in
 *    `network.second_phase_signals` (a 3-character phase type on band 1; band 2
 *    belongs to phase 6b, where the `processing.channel_mode` interlock is lifted
 *    instead of worked around);
 *  - @ref secondPhaseCompanions — which phases a station contributes for one
 *    satellite, given what it reports, what its latch already selected, and the
 *    allow-list;
 *  - @ref secondPhaseCellEdges — the two edges of standing invariant #7 on the
 *    (station, satellite, signal) cells the mechanism opens and closes.
 *
 * ONE DELIBERATE OMISSION.  Card SCC needed a receiver inter-signal CODE state
 * because C1C and C1W both resolve to the per-band receiver DCB and a second code
 * would otherwise assert a zero inter-signal bias.  The phase side needs no such
 * state and must not have one: a float ambiguity absorbs an arbitrary constant
 * per arc, so a receiver inter-signal PHASE state would sit in the null space of
 * the normal equations (the audit's risk 5).
 *
 * Nothing here touches global state, the product cache or the configuration; the
 * callers (@c PppModel::selectUncombinedSignals, the loader, the PASS-A serial
 * sweep and the network teardown) supply everything.
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
 * Pure.  Accepts exactly a 3-character RINEX observation code of type 'L' whose
 * band digit is '1'.  Everything else is refused rather than clamped: a code type
 * belongs to card SCC, and a band-2 entry belongs to phase 6b (this phase keeps
 * the bounded band-1 scope its bars were frozen on).
 *
 * @param entry The configured string, e.g. "L1W".
 * @param why   Optional; on refusal, set to a one-clause reason for the log.
 * @return True when the entry may be admitted.
 */
bool secondPhaseAllowEntryValid(const std::string& entry, std::string* why);

/**
 * @brief The second phases one station contributes for one satellite on band 1.
 *
 * Pure.  A second phase is an allow-listed phase the station REPORTS for this
 * satellite that is not the phase its latch already selected for the band.  The
 * result is in ALLOW-LIST order (not the order the receiver happens to report),
 * so the added rows are deterministic across stations and across runs, and it
 * never repeats a signal.
 *
 * An empty @p selected_phase means the band has no latched phase present this
 * epoch: the caller then adds NOTHING and counts the case as a FALLBACK.  The
 * latch decides what "second" means, so a phase arriving with no canonical beside
 * it is not a second phase; promoting it would silently make it the only one, and
 * it would be seeded against a different signal's arc history.
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
 * @brief The two edges of standing invariant #7 on the open-cell set.
 *
 * Pure.  @p opened receives the cells in @p cur that were not in @p prev (the
 * RE-COUPLING edge: a satellite-station-signal cell starts contributing a second
 * phase), @p closed the cells in @p prev that are gone from @p cur (the
 * DECOUPLING edge).  Both outputs are cleared first and come back sorted, since
 * both inputs are ordered sets.  Either pointer may be null.
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
 * Pure.  Given the two band-1 phases of one line of sight in CYCLES, returns
 * their difference.  A slip on ONE channel moves it by that channel's integer; a
 * slip common to both leaves it unchanged, which is exactly what makes the
 * epoch-to-epoch change of this quantity an exact-integer slip witness.  The two
 * phases share a frequency, so no wavelength enters and the difference is already
 * in cycles of that band.
 *
 * @param latched_cyc The latched phase's raw observation, in cycles.
 * @param second_cyc  The second phase's raw observation, in cycles.
 * @return latched_cyc - second_cyc, in cycles.
 */
double secondPhaseWitness(double latched_cyc, double second_cyc);

/**
 * @brief Card SID phase 6a (R7) — the epoch tail: the slip census by tier, the
 *        anchor census and the witness advance.
 *
 * Runs LATE in the epoch, after every slip tier has written its freeing cause and
 * after the anchor election, because those are the inputs.  Reads only; every
 * class is a counter.  Returns immediately when the knob is down, so an unarmed
 * run pays one flag test.
 *
 * @param ctx      The run context (the carry, the counters, the cells).
 * @param config_  The configuration (the knob).
 * @param stations The producer's stations, for the names and the cell sets.
 */
void networkSecondPhaseEpochTail(NetworkRunContext& ctx, const GnssConfig& config_,
                                 const std::vector<NetStation>& stations);

}  // namespace GnssProcessor

#endif  // GNSS_PROCESSOR_NETWORK_SECOND_PHASE_SCOPE_HPP
