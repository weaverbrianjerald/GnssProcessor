/**
 * @file NetworkChainDatum.cpp
 * @brief Card-3 Eq-34 single-receiver-chain phase-bias datum (see the header).
 *
 * The body was moved out of Application::runNetwork at decomposition INC-8b.  The
 * alias preamble below reproduces the run TU's local names for the entries this body
 * references: 19 `auto& X = ctx.carry.X;` references (zwd_carry and
 * eq34_cpl_sprev_total serve the §14 coupling re-base) and 7 `ctx.knobs` values.
 */

#include "filter/NetworkChainDatum.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkArcGraph.hpp"    // the group graph, its components and the chain forest (card AGT)
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkResponses.hpp"   // prmRespond / ResponseSite (card PRM phase 3a)
#include "filter/StateKeySchema.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, rinexLetter, constellationKey, SignalId, SatId, StationId, wavelengthM

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunTypes.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace GnssProcessor {

void eq34ShiftCarry(NetworkCarryState& carry,
                    const std::map<std::pair<std::string, std::string>, double>& shift_sat,
                    const std::map<std::tuple<int, char, std::string>, double>& shift_rxg,
                    const std::map<std::pair<int, std::string>, double>& shift_rpb,
                    long long& n_values, long long& n_terms, CarrySiteCounts* counts,
                    bool complete) {
    n_values = 0;
    n_terms  = 0;
    if (shift_sat.empty() && shift_rxg.empty()) return;
    // A carried satellite phase bias and every coupling term on it take the same step,
    // the one rule of a phase-bias shift (phaseBiasShiftOf: finite and nonzero).
    const auto satStep = [&](const std::string& sat, const std::string& sig, double& s) {
        s = phaseBiasShiftOf(shift_sat, sat, sig);
        return s != 0.0;
    };
    forEachCarriedReference(
        carry, carryMechanismSiteMask(CarryMechanism::Eq34Shift, complete),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::PhaseBias) return false;
            double d = 0.0;
            switch (r.site) {
                case CarrySite::AmbiguityValue:
                case CarrySite::HoldAmbiguityValue: {   // the held one: network.carry_rebase_complete
                    // -Δb_s - Δδ_r, formed in that order and added once.
                    auto bs = shift_sat.find({r.sat, r.signal});
                    if (bs != shift_sat.end()) d -= bs->second;
                    if (!r.sat.empty()) {
                        auto dr = shift_rxg.find(std::make_tuple(
                            r.station, letterOfSat(r.sat), r.signal));
                        if (dr != shift_rxg.end()) d -= dr->second;
                    }
                    if (!(d != 0.0 && std::isfinite(d))) return false;
                    if (r.site == CarrySite::AmbiguityValue) ++n_values;
                    break;
                }
                case CarrySite::PbLevelPrev:   // network.carry_rebase_complete: as the carried phase bias
                    if (!satStep(r.sat, r.signal, d)) return false;
                    break;
                case CarrySite::RxPhaseBiasValue: {
                    auto it = shift_rpb.find({r.station, r.signal});
                    if (it == shift_rpb.end() || !std::isfinite(it->second) || it->second == 0.0)
                        return false;
                    d = it->second;
                    ++n_values;
                    break;
                }
                case CarrySite::PrevCarry:
                    if (!satStep(r.sat, r.signal, d)) return false;
                    ++n_values;
                    break;
                default:   // a coupling term on a shifted satellite column (same sign as the carry)
                    if (!satStep(r.sat, r.signal, d)) return false;
                    ++n_terms;
                    break;
            }
            *r.value += d;
            return true;
        },
        counts);
}

