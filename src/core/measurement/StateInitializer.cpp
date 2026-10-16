/**
 * @file StateInitializer.cpp
 * @brief First-epoch state seeding for ISB, phase clock, and code biases.
 */

#include "measurement/StateInitializer.hpp"
#include "model/PppModel.hpp"
#include "products/ProductCache.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "filter/StateKeySchema.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

namespace {

/// Compute median of a mutable vector (reorders elements in-place).
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

} // anonymous namespace

// ===========================================================================
// Construction
// ===========================================================================

StateInitializer::StateInitializer(const GnssConfig& config,
                                   const PppModel& ppp_model,
                                   StateManager& state_manager,
                                   LogManager& log_manager)
    : config_(config),
      ppp_model_(ppp_model),
      state_manager_(state_manager),
      log_manager_(log_manager),
      ppp_utils_(log_manager) {
}

// ===========================================================================
// System enum helpers
// ===========================================================================

std::string StateInitializer::systemToIsbName(System system) {
    // GPS is the time reference and has no ISB state; an unknown system has none either.
    if (system == System::GPS || system == System::Unknown) return "";
    return systemName(system);
}

char StateInitializer::systemToClockRefChar(System system) {
    // The BIA keys its clock-reference pairs by RINEX letter.  An unknown system gives
    // '\0' (not the '?' of rinexLetter), which the callers test for.
    if (system == System::Unknown) return '\0';
    return rinexLetter(system);
}

// ===========================================================================
// Receiver bias resolution
// ===========================================================================

