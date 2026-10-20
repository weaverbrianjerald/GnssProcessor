/**
 * @file ParamInventory.cpp
 * @brief The network parameter inventory (see ParamInventory.hpp).
 */
#include "network/assembly/ParamInventory.hpp"

#include "network/state/RunContext.hpp"     // NetworkRunContext::param_inventory
#include "network/driver/SetupResult.hpp"    // NetworkSetupResult::stations (the band-5 receiver-delay run-end report)
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"    // StationAssembly::param_inv, ok, threw
#include "network/state/StationParams.hpp"   // receiverCodeBiasKey (the receiver IFB rule)
#include "network/state/StationParams.hpp"    // StationParams (the inventory records its columns)
#include "network/diag/RunTraces.hpp"  // appendRunManifestRecord
#include "model/PppModel.hpp"            // getAllowedBandDigits (rxExtraBandTracked)
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun

namespace GnssProcessor {

namespace {

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
        if (rule.estimated_by_knob)   // the drop turned off by its own knob
            return fmt::format("rule rx_ifb: estimated ({}); not dropped by {}",
                               rule.estimated_by, rule.dropped_by);
        return fmt::format("rule rx_ifb: not armed ({} off), estimated", rule.dropped_by);
    }
    return {};
}

/// The absorbed part of a class's note, e.g. "absorbed: coverage allowlist 5"; "" when the
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


// ---------------------------------------------------------------------------
// Absorbed instances
// ---------------------------------------------------------------------------

