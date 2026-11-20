/**
 * @file RingFixHold.cpp
 * @brief The ring fix's hold epoch before PASS B as named stages (see network/ar/RingFixHold.hpp
 *        for their order; networkRingFixHoldEpoch in RingFixSeams.cpp calls them).
 */

#include "network/ar/RingFixHold.hpp"

#include "config/GnssConfig.hpp"                // getCodeElevationPower (the release floor's mapping)
#include "network/ar/AmbFloat.hpp"              // ambFloatDropFloored, ambFloatBirthFloor, ambFloatDropFloorNow
#include "network/diag/AmbStateTrace.hpp"       // ambStateNoteDrop
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"         // NetworkEngines (the slip channel)
#include "network/driver/RunTypes.hpp"          // NetStation, RelevelEpochState
#include "network/slip/RawObservables.hpp"      // SlipQcChannel (the truth's arcs, the gap resumptions)
#include "network/solve/ChainChordTable.hpp"    // chainArcFamilies, chainWavelengthM
#include "network/solve/ChainFrame.hpp"         // ChainEpochObs, chainReleaseRowName
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace GnssProcessor {

namespace {

/// The held link of a (station, satellite) whose pair holds @p sig, the first in link order; null when none.
const RingFixLink* heldLinkOf(const NetworkRingFix& rf, int station, const std::string& sat, const std::string& sig) {
    for (const auto& [id, L] : rf.links)
        if (L.held && id.station == station && id.sat == sat && (id.sig1 == sig || id.sig2 == sig)) return &L;
    return nullptr;
}

/// The identity of the link whose state is at @p p; null when @p p is not one of the ring fix's links.
const RingFixLinkId* idOf(const NetworkRingFix& rf, const RingFixLink* p) {
    for (const auto& [id, L] : rf.links)
        if (&L == p) return &id;
    return nullptr;
}

/// The re-attachments through held links (network.ring_fix.held_first): one RINGFIX reattach row per released
/// edge the walk took first, naming the link and the held links whose integers crossed its cuts unchanged.
void traceHeldFirstReattachments(NetworkRingFix& rf, NetworkRunTraces& traces, const std::vector<NetStation>& stations,
                                 const GnssTime& t, const std::vector<RingFixCut>& cuts,
                                 const RingFixCutOutcome& cut_out) {
    std::map<std::pair<int, std::string>, std::vector<std::size_t>> by_entry;
    for (std::size_t k = 0; k < cuts.size(); ++k)
        if (cuts[k].held_first) by_entry[{cuts[k].entry_station, cuts[k].entry_sat}].push_back(k);
    for (const auto& [entry, ks] : by_entry) {
        std::set<RingFixLinkId> kept;
        std::string steps;
        for (std::size_t k : ks) {
            for (const RingFixLinkId& id : cut_out.kept_by_cut[k])
                if (!(id.station == entry.first && id.sat == entry.second)) kept.insert(id);
            steps += fmt::format("{}{}:{}", steps.empty() ? "" : " ", cuts[k].signal,
                                 cuts[k].known ? std::to_string(cuts[k].delta) : std::string("unknown"));
        }
        ++rf.counters.reattached;
        rf.counters.held_first_kept += static_cast<long long>(kept.size());
        const RingFixLink* h = heldLinkOf(rf, entry.first, entry.second, cuts[ks.front()].signal);
        const RingFixLinkId* hid = h != nullptr ? idOf(rf, h) : nullptr;
        traces.emitEvent(t, "RINGFIX", "reattach", entry.second, ringFixStationName(stations, entry.first),
                         std::nullopt, std::nullopt, "held_first",
                         fmt::format("sig1={} sig2={} n1={} n2={} steps={} kept={} cut={}",
                                     hid != nullptr ? hid->sig1 : std::string(),
                                     hid != nullptr ? hid->sig2 : std::string(), h != nullptr ? h->n1 : 0,
                                     h != nullptr ? h->n2 : 0, steps, kept.size(), cuts[ks.front()].row));
    }
}

}  // namespace

RingFixChainFacts ringFixChainFacts(const ChainEpochObs* obs) {
    RingFixChainFacts f;
    if (obs == nullptr || !obs->armed) return f;
    for (const ChainEdgeObs& e : obs->edges) {
        f.tree.insert({e.station, e.sat, e.signal});
        f.root_of[e.group][e.child] = e.r1;
        f.root_of[e.group][e.parent] = e.r1;
        f.depth_of[e.group][e.child] = e.depth;
        f.depth_of[e.group].emplace(e.parent, e.depth - 1);
    }
    for (const ChainChordCycle& c : obs->chords) f.chords.insert({c.station, c.sat, c.signal});
    return f;
}

void networkRingFixHoldTruth(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the truth's arcs
    const SlipQcChannel& qc = run.engines.slip_qc;
    const GnssTime& t = ep.se.time;
    const std::vector<bool>& present = ep.pass_a.present;
    // ---- End of the bindings ----
    if (!(rf.truth && qc.epoch.computed)) return;
    for (std::size_t i = 0; i < qc.epoch.rows.size() && i < present.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& row : qc.epoch.rows[i])
            ringFixTruthObserve(*rf.truth, static_cast<int>(i), row.satellite, t.tow);
    }
}

