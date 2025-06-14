#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "ProductManager.hpp"
#include "RinexParser.hpp"
#include "SSystem.hpp"
#include "StateManager.hpp"
#include "PppModel.hpp"
#include "MeasurementModel.hpp"
#include "KalmanFilter.hpp"
#include <filesystem>
#include <vector>
#include <chrono>
using namespace GnssProcessor;

int main() {
    // Initialize configuration and logging
    GnssProcessor::GnssConfig config;
    GnssProcessor::LogManager log_manager;
    std::filesystem::path log_dir = config.getRinexFilePath().parent_path().parent_path() / "logs";
    log_manager.initialize(log_dir, GnssProcessor::LogLevel::Debug);

    // Load configuration
    if (!config.loadFromJson("config/gnss_config.json", log_manager)) {
        log_manager.getLogger()->error("Failed to load JSON configuration");
        return 1;
    }
    log_manager.getLogger()->info("Configuration loaded from {}", config.getRinexFilePath().string());

    // Initialize product manager
    GnssProcessor::ProductManager product_manager(config, log_manager);
    if (!product_manager.loadProductList(config.getProductListPath())) {
        log_manager.getLogger()->error("Failed to load product list from {}", config.getProductListPath().string());
        return 1;
    }
    if (!product_manager.validateProducts()) {
        log_manager.getLogger()->error("Product validation failed");
        return 1;
    }

    // Parse RINEX file
    GnssProcessor::RinexParser rinex_parser(config);
    if (!rinex_parser.parse()) {
        log_manager.getLogger()->error("Failed to parse RINEX file: {}", config.getRinexFilePath().string());
        return 1;
    }

    // Initialize processing components
    GnssProcessor::SSystem ssystem(false); // Single-station mode
    GnssProcessor::StateManager state_manager(config);
    GnssProcessor::PppModel ppp_model(config, product_manager, log_manager, rinex_parser, state_manager);
    GnssProcessor::MeasurementModel meas_model(state_manager, ppp_model, config, log_manager);
    GnssProcessor::KalmanFilter kalman_filter(state_manager, config, log_manager, ppp_model);
    kalman_filter.initialize();

    // Process observations
    const auto& observations = rinex_parser.getObservations();
    if (observations.empty()) {
        log_manager.getLogger()->error("No observations parsed from RINEX file");
        return 1;
    }

    GnssProcessor::PppState ppp_state;
    std::vector<GnssProcessor::PppObservation> modeled_obs;
    auto start_time = std::chrono::high_resolution_clock::now();

    /*
    GnssProcessor::NetworkProcessor net_proc(rinex_parser.getObservations(), config, log_manager);
    net_proc.process();
    auto corrections = net_proc.getCorrections();

    GnssProcessor::UserProcessor user_proc(rinex_parser.getObservations(), corrections, Eigen::Vector3d(0, 0, 0), config, log_manager);
    user_proc.process();
    Eigen::Vector3d position = user_proc.getPosition();
    log_manager.getLogger()->info("User position: ({:.3f}, {:.3f}, {:.3f}) m", position.x(), position.y(), position.z());
    */

    for (size_t epoch_idx = 0; epoch_idx < observations.size(); ) {
        // Group observations by epoch
        GnssProcessor::GnssTime current_time = observations[epoch_idx].time;
        std::vector<GnssProcessor::RinexObservation> epoch_obs;
        while (epoch_idx < observations.size() && observations[epoch_idx].time.tow == current_time.tow) {
            epoch_obs.push_back(observations[epoch_idx]);
            ++epoch_idx;
        }

        // Compute observation models
        if (!ppp_model.computeModels(epoch_obs, ppp_state, modeled_obs)) {
            log_manager.getLogger()->warn("Failed to compute models for epoch TOW {}", current_time.tow);
            continue;
        }

        // Compute time step for prediction
        double dt = epoch_idx > 0 ? (current_time.tow - observations[epoch_idx - epoch_obs.size()].time.tow) : 1.0;
        kalman_filter.predict(dt);

        // Compute measurement update
        Eigen::VectorXd current_state = state_manager.getStateVector();
        GnssProcessor::EpochMeasurement meas_update = meas_model.computeMeasurementUpdate(modeled_obs, current_state);
        if (meas_update.innovations.size() == 0) {
            log_manager.getLogger()->warn("No valid measurements for epoch TOW {}", current_time.tow);
            continue;
        }

        // Apply Kalman filter update
        kalman_filter.update(meas_update, ppp_state);

        // Log state estimate
        log_manager.getLogger()->info("Epoch TOW {}: Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} ns, Tropo {:.3f} m, Iono {:.3f} m",
            current_time.tow,
            ppp_state.position.x(), ppp_state.position.y(), ppp_state.position.z(),
            ppp_state.clock_bias * 1e9, // seconds to nanoseconds
            ppp_state.troposphere_zenith_delay,
            ppp_state.ionosphere_zenith_delay);
    }

    // Log processing time
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    log_manager.getLogger()->info("Processing completed in {} ms", duration);

    return 0;
}