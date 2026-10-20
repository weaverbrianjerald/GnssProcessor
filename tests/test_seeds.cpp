/**
 * @file test_seeds.cpp
 * @brief The seeds (measurement/Seeds.hpp) against the code they replaced, bit for bit.
 *
 * The seeds used to be written out twice, in @c StateInitializer::computeRawResidual and
 * @c StateManager::computeAmbiguitySeed, and the first-epoch seeds were methods that wrote into
 * the state manager.  This file keeps the old code as a reference (namespace @c legacy, copied
 * from the sources it replaced, with the writes into the state replaced by returned values) and
 * compares the new functions to it over each posture the seeds have a branch for:
 *  - the carrier frequency of the slant ionosphere term, per constellation and band: the same
 *    number outside GLONASS FDMA bands, and channel 0 under @c FdmaChannel::Zero;
 *  - the raw residual and the ambiguity seed of every row of an epoch, bit for bit;
 *  - the order the terms are subtracted in (the inter-system bias before the gradients);
 *  - the inter-system bias, cold clock and phase-clock seeds;
 *  - the effect sets of @c observationEquation.
 *
 *  - the receiver code-bias seed (median over satellites, the warm resume's restored
 *    linearization points taken, refused and counted, the elevation mask, the 200 m limit) and
 *    the slant ionosphere seed (the bias-product presence gate, the configuration's
 *    application gate, the pair choice, the 200 m limit).
 *
 * Both of those read the .BIA product: the clock-reference observables of the code-bias seed
 * and the presence of entries for the ionosphere seed.  They are inputs of the functions
 * (@c ObsInput::clock_refs, the presence predicate), so each case supplies its own and none
 * depends on the process-wide product cache.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/PppUtils.hpp"
#include "filter/StateKeySchema.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "support/SameBits.hpp"
#include "measurement/Seeds.hpp"
#include "measurement/StateInitializer.hpp"
#include "measurement/UserParamView.hpp"
#include "support/ModelStack.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;
using gnss_test::sameBits;
namespace K = GnssProcessor::StateKeys;

namespace {

// ===========================================================================
// The code the seeds replaced
// ===========================================================================

namespace legacy {

/// StateManager.cpp's file-local ISB name table (empty for GPS, the reference).
std::string stateManagerIsbName(System system) {
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

/// StateManager.cpp's file-local carrier frequency table: the GLONASS FDMA bands at slot 0.
double bandFrequency(char band, System system) {
    switch (system) {
    case System::GPS:
        if (band == '2') return Constants::GPS_L2_FREQ;
        if (band == '5') return Constants::GPS_L5_FREQ;
        return Constants::GPS_L1_FREQ;
    case System::QZSS:
        if (band == '2') return Constants::QZSS_L2_FREQ;
        if (band == '5') return Constants::QZSS_L5_FREQ;
        if (band == '6') return Constants::QZSS_L6_FREQ;
        return Constants::QZSS_L1_FREQ;
    case System::SBAS:
        if (band == '5') return Constants::SBAS_L5_FREQ;
        return Constants::SBAS_L1_FREQ;
    case System::Galileo:
        if (band == '5') return Constants::GAL_E5A_FREQ;
        if (band == '7') return Constants::GAL_E5B_FREQ;
        if (band == '8') return Constants::GAL_E5_FREQ;
        if (band == '6') return Constants::GAL_E6_FREQ;
        return Constants::GPS_L1_FREQ;
    case System::BeiDou:
        if (band == '1') return Constants::BDS_B1C_FREQ;
        if (band == '2') return Constants::BDS_B1_FREQ;
        if (band == '5') return Constants::BDS_B2A_FREQ;
        if (band == '7') return Constants::BDS_B2_FREQ;
        if (band == '8') return Constants::BDS_B2AB_FREQ;
        if (band == '6') return Constants::BDS_B3_FREQ;
        return Constants::BDS_B1C_FREQ;
    case System::GLONASS:
        if (band == '2') return Constants::GLONASS_G2_BASE_FREQ;
        if (band == '3') return Constants::GLONASS_G3_BASE_FREQ;
        if (band == '4') return Constants::GLONASS_G1A_FREQ;
        if (band == '6') return Constants::GLONASS_G2A_FREQ;
        return Constants::GLONASS_G1_BASE_FREQ;
    default:
        return Constants::GPS_L1_FREQ;
    }
}

/// The median of a mutable vector (StateInitializer.cpp's helper).
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

/// What the old functions read: the state layout, the configuration, the initializer (whose
/// own frequency helper was the one computeRawResidual scaled the ionosphere with) and the
/// clock-reference observables (the old code took them from the process-wide product cache).
struct World {
    const StateManager&     sm;
    const GnssConfig&       config;
    const StateInitializer& init;
    const ClockRefMap&      clock_refs;   ///< The .BIA clock-reference observables the old code read from the product cache.
};

/// StateManager::computeAmbiguitySeed, as it was.
double computeAmbiguitySeed(const World& w, const PppObservation& obs, const std::string& ref_type,
                            const Eigen::VectorXd& x) {
    const StateManager& sm = w.sm;
    try {
        if (!obs.observed.count(ref_type) || !obs.modeled.count(ref_type))
            return 0.0;

        const double observed_m = obs.observed.at(ref_type);
        const double modeled_m  = obs.modeled.at(ref_type);
        const double tropo_wet  = obs.mf_w * x(sm.getTropoIndex());

        double clk_term = sm.hasClock() ? x(sm.getClockIndex()) : 0.0;
        if (sm.hasPhaseClock())
            clk_term += x(sm.getPhaseClockIndex());

        double isb = 0.0;
        {
            const std::string isb_name = stateManagerIsbName(obs.system);
            const size_t isb_idx = sm.getSystemTimeOffsetIndex(isb_name);
            if (isb_idx < static_cast<size_t>(x.size()))
                isb = x(static_cast<Eigen::Index>(isb_idx));
        }

        double tropo_grad = 0.0;
        if (sm.hasTropoGradients()) {
            const double el_rad = obs.elevation * Constants::PI / 180.0;
            const double az_rad = obs.azimuth  * Constants::PI / 180.0;
            const double mf_grad = 1.0 / (std::sin(el_rad) * std::tan(el_rad) + 0.0032);
            const double gn = x(static_cast<Eigen::Index>(sm.getTropoGradientNorthIndex()));
            const double ge = x(static_cast<Eigen::Index>(sm.getTropoGradientEastIndex()));
            tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
        }

        double iono_term = 0.0;
        if (w.config.isUncombinedEnabled() && !ref_type.empty() && ref_type[0] == 'L') {
            const int iono_idx = sm.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
                double alpha = 1.0;
                if (ref_type.size() >= 2) {
                    const double freq = bandFrequency(ref_type[1], obs.system);
                    alpha = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (freq * freq);
                }
                iono_term = -alpha * x(iono_idx);
            }
        }
        return observed_m - modeled_m - clk_term - tropo_wet - isb - tropo_grad - iono_term;
    } catch (...) {
        return 0.0;
    }
}

/// StateInitializer::computeRawResidual, as it was.
double computeRawResidual(const World& w, const PppObservation& obs, const std::string& ref_type,
                          const Eigen::VectorXd& x) {
    const StateManager& sm = w.sm;
    const PppUtils& ppp_utils = w.init.frequencyHelper();

    const double observed = obs.observed.at(ref_type);
    const double modeled  = obs.modeled.at(ref_type);
    const double rx_clk   = sm.hasClock() ? x(sm.getClockIndex()) : 0.0;
    const double zwd      = x(sm.getTropoIndex());
    const double tropo_wet = obs.mf_w * zwd;

    double isb = 0.0;
    const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
    if (!sys_name.empty()) {
        const size_t isb_idx = sm.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx < static_cast<size_t>(x.size()))
            isb = x(static_cast<Eigen::Index>(isb_idx));
    }

    double tropo_grad = 0.0;
    if (sm.hasTropoGradients()) {
        const double el_rad = obs.elevation * Constants::PI / 180.0;
        const double az_rad = obs.azimuth * Constants::PI / 180.0;
        const double sin_el = std::sin(el_rad);
        const double tan_el = std::tan(el_rad);
        const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
        const double gn = x(static_cast<Eigen::Index>(sm.getTropoGradientNorthIndex()));
        const double ge = x(static_cast<Eigen::Index>(sm.getTropoGradientEastIndex()));
        tropo_grad = mf_grad * (gn * std::cos(az_rad) + ge * std::sin(az_rad));
    }

    double iono = 0.0;
    const ObsType ref_obs_type = obsTypeOf(ref_type);
    if (w.config.isUncombinedEnabled() && !ref_type.empty()
        && (ref_obs_type == ObsType::Code || ref_obs_type == ObsType::Phase)) {
        const int iono_idx = sm.getIonosphereIndex(obs.satellite);
        if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size())) {
            const double freq = ppp_utils.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq > 0.0) {
                const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
                const double alpha = (f_ref * f_ref) / (freq * freq);
                iono = (ref_obs_type == ObsType::Phase ? -1.0 : 1.0) * alpha * x(iono_idx);
            }
        }
    }

    double code_bias = 0.0;
    if (w.config.estimateIfb() && w.config.isUncombinedEnabled()
        && !ref_type.empty() && ref_obs_type == ObsType::Code) {
        const auto bias = w.init.resolveReceiverBias(ref_type, obs.system, obs.satellite,
                                                     w.clock_refs);
        if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
            const int cb_idx = sm.getCodeBiasIndex(bias.state_key);
            if (cb_idx >= 0 && cb_idx < static_cast<int>(x.size()))
                code_bias = bias.coefficient * x(cb_idx);
        }
    }

    double sat_bias = 0.0;
    if (w.config.estimateSatBiases() && !ref_type.empty()) {
        const bool is_if = (ref_type.find('_') != std::string::npos);
        const bool applicable = w.config.isUncombinedEnabled()
            ? (!is_if && ref_obs_type == ObsType::Code)
            : (is_if && ref_obs_type == ObsType::Code);
        if (applicable) {
            const std::string sb_key = StateKeys::makeSatBiasKey(obs.satellite, ref_type);
            const int sb_idx = sm.getSatBiasIndex(sb_key);
            if (sb_idx >= 0 && sb_idx < static_cast<int>(x.size()))
                sat_bias = x(sb_idx);
        }
    }

    return observed - modeled - rx_clk - tropo_wet - isb - tropo_grad
         - iono - code_bias - sat_bias;
}

/// The rows initializeInterSystemBiases and seedColdReceiverClock took residuals of.
bool isCodeRowOfMode(const World& w, const std::string& ref_type) {
    if (obsTypeOf(ref_type) != ObsType::Code) return false;
    const bool has_underscore = (ref_type.find('_') != std::string::npos);
    if (w.config.isUncombinedEnabled()) {
        if (has_underscore) return false;
    } else {
        if (!has_underscore) return false;
    }
    return true;
}

struct IsbValue {
    std::string name;
    double      value;
    size_t      n;
};

/// StateInitializer::initializeInterSystemBiases with the writes returned.
/// @return False when the time reference had no code (the old function returned false).
bool initializeInterSystemBiases(const World& w, const std::vector<PppObservation>& observations,
                                 std::vector<IsbValue>& out, size_t& reference_n) {
    std::map<std::string, std::vector<double>> system_residuals;
    const Eigen::VectorXd x = w.sm.getStateVector();
    for (const auto& obs : observations) {
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRowOfMode(w, ref_type)) continue;
            if (!obs.modeled.count(ref_type)) continue;
            const double residual = computeRawResidual(w, obs, ref_type, x);
            const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
            const std::string key = sys_name.empty() ? systemName(timeReferenceSystem()) : sys_name;
            system_residuals[key].push_back(residual);
            break;
        }
    }
    const std::string gps_key = systemName(timeReferenceSystem());
    if (system_residuals.find(gps_key) == system_residuals.end() ||
        system_residuals[gps_key].empty())
        return false;
    const double gps_median = computeMedian(system_residuals[gps_key]);
    reference_n = system_residuals[gps_key].size();
    for (auto& [sys_name, residuals] : system_residuals) {
        if (sys_name == gps_key || residuals.empty()) continue;
        const size_t isb_idx = w.sm.getSystemTimeOffsetIndex(sys_name);
        if (isb_idx >= static_cast<size_t>(x.size())) continue;
        out.push_back({sys_name, computeMedian(residuals) - gps_median, residuals.size()});
    }
    return true;
}

/// How seedColdReceiverClock ended, with the value it would have written.
struct ColdClock {
    bool   settled{false};
    bool   seeded{false};
    double value{0.0};
    size_t n{0};
};

/// StateInitializer::seedColdReceiverClock with the write returned.
ColdClock seedColdReceiverClock(const World& w, const std::vector<PppObservation>& observations) {
    ColdClock out;
    if (!w.sm.hasClock()) { out.settled = true; return out; }
    constexpr double kColdClockEpsilonM = 1e-6;
    const Eigen::VectorXd x = w.sm.getStateVector();
    if (std::abs(x(static_cast<Eigen::Index>(w.sm.getClockIndex()))) > kColdClockEpsilonM) {
        out.settled = true;
        return out;
    }
    std::vector<double> gps_residuals;
    for (const auto& obs : observations) {
        if (!isTimeReference(obs.system)) continue;
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (!isCodeRowOfMode(w, ref_type)) continue;
            if (!obs.modeled.count(ref_type)) continue;
            gps_residuals.push_back(computeRawResidual(w, obs, ref_type, x));
            break;
        }
    }
    if (gps_residuals.empty()) return out;
    out.settled = true;
    out.seeded  = true;
    out.n       = gps_residuals.size();
    out.value   = computeMedian(gps_residuals);
    return out;
}

/// StateInitializer::initializePhaseClockOffset with the write returned.
/// @return Whether any satellite had both a code and a phase residual.
bool initializePhaseClockOffset(const World& w, const std::vector<PppObservation>& observations,
                                double& value, size_t& n_pairs) {
    const Eigen::VectorXd x = w.sm.getStateVector();
    std::vector<double> code_phase_diffs;
    for (const auto& obs : observations) {
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;
        double code_residual = 0.0, phase_residual = 0.0;
        bool have_code = false, have_phase = false;
        for (const auto& [ref_type, observed] : obs.observed) {
            if (ref_type.empty()) continue;
            const bool has_underscore = (ref_type.find('_') != std::string::npos);
            if (w.config.isUncombinedEnabled()) {
                if (has_underscore) continue;
            } else {
                if (!has_underscore) continue;
            }
            if (!obs.modeled.count(ref_type)) continue;
            const double residual = computeRawResidual(w, obs, ref_type, x);
            const ObsType type = obsTypeOf(ref_type);
            if (type == ObsType::Code && !have_code)   { code_residual  = residual; have_code  = true; }
            if (type == ObsType::Phase && !have_phase) { phase_residual = residual; have_phase = true; }
        }
        if (have_code && have_phase)
            code_phase_diffs.push_back(code_residual - phase_residual);
    }
    if (code_phase_diffs.empty()) return false;
    n_pairs = code_phase_diffs.size();
    value   = computeMedian(code_phase_diffs);
    return true;
}

/// A slant ionosphere seed with the pair it came from.
struct IonosphereValue {
    double      value;
    std::string code_a;
    std::string code_b;
    double      range_a;
    double      range_b;
    double      alpha_a;
    double      alpha_b;
};

/// StateInitializer::computeIonosphereSeed, as it was, with the log line replaced by the pair
/// it came from and the model's table of .BIA entries replaced by @p has_entry.
std::optional<IonosphereValue> computeIonosphereSeed(const World& w, const PppObservation& obs,
                                                     const BiasPresence& has_entry) {
    const PppUtils& ppp_utils = w.init.frequencyHelper();
    struct CodeObs { std::string key; double value; double freq; char band; };
    std::vector<CodeObs> codes;
    for (const auto& [key, val] : obs.observed) {
        const auto code = ObservableId::parse(obs.system, key);
        if (!code || code->type != ObsType::Code) continue;
        if (!obs.modeled.count(key)) continue;
        if (!has_entry(obs.satellite, key)) continue;
        if (w.config.getModelCorrections().bias_coverage_requires_apply
            && w.config.getProcessingMode() != ProcessingMode::PPP_RTK_Network
            && !w.config.getModelCorrections().enable_code_phase_bias) continue;
        const double freq = ppp_utils.getFrequency(key, obs.system, obs.satellite);
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
            if (std::isfinite(iono_seed) && std::abs(iono_seed) < 200.0)
                return IonosphereValue{iono_seed, codes[i].key, codes[j].key, P1 - mod1,
                                       P2 - mod2, alpha1, alpha2};
        }
    }
    return std::nullopt;
}

/// What StateInitializer::initializeCodeBiases wrote and counted.
struct CodeBiasValues {
    std::map<std::string, double>      value;      ///< Value written, by state key.
    std::map<std::string, std::size_t> n_sats;     ///< Satellites in the median (median keys only).
    std::set<std::string>              restored;   ///< Keys seeded from the installed map.
    long long                          applied{0};   ///< The applied out-slot's increments.
    long long                          uncovered{0}; ///< The uncovered out-slot's increments.
};

/// StateInitializer::initializeCodeBiases, as it was, with the writes into the state and the
/// out-slots returned, the clock-reference observables taken from @p w instead of the product
/// cache, and the installed map passed in.
CodeBiasValues initializeCodeBiases(const World& w, const std::vector<PppObservation>& observations,
                                    const std::map<std::string, double>& installed) {
    CodeBiasValues out;
    if (!w.config.estimateIfb() || !w.config.isUncombinedEnabled()) return out;

    const Eigen::VectorXd x = w.sm.getStateVector();
    const PppUtils& ppp_utils = w.init.frequencyHelper();

    struct BiasSample {
        double residual_diff;
        double iono_diff;
        double beta_diff;
    };
    std::map<std::string, std::vector<BiasSample>> samples;

    for (const auto& obs : observations) {
        if (obs.elevation < w.config.getElevationMaskDeg()) continue;

        struct BandResidual {
            double residual;
            double alpha;
            StateInitializer::ReceiverBiasEntry bias;
        };
        std::map<char, BandResidual> band_residuals;
        for (const auto& [ref_type, val] : obs.observed) {
            const auto code = ObservableId::parse(obs.system, ref_type);
            if (!code || code->type != ObsType::Code) continue;
            if (!obs.modeled.count(ref_type)) continue;

            const double freq = ppp_utils.getFrequency(ref_type, obs.system, obs.satellite);
            if (freq <= 0.0) continue;
            const double f_ref = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
            const double alpha = (f_ref * f_ref) / (freq * freq);

            const auto bias = w.init.resolveReceiverBias(ref_type, obs.system, obs.satellite,
                                                         w.clock_refs);
            if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;

            const double residual = computeRawResidual(w, obs, ref_type, x);
            if (!std::isfinite(residual)) continue;

            band_residuals[code->signal.digit] = {residual, alpha, bias};
        }

        if (band_residuals.size() < 2) continue;

        const char sys_char = StateInitializer::systemToClockRefChar(obs.system);
        auto crit = w.clock_refs.find(sys_char);
        if (crit == w.clock_refs.end()) continue;
        const auto ref1_sig = SignalId::fromObsCode(obs.system, crit->second.sig1);
        if (!ref1_sig) continue;
        const char ref1_band = ref1_sig->digit;

        auto ref1_it = band_residuals.find(ref1_band);
        if (ref1_it == band_residuals.end()) continue;
        const auto& ref1 = ref1_it->second;

        double iono_est = 0.0;
        {
            const int iono_idx = w.sm.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(x.size()))
                iono_est = x(iono_idx);
        }

        for (const auto& [band_digit, br] : band_residuals) {
            if (band_digit == ref1_band) continue;
            const double r_diff = br.residual - ref1.residual;
            const double iono_diff = (br.alpha - ref1.alpha) * iono_est;
            const double beta_diff = br.bias.coefficient - ref1.bias.coefficient;
            if (std::abs(beta_diff) < 1e-12) continue;
            samples[br.bias.state_key].push_back({r_diff, iono_diff, beta_diff});
        }
    }

    if (samples.empty()) return out;

    for (const auto& [key, sv] : samples) {
        if (StateInitializer::codeBiasSeedOverrideCovers(installed, key)) {
            bool seeded = false;
            const auto ovr = StateInitializer::resolveCodeBiasSeedOverride(installed, key);
            if (ovr.has_value()) {
                const int ov_idx = w.sm.getCodeBiasIndex(key);
                if (ov_idx >= 0 && ov_idx < static_cast<int>(x.size())) {
                    out.value[key] = *ovr;
                    out.restored.insert(key);
                    ++out.applied;
                    seeded = true;
                }
            }
            if (seeded) continue;
            ++out.uncovered;
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
        const int cb_idx = w.sm.getCodeBiasIndex(key);
        if (cb_idx >= 0 && cb_idx < static_cast<int>(x.size())) {
            out.value[key] = seed;
            out.n_sats[key] = estimates.size();
        }
    }
    return out;
}

} // namespace legacy

/// Every (digit, attribute) the classifier maps to a known band for @p sys.
std::vector<SignalId> classifiedSignals(System sys) {
    std::vector<SignalId> out;
    for (char d = '0'; d <= '9'; ++d)
        for (char a = 'A'; a <= 'Z'; ++a)
            if (classifySignal(traitsOf(sys).name, std::string{'C', d, a}).band
                != FrequencyBand::Unknown)
                out.push_back(SignalId{sys, d, a});
    return out;
}

const std::vector<System> kAllSystems = {System::GPS, System::Galileo, System::GLONASS,
                                         System::BeiDou, System::QZSS, System::SBAS};

/// A lookup that holds one slant ionosphere parameter, whatever the satellite.
struct IonosphereOnly {
    ParamSlot find(const ParamRef& ref) const {
        return ref.kind == NetworkParamKind::SlantIono ? ParamSlot{7, 3.0} : ParamSlot{};
    }
};

// ===========================================================================
// The fixture
// ===========================================================================

constexpr const char* kSat = "G12";

class SeedsTest : public ::testing::Test {
protected:
    LogManager log;
    gnss_test::ModelStack stack;
    int epoch = 0;

    /// The .BIA clock-reference observables of the posture (empty: no product).  The model
    /// reads this map instead of the process-wide product cache, so no case depends on what
    /// another case loaded.
    ClockRefMap clock_refs;

    GnssConfig& config() { return stack.config; }
    StateManager& states() { return *stack.states; }
    MeasurementModel& model() { return *stack.model; }

    void load(const std::string& mode, const std::string& processing = "",
              const std::string& estimation = "", bool bssd = false,
              const std::string& top = "") {
        ASSERT_TRUE(stack.load(
            log, std::string("seeds_") + ::testing::UnitTest::GetInstance()->current_test_info()->name(),
            mode, processing, estimation, bssd, top));
        model().setClockRefs(&clock_refs);
    }

    PppObservation observation(const std::string& sat, System sys,
                               const std::vector<std::string>& signals, double azimuth = 45.0,
                               double elevation = 45.0, double modeled = 2.2e7) const {
        return gnss_test::syntheticObservation(sat, sys, signals, epoch, azimuth, elevation,
                                               modeled);
    }

    EpochMeasurement update(const std::vector<PppObservation>& obs) {
        ++epoch;
        return model().computeMeasurementUpdate(obs, states().getStateVector());
    }

    legacy::World world() {
        return legacy::World{states(), config(), model().getStateInitializer(), clock_refs};
    }

    /// The new functions' lookup over the current state.
    struct View {
        Eigen::VectorXd  x;
        UserParamView    view;
        View(const StateManager& sm)
            : x(sm.getStateVector()), view(sm, x, static_cast<std::size_t>(x.size())) {}
        ParamLookup lookup() const { return ParamLookup(view); }
    };

    /// Every row of @p observations: the new raw residual and ambiguity seed against the old.
    /// @return The number of rows compared.
    int expectRowsMatchLegacy(const std::vector<PppObservation>& observations) {
        const Eigen::VectorXd x = states().getStateVector();
        const View v(states());
        const ObsInput residual_in = model().residualSeedInput();
        const legacy::World w = world();
        int rows = 0;
        for (const PppObservation& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (!obs.modeled.count(ref_type)) continue;
                const bool row_of_mode =
                    config().isUncombinedEnabled() == (ref_type.find('_') == std::string::npos);
                if (!row_of_mode) continue;
                const std::optional<double> raw = rawResidual(residual_in, v.lookup(), obs, ref_type);
                ++rows;
                EXPECT_TRUE(raw.has_value()) << obs.satellite << " " << ref_type;
                if (raw)
                    EXPECT_TRUE(sameBits(*raw, legacy::computeRawResidual(w, obs, ref_type, x)))
                        << "raw residual " << obs.satellite << " " << ref_type;
                if (ref_type[0] == 'L')
                    EXPECT_TRUE(sameBits(model().ambiguitySeed(obs, ref_type, x),
                                         legacy::computeAmbiguitySeed(w, obs, ref_type, x)))
                        << "ambiguity seed " << obs.satellite << " " << ref_type;
            }
        }
        return rows;
    }

    /// Three epochs of the given satellites, then every state moved off its seed.
    void run(const std::vector<PppObservation>& sats) {
        for (int e = 0; e < 3; ++e) {
            update(sats);
            stack.perturbStates();
        }
    }
};

// ===========================================================================
// The carrier frequency
// ===========================================================================

TEST_F(SeedsTest, TheAmbiguitySeedFrequencyIsTheHelpersOutsideGlonassFdmaBands) {
    load("ppp-rtk");
    const PppUtils utils(log);
    const std::map<std::string, char> no_nlwl;
    for (const System sys : kAllSystems) {
        for (const SignalId& sig : classifiedSignals(sys)) {
            const bool fdma = sys == System::GLONASS && (sig.digit == '1' || sig.digit == '2');
            const std::string sat = SatId{sys, 5}.str();
            const std::string text = std::string{'L', sig.digit, sig.attribute};
            const PppObservation obs = observation(sat, sys, {text});
            const double freq = legacy::bandFrequency(sig.digit, sys);
            const double alpha = (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (freq * freq);

            for (const FdmaChannel rule : {FdmaChannel::Satellite, FdmaChannel::Zero}) {
                const ObsInput in{RowProducts{stack.config, *stack.ppp, stack.store}, utils, nullptr,
                                  no_nlwl, StateAmbiguityBasis::PerBand, clock_refs, rule};
                const ObservationEquation eq =
                    observationEquation(in, obs, text, ParamLookup(IonosphereOnly{}),
                                        effectsOf({ObsEffect::Modeled, ObsEffect::Ionosphere}));
                ASSERT_TRUE(eq.usable) << systemName(sys) << " " << text;
                const ObsAddend* iono = findTerm(eq, ObsEffect::Ionosphere);
                ASSERT_NE(iono, nullptr) << systemName(sys) << " " << text;
                // The old table's number: the helper's for every band but a GLONASS FDMA one,
                // channel 0 under the Zero rule.
                if (!fdma || rule == FdmaChannel::Zero)
                    EXPECT_TRUE(sameBits(iono->part[0].partial, -alpha))
                        << systemName(sys) << " " << text << " rule " << static_cast<int>(rule);
            }
        }
    }
}

TEST_F(SeedsTest, AGlonassFdmaBandOnAnotherChannelDiffersFromChannelZero) {
    load("ppp-rtk");
    PppUtils utils(log);
    utils.setGlonassSlots({{"R05", 4}});
    const std::map<std::string, char> no_nlwl;
    const PppObservation obs = observation("R05", System::GLONASS, {"L1C", "L3I"});
    const RowProducts products{stack.config, *stack.ppp, stack.store};
    const auto partial = [&](const char* signal, FdmaChannel rule) {
        const ObsInput in{products, utils, nullptr, no_nlwl, StateAmbiguityBasis::PerBand, clock_refs, rule};
        return findTerm(observationEquation(in, obs, signal, ParamLookup(IonosphereOnly{}),
                                            effectsOf({ObsEffect::Modeled, ObsEffect::Ionosphere})),
                        ObsEffect::Ionosphere)->part[0].partial;
    };
    // G1 depends on the channel; the CDMA band G3 does not.
    EXPECT_NE(partial("L1C", FdmaChannel::Satellite), partial("L1C", FdmaChannel::Zero));
    EXPECT_TRUE(sameBits(partial("L3I", FdmaChannel::Satellite), partial("L3I", FdmaChannel::Zero)));
}

// ===========================================================================
// The residual and the ambiguity seed
// ===========================================================================

TEST_F(SeedsTest, MultiSystemUncombinedRowsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
    };
    run(sats);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 12);
}

TEST_F(SeedsTest, GradientsPhaseClockAndInterSystemBiasKeepTheOldSumOrder) {
    load("ppp-rtk", "", R"(, "estimate_tropo_gradients": true, "estimate_phase_clock": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 310.0, 21.0),
    };
    run(sats);
    ASSERT_TRUE(states().hasTropoGradients());
    ASSERT_TRUE(states().hasPhaseClock());
    EXPECT_EQ(expectRowsMatchLegacy(sats), 12);
}

TEST_F(SeedsTest, SatelliteCodeBiasStatesMatchTheOldCode) {
    load("ppp-rtk", "", R"(, "estimate_sat_biases": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
    };
    run(sats);
    ASSERT_GE(states().getSatBiasIndex(std::string(kSat) + "|C1W"), 0);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 8);
}

TEST_F(SeedsTest, IonoFreeCombinationRowsMatchTheOldCode) {
    load("ppp", R"(, "uncombined_enabled": false)", R"(, "estimate_sat_biases": true)");
    ASSERT_FALSE(config().isUncombinedEnabled());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W_C2W", "L1W_L2W"}),
        observation("G15", System::GPS, {"C1W_C2W", "L1W_L2W"}, 200.0, 60.0),
    };
    run(sats);
    ASSERT_GE(states().getSatBiasIndex(std::string(kSat) + "|C1W_C2W"), 0);
    EXPECT_EQ(expectRowsMatchLegacy(sats), 4);
}

TEST_F(SeedsTest, ARowWithoutAnAmbiguityStillHasASeed) {
    // The seed of an ambiguity is formed before the ambiguity exists.
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});   // births the states; then ask for a signal that has no ambiguity
    PppObservation other = o;
    other.modeled["L2W"] = other.modeled.at("L1W");
    other.observed["L2W"] = other.observed.at("L1W") + 4.0;
    ASSERT_LT(states().getAmbiguityIndex(K::makeAmbiguityKey(kSat, "L2W")), 0);
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_TRUE(sameBits(model().ambiguitySeed(other, "L2W", x),
                         legacy::computeAmbiguitySeed(world(), other, "L2W", x)));
}

TEST_F(SeedsTest, ASignalWithoutObservedOrModeledValueSeedsZero) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_EQ(model().ambiguitySeed(o, "L2W", x), 0.0);
    PppObservation no_model = o;
    no_model.modeled.erase("L1W");
    EXPECT_EQ(model().ambiguitySeed(no_model, "L1W", x), 0.0);
}

TEST_F(SeedsTest, TheTermsAreSubtractedWithTheInterSystemBiasBeforeTheGradients) {
    // (1 - 0.3) - 0.1 and (1 - 0.1) - 0.3 differ in the last bit; the seeds have always
    // subtracted the inter-system bias first.
    ASSERT_FALSE(sameBits((1.0 - 0.3) - 0.1, (1.0 - 0.1) - 0.3));
    ObservationEquation eq;
    ObsAddend& modeled = eq.addend[eq.n++];
    modeled.effect = ObsEffect::Modeled;
    modeled.constant = 0.0;
    ObsAddend& gradient = eq.addend[eq.n++];   // the equation's own order puts the gradient first
    gradient.effect = ObsEffect::TropoGradient;
    gradient.part[0].partial = 1.0;
    gradient.part[0].x = 0.1;
    gradient.n_part = 1;
    ObsAddend& isb = eq.addend[eq.n++];
    isb.effect = ObsEffect::SystemTimeOffset;
    isb.part[0].partial = 1.0;
    isb.part[0].x = 0.3;
    isb.n_part = 1;
    eq.usable = true;
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.residual, 1.0), (1.0 - 0.3) - 0.1));
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.ambiguity, 1.0), (1.0 - 0.3) - 0.1));
}

TEST_F(SeedsTest, ThePhaseClockIsAddedToTheReceiverClockFirstInTheAmbiguitySeedOnly) {
    // observed - modeled - (clock + phase clock) against observed - modeled - clock.
    const double clock = 0.3, phase_clock = 0.2, observed = 1.0;
    ASSERT_FALSE(sameBits((observed - clock) - phase_clock, observed - (clock + phase_clock)));
    ObservationEquation eq;
    ObsAddend& modeled = eq.addend[eq.n++];
    modeled.effect = ObsEffect::Modeled;
    ObsAddend& rx = eq.addend[eq.n++];
    rx.effect = ObsEffect::ReceiverClock;
    rx.part[0].partial = 1.0;
    rx.part[0].x = clock;
    rx.n_part = 1;
    ObsAddend& pc = eq.addend[eq.n++];
    pc.effect = ObsEffect::PhaseClock;
    pc.part[0].partial = 1.0;
    pc.part[0].x = phase_clock;
    pc.n_part = 1;
    eq.usable = true;
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.ambiguity, observed),
                         observed - (clock + phase_clock)));
    // The residual seeds do not know the phase clock at all.
    EXPECT_TRUE(sameBits(seedResidual(eq, kLegacySeedMask.residual, observed), observed - clock));
}

TEST_F(SeedsTest, EverySubtractedEffectIsInTheSeedOrder) {
    for (const SeedMask& mask : {kLegacySeedMask.residual, kLegacySeedMask.ambiguity})
        for (std::size_t e = 0; e < kObsEffectCount; ++e) {
            const ObsEffect effect = static_cast<ObsEffect>(e);
            if (!mask.effects.has(effect) || effect == ObsEffect::PhaseClock) continue;
            EXPECT_NE(std::find(kSeedOrder.begin(), kSeedOrder.end(), effect), kSeedOrder.end())
                << obsEffectName(effect);
        }
}

// ===========================================================================
// The effect sets of the equation
// ===========================================================================

TEST_F(SeedsTest, ASubsetOfEffectsBuildsOnlyThoseTermsAndNeedsNoAmbiguity) {
    load("ppp-rtk");
    const PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    // No epoch has run: the model has no ambiguity state.
    const ObsInput in = model().ambiguitySeedInput();
    const Eigen::VectorXd x = states().getStateVector();
    const UserParamView view(states(), x, states().getStateSize());
    EXPECT_FALSE(observationEquation(in, o, "L1W", ParamLookup(view)).usable);
    const ObservationEquation eq = observationEquation(
        in, o, "L1W", ParamLookup(view), effectsOf({ObsEffect::Modeled, ObsEffect::Troposphere}));
    ASSERT_TRUE(eq.usable);
    ASSERT_EQ(eq.n, 2);
    EXPECT_EQ(eq.addend[0].effect, ObsEffect::Modeled);
    EXPECT_EQ(eq.addend[1].effect, ObsEffect::Troposphere);
    // All effects is what a row builds: the default.
    EXPECT_EQ(kAllEffects.bits, (1u << kObsEffectCount) - 1u);
    EXPECT_TRUE(kAllEffects.has(ObsEffect::PhaseColour));
    EXPECT_FALSE(kAllEffects.without(ObsEffect::SsrBias).has(ObsEffect::SsrBias));
}

TEST_F(SeedsTest, ASatelliteTextThatIsNotCanonicalMakesNoSeedWhenATermNeedsTheName) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1W", "L1W"});
    update({o});
    o.satellite = "G12 ";
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_EQ(model().ambiguitySeed(o, "L1W", x), 0.0);
    const View v(states());
    EXPECT_FALSE(rawResidual(model().residualSeedInput(), v.lookup(), o, "C1W").has_value());
}

// ===========================================================================
// The inter-system bias
// ===========================================================================

TEST_F(SeedsTest, InterSystemBiasSeedsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("G20", System::GPS, {"C1W", "L1W"}, 300.0, 40.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C", "L1C"}, 310.0, 21.0),
    };
    run(sats);
    ASSERT_NE(states().getSystemTimeOffsetIndex("Galileo"), std::numeric_limits<size_t>::max());

    // Code that differs by constellation, so that the medians are not all zero.
    std::vector<PppObservation> epoch_obs = sats;
    for (PppObservation& o : epoch_obs)
        for (auto& [signal, value] : o.observed)
            if (signal[0] == 'C') value += (o.system == System::Galileo ? 31.25 : 4.5)
                                           + 0.37 * static_cast<double>(o.satellite.back());

    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    ASSERT_TRUE(legacy::initializeInterSystemBiases(world(), epoch_obs, old_values, old_reference));

    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), epoch_obs);
    ASSERT_TRUE(seeds.settled);
    EXPECT_EQ(seeds.reference_obs, old_reference);
    ASSERT_EQ(seeds.seeds.size(), old_values.size());
    ASSERT_FALSE(old_values.empty());
    for (std::size_t i = 0; i < old_values.size(); ++i) {
        EXPECT_EQ(std::string(systemName(seeds.seeds[i].sys)), old_values[i].name);
        EXPECT_TRUE(sameBits(seeds.seeds[i].value_m, old_values[i].value)) << old_values[i].name;
        EXPECT_EQ(seeds.seeds[i].n_obs, old_values[i].n);
    }
}

TEST_F(SeedsTest, WithoutTheTimeReferenceTheInterSystemBiasIsNotSettled) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0)};
    update(sats);
    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    EXPECT_FALSE(legacy::initializeInterSystemBiases(world(), sats, old_values, old_reference));
    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_FALSE(seeds.settled);
    EXPECT_TRUE(seeds.seeds.empty());
}

TEST_F(SeedsTest, ASatelliteBelowTheElevationMaskIsLeftOut) {
    load("ppp-rtk");
    const double mask = config().getElevationMaskDeg();
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W"}, 45.0, mask + 20.0),
        observation("G15", System::GPS, {"C1W", "L1W"}, 90.0, mask - 1.0),
        observation("E05", System::Galileo, {"C1C", "L1C"}, 120.0, mask + 10.0),
    };
    update(sats);
    std::vector<legacy::IsbValue> old_values;
    size_t old_reference = 0;
    ASSERT_TRUE(legacy::initializeInterSystemBiases(world(), sats, old_values, old_reference));
    const View v(states());
    const IsbSeeds seeds = seedIsb(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_EQ(seeds.reference_obs, old_reference);
    EXPECT_EQ(seeds.reference_obs, 1u);
}

// ===========================================================================
// The cold receiver clock
// ===========================================================================

TEST_F(SeedsTest, ColdClockSeedsMatchTheOldCode) {
    load("ppp-rtk");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C"}, 120.0, 33.0),
    };
    run(sats);
    std::vector<PppObservation> epoch_obs = sats;
    for (PppObservation& o : epoch_obs)
        for (auto& [signal, value] : o.observed)
            if (signal[0] == 'C') value += 123456.789 + 0.5 * static_cast<double>(o.satellite.back());

    // A cold clock is seeded with the median GPS code residual.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 0.0;
        states().setStateVector(x);
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), epoch_obs);
        ASSERT_TRUE(old.seeded);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), epoch_obs);
        EXPECT_EQ(now.outcome, ColdClockOutcome::Seeded);
        EXPECT_TRUE(now.settled());
        EXPECT_TRUE(sameBits(now.value_m, old.value));
        EXPECT_EQ(now.n_obs, old.n);
    }
    // A clock that is not cold is left alone, and the seed is settled.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 5.0;
        states().setStateVector(x);
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), epoch_obs);
        EXPECT_TRUE(old.settled);
        EXPECT_FALSE(old.seeded);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), epoch_obs);
        EXPECT_EQ(now.outcome, ColdClockOutcome::AlreadySeeded);
        EXPECT_EQ(now.current_m, 5.0);
        EXPECT_TRUE(now.settled());
    }
    // Cold, but no time-reference code this epoch: retry.
    {
        Eigen::VectorXd x = states().getStateVector();
        x(static_cast<Eigen::Index>(states().getClockIndex())) = 0.0;
        states().setStateVector(x);
        const std::vector<PppObservation> galileo_only = {epoch_obs.back()};
        const legacy::ColdClock old = legacy::seedColdReceiverClock(world(), galileo_only);
        EXPECT_FALSE(old.settled);
        const View v(states());
        const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), galileo_only);
        EXPECT_EQ(now.outcome, ColdClockOutcome::NoReference);
        EXPECT_FALSE(now.settled());
    }
}

TEST_F(SeedsTest, UnderBetweenSatelliteDifferencingThereIsNoClockToSeed) {
    load("ppp-rtk", "", "", /*bssd=*/true);
    const std::vector<PppObservation> sats = {observation(kSat, System::GPS, {"C1W", "L1W"})};
    update(sats);
    ASSERT_FALSE(states().hasClock());
    const View v(states());
    const ColdClockSeed now = seedColdClock(model().residualSeedInput(), v.lookup(), sats);
    EXPECT_EQ(now.outcome, ColdClockOutcome::NoClockState);
    EXPECT_TRUE(now.settled());
    EXPECT_TRUE(legacy::seedColdReceiverClock(world(), sats).settled);
}