std::vector<RingFixCut> networkRingFixHoldCuts(NetworkRun& run, const NetworkEpoch& ep,
                                               const std::set<net::ArcTuple>& latched) {
    if (!run.ctx.carry.ring_fix) return {};   // the gate, before the bindings read the ring fix's state
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the nested-cut and entry-check counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX entry_refused events
    const std::vector<NetStation>& stations = run.setup.stations;
    const SlipQcChannel& qc = run.engines.slip_qc;
    const GnssTime& t = ep.se.time;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    // ---- End of the bindings ----
    std::vector<RingFixCut> cuts;
    // The near-null signature of the held Galileo E1/E5a chords, read for the cuts' entry links
    // (network.ring_fix.entry_revalidate); each refused entry is reported once per epoch.
    std::map<RingFixLinkId, RingFixNearNullVerdict> entry_signature;
    std::set<RingFixLinkId> entry_reported;
    if (rf.entry_revalidate && obs_rec != nullptr && obs_rec->armed && !obs_rec->releases.empty())
        for (const RingFixNearNullVerdict& v :
             ringFixNearNullVerdicts(ringFixNearNullInputs(rf), rf.nn_tol_gf_m, rf.nn_max_if_m))
            entry_signature.emplace(v.id, v);
    if (obs_rec != nullptr && obs_rec->armed) {
        std::map<std::string, std::map<std::string, std::vector<std::string>>> kids;   // group -> parent -> children
        for (const ChainEdgeObs& e : obs_rec->edges) kids[e.group][e.parent].push_back(e.child);
        std::set<std::pair<std::string, std::string>> released_at;   // (group, released vertex)
        for (const ChainReleaseObs& r : obs_rec->releases) released_at.insert({r.group, r.vertex});
        for (const ChainReleaseObs& r : obs_rec->releases) {
            RingFixCut c;
            c.sys = r.group.substr(0, r.group.find('|'));
            c.signal = r.signal;
            c.stations.insert(r.stations.begin(), r.stations.end());
            c.sats.insert(r.sats.begin(), r.sats.end());
            c.row = chainReleaseRowName(r.row);
            c.entry_station = r.station;
            c.entry_sat = r.sat;
            bool nested = false;
            const auto gk = kids.find(r.group);
            std::set<std::string> seen;
            std::vector<std::string> stack{r.vertex};
            while (!stack.empty()) {
                const std::string v = stack.back();
                stack.pop_back();
                if (!seen.insert(v).second) continue;
                if (v != r.vertex && released_at.count({r.group, v}) != 0) nested = true;
                if (const std::optional<ChainVertex> cv = chainVertexOfKey(v)) {
                    if (cv->is_sat) c.sats.insert(cv->sat);
                    else c.stations.insert(cv->station);
                }
                if (gk == kids.end()) continue;
                const auto ch = gk->second.find(v);
                if (ch != gk->second.end()) stack.insert(stack.end(), ch->second.begin(), ch->second.end());
            }
            if (nested) ++rf.counters.cuts_nested;
            const RingFixLink* h = heldLinkOf(rf, r.station, r.sat, r.signal);
            const RingFixLinkId* hid = h != nullptr ? idOf(rf, h) : nullptr;
            double target_cyc = std::numeric_limits<double>::quiet_NaN();
            for (const ChainEdgeObs& e : obs_rec->edges)
                if (e.station == r.station && e.sat == r.sat && e.signal == r.signal) {
                    c.held_first = e.held_first;
                    if (e.lambda_m > 0.0) target_cyc = e.target_m / e.lambda_m;
                    break;
                }
            const bool usable = h != nullptr && hid != nullptr && !h->monitor_trip
                             && latched.count({r.station, r.sat, r.signal}) == 0;
            const long long n_held = (usable && hid->sig1 == r.signal) ? h->n1 : (usable ? h->n2 : 0);
            // Whether the entry's held integer is the known step: chainEntryStep owns it.  Off, a usable entry is
            // known, as before (network.ring_fix.entry_revalidate); on, its gap and its signature are asked too.
            RingFixEntryEvidence ev;
            ev.usable = usable;
            RingFixEntryVerdict verdict = chainEntryStep(ChainEntryKind::CutEntry, ev, false, rf.nn_max_if_m);
            if (usable && rf.entryGapChecked()) {
                ev.resumed = c.row == "returning";
                for (const GapResumption& g : qc.epoch.resumptions) {
                    if (g.station != r.station || g.satellite != r.sat) continue;
                    ev.resumed = true;
                    if ((g.sig1 == hid->sig1 && g.sig2 == hid->sig2) || (g.sig1 == hid->sig2 && g.sig2 == hid->sig1)) {
                        ev.gap_measured = g.measurable;
                        ev.gap_shifted = g.shifted;
                    }
                }
                const auto sg = entry_signature.find(*hid);
                if (sg != entry_signature.end()) {
                    ev.signature = true;
                    ev.signature_k = sg->second.k;
                    ev.signature_if_m = sg->second.dev_if_m;
                }
                verdict = chainEntryStep(ChainEntryKind::CutEntry, ev, true, rf.nn_max_if_m);
                ++rf.counters.entry_checked;
                switch (verdict) {
                    case RingFixEntryVerdict::Known:        ++rf.counters.entry_known; break;
                    case RingFixEntryVerdict::GapUnchecked: ++rf.counters.entry_refused_unchecked; break;
                    case RingFixEntryVerdict::GapShifted:   ++rf.counters.entry_refused_shifted; break;
                    case RingFixEntryVerdict::Signature:    ++rf.counters.entry_refused_signature; break;
                    case RingFixEntryVerdict::NotHeld:      break;
                    case RingFixEntryVerdict::RowUnknown:   break;   // not a cut's verdict
                }
                if (verdict != RingFixEntryVerdict::Known && entry_reported.insert(*hid).second)
                    traces.emitEvent(t, "RINGFIX", "entry_refused", r.sat, ringFixStationName(stations, r.station),
                                     sg != entry_signature.end() ? std::optional<double>(sg->second.dev_gf_m)
                                                                 : std::nullopt,
                                     std::nullopt, ringFixEntryVerdictName(verdict),
                                     fmt::format("sig1={} sig2={} n1={} n2={} cut={} resumed={} pair_measured={} "
                                                 "shifted={} k={} dev_if={:.3f}",
                                                 hid->sig1, hid->sig2, h->n1, h->n2, c.row, ev.resumed ? 1 : 0,
                                                 ev.gap_measured ? 1 : 0, ev.gap_shifted ? 1 : 0, ev.signature_k,
                                                 ev.signature_if_m));
            }
            c.known = ringFixCutStep(verdict == RingFixEntryVerdict::Known, n_held, target_cyc, r.sat_is_child, c.delta);
            cuts.push_back(std::move(c));
        }
    }
    return cuts;
}

