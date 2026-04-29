/**
/**
 * @file MeasurementModel.cpp
 * @brief Implements measurement vector, design matrix and covariance assembly for PPP Kalman updates.
 *
 * The implementation converts a set of PPP observations and the current Kalman state
 * into an EpochMeasurement suitable for Kalman filter processing.
 *
 * Heavy sub-tasks are delegated to focused components:
 *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
 *  - BssdPivotManager:  pivot selection, handover, and state re-referencing.
 *  - BssdTransform:     between-satellite single-difference S-transformation.
 *
 * This file retains:
 *  - ensureRequiredStates()   � per-epoch state creation/pruning pre-pass.
 *  - isUsableMeasurement()    � per-measurement filtering predicate.
 *  - buildJacobianRow()       � single-row Jacobian and predicted-observable assembly.
 *  - computeMeasurementUpdate � top-level orchestrator.
 *  - validateMeasurementUpdate � post-assembly validation.
 */

#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "measurement/MeasurementModel.hpp"
#include "products/ProductCache.hpp"
#include "rinex/RinexParser.hpp"
#include "math/TimeUtils.hpp"
#include "common/SignalBand.hpp"
#include "measurement/BrsdTransform.hpp"
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

    } // anonymous namespace

    // ===================================================================
    // Construction
    // ===================================================================

    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), log_manager_(log_manager),
          state_initializer_(config, ppp_model, state_manager, log_manager),
          pivot_manager_(config, log_manager),
          bssd_transform_(config, state_manager, pivot_manager_, log_manager),
          dd_pivot_manager_(config, log_manager),
          ppp_utils_(log_manager) {
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(), modeLabel(config_));
    }

    // ===================================================================
    // IMU state setter
    // ===================================================================

    void MeasurementModel::setImuState(const Eigen::Matrix3d& C_b_e,
                                        const Eigen::Vector3d& f_b_corrected) {
        tc_C_b_e_ = C_b_e;
        tc_f_b_ = f_b_corrected;
    }

    // ===================================================================
    // Product coverage checks
    // ===================================================================

    bool MeasurementModel::ifCombHasFullBia(const std::string& satellite,
                                             const std::string& if_key) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true; // not an IF key � assume covered
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

                // When BSSD is active, skip signals on frequency bands
                // not covered by the pivot — they cannot form SDs and
                // their states would be unobservable.
                if (!bssd_pivot_bands_.empty() && ref_type.size() >= 2
                    && (ref_type[0] == 'C' || ref_type[0] == 'L')) {
                    const std::string cons = PppUtils::getSystemString(obs.satellite);
                    auto cit = bssd_pivot_bands_.find(cons);
                    if (cit != bssd_pivot_bands_.end()
                        && !cit->second.count(ref_type.substr(0, 2)))
                        continue;
                }

                if (is_uncombined && (ref_type[0] == 'C' || ref_type[0] == 'L'))
                    required_iono_sats.insert(obs.satellite);

                if (ref_type[0] != 'L') continue;

                const bool is_iono_free = (ref_type.find('_') != std::string::npos);
                if (!is_uncombined && !is_iono_free) continue;

                required_ambiguities.insert(obs.satellite + "_" + ref_type);
            }
        }

        // --- Ionosphere states (create and seed BEFORE ambiguities) ---
        if (is_uncombined) {
            for (const auto& obs : observations) {
                if (!required_iono_sats.count(obs.satellite)) continue;
                if (state_manager_.getIonosphereIndex(obs.satellite) < 0) {
                    if (obs.elevation > 0.0)
                        state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);

                    const double iono_seed = state_initializer_.computeIonosphereSeed(obs);
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

        // --- Ambiguity states ---
        const bool bssd_active = config_.enableBetweenSatelliteSd();

        // Pivot absolute seeds, keyed by constellation + frequency band
        // (e.g. "GPS|L1") rather than the full signal type.  Different
        // tracking attributes on the same band (L1W vs L1C) share the same
        // frequency; using a band-normalized key ensures a non-pivot
        // satellite that fell back to a different tracking mode still
        // subtracts the correct pivot seed for SD ambiguity initialization.
        std::map<std::string, double> pivot_abs_seeds;
        if (bssd_active) {
            const Eigen::VectorXd x_seed = state_manager_.getStateVector();
            const auto& pivot_sats = pivot_manager_.getPivotSatellites();
            for (const auto& obs : observations) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                auto piv_it = pivot_sats.find(cons);
                if (piv_it == pivot_sats.end() || piv_it->second != obs.satellite)
                    continue;
                for (const auto& [ref_type, val] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    const std::string band = (ref_type.size() >= 2)
                                           ? ref_type.substr(0, 2) : ref_type;
                    const std::string pkey = cons + "|" + band;
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

                bool needs_seed = (existing_idx < 0)
                               || state_manager_.isAmbiguityPendingReseed(amb_key);

                if (needs_seed) {
                    const Eigen::VectorXd x_now = state_manager_.getStateVector();
                    double seed = state_manager_.computeAmbiguitySeed(obs, ref_type, x_now);

                    if (bssd_active) {
                        const std::string cons = PppUtils::getSystemString(obs.satellite);
                        const auto& pivot_sats = pivot_manager_.getPivotSatellites();
                        auto piv_it = pivot_sats.find(cons);
                        const bool is_pivot = piv_it != pivot_sats.end()
                                           && piv_it->second == obs.satellite;
                        if (is_pivot) {
                            seed = 0.0;
                        } else {
                            const std::string band = (ref_type.size() >= 2)
                                                   ? ref_type.substr(0, 2) : ref_type;
                            const std::string pkey = cons + "|" + band;
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
                    state_manager_.clearPendingReseed(amb_key);
                }
            }
        }

        state_manager_.pruneAmbiguitiesExcept(required_ambiguities);

        // --- Mark unresolvable ambiguities ---
        // Ambiguities without full BIA phase bias coverage cannot be resolved
        // to their correct integer.  Mark them as unresolvable so they are
        // excluded from the LAMBDA search.  The float ambiguity still absorbs
        // the time-constant satellite phase bias with zero process noise.
        state_manager_.clearUnresolvableAmbiguities();
        if (!is_uncombined) {
            // Combined (IF) mode: check both component signals of the IF label.
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    if (ref_type.find('_') == std::string::npos) continue;
                    const std::string amb_key = obs.satellite + "_" + ref_type;
                    if (!required_ambiguities.count(amb_key)) continue;
                    const bool missing_bia = !ifCombHasFullBia(obs.satellite, ref_type);
                    const bool missing_atx = !ifCombHasFullRxAtx(ref_type, obs.system);
                    if (missing_bia || missing_atx) {
                        state_manager_.markAmbiguityUnresolvable(amb_key);
                        log_manager_.getLogger("measurement_model")->trace(
                            "{}Unresolvable ambiguity {}: missing_bia={}, missing_atx={}",
                            epoch_prefix, amb_key, missing_bia, missing_atx);
                    }
                }
            }
        } else {
            // Uncombined (PPP-RTK) mode: check the individual phase signal.
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    if (ref_type.find('_') != std::string::npos) continue;
                    const std::string amb_key = obs.satellite + "_" + ref_type;
                    if (!required_ambiguities.count(amb_key)) continue;
                    const bool missing_bia = !ppp_model_.hasBiasEntry(obs.satellite, ref_type);
                    if (missing_bia) {
                        state_manager_.markAmbiguityUnresolvable(amb_key);
                        log_manager_.getLogger("measurement_model")->trace(
                            "{}Unresolvable ambiguity {}: missing phase BIA",
                            epoch_prefix, amb_key);
                    }
                }
            }
        }

        // --- Receiver bias states ---
        std::set<std::string> required_code_biases;
        if (config_.estimateIfb() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
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

        // --- Per-satellite signal bias states (code-only in uncombined) ---
        // Phase ambiguities absorb satellite phase biases, so only code
        // measurements without BIA coverage need a per-satellite bias state.
        std::set<std::string> required_sat_biases;
        if (config_.estimateSatBiases() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type.find('_') != std::string::npos) continue;
                    if (ref_type[0] != 'C') continue;  // code-only
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

        // Sat-bias states are NOT pruned each epoch.  They represent
        // time-constant satellite hardware delays that must survive
        // temporary visibility gaps (e.g. satellite dips below the
        // elevation mask for a few epochs).  The set is bounded by
        // the number of satellite+signal combinations lacking BIA
        // coverage.  Stale states for decommissioned satellites are
        // harmless — the process noise keeps covariance bounded.
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool MeasurementModel::isUsableMeasurement(const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) const {
        if (ref_type.empty()) return false;

        if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()) {
            return false;
        }

        // When BSSD is active, exclude code/phase measurements on frequency
        // bands not covered by the pivot satellite.  Such measurements
        // cannot be differenced and would pass through the BssdTransform
        // as undifferenced singletons with zeroed clock/ISB columns.
        // Doppler measurements are exempt — they are not SD-transformed.
        if (!bssd_pivot_bands_.empty() && ref_type.size() >= 2
            && (ref_type[0] == 'C' || ref_type[0] == 'L')) {
            const std::string cons = PppUtils::getSystemString(obs.satellite);
            auto cit = bssd_pivot_bands_.find(cons);
            if (cit != bssd_pivot_bands_.end()) {
                const std::string band = ref_type.substr(0, 2);
                if (!cit->second.count(band)) {
                    return false;
                }
            }
        }

        const MeasurementType mt = classifyMeasurement(ref_type);

        if (mt != MeasurementType::Code && mt != MeasurementType::Phase &&
            !(mt == MeasurementType::Doppler && config_.estimateVelocity()))
            return false;

        const bool is_uncombined = config_.isUncombinedEnabled();
        const bool has_underscore = (ref_type.find('_') != std::string::npos);

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

        // Doppler measurements
        if (is_doppler) {
            if (!state_manager_.hasVelocity()) return false;

            jacobian.segment<3>(state_manager_.getVelocityIndex()) = -obs.los;

            const Eigen::Vector3d rx_vel = x.segment<3>(state_manager_.getVelocityIndex());
            predicted = obs.modeled.at(ref_type) - obs.los.dot(rx_vel);

            if (config_.tightlyCoupledImu() && state_manager_.hasImuBiases()) {
                const Eigen::Vector3d f_e = tc_C_b_e_ * tc_f_b_;
                Eigen::Matrix3d skew_fe;
                skew_fe <<       0.0, -f_e(2),  f_e(1),
                            f_e(2),      0.0, -f_e(0),
                           -f_e(1),  f_e(0),      0.0;

                const Eigen::Vector3d att_partial = -skew_fe.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAttitudeIndex()) = att_partial;

                const Eigen::Vector3d ba_partial = -tc_C_b_e_.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAccelBiasIndex()) = ba_partial;
            }

            return true;
        }

        // Position partial: -LOS
        jacobian.segment<3>(state_manager_.getPositionIndex()) = -obs.los;

        // Troposphere wet delay
        jacobian(state_manager_.getTropoIndex()) = mapping_wet;

        // Receiver clock
        jacobian(state_manager_.getClockIndex()) = 1.0;
        const double rx_clk = x(state_manager_.getClockIndex());

        // Phase clock offset
        double phase_clk_value = 0.0;
        if (is_phase && state_manager_.hasPhaseClock()) {
            jacobian(state_manager_.getPhaseClockIndex()) = 1.0;
            phase_clk_value = x(state_manager_.getPhaseClockIndex());
        }

        // Tropospheric gradients
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
        const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
        double time_offset_value = 0.0;
        if (!sys_name.empty()) {
            const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
            if (isb_idx < state_size) {
                jacobian(isb_idx) = 1.0;
                time_offset_value = x(isb_idx);
            }
        }

        // Per-satellite ionosphere
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

        // Receiver bias � DCB / IFB (code only, uncombined)
        double code_bias_term = 0.0;
        if (config_.estimateIfb() && is_uncombined && !is_phase) {
            const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
                const int cb_idx = state_manager_.getCodeBiasIndex(bias.state_key);
                if (cb_idx >= 0 && cb_idx < static_cast<int>(state_size)) {
                    jacobian(cb_idx) = bias.coefficient;
                    code_bias_term = bias.coefficient * x(cb_idx);
                }
            }
        }

        // Per-satellite signal bias (code-only: phase ambiguity absorbs sat phase bias)
        double sat_bias_term = 0.0;
        if (config_.estimateSatBiases()) {
            const bool is_if = (ref_type.find('_') != std::string::npos);
            const bool applicable = is_uncombined
                ? (!is_if && !is_phase)
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

        return true;
    }

    // ===================================================================
    // computeMeasurementUpdate � top-level orchestrator
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

        // --- First-epoch initialization (delegated to StateInitializer) ---
        if (!state_initializer_.isIsbInitialized()) {
            state_initializer_.initializeInterSystemBiases(observations, epoch_prefix);
            state_initializer_.markIsbInitialized();
        }
        if (!state_initializer_.isPhaseClockInitialized() && config_.estimatePhaseClock()) {
            state_initializer_.initializePhaseClockOffset(observations, epoch_prefix);
            state_initializer_.markPhaseClockInitialized();
        }

        // --- BSSD pivot handover ---
        // Run only on the first pass of each epoch.  During measurement
        // rebuilds (outlier/ambiguity-reset passes within the same epoch)
        // the pivot must remain fixed so that SD ambiguities stay
        // consistent.  Re-running handover after an outlier handler resets
        // a pivot's ambiguity would detect it as "disturbed", hand over to
        // a different satellite, and invalidate all SD ambiguities for that
        // constellation — producing the cascading large-innovation pattern.
        if (config_.enableBetweenSatelliteSd() && !(update.time == last_pivot_epoch_)) {
            handleBssdPivotHandover(observations, epoch_prefix);
            last_pivot_epoch_ = update.time;
        }

        // --- Build BSSD pivot band coverage map ---
        // When BSSD is active, only measurements on frequency bands
        // covered by the pivot satellite may enter the update.
        // Measurements on uncovered bands would become undifferenced
        // singletons in the BssdTransform with zeroed clock/ISB columns.
        // Built before ensureRequiredStates so that ambiguity states for
        // uncovered bands are not created or retained.
        bssd_pivot_bands_.clear();
        if (config_.enableBetweenSatelliteSd()) {
            const auto& pivot_sats = pivot_manager_.getPivotSatellites();
            for (const auto& obs : observations) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                auto piv_it = pivot_sats.find(cons);
                if (piv_it == pivot_sats.end() || piv_it->second != obs.satellite)
                    continue;
                auto& bands = bssd_pivot_bands_[cons];
                for (const auto& [sig, val] : obs.observed) {
                    if (sig.size() >= 2)
                        bands.insert(sig.substr(0, 2));
                }
            }
        }

        // --- State pre-pass ---
        ensureRequiredStates(observations, epoch_prefix);

        // --- First-epoch code bias seeding ---
        if (!state_initializer_.isCodeBiasInitialized() && config_.estimateIfb()) {
            state_initializer_.initializeCodeBiases(observations, epoch_prefix);
            state_initializer_.markCodeBiasInitialized();
        }

        // --- Freeze pivot covariances ---
        if (config_.enableBetweenSatelliteSd()) {
            freezePivotAmbiguityCovariance();
            freezePivotIonoAndBiasCovariance();
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

                    // Downweight code on signals without BIA coverage
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

        // --- BSSD S-transformation ---
        if (config_.enableBetweenSatelliteSd()) {
            bssd_transform_.apply(update, epoch_prefix);
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
    // BSSD delegation
    // ===================================================================

    void MeasurementModel::handleBssdPivotHandover(
        const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix)
    {
        pivot_manager_.handlePivotHandover(observations, state_manager_, epoch_prefix);
    }

    // -------------------------------------------------------------------
    // DD pivot handover (S-transform) moved to DdPivotManager.
    // The legacy MeasurementModel::handleBaselinePivotHandover method
    // is gone; @c dd_pivot_manager_.selectPivots applies the handover
    // internally when the chosen pivot differs from the sticky one.
    // -------------------------------------------------------------------

    void MeasurementModel::freezePivotAmbiguityCovariance()
    {
        pivot_manager_.freezePivotAmbiguityCovariance(state_manager_);
    }

    void MeasurementModel::freezePivotIonoAndBiasCovariance()
    {
        pivot_manager_.freezePivotIonoAndBiasCovariance(state_manager_);
    }

    // =================================================================
    // Baseline (DD) measurement update — Slice 5e+
    //
    // Unified path: DD rows are built from two per-receiver PppModel
    // outputs rather than a separate geometry layer.  The per-receiver
    // residuals (observed - modeled) already incorporate Sagnac, emission
    // time, satellite clock, Shapiro, antenna PCO/PCV, solid tide, and
    // stochastic-model variances — DD cancels the parts that are common
    // between receivers (sat clock on short baselines, most propagation
    // terms) and leaves the position-dependent geometry + ambiguity
    // signal the filter needs.
    // =================================================================

    namespace {

        /// Build the DD ambiguity key: "DD|cons|sat_i|code|pivot".
        std::string makeDdAmbKey(const std::string& cons,
                                 const std::string& sat_i,
                                 const std::string& code,
                                 const std::string& pivot) {
            return "DD|" + cons + "|" + sat_i + "|" + code + "|" + pivot;
        }

        /// Map first char of a satellite id to constellation name.
        std::string ddConstellationOf(const std::string& sat) {
            if (sat.empty()) return {};
            switch (sat[0]) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return std::string(1, sat[0]);
            }
        }

        /// Constellation string -> System enum.  Used by IF row collapse
        /// to look up frequencies via PppUtils::getFrequency().  Returns
        /// System::Unknown for unrecognised names so the caller can skip.
        System ddSystemFromConsName(const std::string& cons) {
            if (cons == "GPS")     return System::GPS;
            if (cons == "Galileo") return System::Galileo;
            if (cons == "GLONASS") return System::GLONASS;
            if (cons == "BeiDou")  return System::BeiDou;
            if (cons == "QZSS")    return System::QZSS;
            if (cons == "SBAS")    return System::SBAS;
            return System::Unknown;
        }

    } // anonymous namespace

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {

        EpochMeasurement em;
        // Seed the output exclusion set from the caller's carry set so
        // the KF outlier handlers (which use insert(), not assign) build
        // a cumulative union across rebuild passes.  Without this, the
        // caller's `carry_excluded = em.excluded_measurements` after the
        // KF returns would replace the previous pass's exclusions with
        // only this pass's additions, and a satellite with two bad
        // signals would oscillate (pass N excludes C1C; pass N+1 starts
        // fresh, excludes C2W, forgets C1C; pass N+2 sees C1C again).
        // Mirrors the PPP overload's behavior at line 595.
        em.excluded_measurements = excluded_measurements;

        auto klog = log_manager_.getLogger("measurement_model");
        // First-epoch diagnostic: reveal whether shared sats / signals exist.
        // Off after first call via a static flag so we don't spam the log.
        static bool first_dd_call = true;
        const bool diag = first_dd_call;
        if (diag) {
            klog->info("DD first-epoch diagnostic: rov_modeled={} sats, ref_modeled={} sats",
                       rov_modeled.size(), ref_modeled.size());
            if (!rov_modeled.empty()) {
                const auto& r0 = rov_modeled.front();
                std::string codes;
                for (const auto& [code, v] : r0.observed) { codes += code; codes += ' '; }
                klog->info("DD diag: rov[0]={} observed_codes=[{}]", r0.satellite, codes);
            }
            if (!ref_modeled.empty()) {
                const auto& e0 = ref_modeled.front();
                std::string codes;
                for (const auto& [code, v] : e0.observed) { codes += code; codes += ' '; }
                klog->info("DD diag: ref[0]={} observed_codes=[{}]", e0.satellite, codes);
            }
        }
        if (rov_modeled.empty() || ref_modeled.empty()) { first_dd_call = false; return em; }
        em.time = rov_modeled.front().time;

        const bool code_only = config_.getBaselineConfig().code_only;
        const double amb_init_std = config_.getInitialStd().ambiguity;
        const double amb_init_var =
            (amb_init_std > 0.0 ? amb_init_std * amb_init_std : 1e8);

        // Active-system + excluded-sat filters.
        const auto& active_vec = config_.getActiveSystems();
        const std::set<std::string> active_set(active_vec.begin(), active_vec.end());

        // Index by satellite.
        std::map<std::string, const PppObservation*> rov_by_sat, ref_by_sat;
        for (const auto& o : rov_modeled) rov_by_sat[o.satellite] = &o;
        for (const auto& o : ref_modeled) ref_by_sat[o.satellite] = &o;

        // Aliasing policy: when true, codes that share the same (System, Band)
        // but differ in tracking attribute (e.g. rov C2W vs ref C2C) are
        // paired across receivers.  When false, only exact-string code
        // matches are kept (legacy strict behavior).  Off-band cancellation
        // properties of DD do NOT depend on identical attributes — a small
        // sat-DCB residual remains on code DDs (absorbed by the ambiguity
        // float on phase DDs in non-code-only mode).  See Phase 2 plan for
        // DCB harmonization.
        const bool signal_aliasing = config_.getBaselineConfig().signal_aliasing;

        // Per-band canonical code chosen at one receiver: pick the
        // tracking attribute with the lowest @c signalAttributePriority().
        // Returns map keyed by (band, obs_type) → (rinex_code, priority).
        auto pickCanonical = [](const PppObservation& obs, const std::string& cons) {
            std::map<std::pair<FrequencyBand, char>,
                     std::pair<std::string, int>> best;
            for (const auto& [code, val] : obs.observed) {
                if (code.size() < 3) continue;
                const auto cls = classifySignal(cons, code);
                if (cls.band == FrequencyBand::Unknown) continue;
                if (cls.type != ObsType::Code && cls.type != ObsType::Phase) continue;
                const auto key = std::make_pair(cls.band, static_cast<char>(cls.type));
                const int prio = signalAttributePriority(cons, cls.band, cls.attribute);
                auto it = best.find(key);
                if (it == best.end() || prio < it->second.second) {
                    best[key] = { code, prio };
                }
            }
            return best;
        };

        // Group shared sats by constellation.  For each (sat, band, obs_type)
        // tuple shared between the two receivers, run the BRSD transform
        // (BrsdTransform::apply) to compute the between-receiver
        // residual + variance + rover-side LOS once.  Downstream BSSD
        // pivot-differencing then forms DD rows from BRSD entries.
        //
        // BRSD output replaces what used to be a thin SharedSignal
        // metadata struct: BrsdObservation already carries the same
        // (band, obs_type, rov_code, ref_code, brsd_aliased) metadata
        // plus the freshly-computed residual / variance / LOS, so the
        // canonical-code selection and BRSD math live in a single pass.
        struct SharedInfo {
            const PppObservation* rov{ nullptr };
            const PppObservation* ref{ nullptr };
            std::vector<BrsdObservation> brsd_signals;  ///< One BRSD per shared (band, obs_type).
            double avg_snr{ 0.0 };
            size_t num_sigs{ 0 };
        };
        std::map<std::string, std::map<std::string, SharedInfo>> by_cons;

        for (const auto& [sat, rov_ptr] : rov_by_sat) {
            const std::string cons = ddConstellationOf(sat);
            if (!active_set.empty() && active_set.find(cons) == active_set.end()) continue;
            if (config_.isSatelliteExcluded(sat)) continue;

            auto ref_it = ref_by_sat.find(sat);
            if (ref_it == ref_by_sat.end()) continue;
            const PppObservation* ref_ptr = ref_it->second;

            SharedInfo info;
            info.rov = rov_ptr;
            info.ref = ref_ptr;

            const auto rov_canon = pickCanonical(*rov_ptr, cons);
            const auto ref_canon = pickCanonical(*ref_ptr, cons);
            for (const auto& [k, rov_pair] : rov_canon) {
                auto rit = ref_canon.find(k);
                if (rit == ref_canon.end()) continue;
                const std::string& rov_code = rov_pair.first;
                const std::string& ref_code = rit->second.first;
                const bool aliased = (rov_code != ref_code);
                if (aliased && !signal_aliasing) continue;  // strict mode drops mismatches

                auto brsd = BrsdTransform::apply(*rov_ptr, *ref_ptr, cons,
                                                 rov_code, ref_code,
                                                 k.first, k.second);
                if (!brsd) continue;  // missing observed/modeled or NaN
                info.brsd_signals.push_back(std::move(*brsd));
            }

            // SNR averaging — any 'S*' RINEX code on the rover side.
            double snr_sum = 0.0; size_t snr_cnt = 0;
            for (const auto& [code, val] : rov_ptr->observed) {
                if (!code.empty() && code[0] == 'S') { snr_sum += val; ++snr_cnt; }
            }
            info.num_sigs = info.brsd_signals.size();
            info.avg_snr  = (snr_cnt > 0) ? (snr_sum / snr_cnt) : 0.0;
            if (info.num_sigs < 2) continue;
            by_cons[cons][sat] = std::move(info);
        }
        if (diag) {
            size_t total_shared = 0;
            size_t total_aliased = 0;
            for (const auto& [cons, sats] : by_cons) {
                klog->info("DD diag: constellation '{}' has {} shared sats with ≥2 signals", cons, sats.size());
                total_shared += sats.size();
                for (const auto& [sat, info] : sats) {
                    std::string codes;
                    for (const auto& b : info.brsd_signals) {
                        codes += b.rov_code;
                        if (b.brsd_aliased) {
                            codes += "->"; codes += b.ref_code; ++total_aliased;
                        }
                        codes += '(';
                        codes += bandLabel(b.band);
                        codes += ") ";
                    }
                    klog->info("DD diag:   {}: {} BRSD signals=[{}]", sat,
                               info.brsd_signals.size(), codes);
                }
            }
            klog->info("DD diag: {} total viable shared sats; signal_aliasing={}, aliased_signals={}",
                       total_shared, signal_aliasing, total_aliased);
        }
        if (by_cons.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — by_cons empty");
            first_dd_call = false;
            return em;
        }

        // --- Sticky pivot selection per constellation ---
        // Build the candidate map (cons → sat → ranking-info) from the
        // BRSD-populated by_cons structure and hand it to DdPivotManager.
        // The manager's selectPivots applies the sticky-or-rerank logic
        // and, on a pivot change, the S-transform handover (gated by
        // BaselineConfig::enable_pivot_handover) — preserving DD
        // ambiguity integer cycles across handovers.
        DdPivotManager::CandidateMap pivot_candidates;
        for (const auto& [cons, sats] : by_cons) {
            for (const auto& [sat, info] : sats) {
                pivot_candidates[cons][sat] = { info.avg_snr, info.num_sigs };
            }
        }
        const std::map<std::string, std::string> pivots =
            dd_pivot_manager_.selectPivots(pivot_candidates, state_manager_);

        if (diag) {
            for (const auto& [c, p] : pivots) {
                klog->info("DD diag: pivot for '{}' = '{}'", c, p);
            }
        }
        if (pivots.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — pivots empty");
            first_dd_call = false;
            return em;
        }

        // --- Collect DD rows ---
        // Each row carries both rover and reference RINEX codes.  When
        // aliasing is off they are equal (legacy behavior).  When on and
        // the receivers report different tracking attributes on the same
        // band, the row is built cross-attribute — residuals and variances
        // are looked up under each receiver's own code.
        //
        // @c integer_capable is true iff all four observations in the DD
        // (rov_i, rov_p, ref_i, ref_p) use the same RINEX code.  Only
        // those rows have receiver-bias and satellite-bias terms that
        // cancel pairwise; the float ambiguity is then a clean integer
        // multiple of the carrier wavelength and is eligible for AR.
        // Any other combination (rov/ref mismatch on this row, OR rover
        // code differs between sat_i and pivot, OR reference code does)
        // carries a non-integer DCB / IFB residual.
        // The DD row carries the BRSD-combined variance for the non-pivot
        // (var_brsd_i = var_rov_i + var_ref_i) and pivot (var_brsd_p =
        // var_rov_p + var_ref_p).  Diagonal R = var_brsd_i + var_brsd_p;
        // off-diagonal R between rows sharing a pivot = var_brsd_p alone.
        // Splitting along the BRSD seam (rather than four var_*_*
        // components) makes the variance composition trace cleanly back
        // to the BrsdObservation layer.
        struct DdRow {
            char obs_type{ 'C' };
            std::string sat_i, sat_p, cons;
            std::string rov_code, ref_code;
            FrequencyBand band{ FrequencyBand::Unknown };
            bool integer_capable{ true };
            Eigen::RowVector3d h_pos;
            double innov_pre{ 0.0 };
            double var_brsd_i{ 0.0 };  ///< BRSD variance on the non-pivot side (var_rov + var_ref).
            double var_brsd_p{ 0.0 };  ///< BRSD variance on the pivot side (var_rov + var_ref).
            std::string group_key;
            int amb_idx{ -1 };
            double innov{ 0.0 };
        };
        std::vector<DdRow> rows;

        // ---- BSSD step: BRSD entries → DD rows ----
        // Hand the BRSD inputs and pivot map to BssdTransform::differenceBrsd,
        // which performs the BSSD differencing per (cons, band, obs_type)
        // group and returns BssdDdRow entries (residual, variance pair,
        // rover-side LOS pair, integer-capability flag, metadata).
        // MeasurementModel then layers on its private bookkeeping —
        // group_key for R off-diagonal correlation and the H_pos partial
        // derived from the LOS pair — to produce a DdRow.
        //
        // The differencing math now lives entirely inside BssdTransform;
        // the same class also serves PPP-RTK SD via apply().  See the
        // class header for the architectural notes on the two methods.
        {
            // Build a (cons, sat) → vector<BrsdObservation> view over
            // by_cons.  The static differenceBrsd signature wants this
            // shape so the structural dependency on MeasurementModel's
            // local SharedInfo struct stays decoupled.
            std::map<std::string,
                     std::map<std::string, std::vector<BrsdObservation>>> brsd_by_cons_sat;
            for (const auto& [cons, sats] : by_cons) {
                for (const auto& [sat, info] : sats) {
                    brsd_by_cons_sat[cons][sat] = info.brsd_signals;
                }
            }

            const auto bssd_rows = BssdTransform::differenceBrsd(
                brsd_by_cons_sat, pivots, excluded_measurements);

            rows.reserve(bssd_rows.size());
            for (const auto& br : bssd_rows) {
                DdRow r;
                r.obs_type        = br.obs_type;
                r.sat_i           = br.sat_i;
                r.sat_p           = br.sat_p;
                r.cons            = br.cons;
                r.rov_code        = br.rov_code;
                r.ref_code        = br.ref_code;
                r.band            = br.band;
                r.integer_capable = br.integer_capable;
                r.innov_pre       = br.innov_pre;
                r.var_brsd_i      = br.var_brsd_i;
                r.var_brsd_p      = br.var_brsd_p;
                // Geometry partial w.r.t. the rover position state.
                // Reference LOS doesn't appear because the reference is
                // not a state in baseline mode (its contribution already
                // cancelled in the BRSD layer feeding differenceBrsd).
                r.h_pos = -(br.los_rov_i - br.los_rov_p).transpose();
                // Group key for R off-diagonal correlation: same pivot
                // variance contribution is shared by all non-pivot rows
                // on the same (cons, band, obs_type).
                r.group_key = br.cons + "|" + bandLabel(br.band)
                            + "|" + std::string(1, br.obs_type);
                rows.push_back(std::move(r));
            }
        }

        // --- Iono-free row collapse (long-baseline / educational mode) ---
        // When @c BaselineConfig::use_iono_free is on, replace the per-band
        // DD rows for each (sat_i, pivot, obs_type) group with a single
        // row formed from the dual-frequency ionosphere-free combination
        //
        //     y_IF = α·y_1 + β·y_2,   α = f1²/(f1²-f2²),  β = -f2²/(f1²-f2²)
        //
        // where (f1, y_1) is the higher-frequency band and (f2, y_2) is
        // the lower.  This eliminates the first-order ionospheric delay
        // — required for long baselines (>10-20 km) where the DD does
        // NOT cancel iono between receivers.  Cost: ~3x amplified code
        // noise on GPS L1+L2 (the variance scales by α²+β² ≈ 9.5), and
        // the ambiguity is no longer integer-cycle (it's a non-trivial
        // linear combination of N1 and N2 in cycles), so AR is excluded
        // for IF phase rows via @c integer_capable=false.  Sat-pairs
        // with fewer than two shared bands are dropped — IF needs two.
        // The H_pos block is unchanged: the geometry partial is the
        // same linear combination's derivative w.r.t. position, which
        // factors out of α and β (both scaled the same).
        if (config_.getBaselineConfig().use_iono_free && !rows.empty()) {
            // Group row indices by (cons, sat_i, sat_p, obs_type).
            std::map<std::string, std::vector<size_t>> if_groups;
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                const std::string gk = r.cons + "|" + r.sat_i + "|" + r.sat_p
                                     + "|" + std::string(1, r.obs_type);
                if_groups[gk].push_back(i);
            }

            std::vector<DdRow> if_rows;
            if_rows.reserve(if_groups.size());
            size_t n_dropped_single_band = 0;

            for (const auto& [gk, idxs] : if_groups) {
                if (idxs.size() < 2) {
                    // Need two bands to cancel ionosphere; skip.
                    ++n_dropped_single_band;
                    continue;
                }
                // Pick the two highest frequencies among the available
                // bands.  Standard choice for GPS is L1+L2; for Galileo
                // E1+E5a; for BeiDou B1+B3; etc.  Picking by frequency
                // (rather than band priority) keeps the formula's α/β
                // numerically well-conditioned.
                std::vector<size_t> sorted_idxs = idxs;
                const System sys = ddSystemFromConsName(rows[sorted_idxs.front()].cons);
                std::sort(sorted_idxs.begin(), sorted_idxs.end(),
                    [&](size_t a, size_t b) {
                        const double fa = ppp_utils_.getFrequency(
                            rows[a].rov_code, sys, rows[a].sat_i);
                        const double fb = ppp_utils_.getFrequency(
                            rows[b].rov_code, sys, rows[b].sat_i);
                        return fa > fb;
                    });

                const DdRow& r1 = rows[sorted_idxs[0]];  // higher f
                const DdRow& r2 = rows[sorted_idxs[1]];  // lower f

                const double f1 = ppp_utils_.getFrequency(r1.rov_code, sys, r1.sat_i);
                const double f2 = ppp_utils_.getFrequency(r2.rov_code, sys, r2.sat_i);
                if (!(std::isfinite(f1) && std::isfinite(f2))
                 || f1 <= 0.0 || f2 <= 0.0 || f1 == f2) {
                    ++n_dropped_single_band;
                    continue;
                }
                const double f1s   = f1 * f1;
                const double f2s   = f2 * f2;
                const double denom = f1s - f2s;
                const double alpha =  f1s / denom;
                const double beta  = -f2s / denom;
                const double a2 = alpha * alpha;
                const double b2 = beta  * beta;

                DdRow ifr;
                ifr.obs_type = r1.obs_type;
                ifr.sat_i    = r1.sat_i;
                ifr.sat_p    = r1.sat_p;
                ifr.cons     = r1.cons;
                ifr.band     = r1.band;            // documentation only
                // Concatenated codes serve as both the ambiguity-key code
                // segment and the trace signal_types token.
                ifr.rov_code = r1.rov_code + "+" + r2.rov_code;
                ifr.ref_code = r1.ref_code + "+" + r2.ref_code;
                ifr.h_pos    = r1.h_pos;           // identical for both bands
                ifr.innov_pre = alpha * r1.innov_pre + beta * r2.innov_pre;
                ifr.var_brsd_i = a2 * r1.var_brsd_i + b2 * r2.var_brsd_i;
                ifr.var_brsd_p = a2 * r1.var_brsd_p + b2 * r2.var_brsd_p;
                // Distinct group key so IF rows correlate with each other
                // (same pivot variance) but not with any per-band rows
                // that might appear in mixed scenarios (currently they
                // don't; rows is fully replaced below).
                ifr.group_key = ifr.cons + "|IF|" + std::string(1, ifr.obs_type);
                ifr.amb_idx   = -1;
                // IF ambiguity is c·(f1·N1 - f2·N2)/(f1²-f2²) — never an
                // integer cycle.  Force float-only so AR's exclusion set
                // catches it at @c getNonIntegerCapableDdAmbiguityKeys().
                ifr.integer_capable = false;
                if_rows.push_back(std::move(ifr));
            }

            if (diag) {
                klog->info("DD diag: IF row collapse: {} per-band rows -> {} IF rows "
                           "({} sat-pair groups dropped for <2 bands)",
                           rows.size(), if_rows.size(), n_dropped_single_band);
            }
            rows = std::move(if_rows);
        }

        if (rows.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — rows empty (no shared band/obs_type with the pivot)");
            first_dd_call = false;
            return em;
        }

        // --- Prune stale DD ambiguity states ---
        // Compute the set of DD ambiguity keys this epoch will reference
        // by reading the final post-collapse @c rows vector.  Each phase
        // row contributes one expected key built from its rov_code, which
        // is either a single RINEX code ("L1C") in per-band mode or the
        // concatenated IF label ("L1C+L2W") after iono-free collapse.
        // Any DD-keyed ambiguity in the state that's NOT in this set is
        // stale and removed.  This catches every leak path in one pass:
        //   - pivot change for a group (old pivot's keys vanish from the set)
        //   - rover or reference tracker switching codes mid-arc
        //   - satellite leaving view at either receiver
        //   - satellite added to excluded_satellites at runtime
        //   - iono-free toggle changing the code segment of every key
        //   - any other condition that produces no row this epoch
        // Must run BEFORE em.H column indices are sized below — the H
        // matrix uses state_manager.getStateSize(), and removeAmbiguity()
        // compacts the state vector and shifts dynamic-state indices.
        // Building the expected set from @c rows (rather than from the
        // pre-collapse @c by_cons / @c brsd_signals) ensures the expected
        // and added keys agree on whether the code segment is per-band or
        // an IF concatenation; otherwise every IF DD ambiguity is pruned
        // and re-added each epoch, losing covariance.
        {
            std::set<std::string> expected_dd_keys;
            for (const auto& r : rows) {
                if (r.obs_type != 'L') continue;
                expected_dd_keys.insert(
                    makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p));
            }
            std::vector<std::string> to_prune;
            for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
                if (key.compare(0, 3, "DD|") != 0) continue;
                if (expected_dd_keys.count(key)) continue;
                to_prune.push_back(key);
            }
            for (const auto& k : to_prune) state_manager_.removeAmbiguity(k);
            if (!to_prune.empty()) {
                log_manager_.getLogger("main_log")->info(
                    "DD: pruned {} stale ambiguity state(s) (sat dropout, code "
                    "switch, or pivot change)", to_prune.size());
            }
        }

        // --- Refresh the AR-exclusion-key snapshot ---
        // Any phase row whose four observations are NOT all the same
        // RINEX code carries a residual receiver IFB or satellite DCB
        // term that does not round to integer.  Those keys are exposed
        // to AmbiguityResolver::resolveDd via getNonIntegerCapableDdAmbiguityKeys()
        // so they remain float-only.  Rebuilt from scratch each epoch
        // from the current phase rows; code rows are skipped because
        // they do not create ambiguity states.
        non_integer_capable_dd_ambiguity_keys_.clear();
        for (const auto& r : rows) {
            if (r.obs_type != 'L') continue;
            if (r.integer_capable) continue;
            non_integer_capable_dd_ambiguity_keys_.insert(
                makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p));
        }

        // --- Ensure DD ambiguity states exist for phase rows ---
        if (!code_only) {
            for (auto& r : rows) {
                if (r.obs_type != 'L') continue;
                // Ambiguity key uses the rover's RINEX code.  When
                // aliasing is on and the rover and reference disagree on
                // tracking attribute, the float ambiguity will absorb the
                // residual sat-DCB; integer fixing requires DCB
                // harmonization (Phase 2 plan) to be valid.
                const std::string key = makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p);
                int aidx = state_manager_.getAmbiguityIndex(key);
                if (aidx < 0) {
                    state_manager_.addAmbiguity(key, r.innov_pre);
                    aidx = state_manager_.getAmbiguityIndex(key);
                    if (aidx >= 0) {
                        Eigen::MatrixXd P = state_manager_.getCovariance();
                        P(aidx, aidx) = amb_init_var;
                        state_manager_.setCovariance(P);
                    }
                }
                r.amb_idx = aidx;
            }
        }

        // --- Per-row innov (code = pre; phase = pre - amb_state) ---
        Eigen::VectorXd x;
        try {
            const size_t ss = state_manager_.getStateSize();
            x = (current_state.size() == static_cast<Eigen::Index>(ss))
                ? current_state
                : state_manager_.getStateVector();
        } catch (const std::exception& ex) {
            klog->error("DD state-vector fetch failed: {}", ex.what());
            first_dd_call = false;
            return em;
        } catch (...) {
            klog->error("DD state-vector fetch failed: unknown exception");
            first_dd_call = false;
            return em;
        }
        std::vector<size_t> kept;
        kept.reserve(rows.size());
        try {
            for (size_t i = 0; i < rows.size(); ++i) {
                auto& r = rows[i];
                if (code_only && r.obs_type == 'L') continue;
                if (r.obs_type == 'L') {
                    if (r.amb_idx < 0) continue;
                    if (r.amb_idx >= x.size()) {
                        klog->warn("DD row {} amb_idx={} out of range x.size={}", i, r.amb_idx, x.size());
                        continue;
                    }
                    r.innov = r.innov_pre - x(r.amb_idx);
                } else {
                    r.innov = r.innov_pre;
                }
                kept.push_back(i);
            }
        } catch (const std::exception& ex) {
            klog->error("DD innov loop failed: {}", ex.what());
            first_dd_call = false;
            return em;
        }
        const Eigen::Index m = static_cast<Eigen::Index>(kept.size());
        if (diag) {
            klog->info("DD diag: {} total DD rows built, {} kept after filter (code_only={})",
                       rows.size(), m, code_only);
        }
        if (m == 0) {
            if (diag) klog->info("DD diag: EARLY RETURN — m==0 (all rows filtered out)");
            first_dd_call = false;
            return em;
        }

        try {
            const size_t n_state = state_manager_.getStateSize();
            em.innovations = Eigen::VectorXd(m);
            em.H = Eigen::MatrixXd::Zero(m, static_cast<Eigen::Index>(n_state));
            em.R_full = Eigen::MatrixXd::Zero(m, m);
            em.R_v = Eigen::VectorXd::Zero(m);
            em.satellite_ids.reserve(static_cast<size_t>(m));
            em.signal_types.reserve(static_cast<size_t>(m));

            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& r = rows[kept[static_cast<size_t>(a)]];
                em.H.block(a, 0, 1, 3) = r.h_pos;
                if (r.obs_type == 'L' && r.amb_idx >= 0
                    && static_cast<size_t>(r.amb_idx) < n_state) {
                    em.H(a, r.amb_idx) = 1.0;
                }
                em.innovations(a) = r.innov;
                em.R_v(a)         = r.var_brsd_i + r.var_brsd_p;
                em.satellite_ids.push_back(r.sat_i + "-" + r.sat_p);
                // Surface the rover code in residual traces.  When aliased
                // with a different reference attribute, append the ref
                // code so log readers can spot mixed-attribute rows.
                em.signal_types.push_back(
                    (r.rov_code == r.ref_code)
                        ? r.rov_code
                        : (r.rov_code + "/" + r.ref_code));
            }
            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& ra = rows[kept[static_cast<size_t>(a)]];
                em.R_full(a, a) = em.R_v(a);
                for (Eigen::Index b = a + 1; b < m; ++b) {
                    const auto& rb = rows[kept[static_cast<size_t>(b)]];
                    if (ra.group_key != rb.group_key) continue;
                    const double cov = ra.var_brsd_p;
                    em.R_full(a, b) = cov;
                    em.R_full(b, a) = cov;
                }
            }
        } catch (const std::exception& ex) {
            klog->error("DD H/R assembly failed: {}", ex.what());
            first_dd_call = false;
            return em;
        } catch (...) {
            klog->error("DD H/R assembly failed: unknown exception");
            first_dd_call = false;
            return em;
        }

        first_dd_call = false;
        return em;
    }

} // namespace GnssProcessor