// ===========================================================================
// The phase clock
// ===========================================================================

TEST_F(SeedsTest, PhaseClockSeedMatchesTheOldCode) {
    load("ppp-rtk", "", R"(, "estimate_phase_clock": true)");
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"}),
        observation("G15", System::GPS, {"C1W", "L1W", "C2W", "L2W"}, 200.0, 60.0),
        observation("E05", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 120.0, 33.0),
        observation("E09", System::Galileo, {"C1C"}, 310.0, 21.0),   // code only: no pair
    };
    run(sats);
    ASSERT_TRUE(states().hasPhaseClock());

    double old_value = 0.0;
    size_t old_pairs = 0;
    ASSERT_TRUE(legacy::initializePhaseClockOffset(world(), sats, old_value, old_pairs));
    const View v(states());
    const PhaseClockSeed now = seedPhaseClock(model().residualSeedInput(), v.lookup(), sats);
    ASSERT_TRUE(now.found);
    EXPECT_TRUE(sameBits(now.value_m, old_value));
    EXPECT_EQ(now.n_pairs, old_pairs);
    EXPECT_EQ(now.n_pairs, 3u);

    // No satellite with both a code and a phase observation.
    const std::vector<PppObservation> code_only = {observation(kSat, System::GPS, {"C1W"})};
    double unused = 0.0;
    size_t unused_pairs = 0;
    EXPECT_FALSE(legacy::initializePhaseClockOffset(world(), code_only, unused, unused_pairs));
    EXPECT_FALSE(seedPhaseClock(model().residualSeedInput(), v.lookup(), code_only).found);
}

