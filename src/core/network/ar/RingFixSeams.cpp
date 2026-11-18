/**
 * @file RingFixSeams.cpp
 * @brief The network seams of the ring fix: setup, the release and hold table before PASS B, the
 *        PASS B hold rows, the decision after the solve, and the run-end report (see
 *        network/ar/RingFix.hpp).
 */

#include "network/ar/RingFix.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"            // constellationKey, systemOfSat
#include "filter/StateKeySchema.hpp"            // StateKeys::makeAmbiguityKey
#include "network/ar/AmbFloat.hpp"              // ambFloatDropFloored, ambFloatBirthFloor, ambFloatDropFloorNow
#include "network/assembly/Contributions.hpp"   // makeRowTag
#include "network/assembly/StationBuild.hpp"    // StationBuild, StationCarryFrame
#include "network/diag/AmbStateTrace.hpp"       // ambStateNoteDrop
#include "network/diag/LiveRecord.hpp"          // LiveHolds (the live record's held links)
#include "network/diag/RunTraces.hpp"
#include "network/driver/RunGroups.hpp"           // NetworkEngines (the slip channel)
#include "network/slip/OraclePin.hpp"            // networkOraclePinParseAmbCsv
#include "network/slip/RawObservables.hpp"       // SlipQcChannel (the truth's arcs)
#include "network/slip/SlipOracle.hpp"           // networkSlipOracleParseCsv
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/solve/ChainChordTable.hpp"
#include "network/solve/ChainFrame.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>

namespace GnssProcessor {

namespace {



std::string rxKey(int station) { return "R:" + std::to_string(station); }
std::string svKey(const std::string& sat) { return "S:" + sat; }
std::string groupOf(const std::string& sat, const std::string& sig) { return sat.substr(0, 1) + "|" + sig; }

/// The arcs in this epoch's free-epoch latch, as (station, satellite, signal).
std::set<net::ArcTuple> latchedArcs(const RelevelEpochState& relevel) {
    std::set<net::ArcTuple> out;
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const std::optional<ArcId> arc = net::arcKey(sta, amb_key);
        if (!arc) continue;
        out.insert({sta, net::satKey(arc->sat), net::signalKey(arc->signal)});
    }
    return out;
}

/// What the release reads from the chain record: tree arcs, chords, roots and cut vertices.
struct ChainFacts {
    std::set<net::ArcTuple> tree;    ///< Tree edges (station, satellite, signal).
    std::set<net::ArcTuple> chords;  ///< Chords (station, satellite, signal).
    /// group -> vertex key -> its component's root receiver.
    std::map<std::string, std::map<std::string, std::string>> root_of;
    std::set<std::string> cut_vertices;   ///< Vertices on a cut release's moved side ("R:<i>", "S:<sat>").
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

ChainFacts chainFacts(const ChainEpochObs* obs) {
    ChainFacts f;
    if (obs == nullptr || !obs->armed) return f;
    for (const ChainEdgeObs& e : obs->edges) {
        f.tree.insert({e.station, e.sat, e.signal});
        f.root_of[e.group][e.child] = e.r1;
        f.root_of[e.group][e.parent] = e.r1;
        f.depth_of[e.group][e.child] = e.depth;
        f.depth_of[e.group].emplace(e.parent, e.depth - 1);
    }
    for (const ChainChordCycle& c : obs->chords) f.chords.insert({c.station, c.sat, c.signal});
    for (const ChainReleaseObs& r : obs->releases) {
        for (const std::string& s : r.sats) f.cut_vertices.insert(svKey(s));
        for (int i : r.stations) f.cut_vertices.insert(rxKey(i));
    }
    return f;
}

/// The post-fit residual (m) of the observation phase row on an ambiguity column, if one exists.
std::optional<double> phaseResidual(const StationAssembly& R, const Eigen::VectorXd& resid, Eigen::Index col) {
    if (col < 0 || col >= R.A_r.cols()) return std::nullopt;
    const Eigen::Index n = std::min<Eigen::Index>(R.A_r.rows(), static_cast<Eigen::Index>(R.row_tags.size()));
    for (Eigen::Index k = 0; k < n && k < resid.size(); ++k) {
        const ContributionKind kind = R.row_tags[static_cast<std::size_t>(k)].kind;
        if (kind != ContributionKind::DataPhase && kind != ContributionKind::DataPhaseSecond) continue;
        if (std::abs(R.A_r(k, col)) > 0.5) return resid(k);
    }
    return std::nullopt;
}

const char* auditName(RingFixAudit a) {
    switch (a) {
        case RingFixAudit::Right:    return "right";
        case RingFixAudit::Wrong:    return "wrong";
        case RingFixAudit::Unlisted: return "unlisted";
    }
    return "?";
}

std::string cellOf(double v, int digits) {
    if (!std::isfinite(v)) return std::string();
    return fmt::format("{:.{}f}", v, digits);
}

// ---- The re-attachment of untied satellites (network.ring_fix.reattach) ----

/// The parents in one band's trees this epoch, for the test that a station hangs below a satellite.
struct BandParents {
    std::map<std::string, int> sat_parent;       ///< Satellite -> the station its tree edge hangs it under.
    std::map<int, std::string> station_parent;   ///< Station (roster index) -> the satellite its tree edge hangs it under.
};

BandParents bandParents(const ChainEpochObs& obs, const std::string& ck, const std::string& signal) {
    BandParents p;
    for (const ChainEdgeObs& e : obs.edges) {
        if (e.signal != signal || constellationKey(systemOfSat(e.sat)) != ck) continue;
        if (e.sat_is_child) p.sat_parent[e.sat] = e.station;
        else                p.station_parent[e.station] = e.sat;
    }
    return p;
}

/// Whether a station hangs below a satellite in one band's trees: its chain of parents reaches the satellite.
bool hangsBelow(const BandParents& p, int station, const std::string& sat) {
    int cur = station;
    for (int step = 0; step < 1024; ++step) {
        const auto ps = p.station_parent.find(cur);
        if (ps == p.station_parent.end()) return false;
        if (ps->second == sat) return true;
        const auto pp = p.sat_parent.find(ps->second);
        if (pp == p.sat_parent.end()) return false;
        cur = pp->second;
    }
    return false;
}

/// The election's outcomes of this epoch's moves as events, the applied moves into the watch, and every move the
/// election did not reach (its family not elected, or the satellite gone) counted as missed.
void reportUntiedOutcomes(NetworkRingFix& rf, const std::vector<NetStation>& stations, double now_s,
                          const GnssTime& t, NetworkRunTraces& traces) {
    const auto name = [&](int s) {
        return s >= 0 && static_cast<std::size_t>(s) < stations.size() ? stations[static_cast<std::size_t>(s)].name
                                                                       : std::string("none");
    };
    for (const RingFixUntiedOutcome& o : rf.untied_outcomes) {
        traces.emitEvent(t, "RINGFIX", o.applied ? "untied_applied" : "untied_missed", o.sat, name(o.station),
                         std::nullopt, std::nullopt, "chain",
                         fmt::format("family={} sig={} taken={}", o.family, o.signal, name(o.taken)));
        if (o.applied) rf.untied_moved_s[{o.family, o.sat}] = now_s;
    }
    rf.untied_outcomes.clear();
    for (const auto& [key, m] : rf.untied_moves) {
        ++rf.counters.untied_missed;
        traces.emitEvent(t, "RINGFIX", "untied_missed", key.second, name(m.station), std::nullopt, std::nullopt,
                         "chain", fmt::format("family={} sig={} taken=none (not elected)", m.family, key.first));
    }
    rf.untied_moves.clear();
}

/// One held family's re-attachment at one epoch, after its settled set: the watch on the moved satellites, the
/// dwell of the untied ones, and a move for every untied satellite past the dwell whose candidates agree.
/// @p rule_set is the settled rule's set before the wait (its components are the flagged piece), @p flagged the
/// set after the wait (the stream's flags).
void requestUntiedMoves(NetworkRingFix& rf, const std::string& ck, const std::string& s1, const std::string& s2,
                        const std::vector<std::pair<int, std::string>>& certified,
                        const std::map<std::string, int>& root_of_sat, const std::set<std::string>& rule_set,
                        const std::set<std::string>& flagged, const std::vector<RingFixDecideObs>& links,
                        const ChainEpochObs& obs, const std::vector<NetStation>& stations, double now_s,
                        const GnssTime& t, NetworkRunTraces& traces) {
    const std::string fam = ck + ":" + s1 + "/" + s2;
    for (auto it = rf.untied_moved_s.begin(); it != rf.untied_moved_s.end();) {
        if (it->first.first != fam) {
            ++it;
        } else if (flagged.count(it->first.second) != 0) {
            ++rf.counters.untied_flagged_after;
            it = rf.untied_moved_s.erase(it);
        } else if (now_s - it->second >= kRingFixReattachWatchS) {
            ++rf.counters.untied_unflagged_after;
            it = rf.untied_moved_s.erase(it);
        } else {
            ++it;
        }
    }
    // The ties and the flagged piece over the settled rule's graph (stations keep their roster index as id).
    arcgraph::Dsu dsu;
    std::map<std::string, int> sat_id;
    std::set<int> tied_stations;
    int next_id = 1 << 20;
    for (const auto& [sta, sat] : certified) {
        auto f = sat_id.find(sat);
        if (f == sat_id.end()) f = sat_id.emplace(sat, next_id++).first;
        arcgraph::dsuUnite(dsu, sta, f->second);
        tied_stations.insert(sta);
    }
    std::set<int> piece;
    for (const std::string& s : rule_set)
        if (const auto f = sat_id.find(s); f != sat_id.end()) piece.insert(arcgraph::dsuFind(dsu, f->second));
    const auto inPiece = [&](int sta) {
        return tied_stations.count(sta) != 0 && piece.count(arcgraph::dsuFind(dsu, sta)) != 0;
    };
    const BandParents p1 = bandParents(obs, ck, s1);
    const BandParents p2 = bandParents(obs, ck, s2);
    for (auto it = rf.untied_since_s.begin(); it != rf.untied_since_s.end();)
        it = it->first.first == fam && root_of_sat.count(it->first.second) == 0 ? rf.untied_since_s.erase(it)
                                                                                 : std::next(it);
    for (const auto& [sat, root] : root_of_sat) {
        (void)root;
        const std::pair<std::string, std::string> key{fam, sat};
        if (rule_set.count(sat) != 0 || sat_id.count(sat) != 0) {   // settled, or tied
            rf.untied_since_s.erase(key);
            continue;
        }
        auto since = rf.untied_since_s.emplace(key, now_s).first;
        if (now_s - since->second < kRingFixReattachDwellS) continue;
        if (const auto lm = rf.untied_last_move_s.find(key);
            lm != rf.untied_last_move_s.end() && now_s - lm->second < kRingFixReattachHoldoffS) {
            ++rf.counters.untied_held_off;   // moved within the hold-off: its links' window and persistence first
            continue;
        }
        std::vector<RingFixReattachCand> cands;
        for (const RingFixDecideObs& o : links) {
            if (o.id.sat != sat || o.id.sig1 != s1 || o.id.sig2 != s2 || !o.have_float) continue;
            if (!inPiece(o.id.station) || hangsBelow(p1, o.id.station, sat) || hangsBelow(p2, o.id.station, sat))
                continue;
            const auto L = rf.links.find(o.id);
            if (L == rf.links.end()) continue;
            const auto lt = rf.last_tree_s.find({o.id.station, o.id.sat});
            const double last_tree =
                lt == rf.last_tree_s.end() ? -std::numeric_limits<double>::infinity() : lt->second;
            if (!ringFixWindowOk(L->second, o, last_tree, now_s)) continue;
            const RingFixGate g = ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2);
            if (!g.valid || g.sigma_if > kRingFixMaxSigmaCyc) continue;
            cands.push_back({o.id.station, g.n_if - static_cast<double>(g.n1),
                             (o.f1 - o.f2) - static_cast<double>(g.wl)});
        }
        const RingFixReattachPick pk = ringFixReattachChoice(cands);
        if (!pk.ok) {
            ++(pk.n < kRingFixReattachMinLinks ? rf.counters.untied_few
               : pk.converging                 ? rf.counters.untied_converging
                                               : rf.counters.untied_spread);
            continue;
        }
        const int station = cands[static_cast<std::size_t>(pk.index)].station;
        rf.untied_moves[{s1, sat}] = RingFixUntiedMove{station, fam};
        rf.untied_moves[{s2, sat}] = RingFixUntiedMove{station, fam};
        since->second = now_s;   // the dwell restarts after a move
        rf.untied_last_move_s[key] = now_s;
        ++rf.counters.untied_requests;
        const auto host = [&](const BandParents& p) {
            const auto h = p.sat_parent.find(sat);
            return h == p.sat_parent.end() ? std::string("-") : stations[static_cast<std::size_t>(h->second)].name;
        };
        traces.emitEvent(t, "RINGFIX", "untied_move", sat, stations[static_cast<std::size_t>(station)].name, std::nullopt,
                         std::nullopt, "chain",
                         fmt::format("family={} from={}/{} n={} med_if={:+.3f} med_wl={:+.3f} sd_if={:.3f} "
                                     "sd_wl={:.3f}",
                                     fam, host(p1), host(p2), pk.n, pk.med_if, pk.med_wl, pk.sd_if, pk.sd_wl));
    }
}

/// The live record's note (network.trace_live_record): every pair-family link with a float this
/// epoch and whether it is held in this epoch's solve, counted by family, receiver and satellite.
/// These are the rows network_ring_fix.csv writes with a float, "held" where it writes that state.
/// Measure-only; one test when the record is off.
void noteLiveHolds(NetworkRunTraces& traces, const GnssTime& t, const std::vector<RingFixDecideObs>& links,
                   const std::map<RingFixLinkId, bool>& held_before) {
    if (!traces.live_record.on()) return;
    LiveHolds holds;
    for (const RingFixDecideObs& o : links) {
        if (!o.have_float) continue;
        const auto h = held_before.find(o.id);
        holds.add(o.id.sat.substr(0, 1) + "|" + o.id.sig1 + "+" + o.id.sig2, o.id.station, o.id.sat,
                  h != held_before.end() && h->second);
    }
    traces.live_record.noteHolds(t, std::move(holds));
}

}  // namespace

