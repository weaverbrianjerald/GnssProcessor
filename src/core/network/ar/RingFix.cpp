/**
 * @file RingFix.cpp
 * @brief The pure steps of the ring fix: the gate, the rings, the decision, the release and the
 *        audit (see the header).  The network seams are in RingFixSeams.cpp.
 */

#include "network/ar/RingFix.hpp"
#include "network/ar/ArcGraph.hpp"   // arcgraph::Dsu, the components of the settled flags
#include "common/Constants.hpp"   // Constants::PI, the re-attachment's circular statistics
#include "common/GnssIdentity.hpp"   // systemOfSat, constellationKey
#include "network/solve/ChainChordTable.hpp"   // chainWavelengthM (the cascade's reference)
#include "products/NetworkProductEmitter.hpp"   // NetworkSolutionEpoch, the settled flags' records

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace GnssProcessor {

const char* ringFixRuleName(RingFixRule r) {
    switch (r) {
        case RingFixRule::Gated: return "gated";
        case RingFixRule::Plain: return "plain";
        case RingFixRule::Log:   return "log";
    }
    return "?";
}

const char* ringFixReleaseName(RingFixRelease c) {
    switch (c) {
        case RingFixRelease::Slip:    return "slip";
        case RingFixRelease::Refloat: return "refloat";
        case RingFixRelease::Gap:     return "gap";
        case RingFixRelease::Chain:   return "chain";
        case RingFixRelease::Monitor: return "monitor";
    }
    return "?";
}

std::set<std::string> ringFixSettledSats(const std::vector<std::pair<int, std::string>>& links,
                                         const std::map<std::string, int>& root_of_sat) {
    // Receivers keep their roster index as id; satellites take ids above every receiver's.
    std::map<std::string, int> sat_id;
    int next = 1 << 20;
    const auto idOf = [&](const std::string& s) {
        const auto it = sat_id.find(s);
        return it != sat_id.end() ? it->second : (sat_id[s] = next++);
    };
    arcgraph::Dsu dsu;
    for (const auto& [sta, sat] : links) arcgraph::dsuUnite(dsu, sta, idOf(sat));
    std::set<std::string> out;
    for (const auto& [sat, root] : root_of_sat)
        if (sat_id.count(sat) != 0 && arcgraph::dsuFind(dsu, idOf(sat)) == arcgraph::dsuFind(dsu, root)) out.insert(sat);
    return out;
}

void ringFixStampSettledFlags(NetworkSolutionEpoch& sol, const NetworkRingFix& rf) {
    for (NetworkSatSolution& s : sol.sats) {
        const std::string ck = constellationKey(systemOfSat(s.sat));
        for (auto& [signal, npb] : s.phase_bias) {
            bool held_family = false;
            for (const auto& [pc, s1, s2] : rf.pairs)
                if (pc == ck && (signal == s1 || signal == s2)) held_family = true;
            // A cascade's third signal is a held family only while the cascades hold: in the log mode
            // (network.ring_fix.cascade_log) nothing is held and the stream declares what it would without them.
            if (!rf.cascade_log)
                for (const RingFixCascade& c : rf.cascades)
                    if (c.ck == ck && signal == c.s3) held_family = true;
            const auto f = rf.settled.find({s.sat, signal});
            npb.signal_integer    = held_family && f != rf.settled.end() && f->second;
            npb.wide_lane_integer = held_family ? 1 : 0;
        }
    }
}

std::set<std::string> ringFixSettledWait(const std::set<std::string>& in_set, std::map<std::string, int>& run,
                                         int wait) {
    for (auto it = run.begin(); it != run.end();)
        it = in_set.count(it->first) != 0 ? std::next(it) : run.erase(it);
    std::set<std::string> out;
    for (const std::string& s : in_set)
        if (++run[s] >= wait + 1) out.insert(s);
    return out;
}

RingFixCascadeGate ringFixCascadeGate(double f_ref, double f3, double q11, double q12, double q22, double sigma_scale) {
    RingFixCascadeGate g;
    if (!(std::isfinite(f_ref) && std::isfinite(f3) && std::isfinite(q11) && std::isfinite(q12) && std::isfinite(q22)))
        return g;
    const double var = q11 + q22 - 2.0 * q12;
    if (!(var >= 0.0)) return g;
    const double w = f_ref - f3;
    g.d = std::llround(w);
    g.frac = std::abs(w - static_cast<double>(g.d));
    g.sigma = std::sqrt(var);
    const double s = g.sigma * sigma_scale;
    g.log_odds = s > 0.0 ? (1.0 - 2.0 * g.frac) / (2.0 * s * s)
                         : (g.frac < 0.5 ? std::numeric_limits<double>::infinity() : 0.0);
    g.valid = true;
    g.pass = g.frac <= kRingFixCascadeMaxFracCyc && g.log_odds >= kRingFixMinLogOdds;
    return g;
}

std::string ringFixCascadeReference(const std::string& sat, const std::string& s1, const std::string& s2,
                                    const std::string& s3) {
    if (s1.size() >= 2 && s3.size() >= 2 && s1[1] == s3[1]) return s1;
    if (s2.size() >= 2 && s3.size() >= 2 && s2[1] == s3[1]) return s2;
    const double l1 = chainWavelengthM(sat, s1), l2 = chainWavelengthM(sat, s2), l3 = chainWavelengthM(sat, s3);
    if (!(l1 > 0.0 && l2 > 0.0 && l3 > 0.0)) return {};
    return std::abs(1.0 / l1 - 1.0 / l3) <= std::abs(1.0 / l2 - 1.0 / l3) ? s1 : s2;
}

std::optional<long long> ringFixCascadeKeepInteger(const RingFixCascadeLink& link, bool ref_freed, bool third_freed) {
    if (!link.held || link.monitor_trip || ref_freed || third_freed) return std::nullopt;
    return link.n3;
}

int ringFixCascadeLeadRank(bool pair_tree_edge, bool pair_held, bool third_incumbent, bool third_slipped,
                           bool eligible_host) {
    if (third_slipped || (!eligible_host && !third_incumbent)) return 0;
    if (pair_tree_edge) return 1;
    if (pair_held) return third_incumbent ? 2 : 3;
    return 0;
}

RingFixLargestSet ringFixLargestSettledSet(const std::vector<std::pair<int, std::string>>& links,
                                           const std::set<std::string>& tree_sats,
                                           const std::set<std::string>& incumbent, int min_sats, int hysteresis) {
    // Receivers keep their roster index as id; satellites take ids above every receiver's.
    std::map<std::string, int> sat_id;
    int next = 1 << 20;
    const auto idOf = [&](const std::string& s) {
        const auto it = sat_id.find(s);
        return it != sat_id.end() ? it->second : (sat_id[s] = next++);
    };
    arcgraph::Dsu dsu;
    for (const auto& [sta, sat] : links) arcgraph::dsuUnite(dsu, sta, idOf(sat));
    std::map<int, std::set<std::string>> by_root;   // component -> its tree satellites
    for (const std::string& s : tree_sats)
        if (sat_id.count(s) != 0) by_root[arcgraph::dsuFind(dsu, sat_id.at(s))].insert(s);
    const auto first = [](const std::set<std::string>& a, const std::set<std::string>& b) {   // a ranks before b
        return a.size() != b.size() ? a.size() > b.size() : *a.begin() < *b.begin();
    };
    const std::set<std::string>* big = nullptr;   // the largest component
    const std::set<std::string>* inc = nullptr;   // the component holding most of the incumbent
    std::size_t inc_overlap = 0;
    for (const auto& [root, g] : by_root) {
        (void)root;
        if (big == nullptr || first(g, *big)) big = &g;
        std::size_t ov = 0;
        for (const std::string& s : g) ov += incumbent.count(s);
        if (ov > 0 && (inc == nullptr || ov > inc_overlap || (ov == inc_overlap && *g.begin() < *inc->begin()))) {
            inc = &g;
            inc_overlap = ov;
        }
    }
    const auto n = [](const std::set<std::string>* g) { return g != nullptr ? static_cast<int>(g->size()) : 0; };
    RingFixLargestSet out;
    if (inc != nullptr && n(inc) >= min_sats && !(big != inc && n(big) >= n(inc) + hysteresis)) {
        out.sats = *inc;
    } else if (n(big) >= min_sats) {
        out.sats = *big;
        if (!incumbent.empty() && big != inc) {
            out.switched = true;
            for (const std::string& s : incumbent) out.side_changes += out.sats.count(s) == 0 ? 1 : 0;
            for (const std::string& s : out.sats) out.side_changes += incumbent.count(s) == 0 ? 1 : 0;
        }
    }
    return out;
}

bool ringFixCodeEventOpen(double code_event_s, double window_s, double now_s) {
    return window_s > 0.0 && code_event_s >= 0.0 && now_s - code_event_s < window_s;
}

const char* ringFixRefuseName(RingFixRefuse c) {
    switch (c) {
        case RingFixRefuse::NotCandidate: return "not_candidate";
        case RingFixRefuse::NoFloat:      return "no_float";
        case RingFixRefuse::Window:       return "window";
        case RingFixRefuse::Sigma:        return "sigma";
        case RingFixRefuse::Fraction:     return "fraction";
        case RingFixRefuse::Lattice:      return "lattice";
        case RingFixRefuse::Persist:      return "persist";
        case RingFixRefuse::Odds:         return "odds";
        case RingFixRefuse::Cap:          return "cap";
        case RingFixRefuse::WlFraction:   return "wl_fraction";
        case RingFixRefuse::CodeEvent:    return "code_event";
    }
    return "?";
}

RingFixGate ringFixGate(double f1, double f2, double q11, double q12, double q22,
                        double lambda1, double lambda2, double sigma_scale) {
    RingFixGate g;
    if (!(std::isfinite(f1) && std::isfinite(f2) && std::isfinite(q11) && std::isfinite(q12)
          && std::isfinite(q22) && std::isfinite(lambda1) && std::isfinite(lambda2)))
        return g;
    if (!(lambda1 > 0.0 && lambda2 > lambda1)) return g;
    const double r = lambda2 / lambda1;          // F1 / F2
    const double a = r / (r - 1.0);
    const double b = -1.0 / (r - 1.0);
    const double var_if = a * a * q11 + 2.0 * a * b * q12 + b * b * q22;
    const double var_wl = q11 + q22 - 2.0 * q12;
    if (!(var_if >= 0.0) || !(var_wl >= 0.0)) return g;
    const double w = f1 - f2;
    g.wl = std::llround(w);
    g.frac_wl = std::abs(w - static_cast<double>(g.wl));
    g.sigma_wl = std::sqrt(var_wl);
    g.n_if = (r * f1 - f2 - static_cast<double>(g.wl)) / (r - 1.0);
    g.n1 = std::llround(g.n_if);
    g.n2 = g.n1 - g.wl;
    g.frac_if = std::abs(g.n_if - static_cast<double>(g.n1));
    g.sigma_if = std::sqrt(var_if);
    g.valid = true;
    g.pass = g.sigma_if <= kRingFixMaxSigmaCyc && g.frac_if <= kRingFixMaxFracCyc;
    // The likelihood ratio of the nearest and the next integer of a Gaussian float at distance d from
    // the nearest: exp(((1 - d)^2 - d^2) / (2 sigma^2)) = exp((1 - 2 d) / (2 sigma^2)).
    const auto logOdds = [](double d, double sigma) {
        return sigma > 0.0 ? (1.0 - 2.0 * d) / (2.0 * sigma * sigma)
                           : (d < 0.5 ? std::numeric_limits<double>::infinity() : 0.0);
    };
    // A scale of 1 multiplies exactly, so the default reproduces the formal-sigma odds bit for bit.
    g.log_odds_if = logOdds(g.frac_if, g.sigma_if * sigma_scale);
    g.log_odds_wl = logOdds(g.frac_wl, g.sigma_wl * sigma_scale);
    g.pass_odds = g.frac_if <= kRingFixMaxFracCyc && g.log_odds_if >= kRingFixMinLogOdds
               && g.log_odds_wl >= kRingFixMinLogOdds;
    return g;
}

std::map<std::pair<std::string, std::string>, RingFixRingInfo>
ringFixRings(const RingFixTree& tree, const std::set<std::pair<std::string, std::string>>& held) {
    std::map<std::string, int> depth;
    // Every link at a vertex: the other end and whether the link is a tree edge.
    std::map<std::string, std::vector<std::pair<std::string, bool>>> nbr;
    for (const auto& [child, parent, d] : tree.edges) {
        depth[child] = d;
        depth.emplace(parent, d - 1);
        nbr[child].emplace_back(parent, true);
        nbr[parent].emplace_back(child, true);
    }
    for (const auto& [rx, sv] : tree.chords) {
        nbr[rx].emplace_back(sv, false);
        nbr[sv].emplace_back(rx, false);
    }
    const auto isHeld = [&](const std::string& u, const std::string& v) {
        // A chord is keyed (receiver key, satellite key).
        if (!u.empty() && u[0] == 'R') return held.count({u, v}) > 0;
        return held.count({v, u}) > 0;
    };
    std::map<std::pair<std::string, std::string>, RingFixRingInfo> out;
    for (const auto& [rx, sv] : tree.chords) {
        RingFixRingInfo info;
        const auto dr = depth.find(rx), ds = depth.find(sv);
        if (dr == depth.end() || ds == depth.end()) { out[{rx, sv}] = info; continue; }
        info.ring = std::max(dr->second, ds->second);
        info.inner = dr->second < ds->second ? rx : sv;
        const int d_in = std::min(dr->second, ds->second);
        bool complete = true;
        for (const auto& [u, is_tree] : nbr[info.inner]) {
            const auto du = depth.find(u);
            if (du == depth.end() || du->second >= d_in) continue;
            if (!is_tree && !isHeld(info.inner, u)) { complete = false; break; }
        }
        info.candidate = complete;
        out[{rx, sv}] = info;
    }
    return out;
}

bool ringFixWindowOk(const RingFixLink& L, const RingFixDecideObs& o, double last_tree_s, double now_s) {
    if (!o.carry_ok || o.latched) return false;
    if (o.relevel1_s > 0.0 && now_s - o.relevel1_s < kRingFixWindowS) return false;
    if (o.relevel2_s > 0.0 && now_s - o.relevel2_s < kRingFixWindowS) return false;
    if (L.chord_since_s < 0.0 || now_s - L.chord_since_s < kRingFixWindowS) return false;
    if (now_s - last_tree_s < kRingFixWindowS) return false;
    if (L.released_s >= 0.0 && now_s - L.released_s < kRingFixWindowS) return false;
    return true;
}

namespace {

/// A fraction of a cycle wrapped into [-0.5, 0.5).
double wrapCycle(double x) { return x - std::floor(x + 0.5); }

/// The circular mean and standard deviation of fractions of a cycle (cycles).
std::pair<double, double> circularMeanSd(const std::vector<double>& v) {
    double s = 0.0, c = 0.0;
    for (double x : v) {
        s += std::sin(2.0 * Constants::PI * x);
        c += std::cos(2.0 * Constants::PI * x);
    }
    s /= static_cast<double>(v.size());
    c /= static_cast<double>(v.size());
    const double r = std::hypot(s, c);
    return {std::atan2(s, c) / (2.0 * Constants::PI), std::sqrt(std::max(0.0, -2.0 * std::log(std::max(r, 1e-12)))) / (2.0 * Constants::PI)};
}

/// The median of fractions unwrapped about their circular mean, wrapped back (cycles).
double circularMedian(const std::vector<double>& v, double mean) {
    std::vector<double> u;
    u.reserve(v.size());
    for (double x : v) u.push_back(wrapCycle(x - mean));
    std::sort(u.begin(), u.end());
    const std::size_t n = u.size();
    const double med = n % 2 == 1 ? u[n / 2] : 0.5 * (u[n / 2 - 1] + u[n / 2]);
    return wrapCycle(med + mean);
}

}  // namespace

RingFixReattachPick ringFixReattachChoice(const std::vector<RingFixReattachCand>& cands) {
    RingFixReattachPick p;
    p.n = static_cast<int>(cands.size());
    if (cands.empty()) return p;
    std::vector<double> fi, fw;
    for (const RingFixReattachCand& c : cands) {
        fi.push_back(c.frac_if);
        fw.push_back(c.frac_wl);
    }
    const auto [mi, si] = circularMeanSd(fi);
    const auto [mw, sw] = circularMeanSd(fw);
    p.sd_if  = si;
    p.sd_wl  = sw;
    p.med_if = circularMedian(fi, mi);
    p.med_wl = circularMedian(fw, mw);
    if (p.n < kRingFixReattachMinLinks || si > kRingFixReattachMaxSdIfCyc || sw > kRingFixReattachMaxSdWlCyc) return p;
    if (std::abs(p.med_if) < kRingFixReattachMinOffsetIfCyc && std::abs(p.med_wl) < kRingFixReattachMinOffsetWlCyc) {
        p.converging = true;   // the links are converging onto the satellite's level: the gates decide them
        return p;
    }
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const double d = std::abs(wrapCycle(cands[i].frac_if - p.med_if)) / kRingFixReattachMaxSdIfCyc
                       + std::abs(wrapCycle(cands[i].frac_wl - p.med_wl)) / kRingFixReattachMaxSdWlCyc;
        if (d < best) {
            best    = d;
            p.index = static_cast<int>(i);
        }
    }
    p.ok = p.index >= 0;
    return p;
}