StateInitializer::ReceiverBiasEntry
StateInitializer::resolveReceiverBias(const std::string& obs_signal,
                                       System system,
                                       const std::string& satellite) const {
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    ReceiverBiasEntry result{ {}, NaN };

    const char sys_char = systemToClockRefChar(system);
    if (sys_char == '\0') return result;

    // -----------------------------------------------------------------------
    // Receiver inter-signal code bias of a second band-1 code (
    // docs/refound/SCC_second_code_channel_2026-09-21.md).
    //
    // With `network.second_code_channel` a station contributes every allow-listed
    // band-1 code it tracks, not only the one its canonical latch selected.  The
    // per-band keying below cannot represent that: C1C and C1W share a band digit,
    // so both resolve to "<sys>_DCB" with the same coefficient, and the second code
    // would assert a zero receiver inter-signal bias and push the whole difference
    // into the satellite column.
    //
    // A band-1 code that is not this station's latched canonical code therefore
    // gets its own per-(receiver, system, signal) state with unit coefficient: an
    // absolute receiver delay in the frame that the clock and ionosphere datum
    // leave free, not the ionosphere-referenced difference the DCB state carries.
    // The null direction this opens (every receiver inter-signal bias against
    // every satellite one) is closed by the satellite family's own time-linked
    // sum-to-zero row, so the card changes no datum row (R3).
    //
    // Rows covered: both the R1 companions and a fallback code that arrives
    // without its canonical code beside it.  The test compares the signal with the
    // latch, not the way the row entered, so the two cases cannot disagree about
    // which state a row belongs to.  An undecided latch (empty: before the
    // constellation's first observed epoch, or for a band this station never
    // latched) keeps the per-band keying; a code is "second" only relative to a
    // known first.
    //
    // GLONASS is excluded: its FDMA per-slot group delay is a different state
    // family with its own datum, handled in the branch below, and "C1C" is a legal
    // GLONASS code that would otherwise match an allow-list written for GPS.  This
    // block sits before that branch and before the clock-reference lookup because
    // the inter-signal correction is a pure per-signal receiver delay and needs no
    // pin pair.
    //
    // With the knob off the whole block is skipped, so the output is byte-identical.
    //
    // The predicate generalises for the all-channel mode.  With
    // `processing.channel_mode: all` on the network path a station contributes
    // every admitted code attribute of every selected band, not an allow-listed
    // band-1 pair, and the per-band keying below would assert a zero receiver
    // inter-signal bias on each of them.  On band 2 and above the RF-0d
    // geometry-free pin also asserts the satellite side to zero, which repeats
    // the same defect on both sides.  Under the widening the test therefore
    // drops the allow-list and the band-1 restriction and keeps only what makes a
    // row a second one: an admitted code that is not that band's latched canonical
    // code, on any band digit.  The rest (unit coefficient, absolute frame,
    // GLONASS exclusion, undecided-latch fall-through) is unchanged.
    const bool chan_all = config_.networkChannelModeAll();
    const auto code = ObservableId::parse(system, obs_signal);   // e.g. C1W: code, band digit '1'
    if ((config_.networkSecondCodeChannel() || chan_all) && system != System::GLONASS
        && code && code->type == ObsType::Code
        && (chan_all ? (code->signal.digit >= '1' && code->signal.digit <= '9')
                     : code->signal.digit == '1')) {
        const auto& allow = config_.networkSecondCodeSignals();
        const bool listed =
            chan_all || std::find(allow.begin(), allow.end(), obs_signal) != allow.end();
        if (listed) {
            const std::string sys = systemName(system);
            const std::string canonical =
                ppp_model_.latchedCanonicalCodeOnBandDigit(sys, code->signal.digit);
            if (!canonical.empty() && canonical != obs_signal) {
                result.state_key   = StateKeys::makeReceiverIscKey(sys, obs_signal);
                result.coefficient = 1.0;
                return result;
            }
        }
    }

    // -----------------------------------------------------------------------
    // GLONASS per-FCN-slot receiver inter-frequency code bias.
    //
    // FDMA: each GLONASS slot transmits on a distinct carrier frequency
    // (f = f_base + k*df), so the receiver group delay differs per slot.  A
    // single per-band IFB/DCB state cannot represent that, which is why the
    // per-band path below excludes GLONASS.  With enable_glonass_ifb set, this
    // branch instead emits one bias state per (FDMA band, slot k) with unit
    // coefficient.
    //
    // Datum: the central channel k=0 is the held reference (no state).  In
    // undifferenced PPP its constant is absorbed by the GLONASS ISB (a hard
    // anchor when a k=0 satellite is up, otherwise a soft ISB/mean-IFB gauge).
    // Under BSSD these columns survive the receiver-block zeroing (two satellites
    // on different slots do not cancel), but only the inter-slot differences are
    // determined by the measurements; the common offset of the {IFB(k)} set is an
    // unobservable gauge held softly by the per-slot prior.  That gauge lies in
    // the SD null space, so its drift does not affect position.  Only the FDMA
    // bands (G1='1', G2='2') are slot-dependent; the CDMA bands (G3/G4/G6) have a
    // fixed frequency and keep the per-band treatment.
    //
    // This runs before the clock-reference lookup below because GLONASS usually
    // has no SATELLITE_CLOCK_REFERENCE_OBSERVABLES entry in the .BIA product
    // (empty reference pair), which would return early and never reach any IFB
    // logic.
    if (system == System::GLONASS && config_.enableGlonassIfb()) {
        const auto sig = SignalId::fromObsCode(system, obs_signal);
        const FrequencyBand fb = sig ? sig->band() : FrequencyBand::Unknown;
        if (fb == FrequencyBand::GloG1 || fb == FrequencyBand::GloG2) {   // FDMA bands only
            // Resolve k from the model's seeded PppUtils (RINEX header / live
            // broadcast), not from this class's own unseeded instance, whose
            // getGlonassChannel would fall back to the stale default table.
            const int k = ppp_model_.getGlonassChannel(satellite);
            if (k < -7 || k > 6) return result;      // unknown slot -> no IFB column
            if (k == 0) return result;               // central channel = held datum
            result.state_key   = StateKeys::makeGlonassPerSlotIfbKey(
                                     systemName(system), sig->digit, k);
            result.coefficient = 1.0;
        }
        return result;  // GLONASS fully handled here (per-slot, or none for CDMA/k=0)
    }

    const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
    auto it = clock_refs.find(sys_char);
    if (it == clock_refs.end()) return result;

    const auto& ref = it->second;
    const std::string sys = systemName(system);

    // The band digits of this signal and of the clock-reference pair (ref.sig1, ref.sig2).
    const auto sig_j = SignalId::fromObsCode(system, obs_signal);
    const auto sig_1 = SignalId::fromObsCode(system, ref.sig1);
    const auto sig_2 = SignalId::fromObsCode(system, ref.sig2);
    if (!sig_j || !sig_1 || !sig_2) return result;
    const char band_j = sig_j->digit;
    const char band_1 = sig_1->digit;
    const char band_2 = sig_2->digit;

    // Non-reference band: independent IFB state, key "<system>_IFB<band>".
    // GLONASS is excluded here: its FDMA per-channel receiver delay cannot be
    // represented by one per-band IFB state shared across slots, which would bias
    // every channel.  The per-(band, slot) treatment is the enable_glonass_ifb
    // branch above; with that flag off, a non-reference GLONASS band carries no
    // IFB state and its delay is absorbed by the per-(sat, signal) bias path and
    // residual de-weighting.
    if (band_j != band_1 && band_j != band_2) {
        if (system == System::GLONASS) return result;   // no FDMA per-band IFB
        result.state_key   = StateKeys::makeIfbKey(sys, band_j);
        result.coefficient = 1.0;
        return result;
    }

    // Reference band (either band of the clock-reference pair): GF-IF coefficient
    // against the system DCB state, key "<system>_DCB".
    const double f1 = ppp_utils_.getFrequency(ref.sig1, system, satellite);
    const double f2 = ppp_utils_.getFrequency(ref.sig2, system, satellite);
    if (!(std::isfinite(f1) && f1 > 0.0 && std::isfinite(f2) && f2 > 0.0))
        return result;

    const double mu1 = 1.0;
    const double mu2 = (f1 * f1) / (f2 * f2);
    const double dmu = mu1 - mu2;
    if (std::abs(dmu) < 1e-12) return result;

    result.state_key = StateKeys::makeDcbKey(sys);
    result.coefficient = (band_j == band_1) ? (mu1 / dmu) : (mu2 / dmu);
    return result;
}