// ---- Setup ----

void networkRingFixArm(NetworkRun& run) {
    NetworkRunContext& ctx = run.ctx;
    const NetworkConfig& nc = run.nc;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (!nc.ring_fix.enable) return;
    auto rf = std::make_unique<NetworkRingFix>();
    rf->rule = nc.ring_fix.rule == "plain" ? RingFixRule::Plain
             : nc.ring_fix.rule == "log"   ? RingFixRule::Log
                                           : RingFixRule::Gated;
    rf->held_first = nc.ring_fix.held_first;
    rf->max_decisions_per_epoch = nc.ring_fix.max_decisions_per_epoch;
    rf->settled_flags_on = nc.ring_fix.settled_flags;
    if (rf->settled_flags_on)
        log->warn("Network (ring fix): SETTLED FLAGS: on every phase-bias record of a held family, signal_integer 1 "
                  "only while the satellite is reached from its datum receiver through held links and tree edges "
                  "that entered as held links, wide_lane_integer 1; both 0 on the other records.");
    rf->order = nc.ring_fix.order == "certain" ? RingFixOrder::Certain : RingFixOrder::Ring;
    rf->gate_rule = nc.ring_fix.gate == "odds" ? RingFixGateRule::Odds : RingFixGateRule::Sigma;
    rf->max_gap_s = ctx.knobs.pl_max_gap_s;
    rf->audit_file = nc.ring_fix.audit_frame_file;
    if (!rf->audit_file.empty()) rf->audit = ringFixReadAuditFile(rf->audit_file);
    log->warn("Network (ring fix): ARMED, rule {} ({}). Chords of pair families are decided ring by ring "
              "from the chain's root (gate: sigma at most {:.2f} cycle and fraction at most {:.2f} of the "
              "ionosphere-free N1 with the wide lane rounded; event window {:.0f} s; {} epochs of the same "
              "integers; the lattice vetoes), held by a prior row of sigma {:g} m from the next epoch, and "
              "released at a slip latch, a refloat, a gap beyond {:.0f} s, a chain event or the residual "
              "monitor ({:.3f} m at {} epochs). Audit: {}.",
              ringFixRuleName(rf->rule),
              rf->rule == RingFixRule::Plain ? "NEGATIVE CONTROL: every candidate rounded"
              : rf->rule == RingFixRule::Log ? "LOG ONLY: the frozen rule is traced, nothing is held"
                                             : "the frozen rule",
              kRingFixMaxSigmaCyc, kRingFixMaxFracCyc, kRingFixWindowS, kRingFixPersistEpochs,
              kRingFixHoldSigmaM, rf->max_gap_s, kRingFixMonitorBoundM, kRingFixMonitorEpochs,
              rf->audit_file.empty() ? std::string("none")
                                     : fmt::format("'{}' ({} arcs)", rf->audit_file, rf->audit.size()));
    // Stage N2, simulation only: the fixture's truth (the audit in every epoch's frame) and the oracle phase.
    rf->oracle_until_s = nc.ring_fix.oracle_until_s;
    if (!nc.ring_fix.truth_root.empty()) {
        auto T = std::make_unique<RingFixTruth>();
        const std::filesystem::path root(nc.ring_fix.truth_root);
        long long n_arcs = 0, n_slips = 0;
        for (std::size_t i = 0; i < run.setup.stations.size(); ++i) {
            const std::string& name = run.setup.stations[i].name;
            const auto amb_file = root / name / "truth_ambiguities.csv";
            const auto slip_file = root / name / "truth_slips.csv";
            const auto wu_file = root / name / "truth_windup.csv";
            for (const auto* f : {&amb_file, &slip_file, &wu_file})
                if (!std::filesystem::exists(*f))
                    throw std::runtime_error("network.ring_fix.truth_root: no truth file for roster station '" + name
                                             + "' at '" + f->string() + "' (simulation only).");
            const auto ambs = networkOraclePinParseAmbCsv(amb_file.string());
            const auto slips = networkSlipOracleParseCsv(slip_file.string());
            for (const auto& [key, nv] : ambs) {
                RingFixTruth::Arc a;
                a.n_int = nv.first;
                const auto sit = slips.find(key);
                if (sit != slips.end())
                    for (const auto& sl : sit->second) {
                        a.slips.emplace_back(sl.s, sl.delta_cyc);
                        ++n_slips;
                    }
                T->arcs[{static_cast<int>(i), key.first, key.second}] = std::move(a);
                ++n_arcs;
            }
            for (auto& [sat, series] : ringFixParseWindupCsv(wu_file.string()))
                T->windup[{static_cast<int>(i), sat}] = std::move(series);
        }
        log->warn("Network (ring fix): TRUTH from '{}' (simulation only): {} arcs, {} slips; every held integer is "
                  "audited in the frame of its epoch.", nc.ring_fix.truth_root, n_arcs, n_slips);
        rf->truth = std::move(T);
    }
    if (rf->oracle_until_s > 0.0)
        log->warn("Network (ring fix): ORACLE PHASE for the first {:.0f} s (simulation only): every chord with a true "
                  "integer is held at it; afterwards the rule alone decides.", rf->oracle_until_s);
    // The options of stage N1b, named only when armed (a run under the defaults logs what it logged before).
    if (rf->order == RingFixOrder::Certain)
        log->warn("Network (ring fix): order CERTAIN: every chord with a float is a candidate (rings reported, not "
                  "enforced); the links that pass are decided the highest log-odds of N1 first, {}.",
                  rf->max_decisions_per_epoch > 0 ? fmt::format("at most {} per epoch", rf->max_decisions_per_epoch)
                                                  : std::string("with no cap per epoch"));
    if (rf->gate_rule == RingFixGateRule::Odds)
        log->warn("Network (ring fix): gate ODDS: in place of the sigma bound, the log-odds of N1 and of the wide "
                  "lane's own float at least {:.4f} (ln 1e6), the fraction of N1 at most {:.2f}.",
                  kRingFixMinLogOdds, kRingFixMaxFracCyc);
    // Stage O1: the pair families considered and the wide lane's bound, named only when set.
    for (const std::string& e : nc.ring_fix.pairs)
        rf->pairs.insert({e.substr(0, 1), e.substr(2, 3), e.substr(6, 3)});
    rf->wl_frac_max = nc.ring_fix.wl_frac_max;
    if (!rf->pairs.empty()) {
        std::string list;
        for (const std::string& e : nc.ring_fix.pairs) list += (list.empty() ? "" : ", ") + e;
        log->warn("Network (ring fix): PAIRS {} only: a chord of another pair family is never decided, held or "
                  "traced (counted at run end).", list);
    }
    rf->settled_largest = rf->settled_flags_on && nc.ring_fix.settled_rule == "largest";
    rf->settled_wait = rf->settled_flags_on ? nc.ring_fix.settled_wait_emissions : 0;
    if (rf->settled_wait > 0)
        log->warn("Network (ring fix): SETTLED WAIT {}: a satellite the settled flags' rule puts in a family's set is "
                  "flagged only from its {}-th consecutive emission in the set.", rf->settled_wait,
                  rf->settled_wait + 1);
    if (rf->settled_largest)
        log->warn("Network (ring fix): SETTLED RULE LARGEST: the settled flags flag the largest set of a family's "
                  "satellites mutually tied through held links and certified tree edges, whatever receiver roots it, "
                  "once it holds {} satellites; the incumbent set is kept until another holds {} more.",
                  kRingFixSettledMinSats, kRingFixSettledHysteresis);
    rf->reattach = rf->settled_flags_on && nc.ring_fix.reattach;
    if (rf->reattach)
        log->warn("Network (ring fix): RE-ATTACHMENT OF UNTIED SATELLITES: a satellite of a family's tree unsettled "
                  "and without a tie for {:.0f} s, with at least {} candidate links in the flagged piece (window "
                  "passed, ionosphere-free sigma at most {:.2f} cycle) whose fractions agree (circular spreads at most "
                  "{:.2f} cycle ionosphere-free, {:.2f} wide lane) on a common offset (at least {:.2f} or {:.2f} "
                  "cycle), has its tree edge moved at the next election onto the candidate nearest their medians, in "
                  "every band of its family (an ordinary re-host); not again within {:.0f} s.",
                  kRingFixReattachDwellS, kRingFixReattachMinLinks, kRingFixMaxSigmaCyc, kRingFixReattachMaxSdIfCyc,
                  kRingFixReattachMaxSdWlCyc, kRingFixReattachMinOffsetIfCyc, kRingFixReattachMinOffsetWlCyc,
                  kRingFixReattachHoldoffS);
    rf->form_configured_pairs = nc.ring_fix.form_configured_pairs && !rf->pairs.empty();
    if (rf->form_configured_pairs) {
        for (const auto& [c, s1, s2] : rf->pairs) rf->configured_pairs[c[0]].emplace_back(s1, s2);
        log->warn("Network (ring fix): CONFIGURED FAMILIES FORMED: the chord table forms every family of the pairs "
                  "on an arc that tracks both its signals, beside the rule that pairs band 1 with the lowest other "
                  "band.");
    }
    for (const std::string& e : nc.ring_fix.cascade) {
        RingFixCascade c;
        c.letter = e[0];
        c.ck = e.substr(0, 1);
        c.s1 = e.substr(2, 3);
        c.s2 = e.substr(6, 3);
        c.s3 = e.substr(10, 3);
        c.ref = ringFixCascadeReference(std::string(1, c.letter) + "01", c.s1, c.s2, c.s3);
        c.name = e;
        if (c.ref.empty())
            throw std::runtime_error("network.ring_fix.cascade: '" + e + "' has a signal of unknown wavelength.");
        log->warn("Network (ring fix): CASCADE {}: on every link the pair holds, {}'s integer is decided against "
                  "{}'s held integer from their float difference (fraction within {:.2f} cycle, the odds gate, {} "
                  "epochs), then held; released with the pair, at a slip of {}, across a cut of its band, by the "
                  "monitor or after a gap.", e, c.s3, c.ref, kRingFixCascadeMaxFracCyc, kRingFixPersistEpochs, c.s3);
        rf->cascades.push_back(c);
    }
    rf->cascade_tree = nc.ring_fix.cascade_tree && !rf->cascades.empty();
    if (rf->cascade_tree)
        log->warn("Network (ring fix): CASCADE TREE: a cascade's third band grows its tree on its pair's forest: the "
                  "chain's walk takes first the tree edges of both of the pair's bands, then the links the pair holds, "
                  "then the band's own incumbents and new edges.");
    rf->cascade_rehost_keep = nc.ring_fix.cascade_rehost_keep && !rf->cascades.empty();
    if (rf->cascade_rehost_keep)
        log->warn("Network (ring fix): CASCADE RE-HOST KEEPS THE CARRY: a chain re-host of a third band onto a link the "
                  "cascade holds, whose carry sits on the held integer, takes it at a step of 0 and keeps the moved "
                  "side's carries; the held cascade links across it stay held.");
    rf->cascade_log = nc.ring_fix.cascade_log && !rf->cascades.empty();
    if (rf->cascade_log)
        log->warn("Network (ring fix): CASCADE LOG: the cascades decide and trace, never hold: a link that passes the "
                  "gate for the persistence is traced as would_hold at every such epoch, and its third signal stays "
                  "float.");
    rf->flags_per_family = rf->settled_flags_on && !rf->cascades.empty();
    if (rf->flags_per_family)
        log->warn("Network (ring fix): SETTLED FLAGS PER FAMILY: a satellite signal is flagged only while every family "
                  "it belongs to flags the satellite; a cascade's third signal has the cascade's own family.");
    if (rf->wl_frac_max > 0.0)
        log->warn("Network (ring fix): WIDE-LANE BOUND: a link every other test would decide is refused while its wide "
                  "lane's fraction exceeds {:.3f} cycle (refusal wl_fraction).", rf->wl_frac_max);
    // Stage O2: the odds gate's factor on the formal sigmas, named only when set.
    rf->sigma_scale = nc.ring_fix.sigma_scale;
    rf->code_event_window_s = nc.ring_fix.code_event_window_s;
    rf->rehost_keep_carry = nc.ring_fix.rehost_keep_carry;
    if (rf->rehost_keep_carry)
        log->warn("Network (ring fix): RE-HOST KEEPS THE CARRY: a chain re-host onto a held link whose carry sits "
                  "on the held integer (within 1e-4 cycle) takes it at a step of 0 and keeps the moved side's "
                  "carried entries, receiver and satellite phase biases instead of releasing them; a held link "
                  "whose carry is off its integer is released as before.");
    if (rf->code_event_window_s > 0.0)
        log->warn("Network (ring fix): CODE-EVENT WINDOW {:.0f} s: no decision on a satellite's links within that "
                  "time after its code reference moves or its code datum re-seeds (refusal code_event); links "
                  "already held are kept.", rf->code_event_window_s);
    if (rf->sigma_scale != 1.0)
        log->warn("Network (ring fix): SIGMA SCALE {:.2f}: the log-odds of N1 and of the wide lane are computed with "
                  "{:.2f} times their formal sigmas (the trace keeps the formal sigmas).", rf->sigma_scale,
                  rf->sigma_scale);
    // Stage N2c, simulation only: the slip mask.
    if (!nc.ring_fix.mask_slip_arc.empty()) {
        const std::string& arc = nc.ring_fix.mask_slip_arc;
        const std::string sta = arc.substr(0, arc.find(':'));
        rf->mask_sat = arc.substr(arc.find(':') + 1);
        for (std::size_t i = 0; i < run.setup.stations.size(); ++i)
            if (run.setup.stations[i].name == sta) rf->mask_station = static_cast<int>(i);
        if (rf->mask_station < 0)
            throw std::runtime_error("network.ring_fix.mask_slip_arc: no roster station '" + sta + "'.");
        rf->mask_tow = nc.ring_fix.mask_slip_tow;
        rf->mask_n_epochs = nc.ring_fix.mask_slip_epochs;
        log->warn("Network (ring fix): SLIP MASK (simulation only): the producer's slip flags of {} are masked for {} "
                  "epoch(s) from TOW {:.0f}: its carried entries are restored after the epoch's slip drains and it is "
                  "taken out of the free-epoch latch, the slip funnel and the bridge pends.",
                  arc, rf->mask_n_epochs, rf->mask_tow);
    }
    ctx.carry.ring_fix = std::move(rf);
}