// ===========================================================================
// The receiver code biases
// ===========================================================================

TEST_F(SeedsTest, WithoutReceiverBiasEstimationThereAreNoCodeBiasSeeds) {
    load("ppp-rtk");   // estimate_ifb is off
    ASSERT_FALSE(config().estimateIfb());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})};
    update(sats);
    const View v(states());
    const CodeBiasSeeds seeds =
        seedCodeBias(model().residualSeedInput(), v.lookup(), sats, {{"GPS_DCB", 1.5}});
    EXPECT_TRUE(seeds.seeds.empty());
    EXPECT_EQ(seeds.refused, 0u);
}

TEST_F(SeedsTest, WithoutAResolverThereAreNoCodeBiasSeeds) {
    load("ppp-rtk", "", R"(, "estimate_ifb": true)");
    ASSERT_TRUE(config().estimateIfb());
    const std::vector<PppObservation> sats = {
        observation(kSat, System::GPS, {"C1W", "L1W", "C2W", "L2W"})};
    update(sats);
    const View v(states());
    ObsInput in = model().residualSeedInput();
    in.receiver_bias = nullptr;
    EXPECT_TRUE(seedCodeBias(in, v.lookup(), sats, {}).seeds.empty());
}


// ===========================================================================
// The receiver code biases, against a .BIA clock reference
// ===========================================================================

