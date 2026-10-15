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

const std::vector<FieldDecl> kConstellationFields = {};
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

const std::vector<const char*> kRestoreOrder = {"Z", "EQP", "RC"};

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
const FieldDecl& recordRowOrThrow(const char* tag) {
    const FieldDecl* f = recordRow(tag);
    if (f == nullptr || f->to_snapshot == nullptr || f->from_snapshot == nullptr)
        throw std::logic_error(std::string("NetworkEntities: no Record row declares tag ") + tag);
    return *f;
}
}  // namespace

void recordToSnapshot(const char* tag, const NetworkRunContext& ctx, NetworkStateSnapshot& snap) {
    recordRowOrThrow(tag).to_snapshot(ctx, snap);
}

void recordFromSnapshot(const char* tag, const NetworkStateSnapshot& snap, NetworkRunContext& ctx) {
    recordRowOrThrow(tag).from_snapshot(snap, ctx);
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
