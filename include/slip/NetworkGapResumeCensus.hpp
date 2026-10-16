#pragma once
/**
 * @file NetworkGapResumeCensus.hpp
 * @brief The arc-resumption bridge census of one network epoch (card G; card NOM3 step EP2).
 *
 * Instrumentation only: nothing it writes changes a decision.  Of the arcs that came back
 * after a tracking break (SlipQcChannel::epoch.resumptions, collected by the PASS-A gap
 * instrument), it classifies each (station, satellite, signal) arc as re-entering on state
 * that bridged the break (the phase-leveling carry within network.phase_leveling_max_gap_s,
 * or a carried DD chord under network.carry_invariant), the harmful case when the raw level
 * shifted, or as a fresh float; a slip-funnel or freed arc is never bridged.  Counts land in
 * SlipQcChannel::counters (gap_{shifted,clean,unmeas}_{bridged,fresh}) and one T0_gap
 * arc_resume event per arc.
 *
 * Position: after the slip drains (the funnel is complete) and before
 * networkBuildCarryDdPriors re-keys the carry (carriedChordValue, carry_sat_vertex and
 * carry_group_id still describe the state the arc returns to).  carry_group_id is read with
 * find(), never operator[]: minting a group id here would corrupt the driver's stable keying.
 * Armed by the gap instrument or by network.cycle_slip.gap_reentry_relevel
 * (SlipQcParams::collectsResumptions), its first line.
 *
 * The body is the frozen driver text (main 7141240), moved by scripts/nom_ep2_cut.py and
 * checked by scripts/nom_ep2_verify.py.
 */

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;

/**
 * @brief Classifies this epoch's arc resumptions as bridged or fresh (measure-only).
 *
 * @param run The run: reads the slip-QC channel's resumptions and parameters, the leveling
 *            carry, the DD-carry driver's chords and keying; writes the slip-QC counters and
 *            the events.
 * @param ep  The epoch, read: se (the event time), funnel.carry_slipped_ambs, clock.pl_now_s.
 */
void networkGapResumeCensus(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
