#pragma once
/**
 * @file RingFixHold.hpp
 * @brief The ring fix's hold epoch before PASS B as named stages: the chain's cuts, the held
 *        links through them, the links that entered the tree, the pinned root handovers, the
 *        release step, the cascades' releases, the old hosts, the targets for the next epoch and
 *        the hold table PASS B folds.
 *
 * networkRingFixHoldEpoch (network/ar/RingFixSeams.cpp, declared in network/ar/RingFix.hpp) gates
 * on the ring fix's state and calls the stages in this order:
 *
 *   stage                               what it does
 *   networkRingFixHoldTruth             the truth's arcs of this epoch (simulation only)
 *   networkRingFixHoldCuts              1. the cuts of the chain record's releases, each with whether its
 *                                          entry's held integer is the known step
 *   networkRingFixHoldThroughCuts       2. the near-null guard's moved chords re-opened, then the held links
 *                                          through the cuts: re-expressed by a known step, released across an
 *                                          unknown one; the held-first re-attachment rows
 *   networkRingFixHoldToTree            3. a held link that became a tree edge leaves the held set
 *   networkRingFixHoldRoots             4. a held link kept through a root handover the root row pinned
 *   networkRingFixHoldReleases          5. the release step
 *   networkRingFixHoldCascadeReleases   6. the cascades' releases
 *   networkRingFixHoldInherit           7. the old hosts: last epoch's tree edges that are chords now
 *   networkRingFixHoldTargets           8. this epoch's targets, for the next epoch's old hosts
 *   networkRingFixHoldTable             9. the hold table PASS B folds
 *
 * The order is load-bearing:
 *   - The cuts (1) come before every stage that reads them (2, 6, 7).
 *   - The near-null re-opens come before the cuts apply (2): a re-open sets a link's re-base flag,
 *     which a cut release clears.
 *   - The cuts apply (2) before the tree edges leave the held set (3), and both before the release
 *     step (5): the release step's two chain inputs (a tree edge now, a vertex on a cut's moved
 *     side) are then false, and its chain cause is a root change alone.
 *   - A pinned root handover (4) moves the link's root before the release step (5) compares it.
 *   - The release step (5) comes before the cascades' releases (6): a cascade link is released with
 *     its pair.
 *   - The old hosts (7) read the previous epoch's targets, which the targets stage (8) replaces.
 *   - The hold table (9) is last: it holds every link the earlier stages left held.
 *
 * Every stage is called by networkRingFixHoldEpoch after its gate, with the ring fix armed
 * (run.ctx.carry.ring_fix is not null); each binds the ring fix's state in its opening block.
 */

#include "network/ar/RingFix.hpp"   // NetworkRingFix, RingFixCut, RingFixLinkId; net::ArcTuple

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;      // network/driver/Run.hpp
struct NetworkEpoch;    // network/driver/Epoch.hpp
struct ChainEpochObs;   // network/solve/ChainFrame.hpp

/// What the hold epoch's stages read from the chain record: its tree arcs, chords, roots and depths.
struct RingFixChainFacts {
    std::set<net::ArcTuple> tree;    ///< Tree edges (station, satellite, signal).
    std::set<net::ArcTuple> chords;  ///< Chords (station, satellite, signal).
    /// group -> vertex key -> its component's root receiver.
    std::map<std::string, std::map<std::string, std::string>> root_of;
    /// group -> vertex key -> its depth below its component's root.
    std::map<std::string, std::map<std::string, int>> depth_of;
    /// @brief A link's ring in a group (the larger depth of its two vertices; -1 when unknown).
    /// @param g The group. @param r The receiver key. @param v The satellite key. @return The ring.
    int ring(const std::string& g, const std::string& r, const std::string& v) const {
        const auto gi = depth_of.find(g);
        if (gi == depth_of.end()) return -1;
        const auto a = gi->second.find(r), b = gi->second.find(v);
        if (a == gi->second.end() || b == gi->second.end()) return -1;
        return std::max(a->second, b->second);
    }
    /// @brief The root of a vertex's component in a group ("" when unknown).
    /// @param g The group. @param v The vertex key. @return The root receiver's name.
    std::string root(const std::string& g, const std::string& v) const {
        const auto gi = root_of.find(g);
        if (gi == root_of.end()) return {};
        const auto vi = gi->second.find(v);
        return vi == gi->second.end() ? std::string{} : vi->second;
    }
};

