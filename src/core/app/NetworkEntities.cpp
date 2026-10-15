/**
 * @file NetworkEntities.cpp
 * @brief The entity field tables, the snapshot record functions of the moved fields, the
 *        station-name converter and the reach table (card NOM step N4).
 *
 * include/app/NetworkEntities.hpp states the model.  Each entity's field table lists its
 * moved fields in the order of the row struct; a Record row's two fill functions sit
 * above its table.
 */
#include "app/NetworkEntities.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "app/NetworkRun.hpp"
#include "app/NetworkRunContext.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkSharedHistory.hpp"   // sharedHistoryMarginalVar (the Z rows)
#include "products/NetworkStateSnapshot.hpp"

namespace GnssProcessor::net {

const char* persistName(Persist p) {
    switch (p) {
    case Persist::Record:          return "Record";
    case Persist::ResumeSeeded:    return "ResumeSeeded";
    case Persist::ColdStart:       return "ColdStart";
    case Persist::Pending:         return "Pending";
    case Persist::RunConst:        return "RunConst";
    case Persist::WriterTelemetry: return "WriterTelemetry";
    }
    return "?";
}

const char* converterName(Converter c) {
    switch (c) {
    case Converter::StationName: return "station name";
    case Converter::ConstellationLetter: return "constellation letter";
    case Converter::Satellite: return "satellite";
    case Converter::SatSignal: return "satellite signal";
    case Converter::Count:       break;
    }
    return "?";
}

void StationNames::assign(std::vector<std::string> roster_names) {
    names = std::move(roster_names);
    index.clear();
    by_name.clear();
    for (std::size_t i = 0; i < names.size(); ++i) {
        index.emplace(names[i], static_cast<int>(i));
        by_name.push_back(static_cast<int>(i));
    }
    std::stable_sort(by_name.begin(), by_name.end(),
                     [&](int a, int b) { return names[a] < names[b]; });
}

std::optional<StationId> stationOfName(const NetworkEntities& e, const std::string& name) {
    const auto it = e.names.index.find(name);
    if (it == e.names.index.end()) {
        ++e.reach.failures[static_cast<std::size_t>(Converter::StationName)];
        return std::nullopt;
    }
    return StationId{it->second};
}

// ===========================================================================
// The field tables
// ===========================================================================

std::optional<System> constellationOfLetter(const NetworkEntities& e, char letter) {
    const System sys = systemFromLetter(letter);
    if (rinexLetter(sys) != letter) {
        ++e.reach.failures[static_cast<std::size_t>(Converter::ConstellationLetter)];
        return std::nullopt;
    }
    return sys;
}

std::optional<System> constellationOfKey(const NetworkEntities& e, const std::string& key) {
    if (key.size() != 1) {
        ++e.reach.failures[static_cast<std::size_t>(Converter::ConstellationLetter)];
        return std::nullopt;
    }
    return constellationOfLetter(e, key[0]);
}

std::optional<std::pair<System, std::string>> channelOfKey(const NetworkEntities& e,
                                                           const std::string& key) {
    if (key.size() < 2 || key[1] != '|') {
        ++e.reach.failures[static_cast<std::size_t>(Converter::ConstellationLetter)];
        return std::nullopt;
    }
    const std::optional<System> sys = constellationOfLetter(e, key[0]);
    if (!sys) return std::nullopt;
    return std::make_pair(*sys, key.substr(2));
}

std::map<std::string, std::string> letterMap(const ConstellationTable& t,
                                             std::optional<std::string> ConstellationState::*f) {
    std::map<std::string, std::string> out;
    forEach(t, f, [&](System sys, const std::string& v) { out[constellationKey(sys)] = v; });
    return out;
}

void mergeLetterMap(NetworkEntities& e, std::optional<std::string> ConstellationState::*f,
                    const std::map<std::string, std::string>& m) {
    for (const auto& [key, v] : m)
        if (const std::optional<System> sys = constellationOfKey(e, key))
            put(e.constellations, *sys, f) = v;
}

void assignLetterMap(NetworkEntities& e, std::optional<std::string> ConstellationState::*f,
                     const std::map<std::string, std::string>& m) {
    dropAll(e.constellations, f);
    mergeLetterMap(e, f, m);
}

// ---- The constellation fields (step N4b) ----

namespace {

using C  = ConstellationState;
using Ch = ChannelState;

template <auto F>
std::size_t constellationRows(const NetworkEntities& e) { return present(e.constellations, F); }

template <auto F>
std::size_t channelRows(const NetworkEntities& e) {
    return present(e.constellations, &C::channels, F);
}

std::size_t arBasisRows(const NetworkEntities& e) {
    std::size_t n = 0;
    for (const auto& kv : e.constellations) n += kv.second.ar_basis.size();
    return n;
}

// Record V: the two pivot maps, one-letter key -> pivot satellite.  One tag, two rows: the
// writer and the reader call both (recordToSnapshot / recordFromSnapshot).
void prevPivotToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.prev_pivot = letterMap(ctx.entities.constellations, &C::prev_pivot);
}
void lastPivotToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.last_pivot = letterMap(ctx.entities.constellations, &C::last_pivot);
}
void prevPivotFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignLetterMap(ctx.entities, &C::prev_pivot, snap.prev_pivot);
}
void lastPivotFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignLetterMap(ctx.entities, &C::last_pivot, snap.last_pivot);
}

