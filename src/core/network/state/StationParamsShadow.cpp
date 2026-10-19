/**
 * @file StationParamsShadow.cpp
 * @brief The station parameter set built from its state manager, the checks against the
 *        state manager and the run-end table (see network/state/StationParamsShadow.hpp).
 */

#include "network/state/StationParamsShadow.hpp"

#include "filter/StateManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StateColumns.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace GnssProcessor {

namespace {

/// Equality of two doubles in every bit (a NaN equals the same NaN).
bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

/// Collects the findings of one check into the station's tally.
class Findings {
public:
    Findings(StationParamsCheck& out, double tow) : out_(out), tow_(tow) {}

    /// Count a finding; @p detail is called only for the first finding of the run.
    template <class Detail>
    void note(SetMismatch kind, Detail detail) {
        ++out_.mismatch[static_cast<std::size_t>(kind)];
        if (out_.first.empty())
            out_.first = fmt::format("TOW {:.1f}: {}: {}", tow_, setMismatchName(kind), detail());
    }

private:
    StationParamsCheck& out_;
    double              tow_;
};

/// A typed map against the state manager's map of its family: the same keys in the same order
/// at the same columns.
template <class Typed>
void checkMap(const char* family, const std::map<std::string, std::size_t>& text, const Typed& typed,
              const std::vector<LocalSlot>& slots, Findings& f,
              SetMismatch count_kind = SetMismatch::TypedCount,
              SetMismatch order_kind = SetMismatch::TypedOrder) {
    if (text.size() != typed.size()) {
        f.note(count_kind, [&] {
            return fmt::format("{}: {} keys in the state manager, {} typed", family, text.size(),
                               typed.size());
        });
        return;
    }
    auto t = text.begin();
    for (const auto& entry : typed) {
        const int index = entry.second;
        const bool in_range = index >= 0 && static_cast<std::size_t>(index) < slots.size();
        const std::string want = in_range ? keyText(slots[static_cast<std::size_t>(index)].ref) : std::string();
        if (t->first != want || t->second != static_cast<std::size_t>(index)) {
            f.note(order_kind, [&] {
                return fmt::format("{}: the state manager has '{}' at column {}, the typed map has "
                                   "'{}' at slot {}",
                                   family, t->first, t->second, want, index);
            });
            return;
        }
        ++t;
    }
}

/// The inventory recorded from the set against the one the state manager alone gives: every
/// record, the unclassified list and the two counters.
void checkInventory(const StationParamSlot& got, const StationParamSlot& want, Findings& f) {
    if (got.filled != want.filled || got.unclassified != want.unclassified
        || got.no_disposition != want.no_disposition
        || got.absorbed_no_absorber != want.absorbed_no_absorber) {
        f.note(SetMismatch::Inventory, [&] {
            return fmt::format("the counters differ: {} unclassified, {} without a disposition, {} "
                               "without an absorber against the state manager's {}, {}, {}",
                               got.unclassified.size(), got.no_disposition, got.absorbed_no_absorber,
                               want.unclassified.size(), want.no_disposition, want.absorbed_no_absorber);
        });
    }
    if (got.records.size() != want.records.size()) {
        f.note(SetMismatch::Inventory, [&] {
            return fmt::format("{} records against the state manager's {}", got.records.size(),
                               want.records.size());
        });
        return;
    }
    for (std::size_t k = 0; k < got.records.size(); ++k) {
        const StationParamRecord& g = got.records[k];
        const StationParamRecord& w = want.records[k];
        if (!(g.id == w.id) || g.disp != w.disp || g.reason != w.reason) {
            f.note(SetMismatch::Inventory, [&] {
                return fmt::format("record {} differs from the state manager's ({} against {})", k,
                                   paramDispositionName(g.disp), paramDispositionName(w.disp));
            });
        }
    }
}

/// One family of the table: what it is, where the set holds it and the order of its key.
struct Family {
    const char* label;
    const char* held_in;
    const char* key_order;
};

/// The families in print order; @ref familyOf indexes it.
constexpr Family kFamilies[] = {
    {"receiver position",      "position[3]",             "axis"},
    {"receiver clock",         "clock",                   "none"},
    {"zenith wet delay",       "zwd",                     "none"},
    {"inter-system bias",      "isb",                     "constellation name"},
    {"slant ionosphere",       "iono",                    "satellite"},
    {"phase colour",           "colour",                  "satellite"},
    {"ambiguity",              "amb",                     "satellite, signal"},
    {"receiver phase bias",    "rx_pb",                   "constellation name, signal"},
    {"receiver code bias",     "rx_cb",                   "name, form, band or signal, slot text"},
    {"satellite clock",        "shared.sat_clock",        "satellite"},
    {"satellite phase bias",   "shared.sat_phase_bias",   "satellite, signal"},
    {"satellite code bias",    "shared.sat_code_bias",    "satellite, signal"},
    {"no typed map",           "slots only",              "-"},
};
constexpr std::size_t kFamilyCount = sizeof(kFamilies) / sizeof(kFamilies[0]);
constexpr std::size_t kUntypedFamily = kFamilyCount - 1;

/// The row of @ref kFamilies a slot belongs to.
std::size_t familyOf(const LocalSlot& s) {
    using PK = NetworkParamKind;
    if (s.unnamed) return kUntypedFamily;
    const ParamRef& r = s.ref;
    switch (r.kind) {
        case PK::RxPosition:   return 0;
        case PK::RxClock:
            if (r.core != UserCore::None) return kUntypedFamily;
            return r.form == ParamForm::Isb ? 3 : 1;
        case PK::Zwd:          return r.core == UserCore::None ? 2 : kUntypedFamily;
        case PK::SlantIono:    return 4;
        case PK::PhaseColour:  return 5;
        case PK::Ambiguity:    return r.form == ParamForm::None ? 6 : kUntypedFamily;
        case PK::RxPhaseBias:  return 7;
        case PK::RxCodeBias:   return 8;
        case PK::SatClock:     return 9;
        case PK::SatPhaseBias: return 10;
        case PK::SatCodeBias:  return r.form == ParamForm::None ? 11 : kUntypedFamily;
        case PK::VtecSh:       break;
    }
    return kUntypedFamily;
}

}  // namespace

