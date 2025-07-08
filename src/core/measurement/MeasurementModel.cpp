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
        log_manager_.getLogger()->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;

        if (observations.empty()) {
            log_manager_.getLogger()->error("No observations provided for measurement update, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }
        if (current_state.size() < (config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6)) {
            log_manager_.getLogger()->error("Invalid state vector size: {}, expected at least {}, mode={}",
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
            log_manager_.getLogger()->error("No valid observations found in residuals, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }

        // Initialize measurement vectors and matrices
        size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(current_state.size()) != state_size) {
            log_manager_.getLogger()->error("State vector size {} does not match StateManager size {}, mode={}",
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

        size_t row = 0;
        for (const auto& obs : observations) {
            Eigen::Vector3d sat_pos = ppp_model_.getSatellitePosition(obs.satellite, obs.time);
            if (sat_pos.norm() == 0.0 || !sat_pos.allFinite()) {
                log_manager_.getLogger()->warn("No valid satellite position for {} at TOW {:.3f}, mode={}",
                    obs.satellite, obs.time.tow,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            Eigen::Vector3d rx_pos = current_state.segment<3>(state_manager_.getPositionIndex());
            double elevation = ppp_model_.computeElevation(rx_pos, sat_pos);
            double azimuth = ppp_model_.computeAzimuth(rx_pos, sat_pos);
            if (elevation < config_.getElevationMaskDeg()) {
                log_manager_.getLogger()->debug("Satellite {} below elevation mask ({} < {} deg), mode={}",
                    obs.satellite, elevation, config_.getElevationMaskDeg(),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            double range = ppp_model_.computeGeometricRange(rx_pos, sat_pos);
            Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
            double mapping = 1.0 / (std::sin(elevation * Constants::PI / 180.0) +
                0.00143 / (std::tan(elevation * Constants::PI / 180.0) + 0.0445));
            std::string system_str = getSystemString(obs.system);

            for (const auto& [ref_type, residual] : obs.residuals) {
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type)) {
                    log_manager_.getLogger()->warn("Missing observed or variance data for {} {}, skipping, mode={}",
                        obs.satellite, ref_type,
                        config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    continue;
                }

                try {
                    double predicted = 0.0;
                    Eigen::Vector3d d_pred_d_pos = Eigen::Vector3d::Zero();
                    double d_pred_d_clock = 0.0;
                    double d_pred_d_tropo = 0.0;
                    double d_pred_d_iono = 0.0;
                    double d_pred_d_amb = 0.0;
                    std::string amb_key = obs.satellite + "_" + ref_type;

                    // Predicted measurement
                    predicted = range + current_state(state_manager_.getClockIndex()) * Constants::SPEED_OF_LIGHT +
                        current_state(state_manager_.getTropoIndex()) * mapping;
                    d_pred_d_pos = -los;
                    d_pred_d_clock = Constants::SPEED_OF_LIGHT;
                    d_pred_d_tropo = mapping;

                    // Ionospheric delay for single-frequency measurements
                    std::string primary_type = ref_type.substr(0, ref_type.find("_"));
                    if (primary_type.empty()) primary_type = ref_type;
                    if (ref_type.find("_") == std::string::npos) { // Single-frequency
                        double iono_delay = ppp_model_.computeIonosphericDelay(obs.time, rx_pos, sat_pos, ref_type,
                            obs.system, obs.observed);
                        predicted += iono_delay;
                        if (ref_type[0] == 'C' && config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                            double freq = ppp_model_.getFrequency(ref_type, obs.system, obs.satellite);
                            if (freq > 0.0) {
                                double f_L1 = ppp_model_.getFrequency("C1C", System::GPS, "");
                                d_pred_d_iono = (f_L1 * f_L1) / (freq * freq);
                            }
                            else {
                                log_manager_.getLogger()->warn("Invalid frequency for type {} on system {} for satellite {}, mode={}",
                                    ref_type, system_str, obs.satellite,
                                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                                continue;
                            }
                        }
                    }

                    // Ambiguity for phase or iono-free measurements
                    if (ref_type[0] == 'L' || ref_type.find("_") != std::string::npos) {
                        auto amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx >= 0) {
                            predicted += current_state(amb_idx);
                            d_pred_d_amb = 1.0;
                            log_manager_.getLogger()->trace("Applied ambiguity {} (index {}) for {}: {:.3f} m, mode={}",
                                amb_key, amb_idx, obs.satellite, current_state(amb_idx),
                                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                        }
                        else {
                            log_manager_.getLogger()->warn("Ambiguity key {} not found for {}, mode={}",
                                amb_key, obs.satellite,
                                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                            continue;
                        }
                    }

                    // Apply antenna and bias corrections
                    double antenna_corr = ppp_model_.getAntennaCorrection(obs.satellite, primary_type, obs.system,
                        rx_pos, sat_pos, elevation, azimuth,
                        ppp_model_.getReceiverAntennaType(), obs.time);
                    double bias_corr = ppp_model_.getBiasCorrection(obs.satellite, primary_type, obs.time);
                    predicted += antenna_corr + bias_corr;

                    // Rescale GLONASS observations
                    double observed_value = obs.observed.at(ref_type);
                    if (obs.system == System::GLONASS && ref_type[0] != 'D' && ref_type[0] != 'S') {
                        observed_value = ppp_model_.getRinexParser().rescaleGlonassMeasurement(observed_value, primary_type, obs.satellite);
                    }

                    update.innovations(row) = observed_value - predicted;
                    update.R_v(row) = obs.variances.count(ref_type) && obs.variances.at(ref_type) > 0 ?
                        obs.variances.at(ref_type) : (ref_type[0] == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);
                    if (!obs.variances.count(ref_type)) {
                        log_manager_.getLogger()->warn("No variance for {} in observation for {}, using default: {}, mode={}",
                            ref_type, obs.satellite, update.R_v(row),
                            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    }

                    // Populate Jacobian
                    update.H(row, state_manager_.getPositionIndex() + 0) = d_pred_d_pos(0);
                    update.H(row, state_manager_.getPositionIndex() + 1) = d_pred_d_pos(1);
                    update.H(row, state_manager_.getPositionIndex() + 2) = d_pred_d_pos(2);
                    update.H(row, state_manager_.getClockIndex()) = d_pred_d_clock;
                    update.H(row, state_manager_.getTropoIndex()) = d_pred_d_tropo;
                    if (d_pred_d_iono != 0.0 && config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                        update.H(row, state_manager_.getIonoIndex()) = d_pred_d_iono;
                    }
                    if (d_pred_d_amb != 0.0) {
                        auto amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx >= 0) {
                            update.H(row, amb_idx) = d_pred_d_amb;
                        }
                    }

                    // Populate metadata
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);

                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        log_manager_.getLogger()->warn("Invalid measurement for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                            obs.satellite, ref_type, update.innovations(row), update.R_v(row),
                            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6; // High variance to down-weight
                    }

                    row++;
                }
                catch (const std::exception& e) {
                    log_manager_.getLogger()->warn("Error computing measurement for {} {}: {}, mode={}",
                        obs.satellite, ref_type, e.what(),
                        config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    continue;
                }
            }
        }

        if (row == 0) {
            log_manager_.getLogger()->error("No valid measurement updates computed, mode={}",
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger()->warn("Processed {} observations out of {} expected, mode={}",
                row, num_obs, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }

        log_manager_.getLogger()->info("Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
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
            log_manager_.getLogger()->error("Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
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
                log_manager_.getLogger()->warn("Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger()->warn("Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i], measurement.R_v(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) { // Threshold for large residuals
                log_manager_.getLogger()->warn("Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i),
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger()->warn("Non-finite Jacobian row for {} {}, mode={}",
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                return false;
            }
        }

        log_manager_.getLogger()->trace("Validated measurement update: {} measurements, mode={}",
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