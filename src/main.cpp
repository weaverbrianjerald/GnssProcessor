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
#include <algorithm>

using namespace GnssProcessor;

int main() {
    // Initialize logging
    LogManager log_manager;
    std::filesystem::path log_dir = "logs";
    log_manager.initialize(log_dir, LogLevel::Trace);
    log_manager.getLogger("kalman_filter")->info("KalmanFilter logger initialized");
    log_manager.getLogger("ambiguity_log")->info("AmbiguityLog logger initialized");
    log_manager.getLogger("state_manager")->info("StateManager logger initialized");
    log_manager.getLogger("ppp_model")->info("PppModel logger initialized");
    log_manager.getLogger("measurement_model")->info("MeasurementModel logger initialized");
    log_manager.getLogger("rinex_parser")->info("RinexParser logger initialized");
    log_manager.getLogger("product_parser")->info("ProductParser logger initialized");
    log_manager.getLogger("main_log")->info("Main logger initialized");
    log_manager.getLogger("main_log")->info("Application started");

    // Initialize and load configuration
    GnssConfig config;
    std::filesystem::path config_path = "config/gnss_config.json";
    if (!std::filesystem::exists(config_path)) {
        log_manager.getLogger("main_log")->error("Configuration file not found: {}", config_path.string());
        return 1;
    }
    if (!config.loadFromJson(config_path, log_manager)) {
        log_manager.getLogger("main_log")->error("Failed to load JSON configuration from {}", config_path.string());
        return 1;
    }
    log_manager.getLogger("main_log")->info("Configuration loaded from {}", config_path.string());

    // Initialize ProductParser
    ProductParser product_parser(config, log_manager);
    std::vector<std::filesystem::path> sp3_paths = product_parser.getOrbitPaths();
    if (sp3_paths.empty()) {
        log_manager.getLogger("main_log")->error("No SP3 file paths specified in configuration");
        return 1;
    }
    for (const auto& path : sp3_paths) {
        if (!std::filesystem::exists(path)) {
            log_manager.getLogger("main_log")->error("SP3 file not found: {}", path.string());
            return 1;
        }
    }
    log_manager.getLogger("main_log")->info("Found {} SP3 files", sp3_paths.size());

    // Parse observation RINEX file
    RinexParser rinex_parser(config, log_manager, product_parser);
    std::filesystem::path rinex_path = config.getObservationPath();
    if (rinex_path.empty() || !std::filesystem::exists(rinex_path)) {
        log_manager.getLogger("main_log")->error("Observation RINEX file not specified or not found: {}", rinex_path.string());
        return 1;
    }
    if (!rinex_parser.parse()) {
        log_manager.getLogger("main_log")->error("Failed to parse observation RINEX file: {}", rinex_path.string());
        return 1;
    }
    const auto& observations = rinex_parser.getObservations();
    if (observations.empty()) {
        log_manager.getLogger("main_log")->error("No observations parsed from RINEX file: {}", rinex_path.string());
        return 1;
    }
    log_manager.getLogger("main_log")->info("Parsed {} observations from RINEX file: {}", observations.size(), rinex_path.string());

    // Initialize StateManager
    size_t core_state_size = config.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
    StateManager state_manager(core_state_size);

    // Initialize position from RINEX header
    Eigen::Vector3d initial_position(0.0, 0.0, 6371000.0); // Fallback position
    auto approx_pos = rinex_parser.getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { // Check for reasonable position magnitude
            valid_pos = false;
            break;
        }
    }
    if (valid_pos && approx_pos != std::array<double, 3>{0.0, 0.0, 0.0}) {
        initial_position = Eigen::Vector3d(approx_pos[0], approx_pos[1], approx_pos[2]);
        Eigen::VectorXd state_vector = state_manager.getStateVector();
        if (state_vector.size() >= state_manager.getPositionIndex() + 3) {
            state_vector.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(state_vector);
            log_manager.getLogger("main_log")->info("Initialized state position from RINEX header: ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position.x(), initial_position.y(), initial_position.z());
        }
        else {
            log_manager.getLogger("main_log")->warn("State vector size {} too small to set position", state_vector.size());
            valid_pos = false;
        }
    }
    if (!valid_pos) {
        Eigen::VectorXd state_vector = state_manager.getStateVector();
        if (state_vector.size() >= state_manager.getPositionIndex() + 3) {
            state_vector.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(state_vector);
            log_manager.getLogger("main_log")->warn("RINEX header approximate position invalid or zero: ({:.3f}, {:.3f}, {:.3f}), using fallback position: ({:.3f}, {:.3f}, {:.3f}) m",
                approx_pos[0], approx_pos[1], approx_pos[2],
                initial_position.x(), initial_position.y(), initial_position.z());
        }
        else {
            log_manager.getLogger("main_log")->error("State vector size {} too small, cannot set fallback position", state_vector.size());
            return 1;
        }
    }

    // Initialize processing components
    PppModel ppp_model(config, product_parser, log_manager, rinex_parser, state_manager);
    MeasurementModel meas_model(state_manager, ppp_model, config, log_manager);
    KalmanFilter kalman_filter(state_manager, config, log_manager, ppp_model);

    log_manager.getLogger("main_log")->info("Processing components initialized, mode={}",
        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    // Process observations
    PppState ppp_state;
    ppp_model.initializeState(ppp_state);
    std::vector<PppObservation> modeled_obs;
    auto start_time = std::chrono::high_resolution_clock::now();
    double prev_tow = 0.0;
    bool first_epoch = true;

    // Get active systems from configuration
    const auto& active_systems = config.getActiveSystems();

    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        // Group observations by epoch
        GnssTime current_time = observations[epoch_idx].time;
        std::vector<RinexObservation> epoch_obs;
        while (epoch_idx < observations.size() && observations[epoch_idx].time.tow == current_time.tow) {
            // Map observation system to string for comparison
            std::string obs_system;
            switch (observations[epoch_idx].system) {
            case System::GPS: obs_system = "GPS"; break;
            case System::Galileo: obs_system = "Galileo"; break;
            case System::GLONASS: obs_system = "GLONASS"; break;
            case System::BeiDou: obs_system = "BeiDou"; break;
            case System::QZSS: obs_system = "QZSS"; break;
            case System::SBAS: obs_system = "SBAS"; break;
            default: obs_system = "Unknown"; break;
            }
            // Filter observations by active systems
            if (std::find(active_systems.begin(), active_systems.end(), obs_system) != active_systems.end()) {
                epoch_obs.push_back(observations[epoch_idx]);
            }
            else {
                //log_manager.getLogger("main_log")->debug("Skipping observation for satellite {} (system: {}) at TOW {}, not in active systems",
                //    observations[epoch_idx].satellite, obs_system, current_time.tow);
            }
            ++epoch_idx;
        }

        // Skip empty epochs after filtering
        if (epoch_obs.empty()) {
            log_manager.getLogger("main_log")->debug("No observations for epoch TOW {} after filtering by active systems", current_time.tow);
            continue;
        }

        // Update state epoch time
        state_manager.setStateEpochTime(current_time);

        // Compute time step for prediction
        double dt;
        if (first_epoch) {
            dt = rinex_parser.getHeader().interval.value_or(30.0);
        }
        else {
            dt = current_time.tow - prev_tow;
            double expected_interval = rinex_parser.getHeader().interval.value_or(30.0);
            if (std::abs(dt - expected_interval) > 1e-3) {
                log_manager.getLogger("main_log")->warn("Unexpected time step {}s at TOW {}, expected {}s, mode={}",
                    dt, current_time.tow, expected_interval,
                    config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }
        prev_tow = current_time.tow;
        log_manager.getLogger("main_log")->debug("Computed time step dt={}s for epoch TOW {}, mode={}",
            dt, current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        // Get current state and update ppp_state
        Eigen::VectorXd current_state = state_manager.getStateVector();
        try {
            state_manager.updatePppStateFromVector(current_state, ppp_state, state_manager);
        }
        catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("Failed to update ppp_state at TOW {}: {}", current_time.tow, e.what());
            return 1;
        }

        // Compute observation models
        if (!ppp_model.computeModels(epoch_obs, ppp_state, modeled_obs)) {
            log_manager.getLogger("main_log")->warn(
                "Failed to compute models for epoch TOW {}, mode={}",
                current_time.tow, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            continue;
        }

        // WLS estimation, to seed clock in Kalman filter
        auto wls_result = ppp_model.estimateStateWls(current_time, modeled_obs);
        if (!wls_result.first.isZero()) {
            double wls_clock_bias = wls_result.first(3); // Clock bias in meters
            ppp_state.clock_bias = wls_clock_bias;
            Eigen::VectorXd state_vector = state_manager.getStateVector();
            size_t clock_idx = state_manager.getClockIndex();
            if (state_vector.size() >= state_manager.getClockIndex() + 1) {
                //current_state = wls_result.first; // Update full state with WLS result
                state_vector(clock_idx) = wls_clock_bias;
                state_manager.setStateVector(state_vector);
                current_state = state_manager.getStateVector();
                log_manager.getLogger("main_log")->info("Seeded Kalman filter with WLS clock bias: {:.3e} m at TOW {:.3f}",
                    wls_clock_bias, current_time.tow);
            }
            else {
                log_manager.getLogger("main_log")->warn("State vector size {} too small to set WLS clock bias at TOW {:.3f}",
                    state_vector.size(), current_time.tow);
            }
        }
        else {
            log_manager.getLogger("main_log")->warn("WLS estimation failed, using previous clock bias: {:.3e} m at TOW {:.3f}",
                ppp_state.clock_bias, current_time.tow);
        }

        // Predict state
        kalman_filter.predict(dt);

        // Get the predicted state
        current_state = state_manager.getStateVector();

        // TODO: remove this (it was moved to logging after update)
        // Update ppp_state with current_state
        try {
            state_manager.updatePppStateFromVector(current_state, ppp_state, state_manager);
        }
        catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("Failed to update ppp_state at TOW {}: {}", current_time.tow, e.what());
            break;
        }

        // Apaptive Kalman filter update
        EpochMeasurement meas_update;

        // Compute measurement update
        meas_update = meas_model.computeMeasurementUpdate(modeled_obs, current_state);
        std::string mode_str = (config.getProcessingMode() == ProcessingMode::PPP) ? "PPP" : "PPP-RTK";

        if (meas_update.innovations.size() == 0) {
            log_manager.getLogger("main_log")->warn("No valid measurements for TOW {}, mode={}", current_time.tow, mode_str);
            break;
        }
        if (!meas_model.validateMeasurementUpdate(meas_update)) {
            log_manager.getLogger("main_log")->warn("Measurement update validation failed for TOW {}, mode={}", current_time.tow, mode_str);
            break;
        }

        // Apply Kalman filter update
        kalman_filter.update(meas_update);

        // Get updated states
        current_state = state_manager.getStateVector();

        // Update ppp_state with current_state
        try {
            state_manager.updatePppStateFromVector(current_state, ppp_state, state_manager);
        }
        catch (const std::exception& e) {
            log_manager.getLogger("main_log")->error("Failed to update ppp_state at TOW {}: {}", current_time.tow, e.what());
            break;
        }

        // Validate position
        if (ppp_state.position.norm() < 1e-6 || !ppp_state.position.allFinite()) {
            log_manager.getLogger("main_log")->warn("Invalid position after Kalman update at TOW {}: ({:.3f}, {:.3f}, {:.3f}), reverting to initial position",
                current_time.tow, ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z());
            ppp_state.position = initial_position;
            current_state.segment<3>(state_manager.getPositionIndex()) = initial_position;
            state_manager.setStateVector(current_state);
        }

        // Log state estimate
        if (config.getProcessingMode() == ProcessingMode::PPP) {
            log_manager.getLogger("main_log")->info("Epoch TOW {}: Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, mode=PPP",
                current_time.tow,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias,
                ppp_state.troposphere_zenith_delay);
        }
        else {
            log_manager.getLogger("main_log")->info("Epoch TOW {}: Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m, Iono {:.3f} m, mode=PPP-RTK",
                current_time.tow,
                ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
                ppp_state.clock_bias,
                ppp_state.troposphere_zenith_delay,
                ppp_state.ionosphere_zenith_delay);
        }

        // Log residual statistics, satellite count, and GDOP
        /*
        double sum_res = 0.0, sum_sq_res = 0.0;
        size_t num_res = 0;
        std::set<std::string> satellites;
        Eigen::MatrixXd HtH = Eigen::MatrixXd::Zero(3, 3); // For GDOP

        for (const auto& obs : modeled_obs) {
            satellites.insert(obs.satellite);
            std::string observed_keys, variance_keys, residual_keys;
            for (const auto& [key, value] : obs.observed) {
                observed_keys += key + "(" + std::to_string(value) + "),";
            }
            for (const auto& [key, value] : obs.variances) {
                variance_keys += key + "(" + std::to_string(value) + "),";
            }
            for (const auto& [key, value] : obs.residuals) {
                residual_keys += key + "(" + std::to_string(value) + "),";
            }
            for (const auto& [ref_type, residual] : obs.residuals) {
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type)) {
                    log_manager.getLogger("main_log")->warn(
                        "Missing observed or variance data for satellite {}, type {}, observed keys=[{}], variance keys=[{}], residual keys=[{}], mode={}",
                        obs.satellite, ref_type, observed_keys, variance_keys, residual_keys,
                        config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
                    continue;
                }
                sum_res += residual;
                sum_sq_res += residual * residual;
                num_res++;
                log_manager.getLogger("main_log")->trace("Residual for {} {}: {:.3f} m, observed={:.3f} m, variance={:.6f}",
                    obs.satellite, ref_type, residual, obs.observed.at(ref_type), obs.variances.at(ref_type));
            }
            for (size_t i = 0; i < meas_update.innovations.size(); ++i) {
                Eigen::Vector3d los = meas_update.H.block(i, 0, 1, 3).transpose();
                HtH += los * los.transpose();
            }
        }
        double mean_res = num_res > 0 ? sum_res / num_res : 0.0;
        double std_res = num_res > 1 ? std::sqrt((sum_sq_res / num_res - mean_res * mean_res) * num_res / (num_res - 1)) : 0.0;
        double gdop = HtH.inverse().trace() > 0 ? std::sqrt(HtH.inverse().trace()) : 0.0;
        log_manager.getLogger("main_log")->info("Epoch TOW {}: Residuals mean={:.3f} m, std={:.3f} m, count={}, satellites={}, GDOP={:.2f}, mode={}",
            current_time.tow, mean_res, std_res, num_res, satellites.size(), gdop,
            config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            */
    }

    // Log processing time
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    log_manager.getLogger("main_log")->info("Processing completed in {} ms, mode={}",
        duration, config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

    return 0;
}