const char* setMismatchName(SetMismatch m) {
    switch (m) {
        case SetMismatch::Size:        return "size";
        case SetMismatch::Identity:    return "identity";
        case SetMismatch::XLin:        return "x_lin";
        case SetMismatch::Disposition: return "disposition";
        case SetMismatch::Reseed:      return "reseed";
        case SetMismatch::Elevation:   return "elevation";
        case SetMismatch::Lookup:      return "lookup";
        case SetMismatch::Inventory:   return "inventory";
        case SetMismatch::TypedCount:  return "typed count";
        case SetMismatch::TypedOrder:  return "typed order";
        case SetMismatch::Stale:       return "stale";
    }
    return "?";
}

long long StationParamsCheck::mismatches() const {
    long long total = 0;
    for (const long long n : mismatch) total += n;
    return total;
}

void buildStationParams(const StateManager& sm, const std::vector<ParamDisposition>& col_disp,
                        const StationAbsorbed& absorbed, StationParams& out) {
    StationParams set;   // built apart, so that a throw leaves `out` as it was
    const std::vector<StateColumn> columns = listStateColumns(sm);
    const Eigen::VectorXd x = sm.getStateVector();
    set.slots.reserve(columns.size());
    set.decided = std::min(col_disp.size(), columns.size());
    for (const StateColumn& col : columns) {
        LocalSlot slot;
        slot.x_lin = x(static_cast<Eigen::Index>(col.index));
        if (col.index < col_disp.size()) slot.disp = col_disp[col.index];
        if (const auto id = classifyStateColumn(col.container, col.key)) {
            std::uint8_t axis = 0;
            if (col.container == StateContainer::PositionIndex)
                axis = static_cast<std::uint8_t>(col.index - sm.getPositionIndex());
            slot.ref = paramRefOf(*id, axis);
            slot.pending_reseed = col.container == StateContainer::Ambiguities
                               && sm.isAmbiguityPendingReseed(col.key);
            if (col.container == StateContainer::PhaseColourStates)
                slot.elevation_deg = sm.getPhaseColourElevation(col.key);
        } else {
            slot.unnamed = true;
            set.unclassified.push_back(std::string(stateContainerName(col.container)) + ":" + col.key);
        }
        addParam(set, slot);
    }
    std::vector<std::string> absorbed_unclassified;
    for (const AbsorbedNote& note : absorbed.notes) {
        const auto a = classifyAbsorbed(note);
        if (!a) {
            absorbed_unclassified.push_back("absorbed " + absorbedNoteLabel(note));
            continue;
        }
        if (!absorberPresent(sm, *a)) {
            ++set.shared.absorbed_no_absorber;
            continue;
        }
        set.shared.absorbed.push_back(AbsorbedParam{paramRefOf(a->id), a->reason});
    }
    for (std::string& label : absorbed_unclassified) set.unclassified.push_back(std::move(label));
    out = std::move(set);
}