void networkRingFixHoldThroughCuts(NetworkRun& run, const NetworkEpoch& ep, const std::set<net::ArcTuple>& latched,
                                   const std::vector<RingFixCut>& cuts) {
    if (!run.ctx.carry.ring_fix) return;   // the gate, before the bindings read the ring fix's state
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links, the near-null re-opens, the counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX release, reexpress and reattach events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    // The near-null guard's moves after the last solve (network.ring_fix.nn_guard): each moved chord's carried entries
    // re-open as at a release, so that the hold rows at its new integers re-level it; the release step then takes its new
    // re-level stamps as the baseline instead of reading the re-open as a refloat.
    for (const RingFixLinkId& id : rf.nn_reopen) {
        const auto it = rf.links.find(id);
        if (it == rf.links.end() || !it->second.held) continue;
        networkRingFixReopenCarry(run, ep, id, id.sig1, std::numeric_limits<double>::infinity());
        networkRingFixReopenCarry(run, ep, id, id.sig2, std::numeric_limits<double>::infinity());
        it->second.rebase_stamps = true;
        rf.counters.nn_reopened += 2;
    }
    rf.nn_reopen.clear();
    std::set<RingFixLinkId> usable_before;   // held, not latched, monitor not tripped
    for (const auto& [id, L] : rf.links)
        if (L.held && !L.monitor_trip && latched.count({id.station, id.sat, id.sig1}) == 0
            && latched.count({id.station, id.sat, id.sig2}) == 0)
            usable_before.insert(id);
    const RingFixCutOutcome cut_out = ringFixApplyCuts(rf, cuts, now_s);
    // An open suspension (network.ring_fix.resume_response "revalidate") that a cut separates ends; none with it off.
    networkRingFixTraceResumeEdges(run, ep, ringFixEndCrossingsAtCuts(rf, cuts, now_s));
    for (const auto& [id, row] : cut_out.released) {
        const RingFixLink& L = rf.links.at(id);
        if (usable_before.count(id) != 0) ++rf.counters.cut_released_usable;
        networkRingFixReopenCarry(run, ep, id, id.sig1, L.stamp1);
        networkRingFixReopenCarry(run, ep, id, id.sig2, L.stamp2);
        traces.emitEvent(t, "RINGFIX", "release", id.sat, ringFixStationName(stations, id.station), std::nullopt,
                         std::nullopt, "chain",
                         fmt::format("sig1={} sig2={} n1={} n2={} ring={} held_s={:.0f} cut={} step=unknown",
                                     id.sig1, id.sig2, L.n1, L.n2, L.ring, now_s - L.decided_s, row));
    }
    for (const auto& [id, d1, d2, rows] : cut_out.reexpressed) {
        const RingFixLink& L = rf.links.at(id);
        traces.emitEvent(t, "RINGFIX", "reexpress", id.sat, ringFixStationName(stations, id.station), std::nullopt,
                         std::nullopt, "cut",
                         fmt::format("sig1={} sig2={} n1={} n2={} d1={} d2={} cut={}", id.sig1, id.sig2, L.n1, L.n2,
                                     d1, d2, rows));
    }
    traceHeldFirstReattachments(rf, traces, stations, t, cuts, cut_out);
}