void ringFixHoldLink(NetworkRingFix& rf, RingFixLink& L, long long n1, long long n2, int ring, const std::string& root,
                     double now_s, RingFixHoldKind kind, bool rebase_stamps, double stamp1, double stamp2) {
    L.held = true;
    L.n1 = n1;
    L.n2 = n2;
    L.ring = ring;
    L.decided_s = now_s;
    L.nn_n = 0;
    L.nn_gf_m = 0.0;
    L.nn_if_m = 0.0;
    L.nn_moved = false;
    if (kind != RingFixHoldKind::Inherited) {
        L.stamp1 = stamp1;
        L.stamp2 = stamp2;
    }
    L.root = root;
    L.absent_since_s = -1.0;
    L.monitor_bad = 0;
    L.monitor_trip = false;
    L.streak = 0;
    L.wrong_now = false;
    L.rebase_stamps = rebase_stamps;
    L.inherited = kind == RingFixHoldKind::Inherited;
    L.oracle = kind == RingFixHoldKind::Oracle;
    RingFixCounters& C = rf.counters;
    ++C.decided;
    if (kind == RingFixHoldKind::Oracle) ++C.oracle_decided;
    if (kind == RingFixHoldKind::Inherited) ++C.inherited;
    const int ri = std::clamp(ring, 0, 7);
    ++C.decided_by_ring[ri];
    if (C.first_hold_s[ri] < 0.0) C.first_hold_s[ri] = now_s;
}