/// The posture of the code-bias cases: uncombined, receiver biases estimated, and a .BIA that
/// references GPS to C1W and C2W and Galileo to C1C and C5Q (so band 5 of GPS and band 7 of
/// Galileo are the independent IFB parameters).
class CodeBiasSeedsTest : public SeedsTest {
protected:
    void fillClockRefs() {
        clock_refs['G'] = ClockRefSignals{"C1W", "C2W"};
        clock_refs['E'] = ClockRefSignals{"C1C", "C5Q"};
    }

    void estimateBiases() {
        load("ppp-rtk", "", R"(, "estimate_ifb": true)");
        ASSERT_TRUE(config().estimateIfb());
        fillClockRefs();
    }

    /// The state key text of a receiver code-bias parameter.
    static std::string keyText(System sys, ParamForm form, char band_digit = '0') {
        RxCbKey key;
        key.sys  = sys;
        key.form = form;
        if (form == ParamForm::Ifb) key.band = bandOfDigit(sys, band_digit);
        return userCodeBiasKey(key);
    }

    /// Three GPS and three Galileo satellites on three bands each: the states are born from them.
    std::vector<PppObservation> biasSatellites() const {
        const std::vector<std::string> gps = {"C1W", "L1W", "C2W", "L2W", "C5X", "L5X"};
        const std::vector<std::string> gal = {"C1C", "L1C", "C5Q", "L5Q", "C7Q", "L7Q"};
        return {
            observation("G12", System::GPS, gps),
            observation("G15", System::GPS, gps, 200.0, 60.0),
            observation("G20", System::GPS, gps, 300.0, 40.0),
            observation("E05", System::Galileo, gal, 120.0, 33.0),
            observation("E09", System::Galileo, gal, 310.0, 21.0),
            observation("E11", System::Galileo, gal, 75.0, 50.0),
        };
    }