// ---- Before PASS B: the releases and the hold table ----

void networkRingFixHoldEpoch(NetworkRun& run, const NetworkEpoch& ep) {
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const GnssConfig& config_ = run.config;
    const std::vector<NetStation>& stations = run.setup.stations;
    const SlipQcChannel& qc = run.engines.slip_qc;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    const RelevelEpochState& relevel = ep.events.slip.relevel;
    const std::vector<bool>& present = ep.pass_a.present;
    net::ArcTable& arc_rows = ctx.entities.arcs;   // written: the released links' carried entries
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix) return;
    NetworkRingFix& rf = *ctx.carry.ring_fix;
    rf.table.clear();
    const auto nameOf = [&](int i) {
        return (i >= 0 && static_cast<std::size_t>(i) < stations.size()) ? stations[static_cast<std::size_t>(i)].name
                                                                           : std::string{};
    };
    // The truth's arcs (simulation only): every (receiver, satellite) the slip channel modelled this
    // epoch, the rows network_slip_raw.csv writes.
    if (rf.truth && qc.epoch.computed) {
        for (std::size_t i = 0; i < qc.epoch.rows.size() && i < present.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& row : qc.epoch.rows[i])
                ringFixTruthObserve(*rf.truth, static_cast<int>(i), row.satellite, t.tow);
        }
    }
    const ChainFacts facts = chainFacts(obs_rec);
    const std::set<net::ArcTuple> latched = latchedArcs(relevel);
    const auto heldLinkOf = [&](int station, const std::string& sat, const std::string& sig) -> RingFixLink* {
        for (auto& [id, L] : rf.links)
            if (L.held && id.station == station && id.sat == sat && (id.sig1 == sig || id.sig2 == sig)) return &L;
        return nullptr;
    };
    const auto idOf = [&](const RingFixLink* p) -> const RingFixLinkId* {
        for (const auto& [id, L] : rf.links)
            if (&L == p) return &id;
        return nullptr;
    };

    // 1. The cuts of this epoch.  Each takes its whole moved side, the released vertex's subtree in this
    //    epoch's tree on the band: a release nested below another lists only its own part, and a link
    //    from the nested part to outside the outer side crosses both cuts (the steps add).  The step of
    //    a moved side is known where its released edge was held.
    std::vector<RingFixCut> cuts;
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
                if (v.rfind("S:", 0) == 0) c.sats.insert(v.substr(2));
                else if (v.rfind("R:", 0) == 0) c.stations.insert(std::stoi(v.substr(2)));
                if (gk == kids.end()) continue;
                const auto ch = gk->second.find(v);
                if (ch != gk->second.end()) stack.insert(stack.end(), ch->second.begin(), ch->second.end());
            }
            if (nested) ++rf.counters.cuts_nested;
            const RingFixLink* h = heldLinkOf(r.station, r.sat, r.signal);
            const RingFixLinkId* hid = h != nullptr ? idOf(h) : nullptr;
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
            c.known = ringFixCutStep(usable, n_held, target_cyc, r.sat_is_child, c.delta);
            cuts.push_back(std::move(c));
        }
    }

    // 2. The held links through the cuts: re-expressed by a known step, released across an unknown one.
    const long long now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const double elev_power = config_.getCodeElevationPower();
    // A released link's carried entry is re-opened when nothing re-levelled it since the decision, and
    // also when what did (the vehicle's drop at a slip or a refloat) left it tighter than the release
    // floor: an entry born under the hold (held from its first stash, or re-created after a cut erased
    // it) has the hold's own variance as its birth variance, which a drop restores.
    const auto reopen = [&](const RingFixLinkId& id, const std::string& sig, double stamp) {
        const net::ArcTuple key{id.station, id.sat, sig};
        LocalCarry* lc = net::getArc(arc_rows, key, &net::ArcState::amb);
        if (lc == nullptr) return;
        const bool stale = lc->relevel_s <= stamp;
        if (ctx.knobs.amb_float_kf) {
            if (!stale && !(lc->var < kRingFixReleaseFloorM2)) return;
            const double drop_base = ambFloatDropFloorNow(ctx.knobs.amb_drop_var_floor, now_ep,
                                                          ctx.knobs.amb_drop_floor_after_ep);
            const double birth_floor = ambFloatBirthFloor(ctx.knobs.amb_birth_var_floor, -1.0, elev_power);
            const double drop_floor = ambFloatBirthFloor(drop_base, -1.0, elev_power);
            if (std::max({lc->var_birth, lc->var_first, birth_floor, drop_floor}) < kRingFixReleaseFloorM2)
                ++rf.counters.release_floored;
            ambFloatDropFloored(*lc, birth_floor, std::max(drop_floor, kRingFixReleaseFloorM2), now_s, now_ep,
                                ctx.knobs.amb_drop_decouple_always);
            if (ctx.knobs.trace_amb_state)
                ambStateNoteDrop(ctx, t.week, t.tow, nameOf(id.station), id.sat, sig, *lc, "ring_fix_release");
        } else {
            if (!stale) return;
            net::dropArc(arc_rows, key, &net::ArcState::amb);
        }
        ++rf.counters.carry_reopened;
    };
    std::set<RingFixLinkId> usable_before;   // held, not latched, monitor not tripped
    for (const auto& [id, L] : rf.links)
        if (L.held && !L.monitor_trip && latched.count({id.station, id.sat, id.sig1}) == 0
            && latched.count({id.station, id.sat, id.sig2}) == 0)
            usable_before.insert(id);
    const RingFixCutOutcome cut_out = ringFixApplyCuts(rf, cuts, now_s);
    for (const auto& [id, row] : cut_out.released) {
        const RingFixLink& L = rf.links.at(id);
        if (usable_before.count(id) != 0) ++rf.counters.cut_released_usable;
        reopen(id, id.sig1, L.stamp1);
        reopen(id, id.sig2, L.stamp2);
        traces.emitEvent(t, "RINGFIX", "release", id.sat, nameOf(id.station), std::nullopt, std::nullopt, "chain",
                         fmt::format("sig1={} sig2={} n1={} n2={} ring={} held_s={:.0f} cut={} step=unknown",
                                     id.sig1, id.sig2, L.n1, L.n2, L.ring, now_s - L.decided_s, row));
    }
    for (const auto& [id, d1, d2, rows] : cut_out.reexpressed) {
        const RingFixLink& L = rf.links.at(id);
        traces.emitEvent(t, "RINGFIX", "reexpress", id.sat, nameOf(id.station), std::nullopt, std::nullopt, "cut",
                         fmt::format("sig1={} sig2={} n1={} n2={} d1={} d2={} cut={}", id.sig1, id.sig2, L.n1, L.n2,
                                     d1, d2, rows));
    }
    // The re-attachments through held links (network.ring_fix.held_first): one row per released edge the
    // walk took first, naming the link and the held links whose integers crossed its cuts unchanged.
    {
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
            const RingFixLink* h = heldLinkOf(entry.first, entry.second, cuts[ks.front()].signal);
            const RingFixLinkId* hid = h != nullptr ? idOf(h) : nullptr;
            traces.emitEvent(t, "RINGFIX", "reattach", entry.second, nameOf(entry.first), std::nullopt, std::nullopt,
                             "held_first",
                             fmt::format("sig1={} sig2={} n1={} n2={} steps={} kept={} cut={}",
                                         hid != nullptr ? hid->sig1 : std::string(),
                                         hid != nullptr ? hid->sig2 : std::string(), h != nullptr ? h->n1 : 0,
                                         h != nullptr ? h->n2 : 0, steps, kept.size(), cuts[ks.front()].row));
        }
    }

    // 3. A held link that became a tree edge leaves the held set: the chain pins it.
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        if (facts.tree.count({id.station, id.sat, id.sig1}) == 0 && facts.tree.count({id.station, id.sat, id.sig2}) == 0)
            continue;
        L.held = false;
        L.streak = 0;
        L.monitor_bad = 0;
        L.monitor_trip = false;
        L.rebase_stamps = false;
        ++rf.counters.to_tree;
        if (rf.settled_flags_on) rf.certified_edges.insert({id.station, id.sat, id.sig1});
        traces.emitEvent(t, "RINGFIX", "to_tree", id.sat, nameOf(id.station), std::nullopt, std::nullopt, "chain",
                         fmt::format("sig1={} sig2={} n1={} n2={}", id.sig1, id.sig2, L.n1, L.n2));
    }

    // 4. A root handover the root row pinned at the carried receiver phase bias keeps the frame.
    if (obs_rec != nullptr && obs_rec->armed && !obs_rec->roots.empty()) {
        for (auto& [id, L] : rf.links) {
            if (!L.held || L.root.empty()) continue;
            const std::string g = groupOf(id.sat, id.sig1);
            const std::string now_root = facts.root(g, rxKey(id.station));
            if (now_root.empty() || now_root == L.root) continue;
            for (const ChainRootObs& ro : obs_rec->roots)
                if (ro.group == g && ro.new_root == now_root && ro.outcome == ChainRootOutcome::Pinned) {
                    L.root = now_root;
                    ++rf.counters.root_kept;
                    break;
                }
        }
    }

    // 5. The release step.
    {
        std::map<RingFixLinkId, RingFixHoldObs> obs;
        for (const auto& [id, L] : rf.links) {
            if (!L.held) continue;
            RingFixHoldObs o;
            o.tree_now = facts.tree.count({id.station, id.sat, id.sig1}) > 0
                      || facts.tree.count({id.station, id.sat, id.sig2}) > 0;
            o.present = facts.chords.count({id.station, id.sat, id.sig1}) > 0
                     && facts.chords.count({id.station, id.sat, id.sig2}) > 0;
            o.in_cut = false;   // the cuts were applied above
            o.root = facts.root(groupOf(id.sat, id.sig1), rxKey(id.station));
            o.latched = latched.count({id.station, id.sat, id.sig1}) > 0
                     || latched.count({id.station, id.sat, id.sig2}) > 0;
            const LocalCarry* c1 = net::getArc(arc_rows, net::ArcTuple{id.station, id.sat, id.sig1}, &net::ArcState::amb);
            const LocalCarry* c2 = net::getArc(arc_rows, net::ArcTuple{id.station, id.sat, id.sig2}, &net::ArcState::amb);
            o.carry_ok = c1 != nullptr && c2 != nullptr;
            o.stamp1 = c1 != nullptr ? c1->relevel_s : 0.0;
            o.stamp2 = c2 != nullptr ? c2->relevel_s : 0.0;
            obs[id] = o;
        }
        const std::vector<RingFixEvent> events = ringFixReleaseStep(rf, obs, now_s);
        // Every cause re-opens the carried entries: a slip's and a refloat's drop already ran (the
        // floor still binds where it left the entry tighter); the other causes re-open them here,
        // unless something re-levelled them since the decision.
        for (const RingFixEvent& ev : events) {
            const RingFixLink& L = rf.links.at(ev.id);
            reopen(ev.id, ev.id.sig1, L.stamp1);
            reopen(ev.id, ev.id.sig2, L.stamp2);
            traces.emitEvent(t, "RINGFIX", "release", ev.id.sat, nameOf(ev.id.station), std::nullopt, std::nullopt,
                             ringFixReleaseName(ev.release),
                             fmt::format("sig1={} sig2={} n1={} n2={} ring={} held_s={:.0f}", ev.id.sig1,
                                         ev.id.sig2, L.n1, L.n2, L.ring, now_s - L.decided_s));
        }
    }

    // 5b. The cascade's releases (network.ring_fix.cascade): with its pair, at a slip of its third signal, across a
    //     cut of the third band, by its monitor, after a gap.  Each re-opens the third signal's carried entry.
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
        reopen(id, c->s3, CL.stamp3);
        ++rf.cascade_counters[c->name].released[cause];
        traces.emitEvent(t, "RINGFIX", "cascade_release", id.sat, nameOf(id.station), std::nullopt, std::nullopt,
                         cause.c_str(), fmt::format("cascade={} ref={} n_ref={} n3={} held_s={:.0f}", c->name, c->ref,
                                            CL.n_ref, CL.n3, now_s - CL.decided_s));
    }

    // 6. The old hosts: last epoch's tree edges that are chords of their pair family now.
    if (!rf.prev_targets.empty() && obs_rec != nullptr && obs_rec->armed) {
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
                const std::string g = groupOf(sat, s1);
                c.ring = facts.ring(g, rxKey(sta), svKey(sat));
                c.root = facts.root(g, rxKey(sta));
                if (!(c.lambda1 > 0.0 && c.lambda2 > c.lambda1)) continue;
                cands.push_back(c);
            }
        }
        for (const RingFixEvent& ev : ringFixInherit(rf, cands, cuts, now_s))
            traces.emitEvent(t, "RINGFIX", "hold", ev.id.sat, nameOf(ev.id.station), std::nullopt, std::nullopt,
                             "inherit",
                             fmt::format("sig1={} sig2={} ring={} n1={} n2={} wl={} old_host=1", ev.id.sig1,
                                         ev.id.sig2, rf.links.at(ev.id).ring, ev.gate.n1, ev.gate.n2, ev.gate.wl));
    }

    // 7. This epoch's targets, for the next epoch's old hosts.
    if (obs_rec != nullptr && obs_rec->armed) {
        rf.prev_targets.clear();
        for (const ChainEdgeObs& e : obs_rec->edges)
            if (e.lambda_m > 0.0) rf.prev_targets[{e.station, e.sat, e.signal}] = e.target_m / e.lambda_m;
    }

    // 8. The hold table PASS B folds.
    for (const auto& [id, L] : rf.links) {
        if (!L.held) continue;
        rf.table[{id.station, id.sat, id.sig1}] = L.lambda1 * static_cast<double>(L.n1);
        rf.table[{id.station, id.sat, id.sig2}] = L.lambda2 * static_cast<double>(L.n2);
    }
    for (const auto& [id, CL] : rf.cascade_links)
        if (CL.held) rf.table[{id.station, id.sat, id.sig2}] = CL.lambda3 * static_cast<double>(CL.n3);
}