void networkRingFixHoldToTree(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links, the certified edges, the counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX to_tree events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        if (facts.tree.count({id.station, id.sat, id.sig1}) == 0 && facts.tree.count({id.station, id.sat, id.sig2}) == 0)
            continue;
        ringFixUnholdLink(L, now_s, RingFixUnholdKind::ToTree);
        ++rf.counters.to_tree;
        if (rf.settled_flags_on) rf.certified_edges.insert({id.station, id.sat, id.sig1});
        traces.emitEvent(t, "RINGFIX", "to_tree", id.sat, ringFixStationName(stations, id.station), std::nullopt,
                         std::nullopt, "chain", fmt::format("sig1={} sig2={} n1={} n2={}", id.sig1, id.sig2, L.n1, L.n2));
    }
}

void networkRingFixHoldRoots(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links' roots, the counters
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    // ---- End of the bindings ----
    if (obs_rec == nullptr || !obs_rec->armed || obs_rec->roots.empty()) return;
    for (auto& [id, L] : rf.links) {
        if (!L.held || L.root.empty()) continue;
        const std::string g = ringFixGroupOf(id.sat, id.sig1);
        const std::string now_root = facts.root(g, chainVertexKey(id.station));
        if (now_root.empty() || now_root == L.root) continue;
        for (const ChainRootObs& ro : obs_rec->roots)
            if (ro.group == g && ro.new_root == now_root && ro.outcome == ChainRootOutcome::Pinned) {
                L.root = now_root;
                ++rf.counters.root_kept;
                break;
            }
    }
}