    /// Code that differs by satellite and band, so that no median is zero.
    static void spreadCodes(std::vector<PppObservation>& sats) {
        double k = 0.0;
        for (PppObservation& o : sats) {
            k += 1.0;
            for (auto& [signal, value] : o.observed)
                if (signal[0] == 'C') {
                    const double band = static_cast<double>(signal[1] - '0');
                    value += 0.8 * k + 1.3 * band + 0.05 * k * band;
                }
        }
    }

    /// The epoch the seeds are formed from: the born satellites and five that have no states,
    /// each left out of a median for its own reason.
    /// @param g24_offset_deg Elevation of G24 relative to the elevation mask (negative: below).
    std::vector<PppObservation> seedEpoch(double g24_offset_deg = -1.0) {
        const double mask = config().getElevationMaskDeg();
        std::vector<PppObservation> sats = biasSatellites();
        const std::vector<std::string> gps = {"C1W", "C2W", "C5X"};
        sats.push_back(observation("G24", System::GPS, gps, 80.0, mask + g24_offset_deg));
        sats.push_back(observation("G28", System::GPS, gps, 20.0, 35.0));
        sats.push_back(observation("G30", System::GPS, {"C2W", "C5X"}, 250.0, 45.0));   // no first reference band
        sats.push_back(observation("G32", System::GPS, {"C1W"}, 150.0, 55.0));          // one band
        sats.push_back(observation("C06", System::BeiDou, {"C2I", "C7I", "C6I"}, 10.0, 40.0));   // no clock reference
        spreadCodes(sats);
        for (PppObservation& o : sats)
            if (o.satellite == "G28") o.observed["C2W"] += 500.0;   // its DCB estimate is beyond 200 m
        return sats;
    }

