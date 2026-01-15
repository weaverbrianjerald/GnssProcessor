#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "MeasurementModel.hpp"
#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <stdexcept>
#include <algorithm>

namespace GnssProcessor {

    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), ppp_utils_(log_manager), log_manager_(log_manager) {
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

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

            // Recompute geometry at current linearization point (state position)
            Eigen::Vector3d delta_r = obs.sat_pos - current_pos;
            double range_computed = delta_r.norm();
            if (range_computed < 1e-6 || !std::isfinite(range_computed)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid computed range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix,
                    obs.satellite, obs.time.tow, range_computed,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }
            Eigen::Vector3d los_current = delta_r / range_computed;

            // Range used when obs.modeled was formed in computeModels()
            const double range_ref = obs.range;
            if (range_ref < 1e-6 || !std::isfinite(range_ref)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid reference range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix,
                    obs.satellite, obs.time.tow, range_ref,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            const double range_delta = range_computed - range_ref;

            double elev_deg = obs.elevation;
            if (!std::isfinite(elev_deg)) {
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Invalid elevation for {} at TOW {:.3f}: {} deg, mode={}",
                    epoch_prefix,
                    obs.satellite, obs.time.tow, elev_deg,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }
            if (elev_deg < config_.getElevationMaskDeg()) {
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                    epoch_prefix,
                    obs.satellite, obs.time.tow, elev_deg,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                continue;
            }

            double mapping_current = obs.mf_w;

            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeas(obs, ref_type)) continue;

                try {
                    Eigen::VectorXd jacobian = Eigen::VectorXd::Zero(state_size);
                    jacobian.segment<3>(state_manager_.getPositionIndex()) = -los_current;
                    jacobian(state_manager_.getClockIndex()) = 1.0;
                    jacobian(state_manager_.getTropoIndex()) = mapping_current;

                    const double modeled = obs.modeled.at(ref_type);
                    const double clk = x(state_manager_.getClockIndex());
                    const double tropo_term = mapping_current * x(state_manager_.getTropoIndex());

                    // Adjust modeled (which was built using range_ref) to current range_computed
                    double predicted = (modeled + range_delta) + clk + tropo_term;

                    // Ambiguity handling (read-only): always apply state ambiguity, do not modify it here
                    if (ref_type[0] == 'L') {
                        const std::string amb_key = obs.satellite + "_" + ref_type;
                        int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx < 0) {
                            continue;
                        }
                        jacobian(amb_idx) = 1.0;
                        predicted += x(amb_idx);
                    }

                    update.innovations(row) = obs.observed.at(ref_type) - predicted;

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