// ===========================================================================
// Raw residual computation
// ===========================================================================

double StateInitializer::computeRawResidual(const PppObservation& obs,
    const std::string& ref_type,
    const Eigen::VectorXd& x) const {

    const double observed = obs.observed.at(ref_type);
    const double modeled  = obs.modeled.at(ref_type);
    const double rx_clk   = state_manager_.hasClock()
                            ? x(state_manager_.getClockIndex())
                            : 0.0;
    const double zwd      = x(state_manager_.getTropoIndex());
    const double tropo_wet = obs.mf_w * zwd;

    // ISB contribution
    double isb = 0.0;
    const std::string sys_name = systemToIsbName(obs.system);
    if (!sys_name.empty()) {
        const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx < static_cast<size_t>(x.size()))
            isb = x(static_cast<Eigen::Index>(isb_idx));
    }

    // Tropospheric gradient contribution
    double tropo_grad = 0.0;
    if (state_manager_.hasTropoGradients()) {
        const double el_rad = obs.elevation * Constants::PI / 180.0;
        const double az_rad = obs.azimuth * Constants::PI / 180.0;
        const double sin_el = std::sin(el_rad);
        const double tan_el = std::tan(el_rad);
        const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
        const double gn = x(static_cast<Eigen::Index>(state_manager_.getTropoGradientNorthIndex()));
        const double ge = x(static_cast<Eigen::Index>(state_manager_.getTropoGradientEastIndex()));
        tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
    }

    // Per-satellite ionosphere (PPP-RTK uncombined)
    double iono = 0.0;
    const ObsType ref_obs_type = obsTypeOf(ref_type);   // first character: 'C' or 'L'
    if (config_.isUncombinedEnabled() && !ref_type.empty()
        && (ref_obs_type == ObsType::Code || ref_obs_type == ObsType::Phase)) {
        const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
        if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
            const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq > 0.0) {
                const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
                const double alpha = (f_ref * f_ref) / (freq * freq);
                iono = (ref_obs_type == ObsType::Phase ? -1.0 : 1.0) * alpha * x(iono_idx);
            }
        }
    }

    // Receiver code bias (DCB / IFB; code only, uncombined)
    double code_bias = 0.0;
    if (config_.estimateIfb() && config_.isUncombinedEnabled()
        && !ref_type.empty() && ref_obs_type == ObsType::Code) {
        const auto bias = resolveReceiverBias(ref_type, obs.system, obs.satellite);
        if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
            const int cb_idx = state_manager_.getCodeBiasIndex(bias.state_key);
            if (cb_idx >= 0 && cb_idx < static_cast<int>(x.size()))
                code_bias = bias.coefficient * x(cb_idx);
        }
    }

    // Per-satellite signal bias (code-only: phase ambiguity absorbs sat phase bias)
    double sat_bias = 0.0;
    if (config_.estimateSatBiases() && !ref_type.empty()) {
        const bool is_if = (ref_type.find('_') != std::string::npos);
        const bool applicable = config_.isUncombinedEnabled()
            ? (!is_if && ref_obs_type == ObsType::Code)
            : (is_if && ref_obs_type == ObsType::Code);
        if (applicable) {
            const std::string sb_key = StateKeys::makeSatBiasKey(obs.satellite, ref_type);
            const int sb_idx = state_manager_.getSatBiasIndex(sb_key);
            if (sb_idx >= 0 && sb_idx < static_cast<int>(x.size()))
                sat_bias = x(sb_idx);
        }
    }

    return observed - modeled - rx_clk - tropo_wet - isb - tropo_grad
         - iono - code_bias - sat_bias;
}

