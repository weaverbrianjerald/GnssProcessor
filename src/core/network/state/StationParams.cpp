/**
 * @file StationParams.cpp
 * @brief Registration and lookup of the parameters of a station's set (see
 *        network/state/StationParams.hpp).
 */

#include "network/state/StationParams.hpp"

#include "filter/StateFamily.hpp"          // kStateFamilies: the label prefixes
#include "network/state/Entities.hpp"      // net::satSignalKey
#include "measurement/UserParamView.hpp"   // userKey: the one rendering of a typed identity as a key

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
    if (slot.unnamed) {
        ++set.untyped;
        return false;
    }
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

namespace {

/// The label prefix of a dynamic state family.
std::string familyPrefix(StateFamily f) { return specOf(f).label_prefix; }

}  // namespace

std::string stateLabel(const LocalSlot& slot, std::size_t index) {
    using PK = NetworkParamKind;
    const ParamRef& r = slot.ref;
    if (!slot.unnamed) {
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
    }
    return "STATE_" + std::to_string(index);
}

} // namespace GnssProcessor
