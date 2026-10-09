/**
 * @file NetworkParamInventory.cpp
 * @brief The network parameter inventory (see NetworkParamInventory.hpp; card NOM N1).
 */
#include "filter/NetworkParamInventory.hpp"

#include "app/NetworkRunContext.hpp"     // NetworkRunContext::param_inventory
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"    // StationAssembly::param_inv, ok, threw
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"  // appendRunManifestRecord
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace GnssProcessor {

namespace {

namespace K = StateKeys;

/// Most distinct unclassified labels a tally keeps (the count itself is never capped).
constexpr std::size_t kMaxUnclassifiedLabels = 50;

/// Most unclassified labels the table prints.
constexpr std::size_t kMaxUnclassifiedPrinted = 20;

void markStation(std::vector<char>& v, int station) {
    if (station < 0) return;
    const auto i = static_cast<std::size_t>(station);
    if (i >= v.size()) v.resize(i + 1, 0);
    v[i] = 1;
}

void uniteStations(std::vector<char>& into, const std::vector<char>& from) {
    if (from.size() > into.size()) into.resize(from.size(), 0);
    for (std::size_t i = 0; i < from.size(); ++i)
        if (from[i]) into[i] = 1;
}

long long countStations(const std::vector<char>& v) {
    return static_cast<long long>(std::count(v.begin(), v.end(), char{1}));
}

void addLabel(std::map<std::string, long long>& labels, const std::string& label, long long n) {
    const auto it = labels.find(label);
    if (it != labels.end()) it->second += n;
    else if (labels.size() < kMaxUnclassifiedLabels) labels.emplace(label, n);
}

/// The observation type a class's signal is rendered with: phase for the ambiguity and
/// the phase biases, code for the code biases.
ObsType signalTypeOf(const ParamClass& c) {
    switch (c.kind) {
        case NetworkParamKind::Ambiguity:
        case NetworkParamKind::RxPhaseBias:
        case NetworkParamKind::SatPhaseBias: return ObsType::Phase;
        default:                             return ObsType::Code;
    }
}

/// A signal's observation code for a class ("L1C", "C1C"), or "" for an unset signal.
std::string signalText(const ParamClass& c, const SignalId& s) {
    if (s.digit == '\0') return {};
    return s.obsCode(signalTypeOf(c));
}

/// The short name of a band ("L5", "E5a", "G1"), from @c bandLabel without its
/// constellation prefix; "?" for Unknown.
std::string bandText(FrequencyBand b) {
    if (b == FrequencyBand::Unknown) return "?";
    const std::string label = bandLabel(b);
    const auto us = label.find('_');
    return us == std::string::npos ? label : label.substr(us + 1);
}

/// The JSON value of a band: @c bandLabel ("GPS_L5"), or "" for an unset band.
std::string bandJson(FrequencyBand b) {
    return b == FrequencyBand::Unknown ? std::string() : std::string(bandLabel(b));
}

/// The JSON value of a constellation: its name, or "" for the station-wide kinds.
std::string systemJson(System s) {
    return s == System::Unknown ? std::string() : std::string(systemName(s));
}

/// The kind column: the registry name, with the form appended ("rx_code_bias.ifb").
std::string kindText(const ParamClass& c) {
    std::string k = paramKindName(c.kind);
    if (c.form != ParamForm::None) k += std::string(".") + paramFormName(c.form);
    return k;
}

std::string jsonEscape(const std::string& v) {
    std::string o;
    o.reserve(v.size() + 4);
    for (char ch : v) {
        if (ch == '\\' || ch == '"') o.push_back('\\');
        o.push_back(ch);
    }
    return o;
}

std::string w6IonoText(const RxIfbRule& r) { return r.w6_iono_none ? "none" : "auto"; }

/// Why the rule's premise fails on this run, e.g. "estimation.ionosphere_weighting off".
std::string premiseFailure(const RxIfbRule& r) {
    std::string why;
    if (!r.iono_weighting) why = "estimation.ionosphere_weighting off";
    if (r.w6_iono_none)
        why += std::string(why.empty() ? "" : "; ") + "network.w6_iono_constraint none";
    return why;
}

/// The note that explains a class's counts, where a rule or a fixed treatment does.
std::string classNote(const ParamClass& c, const RxIfbRule& rule) {
    if (c.kind == NetworkParamKind::RxPosition) return "pinned: held at the roster coordinate";
    if (RxIfbRule::governs(c)) {
        if (rule.drop)
            return fmt::format("rule rx_ifb: {} ({}); dropped: {}",
                               paramDispositionName(rule.disposition), rule.source, rule.dropped_by);
        return fmt::format("rule rx_ifb: not armed ({} off), estimated", rule.dropped_by);
    }
    return {};
}

/// The absorbed part of a class's note, e.g. "absorbed: unmatched antenna 5"; "" when the
/// class has no absorbed instance.  Reasons in enum order.
std::string absorbedNote(const ParamClassCount& cnt) {
    std::string s;
    for (std::size_t k = 0; k < kAbsorbedReasons; ++k) {
        if (cnt.absorbed_by[k] == 0) continue;
        s += fmt::format("{}{} {}", s.empty() ? "absorbed: " : ", ",
                         absorbedReasonSpec(static_cast<AbsorbedReason>(k)).label, cnt.absorbed_by[k]);
    }
    return s;
}

/// A class's whole note: the rule or fixed treatment, then the absorbed reasons.
std::string fullNote(const ParamClass& c, const ParamClassCount& cnt, const RxIfbRule& rule) {
    std::string note = classNote(c, rule);
    const std::string abs = absorbedNote(cnt);
    if (!abs.empty()) note += (note.empty() ? "" : "; ") + abs;
    return note;
}

std::string fmtTime(int week, double tow) { return fmt::format("week {} TOW {:.1f}", week, tow); }

}  // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* paramDispositionName(ParamDisposition d) {
    switch (d) {
        case ParamDisposition::EstimatedBorder: return "estimated_border";
        case ParamDisposition::EstimatedLocal:  return "estimated_local";
        case ParamDisposition::HeldAtSeed:      return "held_at_seed";
        case ParamDisposition::Absorbed:        return "absorbed";
        case ParamDisposition::Pinned:          return "pinned";
        case ParamDisposition::Dropped:         return "dropped";
        case ParamDisposition::Unobserved:      return "unobserved";
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

// ---------------------------------------------------------------------------
// The station's columns, listed and classified
// ---------------------------------------------------------------------------

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

namespace {

/// What takes up a withheld satellite phase bias, at every gate: the phase row is still
/// built (buildJacobianRow writes the phase-bias column only when the state exists), so
/// the station's float ambiguity of that satellite and signal carries the bias.
constexpr const char* kByAmbiguity = "the station's float ambiguity of the same satellite and signal";

/// The reason table, in enum order (the static_assert below).  Each row was read against
/// the code it names (card NOM N1b).
constexpr AbsorbedReasonSpec kAbsorbedReasonTable[kAbsorbedReasons] = {
    {AbsorbedReason::GlonassCodeOnlyScope, "glonass_code_only_scope", "GLONASS code-only scope",
     "GLONASS is admitted on code-only scope (network.estimate_glonass): a shared "
     "per-(satellite, signal) bias is unphysical under FDMA, so none is allocated",
     "MeasurementModel::ensureNetworkSharedStates", kByAmbiguity,
     // The code comment names the per-slot IFB alone; that column exists only on the FDMA
     // bands at a known slot k != 0 (StateInitializer::resolveReceiverBias).  The case does
     // not arise in a loaded run: the loader refuses network.estimate_sat_code_bias with
     // network.estimate_glonass.
     "the station's receiver per-slot IFB (FDMA bands, slot k != 0 only; not reachable in a "
     "loaded run, which refuses network.estimate_sat_code_bias with network.estimate_glonass)"},
    {AbsorbedReason::UnmatchedAntenna, "unmatched_antenna", "unmatched antenna",
     "the station's receiver antenna has no ATX model and "
     "network.phase_bias_exclude_unmatched_antenna is armed (never the clock-datum station)",
     "MeasurementModel::ensureNetworkSharedStates", kByAmbiguity, ""},
    {AbsorbedReason::CoverageGate, "coverage_gate", "coverage allowlist",
     "the signal is outside the coverage allowlist built from "
     "network.phase_bias_min_station_fraction",
     "MeasurementModel::ensureNetworkSharedStates", kByAmbiguity, ""},
    {AbsorbedReason::Allowlist, "allowlist", "pivot allowlist",
     "the legacy single-pivot datum is in force (network.phase_datum_prioritized off) and the "
     "key is not among the pivot station's satellite phase-bias keys",
     "MeasurementModel::ensureNetworkSharedStates", kByAmbiguity, ""},
    {AbsorbedReason::CodeCensusCogate, "code_census_cogate", "code-census co-gate",
     "the CB-ADM census (network.cb_admission) quarantined the signal's companion code pair",
     "MeasurementModel::ensureNetworkSharedStates", kByAmbiguity, ""},
    {AbsorbedReason::AnchoredUnionCoverage, "anchored_union_coverage", "anchored-union coverage",
     "fewer than network.phase_datum_min_stations stations hold the key this epoch",
     "networkBuildPhaseDatumCandidates", kByAmbiguity, ""},
    {AbsorbedReason::AnchorHygieneBlock, "anchor_hygiene_block", "eq34 hygiene block",
     "every candidate host arc of the key is younger than network.eq34_anchor_min_arc_ep epochs",
     "networkAnchorHygiene", kByAmbiguity, ""},
};

constexpr bool reasonTableInEnumOrder() {
    for (std::size_t i = 0; i < kAbsorbedReasons; ++i)
        if (static_cast<std::size_t>(kAbsorbedReasonTable[i].reason) != i) return false;
    return true;
}
static_assert(reasonTableInEnumOrder(), "kAbsorbedReasonTable must list the reasons in enum order");

}  // namespace

const AbsorbedReasonSpec& absorbedReasonSpec(AbsorbedReason r) {
    const auto i = static_cast<std::size_t>(r);
    return kAbsorbedReasonTable[i < kAbsorbedReasons ? i : 0];
}

const char* absorbedReasonName(AbsorbedReason r) { return absorbedReasonSpec(r).name; }

const char* absorbedBy(AbsorbedReason r, NetworkParamKind kind) {
    const AbsorbedReasonSpec& s = absorbedReasonSpec(r);
    if (kind == NetworkParamKind::SatPhaseBias) return s.by_phase;
    if (kind == NetworkParamKind::SatCodeBias)  return s.by_code;
    return "";
}

void noteAbsorbed(StationAbsorbed& into, StateContainer container, std::string_view key,
                  AbsorbedReason reason) {
    if (const auto id = classifyStateColumn(container, key)) {
        into.instances.push_back(AbsorbedInstance{*id, reason});
        return;
    }
    into.unclassified.push_back(std::string(stateContainerName(container)) + ":" + std::string(key));
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

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

RxIfbRule networkRxIfbRule(const GnssConfig& config) {
    RxIfbRule r;
    r.drop           = config.estimateNetworkSatCodeBias();
    r.iono_weighting = config.getIonosphereWeightingConfig().enabled;
    r.w6_iono_none   = config.getNetworkConfig().w6_iono_constraint == NetworkW6IonoConstraint::None;
    r.premise_holds  = r.iono_weighting && !r.w6_iono_none;
    return r;
}

// ---------------------------------------------------------------------------
// Per-station record
// ---------------------------------------------------------------------------

void recordStationParams(const StateManager& sm, int station,
                         const std::vector<ParamDisposition>& col_disp,
                         const StationAbsorbed& absorbed,
                         StationParamSlot& slot) {
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
    // to absorb it and is counted apart.
    for (const AbsorbedInstance& a : absorbed.instances) {
        if (!absorberPresent(sm, a)) {
            ++slot.absorbed_no_absorber;
            continue;
        }
        StationParamRecord rec;
        rec.id         = a.id;
        rec.id.station = StationId{station};
        rec.disp       = ParamDisposition::Absorbed;
        rec.reason     = a.reason;
        slot.records.push_back(rec);
    }
    for (const std::string& label : absorbed.unclassified)
        slot.unclassified.push_back("absorbed " + label);
}

// ---------------------------------------------------------------------------
// Classes and tallies
// ---------------------------------------------------------------------------

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

std::string paramClassLabel(const ParamClass& c) {
    const std::string sys = traitsOf(c.sys).name;
    const std::string sig = c.signal.digit ? signalText(c, c.signal) : std::string("?");
    switch (c.kind) {
        case NetworkParamKind::SatClock:     return "satellite clock, " + sys;
        case NetworkParamKind::SatPhaseBias: return "satellite phase bias, " + sys + " " + sig;
        case NetworkParamKind::SatCodeBias:  return "satellite code bias, " + sys + " " + sig;
        case NetworkParamKind::VtecSh:       return "SH-VTEC coefficient";
        case NetworkParamKind::Ambiguity:
            switch (c.form) {
                case ParamForm::WideLane:
                    return fmt::format("wide-lane ambiguity, {} {}-{}", sys, bandText(c.band), bandText(c.band_b));
                case ParamForm::NarrowLane:
                    return fmt::format("narrow-lane ambiguity, {} {}", sys, bandText(c.band));
                case ParamForm::IfPair:
                    return "ambiguity, " + sys + " " + SignalPair{c.signal, c.signal_b}.str(ObsType::Phase);
                default:
                    return "ambiguity, " + sys + " " + sig;
            }
        case NetworkParamKind::Zwd:          return "zenith wet delay";
        case NetworkParamKind::SlantIono:    return "slant ionosphere, " + sys;
        case NetworkParamKind::RxClock:
            return c.form == ParamForm::Isb ? "inter-system bias, " + sys : std::string("receiver clock");
        case NetworkParamKind::RxPhaseBias:  return "receiver phase bias, " + sys + " " + sig;
        case NetworkParamKind::RxCodeBias:
            switch (c.form) {
                case ParamForm::Dcb:        return "receiver DCB, " + sys;
                case ParamForm::Ifb:        return "receiver IFB, " + sys + " " + bandText(c.band);
                case ParamForm::Isc:        return "receiver ISC, " + sys + " " + sig;
                case ParamForm::GloSlotIfb: return "receiver slot IFB, " + sys + " " + bandText(c.band);
                default:                    return "receiver code bias, " + sys;
            }
        case NetworkParamKind::PhaseColour:  return "phase colour, " + sys;
        case NetworkParamKind::RxPosition:   return "receiver position";
    }
    return "?";
}

long long ParamClassCount::nStations() const { return countStations(stations); }

long long ParamInventoryTally::nStations() const { return countStations(stations); }

void ParamInventoryTally::addStation(int station, const StationParamSlot& slot) {
    if (!slot.filled) return;
    ++station_epochs;
    markStation(stations, station);
    for (const StationParamRecord& rec : slot.records) {
        ParamClassCount& cnt = classes[rec.id.cls];
        ++cnt.n[static_cast<std::size_t>(rec.disp)];
        if (rec.disp == ParamDisposition::Absorbed && rec.reason)
            ++cnt.absorbed_by[static_cast<std::size_t>(*rec.reason)];
        markStation(cnt.stations, station);
    }
    for (const std::string& label : slot.unclassified) {
        ++unclassified;
        addLabel(unclassified_labels, label, 1);
    }
    no_disposition       += slot.no_disposition;
    absorbed_no_absorber += slot.absorbed_no_absorber;
}

void ParamInventoryTally::merge(const ParamInventoryTally& other) {
    // A tally holds content when an epoch was assembled into it or a roster station failed
    // to reach its split; the time span follows the tallies with content.
    const auto holds = [](const ParamInventoryTally& t) {
        return t.epochs > 0 || t.station_epochs_unreached > 0;
    };
    if (!holds(other)) return;
    if (!holds(*this)) {
        week_first = other.week_first;
        tow_first  = other.tow_first;
    }
    week_last = other.week_last;
    tow_last  = other.tow_last;
    epochs                   += other.epochs;
    station_epochs           += other.station_epochs;
    station_epochs_rejected  += other.station_epochs_rejected;
    station_epochs_unreached += other.station_epochs_unreached;
    unreached_absent         += other.unreached_absent;
    unreached_no_rows        += other.unreached_no_rows;
    unreached_state_mismatch += other.unreached_state_mismatch;
    unreached_exception      += other.unreached_exception;
    uniteStations(stations, other.stations);
    for (const auto& [cls, cnt] : other.classes) {
        ParamClassCount& c = classes[cls];
        for (std::size_t k = 0; k < kParamDispositions; ++k) c.n[k] += cnt.n[k];
        for (std::size_t k = 0; k < kAbsorbedReasons; ++k) c.absorbed_by[k] += cnt.absorbed_by[k];
        uniteStations(c.stations, cnt.stations);
    }
    unclassified += other.unclassified;
    for (const auto& [label, n] : other.unclassified_labels) addLabel(unclassified_labels, label, n);
    no_disposition       += other.no_disposition;
    absorbed_no_absorber += other.absorbed_no_absorber;
}

// ---------------------------------------------------------------------------
// The table and the manifest record
// ---------------------------------------------------------------------------

std::vector<std::string> paramInventoryTable(const ParamInventoryTally& tally, const RxIfbRule& rule,
                                             const std::string& scope) {
    std::vector<std::string> out;
    const bool one_epoch = tally.epochs <= 1;
    const bool one_time = tally.week_first == tally.week_last && tally.tow_first == tally.tow_last;
    out.push_back(fmt::format(
        "{} ({}): {} station(s), {} assembled epoch(s), {} station-epoch(s); counts are state "
        "columns (absorbed: withheld states){}",
        scope,
        one_time ? fmtTime(tally.week_first, tally.tow_first)
                 : fmtTime(tally.week_first, tally.tow_first) + " to "
                       + fmtTime(tally.week_last, tally.tow_last),
        tally.nStations(), tally.epochs, tally.station_epochs,
        one_epoch ? "" : " summed over the epochs, stations distinct over the run"));

    // One line: label, kind, the count per disposition (each column as wide as its name),
    // stations, note.  A line without a note ends at its last count (no trailing blanks).
    const auto line = [](const std::string& label, const std::string& kind,
                         const std::array<std::string, kParamDispositions>& counts,
                         const std::string& stations, const std::string& note) {
        std::string s = fmt::format("{:<36} {:<18}", label, kind);
        for (std::size_t k = 0; k < kParamDispositions; ++k) {
            const std::size_t w = std::string(paramDispositionName(static_cast<ParamDisposition>(k))).size();
            s += fmt::format(" {:>{}}", counts[k], w);
        }
        s += fmt::format(" {:>8}  {}", stations, note);
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    };
    std::array<std::string, kParamDispositions> header;
    for (std::size_t k = 0; k < kParamDispositions; ++k)
        header[k] = paramDispositionName(static_cast<ParamDisposition>(k));
    out.push_back(line("class", "kind", header, "stations", "note"));

    std::vector<std::string> warnings;
    // Absorbed instances per (reason, kind), for the reason lines below the classes.
    std::array<long long, kAbsorbedReasons> abs_phase{}, abs_code{};
    for (const auto& [cls, cnt] : tally.classes) {
        std::array<std::string, kParamDispositions> counts;
        for (std::size_t k = 0; k < kParamDispositions; ++k) counts[k] = std::to_string(cnt.n[k]);
        const std::string label = paramClassLabel(cls);
        out.push_back(line(label, kindText(cls), counts, std::to_string(cnt.nStations()),
                           fullNote(cls, cnt, rule)));
        for (std::size_t k = 0; k < kAbsorbedReasons; ++k) {
            if (cls.kind == NetworkParamKind::SatPhaseBias) abs_phase[k] += cnt.absorbed_by[k];
            else                                            abs_code[k]  += cnt.absorbed_by[k];
        }
        const long long held = cnt.count(rule.disposition);
        if (RxIfbRule::governs(cls) && rule.drop && !rule.premise_holds && held > 0) {
            warnings.push_back(fmt::format(
                "WARNING: {}: held at its first-epoch seed at {} station(s) ({} column(s)); "
                "dropped because the satellite code biases are estimated ({}); premise ({}) does "
                "not hold on this run ({})",
                label, cnt.nStations(), held, rule.dropped_by, rule.premise, premiseFailure(rule)));
        }
    }
    // One line per gate that withheld something: its rule, its site and what took it up.
    for (std::size_t k = 0; k < kAbsorbedReasons; ++k) {
        if (abs_phase[k] == 0 && abs_code[k] == 0) continue;
        const AbsorbedReasonSpec& s = absorbedReasonSpec(static_cast<AbsorbedReason>(k));
        std::string by;
        if (abs_phase[k] > 0) by += fmt::format("satellite phase bias taken up by {}", s.by_phase);
        if (abs_code[k] > 0)
            by += fmt::format("{}satellite code bias taken up by {}", by.empty() ? "" : "; ", s.by_code);
        out.push_back(fmt::format("absorbed, {}: {} instance(s), withheld when {} ({}); {}",
                                  s.label, abs_phase[k] + abs_code[k], s.rule, s.site, by));
    }
    for (auto& w : warnings) out.push_back(std::move(w));

    out.push_back(fmt::format(
        "station-epochs that did not reach the split: {} (no usable observations {}, no "
        "measurement rows {}, state size mismatch {}, exception {})",
        tally.station_epochs_unreached, tally.unreached_absent, tally.unreached_no_rows,
        tally.unreached_state_mismatch, tally.unreached_exception));
    out.push_back(fmt::format("station-epochs rejected after the split: {}", tally.station_epochs_rejected));
    out.push_back(fmt::format("withheld states without their absorbing column at the split: {}",
                              tally.absorbed_no_absorber));
    out.push_back(fmt::format("columns without a disposition from the split: {}", tally.no_disposition));
    std::string un = fmt::format("unclassified: {} column(s)", tally.unclassified);
    if (tally.unclassified > 0) {
        std::size_t k = 0;
        for (const auto& [label, n] : tally.unclassified_labels) {
            if (k++ == kMaxUnclassifiedPrinted) { un += ", ..."; break; }
            un += fmt::format("{} {} ({})", k == 1 ? ":" : ",", label, n);
        }
    }
    out.push_back(std::move(un));
    return out;
}

std::string paramInventoryJson(const ParamInventoryTally& tally, const RxIfbRule& rule,
                               const std::string& scope) {
    std::string j = fmt::format(
        "{{\"record\":\"param_inventory\",\"scope\":\"{}\",\"first\":{{\"week\":{},\"tow\":{:.3f}}},"
        "\"last\":{{\"week\":{},\"tow\":{:.3f}}},\"epochs\":{},\"station_epochs\":{},"
        "\"station_epochs_rejected\":{},\"station_epochs_unreached\":{},"
        "\"unreached\":{{\"absent\":{},\"no_rows\":{},\"state_mismatch\":{},\"exception\":{}}},"
        "\"stations\":{},\"columns\":[",
        jsonEscape(scope), tally.week_first, tally.tow_first, tally.week_last, tally.tow_last,
        tally.epochs, tally.station_epochs, tally.station_epochs_rejected,
        tally.station_epochs_unreached, tally.unreached_absent, tally.unreached_no_rows,
        tally.unreached_state_mismatch, tally.unreached_exception, tally.nStations());
    for (std::size_t k = 0; k < kParamDispositions; ++k)
        j += fmt::format("{}\"{}\"", k ? "," : "", paramDispositionName(static_cast<ParamDisposition>(k)));
    j += "],\"classes\":[";
    bool first = true;
    for (const auto& [cls, cnt] : tally.classes) {
        j += fmt::format("{}{{\"kind\":\"{}\",\"form\":\"{}\",\"system\":\"{}\",\"signal\":\"{}\","
                         "\"signal_b\":\"{}\",\"band\":\"{}\",\"band_b\":\"{}\",\"label\":\"{}\","
                         "\"counts\":[",
                         first ? "" : ",", paramKindName(cls.kind), paramFormName(cls.form),
                         systemJson(cls.sys), signalText(cls, cls.signal),
                         signalText(cls, cls.signal_b), bandJson(cls.band), bandJson(cls.band_b),
                         jsonEscape(paramClassLabel(cls)));
        for (std::size_t k = 0; k < kParamDispositions; ++k)
            j += fmt::format("{}{}", k ? "," : "", cnt.n[k]);
        j += "],\"absorbed_reasons\":{";
        bool first_reason = true;
        for (std::size_t k = 0; k < kAbsorbedReasons; ++k) {
            if (cnt.absorbed_by[k] == 0) continue;
            j += fmt::format("{}\"{}\":{}", first_reason ? "" : ",",
                             absorbedReasonName(static_cast<AbsorbedReason>(k)), cnt.absorbed_by[k]);
            first_reason = false;
        }
        j += fmt::format("}},\"stations\":{},\"rule\":\"{}\"}}", cnt.nStations(),
                         RxIfbRule::governs(cls) ? "rx_ifb" : "");
        first = false;
    }
    j += fmt::format("],\"rules\":[{{\"rule\":\"rx_ifb\",\"drop\":{},\"disposition\":\"{}\","
                     "\"source\":\"{}\",\"dropped_by\":\"{}\",\"premise\":\"{}\","
                     "\"ionosphere_weighting\":{},\"w6_iono_constraint\":\"{}\",\"premise_holds\":{}}}],"
                     "\"reasons\":[",
                     rule.drop ? "true" : "false", paramDispositionName(rule.disposition),
                     jsonEscape(rule.source), jsonEscape(rule.dropped_by), jsonEscape(rule.premise),
                     rule.iono_weighting ? "true" : "false", w6IonoText(rule),
                     rule.premise_holds ? "true" : "false");
    // The reason table itself, so the record names what each absorbed count means.
    for (std::size_t k = 0; k < kAbsorbedReasons; ++k) {
        const AbsorbedReasonSpec& s = absorbedReasonSpec(static_cast<AbsorbedReason>(k));
        j += fmt::format("{}{{\"reason\":\"{}\",\"label\":\"{}\",\"rule\":\"{}\",\"site\":\"{}\","
                         "\"by_phase\":\"{}\",\"by_code\":\"{}\"}}",
                         k ? "," : "", s.name, jsonEscape(s.label), jsonEscape(s.rule),
                         jsonEscape(s.site), jsonEscape(s.by_phase), jsonEscape(s.by_code));
    }
    j += fmt::format("],\"absorbed_no_absorber\":{},\"no_disposition\":{},\"unclassified\":{},"
                     "\"unclassified_labels\":{{",
                     tally.absorbed_no_absorber, tally.no_disposition, tally.unclassified);
    first = true;
    for (const auto& [label, n] : tally.unclassified_labels) {
        j += fmt::format("{}\"{}\":{}", first ? "" : ",", jsonEscape(label), n);
        first = false;
    }
    j += "}}";
    return j;
}

// ---------------------------------------------------------------------------
// The seams
// ---------------------------------------------------------------------------

void networkParamInventoryDrain(NetworkParamInventory& inv, const std::vector<StationAssembly>& asm_res,
                                const std::vector<bool>& present, int week, double tow) {
    ParamInventoryTally t;
    for (std::size_t i = 0; i < asm_res.size(); ++i) {
        const StationAssembly& R = asm_res[i];
        if (!R.param_inv.filled) {                   // the station did not reach the split
            ++t.station_epochs_unreached;
            if (i >= present.size() || !present[i]) ++t.unreached_absent;
            else if (R.empty_innov)                 ++t.unreached_no_rows;
            else if (R.state_mismatch)              ++t.unreached_state_mismatch;
            else                                    ++t.unreached_exception;
            continue;
        }
        if (!R.ok || R.threw) {                      // rejected after the split: never solved
            ++t.station_epochs_rejected;
            continue;
        }
        t.addStation(static_cast<int>(i), R.param_inv);
    }
    if (t.station_epochs > 0 || t.station_epochs_rejected > 0) t.epochs = 1;
    t.week_first = t.week_last = week;
    t.tow_first  = t.tow_last  = tow;
    inv.whole_run.merge(t);          // ignored when the tally holds nothing
    inv.last_epoch = std::move(t);   // no class when no station reached the split
}

namespace {

void logTable(const std::shared_ptr<spdlog::logger>& log, const char* prefix,
              const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (line.rfind("WARNING", 0) == 0) log->warn("{}: {}", prefix, line);
        else                              log->info("{}: {}", prefix, line);
    }
}

}  // namespace

void networkParamInventoryEpochTail(NetworkRunContext& ctx, const std::shared_ptr<spdlog::logger>& log,
                                    NetworkRunTraces& traces, const NetworkConfig& nc, bool emitted) {
    NetworkParamInventory& inv = ctx.param_inventory;
    if (inv.first_table_written || !emitted || inv.last_epoch.station_epochs == 0) return;
    inv.first_table_written = true;
    logTable(log, "Network (parameters as armed)",
             paramInventoryTable(inv.last_epoch, inv.rx_ifb, "first emitted epoch"));
    traces.appendRunManifestRecord(nc, paramInventoryJson(inv.last_epoch, inv.rx_ifb, "first_emitted_epoch"));
}

void networkParamInventoryNoteSatellites(NetworkParamInventory& inv, const SyncedEpoch& se) {
    SatIdentityCensus& c = inv.sat_ids;
    ++c.epochs;
    for (const auto& [station, obs] : se.by_station) {
        (void)station;
        for (const RinexObservation& o : obs) {
            ++c.records;
            const System sys = systemOfSat(o.satellite);
            if (sys == System::Unknown)   c.unknown_system.insert(o.satellite);
            else if (sys == System::SBAS) c.sbas.insert(o.satellite);
        }
    }
}

void networkParamInventoryRunEnd(const NetworkRunContext& ctx, const std::shared_ptr<spdlog::logger>& log,
                                 NetworkRunTraces& traces, const NetworkConfig& nc) {
    const NetworkParamInventory& inv = ctx.param_inventory;
    logTable(log, "Network (parameters as armed, run end)",
             paramInventoryTable(inv.whole_run, inv.rx_ifb, "run"));
    traces.appendRunManifestRecord(nc, paramInventoryJson(inv.whole_run, inv.rx_ifb, "run"));

    // The satellite-id census (card NOM N0b): the ids of no modelled constellation and the
    // SBAS ids, with up to ten of each listed when any was counted.
    const SatIdentityCensus& c = inv.sat_ids;
    const auto listed = [](const std::set<std::string>& ids) {
        std::string out;
        std::size_t n = 0;
        for (const std::string& id : ids) {
            if (n++ == 10) { out += " ..."; break; }
            out += " '" + id + "'";
        }
        return out.empty() ? out : ":" + out;
    };
    const std::string line = fmt::format(
        "satellite ids in the synced observations over {} processed epoch(s) ({} satellite "
        "records): "
        "{} of no modelled constellation{} (they would share the '?' group of the maps keyed "
        "by a constellation letter); {} SBAS{} (they would reach the SBAS carrier wavelength "
        "of the emit re-base)",
        c.epochs, c.records, c.unknown_system.size(), listed(c.unknown_system), c.sbas.size(),
        listed(c.sbas));
    if (c.unknown_system.empty() && c.sbas.empty())
        log->info("Network (parameters as armed, run end): {}.", line);
    else
        log->warn("Network (parameters as armed, run end): {}.", line);
}

}  // namespace GnssProcessor