// Record GG: (letter, signal, group id), filled in (letter, signal) order (O6).
void ggToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.carry_group_id.clear();
    snap.carry_group_id.reserve(present(ctx.entities.constellations, &C::channels, &Ch::group_id));
    forEach(ctx.entities.constellations, &C::channels, &Ch::group_id,
            [&](System sys, const std::string& sig, int gid) {
                snap.carry_group_id.emplace_back(rinexLetter(sys), sig, gid);
            });
}
void ggFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.constellations, &C::channels, &Ch::group_id);
    for (const auto& gg : snap.carry_group_id)
        if (const std::optional<System> sys = constellationOfLetter(e, std::get<0>(gg)))
            put(e.constellations, *sys, &C::channels, std::get<1>(gg), &Ch::group_id) =
                std::get<2>(gg);
}

// Record CBP: "<letter>|<signal>" -> satellite -> value; the writer drops non-finite values
// (a nan would survive the parse and corrupt the restored datum).
void cbpToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    forEach(ctx.entities.constellations, &C::channels, &Ch::code_bias_prev,
            [&](System sys, const std::string& sig, const std::map<std::string, double>& per_sat) {
                for (const auto& [sat, v] : per_sat)
                    if (std::isfinite(v)) snap.code_bias_prev[constellationKey(sys) + "|" + sig][sat] = v;
            });
}
void cbpFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.constellations, &C::channels, &Ch::code_bias_prev);
    for (const auto& [fam, per_sat] : snap.code_bias_prev)
        if (const auto ch = channelOfKey(e, fam))
            put(e.constellations, ch->first, &C::channels, ch->second, &Ch::code_bias_prev) = per_sat;
}

// Record EQD: "<letter>|<signal>" -> the component datum receiver.
void eqdToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    forEach(ctx.entities.constellations, &C::channels, &Ch::datum_rx,
            [&](System sys, const std::string& sig, const std::string& rx) {
                snap.eq34_datum_rx[constellationKey(sys) + "|" + sig] = rx;
            });
}
void eqdFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.constellations, &C::channels, &Ch::datum_rx);
    for (const auto& [key, rx] : snap.eq34_datum_rx)
        if (const auto ch = channelOfKey(e, key))
            put(e.constellations, ch->first, &C::channels, ch->second, &Ch::datum_rx) = rx;
}

}  // namespace

const std::vector<FieldDecl> kConstellationFields = {
    {"prev_pivot", "prev_pivot", Persist::Record, "V", "",
     &prevPivotToSnapshot, &prevPivotFromSnapshot, &constellationRows<&C::prev_pivot>},
    {"last_pivot", "last_pivot", Persist::Record, "V", "",
     &lastPivotToSnapshot, &lastPivotFromSnapshot, &constellationRows<&C::last_pivot>},
    {"cxu_pivot", "cxu_pivot", Persist::ResumeSeeded, "", "",
     nullptr, nullptr, &constellationRows<&C::cxu_pivot>},
    {"pfq_pivot", "pfq_pivot", Persist::ResumeSeeded, "", "",
     nullptr, nullptr, &constellationRows<&C::pfq_pivot>},
    {"pb_hold_frz_M", "pb_hold_frz_M", Persist::ColdStart, "", "",
     nullptr, nullptr, &constellationRows<&C::pb_hold_frz_M>},
    {"sd_ewl_r0", "sd_ewl_r0", Persist::ColdStart, "", "",
     nullptr, nullptr, &constellationRows<&C::sd_ewl_r0>},
    {"ar_basis", "ar_basis", Persist::ColdStart, "", "",
     nullptr, nullptr, &arBasisRows},
    {"group_id", "carry_group_id", Persist::Record, "GG", "",
     &ggToSnapshot, &ggFromSnapshot, &channelRows<&Ch::group_id>},
    {"code_bias_prev", "code_bias_prev_", Persist::Record, "CBP", "",
     &cbpToSnapshot, &cbpFromSnapshot, &channelRows<&Ch::code_bias_prev>},
    {"datum_rx", "eq34_datum_rx", Persist::Record, "EQD", "",
     &eqdToSnapshot, &eqdFromSnapshot, &channelRows<&Ch::datum_rx>},
    {"ddfrac_ref_age", "ddfrac_ref_age", Persist::ColdStart, "", "",
     nullptr, nullptr, &channelRows<&Ch::ddfrac_ref_age>},
    {"ddfrac_ref_prev", "ddfrac_ref_prev", Persist::ColdStart, "", "",
     nullptr, nullptr, &channelRows<&Ch::ddfrac_ref_prev>},
};
// ---- The satellite fields (step N4e1: the clock half) ----

