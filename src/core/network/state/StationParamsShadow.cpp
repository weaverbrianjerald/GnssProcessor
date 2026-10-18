/**
 * @file StationParamsShadow.cpp
 * @brief The station parameter set built from its state manager, the checks against the
 *        state manager and the run-end table (see network/state/StationParamsShadow.hpp).
 */

#include "network/state/StationParamsShadow.hpp"

#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "measurement/UserParamView.hpp"
#include "network/driver/RunTypes.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <set>

namespace GnssProcessor {

namespace {

namespace K = StateKeys;

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

/// The key a slot's parameter has in the state manager's map of its family.
std::string keyOfSlot(const LocalSlot& s) {
    if (s.ref.kind == NetworkParamKind::RxClock && s.ref.form == ParamForm::Isb)
        return K::makeSystemTimeOffsetKey(systemName(s.ref.sys));
    return userKey(s.ref);
}

/// A typed map against the state manager's map of its family: the same keys in the same order
/// at the same columns.
template <class Typed>
void checkMap(const char* family, const std::map<std::string, std::size_t>& text, const Typed& typed,
              const std::vector<LocalSlot>& slots, Findings& f) {
    if (text.size() != typed.size()) {
        f.note(SetMismatch::TypedCount, [&] {
            return fmt::format("{}: {} keys in the state manager, {} typed", family, text.size(),
                               typed.size());
        });
        return;
    }
    auto t = text.begin();
    for (const auto& entry : typed) {
        const int index = entry.second;
        const bool in_range = index >= 0 && static_cast<std::size_t>(index) < slots.size();
        const std::string want = in_range ? keyOfSlot(slots[static_cast<std::size_t>(index)]) : std::string();
        if (t->first != want || t->second != static_cast<std::size_t>(index)) {
            f.note(SetMismatch::TypedOrder, [&] {
                return fmt::format("{}: the state manager has '{}' at column {}, the typed map has "
                                   "'{}' at slot {}",
                                   family, t->first, t->second, want, index);
            });
            return;
        }
        ++t;
    }
}

/// The inventory's record of each column and of each withheld state against the set's slots.
void checkInventory(const StationParams& set, const StationParamSlot& inv, int station, Findings& f) {
    const std::size_t n = set.slots.size();
    const std::size_t absorbed = set.shared.absorbed.size();
    if (inv.records.size() != n + absorbed) {
        f.note(SetMismatch::Inventory, [&] {
            return fmt::format("the inventory has {} records, the set {} slots and {} withheld",
                               inv.records.size(), n, absorbed);
        });
        return;
    }
    const StationId id{station};
    for (std::size_t k = 0; k < n; ++k) {
        const StationParamRecord& rec = inv.records[k];
        const LocalSlot&          s   = set.slots[k];
        if (!(rec.id == paramInstanceOf(s.ref, id)) || rec.disp != s.disp || rec.reason.has_value()) {
            f.note(SetMismatch::Inventory, [&] {
                return fmt::format("record {} is not the set's slot ({})", k, describe(s.ref));
            });
        }
    }
    for (std::size_t j = 0; j < absorbed; ++j) {
        const StationParamRecord& rec = inv.records[n + j];
        const AbsorbedParam&      a   = set.shared.absorbed[j];
        if (!(rec.id == paramInstanceOf(a.ref, id)) || rec.disp != ParamDisposition::Absorbed
            || !rec.reason.has_value() || *rec.reason != a.reason) {
            f.note(SetMismatch::Inventory, [&] {
                return fmt::format("record {} is not the withheld parameter ({})", n + j,
                                   describe(a.ref));
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

/// The row of @ref kFamilies a parameter belongs to.
std::size_t familyOf(const ParamRef& r) {
    using PK = NetworkParamKind;
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

} // namespace

const char* setMismatchName(SetMismatch m) {
    switch (m) {
        case SetMismatch::Size:        return "size";
        case SetMismatch::Identity:    return "identity";
        case SetMismatch::XLin:        return "x_lin";
        case SetMismatch::Disposition: return "disposition";
        case SetMismatch::Reseed:      return "reseed";
        case SetMismatch::Lookup:      return "lookup";
        case SetMismatch::Inventory:   return "inventory";
        case SetMismatch::TypedCount:  return "typed count";
        case SetMismatch::TypedOrder:  return "typed order";
    }
    return "?";
}

long long StationParamsCheck::mismatches() const {
    long long total = 0;
    for (const long long n : mismatch) total += n;
    return total;
}

ShadowBuild buildStationParams(const StateManager& sm, const std::vector<ParamDisposition>& col_disp,
                               const StationAbsorbed& absorbed, StationParams& out) {
    out = StationParams{};
    ShadowBuild result;
    const std::vector<StateColumn> columns = listStateColumns(sm);
    std::vector<ParamInstanceId> ids;
    ids.reserve(columns.size());
    for (const StateColumn& col : columns) {
        const auto id = classifyStateColumn(col.container, col.key);
        if (!id) {
            result.complete = false;
            result.unclassified.push_back(std::string(stateContainerName(col.container)) + ":" + col.key);
            continue;
        }
        ids.push_back(*id);
    }
    if (!result.complete) return result;

    const Eigen::VectorXd x = sm.getStateVector();
    out.slots.reserve(columns.size());
    for (std::size_t k = 0; k < columns.size(); ++k) {
        const StateColumn& col = columns[k];
        std::uint8_t axis = 0;
        if (col.container == StateContainer::PositionIndex)
            axis = static_cast<std::uint8_t>(col.index - sm.getPositionIndex());
        LocalSlot slot;
        slot.ref = paramRefOf(ids[k], axis);
        slot.x_lin = x(static_cast<Eigen::Index>(col.index));
        if (col.index < col_disp.size()) slot.disp = col_disp[col.index];
        else                             ++result.no_disposition;
        slot.pending_reseed = col.container == StateContainer::Ambiguities
                           && sm.isAmbiguityPendingReseed(col.key);
        addParam(out, slot);
    }
    for (const AbsorbedNote& note : absorbed.notes) {
        const auto a = classifyAbsorbed(note);
        if (!a || !absorberPresent(sm, *a)) continue;
        out.shared.absorbed.push_back(AbsorbedParam{paramRefOf(a->id), a->reason});
    }
    return result;
}

void checkStationParams(const StationParams& set, const StateManager& sm,
                        const std::vector<ParamDisposition>& col_disp,
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
        const ParamDisposition want = k < col_disp.size() ? col_disp[k] : ParamDisposition::Unobserved;
        if (s.disp != want) {
            f.note(SetMismatch::Disposition, [&] {
                return fmt::format("slot {} ({}): {} against the split's {}", k, describe(s.ref),
                                   paramDispositionName(s.disp), paramDispositionName(want));
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
        if (findParam(set, s.ref) != static_cast<int>(k)) {
            f.note(SetMismatch::Lookup, [&] {
                return fmt::format("slot {} ({}) is found at slot {}", k, describe(s.ref),
                                   findParam(set, s.ref));
            });
        }
    }
    checkInventory(set, inventory, station, f);
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
                                const StationParamSlot& inventory, double tow) {
    const auto started = std::chrono::steady_clock::now();
    try {
        const ShadowBuild built =
            buildStationParams(*st.sm, col_disp, st.mm->absorbedSharedStates(), st.params);
        if (built.complete) {
            ++st.params_check.builds;
            checkStationParams(st.params, *st.sm, col_disp, inventory, station, tow, st.params_check);
        } else {
            ++st.params_check.incomplete;
        }
    } catch (const std::exception&) {
        ++st.params_check.failed;
    } catch (...) {
        ++st.params_check.failed;
    }
    st.params_check.wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - started).count();
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
        total.incomplete += c.incomplete;
        total.failed += c.failed;
        total.slots += c.slots;
        total.wall_ns += c.wall_ns;
        for (std::size_t m = 0; m < kSetMismatches; ++m) total.mismatch[m] += c.mismatch[m];
        if (c.builds == 0) continue;
        ++built_stations;
        std::array<long long, kFamilyCount> mine{};
        for (const LocalSlot& s : st.params.slots) ++mine[familyOf(s.ref)];
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
    lines.push_back(fmt::format("station-epochs with no set: {} with a column the classifier refused, "
                                "{} with an error",
                                total.incomplete, total.failed));
    const long long attempts = total.builds + total.incomplete + total.failed;
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