// ---- PASS B: the hold rows ----

void networkRingFixPriors(StationCarryFrame& carry) {
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    const NetworkSetupResult& setup = build.setup;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const Eigen::VectorXd& x_lin = carry.x_lin;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;
    const auto& stations = setup.stations;
    // This epoch's hold table, built serially before PASS B and only read here; null on every
    // run without the ring fix, so this step is unreachable when off.
    const std::map<std::tuple<int, std::string, std::string>, double>* table =
        ctx.carry.ring_fix ? &ctx.carry.ring_fix->table : nullptr;
    const bool rfix_on = table != nullptr && !table->empty();
    // ---- End of the bindings ----
    if (!(legacy_local_inject && rfix_on)) return;
    const double lam = 1.0 / (kRingFixHoldSigmaM * kRingFixHoldSigmaM);
    for (const auto& [arc, amb_idx] : st.params.amb) {
        const std::string arc_sat = net::satKey(arc.sat);
        const std::string arc_signal = net::signalKey(arc.signal);
        const auto it = table->find(std::make_tuple(i, arc_sat, arc_signal));
        if (it == table->end()) continue;
        ++R.rfix_offered;
        // A tree edge's 1e10 pin is the frame: a second row there would fight it.  The release
        // step releases a link that became a tree edge before PASS B; this is the backstop.
        if (stations[static_cast<std::size_t>(i)].pinned_amb_targets.count(
                StateKeys::makeAmbiguityKey(arc_sat, arc_signal)) != 0) {
            ++R.rfix_gauge_skipped;
            continue;
        }
        if (amb_idx >= x_lin.size()) { ++R.rfix_no_column; continue; }
        const auto hit = h2local.find(amb_idx);
        if (hit == h2local.end() || !std::isfinite(x_lin(amb_idx))) { ++R.rfix_no_column; continue; }
        const double eta = lam * (it->second - x_lin(amb_idx));
        if (!std::isfinite(eta)) { ++R.rfix_no_column; continue; }
        inj.emplace_back(static_cast<Eigen::Index>(hit->second), lam, eta,
                         makeRowTag(ContributionKind::PriorAmbiguityRingHold, arc_sat, arc_signal));
        ++R.rfix_rows;
    }
}

