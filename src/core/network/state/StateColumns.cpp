/**
 * @file StateColumns.cpp
 * @brief The state manager's columns listed and classified (see network/state/StateColumns.hpp).
 */

#include "network/state/StateColumns.hpp"

#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace GnssProcessor {

namespace {

namespace K = StateKeys;

}  // namespace

// ---------------------------------------------------------------------------
// The station's columns, listed and classified
// ---------------------------------------------------------------------------

const char* stateContainerName(StateContainer c) {
    switch (c) {
        case StateContainer::PositionIndex:           return "position_index";
        case StateContainer::ClockIndex:              return "clock_index";
        case StateContainer::TropoIndex:              return "tropo_index";
        case StateContainer::SystemTimeOffsetStates:  return "system_time_offset_states";
        case StateContainer::IonosphereStates:        return "ionosphere_states";
        case StateContainer::PhaseColourStates:       return "phase_colour_states";
        case StateContainer::Ambiguities:             return "ambiguities";
        case StateContainer::ReceiverPhaseBiasStates: return "receiver_phase_bias_states";
        case StateContainer::CodeBiasStates:          return "code_bias_states";
        case StateContainer::SatClockStates:          return "sat_clock_states";
        case StateContainer::SatPhaseBiasStates:      return "sat_phase_bias_states";
        case StateContainer::SatBiasStates:           return "sat_bias_states";
        case StateContainer::Unlisted:                return "unlisted";
    }
    return "?";
}

std::vector<StateColumn> listStateColumns(const StateManager& sm) {
    using SC = StateContainer;
    const std::size_t n = sm.getStateSize();
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    std::vector<char> seen(n, 0);
    std::vector<StateColumn> out;
    out.reserve(n);
    auto put = [&](std::size_t idx, SC c, const std::string& key) {
        if (idx >= n || seen[idx]) return;
        seen[idx] = 1;
        out.push_back(StateColumn{idx, c, key});
    };
    auto putMap = [&](const std::map<std::string, std::size_t>& m, SC c) {
        for (const auto& kv : m) put(kv.second, c, kv.first);
    };

    const std::size_t pos = sm.getPositionIndex();
    for (std::size_t k = 0; k < 3; ++k) put(pos + k, SC::PositionIndex, std::string());
    if (sm.hasClock()) put(sm.getClockIndex(), SC::ClockIndex, std::string());
    if (sm.getTropoIndex() != kNone) put(sm.getTropoIndex(), SC::TropoIndex, std::string());
    putMap(sm.getSystemTimeOffsetStates(),  SC::SystemTimeOffsetStates);
    putMap(sm.getIonosphereStates(),        SC::IonosphereStates);
    putMap(sm.getPhaseColourStates(),       SC::PhaseColourStates);
    putMap(sm.getAmbiguities(),             SC::Ambiguities);
    putMap(sm.getReceiverPhaseBiasStates(), SC::ReceiverPhaseBiasStates);
    putMap(sm.getCodeBiasStates(),          SC::CodeBiasStates);
    putMap(sm.getSatClockStates(),          SC::SatClockStates);
    putMap(sm.getSatPhaseBiasStates(),      SC::SatPhaseBiasStates);
    putMap(sm.getSatBiasStates(),           SC::SatBiasStates);

    // Every other column: listed under the state manager's own label, so a family the
    // classifier does not know is counted and printed, never missed.
    if (out.size() < n) {
        const std::vector<std::string> labels = sm.getStateLabels();
        for (std::size_t i = 0; i < n; ++i)
            if (!seen[i]) put(i, SC::Unlisted, i < labels.size() ? labels[i] : std::string());
    }
    std::sort(out.begin(), out.end(),
              [](const StateColumn& a, const StateColumn& b) { return a.index < b.index; });
    return out;
}