std::optional<SatId> satIdKey(const std::string& sat) {
    if (sat.empty()) return SatId{};
    const std::optional<SatId> s = SatId::parse(sat);
    if (!s || s->str() != sat) return std::nullopt;
    return *s;
}

std::optional<SatId> satIdOf(const NetworkEntities& e, const std::string& sat) {
    const std::optional<SatId> id = satIdKey(sat);
    if (!id) ++e.reach.failures[static_cast<std::size_t>(Converter::Satellite)];
    return id;
}

bool markSat(NetworkEntities& e, const std::string& sat, bool SatelliteState::*f) {
    const std::optional<SatId> id = satIdOf(e, sat);
    return id ? mark(e.sats, *id, f) : true;
}

std::size_t unmarkSat(SatTable& t, const std::string& sat, bool SatelliteState::*f) {
    const std::optional<SatId> id = satIdKey(sat);
    return id ? unmark(t, *id, f) : 0;
}

std::set<std::string> satSet(const SatTable& t, bool SatelliteState::*f) {
    std::set<std::string> out;
    for (const auto& [id, row] : t)
        if (row.*f) out.insert(satKey(id));
    return out;
}

void assignSatFlags(NetworkEntities& e, bool SatelliteState::*f, const std::set<std::string>& s) {
    for (auto& kv : e.sats) kv.second.*f = false;
    for (const std::string& sat : s) markSat(e, sat, f);
}

namespace {

using A = SatelliteState;

template <auto F>
std::size_t satRows(const NetworkEntities& e) { return present(e.sats, F); }

// Record K: satellite -> the two-state oscillator, field by field.
void kToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    forEach(ctx.entities.sats, &A::clock_osc, [&](const SatId& id, const ClockOsc& o) {
        NetworkStateSnapshot::Osc so;
        so.x0 = o.x0; so.x1 = o.x1;
        so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
        so.t_last = o.t_last; so.init = o.init;
        snap.clock_osc[satKey(id)] = so;
    });
}
void kFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.sats, &A::clock_osc);
    for (const auto& [sat, o] : snap.clock_osc) {
        ClockOsc c;
        c.x0 = o.x0; c.x1 = o.x1;
        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
        c.t_last = o.t_last; c.init = o.init;
        putSat(e, sat, &A::clock_osc) = c;
    }
}

// Records J and Y: satellite-keyed maps, written and restored wholesale.
void jToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.prev_clk = satMap(ctx.entities.sats, &A::prev_clk);
}
void jFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSat(ctx.entities, &A::prev_clk, snap.prev_clk);
}
void yToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.ura_refloat_horizon = satMap(ctx.entities.sats, &A::ura_refloat_horizon);
}
void yFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSat(ctx.entities, &A::ura_refloat_horizon, snap.ura_refloat_horizon);
}

// Records C, R and A: satellite sets as flags, written and restored wholesale.
void cToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.c1_zoh_active = satSet(ctx.entities.sats, &A::c1_zoh_active);
}
void cFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSatFlags(ctx.entities, &A::c1_zoh_active, snap.c1_zoh_active);
}
void rToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.ura_retro_sats = satSet(ctx.entities.sats, &A::ura_retro_sats);
}
void rFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSatFlags(ctx.entities, &A::ura_retro_sats, snap.ura_retro_sats);
}
void aToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.retro_active = satSet(ctx.entities.sats, &A::retro_active);
}
void aFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSatFlags(ctx.entities, &A::retro_active, snap.retro_active);
}

// Record LB (two rows): satellite -> {contrast-frame level information, epochs}.  The
// information row writes the satellites with a finite positive value; the epoch row fills
// their counts (0 when absent).  The restore merges into the table (no reset), as before.
void lbInfoToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    forEach(ctx.entities.sats, &A::clm_info_c, [&](const SatId& id, double info) {
        if (std::isfinite(info) && info > 0.0) snap.level_info[satKey(id)] = {info, 0};
    });
}
void lbNToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    for (auto& [sat, in] : snap.level_info) {
        const int* n = getSat(ctx.entities.sats, sat, &A::clm_n);
        in.second = n ? *n : 0;
    }
}
void lbInfoFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    for (const auto& [sat, in] : snap.level_info) putSat(ctx.entities, sat, &A::clm_info_c) = in.first;
}
void lbNFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    for (const auto& [sat, in] : snap.level_info) putSat(ctx.entities, sat, &A::clm_n) = in.second;
}

}  // namespace

