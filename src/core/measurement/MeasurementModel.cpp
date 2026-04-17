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

    void MeasurementModel::freezePivotAmbiguityCovariance()
    {
        pivot_manager_.freezePivotAmbiguityCovariance(state_manager_);
    }

    void MeasurementModel::freezePivotIonoAndBiasCovariance()
    {
        pivot_manager_.freezePivotIonoAndBiasCovariance(state_manager_);
    }

} // namespace GnssProcessor
