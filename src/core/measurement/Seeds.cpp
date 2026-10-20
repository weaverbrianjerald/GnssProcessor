/**
 * @file Seeds.cpp
 * @brief The seeds of the single-receiver and network states as pure functions of the
 *        observation equation (see measurement/Seeds.hpp).
 *
 * Each seed forms a residual per row through @ref rawResidual or @ref seedAmbiguity, takes a
 * median over satellites where the seed is one, and returns values.  The row filters of the
 * first-epoch seeds are the ones the seeding code has always used: one code row per
 * satellite, raw signals in uncombined mode and iono-free pairs otherwise, above the
 * elevation mask.
 */

#include "measurement/Seeds.hpp"

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "measurement/StateInitializer.hpp"
#include "measurement/UserParamView.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

namespace {

/// Median of a mutable vector (reorders the elements in place); 0 for an empty vector.
double computeMedian(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const size_t n = values.size();
    const size_t mid = n / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    if (n % 2 == 0) {
        double upper = values[mid];
        std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
        return (values[mid - 1] + upper) / 2.0;
    }
    return values[mid];
}

/// Whether a signal is a row of the configured combination: a raw signal in uncombined mode,
/// an iono-free pair otherwise.
bool isRowOfMode(const GnssConfig& config, const std::string& ref_type) {
    const bool is_pair = ref_type.find('_') != std::string::npos;
    return config.isUncombinedEnabled() != is_pair;
}

/// Whether a signal is a code row of the configured combination with a modeled value.
bool isCodeRow(const GnssConfig& config, const PppObservation& obs, const std::string& ref_type) {
    return obsTypeOf(ref_type) == ObsType::Code && isRowOfMode(config, ref_type)
        && obs.modeled.count(ref_type) != 0;
}

} // anonymous namespace

// ===========================================================================
// The residual
// ===========================================================================

double seedResidual(const ObservationEquation& eq, const SeedMask& mask, double observed) {
    const auto value = [&](ObsEffect effect) -> std::optional<double> {
        if (!mask.effects.has(effect)) return std::nullopt;
        const ObsAddend* term = findTerm(eq, effect);
        if (term == nullptr) return std::nullopt;
        return termValue(eq, *term);
    };
    double residual = observed;
    for (const ObsEffect effect : kSeedOrder) {
        if (effect == ObsEffect::ReceiverClock && mask.group_clocks) {
            double clock = value(ObsEffect::ReceiverClock).value_or(0.0);
            if (const auto phase_clock = value(ObsEffect::PhaseClock)) clock += *phase_clock;
            residual -= clock;
        } else if (const auto v = value(effect)) {
            residual -= *v;
        }
    }
    return residual;
}

std::optional<double> rawResidual(const ObsInput& in, const ParamLookup& params,
                                  const PppObservation& obs, const std::string& ref_type) {
    const SeedMask& mask = kLegacySeedMask.residual;
    const ObservationEquation eq = observationEquation(in, obs, ref_type, params, mask.effects);
    if (!eq.usable) return std::nullopt;
    return seedResidual(eq, mask, obs.observed.at(ref_type));
}

// ===========================================================================
// The ambiguity
// ===========================================================================

double seedAmbiguity(const ObsInput& in, const ParamLookup& params, const PppObservation& obs,
                     const std::string& ref_type) {
    if (!obs.observed.count(ref_type) || !obs.modeled.count(ref_type)) return 0.0;
    SeedMask mask = kLegacySeedMask.ambiguity;
    if (obsTypeOf(ref_type) != ObsType::Phase)
        mask.effects = mask.effects.without(ObsEffect::Ionosphere);   // the ionosphere advances phase only
    const ObservationEquation eq = observationEquation(in, obs, ref_type, params, mask.effects);
    if (!eq.usable) return 0.0;
    return seedResidual(eq, mask, obs.observed.at(ref_type));
}

double seedAmbiguityOfState(const ObsInput& in, const StateManager& states,
                            const Eigen::VectorXd& x, const PppObservation& obs,
                            const std::string& ref_type) {
    const UserParamView view(states, x, static_cast<std::size_t>(x.size()));
    return seedAmbiguity(in, ParamLookup(view), obs, ref_type);
}