void ringFixUnholdLink(RingFixLink& L, double now_s, RingFixUnholdKind kind) {
    L.held = false;
    L.streak = 0;
    L.monitor_bad = 0;
    L.monitor_trip = false;
    switch (kind) {
        case RingFixUnholdKind::Release:
            L.released_s = now_s;
            L.absent_since_s = -1.0;
            L.wrong_now = false;
            break;
        case RingFixUnholdKind::CutRelease:
            L.released_s = now_s;
            L.absent_since_s = -1.0;
            L.rebase_stamps = false;
            break;
        case RingFixUnholdKind::ToTree:
            L.rebase_stamps = false;
            break;
    }
}

std::vector<RingFixEvent> ringFixDecideStep(NetworkRingFix& rf,
                                            const std::vector<RingFixDecideObs>& links,
                                            double now_s) {
    std::vector<RingFixEvent> events;
    RingFixCounters& C = rf.counters;
    ++C.epochs_decided;
    if (rf.t0_s < 0.0) rf.t0_s = now_s;
    // The oracle phase (simulation only): every link with true integers is held at them; the rule waits.
    if (rf.oracle_until_s > 0.0 && now_s - rf.t0_s < rf.oracle_until_s) {
        for (const RingFixDecideObs& o : links) {
            RingFixLink& L = rf.links[o.id];
            L.lambda1 = o.lambda1;
            L.lambda2 = o.lambda2;
            if (o.have_float) {
                if (L.last_chord_s < 0.0 || now_s - L.last_chord_s > kRingFixGapS) L.chord_since_s = now_s;
                L.last_chord_s = now_s;
            }
            if (L.held) ++C.held_link_epochs;
            if (!o.have_truth) continue;
            // No hold in an epoch in which the link was released, its arc latched, re-levelled
            // (refloated or dropped) or cut (its carried entry erased): from the next epoch, on its new level.
            if (!L.held && (L.released_s == now_s || o.latched || !o.carry_ok || o.relevel1_s >= now_s - 1e-6
                            || o.relevel2_s >= now_s - 1e-6)) {
                ++C.oracle_waited;
                continue;
            }
            if (L.held) {
                if (L.n1 != o.truth1 || L.n2 != o.truth2) {
                    L.n1 = o.truth1;
                    L.n2 = o.truth2;
                    ++C.oracle_corrected;
                }
                continue;
            }
            RingFixEvent ev;
            ev.id = o.id;
            ev.oracle = true;
            ev.gate.n1 = o.truth1;
            ev.gate.n2 = o.truth2;
            ev.gate.wl = o.truth1 - o.truth2;
            ringFixHoldLink(rf, L, o.truth1, o.truth2, o.ring, o.root, now_s, RingFixHoldKind::Oracle, !o.carry_ok,
                            o.relevel1_s, o.relevel2_s);
            ev.kind = RingFixEvent::Kind::Hold;
            events.push_back(ev);
        }
        return events;
    }
    // Change O: the links that passed every test this epoch, decided after the loop.
    std::vector<std::pair<const RingFixDecideObs*, RingFixEvent>> eligible;
    const auto hold = [&](RingFixLink& L, const RingFixDecideObs& o, RingFixEvent& ev) {
        ringFixHoldLink(rf, L, ev.gate.n1, ev.gate.n2, o.ring, o.root, now_s, RingFixHoldKind::Decided, false,
                        o.relevel1_s, o.relevel2_s);
        ev.kind = RingFixEvent::Kind::Hold;
        events.push_back(ev);
    };
    for (const RingFixDecideObs& o : links) {
        RingFixLink& L = rf.links[o.id];
        L.lambda1 = o.lambda1;
        L.lambda2 = o.lambda2;
        if (o.have_float) {
            if (L.last_chord_s < 0.0 || now_s - L.last_chord_s > kRingFixGapS) L.chord_since_s = now_s;
            L.last_chord_s = now_s;
        }
        if (L.held) {
            ++C.held_link_epochs;
            if (o.have_resid && std::isfinite(o.resid_if_m)
                && std::abs(o.resid_if_m) > kRingFixMonitorBoundM) {
                if (++L.monitor_bad >= kRingFixMonitorEpochs) L.monitor_trip = true;
            } else {
                L.monitor_bad = 0;
            }
            // The moving averages of the chord's code residuals, the near-null signature (network.ring_fix.nn_guard,
            // and network.ring_fix.entry_revalidate, which reads it at a cut).
            if ((rf.nn_guard || rf.entry_revalidate || rf.nn_monitor) && o.have_code && std::isfinite(o.code_gf_m)
                && std::isfinite(o.code_if_m)) {
                ++L.nn_n;
                const double w = 1.0 / static_cast<double>(std::min(L.nn_n, std::max(1, rf.nn_min_epochs)));
                L.nn_gf_m += w * (o.code_gf_m - L.nn_gf_m);
                L.nn_if_m += w * (o.code_if_m - L.nn_if_m);
            }
            continue;
        }
        RingFixEvent ev;
        ev.id = o.id;
        const auto refuse = [&](RingFixRefuse c) {
            ev.kind = RingFixEvent::Kind::Refuse;
            ev.refuse = c;
            ++C.refused[static_cast<std::size_t>(c)];
            if (c != RingFixRefuse::Persist) L.streak = 0;
            events.push_back(ev);
        };
        const bool candidate = rf.order == RingFixOrder::Certain ? o.have_float : o.candidate;
        if (!candidate) { refuse(RingFixRefuse::NotCandidate); continue; }
        if (!o.have_float) { refuse(RingFixRefuse::NoFloat); continue; }
        ev.gate = ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2, rf.sigma_scale);
        if (!ev.gate.valid) { refuse(RingFixRefuse::NoFloat); continue; }
        bool decide = false;
        if (rf.rule == RingFixRule::Plain) {
            decide = true;
        } else {
            const auto lt = rf.last_tree_s.find({o.id.station, o.id.sat});
            const double last_tree =
                lt == rf.last_tree_s.end() ? -std::numeric_limits<double>::infinity() : lt->second;
            if (!ringFixWindowOk(L, o, last_tree, now_s)) { refuse(RingFixRefuse::Window); continue; }
            if (ringFixCodeEventOpen(o.code_event_s, rf.code_event_window_s, now_s)) {
                refuse(RingFixRefuse::CodeEvent);
                continue;
            }
            if (rf.gate_rule == RingFixGateRule::Odds) {
                if (ev.gate.frac_if > kRingFixMaxFracCyc) { refuse(RingFixRefuse::Fraction); continue; }
                if (!ev.gate.pass_odds) { refuse(RingFixRefuse::Odds); continue; }
            } else {
                if (ev.gate.sigma_if > kRingFixMaxSigmaCyc) { refuse(RingFixRefuse::Sigma); continue; }
                if (ev.gate.frac_if > kRingFixMaxFracCyc) { refuse(RingFixRefuse::Fraction); continue; }
            }
            if ((o.have_lat1 && o.lat1 != ev.gate.n1) || (o.have_lat2 && o.lat2 != ev.gate.n2)) {
                refuse(RingFixRefuse::Lattice);
                continue;
            }
            if (L.streak > 0 && L.streak_n1 == ev.gate.n1 && L.streak_n2 == ev.gate.n2) {
                ++L.streak;
            } else {
                L.streak = 1;
                L.streak_n1 = ev.gate.n1;
                L.streak_n2 = ev.gate.n2;
            }
            if (L.streak < kRingFixPersistEpochs) { refuse(RingFixRefuse::Persist); continue; }
            // The wide lane's own bound, last: a refusal here names a link every other test would decide.
            if (rf.wl_frac_max > 0.0 && ev.gate.frac_wl > rf.wl_frac_max) { refuse(RingFixRefuse::WlFraction); continue; }
            decide = true;
        }
        if (!decide) continue;
        if (rf.rule == RingFixRule::Log) {
            ev.kind = RingFixEvent::Kind::Hold;
            ev.log_only = true;
            ++C.log_link_epochs;
            if (!L.log_decided) {
                L.log_decided = true;
                L.log_n1 = ev.gate.n1;
                L.log_n2 = ev.gate.n2;
                ++C.log_links;
            } else if (L.log_n1 != ev.gate.n1 || L.log_n2 != ev.gate.n2) {
                ++C.log_changed;
            }
            events.push_back(ev);
            continue;
        }
        if (rf.order == RingFixOrder::Certain && rf.rule != RingFixRule::Plain) {
            eligible.emplace_back(&o, ev);
            continue;
        }
        hold(L, o, ev);
    }
    if (!eligible.empty()) {
        std::stable_sort(eligible.begin(), eligible.end(), [](const auto& a, const auto& b) {
            if (a.second.gate.log_odds_if != b.second.gate.log_odds_if)
                return a.second.gate.log_odds_if > b.second.gate.log_odds_if;
            return a.first->id < b.first->id;
        });
        int decided_now = 0;
        for (auto& [o, ev] : eligible) {
            RingFixLink& L = rf.links[o->id];
            if (rf.max_decisions_per_epoch <= 0 || decided_now < rf.max_decisions_per_epoch) {
                hold(L, *o, ev);
                ++decided_now;
            } else {
                ev.kind = RingFixEvent::Kind::Refuse;
                ev.refuse = RingFixRefuse::Cap;
                ++C.refused[static_cast<std::size_t>(RingFixRefuse::Cap)];
                events.push_back(ev);   // the persistence count is kept
            }
        }
    }
    return events;
}

