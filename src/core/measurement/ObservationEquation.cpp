/**
 * @file ObservationEquation.cpp
 * @brief The term list of a code or phase row, its sum and its Jacobian row (see
 *        measurement/ObservationEquation.hpp).
 *
 * observationEquation walks the effects in the order of the sum.  Each effect has one
 * function that reads the configuration flag that switches it on, asks the caller's
 * parameters for the columns it needs, and appends its term.  The functions return false
 * when the row cannot be built, with the reason in the equation's fault.
 */

#include "measurement/ObservationEquation.hpp"

#include "common/Constants.hpp"
#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "measurement/StateInitializer.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cmath>
#include <optional>

namespace GnssProcessor {

namespace {

// =======================================================================
// Identities of the row
// =======================================================================

/// The satellite of a canonical RINEX id text ("G05"), or nothing for any other spelling
/// ("G5", "G 5", an empty text): a state key built from a typed satellite would not equal the
/// text the states were born under.
std::optional<SatId> canonicalSat(const std::string& text) {
    const auto sat = SatId::parse(text);
    if (!sat || sat->str() != text) return std::nullopt;
    return sat;
}

/// The signal of a canonical observation code ("L1C"), or nothing for any other text.
std::optional<SignalId> canonicalSignal(System sys, const std::string& code) {
    const auto sig = SignalId::fromObsCode(sys, code);
    if (!sig || sig->obsCode(obsTypeOf(code)) != code) return std::nullopt;
    return sig;
}

/// The pair of a canonical iono-free text ("L1C_L2W"), or nothing for any other text.
std::optional<SignalPair> canonicalPair(System sys, const std::string& text) {
    const auto pair = SignalPair::parse(sys, text);
    if (!pair || pair->str(obsTypeOf(text)) != text) return std::nullopt;
    return pair;
}

// =======================================================================
// One row's inputs
// =======================================================================

/// What every effect function reads, and the equation it appends to.
struct Row {
    const ObsInput&         in;
    const GnssConfig&       cfg;
    const PppObservation&   obs;
    const std::string&      ref_type;
    const ParamLookup&      params;
    ObservationEquation&    eq;
    bool                    is_phase;
    bool                    is_uncombined;
    bool                    is_pair;   ///< The signal is an iono-free pair "a_b".
    std::optional<SatId>    sat;       ///< The canonical satellite, when the text is one.

    /// Ends the row as unusable.
    bool fail(ObsFault fault) const {
        eq.usable = false;
        eq.fault  = fault;
        return false;
    }

