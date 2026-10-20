/**
 * @file UserParamView.cpp
 * @brief The user-side state keys of the typed parameter identities, and the lookup of a
 *        parameter in a StateManager (see measurement/UserParamView.hpp).
 */

#include "measurement/UserParamView.hpp"

#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"

#include <limits>

namespace GnssProcessor {

namespace {

namespace K = StateKeys;

/// The text of a signal pair for one observation type ("L1C_L2W").
std::string pairText(const ParamRef& ref, ObsType type) {
    return SignalPair{ref.signal, ref.signal_b}.str(type);
}

/// The ambiguity key: "<sat>_<signal>", "<sat>_<signal>_<signal>", the NL or the WL key.
std::string ambiguityKey(const ParamRef& ref) {
    const std::string sat = ref.sat.str();
    switch (ref.form) {
        case ParamForm::None:
            return K::makeAmbiguityKey(sat, ref.signal.obsCode(ObsType::Phase));
        case ParamForm::IfPair:
            return K::makeAmbiguityKey(sat, pairText(ref, ObsType::Phase));
        case ParamForm::NarrowLane: {
            const char d = digitOfBand(ref.sat.sys, ref.band);
            return d == '\0' ? std::string() : K::makeNlAmbiguityKey(sat, d);
        }
        case ParamForm::WideLane: {
            const char da = digitOfBand(ref.sat.sys, ref.band);
            const char db = digitOfBand(ref.sat.sys, ref.band_b);
            return (da == '\0' || db == '\0') ? std::string() : K::makeWlAmbiguityKey(sat, da, db);
        }
        default:
            return {};
    }
}

} // namespace

std::string userCodeBiasKey(const RxCbKey& key) {
    const std::string sys = systemName(key.sys);
    switch (key.form) {
        case ParamForm::Dcb:
            return K::makeDcbKey(sys);
        case ParamForm::Ifb: {
            const char d = digitOfBand(key.sys, key.band);
            return d == '\0' ? std::string() : K::makeIfbKey(sys, d);
        }
        case ParamForm::GloSlotIfb: {
            const char d = digitOfBand(key.sys, key.band);
            return d == '\0' ? std::string() : K::makeGlonassPerSlotIfbKey(sys, d, key.glo_slot);
        }
        case ParamForm::Isc:
            return K::makeReceiverIscKey(sys, key.signal.obsCode(ObsType::Code));
        default:
            return {};
    }
}

std::string userKey(const ParamRef& ref) {
    switch (ref.kind) {
        case NetworkParamKind::Ambiguity:
            return ambiguityKey(ref);
        case NetworkParamKind::SlantIono:
            return K::makeIonosphereKey(ref.sat.str());
        case NetworkParamKind::PhaseColour:
            return ref.sat.str();
        case NetworkParamKind::RxCodeBias:
            return userCodeBiasKey(rxCbKeyOf(ref));
        case NetworkParamKind::SatCodeBias:
            return K::makeSatBiasKey(ref.sat.str(),
                                     ref.form == ParamForm::IfPair
                                         ? pairText(ref, ObsType::Code)
                                         : ref.signal.obsCode(ObsType::Code));
        case NetworkParamKind::SatPhaseBias:
            return K::makeSatPhaseBiasKey(ref.sat.str(), ref.signal.obsCode(ObsType::Phase));
        case NetworkParamKind::RxPhaseBias:
            return K::makeReceiverPhaseBiasKey(systemName(ref.sys),
                                               ref.signal.obsCode(ObsType::Phase));
        case NetworkParamKind::SatClock:
            return K::makeSatClockKey(ref.sat.str());
        default:
            return {};
    }
}

ParamSlot UserParamView::fixed(std::size_t column) const {
    return ParamSlot{static_cast<int>(column), x_(static_cast<Eigen::Index>(column))};
}

ParamSlot UserParamView::dynamic(long long column) const {
    if (column < 0 || column >= static_cast<long long>(state_size_)) return ParamSlot{};
    return ParamSlot{static_cast<int>(column), x_(static_cast<Eigen::Index>(column))};
}

ParamSlot UserParamView::find(const ParamRef& ref) const {
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    switch (ref.kind) {
        case NetworkParamKind::RxPosition:
            return fixed(states_.getPositionIndex() + ref.axis);
        case NetworkParamKind::Zwd:
            if (ref.core == UserCore::None) return fixed(states_.getTropoIndex());
            if (!states_.hasTropoGradients()) return ParamSlot{};
            return fixed(ref.core == UserCore::GradientNorth ? states_.getTropoGradientNorthIndex()
                                                             : states_.getTropoGradientEastIndex());
        case NetworkParamKind::RxClock:
            if (ref.core == UserCore::PhaseClock)
                return states_.hasPhaseClock() ? fixed(states_.getPhaseClockIndex()) : ParamSlot{};
            if (ref.form == ParamForm::Isb) {
                const std::size_t idx = states_.getSystemTimeOffsetIndex(systemName(ref.sys));
                return idx == kNone ? ParamSlot{} : dynamic(static_cast<long long>(idx));
            }
            return states_.hasClock() ? fixed(states_.getClockIndex()) : ParamSlot{};
        case NetworkParamKind::SlantIono:
            return dynamic(states_.getIonosphereIndex(userKey(ref)));
        case NetworkParamKind::Ambiguity:
            return dynamic(states_.getAmbiguityIndex(userKey(ref)));
        case NetworkParamKind::RxCodeBias:
            return dynamic(states_.getCodeBiasIndex(userKey(ref)));
        case NetworkParamKind::SatCodeBias:
            return dynamic(states_.getSatBiasIndex(userKey(ref)));
        case NetworkParamKind::SatPhaseBias:
            return dynamic(states_.getSatPhaseBiasIndex(userKey(ref)));
        case NetworkParamKind::RxPhaseBias:
            return dynamic(states_.getReceiverPhaseBiasIndex(userKey(ref)));
        case NetworkParamKind::SatClock:
            return dynamic(states_.getSatClockIndex(userKey(ref)));
        case NetworkParamKind::PhaseColour:
            return dynamic(states_.getPhaseColourIndex(userKey(ref)));
        case NetworkParamKind::VtecSh:
            return ParamSlot{};
    }
    return ParamSlot{};
}

} // namespace GnssProcessor