const std::vector<FieldDecl> kSatelliteFields = {
    {"clock_osc", "clock_osc", Persist::Record, "K", "", &kToSnapshot, &kFromSnapshot,
     &satRows<&A::clock_osc>},
    {"prev_clk", "prev_clk", Persist::Record, "J", "", &jToSnapshot, &jFromSnapshot,
     &satRows<&A::prev_clk>},
    {"c1_zoh_active", "c1_zoh_active", Persist::Record, "C", "", &cToSnapshot, &cFromSnapshot,
     &satRows<&A::c1_zoh_active>},
    {"clm_info", "clm_info", Persist::ColdStart, "", "", nullptr, nullptr, &satRows<&A::clm_info>},
    {"clm_info_c", "clm_info_c", Persist::Record, "LB", "", &lbInfoToSnapshot, &lbInfoFromSnapshot,
     &satRows<&A::clm_info_c>},
    {"clm_n", "clm_n", Persist::Record, "LB", "", &lbNToSnapshot, &lbNFromSnapshot,
     &satRows<&A::clm_n>},
    {"clm_prev_clk", "clm_prev_clk", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::clm_prev_clk>},
    {"clm_prev_pidx", "clm_prev_pidx", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::clm_prev_pidx>},
    {"clm_win", "clm_win", Persist::ColdStart, "", "", nullptr, nullptr, &satRows<&A::clm_win>},
    {"readmit_last_seen_ep", "readmit_last_seen_ep", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::readmit_last_seen_ep>},
    {"readmit_marg", "readmit_marg", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::readmit_marg>},
    {"readmit_seeded_s", "readmit_seeded_s", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::readmit_seeded_s>},
    {"ura_refloat_horizon", "ura_refloat_horizon", Persist::Record, "Y", "", &yToSnapshot,
     &yFromSnapshot, &satRows<&A::ura_refloat_horizon>},
    {"ura_retro_sats", "ura_retro_sats", Persist::Record, "R", "", &rToSnapshot, &rFromSnapshot,
     &satRows<&A::ura_retro_sats>},
    {"retro_active", "retro_active", Persist::Record, "A", "", &aToSnapshot, &aFromSnapshot,
     &satRows<&A::retro_active>},
    {"pb_hold_prev_clock_m", "pb_hold_prev_clock_m", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::pb_hold_prev_clock_m>},
    {"pb_hold_frz_prev_clk", "pb_hold_frz_prev_clk", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::pb_hold_frz_prev_clk>},
    {"cb_exc_prev_clk", "cb_exc_prev_clk", Persist::ColdStart, "", "", nullptr, nullptr,
     &satRows<&A::cb_exc_prev_clk>},
};
// ---- The satellite-signal fields (step N4d) ----

std::optional<SatSignalId> satSignalKey(const std::string& sat, const std::string& signal) {
    // An empty component keeps its row: "" is the no-satellite SatId{} (as for the lines of
    // sight) and the satellite-wide SignalId{}; any other string must round-trip (O2).
    SatId s{};
    if (!sat.empty()) {
        const std::optional<SatId> p = SatId::parse(sat);
        if (!p || p->str() != sat) return std::nullopt;
        s = *p;
    }
    if (signal.empty()) return SatSignalId{s, SignalId{}};
    if (signal.size() != 3 || signal[0] != static_cast<char>(ObsType::Phase)
        || signal[1] < '0' || signal[1] > '9' || signal[2] < 'A' || signal[2] > 'Z')
        return std::nullopt;
    return SatSignalId{s, SignalId{s.sys, signal[1], signal[2]}};
}

std::optional<SatSignalId> satSignalKey(const std::string& key) {
    const std::size_t cut = key.find_first_of("_|");
    if (cut == std::string::npos) return std::nullopt;
    return satSignalKey(key.substr(0, cut), key.substr(cut + 1));
}

std::optional<SatSignalId> satSignalOf(const NetworkEntities& e, const std::string& key) {
    const std::optional<SatSignalId> id = satSignalKey(key);
    if (!id) ++e.reach.failures[static_cast<std::size_t>(Converter::SatSignal)];
    return id;
}

std::string signalKey(const SignalId& s) {
    return s == SignalId{} ? std::string() : s.obsCode(ObsType::Phase);
}

