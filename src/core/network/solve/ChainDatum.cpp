/**
 * @file ChainDatum.cpp
 * @brief Eq-34 single-receiver-chain phase-bias datum (see the header).
 *
 * The body was moved out of Application::runNetwork at decomposition INC-8b.  The
 * alias preamble below reproduces the run TU's local names for the entries this body
 * references: `auto& X = ctx.carry.X;` references (eq34_cpl_sprev_total serves the §14
 * coupling re-base), the entity tables of the station carries (the
 * receiver phase-bias carry and the receiver-side eq34 parents) and 7 `ctx.knobs` values.
 */

#include "network/solve/ChainDatum.hpp"

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/ar/ArcGraph.hpp"    // the group graph, its components and the chain forest
#include "network/ar/ArcLife.hpp"     // the arcs' life stamps (the frame observatory)
#include "network/solve/ChainFrame.hpp"   // ChainEpochObs (the frame observatory)
#include "network/carry/Coupling.hpp"
#include "network/assembly/Responses.hpp"   // prmRespond / ResponseSite
#include "filter/StateKeySchema.hpp"
#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, rinexLetter, constellationKey, SignalId, SatId, StationId, wavelengthM

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StationParams.hpp"   // the station's parameter set: the election's ambiguity values
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "network/solve/Estimator.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include <fmt/format.h>
#include "network/carry/LocalCarryHarvest.hpp"   // HarvestBorder, StationHarvestBlock
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "network/diag/AmbStateTrace.hpp"   // AmbStateRow / ambStateNoteErase (RF-1 §7.4)
#include "network/ar/AmbFloat.hpp"       // ambFloatBirthFloor (RF-1 step 4)
#include "network/carry/CarryVce.hpp"
#include "network/assembly/ParamInventory.hpp" // rxIfbFamilyReseeded
#include "network/qc/PriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "network/solve/NormalEquations.hpp"
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan
#include <set>

