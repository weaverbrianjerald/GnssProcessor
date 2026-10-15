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
const std::vector<FieldDecl> kSatelliteFields = {};
const std::vector<FieldDecl> kSatSignalFields = {};
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
const std::vector<FieldDecl> kLosFields = {};
const std::vector<FieldDecl> kArcFields = {};

const std::array<EntityFields, 6> kEntityFieldTables = {{
    {"constellation", &kConstellationFields},
    {"satellite",     &kSatelliteFields},
    {"sat_signal",    &kSatSignalFields},
    {"station",       &kStationFields},
    {"los",           &kLosFields},
    {"arc",           &kArcFields},
}};

const std::vector<const char*> kRestoreOrder = {"V", "Z", "EQD", "EQP", "CBP", "RC", "GG"};

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