// ===========================================================================
// Ionosphere seed
// ===========================================================================

std::optional<double> StateInitializer::computeIonosphereSeed(const PppObservation& obs) const {
    struct CodeObs { std::string key; double value; double freq; char band; };
    std::vector<CodeObs> codes;
    for (const auto& [key, val] : obs.observed) {
        const auto code = ObservableId::parse(obs.system, key);   // e.g. C1C
        if (!code || code->type != ObsType::Code) continue;
        if (!obs.modeled.count(key)) continue;
        // Only use code signals with valid BIA coverage.  Otherwise
        // (P - mod) carries the raw satellite hardware bias, which
        // appears after differencing as (bias_band1 - bias_band2)
        // divided by (alpha1 - alpha2).  For BeiDou MGEX products
        // that ship zero-valued C2I/C6I entries (treated as missing
        // by buildBiasIndex), this would put 30–60 m of error into
        // the iono seed and, through the per-row prediction, into
        // every sat-bias and ambiguity seed for the satellite.
        if (!ppp_model_.hasBiasEntry(obs.satellite, key)) continue;
        // Coverage requires application (bias_coverage_requires_apply):
        // the gate's contract is that the .BIA value has been removed
        // from (P - mod).  An entry that is loaded but not applied
        // (enable_code_phase_bias off, e.g. forced by the
        // estimate_iono_sh coupling and inherited by the derived
        // closed-loop consumer) leaves the raw hardware bias in the
        // innovation, which is the error this gate keeps out of the
        // seed.  The network producer keeps presence semantics,
        // matching MeasurementModel::biaCoversSignal (its code rows are
        // handled by network-estimated code-bias states, and an armed
        // producer's emission stays byte-identical).
        if (config_.getModelCorrections().bias_coverage_requires_apply
            && config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network
            && !config_.getModelCorrections().enable_code_phase_bias) continue;
        const double freq = ppp_utils_.getFrequency(key, obs.system, obs.satellite);
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
                log_manager_.getLogger("state_initializer")->debug(
                    "Iono seed for {} from {}/{}: P1-mod1={:.3f}, P2-mod2={:.3f}, "
                    "alpha1={:.4f}, alpha2={:.4f}, seed={:.3f} m",
                    obs.satellite, codes[i].key, codes[j].key,
                    P1 - mod1, P2 - mod2, alpha1, alpha2, iono_seed);
                return iono_seed;
            }
        }
    }
    return std::nullopt;
}

