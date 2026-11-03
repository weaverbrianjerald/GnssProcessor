/**
 * @file ChainFrame.cpp
 * @brief The frame observatory of the eq34 chain datum (see the header).
 */

#include "network/solve/ChainFrame.hpp"

#include "common/GnssIdentity.hpp"          // SatId, SignalId, nominalWavelengthOfDigitM
#include "config/GnssConfig.hpp"
#include "filter/IlsCore.hpp"               // ils::ilsFixDecorrelatedPartial
#include "filter/StateKeySchema.hpp"        // StateKeys::makeAmbiguityKey
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/CarrySites.hpp"     // the chain shift's per-site moved tallies
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"         // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

namespace GnssProcessor {

// ---- The chain-event table ----------------------------------------------------------------------

const std::array<ChainEventSpec, kChainEventRowCount> kChainEventSpec = {{
    {ChainEventRow::Kept, "Kept", "kept", "kept",
     "keyed on parent identity; a flip takes target minus a stale carry",
     "arc identity plus life stamp; target frozen; no prune fallback to 0",
     ChainStep::Zero, ChainCarry::Keep, ChainWire::None, true},
    {ChainEventRow::Birth, "Birth", "", "",
     "0, or lambda round(float) under seed-round",
     "unchanged (seed-round by decision D-S)",
     ChainStep::New, ChainCarry::Keep, ChainWire::NewKey, false},
    {ChainEventRow::Returning, "Returning", "returning", "returning",
     "baseline 0: pinmove lambda round(carry), marked known",
     "certified from its own previous carries; else cut release and treated as a birth",
     ChainStep::ZeroOrCutRelease, ChainCarry::KeepOrCutRelease, ChainWire::NoneOrDeclareMoved, true},
    {ChainEventRow::RehostCertified, "RehostCertified", "rehost", "rehost",
     "pinmove = target minus carry (a fraction)",
     "the chord's integer from the previous chord table, jointly certified; no shift",
     ChainStep::Zero, ChainCarry::Keep, ChainWire::None, false},
    {ChainEventRow::RehostUncertified, "RehostUncertified", "rehost", "rehost",
     "pinmove = target minus carry (a fraction)",
     "rounded integer target, cut release, declaration",
     ChainStep::CutRelease, ChainCarry::CutRelease, ChainWire::DeclareMoved, false},
    {ChainEventRow::RootHandover, "RootHandover", "root", "root_handover",
     "unknown step: the component's carries erased",
     "the new root's receiver phase bias pinned at its carried value",
     ChainStep::Zero, ChainCarry::Keep, ChainWire::None, false},
    {ChainEventRow::TreeEdgeSlip, "TreeEdgeSlip", "slip", "slip_deseat",
     "read from carry_slipped_ambs (empty on the float vehicle): the slipped edge stays",
     "read the free-epoch latch with its cause bits, de-seat at the slip epoch, re-host from previous values",
     ChainStep::ZeroOrCutRelease, ChainCarry::ReleaseArc, ChainWire::NoneOrDeclareMoved, false},
    {ChainEventRow::Merge, "Merge", "merge_split", "merge",
     "none",
     "cut release of the absorbed side",
     ChainStep::CutRelease, ChainCarry::CutRelease, ChainWire::ComponentChange, false},
    {ChainEventRow::Split, "Split", "merge_split", "split",
     "none",
     "the split-off root pinned at its carried receiver phase bias; the component change declared",
     ChainStep::Zero, ChainCarry::Keep, ChainWire::ComponentChange, false},
}};

const ChainEventSpec& chainEventSpec(ChainEventRow r) {
    return kChainEventSpec[static_cast<std::size_t>(r)];
}

std::vector<std::string> chainEventRowEntries() {
    std::vector<std::string> entries;
    for (const ChainEventSpec& s : kChainEventSpec) {
        const std::string entry = s.rows_entry;
        if (!entry.empty() && std::find(entries.begin(), entries.end(), entry) == entries.end())
            entries.push_back(entry);
    }
    return entries;
}

bool chainEventRowEntryBuilt(const std::string& entry) {
    bool named = false;
    for (const ChainEventSpec& s : kChainEventSpec) {
        if (entry != s.rows_entry) continue;
        if (!s.built) return false;
        named = true;
    }
    return named;
}

bool chainEventRowArmed(const NetworkKnobs& k, ChainEventRow r) {
    switch (r) {
        case ChainEventRow::Kept:      return k.eq34_row_kept;
        case ChainEventRow::Returning: return k.eq34_row_returning;
        default:                       return false;   // the other rows' integer rules are not built
    }
}

double chainKeptAsBuiltStep(const ChainEdgeObs& e, const double* remembered) {
    if (!e.legacy_known) return std::numeric_limits<double>::quiet_NaN();
    if (e.incumbent_flipped || remembered == nullptr) return e.legacy_pinmove_m;
    return e.legacy_target_m - *remembered;
}

bool chainKeptEdgeMoved(const ChainEdgeObs& e, const double* remembered) {
    if (!e.row_armed) return false;
    if (e.incumbent_flipped || !e.legacy_known) return true;
    return chainKeptAsBuiltStep(e, remembered) != 0.0;
}

const char* chainStepName(ChainStep s) {
    switch (s) {
        case ChainStep::Zero:             return "Zero";
        case ChainStep::New:              return "New";
        case ChainStep::ZeroOrCutRelease: return "ZeroOrCutRelease";
        case ChainStep::CutRelease:       return "CutRelease";
    }
    return "?";
}

const char* chainCarryName(ChainCarry c) {
    switch (c) {
        case ChainCarry::Keep:             return "Keep";
        case ChainCarry::KeepOrCutRelease: return "KeepOrCutRelease";
        case ChainCarry::CutRelease:       return "CutRelease";
        case ChainCarry::ReleaseArc:       return "ReleaseArc";
    }
    return "?";
}

const char* chainWireName(ChainWire w) {
    switch (w) {
        case ChainWire::None:               return "None";
        case ChainWire::NewKey:             return "NewKey";
        case ChainWire::NoneOrDeclareMoved: return "NoneOrDeclareMoved";
        case ChainWire::DeclareMoved:       return "DeclareMoved";
        case ChainWire::ComponentChange:    return "ComponentChange";
    }
    return "?";
}

const char* chainLegacyCauseName(ChainLegacyCause c) {
    switch (c) {
        case ChainLegacyCause::SameEdge:      return "same_edge";
        case ChainLegacyCause::FirstSighting: return "first_sighting";
        case ChainLegacyCause::RehostCarry:   return "rehost_carry";
        case ChainLegacyCause::RehostFramed:  return "rehost_framed";
        case ChainLegacyCause::RehostCold:    return "rehost_cold";
        case ChainLegacyCause::SeedDeferred:  return "seed_deferred";
        case ChainLegacyCause::SeedStep:      return "seed_step";
        case ChainLegacyCause::RootHandover:  return "root_handover";
    }
    return "?";
}

const char* chainCertSourceName(ChainCertSource s) {
    switch (s) {
        case ChainCertSource::None:    return "none";
        case ChainCertSource::Pair:    return "pair";
        case ChainCertSource::Lattice: return "lattice";
    }
    return "?";
}

// ---- The certification of one chord -------------------------------------------------------------

void chainCertifyChord(double f1, double f2, double q11, double q12, double q22,
                       double min_success_rate, double ratio_threshold,
                       double ratio_upper_bound, double max_frac, ChainChordRow& row) {
    row.have_int_1 = row.have_int_2 = false;
    const double s2 = kChainCertSigmaScale * kChainCertSigmaScale;
    Eigen::Vector2d a(f1, f2);
    Eigen::Matrix2d Q;
    Q << s2 * q11, s2 * q12, s2 * q12, s2 * q22;
    if (!(a.allFinite() && Q.allFinite()) || !(Q(0, 0) > 0.0) || !(Q(1, 1) > 0.0)
        || !(Q.determinant() > 0.0))
        return;
    ils::IlsGateConfig gates;
    gates.min_success_rate     = min_success_rate;
    gates.ratio_threshold      = ratio_threshold;
    gates.ratio_upper_bound    = ratio_upper_bound;
    gates.nl_max_per_pair_frac = max_frac;
    const ils::IlsResult r =
        ils::ilsFixDecorrelatedPartial(a, Q, gates, kChainCertDeterminedSigmaCyc);
    for (std::size_t k = 0; k < r.kept.size(); ++k) {
        const long long v = static_cast<long long>(std::llround(r.afix(static_cast<Eigen::Index>(k))));
        if (r.kept[k] == 0) { row.have_int_1 = true; row.int_1 = v; }
        if (r.kept[k] == 1) { row.have_int_2 = true; row.int_2 = v; }
    }
}

namespace {

// ---- Helpers ----------------------------------------------------------------------------------

std::string stationKey(int station) { return "R:" + std::to_string(station); }
std::string satKeyOf(const std::string& sat) { return "S:" + sat; }

/// The carrier wavelength of a phase signal of a satellite's constellation (0: unknown).
double wavelengthOf(const std::string& sat, const std::string& signal) {
    const std::optional<SatId> id = SatId::parse(sat);
    if (!id) return 0.0;
    const std::optional<SignalId> sig = SignalId::fromObsCode(id->sys, signal);
    if (!sig) return 0.0;
    const double lam = nominalWavelengthOfDigitM(id->sys, sig->digit);
    return (std::isfinite(lam) && lam > 0.0) ? lam : 0.0;
}

/// True when x is within 1e-6 of an integer.
bool nearInteger(double x) { return std::isfinite(x) && std::abs(x - std::round(x)) <= 1e-6; }

/// The CHAIN event names of the returning row's cut release (the frame grader reads both).
constexpr const char* kChainCutReleaseEvent   = "cut_release";
constexpr const char* kChainDeclaredKeysEvent = "declared_keys";

/// A blank cell for NaN, else the value with @p digits decimals.
std::string cell(double v, int digits) {
    if (!std::isfinite(v)) return std::string();
    return fmt::format("{:.{}f}", v, digits);
}

/// The printable name of a vertex key ("R:<idx>" -> station name, "S:<sat>" -> sat).
std::string vertexName(const std::string& key, const std::vector<NetStation>& stations) {
    if (key.size() > 2 && key[0] == 'R') {
        const int i = std::atoi(key.c_str() + 2);
        if (i >= 0 && static_cast<std::size_t>(i) < stations.size()) return stations[static_cast<std::size_t>(i)].name;
        return key;
    }
    return key.size() > 2 ? key.substr(2) : key;
}

/// The band-1 and band-2 signal of a pair family for one arc: every band-1 (digit '1') signal
/// pairs with the arc's lowest-digit signal of another band.
std::vector<std::pair<std::string, std::string>> pairFamilies(const std::set<std::string>& sigs) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string other;
    for (const std::string& s : sigs)
        if (s.size() >= 2 && s[1] != '1' && (other.empty() || s[1] < other[1])) other = s;
    if (other.empty()) return out;
    for (const std::string& s : sigs)
        if (s.size() >= 2 && s[1] == '1') out.emplace_back(s, other);
    return out;
}

// ---- One epoch's classification ---------------------------------------------------------------

/// The components of one group at the last observed epoch and now, for the component events.
struct GroupChange {
    std::map<int, std::set<int>> prev_of_comp;   ///< Component now -> the previous components of its vertices.
    std::map<int, std::set<int>> comps_of_prev;  ///< Previous component -> the components now holding its vertices.
    std::set<int>                split_pieces;   ///< Components now that split off a previous component.
    std::set<int>                root_changed;   ///< Components now whose root differs from their predecessor's.
    std::map<int, int>           dominant_prev;  ///< Component now -> the previous component holding most of its vertices.
};

GroupChange groupChange(const ChainFrameMemory& mem, const std::string& g,
                        const std::vector<const ChainComponentObs*>& comps) {
    GroupChange c;
    if (!mem.have) return c;
    const auto vit = mem.vertex_comp.find(g);
    if (vit == mem.vertex_comp.end()) return c;
    const std::map<std::string, int>& prev = vit->second;
    for (const ChainComponentObs* k : comps) {
        std::map<int, int> votes;
        auto vote = [&](const std::string& key) {
            const auto p = prev.find(key);
            if (p != prev.end()) {
                c.prev_of_comp[k->comp].insert(p->second);
                c.comps_of_prev[p->second].insert(k->comp);
                ++votes[p->second];
            }
        };
        for (int s : k->stations) vote(stationKey(s));
        for (const std::string& s : k->sats) vote(satKeyOf(s));
        int best = -1, best_n = 0;
        for (const auto& [pc, n] : votes)
            if (n > best_n) { best = pc; best_n = n; }
        if (best >= 0) c.dominant_prev[k->comp] = best;
    }
    const auto rit = mem.comp_root.find(g);
    for (const auto& [pc, now] : c.comps_of_prev) {
        if (now.size() < 2) continue;
        // The piece that keeps the old root is the continuation; every other piece split off.
        std::string old_root;
        if (rit != mem.comp_root.end()) {
            const auto r = rit->second.find(pc);
            if (r != rit->second.end()) old_root = r->second;
        }
        int keeper = -1;
        for (const ChainComponentObs* k : comps)
            if (now.count(k->comp) && k->r1 == old_root) keeper = k->comp;
        if (keeper < 0) {   // the old root left: the largest piece continues
            std::size_t best = 0;
            for (const ChainComponentObs* k : comps)
                if (now.count(k->comp) && k->stations.size() + k->sats.size() > best) {
                    best = k->stations.size() + k->sats.size();
                    keeper = k->comp;
                }
        }
        for (int piece : now)
            if (piece != keeper) c.split_pieces.insert(piece);
    }
    for (const ChainComponentObs* k : comps) {
        const auto d = c.dominant_prev.find(k->comp);
        if (d == c.dominant_prev.end() || rit == mem.comp_root.end()) continue;
        const auto r = rit->second.find(d->second);
        if (r != rit->second.end() && r->second != k->r1 && !c.split_pieces.count(k->comp))
            c.root_changed.insert(k->comp);
    }
    return c;
}

/// Classify one edge into its table row (the header's rules, in precedence order).
void classifyEdge(ChainEdgeObs& e, const ChainFrameMemory& mem, const GroupChange& change,
                  const std::set<std::pair<int, std::string>>& free_ambs,
                  const std::map<std::pair<int, std::string>, std::uint8_t>& free_cause) {
    const std::pair<int, std::string> amb{e.station, StateKeys::makeAmbiguityKey(e.sat, e.signal)};
    const auto fc = free_cause.find(amb);
    const bool slipped = free_ambs.count(amb) != 0;
    if (fc != free_cause.end()) e.slip_cause = fc->second;

    const std::map<std::string, int>* prev = nullptr;
    if (mem.have) {
        const auto vit = mem.vertex_comp.find(e.group);
        if (vit != mem.vertex_comp.end()) prev = &vit->second;
    }
    auto prevComp = [&](const std::string& key) -> int {
        if (prev == nullptr) return -1;
        const auto p = prev->find(key);
        return p == prev->end() ? -1 : p->second;
    };
    const int cc = prevComp(e.child), pc = prevComp(e.parent);

    if (slipped) {
        e.row = ChainEventRow::TreeEdgeSlip;
    } else if (e.incumbent_same || e.incumbent_flipped) {
        e.row = ChainEventRow::Kept;
    } else if (e.had_parent) {
        e.row = ChainEventRow::RehostUncertified;   // certified below when the chord table says so
    } else if (cc >= 0 && pc >= 0 && cc != pc) {
        e.row = ChainEventRow::Merge;
    } else if (cc < 0) {
        bool seen = false;
        if (mem.have) {
            const auto lit = mem.last_seen_s.find(e.group);
            seen = lit != mem.last_seen_s.end() && lit->second.count(e.child) != 0;
        }
        e.returning = seen;
        e.row = seen ? ChainEventRow::Returning : ChainEventRow::Birth;
    } else {
        // Present at the last epoch without a parent edge: it was a root there.
        e.row = ChainEventRow::RootHandover;
    }
    if (e.row == ChainEventRow::Kept && e.root_unknown)
        e.row = change.split_pieces.count(e.comp) ? ChainEventRow::Split : ChainEventRow::RootHandover;
}

/// Certify a re-host from the chord table of the last observed epoch.
void certifyRehost(ChainEdgeObs& e, const ChainFrameMemory& mem, ChainFrameCounters& cnt) {
    ++cnt.cf_rehosts;
    const auto it = mem.chords.find({e.station, e.sat, e.signal});
    if (it == mem.chords.end()) {
        ++cnt.cf_cert_missing;
        ++cnt.cf_cert[static_cast<std::size_t>(ChainCertSource::None)];
        return;
    }
    const ChainChordRow& c = it->second.first;
    const int band = it->second.second;
    const bool have_pair = band == 1 ? c.have_int_1 : c.have_int_2;
    const bool have_lat  = band == 1 ? c.have_lat_1 : c.have_lat_2;
    const long long vp = band == 1 ? c.int_1 : c.int_2;
    const long long vl = band == 1 ? c.lat_1 : c.lat_2;
    const double f = band == 1 ? c.float_cyc_1 : c.float_cyc_2;
    e.conflict = have_pair && have_lat && vp != vl;
    if (e.conflict) ++cnt.cf_conflicts;
    if (have_pair && !e.conflict) {
        e.certified   = true;
        e.cert_source = ChainCertSource::Pair;
        e.cert_int    = vp;
        e.row         = ChainEventRow::RehostCertified;
        if (vp != std::llround(f)) ++cnt.cf_wrong_cand;
    }
    ++cnt.cf_cert[static_cast<std::size_t>(e.cert_source)];
}

/// The detail tokens of an edge's CHAIN row (space separated, key=value).
std::string edgeDetail(const ChainEdgeObs& e) {
    return fmt::format("sig={} comp={} row={} cert={} src={} cause={} known={} depth={} "
                       "target_cyc={} r1={}",
                       e.signal, e.comp, chainEventSpec(e.row).name, e.certified ? 1 : 0,
                       chainCertSourceName(e.cert_source), chainLegacyCauseName(e.cause),
                       e.known ? 1 : 0, e.depth,
                       e.lambda_m > 0.0 ? fmt::format("{:.6f}", e.target_m / e.lambda_m) : "",
                       e.r1);
}

}  // namespace

