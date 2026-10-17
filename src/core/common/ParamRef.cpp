/**
 * @file ParamRef.cpp
 * @brief Names and orders of the network parameter identities (see common/ParamRef.hpp).
 */
#include "common/ParamRef.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <tuple>

namespace GnssProcessor {

namespace {

/// The signed text of a GLONASS slot as the state key renders it: '+' for zero and above
/// ("+5"), the minus sign of the decimal text below ("-4").
struct SlotText {
    char        buf[16]{};
    std::size_t len{0};
    std::string_view view() const { return std::string_view(buf, len); }
};

SlotText slotText(int slot) {
    SlotText t;
    t.buf[0] = slot >= 0 ? '+' : '-';
    const long long magnitude = slot >= 0 ? slot : -static_cast<long long>(slot);
    const auto res = std::to_chars(t.buf + 1, t.buf + sizeof(t.buf), magnitude);
    t.len = static_cast<std::size_t>(res.ptr - t.buf);
    return t;
}

/// The position of a form among the receiver code-bias keys of one constellation: the
/// differential bias first, then the inter-frequency keys (per band and per slot), then the
/// inter-signal keys; any other form after them.
int rxCbFormRank(ParamForm f) {
    switch (f) {
        case ParamForm::Dcb:        return 0;
        case ParamForm::Ifb:
        case ParamForm::GloSlotIfb: return 1;
        case ParamForm::Isc:        return 2;
        default:                    return 3 + static_cast<int>(f);
    }
}

} // namespace

const char* paramKindName(NetworkParamKind kind) {
    switch (kind) {
        case NetworkParamKind::SatClock:     return "sat_clock";
        case NetworkParamKind::SatPhaseBias: return "sat_phase_bias";
        case NetworkParamKind::SatCodeBias:  return "sat_code_bias";
        case NetworkParamKind::VtecSh:       return "vtec_sh";
        case NetworkParamKind::Ambiguity:    return "ambiguity";
        case NetworkParamKind::Zwd:          return "zwd";
        case NetworkParamKind::SlantIono:    return "slant_iono";
        case NetworkParamKind::RxClock:      return "rx_clock";
        case NetworkParamKind::RxPhaseBias:  return "rx_phase_bias";
        case NetworkParamKind::RxCodeBias:   return "rx_code_bias";
        case NetworkParamKind::PhaseColour:  return "phase_colour";
        case NetworkParamKind::RxPosition:   return "rx_position";
    }
    return "?";
}

const char* paramFormName(ParamForm f) {
    switch (f) {
        case ParamForm::None:       return "";
        case ParamForm::Isb:        return "isb";
        case ParamForm::IfPair:     return "if";
        case ParamForm::NarrowLane: return "nl";
        case ParamForm::WideLane:   return "wl";
        case ParamForm::Dcb:        return "dcb";
        case ParamForm::Ifb:        return "ifb";
        case ParamForm::Isc:        return "isc";
        case ParamForm::GloSlotIfb: return "slot";
    }
    return "?";
}

bool operator<(const ParamClass& a, const ParamClass& b) {
    // Table order: the registry's kind order, then the form, the constellation in enum
    // order (GPS first; none, i.e. the receiver clock, before every constellation), the
    // bands and the signals.
    const auto key = [](const ParamClass& c) {
        const int sys = c.sys == System::Unknown ? -1 : static_cast<int>(c.sys);
        return std::make_tuple(static_cast<int>(c.kind), static_cast<int>(c.form), sys,
                               static_cast<int>(c.band), static_cast<int>(c.band_b));
    };
    if (key(a) != key(b)) return key(a) < key(b);
    if (a.signal != b.signal) return a.signal < b.signal;
    return a.signal_b < b.signal_b;
}

bool operator<(const RxCbKey& a, const RxCbKey& b) {
    // The state key is "<system name>_<rest>": the name decides first (no name is a prefix
    // of another, so the text after the name is compared only for one constellation).
    if (a.sys != b.sys) return systemNameLess(a.sys, b.sys);
    const int ra = rxCbFormRank(a.form);
    const int rb = rxCbFormRank(b.form);
    if (ra != rb) return ra < rb;
    switch (ra) {
        case 1: {
            // "IFB<digit>" and its per-slot keys "IFB<digit>_k<slot>": by digit, the
            // per-band key (a prefix of its per-slot keys) first, then the slot text.
            const char da = digitOfBand(a.sys, a.band);
            const char db = digitOfBand(b.sys, b.band);
            if (da != db) return da < db;
            if (a.form != b.form) return a.form == ParamForm::Ifb;
            if (a.form == ParamForm::Ifb) return false;
            return slotText(a.glo_slot).view() < slotText(b.glo_slot).view();
        }
        case 2:
            return a.signal < b.signal;
        default:
            // The Dcb key is unique per constellation; a form outside the four never
            // reaches a map, but the order stays a strict weak order on the form.
            return a.form < b.form;
    }
}

// =======================================================================
// One parameter, without its station
// =======================================================================

bool operator==(const ParamRef& a, const ParamRef& b) {
    return a.kind == b.kind && a.form == b.form && a.sys == b.sys && a.sat == b.sat
        && a.signal == b.signal && a.signal_b == b.signal_b && a.band == b.band
        && a.band_b == b.band_b && a.glo_slot == b.glo_slot && a.axis == b.axis
        && a.core == b.core;
}

std::string describe(const ParamRef& r) {
    std::string out = paramKindName(r.kind);
    if (r.form != ParamForm::None) {
        out += '.';
        out += paramFormName(r.form);
    }
    switch (r.core) {
        case UserCore::None:          break;
        case UserCore::PhaseClock:    out += " phase_clock"; break;
        case UserCore::GradientNorth: out += " gradient_north"; break;
        case UserCore::GradientEast:  out += " gradient_east"; break;
    }
    if (r.kind == NetworkParamKind::RxPosition) out += " axis=" + std::to_string(r.axis);
    if (r.sys != System::Unknown) out += std::string(" ") + systemName(r.sys);
    if (r.sat.valid()) out += " " + r.sat.str();
    if (r.signal.digit != '\0') out += " " + r.signal.str();
    if (r.signal_b.digit != '\0') out += " " + r.signal_b.str();
    if (r.band != FrequencyBand::Unknown) out += " band=" + std::to_string(static_cast<int>(r.band));
    if (r.band_b != FrequencyBand::Unknown) out += " band_b=" + std::to_string(static_cast<int>(r.band_b));
    if (r.form == ParamForm::GloSlotIfb) out += " k=" + std::to_string(r.glo_slot);
    return out;
}

ParamRef rxPositionRef(std::uint8_t axis) {
    ParamRef r;
    r.kind = NetworkParamKind::RxPosition;
    r.axis = axis;
    return r;
}

ParamRef zwdRef() {
    ParamRef r;
    r.kind = NetworkParamKind::Zwd;
    return r;
}

ParamRef rxClockRef() {
    ParamRef r;
    r.kind = NetworkParamKind::RxClock;
    return r;
}

ParamRef isbRef(System sys) {
    ParamRef r;
    r.kind = NetworkParamKind::RxClock;
    r.form = ParamForm::Isb;
    r.sys  = sys;
    return r;
}

ParamRef phaseClockRef() {
    ParamRef r;
    r.kind = NetworkParamKind::RxClock;
    r.core = UserCore::PhaseClock;
    return r;
}

ParamRef gradientRef(bool north) {
    ParamRef r;
    r.kind = NetworkParamKind::Zwd;
    r.core = north ? UserCore::GradientNorth : UserCore::GradientEast;
    return r;
}

ParamRef slantIonoRef(SatId sat) {
    ParamRef r;
    r.kind = NetworkParamKind::SlantIono;
    r.sat  = sat;
    return r;
}

ParamRef rxCodeBiasRef(const RxCbKey& key) {
    ParamRef r;
    r.kind     = NetworkParamKind::RxCodeBias;
    r.form     = key.form;
    r.sys      = key.sys;
    r.band     = key.band;
    r.signal   = key.signal;
    r.glo_slot = key.glo_slot;
    return r;
}

RxCbKey rxCbKeyOf(const ParamRef& r) {
    RxCbKey key;
    key.sys      = r.sys;
    key.form     = r.form;
    key.band     = r.band;
    key.signal   = r.signal;
    key.glo_slot = r.glo_slot;
    return key;
}

ParamRef satCodeBiasRef(SatId sat, SignalId code) {
    ParamRef r;
    r.kind   = NetworkParamKind::SatCodeBias;
    r.sat    = sat;
    r.signal = code;
    return r;
}

ParamRef satCodeBiasIfRef(SatId sat, const SignalPair& pair) {
    ParamRef r = satCodeBiasRef(sat, pair.a);
    r.form     = ParamForm::IfPair;
    r.signal_b = pair.b;
    return r;
}

ParamRef satPhaseBiasRef(SatId sat, SignalId phase) {
    ParamRef r;
    r.kind   = NetworkParamKind::SatPhaseBias;
    r.sat    = sat;
    r.signal = phase;
    return r;
}

ParamRef rxPhaseBiasRef(System sys, SignalId phase) {
    ParamRef r;
    r.kind   = NetworkParamKind::RxPhaseBias;
    r.sys    = sys;
    r.signal = phase;
    return r;
}

ParamRef satClockRef(SatId sat) {
    ParamRef r;
    r.kind = NetworkParamKind::SatClock;
    r.sat  = sat;
    return r;
}

ParamRef ambiguityRef(SatId sat, SignalId phase) {
    ParamRef r;
    r.kind   = NetworkParamKind::Ambiguity;
    r.sat    = sat;
    r.signal = phase;
    return r;
}

ParamRef ambiguityIfRef(SatId sat, const SignalPair& pair) {
    ParamRef r = ambiguityRef(sat, pair.a);
    r.form     = ParamForm::IfPair;
    r.signal_b = pair.b;
    return r;
}

ParamRef narrowLaneRef(SatId sat, FrequencyBand ref_band) {
    ParamRef r;
    r.kind = NetworkParamKind::Ambiguity;
    r.form = ParamForm::NarrowLane;
    r.sat  = sat;
    r.band = ref_band;
    return r;
}

ParamRef wideLaneRef(SatId sat, FrequencyBand ref_band, FrequencyBand band) {
    ParamRef r = narrowLaneRef(sat, ref_band);
    r.form   = ParamForm::WideLane;
    r.band_b = band;
    return r;
}

ParamRef phaseColourRef(SatId sat) {
    ParamRef r;
    r.kind = NetworkParamKind::PhaseColour;
    r.sat  = sat;
    return r;
}

} // namespace GnssProcessor