void networkRingFixStationTally(NetworkRunContext& ctx, const std::vector<StationAssembly>& asm_res) {
    if (!ctx.carry.ring_fix) return;
    RingFixCounters& C = ctx.carry.ring_fix->counters;
    for (const StationAssembly& Rc : asm_res) {
        C.rows_injected      += Rc.rfix_rows;
        C.rows_gauge_skipped += Rc.rfix_gauge_skipped;
        C.rows_no_column     += Rc.rfix_no_column;
    }
}

// ---- After the solve: the monitor, the audit and the decisions ----

void networkRingFixDecide(NetworkRun& run, const NetworkEpoch& ep) {
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    const RelevelEpochState& relevel = ep.events.slip.relevel;
    const net::ArcTable& arc_rows = ctx.entities.arcs;
    const net::SatTable& sat_rows = ctx.entities.sats;
    const arcgraph::CycleLattice& lattice = ctx.carry.ar_lattice.lattice;
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix || obs_rec == nullptr || !obs_rec->armed) return;
    NetworkRingFix& rf = *ctx.carry.ring_fix;
    const ChainEpochObs& obs = *obs_rec;
    if (rf.reattach) reportUntiedOutcomes(rf, stations, now_s, t, traces);

    // The chain this epoch: tree arcs, groups, each group's tree and chords, the roots.
    std::set<net::ArcTuple> tree_arcs;
    std::set<std::string> chain_signals;
    std::map<std::string, RingFixTree> trees;
    std::map<std::string, std::map<std::string, std::string>> root_of;
    for (const ChainEdgeObs& e : obs.edges) {
        tree_arcs.insert({e.station, e.sat, e.signal});
        chain_signals.insert(e.sys + "|" + e.signal);
        trees[e.group].edges.emplace_back(e.child, e.parent, e.depth);
        root_of[e.group][e.child] = e.r1;
        root_of[e.group][e.parent] = e.r1;
        rf.last_tree_s[{e.station, e.sat}] = now_s;
    }
    std::map<net::ArcTuple, const ChainChordCycle*> cyc_of;
    for (const ChainChordCycle& c : obs.chords) {
        cyc_of[{c.station, c.sat, c.signal}] = &c;
        trees[groupOf(c.sat, c.signal)].chords.emplace_back(rxKey(c.station), svKey(c.sat));
    }
    std::map<std::string, std::set<std::pair<std::string, std::string>>> held_by_group;
    for (const auto& [id, L] : rf.links)
        if (L.held) held_by_group[groupOf(id.sat, id.sig1)].insert({rxKey(id.station), svKey(id.sat)});
    std::map<std::string, std::map<std::pair<std::string, std::string>, RingFixRingInfo>> rings;
    for (const auto& [g, tree] : trees) rings[g] = ringFixRings(tree, held_by_group[g]);
    const std::set<net::ArcTuple> latched = latchedArcs(relevel);

    // Every pair-family chord with its readings.
    std::vector<RingFixDecideObs> links;
    std::map<RingFixLinkId, RingFixGate> gate_of;   // the trace's gate reading of every link with a float
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const int ii = static_cast<int>(i);
        const StationAssembly& R = asm_res[i];
        const ChainAmbColumns cols = chainAmbColumns(R);
        std::vector<ChainChordRow> rows = chainStationChordRows(ii, stations[i].name, cols, tree_arcs, chain_signals,
                                                                rf.form_configured_pairs ? &rf.configured_pairs : nullptr);
        if (rows.empty()) continue;
        std::vector<char> filled;
        Eigen::VectorXd xr;
        const bool station_ok = chainChordFloats(est, static_cast<std::size_t>(est_index_of[i]), x_s,
                                                 plan.shared_datum, cols, rows, filled, &xr);
        Eigen::VectorXd resid;
        bool resid_formed = false;
        for (std::size_t r_i = 0; r_i < rows.size(); ++r_i) {
            const ChainChordRow& r = rows[r_i];
            if (!rf.pairConsidered(constellationKey(systemOfSat(r.sat)), r.sig1, r.sig2)) {
                ++rf.counters.excluded_link_epochs;   // never decided, held or traced
                continue;
            }
            RingFixDecideObs o;
            o.id = {ii, r.sat, r.sig1, r.sig2};
            o.lambda1 = chainWavelengthM(r.sat, r.sig1);
            o.lambda2 = chainWavelengthM(r.sat, r.sig2);
            o.have_float = station_ok && filled[r_i] != 0;
            if (o.have_float) {
                o.f1 = r.float_cyc_1; o.f2 = r.float_cyc_2;
                o.q11 = r.q11; o.q12 = r.q12; o.q22 = r.q22;
                gate_of[o.id] = ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2,
                                            rf.sigma_scale);
            }
            const std::string g = groupOf(r.sat, r.sig1);
            const auto rg = rings.find(g);
            if (rg != rings.end()) {
                const auto ri = rg->second.find({rxKey(ii), svKey(r.sat)});
                if (ri != rg->second.end()) { o.ring = ri->second.ring; o.candidate = ri->second.candidate; }
            }
            const auto rt = root_of.find(g);
            if (rt != root_of.end()) {
                const auto v = rt->second.find(rxKey(ii));
                if (v != rt->second.end()) o.root = v->second;
            }
            const auto cy1 = cyc_of.find({ii, r.sat, r.sig1});
            if (cy1 != cyc_of.end()) o.have_lat1 = chainLatticeChordInteger(lattice, *cy1->second, o.lat1);
            const auto cy2 = cyc_of.find({ii, r.sat, r.sig2});
            if (cy2 != cyc_of.end()) o.have_lat2 = chainLatticeChordInteger(lattice, *cy2->second, o.lat2);
            if (rf.truth) {
                long long n1 = 0, n2 = 0;
                const auto trueOf = [&](const std::string& sig, long long& n) {
                    const auto cy = cyc_of.find({ii, r.sat, sig});
                    if (cy == cyc_of.end()) return false;
                    std::vector<std::tuple<int, std::string, std::string, int>> terms;
                    for (const arcgraph::CycleTerm& term : cy->second->cycle.terms)
                        terms.emplace_back(term.arc.station.index, term.arc.sat.str(), sig, term.coeff);
                    return ringFixTruthFrameInteger(*rf.truth, terms, cy->second->pin_sum_cyc,
                                                    cy->second->pins_integer, now_s, t.tow, n);
                };
                const bool h1 = trueOf(r.sig1, n1), h2 = trueOf(r.sig2, n2);
                o.have_truth = h1 && h2;
                o.truth1 = n1;
                o.truth2 = n2;
                if (!rf.audit.empty()) {
                    const auto check = [&](bool have, const std::string& sig, long long n) {
                        if (!have) return;
                        const auto f = rf.audit.find({stations[i].name, r.sat, sig});
                        if (f == rf.audit.end()) return;
                        ++rf.counters.truth_file_checked;
                        if (f->second != n) ++rf.counters.truth_file_mismatch;
                    };
                    check(h1, r.sig1, n1);
                    check(h2, r.sig2, n2);
                }
            }
            const LocalCarry* lc1 = net::getArc(arc_rows, net::ArcTuple{ii, r.sat, r.sig1}, &net::ArcState::amb);
            const LocalCarry* lc2 = net::getArc(arc_rows, net::ArcTuple{ii, r.sat, r.sig2}, &net::ArcState::amb);
            o.carry_ok = lc1 != nullptr && lc2 != nullptr;
            o.relevel1_s = lc1 != nullptr ? lc1->relevel_s : 0.0;
            o.relevel2_s = lc2 != nullptr ? lc2->relevel_s : 0.0;
            o.latched = latched.count({ii, r.sat, r.sig1}) > 0 || latched.count({ii, r.sat, r.sig2}) > 0;
            if (const double* ce = net::getSat(sat_rows, r.sat, &net::SatelliteState::code_ref_event_s))
                o.code_event_s = *ce;
            const auto held_it = rf.links.find(o.id);
            if (held_it != rf.links.end() && held_it->second.held && station_ok
                && R.A_s.cols() == x_s.size() && xr.size() == R.A_r.cols()) {
                if (!resid_formed) {
                    resid = R.y - R.A_s * x_s - R.A_r * xr;   // post-fit O - C
                    resid_formed = true;
                }
                const std::optional<double> r1 = phaseResidual(R, resid, cols.at({r.sat, r.sig1}).first);
                const std::optional<double> r2 = phaseResidual(R, resid, cols.at({r.sat, r.sig2}).first);
                if (r1 && r2 && o.lambda2 > o.lambda1 && o.lambda1 > 0.0) {
                    const double l1s = o.lambda1 * o.lambda1, l2s = o.lambda2 * o.lambda2;
                    o.have_resid = true;
                    o.resid_if_m = (l2s * *r1 - l1s * *r2) / (l2s - l1s);
                }
            }
            links.push_back(o);
        }
    }

    // The audit of every link held in this epoch's solve: against truth in this epoch's frame when it is
    // armed (simulation only), and against the audit file.
    std::map<RingFixLinkId, RingFixAudit> truth_of;
    if (rf.truth) {
        std::map<RingFixLinkId, const RingFixDecideObs*> obs_of;
        for (const RingFixDecideObs& o : links) obs_of[o.id] = &o;
        for (auto& [id, L] : rf.links) {
            if (!L.held) continue;
            const auto o = obs_of.find(id);
            if (o == obs_of.end() || !o->second->have_truth) { ++rf.counters.truth_none; continue; }
            const bool right = L.n1 == o->second->truth1 && L.n2 == o->second->truth2;
            truth_of[id] = right ? RingFixAudit::Right : RingFixAudit::Wrong;
            if (right) ++rf.counters.truth_right;
            else ++rf.counters.truth_wrong;
        }
    }
    std::map<RingFixLinkId, RingFixAudit> audit_of;
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        const std::string name = (id.station >= 0 && static_cast<std::size_t>(id.station) < stations.size())
                                     ? stations[static_cast<std::size_t>(id.station)].name : std::string{};
        const RingFixAudit a = rf.audit.empty() ? RingFixAudit::Unlisted
                                                : ringFixAuditLink(rf.audit, name, id, L.n1, L.n2);
        audit_of[id] = a;
        L.wrong_now = a == RingFixAudit::Wrong;
        if (a == RingFixAudit::Right) ++rf.counters.audit_right;
        else if (a == RingFixAudit::Wrong) ++rf.counters.audit_wrong;
        else ++rf.counters.audit_unlisted;
    }
    std::map<RingFixLinkId, bool> held_before;
    for (const RingFixDecideObs& o : links) {
        const auto it = rf.links.find(o.id);
        held_before[o.id] = it != rf.links.end() && it->second.held;
    }
    noteLiveHolds(traces, t, links, held_before);

    const std::vector<RingFixEvent> events = ringFixDecideStep(rf, links, now_s);
    std::map<RingFixLinkId, const RingFixEvent*> event_of;
    for (const RingFixEvent& ev : events) event_of[ev.id] = &ev;
    for (const RingFixEvent& ev : events) {
        if (ev.kind != RingFixEvent::Kind::Hold) continue;
        const std::string name = (ev.id.station >= 0 && static_cast<std::size_t>(ev.id.station) < stations.size())
                                     ? stations[static_cast<std::size_t>(ev.id.station)].name : std::string{};
        const RingFixAudit a = rf.audit.empty() ? RingFixAudit::Unlisted
                                                : ringFixAuditLink(rf.audit, name, ev.id, ev.gate.n1, ev.gate.n2);
        if (!ev.log_only) {
            if (a == RingFixAudit::Wrong) ++rf.counters.decided_wrong;
            else if (a == RingFixAudit::Right) ++rf.counters.decided_right;
        }
        std::string truth_tok;
        if (rf.truth) {
            for (const RingFixDecideObs& o : links)
                if (o.id < ev.id || ev.id < o.id) continue;
                else {
                    const bool off = o.have_truth && (o.truth1 != ev.gate.n1 || o.truth2 != ev.gate.n2);
                    if (ev.log_only) {
                        if (o.have_truth) ++(off ? rf.counters.log_truth_wrong : rf.counters.log_truth_right);
                    } else if (off) {
                        ++rf.counters.decided_truth_wrong;
                    }
                    truth_tok = o.have_truth ? ((o.truth1 == ev.gate.n1 && o.truth2 == ev.gate.n2) ? " truth=right"
                                                                                                 : " truth=WRONG")
                                             : " truth=none";
                    break;
                }
        }
        traces.emitEvent(t, "RINGFIX", ev.log_only ? "would_hold" : "hold", ev.id.sat, name, ev.gate.sigma_if,
                         kRingFixMaxSigmaCyc, ev.oracle ? "oracle" : "decide",
                         fmt::format("sig1={} sig2={} ring={} n1={} n2={} wl={} frac_if={:.3f} sigma_if={:.3f} "
                                     "frac_wl={:.3f} rule={} audit={}",
                                     ev.id.sig1, ev.id.sig2, rf.links.at(ev.id).ring, ev.gate.n1, ev.gate.n2,
                                     ev.gate.wl, ev.gate.frac_if, ev.gate.sigma_if, ev.gate.frac_wl,
                                     ringFixRuleName(rf.rule), auditName(a))
                             + (rf.order == RingFixOrder::Certain ? " order=certain" : "")
                             + (rf.gate_rule == RingFixGateRule::Odds
                                    ? fmt::format(" gate=odds log_odds_if={:.2f} log_odds_wl={:.2f}",
                                                  ev.gate.log_odds_if, ev.gate.log_odds_wl)
                                    : std::string())
                             + truth_tok);
    }

    // The cascade (network.ring_fix.cascade): on every link the pair held in this epoch's solve, the third signal's
    // integer from the reference's held integer and their float difference; the held links' monitor and audit.
    struct CascadeTraceRow {
        RingFixLinkId id;
        std::string state, cause, audit;
        bool have_float{false};
        double f_ref{0.0}, f3{0.0};
        RingFixCascadeGate gate;
        long long n_ref{0}, n3{0};
        bool have_n3{false}, have_resid{false};
        double resid_m{0.0};
        int streak{0};
    };
    std::vector<CascadeTraceRow> cascade_trace;
    std::set<RingFixLinkId> cascade_seen;
    if (!rf.cascades.empty()) {
        const auto trueOf3 = [&](int sta, const std::string& sat, const std::string& sig, long long& n) {
            const auto cy = cyc_of.find({sta, sat, sig});
            if (!rf.truth || cy == cyc_of.end()) return false;
            std::vector<std::tuple<int, std::string, std::string, int>> terms;
            for (const arcgraph::CycleTerm& term : cy->second->cycle.terms)
                terms.emplace_back(term.arc.station.index, term.arc.sat.str(), sig, term.coeff);
            return ringFixTruthFrameInteger(*rf.truth, terms, cy->second->pin_sum_cyc, cy->second->pins_integer,
                                            now_s, t.tow, n);
        };
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const int ii = static_cast<int>(i);
            const StationAssembly& R = asm_res[i];
            const ChainAmbColumns cols = chainAmbColumns(R);
            std::vector<ChainChordRow> rows;
            std::vector<std::pair<const RingFixCascade*, RingFixLinkId>> meta;
            for (const RingFixCascade& c : rf.cascades)
                for (const auto& [pid, P] : rf.links) {
                    if (pid.station != ii || pid.sig1 != c.s1 || pid.sig2 != c.s2 || !P.held) continue;
                    if (pid.sat.empty() || pid.sat[0] != c.letter) continue;
                    const auto hb = held_before.find(pid);
                    if (hb == held_before.end() || !hb->second) continue;   // held in this epoch's solve
                    if (cols.count({pid.sat, c.s3}) == 0 || cols.count({pid.sat, c.ref}) == 0) continue;
                    if (tree_arcs.count({ii, pid.sat, c.s3}) != 0) continue;   // a tree edge: the chain pins it
                    ChainChordRow r;
                    r.family = c.name;
                    r.sig1 = c.ref;
                    r.sig2 = c.s3;
                    r.station = ii;
                    r.station_name = stations[i].name;
                    r.sat = pid.sat;
                    rows.push_back(r);
                    meta.emplace_back(&c, RingFixLinkId{ii, pid.sat, c.ref, c.s3});
                }
            if (rows.empty()) continue;
            std::vector<char> filled;
            Eigen::VectorXd xr;
            const bool ok = chainChordFloats(est, static_cast<std::size_t>(est_index_of[i]), x_s, plan.shared_datum,
                                             cols, rows, filled, &xr);
            Eigen::VectorXd resid;
            bool resid_formed = false;
            for (std::size_t k = 0; k < rows.size(); ++k) {
                const RingFixCascade& c = *meta[k].first;
                const RingFixLinkId& id = meta[k].second;
                cascade_seen.insert(id);
                RingFixCascadeLink& CL = rf.cascade_links[id];
                RingFixCascadeCounters& CC = rf.cascade_counters[c.name];
                const RingFixLink& P = rf.links.at(RingFixLinkId{ii, id.sat, c.s1, c.s2});
                CascadeTraceRow tr;
                tr.id = id;
                tr.n_ref = c.ref == c.s1 ? P.n1 : P.n2;
                CL.absent_since_s = -1.0;
                long long truth3 = 0;
                const bool have_truth = trueOf3(ii, id.sat, c.s3, truth3);
                tr.have_float = ok && filled[k] != 0;
                if (tr.have_float) {
                    tr.f_ref = rows[k].float_cyc_1;
                    tr.f3 = rows[k].float_cyc_2;
                    tr.gate = ringFixCascadeGate(tr.f_ref, tr.f3, rows[k].q11, rows[k].q12, rows[k].q22, rf.sigma_scale);
                }
                if (CL.held) {
                    ++CC.held_link_epochs;
                    tr.state = "held";
                    tr.n3 = CL.n3;
                    tr.have_n3 = true;
                    if (ok && R.A_s.cols() == x_s.size() && xr.size() == R.A_r.cols()) {
                        if (!resid_formed) {
                            resid = R.y - R.A_s * x_s - R.A_r * xr;   // post-fit O - C
                            resid_formed = true;
                        }
                        const std::optional<double> r3 = phaseResidual(R, resid, cols.at({id.sat, c.s3}).first);
                        if (r3) {
                            tr.have_resid = true;
                            tr.resid_m = *r3;
                            if (std::abs(*r3) > kRingFixMonitorBoundM) {
                                if (++CL.monitor_bad >= kRingFixMonitorEpochs) CL.monitor_trip = true;
                            } else {
                                CL.monitor_bad = 0;
                            }
                        }
                    }
                    if (have_truth) {
                        ++(truth3 == CL.n3 ? CC.truth_right : CC.truth_wrong);
                        tr.audit = truth3 == CL.n3 ? "right" : "WRONG";
                    }
                } else if (!tr.have_float || !tr.gate.valid) {
                    CL.streak = 0;
                    ++CC.refused_no_float;
                    tr.state = "refused";
                    tr.cause = "no_float";
                } else if (!tr.gate.pass) {
                    CL.streak = 0;
                    const bool frac_out = tr.gate.frac > kRingFixCascadeMaxFracCyc;
                    ++(frac_out ? CC.refused_fraction : CC.refused_odds);
                    tr.state = "refused";
                    tr.cause = frac_out ? "fraction" : "odds";
                } else {
                    const long long n3 = tr.n_ref - tr.gate.d;
                    if (CL.streak > 0 && CL.streak_n3 == n3) ++CL.streak;
                    else { CL.streak = 1; CL.streak_n3 = n3; }
                    tr.n3 = n3;
                    tr.have_n3 = true;
                    if (CL.streak >= kRingFixPersistEpochs && rf.cascade_log) {
                        // The log mode: the decision is traced at every epoch it stands, never held.
                        ++CC.would_hold;
                        if (have_truth && truth3 != n3) ++CC.would_truth_wrong;
                        tr.state = "would_hold";
                        if (have_truth) tr.audit = truth3 == n3 ? "right" : "WRONG";
                    } else if (CL.streak >= kRingFixPersistEpochs) {
                        CL.held = true;
                        CL.n3 = n3;
                        CL.n_ref = tr.n_ref;
                        CL.lambda3 = chainWavelengthM(id.sat, c.s3);
                        CL.decided_s = now_s;
                        CL.streak = 0;
                        CL.monitor_bad = 0;
                        CL.monitor_trip = false;
                        const LocalCarry* lc3 = net::getArc(arc_rows, net::ArcTuple{ii, id.sat, c.s3}, &net::ArcState::amb);
                        CL.stamp3 = lc3 != nullptr ? lc3->relevel_s : 0.0;
                        ++CC.decided;
                        if (have_truth && truth3 != n3) ++CC.decided_truth_wrong;
                        tr.state = "decided";
                        if (have_truth) tr.audit = truth3 == n3 ? "right" : "WRONG";
                        traces.emitEvent(t, "RINGFIX", "cascade_hold", id.sat, stations[i].name, tr.gate.sigma,
                                         kRingFixMaxSigmaCyc, "cascade",
                                         fmt::format("cascade={} ref={} n_ref={} n3={} d={} frac={:.3f} sigma={:.3f} "
                                                     "log_odds={:.2f}{}", c.name, c.ref, tr.n_ref, n3, tr.gate.d,
                                                     tr.gate.frac, tr.gate.sigma, tr.gate.log_odds,
                                                     have_truth ? (truth3 == n3 ? " truth=right" : " truth=WRONG")
                                                                : std::string()));
                    } else {
                        ++CC.refused_persist;
                        tr.state = "refused";
                        tr.cause = "persist";
                    }
                }
                tr.streak = CL.streak;
                cascade_trace.push_back(std::move(tr));
            }
        }
        // Held cascade links not read this epoch: a tree edge of the third band leaves the held set (the chain pins it),
        // any other is absent (its gap runs).
        for (auto& [id, CL] : rf.cascade_links) {
            if (!CL.held || cascade_seen.count(id) != 0) continue;
            const RingFixCascade* c = rf.cascadeOf(id);
            if (c == nullptr) continue;
            if (tree_arcs.count({id.station, id.sat, id.sig2}) != 0) {
                CL.held = false;
                ++rf.cascade_counters[c->name].to_tree;
                if (rf.settled_flags_on) rf.certified_edges.insert({id.station, id.sat, id.sig2});
                traces.emitEvent(t, "RINGFIX", "cascade_to_tree", id.sat, stations[static_cast<std::size_t>(id.station)].name,
                                 std::nullopt, std::nullopt, "chain",
                                 fmt::format("cascade={} n3={}", c->name, CL.n3));
                continue;
            }
            if (CL.absent_since_s < 0.0) CL.absent_since_s = now_s;
        }
    }

    // network_ring_fix.csv: one row per pair-family chord.
    if (traces.ring_fix_trace.is_open()) {
        for (const RingFixDecideObs& o : links) {
            const RingFixLink& L = rf.links.at(o.id);
            const auto ev = event_of.find(o.id);
            const char* state = "refused";
            std::string cause;
            if (held_before[o.id]) { state = "held"; }
            else if (ev != event_of.end() && ev->second->kind == RingFixEvent::Kind::Hold) {
                state = ev->second->log_only ? "would_hold" : "decided";
            }
            else if (ev != event_of.end()) { cause = ringFixRefuseName(ev->second->refuse); }
            const auto g = gate_of.find(o.id);
            const RingFixGate gr = g != gate_of.end() ? g->second : RingFixGate{};
            const auto au = audit_of.find(o.id);
            const auto tr = truth_of.find(o.id);
            std::string audit;
            if (tr != truth_of.end()) audit = auditName(tr->second);
            else if (au != audit_of.end()) audit = auditName(au->second);
            else if (state[0] == 'd' && !rf.audit.empty())
                audit = auditName(ringFixAuditLink(rf.audit, stations[static_cast<std::size_t>(o.id.station)].name,
                                                   o.id, L.n1, L.n2));
            traces.ring_fix_trace
                << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
                << stations[static_cast<std::size_t>(o.id.station)].name << ',' << o.id.sat << ',' << o.id.sig1
                << ',' << o.id.sig2 << ',' << o.ring << ','
                << ((rf.order == RingFixOrder::Certain ? o.have_float : o.candidate) ? 1 : 0) << ',' << state << ','
                << cause << ',' << (o.have_float ? cellOf(o.f1, 4) : std::string()) << ','
                << (o.have_float ? cellOf(o.f2, 4) : std::string()) << ','
                << (gr.valid ? cellOf(gr.n_if, 4) : std::string()) << ','
                << (gr.valid ? cellOf(gr.sigma_if, 4) : std::string()) << ','
                << (gr.valid ? cellOf(gr.frac_if, 4) : std::string()) << ','
                << (gr.valid ? std::to_string(gr.wl) : std::string()) << ','
                << (gr.valid ? cellOf(gr.frac_wl, 4) : std::string()) << ','
                << ((L.held || gr.valid) ? std::to_string(L.held ? L.n1 : gr.n1) : std::string()) << ','
                << ((L.held || gr.valid) ? std::to_string(L.held ? L.n2 : gr.n2) : std::string()) << ','
                << (o.have_lat1 ? std::to_string(o.lat1) : std::string()) << ','
                << (o.have_lat2 ? std::to_string(o.lat2) : std::string()) << ','
                << (o.have_resid ? cellOf(o.resid_if_m, 5) : std::string()) << ',' << L.streak << ','
                << audit << '\n';
        }
        for (const CascadeTraceRow& tr : cascade_trace) {
            traces.ring_fix_trace
                << t.week << ',' << fmt::format("{:.3f}", t.tow) << ','
                << stations[static_cast<std::size_t>(tr.id.station)].name << ',' << tr.id.sat << ',' << tr.id.sig1
                << ',' << tr.id.sig2 << ",-1,1," << tr.state << ',' << tr.cause << ','
                << (tr.have_float ? cellOf(tr.f_ref, 4) : std::string()) << ','
                << (tr.have_float ? cellOf(tr.f3, 4) : std::string()) << ",,"
                << (tr.gate.valid ? cellOf(tr.gate.sigma, 4) : std::string()) << ",,"
                << (tr.gate.valid ? std::to_string(tr.gate.d) : std::string()) << ','
                << (tr.gate.valid ? cellOf(tr.gate.frac, 4) : std::string()) << ','
                << std::to_string(tr.n_ref) << ','
                << (tr.have_n3 ? std::to_string(tr.n3) : std::string()) << ",,,"
                << (tr.have_resid ? cellOf(tr.resid_m, 5) : std::string()) << ',' << tr.streak << ','
                << tr.audit << '\n';
        }
    }

    // The settled flags (network.ring_fix.settled_flags), after this epoch's decisions: per held family, the
    // satellites reached from their datum receiver through held links and tree edges that entered as held links.
    // A held link counts only when it is a chord this epoch (one of the links considered above): a held link in a
    // tracking gap carries no observation and settles nothing.
    if (rf.settled_flags_on) {
        std::set<RingFixLinkId> considered;
        for (const RingFixDecideObs& o : links) considered.insert(o.id);
        for (auto it = rf.certified_edges.begin(); it != rf.certified_edges.end();)
            it = tree_arcs.count({std::get<0>(*it), std::get<1>(*it), std::get<2>(*it)}) ? std::next(it)
                                                                                    : rf.certified_edges.erase(it);
        std::map<std::string, int> index_of;
        for (std::size_t i = 0; i < stations.size(); ++i) index_of[stations[i].name] = static_cast<int>(i);
        std::map<std::pair<std::string, std::string>, bool> now;
        for (const auto& [ck, s1, s2] : rf.pairs) {
            std::vector<std::pair<int, std::string>> certified;
            for (const auto& [id, L] : rf.links)
                if (L.held && considered.count(id) != 0 && id.sig1 == s1 && id.sig2 == s2
                    && constellationKey(systemOfSat(id.sat)) == ck)
                    certified.emplace_back(id.station, id.sat);
            for (const auto& [sta, sat, sig] : rf.certified_edges)
                if (sig == s1 && constellationKey(systemOfSat(sat)) == ck) certified.emplace_back(sta, sat);
            std::map<std::string, int> root_of_sat;
            for (const ChainEdgeObs& e : obs.edges) {
                if (e.signal != s1 || constellationKey(systemOfSat(e.sat)) != ck) continue;
                const auto r = index_of.find(e.r1);
                if (r != index_of.end()) root_of_sat[e.sat] = r->second;
            }
            std::set<std::string> settled;
            if (!rf.settled_largest) {
                settled = ringFixSettledSats(certified, root_of_sat);
            } else {
                std::set<std::string> tree_sats;
                for (const auto& [sat, root] : root_of_sat) { (void)root; tree_sats.insert(sat); }
                std::set<std::string>& inc = rf.settled_incumbent[ck + ":" + s1 + "/" + s2];
                RingFixLargestSet pick = ringFixLargestSettledSet(certified, tree_sats, inc, kRingFixSettledMinSats,
                                                                  kRingFixSettledHysteresis);
                if (pick.switched) {
                    ++rf.counters.settled_identity_changes;
                    rf.counters.settled_side_changes += pick.side_changes;
                }
                inc = pick.sats;
                settled = std::move(pick.sats);
            }
            const std::set<std::string> rule_set = rf.reattach ? settled : std::set<std::string>{};
            if (rf.settled_wait > 0) {
                const std::size_t in_set = settled.size();
                settled = ringFixSettledWait(settled, rf.settled_run[ck + ":" + s1 + "/" + s2], rf.settled_wait);
                rf.counters.settled_wait_held += 2 * static_cast<long long>(in_set - settled.size());
            }
            if (rf.reattach)
                requestUntiedMoves(rf, ck, s1, s2, certified, root_of_sat, rule_set, settled, links, obs, stations,
                                   now_s, t, traces);
            for (const auto& [sat, root] : root_of_sat) {
                (void)root;
                const bool f = settled.count(sat) != 0;
                for (const std::string& sig : {s1, s2}) {
                    if (rf.flags_per_family) {
                        const auto nw = now.find({sat, sig});
                        if (nw == now.end()) now[{sat, sig}] = f;
                        else nw->second = nw->second && f;   // flagged only while every family flags it
                    } else {
                        now[{sat, sig}] = f;
                    }
                    ++(f ? rf.counters.settled_signal_epochs : rf.counters.unsettled_signal_epochs);
                    const auto p0 = rf.settled.find({sat, sig});
                    if (p0 != rf.settled.end() && p0->second != f) ++rf.counters.settled_flips;
                }
            }
        }
        // The cascade families: the third signal's set from its held cascade links and the tree edges of its band that
        // entered as held cascade links, by the same rule and wait as the pairs'.
        for (const RingFixCascade& c : rf.cascades) {
            std::vector<std::pair<int, std::string>> certified;
            for (const auto& [id, CL] : rf.cascade_links)
                if (CL.held && cascade_seen.count(id) != 0 && id.sig1 == c.ref && id.sig2 == c.s3
                    && !id.sat.empty() && id.sat[0] == c.letter)
                    certified.emplace_back(id.station, id.sat);
            for (const auto& [sta, sat, sig] : rf.certified_edges)
                if (sig == c.s3 && !sat.empty() && sat[0] == c.letter) certified.emplace_back(sta, sat);
            std::map<std::string, int> root_of_sat;
            for (const ChainEdgeObs& e : obs.edges) {
                if (e.signal != c.s3 || e.sat.empty() || e.sat[0] != c.letter) continue;
                const auto r = index_of.find(e.r1);
                if (r != index_of.end()) root_of_sat[e.sat] = r->second;
            }
            std::set<std::string> settled;
            if (!rf.settled_largest) {
                settled = ringFixSettledSats(certified, root_of_sat);
            } else {
                std::set<std::string> tree_sats;
                for (const auto& [sat, root] : root_of_sat) { (void)root; tree_sats.insert(sat); }
                std::set<std::string>& inc = rf.settled_incumbent[c.name];
                RingFixLargestSet pick = ringFixLargestSettledSet(certified, tree_sats, inc, kRingFixSettledMinSats,
                                                                  kRingFixSettledHysteresis);
                inc = pick.sats;
                settled = std::move(pick.sats);
            }
            if (rf.settled_wait > 0) settled = ringFixSettledWait(settled, rf.settled_run[c.name], rf.settled_wait);
            for (const auto& [sat, root] : root_of_sat) {
                (void)root;
                const bool f = settled.count(sat) != 0;
                const auto nw = now.find({sat, c.s3});
                if (nw == now.end()) now[{sat, c.s3}] = f;
                else nw->second = nw->second && f;
                ++(f ? rf.counters.settled_signal_epochs : rf.counters.unsettled_signal_epochs);
            }
        }
        rf.settled = std::move(now);
    }
}