namespace {

/// What takes up a withheld satellite phase bias, at every gate: the phase row is still
/// built (the observation equation writes the phase-bias column only when the state exists), so
/// the station's float ambiguity of that satellite and signal carries the bias.
constexpr const char* kByAmbiguity = "the station's float ambiguity of the same satellite and signal";

/// The reason table, in enum order (the static_assert below).  Each row was read against
/// the code it names.
constexpr AbsorbedReasonSpec kAbsorbedReasonTable[kAbsorbedReasons] = {
    {AbsorbedReason::GlonassCodeOnlyScope, "glonass_code_only_scope", "GLONASS code-only scope",
     "GLONASS is admitted on code-only scope (network.estimate_glonass): a shared "
     "per-(satellite, signal) bias is unphysical under FDMA, so none is allocated",
     "networkAdmitSharedStates (SharedAdmission.cpp)", kByAmbiguity,
     // The code comment names the per-slot IFB alone; that column exists only on the FDMA
     // bands at a known slot k != 0 (ReceiverBiasResolver::resolveReceiverBias).  The case does
     // not arise in a loaded run: the loader refuses network.estimate_sat_code_bias with
     // network.estimate_glonass.
     "the station's receiver per-slot IFB (FDMA bands, slot k != 0 only; not reachable in a "
     "loaded run, which refuses network.estimate_sat_code_bias with network.estimate_glonass)"},
    {AbsorbedReason::CoverageGate, "coverage_gate", "coverage allowlist",
     "the signal is outside the coverage allowlist built from "
     "network.phase_bias_min_station_fraction",
     "networkAdmitSharedStates (SharedAdmission.cpp)", kByAmbiguity, ""},
    {AbsorbedReason::Allowlist, "allowlist", "pivot allowlist",
     "the legacy single-pivot datum is in force (network.phase_datum_prioritized off) and the "
     "key is not among the pivot station's satellite phase-bias keys",
     "networkAdmitSharedStates (SharedAdmission.cpp)", kByAmbiguity, ""},
    {AbsorbedReason::CodeCensusCogate, "code_census_cogate", "code-census co-gate",
     "the CB-ADM census (network.cb_admission) quarantined the signal's companion code pair",
     "networkAdmitSharedStates (SharedAdmission.cpp)", kByAmbiguity, ""},
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


// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

RxIfbRule networkRxIfbRule(const GnssConfig& config) {
    RxIfbRule r;
    // network.estimate_rx_extra_band_code_delay turns the drop off where it can
    // act.  Unarmed, or armed where it cannot act, this is the former single assignment
    // (drop = estimateNetworkSatCodeBias()).
    r.extra_band_tracked = rxExtraBandTracked(config.getFrequencyMode(), config.getActiveSystems());
    r.estimated_by_knob  = rxExtraBandDelayEffective(config);
    r.drop               = config.estimateNetworkSatCodeBias() && !r.estimated_by_knob;
    r.iono_weighting = config.getIonosphereWeightingConfig().enabled;
    r.w6_iono_none   = config.getNetworkConfig().w6_iono_constraint == NetworkW6IonoConstraint::None;
    r.premise_holds  = r.iono_weighting && !r.w6_iono_none;
    return r;
}

bool rxExtraBandTracked(FrequencyMode mode, const std::vector<std::string>& active_systems) {
    for (const std::string& name : active_systems) {
        const System sys = systemFromName(name);
        if (sys == System::Unknown || traitsOf(sys).fdma) continue;
        if (getAllowedBandDigits(systemName(sys), mode).size() > 2) return true;
    }
    return false;
}

bool rxExtraBandDelayEffective(const GnssConfig& config) {
    return config.getNetworkConfig().estimate_rx_extra_band_code_delay
        && config.estimateNetworkSatCodeBias()
        && rxExtraBandTracked(config.getFrequencyMode(), config.getActiveSystems());
}

bool rxIfbFamilyReseeded(const RxIfbStationState& s, const std::string& ifb_key,
                         const std::set<std::string>& reseed_channels) {
    if (reseed_channels.empty()) return false;
    const auto sit = s.signals.find(ifb_key);
    if (sit == s.signals.end()) return false;
    const std::optional<RxCbKey> key = receiverCodeBiasKey(ifb_key);
    if (!key || !RxIfbRule::governs(*key)) return false;
    const std::string cst = constellationKey(key->sys);
    for (const std::string& sig : sit->second)
        if (reseed_channels.count(cst + "|" + sig)) return true;
    return false;
}


// ---------------------------------------------------------------------------
// Per-station record
// ---------------------------------------------------------------------------

void recordStationParams(const StationParams& set, int station, StationParamSlot& slot) {
    slot.records.clear();
    slot.unclassified         = set.unclassified;
    slot.no_disposition       = 0;
    slot.absorbed_no_absorber = set.shared.absorbed_no_absorber;
    slot.filled = true;
    const StationId id{station};
    // The columns of the set with the disposition the split just gave them.  One past the end
    // of the split's dispositions is counted, never given a disposition by default.
    for (std::size_t k = 0; k < set.slots.size(); ++k) {
        const LocalSlot& s = set.slots[k];
        if (k >= set.decided) {
            ++slot.no_disposition;
            continue;
        }
        StationParamRecord rec;
        rec.id   = paramInstanceOf(s.ref, id);
        rec.disp = s.disp;
        slot.records.push_back(rec);
    }
    // The shared states the gates withheld from the station (no column), each absorbed by a
    // column the station still holds.
    for (const AbsorbedParam& a : set.shared.absorbed) {
        StationParamRecord rec;
        rec.id     = paramInstanceOf(a.ref, id);
        rec.disp   = ParamDisposition::Absorbed;
        rec.reason = a.reason;
        slot.records.push_back(rec);
    }
}


// ---------------------------------------------------------------------------
// Classes and tallies
// ---------------------------------------------------------------------------

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
                     "\"ionosphere_weighting\":{},\"w6_iono_constraint\":\"{}\",\"premise_holds\":{}",
                     rule.drop ? "true" : "false", paramDispositionName(rule.disposition),
                     jsonEscape(rule.source), jsonEscape(rule.dropped_by), jsonEscape(rule.premise),
                     rule.iono_weighting ? "true" : "false", w6IonoText(rule),
                     rule.premise_holds ? "true" : "false");
    // The knob that turned the drop off, only when it did (an unarmed record's
    // bytes are unchanged).
    if (rule.estimated_by_knob)
        j += fmt::format(",\"estimated_by\":\"{}\"", jsonEscape(rule.estimated_by));
    j += "}],\"reasons\":[";
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

void networkParamInventoryEpochTail(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const NetworkConfig& nc = run.nc;
    bool emitted = ep.emitted;
    // ---- End of the bindings ----
    NetworkParamInventory& inv = ctx.param_inventory;
    if (inv.first_table_written || !emitted || inv.last_epoch.station_epochs == 0) return;
    inv.first_table_written = true;
    logTable(log, "Network (parameters as armed)",
             paramInventoryTable(inv.last_epoch, inv.rx_ifb, "first emitted epoch"));
    traces.appendRunManifestRecord(nc, paramInventoryJson(inv.last_epoch, inv.rx_ifb, "first_emitted_epoch"));
}

void networkParamInventoryNoteSatellites(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkParamInventory& inv = run.ctx.param_inventory;
    const SyncedEpoch& se = ep.se;
    // ---- End of the bindings ----
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

    // The satellite-id census: the ids of no modelled constellation and the
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

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    /**
     * @brief Run-end section: parameters as armed (NOM N1).
     *
     * The table's row for "parameters as armed" and its manifest record; the
     * report is the overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
static int networkParamInventoryTableRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;

    // The instances of every kind over the run, with their treatments
    // ("parameters as armed", the run's table) and its manifest record.
    networkParamInventoryRunEnd(ctx, log, traces, config_.getNetworkConfig());
    return 0;
}

// ---- End of the run-end reports ----

// ---- Run-end reports added after the cut (hand-written; not in scripts/nom_td_manifest.json,
//      so scripts/nom_td_verify.py names their table rows instead of proving them) ----

// Run-end report: the receiver extra-band code delay, as lines.
std::vector<RxIfbReportLine> rxExtraBandDelayReport(
    const RxIfbRule& rule, const ParamInventoryTally& whole_run,
    const std::vector<std::pair<std::string, const RxIfbStationState*>>& stations,
    const std::map<std::pair<int, std::string>, double>& carried,
    bool carry_on, bool in_block) {
    std::vector<RxIfbReportLine> out;
    const auto info = [&](std::string s) { out.push_back({false, std::move(s)}); };
    const auto warn = [&](std::string s) { out.push_back({true, std::move(s)}); };

    // 1. Inert: the rule kept its drop (or had none to turn off).
    if (!rule.estimated_by_knob) {
        warn(fmt::format(
            "network.estimate_rx_extra_band_code_delay was ARMED and INERT: {}, so the rule "
            "rx_ifb did not change on this run.",
            !rule.extra_band_tracked
                ? std::string("no active constellation tracks a band outside a two-band pair "
                              "under processing.frequency_mode (no receiver IFB column exists)")
                : fmt::format("{} is off (no receiver IFB column is dropped)", rule.dropped_by)));
        return out;
    }

    // 2. The split's dispositions of the governed classes, from the inventory.
    long long est_total = 0;
    for (const auto& [cls, cnt] : whole_run.classes) {
        if (!RxIfbRule::governs(cls)) continue;
        const long long est  = cnt.count(ParamDisposition::EstimatedLocal);
        const long long unob = cnt.count(ParamDisposition::Unobserved);
        const long long held = cnt.count(ParamDisposition::HeldAtSeed);
        est_total += est;
        info(fmt::format("{}: estimated in the local block on {} station-epoch(s); unobserved "
                         "on {} (no row informed it: an all-zero H column, or only rows "
                         "without a shared satellite column); held at the seed on {}; {} "
                         "station(s).",
                         paramClassLabel(cls), est, unob, held, cnt.nStations()));
        if (held > 0)
            warn(fmt::format("{}: {} station-epoch(s) HELD at the seed while the knob is "
                             "effective: the split dropped a column the rule no longer drops "
                             "(a defect).", paramClassLabel(cls), held));
    }
    if (est_total == 0) {
        warn("network.estimate_rx_extra_band_code_delay is armed and effective, but no "
             "receiver IFB column was estimated over the whole run: no station brought a code "
             "outside its clock-reference pair into the solve (check processing.frequency_mode "
             "and processing.signal_priorities).  The knob was inert on this run.");
        return out;
    }

    // 3 and 4. The per-station counts, summed; the stations the carry never folded.
    RxIfbStationState sum;
    std::vector<std::string> never_folded;
    for (const auto& [name, s] : stations) {
        if (!s) continue;
        sum.est_epochs        += s->est_epochs;
        sum.rows_unregistered += s->rows_unregistered;
        sum.cols_unregistered += s->cols_unregistered;
        sum.reg_edges         += s->reg_edges;
        sum.dereg_edges       += s->dereg_edges;
        sum.recouple          += s->recouple;
        sum.blk_folds         += s->blk_folds;
        sum.diag_folds        += s->diag_folds;
        sum.diag_innov_skips  += s->diag_innov_skips;
        sum.reseed_erase      += s->reseed_erase;
        // The first estimate seeds the carry at the harvest, so a station estimated on one
        // epoch is never folded by design; from the second on, a zero is the carry not
        // acting (withheld every epoch, or never warm).
        if (carry_on && s->est_epochs > 1 && s->blk_folds + s->diag_folds == 0)
            never_folded.push_back(name);
    }
    info(fmt::format("{} extra-band code row(s) excluded at zero weight (no shared column for "
                     "the row's satellite code bias: held by fewer than two stations, or a "
                     "CB-ADM sponge); {} station-epoch(s) whose IFB column had no other row "
                     "(unobserved); {} registration and {} deregistration edge(s) of such rows.",
                     sum.rows_unregistered, sum.cols_unregistered, sum.reg_edges,
                     sum.dereg_edges));
    if (!carry_on) {
        info("carry: off (network.carry_rx_code_bias): the receiver IFB is re-estimated every "
             "epoch from that epoch's rows.");
    } else {
        info(fmt::format(
            "carry: {}; {} re-coupling(s) after at least one epoch out of the block with a live "
            "entry (their coupling row is gauge-restored per (constellation, kind), not per "
            "signal); {} carry entry erase(s) because the family's satellite sum-to-zero "
            "re-seeded.",
            in_block ? fmt::format("{} fold(s) as rows of a factorized station block "
                                   "(network.carry_rx_code_bias_in_block)", sum.blk_folds)
                     : fmt::format("{} diagonal fold(s), {} withheld by the inject screen "
                                   "(network.carry_rx_code_bias, diagonal route)",
                                   sum.diag_folds, sum.diag_innov_skips),
            sum.recouple, sum.reseed_erase));
        if (!never_folded.empty()) {
            std::string names;
            for (std::size_t k = 0; k < never_folded.size() && k < 20; ++k)
                names += (k ? ", " : "") + never_folded[k];
            if (never_folded.size() > 20) names += ", ...";
            warn(fmt::format("the receiver code-bias carry never folded the receiver IFB at {} "
                             "station(s) estimated on more than one epoch ({}): the column was "
                             "re-estimated from each epoch's rows alone there.",
                             never_folded.size(), names));
        }
    }

    // 5. The carried values at run end, per (key, family): the level and the spread.
    std::map<std::string, std::vector<double>> by_family;
    for (const auto& [sk, value] : carried) {
        if (!std::isfinite(value)) continue;
        const std::optional<RxCbKey> key = receiverCodeBiasKey(sk.second);
        if (!key || !RxIfbRule::governs(*key)) continue;
        std::string family;
        const int i = sk.first;
        if (i >= 0 && static_cast<std::size_t>(i) < stations.size() && stations[i].second) {
            const auto fit = stations[i].second->signals.find(sk.second);
            if (fit != stations[i].second->signals.end())
                for (const std::string& sig : fit->second)
                    family += (family.empty() ? "" : "+") + sig;
        }
        by_family[sk.second + " (" + (family.empty() ? "?" : family) + ")"].push_back(value);
    }
    const auto rank = [](const std::vector<double>& s, double q) {
        const std::size_t k = static_cast<std::size_t>(std::ceil(q * static_cast<double>(s.size())));
        return s[std::min(s.size() - 1, k > 0 ? k - 1 : 0)];
    };
    for (auto& [fam, vals] : by_family) {
        std::sort(vals.begin(), vals.end());
        const double med = rank(vals, 0.5);
        std::vector<double> dev;
        dev.reserve(vals.size());
        for (const double x : vals) dev.push_back(std::abs(x - med));
        std::sort(dev.begin(), dev.end());
        info(fmt::format("{} carried at {} station(s) at run end: median {:.3f} m (a level the "
                         "satellite-side datum realizes, not gradeable on its own); "
                         "|value - median| p90 {:.3f} m, max {:.3f} m (the between-station "
                         "spread, datum-invariant).",
                         fam, vals.size(), med, rank(dev, 0.9), dev.back()));
    }
    if (carry_on && by_family.empty())
        info("no receiver IFB entry in the receiver code-bias carry at run end (pruned past its "
             "gap horizon, or never harvested).");
    return out;
}

    /**
     * @brief Run-end section: the receiver extra-band code delay.
     *
     * The ON-path telemetry (invariants #4 and #7) of
     * @c network.estimate_rx_extra_band_code_delay: @ref rxExtraBandDelayReport over the
     * inventory's run tally, every station's @ref RxIfbStationState and the receiver
     * code-bias carry at run end.
     *
     * Gate: @c network.estimate_rx_extra_band_code_delay armed; prints nothing otherwise,
     * so an unarmed run's log is unchanged.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
static int networkRxExtraBandDelayRunEnd(const NetworkRunEndView& v) {
    if (!v.config.getNetworkConfig().estimate_rx_extra_band_code_delay) return 0;
    const NetworkRunContext& ctx = v.ctx;
    std::vector<std::pair<std::string, const RxIfbStationState*>> stations;
    stations.reserve(v.setup.stations.size());
    for (const NetStation& st : v.setup.stations) stations.emplace_back(st.name, &st.l5b);
    std::map<std::pair<int, std::string>, double> carried;
    net::forEach(ctx.entities.stations, &net::StationState::signals, &net::StationSignalState::rx_cb,
                 [&](const StationId& id, const std::string& key, const LocalCarry& lc) {
                     carried[{id.index, key}] = lc.value_abs;
                 });
    for (const RxIfbReportLine& l :
         rxExtraBandDelayReport(ctx.param_inventory.rx_ifb, ctx.param_inventory.whole_run,
                                stations, carried, ctx.knobs.rx_cb_on, ctx.knobs.rxcb_blk)) {
        if (l.warn) v.log->warn("Network: {}", l.text);
        else        v.log->info("Network (receiver extra-band code delay): {}", l.text);
    }
    return 0;
}

// Run-end report of this subsystem: its sections in print order (
// declared in network/assembly/ParamInventory.hpp).
int networkParamInventoryRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkRxExtraBandDelayRunEnd,
        networkParamInventoryTableRunEnd
    });
}

}  // namespace GnssProcessor