void networkRingFixHoldReleases(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts,
                                const std::set<net::ArcTuple>& latched) {
    if (!run.ctx.carry.ring_fix) return;   // the gate, before the bindings read the ring fix's state
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links, the release counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX release events
    const std::vector<NetStation>& stations = run.setup.stations;
    const net::ArcTable& arc_rows = run.ctx.entities.arcs;
    const SlipQcChannel& qc = run.engines.slip_qc;   // the epoch's gap resumptions (network.ring_fix.resume_response)
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    const bool resume_on = rf.resume_response != RingFixResumeResponse::Off;
    std::map<RingFixLinkId, RingFixHoldObs> obs;
    for (const auto& [id, L] : rf.links) {
        // The held links, and the suspended ones of network.ring_fix.resume_response "revalidate" (none with it off).
        if (!L.held && !L.crossing.active) continue;
        RingFixHoldObs o;
        // Neither chain input of the release step can be true for a held link here: stage 3 took every held link
        // that is a tree edge out of the held set, and stage 2 applied the cuts.  On this path the step's chain cause
        // is a root change alone (the two inputs stay in its contract, which its unit tests exercise).  A suspended
        // link is not held, so stage 3 left it: its tree entry is read here, for the end of its suspension.
        o.tree_now = !L.held && (facts.tree.count({id.station, id.sat, id.sig1}) > 0
                                 || facts.tree.count({id.station, id.sat, id.sig2}) > 0);
        o.in_cut = false;
        o.present = facts.chords.count({id.station, id.sat, id.sig1}) > 0
                 && facts.chords.count({id.station, id.sat, id.sig2}) > 0;
        o.root = facts.root(ringFixGroupOf(id.sat, id.sig1), chainVertexKey(id.station));
        o.latched = latched.count({id.station, id.sat, id.sig1}) > 0
                 || latched.count({id.station, id.sat, id.sig2}) > 0;
        const LocalCarry* c1 = net::getArc(arc_rows, net::ArcTuple{id.station, id.sat, id.sig1}, &net::ArcState::amb);
        const LocalCarry* c2 = net::getArc(arc_rows, net::ArcTuple{id.station, id.sat, id.sig2}, &net::ArcState::amb);
        o.carry_ok = c1 != nullptr && c2 != nullptr;
        o.stamp1 = c1 != nullptr ? c1->relevel_s : 0.0;
        o.stamp2 = c2 != nullptr ? c2->relevel_s : 0.0;
        if (resume_on) o.resume = ringFixResumeClassOf(id, qc.epoch.resumptions, qc.epoch.sig_resumptions);
        obs[id] = o;
    }
    // The response to a tracking break of the link's own, in its epoch and before the release step, which then
    // skips the links it released or suspended (no-op with network.ring_fix.resume_response off).  A released or
    // suspended link's carried entries re-open: the level across the break is unknown.
    const std::vector<RingFixResumeEdge> resumed = ringFixResumeStep(rf, obs, now_s);
    for (const RingFixResumeEdge& e : resumed) {
        if (e.kind == RingFixResumeEdge::Kind::Clear) continue;
        const RingFixLink& L = rf.links.at(e.id);
        networkRingFixReopenCarry(run, ep, e.id, e.id.sig1, L.stamp1);
        networkRingFixReopenCarry(run, ep, e.id, e.id.sig2, L.stamp2);
    }
    networkRingFixTraceResumeEdges(run, ep, resumed);
    const std::vector<RingFixEvent> events = ringFixReleaseStep(rf, obs, now_s);
    // Every cause re-opens the carried entries: a slip's and a refloat's drop already ran (the
    // floor still binds where it left the entry tighter); the other causes re-open them here,
    // unless something re-levelled them since the decision.
    for (const RingFixEvent& ev : events) {
        const RingFixLink& L = rf.links.at(ev.id);
        networkRingFixReopenCarry(run, ep, ev.id, ev.id.sig1, L.stamp1);
        networkRingFixReopenCarry(run, ep, ev.id, ev.id.sig2, L.stamp2);
        traces.emitEvent(t, "RINGFIX", "release", ev.id.sat, ringFixStationName(stations, ev.id.station), std::nullopt,
                         std::nullopt, ringFixReleaseName(ev.release),
                         fmt::format("sig1={} sig2={} n1={} n2={} ring={} held_s={:.0f}", ev.id.sig1,
                                     ev.id.sig2, L.n1, L.n2, L.ring, now_s - L.decided_s));
    }
}