std::optional<ParamInstanceId> classifyStateColumn(StateContainer container, std::string_view key) {
    using SC = StateContainer;
    using PK = NetworkParamKind;
    ParamInstanceId id;
    ParamClass& c = id.cls;
    // A satellite-keyed kind: kind, constellation and satellite from the satellite id.
    auto satKind = [&](PK kind, std::string_view sat_text) -> bool {
        const auto sat = SatId::parse(sat_text);
        if (!sat) return false;
        c.kind = kind;
        c.sys  = sat->sys;
        id.sat = *sat;
        return true;
    };
    // A signal of the class's constellation.
    auto signalOf = [&](std::string_view sig_text, SignalId& out) -> bool {
        const auto sig = SignalId::fromObsCode(c.sys, sig_text);
        if (!sig) return false;
        out = *sig;
        return true;
    };

    switch (container) {
        case SC::PositionIndex:
            c.kind = PK::RxPosition;
            return id;
        case SC::ClockIndex:
            c.kind = PK::RxClock;
            return id;
        case SC::TropoIndex:
            c.kind = PK::Zwd;
            return id;
        case SC::SystemTimeOffsetStates: {
            // The key is the system name (StateKeys::makeSystemTimeOffsetKey).
            const System s = key.size() > 1 ? systemFromName(key) : System::Unknown;
            if (s == System::Unknown) return std::nullopt;
            c.kind = PK::RxClock;
            c.form = ParamForm::Isb;
            c.sys  = s;
            return id;
        }
        case SC::IonosphereStates:
            if (!satKind(PK::SlantIono, key)) return std::nullopt;
            return id;
        case SC::PhaseColourStates:
            if (!satKind(PK::PhaseColour, key)) return std::nullopt;
            return id;
        case SC::Ambiguities: {
            if (K::isDdAmbiguityKey(key)) return std::nullopt;   // the baseline mode's key
            if (const auto nl = K::parseNlAmbiguityKey(key)) {
                if (!satKind(PK::Ambiguity, nl->satellite)) return std::nullopt;
                c.form = ParamForm::NarrowLane;
                c.band = bandOfDigit(c.sys, nl->band);
                if (c.band == FrequencyBand::Unknown) return std::nullopt;
                return id;
            }
            if (const auto wl = K::parseWlAmbiguityKey(key)) {
                if (!satKind(PK::Ambiguity, wl->satellite)) return std::nullopt;
                c.form   = ParamForm::WideLane;
                c.band   = bandOfDigit(c.sys, wl->band_a);
                c.band_b = bandOfDigit(c.sys, wl->band_b);
                if (c.band == FrequencyBand::Unknown || c.band_b == FrequencyBand::Unknown)
                    return std::nullopt;
                return id;
            }
            const auto p = K::parseAmbiguityKey(key);
            if (!p || !satKind(PK::Ambiguity, p->satellite)) return std::nullopt;
            if (p->signal.find(K::kAmbiguityDelimiter) == std::string::npos) {
                if (!signalOf(p->signal, c.signal)) return std::nullopt;
                return id;
            }
            // An ionosphere-free combination: "L1C_L2W".
            const auto pair = SignalPair::parse(c.sys, p->signal);
            if (!pair) return std::nullopt;
            c.form     = ParamForm::IfPair;
            c.signal   = pair->a;
            c.signal_b = pair->b;
            return id;
        }
        case SC::ReceiverPhaseBiasStates: {
            const auto p = K::parseReceiverPhaseBiasKey(key);
            if (!p) return std::nullopt;
            c.kind = PK::RxPhaseBias;
            c.sys  = systemFromName(p->system);
            if (!signalOf(p->signal, c.signal)) return std::nullopt;   // needs a known system
            return id;
        }
        case SC::CodeBiasStates: {
            // The forms without a signal are decided by the key's shape; their system name
            // is read through systemFromName and may be Unknown (the class still stands).
            c.kind = PK::RxCodeBias;
            if (K::isGlonassPerSlotIfbKey(key)) {
                const auto p = K::parseGlonassPerSlotIfbKey(key);
                if (!p) return std::nullopt;
                c.form      = ParamForm::GloSlotIfb;
                c.sys       = systemFromName(p->system);
                c.band      = bandOfDigit(c.sys, p->band);
                id.glo_slot = p->fcn_slot;
                return id;
            }
            if (const auto p = K::parseIfbKey(key)) {
                c.form = ParamForm::Ifb;
                c.sys  = systemFromName(p->system);
                c.band = bandOfDigit(c.sys, p->band);
                return id;
            }
            if (const auto p = K::parseDcbKey(key)) {
                c.form = ParamForm::Dcb;
                c.sys  = systemFromName(*p);
                return id;
            }
            if (const auto p = K::parseReceiverIscKey(key)) {
                c.form = ParamForm::Isc;
                c.sys  = systemFromName(p->system);
                if (!signalOf(p->signal, c.signal)) return std::nullopt;   // needs a known system
                return id;
            }
            return std::nullopt;
        }
        case SC::SatClockStates: {
            const auto sat = K::parseSatClockKey(key);
            if (!sat || !satKind(PK::SatClock, *sat)) return std::nullopt;
            return id;
        }
        case SC::SatPhaseBiasStates: {
            const auto p = K::parseSatPhaseBiasKey(key);
            if (!p || !satKind(PK::SatPhaseBias, p->satellite)) return std::nullopt;
            if (!signalOf(p->signal, c.signal)) return std::nullopt;
            return id;
        }
        case SC::SatBiasStates: {
            const auto p = K::parseSatBiasKey(key);
            if (!p || !satKind(PK::SatCodeBias, p->satellite)) return std::nullopt;
            if (!signalOf(p->signal, c.signal)) return std::nullopt;
            return id;
        }
        case SC::Unlisted:
            return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Absorbed instances
// ---------------------------------------------------------------------------

std::optional<AbsorbedInstance> classifyAbsorbed(const AbsorbedNote& note) {
    if (const auto id = classifyStateColumn(note.container, note.key))
        return AbsorbedInstance{*id, note.reason};
    return std::nullopt;
}

std::string absorbedNoteLabel(const AbsorbedNote& note) {
    return std::string(stateContainerName(note.container)) + ":" + note.key;
}

bool absorberPresent(const StateManager& sm, const AbsorbedInstance& a) {
    const std::string sat = a.id.sat.str();
    switch (a.id.cls.kind) {
        case NetworkParamKind::SatPhaseBias:
            return sm.getAmbiguities().count(
                       K::makeAmbiguityKey(sat, a.id.cls.signal.obsCode(ObsType::Phase))) != 0;
        case NetworkParamKind::SatCodeBias:
            return sm.getSatClockStates().count(K::makeSatClockKey(sat)) != 0;
        default:
            return false;
    }
}

} // namespace GnssProcessor