    /// The new seeds over the states as they are.
    CodeBiasSeeds seeds(const std::vector<PppObservation>& obs,
                        const std::map<std::string, double>& installed = {}) {
        const View v(states());
        return seedCodeBias(model().residualSeedInput(), v.lookup(), obs, installed);
    }

    /// The seed of one parameter, or null.
    static const CodeBiasSeed* find(const CodeBiasSeeds& s, const std::string& text) {
        for (const CodeBiasSeed& seed : s.seeds)
            if (userCodeBiasKey(seed.key) == text) return &seed;
        return nullptr;
    }

    /// The new seeds against what the old function wrote and counted, bit for bit.
    static void expectSameAsOldCode(const CodeBiasSeeds& now, const legacy::CodeBiasValues& old) {
        EXPECT_EQ(now.seeds.size(), old.value.size());
        std::vector<std::string> texts;
        long long restored = 0;
        for (const CodeBiasSeed& seed : now.seeds) {
            const std::string text = userCodeBiasKey(seed.key);
            texts.push_back(text);
            const auto it = old.value.find(text);
            ASSERT_NE(it, old.value.end()) << text;
            EXPECT_TRUE(sameBits(seed.value_m, it->second)) << text;
            EXPECT_EQ(seed.restored, old.restored.count(text) == 1) << text;
            if (seed.restored) {
                ++restored;
                EXPECT_EQ(seed.n_sats, 0u) << text;
            } else {
                EXPECT_EQ(seed.n_sats, old.n_sats.at(text)) << text;
            }
        }
        EXPECT_TRUE(std::is_sorted(texts.begin(), texts.end())) << "applied in the order of the keys";
        EXPECT_EQ(restored, old.applied);
        EXPECT_EQ(static_cast<long long>(now.refused), old.uncovered);
    }
};