void networkRingFixHoldCascadeReleases(NetworkRun& run, const NetworkEpoch& ep,
                                       const std::set<net::ArcTuple>& latched, const std::vector<RingFixCut>& cuts) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the cascade links, the cascade counters
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX cascade_release events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    for (auto& [id, CL] : rf.cascade_links) {
        if (!CL.held) continue;
        const RingFixCascade* c = rf.cascadeOf(id);
        if (c == nullptr) continue;
        std::string cause;
        const auto P = rf.links.find(RingFixLinkId{id.station, id.sat, c->s1, c->s2});
        if (P == rf.links.end() || !P->second.held) cause = "pair";
        else if (latched.count({id.station, id.sat, c->s3}) != 0) cause = "slip";
        else if (CL.monitor_trip) cause = "monitor";
        else if (CL.absent_since_s >= 0.0 && now_s - CL.absent_since_s > rf.max_gap_s) cause = "gap";
        else
            for (const RingFixCut& cut : cuts)
                if (cut.signal == c->s3 && cut.sys == c->ck
                    && (cut.stations.count(id.station) != 0) != (cut.sats.count(id.sat) != 0)) {
                    cause = "cut";
                    break;
                }
        if (cause.empty()) continue;
        CL.held = false;
        CL.released_s = now_s;
        CL.monitor_bad = 0;
        CL.monitor_trip = false;
        CL.absent_since_s = -1.0;
        networkRingFixReopenCarry(run, ep, id, c->s3, CL.stamp3);
        ++rf.cascade_counters[c->name].released[cause];
        traces.emitEvent(t, "RINGFIX", "cascade_release", id.sat, ringFixStationName(stations, id.station), std::nullopt,
                         std::nullopt, cause.c_str(),
                         fmt::format("cascade={} ref={} n_ref={} n3={} held_s={:.0f}", c->name, c->ref,
                                     CL.n_ref, CL.n3, now_s - CL.decided_s));
    }
}

