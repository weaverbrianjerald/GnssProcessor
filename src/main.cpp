#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "ProductParser.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "MeasurementModel.hpp"
#include "KalmanFilter.hpp"
#include <filesystem>
#include <vector>
#include <chrono>
#include <set>
#include <Eigen/Dense>

using namespace GnssProcessor;

int main() {
    // Initialize logging
    LogManager log_manager;
    std::filesystem::path log_dir = "logs";
    log_manager.initialize(log_dir, LogLevel::Debug);
    log_manager.getLogger()->info("Application started");

    // Initialize and load configuration
    GnssConfig config;
    std::filesystem::path config_path = "config/gnss_config.json";
    if (!std::filesystem::exists(config_path)) {
        log_manager.getLogger()->error("Configuration file not found: {}", config_path.string());
        return 1;
    }
    if (!config.loadFromJson(config_path, log_manager)) {
        log_manager.getLogger()->error("Failed to load JSON configuration from {}", config_path.string());
        return 1;
    }
    log_manager.getLogger()->info("Configuration loaded from {}", config_path.string());

    // Initialize ProductParser
    ProductParser product_parser(config, log_manager);
    std::vector<std::filesystem::path> sp3_paths = product_parser.getOrbitPaths();
    if (sp3_paths.empty()) {
        log_manager.getLogger()->error("No SP3 file paths specified in configuration");
        return 1;
    }
    for (const auto& path : sp3_paths) {
        if (!std::filesystem::exists(path)) {
            log_manager.getLogger()->error("SP3 file not found: {}", path.string());
            return 1;
        }
    }
    log_manager.getLogger()->info("Found {} SP3 files", sp3_paths.size());

    // Parse observation RINEX file
    RinexParser rinex_parser(config, log_manager, product_parser);
    std::filesystem::path rinex_path = config.getObservationPath();
    if (rinex_path.empty() || !std::filesystem::exists(rinex_path)) {
        log_manager.getLogger()->error("Observation RINEX file not specified or not found: {}", rinex_path.string());
        return 1;
    }
    if (!rinex_parser.parse()) {
        log_manager.getLogger()->error("Failed to parse observation RINEX file: {}", rinex_path.string());
        return 1;
    }
    const auto& observations = rinex_parser.getObservations();
    if (observations.empty()) {
        log_manager.getLogger()->error("No observations parsed from RINEX file: {}", rinex_path.string());
        return 1;
    }
    log_manager.getLogger()->info("Parsed {} observations from RINEX file: {}", observations.size(), rinex_path.string());

    // Initialize processing components
    StateManager state_manager(config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6);
    PppModel ppp_model(config, product_parser, log_manager, rinex_parser, state_manager);
    MeasurementModel meas_model(state_manager, ppp_model, config, log_manager);
    KalmanFilter kalman_filter(state_manager, config, log_manager, ppp_model);

    log_manager.getLogger()->info("Processing components initialized, mode={}",
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    // Process observations
    PppState ppp_state;
    ppp_model.initializeState(ppp_state);
    std::vector<PppObservation> modeled_obs;
    auto start_time = std::chrono::high_resolution_clock::now();
    double prev_tow = 0.0;
    bool first_epoch = true;

    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        // Group observations by epoch
        GnssTime current_time = observations[epoch_idx].time;
        std::vector<RinexObservation> epoch_obs;
        while (epoch_idx < observations.size() && observations[epoch_idx].time.tow == current_time.tow) {
            epoch_obs.push_back(observations[epoch_idx]);
            ++epoch_idx;
        }

        // Compute observation models
        if (!ppp_model.computeModels(epoch_obs, ppp_state, modeled_obs)) {
            log_manager.getLogger()->warn("Failed to compute models for epoch TOW {}, mode={}",
                current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            continue;
        }

        // Compute time step for prediction
        double dt;
        if (first_epoch) {
            dt = rinex_parser.getHeader().interval.value_or(30.0);
            first_epoch = false;
        }
        else {
            dt = current_time.tow - prev_tow;
            double expected_interval = rinex_parser.getHeader().interval.value_or(30.0);
            if (std::abs(dt - expected_interval) > 1e-3) {
                log_manager.getLogger()->warn("Unexpected time step {}s at TOW {}, expected {}s, mode={}",
                    dt, current_time.tow, expected_interval,
                    config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }
        prev_tow = current_time.tow;
        log_manager.getLogger()->debug("Computed time step dt={}s for epoch TOW {}, mode={}",
            dt, current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        kalman_filter.predict(dt);

        // Compute measurement update
        Eigen::VectorXd current_state = state_manager.getStateVector();
        EpochMeasurement meas_update = meas_model.computeMeasurementUpdate(modeled_obs, current_state);
        if (meas_update.innovations.size() == 0) {
            log_manager.getLogger()->warn("No valid measurements for epoch TOW {}, mode={}",
                current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            continue;
        }
        if (!meas_model.validateMeasurementUpdate(meas_update)) {
            log_manager.getLogger()->warn("Measurement update validation failed for epoch TOW {}, mode={}",
                current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            continue;
        }

        // Apply Kalman filter update
        kalman_filter.update(meas_update);

        // Update PppState from StateManager
        ppp_state.position = state_manager.getStateVector().segment<3>(0);
        ppp_state.clock_bias = state_manager.getStateVector()(3);
        ppp_state.troposphere_zenith_delay = state_manager.getStateVector()(4);
        if (config.getProcessingMode() == ProcessingMode::PPP_RTK) {
            ppp_state.ionosphere_zenith_delay = state_manager.getStateVector()(5);
        }

        // Log state estimate
        if (config.getProcessingMode() == ProcessingMode::PPP) {
            log_manager.getLogger()->info("Epoch TOW {}: Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} ns, Tropo {:.3f} m, mode=PPP",
                current_time.tow,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias * 1e9, // seconds to nanoseconds
                ppp_state.troposphere_zenith_delay);
        }
        else {
            log_manager.getLogger()->info("Epoch TOW {}: Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} ns, Tropo {:.3f} m, Iono {:.3f} m, mode=PPP-RTK",
                current_time.tow,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias * 1e9, // seconds to nanoseconds
                ppp_state.troposphere_zenith_delay,
                ppp_state.ionosphere_zenith_delay);
        }

        // Log residual statistics, satellite count, and GDOP
        double sum_res = 0.0, sum_sq_res = 0.0;
        size_t num_res = 0;
        std::set<std::string> satellites;
        Eigen::MatrixXd HtH = Eigen::MatrixXd::Zero(3, 3); // For GDOP
        for (const auto& obs : meas_update.observations) {
            satellites.insert(obs.satellite);
            for (const auto& [type, res] : obs.residuals) {
                sum_res += res;
                sum_sq_res += res * res;
                num_res++;
            }
            for (size_t i = 0; i < meas_update.H.rows(); ++i) {
                if (meas_update.satellite_ids[i] == obs.satellite) {
                    Eigen::Vector3d los = meas_update.H.block(i, 0, 1, 3).transpose();
                    HtH += los * los.transpose();
                }
            }
        }
        double mean_res = num_res > 0 ? sum_res / num_res : 0.0;
        double std_res = num_res > 1 ? std::sqrt((sum_sq_res / num_res - mean_res * mean_res) * num_res / (num_res - 1)) : 0.0;
        double gdop = HtH.inverse().trace() > 0 ? std::sqrt(HtH.inverse().trace()) : 0.0;
        log_manager.getLogger()->info("Epoch TOW {}: Residuals mean={:.3f} m, std={:.3f} m, count={}, satellites={}, GDOP={:.2f}, mode={}",
            current_time.tow, mean_res, std_res, num_res, satellites.size(), gdop,
            config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

    // Log processing time
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    log_manager.getLogger()->info("Processing completed in {} ms, mode={}",
        duration, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    return 0;
}