namespace {

using G = SatSignalState;

template <auto F>
std::size_t satSignalRows(const NetworkEntities& e) { return present(e.sat_signals, F); }

// Records PA and PT: "<sat>_<signal>" -> the incumbent station / the pin target, written and
// restored wholesale (the old map's assignment both ways).
void paToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.anchor_incumbent = sigMap(ctx.entities.sat_signals, &G::anchor_incumbent, '_');
}
void paFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSig(ctx.entities, &G::anchor_incumbent, snap.anchor_incumbent);
}
void ptToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    snap.anchor_pin_target = sigMap(ctx.entities.sat_signals, &G::anchor_pin_target, '_');
}
void ptFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    assignSig(ctx.entities, &G::anchor_pin_target, snap.anchor_pin_target);
}

}  // namespace

const std::vector<FieldDecl> kSatSignalFields = {
    {"tu_reinit", "tu_reinit_hist", Persist::ResumeSeeded, "", "", nullptr, nullptr,
     &satSignalRows<&G::tu_reinit>},
    {"anchor_incumbent", "anchor_incumbent", Persist::Record, "PA", "",
     &paToSnapshot, &paFromSnapshot, &satSignalRows<&G::anchor_incumbent>},
    {"anchor_pin_target", "anchor_pin_target", Persist::Record, "PT", "",
     &ptToSnapshot, &ptFromSnapshot, &satSignalRows<&G::anchor_pin_target>},
    {"pb_level_prev", "pb_level_prev", Persist::ColdStart, "", "", nullptr, nullptr,
     &satSignalRows<&G::pb_level_prev>},
    {"pin_bridge", "pin_bridge", Persist::ColdStart, "", "", nullptr, nullptr,
     &satSignalRows<&G::pin_bridge>},
    {"phase_basis", "phase_basis", Persist::ColdStart, "", "", nullptr, nullptr,
     &satSignalRows<&G::phase_basis>},
    {"pb_hold", "pb_hold", Persist::ColdStart, "", "", nullptr, nullptr, &satSignalRows<&G::pb_hold>},
    {"smh_clone", "smh_clone", Persist::ColdStart, "", "", nullptr, nullptr,
     &satSignalRows<&G::smh_clone>},
    {"smh_pending", "smh_pending", Persist::Pending, "", "", nullptr, nullptr,
     &satSignalRows<&G::smh_pending>},
    {"smh_block", "smh_block", Persist::ColdStart, "", "", nullptr, nullptr,
     &satSignalRows<&G::smh_block>},
    {"smh_block_pending", "smh_block_pending", Persist::Pending, "", "", nullptr, nullptr,
     &satSignalRows<&G::smh_block_pending>},
};
namespace {

using S  = StationState;
using SS = StationSignalState;

/// The reach-table row count of a station field.
template <auto F>
std::size_t stationRows(const NetworkEntities& e) { return present(e.stations, F); }

/// The reach-table row count of a field keyed below the station.
template <auto F>
std::size_t signalRows(const NetworkEntities& e) { return present(e.stations, &S::signals, F); }

// ---- Record Z: the ZWD carry (was the zwd_carry blocks of the writer and the reader) ----

void zwdToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    const bool shx_marg = ctx.knobs.carry_cpl_shx;
    forEach(ctx.entities.stations, &S::zwd, [&](const StationId& id, const LocalCarry& lc) {
        snap.zwd_carry[id.index] =
            NetworkStateSnapshot::Local{lc.value_abs,
                                        shx_marg ? sharedHistoryMarginalVar(lc) : lc.var,
                                        lc.last_seen_s};
    });
}

void zwdFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    StationTable& t = ctx.entities.stations;
    dropAll(t, &S::zwd);
    for (const auto& [idx, l] : snap.zwd_carry)
        // The restored-entry convention of the L rows (makeRestoredLocalCarry).
        put(t, StationId{idx}, &S::zwd) = makeRestoredLocalCarry(l.value_abs, l.var,
                                                                 l.last_seen_s);
}

// ---- Record EQP: the receiver-side eq34 parent edges, "<name>|<c>|<sigma>" -> satellite ----

void eqpToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    const NetworkEntities& e = ctx.entities;
    byStationName(e.stations, e.names, [&](const StationId& id, const StationState& row) {
        for (const auto& [family, cell] : row.signals)
            if (cell.eq34_rx_parent)
                snap.eq34_rx_parent[e.names.names[id.index] + "|" + family] = *cell.eq34_rx_parent;
    });
}

void eqpFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.stations, &S::signals, &SS::eq34_rx_parent);
    for (const auto& [rxkey, sat] : snap.eq34_rx_parent) {
        // "<name>|<c>|<sigma>": the name is what precedes the last two separators, the
        // parse the election's retain pass uses.
        const auto p2 = rxkey.rfind('|');
        const auto p1 = (p2 == std::string::npos || p2 == 0) ? std::string::npos
                                                              : rxkey.rfind('|', p2 - 1);
        if (p1 == std::string::npos) {
            ++e.reach.failures[static_cast<std::size_t>(Converter::StationName)];
            continue;
        }
        const std::optional<StationId> id = stationOfName(e, rxkey.substr(0, p1));
        if (!id) continue;   // counted by the converter
        put(e.stations, *id, &S::signals, rxkey.substr(p1 + 1), &SS::eq34_rx_parent) = sat;
    }
}