// ===========================================================================
// First-epoch ISB initialization
// ===========================================================================

bool StateInitializer::initializeInterSystemBiases(const std::vector<PppObservation>& observations,
    const std::string& epoch_prefix) {

    std::map<std::string, std::vector<double>> system_residuals;
    const Eigen::VectorXd x = state_manager_.getStateVector();

    for (const auto& obs : observations) {
        if (obs.elevation < config_.getElevationMaskDeg()) continue;

        for (const auto& [ref_type, observed] : obs.observed) {
            if (obsTypeOf(ref_type) != ObsType::Code) continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (config_.isUncombinedEnabled()) {
                if (has_underscore) continue;
            } else {
                if (!has_underscore) continue;
            }
            if (!obs.modeled.count(ref_type)) continue;

            const double residual = computeRawResidual(obs, ref_type, x);
            const std::string sys_name = systemToIsbName(obs.system);
            const std::string key = sys_name.empty() ? systemName(System::GPS) : sys_name;
            system_residuals[key].push_back(residual);
            break;  // one code obs per satellite
        }
    }

    const std::string gps_key = systemName(System::GPS);   // the reference system's key
    if (system_residuals.find(gps_key) == system_residuals.end() ||
        system_residuals[gps_key].empty()) {
        log_manager_.getLogger("state_initializer")->warn(
            "{}Cannot initialize ISB: no GPS code observations", epoch_prefix);
        return false;   // retry next epoch; the caller does not latch as initialized
    }

    double gps_median = computeMedian(system_residuals[gps_key]);
    Eigen::VectorXd x_updated = state_manager_.getStateVector();

    for (auto& [sys_name, residuals] : system_residuals) {
        if (sys_name == gps_key || residuals.empty()) continue;

        const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx >= static_cast<size_t>(x_updated.size())) continue;

        const double isb_estimate = computeMedian(residuals) - gps_median;
        x_updated(isb_idx) = isb_estimate;

        log_manager_.getLogger("state_initializer")->info(
            "{}Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
            epoch_prefix, sys_name, isb_estimate, isb_estimate / Constants::SPEED_OF_LIGHT,
            residuals.size(), system_residuals[gps_key].size());
    }

    state_manager_.setStateVector(x_updated);
    return true;   // GPS reference present, so the ISB seeds are final
}

// ===========================================================================
// Cold-start receiver-clock pre-seed (network path; opt-in)
// ===========================================================================
//
// The receiver clock state is the GPS-referenced offset (ISB carries every
// other system's delta), so an estimator that has not yet been fed a code clock
// holds x(clock)=0.  On the network path nothing pre-solves it before
// ambiguities are first seeded, so computeAmbiguitySeed subtracts clk_term=0
// and the whole free-running clock offset enters every cold ambiguity seed
// uniformly.  Seeding x(clock) = median GPS code residual removes it.
//
// The GPS-code collection mirrors initializeInterSystemBiases: that routine
// computes the same median (its gps_median reference) but uses it only as a
// per-system subtraction anchor and then discards it.  The discarded value is
// the clock that the network path otherwise never writes.