/**
 * @brief The facts of a chain record: every tree edge with its root and depth, every chord.
 * @param obs The epoch's chain record; null or not armed gives empty facts.
 * @return The record's tree arcs, chords, roots and depths by group.
 */
RingFixChainFacts ringFixChainFacts(const ChainEpochObs* obs);

/**
 * @brief The truth's arcs (simulation only, network.ring_fix.truth_root): every (receiver,
 *        satellite) the slip channel modelled this epoch, the rows network_slip_raw.csv writes,
 *        observed for the truth audit in each epoch's frame.
 *
 * Returns at once without the truth or without this epoch's slip evidence.
 *
 * @param run The run. Written: the ring fix's truth. Read: run.engines.slip_qc (this epoch's rows).
 * @param ep  The epoch: ep.se.time, ep.pass_a.present.
 */
void networkRingFixHoldTruth(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Stage 1: the cuts of this epoch, one per release in the chain record.
 *
 * Each cut takes its whole moved side, the released vertex's subtree in this epoch's tree on the
 * band: a release nested below another lists only its own part, and a link from the nested part to
 * outside the outer side crosses both cuts (the steps add).  The step of a moved side is known where
 * its released edge was a usable held link; whether that held integer is the known step is
 * @ref chainEntryStep's (with network.ring_fix.entry_revalidate the entry's gap re-entry and its
 * near-null signature are asked too, and each refused entry is reported once per epoch).
 *
 * @param run     The run. Written: the ring fix's counters (nested cuts, entry checks), run.traces
 *                (RINGFIX entry_refused events). Read: run.setup.stations, run.engines.slip_qc (the gap
 *                resumptions).
 * @param ep      The epoch: ep.se.time, ep.events.phase_datum.chain.
 * @param latched This epoch's latched arcs (@ref ringFixLatchedArcs).
 * @return This epoch's cuts in the record's release order; empty without an armed record.
 */
std::vector<RingFixCut> networkRingFixHoldCuts(NetworkRun& run, const NetworkEpoch& ep,
                                               const std::set<net::ArcTuple>& latched);

/**
 * @brief Stage 2: the held links through the cuts.
 *
 * First the chords the near-null guard moved after the last solve (network.ring_fix.nn_guard) have
 * their carried entries re-opened as at a release, so that the hold rows at their new integers
 * re-level them, and their re-base flag set.  Then @ref ringFixApplyCuts re-expresses the held links
 * crossing a known cut and releases those crossing an unknown one; each released link's carried
 * entries are re-opened and an event names it.  Last, one re-attachment row per entry the walk took
 * through a held link first (network.ring_fix.held_first), naming the link and the held links whose
 * integers crossed its cuts unchanged.
 *
 * @param run     The run. Written: the ring fix's links, re-opens and counters, the released links'
 *                carried entries (@ref networkRingFixReopenCarry), run.traces (RINGFIX release,
 *                reexpress and reattach events). Read: run.setup.stations.
 * @param ep      The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param latched This epoch's latched arcs.
 * @param cuts    This epoch's cuts (stage 1).
 */
void networkRingFixHoldThroughCuts(NetworkRun& run, const NetworkEpoch& ep, const std::set<net::ArcTuple>& latched,
                                   const std::vector<RingFixCut>& cuts);

/**
 * @brief Stage 3: a held link that became a tree edge leaves the held set (the chain pins it).
 *
 * With the settled flags (network.ring_fix.settled_flags) the edge is certified for the settled set
 * while it stays in the tree.
 *
 * @param run   The run. Written: the ring fix's links, certified edges and counters, run.traces
 *              (RINGFIX to_tree events). Read: run.setup.stations.
 * @param ep    The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param facts The chain record's facts (its tree arcs).
 */
void networkRingFixHoldToTree(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts);

/**
 * @brief Stage 4: a held link whose component's root changed in a handover the root row pinned at
 *        the carried receiver phase bias keeps the frame: its remembered root becomes the new one.
 *
 * Returns at once without an armed record or without root handovers in it.
 *
 * @param run   The run. Written: the ring fix's links (their roots) and counters.
 * @param ep    The epoch: ep.events.phase_datum.chain (its root handovers).
 * @param facts The chain record's facts (the roots by vertex).
 */
void networkRingFixHoldRoots(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts);