// ===========================================================================
// The slant ionosphere
// ===========================================================================

std::optional<IonosphereSeed> seedIonosphere(const ObsInput& in, const PppObservation& obs,
                                             const BiasPresence& has_entry) {
    // An entry that is loaded but not applied (enable_code_phase_bias off, as it is for the
    // closed-loop consumer under the spherical-harmonic ionosphere) leaves the raw bias in
    // the innovation, so no entry counts as coverage; the network producer keeps presence
    // semantics.
    if (!biaEntryApplies(in.products.config)) return std::nullopt;
    struct CodeObs { std::string key; double value; double freq; char band; };
    std::vector<CodeObs> codes;
    for (const auto& [key, val] : obs.observed) {
        const auto code = ObservableId::parse(obs.system, key);   // e.g. C1C
        if (!code || code->type != ObsType::Code) continue;
        if (!obs.modeled.count(key)) continue;
        // Only code signals with a bias-product entry: for any other the observed minus the
        // modeled value carries the satellite's raw hardware bias, which differenced across
        // bands and divided by (alpha1 - alpha2) puts tens of metres into the seed (BeiDou
        // products ship zero-valued entries for some signals, which count as missing).
        if (!has_entry(obs.satellite, key)) continue;
        const double freq = in.ppp_utils.getFrequency(key, obs.system, obs.satellite);
        if (freq <= 0.0) continue;
        codes.push_back({key, val, freq, code->signal.digit});
    }
    if (codes.size() < 2) return std::nullopt;

    for (size_t i = 0; i < codes.size(); ++i) {
        for (size_t j = i + 1; j < codes.size(); ++j) {
            if (codes[i].band == codes[j].band) continue;
            const double f1 = codes[i].freq;
            const double f2 = codes[j].freq;
            const double P1 = codes[i].value;
            const double P2 = codes[j].value;
            const double mod1 = obs.modeled.at(codes[i].key);
            const double mod2 = obs.modeled.at(codes[j].key);

            const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
            const double alpha1 = (f_ref * f_ref) / (f1 * f1);
            const double alpha2 = (f_ref * f_ref) / (f2 * f2);
            const double denom = alpha1 - alpha2;
            if (std::abs(denom) < 1e-6) continue;

            const double iono_seed = ((P1 - mod1) - (P2 - mod2)) / denom;

            if (std::isfinite(iono_seed) && std::abs(iono_seed) < 200.0) {
                IonosphereSeed seed;
                seed.value_m  = iono_seed;
                seed.code_a   = codes[i].key;
                seed.code_b   = codes[j].key;
                seed.range_a_m = P1 - mod1;
                seed.range_b_m = P2 - mod2;
                seed.alpha_a  = alpha1;
                seed.alpha_b  = alpha2;
                return seed;
            }
        }
    }
    return std::nullopt;
}

// ===========================================================================
// The inter-system biases
// ===========================================================================

IsbSeeds seedIsb(const ObsInput& in, const ParamLookup& params,
                 const std::vector<PppObservation>& observations) {
    const GnssConfig& config = in.products.config;

    struct Group {
        System              sys{System::Unknown};
        std::vector<double> residuals;
    };
    // By constellation name: the order the seeds are applied and logged in.
    std::map<std::string, Group> groups;
    for (const auto& obs : observations) {
        if (obs.elevation < config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRow(config, obs, ref_type)) continue;
            const auto residual = rawResidual(in, params, obs, ref_type);
            if (!residual) continue;
            const std::string name = StateInitializer::systemToIsbName(obs.system);
            const bool reference = name.empty();   // the time reference (and an unknown constellation)
            Group& group = groups[reference ? std::string(systemName(timeReferenceSystem())) : name];
            group.sys = reference ? timeReferenceSystem() : obs.system;
            group.residuals.push_back(*residual);
            break;   // one code row per satellite
        }
    }

    IsbSeeds out;
    const std::string reference_key = systemName(timeReferenceSystem());
    const auto reference = groups.find(reference_key);
    if (reference == groups.end() || reference->second.residuals.empty()) return out;
    out.settled = true;
    out.reference_obs = reference->second.residuals.size();
    const double reference_median = computeMedian(reference->second.residuals);

    for (auto& [key, group] : groups) {
        if (key == reference_key || group.residuals.empty()) continue;
        if (params.find(isbRef(group.sys)).column < 0) continue;
        IsbSeed seed;
        seed.sys     = group.sys;
        seed.value_m = computeMedian(group.residuals) - reference_median;
        seed.n_obs   = group.residuals.size();
        out.seeds.push_back(seed);
    }
    return out;
}