// ---- The classification ---------------------------------------------------------------------------

ChainEpochClass chainClassifyEpoch(const ChainEpochObs& obs, const ChainFrameMemory& mem,
                                   const std::set<std::pair<int, std::string>>& free_ambs,
                                   const std::map<std::pair<int, std::string>, std::uint8_t>& free_cause,
                                   ChainFrameCounters& cnt) {
    ChainEpochClass out;
    out.edges = obs.edges;
    std::map<std::string, std::vector<const ChainComponentObs*>> comps_of;
    for (const ChainComponentObs& k : obs.comps) comps_of[k.group].push_back(&k);
    std::map<std::string, GroupChange> change;
    for (const auto& [g, list] : comps_of) change[g] = groupChange(mem, g, list);

    ++cnt.cf_epochs;
    for (ChainEdgeObs& e : out.edges) {
        classifyEdge(e, mem, change[e.group], free_ambs, free_cause);
        if (e.row == ChainEventRow::RehostUncertified) certifyRehost(e, mem, cnt);
        const std::size_t r = static_cast<std::size_t>(e.row);
        ++cnt.cf_edges;
        ++cnt.cf_rows[r];
        if (e.row_armed) ++cnt.cf_rows_armed[r];
        if (!e.known) ++cnt.cf_rows_unknown[r];
        else if (e.lambda_m > 0.0 && e.pinmove_m != 0.0 && !nearInteger(e.pinmove_m / e.lambda_m))
            ++cnt.cf_rows_frac[r];
        if (e.lambda_m > 0.0 && !nearInteger(e.target_m / e.lambda_m)) ++cnt.cf_nonint_targets;
        if (e.incumbent_flipped) ++cnt.cf_flipped;
        if (e.returning) ++cnt.cf_returning;
    }
    for (const ChainComponentObs& k : obs.comps) {
        const GroupChange& gc = change[k.group];
        ChainComponentEvent ev;
        ev.group = k.group;
        ev.signal = k.signal;
        ev.comp = k.comp;
        ev.r1 = k.r1;
        ev.split = gc.split_pieces.count(k.comp) != 0;
        ev.root_changed = gc.root_changed.count(k.comp) != 0;
        const auto pv = gc.prev_of_comp.find(k.comp);
        if (pv != gc.prev_of_comp.end() && pv->second.size() > 1)
            ev.merged = static_cast<int>(pv->second.size() - 1);
        const auto d = gc.dominant_prev.find(k.comp);
        const auto rit = mem.comp_root.find(k.group);
        if (d != gc.dominant_prev.end() && rit != mem.comp_root.end()) {
            const auto rr = rit->second.find(d->second);
            if (rr != rit->second.end()) ev.old_root = rr->second;
        }
        if (ev.root_changed) ++cnt.cf_root_handovers;
        if (ev.split) ++cnt.cf_splits;
        cnt.cf_merges += ev.merged;
        out.comps.push_back(std::move(ev));
    }
    cnt.cf_soft_pins  += obs.soft_pins;
    cnt.cf_bridges    += obs.bridges;
    cnt.cf_gated_keys += obs.gated_keys;
    for (const ChainReleaseObs& r : obs.releases) {
        ++cnt.cf_cut_releases;
        cnt.cf_cut_arcs += r.arcs;
        cnt.cf_cut_pb += r.pb;
        cnt.cf_cut_rpb += r.rpb;
        cnt.cf_declared_keys += static_cast<long long>(r.sats.size());
    }
    for (const auto& [key, s] : obs.shift_sat) {
        ++cnt.cf_known_shifts;
        const double lam = wavelengthOf(key.first, key.second);
        if (lam > 0.0 && !nearInteger(s / lam)) ++cnt.cf_frac_shifts;
    }
    for (const auto& [g, list] : comps_of) {
        const int n = static_cast<int>(list.size());
        int& mx = cnt.cf_comp_max[g];
        mx = std::max(mx, n);
        cnt.cf_comp_sum[g] += n;
        ++cnt.cf_comp_epochs[g];
    }
    return out;
}

