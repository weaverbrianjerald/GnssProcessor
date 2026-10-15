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
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "app/NetworkRun.hpp"
#include "app/NetworkRunContext.hpp"
#include "filter/NetworkRunEnd.hpp"
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
const std::vector<FieldDecl> kStationFields = {};
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

const std::vector<const char*> kRestoreOrder = {};

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