namespace GnssProcessor {

void eq34ShiftCarry(NetworkCarryState& carry, net::NetworkEntities& entities,
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
        carry, entities, carryMechanismSiteMask(CarryMechanism::Eq34Shift, complete),
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

namespace {

// =====================================================================================
//  The chain election's helpers.  networkElectChainDatum (below) walks them in order:
//  group the usable keys, form the families, then per family build its graph, seed the
//  incumbent tree, elect the datum receivers and grow the forests, and per band record
//  the pins and walk the frame steps; the last step replaces the sticky baselines.
// =====================================================================================

/// "No receiver elected".
constexpr std::size_t kNoReceiver = static_cast<std::size_t>(-1);

/// The (constellation, phase signal) group of a usable key; groups are visited in
/// (RINEX letter, signal) order.
struct ChainGroupKey {
    System      sys;
    std::string signal;
    bool operator<(const ChainGroupKey& o) const {
        const char a = rinexLetter(sys), b = rinexLetter(o.sys);
        return a != b ? a < b : signal < o.signal;
    }
};

/// The usable keys of the present stations, grouped by (constellation, phase signal).
struct ChainGroups {
    std::map<ChainGroupKey, std::set<std::string>>                      sats;  ///< group -> its satellites
    std::map<ChainGroupKey, std::map<size_t, std::vector<std::string>>> arcs;  ///< group -> station -> satellites, in usable-key order
};

/// One phase signal of a family, with the constants the election reads for it.
struct ChainBand {
    std::string signal;            ///< The phase signal ("L1C").
    bool        roundable{false};  ///< A CDMA constellation: its pin targets are rounded to whole cycles.
    double      lambda_m{0.0};     ///< The carrier wavelength for that rounding (0: none).
    std::string rpb_key;           ///< The receiver phase-bias key of the signal.
    std::string dr_key;            ///< "<constellation key>|<signal>": the audit and observatory group.
    std::string rx_suffix;         ///< "|<constellation key>|<signal>": the suffix of a receiver-parent key.
};

/// One election family: a constellation with one band, or with the two bands of its L1/L2 pair.
/// A family grows its spanning forest over all of its bands, so a pair family pins the same
/// tree edges on both.  Bit b of every band mask is band b of @ref bands.
struct ChainFamily {
    System                 sys{System::Unknown};
    std::vector<ChainBand> bands;   ///< One or two, in the order of the signal strings.

    bool         pair() const { return bands.size() == 2; }
    std::uint8_t mask() const { return static_cast<std::uint8_t>((1u << bands.size()) - 1u); }
    /// The family kind as the traces name it.
    const char*  kind() const { return pair() ? "pair" : "signal"; }
    /// "G|L1C" for a one-band family, "G|L1C+L2W" for a pair.
    std::string  label() const {
        std::string s = bands.front().dr_key;
        for (std::size_t b = 1; b < bands.size(); ++b) s += "+" + bands[b].signal;
        return s;
    }
};

/// What every step of one election reads and writes: the caller's parameters, the tables and
/// knobs they resolve to, the observatory and the results the last step replaces the sticky
/// baselines with.
struct ChainElection {
    ChainElection(NetworkRunContext& ctx_, const GnssConfig& config,
                  const std::shared_ptr<spdlog::logger>& log_, std::vector<NetStation>& stations_,
                  const std::vector<bool>& present_, const PhaseDatumCandidates& cnd_,
                  const AnchorHygiene& hyg_,
                  const std::set<std::pair<int, std::string>>& carry_slipped_ambs_,
                  EpochEvents& events_, net::NetworkEntities& entities_, net::StationTable& sta_rows_,
                  ChainEpochObs& chain_obs_)
        : ctx(ctx_), log(log_), stations(stations_), present(present_), cnd(cnd_), hyg(hyg_),
          carry_slipped_ambs(carry_slipped_ambs_), events(events_), entities(entities_),
          sta_rows(sta_rows_), sig_rows(entities_.sat_signals), arc_rows(entities_.arcs),
          this_epoch_anchor(events_.phase_datum.this_epoch_anchor),
          eq34_reref(events_.phase_datum.eq34_reref), chain_obs(chain_obs_),
          shift_on(ctx_.knobs.eq34_shift_on), longevity(ctx_.knobs.eq34_longevity),
          extend_only(ctx_.knobs.eq34_extend_only), slip_aware(ctx_.knobs.eq34_slip_aware),
          seed_round(ctx_.knobs.eq34_seed_round), seed_rehost(ctx_.knobs.eq34_seed_round_rehost),
          pair_family(ctx_.knobs.eq34_pair_family), trace_chain(ctx_.knobs.trace_chain_datum),
          pref_rx(config.networkPhaseDatumEq34Receiver()),
          // Under carry_invariant pin-to-round is off: the frame is re-realized each epoch and
          // continuity is held by the invariant DD carry.  (Seed-round, below, can still round
          // the targets.)
          pin_round(config.networkPhaseDatumEq34PinRound() && !ctx_.knobs.carry_invariant) {
        if (trace_chain)
            for (size_t i = 0; i < stations.size(); ++i)
                chain_index_of[stations[i].name] = static_cast<int>(i);
    }

    NetworkRunContext&                            ctx;
    const std::shared_ptr<spdlog::logger>&        log;
    std::vector<NetStation>&                      stations;
    const std::vector<bool>&                      present;
    const PhaseDatumCandidates&                   cnd;
    const AnchorHygiene&                          hyg;
    const std::set<std::pair<int, std::string>>&  carry_slipped_ambs;
    EpochEvents&                                  events;
    net::NetworkEntities&                         entities;   ///< eq34_rx_parent: the station signals; datum_rx: the constellation channels
    net::StationTable&                            sta_rows;   ///< rpb: the station signals
    const net::SatSignalTable&                    sig_rows;   ///< the incumbent anchors and the component roots, written only by the replacements at the end
    const net::ArcTable&                          arc_rows;   ///< the arcs' retained edges and pin targets
    std::map<std::string, std::string>&           this_epoch_anchor;
    std::set<std::pair<std::string, std::string>>& eq34_reref;
    ChainEpochObs&                                chain_obs;  ///< the frame observatory's record (a stand-in with the observatory off)

    const bool        shift_on;
    const bool        longevity;
    const bool        extend_only;
    const bool        slip_aware;
    const bool        seed_round;
    const bool        seed_rehost;
    const bool        pair_family;
    const bool        trace_chain;
    const std::string pref_rx;
    const bool        pin_round;

    std::map<std::string, int> chain_index_of;   ///< station name -> roster index (observatory only)

    // This epoch's receiver-side parent edges and per-satellite component roots; they replace
    // eq34_rx_parent / eq34_sat_root wholesale after the election, so the maps always describe
    // one epoch's tree.
    std::map<std::string, std::string> new_rx_parent, new_sat_root;
    // This epoch's pin targets (to eq34_prev_target) and the carry S-transformation shift maps
    // collected by the shift walks: the step per (satellite, signal), per (station, constellation,
    // signal) and per (station, receiver phase-bias key).
    std::map<std::pair<int, std::string>, double>         new_prev_target;
    std::map<std::pair<std::string, std::string>, double> shift_sat;
    std::map<std::tuple<int, char, std::string>, double>  shift_rxg;
    std::map<std::pair<int, std::string>, double>         shift_rpb;
    // Rank bookkeeping: the recorded pins must equal the gauge dimension, the sum over components of
    // n_c + m_c.
    int n_pins = 0, gauge_dim = 0;
    // The tree edges kept across a row gap while their carry lives (extend-only): replaces the arcs' retained edges.
    std::set<std::pair<int, std::string>> new_retained_edges;
};

/// The coverage count of a station in a constellation (0 when unknown).
int chainCoverage(const ChainElection& el, System sys, std::size_t i) {
    const auto it = el.cnd.cover.find(rinexLetter(sys));
    if (it == el.cnd.cover.end()) return 0;
    const auto jt = it->second.find(i);
    return jt == it->second.end() ? 0 : jt->second;
}

/// True when this arc slipped this epoch under the slip-aware election.  The funnel is complete
/// by the ordering contract (every PASS-A drain, signal integrity and measurement admission run
/// before the election).  The key is (station, ambiguity), the same as the C3c membership test,
/// so the election and the DD-prior build agree on which arcs are slipped.
bool chainArcSlipped(const ChainElection& el, std::size_t ri, const std::string& sat,
                     const std::string& sig) {
    return el.slip_aware
        && el.carry_slipped_ambs.count({static_cast<int>(ri), StateKeys::makeAmbiguityKey(sat, sig)}) != 0;
}

/// Longevity score of arc (station ri, sat) (I4): higher means a longer expected life and so a
/// better new tree edge (fewer future deaths, fewer b_s re-references).  A rising arc (elevation
/// grew since last epoch, tracked in eq34_prev_el) will not set soon, so it is lifted above every
/// setting arc (+90 deg, one quadrant); within a class the higher elevation wins.  Low-elevation
/// setting arcs cause the churn and rank last.  Called only under the longevity election (sat_el
/// and eq34_prev_el are valid then).
///
/// Slip-aware: an arc that slipped this epoch is the worst new tree edge at any elevation.  As a
/// tree edge it invalidates every fundamental cycle that closes through it, so the whole dependent
/// chord set refloats instead of the one DD the slip affects.  It is demoted below every clean arc
/// (the clean band is [0,180]) and the ordering within the slipped set is kept, so that if the
/// graph leaves no alternative the completion picks the most durable slipped arc.  The score is
/// read once per arc and band and epoch, as the arc's durability in the family graph, so the
/// counter counts each slipped arc once per band and epoch.
double chainArcLife(ChainElection& el, std::size_t ri, const std::string& sat, const std::string& sig) {
    auto ei = el.cnd.sat_el[ri].find(sat);
    if (ei == el.cnd.sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
    const double elevation = ei->second;
    const double* pe = net::getLos(el.entities.los, static_cast<int>(ri), sat,
                                   &net::LosState::eq34_prev_el);
    const bool rising = (pe != nullptr && elevation > *pe + 1e-6);
    const double life = elevation + (rising ? 90.0 : 0.0);
    if (chainArcSlipped(el, ri, sat, sig)) {
        ++el.ctx.counters.eq34_slip_demoted;
        return life - 1000.0;
    }
    return life;
}

/// Group the ungated (sat, signal) keys of the present stations into (constellation, signal)
/// groups.  (sat, signal) is recovered from the phase-bias key value so the "sat_signal" '_'
/// split is unambiguous.
ChainGroups groupChainKeys(ChainElection& el) {
    ChainGroups groups;
    for (size_t i = 0; i < el.stations.size(); ++i) {
        if (!el.present[i]) continue;
        for (const auto& [amb, pbkey] : el.cnd.usable_keys[i]) {
            if (!el.cnd.union_keys.count(amb)) {                  // gated out
                if (el.trace_chain) ++el.chain_obs.gated_keys;
                continue;
            }
            const auto p = net::satPhaseBiasKey(pbkey);
            if (!p || p->sat == SatId{}) continue;
            const std::string pb_sat = net::satKey(p->sat);
            const ChainGroupKey key{ p->sat.sys, net::signalKey(p->signal) };
            groups.sats[key].insert(pb_sat);
            groups.arcs[key][i].push_back(pb_sat);
        }
    }
    return groups;
}

/// The band of a group: its signal, rounding wavelength and keys.
ChainBand makeChainBand(System sys, const std::string& signal) {
    ChainBand band;
    band.signal = signal;
    // The pin-to-round and the seed-round apply to the CDMA constellations only: a GLONASS
    // (FDMA) or SBAS group keeps the unrounded target.
    band.roundable = sys == System::GPS || sys == System::Galileo
                  || sys == System::BeiDou || sys == System::QZSS;
    // The signal's carrier wavelength for that rounding, from the band digit alone as the
    // traits table declares it (BeiDou digit 1 is B1C there, where SignalId::band reads L1I as
    // B1I); 0 where the signal does not parse or the band is unknown.  No FDMA group is
    // rounded, so the slot is 0.
    if (band.roundable)
        if (const auto s = SignalId::fromObsCode(sys, signal))
            band.lambda_m = nominalWavelengthOfDigitM(sys, s->digit);
    band.rpb_key   = StateKeys::makeReceiverPhaseBiasKey(systemName(sys), signal);
    band.dr_key    = constellationKey(sys) + "|" + signal;
    band.rx_suffix = "|" + constellationKey(sys) + "|" + signal;
    return band;
}

/// The families of this epoch, in visit order (RINEX letter, first signal).  With the pair
/// family off every group is its own one-band family, in group order.  With it on, a
/// constellation that tracks exactly two phase signals of different bands forms one pair family
/// of them (in signal-string order); any other constellation is elected per signal this epoch.
std::vector<ChainFamily> formChainFamilies(ChainElection& el, const ChainGroups& groups) {
    std::vector<ChainFamily> families;
    const auto oneBand = [&](System sys, const std::string& signal) {
        ChainFamily f;
        f.sys = sys;
        f.bands.push_back(makeChainBand(sys, signal));
        families.push_back(std::move(f));
    };
    for (auto it = groups.sats.begin(); it != groups.sats.end();) {
        const System sys = it->first.sys;
        std::vector<std::string> signals;   // this constellation's, in signal-string order
        for (; it != groups.sats.end() && it->first.sys == sys; ++it) signals.push_back(it->first.signal);
        bool pair = false;
        if (el.pair_family && signals.size() == 2) {
            const auto a = SignalId::fromObsCode(sys, signals[0]);
            const auto b = SignalId::fromObsCode(sys, signals[1]);
            pair = a && b && a->digit != b->digit;
        }
        if (pair) {
            ChainFamily f;
            f.sys = sys;
            for (const std::string& s : signals) f.bands.push_back(makeChainBand(sys, s));
            families.push_back(std::move(f));
            ++el.ctx.counters.eq34_pair_families;
            continue;
        }
        if (el.pair_family) ++el.ctx.counters.eq34_pair_fallbacks;
        for (const std::string& s : signals) oneBand(sys, s);
    }
    return families;
}

/// A family's observation graph with the name lookups of its slots.
struct ChainGraph {
    arcgraph::ArcGraph        graph;
    std::map<std::string, int> slot_of_name;   ///< station name -> station slot
    std::map<std::string, int> slot_of_sat;    ///< satellite name -> satellite slot

    /// A station slot's roster index.
    size_t staIdx(int slot) const {
        return static_cast<size_t>(graph.stations[static_cast<size_t>(slot)].index);
    }
    /// A satellite slot's name.
    std::string satName(int slot) const { return graph.sats[static_cast<size_t>(slot)].str(); }
    /// The number of vertices (stations, then satellites).
    size_t nVertices() const { return graph.stations.size() + graph.sats.size(); }
};

/// The family's observation graph (network/ar/ArcGraph.hpp).  Station slots follow the roster
/// index (ascending) over every station with an arc on any of the family's bands, and satellite
/// slots the union of the bands' satellites in string order, the vertex order of the string
/// graph this replaced.  The rows follow band by band, each band's arcs station-major in
/// usable-key order, the order the components are united in; an arc both bands carry merges into
/// its first band's row.  Each row carries what the walk reads: the band's presence bit, its slip
/// bit (the slip-aware election), its longevity score (read only under the longevity election)
/// and the hygiene's host verdict.  A satellite name without a canonical SatId is left out of the
/// graph and warned about; none is expected, because the keys are built from canonical names.
ChainGraph buildChainGraph(ChainElection& el, const ChainGroups& groups, const ChainFamily& fam) {
    std::set<std::string> sats;
    std::set<size_t> stations;
    for (const ChainBand& band : fam.bands) {
        const ChainGroupKey key{fam.sys, band.signal};
        const auto& band_sats = groups.sats.at(key);
        sats.insert(band_sats.begin(), band_sats.end());
        for (const auto& [i, slist] : groups.arcs.at(key)) stations.insert(i);
    }
    std::map<std::string, SatId> sat_id;   // the family's satellite name -> its id
    int n_noncanonical = 0;
    for (const auto& s : sats) {
        const std::optional<SatId> k = SatId::parse(s);
        if (k && k->str() == s) sat_id.emplace(s, *k);
        else                    ++n_noncanonical;
    }
    if (n_noncanonical > 0)
        el.log->warn("Network[eq34]: {} satellite name(s) of group {} have no canonical "
                     "SatId; their arcs are left out of the chain datum's graph.",
                     n_noncanonical, fam.label());
    std::vector<StationId> station_order;
    for (size_t i : stations) station_order.push_back(StationId{static_cast<int>(i)});
    std::vector<SatId> sat_order;
    for (const auto& [name, id] : sat_id) sat_order.push_back(id);
    std::vector<arcgraph::ArcRow> rows;
    for (std::size_t b = 0; b < fam.bands.size(); ++b) {
        const ChainBand& band = fam.bands[b];
        const auto bit = static_cast<std::uint8_t>(1u << b);
        for (const auto& [i, slist] : groups.arcs.at(ChainGroupKey{fam.sys, band.signal}))
            for (const auto& s : slist) {
                const auto id = sat_id.find(s);
                if (id == sat_id.end()) continue;
                arcgraph::ArcRow row;
                row.station       = StationId{static_cast<int>(i)};
                row.sat           = id->second;
                row.present       = bit;
                row.slipped       = chainArcSlipped(el, i, s, band.signal) ? bit : 0;
                row.durability    = el.longevity ? chainArcLife(el, i, s, band.signal) : 0.0;
                row.eligible_host = el.hyg.eligibleHost(i, StateKeys::makeAmbiguityKey(s, band.signal));
                rows.push_back(row);
            }
    }
    arcgraph::ArcFamily family;
    family.sys = fam.sys;
    for (const ChainBand& band : fam.bands)
        if (const auto signal = SignalId::fromObsCode(fam.sys, band.signal)) family.bands.push_back(*signal);
    ChainGraph cg;
    cg.graph = arcgraph::buildArcGraph(family, station_order, sat_order, rows);
    for (int s = 0; s < static_cast<int>(cg.graph.stations.size()); ++s)
        cg.slot_of_name[el.stations[cg.staIdx(s)].name] = s;
    for (int k = 0; k < static_cast<int>(cg.graph.sats.size()); ++k) cg.slot_of_sat[cg.satName(k)] = k;
    return cg;
}

/// The incumbent tree arcs of a family, in seed order.
struct ChainSeeds {
    std::vector<int>              family;  ///< Every band's incumbent arcs: what the family's forest is seeded with.
    std::vector<std::vector<int>> band;    ///< Pair family only: per band, the arcs of that band's own maps (the band extension's seed).
};

/// Sticky-tree seed (spec D2 step 3): the incumbent tree edges (satellite-side parents in
/// anchor_incumbent, "sat_signal" -> station; receiver-side parents in eq34_rx_parent,
/// "station|c|signal" -> satellite) that are still live this epoch (the station is in the
/// family's arcs and still tracks the satellite).  Both maps are replaced wholesale each
/// election, so together they hold last epoch's tree with each edge in one map.  Any live subset
/// of a tree is a forest, so seeding it cannot close a cycle.  The seed is a list of the graph's
/// arcs in seed order: for each band in turn, its anchor_incumbent arcs, then its receiver
/// parents in station-name order.
///
/// Slip-aware: a slipped incumbent loses its sticky seed.  The incumbent queue drains before the
/// new-edge walk, so a seeded edge is in effect unconditional, and an incumbent that slipped this
/// epoch should leave the tree: every chord whose fundamental cycle runs through it is lost with
/// it.  Dropping the seed does not discard the edge; it moves it to the new-edge walk, where the
/// first pass refuses it (and arcLife ranks it last under longevity), so it is re-admitted only
/// if the component has no other route to the vertex.  The resulting mass refloat is then forced
/// by the topology.  In a pair family an arc is slipped when either band's integer may have
/// changed, an arc named by both bands' maps is seeded and counted once, and a de-seeded arc
/// counts once however many maps named it.  In a one-band family every map entry counts.
ChainSeeds seedIncumbents(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg) {
    ChainSeeds seeds;
    seeds.band.resize(fam.bands.size());
    std::set<int> in_family, deseeded;
    for (std::size_t b = 0; b < fam.bands.size(); ++b) {
        const ChainBand& band = fam.bands[b];
        std::set<int> in_band;
        const auto seedEdge = [&](const std::string& sta, const std::string& sat) {
            const auto ni = cg.slot_of_name.find(sta);
            const auto ki = cg.slot_of_sat.find(sat);
            if (ni == cg.slot_of_name.end() || ki == cg.slot_of_sat.end()) return;
            const int arc = arcgraph::arcIndex(cg.graph, ni->second, ki->second);
            if (arc < 0) return;
            if (arcgraph::slippedIn(cg.graph, arc, fam.mask())) {
                if (!fam.pair() || deseeded.insert(arc).second) ++el.ctx.counters.eq34_slip_deseeded;
                return;
            }
            if (!fam.pair()) { seeds.family.push_back(arc); return; }
            if (in_band.insert(arc).second) seeds.band[b].push_back(arc);
            if (in_family.insert(arc).second) seeds.family.push_back(arc);
        };
        for (const auto& [sid, srow] : el.sig_rows) {
            if (!srow.anchor_incumbent) continue;
            const std::string& sta = *srow.anchor_incumbent;
            if (sid.sat == SatId{} || sid.signal == SignalId{}) continue;   // a row that names no arc
            if (sid.sat.sys != fam.sys || net::signalKey(sid.signal) != band.signal) continue;
            seedEdge(sta, net::satKey(sid.sat));
        }
        // The receiver-side parents of this band, in station-name order (O3: the order of the
        // "<name>|<c>|<sigma>" keys they had).
        net::byStationName(el.entities.stations, el.entities.names,
                           [&](const StationId& id, const net::StationState&) {
            const std::string* sat = net::get(el.entities.stations, id, &net::StationState::signals,
                                              band.rx_suffix.substr(1),
                                              &net::StationSignalState::eq34_rx_parent);
            if (sat) seedEdge(el.entities.names.names[id.index], *sat);
        });
    }
    return seeds;
}

/// The datum receiver r1 of a component, from the bands it is elected over: the configured
/// receiver; else the per-component sticky vote; else the remembered datum receiver of the first
/// band that has one, if it names a station of the component; else the highest coverage.  The vote: the
/// component's satellites remember last epoch's root (eq34_sat_root, per band); the station most
/// of them remember is re-elected if it is still in this component (every band of the family
/// votes, so a satellite votes once per band).  The group-level datum_rx slot remembers only the
/// largest component's r1, so without this vote small fragments would re-elect by coverage every
/// epoch, each change a full fragment re-level.  Vote ties break by station name.
std::size_t electDatumReceiver(const ChainElection& el, System sys, const ChainBand* bands,
                               std::size_t n_bands, const std::vector<size_t>& rx_list,
                               const std::vector<std::string>& comp_sats) {
    std::size_t r1 = kNoReceiver;
    if (!el.pref_rx.empty())
        for (size_t i : rx_list)
            if (el.stations[i].name == el.pref_rx) { r1 = i; break; }
    if (r1 == kNoReceiver) {
        std::map<std::string, int> root_votes;
        for (std::size_t b = 0; b < n_bands; ++b)
            for (const auto& s : comp_sats) {
                const std::string* rt = net::getSig(el.sig_rows,
                                                    StateKeys::makeAmbiguityKey(s, bands[b].signal),
                                                    &net::SatSignalState::eq34_sat_root);
                if (rt != nullptr) ++root_votes[*rt];
            }
        int best_v = 0; std::string best_name;
        for (const auto& [nm, v] : root_votes)
            if (v > best_v || (v == best_v && nm < best_name)) {
                best_v = v; best_name = nm;
            }
        if (best_v > 0)
            for (size_t i : rx_list)
                if (el.stations[i].name == best_name) { r1 = i; break; }
    }
    for (std::size_t b = 0; b < n_bands && r1 == kNoReceiver; ++b) {
        const std::string* drx = net::get(el.entities.constellations, sys,
                                          &net::ConstellationState::channels, bands[b].signal,
                                          &net::ChannelState::datum_rx);
        if (drx == nullptr) continue;
        for (size_t i : rx_list)
            if (el.stations[i].name == *drx) { r1 = i; break; }
        break;   // the first band that has one decides; a receiver outside this component leaves it to the coverage
    }
    if (r1 == kNoReceiver) {
        int best = -1;
        for (size_t i : rx_list) {
            const int cv = chainCoverage(el, sys, i);
            if (cv > best
                || (cv == best && el.stations[i].name < el.stations[r1].name)) {
                best = cv; r1 = i;
            }
        }
    }
    return r1;
}

/// One component with its elected datum receiver and its grown forest: what a band records.
struct ChainComponentPlan {
    std::size_t              r1{kNoReceiver};  ///< The datum receiver (roster index).
    std::vector<size_t>      rx_list;          ///< The component's stations (ascending roster index, as their slots are).
    std::vector<std::string> sats;             ///< Its satellites (string order).
    std::vector<int>         vertices;         ///< Its vertices: the station slots, then the satellite slots.
    std::vector<int>         bridged;          ///< Satellite slots grounded by a pin bridge (one-band family only).
    std::vector<int>         roots;            ///< The forest's roots, as vertices: r1, then the bridged satellites.
    arcgraph::SpanningForest forest;           ///< The spanning forest of the component.
};

/// The satellites of a component whose gauge a pin-bridge row realizes on the shared side
/// (eq34 reattach hygiene): a bridged satellite enters the walk already grounded, with no
/// parent edge and no ambiguity pin, and the walk may pass through it to reach receivers.  The
/// pair family never has one (the loader refuses the combination), so its trees are rooted at
/// the datum receiver alone.
std::vector<int> bridgedSlots(const ChainElection& el, const ChainFamily& fam, const ChainGraph& cg,
                              const arcgraph::Component& comp) {
    std::vector<int> slots;
    if (fam.pair() || !el.hyg.active || el.hyg.bridged.empty()) return slots;
    for (int k : comp.sats)
        if (el.hyg.isBridged(StateKeys::makeAmbiguityKey(cg.satName(k), fam.bands[0].signal)))
            slots.push_back(k);
    return slots;
}

/// Elect the datum receiver of a component and grow its spanning forest (the sticky two-queue
/// BFS of arcgraph::growChainForest from the roots).  The incumbent queue drains completely
/// before any new graph edge is explored, so a surviving old subtree, even one detached upstream,
/// is re-attached through its own old edges (one new edge per detached fragment) and is not
/// re-wired vertex by vertex: a tree-edge pin moves only when its arc died (or, under slip-aware
/// election, slipped).  Pure: it writes nothing the pins read.
///
/// @param bands   The bands the component is elected over (the family's, or a band-only
///                component's single band).
/// @param n_bands How many.
/// @return The plan, or none when the component has no station to elect (unreachable).
std::optional<ChainComponentPlan> planChainComponent(const ChainElection& el, const ChainFamily& fam,
                                                     const ChainGraph& cg, const arcgraph::Component& comp,
                                                     const ChainBand* bands, std::size_t n_bands,
                                                     const std::vector<int>& incumbent_arcs,
                                                     const arcgraph::ForestRules& rules,
                                                     const std::vector<int>& chain_rank,
                                                     const std::vector<int>& station_rank) {
    ChainComponentPlan plan;
    // The component's stations (ascending roster index, as their slots are) and satellites
    // (string order).
    for (int s : comp.stations) plan.rx_list.push_back(cg.staIdx(s));
    for (int k : comp.sats) plan.sats.push_back(cg.satName(k));
    plan.r1 = electDatumReceiver(el, fam.sys, bands, n_bands, plan.rx_list, plan.sats);
    if (plan.r1 == kNoReceiver) return std::nullopt;   // empty (unreachable)
    for (int s : comp.stations) plan.vertices.push_back(arcgraph::stationVertex(cg.graph, s));
    for (int k : comp.sats) plan.vertices.push_back(arcgraph::satVertex(cg.graph, k));
    const int r1_slot = comp.stations[static_cast<size_t>(
        std::find(plan.rx_list.begin(), plan.rx_list.end(), plan.r1) - plan.rx_list.begin())];
    // eq34 reattach hygiene INC-2: the spanning structure is a forest rooted at {r1} and the
    // bridged satellites, and the rank arithmetic still closes: 1 RPB pin + B bridge rows
    // + (n_c + m_c - 1 - B) tree edges = n_c + m_c.  Bridge rows are counted as pins for that
    // cross-check.
    plan.roots.push_back(arcgraph::stationVertex(cg.graph, r1_slot));
    plan.bridged = bridgedSlots(el, fam, cg, comp);
    for (int k : plan.bridged) plan.roots.push_back(arcgraph::satVertex(cg.graph, k));
    plan.forest = arcgraph::growChainForest(cg.graph, comp, plan.roots, incumbent_arcs, rules,
                                            chain_rank, station_rank);
    return plan;
}

/// A family's components with their plans, and the datum receiver of its largest component.
struct ChainFamilyPlan {
    std::vector<ChainComponentPlan> comps;          ///< In component order (the order of their root satellites).
    std::string                     best_datum_rx;  ///< The datum receiver of the largest component (the first on a tie).
};

/// The rules of the walk's new edges.  A new tree edge is grown only onto an arc whose ambiguity
/// has converged (age >= eq34_anchor_min_arc_ep; INC-1).  The walk runs twice: the first pass
/// admits eligible edges only, so a mature host wins over a fresh one regardless of the
/// longevity rank order; if that leaves vertices unreachable (their only connection to the
/// component runs through a fresh arc), the second pass admits everything and counts each
/// unavoidable cold pin.  The incumbent queue is not filtered: keeping the incumbent is not a
/// reattach, and blocking it would cause the handover this rule is meant to prevent.
/// Slip-aware: a slipped arc is refused in the first pass on the same terms as a cold host, and
/// admitted in the second.  The refusal is a rule of its own, not only the arcLife ranking,
/// because arcLife is read only under the longevity election; relying on it would leave slip
/// avoidance inactive wherever longevity is off (its default).  Avoiding a slipped tree edge is a
/// correctness preference, not a longevity heuristic.  Without this refusal, de-seeding an
/// incumbent would be worse than no action: the walk would hand the edge straight back and pay a
/// datum handover for it.  Under longevity a receiver tries its longest-lived satellites first,
/// and a satellite its most durable tracker (highest per-constellation coverage, the robust
/// receiver of spec D2 step 3, then the arc's own longevity); off, the legacy satellite-name and
/// station-index orders.
arcgraph::ForestRules chainForestRules(const ChainElection& el, std::uint8_t band_mask) {
    arcgraph::ForestRules rules;
    rules.band_mask               = band_mask;
    rules.refuse_ineligible_first = el.hyg.active && el.hyg.min_arc_ep > 0;
    rules.refuse_slipped_first    = el.slip_aware;
    rules.by_durability           = el.longevity;
    return rules;
}

/// The family's components, each with its datum receiver and forest, in component order.  The
/// connected components come in the order of their roots: under the shared link rule (a station
/// is united under a satellite, arc by arc) every root is a satellite, so the components come in
/// the string order of their root satellites; legacyChainVertexRank also orders the second pass's
/// frontier (receivers by the decimal string of their roster index, then satellites).
ChainFamilyPlan planChainFamily(const ChainElection& el, const ChainFamily& fam, const ChainGraph& cg,
                                const ChainSeeds& seeds, const std::vector<int>& chain_rank,
                                const std::vector<int>& station_rank) {
    ChainFamilyPlan fp;
    const arcgraph::ForestRules rules = chainForestRules(el, fam.mask());
    std::size_t best_comp = 0;
    for (const arcgraph::Component& comp : arcgraph::arcComponents(cg.graph, fam.mask(), chain_rank)) {
        std::optional<ChainComponentPlan> plan =
            planChainComponent(el, fam, cg, comp, fam.bands.data(), fam.bands.size(), seeds.family,
                               rules, chain_rank, station_rank);
        if (!plan) continue;
        if (plan->rx_list.size() > best_comp) {
            best_comp = plan->rx_list.size();
            fp.best_datum_rx = el.stations[plan->r1].name;
        }
        fp.comps.push_back(std::move(*plan));
    }
    return fp;
}

/// One tree edge as the shift walk consumes it: the attached vertex, its parent and the pin's frame
/// step.  pinmove = this pin's exact displacement of its pinned float vs last epoch's frame;
/// known = false where the step is not computable (a re-pin without a carry; a root handover,
/// a common fractional step that no integer transform can express); shiftable = the step came
/// from the new host's own state and may be S-transformed even with the global carry-shift knob
/// off (always false unless the seed-round re-host frame is armed).
struct TreeStep {
    int    child{-1};      ///< The attached vertex.
    int    parent{-1};     ///< The vertex it hangs under.
    double pinmove{0.0};
    bool   known{true};
    bool   shiftable{false};
};

/// One band's walk over a family: the frame step of every vertex across the band's components
/// and its extension (components are disjoint, so one vector per band equals one per component),
/// and the observatory's bookkeeping.
struct BandWalk {
    explicit BandWalk(const ChainGraph& cg)
        : vshift(cg.nVertices(), 0.0), vknown(cg.nVertices(), 0), vdepth(cg.nVertices(), 0),
          owner(cg.nVertices(), -1) {}

    /// One tree of the band: a component's plan with the index the observatory gave it.
    struct Tree {
        const ChainComponentPlan* plan;
        int                       comp;  ///< The observatory's component index (per band).
        std::size_t               obs;   ///< Its record in chain_obs.comps.
    };

    std::vector<double> vshift;  ///< Per vertex: the frame step of its subtree's carries (m).
    std::vector<char>   vknown;  ///< Per vertex: that step is known.
    std::vector<int>    vdepth;  ///< Per vertex: its depth below its root (observatory only).
    std::vector<int>    owner;   ///< Per vertex: the index in @ref trees of the tree it belongs to (-1: none yet).
    std::vector<Tree>   trees;   ///< The band's trees, in election order.
    std::deque<ChainComponentPlan> band_only;  ///< Plans of components only this band spans (pair family); a deque so the trees' pointers stay valid.
    int                 next_comp{0};          ///< The observatory's next component index.
};

/// A pin's target and frame step, and the facts they were read from.
struct PinDraft {
    double target{0.0};
    double carry_val{std::numeric_limits<double>::quiet_NaN()};  ///< The new host's carried value (NaN: none read).
    bool   seed_deferred{false};
    bool   seed_rounded{false};
    /// The new host's pre-pin float (INC-2, SPEC 8.5).  Finite only when the state exists and INC-2
    /// is armed; on a re-host it is this pin's exact frame-step baseline (the pin moves the float
    /// from here to `target`, so the satellite's step is -(target - seed_x) exactly).
    double seed_x{std::numeric_limits<double>::quiet_NaN()};
    bool   frame_shiftable{false};
    double pinmove{0.0};
    bool   known{true};
    bool   edge_changed{false};
};

/// The pin target of the tree edge (ri, sat) on one band.  Target = 0 on a first-sighting edge
/// (unless seed-round applies, below); on a re-pin of an already-lit (sat, signal) (pin-to-round),
/// the |.|-minimizing integer lambda*round(carried float/lambda), so the emitted-bias frame step
/// is about 0 cycles.
///
/// Seed-round (docs/SPEC_eq34_seed_round.md) applies when the carry-round did not engage (first
/// sighting; incumbent whose carry is dead; or every epoch under carry_invariant, where pin-to-round
/// is off by the invariant-carry design).  The target would then be the cold absolute 0, which puts
/// the seeded phase-code offset into the datum frame (km-scale on the synthetic fixture; SPEC 12.8).
/// Instead, round the value the state holds, so |pin innovation| <= lambda/2 by construction.  When
/// the state does not exist yet (cold start or new arc; PASS B's pre-pass allocates it), defer the
/// rounding to the fold via pinned_amb_seed_lambda; the realized target flows back into
/// eq34_prev_target through the serial consume.  `known` is cleared for deferred pins.
PinDraft draftPinTarget(ChainElection& el, const ChainBand& band, std::size_t ri,
                        const std::string& sat, const std::string& amb) {
    PinDraft d;
    if (el.pin_round && net::getSig(el.sig_rows, amb, &net::SatSignalState::anchor_incumbent) != nullptr) {
        auto lc = net::getArc(el.entities.arcs, std::make_tuple(static_cast<int>(ri), sat, band.signal),
                              &net::ArcState::amb);
        if (lc != nullptr && band.roundable) {
            const double lam = band.lambda_m;
            if (std::isfinite(lam) && lam > 0.0) {
                d.carry_val = lc->value_abs;
                d.target = lam * std::round(d.carry_val / lam);
            }
        }
    }
    auto ambStateValue = [&]() -> double {
        const int sidx = findAmbiguity(el.stations[ri].params, amb);
        if (sidx < 0) return std::numeric_limits<double>::quiet_NaN();
        return slotAt(el.stations[ri].params, sidx).x_lin;
    };
    if (el.seed_round && !std::isfinite(d.carry_val) && band.roundable) {
        // Freeze after first realization (INC-1, SPEC 8.5): a frame is realized once.  Reuse this
        // (station, amb)'s previously realized target unchanged (on the same host: pinmove 0, no
        // step, no reref) and round only on a first realization.  Re-rounding every epoch ratchets
        // the invariant carry (13.7).
        const double* ptf = net::getArcAmb(el.arc_rows, static_cast<int>(ri), amb,
                                           &net::ArcState::eq34_prev_target);
        if (ptf != nullptr) {
            d.target = *ptf;
            d.seed_rounded = true;
            ++el.ctx.counters.seed_round_frozen;
            // A frozen target can still land on a re-hosted edge (the entry survived through the I2
            // retention or the fold feedback), and that step is equally computable, so read the
            // pre-pin float here too (INC-2).  Only when armed, so with INC-2 off the freeze branch
            // keeps its single map lookup.
            if (el.seed_rehost) d.seed_x = ambStateValue();
        } else {
            const double lam = band.lambda_m;
            if (std::isfinite(lam) && lam > 0.0) {
                const int sidx = findAmbiguity(el.stations[ri].params, amb);
                if (sidx >= 0) {
                    const double x = slotAt(el.stations[ri].params, sidx).x_lin;
                    if (std::isfinite(x)) {
                        if (el.seed_rehost) d.seed_x = x;   // re-host step baseline (INC-2)
                        d.target = lam * std::round(x / lam);
                        d.seed_rounded = true;
                        ++el.ctx.counters.seed_round_pins;
                        const double y = std::abs(d.target - x);
                        if (y > el.ctx.counters.seed_round_max_abs_y_m)
                            el.ctx.counters.seed_round_max_abs_y_m = y;
                    }
                } else {
                    el.stations[ri].pinned_amb_seed_lambda[amb] = lam;
                    d.seed_deferred = true;
                }
            }
        }
    }
    return d;
}

/// The frame step a pin puts on the tree, and whether its edge changed host.  The attach side
/// selects which endpoint the edge attaches: the satellite (parent edge to this_epoch_anchor) or
/// the receiver (parent edge to new_rx_parent).
void draftPinStep(ChainElection& el, const ChainBand& band, const ChainComponentPlan& plan,
                  std::size_t ri, const std::string& sat, const std::string& amb, bool sat_is_child,
                  PinDraft& d) {
    NetStation& host = el.stations[ri];
    if (sat_is_child) {
        const std::string* ai = net::getSig(el.sig_rows, amb, &net::SatSignalState::anchor_incumbent);
        d.edge_changed = (ai != nullptr && *ai != host.name);
        el.this_epoch_anchor[amb] = host.name;
        // Root handover: the component frame steps by the datum receivers' fractional delta
        // difference, which is not an integer and not computable here, so unknown.
        const std::string* rt = net::getSig(el.sig_rows, amb, &net::SatSignalState::eq34_sat_root);
        if (rt != nullptr && *rt != el.stations[plan.r1].name) d.known = false;
        el.new_sat_root[amb] = el.stations[plan.r1].name;
    } else {
        const std::string rxkey = host.name + band.rx_suffix;
        const std::string* rp =
            net::get(el.entities.stations, StationId{static_cast<int>(ri)},
                     &net::StationState::signals, band.rx_suffix.substr(1),
                     &net::StationSignalState::eq34_rx_parent);
        d.edge_changed = (rp != nullptr && *rp != sat);
        el.new_rx_parent[rxkey] = sat;
    }
    if (d.edge_changed) {
        // Re-pin at a new station: the float there sat at carry_val and the pin drives it to
        // target, an exact, known displacement iff the carry exists.
        if (std::isfinite(d.carry_val)) d.pinmove = d.target - d.carry_val;
        // No carry, but under seed-round (INC-2, SPEC 8.5) the station's own ambiguity state is
        // the pre-pin value, and it is exact: it is the estimator's own number, not a leveling
        // carry that can drift (the I1c mechanism that made carry_shift harmful in the field does
        // not apply).  The frame step is therefore computable, and the edge stays shiftable even
        // with the global carry-shift knob off.  That keeps the (sat,sigma) out of eq34_reref, so
        // its carried PB prior is folded (S-transformed) instead of marginalized for the epoch.
        else if (el.seed_rehost && std::isfinite(d.seed_x)) {
            d.pinmove = d.target - d.seed_x;
            d.frame_shiftable = true;
            ++el.ctx.counters.seed_round_rehost_framed;
            const double st_abs = std::abs(d.pinmove);
            if (st_abs > el.ctx.counters.seed_round_rehost_max_step_m)
                el.ctx.counters.seed_round_rehost_max_step_m = st_abs;
        } else {
            d.known = false;
            // Counted only when armed, so that with INC-2 off the run-end block stays
            // byte-identical.
            if (el.seed_rehost) ++el.ctx.counters.seed_round_rehost_cold;
        }
    } else {
        // Same station: its float was hard-pinned at last epoch's target, so the displacement is
        // exactly the target difference (no carry noise involved).
        const double* pt = net::getArcAmb(el.arc_rows, static_cast<int>(ri), amb,
                                          &net::ArcState::eq34_prev_target);
        d.pinmove = d.target - (pt != nullptr ? *pt : 0.0);
    }
    // Seed-round bookkeeping: a deferred pin's target is decided at the fold, so no frame step is
    // computable here (the serial consume feeds the realized target back as next epoch's
    // baseline).  An election-side seed target that moved since last epoch (the float crossed a
    // half-integer, or a dropped station lost last epoch's deferred feedback) is a frame
    // re-realization, not a transformable shift: it goes through the validated reref hygiene,
    // not the v2-shift walk.  INC-2 exempts the framed re-host: its step is an exactly computed
    // frame move, not a same-station target change, so it takes the shift walk (and its own
    // counter) instead of the reref hygiene.
    if (d.seed_deferred) d.known = false;
    else if (d.seed_rounded && d.pinmove != 0.0 && !d.frame_shiftable) {
        d.known = false;
        ++el.ctx.counters.seed_round_target_steps;
    }
}

/// Record the pin: the target the station folds, the next epoch's baseline and the pin count.
/// eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md): this pin's host arc is (ri, amb)
/// whichever endpoint the edge attaches.  Count the reattach (the satellite-side host moved;
/// NetworkSolPrep traces this event as PHASE_DATUM/eq34_reattach) and, when the eligible pass
/// could not avoid a fresh host, give the pin fold an INC-3 sigma multiplier so the datum row is
/// soft enough for the data to pull the cold ambiguity instead of re-levelling the component onto
/// its code-seeded value.  Inert (no map entry, no counter) when the reattach hygiene is off.
void notePin(ChainElection& el, std::size_t ri, const std::string& amb, bool sat_is_child,
             const PinDraft& d) {
    el.new_prev_target[{static_cast<int>(ri), amb}] = d.target;
    el.stations[ri].pinned_amb_targets[amb] = d.target;
    ++el.n_pins;
    if (el.hyg.active) {
        if (d.edge_changed && sat_is_child) ++el.ctx.counters.hyg.reattaches;
        const double sc = el.hyg.pinSigmaScale(ri, amb);
        if (sc > 1.0) {
            el.stations[ri].pinned_amb_sigma_scale[amb] = sc;
            ++el.ctx.counters.hyg.fresh_pin_ramps;
        }
    }
}

/// The frame observatory's record of one tree edge: the facts the as-built rule used and what it
/// did with them.  Only appends to the epoch's ChainEpochObs; nothing in the election reads it.
void traceTreeEdge(ChainElection& el, const ChainFamily& fam, const ChainBand& band,
                   const ChainComponentPlan& plan, int comp_index, std::size_t ri,
                   const std::string& sat, const std::string& amb, bool sat_is_child,
                   const arcgraph::TreeEdge& edge, const PinDraft& d) {
    ChainEdgeObs o;
    o.group = band.dr_key;
    o.sys = constellationKey(fam.sys);
    o.signal = band.signal;
    o.family = fam.kind();
    o.comp = comp_index;
    o.r1 = el.stations[plan.r1].name;
    o.station = static_cast<int>(ri);
    o.station_name = el.stations[ri].name;
    o.sat = sat;
    o.sat_is_child = sat_is_child;
    const std::string rkey = "R:" + std::to_string(ri), skey = "S:" + sat;
    o.child  = sat_is_child ? skey : rkey;
    o.parent = sat_is_child ? rkey : skey;
    o.lambda_m = band.lambda_m;
    o.target_m = d.target;
    if (const double* pt = net::getArcAmb(el.arc_rows, static_cast<int>(ri), amb,
                                          &net::ArcState::eq34_prev_target))
        o.prev_target_m = *pt;
    o.carry_m = d.carry_val;
    o.pinmove_m = d.pinmove;
    o.known = d.known;
    o.edge_changed = d.edge_changed;
    // The incumbent tree (last epoch's, replaced only after the election): this arc as the
    // satellite's parent edge or as the station's.
    const std::string* inc_sat = net::getSig(el.sig_rows, amb, &net::SatSignalState::anchor_incumbent);
    const std::string* inc_rx =
        net::get(el.entities.stations, StationId{static_cast<int>(ri)}, &net::StationState::signals,
                 band.rx_suffix.substr(1), &net::StationSignalState::eq34_rx_parent);
    const bool was_sat_edge = inc_sat != nullptr && *inc_sat == el.stations[ri].name;
    const bool was_rx_edge  = inc_rx != nullptr && *inc_rx == sat;
    if (sat_is_child) {
        o.had_parent = inc_sat != nullptr;
        o.incumbent_same = was_sat_edge;
        o.incumbent_flipped = !was_sat_edge && was_rx_edge;
        if (inc_sat != nullptr) {
            const auto ix = el.chain_index_of.find(*inc_sat);
            o.prev_parent = ix != el.chain_index_of.end()
                                ? "R:" + std::to_string(ix->second) : std::string();
        }
        const std::string* rt = net::getSig(el.sig_rows, amb, &net::SatSignalState::eq34_sat_root);
        o.root_unknown = rt != nullptr && *rt != el.stations[plan.r1].name;
    } else {
        o.had_parent = inc_rx != nullptr;
        o.incumbent_same = was_rx_edge;
        o.incumbent_flipped = !was_rx_edge && was_sat_edge;
        if (inc_rx != nullptr) o.prev_parent = "S:" + *inc_rx;
    }
    if (o.root_unknown)          o.cause = ChainLegacyCause::RootHandover;
    else if (d.seed_deferred)    o.cause = ChainLegacyCause::SeedDeferred;
    else if (d.seed_rounded && d.pinmove != 0.0 && !d.frame_shiftable)
                                 o.cause = ChainLegacyCause::SeedStep;
    else if (d.edge_changed)
        o.cause = std::isfinite(d.carry_val) ? ChainLegacyCause::RehostCarry
                : d.frame_shiftable          ? ChainLegacyCause::RehostFramed
                                             : ChainLegacyCause::RehostCold;
    else o.cause = std::isfinite(o.prev_target_m) ? ChainLegacyCause::SameEdge
                                                  : ChainLegacyCause::FirstSighting;
    o.from_incumbent = edge.from_incumbent;
    o.second_pass = edge.pass == 2;
    const auto sc = el.stations[ri].pinned_amb_sigma_scale.find(amb);
    if (sc != el.stations[ri].pinned_amb_sigma_scale.end() && sc->second > 1.0) {
        o.sigma_scale = sc->second;
        ++el.chain_obs.soft_pins;
    }
    ArcLifeKeys lk;
    lk.arc = std::make_tuple(static_cast<int>(ri), sat, band.signal);
    lk.amb = {static_cast<int>(ri), amb};
    const ArcLifeStamp ls = arcLifeStamp(el.ctx, lk);
    o.life_stamp = fmt::format("g{}.r{}.c{}.b{}.d{}.l{:.0f}.f{}", ls.adm_generation,
                               ls.adm_run_start, ls.has_carry ? 1 : 0, ls.birth_ep,
                               ls.dropped_ep, ls.relevel_s, ls.flag_ep);
    el.chain_obs.edges.push_back(std::move(o));
}

/// Record one tree edge (r, s): pin ambiguity z^s_r at station r, on one band.  Target = 0 on a
/// first-sighting edge (unless seed-round applies); on a re-pin of an already-lit (sat, signal),
/// the |.|-minimizing integer (see draftPinTarget), so the emitted-bias frame step is about 0
/// cycles.  The edge's attach side selects which endpoint it attaches.
///
/// @param plan       The tree the edge hangs in (its datum receiver is the satellite's root).
/// @param comp_index The observatory's component index of that tree on this band.
/// @return The edge as the shift walk consumes it.
TreeStep recordTreeEdge(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg,
                        const ChainBand& band, const ChainComponentPlan& plan, int comp_index,
                        const arcgraph::TreeEdge& edge) {
    const int         sta_slot     = cg.graph.row_station[static_cast<size_t>(edge.arc)];
    const int         sat_slot     = cg.graph.row_sat[static_cast<size_t>(edge.arc)];
    const size_t      ri           = cg.staIdx(sta_slot);
    const std::string sat          = cg.satName(sat_slot);
    const bool        sat_is_child = edge.attaches_sat;
    const std::string amb = StateKeys::makeAmbiguityKey(sat, band.signal);
    // Slip-aware: this arc is becoming a tree edge.  If it slipped this epoch, the election could
    // not avoid it (the component has no other route to the vertex), and every cycle through it
    // refloats.  Counted separately so that "deseeded > 0" is not read as "no slipped arc ended up
    // in the tree".
    if (chainArcSlipped(el, ri, sat, band.signal)) ++el.ctx.counters.eq34_slip_readmitted;
    // Gap re-pin metric (I2): this edge was retained across a gap (its carry outlived the row
    // absence) and is resuming now.  The incumbent seed made it a recognized edge, so the
    // pin-to-round re-pins it to its pre-gap integer (step ~0) instead of the first-sighting 0 a
    // wholesale replace would have used.  Counts these re-pins.
    if (el.extend_only
        && net::hasArcAmb(el.arc_rows, static_cast<int>(ri), amb, &net::ArcState::eq34_retained_edge))
        ++el.ctx.counters.eq34_gap_repin_total;
    PinDraft d = draftPinTarget(el, band, ri, sat, amb);
    draftPinStep(el, band, plan, ri, sat, amb, sat_is_child, d);
    notePin(el, ri, amb, sat_is_child, d);
    if (el.trace_chain)
        traceTreeEdge(el, fam, band, plan, comp_index, ri, sat, amb, sat_is_child, edge, d);
    const int rv = arcgraph::stationVertex(cg.graph, sta_slot);
    const int sv = arcgraph::satVertex(cg.graph, sat_slot);
    return TreeStep{sat_is_child ? sv : rv, sat_is_child ? rv : sv, d.pinmove, d.known, d.frame_shiftable};
}

/// Shift walk: the carry S-transformation (spec D4 "v2 = SHIFT"), realized at election, where the
/// step is known, so no emit-site plumbing is needed.  A pin displacement propagates down the
/// chain with alternating sign:
///   db_s = -d(delta_parent) - pinmove(edge)
///   d(delta_r) = -db_parent - pinmove(edge)
/// so the whole subtree's frame move is exact and every carried quantity can be transformed into
/// the new basis (variances untouched, no information lost) instead of erased.  Erase/skip
/// (eq34_reref) remains only where the step is not transformed: a re-pin with no carry, a root
/// handover (common fractional), and, with the shift off, any nonzero step.  `order` is in
/// attachment order, parents strictly first, so one forward pass works; the parents' steps are
/// the band walk's (a root's is a known zero, seeded by the caller).
void walkFrameSteps(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg,
                    const ChainBand& band, const std::vector<TreeStep>& order, BandWalk& walk) {
    for (const TreeStep& st : order) {
        const auto v = static_cast<size_t>(st.child), p = static_cast<size_t>(st.parent);
        // Shift off (default): any nonzero pin displacement is treated as unknown, so the
        // erase/skip hygiene handles it (the validated B4/B7-class behavior).  Shift on: exact
        // transforms flow to the apply block of the last step.  INC-2 adds a third case for one
        // edge class only, a seed-round re-host whose step came from the new host's own state, and
        // it composes with the walk unchanged: the nonzero step it seeds propagates through the
        // (pinmove == 0) frozen edges below it as it would in a shift-on run.
        const bool   k = st.known && walk.vknown[p]
                      && (el.shift_on || st.pinmove == 0.0 || st.shiftable);
        const double s = k ? (-walk.vshift[p] - st.pinmove) : 0.0;
        walk.vshift[v] = s;
        walk.vknown[v] = k ? 1 : 0;
        if (arcgraph::isSatVertex(cg.graph, st.child)) {
            const std::string sat =
                cg.satName(st.child - static_cast<int>(cg.graph.stations.size()));
            if (!k)          el.eq34_reref.insert({sat, band.signal});
            else if (s != 0.0) el.shift_sat[{sat, band.signal}] = s;
        } else {
            const int idx = static_cast<int>(cg.staIdx(st.child));
            if (!k) {
                // Unknown frame step: the carried delta_{r,sigma} belongs to an unrecoverable
                // retired frame.  Counted only when an entry was erased.
                if (net::drop(el.sta_rows, StationId{idx}, &net::StationState::signals, band.rpb_key,
                              &net::StationSignalState::rpb) != 0)
                    prmRespond(el.ctx.counters.prm_resp, ResponseSite::RpbEq34Erase);
            } else if (s != 0.0) {
                el.shift_rxg[std::make_tuple(idx, rinexLetter(fam.sys), band.signal)] = s;
                el.shift_rpb[{idx, band.rpb_key}] = s;
            }
        }
    }
}


/// The observatory's depth and frame step of each recorded edge: the edges were appended from
/// index edge0 in the order of `order`, parents strictly first, so one forward pass works.
void observeDepths(ChainElection& el, const std::vector<TreeStep>& order, std::size_t edge0,
                   BandWalk& walk) {
    for (std::size_t k = 0; k < order.size(); ++k) {
        const auto v = static_cast<size_t>(order[k].child), p = static_cast<size_t>(order[k].parent);
        walk.vdepth[v] = walk.vdepth[p] + 1;
        ChainEdgeObs& o = el.chain_obs.edges[edge0 + k];
        o.depth = walk.vdepth[v];
        o.step_m = walk.vknown[v] ? walk.vshift[v] : std::numeric_limits<double>::quiet_NaN();
    }
}

/// The observatory's record of one tree: each edge's depth and frame step, then every chord's
/// fundamental cycle with the targets on its tree path.  The chords of the tree's own vertices
/// only: a vertex a band extension attaches afterwards has no chord record.
void observeTree(ChainElection& el, const ChainGraph& cg, int b, const ChainBand& band,
                 const arcgraph::SpanningForest& forest, const std::vector<TreeStep>& order,
                 std::size_t edge0, std::size_t obs, BandWalk& walk) {
    ChainEpochObs& chain_obs = el.chain_obs;
    observeDepths(el, order, edge0, walk);
    std::map<std::pair<int, std::string>, double> target_of;   // (station, sat) -> target
    for (std::size_t k = 0; k < order.size(); ++k) {
        const ChainEdgeObs& o = chain_obs.edges[edge0 + k];
        target_of[{o.station, o.sat}] = o.target_m;
    }
    chain_obs.comps[obs].n_edges = static_cast<int>(order.size());
    const arcgraph::CycleBasis basis = arcgraph::fundamentalBasis(cg.graph, forest, b);
    for (const arcgraph::BasisCycle& bc : basis.cycles) {
        ChainChordCycle cc;
        cc.station = static_cast<int>(cg.staIdx(cg.graph.row_station[static_cast<size_t>(bc.pivot_arc)]));
        cc.sat = cg.satName(cg.graph.row_sat[static_cast<size_t>(bc.pivot_arc)]);
        cc.signal = band.signal;
        cc.cycle = bc.cycle;
        for (const arcgraph::CycleTerm& term : bc.cycle.terms) {
            const int ts = term.arc.station.index;
            const std::string tsat = term.arc.sat.str();
            if (ts == cc.station && tsat == cc.sat) continue;   // the chord itself
            const auto it = target_of.find({ts, tsat});
            if (it == target_of.end() || !(band.lambda_m > 0.0)) {
                cc.pins_integer = false;
                continue;
            }
            const double kc = it->second / band.lambda_m;
            if (std::abs(kc - std::round(kc)) > 1e-6) cc.pins_integer = false;
            cc.pin_sum_cyc += term.coeff * kc;
        }
        chain_obs.chords.push_back(std::move(cc));
    }
}

/// Record one component's tree on one band: the datum receiver's receiver phase-bias pin and the
/// declared gauge dimension, the observatory's component record, the pin of every tree edge in
/// attachment order and the shift walk down them.  recordTreeEdge is replayed over the forest's
/// edges in attachment order once the walk is done, which is exact because the walk reads nothing
/// recordTreeEdge writes; each edge records (child, parent, edge-changed) for the shift walk.
///
/// Per (constellation, signal) connected component with n_c stations and m_c satellites:
/// (n_c + m_c - 1) tree-edge ambiguity pins + 1 RPB pin = n_c + m_c, the gauge dimension.  Every
/// RPB-carrying station is a tree vertex, so it gets at least one pin and its N_rr stays PD (the
/// rank invariant D3c).
///
/// @param b    The family band index.
/// @param plan The component's plan (its datum receiver, its forest and its bridged roots).
/// @param walk The band's walk; the tree is entered into it.
void electBandComponent(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg, int b,
                        const ChainComponentPlan& plan, BandWalk& walk) {
    const ChainBand& band = fam.bands[static_cast<size_t>(b)];
    const int n_members = static_cast<int>(plan.rx_list.size() + plan.sats.size());   // n_c + m_c
    // r1's RPB pin: the receiver half of the gauge (delta_{r1,sigma}=0).
    el.stations[plan.r1].pinned_rpb_keys.insert(band.rpb_key);
    ++el.n_pins;
    el.gauge_dim += n_members;
    el.events.phase_datum.eq34_epoch_declared[band.dr_key] += n_members;
    // The observatory's record of this component; its edges follow from index edge0
    // (recordTreeEdge appends them in attachment order, as `order`).
    const int comp_index = walk.next_comp++;
    const std::size_t edge0 = el.chain_obs.edges.size();
    const std::size_t obs = el.chain_obs.comps.size();
    if (el.trace_chain) {
        ChainComponentObs k;
        k.group = band.dr_key;
        k.sys = constellationKey(fam.sys);
        k.signal = band.signal;
        k.comp = comp_index;
        k.r1 = el.stations[plan.r1].name;
        k.r1_station = static_cast<int>(plan.r1);
        for (size_t i : plan.rx_list) k.stations.push_back(static_cast<int>(i));
        k.sats = plan.sats;
        k.declared = n_members;
        el.chain_obs.comps.push_back(std::move(k));
    }
    // A bridged satellite's gauge is realized on the shared side, by a soft pseudo-obs row holding
    // b_s at its last solved level (networkInjectPinBridgePrior).  Bridge rows are counted as
    // pins for the rank cross-check.
    for (int k : plan.bridged) {
        ++el.n_pins;
        // Audit: a bridge pin is realized on the shared side, so the station-side fold
        // expectation is declared - bridged.
        ++el.events.phase_datum.eq34_epoch_bridged[band.dr_key];
        if (el.trace_chain) {
            el.chain_obs.comps[obs].bridged.push_back(cg.satName(k));
            ++el.chain_obs.bridges;
        }
    }
    walk.trees.push_back(BandWalk::Tree{&plan, comp_index, obs});
    for (int v : plan.vertices) walk.owner[static_cast<size_t>(v)] = static_cast<int>(walk.trees.size()) - 1;
    std::vector<TreeStep> order;
    for (const arcgraph::TreeEdge& edge : plan.forest.edges) {
        // A new edge on a non-eligible host is an unavoidable cold pin (the second pass admitted it).
        if (edge.ineligible_host) ++el.ctx.counters.hyg.cold_fallback_pins;
        order.push_back(recordTreeEdge(el, fam, cg, band, plan, comp_index, edge));
    }
    // r1 and every bridged satellite are roots of the walk.  A bridged satellite's frame does not
    // move either (the bridge row holds b_s at its previous value), so it seeds the walk with a
    // known zero step.  The subtree below it therefore keeps its carries instead of being
    // re-referenced (no eq34_reref entry, no local-carry erase); this is the purpose of the bridge.
    for (int r : plan.roots) {
        walk.vshift[static_cast<size_t>(r)] = 0.0;
        walk.vknown[static_cast<size_t>(r)] = 1;
    }
    walkFrameSteps(el, fam, cg, band, order, walk);
    if (el.trace_chain) observeTree(el, cg, b, band, plan.forest, order, edge0, obs, walk);
}

/// Attach the vertices that only band b tracks to the pair trees of the component K they connect
/// to (K: a component of the band's own graph that holds pair-tree vertices).  The pair trees'
/// vertices are the roots, band b's own incumbent arcs the seed; every edge the growth records
/// attaches one band-only vertex through an arc only band b carries, and is pinned on band b
/// alone, under the pair tree that vertex hangs in.  The shift walk continues over them from
/// their parents' steps.
void extendPairTrees(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg, int b,
                     const arcgraph::Component& comp, const std::vector<int>& in_pair,
                     const std::vector<int>& band_only, const std::vector<int>& band_seeds,
                     const arcgraph::ForestRules& rules, const std::vector<int>& chain_rank,
                     const std::vector<int>& station_rank, BandWalk& walk) {
    const ChainBand& band = fam.bands[static_cast<size_t>(b)];
    const arcgraph::SpanningForest forest =
        arcgraph::growChainForest(cg.graph, comp, in_pair, band_seeds, rules, chain_rank, station_rank);
    // The band-only vertices join the gauge: n_c + m_c grows by one per vertex, and each takes a
    // tree-edge pin (counted as the edges are recorded).
    el.gauge_dim += static_cast<int>(band_only.size());
    el.events.phase_datum.eq34_epoch_declared[band.dr_key] += static_cast<int>(band_only.size());
    const std::size_t edge0 = el.chain_obs.edges.size();
    std::vector<TreeStep> order;
    for (const arcgraph::TreeEdge& edge : forest.edges) {
        if (edge.ineligible_host) ++el.ctx.counters.hyg.cold_fallback_pins;
        const auto arc = static_cast<size_t>(edge.arc);
        const int sv = arcgraph::stationVertex(cg.graph, cg.graph.row_station[arc]);
        const int kv = arcgraph::satVertex(cg.graph, cg.graph.row_sat[arc]);
        const int child  = edge.attaches_sat ? kv : sv;
        const int parent = edge.attaches_sat ? sv : kv;
        const int tree = walk.owner[static_cast<size_t>(parent)];   // the parent was attached before its child
        walk.owner[static_cast<size_t>(child)] = tree;
        const BandWalk::Tree& t = walk.trees[static_cast<size_t>(tree)];
        order.push_back(recordTreeEdge(el, fam, cg, band, *t.plan, t.comp, edge));
        ++el.ctx.counters.eq34_pair_ext_edges;
    }
    walkFrameSteps(el, fam, cg, band, order, walk);
    if (!el.trace_chain) return;
    observeDepths(el, order, edge0, walk);
    // Each attached vertex joins the observatory's record of the tree it hangs in.
    for (const TreeStep& st : order) {
        ChainComponentObs& k = el.chain_obs.comps[walk.trees[static_cast<size_t>(
            walk.owner[static_cast<size_t>(st.child)])].obs];
        if (arcgraph::isSatVertex(cg.graph, st.child))
            k.sats.push_back(cg.satName(st.child - static_cast<int>(cg.graph.stations.size())));
        else
            k.stations.push_back(static_cast<int>(cg.staIdx(st.child)));
        ++k.declared;
        ++k.n_edges;
    }
}

/// The band extension of a pair family (band b): the vertices that band tracks and no pair tree
/// attached.  A band-only vertex is one whose arcs on the band are all arcs the other band does
/// not carry.  Each component K of the band's own graph that holds some is either attached to the
/// pair trees it touches (extendPairTrees) or, when it holds no pair-tree vertex at all, elected
/// on its own as a one-band component (its datum receiver among its stations, its receiver
/// phase-bias pin on this band, its tree).
///
/// @param band_seeds Band b's incumbent arcs (the arcs of that band's own maps).
void extendBand(ChainElection& el, const ChainFamily& fam, const ChainGraph& cg, int b,
                const std::vector<int>& band_seeds, const std::vector<int>& chain_rank,
                const std::vector<int>& station_rank, BandWalk& walk) {
    const ChainBand& band = fam.bands[static_cast<size_t>(b)];
    const auto bit = static_cast<std::uint8_t>(1u << b);
    const arcgraph::ForestRules rules = chainForestRules(el, bit);
    for (const arcgraph::Component& comp : arcgraph::arcComponents(cg.graph, bit, chain_rank)) {
        std::vector<int> in_pair, band_only;   // the component's vertices, in ascending vertex id
        for (int s : comp.stations) {
            const int v = arcgraph::stationVertex(cg.graph, s);
            (walk.owner[static_cast<size_t>(v)] >= 0 ? in_pair : band_only).push_back(v);
        }
        for (int k : comp.sats) {
            const int v = arcgraph::satVertex(cg.graph, k);
            (walk.owner[static_cast<size_t>(v)] >= 0 ? in_pair : band_only).push_back(v);
        }
        if (band_only.empty()) continue;
        if (!in_pair.empty()) {
            extendPairTrees(el, fam, cg, b, comp, in_pair, band_only, band_seeds, rules, chain_rank,
                            station_rank, walk);
            continue;
        }
        std::optional<ChainComponentPlan> plan =
            planChainComponent(el, fam, cg, comp, &band, 1, band_seeds, rules, chain_rank, station_rank);
        if (!plan) continue;
        walk.band_only.push_back(std::move(*plan));
        ++el.ctx.counters.eq34_pair_band_only_comps;
        electBandComponent(el, fam, cg, b, walk.band_only.back(), walk);
    }
}

/// Elect one family: its graph, its incumbent seed, its components with their datum receivers
/// and forests, then per band the pins and the shift walk of every component (and, in a pair
/// family, the band's extension), and the band's remembered datum receiver.  The band loop is
/// outside the component loop, so a family whose bands' graphs coincide records its rows and
/// counts band by band, each in the order a one-band family has.
void electFamily(ChainElection& el, const ChainGroups& groups, const ChainFamily& fam) {
    const ChainGraph cg = buildChainGraph(el, groups, fam);
    const ChainSeeds seeds = seedIncumbents(el, fam, cg);
    const std::vector<int> chain_rank = arcgraph::legacyChainVertexRank(cg.graph);
    std::vector<int> station_rank;   // per station slot: the coverage
    for (int s = 0; s < static_cast<int>(cg.graph.stations.size()); ++s)
        station_rank.push_back(chainCoverage(el, fam.sys, cg.staIdx(s)));
    const ChainFamilyPlan plan = planChainFamily(el, fam, cg, seeds, chain_rank, station_rank);
    for (std::size_t b = 0; b < fam.bands.size(); ++b) {
        BandWalk walk(cg);
        for (const ChainComponentPlan& cp : plan.comps)
            electBandComponent(el, fam, cg, static_cast<int>(b), cp, walk);
        if (fam.pair())
            extendBand(el, fam, cg, static_cast<int>(b), seeds.band[b], chain_rank, station_rank, walk);
        if (!plan.best_datum_rx.empty())
            net::put(el.entities.constellations, fam.sys, &net::ConstellationState::channels,
                     fam.bands[b].signal, &net::ChannelState::datum_rx) = plan.best_datum_rx;
    }
}

/// Rank cross-check (D3): recorded pins == sum (n_c + m_c).  A mismatch means a component was
/// mis-pinned before the W5 gate ever runs.
void checkChainGauge(const ChainElection& el) {
    const std::shared_ptr<spdlog::logger>& log = el.log;
    auto& eq34_mismatch_epochs = el.ctx.counters.eq34_mismatch_epochs;
    const int eq34_n_pins = el.n_pins, eq34_gauge_dim = el.gauge_dim;
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
}

/// Carry hygiene (D4): erase the phase-leveling local carries for any re-referenced (sat,sigma) at
/// every station, before PASS B consumes the carry.  The b_s frame stepped by an amount the shift
/// walk did not transform, so the carried floats are stale.  They refloat next epoch (safe: the
/// realized wire step is usually 0 under pin-to-round, and erasing an already-integer carry costs
/// one epoch of continuity).
void eraseReferencedCarries(ChainElection& el) {
    NetworkRunContext& ctx = el.ctx;
    net::NetworkEntities& entities = el.entities;
    const std::shared_ptr<spdlog::logger>& log = el.log;
    auto& eq34_reref = el.eq34_reref;
    auto& eq34_reref_total = ctx.counters.eq34_reref_total;
    auto& eq34_erase_total = ctx.counters.eq34_erase_total;
    if (!eq34_reref.empty()) {
        eq34_reref_total += static_cast<long long>(eq34_reref.size());
        // The first post-resume epoch's share of that total, the quantity the
        // datum-continuity check is graded on (RS-3, SPEC_event_harness §21.7).
        // Measure-only; the latch is cleared at the emit site, so a withheld first
        // epoch folds into the window.
        if (ctx.carry.resume_datum_first_ep)
            ctx.counters.n_resume_datum_first_reref +=
                static_cast<long long>(eq34_reref.size());
        int n_erased = 0;
        // The re-referenced (sat, signal) keys' leveling carries, at every station.
        net::eraseIf(entities.arcs, &net::ArcState::amb, [&](const ArcId& id, const LocalCarry&) {
            if (eq34_reref.count({net::satKey(id.sat), net::signalKey(id.signal)})) {
                ++n_erased;
                ++ctx.counters.n_cr_eq34_reref;   // carry-retirement census: frame step
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbEq34Erase);
                return true;
            }
            return false;
        });
        eq34_erase_total += n_erased;
        if (n_erased > 0)
            log->debug("Network[eq34]: {} local ambiguity carry(ies) erased for "
                       "{} re-referenced (sat,σ) — refloat next epoch.",
                       n_erased, eq34_reref.size());
    }
}

/// Carry S-transformation apply (the carries move with the pins).  The text of the rule is the
/// comment at its start; the traversal itself is eq34ShiftCarry.
void applyCarryShift(ChainElection& el) {
    NetworkRunContext& ctx = el.ctx;
    net::NetworkEntities& entities = el.entities;
    const auto& shift_sat = el.shift_sat;
    const auto& shift_rxg = el.shift_rxg;
    const auto& shift_rpb = el.shift_rpb;
    auto& eq34_cpl_sprev_total = ctx.counters.eq34_cpl_sprev_total;
    auto& eq34_shift_total = ctx.counters.eq34_shift_total;
    // Carry S-transformation apply: shift every carried quantity into
    // this epoch's frame (Δz = −Δb_s − Δδ_r on the local ambiguity
    // carries; Δδ_r on the RPB carries; Δb_s on the shared PB carried
    // priors).  Variances untouched: this is an exact basis change,
    // not a refloat.  Runs before PASS B consumes any carry.
    //
    // Coupling references move with the carries (docs/SPEC_card1_coupling_carry.md
    // §9): D4 shifts `lc.value_abs` below and also every coupling `s_prev` on a
    // shifted PB column (SPEC_card1_coupling_carry §14).  A coupling row's RHS is
    // y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j , and `CoupledShared::s_prev` and
    // `CarryPrior::value` hold the same number for the same identity (both are
    // `x_s(j)` at harvest: the LocalCarryHarvest.cpp s_prev assignment and the
    // prev_carry stash).  For a gauge-invariant row the two moves cancel under a
    // known frame step; shifting one reference without the other injects the frame
    // step as row bias.  The same rule holds at Relevel.cpp's eph
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
    // The walk is the traversal of carried references: the ambiguities,
    // the receiver phase biases and the satellite phase biases, then the s_prev of the
    // phase-bias coupling terms in the ambiguity, receiver phase-bias and wet-delay carries.
    if (!shift_sat.empty() || !shift_rxg.empty()) {
        CarrySiteCounts prm_cc;
        long long n_shifted = 0, n_cpl = 0;
        eq34ShiftCarry(ctx.carry, entities, shift_sat, shift_rxg, shift_rpb, n_shifted, n_cpl, &prm_cc,
                       ctx.knobs.rebase_complete);
        if (!shift_sat.empty()) eq34_cpl_sprev_total += n_cpl;
        eq34_shift_total += n_shifted;
        // The held emissions of the shifted families, left in the old
        // frame (PbHold is exempt from the shift).  Measure-only.
        net::forEach(ctx.entities.sat_signals, &net::SatSignalState::pb_hold,
                     [&](const SatSignalId& id, const net::PbHoldKey&) {
            const auto sh = shift_sat.find({net::satKey(id.sat), net::signalKey(id.signal)});
            if (sh != shift_sat.end() && sh->second != 0.0)
                ++ctx.counters.prm_pbhold_unmoved_at_shift;
        });
        ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::Eq34Shift)];
        carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::Eq34Shift, prm_cc);
    }
}