std::vector<RingFixEvent> ringFixReleaseStep(NetworkRingFix& rf,
                                             const std::map<RingFixLinkId, RingFixHoldObs>& obs,
                                             double now_s) {
    std::vector<RingFixEvent> events;
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        const auto it = obs.find(id);
        bool release = false;
        RingFixRelease cause = RingFixRelease::Gap;
        if (it != obs.end()) {
            const RingFixHoldObs& o = it->second;
            if (L.rebase_stamps && o.carry_ok) {   // a cut release erased its carried entries
                L.stamp1 = o.stamp1;
                L.stamp2 = o.stamp2;
                L.rebase_stamps = false;
                ++rf.counters.stamps_rebased;
            }
            const bool root_changed = !o.root.empty() && !L.root.empty() && o.root != L.root;
            const bool relevelled = !o.carry_ok || o.stamp1 > L.stamp1 || o.stamp2 > L.stamp2;
            if (o.tree_now || o.in_cut || root_changed) { release = true; cause = RingFixRelease::Chain; }
            else if (o.latched)                          { release = true; cause = RingFixRelease::Slip; }
            else if (o.present && relevelled && !L.rebase_stamps) { release = true; cause = RingFixRelease::Refloat; }
            else if (L.monitor_trip)                     { release = true; cause = RingFixRelease::Monitor; }
            else if (o.present)                          { L.absent_since_s = -1.0; }
        } else if (L.monitor_trip) {
            release = true;
            cause = RingFixRelease::Monitor;
        }
        if (!release && (it == obs.end() || !it->second.present)) {
            if (L.absent_since_s < 0.0) L.absent_since_s = now_s;
            if (now_s - L.absent_since_s > rf.max_gap_s) { release = true; cause = RingFixRelease::Gap; }
        }
        if (!release) continue;
        ringFixUnholdLink(L, now_s, RingFixUnholdKind::Release);
        ++rf.counters.released[static_cast<std::size_t>(cause)];
        RingFixEvent ev;
        ev.kind = RingFixEvent::Kind::Release;
        ev.id = id;
        ev.release = cause;
        events.push_back(ev);
    }
    return events;
}