void recordStationParamsFromState(const StateManager& sm, int station,
                                  const std::vector<ParamDisposition>& col_disp,
                                  const StationAbsorbed& absorbed, StationParamSlot& slot) {
    slot.records.clear();
    slot.unclassified.clear();
    slot.no_disposition       = 0;
    slot.absorbed_no_absorber = 0;
    slot.filled = true;
    // The columns of the station's state manager, with the split's dispositions.
    for (const StateColumn& col : listStateColumns(sm)) {
        const auto id = classifyStateColumn(col.container, col.key);
        if (!id) {
            slot.unclassified.push_back(std::string(stateContainerName(col.container)) + ":" + col.key);
            continue;
        }
        if (col.index >= col_disp.size()) {   // the split did not decide this column
            ++slot.no_disposition;
            continue;
        }
        StationParamRecord rec;
        rec.id         = *id;
        rec.id.station = StationId{station};
        rec.disp       = col_disp[col.index];
        slot.records.push_back(rec);
    }
    // The shared states the gates withheld from it (no column), each absorbed by a column
    // the station still holds; one whose observation left before the split has nothing
    // to absorb it and is counted apart.  A note whose key the classifier refuses is
    // listed unclassified after the columns' own, in note order.
    std::vector<std::string> absorbed_unclassified;
    for (const AbsorbedNote& note : absorbed.notes) {
        const auto a = classifyAbsorbed(note);
        if (!a) {
            absorbed_unclassified.push_back("absorbed " + absorbedNoteLabel(note));
            continue;
        }
        if (!absorberPresent(sm, *a)) {
            ++slot.absorbed_no_absorber;
            continue;
        }
        StationParamRecord rec;
        rec.id         = a->id;
        rec.id.station = StationId{station};
        rec.disp       = ParamDisposition::Absorbed;
        rec.reason     = a->reason;
        slot.records.push_back(rec);
    }
    for (std::string& label : absorbed_unclassified)
        slot.unclassified.push_back(std::move(label));
}

void checkStationParams(const StationParams& set, const StateManager& sm,
                        const std::vector<ParamDisposition>& col_disp, const StationAbsorbed& absorbed,
                        const StationParamSlot& inventory, int station, double tow,
                        StationParamsCheck& out) {
    Findings f(out, tow);
    const std::size_t n = sm.getStateSize();
    if (set.slots.size() != n) {
        f.note(SetMismatch::Size, [&] {
            return fmt::format("{} slots, {} state columns", set.slots.size(), n);
        });
        return;
    }
    out.slots += static_cast<long long>(n);

    const Eigen::VectorXd x = sm.getStateVector();
    const UserParamView view(sm, x, n);
    for (std::size_t k = 0; k < n; ++k) {
        const LocalSlot& s = set.slots[k];
        if (s.unnamed) {
            // No identity to resolve: the slot still carries its column's value.
            if (!sameBits(x(static_cast<Eigen::Index>(k)), s.x_lin)) {
                f.note(SetMismatch::XLin, [&] {
                    return fmt::format("unnamed slot {}: {:.17g} against the state's {:.17g}", k,
                                       s.x_lin, x(static_cast<Eigen::Index>(k)));
                });
            }
        } else {
            const ParamSlot at = view.find(s.ref);
            if (at.column != static_cast<int>(k)) {
                f.note(SetMismatch::Identity, [&] {
                    return fmt::format("slot {} ({}) resolves to column {}", k, describe(s.ref), at.column);
                });
            } else if (!sameBits(at.x, s.x_lin)) {
                f.note(SetMismatch::XLin, [&] {
                    return fmt::format("slot {} ({}): {:.17g} against the state's {:.17g}", k,
                                       describe(s.ref), s.x_lin, at.x);
                });
            }
            const bool reseed = s.ref.kind == NetworkParamKind::Ambiguity
                             && sm.isAmbiguityPendingReseed(userKey(s.ref));
            if (s.pending_reseed != reseed) {
                f.note(SetMismatch::Reseed, [&] {
                    return fmt::format("slot {} ({}): mark {} against the state manager's {}", k,
                                       describe(s.ref), s.pending_reseed, reseed);
                });
            }
            if (s.ref.kind == NetworkParamKind::PhaseColour
                && !sameBits(s.elevation_deg, sm.getPhaseColourElevation(userKey(s.ref)))) {
                f.note(SetMismatch::Elevation, [&] {
                    return fmt::format("slot {} ({}): {:.17g} against the state manager's {:.17g}", k,
                                       describe(s.ref), s.elevation_deg,
                                       sm.getPhaseColourElevation(userKey(s.ref)));
                });
            }
            if (findParam(set, s.ref) != static_cast<int>(k)) {
                f.note(SetMismatch::Lookup, [&] {
                    return fmt::format("slot {} ({}) is found at slot {}", k, describe(s.ref),
                                       findParam(set, s.ref));
                });
            }
        }
        const ParamDisposition want = k < col_disp.size() ? col_disp[k] : ParamDisposition::Unobserved;
        if (s.disp != want) {
            f.note(SetMismatch::Disposition, [&] {
                return fmt::format("slot {}: {} against the split's {}", k,
                                   paramDispositionName(s.disp), paramDispositionName(want));
            });
        }
    }
    StationParamSlot reference;
    recordStationParamsFromState(sm, station, col_disp, absorbed, reference);
    checkInventory(inventory, reference, f);
    checkMap("inter-system bias", sm.getSystemTimeOffsetStates(), set.isb, set.slots, f);
    checkMap("slant ionosphere", sm.getIonosphereStates(), set.iono, set.slots, f);
    checkMap("phase colour", sm.getPhaseColourStates(), set.colour, set.slots, f);
    checkMap("ambiguity", sm.getAmbiguities(), set.amb, set.slots, f);
    checkMap("receiver phase bias", sm.getReceiverPhaseBiasStates(), set.rx_pb, set.slots, f);
    checkMap("receiver code bias", sm.getCodeBiasStates(), set.rx_cb, set.slots, f);
    checkMap("satellite clock", sm.getSatClockStates(), set.shared.sat_clock, set.slots, f);
    checkMap("satellite phase bias", sm.getSatPhaseBiasStates(), set.shared.sat_phase_bias, set.slots, f);
    checkMap("satellite code bias", sm.getSatBiasStates(), set.shared.sat_code_bias, set.slots, f);
}

