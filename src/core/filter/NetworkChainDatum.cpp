/**
 * @file NetworkChainDatum.cpp
 * @brief Card-3 Eq-34 single-receiver-chain phase-bias datum (see the header).
 *
 * Decomposition INC-8b (2026-07-22) — body relocated VERBATIM from
 * Application::runNetwork (dedent 16 -> 4, no other change).  The alias
 * preamble below is a byte-copy of the run TU's own `auto& X = ctx.carry.X;`
 * block, restricted to the 22 entries this body actually references (derived
 * mechanically, not by hand), so the relocated code reads the former local
 * names unchanged.
 */

#include "filter/NetworkChainDatum.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <tuple>
#include <utility>

namespace GnssProcessor {

void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            LogManager& log_manager_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            std::set<std::string>& shared_phase_keys) {
auto& prev_carry = ctx.carry.prev_carry;
auto& anchor_incumbent = ctx.carry.anchor_incumbent;
auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
auto& eq34_rx_parent = ctx.carry.eq34_rx_parent;
auto& eq34_sat_root = ctx.carry.eq34_sat_root;
auto& eq34_prev_el = ctx.carry.eq34_prev_el;
auto& eq34_ppp_utils = ctx.carry.eq34_ppp_utils;
auto& eq34_reref_total = ctx.carry.eq34_reref_total;
auto& eq34_erase_total = ctx.carry.eq34_erase_total;
auto& eq34_shift_total = ctx.carry.eq34_shift_total;
auto& eq34_gap_repin_total = ctx.carry.eq34_gap_repin_total;
auto& eq34_mismatch_epochs = ctx.carry.eq34_mismatch_epochs;
auto& eq34_retained_edges = ctx.carry.eq34_retained_edges;
auto& eq34_prev_target = ctx.carry.eq34_prev_target;
auto& eq34_reref = ctx.carry.eq34_reref;
auto& local_carry = ctx.carry.local_carry;
auto& rpb_carry = ctx.carry.rpb_carry;
const bool carry_invariant         = ctx.knobs.carry_invariant;
const bool eq34_shift_on = ctx.knobs.eq34_shift_on;
const bool eq34_longevity = ctx.knobs.eq34_longevity;
const bool eq34_extend_only = ctx.knobs.eq34_extend_only;

    const auto& sat_el      = cnd.sat_el;
    const auto& usable_keys = cnd.usable_keys;
    const auto& union_keys  = cnd.union_keys;
    const auto& cover       = cnd.cover;
    auto cov = [&](char c, size_t i) -> int {
        auto it = cover.find(c); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };

    // Card 3: Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN datum.  Per
    // (constellation,signal) connected component of the bipartite
    // (receiver × satellite) observation graph, elect a datum receiver
    // r1 (its RPB is pinned to 0) and pin the ambiguities on the edges
    // of a spanning tree rooted at r1.  Then every between-satellite
    // reference is an integer combination across the whole component —
    // including satellites r1 never sees — because each chain hop
    // contributes only an integer (the hop receiver's fractional bias
    // is its OWN estimable RPB state δ_{r,σ}, no longer part of the
    // pin).  Full-visibility regional case degenerates to the Odijk
    // Eq-34 star (CC-R).  The shared set is the ungated union (as
    // ensemble); the chain pins fix the enlarged (receiver+satellite)
    // gauge.  Per (const,σ) component with n_c stations, m_c sats:
    // (n_c+m_c−1) tree-edge ambiguity pins + 1 RPB pin = n_c+m_c =
    // the exact gauge dimension.  Every RPB-carrying station is a tree
    // vertex, so it gets ≥1 pin ⇒ its N_rr stays PD (the load-bearing
    // rank invariant, D3c).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();
    }
    this_epoch_anchor.clear();
    shared_phase_keys.clear();
    for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
    if (!eq34_ppp_utils) eq34_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    // This epoch's receiver-side parent edges + per-sat component roots;
    // become eq34_rx_parent / eq34_sat_root after the election (wholesale
    // replacement — the maps always describe exactly one epoch's tree).
    std::map<std::string, std::string> new_rx_parent, new_sat_root;
    // This epoch's pin targets (→ eq34_prev_target) and the carry
    // S-transformation shift maps collected by the per-component shift
    // walk: Δb per (sat,σ), Δδ per (station,cons,σ) / (station,rpb_key).
    std::map<std::pair<int, std::string>, double> new_prev_target;
    std::map<std::pair<std::string, std::string>, double> shift_sat;
    std::map<std::tuple<int, char, std::string>, double>  shift_rxg;
    std::map<std::pair<int, std::string>, double>         shift_rpb;

    const std::string eq34_pref_rx = config_.networkPhaseDatumEq34Receiver();
    // Card 4: the per-epoch datum targets are plain 0 (the frame is re-realized
    // freely; continuity moves to the invariant DD carry), so pin-to-round is off.
    const bool        eq34_round   = config_.networkPhaseDatumEq34PinRound()
                                  && !carry_invariant;
    auto eqSysOfChar = [](char c) -> System {
        switch (c) {
            case 'G': return System::GPS;    case 'E': return System::Galileo;
            case 'C': return System::BeiDou; case 'J': return System::QZSS;
            default:  return System::Unknown;
        }
    };

    // Group the ungated (sat,σ) keys into (constellation,signal)
    // sub-problems.  Recover (sat,σ) from the pbkey value so the
    // "sat_signal" '_' split is unambiguous.
    struct GKey {
        char cons; std::string signal;
        bool operator<(const GKey& o) const {
            return cons != o.cons ? cons < o.cons : signal < o.signal;
        }
    };
    std::map<GKey, std::set<std::string>>                      grp_sats;
    std::map<GKey, std::map<size_t, std::vector<std::string>>> grp_arcs;  // group -> station -> sats
    std::map<GKey, std::map<std::string, std::vector<size_t>>> grp_satrx; // group -> sat -> stations
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& [amb, pbkey] : usable_keys[i]) {
            if (!union_keys.count(amb)) continue;          // gated out
            const auto p = StateKeys::parseSatPhaseBiasKey(pbkey);
            if (!p || p->satellite.empty()) continue;
            const GKey g{ p->satellite.front(), p->signal };
            grp_sats[g].insert(p->satellite);
            grp_arcs[g][i].push_back(p->satellite);
            grp_satrx[g][p->satellite].push_back(i);
        }
    }

    // Rank bookkeeping (D3, theory ref §7.1): the recorded pins must
    // equal the exact gauge dimension Σ_components (n_c + m_c).  A cheap
    // always-on cross-check of the W5 null-space gate; logged below.
    int eq34_n_pins = 0, eq34_gauge_dim = 0;
    // Carry hygiene (D4): (sat,σ) whose chain path changed vs last
    // epoch — the emitted b_s re-references, so the phase-leveling local
    // carries for it are stale by an integer AND its D2/C1 shared PB
    // prior belongs to the retired frame.  Collected into the member
    // set (cleared here), erased below BEFORE PASS B consumes the local
    // carry, and marginalized at the shared-carry consume.
    eq34_reref.clear();
    for (auto& [g, sats] : grp_sats) {
        // Normal locals (NOT the structured bindings g/sats) so the
        // recordEdge lambda below can capture them — C++17 forbids
        // capturing structured bindings.
        const char        grp_cons = g.cons;
        const std::string grp_sig  = g.signal;
        const System      system  = eqSysOfChar(grp_cons);
        const std::string sys_str = PppUtils::getSystemString(*sats.begin());
        const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(sys_str, grp_sig);
        const std::string dr_key  = std::string(1, grp_cons) + "|" + grp_sig;
        auto& arcs  = grp_arcs[g];
        auto& satrx = grp_satrx[g];
        auto rV = [](size_t i){ return "R" + std::to_string(i); };
        auto sV = [](const std::string& s){ return "S" + s; };

        // I4 longevity score of arc (station ri, sat): higher = longer
        // expected life ⇒ a better NEW tree edge (fewer future deaths ⇒
        // fewer b_s re-references).  A RISING arc (elevation grew vs last
        // epoch, tracked in eq34_prev_el) will not set for a long time, so
        // it is lifted above EVERY setting arc (+90°, one full quadrant);
        // within a class higher elevation wins.  The churn source is the
        // low-elevation SETTING arc — this ranks it dead last.  Unread
        // unless eq34_longevity (sat_el/eq34_prev_el stay valid here).
        auto arcLife = [&](size_t ri, const std::string& sat) -> double {
            auto ei = sat_el[ri].find(sat);
            if (ei == sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
            const double el = ei->second;
            auto pe = eq34_prev_el.find({ri, sat});
            const bool rising = (pe != eq34_prev_el.end() && el > pe->second + 1e-6);
            return el + (rising ? 90.0 : 0.0);
        };

        // STICKY-tree seed (spec D2 step 3): the incumbent tree edges —
        // sat-side parents in anchor_incumbent ("sat_signal" → station),
        // receiver-side parents in eq34_rx_parent ("station|c|σ" → sat) —
        // that are still LIVE this epoch (station present in this group's
        // arcs and still tracking the satellite).  Both maps are replaced
        // wholesale each election, so together they are exactly last
        // epoch's tree: each edge in exactly one map, and any live subset
        // is a forest — seeding it can never close a cycle.
        std::set<std::pair<size_t, std::string>> arc_set;
        std::map<std::string, size_t> name2idx;
        for (const auto& [i, slist] : arcs) {
            name2idx[stations[i].name] = i;
            for (const auto& s : slist) arc_set.insert({i, s});
        }
        std::map<std::string, std::vector<std::string>> inc_adj;
        auto seedEdge = [&](const std::string& sta, const std::string& sat) {
            auto ni = name2idx.find(sta);
            if (ni == name2idx.end() || !arc_set.count({ni->second, sat})) return;
            inc_adj[rV(ni->second)].push_back(sV(sat));
            inc_adj[sV(sat)].push_back(rV(ni->second));
        };
        const std::string sig_suffix = "_" + grp_sig;
        for (const auto& [amb, sta] : anchor_incumbent) {
            if (amb.empty() || amb.front() != grp_cons) continue;
            if (amb.size() <= sig_suffix.size()
                || amb.compare(amb.size() - sig_suffix.size(),
                               sig_suffix.size(), sig_suffix) != 0) continue;
            seedEdge(sta, amb.substr(0, amb.size() - sig_suffix.size()));
        }
        const std::string rx_suffix =
            "|" + std::string(1, grp_cons) + "|" + grp_sig;
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (rxkey.size() <= rx_suffix.size()
                || rxkey.compare(rxkey.size() - rx_suffix.size(),
                                 rx_suffix.size(), rx_suffix) != 0) continue;
            seedEdge(rxkey.substr(0, rxkey.size() - rx_suffix.size()), sat);
        }

        // --- Union-find over vertices "R<idx>" / "S<sat>". ---
        std::map<std::string, std::string> uf;
        auto find = [&](std::string v) -> std::string {
            auto it = uf.find(v);
            while (it != uf.end() && it->second != v) { v = it->second; it = uf.find(v); }
            return v;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            if (!uf.count(a)) uf[a] = a;
            if (!uf.count(b)) uf[b] = b;
            const std::string ra = find(a), rb = find(b);
            if (ra != rb) uf[ra] = rb;
        };
        for (const auto& [i, slist] : arcs)
            for (const auto& s : slist) unite(rV(i), sV(s));

        std::map<std::string, std::vector<size_t>>      comp_rx;
        std::map<std::string, std::vector<std::string>> comp_sat;
        for (const auto& [i, slist] : arcs)  comp_rx[find(rV(i))].push_back(i);
        for (const auto& s : sats)           comp_sat[find(sV(s))].push_back(s);

        std::string best_datum_rx; size_t best_comp = 0;
        for (auto& [root, rx_list] : comp_rx) {
            std::sort(rx_list.begin(), rx_list.end());   // determinism

            // Elect r1: config pref → per-COMPONENT sticky (sat-root
            // vote) → group incumbent → highest coverage.
            size_t r1 = static_cast<size_t>(-1);
            if (!eq34_pref_rx.empty())
                for (size_t i : rx_list)
                    if (stations[i].name == eq34_pref_rx) { r1 = i; break; }
            // Per-COMPONENT stickiness: the component's sats remember
            // last epoch's root (eq34_sat_root) — re-elect the majority-
            // remembered station still present in this component.  The
            // group-level eq34_datum_rx slot below only remembers the
            // LARGEST component's r1, which left small fragments
            // re-electing by coverage every epoch (Jul-10 field: ~8
            // chronic (sat,σ) re-references per epoch, each a full
            // fragment re-level).  Vote ties break by station name.
            if (r1 == static_cast<size_t>(-1)) {
                std::map<std::string, int> root_votes;
                for (const auto& s : comp_sat[root]) {
                    auto rt = eq34_sat_root.find(s + "_" + grp_sig);
                    if (rt != eq34_sat_root.end()) ++root_votes[rt->second];
                }
                int best_v = 0; std::string best_name;
                for (const auto& [nm, v] : root_votes)
                    if (v > best_v || (v == best_v && nm < best_name)) {
                        best_v = v; best_name = nm;
                    }
                if (best_v > 0)
                    for (size_t i : rx_list)
                        if (stations[i].name == best_name) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                auto it = eq34_datum_rx.find(dr_key);
                if (it != eq34_datum_rx.end())
                    for (size_t i : rx_list)
                        if (stations[i].name == it->second) { r1 = i; break; }
            }
            if (r1 == static_cast<size_t>(-1)) {
                int best = -1;
                for (size_t i : rx_list) {
                    const int cv = cov(g.cons, i);
                    if (cv > best
                        || (cv == best && stations[i].name < stations[r1].name)) {
                        best = cv; r1 = i;
                    }
                }
            }
            if (r1 == static_cast<size_t>(-1)) continue;   // empty (unreachable)

            if (rx_list.size() > best_comp) {
                best_comp = rx_list.size();
                best_datum_rx = stations[r1].name;
            }

            // r1's RPB pin — the receiver half of the gauge (δ_{r1,σ}=0).
            stations[r1].pinned_rpb_keys.insert(rpb_key);
            ++eq34_n_pins;
            eq34_gauge_dim += static_cast<int>(rx_list.size()
                            + comp_sat[root].size());   // n_c + m_c

            // STICKY spanning tree (two-queue BFS from r1).  The
            // incumbent queue drains to exhaustion before any NEW graph
            // edge is explored, so a surviving old subtree — even one
            // detached upstream — is re-attached through its OWN old
            // edges (one new edge per detached fragment), never re-wired
            // vertex-by-vertex: a tree-edge pin moves only when its arc
            // actually died.  One ambiguity pin per tree edge; each
            // attachment records (child, parent, edge-changed) for the
            // dirty walk below.
            //
            // Records one tree edge (r,s): pin ambiguity z^s_r at station
            // r.  Target = 0 on a first-sighting edge; on a re-pin of an
            // already-lit (sat,σ) (pin-to-round), the |·|-minimizing
            // integer λ·round(carried float/λ) so the emitted-bias frame
            // step ≈ 0 cyc.  sat_is_child: which endpoint this edge
            // ATTACHES — the sat (parent-edge → this_epoch_anchor) or the
            // receiver (parent-edge → new_rx_parent).
            std::set<std::string> visited;
            std::deque<std::string> inc_q, new_q;
            // (child, parent, pinmove, known): pinmove = this pin's
            // exact displacement of its pinned float vs last epoch's
            // frame; known = false where the step is NOT computable
            // (re-pin without a carry; root handover — a common
            // FRACTIONAL step no integer transform can express).
            std::vector<std::tuple<std::string, std::string, double, bool>> order;
            auto recordEdge = [&](size_t ri, const std::string& sat,
                                  bool sat_is_child) {
                const std::string amb = sat + "_" + grp_sig;
                // I2 metric: this edge was retained across a gap (its
                // carry outlived the row absence) and is resuming now —
                // the incumbent seed made it a recognized edge, so the
                // pin-to-round below re-pins it to its pre-gap integer
                // (step ~0) instead of the first-sighting 0 a wholesale
                // replace would have forced.  Count the save.
                if (eq34_extend_only
                    && eq34_retained_edges.count({static_cast<int>(ri), amb}))
                    ++eq34_gap_repin_total;
                // Pin target: 0 on first sighting; else λ·round(carry/λ)
                // at the (possibly new) edge station.  carry_val also
                // feeds the pinmove computation below.
                double target = 0.0;
                double carry_val = std::numeric_limits<double>::quiet_NaN();
                if (eq34_round && anchor_incumbent.count(amb)) {
                    auto lc = local_carry.find(std::make_tuple(
                        static_cast<int>(ri), sat, grp_sig));
                    if (lc != local_carry.end() && system != System::Unknown) {
                        const double lam =
                            eq34_ppp_utils->getWavelength(system, grp_sig, sat);
                        if (std::isfinite(lam) && lam > 0.0) {
                            carry_val = lc->second.value_abs;
                            target = lam * std::round(carry_val / lam);
                        }
                    }
                }
                double pinmove = 0.0;
                bool known = true;
                bool edge_changed = false;
                if (sat_is_child) {
                    auto ai = anchor_incumbent.find(amb);
                    edge_changed = (ai != anchor_incumbent.end()
                                    && ai->second != stations[ri].name);
                    this_epoch_anchor[amb] = stations[ri].name;
                    // Root handover: the component frame steps by the
                    // datum receivers' fractional δ difference — not an
                    // integer, not computable here ⇒ unknown.
                    auto rt = eq34_sat_root.find(amb);
                    if (rt != eq34_sat_root.end()
                        && rt->second != stations[r1].name) known = false;
                    new_sat_root[amb] = stations[r1].name;
                } else {
                    const std::string rxkey = stations[ri].name + rx_suffix;
                    auto rp = eq34_rx_parent.find(rxkey);
                    edge_changed = (rp != eq34_rx_parent.end()
                                    && rp->second != sat);
                    new_rx_parent[rxkey] = sat;
                }
                if (edge_changed) {
                    // Re-pin at a NEW station: the float there sat at
                    // carry_val; the pin drives it to target — an exact,
                    // known displacement iff the carry exists.
                    if (std::isfinite(carry_val)) pinmove = target - carry_val;
                    else known = false;
                } else {
                    // Same station: its float was hard-pinned at last
                    // epoch's target, so the displacement is exactly the
                    // target difference (no carry noise involved).
                    auto pt = eq34_prev_target.find(
                        {static_cast<int>(ri), amb});
                    pinmove = target
                        - (pt != eq34_prev_target.end() ? pt->second : 0.0);
                }
                new_prev_target[{static_cast<int>(ri), amb}] = target;
                stations[ri].pinned_amb_targets[amb] = target;
                ++eq34_n_pins;
                order.emplace_back(sat_is_child ? sV(sat) : rV(ri),
                                   sat_is_child ? rV(ri) : sV(sat),
                                   pinmove, known);
            };
            auto attach = [&](const std::string& v) {
                visited.insert(v);
                inc_q.push_back(v);
                new_q.push_back(v);
            };
            attach(rV(r1));
            while (!inc_q.empty() || !new_q.empty()) {
                if (!inc_q.empty()) {
                    const std::string v = inc_q.front(); inc_q.pop_front();
                    auto ia = inc_adj.find(v);
                    if (ia == inc_adj.end()) continue;
                    for (const auto& u : ia->second) {
                        if (visited.count(u)) continue;
                        if (u[0] == 'S')
                            recordEdge(static_cast<size_t>(
                                           std::stoul(v.substr(1))),
                                       u.substr(1), /*sat_is_child=*/true);
                        else
                            recordEdge(static_cast<size_t>(
                                           std::stoul(u.substr(1))),
                                       v.substr(1), /*sat_is_child=*/false);
                        attach(u);
                    }
                } else {
                    const std::string v = new_q.front(); new_q.pop_front();
                    if (v[0] == 'R') {
                        const size_t ri =
                            static_cast<size_t>(std::stoul(v.substr(1)));
                        std::vector<std::string> ss = arcs[ri];
                        // I4: attach this receiver's longest-lived
                        // unvisited sats first (rising/high-elevation),
                        // so the NEW edge is picked to outlive the epoch.
                        // OFF = legacy sat-name order (byte-identical).
                        if (eq34_longevity)
                            std::sort(ss.begin(), ss.end(),
                                      [&](const std::string& a, const std::string& b) {
                                          const double la = arcLife(ri, a),
                                                       lb = arcLife(ri, b);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(ss.begin(), ss.end());
                        for (const auto& s : ss) {
                            if (visited.count(sV(s))) continue;
                            recordEdge(ri, s, /*sat_is_child=*/true);
                            attach(sV(s));
                        }
                    } else {
                        const std::string s = v.substr(1);
                        std::vector<size_t> rr = satrx[s];
                        // I4: attach this sat through its most durable
                        // unvisited tracker first — high per-constellation
                        // coverage (the robust receiver, spec D2 step 3),
                        // then the arc's own longevity at that station.
                        // OFF = legacy station-index order (byte-identical).
                        if (eq34_longevity)
                            std::sort(rr.begin(), rr.end(),
                                      [&](size_t a, size_t b) {
                                          const int ca = cov(grp_cons, a),
                                                    cb = cov(grp_cons, b);
                                          if (ca != cb) return ca > cb;
                                          const double la = arcLife(a, s),
                                                       lb = arcLife(b, s);
                                          if (la != lb) return la > lb;
                                          return a < b;   // deterministic tie-break
                                      });
                        else
                            std::sort(rr.begin(), rr.end());
                        for (size_t ri : rr) {
                            if (visited.count(rV(ri))) continue;
                            recordEdge(ri, s, /*sat_is_child=*/false);
                            attach(rV(ri));
                        }
                    }
                }
            }
            // Shift walk — the carry S-TRANSFORMATION (spec D4 "v2 =
            // SHIFT", realized at election: no emit-site plumbing
            // needed, the step is known here).  A pin displacement
            // propagates down the chain with alternating sign:
            //   Δb_s = −Δδ_parent − pinmove(edge)
            //   Δδ_r = −Δb_parent − pinmove(edge)
            // so the whole subtree's frame move is EXACT and every
            // carried quantity can be transformed into the new basis
            // (variances untouched — zero information loss) instead of
            // erased.  Erase/skip (eq34_reref) remains ONLY where the
            // step is unknown: a re-pin with no carry, or a root
            // handover (common fractional).  order is attachment-
            // ordered, parents strictly first — one forward pass.
            std::map<std::string, double> vshift;
            std::map<std::string, bool>   vknown;
            vshift[rV(r1)] = 0.0;
            vknown[rV(r1)] = true;
            for (const auto& [v, p, pinmove, known] : order) {
                // Shift OFF (default): any nonzero pin displacement is
                // treated as unknown ⇒ the erase/skip hygiene handles it
                // (the validated B4/B7-class behavior).  Shift ON: exact
                // transforms flow to the apply block below.
                const bool   k = known && vknown[p]
                              && (eq34_shift_on || pinmove == 0.0);
                const double s = k ? (-vshift[p] - pinmove) : 0.0;
                vshift[v] = s;
                vknown[v] = k;
                if (v[0] == 'S') {
                    const std::string sat = v.substr(1);
                    if (!k)          eq34_reref.insert({sat, grp_sig});
                    else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;
                } else {
                    const int idx =
                        static_cast<int>(std::stoul(v.substr(1)));
                    if (!k)
                        // Unknown frame step: the carried δ̂_{r,σ}
                        // belongs to an unrecoverable retired frame.
                        rpb_carry.erase({idx, rpb_key});
                    else if (s != 0.0) {
                        shift_rxg[std::make_tuple(idx, grp_cons,
                                                  grp_sig)] = s;
                        shift_rpb[{idx, rpb_key}] = s;
                    }
                }
            }
        }
        if (!best_datum_rx.empty()) eq34_datum_rx[dr_key] = best_datum_rx;
    }
    // Rank cross-check (D3): recorded pins == Σ (n_c + m_c).  A mismatch
    // means a component was mis-pinned before the W5 gate ever runs.
    if (eq34_n_pins != eq34_gauge_dim) {
        ++eq34_mismatch_epochs;
        log->warn("Network[eq34]: pin/gauge mismatch — recorded {} pins vs "
                  "gauge dimension {} (Σ n_c+m_c over components); the W5 "
                  "null-space gate will catch an under/over-constrained datum.",
                  eq34_n_pins, eq34_gauge_dim);
    } else
        log->debug("Network[eq34]: chain datum — {} pins == gauge dim {} "
                   "(1 RPB + tree edges per component).",
                   eq34_n_pins, eq34_gauge_dim);
    // Carry hygiene (D4): erase the phase-leveling local carries for any
    // re-referenced (sat,σ) at EVERY station, before PASS B consumes the
    // carry — the b_s frame stepped by an integer, so the carried floats
    // are stale.  Refloat next epoch (safe; the realized wire step is
    // usually 0 under pin-to-round, but erasing an already-integer carry
    // costs only one epoch's continuity).
    if (!eq34_reref.empty()) {
        eq34_reref_total += static_cast<long long>(eq34_reref.size());
        int n_erased = 0;
        for (auto it = local_carry.begin(); it != local_carry.end();) {
            const auto& key = it->first;   // (station idx, sat, signal)
            if (eq34_reref.count({std::get<1>(key), std::get<2>(key)})) {
                it = local_carry.erase(it);
                ++n_erased;
            } else {
                ++it;
            }
        }
        eq34_erase_total += n_erased;
        if (n_erased > 0)
            log->debug("Network[eq34]: {} local ambiguity carry(ies) erased for "
                       "{} re-referenced (sat,σ) — refloat next epoch.",
                       n_erased, eq34_reref.size());
    }
    // Carry S-transformation apply: shift every carried quantity into
    // this epoch's frame (Δz = −Δb_s − Δδ_r on the local ambiguity
    // carries; Δδ_r on the RPB carries; Δb_s on the shared PB carried
    // priors).  Variances untouched — this is an exact basis change,
    // not a refloat.  Runs BEFORE PASS B consumes any carry.
    if (!shift_sat.empty() || !shift_rxg.empty()) {
        long long n_shifted = 0;
        for (auto& [key, lc] : local_carry) {
            const int idx          = std::get<0>(key);
            const std::string& sat = std::get<1>(key);
            const std::string& sig = std::get<2>(key);
            double d = 0.0;
            auto bs = shift_sat.find({sat, sig});
            if (bs != shift_sat.end()) d -= bs->second;
            if (!sat.empty()) {
                auto dr = shift_rxg.find(
                    std::make_tuple(idx, sat.front(), sig));
                if (dr != shift_rxg.end()) d -= dr->second;
            }
            if (d != 0.0 && std::isfinite(d)) {
                lc.value_abs += d;
                ++n_shifted;
            }
        }
        for (auto& [key, lc] : rpb_carry) {
            auto it = shift_rpb.find(key);
            if (it != shift_rpb.end() && std::isfinite(it->second)
                && it->second != 0.0) {
                lc.value_abs += it->second;
                ++n_shifted;
            }
        }
        for (auto& cp : prev_carry) {
            if (cp.kind != SharedParamKind::PhaseBias) continue;
            auto bs = shift_sat.find({cp.sat, cp.signal});
            if (bs != shift_sat.end() && std::isfinite(bs->second)
                && bs->second != 0.0) {
                cp.value += bs->second;
                ++n_shifted;
            }
        }
        eq34_shift_total += n_shifted;
    }
    // Sticky baselines for next epoch: this epoch's tree becomes the
    // incumbent forest (sat parents + receiver parents, replaced
    // wholesale so the pair of maps always describes exactly one tree),
    // each sat remembers its component root for the r1-handover check,
    // and each pin remembers its target for the same-station pinmove.
    // anchor_incumbent doubles as the was-lit gate for pin-to-round
    // (every tree sat has exactly one parent edge).
    //
    // I2 (de Jonge §3.5 extend-only) — before the wholesale swap, MERGE
    // back the identity of any tree edge that was NOT seen this epoch but
    // whose phase-leveling memory (local_carry) is still alive: its
    // sat-side parent (this_epoch_anchor), receiver-side parent
    // (new_rx_parent), component root (new_sat_root) and pinned integer
    // (new_prev_target).  A momentary row gap is then NOT a dead arc — the
    // edge sits inertly in the incumbent maps (never seeded, because the
    // seed admits LIVE arcs only ⇒ the spanning-forest invariant holds),
    // and the epoch its arc RESUMES it is a recognized incumbent whose
    // pin-to-round re-pins to the SAME integer (emitted-bias step ≈ 0)
    // rather than re-leveling from a first-sighting 0.  local_carry
    // liveness is the age stamp: it is gap-pruned at phase_leveling_max
    // _gap_s, and a genuine reroute/handover erases it — so an edge past
    // that horizon (or truly re-referenced) is dropped and re-floats as a
    // first sighting, by which point the peer carries are gone too.  Keys
    // are all receiver-indexed pins {receiver idx, "sat_signal"}; the two
    // parent maps hold each edge in exactly one direction (unchanged).
    if (eq34_extend_only) {
        std::map<std::string, size_t> gname2idx;
        for (size_t i = 0; i < stations.size(); ++i)
            gname2idx[stations[i].name] = i;
        auto splitAmb = [](const std::string& a, std::string& sat,
                           std::string& sig) -> bool {
            const auto us = a.find('_');
            if (us == std::string::npos) return false;
            sat = a.substr(0, us); sig = a.substr(us + 1); return true;
        };
        auto carryAlive = [&](size_t idx, const std::string& sat,
                              const std::string& sig) -> bool {
            return local_carry.count(std::make_tuple(
                static_cast<int>(idx), sat, sig)) != 0;
        };
        eq34_retained_edges.clear();
        // Sat-side parents (amb → parent station) + their component roots.
        for (const auto& [amb, sta] : anchor_incumbent) {
            if (this_epoch_anchor.count(amb)) continue;    // re-seen live
            std::string sat, sig;
            if (!splitAmb(amb, sat, sig)) continue;
            auto gi = gname2idx.find(sta);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, sat, sig)) continue;  // dead ⇒ drop
            this_epoch_anchor.emplace(amb, sta);
            eq34_retained_edges.emplace(static_cast<int>(gi->second), amb);
            auto sr = eq34_sat_root.find(amb);
            if (sr != eq34_sat_root.end())
                new_sat_root.emplace(amb, sr->second);
        }
        // Receiver-side parents ("<name>|<c>|<σ>" → sat).
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (new_rx_parent.count(rxkey)) continue;
            const auto p2 = rxkey.rfind('|');
            if (p2 == std::string::npos || p2 == 0) continue;
            const auto p1 = rxkey.rfind('|', p2 - 1);
            if (p1 == std::string::npos) continue;
            const std::string name = rxkey.substr(0, p1);
            const std::string sig  = rxkey.substr(p2 + 1);
            auto gi = gname2idx.find(name);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, sat, sig)) continue;
            new_rx_parent.emplace(rxkey, sat);
            eq34_retained_edges.emplace(static_cast<int>(gi->second),
                                        sat + "_" + sig);
        }
        // Pinned integers ({receiver idx, "sat_signal"} → target).
        for (const auto& [k, tgt] : eq34_prev_target) {
            if (new_prev_target.count(k)) continue;
            std::string sat, sig;
            if (!splitAmb(k.second, sat, sig)) continue;
            if (!carryAlive(static_cast<size_t>(k.first), sat, sig)) continue;
            new_prev_target.emplace(k, tgt);
        }
    }
    anchor_incumbent = this_epoch_anchor;
    eq34_rx_parent   = std::move(new_rx_parent);
    eq34_sat_root    = std::move(new_sat_root);
    eq34_prev_target = std::move(new_prev_target);
    // I4: snapshot this epoch's arc elevations so next epoch's arcLife
    // can tell a rising arc from a setting one.  Rebuilt wholesale (a
    // pruned arc simply drops out); skipped entirely when longevity
    // election is off, so it stays empty on the B9 baseline.
    if (eq34_longevity) {
        eq34_prev_el.clear();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& [sat, el] : sat_el[i])
                eq34_prev_el[{i, sat}] = el;
        }
    }
}

} // namespace GnssProcessor