// ---- Record RC: the receiver code-bias carry ----

void rxcbToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    // The non-finite screen at the source: an entry dropped here is only cold on the
    // resume, whereas a corrupt entry cannot be recovered.
    forEach(ctx.entities.stations, &S::signals, &SS::rx_cb,
            [&](const StationId& id, const std::string& key, const LocalCarry& lc) {
                if (std::isfinite(lc.value_abs) && std::isfinite(lc.var) && lc.var > 0.0
                    && std::isfinite(lc.last_seen_s))
                    snap.rx_cb_carry[{id.index, key}] =
                        NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            });
}

void rxcbFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    StationTable& t = ctx.entities.stations;
    dropAll(t, &S::signals, &SS::rx_cb);
    for (const auto& [key, l] : snap.rx_cb_carry)
        // The restored-entry convention of the L / Z rows (makeRestoredLocalCarry).
        put(t, StationId{key.first}, &S::signals, key.second, &SS::rx_cb) =
            makeRestoredLocalCarry(l.value_abs, l.var, l.last_seen_s);
}

}  // namespace

const std::vector<FieldDecl> kStationFields = {
    // name, was, persistence, record, fingerprint, to_snapshot, from_snapshot, rows
    {"zwd", "zwd_carry", Persist::Record, "Z", "",
     &zwdToSnapshot, &zwdFromSnapshot, &stationRows<&S::zwd>},
    {"gross_los", "gl_sta_state", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::gross_los>},
    {"staf_ewma", "staf_ewma", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::staf_ewma>},
    {"staf_updates", "staf_updates", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::staf_updates>},
    {"staf_departed", "staf_departed", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::staf_departed>},
    {"topo_prev_tie", "topo_prev_tie", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::topo_prev_tie>},
    {"redun_unobs_active", "redun_unobs_active", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::redun_unobs_active>},
    {"adm_resume_seen", "adm_resume_seen", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::adm_resume_seen>},
    {"rxcb_lin_restore", "rxcb_lin_restore", Persist::ResumeSeeded, "", "",
     nullptr, nullptr, &stationRows<&S::rxcb_lin_restore>},
    {"resume_canon_rows", "resume_canon_rows", Persist::ResumeSeeded, "", "",
     nullptr, nullptr, &stationRows<&S::resume_canon_rows>},
    {"sd_ewl_fix", "sd_ewl_fix", Persist::ColdStart, "", "",
     nullptr, nullptr, &stationRows<&S::sd_ewl_fix>},
    {"rx_clk", "rx_clk_carry", Persist::ColdStart, "", "",
     nullptr, nullptr, &signalRows<&SS::rx_clk>},
    {"rx_clk_rate", "rx_clk_rate", Persist::ColdStart, "", "",
     nullptr, nullptr, &signalRows<&SS::rx_clk_rate>},
    {"rpb", "rpb_carry", Persist::ColdStart, "", "",
     nullptr, nullptr, &signalRows<&SS::rpb>},
    {"rx_cb", "rx_cb_carry", Persist::Record, "RC", "",
     &rxcbToSnapshot, &rxcbFromSnapshot, &signalRows<&SS::rx_cb>},
    {"eq34_rx_parent", "eq34_rx_parent", Persist::Record, "EQP", "",
     &eqpToSnapshot, &eqpFromSnapshot, &signalRows<&SS::eq34_rx_parent>},
};
std::optional<LosId> losKey(int station, const std::string& sat) {
    if (sat.empty()) return LosId{StationId{station}, SatId{}};
    const std::optional<SatId> s = SatId::parse(sat);
    if (!s || s->str() != sat) return std::nullopt;
    return LosId{StationId{station}, *s};
}

std::optional<LosId> losOf(const NetworkEntities& e, int station, const std::string& sat) {
    const std::optional<LosId> id = losKey(station, sat);
    if (!id) ++e.reach.failures[static_cast<std::size_t>(Converter::Satellite)];
    return id;
}

std::optional<LosId> losOfName(const NetworkEntities& e, const std::string& station,
                               const std::string& sat) {
    const std::optional<StationId> sta = stationOfName(e, station);
    return sta ? losOf(e, sta->index, sat) : std::nullopt;
}

std::string satKey(const SatId& s) { return s == SatId{} ? std::string() : s.str(); }

void assignLosFlags(NetworkEntities& e, bool LosState::*f,
                    const std::set<std::pair<int, std::string>>& cells) {
    for (auto& kv : e.los) kv.second.*f = false;
    for (const auto& [sta, sat] : cells)
        if (const std::optional<LosId> id = losOf(e, sta, sat)) mark(e.los, *id, f);
}

