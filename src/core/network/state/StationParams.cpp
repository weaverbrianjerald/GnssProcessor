/**
 * @file StationParams.cpp
 * @brief Registration and lookup of the parameters of a station's set (see
 *        network/state/StationParams.hpp).
 */

#include "network/state/StationParams.hpp"

#include "config/GnssConfig.hpp"
#include "filter/StateFamily.hpp"          // kStateFamilies: the label prefixes
#include "filter/StateKeySchema.hpp"       // the receiver code-bias key parsers
#include "network/state/Entities.hpp"      // net::satSignalKey
#include "measurement/UserParamView.hpp"   // userKey: the one rendering of a typed identity as a key

#include <algorithm>
#include <utility>

namespace GnssProcessor {

namespace {

/// Insert a key into a typed map; true when the key was free.
template <class Map, class Key>
bool place(Map& map, const Key& key, int index) {
    return map.emplace(key, index).second;
}

/// Set a single-slot index; true when it was free.
bool placeOnce(int& holder, int index) {
    if (holder >= 0) return false;
    holder = index;
    return true;
}

/// The slot a key maps to, or -1.
template <class Map, class Key>
int slotOf(const Map& map, const Key& key) {
    const auto it = map.find(key);
    return it == map.end() ? -1 : it->second;
}

} // namespace

bool addParam(StationParams& set, const LocalSlot& slot) {
    const int index = static_cast<int>(set.slots.size());
    set.slots.push_back(slot);
    const ParamRef& r = slot.ref;
    bool placed = false;
    using PK = NetworkParamKind;
    switch (r.kind) {
        case PK::RxPosition:
            placed = r.axis < set.position.size() && placeOnce(set.position[r.axis], index);
            break;
        case PK::Zwd:
            placed = r.core == UserCore::None && placeOnce(set.zwd, index);
            break;
        case PK::RxClock:
            if (r.core != UserCore::None) break;
            placed = r.form == ParamForm::Isb ? place(set.isb, r.sys, index)
                                              : placeOnce(set.clock, index);
            break;
        case PK::SlantIono:
            placed = place(set.iono, r.sat, index);
            break;
        case PK::PhaseColour:
            placed = place(set.colour, r.sat, index);
            break;
        case PK::Ambiguity:
            placed = r.form == ParamForm::None && place(set.amb, SatSignalId{r.sat, r.signal}, index);
            break;
        case PK::RxPhaseBias:
            placed = place(set.rx_pb, r.signal, index);
            break;
        case PK::RxCodeBias:
            placed = place(set.rx_cb, rxCbKeyOf(r), index);
            break;
        case PK::SatClock:
            placed = place(set.shared.sat_clock, r.sat, index);
            break;
        case PK::SatPhaseBias:
            placed = place(set.shared.sat_phase_bias, SatSignalId{r.sat, r.signal}, index);
            break;
        case PK::SatCodeBias:
            placed = r.form == ParamForm::None
                  && place(set.shared.sat_code_bias, SatSignalId{r.sat, r.signal}, index);
            break;
        case PK::VtecSh:
            break;
    }
    if (!placed) ++set.untyped;
    return placed;
}

int findParam(const StationParams& set, const ParamRef& r) {
    using PK = NetworkParamKind;
    switch (r.kind) {
        case PK::RxPosition:
            return r.axis < set.position.size() ? set.position[r.axis] : -1;
        case PK::Zwd:
            return r.core == UserCore::None ? set.zwd : -1;
        case PK::RxClock:
            if (r.core != UserCore::None) return -1;
            return r.form == ParamForm::Isb ? slotOf(set.isb, r.sys) : set.clock;
        case PK::SlantIono:
            return slotOf(set.iono, r.sat);
        case PK::PhaseColour:
            return slotOf(set.colour, r.sat);
        case PK::Ambiguity:
            return r.form == ParamForm::None ? slotOf(set.amb, SatSignalId{r.sat, r.signal}) : -1;
        case PK::RxPhaseBias:
            return slotOf(set.rx_pb, r.signal);
        case PK::RxCodeBias:
            return slotOf(set.rx_cb, rxCbKeyOf(r));
        case PK::SatClock:
            return slotOf(set.shared.sat_clock, r.sat);
        case PK::SatPhaseBias:
            return slotOf(set.shared.sat_phase_bias, SatSignalId{r.sat, r.signal});
        case PK::SatCodeBias:
            return r.form == ParamForm::None
                       ? slotOf(set.shared.sat_code_bias, SatSignalId{r.sat, r.signal})
                       : -1;
        case PK::VtecSh:
            break;
    }
    return -1;
}

int findAmbiguity(const StationParams& set, const std::string& key) {
    const std::optional<SatSignalId> id = net::satSignalKey(key);
    return id ? slotOf(set.amb, *id) : -1;
}

int findSlantIono(const StationParams& set, const std::string& sat) {
    const std::optional<SatId> id = net::satIdKey(sat);
    return id ? slotOf(set.iono, *id) : -1;
}

std::optional<SignalId> receiverPhaseBiasSignal(const std::string& key) {
    static const std::string infix = "_RPB_";
    const std::size_t pos = key.find(infix);
    if (pos == std::string::npos || pos == 0 || pos + infix.size() >= key.size()) return std::nullopt;
    const System sys = systemFromName(key.substr(0, pos));
    if (sys == System::Unknown) return std::nullopt;
    return SignalId::fromObsCode(sys, key.substr(pos + infix.size()));
}

int findReceiverPhaseBias(const StationParams& set, const std::string& key) {
    const std::optional<SignalId> signal = receiverPhaseBiasSignal(key);
    return signal ? slotOf(set.rx_pb, *signal) : -1;
}

std::optional<RxCbKey> receiverCodeBiasKey(const std::string& key) {
    namespace K = StateKeys;
    RxCbKey out;
    if (K::isGlonassPerSlotIfbKey(key)) {
        const auto p = K::parseGlonassPerSlotIfbKey(key);
        if (!p) return std::nullopt;
        out.form     = ParamForm::GloSlotIfb;
        out.sys      = systemFromName(p->system);
        out.band     = bandOfDigit(out.sys, p->band);
        out.glo_slot = p->fcn_slot;
        return out;
    }
    if (const auto p = K::parseIfbKey(key)) {
        out.form = ParamForm::Ifb;
        out.sys  = systemFromName(p->system);
        out.band = bandOfDigit(out.sys, p->band);
        return out;
    }
    if (const auto p = K::parseDcbKey(key)) {
        out.form = ParamForm::Dcb;
        out.sys  = systemFromName(*p);
        return out;
    }
    if (const auto p = K::parseReceiverIscKey(key)) {
        out.form = ParamForm::Isc;
        out.sys  = systemFromName(p->system);
        const auto sig = SignalId::fromObsCode(out.sys, p->signal);
        if (!sig) return std::nullopt;   // needs a known system
        out.signal = *sig;
        return out;
    }
    return std::nullopt;
}

int findReceiverCodeBias(const StationParams& set, const std::string& key) {
    for (const auto& entry : set.rx_cb)
        if (keyText(slotAt(set, entry.second).ref) == key) return entry.second;
    return -1;
}

std::string keyText(const ParamRef& r) {
    if (r.kind == NetworkParamKind::RxClock && r.form == ParamForm::Isb && r.core == UserCore::None)
        return systemName(r.sys);
    return userKey(r);
}

std::string satPhaseBiasText(const SatSignalId& id) { return userKey(satPhaseBiasRef(id.sat, id.signal)); }

std::string satCodeBiasText(const SatSignalId& id) { return userKey(satCodeBiasRef(id.sat, id.signal)); }

std::string satClockText(const SatId& sat) { return userKey(satClockRef(sat)); }

namespace {

/// The label prefix of a dynamic state family.
std::string familyPrefix(StateFamily f) { return specOf(f).label_prefix; }

}  // namespace

std::string stateLabel(const LocalSlot& slot, std::size_t index) {
    using PK = NetworkParamKind;
    const ParamRef& r = slot.ref;
    switch (r.kind) {
        case PK::RxPosition:
            return r.axis == 0 ? "POS_X" : (r.axis == 1 ? "POS_Y" : "POS_Z");
        case PK::RxClock:
            if (r.core == UserCore::PhaseClock) return "PHASE_CLK";
            if (r.core != UserCore::None) break;
            return r.form == ParamForm::Isb ? "ISB_" + keyText(r) : std::string("CLK");
        case PK::Zwd:
            if (r.core == UserCore::GradientNorth) return "TGRAD_N";
            if (r.core == UserCore::GradientEast) return "TGRAD_E";
            return "ZWD";
        case PK::SlantIono:    return familyPrefix(StateFamily::Ionosphere) + keyText(r);
        case PK::Ambiguity:    return familyPrefix(StateFamily::Ambiguity) + keyText(r);
        case PK::RxCodeBias:   return familyPrefix(StateFamily::ReceiverCodeBias) + keyText(r);
        case PK::SatCodeBias:  return familyPrefix(StateFamily::SatBias) + keyText(r);
        case PK::SatPhaseBias: return familyPrefix(StateFamily::SatPhaseBias) + keyText(r);
        case PK::RxPhaseBias:  return familyPrefix(StateFamily::ReceiverPhaseBias) + keyText(r);
        case PK::PhaseColour:  return familyPrefix(StateFamily::PhaseColour) + keyText(r);
        case PK::SatClock:     return familyPrefix(StateFamily::SatClock) + keyText(r);
        case PK::VtecSh:       break;
    }
    return "STATE_" + std::to_string(index);
}

// =======================================================================
// Writing the set
// =======================================================================

StationParams startStationParams(const GnssConfig& config, const Eigen::Vector3d& position) {
    StationParams set;
    const auto push = [&](const ParamRef& ref, double x) {
        LocalSlot slot;
        slot.ref   = ref;
        slot.x_lin = x;
        addParam(set, slot);
    };
    for (std::uint8_t axis = 0; axis < 3; ++axis) push(rxPositionRef(axis), position(axis));
    push(rxClockRef(), 0.0);
    push(zwdRef(), 0.0);
    // One inter-system bias per active constellation other than the time reference (GPS), in
    // the order of the configuration's list.
    for (const std::string& name : config.getActiveSystems()) {
        if (name == "GPS") continue;
        const System sys = systemFromName(name);
        if (sys == System::Unknown || set.isb.count(sys) != 0) continue;
        push(isbRef(sys), 0.0);
    }
    return set;
}

namespace {

/// Whether a slot is one of the core columns, which are never removed.
bool isCore(const StationParams& set, int k) {
    return k == set.position[0] || k == set.position[1] || k == set.position[2]
        || k == set.clock || k == set.zwd;
}

/// Renumber a typed map after a removal, dropping the entries of removed slots.
template <class Map>
void renumber(Map& map, const std::vector<int>& remap) {
    for (auto it = map.begin(); it != map.end();) {
        const int to = remap[static_cast<std::size_t>(it->second)];
        if (to < 0) {
            it = map.erase(it);
        } else {
            it->second = to;
            ++it;
        }
    }
}

/// The slots of the entries of a typed map whose key is not in the keep set, in map order.
template <class Map, class Keep>
std::vector<int> slotsOutside(const Map& map, const Keep& keep) {
    std::vector<int> out;
    for (const auto& entry : map)
        if (keep.count(entry.first) == 0) out.push_back(entry.second);
    return out;
}

} // namespace

void removeSlots(StationParams& set, std::vector<int> indices) {
    const int n = static_cast<int>(set.slots.size());
    std::vector<char> drop(static_cast<std::size_t>(n), 0);
    bool any = false;
    for (const int k : indices) {
        if (k < 0 || k >= n || isCore(set, k)) continue;
        drop[static_cast<std::size_t>(k)] = 1;
        any = true;
    }
    if (!any) return;

    std::vector<int> remap(static_cast<std::size_t>(n), -1);
    std::vector<LocalSlot> kept;
    kept.reserve(static_cast<std::size_t>(n));
    std::size_t decided = 0;
    for (int k = 0; k < n; ++k) {
        if (drop[static_cast<std::size_t>(k)]) continue;
        remap[static_cast<std::size_t>(k)] = static_cast<int>(kept.size());
        if (static_cast<std::size_t>(k) < set.decided) ++decided;
        kept.push_back(std::move(set.slots[static_cast<std::size_t>(k)]));
    }
    set.slots   = std::move(kept);
    set.decided = decided;

    for (int& p : set.position) p = remap[static_cast<std::size_t>(p)];
    set.clock = remap[static_cast<std::size_t>(set.clock)];
    set.zwd   = remap[static_cast<std::size_t>(set.zwd)];
    renumber(set.isb, remap);
    renumber(set.iono, remap);
    renumber(set.colour, remap);
    renumber(set.amb, remap);
    renumber(set.rx_pb, remap);
    renumber(set.rx_cb, remap);
    renumber(set.shared.sat_clock, remap);
    renumber(set.shared.sat_phase_bias, remap);
    renumber(set.shared.sat_code_bias, remap);
}

void pruneAmbiguitiesExcept(StationParams& set, const std::set<SatSignalId>& keep) {
    std::vector<int> out = slotsOutside(set.amb, keep);
    if (out.empty()) return;
    if (set.record_pruned)
        for (const auto& entry : set.amb)
            if (keep.count(entry.first) == 0) set.pruned_ambiguities.push_back(entry.first);
    removeSlots(set, std::move(out));
}

void pruneSlantIonoExcept(StationParams& set, const std::set<SatId>& keep) {
    removeSlots(set, slotsOutside(set.iono, keep));
}

void prunePhaseColourExcept(StationParams& set, const std::set<SatId>& keep) {
    removeSlots(set, slotsOutside(set.colour, keep));
}

void pruneReceiverCodeBiasExcept(StationParams& set, const std::set<RxCbKey>& keep) {
    removeSlots(set, slotsOutside(set.rx_cb, keep));
}

void pruneReceiverPhaseBiasExcept(StationParams& set, const std::set<SignalId, SignalNameOrder>& keep) {
    removeSlots(set, slotsOutside(set.rx_pb, keep));
}

void pruneSatClockExcept(StationParams& set, const std::set<SatId>& keep) {
    removeSlots(set, slotsOutside(set.shared.sat_clock, keep));
}

void pruneSatPhaseBiasExcept(StationParams& set, const std::set<SatSignalId>& keep) {
    removeSlots(set, slotsOutside(set.shared.sat_phase_bias, keep));
}

void pruneSatCodeBiasExcept(StationParams& set, const std::set<SatSignalId>& keep) {
    removeSlots(set, slotsOutside(set.shared.sat_code_bias, keep));
}

void markPendingReseed(StationParams& set, const SatSignalId& id) { set.pending_reseed.insert(id); }

void markPendingReseed(StationParams& set, const std::string& key) {
    if (const std::optional<SatSignalId> id = net::satSignalKey(key)) set.pending_reseed.insert(*id);
}

bool pendingReseed(const StationParams& set, const SatSignalId& id) {
    return set.pending_reseed.count(id) != 0;
}

bool pendingReseed(const StationParams& set, const std::string& key) {
    const std::optional<SatSignalId> id = net::satSignalKey(key);
    return id && set.pending_reseed.count(*id) != 0;
}

void clearPendingReseed(StationParams& set, const SatSignalId& id) { set.pending_reseed.erase(id); }

void clearPendingReseed(StationParams& set, const std::string& key) {
    if (const std::optional<SatSignalId> id = net::satSignalKey(key)) set.pending_reseed.erase(*id);
}

std::vector<SatSignalId> takePrunedAmbiguities(StationParams& set) {
    std::vector<SatSignalId> out;
    out.swap(set.pruned_ambiguities);
    return out;
}

void settleStationParams(StationParams& set, const std::vector<ParamDisposition>& col_disp) {
    set.decided = std::min(col_disp.size(), set.slots.size());
    for (std::size_t k = 0; k < set.slots.size(); ++k)
        set.slots[k].disp = k < col_disp.size() ? col_disp[k] : ParamDisposition::Unobserved;

    StationShared& shared = set.shared;
    shared.absorbed.clear();
    shared.absorbed_no_absorber = 0;
    for (const AbsorbedParam& note : shared.withheld) {
        const ParamRef& r = note.ref;
        const bool absorber =
            r.kind == NetworkParamKind::SatPhaseBias ? set.amb.count(SatSignalId{r.sat, r.signal}) != 0
          : r.kind == NetworkParamKind::SatCodeBias  ? shared.sat_clock.count(r.sat) != 0
                                                     : false;
        if (absorber) shared.absorbed.push_back(note);
        else          ++shared.absorbed_no_absorber;
    }
    set.unclassified = shared.withheld_unnamed;
}

} // namespace GnssProcessor
