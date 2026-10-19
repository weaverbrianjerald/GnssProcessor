/**
 * @file LocalBirth.cpp
 * @brief The births, re-seeds and prunes of a network station's local parameters, and the
 *        first-epoch seeds (see network/assembly/LocalBirth.hpp).
 *
 * The functions below are the steps of @ref networkBirthLocalStates, in its order.  Each
 * opens with the bindings it uses and reads the set through @ref NetView, so a seed sees the
 * parameters born before it in the same epoch.
 */

#include "network/assembly/LocalBirth.hpp"

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/RowRules.hpp"
#include "measurement/Seeds.hpp"
#include "model/PppModel.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

namespace {

/// What the epoch needs of the station's local states, collected from its observations.
struct RequiredLocal {
    std::set<SatSignalId>   ambiguities;  ///< The phase signals observed: the ambiguity keep set.
    std::set<SatId>         iono_sats;    ///< The satellites with a code or phase signal: the ionosphere keep set.
    std::map<SatId, double> colour_sats;  ///< The satellites with a phase signal, with their elevation.
};

/// A parameter at a value, ready to append.
LocalSlot bornSlot(const ParamRef& ref, double x) {
    LocalSlot slot;
    slot.ref   = ref;
    slot.x_lin = x;
    return slot;
}

/// Sets the value of the slot a parameter holds (no effect when the set does not hold it).
void setValue(StationParams& set, const ParamRef& ref, double x) {
    const int k = findParam(set, ref);
    if (k >= 0) set.slots[static_cast<std::size_t>(k)].x_lin = x;
}

/// Collects what the epoch needs of the station's local states.
RequiredLocal collectRequired(const GnssConfig& config, const std::vector<PppObservation>& observations) {
    RequiredLocal need;
    const bool colour_on = config.getNetworkConfig().phase_colour_state;
    for (const PppObservation& obs : observations) {
        const std::optional<SatId> sat = canonicalSat(obs.satellite);
        if (!sat) continue;
        for (const auto& entry : obs.observed) {
            const std::string& ref_type = entry.first;
            if (ref_type.empty()) continue;
            if (!obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;
            if (ref_type[0] == 'C' || ref_type[0] == 'L') need.iono_sats.insert(*sat);
            if (ref_type[0] != 'L') continue;
            const std::optional<SignalId> phase = canonicalSignal(obs.system, ref_type);
            if (!phase) continue;   // an iono-free pair has no raw ambiguity
            need.ambiguities.insert(SatSignalId{*sat, *phase});
            if (colour_on) need.colour_sats[*sat] = obs.elevation;
        }
    }
    return need;
}

/// The inter-system biases from the first epoch's code; false while the time reference has
/// no code observation (the seed is retried next epoch).
bool seedInterSystemBiases(const StationRowContext& c, StationParams& set,
                           const std::vector<PppObservation>& observations) {
    const NetView view(set);
    const IsbSeeds result = seedIsb(residualSeedObsInput(c), ParamLookup(view), observations);
    auto log = c.log.getLogger("state_initializer");
    if (!result.settled) {
        log->warn("Cannot initialize ISB: no GPS code observations");
        return false;
    }
    for (const IsbSeed& seed : result.seeds) {
        setValue(set, isbRef(seed.sys), seed.value_m);
        log->info("Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
                  systemName(seed.sys), seed.value_m, seed.value_m / Constants::SPEED_OF_LIGHT,
                  seed.n_obs, result.reference_obs);
    }
    return true;
}

/// A slant ionosphere for each satellite that has code or phase, seeded from the code, then
/// the prune of the satellites that have none.
void birthSlantIonosphere(const StationRowContext& c, StationParams& set,
                          const std::vector<PppObservation>& observations,
                          const RequiredLocal& need) {
    const ObsInput seed_input = residualSeedObsInput(c);
    const auto has_entry = [&](const std::string& satellite, const std::string& signal) {
        return c.model.hasBiasEntry(satellite, signal);
    };
    for (const PppObservation& obs : observations) {
        const std::optional<SatId> sat = canonicalSat(obs.satellite);
        if (!sat || need.iono_sats.count(*sat) == 0 || set.iono.count(*sat) != 0) continue;
        // With no .BIA-covered code pair the seed would carry raw receiver code biases.
        // It is 0 then; the value is only a linearization point.
        const std::optional<IonosphereSeed> seed = seedIonosphere(seed_input, obs, has_entry);
        if (seed.has_value())
            c.log.getLogger("state_initializer")->debug(
                "Iono seed for {} from {}/{}: P1-mod1={:.3f}, P2-mod2={:.3f}, "
                "alpha1={:.4f}, alpha2={:.4f}, seed={:.3f} m",
                obs.satellite, seed->code_a, seed->code_b, seed->range_a_m, seed->range_b_m,
                seed->alpha_a, seed->alpha_b, seed->value_m);
        addParam(set, bornSlot(slantIonoRef(*sat), seed ? seed->value_m : 0.0));
    }
    pruneSlantIonoExcept(set, need.iono_sats);
}

/// A phase colour (value 0) for each satellite with a phase signal, its elevation recorded
/// for the prior law, then the prune of the others.  Off without the phase-colour state.
void birthPhaseColour(const StationRowContext& c, StationParams& set, const RequiredLocal& need) {
    if (!c.config.getNetworkConfig().phase_colour_state) return;
    std::set<SatId> keep;
    for (const auto& entry : need.colour_sats) {
        keep.insert(entry.first);
        if (set.colour.count(entry.first) == 0)
            addParam(set, bornSlot(phaseColourRef(entry.first), 0.0));
        set.slots[static_cast<std::size_t>(set.colour.at(entry.first))].elevation_deg = entry.second;
    }
    prunePhaseColourExcept(set, keep);
}

/// The receiver clock from the time reference's code, once, while it is cold.  Opt-in.
void seedColdClockOnce(const StationRowContext& c, StationParams& set,
                       const std::vector<PppObservation>& observations) {
    if (!c.config.networkColdClockSeed() || set.seeded.cold_clock) return;
    const NetView view(set);
    const ColdClockSeed seed = seedColdClock(residualSeedObsInput(c), ParamLookup(view), observations);
    auto log = c.log.getLogger("state_initializer");
    switch (seed.outcome) {
        case ColdClockOutcome::NoClockState:
            break;   // no clock in this configuration: nothing to seed or retry
        case ColdClockOutcome::AlreadySeeded:
            log->debug("Cold receiver-clock seed skipped: clock already {:.3f} m (not cold)",
                       seed.current_m);
            break;
        case ColdClockOutcome::NoReference:
            log->warn("Cannot pre-seed cold receiver clock: no GPS code observations this epoch");
            break;   // retry next epoch; do not latch
        case ColdClockOutcome::Seeded:
            setValue(set, rxClockRef(), seed.value_m);
            log->info("Pre-seeded cold receiver clock: {:.3f} m ({:.3e} s) from {} GPS code obs",
                      seed.value_m, seed.value_m / Constants::SPEED_OF_LIGHT, seed.n_obs);
            break;
    }
    if (seed.settled()) set.seeded.cold_clock = true;
}

/// A float ambiguity for each phase signal observed: born at the observation less the known
/// terms, or given a new value when marked for a re-seed (the mark is consumed).
void birthAmbiguities(const StationRowContext& c, StationParams& set,
                      const std::vector<PppObservation>& observations, const RequiredLocal& need) {
    const ObsInput seed_input = ambiguitySeedObsInput(c);
    for (const PppObservation& obs : observations) {
        const std::optional<SatId> sat = canonicalSat(obs.satellite);
        if (!sat) continue;
        for (const auto& entry : obs.observed) {
            const std::string& ref_type = entry.first;
            if (ref_type.empty() || ref_type[0] != 'L') continue;
            const std::optional<SignalId> phase = canonicalSignal(obs.system, ref_type);
            if (!phase) continue;
            const SatSignalId id{*sat, *phase};
            if (need.ambiguities.count(id) == 0) continue;

            const auto held = set.amb.find(id);
            if (held != set.amb.end() && !pendingReseed(set, id)) continue;

            const NetView view(set);
            const double seed = seedAmbiguity(seed_input, ParamLookup(view), obs, ref_type);
            if (held == set.amb.end()) addParam(set, bornSlot(ambiguityRef(*sat, *phase), seed));
            else                       set.slots[static_cast<std::size_t>(held->second)].x_lin = seed;
            clearPendingReseed(set, id);
        }
    }
}

/// Warns when a satellite holds more ambiguity bands than the frequency mode admits: some
/// upstream path bypassed the band filter.
void warnAmbiguityBandCount(const StationRowContext& c, const RequiredLocal& need) {
    int expected_bands = 0;
    switch (c.config.getFrequencyMode()) {
        case FrequencyMode::Dual:        expected_bands = 2; break;
        case FrequencyMode::Triple:      expected_bands = 3; break;
        case FrequencyMode::All:         expected_bands = 0; break;   // 0 = no cap
        case FrequencyMode::Independent: expected_bands = 0; break;   // per-satellite variable
    }
    if (expected_bands == 0) return;
    std::map<SatId, std::set<char>> per_sat_bands;
    for (const SatSignalId& id : need.ambiguities) per_sat_bands[id.sat].insert(id.signal.digit);
    for (const auto& entry : per_sat_bands) {
        if (static_cast<int>(entry.second.size()) <= expected_bands) continue;
        std::string band_list;
        for (const char b : entry.second) {
            if (!band_list.empty()) band_list += ',';
            band_list += b;
        }
        c.log.getLogger("measurement_model")->warn(
            "Sat {} has {} ambiguity bands {{{}}} - exceeds the frequency mode's cap of {}.  "
            "An upstream path bypassed the band filter (likely a slip detector reset key on "
            "a band-filtered signal).",
            entry.first.str(), entry.second.size(), band_list, expected_bands);
    }
}

/// Marks the ambiguities the integer search leaves out: those whose signal has no applied
/// phase bias.  Every mark of the previous epoch is cleared first.
void markUnresolvable(const StationRowContext& c, StationParams& set,
                      const std::vector<PppObservation>& observations, const RequiredLocal& need) {
    for (const auto& entry : set.amb) set.slots[static_cast<std::size_t>(entry.second)].unresolvable = false;
    const RowProducts products{c.config, c.model, SsrCorrectionStore::getInstance()};
    for (const PppObservation& obs : observations) {
        const std::optional<SatId> sat = canonicalSat(obs.satellite);
        if (!sat) continue;
        for (const auto& entry : obs.observed) {
            const std::string& ref_type = entry.first;
            if (ref_type.empty() || ref_type[0] != 'L') continue;
            const std::optional<SignalId> phase = canonicalSignal(obs.system, ref_type);
            if (!phase) continue;
            const SatSignalId id{*sat, *phase};
            if (need.ambiguities.count(id) == 0) continue;
            if (phaseBiasCoveredForAr(products, obs.satellite, ref_type, obs.time)) continue;
            const auto held = set.amb.find(id);
            if (held != set.amb.end())
                set.slots[static_cast<std::size_t>(held->second)].unresolvable = true;
            c.log.getLogger("measurement_model")->trace(
                "Unresolvable ambiguity {}_{}: no applied phase bias (.BIA or network/SSR)",
                obs.satellite, ref_type);
        }
    }
}

/// A receiver code bias (value 0) for each code signal the receiver-bias rule names, then the
/// prune of the others.  The GLONASS network keeps only the per-slot inter-frequency biases:
/// a bias on another band has no reference code to anchor the slant ionosphere.
void birthReceiverCodeBiases(const StationRowContext& c, StationParams& set,
                             const std::vector<PppObservation>& observations) {
    std::set<RxCbKey> required;
    if (c.config.estimateIfb() && c.config.isUncombinedEnabled()) {
        const ClockRefMap& clock_refs = ProductCache::getInstance().getClockRefSignals();
        for (const PppObservation& obs : observations) {
            for (const auto& entry : obs.observed) {
                const std::string& ref_type = entry.first;
                if (ref_type.empty() || ref_type[0] != 'C') continue;
                const ReceiverBiasEntry bias = c.inputs.rx_bias.resolveReceiverBias(
                    ref_type, obs.system, obs.satellite, clock_refs);
                if (!std::isfinite(bias.coefficient) || !bias.has_key) continue;
                if (c.config.enableGlonassNetwork() && bias.key.form != ParamForm::GloSlotIfb) continue;
                if (!required.insert(bias.key).second) continue;
                if (set.rx_cb.count(bias.key) == 0) addParam(set, bornSlot(rxCodeBiasRef(bias.key), 0.0));
            }
        }
    }
    pruneReceiverCodeBiasExcept(set, required);
}

/// The receiver code biases from the first epoch's code, or from the points a warm resume
/// installed.  Counts the outcome of each covered key into the station's out-slots.
void seedReceiverCodeBiases(const StationRowContext& c, StationParams& set,
                            const std::vector<PppObservation>& observations) {
    if (!c.config.estimateIfb() || !c.config.isUncombinedEnabled()) return;
    const NetView view(set);
    const CodeBiasSeeds result = seedCodeBias(residualSeedObsInput(c), ParamLookup(view), observations,
                                              set.rx_cb_restore.by_key);
    const auto note = [&](bool applied) {
        long long* slot = applied ? set.rx_cb_restore.applied : set.rx_cb_restore.uncovered;
        if (slot != nullptr) ++*slot;
    };
    auto log = c.log.getLogger("state_initializer");
    for (std::size_t i = 0; i < result.refused; ++i) note(false);
    for (const CodeBiasSeed& seed : result.seeds) {
        setValue(set, rxCodeBiasRef(seed.key), seed.value_m);
        if (seed.restored) {
            note(true);
            log->info("Receiver code bias seeded from the restored linearization point: {:.6f} m "
                      "(the one-epoch code median is not used)", seed.value_m);
        } else {
            log->info("Initialized receiver code bias: {:.3f} m from {} satellites", seed.value_m,
                      seed.n_sats);
        }
    }
}

} // namespace

void networkBirthLocalStates(const StationRowContext& c, StationParams& set,
                             const std::vector<PppObservation>& observations) {
    if (observations.empty()) return;

    if (!set.seeded.isb && seedInterSystemBiases(c, set, observations)) set.seeded.isb = true;

    const RequiredLocal need = collectRequired(c.config, observations);
    birthSlantIonosphere(c, set, observations, need);
    birthPhaseColour(c, set, need);
    seedColdClockOnce(c, set, observations);
    birthAmbiguities(c, set, observations, need);
    pruneAmbiguitiesExcept(set, need.ambiguities);
    warnAmbiguityBandCount(c, need);
    markUnresolvable(c, set, observations, need);
    birthReceiverCodeBiases(c, set, observations);

    if (!set.seeded.code_bias && c.config.estimateIfb()) {
        seedReceiverCodeBiases(c, set, observations);
        set.seeded.code_bias = true;
    }
}

} // namespace GnssProcessor