std::map<std::tuple<std::string, std::string, std::string>, long long>
ringFixReadAuditFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("network.ring_fix.audit_frame_file: cannot open '" + path + "'");
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("ring fix audit file '" + path + "' is empty");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("station,sat,signal,frame_int", 0) != 0)
        throw std::runtime_error("ring fix audit file '" + path
                                 + "': the header must begin station,sat,signal,frame_int");
    std::map<std::tuple<std::string, std::string, std::string>, long long> out;
    long long row = 1;
    while (std::getline(in, line)) {
        ++row;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string sta, sat, sig, val;
        if (!std::getline(ss, sta, ',') || !std::getline(ss, sat, ',') || !std::getline(ss, sig, ',')
            || !std::getline(ss, val, ','))
            throw std::runtime_error("ring fix audit file '" + path + "': malformed row "
                                     + std::to_string(row));
        long long v = 0;
        try {
            std::size_t used = 0;
            v = std::stoll(val, &used);
            if (used != val.size()) throw std::invalid_argument(val);
        } catch (...) {
            throw std::runtime_error("ring fix audit file '" + path + "': bad frame_int in row "
                                     + std::to_string(row));
        }
        if (!out.emplace(std::make_tuple(sta, sat, sig), v).second)
            throw std::runtime_error("ring fix audit file '" + path + "': duplicate arc in row "
                                     + std::to_string(row));
    }
    return out;
}