void chainRememberEpoch(const ChainEpochObs& obs, const ChainEpochClass& cls,
                        std::map<std::tuple<int, std::string, std::string>, std::pair<ChainChordRow, int>> chords,
                        int week, double tow, double now_s, ChainFrameCounters& cnt) {
    const std::shared_ptr<ChainFrameMemory> prev = cnt.chain_frame_memory;   // alive until the end
    const ChainFrameMemory none;
    const ChainFrameMemory& mem = prev ? *prev : none;
    ChainFrameMemory next;
    next.have = true;
    next.week = week;
    next.tow  = tow;
    next.last_seen_s = mem.last_seen_s;
    next.last_parent = mem.last_parent;
    next.chords = std::move(chords);
    for (const ChainComponentObs& k : obs.comps) {
        auto& vc = next.vertex_comp[k.group];
        auto& seen = next.last_seen_s[k.group];
        auto& parent = next.last_parent[k.group];
        for (int s : k.stations) { vc[stationKey(s)] = k.comp; seen[stationKey(s)] = now_s; parent[stationKey(s)].clear(); }
        for (const std::string& s : k.sats) { vc[satKeyOf(s)] = k.comp; seen[satKeyOf(s)] = now_s; parent[satKeyOf(s)].clear(); }
        next.comp_root[k.group][k.comp] = k.r1;
    }
    for (const ChainEdgeObs& e : cls.edges) {
        next.vertex_parent[e.group][e.child] = e.parent;
        next.last_parent[e.group][e.child] = e.parent;   // a root keeps the empty parent set above
    }
    // The as-built target of every edge an event row decided, for the next epoch's rows (empty while no
    // row is armed): the as-built draft for an edge the kept row held, the realized target for an edge
    // the returning row released.  A release erases the old-frame carry the as-built draft read, so from
    // the next election on the as-built rule reads the carry of the released target; remembering the
    // draft would make the kept row report the release itself as an as-built step one election later.
    std::set<std::tuple<int, std::string, std::string>> released;
    for (const ChainReleaseObs& r : obs.releases) released.insert({r.station, r.sat, r.signal});
    for (const ChainEdgeObs& e : cls.edges) {
        if (!e.row_armed) continue;
        const std::tuple<int, std::string, std::string> arc{e.station, e.sat, e.signal};
        next.kept_as_built_target[arc] = released.count(arc) ? e.target_m : e.legacy_target_m;
    }
    if (mem.have)
        for (const auto& [g, vc] : mem.vertex_comp) {
            const auto nit = next.vertex_comp.find(g);
            for (const auto& [v, k] : vc) {
                (void)k;
                if (nit == next.vertex_comp.end() || !nit->second.count(v)) ++cnt.cf_departed;
            }
        }
    cnt.chain_frame_memory = std::make_shared<ChainFrameMemory>(std::move(next));
}