// ---- Run-end report ----

int networkRingFixRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix) return 0;
    const NetworkRingFix& rf = *ctx.carry.ring_fix;
    const RingFixCounters& C = rf.counters;
    long long held_end = 0;
    std::map<int, long long> held_by_ring;
    for (const auto& [id, L] : rf.links)
        if (L.held) { ++held_end; ++held_by_ring[L.ring]; }
    std::string by_ring, first, held_rings;
    for (int r = 0; r < 8; ++r) {
        if (C.decided_by_ring[r] > 0)
            by_ring += fmt::format("{}ring {}: {}", by_ring.empty() ? "" : ", ", r, C.decided_by_ring[r]);
        if (C.first_hold_s[r] >= 0.0)
            first += fmt::format("{}ring {} at TOW {:.0f}", first.empty() ? "" : ", ", r,
                                 std::fmod(C.first_hold_s[r], 604800.0));
    }
    for (const auto& [r, n] : held_by_ring)
        held_rings += fmt::format("{}ring {}: {}", held_rings.empty() ? "" : ", ", r, n);
    std::string released, refused;
    for (std::size_t k = 0; k < kRingFixReleaseCount; ++k)
        released += fmt::format("{}{} {}", k ? ", " : "", ringFixReleaseName(static_cast<RingFixRelease>(k)),
                                C.released[k]);
    for (std::size_t k = 0; k < kRingFixRefuseCount; ++k) {
        const auto c = static_cast<RingFixRefuse>(k);
        if (c == RingFixRefuse::Odds && rf.gate_rule != RingFixGateRule::Odds) continue;
        if (c == RingFixRefuse::Cap && rf.order != RingFixOrder::Certain) continue;
        if (c == RingFixRefuse::WlFraction && !(rf.wl_frac_max > 0.0)) continue;
        if (c == RingFixRefuse::CodeEvent && !(rf.code_event_window_s > 0.0)) continue;
        refused += fmt::format("{}{} {}", k ? ", " : "", ringFixRefuseName(c), C.refused[k]);
    }
    long long rel_total = 0;
    for (std::size_t k = 0; k < kRingFixReleaseCount; ++k) rel_total += C.released[k];
    log->warn("Network (ring fix, rule {}): {} link(s) decided over {} decision epoch(s) ({}); first hold by "
              "ring: {}; held at the end {} ({}); {} held link-epoch(s); {} hold row(s) folded, {} skipped on a "
              "tree edge, {} without a column.",
              ringFixRuleName(rf.rule), C.decided, C.epochs_decided,
              by_ring.empty() ? "none" : by_ring, first.empty() ? "none" : first, held_end,
              held_rings.empty() ? "none" : held_rings, C.held_link_epochs, C.rows_injected,
              C.rows_gauge_skipped, C.rows_no_column);
    // A link that became a tree edge left the held set without a release (the chain pins it).
    const std::string to_tree = C.to_tree > 0 ? fmt::format(" + to tree {}", C.to_tree) : std::string();
    log->warn("Network (ring fix): released {} ({}); {} carried entr(ies) re-opened by the release; "
              "decided {} = released {}{} + held {}: {}.  Refused link-epochs: {}.",
              rel_total, released, C.carry_reopened, C.decided, rel_total, to_tree, held_end,
              C.decided == rel_total + C.to_tree + held_end ? "closed" : "NOT CLOSED", refused);
    if (C.cuts > 0 || C.to_tree > 0 || C.inherited > 0 || C.root_kept > 0)
        log->warn("Network (ring fix, the chain's events): {} cut(s) on a band, {} with a known step ({} of 0); {} held "
                  "link(s) re-expressed, {} released across an unknown step; {} became tree edges; {} old host(s) "
                  "inherited; {} kept through a pinned root handover; {} stamp rebase(s) after a cut's erase.",
                  C.cuts, C.cuts_known, C.cuts_known_zero, C.reexpressed, C.cut_released, C.to_tree, C.inherited,
                  C.root_kept, C.stamps_rebased);
    if (C.cuts > 0 || rf.held_first)
        log->warn("Network (ring fix, cuts by kind): {} held first ({} with a step of 0), {} other known, {} unknown; "
                  "{} re-attachment(s) through a held link kept {} held link(s); {} usable held link(s) released "
                  "across an unknown step; {} cut(s) with a nested release; {} carried entr(ies) re-opened to the "
                  "release floor of {:g} m^2.{}",
                  C.cuts_held_first, C.cuts_held_first_zero, C.cuts_known - std::min(C.cuts_known, C.cuts_held_first),
                  C.cuts - C.cuts_known, C.reattached, C.held_first_kept, C.cut_released_usable, C.cuts_nested,
                  C.release_floored, kRingFixReleaseFloorM2,
                  rf.held_first ? " Held first ARMED (network.ring_fix.held_first)." : "");
    if (rf.settled_flags_on)
        log->warn("Network (ring fix, settled flags): {} satellite-signal-epochs of held families flagged settled, "
                  "{} not; {} flips between consecutive epochs.{}",
                  C.settled_signal_epochs, C.unsettled_signal_epochs, C.settled_flips,
                  (rf.settled_largest ? fmt::format("  Rule largest: {} change(s) of identity, {} satellite(s) changed "
                                                    "side at them.", C.settled_identity_changes,
                                                    C.settled_side_changes)
                                      : std::string())
                      + (rf.settled_wait > 0 ? fmt::format("  Wait of {} emission(s): {} satellite-signal-epochs in "
                                                           "the set held back before the raise.", rf.settled_wait,
                                                           C.settled_wait_held)
                                             : std::string()));
    if (rf.reattach)
        log->warn("Network (ring fix, re-attachment of untied satellites): {} move(s) requested; {} band-move(s) "
                  "applied, {} missed; past the {:.0f} s dwell without a move: {} satellite-epoch(s) with fewer than "
                  "{} candidates, {} whose candidates disagree, {} whose candidates converge (offsets below {:.2f} and "
                  "{:.2f} cycle), {} within a {:.0f} s hold-off; moved satellites flagged within {:.0f} s: {}, not: {}.",
                  C.untied_requests, C.untied_applied, C.untied_missed, kRingFixReattachDwellS, C.untied_few,
                  kRingFixReattachMinLinks, C.untied_spread, C.untied_converging, kRingFixReattachMinOffsetIfCyc,
                  kRingFixReattachMinOffsetWlCyc, C.untied_held_off, kRingFixReattachHoldoffS, C.untied_flagged_after,
                  C.untied_unflagged_after);
    for (const RingFixCascade& c : rf.cascades) {
        const auto it = rf.cascade_counters.find(c.name);
        const RingFixCascadeCounters CC = it != rf.cascade_counters.end() ? it->second : RingFixCascadeCounters{};
        std::string rel;
        long long rel_n = 0;
        for (const auto& [cause, n] : CC.released) {
            rel += fmt::format("{}{} {}", rel.empty() ? "" : ", ", cause, n);
            rel_n += n;
        }
        log->warn("Network (ring fix, cascade {} against {}): decided {}; held link-epochs {}; released {}{}; to the "
                  "tree {}; refused link-epochs: fraction {}, odds {}, persistence {}, no float {}.{}{}{}{}",
                  c.name, c.ref, CC.decided, CC.held_link_epochs, rel_n, rel.empty() ? std::string() : " (" + rel + ")",
                  CC.to_tree, CC.refused_fraction, CC.refused_odds, CC.refused_persist, CC.refused_no_float,
                  rf.truth ? fmt::format("  Truth: held link-epochs right {}, WRONG {}; decisions WRONG {}.",
                                         CC.truth_right, CC.truth_wrong, CC.decided_truth_wrong)
                           : std::string(),
                  rf.cascade_tree ? fmt::format("  Tree on the pair's forest: {} tree edge-epochs of {}, {} on the "
                                                "pair's tree edges, {} on links the pair holds, {} otherwise.",
                                                CC.tree_edges, c.s3, CC.tree_pair, CC.tree_held, CC.tree_other)
                                  : std::string(),
                  rf.cascade_log ? fmt::format("  Log mode: {} would-hold link-epochs, none held{}.", CC.would_hold,
                                               rf.truth ? fmt::format(" (truth: WRONG {})", CC.would_truth_wrong)
                                                        : std::string())
                                 : std::string(),
                  rf.cascade_rehost_keep
                      ? fmt::format("  Re-hosts of {} onto held links: {} kept at the held integer, {} released off "
                                    "it, {} released for another cause.", c.s3, CC.rehost_kept, CC.rehost_off_integer,
                                    CC.rehost_not_kept)
                      : std::string());
    }
    if (rf.rehost_keep_carry)
        log->warn("Network (ring fix, re-hosts onto held links): {} kept with their carries (the new host's carry on "
                  "the held integer); {} released because that carry was 1e-4 cycle or more off the held integer (a "
                  "link decided the epoch before); {} released for another cause (no carry at the new host, an "
                  "unknown draft step).",
                  C.rehost_kept, C.rehost_off_integer, C.rehost_not_kept);
    if (!rf.pairs.empty())
        log->warn("Network (ring fix, pairs): {} chord link-epoch(s) of other pair families were not considered.",
                  C.excluded_link_epochs);
    if (rf.mask_station >= 0)
        log->warn("Network (ring fix, slip mask, simulation only): {} masked epoch(s); {} latch, funnel or pend entr(ies) "
                  "removed; {} carried entr(ies) the drains had changed restored.",
                  C.mask_epochs, C.mask_keys_removed, C.mask_carries_restored);
    if (rf.rule == RingFixRule::Log)
        log->warn("Network (ring fix, log rule): the frozen rule would have decided {} link(s) at {} link-epoch(s), "
                  "{} of them with integers other than the link's first{}; nothing was held.",
                  C.log_links, C.log_link_epochs, C.log_changed,
                  rf.truth ? fmt::format("; against truth {} right, {} WRONG", C.log_truth_right, C.log_truth_wrong)
                           : std::string());
    if (rf.oracle_until_s > 0.0)
        log->warn("Network (ring fix, oracle phase of {:.0f} s): {} link(s) held at their true integers, {} held "
                  "integer(s) set again, {} hold(s) put off to the next epoch (released, latched, re-levelled or "
                  "cut in the epoch).", rf.oracle_until_s, C.oracle_decided, C.oracle_corrected, C.oracle_waited);
    if (rf.truth)
        log->warn("Network (ring fix truth audit, in each epoch's frame): held link-epochs right {}, WRONG {}, without "
                  "a true integer {}; decisions WRONG {}; {} branch(es) started without a wind-up row{}.",
                  C.truth_right, C.truth_wrong, C.truth_none, C.decided_truth_wrong, rf.truth->arcs_without_windup,
                  C.truth_file_checked > 0
                      ? fmt::format("; against the audit file {} chord-band-epochs compared, {} differ",
                                    C.truth_file_checked, C.truth_file_mismatch)
                      : std::string());
    if (rf.audit.empty())
        log->warn("Network (ring fix audit): no audit file; nothing was compared against truth.");
    else
        log->warn("Network (ring fix audit, '{}'): decisions right {}, WRONG {}; held link-epochs right {}, "
                  "WRONG {}, not listed {}.",
                  rf.audit_file, C.decided_right, C.decided_wrong, C.audit_right, C.audit_wrong,
                  C.audit_unlisted);
    return 0;
}

} // namespace GnssProcessor