    /// The satellite, or an unusable row when its text is not canonical.
    bool needSat() const { return sat ? true : fail(ObsFault::UnmappedIdentity); }
};

/// Appends an empty term of one effect and returns it.
ObsAddend& open(ObservationEquation& eq, ObsEffect effect) {
    ObsAddend& a = eq.addend[eq.n++];
    a.effect = effect;
    return a;
}

/// Adds a parameter to a term.
void addPart(ObsAddend& a, const ParamRef& ref, double partial, const ParamSlot& slot) {
    ObsPart& p = a.part[a.n_part++];
    p.ref     = ref;
    p.partial = partial;
    p.column  = slot.column;
    p.x       = slot.x;
}

/// A term of one parameter, when the parameter exists.
/// @return True when the term was added.
bool addTerm(Row& r, ObsEffect effect, const ParamRef& ref, double partial) {
    const ParamSlot slot = r.params.find(ref);
    if (slot.column < 0) return false;
    addPart(open(r.eq, effect), ref, partial, slot);
    return true;
}

// =======================================================================
// The effects, in the order of the sum
// =======================================================================

/// The a-priori model value.  Throws when the observation has none.
void addModeled(Row& r) {
    ObsAddend& a = open(r.eq, ObsEffect::Modeled);
    a.constant = r.obs.modeled.at(r.ref_type);
}

/// Minus the line of sight dotted with the position's offset from the linearization point.
void addRangeCorrection(Row& r) {
    std::array<ParamSlot, 3> slot;
    for (std::uint8_t axis = 0; axis < 3; ++axis) {
        slot[axis] = r.params.find(rxPositionRef(axis));
        if (slot[axis].column < 0) return;
    }
    ObsAddend& a = open(r.eq, ObsEffect::RangeCorrection);
    for (std::uint8_t axis = 0; axis < 3; ++axis)
        addPart(a, rxPositionRef(axis), -r.obs.los(axis), slot[axis]);
    r.eq.range_origin = r.obs.rx_pos_linearization;
}

/// The receiver clock and, on phase rows, the phase-clock offset (+1 each).
void addClocks(Row& r) {
    addTerm(r, ObsEffect::ReceiverClock, rxClockRef(), 1.0);
    if (r.is_phase) addTerm(r, ObsEffect::PhaseClock, phaseClockRef(), 1.0);
}

/// The zenith wet delay times the wet mapping function.
void addTroposphere(Row& r) {
    addTerm(r, ObsEffect::Troposphere, zwdRef(), r.obs.mf_w);
}

/// The horizontal gradients: mf_grad * (cos(az) * g_north + sin(az) * g_east).
void addGradients(Row& r) {
    const ParamRef north = gradientRef(true);
    const ParamRef east  = gradientRef(false);
    const ParamSlot sn = r.params.find(north);
    const ParamSlot se = r.params.find(east);
    if (sn.column < 0 || se.column < 0) return;

    const double el_rad = r.obs.elevation * Constants::PI / 180.0;
    const double az_rad = r.obs.azimuth * Constants::PI / 180.0;
    const double sin_el = std::sin(el_rad);
    const double tan_el = std::tan(el_rad);
    const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
    const double cos_az = std::cos(az_rad);
    const double sin_az = std::sin(az_rad);

    ObsAddend& a = open(r.eq, ObsEffect::TropoGradient);
    a.scale = mf_grad;
    addPart(a, north, cos_az, sn);
    addPart(a, east, sin_az, se);
}

/// The inter-system bias of a constellation other than the time reference (+1).
void addSystemTimeOffset(Row& r) {
    if (!hasInterSystemBias(r.obs.system)) return;
    addTerm(r, ObsEffect::SystemTimeOffset, isbRef(r.obs.system), 1.0);
}

/// The per-satellite slant ionosphere: +alpha on code, -alpha on phase, alpha the squared
/// frequency ratio against GPS L1.
bool addIonosphere(Row& r) {
    if (!r.is_uncombined) return true;
    if (!r.needSat()) return false;
    const ParamRef ref = slantIonoRef(*r.sat);
    const ParamSlot slot = r.params.find(ref);
    if (slot.column < 0) return true;
    const double freq = r.in.ppp_utils.getFrequency(r.ref_type, r.obs.system, r.obs.satellite);
    if (!(freq > 0.0)) return true;
    const double f_ref = Constants::GPS_L1_FREQ;
    const double alpha = (f_ref * f_ref) / (freq * freq);
    addPart(open(r.eq, ObsEffect::Ionosphere), ref, (r.is_phase ? -1.0 : 1.0) * alpha, slot);
    return true;
}

/// The receiver code bias (uncombined code rows): the state and coefficient the resolver
/// names for the signal.
void addReceiverCodeBias(Row& r) {
    if (!r.cfg.estimateIfb() || !r.is_uncombined || r.is_phase || r.in.receiver_bias == nullptr)
        return;
    const auto bias = r.in.receiver_bias->resolveReceiverBias(r.ref_type, r.obs.system,
                                                              r.obs.satellite);
    if (!std::isfinite(bias.coefficient) || !bias.has_key) return;
    addTerm(r, ObsEffect::ReceiverCodeBias, rxCodeBiasRef(bias.key), bias.coefficient);
}

/// The per-satellite signal code bias (code rows; phase ambiguities absorb the satellite
/// phase bias): a raw code in uncombined mode, an iono-free pair of codes otherwise.
bool addSatelliteCodeBias(Row& r) {
    if (!r.cfg.estimateSatBiases() || r.is_phase) return true;
    if (r.is_uncombined == r.is_pair) return true;   // uncombined: raw only; combined: pair only
    if (!r.needSat()) return false;
    ParamRef ref;
    if (r.is_pair) {
        const auto pair = canonicalPair(r.sat->sys, r.ref_type);
        if (!pair) return r.fail(ObsFault::UnmappedIdentity);
        ref = satCodeBiasIfRef(*r.sat, *pair);
    } else {
        const auto code = canonicalSignal(r.sat->sys, r.ref_type);
        if (!code) return r.fail(ObsFault::UnmappedIdentity);
        ref = satCodeBiasRef(*r.sat, *code);
    }
    addTerm(r, ObsEffect::SatelliteCodeBias, ref, kSatBiasSign);
    return true;
}

/// The per-satellite signal phase bias (uncombined phase rows, network producer), with the
/// receiver phase bias that splits it when the eq34 datum is armed.  The receiver phase bias
/// appears on exactly the rows whose satellite phase bias exists, with the sign of the
/// ambiguity column it splits, so it is part of the same term.
bool addSatellitePhaseBias(Row& r) {
    if (!r.cfg.estimateSatPhaseBiases() || !r.is_phase || !r.is_uncombined || r.is_pair)
        return true;
    if (!r.needSat()) return false;
    const auto phase = canonicalSignal(r.sat->sys, r.ref_type);
    if (!phase) return r.fail(ObsFault::UnmappedIdentity);

    const ParamRef ref = satPhaseBiasRef(*r.sat, *phase);
    const ParamSlot slot = r.params.find(ref);
    if (slot.column < 0) return true;
    ObsAddend& a = open(r.eq, ObsEffect::SatellitePhaseBias);
    addPart(a, ref, kSatBiasSign, slot);
    if (r.cfg.networkPhaseDatumEq34()) {
        const ParamRef rpb = rxPhaseBiasRef(r.sat->sys, *phase);
        const ParamSlot rpb_slot = r.params.find(rpb);
        if (rpb_slot.column >= 0) addPart(a, rpb, 1.0, rpb_slot);
    }
    return true;
}

/// The per-satellite clock (uncombined rows, network producer): one clock per satellite,
/// shared by all signals.
bool addSatelliteClock(Row& r) {
    if (!r.cfg.estimateSatClocks() || !r.is_uncombined) return true;
    if (!r.needSat()) return false;
    addTerm(r, ObsEffect::SatelliteClock, satClockRef(*r.sat), kSatClockPartial);
    return true;
}

/// The SSR satellite bias as a known term (no parameter).  The network producer estimates
/// the biases as states, so it takes none: importing the same quantity would double-count it.
void addSsrBias(Row& r) {
    if (r.cfg.getProcessingMode() == ProcessingMode::PPP_RTK_Network) return;
    const SsrCorrectionStore& ssr = r.in.products.ssr;
    const bool   gate    = r.cfg.enableSsrBiasMaxAge();   // reject stale biases by obs-vs-record age
    // The last record stays applied across staleness gaps below hold_factor x max_age.
    const double max_age = r.cfg.getSsrBiasMaxAge() * r.cfg.getSsrBiasStaleHoldFactor();
    double bias = 0.0;
    if (r.is_phase) {
        const auto pb = gate ? ssr.getPhaseBias(r.obs.satellite, r.ref_type, r.obs.time, max_age)
                             : ssr.getPhaseBias(r.obs.satellite, r.ref_type);
        if (pb) bias = kSatBiasSign * pb->bias_m;
    } else {
        bias = kSatBiasSign
             * (gate ? ssr.getCodeBias(r.obs.satellite, r.ref_type, r.obs.time, max_age)
                     : ssr.getCodeBias(r.obs.satellite, r.ref_type));
    }
    open(r.eq, ObsEffect::SsrBias).constant = bias;
}

/// The phase-colour state of the line of sight (+1 on every phase row of the satellite).
void addPhaseColour(Row& r) {
    if (!r.cfg.getNetworkConfig().phase_colour_state) return;
    addTerm(r, ObsEffect::PhaseColour, phaseColourRef(*r.sat), 1.0);
}

/// The ambiguity of a per-band row: +1 on its own state.
bool addPerBandAmbiguity(Row& r) {
    if (!r.needSat()) return false;
    ParamRef ref;
    if (r.is_pair) {
        const auto pair = canonicalPair(r.sat->sys, r.ref_type);
        if (!pair) return r.fail(ObsFault::UnmappedIdentity);
        ref = ambiguityIfRef(*r.sat, *pair);
    } else {
        const auto phase = canonicalSignal(r.sat->sys, r.ref_type);
        if (!phase) return r.fail(ObsFault::UnmappedIdentity);
        ref = ambiguityRef(*r.sat, *phase);
    }
    if (!addTerm(r, ObsEffect::Ambiguity, ref, 1.0)) return r.fail(ObsFault::NoAmbiguity);
    // The line of sight's phase colour: +1 on every phase row of the satellite, both bands.
    addPhaseColour(r);
    return true;
}

/// The ambiguity of an NL/WL-basis row.  The state holds NL_ref (cycles, the carrier
/// ambiguity of the reference band) and WL_(ref, b) = N_a - N_b in ascending-band form for
/// each other band b.  The phase in metres is phi_b = rho + lambda_b * N_b with
///     N_b = NL_ref                       (b == ref)
///     N_b = NL_ref - s * WL_<a>_<z>      (b != ref)
/// s = +1 when the reference band is the lower of the pair, -1 when it is the higher.
bool addNlWlAmbiguity(Row& r) {
    if (r.ref_type.size() < 2) return r.fail(ObsFault::NlWlReference);
    const auto rb_it = r.in.nlwl_ref_band.find(r.obs.satellite);
    if (rb_it == r.in.nlwl_ref_band.end()) return r.fail(ObsFault::NlWlReference);
    const char ref_band = rb_it->second;
    const char band = r.ref_type[1];
    const double lambda_b = r.in.ppp_utils.getWavelength(r.obs.system, r.ref_type, r.obs.satellite);
    if (!std::isfinite(lambda_b) || lambda_b <= 0.0) return r.fail(ObsFault::NlWlReference);
    if (!r.needSat()) return false;

    const FrequencyBand ref_fb = bandOfDigit(r.sat->sys, ref_band);
    const FrequencyBand fb     = bandOfDigit(r.sat->sys, band);
    if (ref_fb == FrequencyBand::Unknown || fb == FrequencyBand::Unknown)
        return r.fail(ObsFault::UnmappedIdentity);

    const ParamRef nl = narrowLaneRef(*r.sat, ref_fb);
    const ParamSlot nl_slot = r.params.find(nl);
    if (nl_slot.column < 0) return r.fail(ObsFault::NlWlReference);

    ObsAddend& a = open(r.eq, ObsEffect::Ambiguity);
    a.scale = lambda_b;
    addPart(a, nl, 1.0, nl_slot);
    if (band == ref_band) return true;

    const ParamRef wl = wideLaneRef(*r.sat, ref_fb, fb);
    const ParamSlot wl_slot = r.params.find(wl);
    if (wl_slot.column < 0) return r.fail(ObsFault::NlWlReference);
    const double wl_sign = (ref_band < band) ? +1.0 : -1.0;
    addPart(a, wl, -wl_sign, wl_slot);
    return true;
}

/// The ambiguity of a phase row, by the configured basis.
bool addAmbiguity(Row& r) {
    if (!r.is_phase) return true;
    return r.cfg.getStateAmbiguityBasis() == StateAmbiguityBasis::NlWl ? addNlWlAmbiguity(r)
                                                                       : addPerBandAmbiguity(r);
}

/// The value of one term.
double addendValue(const ObservationEquation& eq, const ObsAddend& a) {
    if (a.effect == ObsEffect::RangeCorrection) {
        // The dot product of the line of sight and the offset, as the filter has always
        // formed it (its partials are minus the line of sight).
        const Eigen::Vector3d los(-a.part[0].partial, -a.part[1].partial, -a.part[2].partial);
        const Eigen::Vector3d pos(a.part[0].x, a.part[1].x, a.part[2].x);
        return -los.dot(pos - eq.range_origin);
    }
    if (a.n_part == 0) return a.constant;
    double sum = a.part[0].partial * a.part[0].x;
    for (std::uint8_t k = 1; k < a.n_part; ++k) sum += a.part[k].partial * a.part[k].x;
    return a.scale * sum;
}

} // namespace

// =======================================================================
// Public functions
// =======================================================================

const char* obsEffectName(ObsEffect effect) {
    switch (effect) {
        case ObsEffect::Modeled:            return "modeled";
        case ObsEffect::RangeCorrection:    return "range_correction";
        case ObsEffect::ReceiverClock:      return "receiver_clock";
        case ObsEffect::PhaseClock:         return "phase_clock";
        case ObsEffect::Troposphere:        return "troposphere";
        case ObsEffect::TropoGradient:      return "tropo_gradient";
        case ObsEffect::SystemTimeOffset:   return "system_time_offset";
        case ObsEffect::Ionosphere:         return "ionosphere";
        case ObsEffect::ReceiverCodeBias:   return "receiver_code_bias";
        case ObsEffect::SatelliteCodeBias:  return "satellite_code_bias";
        case ObsEffect::SatellitePhaseBias: return "satellite_phase_bias";
        case ObsEffect::SatelliteClock:     return "satellite_clock";
        case ObsEffect::SsrBias:            return "ssr_bias";
        case ObsEffect::Ambiguity:          return "ambiguity";
        case ObsEffect::PhaseColour:        return "phase_colour";
    }
    return "?";
}

ObservationEquation observationEquation(const ObsInput& in, const PppObservation& obs,
                                        const std::string& ref_type, const ParamLookup& params) {
    ObservationEquation eq;
    const MeasurementType type = classifyMeasurement(ref_type);
    Row r{in, in.products.config, obs, ref_type, params, eq,
          /*is_phase=*/type == MeasurementType::Phase,
          /*is_uncombined=*/in.products.config.isUncombinedEnabled(),
          /*is_pair=*/ref_type.find('_') != std::string::npos,
          canonicalSat(obs.satellite)};
    if (type == MeasurementType::Doppler) {
        r.fail(ObsFault::NotCodeOrPhase);
        return eq;
    }

    addModeled(r);
    addRangeCorrection(r);
    addClocks(r);
    addTroposphere(r);
    addGradients(r);
    addSystemTimeOffset(r);
    if (!addIonosphere(r)) return eq;
    addReceiverCodeBias(r);
    if (!addSatelliteCodeBias(r) || !addSatellitePhaseBias(r) || !addSatelliteClock(r)) return eq;
    addSsrBias(r);
    if (!addAmbiguity(r)) return eq;
    eq.usable = true;
    return eq;
}

double evaluate(const ObservationEquation& eq, ObsTermValues* terms) {
    double predicted = 0.0;
    for (std::uint8_t i = 0; i < eq.n; ++i) {
        const ObsAddend& a = eq.addend[i];
        const double v = addendValue(eq, a);
        if (terms != nullptr) terms->value[static_cast<std::size_t>(a.effect)] = v;
        if (i == 0) predicted = v;
        else        predicted += v;
    }
    return predicted;
}

void scatterDense(const ObservationEquation& eq, Eigen::VectorXd& row) {
    for (std::uint8_t i = 0; i < eq.n; ++i) {
        const ObsAddend& a = eq.addend[i];
        for (std::uint8_t k = 0; k < a.n_part; ++k)
            row(a.part[k].column) = a.scale * a.part[k].partial;
    }
}

} // namespace GnssProcessor
