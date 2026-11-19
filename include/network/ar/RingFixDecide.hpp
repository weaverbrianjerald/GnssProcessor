#pragma once
/**
 * @file RingFixDecide.hpp
 * @brief The ring fix's decision after the solve as named stages: the election's outcomes, the
 *        chain of this epoch, the chord readings, the audit, the decisions, the cascades, the
 *        trace rows, the settled flags and the near-null guard.
 *
 * networkRingFixDecide (network/ar/RingFixSeams.cpp, declared in network/ar/RingFix.hpp) gates on
 * the ring fix's state and an armed chain record, and calls the stages in this order:
 *
 *   stage                           what it does                                          hands on
 *   networkRingFixDecideReports     the election's outcomes of last epoch's re-attachment
 *                                   moves and refused keeps, as events
 *   networkRingFixDecideChain       the chain this epoch: tree arcs, groups, roots,       RingFixDecideChain
 *                                   chords, rings and the latch
 *   networkRingFixDecideReadings    every pair-family chord with its readings              RingFixDecideReadings
 *   networkRingFixDecideAudit       the audit of every held link                          RingFixDecideAudit
 *   networkRingFixDecideLiveHolds   the live record's note of the links and held links
 *   networkRingFixDecideLinks       the decisions and their hold events                   the decision events
 *   networkRingFixDecideCascades    the cascades' readings, decisions and tree edges      RingFixDecideCascades
 *   networkRingFixDecideTrace       network_ring_fix.csv: one row per chord
 *   networkRingFixDecideSettled     the settled flags and the re-attachment's requests
 *   networkRingFixDecideNearNull    the near-null guard
 *
 * The order is load-bearing:
 *   - The election's outcomes are reported first: they close the moves the last epoch requested
 *     (an applied move joins the watch, an unreached one is counted missed), and this epoch's
 *     settled flags read the watch and request the next moves.
 *   - The audit reads which links are held before the decisions, and the cascades read that set
 *     (a cascade link is decided only on a pair held in this epoch's solve).
 *   - The live record's note follows the audit and precedes the decisions: it counts the links
 *     held in this epoch's solve, the set the audit read.
 *   - The trace rows follow the decisions and the cascades, whose outcomes they write.
 *   - The settled flags follow the decisions (a link decided at this epoch counts in this epoch's
 *     sets) and the cascades (the cascade families' sets read the cascade links read now).
 *   - The near-null guard is last: it moves held integers that the trace rows and the settled
 *     flags of this epoch read unmoved, and its moves and releases act on the next epoch.
 *
 * Every stage is called by networkRingFixDecide after its gate, with the ring fix armed
 * (run.ctx.carry.ring_fix is not null) and the chain record armed
 * (ep.events.phase_datum.chain is not null); each binds what it reads in its opening block.
 */

#include "network/ar/RingFix.hpp"   // NetworkRingFix, RingFixDecideObs, RingFixGate, RingFixEvent; net::ArcTuple

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;        // network/driver/Run.hpp
struct NetworkEpoch;      // network/driver/Epoch.hpp
struct ChainChordCycle;   // network/solve/ChainFrame.hpp

/// The chain of this epoch as the decision reads it (networkRingFixDecideChain).
struct RingFixDecideChain {
    std::set<net::ArcTuple> tree_arcs;    ///< The record's tree edges (station, satellite, signal).
    std::set<std::string>   chain_signals;  ///< The bands with a tree, "<constellation key>|<signal>".
    /// group -> vertex key -> its component's root receiver.
    std::map<std::string, std::map<std::string, std::string>> root_of;
    /// The record's chords (station, satellite, signal) -> their fundamental cycle in the record.
    std::map<net::ArcTuple, const ChainChordCycle*> cyc_of;
    /// group -> (receiver key, satellite key) -> the chord's ring, inner vertex and candidacy.
    std::map<std::string, std::map<std::pair<std::string, std::string>, RingFixRingInfo>> rings;
    std::set<net::ArcTuple> latched;      ///< This epoch's latched arcs (@ref ringFixLatchedArcs).
};

/// Every pair-family chord of this epoch with its readings (networkRingFixDecideReadings).
struct RingFixDecideReadings {
    std::vector<RingFixDecideObs> links;            ///< The considered chords, station by station in roster order.
    std::map<RingFixLinkId, RingFixGate> gate_of;   ///< The trace's gate reading of every chord with a float.
};

/// The audit of the links held in this epoch's solve (networkRingFixDecideAudit).
struct RingFixDecideAudit {
    std::map<RingFixLinkId, RingFixAudit> truth_of;  ///< Simulation only: each held link against truth in this epoch's frame.
    std::map<RingFixLinkId, RingFixAudit> audit_of;  ///< Each held link against the audit file (Unlisted without one).
    std::map<RingFixLinkId, bool> held_before;       ///< Every considered chord: held before this epoch's decisions.
};