/// I2 (de Jonge §3.5 extend-only): before the wholesale swap, merge
/// back the identity of any tree edge that was not seen this epoch but
/// whose phase-leveling memory (local_carry) is still alive: its
/// sat-side parent (this_epoch_anchor), receiver-side parent
/// (new_rx_parent), component root (new_sat_root) and pinned integer
/// (new_prev_target).  A momentary row gap then does not count as a
/// dead arc.  The edge sits inert in the incumbent maps (never seeded,
/// because the seed admits live arcs only, so the spanning-forest
/// invariant holds), and in the epoch its arc resumes it is a
/// recognized incumbent whose pin-to-round re-pins to the same integer
/// (emitted-bias step ≈ 0) instead of re-leveling from a first-sighting
/// 0.  local_carry liveness serves as the age stamp: it is gap-pruned at
/// phase_leveling_max_gap_s, and a genuine reroute or handover erases
/// it, so an edge past that horizon (or re-referenced) is dropped and
/// refloats as a first sighting, by which point the peer carries are
/// gone too.  Keys are receiver-indexed pins {receiver idx,
/// "sat_signal"}; the two parent maps hold each edge in one direction.
void retainGapEdges(ChainElection& el) {
    NetworkRunContext& ctx = el.ctx;
    net::NetworkEntities& entities = el.entities;
    const net::SatSignalTable& sig_rows = el.sig_rows;
    const net::ArcTable& arc_rows = el.arc_rows;
    std::vector<NetStation>& stations = el.stations;
    auto& this_epoch_anchor = el.this_epoch_anchor;
    auto& new_rx_parent = el.new_rx_parent;
    auto& new_sat_root = el.new_sat_root;
    auto& new_prev_target = el.new_prev_target;
    auto& new_retained_edges = el.new_retained_edges;
    const bool eq34_extend_only = el.extend_only;
    if (eq34_extend_only) {
        std::map<std::string, size_t> gname2idx;
        for (size_t i = 0; i < stations.size(); ++i)
            gname2idx[stations[i].name] = i;
        auto carryAlive = [&](size_t idx, const std::string& sat,
                              const std::string& sig) -> bool {
            return (net::getArc(ctx.entities.arcs, std::make_tuple(
                static_cast<int>(idx), sat, sig), &net::ArcState::amb) != nullptr) != 0;
        };
        // Sat-side parents (amb → parent station) + their component roots.
        for (const auto& [sid, srow] : sig_rows) {
            if (!srow.anchor_incumbent) continue;
            const std::string amb = net::ambKey(sid);
            const std::string& sta = *srow.anchor_incumbent;
            if (this_epoch_anchor.count(amb)) continue;    // re-seen live
            if (sid.sat == SatId{} || sid.signal == SignalId{}) continue;   // a row that names no arc
            auto gi = gname2idx.find(sta);
            if (gi == gname2idx.end()
                || !carryAlive(gi->second, net::satKey(sid.sat), net::signalKey(sid.signal)))
                continue;  // dead ⇒ drop
            this_epoch_anchor.emplace(amb, sta);
            new_retained_edges.emplace(static_cast<int>(gi->second), amb);
            const std::string* sr = net::getSig(sig_rows, amb, &net::SatSignalState::eq34_sat_root);
            if (sr != nullptr)
                new_sat_root.emplace(amb, *sr);
        }
        // Receiver-side parents ("<name>|<c>|<σ>" → sat).
        net::byStationName(entities.stations, entities.names,
                           [&](const StationId& sid, const net::StationState& row) {
          for (const auto& [family, cell] : row.signals) {
            if (!cell.eq34_rx_parent) continue;
            const std::string rxkey = entities.names.names[sid.index] + "|" + family;
            const std::string& sat = *cell.eq34_rx_parent;
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
            new_retained_edges.emplace(static_cast<int>(gi->second),
                                       StateKeys::makeAmbiguityKey(sat, sig));
          }
        });
        // Pinned integers ({receiver idx, "sat_signal"} → target).
        for (const auto& [aid, arow] : arc_rows) {
            if (!arow.eq34_prev_target) continue;
            const std::pair<int, std::string> k = net::arcPair(aid);
            const double tgt = *arow.eq34_prev_target;
            if (new_prev_target.count(k)) continue;
            if (aid.sat == SatId{} || aid.signal == SignalId{}) continue;   // a row that names no arc
            if (!carryAlive(static_cast<size_t>(k.first), net::satKey(aid.sat), net::signalKey(aid.signal)))
                continue;
            new_prev_target.emplace(k, tgt);
        }
    }
}