// ---- The line-of-sight fields (step N4c) ----

namespace {

using L  = LosState;
using LE = LosEventState;

template <auto F>
std::size_t losRows(const NetworkEntities& e) { return present(e.los, F); }

template <auto F>
std::size_t losEventRows(const NetworkEntities& e) { return present(e.los, &L::events, F); }

// Record EQE: (station index, satellite) -> elevation.
void eqeToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    forEach(ctx.entities.los, &L::eq34_prev_el, [&](const LosId& id, double el) {
        snap.eq34_prev_el[losPair(id)] = el;
    });
}
void eqeFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.los, &L::eq34_prev_el);
    for (const auto& [key, el] : snap.eq34_prev_el) putLos(e, key.first, key.second, &L::eq34_prev_el) = el;
}

// Record SD: "<station name>|<sat>" -> the deviation (O3: the DTO map orders by the name);
// the writer drops a non-finite entry (cold on the resume, never corrupt).
void sdToSnapshot(const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    const NetworkEntities& e = ctx.entities;
    forEach(e.los, &L::sh_dev, [&](const LosId& id, const ShDevCarry& dc) {
        const auto i = static_cast<std::size_t>(id.station.index);
        if (i < e.names.names.size() && std::isfinite(dc.dev) && std::isfinite(dc.var)
            && dc.var >= 0.0 && std::isfinite(dc.var_ss) && dc.var_ss >= 0.0
            && std::isfinite(dc.t_s))
            snap.sh_dev_carry[e.names.names[i] + "|" + satKey(id.sat)] =
                NetworkStateSnapshot::ShDev{dc.dev, dc.var, dc.var_ss, dc.t_s};
    });
}
void sdFromSnapshot(const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    NetworkEntities& e = ctx.entities;
    dropAll(e.los, &L::sh_dev);
    for (const auto& [key, d] : snap.sh_dev_carry) {
        const auto bar = key.find('|');
        if (bar == std::string::npos) {
            ++e.reach.failures[static_cast<std::size_t>(Converter::StationName)];
            continue;
        }
        const std::optional<StationId> sta = stationOfName(e, key.substr(0, bar));
        if (!sta) continue;   // counted by the converter
        // Restored as written: ShDevCarry's cold test is t_s <= 0, so the writer's stamp
        // reads warm and the aging pre-pass ages it over the true gap.
        putLos(e, sta->index, key.substr(bar + 1), &L::sh_dev) =
            ShDevCarry{d.dev, d.var, d.var_ss, d.t_s};
    }
}

}  // namespace

const std::vector<FieldDecl> kLosFields = {
    {"eq34_prev_el", "eq34_prev_el", Persist::Record, "EQE", "",
     &eqeToSnapshot, &eqeFromSnapshot, &losRows<&L::eq34_prev_el>},
    {"si_arc", "si_arc", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::si_arc>},
    {"iono", "iono_carry", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::iono>},
    {"iono_blk", "iono_blk_carry", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::iono_blk>},
    {"gross_los", "gl_state", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::gross_los>},
    {"gl_event_last", "gl_event_last", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::gl_event_last>},
    {"gl_hold_iono", "gl_hold_iono", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::gl_hold_iono>},
    {"gl_hold_iono_blk", "gl_hold_iono_blk", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::gl_hold_iono_blk>},
    {"iono_cs", "iono_cs", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::iono_cs>},
    {"pc", "pc_carry", Persist::ColdStart, "", "", nullptr, nullptr, &losRows<&L::pc>},
    {"arc_spike_streak", "arc_spike_streak", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::arc_spike_streak>},
    {"sh_dev", "sh_dev_carry", Persist::Record, "SD", "",
     &sdToSnapshot, &sdFromSnapshot, &losRows<&L::sh_dev>},
    {"sd_ewl_prev", "sd_ewl_prev", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::sd_ewl_prev>},
    {"topo_prev_kind", "topo_prev_kind", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::topo_prev_kind>},
    {"prefit_present_prev", "prefit_present_prev", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::prefit_present_prev>},
    {"prefit_code_flag", "prefit_code_flag", Persist::ColdStart, "", "", nullptr, nullptr,
     &losRows<&L::prefit_code_flag>},
    {"redun_event_last", "redun_event_last", Persist::ColdStart, "", "", nullptr, nullptr,
     &losEventRows<&LE::redun_event_last>},
    {"mdb_event_last", "mdb_event_last", Persist::ColdStart, "", "", nullptr, nullptr,
     &losEventRows<&LE::mdb_event_last>},
    {"prires_event_last", "prires_event_last", Persist::ColdStart, "", "", nullptr, nullptr,
     &losEventRows<&LE::prires_event_last>},
    {"prefit_event_last", "prefit_event_last", Persist::ColdStart, "", "", nullptr, nullptr,
     &losEventRows<&LE::prefit_event_last>},
};
const std::vector<FieldDecl> kArcFields = {};