void networkShadowStationParams(NetStation& st, int station,
                                const std::vector<ParamDisposition>& col_disp,
                                StationParamSlot& inventory, double tow) {
    const auto started = std::chrono::steady_clock::now();
    const auto tally_wall = [&] {
        st.params_check.wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started).count();
    };
    const StationAbsorbed& absorbed = st.mm->absorbedSharedStates();
    try {
        buildStationParams(*st.sm, col_disp, absorbed, st.params);
        recordStationParams(st.params, station, inventory);
    } catch (...) {
        // The carried-prior steps read the set, so an empty one must not reach them: the
        // station's assembly fails for the epoch (the produce loop records and reports it).
        ++st.params_check.failed;
        st.params = StationParams{};
        tally_wall();
        throw;
    }
    ++st.params_check.builds;
    if (!st.params.unclassified.empty()) ++st.params_check.unclassified;
    try {
        checkStationParams(st.params, *st.sm, col_disp, absorbed, inventory, station, tow,
                           st.params_check);
    } catch (...) {
        ++st.params_check.failed;
    }
    tally_wall();
}

namespace {

/// Rebuild a station's set from its state manager with no dispositions; false (counted, set
/// left empty) when the build throws.
bool rebuildWithoutDispositions(NetStation& st) {
    static const StationAbsorbed none;
    try {
        buildStationParams(*st.sm, {}, st.mm ? st.mm->absorbedSharedStates() : none, st.params);
        return true;
    } catch (...) {
        ++st.params_check.failed;
        st.params = StationParams{};
        return false;
    }
}

}  // namespace

void networkStartStationParams(NetStation& st) { rebuildWithoutDispositions(st); }

void networkRefreshStationParams(NetStation& st) {
    if (rebuildWithoutDispositions(st)) ++st.params_check.refreshed;
}