/// One cascade link's row of network_ring_fix.csv (networkRingFixDecideCascades).
struct RingFixCascadeTraceRow {
    RingFixLinkId id;          ///< The cascade link (station, satellite, reference signal, third signal).
    std::string state;         ///< "held", "refused", "would_hold" or "decided".
    std::string cause;         ///< A refusal's cause ("no_float", "fraction", "odds", "persist").
    std::string audit;         ///< Simulation only: "right" or "WRONG" against truth ("" without one).
    bool have_float{false};    ///< The reference and third floats were formed.
    double f_ref{0.0};         ///< The reference signal's float (cycles).
    double f3{0.0};            ///< The third signal's float (cycles).
    RingFixCascadeGate gate;   ///< The cascade gate's reading.
    long long n_ref{0};        ///< The pair's held integer of the reference signal.
    long long n3{0};           ///< The third signal's integer (held or decided).
    bool have_n3{false};       ///< @c n3 is set.
    bool have_resid{false};    ///< A held link's post-fit phase residual of the third signal was formed.
    double resid_m{0.0};       ///< That residual (m).
    int streak{0};             ///< The link's persistence streak after this epoch.
};

/// The cascades of this epoch (networkRingFixDecideCascades).
struct RingFixDecideCascades {
    std::vector<RingFixCascadeTraceRow> cascade_trace;   ///< One trace row per cascade link read this epoch.
    std::set<RingFixLinkId> cascade_seen;                ///< The cascade links read this epoch.
};

/**
 * @brief The election's outcomes of last epoch's requests, as events: each re-attachment move it
 *        applied or missed (network.ring_fix.reattach), each keep its entry check refused.
 *
 * @param run The run. Written: the ring fix's untied moves, outcomes and refused keeps (cleared once
 *            reported), run.traces (RINGFIX untied_applied, untied_missed and keep_refused events).
 *            Read: run.setup.stations.
 * @param ep  The epoch: ep.se.time, ep.clock.pl_now_s.
 */
void networkRingFixDecideReports(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The chain this epoch: the record's tree arcs and bands, each group's tree and chords with
 *        their roots, each chord's ring and candidacy (@ref ringFixRings, the held links of the
 *        group as the held set), and the latch.  Every tree arc's (station, satellite) is stamped
 *        as a tree link now.
 *
 * @param run The run. Written: the ring fix's last tree times. Read: its links.
 * @param ep  The epoch: ep.events.phase_datum.chain, ep.events.slip.relevel, ep.clock.pl_now_s.
 * @return The chain as the later stages read it.
 */
RingFixDecideChain networkRingFixDecideChain(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Every pair-family chord with its readings, station by station: its two-band float and
 *        conditional covariance (@c chainChordFloats), its ring, root and lattice integers, its
 *        truth (simulation only), its carried entries' stamps, the latch and the code event; a held
 *        link's post-fit ionosphere-free phase residual and, with the near-null guard, the monitor or
 *        the entry re-validation armed, its code residuals.  A chord of a pair family not considered
 *        (network.ring_fix.pairs) is counted and skipped.
 *
 * @param run   The run. Written: the ring fix's counters (excluded chords, the audit file's checks).
 *              Read: run.engines.est, run.setup.stations, the arcs' carried entries, the satellites'
 *              code events, the lattice.
 * @param ep    The epoch after the solve: ep.solve (x_s, plan), ep.assembly (asm_res, est_index_of),
 *              ep.se.time, ep.clock.pl_now_s.
 * @param chain The chain this epoch.
 * @return The considered chords and their gate readings.
 */
RingFixDecideReadings networkRingFixDecideReadings(NetworkRun& run, const NetworkEpoch& ep,
                                                   const RingFixDecideChain& chain);

/**
 * @brief The audit of every link held in this epoch's solve: against truth in this epoch's frame
 *        when it is armed (simulation only) and against the audit file; each held link's flag of a
 *        wrong integer.  Also which considered chords were held before the decisions.
 *
 * Reads no epoch group.
 *
 * @param run      The run. Written: the ring fix's links (their audit flag) and audit counters. Read:
 *                 run.setup.stations.
 * @param readings This epoch's chord readings.
 * @return The audit's verdicts and the held set before the decisions.
 */
RingFixDecideAudit networkRingFixDecideAudit(NetworkRun& run, const RingFixDecideReadings& readings);

/**
 * @brief The live record's note (network.trace_live_record): every pair-family link with a float
 *        this epoch and whether it is held in this epoch's solve, counted by family, receiver and
 *        satellite.  These are the rows network_ring_fix.csv writes with a float, "held" where it
 *        writes that state.  Measure-only; one test when the record is off.
 *
 * @param run      The run. Written: run.traces.live_record (its held-links note).
 * @param ep       The epoch: ep.se.time.
 * @param readings This epoch's chord readings.
 * @param audit    The audit: its held set before the decisions.
 */