TEST_F(CodeBiasSeedsTest, TheMedianSeedsMatchTheOldCodeAndLeaveOutTheRightSatellites) {
    estimateBiases();
    run(biasSatellites());
    ASSERT_EQ(states().getCodeBiasStates().size(), 4u);   // GPS DCB, GPS IFB5, Galileo DCB, Galileo IFB7

    const std::vector<PppObservation> epoch_obs = seedEpoch();
    const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, {});
    ASSERT_EQ(old.value.size(), 4u);
    const CodeBiasSeeds now = seeds(epoch_obs);
    expectSameAsOldCode(now, old);
    EXPECT_EQ(now.refused, 0u);

    // GPS DCB: G12, G15, G20.  Left out: G24 (below the elevation mask), G28 (its estimate is
    // beyond 200 m), G30 (no first reference band), G32 (one band).  IFB5 also takes G28, whose
    // band 5 is sound.  C06 has no clock reference and enters nothing.
    const CodeBiasSeed* dcb  = find(now, keyText(System::GPS, ParamForm::Dcb));
    const CodeBiasSeed* ifb5 = find(now, keyText(System::GPS, ParamForm::Ifb, '5'));
    ASSERT_NE(dcb, nullptr);
    ASSERT_NE(ifb5, nullptr);
    EXPECT_EQ(dcb->n_sats, 3u);
    EXPECT_EQ(ifb5->n_sats, 4u);
    EXPECT_NE(find(now, keyText(System::Galileo, ParamForm::Dcb)), nullptr);
    EXPECT_NE(find(now, keyText(System::Galileo, ParamForm::Ifb, '7')), nullptr);
}

TEST_F(CodeBiasSeedsTest, TheElevationMaskDecidesWhichSatellitesEnterTheMedian) {
    estimateBiases();
    run(biasSatellites());
    const std::string dcb = keyText(System::GPS, ParamForm::Dcb);

    const std::vector<PppObservation> below = seedEpoch(-1.0);
    const std::vector<PppObservation> above = seedEpoch(+1.0);
    const CodeBiasSeeds now_below = seeds(below);
    const CodeBiasSeeds now_above = seeds(above);
    expectSameAsOldCode(now_below, legacy::initializeCodeBiases(world(), below, {}));
    expectSameAsOldCode(now_above, legacy::initializeCodeBiases(world(), above, {}));

    const CodeBiasSeed* b = find(now_below, dcb);
    const CodeBiasSeed* a = find(now_above, dcb);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->n_sats, b->n_sats + 1);          // G24 joins once it is above the mask
    EXPECT_FALSE(sameBits(a->value_m, b->value_m));
}

TEST_F(CodeBiasSeedsTest, ARestoredKeyTakesTheInstalledValueAndFormsNoMedian) {
    estimateBiases();
    run(biasSatellites());
    const std::string dcb = keyText(System::GPS, ParamForm::Dcb);
    // The second entry names a parameter no satellite samples: it is neither used nor counted.
    const std::map<std::string, double> installed = {{dcb, 1.5}, {"GLONASS_DCB", 9.0}};

    const std::vector<PppObservation> epoch_obs = seedEpoch();
    const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, installed);
    ASSERT_EQ(old.applied, 1);
    ASSERT_EQ(old.uncovered, 0);
    const CodeBiasSeeds now = seeds(epoch_obs, installed);
    expectSameAsOldCode(now, old);

    const CodeBiasSeed* restored = find(now, dcb);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->restored);
    EXPECT_EQ(restored->value_m, 1.5);
    EXPECT_EQ(now.refused, 0u);
    // Every other parameter keeps its median, equal to the one formed with nothing installed.
    const CodeBiasSeeds plain = seeds(epoch_obs);
    for (const CodeBiasSeed& seed : now.seeds) {
        if (seed.restored) continue;
        const CodeBiasSeed* same = find(plain, userCodeBiasKey(seed.key));
        ASSERT_NE(same, nullptr);
        EXPECT_TRUE(sameBits(seed.value_m, same->value_m));
    }
}

TEST_F(CodeBiasSeedsTest, ACoveredKeyWithAnUnusableValueIsRefusedAndTakesTheMedian) {
    estimateBiases();
    run(biasSatellites());
    const std::string dcb = keyText(System::GPS, ParamForm::Dcb);
    const std::map<std::string, double> installed = {{dcb, std::numeric_limits<double>::quiet_NaN()}};

    const std::vector<PppObservation> epoch_obs = seedEpoch();
    const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, installed);
    ASSERT_EQ(old.applied, 0);
    ASSERT_EQ(old.uncovered, 1);
    const CodeBiasSeeds now = seeds(epoch_obs, installed);
    expectSameAsOldCode(now, old);
    EXPECT_EQ(now.refused, 1u);

    const CodeBiasSeeds plain = seeds(epoch_obs);
    const CodeBiasSeed* refused = find(now, dcb);
    const CodeBiasSeed* median  = find(plain, dcb);
    ASSERT_NE(refused, nullptr);
    ASSERT_NE(median, nullptr);
    EXPECT_FALSE(refused->restored);
    EXPECT_TRUE(sameBits(refused->value_m, median->value_m));
}

TEST_F(CodeBiasSeedsTest, ACoveredKeyWhoseParameterDoesNotExistIsRefusedAndNotSeeded) {
    estimateBiases();
    run(biasSatellites());
    const std::string ifb5 = keyText(System::GPS, ParamForm::Ifb, '5');
    std::set<std::string> keep;
    for (const auto& [text, index] : states().getCodeBiasStates())
        if (text != ifb5) keep.insert(text);
    states().pruneCodeBiasStatesExcept(keep);
    ASSERT_LT(states().getCodeBiasIndex(ifb5), 0);

    const std::vector<PppObservation> epoch_obs = seedEpoch();
    // Installed: counted, and the parameter still gets nothing.  Not installed: nothing either.
    for (const bool install : {true, false}) {
        const std::map<std::string, double> installed =
            install ? std::map<std::string, double>{{ifb5, 4.0}} : std::map<std::string, double>{};
        const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, installed);
        const CodeBiasSeeds now = seeds(epoch_obs, installed);
        expectSameAsOldCode(now, old);
        EXPECT_EQ(find(now, ifb5), nullptr);
        EXPECT_EQ(now.refused, install ? 1u : 0u);
        EXPECT_EQ(now.seeds.size(), 3u);
    }
}

TEST_F(CodeBiasSeedsTest, WithoutAClockReferenceAConstellationHasNoSeeds) {
    estimateBiases();
    run(biasSatellites());
    const std::vector<PppObservation> epoch_obs = seedEpoch();

    clock_refs.erase('E');
    {
        const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, {});
        const CodeBiasSeeds now = seeds(epoch_obs);
        expectSameAsOldCode(now, old);
        EXPECT_EQ(now.seeds.size(), 2u);   // GPS only
        EXPECT_EQ(find(now, keyText(System::Galileo, ParamForm::Dcb)), nullptr);
    }
    clock_refs.clear();
    {
        const legacy::CodeBiasValues old = legacy::initializeCodeBiases(world(), epoch_obs, {});
        const CodeBiasSeeds now = seeds(epoch_obs);
        expectSameAsOldCode(now, old);
        EXPECT_TRUE(now.seeds.empty());
    }
}