bool StateInitializer::seedColdReceiverClock(const std::vector<PppObservation>& observations,
    const std::string& epoch_prefix) {

    if (!state_manager_.hasClock())
        return true;   // no clock state in this configuration: nothing to seed or retry

    // Sub-micron threshold below which the clock state is treated as cold
    // (state-init 0.0).  A real seeded receiver clock (PPP path, or a later
    // epoch after the filter update) is metres-to-kilometres, never this small,
    // so the guard never overwrites an already-solved clock.
    constexpr double kColdClockEpsilonM = 1e-6;

    const Eigen::VectorXd x = state_manager_.getStateVector();
    if (std::abs(x(static_cast<Eigen::Index>(state_manager_.getClockIndex())))
            > kColdClockEpsilonM) {
        log_manager_.getLogger("state_initializer")->debug(
            "{}Cold receiver-clock seed skipped: clock already {:.3f} m (not cold)",
            epoch_prefix,
            x(static_cast<Eigen::Index>(state_manager_.getClockIndex())));
        return true;   // already seeded (e.g. PPP path): latch, do not overwrite
    }

    std::vector<double> gps_residuals;
    for (const auto& obs : observations) {
        if (obs.system != System::GPS) continue;   // GPS defines the clock datum
        if (obs.elevation < config_.getElevationMaskDeg()) continue;

        for (const auto& [ref_type, observed] : obs.observed) {
            if (obsTypeOf(ref_type) != ObsType::Code) continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (config_.isUncombinedEnabled()) {
                if (has_underscore) continue;   // uncombined uses raw single-band code
            } else {
                if (!has_underscore) continue;  // IF mode uses the iono-free code combo
            }
            if (!obs.modeled.count(ref_type)) continue;

            gps_residuals.push_back(computeRawResidual(obs, ref_type, x));
            break;   // one code obs per satellite (robust median over satellites)
        }
    }

    if (gps_residuals.empty()) {
        log_manager_.getLogger("state_initializer")->warn(
            "{}Cannot pre-seed cold receiver clock: no GPS code observations this epoch",
            epoch_prefix);
        return false;   // retry next epoch; the caller does not latch
    }

    const double clock_seed = computeMedian(gps_residuals);

    Eigen::VectorXd x_updated = state_manager_.getStateVector();
    x_updated(static_cast<Eigen::Index>(state_manager_.getClockIndex())) = clock_seed;
    state_manager_.setStateVector(x_updated);

    log_manager_.getLogger("state_initializer")->info(
        "{}Pre-seeded cold receiver clock: {:.3f} m ({:.3e} s) from {} GPS code obs",
        epoch_prefix, clock_seed, clock_seed / Constants::SPEED_OF_LIGHT,
        gps_residuals.size());
    return true;
}

// ===========================================================================
// First-epoch phase clock offset initialization
// ===========================================================================