// ---- The observer ---------------------------------------------------------------------------------

void networkChainFrameObserve(NetworkRun& run, const NetworkEpoch& ep) {
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
    const EpochEvents::PhaseDatum& phase_datum_ev = ep.events.phase_datum;
    const ChainEpochObs* obs_rec = phase_datum_ev.chain.get();
    const RelevelEpochState& relevel = ep.events.slip.relevel;
    ChainFrameCounters& cnt = ctx.counters;   // written: the observatory's counters and memory
    // ---- End of the bindings ----
    // Both knobs resolve to false off an eq34 run (network/driver/Knobs.cpp).
    if (!(ctx.knobs.trace_chain_datum || ctx.knobs.trace_rpb_states)) return;
    const ChainEpochObs no_record;   // an epoch the election did not record
    const ChainEpochObs& obs = obs_rec != nullptr ? *obs_rec : no_record;

    // Each station's ambiguity columns: (sat, signal) -> (column, linearization point).
    auto ambColumns = [&](std::size_t i) {
        std::map<std::pair<std::string, std::string>, std::pair<Eigen::Index, double>> out;
        const StationAssembly& R = asm_res[i];
        const std::vector<PlAmb>& src = R.pl_amb.empty() ? R.qc_amb : R.pl_amb;
        for (const PlAmb& a : src) out[{a.sat, a.signal}] = {a.col, a.x_lin};
        return out;
    };

    std::vector<ChainEdgeObs> classified;   // this epoch's classified edges (the chain trace armed)
    if (ctx.knobs.trace_chain_datum && obs.armed) {
        // The last observed epoch's memory, held alive here while chainRememberEpoch replaces it.
        const std::shared_ptr<ChainFrameMemory> mem_hold =
            cnt.chain_frame_memory ? cnt.chain_frame_memory : std::make_shared<ChainFrameMemory>();
        const ChainFrameMemory& mem = *mem_hold;
        // 1. Classify and certify against the last observed epoch.
        const ChainEpochClass cls =
            chainClassifyEpoch(obs, mem, relevel.free_ambs, relevel.free_cause, cnt);
        const std::vector<ChainEdgeObs>& edges = cls.edges;
        cnt.cf_d4_keys += static_cast<long long>(phase_datum_ev.eq34_reref.size());

        // 2. The pin identity: traced pins (edges, roots, bridges) = declared, folded = declared
        //    minus bridged, per group.
        {
            std::map<std::string, long long> traced, folded;
            for (const ChainEdgeObs& e : edges) ++traced[e.group];
            for (const ChainComponentObs& k : obs.comps)
                traced[k.group] += 1 + static_cast<long long>(k.bridged.size());
            for (std::size_t i = 0; i < asm_res.size() && i < stations.size(); ++i)
                for (const auto& [grp, a] : asm_res[i].pin_audit) folded[grp] += a[0];
            bool ok = true;
            for (const auto& [grp, decl] : phase_datum_ev.eq34_epoch_declared) {
                const auto b = phase_datum_ev.eq34_epoch_bridged.find(grp);
                const long long bridged = b == phase_datum_ev.eq34_epoch_bridged.end() ? 0 : b->second;
                if (traced[grp] != decl || folded[grp] != decl - bridged) ok = false;
            }
            ++cnt.cf_pin_epochs;
            if (!ok) ++cnt.cf_pin_mismatch_epochs;
        }

        // 3. network_chain_edges.csv and the CHAIN rows.
        if (traces.chain_edges_trace.is_open()) {
            for (const ChainEdgeObs& e : edges) {
                const std::string life = e.life_stamp;
                traces.chain_edges_trace
                    << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << e.sys << ','
                    << e.family << ','
                    << e.signal << ',' << e.comp << ',' << e.r1 << ',' << e.station_name << ','
                    << e.sat << ',' << vertexName(e.child, stations) << ','
                    << vertexName(e.parent, stations) << ',' << e.depth << ','
                    << fmt::format("{:.6f}", e.target_m) << ','
                    << (e.lambda_m > 0.0 ? fmt::format("{:.6f}", e.target_m / e.lambda_m) : "") << ','
                    << cell(e.prev_target_m, 6) << ',' << fmt::format("{:.6f}", e.pinmove_m) << ','
                    << (e.known ? 1 : 0) << ',' << chainLegacyCauseName(e.cause) << ','
                    << chainEventSpec(e.row).name << ',' << (e.row_armed ? 1 : 0) << ','
                    << (e.certified ? 1 : 0) << ','
                    << chainCertSourceName(e.cert_source) << ',' << (e.incumbent_flipped ? 1 : 0)
                    << ',' << (e.returning ? 1 : 0) << ',' << life << '\n';
            }
        }
        auto event = [&](const char* name, const std::string& sat, const std::string& station,
                         std::optional<double> stat, const char* action, const std::string& detail) {
            traces.emitEvent(t, "CHAIN", name, sat, station, stat, std::nullopt, action, detail);
            ++cnt.cf_events;
        };
        for (const ChainEdgeObs& e : edges) {
            const ChainEventSpec& spec = chainEventSpec(e.row);
            const std::string detail = edgeDetail(e);
            switch (e.row) {
                case ChainEventRow::RehostCertified:
                case ChainEventRow::RehostUncertified: {
                    // Both edges: the parent edge it left and the one it joined.
                    const std::string old_parent = vertexName(e.prev_parent, stations);
                    if (e.sat_is_child) event(spec.event, e.sat, old_parent, std::nullopt, "leave", detail);
                    else                event(spec.event, old_parent, e.station_name, std::nullopt, "leave", detail);
                    event(spec.event, e.sat, e.station_name, e.pinmove_m, "join", detail);
                    break;
                }
                case ChainEventRow::Returning: {
                    double gap = std::numeric_limits<double>::quiet_NaN();
                    std::string prev_parent;
                    const auto lit = mem.last_seen_s.find(e.group);
                    if (lit != mem.last_seen_s.end()) {
                        const auto s = lit->second.find(e.child);
                        if (s != lit->second.end()) gap = now_s - s->second;
                    }
                    const auto pit = mem.vertex_parent.find(e.group);
                    if (pit != mem.vertex_parent.end()) {
                        const auto p = pit->second.find(e.child);
                        if (p != pit->second.end()) prev_parent = vertexName(p->second, stations);
                    }
                    if (!e.row_armed || !chainEventRowArmed(ctx.knobs, ChainEventRow::Returning)) {
                        event(spec.event, e.sat, e.station_name, e.pinmove_m, "join",
                              detail + fmt::format(" gap_s={} prev={}", cell(gap, 1), prev_parent));
                        break;
                    }
                    // The returning row released the edge: both edges of the event with their
                    // identities.  The vertex leaves the parent it had when it was last seen
                    // (empty: it was a root, or the memory has no parent for it) and joins the
                    // parent of this epoch's edge; the as-built facts the release replaced follow.
                    std::string last_parent;
                    const auto lpit = mem.last_parent.find(e.group);
                    if (lpit != mem.last_parent.end()) {
                        const auto p = lpit->second.find(e.child);
                        if (p != lpit->second.end()) last_parent = vertexName(p->second, stations);
                    }
                    ++cnt.cf_ret_pairs;
                    const double asbuilt_cyc = e.lambda_m > 0.0 ? e.legacy_pinmove_m / e.lambda_m : 0.0;
                    if (!e.legacy_known)                                 ++cnt.cf_ret_asbuilt_unknown;
                    else if (e.legacy_pinmove_m == 0.0)                  ++cnt.cf_ret_asbuilt_zero;
                    else if (e.lambda_m > 0.0 && nearInteger(asbuilt_cyc)) ++cnt.cf_ret_asbuilt_whole;
                    else                                                 ++cnt.cf_ret_asbuilt_other;
                    const std::string returning_detail = detail + fmt::format(
                        " legacy_cause={} legacy_known={} legacy_pinmove_m={:.6f} legacy_target_cyc={} gap_s={}",
                        chainLegacyCauseName(e.cause), e.legacy_known ? 1 : 0, e.legacy_pinmove_m,
                        e.lambda_m > 0.0 ? fmt::format("{:.6f}", e.legacy_target_m / e.lambda_m) : "",
                        cell(gap, 1));
                    const std::string child = vertexName(e.child, stations);
                    const std::string parent = vertexName(e.parent, stations);
                    if (e.sat_is_child)
                        event(spec.event, e.sat, last_parent, std::nullopt, "leave",
                              returning_detail + fmt::format(" child={} parent={}", child, last_parent));
                    else
                        event(spec.event, last_parent, e.station_name, std::nullopt, "leave",
                              returning_detail + fmt::format(" child={} parent={}", child, last_parent));
                    event(spec.event, e.sat, e.station_name, e.pinmove_m, "join",
                          returning_detail + fmt::format(" child={} parent={}", child, parent));
                    break;
                }
                case ChainEventRow::Kept: {
                    // Only an edge the kept row held and the as-built rule would have moved at this
                    // election: a difference between the as-built target and the frozen one that
                    // persists is one event, at the election the as-built target changed, and the
                    // rest of the held edges are the quiet majority.
                    const auto was = mem.kept_as_built_target.find({e.station, e.sat, e.signal});
                    const double* remembered = was == mem.kept_as_built_target.end() ? nullptr : &was->second;
                    if (!chainKeptEdgeMoved(e, remembered)) break;
                    const double step = chainKeptAsBuiltStep(e, remembered);
                    const std::string kept_detail = detail + fmt::format(
                        " legacy_cause={} legacy_known={} legacy_pinmove_m={:.6f} legacy_step_m={} "
                        "legacy_target_cyc={} flipped={}",
                        chainLegacyCauseName(e.cause), e.legacy_known ? 1 : 0, e.legacy_pinmove_m,
                        cell(step, 6),
                        e.lambda_m > 0.0 ? fmt::format("{:.6f}", e.legacy_target_m / e.lambda_m) : "",
                        e.incumbent_flipped ? 1 : 0);
                    if (!e.incumbent_flipped) {
                        event(spec.event, e.sat, e.station_name, e.pinmove_m, "hold", kept_detail);
                        if (!e.legacy_known)                                   ++cnt.cf_kept_hold_unknown;
                        else if (e.lambda_m > 0.0 && nearInteger(step / e.lambda_m)) ++cnt.cf_kept_hold_whole;
                        else                                                   ++cnt.cf_kept_hold_other;
                        break;
                    }
                    ++cnt.cf_kept_reversed;
                    // A reversed arc leaves its incumbent orientation (this epoch's parent as the
                    // child) and joins this epoch's.  The sat and station columns name the same two
                    // endpoints in both rows, so each row says which of them is the child.
                    const std::string child = vertexName(e.child, stations);
                    const std::string parent = vertexName(e.parent, stations);
                    event(spec.event, e.sat, e.station_name, std::nullopt, "leave",
                          kept_detail + fmt::format(" child={} parent={}", parent, child));
                    event(spec.event, e.sat, e.station_name, e.pinmove_m, "join",
                          kept_detail + fmt::format(" child={} parent={}", child, parent));
                    break;
                }
                case ChainEventRow::TreeEdgeSlip:
                    event(spec.event, e.sat, e.station_name, e.pinmove_m, "observe",
                          detail + fmt::format(" slip_cause={} deseated=0", static_cast<int>(e.slip_cause)));
                    break;
                case ChainEventRow::Merge:
                    event(spec.event, e.sat, e.station_name, e.pinmove_m, "join", detail);
                    break;
                default:
                    break;
            }
        }
        // The cut releases of the returning row: one cut_release row per release, then one
        // declared_keys row per satellite of its moved side (the keys the producer declares moved).
        for (const ChainReleaseObs& r : obs.releases) {
            const std::string root = vertexName(r.vertex, stations);
            event(kChainCutReleaseEvent, r.sat, vertexName("R:" + std::to_string(r.station), stations),
                  std::nullopt, "release",
                  fmt::format("sig={} comp={} root={} arcs={} rpb={} pb={} n_rx={} n_sat={}", r.signal, r.comp,
                              root, r.arcs, r.rpb, r.pb, r.stations.size(), r.sats.size()));
            for (const std::string& sat : r.sats)
                event(kChainDeclaredKeysEvent, sat, "", std::nullopt, "declare",
                      fmt::format("sig={} cause=returning root={}", r.signal, root));
        }
        // The component events: a root handover or a split names the old root (leave) and the
        // new root (join).
        for (const ChainComponentEvent& k : cls.comps) {
            if (!k.root_changed && !k.split) continue;
            const char* name = k.split ? chainEventSpec(ChainEventRow::Split).event
                                       : chainEventSpec(ChainEventRow::RootHandover).event;
            const std::string detail = fmt::format("sig={} comp={} old_root={} new_root={}",
                                                   k.signal, k.comp, k.old_root, k.r1);
            event(name, "", k.old_root, std::nullopt, "leave", detail);
            event(name, "", k.r1, std::nullopt, "join", detail);
        }

        // 4. This epoch's chord table.
        std::set<std::tuple<int, std::string, std::string>> tree_arcs;
        std::set<std::string> chain_signals;
        for (const ChainEdgeObs& e : edges) {
            tree_arcs.insert({e.station, e.sat, e.signal});
            chain_signals.insert(e.sys + "|" + e.signal);
        }
        std::map<std::tuple<int, std::string, std::string>, const ChainChordCycle*> cyc_of;
        for (const ChainChordCycle& c : obs.chords) cyc_of[{c.station, c.sat, c.signal}] = &c;
        const auto& netar = run.nc.ambiguity_resolution;
        const arcgraph::CycleLattice& lattice = ctx.carry.ar_lattice.lattice;
        auto latticeChord = [&](int station, const std::string& sat, const std::string& sig,
                                long long& out) -> bool {
            const auto c = cyc_of.find({station, sat, sig});
            if (c == cyc_of.end() || !c->second->pins_integer || lattice.rows.empty()) return false;
            const std::optional<long long> v = arcgraph::latticeImplied(lattice, c->second->cycle);
            if (!v) return false;
            out = *v - std::llround(c->second->pin_sum_cyc);
            return true;
        };
        std::map<std::tuple<int, std::string, std::string>, std::pair<ChainChordRow, int>> new_chords;
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const auto cols = ambColumns(i);
            std::map<std::string, std::set<std::string>> sigs_of;   // sat -> its phase signals
            for (const auto& [k, v] : cols) {
                (void)v;
                if (k.second.find('_') != std::string::npos) continue;
                sigs_of[k.first].insert(k.second);
            }
            std::vector<ChainChordRow> rows;
            for (const auto& [sat, sigs] : sigs_of) {
                const std::string c = sat.substr(0, 1);
                for (const auto& [s1, s2] : pairFamilies(sigs)) {
                    if (!chain_signals.count(c + "|" + s1) || !chain_signals.count(c + "|" + s2)) continue;
                    const int ii = static_cast<int>(i);
                    if (tree_arcs.count({ii, sat, s1}) || tree_arcs.count({ii, sat, s2})) continue;
                    ChainChordRow r;
                    r.family = c + "|" + s1 + "+" + s2;
                    r.sig1 = s1; r.sig2 = s2;
                    r.station = ii; r.station_name = stations[i].name; r.sat = sat;
                    rows.push_back(r);
                }
            }
            if (rows.empty()) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            Eigen::VectorXd xr;
            std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
            std::vector<std::vector<int>> blocks;
            for (const ChainChordRow& r : rows) {
                const int k = static_cast<int>(reqs.size());
                reqs.emplace_back(est_idx, cols.at({r.sat, r.sig1}).first);
                reqs.emplace_back(est_idx, cols.at({r.sat, r.sig2}).first);
                blocks.push_back({k, k + 1});
            }
            std::vector<Eigen::MatrixXd> covs;
            try {
                xr = est.reconstructStation(est_idx, x_s);
                covs = est.blockConditionalCovariances(reqs, plan.shared_datum, blocks);
            } catch (...) { continue; }
            if (covs.size() != rows.size()) continue;
            for (std::size_t r_i = 0; r_i < rows.size(); ++r_i) {
                ChainChordRow& r = rows[r_i];
                const auto& c1 = cols.at({r.sat, r.sig1});
                const auto& c2 = cols.at({r.sat, r.sig2});
                const double l1 = wavelengthOf(r.sat, r.sig1), l2 = wavelengthOf(r.sat, r.sig2);
                if (!(l1 > 0.0 && l2 > 0.0) || c1.first >= xr.size() || c2.first >= xr.size()) continue;
                const Eigen::MatrixXd& C = covs[r_i];
                if (C.rows() != 2 || C.cols() != 2) continue;
                r.float_cyc_1 = (c1.second + xr(c1.first)) / l1;
                r.float_cyc_2 = (c2.second + xr(c2.first)) / l2;
                r.q11 = C(0, 0) / (l1 * l1);
                r.q12 = C(0, 1) / (l1 * l2);
                r.q22 = C(1, 1) / (l2 * l2);
                chainCertifyChord(r.float_cyc_1, r.float_cyc_2, r.q11, r.q12, r.q22,
                                  netar.min_success_rate, netar.ratio_threshold,
                                  netar.ratio_upper_bound, netar.nl_max_per_pair_frac, r);
                r.have_lat_1 = latticeChord(r.station, r.sat, r.sig1, r.lat_1);
                r.have_lat_2 = latticeChord(r.station, r.sat, r.sig2, r.lat_2);
                r.conflict = (r.have_int_1 && r.have_lat_1 && r.int_1 != r.lat_1)
                          || (r.have_int_2 && r.have_lat_2 && r.int_2 != r.lat_2);
                ++cnt.cf_chord_rows;
                if (r.have_int_1 || r.have_int_2) ++cnt.cf_chord_pair_det;
                if (r.have_lat_1 || r.have_lat_2) ++cnt.cf_chord_lattice;
                if (r.conflict) ++cnt.cf_chord_conflicts;
                if (traces.chain_frame_trace.is_open()) {
                    const char* source = (r.have_int_1 || r.have_int_2) ? "pair" : "none";
                    traces.chain_frame_trace
                        << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << r.family << ','
                        << r.station_name << ',' << r.sat << ','
                        << fmt::format("{:.6f}", r.float_cyc_1) << ','
                        << fmt::format("{:.6f}", r.float_cyc_2) << ','
                        << fmt::format("{:.6e}", r.q11) << ',' << fmt::format("{:.6e}", r.q12) << ','
                        << fmt::format("{:.6e}", r.q22) << ','
                        << (r.have_int_1 ? std::to_string(r.int_1) : "") << ','
                        << (r.have_int_2 ? std::to_string(r.int_2) : "") << ',' << source << ','
                        << (r.conflict ? 1 : 0) << ','
                        << (r.have_lat_1 ? std::to_string(r.lat_1) : "") << '/'
                        << (r.have_lat_2 ? std::to_string(r.lat_2) : "") << '\n';
                }
                new_chords[{r.station, r.sat, r.sig1}] = {r, 1};
                new_chords[{r.station, r.sat, r.sig2}] = {r, 2};
            }
        }

        // 5. Replace the memory.
        chainRememberEpoch(obs, cls, std::move(new_chords), t.week, t.tow, now_s, cnt);
        classified = cls.edges;
    }

    // network_rpb_states.csv: each receiver phase bias with its prior and posterior.
    if (ctx.knobs.trace_rpb_states && traces.rpb_states_trace.is_open()) {
        std::map<std::pair<int, std::string>, const ChainEdgeObs*> rx_parent;
        for (const ChainEdgeObs& e : classified.empty() ? obs.edges : classified)
            if (!e.sat_is_child) rx_parent[{e.station, e.signal}] = &e;
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size() && i < est_index_of.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const std::vector<RpbTraceCol>& rc = asm_res[i].rpb_trace;
            if (rc.empty()) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            std::vector<Eigen::Index> cols;
            for (const RpbTraceCol& c : rc) cols.push_back(c.col);
            Eigen::VectorXd xr, vard;
            try {
                xr = est.reconstructStation(est_idx, x_s);
                vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < rc.size(); ++a) {
                const RpbTraceCol& c = rc[a];
                if (c.col < 0 || c.col >= xr.size()) continue;
                const double value = c.x_lin + xr(c.col);
                const double var = a < static_cast<std::size_t>(vard.size()) ? vard(static_cast<Eigen::Index>(a)) : -1.0;
                const ChainEdgeObs* pe = nullptr;
                {
                    const auto p = rx_parent.find({static_cast<int>(i), c.signal});
                    if (p != rx_parent.end()) pe = p->second;
                }
                double shift = 0.0;
                {
                    const auto s = obs.shift_rpb.find({static_cast<int>(i), c.key});
                    if (s != obs.shift_rpb.end()) shift = s->second;
                }
                std::string event;
                int depth = 0, comp = -1;
                std::string parent_sat;
                if (pe != nullptr) {
                    parent_sat = pe->sat;
                    depth = pe->depth;
                    comp = pe->comp;
                    if (pe->row != ChainEventRow::Kept) event = chainEventSpec(pe->row).name;
                }
                if (std::isfinite(c.tl_var) && c.tl_var > 0.0 && var > 0.0 && c.tl_var > var
                    && std::isfinite(c.tl_value)) {
                    const double d = value - c.tl_value;
                    cnt.cf_rpb_nees_sum += d * d / (c.tl_var - var);
                    ++cnt.cf_rpb_nees_n;
                }
                traces.rpb_states_trace
                    << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << stations[i].name << ','
                    << c.system << ',' << c.signal << ',' << fmt::format("{:.6f}", value) << ','
                    << (var > 0.0 ? fmt::format("{:.6f}", std::sqrt(var)) : "") << ','
                    << (c.pinned ? 1 : 0) << ',' << cell(c.floor_w, 6) << ','
                    << cell(c.tl_var, 9) << ',' << parent_sat << ',' << (pe ? std::to_string(depth) : "")
                    << ',' << (comp >= 0 ? std::to_string(comp) : "") << ',' << event << ','
                    << fmt::format("{:.6f}", shift) << '\n';
                ++cnt.cf_rpb_rows;
            }
        }
    }
}