// ===========================================================================
// The cold receiver clock
// ===========================================================================

ColdClockSeed seedColdClock(const ObsInput& in, const ParamLookup& params,
                            const std::vector<PppObservation>& observations) {
    const GnssConfig& config = in.products.config;
    ColdClockSeed out;

    const ParamSlot clock = params.find(rxClockRef());
    if (clock.column < 0) {
        out.outcome = ColdClockOutcome::NoClockState;
        return out;
    }
    // Below this the clock is cold (initialized to zero).  A receiver clock the filter has
    // solved is metres to kilometres, never this small, so a solved clock is never overwritten.
    constexpr double kColdClockEpsilonM = 1e-6;
    if (std::abs(clock.x) > kColdClockEpsilonM) {
        out.outcome   = ColdClockOutcome::AlreadySeeded;
        out.current_m = clock.x;
        return out;
    }

    std::vector<double> residuals;
    for (const auto& obs : observations) {
        if (!isTimeReference(obs.system)) continue;   // the time reference defines the clock datum
        if (obs.elevation < config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRow(config, obs, ref_type)) continue;
            const auto residual = rawResidual(in, params, obs, ref_type);
            if (!residual) continue;
            residuals.push_back(*residual);
            break;   // one code row per satellite (a robust median over satellites)
        }
    }
    if (residuals.empty()) {
        out.outcome = ColdClockOutcome::NoReference;
        return out;
    }
    out.outcome = ColdClockOutcome::Seeded;
    out.value_m = computeMedian(residuals);
    out.n_obs   = residuals.size();
    return out;
}

// ===========================================================================
// The phase clock
// ===========================================================================

PhaseClockSeed seedPhaseClock(const ObsInput& in, const ParamLookup& params,
                              const std::vector<PppObservation>& observations) {
    const GnssConfig& config = in.products.config;
    std::vector<double> code_phase_diffs;
    for (const auto& obs : observations) {
        if (obs.elevation < config.getElevationMaskDeg()) continue;

        double code_residual = 0.0, phase_residual = 0.0;
        bool have_code = false, have_phase = false;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (ref_type.empty() || !isRowOfMode(config, ref_type)) continue;
            if (!obs.modeled.count(ref_type)) continue;
            const ObsType type = obsTypeOf(ref_type);
            if (type != ObsType::Code && type != ObsType::Phase) continue;
            const auto residual = rawResidual(in, params, obs, ref_type);
            if (!residual) continue;
            if (type == ObsType::Code && !have_code)   { code_residual  = *residual; have_code  = true; }
            if (type == ObsType::Phase && !have_phase) { phase_residual = *residual; have_phase = true; }
        }
        if (have_code && have_phase)
            code_phase_diffs.push_back(code_residual - phase_residual);
    }

    PhaseClockSeed out;
    if (code_phase_diffs.empty()) return out;
    out.found   = true;
    out.n_pairs = code_phase_diffs.size();
    out.value_m = computeMedian(code_phase_diffs);
    return out;
}

// ===========================================================================
// The receiver code biases
// ===========================================================================