void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            const AnchorHygiene& hyg,
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys) {
auto& prev_carry = ctx.carry.prev_carry;
auto& anchor_incumbent = ctx.carry.anchor_incumbent;
auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
auto& eq34_rx_parent = ctx.carry.eq34_rx_parent;
auto& eq34_sat_root = ctx.carry.eq34_sat_root;
auto& eq34_prev_el = ctx.carry.eq34_prev_el;
auto& eq34_reref_total = ctx.carry.eq34_reref_total;
auto& eq34_erase_total = ctx.carry.eq34_erase_total;
auto& eq34_shift_total = ctx.carry.eq34_shift_total;
auto& eq34_cpl_sprev_total = ctx.carry.eq34_cpl_sprev_total;
auto& eq34_gap_repin_total = ctx.carry.eq34_gap_repin_total;
auto& eq34_mismatch_epochs = ctx.carry.eq34_mismatch_epochs;
auto& eq34_retained_edges = ctx.carry.eq34_retained_edges;
auto& eq34_prev_target = ctx.carry.eq34_prev_target;
auto& eq34_reref = ctx.carry.eq34_reref;
auto& local_carry = ctx.carry.local_carry;
auto& rpb_carry = ctx.carry.rpb_carry;
auto& zwd_carry = ctx.carry.zwd_carry;
const bool carry_invariant         = ctx.knobs.carry_invariant;
const bool eq34_shift_on = ctx.knobs.eq34_shift_on;
const bool eq34_longevity = ctx.knobs.eq34_longevity;
const bool eq34_extend_only = ctx.knobs.eq34_extend_only;
const bool eq34_slip_aware = ctx.knobs.eq34_slip_aware;
const bool eq34_seed_round = ctx.knobs.eq34_seed_round;
const bool eq34_seed_rehost = ctx.knobs.eq34_seed_round_rehost;

    // Slip-aware election (Card 3/4): true when this arc slipped this epoch.  The
    // funnel is complete here by the ordering contract (every PASS-A drain, signal
    // integrity and measurement admission run before the election).  The key is
    // (station, ambiguity), the same as the C3c membership test, so the election and
    // the DD-prior build agree on which arcs are slipped.
    auto arcSlipped = [&](std::size_t ri, const std::string& sat,
                          const std::string& sig) -> bool {
        return eq34_slip_aware
            && carry_slipped_ambs.count({static_cast<int>(ri),
                                         StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
    };

    const auto& sat_el      = cnd.sat_el;
    const auto& usable_keys = cnd.usable_keys;
    const auto& union_keys  = cnd.union_keys;
    const auto& cover       = cnd.cover;
    auto cov = [&](System sys, size_t i) -> int {
        auto it = cover.find(rinexLetter(sys)); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };

    // Eq-34 / de Jonge single-receiver-chain datum (Card 3).  Per
    // (constellation,signal) connected component of the bipartite
    // (receiver × satellite) observation graph, elect a datum receiver
    // r1 (its RPB is pinned to 0) and pin the ambiguities on the edges
    // of a spanning tree rooted at r1.  Every between-satellite
    // reference is then an integer combination across the whole
    // component, including satellites r1 does not see, because each
    // chain hop contributes only an integer (the hop receiver's
    // fractional bias is its own estimable RPB state δ_{r,σ} and is not
    // part of the pin).  With full visibility (regional case) this is
    // the Odijk Eq-34 star (CC-R).  The shared set is the ungated union
    // (as in the retired ensemble datum); the chain pins fix the enlarged
    // (receiver+satellite) gauge.  Per (const,σ) component with n_c
    // stations and m_c sats: (n_c+m_c−1) tree-edge ambiguity pins + 1
    // RPB pin = n_c+m_c, the gauge dimension.  Every RPB-carrying
    // station is a tree vertex, so it gets ≥1 pin and its N_rr stays PD
    // (the rank invariant D3c).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (empty ⇒ no scaling)
        st.pinned_amb_seed_lambda.clear();   // seed-round fold-deferred (empty ⇒ inert)
    }
    this_epoch_anchor.clear();
    // Chain-pin audit (SPEC_synthetic_minimum_network §14 T1): this epoch's per-group
    // declared gauge dimension + shared-side bridge pins, replaced wholesale
    // exactly like the tree maps.  Read after PASS B's serial consume.
    ctx.carry.eq34_epoch_declared.clear();
    ctx.carry.eq34_epoch_bridged.clear();
    shared_phase_keys.clear();
    for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
    // This epoch's receiver-side parent edges + per-sat component roots;
    // they replace eq34_rx_parent / eq34_sat_root wholesale after the
    // election, so the maps always describe one epoch's tree.
    std::map<std::string, std::string> new_rx_parent, new_sat_root;
    // This epoch's pin targets (→ eq34_prev_target) and the carry
    // S-transformation shift maps collected by the per-component shift
    // walk: Δb per (sat,σ), Δδ per (station,cons,σ) / (station,rpb_key).
    std::map<std::pair<int, std::string>, double> new_prev_target;
    std::map<std::pair<std::string, std::string>, double> shift_sat;
    std::map<std::tuple<int, char, std::string>, double>  shift_rxg;
    std::map<std::pair<int, std::string>, double>         shift_rpb;

    const std::string eq34_pref_rx = config_.networkPhaseDatumEq34Receiver();
    // Under carry_invariant (Card 4) pin-to-round is off: the frame is re-realized
    // each epoch and continuity is held by the invariant DD carry.  (Seed-round,
    // below, can still round the targets.)
    const bool        eq34_round   = config_.networkPhaseDatumEq34PinRound()
                                  && !carry_invariant;
    // Group the ungated (sat,σ) keys into (constellation,signal)
    // sub-problems.  Recover (sat,σ) from the pbkey value so the
    // "sat_signal" '_' split is unambiguous.  The groups are visited in
    // (RINEX letter, signal) order.
    struct GKey {
        System sys; std::string signal;
        bool operator<(const GKey& o) const {
            const char a = rinexLetter(sys), b = rinexLetter(o.sys);
            return a != b ? a < b : signal < o.signal;
        }
    };
    std::map<GKey, std::set<std::string>>                      grp_sats;
    std::map<GKey, std::map<size_t, std::vector<std::string>>> grp_arcs;  // group -> station -> sats
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& [amb, pbkey] : usable_keys[i]) {
            if (!union_keys.count(amb)) continue;          // gated out
            const auto p = StateKeys::parseSatPhaseBiasKey(pbkey);
            if (!p || p->satellite.empty()) continue;
            const GKey g{ systemOfSat(p->satellite), p->signal };
            grp_sats[g].insert(p->satellite);
            grp_arcs[g][i].push_back(p->satellite);
        }
    }

    // Rank bookkeeping (D3, theory ref §7.1): the recorded pins must
    // equal the gauge dimension Σ_components (n_c + m_c).  A cheap
    // always-on cross-check of the W5 null-space gate; logged below.
    int eq34_n_pins = 0, eq34_gauge_dim = 0;
    // Carry hygiene (D4): (sat,σ) whose chain frame stepped and whose
    // step the shift walk below does not transform (unknown, or nonzero
    // with the carry shift off).  The emitted b_s re-references, so the
    // phase-leveling local carries for it are stale and its D2/C1 shared
    // PB prior belongs to the retired frame.  Collected into the member
    // set (cleared here), erased below before PASS B consumes the local
    // carry, and marginalized at the shared-carry consume.
    eq34_reref.clear();
    for (auto& [g, sats] : grp_sats) {
        // Plain locals rather than the structured bindings g/sats, so the
        // recordEdge lambda below can capture them (C++17 does not allow
        // capturing structured bindings).
        const System      grp_sys  = g.sys;
        const std::string grp_sig  = g.signal;
        // The pin-to-round and the seed-round below apply to the CDMA constellations
        // only: a GLONASS (FDMA) or SBAS group keeps the unrounded target.
        const bool        roundable = grp_sys == System::GPS || grp_sys == System::Galileo
                                   || grp_sys == System::BeiDou || grp_sys == System::QZSS;
        // The group signal's carrier wavelength for that rounding, from the band digit
        // alone as PppUtils::getWavelength reads it (BeiDou digit 1 is B1C there, where
        // SignalId::band reads L1I as B1I); 0 where the signal does not parse or the band
        // is unknown.  No FDMA group is rounded, so the slot is 0.
        double grp_lambda = 0.0;
        if (roundable)
            if (const auto signal = SignalId::fromObsCode(grp_sys, grp_sig))
                grp_lambda = wavelengthM(bandOfDigit(grp_sys, signal->digit), /*glo_slot=*/0);
        const std::string sys_str = systemName(grp_sys);
        const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(sys_str, grp_sig);
        const std::string dr_key  = constellationKey(grp_sys) + "|" + grp_sig;
        auto& arcs = grp_arcs[g];

        // Longevity score of arc (station ri, sat) (I4): higher means a longer
        // expected life and so a better new tree edge (fewer future deaths,
        // fewer b_s re-references).  A rising arc (elevation grew since last
        // epoch, tracked in eq34_prev_el) will not set soon, so it is lifted
        // above every setting arc (+90°, one quadrant); within a class the
        // higher elevation wins.  Low-elevation setting arcs cause the churn
        // and rank last.  Called only under eq34_longevity (sat_el and
        // eq34_prev_el are valid here).
        auto arcLife = [&](size_t ri, const std::string& sat) -> double {
            auto ei = sat_el[ri].find(sat);
            if (ei == sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
            const double el = ei->second;
            auto pe = eq34_prev_el.find({ri, sat});
            const bool rising = (pe != eq34_prev_el.end() && el > pe->second + 1e-6);
            // Slip-aware (Card 3/4): an arc that slipped this epoch is the worst new
            // tree edge at any elevation.  As a tree edge it invalidates every
            // fundamental cycle that closes through it, so the whole dependent chord set
            // refloats instead of the one DD the slip affects.  Demote it below every
            // clean arc (the clean band is [0,180]) and keep the ordering within the
            // slipped set, so that if the graph leaves no alternative the completion
            // picks the most durable slipped arc.  The score is read once per arc and
            // epoch, as the arc's durability in the group graph below, so the counter
            // counts each slipped arc once per epoch (card AGT step AGT-4; it counted
            // the sort's comparator calls before).
            const double life = el + (rising ? 90.0 : 0.0);
            if (arcSlipped(ri, sat, grp_sig)) {
                ++ctx.counters.eq34_slip_demoted;
                return life - 1000.0;
            }
            return life;
        };

        // The group's observation graph (card AGT step AGT-4, filter/NetworkArcGraph).
        // Station slots follow the roster index (ascending) and satellite slots the
        // group's string order, the vertex order of the string graph this replaced; the
        // arcs follow usable_keys, the order the components are united in.  Each arc
        // carries what the walk reads: its slip bit (the slip-aware election), its
        // longevity score (read only under the longevity election) and the hygiene's host
        // verdict.  A satellite name without a canonical SatId is left out of the graph
        // and warned about; none is expected, because the keys are built from canonical
        // names.
        std::map<std::string, SatId> sat_id;   // the group's satellite name -> its id
        int n_noncanonical = 0;
        for (const auto& s : sats) {
            const std::optional<SatId> k = SatId::parse(s);
            if (k && k->str() == s) sat_id.emplace(s, *k);
            else                    ++n_noncanonical;
        }
        if (n_noncanonical > 0)
            log->warn("Network[eq34]: {} satellite name(s) of group {} have no canonical "
                      "SatId; their arcs are left out of the chain datum's graph.",
                      n_noncanonical, dr_key);
        std::vector<StationId> station_order;
        for (const auto& [i, slist] : arcs) station_order.push_back(StationId{static_cast<int>(i)});
        std::vector<SatId> sat_order;
        for (const auto& [name, id] : sat_id) sat_order.push_back(id);
        std::vector<arcgraph::ArcRow> rows;
        for (const auto& [i, slist] : arcs)
            for (const auto& s : slist) {
                const auto id = sat_id.find(s);
                if (id == sat_id.end()) continue;
                arcgraph::ArcRow row;
                row.station       = StationId{static_cast<int>(i)};
                row.sat           = id->second;
                row.present       = 1;
                row.slipped       = arcSlipped(i, s, grp_sig) ? 1 : 0;
                row.durability    = eq34_longevity ? arcLife(i, s) : 0.0;
                row.eligible_host = hyg.eligibleHost(i, StateKeys::makeAmbiguityKey(s, grp_sig));
                rows.push_back(row);
            }
        arcgraph::ArcFamily family;
        family.sys = grp_sys;
        if (const auto signal = SignalId::fromObsCode(grp_sys, grp_sig)) family.bands.push_back(*signal);
        const arcgraph::ArcGraph graph =
            arcgraph::buildArcGraph(family, station_order, sat_order, rows);
        // A station slot's roster index and a satellite slot's name.
        auto staIdx = [&](int slot) {
            return static_cast<size_t>(graph.stations[static_cast<size_t>(slot)].index);
        };
        auto satName = [&](int slot) { return graph.sats[static_cast<size_t>(slot)].str(); };

        // Sticky-tree seed (spec D2 step 3): the incumbent tree edges
        // (sat-side parents in anchor_incumbent, "sat_signal" → station;
        // receiver-side parents in eq34_rx_parent, "station|c|σ" → sat)
        // that are still live this epoch (station present in this group's
        // arcs and still tracking the satellite).  Both maps are replaced
        // wholesale each election, so together they hold last epoch's tree
        // with each edge in one map.  Any live subset of a tree is a
        // forest, so seeding it cannot close a cycle.  The seed is a list of
        // the graph's arcs in seed order.
        std::map<std::string, int> slot_of_name;   // station name -> station slot
        for (int s = 0; s < static_cast<int>(graph.stations.size()); ++s)
            slot_of_name[stations[staIdx(s)].name] = s;
        std::map<std::string, int> slot_of_sat;    // satellite name -> satellite slot
        for (int k = 0; k < static_cast<int>(graph.sats.size()); ++k) slot_of_sat[satName(k)] = k;
        std::vector<int> incumbent_arcs;
        auto seedEdge = [&](const std::string& sta, const std::string& sat) {
            const auto ni = slot_of_name.find(sta);
            const auto ki = slot_of_sat.find(sat);
            if (ni == slot_of_name.end() || ki == slot_of_sat.end()) return;
            const int arc = arcgraph::arcIndex(graph, ni->second, ki->second);
            if (arc < 0) return;
            // Slip-aware (Card 3/4): a slipped incumbent loses its sticky seed.  The
            // incumbent queue drains before the new-edge walk, so a seeded edge is in
            // effect unconditional, and an incumbent that slipped this epoch should
            // leave the tree: every chord whose fundamental cycle runs through it is
            // lost with it.  Dropping the seed does not discard the edge; it moves it to
            // the new-edge walk, where the first pass refuses it (and arcLife ranks it
            // last under longevity), so it is re-admitted only if the component has no
            // other route to the vertex.  The resulting mass refloat is then forced by
            // the topology.
            if (arcSlipped(staIdx(ni->second), sat, grp_sig)) {
                ++ctx.counters.eq34_slip_deseeded;
                return;
            }
            incumbent_arcs.push_back(arc);
        };
        for (const auto& [amb, sta] : anchor_incumbent) {
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a || systemOfSat(a->satellite) != grp_sys || a->signal != grp_sig) continue;
            seedEdge(sta, a->satellite);
        }
        const std::string rx_suffix =
            "|" + constellationKey(grp_sys) + "|" + grp_sig;
        for (const auto& [rxkey, sat] : eq34_rx_parent) {
            if (rxkey.size() <= rx_suffix.size()
                || rxkey.compare(rxkey.size() - rx_suffix.size(),
                                 rx_suffix.size(), rx_suffix) != 0) continue;
            seedEdge(rxkey.substr(0, rxkey.size() - rx_suffix.size()), sat);
        }

        // The connected components, in the order of their roots.  Under the shared link
        // rule (a station is united under a satellite, arc by arc) every root is a
        // satellite, so the components come in the string order of their root
        // satellites; legacyChainVertexRank also orders the second pass's frontier
        // (receivers by the decimal string of their roster index, then satellites).
        const std::vector<int> chain_rank = arcgraph::legacyChainVertexRank(graph);
        const std::vector<arcgraph::Component> comps =
            arcgraph::arcComponents(graph, /*band_mask=*/1, chain_rank);

        // The walk's new-edge rules.  A new tree edge is grown only onto an arc whose
        // ambiguity has converged (age ≥ eq34_anchor_min_arc_ep; INC-1).  The walk runs
        // twice: the first pass admits eligible edges only, so a mature host wins over a
        // fresh one regardless of the I4/longevity rank order; if that leaves vertices
        // unreachable (their only connection to the component runs through a fresh arc),
        // the second pass admits everything and counts each unavoidable cold pin.  The
        // incumbent queue is not filtered: keeping the incumbent is not a reattach, and
        // blocking it would cause the handover this card is meant to prevent.
        // Slip-aware (Card 3/4): a slipped arc is refused in the first pass on the same
        // terms as a cold host, and admitted in the second.  The refusal is a rule of its
        // own, not only the arcLife ranking, because arcLife is read only under the
        // longevity election; relying on it would leave slip avoidance inactive wherever
        // longevity is off (its default).  Avoiding a slipped tree edge is a correctness
        // preference, not a longevity heuristic.  Without this refusal, de-seeding an
        // incumbent (above) would be worse than no action: the walk would hand the edge
        // straight back and pay a datum handover for it.  Under longevity a receiver tries
        // its longest-lived satellites first, and a satellite its most durable tracker
        // (highest per-constellation coverage, the robust receiver of spec D2 step 3, then
        // the arc's own longevity); off, the legacy satellite-name and station-index
        // orders.
        arcgraph::ForestRules rules;
        rules.band_mask               = 1;
        rules.refuse_ineligible_first = hyg.active && hyg.min_arc_ep > 0;
        rules.refuse_slipped_first    = eq34_slip_aware;
        rules.by_durability           = eq34_longevity;
        std::vector<int> station_rank;   // per station slot: the coverage
        for (int s = 0; s < static_cast<int>(graph.stations.size()); ++s)
            station_rank.push_back(cov(grp_sys, staIdx(s)));

        std::string best_datum_rx; size_t best_comp = 0;
        for (const arcgraph::Component& comp : comps) {
            // The component's stations (ascending roster index, as their slots are) and
            // satellites (string order).
            std::vector<size_t> rx_list;
            for (int s : comp.stations) rx_list.push_back(staIdx(s));
            std::vector<std::string> comp_sats;
            for (int k : comp.sats) comp_sats.push_back(satName(k));

            // Elect r1: config pref → per-component sticky (sat-root
            // vote) → group incumbent → highest coverage.
            size_t r1 = static_cast<size_t>(-1);
            if (!eq34_pref_rx.empty())
                for (size_t i : rx_list)
                    if (stations[i].name == eq34_pref_rx) { r1 = i; break; }
            // Per-component stickiness: the component's sats remember
            // last epoch's root (eq34_sat_root); re-elect the station most
            // of them remember, if it is still in this component.  The
            // group-level eq34_datum_rx slot below remembers only the
            // largest component's r1, so without this vote small fragments
            // would re-elect by coverage every epoch, each change a full
            // fragment re-level.  Vote ties break by station name.
            if (r1 == static_cast<size_t>(-1)) {
                std::map<std::string, int> root_votes;
                for (const auto& s : comp_sats) {
                    auto rt = eq34_sat_root.find(StateKeys::makeAmbiguityKey(s, grp_sig));
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
                    const int cv = cov(grp_sys, i);
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

            // r1's RPB pin: the receiver half of the gauge (δ_{r1,σ}=0).
            stations[r1].pinned_rpb_keys.insert(rpb_key);
            ++eq34_n_pins;
            eq34_gauge_dim += static_cast<int>(rx_list.size()
                            + comp_sats.size());   // n_c + m_c
            ctx.carry.eq34_epoch_declared[dr_key] +=
                static_cast<int>(rx_list.size() + comp_sats.size());

            // Sticky spanning tree (arcgraph::growChainForest, a two-queue BFS
            // from r1).  The incumbent queue drains completely before any new
            // graph edge is explored, so a surviving old subtree, even one
            // detached upstream, is re-attached through its own old edges (one
            // new edge per detached fragment) and is not re-wired vertex by
            // vertex: a tree-edge pin moves only when its arc died (or, under
            // slip-aware election, slipped).  One ambiguity pin per tree edge.
            // recordEdge is replayed over the forest's edges in attachment
            // order once the walk is done, which is exact because the walk
            // reads nothing recordEdge writes; each edge records (child,
            // parent, edge-changed) for the dirty walk below.
            //
            // recordEdge records one tree edge (r,s): pin ambiguity z^s_r
            // at station r.  Target = 0 on a first-sighting edge (unless
            // seed-round applies, below); on a re-pin of an already-lit
            // (sat,σ) (pin-to-round), the |·|-minimizing integer
            // λ·round(carried float/λ), so the emitted-bias frame step is
            // ≈ 0 cyc.  The edge's attach side selects which endpoint it
            // attaches: the sat (parent edge → this_epoch_anchor) or the
            // receiver (parent edge → new_rx_parent).
            //
            // (child, parent, pinmove, known, frame_shiftable), the child and
            // the parent as graph vertices: pinmove = this pin's exact
            // displacement of its pinned float vs last epoch's frame;
            // known = false where the step is not computable
            // (re-pin without a carry; root handover, a common fractional
            // step that no integer transform can express);
            // frame_shiftable (INC-2) = this step came from the new host's
            // own state and may be S-transformed even with the global
            // carry-shift knob off (always false unless INC-2 is armed).
            std::vector<std::tuple<int, int, double, bool, bool>> order;
            auto recordEdge = [&](const arcgraph::TreeEdge& edge) {
                const int         sta_slot     = graph.row_station[static_cast<size_t>(edge.arc)];
                const int         sat_slot     = graph.row_sat[static_cast<size_t>(edge.arc)];
                const size_t      ri           = staIdx(sta_slot);
                const std::string sat          = satName(sat_slot);
                const bool        sat_is_child = edge.attaches_sat;
                const std::string amb = StateKeys::makeAmbiguityKey(sat, grp_sig);
                // Slip-aware (Card 3/4): this arc is becoming a tree edge.  If it slipped
                // this epoch, the election could not avoid it (the component has no other
                // route to the vertex), and every cycle through it refloats.  Counted
                // separately so that "deseeded > 0" is not read as "no slipped arc ended
                // up in the tree".
                if (arcSlipped(ri, sat, grp_sig)) ++ctx.counters.eq34_slip_readmitted;
                // Gap re-pin metric (I2): this edge was retained across a gap
                // (its carry outlived the row absence) and is resuming now.
                // The incumbent seed made it a recognized edge, so the
                // pin-to-round below re-pins it to its pre-gap integer
                // (step ~0) instead of the first-sighting 0 a wholesale
                // replace would have used.  Counts these re-pins.
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
                    if (lc != local_carry.end() && roundable) {
                        const double lam = grp_lambda;
                        if (std::isfinite(lam) && lam > 0.0) {
                            carry_val = lc->second.value_abs;
                            target = lam * std::round(carry_val / lam);
                        }
                    }
                }
                // Seed-round (docs/SPEC_eq34_seed_round.md) applies when the
                // carry-round above did not engage (first sighting; incumbent whose
                // carry is dead; or every epoch under carry_invariant, where
                // eq34_round is off by the Card-4 design).  The target would then be
                // the cold absolute 0, which puts the seeded phase−code offset into
                // the datum frame (km-scale on the synthetic fixture; SPEC §12.8).
                // Instead, round the value the state holds, so |pin innovation| ≤ λ/2
                // by construction.  When the state does not exist yet (cold start or
                // new arc; PASS B's pre-pass allocates it), defer the rounding to the
                // fold via pinned_amb_seed_lambda; the realized target flows back into
                // eq34_prev_target through the serial consume.  `known` is cleared
                // below for deferred pins.
                bool seed_deferred = false, seed_rounded = false;
                // The new host's pre-pin float (INC-2, SPEC §8.5).  Finite only when
                // the state exists and INC-2 is armed; on a re-host it is this pin's
                // exact frame-step baseline (the pin moves the float from here to
                // `target`, so Δb_s = −(target − seed_x) exactly).
                double seed_x = std::numeric_limits<double>::quiet_NaN();
                bool   frame_shiftable = false;
                auto ambStateValue = [&]() -> double {
                    const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                    if (sidx < 0) return std::numeric_limits<double>::quiet_NaN();
                    return stations[ri].sm->getStateVector()(sidx);
                };
                if (eq34_seed_round && !std::isfinite(carry_val) && roundable) {
                    // Freeze after first realization (INC-1, SPEC §8.5): a frame is
                    // realized once.  Reuse this (station, amb)'s previously realized
                    // target unchanged (on the same host: pinmove 0, no step, no
                    // reref) and round only on a first realization.  Re-rounding
                    // every epoch ratchets the invariant carry (§13.7).
                    auto ptf = eq34_prev_target.find({static_cast<int>(ri), amb});
                    if (ptf != eq34_prev_target.end()) {
                        target = ptf->second;
                        seed_rounded = true;
                        ++ctx.counters.seed_round_frozen;
                        // A frozen target can still land on a re-hosted edge (the
                        // entry survived through the I2 retention or the fold
                        // feedback), and that step is equally computable, so read the
                        // pre-pin float here too (INC-2).  Only when armed, so with
                        // INC-2 off the freeze branch keeps its single map lookup.
                        if (eq34_seed_rehost) seed_x = ambStateValue();
                    } else {
                    const double lam = grp_lambda;
                    if (std::isfinite(lam) && lam > 0.0) {
                        const int sidx = stations[ri].sm->getAmbiguityIndex(amb);
                        if (sidx >= 0) {
                            const double x =
                                stations[ri].sm->getStateVector()(sidx);
                            if (std::isfinite(x)) {
                                if (eq34_seed_rehost) seed_x = x;   // re-host step baseline (INC-2)
                                target = lam * std::round(x / lam);
                                seed_rounded = true;
                                ++ctx.counters.seed_round_pins;
                                const double y = std::abs(target - x);
                                if (y > ctx.counters.seed_round_max_abs_y_m)
                                    ctx.counters.seed_round_max_abs_y_m = y;
                            }
                        } else {
                            stations[ri].pinned_amb_seed_lambda[amb] = lam;
                            seed_deferred = true;
                        }
                    }
                    }   // end of the freeze else-arm (INC-1)
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
                    // datum receivers' fractional δ difference, which is
                    // not an integer and not computable here, so unknown.
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
                    // Re-pin at a new station: the float there sat at
                    // carry_val and the pin drives it to target, an exact,
                    // known displacement iff the carry exists.
                    if (std::isfinite(carry_val)) pinmove = target - carry_val;
                    // No carry, but under seed-round (INC-2, SPEC §8.5) the
                    // station's own ambiguity state is the pre-pin value, and it
                    // is exact: it is the estimator's own number, not a leveling
                    // carry that can drift (the I1c mechanism that made carry_shift
                    // harmful in the field does not apply).  The frame step is
                    // therefore computable, and the edge stays shiftable even with
                    // the global carry-shift knob off.  That keeps the (sat,σ) out
                    // of eq34_reref, so its carried PB prior is folded
                    // (S-transformed) instead of marginalized for the epoch.
                    else if (eq34_seed_rehost && std::isfinite(seed_x)) {
                        pinmove = target - seed_x;
                        frame_shiftable = true;
                        ++ctx.counters.seed_round_rehost_framed;
                        const double st_abs = std::abs(pinmove);
                        if (st_abs > ctx.counters.seed_round_rehost_max_step_m)
                            ctx.counters.seed_round_rehost_max_step_m = st_abs;
                    } else {
                        known = false;
                        // Counted only when armed, so that with INC-2 off the
                        // run-end block stays byte-identical.
                        if (eq34_seed_rehost) ++ctx.counters.seed_round_rehost_cold;
                    }
                } else {
                    // Same station: its float was hard-pinned at last
                    // epoch's target, so the displacement is exactly the
                    // target difference (no carry noise involved).
                    auto pt = eq34_prev_target.find(
                        {static_cast<int>(ri), amb});
                    pinmove = target
                        - (pt != eq34_prev_target.end() ? pt->second : 0.0);
                }
                // Seed-round bookkeeping: a deferred pin's target is decided at
                // the fold, so no frame step is computable here (the serial
                // consume feeds the realized target back as next epoch's
                // baseline).  An election-side seed target that moved since last
                // epoch (the float crossed a half-integer, or a dropped station
                // lost last epoch's deferred feedback) is a frame re-realization,
                // not a transformable shift: it goes through the validated reref
                // hygiene, not the v2-shift walk.
                // INC-2 exempts the framed re-host: its step is an exactly
                // computed frame move, not a same-station target change, so it
                // takes the shift walk (and its own counter) instead of the reref
                // hygiene.
                if (seed_deferred) known = false;
                else if (seed_rounded && pinmove != 0.0 && !frame_shiftable) {
                    known = false;
                    ++ctx.counters.seed_round_target_steps;
                }
                new_prev_target[{static_cast<int>(ri), amb}] = target;
                stations[ri].pinned_amb_targets[amb] = target;
                ++eq34_n_pins;
                // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md): this
                // pin's host arc is (ri, amb) whichever endpoint the edge attaches.
                // Count the reattach (the sat-side host moved; NetworkSolPrep traces
                // this event as PHASE_DATUM/eq34_reattach) and, when the eligible
                // pass could not avoid a fresh host, give the pin fold an INC-3 σ
                // multiplier so the datum row is soft enough for the data to pull
                // the cold ambiguity instead of re-levelling the component onto its
                // code-seeded value.  Inert (no map entry, no counter) when the card
                // is off.
                if (hyg.active) {
                    if (edge_changed && sat_is_child) ++ctx.counters.hyg.reattaches;
                    const double sc = hyg.pinSigmaScale(ri, amb);
                    if (sc > 1.0) {
                        stations[ri].pinned_amb_sigma_scale[amb] = sc;
                        ++ctx.counters.hyg.fresh_pin_ramps;
                    }
                }
                const int rv = arcgraph::stationVertex(graph, sta_slot);
                const int sv = arcgraph::satVertex(graph, sat_slot);
                order.emplace_back(sat_is_child ? sv : rv, sat_is_child ? rv : sv,
                                   pinmove, known, frame_shiftable);
            };
            // eq34 reattach hygiene INC-2: a bridged satellite's gauge is realized on
            // the shared side, by a soft pseudo-obs row holding b_s at its last solved
            // level (networkInjectPinBridgePrior).  It enters the walk already
            // grounded: no parent edge, no ambiguity pin, and the walk may pass
            // through it to reach receivers.  The spanning structure is then a forest
            // rooted at {r1} ∪ {bridged sats}, and the rank arithmetic still closes:
            // 1 RPB pin + B bridge rows + (n_c + m_c − 1 − B) tree edges = n_c + m_c.
            // Bridge rows are counted as pins for that cross-check.
            const int r1_slot = comp.stations[static_cast<size_t>(
                std::find(rx_list.begin(), rx_list.end(), r1) - rx_list.begin())];
            std::vector<int> roots{arcgraph::stationVertex(graph, r1_slot)};
            if (hyg.active && !hyg.bridged.empty()) {
                for (int k : comp.sats) {
                    if (!hyg.isBridged(StateKeys::makeAmbiguityKey(satName(k), grp_sig))) continue;
                    roots.push_back(arcgraph::satVertex(graph, k));
                    ++eq34_n_pins;
                    // Audit: a bridge pin is realized on the shared side, so the
                    // station-side fold expectation is declared − bridged.
                    ++ctx.carry.eq34_epoch_bridged[dr_key];
                }
            }
            const arcgraph::SpanningForest forest = arcgraph::growChainForest(
                graph, comp, roots, incumbent_arcs, rules, chain_rank, station_rank);
            for (const arcgraph::TreeEdge& edge : forest.edges) {
                // A new edge on a non-eligible host is an unavoidable cold pin (the
                // second pass admitted it).
                if (edge.ineligible_host) ++ctx.counters.hyg.cold_fallback_pins;
                recordEdge(edge);
            }
            // Shift walk: the carry S-transformation (spec D4 "v2 =
            // SHIFT", realized at election, where the step is known, so
            // no emit-site plumbing is needed).  A pin displacement
            // propagates down the chain with alternating sign:
            //   Δb_s = −Δδ_parent − pinmove(edge)
            //   Δδ_r = −Δb_parent − pinmove(edge)
            // so the whole subtree's frame move is exact and every
            // carried quantity can be transformed into the new basis
            // (variances untouched, no information lost) instead of
            // erased.  Erase/skip (eq34_reref) remains only where the
            // step is not transformed: a re-pin with no carry, a root
            // handover (common fractional), and, with the shift off,
            // any nonzero step (see the loop).  `order` is in attachment
            // order, parents strictly first, so one forward pass works.
            // The vertices are graph slots.
            const size_t n_vertices = graph.stations.size() + graph.sats.size();
            std::vector<double> vshift(n_vertices, 0.0);
            std::vector<char>   vknown(n_vertices, 0);
            // r1 and every bridged satellite (INC-2) are roots of the walk.  A bridged
            // satellite's frame does not move either (the bridge row holds b_s at its
            // previous value), so it seeds the walk with a known zero shift.  The subtree
            // below it therefore keeps its carries instead of being re-referenced (no
            // eq34_reref entry, no local-carry erase); this is the purpose of the bridge.
            for (int r : roots) {
                vshift[static_cast<size_t>(r)] = 0.0;
                vknown[static_cast<size_t>(r)] = 1;
            }
            for (const auto& [v, p, pinmove, known, shiftable] : order) {
                // Shift off (default): any nonzero pin displacement is
                // treated as unknown, so the erase/skip hygiene handles it
                // (the validated B4/B7-class behavior).  Shift on: exact
                // transforms flow to the apply block below.  INC-2 adds a
                // third case for one edge class only, a seed-round re-host
                // whose step came from the new host's own state, and it
                // composes with the walk unchanged: the nonzero vshift it
                // seeds propagates through the (pinmove == 0) frozen edges
                // below it as it would in a shift-on run.
                const bool   k = known && vknown[static_cast<size_t>(p)]
                              && (eq34_shift_on || pinmove == 0.0 || shiftable);
                const double s = k ? (-vshift[static_cast<size_t>(p)] - pinmove) : 0.0;
                vshift[static_cast<size_t>(v)] = s;
                vknown[static_cast<size_t>(v)] = k ? 1 : 0;
                if (arcgraph::isSatVertex(graph, v)) {
                    const std::string sat =
                        satName(v - static_cast<int>(graph.stations.size()));
                    if (!k)          eq34_reref.insert({sat, grp_sig});
                    else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;
                } else {
                    const int idx = static_cast<int>(staIdx(v));
                    if (!k) {
                        // Unknown frame step: the carried δ̂_{r,σ}
                        // belongs to an unrecoverable retired frame.
                        // Card PRM phase 3a: counted only when an entry was erased.
                        if (rpb_carry.erase({idx, rpb_key}) != 0)
                            prmRespond(ctx.counters.prm_resp, ResponseSite::RpbEq34Erase);
                    } else if (s != 0.0) {
                        shift_rxg[std::make_tuple(idx, rinexLetter(grp_sys),
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
    // re-referenced (sat,σ) at every station, before PASS B consumes the
    // carry.  The b_s frame stepped by an amount the shift walk did not
    // transform, so the carried floats are stale.  They refloat next epoch
    // (safe: the realized wire step is usually 0 under pin-to-round, and
    // erasing an already-integer carry costs one epoch of continuity).
    if (!eq34_reref.empty()) {
        eq34_reref_total += static_cast<long long>(eq34_reref.size());
        // The first post-resume epoch's share of that total, the quantity the
        // datum-continuity card is graded on (RS-3, SPEC_event_harness §21.7).
        // Measure-only; the latch is cleared at the emit site, so a withheld first
        // epoch folds into the window.
        if (ctx.carry.resume_datum_first_ep)
            ctx.counters.n_resume_datum_first_reref +=
                static_cast<long long>(eq34_reref.size());
        int n_erased = 0;
        for (auto it = local_carry.begin(); it != local_carry.end();) {
            const auto& key = it->first;   // (station idx, sat, signal)
            if (eq34_reref.count({std::get<1>(key), std::get<2>(key)})) {
                it = local_carry.erase(it);
                ++n_erased;
                ++ctx.counters.n_cr_eq34_reref;   // carry-retirement census: frame step
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbEq34Erase);   // card PRM phase 3a
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
    // priors).  Variances untouched: this is an exact basis change,
    // not a refloat.  Runs before PASS B consumes any carry.
    //
    // Coupling references move with the carries (docs/SPEC_card1_coupling_carry.md
    // §9): D4 shifts `lc.value_abs` below and also every coupling `s_prev` on a
    // shifted PB column (SPEC_card1_coupling_carry §14).  A Card-1 coupling row's RHS is
    // y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j , and `CoupledShared::s_prev` and
    // `CarryPrior::value` hold the same number for the same identity (both are
    // `x_s(j)` at harvest: the NetworkLocalCarryHarvest.cpp s_prev assignment and the
    // prev_carry stash).  For a gauge-invariant row the two moves cancel under a
    // known frame step; shifting one reference without the other injects the frame
    // step as row bias.  The same rule holds at NetworkRelevel.cpp's Card-6 eph
    // cutover: the row's functional u = r + Mᵀs is frame-consistent only if all
    // carried references move together.
    //
    // The coupling walk sits outside the per-carry `d != 0` guard, because a carry
    // whose own (sat,σ) did not move can still hold coupling terms on other
    // satellites' shifted columns.  Only `shift_sat` participates: SharedParamKind
    // spans satellite-side + SH columns, so the receiver-side maps
    // (`shift_rxg`/`shift_rpb`) have no coupling image.  The D4 erase above does not
    // cover these keys: the erase set (`eq34_reref`) and the shift set (`shift_sat`)
    // are mutually exclusive by construction at the population site, so the shifted
    // keys are exactly those whose carries survive.  With coupling off the stashes
    // are empty and the walk leaves the output byte-identical.
    // The walk is the traversal of carried references (card PRM phase 2): the ambiguities,
    // the receiver phase biases and the satellite phase biases, then the s_prev of the
    // phase-bias coupling terms in the ambiguity, receiver phase-bias and wet-delay carries.
    if (!shift_sat.empty() || !shift_rxg.empty()) {
        CarrySiteCounts prm_cc;
        long long n_shifted = 0, n_cpl = 0;
        eq34ShiftCarry(ctx.carry, shift_sat, shift_rxg, shift_rpb, n_shifted, n_cpl, &prm_cc,
                       ctx.knobs.rebase_complete);
        if (!shift_sat.empty()) eq34_cpl_sprev_total += n_cpl;
        eq34_shift_total += n_shifted;
        // Card PRM phase 3b step 2: the held emissions of the shifted families, left in the old
        // frame (PbHold is exempt from the shift).  Measure-only.
        for (const auto& [hk, hv] : ctx.carry.pb_hold) {
            (void)hv;
            const auto bar = hk.find('|');
            if (bar == std::string::npos) continue;
            const auto sh = shift_sat.find({hk.substr(0, bar), hk.substr(bar + 1)});
            if (sh != shift_sat.end() && sh->second != 0.0)
                ++ctx.counters.prm_pbhold_unmoved_at_shift;
        }
        ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::Eq34Shift)];
        carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::Eq34Shift, prm_cc);
    }
    // Sticky baselines for next epoch: this epoch's tree becomes the
    // incumbent forest (sat parents + receiver parents, replaced
    // wholesale so the pair of maps always describes one tree), each
    // sat remembers its component root for the r1-handover check, and
    // each pin remembers its target for the same-station pinmove.
    // anchor_incumbent doubles as the was-lit gate for pin-to-round
    // (every tree sat has exactly one parent edge).
    //
    // I2 (de Jonge §3.5 extend-only): before the wholesale swap, merge
    // back the identity of any tree edge that was not seen this epoch but
    // whose phase-leveling memory (local_carry) is still alive: its
    // sat-side parent (this_epoch_anchor), receiver-side parent
    // (new_rx_parent), component root (new_sat_root) and pinned integer
    // (new_prev_target).  A momentary row gap then does not count as a
    // dead arc.  The edge sits inert in the incumbent maps (never seeded,
    // because the seed admits live arcs only, so the spanning-forest
    // invariant holds), and in the epoch its arc resumes it is a
    // recognized incumbent whose pin-to-round re-pins to the same integer
    // (emitted-bias step ≈ 0) instead of re-leveling from a first-sighting
    // 0.  local_carry liveness serves as the age stamp: it is gap-pruned at
    // phase_leveling_max_gap_s, and a genuine reroute or handover erases
    // it, so an edge past that horizon (or re-referenced) is dropped and
    // refloats as a first sighting, by which point the peer carries are
    // gone too.  Keys are receiver-indexed pins {receiver idx,
    // "sat_signal"}; the two parent maps hold each edge in one direction.
    if (eq34_extend_only) {
        std::map<std::string, size_t> gname2idx;
        for (size_t i = 0; i < stations.size(); ++i)
            gname2idx[stations[i].name] = i;
        auto carryAlive = [&](size_t idx, const std::string& sat,
                              const std::string& sig) -> bool {
            return local_carry.count(std::make_tuple(
                static_cast<int>(idx), sat, sig)) != 0;
        };
        eq34_retained_edges.clear();
        // Sat-side parents (amb → parent station) + their component roots.
        for (const auto& [amb, sta] : anchor_incumbent) {
            if (this_epoch_anchor.count(amb)) continue;    // re-seen live
            const auto a = StateKeys::parseAmbiguityKey(amb);
            if (!a) continue;
            auto gi = gname2idx.find(sta);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, a->satellite, a->signal)) continue;  // dead ⇒ drop
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
                                        StateKeys::makeAmbiguityKey(sat, sig));
        }
        // Pinned integers ({receiver idx, "sat_signal"} → target).
        for (const auto& [k, tgt] : eq34_prev_target) {
            if (new_prev_target.count(k)) continue;
            const auto a = StateKeys::parseAmbiguityKey(k.second);
            if (!a) continue;
            if (!carryAlive(static_cast<size_t>(k.first), a->satellite, a->signal)) continue;
            new_prev_target.emplace(k, tgt);
        }
    }
    anchor_incumbent = this_epoch_anchor;
    eq34_rx_parent   = std::move(new_rx_parent);
    eq34_sat_root    = std::move(new_sat_root);
    eq34_prev_target = std::move(new_prev_target);
    // Snapshot this epoch's arc elevations (I4) so next epoch's arcLife
    // can tell a rising arc from a setting one.  Rebuilt wholesale (a
    // pruned arc drops out); skipped when the longevity election is
    // off, so the map stays empty on the B9 baseline.
    if (eq34_longevity) {
        eq34_prev_el.clear();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& [sat, el] : sat_el[i])
                eq34_prev_el[{i, sat}] = el;
        }
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the eq34 chain datum: re-references, pin audit, seed round, slip-aware election.
int networkChainDatumRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& eq34_reref_total = v.ctx.carry.eq34_reref_total;
    const auto& eq34_erase_total = v.ctx.carry.eq34_erase_total;
    const auto& eq34_shift_total = v.ctx.carry.eq34_shift_total;
    const auto& eq34_cpl_sprev_total = v.ctx.carry.eq34_cpl_sprev_total;
    const auto& eq34_gap_repin_total = v.ctx.carry.eq34_gap_repin_total;
    const auto& eq34_mismatch_epochs = v.ctx.carry.eq34_mismatch_epochs;

    if (config_.networkPhaseDatumEq34())
        log->info("Network (Card 3/eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} coupling s_prev reference(s) re-based with them (§14 — on an "
                  "eq34+coupling lineage 0 here with nonzero shifts is the "
                  "wiring-inert alarm), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_cpl_sprev_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    // Chain-pin audit (docs/SPEC_synthetic_minimum_network.md §14 T1, measured record
    // §12.8): declared → folded → landed per (constellation, signal).  The election's
    // per-epoch cross-check only proves that the election recorded Σ (n_c + m_c) pins;
    // foldAmb/foldRpb return early without a message (no state, no live phase row), and
    // a station drop takes its folded pins with it, so "landed" is the number that
    // constrained the gauge.  A short group means the chain datum ran under-constrained
    // and the W5 near-null it is meant to remove is still open (the motivating case is
    // the W6+SH n-invariant L2W phase-bias common mode).  The number is reported rather
    // than inferred from the absence of a warning (invariant #4).
    if (config_.networkPhaseDatumEq34()) {
        const auto& pa = ctx.counters.eq34_pin_audit;
        if (pa.epochs == 0) {
            log->warn("Network (eq34 pin audit): ARMED but ZERO epochs audited — "
                      "every epoch aborted before the serial consume finished, or "
                      "the election never ran.  Nothing about the chain datum is "
                      "certified by this run.");
        }
        bool any_short = false;
        for (const auto& [grp, decl] : pa.declared) {
            auto at = [&](const std::map<std::string, long long>& m) -> long long {
                const auto it = m.find(grp); return it == m.end() ? 0 : it->second;
            };
            const int  n_short = [&]{ const auto it = pa.short_epochs.find(grp);
                                      return it == pa.short_epochs.end() ? 0 : it->second; }();
            const int  worst   = [&]{ const auto it = pa.worst_short.find(grp);
                                      return it == pa.worst_short.end() ? 0 : it->second; }();
            const long long bridged = at(pa.bridged);
            const long long landed  = at(pa.landed);
            if (n_short > 0) {
                any_short = true;
                log->warn("Network (eq34 pin audit) {}: SHORT in {} of {} epoch(s) "
                          "(worst −{} pins) — declared {} (−{} bridged), folded {}, "
                          "landed {}; fold refusals: {} no-state, {} no-row; {} "
                          "folded pin(s) lost to dropped stations.  The chain datum "
                          "ran UNDER-CONSTRAINED in those epochs.",
                          grp, n_short, pa.epochs, worst, decl, bridged,
                          at(pa.folded), landed, at(pa.miss_state), at(pa.miss_row),
                          at(pa.dropped_station));
            } else {
                log->info("Network (eq34 pin audit) {}: COMPLETE all {} epoch(s) — "
                          "declared {} (−{} bridged), folded {}, landed {} "
                          "(fold refusals: {} no-state, {} no-row; {} lost to "
                          "dropped stations).",
                          grp, pa.epochs, decl, bridged, at(pa.folded), landed,
                          at(pa.miss_state), at(pa.miss_row),
                          at(pa.dropped_station));
            }
        }
        if (pa.epochs > 0 && !any_short)
            log->info("Network (eq34 pin audit): every (constellation,signal) "
                      "group landed its full declared gauge dimension every "
                      "audited epoch — the chain pins are NOT the open near-null.");
    }
    // Seed-round run tallies (docs/SPEC_eq34_seed_round.md §5).  The assertion is
    // |pin innovation| ≤ λ/2 on every pin, which holds by construction of std::round
    // at both computation sites, so a violation is a logic error (stale λ map entry,
    // mismatched state), never a property of the data.
    if (config_.networkPhaseDatumEq34SeedRound()) {
        const auto& c = ctx.counters;
        const long long sr_total = c.seed_round_pins + c.seed_round_deferred;
        if (sr_total == 0) {
            log->warn("Network (eq34 seed-round): ARMED but ZERO pins were "
                      "seed-rounded — every elected pin took the carry-round or "
                      "legacy-0 path.  Standing invariant #4: that is INERT, not "
                      "validated.  Expected on a warm resume whose carries all "
                      "survived; on a COLD start it means the knob never engaged "
                      "— check the election wiring.");
        } else {
            log->info("Network (eq34 seed-round): {} pin target(s) seed-rounded "
                      "({} at the election from a live state, {} fold-deferred "
                      "for states born in PASS B, {} FROZEN reuses of a "
                      "previously realized target — INC-1), max |pin innovation| "
                      "{:.4f} m, {} same-station target step(s) (each routed "
                      "through the reref hygiene, never the v2-shift walk).",
                      sr_total, c.seed_round_pins, c.seed_round_deferred,
                      c.seed_round_frozen,
                      c.seed_round_max_abs_y_m, c.seed_round_target_steps);
            if (c.seed_round_max_abs_y_m > 0.13)
                log->warn("Network (eq34 seed-round): max |pin innovation| "
                          "{:.4f} m EXCEEDS λ/2 of every GNSS phase band "
                          "(≤ 0.1274 m) — that is a LOGIC ERROR in the seed-round "
                          "target computation, not a data property.  Do not "
                          "trust this run's datum realization.",
                          c.seed_round_max_abs_y_m);
        }
        // Re-host ledger (§8.5, INC-2).  Reported only when armed, so a twin with the
        // knob off keeps a run-end block byte-identical to a build without it.
        if (config_.networkPhaseDatumEq34SeedRoundRehostFrame()) {
            const long long rh = c.seed_round_rehost_framed
                               + c.seed_round_rehost_cold;
            if (rh == 0)
                log->warn("Network (eq34 seed-round INC-2): ARMED but NO tree pin "
                          "ever re-hosted without a carry — the frame-continuous "
                          "re-host path never ran.  Standing invariant #4: INERT, "
                          "not validated.");
            else
                log->info("Network (eq34 seed-round INC-2): {} carry-less re-host(s) "
                          "— {} FRAMED (step taken from the new host's own state, "
                          "carries S-transformed, the (sat,signal) kept out of "
                          "eq34_reref) and {} COLD (no allocated state ⇒ legacy "
                          "unknown step + reref).  Max |frame step| {:.4f} m: that "
                          "is bounded by λ/2 only while the new host's float sits on "
                          "the realized frame, so read a large value as evidence the "
                          "solution had drifted off it, NOT as a health violation.",
                          rh, c.seed_round_rehost_framed,
                          c.seed_round_rehost_cold,
                          c.seed_round_rehost_max_step_m);
        }
    }
    if (config_.networkPhaseDatumEq34SlipAwareElection()) {
        const long long ds = ctx.counters.eq34_slip_deseeded;
        const long long ra = ctx.counters.eq34_slip_readmitted;
        log->info("Network (Card 3/4 slip-aware election): {} slipped incumbent(s) DE-SEEDED "
                  "from the sticky tree, {} arcLife demotion(s) (slipped arcs ranked last, "
                  "each counted once per epoch; inert without the longevity election), "
                  "and {} slipped arc(s) became a tree edge "
                  "ANYWAY because the component had no other route to the vertex.  A slipped "
                  "TREE edge refloats every fundamental cycle through it; a slipped CHORD "
                  "costs one DD — so (de-seeded − re-admitted) is the mass-refloat this "
                  "converted to a rank-1 kill, and the re-admitted count is the share the "
                  "graph topology, not the policy, still forces.",
                  ds, ctx.counters.eq34_slip_demoted, ra);
        if (ds == 0)
            log->warn("Network (Card 3/4): the slip-aware election is ARMED but DE-SEEDED "
                      "NOTHING — no slipped arc was ever an incumbent tree edge this run.  "
                      "Standing invariant #4: that is INERT, not proof the policy works.  "
                      "Check that slips reached the funnel at all (the slip-QC lines above); "
                      "with few slips and a dense graph a zero here is genuinely expected.");
        else if (ra >= ds)
            log->warn("Network (Card 3/4): the slip-aware election de-seeded {} arc(s) but {} "
                      "slipped arc(s) still ended up in the tree — the graph, not the "
                      "election, is the binding constraint here and this knob is buying "
                      "little.  A denser observation graph (or a per-component redundancy "
                      "check) is the lever, not this policy.", ds, ra);
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