/// Sticky baselines for next epoch: this epoch's tree becomes the
/// incumbent forest (sat parents + receiver parents, replaced
/// wholesale so the pair of maps always describes one tree), each
/// sat remembers its component root for the r1-handover check, and
/// each pin remembers its target for the same-station pinmove.
/// anchor_incumbent doubles as the was-lit gate for pin-to-round
/// (every tree sat has exactly one parent edge).
///
void advanceChainBaselines(ChainElection& el) {
    net::NetworkEntities& entities = el.entities;
    std::vector<NetStation>& stations = el.stations;
    const std::vector<bool>& present = el.present;
    const auto& sat_el = el.cnd.sat_el;
    auto& this_epoch_anchor = el.this_epoch_anchor;
    auto& new_rx_parent = el.new_rx_parent;
    auto& new_sat_root = el.new_sat_root;
    auto& new_prev_target = el.new_prev_target;
    auto& new_retained_edges = el.new_retained_edges;
    const bool eq34_extend_only = el.extend_only;
    const bool eq34_longevity = el.longevity;
    net::assignSig(entities, &net::SatSignalState::anchor_incumbent, this_epoch_anchor);
    // The receiver-side parents are replaced wholesale ("<name>|<c>|<sigma>" -> sat).
    net::dropAll(entities.stations, &net::StationState::signals,
                 &net::StationSignalState::eq34_rx_parent);
    for (const auto& [rxkey, sat] : new_rx_parent) {
        const auto p2 = rxkey.rfind('|');
        const auto p1 = (p2 == std::string::npos || p2 == 0) ? std::string::npos
                                                              : rxkey.rfind('|', p2 - 1);
        const std::optional<StationId> sid =
            p1 == std::string::npos ? std::nullopt : net::stationOfName(entities, rxkey.substr(0, p1));
        if (sid)
            net::put(entities.stations, *sid, &net::StationState::signals, rxkey.substr(p1 + 1),
                     &net::StationSignalState::eq34_rx_parent) = sat;
    }
    net::assignSig(entities, &net::SatSignalState::eq34_sat_root, new_sat_root);
    net::assignArcAmb(entities, &net::ArcState::eq34_prev_target, new_prev_target);
    if (eq34_extend_only)
        net::assignArcAmbFlags(entities, &net::ArcState::eq34_retained_edge, new_retained_edges);
    // Snapshot this epoch's arc elevations (I4) so next epoch's arcLife
    // can tell a rising arc from a setting one.  Rebuilt wholesale (a
    // pruned arc drops out); skipped when the longevity election is
    // off, so the map stays empty on the B9 baseline.
    if (eq34_longevity) {
        net::dropAll(entities.los, &net::LosState::eq34_prev_el);
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& [sat, elevation] : sat_el[i])
                net::putLos(entities, static_cast<int>(i), sat, &net::LosState::eq34_prev_el) = elevation;
        }
    }
}