RingFixAudit ringFixAuditLink(
    const std::map<std::tuple<std::string, std::string, std::string>, long long>& audit,
    const std::string& station_name, const RingFixLinkId& id, long long n1, long long n2) {
    const auto a1 = audit.find({station_name, id.sat, id.sig1});
    const auto a2 = audit.find({station_name, id.sat, id.sig2});
    if (a1 == audit.end() && a2 == audit.end()) return RingFixAudit::Unlisted;
    if ((a1 != audit.end() && a1->second != n1) || (a2 != audit.end() && a2->second != n2))
        return RingFixAudit::Wrong;
    return RingFixAudit::Right;
}

// ---- Stage N2: held integers through the chain's events ------------------------------------------

bool ringFixCutStep(bool held_before, long long n_held, double target_cyc, bool sat_is_child,
                    long long& delta) {
    if (!held_before || !std::isfinite(target_cyc)) return false;
    const double r = std::round(target_cyc);
    if (std::abs(target_cyc - r) > 1e-6) return false;
    const long long k = std::llround(r);
    delta = sat_is_child ? n_held - k : k - n_held;
    return true;
}

std::vector<RingFixNnMonitorEdge> ringFixNnMonitorStep(RingFixNnMonitor& m, const std::vector<RingFixNearNullVerdict>& v,
                                                       double tow, double max_if_m) {
    std::vector<RingFixNnMonitorEdge> edges;
    for (const RingFixNearNullVerdict& x : v) {
        ++m.checks;
        RingFixNnMonitorLink& L = m.links[x.id];
        const bool step = x.k != 0 && std::abs(x.dev_if_m) <= max_if_m;
        if (step) {
            ++m.steps;
            ++L.epochs;
            L.k = x.k;
            L.sum_gf_m += x.dev_gf_m;
            L.sum_if_m += x.dev_if_m;
            if (L.first_tow < 0.0) L.first_tow = tow;
            L.last_tow = tow;
            if (!L.on) {
                L.on = true;
                ++L.episodes;
                edges.push_back({x.id, true, x.k, x.dev_gf_m, x.dev_if_m});
            }
        } else if (L.on) {
            L.on = false;
            edges.push_back({x.id, false, x.k, x.dev_gf_m, x.dev_if_m});
        }
    }
    return edges;
}

RingFixEntryVerdict ringFixEntryVerdict(const RingFixEntryEvidence& e, bool revalidate, double max_if_m) {
    if (!e.usable) return RingFixEntryVerdict::NotHeld;
    if (!revalidate) return RingFixEntryVerdict::Known;
    if (e.resumed) {
        if (!e.gap_measured) return RingFixEntryVerdict::GapUnchecked;
        if (e.gap_shifted) return RingFixEntryVerdict::GapShifted;
    }
    if (e.signature && e.signature_k != 0 && std::abs(e.signature_if_m) <= max_if_m)
        return RingFixEntryVerdict::Signature;
    return RingFixEntryVerdict::Known;
}