CodeBiasSeeds seedCodeBias(const ObsInput& in, const ParamLookup& params,
                           const std::vector<PppObservation>& observations,
                           const std::map<std::string, double>& restore) {
    const GnssConfig& config = in.products.config;
    CodeBiasSeeds out;
    if (!config.estimateIfb() || !config.isUncombinedEnabled()) return out;
    if (in.receiver_bias == nullptr) return out;

    struct Sample {
        double residual_diff;
        double iono_diff;
        double beta_diff;
    };
    std::map<RxCbKey, std::vector<Sample>> samples;

    for (const auto& obs : observations) {
        if (obs.elevation < config.getElevationMaskDeg()) continue;

        struct BandResidual {
            double  residual;
            double  alpha;
            double  coefficient;
            RxCbKey key;
        };
        std::map<char, BandResidual> band_residuals;
        for (const auto& [ref_type, val] : obs.observed) {
            // One single-band code ("C1C"); an ionosphere-free pair ("C1C_C2W") does not
            // parse as one code and is skipped.
            const auto code = ObservableId::parse(obs.system, ref_type);
            if (!code || code->type != ObsType::Code) continue;
            if (!obs.modeled.count(ref_type)) continue;

            const double freq = in.ppp_utils.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq <= 0.0) continue;
            const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
            const double alpha = (f_ref * f_ref) / (freq * freq);

            const auto bias = in.receiver_bias->resolveReceiverBias(ref_type, obs.system,
                                                                    obs.satellite, in.clock_refs);
            if (!std::isfinite(bias.coefficient) || !bias.has_key) continue;

            const auto residual = rawResidual(in, params, obs, ref_type);
            if (!residual || !std::isfinite(*residual)) continue;

            band_residuals[code->signal.digit] = {*residual, alpha, bias.coefficient, bias.key};
        }

        if (band_residuals.size() < 2) continue;

        const char sys_char = StateInitializer::systemToClockRefChar(obs.system);
        const auto crit = in.clock_refs.find(sys_char);
        if (crit == in.clock_refs.end()) continue;
        const auto ref1_sig = SignalId::fromObsCode(obs.system, crit->second.sig1);
        if (!ref1_sig) continue;
        const char ref1_band = ref1_sig->digit;   // the first clock-reference band

        const auto ref1_it = band_residuals.find(ref1_band);
        if (ref1_it == band_residuals.end()) continue;
        const BandResidual& ref1 = ref1_it->second;

        double iono_est = 0.0;
        if (const auto sat = SatId::parse(obs.satellite)) {
            const ParamSlot iono = params.find(slantIonoRef(*sat));
            if (iono.column >= 0) iono_est = iono.x;
        }

        for (const auto& [band_digit, br] : band_residuals) {
            if (band_digit == ref1_band) continue;

            const double r_diff = br.residual - ref1.residual;
            const double iono_diff = (br.alpha - ref1.alpha) * iono_est;
            const double beta_diff = br.coefficient - ref1.coefficient;

            if (std::abs(beta_diff) < 1e-12) continue;

            samples[br.key].push_back({r_diff, iono_diff, beta_diff});
        }
    }

    for (const auto& [key, sv] : samples) {
        const bool exists = params.find(rxCodeBiasRef(key)).column >= 0;
        // A warm resume that restored this station's carry restored the carry's numerator but
        // not the linearization point the injection gate judges it against, which would
        // otherwise be re-derived from the resume epoch's code.  A covered key takes the
        // writer's value; a covered key whose value cannot be used falls through to the median
        // and is counted.  A key the map does not cover has no carry and takes the median.
        const std::string text = userCodeBiasKey(key);
        if (StateInitializer::codeBiasSeedOverrideCovers(restore, text)) {
            const auto restored = StateInitializer::resolveCodeBiasSeedOverride(restore, text);
            if (restored.has_value() && exists) {
                CodeBiasSeed seed;
                seed.key      = key;
                seed.value_m  = *restored;
                seed.restored = true;
                out.seeds.push_back(seed);
                continue;
            }
            ++out.refused;
        }
        std::vector<double> estimates;
        estimates.reserve(sv.size());
        for (const auto& s : sv) {
            const double est = (s.residual_diff - s.iono_diff) / s.beta_diff;
            if (std::isfinite(est) && std::abs(est) < 200.0)
                estimates.push_back(est);
        }
        if (estimates.empty() || !exists) continue;

        CodeBiasSeed seed;
        seed.key     = key;
        seed.n_sats  = estimates.size();
        seed.value_m = computeMedian(estimates);
        out.seeds.push_back(seed);
    }
    return out;
}

} // namespace GnssProcessor