/// The last step of the election: the rank cross-check, the carry hygiene (the re-reference erase,
/// the S-transformation apply and the extend-only merge-back) and the sticky baselines for the next
/// epoch.  The families' results are in the election's maps and sets; nothing here reads a family
/// again.
void finishChainElection(ChainElection& el) {
    if (el.trace_chain) {
        el.chain_obs.shift_sat = el.shift_sat;
        el.chain_obs.shift_rpb = el.shift_rpb;
    }
    checkChainGauge(el);
    eraseReferencedCarries(el);
    applyCarryShift(el);
    retainGapEdges(el);
    advanceChainBaselines(el);
}

}  // namespace

void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            const AnchorHygiene& hyg,
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys, EpochEvents& events) {
net::NetworkEntities& entities = ctx.entities;   // eq34_rx_parent: the station signals (N4a); datum_rx: the constellation channels (N4b)
net::StationTable& sta_rows = ctx.entities.stations;   // rpb: the station signals (N4a)
// The frame observatory (network/solve/ChainFrame.hpp): with network.trace_chain_datum armed
// the election records each tree edge, component and chord for the post-solve observer.  It
// only appends to `chain_obs`; nothing below reads it.
const bool trace_chain = ctx.knobs.trace_chain_datum;
if (trace_chain) events.phase_datum.chain = std::make_shared<ChainEpochObs>();   // written: a fresh record
ChainEpochObs chain_unarmed;   // the record's stand-in with the observatory off (written by no guarded line)
ChainEpochObs& chain_obs = trace_chain ? *events.phase_datum.chain : chain_unarmed;
chain_obs.armed = trace_chain;
ChainElection el(ctx, config_, log, stations, present, cnd, hyg, carry_slipped_ambs, events,
                 entities, sta_rows, chain_obs);

    // Eq-34 / de Jonge single-receiver-chain datum.  Per family (a constellation's signal, or
    // with network.phase_datum_eq34_family "pair" the L1/L2 pair of a constellation) connected
    // component of the bipartite (receiver x satellite) observation graph, elect a datum
    // receiver r1 (its RPB is pinned to 0) and pin the ambiguities on the edges of a spanning
    // tree rooted at r1.  Every between-satellite reference is then an integer combination
    // across the whole component, including satellites r1 does not see, because each chain hop
    // contributes only an integer (the hop receiver's fractional bias is its own estimable RPB
    // state delta_{r,sigma} and is not part of the pin).  With full visibility (regional case)
    // this is the Odijk Eq-34 star (CC-R).  The shared set is the ungated union (as in the
    // retired ensemble datum); the chain pins fix the enlarged (receiver+satellite) gauge.  Per
    // (const,sigma) component with n_c stations and m_c sats: (n_c+m_c-1) tree-edge ambiguity
    // pins + 1 RPB pin = n_c+m_c, the gauge dimension.  Every RPB-carrying station is a tree
    // vertex, so it gets >=1 pin and its N_rr stays PD (the rank invariant D3c).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (empty => no scaling)
        st.pinned_amb_seed_lambda.clear();   // seed-round fold-deferred (empty => inert)
    }
    // this_epoch_anchor is an epoch event: empty at the epoch's start.
    // Chain-pin audit (SPEC_synthetic_minimum_network 14 T1): this epoch's per-group
    // declared gauge dimension + shared-side bridge pins, filled by the families (epoch events).
    // Read after PASS B's serial consume.
    shared_phase_keys.clear();
    for (const auto& [amb, pbkey] : cnd.union_keys) shared_phase_keys.insert(pbkey);
    const ChainGroups groups = groupChainKeys(el);
    for (const ChainFamily& fam : formChainFamilies(el, groups)) electFamily(el, groups, fam);
    finishChainElection(el);
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the eq34 chain datum: re-references, pin audit, seed round, slip-aware election.
int networkChainDatumRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& eq34_reref_total = v.ctx.counters.eq34_reref_total;
    const auto& eq34_erase_total = v.ctx.counters.eq34_erase_total;
    const auto& eq34_shift_total = v.ctx.counters.eq34_shift_total;
    const auto& eq34_cpl_sprev_total = v.ctx.counters.eq34_cpl_sprev_total;
    const auto& eq34_gap_repin_total = v.ctx.counters.eq34_gap_repin_total;
    const auto& eq34_mismatch_epochs = v.ctx.counters.eq34_mismatch_epochs;

    if (config_.networkPhaseDatumEq34())
        log->info("Network (eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} coupling s_prev reference(s) re-based with them (§14 — on an "
                  "eq34+coupling lineage 0 here with nonzero shifts is the "
                  "wiring-inert alarm), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_cpl_sprev_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    // The pair family (network.phase_datum_eq34_family "pair"), reported only when armed so that
    // a twin with the knob off keeps a run-end block byte-identical to a build without it.
    if (ctx.knobs.eq34_pair_family) {
        const auto& c = ctx.counters;
        if (c.eq34_pair_families == 0)
            log->warn("Network (eq34 pair family): ARMED but ZERO pair families were formed - every "
                      "constellation-epoch ({} of them) was elected per signal.  Standing invariant "
                      "#4: that is INERT, not validated.  A pair family needs a constellation that "
                      "tracks exactly two phase signals of different bands in an epoch.",
                      c.eq34_pair_fallbacks);
        else
            log->info("Network (eq34 pair family): {} pair family(ies) elected, {} "
                      "constellation-epoch(s) elected per signal instead (one signal, three or more, "
                      "or two of one band); {} extension edge(s) attached a vertex only one band "
                      "tracks to a pair tree on that band, and {} band-only component(s) were elected "
                      "on their own band.",
                      c.eq34_pair_families, c.eq34_pair_fallbacks, c.eq34_pair_ext_edges,
                      c.eq34_pair_band_only_comps);
    }
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
        log->info("Network (slip-aware election): {} slipped incumbent(s) DE-SEEDED "
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
            log->warn("Network: the slip-aware election is ARMED but DE-SEEDED "
                      "NOTHING — no slipped arc was ever an incumbent tree edge this run.  "
                      "Standing invariant #4: that is INERT, not proof the policy works.  "
                      "Check that slips reached the funnel at all (the slip-QC lines above); "
                      "with few slips and a dense graph a zero here is genuinely expected.");
        else if (ra >= ds)
            log->warn("Network: the slip-aware election de-seeded {} arc(s) but {} "
                      "slipped arc(s) still ended up in the tree — the graph, not the "
                      "election, is the binding constraint here and this knob is buying "
                      "little.  A denser observation graph (or a per-component redundancy "
                      "check) is the lever, not this policy.", ds, ra);
    }
    return 0;
}