const char* ringFixEntryVerdictName(RingFixEntryVerdict v) {
    switch (v) {
        case RingFixEntryVerdict::NotHeld:      return "not_held";
        case RingFixEntryVerdict::Known:        return "known";
        case RingFixEntryVerdict::GapUnchecked: return "gap_unchecked";
        case RingFixEntryVerdict::GapShifted:   return "gap_shifted";
        case RingFixEntryVerdict::Signature:    return "signature";
    }
    return "unknown";
}

int ringFixCutBand(const RingFixCut& c, const RingFixLinkId& id) {
    if (!c.sys.empty() && c.sys != constellationKey(systemOfSat(id.sat))) return 0;
    return c.signal == id.sig1 ? 1 : (c.signal == id.sig2 ? 2 : 0);
}

RingFixCutOutcome ringFixApplyCuts(NetworkRingFix& rf, const std::vector<RingFixCut>& cuts, double now_s) {
    RingFixCutOutcome out;
    if (cuts.empty()) return out;
    out.kept_by_cut.resize(cuts.size());
    for (const RingFixCut& c : cuts) {
        ++rf.counters.cuts;
        if (c.known) {
            ++rf.counters.cuts_known;
            if (c.delta == 0) ++rf.counters.cuts_known_zero;
        }
        if (c.held_first) {
            ++rf.counters.cuts_held_first;
            if (c.known && c.delta == 0) ++rf.counters.cuts_held_first_zero;
        }
    }
    for (auto& [id, L] : rf.links) {
        if (!L.held) continue;
        long long d1 = 0, d2 = 0;
        bool unknown = false, touched = false;
        std::string rows, unknown_row;
        for (const RingFixCut& c : cuts) {
            const int band = ringFixCutBand(c, id);
            if (band == 0) continue;
            const bool in_s = c.sats.count(id.sat) > 0;
            const bool in_r = c.stations.count(id.station) > 0;
            if (in_s || in_r) touched = true;
            if (in_s == in_r) continue;   // both ends in M, or none: the integer is kept
            if (!c.known) { unknown = true; unknown_row = c.row; continue; }
            const long long step = in_s ? -c.delta : c.delta;
            (band == 1 ? d1 : d2) += step;
            if (rows.find(c.row) == std::string::npos) rows += (rows.empty() ? "" : "+") + c.row;
        }
        if (unknown) {
            ringFixUnholdLink(L, now_s, RingFixUnholdKind::CutRelease);
            ++rf.counters.released[static_cast<std::size_t>(RingFixRelease::Chain)];
            ++rf.counters.cut_released;
            out.released.emplace_back(id, unknown_row);
            continue;
        }
        if (d1 != 0 || d2 != 0) {
            L.n1 += d1;
            L.n2 += d2;
            ++rf.counters.reexpressed;
            out.reexpressed.emplace_back(id, d1, d2, rows);
        }
        for (std::size_t k = 0; k < cuts.size(); ++k) {
            const RingFixCut& c = cuts[k];
            if (ringFixCutBand(c, id) == 0) continue;
            if ((c.sats.count(id.sat) > 0) != (c.stations.count(id.station) > 0)) out.kept_by_cut[k].push_back(id);
        }
        if (touched) {
            L.rebase_stamps = true;
            ++out.touched;
        }
    }
    return out;
}

std::vector<RingFixEvent> ringFixInherit(NetworkRingFix& rf, const std::vector<RingFixInheritCand>& cands,
                                         const std::vector<RingFixCut>& cuts, double now_s) {
    std::vector<RingFixEvent> events;
    for (const RingFixInheritCand& c : cands) {
        if (c.latched) continue;
        RingFixLink& L = rf.links[c.id];
        if (L.held) continue;
        long long n1 = c.k1, n2 = c.k2;
        bool ok = true;
        for (const RingFixCut& cut : cuts) {
            const int band = ringFixCutBand(cut, c.id);
            if (band == 0) continue;
            const bool in_s = cut.sats.count(c.id.sat) > 0;
            const bool in_r = cut.stations.count(c.id.station) > 0;
            if (in_s == in_r) continue;
            if (!cut.known) { ok = false; break; }
            (band == 1 ? n1 : n2) += in_s ? -cut.delta : cut.delta;
        }
        if (!ok) continue;
        L.lambda1 = c.lambda1;
        L.lambda2 = c.lambda2;
        // The re-base flag is set: its carried entries were erased by the cut or are being re-created.
        ringFixHoldLink(rf, L, n1, n2, c.ring, c.root, now_s, RingFixHoldKind::Inherited, true, 0.0, 0.0);
        RingFixEvent ev;
        ev.kind = RingFixEvent::Kind::Hold;
        ev.id = c.id;
        ev.inherited = true;
        ev.gate.n1 = n1;
        ev.gate.n2 = n2;
        ev.gate.wl = n1 - n2;
        events.push_back(ev);
    }
    return events;
}

// ---- Truth (simulation only) ----------------------------------------------------------------------

long long ringFixWindupBranch(double w) { return static_cast<long long>(std::floor(w + 0.5)); }

void ringFixTruthObserve(RingFixTruth& T, int station, const std::string& sat, double tow) {
    auto& list = T.branches[{station, sat}];
    if (!list.empty() && tow - list.back().last_tow <= kRingFixArcGapS && tow >= list.back().last_tow) {
        list.back().last_tow = tow;
        return;
    }
    RingFixTruth::Branch b;
    b.start_tow = tow;
    b.last_tow = tow;
    const auto w = T.windup.find({station, sat});
    bool found = false;
    if (w != T.windup.end()) {
        const auto it = w->second.lower_bound(tow - 1e-3);
        if (it != w->second.end() && std::abs(it->first - tow) <= 1e-3) {
            b.k = ringFixWindupBranch(it->second);
            found = true;
        }
    }
    if (!found) ++T.arcs_without_windup;
    list.push_back(b);
}