/**
 * @brief Stage 5: the release step (@ref ringFixReleaseStep) over every held link's reading.
 *
 * Every cause re-opens the released link's carried entries: a slip's and a refloat's drop already
 * ran (the floor still binds where it left the entry tighter); the other causes re-open them here,
 * unless something re-levelled them since the decision.
 *
 * @param run     The run. Written: the ring fix's links and counters, the released links' carried
 *                entries, run.traces (RINGFIX release events). Read: run.setup.stations, the arcs'
 *                carried entries.
 * @param ep      The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param facts   The chain record's facts (chords and roots).
 * @param latched This epoch's latched arcs.
 */
void networkRingFixHoldReleases(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts,
                                const std::set<net::ArcTuple>& latched);

/**
 * @brief Stage 6: the cascades' releases (network.ring_fix.cascade).
 *
 * A held cascade link is released with its pair, at a slip of its third signal, by its monitor, after
 * a gap, or across a cut of the third band (the first cause that applies); each release re-opens the
 * third signal's carried entry.
 *
 * @param run     The run. Written: the ring fix's cascade links and cascade counters, the third
 *                signals' carried entries, run.traces (RINGFIX cascade_release events). Read:
 *                run.setup.stations.
 * @param ep      The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param latched This epoch's latched arcs.
 * @param cuts    This epoch's cuts (stage 1).
 */
void networkRingFixHoldCascadeReleases(NetworkRun& run, const NetworkEpoch& ep,
                                       const std::set<net::ArcTuple>& latched, const std::vector<RingFixCut>& cuts);

/**
 * @brief Stage 7: the old hosts, last epoch's tree edges that are chords of their pair family now,
 *        held at their previous targets moved by the known steps of this epoch's cuts
 *        (@ref ringFixInherit).
 *
 * Returns at once without previous targets or without an armed record.
 *
 * @param run     The run. Written: the ring fix's links and counters, run.traces (RINGFIX hold
 *                events with the action inherit). Read: run.setup.stations.
 * @param ep      The epoch: ep.se.time, ep.clock.pl_now_s, ep.events.phase_datum.chain.
 * @param facts   The chain record's facts (tree arcs, chords, rings and roots).
 * @param latched This epoch's latched arcs.
 * @param cuts    This epoch's cuts (stage 1).
 */
void networkRingFixHoldInherit(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts,
                               const std::set<net::ArcTuple>& latched, const std::vector<RingFixCut>& cuts);

/**
 * @brief Stage 8: this epoch's tree-edge targets (cycles), for the next epoch's old hosts.
 *
 * Returns at once without an armed record (the previous targets are then kept).
 *
 * @param run The run. Written: the ring fix's previous targets.
 * @param ep  The epoch: ep.events.phase_datum.chain (its edges).
 */
void networkRingFixHoldTargets(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Stage 9: the hold table PASS B folds, emptied and rebuilt: lambda times the integer of
 *        both bands of every held link and of the third band of every held cascade link.
 *
 * Reads no epoch group.
 *
 * @param run The run. Written: the ring fix's hold table.
 */
void networkRingFixHoldTable(NetworkRun& run);

/**
 * @brief Re-opens the carried entry of one signal of a released link.
 *
 * The entry is re-opened when nothing re-levelled it since @p stamp, and also, on the float vehicle,
 * when what did (the vehicle's drop at a slip or a refloat) left it tighter than the release floor
 * (@ref kRingFixReleaseFloorM2): an entry born under the hold (held from its first stash, or
 * re-created after a cut erased it) has the hold's own variance as its birth variance, which a drop
 * restores.  On the float vehicle the entry takes the vehicle's bounded drop to at least the release
 * floor; on the leveling vehicle a stale entry is erased.  Called by stages 2, 5 and 6.
 *
 * @param run   The run. Written: the link's carried entry, the ring fix's re-open counters, the carry-state
 *              trace (network.trace_amb_state). Read: the float vehicle's knobs, run.config (the code
 *              elevation power), run.setup.stations, the loop epoch count.
 * @param ep    The epoch: ep.se.time, ep.clock.pl_now_s.
 * @param id    The link.
 * @param sig   The link's signal whose entry is re-opened.
 * @param stamp The entry's re-level stamp at the link's decision (+infinity: re-open in any case).
 */
void networkRingFixReopenCarry(NetworkRun& run, const NetworkEpoch& ep, const RingFixLinkId& id,
                               const std::string& sig, double stamp);

} // namespace GnssProcessor
