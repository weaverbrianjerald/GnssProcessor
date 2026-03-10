/**
 * @file MeasurementModel.cpp
 * @brief Implements measurement vector, design matrix and covariance assembly for PPP Kalman updates.
 *
 * The implementation converts a set of PPP observations and the current Kalman state
 * into an EpochMeasurement suitable for Kalman filter processing. Responsibilities:
 *  - Ensure ambiguity states exist for phase iono-free combinations.
 *  - Compute LOS-based Jacobian rows using the current receiver position.
 *  - Assemble innovations, H matrix and per-measurement variances R_v.
 *  - Perform basic validation and logging for invalid/low-quality data.
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

    /**
     * @brief Construct a MeasurementModel.
     *
     * Stores references to the provided managers and utilities. Emits an informational
     * log with configured signal priorities and processing mode.
     *
     * @param state_manager Non-owning reference to the StateManager for state access and ambiguity bookkeeping.
     * @param ppp_model Const reference to the PppModel used to obtain modeled observables.
     * @param config Const reference to the global GnssConfig providing processing parameters.
     * @param log_manager Non-owning reference to the LogManager used for logging.
     */
    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), ppp_utils_(log_manager), log_manager_(log_manager) {
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

    /**
     * @brief Build an epoch measurement update from parsed PPP observations.
     *
     * This method is read-only with respect to the Kalman state except for ensuring
     * ambiguity entries exist in the StateManager when required (it will call
     * StateManager::addAmbiguity). The routine:
     *  - Pre-pass observations to create ambiguity states for ionosphere-free phase combos.
     *  - Validate geometry/observations and apply elevation mask and other filters.
     *  - Compute line-of-sight (LOS) using the current receiver position from the state.
     *  - Assemble the innovation vector (observed - predicted), Jacobian H and per-measurement variances R_v.
     *
     * Notes:
     *  - Only GPS measurements with ionosphere-free dual-frequency combinations are processed.
     *  - Ambiguities are expected to be stored in the StateManager keyed as "<sat>_<ref_type>".
     *
     * @param observations Vector of PPP observations for the epoch (may be empty).
     * @param current_state Current Kalman state vector (read-only); the implementation will also retrieve the authoritative state vector
     *                      from StateManager where needed (post ambiguity additions).
     * @return Populated EpochMeasurement containing innovations, H, R_v and ancillary vectors. On error or no usable observations,
     *         an EpochMeasurement with empty or partially filled fields is returned; callers should call validateMeasurementUpdate()
     *         before using the result for a Kalman update.
     *
     * @throws std::exception Propagates only if an unexpected runtime error occurs during assembly. Individual observation errors are caught
     *                       and logged; failing observations are skipped.
     */
    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;
        const std::string epoch_tag = TimeUtils::formatGnssTime(update.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        if (observations.empty()) {
            log_manager_.getLogger("measurement_model")->error("{}No observations provided for measurement update, mode={}",
                epoch_prefix,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        // === First-epoch ISB initialization ===
        // If this is the first epoch and we have non-GPS systems, estimate initial ISB
        // from code observations to avoid massive innovations that would be flagged as outliers.
        if (!isb_initialized_) {
            initializeInterSystemBiases(observations, epoch_prefix);
            isb_initialized_ = true;
        }

        // Pre-pass to ensure phase ambiguities exist
        std::set<std::string> required_ambiguities;
        for (const auto& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty() || ref_type[0] != 'L') continue;
                const bool is_iono_free = (ref_type.find("_") != std::string::npos);
                if (!is_iono_free) continue;
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;

                const std::string amb_key = obs.satellite + "_" + ref_type;
                required_ambiguities.insert(amb_key);

                const int existing_amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                // Seed new ambiguities, and re-seed existing ones that were reset to 0
                // (e.g. by MW slip detector or KF outlier handling).
                const bool needs_seed = (existing_amb_idx < 0) ||
                    (existing_amb_idx >= 0 && state_manager_.getStateVector()(existing_amb_idx) == 0.0);

                if (needs_seed) {
                    const Eigen::VectorXd x_now = state_manager_.getStateVector();
                    const double seed_value = state_manager_.computeAmbiguitySeed(obs, ref_type, x_now);

                    if (existing_amb_idx < 0) {
                        state_manager_.addAmbiguity(amb_key, seed_value);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added missing ambiguity state for {} (key={}), seed={:.3f} m", epoch_prefix, obs.satellite, amb_key, seed_value);
                    } else {
                        // Re-seed an existing ambiguity that was reset to 0 (e.g. by MW slip detector)
                        Eigen::VectorXd x_reseed = x_now;
                        x_reseed(existing_amb_idx) = seed_value;
                        state_manager_.setStateVector(x_reseed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Re-seeded reset ambiguity for {} (key={}), seed={:.3f} m", epoch_prefix, obs.satellite, amb_key, seed_value);
                    }
                }
            }
        }

        // Remove ambiguities that are not needed by this epoch (prune stale ones).
        // This keeps the state vector compact and avoids unbounded growth of unused ambiguity states.
        state_manager_.pruneAmbiguitiesExcept(required_ambiguities);

        // After potential resizing, always use the up-to-date state vector size.
        const Eigen::VectorXd x = state_manager_.getStateVector();

        if (x.size() < (config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6)) {
            log_manager_.getLogger("measurement_model")->error("{}Invalid state vector size: {}, expected at least {}, mode={}",
                epoch_prefix,
                x.size(),
                config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        /*!
         * @brief Predicate to determine whether a given observation/ref_type pair is usable.
         *
         * Criteria:
         *  - Non-empty ref_type beginning with 'C' or 'L'.
         *  - Ionosphere-free dual-frequency combination (contains '_').
         *  - Must have observed, variance and modeled entries for the ref_type.
         */
        auto isUsableMeas = [&](const PppObservation& obs, const std::string& ref_type) -> bool {
            if (ref_type.empty()) return false;

            // Only code/phase combos
            const char mt = ref_type[0];
            if (mt != 'C' && mt != 'L') return false;

            // Only iono-free combinations here (dual-freq C*_C* and L*_L*)
            const bool is_iono_free = (ref_type.find('_') != std::string::npos);
            if (!is_iono_free) return false;

            // Must have full modeled triple
            if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) return false;

            return true;
        };

        // Count only the measurements we will actually process (iono-free dual-freq combos)
        size_t num_obs = 0;
        for (const auto& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (isUsableMeas(obs, ref_type)) {
                    ++num_obs;
                }
            }
        }

        if (num_obs == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid observations found in residuals, mode={}",
                epoch_prefix,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        // Initialize measurement vectors and matrices using current StateManager size
        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(x.size()) != state_size) {
            log_manager_.getLogger("measurement_model")->error("{}State vector size {} does not match StateManager size {}, mode={}",
                epoch_prefix,
                x.size(), state_size,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        update.innovations.resize(num_obs);
        update.R_v.resize(num_obs);
        update.H.resize(num_obs, state_size);
        update.H.setZero();
        update.satellite_ids.reserve(num_obs);
        update.signal_types.reserve(num_obs);

        size_t row = 0;
        for (const auto& obs : observations) {
            // Use the geometry that was computed in computeModels() and stored in the observation.
            // The satellite position, LOS, range, and all corrections are pre-computed and stored
            // in the PppObservation; we use them directly for the Jacobian and innovation assembly.

            // Validate stored geometry
            if (obs.sat_pos.norm() < 1e6 || !obs.sat_pos.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored satellite position for {} at TOW {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            if (obs.range < 1e6 || !std::isfinite(obs.range)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.range,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            if (obs.elevation < config_.getElevationMaskDeg()) {
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.elevation,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            // Use the stored LOS unit vector from computeModels().
            // The LOS was computed at the linearization position (obs.rx_pos_linearization).
            // When the current state position differs, the range correction term
            // (-LOS · pos_delta) accounts for the first-order geometric range change.
            if (obs.los.norm() < 0.5 || !obs.los.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored LOS for {} at TOW {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }
            const Eigen::Vector3d los_current = obs.los;

            // Use the stored wet mapping function from computeModels()
            // (elevation-dependent, but elevation change from position delta is negligible)
            const double mapping_wet = obs.mf_w;
            if (mapping_wet <= 0.0 || !std::isfinite(mapping_wet)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid wet mapping function for {} at TOW {:.3f}: {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, mapping_wet,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeas(obs, ref_type)) continue;

                try {
                    // Build Jacobian row
                    Eigen::VectorXd jacobian = Eigen::VectorXd::Zero(state_size);
                    jacobian.segment<3>(state_manager_.getPositionIndex()) = -los_current;
                    jacobian(state_manager_.getClockIndex()) = 1.0;
                    jacobian(state_manager_.getTropoIndex()) = mapping_wet;

                    // Tropospheric gradient contribution (Chen & Herring, 1997)
                    // dm_grad = mf_grad * (Gn * cos(az) + Ge * sin(az))
                    // mf_grad = 1 / (sin(el) * tan(el) + 0.0032)
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
                        jacobian(state_manager_.getTropoGradientEastIndex()) = mf_grad * sin_az;

                        const double gn = x(state_manager_.getTropoGradientNorthIndex());
                        const double ge = x(state_manager_.getTropoGradientEastIndex());
                        tropo_grad_term = mf_grad * (gn * cos_az + ge * sin_az);
                    }

                    // Add inter-system time offset for non-GPS systems
                    // The time offset is added to the predicted observable for non-GPS systems
                    std::string system_name;
                    switch (obs.system) {
                        case System::GPS: system_name = ""; break;  // GPS is reference - no ISB
                        case System::Galileo: system_name = "Galileo"; break;
                        case System::GLONASS: system_name = "GLONASS"; break;
                        case System::BeiDou: system_name = "BeiDou"; break;
                        case System::QZSS: system_name = "QZSS"; break;
                        case System::SBAS: system_name = "SBAS"; break;
                        default: system_name = ""; break;
                    }
                    
                    size_t time_offset_idx = state_manager_.getSystemTimeOffsetIndex(system_name);
                    double time_offset_value = 0.0;
                    if (!system_name.empty() && time_offset_idx < state_size) {
                        jacobian(time_offset_idx) = 1.0;
                        time_offset_value = x(time_offset_idx);
                    }

                    // The modeled value from processIonoFreeObservation includes:
                    //   range - sat_clock*c + tropo_zhd*mf_d + antenna + shapiro + solid_tide + ...
                    // 
                    // It does NOT include receiver clock (that's in the state).
                    // It DOES include a reference troposphere (slant_dry_delay = tropo_zhd * mf_d).
                    //
                    // The modeled value was computed at the linearization position
                    // (obs.rx_pos_linearization). When the current state position differs
                    // from the linearization point, we must apply a first-order range
                    // correction so the innovation is consistent with the EKF formulation:
                    //   predicted = h(x0) + H_pos * (x - x0) + clock + tropo + ...
                    // where H_pos = -LOS.  The range correction accounts for the
                    // geometric range change due to the position delta.
                    //
                    // Our state estimates: [position, receiver_clock, ZWD, ..., time_offsets, ambiguities]
                    // 
                    // Predicted = modeled + range_correction + receiver_clock + (ZWD * mf_w) + time_offset
                    
                    const double modeled = obs.modeled.at(ref_type);
                    const double rx_clk = x(state_manager_.getClockIndex());
                    const double zwd = x(state_manager_.getTropoIndex());
                    const double tropo_wet_term = mapping_wet * zwd;

                    // Range correction for position delta from linearization point.
                    // modeled was computed with range = |sat_pos - rx_pos_linearization|.
                    // The current state position may differ, so correct the modeled
                    // range by the first-order term: delta_range = -LOS · (pos_current - pos_linearization).
                    // This is equivalent to the WLS approach: range_delta = |sat - pos_current| - |sat - pos_lin|.
                    const Eigen::Vector3d pos_current = x.segment<3>(state_manager_.getPositionIndex());
                    const Eigen::Vector3d pos_delta = pos_current - obs.rx_pos_linearization;
                    const double range_correction = -los_current.dot(pos_delta);

                    // Predicted observable at current state (includes time offset for non-GPS)
                    double predicted = modeled + range_correction + rx_clk + tropo_wet_term + tropo_grad_term + time_offset_value;

                    // Ambiguity handling for phase measurements
                    if (ref_type[0] == 'L') {
                        const std::string amb_key = obs.satellite + "_" + ref_type;
                        int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx < 0) {
                            continue;
                        }
                        jacobian(amb_idx) = 1.0;
                        predicted += x(amb_idx);
                    }

                    // Innovation = observed - predicted
                    update.innovations(row) = observed - predicted;

                    // Measurement variance with elevation-dependent scaling
					update.R_v(row) = obs.variances.at(ref_type); // From PppModel, already includes elevation scaling

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
                        observed, modeled, predicted, update.innovations(row));

                    row++;
                }
                catch (const std::exception&) {
                    continue;
                }
            }
        }

        if (row == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid measurement updates computed, mode={}",
                epoch_prefix,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger("measurement_model")->warn("{}Processed {} observations out of {} expected, mode={}",
                epoch_prefix,
                row, num_obs, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }
        update.bad_residual_count = 0;// initialize each new epoch with assumed inlier condition for all measurements
        log_manager_.getLogger("measurement_model")->info("{}Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
            epoch_prefix,
            row, update.H.rows(), update.H.cols(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return update;
    }

    /**
     * @brief Validate an assembled EpochMeasurement before using it for a Kalman update.
     *
     * Checks for:
     *  - Dimension consistency between innovations, R_v, H and ancillary vectors.
     *  - Finite and positive variances.
     *  - Finite innovations and finite Jacobian rows.
     *  - Logs warnings for large residuals (but does not automatically reject them unless they break validity).
     *
     * @param measurement EpochMeasurement to validate.
     * @return True if the measurement passes structural and basic numerical checks; false otherwise.
     */
    bool MeasurementModel::validateMeasurementUpdate(const EpochMeasurement& measurement) const {
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix = "[" + epoch_tag + "] ";

        if (measurement.innovations.size() != measurement.R_v.size() ||
            measurement.innovations.size() != measurement.H.rows() ||
            measurement.H.cols() != state_manager_.getStateSize() ||
            measurement.innovations.size() != measurement.satellite_ids.size() ||
            measurement.innovations.size() != measurement.signal_types.size()) {
            log_manager_.getLogger("measurement_model")->error("{}Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
                "sat_ids={}, signal_types={}, state={}, mode={}",
                epoch_prefix,
                measurement.innovations.size(), measurement.R_v.size(),
                measurement.H.rows(), measurement.H.cols(),
                measurement.satellite_ids.size(), measurement.signal_types.size(),
                state_manager_.getStateSize(),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return false;
        }

        for (size_t i = 0; i < measurement.innovations.size(); ++i) {
            if (!std::isfinite(measurement.innovations(i)) || !std::isfinite(measurement.R_v(i))) {
                log_manager_.getLogger("measurement_model")->warn("{}Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger("measurement_model")->warn("{}Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i], measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) { // Threshold for large residuals
                log_manager_.getLogger("measurement_model")->warn("{}Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger("measurement_model")->warn("{}Non-finite Jacobian row for {} {}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
        }

        log_manager_.getLogger("measurement_model")->trace("{}Validated measurement update: {} measurements, mode={",
            epoch_prefix,
            measurement.innovations.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    void MeasurementModel::initializeInterSystemBiases(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        // Collect code residuals (observed - modeled) per system
        // GPS residuals include receiver clock; other systems include clock + ISB
        // ISB ≈ median(non-GPS residuals) - median(GPS residuals)

        std::map<std::string, std::vector<double>> system_residuals;

        const Eigen::VectorXd x = state_manager_.getStateVector();
        const double rx_clk = x(state_manager_.getClockIndex());
        const double zwd = x(state_manager_.getTropoIndex());

        for (const auto& obs : observations) {
            if (obs.elevation < config_.getElevationMaskDeg()) continue;

            // Find a code iono-free observation
            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty() || ref_type[0] != 'C') continue;
                if (ref_type.find('_') == std::string::npos) continue;  // Must be iono-free
                if (!obs.modeled.count(ref_type)) continue;

                const double modeled = obs.modeled.at(ref_type);
                const double tropo_wet = obs.mf_w * zwd;

                // Residual without ISB: observed - (modeled + clock + tropo_wet)
                const double residual = observed - modeled - rx_clk - tropo_wet;

                std::string sys_name;
                switch (obs.system) {
                case System::GPS: sys_name = "GPS"; break;
                case System::Galileo: sys_name = "Galileo"; break;
                case System::GLONASS: sys_name = "GLONASS"; break;
                case System::BeiDou: sys_name = "BeiDou"; break;
                case System::QZSS: sys_name = "QZSS"; break;
                case System::SBAS: sys_name = "SBAS"; break;
                default: continue;
                }

                system_residuals[sys_name].push_back(residual);
                break;  // One code obs per satellite is enough
            }
        }

        // Helper lambda to compute median
        auto computeMedian = [](std::vector<double>& values) -> double {
            if (values.empty()) return 0.0;
            size_t n = values.size();
            size_t mid = n / 2;
            std::nth_element(values.begin(), values.begin() + mid, values.end());
            if (n % 2 == 0) {
                // Even count: average of two middle elements
                double upper = values[mid];
                std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
                double lower = values[mid - 1];
                return (lower + upper) / 2.0;
            }
            return values[mid];
            };

        // Compute GPS median residual as reference
        if (system_residuals.find("GPS") == system_residuals.end() ||
            system_residuals["GPS"].empty()) {
            log_manager_.getLogger("measurement_model")->warn(
                "{}Cannot initialize ISB: no GPS code observations", epoch_prefix);
            return;
        }

        double gps_median = computeMedian(system_residuals["GPS"]);

        // Initialize ISB for each non-GPS system
        Eigen::VectorXd x_updated = state_manager_.getStateVector();

        for (auto& [sys_name, residuals] : system_residuals) {
            if (sys_name == "GPS" || residuals.empty()) continue;

            size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
            if (isb_idx >= static_cast<size_t>(x_updated.size())) continue;

            double sys_median = computeMedian(residuals);

            // ISB = median(sys residuals) - median(GPS residuals)
            const double isb_estimate = sys_median - gps_median;
            x_updated(isb_idx) = isb_estimate;

            log_manager_.getLogger("measurement_model")->info(
                "{}Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
                epoch_prefix, sys_name, isb_estimate, isb_estimate / Constants::SPEED_OF_LIGHT,
                residuals.size(), system_residuals["GPS"].size());
        }

        state_manager_.setStateVector(x_updated);
    }

} // namespace GnssProcessor