bool ringFixTruthN(const RingFixTruth& T, int station, const std::string& sat, const std::string& signal,
                   double now_s, double tow, long long& n) {
    // A slip stamped at an epoch is in that epoch's observation; the epoch's clock can sit a few
    // microseconds before the stamp, so the bound takes half a second of slack.
    constexpr double kSlipStampSlackS = 0.5;
    const auto a = T.arcs.find({station, sat, signal});
    if (a == T.arcs.end()) return false;
    long long v = a->second.n_int;
    for (const auto& [s, d] : a->second.slips) {
        if (s > now_s + kSlipStampSlackS) break;
        v += d;
    }
    const auto b = T.branches.find({station, sat});
    if (b != T.branches.end()) {
        long long k = 0;
        for (const RingFixTruth::Branch& br : b->second) {
            if (br.start_tow <= tow + 1e-6) k = br.k;
            else break;
        }
        v += k;
    }
    n = v;
    return true;
}

bool ringFixTruthFrameInteger(const RingFixTruth& T,
                              const std::vector<std::tuple<int, std::string, std::string, int>>& terms,
                              double pin_sum_cyc, bool pins_integer, double now_s, double tow, long long& n) {
    if (!pins_integer || !std::isfinite(pin_sum_cyc)) return false;
    double total = -pin_sum_cyc;
    for (const auto& [sta, sat, sig, coeff] : terms) {
        long long m = 0;
        if (!ringFixTruthN(T, sta, sat, sig, now_s, tow, m)) return false;
        total += static_cast<double>(coeff) * static_cast<double>(m);
    }
    const double r = std::round(total);
    if (std::abs(total - r) > 1e-6) return false;
    n = std::llround(r);
    return true;
}

std::map<std::string, std::map<double, double>> ringFixParseWindupCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("ring fix truth: cannot open '" + path + "'");
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("ring fix truth: '" + path + "' is empty");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("week,tow_s,satellite,windup_cycles", 0) != 0)
        throw std::runtime_error("ring fix truth: '" + path
                                 + "': the header must begin week,tow_s,satellite,windup_cycles");
    std::map<std::string, std::map<double, double>> out;
    long long row = 1;
    while (std::getline(in, line)) {
        ++row;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string week, tow, sat, wu;
        if (!std::getline(ss, week, ',') || !std::getline(ss, tow, ',') || !std::getline(ss, sat, ',')
            || !std::getline(ss, wu, ','))
            throw std::runtime_error("ring fix truth: '" + path + "': malformed row " + std::to_string(row));
        try {
            out[sat][std::stod(tow)] = std::stod(wu);
        } catch (...) {
            throw std::runtime_error("ring fix truth: '" + path + "': bad number in row " + std::to_string(row));
        }
    }
    return out;
}

// ---- The near-null guard (network.ring_fix.nn_guard) -------------------------------------------

std::vector<RingFixNearNullVerdict> ringFixNearNullVerdicts(const std::vector<RingFixNearNullIn>& in, double tol_gf_m,
                                                            double max_if_m) {
    const auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        const std::size_t m = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
        const double hi = v[m];
        if (v.size() % 2 != 0) return hi;
        return 0.5 * (hi + *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m)));
    };
    // 1. Centred on the satellite's family: the median over the chords of the same satellite and pair.
    std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::size_t>> by_sat;
    for (std::size_t i = 0; i < in.size(); ++i) by_sat[{in[i].id.sat, in[i].id.sig1, in[i].id.sig2}].push_back(i);
    std::vector<double> d_gf(in.size(), 0.0), d_if(in.size(), 0.0);
    std::vector<char> grouped(in.size(), 0);
    for (const auto& [key, idx] : by_sat) {
        if (idx.size() < 4) continue;
        std::vector<double> g, f;
        for (std::size_t i : idx) { g.push_back(in[i].gf_m); f.push_back(in[i].if_m); }
        const double mg = median(g), mf = median(f);
        for (std::size_t i : idx) {
            d_gf[i] = in[i].gf_m - mg;
            d_if[i] = in[i].if_m - mf;
            grouped[i] = 1;
        }
    }
    // 2. Then on the receiver: the median of its grouped chords' remaining deviations.
    std::map<int, std::vector<std::size_t>> by_rx;
    for (std::size_t i = 0; i < in.size(); ++i)
        if (grouped[i]) by_rx[in[i].id.station].push_back(i);
    std::vector<RingFixNearNullVerdict> out;
    for (const auto& [rx, idx] : by_rx) {
        if (idx.size() < 3) continue;
        std::vector<double> g, f;
        for (std::size_t i : idx) { g.push_back(d_gf[i]); f.push_back(d_if[i]); }
        const double mg = median(g), mf = median(f);
        for (std::size_t i : idx) {
            RingFixNearNullVerdict v;
            v.id = in[i].id;
            v.dev_gf_m = d_gf[i] - mg;
            v.dev_if_m = d_if[i] - mf;
            if (in[i].step_m > 0.0) {
                v.k = static_cast<int>(std::lround(v.dev_gf_m / in[i].step_m));
                const bool fits = v.k != 0 && std::abs(v.dev_gf_m - v.k * in[i].step_m) <= tol_gf_m;
                v.act = fits && std::abs(v.dev_if_m) <= max_if_m;
                v.refused_if = fits && !v.act;
            }
            out.push_back(v);
        }
    }
    return out;
}

} // namespace GnssProcessor