const std::array<EntityFields, 6> kEntityFieldTables = {{
    {"constellation", &kConstellationFields},
    {"satellite",     &kSatelliteFields},
    {"sat_signal",    &kSatSignalFields},
    {"station",       &kStationFields},
    {"los",           &kLosFields},
    {"arc",           &kArcFields},
}};

const std::vector<const char*> kRestoreOrder = {"V", "C", "J", "K", "Z", "PA", "PT", "EQD", "EQP", "EQE",
                                                  "CBP", "LB", "RC", "SD", "Y", "R", "A", "GG"};

// ===========================================================================
// Record lookup
// ===========================================================================

const FieldDecl* recordRow(const char* tag) {
    for (const EntityFields& t : kEntityFieldTables)
        for (const FieldDecl& f : *t.fields)
            if (f.persist == Persist::Record && std::string(f.record) == tag) return &f;
    return nullptr;
}

namespace {
// Every Record row declaring the tag, in table order (one tag may span rows: V holds both
// pivot maps); throws when none does or a row lacks a fill function.
std::vector<const FieldDecl*> recordRowsOrThrow(const char* tag) {
    std::vector<const FieldDecl*> rows;
    bool complete = true;
    for (const EntityFields& t : kEntityFieldTables)
        for (const FieldDecl& f : *t.fields)
            if (f.persist == Persist::Record && std::string(f.record) == tag) {
                complete = complete && f.to_snapshot != nullptr && f.from_snapshot != nullptr;
                rows.push_back(&f);
            }
    if (rows.empty() || !complete)
        throw std::logic_error(std::string("NetworkEntities: no Record row declares tag ") + tag);
    return rows;
}
}  // namespace

void recordToSnapshot(const char* tag, const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    for (const FieldDecl* f : recordRowsOrThrow(tag)) f->to_snapshot(ctx, snap);
}

void recordFromSnapshot(const char* tag, const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    for (const FieldDecl* f : recordRowsOrThrow(tag)) f->from_snapshot(snap, ctx);
}

// ===========================================================================
// The reach table
// ===========================================================================

namespace {
std::size_t declaredFieldCount() {
    std::size_t n = 0;
    for (const EntityFields& t : kEntityFieldTables) n += t.fields->size();
    return n;
}

/// The number of ordered pairs of roster names where one is a proper prefix of the other.
long long namePrefixPairs(const StationNames& names) {
    long long n = 0;
    for (const std::string& a : names.names)
        for (const std::string& b : names.names)
            if (a.size() < b.size() && b.compare(0, a.size(), a) == 0) ++n;
    return n;
}
}  // namespace

void networkEntitiesReachSample(NetworkRun& run, const NetworkEpoch& /*ep*/) {
    const NetworkEntities& e = run.ctx.entities;
    EntityReach& r = e.reach;
    if (r.max_rows.size() != declaredFieldCount()) r.max_rows.assign(declaredFieldCount(), 0);
    std::size_t k = 0;
    for (const EntityFields& t : kEntityFieldTables)
        for (const FieldDecl& f : *t.fields) {
            r.max_rows[k] = std::max(r.max_rows[k], f.rows(e));
            ++k;
        }
    ++r.sampled;
}

int networkEntitiesRunEnd(const NetworkRunEndView& v) {
    const NetworkEntities& e = v.ctx.entities;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const EntityReach& r = e.reach;
    log->info("Network (entities as carried): {} declared field(s) in {} entity table(s), "
              "maximum rows sampled at {} kept epoch(s)",
              declaredFieldCount(), kEntityFieldTables.size(), r.sampled);
    std::size_t k = 0;
    for (const EntityFields& t : kEntityFieldTables)
        for (const FieldDecl& f : *t.fields) {
            const std::size_t fin = f.rows(e);
            const std::size_t mx = std::max(fin, k < r.max_rows.size() ? r.max_rows[k] : 0);
            log->info("Network (entities as carried): {}.{} (was {}) {}{}{} rows max {} final {}",
                      t.entity, f.name, f.was, persistName(f.persist),
                      f.persist == Persist::Record ? " " : "", f.record, mx, fin);
            ++k;
        }
    for (std::size_t c = 0; c < static_cast<std::size_t>(Converter::Count); ++c)
        log->info("Network (entities as carried): converter {} round-trip failures {}",
                  converterName(static_cast<Converter>(c)), r.failures[c]);
    log->info("Network (entities as carried): station names {} in roster, {} prefix pair(s)",
              e.names.names.size(), namePrefixPairs(e.names));
    return 0;
}

}  // namespace GnssProcessor::net