// ---- End of the run-end reports ----

// ---- PASS B serial stages (generated by scripts/nom_pb1_cut.py) ----

void networkEq34PinAudit(NetworkRun& run, const NetworkEpoch& ep, const EpochAssembly& assembly,
                         bool epoch_ok) {
    // ---- From the run and the epoch (generated by scripts/nom_pb1_cut.py,
    //      each line checked by scripts/nom_pb1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    const std::vector<StationAssembly>& asm_res = assembly.asm_res;
    const std::vector<int>& est_index_of = assembly.est_index_of;
    const EpochEvents::PhaseDatum& phase_datum_ev = ep.events.phase_datum;
    // ---- End of the bindings ----
    net::NetworkEntities& entities = ctx.entities;   // written: arcs
    // The alias lines of networkPassB_ this stage reads, verbatim.
    auto& stations = setup.stations;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;

    // Chain-pin audit (SPEC_synthetic_minimum_network §14 T1): compare the pins that
    // landed (folded by foldAmb/foldRpb and on a station est.addStation accepted)
    // against the election's declared per-group gauge dimension Σ (n_c + m_c), minus
    // the hygiene bridge pins realized on the shared side.  Serial, after the consume,
    // so acceptance is known (est_index_of ≥ 0); an aborted epoch (pivot failure)
    // never reaches a solve and is not audited.  Log-only run tallies; no solve input
    // is touched.
    if (!(phase_datum_eq34 && epoch_ok)) return;
    auto& pa = ctx.counters.eq34_pin_audit;
    ++pa.epochs;
    std::map<std::string, long long> folded_ep, landed_ep;
    for (size_t i = 0; i < stations.size(); ++i) {
        for (const auto& [grp, t] : asm_res[i].pin_audit) {
            folded_ep[grp]     += t[0];
            pa.miss_state[grp] += t[1];
            pa.miss_row[grp]   += t[2];
            if (est_index_of[i] >= 0) landed_ep[grp] += t[0];
        }
        // Feed each accepted station's fold-realized targets into the next
        // epoch's pinmove baseline (seed-round, §3.4).  Without this the election
        // reads the provisional 0 and computes a spurious km-scale step every
        // epoch, re-referencing the (sat,σ) permanently.  A dropped station's pin
        // did not land; its arc re-elects from scratch in the next epoch.
        if (est_index_of[i] >= 0) {
            for (const auto& [amb, tgt, y] : asm_res[i].pin_seed_targets) {
                net::putArcAmb(entities, static_cast<int>(i), amb, &net::ArcState::eq34_prev_target) = tgt;
                ++ctx.counters.seed_round_deferred;
                const double ay = std::abs(y);
                if (ay > ctx.counters.seed_round_max_abs_y_m)
                    ctx.counters.seed_round_max_abs_y_m = ay;
            }
        }
    }
    for (const auto& [grp, decl] : phase_datum_ev.eq34_epoch_declared) {
        const auto bit = phase_datum_ev.eq34_epoch_bridged.find(grp);
        const long long bridged =
            bit == phase_datum_ev.eq34_epoch_bridged.end() ? 0 : bit->second;
        pa.declared[grp] += decl;
        pa.bridged[grp]  += bridged;
        pa.folded[grp]   += folded_ep[grp];
        pa.landed[grp]   += landed_ep[grp];
        pa.dropped_station[grp] += folded_ep[grp] - landed_ep[grp];
        const long long short_n = (decl - bridged) - landed_ep[grp];
        if (short_n > 0) {
            ++pa.short_epochs[grp];
            if (static_cast<int>(short_n) > pa.worst_short[grp])
                pa.worst_short[grp] = static_cast<int>(short_n);
        }
    }
}

