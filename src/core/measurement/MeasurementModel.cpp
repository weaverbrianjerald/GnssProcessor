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
#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <stdexcept>
#include <algorithm>
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

        return observed - modeled - rx_clk - tropo_wet - isb - tropo_grad;
    }

    // ===================================================================
    // State pre-pass
    // ===================================================================

    void MeasurementModel::ensureRequiredStates(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        const bool is_uncombined = config_.isUncombinedEnabled();

        std::set<std::string> required_ambiguities;
        std::set<std::string> required_iono_sats;

        for (const auto& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty()) continue;
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;

                if (is_uncombined && (ref_type[0] == 'C' || ref_type[0] == 'L'))
                    required_iono_sats.insert(obs.satellite);

                if (ref_type[0] != 'L') continue;

                const bool is_iono_free = (ref_type.find('_') != std::string::npos);
                if (!is_uncombined && !is_iono_free) continue;

                const std::string amb_key = obs.satellite + "_" + ref_type;
                required_ambiguities.insert(amb_key);

                const int existing_idx = state_manager_.getAmbiguityIndex(amb_key);
                const bool needs_seed = (existing_idx < 0) ||
                    (existing_idx >= 0 && state_manager_.getStateVector()(existing_idx) == 0.0);

                if (needs_seed) {
                    const Eigen::VectorXd x_now = state_manager_.getStateVector();
                    const double seed = state_manager_.computeAmbiguitySeed(obs, ref_type, x_now);

                    if (existing_idx < 0) {
                        state_manager_.addAmbiguity(amb_key, seed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added missing ambiguity state for {} (key={}), seed={:.3f} m",
                            epoch_prefix, obs.satellite, amb_key, seed);
                    } else {
                        Eigen::VectorXd x_reseed = x_now;
                        x_reseed(existing_idx) = seed;
                        state_manager_.setStateVector(x_reseed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Re-seeded reset ambiguity for {} (key={}), seed={:.3f} m",
                            epoch_prefix, obs.satellite, amb_key, seed);
                    }
                }
            }
        }

        state_manager_.pruneAmbiguitiesExcept(required_ambiguities);

        if (!is_uncombined) return;

        // Ionosphere states
        for (const auto& sat : required_iono_sats) {
            if (state_manager_.getIonosphereIndex(sat) < 0) {
                state_manager_.addIonosphereState(sat, 0.0);
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Added ionosphere state for satellite {}", epoch_prefix, sat);
            }
        }
        state_manager_.pruneIonosphereStatesExcept(required_iono_sats);

        for (const auto& obs : observations) {
            if (required_iono_sats.count(obs.satellite) && obs.elevation > 0.0)
                state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
        }

        // Code bias (IFB) states for non-reference bands
        std::set<std::string> required_code_biases;
        if (config_.estimateIfb()) {
            for (const auto& obs : observations) {
                const std::string sys = PppUtils::getSystemString(obs.satellite);
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C' || ref_type.size() < 2) continue;
                    if (ref_type[1] == '1') continue;
                    const std::string cb_key = sys + "_" + ref_type;
                    required_code_biases.insert(cb_key);
                    if (state_manager_.getCodeBiasIndex(cb_key) < 0) {
                        state_manager_.addCodeBiasState(cb_key, 0.0);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added code bias state for {}", epoch_prefix, cb_key);
                    }
                }
            }
        }
        state_manager_.pruneCodeBiasStatesExcept(required_code_biases);
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool MeasurementModel::isUsableMeasurement(const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) const {
        if (ref_type.empty()) return false;

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

        // Receiver code bias / IFB (PPP-RTK uncombined, non-reference code bands)
        // The IFB is a direct receiver hardware delay in meters — its Jacobian
        // coefficient is simply 1.0.  Previous code used an iono-free combination
        // coefficient (-mu_j / (mu_1 - mu_j)) which incorrectly coupled the IFB
        // to the ionosphere state and is inconsistent with the cssrlib reference.
        double code_bias_term = 0.0;
        if (config_.estimateIfb() && is_uncombined && !is_phase && ref_type.size() >= 2 && ref_type[1] != '1') {
            const std::string sys = PppUtils::getSystemString(obs.satellite);
            const std::string cb_key = sys + "_" + ref_type;
            const int cb_idx = state_manager_.getCodeBiasIndex(cb_key);
            if (cb_idx >= 0 && cb_idx < static_cast<int>(state_size)) {
                jacobian(cb_idx) = 1.0;
                code_bias_term = x(cb_idx);
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
            + iono_term + code_bias_term;

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

        // --- State pre-pass (create/seed/prune ambiguities, iono, code biases) ---
        ensureRequiredStates(observations, epoch_prefix);

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
            if (obs.range < 1.9e7 || !std::isfinite(obs.range)) {
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

    // ===================================================================
    // Between-satellite single difference
    // ===================================================================

    /**
     * @brief Apply between-satellite single-difference (BSSD) S-transformation.
     *
     * Modern BSSD (Odijk et al. 2016, Khodabandeh & Teunissen 2015):
     *
     * 1. For each (constellation, signal_type) group with >= 2 members,
     *    select the highest-elevation satellite as pivot.
     * 2. Build a differencing operator D that subtracts the pivot row from
     *    every non-pivot row.  Pivot rows are consumed (removed).
     * 3. Apply D to z, H, and R:  z_sd = D*z,  H_sd = D*H,  R_sd = D*R*D^T.
     * 4. Explicitly zero the clock, phase-clock and ISB columns in H_sd
     *    (algebraically zero after differencing; explicit zeroing for safety).
     * 5. For phase SD rows, reparameterize ambiguities:
     *    - The SD row's H has +1 on amb(i) and -1 on amb(ref).
     *    - Zero the amb(ref) column: the reference ambiguity is absorbed into
     *      the eliminated clock.  The SD ambiguity N_sd(i) = N(i) - N(ref)
     *      is now what the state N(i) represents in the SD system.
     * 6. Freeze reference ambiguities and clock states by zeroing their H
     *    columns.  They receive no update from the SD observations but remain
     *    in the state vector (maintained by the WLS seeder and process noise).
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

        // ---- Step 2: Pivot selection per (constellation, signal_type) -------
        std::map<std::string, Eigen::Index> pivot_map;
        std::map<std::string, double>       pivot_el;
        for (const auto& ri : rows) {
            const std::string gk = ri.constellation + "|" + ri.signal_type;
            auto it = pivot_el.find(gk);
            if (it == pivot_el.end() || ri.elevation > it->second) {
                pivot_map[gk] = ri.row;
                pivot_el[gk]  = ri.elevation;
            }
        }

        // ---- Step 3: Partition into SD / singleton -------------------------
        std::map<std::string, int> grp_cnt;
        for (const auto& ri : rows)
            ++grp_cnt[ri.constellation + "|" + ri.signal_type];

        struct SdEntry { Eigen::Index row; std::string gk; };
        std::vector<SdEntry>       sd_entries;
        std::vector<Eigen::Index>  pass_rows;  // singletons kept undifferenced

        for (const auto& ri : rows) {
            const std::string gk = ri.constellation + "|" + ri.signal_type;
            if (grp_cnt[gk] <= 1) {
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

        // ---- Step 6: Zero clock / phase-clock / ISB columns ----------------
        // After differencing within a constellation these are algebraically 0.
        // Explicit zeroing ensures no floating-point residue leaks through.
        const auto zero_col = [&](size_t idx) {
            if (idx < static_cast<size_t>(new_H.cols()))
                new_H.col(static_cast<Eigen::Index>(idx)).setZero();
        };
        zero_col(state_manager_.getClockIndex());
        if (state_manager_.hasPhaseClock())
            zero_col(state_manager_.getPhaseClockIndex());
        for (const auto& sys : state_manager_.getSystemsWithTimeOffsets())
            zero_col(state_manager_.getSystemTimeOffsetIndex(sys));

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
        for (const auto& [gk, pr] : pivot_map) {
            if (grp_cnt[gk] > 1) {
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
            "Zeroed {} ref-amb cols. Pivots: {}",
            epoch_prefix, n, n_sd, n_pass, m,
            ref_amb_cols.size(), pivot_summary);
    }

} // namespace GnssProcessor