// ---- The run end --------------------------------------------------------------------------------

int networkChainFrameRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const ChainFrameCounters& c = ctx.counters;
    if (!(ctx.knobs.phase_datum_eq34 && ctx.knobs.trace_chain_datum)) return 0;

    if (c.cf_epochs == 0) {
        log->warn("Network (chain frame): ARMED but ZERO epochs observed: the election recorded "
                  "nothing or no epoch reached the observer.  Nothing about the frame is "
                  "measured by this run.");
        return 0;
    }
    // The armed rows' entries of network.phase_datum_eq34_event_rows (two rows may share one).
    std::vector<std::string> armed;
    for (const ChainEventSpec& s : kChainEventSpec)
        if (chainEventRowArmed(ctx.knobs, s.row)
            && std::find(armed.begin(), armed.end(), std::string(s.rows_entry)) == armed.end())
            armed.push_back(s.rows_entry);
    std::string armed_entries;
    for (const std::string& entry : armed) armed_entries += (armed_entries.empty() ? "" : ", ") + entry;
    if (armed_entries.empty())
        log->info("Network (chain frame): {} epoch(s) observed, {} tree edge(s); no event row is "
                  "armed, so the election ran the legacy rule throughout (observe mode).",
                  c.cf_epochs, c.cf_edges);
    else
        log->info("Network (chain frame): {} epoch(s) observed, {} tree edge(s); event row(s) "
                  "armed: {}; the other rows ran the as-built rule.",
                  c.cf_epochs, c.cf_edges, armed_entries);
    for (std::size_t r = 0; r < kChainEventRowCount; ++r) {
        const ChainEventSpec& s = kChainEventSpec[r];
        if (chainEventRowArmed(ctx.knobs, s.row))
            log->info("Network (chain frame)   {:<18} {:>8} edge(s), {} under the integer rule: "
                      "realized step unknown {}, fractional {}; integer rule {} / {} / {}.",
                      s.name, c.cf_rows[r], c.cf_rows_armed[r], c.cf_rows_unknown[r],
                      c.cf_rows_frac[r], chainStepName(s.step), chainCarryName(s.carry),
                      chainWireName(s.wire));
        else
            log->info("Network (chain frame)   {:<18} {:>8} edge(s): legacy step unknown {}, "
                      "fractional {}; integer rule {} / {} / {}.",
                      s.name, c.cf_rows[r], c.cf_rows_unknown[r], c.cf_rows_frac[r],
                      chainStepName(s.step), chainCarryName(s.carry), chainWireName(s.wire));
    }
    if (chainEventRowArmed(ctx.knobs, ChainEventRow::Kept)) {
        const long long holds = c.cf_kept_hold_whole + c.cf_kept_hold_other + c.cf_kept_hold_unknown;
        log->info("Network (chain frame): {} kept row(s) written to the event trace: {} reversed "
                  "arc(s) (a leave and a join row each) and {} hold row(s), the hold rows by the "
                  "as-built step {} of whole cycles, {} other nonzero and {} unknown.  A hold row marks "
                  "the election at which the as-built target changed, so a difference from the frozen "
                  "target that persists is written once.",
                  2 * c.cf_kept_reversed + holds, c.cf_kept_reversed, holds, c.cf_kept_hold_whole,
                  c.cf_kept_hold_other, c.cf_kept_hold_unknown);
    }
    if (chainEventRowArmed(ctx.knobs, ChainEventRow::Returning))
        log->info("Network (chain frame): {} returning row(s) written to the event trace: {} released edge(s) "
                  "(a leave and a join row each; the as-built step was a known zero on {}, whole cycles on "
                  "{}, another nonzero step on {} and unknown on {}), {} cut_release row(s) and {} "
                  "declared_keys row(s).",
                  2 * c.cf_ret_pairs + c.cf_cut_releases + c.cf_declared_keys, c.cf_ret_pairs,
                  c.cf_ret_asbuilt_zero, c.cf_ret_asbuilt_whole, c.cf_ret_asbuilt_other,
                  c.cf_ret_asbuilt_unknown, c.cf_cut_releases, c.cf_declared_keys);
    const long long n_cert = c.cf_cert[static_cast<std::size_t>(ChainCertSource::Pair)]
                           + c.cf_cert[static_cast<std::size_t>(ChainCertSource::Lattice)];
    log->info("Network (chain frame): {} re-host(s), {} certified ({} by the two-band fix of the "
              "previous chord table, {} by the lattice), {} uncertified ({} of them with no row "
              "in that table); {} pair/lattice conflict(s) counted uncertified; {} "
              "wrong-certification candidate(s) (a certified integer that is not the nearest "
              "integer of the band's own float).",
              c.cf_rehosts, n_cert, c.cf_cert[static_cast<std::size_t>(ChainCertSource::Pair)],
              c.cf_cert[static_cast<std::size_t>(ChainCertSource::Lattice)],
              c.cf_cert[static_cast<std::size_t>(ChainCertSource::None)], c.cf_cert_missing,
              c.cf_conflicts, c.cf_wrong_cand);
    log->info("Network (chain frame): {} non-integer target(s); {} soft pin(s) and {} bridge "
              "pin(s) (no key is claimed); {} legacy re-reference(s) of a (sat, signal) (D4 "
              "erase); {} known shift(s), {} of them fractional (not an integer of cycles).",
              c.cf_nonint_targets, c.cf_soft_pins, c.cf_bridges, c.cf_d4_keys,
              c.cf_known_shifts, c.cf_frac_shifts);
    log->info("Network (chain frame): {} root handover(s), {} merge(s), {} split(s); {} "
              "returning and {} departed vertex(es), {} flipped kept edge(s); {} gated key(s).",
              c.cf_root_handovers, c.cf_merges, c.cf_splits, c.cf_returning, c.cf_departed,
              c.cf_flipped, c.cf_gated_keys);
    // Only the returning row cuts and declares: the kept row does neither, so these stay 0 with it
    // armed alone.
    log->info("Network (chain frame): {} cut-released arc(s), {} phase-bias prior(s) and {} "
              "receiver phase bias(es); {} declared key(s) ({}).",
              c.cf_cut_arcs, c.cf_cut_pb, c.cf_cut_rpb, c.cf_declared_keys,
              armed_entries.empty() ? "all 0 while no event row is armed"
              : chainEventRowArmed(ctx.knobs, ChainEventRow::Returning)
                  ? "cut and declared by the returning row"
                  : "the armed rows cut and declare nothing");
    for (const auto& [g, mx] : c.cf_comp_max) {
        const auto s = c.cf_comp_sum.find(g);
        const auto n = c.cf_comp_epochs.find(g);
        const double mean = (s != c.cf_comp_sum.end() && n != c.cf_comp_epochs.end() && n->second > 0)
                                ? static_cast<double>(s->second) / static_cast<double>(n->second) : 0.0;
        log->info("Network (chain frame)   components of {}: mean {:.2f}, max {}.", g, mean, mx);
    }
    if (c.cf_pin_mismatch_epochs == 0)
        log->info("Network (chain frame): pin identity held in {} of {} epoch(s): traced pins "
                  "(tree edges, roots, bridges) = declared, folded = declared minus bridged, "
                  "every group.", c.cf_pin_epochs, c.cf_pin_epochs);
    else
        log->warn("Network (chain frame): pin identity FAILED in {} of {} epoch(s) (traced, "
                  "declared and folded pins disagree in a group).",
                  c.cf_pin_mismatch_epochs, c.cf_pin_epochs);
    log->info("Network (chain frame): chord table {} row(s): {} with a band determined by the "
              "two-band fix, {} with a band the lattice implies, {} conflict(s).",
              c.cf_chord_rows, c.cf_chord_pair_det, c.cf_chord_lattice, c.cf_chord_conflicts);
    {
        const int m = static_cast<int>(CarryMechanism::Eq34Shift);
        std::string sites;
        for (int i = 0; i < kCarrySites; ++i) {
            const long long n = ctx.counters.prm_ref_moved.site[i][m];
            if (n == 0) continue;
            sites += fmt::format("{}{} {}", sites.empty() ? "" : ", ",
                                 carrySiteSpec(static_cast<CarrySite>(i)).name, n);
        }
        log->info("Network (chain frame): values the chain shift moved, by site: {}.",
                  sites.empty() ? std::string("none") : sites);
    }
    if (ctx.knobs.trace_rpb_states) {
        if (c.cf_rpb_nees_n > 0)
            log->info("Network (chain frame): receiver phase-bias time-link NEES {:.3f} over {} "
                      "term(s) ({} trace row(s)).",
                      c.cf_rpb_nees_sum / static_cast<double>(c.cf_rpb_nees_n), c.cf_rpb_nees_n,
                      c.cf_rpb_rows);
        else
            log->info("Network (chain frame): receiver phase-bias time-link NEES: no time-linked "
                      "term ({} trace row(s); network.phase_datum_eq34_rpb_q_m2s is 0).",
                      c.cf_rpb_rows);
    }
    if (ctx.knobs.trace_pb_prehold)
        log->info("Network (chain frame): {} pre-hold phase-bias row(s) written.",
                  c.cf_pb_prehold_rows);
    log->info("Network (chain frame): {} CHAIN event row(s) written.", c.cf_events);
    return 0;
}

} // namespace GnssProcessor
