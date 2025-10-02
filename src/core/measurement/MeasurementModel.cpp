#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "MeasurementModel.hpp"
#include "RinexParser.hpp"
#include <stdexcept>
#include <algorithm>

namespace GnssProcessor {

    MeasurementModel::MeasurementModel(const StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), log_manager_(log_manager) {
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        Eigen::VectorXd& current_state) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;

        if (observations.empty()) {
            log_manager_.getLogger("measurement_model")->error("No observations provided for measurement update, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }
        if (current_state.size() < (config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6)) {
            log_manager_.getLogger("measurement_model")->error("Invalid state vector size: {}, expected at least {}, mode={}",
                current_state.size(),
                config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        // Count valid observations
        size_t num_obs = 0;
        for (const auto& obs : observations) {
            num_obs += obs.residuals.size();
        }
        if (num_obs == 0) {
            log_manager_.getLogger("measurement_model")->error("No valid observations found in residuals, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        // Initialize measurement vectors and matrices
        size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(current_state.size()) != state_size) {
            log_manager_.getLogger("measurement_model")->error("State vector size {} does not match StateManager size {}, mode={}",
                current_state.size(), state_size,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        update.innovations.resize(num_obs);
        update.R_v.resize(num_obs);
        update.H.resize(num_obs, state_size);
        update.H.setZero();
        update.satellite_ids.reserve(num_obs);
        update.signal_types.reserve(num_obs);

        Eigen::Vector3d current_pos = current_state.segment<3>(state_manager_.getPositionIndex());

        size_t row = 0;
        for (const auto& obs : observations) {
            // Compute current geometry
            double range_current = obs.range;
            if (range_current < 1e-6) {
                log_manager_.getLogger("measurement_model")->warn("Invalid current range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    obs.satellite, obs.time.tow, range_current,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            // Troposphere mapping parameters
            double elev_rad = obs.elevation * Constants::PI / 180.0;
            double sin_elev = std::sin(elev_rad);
            if (sin_elev < std::sin(config_.getElevationMaskDeg() * Constants::PI / 180.0)) {
                log_manager_.getLogger("measurement_model")->debug("Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                    obs.satellite, obs.time.tow, obs.elevation,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }
            // Compute mapping function using obs.elevation, matching WLS
            double mapping_current = 1.0 / (sin_elev + 0.00143 / (std::tan(elev_rad) + 0.0445));

            // Log usage of current geometry
            Eigen::Vector3d los_current = obs.los;
            log_manager_.getLogger("measurement_model")->trace("Using current geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow,
                obs.sat_pos.x(), obs.sat_pos.y(), obs.sat_pos.z(),
                elev_rad * 180.0 / Constants::PI,
                los_current.x(), los_current.y(), los_current.z(), range_current);

            for (const auto& [ref_type, residual] : obs.residuals) {
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) {
                    log_manager_.getLogger("measurement_model")->warn("Missing observed, variance, or modeled data for {} {}, skipping, mode={}",
                        obs.satellite, ref_type,
                        config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    continue;
                }
                // Validate iono-free ref_type for PPP mode
                bool is_iono_free = ref_type.find("_") != std::string::npos;
                if (config_.getProcessingMode() == ProcessingMode::PPP && !is_iono_free && !config_.isUncombinedEnabled()) {
                    log_manager_.getLogger("measurement_model")->warn("Non-ionosphere-free ref_type {} for {} in PPP mode, skipping",
                        ref_type, obs.satellite);
                    continue;
                }

                try {
                    // H-matrix elements (Jacobians)
                    Eigen::VectorXd jacobian = Eigen::VectorXd::Zero(state_size);
                    double d_pred_d_clock = 1.0;
                    double d_pred_d_tropo = mapping_current;
                    double d_pred_d_iono = 0.0;
                    std::string amb_key = obs.satellite + "_" + ref_type;

                    jacobian.segment<3>(state_manager_.getPositionIndex()) = -los_current;
                    jacobian(state_manager_.getClockIndex()) = d_pred_d_clock;
                    jacobian(state_manager_.getTropoIndex()) = d_pred_d_tropo;

                    // Ionospheric delay adjustment for single-frequency measurements
                    double iono_adj = 0.0;
                    if (config_.getProcessingMode() == ProcessingMode::PPP_RTK && ref_type.find("_") == std::string::npos) {
                        double freq = ppp_model_.getFrequency(ref_type, obs.system, obs.satellite);
                        if (freq > 0.0) {
                            double f_L1 = ppp_model_.getFrequency("C1C", System::GPS, "");
                            double coeff = (f_L1 * f_L1) / (freq * freq);
                            if (ref_type[0] == 'C') {
                                d_pred_d_iono = coeff * mapping_current; // why does the ionosphere use the same mapping function as the tropo here?
                            }
                            else if (ref_type[0] == 'L') {
                                d_pred_d_iono = -coeff * mapping_current;
                            }
                            jacobian(state_manager_.getIonoIndex()) = d_pred_d_iono;
                            iono_adj += d_pred_d_iono * current_state(state_manager_.getIonoIndex());
                        }
                        else {
                            log_manager_.getLogger("measurement_model")->warn("Invalid frequency for type {} on system {} for satellite {}, mode={}",
                                ref_type, getSystemString(obs.system), obs.satellite,
                                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                            continue;
                        }
                    }

                    // Compute predicted contribution from states
                    double range = range_current;
                    double modeled = obs.modeled.at(ref_type);
                    double clk = current_state(state_manager_.getClockIndex());
                    double tropo = mapping_current * current_state(state_manager_.getTropoIndex());
                    double predicted = 0.0;
                    predicted += range;
                    predicted += modeled;
                    predicted += clk;
                    predicted += tropo;
                    predicted += iono_adj;

                    // Jacobian for ambiguity (for phase measurements)
                    if (ref_type[0] == 'L') {
                        auto amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx >= 0) {
                            if (current_state(amb_idx) == 0.0) {
                                // Initialize ambiguity
                                current_state(amb_idx) = obs.observed.at(ref_type) - predicted;
                                log_manager_.getLogger("ambiguity_log")->trace("Initialize ambiguity for {} {} (index {}): value={:.3f} m, mode={}",
                                    obs.satellite, ref_type, amb_idx, current_state(amb_idx),
                                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                            }
                            else {
                                log_manager_.getLogger("ambiguity_log")->trace("Use ambiguity for {} {} (index {}): value={:.3f} m, mode={}",
                                    obs.satellite, ref_type, amb_idx, current_state(amb_idx),
                                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                            }
                            jacobian(amb_idx) = 1.0;
                            predicted += current_state(amb_idx);
                        }
                        else {
                            log_manager_.getLogger("ambiguity_log")->warn("Ambiguity key {} not found for {}, mode={}",
                                amb_key, obs.satellite,
                                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                            continue;
                        }
                    }

                    // Populate Kalman filter components
                    //update.innovations(row) = obs.residuals.at(ref_type) - predicted;
                    update.innovations(row) = obs.observed.at(ref_type) - predicted;
                    update.R_v(row) = obs.variances.at(ref_type);
                    update.H.row(row) = jacobian;
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);


                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        log_manager_.getLogger("measurement_model")->warn("Invalid measurement for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                            obs.satellite, ref_type, update.innovations(row), update.R_v(row),
                            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6; // High variance to down-weight
                    }

                    log_manager_.getLogger("measurement_model")->trace("Computed Kalman update for {} {}: innovation={:.3f} m, variance={:.6f}, "
                        "residual={:.3f} m, modeled={:.3f} m, clock_adj={:.3f} m, tropo_adj={:.3f} m, mode={}",
                        obs.satellite, ref_type, update.innovations(row), update.R_v(row),
                        obs.residuals.at(ref_type), obs.modeled.at(ref_type), clk, tropo,
                        config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    row++;
                }
                catch (const std::exception& e) {
                    log_manager_.getLogger("measurement_model")->warn("Error computing measurement for {} {}: {}, mode={}",
                        obs.satellite, ref_type, e.what(),
                        config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    continue;
                }
            }
        }

        if (row == 0) {
            log_manager_.getLogger("measurement_model")->error("No valid measurement updates computed, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger("measurement_model")->warn("Processed {} observations out of {} expected, mode={}",
                row, num_obs, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }
        update.bad_residual_count = 0;// initialize each new epoch with assumed inlier condition for all measurements
        log_manager_.getLogger("measurement_model")->info("Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
            row, update.H.rows(), update.H.cols(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return update;
    }

    bool MeasurementModel::validateMeasurementUpdate(const EpochMeasurement& measurement) const {
        if (measurement.innovations.size() != measurement.R_v.size() ||
            measurement.innovations.size() != measurement.H.rows() ||
            measurement.H.cols() != state_manager_.getStateSize() ||
            measurement.innovations.size() != measurement.satellite_ids.size() ||
            measurement.innovations.size() != measurement.signal_types.size()) {
            log_manager_.getLogger("measurement_model")->error("Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
                "sat_ids={}, signal_types={}, state={}, mode={}",
                measurement.innovations.size(), measurement.R_v.size(),
                measurement.H.rows(), measurement.H.cols(),
                measurement.satellite_ids.size(), measurement.signal_types.size(),
                state_manager_.getStateSize(),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return false;
        }

        for (size_t i = 0; i < measurement.innovations.size(); ++i) {
            if (!std::isfinite(measurement.innovations(i)) || !std::isfinite(measurement.R_v(i))) {
                log_manager_.getLogger("measurement_model")->warn("Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger("measurement_model")->warn("Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i], measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) { // Threshold for large residuals
                log_manager_.getLogger("measurement_model")->warn("Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger("measurement_model")->warn("Non-finite Jacobian row for {} {}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
        }

        log_manager_.getLogger("measurement_model")->trace("Validated measurement update: {} measurements, mode={}",
            measurement.innovations.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    std::string MeasurementModel::getSystemString(System system) {
        switch (system) {
        case System::GPS: return "GPS";
        case System::Galileo: return "Galileo";
        case System::GLONASS: return "GLONASS";
        case System::BeiDou: return "BeiDou";
        case System::QZSS: return "QZSS";
        case System::SBAS: return "SBAS";
        default: return "Unknown";
        }
    }

} // namespace GnssProcessor