void StateInitializer::initializePhaseClockOffset(const std::vector<PppObservation>& observations,
    const std::string& epoch_prefix) {

    const Eigen::VectorXd x = state_manager_.getStateVector();
    std::vector<double> code_phase_diffs;

    for (const auto& obs : observations) {
        if (obs.elevation < config_.getElevationMaskDeg()) continue;

        double code_residual = 0.0, phase_residual = 0.0;
        bool have_code = false, have_phase = false;

        for (const auto& [ref_type, observed] : obs.observed) {
            if (ref_type.empty()) continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (config_.isUncombinedEnabled()) {
                if (has_underscore) continue;
            } else {
                if (!has_underscore) continue;
            }
            if (!obs.modeled.count(ref_type)) continue;

            const double residual = computeRawResidual(obs, ref_type, x);

            const ObsType type = obsTypeOf(ref_type);
            if (type == ObsType::Code && !have_code)   { code_residual  = residual; have_code  = true; }
            if (type == ObsType::Phase && !have_phase) { phase_residual = residual; have_phase = true; }
        }

        if (have_code && have_phase)
            code_phase_diffs.push_back(code_residual - phase_residual);
    }

    if (code_phase_diffs.empty()) {
        log_manager_.getLogger("state_initializer")->warn(
            "{}Cannot initialize phase clock offset: no satellite has both code and phase observations",
            epoch_prefix);
        return;
    }

    const double phase_clock_seed = computeMedian(code_phase_diffs);

    Eigen::VectorXd x_updated = state_manager_.getStateVector();
    x_updated(state_manager_.getPhaseClockIndex()) = phase_clock_seed;
    state_manager_.setStateVector(x_updated);

    // Tighten the phase clock covariance to reflect the seed accuracy.
    {
        const double code_std = config_.getDefaultMeasurementNoise().code_std_m;
        const double seed_std = code_std / std::sqrt(static_cast<double>(code_phase_diffs.size()));
        const double clamped_std = std::max(seed_std, code_std);
        const size_t pc_idx = state_manager_.getPhaseClockIndex();

        Eigen::MatrixXd P = state_manager_.getCovariance();
        P.row(pc_idx).setZero();
        P.col(pc_idx).setZero();
        P(pc_idx, pc_idx) = clamped_std * clamped_std;
        state_manager_.setCovariance(P);

        log_manager_.getLogger("state_initializer")->info(
            "{}Set phase clock covariance: std={:.3f} m (seed_std={:.3f} m from {} pairs)",
            epoch_prefix, clamped_std, seed_std, code_phase_diffs.size());
    }

    log_manager_.getLogger("state_initializer")->info(
        "{}Initialized phase clock offset: {:.3f} m from {} code-phase pairs",
        epoch_prefix, phase_clock_seed, code_phase_diffs.size());
}

// ===========================================================================
// First-epoch code bias (DCB/IFB) initialization
// ===========================================================================