void networkRingFixHoldInherit(NetworkRun& run, const NetworkEpoch& ep, const RingFixChainFacts& facts,
                               const std::set<net::ArcTuple>& latched, const std::vector<RingFixCut>& cuts) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the links, the counters (ringFixInherit)
    NetworkRunTraces& traces = run.traces;          // written: the RINGFIX hold events of the old hosts
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    // ---- End of the bindings ----
    if (rf.prev_targets.empty() || obs_rec == nullptr || !obs_rec->armed) return;
    std::map<std::pair<int, std::string>, std::map<std::string, double>> prev_of;   // (sta, sat) -> sig -> k
    for (const auto& [key, k] : rf.prev_targets) prev_of[{std::get<0>(key), std::get<1>(key)}][std::get<2>(key)] = k;
    std::vector<RingFixInheritCand> cands;
    for (const auto& [ss, sigs] : prev_of) {
        std::set<std::string> names;
        for (const auto& kv : sigs) names.insert(kv.first);
        for (const auto& [s1, s2] : chainArcFamilies(ss.second, names,
                                                     rf.form_configured_pairs ? &rf.configured_pairs : nullptr)) {
            const int sta = ss.first;
            const std::string& sat = ss.second;
            if (facts.tree.count({sta, sat, s1}) || facts.tree.count({sta, sat, s2})) continue;
            if (!facts.chords.count({sta, sat, s1}) || !facts.chords.count({sta, sat, s2})) continue;
            const double k1 = sigs.at(s1), k2 = sigs.at(s2);
            if (std::abs(k1 - std::round(k1)) > 1e-6 || std::abs(k2 - std::round(k2)) > 1e-6) continue;
            RingFixInheritCand c;
            c.id = {sta, sat, s1, s2};
            c.k1 = std::llround(k1);
            c.k2 = std::llround(k2);
            c.lambda1 = chainWavelengthM(sat, s1);
            c.lambda2 = chainWavelengthM(sat, s2);
            c.latched = latched.count({sta, sat, s1}) > 0 || latched.count({sta, sat, s2}) > 0;
            const std::string g = ringFixGroupOf(sat, s1);
            c.ring = facts.ring(g, chainVertexKey(sta), chainVertexKey(sat));
            c.root = facts.root(g, chainVertexKey(sta));
            if (!(c.lambda1 > 0.0 && c.lambda2 > c.lambda1)) continue;
            cands.push_back(c);
        }
    }
    for (const RingFixEvent& ev : ringFixInherit(rf, cands, cuts, now_s))
        traces.emitEvent(t, "RINGFIX", "hold", ev.id.sat, ringFixStationName(stations, ev.id.station), std::nullopt,
                         std::nullopt, "inherit",
                         fmt::format("sig1={} sig2={} ring={} n1={} n2={} wl={} old_host=1", ev.id.sig1,
                                     ev.id.sig2, rf.links.at(ev.id).ring, ev.gate.n1, ev.gate.n2, ev.gate.wl));
}

void networkRingFixHoldTargets(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the previous targets
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    // ---- End of the bindings ----
    if (obs_rec == nullptr || !obs_rec->armed) return;
    rf.prev_targets.clear();
    for (const ChainEdgeObs& e : obs_rec->edges)
        if (e.lambda_m > 0.0) rf.prev_targets[{e.station, e.sat, e.signal}] = e.target_m / e.lambda_m;
}

void networkRingFixHoldTable(NetworkRun& run) {
    // ---- From the run ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the hold table
    // ---- End of the bindings ----
    rf.table.clear();
    for (const auto& [id, L] : rf.links) {
        if (!L.held) continue;
        rf.table[{id.station, id.sat, id.sig1}] = L.lambda1 * static_cast<double>(L.n1);
        rf.table[{id.station, id.sat, id.sig2}] = L.lambda2 * static_cast<double>(L.n2);
    }
    for (const auto& [id, CL] : rf.cascade_links)
        if (CL.held) rf.table[{id.station, id.sat, id.sig2}] = CL.lambda3 * static_cast<double>(CL.n3);
}

