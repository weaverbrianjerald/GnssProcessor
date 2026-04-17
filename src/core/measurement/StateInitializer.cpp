/**
 * @file StateInitializer.cpp
 * @brief First-epoch state seeding for ISB, phase clock, and code biases.
 */

#include "StateInitializer.hpp"
#include "PppModel.hpp"
#include "ProductCache.hpp"
#include "Constants.hpp"
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
    switch (system) {
    case System::GPS:     return "";
    case System::Galileo: return "Galileo";
    case System::GLONASS: return "GLONASS";
    case System::BeiDou:  return "BeiDou";
    case System::QZSS:    return "QZSS";
    case System::SBAS:    return "SBAS";
    default:              return "";
    }
}

char StateInitializer::systemToClockRefChar(System system) {
    switch (system) {
    case System::GPS:     return 'G';
    case System::Galileo: return 'E';
    case System::GLONASS: return 'R';
    case System::BeiDou:  return 'C';
    case System::QZSS:    return 'J';
    case System::SBAS:    return 'S';
    default:              return '\0';
    }
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

    const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
    auto it = clock_refs.find(sys_char);
    if (it == clock_refs.end()) return result;

    const auto& ref = it->second;
    const std::string sys = PppUtils::getSystemString(system);

    const char band_j = (obs_signal.size() >= 2) ? obs_signal[1] : '\0';
    const char band_1 = (ref.sig1.size() >= 2) ? ref.sig1[1] : '\0';
    const char band_2 = (ref.sig2.size() >= 2) ? ref.sig2[1] : '\0';
    if (band_j == '\0' || band_1 == '\0' || band_2 == '\0') return result;

    // Non-reference band: independent IFB state
    if (band_j != band_1 && band_j != band_2) {
        result.state_key   = sys + "_IFB" + band_j;
        result.coefficient = 1.0;
        return result;
    }

    // Reference band: GF-IF coefficient against the system DCB state
    const double f1 = ppp_utils_.getFrequency(ref.sig1, system, satellite);
    const double f2 = ppp_utils_.getFrequency(ref.sig2, system, satellite);
    if (!(std::isfinite(f1) && f1 > 0.0 && std::isfinite(f2) && f2 > 0.0))
        return result;

    const double mu1 = 1.0;
    const double mu2 = (f1 * f1) / (f2 * f2);
    const double dmu = mu1 - mu2;
    if (std::abs(dmu) < 1e-12) return result;

    result.state_key = sys + "_DCB";
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
    const double rx_clk   = x(state_manager_.getClockIndex());
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
    if (config_.isUncombinedEnabled() && !ref_type.empty() && (ref_type[0] == 'C' || ref_type[0] == 'L')) {
        const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
        if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
            const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq > 0.0) {
                const double f_ref = Constants::GPS_L1_FREQ;
                const double alpha = (f_ref * f_ref) / (freq * freq);
                iono = (ref_type[0] == 'L' ? -1.0 : 1.0) * alpha * x(iono_idx);
            }
        }
    }

    // Receiver code bias (DCB / IFB — code only, uncombined)
    double code_bias = 0.0;
    if (config_.estimateIfb() && config_.isUncombinedEnabled()
        && !ref_type.empty() && ref_type[0] == 'C') {
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
            ? (!is_if && ref_type[0] == 'C')
            : (is_if && ref_type[0] == 'C');
        if (applicable) {
            const std::string sb_key = obs.satellite + "|" + ref_type;
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

double StateInitializer::computeIonosphereSeed(const PppObservation& obs) const {
    struct CodeObs { std::string key; double value; double freq; char band; };
    std::vector<CodeObs> codes;
    for (const auto& [key, val] : obs.observed) {
        if (key.size() < 2 || key[0] != 'C') continue;
        if (!obs.modeled.count(key)) continue;
        const double freq = ppp_utils_.getFrequency(key, obs.system, obs.satellite);
        if (freq <= 0.0) continue;
        codes.push_back({key, val, freq, key[1]});
    }
    if (codes.size() < 2) return 0.0;

    for (size_t i = 0; i < codes.size(); ++i) {
        for (size_t j = i + 1; j < codes.size(); ++j) {
            if (codes[i].band == codes[j].band) continue;
            const double f1 = codes[i].freq;
            const double f2 = codes[j].freq;
            const double P1 = codes[i].value;
            const double P2 = codes[j].value;
            const double mod1 = obs.modeled.at(codes[i].key);
            const double mod2 = obs.modeled.at(codes[j].key);

            const double f_ref = Constants::GPS_L1_FREQ;
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
    return 0.0;
}

// ===========================================================================
// First-epoch ISB initialization
// ===========================================================================

void StateInitializer::initializeInterSystemBiases(const std::vector<PppObservation>& observations,
    const std::string& epoch_prefix) {

    std::map<std::string, std::vector<double>> system_residuals;
    const Eigen::VectorXd x = state_manager_.getStateVector();

    for (const auto& obs : observations) {
        if (obs.elevation < config_.getElevationMaskDeg()) continue;

        for (const auto& [ref_type, observed] : obs.observed) {
            if (ref_type.empty() || ref_type[0] != 'C') continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (config_.isUncombinedEnabled()) {
                if (has_underscore) continue;
            } else {
                if (!has_underscore) continue;
            }
            if (!obs.modeled.count(ref_type)) continue;

            const double residual = computeRawResidual(obs, ref_type, x);
            const std::string sys_name = systemToIsbName(obs.system);
            const std::string key = sys_name.empty() ? "GPS" : sys_name;
            system_residuals[key].push_back(residual);
            break;  // one code obs per satellite
        }
    }

    if (system_residuals.find("GPS") == system_residuals.end() ||
        system_residuals["GPS"].empty()) {
        log_manager_.getLogger("state_initializer")->warn(
            "{}Cannot initialize ISB: no GPS code observations", epoch_prefix);
        return;
    }

    double gps_median = computeMedian(system_residuals["GPS"]);
    Eigen::VectorXd x_updated = state_manager_.getStateVector();

    for (auto& [sys_name, residuals] : system_residuals) {
        if (sys_name == "GPS" || residuals.empty()) continue;

        const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx >= static_cast<size_t>(x_updated.size())) continue;

        const double isb_estimate = computeMedian(residuals) - gps_median;
        x_updated(isb_idx) = isb_estimate;

        log_manager_.getLogger("state_initializer")->info(
            "{}Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
            epoch_prefix, sys_name, isb_estimate, isb_estimate / Constants::SPEED_OF_LIGHT,
            residuals.size(), system_residuals["GPS"].size());
    }

    state_manager_.setStateVector(x_updated);
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

            if (ref_type[0] == 'C' && !have_code)  { code_residual  = residual; have_code  = true; }
            if (ref_type[0] == 'L' && !have_phase) { phase_residual = residual; have_phase = true; }
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
            if (ref_type.empty() || ref_type[0] != 'C' || ref_type.size() < 2) continue;
            if (ref_type.find('_') != std::string::npos) continue;
            if (!obs.modeled.count(ref_type)) continue;

            const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq <= 0.0) continue;
            const double f_ref = Constants::GPS_L1_FREQ;
            const double alpha = (f_ref * f_ref) / (freq * freq);

            const auto bias = resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;

            const double residual = computeRawResidual(obs, ref_type, x);
            if (!std::isfinite(residual)) continue;

            const char band_digit = ref_type[1];
            band_residuals[band_digit] = { residual, alpha, bias };
        }

        if (band_residuals.size() < 2) continue;

        const char sys_char = systemToClockRefChar(obs.system);
        const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
        auto crit = clock_refs.find(sys_char);
        if (crit == clock_refs.end()) continue;
        const char ref1_band = (crit->second.sig1.size() >= 2) ? crit->second.sig1[1] : '\0';
        if (ref1_band == '\0') continue;

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