void StateInitializer::initializeCodeBiases(const std::vector<PppObservation>& observations,
    const std::string& epoch_prefix) {

    if (!config_.estimateIfb() || !config_.isUncombinedEnabled()) return;

    const Eigen::VectorXd x = state_manager_.getStateVector();
    auto log = log_manager_.getLogger("state_initializer");

    struct BiasSample {
        double residual_diff;
        double iono_diff;
        double beta_diff;
    };
    std::map<std::string, std::vector<BiasSample>> samples;

    for (const auto& obs : observations) {
        if (obs.elevation < config_.getElevationMaskDeg()) continue;

        struct BandResidual {
            double residual;
            double alpha;
            ReceiverBiasEntry bias;
        };
        std::map<char, BandResidual> band_residuals;
        for (const auto& [ref_type, val] : obs.observed) {
            // One single-band code ("C1C"); an ionosphere-free pair ("C1C_C2W") does not
            // parse as one code and is skipped.
            const auto code = ObservableId::parse(obs.system, ref_type);
            if (!code || code->type != ObsType::Code) continue;
            if (!obs.modeled.count(ref_type)) continue;

            const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq <= 0.0) continue;
            const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
            const double alpha = (f_ref * f_ref) / (freq * freq);

            const auto bias = resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;

            const double residual = computeRawResidual(obs, ref_type, x);
            if (!std::isfinite(residual)) continue;

            band_residuals[code->signal.digit] = { residual, alpha, bias };
        }

        if (band_residuals.size() < 2) continue;

        const char sys_char = systemToClockRefChar(obs.system);
        const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
        auto crit = clock_refs.find(sys_char);
        if (crit == clock_refs.end()) continue;
        const auto ref1_sig = SignalId::fromObsCode(obs.system, crit->second.sig1);
        if (!ref1_sig) continue;
        const char ref1_band = ref1_sig->digit;   // the first clock-reference band

        auto ref1_it = band_residuals.find(ref1_band);
        if (ref1_it == band_residuals.end()) continue;
        const auto& ref1 = ref1_it->second;

        double iono_est = 0.0;
        {
            const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size()))
                iono_est = x(iono_idx);
        }

        for (const auto& [band_digit, br] : band_residuals) {
            if (band_digit == ref1_band) continue;

            const double r_diff = br.residual - ref1.residual;
            const double iono_diff = (br.alpha - ref1.alpha) * iono_est;
            const double beta_diff = br.bias.coefficient - ref1.bias.coefficient;

            if (std::abs(beta_diff) < 1e-12) continue;

            samples[br.bias.state_key].push_back({ r_diff, iono_diff, beta_diff });
        }
    }

    if (samples.empty()) return;

    Eigen::VectorXd x_updated = x;
    for (const auto& [key, sv] : samples) {
        // Linearization-point restore (RS-9; docs/SPEC_event_harness.md §21.18).
        // A warm resume that restored this station's rx-CB carry restored the
        // carry's numerator but not the reference it is judged against: the
        // inject gate's statistic is |value_abs − x_lin(cb)| against a fixed
        // 5.13 m bar, and x_lin(cb) is the one-shot seed computed below, taken
        // from the resume epoch's code instead of the writer's first epoch.
        // With the override installed, a covered key takes the writer's
        // linearization point and the median seed below is not computed.  This
        // is an exact solve no-op by construction (a code bias enters linearly,
        // so an x_lin shift is absorbed by the state;
        // project_network_xlin_invariance); the only consumer of the value is
        // the gate.  network.rx_code_bias_datum is the one mechanism whose RHS
        // also reads x_lin (η = λ(0 − x_lin)); the loader refuses that pair, so
        // it cannot be reached here.
        // Only keys the map carries a reference for are overridden.  The RL rows
        // pair 1:1 with the RC rows, so a key absent here has no rx-CB carry, the
        // gate never differences anything against its x_lin, and it takes the
        // median seed (a GPS L1/L2/L5 station holds GPS_DCB, carried, and
        // GPS_IFB5, not).  `uncovered` therefore counts only a carried key whose
        // restored reference could not be applied (such a key falls through to
        // the median seed); it should read 0.
        if (codeBiasSeedOverrideCovers(code_bias_seed_override_, key)) {
            bool seeded = false;
            const auto ovr = resolveCodeBiasSeedOverride(code_bias_seed_override_, key);
            if (ovr.has_value()) {
                const int ov_idx = state_manager_.getCodeBiasIndex(key);
                if (ov_idx >= 0 && ov_idx < static_cast<int>(x_updated.size())) {
                    x_updated(ov_idx) = *ovr;
                    if (cb_seed_override_applied_) ++*cb_seed_override_applied_;
                    log->info("{}RS-9: code bias {} seeded from the RESTORED "
                              "linearization point: {:.6f} m (the one-epoch code "
                              "median is NOT used)", epoch_prefix, key, *ovr);
                    seeded = true;
                }
            }
            if (seeded) continue;
            if (cb_seed_override_uncovered_) ++*cb_seed_override_uncovered_;
        }
        std::vector<double> estimates;
        estimates.reserve(sv.size());
        for (const auto& s : sv) {
            const double est = (s.residual_diff - s.iono_diff) / s.beta_diff;
            if (std::isfinite(est) && std::abs(est) < 200.0)
                estimates.push_back(est);
        }
        if (estimates.empty()) continue;

        const double seed = computeMedian(estimates);
        const int cb_idx = state_manager_.getCodeBiasIndex(key);
        if (cb_idx >= 0 && cb_idx < static_cast<int>(x_updated.size())) {
            x_updated(cb_idx) = seed;
            log->info("{}Initialized code bias {}: {:.3f} m from {} satellites",
                epoch_prefix, key, seed, estimates.size());
        }
    }
    state_manager_.setStateVector(x_updated);
}

} // namespace GnssProcessor