void networkRingFixDecideLiveHolds(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideReadings& readings,
                                   const RingFixDecideAudit& audit);

/**
 * @brief The decisions of this epoch (@ref ringFixDecideStep) and a RINGFIX hold (or would_hold)
 *        event per decision, with its audit and, simulation only, its truth.
 *
 * @param run      The run. Written: the ring fix's links and counters, run.traces (RINGFIX hold and
 *                 would_hold events). Read: run.setup.stations.
 * @param ep       The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param readings This epoch's chord readings.
 * @return One event per decision and per refusal, in the order of the readings.
 */
std::vector<RingFixEvent> networkRingFixDecideLinks(NetworkRun& run, const NetworkEpoch& ep,
                                                    const RingFixDecideReadings& readings);

/**
 * @brief The cascades (network.ring_fix.cascade): on every link the pair held in this epoch's
 *        solve, the third signal's integer from the reference's held integer and their float
 *        difference; the held cascade links' monitor and audit.  A held cascade link not read this
 *        epoch leaves the held set when its third band's arc is a tree edge (the chain pins it, and
 *        with the settled flags it is certified), else its absence clock runs.
 *
 * Returns empty at once without a cascade.
 *
 * @param run   The run. Written: the ring fix's cascade links, cascade counters and certified
 *              edges, run.traces (RINGFIX cascade_hold and cascade_to_tree events). Read:
 *              run.engines.est, run.setup.stations, the arcs' carried entries, the ring fix's links.
 * @param ep    The epoch after the solve: ep.solve (x_s, plan), ep.assembly (asm_res, est_index_of),
 *              ep.se.time, ep.clock.pl_now_s.
 * @param chain The chain this epoch (tree arcs, chord cycles).
 * @param audit The audit (the links held before the decisions).
 * @return The cascade links' trace rows and the set read this epoch.
 */
RingFixDecideCascades networkRingFixDecideCascades(NetworkRun& run, const NetworkEpoch& ep,
                                                   const RingFixDecideChain& chain, const RingFixDecideAudit& audit);

/**
 * @brief network_ring_fix.csv: one row per pair-family chord (its state, cause, floats, gate,
 *        integers, lattice integers, residual, streak and audit), then one per cascade link read.
 *
 * Returns at once when the trace is not open.
 *
 * @param run       The run. Written: run.traces (network_ring_fix.csv). Read: the ring fix's links,
 *                  rule and audit file, run.setup.stations.
 * @param ep        The epoch: ep.se.time.
 * @param readings  This epoch's chord readings.
 * @param audit     The audit and the held set before the decisions.
 * @param decisions This epoch's decision events.
 * @param cascades  The cascade links' trace rows.
 */
void networkRingFixDecideTrace(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideReadings& readings,
                               const RingFixDecideAudit& audit, const std::vector<RingFixEvent>& decisions,
                               const RingFixDecideCascades& cascades);

/**
 * @brief The settled flags (network.ring_fix.settled_flags), after this epoch's decisions: per held
 *        family, the satellites reached from their datum receiver through held links and tree edges
 *        that entered as held links, by the settled rule and its wait; then the cascade families'.
 *
 * A held link counts only when it is a chord this epoch (one of the readings): a held link in a
 * tracking gap carries no observation and settles nothing.  The certified edges no longer in the tree
 * are dropped first.  With the re-attachment (network.ring_fix.reattach) each pair family's untied
 * satellites are watched and moves requested for the next election.  Returns at once without the
 * settled flags.
 *
 * @param run      The run. Written: the ring fix's certified edges, settled flags, incumbent sets and
 *                 waits, settled counters, re-attachment state, run.traces (RINGFIX untied_move events).
 *                 Read: run.setup.stations.
 * @param ep       The epoch: ep.events.phase_datum.chain, ep.se.time, ep.clock.pl_now_s.
 * @param chain    The chain this epoch (its tree arcs).
 * @param readings This epoch's chord readings.
 * @param cascades The cascade links read this epoch.
 */
void networkRingFixDecideSettled(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideChain& chain,
                                 const RingFixDecideReadings& readings, const RingFixDecideCascades& cascades);

/**
 * @brief The near-null guard (network.ring_fix.nn_guard): a held Galileo E1/E5a chord whose centred
 *        code residuals sit at k near-null steps with a small ionosphere-free deviation moves by -k
 *        (3, 4) from the next epoch's hold rows (its carried entries re-open before the next PASS B);
 *        a second flag during the same hold, or the first under the release form
 *        (network.ring_fix.nn_guard_release), trips its monitor, which releases it before the next
 *        PASS B.  Returns at once without the guard.
 *
 * @param run The run. Written: the ring fix's links, re-opens and guard counters, run.traces (RINGFIX
 *            nn_move and nn_release events). Read: run.setup.stations.
 * @param ep  The epoch: ep.se.time.
 */
void networkRingFixDecideNearNull(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
