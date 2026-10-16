#pragma once
/**
 * @file NetworkPhaseAnchorEpoch.hpp
 * @brief The phase datum of one network epoch: the shared phase-bias set, the station that
 *        anchors each (sat,signal), and the eq34 receiver phase-bias allocation (card NOM3
 *        step EP2).
 *
 * The bodies are the frozen driver text (main 7141240), moved by scripts/nom_ep2_cut.py and
 * checked by scripts/nom_ep2_verify.py.  The algorithms they call are elsewhere: the candidate
 * census and the elections in filter/NetworkPhaseDatum, the reattach hygiene in
 * filter/NetworkAnchorHygiene, the chain datum in filter/NetworkChainDatum.
 *
 * Position (Application::networkProcessEpoch_): after the PASS-A slip drains and the two
 * admission screens (the funnel is complete, and their withholds have pruned the states an
 * election could otherwise pin), and before the level-authority census, the column
 * registration, est.beginEpoch and PASS B.
 */

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct EpochEvents;             // network/driver/NetworkEpoch.hpp
struct EpochPassA;

/**
 * @brief The final shared phase-bias set and the anchor election.
 *
 * Legacy (network.phase_datum_prioritized off): the pivot's phase-bias states are the shared
 * set.  Prioritized: the candidate census and coverage gate (networkBuildPhaseDatumCandidates),
 * the eq34 reattach hygiene, then either the chain datum (network.phase_datum_eq34) or the
 * two-edge continuous handover election (anchor-slip handover, cold fallback candidacy, the
 * held S-basis, the incumbent swap and its carry re-reference).  Writes events (PHASE_ANCHOR,
 * INJECT, PBGAUGE, STRANSFORM) and the phase-datum counters.
 *
 * @param run    The run: the stations' phase-bias states, ctx.carry (anchors, incumbents, the
 *               re-referenced set, the held basis), ctx.counters, the traces.
 * @param ep     The epoch, read: se, funnel.carry_slipped_ambs.
 * @param pass_a PASS A's layout: reads modeled and present, writes shared_phase_keys.
 * @param events The epoch's events; written: phase_datum.
 */
void networkPhaseAnchorEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a, EpochEvents& events);

/**
 * @brief The eq34 receiver phase-bias allocation (card 3, rule D3c).
 *
 * A present station carries the per-(receiver,system,signal) split state if and only if it
 * carries an ungated shared phase-bias column on that signal; the set is re-derived each epoch
 * (allocate what is needed, prune the rest), before est.reset() and PASS B.  Inert (no
 * allocation) unless network.phase_datum_eq34.
 *
 * @param run The run: the stations' StateManagers (written).
 * @param ep  The epoch, read: pass_a.present, pass_a.shared_phase_keys.
 */
void networkReceiverPhaseBiasAllocation(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
