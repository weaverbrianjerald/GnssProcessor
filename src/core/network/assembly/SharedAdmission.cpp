/**
 * @file SharedAdmission.cpp
 * @brief The shared satellite parameters a network station references (see
 *        network/assembly/SharedAdmission.hpp).
 */

#include "network/assembly/SharedAdmission.hpp"

#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/RowRules.hpp"

#include <cmath>
#include <optional>

namespace GnssProcessor {

namespace {

/// A shared parameter at value 0, ready to append.
LocalSlot sharedSlot(const ParamRef& ref) {
    LocalSlot slot;
    slot.ref = ref;
    return slot;
}

/// Whether the coverage allowlist admits a phase signal of a constellation: an empty list, a
/// constellation not in it, or a listed signal.
bool phaseBiasAdmitted(const StationRowInputs& inputs, const std::string& system,
                       const std::string& signal) {
    if (inputs.phase_bias_allowlist.empty()) return true;
    const auto it = inputs.phase_bias_allowlist.find(system);
    if (it == inputs.phase_bias_allowlist.end()) return true;
    return it->second.count(signal) > 0;
}

/// Notes a withheld shared parameter by the texts the gate saw.  The identity is typed the way
/// the parameter inventory reads a key: the satellite as an id, the signal as a signal of its
/// constellation; a text that does not read that way is listed unnamed, never dropped.
void noteWithheldText(StationParams& set, NetworkParamKind kind, const std::string& satellite,
                      const std::string& signal, AbsorbedReason reason) {
    const std::optional<SatId> sat = SatId::parse(satellite);
    const std::optional<SignalId> sig =
        sat ? SignalId::fromObsCode(sat->sys, signal) : std::nullopt;
    if (!sat || !sig) {
        const bool phase = kind == NetworkParamKind::SatPhaseBias;
        set.shared.withheld_unnamed.push_back(
            std::string("absorbed ") + (phase ? "sat_phase_bias_states:" : "sat_bias_states:")
            + (phase ? StateKeys::makeSatPhaseBiasKey(satellite, signal)
                     : StateKeys::makeSatBiasKey(satellite, signal)));
        return;
    }
    const ParamRef ref = kind == NetworkParamKind::SatPhaseBias ? satPhaseBiasRef(*sat, *sig)
                                                                : satCodeBiasRef(*sat, *sig);
    set.shared.withheld.push_back(AbsorbedParam{ref, reason});
}

/// Whether an observation's stored geometry passes the gates the row loop applies, without
/// logging (the row loop logs).
bool geometryAdmitted(const GnssConfig& config, const PppObservation& obs) {
    if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) return false;
    if (obs.range < 1.5e7 || !std::isfinite(obs.range)) return false;
    if (obs.elevation < config.getElevationMaskDeg()) return false;
    if (obs.los.norm() < 0.5 || !obs.los.allFinite()) return false;
    if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w)) return false;
    return true;
}

} // namespace

void noteWithheldPhaseBias(StationParams& set, const SatSignalId& id, AbsorbedReason reason) {
    set.shared.withheld.push_back(AbsorbedParam{satPhaseBiasRef(id.sat, id.signal), reason});
}