TEST_F(CodeBiasSeedsTest, TheModelWritesTheSeedsAndCountsTheRestoreOnTheInitializersSlots) {
    fillClockRefs();
    const std::string dcb  = keyText(System::GPS, ParamForm::Dcb);
    const std::string ifb5 = keyText(System::GPS, ParamForm::Ifb, '5');
    // One key restored, one covered with an unusable value, one that nothing samples.
    const std::map<std::string, double> installed = {
        {dcb, 1.5}, {ifb5, std::numeric_limits<double>::quiet_NaN()}, {"GLONASS_DCB", 9.0}};

    struct FirstEpoch {
        std::map<std::string, double> code_bias;   ///< The code-bias states after the first epoch, by key.
        long long applied{0};
        long long uncovered{0};
    };
    // The first epoch of a model: the states are born, then seeded behind the one-shot latch.
    const auto first_epoch = [&](const std::string& tag, const std::map<std::string, double>& restore) {
        FirstEpoch out;
        gnss_test::ModelStack s;
        EXPECT_TRUE(s.load(log, tag, "ppp-rtk", "", R"(, "estimate_ifb": true)", false));
        s.model->setClockRefs(&clock_refs);
        if (!restore.empty())
            s.model->getStateInitializer().setCodeBiasSeedOverride(restore, &out.applied,
                                                                   &out.uncovered);
        std::vector<PppObservation> sats = biasSatellites();
        spreadCodes(sats);
        s.model->computeMeasurementUpdate(sats, s.states->getStateVector());
        const Eigen::VectorXd x = s.states->getStateVector();
        for (const auto& [key, index] : s.states->getCodeBiasStates())
            out.code_bias[key] = x(static_cast<Eigen::Index>(index));
        return out;
    };

    const FirstEpoch plain    = first_epoch("seeds_model_plain", {});
    const FirstEpoch restored = first_epoch("seeds_model_restored", installed);
    ASSERT_EQ(plain.code_bias.size(), 4u);
    ASSERT_EQ(restored.code_bias.size(), 4u);
    EXPECT_EQ(plain.applied, 0);
    EXPECT_EQ(plain.uncovered, 0);

    // The restored key holds the installed value; the refused key and the two uncovered ones
    // hold the median the plain run formed; the counters read one applied and one refused.
    EXPECT_EQ(restored.applied, 1);
    EXPECT_EQ(restored.uncovered, 1);
    EXPECT_EQ(restored.code_bias.at(dcb), 1.5);
    for (const auto& [key, value] : restored.code_bias)
        if (key != dcb) EXPECT_TRUE(sameBits(value, plain.code_bias.at(key))) << key;
    EXPECT_NE(plain.code_bias.at(dcb), 1.5);
}

// ===========================================================================
// The slant ionosphere seed
// ===========================================================================

/// A .BIA presence predicate that holds the listed signals, for every satellite.
BiasPresence presenceOf(std::set<std::string> signals) {
    return [signals = std::move(signals)](const std::string&, const std::string& signal) {
        return signals.count(signal) != 0;
    };
}

void expectSameIonosphereSeed(const std::optional<IonosphereSeed>& now,
                              const std::optional<legacy::IonosphereValue>& old) {
    ASSERT_EQ(now.has_value(), old.has_value());
    if (!now) return;
    EXPECT_TRUE(sameBits(now->value_m, old->value));
    EXPECT_EQ(now->code_a, old->code_a);
    EXPECT_EQ(now->code_b, old->code_b);
    EXPECT_TRUE(sameBits(now->range_a_m, old->range_a));
    EXPECT_TRUE(sameBits(now->range_b_m, old->range_b));
    EXPECT_TRUE(sameBits(now->alpha_a, old->alpha_a));
    EXPECT_TRUE(sameBits(now->alpha_b, old->alpha_b));
}

class IonosphereSeedTest : public SeedsTest {
protected:
    /// A GPS satellite on three bands whose code differs from the model by the given offsets (m).
    PppObservation satellite(double c1w, double c2w, double c5x) const {
        PppObservation o = observation(kSat, System::GPS, {"C1W", "C2W", "C5X"});
        o.observed["C1W"] += c1w;
        o.observed["C2W"] += c2w;
        o.observed["C5X"] += c5x;
        return o;
    }

    /// The new seed against the old code with the same presence; returns the new one.
    std::optional<IonosphereSeed> bothFor(const PppObservation& o, const BiasPresence& has_entry) {
        const auto now = seedIonosphere(model().residualSeedInput(), o, has_entry);
        expectSameIonosphereSeed(now, legacy::computeIonosphereSeed(world(), o, has_entry));
        return now;
    }
};

TEST_F(IonosphereSeedTest, TheFirstCoveredPairOfDistinctBandsGivesTheSeed) {
    load("ppp-rtk");
    const PppObservation o = satellite(0.0, 4.0, 6.5);

    const auto all = bothFor(o, presenceOf({"C1W", "C2W", "C5X"}));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->code_a, "C1W");
    EXPECT_EQ(all->code_b, "C2W");
    EXPECT_EQ(all->range_a_m, 0.0);
    EXPECT_EQ(all->range_b_m, 4.0);
    EXPECT_GT(all->value_m, 0.0);     // L2 is delayed more than L1, so (0 - 4) / (1 - alpha2) > 0

    // The bias product decides which codes are used: without C2W the pair is C1W and C5X.
    const auto no_l2 = bothFor(o, presenceOf({"C1W", "C5X"}));
    ASSERT_TRUE(no_l2.has_value());
    EXPECT_EQ(no_l2->code_a, "C1W");
    EXPECT_EQ(no_l2->code_b, "C5X");
    EXPECT_NE(no_l2->value_m, all->value_m);

    // One covered code, or none, gives no seed (the caller then seeds 0 with an inflated variance).
    EXPECT_FALSE(bothFor(o, presenceOf({"C1W"})).has_value());
    EXPECT_FALSE(bothFor(o, presenceOf({})).has_value());
}

TEST_F(IonosphereSeedTest, TwoCodesOfOneBandMakeNoPair) {
    load("ppp-rtk");
    PppObservation o = observation(kSat, System::GPS, {"C1C", "C1W", "C2W"});
    o.observed["C1W"] += 0.5;
    o.observed["C2W"] += 4.0;

    // C1C and C1W share band 1: the pair is C1C with C2W.
    const auto pair = bothFor(o, presenceOf({"C1C", "C1W", "C2W"}));
    ASSERT_TRUE(pair.has_value());
    EXPECT_EQ(pair->code_a, "C1C");
    EXPECT_EQ(pair->code_b, "C2W");
    // Only the two band-1 codes: no pair at all.
    EXPECT_FALSE(bothFor(o, presenceOf({"C1C", "C1W"})).has_value());
}

TEST_F(IonosphereSeedTest, APairBeyond200MetresIsSkippedAndNonFiniteOnesToo) {
    load("ppp-rtk");
    const BiasPresence all = presenceOf({"C1W", "C2W", "C5X"});

    // (C1W, C2W) would give about 460 m: skipped, so (C1W, C5X) is returned.
    const auto next = bothFor(satellite(0.0, 300.0, 0.0), all);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->code_a, "C1W");
    EXPECT_EQ(next->code_b, "C5X");
    EXPECT_LT(std::abs(next->value_m), 200.0);

    // Every pair beyond the limit: no seed.
    EXPECT_FALSE(bothFor(satellite(0.0, 300.0, -300.0), all).has_value());
    // A non-finite observation: its pairs give no seed.
    EXPECT_FALSE(bothFor(satellite(0.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity()), all).has_value());
}

TEST_F(IonosphereSeedTest, AnEntryThatIsLoadedButNotAppliedCountsOnlyWhereTheRuleIsOff) {
    // bias_coverage_requires_apply on and the entries not applied: no entry counts, so the seed
    // is absent although every code has an entry.  Off, or applied, the entries count.
    struct Posture { bool requires_apply; bool applied; };
    int index = 0;
    for (const Posture p : {Posture{false, false}, Posture{false, true}, Posture{true, true},
                            Posture{true, false}}) {
        gnss_test::ModelStack s;
        const std::string top = std::string(R"(, "model_corrections": {"bias_coverage_requires_apply": )")
            + (p.requires_apply ? "true" : "false") + R"(, "enable_code_phase_bias": )"
            + (p.applied ? "true" : "false") + "}";
        ASSERT_TRUE(s.load(log, "seeds_apply_" + std::to_string(index++), "ppp-rtk", "", "", false, top));
        s.model->setClockRefs(&clock_refs);
        const PppObservation o = satellite(0.0, 4.0, 6.5);
        const BiasPresence all = presenceOf({"C1W", "C2W", "C5X"});
        const legacy::World w{*s.states, s.config, s.model->getStateInitializer(), clock_refs};

        const auto now = seedIonosphere(s.model->residualSeedInput(), o, all);
        expectSameIonosphereSeed(now, legacy::computeIonosphereSeed(w, o, all));
        const bool counts = !p.requires_apply || p.applied;
        EXPECT_EQ(now.has_value(), counts) << "requires_apply " << p.requires_apply
                                           << " applied " << p.applied;
        EXPECT_EQ(biaEntryApplies(s.config), counts);
    }
}

}  // namespace
