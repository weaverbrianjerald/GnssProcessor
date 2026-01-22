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

        // Pre-pass to ensure phase ambiguities exist
        // We only create ambiguities for phase measurements that we *might* use.
        for (const auto& obs : observations) {
            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty() || ref_type[0] != 'L') continue;

                // Iono-free only (dual-freq combo key like L1C_L2W)
                const bool is_iono_free = (ref_type.find("_") != std::string::npos);
                if (!is_iono_free) continue;

                // Must have full modeled triple to be usable
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) {
                    continue;
                }

                const std::string amb_key = obs.satellite + "_" + ref_type;
                if (state_manager_.getAmbiguityIndex(amb_key) < 0) {
                    // Float ambiguity starts at 0 in meters; variance comes from config in StateManager
                    state_manager_.addAmbiguity(amb_key, 0.0);

                    log_manager_.getLogger("measurement_model")->debug(
                        "{}Added missing ambiguity state for {} (key={})", epoch_prefix, obs.satellite, amb_key);
                }
            }
        }

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
         *  - GPS system only.
         *  - Non-empty ref_type beginning with 'C' or 'L'.
         *  - Ionosphere-free dual-frequency combination (contains '_').
         *  - Must have observed, variance and modeled entries for the ref_type.
         */
        auto isUsableMeas = [&](const PppObservation& obs, const std::string& ref_type) -> bool {
            if (obs.system != System::GPS) return false;
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

        // Count only the measurements we will actually process (GPS + iono-free dual-freq combos)
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

        Eigen::Vector3d current_pos = x.segment<3>(state_manager_.getPositionIndex());

        size_t row = 0;
        for (const auto& obs : observations) {
            if (obs.system != System::GPS) continue;

            // Use the geometry that was computed in computeModels() and stored in the observation.
            // The satellite position (obs.sat_pos) is already in reception-time ECEF frame.
            // We only need to recompute the LOS vector for the current state position (for Jacobian).
            //
            // Note: We do NOT call computeSatelliteGeometry() again here because:
            //   1. It's computationally expensive (iterative TOF refinement)
            //   2. The satellite position and all corrections are position-independent
            //   3. We only need the LOS direction, which we can compute directly

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

            // Compute LOS vector from current state position to satellite
            const Eigen::Vector3d delta = obs.sat_pos - current_pos;
            const double range_current = delta.norm();
            if (range_current < 1e6) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid current range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, range_current,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }
            const Eigen::Vector3d los_current = delta / range_current;

            // Range delta: difference between current and reference range
            // This accounts for the linearization from reference position to current state
            const double range_ref = obs.range;
            const double range_delta = range_current - range_ref;

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

                    // The modeled value from processIonoFreeObservation includes:
                    //   range - sat_clock*c + tropo_zhd*mf_d + antenna + shapiro + solid_tide + ...
                    // 
                    // It does NOT include receiver clock (that's in the state).
                    // It DOES include a reference troposphere (slant_dry_delay = tropo_zhd * mf_d).
                    //
                    // Our state estimates: [position, receiver_clock, ZWD, ...]
                    // 
                    // Predicted = modeled + range_delta + receiver_clock + (ZWD * mf_w)
                    
                    const double modeled = obs.modeled.at(ref_type);
                    const double rx_clk = x(state_manager_.getClockIndex());
                    const double zwd = x(state_manager_.getTropoIndex());
                    const double tropo_wet_term = mapping_wet * zwd;

                    // Predicted observable at current state
                    double predicted = modeled + range_delta + rx_clk + tropo_wet_term;

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
                    const double base_var = obs.variances.at(ref_type);
                    const double scale = state_manager_.getMeasurementVarianceScale(obs.satellite, ref_type);
                    update.R_v(row) = base_var * scale;

                    update.H.row(row) = jacobian;
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);

                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6;
                    }

                    log_manager_.getLogger("measurement_model")->trace(
                        "{}Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}, range_delta={:.3f}",
                        epoch_prefix, obs.satellite, ref_type,
                        observed, modeled, predicted, update.innovations(row), range_delta);

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

} // namespace GnssProcessor