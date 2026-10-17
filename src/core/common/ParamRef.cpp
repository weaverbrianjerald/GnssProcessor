/**
 * @file ParamRef.cpp
 * @brief Names and orders of the network parameter identities (see common/ParamRef.hpp).
 */
#include "common/ParamRef.hpp"

#include <charconv>
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

} // namespace GnssProcessor