void networkCheckStationParamsFresh(NetStation& st, double tow) {
    if (!st.sm) return;
    try {
        const StateManager& sm = *st.sm;
        const StationParams& set = st.params;
        Findings f(st.params_check, tow);
        const auto stale = [&](const char* family, const auto& text, const auto& typed) {
            checkMap(family, text, typed, set.slots, f, SetMismatch::Stale, SetMismatch::Stale);
        };
        stale("inter-system bias", sm.getSystemTimeOffsetStates(), set.isb);
        stale("slant ionosphere", sm.getIonosphereStates(), set.iono);
        stale("phase colour", sm.getPhaseColourStates(), set.colour);
        stale("ambiguity", sm.getAmbiguities(), set.amb);
        stale("receiver phase bias", sm.getReceiverPhaseBiasStates(), set.rx_pb);
        stale("receiver code bias", sm.getCodeBiasStates(), set.rx_cb);
        const bool has_zwd = sm.getTropoIndex() != std::numeric_limits<std::size_t>::max();
        if (has_zwd != (set.zwd >= 0)) {
            f.note(SetMismatch::Stale, [&] {
                return std::string("wet delay: the state manager and the set disagree on holding it");
            });
        }
        // The values a reader takes from the set are the state's, bit for bit, for every local
        // column (the border columns are rebuilt by the admissions).
        const Eigen::VectorXd x = sm.getStateVector();
        if (static_cast<std::size_t>(x.size()) == set.slots.size()) {
            for (std::size_t k = 0; k < set.slots.size(); ++k) {
                const LocalSlot& s = set.slots[k];
                if (!s.unnamed && (s.ref.kind == NetworkParamKind::SatClock
                                   || s.ref.kind == NetworkParamKind::SatPhaseBias
                                   || s.ref.kind == NetworkParamKind::SatCodeBias))
                    continue;
                if (!sameBits(x(static_cast<Eigen::Index>(k)), s.x_lin)) {
                    f.note(SetMismatch::Stale, [&] {
                        return fmt::format("slot {}: {:.17g} against the state's {:.17g}", k, s.x_lin,
                                           x(static_cast<Eigen::Index>(k)));
                    });
                    break;
                }
            }
        }
        ++st.params_check.fresh_checks;
    } catch (...) {
        ++st.params_check.failed;
    }
}

std::vector<std::string> stationParamsReport(const std::vector<NetStation>& stations) {
    std::vector<std::string> lines;
    StationParamsCheck total;
    std::array<long long, kFamilyCount> slots{};
    std::array<long long, kFamilyCount> held_by{};
    long long built_stations = 0;
    for (const NetStation& st : stations) {
        const StationParamsCheck& c = st.params_check;
        total.builds += c.builds;
        total.refreshed += c.refreshed;
        total.fresh_checks += c.fresh_checks;
        total.unclassified += c.unclassified;
        total.failed += c.failed;
        total.slots += c.slots;
        total.wall_ns += c.wall_ns;
        for (std::size_t m = 0; m < kSetMismatches; ++m) total.mismatch[m] += c.mismatch[m];
        if (c.builds == 0) continue;
        ++built_stations;
        std::array<long long, kFamilyCount> mine{};
        for (const LocalSlot& s : st.params.slots) ++mine[familyOf(s)];
        for (std::size_t i = 0; i < kFamilyCount; ++i) {
            slots[i] += mine[i];
            if (mine[i] > 0) ++held_by[i];
        }
    }

    lines.push_back("shadow of the stations' state managers, rebuilt and checked after each station's "
                    "split; the table is each station's last build");
    lines.push_back(fmt::format("{} stations built, {} station-epochs, {} slots checked",
                                built_stations, total.builds, total.slots));
    lines.push_back(fmt::format("{:<22} {:<22} {:<38} {:>8} {:>9}", "parameter", "held in", "key order",
                                "slots", "stations"));
    for (std::size_t i = 0; i < kFamilyCount; ++i) {
        if (i == kUntypedFamily && slots[i] == 0) continue;
        lines.push_back(fmt::format("{:<22} {:<22} {:<38} {:>8} {:>9}", kFamilies[i].label,
                                    kFamilies[i].held_in, kFamilies[i].key_order, slots[i], held_by[i]));
    }
    std::string findings;
    for (std::size_t m = 0; m < kSetMismatches; ++m) {
        if (m > 0) findings += ", ";
        findings += fmt::format("{} {}", setMismatchName(static_cast<SetMismatch>(m)), total.mismatch[m]);
    }
    lines.push_back("checks against the state managers, findings: " + findings);
    lines.push_back(fmt::format("station-epochs with a column or a withheld state the classifier "
                                "refused: {}; with a build or check error (no set): {}",
                                total.unclassified, total.failed));
    lines.push_back(fmt::format("station-epochs whose assembly ended before the split (set rebuilt "
                                "without dispositions): {}; epoch starts checked for staleness: {}",
                                total.refreshed, total.fresh_checks));
    const long long attempts = total.builds + total.failed;
    lines.push_back(fmt::format("wall of the builds and checks, summed over the station-epochs: "
                                "{:.1f} ms ({:.1f} us per station-epoch)",
                                static_cast<double>(total.wall_ns) * 1e-6,
                                attempts > 0 ? static_cast<double>(total.wall_ns) * 1e-3
                                                   / static_cast<double>(attempts)
                                             : 0.0));
    int shown = 0;
    for (const NetStation& st : stations) {
        if (st.params_check.first.empty()) continue;
        if (shown++ >= 5) break;
        lines.push_back(fmt::format("WARNING: station {}: {}", st.name, st.params_check.first));
    }
    return lines;
}

} // namespace GnssProcessor