void networkRingFixTraceResumeEdges(NetworkRun& run, const NetworkEpoch& ep, const std::vector<RingFixResumeEdge>& edges) {
    // ---- From the run and the epoch ----
    NetworkRunTraces& traces = run.traces;   // written: the RINGFIX release, resume_suspend and resume_clear events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    // ---- End of the bindings ----
    for (const RingFixResumeEdge& e : edges) {
        const std::string detail =
            fmt::format("sig1={} sig2={} n1={} n2={} class={} carry={} dgf={} gap_ep={} lli={}", e.id.sig1, e.id.sig2,
                        e.n1, e.n2, ringFixResumeClassName(e.cls), e.carry_fresh ? "fresh" : "bridged",
                        e.cls == RingFixResumeClass::Shifted || e.cls == RingFixResumeClass::Clean
                            ? fmt::format("{:.4f}", e.d_gf_m) : std::string(),
                        e.gap_epochs, e.lli)
            + (e.verdict == "contradicted" ? fmt::format(" m1={} m2={}", e.m1, e.m2) : std::string());
        const std::string sta = ringFixStationName(stations, e.id.station);
        switch (e.kind) {
            case RingFixResumeEdge::Kind::Release:
                traces.emitEvent(t, "RINGFIX", "release", e.id.sat, sta, std::nullopt, std::nullopt,
                                 ringFixReleaseName(RingFixRelease::Resume), detail);
                break;
            case RingFixResumeEdge::Kind::Suspend:
                traces.emitEvent(t, "RINGFIX", "resume_suspend", e.id.sat, sta, std::nullopt, std::nullopt, "resume",
                                 detail);
                break;
            case RingFixResumeEdge::Kind::Clear:
                traces.emitEvent(t, "RINGFIX", "resume_clear", e.id.sat, sta, std::nullopt, std::nullopt,
                                 e.verdict.c_str(), detail);
                break;
        }
    }
}

void networkRingFixReopenCarry(NetworkRun& run, const NetworkEpoch& ep, const RingFixLinkId& id,
                               const std::string& sig, double stamp) {
    // ---- From the run and the epoch ----
    NetworkRunContext& ctx = run.ctx;                // written: the carry-state trace (ambStateNoteDrop)
    NetworkRingFix& rf = *ctx.carry.ring_fix;        // written: the re-open counters
    net::ArcTable& arc_rows = ctx.entities.arcs;     // written: the link's carried entry
    const std::vector<NetStation>& stations = run.setup.stations;
    const double elev_power = run.config.getCodeElevationPower();
    const long long now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const bool amb_float_kf = ctx.knobs.amb_float_kf;
    const double amb_drop_var_floor = ctx.knobs.amb_drop_var_floor;
    const int amb_drop_floor_after_ep = ctx.knobs.amb_drop_floor_after_ep;
    const double amb_birth_var_floor = ctx.knobs.amb_birth_var_floor;
    const bool amb_drop_decouple_always = ctx.knobs.amb_drop_decouple_always;
    const bool trace_amb_state = ctx.knobs.trace_amb_state;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    const net::ArcTuple key{id.station, id.sat, sig};
    LocalCarry* lc = net::getArc(arc_rows, key, &net::ArcState::amb);
    if (lc == nullptr) return;
    const bool stale = lc->relevel_s <= stamp;
    if (amb_float_kf) {
        if (!stale && !(lc->var < kRingFixReleaseFloorM2)) return;
        const double drop_base = ambFloatDropFloorNow(amb_drop_var_floor, now_ep, amb_drop_floor_after_ep);
        const double birth_floor = ambFloatBirthFloor(amb_birth_var_floor, -1.0, elev_power);
        const double drop_floor = ambFloatBirthFloor(drop_base, -1.0, elev_power);
        if (std::max({lc->var_birth, lc->var_first, birth_floor, drop_floor}) < kRingFixReleaseFloorM2)
            ++rf.counters.release_floored;
        ambFloatDropFloored(*lc, birth_floor, std::max(drop_floor, kRingFixReleaseFloorM2), now_s, now_ep,
                            amb_drop_decouple_always);
        if (trace_amb_state)
            ambStateNoteDrop(ctx, t.week, t.tow, ringFixStationName(stations, id.station), id.sat, sig, *lc,
                             "ring_fix_release");
    } else {
        if (!stale) return;
        net::dropArc(arc_rows, key, &net::ArcState::amb);
    }
    ++rf.counters.carry_reopened;
}

} // namespace GnssProcessor
