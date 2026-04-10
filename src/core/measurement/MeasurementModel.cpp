/**
/**
 * @file MeasurementModel.cpp
 * @brief Implements measurement vector, design matrix and covariance assembly for PPP Kalman updates.
 *
 * The implementation converts a set of PPP observations and the current Kalman state
 * into an EpochMeasurement suitable for Kalman filter processing.
 *
 * The code is organized into focused helpers:
 *  - systemToIsbName()        — System enum ? ISB name (eliminates duplicate switch blocks).
 *  - computeRawResidual()     — observed ? modeled ? clk ? tropo ? ISB ? grad (shared formula).
 *  - ensureRequiredStates()   — ambiguity / iono / code-bias creation and pruning pre-pass.
 *  - isUsableMeasurement()    — per-measurement filtering predicate.
 *  - buildJacobianRow()       — single-row Jacobian and predicted-observable assembly.
 *  - computeMeasurementUpdate — top-level orchestrator (kept short).
 */

#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "MeasurementModel.hpp"
#include "ProductCache.hpp"
#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {

    // ===================================================================
    // Helpers (file-scope)
    // ===================================================================

    namespace {

    /// Human-readable mode label used in log messages.
    const char* modeLabel(const GnssConfig& cfg) {
        return cfg.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK";
    }

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

    // ===================================================================
    // Construction
    // ===================================================================

    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), ppp_utils_(log_manager), log_manager_(log_manager) {
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(), modeLabel(config_));
    }

    // ===================================================================
    // Utility helpers
    // ===================================================================

    void MeasurementModel::setImuState(const Eigen::Matrix3d& C_b_e,
                                        const Eigen::Vector3d& f_b_corrected) {
        tc_C_b_e_ = C_b_e;
        tc_f_b_ = f_b_corrected;
    }

    std::string MeasurementModel::systemToIsbName(System system) {
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

    char MeasurementModel::systemToClockRefChar(System system) {
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

    bool MeasurementModel::ifCombHasFullBia(const std::string& satellite,
                                             const std::string& if_key) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true; // not an IF key — assume covered
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasBiasEntry(satellite, sig1)
            && ppp_model_.hasBiasEntry(satellite, sig2);
    }

    bool MeasurementModel::ifCombHasFullRxAtx(const std::string& if_key,
                                               System system) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true;
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasReceiverAtxEntry(sig1, system)
            && ppp_model_.hasReceiverAtxEntry(sig2, system);
    }





    MeasurementModel::ReceiverBiasEntry
    MeasurementModel::resolveReceiverBias(const std::string& obs_signal,
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

        // Band digit of the observation and the two reference signals
        const char band_j = (obs_signal.size() >= 2) ? obs_signal[1] : '\0';
        const char band_1 = (ref.sig1.size() >= 2) ? ref.sig1[1] : '\0';
        const char band_2 = (ref.sig2.size() >= 2) ? ref.sig2[1] : '\0';
        if (band_j == '\0' || band_1 == '\0' || band_2 == '\0') return result;

        // --- Non-reference band: independent IFB state, coefficient = 1.0 ---
        if (band_j != band_1 && band_j != band_2) {
            result.state_key   = sys + "_IFB" + band_j;   // e.g. "G_IFB5", "E_IFB7"
            result.coefficient = 1.0;
            return result;
        }

        // --- Reference band: GF-IF coefficient against the system DCB state ---
        const double f1 = ppp_utils_.getFrequency(ref.sig1, system, satellite);
        const double f2 = ppp_utils_.getFrequency(ref.sig2, system, satellite);
        if (!(std::isfinite(f1) && f1 > 0.0 && std::isfinite(f2) && f2 > 0.0))
            return result;

        // mu_j = f1^2 / f_j^2   (using f1 as f_ref; cancels in the ratio)
        const double mu1 = 1.0;                       // f1^2 / f1^2
        const double mu2 = (f1 * f1) / (f2 * f2);
        const double dmu = mu1 - mu2;
        if (std::abs(dmu) < 1e-12) return result;

        result.state_key = sys + "_DCB";
        result.coefficient = (band_j == band_1) ? (mu1 / dmu) : (mu2 / dmu);
        return result;
    }

    double MeasurementModel::computeRawResidual(const PppObservation& obs,
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

        // Per-satellite signal bias (uncorrected satellite HW delay)
        double sat_bias = 0.0;
        if (config_.estimateSatBiases() && !ref_type.empty()) {
            // Uncombined mode: raw signals without BIA coverage.
            // IF mode: code IF combinations with incomplete BIA coverage.
            const bool is_if = (ref_type.find('_') != std::string::npos);
            const bool applicable = config_.isUncombinedEnabled()
                ? (!is_if && (ref_type[0] == 'C' || ref_type[0] == 'L'))
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

    double MeasurementModel::computeIonosphereSeed(const PppObservation& obs) const {
        // Find two code pseudoranges on different frequency bands.
        // The ionosphere state is at the GPS-L1 reference frequency, so the
        // seed is:  I = (P1 - P2) / (1 - (f1/f2)^2)
        // which gives the slant iono delay at f1.  When f1 == f_ref this is
        // directly the state value.  Otherwise we rescale by alpha1.
        //
        // We search observed code keys for two distinct bands (digit after 'C').
        struct CodeObs { std::string key; double value; double freq; char band; };
        std::vector<CodeObs> codes;
        for (const auto& [key, val] : obs.observed) {
            if (key.size() < 2 || key[0] != 'C') continue;
            if (!obs.modeled.count(key)) continue;
            const double freq = ppp_utils_.getFrequency(key, obs.system, obs.satellite);
            if (freq <= 0.0) continue;
            // Use the raw code pseudorange (observed value in meters).
            // We only need the geometric-range-cancelled difference P1-P2,
            // so modeled corrections cancel.  But for accuracy we use the
            // model-subtracted residual: (obs - mod) contains the iono and
            // receiver clock.  Since the clock is common it cancels in the
            // difference.
            codes.push_back({key, val, freq, key[1]});
        }
        if (codes.size() < 2) return 0.0;

        // Pick two codes on different bands.
        for (size_t i = 0; i < codes.size(); ++i) {
            for (size_t j = i + 1; j < codes.size(); ++j) {
                if (codes[i].band == codes[j].band) continue;
                const double f1 = codes[i].freq;
                const double f2 = codes[j].freq;
                const double P1 = codes[i].value;
                const double P2 = codes[j].value;
                const double mod1 = obs.modeled.at(codes[i].key);
                const double mod2 = obs.modeled.at(codes[j].key);

                // Geometry-free combination of model-subtracted pseudoranges:
                // (P1 - mod1) - (P2 - mod2) ? clk + ?1·I - clk - ?2·I = (?1 - ?2)·I
                // ? I = ((P1 - mod1) - (P2 - mod2)) / (?1 - ?2)
                const double f_ref = Constants::GPS_L1_FREQ;
                const double alpha1 = (f_ref * f_ref) / (f1 * f1);
                const double alpha2 = (f_ref * f_ref) / (f2 * f2);
                const double denom = alpha1 - alpha2;
                if (std::abs(denom) < 1e-6) continue;

                const double iono_seed = ((P1 - mod1) - (P2 - mod2)) / denom;

                if (std::isfinite(iono_seed) && std::abs(iono_seed) < 200.0) {
                    log_manager_.getLogger("measurement_model")->debug(
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

    // ===================================================================
    // State pre-pass
    // ===================================================================

    void MeasurementModel::ensureRequiredStates(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        const bool is_uncombined = config_.isUncombinedEnabled();

        std::set<std::string> required_ambiguities;
        std::set<std::string> required_iono_sats;

        // --- First pass: collect required satellites and ambiguity keys ---
        for (const auto& obs : observations) {
            // Skip GLONASS entirely when BSSD is active (FDMA ICB issue).
            if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd())
                continue;

            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty()) continue;
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;

                if (is_uncombined && (ref_type[0] == 'C' || ref_type[0] == 'L'))
                    required_iono_sats.insert(obs.satellite);

                if (ref_type[0] != 'L') continue;

                const bool is_iono_free = (ref_type.find('_') != std::string::npos);
                if (!is_uncombined && !is_iono_free) continue;

                required_ambiguities.insert(obs.satellite + "_" + ref_type);
            }
        }

        // --- Ionosphere states (create and seed BEFORE ambiguities) ---
        // Seeding iono from dual-frequency code pseudoranges ensures
        // that the subsequent ambiguity seeds are not corrupted by
        // an all-zero ionosphere estimate.  This matches cssrlib which
        // computes ion[i] = (P1 - P2) / (1 - (f1/f2)^2) before seeding
        // ambiguities.
        if (is_uncombined) {
            for (const auto& obs : observations) {
                if (!required_iono_sats.count(obs.satellite)) continue;
                if (state_manager_.getIonosphereIndex(obs.satellite) < 0) {
                    if (obs.elevation > 0.0)
                        state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);

                    const double iono_seed = computeIonosphereSeed(obs);
                    state_manager_.addIonosphereState(obs.satellite, iono_seed);
                    log_manager_.getLogger("measurement_model")->debug(
                        "{}Added ionosphere state for satellite {} with seed={:.3f} m",
                        epoch_prefix, obs.satellite, iono_seed);
                }
            }
            state_manager_.pruneIonosphereStatesExcept(required_iono_sats);

            for (const auto& obs : observations) {
                if (required_iono_sats.count(obs.satellite) && obs.elevation > 0.0)
                    state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
            }
        }

        // --- Ambiguity states (seeded after iono so that the seed accounts
        //     for the current ionosphere estimate) ---
        //
        // When BSSD is active the ambiguity state N(i) represents the
        // single-difference N_sd(i) = N_abs(i) - N_abs(pivot).  New or
        // re-seeded ambiguities must therefore be seeded as SD values,
        // not raw absolute seeds.  For the pivot satellite the SD
        // ambiguity is always 0.
        const bool bssd_active = config_.enableBetweenSatelliteSd();

        // Pre-compute per-(constellation, signal) pivot absolute seeds
        // so each non-pivot seed can be differenced.
        std::map<std::string, double> pivot_abs_seeds; // key: "cons|sig"
        if (bssd_active) {
            const Eigen::VectorXd x_seed = state_manager_.getStateVector();
            for (const auto& obs : observations) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                auto piv_it = bssd_pivot_sat_.find(cons);
                if (piv_it == bssd_pivot_sat_.end() || piv_it->second != obs.satellite)
                    continue;
                for (const auto& [ref_type, val] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    const std::string pkey = cons + "|" + ref_type;
                    if (pivot_abs_seeds.count(pkey)) continue;
                    pivot_abs_seeds[pkey] =
                        state_manager_.computeAmbiguitySeed(obs, ref_type, x_seed);
                }
            }
        }

        for (const auto& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty() || ref_type[0] != 'L') continue;
                const std::string amb_key = obs.satellite + "_" + ref_type;
                if (!required_ambiguities.count(amb_key)) continue;

                const int existing_idx = state_manager_.getAmbiguityIndex(amb_key);

                // Determine whether this ambiguity needs (re-)seeding.
                //  - New state (not yet in the state vector): always seed.
                //  - Existing state whose value was zeroed by resetAmbiguity():
                //    detect via large variance (resetAmbiguity sets var = amb_std²).
                //    A converged ambiguity near 0 has a much smaller variance
                //    and should NOT be re-seeded.
                bool needs_seed = (existing_idx < 0);
                if (!needs_seed && existing_idx >= 0) {
                    const double val = state_manager_.getStateVector()(existing_idx);
                    const double var = state_manager_.getCovariance()(existing_idx, existing_idx);
                    const double reset_var_thresh = 0.5 * config_.getAmbiguityStd() * config_.getAmbiguityStd();
                    needs_seed = (val == 0.0 && var >= reset_var_thresh);
                }

                if (needs_seed) {
                    const Eigen::VectorXd x_now = state_manager_.getStateVector();
                    double seed = state_manager_.computeAmbiguitySeed(obs, ref_type, x_now);

                    // In BSSD mode convert the absolute seed to a
                    // single-difference seed relative to the pivot.
                    if (bssd_active) {
                        const std::string cons = PppUtils::getSystemString(obs.satellite);
                        auto piv_it = bssd_pivot_sat_.find(cons);
                        const bool is_pivot = piv_it != bssd_pivot_sat_.end()
                                           && piv_it->second == obs.satellite;
                        if (is_pivot) {
                            seed = 0.0;
                        } else {
                            const std::string pkey = cons + "|" + ref_type;
                            auto ps_it = pivot_abs_seeds.find(pkey);
                            if (ps_it != pivot_abs_seeds.end())
                                seed -= ps_it->second;
                        }
                    }

                    if (existing_idx < 0) {
                        state_manager_.addAmbiguity(amb_key, seed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added missing ambiguity state for {} (key={}), seed={:.3f} m{}",
                            epoch_prefix, obs.satellite, amb_key, seed,
                            bssd_active ? " (SD)" : "");
                    } else {
                        Eigen::VectorXd x_reseed = x_now;
                        x_reseed(existing_idx) = seed;
                        state_manager_.setStateVector(x_reseed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Re-seeded reset ambiguity for {} (key={}), seed={:.3f} m{}",
                            epoch_prefix, obs.satellite, amb_key, seed,
                            bssd_active ? " (SD)" : "");
                    }
                }
            }
        }

        state_manager_.pruneAmbiguitiesExcept(required_ambiguities);

        // --- Mark relaxed ambiguities -----------------------------------------
        // Phase IF ambiguities where the combination lacks full BIA or
        // receiver ATX coverage receive a non-zero process noise
        // (phase_bias_q).  The slowly-varying state tracks the
        // elevation-dependent receiver PCV residual instead of holding
        // a constant value.  This replaces the previous approach of
        // downweighting the phase measurement by 100×.
        state_manager_.clearRelaxedAmbiguities();
        if (!is_uncombined) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    if (ref_type.find('_') == std::string::npos) continue;
                    const std::string amb_key = obs.satellite + "_" + ref_type;
                    if (!required_ambiguities.count(amb_key)) continue;
                    // Relax if missing BIA or receiver ATX on either component signal
                    const bool missing_bia = !ifCombHasFullBia(obs.satellite, ref_type);
                    const bool missing_atx = !ifCombHasFullRxAtx(ref_type, obs.system);
                    if (missing_bia || missing_atx) {
                        state_manager_.markAmbiguityRelaxed(amb_key);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Relaxed ambiguity {}: missing_bia={}, missing_atx={}",
                            epoch_prefix, amb_key, missing_bia, missing_atx);
                    }
                }
            }
        }

        // Receiver bias states — DCB + per-extra-band IFB (PPP-RTK uncombined).
        //
        // The satellite clock is estimated from an IF combination of two
        // reference code signals.  For observations on those two reference
        // bands the receiver hardware delay is parameterised by a single
        // DCB state with GF-IF coefficients.  For any additional frequency
        // band (e.g. L5, E5b, E6) an independent IFB state is added with
        // coefficient 1.0, since the hardware delay on that band cannot be
        // decomposed into the dual-frequency DCB.
        //
        // Only code observations drive the receiver bias states.  Phase
        // observations absorb the receiver phase hardware delay into the
        // float ambiguity, so they do not require a receiver bias state.
        std::set<std::string> required_code_biases;
        if (config_.estimateIfb() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    const auto bias = resolveReceiverBias(ref_type, obs.system, obs.satellite);
                    if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;
                    if (required_code_biases.count(bias.state_key)) continue;
                    required_code_biases.insert(bias.state_key);
                    if (state_manager_.getCodeBiasIndex(bias.state_key) < 0) {
                        state_manager_.addCodeBiasState(bias.state_key, 0.0);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added receiver bias state for {}", epoch_prefix, bias.state_key);
                    }
                }
            }
        }
        state_manager_.pruneCodeBiasStatesExcept(required_code_biases);

        // Per-satellite signal bias states (PPP-RTK uncombined).
        // For signals without BIA product coverage, a per-satellite bias
        // state absorbs the uncorrected satellite hardware delay.
        std::set<std::string> required_sat_biases;
        if (config_.estimateSatBiases() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type.find('_') != std::string::npos) continue;
                    if (ref_type[0] != 'C' && ref_type[0] != 'L') continue;
                    // Only add state for signals that lack BIA coverage
                    if (ppp_model_.hasBiasEntry(obs.satellite, ref_type)) continue;
                    const std::string key = obs.satellite + "|" + ref_type;
                    required_sat_biases.insert(key);
                    if (state_manager_.getSatBiasIndex(key) < 0) {
                        state_manager_.addSatBiasState(key, 0.0);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added sat bias state for {}", epoch_prefix, key);
                    }
                }
            }
        }
        // Per-satellite IF bias states (PPP combined / iono-free mode).
        // For IF code combinations where at least one component signal
        // lacks BIA coverage, add a per-satellite bias state to absorb
        // the residual uncorrected satellite hardware delay.  Phase IF
        // combinations do not need this — the float ambiguity absorbs it.
        if (config_.estimateSatBiases() && !is_uncombined) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    if (ref_type.find('_') == std::string::npos) continue;
                    if (ifCombHasFullBia(obs.satellite, ref_type)) continue;
                    const std::string key = obs.satellite + "|" + ref_type;
                    required_sat_biases.insert(key);
                    if (state_manager_.getSatBiasIndex(key) < 0) {
                        state_manager_.addSatBiasState(key, 0.0);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added IF sat bias state for {}", epoch_prefix, key);
                    }
                }
            }
        }

        state_manager_.pruneSatBiasStatesExcept(required_sat_biases);
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool MeasurementModel::isUsableMeasurement(const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) const {
        if (ref_type.empty()) return false;

        // GLONASS FDMA uses satellite-dependent frequencies, so the receiver
        // inter-channel bias (ICB) does NOT cancel in between-satellite
        // single-differences.  Exclude GLONASS when BSSD is active.
        if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()) {
            return false;
        }

        const MeasurementType mt = classifyMeasurement(ref_type);

        // Accept Code, Phase, and (if velocity estimation is active) Doppler.
        if (mt != MeasurementType::Code && mt != MeasurementType::Phase &&
            !(mt == MeasurementType::Doppler && config_.estimateVelocity()))
            return false;

        const bool is_uncombined = config_.isUncombinedEnabled();
        const bool has_underscore = (ref_type.find('_') != std::string::npos);

        // Doppler measurements are always single-frequency observables (no IF combination).
        if (mt == MeasurementType::Doppler) {
            if (has_underscore) return false;
        } else if (is_uncombined) {
            if (has_underscore) return false;
        } else {
            if (!has_underscore) return false;
        }

        if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type))
            return false;

        const std::string excl_key = obs.satellite + "_" + ref_type;
        if (excluded.count(excl_key)) {
            log_manager_.getLogger("measurement_model")->debug(
                "{}Excluding previously rejected measurement: {}", epoch_prefix, excl_key);
            return false;
        }

        return true;
    }

    // ===================================================================
    // Jacobian + innovation assembly
    // ===================================================================

    bool MeasurementModel::buildJacobianRow(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        const MeasurementType mtype = classifyMeasurement(ref_type);
        const bool is_phase = (mtype == MeasurementType::Phase);
        const bool is_doppler = (mtype == MeasurementType::Doppler);
        const bool is_uncombined = config_.isUncombinedEnabled();
        const double mapping_wet = obs.mf_w;

        jacobian = Eigen::VectorXd::Zero(state_size);

        // -----------------------------------------------------------------
        // Doppler measurements: range-rate = LOS . (sat_vel - rx_vel)
        // H_vel = -LOS.
        // Doppler measurements do not depend on tropo, ambiguity, iono, etc.
        // -----------------------------------------------------------------
        if (is_doppler) {
            if (!state_manager_.hasVelocity()) return false;

            // Velocity partial: -LOS
            jacobian.segment<3>(state_manager_.getVelocityIndex()) = -obs.los;

            // Predicted range-rate: modeled (sat contribution) - LOS . rx_vel
            const Eigen::Vector3d rx_vel = x.segment<3>(state_manager_.getVelocityIndex());
            predicted = obs.modeled.at(ref_type) - obs.los.dot(rx_vel);

            // --- Tightly-coupled INS partials for Doppler ---
            // In TC mode the velocity error includes the attitude-error coupling:
            //   ?v = ... + skew(C_b^e * f_b) * ?? + C_b^e * ?ba
            // Therefore:
            //   ?(range-rate)/???  = -LOS^T * [C*f]×
            //   ?(range-rate)/??ba = -LOS^T * C_b^e
            if (config_.tightlyCoupledImu() && state_manager_.hasImuBiases()) {
                const Eigen::Vector3d f_e = tc_C_b_e_ * tc_f_b_; // specific force in ECEF
                // skew(f_e) such that skew(f_e) * ?? = f_e × ??
                Eigen::Matrix3d skew_fe;
                skew_fe <<       0.0, -f_e(2),  f_e(1),
                            f_e(2),      0.0, -f_e(0),
                           -f_e(1),  f_e(0),      0.0;

                // Attitude error partial: ?(range-rate)/??? = -LOS^T * skew(f_e)
                const Eigen::Vector3d att_partial = -skew_fe.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAttitudeIndex()) = att_partial;

                // Accelerometer bias partial: ?(range-rate)/??ba = -LOS^T * C
                const Eigen::Vector3d ba_partial = -tc_C_b_e_.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAccelBiasIndex()) = ba_partial;
            }

            return true;
        }

        // Position partial: -LOS
        jacobian.segment<3>(state_manager_.getPositionIndex()) = -obs.los;

        // Troposphere wet delay
        jacobian(state_manager_.getTropoIndex()) = mapping_wet;

        // Receiver clock (common to code and phase)
        jacobian(state_manager_.getClockIndex()) = 1.0;
        const double rx_clk = x(state_manager_.getClockIndex());

        // Phase clock offset (additive delta for phase only)
        double phase_clk_value = 0.0;
        if (is_phase && state_manager_.hasPhaseClock()) {
            jacobian(state_manager_.getPhaseClockIndex()) = 1.0;
            phase_clk_value = x(state_manager_.getPhaseClockIndex());
        }

        // Tropospheric gradients (Chen & Herring, 1997)
        double tropo_grad_term = 0.0;
        if (state_manager_.hasTropoGradients()) {
            const double el_rad = obs.elevation * Constants::PI / 180.0;
            const double az_rad = obs.azimuth * Constants::PI / 180.0;
            const double sin_el = std::sin(el_rad);
            const double tan_el = std::tan(el_rad);
            const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
            const double cos_az = std::cos(az_rad);
            const double sin_az = std::sin(az_rad);

            jacobian(state_manager_.getTropoGradientNorthIndex()) = mf_grad * cos_az;
            jacobian(state_manager_.getTropoGradientEastIndex())  = mf_grad * sin_az;

            tropo_grad_term = mf_grad * (x(state_manager_.getTropoGradientNorthIndex()) * cos_az
                                        + x(state_manager_.getTropoGradientEastIndex()) * sin_az);
        }

        // Inter-system time offset
        const std::string sys_name = systemToIsbName(obs.system);
        double time_offset_value = 0.0;
        if (!sys_name.empty()) {
            const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
            if (isb_idx < state_size) {
                jacobian(isb_idx) = 1.0;
                time_offset_value = x(isb_idx);
            }
        }

        // Per-satellite ionosphere (PPP-RTK uncombined)
        double iono_term = 0.0;
        if (is_uncombined) {
            const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(state_size)) {
                const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
                if (freq > 0.0) {
                    const double f_ref = Constants::GPS_L1_FREQ;
                    const double alpha = (f_ref * f_ref) / (freq * freq);
                    const double iono_coeff = (is_phase ? -1.0 : 1.0) * alpha;
                    jacobian(iono_idx) = iono_coeff;
                    iono_term = iono_coeff * x(iono_idx);
                }
            }
        }

        // Receiver bias — DCB for reference bands, IFB for extra bands
        // (PPP-RTK uncombined, code observations only).
        //
        // The receiver hardware delay for code is parameterised explicitly:
        // for the two clock-reference bands the GF-IF coefficient ?_j maps
        // the per-system DCB state into the code equation; for any additional
        // band an independent IFB state enters with coefficient 1.0.
        //
        // Phase observations do NOT include the receiver code bias state.
        // The receiver phase hardware delay is absorbed into the float
        // ambiguity, following the standard PPP-RTK parameterisation
        // (consistent with cssrlib's separate cbias/pbias handling).
        double code_bias_term = 0.0;
        if (config_.estimateIfb() && is_uncombined && !is_phase) {
            const auto bias = resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
                const int cb_idx = state_manager_.getCodeBiasIndex(bias.state_key);
                if (cb_idx >= 0 && cb_idx < static_cast<int>(state_size)) {
                    jacobian(cb_idx) = bias.coefficient;
                    code_bias_term = bias.coefficient * x(cb_idx);
                }
            }
        }

        // Per-satellite signal bias (uncorrected satellite HW delay).
        // Uncombined mode: coefficient 1.0 for both code and phase.
        // IF mode: coefficient 1.0 for code IF combinations with
        // incomplete BIA coverage (phase IF absorbed by ambiguity).
        double sat_bias_term = 0.0;
        if (config_.estimateSatBiases()) {
            const bool is_if = (ref_type.find('_') != std::string::npos);
            const bool applicable = is_uncombined
                ? !is_if
                : (is_if && !is_phase);
            if (applicable) {
                const std::string sb_key = obs.satellite + "|" + ref_type;
                const int sb_idx = state_manager_.getSatBiasIndex(sb_key);
                if (sb_idx >= 0 && sb_idx < static_cast<int>(state_size)) {
                    jacobian(sb_idx) = 1.0;
                    sat_bias_term = x(sb_idx);
                }
            }
        }

        // Assemble predicted observable
        const double modeled = obs.modeled.at(ref_type);
        const double zwd = x(state_manager_.getTropoIndex());
        const double tropo_wet_term = mapping_wet * zwd;

        const Eigen::Vector3d pos_current = x.segment<3>(state_manager_.getPositionIndex());
        const double range_correction = -obs.los.dot(pos_current - obs.rx_pos_linearization);

        predicted = modeled + range_correction + rx_clk + phase_clk_value
            + tropo_wet_term + tropo_grad_term + time_offset_value
            + iono_term + code_bias_term + sat_bias_term;

        // Ambiguity (phase only)
        if (is_phase) {
            const std::string amb_key = obs.satellite + "_" + ref_type;
            const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
            if (amb_idx < 0) return false;
            jacobian(amb_idx) = 1.0;
            predicted += x(amb_idx);
        }

        // --- Tightly-coupled INS partials for code/phase (range) ---
        // The position error ?p is already handled by the -LOS partial above.
        // In TC mode the attitude error state ?? contributes to the position
        // error budget through the INS error-state transition matrix (?).
        // The cross-covariance P(pos, att) from the IMU prediction already
        // propagates this coupling, so the range Jacobian does not need an
        // explicit ??/??? term.  However, to improve the *direct*
        // observability of IMU biases from range measurements, we add the
        // second-order position-dependent partial when TC mode is active:
        //   ??/???  ? 0  (attitude ? position coupling is in ?, not H)
        //   ??/??ba ? 0  (same reasoning)
        //   ??/??bg ? 0
        // These remain zero; the cross-covariance from the IMU prediction
        // provides the coupling.  The explicit attitude-error partials in
        // the Doppler row (above) provide direct observability for the
        // bias and attitude states in tightly-coupled mode.

        return true;
    }

    // ===================================================================
    // computeMeasurementUpdate — top-level orchestrator
    // ===================================================================

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;
        update.excluded_measurements = excluded_measurements;
        const std::string epoch_tag = TimeUtils::formatGnssTime(update.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        if (observations.empty()) {
            log_manager_.getLogger("measurement_model")->error("{}No observations provided for measurement update, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- First-epoch initialization ---
        if (!isb_initialized_) {
            initializeInterSystemBiases(observations, epoch_prefix);
            isb_initialized_ = true;
        }
        if (!phase_clock_initialized_ && config_.estimatePhaseClock()) {
            initializePhaseClockOffset(observations, epoch_prefix);
            phase_clock_initialized_ = true;
        }

        // --- BSSD pivot handover (re-reference SD ambiguities before state pre-pass) ---
        // Must run before ensureRequiredStates() so that newly-created ambiguities
        // can use the correct (possibly updated) pivot for SD seeding.
        if (config_.enableBetweenSatelliteSd())
            handleBssdPivotHandover(observations, epoch_prefix);

        // --- State pre-pass (create/seed/prune ambiguities, iono, code biases) ---
        ensureRequiredStates(observations, epoch_prefix);

        // --- First-epoch code bias (DCB/IFB) seeding ---
        // Runs after ensureRequiredStates has created the code bias states and
        // after iono states are seeded so the residual differences are accurate.
        if (!code_bias_initialized_ && config_.estimateIfb()) {
            initializeCodeBiases(observations, epoch_prefix);
            code_bias_initialized_ = true;
        }

        // --- Freeze pivot ambiguity and ionosphere covariance ---
        // Must run after ensureRequiredStates so that newly-created pivot
        // ambiguities (which start with large initial variance) are frozen.
        // The pivot's ionosphere column is also zeroed in the BSSD
        // S-transformation, so its variance must be frozen too.
        if (config_.enableBetweenSatelliteSd()) {
            freezePivotAmbiguityCovariance();
            freezePivotIonosphereCovariance();
        }

        // --- Validate state vector ---
        const Eigen::VectorXd x = state_manager_.getStateVector();
        if (x.size() < 5) {
            log_manager_.getLogger("measurement_model")->error("{}Invalid state vector size: {}, expected at least 5, mode={}",
                epoch_prefix, x.size(), modeLabel(config_));
            return update;
        }

        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(x.size()) != state_size) {
            log_manager_.getLogger("measurement_model")->error("{}State vector size {} does not match StateManager size {}, mode={}",
                epoch_prefix, x.size(), state_size, modeLabel(config_));
            return update;
        }

        // --- Count usable measurements ---
        size_t num_obs = 0;
        for (const auto& obs : observations)
            for (const auto& [ref_type, observed] : obs.observed)
                if (isUsableMeasurement(obs, ref_type, excluded_measurements, epoch_prefix))
                    ++num_obs;

        if (num_obs == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid observations found in residuals, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- Allocate output ---
        update.innovations.resize(num_obs);
        update.R_v.resize(num_obs);
        update.H.resize(num_obs, state_size);
        update.H.setZero();
        update.satellite_ids.reserve(num_obs);
        update.signal_types.reserve(num_obs);

        // --- Assemble rows ---
        size_t row = 0;
        for (const auto& obs : observations) {
            // Validate stored geometry
            if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored satellite position for {} at TOW {:.3f}: norm={:.3e} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.sat_pos.norm(), modeLabel(config_));
                continue;
            }
            if (obs.range < 1.5e7 || !std::isfinite(obs.range)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.range, modeLabel(config_));
                continue;
            }
            if (obs.elevation < config_.getElevationMaskDeg()) {
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.elevation, modeLabel(config_));
                continue;
            }
            if (obs.los.norm() < 0.5 || !obs.los.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored LOS for {} at TOW {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, modeLabel(config_));
                continue;
            }
            if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid wet mapping function for {} at TOW {:.3f}: {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.mf_w, modeLabel(config_));
                continue;
            }

            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeasurement(obs, ref_type, excluded_measurements, epoch_prefix)) continue;

                try {
                    Eigen::VectorXd jacobian;
                    double predicted = 0.0;
                    if (!buildJacobianRow(obs, ref_type, x, state_size, jacobian, predicted))
                        continue;

                    update.innovations(row) = observed - predicted;
                    update.R_v(row) = obs.variances.at(ref_type);

                    // Downweight code measurements on signals without BIA
                    // coverage when estimate_sat_biases is off.  The
                    // uncorrected satellite HW delay acts as unmodelled
                    // noise on code; phase absorbs it into the float
                    // ambiguity and needs no inflation.
                    // Covers both uncombined raw signals and IF combinations.
                    if (!config_.estimateSatBiases()
                        && config_.noBiaCodeDownweight() > 1.0
                        && ref_type.size() >= 2 && ref_type[0] == 'C') {
                        const bool is_if = (ref_type.find('_') != std::string::npos);
                        const bool missing_bia = is_if
                            ? !ifCombHasFullBia(obs.satellite, ref_type)
                            : (config_.isUncombinedEnabled()
                               && !ppp_model_.hasBiasEntry(obs.satellite, ref_type));
                        if (missing_bia)
                            update.R_v(row) *= config_.noBiaCodeDownweight();
                    }

                    // Phase IF combinations with incomplete product coverage
                    // (missing BIA or receiver ATX) are handled by a relaxed
                    // ambiguity (non-zero process noise) that tracks the
                    // elevation-dependent receiver PCV residual.  No
                    // variance inflation is needed.

                    update.H.row(row) = jacobian;
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);

                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6;
                    }

                    log_manager_.getLogger("measurement_model")->trace(
                        "{}Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}",
                        epoch_prefix, obs.satellite, ref_type,
                        observed, obs.modeled.at(ref_type), predicted, update.innovations(row));

                    row++;
                }
                catch (const std::exception&) {
                    continue;
                }
            }
        }

        // --- Finalize ---
        if (row == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid measurement updates computed, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger("measurement_model")->warn("{}Processed {} observations out of {} expected, mode={}",
                epoch_prefix, row, num_obs, modeLabel(config_));
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }
        update.bad_residual_count = 0;
        log_manager_.getLogger("measurement_model")->info("{}Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
            epoch_prefix, row, update.H.rows(), update.H.cols(), modeLabel(config_));

        if (config_.enableBetweenSatelliteSd()) {
            applyBetweenSatelliteSingleDifference(update, epoch_prefix);
        }

        return update;
    }

    // ===================================================================
    // Validation
    // ===================================================================

    bool MeasurementModel::validateMeasurementUpdate(const EpochMeasurement& measurement) const {
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        if (measurement.innovations.size() != measurement.R_v.size() ||
            measurement.innovations.size() != measurement.H.rows() ||
            measurement.H.cols() != state_manager_.getStateSize() ||
            measurement.innovations.size() != measurement.satellite_ids.size() ||
            measurement.innovations.size() != measurement.signal_types.size()) {
            log_manager_.getLogger("measurement_model")->error(
                "{}Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
                "sat_ids={}, signal_types={}, state={}, mode={}",
                epoch_prefix,
                measurement.innovations.size(), measurement.R_v.size(),
                measurement.H.rows(), measurement.H.cols(),
                measurement.satellite_ids.size(), measurement.signal_types.size(),
                state_manager_.getStateSize(), modeLabel(config_));
            return false;
        }

        for (size_t i = 0; i < measurement.innovations.size(); ++i) {
            if (!std::isfinite(measurement.innovations(i)) || !std::isfinite(measurement.R_v(i))) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), modeLabel(config_));
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-finite Jacobian row for {} {}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i], modeLabel(config_));
                return false;
            }
        }

        log_manager_.getLogger("measurement_model")->trace(
            "{}Validated measurement update: {} measurements, mode={}",
            epoch_prefix, measurement.innovations.size(), modeLabel(config_));
        return true;
    }

    // ===================================================================
    // First-epoch initializers
    // ===================================================================

    void MeasurementModel::initializeInterSystemBiases(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        // Collect code residuals per system.
        // GPS residuals include receiver clock; other systems include clock + ISB.
        // ISB ? median(non-GPS residuals) ? median(GPS residuals).

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
            log_manager_.getLogger("measurement_model")->warn(
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

            log_manager_.getLogger("measurement_model")->info(
                "{}Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
                epoch_prefix, sys_name, isb_estimate, isb_estimate / Constants::SPEED_OF_LIGHT,
                residuals.size(), system_residuals["GPS"].size());
        }

        state_manager_.setStateVector(x_updated);
    }

    void MeasurementModel::initializePhaseClockOffset(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        // Compute the median code?phase residual difference across satellites.
        // This captures the receiver-wide code-phase bias used to seed the
        // phase clock state so the filter does not start from zero.

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
            log_manager_.getLogger("measurement_model")->warn(
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

            log_manager_.getLogger("measurement_model")->info(
                "{}Set phase clock covariance: std={:.3f} m (seed_std={:.3f} m from {} pairs)",
                epoch_prefix, clamped_std, seed_std, code_phase_diffs.size());
        }

        log_manager_.getLogger("measurement_model")->info(
            "{}Initialized phase clock offset: {:.3f} m from {} code-phase pairs",
            epoch_prefix, phase_clock_seed, code_phase_diffs.size());
    }

    void MeasurementModel::initializeCodeBiases(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        // Seed each DCB/IFB state from the median cross-band code residual
        // difference.  For a reference band j with GF-IF coefficient ?_j the
        // code residual on band j (after clock/tropo/ISB removal) is:
        //   r_j ? ?_j·I + ?_j·DCB
        // The same residual on band k ? j is:
        //   r_k ? ?_k·I + ?_k·DCB          (reference bands)
        //   r_k ? ?_k·I + IFB_k            (non-reference band)
        //
        // For a non-reference band the difference r_k - r_1 (using the
        // first reference band) isolates the IFB_k:
        //   r_k - r_1 ? (?_k - ?_1)·I + IFB_k - ?_1·DCB
        // At initialization DCB?0 and iono is seeded, so this gives a
        // reasonable first estimate of IFB_k.
        //
        // For the DCB itself we use the two reference bands:
        //   r_1 - r_2 ? (?_1 - ?_2)·I + (?_1 - ?_2)·DCB
        //   DCB ? (r_1 - r_2 - (?_1 - ?_2)·I) / (?_1 - ?_2)

        if (!config_.estimateIfb() || !config_.isUncombinedEnabled()) return;

        const Eigen::VectorXd x = state_manager_.getStateVector();
        auto log = log_manager_.getLogger("measurement_model");

        // Collect per-band code residuals across all satellites.
        // Key: bias state_key  ?  list of (residual_diff, iono_diff, beta_diff)
        struct BiasSample {
            double residual_diff; // r_j - r_ref1
            double iono_diff;    // (alpha_j - alpha_ref1) * I_est
            double beta_diff;    // coeff_j - coeff_ref1  (for DCB); 1.0 (for IFB)
        };
        std::map<std::string, std::vector<BiasSample>> samples;

        for (const auto& obs : observations) {
            if (obs.elevation < config_.getElevationMaskDeg()) continue;

            // Gather code residuals per band digit for this satellite
            struct BandResidual {
                double residual;
                double alpha;
                ReceiverBiasEntry bias;
            };
            std::map<char, BandResidual> band_residuals;
            for (const auto& [ref_type, val] : obs.observed) {
                if (ref_type.empty() || ref_type[0] != 'C' || ref_type.size() < 2) continue;
                if (ref_type.find('_') != std::string::npos) continue; // skip IF
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

            // Use the first reference band as the differencing anchor
            const char sys_char = systemToClockRefChar(obs.system);
            const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
            auto crit = clock_refs.find(sys_char);
            if (crit == clock_refs.end()) continue;
            const char ref1_band = (crit->second.sig1.size() >= 2) ? crit->second.sig1[1] : '\0';
            if (ref1_band == '\0') continue;

            auto ref1_it = band_residuals.find(ref1_band);
            if (ref1_it == band_residuals.end()) continue;
            const auto& ref1 = ref1_it->second;

            // Current ionosphere estimate for this satellite
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
                // For IFB states (non-ref band): coeff=1.0, ref1 coeff=??
                // For DCB states (2nd ref band): coeff=??, ref1 coeff=??
                const double beta_diff = br.bias.coefficient - ref1.bias.coefficient;

                if (std::abs(beta_diff) < 1e-12) continue;

                samples[br.bias.state_key].push_back({ r_diff, iono_diff, beta_diff });
            }
        }

        if (samples.empty()) return;

        Eigen::VectorXd x_updated = x;
        for (const auto& [key, sv] : samples) {
            // Estimate bias = median((r_diff - iono_diff) / beta_diff)
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

    // ===================================================================
    // Between-satellite single difference
    // ===================================================================

    /**
     * @brief Detect and handle BSSD pivot satellite handovers.
     *
     * Implements two mechanisms:
     *
     * 1. **Sticky pivot with hysteresis.**  The current pivot is retained as
     *    long as it is present in the epoch and its elevation exceeds the
     *    configured threshold (bssd_pivot_min_elevation).  A handover to the
     *    new highest-elevation satellite only occurs when the current pivot
     *    drops below the threshold or disappears entirely.  This prevents
     *    frequent ping-pong between satellites at similar elevations.
     *
     * 2. **Pivot ambiguity reset detection.**  If the current pivot's phase
     *    ambiguity state has been disturbed (value ? 0 after it should have
     *    been frozen, or covariance blown up by a cycle-slip or outlier
     *    reset), all SD phase ambiguities in that constellation for the
     *    affected signal are reset to fresh absolute seeds.  This ensures
     *    the BSSD starts from a consistent baseline rather than propagating
     *    a discontinuity through every non-pivot ambiguity.
     *
     * Correction per phase signal type on a normal handover:
     *   For non-pivot satellite i:   N(i) ? N(i) ? N(new_ref)
     *   For old_ref (now regular):   N(old_ref) ? ?N(new_ref)
     *   For new_ref (now frozen):    N(new_ref) ? 0
     */
    void MeasurementModel::handleBssdPivotHandover(
        const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix)
    {
        if (!config_.isUncombinedEnabled()) return;

        auto log = log_manager_.getLogger("measurement_model");
        const double pivot_min_el = config_.getBssdPivotMinElevation();

        // Build satellite ? elevation and satellite ? constellation maps.
        std::map<std::string, double> sat_elev;
        std::map<std::string, std::string> sat_cons;
        for (const auto& obs : observations) {
            // Skip GLONASS — excluded from BSSD due to FDMA ICBs.
            if (obs.system == System::GLONASS) continue;
            sat_elev[obs.satellite] = obs.elevation;
            sat_cons[obs.satellite] = PppUtils::getSystemString(obs.satellite);
        }

        // Find the highest-elevation satellite per constellation.
        std::map<std::string, std::string> highest_sat;   // constellation -> satellite
        std::map<std::string, double>      highest_el;
        for (const auto& obs : observations) {
            if (obs.system == System::GLONASS) continue;
            const std::string& cons = sat_cons[obs.satellite];
            auto it = highest_el.find(cons);
            if (it == highest_el.end() || obs.elevation > it->second) {
                highest_sat[cons] = obs.satellite;
                highest_el[cons]  = obs.elevation;
            }
        }

        // Determine the pivot for each constellation (sticky with hysteresis).
        std::map<std::string, std::string> new_pivot_sat;
        for (const auto& [cons, best_sat] : highest_sat) {
            auto prev_it = bssd_pivot_sat_.find(cons);
            if (prev_it != bssd_pivot_sat_.end()) {
                const std::string& old_pivot = prev_it->second;
                auto elev_it = sat_elev.find(old_pivot);
                if (elev_it != sat_elev.end() && elev_it->second >= pivot_min_el) {
                    // Current pivot is still visible and above threshold — keep it.
                    new_pivot_sat[cons] = old_pivot;
                    continue;
                }
                // Old pivot dropped below threshold or disappeared.
            }
            // First epoch or handover required: use highest elevation.
            new_pivot_sat[cons] = best_sat;
        }

        if (bssd_pivot_sat_.empty()) {
            // Very first BSSD epoch — just record pivots, no corrections.
            // Pivot covariance freezing is handled by
            // freezePivotAmbiguityCovariance() which runs after
            // ensureRequiredStates() in the caller.
            bssd_pivot_sat_ = new_pivot_sat;
            return;
        }

        // Collect phase signal types present this epoch.
        std::set<std::string> phase_sigs;
        for (const auto& obs : observations)
            for (const auto& [sig, val] : obs.observed)
                if (!sig.empty() && sig[0] == 'L')
                    phase_sigs.insert(sig);

        const Eigen::VectorXd x = state_manager_.getStateVector();
        Eigen::MatrixXd P = state_manager_.getCovariance();
        Eigen::VectorXd x_mod = x;
        bool state_modified = false;
        bool cov_modified   = false;

        const double amb_std = config_.getInitialStd().ambiguity;

        // Helper: check whether the **pivot** satellite's ambiguity has been
        // disturbed from its expected frozen state (value=0, variance=0).
        //
        // Under BSSD the pivot's ambiguity is frozen: value=0 and
        // variance=0 (the S-transformation zeroes its H-column so it
        // receives no Kalman update).  A disturbance means something
        // external set value != 0 or inflated the variance (cycle-slip
        // reset, complete filter reset, outlier handler, etc.).
        //
        // This check must NOT be applied to non-pivot satellites: they
        // are expected to have non-zero SD values and may have large
        // variance (unconverged or recently created).  Those conditions
        // are normal and do not make a satellite unsuitable as a
        // replacement pivot.
        auto isPivotDisturbed = [&](const std::string& sat) -> bool {
            for (const auto& sig : phase_sigs) {
                const std::string key = sat + "_" + sig;
                const int idx = state_manager_.getAmbiguityIndex(key);
                if (idx < 0) continue;
                // The pivot's frozen state is (0, 0).  Any deviation means
                // something corrupted it.  Use a small tolerance for
                // floating-point drift.
                if (std::abs(x_mod(idx)) > 1e-6 || P(idx, idx) > 1e-12)
                    return true;
            }
            return false;
        };

        for (auto& [cons, pivot] : new_pivot_sat) {
            const std::string& old_pivot = bssd_pivot_sat_.count(cons)
                                         ? bssd_pivot_sat_.at(cons) : pivot;

            // --- Check for pivot ambiguity disturbance (even if no handover) ---
            // If the pivot's frozen ambiguity state was corrupted (value != 0
            // or variance > 0), hand over to the highest-elevation satellite
            // in the constellation using the lossless T P T' re-referencing.
            // Any visible satellite (except the disturbed pivot) is a valid
            // candidate — non-zero SD values and high variance on non-pivot
            // satellites are expected under BSSD and do not constitute a
            // disturbance.
            if (isPivotDisturbed(pivot)) {
                log->warn("{}BSSD pivot {} disturbed in {}. "
                          "Searching for healthy replacement.",
                          epoch_prefix, pivot, cons);

                // Find the highest-elevation satellite in this constellation
                // (excluding the current disturbed pivot).
                std::string best_replacement;
                double best_replacement_el = -1.0;
                for (const auto& [sat, el] : sat_elev) {
                    if (sat_cons.count(sat) && sat_cons.at(sat) == cons
                        && sat != pivot) {
                        if (el > best_replacement_el) {
                            best_replacement = sat;
                            best_replacement_el = el;
                        }
                    }
                }

                if (!best_replacement.empty()) {
                    log->info("{}BSSD pivot disturbance recovery for {}: {} -> {} "
                              "(elev={:.1f}°). Using lossless handover.",
                              epoch_prefix, cons, pivot, best_replacement, best_replacement_el);
                    pivot = best_replacement;
                    // Fall through to the normal handover path below, which
                    // applies the lossless T P T' re-referencing.
                } else {
                    log->warn("{}BSSD pivot disturbance in {}: no replacement found "
                              "(single-satellite constellation). "
                              "Re-zeroing disturbed pivot {} in place.",
                              epoch_prefix, cons, pivot);
                    // Only one satellite in the constellation — re-zero the
                    // pivot's own ambiguity states and continue.
                    for (const auto& sig : phase_sigs) {
                        const std::string piv_key = pivot + "_" + sig;
                        const int piv_idx = state_manager_.getAmbiguityIndex(piv_key);
                        if (piv_idx < 0) continue;
                        const auto pidx = static_cast<Eigen::Index>(piv_idx);
                        x_mod(pidx) = 0.0;
                        P.row(pidx).setZero();
                        P.col(pidx).setZero();
                        state_modified = true;
                        cov_modified = true;
                    }
                    continue;
                }
            }

            const bool is_handover = (pivot != old_pivot);

            // --- Normal handover (no disturbance) ---
            //
            // Re-reference SD ambiguities from old pivot to new pivot.
            // The state transformation is:
            //   N'(new_ref)  = 0
            //   N'(old_ref)  = N(old_ref) - N(new_ref)     (= -N(new_ref))
            //   N'(i)        = N(i)       - N(new_ref)
            //
            // The corresponding covariance transformation is  P' = T P T'
            // where T is identity except for the affected ambiguity rows:
            //   T(i, i)           = 1          for all i
            //   T(i, new_ref_idx) = -1         for every i != new_ref in this constellation/sig
            //   T(new_ref, :)     = 0 row with T(new_ref, new_ref) = 0  (frozen to 0)
            //
            // We build the compact transformation per signal type.
            if (!is_handover) continue;

            log->info("{}BSSD pivot handover for {}: {} -> {} "
                      "(old elev={:.1f}°, threshold={:.1f}°)",
                      epoch_prefix, cons, old_pivot, pivot,
                      sat_elev.count(old_pivot) ? sat_elev.at(old_pivot) : -1.0,
                      pivot_min_el);

            for (const auto& sig : phase_sigs) {
                const std::string new_ref_key = pivot + "_" + sig;
                const int new_ref_idx = state_manager_.getAmbiguityIndex(new_ref_key);
                if (new_ref_idx < 0) continue;

                const double n_new_ref = x_mod(new_ref_idx);
                const auto ri = static_cast<Eigen::Index>(new_ref_idx);

                // Collect all constellation ambiguity indices for this signal
                // (including the new ref) so we can apply the covariance transform.
                std::vector<Eigen::Index> group_indices;
                for (const auto& [amb_key, amb_idx] : state_manager_.getAmbiguities()) {
                    const auto us = amb_key.find('_');
                    if (us == std::string::npos) continue;
                    const std::string amb_sat = amb_key.substr(0, us);
                    const std::string amb_sig = amb_key.substr(us + 1);
                    if (amb_sig != sig) continue;
                    if (PppUtils::getSystemString(amb_sat) != cons) continue;
                    group_indices.push_back(static_cast<Eigen::Index>(amb_idx));
                }

                // Apply state vector transformation
                for (const auto& [amb_key, amb_idx] : state_manager_.getAmbiguities()) {
                    const auto us = amb_key.find('_');
                    if (us == std::string::npos) continue;
                    const std::string amb_sat = amb_key.substr(0, us);
                    const std::string amb_sig = amb_key.substr(us + 1);
                    if (amb_sig != sig) continue;
                    if (PppUtils::getSystemString(amb_sat) != cons) continue;

                    const auto aidx = static_cast<Eigen::Index>(amb_idx);
                    const double old_val = x_mod(aidx);

                    if (amb_sat == pivot) {
                        x_mod(aidx) = 0.0;
                    } else {
                        x_mod(aidx) = old_val - n_new_ref;
                    }

                    log->debug("{}  Handover {}: {:.4f} -> {:.4f}",
                        epoch_prefix, amb_key, old_val, x_mod(aidx));
                    state_modified = true;
                }

                // Apply covariance transformation  P' = T P T'
                //
                // For non-pivot indices i:
                //   P'(i,:) = P(i,:) - P(ri,:)          (T row: 1 at i, -1 at ri)
                //   P'(:,i) = P(:,i) - P(:,ri)
                // For the new-ref index ri:
                //   P'(ri,:) = 0,  P'(:,ri) = 0,  P'(ri,ri) = 0  (frozen)
                //
                // We must apply row ops first (T*P), then column ops ((T*P)*T').
                // Do row ops for all group members except the pivot.
                for (const Eigen::Index gi : group_indices) {
                    if (gi == ri) continue;
                    P.row(gi) -= P.row(ri);
                }
                // Now column ops (on the already row-transformed P).
                for (const Eigen::Index gi : group_indices) {
                    if (gi == ri) continue;
                    P.col(gi) -= P.col(ri);
                }
                // Freeze the new-ref row/col to zero variance.
                P.row(ri).setZero();
                P.col(ri).setZero();

                cov_modified = true;
            }

            // --- Re-reference ionosphere states for this constellation ------
            // The SD ionosphere is  dI(i) = I(i) - I(pivot).  On handover the
            // new pivot's absolute iono value must be subtracted from every
            // satellite in the constellation, and the new pivot itself frozen
            // to 0.  The covariance is transformed with the same T P T' pattern
            // used for ambiguities.
            if (config_.isUncombinedEnabled()) {
                const int new_piv_iono_idx = state_manager_.getIonosphereIndex(pivot);
                if (new_piv_iono_idx >= 0) {
                    const auto pi = static_cast<Eigen::Index>(new_piv_iono_idx);
                    const double iono_piv = x_mod(pi);

                    // Collect all constellation iono indices (including the pivot).
                    std::vector<Eigen::Index> iono_group;
                    for (const auto& [sat, idx] : state_manager_.getIonosphereStates()) {
                        if (PppUtils::getSystemString(sat) != cons) continue;
                        iono_group.push_back(static_cast<Eigen::Index>(idx));
                    }

                    // State vector transformation.
                    for (const auto& [sat, idx] : state_manager_.getIonosphereStates()) {
                        if (PppUtils::getSystemString(sat) != cons) continue;
                        const auto ii = static_cast<Eigen::Index>(idx);
                        const double old_val = x_mod(ii);
                        if (sat == pivot) {
                            x_mod(ii) = 0.0;
                        } else {
                            x_mod(ii) = old_val - iono_piv;
                        }
                        log->debug("{}  Iono handover {}: {:.4f} -> {:.4f}",
                            epoch_prefix, sat, old_val, x_mod(ii));
                    }
                    state_modified = true;

                    // Covariance transformation  P' = T P T'.
                    for (const Eigen::Index gi : iono_group) {
                        if (gi == pi) continue;
                        P.row(gi) -= P.row(pi);
                    }
                    for (const Eigen::Index gi : iono_group) {
                        if (gi == pi) continue;
                        P.col(gi) -= P.col(pi);
                    }
                    P.row(pi).setZero();
                    P.col(pi).setZero();
                    cov_modified = true;
                }
            }

            // --- Re-reference per-satellite signal bias states ---------------
            // Under BSSD the per-satellite bias states for signals without BIA
            // coverage are SD quantities: b_sd(i) = b(i) - b(pivot).  On a
            // handover the new pivot's value must be subtracted from every
            // satellite in the constellation, and the new pivot frozen to 0.
            if (config_.estimateSatBiases() && config_.isUncombinedEnabled()) {
                // Group sat bias states by signal within this constellation.
                // For each signal, apply the same T P T' transformation used
                // for ionosphere (one pivot index per signal).
                std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> sig_groups;
                std::map<std::string, Eigen::Index> sig_pivot_idx; // signal -> pivot state index

                for (const auto& [sb_key, sb_idx] : state_manager_.getSatBiasStates()) {
                    const auto bar = sb_key.find('|');
                    if (bar == std::string::npos) continue;
                    const std::string sat = sb_key.substr(0, bar);
                    if (PppUtils::getSystemString(sat) != cons) continue;
                    const std::string sig = sb_key.substr(bar + 1);
                    const auto si = static_cast<Eigen::Index>(sb_idx);
                    sig_groups[sig].emplace_back(sat, si);
                    if (sat == pivot)
                        sig_pivot_idx[sig] = si;
                }

                for (const auto& [sig, members] : sig_groups) {
                    auto pi_it = sig_pivot_idx.find(sig);
                    if (pi_it == sig_pivot_idx.end()) continue;
                    const Eigen::Index pi = pi_it->second;
                    const double piv_val = x_mod(pi);

                    // State vector transformation.
                    for (const auto& [sat, si] : members) {
                        const double old_val = x_mod(si);
                        if (sat == pivot) {
                            x_mod(si) = 0.0;
                        } else {
                            x_mod(si) = old_val - piv_val;
                        }
                        log->debug("{}  SatBias handover {}: {:.4f} -> {:.4f}",
                            epoch_prefix, sat + "|" + sig, old_val, x_mod(si));
                    }
                    state_modified = true;

                    // Covariance transformation  P' = T P T'.
                    for (const auto& [sat, si] : members) {
                        if (si == pi) continue;
                        P.row(si) -= P.row(pi);
                    }
                    for (const auto& [sat, si] : members) {
                        if (si == pi) continue;
                        P.col(si) -= P.col(pi);
                    }
                    P.row(pi).setZero();
                    P.col(pi).setZero();
                    cov_modified = true;
                }
            }
        }

        if (state_modified)
            state_manager_.setStateVector(x_mod);
        if (cov_modified)
            state_manager_.setCovariance(P);

        bssd_pivot_sat_ = new_pivot_sat;
    }

    void MeasurementModel::freezePivotAmbiguityCovariance()
    {
        if (bssd_pivot_sat_.empty()) return;

        Eigen::MatrixXd Pf = state_manager_.getCovariance();
        bool any_frozen = false;
        for (const auto& [cons, piv] : bssd_pivot_sat_) {
            for (const auto& [amb_key, amb_idx] : state_manager_.getAmbiguities()) {
                const auto us = amb_key.find('_');
                if (us == std::string::npos) continue;
                if (amb_key.substr(0, us) != piv) continue;
                const std::string amb_sig = amb_key.substr(us + 1);
                if (amb_sig.empty() || amb_sig[0] != 'L') continue;
                const auto aidx = static_cast<Eigen::Index>(amb_idx);
                if (Pf(aidx, aidx) != 0.0) {
                    Pf.row(aidx).setZero();
                    Pf.col(aidx).setZero();
                    any_frozen = true;
                }
            }
        }
        if (any_frozen)
            state_manager_.setCovariance(Pf);
    }

    void MeasurementModel::freezePivotIonosphereCovariance()
    {
        if (bssd_pivot_sat_.empty()) return;
        if (!config_.isUncombinedEnabled()) return;

        // Inform StateManager which iono states are frozen so that
        // getProcessNoiseMatrix() can suppress their process noise.
        std::set<std::string> frozen_sats;
        for (const auto& [cons, piv] : bssd_pivot_sat_)
            frozen_sats.insert(piv);
        state_manager_.setFrozenIonosphereSatellites(frozen_sats);

        Eigen::MatrixXd Pf = state_manager_.getCovariance();
        bool any_frozen = false;
        for (const auto& [cons, piv] : bssd_pivot_sat_) {
            const int iono_idx = state_manager_.getIonosphereIndex(piv);
            if (iono_idx < 0) continue;
            const auto idx = static_cast<Eigen::Index>(iono_idx);
            if (Pf(idx, idx) != 0.0) {
                Pf.row(idx).setZero();
                Pf.col(idx).setZero();
                any_frozen = true;
            }

            // Freeze pivot satellite bias states (same reasoning: absorbed
            // into the eliminated clock, unobservable under BSSD).
            for (const auto& [sb_key, sb_raw_idx] : state_manager_.getSatBiasStates()) {
                if (sb_key.compare(0, piv.size() + 1, piv + "|") != 0) continue;
                const auto si = static_cast<Eigen::Index>(sb_raw_idx);
                if (si < Pf.rows() && Pf(si, si) != 0.0) {
                    Pf.row(si).setZero();
                    Pf.col(si).setZero();
                    any_frozen = true;
                }
            }
        }
        if (any_frozen)
            state_manager_.setCovariance(Pf);
    }

    /**
     * @brief Apply between-satellite single-difference (BSSD) S-transformation.
     *
     * Combined (IF) mode  — per Odijk et al. 2016, Khodabandeh & Teunissen 2015:
     *   Pivot selection: one pivot per (constellation, signal_type), highest elev.
     *   Columns zeroed:  receiver clock, phase clock, ISB.
     *
     * Uncombined PPP-RTK mode:
     *   Pivot selection: reads the sticky pivot from bssd_pivot_sat_ (set by
     *   handleBssdPivotHandover which ran before innovations were built).
     *   All signal types within a constellation share the same pivot.
     *   Columns zeroed:  receiver clock, phase clock, ISB, receiver DCB/IFB,
     *   and the reference satellite's ionosphere.
     *
     * In both modes reference ambiguity columns are zeroed (absorbed into
     * the eliminated clock), and the differencing operator D is applied to
     * z, H, and R.
     *
     * @param update         EpochMeasurement to transform (modified in place).
     * @param epoch_prefix   Log prefix string for diagnostic messages.
     */
    void MeasurementModel::applyBetweenSatelliteSingleDifference(
        EpochMeasurement& update,
        const std::string& epoch_prefix)
    {
        const Eigen::Index n = update.innovations.size();
        if (n < 2) return;

        const bool is_uncombined = config_.isUncombinedEnabled();

        // ---- Step 1: Classify rows -----------------------------------------
        struct RowInfo {
            Eigen::Index row;
            std::string satellite;
            std::string signal_type;
            std::string constellation;
            double elevation;
        };
        std::vector<RowInfo> rows(static_cast<size_t>(n));
        std::map<std::string, double> sat_elev;
        for (const auto& obs : update.observations)
            sat_elev[obs.satellite] = obs.elevation;

        for (Eigen::Index i = 0; i < n; ++i) {
            auto& ri = rows[static_cast<size_t>(i)];
            ri.row = i;
            ri.satellite   = update.satellite_ids[static_cast<size_t>(i)];
            ri.signal_type = update.signal_types[static_cast<size_t>(i)];
            ri.constellation = PppUtils::getSystemString(ri.satellite);
            auto it = sat_elev.find(ri.satellite);
            ri.elevation = (it != sat_elev.end()) ? it->second : 0.0;
        }

        // ---- Step 2: Pivot selection ---------------------------------------
        //
        // Combined (IF) mode:  one pivot per (constellation, signal_type).
        //
        // Uncombined PPP-RTK:  one pivot per constellation, determined by
        // handleBssdPivotHandover() which ran before innovations were built.
        // The tracked pivot is read from bssd_pivot_sat_ to guarantee
        // consistency between the state-vector corrections and the SD
        // differencing.
        //
        // pivot_map   maps  group_key -> undiff row index of the pivot.
        // group_key   is  "constellation|signal_type"  (so D has the right
        //             per-signal pivot row), but in uncombined mode all keys
        //             for a given constellation resolve to the same satellite.

        // Resolve the per-constellation pivot satellite.
        // Uncombined: use the tracked sticky pivot from bssd_pivot_sat_.
        // Combined:   use highest-elevation satellite per constellation
        //             (only used below to populate pivot_map).
        std::map<std::string, std::string> cons_pivot_sat;
        std::map<std::string, double>      cons_pivot_el;
        if (is_uncombined) {
            cons_pivot_sat = bssd_pivot_sat_;
            for (const auto& [cons, sat] : cons_pivot_sat) {
                auto it = sat_elev.find(sat);
                cons_pivot_el[cons] = (it != sat_elev.end()) ? it->second : 0.0;
            }
        } else {
            for (const auto& ri : rows) {
                auto it = cons_pivot_el.find(ri.constellation);
                if (it == cons_pivot_el.end() || ri.elevation > it->second) {
                    cons_pivot_sat[ri.constellation] = ri.satellite;
                    cons_pivot_el[ri.constellation]  = ri.elevation;
                }
            }
        }

        // Build the per-(constellation, signal_type) pivot_map.
        std::map<std::string, Eigen::Index> pivot_map;
        std::map<std::string, double>       pivot_el;

        if (is_uncombined) {
            // For each (constellation, signal_type) group, find the row that
            // belongs to the constellation's pivot satellite.  If the pivot
            // satellite has no row for a particular signal_type, that group
            // cannot be differenced (treated as singleton later).
            for (const auto& ri : rows) {
                const std::string gk = ri.constellation + "|" + ri.signal_type;
                if (ri.satellite == cons_pivot_sat[ri.constellation]) {
                    pivot_map[gk] = ri.row;
                    pivot_el[gk]  = ri.elevation;
                }
            }
        } else {
            // Combined mode: independent pivot per (constellation, signal_type).
            for (const auto& ri : rows) {
                const std::string gk = ri.constellation + "|" + ri.signal_type;
                auto it = pivot_el.find(gk);
                if (it == pivot_el.end() || ri.elevation > it->second) {
                    pivot_map[gk] = ri.row;
                    pivot_el[gk]  = ri.elevation;
                }
            }
        }

        // ---- Step 3: Partition into SD / singleton / pass-through ----------
        // A row is SD-differenced if its group has a pivot AND the group has
        // at least 2 members (including the pivot).  Pivot rows in multi-
        // member groups are consumed.
        std::map<std::string, int> grp_cnt;
        for (const auto& ri : rows) {
            const std::string gk = ri.constellation + "|" + ri.signal_type;
            if (pivot_map.count(gk))
                ++grp_cnt[gk];
        }

        struct SdEntry { Eigen::Index row; std::string gk; };
        std::vector<SdEntry>       sd_entries;
        std::vector<Eigen::Index>  pass_rows;  // singletons / no-pivot kept undifferenced

        for (const auto& ri : rows) {
            const std::string gk = ri.constellation + "|" + ri.signal_type;
            if (!pivot_map.count(gk) || grp_cnt[gk] <= 1) {
                pass_rows.push_back(ri.row);
            } else if (pivot_map[gk] != ri.row) {
                sd_entries.push_back({ri.row, gk});
            }
            // pivot rows in multi-member groups are consumed
        }

        const size_t n_sd   = sd_entries.size();
        const size_t n_pass = pass_rows.size();
        if (n_sd == 0) {
            log_manager_.getLogger("measurement_model")->debug(
                "{}BSSD: all groups are singletons", epoch_prefix);
            return;
        }

        // ---- Step 4: Build differencing operator D -------------------------
        // D is (n_sd + n_pass) x n.
        const Eigen::Index m = static_cast<Eigen::Index>(n_sd + n_pass);
        Eigen::MatrixXd D = Eigen::MatrixXd::Zero(m, n);

        Eigen::Index o = 0;
        for (size_t k = 0; k < n_sd; ++k, ++o) {
            D(o, sd_entries[k].row)            =  1.0;
            D(o, pivot_map[sd_entries[k].gk])  = -1.0;
        }
        for (size_t k = 0; k < n_pass; ++k, ++o) {
            D(o, pass_rows[k]) = 1.0;
        }

        // ---- Step 5: Transform z, H, R -------------------------------------
        Eigen::MatrixXd R_undiff = Eigen::MatrixXd::Zero(n, n);
        for (Eigen::Index i = 0; i < n; ++i)
            R_undiff(i, i) = update.R_v(i);

        Eigen::VectorXd new_z = D * update.innovations;
        Eigen::MatrixXd new_H = D * update.H;
        Eigen::MatrixXd new_R = D * R_undiff * D.transpose();

        // ---- Step 6: Zero columns that cancel after differencing -----------
        // After differencing within a constellation these are algebraically 0.
        // Explicit zeroing ensures no floating-point residue leaks through.
        const auto zero_col = [&](size_t idx) {
            if (idx < static_cast<size_t>(new_H.cols()))
                new_H.col(static_cast<Eigen::Index>(idx)).setZero();
        };

        // 6a. Receiver clock, phase clock, ISB — always cancel.
        zero_col(state_manager_.getClockIndex());
        if (state_manager_.hasPhaseClock())
            zero_col(state_manager_.getPhaseClockIndex());
        for (const auto& sys : state_manager_.getSystemsWithTimeOffsets())
            zero_col(state_manager_.getSystemTimeOffsetIndex(sys));

        // 6b. Receiver DCB / IFB columns — cancel after SD on the same band
        //     because both satellites share the same receiver hardware delay.
        for (const auto& [key, idx] : state_manager_.getCodeBiasStates())
            zero_col(idx);

        // 6c. Reference satellite ionosphere columns — in uncombined mode the
        //     per-satellite iono state is shared across all signals.  After SD
        //     with a single pivot, the pivot's iono is absorbed into the
        //     eliminated clock.  Zero those columns to freeze them.
        //     Same for the pivot's per-satellite signal bias states.
        std::set<Eigen::Index> ref_iono_cols;
        if (is_uncombined) {
            for (const auto& [cons, pivot_sat] : cons_pivot_sat) {
                const int iono_idx = state_manager_.getIonosphereIndex(pivot_sat);
                if (iono_idx >= 0)
                    ref_iono_cols.insert(static_cast<Eigen::Index>(iono_idx));

                // Zero pivot satellite bias columns (absorbed into eliminated clock)
                for (const auto& [sb_key, sb_idx] : state_manager_.getSatBiasStates()) {
                    if (sb_key.compare(0, pivot_sat.size() + 1, pivot_sat + "|") == 0)
                        zero_col(sb_idx);
                }
            }
            for (const Eigen::Index col : ref_iono_cols)
                new_H.col(col).setZero();
        }

        // ---- Step 7: Reparameterize phase ambiguities ----------------------
        // For each SD phase row the H-matrix currently has:
        //   H(amb_i) = +1,  H(amb_ref) = -1
        // The SD ambiguity is N_sd(i) = N(i) - N(ref).  We keep N(i) in the
        // state vector to represent N_sd(i) and zero the amb_ref column so the
        // reference ambiguity is frozen (absorbed into the eliminated clock).
        //
        // We collect all reference ambiguity indices so we can zero their
        // columns once (a reference may serve multiple SD rows of the same type).
        std::set<Eigen::Index> ref_amb_cols;
        for (size_t k = 0; k < n_sd; ++k) {
            const auto& entry = sd_entries[k];
            const Eigen::Index i_row = entry.row;
            const Eigen::Index r_row = pivot_map[entry.gk];
            const std::string& sig = update.signal_types[static_cast<size_t>(i_row)];
            if (sig.empty() || sig[0] != 'L') continue; // only phase

            const std::string ref_sat = update.satellite_ids[static_cast<size_t>(r_row)];
            const std::string ref_amb_key = ref_sat + "_" + sig;
            const int ref_amb_idx = state_manager_.getAmbiguityIndex(ref_amb_key);
            if (ref_amb_idx >= 0)
                ref_amb_cols.insert(static_cast<Eigen::Index>(ref_amb_idx));
        }
        for (const Eigen::Index col : ref_amb_cols)
            new_H.col(col).setZero();

        // ---- Step 8: Build metadata ----------------------------------------
        std::vector<std::string> new_sat_ids;
        std::vector<std::string> new_sig_types;
        new_sat_ids.reserve(static_cast<size_t>(m));
        new_sig_types.reserve(static_cast<size_t>(m));

        for (size_t k = 0; k < n_sd; ++k) {
            const Eigen::Index i = sd_entries[k].row;
            const Eigen::Index r = pivot_map[sd_entries[k].gk];
            new_sat_ids.push_back(
                update.satellite_ids[static_cast<size_t>(i)] + "-" +
                update.satellite_ids[static_cast<size_t>(r)]);
            new_sig_types.push_back(update.signal_types[static_cast<size_t>(i)]);
        }
        for (size_t k = 0; k < n_pass; ++k) {
            const Eigen::Index i = pass_rows[k];
            new_sat_ids.push_back(update.satellite_ids[static_cast<size_t>(i)]);
            new_sig_types.push_back(update.signal_types[static_cast<size_t>(i)]);
        }

        // ---- Step 9: Pivot summary -----------------------------------------
        std::string pivot_summary;
        if (is_uncombined) {
            for (const auto& [cons, sat] : cons_pivot_sat)
                pivot_summary += cons + "=" + sat + " ";
        } else {
            for (const auto& [gk, pr] : pivot_map) {
                if (grp_cnt[gk] > 1)
                    pivot_summary += gk + "=" +
                        update.satellite_ids[static_cast<size_t>(pr)] + " ";
            }
        }

        // ---- Step 10: Replace update contents ------------------------------
        update.innovations   = new_z;
        update.H             = new_H;
        update.R_full        = new_R;
        update.R_v           = new_R.diagonal();
        update.satellite_ids = std::move(new_sat_ids);
        update.signal_types  = std::move(new_sig_types);

        log_manager_.getLogger("measurement_model")->info(
            "{}BSSD: {} undiff -> {} SD + {} pass = {} total (pivots consumed). "
            "Zeroed {} ref-amb, {} ref-iono, {} code-bias cols. Pivots: {}",
            epoch_prefix, n, n_sd, n_pass, m,
            ref_amb_cols.size(), ref_iono_cols.size(),
            state_manager_.getCodeBiasStates().size(),
            pivot_summary);
    }

} // namespace GnssProcessor