void networkAdmitSharedStates(const StationRowContext& c, StationParams& set,
                              const std::vector<PppObservation>& observations,
                              const std::set<std::string>& allowed_phase_keys,
                              const std::set<std::string>* cbadm_admitted_code_keys) {
    const GnssConfig& config = c.config;
    if (!config.isUncombinedEnabled()) return;   // the network is uncombined only

    // The record of what this station is not given this epoch starts empty here; the later
    // serial gates append to it.
    set.shared.withheld.clear();
    set.shared.withheld_unnamed.clear();

    // No row is carried-excluded in the network: the admission matches the row loop's.
    const PivotSignalMap no_pivot_filter;
    const std::set<std::string> no_exclusions;

    std::set<SatId>        required_clock;
    std::set<SatSignalId>  required_phase;
    std::set<SatSignalId>  required_code;

    for (const PppObservation& obs : observations) {
        // GLONASS is in the network product on the code-only scope: it gets a clock column,
        // but no shared per-signal phase bias (unphysical under FDMA).  Off by default.
        if (obs.system == System::GLONASS && !config.enableGlonassNetwork()) continue;

        // Mirror the row loop's observation gates, so every admitted column is written by a
        // row this epoch (no phantom all-zero column, no spurious null-space direction).
        if (!geometryAdmitted(config, obs)) continue;

        const std::optional<SatId> sat = canonicalSat(obs.satellite);
        bool sat_usable = false;
        for (const auto& entry : obs.observed) {
            const std::string& ref_type = entry.first;
            if (!isUsableMeasurement(config, c.log, no_pivot_filter, obs, ref_type, no_exclusions, ""))
                continue;
            if (ref_type.find('_') != std::string::npos) continue;   // raw signals only
            sat_usable = true;
            const std::optional<SignalId> signal =
                sat ? canonicalSignal(obs.system, ref_type) : std::nullopt;
            const MeasurementType type = classifyMeasurement(ref_type);

            // Per-signal satellite phase bias on raw phase rows.  Every gate below withholds
            // the shared column only; the phase row is still built and the station's float
            // ambiguity absorbs the bias.
            if (config.estimateSatPhaseBiases() && type == MeasurementType::Phase) {
                const std::string pk = StateKeys::makeSatPhaseBiasKey(obs.satellite, ref_type);
                if (obs.system == System::GLONASS) {
                    noteWithheldText(set, NetworkParamKind::SatPhaseBias, obs.satellite, ref_type,
                                     AbsorbedReason::GlonassCodeOnlyScope);
                    continue;
                }
                // Phase-bias coverage gate: a signal tracked too sparsely across the network
                // gets no phase bias.
                if (!phaseBiasAdmitted(c.inputs, PppUtils::getSystemString(obs.satellite), ref_type)) {
                    noteWithheldText(set, NetworkParamKind::SatPhaseBias, obs.satellite, ref_type,
                                     AbsorbedReason::CoverageGate);
                    continue;
                }
                // Legacy single-pivot datum: only the pivot station's keys are shared.
                if (!allowed_phase_keys.empty() && !allowed_phase_keys.count(pk)) {
                    noteWithheldText(set, NetworkParamKind::SatPhaseBias, obs.satellite, ref_type,
                                     AbsorbedReason::Allowlist);
                    continue;
                }
                // Code-census co-gate: when the census quarantined this signal's code pair,
                // its phase bias is withheld as well, so no pure phase-bias gauge is left
                // behind.  The companion code key is the same satellite and signal with the
                // observation type letter changed.
                if (cbadm_admitted_code_keys != nullptr) {
                    std::string code_signal = ref_type;
                    if (!code_signal.empty()) code_signal[0] = 'C';
                    if (!cbadm_admitted_code_keys->count(obs.satellite + "|" + code_signal)) {
                        noteWithheldText(set, NetworkParamKind::SatPhaseBias, obs.satellite, ref_type,
                                         AbsorbedReason::CodeCensusCogate);
                        continue;
                    }
                }
                if (sat && signal) {
                    const SatSignalId id{*sat, *signal};
                    required_phase.insert(id);
                    if (set.shared.sat_phase_bias.count(id) == 0)
                        addParam(set, sharedSlot(satPhaseBiasRef(*sat, *signal)));
                }
            }

            // Per-signal satellite code bias on raw code rows: a shared column for every
            // constellation but GLONASS, allocated at 0 (an absolute parameter).
            if (config.estimateNetworkSatCodeBias() && type == MeasurementType::Code) {
                if (obs.system == System::GLONASS) {
                    noteWithheldText(set, NetworkParamKind::SatCodeBias, obs.satellite, ref_type,
                                     AbsorbedReason::GlonassCodeOnlyScope);
                } else if (sat && signal) {
                    const SatSignalId id{*sat, *signal};
                    required_code.insert(id);
                    if (set.shared.sat_code_bias.count(id) == 0)
                        addParam(set, sharedSlot(satCodeBiasRef(*sat, *signal)));
                }
            }
        }

        // One clock per satellite with any usable raw measurement.
        if (config.estimateSatClocks() && sat_usable && sat) {
            required_clock.insert(*sat);
            if (set.shared.sat_clock.count(*sat) == 0) addParam(set, sharedSlot(satClockRef(*sat)));
        }
    }

    // Drop the shared parameters whose satellite or signal is not usable this epoch, so the
    // station's columns are exactly this epoch's.
    if (config.estimateSatClocks()) pruneSatClockExcept(set, required_clock);
    if (config.estimateSatPhaseBiases()) pruneSatPhaseBiasExcept(set, required_phase);
    if (config.estimateNetworkSatCodeBias()) pruneSatCodeBiasExcept(set, required_code);
}

} // namespace GnssProcessor