// ---- End of the PASS B serial stages ----

// ---- Harvest half (generated by scripts/nom_hv1_cut.py) ----

void networkHarvestRxPhaseBias(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run, the epoch and the harvest (generated by
    //      scripts/nom_hv1_cut.py, each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    double pl_now_s = ep.clock.pl_now_s;
    net::StationTable& sta_rows = ctx.entities.stations;   // the station carries
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    // ---- End of the bindings ----
    // --- RPB carry (eq34): harvest δ̂_{r,σ}, stash and prune ----------
    // Mirrors the ZWD harvest.  There is no coupling harvest for RPB
    // columns, on purpose: coupling rows would give the RPB a de facto
    // shared-border footprint (the D6 v2 caveat zone, tuGaugeRestore group
    // exclusions); the diagonal conditional variance keeps the carry strictly
    // local and conservative.
    if (!(eq34_rpb_q > 0.0)) return;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (R.pl_rpb.empty()) continue;
        const std::size_t est_idx =
            static_cast<std::size_t>(est_index_of[i]);
        Eigen::VectorXd xr, vard;
        std::vector<Eigen::Index> cols;
        cols.reserve(R.pl_rpb.size());
        for (const auto& t : R.pl_rpb) cols.push_back(std::get<1>(t));
        try {
            xr = est.reconstructStation(est_idx, x_s);
            vard = est.conditionalLocalVarianceDiag(
                est_idx, cols, plan.shared_datum);
        } catch (...) { continue; }
        for (std::size_t a = 0; a < R.pl_rpb.size(); ++a) {
            const auto& [rpb_key, rcol, rx_lin] = R.pl_rpb[a];
            if (rcol < 0 || rcol >= xr.size()) continue;
            const double value_abs = rx_lin + xr(rcol);
            const double var = (a < static_cast<std::size_t>(vard.size()))
                                   ? vard(a) : -1.0;
            if (!std::isfinite(value_abs) || !(var > 0.0)
                || !std::isfinite(var)) continue;
            net::put(sta_rows, StationId{static_cast<int>(i)}, &net::StationState::signals, rpb_key,
                     &net::StationSignalState::rpb) =
                LocalCarry{value_abs, var, pl_now_s};
        }
    }
    // The horizon is the registry's (network.phase_leveling_max_gap_s, the same
    // double as pl_max_gap_s).
    carryGapPrune(sta_rows, &net::StationState::signals, &net::StationSignalState::rpb, pl_now_s,
                  ctx.params.spec(NetworkParamKind::RxPhaseBias).gap_horizon_s,
                  [&](const auto&, const auto&, const auto&) {
                      ++ctx.counters.n_cr_rpb_gap_prune;   // same predicate, same census
                      prmRespond(ctx.counters.prm_resp, ResponseSite::RpbGapPrune);
                  });
}

// ---- End of the harvest half ----

} // namespace GnssProcessor
